// Platform primitives retained for staging and the Mac quit-swap lane.
#pragma once
#include "CoreMinimal.h"
#include "WoTBootstrapCore/Error.h"

WOTUPDATERCORE_API bool WoTIsRunningTreeTranslocated(const FString& RunningTreeRoot);
WOTUPDATERCORE_API FWoTBootstrapStatus WoTVerifyStagedCodeSeal(const FString& StagedTreeRoot);
#if PLATFORM_MAC
WOTUPDATERCORE_API FWoTBootstrapStatus WoTRegisterLiveBundle(const FString& LiveTreeRoot);
#endif
