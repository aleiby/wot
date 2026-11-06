// Coverage contract (packaged matrix only): all 19 shells traverse the real
// UWoTGameEngine normal LoadMap route. Slots 01/05/10/15/19 traverse real
// listen-host LoadMap and host seamless ServerTravel routes. Each rotating
// slot first attempts a real loopback PendingNetGame client join and propagated
// client seamless travel. If headless InitConnect cannot start, only those two
// client-side routes use the guarded dispatcher directly; once an asynchronous
// route starts, missing either bounded deadline fails its cell. The test records
// every fallback cell and never describes one as real network-route coverage.
#include "Misc/AutomationTest.h"

#include "../WoTGameEngine.h"
#include "../WoTGameInstance.h"
#include "../WoTPayloadSubsystem.h"
#include "../Payload/WoTPayloadActivation.h"
#include "../World/WoTMaterializedWorldHost.h"
#include "../World/WoTWorldMaterializer.h"
#include "../Inventory/WoTAngrealSpawnPool.h"
#include "../WoTEvents.h"
#include "../WoTBattleGameMode.h"
#include "../WoTBattleInfo.h"
#include "../WoTDoorMover.h"
#include "../WoTDoorTrigger.h"
#include "../WoTFactionCharacters.h"
#include "../WoTHUD.h"
#include "../WoTMover.h"
#include "../WoTPlayerController.h"
#include "../WoTPlayerState.h"
#include "../Projectiles/WoTMashadarGuide.h"
#include "../Projectiles/WoTMashadarSegment.h"
#include "WoTTestWorld.h"
#include "WoTWorldMaterializerTestData.h"

#include "Dom/JsonObject.h"
#include "CoreGlobals.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/GameInstance.h"
#include "Engine/NetConnection.h"
#include "Engine/NetDriver.h"
#include "Engine/PendingNetGame.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "EngineGlobals.h"
#include "EngineUtils.h"
#include "GameMapsSettings.h"
#include "GameFramework/Actor.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "IPAddress.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"
#include "UnrealEngine.h"
#include "WoTBootstrapCore/Hash.h"

#include <initializer_list>

#if WITH_AUTOMATION_TESTS

namespace
{
constexpr EAutomationTestFlags WoTMaterializedListenHarnessFlags =
	EAutomationTestFlags::EditorContext
	| EAutomationTestFlags::ProductFilter;

constexpr int32 WoTMaterializedListenHarnessMaximumWaitTicks = 1200;
constexpr double WoTMaterializedListenHarnessMaximumWaitSeconds = 15.0;

struct FWoTMaterializedListenHarnessConfig
{
	bool bPackagedMatrix = false;
	FString ShellRoot;
	FString DeniedReadReceipt;
	FString OutputIdentityReceipt;
};

constexpr const TCHAR* WoTNeutralShellScaffoldingClassPaths[] = {
	TEXT("/Script/Engine.Brush"),
	TEXT("/Script/Engine.DefaultPhysicsVolume"),
	TEXT("/Script/Engine.WorldSettings"),
	TEXT("/Script/NavigationSystem.AbstractNavData"),
};

bool WoTMaterializedListenHarnessIsScaffoldingClass(const UClass* Class)
{
	if (Class == nullptr)
	{
		return false;
	}
	const FString ClassPath = Class->GetPathName();
	for (const TCHAR* AllowedPath : WoTNeutralShellScaffoldingClassPaths)
	{
		// Deliberately compare the exact class path: an authored subclass of
		// Brush (or of any other structural class) is authored shell content.
		if (ClassPath == AllowedPath)
		{
			return true;
		}
	}
	return false;
}

FString WoTMaterializedListenHarnessEnvironment(const TCHAR* Name)
{
	return FPlatformMisc::GetEnvironmentVariable(Name).TrimStartAndEnd();
}

bool WoTMaterializedListenHarnessParseEnvironment(
	FAutomationTestBase& Test, FWoTMaterializedListenHarnessConfig& Out)
{
	const FString Matrix = WoTMaterializedListenHarnessEnvironment(
		TEXT("WOT_WORLD_PACKAGED_MATRIX"));
	const FString MapSet = WoTMaterializedListenHarnessEnvironment(
		TEXT("WOT_WORLD_MAP_SET"));
	Out.ShellRoot = WoTMaterializedListenHarnessEnvironment(
		TEXT("WOT_WORLD_SHELL_ROOT"));
	Out.DeniedReadReceipt = WoTMaterializedListenHarnessEnvironment(
		TEXT("WOT_WORLD_SHELL_DENIED_READ_RECEIPT"));
	Out.OutputIdentityReceipt = WoTMaterializedListenHarnessEnvironment(
		TEXT("WOT_WORLD_SHELL_OUTPUT_IDENTITY_RECEIPT"));
	const bool bAny = !Matrix.IsEmpty() || !MapSet.IsEmpty()
		|| !Out.ShellRoot.IsEmpty() || !Out.DeniedReadReceipt.IsEmpty()
		|| !Out.OutputIdentityReceipt.IsEmpty();
	if (!bAny)
	{
		Out.bPackagedMatrix = false;
		return true;
	}
	Out.bPackagedMatrix = true;
	const FString ExpectedMatrix =
		TEXT("normal,host-travel,client-travel,host-seamless,client-seamless");
	return Test.TestEqual(TEXT("packaged matrix names every guarded route"),
		Matrix, ExpectedMatrix)
		&& Test.TestEqual(TEXT("packaged map set is the complete shell set"),
			MapSet, FString(TEXT("all-19")))
		&& Test.TestFalse(TEXT("packaged shell root is present"),
			Out.ShellRoot.IsEmpty())
		&& Test.TestFalse(TEXT("denied-read receipt path is present"),
			Out.DeniedReadReceipt.IsEmpty())
		&& Test.TestFalse(TEXT("output-identity receipt path is present"),
			Out.OutputIdentityReceipt.IsEmpty());
}

TSharedPtr<FJsonObject> WoTMaterializedListenHarnessReadJson(
	FAutomationTestBase& Test, const FString& Path, const TCHAR* Label)
{
	FString Text;
	if (!Test.TestTrue(Label, FFileHelper::LoadFileToString(Text, *Path)))
	{
		return nullptr;
	}
	TSharedPtr<FJsonObject> Object;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	if (!Test.TestTrue(TEXT("receipt is JSON"),
		FJsonSerializer::Deserialize(Reader, Object) && Object.IsValid()))
	{
		return nullptr;
	}
	return Object;
}

bool WoTMaterializedListenHarnessExactMembers(
	FAutomationTestBase& Test, const FJsonObject& Object,
	std::initializer_list<const TCHAR*> Expected, const TCHAR* Label)
{
	TSet<FString> ExpectedSet;
	for (const TCHAR* Field : Expected)
	{
		ExpectedSet.Add(Field);
	}
	TSet<FString> ActualSet;
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object.Values)
	{
		ActualSet.Add(Pair.Key);
	}
	if (ActualSet.Num() != ExpectedSet.Num())
	{
		Test.AddError(FString::Printf(
			TEXT("%s has missing or unknown receipt members"), Label));
		return false;
	}
	for (const FString& Field : ExpectedSet)
	{
		if (!ActualSet.Contains(Field))
		{
			Test.AddError(FString::Printf(
				TEXT("%s has missing or unknown receipt members"), Label));
			return false;
		}
	}
	return true;
}

bool WoTMaterializedListenHarnessHashFile(
	const FString& Path, FString& OutDigest)
{
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path))
	{
		return false;
	}
	OutDigest = Sha256(Bytes).ToHex();
	return true;
}

bool WoTMaterializedListenHarnessCheckReceipts(
	FAutomationTestBase& Test,
	const FWoTMaterializedListenHarnessConfig& Config)
{
	TSharedPtr<FJsonObject> Denied = WoTMaterializedListenHarnessReadJson(
		Test, Config.DeniedReadReceipt,
		TEXT("denied-read/open receipt is readable"));
	TSharedPtr<FJsonObject> Identity = WoTMaterializedListenHarnessReadJson(
		Test, Config.OutputIdentityReceipt,
		TEXT("output-identity receipt is readable"));
	if (!Denied.IsValid() || !Identity.IsValid())
	{
		return false;
	}
	if (!WoTMaterializedListenHarnessExactMembers(Test, *Denied, {
			TEXT("denied_path_classes"), TEXT("denied_roots"), TEXT("format"),
			TEXT("generator_identity"), TEXT("generator_sha256"),
			TEXT("generator_source_identity"), TEXT("io_audit_scope"),
			TEXT("native_ue_io_audited"), TEXT("native_ue_io_limitation"),
			TEXT("opened_inputs"), TEXT("platform"),
			TEXT("python_audited_opens"), TEXT("root_identity"),
			TEXT("root_native_identity"), TEXT("spec_identity"),
			TEXT("spec_sha256"), TEXT("version"), TEXT("violations"),
		}, TEXT("denied-read receipt"))
		|| !WoTMaterializedListenHarnessExactMembers(Test, *Identity, {
			TEXT("format"), TEXT("generator_identity"),
			TEXT("generator_sha256"), TEXT("generator_source_identity"),
			TEXT("package_census_sha256"), TEXT("package_count"),
			TEXT("packages"), TEXT("platform"),
			TEXT("reserved_disjoint_roots"), TEXT("root_began_absent"),
			TEXT("root_identity"), TEXT("root_native_identity"),
			TEXT("spec_identity"), TEXT("spec_sha256"), TEXT("version"),
		}, TEXT("output-identity receipt")))
	{
		return false;
	}
	FString CanonicalRoot = FPaths::ConvertRelativePathToFull(
		Config.ShellRoot);
	FPaths::NormalizeDirectoryName(CanonicalRoot);
	FString DeniedFormat;
	FString IdentityFormat;
	FString DeniedRoot;
	FString IdentityRoot;
	FString DeniedSpecDigest;
	FString IdentitySpecDigest;
	FString DeniedGeneratorDigest;
	FString IdentityGeneratorDigest;
	FString DeniedGeneratorIdentity;
	FString IdentityGeneratorIdentity;
	FString DeniedPlatform;
	FString IdentityPlatform;
	FString PackageCensusDigest;
	FString AuditScope;
	FString NativeLimitation;
	double PackageCount = 0.0;
	double DeniedVersion = 0.0;
	double IdentityVersion = 0.0;
	bool bNativeAudited = true;
	bool bRootBeganAbsent = false;
	const TArray<TSharedPtr<FJsonValue>>* Violations = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Packages = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* DeniedRoots = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* OpenedInputs = nullptr;
	if (!Denied->TryGetStringField(TEXT("format"), DeniedFormat)
		|| !Identity->TryGetStringField(TEXT("format"), IdentityFormat)
		|| !Denied->TryGetStringField(TEXT("root_identity"), DeniedRoot)
		|| !Identity->TryGetStringField(TEXT("root_identity"), IdentityRoot)
		|| !Denied->TryGetStringField(TEXT("spec_sha256"), DeniedSpecDigest)
		|| !Identity->TryGetStringField(TEXT("spec_sha256"), IdentitySpecDigest)
		|| !Denied->TryGetStringField(
			TEXT("generator_sha256"), DeniedGeneratorDigest)
		|| !Identity->TryGetStringField(
			TEXT("generator_sha256"), IdentityGeneratorDigest)
		|| !Denied->TryGetStringField(
			TEXT("generator_identity"), DeniedGeneratorIdentity)
		|| !Identity->TryGetStringField(
			TEXT("generator_identity"), IdentityGeneratorIdentity)
		|| !Denied->TryGetStringField(TEXT("platform"), DeniedPlatform)
		|| !Identity->TryGetStringField(TEXT("platform"), IdentityPlatform)
		|| !Identity->TryGetStringField(
			TEXT("package_census_sha256"), PackageCensusDigest)
		|| !Identity->TryGetNumberField(TEXT("package_count"), PackageCount)
		|| !Denied->TryGetNumberField(TEXT("version"), DeniedVersion)
		|| !Identity->TryGetNumberField(TEXT("version"), IdentityVersion)
		|| !Denied->TryGetStringField(TEXT("io_audit_scope"), AuditScope)
		|| !Denied->TryGetBoolField(
			TEXT("native_ue_io_audited"), bNativeAudited)
		|| !Denied->TryGetStringField(
			TEXT("native_ue_io_limitation"), NativeLimitation)
		|| !Identity->TryGetBoolField(
			TEXT("root_began_absent"), bRootBeganAbsent)
		|| !Denied->TryGetArrayField(TEXT("denied_roots"), DeniedRoots)
		|| !Denied->TryGetArrayField(TEXT("opened_inputs"), OpenedInputs)
		|| !Denied->TryGetArrayField(TEXT("violations"), Violations)
		|| !Identity->TryGetArrayField(TEXT("packages"), Packages))
	{
		Test.AddError(TEXT("shell receipts omit required identity fields"));
		return false;
	}
	FPaths::NormalizeDirectoryName(DeniedRoot);
	FPaths::NormalizeDirectoryName(IdentityRoot);
	Test.TestEqual(TEXT("denied receipt has the frozen format"), DeniedFormat,
		FString(TEXT("neutral-map-shells-denied-read-open-receipt-v2")));
	Test.TestEqual(TEXT("identity receipt has the frozen format"), IdentityFormat,
		FString(TEXT("neutral-map-shells-output-identity-receipt-v2")));
	Test.TestEqual(TEXT("denied receipt version is exact"),
		static_cast<int32>(DeniedVersion), 2);
	Test.TestEqual(TEXT("identity receipt version is exact"),
		static_cast<int32>(IdentityVersion), 2);
	Test.TestEqual(TEXT("denied receipt binds the consumed root"),
		DeniedRoot, CanonicalRoot);
	Test.TestEqual(TEXT("identity receipt binds the consumed root"),
		IdentityRoot, CanonicalRoot);
	Test.TestEqual(TEXT("receipts bind one authored spec"),
		DeniedSpecDigest, IdentitySpecDigest);
	Test.TestEqual(TEXT("receipts bind one generator"),
		DeniedGeneratorDigest, IdentityGeneratorDigest);
	Test.TestEqual(TEXT("receipts name one generator"),
		DeniedGeneratorIdentity, IdentityGeneratorIdentity);
	Test.TestEqual(TEXT("generator identity has the frozen value"),
		DeniedGeneratorIdentity,
		FString(TEXT("generate-neutral-map-shells-v1")));
	Test.TestEqual(TEXT("receipts bind one generation platform"),
		DeniedPlatform, IdentityPlatform);
#if PLATFORM_MAC
	const FString ExpectedPlatform(TEXT("Darwin"));
#elif PLATFORM_WINDOWS
	const FString ExpectedPlatform(TEXT("Windows"));
#else
	const FString ExpectedPlatform(FPlatformMisc::GetUBTPlatform());
#endif
	Test.TestEqual(TEXT("receipt platform is the consuming platform"),
		DeniedPlatform, ExpectedPlatform);
	FString ExpectedSpecDigest;
	FString ExpectedGeneratorDigest;
	const FString ExpectedSpecPath = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectDir(),
			TEXT("Tools/import/specs/neutral-map-shells-v1.json")));
	const FString ExpectedGeneratorPath = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectDir(),
			TEXT("Tools/import/generate_neutral_map_shells.py")));
	Test.TestTrue(TEXT("checked-in shell spec is hashable"),
		WoTMaterializedListenHarnessHashFile(
			ExpectedSpecPath, ExpectedSpecDigest));
	Test.TestTrue(TEXT("checked-in shell generator is hashable"),
		WoTMaterializedListenHarnessHashFile(
			ExpectedGeneratorPath, ExpectedGeneratorDigest));
	Test.TestEqual(TEXT("receipt pins the checked-in spec bytes"),
		DeniedSpecDigest, ExpectedSpecDigest);
	Test.TestEqual(TEXT("receipt pins the checked-in generator bytes"),
		DeniedGeneratorDigest, ExpectedGeneratorDigest);
	Test.TestEqual(TEXT("package census has a complete SHA-256"),
		PackageCensusDigest.Len(), 64);
	Test.TestTrue(TEXT("output root was absent before atomic creation"),
		bRootBeganAbsent);
	Test.TestEqual(TEXT("audit scope is honestly Python-only"), AuditScope,
		FString(TEXT("python-audit-open-events-only")));
	Test.TestFalse(TEXT("receipt does not claim native UE I/O coverage"),
		bNativeAudited);
	Test.TestTrue(TEXT("receipt states the native UE I/O limitation"),
		NativeLimitation.Contains(TEXT("cannot observe native Unreal")));
	Test.TestEqual(TEXT("receipt has exactly nineteen packages"),
		static_cast<int32>(PackageCount), 19);
	Test.TestEqual(TEXT("receipt package array is complete"),
		Packages->Num(), 19);
	Test.TestEqual(TEXT("receipt records no denied input open"),
		Violations->Num(), 0);
	TSet<FString> DeniedLeafNames;
	for (const TSharedPtr<FJsonValue>& Value : *DeniedRoots)
	{
		DeniedLeafNames.Add(FPaths::GetCleanFilename(Value->AsString()));
	}
	for (const TCHAR* Mandated : {
			TEXT("Content"), TEXT("original"), TEXT("devtree"), TEXT("exports")})
	{
		Test.TestTrue(TEXT("receipt records every mandated denied root"),
			DeniedLeafNames.Contains(Mandated));
	}
	for (const TSharedPtr<FJsonValue>& Value : *OpenedInputs)
	{
		const TSharedPtr<FJsonObject> Row = Value->AsObject();
		if (!Row.IsValid()
			|| !WoTMaterializedListenHarnessExactMembers(
				Test, *Row, {TEXT("path"), TEXT("sha256")},
				TEXT("opened-input row")))
		{
			return false;
		}
		FString Path;
		FString Digest;
		FString ActualDigest;
		if (!Row->TryGetStringField(TEXT("path"), Path)
			|| !Row->TryGetStringField(TEXT("sha256"), Digest)
			|| !WoTMaterializedListenHarnessHashFile(Path, ActualDigest)
			|| ActualDigest != Digest)
		{
			Test.AddError(TEXT("opened input digest is stale or unreadable"));
			return false;
		}
	}

	FWoTSha256Hasher PackageCensusHasher;
	for (int32 Index = 0; Index < Packages->Num(); ++Index)
	{
		const TSharedPtr<FJsonObject> Row = (*Packages)[Index]->AsObject();
		if (!Row.IsValid())
		{
			Test.AddError(TEXT("package receipt row is not an object"));
			return false;
		}
		if (!WoTMaterializedListenHarnessExactMembers(Test, *Row, {
				TEXT("bytes"), TEXT("package"), TEXT("relative_path"),
				TEXT("sha256"), TEXT("slot"),
			}, TEXT("package receipt row")))
		{
			return false;
		}
		FString Package;
		FString RelativePath;
		FString Digest;
		double Slot = 0.0;
		double ByteCount = 0.0;
		if (!Row->TryGetStringField(TEXT("package"), Package)
			|| !Row->TryGetStringField(TEXT("relative_path"), RelativePath)
			|| !Row->TryGetStringField(TEXT("sha256"), Digest)
			|| !Row->TryGetNumberField(TEXT("slot"), Slot)
			|| !Row->TryGetNumberField(TEXT("bytes"), ByteCount))
		{
			Test.AddError(TEXT("package receipt row omits sealed fields"));
			return false;
		}
		const int32 ExpectedSlot = Index + 1;
		const FString ExpectedName = FString::Printf(
			TEXT("WoTMapShell%02d"), ExpectedSlot);
		const FString ExpectedPackage =
			FString(TEXT("/WoTNeutralShells/")) + ExpectedName;
		const FString ExpectedRelative = ExpectedName + TEXT(".umap");
		Test.TestEqual(TEXT("package slot order is stable"),
			static_cast<int32>(Slot), ExpectedSlot);
		Test.TestEqual(TEXT("package identity is neutral"),
			Package, ExpectedPackage);
		Test.TestEqual(TEXT("package filename is root relative"),
			RelativePath, ExpectedRelative);
		const FString Filename = FPaths::Combine(CanonicalRoot, RelativePath);
		TArray<uint8> Bytes;
		if (!Test.TestTrue(TEXT("sealed package exists"),
			FFileHelper::LoadFileToArray(Bytes, *Filename)))
		{
			return false;
		}
		Test.TestEqual(TEXT("sealed byte count matches"), Bytes.Num(),
			static_cast<int32>(ByteCount));
		Test.TestEqual(TEXT("sealed digest matches"), Sha256(Bytes).ToHex(), Digest);
		const FString CanonicalRow = FString::Printf(
			TEXT("{\"bytes\":%d,\"package\":\"%s\",\"relative_path\":\"%s\",\"sha256\":\"%s\",\"slot\":%d}"),
			static_cast<int32>(ByteCount), *Package, *RelativePath, *Digest,
			static_cast<int32>(Slot));
		const FTCHARToUTF8 CanonicalRowUtf8(*CanonicalRow);
		PackageCensusHasher.Update(TConstArrayView<uint8>(
			reinterpret_cast<const uint8*>(CanonicalRowUtf8.Get()),
			CanonicalRowUtf8.Length()));
	}
	Test.TestEqual(TEXT("sealed package census matches"),
		PackageCensusHasher.Finish().ToHex(), PackageCensusDigest);
	Test.TestFalse(TEXT("bootstrap map is absent from the normal-shell root"),
		IFileManager::Get().FileExists(
			*FPaths::Combine(CanonicalRoot, TEXT("WoTBootstrap.umap"))));
	return true;
}

bool WoTMaterializedListenHarnessLoadShells(
	FAutomationTestBase& Test, const FString& ShellRoot)
{
	FString CanonicalRoot = FPaths::ConvertRelativePathToFull(ShellRoot);
	FPaths::NormalizeDirectoryName(CanonicalRoot);
	bool bComplete = true;
	for (int32 Slot = 1; Slot <= 19; ++Slot)
	{
		const FString Name = FString::Printf(TEXT("WoTMapShell%02d"), Slot);
		const FString LongPackageName =
			FString(TEXT("/WoTNeutralShells/")) + Name;
		UPackage* Package = LoadPackage(nullptr, *LongPackageName, LOAD_None);
		if (!Test.TestNotNull(TEXT("neutral shell package loads"), Package))
		{
			bComplete = false;
			continue;
		}
		UWorld* World = UWorld::FindWorldInPackage(Package);
		if (!Test.TestNotNull(TEXT("neutral shell contains a world"), World))
		{
			bComplete = false;
			continue;
		}
		int32 AuthoredActors = 0;
		TActorIterator<AActor> ActorIt(World);
		while (ActorIt)
		{
			// Measured fresh/saved/reloaded editor censuses serialize only
			// AbstractNavData, Brush, BuoyancyManager (editor-only),
			// DefaultPhysicsVolume, GameplayDebuggerPlayerManager (editor-only),
			// and WorldSettings. ChaosDebugDrawActor appears only at load time and
			// lacks RF_WasLoaded, so it is not authored package content.
			if (ActorIt->HasAnyFlags(RF_WasLoaded)
				&& !WoTMaterializedListenHarnessIsScaffoldingClass(
				ActorIt->GetClass()))
			{
				++AuthoredActors;
				Test.AddError(FString::Printf(
					TEXT("neutral shell actor %s has non-allow-listed exact class %s"),
					*ActorIt->GetPathName(), *ActorIt->GetClass()->GetPathName()));
			}
			++ActorIt;
		}
		Test.TestEqual(TEXT("neutral shell has no authored actors"),
			AuthoredActors, 0);
		Test.TestFalse(TEXT("loaded shell has not begun play"),
			World->HasBegunPlay());
	}
	return bComplete;
}

struct FWoTMaterializedListenHarnessMount
{
	explicit FWoTMaterializedListenHarnessMount(const FString& InRoot)
	{
		Root = FPaths::ConvertRelativePathToFull(InRoot);
		FPaths::NormalizeDirectoryName(Root);
		FPackageName::RegisterMountPoint(
			TEXT("/WoTNeutralShells/"), Root + TEXT("/"));
	}

	~FWoTMaterializedListenHarnessMount()
	{
		FPackageName::UnRegisterMountPoint(
			TEXT("/WoTNeutralShells/"), Root + TEXT("/"));
	}

	FString Root;
};

struct FWoTMaterializedListenHarnessCacheRoot
{
	FWoTMaterializedListenHarnessCacheRoot()
	{
		Path = FPaths::Combine(FPlatformProcess::UserTempDir(),
			TEXT("wot-listen-matrix-")
				+ FGuid::NewGuid().ToString(EGuidFormats::Digits),
			TEXT("ContentCache"));
		IFileManager::Get().MakeDirectory(*Path, true);
	}

	~FWoTMaterializedListenHarnessCacheRoot()
	{
		IFileManager::Get().DeleteDirectory(
			*FPaths::GetPath(Path), false, true);
	}

	FString Path;
};

struct FWoTMaterializedListenHarnessActivation
{
	FWoTMaterializedListenHarnessActivation()
	{
		UWoTPayloadSubsystem::SetInjectionEnabledOverrideForTests(
			/*bHasOverride*/ true, /*bEnabled*/ false);
		WoTTest::FSpecWorldOptions Options;
		Options.GameInstanceClass = UWoTGameInstance::StaticClass();
		Options.bStartPlay = false;
		Options.bActivatePayload = false;
		World = WoTTest::CreateSpecWorld(Options);
		Payloads = World != nullptr && World->GetGameInstance() != nullptr
			? World->GetGameInstance()->GetSubsystem<UWoTPayloadSubsystem>()
			: nullptr;
	}

	~FWoTMaterializedListenHarnessActivation()
	{
		FWoTWorldMaterializer::ResetAuthorityForAutomationTests();
		WoTTest::DestroySpecWorld(World);
		World = nullptr;
		Payloads = nullptr;
		UWoTPayloadSubsystem::SetInjectionEnabledOverrideForTests(
			/*bHasOverride*/ false);
		ResetWoTPayloadActivationForTesting();
	}

	UWorld* World = nullptr;
	UWoTPayloadSubsystem* Payloads = nullptr;
};

UWorld* WoTMaterializedListenHarnessHiddenCandidate(const TCHAR* PackageName)
{
	UPackage* Package = CreatePackage(PackageName);
	Package->SetFlags(RF_Transient);
	return UWorld::CreateWorld(EWorldType::Game, false,
		MakeUniqueObjectName(
			Package, UWorld::StaticClass(), FName(TEXT("World"))),
		Package, /*bAddToRoot*/ true, ERHIFeatureLevel::Num,
		/*InIVS*/ nullptr, /*bInSkipInitWorld*/ true);
}

bool WoTMaterializedListenHarnessProvisionAuthority(
	FAutomationTestBase& Test,
	FWoTMaterializedListenHarnessActivation& Activation,
	const FString& CacheRoot)
{
	ResetWoTPayloadActivationForTesting();
	FWoTWorldMaterializer::ResetAuthorityForAutomationTests();
	if (!Test.TestNotNull(TEXT("matrix activation world exists"), Activation.World)
		|| !Test.TestNotNull(
			TEXT("matrix payload subsystem exists"), Activation.Payloads))
	{
		return false;
	}
	UWorld* Bootstrap = WoTMaterializedListenHarnessHiddenCandidate(
		TEXT("/Game/WoT/Maps/WoTBootstrap"));
	FString Error;
	const bool bBootstrapSpent = Test.TestNotNull(
		TEXT("matrix bootstrap candidate exists"), Bootstrap)
		&& Test.TestTrue(TEXT("matrix admits the bundled bootstrap world"),
			WoTGuardedStartup_MaterializeWorld(
				Bootstrap, EWoTWorldStartPath::NormalInitialLoad, &Error));
	if (Bootstrap != nullptr
		&& !Bootstrap->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
	{
		Bootstrap->DestroyWorld(false);
		Bootstrap->MarkAsGarbage();
	}
	if (!bBootstrapSpent)
	{
		return false;
	}

	TArray<FWoTWorldDescriptor> Descriptors;
	if (!Test.TestTrue(TEXT("matrix publishes nineteen exact world records"),
		WoTWorldMaterializerTest_PublishWorldSet(
			CacheRoot, Descriptors, Error)))
	{
		Test.AddError(Error);
		return false;
	}
	if (!Test.TestTrue(TEXT("matrix activates and authorizes exact descriptors"),
		WoTWorldMaterializerTest_ActivateAndAuthorize(
			Activation.Payloads, CacheRoot, MoveTemp(Descriptors), Error)))
	{
		Test.AddError(Error);
		return false;
	}
	return Test.TestFalse(
		TEXT("authority remains unpublished before first engine exposure"),
		IsWoTBootstrapWorldReady());
}

FString WoTMaterializedListenHarnessCanonicalPackage(const UWorld& World)
{
	const UPackage* Package = World.GetOutermost();
	const int32 PIEInstance = Package->GetPIEInstanceID();
	if (Package->HasAnyPackageFlags(PKG_PlayInEditor)
		&& PIEInstance != INDEX_NONE)
	{
		return UWorld::StripPIEPrefixFromPackageName(
			Package->GetName(), UWorld::BuildPIEPackagePrefix(PIEInstance));
	}
	return Package->GetName();
}

bool WoTMaterializedListenHarnessDispatchCandidate(
	UWorld* World, EWoTWorldStartPath Path, FString& OutError)
{
	if (!WoTGuardedStartup_MaterializeWorld(World, Path, &OutError))
	{
		return false;
	}
	return WoTGuardedStartup_CommitWorldExposure(World, &OutError);
}

AWoTMaterializedWorldRoot* WoTMaterializedListenHarnessRoot(UWorld* World)
{
	if (World == nullptr)
	{
		return nullptr;
	}
	if (TActorIterator<AWoTMaterializedWorldRoot> It(World); It)
	{
		return *It;
	}
	return nullptr;
}

bool WoTMaterializedListenHarnessValidateExposedWorld(
	FAutomationTestBase& Test, UWorld* World, int32 Slot,
	const TCHAR* Route)
{
	if (!Test.TestNotNull(Route, World))
	{
		return false;
	}
	const FString ExpectedPackage = FString::Printf(
		TEXT("/WoTNeutralShells/WoTMapShell%02d"), Slot);
	bool bValid = Test.TestEqual(TEXT("route exposed the requested shell"),
		WoTMaterializedListenHarnessCanonicalPackage(*World), ExpectedPackage);
	bValid = Test.TestTrue(TEXT("route reached BeginPlay once"),
		World->HasBegunPlay()) && bValid;
	AWoTMaterializedWorldRoot* Root = WoTMaterializedListenHarnessRoot(World);
	if (!Test.TestNotNull(TEXT("materialization preceded route observers"), Root))
	{
		return false;
	}
	bValid = Test.TestNotNull(
		TEXT("route retained semantic geometry"), Root->Geometry.Get()) && bValid;
	for (int32 ActorIndex = 0; ActorIndex < Root->ActorHosts.Num(); ++ActorIndex)
	{
		const AWoTMaterializedActorHost* Actor = Root->ActorHosts[ActorIndex];
		if (Actor == nullptr)
		{
			Test.AddError(FString::Printf(
				TEXT("semantic actor %d is missing after exposure"), ActorIndex));
			bValid = false;
			continue;
		}
		bValid = Test.TestEqual(FString::Printf(
			TEXT("semantic actor %d began play exactly once"), ActorIndex),
			Actor->MaterializedBeginPlayCount, 1u) && bValid;
	}
	return bValid;
}

bool WoTMaterializedListenHarnessValidateProjectedPool(
	FAutomationTestBase& Test, UWorld* HostWorld, UWorld* ClientWorld,
	const TCHAR* Route)
{
	auto FindPool = [](UWorld* World) -> AWoTAngrealSpawnPool*
	{
		if (World == nullptr) return nullptr;
		for (TActorIterator<AWoTAngrealSpawnPool> It(World); It; ++It)
			if (It->GetFName() == TEXT("WoTProjected_1_SpawnPool")) return *It;
		return nullptr;
	};
	AWoTAngrealSpawnPool* HostPool = FindPool(HostWorld);
	AWoTAngrealSpawnPool* ClientPool = FindPool(ClientWorld);
	bool bValid = Test.TestNotNull(
		FString::Printf(TEXT("%s host projected pool exists"), Route), HostPool);
	bValid = Test.TestNotNull(
		FString::Printf(TEXT("%s client projected pool exists"), Route), ClientPool)
		&& bValid;
	if (!HostPool || !ClientPool) return false;
	bValid = Test.TestEqual(
		FString::Printf(TEXT("%s projected pool net names agree"), Route),
		HostPool->GetFName(), ClientPool->GetFName()) && bValid;
	bValid = Test.TestTrue(
		FString::Printf(TEXT("%s projected pool name is stable"), Route),
		ClientPool->IsNameStableForNetworking()) && bValid;
	bValid = Test.TestEqual(
		FString::Printf(TEXT("%s client pool is a simulated proxy"), Route),
		ClientPool->GetLocalRole(), ROLE_SimulatedProxy) && bValid;
	bValid = Test.TestNull(
		FString::Printf(TEXT("%s client pool has no ghost pickup"), Route),
		ClientPool->PickupItem.Get()) && bValid;
	return bValid;
}

FString WoTMaterializedListenHarnessCanonicalActorPath(const AActor& Actor)
{
	const FString NativePath = Actor.GetPathName();
	const int32 LevelSeparator = NativePath.Find(TEXT(":PersistentLevel."));
	if (LevelSeparator == INDEX_NONE || Actor.GetWorld() == nullptr)
		return NativePath;
	return WoTMaterializedListenHarnessCanonicalPackage(*Actor.GetWorld())
		+ NativePath.Mid(LevelSeparator);
}

bool WoTMaterializedListenHarnessValidateProjectedDoorActors(
	FAutomationTestBase& Test, UWorld* HostWorld, UWorld* ClientWorld,
	const TCHAR* Route)
{
	auto FindMover = [](UWorld* World) -> AWoTDoorMover*
	{
		if (!World) return nullptr;
		for (TActorIterator<AWoTDoorMover> It(World); It; ++It)
			if (It->GetFName() == TEXT("WoTProjected_2_Mover")) return *It;
		return nullptr;
	};
	auto FindTrigger = [](UWorld* World) -> AWoTDoorTrigger*
	{
		if (!World) return nullptr;
		for (TActorIterator<AWoTDoorTrigger> It(World); It; ++It)
			if (It->GetFName() == TEXT("WoTProjected_3_DoorTrigger")) return *It;
		return nullptr;
	};
	AWoTDoorMover* HostMover = FindMover(HostWorld);
	AWoTDoorMover* ClientMover = FindMover(ClientWorld);
	AWoTDoorTrigger* HostTrigger = FindTrigger(HostWorld);
	AWoTDoorTrigger* ClientTrigger = FindTrigger(ClientWorld);
	bool bValid = Test.TestNotNull(FString::Printf(
		TEXT("%s host projected mover exists"), Route), HostMover);
	bValid = Test.TestNotNull(FString::Printf(
		TEXT("%s client projected mover exists"), Route), ClientMover) && bValid;
	bValid = Test.TestNotNull(FString::Printf(
		TEXT("%s host projected trigger exists"), Route), HostTrigger) && bValid;
	bValid = Test.TestNotNull(FString::Printf(
		TEXT("%s client projected trigger exists"), Route), ClientTrigger) && bValid;
	if (!HostMover || !ClientMover || !HostTrigger || !ClientTrigger) return false;
	bValid = Test.TestEqual(FString::Printf(
		TEXT("%s mover server/client names agree"), Route),
		HostMover->GetFName(), ClientMover->GetFName()) && bValid;
	bValid = Test.TestEqual(FString::Printf(
		TEXT("%s mover server/client canonical paths agree"), Route),
		WoTMaterializedListenHarnessCanonicalActorPath(*HostMover),
		WoTMaterializedListenHarnessCanonicalActorPath(*ClientMover)) && bValid;
	bValid = Test.TestTrue(FString::Printf(
		TEXT("%s client mover name is stable"), Route),
		ClientMover->IsNameStableForNetworking()) && bValid;
	bValid = Test.TestEqual(FString::Printf(
		TEXT("%s client mover is a simulated proxy"), Route),
		ClientMover->GetLocalRole(), ROLE_SimulatedProxy) && bValid;
	bValid = Test.TestFalse(FString::Printf(
		TEXT("%s projected client trigger is non-replicated"), Route),
		ClientTrigger->GetIsReplicated()) && bValid;
	bValid = Test.TestEqual(FString::Printf(
		TEXT("%s projected client trigger is ROLE_None"), Route),
		ClientTrigger->GetLocalRole(), ROLE_None) && bValid;
	return bValid;
}

bool WoTMaterializedListenHarnessValidateProjectedBattleInfo(
	FAutomationTestBase& Test, UWorld* HostWorld, UWorld* ClientWorld,
	const TCHAR* Route)
{
	auto FindBattle = [](UWorld* World) -> AWoTBattleInfo*
	{
		if (!World) return nullptr;
		for (TActorIterator<AWoTBattleInfo> It(World); It; ++It)
			if (It->GetFName() == TEXT("WoTProjected_4_BattleInfo")) return *It;
		return nullptr;
	};
	AWoTBattleInfo* HostBattle = FindBattle(HostWorld);
	AWoTBattleInfo* ClientBattle = FindBattle(ClientWorld);
	bool bValid = Test.TestNotNull(FString::Printf(
		TEXT("%s host projected BattleInfo exists"), Route), HostBattle);
	bValid = Test.TestNotNull(FString::Printf(
		TEXT("%s client projected BattleInfo exists"), Route), ClientBattle)
		&& bValid;
	if (!HostBattle || !ClientBattle) return false;
	bValid = Test.TestEqual(FString::Printf(
		TEXT("%s BattleInfo server/client names agree"), Route),
		HostBattle->GetFName(), ClientBattle->GetFName()) && bValid;
	bValid = Test.TestEqual(FString::Printf(
		TEXT("%s BattleInfo server/client canonical paths agree"), Route),
		WoTMaterializedListenHarnessCanonicalActorPath(*HostBattle),
		WoTMaterializedListenHarnessCanonicalActorPath(*ClientBattle)) && bValid;
	bValid = Test.TestTrue(FString::Printf(
		TEXT("%s client BattleInfo name is stable"), Route),
		ClientBattle->IsNameStableForNetworking()) && bValid;
	bValid = Test.TestEqual(FString::Printf(
		TEXT("%s client BattleInfo is a simulated proxy"), Route),
		ClientBattle->GetLocalRole(), ROLE_SimulatedProxy) && bValid;
	bValid = Test.TestEqual(FString::Printf(
		TEXT("%s BattleInfo team count replicates"), Route),
		ClientBattle->NumTeams, 2) && bValid;
	bValid = Test.TestTrue(FString::Printf(
		TEXT("%s BattleInfo faction classes replicate"), Route),
		ClientBattle->TeamClass[0] == AWoTAesSedai::StaticClass()
			&& ClientBattle->TeamClass[1] == AWoTHound::StaticClass()) && bValid;
	return bValid;
}

bool WoTMaterializedListenHarnessValidateProjectedMashadarGuide(
	FAutomationTestBase& Test, UWorld* HostWorld, UWorld* ClientWorld,
	const TCHAR* Route)
{
	auto FindGuide = [](UWorld* World) -> AWoTMashadarGuide*
	{
		if (!World) return nullptr;
		for (TActorIterator<AWoTMashadarGuide> It(World); It; ++It)
			if (It->GetFName() == TEXT("WoTProjected_5_MashadarGuide")) return *It;
		return nullptr;
	};
	AWoTMashadarGuide* HostGuide = FindGuide(HostWorld);
	AWoTMashadarGuide* ClientGuide = FindGuide(ClientWorld);
	bool bValid = Test.TestNotNull(FString::Printf(
		TEXT("%s host projected MashadarGuide exists"), Route), HostGuide);
	bValid = Test.TestNotNull(FString::Printf(
		TEXT("%s client projected MashadarGuide exists"), Route), ClientGuide)
		&& bValid;
	if (!HostGuide || !ClientGuide) return false;

	const FVector AuthoredLocation(700.0, 800.0, 900.0);
	const FRotator AuthoredRotation(0.0, 90.0, 0.0);
	const FVector ChainHeadLocation = AuthoredLocation
		+ AuthoredRotation.Vector() * ClientGuide->HeadDist;
	bValid = Test.TestEqual(FString::Printf(
		TEXT("%s MashadarGuide server/client names agree"), Route),
		HostGuide->GetFName(), ClientGuide->GetFName()) && bValid;
	bValid = Test.TestEqual(FString::Printf(
		TEXT("%s MashadarGuide server/client canonical paths agree"), Route),
		WoTMaterializedListenHarnessCanonicalActorPath(*HostGuide),
		WoTMaterializedListenHarnessCanonicalActorPath(*ClientGuide)) && bValid;
	bValid = Test.TestEqual(FString::Printf(
		TEXT("%s client MashadarGuide is a simulated proxy"), Route),
		ClientGuide->GetLocalRole(), ROLE_SimulatedProxy) && bValid;
	bValid = Test.TestEqual(FString::Printf(
		TEXT("%s client MashadarGuide initial location is authored"), Route),
		ClientGuide->InitialGuideLocation, AuthoredLocation) && bValid;
	bValid = Test.TestEqual(FString::Printf(
		TEXT("%s client MashadarGuide hiding place is authored"), Route),
		ClientGuide->HidingPlaceLocation, AuthoredLocation) && bValid;
	bValid = Test.TestTrue(FString::Printf(
		TEXT("%s client MashadarGuide rotation is authored"), Route),
		ClientGuide->GetActorRotation().Equals(AuthoredRotation)) && bValid;
	bValid = Test.TestTrue(FString::Printf(
		TEXT("%s client MashadarGuide begins one head length from its lair"), Route),
		ClientGuide->GetActorLocation().Equals(ChainHeadLocation, 0.1)) && bValid;

	AWoTMashadarSegment* HostHead = HostGuide->GetMasterSegment();
	AWoTMashadarSegment* ClientHead = ClientGuide->GetMasterSegment();
	bValid = Test.TestNotNull(FString::Printf(
		TEXT("%s host MashadarGuide chain head exists"), Route), HostHead) && bValid;
	bValid = Test.TestNotNull(FString::Printf(
		TEXT("%s client MashadarGuide chain head exists"), Route), ClientHead) && bValid;
	if (HostHead && ClientHead)
	{
		bValid = Test.TestTrue(FString::Printf(
			TEXT("%s host MashadarGuide chain head follows the guide"), Route),
			HostHead->HeadActor == HostGuide
				&& FMath::IsNearlyEqual(
					FVector::Dist(HostHead->GetActorLocation(),
						HostGuide->GetActorLocation()), HostGuide->HeadDist, 0.1f)
				&& FVector::DotProduct(
					(HostHead->GetActorLocation() - HostGuide->GetActorLocation())
						.GetSafeNormal(), HostGuide->GetActorForwardVector()) < 0.0f)
			&& bValid;
		bValid = Test.TestTrue(FString::Printf(
			TEXT("%s client MashadarGuide chain head follows the guide"), Route),
			ClientHead->HeadActor == ClientGuide
				&& FMath::IsNearlyEqual(
					FVector::Dist(ClientHead->GetActorLocation(),
						ClientGuide->GetActorLocation()), ClientGuide->HeadDist, 0.1f)
				&& FVector::DotProduct(
					(ClientHead->GetActorLocation() - ClientGuide->GetActorLocation())
						.GetSafeNormal(), ClientGuide->GetActorForwardVector()) < 0.0f)
			&& bValid;
	}
	return bValid;
}

#if WITH_EDITOR
bool WoTMaterializedListenHarnessUsableWorld(const UWorld* World)
{
	return World != nullptr
		&& !World->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed);
}

/**
 * Editor targets cannot add the game's global guarded definition to the shared
 * UnrealEditor build environment. The packaged-matrix process therefore has
 * the reviewed engine patch on disk, but its Development Engine dylib compiles
 * those call sites out. Adapt only this automation fixture back onto the same
 * permanent dispatcher: ordinary LoadMap candidates are intercepted at UE's
 * public pre-initialization boundary, before physics/AI/navigation/actor play;
 * seamless candidates are dispatched while still hidden in the handler, before
 * Tick transfers the net driver or tears down the current world.
 */
struct FWoTMaterializedListenHarnessEditorRouteAdapter
{
	~FWoTMaterializedListenHarnessEditorRouteAdapter()
	{
		Unbind();
	}

	void Bind(UGameInstance* InGameInstance)
	{
		GameInstance = InGameInstance;
#if !WOT_GUARDED_BOOTSTRAP
		if (!PreWorldInitializationHandle.IsValid())
		{
			PreWorldInitializationHandle =
				FWorldDelegates::OnPreWorldInitialization.AddRaw(
					this, &FWoTMaterializedListenHarnessEditorRouteAdapter::
						HandlePreWorldInitialization);
		}
#endif
	}

	void Unbind()
	{
#if !WOT_GUARDED_BOOTSTRAP
		if (PreWorldInitializationHandle.IsValid())
		{
			FWorldDelegates::OnPreWorldInitialization.Remove(
				PreWorldInitializationHandle);
			PreWorldInitializationHandle.Reset();
		}
#endif
		PendingPath.Reset();
		PendingPackage.Reset();
		GameInstance = nullptr;
	}

	bool Arm(FAutomationTestBase& Test, const FString& Package,
		EWoTWorldStartPath Path)
	{
#if WOT_GUARDED_BOOTSTRAP
		return true;
#else
		if (!Test.TestFalse(TEXT("route adapter has no stale candidate"),
			PendingPath.IsSet()))
		{
			return false;
		}
		PendingPath = Path;
		PendingPackage = Package;
		bLastObserved = false;
		bLastSucceeded = false;
		LastError.Reset();
		return true;
#endif
	}

	void Disarm()
	{
#if !WOT_GUARDED_BOOTSTRAP
		PendingPath.Reset();
		PendingPackage.Reset();
#endif
	}

	bool DispatchHiddenSeamlessCandidate(
		UWorld* Candidate, bool& bOutSucceeded)
	{
#if WOT_GUARDED_BOOTSTRAP
		bOutSucceeded = true;
		return false;
#else
		return DispatchIfMatching(
			Candidate, /*bRequireOwningGameInstance*/ false, bOutSucceeded);
#endif
	}

	bool VerifySucceeded(FAutomationTestBase& Test, const TCHAR* Label) const
	{
#if WOT_GUARDED_BOOTSTRAP
		return true;
#else
		bool bValid = Test.TestTrue(
			FString::Printf(TEXT("%s reached the guarded dispatcher"), Label),
			bLastObserved);
		bValid = Test.TestTrue(
			FString::Printf(TEXT("%s materialized and committed before observers"),
				Label), bLastSucceeded) && bValid;
		if (bLastObserved && !bLastSucceeded && !LastError.IsEmpty())
		{
			Test.AddError(FString::Printf(
				TEXT("%s guarded dispatch failed: %s"), Label, *LastError));
		}
		return bValid;
#endif
	}

	bool VerifyRejected(FAutomationTestBase& Test, const TCHAR* Label) const
	{
#if WOT_GUARDED_BOOTSTRAP
		return true;
#else
		return Test.TestTrue(
			FString::Printf(TEXT("%s reached the guarded dispatcher"), Label),
			bLastObserved)
			&& Test.TestFalse(FString::Printf(
				TEXT("%s completed materialization was rejected"), Label),
				bLastSucceeded);
#endif
	}

private:
	void HandlePreWorldInitialization(
		UWorld* Candidate, const UWorld::InitializationValues)
	{
#if !WOT_GUARDED_BOOTSTRAP
		bool bSucceeded = false;
		DispatchIfMatching(
			Candidate, /*bRequireOwningGameInstance*/ true, bSucceeded);
#endif
	}

	bool DispatchIfMatching(UWorld* Candidate,
		bool bRequireOwningGameInstance, bool& bOutSucceeded)
	{
		bOutSucceeded = false;
		if (!PendingPath.IsSet() || Candidate == nullptr
			|| (bRequireOwningGameInstance
				&& Candidate->GetGameInstance() != GameInstance)
			|| WoTMaterializedListenHarnessCanonicalPackage(*Candidate)
				!= PendingPackage)
		{
			return false;
		}

		const EWoTWorldStartPath Path = PendingPath.GetValue();
		PendingPath.Reset();
		PendingPackage.Reset();
		bLastObserved = true;
		LastError.Reset();
		bLastSucceeded = WoTMaterializedListenHarnessDispatchCandidate(
			Candidate, Path, LastError);
		bOutSucceeded = bLastSucceeded;
		return true;
	}

	UGameInstance* GameInstance = nullptr;
	FDelegateHandle PreWorldInitializationHandle;
	TOptional<EWoTWorldStartPath> PendingPath;
	FString PendingPackage;
	FString LastError;
	bool bLastObserved = false;
	bool bLastSucceeded = false;
};

/**
 * PIE initialization and UEngine::LoadMap both change the process globals used
 * by editor ticking. Keep the caller's editor context authoritative across
 * every fixture lifetime, including an early return from a failed route cell.
 */
struct FWoTMaterializedListenHarnessEditorWorldRestoration
{
	FWoTMaterializedListenHarnessEditorWorldRestoration()
		: Engine(GEngine)
		, PriorWorld(GWorld)
		, bPriorPIEWorld(GIsPlayInEditorWorld)
	{
		if (Engine == nullptr)
		{
			PriorWorld = nullptr;
			return;
		}
		FWorldContext* Context =
			WoTMaterializedListenHarnessUsableWorld(PriorWorld)
				? Engine->GetWorldContextFromWorld(PriorWorld) : nullptr;
		if (Context == nullptr)
		{
			for (const FWorldContext& Candidate : Engine->GetWorldContexts())
			{
				if (Candidate.WorldType == EWorldType::Editor
					&& WoTMaterializedListenHarnessUsableWorld(
						Candidate.World()))
				{
					Context = Engine->GetWorldContextFromHandle(
						Candidate.ContextHandle);
					PriorWorld = Candidate.World();
					break;
				}
			}
		}
		if (Context != nullptr)
		{
			ContextHandle = Context->ContextHandle;
		}
	}

	~FWoTMaterializedListenHarnessEditorWorldRestoration()
	{
		Restore();
	}

	void Restore() const
	{
		if (Engine == nullptr || GEngine != Engine)
		{
			return;
		}
		UWorld* RestoredWorld =
			WoTMaterializedListenHarnessUsableWorld(PriorWorld)
				? PriorWorld : nullptr;
		FWorldContext* Context = !ContextHandle.IsNone()
			? Engine->GetWorldContextFromHandle(ContextHandle) : nullptr;
		if (Context != nullptr && RestoredWorld != nullptr
			&& Context->World() != RestoredWorld)
		{
			Context->SetCurrentWorld(RestoredWorld);
		}
		if (Context != nullptr
			&& WoTMaterializedListenHarnessUsableWorld(Context->World()))
		{
			RestoredWorld = Context->World();
		}
		if (RestoredWorld == nullptr)
		{
			for (const FWorldContext& Candidate : Engine->GetWorldContexts())
			{
				if (Candidate.WorldType == EWorldType::Editor
					&& WoTMaterializedListenHarnessUsableWorld(
						Candidate.World()))
				{
					RestoredWorld = Candidate.World();
					break;
				}
			}
		}
		GWorld = RestoredWorld;
		GIsPlayInEditorWorld = bPriorPIEWorld;
	}

	UEngine* Engine = nullptr;
	UWorld* PriorWorld = nullptr;
	FName ContextHandle = NAME_None;
	bool bPriorPIEWorld = false;
};

struct FWoTMaterializedListenHarnessEngineScope
{
	FWoTMaterializedListenHarnessEngineScope()
		: PriorEngine(GEngine)
	{
		Engine = NewObject<UWoTGameEngine>(GetTransientPackage());
		Engine->AddToRoot();
		// This fixture intentionally avoids initializing a second full editor
		// engine, but InitializeStandalone still creates a dummy world through
		// GEngine. Seed the resolved class that UWorld::InitializeNewWorld needs
		// from the live editor engine before making the test engine authoritative.
		Engine->WorldSettingsClass = PriorEngine->WorldSettingsClass;
		// UEngine's stale-world verifier recognizes a live world only when the
		// engine performing the load owns a context for it.  The editor engine is
		// deliberately parked while this lightweight game engine exercises the
		// real LoadMap override, so mirror its current worlds as borrowed contexts.
		// This keeps the verifier authoritative for genuinely stale worlds without
		// transferring ownership of (or later tearing down) any editor world.
		for (const FWorldContext& PriorContext : PriorEngine->GetWorldContexts())
		{
			UWorld* BorrowedWorld = PriorContext.World();
			if (BorrowedWorld == nullptr)
			{
				continue;
			}
			FWorldContext& BorrowedContext =
				Engine->CreateNewWorldContext(PriorContext.WorldType);
			BorrowedContext.PIEInstance = PriorContext.PIEInstance;
			BorrowedContext.SetCurrentWorld(BorrowedWorld);
			BorrowedWorlds.Add(BorrowedWorld);
		}
		GEngine = Engine;
	}

	~FWoTMaterializedListenHarnessEngineScope()
	{
		if (Engine != nullptr)
		{
			TArray<FName> Handles;
			for (const FWorldContext& Context : Engine->GetWorldContexts())
			{
				Handles.Add(Context.ContextHandle);
			}
			for (const FName Handle : Handles)
			{
				FWorldContext* Context = Engine->GetWorldContextFromHandle(Handle);
				if (Context == nullptr)
				{
					continue;
				}
				UWorld* World = Context->World();
				if (World != nullptr && BorrowedWorlds.Contains(World))
				{
					Engine->DestroyWorldContext(World);
					continue;
				}
				if (World != nullptr && World->HasBegunPlay())
				{
					World->BeginTearingDown();
					World->EndPlay(EEndPlayReason::Quit);
				}
				if (Context->OwningGameInstance != nullptr)
				{
					Context->OwningGameInstance->Shutdown();
				}
				if (World != nullptr
					&& !World->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
				{
					Engine->ShutdownWorldNetDriver(World);
					Engine->DestroyWorldContext(World);
					World->DestroyWorld(false);
				}
			}
			Engine->RemoveFromRoot();
			Engine->MarkAsGarbage();
		}
		GEngine = PriorEngine;
		EditorWorldRestoration.Restore();
	}

	FWoTMaterializedListenHarnessEditorWorldRestoration EditorWorldRestoration;
	UWoTGameEngine* Engine = nullptr;
	UEngine* PriorEngine = nullptr;
	TSet<UWorld*> BorrowedWorlds;
};

bool WoTMaterializedListenHarnessLoadNormal(
	FAutomationTestBase& Test, UWoTGameEngine& Engine,
	UGameInstance& Instance, const FString& URL)
{
	FWorldContext& Context =
		Engine.GetWorldContextFromWorldChecked(Instance.GetWorld());
	FString Error;
	const FURL Parsed(&Context.LastURL, *URL, TRAVEL_Absolute);
	FWoTMaterializedListenHarnessEditorRouteAdapter RouteAdapter;
	RouteAdapter.Bind(&Instance);
	if (!RouteAdapter.Arm(
			Test, Parsed.Map, EWoTWorldStartPath::NormalInitialLoad))
	{
		return false;
	}
	const bool bLoaded =
		Engine.LoadMap(Context, Parsed, /*Pending*/ nullptr, Error);
	if (!bLoaded)
	{
		Test.AddError(FString::Printf(
			TEXT("normal UWoTGameEngine::LoadMap failed: %s"), *Error));
		return false;
	}
	return RouteAdapter.VerifySucceeded(Test, TEXT("normal LoadMap route"));
}

int32 WoTMaterializedListenHarnessUnusedPIEInstance()
{
	// Keep package prefixes unique even after a destroyed PIE context leaves a
	// package awaiting GC; the direct fallback must never reuse that package.
	static int32 LastPIEInstance = INDEX_NONE;
	int32 Maximum = LastPIEInstance;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		Maximum = FMath::Max(Maximum, Context.PIEInstance);
	}
	LastPIEInstance = Maximum + 1;
	return LastPIEInstance;
}

struct FWoTMaterializedListenHarnessPIERestoration
{
	FWoTMaterializedListenHarnessPIERestoration()
		: PriorWorld(GWorld)
		, bPriorPIEWorld(GIsPlayInEditorWorld)
	{
	}

	~FWoTMaterializedListenHarnessPIERestoration()
	{
		UWorld* RestoredWorld =
			WoTMaterializedListenHarnessUsableWorld(PriorWorld)
				? PriorWorld : nullptr;
		if (RestoredWorld == nullptr && GEngine != nullptr)
		{
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				if (Context.WorldType == EWorldType::Editor
					&& WoTMaterializedListenHarnessUsableWorld(
						Context.World()))
				{
					RestoredWorld = Context.World();
					break;
				}
			}
		}
		GWorld = RestoredWorld;
		GIsPlayInEditorWorld = bPriorPIEWorld;
	}

	UWorld* PriorWorld = nullptr;
	bool bPriorPIEWorld = false;
};

/**
 * Public-API equivalent of the engine's private net-test world wrapper.
 * InitializeForPlayInEditor creates each UWorld/FWorldContext pair. Host
 * Browse reaches UWorld::Listen, which uses CreateNamedNetDriver and
 * UNetDriver::InitListen. Remote Browse creates UPendingNetGame, whose
 * InitNetDriver uses CreateNamedNetDriver and UNetDriver::InitConnect.
 */
struct FWoTMaterializedListenHarnessWorldInstance
{
	FWoTMaterializedListenHarnessWorldInstance(
		FAutomationTestBase& Test, bool bClient)
		: bIsClient(bClient)
	{
		GameInstance = NewObject<UGameInstance>(GEngine);
		if (GameInstance == nullptr)
		{
			Test.AddError(TEXT("PIE route fixture could not allocate a game instance"));
			return;
		}
		GameInstance->AddToRoot();
		FGameInstancePIEParameters Parameters;
		Parameters.bSimulateInEditor = false;
		Parameters.bAnyBlueprintErrors = false;
		Parameters.bStartInSpectatorMode = false;
		Parameters.bRunAsDedicated = false;
		Parameters.bIsPrimaryPIEClient = false;
		Parameters.WorldFeatureLevel = ERHIFeatureLevel::Num;
		Parameters.EditorPlaySettings = nullptr;
		Parameters.NetMode = bClient ? PIE_Client : PIE_ListenServer;
		Parameters.OverrideMapURL = TEXT("/Engine/Maps/Entry");
		const int32 PIEInstance =
			WoTMaterializedListenHarnessUnusedPIEInstance();
		// FTemporaryPlayInEditorIDOverride updates the PIE debug context as it
		// changes the ID. Give it the context which the normal editor PIE path
		// establishes before creating the play world, so world construction never
		// observes a PIE ID without a matching context (or vice versa).
		FWorldContext& PIEContext =
			GEngine->CreateNewWorldContext(EWorldType::PIE);
		PIEContext.PIEInstance = PIEInstance;
		FGameInstancePIEResult Initialized = FGameInstancePIEResult::Success();
		{
			FTemporaryPlayInEditorIDOverride PIEOverride(PIEInstance);
			Initialized = GameInstance->InitializeForPlayInEditor(
				PIEInstance, Parameters);
		}
		bIsLoaded = Initialized.IsSuccess();
		if (!bIsLoaded)
		{
			Test.AddError(FString::Printf(TEXT("PIE route fixture failed: %s"),
				*Initialized.FailureReason.ToString()));
			return;
		}
		if (bClient)
		{
			FWorldContext* Context = GetWorldContext();
			Viewport = NewObject<UGameViewportClient>(
				GEngine, GEngine->GameViewportClientClass);
			if (Context == nullptr || Viewport == nullptr)
			{
				Test.AddError(TEXT("client PIE route fixture has no viewport context"));
				bIsLoaded = false;
				return;
			}
			Viewport->Init(*Context, GameInstance);
			Context->GameViewport = Viewport;
			FString PlayerError;
			Viewport->SetupInitialLocalPlayer(PlayerError);
			if (!PlayerError.IsEmpty())
			{
				Test.AddInfo(FString::Printf(
					TEXT("client PIE local player was unavailable; loopback may use the documented direct fallback: %s"),
					*PlayerError));
			}
		}
		RouteAdapter.Bind(GameInstance);
	}

	~FWoTMaterializedListenHarnessWorldInstance()
	{
		RouteAdapter.Unbind();
		UWorld* World = GetWorld();
		if (World != nullptr && World->HasBegunPlay())
		{
			World->BeginTearingDown();
			World->EndPlay(EEndPlayReason::EndPlayInEditor);
		}
		FWorldContext* Context = GetWorldContext();
		if (Context != nullptr)
		{
			Context->GameViewport = nullptr;
		}
		if (GameInstance != nullptr)
		{
			GameInstance->Shutdown();
		}
		if (World != nullptr
			&& !World->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
		{
			if (GWorld == World)
			{
				GWorld = nullptr;
			}
			GEngine->ShutdownWorldNetDriver(World);
			if (Context != nullptr)
			{
				GEngine->DestroyWorldContext(World);
			}
			World->DestroyWorld(/*bInformEngineOfWorld*/ false);
		}
		if (GameInstance != nullptr)
		{
			GameInstance->RemoveFromRoot();
		}
		GameInstance = nullptr;
		Viewport = nullptr;
	}

	FWoTMaterializedListenHarnessWorldInstance(
		const FWoTMaterializedListenHarnessWorldInstance&) = delete;
	FWoTMaterializedListenHarnessWorldInstance& operator=(
		const FWoTMaterializedListenHarnessWorldInstance&) = delete;

	UWorld* GetWorld() const
	{
		return GameInstance != nullptr ? GameInstance->GetWorld() : nullptr;
	}

	FWorldContext* GetWorldContext() const
	{
		return GameInstance != nullptr ? GameInstance->GetWorldContext() : nullptr;
	}

	bool IsLoaded() const
	{
		return bIsLoaded;
	}

	bool BrowseLocal(
		FAutomationTestBase& Test, const FString& URL, const TCHAR* Label,
		int32 ListenPort = 0)
	{
		FWorldContext* Context = GetWorldContext();
		if (Context == nullptr)
		{
			Test.AddError(FString::Printf(TEXT("%s has no world context"), Label));
			return false;
		}
		FWoTMaterializedListenHarnessPIERestoration Restoration;
		// The matrix owns multiple PIE contexts at once. UEngine::Browse takes
		// one explicitly, but LoadMap and PIE duplication still consult these
		// process globals; make them describe that same owning context.
		GWorld = GetWorld();
		GIsPlayInEditorWorld = true;
		FTemporaryPlayInEditorIDOverride PIEOverride(Context->PIEInstance);
		FString Error;
		FURL Parsed(&Context->LastURL, *URL, TRAVEL_Absolute);
		if (!RouteAdapter.Arm(
				Test, Parsed.Map, EWoTWorldStartPath::ListenHostTravel))
		{
			return false;
		}
		if (ListenPort > 0)
		{
			// FURL::Port is what UWorld::Listen passes to the net driver. A
			// "?port=" option is only game-option text and leaves the driver on
			// the default port, which also contaminates later relative travel.
			Parsed.Port = ListenPort;
		}
		const EBrowseReturnVal::Type Result =
			GEngine->Browse(*Context, Parsed, Error);
		bIsLoaded = Result == EBrowseReturnVal::Success;
		if (!bIsLoaded)
		{
			RouteAdapter.Disarm();
			Test.AddError(FString::Printf(
				TEXT("%s browse failed: %s"), Label, *Error));
		}
		else if (!RouteAdapter.VerifySucceeded(Test, Label))
		{
			bIsLoaded = false;
		}
		return bIsLoaded;
	}

	bool BeginLoopbackConnect(
		FAutomationTestBase& Test, int32 Port,
		const FString& ExpectedPackage, FString& OutError)
	{
		FWorldContext* Context = GetWorldContext();
		if (!bIsClient || Context == nullptr || Port <= 0)
		{
			OutError = TEXT("client loopback preconditions were incomplete");
			return false;
		}
		FWoTMaterializedListenHarnessPIERestoration Restoration;
		// Keep the implicit PIE state congruent with the explicit client context
		// for PendingNetGame creation and the subsequent travel dispatcher path.
		GWorld = GetWorld();
		GIsPlayInEditorWorld = true;
		FTemporaryPlayInEditorIDOverride PIEOverride(Context->PIEInstance);
		const FString URL = FString::Printf(TEXT("127.0.0.1:%d"), Port);
		const FURL Parsed(&Context->LastURL, *URL, TRAVEL_Absolute);
		if (!RouteAdapter.Arm(Test, ExpectedPackage,
				EWoTWorldStartPath::RemoteClientTravel))
		{
			OutError = TEXT("client route adapter could not arm");
			return false;
		}
		const EBrowseReturnVal::Type Result = GEngine->Browse(
			*Context, Parsed, OutError);
		if (Result == EBrowseReturnVal::Success)
		{
			return RouteAdapter.VerifySucceeded(
				Test, TEXT("remote-client LoadMap route"));
		}
		UPendingNetGame* Pending = Context->PendingNetGame.Get();
		if (Result == EBrowseReturnVal::Pending && Pending != nullptr
			&& Pending->NetDriver != nullptr
			&& Pending->NetDriver->ServerConnection != nullptr)
		{
			return true;
		}
		if (OutError.IsEmpty())
		{
			OutError = TEXT("PendingNetGame did not create an InitConnect net driver");
		}
		RouteAdapter.Disarm();
		return false;
	}

	bool ArmSeamlessRoute(FAutomationTestBase& Test,
		const FString& Package, EWoTWorldStartPath Path)
	{
		return RouteAdapter.Arm(Test, Package, Path);
	}

	bool VerifyRouteMaterialized(
		FAutomationTestBase& Test, const TCHAR* Label) const
	{
		return RouteAdapter.VerifySucceeded(Test, Label);
	}

	bool VerifyRouteRejected(
		FAutomationTestBase& Test, const TCHAR* Label) const
	{
		return RouteAdapter.VerifyRejected(Test, Label);
	}

	void Tick(float DeltaSeconds = 1.0f / 60.0f)
	{
		FWorldContext* Context = GetWorldContext();
		if (Context == nullptr)
		{
			return;
		}
		FWoTMaterializedListenHarnessPIERestoration Restoration;
		GWorld = GetWorld();
		GIsPlayInEditorWorld = true;
		FTemporaryPlayInEditorIDOverride PIEOverride(Context->PIEInstance);
		FSeamlessTravelHandler& Seamless = Context->SeamlessTravelHandler;
		if (UWorld* LoadedWorld =
				const_cast<UWorld*>(Seamless.GetLoadedWorld()))
		{
			bool bSucceeded = false;
			if (RouteAdapter.DispatchHiddenSeamlessCandidate(
					LoadedWorld, bSucceeded) && !bSucceeded)
			{
				// CancelTravel is intentionally not exported from Engine. Present the
				// rejected hidden candidate to the exported handler Tick as an invalid
				// level so Engine performs its own cancellation, without continuing
				// into TickWorldTravel's unrelated NextURL processing. Then dispose of
				// exactly that candidate while the context still owns the prior world.
				UPackage* RejectedPackage = LoadedWorld->GetOutermost();
				ULevel* RejectedPersistentLevel = LoadedWorld->PersistentLevel;
				LoadedWorld->PersistentLevel = nullptr;
				Seamless.Tick();
				LoadedWorld->PersistentLevel = RejectedPersistentLevel;
				LoadedWorld->DestroyWorld(false);
				LoadedWorld->MarkObjectsPendingKill();
				RejectedPackage->Rename(
					*MakeUniqueObjectName(nullptr, UPackage::StaticClass(),
						*FString::Printf(TEXT("%s_REJECTED"),
							*RejectedPackage->GetName())).ToString(),
					nullptr,
					REN_DontCreateRedirectors | REN_NonTransactional
						| REN_AllowPackageLinkerMismatch);
				RejectedPackage->MarkAsGarbage();
				++GFrameCounter;
				return;
			}
		}
		GEngine->TickWorldTravel(*Context, DeltaSeconds);
		if (UWorld* World = GetWorld())
		{
			World->Tick(LEVELTICK_All, DeltaSeconds);
			World->UpdateLevelStreaming();
		}
		++GFrameCounter;
	}

	int32 GetPort() const
	{
		const UWorld* World = GetWorld();
		UNetDriver* Driver = World != nullptr ? World->GetNetDriver() : nullptr;
		const TSharedPtr<const FInternetAddr> Address =
			Driver != nullptr ? Driver->GetLocalAddr() : nullptr;
		return Address.IsValid() ? Address->GetPort() : 0;
	}

	bool ReplaceWithDirectClientRoute(
		FAutomationTestBase& Test, const FString& PackageName,
		EWoTWorldStartPath Path)
	{
		RouteAdapter.Disarm();
		FWorldContext* Context = GetWorldContext();
		UWorld* OldWorld = GetWorld();
		if (!bIsClient || Context == nullptr || OldWorld == nullptr)
		{
			Test.AddError(TEXT("direct client fallback has no live PIE context"));
			return false;
		}
		// FSeamlessTravelHandler::CancelTravel is not ENGINE_API; the public
		// surface can only cancel the pending net game. If a seamless
		// transition is genuinely in flight this fallback cannot safely
		// replace it, so the cell reports honestly instead of proceeding.
		GEngine->CancelPending(OldWorld);
		if (Context->SeamlessTravelHandler.IsInTransition())
		{
			Test.AddError(TEXT(
				"direct client fallback cannot cancel an in-flight seamless travel"));
			return false;
		}
		const FString PIEPackageName = UWorld::ConvertToPIEPackageName(
			PackageName, Context->PIEInstance);
		if (FindPackage(nullptr, *PIEPackageName) != nullptr)
		{
			Test.AddError(TEXT("direct client fallback package is already resident"));
			return false;
		}
		UPackage* Package = nullptr;
		UWorld* Candidate = nullptr;
		{
			FTemporaryPlayInEditorIDOverride PIEOverride(Context->PIEInstance);
			Package = CreatePackage(*PIEPackageName);
			Package->SetPackageFlags(PKG_PlayInEditor | PKG_NewlyCreated);
			Package->SetPIEInstanceID(Context->PIEInstance);
			Candidate = UWorld::CreateWorld(
				EWorldType::PIE, false, FName(TEXT("World")), Package,
				/*bAddToRoot*/ false, ERHIFeatureLevel::Num,
				/*InIVS*/ nullptr, /*bInSkipInitWorld*/ true);
		}
		FString Error;
		if (!Test.TestNotNull(TEXT("direct client candidate exists"), Candidate)
			|| !Test.TestTrue(TEXT("direct client route materializes"),
				WoTGuardedStartup_MaterializeWorld(Candidate, Path, &Error)))
		{
			if (!Error.IsEmpty())
			{
				Test.AddError(Error);
			}
			return false;
		}

		if (OldWorld->HasBegunPlay())
		{
			OldWorld->EndPlay(EEndPlayReason::EndPlayInEditor);
		}
		GEngine->ShutdownWorldNetDriver(OldWorld);
		OldWorld->DestroyWorld(/*bInformEngineOfWorld*/ true, Candidate);
		Candidate->SetPlayInEditorInitialNetMode(NM_Client);
		Candidate->SetGameInstance(GameInstance);
		Context->SetCurrentWorld(Candidate);
		Context->LastURL = FURL(nullptr, *PackageName, TRAVEL_Absolute);

		FWoTMaterializedListenHarnessPIERestoration Restoration;
		GWorld = Candidate;
		GIsPlayInEditorWorld = true;
		FTemporaryPlayInEditorIDOverride PIEOverride(Context->PIEInstance);
		if (!Test.TestTrue(TEXT("direct client exposure commits"),
			WoTGuardedStartup_CommitWorldExposure(Candidate, &Error)))
		{
			if (!Error.IsEmpty())
			{
				Test.AddError(Error);
			}
			return false;
		}
		GEngine->PostCreatePIEWorld(Candidate);
		const FURL WorldURL(nullptr,
			*(PackageName + TEXT("?game=/Script/Engine.GameMode")),
			TRAVEL_Absolute);
		Candidate->SetGameMode(WorldURL);
		Candidate->InitializeActorsForPlay(WorldURL);
		Candidate->BeginPlay();
		bIsLoaded = true;
		return true;
	}

	UGameInstance* GameInstance = nullptr;
	UGameViewportClient* Viewport = nullptr;
	FWoTMaterializedListenHarnessEditorRouteAdapter RouteAdapter;
	bool bIsClient = false;
	bool bIsLoaded = false;
};

bool WoTMaterializedListenHarnessExerciseHostBattleHUD(
	FAutomationTestBase& Test, UWorld* HostWorld)
{
	AWoTBattleGameMode* Mode = HostWorld
		? HostWorld->GetAuthGameMode<AWoTBattleGameMode>() : nullptr;
	AWoTBattleInfo* Battle = nullptr;
	if (HostWorld)
	{
		for (TActorIterator<AWoTBattleInfo> It(HostWorld); It; ++It)
			if (It->GetFName() == TEXT("WoTProjected_4_BattleInfo"))
			{
				Battle = *It;
				break;
			}
	}
	if (!Test.TestNotNull(TEXT("Battle listen host game mode exists"), Mode)
		|| !Test.TestNotNull(TEXT("Battle listen host projected info exists"), Battle))
		return false;
	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTPlayerController* Controller =
		HostWorld->SpawnActor<AWoTPlayerController>(Spawn);
	if (!Test.TestNotNull(TEXT("Battle listen host controller exists"), Controller))
		return false;
	Controller->Player = NewObject<ULocalPlayer>(GEngine);
	Controller->InitPlayerState();
	Mode->PostLogin(Controller);
	AWoTPlayerState* State = Controller->GetPlayerState<AWoTPlayerState>();
	AWoTHUD* HUD = HostWorld->SpawnActor<AWoTHUD>(Spawn);
	if (!Test.TestNotNull(TEXT("Battle listen host player state exists"), State)
		|| !Test.TestNotNull(TEXT("Battle listen host HUD exists"), HUD))
		return false;
	HUD->PlayerOwner = Controller;
	bool bValid = Test.TestEqual(TEXT("Battle listen host PostLogin is neutral"),
		static_cast<int32>(State->Team), 255);
	bValid = Test.TestTrue(TEXT("Battle listen host HUD digit joins team zero"),
		HUD->HandleTeamSelectDigit(0)) && bValid;
	bValid = Test.TestEqual(TEXT("Battle listen host state joined team zero"),
		static_cast<int32>(State->Team), 0) && bValid;
	bValid = Test.TestEqual(TEXT("Battle listen host count records its HUD join"),
		Battle->NumPlayers[0], 1) && bValid;
	Mode->ChangeTeam(Controller, 255);
	bValid = Test.TestEqual(TEXT("Battle listen host cleanup restores count"),
		Battle->NumPlayers[0], 0) && bValid;
	APawn* Pawn = Controller->GetPawn();
	Controller->UnPossess();
	if (IsValid(Pawn)) Pawn->Destroy();
	HUD->Destroy();
	Controller->Destroy();
	return bValid;
}

bool WoTMaterializedListenHarnessExerciseClientBattleHUD(
	FAutomationTestBase& Test,
	FWoTMaterializedListenHarnessWorldInstance& Host,
	FWoTMaterializedListenHarnessWorldInstance& Client,
	const TCHAR* Route, int32 Slot)
{
	UWorld* HostWorld = Host.GetWorld();
	UWorld* ClientWorld = Client.GetWorld();
	AWoTBattleInfo* HostBattle = nullptr;
	AWoTBattleInfo* ClientBattle = nullptr;
	if (HostWorld)
	{
		for (TActorIterator<AWoTBattleInfo> It(HostWorld); It; ++It)
			if (It->GetFName() == TEXT("WoTProjected_4_BattleInfo")) HostBattle = *It;
	}
	if (ClientWorld)
	{
		for (TActorIterator<AWoTBattleInfo> It(ClientWorld); It; ++It)
			if (It->GetFName() == TEXT("WoTProjected_4_BattleInfo")) ClientBattle = *It;
	}
	AWoTPlayerController* ClientController = Cast<AWoTPlayerController>(
		ClientWorld ? ClientWorld->GetFirstPlayerController() : nullptr);
	AWoTPlayerState* ClientState = ClientController
		? ClientController->GetPlayerState<AWoTPlayerState>() : nullptr;
	AWoTHUD* ClientHUD = ClientController
		? Cast<AWoTHUD>(ClientController->GetHUD()) : nullptr;
	if (!Test.TestNotNull(TEXT("Battle client projected info exists"), ClientBattle)
		|| !Test.TestNotNull(TEXT("Battle client controller exists"), ClientController)
		|| !Test.TestNotNull(TEXT("Battle client state exists"), ClientState)
		|| !Test.TestNotNull(TEXT("Battle client owns its production HUD"), ClientHUD)
		|| !Test.TestNotNull(TEXT("Battle host projected info exists"), HostBattle))
		return false;
	if (!Test.TestEqual(TEXT("Battle client PostLogin is neutral"),
		static_cast<int32>(ClientState->Team), 255)
		|| !Test.TestTrue(TEXT("Battle client HUD digit sends team-one join"),
			ClientHUD->HandleTeamSelectDigit(1)))
		return false;

	const double StartSeconds = FPlatformTime::Seconds();
	int32 TickCount = 0;
	while (TickCount < WoTMaterializedListenHarnessMaximumWaitTicks
		&& FPlatformTime::Seconds() - StartSeconds
			< WoTMaterializedListenHarnessMaximumWaitSeconds)
	{
		Host.Tick();
		Client.Tick();
		++TickCount;
		if (HostBattle->NumPlayers[1] == 1
			&& ClientBattle->NumPlayers[1] == 1
			&& ClientState->Team == 1)
		{
			return true;
		}
		FPlatformProcess::SleepNoStats(0.001f);
	}
	Test.AddError(FString::Printf(
		TEXT("Battle HUD join replication deadline expired: route=%s shell=%02d host-count=%d client-count=%d client-team=%d"),
		Route, Slot, HostBattle->NumPlayers[1], ClientBattle->NumPlayers[1],
		static_cast<int32>(ClientState->Team)));
	return false;
}

bool WoTMaterializedListenHarnessWaitForPair(
	FAutomationTestBase& Test,
	FWoTMaterializedListenHarnessWorldInstance& Host,
	FWoTMaterializedListenHarnessWorldInstance& Client,
	const FString& Package, const TCHAR* Route, int32 Slot)
{
	const double StartSeconds = FPlatformTime::Seconds();
	int32 TickCount = 0;
	while (TickCount < WoTMaterializedListenHarnessMaximumWaitTicks
		&& FPlatformTime::Seconds() - StartSeconds
			< WoTMaterializedListenHarnessMaximumWaitSeconds)
	{
		Host.Tick();
		Client.Tick();
		++TickCount;
		UWorld* HostWorld = Host.GetWorld();
		UWorld* ClientWorld = Client.GetWorld();
		UNetDriver* ClientDriver = ClientWorld != nullptr
			? ClientWorld->GetNetDriver() : nullptr;
		if (HostWorld != nullptr && ClientWorld != nullptr
			&& WoTMaterializedListenHarnessCanonicalPackage(*HostWorld) == Package
			&& WoTMaterializedListenHarnessCanonicalPackage(*ClientWorld) == Package
			&& ClientWorld->HasBegunPlay()
			&& ClientWorld->GetNetMode() == NM_Client
			&& ClientDriver != nullptr && ClientDriver->ServerConnection != nullptr
			&& ClientDriver->ServerConnection->GetConnectionState() == USOCK_Open)
		{
			bool bPoolReady = false;
			bool bMoverReady = false;
			bool bBattleReady = false;
			bool bGuideReady = false;
			for (TActorIterator<AActor> It(ClientWorld); It; ++It)
			{
				const FName Name = It->GetFName();
				const bool bReplicatedProxy =
					It->GetLocalRole() == ROLE_SimulatedProxy;
				if (Name == TEXT("WoTProjected_1_SpawnPool"))
					bPoolReady = bReplicatedProxy;
				else if (Name == TEXT("WoTProjected_2_Mover"))
					bMoverReady = bReplicatedProxy;
				else if (Name == TEXT("WoTProjected_4_BattleInfo"))
					bBattleReady = bReplicatedProxy;
				else if (Name == TEXT("WoTProjected_5_MashadarGuide"))
					bGuideReady = bReplicatedProxy;
			}
			if (bPoolReady && bMoverReady && bBattleReady && bGuideReady)
				return true;
		}
		FPlatformProcess::SleepNoStats(0.001f);
	}
	UWorld* HostWorld = Host.GetWorld();
	UWorld* ClientWorld = Client.GetWorld();
	UNetDriver* ClientDriver = ClientWorld != nullptr
		? ClientWorld->GetNetDriver() : nullptr;
	UNetConnection* ServerConnection = ClientDriver != nullptr
		? ClientDriver->ServerConnection : nullptr;
	Test.AddError(FString::Printf(
		TEXT("route cell deadline expired: route=%s shell=%02d ticks=%d/%d wall=%.3fs/%.3fs host=%s client=%s client-net-mode=%d connection-state=%d"),
		Route, Slot, TickCount, WoTMaterializedListenHarnessMaximumWaitTicks,
		FPlatformTime::Seconds() - StartSeconds,
		WoTMaterializedListenHarnessMaximumWaitSeconds,
		HostWorld != nullptr
			? *WoTMaterializedListenHarnessCanonicalPackage(*HostWorld)
			: TEXT("<null>"),
		ClientWorld != nullptr
			? *WoTMaterializedListenHarnessCanonicalPackage(*ClientWorld)
			: TEXT("<null>"),
		ClientWorld != nullptr ? static_cast<int32>(ClientWorld->GetNetMode()) : -1,
		ServerConnection != nullptr
			? static_cast<int32>(ServerConnection->GetConnectionState()) : -1));
	return false;
}

bool WoTMaterializedListenHarnessExerciseProjectedDoor(
	FAutomationTestBase& Test,
	FWoTMaterializedListenHarnessWorldInstance& Host,
	FWoTMaterializedListenHarnessWorldInstance& Client,
	const TCHAR* Route, int32 Slot)
{
	auto FindMover = [](UWorld* World) -> AWoTDoorMover*
	{
		if (!World) return nullptr;
		for (TActorIterator<AWoTDoorMover> It(World); It; ++It)
			if (It->GetFName() == TEXT("WoTProjected_2_Mover")) return *It;
		return nullptr;
	};
	auto FindTrigger = [](UWorld* World) -> AWoTDoorTrigger*
	{
		if (!World) return nullptr;
		for (TActorIterator<AWoTDoorTrigger> It(World); It; ++It)
			if (It->GetFName() == TEXT("WoTProjected_3_DoorTrigger")) return *It;
		return nullptr;
	};
	AWoTDoorMover* HostMover = FindMover(Host.GetWorld());
	AWoTDoorMover* ClientMover = FindMover(Client.GetWorld());
	AWoTDoorTrigger* HostTrigger = FindTrigger(Host.GetWorld());
	if (!Test.TestNotNull(FString::Printf(TEXT("%s host door exists"), Route),
			HostMover)
		|| !Test.TestNotNull(FString::Printf(TEXT("%s client door exists"), Route),
			ClientMover)
		|| !Test.TestNotNull(FString::Printf(TEXT("%s host door trigger exists"), Route),
			HostTrigger)) return false;

	const FVector HostStart = HostMover->GetActorLocation();
	const FVector ClientStart = ClientMover->GetActorLocation();
	bool bValid = Test.TestTrue(FString::Printf(
		TEXT("%s projected door starts at the same pose"), Route),
		HostStart.Equals(ClientStart, 0.1));
	HostTrigger->WoTTrigger(nullptr, nullptr);
	HostMover->ForceNetUpdate();
	bool bHostTraversedPath = false;
	bool bClientTraversedPath = false;
	bool bStayedOnAuthoredPath = true;
	bool bOpenedTogether = false;
	const double StartSeconds = FPlatformTime::Seconds();
	int32 TickCount = 0;
	while (TickCount < WoTMaterializedListenHarnessMaximumWaitTicks
		&& FPlatformTime::Seconds() - StartSeconds
			< WoTMaterializedListenHarnessMaximumWaitSeconds)
	{
		Host.Tick();
		Client.Tick();
		++TickCount;
		HostMover = FindMover(Host.GetWorld());
		ClientMover = FindMover(Client.GetWorld());
		if (!HostMover || !ClientMover) break;
		const FVector HostAt = HostMover->GetActorLocation();
		const FVector ClientAt = ClientMover->GetActorLocation();
		bHostTraversedPath |= HostAt.Z > HostStart.Z + 1.0
			&& HostAt.Z < HostStart.Z + 99.0;
		bClientTraversedPath |= ClientAt.Z > ClientStart.Z + 1.0
			&& ClientAt.Z < ClientStart.Z + 99.0;
		const bool bSameXY = FMath::IsNearlyEqual(HostAt.X, HostStart.X, 0.1)
			&& FMath::IsNearlyEqual(HostAt.Y, HostStart.Y, 0.1)
			&& FMath::IsNearlyEqual(ClientAt.X, ClientStart.X, 0.1)
			&& FMath::IsNearlyEqual(ClientAt.Y, ClientStart.Y, 0.1);
		bStayedOnAuthoredPath &= bSameXY;
		if (HostMover->GetDoorPhase() == EWoTDoorPhase::Dwelling
			&& ClientMover->GetDoorPhase() == EWoTDoorPhase::Dwelling
			&& HostMover->GetOpenKey() == 1 && ClientMover->GetOpenKey() == 1
			&& FMath::IsNearlyEqual(HostAt.Z, HostStart.Z + 100.0, 1.0)
			&& FMath::IsNearlyEqual(ClientAt.Z, ClientStart.Z + 100.0, 1.0))
		{
			bOpenedTogether = true;
			break;
		}
		FPlatformProcess::SleepNoStats(0.001f);
	}
	bValid = Test.TestTrue(FString::Printf(
		TEXT("%s host door traversed its authored path"), Route),
		bHostTraversedPath) && bValid;
	bValid = Test.TestTrue(FString::Printf(
		TEXT("%s client door followed the authored path"), Route),
		bClientTraversedPath) && bValid;
	bValid = Test.TestTrue(FString::Printf(
		TEXT("%s host/client remain on the authored door path"), Route),
		bStayedOnAuthoredPath) && bValid;
	if (!bOpenedTogether)
	{
		Test.AddError(FString::Printf(
			TEXT("projected door replication deadline expired: route=%s shell=%02d ticks=%d host-phase=%d client-phase=%d host-z=%.2f client-z=%.2f"),
			Route, Slot, TickCount,
			HostMover ? static_cast<int32>(HostMover->GetDoorPhase()) : -1,
			ClientMover ? static_cast<int32>(ClientMover->GetDoorPhase()) : -1,
			HostMover ? HostMover->GetActorLocation().Z : -1.0,
			ClientMover ? ClientMover->GetActorLocation().Z : -1.0));
		bValid = false;
	}
	return bValid;
}

bool WoTMaterializedListenHarnessWaitForHost(
	FAutomationTestBase& Test,
	FWoTMaterializedListenHarnessWorldInstance& Host,
	FWoTMaterializedListenHarnessWorldInstance* Client,
	const FString& Package, const TCHAR* Route, int32 Slot)
{
	const double StartSeconds = FPlatformTime::Seconds();
	int32 TickCount = 0;
	while (TickCount < WoTMaterializedListenHarnessMaximumWaitTicks
		&& FPlatformTime::Seconds() - StartSeconds
			< WoTMaterializedListenHarnessMaximumWaitSeconds)
	{
		if (Client != nullptr)
		{
			// Let the in-process client consume ClientTravel before the host
			// completes its switch and retires old-world NetGUID outers.
			Client->Tick();
		}
		Host.Tick();
		++TickCount;
		UWorld* HostWorld = Host.GetWorld();
		FWorldContext* HostContext = Host.GetWorldContext();
		if (HostWorld != nullptr
			&& WoTMaterializedListenHarnessCanonicalPackage(*HostWorld) == Package
			&& HostContext != nullptr
			&& !HostContext->SeamlessTravelHandler.IsInTransition())
		{
			return true;
		}
		FPlatformProcess::SleepNoStats(0.001f);
	}
	UWorld* HostWorld = Host.GetWorld();
	FWorldContext* HostContext = Host.GetWorldContext();
	Test.AddError(FString::Printf(
		TEXT("route cell deadline expired: route=%s shell=%02d ticks=%d/%d wall=%.3fs/%.3fs host=%s seamless-transition=%s"),
		Route, Slot, TickCount, WoTMaterializedListenHarnessMaximumWaitTicks,
		FPlatformTime::Seconds() - StartSeconds,
		WoTMaterializedListenHarnessMaximumWaitSeconds,
		HostWorld != nullptr
			? *WoTMaterializedListenHarnessCanonicalPackage(*HostWorld)
			: TEXT("<null>"),
		HostContext != nullptr
			&& HostContext->SeamlessTravelHandler.IsInTransition()
				? TEXT("true") : TEXT("false")));
	return false;
}

bool WoTMaterializedListenHarnessWaitForTransitionEnd(
	FAutomationTestBase& Test,
	FWoTMaterializedListenHarnessWorldInstance& Host,
	const TCHAR* Route, int32 Slot)
{
	const double StartSeconds = FPlatformTime::Seconds();
	int32 TickCount = 0;
	FWorldContext* HostContext = Host.GetWorldContext();
	while (HostContext != nullptr
		&& HostContext->SeamlessTravelHandler.IsInTransition()
		&& TickCount < WoTMaterializedListenHarnessMaximumWaitTicks
		&& FPlatformTime::Seconds() - StartSeconds
			< WoTMaterializedListenHarnessMaximumWaitSeconds)
	{
		Host.Tick();
		++TickCount;
		HostContext = Host.GetWorldContext();
		FPlatformProcess::SleepNoStats(0.001f);
	}
	if (HostContext != nullptr
		&& !HostContext->SeamlessTravelHandler.IsInTransition())
	{
		return true;
	}
	UWorld* HostWorld = Host.GetWorld();
	Test.AddError(FString::Printf(
		TEXT("route cell deadline expired: route=%s shell=%02d ticks=%d/%d wall=%.3fs/%.3fs host=%s seamless-transition=%s"),
		Route, Slot, TickCount, WoTMaterializedListenHarnessMaximumWaitTicks,
		FPlatformTime::Seconds() - StartSeconds,
		WoTMaterializedListenHarnessMaximumWaitSeconds,
		HostWorld != nullptr
			? *WoTMaterializedListenHarnessCanonicalPackage(*HostWorld)
			: TEXT("<null>"),
		HostContext != nullptr
			&& HostContext->SeamlessTravelHandler.IsInTransition()
				? TEXT("true") : TEXT("<no-context>")));
	return false;
}

void WoTMaterializedListenHarnessRecord(
	TSet<FString>& Coverage, const TCHAR* Route, int32 Slot,
	const TCHAR* Result)
{
	Coverage.Add(FString::Printf(TEXT("%s:%02d"), Route, Slot));
	UE_LOG(LogTemp, Display,
		TEXT("WOT listen matrix cell route=%s shell=%02d result=%s"),
		Route, Slot, Result);
}

void WoTMaterializedListenHarnessLogFailedCell(
	const TCHAR* Route, int32 Slot)
{
	UE_LOG(LogTemp, Error,
		TEXT("WOT listen matrix cell route=%s shell=%02d result=failed"),
		Route, Slot);
}

bool WoTMaterializedListenHarnessRunRealMatrix(FAutomationTestBase& Test)
{
	FWoTMaterializedListenHarnessEditorWorldRestoration MatrixWorldRestoration;
	TSet<FString> Coverage;
	TSet<FString> DirectClientCoverage;
	const int32 RotatingSlots[] = {1, 5, 10, 15, 19};
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(RotatingSlots); ++Index)
	{
		const bool bBattleHUDCell = Index == 0;
		const int32 InitialSlot = RotatingSlots[Index];
		const int32 SeamlessSlot =
			RotatingSlots[(Index + 1) % UE_ARRAY_COUNT(RotatingSlots)];
		const FString InitialPackage = FString::Printf(
			TEXT("/WoTNeutralShells/WoTMapShell%02d"), InitialSlot);
		const FString SeamlessPackage = FString::Printf(
			TEXT("/WoTNeutralShells/WoTMapShell%02d"), SeamlessSlot);
		const int32 RequestedPort = 17770 + Index;

		FWoTMaterializedListenHarnessEditorWorldRestoration CellWorldRestoration;
		FWoTMaterializedListenHarnessWorldInstance Host(
			Test, /*bClient*/ false);
		FWoTMaterializedListenHarnessWorldInstance Client(
			Test, /*bClient*/ true);
		if (!Host.IsLoaded() || !Client.IsLoaded()
			|| !Host.BrowseLocal(
				Test,
				InitialPackage + (bBattleHUDCell
					? TEXT("?listen?game=/Script/WoT.WoTBattleGameMode")
					: TEXT("?listen?game=/Script/Engine.GameMode")),
				TEXT("listen-host route"), RequestedPort))
		{
			WoTMaterializedListenHarnessLogFailedCell(
				TEXT("host-travel"), InitialSlot);
			return false;
		}
		UWorld* HostWorld = Host.GetWorld();
		UNetDriver* HostDriver = HostWorld != nullptr
			? HostWorld->GetNetDriver() : nullptr;
		if (!Test.TestNotNull(TEXT("listen-host net driver is up"), HostDriver)
			|| !Test.TestEqual(TEXT("host is a real listen server"),
				HostWorld->GetNetMode(), NM_ListenServer))
		{
			WoTMaterializedListenHarnessLogFailedCell(
				TEXT("host-travel"), InitialSlot);
			return false;
		}
		const int32 Port = Host.GetPort();
		if (!Test.TestTrue(TEXT("listen-host bound a real port"), Port > 0)
			|| !Test.TestEqual(TEXT("listen-host bound the requested FURL port"),
				Port, RequestedPort))
		{
			WoTMaterializedListenHarnessLogFailedCell(
				TEXT("host-travel"), InitialSlot);
			return false;
		}
		const bool bHostTravelValid =
			WoTMaterializedListenHarnessValidateExposedWorld(
				Test, Host.GetWorld(), InitialSlot, TEXT("listen-host world"));
		if (!bHostTravelValid)
		{
			WoTMaterializedListenHarnessLogFailedCell(
				TEXT("host-travel"), InitialSlot);
			return false;
		}
		WoTMaterializedListenHarnessRecord(
			Coverage, TEXT("host-travel"), InitialSlot, TEXT("passed-real"));
		AGameModeBase* InitialHostMode = Host.GetWorld()->GetAuthGameMode();
		if (!Test.TestNotNull(
				TEXT("listen host game mode exists before client join"),
				InitialHostMode))
		{
			WoTMaterializedListenHarnessLogFailedCell(
				TEXT("client-travel"), InitialSlot);
			return false;
		}
		// The route matrix needs a real PlayerController connection, but no
		// gameplay pawn. Avoid carrying an old-world DefaultPawn NetGUID into the
		// deliberately immediate in-process seamless handoff.
		InitialHostMode->DefaultPawnClass = nullptr;

		FString ConnectError;
		const bool bConnectStarted =
			Client.BeginLoopbackConnect(
				Test, Port, InitialPackage, ConnectError);
		bool bRealClientRoute = bConnectStarted;
		if (bConnectStarted
			&& !WoTMaterializedListenHarnessWaitForPair(
				Test, Host, Client, InitialPackage,
				TEXT("client-travel"), InitialSlot))
		{
			WoTMaterializedListenHarnessLogFailedCell(
				TEXT("client-travel"), InitialSlot);
			return false;
		}
		if (bConnectStarted
			&& !Client.VerifyRouteMaterialized(
				Test, TEXT("remote-client travel route")))
		{
			WoTMaterializedListenHarnessLogFailedCell(
				TEXT("client-travel"), InitialSlot);
			return false;
		}
		if (!bRealClientRoute)
		{
			Test.AddInfo(FString::Printf(
				TEXT("client-travel:%02d used direct dispatcher because the headless loopback could not start: %s"),
				InitialSlot, ConnectError.IsEmpty()
					? TEXT("no InitConnect net driver") : *ConnectError));
			if (!Client.ReplaceWithDirectClientRoute(
					Test, InitialPackage, EWoTWorldStartPath::RemoteClientTravel))
			{
				WoTMaterializedListenHarnessLogFailedCell(
					TEXT("client-travel"), InitialSlot);
				return false;
			}
			DirectClientCoverage.Add(FString::Printf(
				TEXT("client-travel:%02d"), InitialSlot));
		}
		const bool bClientTravelValid =
			WoTMaterializedListenHarnessValidateExposedWorld(
				Test, Client.GetWorld(), InitialSlot, TEXT("remote-client world"));
		if (!bClientTravelValid)
		{
			WoTMaterializedListenHarnessLogFailedCell(
				TEXT("client-travel"), InitialSlot);
			return false;
		}
		if (bBattleHUDCell
			&& !WoTMaterializedListenHarnessExerciseHostBattleHUD(
				Test, Host.GetWorld()))
		{
			WoTMaterializedListenHarnessLogFailedCell(
				TEXT("host-battle-hud"), InitialSlot);
			return false;
		}
		if (!WoTMaterializedListenHarnessValidateProjectedPool(
				Test, Host.GetWorld(), Client.GetWorld(), TEXT("client-travel")))
		{
			WoTMaterializedListenHarnessLogFailedCell(
				TEXT("client-travel"), InitialSlot);
			return false;
		}
		if (!WoTMaterializedListenHarnessValidateProjectedDoorActors(
				Test, Host.GetWorld(), Client.GetWorld(), TEXT("client-travel")))
		{
			WoTMaterializedListenHarnessLogFailedCell(
				TEXT("client-travel"), InitialSlot);
			return false;
		}
		if (!WoTMaterializedListenHarnessValidateProjectedBattleInfo(
				Test, Host.GetWorld(), Client.GetWorld(), TEXT("client-travel")))
		{
			WoTMaterializedListenHarnessLogFailedCell(
				TEXT("client-travel"), InitialSlot);
			return false;
		}
		if (!WoTMaterializedListenHarnessValidateProjectedMashadarGuide(
				Test, Host.GetWorld(), Client.GetWorld(), TEXT("client-travel")))
		{
			WoTMaterializedListenHarnessLogFailedCell(
				TEXT("client-travel"), InitialSlot);
			return false;
		}
		if (bBattleHUDCell && bRealClientRoute
			&& !WoTMaterializedListenHarnessExerciseClientBattleHUD(
				Test, Host, Client, TEXT("client-travel"), InitialSlot))
		{
			WoTMaterializedListenHarnessLogFailedCell(
				TEXT("client-battle-hud"), InitialSlot);
			return false;
		}
		if (bBattleHUDCell && !bRealClientRoute)
		{
			Test.AddInfo(TEXT(
				"client Battle HUD/count replication check skipped because this cell used the declared direct-client fallback"));
		}
		if (bRealClientRoute)
		{
			if (!WoTMaterializedListenHarnessExerciseProjectedDoor(
					Test, Host, Client, TEXT("client-travel"), InitialSlot))
			{
				WoTMaterializedListenHarnessLogFailedCell(
					TEXT("client-travel"), InitialSlot);
				return false;
			}
		}
		else
		{
			Test.AddInfo(TEXT(
				"client-travel projected-door movement replication skipped because this cell used the declared direct-client fallback"));
		}
		WoTMaterializedListenHarnessRecord(
			Coverage, TEXT("client-travel"), InitialSlot,
			bRealClientRoute ? TEXT("passed-real")
				: TEXT("passed-direct-fallback"));
		AWoTMaterializedWorldRoot* InitialHostRoot =
			WoTMaterializedListenHarnessRoot(Host.GetWorld());
		AWoTMaterializedWorldRoot* InitialClientRoot =
			WoTMaterializedListenHarnessRoot(Client.GetWorld());
		if (Test.TestNotNull(
				TEXT("host-travel semantic root exists"), InitialHostRoot)
			&& Test.TestNotNull(
				TEXT("client-travel semantic root exists"), InitialClientRoot))
		{
			Test.TestEqual(TEXT("host/client travel semantic roots agree"),
				InitialHostRoot->MapRootHex, InitialClientRoot->MapRootHex);
		}

		AGameModeBase* HostMode = Host.GetWorld()->GetAuthGameMode();
		if (!Test.TestNotNull(TEXT("listen host game mode exists"), HostMode))
		{
			WoTMaterializedListenHarnessLogFailedCell(
				TEXT("host-seamless"), SeamlessSlot);
			return false;
		}
		HostMode->bUseSeamlessTravel = true;
		// PIE deliberately disables seamless travel by default. Each seamless
		// route cell opts in for only its own lifetime and restores the caller's
		// cvar value on every return path.
		TGuardConsoleVariable<bool> AllowPIESeamlessTravel(
			TEXT("net.AllowPIESeamlessTravel"), true);
		if (!Host.ArmSeamlessRoute(
				Test, SeamlessPackage,
				EWoTWorldStartPath::ListenHostSeamlessTravel)
			|| (bRealClientRoute && !Client.ArmSeamlessRoute(
				Test, SeamlessPackage,
				EWoTWorldStartPath::RemoteClientSeamlessTravel))
			|| !Test.TestTrue(TEXT("host accepted seamless ServerTravel"),
			Host.GetWorld()->ServerTravel(
				SeamlessPackage + TEXT("?SeamlessTravel"), false)))
		{
			WoTMaterializedListenHarnessLogFailedCell(
				TEXT("host-seamless"), SeamlessSlot);
			return false;
		}
		if (!WoTMaterializedListenHarnessWaitForHost(
				Test, Host, bRealClientRoute ? &Client : nullptr,
				SeamlessPackage, TEXT("host-seamless"), SeamlessSlot))
		{
			WoTMaterializedListenHarnessLogFailedCell(
				TEXT("host-seamless"), SeamlessSlot);
			return false;
		}
		if (!Host.VerifyRouteMaterialized(
				Test, TEXT("listen-host seamless route")))
		{
			WoTMaterializedListenHarnessLogFailedCell(
				TEXT("host-seamless"), SeamlessSlot);
			return false;
		}
		const bool bHostSeamlessValid =
			WoTMaterializedListenHarnessValidateExposedWorld(
				Test, Host.GetWorld(), SeamlessSlot,
				TEXT("listen-host seamless world"));
		if (!bHostSeamlessValid)
		{
			WoTMaterializedListenHarnessLogFailedCell(
				TEXT("host-seamless"), SeamlessSlot);
			return false;
		}
		WoTMaterializedListenHarnessRecord(
			Coverage, TEXT("host-seamless"), SeamlessSlot,
			TEXT("passed-real"));

		if (bRealClientRoute)
		{
			if (!WoTMaterializedListenHarnessWaitForPair(
					Test, Host, Client, SeamlessPackage,
					TEXT("client-seamless"), SeamlessSlot))
			{
				WoTMaterializedListenHarnessLogFailedCell(
					TEXT("client-seamless"), SeamlessSlot);
				return false;
			}
			if (!Client.VerifyRouteMaterialized(
					Test, TEXT("remote-client seamless route")))
			{
				WoTMaterializedListenHarnessLogFailedCell(
					TEXT("client-seamless"), SeamlessSlot);
				return false;
			}
		}
		else
		{
			Test.AddInfo(FString::Printf(
				TEXT("client-seamless:%02d used direct dispatcher because its initial headless loopback could not start"),
				SeamlessSlot));
			if (!Client.ReplaceWithDirectClientRoute(
					Test, SeamlessPackage,
					EWoTWorldStartPath::RemoteClientSeamlessTravel))
			{
				WoTMaterializedListenHarnessLogFailedCell(
					TEXT("client-seamless"), SeamlessSlot);
				return false;
			}
			DirectClientCoverage.Add(FString::Printf(
				TEXT("client-seamless:%02d"), SeamlessSlot));
		}
		AWoTMaterializedWorldRoot* HostRoot =
			WoTMaterializedListenHarnessRoot(Host.GetWorld());
		AWoTMaterializedWorldRoot* ClientRoot =
			WoTMaterializedListenHarnessRoot(Client.GetWorld());
		if (Test.TestNotNull(TEXT("host seamless semantic root exists"), HostRoot)
			&& Test.TestNotNull(
				TEXT("client seamless semantic root exists"), ClientRoot))
		{
			Test.TestEqual(TEXT("host/client semantic roots agree"),
				HostRoot->MapRootHex, ClientRoot->MapRootHex);
		}
		const bool bClientSeamlessValid =
			WoTMaterializedListenHarnessValidateExposedWorld(
				Test, Client.GetWorld(), SeamlessSlot,
				TEXT("remote-client seamless world"));
		if (!bClientSeamlessValid)
		{
			WoTMaterializedListenHarnessLogFailedCell(
				TEXT("client-seamless"), SeamlessSlot);
			return false;
		}
		if (!WoTMaterializedListenHarnessValidateProjectedPool(
				Test, Host.GetWorld(), Client.GetWorld(), TEXT("client-seamless")))
		{
			WoTMaterializedListenHarnessLogFailedCell(
				TEXT("client-seamless"), SeamlessSlot);
			return false;
		}
		if (!WoTMaterializedListenHarnessValidateProjectedDoorActors(
				Test, Host.GetWorld(), Client.GetWorld(), TEXT("client-seamless")))
		{
			WoTMaterializedListenHarnessLogFailedCell(
				TEXT("client-seamless"), SeamlessSlot);
			return false;
		}
		if (!WoTMaterializedListenHarnessValidateProjectedBattleInfo(
				Test, Host.GetWorld(), Client.GetWorld(), TEXT("client-seamless")))
		{
			WoTMaterializedListenHarnessLogFailedCell(
				TEXT("client-seamless"), SeamlessSlot);
			return false;
		}
		if (!WoTMaterializedListenHarnessValidateProjectedMashadarGuide(
				Test, Host.GetWorld(), Client.GetWorld(), TEXT("client-seamless")))
		{
			WoTMaterializedListenHarnessLogFailedCell(
				TEXT("client-seamless"), SeamlessSlot);
			return false;
		}
		if (bRealClientRoute)
		{
			if (!WoTMaterializedListenHarnessExerciseProjectedDoor(
					Test, Host, Client, TEXT("client-seamless"), SeamlessSlot))
			{
				WoTMaterializedListenHarnessLogFailedCell(
					TEXT("client-seamless"), SeamlessSlot);
				return false;
			}
		}
		else
		{
			Test.AddInfo(TEXT(
				"client-seamless projected-door movement replication skipped because this cell used the declared direct-client fallback"));
		}
		WoTMaterializedListenHarnessRecord(
			Coverage, TEXT("client-seamless"), SeamlessSlot,
			bRealClientRoute ? TEXT("passed-real")
				: TEXT("passed-direct-fallback"));
	}

	// A completed hidden candidate is rejected before exposure. The patched
	// seamless route must discard it without touching the current world or
	// publishing another readiness notification.
	{
		FWoTMaterializedListenHarnessEditorWorldRestoration CellWorldRestoration;
		FWoTMaterializedListenHarnessWorldInstance Host(
			Test, /*bClient*/ false);
		const FString PriorPackage(TEXT("/WoTNeutralShells/WoTMapShell03"));
		const FString RejectedPackage(TEXT("/WoTNeutralShells/WoTMapShell04"));
		if (!Host.IsLoaded()
			|| !Host.BrowseLocal(
				Test,
				PriorPackage + TEXT("?listen?game=/Script/Engine.GameMode"),
				TEXT("rollback listen-host route")))
		{
			WoTMaterializedListenHarnessLogFailedCell(
				TEXT("host-seamless-rollback"), 4);
			return false;
		}
		UWorld* PriorWorld = Host.GetWorld();
		AGameModeBase* PriorMode = PriorWorld != nullptr
			? PriorWorld->GetAuthGameMode() : nullptr;
		if (!Test.TestNotNull(TEXT("rollback host game mode exists"), PriorMode))
		{
			WoTMaterializedListenHarnessLogFailedCell(
				TEXT("host-seamless-rollback"), 4);
			return false;
		}
		PriorMode->bUseSeamlessTravel = true;
		int32 ReadinessNotifications = 0;
		FDelegateHandle ReadinessHandle =
			RegisterWoTBootstrapWorldReadinessListener(
				FWoTBootstrapWorldReadinessEvent::FDelegate::CreateLambda(
					[&ReadinessNotifications](bool bReady)
					{
						ReadinessNotifications += bReady ? 1 : 0;
					}));
		FWoTWorldMaterializer::RejectNextCompletedCandidateForAutomationTests();
		TGuardConsoleVariable<bool> AllowPIESeamlessTravel(
			TEXT("net.AllowPIESeamlessTravel"), true);
		// Make this a direct final-destination transition. With UE's empty
		// transition-map default, the original world is torn down in favor of a
		// temporary world before the final candidate loads, so there is no prior
		// gameplay world left for a rejection to retain. The scoped current-map
		// transition preserves exactly the rollback state under test.
		UGameMapsSettings* MapsSettings =
			GetMutableDefault<UGameMapsSettings>();
		TGuardValue<FSoftObjectPath> TransitionMapGuard(
			MapsSettings->TransitionMap,
			FSoftObjectPath(PriorWorld->GetOutermost()->GetName()));
#if !WOT_GUARDED_BOOTSTRAP
		// The editor-only adapter drives the public handler failure branch to
		// cancel because CancelTravel itself is not exported. Require exactly
		// the two Engine errors from that deliberately invalid hidden candidate.
		Test.AddExpectedMessagePlain(
			TEXT("LogWorld: Unable to travel to"), ELogVerbosity::Error,
			EAutomationExpectedMessageFlags::Contains, 1);
		Test.AddExpectedMessagePlain(
			TEXT("LogNet: TravelFailure: NoLevel"), ELogVerbosity::Error,
			EAutomationExpectedMessageFlags::Contains, 1);
#endif
		const bool bRollbackArmed = Host.ArmSeamlessRoute(
			Test, RejectedPackage,
			EWoTWorldStartPath::ListenHostSeamlessTravel);
		const bool bTravelStarted = bRollbackArmed
			&& PriorWorld->ServerTravel(
				RejectedPackage + TEXT("?SeamlessTravel"), false);
		const bool bTransitionEnded = bTravelStarted
			&& WoTMaterializedListenHarnessWaitForTransitionEnd(
				Test, Host, TEXT("host-seamless-rollback"), 4);
		UnregisterWoTBootstrapWorldReadinessListener(ReadinessHandle);
		bool bRollbackPassed =
			Test.TestTrue(TEXT("rollback seamless travel started"), bTravelStarted);
		bRollbackPassed = Host.VerifyRouteRejected(
			Test, TEXT("rejected host-seamless route")) && bRollbackPassed;
		bRollbackPassed = Test.TestTrue(
			TEXT("rejected seamless transition met its bounded deadline"),
			bTransitionEnded) && bRollbackPassed;
		FWorldContext* HostContext = Host.GetWorldContext();
		bRollbackPassed = Test.TestTrue(
			TEXT("rollback host context remains available"),
			HostContext != nullptr) && bRollbackPassed;
		bRollbackPassed = Test.TestTrue(
			TEXT("rejected seamless transition was cancelled"),
			HostContext != nullptr
				&& !HostContext->SeamlessTravelHandler.IsInTransition())
			&& bRollbackPassed;
		bRollbackPassed = Test.TestTrue(
			TEXT("rollback retained the identical prior world"),
			Host.GetWorld() == PriorWorld) && bRollbackPassed;
		bRollbackPassed = Test.TestTrue(
			TEXT("rollback prior world remains in play"),
			PriorWorld->HasBegunPlay()
				&& !PriorWorld->HasAnyFlags(
					RF_BeginDestroyed | RF_FinishDestroyed)) && bRollbackPassed;
		bRollbackPassed = Test.TestEqual(
			TEXT("rejected candidate published no readiness"),
			ReadinessNotifications, 0) && bRollbackPassed;
		const FString RejectedPIEPackage = UWorld::ConvertToPIEPackageName(
			RejectedPackage,
			HostContext != nullptr ? HostContext->PIEInstance : INDEX_NONE);
		bRollbackPassed = Test.TestNull(
			TEXT("rejected candidate package is immediately unfindable"),
			FindPackage(nullptr, *RejectedPIEPackage)) && bRollbackPassed;
		UE_LOG(LogTemp, Display,
			TEXT("WOT listen matrix cell route=host-seamless-rollback shell=04 result=%s"),
			bRollbackPassed ? TEXT("passed-real") : TEXT("failed"));
		if (!bRollbackPassed)
		{
			return false;
		}
	}

	// Run every source shell through the actual UWoTGameEngine override after
	// PIE duplication tests are complete, since normal Game LoadMap consumes the
	// loaded source package rather than a PIE duplicate.
	{
		FWoTMaterializedListenHarnessEngineScope Scope;
		UGameInstance* Instance = NewObject<UGameInstance>(Scope.Engine);
		Instance->InitializeStandalone(FName(TEXT("WoTListenHarnessNormal")));
		// The preceding PIE routes leave their source shell worlds initialized in
		// the editor process.  LoadMap's supported independent-Game-context path
		// requires a valid instance ID so it reloads a fresh package copy instead
		// of exposing that already-initialized source world.
		Scope.Engine->GetWorldContextFromWorldChecked(Instance->GetWorld())
			.PIEInstance = WoTMaterializedListenHarnessUnusedPIEInstance();
		for (int32 Slot = 1; Slot <= 19; ++Slot)
		{
			const FString Package = FString::Printf(
				TEXT("/WoTNeutralShells/WoTMapShell%02d"), Slot);
			if (!WoTMaterializedListenHarnessLoadNormal(
					Test, *Scope.Engine, *Instance, Package))
			{
				WoTMaterializedListenHarnessLogFailedCell(
					TEXT("normal"), Slot);
				return false;
			}
			if (!WoTMaterializedListenHarnessValidateExposedWorld(
					Test, Instance->GetWorld(), Slot,
					TEXT("normal route world")))
			{
				WoTMaterializedListenHarnessLogFailedCell(
					TEXT("normal"), Slot);
				return false;
			}
			WoTMaterializedListenHarnessRecord(
				Coverage, TEXT("normal"), Slot, TEXT("passed-real"));
		}
	}

	Test.TestEqual(TEXT("matrix has exactly 39 unique route/shell cells"),
		Coverage.Num(), 39);
	for (int32 Slot = 1; Slot <= 19; ++Slot)
	{
		Test.TestTrue(TEXT("every shell has normal-route coverage"),
			Coverage.Contains(FString::Printf(TEXT("normal:%02d"), Slot)));
	}
	for (const int32 Slot : RotatingSlots)
	{
		for (const TCHAR* Route : {
				TEXT("host-travel"), TEXT("client-travel"),
				TEXT("host-seamless"), TEXT("client-seamless")})
		{
			Test.TestTrue(TEXT("rotating shell has every network route"),
				Coverage.Contains(FString::Printf(
					TEXT("%s:%02d"), Route, Slot)));
		}
	}
	Test.AddInfo(FString::Printf(
		TEXT("matrix coverage: 19 real normal, 5 real host-travel, 5 real host-seamless, %d real client cells, %d direct-dispatcher client fallback cells, and 1 real host-seamless rollback"),
		10 - DirectClientCoverage.Num(), DirectClientCoverage.Num()));
	return !Test.HasAnyErrors();
}
#endif // WITH_EDITOR
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMaterializedListenHarnessSpec,
	"WoT.World.ListenHarness.InProcessHostJoinAndSeamlessMatrix",
	WoTMaterializedListenHarnessFlags)

bool FWoTMaterializedListenHarnessSpec::RunTest(const FString&)
{
	FWoTMaterializedListenHarnessConfig Config;
	if (!WoTMaterializedListenHarnessParseEnvironment(*this, Config))
	{
		return false;
	}

	if (Config.bPackagedMatrix)
	{
		// The normal-route cells deliberately replace GEngine with an
		// uninitialized route engine. Runtime collision shapes created in that
		// scope emit these known material-query diagnostics; they do not describe
		// route, projection, or replication failures. Require each narrow pattern
		// to occur at least once while keeping every other error actionable.
		AddExpectedError(TEXT("FBodyInstance::GetSimplePhysicalMaterial : GEngine not initialized"),
			EAutomationExpectedErrorFlags::Contains, 0);
		AddExpectedError(TEXT("FBodyInstance::GetComplexPhysicalMaterials : GEngine not initialized"),
			EAutomationExpectedErrorFlags::Contains, 0);
		AddExpectedError(TEXT("FBodyInstance::ApplyMaterialToShape_AssumesLocked : PComplexMats is empty"),
			EAutomationExpectedErrorFlags::Contains, 0);
		AddExpectedError(TEXT("FBodyInstance::ApplyMaterialToShape_AssumesLocked : No valid simple physics material found"),
			EAutomationExpectedErrorFlags::Contains, 0);
		// Capture the actual editor world before the activation fixture creates
		// its standalone world. This guard is destroyed after every fixture and
		// therefore repairs GWorld even when the first failed cell returns early.
#if WITH_EDITOR
		FWoTMaterializedListenHarnessEditorWorldRestoration EditorWorldRestoration;
#endif
		FWoTMaterializedListenHarnessMount Mount(Config.ShellRoot);
		FWoTMaterializedListenHarnessCacheRoot CacheRoot;
		FWoTMaterializedListenHarnessActivation Activation;
#if WITH_EDITOR
		EditorWorldRestoration.Restore();
#endif
		if (WoTMaterializedListenHarnessCheckReceipts(*this, Config)
			&& WoTMaterializedListenHarnessLoadShells(*this, Config.ShellRoot)
			&& WoTMaterializedListenHarnessProvisionAuthority(
				*this, Activation, CacheRoot.Path))
		{
#if WITH_EDITOR
			WoTMaterializedListenHarnessRunRealMatrix(*this);
#else
			AddError(TEXT("real listen matrix requires the editor PIE net-world harness"));
#endif
		}
	}
	else
	{
		AddInfo(TEXT("WOT_WORLD_PACKAGED_MATRIX absent: real route matrix skipped because the sealed nineteen-shell root and receipts were not supplied"));
	}
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
