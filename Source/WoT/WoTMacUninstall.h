#pragma once

#include "CoreMinimal.h"

/** Installs the Mac-only game-menu command and keeps it present across rebuilds. */
void WoTInstallMacUninstallMenuItem();

/** Removes the menu observer during module shutdown. */
void WoTShutdownMacUninstallMenuItem();

#if PLATFORM_MAC && WITH_AUTOMATION_TESTS
struct FWoTMacUninstallTestResult
{
	FString FailureReason;
	TArray<FString> RemovedPaths;
	TArray<FString> RemainingPaths;
};

FWoTMacUninstallTestResult WoTRunMacUninstallFilesystemForTesting(
	bool bConfirm, const FString& ExpectedDataRoot,
	const FString& AuthenticatedDataRoot, const FString& RuntimeBundle,
	TFunction<TOptional<FString>(const FString&)> ResolveOriginalBundle,
	TFunction<bool(const FString&)> RecycleOriginalBundle);

struct FWoTMacUninstallProductionFlowTestResult
{
	FString FailureReason;
	bool bInProgress = true;
	bool bMenuEnabled = false;
	bool bProcessSessionShared = false;
	bool bSubsystemsAlive = false;
	bool bExitRequested = true;
};

/** Drive the real confirmed-uninstall controller through a refusal. */
FWoTMacUninstallProductionFlowTestResult
	WoTRunMacUninstallProductionRefusalForTesting(
		const FString& ExpectedDataRoot,
		const FString& AuthenticatedDataRoot,
		const FString& RuntimeBundle,
		TFunction<TOptional<FString>(const FString&)> ResolveOriginalBundle);
void WoTInstallMacUninstallMenuItemForTesting();
int32 WoTMacUninstallMenuItemCountForTesting();
bool WoTMacUninstallMenuEnabledForTesting();
void WoTRemoveMacUninstallMenuItemForTesting();
#endif
