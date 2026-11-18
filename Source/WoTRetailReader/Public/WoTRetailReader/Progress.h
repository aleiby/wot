// Immutable cache-build progress snapshots and the monotonic importer adapter.
#pragma once

#include "CoreMinimal.h"
#include "WoTRetailReader/Contract.h"

class WOTRETAILREADER_API FWoTCacheProgressSnapshot final
{
public:
	FWoTCacheProgressSnapshot() = default;
	FWoTCacheProgressSnapshot(FString InStage, uint64 InCompletedUnits,
		uint64 InTotalUnits);

	const FString& GetStage() const { return Stage; }
	uint64 GetCompletedUnits() const { return CompletedUnits; }
	uint64 GetTotalUnits() const { return TotalUnits; }

private:
	FString Stage;
	uint64 CompletedUnits = 0;
	uint64 TotalUnits = 0;
};

/**
 * Maps each importer's local progress onto one immutable global unit scale.
 *
 * The global total is fixed before importer work starts; every build starts at zero.
 */
class WOTRETAILREADER_API FWoTCacheProgressAggregator final
	: public IWoTProgressSink
{
public:
	FWoTCacheProgressAggregator(IWoTProgressSink& InDownstream,
		uint64 InTotalUnits);

	FWoTBootstrapStatus BeginStage(FStringView Stage, uint64 CompletedBeforeStage,
		uint64 StageUnits);
	FWoTBootstrapStatus CompleteStage();
	FWoTBootstrapStatus CompleteBuild();

	virtual FWoTBootstrapStatus ReportProgress(FStringView LocalStage,
		uint64 Completed, uint64 Total) override;
	virtual bool IsCancellationRequested() const override;

	FWoTCacheProgressSnapshot Snapshot() const;

private:
	FWoTBootstrapStatus Publish(FStringView Stage, uint64 CandidateCompleted);

	IWoTProgressSink& Downstream;
	FString CurrentStage = TEXT("cache-builder");
	uint64 TotalUnits = 0;
	uint64 CompletedBeforeStage = 0;
	uint64 CurrentStageUnits = 0;
	uint64 PublishedCompleted = 0;
};
