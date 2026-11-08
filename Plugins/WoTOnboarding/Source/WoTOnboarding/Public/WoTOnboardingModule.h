#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

// Runtime module for the optional onboarding aids
// (docs/plans/2026-11-06-onboarding-aids.md). Task 3 carries only the guide
// data asset; the handlers that bind the game module's seams arrive with
// Tasks 4-7.
class FWoTOnboardingModule final : public IModuleInterface
{
};
