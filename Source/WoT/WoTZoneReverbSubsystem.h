#pragma once

#include "CoreMinimal.h"
#include "AudioDeviceHandle.h"
#include "Sound/SoundSubmix.h"
#include "Subsystems/WorldSubsystem.h"
#include "WoTZoneReverb.h"
#include "WoTZoneReverbSubsystem.generated.h"

class UAudioComponent;
class UWoTZoneReverbPreset;

// Runtime-only: UE keeps the dynamic flag protected, so establish it at
// construction before any device can enumerate this submix.
UCLASS(Transient)
class UWoTZoneReverbSubmix final : public USoundSubmix
{
	GENERATED_BODY()

public:
	UWoTZoneReverbSubmix(const FObjectInitializer& ObjectInitializer);
};

// The local listener's room reverb (WoTZoneReverb.h). Once per frame it takes
// the zone the local player's view target stands in (the actor's centre, not
// the camera; the player itself when there is no view target), reads that
// zone's reverb from the map's zone table, and hands it to the reverb effect
// only when it differs from the reverb in use, as the original did: so moving
// between zones with the same reverb leaves its tail alone, and any other
// change restarts it. No table, no listener, the "Room reverb" setting off, or
// a zone that is not a reverb zone: no reverb.
//
// The reverb runs as an effect on a submix of its own, created on this
// world's audio device the first time it is needed and fed by a send on every
// effects voice (AddSend), so the direct sound keeps its own path unchanged.
// Local presentation only: a dedicated server creates and sends nothing.
UCLASS()
class WOT_API UWoTZoneReverbSubsystem final : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	static UWoTZoneReverbSubsystem* Get(const UWorld* World);

	// Adds the reverb send to an effects voice: one-shots, ambient loops and
	// interface sounds alike, at 127/128 of the voice's heard level (after
	// distance and panning). Call it before the voice's Play so the send is in
	// place from the voice's first sample. The original also reverberated its
	// music, which no multiplayer map plays; a music player, should the port
	// gain one, sends its voice here too.
	static void AddSend(const UWorld* World, UAudioComponent& Voice);

	// UTickableWorldSubsystem
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickable() const override;
	virtual bool IsTickableWhenPaused() const override { return true; }

	// The reverb now in use and how many times it changed.
	const FWoTZoneReverbSettings& GetCurrentSettings() const { return Current; }
	int32 GetChangeCount() const { return Changes; }
	// The zone the last frame's listener stood in (INDEX_NONE without one).
	int32 GetListenerZone() const { return ListenerZone; }
	USoundSubmix* GetSubmix() const { return Submix; }
	UWoTZoneReverbPreset* GetPreset() const { return Preset; }

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	bool IsLocalPresentation() const;
	bool EnsureSubmix();

	UPROPERTY(Transient)
	TObjectPtr<USoundSubmix> Submix;

	UPROPERTY(Transient)
	TObjectPtr<UWoTZoneReverbPreset> Preset;
	FAudioDeviceHandle OwningAudioDevice;

	FWoTZoneReverbSettings Current;
	int32 Changes = 0;
	int32 ListenerZone = INDEX_NONE;
	bool bSubmixFailed = false;
};
