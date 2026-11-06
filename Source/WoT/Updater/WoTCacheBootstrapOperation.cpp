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
	TWoTBootstrapResult<FWoTRetailPackage> Package = FWoTRetailPackage::Open(
		MoveTemp(Installer.Snapshot), FWoTArchiveLimits());
	if (Package.IsError())
	{
		return FResult::Error(MoveTemp(Package).GetError());
	}
	return BuildAndPublish(MoveTemp(Package).GetValue(), Store, Progress);
}

TWoTBootstrapResult<FWoTPublishedCacheReceipt>
FWoTCacheBootstrapOperation::BuildAndPublish(FWoTRetailPackage&& Package,
	FWoTCacheStore& Store, IWoTProgressSink& Progress)
{
	using FResult = TWoTBootstrapResult<FWoTPublishedCacheReceipt>;
	FWoTBootstrapStatus InitialIdentity =
		Package.ReverifyHeldSnapshotAfterExtraction();
	if (InitialIdentity.IsError())
	{
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
	TWoTBootstrapResult<FWoTSha256> DescriptorSha256 =
		GetCacheExpectedProductionDescriptorSha256();
	if (DescriptorSha256.IsError())
	{
		return FResult::Error(MoveTemp(DescriptorSha256).GetError());
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
		[&Session](FWoTBootstrapError Error) -> FResult
	{
		FWoTBootstrapStatus Restored =
			WoTRestoreCacheBuildProfileSession(Session);
		return Restored.IsError()
			? FResult::Error(MoveTemp(Restored).GetError())
			: FResult::Error(MoveTemp(Error));
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
#if WITH_AUTOMATION_TESTS
	auto Build = BuildForTesting
		? BuildForTesting(Package, Contract.GetValue(), Store, Progress)
		: Reader.Build(Package, Contract.GetValue(), Store, Progress);
#else
	auto Build = Reader.Build(Package, Contract.GetValue(), Store, Progress);
#endif
	if (Build.IsError())
	{
		return FailAfterRestoringSession(MoveTemp(Build).GetError());
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
	Expected.ProductionDescriptorSha256 = DescriptorSha256.GetValue();
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
	Receipt.ProductionDescriptorSha256 = DescriptorSha256.GetValue();
	Receipt.ManifestSha256 = Build.GetValue().ManifestSha256;
	Receipt.CompletedProgressUnits = Build.GetValue().Progress.GetCompletedUnits();
	Receipt.TotalProgressUnits = Build.GetValue().Progress.GetTotalUnits();
	return FResult::Success(MoveTemp(Receipt));
}
