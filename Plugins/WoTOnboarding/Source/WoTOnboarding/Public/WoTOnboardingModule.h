#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

// Runtime module for the optional onboarding aids
// (docs/plans/2026-11-06-onboarding-aids.md). Task 3 carried the guide data
// asset; Task 4 adds the persisted settings and registers their rows into the
// game module's settings-row seam. The handlers that bind the remaining seams
// (guide browser, pickup card, death recap) arrive with Tasks 5-7, so
// ShowGuideRequested stays unbound for now.
class FWoTOnboardingModule final : public IModuleInterface
{
public:
	/** The registrar tag carried by every seam row this module adds. */
	static FName SettingsRowOwner();

	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
