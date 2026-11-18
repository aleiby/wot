// Held-handle package specs driven by the source-derived Inno 5.5.6 fixtures.
#include "Misc/AutomationTest.h"

#include "Async/Async.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "NativeHandleTestFactory.h"
#include "WoTBootstrapCore/Hash.h"
#include "WoTBootstrapCore/NativeHandles.h"
#include "WoTRetailReader/Package.h"

#if WITH_AUTOMATION_TESTS

namespace
{
constexpr EAutomationTestFlags WoTRetailPackageSpecFlags =
	EAutomationTestFlags::ProgramContext | EAutomationTestFlags::EditorContext
	| EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter;

struct FWoTRetailPackageSpecRoot
{
	FString Path = WoTNativeHandleTestFactory::CreateTemporaryRoot();
	~FWoTRetailPackageSpecRoot()
	{
		WoTNativeHandleTestFactory::RemoveTree(Path);
	}
};

FString WoTRetailPackageSpecFixturePath(const TCHAR* Variant)
{
	const FString ThisFile(ANSI_TO_TCHAR(__FILE__));
	const FString Name = FString::Printf(TEXT("innoextract-5.5.6-%s.bin"), Variant);
	return FPaths::ConvertRelativePathToFull(FPaths::GetPath(ThisFile)
		/ TEXT("../../../../Tools/dist/fixtures") / Name);
}

bool WoTRetailPackageSpecLoadFixture(const TCHAR* Variant, TArray<uint8>& OutBytes)
{
	return FFileHelper::LoadFileToArray(OutBytes,
		*WoTRetailPackageSpecFixturePath(Variant));
}

TWoTBootstrapResult<FWoTHeldInstallerSnapshot> WoTRetailPackageSpecSnapshot(
	const FString& Path)
{
	using FResult = TWoTBootstrapResult<FWoTHeldInstallerSnapshot>;
	TWoTBootstrapResult<FWoTNativeFileHandle> Opened = OpenNativeFileReadNoFollow(Path);
	if (Opened.IsError())
	{
		return FResult::Error(MoveTemp(Opened).GetError());
	}
	TWoTBootstrapResult<TArray<uint8>> Read = Opened.GetValue().ReadAll();
	if (Read.IsError())
	{
		return FResult::Error(MoveTemp(Read).GetError());
	}
	FWoTHeldInstallerSnapshot Snapshot;
	Snapshot.Size = static_cast<uint64>(Read.GetValue().Num());
	Snapshot.Sha256 = Sha256(Read.GetValue());
	Snapshot.Identity = Opened.GetValue().GetIdentity();
	Snapshot.Handle = MoveTemp(Opened).GetValue();
	return FResult::Success(MoveTemp(Snapshot));
}

class FWoTRetailPackageSpecCollectSink final : public IWoTByteSink
{
public:
	FWoTBootstrapStatus Consume(TConstArrayView<uint8> Bytes) override
	{
		Collected.Append(Bytes.GetData(), Bytes.Num());
		return FWoTBootstrapStatus::Success();
	}

	TArray<uint8> Collected;
};

struct FWoTRetailPackageSpecPause
{
	static void Invoke(void* Context)
	{
		FWoTRetailPackageSpecPause* const Pause =
			static_cast<FWoTRetailPackageSpecPause*>(Context);
		Pause->Started.Trigger();
		Pause->bContinued = Pause->Continue.Wait(10000);
	}

	FEvent& Started;
	FEvent& Continue;
	bool bContinued = false;
};

bool WoTRetailPackageSpecMutateUntilIdentityChange(const FString& Path,
	TConstArrayView<uint8> MutatedBytes,
	const FWoTNativeFileHandle& Observer,
	const FWoTNativeFileIdentity& Original)
{
	const double Deadline = FPlatformTime::Seconds() + 2.0;
	do
	{
		if (!WoTNativeHandleTestFactory::WriteFile(Path, MutatedBytes))
		{
			return false;
		}
		TWoTBootstrapResult<FWoTNativeFileIdentity> Current =
			Observer.QueryCurrentIdentity();
		if (Current.IsError() || Current.GetValue() != Original)
		{
			return !Current.IsError();
		}
		FPlatformProcess::Sleep(0.001f);
	}
	while (FPlatformTime::Seconds() < Deadline);
	return false;
}

TWoTBootstrapResult<FWoTRetailPackage> WoTRetailPackageSpecOpenVariant(
	FWoTRetailPackageSpecRoot& Root, const TCHAR* Variant,
	const FWoTArchiveLimits& Limits = FWoTArchiveLimits())
{
	using FResult = TWoTBootstrapResult<FWoTRetailPackage>;
	TArray<uint8> Fixture;
	if (!WoTRetailPackageSpecLoadFixture(Variant, Fixture))
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Io,
			TEXT("the checked-in Inno fixture is missing"), Variant));
	}
	const FString Installer = Root.Path / FString::Printf(TEXT("%s.exe"), Variant);
	if (!WoTNativeHandleTestFactory::WriteFile(Installer, Fixture))
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Io,
			TEXT("the checked-in Inno fixture could not be staged"), Installer));
	}
	TWoTBootstrapResult<FWoTHeldInstallerSnapshot> Snapshot =
		WoTRetailPackageSpecSnapshot(Installer);
	if (Snapshot.IsError())
	{
		return FResult::Error(MoveTemp(Snapshot).GetError());
	}
	return FWoTRetailPackage::Open(MoveTemp(Snapshot).GetValue(), Limits);
}
}  // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailPackageIndexSpec,
	"WoT.RetailReader.Package.IndexAndBounds", WoTRetailPackageSpecFlags)

bool FWoTRetailPackageIndexSpec::RunTest(const FString&)
{
	FWoTRetailPackageSpecRoot Root;
	TArray<uint8> Fixture;
	if (!TestFalse(TEXT("temporary root exists"), Root.Path.IsEmpty())
		|| !TestTrue(TEXT("source-derived fixture loads"),
			WoTRetailPackageSpecLoadFixture(TEXT("standard"), Fixture)))
	{
		return false;
	}
	const FString Installer = Root.Path / TEXT("setup.exe");
	if (!TestTrue(TEXT("fixture is staged"),
		WoTNativeHandleTestFactory::WriteFile(Installer, Fixture)))
	{
		return false;
	}

	TWoTBootstrapResult<FWoTHeldInstallerSnapshot> Snapshot =
		WoTRetailPackageSpecSnapshot(Installer);
	if (!TestFalse(TEXT("snapshot opens through Task 5 factory"), Snapshot.IsError()))
	{
		return false;
	}
	TWoTBootstrapResult<FWoTNativeFileHandle> Duplicate =
		Snapshot.GetValue().Handle.DuplicateForRead();
	if (TestFalse(TEXT("held handle duplicates"), Duplicate.IsError()))
	{
		TWoTBootstrapResult<TArray<uint8>> DuplicateBytes = Duplicate.GetValue().ReadAll();
		TestTrue(TEXT("duplicate sees stable held bytes"), !DuplicateBytes.IsError()
			&& DuplicateBytes.GetValue() == Fixture);
	}

	const FWoTArchiveLimits Limits;
	TWoTBootstrapResult<FWoTRetailPackage> Opened =
		FWoTRetailPackage::Open(MoveTemp(Snapshot).GetValue(), Limits);
	if (!TestFalse(TEXT("fixture parses in process"), Opened.IsError()))
	{
		if (Opened.IsError()) AddError(Opened.GetError().ToString());
		return false;
	}
	TWoTBootstrapResult<FWoTPackageEntry> Stored =
		Opened.GetValue().FindExact(TEXT("Data/Stored.txt"));
	TWoTBootstrapResult<FWoTPackageEntry> Compressed =
		Opened.GetValue().FindExact(TEXT("Data/Compressed.txt"));
	TestFalse(TEXT("stored entry exact spelling resolves"), Stored.IsError());
	TestFalse(TEXT("zlib entry exact spelling resolves"), Compressed.IsError());
	TestTrue(TEXT("lookup is case-aware"),
		Opened.GetValue().FindExact(TEXT("data/stored.txt")).IsError());
	TestTrue(TEXT("lookup requires canonical slashes"),
		Opened.GetValue().FindExact(TEXT("Data\\Stored.txt")).IsError());

	if (!Stored.IsError())
	{
		FWoTRetailPackageSpecCollectSink Sink;
		FWoTBootstrapStatus Status = Opened.GetValue().StreamExact(Stored.GetValue(), Sink);
		TestFalse(TEXT("stored entry streams and reverifies"), Status.IsError());
		static const ANSICHAR ExpectedStored[] = "stored fixture bytes\n";
		TestTrue(TEXT("stored bytes"), Sink.Collected.Num() == sizeof(ExpectedStored) - 1
			&& FMemory::Memcmp(Sink.Collected.GetData(), ExpectedStored,
				sizeof(ExpectedStored) - 1) == 0);
	}
	if (!Compressed.IsError())
	{
		FWoTRetailPackageSpecCollectSink Sink;
		FWoTBootstrapStatus Status = Opened.GetValue().StreamExact(
			Compressed.GetValue(), Sink);
		TestFalse(TEXT("zlib entry streams and reverifies"), Status.IsError());
		TestEqual(TEXT("zlib decoded byte count"), Sink.Collected.Num(), 16 * 19 + 1);
	}

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailPackageDuplicateSpec,
	"WoT.RetailReader.Package.DuplicateNormalizedPaths", WoTRetailPackageSpecFlags)

bool FWoTRetailPackageDuplicateSpec::RunTest(const FString&)
{
	FWoTRetailPackageSpecRoot Root;
	TArray<uint8> Fixture;
	if (!WoTRetailPackageSpecLoadFixture(TEXT("duplicate-folded"), Fixture)) return false;
	const FString Installer = Root.Path / TEXT("duplicate.exe");
	WoTNativeHandleTestFactory::WriteFile(Installer, Fixture);
	TWoTBootstrapResult<FWoTHeldInstallerSnapshot> Snapshot =
		WoTRetailPackageSpecSnapshot(Installer);
	if (Snapshot.IsError()) return false;
	TWoTBootstrapResult<FWoTRetailPackage> Opened = FWoTRetailPackage::Open(
		MoveTemp(Snapshot).GetValue(), FWoTArchiveLimits());
	if (TestTrue(TEXT("case/slash-normalized duplicate is rejected"), Opened.IsError()))
	{
		TestEqual(TEXT("duplicate has stable code"), Opened.GetError().Code,
			EWoTBootstrapError::DuplicateField);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailPackageMutationSpec,
	"WoT.RetailReader.Package.SameSizePausedMutation", WoTRetailPackageSpecFlags)

bool FWoTRetailPackageMutationSpec::RunTest(const FString&)
{
	TArray<uint8> Fixture;
	if (!WoTRetailPackageSpecLoadFixture(TEXT("standard"), Fixture)) return false;

	FWoTRetailPackageSpecRoot Root;
	const FString Installer = Root.Path / TEXT("paused.exe");
	WoTNativeHandleTestFactory::WriteFile(Installer, Fixture);
	TWoTBootstrapResult<FWoTHeldInstallerSnapshot> Snapshot =
		WoTRetailPackageSpecSnapshot(Installer);
	TWoTBootstrapResult<FWoTRetailPackage> Opened = FWoTRetailPackage::Open(
		MoveTemp(Snapshot).GetValue(), FWoTArchiveLimits());
	TWoTBootstrapResult<FWoTPackageEntry> Entry =
		Opened.GetValue().FindExact(TEXT("Data/Compressed.txt"));
	FEvent* Started = FPlatformProcess::GetSynchEventFromPool(true);
	FEvent* Continue = FPlatformProcess::GetSynchEventFromPool(true);
	FWoTRetailPackageSpecPause Pause{*Started, *Continue};
	Opened.GetValue().SetBeforeReverifyForTesting(
		&FWoTRetailPackageSpecPause::Invoke, &Pause);
	FWoTRetailPackageSpecCollectSink Sink;
	TFuture<FWoTBootstrapStatus> Future = Async(EAsyncExecution::Thread,
		[&Opened, &Entry, &Sink]()
		{
			return Opened.GetValue().StreamExact(Entry.GetValue(), Sink);
		});
	const bool bPaused = Started->Wait(10000);
	// Same-size by design, so the change identity is the file's NTFS
	// ChangeTime (NativeHandlesWindows.cpp). A rewrite inside the same
	// system-clock tick as the fixture write kept it unchanged and the spec
	// flaked; step past the timestamp granularity (<= 15.6 ms) first.
	FPlatformProcess::Sleep(0.05f);
	TArray<uint8> Mutated = Fixture;
	Mutated[Mutated.Num() - 1] ^= 0x5a;
	const bool bMutated = bPaused
		&& WoTNativeHandleTestFactory::WriteFile(Installer, Mutated);
	Continue->Trigger();
	const FWoTBootstrapStatus& Status = Future.Get();
	FPlatformProcess::ReturnSynchEventToPool(Started);
	FPlatformProcess::ReturnSynchEventToPool(Continue);
	TestTrue(TEXT("extraction paused before held-snapshot re-verification"), bPaused);
	TestTrue(TEXT("same-size held source mutation succeeded while paused"), bMutated);
	TestTrue(TEXT("pause callback resumed"), Pause.bContinued);
	TestTrue(TEXT("same-size paused mutation invalidates extraction"), Status.IsError());
	TestEqual(TEXT("failed re-verification publishes zero bytes"), Sink.Collected.Num(), 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailPackageEntryBufferSpec,
	"WoT.RetailReader.Package.DecompressOnceEntryBuffer", WoTRetailPackageSpecFlags)

bool FWoTRetailPackageEntryBufferSpec::RunTest(const FString&)
{
	FWoTRetailPackageSpecRoot Root;
	TWoTBootstrapResult<FWoTRetailPackage> Opened =
		WoTRetailPackageSpecOpenVariant(Root, TEXT("standard"));
	if (!TestFalse(TEXT("entry-buffer fixture opens"), Opened.IsError()))
	{
		if (Opened.IsError()) AddError(Opened.GetError().ToString());
		return false;
	}
	TWoTBootstrapResult<FWoTPackageEntry> Stored =
		Opened.GetValue().FindExact(TEXT("Data/Stored.txt"));
	TWoTBootstrapResult<FWoTPackageEntry> Compressed =
		Opened.GetValue().FindExact(TEXT("Data/Compressed.txt"));
	if (Stored.IsError() || Compressed.IsError())
	{
		AddError(TEXT("entry-buffer fixture members are missing"));
		return false;
	}

	FWoTRetailPackageSpecCollectSink FirstStored;
	FWoTRetailPackageSpecCollectSink SecondStored;
	TestFalse(TEXT("first current entry decodes"),
		Opened.GetValue().StreamExact(Stored.GetValue(), FirstStored).IsError());
	TestFalse(TEXT("same current entry republishes"),
		Opened.GetValue().StreamExact(Stored.GetValue(), SecondStored).IsError());
	TestTrue(TEXT("same-entry bytes are identical"),
		FirstStored.Collected == SecondStored.Collected);
	TestEqual(TEXT("same-entry repeat performs one native decode"),
		Opened.GetValue().EntryDecodeCountForTesting(), 1ull);

	FWoTRetailPackageSpecCollectSink FirstCompressed;
	FWoTRetailPackageSpecCollectSink SecondCompressed;
	TestFalse(TEXT("advancing entry decodes once"),
		Opened.GetValue().StreamExact(
			Compressed.GetValue(), FirstCompressed).IsError());
	TestFalse(TEXT("advanced current entry republishes"),
		Opened.GetValue().StreamExact(
			Compressed.GetValue(), SecondCompressed).IsError());
	TestTrue(TEXT("advanced same-entry bytes are identical"),
		FirstCompressed.Collected == SecondCompressed.Collected);
	TestEqual(TEXT("two distinct entries perform two native decodes"),
		Opened.GetValue().EntryDecodeCountForTesting(), 2ull);
	TestEqual(TEXT("entry buffer high-water is the largest visited entry"),
		Opened.GetValue().EntryBufferHighWaterBytes(),
		FMath::Max(Stored.GetValue().Size, Compressed.GetValue().Size));

	FWoTRetailPackageSpecCollectSink RevisitedStored;
	TestFalse(TEXT("moving backward can decode the dropped entry again"),
		Opened.GetValue().StreamExact(
			Stored.GetValue(), RevisitedStored).IsError());
	TestEqual(TEXT("only the current entry is retained"),
		Opened.GetValue().EntryDecodeCountForTesting(), 3ull);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailPackageCachedMutationSpec,
	"WoT.RetailReader.Package.CachedRepeatMutation", WoTRetailPackageSpecFlags)

bool FWoTRetailPackageCachedMutationSpec::RunTest(const FString&)
{
	TArray<uint8> Fixture;
	if (!WoTRetailPackageSpecLoadFixture(TEXT("standard"), Fixture)) return false;

	FWoTRetailPackageSpecRoot Root;
	const FString Installer = Root.Path / TEXT("cached-mutation.exe");
	WoTNativeHandleTestFactory::WriteFile(Installer, Fixture);
	TWoTBootstrapResult<FWoTHeldInstallerSnapshot> Snapshot =
		WoTRetailPackageSpecSnapshot(Installer);
	const FWoTNativeFileIdentity OriginalIdentity = Snapshot.GetValue().Identity;
	TWoTBootstrapResult<FWoTNativeFileHandle> IdentityObserver =
		Snapshot.GetValue().Handle.DuplicateForRead();
	TWoTBootstrapResult<FWoTRetailPackage> Opened = FWoTRetailPackage::Open(
		MoveTemp(Snapshot).GetValue(), FWoTArchiveLimits());
	TWoTBootstrapResult<FWoTPackageEntry> Entry =
		Opened.GetValue().FindExact(TEXT("Data/Compressed.txt"));
	FWoTRetailPackageSpecCollectSink Primed;
	if (!TestFalse(TEXT("entry cache is primed"),
		Opened.GetValue().StreamExact(Entry.GetValue(), Primed).IsError()))
	{
		return false;
	}

	FEvent* Started = FPlatformProcess::GetSynchEventFromPool(true);
	FEvent* Continue = FPlatformProcess::GetSynchEventFromPool(true);
	FWoTRetailPackageSpecPause Pause{*Started, *Continue};
	Opened.GetValue().SetBeforeReverifyForTesting(
		&FWoTRetailPackageSpecPause::Invoke, &Pause);
	FWoTRetailPackageSpecCollectSink Sink;
	TFuture<FWoTBootstrapStatus> Future = Async(EAsyncExecution::Thread,
		[&Opened, &Entry, &Sink]()
		{
			return Opened.GetValue().StreamExact(Entry.GetValue(), Sink);
		});
	const bool bPaused = Started->Wait(10000);
	Fixture[Fixture.Num() - 1] ^= 0x2d;
	const bool bIdentityChanged = bPaused && !IdentityObserver.IsError()
		&& WoTRetailPackageSpecMutateUntilIdentityChange(Installer, Fixture,
			IdentityObserver.GetValue(), OriginalIdentity);
	Continue->Trigger();
	const FWoTBootstrapStatus& Status = Future.Get();
	FPlatformProcess::ReturnSynchEventToPool(Started);
	FPlatformProcess::ReturnSynchEventToPool(Continue);
	TestTrue(TEXT("cached repeat pauses before held-snapshot re-verification"),
		bPaused);
	TestTrue(TEXT("cached-repeat fixture establishes a changed identity"),
		bIdentityChanged);
	TestTrue(TEXT("cached repeat still re-verifies the held source"), Status.IsError());
	TestEqual(TEXT("failed cached repeat publishes zero bytes"), Sink.Collected.Num(), 0);
	TestEqual(TEXT("cache hit did not perform another native decode"),
		Opened.GetValue().EntryDecodeCountForTesting(), 1ull);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailPackageIdentitySpec,
	"WoT.RetailReader.Package.HeldIdentityReplacementAndMutation", WoTRetailPackageSpecFlags)

bool FWoTRetailPackageIdentitySpec::RunTest(const FString&)
{
	TArray<uint8> Fixture;
	if (!WoTRetailPackageSpecLoadFixture(TEXT("standard"), Fixture)) return false;
	FWoTRetailPackageSpecRoot Root;

	const FString Installer = Root.Path / TEXT("replace.exe");
	const FString Replacement = Root.Path / TEXT("replacement.exe");
	WoTNativeHandleTestFactory::WriteFile(Installer, Fixture);
	TWoTBootstrapResult<FWoTHeldInstallerSnapshot> Snapshot =
		WoTRetailPackageSpecSnapshot(Installer);
	TWoTBootstrapResult<FWoTRetailPackage> Opened = FWoTRetailPackage::Open(
		MoveTemp(Snapshot).GetValue(), FWoTArchiveLimits());
	TWoTBootstrapResult<FWoTPackageEntry> Entry =
		Opened.GetValue().FindExact(TEXT("Data/Stored.txt"));
	TArray<uint8> AdjacentReplacement;
	AdjacentReplacement.Init(0xA5, Fixture.Num());
	WoTNativeHandleTestFactory::WriteFile(Replacement, AdjacentReplacement);
	const bool bReplaced = WoTNativeHandleTestFactory::ReplaceByRename(
		Installer, Replacement);
	TestTrue(TEXT("adjacent pathname is replaced"), bReplaced);
	FWoTRetailPackageSpecCollectSink Sink;
	FWoTBootstrapStatus ReplacementStatus = Opened.GetValue().StreamExact(
		Entry.GetValue(), Sink);
	TestTrue(TEXT("path replacement invalidates strict snapshot identity"),
		ReplacementStatus.IsError());

	const FString MutateInstaller = Root.Path / TEXT("mutate.exe");
	WoTNativeHandleTestFactory::WriteFile(MutateInstaller, Fixture);
	TWoTBootstrapResult<FWoTHeldInstallerSnapshot> MutateSnapshot =
		WoTRetailPackageSpecSnapshot(MutateInstaller);
	const FWoTNativeFileIdentity MutateIdentity = MutateSnapshot.GetValue().Identity;
	TWoTBootstrapResult<FWoTNativeFileHandle> MutateObserver =
		MutateSnapshot.GetValue().Handle.DuplicateForRead();
	TWoTBootstrapResult<FWoTRetailPackage> MutatePackage = FWoTRetailPackage::Open(
		MoveTemp(MutateSnapshot).GetValue(), FWoTArchiveLimits());
	TArray<uint8> Mutated = Fixture;
	Mutated[0x20] ^= 0x7f;
	TestTrue(TEXT("same-size fixture establishes a changed identity"),
		!MutateObserver.IsError()
		&& WoTRetailPackageSpecMutateUntilIdentityChange(MutateInstaller, Mutated,
			MutateObserver.GetValue(), MutateIdentity));
	TestTrue(TEXT("same-size in-place mutation fails snapshot revalidation"),
		MutatePackage.GetValue().ReverifyHeldSnapshotAfterExtraction().IsError());

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailPackageMetadataChurnSpec,
	"WoT.RetailReader.Package.MetadataChurnReproved", WoTRetailPackageSpecFlags)

bool FWoTRetailPackageMetadataChurnSpec::RunTest(const FString&)
{
	TArray<uint8> Fixture;
	if (!WoTRetailPackageSpecLoadFixture(TEXT("standard"), Fixture)) return false;
	FWoTRetailPackageSpecRoot Root;
	TArray<uint8> Mutated = Fixture;
	Mutated[0x20] ^= 0x7f;

	// macOS labels a file picked in an Open panel (com.apple.macl) moments
	// after the pick, while its package is already extracting (aleiby/wot#21).
	const FString Installer = Root.Path / TEXT("picked.exe");
	WoTNativeHandleTestFactory::WriteFile(Installer, Fixture);
	TWoTBootstrapResult<FWoTHeldInstallerSnapshot> Snapshot =
		WoTRetailPackageSpecSnapshot(Installer);
	if (!TestFalse(TEXT("picked installer snapshot is held"), Snapshot.IsError()))
	{
		return false;
	}
	const FWoTNativeFileIdentity PickedIdentity = Snapshot.GetValue().Identity;
	TWoTBootstrapResult<FWoTNativeFileHandle> Observer =
		Snapshot.GetValue().Handle.DuplicateForRead();
	TWoTBootstrapResult<FWoTRetailPackage> Opened = FWoTRetailPackage::Open(
		MoveTemp(Snapshot).GetValue(), FWoTArchiveLimits());
	if (!TestFalse(TEXT("picked installer opens"), Opened.IsError())
		|| !TestFalse(TEXT("identity observer opens"), Observer.IsError()))
	{
		return false;
	}
	TestTrue(TEXT("change clock passed the picked snapshot"),
		WoTNativeHandleTestFactory::WaitPastChangeStateTick(PickedIdentity.ChangeState));
	TestTrue(TEXT("metadata-only label is written"),
		WoTNativeHandleTestFactory::ChangeMetadataOnly(Installer));
	TWoTBootstrapResult<FWoTNativeFileIdentity> Labelled =
		Observer.GetValue().QueryCurrentIdentity();
	if (!TestTrue(TEXT("label moved only ChangeState"), !Labelled.IsError()
		&& Labelled.GetValue().DiffersOnlyInChangeState(PickedIdentity)))
	{
		return false;
	}
	TestFalse(TEXT("metadata-only churn is re-proved, not refused"),
		Opened.GetValue().ReverifyHeldSnapshotAfterExtraction().IsError());
	TWoTBootstrapResult<FWoTPackageEntry> Entry =
		Opened.GetValue().FindExact(TEXT("Data/Stored.txt"));
	FWoTRetailPackageSpecCollectSink Sink;
	TestFalse(TEXT("extraction continues after the re-proof"), Entry.IsError()
		|| Opened.GetValue().StreamExact(Entry.GetValue(), Sink).IsError());
	TestTrue(TEXT("re-proved entry streams its bytes"), Sink.Collected.Num() > 0);

	// The re-proved identity is the new baseline, and a same-length rewrite
	// after the label moves only ChangeState too: the re-hash refuses it.
	TestTrue(TEXT("same-size rewrite after the label changes identity"),
		WoTRetailPackageSpecMutateUntilIdentityChange(Installer, Mutated,
			Observer.GetValue(), Labelled.GetValue()));
	TestTrue(TEXT("rewrite after the label is still refused"),
		Opened.GetValue().ReverifyHeldSnapshotAfterExtraction().IsError());

	// A label that lands between acquisition and Open is re-proved as well...
	const FString Early = Root.Path / TEXT("labelled-before-open.exe");
	WoTNativeHandleTestFactory::WriteFile(Early, Fixture);
	TWoTBootstrapResult<FWoTHeldInstallerSnapshot> EarlySnapshot =
		WoTRetailPackageSpecSnapshot(Early);
	if (!TestFalse(TEXT("early snapshot is held"), EarlySnapshot.IsError()))
	{
		return false;
	}
	TestTrue(TEXT("change clock passed the early snapshot"),
		WoTNativeHandleTestFactory::WaitPastChangeStateTick(
			EarlySnapshot.GetValue().Identity.ChangeState));
	TestTrue(TEXT("early label is written"),
		WoTNativeHandleTestFactory::ChangeMetadataOnly(Early));
	TWoTBootstrapResult<FWoTRetailPackage> EarlyOpened = FWoTRetailPackage::Open(
		MoveTemp(EarlySnapshot).GetValue(), FWoTArchiveLimits());
	TestFalse(TEXT("label before Open is re-proved, not refused"), EarlyOpened.IsError());
	TestFalse(TEXT("re-proved Open baseline is reused"), EarlyOpened.IsError()
		|| EarlyOpened.GetValue().ReverifyHeldSnapshotAfterExtraction().IsError());

	// ...but different bytes before Open are not, even when the approved bytes
	// are put back right after Open read the rewritten ones (so the live file
	// would re-prove): the bytes Open holds are what it must prove.
	const FString Rewritten = Root.Path / TEXT("rewritten-before-open.exe");
	WoTNativeHandleTestFactory::WriteFile(Rewritten, Fixture);
	TWoTBootstrapResult<FWoTHeldInstallerSnapshot> RewrittenSnapshot =
		WoTRetailPackageSpecSnapshot(Rewritten);
	if (!TestFalse(TEXT("rewritten snapshot is held"), RewrittenSnapshot.IsError()))
	{
		return false;
	}
	const FWoTNativeFileIdentity RewrittenIdentity = RewrittenSnapshot.GetValue().Identity;
	TWoTBootstrapResult<FWoTNativeFileHandle> RewrittenObserver =
		RewrittenSnapshot.GetValue().Handle.DuplicateForRead();
	if (!TestFalse(TEXT("rewritten observer opens"), RewrittenObserver.IsError()))
	{
		return false;
	}
	TestTrue(TEXT("change clock passed the rewritten snapshot"),
		WoTNativeHandleTestFactory::WaitPastChangeStateTick(RewrittenIdentity.ChangeState));
	// The tail is entry data, so the rewritten image still parses: Open must
	// refuse it for its bytes, not because the adapter could not read it.
	TArray<uint8> TailRewrite = Fixture;
	TailRewrite.Last() ^= 0x5a;
	TestTrue(TEXT("same-size rewrite before Open lands"),
		WoTNativeHandleTestFactory::WriteFile(Rewritten, TailRewrite));
	TWoTBootstrapResult<FWoTNativeFileIdentity> RewrittenNow =
		RewrittenObserver.GetValue().QueryCurrentIdentity();
	TestTrue(TEXT("rewrite before Open moved only ChangeState"), !RewrittenNow.IsError()
		&& RewrittenNow.GetValue().DiffersOnlyInChangeState(RewrittenIdentity));
	struct FRestore
	{
		static void Invoke(void* Context)
		{
			FRestore* const Self = static_cast<FRestore*>(Context);
			Self->bRestored = WoTNativeHandleTestFactory::WriteFile(*Self->Path, *Self->Bytes);
		}
		const FString* Path = nullptr;
		const TArray<uint8>* Bytes = nullptr;
		bool bRestored = false;
	} Restore{&Rewritten, &Fixture};
	FWoTRetailPackage::SetAfterOpenReadForTesting(&FRestore::Invoke, &Restore);
	TWoTBootstrapResult<FWoTRetailPackage> RewrittenOpened = FWoTRetailPackage::Open(
		MoveTemp(RewrittenSnapshot).GetValue(), FWoTArchiveLimits());
	FWoTRetailPackage::SetAfterOpenReadForTesting(nullptr, nullptr);
	TestTrue(TEXT("approved bytes were put back after Open read the rewrite"),
		Restore.bRestored);
	TestTrue(TEXT("rewrite read before Open is refused although the file re-proves"),
		RewrittenOpened.IsError());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailPackageCachedBytesSpec,
	"WoT.RetailReader.Package.ApprovedCachedBytesDigest", WoTRetailPackageSpecFlags)

bool FWoTRetailPackageCachedBytesSpec::RunTest(const FString&)
{
	FWoTRetailPackageSpecRoot Root;
	TArray<uint8> Fixture;
	if (!WoTRetailPackageSpecLoadFixture(TEXT("standard"), Fixture)) return false;
	const FString Installer = Root.Path / TEXT("cached.exe");
	WoTNativeHandleTestFactory::WriteFile(Installer, Fixture);
	TWoTBootstrapResult<FWoTHeldInstallerSnapshot> Snapshot =
		WoTRetailPackageSpecSnapshot(Installer);
	if (Snapshot.IsError()) return false;
	const FWoTSha256 Approved = Snapshot.GetValue().Sha256;
	TWoTBootstrapResult<FWoTRetailPackage> Opened = FWoTRetailPackage::Open(
		MoveTemp(Snapshot).GetValue(), FWoTArchiveLimits());
	TestFalse(TEXT("approved fixture opens"), Opened.IsError());
	if (!Opened.IsError())
	{
		TestTrue(TEXT("adapter callback cache hashes to the approved snapshot digest"),
			Opened.GetValue().CachedInputSha256() == Approved);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailPackageTotalLimitSpec,
	"WoT.RetailReader.Package.Limits.TotalBytes", WoTRetailPackageSpecFlags)

bool FWoTRetailPackageTotalLimitSpec::RunTest(const FString&)
{
	FWoTRetailPackageSpecRoot Root;
	FWoTArchiveLimits Limits;
	Limits.MaxTotalBytes = 310; // each entry fits; their 326-byte sum does not.
	TestTrue(TEXT("cumulative decoded bytes are bounded"),
		WoTRetailPackageSpecOpenVariant(Root, TEXT("standard"), Limits).IsError());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailPackageEntryCountLimitSpec,
	"WoT.RetailReader.Package.Limits.EntryCount", WoTRetailPackageSpecFlags)

bool FWoTRetailPackageEntryCountLimitSpec::RunTest(const FString&)
{
	FWoTRetailPackageSpecRoot Root;
	FWoTArchiveLimits Limits;
	Limits.MaxEntries = 1;
	TestTrue(TEXT("caller entry-count limit is enforced"),
		WoTRetailPackageSpecOpenVariant(Root, TEXT("standard"), Limits).IsError());
	FWoTRetailPackageSpecRoot HostileRoot;
	TestTrue(TEXT("aggregate upstream entry-count cap rejects before allocation"),
		WoTRetailPackageSpecOpenVariant(HostileRoot, TEXT("entry-count-over")).IsError());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailPackagePerEntryLimitSpec,
	"WoT.RetailReader.Package.Limits.PerEntry", WoTRetailPackageSpecFlags)

bool FWoTRetailPackagePerEntryLimitSpec::RunTest(const FString&)
{
	FWoTRetailPackageSpecRoot Root;
	FWoTArchiveLimits Limits;
	Limits.MaxEntryBytes = 304;
	TestTrue(TEXT("per-entry decoded-byte limit is enforced"),
		WoTRetailPackageSpecOpenVariant(Root, TEXT("standard"), Limits).IsError());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailPackageDepthLimitSpec,
	"WoT.RetailReader.Package.Limits.PathDepth", WoTRetailPackageSpecFlags)

bool FWoTRetailPackageDepthLimitSpec::RunTest(const FString&)
{
	FWoTRetailPackageSpecRoot Root;
	FWoTArchiveLimits Limits;
	Limits.MaxDepth = 1;
	TestTrue(TEXT("logical path depth limit is enforced"),
		WoTRetailPackageSpecOpenVariant(Root, TEXT("standard"), Limits).IsError());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailPackageAccumulationOverflowSpec,
	"WoT.RetailReader.Package.Limits.AccumulationOverflow", WoTRetailPackageSpecFlags)

bool FWoTRetailPackageAccumulationOverflowSpec::RunTest(const FString&)
{
	FWoTRetailPackageSpecRoot Root;
	TestTrue(TEXT("uint64 accumulation-wrap declarations are refused"),
		WoTRetailPackageSpecOpenVariant(Root, TEXT("accumulation-overflow")).IsError());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailPackageTransactionalInvalidationSpec,
	"WoT.RetailReader.Package.TransactionalInvalidation", WoTRetailPackageSpecFlags)

bool FWoTRetailPackageTransactionalInvalidationSpec::RunTest(const FString&)
{
	FWoTRetailPackageSpecRoot Root;
	TArray<uint8> Fixture;
	if (!WoTRetailPackageSpecLoadFixture(TEXT("standard"), Fixture)) return false;
	const FString Installer = Root.Path / TEXT("transactional.exe");
	WoTNativeHandleTestFactory::WriteFile(Installer, Fixture);
	TWoTBootstrapResult<FWoTHeldInstallerSnapshot> Snapshot =
		WoTRetailPackageSpecSnapshot(Installer);
	TWoTBootstrapResult<FWoTRetailPackage> Opened = FWoTRetailPackage::Open(
		MoveTemp(Snapshot).GetValue(), FWoTArchiveLimits());
	TWoTBootstrapResult<FWoTPackageEntry> Entry =
		Opened.GetValue().FindExact(TEXT("Data/Stored.txt"));
	TWoTBootstrapResult<FWoTNativeFileHandle> Observer =
		OpenNativeFileReadNoFollow(Installer);
	if (!TestFalse(TEXT("transactional mutation observer opens"), Observer.IsError()))
		return false;
	const FWoTNativeFileIdentity Original = Observer.GetValue().GetIdentity();
	Fixture[0x20] ^= 0x33;
	TestTrue(TEXT("same-size pre-publication mutation is staged"),
		WoTRetailPackageSpecMutateUntilIdentityChange(
			Installer, Fixture, Observer.GetValue(), Original));
	FWoTRetailPackageSpecCollectSink Sink;
	FWoTBootstrapStatus Status = Opened.GetValue().StreamExact(Entry.GetValue(), Sink);
	TestTrue(TEXT("held mutation invalidates extraction"), Status.IsError());
	TestEqual(TEXT("invalidated extraction delivers no sink bytes"), Sink.Collected.Num(), 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailPackageLinkRefusalSpec,
	"WoT.RetailReader.Package.LinkAndReparseRefusal", WoTRetailPackageSpecFlags)

bool FWoTRetailPackageLinkRefusalSpec::RunTest(const FString&)
{
	FWoTRetailPackageSpecRoot Root;
	TArray<uint8> Fixture;
	if (!WoTRetailPackageSpecLoadFixture(TEXT("standard"), Fixture)) return false;
	const FString Installer = Root.Path / TEXT("original.exe");
	WoTNativeHandleTestFactory::WriteFile(Installer, Fixture);

	const FString HardLink = Root.Path / TEXT("hardlink.exe");
	const bool bHardLinkCreated =
		WoTNativeHandleTestFactory::CreateHardLink(HardLink, Installer);
	TestTrue(TEXT("hard-link fixture creation is required for this security spec"),
		bHardLinkCreated);
	if (bHardLinkCreated)
	{
		TWoTBootstrapResult<FWoTHeldInstallerSnapshot> Linked =
			WoTRetailPackageSpecSnapshot(Installer);
		TestTrue(TEXT("multiply-linked held installer is refused"), !Linked.IsError()
			&& FWoTRetailPackage::Open(MoveTemp(Linked).GetValue(),
				FWoTArchiveLimits()).IsError());
	}

	const FString Symlink = Root.Path / TEXT("symlink.exe");
	const bool bSymlinkCreated =
		WoTNativeHandleTestFactory::CreateSymbolicLink(Symlink, Installer);
	TestTrue(TEXT("link/reparse fixture creation is required for this security spec"),
		bSymlinkCreated);
	if (bSymlinkCreated)
	{
		TestTrue(TEXT("Task 5 no-follow factory refuses the link/reparse fixture"),
			OpenNativeFileReadNoFollow(Symlink).IsError());
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailPackageDestinationRootSpec,
	"WoT.RetailReader.Package.ProductionDestinationRoot", WoTRetailPackageSpecFlags)

bool FWoTRetailPackageDestinationRootSpec::RunTest(const FString&)
{
	const TCHAR* const Refused[] = {
		TEXT("bare-app"), TEXT("nested-app"), TEXT("path-over-limit"),
	};
	for (const TCHAR* Variant : Refused)
	{
		FWoTRetailPackageSpecRoot Root;
		TestTrue(FString::Printf(TEXT("%s destination is refused"), Variant),
			WoTRetailPackageSpecOpenVariant(Root, Variant).IsError());
	}
	const TCHAR* const Ignored[] = {TEXT("constant-tmp"), TEXT("constant-sys")};
	for (const TCHAR* Variant : Ignored)
	{
		FWoTRetailPackageSpecRoot Root;
		TWoTBootstrapResult<FWoTRetailPackage> Opened =
			WoTRetailPackageSpecOpenVariant(Root, Variant);
		TestFalse(FString::Printf(TEXT("%s non-app member is ignored"), Variant),
			Opened.IsError());
		if (Opened.IsError())
		{
			AddError(FString::Printf(TEXT("%s open error: %s"), Variant,
				*Opened.GetError().ToString()));
		}
		if (!Opened.IsError())
		{
			TestTrue(FString::Printf(TEXT("%s is not exposed"), Variant),
				Opened.GetValue().FindExact(TEXT("Data/Stored.txt")).IsError());
			TestFalse(FString::Printf(TEXT("%s app member remains"), Variant),
				Opened.GetValue().FindExact(TEXT("Data/Compressed.txt")).IsError());
		}
	}
	{
		FWoTRetailPackageSpecRoot Root;
		TWoTBootstrapResult<FWoTRetailPackage> Opened =
			WoTRetailPackageSpecOpenVariant(Root, TEXT("galaxy-multipart"));
		TestFalse(TEXT("GOG Galaxy multipart fixture opens"), Opened.IsError());
		if (Opened.IsError())
		{
			AddError(TEXT("galaxy-multipart open error: ")
				+ Opened.GetError().ToString());
		}
		if (!Opened.IsError())
		{
			TWoTBootstrapResult<FWoTPackageEntry> Entry =
				Opened.GetValue().FindExact(TEXT("Data/Galaxy.txt"));
			TestFalse(TEXT("recovered GOG destination is exposed"), Entry.IsError());
			if (!Entry.IsError())
			{
				FWoTRetailPackageSpecCollectSink Sink;
				FWoTBootstrapStatus Extracted =
					Opened.GetValue().StreamExact(Entry.GetValue(), Sink);
				TestFalse(TEXT("GOG multipart stream verifies"), Extracted.IsError());
				static const ANSICHAR Expected[] =
					"galaxy part one\ngalaxy part two\n";
				TArray<uint8> ExpectedBytes;
				ExpectedBytes.Append(
					reinterpret_cast<const uint8*>(Expected), sizeof(Expected) - 1);
				TestTrue(TEXT("GOG multipart bytes concatenate in order"),
					Sink.Collected == ExpectedBytes);
			}
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailPackageLzmaDictionarySpec,
	"WoT.RetailReader.Package.LzmaDictionaryLimit", WoTRetailPackageSpecFlags)

bool FWoTRetailPackageLzmaDictionarySpec::RunTest(const FString&)
{
	const TCHAR* const Accepted[] = {TEXT("lzma1-boundary"), TEXT("lzma2-boundary")};
	for (const TCHAR* Variant : Accepted)
	{
		FWoTRetailPackageSpecRoot Root;
		TWoTBootstrapResult<FWoTRetailPackage> Opened =
			WoTRetailPackageSpecOpenVariant(Root, Variant);
		TestFalse(FString::Printf(TEXT("%s opens"), Variant), Opened.IsError());
		if (Opened.IsError())
		{
			AddError(FString::Printf(TEXT("%s open error: %s"), Variant,
				*Opened.GetError().ToString()));
		}
		if (!Opened.IsError())
		{
			TWoTBootstrapResult<FWoTPackageEntry> Entry =
				Opened.GetValue().FindExact(TEXT("Data/Compressed.txt"));
			FWoTRetailPackageSpecCollectSink Sink;
			TestFalse(FString::Printf(TEXT("%s extracts at 64 MiB"), Variant),
				Opened.GetValue().StreamExact(Entry.GetValue(), Sink).IsError());
		}
	}
	const TCHAR* const Refused[] = {TEXT("lzma1-over"), TEXT("lzma2-over")};
	for (const TCHAR* Variant : Refused)
	{
		FWoTRetailPackageSpecRoot Root;
		TWoTBootstrapResult<FWoTRetailPackage> Opened =
			WoTRetailPackageSpecOpenVariant(Root, Variant);
		if (!TestFalse(FString::Printf(TEXT("%s metadata opens"), Variant), Opened.IsError()))
		{
			continue;
		}
		TWoTBootstrapResult<FWoTPackageEntry> Entry =
			Opened.GetValue().FindExact(TEXT("Data/Compressed.txt"));
		FWoTRetailPackageSpecCollectSink Sink;
		TestTrue(FString::Printf(TEXT("%s is refused before decoder allocation"), Variant),
			Opened.GetValue().StreamExact(Entry.GetValue(), Sink).IsError());
		TestEqual(TEXT("refused decoder publishes no bytes"), Sink.Collected.Num(), 0);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailPackagePortableNamesSpec,
	"WoT.RetailReader.Package.PortableNamesAndCanonicalCollisions", WoTRetailPackageSpecFlags)

bool FWoTRetailPackagePortableNamesSpec::RunTest(const FString&)
{
	const TCHAR* const Unsafe[] = {
		TEXT("portable-angle"), TEXT("portable-angle-close"), TEXT("portable-colon"),
		TEXT("portable-quote"), TEXT("portable-pipe"), TEXT("portable-question"),
		TEXT("portable-star"), TEXT("portable-control"), TEXT("portable-trailing-dot"),
		TEXT("portable-trailing-space"), TEXT("portable-reserved-exact"),
		TEXT("portable-reserved-folded"),
	};
	for (const TCHAR* Variant : Unsafe)
	{
		FWoTRetailPackageSpecRoot Root;
		TestTrue(FString::Printf(TEXT("%s portable-name policy"), Variant),
			WoTRetailPackageSpecOpenVariant(Root, Variant).IsError());
	}
	for (const TCHAR* Variant : {TEXT("duplicate-exact"), TEXT("duplicate-folded")})
	{
		FWoTRetailPackageSpecRoot Root;
		TWoTBootstrapResult<FWoTRetailPackage> Opened =
			WoTRetailPackageSpecOpenVariant(Root, Variant);
		TestTrue(FString::Printf(TEXT("%s canonical-domain collision"), Variant),
			Opened.IsError() && Opened.GetError().Code == EWoTBootstrapError::DuplicateField);
	}
	return !HasAnyErrors();
}

#endif  // WITH_AUTOMATION_TESTS
