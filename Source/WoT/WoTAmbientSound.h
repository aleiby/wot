#pragma once

#include "CoreMinimal.h"
#include "Components/AudioComponent.h"
#include "GameFramework/Actor.h"
#include "Subsystems/WorldSubsystem.h"
#include "WoTAmbientSound.generated.h"

// UE1 looping AmbientSound, ported (presentation Slice 8). Every 1999 actor with a
// non-None AmbientSound emitted it looped and spatialized (UE1 audio subsystem):
// steady-state volume 2.0*AmbientFactor*SoundVolume/255, pitch SoundPitch/64,
// and linear falloff to zero at 25*(SoundRadius+1) UU. UE5 keeps an
// out-of-range loop virtualized where UE1 stopped and restarted it at the
// range edge. Light-carried loops follow the shared live retail waveform and
// brightness value every tick.
UCLASS(meta = (BlueprintSpawnableComponent))
class UWoTAmbientSoundComponent : public UAudioComponent
{
	GENERATED_BODY()

public:
	UWoTAmbientSoundComponent();

	// Set the loop + the UE1 byte-derived play parameters (does not Play). The wave
	// itself carries the loop flag (import_sounds.py smpl/.uc/T3D derivation).
	// bLightCarried: the emitter has LightType != LT_None, whose ambient volume
	// UE1 routed through URender::GlobalLighting, which clamps it to [0,1].
	void ConfigureUE1Ambient(USoundBase* InSound, uint8 RadiusByte, uint8 VolumeByte,
	                         uint8 PitchByte, float ExtraVolumeFactor = 1.0f,
	                         bool bLightCarried = false);

	// The listener-head-underwater muffle (UE1's audio subsystem): while
	// the LOCAL player's head is in a water zone every ambient loop plays at
	// volume x0.90, falloff radius x0.80, pitch x0.77 (ambient channels only —
	// one-shots take the script-side x0.85 pitch rule in WoTSound).
	// UWoTUnderwaterAudioSubsystem drives the flips.
	void SetUnderwaterMuffle(bool bMuffle);
	/** Re-applies UE1 GlobalLighting's live light factor without disturbing
	 * listener-underwater state. */
	void SetLightVolumeFactor(float InFactor, bool bLightCarried);

	// Pre-muffle configured state (ConfigureUE1Ambient stamps these).
	float BaseVolumeMultiplier = 1.0f;
	float BasePitchMultiplier = 1.0f;
	float BaseRadiusUU = 0.0f;
	bool bMuffled = false;

private:
	float UnlitBaseVolumeMultiplier = 1.0f;
	// Re-derive the live play parameters from the base values + the muffle state.
	void ApplyMuffleState();
};

// Watches the local player's head against the water zones (a 0.1s poll off the
// world timer — UE1 refreshed per audio-frame in its audio code; timer-driven here so
// the flip is deterministic headless, PORT_NOTES divergence); on a change it pushes
// SetUnderwaterMuffle to every live ambient component (flips are rare — the object
// walk only runs then). Client/listen only.
UCLASS()
class UWoTUnderwaterAudioSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;

	bool bListenerUnderwater = false;

private:
	void Poll();
	FTimerHandle PollTimer;
};

// A placed ambient emitter: one per map actor carrying an AmbientSound (the importer
// spawns these for AmbientSound keypoints, torch flames, lights, sprayers... with the
// EFFECTIVE byte values — instance override, else class default: native AmbientSound
// keypoint 64/190/64, Actor 32/128/64).
UCLASS()
class AWoTAmbientSound : public AActor
{
	GENERATED_BODY()

public:
	AWoTAmbientSound();

	// Imported-asset subpath under /Game/WoT/Sounds ("AmbientA/Fire17" — the UE1
	// Pkg.Group.Name group folds away at import).
	UPROPERTY(EditAnywhere, Category = "WoT") FName SoundSubPath;
	UPROPERTY(EditAnywhere, Category = "WoT") uint8 SoundRadius = 64;
	UPROPERTY(EditAnywhere, Category = "WoT") uint8 SoundVolume = 190;
	UPROPERTY(EditAnywhere, Category = "WoT") uint8 SoundPitch = 64;
	UPROPERTY(EditAnywhere, Category = "WoT") uint8 LightType = 0;
	UPROPERTY(EditAnywhere, Category = "WoT") uint8 LightBrightness = 0;
	UPROPERTY(EditAnywhere, Category = "WoT") uint8 LightPeriod = 0;
	UPROPERTY(EditAnywhere, Category = "WoT") uint8 LightPhase = 0;
	uint64 LightRegistrationId = 0;

	UPROPERTY(EditAnywhere, Category = "WoT")
	TObjectPtr<UWoTAmbientSoundComponent> Ambient;

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaSeconds) override;
	void UpdateLightVolumeAtTime(float TimeSeconds, uint64 Frame);

private:
	void HandleWorldReadinessChanged(bool bReady);
	void ActivateForAuthorizedWorld();
	void DeactivateForUnauthorizedWorld();
	FDelegateHandle WorldReadinessHandle;
	bool bAuthorizedWorldActive = false;
};

// DynamicAmbientSound.uc port: a keypoint playing random one-shots from Sounds[6]
// on a 5-10s (default) recheck timer, odds-gated per fire, optionally never
// repeating the same pick twice. NOTE its own scaling differs from ambient loops:
// PlaySound(..., SoundVolume/128, ..., SoundRadius*25) — DynamicAmbientSound.uc:40,60.
// The port rolls per machine (UE1 rolled server-side and replicated the play) —
// cosmetic chirps, divergence noted in PORT_NOTES.
UCLASS()
class AWoTDynamicAmbientSound : public AActor
{
	GENERATED_BODY()

public:
	AWoTDynamicAmbientSound();

	UPROPERTY(EditAnywhere, Category = "WoT") bool bInitiallyOn = false;
	// Up to 6 subpaths under /Game/WoT/Sounds; the first empty entry terminates the
	// usable set (shipped BeginPlay count).
	UPROPERTY(EditAnywhere, Category = "WoT") TArray<FName> SoundSubPaths;
	UPROPERTY(EditAnywhere, Category = "WoT") float PlayProbability = 0.6f;
	UPROPERTY(EditAnywhere, Category = "WoT") float MinReCheckTime = 5.0f;
	UPROPERTY(EditAnywhere, Category = "WoT") float MaxReCheckTime = 10.0f;
	UPROPERTY(EditAnywhere, Category = "WoT") bool bDontRepeat = false;
	UPROPERTY(EditAnywhere, Category = "WoT") uint8 SoundRadius = 32;   // Actor default
	UPROPERTY(EditAnywhere, Category = "WoT") uint8 SoundVolume = 42;   // class default

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	// Shipped semantics, public for specs.
	int32 CountSounds() const;
	int32 PickSoundIndex(int32 NumSounds);

#if WITH_AUTOMATION_TESTS
	UAudioComponent* PlayIndexForTesting(int32 Index) { return PlayIndex(Index); }
#endif

	int32 LastSound = -1;
	FTimerHandle RecheckTimer;

private:
	void HandleWorldReadinessChanged(bool bReady);
	void ActivateForAuthorizedWorld();
	void DeactivateForUnauthorizedWorld();
	void OnRecheck();
	void ScheduleRecheck();
	UAudioComponent* PlayIndex(int32 Index);
	FDelegateHandle WorldReadinessHandle;
	bool bAuthorizedWorldActive = false;
};
