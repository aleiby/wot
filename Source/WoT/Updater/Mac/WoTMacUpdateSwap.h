#pragma once

#include "CoreMinimal.h"

#if PLATFORM_MAC

#include "WoTBootstrapCore/Error.h"
#include "WoTBootstrapCore/NativeHandles.h"
#include "WoTUpdaterCore/UpdaterPaths.h"

enum class EWoTMacTrialAction : uint8
{
	Continue,
	RollBack,
	DropStaleTrial,
};

EWoTMacTrialAction WoTDecideMacTrialAction(
	const TOptional<FWoTUpdateMarker>& Trial, uint64 RunningSequence,
	bool bPreviousTreeExists);

enum class EWoTMacQuitAction : uint8
{
	None,
	Swap,
	DropStaleReady,
};

EWoTMacQuitAction WoTDecideMacQuitAction(
	const TOptional<FWoTUpdateMarker>& Ready, uint64 RunningSequence,
	bool bStagedTreeExists, bool bTranslocated);

struct FWoTMacUpdateSwapEffects
{
	TFunction<TOptional<bool>()> TryExclusive;
	TFunction<TWoTBootstrapResult<bool>(
		const FWoTNativeDirectoryHandle&, FStringView,
		const FWoTNativeDirectoryHandle&, FStringView)> Exchange;
	TFunction<FWoTBootstrapStatus(
		const FWoTUpdaterStateRoot&, uint64)> WriteTrial;
	TFunction<FWoTBootstrapStatus(
		const FWoTNativeDirectoryHandle&, FStringView,
		const FWoTNativeDirectoryHandle&, FStringView)> MoveOldToPrevious;
	TFunction<void(uint64)> ShowRollbackAlert;
	TFunction<void(const FString&)> RegisterLiveBundle;
};

bool WoTMacCheckUpdateTrialAtStartup(const FWoTUpdaterStateRoot& Root,
	uint64 RunningSequence, const FWoTMacUpdateSwapEffects& Effects);
EWoTMacQuitAction WoTMacApplyStagedUpdateAtQuit(
	const FWoTUpdaterStateRoot& Root, uint64 RunningSequence,
	const FWoTMacUpdateSwapEffects& Effects);
FWoTMacUpdateSwapEffects WoTMacDefaultUpdateSwapEffects();
void WoTMacRememberUpdateRootForQuit(
	const FWoTUpdaterStateRoot& Root, uint64 RunningSequence);
void WoTMacApplyRememberedUpdateAtQuit();

#endif
