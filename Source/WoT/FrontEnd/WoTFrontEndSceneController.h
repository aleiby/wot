#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "WoTFrontEndLightning.h"
#include "RHITypes.h"
#include "UObject/StrongObjectPtr.h"

class UAudioComponent;
class UMaterialInstanceDynamic;
class UPointLightComponent;
class UTexture2D;

// Built on the game thread, then shared immutably through render/RHI cleanup.
// No callback needs the controller or the scene plan to remain alive.
struct FWoTEntryAtlasUpload
{
	FWoTEntryAtlasUpload(UTexture2D* InTexture, uint32 Width, uint32 Height,
		TArray<uint8>&& InBytes, uint64 InGeneration);
	const TStrongObjectPtr<UTexture2D> Texture;
	const TArray<uint8> Bytes;
	const FUpdateTextureRegion2D Region;
	const uint64 Generation;
};

// True transfers cleanup responsibility: completion must run exactly once.
// False rejects synchronously and must not retain the job or callback.
using FWoTEntryAtlasUploader = TFunction<bool(
	TSharedRef<const FWoTEntryAtlasUpload, ESPMode::ThreadSafe>, TFunction<void()>)>;
struct FWoTEntryAtlasRetirement;

/** Live behavior owned by one committed front-end scene. Prepare installs the
 * fallible ticker capability while it remains inert; Publish is the no-fail
 * atomic boundary that permits tick/audio behavior. */
class FWoTFrontEndSceneController final
	: public TSharedFromThis<FWoTFrontEndSceneController>
{
public:
	FWoTFrontEndSceneController(bool bInReducedMotion, float InAudioVolume,
		FWoTEntryAtlasUploader InUploader = {});
	~FWoTFrontEndSceneController();

	void AddAmbient(UAudioComponent* Component, float AuthoredVolume);
	void AddThunderEndpoint(UAudioComponent* Component, float AuthoredVolume);
	void AddThunderWave(class USoundBase* Wave);
	void AddLightningLight(UPointLightComponent* Component);
	void ApplyInitialLightningGate();
	void ConfigureLightningTiming(float MinStrikeSeconds, float MaxStrikeSeconds,
		float MinThunderSeconds, float MaxThunderSeconds);
	void AddWaterMaterial(UMaterialInstanceDynamic* Material);
	void AddSkyMaterial(UMaterialInstanceDynamic* Material);
	// Atlas is scene-owned, nonstreaming BGRA8, already initialized to Base.
	// Do not replace/release its resource or change streaming while uploads are
	// outstanding. Stop detaches this generation; rematerialize with a fresh atlas
	// and controller after hiding the old scene. A stopped controller is terminal.
	bool ConfigureEntryLightning(TSharedRef<const FWoTBSPPayloadData> Base,
		UTexture2D* Atlas, const FWoTEntryStrikeSettings& Settings, FString& Error);
	bool Prepare();
	void Publish();
	void Stop();
	bool IsRunning() const { return bRunning; }
	uint32 AudibleCount() const;
	uint32 AnimatedWeatherCount(uint32 AuthoredCount) const;
	uint32 ThunderEndpointCount() const { return Thunder.Num(); }
	float MinStrikeSeconds() const { return MinStrikeInterval; }
	float MaxStrikeSeconds() const { return MaxStrikeInterval; }
	float NextStrikeSeconds() const
	{
		return EntryBase ? EntryStrike.SecondsUntilNextStrike() : LightningCountdown;
	}
	int32 ThunderSoundIndexForDelay(float DelaySeconds) const;

#if WITH_AUTOMATION_TESTS
	bool TickForTesting(float DeltaSeconds) { return Tick(DeltaSeconds); }
	float SceneTimeForTesting() const { return WaterTime; }
	int32 UploadsInFlightForTesting() const;
	bool HasPendingUploadForTesting() const { return PendingUpload.IsValid(); }
	uint64 AcceptedUploadsForTesting() const { return AcceptedUploads; }
	const TArray<float>& LightningSamplesForTesting() const { return LastSamples; }
#endif

private:
	bool Tick(float DeltaSeconds);
	void TickEntryLightning(float DeltaSeconds);
	void SubmitPendingAtlas();
	void PlayThunder(float Delay);

	struct FAudioEndpoint
	{
		TWeakObjectPtr<UAudioComponent> Component;
		float AuthoredVolume = 1.0f;
	};
	TArray<FAudioEndpoint> Ambient;
	TArray<FAudioEndpoint> Thunder;
	TArray<TWeakObjectPtr<class USoundBase>> ThunderWaves;
	TArray<TWeakObjectPtr<UPointLightComponent>> LightningLights;
	TArray<TWeakObjectPtr<UMaterialInstanceDynamic>> WaterMaterials;
	TArray<TWeakObjectPtr<UMaterialInstanceDynamic>> SkyMaterials;
	TSharedPtr<const FWoTBSPPayloadData> EntryBase;
	TStrongObjectPtr<UTexture2D> EntryAtlas;
	FWoTEntryStrikeState EntryStrike;
	FWoTEntryAtlasUploader AtlasUploader;
	TSharedPtr<FWoTEntryAtlasRetirement, ESPMode::ThreadSafe> Retirement;
	TSharedPtr<const FWoTEntryAtlasUpload, ESPMode::ThreadSafe> PendingUpload;
	TArray<float> LastSamples;
	uint64 UploadGeneration = 0, AcceptedUploads = 0;
	FTSTicker::FDelegateHandle TickHandle;
	FRandomStream Random{0x574f54};
	// FApp owns the global BootstrapVolume multiplier. This is only the typed
	// zero/play gate; components retain their authenticated authored volumes.
	float AudioVolume = 1.0f;
	float LightningCountdown = 1.0f;
	float WaterTime = 0.0f;
	float ThunderDelay = -1.0f;
	float LastThunderDelay = 0.0f;
	float MinStrikeInterval = 3.0f;
	float MaxStrikeInterval = 7.0f;
	float MinThunderDelay = 0.0f;
	float MaxThunderDelay = 3.0f;
	bool bReducedMotion = false;
	bool bPrepared = false;
	bool bRunning = false;
	bool bLightningOn = false;
	bool bStopped = false;
};
