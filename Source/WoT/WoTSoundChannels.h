#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "UObject/ObjectKey.h"
#include "WoTSound.h"
#include "WoTSoundChannels.generated.h"

class AActor;
class UAudioComponent;
class USoundBase;
class UWoTAmbientSoundComponent;

// The original engine's effects channels for one world's local listener.
// One-shots, ambient loops and interface sounds all take one of sixteen
// channels. A sound's priority is volume x (1 - distance / radius), measured
// from the listener's view target; a newcomer replaces the sound in its own
// actor's slot, or else takes the last of the quietest channels whose
// priority does not exceed its own, or is not played. On a network client a
// slot belongs either to the server's sounds or to the client's own
// (WoTSound::ECaller), never to both. A sound whose engine voice does not
// start gives its channel back at once.
//
// Every frame each channel follows its actor (a sound whose actor is
// destroyed finishes from where the actor was), refreshes its priority, and
// ambient loops start inside their radius, stop outside it and take Doppler
// from their actor's motion. Local presentation: dedicated servers keep no
// channels.
UCLASS()
class WOT_API UWoTSoundChannels final : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	static UWoTSoundChannels* Get(const UWorld* World);

	// Actor.PlaySound heard by the local listener (the hearing test, the
	// underwater rules, then a channel). Returns the engine voice if one
	// started; a null Sound only stops the source's slot.
	UAudioComponent* Play(const WoTSound::FPlayParams& Params);
	// The same for a voice heard without position (a ClientPlaySound copy).
	UAudioComponent* PlayInterface(const WoTSound::FPlayParams& Params);

	// An ambient loop that wants to play (UWoTAmbientSoundComponent::Play) or
	// no longer does (Stop). It holds a channel only while it is in range and
	// wins one.
	void AddAmbient(UWoTAmbientSoundComponent& Ambient);
	void RemoveAmbient(UWoTAmbientSoundComponent& Ambient);

	// UTickableWorldSubsystem
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickable() const override;

#if WITH_AUTOMATION_TESTS
	WoTSound::EEffectAdmissionResult PrepareForTesting(const WoTSound::FPlayParams& Params);
	int32 SeedForTesting(TConstArrayView<WoTSound::FEffectVoicePrioritySample> Voices);
	TArray<WoTSound::FChannelSnapshot> SnapshotForTesting() const;
#endif

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	struct FChannel
	{
		bool bBusy = false;
		// Slot identity: the playing actor (an ambient loop: its component),
		// the slot, and on a network client whether the client played it
		// itself. SLOT_None and actorless sounds never match.
		FObjectKey SourceKey;
		WoTSound::ESlot Slot = WoTSound::ESlot::None;
		bool bClientOwnSlot = false;
		TWeakObjectPtr<const AActor> Source;
		bool bHadSource = false;
		// A looping AmbientSound (its component starts and stops the voice).
		bool bAmbientLoop = false;
		TWeakObjectPtr<UWoTAmbientSoundComponent> Ambient;
		TWeakObjectPtr<UAudioComponent> Voice;
		bool bVoicePlayed = false;
		bool bInterface = false;
		FName SoundName;
		FVector Location = FVector::ZeroVector;
		float Volume = 0.0f;
		float Radius = 0.0f;
		float Pitch = 1.0f;
		float Priority = 0.0f;
		bool bLooping = false;
		double EndTime = 0.0;
	};

	enum class EAdmission : uint8
	{
		Admitted,
		NoListener,
		OutsideHearingRadius,
		Rejected,
	};

	// PlaySound's hearing test and ClientHearSound's rules, then the channel
	// choice. Leaves OutChannel INDEX_NONE unless a channel was granted.
	EAdmission Admit(const WoTSound::FPlayParams& Params, float& OutVolume,
		float& OutRadius, float& OutPitch, float& OutPriority, int32& OutChannel) const;
	// Whether a play from Caller takes the client's own slots: only a play
	// this network client makes itself.
	bool TakesClientOwnSlot(WoTSound::ECaller Caller) const;
	int32 ChooseChannel(const FObjectKey& Key, bool bClientOwnSlot,
		WoTSound::ESlot Slot, bool bNoOverride, float Priority) const;
	UAudioComponent* Start(const WoTSound::FPlayParams& Params, bool bInterface);
	void StopChannel(int32 Index);
	bool HoldsChannel(const UWoTAmbientSoundComponent& Ambient) const;
	void StartAmbience(const FVector& View);
	void UpdateAmbience(const FVector& View);
	void UpdateChannels(const FVector& View, bool bHasView);

	FChannel Channels[WoTSound::EffectsChannelCount];
	TArray<TWeakObjectPtr<UWoTAmbientSoundComponent>> Ambients;
};
