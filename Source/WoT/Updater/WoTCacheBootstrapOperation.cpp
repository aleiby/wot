#include "WoTCacheBootstrapOperation.h"

#include "WoTBootstrapCore/Hash.h"
#include "WoTBootstrapCore/ProfileSession.h"
#include "WoTRetailReader/Package.h"
#include "WoTRetailReader/ProductionContract.h"
#include "WoTRetailReader/Reader.h"
#include "WoTRetailReader/WorldDescriptorSidecar.h"

#if !PLATFORM_WINDOWS
#include <errno.h>
#endif

namespace
{
class FWoTCacheReopenCancellation final : public IWoTProgressSink
{
public:
	explicit FWoTCacheReopenCancellation(
		const IWoTProgressSink& InBuildProgress)
		: BuildProgress(InBuildProgress)
	{
	}

	virtual FWoTBootstrapStatus ReportProgress(
		FStringView, uint64, uint64) override
	{
		return FWoTBootstrapStatus::Success();
	}

	virtual bool IsCancellationRequested() const override
	{
		return BuildProgress.IsCancellationRequested();
	}

private:
	const IWoTProgressSink& BuildProgress;
};

/** Passes the reader's writes to the cache store and remembers whether the
 * store refused one. The store and the reader both report CorruptInstallation,
 * so the code alone cannot say which side a failed build came from. */
class FWoTCacheDestinationWriter final : public IWoTObjectWriter
{
public:
	explicit FWoTCacheDestinationWriter(IWoTObjectWriter& InDestination)
		: Destination(InDestination)
	{
	}

	virtual FWoTBootstrapStatus WriteObject(FStringView LogicalId,
		FStringView Schema, TConstArrayView<uint8> Bytes,
		const FWoTSha256& Sha256Digest, uint64 MaximumBytes) override
	{
		FWoTBootstrapStatus Written = Destination.WriteObject(
			LogicalId, Schema, Bytes, Sha256Digest, MaximumBytes);
		bRefused = bRefused || Written.IsError();
		return Written;
	}

	bool Refused() const { return bRefused; }

private:
	IWoTObjectWriter& Destination;
	bool bRefused = false;
};

FWoTBootstrapStatus WoTRestoreCacheBuildProfileSession(
	TUniquePtr<FWoTProfileSession>& Session)
{
	if (!Session.IsValid())
	{
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::UnsafePath,
			TEXT("the cache build lost its profile session owner")));
	}
	if (Session->IsExclusive())
	{
		FWoTBootstrapStatus Shared = Session->RestoreShared();
		if (Shared.IsError()) return Shared;
	}
	if (!WoTRestoreProcessProfileSession(MoveTemp(Session)))
	{
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::UnsafePath,
			TEXT("the cache build could not restore the process profile session")));
	}
	return FWoTBootstrapStatus::Success();
}
} // namespace

TWoTBootstrapResult<FWoTPublishedCacheReceipt>
FWoTCacheBootstrapOperation::BuildAndPublish(FWoTVerifiedInstaller&& Installer,
	FWoTCacheStore& Store, IWoTProgressSink& Progress)
{
	using FResult = TWoTBootstrapResult<FWoTPublishedCacheReceipt>;
	bLastFailureCameFromInstaller = false;
	TWoTBootstrapResult<FWoTRetailPackage> Package = FWoTRetailPackage::Open(
		MoveTemp(Installer.Snapshot), FWoTArchiveLimits());
	if (Package.IsError())
	{
		bLastFailureCameFromInstaller = true;
		return FResult::Error(MoveTemp(Package).GetError());
	}
	return BuildAndPublish(MoveTemp(Package).GetValue(), Store, Progress);
}

TWoTBootstrapResult<FWoTPublishedCacheReceipt>
FWoTCacheBootstrapOperation::BuildAndPublish(FWoTRetailPackage&& Package,
	FWoTCacheStore& Store, IWoTProgressSink& Progress)
{
	using FResult = TWoTBootstrapResult<FWoTPublishedCacheReceipt>;
	bLastFailureCameFromInstaller = false;
	FWoTBootstrapStatus InitialIdentity =
		Package.ReverifyHeldSnapshotAfterExtraction();
	if (InitialIdentity.IsError())
	{
		bLastFailureCameFromInstaller = true;
		return FResult::Error(MoveTemp(InitialIdentity).GetError());
	}
	const FWoTSha256 InstallerSha256 = Package.CachedInputSha256();
	TWoTBootstrapResult<FWoTRetailContract> Contract =
		WoTLoadProductionRetailContract();
	if (Contract.IsError())
	{
		return FResult::Error(MoveTemp(Contract).GetError());
	}
	const FWoTSha256 ContractSha256 =
		ComputeRetailContractIdentity(Contract.GetValue());
	TWoTBootstrapResult<FWoTSha256> ContentLabel = GetCacheContentLabel();
	if (ContentLabel.IsError())
	{
		return FResult::Error(MoveTemp(ContentLabel).GetError());
	}
	TUniquePtr<FWoTProfileSession> Session =
		WoTTakeProcessProfileSession();
	if (!Session.IsValid())
	{
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::UnsafePath,
			TEXT("cache build requires the process profile session")));
	}
	auto FailAfterRestoringSession =
		[this, &Session](FWoTBootstrapError Error,
			bool bFromInstaller = false) -> FResult
	{
		FWoTBootstrapStatus Restored =
			WoTRestoreCacheBuildProfileSession(Session);
		if (Restored.IsError())
		{
			return FResult::Error(MoveTemp(Restored).GetError());
		}
		bLastFailureCameFromInstaller = bFromInstaller;
		return FResult::Error(MoveTemp(Error));
	};
	TWoTBootstrapResult<bool> Exclusive = Session->TryAcquireExclusive();
	if (Exclusive.IsError() || !Exclusive.GetValue())
	{
		FWoTBootstrapError Error = Exclusive.IsError()
			? MoveTemp(Exclusive).GetError()
			: MakeWoTBootstrapError(EWoTBootstrapError::ServiceUnavailable,
				WoTProfileSessionContentionMessage(), Session->DataRoot());
		if (!Exclusive.IsError())
		{
			// Another copy holds the shared lock. Carry the platform's
			// lock-contention code so the worker shows "close other copies"
			// rather than the generic unwritable-folder guidance.
#if PLATFORM_WINDOWS
			Error.NativeCode = 33; // ERROR_LOCK_VIOLATION
#else
			Error.NativeCode = EWOULDBLOCK;
#endif
		}
		return FailAfterRestoringSession(MoveTemp(Error));
	}
	auto Begun = Store.BeginBuild(*Session);
	if (Begun.IsError())
	{
		return FailAfterRestoringSession(MoveTemp(Begun).GetError());
	}
	FWoTRetailReader Reader;
	FWoTCacheDestinationWriter Destination(Store);
#if WITH_AUTOMATION_TESTS
	auto Build = BuildForTesting
		? BuildForTesting(Package, Contract.GetValue(), Destination, Progress)
		: Reader.Build(Package, Contract.GetValue(), Destination, Progress);
#else
	auto Build = Reader.Build(Package, Contract.GetValue(), Destination, Progress);
#endif
	if (Build.IsError())
	{
		// The reader reads the installer and writes through Destination. A
		// refused write is the cache's. Otherwise the installer is to blame when
		// its held snapshot failed the reader's check, or when it no longer
		// verifies now. Other CorruptInstallation errors come from bytes already
		// proven by hash, so they are release bugs that choosing the installer
		// again would not fix. Skip the re-verify (a full re-read when only the
		// change time moved) once the player has quit.
		const bool bFromInstaller = !Destination.Refused()
			&& (Build.GetError().Path == TEXT("held-installer-snapshot")
				|| (!Progress.IsCancellationRequested()
					&& Package.ReverifyHeldSnapshotAfterExtraction().IsError()));
		return FailAfterRestoringSession(
			MoveTemp(Build).GetError(), bFromInstaller);
	}
	auto Published = Store.Publish(Build.GetValue());
	if (Published.IsError())
	{
		return FailAfterRestoringSession(MoveTemp(Published).GetError());
	}
	if (FWoTBootstrapStatus Restored =
			WoTRestoreCacheBuildProfileSession(Session); Restored.IsError())
	{
		return FResult::Error(MoveTemp(Restored).GetError());
	}
	FWoTCacheGenerationBuildIdentity Expected;
	Expected.bHasBuildIdentity = true;
	Expected.InstallerSha256 = InstallerSha256;
	Expected.ContractSha256 = ContractSha256;
	Expected.ProductionDescriptorSha256 = ContentLabel.GetValue();
	FWoTCacheReopenCancellation ReopenCancellation(Progress);
	TWoTBootstrapResult<TOptional<FWoTCacheGenerationLease>> Reopened =
		Store.TryOpenCurrent(Expected, &ReopenCancellation);
	if (Reopened.IsError())
	{
		return FResult::Error(MoveTemp(Reopened).GetError());
	}
	if (!Reopened.GetValue().IsSet()
		|| Reopened.GetValue()->GetManifestSha256()
			!= Build.GetValue().ManifestSha256)
	{
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("the published cache could not be reopened after restoring the shared profile session")));
	}
	FWoTPublishedCacheReceipt Receipt;
	Receipt.InstallerSha256 = InstallerSha256;
	Receipt.ContractSha256 = ContractSha256;
	Receipt.ProductionDescriptorSha256 = ContentLabel.GetValue();
	Receipt.ManifestSha256 = Build.GetValue().ManifestSha256;
	Receipt.CompletedProgressUnits = Build.GetValue().Progress.GetCompletedUnits();
	Receipt.TotalProgressUnits = Build.GetValue().Progress.GetTotalUnits();
	return FResult::Success(MoveTemp(Receipt));
}
