#pragma once

#include "CoreMinimal.h"
#include "Sound/SoundWaveProcedural.h"
#include "WoTPayloadSoundWave.generated.h"

struct FWoTSoundPayloadData;

// Process-wide counts of payload sound generators and the samples they produced,
// read by the net-test capture to tell a silent mix from silent sources.
struct FWoTPayloadSoundStats
{
	int64 Generators = 0;
	int64 NullGenerators = 0;
	int64 Samples = 0;
	int64 NonZeroSamples = 0;
};
FWoTPayloadSoundStats GetWoTPayloadSoundStats();

// Metadata-only stripped sound asset. The retail PCM stream is supplied by
// UWoTPayloadSubsystem and every playback receives an independent generator.
UCLASS(BlueprintType)
class UWoTPayloadSoundWave : public USoundWaveProcedural
{
	GENERATED_BODY()

public:
	UWoTPayloadSoundWave(const FObjectInitializer& ObjectInitializer);

	virtual void PostLoad() override;
	virtual ISoundGeneratorPtr CreateSoundGenerator(
		const FSoundGeneratorInitParams& InParams) override;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="WoT Payload")
	FString PayloadKey;

	// Serialized from the source WAV by the stripped importer so the ordinary
	// sound path has correct structure before cache binding/proxy creation.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="WoT Payload",
		meta=(ClampMin="1", ClampMax="8"))
	int32 PayloadChannels = 1;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="WoT Payload",
		meta=(ClampMin="1", ClampMax="384000"))
	int32 PayloadSampleRate = 22050;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="WoT Payload")
	int32 PayloadBitsPerSample = 16;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="WoT Payload",
		meta=(ClampMin="0"))
	int32 PayloadFrameCount = 0;

	// True only while the current active subsystem has published an authenticated
	// immutable snapshot for this exact wave object.
	bool IsPayloadBound() const;

private:
	bool ApplyStructuralMetadata();
	bool PreparePayloadSnapshot(const FWoTSoundPayloadData& Payload);
	bool IsPayloadStructurallyCompatible(
		const FWoTSoundPayloadData& Payload) const;

	friend class UWoTPayloadSubsystem;
};
