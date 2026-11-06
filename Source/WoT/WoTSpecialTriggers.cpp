#include "WoTSpecialTriggers.h"
#include "WoTRandom.h"

#include "Payload/WoTPayloadActivation.h"
#include "Projectiles/WoTProjectile.h"
#include "WoTCharacter.h"
#include "WoTAmbientSound.h"
#include "WoTGameMode.h"
#include "WoTPlayerController.h"
#include "WoTSound.h"
#include "WoTSoundTable.h"

#include "Components/PointLightComponent.h"
#include "Components/SceneComponent.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"

namespace
{
bool IsLocalPresentationDispatch(UWorld* World)
{
	const UWoTEventSubsystem* Events = World
		? World->GetSubsystem<UWoTEventSubsystem>() : nullptr;
	return Events && Events->IsDispatchingLocalPresentation();
}

void DeliverMessage(UWorld& World, const FWoTSpecialEventConfig& Config,
	AActor* Other, APawn* EventInstigator)
{
	if (Config.Message.IsEmpty()) return;
	if (Config.bBroadcast)
	{
		if (AWoTGameMode* Game = Cast<AWoTGameMode>(World.GetAuthGameMode()))
		{
			Game->BroadcastTypedMessage(Config.Message, FName(TEXT("Event")));
		}
		return;
	}
	APawn* Recipient = EventInstigator ? EventInstigator : Cast<APawn>(Other);
	if (APlayerController* PC = Recipient ? Cast<APlayerController>(Recipient->GetController()) : nullptr)
	{
		PC->ClientMessage(Config.Message);
	}
}

void ExecuteSpecialEvent(AActor& Source, const FWoTSpecialEventConfig& Config,
	AActor* Other, APawn* EventInstigator)
{
	UWorld* World = Source.GetWorld();
	if (!World) return;
	DeliverMessage(*World, Config, Other, EventInstigator);
	AActor* Victim = Other ? Other : EventInstigator;
	switch (Config.Action)
	{
	case EWoTSpecialEventAction::DamageInstigator:
		if (Victim && Source.GetNetMode()!=NM_Client)
		{
			if (Config.Damage >= 0)
			{
				FWoTDamageEvent DamageEvent;
				DamageEvent.WoTDamageType = Config.DamageType;
				Victim->TakeDamage(Config.Damage, DamageEvent,
					EventInstigator ? EventInstigator->GetController() : nullptr, &Source);
			}
			else if (AWoTCharacter* Character = Cast<AWoTCharacter>(Victim))
			{
				Character->Health = FMath::Min(Character->MaxHealth,
					Character->Health - Config.Damage);
			}
		}
		break;
	case EWoTSpecialEventAction::KillInstigator:
		if (EventInstigator && Source.GetNetMode()!=NM_Client)
		{
			if (AWoTCharacter* Character = Cast<AWoTCharacter>(EventInstigator))
			{
				Character->Died(nullptr, Config.DamageType,
					EventInstigator->GetActorLocation());
			}
		}
		break;
	case EWoTSpecialEventAction::PlaySoundEffect:
		if (!Config.SoundSubPath.IsNone())
		{
			if (IsLocalPresentationDispatch(World))
			{
				WoTSound::PlayOneShot(&Source, WoTSound::LoadBySubPath(Config.SoundSubPath),
					Source.GetActorLocation(), Config.SoundRadius,
					Config.SoundPitch, Config.SoundVolume);
			}
			else if (Source.GetNetMode()!=NM_Client)
			{
				WoTSound::PlayReplicated(*World, Config.SoundSubPath,
					Source.GetActorLocation(), Config.SoundRadius,
					Config.SoundVolume, Config.SoundPitch);
			}
		}
		break;
	case EWoTSpecialEventAction::PlayAmbientSoundEffect:
		if (AWoTCharacter* Character = Cast<AWoTCharacter>(EventInstigator))
		{
			if (Character->AmbientHum)
			{
				Character->AmbientHum->Stop();
				Character->AmbientHum->DestroyComponent();
				Character->AmbientHum = nullptr;
			}
			Character->AmbientHumPath = Config.AmbientSoundSubPath;
			Character->UpdateAmbientHum();
			Character->ForceNetUpdate();
		}
		break;
	case EWoTSpecialEventAction::PlayersPlaySoundEffect:
		if (Source.GetNetMode()!=NM_Client && !Config.SoundSubPath.IsNone())
		{
			for (FConstPlayerControllerIterator It=World->GetPlayerControllerIterator(); It; ++It)
			{
				if (AWoTPlayerController* PC=Cast<AWoTPlayerController>(It->Get()))
					PC->ClientPlayUISound(Config.SoundSubPath,Config.SoundVolume);
			}
		}
		break;
	case EWoTSpecialEventAction::PlayerPath:
		// Retail explicitly limits this state to NM_Standalone. No retail map in
		// the multiplayer census uses it, so it remains inert here by design.
		break;
	default:
		break;
	}
}
}

AWoTSpecialEvent::AWoTSpecialEvent()
{
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetActorEnableCollision(false);
}

void AWoTSpecialEvent::BeginPlay()
{
	Super::BeginPlay();
	if (IsWoTBootstrapWorldReady())
	{
		GetWorld()->GetSubsystem<UWoTEventSubsystem>()->RegisterTag(TagName, this);
	}
}

void AWoTSpecialEvent::WoTTrigger(AActor* Other, APawn* EventInstigator)
{
	const bool bLocalEffectsSource = IsLocalPresentationDispatch(GetWorld());
	// Ordinary SpecialEvent actions are authority-owned. Arena_09 deliberately
	// routes ROLE_None SpecialEffectsTrigger events to same-tag local sound and
	// particle actors, so that one exported presentation path runs per machine.
	if (!IsWoTBootstrapWorldReady()
		|| (GetNetMode() == NM_Client && !bLocalEffectsSource)
		|| (!HasAuthority() && !bLocalEffectsSource)) return;
	ExecuteSpecialEvent(*this, Config, Other, EventInstigator);
}

void AWoTSpecialEventTrigger::FireTargetEvent(AActor* Other, APawn* EventInstigator)
{
	if (!EventName.IsNone())
	{
		Super::FireTargetEvent(Other, EventInstigator);
		DeliverMessage(*GetWorld(), Config, Other, EventInstigator);
	}
	else
	{
		ExecuteSpecialEvent(*this, Config, Other, EventInstigator);
	}
}

void AWoTSpecialEventTrigger::WoTTrigger(AActor* Other, APawn* EventInstigator)
{
	if (!IsWoTBootstrapWorldReady() || !HasAuthority() || !bTriggerEnabled) return;
	// State Trigger() calls Global.Trigger but does not rebroadcast Event. This
	// matters for retail fire/poison records whose Event intentionally equals
	// their own Tag: the overlap dispatch reaches this action exactly once.
	FWoTSpecialEventConfig ActionOnly = Config;
	ActionOnly.Message.Reset();
	ExecuteSpecialEvent(*this, ActionOnly, Other, EventInstigator);
}

AWoTRandomEventTrigger::AWoTRandomEventTrigger()
{
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetActorEnableCollision(false);
}

void AWoTRandomEventTrigger::BeginPlay()
{
	Super::BeginPlay();
	if (!IsWoTBootstrapWorldReady()) return;
	if (!bLocalOnly && GetNetMode()==NM_Client)
	{
		SetRole(ROLE_None);
		return;
	}
	GetWorld()->GetSubsystem<UWoTEventSubsystem>()->RegisterTag(TagName, this);
	bActive = bInitiallyActive;
	ScheduleNext();
}

void AWoTRandomEventTrigger::WoTTrigger(AActor* Other, APawn* EventInstigator)
{
	if (!bLocalOnly && !HasAuthority()) return;
	TriggerInstigatorActor = TriggerInstigatorActor.IsValid()
		? Other : static_cast<AActor*>(EventInstigator);
	EventInstigatorPawn = EventInstigator;
	bActive = true;
}

void AWoTRandomEventTrigger::WoTUnTrigger(AActor* Other, APawn* EventInstigator)
{
	if (!bLocalOnly && !HasAuthority()) return;
	TriggerInstigatorActor = TriggerInstigatorActor.IsValid()
		? Other : static_cast<AActor*>(EventInstigator);
	EventInstigatorPawn = EventInstigator;
	bActive = false;
}

void AWoTRandomEventTrigger::ScheduleNext()
{
	const float Delay = WoTRandom::FRandRange(
		FMath::Max(0.001f, MinRecheckTime), FMath::Max(MinRecheckTime, MaxRecheckTime));
	GetWorldTimerManager().SetTimer(TimerHandle, this,
		&AWoTRandomEventTrigger::OnTimer, Delay, false);
}

void AWoTRandomEventTrigger::OnTimer()
{
	if ((bLocalOnly || HasAuthority()) && bActive && Events.Num() > 0
		&& WoTRandom::FRand() <= TriggerProbability)
	{
		const FName Event = Events[WoTRandom::RandRange(0, Events.Num() - 1)];
		UWoTEventSubsystem* EventSubsystem =
			GetWorld()->GetSubsystem<UWoTEventSubsystem>();
		if (bLocalOnly)
		{
			EventSubsystem->FirePresentationEvent(
				Event, TriggerInstigatorActor.Get(), EventInstigatorPawn.Get());
		}
		else
		{
			EventSubsystem->FireEvent(
				Event, TriggerInstigatorActor.Get(), EventInstigatorPawn.Get());
		}
	}
	ScheduleNext();
}

AWoTTriggerLight::AWoTTriggerLight()
{
	PrimaryActorTick.bCanEverTick = true;
	bReplicates = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Light = CreateDefaultSubobject<UPointLightComponent>(TEXT("Light"));
	Light->SetupAttachment(RootComponent);
	Light->SetMobility(EComponentMobility::Movable);
	Light->SetCastShadows(false);
	Light->SetUseInverseSquaredFalloff(false);
}

void AWoTTriggerLight::BeginPlay()
{
	Super::BeginPlay();
	CurrentAlpha = TargetAlpha = bInitiallyOn ? 1.0f : 0.0f;
	Light->SetIntensity(CurrentAlpha * FullIntensity);
	if (IsWoTBootstrapWorldReady() && GetNetMode()!=NM_Client)
	{
		GetWorld()->GetSubsystem<UWoTEventSubsystem>()->RegisterTag(TagName, this);
	}
}

void AWoTTriggerLight::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	CurrentAlpha = FMath::FInterpConstantTo(CurrentAlpha, TargetAlpha, DeltaSeconds,
		ChangeTime <= 0.0f ? 1000000.0f : 1.0f / ChangeTime);
	if (!bDelayFullOn)
	{
		Light->SetIntensity(FullIntensity * CurrentAlpha);
	}
	else
	{
		const bool bWaitingToReachFull = TargetAlpha > CurrentAlpha
			&& !FMath::IsNearlyEqual(CurrentAlpha, 1.0f);
		Light->SetIntensity((bWaitingToReachFull || FMath::IsNearlyZero(CurrentAlpha))
			? 0.0f : FullIntensity);
	}
}

void AWoTTriggerLight::SetTarget(float NewTarget)
{
	if (!HasAuthority()) return;
	TargetAlpha = FMath::Clamp(NewTarget, 0.0f, 1.0f);
	ForceNetUpdate();
}

void AWoTTriggerLight::WoTTrigger(AActor*, APawn*)
{
	switch (Mode)
	{
	case EWoTTriggerLightMode::TurnsOn: SetTarget(1.0f); break;
	case EWoTTriggerLightMode::TurnsOff: SetTarget(0.0f); break;
	case EWoTTriggerLightMode::Toggle:
		SetTarget(TargetAlpha < 0.5f ? 1.0f : 0.0f);
		break;
	case EWoTTriggerLightMode::Pound:
		PoundElapsed = ChangeTime;
		SetTarget(1.0f);
		GetWorldTimerManager().SetTimer(PoundTimerHandle, this,
			&AWoTTriggerLight::OnPoundTimer, FMath::Max(0.001f, ChangeTime), false);
		break;
	case EWoTTriggerLightMode::Control: SetTarget(bInitiallyOn ? 0.0f : 1.0f); break;
	default: break;
	}
}

void AWoTTriggerLight::OnPoundTimer()
{
	if (!HasAuthority()) return;
	const bool bFinished = PoundElapsed >= RemainOnTime;
	PoundElapsed += ChangeTime;
	SetTarget(TargetAlpha < 0.5f ? 1.0f : 0.0f);
	if (!bFinished)
	{
		GetWorldTimerManager().SetTimer(PoundTimerHandle, this,
			&AWoTTriggerLight::OnPoundTimer, FMath::Max(0.001f, ChangeTime), false);
	}
}

void AWoTTriggerLight::WoTUnTrigger(AActor*, APawn*)
{
	if (Mode == EWoTTriggerLightMode::Control) SetTarget(bInitiallyOn ? 1.0f : 0.0f);
}

void AWoTTriggerLight::OnRep_TargetAlpha()
{
	TargetAlpha = FMath::Clamp(TargetAlpha, 0.0f, 1.0f);
}

void AWoTTriggerLight::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWoTTriggerLight, TargetAlpha);
}
