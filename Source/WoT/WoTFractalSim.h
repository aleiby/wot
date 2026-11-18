#pragma once

#include "CoreMinimal.h"
#include "WoTFractalData.h"

// UTexture::Tick's shared MinFrameRate/MaxFrameRate accumulator rule. The
// callback is invoked at most once per request, even after a long hitch.
WOT_API bool WoTAdvanceRateLimitedTexture(float& BankedSeconds,
	float SlowestFramesPerSecond, float FastestFramesPerSecond,
	float DeltaSeconds, TFunctionRef<void()> Step);

/** The process-global lagged-XOR byte stream used by the original fractal textures. */
class WOT_API FWoTFractalRandomStream
{
public:
	void Initialize(uint32 Seed);
	uint8 Next();

private:
	TArray<uint8> Table;
	uint32 Index = 0;
};

/** Small class-neutral seam used by the world-owned activation lifecycle. */
class WOT_API IWoTFractalSim
{
public:
	using FBeforeStep = TFunction<void()>;
	virtual ~IWoTFractalSim() = default;

	virtual bool Initialize(const FWoTFractalTextureRecord& InDefinition) = 0;
	virtual void SetRandomStream(FWoTFractalRandomStream* InRandomStream) = 0;
	virtual bool UpdateSourceIndices(TConstArrayView<uint8> InSourceIndices,
		int32 SourceSlot = 0) = 0;
	virtual void Reset() = 0;
	virtual void Prime(const FBeforeStep& BeforeStep = FBeforeStep()) = 0;
	virtual int32 TickTexture(float DeltaSeconds,
		const FBeforeStep& BeforeStep = FBeforeStep()) = 0;
	virtual void ConstantTimeTick(
		const FBeforeStep& BeforeStep = FBeforeStep()) = 0;
	virtual const TArray<uint8>& GetOutputIndices() const = 0;
	virtual const FWoTFractalTextureRecord& GetDefinition() const = 0;
	virtual int32 GetStepCount() const = 0;
	virtual bool IsValid() const = 0;
};

/** Pure port of UWaterTexture/UWetTexture/UWaveTexture constant-time ticks. */
class WOT_API FWoTWaterFractalSim final : public IWoTFractalSim
{
public:
	bool Initialize(const FWoTFractalTextureRecord& InDefinition) override;
	void SetRandomStream(FWoTFractalRandomStream* InRandomStream) override;
	bool UpdateSourceIndices(TConstArrayView<uint8> InSourceIndices,
		int32 SourceSlot = 0) override;
	void Reset() override;
	void Prime(const FBeforeStep& BeforeStep = FBeforeStep()) override;
	int32 TickTexture(float DeltaSeconds,
		const FBeforeStep& BeforeStep = FBeforeStep()) override;
	void ConstantTimeTick(
		const FBeforeStep& BeforeStep = FBeforeStep()) override;

	const TArray<uint8>& GetOutputIndices() const override { return OutputIndices; }
	const FWoTFractalTextureRecord& GetDefinition() const override { return Definition; }
	int32 GetPrimeCurrent() const { return PrimeCurrent; }
	int32 GetStepCount() const override { return StepCount; }
	float GetAccumulator() const { return Accumulator; }
	bool IsValid() const override { return bValid; }

private:
	void BuildTables();
	void InitializeSourceBitmap();
	void WaterRedrawDrops();
	void CalculateWater();
	void ApplyWetTexture();
	int32 FieldIndex(bool bFieldB, int32 X, int32 Y) const;
	int32 PixelIndex(int32 X, int32 Y) const;
	void Output4Pix(int32 DestIndex,
		uint8 SourceA, uint8 SourceC, uint8 SourceE, uint8 SourceG,
		uint8 SourceB, uint8 SourceD, uint8 SourceF, uint8 SourceH,
		int32 Dest1, int32 Dest2, int32 Dest3, int32 Dest4);
	uint8 SpeedRand();

	FWoTFractalTextureRecord Definition;
	TArray<FWoTFractalDrop> Drops;
	TArray<uint8> SourceFields;
	TArray<uint8> SourceBitmap;
	TArray<uint8> OutputIndices;
	TArray<uint8> RenderTable;
	TArray<uint8> WaveTable;
	TArray<uint8> PhaseTable;
	FWoTFractalRandomStream LocalRandom;
	FWoTFractalRandomStream* RandomStream = nullptr;
	int32 XDimension = 0;
	int32 YDimension = 0;
	int32 UBits = 0;
	uint8 WaveParity = 0;
	int32 GlobalPhase = 0;
	int32 PrimeCurrent = 0;
	int32 StepCount = 0;
	float Accumulator = 0.0f;
	bool bValid = false;
};

/** Exact C-path UFireTexture byte automaton and shipped spark machinery. */
class WOT_API FWoTFireFractalSim final : public IWoTFractalSim
{
public:
	bool Initialize(const FWoTFractalTextureRecord& InDefinition) override;
	void SetRandomStream(FWoTFractalRandomStream* InRandomStream) override;
	bool UpdateSourceIndices(TConstArrayView<uint8>, int32 = 0) override
	{
		return false;
	}
	void Reset() override;
	void Prime(const FBeforeStep& BeforeStep = FBeforeStep()) override;
	int32 TickTexture(float DeltaSeconds,
		const FBeforeStep& BeforeStep = FBeforeStep()) override;
	void ConstantTimeTick(
		const FBeforeStep& BeforeStep = FBeforeStep()) override;

	const TArray<uint8>& GetOutputIndices() const override { return OutputIndices; }
	const FWoTFractalTextureRecord& GetDefinition() const override { return Definition; }
	int32 GetStepCount() const override { return StepCount; }
	bool IsValid() const override { return bValid; }
	static bool IsAuthoredSparkTypeSupported(uint8 Type);

private:
	void BuildTables();
	void RedrawSparks();
	void PostDrawSparks();
	void CalculateWrapFire();
	void CalculateSlowFire();
	void MoveSpark(FWoTFractalSpark& Spark);
	void DrawFlashRamp(uint8 X, uint8 Y, uint8 XLength, uint8 YLength,
		uint8 Color1, uint8 Color2);
	void PutPixel(int32 X, int32 Y, uint8 Value);
	uint8 SpeedRand();

	FWoTFractalTextureRecord Definition;
	TArray<FWoTFractalSpark> Sparks;
	TArray<uint8> OutputIndices;
	TArray<uint8> RenderTable;
	TArray<uint8> PhaseTable;
	FWoTFractalRandomStream LocalRandom;
	FWoTFractalRandomStream* RandomStream = nullptr;
	int32 UBits = 0;
	int32 GlobalPhase = 0;
	int32 PrimeCurrent = 0;
	int32 StepCount = 0;
	float Accumulator = 0.0f;
	bool bStarStatus = true;
	bool bValid = false;
};

/** Exact UIceTexture movement plus scalar indexed blitters. */
class WOT_API FWoTIceFractalSim final : public IWoTFractalSim
{
public:
	bool Initialize(const FWoTFractalTextureRecord& InDefinition) override;
	void SetRandomStream(FWoTFractalRandomStream*) override {}
	bool UpdateSourceIndices(TConstArrayView<uint8> InSourceIndices,
		int32 SourceSlot = 0) override;
	void Reset() override;
	void Prime(const FBeforeStep& BeforeStep = FBeforeStep()) override;
	int32 TickTexture(float DeltaSeconds,
		const FBeforeStep& BeforeStep = FBeforeStep()) override;
	void ConstantTimeTick(
		const FBeforeStep& BeforeStep = FBeforeStep()) override;

	const TArray<uint8>& GetOutputIndices() const override { return OutputIndices; }
	const FWoTFractalTextureRecord& GetDefinition() const override { return Definition; }
	int32 GetStepCount() const override { return StepCount; }
	bool IsValid() const override { return bValid; }
	float GetMasterCount() const { return MasterCount; }
	float GetUPosition() const { return UPosition; }
	float GetVPosition() const { return VPosition; }

private:
	void MoveIcePosition(float DeltaSeconds);
	void RenderIce(float DeltaSeconds);
	void BlitTexIce();
	void BlitIceTex();
	static int32 RetailRound(float Value);

	FWoTFractalTextureRecord Definition;
	TArray<uint8> SourceIndices;
	TArray<uint8> GlassIndices;
	TArray<uint8> OutputIndices;
	float MasterCount = 0.0f;
	float UDisplace = 0.0f;
	float VDisplace = 0.0f;
	float UPosition = 0.0f;
	float VPosition = 0.0f;
	float Accumulator = 0.0f;
	int32 OldUDisplace = -1;
	int32 OldVDisplace = -1;
	int32 PrimeCurrent = 0;
	int32 StepCount = 0;
	bool bForceRefresh = true;
	bool bValid = false;
};
