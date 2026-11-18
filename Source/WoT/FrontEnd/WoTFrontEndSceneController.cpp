#include "WoTFrontEndSceneController.h"

#include "Components/AudioComponent.h"
#include "Components/PointLightComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/Texture2D.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "TextureResource.h"
#include <atomic>

CSV_DEFINE_CATEGORY(WoTEntryWeather, false);

struct FWoTEntryAtlasRetirement
{
	std::atomic<int32> InFlight{0};
};

namespace
{
bool UploadEntryAtlas(TSharedRef<const FWoTEntryAtlasUpload, ESPMode::ThreadSafe> Job,
	TFunction<void()> Completion)
{
	check(IsInGameThread());
	UTexture2D* Texture = Job->Texture.Get();
	// These are UpdateTextureRegions' no-cleanup paths in the pinned engine.
	// The scene owns resource/configuration stability until cleanup completes.
	if (!Texture || !Texture->NeverStream || Texture->IsCurrentlyVirtualTextured()
		|| Texture->IsStreamable() || !Texture->GetResource()
		|| !Texture->GetResource()->GetTexture2DResource()) return false;
	Texture->UpdateTextureRegions(0, 1, &Job->Region, Job->Region.Width * 4, 4,
		// Engine API is non-const but reads these immutable source bytes only.
		const_cast<uint8*>(Job->Bytes.GetData()),
		[Job, Completion = MoveTemp(Completion)](uint8*, const FUpdateTextureRegion2D*)
		{
			Completion();
		});
	return true;
}
}

FWoTFrontEndSceneController::FWoTFrontEndSceneController(
	bool bInReducedMotion, float InAudioVolume, FWoTEntryAtlasUploader InUploader)
	: AtlasUploader(MoveTemp(InUploader))
	, Retirement(MakeShared<FWoTEntryAtlasRetirement, ESPMode::ThreadSafe>())
	, AudioVolume(InAudioVolume > 0.0f ? 1.0f : 0.0f)
	, bReducedMotion(bInReducedMotion)
{
	if (!AtlasUploader) AtlasUploader = UploadEntryAtlas;
}

FWoTEntryAtlasUpload::FWoTEntryAtlasUpload(UTexture2D* InTexture,
	uint32 Width, uint32 Height, TArray<uint8>&& InBytes, uint64 InGeneration)
	: Texture(InTexture), Bytes(MoveTemp(InBytes)), Region(0, 0, 0, 0, Width, Height)
	, Generation(InGeneration)
{
	check(IsInGameThread());
}

void FWoTFrontEndSceneController::AddSkyMaterial(UMaterialInstanceDynamic* Material)
{
	if (Material)
	{
		Material->SetScalarParameterValue(TEXT("WoTSceneTime"), WaterTime);
		SkyMaterials.Add(Material);
	}
}

bool FWoTFrontEndSceneController::ConfigureEntryLightning(
	TSharedRef<const FWoTBSPPayloadData> Base, UTexture2D* Atlas,
	const FWoTEntryStrikeSettings& Settings, FString& Error)
{
	if (!IsInGameThread() || bPrepared || bRunning || bStopped || !Atlas
		|| !Atlas->NeverStream || Atlas->IsCurrentlyVirtualTextured()
		|| Atlas->IsStreamable() || Atlas->GetPixelFormat() != PF_B8G8R8A8
		|| Atlas->GetSizeX() != static_cast<int32>(Base->LightmapWidth)
		|| Atlas->GetSizeY() != static_cast<int32>(Base->LightmapHeight)
		|| Settings.LightningLengths.Num() != Base->EntryLights.Num())
	{
		Error = TEXT("Invalid entry atlas binding or controller lifecycle");
		return false;
	}
	FWoTEntryStrikeState Validated;
	if (!Validated.Initialize(Settings, Error)) return false;
	TArray<float> Zero;
	Zero.Init(0.f, Settings.LightningLengths.Num());
	TArray<uint8> CheckedBase;
	if (!WoTComposeEntryLightning(*Base, Zero, CheckedBase, Error)) return false;
	EntryBase = Base;
	EntryAtlas.Reset(Atlas);
	EntryStrike = MoveTemp(Validated);
	LastSamples = MoveTemp(Zero);
	MinStrikeInterval = Settings.MinInterval;
	MaxStrikeInterval = Settings.MaxInterval;
	MinThunderDelay = Settings.MinThunderDelay;
	MaxThunderDelay = Settings.MaxThunderDelay;
	Error.Reset();
	return true;
}

#if WITH_AUTOMATION_TESTS
int32 FWoTFrontEndSceneController::UploadsInFlightForTesting() const
{
	return Retirement->InFlight.load();
}
#endif

FWoTFrontEndSceneController::~FWoTFrontEndSceneController()
{
	Stop();
}

void FWoTFrontEndSceneController::AddAmbient(
	UAudioComponent* Component, float AuthoredVolume)
{
	if (Component != nullptr) Ambient.Add({Component, AuthoredVolume});
}

void FWoTFrontEndSceneController::AddThunderEndpoint(
	UAudioComponent* Component, float AuthoredVolume)
{
	if (Component != nullptr) Thunder.Add({Component, AuthoredVolume});
}

void FWoTFrontEndSceneController::AddThunderWave(USoundBase* Wave)
{
	if (Wave != nullptr) ThunderWaves.Add(Wave);
}

void FWoTFrontEndSceneController::AddLightningLight(
	UPointLightComponent* Component)
{
	if (Component != nullptr) LightningLights.Add(Component);
}

void FWoTFrontEndSceneController::ApplyInitialLightningGate()
{
	for (const TWeakObjectPtr<UPointLightComponent>& Weak : LightningLights)
		if (UPointLightComponent* Component = Weak.Get())
			Component->SetVisibility(false, true);
}

void FWoTFrontEndSceneController::ConfigureLightningTiming(
	float MinStrikeSeconds, float MaxStrikeSeconds,
	float MinThunderSeconds, float MaxThunderSeconds)
{
	MinStrikeInterval = FMath::Max(0.05f, MinStrikeSeconds);
	MaxStrikeInterval = FMath::Max(MinStrikeInterval, MaxStrikeSeconds);
	MinThunderDelay = FMath::Max(0.0f, MinThunderSeconds);
	MaxThunderDelay = FMath::Max(MinThunderDelay, MaxThunderSeconds);
	LightningCountdown = Random.FRandRange(
		MinStrikeInterval, MaxStrikeInterval);
}

void FWoTFrontEndSceneController::AddWaterMaterial(
	UMaterialInstanceDynamic* Material)
{
	if (Material != nullptr) WaterMaterials.Add(Material);
}

bool FWoTFrontEndSceneController::Prepare()
{
	if (bPrepared || bRunning || bStopped || !IsInGameThread()) return false;
	TickHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateSP(AsShared(), &FWoTFrontEndSceneController::Tick));
	if (!TickHandle.IsValid()) return false;
	bPrepared = true;
	return true;
}

void FWoTFrontEndSceneController::Publish()
{
	check(IsInGameThread() && bPrepared && !bRunning && !bStopped && TickHandle.IsValid());
	WaterTime = 0;
	for (const auto& Weak : WaterMaterials)
		if (auto* Material = Weak.Get()) Material->SetScalarParameterValue(TEXT("WoTSceneTime"), 0.f);
	for (const auto& Weak : SkyMaterials)
		if (auto* Material = Weak.Get()) Material->SetScalarParameterValue(TEXT("WoTSceneTime"), 0.f);
	if (EntryBase)
	{
		ApplyInitialLightningGate();
		Random.Initialize(0x574f54);
		if (!bReducedMotion)
		{
			FWoTEntryStrikeFrame Initial;
			FString Error;
			const bool bValid = EntryStrike.Advance(0.f, [this] { return Random.FRand(); }, Initial, Error);
			check(bValid);
		}
	}
	bRunning = true;
	// Braced: CSV_EVENT expands to nothing in Shipping (MSVC C4390 is an error).
	if (EntryBase) { CSV_EVENT(WoTEntryWeather, TEXT("EntryPublished")); }
	for (const FAudioEndpoint& Endpoint : Ambient)
	{
		if (UAudioComponent* Component = Endpoint.Component.Get())
		{
			Component->SetVolumeMultiplier(Endpoint.AuthoredVolume);
			if (AudioVolume > 0.0f && Component->Sound != nullptr)
			{
				Component->Play();
			}
		}
	}
}

void FWoTFrontEndSceneController::Stop()
{
	// No upload completion can access this controller. Accepted jobs retain only
	// immutable owners and retirement counters, and finish on the detached atlas.
	bStopped = true;
	bRunning = false;
	PendingUpload.Reset();
	EntryAtlas.Reset();
	EntryBase.Reset();
	EntryStrike.Reset();
	LastSamples.Reset();
	if (TickHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
		TickHandle.Reset();
	}
	for (const FAudioEndpoint& Endpoint : Ambient)
	{
		if (UAudioComponent* Component = Endpoint.Component.Get()) Component->Stop();
	}
	for (const FAudioEndpoint& Endpoint : Thunder)
	{
		if (UAudioComponent* Component = Endpoint.Component.Get()) Component->Stop();
	}
	for (const TWeakObjectPtr<UPointLightComponent>& Weak : LightningLights)
	{
		if (UPointLightComponent* Component = Weak.Get())
			Component->SetVisibility(false, true);
	}
	bRunning = false;
	bPrepared = false;
	bLightningOn = false;
	ThunderDelay = -1.0f;
}

uint32 FWoTFrontEndSceneController::AudibleCount() const
{
	if (!bRunning || AudioVolume <= 0.0f) return 0;
	uint32 Count = 0;
	for (const FAudioEndpoint& Endpoint : Ambient)
		if (const UAudioComponent* Component = Endpoint.Component.Get();
			Component != nullptr && Component->IsPlaying()) ++Count;
	return Count;
}

uint32 FWoTFrontEndSceneController::AnimatedWeatherCount(
	uint32 AuthoredCount) const
{
	return bRunning && !bReducedMotion ? AuthoredCount : 0u;
}

int32 FWoTFrontEndSceneController::ThunderSoundIndexForDelay(
	float DelaySeconds) const
{
	if (ThunderWaves.IsEmpty()) return INDEX_NONE;
	// Match RandomLightning.Initialize's thresholds and inclusive comparison.
	for (int32 Index = 0; Index < ThunderWaves.Num(); ++Index)
	{
		const float Threshold = MinThunderDelay
			+ (float(Index + 1) * (MaxThunderDelay - MinThunderDelay) / ThunderWaves.Num());
		if (DelaySeconds <= Threshold) return Index;
	}
	return ThunderWaves.Num() - 1;
}

void FWoTFrontEndSceneController::PlayThunder(float Delay)
{
	if (AudioVolume <= 0.f || ThunderWaves.IsEmpty()) return;
	USoundBase* Wave = ThunderWaves[ThunderSoundIndexForDelay(Delay)].Get();
	if (!Wave) return;
	for (const FAudioEndpoint& Endpoint : Thunder)
	{
		if (UAudioComponent* Component = Endpoint.Component.Get())
		{
			Component->SetSound(Wave);
			Component->SetVolumeMultiplier(Endpoint.AuthoredVolume);
			Component->Play();
		}
	}
}

void FWoTFrontEndSceneController::SubmitPendingAtlas()
{
	check(IsInGameThread());
	CSV_SCOPED_TIMING_STAT(WoTEntryWeather, Submit);
	if (!bRunning || !PendingUpload || Retirement->InFlight.load() >= 2) return;
	const auto Job = PendingUpload.ToSharedRef();
	const auto Counter = Retirement.ToSharedRef();
	const auto Retired = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
	const int32 AdmittedInFlight = Counter->InFlight.fetch_add(1) + 1;
	TFunction<void()> Complete = [Job, Counter, Retired]
	{
		// Safe on render/RHI thread; no raw controller, plan or reusable array.
		if (!Retired->exchange(true)) Counter->InFlight.fetch_sub(1);
	};
	if (AtlasUploader(Job, Complete))
	{
		// Sample admission before a synchronous completion can hide an accepted job.
		CSV_CUSTOM_STAT(WoTEntryWeather, InFlightAcceptedPeak,
			AdmittedInFlight, ECsvCustomStatOp::Max);
		CSV_CUSTOM_STAT(WoTEntryWeather, AcceptedUploads, 1,
			ECsvCustomStatOp::Accumulate);
		++AcceptedUploads;
		PendingUpload.Reset();
	}
	else Complete(); // Includes engine early-return paths, without a leaked slot.
}

void FWoTFrontEndSceneController::TickEntryLightning(float DeltaSeconds)
{
	FWoTEntryStrikeFrame Frame;
	FString Error;
	if (!EntryStrike.Advance(DeltaSeconds, [this] { return Random.FRand(); }, Frame, Error)) return;
	#if CSV_PROFILER_STATS
	if (FCsvProfiler::IsCapturing() &&
		FCsvProfiler::Get()->IsCategoryEnabled(CSV_CATEGORY_INDEX(WoTEntryWeather)))
	{
		int32 NonzeroSamples = 0;
		for (float Sample : Frame.Samples) if (Sample != 0.f) ++NonzeroSamples;
		CSV_CUSTOM_STAT(WoTEntryWeather, NonzeroSamplePeak,
			NonzeroSamples, ECsvCustomStatOp::Max);
	}
	#endif
	CSV_CUSTOM_STAT(WoTEntryWeather, Strikes, Frame.bStrike ? 1 : 0,
		ECsvCustomStatOp::Accumulate);
	// A routed thunder event can occur while audio is muted or unbound.
	CSV_CUSTOM_STAT(WoTEntryWeather, ThunderEvents, Frame.bThunder ? 1 : 0,
		ECsvCustomStatOp::Accumulate);
	if (Frame.bThunder) PlayThunder(Frame.ThunderDelay);
	if (Frame.Samples != LastSamples)
	{
		TArray<uint8> Bytes;
		const bool bAllZero = !Frame.Samples.ContainsByPredicate([](float R) { return R != 0.f; });
		if (bAllZero)
		{
			CSV_SCOPED_TIMING_STAT(WoTEntryWeather, Reset);
			Bytes = EntryBase->LightmapBGRA; // Exact reset, no composition.
			CSV_CUSTOM_STAT(WoTEntryWeather, Resets, 1,
				ECsvCustomStatOp::Accumulate);
		}
		else
		{
			CSV_SCOPED_TIMING_STAT(WoTEntryWeather, Compose);
			if (!WoTComposeEntryLightning(*EntryBase, Frame.Samples, Bytes, Error)) return;
			CSV_CUSTOM_STAT(WoTEntryWeather, Compositions, 1,
				ECsvCustomStatOp::Accumulate);
		}
		LastSamples = MoveTemp(Frame.Samples);
		PendingUpload = MakeShared<const FWoTEntryAtlasUpload, ESPMode::ThreadSafe>(
			EntryAtlas.Get(), EntryBase->LightmapWidth, EntryBase->LightmapHeight,
			MoveTemp(Bytes), ++UploadGeneration);
		CSV_CUSTOM_STAT(WoTEntryWeather, AtlasPayloadBytes,
			PendingUpload->Bytes.Num(), ECsvCustomStatOp::Max);
		CSV_CUSTOM_STAT(WoTEntryWeather, AtlasAllocationBytes,
			static_cast<int32>(PendingUpload->Bytes.GetAllocatedSize()),
			ECsvCustomStatOp::Max);
	}
	// Completion only retires a slot; latest state is submitted on game thread.
	SubmitPendingAtlas();
}

bool FWoTFrontEndSceneController::Tick(float DeltaSeconds)
{
	#if CSV_PROFILER_STATS
	CSV_SCOPED_TIMING_STAT_CONDITIONAL(WoTEntryWeather, EntryTick,
		bRunning && EntryBase.IsValid());
	#endif
	if (!bRunning) return bPrepared && !bStopped;
	if (EntryBase)
	{
		CSV_CUSTOM_STAT(WoTEntryWeather, PendingUpload,
			PendingUpload.IsValid() ? 1 : 0, ECsvCustomStatOp::Set);
	}
	if (!FMath::IsFinite(DeltaSeconds) || DeltaSeconds < 0.f) return true;
	if (bReducedMotion) return true;
	WaterTime += FMath::Max(0.0f, DeltaSeconds);
	for (const auto& Weak : SkyMaterials)
		if (auto* Material = Weak.Get()) Material->SetScalarParameterValue(TEXT("WoTSceneTime"), WaterTime);
	if (EntryBase)
	{
		for (const auto& Weak : WaterMaterials)
			if (auto* Material = Weak.Get()) Material->SetScalarParameterValue(TEXT("WoTSceneTime"), WaterTime);
		TickEntryLightning(DeltaSeconds);
		CSV_CUSTOM_STAT(WoTEntryWeather, PendingUpload,
			PendingUpload.IsValid() ? 1 : 0, ECsvCustomStatOp::Set);
		return true; // Entry atlas lighting never also enables point lights.
	}
	if (ThunderDelay >= 0.0f)
	{
		ThunderDelay -= FMath::Max(0.0f, DeltaSeconds);
		if (ThunderDelay <= 0.0f)
		{
			ThunderDelay = -1.0f;
			if (AudioVolume > 0.0f && !Thunder.IsEmpty()
				&& !ThunderWaves.IsEmpty())
			{
				const int32 SoundIndex =
					ThunderSoundIndexForDelay(LastThunderDelay);
				USoundBase* Wave = ThunderWaves[SoundIndex].Get();
				for (const FAudioEndpoint& Endpoint : Thunder)
				{
					if (UAudioComponent* Component = Endpoint.Component.Get())
					{
						Component->SetSound(Wave);
						Component->SetVolumeMultiplier(Endpoint.AuthoredVolume);
						Component->Play();
					}
				}
			}
		}
	}
	for (const TWeakObjectPtr<UMaterialInstanceDynamic>& Weak : WaterMaterials)
	{
		if (UMaterialInstanceDynamic* Material = Weak.Get())
			Material->SetScalarParameterValue(TEXT("WoTSceneTime"), WaterTime);
	}
	LightningCountdown -= FMath::Max(0.0f, DeltaSeconds);
	if (LightningCountdown <= 0.0f)
	{
		bLightningOn = !bLightningOn;
		for (const TWeakObjectPtr<UPointLightComponent>& Weak : LightningLights)
		{
			if (UPointLightComponent* Component = Weak.Get())
				Component->SetVisibility(bLightningOn, true);
		}
		// A flash is short; the quiet interval is bounded and randomized.
		LightningCountdown = bLightningOn
			? Random.FRandRange(0.05f, 0.25f)
			: Random.FRandRange(MinStrikeInterval, MaxStrikeInterval);
		if (bLightningOn)
		{
			// Light reaches the viewer first. Thunder remains bounded and
			// randomized without introducing a wall-clock or network source.
			ThunderDelay = Random.FRandRange(MinThunderDelay, MaxThunderDelay);
			LastThunderDelay = ThunderDelay;
		}
	}
	return true;
}
