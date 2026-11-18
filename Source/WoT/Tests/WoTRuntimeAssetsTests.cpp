#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "../Assets/WoTRetailIdentity.h"
#include "../Assets/WoTRuntimeAssets.h"
#include "../Payload/WoTNeutralResources.h"
#include "../Payload/WoTPayloadActivation.h"
#include "../WoTPayloadSubsystem.h"
#include "../WoTPayloadSoundWave.h"
#include "../WoTSpriteBillboard.h"

#include "Engine/Texture2D.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeAssetPathRulesSpec,
	"WoT.Assets.PathRulesAndAliases",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTRuntimeAssetPathRulesSpec::RunTest(const FString&)
{
	const TArray<FWoTNeutralResourceId> Resources = {
		{TEXT("cache/sound/AmbientA__Dive__2"), EWoTNeutralResourceKind::Sound},
		{TEXT("cache/texture/WOT__Crosshair7"), EWoTNeutralResourceKind::Texture},
		{TEXT("cache/texture/WoTBase__S"), EWoTNeutralResourceKind::Texture},
		{TEXT("cache/texture/WoTBase__M"), EWoTNeutralResourceKind::Texture},
		{TEXT("cache/texture/WOT__T32_09"), EWoTNeutralResourceKind::Texture},
		{TEXT("cache/texture/Angreal__M_Freeze"), EWoTNeutralResourceKind::Texture},
		{TEXT("cache/texture/WoTPawns__M_SMyrddraal"), EWoTNeutralResourceKind::Texture},
		{TEXT("cache/texture/WoTPawns__H_AesSedai0"), EWoTNeutralResourceKind::Texture}};
	struct FCase
	{
		const TCHAR* Path;
		EWoTRuntimeAssetKind Kind;
		const TCHAR* Expected;
	};
	const FCase Cases[] = {
		{TEXT("/Game/WoT/Sounds/ambienta/dive__2.DIVE__2"),
			EWoTRuntimeAssetKind::Sound, TEXT("cache/sound/AmbientA__Dive__2")},
		{TEXT("/Game/WoT/UI/Crosshair7.Crosshair7"),
			EWoTRuntimeAssetKind::Texture, TEXT("cache/texture/WOT__Crosshair7")},
		{TEXT("/Game/WoT/UI/StatusIconSelected.StatusIconSelected"),
			EWoTRuntimeAssetKind::Texture, TEXT("cache/texture/WoTBase__S")},
		{TEXT("/Game/WoT/UI/StatusIconFrame.StatusIconFrame"),
			EWoTRuntimeAssetKind::Texture, TEXT("cache/texture/WoTBase__M")},
		{TEXT("/Game/WoT/UI/T32_09.T32_09"),
			EWoTRuntimeAssetKind::Texture, TEXT("cache/texture/WOT__T32_09")},
		{TEXT("/Game/WoT/UI/M_Freeze.M_Freeze"),
			EWoTRuntimeAssetKind::Texture, TEXT("cache/texture/Angreal__M_Freeze")},
		{TEXT("/Game/WoT/UI/M_SMyrddraal.M_SMyrddraal"),
			EWoTRuntimeAssetKind::Texture, TEXT("cache/texture/WoTPawns__M_SMyrddraal")},
		{TEXT("/Game/WoT/Textures/wotpawns/h_aessedai0.H_AesSedai0"),
			EWoTRuntimeAssetKind::Texture, TEXT("cache/texture/WoTPawns__H_AesSedai0")}};
	for (const FCase& Row : Cases)
	{
		FString Actual;
		FString Failure;
		TestTrue(*FString::Printf(TEXT("resolves %s"), Row.Path),
			UWoTRuntimeAssets::ResolveCacheId(
				Row.Path, Row.Kind, Resources, Actual, &Failure));
		TestEqual(*FString::Printf(TEXT("identity for %s"), Row.Path),
			Actual, FString(Row.Expected));
	}

	FString Package;
	FString Name;
	TestTrue(TEXT("cache identity splits at first separator"),
		UWoTRuntimeAssets::SplitCacheIdentity(
			TEXT("cache/sound/AmbientA__Dive__2"),
			EWoTRuntimeAssetKind::Sound, Package, Name));
	TestEqual(TEXT("split package"), Package, FString(TEXT("AmbientA")));
	TestEqual(TEXT("double-separator remains in retail name"),
		Name, FString(TEXT("Dive__2")));

	TArray<FWoTNeutralResourceId> Ambiguous = Resources;
	Ambiguous.Add({TEXT("cache/sound/ambienta__dive__2"),
		EWoTNeutralResourceKind::Sound});
	FString Ignored;
	FString Ambiguity;
	TestFalse(TEXT("case-folded duplicate fails closed"),
		UWoTRuntimeAssets::ResolveCacheId(
			TEXT("/Game/WoT/Sounds/AmbientA/Dive__2.Dive__2"),
			EWoTRuntimeAssetKind::Sound, Ambiguous, Ignored, &Ambiguity));
	TestTrue(TEXT("ambiguity is diagnosed"),
		Ambiguity.Contains(TEXT("ambiguous"), ESearchCase::IgnoreCase));
	TestFalse(TEXT("missing identity fails without a crash"),
		UWoTRuntimeAssets::ResolveCacheId(
			TEXT("/Game/WoT/Textures/WOT/Missing.Missing"),
			EWoTRuntimeAssetKind::Texture, Resources, Ignored));
	return true;
}

// Exact identity (WoTRetailIdentity.h mirrors Tools/dist/
// retail_identity.py): names that collide inside their package are grouped
// exact identities; everything else keeps Pkg__Name; FName-case comparison.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailIdentityRulesSpec,
	"WoT.Assets.RetailIdentityRules",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTRetailIdentityRulesSpec::RunTest(const FString&)
{
	FWoTRetailObjectPath Path;
	TestTrue(TEXT("grouped path parses"),
		WoTParseRetailObjectPath(TEXT("AesSedaiT.Ornament.PaprWall1441"), Path));
	TestEqual(TEXT("package"), Path.Package, FString(TEXT("AesSedaiT")));
	TestTrue(TEXT("one group"), Path.Groups == TArray<FString>{TEXT("Ornament")});
	TestEqual(TEXT("name"), Path.Name, FString(TEXT("PaprWall1441")));
	TestEqual(TEXT("plain base"), WoTRetailPlainLogicalBase(Path),
		FString(TEXT("AesSedaiT__PaprWall1441")));
	TestEqual(TEXT("exact base"), WoTRetailExactLogicalBase(Path),
		FString(TEXT("AesSedaiT.Ornament__PaprWall1441")));
	FWoTRetailObjectPath Nested;
	TestTrue(TEXT("nested groups parse and drop the builder suffix"),
		WoTParseRetailObjectPath(TEXT("WOT.WaterEffects.Water.Watr_400#export-00000012"),
			Nested));
	TestEqual(TEXT("nested exact base"), WoTRetailExactLogicalBase(Nested),
		FString(TEXT("WOT.WaterEffects.Water__Watr_400")));
	FWoTRetailObjectPath TopLevel;
	TestTrue(TEXT("top-level path parses"),
		WoTParseRetailObjectPath(TEXT("WarrenT.Rain3"), TopLevel));
	TestEqual(TEXT("a top-level exact base is the plain form"),
		WoTRetailExactLogicalBase(TopLevel), FString(TEXT("WarrenT__Rain3")));
	FWoTRetailObjectPath Rejected;
	TestFalse(TEXT("a bare name is not an object path"),
		WoTParseRetailObjectPath(TEXT("PaprWall1441"), Rejected));
	TestFalse(TEXT("an empty component is rejected"),
		WoTParseRetailObjectPath(TEXT("AesSedaiT..PaprWall1441"), Rejected));

	const TSet<FString> Contract = {
		TEXT("cache/texture/AesSedaiT.Ornament__PaprWall1441"),
		TEXT("cache/texture/AesSedaiT.SurfaceWall__PaprWall1441"),
		TEXT("cache/texture/NatureT__Sky_340a"),
		TEXT("cache/sound/AmbientA.Nature__Rustle3"),
		TEXT("cache/sound/AmbientA.Bush__Rustle3")};
	auto Holds = [&Contract](const FString& LogicalId) { return Contract.Contains(LogicalId); };
	TestEqual(TEXT("a colliding name resolves its exact identity"),
		WoTRetailResolveLogicalBase(EWoTNeutralResourceKind::Texture, Path, Holds),
		FString(TEXT("AesSedaiT.Ornament__PaprWall1441")));
	FWoTRetailObjectPath Unique;
	WoTParseRetailObjectPath(TEXT("Naturet.Sky.Sky_340a"), Unique);
	TestEqual(TEXT("a unique name keeps Pkg__Name (any case spelling)"),
		WoTRetailResolveLogicalBase(EWoTNeutralResourceKind::Texture, Unique, Holds),
		FString(TEXT("Naturet__Sky_340a")));
	FWoTRetailObjectPath Nature;
	WoTParseRetailObjectPath(TEXT("AmbientA.Nature.Rustle3"), Nature);
	TestEqual(TEXT("exact sound sub path"),
		WoTRetailAssetSubPath(EWoTNeutralResourceKind::Sound, Nature, Holds),
		FString(TEXT("AmbientA/Nature/Rustle3")));
	TestEqual(TEXT("unique texture sub path stays flat"),
		WoTRetailAssetSubPath(EWoTNeutralResourceKind::Texture, Unique, Holds),
		FString(TEXT("Naturet/Sky_340a")));
	TestEqual(TEXT("without the authenticated set a sub path is flat"),
		WoTRetailAssetSubPath(EWoTNeutralResourceKind::Sound, Nature,
			[](const FString&) { return false; }),
		FString(TEXT("AmbientA/Rustle3")));

	FString Base;
	TestTrue(TEXT("grouped sub path maps back"),
		WoTRetailLogicalBaseFromSubPath(TEXT("AmbientA/Nature/Rustle3"), Base));
	TestEqual(TEXT("grouped sub path base"), Base, FString(TEXT("AmbientA.Nature__Rustle3")));
	TestTrue(TEXT("a double underscore object name survives"),
		WoTRetailLogicalBaseFromSubPath(TEXT("AmbientA/Dive__2"), Base));
	TestEqual(TEXT("flat base"), Base, FString(TEXT("AmbientA__Dive__2")));
	TestFalse(TEXT("a group may not contain the base separator"),
		WoTRetailLogicalBaseFromSubPath(TEXT("AmbientA/Na__ture/Rustle3"), Base));

	TestTrue(TEXT("FName case: Naturet == NatureT"), WoTRetailSourceExportMatches(
		TEXT("NatureT.Sky.Sky_340a"), TEXT("Naturet.Sky.Sky_340a")));
	TestTrue(TEXT("builder disambiguator is not part of the identity"),
		WoTRetailSourceExportMatches(
			TEXT("ParticleSystems.Fire.Fire_Torch_001#export-00000575"),
			TEXT("ParticleSystems.Fire.Fire_Torch_001")));
	TestFalse(TEXT("another group is another object"), WoTRetailSourceExportMatches(
		TEXT("AesSedaiT.SurfaceWall.PaprWall1441"), TEXT("AesSedaiT.Ornament.PaprWall1441")));
	FWoTRetailObjectPath Escaped;
	TestTrue(TEXT("a group outside the logical-id grammar parses"),
		WoTParseRetailObjectPath(TEXT("FootStubA.(All).METL"), Escaped));
	TestEqual(TEXT("the group is escaped reversibly"), WoTRetailExactLogicalBase(Escaped),
		FString(TEXT("FootStubA.-28All-29__METL")));
	TestEqual(TEXT("a literal dash is escaped too"), WoTRetailEncodeGroup(TEXT("A-B")),
		FString(TEXT("A-2DB")));
	TestTrue(TEXT("the escaped sub path maps back"),
		WoTRetailLogicalBaseFromSubPath(TEXT("FootStubA/(All)/METL"), Base)
		&& Base == TEXT("FootStubA.-28All-29__METL"));
	TestEqual(TEXT("object-name-safe exact base"),
		WoTRetailObjectNameSafe(TEXT("WarrenT.Effects__Rain3")),
		FString(TEXT("WarrenT_Effects__Rain3")));
	TestEqual(TEXT("object-name-safe plain base is unchanged"),
		WoTRetailObjectNameSafe(TEXT("WarrenT__Rain2")), FString(TEXT("WarrenT__Rain2")));

	// The runtime asset gateway maps the exact grouped soft path back to the
	// exact logical id and the flat path to the plain one.
	const TArray<FWoTNeutralResourceId> Resources = {
		{TEXT("cache/sound/AmbientA.Nature__Rustle3"), EWoTNeutralResourceKind::Sound},
		{TEXT("cache/sound/AmbientA.Bush__Rustle3"), EWoTNeutralResourceKind::Sound},
		{TEXT("cache/texture/AesSedaiT.Ornament__PaprWall1441"),
			EWoTNeutralResourceKind::Texture},
		{TEXT("cache/texture/NatureT__Sky_340a"), EWoTNeutralResourceKind::Texture}};
	FString LogicalId;
	TestTrue(TEXT("grouped sound path resolves"), UWoTRuntimeAssets::ResolveCacheId(
		TEXT("/Game/WoT/Sounds/AmbientA/Nature/Rustle3.Rustle3"),
		EWoTRuntimeAssetKind::Sound, Resources, LogicalId));
	TestEqual(TEXT("grouped sound identity"), LogicalId,
		FString(TEXT("cache/sound/AmbientA.Nature__Rustle3")));
	TestTrue(TEXT("grouped texture path resolves"), UWoTRuntimeAssets::ResolveCacheId(
		TEXT("/Game/WoT/Textures/AesSedaiT/Ornament/PaprWall1441.PaprWall1441"),
		EWoTRuntimeAssetKind::Texture, Resources, LogicalId));
	TestEqual(TEXT("grouped texture identity"), LogicalId,
		FString(TEXT("cache/texture/AesSedaiT.Ornament__PaprWall1441")));
	TestFalse(TEXT("the plain alias of a colliding name no longer exists"),
		UWoTRuntimeAssets::ResolveCacheId(TEXT("/Game/WoT/Sounds/AmbientA/Rustle3.Rustle3"),
			EWoTRuntimeAssetKind::Sound, Resources, LogicalId));
	TestTrue(TEXT("a case-variant flat path resolves"), UWoTRuntimeAssets::ResolveCacheId(
		TEXT("/Game/WoT/Textures/Naturet/Sky_340a.Sky_340a"),
		EWoTRuntimeAssetKind::Texture, Resources, LogicalId));
	TestEqual(TEXT("case-variant identity"), LogicalId,
		FString(TEXT("cache/texture/NatureT__Sky_340a")));
	FString Package;
	FString Name;
	TestTrue(TEXT("an exact identity splits at its first separator"),
		UWoTRuntimeAssets::SplitCacheIdentity(TEXT("cache/sound/AmbientA.Nature__Rustle3"),
			EWoTRuntimeAssetKind::Sound, Package, Name));
	TestEqual(TEXT("exact package part"), Package, FString(TEXT("AmbientA.Nature")));
	TestEqual(TEXT("exact name part"), Name, FString(TEXT("Rustle3")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeTextureOptionsSpec,
	"WoT.Assets.TextureOptions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTRuntimeTextureOptionsSpec::RunTest(const FString&)
{
	FWoTRuntimeTextureOptions Default;
	FWoTRuntimeTextureOptions Same;
	TestTrue(TEXT("identical options share a cache identity"), Default == Same);
	Same.bLoadMips = true;
	TestFalse(TEXT("mip policy separates cache identities"), Default == Same);
	Same = Default;
	Same.Filter = TF_Bilinear;
	TestFalse(TEXT("filter separates cache identities"), Default == Same);
	Same = Default;
	Same.LODBias = -1;
	TestFalse(TEXT("LOD bias separates cache identities"), Default == Same);
	Same = Default;
	Same.bSRGB = false;
	TestFalse(TEXT("color-space policy separates cache identities"), Default == Same);
	Same = Default;
	Same.AddressX = TA_Clamp;
	TestFalse(TEXT("addressing separates cache identities"), Default == Same);
	Same = Default;
	Same.bForcePaletteIndexZeroTransparent = true;
	TestFalse(TEXT("forced keyed alpha separates cache identities"), Default == Same);
	const FLinearColor MidKey = UWoTRuntimeAssets::PaletteColorToMaterialColor(
		FColor(128, 64, 32, 0));
	TestTrue(TEXT("palette key color keeps raw non-sRGB byte values"),
		MidKey.Equals(FLinearColor(128.0f / 255.0f, 64.0f / 255.0f,
			32.0f / 255.0f, 1.0f), KINDA_SMALL_NUMBER));

	TestFalse(TEXT("guarded provider is dormant before payload activation"),
		IsWoTPayloadBootstrapActive());
	TestNull(TEXT("dormant missing cooked texture fails normally"),
		UWoTRuntimeAssets::ResolveTexturePath(FSoftObjectPath(
			TEXT("/Game/WoT/Textures/NoPackage/NoTexture.NoTexture"))));

	UGameInstance* CounterOwner = NewObject<UGameInstance>();
	UWoTRuntimeAssets* Counters = NewObject<UWoTRuntimeAssets>(CounterOwner);
	Counters->ResetTravelStatsForTesting();
	Counters->RecordResolutionForTesting(true, false, TEXT("/bad sound"));
	Counters->RecordResolutionForTesting(false, true, TEXT("/good texture"));
	const FString Summary = Counters->TravelSummaryForTesting();
	TestTrue(TEXT("failure is counted instead of crashing"),
		Summary.Contains(TEXT("failed_sounds=1")));
	TestTrue(TEXT("success is counted"),
		Summary.Contains(TEXT("textures=1")));
	TestTrue(TEXT("first failure is bounded into diagnostics"),
		Summary.Contains(TEXT("/bad%20sound")));
	const FWoTRuntimeTextureOptions SpriteOptions =
		UWoTSpriteBillboardComponent::RuntimeTextureOptions();
	TestTrue(TEXT("particle sprite requests source mips"),
		SpriteOptions.bLoadMips);
	TestFalse(TEXT("particle sprite samples in linear space"),
		SpriteOptions.bSRGB);
	TestEqual(TEXT("particle sprite uses the linear master sampler filter"),
		SpriteOptions.Filter, TF_Bilinear);

	FWoTTexturePayloadData Payload;
	Payload.Palette.SetNum(256);
	for (int32 Index = 0; Index < Payload.Palette.Num(); ++Index)
		Payload.Palette[Index] = FColor(
			static_cast<uint8>(Index), static_cast<uint8>(Index + 1),
			static_cast<uint8>(Index + 2), 255);
	Payload.Palette[3].A = 0; // Stored palette alpha is non-semantic.
#if !WOT_GUARDED_BOOTSTRAP
	Payload.BlobBytes = {0, 1, 2, 3, 4};
#endif
	FWoTTexturePayloadMip Base;
	Base.Width = 2; Base.Height = 2; Base.StoredByteLength = 4;
	Base.ExpandedByteLength = 16; Base.DataOffset = 0;
	Payload.Mips.Add(Base);
	FWoTTexturePayloadMip Mip;
	Mip.Width = 1; Mip.Height = 1; Mip.StoredByteLength = 1;
	Mip.ExpandedByteLength = 4; Mip.DataOffset = 4;
	Payload.Mips.Add(Mip);
	FWoTRuntimeTextureOptions MaterialOptions;
	MaterialOptions.bLoadMips = true;
	MaterialOptions.bSRGB = false;
	MaterialOptions.Filter = TF_Bilinear;
	MaterialOptions.LODBias = -1;
	MaterialOptions.AddressX = TA_Clamp;
	MaterialOptions.AddressY = TA_Mirror;
	UTexture2D* Texture = WoTRuntimeMaterializeTexture(
		GetTransientPackage(), Payload, MaterialOptions);
	if (TestNotNull(TEXT("header-shaped texture materializes"), Texture))
	{
		TestEqual(TEXT("header width is retained"), Texture->GetSizeX(), 2);
		TestEqual(TEXT("header height is retained"), Texture->GetSizeY(), 2);
		TestEqual(TEXT("requested source mips are retained"),
			Texture->GetPlatformData()->Mips.Num(), 2);
		TestFalse(TEXT("requested linear color is retained"), Texture->SRGB != 0);
		TestEqual(TEXT("requested filter is retained"), Texture->Filter, TF_Bilinear);
		TestEqual(TEXT("requested LOD bias is retained"), Texture->LODBias, -1);
		TestEqual(TEXT("requested X addressing is retained"), Texture->AddressX, TA_Clamp);
		TestEqual(TEXT("requested Y addressing is retained"), Texture->AddressY, TA_Mirror);
		FTexture2DMipMap& BaseMip = Texture->GetPlatformData()->Mips[0];
		const FColor* Pixels = reinterpret_cast<const FColor*>(
			BaseMip.BulkData.LockReadOnly());
		if (TestNotNull(TEXT("materialized pixels are readable"), Pixels))
		{
			TestEqual(TEXT("palette index zero expands exactly"), Pixels[0], Payload.Palette[0]);
			TestEqual(TEXT("unmasked palette alpha is forced opaque"), Pixels[3],
				FColor(Payload.Palette[3].R, Payload.Palette[3].G,
					Payload.Palette[3].B, 255));
		}
		BaseMip.BulkData.Unlock();
		Texture->ReleaseResource();
		Texture->MarkAsGarbage();
	}
	Payload.bMasked = false;
	MaterialOptions.bForcePaletteIndexZeroTransparent = true;
	Texture = WoTRuntimeMaterializeTexture(
		GetTransientPackage(), Payload, MaterialOptions);
	if (TestNotNull(TEXT("mesh masked override materializes"), Texture))
	{
		FTexture2DMipMap& BaseMip = Texture->GetPlatformData()->Mips[0];
		const FColor* Pixels = reinterpret_cast<const FColor*>(
			BaseMip.BulkData.LockReadOnly());
		if (TestNotNull(TEXT("forced keyed pixels are readable"), Pixels))
		{
			TestEqual(TEXT("masked mesh forces palette index zero alpha"),
				Pixels[0].A, static_cast<uint8>(0));
			TestEqual(TEXT("masked mesh leaves non-key pixels opaque"),
				Pixels[1].A, static_cast<uint8>(255));
		}
		BaseMip.BulkData.Unlock();
		Texture->ReleaseResource();
		Texture->MarkAsGarbage();
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeAssetReleaseLifetimeSpec,
	"WoT.Assets.ReleasePreservesConsumersAndHeartbeat",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTRuntimeAssetReleaseLifetimeSpec::RunTest(const FString&)
{
	UGameInstance* LifetimeOwner = NewObject<UGameInstance>();
	UWoTRuntimeAssets* Assets = NewObject<UWoTRuntimeAssets>(LifetimeOwner);
	UWoTPayloadSoundWave* ConsumerWave = NewObject<UWoTPayloadSoundWave>();
	UTexture2D* ConsumerTexture = UTexture2D::CreateTransient(1, 1, PF_B8G8R8A8);
	Assets->RuntimeSounds.Add(ConsumerWave);
	Assets->RuntimeTextures.Add(ConsumerTexture);
	Assets->SoundCache.Add(TEXT("consumer-wave"), ConsumerWave);
	Assets->TextureCache.Add(TEXT("consumer-texture"), ConsumerTexture);
	Assets->ShutdownForPayloadDeactivation();
	TestTrue(TEXT("consumer-held wave remains a valid UObject after provider release"),
		IsValid(ConsumerWave));
	TestTrue(TEXT("consumer-held texture remains a valid UObject after provider release"),
		IsValid(ConsumerTexture));
	TestNotNull(TEXT("consumer-held texture keeps its platform pixels"),
		ConsumerTexture->GetPlatformData());
	TestEqual(TEXT("provider sound roots are dropped"), Assets->SoundCache.Num(), 0);
	TestEqual(TEXT("provider texture roots are dropped"), Assets->TextureCache.Num(), 0);

	UGameInstance* HeartbeatOwner = NewObject<UGameInstance>();
	Assets = NewObject<UWoTRuntimeAssets>(HeartbeatOwner);
	Assets->RecordMaterialFailure(
		TEXT("/Game/WoT/Materials/M_Missing.M_Missing"));
	const FString MaterialSummary = Assets->TravelSummary();
	TestTrue(TEXT("material failure has its own count"),
		MaterialSummary.Contains(TEXT("failed_materials=1")));
	TestTrue(TEXT("material path is retained in bounded diagnostics"),
		MaterialSummary.Contains(TEXT("M_Missing.M_Missing")));
	Assets->bTravelSummaryPending = true;
	TestFalse(TEXT("heartbeat clock does not start before BeginPlay"),
		Assets->AdvanceTravelSummaryClock(false, 100.0));
	TestFalse(TEXT("BeginPlay starts rather than fires the five-second clock"),
		Assets->AdvanceTravelSummaryClock(true, 100.0));
	TestFalse(TEXT("heartbeat stays pending before five real seconds"),
		Assets->AdvanceTravelSummaryClock(true, 104.999));
	TestTrue(TEXT("heartbeat fires at five real seconds"),
		Assets->AdvanceTravelSummaryClock(true, 105.0));
	TestFalse(TEXT("heartbeat is bounded to one delayed emission"),
		Assets->AdvanceTravelSummaryClock(true, 110.0));

	UWorld* SummaryWorld = NewObject<UWorld>();
	Assets->TravelWorld = SummaryWorld;
	Assets->bTravelSummaryPending = true;
	Assets->ShutdownForPayloadDeactivation();
	TestEqual(TEXT("shutdown before cleanup emits one final summary"),
		Assets->FinalTravelSummaryEmissionsForTesting, 1);
	Assets->HandleWorldCleanup(SummaryWorld, true, true);
	Assets->ShutdownForPayloadDeactivation();
	TestEqual(TEXT("cleanup and repeated shutdown cannot duplicate the final summary"),
		Assets->FinalTravelSummaryEmissionsForTesting, 1);

	UGameInstance* CleanupFirstOwner = NewObject<UGameInstance>();
	UWoTRuntimeAssets* CleanupFirst =
		NewObject<UWoTRuntimeAssets>(CleanupFirstOwner);
	UWorld* CleanupFirstWorld = NewObject<UWorld>();
	CleanupFirst->TravelWorld = CleanupFirstWorld;
	CleanupFirst->bTravelSummaryPending = true;
	CleanupFirst->HandleWorldCleanup(CleanupFirstWorld, false, true);
	CleanupFirst->ShutdownForPayloadDeactivation();
	TestEqual(TEXT("cleanup before shutdown emits one final summary"),
		CleanupFirst->FinalTravelSummaryEmissionsForTesting, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeAssetProviderOwnershipSpec,
	"WoT.Assets.AuthenticatedProviderOwnership",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTRuntimeAssetProviderOwnershipSpec::RunTest(const FString&)
{
	UWoTRuntimeAssets::ResetProvidersForTesting();
	UGameInstance* AuthenticatedGameInstance = NewObject<UGameInstance>();
	UWoTPayloadSubsystem* AuthenticatedPayload =
		NewObject<UWoTPayloadSubsystem>(AuthenticatedGameInstance);
	UWoTRuntimeAssets* AuthenticatedAssets =
		NewObject<UWoTRuntimeAssets>(AuthenticatedGameInstance);
	UGameInstance* TransientGameInstance = NewObject<UGameInstance>();
	UWoTRuntimeAssets* TransientAssets =
		NewObject<UWoTRuntimeAssets>(TransientGameInstance);

	UWoTRuntimeAssets::RegisterProviderForTesting(AuthenticatedAssets);
	UWoTRuntimeAssets::RegisterProviderForTesting(TransientAssets);
	TestEqual(TEXT("developer mode initially selects the newest provider"),
		UWoTRuntimeAssets::ProviderForTesting(), TransientAssets);
	UWoTRuntimeAssets::RefreshProviderBindingForTesting(AuthenticatedPayload);
	TestEqual(TEXT("a transient game instance cannot take authenticated routing"),
		UWoTRuntimeAssets::ProviderForTesting(), AuthenticatedAssets);
	UWoTRuntimeAssets::RemoveProviderForTesting(TransientAssets);
	UWoTRuntimeAssets::RefreshProviderBindingForTesting(AuthenticatedPayload);
	TestEqual(TEXT("authenticated routing remains stable after transient teardown"),
		UWoTRuntimeAssets::ProviderForTesting(), AuthenticatedAssets);
	UWoTRuntimeAssets::RegisterProviderForTesting(TransientAssets);
	UWoTRuntimeAssets::RefreshProviderBindingForTesting(AuthenticatedPayload);
	UWoTRuntimeAssets::RemoveProviderForTesting(AuthenticatedAssets);
	UWoTRuntimeAssets::RefreshProviderBindingForTesting(nullptr);
	TestEqual(TEXT("provider routing restores the surviving instance after owner teardown"),
		UWoTRuntimeAssets::ProviderForTesting(), TransientAssets);
	UWoTRuntimeAssets::ResetProvidersForTesting();
	return true;
}

#endif
