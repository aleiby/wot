#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"
#include "UObject/WeakObjectPtrTemplates.h"

class SWoTGuideBrowser;
class UGameViewportClient;

// Runtime module for the optional onboarding aids
// (docs/plans/2026-11-06-onboarding-aids.md). Task 3 carried the guide data
// asset, Task 4 the persisted settings and their seam rows; Task 5 adds the
// guide browser: ShowGuideRequested toggles it, a Field Guide menu row opens
// it from the front-end and in-game menus, and the row itself follows the
// master toggle live (the settings rows stay unconditional — they are the way
// back on; the guide row is an aid). The pickup card and death recap arrive
// with Tasks 6-7.
class WOTONBOARDING_API FWoTOnboardingModule final : public IModuleInterface
{
public:
	/** The registrar tag carried by every seam row this module adds. */
	static FName SettingsRowOwner();

	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/** True while the guide browser is on the viewport. */
	bool IsGuideBrowserOpen() const { return GuideBrowser.IsValid(); }

private:
	void RefreshMenuRowRegistration();
	void HandleShowGuideRequested();
	void HandleCloseClientWindows(FName Keeper);
	void HandleSettingsChanged();
	void ToggleGuideBrowser();
	void OpenGuideBrowser();
	void CloseGuideBrowser();

	FDelegateHandle ShowGuideHandle;
	FDelegateHandle CloseWindowsHandle;
	FDelegateHandle SettingsChangedHandle;
	TSharedPtr<SWoTGuideBrowser> GuideBrowser;
	TWeakObjectPtr<UGameViewportClient> GuideViewport;
};
