#pragma once

#include "CoreMinimal.h"
#include "Engine/GameEngine.h"
#include "WoTGameEngine.generated.h"

// First-browse routing decision: returns the full replacement URL for the
// one initial browse, unset afterwards. Stateless; the engine wrapper owns
// the one-shot latch.
WOT_API TOptional<FString> WoTGuardedFirstBrowseUrl(bool bFirstBrowse);
// Later browses/loads: only a LOCAL load of the uncooked default front end
// is redirected. A network URL (non-empty host) never is: FURL fills Map with
// GameDefaultMap when a client connect address such as "127.0.0.1:7777" names
// no map, and replacing it would drop the host and turn every Connect into a
// local front-end reload.
WOT_API TOptional<FString> WoTGuardedBrowseReplacement(
	bool bFirstBrowse, FStringView RequestedMap, bool bNetworkURL = false);

// Removes only the two command-line replay routes that bypass Browse. All
// bytes outside matching tokens are retained exactly.
WOT_API FString WoTGuardedSanitizeReplayCommandLine(FStringView CommandLine);

/** Code-only engine host for the retail-independent neutral shells. */
UCLASS(Transient, Config=Engine)
class WOT_API UWoTGameEngine final : public UGameEngine
{
	GENERATED_BODY()

public:
	virtual void Init(IEngineLoop* InEngineLoop) override;
	virtual EBrowseReturnVal::Type Browse(FWorldContext& WorldContext, FURL URL,
		FString& Error) override;
	virtual bool LoadMap(FWorldContext& WorldContext, FURL URL,
		UPendingNetGame* Pending, FString& Error) override;

private:
#if WOT_GUARDED_BOOTSTRAP
	bool bHasBrowsedGuardedGameWorld = false;
#endif
};
