#pragma once

#include "CoreMinimal.h"

// The onboarding extension seam (docs/plans/2026-11-06-onboarding-aids.md,
// Architecture §2): the one surface the game module exposes to the optional
// WoTOnboarding plugin, and the seed of a general improvement-module surface.
// Plain types only — a consumer binds these without linking any game class —
// and every seam is inert when unbound: the game module broadcasts and
// registers through them whether or not anything listens, and a build without
// the plugin behaves identically to one with it disabled.

/** An item class newly present in the owning client's inventory. The name is
    the shipped script class name (the HandSet item name), the same key the
    guide rows and retail-text sections use. */
DECLARE_MULTICAST_DELEGATE_OneParam(FWoTClientItemAdded, FName /*ClassName*/);

/** A 'WOTDeathMessage' line the owning client just queued to its right-hand
    message column. */
DECLARE_MULTICAST_DELEGATE_OneParam(FWoTClientDeathMessage, const FString& /*Line*/);

/** The player asked to open the field guide (input wiring lands in Task 4). */
DECLARE_MULTICAST_DELEGATE(FWoTShowGuideRequested);

/** One registered extra row for the menus or the settings screen. Both
    callbacks are invoked lazily — the label at draw time (display text may
    resolve late), the action on activation. */
struct FWoTOnboardingRow
{
	TFunction<FString()> Label;
	TFunction<void()> Invoke;
};

struct WOT_API FWoTOnboardingSeams
{
	/** The process-wide seam registry. */
	static FWoTOnboardingSeams& Get();

	FWoTClientItemAdded ClientItemAdded;
	FWoTClientDeathMessage ClientDeathMessage;
	FWoTShowGuideRequested ShowGuideRequested;

	/** Extra rows appended to the front-end and in-game menus, and to the
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
};
