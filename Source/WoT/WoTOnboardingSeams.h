#pragma once

#include "CoreMinimal.h"
#include "Fonts/SlateFontInfo.h"

class SWidget;
class UTexture2D;

// The onboarding extension seam (docs/plans/2026-11-06-onboarding-aids.md,
// Architecture §2): the one surface the game module exposes to the optional
// WoTOnboarding plugin, and the seed of a general improvement-module surface.
// Plain types only, so a consumer binds these without linking any game class,
// and every seam is inert when unbound: the game module broadcasts and
// registers through them whether or not anything listens, and a build without
// the plugin behaves identically to one with it disabled.

/** An item class newly present in the owning client's inventory. The name is
    the shipped script class name (the HandSet item name), the same key the
    guide rows and retail-text sections use. */
DECLARE_MULTICAST_DELEGATE_OneParam(FWoTClientItemAdded, FName /*ClassName*/);

/** The item that killed the owning client's player, as the server named it
    (WoTKillingItem): the shipped script class name, the same key as above.
    The server sends it whether or not anything on the client listens. */
DECLARE_MULTICAST_DELEGATE_OneParam(FWoTClientKilledBy, FName /*ClassName*/);

/** The player asked to open the field guide (input wiring lands in Task 4). */
DECLARE_MULTICAST_DELEGATE(FWoTShowGuideRequested);

/** A client window that wants the screen to itself is about to open (the
    original kept one WOTWindow at a time: opening one removed the active
    window first). Keeper names the window that is opening and therefore
    stays; every other listener closes its own. The game module broadcasts
    with its window names (ItemInfo, Help, InGameMenu) and, for gameplay
    teardown, NAME_None (nothing stays); it closes its retail windows only
    for a Keeper it does not know, so a build with nothing foreign bound
    behaves exactly as before. */
DECLARE_MULTICAST_DELEGATE_OneParam(FWoTCloseClientWindows, FName /*Keeper*/);

/** The faces the game module publishes so an improvement module's windows
    can draw in the original game's type without linking any game class.
    Resolved per open: the faces are revocable payload data, so a resolver
    answering unset means "draw with your own fallback". */
struct FWoTOnboardingMenuChrome
{
	static constexpr int32 MaxPixelScale = 8;

	/** One of the original game's bitmap faces as the exact PixelScale strike,
	    from 1 to MaxPixelScale, sized to draw 1:1 where one Slate unit covers
	    PixelScale screen pixels. Size is the face's native line height times
	    72/96 points, whatever the strike. A consumer drawing at another scale
	    rescales Size itself. Answering a font without a face means "draw this
	    face with your own fallback". */
	TFunction<FSlateFontInfo(FName /*Face*/, int32 /*PixelScale*/)> PixelFont;
	/** Ties a widget drawing these faces to their revocable generation (the
	    widget collapses if the payload is revoked). */
	TFunction<void(const TSharedRef<SWidget>&)> BindWidget;
};

/** One registered extra row for the menus or the settings screen. Both
    callbacks are invoked lazily: the label at draw time (display text may
    resolve late), the action on activation. */
struct FWoTOnboardingRow
{
	TFunction<FString()> Label;
	TFunction<void()> Invoke;
	/** Registrar identity: a module tags the rows it adds so its shutdown can
	    remove exactly those, keeping registration restart-safe. */
	FName Owner;
};

struct WOT_API FWoTOnboardingSeams
{
	/** The process-wide seam registry. */
	static FWoTOnboardingSeams& Get();

	FWoTClientItemAdded ClientItemAdded;
	FWoTClientKilledBy ClientKilledBy;
	FWoTShowGuideRequested ShowGuideRequested;
	FWoTCloseClientWindows CloseClientWindows;

	/** Extra rows added to the front-end and in-game menus, and to the
	    settings screen (consumed by Tasks 4-5; empty lists cost nothing). */
	TArray<FWoTOnboardingRow> MenuRows;
	TArray<FWoTOnboardingRow> SettingsRows;

	/** Retail display text by (file, section, property), the UE1 localization
	    address. The game module installs this at startup wrapping its runtime
	    retail-text mechanism; that mechanism resolves the owning file from the
	    section (a section names exactly one bound package), so File documents
	    the address and validates nothing today. Unbound or unresolved -> empty. */
	TFunction<FString(const FString& File, const FString& Section,
		const FString& Property)> ResolveRetailText;

	/** ResolveRetailText through the unbound-safe gate: empty when no
	    resolver is installed or the address does not resolve. */
	FString ResolveText(const FString& File, const FString& Section,
		const FString& Property) const;

	/** The original game's faces, installed by the game module at startup.
	    Unset (or answering unset while the guarded faces are unavailable)
	    means the caller draws with its own fallback. */
	TFunction<TOptional<FWoTOnboardingMenuChrome>()> ResolveMenuChrome;

	/** In-match UI input hosting for an improvement module's window: true
	    enters UI-only input with the cursor shown and focus on the given
	    widget; false restores game input (what the retail overlays do through
	    their owning controller). Installed by the local gameplay controller
	    while it lives; unbound it is a no-op, since the front end already
	    runs UI input and a window there manages focus itself. */
	TFunction<void(bool /*bOverlayOpen*/, TSharedPtr<SWidget> /*FocusWidget*/)>
		SetOverlayInput;

	/** Whether a retail window that wants the screen to itself is open (item
	    info, help, the in-game menu). A passive overlay, such as the
	    first-pickup card, stays off the screen while one is. Installed by the
	    local gameplay controller while it lives; unbound reads as nothing
	    open. */
	TFunction<bool()> IsClientWindowOpen;

	/** The original UI art by name: InfoBoxC1, InfoBoxC2 and IconMask (the
	    item-info window's frame, gem bar, dividers and icon bevel) and
	    StatusIconFrame (the HUD's gold selected-slot frame). Installed by the
	    game module at startup and resolved the way the HUD resolves its art,
	    so a release reads it from the player's own files. bSmooth asks for
	    bilinear filtering, for art drawn at a size that is not a whole
	    multiple of its own; otherwise it is filtered as the HUD filters it,
	    so whole-number scales stay crisp (a build that loads cooked assets
	    keeps each asset's own filtering). Unbound, an unknown name, or
	    unresolved -> null. The caller keeps what it draws alive. */
	TFunction<UTexture2D*(FName /*Name*/, bool /*bSmooth*/)> ResolveRetailTexture;

	/** A shipped item class's inventory icon (its StatusIcon), by the same
	    class-name key as ClientItemAdded, filtered as ResolveRetailTexture
	    describes. Installed by the game module at startup. Unbound or
	    unresolved -> null. */
	TFunction<UTexture2D*(FName /*ClassName*/, bool /*bSmooth*/)> ResolveItemIcon;

	/** The shipped class name of the item in the local player's selected
	    hand (the HandSet item name), or NAME_None. Installed by the local
	    gameplay controller while it lives; unbound reads as nothing
	    selected. */
	TFunction<FName()> SelectedItemClass;

	/** The shipped class name of the item that killed the local player, while
	    the player waits to respawn (the controller's Dying state), or
	    NAME_None: alive, respawned, or killed by nothing the server could
	    name. Installed by the local gameplay controller while it lives;
	    unbound reads as none. */
	TFunction<FName()> KilledByItemClass;
};
