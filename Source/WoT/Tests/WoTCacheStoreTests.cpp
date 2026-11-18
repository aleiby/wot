// Disposable current cache: publication, invalid selection, and profile ownership.
#include "Misc/AutomationTest.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "../Payload/WoTCacheStore.h"
#include "WoTBootstrapCore/Hash.h"
#include "WoTBootstrapCore/NativeHandles.h"
#include "WoTRetailReader/CacheManifest.h"

#if WITH_AUTOMATION_TESTS
namespace
{
constexpr EAutomationTestFlags SpecFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;
constexpr TCHAR LogicalId[] = TEXT("test.cache.store");
constexpr TCHAR Schema[] = TEXT("wot-test-v1");

struct FSpecRoot
{
	FSpecRoot()
	{
		Path = FPaths::Combine(FPaths::ConvertRelativePathToFull(FPaths::AutomationTransientDir()),
			TEXT("wot-cache-store-") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
		IFileManager::Get().MakeDirectory(*Path, true);
		ChildUserDirectory = Path + TEXT("-child-user");
		IFileManager::Get().MakeDirectory(*ChildUserDirectory, true);
		Cache = FPaths::Combine(Path, TEXT("ContentCache"));
		auto Acquired = AcquireWoTProfileSessionShared(Path);
		check(!Acquired.IsError());
		Session = MoveTemp(Acquired).GetValue();
	}
	~FSpecRoot()
	{
		Session = FWoTProfileSession();
		IFileManager::Get().DeleteDirectory(*Path, false, true);
		IFileManager::Get().DeleteDirectory(*ChildUserDirectory, false, true);
	}
	FString Path;
	FString Cache;
	FString ChildUserDirectory;
	FWoTProfileSession Session;
};

TArray<uint8> FixtureBytes()
{
	return {0x73, 0x31, 0x2d, 0x63, 0x61, 0x63, 0x68, 0x65};
}

FString BlobPath(const FSpecRoot& Root)
{
	const FTCHARToUTF8 Utf8(LogicalId);
	const FWoTSha256 NameHash = Sha256(TConstArrayView<uint8>(
		reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()));
	return FPaths::Combine(Root.Cache, TEXT("building"), TEXT("blobs"),
		NameHash.ToHex() + TEXT(".blob"));
}

FWoTCacheBuildResult MakeBuild(TConstArrayView<uint8> Bytes)
{
	FWoTCacheBuildResult Build;
	FWoTCacheBlob& Blob = Build.Manifest.Blobs.AddDefaulted_GetRef();
	Blob.LogicalId = LogicalId;
	Blob.Schema = Schema;
	Blob.Sha256 = Sha256(Bytes);
	Blob.Size = Bytes.Num();
	Blob.Satisfies.Add(FWoTRequiredFactId{TEXT("spec.cache.store")});
	auto Encoded = WriteCanonicalCacheManifest(Build.Manifest);
	if (!Encoded.IsError())
	{
		Build.CanonicalManifest = MoveTemp(Encoded).GetValue();
		Build.ManifestSha256 = Sha256(Build.CanonicalManifest);
	}
	return Build;
}

bool BuildCurrent(FAutomationTestBase& Test, FWoTCacheStore& Store,
	TConstArrayView<uint8> Bytes, FWoTProfileSession& Session)
{
	auto Exclusive = Session.TryAcquireExclusive();
	if (!Test.TestTrue(TEXT("profile session becomes exclusive"),
		!Exclusive.IsError() && Exclusive.GetValue())) return false;
	auto Begun = Store.BeginBuild(Session);
	if (!Test.TestFalse(TEXT("BeginBuild succeeds"), Begun.IsError())) return false;
	auto Written = Store.WriteObject(LogicalId, Schema, Bytes, Sha256(Bytes), 1024);
	if (!Test.TestFalse(TEXT("WriteObject succeeds"), Written.IsError())) return false;
	auto Published = Store.Publish(MakeBuild(Bytes));
	if (!Test.TestFalse(TEXT("Publish succeeds"), Published.IsError())) return false;
	return Test.TestFalse(TEXT("profile session returns to shared"),
		Session.RestoreShared().IsError());
}

bool TakeExclusive(FAutomationTestBase& Test, FWoTProfileSession& Session)
{
	auto Exclusive = Session.TryAcquireExclusive();
	return Test.TestTrue(TEXT("profile session becomes exclusive"),
		!Exclusive.IsError() && Exclusive.GetValue());
}

bool IsCurrentAbsent(FAutomationTestBase& Test, FWoTCacheStore& Store)
{
	auto Current = Store.TryOpenCurrent({}, nullptr);
	if (!Test.TestFalse(TEXT("selection is not an I/O error"), Current.IsError())) return false;
	return Test.TestFalse(TEXT("current is absent"), Current.GetValue().IsSet());
}

bool IsCurrentPresent(FAutomationTestBase& Test, FWoTCacheStore& Store)
{
	auto Current = Store.TryOpenCurrent({}, nullptr);
	if (!Test.TestFalse(TEXT("selection succeeds"), Current.IsError())) return false;
	return Test.TestTrue(TEXT("current is selected"), Current.GetValue().IsSet());
}

TWoTBootstrapResult<FWoTCacheStore> OpenFixture(const FSpecRoot& Root)
{
	return FWoTCacheStore::ForTesting(Root.Cache);
}

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCacheInterruptedBuildingSpec,
	"WoT.Cache.Store.InterruptedBuildingDiscardedBeforeRebuild", SpecFlags)
bool FWoTCacheInterruptedBuildingSpec::RunTest(const FString&)
{
	FSpecRoot Root;
	const TArray<uint8> Bytes = FixtureBytes();
	{
		auto Store = OpenFixture(Root);
		if (!TestFalse(TEXT("fixture opens"), Store.IsError())) return false;
		if (!TakeExclusive(*this, Root.Session)) return false;
		if (!TestFalse(TEXT("build begins"), Store.GetValue().BeginBuild(Root.Session).IsError())) return false;
		if (!TestFalse(TEXT("object writes"), Store.GetValue().WriteObject(
			LogicalId, Schema, Bytes, Sha256(Bytes), 1024).IsError())) return false;
		TestTrue(TEXT("interrupted building exists"), IFileManager::Get().DirectoryExists(
			*FPaths::Combine(Root.Cache, TEXT("building"))));
	}
	if (!TestFalse(TEXT("interrupted owner returns to shared"),
		Root.Session.RestoreShared().IsError())) return false;
	auto Store = OpenFixture(Root);
	if (!TestFalse(TEXT("store reopens"), Store.IsError())) return false;
	if (!IsCurrentAbsent(*this, Store.GetValue())) return false;
	if (!BuildCurrent(*this, Store.GetValue(), Bytes, Root.Session)) return false;
	TestFalse(TEXT("stale building was discarded"), IFileManager::Get().DirectoryExists(
		*FPaths::Combine(Root.Cache, TEXT("building"))));
	return IsCurrentPresent(*this, Store.GetValue());
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCachePublicationSpec,
	"WoT.Cache.Store.PublicationMakesOnlyCurrentWithManifestLast", SpecFlags)
bool FWoTCachePublicationSpec::RunTest(const FString&)
{
	FSpecRoot Root;
	{
		auto Store = OpenFixture(Root);
		if (!TestFalse(TEXT("fixture opens"), Store.IsError())) return false;
		const TArray<uint8> Bytes = FixtureBytes();
		if (!TakeExclusive(*this, Root.Session)) return false;
		if (!TestFalse(TEXT("build begins"), Store.GetValue().BeginBuild(Root.Session).IsError())) return false;
		if (!TestFalse(TEXT("object writes"), Store.GetValue().WriteObject(
			LogicalId, Schema, Bytes, Sha256(Bytes), 1024).IsError())) return false;
		TestFalse(TEXT("manifest is withheld while building"), IFileManager::Get().FileExists(
			*FPaths::Combine(Root.Cache, TEXT("building"), TEXT("manifest.json"))));
		TestFalse(TEXT("current is withheld while building"), IFileManager::Get().DirectoryExists(
			*FPaths::Combine(Root.Cache, TEXT("current"))));
		if (!TestFalse(TEXT("publication succeeds"), Store.GetValue().Publish(MakeBuild(Bytes)).IsError()))
			return false;
		if (!TestFalse(TEXT("published owner returns to shared"),
			Root.Session.RestoreShared().IsError())) return false;
		TestTrue(TEXT("current manifest is published"), IFileManager::Get().FileExists(
			*FPaths::Combine(Root.Cache, TEXT("current"), TEXT("manifest.json"))));
		TestFalse(TEXT("building no longer exists"), IFileManager::Get().DirectoryExists(
			*FPaths::Combine(Root.Cache, TEXT("building"))));
		TestTrue(TEXT("root session lock exists"), IFileManager::Get().FileExists(
			*FPaths::Combine(Root.Path, WoTProfileSessionLockLeaf())));
		if (!IsCurrentPresent(*this, Store.GetValue())) return false;
	}
	const FString MovedRoot = FPaths::Combine(Root.Path, TEXT("MovedCache"));
	if (!TestTrue(TEXT("closed cache root moves within its fixture directory"),
		IFileManager::Get().Move(*MovedRoot, *Root.Cache, false, false, false, true))) return false;
	auto Reopened = FWoTCacheStore::ForTesting(MovedRoot);
	if (!TestFalse(TEXT("relocated cache store opens"), Reopened.IsError())) return false;
	auto Current = Reopened.GetValue().TryOpenCurrent({}, nullptr);
	if (!TestTrue(TEXT("relocated current opens without rebuilding"),
		!Current.IsError() && Current.GetValue().IsSet())) return false;
	const TArray<FString> Required{LogicalId};
	return TestFalse(TEXT("relocated snapshot authenticates its existing blob"),
		Current.GetValue()->VerifyCriticalClosureForTesting(Required, nullptr).IsError());
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCacheInvalidSelectionSpec,
	"WoT.Cache.Store.InvalidManifestDamagedAndMismatchedIdentityMiss", SpecFlags)
bool FWoTCacheInvalidSelectionSpec::RunTest(const FString&)
{
	FSpecRoot Root;
	auto Store = OpenFixture(Root);
	if (!TestFalse(TEXT("fixture opens"), Store.IsError())) return false;
	const TArray<uint8> Bytes = FixtureBytes();
	if (!BuildCurrent(*this, Store.GetValue(), Bytes, Root.Session)) return false;
	FWoTCacheGenerationBuildIdentity Wrong;
	Wrong.bHasBuildIdentity = true;
	Wrong.InstallerSha256 = Sha256(Bytes);
	auto Mismatch = Store.GetValue().TryOpenCurrent(Wrong, nullptr);
	if (!TestFalse(TEXT("identity selection is not an I/O error"), Mismatch.IsError())) return false;
	TestFalse(TEXT("wrong identity is not selected"), Mismatch.GetValue().IsSet());
	const FString Manifest = FPaths::Combine(Root.Cache, TEXT("current"), TEXT("manifest.json"));
	if (!TestTrue(TEXT("corrupt manifest written"), FFileHelper::SaveStringToFile(
		TEXT("{bad json"), *Manifest))) return false;
	if (!IsCurrentAbsent(*this, Store.GetValue())) return false;
	if (!BuildCurrent(*this, Store.GetValue(), Bytes, Root.Session)) return false;
	const FString Damaged = FPaths::Combine(Root.Cache, TEXT("current"), TEXT("damaged"));
	if (!TestTrue(TEXT("damaged marker written"), FFileHelper::SaveStringToFile(
		TEXT(""), *Damaged))) return false;
	if (!IsCurrentAbsent(*this, Store.GetValue())) return false;
	if (!BuildCurrent(*this, Store.GetValue(), Bytes, Root.Session)) return false;
	if (!TestTrue(TEXT("manifest removed"), IFileManager::Get().Delete(*Manifest))) return false;
	if (!IsCurrentAbsent(*this, Store.GetValue())) return false;
	if (!TakeExclusive(*this, Root.Session)) return false;
	if (!TestFalse(TEXT("delete cache"), Store.GetValue().DeleteCache(Root.Session).IsError())) return false;
	if (!TestFalse(TEXT("delete owner returns to shared"),
		Root.Session.RestoreShared().IsError())) return false;
	if (!TestTrue(TEXT("create root"), IFileManager::Get().MakeDirectory(*Root.Cache, true)))
		return false;
	if (!TestTrue(TEXT("replace current directory with file"), FFileHelper::SaveStringToFile(
		TEXT("invalid current"), *FPaths::Combine(Root.Cache, TEXT("current"))))) return false;
	return IsCurrentAbsent(*this, Store.GetValue());
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCacheIncompletePublicationSpec,
	"WoT.Cache.Store.PublicationRejectsMissingAndShortObjects", SpecFlags)
bool FWoTCacheIncompletePublicationSpec::RunTest(const FString&)
{
	FSpecRoot Root;
	auto Store = OpenFixture(Root);
	if (!TestFalse(TEXT("fixture opens"), Store.IsError())) return false;
	const TArray<uint8> Bytes = FixtureBytes();
	for (int32 Attempt = 0; Attempt < 2; ++Attempt)
	{
		if (!TakeExclusive(*this, Root.Session)) return false;
		if (!TestFalse(TEXT("build begins"), Store.GetValue().BeginBuild(Root.Session).IsError())) return false;
		if (!TestFalse(TEXT("object writes"), Store.GetValue().WriteObject(
			LogicalId, Schema, Bytes, Sha256(Bytes), 1024).IsError())) return false;
		const FString Blob = BlobPath(Root);
		if (Attempt == 0)
		{
			if (!TestTrue(TEXT("blob removed"), IFileManager::Get().Delete(*Blob))) return false;
		}
		else
		{
			const TArray<uint8> Short = {Bytes[0]};
			if (!TestTrue(TEXT("blob shortened"), FFileHelper::SaveArrayToFile(Short, *Blob)))
				return false;
		}
		TestTrue(TEXT("publication refuses incomplete object"),
			Store.GetValue().Publish(MakeBuild(Bytes)).IsError());
		if (!TestFalse(TEXT("failed publication owner returns to shared"),
			Root.Session.RestoreShared().IsError())) return false;
		TestFalse(TEXT("failed publication has no current"), IFileManager::Get().DirectoryExists(
			*FPaths::Combine(Root.Cache, TEXT("current"))));
	}
	return IsCurrentAbsent(*this, Store.GetValue());
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCacheDeleteLockSpec,
	"WoT.Cache.Store.DeletePreservesProfileSessionLock", SpecFlags)
bool FWoTCacheDeleteLockSpec::RunTest(const FString&)
{
	FSpecRoot Root;
	auto Store = OpenFixture(Root);
	if (!TestFalse(TEXT("fixture opens"), Store.IsError())) return false;
	if (!BuildCurrent(*this, Store.GetValue(), FixtureBytes(), Root.Session)) return false;
	if (!TakeExclusive(*this, Root.Session)) return false;
	if (!TestFalse(TEXT("delete succeeds"), Store.GetValue().DeleteCache(Root.Session).IsError())) return false;
	TestFalse(TEXT("cache root is gone"), IFileManager::Get().DirectoryExists(*Root.Cache));
	TestTrue(TEXT("profile session lock survives"), IFileManager::Get().FileExists(
		*FPaths::Combine(Root.Path, WoTProfileSessionLockLeaf())));
	if (!TestFalse(TEXT("delete owner returns to shared"),
		Root.Session.RestoreShared().IsError())) return false;
	return IsCurrentAbsent(*this, Store.GetValue());
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCacheBlockedManifestDestinationSpec,
	"WoT.Cache.Store.BlockedManifestDestinationDoesNotPublish", SpecFlags)
bool FWoTCacheBlockedManifestDestinationSpec::RunTest(const FString&)
{
	FSpecRoot Root;
	auto Store = OpenFixture(Root);
	if (!TestFalse(TEXT("fixture opens"), Store.IsError())) return false;
	const TArray<uint8> Bytes = FixtureBytes();
	if (!TakeExclusive(*this, Root.Session)) return false;
	if (!TestFalse(TEXT("build begins"), Store.GetValue().BeginBuild(Root.Session).IsError())) return false;
	if (!TestFalse(TEXT("object writes"), Store.GetValue().WriteObject(
		LogicalId, Schema, Bytes, Sha256(Bytes), 1024).IsError())) return false;
	const FString BlockedManifest = FPaths::Combine(
		Root.Cache, TEXT("building"), TEXT("manifest.json"));
	if (!TestTrue(TEXT("directory obstructs manifest destination"),
		IFileManager::Get().MakeDirectory(*BlockedManifest, false))) return false;
	TestTrue(TEXT("real filesystem create failure rejects publication"),
		Store.GetValue().Publish(MakeBuild(Bytes)).IsError());
	if (!TestFalse(TEXT("failed publication owner returns to shared"),
		Root.Session.RestoreShared().IsError())) return false;
	TestFalse(TEXT("no current published after create failure"),
		IFileManager::Get().DirectoryExists(*FPaths::Combine(Root.Cache, TEXT("current"))));
	return IsCurrentAbsent(*this, Store.GetValue());
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCacheRequiresExclusiveOwnerSpec,
	"WoT.Cache.Store.MutationRequiresExclusiveProfileOwner", SpecFlags)
bool FWoTCacheRequiresExclusiveOwnerSpec::RunTest(const FString&)
{
	FSpecRoot Root;
	auto Store = OpenFixture(Root);
	if (!TestFalse(TEXT("store opens"), Store.IsError())) return false;
	TestTrue(TEXT("SHARED refuses build"),
		Store.GetValue().BeginBuild(Root.Session).IsError());
	TestTrue(TEXT("SHARED refuses delete"),
		Store.GetValue().DeleteCache(Root.Session).IsError());
	return TestFalse(TEXT("refused mutation creates no cache root"),
		IFileManager::Get().DirectoryExists(*Root.Cache));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCacheTwoProcessLockSpec,
	"WoT.Cache.Store.TwoProcessesShareAndBlockRebuild", SpecFlags)
bool FWoTCacheTwoProcessLockSpec::RunTest(const FString&)
{
	FSpecRoot Root;
	auto Store = OpenFixture(Root);
	if (!TestFalse(TEXT("fixture opens"), Store.IsError())) return false;
	if (!BuildCurrent(*this, Store.GetValue(), FixtureBytes(), Root.Session)) return false;
	const FString Ready = FPaths::Combine(Root.Path, TEXT("child-ready"));
	const FString Release = FPaths::Combine(Root.Path, TEXT("child-release"));
#if PLATFORM_WINDOWS
	const FString Runner = FPaths::Combine(FPaths::ProjectDir(), TEXT("Binaries/Win64/WoTBootstrapCoreTests.exe"));
#elif PLATFORM_MAC
	FString Runner = FPaths::Combine(FPaths::ProjectDir(), TEXT("Binaries/Mac/WoTBootstrapCoreTests"));
	if (!IFileManager::Get().FileExists(*Runner))
	{
		Runner = FPaths::Combine(FPaths::ProjectDir(),
			TEXT("Binaries/Mac/WoTBootstrapCoreTests.app/Contents/MacOS/WoTBootstrapCoreTests"));
	}
#else
	AddError(TEXT("no cache-lock probe runner path for this platform"));
	return false;
#endif
	if (!TestTrue(TEXT("native runner exists"), IFileManager::Get().FileExists(*Runner))) return false;
	uint32 ChildPid = 0;
	const FString Arguments = FString::Printf(
		TEXT("-UserDir=\"%s\" --profile-session-probe=\"%s\" --ready=\"%s\" --release=\"%s\""),
		*Root.ChildUserDirectory, *Root.Path, *Ready, *Release);
	FProcHandle Child;
	{
		auto Current = Store.GetValue().TryOpenCurrent({}, nullptr);
		if (!TestFalse(TEXT("parent opens current"), Current.IsError())) return false;
		if (!TestTrue(TEXT("parent holds a current lease"), Current.GetValue().IsSet())) return false;
		Child = FPlatformProcess::CreateProc(*Runner, *Arguments, false, false, false,
			&ChildPid, 0, *FPaths::GetPath(Runner), nullptr);
		if (!TestTrue(TEXT("child starts"), Child.IsValid())) return false;
		const double Deadline = FPlatformTime::Seconds() + 20.0;
		while (!IFileManager::Get().FileExists(*Ready) && FPlatformProcess::IsProcRunning(Child)
			&& FPlatformTime::Seconds() < Deadline)
			FPlatformProcess::Sleep(0.01f);
		if (!TestTrue(TEXT("child acquired shared lock and read manifest"),
			IFileManager::Get().FileExists(*Ready)))
		{
			FPlatformProcess::TerminateProc(Child, true);
			FPlatformProcess::WaitForProc(Child);
			FPlatformProcess::CloseProc(Child);
			return false;
		}
	}
	ON_SCOPE_EXIT
	{
		if (Child.IsValid())
		{
			if (FPlatformProcess::IsProcRunning(Child))
			{
				FPlatformProcess::TerminateProc(Child, true);
				FPlatformProcess::WaitForProc(Child);
			}
			FPlatformProcess::CloseProc(Child);
		}
	};
	auto Contended = Root.Session.TryAcquireExclusive();
	TestTrue(TEXT("child process SHARED blocks parent EXCLUSIVE"),
		!Contended.IsError() && !Contended.GetValue());
	TestTrue(TEXT("refused rebuild mutates nothing"),
		IFileManager::Get().DirectoryExists(
			*FPaths::Combine(Root.Cache, TEXT("current"))));
	TestTrue(TEXT("SHARED owner cannot bypass contention for delete"),
		Store.GetValue().DeleteCache(Root.Session).IsError());
	if (!TestTrue(TEXT("release child"), FFileHelper::SaveStringToFile(
		TEXT("release"), *Release))) return false;
	FPlatformProcess::WaitForProc(Child);
	int32 ExitCode = -1;
	if (!TestTrue(TEXT("child exit status obtained"),
		FPlatformProcess::GetProcReturnCode(Child, &ExitCode))) return false;
	if (!TestEqual(TEXT("child exited normally"), ExitCode, 0)) return false;
	if (!TakeExclusive(*this, Root.Session)) return false;
	if (!TestFalse(TEXT("build accepted after child release"),
		Store.GetValue().BeginBuild(Root.Session).IsError())) return false;
	TestFalse(TEXT("rebuild cleared old current"), IFileManager::Get().DirectoryExists(
		*FPaths::Combine(Root.Cache, TEXT("current"))));
	const TArray<uint8> Bytes = FixtureBytes();
	if (!TestFalse(TEXT("replacement object writes"), Store.GetValue().WriteObject(
		LogicalId, Schema, Bytes, Sha256(Bytes), 1024).IsError())) return false;
	if (!TestFalse(TEXT("replacement publishes"), Store.GetValue().Publish(MakeBuild(Bytes)).IsError()))
		return false;
	if (!TestFalse(TEXT("delete accepted after child release"),
		Store.GetValue().DeleteCache(Root.Session).IsError())) return false;
	TestTrue(TEXT("profile lock persists after deletion"),
		IFileManager::Get().FileExists(*FPaths::Combine(
			Root.Path, WoTProfileSessionLockLeaf())));
	return TestFalse(TEXT("owner restores SHARED after mutation"),
		Root.Session.RestoreShared().IsError());
}

#if PLATFORM_WINDOWS
// Windows refuses to rename a directory while any handle is open on anything
// below it, whatever that handle shares. Antivirus scanners, the search indexer,
// sync clients and Explorer open freshly written files briefly, so publication
// must outlast such a holder. The holder here shares read, write and delete like
// the game's own readers, so only the rename itself can be what it blocks.
namespace
{
bool BeginHeldBuild(FAutomationTestBase& Test, const FSpecRoot& Root,
	FWoTCacheStore& Store, FWoTProfileSession& Session,
	TConstArrayView<uint8> Bytes, FWoTNativeFileHandle& OutHeld)
{
	if (!TakeExclusive(Test, Session)) return false;
	if (!Test.TestFalse(TEXT("build begins"), Store.BeginBuild(Session).IsError())) return false;
	if (!Test.TestFalse(TEXT("object writes"), Store.WriteObject(
		LogicalId, Schema, Bytes, Sha256(Bytes), 1024).IsError())) return false;
	auto Held = OpenNativeFileReadNoFollow(BlobPath(Root));
	if (!Test.TestFalse(TEXT("another holder opens a blob below building"), Held.IsError()))
		return false;
	OutHeld = MoveTemp(Held).GetValue();
	return true;
}

struct FDirectoryRenameRetrySeam
{
	~FDirectoryRenameRetrySeam()
	{
		WoTSetNativeDirectoryRenameRetryObserverForTesting({});
		WoTSetNativeDirectoryRenameRetryBudgetForTesting(-1.0);
	}
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCacheTransientDescendantHolderSpec,
	"WoT.Cache.Store.PublicationSurvivesTransientDescendantHolder", SpecFlags)
bool FWoTCacheTransientDescendantHolderSpec::RunTest(const FString&)
{
	FSpecRoot Root;
	auto Store = OpenFixture(Root);
	if (!TestFalse(TEXT("fixture opens"), Store.IsError())) return false;
	const TArray<uint8> Bytes = FixtureBytes();
	FWoTNativeFileHandle Held;
	if (!BeginHeldBuild(*this, Root, Store.GetValue(), Root.Session, Bytes, Held)) return false;
	FDirectoryRenameRetrySeam Seam;
	TArray<int32> Refusals;
	// The holder lets go after the first refused attempt, as a scanner would.
	WoTSetNativeDirectoryRenameRetryObserverForTesting(
		[&Refusals, &Held](int32 FailedAttempts)
	{
		Refusals.Add(FailedAttempts);
		Held = FWoTNativeFileHandle();
	});
	const FWoTBootstrapStatus Published = Store.GetValue().Publish(MakeBuild(Bytes));
	WoTSetNativeDirectoryRenameRetryObserverForTesting({});
	Held = FWoTNativeFileHandle();
	if (Published.IsError())
	{
		AddError(Published.GetError().ToString());
	}
	// At least the held blob's refusal; a real scanner or indexer touching the
	// fresh files can add more before the rename goes through.
	TestTrue(TEXT("the held blob refused the first attempt"),
		Refusals.Num() >= 1 && Refusals[0] == 1);
	if (!TestFalse(TEXT("publication succeeds once the holder lets go"), Published.IsError()))
		return false;
	if (!TestFalse(TEXT("published owner returns to shared"),
		Root.Session.RestoreShared().IsError())) return false;
	TestTrue(TEXT("current manifest is published"), IFileManager::Get().FileExists(
		*FPaths::Combine(Root.Cache, TEXT("current"), TEXT("manifest.json"))));
	TestFalse(TEXT("building no longer exists"), IFileManager::Get().DirectoryExists(
		*FPaths::Combine(Root.Cache, TEXT("building"))));
	return IsCurrentPresent(*this, Store.GetValue());
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCachePersistentDescendantHolderSpec,
	"WoT.Cache.Store.PublicationPastRetryBudgetFailsWithStatus", SpecFlags)
bool FWoTCachePersistentDescendantHolderSpec::RunTest(const FString&)
{
	FSpecRoot Root;
	auto Store = OpenFixture(Root);
	if (!TestFalse(TEXT("fixture opens"), Store.IsError())) return false;
	const TArray<uint8> Bytes = FixtureBytes();
	FWoTNativeFileHandle Held;
	if (!BeginHeldBuild(*this, Root, Store.GetValue(), Root.Session, Bytes, Held)) return false;
	FDirectoryRenameRetrySeam Seam;
	constexpr double BudgetSeconds = 0.3;
	WoTSetNativeDirectoryRenameRetryBudgetForTesting(BudgetSeconds);
	int32 Refusals = 0;
	WoTSetNativeDirectoryRenameRetryObserverForTesting(
		[&Refusals](int32) { ++Refusals; });
	const double Started = FPlatformTime::Seconds();
	const FWoTBootstrapStatus Published = Store.GetValue().Publish(MakeBuild(Bytes));
	const double Elapsed = FPlatformTime::Seconds() - Started;
	if (!TestTrue(TEXT("publication fails while the holder stays"), Published.IsError()))
		return false;
	const FWoTBootstrapError& Refused = Published.GetError();
	AddInfo(Refused.ToString());
	TestTrue(TEXT("the failure is Windows error 5"), Refused.NativeCode == 5u);
	TestTrue(TEXT("the message carries the raw NTSTATUS"),
		Refused.Message.Contains(TEXT("NTSTATUS 0xC0000022")));
	TestTrue(TEXT("the message carries the retry count"), Refused.Message.Contains(
		FString::Printf(TEXT("; %d retries over "), Refusals)));
	TestTrue(TEXT("the refused rename was retried"), Refusals >= 2);
	TestTrue(TEXT("the retry waited out its budget and no longer"),
		Elapsed >= BudgetSeconds && Elapsed < 5.0);
	Held = FWoTNativeFileHandle();
	if (!TestFalse(TEXT("failed publication owner returns to shared"),
		Root.Session.RestoreShared().IsError())) return false;
	TestFalse(TEXT("no current after the refused rename"), IFileManager::Get().DirectoryExists(
		*FPaths::Combine(Root.Cache, TEXT("current"))));
	TestTrue(TEXT("building stays whole"), IFileManager::Get().FileExists(
		*FPaths::Combine(Root.Cache, TEXT("building"), TEXT("manifest.json")))
		&& IFileManager::Get().FileExists(*BlobPath(Root)));
	if (!IsCurrentAbsent(*this, Store.GetValue())) return false;
	// Relaunching rebuilds over the stranded building and publishes.
	if (!BuildCurrent(*this, Store.GetValue(), Bytes, Root.Session)) return false;
	return IsCurrentPresent(*this, Store.GetValue());
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCachePublicationCollisionSpec,
	"WoT.Cache.Store.PublicationCollisionIsNotRetried", SpecFlags)
bool FWoTCachePublicationCollisionSpec::RunTest(const FString&)
{
	FSpecRoot Root;
	auto Store = OpenFixture(Root);
	if (!TestFalse(TEXT("fixture opens"), Store.IsError())) return false;
	const TArray<uint8> Bytes = FixtureBytes();
	if (!TakeExclusive(*this, Root.Session)) return false;
	if (!TestFalse(TEXT("build begins"), Store.GetValue().BeginBuild(Root.Session).IsError())) return false;
	if (!TestFalse(TEXT("object writes"), Store.GetValue().WriteObject(
		LogicalId, Schema, Bytes, Sha256(Bytes), 1024).IsError())) return false;
	if (!TestTrue(TEXT("a directory already holds the current name"),
		IFileManager::Get().MakeDirectory(*FPaths::Combine(Root.Cache, TEXT("current")), false)))
		return false;
	FDirectoryRenameRetrySeam Seam;
	int32 Refusals = 0;
	WoTSetNativeDirectoryRenameRetryObserverForTesting(
		[&Refusals](int32) { ++Refusals; });
	const FWoTBootstrapStatus Published = Store.GetValue().Publish(MakeBuild(Bytes));
	if (!TestTrue(TEXT("publication refuses an existing destination"), Published.IsError()))
		return false;
	const FWoTBootstrapError& Refused = Published.GetError();
	AddInfo(Refused.ToString());
	TestTrue(TEXT("the failure is Windows error 183"), Refused.NativeCode == 183u);
	TestTrue(TEXT("the message carries the raw NTSTATUS and no retries"),
		Refused.Message.Contains(TEXT("NTSTATUS 0xC0000035; 0 retries over ")));
	TestEqual(TEXT("an existing destination is never retried"), Refusals, 0);
	return TestFalse(TEXT("failed publication owner returns to shared"),
		Root.Session.RestoreShared().IsError());
}
#endif
#endif
