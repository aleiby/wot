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
struct FWoTPickupCardText;

// Runtime module for the optional onboarding aids
// (docs/plans/2026-11-06-onboarding-aids.md). Task 3 carried the guide data
// asset, Task 4 the persisted settings and their seam rows, Task 5 the guide
// browser (ShowGuideRequested toggles it; a Field Guide menu row that follows
// the master toggle live opens it from the front-end and in-game menus).
// Task 6 adds the first-pickup card: the first add of each guide class shows
// a passive card once per profile, and the guide key pressed while a card is
// up opens the guide at that card's row. Task 7 adds the death recap: when the
// server names the item that killed the local player, a card in the same slot
// names it and its counter. Every seam binding and the menu row follow the
// master switch live (Task 7's parity-off gate); only the settings rows, the
// way back on, stay registered while it is off.
class WOTONBOARDING_API FWoTOnboardingModule final : public IModuleInterface
{
public:
	/** The registrar tag carried by every seam row this module adds. */
	static FName SettingsRowOwner();

	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/** True while the guide browser is on the viewport. */
	bool IsGuideBrowserOpen() const { return GuideBrowser.IsValid(); }
	/** True while a first-pickup card is on the viewport. */
	bool IsPickupCardOpen() const { return PickupCard.IsValid(); }

	/** Specs only. Under automation the ClientItemAdded and ClientKilledBy
	    handlers are inert, so no other suite's inventory traffic writes the
	    persisted seen list and no other suite's kills load the guide, unless
	    a spec sets a guide here; the handlers then decide over it. Hosting
	    stays off under automation either way. Null restores inert. */
	void SetPickupCardGuideForTesting(UWoTAngrealGuide* Guide)
	{
		PickupCardTestGuide = Guide;
	}
	/** Cards the handler decided to show since startup, hosted or not. */
	int32 GetPickupCardsShownForTesting() const { return PickupCardsShown; }
	/** Death recaps the handler decided to show since startup, hosted or not. */
	int32 GetDeathRecapsShownForTesting() const { return DeathRecapsShown; }

private:
	void RefreshMenuRowRegistration();
	/** Binds the seam handlers while the master switch is on and unbinds them
	    while it is off, so a switched-off plugin holds no seam binding. */
	void RefreshSeamBindings();
	void UnbindSeamHandlers();
	void HandleShowGuideRequested();
	void HandleCloseClientWindows(FName Keeper);
	void HandleSettingsChanged();
	void HandleClientItemAdded(FName ClassName);
	void HandleClientKilledBy(FName ClassName);
	void ToggleGuideBrowser();
	void OpenGuideBrowser();
	void CloseGuideBrowser();
	/** The imported guide, loaded on first use and kept for the session;
	    null when the content tree does not carry it (asked once). */
	UWoTAngrealGuide* ResolveGuide();
	void ShowPickupCard(const FWoTAngrealGuideRow& Row);
	void ShowDeathRecap(const UWoTAngrealGuide& Guide, const FWoTAngrealGuideRow& Row);
	/** Puts a card on the viewport for ClassName, replacing any card already
	    up. Callers check CanHostCard first. */
	void HostCard(const FWoTPickupCardText& Text, FName ClassName,
		bool bDeathRecap);
	/** A real local viewport, never under automation (specs construct the
	    card widget directly). */
	static bool CanHostCard();
	void ClosePickupCard();
	/** Closes the card on the next tick (its expiry fires mid-paint). */
	void ClosePickupCardDeferred();

	FDelegateHandle ShowGuideHandle;
	FDelegateHandle CloseWindowsHandle;
	FDelegateHandle SettingsChangedHandle;
	FDelegateHandle ItemAddedHandle;
	FDelegateHandle KilledByHandle;
	TSharedPtr<SWoTGuideBrowser> GuideBrowser;
	TWeakObjectPtr<UGameViewportClient> GuideViewport;

	TStrongObjectPtr<UWoTAngrealGuide> CachedGuide;
	bool bGuideAbsent = false;

	TSharedPtr<SWoTPickupCard> PickupCard;
	TWeakObjectPtr<UGameViewportClient> PickupCardViewport;
	/** The class the visible card (a pickup card or a death recap) shows;
	    None without a card. */
	FName PickupCardClass;
	/** Whether the visible card is a death recap rather than a pickup card. */
	bool bCardIsDeathRecap = false;
	FTSTicker::FDelegateHandle PickupCardCloseTicker;
	TWeakObjectPtr<UWoTAngrealGuide> PickupCardTestGuide;
	int32 PickupCardsShown = 0;
	int32 DeathRecapsShown = 0;
};
