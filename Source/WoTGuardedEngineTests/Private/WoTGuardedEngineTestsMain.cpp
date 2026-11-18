#include "CoreMinimal.h"

#include "WoTGuardedEngineTests.h"

#include "Async/ParallelFor.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Engine/GameEngine.h"
#include "Engine/GameInstance.h"
#include "GameFramework/GameUserSettings.h"
#include "LaunchEngineLoop.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "UnrealEngine.h"
#include "WoTBootstrapCore/NativeHandles.h"
#include "WoTLaunchEngineHooks.h"
#include "WoTUserDataRoot.h"
#include "WoTEarlyUI.h"
#include "../../WoT/WoTBootstrapSubsystem.h"
#include "../../WoT/WoTBootFailureReceipt.h"
#include "../../WoT/WoTGameEngine.h"
#include "../../WoT/WoTPreferencesSubsystem.h"
#include "../../WoT/WoTSha256.h"
#include "../../WoT/Updater/WoTExecutableLayout.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>

#if PLATFORM_MAC
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#elif PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <KnownFolders.h>
#include <ShlObj.h>
#include <Windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

FString WoTDefaultUserStateRootPathForDiagnostics();

// Launch is a real dependency in this target and owns GEngineLoop. The standard
// IMPLEMENT_APPLICATION macro would define a second GEngineLoop, so spell out
// the remaining Program application boilerplate while retaining exactly one
// process entry and one loop instance.
TCHAR GInternalProjectName[64] = TEXT("WoTGuardedEngineTests");
IMPLEMENT_FOREIGN_ENGINE_DIR()
IMPLEMENT_SIGNING_KEY_REGISTRATION()
IMPLEMENT_ENCRYPTION_KEY_REGISTRATION()
IMPLEMENT_GAME_MODULE(FDefaultGameModuleImpl, WoTGuardedEngineTests)
PER_MODULE_BOILERPLATE

namespace
{
FWoTGuardedCacheDecodeInputs GCacheDecodeInputs;

UWorld* GAuthenticatedWorld = nullptr;
std::atomic<int32> GDispatchCounts[5];

constexpr ANSICHAR WoTEarlyUIFixture[] =
	"Schema=EarlyUI-v1\n"
	"Width=1733\n"
	"Height=977\n"
	"WindowMode=WindowedFullscreen\n"
	"UIScale=1.375\n"
	"Locale=de-DE\n"
	"ReducedMotion=1\n"
	"HighContrast=1\n"
	"ScreenReader=1\n"
	"BootstrapVolume=0.375\n"
	"InputSensitivity=2.25\n"
	"SelectedFaction=Hound\n";
constexpr TCHAR WoTEarlyUIFixtureSha256[] =
	TEXT("fe5697e780a5afd2044e799221b483e4cffdcbd06fd2d8eb69ad25e331136120");
constexpr ANSICHAR WoTGameUserSettingsPoison[] =
	"[/Script/Engine.GameUserSettings]\n"
	"ResolutionSizeX=6111\n"
	"ResolutionSizeY=3777\n"
	"FullscreenMode=2\n"
	"AudioQualityLevel=3\n"
	"Version=5\n"
	"[ScalabilityGroups]\n"
	"sg.ResolutionQuality=13.000000\n"
	"sg.ViewDistanceQuality=0\n"
	"[WoT.GuardedStartupPoison]\n"
	"Sentinel=task32-review1-poison\n";

std::atomic<int32> GGameUserSettingsOpenCount{0};
std::atomic<int32> GGameUserSettingsProbeCount{0};
std::atomic<int32> GFullPreferenceOpenCount{0};
std::atomic<int32> GFullPreferenceProbeCount{0};
bool GGuardedAudioSettingsConsumerCompleted = false;
int32 GGuardedAudioQualityLevel = INDEX_NONE;

class FWoTGuardedOpenObserver final : public IPlatformFile
{
public:
	bool Initialize(IPlatformFile* Inner, const TCHAR*) override
	{
		LowerLevel = Inner;
		return LowerLevel != nullptr;
	}
	IPlatformFile* GetLowerLevel() override { return LowerLevel; }
	void SetLowerLevel(IPlatformFile* Inner) override { LowerLevel = Inner; }
	const TCHAR* GetName() const override { return TEXT("WoTGuardedOpenObserver"); }
	bool FileExists(const TCHAR* Filename) override
	{
		Observe(Filename, false);
		return LowerLevel->FileExists(Filename);
	}
	int64 FileSize(const TCHAR* Filename) override
	{
		Observe(Filename, false);
		return LowerLevel->FileSize(Filename);
	}
	bool DeleteFile(const TCHAR* Filename) override
	{
		return LowerLevel->DeleteFile(Filename);
	}
	bool IsReadOnly(const TCHAR* Filename) override
	{
		return LowerLevel->IsReadOnly(Filename);
	}
	bool MoveFile(const TCHAR* To, const TCHAR* From) override
	{
		return LowerLevel->MoveFile(To, From);
	}
	bool SetReadOnly(const TCHAR* Filename, bool bReadOnly) override
	{
		return LowerLevel->SetReadOnly(Filename, bReadOnly);
	}
	FDateTime GetTimeStamp(const TCHAR* Filename) override
	{
		return LowerLevel->GetTimeStamp(Filename);
	}
	void SetTimeStamp(const TCHAR* Filename, FDateTime DateTime) override
	{
		LowerLevel->SetTimeStamp(Filename, DateTime);
	}
	FDateTime GetAccessTimeStamp(const TCHAR* Filename) override
	{
		return LowerLevel->GetAccessTimeStamp(Filename);
	}
	FString GetFilenameOnDisk(const TCHAR* Filename) override
	{
		return LowerLevel->GetFilenameOnDisk(Filename);
	}
	FFileOpenResult OpenRead(
		const TCHAR* Filename, EOpenReadFlags Flags) override
	{
		Observe(Filename, true);
		return LowerLevel->OpenRead(Filename, Flags);
	}
	IFileHandle* OpenRead(
		const TCHAR* Filename, bool bAllowWrite = false) override
	{
		Observe(Filename, true);
		return LowerLevel->OpenRead(Filename, bAllowWrite);
	}
	FFileOpenResult OpenWrite(
		const TCHAR* Filename, EOpenWriteFlags Flags) override
	{
		return LowerLevel->OpenWrite(Filename, Flags);
	}
	IFileHandle* OpenWrite(const TCHAR* Filename,
		bool bAppend = false, bool bAllowRead = false) override
	{
		return LowerLevel->OpenWrite(Filename, bAppend, bAllowRead);
	}
	bool DirectoryExists(const TCHAR* Directory) override
	{
		return LowerLevel->DirectoryExists(Directory);
	}
	bool CreateDirectory(const TCHAR* Directory) override
	{
		return LowerLevel->CreateDirectory(Directory);
	}
	bool DeleteDirectory(const TCHAR* Directory) override
	{
		return LowerLevel->DeleteDirectory(Directory);
	}
	FFileStatData GetStatData(const TCHAR* FilenameOrDirectory) override
	{
		return LowerLevel->GetStatData(FilenameOrDirectory);
	}
	bool IterateDirectory(
		const TCHAR* Directory, FDirectoryVisitor& Visitor) override
	{
		return LowerLevel->IterateDirectory(Directory, Visitor);
	}
	bool IterateDirectoryStat(
		const TCHAR* Directory, FDirectoryStatVisitor& Visitor) override
	{
		return LowerLevel->IterateDirectoryStat(Directory, Visitor);
	}

private:
	static void Observe(const TCHAR* Filename, bool bOpen)
	{
		const FString Path(Filename);
		if (Path.EndsWith(TEXT("GameUserSettings.ini"),
			ESearchCase::CaseSensitive))
		{
			(bOpen ? GGameUserSettingsOpenCount
				: GGameUserSettingsProbeCount).fetch_add(
					1, std::memory_order_relaxed);
		}
		if (Path.EndsWith(TEXT("WoTUserPreferences"),
			ESearchCase::CaseSensitive))
		{
			(bOpen ? GFullPreferenceOpenCount
				: GFullPreferenceProbeCount).fetch_add(
					1, std::memory_order_relaxed);
		}
	}

	IPlatformFile* LowerLevel = nullptr;
};

FWoTGuardedOpenObserver GOpenObserver;

bool FixedUserPreferencePath(FString& OutPath)
{
	const FString Root = WoTUserDataRoot();
	if (Root.IsEmpty())
	{
		return false;
	}
	OutPath = FPaths::Combine(
		Root, TEXT("Preferences"), TEXT("WoTUserPreferences"));
	return true;
}

class FWoTGuardedStartupFixtures final
{
public:
	bool Stage(FString& Error)
	{
		if (!FixedUserPreferencePath(PreferencePath))
		{
			Error = TEXT("could not resolve the fixed user-preference fixture path");
			return false;
		}
		GameUserSettingsPath = FConfigCacheIni::GetDestIniFilename(
			TEXT("GameUserSettings"), nullptr, *FPaths::GeneratedConfigDir());
		PreferenceDirectory = FPaths::GetPath(PreferencePath);
		GameUserSettingsDirectory = FPaths::GetPath(GameUserSettingsPath);

		IPlatformFile& PlatformFile =
			FPlatformFileManager::Get().GetPlatformFile();
		if (PlatformFile.GetStatData(*PreferencePath).bIsValid
			|| PlatformFile.GetStatData(*GameUserSettingsPath).bIsValid)
		{
			Error = TEXT("startup fixtures refuse to replace existing player files");
			return false;
		}
		bPreferenceDirectoryExisted =
			PlatformFile.DirectoryExists(*PreferenceDirectory);
		bGameUserSettingsDirectoryExisted =
			PlatformFile.DirectoryExists(*GameUserSettingsDirectory);
		if (!IFileManager::Get().MakeDirectory(*PreferenceDirectory, true)
			|| !IFileManager::Get().MakeDirectory(
				*GameUserSettingsDirectory, true))
		{
			Error = TEXT("could not create startup fixture directories");
			Restore();
			return false;
		}

		const int32 PreferenceSize =
			static_cast<int32>(UE_ARRAY_COUNT(WoTEarlyUIFixture) - 1);
		if (WoTSha256::HashBuffer(WoTEarlyUIFixture, PreferenceSize)
			!= WoTEarlyUIFixtureSha256)
		{
			Error = TEXT("the independently pinned EarlyUI fixture digest drifted");
			Restore();
			return false;
		}
		TArray<uint8> PreferenceBytes;
		PreferenceBytes.Append(
			reinterpret_cast<const uint8*>(WoTEarlyUIFixture), PreferenceSize);
		TArray<uint8> PoisonBytes;
		PoisonBytes.Append(
			reinterpret_cast<const uint8*>(WoTGameUserSettingsPoison),
			static_cast<int32>(UE_ARRAY_COUNT(WoTGameUserSettingsPoison) - 1));
		if (!FFileHelper::SaveArrayToFile(PreferenceBytes, *PreferencePath))
		{
			Error = TEXT("could not stage the distinctive EarlyUI fixture");
			Restore();
			return false;
		}
		bPreferenceStaged = true;
		if (!FFileHelper::SaveArrayToFile(
			PoisonBytes, *GameUserSettingsPath))
		{
			Error = TEXT("could not stage the poisoned GameUserSettings fixture");
			Restore();
			return false;
		}
		bGameUserSettingsStaged = true;
		return true;
	}

	bool Restore()
	{
		bool bRestored = true;
		if (bGameUserSettingsStaged)
		{
			bRestored &= IFileManager::Get().Delete(
				*GameUserSettingsPath, false, true, true);
			bGameUserSettingsStaged = false;
		}
		if (bPreferenceStaged)
		{
			bRestored &= IFileManager::Get().Delete(
				*PreferencePath, false, true, true);
			bPreferenceStaged = false;
		}
		if (!bGameUserSettingsDirectoryExisted
			&& !GameUserSettingsDirectory.IsEmpty())
		{
			IFileManager::Get().DeleteDirectory(
				*GameUserSettingsDirectory, false, false);
		}
		if (!bPreferenceDirectoryExisted && !PreferenceDirectory.IsEmpty())
		{
			IFileManager::Get().DeleteDirectory(
				*PreferenceDirectory, false, false);
		}
		return bRestored;
	}

private:
	FString PreferencePath;
	FString PreferenceDirectory;
	FString GameUserSettingsPath;
	FString GameUserSettingsDirectory;
	bool bPreferenceDirectoryExisted = false;
	bool bGameUserSettingsDirectoryExisted = false;
	bool bPreferenceStaged = false;
	bool bGameUserSettingsStaged = false;
};

int32 PathIndex(EWoTWorldStartPath Path)
{
	switch (Path)
	{
	case EWoTWorldStartPath::NormalInitialLoad: return 0;
	case EWoTWorldStartPath::ListenHostTravel: return 1;
	case EWoTWorldStartPath::RemoteClientTravel: return 2;
	case EWoTWorldStartPath::ListenHostSeamlessTravel: return 3;
	case EWoTWorldStartPath::RemoteClientSeamlessTravel: return 4;
	default: return INDEX_NONE;
	}
}

bool MaterializeOnlyAuthenticatedWorld(
	UWorld* World, EWoTWorldStartPath Path, FString* Error)
{
	const int32 Index = PathIndex(Path);
	if (Index != INDEX_NONE)
	{
		GDispatchCounts[Index].fetch_add(1, std::memory_order_relaxed);
	}
	if (World != GAuthenticatedWorld)
	{
		if (Error)
		{
			*Error = TEXT("the callback rejected a world it did not authenticate");
		}
		return false;
	}
	return Index != INDEX_NONE;
}

bool ReplacementMaterializer(UWorld*, EWoTWorldStartPath, FString*)
{
	return true;
}

bool CommitOnlyAuthenticatedWorld(UWorld* World, FString* Error)
{
	if (World != GAuthenticatedWorld)
	{
		if (Error)
		{
			*Error = TEXT("world exposure was not authenticated");
		}
		return false;
	}
	return true;
}

bool FindOption(int32 ArgC, TCHAR* ArgV[], const TCHAR* Name, FString& OutValue)
{
	const FString Flag = FString(TEXT("--")) + Name;
	const FString Prefix = Flag + TEXT("=");
	for (int32 Index = 1; Index < ArgC; ++Index)
	{
		const FString Argument(ArgV[Index]);
		if (Argument == Flag)
		{
			if (Index + 1 < ArgC)
			{
				OutValue = ArgV[Index + 1];
			}
			return true;
		}
		if (Argument.StartsWith(Prefix, ESearchCase::CaseSensitive))
		{
			OutValue = Argument.RightChop(Prefix.Len());
			return true;
		}
	}
	return false;
}

int32 RunSuite(const FString& Filter)
{
	FAutomationTestFramework& Framework = FAutomationTestFramework::Get();
	Framework.SetRequestedTestFilter(
		EAutomationTestFlags::ProgramContext | EAutomationTestFlags::ProductFilter);

	TArray<FAutomationTestInfo> Available;
	Framework.GetValidTestNames(Available);
	int32 Selected = 0;
	int32 Failed = 0;
	for (const FAutomationTestInfo& Info : Available)
	{
		const FString Path = Info.GetFullTestPath();
		if (Path != Filter &&
			!Path.StartsWith(Filter + TEXT("."), ESearchCase::CaseSensitive))
		{
			continue;
		}

		++Selected;
		Framework.StartTestByName(Info.GetTestName(), 0);
		while (!Framework.ExecuteLatentCommands())
		{
			FPlatformProcess::Sleep(0.0f);
		}
		FAutomationTestExecutionInfo ExecutionInfo;
		const bool bPassed = Framework.StopTest(ExecutionInfo);
		Failed += bPassed ? 0 : 1;
		UE_LOG(LogTemp, Display, TEXT("%s %s"),
			bPassed ? TEXT("PASS") : TEXT("FAIL"), *Path);
		FPlatformMisc::LowLevelOutputDebugStringf(TEXT("%s %s\n"),
			bPassed ? TEXT("PASS") : TEXT("FAIL"), *Path);
		std::fprintf(stderr, "%s %s\n", bPassed ? "PASS" : "FAIL",
			TCHAR_TO_UTF8(*Path));
		for (const FAutomationExecutionEntry& Entry : ExecutionInfo.GetEntries())
		{
			if (Entry.Event.Type == EAutomationEventType::Error)
			{
				UE_LOG(LogTemp, Error, TEXT("  %s"), *Entry.Event.Message);
				FPlatformMisc::LowLevelOutputDebugStringf(
					TEXT("  %s\n"), *Entry.Event.Message);
				std::fprintf(stderr, "  %s\n",
					TCHAR_TO_UTF8(*Entry.Event.Message));
			}
		}
	}

	if (Selected == 0)
	{
		UE_LOG(LogTemp, Error, TEXT("suite '%s' selected no tests"), *Filter);
		return 2;
	}
	return Failed == 0 ? 0 : 1;
}
}  // namespace

const FWoTGuardedCacheDecodeInputs& WoTGuardedCacheDecodeInputs()
{
	return GCacheDecodeInputs;
}

#if WITH_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAllWorldStartPathsSpec,
	"WoT.GuardedEngine.AllWorldStartPaths", WoTGuardedEngineSpecFlags)

bool FWoTAllWorldStartPathsSpec::RunTest(const FString&)
{
	for (std::atomic<int32>& Count : GDispatchCounts)
	{
		Count.store(0, std::memory_order_relaxed);
	}

	UWorld* const Authenticated = reinterpret_cast<UWorld*>(UPTRINT(0x1000));
	UWorld* const Unauthenticated = reinterpret_cast<UWorld*>(UPTRINT(0x2000));
	GAuthenticatedWorld = Authenticated;

	FString Error;
	TestFalse(TEXT("null registration is rejected"),
		WoTRegisterGuardedWorldMaterializer(
			nullptr, &CommitOnlyAuthenticatedWorld, &Error));
	TestFalse(TEXT("null exposure committer is rejected"),
		WoTRegisterGuardedWorldMaterializer(
			&MaterializeOnlyAuthenticatedWorld, nullptr, &Error));
	TestFalse(TEXT("dispatch before registration fails closed"),
		WoTGuardedStartup_MaterializeWorld(Authenticated,
			EWoTWorldStartPath::NormalInitialLoad, &Error));

	std::atomic<int32> InitialRegistrationAccepts{0};
	ParallelFor(8, [&InitialRegistrationAccepts](int32)
	{
		FString ConcurrentError;
		if (WoTRegisterGuardedWorldMaterializer(
			&MaterializeOnlyAuthenticatedWorld,
			&CommitOnlyAuthenticatedWorld, &ConcurrentError))
		{
			InitialRegistrationAccepts.fetch_add(1, std::memory_order_relaxed);
		}
	});
	TestEqual(TEXT("concurrent non-null registration publishes exactly once"),
		InitialRegistrationAccepts.load(std::memory_order_relaxed), 1);
	TestFalse(TEXT("a second callback cannot replace it"),
		WoTRegisterGuardedWorldMaterializer(&ReplacementMaterializer,
			&CommitOnlyAuthenticatedWorld, &Error));

	const EWoTWorldStartPath Paths[] = {
		EWoTWorldStartPath::NormalInitialLoad,
		EWoTWorldStartPath::ListenHostTravel,
		EWoTWorldStartPath::RemoteClientTravel,
		EWoTWorldStartPath::ListenHostSeamlessTravel,
		EWoTWorldStartPath::RemoteClientSeamlessTravel,
	};
	for (EWoTWorldStartPath Path : Paths)
	{
		TestTrue(TEXT("the registered callback approves the authenticated world"),
			WoTGuardedStartup_MaterializeWorld(Authenticated, Path, &Error));
		TestTrue(TEXT("the registered exposure callback commits the world"),
			WoTGuardedStartup_CommitWorldExposure(Authenticated, &Error));
	}
	for (int32 Index = 0; Index < 5; ++Index)
	{
		TestEqual(TEXT("each start path dispatches exactly once"),
			GDispatchCounts[Index].load(std::memory_order_relaxed), 1);
	}

	TestFalse(TEXT("the callback rejects a different world"),
		WoTGuardedStartup_MaterializeWorld(Unauthenticated,
			EWoTWorldStartPath::NormalInitialLoad, &Error));
	TestFalse(TEXT("the dispatcher rejects a null world"),
		WoTGuardedStartup_MaterializeWorld(nullptr,
			EWoTWorldStartPath::NormalInitialLoad, &Error));
	TestFalse(TEXT("registration after dispatch is late"),
		WoTRegisterGuardedWorldMaterializer(&ReplacementMaterializer,
			&CommitOnlyAuthenticatedWorld, &Error));

	std::atomic<int32> ConcurrentAccepts{0};
	ParallelFor(8, [&ConcurrentAccepts](int32)
	{
		FString ConcurrentError;
		if (WoTRegisterGuardedWorldMaterializer(
			&ReplacementMaterializer,
			&CommitOnlyAuthenticatedWorld, &ConcurrentError))
		{
			ConcurrentAccepts.fetch_add(1, std::memory_order_relaxed);
		}
	});
	TestEqual(TEXT("concurrent replacement attempts are all rejected"),
		ConcurrentAccepts.load(std::memory_order_relaxed), 0);

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUserDataRootSpec,
	"WoT.GuardedEngine.UserDataRoot", WoTGuardedEngineSpecFlags)

bool FWoTUserDataRootSpec::RunTest(const FString&)
{
	TestEqual(TEXT("the product data-root leaf is stable"),
		FString(WoTUserDataRootLeafName()), FString(TEXT("WoT Reborn")));

	const FString OriginalRoot = WoTUserDataRoot();
	const FString OverrideSupport = FPaths::Combine(
		FPlatformProcess::UserTempDir(), TEXT("Resolver Home With Space"),
		TEXT("Library"), TEXT("Application Support"));
	TestTrue(TEXT("the resolver seam base can be prepared"),
		IFileManager::Get().MakeDirectory(*OverrideSupport, true));
	const FString Override = FPaths::Combine(
		OverrideSupport, TEXT("WoT Reborn"));
	WoTSetUserDataRootForTesting(Override + TEXT("/"));
	TestEqual(TEXT("the resolver seam preserves an absolute root with a space"),
		WoTUserDataRoot(), Override);
	TestEqual(TEXT("the guarded worker consumes the engine resolver verbatim"),
		WoTDefaultUserStateRootPathForDiagnostics(), Override);
	WoTSetUserDataRootForTesting(OriginalRoot);

#if PLATFORM_MAC
	const FString Root = WoTUserDataRoot();
	TestFalse(TEXT("the Mac resolver produces a non-empty root"), Root.IsEmpty());
	TestFalse(TEXT("the Mac resolver produces an absolute root"),
		FPaths::IsRelative(Root));
	TestFalse(TEXT("the Mac resolver has no trailing separator"),
		Root.EndsWith(TEXT("/")) || Root.EndsWith(TEXT("\\")));
	for (const TCHAR* Relative : {TEXT(""), TEXT("ContentCache"),
		TEXT("UpdaterState"), TEXT("Preferences"), TEXT("Saved"),
		TEXT("Engine")})
	{
		const FString Path = Relative[0] == 0
			? Root : FPaths::Combine(Root, Relative);
		const FTCHARToUTF8 Utf8(*Path);
		struct stat Identity = {};
		TestTrue(*FString::Printf(TEXT("%s is an owned directory"), *Path),
			lstat(Utf8.Get(), &Identity) == 0 && S_ISDIR(Identity.st_mode)
			&& Identity.st_uid == getuid());
		TestEqual(*FString::Printf(TEXT("%s is mode 0700"), *Path),
			static_cast<uint32>(Identity.st_mode & 0777), 0700u);
	}

	const FString ProjectSaved = FPaths::ProjectSavedDir();
	const FString EngineUser = FPaths::EngineUserDir();
	const FString ProjectLog = FPaths::ProjectLogDir();
	TestEqual(TEXT("ProjectSavedDir is rooted under the product data root"),
		ProjectSaved, FPaths::Combine(Root, TEXT("Saved")) + TEXT("/"));
	TestEqual(TEXT("EngineUserDir is rooted under the product data root"),
		EngineUser, FPaths::Combine(Root, TEXT("Engine")) + TEXT("/"));
	TestEqual(TEXT("ProjectLogDir is rooted under the product data root"),
		ProjectLog, FPaths::Combine(Root, TEXT("Saved"), TEXT("Logs")) + TEXT("/"));
	for (const FString* Path : {&Root, &ProjectSaved, &EngineUser, &ProjectLog})
	{
		TestFalse(TEXT("no resolved Mac path contains an Epic component"),
			Path->Contains(TEXT("/Epic/"), ESearchCase::CaseSensitive));
	}
#elif PLATFORM_WINDOWS
	const FString Root = WoTUserDataRoot();
	TestFalse(TEXT("the Windows resolver produces a non-empty guarded root"),
		Root.IsEmpty());
	TestEqual(TEXT("ProjectSavedDir is rooted under install-local Data"),
		FPaths::ProjectSavedDir(), FPaths::Combine(Root, TEXT("Saved")) + TEXT("/"));
	TestEqual(TEXT("EngineUserDir is rooted under install-local Data"),
		FPaths::EngineUserDir(), FPaths::Combine(Root, TEXT("Engine")) + TEXT("/"));
	const FString HiddenFallback = FPaths::Combine(
		FPlatformProcess::UserSettingsDir(), TEXT("WoT Reborn"));
	TestNotEqual(TEXT("the Windows guarded root is not a hidden LocalAppData fallback"),
		Root, HiddenFallback);

	const FString ResolverRoot = FPaths::Combine(
		FPlatformProcess::UserTempDir(), TEXT("R1 Windows Resolver With Space"),
		FString::Printf(TEXT("Run-%u"), FPlatformProcess::GetCurrentProcessId()));
	const FString LiveImage = FPaths::Combine(ResolverRoot, TEXT("WoT Reborn"),
		TEXT("Game"), TEXT("WoT"), TEXT("Binaries"), TEXT("Win64"),
		TEXT("WoTGuardedEngineTests.exe"));
	const FString StagedImage = FPaths::Combine(ResolverRoot, TEXT("WoT Reborn"),
		TEXT(".wot-update"), TEXT("staged-1"), TEXT("Game"), TEXT("WoT"),
		TEXT("Binaries"), TEXT("Win64"), TEXT("WoTGuardedEngineTests.exe"));
	const FString OriginalImage(FPlatformProcess::ExecutablePath());
	const FString WorkingDirectory = FPaths::GetPath(OriginalImage);
	const auto ResolveCopiedImage = [this, &OriginalImage, &WorkingDirectory](
		const FString& Image, const FString& Label)
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Image), true);
		if (!TestEqual(*FString::Printf(TEXT("%s image copy succeeds"), *Label),
			IFileManager::Get().Copy(*Image, *OriginalImage), COPY_OK))
		{
			return FString();
		}
		for (const TCHAR* RuntimeDll : {TEXT("tbb12.dll"), TEXT("tbbmalloc.dll")})
		{
			const FString SourceDll = FPaths::Combine(WorkingDirectory, RuntimeDll);
			const FString TargetDll = FPaths::Combine(
				FPaths::GetPath(Image), RuntimeDll);
			if (!TestEqual(*FString::Printf(
					TEXT("%s copies %s"), *Label, RuntimeDll),
				IFileManager::Get().Copy(*TargetDll, *SourceDll), COPY_OK))
			{
				return FString();
			}
		}
		const FString Output = Image + TEXT(".root.txt");
		const FString Arguments = FString::Printf(
			TEXT("--write-user-data-root=\"%s\""), *Output);
		FProcHandle Child = FPlatformProcess::CreateProc(*Image, *Arguments,
			false, true, true, nullptr, 0, *WorkingDirectory, nullptr);
		if (!TestTrue(*FString::Printf(TEXT("%s child starts"), *Label),
			Child.IsValid()))
		{
			return FString();
		}
		FPlatformProcess::WaitForProc(Child);
		int32 ReturnCode = -1;
		FPlatformProcess::GetProcReturnCode(Child, &ReturnCode);
		FPlatformProcess::CloseProc(Child);
		TestEqual(*FString::Printf(TEXT("%s child succeeds"), *Label),
			ReturnCode, 0);
		FString Resolved;
		TestTrue(*FString::Printf(TEXT("%s child writes its resolver result"),
			*Label), FFileHelper::LoadFileToString(Resolved, *Output));
		return Resolved;
	};
	const FString LiveRoot = ResolveCopiedImage(LiveImage, TEXT("live-layout"));
	TestEqual(TEXT("a canonical live image resolves install-local Data"), LiveRoot,
		FPaths::Combine(ResolverRoot, TEXT("WoT Reborn"), TEXT("Data")));
	TestTrue(TEXT("an image under .wot-update fails closed"),
		ResolveCopiedImage(StagedImage, TEXT("staged-layout")).IsEmpty());
	IFileManager::Get().DeleteDirectory(*ResolverRoot, false, true);
#endif
	return !HasAnyErrors();
}

// A fresh install's data root has no Preferences folder: only a save creates
// it, and nothing saves before cache activation. The guarded read used to open
// the folder without creating it, so the post-activation preference load failed
// with "file not found" and the first launch stopped at
// PAYLOAD-CAPABILITY-ABSENT. A missing
// folder must read as "nothing saved yet"; a non-directory there must still fail.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFreshDataRootPreferencesSpec,
	"WoT.GuardedEngine.FreshDataRootPreferences", WoTGuardedEngineSpecFlags)

bool FWoTFreshDataRootPreferencesSpec::RunTest(const FString&)
{
	const FString OriginalRoot = WoTUserDataRoot();
	const FString Base = FPaths::Combine(FPlatformProcess::UserTempDir(),
		FString::Printf(TEXT("WoT Fresh Data Root %u"),
			FPlatformProcess::GetCurrentProcessId()));
	// The product leaf: the Mac resolver accepts an override only when it is
	// <parent>/WoTUserDataRootLeafName(), and otherwise resolves to "".
	const FString Root = FPaths::Combine(Base, FString(WoTUserDataRootLeafName()));
	IFileManager::Get().DeleteDirectory(*Base, false, true);
	if (!TestTrue(TEXT("an empty data root can be prepared"),
		IFileManager::Get().MakeDirectory(*Root, true)))
	{
		return false;
	}
	WoTSetUserDataRootForTesting(Root);
	TestEqual(TEXT("the guarded data root resolves to the fixture"),
		WoTUserDataRoot(), Root);
	const FString Folder = FPaths::Combine(Root, TEXT("Preferences"));
	// Not the WoTUserPreferences leaf: the startup open observer counts that
	// name, and the read decision depends only on the Preferences folder.
	const FString Child = FPaths::Combine(Folder, TEXT("FreshRootProbe"));

	TArray<uint8> Bytes;
	bool bExists = true;
	FWoTBootstrapStatus Read =
		UWoTPreferencesSubsystem::ReadPreferenceChildForTesting(Child, Bytes, bExists);
#if PLATFORM_MAC
	// The Mac resolver creates Preferences (and its siblings) on every
	// successful resolve, so a resolved Mac root never lacks it: the
	// fresh-install failure was Windows-only. Reads still take the guarded path.
	TestTrue(TEXT("resolving a Mac data root creates Preferences"),
		IFileManager::Get().DirectoryExists(*Folder));
	if (!TestFalse(TEXT("a missing child of the Mac Preferences folder is not a read error"),
		Read.IsError()))
	{
		AddError(Read.GetError().ToString());
	}
	TestFalse(TEXT("a missing child reads as no saved preferences"), bExists);
#else
	if (!TestFalse(TEXT("a missing Preferences folder is not a read error"),
		Read.IsError()))
	{
		AddError(Read.GetError().ToString());
	}
	TestFalse(TEXT("a missing Preferences folder reads as no saved preferences"),
		bExists);
	TestTrue(TEXT("nothing was read"), Bytes.IsEmpty());
	TestFalse(TEXT("a read does not create the Preferences folder"),
		IFileManager::Get().DirectoryExists(*Folder));
	FWoTBootstrapStatus Removed =
		UWoTPreferencesSubsystem::RemovePreferenceChildForTesting(Child);
	if (!TestFalse(TEXT("removing from a missing Preferences folder succeeds"),
		Removed.IsError()))
	{
		AddError(Removed.GetError().ToString());
	}
#endif

	// Positive control: an existing folder and file still read back.
	IFileManager::Get().MakeDirectory(*Folder, true);
	const FString Saved(TEXT("faction=Hound\n"));
	TestTrue(TEXT("a preference file can be staged"),
		FFileHelper::SaveStringToFile(Saved, *Child));
	bExists = false;
	Bytes.Reset();
	Read = UWoTPreferencesSubsystem::ReadPreferenceChildForTesting(Child, Bytes, bExists);
	TestFalse(TEXT("an existing preference file reads without error"), Read.IsError());
	TestTrue(TEXT("an existing preference file is reported"), bExists);
	TestEqual(TEXT("the staged bytes read back"), Bytes.Num(), Saved.Len());

	// Negative control: a FILE named Preferences is unsafe, never "absent".
	IFileManager::Get().DeleteDirectory(*Folder, false, true);
	TestTrue(TEXT("a file can be staged where the folder belongs"),
		FFileHelper::SaveStringToFile(FString(TEXT("x")), *Folder));
#if PLATFORM_MAC
	// On the Mac the resolver itself refuses such a root (fails closed to "").
	TestTrue(TEXT("a non-directory Preferences entry makes the Mac root fail closed"),
		WoTUserDataRoot().IsEmpty());
#else
	Read = UWoTPreferencesSubsystem::ReadPreferenceChildForTesting(Child, Bytes, bExists);
	TestTrue(TEXT("a non-directory Preferences entry still fails the read"),
		Read.IsError());
#endif

	WoTSetUserDataRootForTesting(OriginalRoot);
	IFileManager::Get().DeleteDirectory(*Base, false, true);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTEarlyUIProductionStartupSpec,
	"WoT.Launch.EarlyUI.ProductionStartup", WoTGuardedEngineSpecFlags)

bool FWoTEarlyUIProductionStartupSpec::RunTest(const FString&)
{
	// PreInitPreStartupScreen traversed the patched production call site before
	// suite dispatch. Taking here therefore measures the actual retained value,
	// not a receipt manufactured by this spec.
	TWoTBootstrapResult<FWoTAuthenticatedEarlyUI> Taken =
		GetEarlyUIAuthority().TakeOnce();
	if (!TestFalse(TEXT("production startup published and projected EarlyUI"),
		Taken.IsError()))
	{
		if (Taken.IsError())
		{
			AddError(Taken.GetError().ToString());
		}
		return false;
	}
	const FWoTEarlyUIV1& Value = Taken.GetValue().Get();
	TestEqual(TEXT("review fixture has the independently pinned SHA-256"),
		WoTSha256::HashBuffer(WoTEarlyUIFixture,
			static_cast<uint64>(UE_ARRAY_COUNT(WoTEarlyUIFixture) - 1)),
		FString(WoTEarlyUIFixtureSha256));
	TestEqual(TEXT("production parsed independently expected width"),
		static_cast<int32>(Value.Width), 1733);
	TestEqual(TEXT("production parsed independently expected height"),
		static_cast<int32>(Value.Height), 977);
	TestEqual(TEXT("production parsed independently expected window mode"),
		static_cast<int32>(Value.WindowMode),
		static_cast<int32>(EWoTWindowMode::WindowedFullscreen));
	TestEqual(TEXT("production parsed independently expected UI scale"),
		Value.UIScale, 1.375f);
	TestEqual(TEXT("production parsed independently expected locale"),
		static_cast<int32>(Value.Locale),
		static_cast<int32>(EWoTShippedLocale::DeDE));
	TestTrue(TEXT("production parsed reduced-motion fixture"),
		Value.bReducedMotion);
	TestTrue(TEXT("production parsed high-contrast fixture"),
		Value.bHighContrast);
	TestTrue(TEXT("production parsed screen-reader fixture"),
		Value.bScreenReader);
	TestEqual(TEXT("production parsed independently expected volume"),
		Value.BootstrapVolume, 0.375f);
	TestEqual(TEXT("production parsed independently expected input sensitivity"),
		Value.InputSensitivity, 2.25f);
	TestEqual(TEXT("real system resolution width came from EarlyUI"),
		GSystemResolution.ResX, 1733);
	TestEqual(TEXT("real system resolution height came from EarlyUI"),
		GSystemResolution.ResY, 977);
	TestEqual(TEXT("real system window mode came from EarlyUI"),
		static_cast<int32>(GSystemResolution.WindowMode),
		static_cast<int32>(EWindowMode::WindowedFullscreen));
	TestTrue(TEXT("guarded startup exercised the audio settings consumer chain"),
		GGuardedAudioSettingsConsumerCompleted);
	if (const UGameUserSettings* Settings = GEngine != nullptr
		? GEngine->GetGameUserSettings() : nullptr;
		TestNotNull(TEXT("audio consumer constructed GameUserSettings"), Settings))
	{
		TestEqual(TEXT("audio consumer observed the default quality level"),
			GGuardedAudioQualityLevel, 0);
		TestEqual(TEXT("constructed settings kept the default resolution"),
			Settings->GetScreenResolution(),
			UGameUserSettings::GetDefaultResolution());
		TestEqual(TEXT("constructed settings kept the default window mode"),
			static_cast<int32>(Settings->GetFullscreenMode()),
			static_cast<int32>(UGameUserSettings::GetDefaultWindowMode()));
		TestNotEqual(TEXT("constructed settings rejected poisoned audio quality"),
			Settings->GetAudioQualityLevel(), 3);
		TestNotEqual(TEXT("constructed settings rejected poisoned resolution"),
			Settings->GetScreenResolution(), FIntPoint(6111, 3777));
	}
	TestEqual(TEXT("guarded startup made zero GameUserSettings opens"),
		GGameUserSettingsOpenCount.load(std::memory_order_relaxed), 0);
	TestEqual(TEXT("guarded startup made zero GameUserSettings probes"),
		GGameUserSettingsProbeCount.load(std::memory_order_relaxed), 0);
	TestEqual(TEXT("pre-activation made zero full-preference opens"),
		GFullPreferenceOpenCount.load(std::memory_order_relaxed), 0);
	TestEqual(TEXT("pre-activation made zero full-preference probes"),
		GFullPreferenceProbeCount.load(std::memory_order_relaxed), 0);
	if (TestNotNull(TEXT("global config initialized"), GConfig))
	{
		FString Poison;
		TestFalse(TEXT("poisoned GameUserSettings sentinel was not consumed"),
			GConfig->GetString(TEXT("WoT.GuardedStartupPoison"),
				TEXT("Sentinel"), Poison, GGameUserSettingsIni));
	}
	TestTrue(TEXT("production take is one-shot"),
		GetEarlyUIAuthority().TakeOnce().IsError());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTGuardedFirstBrowseUrlSpec,
	"WoT.GuardedEngine.FirstBrowseUrl", WoTGuardedEngineSpecFlags)

bool FWoTGuardedFirstBrowseUrlSpec::RunTest(const FString&)
{
	const FString Bootstrap = TEXT("/Game/WoT/Maps/WoTBootstrap");
	const FURL Requested(nullptr,
		TEXT("unreal://poison.example/Game/IniMap?listen?game=PoisonMode"),
		TRAVEL_Absolute);
	TestFalse(TEXT("test request has a hostile host"), Requested.Host.IsEmpty());
	TestTrue(TEXT("test request has hostile options"), Requested.Op.Num() > 0);
	const TOptional<FString> Rewritten = WoTGuardedFirstBrowseUrl(true);
	TestTrue(TEXT("first browse replaces a hostile complete URL regardless of options"),
		Rewritten.IsSet());
	if (Rewritten.IsSet())
	{
		TestEqual(TEXT("replacement uses the pinned bootstrap package"),
			Rewritten.GetValue(), Bootstrap);
		const FURL Replacement(nullptr, *Rewritten.GetValue(), TRAVEL_Absolute);
		TestTrue(TEXT("replacement has no host"), Replacement.Host.IsEmpty());
		TestEqual(TEXT("replacement has no options"), Replacement.Op.Num(), 0);
	}
	TestFalse(TEXT("second browse passes through unchanged"),
		WoTGuardedFirstBrowseUrl(false).IsSet());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTGuardedReplayCommandLineSpec,
	"WoT.GuardedEngine.ReplayCommandLine", WoTGuardedEngineSpecFlags)

bool FWoTGuardedReplayCommandLineSpec::RunTest(const FString&)
{
	struct FCell
	{
		const TCHAR* Label;
		const TCHAR* Input;
		const TCHAR* Expected;
	};
	const FCell Cells[] = {
		{TEXT("replay unquoted"),
			TEXT("-foo=1 -REPLAY=Demo01 -bar=2"),
			TEXT("-foo=1  -bar=2")},
		{TEXT("replay quoted"),
			TEXT("-foo=1 -REPLAY=\"Demo One\" -bar=2"),
			TEXT("-foo=1  -bar=2")},
		{TEXT("playlist unquoted"),
			TEXT("-REPLAYPLAYLIST=List01 -bar=2"),
			TEXT(" -bar=2")},
		{TEXT("playlist whole-token quoted"),
			TEXT("-foo=1 \"-replayplaylist=List One\" -bar=2"),
			TEXT("-foo=1  -bar=2")},
		{TEXT("both replay routes"),
			TEXT("-REPLAY=One -keep -REPLAYPLAYLIST=\"Two Three\""),
			TEXT(" -keep ")},
		{TEXT("unrelated bytes remain exact"),
			TEXT("  -keep=\"a b\" -REPLAYSTREAMER=live x-REPLAY=demo  "),
			TEXT("  -keep=\"a b\" -REPLAYSTREAMER=live x-REPLAY=demo  ")},
	};
	for (const FCell& Cell : Cells)
	{
		TestEqual(Cell.Label,
			WoTGuardedSanitizeReplayCommandLine(Cell.Input),
			FString(Cell.Expected));
	}
	return !HasAnyErrors();
}

#if !WITH_AUTOMATION_WORKER
// Shipping defaults WITH_AUTOMATION_WORKER to 0, and IMPLEMENT_SIMPLE_AUTOMATION_TEST
// then declares the class WITHOUT the static instance that registers it — so the
// suite would compile, find nothing, and RunSuite would report "selected no tests".
// FAutomationTestBase registers itself in its constructor, so one explicit
// instance is all that is missing. Preferred over defining
// WITH_AUTOMATION_WORKER=1, which would change public engine headers this
// harness has no need to touch.
namespace
{
FWoTAllWorldStartPathsSpec WoTAllWorldStartPathsSpecInstance(
	TEXT("FWoTAllWorldStartPathsSpec"));
FWoTUserDataRootSpec WoTUserDataRootSpecInstance(
	TEXT("FWoTUserDataRootSpec"));
FWoTFreshDataRootPreferencesSpec WoTFreshDataRootPreferencesSpecInstance(
	TEXT("FWoTFreshDataRootPreferencesSpec"));
FWoTEarlyUIProductionStartupSpec WoTEarlyUIProductionStartupSpecInstance(
	TEXT("FWoTEarlyUIProductionStartupSpec"));
FWoTGuardedFirstBrowseUrlSpec WoTGuardedFirstBrowseUrlSpecInstance(
	TEXT("FWoTGuardedFirstBrowseUrlSpec"));
FWoTGuardedReplayCommandLineSpec WoTGuardedReplayCommandLineSpecInstance(
	TEXT("FWoTGuardedReplayCommandLineSpec"));
}
#endif

#endif  // WITH_AUTOMATION_TESTS

INT32_MAIN_INT32_ARGC_TCHAR_ARGV()
{
	FTaskTagScope Scope(ETaskTag::EGameThread);

#if PLATFORM_WINDOWS && WITH_AUTOMATION_TESTS
	FString ResolverOutput;
	if (FindOption(ArgC, ArgV, TEXT("write-user-data-root"), ResolverOutput))
	{
		const bool bWritten = FFileHelper::SaveStringToFile(
			WoTUserDataRoot(), *ResolverOutput);
		::ExitProcess(bWritten ? 0u : 12u);
		return 12;
	}
#endif

	FString CmdLine =
		FCommandLine::BuildFromArgV(nullptr, ArgC, ArgV, nullptr);
	// This contentless Program intentionally exercises only the first half of
	// PreInit. Keep it headless so cooked shader-library diagnostics cannot open
	// a modal before the guarded startup assertions run.
	CmdLine += TEXT(" -unattended -nullrhi -nosplash");
	FCommandLine::Set(*CmdLine);

#if (PLATFORM_MAC || PLATFORM_WINDOWS) && WITH_AUTOMATION_TESTS
	// Establish an allowed, home-shaped resolver root before any patched FPaths
	// accessor or startup fixture can resolve the player's real profile.
	const FString GuardedHome = FPaths::Combine(
		FPlatformProcess::UserTempDir(), TEXT("R1a Guarded Engine Tests"),
		FString::Printf(TEXT("Home %u"),
			FPlatformProcess::GetCurrentProcessId()));
	const FString GuardedApplicationSupport = FPaths::Combine(
		GuardedHome, TEXT("Library"), TEXT("Application Support"));
	if (!IFileManager::Get().MakeDirectory(*GuardedApplicationSupport, true))
	{
		FPlatformMisc::LowLevelOutputDebugString(
			TEXT("Guarded resolver sandbox parent could not be created\n"));
		return 8;
	}
	const FString GuardedRoot = FPaths::Combine(
#if PLATFORM_MAC
		GuardedApplicationSupport, TEXT("WoT Reborn"));
#else
		GuardedHome, TEXT("WoT Reborn"), TEXT("Data"));
#endif
	WoTSetUserDataRootForTesting(GuardedRoot);
	if (WoTUserDataRoot() != GuardedRoot)
	{
		FPlatformMisc::LowLevelOutputDebugString(
			TEXT("Guarded resolver sandbox could not be prepared\n"));
		return 9;
	}
#endif

#if !UE_BUILD_SHIPPING
	FString QuietFatalRoot;
	if (FindOption(ArgC, ArgV, TEXT("quiet-guarded-fatal-root"), QuietFatalRoot)
		&& !QuietFatalRoot.IsEmpty())
	{
		WoTWriteBootFailureReceiptAndExitForTesting(
			QuietFatalRoot,
			EWoTBootstrapDiagnosticCode::GuardedSessionAbsent,
			EWoTBootstrapPhase::EstablishingIdentity);
	}
#endif

	FWoTGuardedStartupFixtures Fixtures;
	FString FixtureError;
	if (!Fixtures.Stage(FixtureError))
	{
		FPlatformMisc::LowLevelOutputDebugStringf(
			TEXT("Guarded startup fixture staging failed: %s\n"), *FixtureError);
		return 3;
	}
	IPlatformFile* ObservedLower =
		&FPlatformFileManager::Get().GetPlatformFile();
	if (!GOpenObserver.Initialize(ObservedLower, *CmdLine))
	{
		Fixtures.Restore();
		return 4;
	}
	FPlatformFileManager::Get().SetPlatformFile(GOpenObserver);

	// Deliberately run the FIRST HALF of PreInit, then directly exercise the
	// audio startup's lazy UEngine::GetGameUserSettings consumer below.
	// FEngineLoop::PreInit is
	// literally PreInitPreStartupScreen followed by PreInitPostStartupScreen,
	// and everything this harness verifies lives in the first half: guarded
	// trusted-path init, config attestation, and the earliest possible module
	// load all happen there, ahead of the platform-file override.
	//
	// The second half brings up the UObject/default-material work and flushes
	// async loading. This is a cooked-data Program (WITH_EDITORONLY_DATA=0)
	// with no cooked payload, so that flush finds the engine's UNCOOKED loose
	// .uasset files — /Engine/EngineMaterials/WorldGridMaterial and friends —
	// and their package summaries overrun the fixed 8192-byte initial read
	// buffer, tripping "ReaderPos + Num <= ReaderSize" in FBufferReader
	// (FAsyncArchive::ReadCallback -> FPackageFileSummary -> FEngineVersion).
	// No switch or target flag makes a cooked-data Engine Program complete a
	// full PreInit without a payload; Epic's own contentless Programs exclude
	// Engine entirely. Keeping Engine linked is the point here — it is what
	// compiles and links the patched Engine/CoreUObject/Projects modules and
	// proves exactly one entry symbol — so stop at that boundary, then extend
	// coverage through the exact consumer chain that UEngine::Init audio startup
	// uses. This catches any attempt to turn defaults-only object construction
	// back into a GameUserSettings.ini load without requiring uncooked Engine
	// content to masquerade as a packaged payload.
	const int32 PreInitResult = GEngineLoop.PreInitPreStartupScreen(*CmdLine);
	int32 SettingsConsumerResult = 0;
	if (PreInitResult == 0)
	{
		GEngine = NewObject<UGameEngine>();
		if (GEngine == nullptr)
		{
			SettingsConsumerResult = 6;
		}
		else
		{
			GEngine->GameUserSettingsClass = UGameUserSettings::StaticClass();
			// FAudioDevice::GetQualityLevelSettings performs this same lazy query.
			// A guarded LoadConfigIni/LoadSettings regression terminates here before
			// the observer or poisoned fixture is removed.
			const UGameUserSettings* Settings = GEngine->GetGameUserSettings();
			if (Settings == nullptr)
			{
				SettingsConsumerResult = 7;
			}
			else
			{
				GGuardedAudioQualityLevel = Settings->GetAudioQualityLevel();
				GGuardedAudioSettingsConsumerCompleted = true;
			}
		}
	}
	FPlatformFileManager::Get().SetPlatformFile(*ObservedLower);
	const bool bFixturesRestored = Fixtures.Restore();
	if (PreInitResult != 0)
	{
		return PreInitResult;
	}
	if (SettingsConsumerResult != 0)
	{
		return SettingsConsumerResult;
	}
	if (!bFixturesRestored)
	{
		return 5;
	}

	FindOption(ArgC, ArgV, TEXT("cache-root"), GCacheDecodeInputs.CacheRoot);
	FindOption(ArgC, ArgV, TEXT("decode-receipt"),
		GCacheDecodeInputs.DecodeReceipt);

	FString Suite;
	int32 ExitCode = 2;
	if (FindOption(ArgC, ArgV, TEXT("suite"), Suite) && !Suite.IsEmpty())
	{
		ExitCode = RunSuite(Suite);
	}
	else
	{
		UE_LOG(LogTemp, Error,
			TEXT("usage: WoTGuardedEngineTests --suite WoT.GuardedEngine.AllWorldStartPaths"
				" [--cache-root <dir> --decode-receipt <json>]"));
	}

	// Teardown is explicit rather than scope-guarded so that it is ordered
	// against the exit below and so a failure inside it is still visible.
	GEngine = nullptr;
	RequestEngineExit(TEXT("WoT guarded-engine suite complete"));
	GEngineLoop.CleanupPreInitContext();
	FEngineLoop::AppPreExit();
	FModuleManager::Get().UnloadModulesAtShutdown();
	FEngineLoop::AppExit();

	// Everything above is complete and the engine logs "Exiting." by this point.
	// Returning normally from main then runs global destructors belonging to
	// subsystems that only the SECOND half of PreInit fully initializes, and one
	// of them segfaults — after all work is done, with no crash frames recorded,
	// so it reports a false process failure for a suite that passed. Skip static
	// destruction and exit with the suite's own status.
	//
	// std::_Exit, NOT FPlatformMisc::RequestExitWithStatus: the generic
	// implementation DISCARDS the return code (GenericPlatformMisc.cpp:1167) and
	// Mac does not override it, so every run — including "selected no tests" and
	// the usage error — exited 0 and the harness could not fail. Its warning
	// about that is invisible here because AppExit already closed the log.
	//
	// This cannot mask a harness failure: it runs only after AppExit returned.
	std::_Exit(ExitCode);
}
