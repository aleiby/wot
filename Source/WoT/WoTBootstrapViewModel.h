#pragma once

#include "CoreMinimal.h"

/** The complete, closed set of player-visible bootstrap phases. */
enum class EWoTBootstrapPhase : uint8
{
	EstablishingIdentity,
	CheckingGameUpdates,
	DownloadingUpdate,
	CheckingOriginalGameData,
	OriginalGameNeeded,
	ExtractingOriginalGame,
	BuildingContent,
	VerifyingContent,
	StartingGame,
	Ready,
	Fatal,
};

enum class EWoTOriginalGameNeededReason : uint8
{
	Missing,
	NotTheInstaller,
	Downloading,
	PickedWrongFile,
	OnlineOnly,
	UnsupportedReparsePoint,
	HardLinked,
};

WOT_API const TCHAR* LexToString(EWoTOriginalGameNeededReason Reason);

/** The closed set of diagnostic codes that may cross into player-visible UI. */
enum class EWoTBootstrapDiagnosticCode : uint8
{
	None,
	BootstrapFailure,
	EarlyUINotAdopted,
	GuardedSessionAbsent,
	DiskPreflightAbsent,
	DiskReportRejected,
	AnnouncementReplay,
	DiskPreflightPhase,
	DiskBudgetOverflow,
	DiskProbeFailed,
	InsufficientDiskSpace,
	UnexpectedPhase,
	InstallerInvalid,
	InstallerPathRejected,
	PayloadCapabilityAbsent,
	IllegalPhaseTransition,
	SignatureInvalid,
	ConfigInvalid,
	InstallFailed,
	CacheIntegrityFailure,
};

enum class EWoTUpdateStatus : uint8
{
	None,
	Checking,
	Downloading,
	Ready,
	Available,
	KnownBad,
};

/** Immutable background-update state delivered onto the game thread. */
struct FWoTUpdateState
{
	EWoTUpdateStatus Status = EWoTUpdateStatus::None;
	uint64 CompletedBytes = 0;
	uint64 TotalBytes = 0;
	uint64 ReleaseSequence = 0;
	/** The release's human version (0.1.2) when known; banners fall back to the sequence. */
	FString HumanVersion;
	FString Reason;
	FString ReleasePageUrl;
};

/** Where the LAN installer copy stands at the original-game-needed screen. */
enum class EWoTInstallerCopyStatus : uint8
{
	/** Nothing published yet: no line and no button. */
	Unknown,
	/** Not listening: the player's click starts it (Windows, Mac). */
	Off,
	/** Listening for WoT Reborn on another computer. */
	Waiting,
	/** A copy is streaming in (bytes in CompletedBytes/TotalBytes). */
	Copying,
	/** The last copy failed its check; waiting for another one. */
	Damaged,
	/** Three copies arrived damaged: no longer listening. */
	GaveUp,
	/** An offer came but there is not enough free space (NeededBytes). */
	NotEnoughSpace,
	/** Receiving is unavailable here (switched off, no data folder). */
	Disabled,
};

/** The LAN installer copy's state, published by the worker with every
 * original-game-needed progress event (spec B1, B5). */
struct FWoTInstallerCopyState
{
	EWoTInstallerCopyStatus Status = EWoTInstallerCopyStatus::Unknown;
	/** The receiver starts by itself here (Wine on Linux: a Steam Deck), so
	 * there is no "Copy from another computer" button. */
	bool bAutomatic = false;
	/** The sharer's generic display label ("a Windows PC", ...). */
	FString PeerLabel;
	/** NotEnoughSpace: the free bytes the copy needs. */
	uint64 NeededBytes = 0;

	bool operator==(const FWoTInstallerCopyState& Other) const
	{
		return Status == Other.Status && bAutomatic == Other.bAutomatic
			&& PeerLabel == Other.PeerLabel && NeededBytes == Other.NeededBytes;
	}
	bool operator!=(const FWoTInstallerCopyState& Other) const
	{
		return !(*this == Other);
	}
};

/** Every copy attempt publishes its bytes under its own stage id with this
 * prefix ("lan-copy-1", "lan-copy-2", ...): a new id restarts the bytes. */
inline constexpr TCHAR WoTInstallerCopyStagePrefix[] = TEXT("lan-copy-");

/** A bounded worker-to-game-thread progress publication. */
struct FWoTBootstrapProgressEvent
{
	uint64 OperationEpoch = 0;
	FString StageId;
	FString CurrentItem;
	uint64 CompletedUnits = 0;
	uint64 TotalUnits = 0;
	TOptional<uint64> CompletedBytes;
	TOptional<uint64> TotalBytes;
	double ElapsedSeconds = 0.0;
	TOptional<EWoTOriginalGameNeededReason> OriginalGameNeededReason;
	FString OriginalGameNeededDetail;
	/** Original-game-needed only: the LAN installer copy's state. */
	TOptional<FWoTInstallerCopyState> InstallerCopy;
};

/**
 * An immutable-by-publication copy of bootstrap state. Callers receive this
 * value by copy; the mutable state machine that produced it remains private.
 */
struct FWoTBootstrapSnapshot
{
	EWoTBootstrapPhase Phase = EWoTBootstrapPhase::EstablishingIdentity;
	uint64 OperationEpoch = 0;
	uint64 CompletedUnits = 0;
	uint64 TotalUnits = 0;
	TOptional<uint64> CompletedBytes;
	TOptional<uint64> TotalBytes;
	TOptional<uint64> RequiredBytes;
	TOptional<uint64> AvailableBytes;
	bool bDiskPreflightVisible = false;
	FString StageId;
	FText StageText;
	FString CurrentItem;
	TArray<FString> ActivityFeed;
	double UnitsPerSecond = 0.0;
	double ElapsedSeconds = 0.0;
	bool bETAIsConfident = false;
	/** The current phase's own 0..1 completion, restarting at every transition.
	 * Unset until the phase reports a total. Per-phase rather than a weighted
	 * whole-install fraction: which phases run (downloads, extraction) is only
	 * known as the flow unfolds, so any fixed weighting misplaces the bar. */
	TOptional<float> PhaseFraction;
	bool bCanExit = true;
	EWoTOriginalGameNeededReason OriginalGameNeededReason =
		EWoTOriginalGameNeededReason::Missing;
	FString OriginalGameNeededDetail;
	/** Original-game-needed only: the LAN installer copy's state. */
	FWoTInstallerCopyState InstallerCopy;
	bool bMandatoryWork = false;
	bool bResumeStateVerified = false;
	/** Notify-only update state; the bootstrap intentionally presents no menu UI for it. */
	bool bUpdateNotifyOnly = false;
	FString NotifyOnlyReason;
	uint64 NotifyOnlyReleaseSequence = 0;
	FString NotifyOnlyReleasePageUrl;
	FWoTUpdateState Update;
	uint64 RunningReleaseSequence = 0;
	EWoTBootstrapDiagnosticCode DiagnosticCode =
		EWoTBootstrapDiagnosticCode::None;
	TOptional<EWoTBootstrapPhase> FailedPhase;
	/** Actionable player-facing failure text; empty when only a diagnostic applies. */
	FString FatalMessage;

	bool IsTerminal() const
	{
		return Phase == EWoTBootstrapPhase::Ready
			|| Phase == EWoTBootstrapPhase::Fatal;
	}
};

/** Pure monotonic state and snapshot publication; it performs no I/O. */
class FWoTBootstrapViewModel final
{
public:
	FWoTBootstrapViewModel();

	FWoTBootstrapSnapshot Snapshot() const { return Current; }
	bool TransitionTo(EWoTBootstrapPhase NextPhase,
		bool bPayloadCapabilityIsObservable = false);
	bool UpdateProgress(uint64 ExpectedEpoch, uint64 Completed, uint64 Total,
		TOptional<uint64> InCompletedBytes = {},
		TOptional<uint64> InTotalBytes = {});
	bool UpdateProgress(FWoTBootstrapProgressEvent&& Event);
	bool ReportDiskBudget(uint64 ExpectedEpoch,
		uint64 RequiredBytes, uint64 AvailableBytes);
	void ClearDiskBudget(uint64 ExpectedEpoch);
	void Fail(EWoTBootstrapDiagnosticCode DiagnosticCode, FString Message = FString());
	/** Dedicated post-Ready terminal path. It intentionally does not broaden
	 * the ordinary monotonic transition graph. */
	void FailRuntimeIntegrity(FString Message);
	void SetOriginalGameNeeded(EWoTOriginalGameNeededReason Reason,
		FString Detail = FString());
	void SetMandatoryWork(bool bMandatory);
	void MarkVerifiedResumeState(bool bVerified);
	void SetUpdateNotifyOnly(bool bNotifyOnly, FString Reason,
		uint64 ReleaseSequence, FString ReleasePageUrl);
	void SetUpdateState(FWoTUpdateState&& State);
	void SetRunningReleaseSequence(uint64 Sequence);
	void InvalidateOutstandingOperation();

	static bool IsLegalTransition(
		EWoTBootstrapPhase From, EWoTBootstrapPhase To);
	static bool IsBytePhase(EWoTBootstrapPhase Phase);
	/** IsBytePhase, relaxed for one stage: the original-game-needed screen's
	 * LAN copy ("lan-copy-N") carries byte progress too. */
	static bool IsBytePhase(EWoTBootstrapPhase Phase, FStringView StageId);
	static bool IsItemPhase(EWoTBootstrapPhase Phase);
	static FText PhaseText(EWoTBootstrapPhase Phase);
	static FText StageText(FStringView StageId);
	static FText DiagnosticCodeText(EWoTBootstrapDiagnosticCode Code);

private:
	FWoTBootstrapSnapshot Current;
};
