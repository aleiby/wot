#pragma once

#include "CoreMinimal.h"

#if WITH_AUTOMATION_TESTS

#include "Engine/EngineBaseTypes.h"
#include "WoTVRSession.h"

class UWorld;

/** A session specs drive: whether an XR system exists, the session state, the
 * head pose and its validity, session loss and recovery, reference-space
 * resets and interaction profiles. No runtime is touched.
 *
 * It also models what the engine's OpenXR plugin does around a session that
 * the specs must survive: a stopped session that takes a few frames to end
 * (no second session can start meanwhile), a session the runtime ends while
 * stereo is on (it stays gone until stereo starts again), and, on request, the
 * plugin's own frame-rate writes. It advances once per world tick. */
class FWoTVRSyntheticSession final : public IWoTVRSession
{
public:
	/** An XR system with a headset exists (stereo can be requested). */
	bool bSystemAvailable = true;
	/** A stereo request starts the session at once (Running); otherwise it stays
	 * Starting until a spec sets Running. */
	bool bStartsImmediately = true;
	/** Stereo requests fail. */
	bool bFailStereo = false;
	EWoTVRSessionState RunningState = EWoTVRSessionState::Running;
	FWoTVRHeadPose Pose;
	float InterpupillaryDistanceMeters = 0.064f;
	int32 StereoOnRequests = 0;
	int32 StereoOffRequests = 0;
	int32 PrepareTrackingCalls = 0;
	FWoTVRDiagnostics Diagnostics;
	/** World ticks a stopped session takes to end, as OpenXR's runtime-driven
	 * STOPPING and EXITING events take a few frames after stereo stops. Zero
	 * ends it at once. While it ends, a stereo request fails, as the engine's
	 * second xrCreateSession is refused (FOpenXRHMD::OnStereoStartup). */
	int32 EndingFrames = 0;
	/** Reproduce the engine OpenXR plugin's frame-rate writes: stereo on uncaps
	 * the rate and turns smoothing off (FOpenXRHMD::EnableStereo), stereo off
	 * turns smoothing back on, a stopping session caps the rate at
	 * xr.OpenXRPausedIdleFPS and an exiting one uncaps it again
	 * (FOpenXRHMD::OnStartGameFrame). */
	bool bEmulateOpenXRFrameRate = false;

	FWoTVRSyntheticSession();
	virtual ~FWoTVRSyntheticSession() override;

	/** A tracked head at PositionMeters looking along Rotation (degrees). */
	void SetHead(const FVector& PositionMeters, const FRotator& Rotation);
	void SetTracked(bool bTracked) { Pose.bValid = bTracked; }
	/** A transient loss the session recovers from by itself (state Starting, as
	 * while the runtime has paused it). */
	void LoseSession() { bLost = true; }
	void RecoverSession() { bLost = false; }
	/** The runtime ends the session while stereo is on (OpenXR's EXITING or
	 * LOSS_PENDING): it stays gone until stereo starts again. bLoseSystem also
	 * loses the XR system (the headset disconnected) until OfferSystem. */
	void EndSessionByRuntime(bool bLoseSystem);
	/** The runtime offers an XR system again (OpenXR re-acquires it each frame). */
	void OfferSystem() { bSystemAvailable = true; }
	void ResetReferenceSpace() { ++ReferenceGeneration; }
	void ChangeInteractionProfile(const FString& Left, const FString& Right);
	/** A previous session is still shutting down. */
	bool IsEnding() const { return EndingLeft > 0; }

	virtual FName GetKind() const override;
	virtual EWoTVRSessionState GetState() const override;
	virtual bool GetHeadPose(FWoTVRHeadPose& OutPose) const override;
	virtual float GetInterpupillaryDistanceMeters() const override { return InterpupillaryDistanceMeters; }
	virtual bool SetStereo(bool bEnable) override;
	virtual bool IsStereoEnabled() const override { return bStereo; }
	virtual void PrepareTracking() override { ++PrepareTrackingCalls; }
	virtual uint32 GetReferenceSpaceGeneration() const override { return ReferenceGeneration; }
	virtual uint32 GetInteractionProfileGeneration() const override { return ProfileGeneration; }
	virtual FWoTVRDiagnostics GetDiagnostics() const override { return Diagnostics; }

	/** Spec control of the started state when bStartsImmediately is false. */
	void SetSessionStarted(bool bStarted) { bSessionStarted = bStarted; }

private:
	void HandleWorldTickStart(UWorld* World, ELevelTick TickType, float DeltaSeconds);
	static float PausedIdleFPS();

	bool bStereo = false;
	bool bSessionStarted = false;
	bool bLost = false;
	bool bEndedByRuntime = false;
	int32 EndingLeft = 0;
	uint32 ReferenceGeneration = 0;
	uint32 ProfileGeneration = 0;
	FDelegateHandle WorldTickHandle;
};

#endif // WITH_AUTOMATION_TESTS
