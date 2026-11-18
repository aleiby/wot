#pragma once

#include "CoreMinimal.h"
#include "Camera/CameraTypes.h"
#include "Engine/EngineBaseTypes.h"
#include "InputCoreTypes.h"
#include "Subsystems/LocalPlayerSubsystem.h"
#include "WoTVRRig.h"
#include "WoTVRSession.h"
#include "WoTVRPlayerSubsystem.generated.h"

class APlayerController;
class FWoTVRPresentation;
class IWoTVRSession;
enum class EWoTLocalPlayerEvent : uint8;
struct FWoTLocalPlayerContext;

UENUM()
enum class EWoTVRMode : uint8
{
	Flat,
	VR
};

/** What the view shows, which sets how the head composes with it. */
enum class EWoTVRViewKind : uint8
{
	/** The possessed body's own eye: the full rig, guarded by view safety. */
	Body,
	/** No body (the front end, before possession, pawnless travel): the
	 * camera's eye is the neutral anchor. */
	Neutral,
	/** Death and the match end: held where the eye last was. */
	Frozen,
	/** Another actor's camera (the remote-view artifact), which follows the
	 * aim: the head's orientation at the camera, never its position. */
	Remote,
	/** The controller's own camera while it owns a body (the citadel orbit),
	 * which is not aim: its heading, with the head looking around from it,
	 * never its position. */
	Controller
};

/** Who asked for VR, which sets how loudly a failure is reported. */
enum class EWoTVRRequestKind : uint8
{
	/** The Auto startup preference: a failure is routine and play goes flat. */
	Auto,
	/** A switch the player asked for (key or console): a warning. */
	User,
	/** -vr or the VR startup preference: an error, then flat. */
	Explicit
};

/**
 * One local player's VR rig and mode (design sections 6, 8, 9 and 14).
 *
 * It lives with the local player, independent of its controller, pawn and
 * world, so the rig survives possession, death, travel and the front end. In
 * flat mode it attaches nothing to the game. Switching to VR (in place, at any
 * time) starts stereo and attaches one exclusive consumer to the local-player
 * hooks: the head aim (H2), the head-relative ground movement basis (H3) and
 * the view (H4: stereo composition, synthetic-motion removal and the stereo
 * near plane). Switching to flat releases held input once, detaches it so every
 * flat policy resumes, and stops stereo.
 */
UCLASS()
class WOTVR_API UWoTVRPlayerSubsystem : public ULocalPlayerSubsystem
{
	GENERATED_BODY()

public:
	/** The rig's owner name in the local-player hooks. */
	static const FName RigOwner;

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** The plugin participates for this player (FWoTVRModule::IsActive). */
	bool IsActive() const { return bActive; }
	EWoTVRMode GetMode() const { return Mode; }
	bool IsVR() const { return Mode == EWoTVRMode::VR; }

	/** Switches in place: in a match, in menus, in the front end or while dead,
	 * with no travel or rejoin. VR needs an XR system; a VR start that never
	 * becomes usable falls back to flat. False, with the reason logged, when the
	 * request cannot be honoured. Source names the requester in the log. */
	bool RequestMode(EWoTVRMode Target, const TCHAR* Source);
	bool ToggleMode(const TCHAR* Source);
	/** A switch can act: in VR, or in flat with a headset that can start VR. */
	bool CanSwitchMode() const;
	/** Confirms the current head pose as neutral (calibration), keeping the
	 * pawn's position and the world aim. Needs VR and a tracked head. */
	bool Recentre(const TCHAR* Source);

	void LogDiagnostics(const TCHAR* Why) const;
	FString DescribeStatus() const;

	/** The rig's arithmetic, for specs and the status command. */
	const FWoTVRRig& GetRig() const { return Rig; }
	/** VR has started: the session ran with a tracked head and the heading took
	 * the aim (false while a VR start is pending). */
	bool HasEnteredVR() const { return Mode == EWoTVRMode::VR && !bEntryPending; }
	/** Play is held: the character stands still and the aim holds, because the
	 * session is not usable or the calibration was invalidated. */
	bool IsHoldingStill() const;
	bool IsCalibrationInvalid() const { return bCalibrationInvalid; }
	bool IsRigAttached() const { return bRigAttached; }
	float GetFadeAlpha() const { return FadeAlpha; }
	/** The safety part of the fade: 1 when the eye path is blocked or the head
	 * left the safe volume. */
	float GetSafetyAlpha() const { return SafetyAlpha; }
	bool IsAnchorFrozen() const { return bAnchorFrozen; }
	int32 GetSwitchCount() const { return SwitchCount; }
	int32 GetReleaseCount() const { return ReleaseCount; }
	int32 GetSnapTurnCount() const { return SnapTurnCount; }
	/** What the view shows now. */
	EWoTVRViewKind ClassifyView() const;
	/** The headset HUD and menus (design section 10): present while VR runs
	 * with a usable session outside travel, else null (the desktop window
	 * shows the HUD and menus, and any surfaces kept for the next attach are
	 * hidden). */
	FWoTVRPresentation* GetPresentation() const;

	/** H4: the final view from the camera's view (also called by the hooks for
	 * every view, so free of side effects). The result is the rendered centre
	 * eye with the head in it exactly once: where the engine's stereo pipeline
	 * adds the tracked head to each eye itself (a view target without an active
	 * camera component), the composed location leaves it out. */
	void ComposeView(const FMinimalViewInfo& View, FVector& Location, FRotator& Rotation) const;

private:
	IWoTVRSession& Session() const;
	APlayerController* Controller() const;
	UWorld* PlayerWorld() const;

	void RefreshActivation();
	void Activate();
	void Deactivate();
	void Boot();

	bool EnterVR(const TCHAR* Source, EWoTVRRequestKind Kind);
	void ExitVR(const TCHAR* Source);
	bool AttachRig();
	void DetachRig();
	/** Exactly one release of held input: the hooks' release contract
	 * (InputContextChanged) and one flush of every held key. */
	void ReleaseHeldInput(const TCHAR* Why);

	void HandleLifecycle(EWoTLocalPlayerEvent Event, const FWoTLocalPlayerContext& Context);
	bool SampleAim(FRotator& OutAim) const;
	bool SampleGroundYaw(double& OutYaw) const;

	void HandleWorldTickStart(UWorld* World, ELevelTick TickType, float DeltaSeconds);
	void HandleWorldPostActorTick(UWorld* World, ELevelTick TickType, float DeltaSeconds);
	/** The headset HUD and menus, every world tick, paused or not. */
	void HandleWorldTickEnd(UWorld* World, ELevelTick TickType, float DeltaSeconds);
	void UpdateVR(float DeltaSeconds);
	/** Keeps a session running while VR is on: a session the runtime ended is
	 * replaced as soon as the runtime offers one, and a start that waited for
	 * the previous session to end goes ahead (design section 14). */
	void UpdateStereo(float DeltaSeconds);
	/** The head composed with the view of the given kind. Returns the composed
	 * view's turn from tracking space to the world (a yaw). */
	double ComposeHead(EWoTVRViewKind Kind, const FMinimalViewInfo& View, const FWoTVRHeadPose& Pose,
		FVector& Location, FRotator& Rotation) const;
	/** A change of view kind: into a special view, and back to the body. */
	void UpdateViewKind(const FWoTVRHeadPose& Pose);
	void CompleteEntry(const FWoTVRHeadPose& Pose);
	void FailEntry(const TCHAR* Why);
	void BeginHold(const TCHAR* Why);
	void EndHold(const FWoTVRHeadPose& Pose);
	/** The heading takes the current aim yaw: the aim continues without a jump. */
	void AlignHeadingToAim(const FWoTVRHeadPose& Pose);
	/** Folds the gameplay yaw into the heading, with the short transition fade. */
	void FoldGameplayYaw(const TCHAR* Why);
	void UpdateTurning(const FWoTVRHeadPose& Pose, float DeltaSeconds);
	void UpdateGameplayYawSettle(float DeltaSeconds);
	float ComputeSafetyAlpha(const FWoTVRHeadPose& Pose) const;
	void ApplyFade(float SafetyTarget, float DeltaSeconds);
	void StartTransitionFade();
	/** Applies FadeAlpha to the current controller's camera (black over both eyes). */
	void PushFade();
	/** Holds the body still through the hooks (never the controller's own
	 * ignore-input count, which a respawn resets and the match end uses). */
	void ApplyMovementBlock(bool bBlock);
	void ApplyWorldScale();
	void RestoreWorldScale();
	void ClearCameraFade();
	/** The engine's global near plane, which the headset's render-thread
	 * projection reads (FOpenXRHMD::GetStereoProjectionMatrix), is VR's while
	 * VR owns the view and the engine's own again afterwards. */
	void ApplyNearClipPlane();
	void RestoreNearClipPlane();
	/** The player's frame-rate cap and smoothing, which the engine's OpenXR
	 * plugin rewrites when stereo starts and again on its session's STOPPING
	 * and EXITING events after stereo stops: saved before VR, restored after,
	 * and guarded until the old session has ended. */
	void SaveFrameCap();
	void RestoreFrameCap();
	void GuardFrameCap();
	/** A turn key the player has bound to a game action stays that action's. */
	bool IsTurnKeyFree(const class UPlayerInput& Input, const FKey& Key);
	/** The headset HUD and menus attach when VR starts (the session usable)
	 * and detach on the switch to flat, at travel and at session loss, giving
	 * the HUD and every screen back to the desktop window; Pose is the head
	 * while they may attach, null otherwise. They attach again after travel,
	 * in the next world, and once the session recovers, on the surfaces kept
	 * since the last attach. */
	void UpdatePresentation(const FWoTVRHeadPose* Pose);
	/** Detaches the presentation, keeping its surfaces for the next attach in
	 * this VR session. */
	void DetachPresentation(const TCHAR* Why);
	/** Detaches it and frees its surfaces: the switch to flat, shutdown. */
	void ReleasePresentation(const TCHAR* Why);

	bool bActive = false;
	bool bBooted = false;
	EWoTVRMode Mode = EWoTVRMode::Flat;
	bool bRigAttached = false;
	bool bDetaching = false;
	bool bEntryPending = false;
	EWoTVRRequestKind EntryKind = EWoTVRRequestKind::User;
	float EntryElapsed = 0.0f;
	bool bHoldActive = false;
	bool bCalibrationInvalid = false;
	bool bReanchorPending = false;
	bool bFoldPending = false;
	bool bAnchorFrozen = false;
	FVector FrozenAnchor = FVector::ZeroVector;
	bool bWasUsable = false;
	uint32 SeenReferenceGeneration = 0;
	uint32 SeenProfileGeneration = 0;

	FWoTVRRig Rig;
	FWoTVRHeadPose LastValidPose;
	bool bHasLastValidPose = false;

	bool bSnapArmed = true;
	double SettleYaw = 0.0;
	float SettleElapsed = 0.0f;

	float SafetyAlpha = 0.0f;
	float TransitionAlpha = 0.0f;
	float FadeAlpha = 0.0f;
	TWeakObjectPtr<APlayerController> FadedController;

	TWeakObjectPtr<UWorld> ScaledWorld;
	float OriginalWorldToMeters = 100.0f;
	TOptional<float> SavedNearClip;
	TOptional<float> SavedMaxFPS;
	bool bSavedForceDisableSmoothing = false;
	bool bFrameCapGuard = false;
	/** Seconds until the next attempt to start a session after the runtime
	 * refused one. */
	float StereoRetryCooldown = 0.0f;

	/** The view kind the last update saw, and the head's relative yaw when a
	 * special view began (the controller's camera keeps its heading from there). */
	EWoTVRViewKind ViewKind = EWoTVRViewKind::Neutral;
	double SpecialReferenceYaw = 0.0;
	TSet<FKey> ReportedTurnKeys;

	/** An empty scene component the late update can be pointed at. */
	UPROPERTY(Transient)
	TObjectPtr<class USceneComponent> LateUpdateAnchor;

	TSharedPtr<FWoTVRPresentation> Presentation;
	/** The world travel left: the presentation waits for the next one. */
	TWeakObjectPtr<UWorld> PresentationTravelWorld;
	bool bPresentationTravelling = false;
	/** The presentation could not attach (no stereo layers, or another
	 * consumer owns the HUD or UI slot): not retried until VR starts again. */
	bool bPresentationRefused = false;

	int32 SwitchCount = 0;
	int32 ReleaseCount = 0;
	int32 SnapTurnCount = 0;

	FDelegateHandle WorldTickHandle;
	FDelegateHandle WorldPostActorTickHandle;
	FDelegateHandle WorldTickEndHandle;
	FDelegateHandle ActivationHandle;
};
