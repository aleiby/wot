#include "WoTVROpenXRSession.h"

#include "Engine/Engine.h"
#include "HAL/IConsoleManager.h"
#include "HeadMountedDisplayTypes.h"
#include "IHeadMountedDisplay.h"
#include "IOpenXRHMD.h"
#include "IXRTrackingSystem.h"
#include "Misc/CoreDelegates.h"
#include "OpenXRCore.h"
#include "StereoRendering.h"
#include "XRTrackingSystemBase.h"

const TCHAR* LexToString(EWoTVRSessionState State)
{
	switch (State)
	{
	case EWoTVRSessionState::Unavailable: return TEXT("unavailable");
	case EWoTVRSessionState::Idle: return TEXT("idle (stereo off)");
	case EWoTVRSessionState::Ending: return TEXT("ending the previous session");
	case EWoTVRSessionState::Starting: return TEXT("starting");
	case EWoTVRSessionState::Visible: return TEXT("running without focus");
	case EWoTVRSessionState::Running: return TEXT("running");
	case EWoTVRSessionState::Lost: return TEXT("ended by the runtime");
	}
	return TEXT("unknown");
}

FWoTVROpenXRSession::FWoTVROpenXRSession()
{
	// The runtime changed or reset the reference space (OpenXR's
	// REFERENCE_SPACE_CHANGE_PENDING): any confirmed neutral pose is stale.
	RecenterHandle = FCoreDelegates::VRHeadsetRecenter.AddLambda([this]() { ++ReferenceGeneration; });
	ProfileHandle = FXRTrackingSystemDelegates::OnXRInteractionProfileChanged.AddLambda(
		[this]() { ++ProfileGeneration; });
}

FWoTVROpenXRSession::~FWoTVROpenXRSession()
{
	FCoreDelegates::VRHeadsetRecenter.Remove(RecenterHandle);
	FXRTrackingSystemDelegates::OnXRInteractionProfileChanged.Remove(ProfileHandle);
}

FName FWoTVROpenXRSession::GetKind() const
{
	return TEXT("OpenXR");
}

IXRTrackingSystem* FWoTVROpenXRSession::GetOpenXRSystem()
{
	IXRTrackingSystem* XR = GEngine ? GEngine->XRSystem.Get() : nullptr;
	return XR && XR->GetSystemName() == FName(TEXT("OpenXR")) ? XR : nullptr;
}

IOpenXRHMD* FWoTVROpenXRSession::GetOpenXRHMD()
{
	IXRTrackingSystem* XR = GetOpenXRSystem();
	return XR ? XR->GetIOpenXRHMD() : nullptr;
}

EWoTVRSessionState FWoTVROpenXRSession::GetState() const
{
	IOpenXRHMD* OpenXR = GetOpenXRHMD();
	if (!OpenXR || OpenXR->GetSystem() == XR_NULL_SYSTEM_ID)
	{
		return EWoTVRSessionState::Unavailable;
	}
	// Stereo off asks the runtime to end the session (xrRequestExitSession); the
	// handle lives on until its EXITING event a few frames later, and until then
	// a new session cannot be created (FOpenXRHMD::OnStereoStartup).
	const bool bSession = OpenXR->GetSession() != XR_NULL_HANDLE;
	if (!IsStereoEnabled())
	{
		return bSession ? EWoTVRSessionState::Ending : EWoTVRSessionState::Idle;
	}
	// Stereo on without a session: the runtime ended it (EXITING or
	// LOSS_PENDING destroy the session and leave stereo on).
	if (!bSession)
	{
		return EWoTVRSessionState::Lost;
	}
	if (!OpenXR->IsRunning())
	{
		return EWoTVRSessionState::Starting;
	}
	return OpenXR->IsFocused() ? EWoTVRSessionState::Running : EWoTVRSessionState::Visible;
}

bool FWoTVROpenXRSession::GetHeadPose(FWoTVRHeadPose& OutPose) const
{
	OutPose = FWoTVRHeadPose();
	IXRTrackingSystem* XR = GetOpenXRSystem();
	if (!XR)
	{
		return false;
	}
	FQuat Orientation = FQuat::Identity;
	FVector Position = FVector::ZeroVector;
	// The game thread's snapshot for this frame: the same pose the XR camera
	// records as the view's base for its render-thread late update.
	const bool bValid = XR->GetCurrentPose(IXRTrackingSystem::HMDDeviceId, Orientation, Position);
	const float Scale = XR->GetWorldToMetersScale();
	OutPose.Orientation = Orientation;
	OutPose.PositionMeters = Scale > 0.0f ? Position / Scale : FVector::ZeroVector;
	OutPose.bValid = bValid;
	return bValid;
}

float FWoTVROpenXRSession::GetInterpupillaryDistanceMeters() const
{
	IXRTrackingSystem* XR = GetOpenXRSystem();
	IHeadMountedDisplay* HMD = XR ? XR->GetHMDDevice() : nullptr;
	return HMD ? HMD->GetInterpupillaryDistance() : 0.0f;
}

bool FWoTVROpenXRSession::SetStereo(bool bEnable)
{
	if (!GetOpenXRSystem() || !GEngine || !GEngine->StereoRenderingDevice.IsValid())
	{
		return !bEnable;
	}
	// The engine's OpenXR plugin rewrites the frame-rate cap and smoothing on the
	// way in and out (and again on its session events afterwards); the rig keeps
	// the player's own (UWoTVRPlayerSubsystem's frame-rate guard).
	IStereoRendering& Stereo = *GEngine->StereoRenderingDevice;
	if (bEnable)
	{
		return Stereo.EnableStereo(true) && Stereo.IsStereoEnabled();
	}
	return Stereo.EnableStereo(false);
}

bool FWoTVROpenXRSession::IsStereoEnabled() const
{
	return GetOpenXRSystem() && GEngine && GEngine->StereoRenderingDevice.IsValid()
		&& GEngine->StereoRenderingDevice->IsStereoEnabled();
}

void FWoTVROpenXRSession::PrepareTracking()
{
	// Eye level (Local): the origin confirmed in the owner's headset tests. The
	// rig recentres in its own calibration and never moves this origin.
	if (IXRTrackingSystem* XR = GetOpenXRSystem())
	{
		XR->SetTrackingOrigin(EHMDTrackingOrigin::Local);
	}
}

namespace
{
FString WoTVROpenXRPathString(XrInstance Instance, XrPath Path)
{
	if (Path == XR_NULL_PATH)
	{
		return TEXT("none");
	}
	char Buffer[XR_MAX_PATH_LENGTH] = {};
	uint32_t Length = 0;
	const XrResult Result = xrPathToString(Instance, Path, XR_MAX_PATH_LENGTH, &Length, Buffer);
	return XR_SUCCEEDED(Result) ? FString(UTF8_TO_TCHAR(Buffer))
		: FString::Printf(TEXT("unreadable (%s)"), OpenXRResultToString(Result));
}

FString WoTVROpenXRHandProfile(XrInstance Instance, XrSession Session, const char* Hand)
{
	if (Session == XR_NULL_HANDLE)
	{
		return TEXT("unavailable (no session)");
	}
	XrPath HandPath = XR_NULL_PATH;
	XrResult Result = xrStringToPath(Instance, Hand, &HandPath);
	if (!XR_SUCCEEDED(Result))
	{
		return FString::Printf(TEXT("unavailable (%s)"), OpenXRResultToString(Result));
	}
	XrInteractionProfileState State{XR_TYPE_INTERACTION_PROFILE_STATE};
	Result = xrGetCurrentInteractionProfile(Session, HandPath, &State);
	if (!XR_SUCCEEDED(Result))
	{
		return FString::Printf(TEXT("unavailable (%s)"), OpenXRResultToString(Result));
	}
	return WoTVROpenXRPathString(Instance, State.interactionProfile);
}
}

FWoTVRDiagnostics FWoTVROpenXRSession::GetDiagnostics() const
{
	FWoTVRDiagnostics Result;
	IOpenXRHMD* OpenXR = GetOpenXRHMD();
	const XrInstance Instance = OpenXR ? OpenXR->GetInstance() : XR_NULL_HANDLE;
	if (Instance == XR_NULL_HANDLE)
	{
		return Result;
	}
	Result.bAvailable = true;
	XrInstanceProperties InstanceProperties{XR_TYPE_INSTANCE_PROPERTIES};
	if (XR_SUCCEEDED(xrGetInstanceProperties(Instance, &InstanceProperties)))
	{
		Result.RuntimeName = UTF8_TO_TCHAR(InstanceProperties.runtimeName);
		Result.RuntimeVersion = FString::Printf(TEXT("%d.%d.%d"),
			static_cast<int32>(XR_VERSION_MAJOR(InstanceProperties.runtimeVersion)),
			static_cast<int32>(XR_VERSION_MINOR(InstanceProperties.runtimeVersion)),
			static_cast<int32>(XR_VERSION_PATCH(InstanceProperties.runtimeVersion)));
	}
	const XrSystemId System = OpenXR->GetSystem();
	if (System != XR_NULL_SYSTEM_ID)
	{
		XrSystemProperties SystemProperties{XR_TYPE_SYSTEM_PROPERTIES};
		if (XR_SUCCEEDED(xrGetSystemProperties(Instance, System, &SystemProperties)))
		{
			Result.SystemName = UTF8_TO_TCHAR(SystemProperties.systemName);
		}
	}
	uint32_t Count = 0;
	if (XR_SUCCEEDED(xrEnumerateInstanceExtensionProperties(nullptr, 0, &Count, nullptr)) && Count > 0)
	{
		TArray<XrExtensionProperties> Extensions;
		Extensions.Init(XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES}, static_cast<int32>(Count));
		if (XR_SUCCEEDED(xrEnumerateInstanceExtensionProperties(nullptr, Count, &Count, Extensions.GetData())))
		{
			for (uint32_t Index = 0; Index < Count; ++Index)
			{
				const FString Name = UTF8_TO_TCHAR(Extensions[static_cast<int32>(Index)].extensionName);
				if (OpenXR->IsExtensionEnabled(Name))
				{
					Result.EnabledExtensions.Add(Name);
				}
			}
		}
	}
	Result.EnabledExtensions.Sort();
	const XrSession Session = OpenXR->GetSession();
	Result.LeftHandProfile = WoTVROpenXRHandProfile(Instance, Session, "/user/hand/left");
	Result.RightHandProfile = WoTVROpenXRHandProfile(Instance, Session, "/user/hand/right");
	return Result;
}
