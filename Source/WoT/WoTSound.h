#pragma once

#include "CoreMinimal.h"

class USoundBase;
class USoundAttenuation;
class UAudioComponent;
class USceneComponent;
class UWorld;

// UE1 audio model, ported for the artifact/projectile SFX. The 1999 engine
// attenuated every positional sound LINEARLY to zero at a world radius
// (attenuation = Clamp(1 - dist/radius, 0, 1)); one-shot
// PlaySound calls pass that radius (world units) + a float pitch directly, while
// looping AmbientSounds derive it from the byte SoundRadius/Volume/Pitch
// (world radius = 25*(SoundRadius+1); volume/255; pitch/64).
// Presentation layer — audio falloff is not simulation-locked, but
// reproducing it keeps the mix faithful and stops every sound playing at full
// volume everywhere (UE5's no-attenuation default).
namespace WoTSound
{
	struct FEffectVoicePrioritySample
	{
		FVector SourceLocation = FVector::ZeroVector;
		float RadiusUU = 0.0f;
		float Volume = 0.0f;
	};

	// original/System/{Default,WoT}.ini: EffectsChannels=16. Positional
	// PlaySound voices share this pool; music, UI, and ambient audio do not.
	inline constexpr int32 EffectsChannelCount = 16;

	// PlayerPawn.ClientPlaySound overlaps SLOT_None/Interface/Misc/Talk. UE1's
	// volume 16 is already full scale after Galaxy's per-voice clamp, so UE5 gets
	// four voices at a normalized [0,1] gain rather than four 16x voices.
	inline constexpr int32 ClientUISoundVoiceCount = 4;

	// Retail ambient gain (original/System/WoT.ini:159 AmbientFactor=0.7): the UE1
	// audio driver scaled every SLOT_Ambient loop by it —
	// one-shots are NOT affected.
	inline constexpr float AmbientFactor = 0.7f;

	// UE1 Actor.PlaySound with no Radius argument: TransientSoundRadius ships unset
	// (only PixieSprayer overrides it), and the engine substitutes 1600 on both
	// the server gate and the client play.
	inline constexpr float DefaultPlaySoundRadius = 1600.0f;

	// UE1 world sound radius (byte SoundRadius -> world units).
	float WorldSoundRadius(uint8 ByteRadius);

	// A cached UE1-falloff attenuation reaching zero at RadiusUU. Returns null for
	// RadiusUU <= 0 (no attenuation). Objects are cached per rounded radius (and
	// clamp volume) and kept alive against GC, so repeated plays don't re-allocate.
	//
	// Volume is the play's volume multiplier. UE1 gave every voice
	// Clamp(Volume * (1 - dist/Radius), 0, 1) of the sample's full scale
	// (the Galaxy driver's GlxVolume, clamped to GLX_MAXSMPVOLUME); UE5
	// lets a source run to 4x (MAX_VOLUME). For Volume <= 1 the plain linear model
	// is exact; for Volume > 1 the curve is min(1/Volume, 1 - d/R), so with the
	// component's Volume multiplier the product is exactly min(1, Volume*(1-d/R)).
	USoundAttenuation* LinearAttenuation(float RadiusUU, float Volume = 1.0f);

	// UE1 Actor.PlaySound hearing/admission math. The comparison is deliberately
	// strict, and Galaxy considers the last equal-lowest
	// channel when a 17th effect arrives.
	bool PassesHearingGate(float DistanceUU, float RadiusUU);
	float EffectsPriority(float DistanceUU, float RadiusUU, float Volume);
	int32 ChooseEffectsVoice(TConstArrayView<float> ExistingPriorities,
		float NewPriority);
	// Re-evaluate active voices at the current listener/source locations before
	// applying Galaxy's 17th-voice stealing rule.
	int32 ChooseEffectsVoiceForListener(
		TConstArrayView<FEffectVoicePrioritySample> ExistingVoices,
		const FVector& ListenerLocation, float NewPriority);
#if WOT_WITH_BOOTSTRAP_TEST_HARNESSES
	// Trailer rig only (its `listener` command): pin one world's hearing-gate
	// origin, which is otherwise the first local player's view target. The rig
	// pins the UE audio listener near the action while its free camera films from
	// across a hall; the gate, still measured from the camera, would cull the very
	// sounds the pinned listener is placed to hear. Unpinned = retail behaviour.
	void SetHearingOriginOverride(const UWorld& World, const FVector& Location);
	void ClearHearingOriginOverride(const UWorld& World);
#endif
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
	// Runs the production prepare/admission path but deliberately stops before
	// audio spawning, so -NoSound specs can distinguish each rejection reason.
	EEffectAdmissionResult PrepareEffectForTesting(
		const UObject* WorldContext, USoundBase* Sound,
		const FVector& SourceLocation, float RadiusUU,
		float PitchMult = 1.0f, float VolumeMult = 1.0f);
	// Populate a world's live effects pool without requiring an audio device.
	// Headless automation runs use -NoSound, under which SpawnSoundAtLocation
	// deliberately returns null and cannot create the full-pool fixture.
	int32 SeedEffectsPoolForTesting(UWorld& World,
		TConstArrayView<FEffectVoicePrioritySample> Voices);
#endif
	float NormalizedUISoundVoiceGain(float RetailVolume);
	float StackedUISoundEffectiveGain(float RetailVolume);

	// "Angreal/RespawnA" for /Game/WoT/Sounds/Angreal/RespawnA.RespawnA (the
	// PlayReplicated key); None for a null or out-of-tree reference.
	FName SubPathOf(const TSoftObjectPtr<USoundBase>& Sound);

	// Fire-and-forget positional one-shot with a UE1 world radius + float pitch
	// (1.0 = normal) + volume multiplier (1.0 = full).
	UAudioComponent* PlayOneShot(const UObject* WorldContext, USoundBase* Sound,
	                 const FVector& Location, float RadiusUU,
	                 float PitchMult = 1.0f, float VolumeMult = 1.0f);

	// SLOT_Misc's attached variant shares the same hearing gate and effects pool.
	// StopEffectVoice removes a replaced owner-slot component from that pool.
	UAudioComponent* PlayAttachedOneShot(const UObject* WorldContext,
		USoundBase* Sound, USceneComponent* AttachTo,
		const FVector& SourceLocation, float RadiusUU,
		float PitchMult = 1.0f, float VolumeMult = 1.0f);
	void StopEffectVoice(UAudioComponent* Component);

	// WOTPlayer.ClientHearSound (WOTPlayer.uc:1394-1439): a non-ambient positional
	// sound heard while the local player's head is underwater — or originating
	// underwater — plays at volume x0.90, radius x0.80, pitch x0.85; DROWN/GASP
	// waves are exempt (designed to sound underwater). UI sounds never pass through
	// here (ClientPlayUISound is PlaySound2D — the SLOT_Interface/Interact
	// exemption). Applied per-machine inside PlayOneShot, like the original's
	// client-side hook. True when the modification applied (the spec observable).
	bool ApplyUnderwaterOneShot(const UWorld* World, const FVector& SourceLocation,
	                            FName WaveName, float& InOutVolume,
	                            float& InOutRadiusUU, float& InOutPitch);

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
