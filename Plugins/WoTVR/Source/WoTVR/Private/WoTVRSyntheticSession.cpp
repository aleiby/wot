#include "WoTVRSyntheticSession.h"

#if WITH_AUTOMATION_TESTS

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"

FWoTVRSyntheticSession::FWoTVRSyntheticSession()
{
	Pose.bValid = true;
	Diagnostics.bAvailable = true;
	Diagnostics.RuntimeName = TEXT("Synthetic runtime");
	Diagnostics.RuntimeVersion = TEXT("1.0.0");
	Diagnostics.SystemName = TEXT("Synthetic headset");
	Diagnostics.EnabledExtensions = {TEXT("XR_KHR_synthetic_one"), TEXT("XR_KHR_synthetic_two")};
	Diagnostics.LeftHandProfile = TEXT("none");
	Diagnostics.RightHandProfile = TEXT("none");
	WorldTickHandle = FWorldDelegates::OnWorldTickStart.AddRaw(this, &FWoTVRSyntheticSession::HandleWorldTickStart);
}

FWoTVRSyntheticSession::~FWoTVRSyntheticSession()
{
	FWorldDelegates::OnWorldTickStart.Remove(WorldTickHandle);
}

void FWoTVRSyntheticSession::SetHead(const FVector& PositionMeters, const FRotator& Rotation)
{
	Pose.PositionMeters = PositionMeters;
	Pose.Orientation = Rotation.Quaternion();
	Pose.bValid = true;
}

void FWoTVRSyntheticSession::EndSessionByRuntime(bool bLoseSystem)
{
	if (bStereo)
	{
		bEndedByRuntime = true;
		if (bEmulateOpenXRFrameRate && GEngine)
		{
			GEngine->SetMaxFPS(0.0f);
		}
	}
	if (bLoseSystem)
	{
		bSystemAvailable = false;
	}
}

void FWoTVRSyntheticSession::ChangeInteractionProfile(const FString& Left, const FString& Right)
{
	Diagnostics.LeftHandProfile = Left;
	Diagnostics.RightHandProfile = Right;
	++ProfileGeneration;
}

FName FWoTVRSyntheticSession::GetKind() const
{
	return TEXT("Synthetic");
}

EWoTVRSessionState FWoTVRSyntheticSession::GetState() const
{
	if (!bSystemAvailable)
	{
		return EWoTVRSessionState::Unavailable;
	}
	if (!bStereo)
	{
		return EndingLeft > 0 ? EWoTVRSessionState::Ending : EWoTVRSessionState::Idle;
	}
	if (bEndedByRuntime)
	{
		return EWoTVRSessionState::Lost;
	}
	if (bLost || !(bStartsImmediately || bSessionStarted))
	{
		return EWoTVRSessionState::Starting;
	}
	return RunningState;
}

bool FWoTVRSyntheticSession::GetHeadPose(FWoTVRHeadPose& OutPose) const
{
	const bool bValid = bSystemAvailable && bStereo && !bLost && !bEndedByRuntime && Pose.bValid;
	OutPose = Pose;
	OutPose.bValid = bValid;
	return bValid;
}

bool FWoTVRSyntheticSession::SetStereo(bool bEnable)
{
	if (bEnable)
	{
		++StereoOnRequests;
		if (bStereo)
		{
			// As FOpenXRHMD::EnableStereo: already on, nothing to do, even when
			// the runtime has ended the session underneath.
			return true;
		}
		if (!bSystemAvailable || bFailStereo || EndingLeft > 0)
		{
			return false;
		}
		bStereo = true;
		bEndedByRuntime = false;
		if (bEmulateOpenXRFrameRate && GEngine)
		{
			GEngine->bForceDisableFrameRateSmoothing = true;
			GEngine->SetMaxFPS(0.0f);
		}
		return true;
	}
	++StereoOffRequests;
	if (bStereo)
	{
		// A session the runtime already ended has nothing left to shut down.
		EndingLeft = bEndedByRuntime ? 0 : FMath::Max(0, EndingFrames);
		bStereo = false;
		bEndedByRuntime = false;
		if (bEmulateOpenXRFrameRate && GEngine)
		{
			GEngine->bForceDisableFrameRateSmoothing = false;
		}
	}
	return true;
}

float FWoTVRSyntheticSession::PausedIdleFPS()
{
	const IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(TEXT("xr.OpenXRPausedIdleFPS"));
	return Variable ? Variable->GetFloat() : 10.0f;
}

void FWoTVRSyntheticSession::HandleWorldTickStart(UWorld*, ELevelTick, float)
{
	if (EndingLeft <= 0)
	{
		return;
	}
	if (bEmulateOpenXRFrameRate && GEngine && EndingLeft == FMath::Max(1, EndingFrames))
	{
		// XR_SESSION_STATE_STOPPING.
		GEngine->SetMaxFPS(PausedIdleFPS());
	}
	if (--EndingLeft == 0 && bEmulateOpenXRFrameRate && GEngine)
	{
		// XR_SESSION_STATE_EXITING: the session is destroyed.
		GEngine->SetMaxFPS(0.0f);
	}
}

#endif // WITH_AUTOMATION_TESTS
