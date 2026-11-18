#pragma once

#include "CoreMinimal.h"

// The one interface through which the plugin reaches XR (design section 5.1).
// The rig and the policies never call the engine's XR system directly: an
// implementation backed by stock OpenXR serves real headsets, and a synthetic
// one lets specs drive poses, validity, session loss and recovery and
// reference-space resets without any runtime.

enum class EWoTVRSessionState : uint8
{
	/** No XR system: the gate is closed, no runtime answered, or no headset. */
	Unavailable,
	/** An XR system exists and stereo is off (flat play with XR initialised). */
	Idle,
	/** Stereo is off and the previous session is still shutting down (OpenXR
	 * ends it a few frames after stereo stops). A new session can start only
	 * once it has ended. */
	Ending,
	/** Stereo is on and the runtime has not started the session yet. */
	Starting,
	/** The session runs but the application lacks input focus (a runtime
	 * overlay or dashboard is up). Not usable for play. */
	Visible,
	/** The session runs with input focus: usable. */
	Running,
	/** The runtime ended the session while stereo was on (it quit, left VR or
	 * lost the headset). No session exists until stereo starts again. */
	Lost
};

WOTVR_API const TCHAR* LexToString(EWoTVRSessionState State);

/** A head pose in the runtime's tracking space: metres, in engine axes (X
 * forward, Y right, Z up). The rig converts it to world units once. */
struct FWoTVRHeadPose
{
	FQuat Orientation = FQuat::Identity;
	FVector PositionMeters = FVector::ZeroVector;
	/** Orientation and position are both tracked. */
	bool bValid = false;
};

/** What the first headset session must record (design section 16): the active
 * runtime, the system, the enabled extensions and each hand's current
 * interaction profile. */
struct FWoTVRDiagnostics
{
	bool bAvailable = false;
	FString RuntimeName;
	FString RuntimeVersion;
	FString SystemName;
	TArray<FString> EnabledExtensions;
	FString LeftHandProfile;
	FString RightHandProfile;
};

class WOTVR_API IWoTVRSession
{
public:
	virtual ~IWoTVRSession() = default;

	/** "OpenXR" or "Synthetic", for logs. */
	virtual FName GetKind() const = 0;
	virtual EWoTVRSessionState GetState() const = 0;
	/** An XR system with a headset exists, so stereo can be requested. */
	bool IsSystemAvailable() const { return GetState() != EWoTVRSessionState::Unavailable; }
	/** The session runs with focus: poses can drive play. */
	bool IsUsable() const { return GetState() == EWoTVRSessionState::Running; }

	/** The game-thread head pose of the current frame: stable within a frame,
	 * so the view, the aim and every eye read the same pose. False with no
	 * valid pose. */
	virtual bool GetHeadPose(FWoTVRHeadPose& OutPose) const = 0;
	/** The distance between the eyes in metres (0 when unknown). */
	virtual float GetInterpupillaryDistanceMeters() const = 0;

	/** Stereo rendering on or off: on starts the runtime's session, off ends
	 * it and restores the flat window. False when the request failed. */
	virtual bool SetStereo(bool bEnable) = 0;
	virtual bool IsStereoEnabled() const = 0;

	/** Prepares tracking for play: the eye-level (Local) tracking origin. */
	virtual void PrepareTracking() = 0;

	/** Bumped whenever the runtime resets or changes the reference space, so
	 * the rig can invalidate its calibration until the user recentres. */
	virtual uint32 GetReferenceSpaceGeneration() const = 0;
	/** Bumped whenever a hand's interaction profile changes. */
	virtual uint32 GetInteractionProfileGeneration() const = 0;

	virtual FWoTVRDiagnostics GetDiagnostics() const = 0;

	// The engine's own stereo pipeline. Whatever runtime supplies the poses, the
	// engine's XR camera (FDefaultXRCamera) turns the composed view into each
	// rendered eye; these read what it does, from the engine's XR system.

	/** Each eye's position relative to the head, as the runtime renders it: in
	 * metres, in the head's frame (X forward, Y right, Z up). Eye 0 is the left
	 * eye. Without an engine XR system, half the eye separation to either side.
	 * False when the eye cannot be placed. */
	virtual bool GetEyeOffsetMeters(int32 Eye, FVector& OutOffset) const;

	/** The head offset, in world units, that the engine's stereo pipeline adds
	 * to each eye of a view whose target has no active camera component, this
	 * frame; zero when it adds none. ULocalPlayer::GetProjectionData leaves the
	 * XR camera's implicit head position off for such a view, so
	 * FDefaultXRCamera::CalculateStereoCameraOffset adds the tracked head
	 * position, turned by the XR camera's control yaw, to every eye: on the
	 * game thread, and again with the newer pose in the render-thread late
	 * update (FSceneView::UpdateViewMatrix).
	 * TrackingYaw: the view's own turn from tracking space to the world. When
	 * set, the control yaw is set to it first, so the late update moves each
	 * eye by the newer pose turned as the view is.
	 * Controller: the local player's. The control yaw's only setter,
	 * IXRCamera::ApplyHMDRotation, measures from its control rotation; without
	 * one the yaw is taken as unturned. */
	virtual FVector GetEngineAddedHeadOffset(class APlayerController* Controller, const TOptional<double>& TrackingYaw);

	/** Keeps the engine's render-thread late update on for this frame's view. A
	 * scene camera that does not follow the headset (UCameraComponent::bLockToHmd
	 * off) switches it off each frame it is viewed through
	 * (UCameraComponent::HandleXRCamera), and the switch outlives the camera.
	 * Anchor: any scene component without primitives or children. */
	virtual void KeepLateUpdate(class USceneComponent& Anchor);

	/** The engine's stereo layers (quads the runtime composites over the scene
	 * in both eyes), from its stereo rendering device; null without one. Stock
	 * OpenXR provides them natively, or draws them itself where the runtime
	 * cannot. */
	virtual class IStereoLayers* GetStereoLayers() const;
	/** World units per metre at which the engine's XR system converts the
	 * stereo layers' tracking-space poses (the world's scale while VR runs). */
	virtual float GetWorldToMetersScale() const;

private:
	/** The XR camera's control yaw as last read or set. It changes only while
	 * the head is tracked, so it stands while the head is not. */
	double EngineControlYaw = 0.0;
};
