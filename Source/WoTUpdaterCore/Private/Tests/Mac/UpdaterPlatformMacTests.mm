// Headless coverage for Mac code seals and translocation.
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "WoTBootstrapCore/NativeHandles.h"
#include "WoTUpdaterCore/UpdaterPlatform.h"

#if PLATFORM_MAC && WITH_AUTOMATION_TESTS

#include <errno.h>
#include <fcntl.h>
#include <libproc.h>
#include <signal.h>
#include <spawn.h>
#include <sys/unistd.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;


namespace
{
constexpr EAutomationTestFlags WoTUpdaterPlatformMacSpecFlags =
	EAutomationTestFlags::ProgramContext | EAutomationTestFlags::ProductFilter;

struct FWoTUpdaterPlatformMacSpecScope final
{
	FString Root;

	FWoTUpdaterPlatformMacSpecScope()
		: Root(FPaths::Combine(FPlatformProcess::UserTempDir(),
			TEXT("wot-updater-platform-mac-")
				+ FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower()))
	{
		IFileManager::Get().MakeDirectory(*Root, true);
	}

	~FWoTUpdaterPlatformMacSpecScope()
	{
		IFileManager::Get().DeleteDirectory(*Root, false, true);
	}
};

bool WoTUpdaterPlatformMacSpecSetMode(const FString& Path, uint32 Mode)
{
	TWoTBootstrapResult<FWoTNativeFileHandle> File =
		OpenNativeFileReadNoFollow(Path);
	return !File.IsError() && !SetNativeMode(File.GetValue(), Mode).IsError();
}

bool WoTUpdaterPlatformMacSpecRunCodeSign(const FString& BundlePath)
{
	const FTCHARToUTF8 BundleUtf8(*BundlePath);
	char* Arguments[] = {
		const_cast<char*>("/usr/bin/codesign"),
		const_cast<char*>("--force"),
		const_cast<char*>("--sign"),
		const_cast<char*>("-"),
		const_cast<char*>(BundleUtf8.Get()),
		nullptr,
	};
	pid_t Child = 0;
	const int Spawned = posix_spawn(
		&Child, Arguments[0], nullptr, nullptr, Arguments, environ);
	if (Spawned != 0) return false;
	int Status = 0;
	while (waitpid(Child, &Status, 0) < 0)
	{
		if (errno != EINTR) return false;
	}
	return WIFEXITED(Status) && WEXITSTATUS(Status) == 0;
}

bool WoTUpdaterPlatformMacSpecWriteInfoPlist(
	const FString& AppPath, const FString& ExecutableLeaf,
	const FString& ArgumentsPath = FString(),
	const FString& PidPath = FString(),
	const FString& EnvironmentPath = FString())
{
	const FString Contents = FPaths::Combine(AppPath, TEXT("Contents"));
	if (!IFileManager::Get().MakeDirectory(
			*FPaths::Combine(Contents, TEXT("MacOS")), true)
		|| !IFileManager::Get().MakeDirectory(
			*FPaths::Combine(Contents, TEXT("Resources")), true))
	{
		return false;
	}
	const FString FixtureKeys = ArgumentsPath.IsEmpty() ? FString()
		: FString::Printf(
			TEXT("<key>WoTLaunchServicesFixture</key><true/>")
			TEXT("<key>WoTLaunchServicesFixtureArgumentsPath</key><string>%s</string>")
			TEXT("<key>WoTLaunchServicesFixturePidPath</key><string>%s</string>")
			TEXT("<key>WoTLaunchServicesFixtureEnvironmentPath</key><string>%s</string>"),
			*ArgumentsPath, *PidPath, *EnvironmentPath);
	const FString Plist = FString::Printf(
		TEXT("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n")
		TEXT("<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" ")
		TEXT("\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n")
		TEXT("<plist version=\"1.0\"><dict>")
		TEXT("<key>CFBundleExecutable</key><string>%s</string>")
		TEXT("<key>CFBundleIdentifier</key><string>com.aleiby.wot.updater-platform-spec</string>")
		TEXT("<key>CFBundlePackageType</key><string>APPL</string>")
		TEXT("<key>CFBundleVersion</key><string>1</string>")
		TEXT("<key>LSUIElement</key><true/>")
		TEXT("%s</dict></plist>\n"), *ExecutableLeaf, *FixtureKeys);
	return FFileHelper::SaveStringToFile(
		Plist, *FPaths::Combine(Contents, TEXT("Info.plist")));
}

bool WoTUpdaterPlatformMacSpecBuildTrueBundle(
	const FString& AppPath, FString& OutSealedResource)
{
	const FString ExecutableLeaf = TEXT("SpecApp");
	if (!WoTUpdaterPlatformMacSpecWriteInfoPlist(AppPath, ExecutableLeaf))
		return false;
	TArray<uint8> Executable;
	if (!FFileHelper::LoadFileToArray(Executable, TEXT("/usr/bin/true")))
		return false;
	const FString ExecutablePath = FPaths::Combine(
		AppPath, TEXT("Contents/MacOS"), ExecutableLeaf);
	OutSealedResource = FPaths::Combine(
		AppPath, TEXT("Contents/Resources/sealed.txt"));
	return FFileHelper::SaveArrayToFile(Executable, *ExecutablePath)
		&& WoTUpdaterPlatformMacSpecSetMode(ExecutablePath, 0755u)
		&& FFileHelper::SaveStringToFile(TEXT("sealed\n"), *OutSealedResource)
		&& WoTUpdaterPlatformMacSpecRunCodeSign(AppPath);
}

} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUpdaterPlatformMacSealSpec,
	"WoT.UpdaterCore.MacPlatform.CodeSealSurvivesExchangeAndRejectsTamper",
	WoTUpdaterPlatformMacSpecFlags)

bool FWoTUpdaterPlatformMacSealSpec::RunTest(const FString&)
{
	FWoTUpdaterPlatformMacSpecScope Scope;
	const FString SignedApp = FPaths::Combine(Scope.Root, TEXT("Signed.app"));
	const FString OtherApp = FPaths::Combine(Scope.Root, TEXT("Other.app"));
	FString SealedResource;
	if (!TestTrue(TEXT("the ad-hoc signed app fixture is created"),
		WoTUpdaterPlatformMacSpecBuildTrueBundle(SignedApp, SealedResource))
		|| !TestTrue(TEXT("the exchange peer is created"),
			IFileManager::Get().MakeDirectory(*OtherApp, true)))
	{
		return false;
	}
	FWoTBootstrapStatus Before = WoTVerifyStagedCodeSeal(SignedApp);
	if (!TestFalse(TEXT("strict validation accepts the ad-hoc seal"),
		Before.IsError()))
	{
		if (Before.IsError()) AddError(Before.GetError().ToString());
		return false;
	}

	TWoTBootstrapResult<FWoTNativeDirectoryHandle> Root =
		OpenNativeDirectoryNoFollow(Scope.Root);
	if (Root.IsError()) return false;
	TWoTBootstrapResult<bool> Exchanged = ExchangeNativeEntriesAtNoFollow(
		Root.GetValue(), TEXT("Signed.app"), Root.GetValue(), TEXT("Other.app"));
	if (!TestTrue(TEXT("the real RENAME_SWAP succeeds"),
		!Exchanged.IsError() && Exchanged.GetValue()))
	{
		if (Exchanged.IsError()) AddError(Exchanged.GetError().ToString());
		return false;
	}
	const FString SwappedApp = FPaths::Combine(Scope.Root, TEXT("Other.app"));
	TestFalse(TEXT("the seal remains valid after the directory exchange"),
		WoTVerifyStagedCodeSeal(SwappedApp).IsError());
	const FString SwappedResource = FPaths::Combine(
		SwappedApp, TEXT("Contents/Resources/sealed.txt"));
	if (!FFileHelper::SaveStringToFile(TEXT("tampered\n"), *SwappedResource,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
		&IFileManager::Get(), FILEWRITE_Append))
	{
		AddError(TEXT("could not tamper with the sealed resource"));
		return false;
	}
	TestTrue(TEXT("strict validation rejects a tampered sealed resource"),
		WoTVerifyStagedCodeSeal(SwappedApp).IsError());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUpdaterPlatformMacTranslocationSpec,
	"WoT.UpdaterCore.MacPlatform.NormalPathIsNotTranslocated",
	WoTUpdaterPlatformMacSpecFlags)

bool FWoTUpdaterPlatformMacTranslocationSpec::RunTest(const FString&)
{
	FWoTUpdaterPlatformMacSpecScope Scope;
	const FString AppPath = FPaths::Combine(Scope.Root, TEXT("Normal.app"));
	FString Resource;
	if (!WoTUpdaterPlatformMacSpecBuildTrueBundle(AppPath, Resource)) return false;
	TestFalse(TEXT("an ordinary temporary app path is not translocated"),
		WoTIsRunningTreeTranslocated(AppPath));
	return !HasAnyErrors();
}

#endif
