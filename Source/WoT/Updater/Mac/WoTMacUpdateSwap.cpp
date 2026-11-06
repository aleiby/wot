#include "WoTMacUpdateSwap.h"

#if PLATFORM_MAC

#include "../../WoTBootstrapHeartbeat.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "../../WoTBootstrapViewModel.h"
#include "WoTBootstrapCore/ProfileSession.h"
#include "WoTUpdaterCore/UpdaterPlatform.h"

#include <cstdlib>

namespace
{
struct FWoTMacRememberedUpdate final
{
	FWoTUpdaterStateRoot Root;
	uint64 RunningSequence = 0;
};

TOptional<FWoTMacRememberedUpdate> GWoTMacRememberedUpdate;
TUniquePtr<FWoTProfileSession> GWoTMacExclusiveUpdateSession;

[[noreturn]] void WoTMacProfileSessionRestoreFailure()
{
	FPlatformMisc::LowLevelOutputDebugString(
		TEXT("WoT could not restore its shared profile session after the update transaction\n"));
	std::_Exit(EXIT_FAILURE);
}

void WoTMacUpdateHeartbeat(FStringView Event, uint64 Sequence,
	FStringView Detail = {})
{
	(void)WoTRecordBootstrapHeartbeat(Event,
		EWoTBootstrapPhase::EstablishingIdentity, Sequence, {}, {}, Detail);
}

void WoTMacUpdateSkip(uint64 Sequence, FStringView Detail)
{
	WoTMacUpdateHeartbeat(TEXT("quit-swap-skipped"), Sequence, Detail);
}

bool WoTMacUpdateRemove(const FString& ParentPath, FStringView Leaf)
{
	auto Parent = OpenNativeDirectoryNoFollow(ParentPath);
	return !Parent.IsError()
		&& !RemoveWoTUpdateEntry(Parent.GetValue(), Leaf).IsError();
}

TWoTBootstrapResult<FWoTNativeDirectoryHandle> WoTMacUpdateOpenOrCreateChild(
	const FWoTNativeDirectoryHandle& Parent, FStringView Leaf)
{
	bool bCreated = false;
	return OpenOrCreateNativeDirectoryAtNoFollow(Parent, Leaf, bCreated);
}

bool WoTMacUpdateIsUnsupported(const TWoTBootstrapResult<bool>& Result)
{
	return Result.IsError()
		&& Result.GetError().Code == EWoTBootstrapError::UnsupportedProtocol;
}

bool WoTMacUpdateTryExclusive(const FWoTMacUpdateSwapEffects& Effects,
	uint64 Sequence, FStringView ErrorDetail, FStringView BusyDetail)
{
	if (!Effects.TryExclusive)
	{
		WoTMacUpdateSkip(Sequence, ErrorDetail);
		return false;
	}
	const TOptional<bool> Exclusive = Effects.TryExclusive();
	if (!Exclusive.IsSet())
	{
		WoTMacUpdateSkip(Sequence, ErrorDetail);
		return false;
	}
	if (!Exclusive.GetValue())
	{
		WoTMacUpdateSkip(Sequence, BusyDetail);
		return false;
	}
	return true;
}

void WoTMacRestoreDefaultUpdateGateAfterRefusal()
{
	if (!GWoTMacExclusiveUpdateSession.IsValid()) return;
	if (GWoTMacExclusiveUpdateSession->RestoreShared().IsError())
	{
		GWoTMacExclusiveUpdateSession.Reset();
		WoTMacProfileSessionRestoreFailure();
	}
	if (!WoTRestoreProcessProfileSession(
			MoveTemp(GWoTMacExclusiveUpdateSession)))
	{
		WoTMacProfileSessionRestoreFailure();
	}
}

bool WoTMacUpdateFallbackRollback(const FWoTUpdaterStateRoot& Root,
	const FWoTNativeDirectoryHandle& Install,
	const FWoTNativeDirectoryHandle& Update,
	const FWoTNativeDirectoryHandle& Previous)
{
	auto Failed = WoTMacUpdateOpenOrCreateChild(Update, TEXT("failed"));
	if (Failed.IsError()) return false;
	if (RemoveWoTUpdateEntry(Failed.GetValue(), Root.LiveLeaf).IsError())
		return false;
	if (RenameNativeEntryAtNoFollow(Install, Root.LiveLeaf,
		Failed.GetValue(), Root.LiveLeaf, false).IsError())
	{
		return false;
	}
	if (RenameNativeEntryAtNoFollow(Previous, Root.LiveLeaf,
		Install, Root.LiveLeaf, false).IsError())
	{
		(void)RenameNativeEntryAtNoFollow(Failed.GetValue(), Root.LiveLeaf,
			Install, Root.LiveLeaf, false);
		return false;
	}
	(void)RemoveWoTUpdateEntry(Failed.GetValue(), Root.LiveLeaf);
	return true;
}

bool WoTMacUpdateFallbackSwap(const FWoTUpdaterStateRoot& Root,
	const FWoTNativeDirectoryHandle& Install,
	const FWoTNativeDirectoryHandle& Staged,
	const FWoTNativeDirectoryHandle& Previous)
{
	if (RenameNativeEntryAtNoFollow(Install, Root.LiveLeaf,
		Previous, Root.LiveLeaf, false).IsError())
	{
		return false;
	}
	if (RenameNativeEntryAtNoFollow(Staged, Root.LiveLeaf,
		Install, Root.LiveLeaf, false).IsError())
	{
		(void)RenameNativeEntryAtNoFollow(Previous, Root.LiveLeaf,
			Install, Root.LiveLeaf, false);
		return false;
	}
	return true;
}

enum class EWoTMacCompletedSwap : uint8
{
	Exchange,
	Fallback,
};

bool WoTMacRestoreOldLiveAfterSwap(const FWoTUpdaterStateRoot& Root,
	const FWoTNativeDirectoryHandle& Install,
	const FWoTNativeDirectoryHandle& Staged,
	const FWoTNativeDirectoryHandle& Previous,
	const FWoTMacUpdateSwapEffects& Effects,
	EWoTMacCompletedSwap CompletedSwap)
{
	if (CompletedSwap == EWoTMacCompletedSwap::Exchange)
	{
		if (!Effects.Exchange) return false;
		auto Restored = Effects.Exchange(Staged, Root.LiveLeaf,
			Install, Root.LiveLeaf);
		return !Restored.IsError() && Restored.GetValue();
	}

	if (RenameNativeEntryAtNoFollow(Install, Root.LiveLeaf,
		Staged, Root.LiveLeaf, false).IsError())
	{
		return false;
	}
	if (RenameNativeEntryAtNoFollow(Previous, Root.LiveLeaf,
		Install, Root.LiveLeaf, false).IsError())
	{
		(void)RenameNativeEntryAtNoFollow(Staged, Root.LiveLeaf,
			Install, Root.LiveLeaf, false);
		return false;
	}
	return true;
}

void WoTMacUpdateReverted(uint64 Sequence, FStringView Detail,
	bool bRestored)
{
	WoTMacUpdateHeartbeat(TEXT("quit-swap-reverted"), Sequence,
		bRestored ? Detail : FStringView(TEXT("old-live-restore-failed")));
}
} // namespace

EWoTMacTrialAction WoTDecideMacTrialAction(
	const TOptional<FWoTUpdateMarker>& Trial, uint64 RunningSequence,
	bool bPreviousTreeExists)
{
	if (!Trial.IsSet() || Trial->State == EWoTUpdateTrialState::Pending)
		return EWoTMacTrialAction::Continue;
	if (Trial->State == EWoTUpdateTrialState::Started
		&& Trial->Sequence == RunningSequence && bPreviousTreeExists)
	{
		return EWoTMacTrialAction::RollBack;
	}
	return EWoTMacTrialAction::DropStaleTrial;
}

EWoTMacQuitAction WoTDecideMacQuitAction(
	const TOptional<FWoTUpdateMarker>& Ready, uint64 RunningSequence,
	bool bStagedTreeExists, bool bTranslocated)
{
	if (bTranslocated || !Ready.IsSet()) return EWoTMacQuitAction::None;
	if (Ready->Sequence <= RunningSequence || !bStagedTreeExists)
		return EWoTMacQuitAction::DropStaleReady;
	return EWoTMacQuitAction::Swap;
}

bool WoTMacCheckUpdateTrialAtStartup(const FWoTUpdaterStateRoot& Root,
	uint64 RunningSequence, const FWoTMacUpdateSwapEffects& Effects)
{
	auto TrialRead = ReadWoTUpdateMarker(Root, TEXT("trial"), true);
	if (TrialRead.IsError())
	{
		(void)WoTMacUpdateRemove(Root.UpdateDir, TEXT("trial"));
		WoTMacUpdateHeartbeat(TEXT("trial-malformed-dropped"), RunningSequence);
		return true;
	}
	const TOptional<FWoTUpdateMarker>& Trial = TrialRead.GetValue();
	const EWoTMacTrialAction Action = WoTDecideMacTrialAction(Trial,
		RunningSequence,
		IFileManager::Get().DirectoryExists(*Root.PreviousTreeDir));
	if (Action == EWoTMacTrialAction::Continue) return true;

	if (Action == EWoTMacTrialAction::DropStaleTrial)
	{
		(void)WoTMacUpdateRemove(Root.UpdateDir, TEXT("trial"));
		return true;
	}

	const uint64 Sequence = Trial->Sequence;
	if (!WoTMacUpdateTryExclusive(Effects, Sequence,
		TEXT("rollback-gate-error"), TEXT("rollback-other-copy-running")))
	{
		return true;
	}
	if (!Effects.Exchange)
	{
		WoTMacUpdateSkip(Sequence, TEXT("rollback-exchange-unavailable"));
		WoTMacRestoreDefaultUpdateGateAfterRefusal();
		return true;
	}

	auto Install = OpenNativeDirectoryNoFollow(Root.InstallRoot);
	auto Update = OpenNativeDirectoryNoFollow(Root.UpdateDir);
	if (Install.IsError() || Update.IsError())
	{
		WoTMacUpdateSkip(Sequence, TEXT("rollback-root-open-failed"));
		WoTMacRestoreDefaultUpdateGateAfterRefusal();
		return true;
	}
	auto Previous = OpenNativeDirectoryAtNoFollow(Update.GetValue(), TEXT("prev"));
	if (Previous.IsError())
	{
		WoTMacUpdateSkip(Sequence, TEXT("rollback-prev-open-failed"));
		WoTMacRestoreDefaultUpdateGateAfterRefusal();
		return true;
	}

	auto Exchanged = Effects.Exchange(Previous.GetValue(), Root.LiveLeaf,
		Install.GetValue(), Root.LiveLeaf);
	bool bRolledBack = false;
	if (WoTMacUpdateIsUnsupported(Exchanged))
	{
		bRolledBack = WoTMacUpdateFallbackRollback(
			Root, Install.GetValue(), Update.GetValue(), Previous.GetValue());
	}
	else if (!Exchanged.IsError())
	{
		bRolledBack = Exchanged.GetValue();
		if (bRolledBack)
			(void)RemoveWoTUpdateEntry(Previous.GetValue(), Root.LiveLeaf);
	}
	if (!bRolledBack)
	{
		WoTMacUpdateSkip(Sequence, TEXT("rollback-swap-failed"));
		WoTMacRestoreDefaultUpdateGateAfterRefusal();
		return true;
	}

	FWoTBootstrapStatus Recorded = WriteWoTUpdateMarker(
		Root, TEXT("rolled-back"), {Sequence});
	if (!Recorded.IsError())
		(void)RemoveWoTUpdateEntry(Update.GetValue(), TEXT("trial"));
	WoTMacUpdateHeartbeat(TEXT("update-rolled-back"), Sequence,
		Recorded.IsError() ? FStringView(TEXT("rollback-marker-write-failed"))
			: FStringView());
	if (Effects.ShowRollbackAlert) Effects.ShowRollbackAlert(Sequence);
	return false;
}

EWoTMacQuitAction WoTMacApplyStagedUpdateAtQuit(
	const FWoTUpdaterStateRoot& Root, uint64 RunningSequence,
	const FWoTMacUpdateSwapEffects& Effects)
{
	const bool bTranslocated = WoTIsRunningTreeTranslocated(
		FPaths::Combine(Root.InstallRoot, Root.LiveLeaf));
	auto ReadyRead = ReadWoTUpdateMarker(Root, TEXT("ready"));
	if (ReadyRead.IsError())
	{
		WoTMacUpdateSkip(RunningSequence, TEXT("ready-read-failed"));
		return EWoTMacQuitAction::None;
	}
	TOptional<FWoTUpdateMarker> Ready = ReadyRead.GetValue();
	EWoTMacQuitAction Decision = WoTDecideMacQuitAction(Ready,
		RunningSequence,
		IFileManager::Get().DirectoryExists(*Root.StagedTreeDir),
		bTranslocated);
	if (Decision == EWoTMacQuitAction::None)
	{
		WoTMacUpdateSkip(RunningSequence,
			bTranslocated ? TEXT("translocated") : TEXT("no-ready"));
		return Decision;
	}

	uint64 Sequence = Ready->Sequence;
	if (!WoTMacUpdateTryExclusive(Effects, Sequence,
		TEXT("quit-gate-error"), TEXT("quit-other-copy-running")))
	{
		return EWoTMacQuitAction::None;
	}

	ReadyRead = ReadWoTUpdateMarker(Root, TEXT("ready"));
	if (ReadyRead.IsError())
	{
		WoTMacUpdateSkip(Sequence, TEXT("ready-reread-failed"));
		return EWoTMacQuitAction::None;
	}
	Ready = ReadyRead.GetValue();
	Decision = WoTDecideMacQuitAction(Ready, RunningSequence,
		IFileManager::Get().DirectoryExists(*Root.StagedTreeDir),
		bTranslocated);
	if (Decision == EWoTMacQuitAction::None)
	{
		WoTMacUpdateSkip(Sequence, TEXT("ready-changed"));
		return Decision;
	}
	Sequence = Ready->Sequence;
	if (Decision == EWoTMacQuitAction::DropStaleReady)
	{
		(void)WoTMacUpdateRemove(Root.UpdateDir, TEXT("ready"));
		(void)WoTMacUpdateRemove(Root.StagingDir, Root.LiveLeaf);
		WoTMacUpdateSkip(Sequence, Ready->Sequence <= RunningSequence
			? TEXT("stale-ready") : TEXT("staged-tree-missing"));
		return Decision;
	}

	if (!Effects.Exchange)
	{
		WoTMacUpdateSkip(Sequence, TEXT("quit-exchange-unavailable"));
		return EWoTMacQuitAction::None;
	}

	auto Install = OpenNativeDirectoryNoFollow(Root.InstallRoot);
	auto Update = OpenNativeDirectoryNoFollow(Root.UpdateDir);
	if (Install.IsError() || Update.IsError())
	{
		WoTMacUpdateSkip(Sequence, TEXT("quit-root-open-failed"));
		return EWoTMacQuitAction::None;
	}
	auto Staged = OpenNativeDirectoryAtNoFollow(Update.GetValue(), TEXT("staged"));
	auto Previous = WoTMacUpdateOpenOrCreateChild(Update.GetValue(), TEXT("prev"));
	if (Staged.IsError() || Previous.IsError())
	{
		WoTMacUpdateSkip(Sequence, TEXT("quit-workspace-open-failed"));
		return EWoTMacQuitAction::None;
	}
	if (RemoveWoTUpdateEntry(Previous.GetValue(), Root.LiveLeaf).IsError())
	{
		WoTMacUpdateSkip(Sequence, TEXT("previous-tree-remove-failed"));
		return EWoTMacQuitAction::None;
	}

	auto Exchanged = Effects.Exchange(Staged.GetValue(), Root.LiveLeaf,
		Install.GetValue(), Root.LiveLeaf);
	TOptional<EWoTMacCompletedSwap> CompletedSwap;
	if (WoTMacUpdateIsUnsupported(Exchanged))
	{
		if (WoTMacUpdateFallbackSwap(
			Root, Install.GetValue(), Staged.GetValue(), Previous.GetValue()))
		{
			CompletedSwap = EWoTMacCompletedSwap::Fallback;
		}
	}
	else if (!Exchanged.IsError() && Exchanged.GetValue())
	{
		CompletedSwap = EWoTMacCompletedSwap::Exchange;
	}
	if (!CompletedSwap.IsSet())
	{
		WoTMacUpdateSkip(Sequence, TEXT("quit-swap-failed"));
		return EWoTMacQuitAction::None;
	}

	const bool bTrialWritten = Effects.WriteTrial
		&& !Effects.WriteTrial(Root, Sequence).IsError();
	if (!bTrialWritten)
	{
		(void)RemoveWoTUpdateEntry(Update.GetValue(), TEXT("trial"));
		const bool bRestored = WoTMacRestoreOldLiveAfterSwap(Root,
			Install.GetValue(), Staged.GetValue(), Previous.GetValue(), Effects,
			CompletedSwap.GetValue());
		WoTMacUpdateReverted(Sequence, TEXT("trial-write-failed"), bRestored);
		return EWoTMacQuitAction::None;
	}

	if (CompletedSwap.GetValue() == EWoTMacCompletedSwap::Exchange)
	{
		const bool bMoved = Effects.MoveOldToPrevious
			&& !Effects.MoveOldToPrevious(Staged.GetValue(), Root.LiveLeaf,
				Previous.GetValue(), Root.LiveLeaf).IsError();
		if (!bMoved)
		{
			const bool bRestored = WoTMacRestoreOldLiveAfterSwap(Root,
				Install.GetValue(), Staged.GetValue(), Previous.GetValue(), Effects,
				CompletedSwap.GetValue());
			(void)RemoveWoTUpdateEntry(Update.GetValue(), TEXT("trial"));
			WoTMacUpdateReverted(Sequence,
				TEXT("previous-tree-move-failed"), bRestored);
			return EWoTMacQuitAction::None;
		}
	}
	if (RemoveWoTUpdateEntry(Update.GetValue(), TEXT("ready")).IsError())
	{
		WoTMacUpdateSkip(Sequence, TEXT("ready-remove-failed"));
		return EWoTMacQuitAction::Swap;
	}
	if (Effects.RegisterLiveBundle)
		Effects.RegisterLiveBundle(FPaths::Combine(Root.InstallRoot, Root.LiveLeaf));
	WoTMacUpdateHeartbeat(TEXT("quit-swap-applied"), Sequence);
	return EWoTMacQuitAction::Swap;
}

FWoTMacUpdateSwapEffects WoTMacDefaultUpdateSwapEffects()
{
	FWoTMacUpdateSwapEffects Effects;
	Effects.TryExclusive = []() -> TOptional<bool>
	{
		TUniquePtr<FWoTProfileSession> Session =
			WoTTakeProcessProfileSession();
		if (!Session.IsValid()) return {};
		auto Exclusive = Session->TryAcquireExclusive();
		if (Exclusive.IsError())
		{
			if (!Session->IsShared()
				|| !WoTRestoreProcessProfileSession(MoveTemp(Session)))
			{
				WoTMacProfileSessionRestoreFailure();
			}
			return {};
		}
		if (!Exclusive.GetValue())
		{
			if (!WoTRestoreProcessProfileSession(MoveTemp(Session)))
			{
				WoTMacProfileSessionRestoreFailure();
			}
			return false;
		}
		GWoTMacExclusiveUpdateSession = MoveTemp(Session);
		return true;
	};
	Effects.Exchange = [](const FWoTNativeDirectoryHandle& DirectoryA,
		FStringView LeafA, const FWoTNativeDirectoryHandle& DirectoryB,
		FStringView LeafB)
	{
		return ExchangeNativeEntriesAtNoFollow(
			DirectoryA, LeafA, DirectoryB, LeafB);
	};
	Effects.WriteTrial = [](const FWoTUpdaterStateRoot& Root, uint64 Sequence)
	{
		return WriteWoTUpdateMarker(Root, TEXT("trial"),
			{Sequence, EWoTUpdateTrialState::Pending});
	};
	Effects.MoveOldToPrevious = [](
		const FWoTNativeDirectoryHandle& Staged, FStringView StagedLeaf,
		const FWoTNativeDirectoryHandle& Previous, FStringView PreviousLeaf)
	{
		return RenameNativeEntryAtNoFollow(
			Staged, StagedLeaf, Previous, PreviousLeaf, false);
	};
	Effects.ShowRollbackAlert = [](uint64)
	{
		if (!FApp::IsUnattended())
		{
			FPlatformMisc::MessageBoxExt(EAppMsgType::Ok,
				TEXT("The update didn't start. WoT Reborn restored the previous version. Open it again to keep playing."),
				TEXT("WoT Reborn"));
		}
	};
	Effects.RegisterLiveBundle = [](const FString& Live)
	{
		(void)WoTRegisterLiveBundle(Live);
	};
	return Effects;
}

void WoTMacRememberUpdateRootForQuit(
	const FWoTUpdaterStateRoot& Root, uint64 RunningSequence)
{
	GWoTMacRememberedUpdate = FWoTMacRememberedUpdate{Root, RunningSequence};
}

void WoTMacApplyRememberedUpdateAtQuit()
{
	if (!GWoTMacRememberedUpdate.IsSet()) return;
	const FWoTMacRememberedUpdate Remembered = GWoTMacRememberedUpdate.GetValue();
	GWoTMacRememberedUpdate.Reset();
	(void)WoTMacApplyStagedUpdateAtQuit(Remembered.Root,
		Remembered.RunningSequence, WoTMacDefaultUpdateSwapEffects());
}

#endif
