#include "WoTAmbientSound.h"
#include "WoTRandom.h"

#include "WoTSound.h"
#include "WoTZoneVolume.h"
#include "WoTDynamicLightSubsystem.h"
#include "Payload/WoTPayloadActivation.h"

#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Sound/SoundAttenuation.h"
#include "TimerManager.h"
#include "UObject/UObjectIterator.h"

UWoTAmbientSoundComponent::UWoTAmbientSoundComponent()
{
	// Pure local presentation; owners decide when to Play/Stop.
	bAutoActivate = false;
	SetIsReplicatedByDefault(false);
}

void UWoTAmbientSoundComponent::ConfigureUE1Ambient(USoundBase* InSound, uint8 RadiusByte,
                                                    uint8 VolumeByte, uint8 PitchByte,
                                                    float ExtraVolumeFactor,
                                                    bool bLightCarried)
{
	SetSound(InSound);
	BaseRadiusUU = WoTSound::WorldSoundRadius(RadiusByte);
	// Steady-state native volume is 2.0 * AmbientFactor * SoundVolume/255 — the
	// in-range start tick played at 1x but the first update doubled it
	// in UE1; the port uses the steady state throughout,
	// with the retail ini's AmbientFactor 0.7.
	UnlitBaseVolumeMultiplier = 2.0f * WoTSound::AmbientFactor
		* (static_cast<float>(VolumeByte) / 255.0f);
	SetLightVolumeFactor(ExtraVolumeFactor, bLightCarried);
	BasePitchMultiplier = static_cast<float>(PitchByte) / 64.0f;
	// A component configured while the listener is already submerged starts muffled.
	if (const UWorld* World = GetWorld())
	{
		if (const UWoTUnderwaterAudioSubsystem* Sub =
		        World->GetSubsystem<UWoTUnderwaterAudioSubsystem>())
		{
			bMuffled = Sub->bListenerUnderwater;
		}
	}
	ApplyMuffleState();
}

void UWoTAmbientSoundComponent::SetLightVolumeFactor(
	const float InFactor, const bool bLightCarried)
{
	BaseVolumeMultiplier = UnlitBaseVolumeMultiplier
		* FMath::Max(0.0f, InFactor);
	if (bLightCarried)
	{
		// UE1's audio subsystem sends the brightness-modulated value through
		// GlobalLighting, whose final stage clamps the voice volume to [0,1].
		BaseVolumeMultiplier = FMath::Min(BaseVolumeMultiplier, 1.0f);
	}
	ApplyMuffleState();
}

void UWoTAmbientSoundComponent::SetUnderwaterMuffle(bool bMuffle)
{
	if (bMuffled == bMuffle)
	{
		return;
	}
	bMuffled = bMuffle;
	ApplyMuffleState();
}

void UWoTAmbientSoundComponent::ApplyMuffleState()
{
	// UE1's underwater muffle — volume x0.90, radius x0.80, pitch x0.77.
	const float Volume = BaseVolumeMultiplier * (bMuffled ? 0.90f : 1.0f);
	SetVolumeMultiplier(Volume);
	SetPitchMultiplier(BasePitchMultiplier * (bMuffled ? 0.77f : 1.0f));
	// The voice gain is Clamp(Volume * (1 - d/R), 0, 1) in UE1:
	// loud loops (2*0.7*SoundVolume/255 reaches 1.4) flatten at full scale.
	AttenuationSettings =
	    WoTSound::LinearAttenuation(BaseRadiusUU * (bMuffled ? 0.80f : 1.0f), Volume);
	if (IsPlaying() && AttenuationSettings)
	{
		// Push the rescaled falloff onto the already-active sound.
		AdjustAttenuation(AttenuationSettings->Attenuation);
	}
}

void UWoTUnderwaterAudioSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	if (!IsWoTBootstrapWorldReady())
	{
		return;
	}
	// Dedicated servers have no listeners; everything else polls the local
	// player's head at 10Hz (UE1 refreshed per audio-frame — PORT_NOTES).
	if (InWorld.GetNetMode() != NM_DedicatedServer)
	{
		InWorld.GetTimerManager().SetTimer(
		    PollTimer, this, &UWoTUnderwaterAudioSubsystem::Poll, 0.1f, true);
	}
}

void UWoTUnderwaterAudioSubsystem::Poll()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	bool bUnder = false;
	if (const APlayerController* PC = World->GetFirstPlayerController())
	{
		if (const APawn* Pawn = PC->GetPawn())
		{
			bUnder = AWoTZoneVolume::IsPointInWater(
			    World, Pawn->GetActorLocation() +
			               FVector(0.0f, 0.0f, Pawn->BaseEyeHeight));
		}
	}
	if (bUnder == bListenerUnderwater)
	{
		return;
	}
	bListenerUnderwater = bUnder;
	for (TObjectIterator<UWoTAmbientSoundComponent> It; It; ++It)
	{
		if (IsValid(*It) && It->GetWorld() == World)
		{
			It->SetUnderwaterMuffle(bUnder);
		}
	}
}

AWoTAmbientSound::AWoTAmbientSound()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;
	Ambient = CreateDefaultSubobject<UWoTAmbientSoundComponent>(TEXT("Ambient"));
	SetRootComponent(Ambient);
	bReplicates = false;
	SetCanBeDamaged(false);
}

void AWoTAmbientSound::BeginPlay()
{
	Super::BeginPlay();
	LightRegistrationId = static_cast<uint64>(GetTypeHash(GetPathName()));
	SetActorTickEnabled(LightType != 0 && GetNetMode() != NM_DedicatedServer);
	WorldReadinessHandle = RegisterWoTBootstrapWorldReadinessListener(
		FWoTBootstrapWorldReadinessEvent::FDelegate::CreateUObject(
			this, &AWoTAmbientSound::HandleWorldReadinessChanged));
	ActivateForAuthorizedWorld();
}

void AWoTAmbientSound::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	UnregisterWoTBootstrapWorldReadinessListener(WorldReadinessHandle);
	DeactivateForUnauthorizedWorld();
	Super::EndPlay(EndPlayReason);
}

void AWoTAmbientSound::HandleWorldReadinessChanged(bool bReady)
{
	if (bReady)
	{
		ActivateForAuthorizedWorld();
	}
	else
	{
		DeactivateForUnauthorizedWorld();
	}
}

void AWoTAmbientSound::ActivateForAuthorizedWorld()
{
	if (bAuthorizedWorldActive || !IsWoTBootstrapWorldReady()
		|| GetNetMode() == NM_DedicatedServer || SoundSubPath.IsNone())
	{
		return;
	}
	bAuthorizedWorldActive = true;
	if (USoundBase* Wave = WoTSound::LoadBySubPath(SoundSubPath, true))
	{
		const bool bLightCarried = LightType != 0;
		Ambient->ConfigureUE1Ambient(Wave, SoundRadius, SoundVolume, SoundPitch,
			bLightCarried ? static_cast<float>(LightBrightness) / 255.0f : 1.0f,
			bLightCarried);
		UpdateLightVolumeAtTime(GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f, 0);
		Ambient->Play();
	}
}

void AWoTAmbientSound::UpdateLightVolumeAtTime(
	const float TimeSeconds, const uint64 Frame)
{
	if (Ambient == nullptr || LightType == 0)
	{
		return;
	}
	const float Wave = UWoTDynamicLightSubsystem::EvaluateRetailWaveform(
		LightType, TimeSeconds, LightPeriod, LightPhase, LightRegistrationId, Frame);
	Ambient->SetLightVolumeFactor(
		(static_cast<float>(LightBrightness) / 255.0f) * Wave, true);
}

void AWoTAmbientSound::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	const UWorld* World = GetWorld();
	if (World != nullptr)
	{
		UpdateLightVolumeAtTime(World->GetTimeSeconds(),
			static_cast<uint64>(FMath::FloorToInt64(World->GetTimeSeconds() * 35.0)));
	}
}

void AWoTAmbientSound::DeactivateForUnauthorizedWorld()
{
	bAuthorizedWorldActive = false;
	if (Ambient)
	{
		Ambient->Stop();
		Ambient->SetSound(nullptr);
	}
}

AWoTDynamicAmbientSound::AWoTDynamicAmbientSound()
{
	PrimaryActorTick.bCanEverTick = false;
	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Root")));
	bReplicates = false;
	SetCanBeDamaged(false);
}

void AWoTDynamicAmbientSound::BeginPlay()
{
	Super::BeginPlay();
	WorldReadinessHandle = RegisterWoTBootstrapWorldReadinessListener(
		FWoTBootstrapWorldReadinessEvent::FDelegate::CreateUObject(
			this, &AWoTDynamicAmbientSound::HandleWorldReadinessChanged));
	ActivateForAuthorizedWorld();
}

void AWoTDynamicAmbientSound::EndPlay(
	const EEndPlayReason::Type EndPlayReason)
{
	UnregisterWoTBootstrapWorldReadinessListener(WorldReadinessHandle);
	DeactivateForUnauthorizedWorld();
	Super::EndPlay(EndPlayReason);
}

void AWoTDynamicAmbientSound::HandleWorldReadinessChanged(bool bReady)
{
	if (bReady)
	{
		ActivateForAuthorizedWorld();
	}
	else
	{
		DeactivateForUnauthorizedWorld();
	}
}

void AWoTDynamicAmbientSound::ActivateForAuthorizedWorld()
{
	// Each machine rolls its own picks locally (dedicated servers have no
	// listeners); UE1 rolled once server-side and replicated the PlaySound.
	if (bAuthorizedWorldActive || !IsWoTBootstrapWorldReady()
		|| GetNetMode() == NM_DedicatedServer)
	{
		return;
	}
	bAuthorizedWorldActive = true;
	const int32 NumSounds = CountSounds();
	if (NumSounds <= 0)
	{
		return;
	}
	if (bInitiallyOn)
	{
		PlayIndex(PickSoundIndex(NumSounds));
	}
	ScheduleRecheck();
}

void AWoTDynamicAmbientSound::DeactivateForUnauthorizedWorld()
{
	bAuthorizedWorldActive = false;
	GetWorldTimerManager().ClearTimer(RecheckTimer);
	LastSound = -1;
}

int32 AWoTDynamicAmbientSound::CountSounds() const
{
	const int32 Max = FMath::Min(SoundSubPaths.Num(), 6);
	int32 Num = 0;
	while (Num < Max && !SoundSubPaths[Num].IsNone())
	{
		++Num;
	}
	return Num;
}

int32 AWoTDynamicAmbientSound::PickSoundIndex(int32 NumSounds)
{
	int32 Index = WoTRandom::RandHelper(NumSounds);
	while (Index == LastSound && bDontRepeat && NumSounds > 1)
	{
		Index = WoTRandom::RandHelper(NumSounds);
	}
	return Index;
}

void AWoTDynamicAmbientSound::OnRecheck()
{
	if (!bAuthorizedWorldActive || !IsWoTBootstrapWorldReady())
	{
		DeactivateForUnauthorizedWorld();
		return;
	}
	const int32 NumSounds = CountSounds();
	if (NumSounds > 0 && WoTRandom::FRand() <= PlayProbability)
	{
		PlayIndex(PickSoundIndex(NumSounds));
	}
	ScheduleRecheck();
}

void AWoTDynamicAmbientSound::ScheduleRecheck()
{
	if (!bAuthorizedWorldActive || !IsWoTBootstrapWorldReady())
	{
		return;
	}
	const float Delay =
	    FMath::Max(WoTRandom::FRandRange(MinReCheckTime, MaxReCheckTime), 0.01f);
	GetWorldTimerManager().SetTimer(RecheckTimer, this,
	                                &AWoTDynamicAmbientSound::OnRecheck, Delay, false);
}

UAudioComponent* AWoTDynamicAmbientSound::PlayIndex(int32 Index)
{
	if (!bAuthorizedWorldActive || !IsWoTBootstrapWorldReady()
		|| !SoundSubPaths.IsValidIndex(Index))
	{
		return nullptr;
	}
	// lastSound stamps regardless of resolution, matching the shipped flow.
	LastSound = Index;
	if (USoundBase* Wave = WoTSound::LoadBySubPath(SoundSubPaths[Index]))
	{
		// DynamicAmbientSound.uc:40/60 — vol/128, radius*25 (NOT the 25*(r+1)
		// ambient-loop rule), engine-default pitch.
		return WoTSound::PlayOneShot(this, Wave, GetActorLocation(),
		                      static_cast<float>(SoundRadius) * 25.0f, 1.0f,
		                      static_cast<float>(SoundVolume) / 128.0f);
	}
	return nullptr;
}
