#pragma once

#include "Camera/CameraModifier.h"
#include "WoTFirstPersonAnchor.h"
#include "WoTFirstPersonCameraModifier.generated.h"

class FWoTFirstPersonPresenter;

UCLASS()
class WOTFIRSTPERSON_API UWoTFirstPersonCameraModifier : public UCameraModifier
{
	GENERATED_BODY()
public:
	UWoTFirstPersonCameraModifier();
	virtual bool ModifyCamera(float DeltaTime, FMinimalViewInfo& InOutPOV) override;
	// Flat mode clears this before removing the modifier or destroying its owner.
	FWoTFirstPersonPresenter* Presenter = nullptr;
};

// Owns the projection policy and camera anchor, separately from model hosting.
class FWoTFirstPersonFlatMode
{
public:
	~FWoTFirstPersonFlatMode();
	FWoTFirstPersonAnchor Anchor(APlayerCameraManager* Camera, EWoTFirstPersonHand Hand) const;
	void Show(APlayerCameraManager* Camera, FWoTFirstPersonPresenter& Presenter);
	void Hide();
private:
	TWeakObjectPtr<APlayerCameraManager> Manager;
	TWeakObjectPtr<UWoTFirstPersonCameraModifier> Modifier;
};
