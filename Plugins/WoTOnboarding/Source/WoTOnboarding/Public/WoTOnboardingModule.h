#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Modules/ModuleInterface.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/WeakObjectPtrTemplates.h"
#include "WoTAngrealGuide.h"

class SWoTGuideBrowser;
class SWoTPickupCard;
class UGameViewportClient;

// Runtime module for the optional onboarding aids
// (docs/plans/2026-11-06-onboarding-aids.md). Task 3 carried the guide data
// asset, Task 4 the persisted settings and their seam rows, Task 5 the guide
// browser (ShowGuideRequested toggles it; a Field Guide menu row that follows
// the master toggle live opens it from the front-end and in-game menus).
// Task 6 adds the first-pickup card: the first add of each guide class shows
// a passive card once per profile, and the guide key pressed while a card is
// up opens the guide at that card's row. The death recap arrives with Task 7.
class WOTONBOARDING_API FWoTOnboardingModule final : public IModuleInterface
{
public:
	/** The registrar tag carried by every seam row this module adds. */
	static FName SettingsRowOwner();

	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/** True while the guide browser is on the viewport. */
	bool IsGuideBrowserOpen() const { return GuideBrowser.IsValid(); }

	/** Specs only. Under automation the ClientItemAdded handler is inert, so
	    no other suite's inventory traffic writes the persisted seen list,
	    unless a spec sets a guide here; the handler then decides over it.
	    Hosting stays off under automation either way. Null restores inert. */
	void SetPickupCardGuideForTesting(UWoTAngrealGuide* Guide)
	{
		PickupCardTestGuide = Guide;
	}
	/** Cards the handler decided to show since startup, hosted or not. */
	int32 GetPickupCardsShownForTesting() const { return PickupCardsShown; }

private:
	void RefreshMenuRowRegistration();
	void HandleShowGuideRequested();
	void HandleCloseClientWindows(FName Keeper);
	void HandleSettingsChanged();
	void HandleClientItemAdded(FName ClassName);
	void ToggleGuideBrowser();
	void OpenGuideBrowser();
	void CloseGuideBrowser();
	/** The imported guide, loaded on first use and kept for the session;
	    null when the content tree does not carry it (asked once). */
	UWoTAngrealGuide* ResolveGuide();
	void ShowPickupCard(const FWoTAngrealGuideRow& Row);
	void ClosePickupCard();
	/** Closes the card on the next tick (its expiry fires mid-paint). */
	void ClosePickupCardDeferred();

	FDelegateHandle ShowGuideHandle;
	FDelegateHandle CloseWindowsHandle;
	FDelegateHandle SettingsChangedHandle;
	FDelegateHandle ItemAddedHandle;
	TSharedPtr<SWoTGuideBrowser> GuideBrowser;
	TWeakObjectPtr<UGameViewportClient> GuideViewport;

	TStrongObjectPtr<UWoTAngrealGuide> CachedGuide;
	bool bGuideAbsent = false;

	TSharedPtr<SWoTPickupCard> PickupCard;
	TWeakObjectPtr<UGameViewportClient> PickupCardViewport;
	/** The class the visible card shows; None without a card. */
	FName PickupCardClass;
	FTSTicker::FDelegateHandle PickupCardCloseTicker;
	TWeakObjectPtr<UWoTAngrealGuide> PickupCardTestGuide;
	int32 PickupCardsShown = 0;
};
