#include "WoTFirstPersonCameraModifier.h"
#include "WoTFirstPersonPresenter.h"
#include "WoTFirstPersonSettings.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/SceneComponent.h"

UWoTFirstPersonCameraModifier::UWoTFirstPersonCameraModifier()
{
	Priority = 255;
}

bool UWoTFirstPersonCameraModifier::ModifyCamera(float DeltaTime, FMinimalViewInfo& InOutPOV)
{
	if (Presenter)
	{
		const TOptional<float> Scale = Presenter->UpdateModel(DeltaTime, InOutPOV);
		if (Scale.IsSet())
		{
			InOutPOV.bUseFirstPersonParameters = true;
			InOutPOV.FirstPersonFOV = UWoTFirstPersonSettings::Get().ModelFieldOfView;
			InOutPOV.FirstPersonScale = Scale.GetValue();
			// The framing assumes this near plane while the model shows.
			const float Near = static_cast<float>(WoTFirstPerson::ShownNearPlane());
			if (InOutPOV.GetFinalPerspectiveNearClipPlane() > Near)
			{
				InOutPOV.PerspectiveNearClipPlane = Near;
			}
		}
	}
	return false;
}

FWoTFirstPersonFlatMode::~FWoTFirstPersonFlatMode()
{
	Hide();
}

FWoTFirstPersonAnchor FWoTFirstPersonFlatMode::Anchor(APlayerCameraManager* Camera, EWoTFirstPersonHand Hand) const
{
	FWoTFirstPersonAnchor Result;
	if (Camera && Hand == EWoTFirstPersonHand::Primary) Result.Parent = Camera->GetTransformComponent();
	return Result;
}

void FWoTFirstPersonFlatMode::Show(APlayerCameraManager* Camera, FWoTFirstPersonPresenter& Presenter)
{
	if (Manager.Get() == Camera && Modifier.IsValid()) return;
	Hide();
	if (!Camera) return;
	Manager = Camera;
	UWoTFirstPersonCameraModifier* Added = Cast<UWoTFirstPersonCameraModifier>(
		Camera->AddNewCameraModifier(UWoTFirstPersonCameraModifier::StaticClass()));
	if (Added)
	{
		Added->Presenter = &Presenter;
		Modifier = Added;
	}
}

void FWoTFirstPersonFlatMode::Hide()
{
	if (UWoTFirstPersonCameraModifier* Existing = Modifier.Get())
	{
		Existing->Presenter = nullptr;
		if (APlayerCameraManager* Camera = Manager.Get()) Camera->RemoveCameraModifier(Existing);
	}
	Modifier.Reset();
	Manager.Reset();
}
