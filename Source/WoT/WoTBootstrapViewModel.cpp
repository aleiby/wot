#include "WoTBootstrapViewModel.h"

#define LOCTEXT_NAMESPACE "WoTBootstrap"

const TCHAR* LexToString(EWoTOriginalGameNeededReason Reason)
{
	switch (Reason)
	{
	case EWoTOriginalGameNeededReason::NotTheInstaller:
		return TEXT("not-the-installer");
	case EWoTOriginalGameNeededReason::Downloading:
		return TEXT("downloading");
	case EWoTOriginalGameNeededReason::PickedWrongFile:
		return TEXT("picked-wrong-file");
	case EWoTOriginalGameNeededReason::OnlineOnly:
		return TEXT("online-only");
	case EWoTOriginalGameNeededReason::UnsupportedReparsePoint:
		return TEXT("unsupported-reparse-point");
	case EWoTOriginalGameNeededReason::HardLinked:
		return TEXT("hard-linked");
	case EWoTOriginalGameNeededReason::Missing:
	default:
		return TEXT("missing");
	}
}

FWoTBootstrapViewModel::FWoTBootstrapViewModel()
{
	Current.OperationEpoch = 1;
}

bool FWoTBootstrapViewModel::IsLegalTransition(
	EWoTBootstrapPhase From, EWoTBootstrapPhase To)
{
	if (To == EWoTBootstrapPhase::Fatal)
	{
		return From != EWoTBootstrapPhase::Ready
			&& From != EWoTBootstrapPhase::Fatal;
	}
	switch (From)
	{
	case EWoTBootstrapPhase::EstablishingIdentity:
        return To == EWoTBootstrapPhase::CheckingGameUpdates;
    case EWoTBootstrapPhase::CheckingGameUpdates:
        return To == EWoTBootstrapPhase::DownloadingUpdate
            || To == EWoTBootstrapPhase::CheckingOriginalGameData;
    case EWoTBootstrapPhase::DownloadingUpdate:
        return To == EWoTBootstrapPhase::CheckingOriginalGameData;
	case EWoTBootstrapPhase::CheckingOriginalGameData:
		return To == EWoTBootstrapPhase::OriginalGameNeeded
			|| To == EWoTBootstrapPhase::ExtractingOriginalGame
			|| To == EWoTBootstrapPhase::BuildingContent
			|| To == EWoTBootstrapPhase::VerifyingContent;
	case EWoTBootstrapPhase::OriginalGameNeeded:
		return To == EWoTBootstrapPhase::ExtractingOriginalGame;
	case EWoTBootstrapPhase::ExtractingOriginalGame:
		return To == EWoTBootstrapPhase::BuildingContent;
	case EWoTBootstrapPhase::BuildingContent:
		return To == EWoTBootstrapPhase::VerifyingContent;
	case EWoTBootstrapPhase::VerifyingContent:
		return To == EWoTBootstrapPhase::StartingGame;
	case EWoTBootstrapPhase::StartingGame:
		return To == EWoTBootstrapPhase::Ready;
	case EWoTBootstrapPhase::Ready:
	case EWoTBootstrapPhase::Fatal:
	default:
		return false;
	}
}

bool FWoTBootstrapViewModel::TransitionTo(
	EWoTBootstrapPhase NextPhase, bool bPayloadCapabilityIsObservable)
{
	if (!IsLegalTransition(Current.Phase, NextPhase)
		|| (NextPhase == EWoTBootstrapPhase::Ready
			&& !bPayloadCapabilityIsObservable))
	{
		return false;
	}
	Current.Phase = NextPhase;
	if (++Current.OperationEpoch == 0)
	{
		++Current.OperationEpoch;
	}
	Current.CompletedUnits = 0;
	Current.TotalUnits = 0;
	Current.PhaseFraction.Reset();
	Current.CompletedBytes.Reset();
	Current.TotalBytes.Reset();
	Current.RequiredBytes.Reset();
	Current.AvailableBytes.Reset();
	Current.bDiskPreflightVisible = false;
	Current.StageId.Reset();
	Current.StageText = FText::GetEmpty();
	Current.CurrentItem.Reset();
	Current.ActivityFeed.Reset();
	Current.UnitsPerSecond = 0.0;
	Current.ElapsedSeconds = 0.0;
	Current.bETAIsConfident = false;
	Current.DiagnosticCode = EWoTBootstrapDiagnosticCode::None;
	Current.OriginalGameNeededReason = EWoTOriginalGameNeededReason::Missing;
	Current.OriginalGameNeededDetail.Reset();
	Current.InstallerCopy = FWoTInstallerCopyState();
	Current.FailedPhase.Reset();
	Current.FatalMessage.Reset();
	Current.bCanExit = NextPhase != EWoTBootstrapPhase::StartingGame;
	return true;
}

bool FWoTBootstrapViewModel::UpdateProgress(uint64 ExpectedEpoch,
	uint64 Completed, uint64 Total, TOptional<uint64> InCompletedBytes,
	TOptional<uint64> InTotalBytes)
{
	FWoTBootstrapProgressEvent Event;
	Event.OperationEpoch = ExpectedEpoch;
	Event.CompletedUnits = Completed;
	Event.TotalUnits = Total;
	Event.CompletedBytes = InCompletedBytes;
	Event.TotalBytes = InTotalBytes;
	return UpdateProgress(MoveTemp(Event));
}

bool FWoTBootstrapViewModel::UpdateProgress(FWoTBootstrapProgressEvent&& Event)
{
	if (Current.IsTerminal() || Event.OperationEpoch != Current.OperationEpoch
		|| Event.CompletedUnits > Event.TotalUnits
		|| Event.CompletedUnits < Current.CompletedUnits
		|| (Event.TotalUnits != 0 && Current.TotalUnits != 0
			&& Event.TotalUnits != Current.TotalUnits))
	{
		return false;
	}
	// While the original game is needed, each LAN copy attempt publishes its
	// bytes under its own stage id. A new id, or the end of a copy, restarts
	// the byte baseline: a copy after a damaged one starts again from zero.
	// Within one stage the bytes stay monotonic.
	const bool bRestartsBytes =
		Current.Phase == EWoTBootstrapPhase::OriginalGameNeeded
		&& Event.StageId != Current.StageId;
	const TOptional<uint64> BaselineBytes = bRestartsBytes
		? TOptional<uint64>() : Current.CompletedBytes;
	const TOptional<uint64> BaselineTotal = bRestartsBytes
		? TOptional<uint64>() : Current.TotalBytes;
	if (Event.CompletedBytes.IsSet() != Event.TotalBytes.IsSet()
		|| (Event.CompletedBytes.IsSet()
			&& (!IsBytePhase(Current.Phase, Event.StageId)
				|| Event.CompletedBytes.GetValue() > Event.TotalBytes.GetValue()
				|| (BaselineBytes.IsSet()
					&& Event.CompletedBytes.GetValue() < BaselineBytes.GetValue())
				|| (BaselineTotal.IsSet()
					&& Event.TotalBytes.GetValue() != BaselineTotal.GetValue())))
		|| (!Event.CompletedBytes.IsSet() && IsBytePhase(Current.Phase)
			&& (Event.CompletedUnits != 0 || Event.TotalUnits != 0))
		|| Event.ElapsedSeconds < Current.ElapsedSeconds)
	{
		return false;
	}
	const uint64 PreviousCompleted = Current.CompletedUnits;
	const TOptional<uint64> PreviousCompletedBytes = BaselineBytes;
	const double PreviousElapsed = Current.ElapsedSeconds;
	if (bRestartsBytes)
	{
		Current.PhaseFraction.Reset();
	}
	Current.CompletedUnits = Event.CompletedUnits;
	Current.TotalUnits = Event.TotalUnits;
	Current.CompletedBytes = Event.CompletedBytes;
	Current.TotalBytes = Event.TotalBytes;
	Current.StageId = MoveTemp(Event.StageId);
	Current.StageText = StageText(Current.StageId);
	Current.CurrentItem = MoveTemp(Event.CurrentItem);
	Current.ElapsedSeconds = Event.ElapsedSeconds;
	if (Current.Phase == EWoTBootstrapPhase::OriginalGameNeeded
		&& Event.OriginalGameNeededReason.IsSet())
	{
		Current.OriginalGameNeededReason =
			Event.OriginalGameNeededReason.GetValue();
		Current.OriginalGameNeededDetail =
			MoveTemp(Event.OriginalGameNeededDetail);
	}
	if (Current.Phase == EWoTBootstrapPhase::OriginalGameNeeded
		&& Event.InstallerCopy.IsSet())
	{
		Current.InstallerCopy = MoveTemp(Event.InstallerCopy.GetValue());
	}
	if (Current.ElapsedSeconds > PreviousElapsed)
	{
		const uint64 CompletedDelta = Current.CompletedBytes.IsSet()
			? Current.CompletedBytes.GetValue()
				- PreviousCompletedBytes.Get(0)
			: Current.CompletedUnits - PreviousCompleted;
		Current.UnitsPerSecond = static_cast<double>(CompletedDelta)
			/ (Current.ElapsedSeconds - PreviousElapsed);
		const uint64 Completed = Current.CompletedBytes.IsSet()
			? Current.CompletedBytes.GetValue() : Current.CompletedUnits;
		const uint64 Total = Current.TotalBytes.IsSet()
			? Current.TotalBytes.GetValue() : Current.TotalUnits;
		Current.bETAIsConfident = Current.UnitsPerSecond > 0.0
			&& Total > Completed;
	}
	if (!Current.StageId.IsEmpty())
	{
		const FString Activity = Current.CurrentItem.IsEmpty()
			? Current.StageText.ToString() : Current.CurrentItem;
		if (!Activity.IsEmpty()
			&& (Current.ActivityFeed.IsEmpty()
				|| Current.ActivityFeed.Last() != Activity))
		{
			Current.ActivityFeed.Add(Activity);
			if (Current.ActivityFeed.Num() > 8)
			{
				Current.ActivityFeed.RemoveAt(0);
			}
		}
	}
	const TOptional<uint64> Numerator = Current.CompletedBytes.IsSet()
		? Current.CompletedBytes : TOptional<uint64>(Current.CompletedUnits);
	const TOptional<uint64> Denominator = Current.TotalBytes.IsSet()
		? Current.TotalBytes : TOptional<uint64>(Current.TotalUnits);
	if (Denominator.IsSet() && Denominator.GetValue() != 0)
	{
		const float LocalFraction = static_cast<float>(
			static_cast<double>(Numerator.GetValue())
			/ static_cast<double>(Denominator.GetValue()));
		Current.PhaseFraction = FMath::Max(Current.PhaseFraction.Get(0.0f),
			FMath::Clamp(LocalFraction, 0.0f, 1.0f));
	}
	return true;
}

bool FWoTBootstrapViewModel::ReportDiskBudget(uint64 ExpectedEpoch,
	uint64 InRequiredBytes, uint64 InAvailableBytes)
{
	if (ExpectedEpoch != Current.OperationEpoch || Current.IsTerminal()
		|| Current.RequiredBytes.IsSet() || Current.AvailableBytes.IsSet())
	{
		return false;
	}
	Current.RequiredBytes = InRequiredBytes;
	Current.AvailableBytes = InAvailableBytes;
	Current.bDiskPreflightVisible = true;
	return true;
}

void FWoTBootstrapViewModel::ClearDiskBudget(uint64 ExpectedEpoch)
{
	if (ExpectedEpoch == Current.OperationEpoch && !Current.IsTerminal())
	{
		Current.bDiskPreflightVisible = false;
		Current.RequiredBytes.Reset();
		Current.AvailableBytes.Reset();
	}
}

FText FWoTBootstrapViewModel::DiagnosticCodeText(
	EWoTBootstrapDiagnosticCode Code)
{
	switch (Code)
	{
	case EWoTBootstrapDiagnosticCode::None:
		return FText::GetEmpty();
	case EWoTBootstrapDiagnosticCode::EarlyUINotAdopted:
		return FText::FromString(TEXT("EARLY-UI-NOT-ADOPTED"));
	case EWoTBootstrapDiagnosticCode::GuardedSessionAbsent:
		return FText::FromString(TEXT("GUARDED-SESSION-ABSENT"));
	case EWoTBootstrapDiagnosticCode::DiskPreflightAbsent:
		return FText::FromString(TEXT("DISK-PREFLIGHT-ABSENT"));
	case EWoTBootstrapDiagnosticCode::DiskReportRejected:
		return FText::FromString(TEXT("DISK-REPORT-REJECTED"));
	case EWoTBootstrapDiagnosticCode::AnnouncementReplay:
		return FText::FromString(TEXT("ANNOUNCEMENT-REPLAY"));
	case EWoTBootstrapDiagnosticCode::DiskPreflightPhase:
		return FText::FromString(TEXT("DISK-PREFLIGHT-PHASE"));
	case EWoTBootstrapDiagnosticCode::DiskBudgetOverflow:
		return FText::FromString(TEXT("DISK-BUDGET-OVERFLOW"));
	case EWoTBootstrapDiagnosticCode::DiskProbeFailed:
		return FText::FromString(TEXT("DISK-PROBE-FAILED"));
	case EWoTBootstrapDiagnosticCode::InsufficientDiskSpace:
		return FText::FromString(TEXT("INSUFFICIENT-DISK-SPACE"));
	case EWoTBootstrapDiagnosticCode::UnexpectedPhase:
		return FText::FromString(TEXT("UNEXPECTED-PHASE"));
	case EWoTBootstrapDiagnosticCode::InstallerInvalid:
		return FText::FromString(TEXT("INSTALLER-INVALID"));
	case EWoTBootstrapDiagnosticCode::InstallerPathRejected:
		return FText::FromString(TEXT("INSTALLER-PATH-REJECTED"));
	case EWoTBootstrapDiagnosticCode::PayloadCapabilityAbsent:
		return FText::FromString(TEXT("PAYLOAD-CAPABILITY-ABSENT"));
	case EWoTBootstrapDiagnosticCode::IllegalPhaseTransition:
		return FText::FromString(TEXT("ILLEGAL-PHASE-TRANSITION"));
	case EWoTBootstrapDiagnosticCode::SignatureInvalid:
		return FText::FromString(TEXT("SIGNATURE-INVALID"));
	case EWoTBootstrapDiagnosticCode::ConfigInvalid:
		return FText::FromString(TEXT("CONFIG-INVALID"));
	case EWoTBootstrapDiagnosticCode::InstallFailed:
		return FText::FromString(TEXT("INSTALL-FAILED"));
	case EWoTBootstrapDiagnosticCode::CacheIntegrityFailure:
		return FText::FromString(TEXT("CACHE-INTEGRITY-FAILURE"));
	case EWoTBootstrapDiagnosticCode::BootstrapFailure:
	default:
		return FText::FromString(TEXT("BOOTSTRAP-FAILURE"));
	}
}

void FWoTBootstrapViewModel::Fail(EWoTBootstrapDiagnosticCode DiagnosticCode, FString Message)
{
	if (Current.IsTerminal())
	{
		return;
	}
	const EWoTBootstrapPhase FailedPhase = Current.Phase;
	TransitionTo(EWoTBootstrapPhase::Fatal);
	Current.FailedPhase = FailedPhase;
	Current.FatalMessage = MoveTemp(Message);
	Current.DiagnosticCode = DiagnosticCode == EWoTBootstrapDiagnosticCode::None
		? EWoTBootstrapDiagnosticCode::BootstrapFailure : DiagnosticCode;
}

void FWoTBootstrapViewModel::FailRuntimeIntegrity(FString Message)
{
	if (Current.Phase != EWoTBootstrapPhase::Ready) return;
	const EWoTBootstrapPhase FailedPhase = Current.Phase;
	Current.Phase = EWoTBootstrapPhase::Fatal;
	if (++Current.OperationEpoch == 0) ++Current.OperationEpoch;
	Current.CompletedUnits = 0;
	Current.TotalUnits = 0;
	Current.PhaseFraction.Reset();
	Current.CompletedBytes.Reset();
	Current.TotalBytes.Reset();
	Current.RequiredBytes.Reset();
	Current.AvailableBytes.Reset();
	Current.bDiskPreflightVisible = false;
	Current.StageId.Reset();
	Current.StageText = FText::GetEmpty();
	Current.CurrentItem.Reset();
	Current.ActivityFeed.Reset();
	Current.UnitsPerSecond = 0.0;
	Current.ElapsedSeconds = 0.0;
	Current.bETAIsConfident = false;
	Current.bCanExit = true;
	Current.FailedPhase = FailedPhase;
	Current.DiagnosticCode = EWoTBootstrapDiagnosticCode::CacheIntegrityFailure;
	Current.FatalMessage = MoveTemp(Message);
}

void FWoTBootstrapViewModel::SetOriginalGameNeeded(
	EWoTOriginalGameNeededReason Reason, FString Detail)
{
	if (Current.Phase != EWoTBootstrapPhase::OriginalGameNeeded)
	{
		return;
	}
	Current.OriginalGameNeededReason = Reason;
	Current.OriginalGameNeededDetail = MoveTemp(Detail);
}

void FWoTBootstrapViewModel::SetMandatoryWork(bool bMandatory)
{
	if (!Current.IsTerminal())
	{
		Current.bMandatoryWork = bMandatory;
	}
}

void FWoTBootstrapViewModel::MarkVerifiedResumeState(bool bVerified)
{
	Current.bResumeStateVerified = bVerified;
}

void FWoTBootstrapViewModel::SetUpdateNotifyOnly(bool bNotifyOnly,
	FString Reason, uint64 ReleaseSequence, FString ReleasePageUrl)
{
	Current.bUpdateNotifyOnly = bNotifyOnly;
	Current.NotifyOnlyReason = bNotifyOnly ? MoveTemp(Reason) : FString();
	Current.NotifyOnlyReleaseSequence = bNotifyOnly ? ReleaseSequence : 0;
	Current.NotifyOnlyReleasePageUrl = bNotifyOnly
		? MoveTemp(ReleasePageUrl) : FString();
}

void FWoTBootstrapViewModel::SetUpdateState(FWoTUpdateState&& State)
{
	Current.Update = MoveTemp(State);
}

void FWoTBootstrapViewModel::SetRunningReleaseSequence(uint64 Sequence)
{
	Current.RunningReleaseSequence = Sequence;
}

void FWoTBootstrapViewModel::InvalidateOutstandingOperation()
{
	if (++Current.OperationEpoch == 0)
	{
		++Current.OperationEpoch;
	}
}

bool FWoTBootstrapViewModel::IsBytePhase(EWoTBootstrapPhase Phase)
{
	return Phase == EWoTBootstrapPhase::DownloadingUpdate;
}

bool FWoTBootstrapViewModel::IsBytePhase(EWoTBootstrapPhase Phase,
	FStringView StageId)
{
	return IsBytePhase(Phase)
		|| (Phase == EWoTBootstrapPhase::OriginalGameNeeded
			&& StageId.StartsWith(WoTInstallerCopyStagePrefix,
				ESearchCase::CaseSensitive));
}

bool FWoTBootstrapViewModel::IsItemPhase(EWoTBootstrapPhase Phase)
{
	return Phase == EWoTBootstrapPhase::BuildingContent;
}

FText FWoTBootstrapViewModel::PhaseText(EWoTBootstrapPhase Phase)
{
	switch (Phase)
	{
	case EWoTBootstrapPhase::EstablishingIdentity:
		return LOCTEXT("EstablishingIdentity", "Establishing identity");
	case EWoTBootstrapPhase::CheckingGameUpdates:
		return LOCTEXT("CheckingUpdates", "Checking for game updates");
	case EWoTBootstrapPhase::DownloadingUpdate:
		return LOCTEXT("DownloadingUpdate", "Downloading update");
	case EWoTBootstrapPhase::CheckingOriginalGameData:
		return LOCTEXT("CheckingOriginal", "Checking original game data");
	case EWoTBootstrapPhase::OriginalGameNeeded:
		return LOCTEXT("OriginalGameNeeded", "Original game needed");
	case EWoTBootstrapPhase::ExtractingOriginalGame:
		return LOCTEXT("ExtractingOriginal", "Extracting original game");
	case EWoTBootstrapPhase::BuildingContent:
		return LOCTEXT("BuildingContent",
			"Building textures, sounds, meshes, and maps");
	case EWoTBootstrapPhase::VerifyingContent:
		return LOCTEXT("VerifyingContent", "Verifying content");
	case EWoTBootstrapPhase::StartingGame:
		return LOCTEXT("StartingGame", "Starting game");
	case EWoTBootstrapPhase::Ready:
		return LOCTEXT("Ready", "Ready");
	case EWoTBootstrapPhase::Fatal:
	default:
		return LOCTEXT("Fatal", "Unable to continue");
	}
}

FText FWoTBootstrapViewModel::StageText(FStringView StageId)
{
	if (StageId == TEXT("cache-builder-complete"))
		return LOCTEXT("CacheBuilderCompleteStage", "Verifying imported content");
	if (StageId == TEXT("texture-sound"))
	{
		return LOCTEXT("TextureSoundStage", "Processing textures and sounds");
	}
	if (StageId == TEXT("mesh-animation"))
		return LOCTEXT("MeshAnimationStage", "Processing meshes and animations");
	if (StageId == TEXT("required-world-set"))
		return LOCTEXT("RequiredWorldStage", "Processing maps");
	if (StageId == TEXT("front-end-entry"))
		return LOCTEXT("FrontEndEntryStage", "Preparing the main menu");
	if (StageId == TEXT("front-end-world"))
		return LOCTEXT("FrontEndWorldStage", "Processing the main-menu world");
	if (StageId == TEXT("font-procedural"))
		return LOCTEXT("FontProceduralStage", "Processing fonts and effects");
	if (StageId == TEXT("registry-localization-defaults"))
		return LOCTEXT("RegistryLocalizationStage", "Processing game data and text");
	if (StageId == TEXT("front-end-scene"))
		return LOCTEXT("FrontEndSceneStage", "Finishing the main menu");
	if (StageId.StartsWith(WoTInstallerCopyStagePrefix, ESearchCase::CaseSensitive))
		return LOCTEXT("InstallerCopyStage", "Copying the original game installer");
	return StageId.IsEmpty() ? FText::GetEmpty()
		: LOCTEXT("UnknownStage", "Processing content");
}

#undef LOCTEXT_NAMESPACE
