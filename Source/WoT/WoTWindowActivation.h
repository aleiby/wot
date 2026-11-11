#pragma once

#include "CoreMinimal.h"

#if PLATFORM_MAC
void WoTInstallWindowActivationMonitor();
void WoTShutdownWindowActivationMonitor();
bool WoTWasWindowActivatedByClick();
#else
inline void WoTInstallWindowActivationMonitor() {}
inline void WoTShutdownWindowActivationMonitor() {}
inline bool WoTWasWindowActivatedByClick() { return false; }
#endif
