#include "WoTBootstrapDiskPreflight.h"

namespace
{
bool WoTBootstrapDiskPreflight_Add(uint64 Value, uint64& Total)
{
	if (Value > MAX_uint64 - Total)
	{
		Total = MAX_uint64;
		return false;
	}
	Total += Value;
	return true;
}
}

FWoTDiskPreflightResult CheckDiskBudget(EWoTBootstrapPhase Phase,
	const FWoTDiskBudgetInputs& Inputs, IWoTFreeSpaceProbe& FreeSpace)
{
	FWoTDiskPreflightResult Result;
	Result.Phase = Phase;
	bool bNoOverflow = true;
	auto Add = [&Result, &bNoOverflow](uint64 Value)
	{
		bNoOverflow = WoTBootstrapDiskPreflight_Add(
			Value, Result.RequiredBytes) && bNoOverflow;
	};
	switch (Phase)
	{
	case EWoTBootstrapPhase::DownloadingUpdate:
		Add(Inputs.UpdateDownloadBytes);

		Add(Inputs.ExtractionBytes);
		Add(Inputs.CandidateInstallBytes);
		break;
	case EWoTBootstrapPhase::OriginalGameNeeded:
		break;
	case EWoTBootstrapPhase::ExtractingOriginalGame:
		Add(Inputs.ExtractionBytes);
		Add(Inputs.CacheConstructionBytes);
		break;
	case EWoTBootstrapPhase::BuildingContent:
		Add(Inputs.CacheConstructionBytes);
		break;
	default:
		Result.DiagnosticCode =
			EWoTBootstrapDiagnosticCode::DiskPreflightPhase;
		return Result;
	}
	Add(Inputs.SafetyMarginBytes);
	if (!bNoOverflow)
	{
		Result.DiagnosticCode =
			EWoTBootstrapDiagnosticCode::DiskBudgetOverflow;
		return Result;
	}
	Result.bProbeSucceeded =
		FreeSpace.QueryAvailableBytes(Phase, Result.AvailableBytes);
	if (!Result.bProbeSucceeded)
	{
		Result.DiagnosticCode =
			EWoTBootstrapDiagnosticCode::DiskProbeFailed;
		return Result;
	}
	Result.bPassed = Result.AvailableBytes >= Result.RequiredBytes;
	if (!Result.bPassed)
	{
		Result.DiagnosticCode =
			EWoTBootstrapDiagnosticCode::InsufficientDiskSpace;
	}
	return Result;
}
