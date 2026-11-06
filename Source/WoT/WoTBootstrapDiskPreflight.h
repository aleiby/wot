#pragma once

#include "CoreMinimal.h"
#include "WoTBootstrapViewModel.h"

/** Conservative phase-specific byte requirements supplied by the real engine. */
struct FWoTDiskBudgetInputs
{
	uint64 UpdateDownloadBytes = 0;
	uint64 ExtractionBytes = 0;
	uint64 CandidateInstallBytes = 0;
	uint64 RollbackBackupBytes = 0;
	uint64 CacheConstructionBytes = 0;
	uint64 SafetyMarginBytes = 0;
};

struct FWoTDiskPreflightResult
{
	EWoTBootstrapPhase Phase = EWoTBootstrapPhase::Fatal;
	uint64 RequiredBytes = 0;
	uint64 AvailableBytes = 0;
	bool bProbeSucceeded = false;
	bool bPassed = false;
	EWoTBootstrapDiagnosticCode DiagnosticCode =
		EWoTBootstrapDiagnosticCode::None;
};

/** Narrow platform/test seam; implementations may not redirect the target. */
class IWoTFreeSpaceProbe
{
public:
	virtual ~IWoTFreeSpaceProbe() = default;
	virtual bool QueryAvailableBytes(
		EWoTBootstrapPhase Phase, uint64& OutAvailableBytes) = 0;
};

FWoTDiskPreflightResult CheckDiskBudget(EWoTBootstrapPhase Phase,
	const FWoTDiskBudgetInputs& Inputs, IWoTFreeSpaceProbe& FreeSpace);
