#include "WoTVRPlayerSubsystem.h"

#include "WoTLocalPlayerHooks.h"
#include "WoTVRGate.h"
#include "WoTVRLog.h"
#include "WoTVRModule.h"
#include "WoTVRPresentation.h"
#include "WoTVRSettings.h"
#include "WoTVRTuning.h"

#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "CollisionQueryParams.h"
#include "CollisionShape.h"
#include "Components/SceneComponent.h"
#include "CoreGlobals.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/HitResult.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/IConsoleManager.h"
#include "InputCoreTypes.h"
#include "RenderCore.h"

const FName UWoTVRPlayerSubsystem::RigOwner(TEXT("WoTVR"));

namespace
{
const TCHAR* WoTVRPostureName(EWoTVRPosture Posture)
{
	return Posture == EWoTVRPosture::Seated ? TEXT("Seated") : TEXT("Standing");
}

/** The view target's camera component the engine views through (as
 * AActor::CalcCamera picks it: the first active one), or null. */
const UCameraComponent* WoTVRViewCamera(const AActor* Target)
{
	if (!Target || !Target->HasActiveCameraComponent())
	{
		return nullptr;
	}
	TInlineComponentArray<UCameraComponent*> Cameras;
	Target->GetComponents(Cameras);
	for (const UCameraComponent* Camera : Cameras)
	{
		if (Camera && Camera->IsActive())
		{
			return Camera;
		}
	}
	return nullptr;
}

/** The rate the engine's OpenXR plugin caps a stopping session at. */
float WoTVROpenXRPausedIdleFPS()
{
	const IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(TEXT("xr.OpenXRPausedIdleFPS"));
	return Variable ? Variable->GetFloat() : 10.0f;
}

const TCHAR* WoTVRTurnModeName(EWoTVRTurnMode TurnMode)
{
	return TurnMode == EWoTVRTurnMode::Smooth ? TEXT("Smooth") : TEXT("Snap");
}

const TCHAR* WoTVRViewKindName(EWoTVRViewKind Kind)
{
	switch (Kind)
	{
	case EWoTVRViewKind::Body: return TEXT("body");
	case EWoTVRViewKind::Neutral: return TEXT("neutral anchor");
	case EWoTVRViewKind::Frozen: return TEXT("held (death or match end)");
	case EWoTVRViewKind::Remote: return TEXT("remote camera");
	case EWoTVRViewKind::Controller: return TEXT("controller camera (orbit)");
	}
	return TEXT("unknown");
}

FString WoTVRKeyName(const FKey& Key)
{
	return Key.IsValid() ? Key.ToString() : FString(TEXT("(unbound)"));
}
}

// --- lifetime and activation ---------------------------------------------

void UWoTVRPlayerSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	if (FWoTVRModule::IsAvailable())
	{
		ActivationHandle = FWoTVRModule::Get().OnActivationChanged.AddUObject(
			this, &UWoTVRPlayerSubsystem::RefreshActivation);
	}
	RefreshActivation();
}

void UWoTVRPlayerSubsystem::Deinitialize()
{
	if (FWoTVRModule::IsAvailable())
	{
		FWoTVRModule::Get().OnActivationChanged.Remove(ActivationHandle);
	}
	ActivationHandle.Reset();
	Deactivate();
	ReleasePresentation(TEXT("local player leaving"));
	Super::Deinitialize();
}

IWoTVRSession& UWoTVRPlayerSubsystem::Session() const
{
	return FWoTVRModule::Get().GetSession();
}

APlayerController* UWoTVRPlayerSubsystem::Controller() const
{
	const ULocalPlayer* LocalPlayer = GetLocalPlayer();
	APlayerController* PC = LocalPlayer ? LocalPlayer->PlayerController.Get() : nullptr;
	return IsValid(PC) ? PC : nullptr;
}

UWorld* UWoTVRPlayerSubsystem::PlayerWorld() const
{
	if (const APlayerController* PC = Controller())
	{
		return PC->GetWorld();
	}
	const ULocalPlayer* LocalPlayer = GetLocalPlayer();
	const UGameInstance* GameInstance = LocalPlayer ? LocalPlayer->GetGameInstance() : nullptr;
	return GameInstance ? GameInstance->GetWorld() : nullptr;
}

void UWoTVRPlayerSubsystem::RefreshActivation()
{
	const bool bShouldBeActive = FWoTVRModule::IsAvailable() && FWoTVRModule::Get().IsActive();
	if (bShouldBeActive && !bActive)
	{
		Activate();
	}
	else if (!bShouldBeActive && bActive)
	{
		Deactivate();
	}
}

void UWoTVRPlayerSubsystem::Activate()
{
	bActive = true;
	WorldTickHandle = FWorldDelegates::OnWorldTickStart.AddUObject(
		this, &UWoTVRPlayerSubsystem::HandleWorldTickStart);
	WorldPostActorTickHandle = FWorldDelegates::OnWorldPostActorTick.AddUObject(
		this, &UWoTVRPlayerSubsystem::HandleWorldPostActorTick);
	WorldTickEndHandle = FWorldDelegates::OnWorldTickEnd.AddUObject(
		this, &UWoTVRPlayerSubsystem::HandleWorldTickEnd);
	if (!LateUpdateAnchor)
	{
		LateUpdateAnchor = NewObject<USceneComponent>(this, TEXT("WoTVRLateUpdateAnchor"), RF_Transient);
	}
	FWoTVRModule::Get().AcquireShortcuts();
	UE_LOG(LogWoTVR, Log, TEXT("VR rig ready for %s (session: %s, %s)"),
		*GetNameSafe(GetLocalPlayer()), *Session().GetKind().ToString(),
		LexToString(Session().GetState()));
	if (!bBooted)
	{
		bBooted = true;
		Boot();
	}
}

void UWoTVRPlayerSubsystem::Deactivate()
{
	if (!bActive)
	{
		return;
	}
	if (Mode == EWoTVRMode::VR)
	{
		ExitVR(TEXT("VR deactivated"));
	}
	FWorldDelegates::OnWorldTickStart.Remove(WorldTickHandle);
	WorldTickHandle.Reset();
	FWorldDelegates::OnWorldPostActorTick.Remove(WorldPostActorTickHandle);
	WorldPostActorTickHandle.Reset();
	FWorldDelegates::OnWorldTickEnd.Remove(WorldTickEndHandle);
	WorldTickEndHandle.Reset();
	// The frame-rate guard needs the ticks; leave the player's cap in place.
	if (bFrameCapGuard && SavedMaxFPS.IsSet() && GEngine && GEngine->GetMaxFPS() != *SavedMaxFPS)
	{
		GEngine->SetMaxFPS(*SavedMaxFPS);
	}
	bFrameCapGuard = false;
	SavedMaxFPS.Reset();
	if (FWoTVRModule::IsAvailable())
	{
		FWoTVRModule::Get().ReleaseShortcuts();
	}
	bActive = false;
}

void UWoTVRPlayerSubsystem::Boot()
{
	const FWoTVRStartupDecision Startup = FWoTVRModule::Get().ResolveProcessStartup();
	switch (Startup.Request)
	{
	case EWoTVRStartupRequest::AutoVR:
		EnterVR(*Startup.Reason, EWoTVRRequestKind::Auto);
		break;
	case EWoTVRStartupRequest::ExplicitVR:
		EnterVR(*Startup.Reason, EWoTVRRequestKind::Explicit);
		break;
	case EWoTVRStartupRequest::Flat:
	default:
		// A stereo start made by engine configuration alone does not outvote the
		// preference and the flags.
		if (Session().IsStereoEnabled())
		{
			Session().SetStereo(false);
		}
		UE_LOG(LogWoTVR, Log, TEXT("Starting flat (%s); %s switches to VR"),
			*Startup.Reason, *WoTVRKeyName(UWoTVRSettings::Get().ModeSwitchKey));
		break;
	}
}

// --- switching -------------------------------------------------------------

bool UWoTVRPlayerSubsystem::RequestMode(EWoTVRMode Target, const TCHAR* Source)
{
	if (!bActive)
	{
		UE_LOG(LogWoTVR, Display, TEXT("VR is not active this launch: %s ignored (%s)"), Source,
			*FWoTVRGate::GetProcessDecision().Reason);
		return false;
	}
	if (Target == Mode)
	{
		return true;
	}
	if (Target == EWoTVRMode::VR)
	{
		return EnterVR(Source, EWoTVRRequestKind::User);
	}
	ExitVR(Source);
	return true;
}

bool UWoTVRPlayerSubsystem::ToggleMode(const TCHAR* Source)
{
	return RequestMode(Mode == EWoTVRMode::VR ? EWoTVRMode::Flat : EWoTVRMode::VR, Source);
}

bool UWoTVRPlayerSubsystem::CanSwitchMode() const
{
	return bActive && (Mode == EWoTVRMode::VR || Session().IsSystemAvailable());
}

bool UWoTVRPlayerSubsystem::EnterVR(const TCHAR* Source, EWoTVRRequestKind Kind)
{
	if (Mode == EWoTVRMode::VR)
	{
		return true;
	}
	IWoTVRSession& XR = Session();
	auto ReportFailure = [Source, Kind](const TCHAR* Why)
	{
		switch (Kind)
		{
		case EWoTVRRequestKind::Explicit:
			UE_LOG(LogWoTVR, Error, TEXT("VR requested by %s, but %s; continuing flat"), Source, Why);
			break;
		case EWoTVRRequestKind::User:
			UE_LOG(LogWoTVR, Warning, TEXT("Switch to VR (%s) failed: %s; staying flat"), Source, Why);
			break;
		case EWoTVRRequestKind::Auto:
		default:
			UE_LOG(LogWoTVR, Display, TEXT("Auto VR (%s): %s; starting flat"), Source, Why);
			break;
		}
	};
	if (!XR.IsSystemAvailable())
	{
		ReportFailure(TEXT("no XR system is available (no runtime or headset answered)"));
		return false;
	}
	ReleaseHeldInput(TEXT("switch to VR"));
	SaveFrameCap();
	if (XR.GetState() == EWoTVRSessionState::Lost)
	{
		// A session the runtime ended under engine-started stereo: stereo off
		// first, so the start below makes a new one.
		XR.SetStereo(false);
	}
	if (XR.GetState() == EWoTVRSessionState::Ending)
	{
		// The previous session is still shutting down (a quick switch back), and
		// the runtime refuses a second one until it has: stereo starts once it
		// has ended (UpdateStereo), within the start timeout.
		UE_LOG(LogWoTVR, Log, TEXT("Switch to VR (%s): waiting for the previous session to end"), Source);
	}
	else if (!XR.SetStereo(true))
	{
		ReportFailure(TEXT("the runtime did not start stereo"));
		RestoreFrameCap();
		return false;
	}
	XR.PrepareTracking();
	StereoRetryCooldown = 0.0f;
	bPresentationRefused = false;
	bPresentationTravelling = false;
	PresentationTravelWorld.Reset();
	Mode = EWoTVRMode::VR;
	bEntryPending = true;
	EntryKind = Kind;
	EntryElapsed = 0.0f;
	bHoldActive = false;
	bCalibrationInvalid = false;
	bReanchorPending = false;
	bFoldPending = false;
	bAnchorFrozen = false;
	bWasUsable = false;
	bSnapArmed = false;
	bHasLastValidPose = false;
	Rig.bCalibrated = false;
	SettleElapsed = 0.0f;
	SettleYaw = 0.0;
	SafetyAlpha = 0.0f;
	TransitionAlpha = 1.0f;
	SeenReferenceGeneration = XR.GetReferenceSpaceGeneration();
	SeenProfileGeneration = XR.GetInteractionProfileGeneration();
	if (!AttachRig())
	{
		ReportFailure(TEXT("another consumer already owns this player's aim, movement or view"));
		if (XR.IsStereoEnabled())
		{
			XR.SetStereo(false);
		}
		RestoreFrameCap();
		Mode = EWoTVRMode::Flat;
		bEntryPending = false;
		return false;
	}
	ApplyWorldScale();
	ApplyNearClipPlane();
	ViewKind = ClassifyView();
	++SwitchCount;
	UE_LOG(LogWoTVR, Display, TEXT("Switched to VR (%s)"), Source);
	FWoTVRHeadPose Pose;
	const bool bUsable = XR.IsUsable() && XR.GetHeadPose(Pose) && Pose.bValid;
	if (bUsable)
	{
		CompleteEntry(Pose);
	}
	// Held still from the switch itself until the head is tracked.
	ApplyMovementBlock(IsHoldingStill());
	// The HUD and the open screens move to the headset with the switch.
	UpdatePresentation(bUsable ? &Pose : nullptr);
	return true;
}

void UWoTVRPlayerSubsystem::ExitVR(const TCHAR* Source)
{
	if (Mode != EWoTVRMode::VR)
	{
		return;
	}
	// The HUD goes back to the flat Canvas, and every screen to the viewport
	// with its focus, before anything else changes; flat keeps no surfaces.
	ReleasePresentation(Source);
	ReleaseHeldInput(TEXT("switch to flat"));
	// The hold on the body goes with the hooks consumer.
	DetachRig();
	ClearCameraFade();
	SafetyAlpha = 0.0f;
	TransitionAlpha = 0.0f;
	FadeAlpha = 0.0f;
	RestoreWorldScale();
	RestoreNearClipPlane();
	IWoTVRSession& XR = Session();
	if (XR.IsStereoEnabled())
	{
		XR.SetStereo(false);
	}
	RestoreFrameCap();
	Mode = EWoTVRMode::Flat;
	bEntryPending = false;
	bHoldActive = false;
	bCalibrationInvalid = false;
	bReanchorPending = false;
	bFoldPending = false;
	bAnchorFrozen = false;
	bHasLastValidPose = false;
	Rig.bCalibrated = false;
	++SwitchCount;
	UE_LOG(LogWoTVR, Display, TEXT("Switched to flat (%s)"), Source);
}

bool UWoTVRPlayerSubsystem::AttachRig()
{
	ULocalPlayer* LocalPlayer = GetLocalPlayer();
	if (!LocalPlayer)
	{
		return false;
	}
	const TWeakObjectPtr<UWoTVRPlayerSubsystem> Weak(this);
	FWoTLocalPlayerConsumer Consumer;
	Consumer.Lifecycle = [Weak](EWoTLocalPlayerEvent Event, const FWoTLocalPlayerContext& Context)
	{
		if (UWoTVRPlayerSubsystem* Self = Weak.Get())
		{
			Self->HandleLifecycle(Event, Context);
		}
	};
	Consumer.AimRotation = [Weak](FRotator& OutAim)
	{
		const UWoTVRPlayerSubsystem* Self = Weak.Get();
		return Self && Self->SampleAim(OutAim);
	};
	Consumer.GroundMovementYaw = [Weak](double& OutYaw)
	{
		const UWoTVRPlayerSubsystem* Self = Weak.Get();
		return Self && Self->SampleGroundYaw(OutYaw);
	};
	FWoTLocalViewPolicy& View = Consumer.View.Emplace();
	View.bSuppressSyntheticMotion = true;
	View.Compose = [Weak](const FWoTLocalPlayerContext&, const FMinimalViewInfo& Camera,
		FVector& Location, FRotator& Rotation)
	{
		if (const UWoTVRPlayerSubsystem* Self = Weak.Get())
		{
			Self->ComposeView(Camera, Location, Rotation);
		}
	};
	View.NearClip = []() { return WoTVRTuning::NearClip(); };
	return FWoTLocalPlayerHooks::Attach(*LocalPlayer, RigOwner, Consumer);
}

void UWoTVRPlayerSubsystem::DetachRig()
{
	bDetaching = true;
	if (ULocalPlayer* LocalPlayer = GetLocalPlayer())
	{
		FWoTLocalPlayerHooks::Detach(*LocalPlayer, RigOwner);
	}
	bDetaching = false;
	bRigAttached = false;
}

void UWoTVRPlayerSubsystem::ReleaseHeldInput(const TCHAR* Why)
{
	if (APlayerController* PC = Controller())
	{
		// The hooks' release contract: every attached consumer releases its held
		// commands once. Then every held key and button is released once,
		// through its own release binding, at the next input pass.
		FWoTLocalPlayerHooks::Notify(*PC, EWoTLocalPlayerEvent::InputContextChanged);
		if (PC->PlayerInput)
		{
			PC->PlayerInput->FlushPressedKeys();
		}
	}
	bSnapArmed = false;
	++ReleaseCount;
	UE_LOG(LogWoTVR, Verbose, TEXT("Released held input (%s)"), Why);
}

bool UWoTVRPlayerSubsystem::Recentre(const TCHAR* Source)
{
	if (Mode != EWoTVRMode::VR || bEntryPending)
	{
		UE_LOG(LogWoTVR, Display, TEXT("Recentre (%s): VR is not running"), Source);
		return false;
	}
	IWoTVRSession& XR = Session();
	FWoTVRHeadPose Pose;
	if (!XR.IsUsable() || !XR.GetHeadPose(Pose) || !Pose.bValid)
	{
		UE_LOG(LogWoTVR, Display, TEXT("Recentre (%s): the head is not tracked"), Source);
		return false;
	}
	ReleaseHeldInput(TEXT("recentre"));
	// The head returns over the anchor; the heading maps the head's current
	// direction onto the aim the player has now (held while the calibration
	// was invalid), so neither the pawn nor the world aim moves.
	Rig.Recentre(Pose);
	AlignHeadingToAim(Pose);
	LastValidPose = Pose;
	bHasLastValidPose = true;
	if (Presentation)
	{
		// Open menus and the board move in front of the view; the HUD panel
		// faces the head again.
		Presentation->Recentre(Pose);
	}
	const bool bRestored = bCalibrationInvalid;
	bCalibrationInvalid = false;
	bReanchorPending = false;
	bFoldPending = false;
	StartTransitionFade();
	UE_LOG(LogWoTVR, Display, TEXT("Recentred (%s): %s neutral pose confirmed%s"), Source,
		WoTVRPostureName(UWoTVRSettings::Get().Posture),
		bRestored ? TEXT("; calibration valid again") : TEXT(""));
	return true;
}

// --- hooks callbacks ---------------------------------------------------------

void UWoTVRPlayerSubsystem::HandleLifecycle(EWoTLocalPlayerEvent Event, const FWoTLocalPlayerContext& Context)
{
	auto ReanchorNow = [this]()
	{
		FWoTVRHeadPose Pose;
		if (Session().GetHeadPose(Pose) && Pose.bValid && Rig.bCalibrated && !bCalibrationInvalid)
		{
			Rig.ReanchorHorizontal(Pose);
			bReanchorPending = false;
		}
		else
		{
			bReanchorPending = true;
		}
	};
	switch (Event)
	{
	case EWoTLocalPlayerEvent::Attached:
		bRigAttached = true;
		break;
	case EWoTLocalPlayerEvent::Detached:
		bRigAttached = false;
		if (Mode == EWoTVRMode::VR && !bDetaching)
		{
			// The game let the consumer go (the local player is leaving): leave VR
			// without detaching again.
			UE_LOG(LogWoTVR, Log, TEXT("VR rig released by the game; leaving VR"));
			ReleasePresentation(TEXT("released by the game"));
			ClearCameraFade();
			RestoreWorldScale();
			RestoreNearClipPlane();
			if (Session().IsStereoEnabled())
			{
				Session().SetStereo(false);
			}
			RestoreFrameCap();
			Mode = EWoTVRMode::Flat;
			bEntryPending = false;
			bHoldActive = false;
			Rig.bCalibrated = false;
		}
		break;
	case EWoTLocalPlayerEvent::ControllerChanged:
		// The rig stays with the local player (its hold on the body with it);
		// world-bound state rebinds.
		bAnchorFrozen = false;
		ClearCameraFade();
		break;
	case EWoTLocalPlayerEvent::Possessed:
	case EWoTLocalPlayerEvent::Respawn:
		bAnchorFrozen = false;
		if (!bEntryPending)
		{
			// The head goes back over the new pawn and the view lines up with its
			// aim, behind a short fade. The game sets the spawn facing just after
			// possession, so the next frame's update takes that too.
			FWoTVRHeadPose Pose;
			if (Session().GetHeadPose(Pose) && Pose.bValid)
			{
				AlignHeadingToAim(Pose);
			}
			ReanchorNow();
			bFoldPending = true;
			StartTransitionFade();
		}
		break;
	case EWoTLocalPlayerEvent::Teleported:
		if (!bEntryPending)
		{
			// Sent after the whole relocation, exit heading included.
			FoldGameplayYaw(TEXT("teleport"));
			ReanchorNow();
			StartTransitionFade();
		}
		break;
	case EWoTLocalPlayerEvent::FrontEnd:
		bAnchorFrozen = false;
		if (!bEntryPending)
		{
			ReanchorNow();
			StartTransitionFade();
		}
		break;
	case EWoTLocalPlayerEvent::Travel:
		bAnchorFrozen = false;
		RestoreWorldScale();
		StartTransitionFade();
		// The HUD and screens go back to the desktop window for the travel and
		// come back in the next world.
		DetachPresentation(TEXT("travel"));
		bPresentationTravelling = true;
		PresentationTravelWorld = Context.World.IsValid() ? Context.World.Get() : PlayerWorld();
		break;
	case EWoTLocalPlayerEvent::Death:
	case EWoTLocalPlayerEvent::MatchEnded:
		// A stable view: hold the eye where the camera last was, so neither the
		// death camera nor the head-steered match-end camera moves the player.
		if (const APlayerController* PC = Context.Controller.Get())
		{
			if (PC->PlayerCameraManager)
			{
				FrozenAnchor = PC->PlayerCameraManager->GetCameraCacheView().Location;
				bAnchorFrozen = true;
			}
		}
		StartTransitionFade();
		break;
	case EWoTLocalPlayerEvent::InputContextChanged:
		// A fresh deflection is needed for the next snap.
		bSnapArmed = false;
		break;
	case EWoTLocalPlayerEvent::ForcedTurnEnded:
		// The forced rotation never reached the rendered head; the view takes it
		// now that it has ended.
		if (!bEntryPending)
		{
			if (const ULocalPlayer* LocalPlayer = GetLocalPlayer();
				LocalPlayer && !FWoTLocalPlayerHooks::IsForcedTurn(*LocalPlayer))
			{
				FoldGameplayYaw(TEXT("forced turn ended"));
			}
		}
		break;
	case EWoTLocalPlayerEvent::Unpossessed:
	case EWoTLocalPlayerEvent::ForcedTurnStarted:
	default:
		break;
	}
}

bool UWoTVRPlayerSubsystem::SampleAim(FRotator& OutAim) const
{
	if (Mode != EWoTVRMode::VR || !bRigAttached || bEntryPending || bHoldActive || bCalibrationInvalid)
	{
		return false;
	}
	const IWoTVRSession& XR = Session();
	FWoTVRHeadPose Pose;
	if (!XR.IsUsable() || !XR.GetHeadPose(Pose) || !Pose.bValid)
	{
		return false;
	}
	FRotator Aim = Rig.Aim(Pose);
	// While an XR system tracks the head, the engine's camera manager skips its
	// view pitch limits; apply them here, as mouse aim gets them. Gameplay
	// constraints (the impaled pitch band) apply after this, in the game.
	if (const APlayerController* PC = Controller(); PC && PC->PlayerCameraManager)
	{
		Aim.Pitch = FMath::ClampAngle(Aim.Pitch, PC->PlayerCameraManager->ViewPitchMin,
			PC->PlayerCameraManager->ViewPitchMax);
	}
	OutAim = Aim;
	return true;
}

bool UWoTVRPlayerSubsystem::SampleGroundYaw(double& OutYaw) const
{
	if (Mode != EWoTVRMode::VR || !bRigAttached || bEntryPending)
	{
		return false;
	}
	FWoTVRHeadPose Pose;
	if (!(Session().GetHeadPose(Pose) && Pose.bValid))
	{
		if (!bHasLastValidPose)
		{
			return false;
		}
		Pose = LastValidPose;
	}
	// Head-relative: forward is where the head looks, projected on the ground,
	// keeping the last valid yaw near vertical.
	OutYaw = Rig.WorldYaw(Pose);
	return true;
}

EWoTVRViewKind UWoTVRPlayerSubsystem::ClassifyView() const
{
	if (bAnchorFrozen)
	{
		return EWoTVRViewKind::Frozen;
	}
	const APlayerController* PC = Controller();
	const APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	if (!Pawn)
	{
		return EWoTVRViewKind::Neutral;
	}
	const AActor* Target = PC->GetViewTarget();
	if (!Target || Target == Pawn)
	{
		return EWoTVRViewKind::Body;
	}
	return Target == PC ? EWoTVRViewKind::Controller : EWoTVRViewKind::Remote;
}

void UWoTVRPlayerSubsystem::ComposeView(const FMinimalViewInfo& View, FVector& Location,
	FRotator& Rotation) const
{
	if (Mode != EWoTVRMode::VR)
	{
		return;
	}
	TOptional<double> TrackingYaw;
	if (!bEntryPending)
	{
		FWoTVRHeadPose Pose;
		bool bPose = Session().GetHeadPose(Pose) && Pose.bValid;
		if (!bPose && bHasLastValidPose)
		{
			Pose = LastValidPose;
			bPose = true;
		}
		if (bPose)
		{
			TrackingYaw = ComposeHead(ClassifyView(), View, Pose, Location, Rotation);
		}
	}
	// The head is in the composed view once. For a view target without an
	// active camera component the engine's stereo pipeline adds the tracked head
	// position to each eye again, turned by its control yaw: on the game thread,
	// and with the newer pose in the render-thread late update. That yaw is set
	// to the composed view's own turn from tracking space and the composed
	// location leaves the engine's share out, so the late update moves each eye
	// only by the pose's change since the frame began, turned as the view is. A
	// scene camera's view gets no such addition (the late update turns the
	// change by the view itself). The same test as
	// ULocalPlayer::GetProjectionData's.
	APlayerController* PC = Controller();
	const AActor* Target = PC ? PC->GetViewTarget() : nullptr;
	if (!(Target && Target->HasActiveCameraComponent()))
	{
		Location -= Session().GetEngineAddedHeadOffset(PC, TrackingYaw);
	}
}

double UWoTVRPlayerSubsystem::ComposeHead(EWoTVRViewKind Kind, const FMinimalViewInfo& View,
	const FWoTVRHeadPose& Pose, FVector& Location, FRotator& Rotation) const
{
	// Tracked translation is added through the heading; with an invalidated
	// calibration only the orientation is trusted.
	const bool bTranslate = Rig.bCalibrated && !bCalibrationInvalid;
	// Every kind turns the tracked head by a heading: R(Heading) * R(-NeutralYaw).
	double Heading = Rig.HeadingYaw;
	switch (Kind)
	{
	case EWoTVRViewKind::Remote:
		// A remote camera follows the aim, which follows the head: the head's
		// orientation at the camera. Never its position, so physical movement
		// never carries a remote camera into a wall.
		Location = View.Location;
		Rotation = Rig.WorldOrientation(Pose).Rotator();
		break;
	case EWoTVRViewKind::Controller:
	{
		// The controller's own camera (the citadel orbit) keeps its heading,
		// the head looking around from where it looked when the view began; its
		// tilt is not imposed on the headset and its position never moves.
		const double Reference = ViewKind == EWoTVRViewKind::Controller ? SpecialReferenceYaw : Rig.RelativeYaw(Pose);
		Heading = View.Rotation.Yaw - Reference;
		Location = View.Location;
		Rotation = Rig.OrientationUnderHeading(Pose, Heading).Rotator();
		break;
	}
	case EWoTVRViewKind::Frozen:
		// Death and the match end: held where the eye last was.
		Location = bTranslate ? FrozenAnchor + Rig.WorldOffset(Pose) : FrozenAnchor;
		Rotation = Rig.WorldOrientation(Pose).Rotator();
		break;
	case EWoTVRViewKind::Body:
	case EWoTVRViewKind::Neutral:
	default:
		// The camera's eye: the simulation eye in play (synthetic motion
		// removed), the camera before possession (the front end's scene camera
		// at its authored eye).
		Location = bTranslate ? View.Location + Rig.WorldOffset(Pose) : View.Location;
		Rotation = Rig.WorldOrientation(Pose).Rotator();
		break;
	}
	return FMath::UnwindDegrees(Heading - Rig.NeutralYaw);
}

// --- per frame ---------------------------------------------------------------

void UWoTVRPlayerSubsystem::HandleWorldTickStart(UWorld* World, ELevelTick, float DeltaSeconds)
{
	if (!bActive || !World || World != PlayerWorld())
	{
		return;
	}
	if (Mode == EWoTVRMode::VR)
	{
		UpdateVR(DeltaSeconds);
	}
	else
	{
		GuardFrameCap();
	}
}

void UWoTVRPlayerSubsystem::HandleWorldPostActorTick(UWorld* World, ELevelTick, float DeltaSeconds)
{
	if (!bActive || !World || World != PlayerWorld())
	{
		return;
	}
	if (Mode != EWoTVRMode::VR)
	{
		// OpenXR reads its session events after the world tick starts, so a
		// stopping session's cap lands after the tick-start guard: undone here,
		// before the next frame is timed.
		GuardFrameCap();
		return;
	}
	if (!LateUpdateAnchor)
	{
		return;
	}
	// After this frame's camera update: a scene camera that does not follow the
	// headset (the front end's) has just switched the engine's late update off
	// (UCameraComponent::HandleXRCamera). The rig composes the head itself, so
	// the late update stays valid and stays on, there and in every view after.
	const APlayerController* PC = Controller();
	const UCameraComponent* Camera = WoTVRViewCamera(PC ? PC->GetViewTarget() : nullptr);
	if (Camera && !Camera->bLockToHmd)
	{
		Session().KeepLateUpdate(*LateUpdateAnchor);
	}
}

void UWoTVRPlayerSubsystem::HandleWorldTickEnd(UWorld* World, ELevelTick, float DeltaSeconds)
{
	// The world's last broadcast, sent every tick: unlike the post-actor tick it
	// also runs while the world is paused (the Pause key, the map-load barrier)
	// and while a client's connection is not open (UWorld::Tick sends
	// OnWorldPostActorTick only when actors tick). After this frame's game
	// logic and the rig's view update at the tick's start, before the frame is
	// drawn: the panels follow the head, the pointer moves, and the hosted
	// screens paint, so a menu opened while paused shows in the headset.
	if (!bActive || !World || World != PlayerWorld() || Mode != EWoTVRMode::VR)
	{
		return;
	}
	if (Presentation && Presentation->IsAttached())
	{
		FWoTVRHeadPose Pose;
		Session().GetHeadPose(Pose);
		Presentation->Tick(DeltaSeconds, Pose);
	}
}

void UWoTVRPlayerSubsystem::UpdateStereo(float DeltaSeconds)
{
	IWoTVRSession& XR = Session();
	StereoRetryCooldown = FMath::Max(0.0f, StereoRetryCooldown - DeltaSeconds);
	switch (XR.GetState())
	{
	case EWoTVRSessionState::Lost:
		// The runtime ended the session (it quit, left VR or lost the headset)
		// and left stereo on: stereo off, so the next start makes a new session.
		UE_LOG(LogWoTVR, Warning,
			TEXT("The VR runtime ended the session; a new one starts when the runtime offers it (%s or WoT.VR.Mode Flat continues flat)"),
			*WoTVRKeyName(UWoTVRSettings::Get().ModeSwitchKey));
		XR.SetStereo(false);
		StereoRetryCooldown = 0.0f;
		break;
	case EWoTVRSessionState::Idle:
		// VR is on and no session runs: the previous one has ended (a quick
		// switch back waited for it) or the runtime ended it. Start one as soon
		// as the runtime has a system for it, and retry at a modest rate while
		// it refuses.
		if (StereoRetryCooldown <= 0.0f)
		{
			if (XR.SetStereo(true))
			{
				XR.PrepareTracking();
				UE_LOG(LogWoTVR, Log, TEXT("Starting a VR session"));
			}
			else
			{
				StereoRetryCooldown = WoTVRTuning::SessionRetrySeconds();
			}
		}
		break;
	default:
		break;
	}
}

void UWoTVRPlayerSubsystem::UpdateVR(float DeltaSeconds)
{
	IWoTVRSession& XR = Session();
	UpdateStereo(DeltaSeconds);
	ApplyNearClipPlane();
	const EWoTVRSessionState State = XR.GetState();
	FWoTVRHeadPose Pose;
	const bool bPose = XR.GetHeadPose(Pose) && Pose.bValid;
	const bool bUsable = State == EWoTVRSessionState::Running && bPose;
	if (bUsable)
	{
		LastValidPose = Pose;
		bHasLastValidPose = true;
		Rig.NoteValidPose(Pose);
	}

	// Diagnostics for the first headset sessions: at each session start, and
	// whenever a hand's interaction profile changes.
	const uint32 ProfileGeneration = XR.GetInteractionProfileGeneration();
	if (ProfileGeneration != SeenProfileGeneration)
	{
		SeenProfileGeneration = ProfileGeneration;
		const FWoTVRDiagnostics Diagnostics = XR.GetDiagnostics();
		UE_LOG(LogWoTVR, Display, TEXT("VR interaction profile changed: left hand %s, right hand %s"),
			*Diagnostics.LeftHandProfile, *Diagnostics.RightHandProfile);
	}
	if (bUsable && !bWasUsable)
	{
		LogDiagnostics(TEXT("session start"));
	}
	bWasUsable = bUsable;

	// A reset reference space makes the confirmed neutral pose meaningless.
	const uint32 ReferenceGeneration = XR.GetReferenceSpaceGeneration();
	if (ReferenceGeneration != SeenReferenceGeneration)
	{
		SeenReferenceGeneration = ReferenceGeneration;
		if (!bEntryPending && !bCalibrationInvalid)
		{
			bCalibrationInvalid = true;
			ReleaseHeldInput(TEXT("reference space reset"));
			UE_LOG(LogWoTVR, Warning,
				TEXT("The VR reference space was reset: holding still until you recentre (%s or WoT.VR.Recentre)"),
				*WoTVRKeyName(UWoTVRSettings::Get().RecentreKey));
		}
	}

	if (bEntryPending)
	{
		if (bUsable)
		{
			CompleteEntry(Pose);
		}
		else
		{
			EntryElapsed += DeltaSeconds;
			if (EntryElapsed >= WoTVRTuning::StartTimeoutSeconds())
			{
				FailEntry(LexToString(State));
				return;
			}
		}
	}
	else if (!bUsable && !bHoldActive)
	{
		BeginHold(State != EWoTVRSessionState::Running ? LexToString(State) : TEXT("head not tracked"));
	}
	else if (bUsable && bHoldActive)
	{
		EndHold(Pose);
	}

	const bool bHold = bEntryPending || bHoldActive || bCalibrationInvalid;
	ApplyMovementBlock(bHold);
	// The headset shows the HUD and menus while the session is usable (an
	// invalidated calibration keeps them: the menu is how to recentre); a lost
	// session gives them back to the desktop window, which stays usable.
	UpdatePresentation(bUsable && !bEntryPending && !bHoldActive ? &Pose : nullptr);
	if (bUsable && !bHold)
	{
		UpdateViewKind(Pose);
		if (bFoldPending)
		{
			// Before this frame's aim sample: the spawn facing joins the heading.
			bFoldPending = false;
			FoldGameplayYaw(TEXT("spawn facing"));
		}
		if (bReanchorPending)
		{
			Rig.ReanchorHorizontal(Pose);
			bReanchorPending = false;
		}
		UpdateTurning(Pose, DeltaSeconds);
		UpdateGameplayYawSettle(DeltaSeconds);
	}
	else
	{
		bSnapArmed = false;
	}
	ApplyWorldScale();
	ApplyFade(bUsable && !bHold ? ComputeSafetyAlpha(Pose) : 0.0f, DeltaSeconds);
}

void UWoTVRPlayerSubsystem::CompleteEntry(const FWoTVRHeadPose& Pose)
{
	bEntryPending = false;
	// Pose origins are session-local: each VR start confirms the current pose
	// as neutral, which puts the head over the anchor; Recentre confirms another.
	Rig.Calibrate(Pose);
	bCalibrationInvalid = false;
	bReanchorPending = false;
	bFoldPending = false;
	LastValidPose = Pose;
	bHasLastValidPose = true;
	AlignHeadingToAim(Pose);
	ViewKind = ClassifyView();
	SpecialReferenceYaw = Rig.RelativeYaw(Pose);
	StartTransitionFade();
	UE_LOG(LogWoTVR, Display,
		TEXT("VR running: heading %.1f, %s posture; %s recentres, %s switches to flat"),
		Rig.HeadingYaw, WoTVRPostureName(UWoTVRSettings::Get().Posture),
		*WoTVRKeyName(UWoTVRSettings::Get().RecentreKey),
		*WoTVRKeyName(UWoTVRSettings::Get().ModeSwitchKey));
}

void UWoTVRPlayerSubsystem::FailEntry(const TCHAR* Why)
{
	const FString Reason = FString::Printf(
		TEXT("the headset session did not start within %.0f s (session %s)"),
		WoTVRTuning::StartTimeoutSeconds(), Why);
	switch (EntryKind)
	{
	case EWoTVRRequestKind::Explicit:
		UE_LOG(LogWoTVR, Error, TEXT("VR start failed: %s; continuing flat"), *Reason);
		break;
	case EWoTVRRequestKind::User:
		UE_LOG(LogWoTVR, Warning, TEXT("Switch to VR failed: %s; back to flat"), *Reason);
		break;
	case EWoTVRRequestKind::Auto:
	default:
		UE_LOG(LogWoTVR, Display, TEXT("Auto VR: %s; continuing flat"), *Reason);
		break;
	}
	ExitVR(TEXT("VR start failed"));
}

void UWoTVRPlayerSubsystem::BeginHold(const TCHAR* Why)
{
	bHoldActive = true;
	ReleaseHeldInput(TEXT("VR session not usable"));
	UE_LOG(LogWoTVR, Warning,
		TEXT("VR session %s: holding still with input released until it recovers (%s or WoT.VR.Mode Flat continues flat)"),
		Why, *WoTVRKeyName(UWoTVRSettings::Get().ModeSwitchKey));
}

void UWoTVRPlayerSubsystem::EndHold(const FWoTVRHeadPose& Pose)
{
	bHoldActive = false;
	if (!bCalibrationInvalid)
	{
		Rig.ReanchorHorizontal(Pose);
	}
	AlignHeadingToAim(Pose);
	StartTransitionFade();
	UE_LOG(LogWoTVR, Display, TEXT("VR session recovered"));
}

void UWoTVRPlayerSubsystem::UpdateViewKind(const FWoTVRHeadPose& Pose)
{
	const EWoTVRViewKind Kind = ClassifyView();
	if (Kind == ViewKind)
	{
		return;
	}
	const EWoTVRViewKind Previous = ViewKind;
	ViewKind = Kind;
	const bool bSpecial = Kind == EWoTVRViewKind::Remote || Kind == EWoTVRViewKind::Controller;
	const bool bWasSpecial = Previous == EWoTVRViewKind::Remote || Previous == EWoTVRViewKind::Controller;
	if (bSpecial)
	{
		// Into a special view: the head looks around from where it looks now.
		SpecialReferenceYaw = Rig.RelativeYaw(Pose);
		StartTransitionFade();
		UE_LOG(LogWoTVR, Verbose, TEXT("Special view: %s"),
			Kind == EWoTVRViewKind::Remote ? TEXT("remote camera") : TEXT("the controller's own camera"));
	}
	else if (bWasSpecial && Kind == EWoTVRViewKind::Body)
	{
		// Back in the body: the head over it again (the head may have moved
		// while it looked through another camera), and the aim continuing where
		// it was (the orbit held it while the head looked around).
		if (Rig.bCalibrated && !bCalibrationInvalid)
		{
			Rig.ReanchorHorizontal(Pose);
		}
		AlignHeadingToAim(Pose);
		StartTransitionFade();
	}
}

void UWoTVRPlayerSubsystem::AlignHeadingToAim(const FWoTVRHeadPose& Pose)
{
	// The current aim becomes the provider's own (no gameplay yaw left over),
	// and the heading maps the head's current direction onto it. During a
	// forced turn the gameplay yaw stays where it is, taken when the turn ends:
	// the head maps onto the aim without the forced share, so the forced
	// rotation never reaches the rendered head.
	ULocalPlayer* LocalPlayer = GetLocalPlayer();
	double Forced = 0.0;
	if (LocalPlayer && bRigAttached)
	{
		if (FWoTLocalPlayerHooks::IsForcedTurn(*LocalPlayer))
		{
			Forced = FWoTLocalPlayerHooks::PeekGameplayYaw(*LocalPlayer);
		}
		else
		{
			FWoTLocalPlayerHooks::TakeGameplayYaw(*LocalPlayer, RigOwner);
		}
	}
	double AimYaw = Rig.WorldYaw(Pose);
	if (const APlayerController* PC = Controller())
	{
		// The aim in play; before possession the camera's own direction.
		AimYaw = PC->GetPawn() || !PC->PlayerCameraManager
			? PC->GetControlRotation().Yaw
			: PC->PlayerCameraManager->GetCameraCacheView().Rotation.Yaw;
	}
	Rig.AlignHeading(Pose, FMath::UnwindDegrees(AimYaw - Forced));
}

void UWoTVRPlayerSubsystem::FoldGameplayYaw(const TCHAR* Why)
{
	ULocalPlayer* LocalPlayer = GetLocalPlayer();
	if (!LocalPlayer || !bRigAttached)
	{
		return;
	}
	if (FWoTLocalPlayerHooks::IsForcedTurn(*LocalPlayer))
	{
		// Never during a forced turn: its end folds everything (ForcedTurnEnded).
		UE_LOG(LogWoTVR, Verbose, TEXT("Gameplay yaw held for the forced turn's end (%s)"), Why);
		return;
	}
	const double Taken = FWoTLocalPlayerHooks::TakeGameplayYaw(*LocalPlayer, RigOwner);
	SettleElapsed = 0.0f;
	SettleYaw = 0.0;
	if (FMath::IsNearlyZero(Taken, 0.001))
	{
		return;
	}
	Rig.HeadingYaw = FMath::UnwindDegrees(Rig.HeadingYaw + Taken);
	StartTransitionFade();
	UE_LOG(LogWoTVR, Verbose, TEXT("Heading took %.2f degrees of gameplay yaw (%s)"), Taken, Why);
}

void UWoTVRPlayerSubsystem::UpdateTurning(const FWoTVRHeadPose& Pose, float DeltaSeconds)
{
	APlayerController* PC = Controller();
	UPlayerInput* Input = PC ? PC->PlayerInput.Get() : nullptr;
	const UWorld* World = PC ? PC->GetWorld() : nullptr;
	// Turning is gameplay input with the stick's gates: not while paused, while
	// a window holds the keys or while chat is typed (chat still passes stick
	// values through). A window's opening also flushed any held turn key.
	if (!Input || !World || World->IsPaused() || FWoTLocalPlayerHooks::IsGameInputBlocked(PC))
	{
		bSnapArmed = false;
		return;
	}
	const UWoTVRSettings& Settings = UWoTVRSettings::Get();
	float Deflection = Input->GetRawKeyValue(EKeys::Gamepad_RightX);
	if (IsTurnKeyFree(*Input, Settings.TurnLeftKey) && Input->IsPressed(Settings.TurnLeftKey))
	{
		Deflection -= 1.0f;
	}
	if (IsTurnKeyFree(*Input, Settings.TurnRightKey) && Input->IsPressed(Settings.TurnRightKey))
	{
		Deflection += 1.0f;
	}
	Deflection = FMath::Clamp(Deflection, -1.0f, 1.0f);
	const float Magnitude = FMath::Abs(Deflection);
	if (Settings.TurnMode == EWoTVRTurnMode::Smooth)
	{
		const float DeadZone = WoTVRTuning::SmoothDeadZone();
		if (Magnitude > DeadZone)
		{
			const float Scaled = (Magnitude - DeadZone) / FMath::Max(1.0f - DeadZone, KINDA_SMALL_NUMBER);
			const float Rate = FMath::Clamp(Settings.SmoothTurnDegreesPerSecond, 1.0f, 720.0f);
			Rig.Turn(Pose, FMath::Sign(Deflection) * Scaled * Rate * DeltaSeconds);
		}
		bSnapArmed = Magnitude < WoTVRTuning::SnapRearmThreshold();
		return;
	}
	if (!bSnapArmed)
	{
		bSnapArmed = Magnitude < WoTVRTuning::SnapRearmThreshold();
		return;
	}
	if (Magnitude >= WoTVRTuning::SnapTriggerThreshold())
	{
		const float Step = FMath::Clamp(Settings.SnapTurnDegrees, 1.0f, 180.0f);
		Rig.Turn(Pose, FMath::Sign(Deflection) * Step);
		bSnapArmed = false;
		++SnapTurnCount;
	}
}

bool UWoTVRPlayerSubsystem::IsTurnKeyFree(const UPlayerInput& Input, const FKey& Key)
{
	if (!Key.IsValid())
	{
		return false;
	}
	FName Taken;
	for (const FInputActionKeyMapping& Mapping : Input.ActionMappings)
	{
		if (Mapping.Key == Key)
		{
			Taken = Mapping.ActionName;
			break;
		}
	}
	if (Taken.IsNone())
	{
		for (const FInputAxisKeyMapping& Mapping : Input.AxisMappings)
		{
			if (Mapping.Key == Key)
			{
				Taken = Mapping.AxisName;
				break;
			}
		}
	}
	if (Taken.IsNone())
	{
		ReportedTurnKeys.Remove(Key);
		return true;
	}
	if (!ReportedTurnKeys.Contains(Key))
	{
		ReportedTurnKeys.Add(Key);
		UE_LOG(LogWoTVR, Display,
			TEXT("VR turn key %s is bound to the game's %s, which keeps it; WoT.VR.Key TurnLeft|TurnRight <key> moves VR turning"),
			*Key.ToString(), *Taken.ToString());
	}
	return false;
}

void UWoTVRPlayerSubsystem::UpdateGameplayYawSettle(float DeltaSeconds)
{
	const ULocalPlayer* LocalPlayer = GetLocalPlayer();
	if (!LocalPlayer || !bRigAttached || bAnchorFrozen || FWoTLocalPlayerHooks::IsForcedTurn(*LocalPlayer))
	{
		SettleElapsed = 0.0f;
		return;
	}
	// Other gameplay turns (a rotating base, a short place swap) leave the
	// rendered head alone while they change; once the aim's gameplay yaw has
	// held still, the view takes it behind a short fade.
	const double GameplayYaw = FWoTLocalPlayerHooks::PeekGameplayYaw(*LocalPlayer);
	if (FMath::Abs(GameplayYaw) < 0.5 || !FMath::IsNearlyEqual(GameplayYaw, SettleYaw, 0.01))
	{
		SettleYaw = GameplayYaw;
		SettleElapsed = 0.0f;
		return;
	}
	SettleElapsed += DeltaSeconds;
	if (SettleElapsed >= WoTVRTuning::GameplayYawSettleSeconds())
	{
		FoldGameplayYaw(TEXT("settled gameplay turn"));
	}
}

float UWoTVRPlayerSubsystem::ComputeSafetyAlpha(const FWoTVRHeadPose& Pose) const
{
	const APlayerController* PC = Controller();
	const APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	// Only the possessed body's own view has a capsule eye to guard; special
	// views (death, match end, the editor orbit, another view target) do not.
	if (!Pawn || bAnchorFrozen || PC->GetViewTarget() != Pawn)
	{
		return 0.0f;
	}
	UWorld* World = Pawn->GetWorld();
	if (!World)
	{
		return 0.0f;
	}
	// The swept path starts at the capsule's eye. The rendered centre eye is
	// the composed view's: the camera's eye (the simulation eye with synthetic
	// motion removed) plus the tracked offset, the head in it once.
	const FVector Eye = Pawn->GetPawnViewLocation();
	const FVector Anchor = PC->PlayerCameraManager ? PC->PlayerCameraManager->GetCameraCacheView().Location : Eye;
	const FVector Offset = Rig.WorldOffset(Pose);
	const FVector Centre = Anchor + Offset;
	const double Ramp = WoTVRTuning::FadeRamp();
	// Beyond the follow gap from the capsule's axis.
	const double Lead = FVector2D(Offset.X, Offset.Y).Size();
	double Alpha = FMath::Clamp((Lead - (WoTVRTuning::FollowGap() - Ramp)) / Ramp, 0.0, 1.0);
	// Rising above the standing eye.
	Alpha = FMath::Max(Alpha, FMath::Clamp((Offset.Z - WoTVRTuning::RiseMargin()) / Ramp, 0.0, 1.0));
	// Sinking into the floor.
	float Radius = 0.0f;
	float HalfHeight = 0.0f;
	Pawn->GetSimpleCollisionCylinder(Radius, HalfHeight);
	if (Centre.Z < Pawn->GetActorLocation().Z - HalfHeight + WoTVRTuning::FloorClearance())
	{
		return 1.0f;
	}
	// The swept path from the capsule's eye to each rendered eye: anything
	// within the probe's reach blacks the view out at once, so no eye and no
	// near plane ever reaches past a surface. Each eye sits where the engine
	// renders it: the centre eye plus that eye's own offset from the head, as
	// the runtime places it, turned with the head.
	const FQuat Orientation = Rig.WorldOrientation(Pose);
	TArray<FVector, TInlineAllocator<3>> RenderedEyes;
	RenderedEyes.Add(Centre);
	for (int32 Index = 0; Index < 2; ++Index)
	{
		FVector EyeMeters;
		if (Session().GetEyeOffsetMeters(Index, EyeMeters))
		{
			RenderedEyes.Add(Centre + Orientation.RotateVector(EyeMeters * WoTVR::UnitsPerMeter));
		}
	}
	FCollisionQueryParams Params(SCENE_QUERY_STAT(WoTVRViewSafety), false, Pawn);
	Params.bFindInitialOverlaps = false;
	const FCollisionShape Probe = FCollisionShape::MakeSphere(WoTVRTuning::ProbeRadius());
	for (const FVector& RenderedEye : RenderedEyes)
	{
		if ((RenderedEye - Eye).IsNearlyZero(0.01))
		{
			continue;
		}
		FHitResult Hit;
		if (World->SweepSingleByChannel(Hit, Eye, RenderedEye, FQuat::Identity, ECC_Camera, Probe, Params)
			&& Hit.bBlockingHit && !Hit.bStartPenetrating)
		{
			return 1.0f;
		}
	}
	return static_cast<float>(Alpha);
}

void UWoTVRPlayerSubsystem::StartTransitionFade()
{
	if (WoTVRTuning::TransitionFadeSeconds() > 0.0f && Mode == EWoTVRMode::VR)
	{
		// Black from the event's own frame: the view that frame may already show
		// the new place or heading.
		TransitionAlpha = 1.0f;
		FadeAlpha = 1.0f;
		PushFade();
	}
}

void UWoTVRPlayerSubsystem::ApplyFade(float SafetyTarget, float DeltaSeconds)
{
	// To black at once, back to clear gradually.
	if (SafetyTarget >= SafetyAlpha)
	{
		SafetyAlpha = SafetyTarget;
	}
	else
	{
		const float FadeIn = WoTVRTuning::FadeInSeconds();
		SafetyAlpha = FadeIn > 0.0f ? FMath::Max(SafetyTarget, SafetyAlpha - DeltaSeconds / FadeIn) : SafetyTarget;
	}
	FadeAlpha = FMath::Max(SafetyAlpha, TransitionAlpha);
	const float Transition = WoTVRTuning::TransitionFadeSeconds();
	TransitionAlpha = Transition > 0.0f ? FMath::Max(0.0f, TransitionAlpha - DeltaSeconds / Transition) : 0.0f;
	PushFade();
}

void UWoTVRPlayerSubsystem::PushFade()
{
	APlayerController* PC = Controller();
	if (FadedController.IsValid() && FadedController.Get() != PC)
	{
		ClearCameraFade();
	}
	APlayerCameraManager* Camera = PC ? PC->PlayerCameraManager.Get() : nullptr;
	if (!Camera)
	{
		return;
	}
	if (FadeAlpha > 0.0f)
	{
		Camera->SetManualCameraFade(FadeAlpha, FLinearColor::Black, false);
		FadedController = PC;
	}
	else if (FadedController.Get() == PC)
	{
		Camera->StopCameraFade();
		FadedController.Reset();
	}
}

void UWoTVRPlayerSubsystem::ClearCameraFade()
{
	if (APlayerController* PC = FadedController.Get())
	{
		if (PC->PlayerCameraManager)
		{
			PC->PlayerCameraManager->StopCameraFade();
		}
	}
	FadedController.Reset();
}

void UWoTVRPlayerSubsystem::ApplyMovementBlock(bool bBlock)
{
	// The hooks' hold, which belongs to the local player: it survives a respawn
	// (ClientRestart resets the controller's ignore-input state) and lifting it
	// never touches the game's own ignore-input count (the match-end freeze).
	if (ULocalPlayer* LocalPlayer = GetLocalPlayer(); LocalPlayer && bRigAttached)
	{
		FWoTLocalPlayerHooks::HoldMovement(*LocalPlayer, RigOwner, bBlock);
	}
}

void UWoTVRPlayerSubsystem::ApplyNearClipPlane()
{
	const float Near = WoTVRTuning::NearClip();
	if (!SavedNearClip.IsSet())
	{
		SavedNearClip = GNearClippingPlane;
	}
	if (GNearClippingPlane != Near)
	{
		// Both the game thread's and, through a render command ahead of this
		// frame's rendering, the render thread's copy.
		SetNearClipPlaneGlobals(Near);
	}
}

void UWoTVRPlayerSubsystem::RestoreNearClipPlane()
{
	if (SavedNearClip.IsSet())
	{
		if (GNearClippingPlane != *SavedNearClip)
		{
			SetNearClipPlaneGlobals(*SavedNearClip);
		}
		SavedNearClip.Reset();
	}
}

void UWoTVRPlayerSubsystem::SaveFrameCap()
{
	bFrameCapGuard = false;
	if (SavedMaxFPS.IsSet() || !GEngine)
	{
		// A quick switch back: the player's values are still the saved ones.
		return;
	}
	// A stereo start the engine made itself (-vr) has already uncapped the rate
	// and turned smoothing off: the cap configured at startup stands in.
	const bool bEngineStereo = Session().IsStereoEnabled();
	SavedMaxFPS = bEngineStereo ? FWoTVRModule::Get().GetStartupMaxFPS() : GEngine->GetMaxFPS();
	bSavedForceDisableSmoothing = bEngineStereo ? false : GEngine->bForceDisableFrameRateSmoothing;
}

void UWoTVRPlayerSubsystem::RestoreFrameCap()
{
	if (!SavedMaxFPS.IsSet() || !GEngine)
	{
		return;
	}
	if (GEngine->GetMaxFPS() != *SavedMaxFPS)
	{
		GEngine->SetMaxFPS(*SavedMaxFPS);
	}
	GEngine->bForceDisableFrameRateSmoothing = bSavedForceDisableSmoothing;
	// OpenXR writes the cap again while the stopped session ends.
	bFrameCapGuard = true;
}

void UWoTVRPlayerSubsystem::GuardFrameCap()
{
	if (!bFrameCapGuard)
	{
		return;
	}
	if (!SavedMaxFPS.IsSet() || !GEngine)
	{
		bFrameCapGuard = false;
		return;
	}
	// Only OpenXR's own writes are undone (its paused idle rate on STOPPING, an
	// uncapped rate on EXITING), never a cap the player has just chosen.
	const float Current = GEngine->GetMaxFPS();
	if (Current != *SavedMaxFPS
		&& (FMath::IsNearlyZero(Current) || FMath::IsNearlyEqual(Current, WoTVROpenXRPausedIdleFPS())))
	{
		GEngine->SetMaxFPS(*SavedMaxFPS);
	}
	if (Session().GetState() != EWoTVRSessionState::Ending)
	{
		bFrameCapGuard = false;
		SavedMaxFPS.Reset();
	}
}

void UWoTVRPlayerSubsystem::ApplyWorldScale()
{
	// The headset's eye separation and tracked positions use the world's scale:
	// 52.5 units per metre while VR runs; the map's own value comes back after.
	UWorld* World = PlayerWorld();
	AWorldSettings* Settings = World ? World->GetWorldSettings() : nullptr;
	if (!Settings)
	{
		return;
	}
	if (ScaledWorld.Get() != World)
	{
		RestoreWorldScale();
		ScaledWorld = World;
		OriginalWorldToMeters = Settings->WorldToMeters;
	}
	Settings->WorldToMeters = static_cast<float>(WoTVR::UnitsPerMeter);
}

void UWoTVRPlayerSubsystem::RestoreWorldScale()
{
	if (UWorld* World = ScaledWorld.Get())
	{
		if (AWorldSettings* Settings = World->GetWorldSettings())
		{
			Settings->WorldToMeters = OriginalWorldToMeters;
		}
	}
	ScaledWorld.Reset();
}

void UWoTVRPlayerSubsystem::UpdatePresentation(const FWoTVRHeadPose* Pose)
{
	if (bPresentationTravelling && PlayerWorld() != PresentationTravelWorld.Get())
	{
		// Travel arrived in another world: present again there.
		bPresentationTravelling = false;
		PresentationTravelWorld.Reset();
	}
	const bool bWanted = Pose && Pose->bValid && Mode == EWoTVRMode::VR && !bEntryPending
		&& !bHoldActive && !bPresentationTravelling;
	if (!bWanted)
	{
		// Kept for the next attach in this VR session, hidden and unpainted.
		DetachPresentation(bPresentationTravelling ? TEXT("travel") : TEXT("VR session not usable"));
		return;
	}
	if ((Presentation && Presentation->IsAttached()) || bPresentationRefused)
	{
		return;
	}
	ULocalPlayer* LocalPlayer = GetLocalPlayer();
	if (!LocalPlayer)
	{
		return;
	}
	if (!Presentation)
	{
		Presentation = MakeShared<FWoTVRPresentation>(*LocalPlayer);
	}
	// After travel, a lost session or the runtime's dashboard, the surfaces
	// kept from the last attach in this VR session come back as they were.
	const TSharedPtr<FWoTVRPresentation> Attaching = Presentation;
	if (!Attaching->Attach(*Pose))
	{
		// Logged by the presentation, which kept nothing; the desktop window
		// keeps the HUD and menus until VR starts again.
		bPresentationRefused = true;
		if (Presentation == Attaching)
		{
			Presentation.Reset();
		}
	}
}

FWoTVRPresentation* UWoTVRPlayerSubsystem::GetPresentation() const
{
	return Presentation && Presentation->IsAttached() ? Presentation.Get() : nullptr;
}

void UWoTVRPlayerSubsystem::DetachPresentation(const TCHAR* Why)
{
	// Held here: a callback inside the detach may release it meanwhile.
	const TSharedPtr<FWoTVRPresentation> Detaching = Presentation;
	if (Detaching && Detaching->IsAttached())
	{
		Detaching->Detach();
		UE_LOG(LogWoTVR, Log, TEXT("Headset HUD and menus handed back to the desktop window (%s)"), Why);
	}
}

void UWoTVRPlayerSubsystem::ReleasePresentation(const TCHAR* Why)
{
	if (!Presentation)
	{
		return;
	}
	// Taken first: the release's own callbacks find no presentation to act on.
	const TSharedPtr<FWoTVRPresentation> Releasing = MoveTemp(Presentation);
	Presentation.Reset();
	const bool bWasAttached = Releasing->IsAttached();
	Releasing->Release();
	if (bWasAttached)
	{
		UE_LOG(LogWoTVR, Log, TEXT("Headset HUD and menus handed back to the desktop window (%s)"), Why);
	}
}

bool UWoTVRPlayerSubsystem::IsHoldingStill() const
{
	return Mode == EWoTVRMode::VR && (bEntryPending || bHoldActive || bCalibrationInvalid);
}

// --- diagnostics -----------------------------------------------------------

void UWoTVRPlayerSubsystem::LogDiagnostics(const TCHAR* Why) const
{
	const IWoTVRSession& XR = Session();
	const FWoTVRDiagnostics Diagnostics = XR.GetDiagnostics();
	if (!Diagnostics.bAvailable)
	{
		UE_LOG(LogWoTVR, Display, TEXT("VR diagnostics (%s): no XR runtime (%s session %s)"), Why,
			*XR.GetKind().ToString(), LexToString(XR.GetState()));
		return;
	}
	UE_LOG(LogWoTVR, Display,
		TEXT("VR diagnostics (%s): %s session %s; runtime '%s' version %s; system '%s'"), Why,
		*XR.GetKind().ToString(), LexToString(XR.GetState()), *Diagnostics.RuntimeName,
		*Diagnostics.RuntimeVersion, *Diagnostics.SystemName);
	UE_LOG(LogWoTVR, Display, TEXT("VR diagnostics: %d enabled OpenXR extensions: %s"),
		Diagnostics.EnabledExtensions.Num(), *FString::Join(Diagnostics.EnabledExtensions, TEXT(", ")));
	UE_LOG(LogWoTVR, Display, TEXT("VR diagnostics: interaction profiles: left hand %s, right hand %s"),
		*Diagnostics.LeftHandProfile, *Diagnostics.RightHandProfile);
}

FString UWoTVRPlayerSubsystem::DescribeStatus() const
{
	const UWoTVRSettings& Settings = UWoTVRSettings::Get();
	IWoTVRSession& XR = Session();
	const FWoTVRGateDecision& Gate = FWoTVRGate::GetProcessDecision();
	// For the headset checks: the near plane every projection starts at, and
	// the head offset the engine adds to each eye of a view without a camera
	// (the composed view leaves it out, so the head is in each eye once).
	const FVector EngineHead = XR.GetEngineAddedHeadOffset(Controller(), {});
	FVector LeftEye;
	const bool bLeftEye = XR.GetEyeOffsetMeters(0, LeftEye);
	return FString::Printf(
		TEXT("WoTVR: %s, mode %s%s%s; session %s (%s); XR gate %s (%s); master enable %s; startup %s; ")
		TEXT("posture %s; turn %s (%.0f deg snap, %.0f deg/s smooth); heading %.1f; calibrated %s; ")
		TEXT("fade %.2f; view %s; near plane %.2f (engine %.2f); engine head offset %s; left eye %s m; ")
		TEXT("keys: switch %s, recentre %s, turn %s/%s; %s"),
		bActive ? TEXT("active") : TEXT("inactive"),
		Mode == EWoTVRMode::VR ? TEXT("VR") : TEXT("Flat"),
		bEntryPending ? TEXT(" (starting)") : TEXT(""),
		IsHoldingStill() && !bEntryPending ? TEXT(" (holding still)") : TEXT(""),
		*XR.GetKind().ToString(), LexToString(XR.GetState()),
		Gate.bXRAllowed ? TEXT("open") : TEXT("closed"), *Gate.Reason,
		Settings.bEnableVR ? TEXT("on") : TEXT("off"),
		*UEnum::GetValueAsString(Settings.StartupMode),
		WoTVRPostureName(Settings.Posture), WoTVRTurnModeName(Settings.TurnMode),
		Settings.SnapTurnDegrees, Settings.SmoothTurnDegreesPerSecond,
		Rig.HeadingYaw, Rig.bCalibrated && !bCalibrationInvalid ? TEXT("yes") : TEXT("no"),
		FadeAlpha, WoTVRViewKindName(ClassifyView()), WoTVRTuning::NearClip(), GNearClippingPlane,
		*EngineHead.ToCompactString(), bLeftEye ? *LeftEye.ToCompactString() : TEXT("unknown"),
		*WoTVRKeyName(Settings.ModeSwitchKey), *WoTVRKeyName(Settings.RecentreKey),
		*WoTVRKeyName(Settings.TurnLeftKey), *WoTVRKeyName(Settings.TurnRightKey),
		Presentation ? *Presentation->Describe() : TEXT("headset HUD and menus detached"));
}
