#include "../Updater/Mac/WoTMacUpdateSwap.h"

#if PLATFORM_MAC && WITH_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "WoTBootstrapCore/NativeHandles.h"
#include "WoTUpdaterCore/UpdaterPaths.h"

namespace
{
constexpr EAutomationTestFlags WoTMacUpdateSwapSpecFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;
constexpr uint64 WoTMacUpdateSwapSpecSequence = 42;

class FWoTMacUpdateSwapSpecFixture final
{
public:
	FWoTMacUpdateSwapSpecFixture()
	{
		ScratchRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectSavedDir(), TEXT("Mac Update Swap Tests"),
			FGuid::NewGuid().ToString(EGuidFormats::Digits)));
		UserStateRoot = FPaths::Combine(ScratchRoot, TEXT("Data"));
		InstallRoot = FPaths::Combine(ScratchRoot, TEXT("Install"));
		IFileManager::Get().MakeDirectory(*UserStateRoot, true);
		IFileManager::Get().MakeDirectory(*InstallRoot, true);
		auto Resolved = ResolveUpdaterStateRoot(
			UserStateRoot, InstallRoot, TEXT("WoT.app"));
		check(!Resolved.IsError());
		Root = MoveTemp(Resolved).GetValue();
		IFileManager::Get().MakeDirectory(*Root.UpdateDir, true);
		IFileManager::Get().MakeDirectory(*Root.StagingDir, true);
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Root.PreviousTreeDir), true);
	}

	~FWoTMacUpdateSwapSpecFixture()
	{
		IFileManager::Get().DeleteDirectory(*ScratchRoot, false, true);
	}

	void WriteLive(FStringView Identity) const
	{
		WriteTree(LivePath(), Identity);
	}

	void WriteStaged(FStringView Identity) const
	{
		WriteTree(Root.StagedTreeDir, Identity);
	}

	void WritePrevious(FStringView Identity) const
	{
		WriteTree(Root.PreviousTreeDir, Identity);
	}

	void WriteReady(uint64 Sequence = WoTMacUpdateSwapSpecSequence) const
	{
		check(!WriteWoTUpdateMarker(Root, TEXT("ready"), {Sequence}).IsError());
	}

	void WriteTrial(uint64 Sequence, EWoTUpdateTrialState State) const
	{
		check(!WriteWoTUpdateMarker(Root, TEXT("trial"), {Sequence, State}).IsError());
	}

	void WriteEmptyTrial() const
	{
		const TArray<uint8> Empty;
		check(FFileHelper::SaveArrayToFile(Empty, *Root.TrialPath));
	}

	FString LivePath() const
	{
		return FPaths::Combine(Root.InstallRoot, Root.LiveLeaf);
	}

	FString ReadIdentity(const FString& Tree) const
	{
		FString Identity;
		FFileHelper::LoadFileToString(Identity,
			*FPaths::Combine(Tree, TEXT("identity.txt")));
		return Identity;
	}

	FWoTMacUpdateSwapEffects Effects(bool bExclusive = true,
		bool bUnsupportedExchange = false)
	{
		FWoTMacUpdateSwapEffects Result;
		Result.TryExclusive = [bExclusive]() -> TOptional<bool>
		{
			return bExclusive;
		};
		Result.Exchange = [bUnsupportedExchange](
			const FWoTNativeDirectoryHandle& DirectoryA, FStringView LeafA,
			const FWoTNativeDirectoryHandle& DirectoryB, FStringView LeafB)
			-> TWoTBootstrapResult<bool>
		{
			if (bUnsupportedExchange)
			{
				return TWoTBootstrapResult<bool>::Error(MakeWoTBootstrapError(
					EWoTBootstrapError::UnsupportedProtocol,
					TEXT("the spec forces the two-rename fallback")));
			}
			return ExchangeNativeEntriesAtNoFollow(
				DirectoryA, LeafA, DirectoryB, LeafB);
		};
		Result.WriteTrial = [](const FWoTUpdaterStateRoot& Root, uint64 Sequence)
		{
			return WriteWoTUpdateMarker(Root, TEXT("trial"),
				{Sequence, EWoTUpdateTrialState::Pending});
		};
		Result.MoveOldToPrevious = [](
			const FWoTNativeDirectoryHandle& Staged, FStringView StagedLeaf,
			const FWoTNativeDirectoryHandle& Previous, FStringView PreviousLeaf)
		{
			return RenameNativeEntryAtNoFollow(
				Staged, StagedLeaf, Previous, PreviousLeaf, false);
		};
		Result.ShowRollbackAlert = [this](uint64 Sequence)
		{
			AlertedSequence = Sequence;
		};
		Result.RegisterLiveBundle = [this](const FString& Path)
		{
			RegisteredPath = Path;
		};
		return Result;
	}

	TOptional<FWoTUpdateMarker> ReadMarker(FStringView Leaf, bool bTrial = false) const
	{
		auto Read = ReadWoTUpdateMarker(Root, Leaf, bTrial);
		check(!Read.IsError());
		return MoveTemp(Read).GetValue();
	}

	FWoTUpdaterStateRoot Root;
	TOptional<uint64> AlertedSequence;
	FString RegisteredPath;

private:
	static void WriteTree(const FString& Tree, FStringView Identity)
	{
		IFileManager::Get().DeleteDirectory(*Tree, false, true);
		check(IFileManager::Get().MakeDirectory(*Tree, true));
		check(FFileHelper::SaveStringToFile(
			FString(Identity), *FPaths::Combine(Tree, TEXT("identity.txt"))));
	}

	FString ScratchRoot;
	FString UserStateRoot;
	FString InstallRoot;
};

bool WoTMacUpdateSwapSpecAssertApplied(FAutomationTestBase& Test,
	const FWoTMacUpdateSwapSpecFixture& Fixture)
{
	const TOptional<FWoTUpdateMarker> Trial = Fixture.ReadMarker(TEXT("trial"), true);
	const TOptional<FWoTUpdateMarker> Ready = Fixture.ReadMarker(TEXT("ready"));
	return Test.TestEqual(TEXT("the new tree is live"),
		Fixture.ReadIdentity(Fixture.LivePath()), FString(TEXT("new")))
		&& Test.TestEqual(TEXT("the old tree is retained"),
			Fixture.ReadIdentity(Fixture.Root.PreviousTreeDir), FString(TEXT("old")))
		&& Test.TestTrue(TEXT("trial is pending for the staged sequence"),
			Trial.IsSet()
			&& Trial->Sequence == WoTMacUpdateSwapSpecSequence
			&& Trial->State == EWoTUpdateTrialState::Pending)
		&& Test.TestFalse(TEXT("ready is removed"), Ready.IsSet())
		&& Test.TestEqual(TEXT("the live bundle is registered"),
			Fixture.RegisteredPath, Fixture.LivePath());
}

FWoTBootstrapStatus WoTMacUpdateSwapSpecInjectedFailure(FStringView Detail)
{
	return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
		EWoTBootstrapError::Io, FString(Detail)));
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMacQuitSwapAppliesSpec,
	"WoT.Updater.MacQuitSwap.QuitSwapApplies", WoTMacUpdateSwapSpecFlags)
bool FWoTMacQuitSwapAppliesSpec::RunTest(const FString&)
{
	FWoTMacUpdateSwapSpecFixture Fixture;
	Fixture.WriteLive(TEXT("old"));
	Fixture.WriteStaged(TEXT("new"));
	Fixture.WriteReady();
	auto Effects = Fixture.Effects();
	TestTrue(TEXT("the swap is applied"),
		WoTMacApplyStagedUpdateAtQuit(Fixture.Root,
			WoTMacUpdateSwapSpecSequence - 1, Effects) == EWoTMacQuitAction::Swap);
	WoTMacUpdateSwapSpecAssertApplied(*this, Fixture);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMacQuitSwapStaleReadySpec,
	"WoT.Updater.MacQuitSwap.QuitSwapStaleReadyIsDropped", WoTMacUpdateSwapSpecFlags)
bool FWoTMacQuitSwapStaleReadySpec::RunTest(const FString&)
{
	FWoTMacUpdateSwapSpecFixture Fixture;
	Fixture.WriteLive(TEXT("new"));
	Fixture.WriteStaged(TEXT("old"));
	Fixture.WriteReady();
	auto Effects = Fixture.Effects();
	TestTrue(TEXT("stale ready is dropped"),
		WoTMacApplyStagedUpdateAtQuit(Fixture.Root,
			WoTMacUpdateSwapSpecSequence, Effects) == EWoTMacQuitAction::DropStaleReady);
	TestEqual(TEXT("the live tree does not swap back"),
		Fixture.ReadIdentity(Fixture.LivePath()), FString(TEXT("new")));
	TestFalse(TEXT("ready is removed"), Fixture.ReadMarker(TEXT("ready")).IsSet());
	TestFalse(TEXT("the stale staged tree is removed"),
		IFileManager::Get().DirectoryExists(*Fixture.Root.StagedTreeDir));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMacQuitSwapOtherCopySpec,
	"WoT.Updater.MacQuitSwap.QuitSwapOtherCopySkips", WoTMacUpdateSwapSpecFlags)
bool FWoTMacQuitSwapOtherCopySpec::RunTest(const FString&)
{
	FWoTMacUpdateSwapSpecFixture Fixture;
	Fixture.WriteLive(TEXT("old"));
	Fixture.WriteStaged(TEXT("new"));
	Fixture.WriteReady();
	auto Effects = Fixture.Effects(false);
	TestTrue(TEXT("busy gate skips the swap"),
		WoTMacApplyStagedUpdateAtQuit(Fixture.Root,
			WoTMacUpdateSwapSpecSequence - 1, Effects) == EWoTMacQuitAction::None);
	TestEqual(TEXT("live is unchanged"), Fixture.ReadIdentity(Fixture.LivePath()),
		FString(TEXT("old")));
	TestTrue(TEXT("ready remains"), Fixture.ReadMarker(TEXT("ready")).IsSet());
	TestTrue(TEXT("staged remains"),
		IFileManager::Get().DirectoryExists(*Fixture.Root.StagedTreeDir));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMacQuitStaleCleanupOtherCopySpec,
	"WoT.Updater.MacQuitSwap.QuitStaleCleanupOtherCopyLeavesArtifacts",
	WoTMacUpdateSwapSpecFlags)
bool FWoTMacQuitStaleCleanupOtherCopySpec::RunTest(const FString&)
{
	FWoTMacUpdateSwapSpecFixture Fixture;
	Fixture.WriteLive(TEXT("new"));
	Fixture.WriteStaged(TEXT("old"));
	Fixture.WriteReady();
	auto Effects = Fixture.Effects(false);
	TestTrue(TEXT("busy gate skips stale cleanup"),
		WoTMacApplyStagedUpdateAtQuit(Fixture.Root,
			WoTMacUpdateSwapSpecSequence, Effects) == EWoTMacQuitAction::None);
	TestEqual(TEXT("live is unchanged"), Fixture.ReadIdentity(Fixture.LivePath()),
		FString(TEXT("new")));
	TestTrue(TEXT("ready remains"), Fixture.ReadMarker(TEXT("ready")).IsSet());
	TestTrue(TEXT("staged remains"),
		IFileManager::Get().DirectoryExists(*Fixture.Root.StagedTreeDir));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMacQuitSwapFallbackSpec,
	"WoT.Updater.MacQuitSwap.QuitSwapFallbackTwoRenames", WoTMacUpdateSwapSpecFlags)
bool FWoTMacQuitSwapFallbackSpec::RunTest(const FString&)
{
	FWoTMacUpdateSwapSpecFixture Fixture;
	Fixture.WriteLive(TEXT("old"));
	Fixture.WriteStaged(TEXT("new"));
	Fixture.WriteReady();
	auto Effects = Fixture.Effects(true, true);
	TestTrue(TEXT("the fallback swap is applied"),
		WoTMacApplyStagedUpdateAtQuit(Fixture.Root,
			WoTMacUpdateSwapSpecSequence - 1, Effects) == EWoTMacQuitAction::Swap);
	WoTMacUpdateSwapSpecAssertApplied(*this, Fixture);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMacQuitSwapReplacesPrevSpec,
	"WoT.Updater.MacQuitSwap.QuitSwapReplacesOlderPrev", WoTMacUpdateSwapSpecFlags)
bool FWoTMacQuitSwapReplacesPrevSpec::RunTest(const FString&)
{
	FWoTMacUpdateSwapSpecFixture Fixture;
	Fixture.WriteLive(TEXT("old"));
	Fixture.WriteStaged(TEXT("new"));
	Fixture.WritePrevious(TEXT("older"));
	Fixture.WriteReady();
	auto Effects = Fixture.Effects();
	TestTrue(TEXT("the swap replaces the previous tree"),
		WoTMacApplyStagedUpdateAtQuit(Fixture.Root,
			WoTMacUpdateSwapSpecSequence - 1, Effects) == EWoTMacQuitAction::Swap);
	WoTMacUpdateSwapSpecAssertApplied(*this, Fixture);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMacQuitSwapTrialWriteFailureSpec,
	"WoT.Updater.MacQuitSwap.QuitSwapTrialWriteFailureRestoresOld",
	WoTMacUpdateSwapSpecFlags)
bool FWoTMacQuitSwapTrialWriteFailureSpec::RunTest(const FString&)
{
	FWoTMacUpdateSwapSpecFixture Fixture;
	Fixture.WriteLive(TEXT("old"));
	Fixture.WriteStaged(TEXT("new"));
	Fixture.WriteReady();
	auto Effects = Fixture.Effects();
	Effects.WriteTrial = [](const FWoTUpdaterStateRoot& Root, uint64)
	{
		(void)FFileHelper::SaveStringToFile(TEXT("partial"), *Root.TrialPath);
		return WoTMacUpdateSwapSpecInjectedFailure(TEXT("injected trial write failure"));
	};
	TestTrue(TEXT("failed trial write reports no applied swap"),
		WoTMacApplyStagedUpdateAtQuit(Fixture.Root,
			WoTMacUpdateSwapSpecSequence - 1, Effects) == EWoTMacQuitAction::None);
	TestEqual(TEXT("the old tree is restored live"),
		Fixture.ReadIdentity(Fixture.LivePath()), FString(TEXT("old")));
	TestEqual(TEXT("the new tree is restored staged"),
		Fixture.ReadIdentity(Fixture.Root.StagedTreeDir), FString(TEXT("new")));
	TestFalse(TEXT("no trial remains"),
		IFileManager::Get().FileExists(*Fixture.Root.TrialPath));
	TestTrue(TEXT("ready remains for retry"),
		Fixture.ReadMarker(TEXT("ready")).IsSet());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMacQuitSwapPrevMoveFailureSpec,
	"WoT.Updater.MacQuitSwap.QuitSwapPrevMoveFailureRestoresOld",
	WoTMacUpdateSwapSpecFlags)
bool FWoTMacQuitSwapPrevMoveFailureSpec::RunTest(const FString&)
{
	FWoTMacUpdateSwapSpecFixture Fixture;
	Fixture.WriteLive(TEXT("old"));
	Fixture.WriteStaged(TEXT("new"));
	Fixture.WriteReady();
	auto Effects = Fixture.Effects();
	Effects.MoveOldToPrevious = [](
		const FWoTNativeDirectoryHandle&, FStringView,
		const FWoTNativeDirectoryHandle&, FStringView)
	{
		return WoTMacUpdateSwapSpecInjectedFailure(TEXT("injected prev move failure"));
	};
	TestTrue(TEXT("failed prev move reports no applied swap"),
		WoTMacApplyStagedUpdateAtQuit(Fixture.Root,
			WoTMacUpdateSwapSpecSequence - 1, Effects) == EWoTMacQuitAction::None);
	TestEqual(TEXT("the old tree is restored live"),
		Fixture.ReadIdentity(Fixture.LivePath()), FString(TEXT("old")));
	TestEqual(TEXT("the new tree is restored staged"),
		Fixture.ReadIdentity(Fixture.Root.StagedTreeDir), FString(TEXT("new")));
	TestFalse(TEXT("no trial remains"),
		IFileManager::Get().FileExists(*Fixture.Root.TrialPath));
	TestTrue(TEXT("ready remains for retry"),
		Fixture.ReadMarker(TEXT("ready")).IsSet());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMacStartupRollbackSpec,
	"WoT.Updater.MacQuitSwap.StartupStartedRollsBack", WoTMacUpdateSwapSpecFlags)
bool FWoTMacStartupRollbackSpec::RunTest(const FString&)
{
	FWoTMacUpdateSwapSpecFixture Fixture;
	Fixture.WriteLive(TEXT("new"));
	Fixture.WritePrevious(TEXT("old"));
	Fixture.WriteTrial(WoTMacUpdateSwapSpecSequence, EWoTUpdateTrialState::Started);
	auto Effects = Fixture.Effects();
	TestFalse(TEXT("completed rollback asks the caller to exit"),
		WoTMacCheckUpdateTrialAtStartup(Fixture.Root,
			WoTMacUpdateSwapSpecSequence, Effects));
	TestEqual(TEXT("the old tree is live"), Fixture.ReadIdentity(Fixture.LivePath()),
		FString(TEXT("old")));
	TestFalse(TEXT("the failed tree is removed"),
		IFileManager::Get().DirectoryExists(*Fixture.Root.PreviousTreeDir));
	const TOptional<FWoTUpdateMarker> RolledBack =
		Fixture.ReadMarker(TEXT("rolled-back"));
	TestTrue(TEXT("rollback is recorded"), RolledBack.IsSet()
		&& RolledBack->Sequence == WoTMacUpdateSwapSpecSequence);
	TestFalse(TEXT("trial is removed"), Fixture.ReadMarker(TEXT("trial"), true).IsSet());
	TestTrue(TEXT("the rollback alert records the failed sequence"),
		Fixture.AlertedSequence.IsSet()
		&& Fixture.AlertedSequence.GetValue() == WoTMacUpdateSwapSpecSequence);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMacStartupOtherCopySpec,
	"WoT.Updater.MacQuitSwap.StartupStartedOtherCopyContinues", WoTMacUpdateSwapSpecFlags)
bool FWoTMacStartupOtherCopySpec::RunTest(const FString&)
{
	FWoTMacUpdateSwapSpecFixture Fixture;
	Fixture.WriteLive(TEXT("new"));
	Fixture.WritePrevious(TEXT("old"));
	Fixture.WriteTrial(WoTMacUpdateSwapSpecSequence, EWoTUpdateTrialState::Started);
	auto Effects = Fixture.Effects(false);
	TestTrue(TEXT("busy rollback continues startup"),
		WoTMacCheckUpdateTrialAtStartup(Fixture.Root,
			WoTMacUpdateSwapSpecSequence, Effects));
	TestEqual(TEXT("live is unchanged"), Fixture.ReadIdentity(Fixture.LivePath()),
		FString(TEXT("new")));
	TestTrue(TEXT("trial remains"), Fixture.ReadMarker(TEXT("trial"), true).IsSet());
	TestTrue(TEXT("previous remains"),
		IFileManager::Get().DirectoryExists(*Fixture.Root.PreviousTreeDir));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMacStartupStaleTrialSpec,
	"WoT.Updater.MacQuitSwap.StartupStaleTrialDropped", WoTMacUpdateSwapSpecFlags)
bool FWoTMacStartupStaleTrialSpec::RunTest(const FString&)
{
	FWoTMacUpdateSwapSpecFixture Fixture;
	Fixture.WriteLive(TEXT("current"));
	Fixture.WritePrevious(TEXT("old"));
	Fixture.WriteTrial(WoTMacUpdateSwapSpecSequence - 1,
		EWoTUpdateTrialState::Started);
	auto Effects = Fixture.Effects();
	TestTrue(TEXT("stale trial continues startup"),
		WoTMacCheckUpdateTrialAtStartup(Fixture.Root,
			WoTMacUpdateSwapSpecSequence, Effects));
	TestFalse(TEXT("stale trial is removed"),
		Fixture.ReadMarker(TEXT("trial"), true).IsSet());
	TestEqual(TEXT("live is unchanged"), Fixture.ReadIdentity(Fixture.LivePath()),
		FString(TEXT("current")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMacStartupPendingSpec,
	"WoT.Updater.MacQuitSwap.StartupPendingContinues", WoTMacUpdateSwapSpecFlags)
bool FWoTMacStartupPendingSpec::RunTest(const FString&)
{
	FWoTMacUpdateSwapSpecFixture Fixture;
	Fixture.WriteLive(TEXT("new"));
	Fixture.WritePrevious(TEXT("old"));
	Fixture.WriteTrial(WoTMacUpdateSwapSpecSequence, EWoTUpdateTrialState::Pending);
	auto Effects = Fixture.Effects();
	TestTrue(TEXT("pending trial continues startup"),
		WoTMacCheckUpdateTrialAtStartup(Fixture.Root,
			WoTMacUpdateSwapSpecSequence, Effects));
	const TOptional<FWoTUpdateMarker> Trial = Fixture.ReadMarker(TEXT("trial"), true);
	TestTrue(TEXT("pending trial is unchanged"), Trial.IsSet()
		&& Trial->Sequence == WoTMacUpdateSwapSpecSequence
		&& Trial->State == EWoTUpdateTrialState::Pending);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMacStartupMalformedTrialSpec,
	"WoT.Updater.MacQuitSwap.StartupMalformedTrialDropped",
	WoTMacUpdateSwapSpecFlags)
bool FWoTMacStartupMalformedTrialSpec::RunTest(const FString&)
{
	FWoTMacUpdateSwapSpecFixture Fixture;
	Fixture.WriteLive(TEXT("current"));
	Fixture.WriteEmptyTrial();
	auto Effects = Fixture.Effects();
	TestTrue(TEXT("malformed trial continues startup"),
		WoTMacCheckUpdateTrialAtStartup(Fixture.Root,
			WoTMacUpdateSwapSpecSequence, Effects));
	TestFalse(TEXT("malformed trial is removed"),
		IFileManager::Get().FileExists(*Fixture.Root.TrialPath));
	TestEqual(TEXT("live is unchanged"), Fixture.ReadIdentity(Fixture.LivePath()),
		FString(TEXT("current")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMacRollbackFallbackSpec,
	"WoT.Updater.MacQuitSwap.RollbackFallbackTwoRenames", WoTMacUpdateSwapSpecFlags)
bool FWoTMacRollbackFallbackSpec::RunTest(const FString&)
{
	FWoTMacUpdateSwapSpecFixture Fixture;
	Fixture.WriteLive(TEXT("new"));
	Fixture.WritePrevious(TEXT("old"));
	Fixture.WriteTrial(WoTMacUpdateSwapSpecSequence, EWoTUpdateTrialState::Started);
	auto Effects = Fixture.Effects(true, true);
	TestFalse(TEXT("fallback rollback asks the caller to exit"),
		WoTMacCheckUpdateTrialAtStartup(Fixture.Root,
			WoTMacUpdateSwapSpecSequence, Effects));
	TestEqual(TEXT("the fallback restores the old tree"),
		Fixture.ReadIdentity(Fixture.LivePath()), FString(TEXT("old")));
	TestTrue(TEXT("fallback rollback is recorded"),
		Fixture.ReadMarker(TEXT("rolled-back")).IsSet());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMacInterruptedQuitSpec,
	"WoT.Updater.MacQuitSwap.InterruptedQuitDoesNotSwapBack", WoTMacUpdateSwapSpecFlags)
bool FWoTMacInterruptedQuitSpec::RunTest(const FString&)
{
	FWoTMacUpdateSwapSpecFixture Fixture;
	// Exact post-exchange/pre-marker interruption: new is live, old is staged,
	// and ready still names the sequence now running.
	Fixture.WriteLive(TEXT("new"));
	Fixture.WriteStaged(TEXT("old"));
	Fixture.WriteReady();
	auto Effects = Fixture.Effects();
	TestTrue(TEXT("startup without trial continues"),
		WoTMacCheckUpdateTrialAtStartup(Fixture.Root,
			WoTMacUpdateSwapSpecSequence, Effects));
	TestTrue(TEXT("quit drops the stale ready instead of swapping back"),
		WoTMacApplyStagedUpdateAtQuit(Fixture.Root,
			WoTMacUpdateSwapSpecSequence, Effects) == EWoTMacQuitAction::DropStaleReady);
	TestEqual(TEXT("the new tree stays live"), Fixture.ReadIdentity(Fixture.LivePath()),
		FString(TEXT("new")));
	TestFalse(TEXT("ready is removed"), Fixture.ReadMarker(TEXT("ready")).IsSet());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMacUpdateDecisionTablesSpec,
	"WoT.Updater.MacQuitSwap.DecisionTables", WoTMacUpdateSwapSpecFlags)
bool FWoTMacUpdateDecisionTablesSpec::RunTest(const FString&)
{
	const FWoTUpdateMarker Pending{42, EWoTUpdateTrialState::Pending};
	const FWoTUpdateMarker Started{42, EWoTUpdateTrialState::Started};
	TestTrue(TEXT("no trial continues"), WoTDecideMacTrialAction({}, 42, true)
		== EWoTMacTrialAction::Continue);
	TestTrue(TEXT("pending continues"), WoTDecideMacTrialAction(Pending, 42, true)
		== EWoTMacTrialAction::Continue);
	TestTrue(TEXT("matching started with previous rolls back"),
		WoTDecideMacTrialAction(Started, 42, true) == EWoTMacTrialAction::RollBack);
	TestTrue(TEXT("other-sequence started trial is stale"),
		WoTDecideMacTrialAction(Started, 43, true) == EWoTMacTrialAction::DropStaleTrial);
	TestTrue(TEXT("started trial without previous is stale"),
		WoTDecideMacTrialAction(Started, 42, false) == EWoTMacTrialAction::DropStaleTrial);

	const FWoTUpdateMarker Ready{42, EWoTUpdateTrialState::None};
	TestTrue(TEXT("translocation prevents a quit action"),
		WoTDecideMacQuitAction(Ready, 41, true, true) == EWoTMacQuitAction::None);
	TestTrue(TEXT("no ready prevents a quit action"),
		WoTDecideMacQuitAction({}, 41, true, false) == EWoTMacQuitAction::None);
	TestTrue(TEXT("equal ready is stale"),
		WoTDecideMacQuitAction(Ready, 42, true, false) == EWoTMacQuitAction::DropStaleReady);
	TestTrue(TEXT("older ready is stale"),
		WoTDecideMacQuitAction(Ready, 43, true, false) == EWoTMacQuitAction::DropStaleReady);
	TestTrue(TEXT("missing staged tree drops ready"),
		WoTDecideMacQuitAction(Ready, 41, false, false) == EWoTMacQuitAction::DropStaleReady);
	TestTrue(TEXT("new ready with staged tree swaps"),
		WoTDecideMacQuitAction(Ready, 41, true, false) == EWoTMacQuitAction::Swap);
	return !HasAnyErrors();
}

#endif
