#include "WoTSound.h"

#include "Assets/WoTRuntimeAssets.h"
#include "WoTZoneVolume.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Components/AudioComponent.h"
#include "Components/SceneComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/Paths.h"
#include "Curves/RichCurve.h"
#include "Sound/SoundAttenuation.h"
#include "UObject/GCObject.h"
#include "UObject/SoftObjectPath.h"

namespace WoTSound
{
float WorldSoundRadius(uint8 ByteRadius)
{
	// UE1 world sound radius in UU, derived from the byte SoundRadius.
	return 25.0f * (static_cast<float>(ByteRadius) + 1.0f);
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

struct FEffectVoice
{
	TWeakObjectPtr<UAudioComponent> Component;
	float Radius = DefaultPlaySoundRadius;
	float Volume = 1.0f;
};

struct FEffectsPool
{
	TArray<FEffectVoice> Voices;
};

TMap<TWeakObjectPtr<UWorld>, FEffectsPool>& EffectsPools()
{
	static TMap<TWeakObjectPtr<UWorld>, FEffectsPool> Pools;
	return Pools;
}

FEffectsPool& LiveEffectsPool(UWorld& World)
{
	TMap<TWeakObjectPtr<UWorld>, FEffectsPool>& Pools = EffectsPools();
	for (auto It = Pools.CreateIterator(); It; ++It)
	{
		if (!It.Key().IsValid())
		{
			It.RemoveCurrent();
		}
	}
	FEffectsPool& Pool = Pools.FindOrAdd(TWeakObjectPtr<UWorld>(&World));
	Pool.Voices.RemoveAll([](const FEffectVoice& Voice)
	{
		return !Voice.Component.IsValid();
	});
	return Pool;
}

#if WOT_WITH_BOOTSTRAP_TEST_HARNESSES
// The trailer rig's pinned hearing origins (SetHearingOriginOverride).
TMap<TWeakObjectPtr<const UWorld>, FVector>& HearingOriginOverrides()
{
	static TMap<TWeakObjectPtr<const UWorld>, FVector> Overrides;
	return Overrides;
}
#endif

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

struct FPreparedEffect
{
	UWorld* World = nullptr;
	FEffectsPool* Pool = nullptr;
	float Volume = 1.0f;
	float Radius = DefaultPlaySoundRadius;
	float Pitch = 1.0f;
	float Priority = 0.0f;
	int32 VoiceIndex = INDEX_NONE;
};

enum class EPrepareEffectResult : uint8
{
	Admitted,
	InvalidSound,
	InvalidWorld,
	NoListener,
	OutsideHearingRadius,
	PoolPriorityRejected,
};

EPrepareEffectResult PrepareEffect(const UObject* WorldContext, USoundBase& Sound,
	const FVector& SourceLocation, float RadiusUU, float PitchMult,
	float VolumeMult, FPreparedEffect& Out)
{
	UWorld* World = GEngine
		? GEngine->GetWorldFromContextObject(
			WorldContext, EGetWorldErrorMode::ReturnNull)
		: nullptr;
	if (!World || World->GetNetMode() == NM_DedicatedServer)
	{
		return EPrepareEffectResult::InvalidWorld;
	}
	FVector HearSource;
	if (!ListenerLocation(*World, HearSource))
	{
		return EPrepareEffectResult::NoListener;
	}

	Out.World = World;
	Out.Volume = VolumeMult;
	Out.Radius = RadiusUU > 0.0f ? RadiusUU : DefaultPlaySoundRadius;
	Out.Pitch = PitchMult;
	const float Distance = FVector::Distance(HearSource, SourceLocation);
	if (!PassesHearingGate(Distance, Out.Radius))
	{
		return EPrepareEffectResult::OutsideHearingRadius;
	}

	// The native hearing test precedes WOTPlayer.ClientHearSound; the latter can
	// then muffle the admitted voice before Galaxy computes its priority.
	ApplyUnderwaterOneShot(World, SourceLocation, Sound.GetFName(),
		Out.Volume, Out.Radius, Out.Pitch);
	Out.Priority = EffectsPriority(Distance, Out.Radius, Out.Volume);
	Out.Pool = &LiveEffectsPool(*World);
	TArray<FEffectVoicePrioritySample, TInlineAllocator<EffectsChannelCount>>
		ActiveVoices;
	ActiveVoices.Reserve(Out.Pool->Voices.Num());
	for (const FEffectVoice& Voice : Out.Pool->Voices)
	{
		if (const UAudioComponent* Component = Voice.Component.Get())
		{
			ActiveVoices.Add({Component->GetComponentLocation(),
				Voice.Radius, Voice.Volume});
		}
	}
	Out.VoiceIndex = ChooseEffectsVoiceForListener(
		ActiveVoices, HearSource, Out.Priority);
	return Out.VoiceIndex != INDEX_NONE
		? EPrepareEffectResult::Admitted
		: EPrepareEffectResult::PoolPriorityRejected;
}

UAudioComponent* CommitEffect(FPreparedEffect& Prepared,
	UAudioComponent* Component)
{
	if (!Component || !Prepared.Pool || Prepared.VoiceIndex == INDEX_NONE)
	{
		return nullptr;
	}
	FEffectVoice Voice{TWeakObjectPtr<UAudioComponent>(Component),
		Prepared.Radius, Prepared.Volume};
	if (Prepared.VoiceIndex < Prepared.Pool->Voices.Num())
	{
		if (UAudioComponent* Replaced =
			Prepared.Pool->Voices[Prepared.VoiceIndex].Component.Get())
		{
			Replaced->Stop();
		}
		Prepared.Pool->Voices[Prepared.VoiceIndex] = Voice;
	}
	else
	{
		Prepared.Pool->Voices.Add(Voice);
	}
	return Component;
}
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
	if (ExistingPriorities.Num() < EffectsChannelCount)
	{
		return ExistingPriorities.Num();
	}
	int32 Index = INDEX_NONE;
	float BestPriority = NewPriority;
	for (int32 VoiceIndex = 0; VoiceIndex < EffectsChannelCount; ++VoiceIndex)
	{
		if (ExistingPriorities[VoiceIndex] <= BestPriority)
		{
			Index = VoiceIndex;
			BestPriority = ExistingPriorities[VoiceIndex];
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
	FPreparedEffect Prepared;
	return static_cast<EEffectAdmissionResult>(PrepareEffect(
		WorldContext, *Sound, SourceLocation, RadiusUU,
		PitchMult, VolumeMult, Prepared));
}

int32 SeedEffectsPoolForTesting(UWorld& World,
	TConstArrayView<FEffectVoicePrioritySample> Voices)
{
	FEffectsPool& Pool = LiveEffectsPool(World);
	Pool.Voices.Reset();
	for (const FEffectVoicePrioritySample& Sample : Voices)
	{
		UAudioComponent* Component = NewObject<UAudioComponent>(&World);
		Component->SetWorldLocation(Sample.SourceLocation);
		Pool.Voices.Add({TWeakObjectPtr<UAudioComponent>(Component),
			Sample.RadiusUU, Sample.Volume});
	}
	return Pool.Voices.Num();
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
	if (bClamp)
	{
		// UE1's Galaxy driver: GlxVolume = Clamp(MAX * Volume * (1 - d/R), 0, MAX).
		// With the play's Volume multiplier V > 1 the attenuation must be
		// min(1/V, 1 - x) over x = d/R: flat at 1/V out to x = 1 - 1/V, then the
		// ordinary linear ramp to zero at the radius. Linear keys reproduce it
		// exactly; UE evaluates the curve at d/FalloffDistance and holds the last
		// key (0) beyond it (Attenuation.cpp AttenuationEval, Custom).
		const float InvVolume = 1000.0f / static_cast<float>(VolumeKey);
		S.DistanceAlgorithm = EAttenuationDistanceModel::Custom;
		FRichCurve& Curve = *S.CustomAttenuationCurve.GetRichCurve();
		Curve.Reset();
		const FKeyHandle A = Curve.AddKey(0.0f, InvVolume);
		const FKeyHandle B = Curve.AddKey(1.0f - InvVolume, InvVolume);
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

bool ApplyUnderwaterOneShot(const UWorld* World, const FVector& SourceLocation,
                            FName WaveName, float& InOutVolume,
                            float& InOutRadiusUU, float& InOutPitch)
{
	if (!World || World->GetNetMode() == NM_DedicatedServer)
	{
		return false;
	}
	// WOTPlayer.uc:1425-1428: DROWN/GASP waves are exempt by NAME substring.
	const FString Wave = WaveName.ToString().ToUpper();
	if (Wave.Contains(TEXT("DROWN")) || Wave.Contains(TEXT("GASP")))
	{
		return false;
	}
	bool bListenerUnder = false;
	if (const APlayerController* PC = World->GetFirstPlayerController())
	{
		if (const APawn* Pawn = PC->GetPawn())
		{
			bListenerUnder = AWoTZoneVolume::IsPointInWater(
			    World, Pawn->GetActorLocation() +
			               FVector(0.0f, 0.0f, Pawn->BaseEyeHeight));
		}
	}
	if (!bListenerUnder && !AWoTZoneVolume::IsPointInWater(World, SourceLocation))
	{
		return false;
	}
	// WOTPlayer.uc:1430-1436 — the shipped one-shot triple (pitch x0.85; the
	// ambient-channel engine path is x0.77, WoTAmbientSound).
	InOutVolume *= 0.90f;
	InOutRadiusUU *= 0.80f;
	InOutPitch *= 0.85f;
	return true;
}

UAudioComponent* PlayOneShot(const UObject* WorldContext, USoundBase* Sound,
                 const FVector& Location, float RadiusUU,
                 float PitchMult, float VolumeMult)
{
	if (!Sound)
	{
		return nullptr;
	}
	FPreparedEffect Prepared;
	if (PrepareEffect(WorldContext, *Sound, Location, RadiusUU, PitchMult,
		VolumeMult, Prepared) != EPrepareEffectResult::Admitted)
	{
		return nullptr;
	}
	UAudioComponent* Component = UGameplayStatics::SpawnSoundAtLocation(
		WorldContext, Sound, Location, FRotator::ZeroRotator, Prepared.Volume,
		Prepared.Pitch, 0.0f,
		LinearAttenuation(Prepared.Radius, Prepared.Volume));
	return CommitEffect(Prepared, Component);
}

UAudioComponent* PlayAttachedOneShot(const UObject* WorldContext,
	USoundBase* Sound, USceneComponent* AttachTo,
	const FVector& SourceLocation, float RadiusUU,
	float PitchMult, float VolumeMult)
{
	if (!Sound || !AttachTo)
	{
		return nullptr;
	}
	FPreparedEffect Prepared;
	if (PrepareEffect(WorldContext, *Sound, SourceLocation, RadiusUU,
		PitchMult, VolumeMult, Prepared) != EPrepareEffectResult::Admitted)
	{
		return nullptr;
	}
	UAudioComponent* Component = UGameplayStatics::SpawnSoundAttached(
		Sound, AttachTo, NAME_None, FVector::ZeroVector,
		EAttachLocation::SnapToTarget,
		/*bStopWhenAttachedToDestroyed*/ true, Prepared.Volume, Prepared.Pitch,
		0.0f, LinearAttenuation(Prepared.Radius, Prepared.Volume));
	return CommitEffect(Prepared, Component);
}

void StopEffectVoice(UAudioComponent* Component)
{
	if (!Component)
	{
		return;
	}
	Component->Stop();
	for (TPair<TWeakObjectPtr<UWorld>, FEffectsPool>& Pair : EffectsPools())
	{
		Pair.Value.Voices.RemoveAll([Component](const FEffectVoice& Voice)
		{
			return Voice.Component.Get() == Component;
		});
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
