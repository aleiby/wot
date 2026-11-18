#pragma once

#include "CoreMinimal.h"
#include "WoTRetailReader/FrontEndWorld.h"

#if WITH_AUTOMATION_TESTS

class UWorld;
class UWoTPayloadSubsystem;
class FWoTCacheGenerationLease;

inline const TCHAR* WoTFrontEndTest_DomainName(EWoTFactDomain Domain)
{
	switch (Domain)
	{
	case EWoTFactDomain::Texture: return TEXT("texture");
	case EWoTFactDomain::Sound: return TEXT("sound");
	case EWoTFactDomain::Mesh: return TEXT("mesh");
	case EWoTFactDomain::Animation: return TEXT("animation");
	case EWoTFactDomain::Font: return TEXT("font");
	case EWoTFactDomain::Procedural: return TEXT("procedural");
	case EWoTFactDomain::Localization: return TEXT("localization");
	default: return nullptr;
	}
}

/** Closed semantic entry fixture derived from the contract's literal closure. */
inline TWoTBootstrapResult<FWoTFrontEndDescriptor>
WoTFrontEndTest_MakeDescriptor(const FWoTRetailContract& Contract)
{
	using FResult = TWoTBootstrapResult<FWoTFrontEndDescriptor>;
	FString Actors;
	auto AddActors = [&Actors](const TCHAR* ClassName, int32 Count)
	{
		for (int32 Index = 0; Index < Count; ++Index)
		{
			if (!Actors.IsEmpty()) Actors += TEXT(",");
			Actors += FString::Printf(
				TEXT("{\"class\":\"%s\",\"identity\":\"WheelOfTime.%s%d\"}"),
				ClassName, ClassName, Index);
		}
	};
	AddActors(TEXT("LevelInfo"), 1);
	AddActors(TEXT("Brush"), 76);
	AddActors(TEXT("PlayerStart"), 1);
	AddActors(TEXT("WOTZoneInfo"), 1);
	AddActors(TEXT("Light"), 39);
	AddActors(TEXT("ParticleSprayer"), 4);
	AddActors(TEXT("AmbientSound"), 5);
	AddActors(TEXT("RandomLightning"), 1);
	AddActors(TEXT("Lightning"), 5);
	AddActors(TEXT("WaterZone"), 1);
	AddActors(TEXT("SkyZoneInfo"), 1);
	AddActors(TEXT("RainCyd"), 6);

	FString References;
	for (const FString& LogicalId : Contract.StartupCriticalLogicalIds)
	{
		const FWoTRequiredFact* BoundFact = nullptr;
		for (const FWoTRequiredFact& Fact : Contract.Facts)
		{
			if (Fact.Outputs.ContainsByPredicate(
				[&LogicalId](const FWoTRequiredFactOutput& Output)
				{
					return Output.LogicalId == LogicalId;
				}))
			{
				BoundFact = &Fact;
				break;
			}
		}
		if (BoundFact == nullptr)
		{
			return FResult::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::UnknownField,
				TEXT("the front-end fixture closure is not contract-bound"), LogicalId));
		}
		const TCHAR* Domain = WoTFrontEndTest_DomainName(BoundFact->Domain);
		if (Domain == nullptr) continue;
		if (!References.IsEmpty()) References += TEXT(",");
		References += FString::Printf(
			TEXT("{\"domain\":\"%s\",\"identity\":\"%s\",\"kind\":\"resource\"}"),
			Domain, *BoundFact->SourceExport);
	}
	const FString Fixture = FString::Printf(TEXT(
		"{\"actors\":[%s],"
		"\"camera\":{\"horizontal_fov\":90,"
		"\"location\":[\"-412.484130859375\",\"-423.762054443359375\","
		"\"27.020946502685547\"],"
		"\"rotation\":[\"16.4794921875\",\"59.853515625\",\"0\"]},"
		"\"format\":\"wot-front-end-semantic-fixture-v1\","
		"\"map_name\":\"WheelOfTime\",\"references\":[%s],"
		"\"thunder_sound_source_exports\":["
		"\"Environment.Thunder.Thunder9\","
		"\"Environment.Thunder.Thunder3\","
		"\"Environment.Thunder.Thunder10\","
		"\"Environment.Thunder.Thunder2\","
		"\"Environment.Thunder.Thunder4\","
		"\"Environment.Thunder.Thunder8\","
		"\"Environment.Thunder.Thunder8\","
		"\"Environment.Thunder.Thunder2\","
		"\"Environment.Thunder.Thunder3\","
		"\"Environment.Thunder.Thunder10\"]}"),
		*Actors, *References);
	FTCHARToUTF8 Utf8(*Fixture);
	TArray<uint8> Bytes;
	Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	return BuildFrontEndDescriptorSemanticFixtureForTesting(Bytes, Contract);
}

/** Shared exact-closure fixture for exercising the production Stage
 * path. All objects are synthetic canonical cache records; no retail bytes or
 * retail-derived pins enter the test binary. */
struct FWoTFrontEndStageProductionFixture
{
	FWoTFrontEndStageProductionFixture();
	~FWoTFrontEndStageProductionFixture();
	FWoTFrontEndStageProductionFixture(
		FWoTFrontEndStageProductionFixture&&) = delete;
	FWoTFrontEndStageProductionFixture& operator=(
		FWoTFrontEndStageProductionFixture&&) = delete;
	FWoTFrontEndStageProductionFixture(
		const FWoTFrontEndStageProductionFixture&) = delete;
	FWoTFrontEndStageProductionFixture& operator=(
		const FWoTFrontEndStageProductionFixture&) = delete;

	FString CacheRoot;
	bool bIncludeBrowserResources = false;
	bool bMalformedBrowserLookAndFeel = false;
	bool bPatternedFontGlyphs = false;
	bool bZeroWidthFontGlyph = false;
	FWoTFrontEndDescriptor Descriptor;
	UWorld* World = nullptr;
	UWoTPayloadSubsystem* Payload = nullptr;
	TUniquePtr<FWoTCacheGenerationLease> WorldLease;
};

struct FWoTFrontEndWeatherFixtureOptions
{
	bool bLegacyV2 = false;
	bool bBSPV4 = false;
	bool bSharedSkyTexture = false;
	bool bMissingGround = false;
	bool bMalformedRainScaleGlow = false;
	bool bMalformedRainAmbientGlow = false;
	bool bMalformedLightningLength = false;
	bool bMismatchedLightMetadata = false;
	bool bDefaultRainGlow = false;
	bool bAdditionalWaterSection = false;
	bool bSkyStyleCoverage = false;
	bool bMaskedSurfaceTexture = false;
	bool bWorldSkyBlendCoverage = false;
	bool bUseSplash02ParticleTexture = false;
	FString WorldTextureOverride;
	TArray<float> RainScaleGlow = {0.1f, 0.2f, 0.1f, 0.1f, 0.1f, 0.1f};
};

enum class EWoTFrontEndRainSkinFixture : uint8
{
	MeshLocal,
	ClassDefaultOneSection,
	ClassDefaultMultiSlot,
	ClassDefaultWrongIndex,
	ClassDefaultWrongIdentity,
	ClassDefaultExtraSection,
};

bool WoTFrontEndTest_BuildProductionStageFixture(
	FWoTFrontEndStageProductionFixture& Out, bool bCorruptBSP,
	FString& OutError, bool bDuplicateLightningRoute = false,
	bool bThunderIndexGap = false, bool bThunderOverrideMismatch = false,
	bool bThunderOverrideRootMismatch = false,
	bool bThunderOverrideCaseMismatch = false,
	FStringView BSPMaterialSlotOverride = {},
	EWoTFrontEndRainSkinFixture RainSkinFixture =
		EWoTFrontEndRainSkinFixture::MeshLocal,
	bool bMissingLightmap = false,
	bool bIncludeDefaultWaterSection = false);
bool WoTFrontEndTest_BuildWeatherStageFixture(
	FWoTFrontEndStageProductionFixture& Out,
	const FWoTFrontEndWeatherFixtureOptions& Options, FString& Error);
bool WoTFrontEndTest_BuildRealWeatherStageFixture(
	FWoTFrontEndStageProductionFixture& Out, FString& Error);
void WoTFrontEndTest_DestroyProductionStageFixture(
	FWoTFrontEndStageProductionFixture& Fixture);

/** A WoT.int in the retail layout holding placeholder text only (never
 * retail text): a MenuTitle and MenuList/HelpMessage[1..20] in every menu
 * section the front end, Player Config and settings read, so a projection
 * that drops any label shows as a raw key on the real screens. Labels read
 * "Label <section> <index>"; menuKeyboard (section 6) MenuList[12] is
 * quoted, as retail quotes it. */
TArray<uint8> WoTFrontEndTest_PlaceholderMenuLocalization();
/** True for text that is an unresolved localization id (what
 * FWoTMenuStyle::RetailText shows when no source holds the label). */
inline bool WoTFrontEndTest_IsRawMenuKey(const FString& Text)
{
	return Text.StartsWith(TEXT("menu"), ESearchCase::IgnoreCase)
		&& Text.Contains(TEXT(".")) && !Text.Contains(TEXT(" "));
}

#endif
