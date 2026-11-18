#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectKey.h"

class AActor;
class APlayerController;
class USoundBase;
class USoundAttenuation;
class UAudioComponent;
class UWorld;

// The original engine's default audio path, ported for the game's positional
// sounds. A sound is played by an actor: it starts at that actor's location
// and follows it while the actor lives. The actor's slots decide which of its
// sounds replace one another; on a network client the sounds the server sent
// and the client's own sounds keep separate slots. Sixteen effects channels
// are shared by one-shots, ambient loops and interface sounds, and a new sound
// takes the quietest channel whose priority does not exceed its own. A channel
// is held only by a voice the engine started. Every voice fades linearly
// to silence at its world radius (Clamp(volume * (1 - distance / radius), 0, 1)).
// One-shots take a radius in world units and a float pitch; looping ambient
// sounds derive theirs from the byte SoundRadius/Volume/Pitch (radius
// 25 * (SoundRadius + 1); volume / 255; pitch / 64). Presentation only: none
// of this feeds the simulation.
namespace WoTSound
{
	// The original engine's per-actor sound slots, in its enum order (the
	// shipped WOTPlayer.uc SlotToStr names them). A non-None slot holds one
	// sound per actor: a new sound there replaces the old one unless the new
	// call says bNoOverride, which makes it yield instead. SLOT_None sounds
	// stack independently, and playing no sound on a slot stops it.
	enum class ESlot : uint8
	{
		None,
		Misc,
		Pain,
		Interact,
		Ambient,
		Talk,
		Interface,
	};

	// The machine whose PlaySound a play stands for. The original named a slot
	// by the playing actor's object index on the machine that called
	// PlaySound, so a network client kept the sounds the server sent apart
	// from the sounds it played itself, even for the same actor and slot. A
	// listen host or a standalone game is a single machine with one set of
	// slots, where the caller changes nothing.
	enum class ECaller : uint8
	{
		// The server: a replicated play, or this machine's own copy of a sound
		// the scripts play on the server (in a function that is not simulated).
		Server,
		// This machine alone: PlayerPawn.ClientPlaySound, or a PlaySound in
		// simulated code, which every machine runs for itself.
		Local,
	};

	// One Actor.PlaySound call: the sound (null stops Slot), the actor that
	// played it, where it started, and the raw script parameters. A radius of
	// 0 means the engine default, which is applied only at playback, after the
	// underwater rules have scaled the raw value.
	struct FPlayParams
	{
		USoundBase* Sound = nullptr;
		const AActor* Source = nullptr;
		FVector Location = FVector::ZeroVector;
		ESlot Slot = ESlot::Misc;   // PlaySound's default slot
		bool bNoOverride = false;
		float Volume = 1.0f;
		float Radius = 0.0f;
		float Pitch = 1.0f;
		// Whose slots it takes on a network client (see ECaller).
		ECaller Caller = ECaller::Server;
	};

	struct FEffectVoicePrioritySample
	{
		FVector SourceLocation = FVector::ZeroVector;
		float RadiusUU = 0.0f;
		float Volume = 0.0f;
	};

	// The original game's audio settings: sixteen effects channels, an ambient
	// gain of 0.7 on every looping ambient sound (one-shots are not affected)
	// and a Doppler speed of 6500 units per second.
	inline constexpr int32 EffectsChannelCount = 16;
	inline constexpr float AmbientFactor = 0.7f;
	inline constexpr float DopplerSpeed = 6500.0f;

	// PlayerPawn.ClientPlaySound plays the sound four times on the viewer, in
	// SLOT_None, Interface, Misc and Talk, at volume 16. Each voice clamps to
	// full scale, so UE5 gets four voices at normalized gain.
	inline constexpr int32 ClientUISoundVoiceCount = 4;
	inline constexpr float ClientSoundVolume = 16.0f;

	// The engine's radius for a PlaySound with none (TransientSoundRadius ships
	// unset on every class but one).
	inline constexpr float DefaultPlaySoundRadius = 1600.0f;

	// The original centred a sound progressively inside this fraction of its
	// radius, reaching the middle at the listener.
	inline constexpr float CentringRadiusFraction = 0.1f;

	// Byte SoundRadius -> world radius.
	float WorldSoundRadius(uint8 ByteRadius);

	// The engine voice's own volume multiplier for a play at Volume: the
	// volume itself, up to the engine's limit of four times full scale on a
	// voice's gain before its distance fade. LinearAttenuation carries the
	// rest of a louder play.
	float VoiceVolume(float Volume);

	// A cached linear-falloff attenuation reaching zero at RadiusUU, with the
	// close-range centring. Null for RadiusUU <= 0. Objects are cached per
	// rounded radius (and clamp volume) and kept alive against GC.
	//
	// Volume is the play's volume multiplier. The original gave every voice
	// Clamp(Volume * (1 - dist/Radius), 0, 1) of full scale, while UE5 lets a
	// source run to 4x. For Volume <= 1 the plain linear model is exact; for
	// Volume > 1 the curve is min(1, Volume*(1-d/R)) / VoiceVolume(Volume), so
	// with the voice at VoiceVolume(Volume) the product is exactly
	// min(1, Volume*(1-d/R)), however loud the play.
	USoundAttenuation* LinearAttenuation(float RadiusUU, float Volume = 1.0f);

	// PlaySound's hearing test: the listener must be strictly inside
	// radius / sqrt(1.3).
	bool PassesHearingGate(float DistanceUU, float RadiusUU);
	// A voice's priority: volume x (1 - distance / radius).
	float EffectsPriority(float DistanceUU, float RadiusUU, float Volume);
	// The channel a new sound takes, by priority alone: the last of the
	// lowest channels whose priority does not exceed the newcomer's. Missing
	// entries are idle channels at priority 0. INDEX_NONE rejects it.
	int32 ChooseEffectsVoice(TConstArrayView<float> ExistingPriorities,
		float NewPriority);
	// The same after refreshing each voice's priority at the listener.
	int32 ChooseEffectsVoiceForListener(
		TConstArrayView<FEffectVoicePrioritySample> ExistingVoices,
		const FVector& ListenerLocation, float NewPriority);

	// Ambient loops are pitched by their source's own motion along the line to
	// the listener (the listener's motion does not count):
	// Clamp(1 - radial speed / DopplerSpeed, 0.5, 2).
	float DopplerFactor(const FVector& SourceVelocity, const FVector& SourceLocation,
		const FVector& ListenerLocation);

#if WOT_WITH_BOOTSTRAP_TEST_HARNESSES
	// Trailer rig only (its `listener` command): pin one world's hearing-gate
	// origin, which is otherwise the first local player's view target. The rig
	// pins the UE audio listener near the action while its free camera films from
	// across a hall; the gate, still measured from the camera, would cull the very
	// sounds the pinned listener is placed to hear. Unpinned = retail behaviour.
	void SetHearingOriginOverride(const UWorld& World, const FVector& Location);
	void ClearHearingOriginOverride(const UWorld& World);
#endif
	// The hearing origin: the first local player's view target, or its focal
	// point when it has none.
	bool ListenerLocation(const UWorld& World, FVector& OutLocation);

	float NormalizedUISoundVoiceGain(float RetailVolume);
	float StackedUISoundEffectiveGain(float RetailVolume);

	// "Angreal/RespawnA" for /Game/WoT/Sounds/Angreal/RespawnA.RespawnA (the
	// PlayReplicated key); None for a null or out-of-tree reference.
	FName SubPathOf(const TSoftObjectPtr<USoundBase>& Sound);

	// Actor.PlaySound on this machine only: the hearing test, the underwater
	// rules, then an effects channel. Returns the engine voice when one starts.
	UAudioComponent* PlaySound(const UObject* WorldContext, const FPlayParams& Params);

	// The same with UnrealScript's argument order, played by Source from its
	// current location: this machine's copy of a sound the scripts play on the
	// server, so it takes the server's slots. A null Sound plays nothing.
	UAudioComponent* PlayFrom(const AActor& Source, USoundBase* Sound,
		ESlot Slot = ESlot::Misc, float Volume = 1.0f, bool bNoOverride = false,
		float Radius = 0.0f, float Pitch = 1.0f);

	// The same for a PlaySound in simulated code, which every machine runs for
	// itself: on a network client it takes the client's own slots.
	UAudioComponent* PlaySimulatedFrom(const AActor& Source, USoundBase* Sound,
		ESlot Slot = ESlot::Misc, float Volume = 1.0f, bool bNoOverride = false,
		float Radius = 0.0f, float Pitch = 1.0f);

	// A sound with no playing actor: it stays where it started and takes no
	// slot. RadiusUU 0 is the engine default.
	UAudioComponent* PlayOneShot(const UObject* WorldContext, USoundBase* Sound,
		const FVector& Location, float RadiusUU,
		float PitchMult = 1.0f, float VolumeMult = 1.0f);

	// PlayerPawn.ClientPlaySound on the controller's own machine: four voices on
	// the viewer at volume 16 x VolumeScale, heard centred. They are the
	// machine's own plays: on a network client they replace only the client's
	// own sounds in the viewer's slots, never the ones the server sent.
	void PlayClientSound(APlayerController& Controller, USoundBase* Sound,
		float VolumeScale = 1.0f);

	// WOTPlayer.uc ClientHearSound's underwater rule: unless the sound plays in
	// SLOT_Interface or SLOT_Interact, or its name contains DROWN or GASP, it
	// plays at volume x0.90, radius x0.80 and pitch x0.85 while the listener's
	// head is underwater, or while the source's head is (a pawn's eye height
	// above where the sound started; tested only for sounds that do not start
	// at the listener's own location). Radius is the raw script value, so 0
	// stays 0. Applied on each machine as it hears the sound. True when the
	// sound was modified.
	bool ApplyUnderwater(const UWorld& World, const AActor* Source,
		const FVector& SoundLocation, ESlot Slot, const USoundBase* Sound,
		float& InOutVolume, float& InOutRadiusUU, float& InOutPitch);
	// The rule for a sourceless SLOT_Misc sound identified by its wave name.
	bool ApplyUnderwaterOneShot(const UWorld* World, const FVector& SourceLocation,
		FName WaveName, float& InOutVolume, float& InOutRadiusUU, float& InOutPitch);

#if WITH_AUTOMATION_TESTS
	enum class EEffectAdmissionResult : uint8
	{
		Admitted,
		InvalidSound,
		InvalidWorld,
		NoListener,
		OutsideHearingRadius,
		PoolPriorityRejected,
	};
	// Runs the production admission path for a sourceless sound but stops
	// before taking a channel, so specs can tell each rejection apart.
	EEffectAdmissionResult PrepareEffectForTesting(
		const UObject* WorldContext, USoundBase* Sound,
		const FVector& SourceLocation, float RadiusUU,
		float PitchMult = 1.0f, float VolumeMult = 1.0f);
	// Fill a world's channels with silent, endless sourceless voices.
	int32 SeedEffectsPoolForTesting(UWorld& World,
		TConstArrayView<FEffectVoicePrioritySample> Voices);

	struct FChannelSnapshot
	{
		int32 Index = INDEX_NONE;
		FName SoundName;
		FObjectKey SourceKey;
		ESlot Slot = ESlot::None;
		// On a network client: the client's own slot, apart from the server's.
		bool bClientOwnSlot = false;
		FVector Location = FVector::ZeroVector;
		float Volume = 0.0f;
		float Radius = 0.0f;
		float Pitch = 1.0f;
		float Priority = 0.0f;
		bool bAmbient = false;
	};
	// The world's busy effects channels.
	TArray<FChannelSnapshot> GetChannelsForTesting(const UWorld& World);
#endif

	// Synchronously load an imported wave by its subpath under /Game/WoT/Sounds
	// ("AmbientA/Fire17"). Null when unset or missing.
	USoundBase* LoadBySubPath(FName SubPath, bool bLoop = false);

	// Resolve a gameplay soft reference through the authenticated cache only
	// while guarded bootstrap is active; developer builds keep cooked loading.
	USoundBase* Resolve(const TSoftObjectPtr<USoundBase>& Sound,
		bool bLoop = false);

	// Preserve the pre-runtime-provider resident-only contract outside guarded
	// builds while allowing uncooked guarded assets to resolve from the cache.
	USoundBase* ResolveResidentOrRuntime(
		const TSoftObjectPtr<USoundBase>& Sound, bool bLoop = false);
}
