#pragma once

#include "CoreMinimal.h"

/** Shows a single-selection native .exe picker and reports a chosen path on the
 * game thread. Cancellation deliberately produces no callback. */
void WoTShowInstallerPicker(const FString& DefaultDirectory,
	TFunction<void(const FString&)> OnPicked);

#if PLATFORM_MAC && WITH_AUTOMATION_TESTS
/** Replaces only the Cocoa panel operation and observes modal entry/exit. */
void WoTSetInstallerPickerMacHooksForTesting(
	TFunction<TOptional<FString>()> ShowPanel,
	TFunction<void(bool)> ObserveModalState);
#endif
