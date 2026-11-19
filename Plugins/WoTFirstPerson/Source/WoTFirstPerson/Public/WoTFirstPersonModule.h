#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

class FWoTFirstPersonPresenter;
class FWoTFirstPersonCaptureScript;

class WOTFIRSTPERSON_API FWoTFirstPersonModule final : public IModuleInterface
{
public:
	static FName SettingsSectionOwner();
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	void RefreshPresenter();

	FDelegateHandle SettingsChangedHandle;
	TSharedPtr<FWoTFirstPersonPresenter> Presenter;
#if !UE_BUILD_SHIPPING
	TSharedPtr<FWoTFirstPersonCaptureScript> CaptureScript;
#endif
};
