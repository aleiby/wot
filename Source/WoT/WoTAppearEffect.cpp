#include "WoTAppearEffect.h"

#include "WoTParticleSprayer.h"
#include "WoTWeaveFX.h"

#include "Engine/World.h"
#include "Net/UnrealNetwork.h"

AWoTAppearEffect::AWoTAppearEffect()
{
	PrimaryActorTick.bCanEverTick = true;
	bReplicates = true;   // RemoteRole=ROLE_SimulatedProxy (AppearEffect.uc:303)
	SetReplicatingMovement(false);
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	NetPriority = 6.0f;
}

FName AWoTAppearEffect::CoronaForColor(FName Color)
{
	const FString Name = Color.ToString();
	if (Name == TEXT("Red") || Name == TEXT("PC_Red"))
	{
		return FName(TEXT("RedCorona"));
	}
	if (Name == TEXT("Green") || Name == TEXT("PC_Green"))
	{
		return FName(TEXT("GreenCorona"));
	}
	if (Name == TEXT("Purple") || Name == TEXT("PC_Purple"))
	{
		return FName(TEXT("PurpleCorona"));
	}
	if (Name == TEXT("Gold") || Name == TEXT("PC_Gold")
		|| Name == TEXT("Yellow") || Name == TEXT("PC_Yellow"))
	{
		return FName(TEXT("YellowCorona"));
	}
	return FName(TEXT("CyanCorona"));   // 'Blue'/'PC_Blue'/default (uc:102-106)
}

void AWoTAppearEffect::ResolveColorPair(FName TopColor, FName BottomColor,
	FName& OutTop, FName& OutBottom)
{
	// SetColors (uc:52-74): both empty -> Blue/Blue; one empty -> both the given.
	if (TopColor.IsNone() && BottomColor.IsNone())
	{
		OutTop = OutBottom = FName(TEXT("Blue"));
	}
	else if (TopColor.IsNone())
	{
		OutTop = OutBottom = BottomColor;
	}
	else if (BottomColor.IsNone())
	{
		OutTop = OutBottom = TopColor;
	}
	else
	{
		OutTop = TopColor;
		OutBottom = BottomColor;
	}
}

AWoTAppearEffect* AWoTAppearEffect::SpawnWithSprites(UWorld& World,
	AActor* AppearTarget, FName TopCorona, FName BottomCorona, float Duration,
	float InTargetRadiusOverride, float InTargetHalfHeightOverride)
{
	if (World.bIsTearingDown || World.GetNetMode() == NM_Client)
	{
		return nullptr;
	}
	// SetAppearActor (uc:246-261): a null or hidden target destroys the effect —
	// the port simply never spawns it.
	if (!IsValid(AppearTarget) || AppearTarget->IsHidden())
	{
		return nullptr;
	}
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTAppearEffect* Effect = World.SpawnActor<AWoTAppearEffect>(
		AppearTarget->GetActorLocation(), FRotator::ZeroRotator, Params);
	if (Effect)
	{
		Effect->TopSprite = TopCorona;
		Effect->BottomSprite = BottomCorona;
		Effect->AppearTarget = AppearTarget;
		Effect->EffectDuration = Duration;
		Effect->TargetRadiusOverride = InTargetRadiusOverride;
		Effect->TargetHalfHeightOverride = InTargetHalfHeightOverride;
		// Retail LifeSpan(1.5) += ExtraFadeTime at Initialize (uc:225).
		Effect->SetLifeSpan(Duration + ExtraFadeTime);
	}
	return Effect;
}

AWoTAppearEffect* AWoTAppearEffect::SpawnColored(UWorld& World,
	AActor* AppearTarget, FName TopColor, FName BottomColor, float Duration)
{
	FName Top, Bottom;
	ResolveColorPair(TopColor, BottomColor, Top, Bottom);
	return SpawnWithSprites(World, AppearTarget,
		CoronaForColor(Top), CoronaForColor(Bottom), Duration);
}

AWoTAppearEffect* AWoTAppearEffect::SpawnLocalWithSprites(UWorld& World,
	AActor* AppearTarget, FName TopCorona, FName BottomCorona, float Duration,
	float InTargetRadiusOverride, float InTargetHalfHeightOverride)
{
	if (World.bIsTearingDown || World.GetNetMode() == NM_DedicatedServer
		|| !IsValid(AppearTarget) || AppearTarget->IsHidden())
	{
		return nullptr;
	}
	const FTransform Transform(FRotator::ZeroRotator,
		AppearTarget->GetActorLocation());
	AWoTAppearEffect* Effect = World.SpawnActorDeferred<AWoTAppearEffect>(
		AWoTAppearEffect::StaticClass(), Transform, nullptr, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!Effect) return nullptr;
	Effect->SetReplicates(false);
	Effect->TopSprite = TopCorona;
	Effect->BottomSprite = BottomCorona;
	Effect->AppearTarget = AppearTarget;
	Effect->EffectDuration = Duration;
	Effect->TargetRadiusOverride = InTargetRadiusOverride;
	Effect->TargetHalfHeightOverride = InTargetHalfHeightOverride;
	Effect->FinishSpawning(Transform);
	Effect->SetLifeSpan(Duration + ExtraFadeTime);
	return Effect;
}

void AWoTAppearEffect::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	Elapsed += DeltaSeconds;

	AActor* Target = AppearTarget;
	if (!IsValid(Target) || Target->IsHidden())
	{
		// uc:174-177: target gone/hidden -> Destroy. Only the authority may
		// destroy a replicated actor in UE5 — a client tears its visuals down
		// and coasts to the channel close; the pre-replication window before
		// the target ref resolves on a client just waits (rings not started).
		if (bRings && !bParticlesOff)
		{
			TurnOffParticles();
		}
		// AppearEffect.uc:149-176 destroys unconditionally for a gone/hidden target.
		// bRings only gates visual teardown and the client's pre-replication null wait.
		if (HasAuthority())
		{
			Destroy();
		}
		return;
	}

	if (Elapsed < EffectDuration)
	{
		if (!bRings)
		{
			InitializeRings();   // first eligible tick (uc:151-156)
		}
		else
		{
			// uc:159-161: converge, then reposition both rings.
			RingOffset -= HeightShiftRate * DeltaSeconds;
			OrbitPhaseRadians += (SprayerRotationRate / 65536.0f)
				* 2.0f * UE_PI * DeltaSeconds;
			UpdateRingPositions();
		}
	}
	else if (bRings && !bParticlesOff)
	{
		TurnOffParticles();      // uc:163-166 (LifeSpan <= ExtraFadeTime)
	}
}

void AWoTAppearEffect::InitializeRings()
{
	AActor* Target = AppearTarget;
	// Local cosmetics: SpawnLocal no-ops on dedicated servers; the effect actor
	// itself still lives out its replicated lifetime.
	const FName TopConfig(*FString::Printf(TEXT("AppearSprayer%s"),
		*TopSprite.ToString()));
	const FName BottomConfig(*FString::Printf(TEXT("AppearSprayer%s"),
		*BottomSprite.ToString()));
	OrbitRadius = TargetRadiusOverride > 0.0f
		? TargetRadiusOverride : Target->GetSimpleCollisionRadius();
	RingOffset = TargetHalfHeightOverride > 0.0f
		? TargetHalfHeightOverride : Target->GetSimpleCollisionHalfHeight();
	// uc:223: (CollisionHeight*2)/LifeSpan, computed BEFORE the ExtraFadeTime
	// extension — the rings cross the full height during the converge phase.
	HeightShiftRate = (RingOffset * 2.0f) / FMath::Max(EffectDuration, 0.01f);
	BottomSprayer = WoTWeaveFX::SpawnLocal(*GetWorld(), BottomConfig,
		Target->GetActorLocation() - FVector(0, 0, RingOffset),
		FRotator::ZeroRotator);
	TopSprayer = WoTWeaveFX::SpawnLocal(*GetWorld(), TopConfig,
		Target->GetActorLocation() + FVector(0, 0, RingOffset),
		FRotator::ZeroRotator);
	bRings = true;
	UpdateRingPositions();
}

void AWoTAppearEffect::UpdateRingPositions()
{
	const AActor* Target = AppearTarget;
	if (!IsValid(Target))
	{
		return;
	}
	const FVector Center = Target->GetActorLocation();
	// Counter-rotation (uc:200/211: rate vs -rate on the two rotators).
	const FVector Orbit(FMath::Cos(OrbitPhaseRadians) * OrbitRadius,
		FMath::Sin(OrbitPhaseRadians) * OrbitRadius, 0.0f);
	const FVector CounterOrbit(FMath::Cos(-OrbitPhaseRadians) * OrbitRadius,
		FMath::Sin(-OrbitPhaseRadians) * OrbitRadius, 0.0f);
	if (IsValid(BottomSprayer))
	{
		BottomSprayer->SetActorLocation(
			Center - FVector(0, 0, RingOffset) + Orbit);
	}
	if (IsValid(TopSprayer))
	{
		TopSprayer->SetActorLocation(
			Center + FVector(0, 0, RingOffset) + CounterOrbit);
	}
}

void AWoTAppearEffect::TurnOffParticles()
{
	// uc:278-288: sprayers off with a 2s linger; the orbit drive stops.
	bParticlesOff = true;
	WoTWeaveFX::FadeOut(BottomSprayer, 2.0f);
	WoTWeaveFX::FadeOut(TopSprayer, 2.0f);
}

void AWoTAppearEffect::Destroyed()
{
	// uc:264-275: an early destroy (pickup taken -> BecomeItem) still lingers
	// the particles out instead of popping them.
	if (bRings && !bParticlesOff)
	{
		TurnOffParticles();
	}
	Super::Destroyed();
}

void AWoTAppearEffect::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWoTAppearEffect, TopSprite);
	DOREPLIFETIME(AWoTAppearEffect, BottomSprite);
	DOREPLIFETIME(AWoTAppearEffect, AppearTarget);
	DOREPLIFETIME(AWoTAppearEffect, EffectDuration);
	DOREPLIFETIME(AWoTAppearEffect, TargetRadiusOverride);
	DOREPLIFETIME(AWoTAppearEffect, TargetHalfHeightOverride);
}
