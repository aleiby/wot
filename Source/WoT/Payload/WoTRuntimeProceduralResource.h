// Strict cache-v3 procedural seed/state decoder (ue1-fractal-inputs-v2).
#pragma once

#include "CoreMinimal.h"
#include "WoTRuntimeRegistry.h"

struct FWoTFractalTextureRecord;

struct FWoTRuntimeProceduralMip
{
	uint32 Width = 0;
	uint32 Height = 0;
	TArray<uint8> Indices;
};

/** One SourceTexture/GlassTexture input of a procedural record. */
struct FWoTRuntimeProceduralInput
{
	/** 0 none, 1 plain texture (texture domain), 2 procedural texture. */
	uint8 Kind = 0;
	FString Identity;
	FString ClassName;
	uint32 Width = 0;
	uint32 Height = 0;
};

/** Simulation parameters the v2 object resolves at cache build (mirrors
    Tools/convert/bake_flipbooks.py _runtime_base). */
struct FWoTRuntimeProceduralParameters
{
	float MinFrameRate = 0.0f;
	float MaxFrameRate = 0.0f;
	int32 PrimeCount = 0;
	uint8 WaveAmp = 0;
	uint8 BumpMapLight = 0;
	uint8 BumpMapAngle = 0;
	uint8 PhongRange = 0;
	uint8 PhongSize = 0;
	uint8 RenderHeat = 0;
	bool bRising = false;
	int32 SparksLimit = 0;
	uint8 PanningStyle = 0;
	uint8 TimeMethod = 0;
	uint8 HorizPanSpeed = 0;
	uint8 VertPanSpeed = 0;
	uint8 Frequency = 0;
	uint8 Amplitude = 0;
	bool bMoveIce = false;
	float MasterCount = 0.0f;
	float UDisplace = 0.0f;
	float VDisplace = 0.0f;
	float UPosition = 0.0f;
	float VPosition = 0.0f;
	bool bMasked = false;
};

class WOT_API FWoTRuntimeProceduralResource final
{
public:
	FWoTRuntimeProceduralResource() = default;
	FWoTRuntimeProceduralResource(FWoTRuntimeProceduralResource&&) = default;
	FWoTRuntimeProceduralResource& operator=(
		FWoTRuntimeProceduralResource&&) = default;
	FWoTRuntimeProceduralResource(
		const FWoTRuntimeProceduralResource&) = delete;
	FWoTRuntimeProceduralResource& operator=(
		const FWoTRuntimeProceduralResource&) = delete;

	FWoTBootstrapStatus LoadFirstUse(
		const FWoTNeutralResourceId& Resource);
	const FString& ClassName() const { return ParsedClassName; }
	/** Canonical retail identity ("Package[.Group].Name", collision-suffixed). */
	const FString& SourceExport() const { return ParsedSourceExport; }
	/** The authenticated logical id this object was loaded from. */
	const FString& LogicalId() const { return ParsedLogicalId; }
	const TArray<FWoTRuntimeProceduralMip>& Mips() const { return ParsedMips; }
	const TArray<TArray<uint8>>& States() const { return ParsedStates; }
	uint32 PropertyCount() const { return ParsedPropertyCount; }
	uint32 RandomSeed() const { return ParsedRandomSeed; }
	/** Raw 256-entry RGBA palette bytes as serialized by the retail package. */
	const TArray<uint8>& PaletteRGBA() const { return ParsedPalette; }
	const FWoTRuntimeProceduralParameters& Parameters() const { return ParsedParameters; }
	const TArray<TArray<uint8>>& Drops() const { return ParsedDrops; }
	const FWoTRuntimeProceduralInput& SourceInput() const { return ParsedSource; }
	const FWoTRuntimeProceduralInput& GlassInput() const { return ParsedGlass; }

	/** Fills the runtime fractal record from the authenticated object: identity,
	    class, mip-0 seed, palette (alpha 255; masked slot zero 0), sparks,
	    drops, parameters, and the input identities/dimensions. Plain-texture
	    source/glass indices are left empty for the subsystem to rehydrate from
	    the texture domain. False for a class the runtime port does not simulate. */
	bool BuildRecord(FWoTFractalTextureRecord& Out) const;

	/** "Package__Name" identity key of a canonical procedural identity: the
	    first and last dotted components, any "#export-N" suffix stripped. */
	static FName IdentityKeyForSourceExport(FStringView CanonicalIdentity);
	static FString PackageOfSourceExport(FStringView CanonicalIdentity);
	static FString NameOfSourceExport(FStringView CanonicalIdentity);

private:
	TOptional<FWoTRuntimeVerifiedObject> Object;
	FString ParsedClassName;
	FString ParsedSourceExport;
	FString ParsedLogicalId;
	TArray<FWoTRuntimeProceduralMip> ParsedMips;
	TArray<TArray<uint8>> ParsedStates;
	TArray<uint8> ParsedPalette;
	FWoTRuntimeProceduralParameters ParsedParameters;
	TArray<TArray<uint8>> ParsedDrops;
	FWoTRuntimeProceduralInput ParsedSource;
	FWoTRuntimeProceduralInput ParsedGlass;
	uint32 ParsedPropertyCount = 0;
	uint32 ParsedRandomSeed = 0;
};
