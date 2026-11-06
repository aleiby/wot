#include "Misc/AutomationTest.h"

#include "../RetailData/WoTRetailBuildContext.h"
#include "../RetailData/WoTRetailFamilies.h"
#include "../RetailData/WoTRetailFamilyPins.h"
#if !WOT_GUARDED_BOOTSTRAP
#include "../RetailData/WoTDevRetailFamilies.h"
#endif
#include "../World/WoTRuntimeClassDefaults.h"

#if WITH_AUTOMATION_TESTS

#include "WoTPayloadActivationTestSupport.h"
#include "WoTTestWorld.h"

#include "../Payload/WoTPayloadActivation.h"
#include "../WoTGameInstance.h"
#include "../WoTPayloadSubsystem.h"

#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "WoTBootstrapCore/Hash.h"
#include "WoTRetailReader/DevelopmentClassDefaults.h"
#include "WoTRetailReader/Testing/ReferencePaths.h"

namespace
{
constexpr EAutomationTestFlags WoTRetailDataSpecFlags =
	EAutomationTestFlags::EditorContext
	| EAutomationTestFlags::EngineFilter;

FWoTRetailFamily WoTRetailDataFakeFamily(
	const TCHAR* Name, const TCHAR* Digest, int32& Builds, int32& Installs,
	bool bBuildSucceeds = true)
{
	return FWoTRetailFamily{
		Name,
		[Name, Digest, &Builds, &Installs, bBuildSucceeds](
			const FWoTRetailBuildContext&, FString& OutDigest,
			TFunction<void()>& OutInstall, FString& OutError)
		{
			++Builds;
			if (!bBuildSucceeds)
			{
				OutError = TEXT("synthetic build failure");
				return false;
			}
			OutDigest = Digest;
			OutInstall = [&Installs]() { ++Installs; };
			return true;
		}};
}

FWoTRetailBuildContext WoTRetailDataEmptyContext(
	const FWoTRuntimeClassDefaults& Defaults)
{
	return FWoTRetailBuildContext{
		Defaults,
		[](FStringView, FWoTRetailTextureFacts&) { return false; },
		[](FStringView, TArray<FWoTVertexMeshSequence>&) { return false; }};
}

struct FWoTRetailDataActivationScope
{
	FWoTRetailDataActivationScope()
	{
		WoTRetailFamiliesResetActivationForTesting();
	}
	~FWoTRetailDataActivationScope()
	{
		WoTRetailFamiliesResetActivationForTesting();
	}
};

struct FWoTRetailDataCacheRoot
{
	FWoTRetailDataCacheRoot()
	{
		Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::AutomationTransientDir(),
			TEXT("wot-retail-data-")
				+ FGuid::NewGuid().ToString(EGuidFormats::Digits),
			TEXT("ContentCache")));
		IFileManager::Get().MakeDirectory(*Path, true);
	}
	~FWoTRetailDataCacheRoot()
	{
		IFileManager::Get().DeleteDirectory(*FPaths::GetPath(Path), false, true);
	}
	FString Path;
};

struct FWoTRetailDataReferenceRootScope
{
	explicit FWoTRetailDataReferenceRootScope(const FString& Root)
		: Previous(FPlatformMisc::GetEnvironmentVariable(
			TEXT("WOT_REFERENCE_ROOT")))
	{
		FPlatformMisc::SetEnvironmentVar(TEXT("WOT_REFERENCE_ROOT"), *Root);
	}
	~FWoTRetailDataReferenceRootScope()
	{
		FPlatformMisc::SetEnvironmentVar(
			TEXT("WOT_REFERENCE_ROOT"), *Previous);
	}
	FString Previous;
};

TArray<uint8> WoTRetailDataEmptyClassDefaultObject()
{
	const uint8 PayloadBytes[] = {
		'W', 'O', 'T', 'D', 'E', 'F', '3', 0,
		3, 0, 0, 0,
		0, 0, 0, 0};
	const FString Header = FString::Printf(
		TEXT("{\"class_count\":0,\"default_count\":0,\"encoding\":\"ue1-class-defaults-v2\",\"fact_id\":\"spec.empty-class-defaults\",\"importer\":\"class-default-v2\",\"importer_version\":2,\"kind\":\"class_default\",\"payload_byte_length\":%d,\"reference_count\":0,\"source\":{\"export\":\"Spec.Empty\",\"package\":\"System/Spec.u\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}}"),
		UE_ARRAY_COUNT(PayloadBytes));
	return WoTPayloadActivationTest_FrameObject(
		Header, MakeArrayView(PayloadBytes));
}

TArray<FWoTPayloadActivationTestObject>
WoTRetailDataClassDefaultObjects()
{
	static const TCHAR* LogicalIds[] = {
		TEXT("cache/class_default/Angreal"),
		TEXT("cache/class_default/Core"),
		TEXT("cache/class_default/Editor"),
		TEXT("cache/class_default/Engine"),
		TEXT("cache/class_default/Environment"),
		TEXT("cache/class_default/Fire"),
		TEXT("cache/class_default/IpDrv"),
		TEXT("cache/class_default/IpServer"),
		TEXT("cache/class_default/Legend"),
		TEXT("cache/class_default/MachinShin"),
		TEXT("cache/class_default/ParticleSystems"),
		TEXT("cache/class_default/UBrowser"),
		TEXT("cache/class_default/UWindow"),
		TEXT("cache/class_default/WinDrv"),
		TEXT("cache/class_default/WOT"),
		TEXT("cache/class_default/WoTBase"),
		TEXT("cache/class_default/WoTBrowser"),
		TEXT("cache/class_default/WoTDecorations"),
		TEXT("cache/class_default/WoTPawns"),
		TEXT("cache/class_default/WoTTraps")};
	const TArray<uint8> ObjectBytes =
		WoTRetailDataEmptyClassDefaultObject();
	TArray<FWoTPayloadActivationTestObject> Objects;
	for (const TCHAR* LogicalId : LogicalIds)
	{
		FWoTPayloadActivationTestObject& Object = Objects.AddDefaulted_GetRef();
		Object.LogicalId = LogicalId;
		Object.Schema = TEXT("wot-class-default-v3");
		Object.Bytes = ObjectBytes;
	}
	return Objects;
}

bool WoTRetailDataLoadFixture(const TCHAR* Name, TArray<uint8>& Out)
{
	return FFileHelper::LoadFileToArray(Out, *WoTRepositoryOrReferencePath(
		*(FString(TEXT("Tools/dist/fixtures/")) + Name)));
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailDataAllFamiliesInstallSpec,
	"WoT.RetailData.AllFamiliesInstallAfterValidation",
	WoTRetailDataSpecFlags)

bool FWoTRetailDataAllFamiliesInstallSpec::RunTest(const FString&)
{
	FWoTRetailDataActivationScope Scope;
	FWoTRuntimeClassDefaults Defaults;
	int32 Builds = 0;
	int32 Installs = 0;
	TArray<FWoTRetailFamily> Families;
	Families.Add(WoTRetailDataFakeFamily(
		TEXT("Alpha"), TEXT("aaaaaaaa"), Builds, Installs));
	Families.Add(WoTRetailDataFakeFamily(
		TEXT("Beta"), TEXT("bbbbbbbb"), Builds, Installs));
	const TMap<FString, FString> Pins = {
		{TEXT("Alpha"), TEXT("aaaaaaaa")},
		{TEXT("Beta"), TEXT("bbbbbbbb")}};
	FString Error;
	TestTrue(TEXT("all valid families activate"),
		WoTActivateRetailFamilies(
			WoTRetailDataEmptyContext(Defaults), Families, Pins, Error));
	TestEqual(TEXT("every family builds"), Builds, 2);
	TestEqual(TEXT("every family installs"), Installs, 2);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailDataLookupMemoizationSpec,
	"WoT.RetailData.LookupMemoization",
	WoTRetailDataSpecFlags)

bool FWoTRetailDataLookupMemoizationSpec::RunTest(const FString&)
{
	FWoTRetailDataActivationScope Scope;
	FWoTRuntimeClassDefaults Defaults;
	int32 TextureDecodes = 0;
	int32 MeshDecodes = 0;
	int32 Installs = 0;
	FWoTRetailBuildContext Context{
		Defaults,
		[&TextureDecodes](FStringView Identity, FWoTRetailTextureFacts& Out)
		{
			++TextureDecodes;
			if (!Identity.Equals(TEXT("Spec.Textures.Known"),
				ESearchCase::IgnoreCase))
			{
				return false;
			}
			Out = {64, 32, 0};
			return true;
		},
		[&MeshDecodes](FStringView Identity,
			TArray<FWoTVertexMeshSequence>& Out)
		{
			++MeshDecodes;
			if (!Identity.Equals(TEXT("Spec.Meshes.Known"),
				ESearchCase::IgnoreCase))
			{
				return false;
			}
			FWoTVertexMeshSequence& Sequence = Out.AddDefaulted_GetRef();
			Sequence.Name = TEXT("Move");
			return true;
		}};
	TArray<FWoTRetailFamily> Families;
	Families.Add(FWoTRetailFamily{
		TEXT("Lookup"),
		[&Installs](const FWoTRetailBuildContext& BuildContext,
			FString& OutDigest, TFunction<void()>& OutInstall,
			FString& OutError)
		{
			FWoTRetailTextureFacts FirstTexture;
			FWoTRetailTextureFacts SecondTexture;
			TArray<FWoTVertexMeshSequence> FirstSequences;
			TArray<FWoTVertexMeshSequence> SecondSequences;
			if (!BuildContext.FindTexture(
				TEXT("Spec.Textures.Known"), FirstTexture)
				|| !BuildContext.FindTexture(
					TEXT("spec.textures.known"), SecondTexture)
				|| !BuildContext.FindMeshSequences(
					TEXT("Spec.Meshes.Known"), FirstSequences)
				|| !BuildContext.FindMeshSequences(
					TEXT("spec.meshes.known"), SecondSequences)
				|| FirstTexture.USize != 64 || SecondTexture.VSize != 32
				|| FirstSequences.Num() != 1 || SecondSequences.Num() != 1)
			{
				OutError = TEXT("the fake retail lookups did not return their facts");
				return false;
			}
			OutDigest = TEXT("lookup-digest");
			OutInstall = [&Installs]() { ++Installs; };
			return true;
		}});
	FString Error;
	TestTrue(TEXT("fake lookup facts drive a family build"),
		WoTActivateRetailFamilies(Context, Families,
			{{TEXT("Lookup"), TEXT("lookup-digest")}}, Error));
	TestEqual(TEXT("texture identity decodes once"), TextureDecodes, 1);
	TestEqual(TEXT("mesh identity decodes once"), MeshDecodes, 1);
	TestEqual(TEXT("the lookup family installs"), Installs, 1);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailDataUnknownLookupFailsSpec,
	"WoT.RetailData.UnknownLookupFailsActivation",
	WoTRetailDataSpecFlags)

bool FWoTRetailDataUnknownLookupFailsSpec::RunTest(const FString&)
{
	FWoTRetailDataActivationScope Scope;
	FWoTRuntimeClassDefaults Defaults;
	int32 Installs = 0;
	TArray<FWoTRetailFamily> Families;
	Families.Add(FWoTRetailFamily{
		TEXT("UnknownLookup"),
		[&Installs](const FWoTRetailBuildContext& Context,
			FString&, TFunction<void()>& OutInstall, FString& OutError)
		{
			FWoTRetailTextureFacts Facts;
			if (!Context.FindTexture(TEXT("Spec.Textures.Unknown"), Facts))
			{
				OutError = TEXT("unknown texture identity");
				return false;
			}
			OutInstall = [&Installs]() { ++Installs; };
			return true;
		}});
	FString Error;
	TestFalse(TEXT("an unknown identity fails family activation"),
		WoTActivateRetailFamilies(WoTRetailDataEmptyContext(Defaults),
			Families, {{TEXT("UnknownLookup"), TEXT("unused")}}, Error));
	TestTrue(TEXT("the failed lookup is diagnosed"),
		Error.Contains(TEXT("unknown texture identity")));
	TestEqual(TEXT("a failed lookup installs nothing"), Installs, 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailDataBuildFailureIsAtomicSpec,
	"WoT.RetailData.BuildFailureInstallsNothing",
	WoTRetailDataSpecFlags)

bool FWoTRetailDataBuildFailureIsAtomicSpec::RunTest(const FString&)
{
	FWoTRetailDataActivationScope Scope;
	FWoTRuntimeClassDefaults Defaults;
	int32 Builds = 0;
	int32 Installs = 0;
	TArray<FWoTRetailFamily> Families;
	Families.Add(WoTRetailDataFakeFamily(
		TEXT("Alpha"), TEXT("aaaaaaaa"), Builds, Installs));
	Families.Add(WoTRetailDataFakeFamily(
		TEXT("Broken"), TEXT("bbbbbbbb"), Builds, Installs, false));
	const TMap<FString, FString> Pins = {
		{TEXT("Alpha"), TEXT("aaaaaaaa")},
		{TEXT("Broken"), TEXT("bbbbbbbb")}};
	FString Error;
	TestFalse(TEXT("a build failure rejects activation"),
		WoTActivateRetailFamilies(
			WoTRetailDataEmptyContext(Defaults), Families, Pins, Error));
	TestEqual(TEXT("the failing family is named"),
		Error.Contains(TEXT("Broken")), true);
	TestEqual(TEXT("nothing installs after a build failure"), Installs, 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailDataDigestMismatchIsAtomicSpec,
	"WoT.RetailData.DigestMismatchInstallsNothing",
	WoTRetailDataSpecFlags)

bool FWoTRetailDataDigestMismatchIsAtomicSpec::RunTest(const FString&)
{
	FWoTRetailDataActivationScope Scope;
	FWoTRuntimeClassDefaults Defaults;
	int32 Builds = 0;
	int32 Installs = 0;
	TArray<FWoTRetailFamily> Families;
	Families.Add(WoTRetailDataFakeFamily(
		TEXT("Alpha"), TEXT("observed-alpha"), Builds, Installs));
	Families.Add(WoTRetailDataFakeFamily(
		TEXT("Beta"), TEXT("observed-beta"), Builds, Installs));
	const TMap<FString, FString> Pins = {
		{TEXT("Alpha"), TEXT("observed-alpha")},
		{TEXT("Beta"), TEXT("expected-beta")}};
	FString Error;
	TestFalse(TEXT("a digest mismatch rejects activation"),
		WoTActivateRetailFamilies(
			WoTRetailDataEmptyContext(Defaults), Families, Pins, Error));
	TestTrue(TEXT("the mismatched family is named"),
		Error.Contains(TEXT("Beta")));
	TestTrue(TEXT("the expected digest is named"),
		Error.Contains(TEXT("expected-beta")));
	TestTrue(TEXT("the observed digest is named"),
		Error.Contains(TEXT("observed-beta")));
	TestEqual(TEXT("nothing installs after a digest mismatch"), Installs, 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailDataMissingPinSpec,
	"WoT.RetailData.MissingPinFails",
	WoTRetailDataSpecFlags)

bool FWoTRetailDataMissingPinSpec::RunTest(const FString&)
{
	FWoTRetailDataActivationScope Scope;
	FWoTRuntimeClassDefaults Defaults;
	int32 Builds = 0;
	int32 Installs = 0;
	TArray<FWoTRetailFamily> Families;
	Families.Add(WoTRetailDataFakeFamily(
		TEXT("Unpinned"), TEXT("observed"), Builds, Installs));
	FString Error;
	TestFalse(TEXT("an unpinned family rejects activation"),
		WoTActivateRetailFamilies(
			WoTRetailDataEmptyContext(Defaults), Families,
			TMap<FString, FString>(), Error));
	TestTrue(TEXT("the unpinned family is named"),
		Error.Contains(TEXT("Unpinned")));
	TestTrue(TEXT("the unpinned family's observed digest is reported"),
		Error.Contains(TEXT("observed")));
	TestEqual(TEXT("an unpinned family does not install"), Installs, 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailDataActivationRunsOnceSpec,
	"WoT.RetailData.ActivationRunsOnce",
	WoTRetailDataSpecFlags)

bool FWoTRetailDataActivationRunsOnceSpec::RunTest(const FString&)
{
	FWoTRetailDataActivationScope Scope;
	FWoTRuntimeClassDefaults Defaults;
	int32 Builds = 0;
	int32 Installs = 0;
	TArray<FWoTRetailFamily> Families;
	Families.Add(WoTRetailDataFakeFamily(
		TEXT("Only"), TEXT("digest"), Builds, Installs));
	const TMap<FString, FString> Pins = {{TEXT("Only"), TEXT("digest")}};
	FString FirstError;
	FString SecondError;
	TestTrue(TEXT("the first activation succeeds"),
		WoTActivateRetailFamilies(
			WoTRetailDataEmptyContext(Defaults), Families, Pins, FirstError));
	TestTrue(TEXT("the repeated activation reports the stored success"),
		WoTActivateRetailFamilies(
			WoTRetailDataEmptyContext(Defaults), Families, Pins, SecondError));
	TestEqual(TEXT("the family builds once"), Builds, 1);
	TestEqual(TEXT("the family installs once"), Installs, 1);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailDataProductionListRequiresInputsSpec,
	"WoT.RetailData.ProductionListRequiresInputs",
	WoTRetailDataSpecFlags)

bool FWoTRetailDataProductionListRequiresInputsSpec::RunTest(const FString&)
{
	FWoTRetailDataActivationScope Scope;
	FWoTRuntimeClassDefaults Defaults;
	FString Error;
	TestFalse(TEXT("the production list rejects an empty build context"),
		WoTActivateRetailFamilies(
			WoTRetailDataEmptyContext(Defaults), WoTRetailFamilies(),
			WoTRetailFamilyPins(), Error));
	TestEqual(TEXT("the production family list contains every migrated family"),
		WoTRetailFamilies().Num(), 12);
	TestFalse(TEXT("the failed activation explains the missing input"),
		Error.IsEmpty());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailDataGuardedActivationFailureSpec,
	"WoT.RetailData.GuardedActivationFailurePropagates",
	WoTRetailDataSpecFlags)

bool FWoTRetailDataGuardedActivationFailureSpec::RunTest(const FString&)
{
	FWoTRetailDataActivationScope ActivationScope;
	ResetWoTPayloadActivationForTesting();
	UWoTPayloadSubsystem::SetInjectionEnabledOverrideForTests(
		/*bHasOverride*/ true, /*bEnabled*/ false);

	int32 Builds = 0;
	int32 Installs = 0;
	TArray<FWoTRetailFamily> Families;
	Families.Add(WoTRetailDataFakeFamily(
		TEXT("InjectedFailure"), TEXT("unused"), Builds, Installs, false));
	TMap<FString, FString> Pins = {
		{TEXT("InjectedFailure"), TEXT("unused")}};
	WoTRetailFamiliesSetActivationInputsForTesting(
		MoveTemp(Families), MoveTemp(Pins));

	FWoTRetailDataCacheRoot Root;
	FString Error;
	if (!TestTrue(TEXT("the class-default generation publishes"),
		WoTPayloadActivationTest_PublishObjects(
			Root.Path, WoTRetailDataClassDefaultObjects(), Error)))
	{
		AddError(Error);
		UWoTPayloadSubsystem::SetInjectionEnabledOverrideForTests(false);
		ResetWoTPayloadActivationForTesting();
		return false;
	}

	WoTTest::FSpecWorldOptions Options;
	Options.GameInstanceClass = UWoTGameInstance::StaticClass();
	Options.bStartPlay = false;
	Options.bActivatePayload = false;
	UWorld* World = WoTTest::CreateSpecWorld(Options);
	UWoTPayloadSubsystem* Payloads = World != nullptr
		? World->GetGameInstance()->GetSubsystem<UWoTPayloadSubsystem>()
		: nullptr;
	if (TestNotNull(TEXT("the payload subsystem exists"), Payloads))
	{
		TestFalse(TEXT("the injected family failure propagates"),
			WoTPayloadActivationTest_ActivatePublished(
				Payloads, Root.Path, Error));
		TestTrue(TEXT("the activation error names the family"),
			Error.Contains(TEXT("InjectedFailure")));
		TestTrue(TEXT("the generation remains dormant"),
			Payloads->IsDormant());
		TestFalse(TEXT("the payload activation is not published"),
			IsWoTPayloadBootstrapActive());
		TestEqual(TEXT("the family was built once"), Builds, 1);
		TestEqual(TEXT("the failed family was never installed"), Installs, 0);
	}
	WoTTest::DestroySpecWorld(World);
	UWoTPayloadSubsystem::SetInjectionEnabledOverrideForTests(false);
	ResetWoTPayloadActivationForTesting();
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailDataGuardedBuildContextSpec,
	"WoT.RetailData.GuardedBuildContext",
	WoTRetailDataSpecFlags)

bool FWoTRetailDataGuardedBuildContextSpec::RunTest(const FString&)
{
	FWoTRetailDataActivationScope ActivationScope;
	ResetWoTPayloadActivationForTesting();
	UWoTPayloadSubsystem::SetInjectionEnabledOverrideForTests(
		/*bHasOverride*/ true, /*bEnabled*/ false);

	TArray<uint8> TextureBytes;
	TArray<uint8> MeshBytes;
	TArray<uint8> AnimationBytes;
	if (!TestTrue(TEXT("the texture fixture loads"),
		WoTRetailDataLoadFixture(
			TEXT("cache-v3-texture-opaque.bin"), TextureBytes))
		|| !TestTrue(TEXT("the mesh fixture loads"),
			WoTRetailDataLoadFixture(
				TEXT("cache-v3-mesh-plain.bin"), MeshBytes))
		|| !TestTrue(TEXT("the animation fixture loads"),
			WoTRetailDataLoadFixture(
				TEXT("cache-v3-animation-plain.bin"), AnimationBytes)))
	{
		UWoTPayloadSubsystem::SetInjectionEnabledOverrideForTests(false);
		ResetWoTPayloadActivationForTesting();
		return false;
	}

	bool bSawTexture = false;
	bool bSawSequences = false;
	int32 Installs = 0;
	TArray<FWoTRetailFamily> Families;
	Families.Add(FWoTRetailFamily{
		TEXT("GuardedLookup"),
		[&bSawTexture, &bSawSequences, &Installs](
			const FWoTRetailBuildContext& Context, FString& OutDigest,
			TFunction<void()>& OutInstall, FString& OutError)
		{
			FWoTRetailTextureFacts Facts;
			TArray<FWoTVertexMeshSequence> Sequences;
			bSawTexture = Context.FindTexture(
				TEXT("texture.SurfaceFloor.Stone"), Facts)
				&& Facts.USize == 4 && Facts.VSize == 1 && Facts.Format == 0;
			bSawSequences = Context.FindMeshSequences(
				TEXT("mesh.PlainMesh"), Sequences)
				&& Sequences.Num() == 2
				&& Sequences.ContainsByPredicate(
					[](const FWoTVertexMeshSequence& Sequence)
					{
						return !Sequence.Notifies.IsEmpty();
					});
			if (!bSawTexture || !bSawSequences)
			{
				OutError = TEXT("authenticated fixture facts were unavailable");
				return false;
			}
			OutDigest = TEXT("guarded-lookup-digest");
			OutInstall = [&Installs]() { ++Installs; };
			return true;
		}});
	WoTRetailFamiliesSetActivationInputsForTesting(
		MoveTemp(Families),
		{{TEXT("GuardedLookup"), TEXT("guarded-lookup-digest")}});

	TArray<FWoTPayloadActivationTestObject> Objects =
		WoTRetailDataClassDefaultObjects();
	Objects.Add({TEXT("cache/texture/texture.SurfaceFloor__Stone"),
		TEXT("wot-texture-v3"), MoveTemp(TextureBytes)});
	Objects.Add({TEXT("cache/mesh/mesh__PlainMesh"),
		TEXT("wot-mesh-v3"), MoveTemp(MeshBytes)});
	Objects.Add({TEXT("cache/animation/mesh__PlainMesh"),
		TEXT("wot-animation-v3"), MoveTemp(AnimationBytes)});
	FWoTRetailDataCacheRoot Root;
	FString Error;
	if (!TestTrue(TEXT("the authenticated fixture generation publishes"),
		WoTPayloadActivationTest_PublishObjects(Root.Path, Objects, Error)))
	{
		AddError(Error);
		UWoTPayloadSubsystem::SetInjectionEnabledOverrideForTests(false);
		ResetWoTPayloadActivationForTesting();
		return false;
	}

	WoTTest::FSpecWorldOptions Options;
	Options.GameInstanceClass = UWoTGameInstance::StaticClass();
	Options.bStartPlay = false;
	Options.bActivatePayload = false;
	UWorld* World = WoTTest::CreateSpecWorld(Options);
	UWoTPayloadSubsystem* Payloads = World != nullptr
		? World->GetGameInstance()->GetSubsystem<UWoTPayloadSubsystem>()
		: nullptr;
	if (TestNotNull(TEXT("the payload subsystem exists"), Payloads))
	{
		TestTrue(TEXT("the fixture generation activates"),
			WoTPayloadActivationTest_ActivatePublished(
				Payloads, Root.Path, Error));
		if (!Error.IsEmpty())
		{
			AddError(Error);
		}
		TestTrue(TEXT("the texture lookup returned mip-zero facts"),
			bSawTexture);
		TestTrue(TEXT("the mesh lookup returned sequence notifies"),
			bSawSequences);
		TestEqual(TEXT("the guarded lookup family installed"), Installs, 1);
		const TSharedPtr<const FWoTTexturePayloadData> AfterActivation =
			Payloads->LoadTexturePayload(
				TEXT("cache/texture/texture.SurfaceFloor__Stone"));
		TestTrue(TEXT("a texture the build context read keeps its bytes for later consumers"),
			AfterActivation.IsValid() && !AfterActivation->PayloadBytes().IsEmpty());
	}
	WoTTest::DestroySpecWorld(World);
	UWoTPayloadSubsystem::SetInjectionEnabledOverrideForTests(false);
	ResetWoTPayloadActivationForTesting();
	return !HasAnyErrors();
}

#if !WOT_GUARDED_BOOTSTRAP

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailDataDevCacheFingerprintSpec,
	"WoT.RetailData.DevCacheFingerprintTracksDependencies",
	WoTRetailDataSpecFlags)

bool FWoTRetailDataDevCacheFingerprintSpec::RunTest(const FString&)
{
	TMap<FString, TArray<uint8>> Inputs;
	Inputs.Add(TEXT("System/Consumer.u"), {1, 2, 3});
	Inputs.Add(TEXT("System/Dependency.u"), {4, 5, 6});
	const FString Before =
		WoTDevelopmentClassDefaultInputSetDigestForTesting(Inputs);
	Inputs[TEXT("System/Dependency.u")][1] = 9;
	const FString After =
		WoTDevelopmentClassDefaultInputSetDigestForTesting(Inputs);
	TestNotEqual(TEXT("a dependency-only edit changes the cache namespace"),
		Before, After);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailDataDevMissingRootSpec,
	"WoT.RetailData.DevMissingReferenceRootWarnsOnce",
	WoTRetailDataSpecFlags)

bool FWoTRetailDataDevMissingRootSpec::RunTest(const FString&)
{
	FWoTRetailDataActivationScope ActivationScope;
	WoTDevRetailFamiliesResetForTesting();
	FWoTRetailDataCacheRoot EmptyRoot;
	FWoTRetailDataReferenceRootScope ReferenceRoot(EmptyRoot.Path);
	int32 Builds = 0;
	int32 Installs = 0;
	TArray<FWoTRetailFamily> Families;
	Families.Add(WoTRetailDataFakeFamily(
		TEXT("MustStayUnbuilt"), TEXT("digest"), Builds, Installs));
	TMap<FString, FString> Pins = {
		{TEXT("MustStayUnbuilt"), TEXT("digest")}};
	WoTRetailFamiliesSetActivationInputsForTesting(
		MoveTemp(Families), MoveTemp(Pins));
	AddExpectedError(
		TEXT("WOT_REFERENCE_ROOT"),
		EAutomationExpectedErrorFlags::Contains, 1);
	WoTDevRetailFamiliesEnsureActivated();
	WoTDevRetailFamiliesEnsureActivated();
	TestEqual(TEXT("a missing root builds no family"), Builds, 0);
	TestEqual(TEXT("a missing root installs no family"), Installs, 0);
	TestFalse(TEXT("a missing root records no successful decode"),
		WoTDevRetailFamiliesLastDecodeSucceededForTesting());
	TestEqual(TEXT("a missing root decodes no objects"),
		WoTDevRetailFamiliesLastDecodedObjectCountForTesting(), 0);
	WoTDevRetailFamiliesResetForTesting();
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailDataRealRetailFamilyDigestsSpec,
	"WoT.RetailData.RealRetailFamilyDigests",
	WoTRetailDataSpecFlags)

bool FWoTRetailDataRealRetailFamilyDigestsSpec::RunTest(const FString&)
{
	if (!WoTReferenceTreeExists())
	{
		AddWarning(WoTReferenceMissingWarning(TEXT("original")));
		return true;
	}
	FWoTRetailDataActivationScope ActivationScope;
	WoTDevRetailFamiliesResetForTesting();
	FWoTRetailDataCacheRoot CacheRoot;
	WoTDevRetailFamiliesSetCacheRootForTesting(CacheRoot.Path);
	bool bAllFamiliesPinned = true;
	for (const FWoTRetailFamily& Family : WoTRetailFamilies())
	{
		bAllFamiliesPinned &= WoTRetailFamilyPins().Contains(Family.Name);
	}
	if (!bAllFamiliesPinned)
	{
		AddExpectedError(
			TEXT("Retail family development activation failed"),
			EAutomationExpectedErrorFlags::Contains, 5);
	}
	WoTDevRetailFamiliesEnsureActivated();
	TestTrue(TEXT("the real dev decode succeeds"),
		WoTDevRetailFamiliesLastDecodeSucceededForTesting());
	TestEqual(TEXT("all twenty production class-default objects decode"),
		WoTDevRetailFamiliesLastDecodedObjectCountForTesting(), 20);
	TestEqual(TEXT("the empty cache starts with no hits"),
		WoTDevRetailFamiliesLastCacheHitCountForTesting(), 0);
	TestTrue(TEXT("the decode has a nonnegative elapsed time"),
		WoTDevRetailFamiliesLastDecodeSecondsForTesting() >= 0.0);
	const double ColdSeconds =
		WoTDevRetailFamiliesLastDecodeSecondsForTesting();

	WoTDevRetailFamiliesResetForTesting();
	WoTRetailFamiliesResetActivationForTesting();
	WoTDevRetailFamiliesSetCacheRootForTesting(CacheRoot.Path);
	WoTDevRetailFamiliesEnsureActivated();
	TestTrue(TEXT("the warm dev decode succeeds"),
		WoTDevRetailFamiliesLastDecodeSucceededForTesting());
	TestEqual(TEXT("the warm decode loads every package by its digest key"),
		WoTDevRetailFamiliesLastCacheHitCountForTesting(), 20);
	const double WarmSeconds =
		WoTDevRetailFamiliesLastDecodeSecondsForTesting();

	TArray<FString> CachedObjects;
	IFileManager::Get().FindFilesRecursive(
		CachedObjects, *CacheRoot.Path, TEXT("*.wotobj3"), true, false);
	if (TestEqual(TEXT("the cache contains all twenty objects"),
		CachedObjects.Num(), 20))
	{
		TArray<uint8> Corrupted;
		if (TestTrue(TEXT("a cached object can be read for the corruption probe"),
			FFileHelper::LoadFileToArray(Corrupted, *CachedObjects[0]))
			&& TestTrue(TEXT("the cached object is nonempty"), !Corrupted.IsEmpty()))
		{
			Corrupted.Last() ^= 0xff;
			TestTrue(TEXT("the corruption probe is written"),
				FFileHelper::SaveArrayToFile(Corrupted, *CachedObjects[0]));
		}
	}
	WoTDevRetailFamiliesResetForTesting();
	WoTRetailFamiliesResetActivationForTesting();
	WoTDevRetailFamiliesSetCacheRootForTesting(CacheRoot.Path);
	WoTDevRetailFamiliesEnsureActivated();
	TestTrue(TEXT("a corrupted cache entry is rebuilt from source"),
		WoTDevRetailFamiliesLastDecodeSucceededForTesting());
	TestEqual(TEXT("only the corrupted cache entry misses"),
		WoTDevRetailFamiliesLastCacheHitCountForTesting(), 19);

	TArray<uint8> RuntimeCorrupted;
	if (TestTrue(TEXT("the repaired object can be read for runtime validation"),
		FFileHelper::LoadFileToArray(RuntimeCorrupted, *CachedObjects[0]))
		&& TestTrue(TEXT("the repaired object has a framed payload"),
			RuntimeCorrupted.Num() >= 20))
	{
		const uint32 HeaderBytes = static_cast<uint32>(RuntimeCorrupted[8])
			| (static_cast<uint32>(RuntimeCorrupted[9]) << 8)
			| (static_cast<uint32>(RuntimeCorrupted[10]) << 16)
			| (static_cast<uint32>(RuntimeCorrupted[11]) << 24);
		const int32 PayloadOffset = 12 + static_cast<int32>(HeaderBytes);
		if (TestTrue(TEXT("the framed payload offset is in bounds"),
			RuntimeCorrupted.IsValidIndex(PayloadOffset)))
		{
			RuntimeCorrupted[PayloadOffset] ^= 0xff;
			TestTrue(TEXT("the runtime-corrupt object is written"),
				FFileHelper::SaveArrayToFile(
					RuntimeCorrupted, *CachedObjects[0]));
			TestTrue(TEXT("the runtime-corrupt checksum is written"),
				FFileHelper::SaveStringToFile(
					Sha256(RuntimeCorrupted).ToHex(),
					*(CachedObjects[0] + TEXT(".sha256"))));
		}
	}
	WoTDevRetailFamiliesResetForTesting();
	WoTRetailFamiliesResetActivationForTesting();
	WoTDevRetailFamiliesSetCacheRootForTesting(CacheRoot.Path);
	WoTDevRetailFamiliesEnsureActivated();
	TestTrue(TEXT("runtime-invalid cached objects trigger a source rebuild"),
		WoTDevRetailFamiliesLastDecodeSucceededForTesting());
	TestEqual(TEXT("the bounded runtime recovery rebuilds the full object set"),
		WoTDevRetailFamiliesLastCacheHitCountForTesting(), 0);
	WoTDevRetailFamiliesResetForTesting();
	WoTRetailFamiliesResetActivationForTesting();
	WoTDevRetailFamiliesSetCacheRootForTesting(CacheRoot.Path);
	WoTDevRetailFamiliesEnsureActivated();
	TestEqual(TEXT("the recovered cache is warm on the next load"),
		WoTDevRetailFamiliesLastCacheHitCountForTesting(), 20);
	TestEqual(TEXT("all builder families join production activation"),
		WoTRetailFamilies().Num(), 12);
	AddInfo(FString::Printf(
		TEXT("Decoded 20 retail class-default packages cold in %.3f seconds and warm in %.3f seconds; production families: %d"),
		ColdSeconds, WarmSeconds,
		WoTRetailFamilies().Num()));
	for (const FWoTRetailFamily& Family : WoTRetailFamilies())
	{
		const FString* Pinned = WoTRetailFamilyPins().Find(Family.Name);
		const FString* Observed =
			WoTRetailFamilyObservedDigestsForTesting().Find(Family.Name);
		TestNotNull(TEXT("the family produced an observed digest"), Observed);
		if (Observed != nullptr && Pinned != nullptr)
		{
			TestEqual(TEXT("the observed family digest matches its pin"),
				*Observed, *Pinned);
		}
		AddInfo(FString::Printf(
			TEXT("Retail family %s observed digest: %s (pin: %s)"),
			Family.Name,
			Observed != nullptr ? **Observed : TEXT("<missing>"),
			Pinned != nullptr ? **Pinned : TEXT("<missing>")));
	}
	WoTDevRetailFamiliesResetForTesting();
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailDataRealRetailBuildContextSpec,
	"WoT.RetailData.RealRetailBuildContext",
	WoTRetailDataSpecFlags)

bool FWoTRetailDataRealRetailBuildContextSpec::RunTest(const FString&)
{
	if (!WoTReferenceTreeExists())
	{
		AddWarning(WoTReferenceMissingWarning(TEXT("original")));
		return true;
	}
	FWoTRetailDataActivationScope ActivationScope;
	WoTDevRetailFamiliesResetForTesting();
	WoTRetailFamiliesSetActivationInputsForTesting({}, {});
	FWoTRetailDataCacheRoot CacheRoot;
	WoTDevRetailFamiliesSetCacheRootForTesting(CacheRoot.Path);
	WoTDevRetailFamiliesEnsureActivated();
	FWoTRetailTextureFacts Facts;
	TestTrue(TEXT("the referenced blood texture decodes on demand"),
		WoTDevRetailFamiliesFindTextureForTesting(
			TEXT("WOTParticles.Blood.Blood16"), Facts));
	TestTrue(TEXT("the referenced blood texture has a nonzero size"),
		Facts.USize > 0 && Facts.VSize > 0);
	TArray<FWoTVertexMeshSequence> Sequences;
	TestTrue(TEXT("the referenced creature mesh decodes on demand"),
		WoTDevRetailFamiliesFindMeshSequencesForTesting(
			TEXT("WoTPawns.Hound"), Sequences));
	TestTrue(TEXT("the creature mesh contains a notify"),
		Sequences.ContainsByPredicate(
			[](const FWoTVertexMeshSequence& Sequence)
			{
				return !Sequence.Notifies.IsEmpty();
			}));
	WoTDevRetailFamiliesResetForTesting();
	return !HasAnyErrors();
}

#endif // !WOT_GUARDED_BOOTSTRAP

#endif // WITH_AUTOMATION_TESTS
