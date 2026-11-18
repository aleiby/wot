#include "WoTRetailReader/Progress.h"

namespace
{
FWoTBootstrapError WoTCacheProgressError(const TCHAR* Message)
{
	return MakeWoTBootstrapError(EWoTBootstrapError::InvalidNumber, Message,
		TEXT("cache-progress"));
}
}

FWoTCacheProgressSnapshot::FWoTCacheProgressSnapshot(FString InStage,
	uint64 InCompletedUnits, uint64 InTotalUnits)
	: Stage(MoveTemp(InStage))
	, CompletedUnits(InCompletedUnits)
	, TotalUnits(InTotalUnits)
{
}

FWoTCacheProgressAggregator::FWoTCacheProgressAggregator(
	IWoTProgressSink& InDownstream, uint64 InTotalUnits)
	: Downstream(InDownstream)
	, TotalUnits(InTotalUnits)
{
}

FWoTBootstrapStatus FWoTCacheProgressAggregator::Publish(
	FStringView Stage, uint64 CandidateCompleted)
{
	const uint64 Bounded = FMath::Min(CandidateCompleted, TotalUnits);
	PublishedCompleted = FMath::Max(PublishedCompleted,
		Bounded);
	CurrentStage = FString(Stage);
	return Downstream.ReportProgress(CurrentStage, PublishedCompleted, TotalUnits);
}

FWoTBootstrapStatus FWoTCacheProgressAggregator::BeginStage(
	FStringView Stage, uint64 InCompletedBeforeStage, uint64 StageUnits)
{
	if (InCompletedBeforeStage > TotalUnits
		|| StageUnits > TotalUnits - InCompletedBeforeStage)
	{
		return FWoTBootstrapStatus::Error(WoTCacheProgressError(
			TEXT("cache progress stage arithmetic exceeds the fixed total")));
	}
	CompletedBeforeStage = InCompletedBeforeStage;
	CurrentStageUnits = StageUnits;
	return Publish(Stage, CompletedBeforeStage);
}

FWoTBootstrapStatus FWoTCacheProgressAggregator::CompleteStage()
{
	return Publish(CurrentStage, CompletedBeforeStage + CurrentStageUnits);
}

FWoTBootstrapStatus FWoTCacheProgressAggregator::CompleteBuild()
{
	return Publish(TEXT("cache-builder-complete"), TotalUnits);
}

FWoTBootstrapStatus FWoTCacheProgressAggregator::ReportProgress(
	FStringView LocalStage, uint64 Completed, uint64 Total)
{
	if (Completed > Total)
	{
		return FWoTBootstrapStatus::Error(WoTCacheProgressError(
			TEXT("an importer reported completed progress beyond its total")));
	}
	uint64 StageCompleted = 0;
	if (Total != 0 && CurrentStageUnits != 0)
	{
		if (Completed == Total)
		{
			StageCompleted = CurrentStageUnits;
		}
		else
		{
			// Progress is observational rather than an artifact identity.  The
			// long-double conversion avoids uint64 multiplication overflow; the
			// monotonic clamp in Publish makes host rounding harmless.
			const long double Fraction = static_cast<long double>(Completed)
				/ static_cast<long double>(Total);
			StageCompleted = static_cast<uint64>(
				Fraction * static_cast<long double>(CurrentStageUnits));
		}
	}
	return Publish(LocalStage.IsEmpty() ? FStringView(CurrentStage) : LocalStage,
		CompletedBeforeStage + StageCompleted);
}

bool FWoTCacheProgressAggregator::IsCancellationRequested() const
{
	return Downstream.IsCancellationRequested();
}

FWoTCacheProgressSnapshot FWoTCacheProgressAggregator::Snapshot() const
{
	return FWoTCacheProgressSnapshot(CurrentStage, PublishedCompleted, TotalUnits);
}
