#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "WoTOnboardingSettings.generated.h"

// Persisted onboarding-aid toggles (docs/plans/2026-11-06-onboarding-aids.md,
// Task 4). The plugin keeps its own GameUserSettings section, so a parity
// build that disables or omits the plugin carries no trace of it. All three
// default on by design: the aids exist exactly for the player who does not
// yet know to look for them. The master switch gates every aid's BEHAVIOR
// (the handlers of Tasks 5-7); it never hides the settings rows themselves,
// which are the only in-game way to turn the aids back on.
UCLASS(Config=GameUserSettings)
class WOTONBOARDING_API UWoTOnboardingSettings : public UObject
{
	GENERATED_BODY()

public:
	/** The compiled-in defaults: every aid ships enabled. */
	static constexpr bool bDefaultEnableOnboardingAids = true;
	static constexpr bool bDefaultShowFirstPickupCards = true;
	static constexpr bool bDefaultShowDeathCounterHints = true;

	/** Master switch: off suppresses every onboarding aid. */
	UPROPERTY(Config)
	bool bEnableOnboardingAids = bDefaultEnableOnboardingAids;

	/** First-pickup info cards (handler arrives with Task 6). */
	UPROPERTY(Config)
	bool bShowFirstPickupCards = bDefaultShowFirstPickupCards;

	/** Death-recap counter hints (handler arrives with Task 7). */
	UPROPERTY(Config)
	bool bShowDeathCounterHints = bDefaultShowDeathCounterHints;

	/** The live, persisted settings: the config-backed class default object. */
	static UWoTOnboardingSettings& Get();

	/** Set/Toggle write through SaveConfig and then broadcast OnChanged, so
	    every consumer observes one authority. Setting the current value is a
	    no-op: no save, no broadcast. Call these on Get(); a detached instance
	    would persist into the same shared section. */
	void SetEnableOnboardingAids(bool bValue);
	void SetShowFirstPickupCards(bool bValue);
	void SetShowDeathCounterHints(bool bValue);
	void ToggleEnableOnboardingAids();
	void ToggleShowFirstPickupCards();
	void ToggleShowDeathCounterHints();

	/** Fires after any persisted change. The settings rows re-read live values
	    each draw and need no subscription; this is for the aid handlers of
	    Tasks 5-7, which tear widgets down or up when a toggle flips. */
	FSimpleMulticastDelegate OnChanged;

private:
	void Commit(bool& Field, bool bValue);
};
