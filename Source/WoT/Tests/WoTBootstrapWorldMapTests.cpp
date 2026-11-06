#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "GameFramework/WorldSettings.h"
#include "GameFramework/HUD.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectHash.h"
#include "Modules/ModuleManager.h"
#include "../WoTBootstrapGameMode.h"
#include "../WoTFrontEndPlayerController.h"
#include "../WoTGameEngine.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBootstrapWorldMapSpec,
	"WoT.Bootstrap.WorldMap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTBootstrapWorldMapSpec::RunTest(const FString&)
{
	const AWoTBootstrapGameMode* Mode = GetDefault<AWoTBootstrapGameMode>();
	if (TestNotNull(TEXT("bootstrap game mode CDO exists"), Mode))
	{
		TestNull(TEXT("bootstrap mode has no default pawn"),
			Mode->DefaultPawnClass.Get());
		TestNull(TEXT("bootstrap mode has no HUD"), Mode->HUDClass.Get());
		TestTrue(TEXT("bootstrap mode projects through the front-end controller"),
			Mode->PlayerControllerClass.Get()
				== AWoTFrontEndPlayerController::StaticClass());
		const FBoolProperty* SpectatorProperty = FindFProperty<FBoolProperty>(
			Mode->GetClass(), TEXT("bStartPlayersAsSpectators"));
		if (TestNotNull(TEXT("spectator-start property exists"),
			SpectatorProperty))
		{
			TestTrue(TEXT("bootstrap mode starts players as spectators"),
				SpectatorProperty->GetPropertyValue_InContainer(Mode));
		}
	}

	const FString BootstrapPackage = TEXT("/Game/WoT/Maps/WoTBootstrap");
	FString Filename;
	if (TestTrue(TEXT("committed bootstrap package exists"),
		FPackageName::DoesPackageExist(BootstrapPackage, &Filename)))
	{
		const int64 PackageBytes = IFileManager::Get().FileSize(*Filename);
		TestTrue(TEXT("bootstrap package is non-empty"), PackageBytes > 0);
		TestTrue(TEXT("bootstrap package stays within the materializer limit"),
			PackageBytes <= 64ll * 1024ll * 1024ll);

		UPackage* Package = LoadPackage(nullptr, *BootstrapPackage, LOAD_None);
		UWorld* World = Package != nullptr
			? UWorld::FindWorldInPackage(Package) : nullptr;
		if (TestNotNull(TEXT("bootstrap package loads as a world"), World)
			&& TestNotNull(TEXT("bootstrap world has WorldSettings"),
				World->GetWorldSettings()))
		{
			TestTrue(TEXT("world binds the dedicated bootstrap mode"),
				World->GetWorldSettings()->DefaultGameMode.Get()
					== AWoTBootstrapGameMode::StaticClass());
			TestNull(TEXT("bootstrap world is not partitioned"),
				World->GetWorldPartition());
		}

		// Leaving the real package loaded poisons every later spec that
		// creates a transient candidate under the pinned bootstrap name (the
		// materializer authority spec's CreatePackage would receive THIS
		// loaded package, whose non-empty LoadedPath forces the byte-hash
		// branch against a synthetic permit digest). Unload before returning
		// and pin the invariant.
		if (Package != nullptr)
		{
			if (World != nullptr)
			{
				if (World->bIsWorldInitialized)
				{
					World->CleanupWorld();
				}
				World->ClearFlags(RF_Standalone);
			}
			ResetLoaders(Package);
			ForEachObjectWithPackage(Package, [](UObject* Object)
			{
				Object->ClearFlags(RF_Standalone);
				return true;
			});
			CollectGarbage(RF_NoFlags);
			TestNull(TEXT("bootstrap package is unloaded after inspection"),
				FindPackage(nullptr, *BootstrapPackage));
		}

		FAssetRegistryModule& Module =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		Module.Get().ScanFilesSynchronous({Filename}, true);
		TArray<FName> Dependencies;
		TestTrue(TEXT("asset registry reports bootstrap dependencies"),
			Module.Get().GetDependencies(FName(*BootstrapPackage), Dependencies));
		for (FName Dependency : Dependencies)
		{
			TestTrue(*FString::Printf(TEXT("dependency is code-only: %s"),
				*Dependency.ToString()),
				Dependency.ToString().StartsWith(TEXT("/Script/")));
		}
	}

	const FURL Requested(nullptr,
		TEXT("unreal://poison.example/Game/IniMap?listen?game=PoisonMode"),
		TRAVEL_Absolute);
	TestFalse(TEXT("test request carries an untrusted host"),
		Requested.Host.IsEmpty());
	TestTrue(TEXT("test request carries untrusted options"),
		Requested.Op.Num() > 0);
	const TOptional<FString> Initial = WoTGuardedFirstBrowseUrl(true);
	TestTrue(TEXT("first browse replaces the complete untrusted URL"),
		Initial.IsSet());
	if (Initial.IsSet())
	{
		TestEqual(TEXT("first browse selects the pinned bootstrap package"),
			Initial.GetValue(), BootstrapPackage);
		const FURL Replacement(nullptr, *Initial.GetValue(), TRAVEL_Absolute);
		TestTrue(TEXT("replacement browse URL has no host"),
			Replacement.Host.IsEmpty());
		TestEqual(TEXT("replacement browse URL has no options"),
			Replacement.Op.Num(), 0);
	}
	const FString LaterUrl =
		TEXT("unreal://server.example/Game/Arena?listen?game=ServerMode");
	FString EffectiveLaterUrl = LaterUrl;
	const TOptional<FString> LaterReplacement =
		WoTGuardedFirstBrowseUrl(false);
	if (LaterReplacement.IsSet())
	{
		EffectiveLaterUrl = LaterReplacement.GetValue();
	}
	TestFalse(TEXT("second browse passes through with all options untouched"),
		LaterReplacement.IsSet());
	TestEqual(TEXT("later browse remains byte-identical at the policy boundary"),
		EffectiveLaterUrl, LaterUrl);
	const TOptional<FString> DefaultMapFallback =
		WoTGuardedBrowseReplacement(
			false, TEXTVIEW("/Game/WoT/Maps/WoTFrontEnd"));
	TestTrue(TEXT("later engine default-map fallback returns through bootstrap"),
		DefaultMapFallback.IsSet());
	if (DefaultMapFallback.IsSet())
	{
		TestEqual(TEXT("fallback selects the guarded front-end package"),
			DefaultMapFallback.GetValue(), BootstrapPackage);
	}
	// A client Connect to "127.0.0.1:7777" parses with Map = GameDefaultMap
	// (the uncooked WoTFrontEnd) and a host; it must reach the server untouched.
	const FURL ClientConnect(nullptr, TEXT("127.0.0.1:7777"), TRAVEL_Absolute);
	TestFalse(TEXT("a client connect URL carries a host"),
		ClientConnect.Host.IsEmpty());
	TestFalse(TEXT("a client connect to a server is never redirected"),
		WoTGuardedBrowseReplacement(false,
			TEXTVIEW("/Game/WoT/Maps/WoTFrontEnd"), true).IsSet());
	TestFalse(TEXT("later non-default travel remains untouched"),
		WoTGuardedBrowseReplacement(
			false, TEXTVIEW("/Game/WoT/Maps/Battle2_01")).IsSet());
	return !HasAnyErrors();
}
