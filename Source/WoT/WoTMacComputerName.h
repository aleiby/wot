#pragma once

#include "CoreMinimal.h"

#if PLATFORM_MAC
/**
 * The user-facing macOS Computer Name (System Settings > General > Sharing),
 * e.g. "Robin’s MacBook Pro", or empty when the system store has none.
 * FPlatformProcess::ComputerName is gethostname ("Robins-MacBook-Pro.local").
 */
FString WoTMacFriendlyComputerName();
#endif
