#include "WoTVRSession.h"

#include "WoTVRRig.h"

#include "Components/SceneComponent.h"
#include "Engine/Engine.h"
#include "GameFramework/PlayerController.h"
#include "IXRCamera.h"
#include "IXRTrackingSystem.h"
#include "StereoRendering.h"

namespace
{
/** The engine's XR system while it renders in stereo, else null. */
IXRTrackingSystem* WoTVREngineStereoSystem()
{
	if (!GEngine || !GEngine->XRSystem.IsValid() || !GEngine->StereoRenderingDevice.IsValid()
		|| !GEngine->StereoRenderingDevice->IsStereoEnabled())
	{
		return nullptr;
	}
	return GEngine->XRSystem.Get();
}
}

bool IWoTVRSession::GetEyeOffsetMeters(int32 Eye, FVector& OutOffset) const
{
	OutOffset = FVector::ZeroVector;
	if (Eye != EStereoscopicEye::eSSE_LEFT_EYE && Eye != EStereoscopicEye::eSSE_RIGHT_EYE)
	{
		return false;
	}
	if (IXRTrackingSystem* XR = WoTVREngineStereoSystem())
	{
		// The eye pose relative to the head that the XR camera adds to each eye
		// (FDefaultXRCamera::CalculateStereoCameraOffset), at the engine's scale.
		FQuat Orientation = FQuat::Identity;
		FVector Position = FVector::ZeroVector;
		const float Scale = XR->GetWorldToMetersScale();
		if (Scale > 0.0f && XR->GetRelativeEyePose(IXRTrackingSystem::HMDDeviceId, Eye, Orientation, Position))
		{
			OutOffset = Position / Scale;
			return true;
		}
	}
	OutOffset = FVector(0.0, (Eye == EStereoscopicEye::eSSE_LEFT_EYE ? -0.5 : 0.5) * GetInterpupillaryDistanceMeters(), 0.0);
	return true;
}

FVector IWoTVRSession::GetEngineAddedHeadOffset(APlayerController* Controller, const TOptional<double>& TrackingYaw)
{
	IXRTrackingSystem* XR = WoTVREngineStereoSystem();
	if (!XR)
	{
		return FVector::ZeroVector;
	}
	// FDefaultXRCamera::CalculateStereoCameraOffset adds nothing to an eye it
	// cannot place.
	FQuat Orientation = FQuat::Identity;
	FVector Position = FVector::ZeroVector;
	if (!XR->GetRelativeEyePose(IXRTrackingSystem::HMDDeviceId, EStereoscopicEye::eSSE_LEFT_EYE, Orientation, Position))
	{
		return FVector::ZeroVector;
	}
	// Otherwise it adds the device position, whatever its validity, turned by
	// the XR camera's control yaw.
	FQuat DeviceOrientation = FQuat::Identity;
	FVector DevicePosition = FVector::ZeroVector;
	const bool bTracked = XR->GetCurrentPose(IXRTrackingSystem::HMDDeviceId, DeviceOrientation, DevicePosition);
	double ControlYaw = EngineControlYaw;
	const TSharedPtr<IXRCamera, ESPMode::ThreadSafe> Camera = XR->GetXRCamera();
	if (Camera.IsValid() && Controller && bTracked)
	{
		// The yaw's only setter (FDefaultXRCamera::ApplyHMDRotation) adds the
		// given rotation's yaw turn from the controller's control rotation, never
		// pitch or roll, and hands back the head's orientation under the result.
		// Given the control rotation itself it turns nothing: a read. The engine
		// calls it only from APlayerController::UpdateRotation: the game's
		// controller skips that while the rig owns the aim, and the front end's
		// controller has no look input, so its call is a read too.
		FRotator Read = Controller->GetControlRotation();
		Camera->ApplyHMDRotation(Controller, Read);
		ControlYaw = (Read.Quaternion() * DeviceOrientation.Inverse()).Rotator().Yaw;
		const double Turn = TrackingYaw.IsSet() ? FMath::FindDeltaAngleDegrees(ControlYaw, *TrackingYaw) : 0.0;
		if (!FMath::IsNearlyZero(Turn, 1e-6))
		{
			FRotator Turned = Controller->GetControlRotation();
			Turned.Yaw += Turn;
			Camera->ApplyHMDRotation(Controller, Turned);
			ControlYaw = (Turned.Quaternion() * DeviceOrientation.Inverse()).Rotator().Yaw;
		}
		EngineControlYaw = ControlYaw;
	}
	return FRotator(0.0, ControlYaw, 0.0).RotateVector(DevicePosition);
}

void IWoTVRSession::KeepLateUpdate(USceneComponent& Anchor)
{
	IXRTrackingSystem* XR = WoTVREngineStereoSystem();
	const TSharedPtr<IXRCamera, ESPMode::ThreadSafe> Camera = XR ? XR->GetXRCamera() : nullptr;
	if (Camera.IsValid())
	{
		// The anchor has no primitives, so nothing else is late-updated with it.
		Camera->SetupLateUpdate(FTransform::Identity, &Anchor, false);
	}
}

IStereoLayers* IWoTVRSession::GetStereoLayers() const
{
	return GEngine && GEngine->StereoRenderingDevice.IsValid()
		? GEngine->StereoRenderingDevice->GetStereoLayers() : nullptr;
}

float IWoTVRSession::GetWorldToMetersScale() const
{
	const float Scale = GEngine && GEngine->XRSystem.IsValid()
		? GEngine->XRSystem->GetWorldToMetersScale() : 0.0f;
	return Scale > 0.0f ? Scale : static_cast<float>(WoTVR::UnitsPerMeter);
}
