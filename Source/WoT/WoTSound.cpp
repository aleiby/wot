#include "WoTSound.h"

#include "Assets/WoTRuntimeAssets.h"
#include "WoTSoundChannels.h"
#include "WoTZoneVolume.h"

#include "AudioDefines.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Components/AudioComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Misc/Paths.h"
#include "Curves/RichCurve.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundBase.h"
#include "UObject/GCObject.h"
#include "UObject/SoftObjectPath.h"

namespace WoTSound
{
float WorldSoundRadius(uint8 ByteRadius)
{
	// UE1 world sound radius in UU, derived from the byte SoundRadius.
	return 25.0f * (static_cast<float>(ByteRadius) + 1.0f);
}

float VoiceVolume(float Volume)
{
	// The mixer clamps a voice's gain before its distance attenuation to
	// MAX_VOLUME (four times full scale).
	return FMath::Min(Volume, MAX_VOLUME);
}

namespace
{
// Engine-lifetime holder for the attenuation cache. FGCObject keeps the entries
// alive across GC without rooting; unlike a function-local static map of
// TStrongObjectPtr (whose release path calls into the UObject system during
// static destruction, after UObject shutdown), this tears down safely at exit:
// ~FGCObject no-ops once the referencer dies in the exit purge, and destroying
// a TMap of TObjectPtr never touches UObjects.
class FAttenuationCache final : public FGCObject
{
public:
	static FAttenuationCache& Get()
	{
		static FAttenuationCache Singleton;
		return Singleton;
	}

	TMap<int32, TObjectPtr<USoundAttenuation>> ByRadius;
	// Volume > 1 (the UE1 per-voice clamp curve): keyed by (radius, volume in
	// thousandths).
	TMap<TPair<int32, int32>, TObjectPtr<USoundAttenuation>> ByRadiusVolume;

	virtual void AddReferencedObjects(FReferenceCollector& Collector) override
	{
		Collector.AddReferencedObjects(ByRadius);
		Collector.AddReferencedObjects(ByRadiusVolume);
	}

	virtual FString GetReferencerName() const override
	{
		return TEXT("WoTSound::FAttenuationCache");
	}
};

#if WOT_WITH_BOOTSTRAP_TEST_HARNESSES
// The trailer rig's pinned hearing origins (SetHearingOriginOverride).
TMap<TWeakObjectPtr<const UWorld>, FVector>& HearingOriginOverrides()
{
	static TMap<TWeakObjectPtr<const UWorld>, FVector> Overrides;
	return Overrides;
}
#endif

UWorld* SoundWorld(const UObject* WorldContext)
{
	return GEngine
		? GEngine->GetWorldFromContextObject(WorldContext, EGetWorldErrorMode::ReturnNull)
		: nullptr;
}
}

bool ListenerLocation(const UWorld& World, FVector& OutLocation)
{
#if WOT_WITH_BOOTSTRAP_TEST_HARNESSES
	if (const FVector* Pinned =
		HearingOriginOverrides().Find(TWeakObjectPtr<const UWorld>(&World)))
	{
		OutLocation = *Pinned;
		return true;
	}
#endif
	const APlayerController* PC = World.GetFirstPlayerController();
	if (!PC)
	{
		return false;
	}
	if (const AActor* ViewTarget = PC->GetViewTarget())
	{
		OutLocation = ViewTarget->GetActorLocation();
	}
	else
	{
		OutLocation = PC->GetFocalLocation();
	}
	return true;
}

#if WOT_WITH_BOOTSTRAP_TEST_HARNESSES
void SetHearingOriginOverride(const UWorld& World, const FVector& Location)
{
	TMap<TWeakObjectPtr<const UWorld>, FVector>& Overrides = HearingOriginOverrides();
	for (auto It = Overrides.CreateIterator(); It; ++It)
	{
		if (!It.Key().IsValid())
		{
			It.RemoveCurrent();
		}
	}
	Overrides.Add(TWeakObjectPtr<const UWorld>(&World), Location);
}

void ClearHearingOriginOverride(const UWorld& World)
{
	HearingOriginOverrides().Remove(TWeakObjectPtr<const UWorld>(&World));
}
#endif

bool PassesHearingGate(float DistanceUU, float RadiusUU)
{
	return DistanceUU >= 0.0f && RadiusUU > 0.0f
		&& DistanceUU < RadiusUU / FMath::Sqrt(1.3f);
}

float EffectsPriority(float DistanceUU, float RadiusUU, float Volume)
{
	return RadiusUU > 0.0f
		? Volume * (1.0f - DistanceUU / RadiusUU)
		: 0.0f;
}

int32 ChooseEffectsVoice(TConstArrayView<float> ExistingPriorities,
	float NewPriority)
{
	int32 Index = INDEX_NONE;
	float BestPriority = NewPriority;
	for (int32 VoiceIndex = 0; VoiceIndex < EffectsChannelCount; ++VoiceIndex)
	{
		const float Priority = ExistingPriorities.IsValidIndex(VoiceIndex)
			? ExistingPriorities[VoiceIndex] : 0.0f;
		if (Priority <= BestPriority)
		{
			Index = VoiceIndex;
			BestPriority = Priority;
		}
	}
	return Index;
}

int32 ChooseEffectsVoiceForListener(
	TConstArrayView<FEffectVoicePrioritySample> ExistingVoices,
	const FVector& ListenerLocationValue, float NewPriority)
{
	TArray<float, TInlineAllocator<EffectsChannelCount>> Priorities;
	Priorities.Reserve(ExistingVoices.Num());
	for (const FEffectVoicePrioritySample& Voice : ExistingVoices)
	{
		Priorities.Add(EffectsPriority(
			FVector::Distance(ListenerLocationValue, Voice.SourceLocation),
			Voice.RadiusUU, Voice.Volume));
	}
	return ChooseEffectsVoice(Priorities, NewPriority);
}

float DopplerFactor(const FVector& SourceVelocity, const FVector& SourceLocation,
	const FVector& ListenerLocation)
{
	// Positive radial speed moves the source away and lowers the pitch.
	const float RadialSpeed = FVector::DotProduct(SourceVelocity,
		(SourceLocation - ListenerLocation).GetSafeNormal());
	return FMath::Clamp(1.0f - RadialSpeed / DopplerSpeed, 0.5f, 2.0f);
}

#if WITH_AUTOMATION_TESTS
EEffectAdmissionResult PrepareEffectForTesting(
	const UObject* WorldContext, USoundBase* Sound,
	const FVector& SourceLocation, float RadiusUU,
	float PitchMult, float VolumeMult)
{
	if (!Sound)
	{
		return EEffectAdmissionResult::InvalidSound;
	}
	UWorld* World = SoundWorld(WorldContext);
	UWoTSoundChannels* Channels = UWoTSoundChannels::Get(World);
	if (!World || !Channels || World->GetNetMode() == NM_DedicatedServer)
	{
		return EEffectAdmissionResult::InvalidWorld;
	}
	FPlayParams Params;
	Params.Sound = Sound;
	Params.Location = SourceLocation;
	Params.Radius = RadiusUU;
	Params.Pitch = PitchMult;
	Params.Volume = VolumeMult;
	return Channels->PrepareForTesting(Params);
}

int32 SeedEffectsPoolForTesting(UWorld& World,
	TConstArrayView<FEffectVoicePrioritySample> Voices)
{
	UWoTSoundChannels* Channels = UWoTSoundChannels::Get(&World);
	return Channels ? Channels->SeedForTesting(Voices) : 0;
}

TArray<FChannelSnapshot> GetChannelsForTesting(const UWorld& World)
{
	const UWoTSoundChannels* Channels = UWoTSoundChannels::Get(&World);
	return Channels ? Channels->SnapshotForTesting() : TArray<FChannelSnapshot>();
}
#endif

float NormalizedUISoundVoiceGain(float RetailVolume)
{
	return FMath::Clamp(RetailVolume, 0.0f, 1.0f);
}

float StackedUISoundEffectiveGain(float RetailVolume)
{
	return static_cast<float>(ClientUISoundVoiceCount)
		* NormalizedUISoundVoiceGain(RetailVolume);
}

USoundAttenuation* LinearAttenuation(float RadiusUU, float Volume)
{
	if (RadiusUU <= 0.0f)
	{
		return nullptr;
	}
	// Cache one attenuation per distinct (rounded) radius, so plays reuse rather
	// than re-allocate. Bounded (byte SoundRadius -> at most 256 distinct radii
	// plus a handful of hand-passed ones), so engine lifetime is fine. The clamp
	// curves add one entry per distinct (radius, >1 volume in thousandths) pair:
	// the ambient byte volumes give a few dozen; the randomized footstep/landing
	// volumes (FRandRange 0.5-1.5 x row x pawn scalars) can add a few thousand
	// small objects over a long session at most - bounded by that quantization.
	FAttenuationCache& Caches = FAttenuationCache::Get();
	const int32 Key = FMath::RoundToInt(RadiusUU);
	const int32 VolumeKey = FMath::RoundToInt(Volume * 1000.0f);
	const bool bClamp = VolumeKey > 1000;
	TObjectPtr<USoundAttenuation>* Found = bClamp
		? Caches.ByRadiusVolume.Find(TPair<int32, int32>(Key, VolumeKey))
		: Caches.ByRadius.Find(Key);
	if (Found)
	{
		// GC nulls a destroyed entry rather than erasing it; rebuild in that case.
		if (USoundAttenuation* Live = *Found)
		{
			return Live;
		}
	}

	USoundAttenuation* Att = NewObject<USoundAttenuation>(GetTransientPackage());
	FSoundAttenuationSettings& S = Att->Attenuation;
	S.bAttenuate = true;
	S.bSpatialize = true;
	S.AttenuationShape = EAttenuationShape::Sphere;
	S.AttenuationShapeExtents = FVector::ZeroVector;             // inner radius 0
	S.FalloffDistance = static_cast<float>(Key);
	S.DistanceAlgorithm = EAttenuationDistanceModel::Linear;     // UE1 linear falloff
	// Close-range centring: the original scaled a sound's pan toward the
	// middle in proportion to its distance inside a tenth of its radius. UE
	// blends the panned speaker gains linearly toward the centred 2D map over
	// the same span (from Start down to End, which must be above 0 to keep the
	// blend linear) - the same transition, under UE's pan law.
	S.NonSpatializedRadiusStart = CentringRadiusFraction * static_cast<float>(Key);
	S.NonSpatializedRadiusEnd = FMath::Min(0.01f, S.NonSpatializedRadiusStart);
	S.NonSpatializedRadiusMode = ENonSpatializedRadiusSpeakerMapMode::Direct2D;
	if (bClamp)
	{
		// The original's voice gain: Clamp(Volume * (1 - d/R), 0, 1) of full
		// scale. The voice plays at VoiceVolume(V) (V itself up to the engine's
		// 4x cap), so over x = d/R the attenuation must be
		// min(1, V * (1 - x)) / VoiceVolume(V): flat at 1/VoiceVolume(V) out to
		// x = 1 - 1/V, then linear to zero at the radius. Below the cap that is
		// min(1/V, 1 - x). Linear keys reproduce it exactly; UE evaluates the
		// curve at d/FalloffDistance and holds the last key (0) beyond it.
		const float ClampVolume = static_cast<float>(VolumeKey) / 1000.0f;
		const float Flat = 1.0f / VoiceVolume(ClampVolume);
		const float Knee = 1.0f - 1.0f / ClampVolume;
		S.DistanceAlgorithm = EAttenuationDistanceModel::Custom;
		FRichCurve& Curve = *S.CustomAttenuationCurve.GetRichCurve();
		Curve.Reset();
		const FKeyHandle A = Curve.AddKey(0.0f, Flat);
		const FKeyHandle B = Curve.AddKey(Knee, Flat);
		const FKeyHandle C = Curve.AddKey(1.0f, 0.0f);
		for (const FKeyHandle Handle : {A, B, C})
		{
			Curve.SetKeyInterpMode(Handle, RCIM_Linear);
		}
		Curve.PreInfinityExtrap = RCCE_Constant;
		Curve.PostInfinityExtrap = RCCE_Constant;
		Caches.ByRadiusVolume.Add(TPair<int32, int32>(Key, VolumeKey), Att);
	}
	else
	{
		Caches.ByRadius.Add(Key, Att);
	}
	return Att;
}

FName SubPathOf(const TSoftObjectPtr<USoundBase>& Sound)
{
	if (Sound.IsNull())
	{
		return NAME_None;
	}
	FString Package = Sound.ToSoftObjectPath().GetLongPackageName();
	if (!Package.RemoveFromStart(TEXT("/Game/WoT/Sounds/")) || Package.IsEmpty())
	{
		return NAME_None;
	}
	return FName(*Package);
}

bool ApplyUnderwater(const UWorld& World, const AActor* Source,
	const FVector& SoundLocation, ESlot Slot, const USoundBase* Sound,
	float& InOutVolume, float& InOutRadiusUU, float& InOutPitch)
{
	if (World.GetNetMode() == NM_DedicatedServer
		|| Slot == ESlot::Interface || Slot == ESlot::Interact)
	{
		return false;
	}
	// Drowning and gasping sounds already sound underwater (matched by name).
	const FString Name = Sound ? Sound->GetName().ToUpper() : FString(TEXT("NONE"));
	if (Name.Contains(TEXT("DROWN")) || Name.Contains(TEXT("GASP")))
	{
		return false;
	}
	const APlayerController* PC = World.GetFirstPlayerController();
	const APawn* Hearer = PC ? PC->GetPawn() : nullptr;
	bool bUnderwater = Hearer && AWoTZoneVolume::IsPointInWater(&World,
		Hearer->GetActorLocation() + FVector(0.0f, 0.0f, Hearer->BaseEyeHeight));
	if (!bUnderwater && !(Hearer && SoundLocation == Hearer->GetActorLocation()))
	{
		// The source's head: a pawn's eye height above where the sound started.
		FVector Head = SoundLocation;
		if (const APawn* Pawn = Cast<APawn>(Source))
		{
			Head.Z += Pawn->BaseEyeHeight;
		}
		bUnderwater = AWoTZoneVolume::IsPointInWater(&World, Head);
	}
	if (!bUnderwater)
	{
		return false;
	}
	// The one-shot triple (ambient loops take x0.77 pitch, WoTAmbientSound).
	InOutVolume *= 0.90f;
	InOutRadiusUU *= 0.80f;
	InOutPitch *= 0.85f;
	return true;
}

bool ApplyUnderwaterOneShot(const UWorld* World, const FVector& SourceLocation,
	FName WaveName, float& InOutVolume, float& InOutRadiusUU, float& InOutPitch)
{
	if (!World || World->GetNetMode() == NM_DedicatedServer)
	{
		return false;
	}
	const FString Wave = WaveName.ToString().ToUpper();
	if (Wave.Contains(TEXT("DROWN")) || Wave.Contains(TEXT("GASP")))
	{
		return false;
	}
	// A named wave stands in for the sound; the rest of the rule is shared.
	return ApplyUnderwater(*World, nullptr, SourceLocation, ESlot::Misc, nullptr,
		InOutVolume, InOutRadiusUU, InOutPitch);
}

UAudioComponent* PlaySound(const UObject* WorldContext, const FPlayParams& Params)
{
	UWoTSoundChannels* Channels = UWoTSoundChannels::Get(SoundWorld(WorldContext));
	return Channels ? Channels->Play(Params) : nullptr;
}

namespace
{
UAudioComponent* PlayFromAs(ECaller Caller, const AActor& Source, USoundBase* Sound,
	ESlot Slot, float Volume, bool bNoOverride, float Radius, float Pitch)
{
	// A wave that failed to resolve plays nothing (it never stops the slot).
	if (!Sound)
	{
		return nullptr;
	}
	FPlayParams Params;
	Params.Sound = Sound;
	Params.Source = &Source;
	Params.Location = Source.GetActorLocation();
	Params.Slot = Slot;
	Params.bNoOverride = bNoOverride;
	Params.Volume = Volume;
	Params.Radius = Radius;
	Params.Pitch = Pitch;
	Params.Caller = Caller;
	return PlaySound(&Source, Params);
}
}

UAudioComponent* PlayFrom(const AActor& Source, USoundBase* Sound, ESlot Slot,
	float Volume, bool bNoOverride, float Radius, float Pitch)
{
	return PlayFromAs(ECaller::Server, Source, Sound, Slot, Volume, bNoOverride,
		Radius, Pitch);
}

UAudioComponent* PlaySimulatedFrom(const AActor& Source, USoundBase* Sound, ESlot Slot,
	float Volume, bool bNoOverride, float Radius, float Pitch)
{
	return PlayFromAs(ECaller::Local, Source, Sound, Slot, Volume, bNoOverride,
		Radius, Pitch);
}

UAudioComponent* PlayOneShot(const UObject* WorldContext, USoundBase* Sound,
	const FVector& Location, float RadiusUU, float PitchMult, float VolumeMult)
{
	if (!Sound)
	{
		return nullptr;
	}
	FPlayParams Params;
	Params.Sound = Sound;
	Params.Location = Location;
	Params.Radius = RadiusUU;
	Params.Pitch = PitchMult;
	Params.Volume = VolumeMult;
	return PlaySound(WorldContext, Params);
}

void PlayClientSound(APlayerController& Controller, USoundBase* Sound,
	float VolumeScale)
{
	UWoTSoundChannels* Channels = UWoTSoundChannels::Get(Controller.GetWorld());
	if (!Sound || !Channels)
	{
		return;
	}
	// The viewer plays it: a spectated view target at the default radius, or
	// the player itself with a radius of a million units.
	AActor* Self = Controller.GetPawn();
	if (!Self)
	{
		Self = &Controller;
	}
	AActor* ViewTarget = Controller.GetViewTarget();
	const bool bViewingOther = ViewTarget && ViewTarget != Self && ViewTarget != &Controller;
	AActor* Speaker = bViewingOther ? ViewTarget : Self;
	FPlayParams Params;
	Params.Sound = Sound;
	Params.Source = Speaker;
	Params.Location = Speaker->GetActorLocation();
	Params.Volume = ClientSoundVolume * VolumeScale;
	Params.Radius = bViewingOther ? 0.0f : 1000000.0f;
	// A client function: on a network client the copies take the client's
	// own slots and leave the viewer's server-sent sounds playing.
	Params.Caller = ECaller::Local;
	for (const ESlot Slot : {ESlot::None, ESlot::Interface, ESlot::Misc, ESlot::Talk})
	{
		Params.Slot = Slot;
		Channels->PlayInterface(Params);
	}
}

USoundBase* LoadBySubPath(FName SubPath, bool bLoop)
{
	if (SubPath.IsNone())
	{
		return nullptr;
	}
	const FString Sub = SubPath.ToString();
	const FString Leaf = FPaths::GetCleanFilename(Sub);
	return UWoTRuntimeAssets::ResolveSoundPath(FSoftObjectPath(
		FString::Printf(TEXT("/Game/WoT/Sounds/%s.%s"), *Sub, *Leaf)), bLoop);
}

USoundBase* Resolve(const TSoftObjectPtr<USoundBase>& Sound, bool bLoop)
{
	return Sound.IsNull() ? nullptr
		: UWoTRuntimeAssets::ResolveSoundPath(Sound.ToSoftObjectPath(), bLoop);
}

USoundBase* ResolveResidentOrRuntime(
	const TSoftObjectPtr<USoundBase>& Sound, bool bLoop)
{
#if WOT_GUARDED_BOOTSTRAP
	return Resolve(Sound, bLoop);
#else
	(void)bLoop;
	return Sound.Get();
#endif
}
}
