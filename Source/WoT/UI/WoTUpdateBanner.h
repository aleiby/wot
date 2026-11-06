#pragma once

#include "CoreMinimal.h"
#include "WoTBootstrapCore/TestOverrides.h"
#include "../WoTBootstrapViewModel.h"

enum class EWoTUpdateBannerAction : uint8
{
	None,
	Restart,
	OpenReleasePage,
};

/** Player-facing projection of one background update snapshot. */
struct FWoTUpdateBannerPresentation
{
	bool bVisible = false;
	FString PrimaryLine;
	FString ReasonLine;
	FString MultiplayerLine;
	EWoTUpdateBannerAction Action = EWoTUpdateBannerAction::None;
	FString ButtonLabel;
	FString ReleasePageUrl;
};

WOT_API bool WoTIsHttpsReleasePageUrl(FStringView Url);
WOT_API FWoTUpdateBannerPresentation WoTBuildUpdateBannerPresentation(
	const FWoTUpdateState& State);

#if WOT_WITH_BOOTSTRAP_TEST_HARNESSES
/** Strict packaged-probe grammar; absent from guarded Shipping. */
WOT_API bool WoTParseFakeUpdateState(
	FStringView Value, FWoTUpdateState& OutState);
WOT_API TOptional<FWoTUpdateState> WoTFakeUpdateStateFromCommandLine();
#endif
