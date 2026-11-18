#include "WoTUpdaterCore/UpdaterPlatform.h"

bool WoTIsRunningTreeTranslocated(const FString&) { return false; }
FWoTBootstrapStatus WoTVerifyStagedCodeSeal(const FString&) { return FWoTBootstrapStatus::Success(); }
