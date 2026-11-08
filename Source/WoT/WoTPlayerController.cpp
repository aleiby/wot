#include "WoTPlayerController.h"
#include "Gamepad/WoTGamepadLayer.h"
#include "EnhancedInputComponent.h"
#include "WoTRandom.h"
#include "WoTSound.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundBase.h"

#include "WoTGameMode.h"
#include "WoTGameInstance.h"
#include "WoTFrontEnd.h"
#include "WoTBattleGameMode.h"
#include "WoTBattleInfo.h"
#include "WoTBudgetInfo.h"
#include "WoTCharacter.h"
#include "WoTCollisionBlocker.h"
#include "WoTCitadelResources.h"
#include "WoTHUD.h"
#include "WoTInGameSlate.h"
#include "WoTMenuStyle.h"
#include "WoTPlayerState.h"
#include "WoTBootstrapHeartbeat.h"
#include "WoTOnboardingSeams.h"
#include "WoTPreferencesSubsystem.h"
#include "Settings/WoTGameSettings.h"
#include "WoTRetailText.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/LightComponent.h"
#include "EngineUtils.h"
#include "TimerManager.h"
#include "WoTSealAltar.h"
#include "WoTSessionSubsystem.h"
#include "WoTZoneVolume.h"
#include "WoTSpriteBillboard.h"
#include "World/WoTMapDisplay.h"
#include "WoTVertexAnimComponent.h"
#include "AI/WoTBrainComponent.h"
#include "AI/WoTCreatureData.h"
#include "AI/WoTNPCCharacter.h"
#include "AI/WoTNPCController.h"
#include "Inventory/WoTInventory.h"
#include "Inventory/WoTHandInfo.h"
#include "Inventory/WoTHandSet.h"
#include "Inventory/WoTSeal.h"
#include "Projectiles/WoTMashadarGuide.h"
#include "Traps/WoTPortcullis.h"
#include "Traps/WoTStaircase.h"
#include "Traps/WoTTrap.h"

#include "EngineUtils.h"
#include "Engine/Engine.h"
#include "Engine/EngineBaseTypes.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/NetDriver.h"
#include "Engine/UserInterfaceSettings.h"
#include "Engine/World.h"
#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/SlateUser.h"
#include "UnrealClient.h"
#include "Camera/CameraTypes.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SphereComponent.h"
#include "Components/SceneComponent.h"
#include "Components/InputComponent.h"
#include "Framework/Application/SlateApplication.h"
#include "CollisionQueryParams.h"
#include "Misc/App.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/InputSettings.h"
#include "GameFramework/PlayerInput.h"

class FWoTChatInputPreProcessor final : public IInputProcessor
{
public:
	FWoTChatInputPreProcessor(AWoTPlayerController* InController,
		TSet<FKey>&& InHeldGameplayMouseButtons, uint32 InUserIndex,
		TWeakPtr<SViewport> InViewportWidget)
		: Controller(InController)
		, HeldGameplayMouseButtons(MoveTemp(InHeldGameplayMouseButtons))
		, UserIndex(InUserIndex)
		, ViewportWidget(MoveTemp(InViewportWidget))
	{
	}

	virtual void Tick(float, FSlateApplication&, TSharedRef<ICursor>) override
	{
	}

	virtual bool HandleKeyUpEvent(FSlateApplication&,
		const FKeyEvent& Event) override
	{
		return Dispatch(Event.GetKey(), IE_Released, 0.0f,
			Event.GetUserIndex());
	}

	virtual bool HandleAnalogInputEvent(FSlateApplication&,
		const FAnalogInputEvent& Event) override
	{
		return Dispatch(Event.GetKey(), IE_Axis, Event.GetAnalogValue(),
			Event.GetUserIndex());
	}

	virtual bool HandleMouseWheelOrGestureEvent(FSlateApplication&,
		const FPointerEvent& WheelEvent,
		const FPointerEvent* GestureEvent) override
	{
		// Gestures are Slate input in this port. A physical wheel follows
		// FSceneViewport::OnMouseWheel, except retail Typing eats its press.
		if (GestureEvent || WheelEvent.GetUserIndex() != UserIndex
			|| !Controller.IsValid())
		{
			return false;
		}
		const float WheelDelta = WheelEvent.GetWheelDelta();
		if (WheelDelta == 0.0f)
		{
			return false;
		}
		const FKey WheelKey = WheelDelta < 0.0f
			? EKeys::MouseScrollDown : EKeys::MouseScrollUp;
		Controller->InputKey(FInputKeyEventArgs(nullptr,
			WheelEvent.GetInputDeviceId(), WheelKey, IE_Released, 1.0f,
			WheelEvent.IsTouchEvent(), WheelEvent.GetEventTimestamp()));
		Controller->InputKey(FInputKeyEventArgs(nullptr,
			WheelEvent.GetInputDeviceId(), EKeys::MouseWheelAxis, WheelDelta,
			FApp::GetDeltaTime(), 1, WheelEvent.GetEventTimestamp()));
		return true;
	}

	virtual bool HandleMouseButtonDownEvent(FSlateApplication&,
		const FPointerEvent& Event) override
	{
		// Console Typing consumes every press. A mouse press must therefore reach
		// neither gameplay nor the editable Slate projection; releases and axes
		// keep following the gameplay route below.
		return Event.GetUserIndex() == UserIndex && Controller.IsValid();
	}

	virtual bool HandleMouseButtonDoubleClickEvent(FSlateApplication&,
		const FPointerEvent& Event) override
	{
		return Event.GetUserIndex() == UserIndex && Controller.IsValid();
	}

	virtual bool HandleMotionDetectedEvent(FSlateApplication&,
		const FMotionEvent& Event) override
	{
		// Motion is axis input: Typing passes every non-press action through,
		// so forward it as FSceneViewport::OnMotionDetected would.
		if (Event.GetUserIndex() != UserIndex || !Controller.IsValid())
		{
			return false;
		}
		Controller->InputMotion(Event.GetInputDeviceId(), Event.GetTilt(),
			Event.GetRotationRate(), Event.GetGravity(), Event.GetAcceleration(),
			Event.GetEventTimestamp());
		return true;
	}

	virtual bool HandleMouseMoveEvent(FSlateApplication&,
		const FPointerEvent& Event) override
	{
		if (Event.GetUserIndex() != UserIndex || !Controller.IsValid())
		{
			return false;
		}
		if (Event.IsTouchEvent())
		{
			return true;
		}
		const FVector2D Delta = Event.GetCursorDelta();
		Controller->InputKey(FInputKeyEventArgs(nullptr, INPUTDEVICEID_NONE,
			EKeys::MouseX, IE_Axis, Delta.X, false, 0));
		Controller->InputKey(FInputKeyEventArgs(nullptr, INPUTDEVICEID_NONE,
			EKeys::MouseY, IE_Axis, -Delta.Y, false, 0));
		return true;
	}

	virtual bool HandleMouseButtonUpEvent(FSlateApplication& Slate,
		const FPointerEvent& Event) override
	{
		if (Event.IsTouchEvent())
		{
			return Event.GetUserIndex() == UserIndex && Controller.IsValid();
		}
		if (!Dispatch(Event.GetEffectingButton(), IE_Released, 0.0f,
			Event.GetUserIndex()))
		{
			return false;
		}
		if (HeldGameplayMouseButtons.Remove(Event.GetEffectingButton()) > 0
			&& HeldGameplayMouseButtons.IsEmpty())
		{
			ReleaseGameplayMouseCapture(Slate);
		}
		return true;
	}

	virtual const TCHAR* GetDebugName() const override
	{
		return TEXT("WoT retail chat input router");
	}

	bool HasHeldGameplayMouseButton() const
	{
		return !HeldGameplayMouseButtons.IsEmpty();
	}

	void MarkGameplayMouseCaptureRetained()
	{
		bGameplayMouseCaptureRetained = true;
	}

	void ReleaseGameplayMouseCapture(FSlateApplication& Slate)
	{
		if (!bGameplayMouseCaptureRetained)
		{
			return;
		}
		if (const TSharedPtr<SViewport> Viewport = ViewportWidget.Pin())
		{
			FWidgetPath ViewportPath;
			if (Slate.GeneratePathToWidgetUnchecked(
				Viewport.ToSharedRef(), ViewportPath))
			{
				Slate.ProcessExternalReply(ViewportPath,
					FReply::Handled().ReleaseMouseCapture(),
					static_cast<int32>(UserIndex),
					static_cast<int32>(
						FSlateApplicationBase::CursorPointerIndex));
			}
		}
		bGameplayMouseCaptureRetained = false;
	}

private:
	bool Dispatch(const FKey& Key, EInputEvent Event, float Amount,
		uint32 EventUserIndex)
	{
		if (EventUserIndex != UserIndex || !Controller.IsValid())
		{
			return false;
		}
		Controller->InputKey(FInputKeyEventArgs(nullptr, INPUTDEVICEID_NONE,
			Key, Event, Amount, false, 0));
		return true;
	}

	TWeakObjectPtr<AWoTPlayerController> Controller;
	TSet<FKey> HeldGameplayMouseButtons;
	uint32 UserIndex;
	TWeakPtr<SViewport> ViewportWidget;
	bool bGameplayMouseCaptureRetained = false;
};

namespace
{
	constexpr float DeathScoreboardAutoShowDelaySecs = 2.0f;

	FString WoTPlayerControllerRetailText(
		FStringView Section, FStringView Key, const TCHAR* Fallback)
	{
		if (FWoTRetailText::EnsureLoaded())
		{
			if (const FString* Value = FWoTRetailText::FindSectionValue(Section, Key))
			{
				return *Value;
			}
		}
		return FString(Fallback);
	}

	bool WoTPlayerControllerBindsRetailPackage(FStringView PackageName)
	{
		for (const TCHAR* BoundPackage : FWoTRetailText::BoundPackages())
		{
			if (PackageName.Equals(FStringView(BoundPackage), ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	uint8 CitadelControllerTeam(const AController* Controller)
	{
		const AWoTPlayerState* PS =
			Controller ? Controller->GetPlayerState<AWoTPlayerState>() : nullptr;
		return PS ? PS->Team : uint8(255);
	}

	// WOTZoneInfo.IsZoneVisible (WOTZoneInfo.uc:438-441): a zone is visible to a viewer
	// when it is neutral (Team=255, the WOTZoneInfo default) or matches the viewer's
	// team. A point in no zone volume reads visible (the UE1 base ZoneInfo carries no
	// team).
	bool CitadelZoneVisibleAt(const UWorld* World, const FVector& Point, uint8 ViewerTeam)
	{
		const AWoTZoneVolume* Zone = AWoTZoneVolume::EditZoneAt(World, Point);
		return !IsValid(Zone) || Zone->EditTeam == 255 || Zone->EditTeam == ViewerTeam;
	}

	TArray<AWoTZoneVolume*> CitadelVisibleOrbitZones(
		const AWoTPlayerController* Controller)
	{
		TArray<AWoTZoneVolume*> Zones;
		if (!Controller || !Controller->GetWorld()) return Zones;
		const uint8 ViewerTeam = CitadelControllerTeam(Controller);
		for (TActorIterator<AWoTZoneVolume> It(Controller->GetWorld()); It; ++It)
		{
			AWoTZoneVolume* Zone = *It;
			if (IsValid(Zone) && Zone->ProjectedZoneIndex >= 1
				&& Zone->ProjectedZoneIndex <= 63
				&& (Zone->EditTeam == 255 || Zone->EditTeam == ViewerTeam))
			{
				Zones.Add(Zone);
			}
		}
		Zones.Sort([](const AWoTZoneVolume& Left, const AWoTZoneVolume& Right)
		{
			return Left.ProjectedZoneIndex < Right.ProjectedZoneIndex;
		});
		return Zones;
	}

	bool CitadelResourceIsActive(const AWoTTrap* Resource)
	{
		if (const AWoTPortcullis* Portcullis = Cast<AWoTPortcullis>(Resource))
		{
			return Portcullis->IsActive();
		}
		if (const AWoTStaircase* Staircase = Cast<AWoTStaircase>(Resource))
		{
			return Staircase->IsActive();
		}
		return false;
	}

	void CitadelResourceActivate(AWoTTrap* Resource)
	{
		if (AWoTPortcullis* Portcullis = Cast<AWoTPortcullis>(Resource))
		{
			Portcullis->Activate();
		}
		else if (AWoTStaircase* Staircase = Cast<AWoTStaircase>(Resource))
		{
			Staircase->Activate();
		}
	}

	void CitadelResourceDeactivate(AWoTTrap* Resource)
	{
		if (AWoTPortcullis* Portcullis = Cast<AWoTPortcullis>(Resource))
		{
			Portcullis->Deactivate();
		}
		else if (AWoTStaircase* Staircase = Cast<AWoTStaircase>(Resource))
		{
			Staircase->Deactivate();
		}
	}

	FWoTCitadelEditorSelection CitadelEditorSelection(
		FName AdapterName, int32 Count)
	{
		FWoTCitadelEditorSelection Selection;
		const FWoTResourceDescriptor& Descriptor =
			WoTCitadelResources::FindByAdapterName(AdapterName);
		if (Descriptor.IsValidDescriptor())
		{
			Selection.ResourceClass = Descriptor.ResourceClass;
			Selection.Count = Count;
			Selection.bSpawnTempResource = Descriptor.bSpawnTempResource;
		}
		return Selection;
	}

	bool CalculateCitadelTroopPlacement(const APawn* PlayerPawn,
		FName CreatureRow, bool bSpawnAdjusted, FVector& OutLocation,
		FRotator& OutRotation)
	{
		const FWoTCreatureRow* Row = WoTCreatureData::Find(CreatureRow);
		if (!IsValid(PlayerPawn) || !Row)
		{
			return false;
		}
		// WOTPawn.uc:617-635 CalcLocation/AdjustSpawnLocation: flatten pitch,
		// then use player radius + pawn CDO radius + SpawnSeparation=50.
		OutRotation = PlayerPawn->GetActorRotation();
		OutRotation.Pitch = 0.0f;
		const float PlayerRadius = PlayerPawn->GetSimpleCollisionRadius();
		OutLocation = PlayerPawn->GetActorLocation()
			+ OutRotation.Vector()
				* (PlayerRadius + Row->CollisionRadius + 50.0f);
		if (bSpawnAdjusted)
		{
			OutLocation.Z += 16.0f;
		}
		return true;
	}

	// Row identity remains shared by the Mashadar guide/deploy/remove paths even
	// though collision is now entirely row-driven in WoTCreatureData.
	bool IsMashadarFocusPawn(const AWoTNPCCharacter* Troop)
	{
		return IsValid(Troop)
			&& Troop->CreatureRowName == FName(TEXT("MashadarFocusPawn"));
	}

	AWoTMashadarGuide* FindClosestBindableMashadarGuide(
		UWorld* World, const FVector& Position)
	{
		if (!World)
		{
			return nullptr;
		}
		AWoTMashadarGuide* Closest = nullptr;
		double ClosestDistanceSq = TNumericLimits<double>::Max();
		for (TActorIterator<AWoTMashadarGuide> It(World); It; ++It)
		{
			AWoTMashadarGuide* Guide = *It;
			if (!IsValid(Guide) || !Guide->CanBindFocusAt(Position))
			{
				continue;
			}
			const double DistanceSq = FVector::DistSquared(
				Position, Guide->HidingPlaceLocation);
			if (DistanceSq < ClosestDistanceSq)
			{
				Closest = Guide;
				ClosestDistanceSq = DistanceSq;
			}
		}
		return Closest;
	}
}

bool AWoTPlayerController::PreTeleport()
{
	const TCHAR* Key = nullptr;
	const TCHAR* Fallback = nullptr;
	if (IsEditingCitadel())
	{
		Key = TEXT("TeleportProhibitedEditingStr");
		Fallback = TEXT("Teleport unavailable while editing.");
	}
	else if (bTeleportingDisabled)
	{
		Key = TEXT("TeleportProhibitedJoinStr");
		Fallback = TEXT("Teleport unavailable until battle begins.");
	}
	else
	{
		return false;
	}
	if (HasAuthority() && GetWorld()
		&& GetWorld()->GetTimeSeconds() > LastTeleportProhibitedMessageTime + 4.0f)
	{
		LastTeleportProhibitedMessageTime = GetWorld()->GetTimeSeconds();
		ClientCenterMessage(WoTPlayerControllerRetailText(TEXT("WOTPlayer"), Key, Fallback));
	}
	return true;
}

void AWoTPlayerController::BeginPlay()
{
	Super::BeginPlay();
	if (IsLocalController() && PlayerInput && GetGameInstance())
	{
		if (UWoTPreferencesSubsystem* Preferences =
			GetGameInstance()->GetSubsystem<UWoTPreferencesSubsystem>())
		{
			if (Preferences->HasLoadedFullPreferences())
			{
				Preferences->ApplyGameSettingsToInput(*PlayerInput);
			}
		}
	}
	ClearProgressMessages();
	BindProgressFailureDelegates();
}

void AWoTPlayerController::ClientReturnToMainMenuWithTextReason_Implementation(
	const FText& ReturnReason)
{
	// APlayerController's implementation throws ReturnReason away before it
	// disconnects.  Publish the server-authored NMT_Failure equivalent while this
	// owning client still has its world/driver so the normal progress and front-end
	// failure consumers receive the exact reason.
	if (GEngine && GetNetMode() == NM_Client)
	{
		GEngine->OnNetworkFailure().Broadcast(GetWorld(), GetNetDriver(),
			ENetworkFailure::FailureReceived, ReturnReason.ToString());
	}
	Super::ClientReturnToMainMenuWithTextReason_Implementation(ReturnReason);
}

void AWoTPlayerController::BindProgressFailureDelegates()
{
	UnbindProgressFailureDelegates();
	if (!GEngine)
	{
		return;
	}
	ProgressNetworkFailureHandle = GEngine->OnNetworkFailure().AddWeakLambda(this,
		[this](UWorld* FailedWorld, UNetDriver* NetDriver,
			ENetworkFailure::Type FailureType, const FString& ErrorString)
		{
			if (!IsLocalController() ||
				(FailedWorld && FailedWorld != GetWorld()))
			{
				return;
			}
			if (!FailedWorld && NetDriver && GEngine)
			{
				const FWorldContext* Context =
					GEngine->GetWorldContextFromPendingNetGameNetDriver(NetDriver);
				if (!Context || Context->OwningGameInstance != GetGameInstance())
				{
					return;
				}
			}
			PopulateNetworkFailureProgress(
				ENetworkFailure::ToString(FailureType), ErrorString);
		});
	ProgressTravelFailureHandle = GEngine->OnTravelFailure().AddWeakLambda(this,
		[this](UWorld* FailedWorld, ETravelFailure::Type FailureType,
			const FString& ErrorString)
		{
			if (!IsLocalController() || !FailedWorld || FailedWorld != GetWorld())
			{
				return;
			}
			ClearProgressMessages();
			SetProgressMessage(TEXT("Travel failed"), 0);
			SetProgressMessage(ErrorString.IsEmpty()
				? ETravelFailure::ToString(FailureType) : ErrorString, 1);
			SetProgressTime(6.0f);
		});
}

void AWoTPlayerController::UnbindProgressFailureDelegates()
{
	if (!GEngine)
	{
		ProgressNetworkFailureHandle.Reset();
		ProgressTravelFailureHandle.Reset();
		return;
	}
	if (ProgressNetworkFailureHandle.IsValid())
	{
		GEngine->OnNetworkFailure().Remove(ProgressNetworkFailureHandle);
		ProgressNetworkFailureHandle.Reset();
	}
	if (ProgressTravelFailureHandle.IsValid())
	{
		GEngine->OnTravelFailure().Remove(ProgressTravelFailureHandle);
		ProgressTravelFailureHandle.Reset();
	}
}

float AWoTPlayerController::ClampCitadelEditViewDistanceFeet(float DistanceFeet)
{
	return FMath::Clamp(DistanceFeet, 5.0f, 100.0f);
}

float AWoTPlayerController::CitadelEditViewDistanceFeetToUU(float DistanceFeet)
{
	return DistanceFeet * 16.0f;
}

float AWoTPlayerController::AdjustCitadelEditViewDistanceFeet(
	float CurrentFeet, float AForward, float DeltaTime)
{
	return ClampCitadelEditViewDistanceFeet(
		CurrentFeet - AForward * DeltaTime / 16.0f);
}

void AWoTPlayerController::CalculateCitadelOrbitView(const FVector& ZoneLocation,
	const FRotator& EditViewAngle, float EditViewDistanceFeet,
	FVector& OutLocation, FRotator& OutRotation, const FRotator& ViewOffset)
{
	// The camera sits on the authored angle; ViewOffset only turns the view.
	const FRotator Orbit = EditViewAngle.GetNormalized();
	OutRotation = (Orbit + ViewOffset).GetNormalized();
	OutLocation = ZoneLocation - Orbit.Vector()
		* CitadelEditViewDistanceFeetToUU(
			ClampCitadelEditViewDistanceFeet(EditViewDistanceFeet));
}

FRotator AWoTPlayerController::AdjustCitadelEditViewAngle(
	const FRotator& EditViewAngle, float AStrafe, float DeltaTime)
{
	// PlayerTickEdit: EditViewAngle.Yaw -= 24 * aStrafe * DeltaTime, in UE1 rotation units.
	FRotator Result = EditViewAngle;
	Result.Yaw -= 24.0f * AStrafe * DeltaTime * 360.0f / 65536.0f;
	return Result.GetNormalized();
}

int32 AWoTPlayerController::GetOwnerCitadelBudgetCount(FName AdapterName) const
{
	// EditorHUD reads the per-player SealInventory card (Count=1), not the
	// separate team SealBudget (EditorHUD.uc:178,270; SealInventory.uc:19).
	if (AdapterName == FName(TEXT("SealInventory")))
	{
		return OwnerSealCardCount;
	}
	for (const FWoTCitadelBudgetCount& Entry : OwnerEditBudget)
	{
		if (Entry.AdapterName == AdapterName)
		{
			return Entry.ItemCount;
		}
	}
	return 0;
}

FName AWoTPlayerController::GetOwnerCitadelTroopAdapter(FName TierKeyword) const
{
	if (TierKeyword.IsNone())
	{
		return NAME_None;
	}
	for (const FWoTCitadelBudgetCount& Entry : OwnerEditBudget)
	{
		if (!Entry.AdapterName.IsNone()
			&& WoTCitadelResources::IsAdapterForTier(
				Entry.AdapterName, TierKeyword))
		{
			return Entry.AdapterName;
		}
	}
	return NAME_None;
}

void AWoTPlayerController::RefreshOwnerCitadelBudgetMirror()
{
	if (!HasAuthority())
	{
		return;
	}
	OwnerEditBudget.Reset();
	if (IsValid(EditSessionBudget.Get()))
	{
		OwnerEditBudget.Reserve(EditSessionBudget->GetArrayCount());
		for (int32 BudgetIndex = 0;
			BudgetIndex < EditSessionBudget->GetArrayCount(); ++BudgetIndex)
		{
			// Only registered citadel resource rows cross the wire. The registry holds the
			// troop and SealInventory cards for the hand population consumer.
			const FName AdapterName =
				EditSessionBudget->GetResourceAdapterName(BudgetIndex);
			if (!WoTCitadelResources::FindByAdapterName(AdapterName).IsValidDescriptor())
			{
				continue;
			}
			FWoTCitadelBudgetCount& Entry = OwnerEditBudget.AddDefaulted_GetRef();
			Entry.AdapterName = AdapterName;
			Entry.ItemCount = EditSessionBudget->GetItemCount(BudgetIndex);
		}
	}
	OnRep_OwnerEditBudget();
	ForceNetUpdate();
}

void AWoTPlayerController::ApplyOwnerCitadelEditState()
{
	if (!IsLocalController())
	{
		return;
	}
	const bool bEditing = IsOwnerEditingCitadel();
	ClearCitadelSelectionPresentation();
	if (AWoTCharacter* PawnChar = Cast<AWoTCharacter>(GetPawn()))
	{
		PawnChar->HandleCitadelEditStateChanged(bEditing);
		if (bEditing && PawnChar->CurrentHandSet
			&& PawnChar->CurrentHandSet->GetSelectedHand())
		{
			SetCitadelPaletteSelection(PawnChar->CurrentHandSet
				->GetSelectedHand()->GetSelectedClassName());
		}
	}
	OnCitadelEditSessionChanged.Broadcast(bEditing, OwnerEditSessionTeam);
}

void AWoTPlayerController::ApplyCitadelEditCameraState()
{
	if (!IsLocalController())
	{
		return;
	}
	if (CitadelEditCameraMode == EWoTCitadelEditCameraMode::Orbit
		&& IsValid(CitadelEditZone.Get()))
	{
		SetViewTarget(this);
	}
	else if (APawn* ViewerPawn = GetPawn(); IsValid(ViewerPawn))
	{
		SetViewTarget(ViewerPawn);
	}
}

void AWoTPlayerController::CaptureCitadelEditStart(
	EWoTCitadelEditCameraMode Mode)
{
	if (Mode != EWoTCitadelEditCameraMode::Orbit)
	{
		EditStartLoc = FVector::ZeroVector;
		EditStartRot = FRotator::ZeroRotator;
		return;
	}
	if (const APawn* ViewerPawn = GetPawn(); IsValid(ViewerPawn))
	{
		EditStartLoc = ViewerPawn->GetActorLocation();
	}
	else if (const USceneComponent* Root = GetRootComponent())
	{
		EditStartLoc = Root->GetComponentLocation();
	}
	else
	{
		EditStartLoc = FVector::ZeroVector;
	}
	EditStartRot = GetControlRotation();
}

void AWoTPlayerController::RestoreCitadelEditStart()
{
	if (!EditStartLoc.IsZero())
	{
		if (APawn* ViewerPawn = GetPawn(); IsValid(ViewerPawn))
		{
			if (ViewerPawn->SetActorLocation(EditStartLoc))
			{
				UWoTZoneIndex::RefreshAfterRelocation(ViewerPawn);
			}
			ViewerPawn->SetActorRotation(EditStartRot);
		}
		else if (USceneComponent* Root = GetRootComponent())
		{
			Root->SetWorldLocationAndRotation(EditStartLoc, EditStartRot);
		}
		SetControlRotation(EditStartRot);
	}
	EditStartLoc = FVector::ZeroVector;
	EditStartRot = FRotator::ZeroRotator;
}

void AWoTPlayerController::ClientRestoreCitadelEditTransform_Implementation(
	FVector SavedLocation, FRotator SavedRotation)
{
	if (SavedLocation.IsZero())
	{
		return;
	}
	if (APawn* ViewerPawn = GetPawn(); IsValid(ViewerPawn))
	{
		if (ViewerPawn->SetActorLocation(SavedLocation))
		{
			UWoTZoneIndex::RefreshAfterRelocation(ViewerPawn);
		}
		ViewerPawn->SetActorRotation(SavedRotation);
	}
	else if (USceneComponent* Root = GetRootComponent())
	{
		Root->SetWorldLocationAndRotation(SavedLocation, SavedRotation);
	}
	SetControlRotation(SavedRotation);
}

void AWoTPlayerController::ClearLocalCitadelEditStateForTeardown()
{
	if (OwnerEditSessionTeam == 255
		&& CitadelEditCameraMode == EWoTCitadelEditCameraMode::Inactive)
	{
		return;
	}
	RestoreCitadelEditStart();
	CitadelEditCameraMode = EWoTCitadelEditCameraMode::Inactive;
	CitadelEditZone = nullptr;
	CitadelOrbitViewOffset = FRotator::ZeroRotator;
	OwnerEditBudget.Reset();
	OwnerEditSessionTeam = 255;
	OnRep_CitadelEditCamera();
	OnRep_OwnerEditBudget();
	OnRep_OwnerEditSessionTeam();
}

void AWoTPlayerController::OnRep_CitadelEditCamera()
{
	CitadelOrbitViewOffset = FRotator::ZeroRotator;
	ApplyCitadelEditCameraState();
}

void AWoTPlayerController::AdjustCitadelOrbitDistance(
	float AForward, float DeltaTime)
{
	if (!IsLocalController()
		|| CitadelEditCameraMode != EWoTCitadelEditCameraMode::Orbit
		|| !IsValid(CitadelEditZone.Get()) || AForward == 0.0f)
	{
		return;
	}
	CitadelEditZone->EditViewDistance = AdjustCitadelEditViewDistanceFeet(
		CitadelEditZone->EditViewDistance, AForward, DeltaTime);
}

void AWoTPlayerController::RotateCitadelOrbit(float AStrafe, float DeltaTime)
{
	if (!IsLocalController()
		|| CitadelEditCameraMode != EWoTCitadelEditCameraMode::Orbit
		|| !IsValid(CitadelEditZone.Get()) || AStrafe == 0.0f)
	{
		return;
	}
	CitadelEditZone->EditViewAngle = AdjustCitadelEditViewAngle(
		CitadelEditZone->EditViewAngle, AStrafe, DeltaTime);
}

void AWoTPlayerController::CalcCamera(float DeltaTime, FMinimalViewInfo& OutResult)
{
	auto ApplyViewFlash = [this, &OutResult]
	{
		if (OutResult.PostProcessSettings.bOverride_ColorOffset)
		{
			OutResult.PostProcessSettings.ColorOffset +=
				FVector4(CurrentViewFlashFog, 0.0f);
		}
		else
		{
			OutResult.PostProcessSettings.ColorOffset =
				FVector4(CurrentViewFlashFog, 0.0f);
		}
		OutResult.PostProcessSettings.bOverride_ColorOffset = true;
		OutResult.PostProcessBlendWeight = 1.0f;
	};
	if (CitadelEditCameraMode == EWoTCitadelEditCameraMode::Orbit
		&& IsValid(CitadelEditZone.Get()))
	{
		CalculateCitadelOrbitView(CitadelEditZone->GetActorLocation(),
			CitadelEditZone->EditViewAngle, CitadelEditZone->EditViewDistance,
			OutResult.Location, OutResult.Rotation, CitadelOrbitViewOffset);
		OutResult.Location.Z += UE1ShakeVert;
		ApplyViewFlash();
		return;
	}
	// End camera: the GameEnded behind view of the frozen pawn (steerable — the
	// live ControlRotation feeds the arm each frame, retail GameEnded.PlayerMove).
	if (bMatchEnded)
	{
		const bool bUseExternalTarget = bEndCameraUsesExternalViewTarget
			&& IsValid(EndCameraViewTarget.Get());
		const AActor* CameraTarget = bUseExternalTarget
			? EndCameraViewTarget.Get() : GetPawn();
		if (IsValid(CameraTarget))
		{
			OutResult.Rotation = GetControlRotation();
			OutResult.Location = CalcEndCamLocation(
				CameraTarget->GetActorLocation(), OutResult.Rotation,
				bUseExternalTarget ? ExternalEndCamDistance : EndCamDistance);
			OutResult.Location.Z += UE1ShakeVert;
			ApplyViewFlash();
			return;
		}
	}
	Super::CalcCamera(DeltaTime, OutResult);
	OutResult.Location += UE1WalkBob;
	if (const AWoTCharacter* PawnCharacter = Cast<AWoTCharacter>(GetPawn()))
	{
		OutResult.Location.Z += UE1EyeHeight - PawnCharacter->BaseEyeHeight;
	}
	OutResult.Location.Z += UE1ShakeVert;
	ApplyViewFlash();
}

bool AWoTPlayerController::HasValidLocalCitadelCreateZone() const
{
	if (!IsLocalController() || !IsOwnerEditingCitadel())
	{
		return false;
	}
	AWoTZoneVolume* ViewerZone = AWoTZoneVolume::EditZoneAt(
		GetWorld(), GetCitadelViewerLocation());
	return IsValid(ViewerZone) && ViewerZone->bAllowEditing;
}

bool AWoTPlayerController::HasValidLocalCitadelPresentationZone() const
{
	if (!HasValidLocalCitadelCreateZone())
	{
		return false;
	}
	AWoTZoneVolume* ViewerZone = AWoTZoneVolume::EditZoneAt(
		GetWorld(), GetCitadelViewerLocation());
	return IsValid(ViewerZone) && ViewerZone->EditTeam == OwnerEditSessionTeam;
}

void AWoTPlayerController::ApplyCitadelResourcePresentation(
	AWoTTrap* Resource, bool bActive, bool bHighlight)
{
	if (!IsValid(Resource))
	{
		return;
	}
	const EWoTCitadelResourcePresentation Presentation = bActive
		? (bHighlight ? EWoTCitadelResourcePresentation::ActiveHighlight
			: EWoTCitadelResourcePresentation::Active)
		: (bHighlight ? EWoTCitadelResourcePresentation::InactiveHighlight
			: EWoTCitadelResourcePresentation::Inactive);
	const TWeakObjectPtr<AWoTTrap> ResourceKey(Resource);
	// Skip only when the presentation is unchanged AND its marker still exists — a lost
	// marker (destroyed elsewhere) must be recreated even at the same presentation id.
	if (Resource->GetCitadelEditorPresentation() == Presentation
		&& CitadelResourceMarkers.FindRef(ResourceKey).IsValid())
	{
		return;
	}
	Resource->SetCitadelEditorPresentation(Presentation);

	AActor* Marker = CitadelResourceMarkers.FindRef(ResourceKey).Get();
	if (!IsValid(Marker))
	{
		FActorSpawnParameters Params;
		Params.Owner = this;
		Params.ObjectFlags |= RF_Transient;
		Params.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Marker = GetWorld()->SpawnActor<AActor>(
			AActor::StaticClass(), Resource->GetActorTransform(), Params);
		if (!IsValid(Marker))
		{
			Resource->SetCitadelEditorPresentation(
				EWoTCitadelResourcePresentation::Hidden);
			return;
		}
		Marker->SetReplicates(false);
		Marker->SetActorEnableCollision(false);
		UWoTSpriteBillboardComponent* Sprite =
			NewObject<UWoTSpriteBillboardComponent>(Marker, TEXT("CitadelResourceSprite"));
		Marker->SetRootComponent(Sprite);
		Sprite->DrawScale = 0.4f;
		Sprite->RegisterComponent();
		CitadelResourceMarkers.Add(ResourceKey, Marker);
	}
	Marker->SetActorTransform(Resource->GetActorTransform());
	if (UWoTSpriteBillboardComponent* Sprite =
		Marker->FindComponentByClass<UWoTSpriteBillboardComponent>())
	{
		const FName TextureName =
			Resource->ResolveCitadelEditorMarkerTexture(bActive, bHighlight);
		if (!TextureName.IsNone())
		{
			Sprite->SetSpriteSeries(TEXT("WoTTraps"),
				{TextureName.ToString()}, 64.0f, 64.0f);
		}
	}
}

void AWoTPlayerController::ClearCitadelPresentationActors()
{
	for (const TWeakObjectPtr<AWoTTrap>& ResourcePtr : RevealedCitadelResources)
	{
		if (AWoTTrap* Resource = ResourcePtr.Get())
		{
			Resource->SetCitadelEditorPresentation(
				EWoTCitadelResourcePresentation::Hidden);
		}
	}
	for (const TPair<TWeakObjectPtr<AWoTTrap>, TWeakObjectPtr<AActor>>& Entry
		: CitadelResourceMarkers)
	{
		if (AActor* Marker = Entry.Value.Get())
		{
			Marker->Destroy();
		}
	}
	RevealedCitadelResources.Reset();
	CitadelResourceMarkers.Reset();
	HoveredCitadelResource.Reset();
}

void AWoTPlayerController::ClearCitadelSelectionPresentation()
{
	DestroyCitadelGhostPreview();
	ClearCitadelPresentationActors();
	SelectedCitadelAdapter = NAME_None;
	bCitadelPresentationZoneValid = false;
	RefreshCitadelSelectTraceability();   // EndSelect: every toggle resource deselects
}

void AWoTPlayerController::RefreshCitadelSelectTraceability()
{
	// ServerBeginSelect/ServerEndSelect (WOTPlayer.uc:5059-5091), owner-LOCAL like the
	// palette that drives them: stamp each toggle-resource root in the session team's
	// zones selected while its adapter is the current palette selection (traceable —
	// Portcullis/Staircase.BeginSelectResource), deselected otherwise (an inactive one
	// hides from the cursor — EndSelectResource). The selectable query applies retail's
	// class + team-zone filter; the viewer-zone gate mirrors EditorHUD's
	// IsInvalidEditZone sync (leaving the editable zone destroys the selection and
	// EndSelects). The stamps are per-machine cursor state — the server RPC gates never
	// trace, so nothing here needs to cross the wire.
	if (!GetWorld())
	{
		return;
	}
	const bool bSelecting = IsOwnerEditingCitadel()
		&& HasValidLocalCitadelPresentationZone();
	for (TActorIterator<AWoTTrap> It(GetWorld()); It; ++It)
	{
		AWoTTrap* ResourceRoot = *It;
		if (!IsValid(ResourceRoot) || ResourceRoot->GetBaseResource() != ResourceRoot
			|| ResourceRoot->IsPreviewMode())
		{
			continue;
		}
		const bool bSelected = bSelecting
			&& WoTCitadelResources::QuerySelectableResource(ResourceRoot,
				SelectedCitadelAdapter, OwnerEditSessionTeam).IsSelectable();
		ResourceRoot->SetCitadelSelectTraceability(bSelected);
	}
}

void AWoTPlayerController::DestroyCitadelGhostPreview()
{
	if (AActor* Ghost = CitadelGhostPreviewActor.Get())
	{
		Ghost->Destroy();
	}
	CitadelGhostPreviewActor.Reset();
	CitadelGhostPreview.Reset();
	CitadelGhostResourceClass.Reset();
	CitadelGhostCreatureRow = NAME_None;
	bCitadelGhostPlacementValid = false;
}

void AWoTPlayerController::CreateCitadelGhostPreview(
	const FWoTResourceDescriptor& Descriptor)
{
	if (!IsLocalController() || !GetWorld())
	{
		return;
	}
	if (!Descriptor.CreatureRow.IsNone())
	{
		// TroopSpawnLocation: APlayerController already has a SpawnLocation member
		// (MSVC C4458-as-error; Clang allowed the shadow).
		FVector TroopSpawnLocation;
		FRotator SpawnRotation;
		if (!CalculateCitadelTroopPlacement(GetPawn(), Descriptor.CreatureRow,
			/*bSpawnAdjusted*/ false, TroopSpawnLocation, SpawnRotation))
		{
			return;
		}
		const FTransform SpawnTransform(SpawnRotation, TroopSpawnLocation);
		AWoTNPCCharacter* Ghost = GetWorld()->SpawnActorDeferred<AWoTNPCCharacter>(
			AWoTNPCCharacter::StaticClass(), SpawnTransform, this, nullptr,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!IsValid(Ghost))
		{
			return;
		}
		Ghost->SetFlags(RF_Transient);
		Ghost->SetReplicates(false);
		Ghost->AutoPossessAI = EAutoPossessAI::Disabled;
		Ghost->CreatureRowName = Descriptor.CreatureRow;
		Ghost->SetActorEnableCollision(false);
		WoTCreatureData::BindCreatureBody(*Ghost, Descriptor.CreatureRow);
		Ghost->FinishSpawning(SpawnTransform);
		if (!IsValid(Ghost))
		{
			return; // SpawnActor may return a self-destroyed fixture actor.
		}
		Ghost->SetReplicates(false);
		Ghost->SetActorEnableCollision(false);
		Ghost->SetActorHiddenInGame(true); // first Sync validates altar/guide eligibility.
		CitadelGhostPreviewActor = Ghost;
		CitadelGhostResourceClass = AWoTNPCCharacter::StaticClass();
		CitadelGhostCreatureRow = Descriptor.CreatureRow;
		bCitadelGhostPlacementValid = false;
		return;
	}

	UClass* ResourceClass = Descriptor.ResourceClass;
	if (!ResourceClass)
	{
		return;
	}
	if (ResourceClass->IsChildOf(AWoTSeal::StaticClass()))
	{
		FVector ViewOrigin;
		FRotator ViewRotation;
		GetPlayerViewPoint(ViewOrigin, ViewRotation);
		const FTransform SpawnTransform(
			ViewRotation.Vector().GetSafeNormal().Rotation(), ViewOrigin);
		AWoTSeal* Ghost = GetWorld()->SpawnActorDeferred<AWoTSeal>(
			ResourceClass, SpawnTransform, this, nullptr,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!IsValid(Ghost))
		{
			return;
		}
		Ghost->SetFlags(RF_Transient);
		Ghost->bPreviewSeal = true;
		Ghost->SetReplicates(false);
		Ghost->SetReplicateMovement(false);
		Ghost->SetActorEnableCollision(false);
		Ghost->SetActorHiddenInGame(true);
		Ghost->FinishSpawning(SpawnTransform);
		if (!IsValid(Ghost))
		{
			return;
		}
		Ghost->bPreviewSeal = true;
		Ghost->SetReplicates(false);
		Ghost->SetReplicateMovement(false);
		Ghost->SetActorEnableCollision(false);
		Ghost->SetActorHiddenInGame(true);
		CitadelGhostPreviewActor = Ghost;
		CitadelGhostResourceClass = ResourceClass;
		bCitadelGhostPlacementValid = false;
		return;
	}
	if (!ResourceClass->IsChildOf(AWoTTrap::StaticClass()))
	{
		return;
	}

	FVector ViewOrigin;
	FRotator ViewRotation;
	GetPlayerViewPoint(ViewOrigin, ViewRotation);
	const FVector ViewNormal = ViewRotation.Vector().GetSafeNormal();
	const FTransform SpawnTransform(ViewNormal.Rotation(), ViewOrigin);
	AWoTTrap* Ghost = GetWorld()->SpawnActorDeferred<AWoTTrap>(
		ResourceClass, SpawnTransform, this, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!IsValid(Ghost))
	{
		return;
	}
	Ghost->SetFlags(RF_Transient);
	Ghost->SetPreviewMode(true);
	Ghost->FinishSpawning(SpawnTransform);
	if (!IsValid(Ghost))
	{
		return;
	}
	// Re-assert after subclass BeginPlay in case it configured a component for gameplay.
	Ghost->SetPreviewMode(true);
	Ghost->BeginEditingResource(OwnerEditSessionTeam);
	Ghost->SetPreviewMode(true);
	Ghost->SetPreviewVisible(false);
	CitadelGhostPreviewActor = Ghost;
	CitadelGhostPreview = Ghost;
	CitadelGhostResourceClass = ResourceClass;
	bCitadelGhostPlacementValid = false;
}

void AWoTPlayerController::ReconcileCitadelGhostPreview()
{
	const FWoTResourceDescriptor& Descriptor =
		WoTCitadelResources::FindByAdapterName(SelectedCitadelAdapter);
	const bool bPreviewable = !Descriptor.CreatureRow.IsNone()
		|| (Descriptor.ResourceClass
			&& (Descriptor.ResourceClass->IsChildOf(AWoTTrap::StaticClass())
				|| Descriptor.ResourceClass->IsChildOf(AWoTSeal::StaticClass())));
	const bool bEligible = IsLocalController()
		&& HasValidLocalCitadelCreateZone()
		&& Descriptor.IsValidDescriptor() && Descriptor.bSpawnTempResource
		&& bPreviewable
		&& GetOwnerCitadelBudgetCount(SelectedCitadelAdapter) > 0;
	if (!bEligible)
	{
		DestroyCitadelGhostPreview();
		return;
	}
	const bool bMatches = Descriptor.CreatureRow.IsNone()
		? CitadelGhostResourceClass.Get() == Descriptor.ResourceClass
			&& (Descriptor.ResourceClass->IsChildOf(AWoTSeal::StaticClass())
				? CitadelGhostPreviewActor.IsValid() : CitadelGhostPreview.IsValid())
		: CitadelGhostCreatureRow == Descriptor.CreatureRow
			&& CitadelGhostPreviewActor.IsValid();
	if (!bMatches)
	{
		DestroyCitadelGhostPreview();
		CreateCitadelGhostPreview(Descriptor);
	}
}

void AWoTPlayerController::SyncCitadelGhostPreview(
	const FWoTCitadelEditorTraceResult& Hit)
{
	if (AWoTNPCCharacter* TroopGhost =
		Cast<AWoTNPCCharacter>(CitadelGhostPreviewActor.Get()))
	{
		FVector PreviewLocation;
		FRotator PreviewRotation;
		bCitadelGhostPlacementValid = CalculateCitadelTroopPlacement(
			GetPawn(), CitadelGhostCreatureRow, /*bSpawnAdjusted*/ false,
			PreviewLocation, PreviewRotation);
		if (bCitadelGhostPlacementValid)
		{
			TroopGhost->SetActorLocationAndRotation(
				PreviewLocation, PreviewRotation, false, nullptr,
				ETeleportType::TeleportPhysics);
			bCitadelGhostPlacementValid =
				AWoTBattleGameMode::NearestAltarTeamInWorld(
					GetWorld(), PreviewLocation) == OwnerEditSessionTeam;
			if (bCitadelGhostPlacementValid
				&& CitadelGhostCreatureRow == FName(TEXT("MashadarFocusPawn")))
			{
				// MashadarFocusPawn.uc:177-184: its preview validity is the same
				// closest-guide deploy result as the server create path.
				bCitadelGhostPlacementValid = IsValid(
					FindClosestBindableMashadarGuide(GetWorld(), PreviewLocation));
			}
		}
		TroopGhost->SetActorEnableCollision(false);
		TroopGhost->SetActorHiddenInGame(!bCitadelGhostPlacementValid);
		return;
	}
	if (AWoTSeal* SealGhost = Cast<AWoTSeal>(CitadelGhostPreviewActor.Get()))
	{
		bCitadelGhostPlacementValid = false;
		SealGhost->SetActorHiddenInGame(true);
		SealGhost->SetActorEnableCollision(false);
		if (!Hit.bBlockingHit || !Hit.bHitWorldGeometry
			|| Hit.HitLocation.ContainsNaN() || Hit.HitNormal.ContainsNaN()
			|| Hit.HitNormal.IsNearlyZero()
			|| AWoTBattleGameMode::NearestAltarTeamInWorld(
				GetWorld(), Hit.HitLocation) != OwnerEditSessionTeam)
		{
			return;
		}
		bCitadelGhostPlacementValid = SealGhost->DeployResource(
			Hit.HitLocation, Hit.HitNormal.GetSafeNormal());
		if (bCitadelGhostPlacementValid
			&& AWoTBattleGameMode::NearestAltarTeamInWorld(
				GetWorld(), SealGhost->GetActorLocation()) != OwnerEditSessionTeam)
		{
			bCitadelGhostPlacementValid = false;
		}
		SealGhost->SetActorEnableCollision(false);
		SealGhost->SetActorHiddenInGame(!bCitadelGhostPlacementValid);
		return;
	}

	AWoTTrap* Ghost = CitadelGhostPreview.Get();
	if (!IsValid(Ghost))
	{
		bCitadelGhostPlacementValid = false;
		return;
	}

	Ghost->SetPreviewVisible(false);
	if (Hit.bBlockingHit && IsValid(Hit.HitActor)
		&& Ghost->SameLogicalActor(Hit.HitActor))
	{
		Ghost->SetPreviewVisible(bCitadelGhostPlacementValid);
		return;
	}

	bCitadelGhostPlacementValid = false;
	if (!Hit.bBlockingHit || !Hit.bHitWorldGeometry)
	{
		Ghost->RemoveResource();
		return;
	}
	if (Hit.HitLocation.ContainsNaN() || Hit.HitNormal.ContainsNaN()
		|| Hit.HitNormal.IsNearlyZero())
	{
		Ghost->RemoveResource();
		return;
	}
	if (AWoTBattleGameMode::NearestAltarTeamInWorld(
		GetWorld(), Hit.HitLocation) != OwnerEditSessionTeam)
	{
		Ghost->RemoveResource();
		return;
	}
	bCitadelGhostPlacementValid = Ghost->DeployResource(
		Hit.HitLocation, Hit.HitNormal.GetSafeNormal());
	if (bCitadelGhostPlacementValid
		&& AWoTBattleGameMode::NearestAltarTeamInWorld(
			GetWorld(), Ghost->GetActorLocation()) != OwnerEditSessionTeam)
	{
		Ghost->RemoveResource();
		bCitadelGhostPlacementValid = false;
	}
	Ghost->SetPreviewMode(true);
	Ghost->SetPreviewVisible(bCitadelGhostPlacementValid);
}

void AWoTPlayerController::SetCitadelPaletteSelection(FName AdapterName)
{
	if (!IsLocalController())
	{
		return;
	}
	ClearCitadelPresentationActors();
	SelectedCitadelAdapter = AdapterName;
	// The palette moved: Begin/EndSelect the toggle resources before any click can
	// trace against stale cursor collision.
	RefreshCitadelSelectTraceability();
	bCitadelPresentationZoneValid = HasValidLocalCitadelPresentationZone();
	if (bCitadelPresentationZoneValid)
	{
		RefreshCitadelSelectionPresentation();
	}
	ReconcileCitadelGhostPreview();
}

void AWoTPlayerController::RefreshCitadelSelectionPresentation()
{
	if (!HasValidLocalCitadelPresentationZone()
		|| SelectedCitadelAdapter.IsNone() || !GetWorld())
	{
		ClearCitadelPresentationActors();
		return;
	}

	TArray<TWeakObjectPtr<AWoTTrap>> NewRevealed;
	for (TActorIterator<AWoTTrap> It(GetWorld()); It; ++It)
	{
		FWoTCitadelSelectableResource Selection =
			WoTCitadelResources::QuerySelectableResource(
				*It, SelectedCitadelAdapter, OwnerEditSessionTeam);
		AWoTTrap* LogicalResource = Cast<AWoTTrap>(Selection.LogicalResource);
		const TWeakObjectPtr<AWoTTrap> LogicalResourceKey(LogicalResource);
		if (!Selection.IsSelectable() || !IsValid(LogicalResource)
			|| NewRevealed.Contains(LogicalResourceKey))
		{
			continue;
		}
		NewRevealed.Add(LogicalResourceKey);
		const bool bHighlight = HoveredCitadelResource.Get() == LogicalResource;
		ApplyCitadelResourcePresentation(LogicalResource,
			Selection.State == EWoTCitadelSelectableState::Active, bHighlight);
	}

	for (const TWeakObjectPtr<AWoTTrap>& OldPtr : RevealedCitadelResources)
	{
		if (NewRevealed.Contains(OldPtr))
		{
			continue;
		}
		// Reset the trap's presentation id only if it still exists...
		if (AWoTTrap* OldResource = OldPtr.Get())
		{
			OldResource->SetCitadelEditorPresentation(
				EWoTCitadelResourcePresentation::Hidden);
		}
		// ...but always destroy/remove its marker (a destroyed resource would otherwise
		// leak its independently-owned marker until broader teardown).
		if (TWeakObjectPtr<AActor>* MarkerPtr = CitadelResourceMarkers.Find(OldPtr))
		{
			if (AActor* Marker = MarkerPtr->Get())
			{
				Marker->Destroy();
			}
			CitadelResourceMarkers.Remove(OldPtr);
		}
	}
	RevealedCitadelResources = MoveTemp(NewRevealed);
	if (HoveredCitadelResource.IsValid()
		&& !RevealedCitadelResources.Contains(HoveredCitadelResource))
	{
		HoveredCitadelResource.Reset();
	}
}

void AWoTPlayerController::SetHoveredCitadelResource(AActor* Candidate)
{
	if (!IsLocalController())
	{
		return;
	}
	AWoTTrap* OldResource = HoveredCitadelResource.Get();
	AWoTTrap* NewResource = nullptr;
	FWoTCitadelSelectableResource Selection =
		WoTCitadelResources::QuerySelectableResource(
			Candidate, SelectedCitadelAdapter, OwnerEditSessionTeam);
	if (Selection.IsSelectable())
	{
		NewResource = Cast<AWoTTrap>(Selection.LogicalResource);
	}
	if (OldResource == NewResource)
	{
		return;
	}
	HoveredCitadelResource = NewResource;
	if (IsValid(OldResource))
	{
		FWoTCitadelSelectableResource OldSelection =
			WoTCitadelResources::QuerySelectableResource(
				OldResource, SelectedCitadelAdapter, OwnerEditSessionTeam);
		if (OldSelection.IsSelectable())
		{
			ApplyCitadelResourcePresentation(OldResource,
				OldSelection.State == EWoTCitadelSelectableState::Active, false);
		}
	}
	if (IsValid(NewResource))
	{
		ApplyCitadelResourcePresentation(NewResource,
			Selection.State == EWoTCitadelSelectableState::Active, true);
	}
}

void AWoTPlayerController::OnRep_OwnerEditSessionTeam()
{
	ApplyOwnerCitadelEditState();
}

void AWoTPlayerController::OnRep_OwnerEditBudget()
{
	if (!IsLocalController())
	{
		return;
	}
	if (AWoTCharacter* PawnChar = Cast<AWoTCharacter>(GetPawn()))
	{
		if (IsOwnerEditingCitadel() || PawnChar->CitadelEditorHandSet)
		{
			PawnChar->ReconcileCitadelEditorHandSet();
		}
	}
	ReconcileCitadelGhostPreview();
}

FVector AWoTPlayerController::GetCitadelViewerLocation() const
{
	// UE1 EditCitadel reads the player actor's Region.Zone. AController::GetActorLocation
	// is private in UE5 (a controller has no meaningful location while possessing), so use
	// the pawn's location during real editing; fall back to the controller's own
	// TransformComponent (the pawnless test-harness viewer position).
	const APawn* ViewerPawn = GetPawn();
	if (IsValid(ViewerPawn))
	{
		return ViewerPawn->GetActorLocation();
	}
	const USceneComponent* Root = GetRootComponent();
	return Root ? Root->GetComponentLocation() : FVector::ZeroVector;
}

bool AWoTPlayerController::EditorTrace(
	FWoTCitadelEditorTraceResult& OutHit, AActor* PreviewActor) const
{
	OutHit = FWoTCitadelEditorTraceResult();
	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	FVector ViewOrigin;
	FRotator ViewRotation;
	GetPlayerViewPoint(ViewOrigin, ViewRotation);
	const FVector ViewForward = ViewRotation.Vector().GetSafeNormal();
	if (ViewOrigin.ContainsNaN() || ViewForward.ContainsNaN()
		|| ViewForward.IsNearlyZero())
	{
		return false;
	}

	FCollisionQueryParams Params(SCENE_QUERY_STAT(WoTCitadelEditorTrace), false);
	if (APawn* ViewerPawn = GetPawn(); IsValid(ViewerPawn))
	{
		Params.AddIgnoredActor(ViewerPawn);
	}
	if (IsValid(PreviewActor))
	{
		Params.AddIgnoredActor(PreviewActor);
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Candidate = *It;
			if (IsValid(Candidate) && Candidate != PreviewActor
				&& Candidate->IsOwnedBy(PreviewActor))
			{
				Params.AddIgnoredActor(Candidate);
			}
		}
	}

	// Retail's native TraceVisible skips a hit owned by another team and resumes
	// 16 uu beyond it. Each pass
	// advances at least those 16 uu, so the distance guard bounds the loop. Retail
	// keys IsZoneVisible off PlayerReplicationInfo.Team; while an edit session is live
	// the owner mirror IS that team (the begin-edit gate requires equality), and it is
	// present even where the headless harness carries no PlayerState.
	const uint8 ViewerTeam = IsOwnerEditingCitadel()
		? OwnerEditSessionTeam : CitadelControllerTeam(this);
	const FVector TraceEnd = ViewOrigin + ViewForward * TRACE_DISTANCE;
	FHitResult PhysicalHit;
	FVector TraceStart = ViewOrigin;
	bool bVisibleHit = false;
	while (FVector::Dist(ViewOrigin, TraceStart) < TRACE_DISTANCE)
	{
		if (!World->LineTraceSingleByChannel(
			PhysicalHit, TraceStart, TraceEnd, ECC_Visibility, Params)
			|| !PhysicalHit.bBlockingHit)
		{
			return false;
		}
		if (UWoTFlatCylinderComponent* Flat = Cast<UWoTFlatCylinderComponent>(
			PhysicalHit.GetComponent()); Flat && Flat->IsAnalyticCursorTrace())
		{
			FHitResult Exact;
			if (!Flat->ExactLineTrace(TraceStart, TraceEnd, Exact))
			{
				// The circumscribed 16-gon is only a conservative broad phase.
				// Ignore this component and resume the same visibility trace.
				Params.AddIgnoredComponent(Flat);
				continue;
			}
			PhysicalHit = Exact;
		}
		if (CitadelZoneVisibleAt(World, PhysicalHit.ImpactPoint, ViewerTeam))
		{
			bVisibleHit = true;
			break;
		}
		TraceStart = PhysicalHit.ImpactPoint + 16.0f * ViewForward;
	}
	if (!bVisibleHit)
	{
		return false;
	}

	AActor* PhysicalActor = PhysicalHit.GetActor();
	AActor* LogicalResource =
		WoTCitadelResources::ResolveLogicalResource(PhysicalActor);
	OutHit.HitActor = IsValid(LogicalResource) ? LogicalResource : PhysicalActor;
	// TraceVisible's SingleLineCheck returns the hit backed off 0.5 uu along the
	// trace: placement traces start just in
	// front of the surface the cursor found, never on it.
	OutHit.HitLocation = PhysicalHit.ImpactPoint - 0.5f * ViewForward;
	OutHit.HitNormal = PhysicalHit.ImpactNormal;
	OutHit.bBlockingHit = true;
	OutHit.bHitWorldGeometry = !IsValid(LogicalResource)
		&& IsValid(PhysicalHit.GetComponent())
		&& PhysicalHit.GetComponent()->GetCollisionObjectType() == ECC_WorldStatic;
	return true;
}

void AWoTPlayerController::NotifyMatchEnded()
{
	// Server-only; called by AWoTGameMode::EndGame. WOTPlayer.GameEnded.BeginState arms a
	// SetTimer(PlayerRestartGameDelay); the player is frozen until it lifts.
	if (bMatchEnded || !HasAuthority())
	{
		return;
	}
	bMatchEnded = true;
	bRestartAllowed = false;
	EnterPlayerState(EWoTPlayerStateMachineState::GameEnded);
	GetWorldTimerManager().SetTimer(RestartDelayTimer, this,
		&AWoTPlayerController::OnRestartDelayElapsed,
		FMath::Max(PlayerRestartGameDelay, 0.01f), /*loop*/ false);
	// WOTPlayer.GameEnded.BeginState freeze — apply on the authority (a listen-server host
	// freezes locally here; remote clients get it via OnRep_MatchEnded). The fire→restart
	// re-route + the scoreboard are the HUD/character halves of the GameEnded presentation.
	FreezeMatchEndedPawn();
	ApplyMatchEndedInput();
}

void AWoTPlayerController::ArmRespawnFire(AWoTCharacter* Corpse,
	float RespawnDelaySecs)
{
	if (!HasAuthority() || bMatchEnded || !GetWorld())
	{
		return;
	}
	RespawnCorpse = Corpse;
	OKToFireTime = GetWorld()->GetTimeSeconds() + FMath::Max(RespawnDelaySecs, 0.0f);
	EnterPlayerState(EWoTPlayerStateMachineState::Dying);
}

void AWoTPlayerController::Fire()
{
	if (IsChatTyping())
	{
		return;
	}
	switch (PlayerStateMachineState)
	{
	case EWoTPlayerStateMachineState::Playing:
		if (AWoTCharacter* PawnChar = Cast<AWoTCharacter>(GetPawn()))
		{
			PawnChar->UseAngreal();
		}
		break;
	case EWoTPlayerStateMachineState::Dying:
	case EWoTPlayerStateMachineState::WaitingForSpawn:
		ServerRespawnPlayer();
		break;
	case EWoTPlayerStateMachineState::GameEnded:
		ServerRestartGame();
		break;
	default:
		break;
	}
}

void AWoTPlayerController::CeaseFire()
{
	// exec CeaseUsingAngreal (WOTPlayer.uc:1215) has no state gate: a live pawn always
	// ceases (the pawn handler keeps the citadel-editor press swallow). Dying has no
	// pawn, and a fresh post-respawn pawn's cease is a no-op — so no FSM dispatch here.
	if (AWoTCharacter* PawnChar = Cast<AWoTCharacter>(GetPawn()))
	{
		PawnChar->CeaseUsingAngreal();
	}
}

void AWoTPlayerController::Jump()
{
	if (UWorld* World = GetWorld(); World && World->IsPaused())
	{
		const AWorldSettings* Settings = World->GetWorldSettings();
		if (!InGameMenu.IsOpen() && !InGameMenu.IsLeaving() && Settings
			&& Settings->GetPauserPlayerState() == PlayerState)
		{
			ServerRequestPause();
		}
		return;
	}
	if (IsChatTyping())
	{
		return;
	}
	// Dying: retail routes a jump press into Fire(0) — UE1 PlayerPawn's
	// Dying.PlayerMove fires on bPressedJump and Dying.ServerMove fires on the jump-
	// status edge (WOTPlayer's Dying does not override either) — so the jump key
	// respawns through the same OKToFireTime-gated path as Fire.
	if (PlayerStateMachineState == EWoTPlayerStateMachineState::Dying)
	{
		Fire();
		return;
	}
	// CitadelOrbitMode consumes movement input (WOTPlayer.uc:5262-5272): a jump press
	// must not launch the ghosted body. Gate the live FORWARD, not the binding — the
	// controller binding must stay to keep consuming the key's edges (see the header).
	if (CitadelEditCameraMode == EWoTCitadelEditCameraMode::Orbit)
	{
		return;
	}
	// Alive (or GameEnded, matching the previous pawn-side binding's behavior): the
	// press is the pawn's ordinary jump.
	if (AWoTCharacter* PawnChar = Cast<AWoTCharacter>(GetPawn()))
	{
		// The same key is also PlayerSwimming's aUp while held.
		PawnChar->SetSwimUpHeld(true);
		if (!PawnChar->IsHardLanding())
		{
			PawnChar->Jump();
		}
	}
}

void AWoTPlayerController::UpdateRotation(float DeltaTime)
{
	if (CitadelEditCameraMode == EWoTCitadelEditCameraMode::Orbit
		&& IsValid(CitadelEditZone.Get()))
	{
		// PlayerTickEdit replaces the view update: look input (aTurn/aLookUp,
		// decelerated x0.3) turns ViewOffset instead of the ghosted body.
		CitadelOrbitViewOffset = (CitadelOrbitViewOffset + RotationInput * 0.3f).GetNormalized();
		RotationInput = FRotator::ZeroRotator;
	}
	// Capture the real, scaled input before the base applies it to ControlRotation.
	// Impaled.UpdateRotation chooses its 3000/8000 boundary from this sign.
	const float AppliedPitchInput = RotationInput.Pitch;
	Super::UpdateRotation(DeltaTime);
	if (AWoTCharacter* WoTPawn = Cast<AWoTCharacter>(GetPawn()))
	{
		if (WoTPawn->GetEffectivePlayerMovementConstraint()
			== EWoTPlayerMovementConstraint::Impaled)
		{
			WoTPawn->ApplyImpaledRotationAfterViewUpdate(AppliedPitchInput);
		}
		else
		{
			// BATrolloc.HeadToss is a single actor-pitch pulse. Retail's normal
			// PlayerPawn.UpdateRotation overwrites it on the following update.
			WoTPawn->ClearCreaturePitchAfterNormalViewUpdate();
		}
	}
}

#if WITH_AUTOMATION_TESTS
void AWoTPlayerController::ApplyPitchRotationForTest(float PitchInputDegrees)
{
	const FRotator SavedRotationInput = RotationInput;
	RotationInput = FRotator(PitchInputDegrees, 0.0f, 0.0f);
	UpdateRotation(1.0f);
	RotationInput = SavedRotationInput;
}
#endif

void AWoTPlayerController::StopJumping()
{
	if (ACharacter* PawnChar = Cast<ACharacter>(GetPawn()))
	{
		PawnChar->StopJumping();
	}
	if (AWoTCharacter* WoTChar = Cast<AWoTCharacter>(GetPawn()))
	{
		WoTChar->SetSwimUpHeld(false);
	}
}

void AWoTPlayerController::ShowScores()
{
	// The retail timer writes bShowScores directly. Once it has fired, manual input
	// consumes that auto-show source and remains a true toggle instead of a forced hold.
	bDeathScoreboardAutoShowActive = false;
	bShowScores = !bShowScores;
}

bool AWoTPlayerController::ServerRespawnPlayer_Validate() { return true; }

void AWoTPlayerController::ServerRespawnPlayer_Implementation()
{
	AWoTGameMode* Game = GetWorld() ? GetWorld()->GetAuthGameMode<AWoTGameMode>() : nullptr;
	if ((PlayerStateMachineState != EWoTPlayerStateMachineState::Dying
		&& PlayerStateMachineState != EWoTPlayerStateMachineState::WaitingForSpawn)
		|| !IsValid(Game) || bMatchEnded || Game->bGameEnded
		|| (PlayerStateMachineState == EWoTPlayerStateMachineState::Dying
			&& GetWorld()->GetTimeSeconds() < OKToFireTime))
	{
		return;
	}

	// Retail reuses its PlayerPawn. This port replaces it, so consume the retained body
	// immediately before RestartPlayer creates and possesses the new pawn.
	if (AWoTCharacter* Corpse = RespawnCorpse.Get())
	{
		Corpse->Destroy();
	}
	RespawnCorpse.Reset();
	Game->RestartPlayer(this);
	if (!GetPawn())
	{
		EnterPlayerState(EWoTPlayerStateMachineState::WaitingForSpawn);
	}
}

bool AWoTPlayerController::ServerRestartPlayerWoT_Validate() { return true; }

void AWoTPlayerController::ServerRestartPlayerWoT_Implementation()
{
	// PlayerPawn.ServerRestartPlayer → Level.Game.RestartPlayer(self), no state gate:
	// the team-select restart (BattleHUD.uc:63-64) runs on a LIVE player. Retail reuses
	// the single PlayerPawn; the port replaces it, so consume the current body (live
	// pawn or retained corpse) before RestartPlayer spawns the team pawn at a team
	// start (Battle's GetDefaultPawnClassForController + bSpawnInTeamArea weighting).
	// Only the match-over freeze holds the restart back.
	AWoTGameMode* Game = GetWorld() ? GetWorld()->GetAuthGameMode<AWoTGameMode>() : nullptr;
	if (!IsValid(Game) || bMatchEnded || Game->bGameEnded)
	{
		return;
	}
	if (AWoTCharacter* Corpse = RespawnCorpse.Get())
	{
		Corpse->Destroy();
	}
	RespawnCorpse.Reset();
	if (APawn* LivePawn = GetPawn())
	{
		UnPossess();
		LivePawn->Destroy();
	}
	Game->RestartPlayer(this);
	if (!GetPawn())
	{
		EnterPlayerState(EWoTPlayerStateMachineState::WaitingForSpawn);
	}
}

void AWoTPlayerController::EnterPlayerState(EWoTPlayerStateMachineState NewState)
{
	if (PlayerStateMachineState == NewState)
	{
		return;
	}
	ExitPlayerState(PlayerStateMachineState);
	PlayerStateMachineState = NewState;
	ApplyPlayerStateEntryPresentation(NewState);
	ForceNetUpdate();
}

void AWoTPlayerController::ExitPlayerState(EWoTPlayerStateMachineState OldState)
{
	if (!IsLocalController() || OldState != EWoTPlayerStateMachineState::Dying)
	{
		return;
	}
	GetWorldTimerManager().ClearTimer(DeathScoreboardAutoShowTimer);
	bDeathScoreboardAutoShowActive = false;
	bShowScores = false;
}

void AWoTPlayerController::ApplyPlayerStateEntryPresentation(
	EWoTPlayerStateMachineState NewState)
{
	if (!IsLocalController())
	{
		return;
	}
	switch (NewState)
	{
	case EWoTPlayerStateMachineState::Dying:
		bDeathScoreboardAutoShowActive = false;
		GetWorldTimerManager().SetTimer(DeathScoreboardAutoShowTimer, this,
			&AWoTPlayerController::OnDeathScoreboardAutoShowElapsed,
			DeathScoreboardAutoShowDelaySecs, /*loop*/ false);
		break;
	case EWoTPlayerStateMachineState::GameEnded:
		bDeathScoreboardAutoShowActive = false;
		bShowScores = true;
		break;
	case EWoTPlayerStateMachineState::Playing:
	default:
		break;
	}
}

void AWoTPlayerController::OnDeathScoreboardAutoShowElapsed()
{
	if (PlayerStateMachineState == EWoTPlayerStateMachineState::Dying)
	{
		bDeathScoreboardAutoShowActive = true;
		bShowScores = true;
	}
}

void AWoTPlayerController::OnRep_PlayerStateMachineState(
	EWoTPlayerStateMachineState OldState)
{
	ExitPlayerState(OldState);
	ApplyPlayerStateEntryPresentation(PlayerStateMachineState);
}

void AWoTPlayerController::TickPlayerState(float /*DeltaTime*/)
{
	// WOTPlayer's Dying and GameEnded input gates are event/timer driven. Keep the
	// shared FSM tick seam explicit for parity with the project's state-port pattern.
}

void AWoTPlayerController::OnRestartDelayElapsed()
{
	// WOTPlayer's GameEnded-state Timer clears bFrozen — from here a fire may restart.
	bRestartAllowed = true;
}

void AWoTPlayerController::OnRep_MatchEnded()
{
	if (bMatchEnded)
	{
		FreezeMatchEndedPawn();
		ApplyMatchEndedInput();
	}
}

FVector AWoTPlayerController::CalcEndCamLocation(const FVector& Anchor,
	const FRotator& View, float Distance) const
{
	// UE1 PlayerPawn.CalcBehindView: trace back Dist+30 along the
	// view; on a hit, camera distance = min(hit distance along view, Dist); place at
	// Anchor - (ViewDist - 30) * ViewDir (the 30 UU wall standoff).
	const FVector ViewDir = View.Vector();
	float ViewDist = Distance;
	FHitResult Hit;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(WoTEndCam), /*bTraceComplex*/ false,
		GetPawn());
	if (GetWorld() && GetWorld()->LineTraceSingleByChannel(Hit, Anchor,
			Anchor - (Distance + 30.0f) * ViewDir, ECC_Visibility, Params))
	{
		ViewDist = FMath::Min(
			static_cast<float>(FVector::DotProduct(Anchor - Hit.Location, ViewDir)),
			Distance);
	}
	return Anchor - (ViewDist - 30.0f) * ViewDir;
}

FRotator AWoTPlayerController::FindGoodEndView(const FVector& Anchor) const
{
	// UE1 PlayerPawn.FindGoodView: for an external ViewTarget,
	// PlayerCalcView already takes the 180-distance behind-view branch during the
	// pre-bBehindView trials, so retain the farthest of the 16 yaw candidates.
	FRotator View = GetControlRotation();
	View.Pitch = -52.3828125f;   // normalized 56000 UU
	const float StartYaw = View.Yaw;
	float BestDist = 0.0f;
	int32 BestTry = 0;
	for (int32 Try = 0; Try < 16; ++Try)
	{
		View.Yaw = StartYaw + Try * 22.5f;
		const float Dist = FVector::Dist(
			CalcEndCamLocation(Anchor, View, ExternalEndCamDistance), Anchor);
		if (Dist > BestDist)
		{
			BestDist = Dist;
			BestTry = Try;
		}
	}
	View.Yaw = StartYaw + BestTry * 22.5f;
	return View;
}

void AWoTPlayerController::ApplyEndCamera()
{
	// PlayerPawn.GameEnded.BeginState: FindGoodView runs before
	// bBehindView. UE5's normal self ViewTarget is the possessed pawn (UE1 represented
	// that as ViewTarget == None), so only a different actor takes the 180-distance sweep.
	if (!IsLocalController())
	{
		return;
	}
	APawn* OwningPawn = GetPawn();
	AActor* CurrentViewTarget = GetViewTarget();
	bEndCameraUsesExternalViewTarget = IsValid(CurrentViewTarget)
		&& CurrentViewTarget != OwningPawn && CurrentViewTarget != this;
	EndCameraViewTarget = bEndCameraUsesExternalViewTarget
		? CurrentViewTarget : OwningPawn;

	FRotator EndView = GetControlRotation();
	EndView.Pitch = -52.3828125f;   // normalized 56000 UU
	if (bEndCameraUsesExternalViewTarget)
	{
		EndView = FindGoodEndView(CurrentViewTarget->GetActorLocation());
	}
	SetControlRotation(EndView);
	SetViewTarget(this);
}

void AWoTPlayerController::FreezeMatchEndedPawn()
{
	// PlayerPawn.GameEnded.BeginState: both the
	// server transition and ClientGameEnded disable collision and enter PHYS_None.
	// Actor collision itself is not replicated, so the owning client must apply it too.
	APawn* EndedPawn = GetPawn();
	if (!IsValid(EndedPawn))
	{
		return;
	}
	EndedPawn->SetActorEnableCollision(false);
	if (ACharacter* EndedChar = Cast<ACharacter>(EndedPawn))
	{
		if (UCharacterMovementComponent* Move = EndedChar->GetCharacterMovement())
		{
			Move->StopMovementImmediately();
			Move->SetMovementMode(MOVE_None);
		}
	}
}

void AWoTPlayerController::ApplyMatchEndedInput()
{
	// Freeze movement while the match is decided (idempotent: bMatchEnded latches before
	// this is called, and OnRep fires once on the false->true edge, so the ignore counter
	// is bumped exactly once per player).
	SetIgnoreMoveInput(true);
	bShowScores = true;
	// The end camera (owner-local; no-op on the server for remote players).
	ApplyEndCamera();
}

bool AWoTPlayerController::ServerRestartGame_Validate() { return true; }

void AWoTPlayerController::ServerRestartGame_Implementation()
{
	// WOTPlayer.GameEnded.Fire: only once the freeze has lifted (bFrozen cleared). The
	// original re-arms the delay if fired early; the port simply ignores an early fire.
	if (!bMatchEnded || !bRestartAllowed)
	{
		return;
	}
	// PlayerPawn.ServerReStartGame → Level.Game.RestartGame().
	if (AWoTGameMode* GM = GetWorld()->GetAuthGameMode<AWoTGameMode>())
	{
		GM->RestartGame();
	}
}

bool AWoTPlayerController::ServerChangeTeam_Validate(int32 /*Team*/) { return true; }

void AWoTPlayerController::ServerChangeTeam_Implementation(int32 Team)
{
	// WOTPlayer.ServerChangeTeam: route to the authority game mode. ChangeTeam owns the range
	// clamp / smallest-team resolution (giCombatBase.ChangeTeam / giMPBattle.ChangeTeam).
	if (!CanChangeTeamNow())
	{
		// WOTPlayer.uc:4228-4232: refuse while editing and tell the requester why
		// (CantChangeTeamsStr, WoT.int:48). Surfaced center-screen in the port.
		ClientCenterMessage(WoTPlayerControllerRetailText(
			TEXT("WOTPlayer"), TEXT("CantChangeTeamsStr"),
			TEXT("Team switching is unavailable during editing.")));
		return;
	}
	if (AWoTGameMode* GM = GetWorld()->GetAuthGameMode<AWoTGameMode>())
	{
		GM->ChangeTeam(this, Team);
	}
}

bool AWoTPlayerController::ServerChangeSkin_Validate(
	const FString& SkinName, const FString& FaceName, uint8 TeamNum)
{
	return FaceName.IsEmpty() && TeamNum == 255
		&& FWoTFrontEndModel::IsValidSkinOption(SkinName);
}

void AWoTPlayerController::ServerChangeSkin_Implementation(
	const FString& SkinName, const FString& /*FaceName*/, uint8 /*TeamNum*/)
{
	// PlayerPawn.ServerChangeSkin gates on bCanChangeSkin BEFORE SetMultiSkin;
	// the pawn fold re-checks it with the prefix gate.
	const AWoTGameMode* GM = GetWorld()
		? GetWorld()->GetAuthGameMode<AWoTGameMode>() : nullptr;
	if (!GM || !GM->bCanChangeSkin)
	{
		return;
	}
	// The menu is client-owned and can briefly still show the faction which
	// preceded a Battle team change. Gate persistence against the authority's
	// current faction identity, not the replicated menu snapshot or live pawn.
	const AWoTPlayerState* State = GetPlayerState<AWoTPlayerState>();
	const UClass* FactionClass = State && State->FactionClass
		? State->FactionClass.Get() : nullptr;
	const AWoTCharacter* FactionCDO = FactionClass
		? FactionClass->GetDefaultObject<AWoTCharacter>() : nullptr;
	const FString MeshName = FactionCDO
		? FactionCDO->BodyMeshPath.GetAssetName() : FString();
	if (MeshName.IsEmpty()
		|| !SkinName.StartsWith(MeshName, ESearchCase::IgnoreCase))
	{
		return;
	}
	// Remember the request for future respawns regardless of the current pawn's gate
	// outcome: UE1 stored the accepted skin on the persistent pawn; a request rejected
	// only by the CURRENT mesh's prefix (e.g. pre-morph) matches retail by re-running
	// the same gate against the next pawn.
	PersonalSkinURL = SkinName;
	if (AWoTCharacter* WoTPawn = Cast<AWoTCharacter>(GetPawn()))
	{
		WoTPawn->ServerSetPersonalSkin(SkinName);
	}
}

void AWoTPlayerController::ClientApplyCitadelSkin_Implementation(
	EWoTPlayerFaction TeamFaction)
{
	if (const UWoTGameInstance* Instance =
		Cast<UWoTGameInstance>(GetGameInstance()))
	{
		ServerChangeSkin(FWoTFrontEndModel::SkinOption(
			TeamFaction, Instance->GetCitadelSkinColor()), FString(), 255);
	}
}

bool AWoTPlayerController::CanRequestCitadelEdit() const
{
	if (IsEditingCitadel() || IsOwnerEditingCitadel() || !GetWorld())
	{
		return false;
	}
	if (const AWoTBattleGameMode* BattleMode =
		GetWorld()->GetAuthGameMode<AWoTBattleGameMode>())
	{
		return !BattleMode->CanTeleport();
	}
	for (TActorIterator<AWoTBattleInfo> It(GetWorld()); It; ++It)
	{
		if (IsValid(*It)) return It->bTeleportersDisabled;
	}
	return false;
}

void AWoTPlayerController::ServerEditCitadel(int32 Mode)
{
	// CitadelEditor.ServerEditCitadel overrides the normal entry exec and uses
	// the same key to leave (WOTPlayer.uc:5146-5155).
	if (IsEditingCitadel() || IsOwnerEditingCitadel())
	{
		CitadelEndEdit();
		return;
	}
	ServerRequestCitadelEdit(Mode);
}

void AWoTPlayerController::ServerEditCitadelInput()
{
	ServerEditCitadel();
}

void AWoTPlayerController::ServerRequestCitadelEdit_Implementation(int32 Mode)
{
	// Do not trust a stale owner mirror: the authoritative state still treats a
	// repeated request as the retail voluntary-exit command.
	if (IsEditingCitadel())
	{
		CitadelEndEdit();
		return;
	}
	if (CanRequestCitadelEdit())
	{
		CitadelBeginEdit(Mode);
		return;
	}
	if (FWoTRetailText::EnsureLoaded())
	{
		if (const FString* Message = FWoTRetailText::FindSectionValue(
			TEXT("WOTPlayer"), TEXT("CantEditLevelStr")))
		{
			ClientCenterMessage(*Message);
		}
	}
}

void AWoTPlayerController::CitadelBeginEdit(int32 Mode)
{
	if (!HasAuthority())
	{
		ServerBeginCitadelEdit(Mode);
		return;
	}

	AWoTBattleGameMode* BattleMode = GetWorld()
		? GetWorld()->GetAuthGameMode<AWoTBattleGameMode>() : nullptr;
	if (!IsValid(BattleMode) || BattleMode->CanTeleport() || IsEditingCitadel())
	{
		return;
	}
	const uint8 SessionTeam = CitadelControllerTeam(this);
	if (SessionTeam == 255 || !IsValid(BattleMode->Battle.Get())
		|| SessionTeam >= BattleMode->Battle->NumTeams || SessionTeam >= 4)
	{
		return;
	}
	AWoTZoneVolume* EntryZone = AWoTZoneVolume::EditZoneAt(
		GetWorld(), GetCitadelViewerLocation());
	if (!IsValid(EntryZone) || EntryZone->EditTeam != SessionTeam)
	{
		return;
	}
	AWoTBudgetInfo* Budget = BattleMode->FindTeamBudget(SessionTeam);
	if (!IsValid(Budget) || !Budget->BeginEdit(this))
	{
		return;
	}
	const EWoTCitadelEditCameraMode RequestedCameraMode = Mode == 1
		? EWoTCitadelEditCameraMode::Orbit : EWoTCitadelEditCameraMode::Roam;
	CaptureCitadelEditStart(RequestedCameraMode);

	EditSessionTeam = SessionTeam;
	EditSessionBudget = Budget;
	CitadelEditCameraMode = RequestedCameraMode;
	CitadelEditZone = EntryZone;
	CitadelOrbitViewOffset = FRotator::ZeroRotator;
	// CitadelOrbitMode.BeginState (WOTPlayer.uc:5262-5272): orbiting ghosts the body —
	// hidden, non-colliding, PHYS_None — so it cannot wander or be seen while the
	// player edits from the zone camera. Roam keeps the walking body (retail roam
	// moves the player). EndEditSession restores it.
	if (RequestedCameraMode == EWoTCitadelEditCameraMode::Orbit)
	{
		if (AWoTCharacter* PawnChar = Cast<AWoTCharacter>(GetPawn()))
		{
			PawnChar->SetCitadelOrbitGhost(true);
		}
	}
	OnRep_CitadelEditCamera();
	RefreshOwnerCitadelBudgetMirror();
	OwnerEditSessionTeam = SessionTeam;
	OnRep_OwnerEditSessionTeam();
	ForceNetUpdate();
	BattleMode->SetEditing(SessionTeam, true);

	for (TActorIterator<AWoTTrap> It(GetWorld()); It; ++It)
	{
		AWoTTrap* ResourceRoot = *It;
		if (!IsValid(ResourceRoot) || ResourceRoot->GetBaseResource() != ResourceRoot)
		{
			continue;
		}
		AWoTZoneVolume* ResourceZone = AWoTZoneVolume::EditZoneAt(
			GetWorld(), ResourceRoot->GetActorLocation());
		if (IsValid(ResourceZone) && ResourceZone->EditTeam == EditSessionTeam)
		{
			ResourceRoot->BeginEditingResource(EditSessionTeam);
		}
	}
	for (TActorIterator<AWoTSeal> It(GetWorld()); It; ++It)
	{
		AWoTSeal* Seal = *It;
		if (!IsValid(Seal) || Seal->bPreviewSeal)
		{
			continue;
		}
		AWoTZoneVolume* ResourceZone = AWoTZoneVolume::EditZoneAt(
			GetWorld(), Seal->GetActorLocation());
		if (IsValid(ResourceZone) && ResourceZone->EditTeam == EditSessionTeam)
		{
			Seal->BeginEditingResource(EditSessionTeam);
		}
	}
	// WOTPawn.BeginEditingResource (uc:552-559), narrowed to the Battle-mode
	// deployed registry so summons sharing AWoTNPCCharacter are never frozen.
	TArray<AWoTNPCCharacter*> DeployedTroops;
	BattleMode->GetCitadelDeployedTroops(EditSessionTeam, DeployedTroops);
	for (AWoTNPCCharacter* Troop : DeployedTroops)
	{
		if (!IsValid(Troop))
		{
			continue;
		}
		AWoTZoneVolume* ResourceZone = AWoTZoneVolume::EditZoneAt(
			GetWorld(), Troop->GetActorLocation());
		if (!IsValid(ResourceZone) || ResourceZone->EditTeam != EditSessionTeam)
		{
			continue;
		}
		Troop->AssignedTeam = EditSessionTeam;
		if (AWoTNPCController* NPCController =
			Cast<AWoTNPCController>(Troop->GetController()))
		{
			NPCController->ForceDormant();
		}
		if (IsMashadarFocusPawn(Troop))
		{
			Troop->SetActorHiddenInGame(false); // MashadarFocusPawn.uc:124-133 Show.
			// MashadarFocusPawn.uc:130: the RETAINED guide is reclaimed unconditionally
			// (no fresh eligibility/closest lookup); search only when it no longer exists.
			AWoTMashadarGuide* Guide =
				BattleMode->GetCitadelTroopMashadarGuide(Troop);
			if (!IsValid(Guide))
			{
				Guide = FindClosestBindableMashadarGuide(
					GetWorld(), Troop->GetActorLocation());
				BattleMode->SetCitadelTroopMashadarGuide(Troop, Guide);
			}
			if (IsValid(Guide) && Guide->GetFocusPawn() != Troop)
			{
				Guide->SetFocusPawn(Troop);
			}
		}
	}
}

void AWoTPlayerController::ServerBeginCitadelEdit_Implementation(int32 Mode)
{
	CitadelBeginEdit(Mode);
}

FString AWoTPlayerController::CantLeaveEditorStr()
{
	return WoTPlayerControllerRetailText(
		TEXT("WOTPlayer"), TEXT("CantLeaveEditorStr"),
		TEXT("Finish editing before leaving."));
}

void AWoTPlayerController::CitadelEndEdit()
{
	// WOTPlayer.uc:5146-5155 (CitadelEditor.ServerEditCitadel): bProhibitEditorExit
	// refuses the VOLUNTARY exit with the shipped center message. Involuntary
	// teardown (logout/match flow) calls EndEditSession directly and is never gated.
	if (bProhibitEditorExit)
	{
		ClientCenterMessage(CantLeaveEditorStr());
		return;
	}
	// EditorHUD.LeftMouseDown/exit cleanup is owner-local and must happen before a remote
	// exit request can race the replicated session edge back to this client.
	ClearCitadelSelectionPresentation();
	if (!HasAuthority())
	{
		ServerEndCitadelEdit();
		return;
	}
	EndEditSession();
}

void AWoTPlayerController::ServerEndCitadelEdit_Implementation()
{
	// The server-side twin of the exec gate (the RPC must not trust the client's
	// owner-mirrored flag).
	if (bProhibitEditorExit)
	{
		ClientCenterMessage(CantLeaveEditorStr());
		return;
	}
	EndEditSession();
}

void AWoTPlayerController::EndEditSession()
{
	if (!HasAuthority() || !IsEditingCitadel())
	{
		return;
	}

	const uint8 SessionTeam = EditSessionTeam;
	AWoTBudgetInfo* Budget = EditSessionBudget.Get();
	// CitadelOrbitMode.EndState (WOTPlayer.uc:5277-5289): restore the ghosted body —
	// collision/visibility/PHYS_Falling — before the transform restore below. Idempotent
	// (a roam session never ghosted), and every teardown seam (normal exit, Logout,
	// OnPossess, forced-phase reconcile, Destroyed) converges here.
	if (AWoTCharacter* PawnChar = Cast<AWoTCharacter>(GetPawn()))
	{
		PawnChar->SetCitadelOrbitGhost(false);
	}
	if (!EditStartLoc.IsZero())
	{
		ClientRestoreCitadelEditTransform(EditStartLoc, EditStartRot);
	}
	RestoreCitadelEditStart();
	CitadelEditCameraMode = EWoTCitadelEditCameraMode::Inactive;
	CitadelEditZone = nullptr;
	CitadelOrbitViewOffset = FRotator::ZeroRotator;
	OnRep_CitadelEditCamera();
	EditSessionTeam = 255;
	EditSessionBudget = nullptr;
	OwnerEditBudget.Reset();
	OwnerEditSessionTeam = 255;
	OnRep_OwnerEditBudget();
	OnRep_OwnerEditSessionTeam();
	ForceNetUpdate();

	UWorld* World = GetWorld();
	AWoTBattleGameMode* BattleMode = World
		? World->GetAuthGameMode<AWoTBattleGameMode>() : nullptr;
	if (IsValid(BattleMode))
	{
		BattleMode->SetEditing(SessionTeam, false);
	}
	if (World)
	{
		for (TActorIterator<AWoTTrap> It(World); It; ++It)
		{
			AWoTTrap* ResourceRoot = *It;
			if (!IsValid(ResourceRoot) || ResourceRoot->GetBaseResource() != ResourceRoot)
			{
				continue;
			}
			AWoTZoneVolume* ResourceZone = AWoTZoneVolume::EditZoneAt(
				World, ResourceRoot->GetActorLocation());
			if (IsValid(ResourceZone) && ResourceZone->EditTeam == SessionTeam)
			{
				ResourceRoot->EndEditingResource();
			}
		}
		for (TActorIterator<AWoTSeal> It(World); It; ++It)
		{
			AWoTSeal* Seal = *It;
			if (!IsValid(Seal) || Seal->bPreviewSeal)
			{
				continue;
			}
			AWoTZoneVolume* ResourceZone = AWoTZoneVolume::EditZoneAt(
				World, Seal->GetActorLocation());
			if (IsValid(ResourceZone) && ResourceZone->EditTeam == SessionTeam)
			{
				Seal->EndEditingResource();
			}
		}
		if (IsValid(BattleMode))
		{
			TArray<AWoTNPCCharacter*> DeployedTroops;
			BattleMode->GetCitadelDeployedTroops(SessionTeam, DeployedTroops);
			for (AWoTNPCCharacter* Troop : DeployedTroops)
			{
				if (!IsValid(Troop))
				{
					continue;
				}
				AWoTZoneVolume* ResourceZone = AWoTZoneVolume::EditZoneAt(
					World, Troop->GetActorLocation());
				if (!IsValid(ResourceZone) || ResourceZone->EditTeam != SessionTeam)
				{
					continue;
				}
				// MashadarFocusPawn.uc:111-120: unbind and hide before Super wakes it.
				if (IsMashadarFocusPawn(Troop))
				{
					if (AWoTMashadarGuide* Guide =
						BattleMode->GetCitadelTroopMashadarGuide(Troop);
						IsValid(Guide) && Guide->GetFocusPawn() == Troop)
					{
						Guide->SetFocusPawn(nullptr);
					}
					// WOTPawn.Hide (uc:660-664) drops only actor/player collision — the
					// marker already ignores pawns; world collision stays so the hidden
					// marker keeps standing in its edit zone (bHidden replicates).
					Troop->SetActorHiddenInGame(true);
				}
				if (AWoTNPCController* NPCController =
					Cast<AWoTNPCController>(Troop->GetController()))
				{
					NPCController->UnForceDormant();
				}
			}
		}
	}
	// Retail EndState order: the EndEditingResource sweeps run FIRST, then
	// Budget.EndEdit (whose authored EndEditEvent fires synchronously — MP-live,
	// Battle3_02 authors three), then the seal credit/bank (WOTPlayer.uc:5195-5211).
	// A listener must never observe a resource still in its editing state.
	if (IsValid(Budget))
	{
		Budget->EndEdit(this);
	}
	if (IsValid(BattleMode))
	{
		// AutoPlaceSealsOnAltar first credits EVERY team-stamped seal, including
		// collision-off ground seals (WOTPlayer.uc:4324-4331). Preview ghosts are
		// local presentation and never participate.
		if (World)
		{
			for (TActorIterator<AWoTSeal> It(World); It; ++It)
			{
				AWoTSeal* Seal = *It;
				if (IsValid(Seal) && !Seal->bPreviewSeal
					&& Seal->Team == SessionTeam)
				{
					Seal->SetControllingTeam(this);
				}
			}
		}
		int32 Placed = 0;
		if (AWoTSealAltar* Altar = BattleMode->FindTeamAltar(SessionTeam))
		{
			if (IsValid(Budget) && IsValid(Altar))
			{
				Placed = Budget->PlaceSeals(Altar, this);
			}
		}
		// Retail decrements the per-player card once per forced bank and does not
		// clamp; after any first edit session this may legitimately be negative.
		OwnerSealCardCount -= Placed;
		ForceNetUpdate();
		// CitadelEditor.EndState (WOTPlayer.uc): auto-place the remaining seals and ready
		// the team — but it never ASSESSES. Play begins only via the fire path
		// (WOTPlayer.UseAngreal while frozen -> ServerAssessBattlefield), so the last
		// editor exiting must NOT auto-start the battle.
		BattleMode->EnableTeamReady(SessionTeam);
	}
}

void AWoTPlayerController::ServerToggleResource_Implementation(AWoTTrap* Selected)
{
	if (!HasAuthority() || !IsEditingCitadel() || !IsValid(Selected)
		|| !IsValid(EditSessionBudget.Get()))
	{
		return;
	}
	UWorld* World = GetWorld();
	AWoTZoneVolume* ViewerZone = AWoTZoneVolume::EditZoneAt(
		World, GetCitadelViewerLocation());
	if (!IsValid(ViewerZone) || !ViewerZone->bAllowEditing)
	{
		return;
	}
	const FName AdapterName =
		WoTCitadelResources::FindAdapterForResourceClass(Selected->GetClass());
	const FWoTResourceDescriptor& Descriptor =
		WoTCitadelResources::FindByAdapterName(AdapterName);
	if (AdapterName.IsNone() || !Descriptor.IsValidDescriptor()
		|| Descriptor.bSpawnTempResource)
	{
		return;
	}
	AWoTBattleGameMode* BattleMode = World
		? World->GetAuthGameMode<AWoTBattleGameMode>() : nullptr;
	// Reject once play has begun — an edit session must not outlive the build phase
	// (independent of the begin-edit gate) — and require the resource sit in THIS
	// team's citadel (nearest seal altar).
	if (!IsValid(BattleMode) || BattleMode->CanTeleport()
		|| BattleMode->NearestAltarTeam(Selected->GetActorLocation()) != EditSessionTeam)
	{
		return;
	}

	AWoTBudgetInfo* Budget = EditSessionBudget.Get();
	const int32 BudgetIndex = Budget->GetIndex(AdapterName);
	if (BudgetIndex == INDEX_NONE)
	{
		return;
	}
	if (!CitadelResourceIsActive(Selected))
	{
		// Activate: an unowned (255) or already-ours resource; spend one.
		if ((Selected->Team != 255 && Selected->Team != EditSessionTeam)
			|| Budget->GetItemCount(BudgetIndex) <= 0)
		{
			return;
		}
		Selected->BeginEditingResource(EditSessionTeam);
		CitadelResourceActivate(Selected);
		Budget->Decrement(AdapterName);
		RefreshOwnerCitadelBudgetMirror();
		// ServerActivateResource success+ChangeState (WOTPlayer.uc:5014):
		// ClientPlaySound(Resource.GetDeploySound()) after the budget spend.
		ClientPlayUISound(Selected->GetDeploySoundPath(), 1.0f);
	}
	else
	{
		// Deactivate/refund: ONLY our own active resource — never mint budget from a
		// 255/foreign one that this team never paid for.
		if (Selected->Team != EditSessionTeam)
		{
			return;
		}
		CitadelResourceDeactivate(Selected);
		Budget->Increment(AdapterName);
		RefreshOwnerCitadelBudgetMirror();
		// ServerDeactivateResource success+ChangeState (WOTPlayer.uc:5043):
		// ClientPlaySound(Resource.GetRemoveSound()) after the refund.
		ClientPlayUISound(Selected->GetRemoveSoundPath(), 1.0f);
	}
}

void AWoTPlayerController::ServerCreateResource_Implementation(
	FName Adapter, FVector Loc, FVector Normal)
{
	if (!HasAuthority() || !IsEditingCitadel() || !IsValid(EditSessionBudget.Get()))
	{
		return;
	}
	UWorld* World = GetWorld();
	AWoTZoneVolume* ViewerZone = AWoTZoneVolume::EditZoneAt(
		World, GetCitadelViewerLocation());
	if (!IsValid(ViewerZone) || !ViewerZone->bAllowEditing)
	{
		return;
	}
	AWoTBattleGameMode* BattleMode = World
		? World->GetAuthGameMode<AWoTBattleGameMode>() : nullptr;
	if (!IsValid(BattleMode) || BattleMode->CanTeleport())
	{
		return;
	}
	const FWoTResourceDescriptor& Descriptor =
		WoTCitadelResources::FindByAdapterName(Adapter);
	if (!Descriptor.IsValidDescriptor() || !Descriptor.bSpawnTempResource)
	{
		return;
	}
	AWoTBudgetInfo* Budget = EditSessionBudget.Get();
	const int32 BudgetIndex = Budget->GetIndex(Adapter);
	if (BudgetIndex == INDEX_NONE || Budget->GetItemCount(BudgetIndex) <= 0)
	{
		return;
	}

	if (!Descriptor.CreatureRow.IsNone())
	{
		// WOTPlayer.uc:4913-4936 + WOTPawn.uc:617-647. Pawn placement ignores
		// the cursor vectors: static AdjustSpawnLocation uses the player-front spot
		// (+Z16), then DeployResource stamps the unadjusted waiting goal.
		APawn* PlayerPawn = GetPawn();
		// TroopSpawnLocation: APlayerController already has a SpawnLocation member
		// (MSVC C4458-as-error; Clang allowed the shadow).
		FVector TroopSpawnLocation;
		FRotator SpawnRotation;
		FVector WaitingLocation;
		FRotator WaitingRotation;
		if (!CalculateCitadelTroopPlacement(PlayerPawn, Descriptor.CreatureRow,
				/*bSpawnAdjusted*/ true, TroopSpawnLocation, SpawnRotation)
			|| !CalculateCitadelTroopPlacement(PlayerPawn, Descriptor.CreatureRow,
				/*bSpawnAdjusted*/ false, WaitingLocation, WaitingRotation)
			|| BattleMode->NearestAltarTeam(TroopSpawnLocation) != EditSessionTeam)
		{
			return;
		}

		AWoTNPCCharacter* Troop = WoTCreatureData::SpawnCreature(
			*World, Descriptor.CreatureRow, TroopSpawnLocation, EditSessionTeam,
			/*bCombatReady*/ false);
		if (!IsValid(Troop))
		{
			return; // SpawnActor may return null when the actor self-destroys.
		}
		Troop->SetOwner(PlayerPawn);
		Troop->SetActorRotation(SpawnRotation);
		Troop->AssignedTeam = EditSessionTeam;
		AWoTNPCController* NPCController =
			Cast<AWoTNPCController>(Troop->GetController());
		if (!IsValid(NPCController) || !IsValid(NPCController->GetBrain()))
		{
			Troop->Destroy();
			return;
		}
		if (UCharacterMovementComponent* Movement = Troop->GetCharacterMovement())
		{
			Movement->SetMovementMode(MOVE_Falling);
		}
		NPCController->GetBrain()->SetWaitingGoal(WaitingLocation, WaitingRotation);
		// LegendPawn.uc:2475-2515: Dormant.SetWaitingGoal also teleports. Most
		// rows start Inactive and retain +Z16; MashadarFocusPawn starts Dormant.
		if (NPCController->IsDormant())
		{
			Troop->SetActorLocationAndRotation(
				WaitingLocation, WaitingRotation, false, nullptr,
				ETeleportType::TeleportPhysics);
		}

		AWoTMashadarGuide* MashadarGuide = nullptr;
		if (IsMashadarFocusPawn(Troop))
		{
			// MashadarFocusPawn.uc:177-184 is the only pawn DeployResource that
			// can fail. No eligible guide means destroy with no budget spend.
			MashadarGuide = FindClosestBindableMashadarGuide(
				World, Troop->GetActorLocation());
			if (!IsValid(MashadarGuide))
			{
				Troop->Destroy();
				return;
			}
		}

		// BeginEditingResource -> ActivateResource -> Show (WOTPlayer.uc:4930-4932).
		BattleMode->RegisterCitadelDeployedTroop(Troop, EditSessionTeam);
		NPCController->ForceDormant();
		Troop->SetActorHiddenInGame(false);
		Troop->SetActorEnableCollision(true);
		if (IsValid(MashadarGuide))
		{
			BattleMode->SetCitadelTroopMashadarGuide(Troop, MashadarGuide);
			MashadarGuide->SetFocusPawn(Troop);
			MashadarGuide->Deploy();
		}
		Budget->Decrement(Adapter);
		RefreshOwnerCitadelBudgetMirror();
		// WOTPlayer.uc:4938 ClientPlaySound(Resource.GetDeploySound()) — the pawn
		// resource virtual resolves WOT.Editor.DeployPawn (WOTPawn.uc:538-542). The
		// pawn-class getter is folded to this branch: it is the funnel's single troop
		// dispatch point. Success path only,
		// after the budget spend, owner-only.
		ClientPlayUISound(FName(TEXT("WOT/DeployPawn")), 1.0f);
		return;
	}

	if (Descriptor.ResourceClass
		&& Descriptor.ResourceClass->IsChildOf(AWoTSeal::StaticClass()))
	{
		// EditorHUD.LeftMouseDown card gate + WOTPlayer.ServerCreateResource
		// (EditorHUD.uc:270; WOTPlayer.uc:4897-4939).
		if (OwnerSealCardCount <= 0 || Loc.ContainsNaN() || Normal.ContainsNaN()
			|| Normal.IsNearlyZero())
		{
			return;
		}
		Normal = Normal.GetSafeNormal();
		if (BattleMode->NearestAltarTeam(Loc) != EditSessionTeam)
		{
			return;
		}

		const FTransform SealSpawnTransform(Normal.Rotation(), Loc);
		AWoTSeal* Seal = World->SpawnActorDeferred<AWoTSeal>(
			Descriptor.ResourceClass, SealSpawnTransform, GetPawn(), nullptr,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!IsValid(Seal))
		{
			return;
		}
		// The deferred inventory must not emit a transient UE5 pickup overlap
		// before DeployResource validates and freezes it collision-off.
		if (IsValid(Seal->Collision))
		{
			Seal->Collision->SetGenerateOverlapEvents(false);
		}
		Seal->FinishSpawning(SealSpawnTransform);
		if (!IsValid(Seal) || !Seal->DeployResource(Loc, Normal))
		{
			if (IsValid(Seal))
			{
				Seal->Destroy();
			}
			return;
		}
		if (BattleMode->NearestAltarTeam(Seal->GetActorLocation()) != EditSessionTeam)
		{
			Seal->Destroy();
			return;
		}
		Seal->BeginEditingResource(EditSessionTeam);
		Seal->SetOwner(nullptr);
		Budget->Decrement(Adapter);
		--OwnerSealCardCount;
		RefreshOwnerCitadelBudgetMirror();
		ClientPlayUISound(FName(TEXT("WOT/DeploySeal")), 1.0f);
		return;
	}

	// The live Seal create leg is above. Remove/toggle remain retail-dead:
	// placed seals are collision-off and untraceable (Seal.uc:311/317), while an
	// altar has no logical base resource. This return now rejects only descriptors
	// outside the troop/seal/trap families.
	if (!Descriptor.ResourceClass
		|| !Descriptor.ResourceClass->IsChildOf(AWoTTrap::StaticClass()))
	{
		return;
	}
	if (Loc.ContainsNaN() || Normal.ContainsNaN() || Normal.IsNearlyZero())
	{
		return;
	}
	Normal = Normal.GetSafeNormal();
	if (BattleMode->NearestAltarTeam(Loc) != EditSessionTeam)
	{
		return;
	}

	const FTransform SpawnTransform(Normal.Rotation(), Loc);
	APawn* PlayerPawn = GetPawn();
	AWoTTrap* Trap = World->SpawnActorDeferred<AWoTTrap>(
		Descriptor.ResourceClass, SpawnTransform, this, PlayerPawn,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!IsValid(Trap))
	{
		return;
	}
	Trap->SetDamageInstigatorController(this);
	Trap->FinishSpawning(SpawnTransform);
	if (!IsValid(Trap) || !Trap->DeployResource(Loc, Normal))
	{
		if (IsValid(Trap))
		{
			Trap->Destroy();
		}
		return;
	}
	if (BattleMode->NearestAltarTeam(Trap->GetActorLocation()) != EditSessionTeam)
	{
		Trap->Destroy();
		return;
	}
	Trap->BeginEditingResource(EditSessionTeam);
	Budget->Decrement(Adapter);
	RefreshOwnerCitadelBudgetMirror();
	// WOTPlayer.uc:4938 ClientPlaySound(Resource.GetDeploySound()) — the trap base
	// virtual resolves WOTBase.Editor.DeployTrap (Trap.uc:57-60). Success path only,
	// after the budget spend; every failure leg above returns before any sound.
	ClientPlayUISound(Trap->GetDeploySoundPath(), 1.0f);
}

void AWoTPlayerController::ServerRemoveResource_Implementation(AActor* Selected)
{
	if (!HasAuthority() || !IsEditingCitadel() || !IsValid(Selected)
		|| !IsValid(EditSessionBudget.Get()))
	{
		return;
	}
	UWorld* World = GetWorld();
	AWoTZoneVolume* ViewerZone = AWoTZoneVolume::EditZoneAt(
		World, GetCitadelViewerLocation());
	if (!IsValid(ViewerZone) || !ViewerZone->bAllowEditing)
	{
		return;
	}
	AWoTBattleGameMode* BattleMode = World
		? World->GetAuthGameMode<AWoTBattleGameMode>() : nullptr;
	if (!IsValid(BattleMode) || BattleMode->CanTeleport())
	{
		return;
	}

	AActor* LogicalResource = WoTCitadelResources::ResolveLogicalResource(Selected);
	if (!IsValid(LogicalResource))
	{
		return;
	}
	// Citadel-location ownership (parity with create/toggle): only remove a resource
	// that sits in THIS team's citadel — Team alone would let a client refund-grief a
	// same-team-tagged resource elsewhere (e.g. a pre-placed one).
	AWoTZoneVolume* ResourceZone = AWoTZoneVolume::EditZoneAt(
		World, LogicalResource->GetActorLocation());
	if (!IsValid(ResourceZone) || ResourceZone->EditTeam != EditSessionTeam
		|| BattleMode->NearestAltarTeam(LogicalResource->GetActorLocation())
			!= EditSessionTeam)
	{
		return;
	}

	AWoTNPCCharacter* Troop = Cast<AWoTNPCCharacter>(LogicalResource);
	AWoTTrap* Base = Cast<AWoTTrap>(LogicalResource);
	if (IsValid(Troop))
	{
		// The weak registry is the authority seam: a same-row summon/map NPC is not
		// removable and cannot mint a refund.
		if (Troop->AssignedTeam != EditSessionTeam
			|| !BattleMode->IsCitadelDeployedTroop(Troop, EditSessionTeam))
		{
			return;
		}
	}
	else if (!IsValid(Base) || Base->Team != EditSessionTeam)
	{
		return;
	}

	const FName Adapter = IsValid(Troop)
		? WoTCitadelResources::FindAdapterForCreatureRow(Troop->CreatureRowName)
		: WoTCitadelResources::FindAdapterForResourceClass(Base->GetClass());
	const FWoTResourceDescriptor& Descriptor =
		WoTCitadelResources::FindByAdapterName(Adapter);
	if (Adapter.IsNone() || !Descriptor.IsValidDescriptor()
		|| !Descriptor.bSpawnTempResource)
	{
		return;
	}
	if (IsValid(Troop))
	{
		// MashadarFocusPawn.uc:158-168: UnGuard + unbind before the marker is
		// hidden/destroyed. The association is removed with the registry entry.
		if (IsMashadarFocusPawn(Troop))
		{
			if (AWoTMashadarGuide* Guide =
				BattleMode->GetCitadelTroopMashadarGuide(Troop); IsValid(Guide))
			{
				// MashadarFocusPawn.uc:160: BOTH UnGuard and the unbind run only when
				// this marker owns the guide — a stale association must not clear
				// another focus's live guard state.
				if (Guide->GetFocusPawn() == Troop)
				{
					Guide->UnGuard();
					Guide->SetFocusPawn(nullptr);
				}
			}
		}
		Troop->SetActorHiddenInGame(true); // WOTPawn.RemoveResource (uc:652-655).
		Troop->SetActorEnableCollision(false);
		EditSessionBudget->Increment(Adapter);
		RefreshOwnerCitadelBudgetMirror();
		// WOTPlayer.uc:4965 ClientPlaySound(Resource.GetRemoveSound()) — the pawn
		// resource virtual resolves WOT.Editor.RemovePawn (WOTPawn.uc:546-548); retail
		// plays after the refund, before Destroy. Folded literal, as at the deploy leg.
		ClientPlayUISound(FName(TEXT("WOT/RemovePawn")), 1.0f);
		BattleMode->UnregisterCitadelDeployedTroop(Troop);
		Troop->Destroy();
		return;
	}

	if (Base->RemoveResource())
	{
		EditSessionBudget->Increment(Adapter);
		RefreshOwnerCitadelBudgetMirror();
		// WOTPlayer.uc:4965 ClientPlaySound(Resource.GetRemoveSound()) — the trap base
		// virtual resolves WOTBase.Editor.RemoveTrap (Trap.uc:62-65); RemoveResource()
		// false plays nothing (retail's success-only ordering).
		ClientPlayUISound(Base->GetRemoveSoundPath(), 1.0f);
		Base->Destroy();
	}
}

EWoTCitadelEditorAction AWoTPlayerController::HandleCitadelEditorClick()
{
	if (!IsLocalController() || !IsOwnerEditingCitadel()
		|| !HasValidLocalCitadelCreateZone())
	{
		return EWoTCitadelEditorAction::None;
	}

	// The native HUD has no window stack yet. When one is ported, its active-window
	// dismiss-and-return belongs here, before this trace, matching EditorHUD.LeftMouseDown.
	AActor* GhostActor = CitadelGhostPreviewActor.Get();
	if (AWoTTrap* Ghost = Cast<AWoTTrap>(GhostActor); IsValid(Ghost))
	{
		Ghost->SetPreviewVisible(false);
	}
	else if (IsValid(GhostActor))
	{
		GhostActor->SetActorHiddenInGame(true);
	}
	FWoTCitadelEditorTraceResult Hit;
	if (!EditorTrace(Hit, GhostActor))
	{
		return EWoTCitadelEditorAction::None;
	}
	if (IsValid(GhostActor))
	{
		SyncCitadelGhostPreview(Hit);
	}

	const FName AdapterName = SelectedCitadelAdapter;
	const FWoTCitadelEditorSelection Selection = CitadelEditorSelection(
		AdapterName, GetOwnerCitadelBudgetCount(AdapterName));
	const EWoTCitadelEditorAction Action =
		WoTCitadelResources::ResolveEditorAction(Hit, Selection);
	// Recheck after trace/resolution: session exit and actor destruction make this click
	// stale. The RPCs repeat the authoritative checks, but the local seam is fail-closed too.
	if (!IsOwnerEditingCitadel() || !HasValidLocalCitadelCreateZone())
	{
		return EWoTCitadelEditorAction::None;
	}

	AActor* SelectedResource =
		WoTCitadelResources::ResolveLogicalResource(Hit.HitActor);
	AWoTTrap* Selected = Cast<AWoTTrap>(SelectedResource);
	switch (Action)
	{
	case EWoTCitadelEditorAction::Create:
		ServerCreateResource(AdapterName, Hit.HitLocation, Hit.HitNormal);
		break;
	case EWoTCitadelEditorAction::Remove:
		if (IsValid(SelectedResource))
		{
			ServerRemoveResource(SelectedResource);
		}
		break;
	case EWoTCitadelEditorAction::Toggle:
		if (IsValid(Selected))
		{
			ServerToggleResource(Selected);
		}
		break;
	case EWoTCitadelEditorAction::Deactivate:
		// Hover deactivation is owner-local. The shipped `true` deactivation is
		// gameplay, so retain the idempotent authority dispatch through the one toggle RPC.
		SetHoveredCitadelResource(nullptr);
		if (IsValid(Selected) && CitadelResourceIsActive(Selected))
		{
			ServerToggleResource(Selected);
		}
		break;
	case EWoTCitadelEditorAction::None:
	default:
		break;
	}
	return Action;
}

void AWoTPlayerController::CitadelClick()
{
	HandleCitadelEditorClick();
}

void AWoTPlayerController::ToggleInGameMenu()
{
	// Console.Typing consumes Escape and leaves no key event for ShowMenu.
	if (IsChatTyping())
	{
		CancelChatInput();
		return;
	}
	// WOTPlayer.ShowMenu destroys the active WOTWindow and returns. Help is a
	// WOTWindow, so this Escape must not also open the game menu.
	if (IsHelpOpen())
	{
		RemoveHelpWidget();
		return;
	}
	if (InGameMenu.IsLeaving())
	{
		return;
	}
	if (InGameMenu.IsOpen())
	{
		ResumeFromInGameMenu();
		return;
	}

	// WOTPlayer.CitadelEditor.ShowMenu routes Escape to ServerEditCitadel,
	// which ends the edit session instead of opening the retail menu.
	if (IsOwnerEditingCitadel())
	{
		CitadelEndEdit();
		return;
	}
	// WOTPlayer.ShowMenu first destroys an active WOTWindow and returns. Thus
	// Escape closes item info on the first press; only a later press opens menu.
	if (IsItemInfoOpen())
	{
		RemoveItemInfoWidget();
		return;
	}

	if (InGameMenu.Open())
	{
		CreateInGameMenuWidget();
	}
}

void AWoTPlayerController::ResumeFromInGameMenu()
{
	if (!InGameMenu.Resume())
	{
		return;
	}
	RemoveInGameMenuWidget(/*bRestoreGameInput*/ true);
}

void AWoTPlayerController::LeaveToFrontEnd()
{
	if (!InGameMenu.BeginLeave())
	{
		return;
	}
	UWoTSessionSubsystem* Sessions = GetGameInstance()
		? GetGameInstance()->GetSubsystem<UWoTSessionSubsystem>() : nullptr;
	if (!Sessions || !Sessions->Leave())
	{
		// A synchronous refusal leaves the overlay usable for Resume or retry.
		InGameMenu.CancelLeave();
	}
}

void AWoTPlayerController::CreateInGameMenuWidget()
{
	// Keep the model testable in headless worlds, but never initialize Slate or
	// mutate input/cursor state without a real local gameplay viewport.
	if (!IsLocalController() || GIsAutomationTesting || IsRunningDedicatedServer()
		|| GetNetMode() == NM_DedicatedServer || !GEngine || !GEngine->GameViewport)
	{
		return;
	}

	InGameMenuViewport = GEngine->GameViewport;
	InGameMenuPresentationAuthority =
		FWoTFrontEndPresentationAuthority::Create(this);
	InGameMenuWidget = SNew(SWoTInGameMenuOverlay)
		.PlayerController(this)
		.PresentationAuthority(InGameMenuPresentationAuthority);
	InGameMenuViewport->AddViewportWidgetContent(
		InGameMenuWidget.ToSharedRef(), /*ZOrder*/ 2000);

	// Opening from inside an input bind can otherwise strand held gameplay keys
	// (notably Fire's release) behind UI-only routing. UE's flush synthesizes the
	// release edges before focus moves to Slate.
	FlushPressedKeys();
	bShowMouseCursor = true;
	FInputModeUIOnly InputMode;
	InputMode.SetWidgetToFocus(InGameMenuWidget->InitialFocusWidget());
	InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	SetInputMode(InputMode);
	InGameMenuWidget->FocusResumeButton();
}

void AWoTPlayerController::RemoveInGameMenuWidget(bool bRestoreGameInput)
{
	const bool bHadWidget = InGameMenuWidget.IsValid();
	if (InGameMenuWidget.IsValid())
	{
		if (UGameViewportClient* Viewport = InGameMenuViewport.Get())
		{
			Viewport->RemoveViewportWidgetContent(InGameMenuWidget.ToSharedRef());
		}
		InGameMenuWidget.Reset();
	}
	InGameMenuPresentationAuthority.Reset();
	InGameMenuViewport.Reset();
	if (bRestoreGameInput && bHadWidget)
	{
		bShowMouseCursor = false;
		SetInputMode(FInputModeGameOnly());
	}
}

void AWoTPlayerController::Type()
{
	BeginChatInput(FString());
}

void AWoTPlayerController::Talk()
{
	// UE1 Console.Talk: TypedStr = "Say "; goto Typing (bNoStuff suppresses
	// the T which opened it; Slate receives focus after this action edge).
	BeginChatInput(TEXT("Say "));
}

void AWoTPlayerController::TeamTalk()
{
	// UE1 Console.TeamTalk: TypedStr = "TeamSay "; goto Typing.
	BeginChatInput(TEXT("TeamSay "));
}

void AWoTPlayerController::BeginChatInput(const FString& InitialText)
{
	if (bChatTyping || !IsLocalController() || IsInGameMenuOpen()
		|| IsHelpOpen() || IsItemInfoOpen())
	{
		return;
	}
	// UE1 Console's state Typing does not flush gameplay state on entry. Its
	// KeyEvent consumes presses but lets releases and axes continue to PlayerInput.
	bChatTyping = true;
	ChatInputText = InitialText;
	CreateChatInputWidget();
}

void AWoTPlayerController::CreateChatInputWidget()
{
	// The model and input suppression remain testable headlessly; Slate exists
	// only for a real owning gameplay viewport.
	if (!IsLocalController()
		|| (GIsAutomationTesting
#if WITH_AUTOMATION_TESTS
			&& !bEnableChatWidgetForTesting
#endif
		)
		|| IsRunningDedicatedServer()
		|| GetNetMode() == NM_DedicatedServer || !GEngine || !GEngine->GameViewport)
	{
		return;
	}

	ChatInputViewport = GEngine->GameViewport;
	ChatInputWidget = SNew(SWoTChatInputOverlay)
		.PlayerController(this)
		.InitialText(ChatInputText);
	ChatInputViewport->AddViewportWidgetContent(
		ChatInputWidget.ToSharedRef(), /*ZOrder*/ 2200);

	TSharedPtr<SViewport> ViewportWidget =
		ChatInputViewport->GetGameViewportWidget();
	uint32 SlateUserIndex = 0;
	if (FSlateApplication::IsInitialized())
	{
		SlateUserIndex = static_cast<uint32>(
			FSlateApplication::Get().GetUserIndexForKeyboard());
		if (const ULocalPlayer* LocalPlayer = Cast<ULocalPlayer>(Player))
		{
			if (const TSharedPtr<const FSlateUser> SlateUser =
				LocalPlayer->GetSlateUser())
			{
				SlateUserIndex = static_cast<uint32>(SlateUser->GetUserIndex());
			}
		}

		TSet<FKey> HeldGameplayMouseButtons;
		if (PlayerInput)
		{
			auto AddHeldGameplayMouseButton = [this,
				&HeldGameplayMouseButtons](const FKey& Key)
			{
				if (Key.IsMouseButton() && PlayerInput->IsPressed(Key))
				{
					HeldGameplayMouseButtons.Add(Key);
				}
			};
			for (const FInputActionKeyMapping& Mapping : PlayerInput->ActionMappings)
			{
				AddHeldGameplayMouseButton(Mapping.Key);
			}
			for (const FInputAxisKeyMapping& Mapping : PlayerInput->AxisMappings)
			{
				AddHeldGameplayMouseButton(Mapping.Key);
			}
		}
		ChatInputPreProcessor = MakeShared<FWoTChatInputPreProcessor>(
			this, MoveTemp(HeldGameplayMouseButtons), SlateUserIndex,
			ViewportWidget);
		if (!FSlateApplication::Get().RegisterInputPreProcessor(
			ChatInputPreProcessor, EInputPreProcessorType::Game))
		{
			ChatInputPreProcessor.Reset();
		}
	}

	bShowMouseCursor = false;
	const bool bRetainMouseCapture = ChatInputPreProcessor.IsValid()
		&& ChatInputPreProcessor->HasHeldGameplayMouseButton();
	FInputModeGameAndUI InputMode;
	InputMode.SetWidgetToFocus(ChatInputWidget->TextWidget());
	InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	InputMode.SetHideCursorDuringCapture(false);

	// Moving focus clears FSceneViewport's physical cache. Keep PlayerInput's
	// held state intact; the unified preprocessor forwards the eventual releases.
	UInputSettings* InputSettings = GetMutableDefault<UInputSettings>();
	const bool bPreviouslyFlushOnFocusLost =
		InputSettings->bShouldFlushPressedKeysOnViewportFocusLost;
	InputSettings->bShouldFlushPressedKeysOnViewportFocusLost = false;
	SetInputMode(InputMode);
	ChatInputViewport->SetIgnoreInput(true);
	ChatInputViewport->SetMouseCaptureMode(EMouseCaptureMode::NoCapture);
	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication& Slate = FSlateApplication::Get();
		Slate.SetKeyboardFocus(
			ChatInputWidget->TextWidget(), EFocusCause::SetDirectly);
		// Focus transfer releases ordinary viewport capture. If gameplay already
		// owns a mouse button, explicitly retain capture until that same button is
		// released so an outside-window release cannot be lost.
		if (bRetainMouseCapture && ViewportWidget.IsValid())
		{
			FWidgetPath ViewportPath;
			if (Slate.GeneratePathToWidgetUnchecked(
				ViewportWidget.ToSharedRef(), ViewportPath))
			{
				Slate.ProcessExternalReply(ViewportPath,
					FReply::Handled().CaptureMouse(ViewportWidget.ToSharedRef()),
					static_cast<int32>(SlateUserIndex),
					static_cast<int32>(
						FSlateApplicationBase::CursorPointerIndex));
				ChatInputPreProcessor->MarkGameplayMouseCaptureRetained();
			}
		}
	}
	InputSettings->bShouldFlushPressedKeysOnViewportFocusLost =
		bPreviouslyFlushOnFocusLost;
}

void AWoTPlayerController::RemoveChatInputWidget(bool bRestoreGameInput)
{
	const bool bHadWidget = ChatInputWidget.IsValid();
	if (ChatInputPreProcessor.IsValid())
	{
		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication& Slate = FSlateApplication::Get();
			ChatInputPreProcessor->ReleaseGameplayMouseCapture(Slate);
			Slate.UnregisterInputPreProcessor(ChatInputPreProcessor);
		}
		ChatInputPreProcessor.Reset();
	}
	if (ChatInputWidget.IsValid())
	{
		if (UGameViewportClient* Viewport = ChatInputViewport.Get())
		{
			Viewport->RemoveViewportWidgetContent(ChatInputWidget.ToSharedRef());
		}
		ChatInputWidget.Reset();
	}
	ChatInputViewport.Reset();
	bChatTyping = false;
	ChatInputText.Reset();
	if (bRestoreGameInput && bHadWidget)
	{
		SetInputMode(FInputModeGameOnly());
	}
}

void AWoTPlayerController::CancelChatInput()
{
	if (bChatTyping)
	{
		RemoveChatInputWidget(/*bRestoreGameInput*/ true);
	}
}

void AWoTPlayerController::SubmitChatInput(const FString& Line)
{
	if (!bChatTyping)
	{
		return;
	}
	ChatInputText = Line;
	RemoveChatInputWidget(/*bRestoreGameInput*/ true);

	// Shipping never forwards a bare Type line to APlayerController::ConsoleCommand.
	// Only the two retail chat commands are admitted, case-insensitively like UE1 exec.
	// The typed text is clamped here, so Say/TeamSay validation (which drops the
	// connection) only ever rejects a modified client.
	if (Line.Equals(TEXT("Say"), ESearchCase::IgnoreCase))
	{
		Say(FString());
	}
	else if (Line.StartsWith(TEXT("Say "), ESearchCase::IgnoreCase))
	{
		Say(Line.Mid(4).Left(MaxChatMessageLength));
	}
	else if (Line.Equals(TEXT("TeamSay"), ESearchCase::IgnoreCase))
	{
		TeamSay(FString());
	}
	else if (Line.StartsWith(TEXT("TeamSay "), ESearchCase::IgnoreCase))
	{
		TeamSay(Line.Mid(8).Left(MaxChatMessageLength));
	}
	else if (Line.Equals(TEXT("Vote"), ESearchCase::IgnoreCase))
	{
		Vote(FString());
	}
	else if (Line.StartsWith(TEXT("Vote "), ESearchCase::IgnoreCase))
	{
		Vote(Line.Mid(5));
	}
}

#if WITH_AUTOMATION_TESTS
FString AWoTPlayerController::GetChatTextForTesting() const
{
	return ChatInputWidget.IsValid() ? ChatInputWidget->GetText() : ChatInputText;
}

void AWoTPlayerController::SetChatTextForTesting(const FString& Text)
{
	ChatInputText = Text;
	if (ChatInputWidget.IsValid())
	{
		ChatInputWidget->SetText(Text);
	}
}

void AWoTPlayerController::SubmitChatForTesting()
{
	SubmitChatInput(GetChatTextForTesting());
}
#endif

void AWoTPlayerController::BroadcastSay(const FString& Message)
{
	AWoTGameMode* Game = GetWorld()
		? GetWorld()->GetAuthGameMode<AWoTGameMode>() : nullptr;
	if (!Game)
	{
		return;
	}
	const APlayerState* State = PlayerState;
	const FString PlayerName = State ? State->GetPlayerName() : FString();
	const FString Formatted = PlayerName + TEXT(": ") + Message;
	// WOTPlayer.Say calls BroadcastLeftMessage with the already-formatted line;
	// BroadcastLeftMessage therefore charges the full "PlayerName: Message" length.
	if (!Game->AllowsBroadcast(this, Formatted.Len()))
	{
		return;
	}
	for (TActorIterator<AWoTPlayerController> It(GetWorld()); It; ++It)
	{
		It->ClientLeftMessage(Formatted);
	}
}

FString AWoTPlayerController::SanitizeChatMessage(const FString& Message)
{
	// Console.TextMsgSize bounds what the retail typing line could
	// produce. Flattening line breaks keeps a client from forging a second
	// "Name: text" row beneath the server-derived prefix.
	FString Clean = Message.Left(MaxChatMessageLength);
	for (int32 Index = 0; Index < Clean.Len(); ++Index)
	{
		TCHAR& Char = Clean[Index];
		if (Char < TEXT(' ') || Char == TCHAR(0x7F) || FChar::IsLinebreak(Char))
		{
			Char = TEXT(' ');
		}
	}
	return Clean;
}

bool AWoTPlayerController::Say_Validate(const FString& Message)
{
	return Message.Len() <= MaxChatMessageLength;
}

bool AWoTPlayerController::TeamSay_Validate(const FString& Message)
{
	return Message.Len() <= MaxChatMessageLength;
}

void AWoTPlayerController::Say_Implementation(const FString& Message)
{
	BroadcastSay(SanitizeChatMessage(Message));
}

void AWoTPlayerController::TeamSay_Implementation(const FString& Message)
{
	const FString Bounded = SanitizeChatMessage(Message);
	AWoTGameMode* Game = GetWorld()
		? GetWorld()->GetAuthGameMode<AWoTGameMode>() : nullptr;
	AWoTBattleGameMode* BattleGame = Cast<AWoTBattleGameMode>(Game);
	if (!BattleGame)
	{
		// PlayerPawn.TeamSay falls back to Say whenever bTeamGame is false.
		BroadcastSay(Bounded);
		return;
	}
	// Retail reserves TeamSay "Help" for PlayerPawn.CallForHelp rather than chat.
	// The UE5 player-AI help propagation is not ported, but the reserved word still
	// must not be misrepresented as an ordinary team message.
	if (Bounded.Equals(TEXT("Help"), ESearchCase::IgnoreCase))
	{
		return;
	}
	// PlayerPawn.TeamSay charges AllowsBroadcast only Len(Msg), so an empty team
	// line would cost nothing yet reach every teammate: the port drops it.
	if (Bounded.IsEmpty())
	{
		return;
	}
	if (!Game->AllowsBroadcast(this, Bounded.Len()))
	{
		return;
	}

	const AWoTPlayerState* SenderState = GetPlayerState<AWoTPlayerState>();
	const uint8 SenderTeam = SenderState ? SenderState->Team : uint8(255);
	const FString PlayerName = SenderState
		? SenderState->GetPlayerName() : FString();
	FString TeamName = SenderTeam == 255 ? TEXT("Spectator") : TEXT("Team");
	if (IsValid(BattleGame->Battle))
	{
		TeamName = BattleGame->Battle->GetTeamDescription(SenderTeam);
	}
	const FString Formatted = FString::Printf(TEXT("[%s]%s: %s"),
		*TeamName, *PlayerName, *Bounded);
	for (TActorIterator<AWoTPlayerController> It(GetWorld()); It; ++It)
	{
		const AWoTPlayerState* RecipientState =
			It->GetPlayerState<AWoTPlayerState>();
		if (RecipientState && RecipientState->Team == SenderTeam)
		{
			It->ClientLeftMessage(Formatted);
		}
	}
}

void AWoTPlayerController::Vote(const FString& Request)
{
	ServerVote(Request);
}

void AWoTPlayerController::Admin(const FString& Command)
{
	ServerAdminCommand(Command);
}

void AWoTPlayerController::AdminLogin(const FString& Password)
{
	ServerAdminLogin(Password);
}

void AWoTPlayerController::AdminLogout()
{
	ServerAdminLogout();
}

void AWoTPlayerController::Kick(const FString& PlayerName)
{
	ServerAdminCommand(TEXT("KICK ") + PlayerName);
}

void AWoTPlayerController::KickBan(const FString& PlayerName)
{
	ServerAdminCommand(TEXT("KICKBAN ") + PlayerName);
}

bool AWoTPlayerController::ServerAdminCommand_Validate(
	const FString& Command)
{
	if (Command.IsEmpty() || Command.Len() > 256)
	{
		return false;
	}
	for (TCHAR CommandCharacter : Command)
	{
		if (CommandCharacter < TEXT(' ') || CommandCharacter == TCHAR(0x7f))
		{
			return false;
		}
	}
	return true;
}

void AWoTPlayerController::ServerAdminCommand_Implementation(
	const FString& Command)
{
	if (AWoTGameMode* Rules = GetWorld()
		? GetWorld()->GetAuthGameMode<AWoTGameMode>() : nullptr)
	{
		Rules->ExecuteAdminCommand(this, Command);
	}
}

bool AWoTPlayerController::ServerAdminLogin_Validate(
	const FString& Password)
{
	if (Password.Len() > 32)
	{
		return false;
	}
	for (TCHAR PasswordCharacter : Password)
	{
		if (PasswordCharacter < TEXT(' ')
			|| PasswordCharacter == TCHAR(0x7f))
		{
			return false;
		}
	}
	return true;
}

void AWoTPlayerController::ServerAdminLogin_Implementation(
	const FString& Password)
{
	if (AWoTGameMode* Rules = GetWorld()
		? GetWorld()->GetAuthGameMode<AWoTGameMode>() : nullptr)
	{
		Rules->AdminLogin(this, Password);
	}
}

void AWoTPlayerController::ServerAdminLogout_Implementation()
{
	if (AWoTGameMode* Rules = GetWorld()
		? GetWorld()->GetAuthGameMode<AWoTGameMode>() : nullptr)
	{
		Rules->AdminLogout(this);
	}
}

bool AWoTPlayerController::ServerVote_Validate(const FString& Request)
{
	if (Request.Len() > 256) return false;
	for (const TCHAR RequestCharacter : Request)
	{
		if (RequestCharacter < TEXT(' ') || RequestCharacter == TCHAR(0x7f)) return false;
	}
	return true;
}

void AWoTPlayerController::ServerVote_Implementation(const FString& Request)
{
	if (AWoTGameMode* Game = GetWorld()
		? GetWorld()->GetAuthGameMode<AWoTGameMode>() : nullptr)
	{
		Game->VoteRequest(this, Request);
	}
}

void AWoTPlayerController::SShot()
{
	// Engine.PlayerPawn.SShot: direct engine screenshot, without UI chrome.
	FScreenshotRequest::RequestScreenshot(/*bInShowUI*/ false,
		/*bInRestrictToGameViewport*/ true);
}

void AWoTPlayerController::ToggleFullscreen()
{
	if (GEngine && GEngine->GameViewport)
	{
		GEngine->GameViewport->HandleToggleFullscreenCommand();
	}
}

FName AWoTPlayerController::TauntSlotForIndex(int32 TauntIndex)
{
	return TauntIndex >= 1 && TauntIndex <= 6
		? FName(*FString::Printf(TEXT("Taunt%d"), TauntIndex)) : NAME_None;
}

void AWoTPlayerController::PlayTaunt(int32 TauntIndex)
{
	ServerPlayTaunt(TauntIndex);
}

void AWoTPlayerController::ServerPlayTaunt_Implementation(int32 TauntIndex)
{
	// WOTPlayer.PlayTaunt (WOTPlayer.uc:576-606); the RPC mirrors its shipped
	// client-to-server replication declaration near WOTPlayer.uc:429.
	AWoTCharacter* TauntPawn = Cast<AWoTCharacter>(GetPawn());
	if (!TauntPawn || TauntPawn->bDead || TauntPawn->Health <= 0)
	{
		return;
	}
	if (TauntIndex != -1 && TauntIndex < 1)
	{
		ClientTypedMessage(
			TEXT("Taunt number is outside the available range."), NAME_None);
		return;
	}
	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
	if (Now < NextTauntTime)
	{
		return;
	}
	const int32 ResolvedIndex = TauntIndex == -1 ? WoTRandom::RandRange(1, 6) : TauntIndex;
	const FName Slot = TauntSlotForIndex(ResolvedIndex);
	if (Slot.IsNone())
	{
		ClientTypedMessage(
			TEXT("Taunt number is outside the available range."), NAME_None);
		return;
	}
	NextTauntTime = Now + 1.0f;
	LastTauntSlotForTesting = Slot;
	TauntPawn->PlayVoiceSlot(Slot);
}

void AWoTPlayerController::Brightness()
{
	// UE1's UViewport::Brightness: +0.1, wrap, persist, message.
	if (GetNetMode() == NM_DedicatedServer || !GetGameInstance()) { return; }
	UWoTPreferencesSubsystem* Preferences =
		GetGameInstance()->GetSubsystem<UWoTPreferencesSubsystem>();
	if (!Preferences || !Preferences->HasLoadedFullPreferences()) { return; }
	FWoTGameSettings Settings = Preferences->GetGameSettings();
	Settings.Brightness =
		FWoTGameSettingsAdapter::AdvanceBrightness(Settings.Brightness);
	if (!Preferences->SaveGameSettings(Settings).IsError())
	{
		ClientTypedMessage(
			FWoTGameSettingsAdapter::BrightnessMessage(Settings.Brightness), NAME_None);
	}
}

void AWoTPlayerController::Pause()
{
	// PlayerPawn.Pause ignores the menu-open state.
	if (InGameMenu.IsOpen() || InGameMenu.IsLeaving()) { return; }
	ServerRequestPause();
}

void AWoTPlayerController::ServerRequestPause_Implementation()
{
	HandlePauseRequest(GetNetMode());
}

void AWoTPlayerController::HandlePauseRequest(ENetMode NetMode)
{
	AWoTGameMode* Rules = GetWorld() ? GetWorld()->GetAuthGameMode<AWoTGameMode>() : nullptr;
	if (!Rules || !Rules->TrySetPlayerPause(this, NetMode))
	{
		FString Message(TEXT("Pausing is unavailable."));
		if (WoTPlayerControllerBindsRetailPackage(TEXT("Engine")))
		{
			Message = WoTPlayerControllerRetailText(
				TEXT("PlayerPawn"), TEXT("NoPauseMessage"), *Message);
		}
		ClientTypedMessage(Message, NAME_None);
	}
}

void AWoTPlayerController::ShowHelp()
{
	if (IsHelpOpen())
	{
		RemoveHelpWidget();
		return;
	}
	if (InGameMenu.IsLeaving())
	{
		return;
	}
	// WOTPlayer.ShowHelp calls RemoveWindows before adding WOTHelpWindow. Keep
	// the port's mutually-exclusive window models on that same path.
	if (InGameMenu.IsOpen())
	{
		ResumeFromInGameMenu();
	}
	if (IsItemInfoOpen())
	{
		RemoveItemInfoWidget();
	}
	FWoTHelpContent Content;
	if (!FWoTHelpContent::LoadRuntime(Content))
	{
		return;
	}
	bHelpOpen = true;
	CreateHelpWidget(Content);
}

void AWoTPlayerController::CreateHelpWidget(const FWoTHelpContent& Content)
{
	if (!IsLocalController() || GIsAutomationTesting || IsRunningDedicatedServer()
		|| GetNetMode() == NM_DedicatedServer || !GEngine || !GEngine->GameViewport)
	{
		return;
	}
	HelpViewport = GEngine->GameViewport;
	HelpPresentationAuthority = FWoTFrontEndPresentationAuthority::Create(this);
	FWoTGameSettings Settings = FWoTGameSettings::Defaults();
	if (GetGameInstance())
	{
		if (const UWoTPreferencesSubsystem* Preferences =
			GetGameInstance()->GetSubsystem<UWoTPreferencesSubsystem>())
		{
			Settings = Preferences->GetGameSettings();
		}
	}
	const FWoTManagedKeyBinding* HelpKeys =
		Settings.FindBinding(EWoTManagedBinding::ShowHelp);
	FVector2D HelpViewportSize = FVector2D::ZeroVector;
	HelpViewport->GetViewportSize(HelpViewportSize);
	// The game layer applies the UI DPI curve for the viewport size (the
	// native DPI factor is extracted by SGameLayerManager).
	const float HelpDpiScale = GetDefault<UUserInterfaceSettings>()->GetDPIScaleBasedOnSize(
		FIntPoint(FMath::TruncToInt32(HelpViewportSize.X), FMath::TruncToInt32(HelpViewportSize.Y)));
	HelpWidget = SNew(SWoTHelpOverlay)
		.Content(Content)
		.InitialPhysicalSize(HelpViewportSize)
		.InitialDpiScale(HelpDpiScale)
		.PresentationAuthority(HelpPresentationAuthority)
		.PrimaryCloseKey(HelpKeys ? HelpKeys->Primary : EKeys::F1)
		.SecondaryCloseKey(HelpKeys ? HelpKeys->Secondary : FKey())
		.OnClose(FSimpleDelegate::CreateUObject(this,
			&AWoTPlayerController::ShowHelp));
	HelpViewport->AddViewportWidgetContent(HelpWidget.ToSharedRef(), 2100);
	FlushPressedKeys();
	bShowMouseCursor = false;
	FInputModeUIOnly InputMode;
	InputMode.SetWidgetToFocus(HelpWidget);
	InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	SetInputMode(InputMode);
	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().SetKeyboardFocus(
			HelpWidget, EFocusCause::SetDirectly);
	}
}

void AWoTPlayerController::RemoveHelpWidget(bool bRestoreGameInput)
{
	const bool bWasOpen = bHelpOpen;
	if (HelpWidget.IsValid())
	{
		if (UGameViewportClient* Viewport = HelpViewport.Get())
		{
			Viewport->RemoveViewportWidgetContent(HelpWidget.ToSharedRef());
		}
		HelpWidget.Reset();
	}
	HelpPresentationAuthority.Reset();
	HelpViewport.Reset();
	bHelpOpen = false;
	if (bRestoreGameInput && bWasOpen)
	{
		bShowMouseCursor = false;
		SetInputMode(FInputModeGameOnly());
	}
}

void AWoTPlayerController::ViewUp()
{
	if (CitadelEditCameraMode == EWoTCitadelEditCameraMode::Orbit)
	{
		NextView();
		return;
	}
	if (AWoTHUD* WoTHUD = Cast<AWoTHUD>(GetHUD()))
	{
		WoTHUD->SetInterfaceLevel(
			AWoTHUD::NextInterfaceLevel(WoTHUD->GetInterfaceLevel()));
	}
}

void AWoTPlayerController::ViewDown()
{
	if (CitadelEditCameraMode == EWoTCitadelEditCameraMode::Orbit)
	{
		PrevView();
		return;
	}
	if (AWoTHUD* WoTHUD = Cast<AWoTHUD>(GetHUD()))
	{
		WoTHUD->SetInterfaceLevel(
			AWoTHUD::PreviousInterfaceLevel(WoTHUD->GetInterfaceLevel()));
	}
}

void AWoTPlayerController::NextView()
{
	if (!IsLocalController()
		|| CitadelEditCameraMode != EWoTCitadelEditCameraMode::Orbit)
	{
		return;
	}
	const TArray<AWoTZoneVolume*> Zones = CitadelVisibleOrbitZones(this);
	if (Zones.IsEmpty()) return;
	int32 Current = Zones.IndexOfByKey(CitadelEditZone.Get());
	Current = Current == INDEX_NONE ? 0 : (Current + 1) % Zones.Num();
	CitadelEditZone = Zones[Current];
	CitadelOrbitViewOffset = FRotator::ZeroRotator;
	ApplyCitadelEditCameraState();
	ClientPlayUISound(FName(TEXT("WOT/Editor/EditSelect")), 1.0f);
}

void AWoTPlayerController::PrevView()
{
	if (!IsLocalController()
		|| CitadelEditCameraMode != EWoTCitadelEditCameraMode::Orbit)
	{
		return;
	}
	const TArray<AWoTZoneVolume*> Zones = CitadelVisibleOrbitZones(this);
	if (Zones.IsEmpty()) return;
	int32 Current = Zones.IndexOfByKey(CitadelEditZone.Get());
	Current = Current == INDEX_NONE ? Zones.Num() - 1
		: (Current + Zones.Num() - 1) % Zones.Num();
	CitadelEditZone = Zones[Current];
	CitadelOrbitViewOffset = FRotator::ZeroRotator;
	ApplyCitadelEditCameraState();
	ClientPlayUISound(FName(TEXT("WOT/Editor/EditSelect")), 1.0f);
}

void AWoTPlayerController::SwitchView(int32 Index)
{
	if (!IsLocalController() || Index < 0
		|| CitadelEditCameraMode != EWoTCitadelEditCameraMode::Orbit)
	{
		return;
	}
	const TArray<AWoTZoneVolume*> Zones = CitadelVisibleOrbitZones(this);
	if (!Zones.IsValidIndex(Index)) return;
	CitadelEditZone = Zones[Index];
	CitadelOrbitViewOffset = FRotator::ZeroRotator;
	ApplyCitadelEditCameraState();
	ClientPlayUISound(FName(TEXT("WOT/Editor/EditSelect")), 1.0f);
}

AWoTInventory* AWoTPlayerController::GetSelectedItemForInfo() const
{
	const AWoTCharacter* WoTCharacter = Cast<AWoTCharacter>(GetPawn());
	const UWoTHandInfo* Hand = WoTCharacter && WoTCharacter->CurrentHandSet
		? WoTCharacter->CurrentHandSet->GetSelectedHand() : nullptr;
	return Hand ? Hand->GetSelectedItem() : nullptr;
}

void AWoTPlayerController::ShowInventoryInfo()
{
	// WOTPlayer.ShowInventoryInfo always removes the current window first. If
	// item info itself was active, that makes F2 a close-only toggle.
	if (IsItemInfoOpen())
	{
		RemoveItemInfoWidget();
		return;
	}
	if (InGameMenu.IsOpen() || InGameMenu.IsLeaving())
	{
		return;
	}
	AWoTInventory* SelectedItem = GetSelectedItemForInfo();
	if (!IsValid(SelectedItem))
	{
		return;
	}
	bItemInfoOpen = true;
	ItemInfoItem = SelectedItem;
	CreateItemInfoWidget(SelectedItem);
}

void AWoTPlayerController::CreateItemInfoWidget(AWoTInventory* Item)
{
	if (!IsValid(Item) || !IsLocalController() || GIsAutomationTesting ||
		IsRunningDedicatedServer() || GetNetMode() == NM_DedicatedServer ||
		!GEngine || !GEngine->GameViewport)
	{
		return;
	}
	ItemInfoViewport = GEngine->GameViewport;
	ItemInfoWidget = SNew(SWoTItemInfoOverlay)
		.PlayerController(this)
		.Item(Item);
	ItemInfoViewport->AddViewportWidgetContent(
		ItemInfoWidget.ToSharedRef(), /*ZOrder*/ 1900);
}

void AWoTPlayerController::RemoveItemInfoWidget(bool bCloseWindow)
{
	if (ItemInfoWidget.IsValid())
	{
		if (UGameViewportClient* Viewport = ItemInfoViewport.Get())
		{
			Viewport->RemoveViewportWidgetContent(ItemInfoWidget.ToSharedRef());
		}
		ItemInfoWidget.Reset();
	}
	ItemInfoViewport.Reset();
	ItemInfoItem.Reset();
	if (bCloseWindow)
	{
		bItemInfoOpen = false;
	}
}

void AWoTPlayerController::ClearProgressMessages()
{
	for (int32 Index = 0; Index < NumProgressMessages; ++Index)
	{
		ProgressMessage[Index].Reset();
		ProgressColor[Index] = FLinearColor::White;
	}
	// PlayerPawn.ClearProgressMessages leaves ProgressTimeOut untouched.
}

void AWoTPlayerController::SetProgressMessage(const FString& Message, int32 Index)
{
	if (Index >= 0 && Index < NumProgressMessages)
	{
		ProgressMessage[Index] = Message;
	}
}

void AWoTPlayerController::SetProgressColor(
	const FLinearColor& Color, int32 Index)
{
	if (Index >= 0 && Index < NumProgressMessages)
	{
		ProgressColor[Index] = Color;
	}
}

void AWoTPlayerController::SetProgressTime(float Duration)
{
	ProgressTimeOut = (GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f)
		+ Duration;
}

void AWoTPlayerController::ClientClearProgressMessages_Implementation()
{
	ClearProgressMessages();
}

void AWoTPlayerController::ClientSetProgressMessage_Implementation(
	const FString& Message, int32 Index)
{
	SetProgressMessage(Message, Index);
}

void AWoTPlayerController::ClientSetProgressColor_Implementation(
	FLinearColor Color, int32 Index)
{
	SetProgressColor(Color, Index);
}

void AWoTPlayerController::ClientSetProgressTime_Implementation(float Duration)
{
	SetProgressTime(Duration);
}

FString AWoTPlayerController::GetProgressMessage(int32 Index) const
{
	return Index >= 0 && Index < NumProgressMessages
		? ProgressMessage[Index] : FString();
}

FLinearColor AWoTPlayerController::GetProgressColor(int32 Index) const
{
	return Index >= 0 && Index < NumProgressMessages
		? ProgressColor[Index] : FLinearColor::White;
}

bool AWoTPlayerController::AreProgressMessagesActive(float TimeSeconds) const
{
	return ProgressTimeOut > TimeSeconds;
}

TArray<FWoTProgressEntry> AWoTPlayerController::GetProgressEntries() const
{
	TArray<FWoTProgressEntry> Entries;
	Entries.Reserve(NumProgressMessages);
	for (int32 Index = 0; Index < NumProgressMessages; ++Index)
	{
		FWoTProgressEntry& Entry = Entries.AddDefaulted_GetRef();
		Entry.Message = ProgressMessage[Index];
		Entry.Color = ProgressColor[Index];
	}
	return Entries;
}

void AWoTPlayerController::PopulateNetworkFailureProgress(
	const FString& FailureType, const FString& ErrorString)
{
	ClearProgressMessages();
	SetProgressMessage(TEXT("Network link could not be established"), 0);
	SetProgressMessage(ErrorString.IsEmpty() ? FailureType : ErrorString, 1);
	SetProgressTime(6.0f);
}

FString AWoTPlayerController::GetTravelDestinationDisplay(
	const FString& PendingURL)
{
	// Retail formats only FURL.Host and FURL.Map. Never
	// project Op/Portal back into the HUD: travel options may contain passwords.
	const FURL ParsedURL(nullptr, *PendingURL, TRAVEL_Absolute);
	if (!ParsedURL.Valid)
	{
		return TEXT("Unknown destination");
	}

	// A guarded server travel names the neutral shell, never the retail map:
	// show the adopted identity the player chose (retail prints FURL.Map).
	FString Map = WoTDisplayMapForTravelURLMap(ParsedURL.Map);
	Map.RemoveFromStart(TEXT("/"));
	if (ParsedURL.Host.IsEmpty())
	{
		return Map;
	}
	return Map.IsEmpty()
		? ParsedURL.Host
		: FString::Printf(TEXT("%s/%s"), *ParsedURL.Host, *Map);
}

void AWoTPlayerController::ClearTravelProgressOnArrival()
{
	ClearProgressMessages();
	// ClearProgressMessages intentionally preserves the UE1 timeout. Arrival is
	// a lifecycle boundary, so explicitly make the stale travel window inactive.
	ProgressTimeOut = 0.0f;
}

void AWoTPlayerController::PreClientTravel(const FString& PendingURL,
	ETravelType TravelType, bool bIsSeamlessTravel)
{
	// FInputModeUIOnly leaves the shared game viewport ignoring input, and no
	// later controller re-applies game input on a game-to-game travel.
	RemoveChatInputWidget(/*bRestoreGameInput*/ true);
	RemoveHelpWidget(/*bRestoreGameInput*/ true);
	RemoveItemInfoWidget();
	if (IsLocalController())
	{
		ClearProgressMessages();
		SetProgressMessage(TEXT("Connecting"), 0);
		SetProgressMessage(GetTravelDestinationDisplay(PendingURL), 1);
		SetProgressTime(60.0f);
	}
	Super::PreClientTravel(PendingURL, TravelType, bIsSeamlessTravel);
}

void AWoTPlayerController::PostSeamlessTravel()
{
	Super::PostSeamlessTravel();
	if (IsLocalController())
	{
		ClearTravelProgressOnArrival();
	}
}

void AWoTPlayerController::NotifyLoadedWorld(
	FName WorldPackageName, bool bFinalDest)
{
	Super::NotifyLoadedWorld(WorldPackageName, bFinalDest);
	if (bFinalDest && IsLocalController())
	{
		ClearTravelProgressOnArrival();
	}
}

void AWoTPlayerController::EndPlay(const EEndPlayReason::Type Reason)
{
	UnbindProgressFailureDelegates();
	// FInputModeUIOnly leaves the shared game viewport ignoring input, and no
	// later controller re-applies game input on a game-to-game travel.
	RemoveChatInputWidget(/*bRestoreGameInput*/ true);
	RemoveHelpWidget(/*bRestoreGameInput*/ true);
	RemoveItemInfoWidget();
	RemoveInGameMenuWidget(/*bRestoreGameInput*/ true);
	InGameMenu.Reset();
	ClearCitadelSelectionPresentation();
	EndEditSession();
	ClearLocalCitadelEditStateForTeardown();
	Super::EndPlay(Reason);
}

void AWoTPlayerController::OnPossess(APawn* InPawn)
{
	if (GamepadLayer)
	{
		GamepadLayer->OnPawnChanged();
	}
	if (IsLocalController() && GetWorld() && !GetWorld()->IsEditorWorld())
	{
		const FVector Where = InPawn ? InPawn->GetActorLocation() : FVector::ZeroVector;
		WoTRecordBootstrapHeartbeat(TEXT("player-possess"),
			EWoTBootstrapPhase::Ready, 0, {}, {},
			FString::Printf(TEXT("pawn=%s at=(%.0f,%.0f,%.0f) world=%s map=%s net=%d"),
				InPawn ? *InPawn->GetClass()->GetName() : TEXT("none"),
				Where.X, Where.Y, Where.Z,
				*GetWorld()->GetOutermost()->GetName(),
				*WoTWorldMapIdentity(GetWorld()),
				static_cast<int32>(GetWorld()->GetNetMode())));
		TWeakObjectPtr<AWoTPlayerController> WeakThis(this);
		FTimerHandle Snapshot;
		GetWorld()->GetTimerManager().SetTimer(Snapshot,
			FTimerDelegate::CreateLambda([WeakThis]()
			{
				AWoTPlayerController* Self = WeakThis.Get();
				if (!Self || !Self->GetWorld()) return;
				UWorld* W = Self->GetWorld();
				int32 VisibleLights = 0;
				int32 Actors = 0;
				for (TActorIterator<AActor> It(W); It; ++It)
				{
					++Actors;
					if (ULightComponent* Light = It->FindComponentByClass<ULightComponent>())
						if (Light->IsVisible() && Light->Intensity > 0.0f) ++VisibleLights;
				}
				const APawn* P = Self->GetPawn();
				const AActor* View = Self->GetViewTarget();
				const FVector Cam = Self->PlayerCameraManager
					? Self->PlayerCameraManager->GetCameraLocation() : FVector::ZeroVector;
				const FRotator CamRot = Self->PlayerCameraManager
					? Self->PlayerCameraManager->GetCameraRotation() : FRotator::ZeroRotator;
				FWoTVertexMeshDiagnostics MeshState;
				if (const UWoTVertexAnimComponent* Body = P
					? P->FindComponentByClass<UWoTVertexAnimComponent>() : nullptr)
				{
					MeshState = Body->GetMeshDiagnostics();
				}
				WoTRecordBootstrapHeartbeat(TEXT("player-view"),
					EWoTBootstrapPhase::Ready, 0, {}, {},
					FString::Printf(TEXT("pawn=%s pawn_at=(%.0f,%.0f,%.0f) view=%s cam=(%.0f,%.0f,%.0f) rot=(%.0f,%.0f) actors=%d visible_lights=%d hud=%s viewport=%s mesh_id=%s proxy=%s sections=%d materials=%d bounds=(%.1f,%.1f,%.1f) submitted_last_frame=%d"),
						P ? *P->GetClass()->GetName() : TEXT("none"),
						P ? P->GetActorLocation().X : 0.0, P ? P->GetActorLocation().Y : 0.0, P ? P->GetActorLocation().Z : 0.0,
						View ? *View->GetClass()->GetName() : TEXT("none"),
						Cam.X, Cam.Y, Cam.Z, CamRot.Pitch, CamRot.Yaw, Actors, VisibleLights,
						Self->GetHUD() ? *Self->GetHUD()->GetClass()->GetName() : TEXT("none"),
						(GEngine && GEngine->GameViewport) ? TEXT("yes") : TEXT("no"),
						MeshState.BoundMeshId.IsEmpty() ? TEXT("none") : *MeshState.BoundMeshId,
						*MeshState.ProxyKind.ToString(), MeshState.SectionCount,
						MeshState.MaterialCount, MeshState.BoundsExtent.X,
						MeshState.BoundsExtent.Y, MeshState.BoundsExtent.Z,
						MeshState.bSubmittedDrawLastFrame ? 1 : 0));
			}), 5.0f, false);
	}
	if (HasAuthority() && IsEditingCitadel() && GetPawn() != InPawn)
	{
		EndEditSession();
	}
	Super::OnPossess(InPawn);
	if (HasAuthority() && IsValid(InPawn) && !bMatchEnded)
	{
		EnterPlayerState(EWoTPlayerStateMachineState::Playing);
	}
}

void AWoTPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();
	if (IsLocalController() && PlayerInput && GetGameInstance())
	{
		if (UWoTPreferencesSubsystem* Preferences =
			GetGameInstance()->GetSubsystem<UWoTPreferencesSubsystem>())
		{
			if (Preferences->HasLoadedFullPreferences())
			{
				Preferences->ApplyGameSettingsToInput(*PlayerInput);
			}
		}
	}
	if (InputComponent)
	{
		InputComponent->BindAction(TEXT("Fire"), IE_Pressed,
			this, &AWoTPlayerController::Fire);
		// The release must live beside the press: UPlayerInput consumes a bound
		// action's key on BOTH edges for everything below this component in the
		// input stack, so a pawn-owned release would never fire (stuck weave).
		InputComponent->BindAction(TEXT("Fire"), IE_Released,
			this, &AWoTPlayerController::CeaseFire).bExecuteWhenPaused = true;
		// Jump is controller-owned like Fire (see the header comment): dead players
		// respawn on the jump key too, and only a controller binding survives the
		// pawnless Dying state without starving the live pawn's jump.
		InputComponent->BindAction(TEXT("Jump"), IE_Pressed,
			this, &AWoTPlayerController::Jump).bExecuteWhenPaused = true;
		InputComponent->BindAction(TEXT("Jump"), IE_Released,
			this, &AWoTPlayerController::StopJumping).bExecuteWhenPaused = true;
		InputComponent->BindAction(TEXT("ShowScores"), IE_Pressed,
			this, &AWoTPlayerController::ShowScores);
		InputComponent->BindAction(TEXT("InGameMenu"), IE_Pressed,
			this, &AWoTPlayerController::ToggleInGameMenu).bExecuteWhenPaused = true;
		InputComponent->BindAction(TEXT("Type"), IE_Pressed,
			this, &AWoTPlayerController::Type).bExecuteWhenPaused = true;
		InputComponent->BindAction(TEXT("Talk"), IE_Pressed,
			this, &AWoTPlayerController::Talk).bExecuteWhenPaused = true;
		InputComponent->BindAction(TEXT("TeamTalk"), IE_Pressed,
			this, &AWoTPlayerController::TeamTalk).bExecuteWhenPaused = true;
		InputComponent->BindAction(TEXT("ViewUp"), IE_Pressed,
			this, &AWoTPlayerController::ViewUp);
		InputComponent->BindAction(TEXT("ViewDown"), IE_Pressed,
			this, &AWoTPlayerController::ViewDown);
		InputComponent->BindAction(TEXT("ShowInventoryInfo"), IE_Pressed,
			this, &AWoTPlayerController::ShowInventoryInfo);
		InputComponent->BindAction(TEXT("SShot"), IE_Pressed,
			this, &AWoTPlayerController::SShot);
		InputComponent->BindAction(TEXT("ToggleFullscreen"), IE_Pressed,
			this, &AWoTPlayerController::ToggleFullscreen);
		InputComponent->BindAction(TEXT("ServerEditCitadel"), IE_Pressed,
			this, &AWoTPlayerController::ServerEditCitadelInput);
		InputComponent->BindAction(TEXT("ShowHelp"), IE_Pressed,
			this, &AWoTPlayerController::ShowHelp);
		for (int32 TauntIndex = 1; TauntIndex <= 6; ++TauntIndex)
		{
			FInputActionBinding Binding(FName(*FString::Printf(
				TEXT("PlayTaunt%d"), TauntIndex)), IE_Pressed);
			Binding.ActionDelegate.GetDelegateForManualSet().BindWeakLambda(
				this, [this, TauntIndex]() { PlayTaunt(TauntIndex); });
			InputComponent->AddActionBinding(MoveTemp(Binding));
		}
		{
			FInputActionBinding Binding(FName(TEXT("PlayTauntRandom")), IE_Pressed);
			Binding.ActionDelegate.GetDelegateForManualSet().BindWeakLambda(
				this, [this]() { PlayTaunt(-1); });
			InputComponent->AddActionBinding(MoveTemp(Binding));
		}
		InputComponent->BindAction(TEXT("Brightness"), IE_Pressed,
			this, &AWoTPlayerController::Brightness);
		InputComponent->BindAction(TEXT("Pause"), IE_Pressed,
			this, &AWoTPlayerController::Pause).bExecuteWhenPaused = true;
	}
	if (IsLocalController())
	{
		if (UEnhancedInputComponent* Enhanced = Cast<UEnhancedInputComponent>(InputComponent))
		{
			if (GamepadLayer)
			{
				GamepadLayer->Shutdown();
			}
			GamepadLayer = NewObject<UWoTGamepadLayer>(this);
			GamepadLayer->Initialize(*this, *Enhanced);
		}
	}
}

void AWoTPlayerController::OnUnPossess()
{
	if (GamepadLayer)
	{
		GamepadLayer->OnPawnChanged();
	}
	ReconcileOwnerLocalZoneFOVPawn(nullptr);
	RemoveHelpWidget();
	RemoveItemInfoWidget();
	Super::OnUnPossess();
}

void AWoTPlayerController::SetPawn(APawn* InPawn)
{
	// ClientRestart replaces the autonomous client's pawn through SetPawn rather
	// than OnPossess. Close before the new selection can become the live model.
	if (GetPawn() != InPawn)
	{
		RemoveItemInfoWidget();
		ResetRetailCameraState(InPawn);
	}
	Super::SetPawn(InPawn);
	ReconcileOwnerLocalZoneFOVPawn(InPawn);
}

void AWoTPlayerController::OnRep_Pawn()
{
	// Replication assigns AController::Pawn before its rep-notify, so SetPawn's
	// old/new comparison cannot detect this replacement path by itself.
	ReconcileOwnerLocalZoneFOVPawn(nullptr);
	RemoveItemInfoWidget();
	Super::OnRep_Pawn();
	ResetRetailCameraState(GetPawn());
	ReconcileOwnerLocalZoneFOVPawn(GetPawn());
}

void AWoTPlayerController::ReconcileOwnerLocalZoneFOVPawn(APawn* NewPawn)
{
	AWoTCharacter* NewCharacter = Cast<AWoTCharacter>(NewPawn);
	AWoTCharacter* OldCharacter = OwnerLocalZoneFOVPawn.Get();
	if (OldCharacter != NewCharacter && IsValid(OldCharacter))
	{
		OldCharacter->TeardownOwnerLocalZoneFOV();
		// Controller.Pawn can replicate before the old Pawn.Controller clears.
		// Restore through the controller as well so that ordering cannot strand a
		// randomized FOV on the owning client.
		if (IsLocalController()) ClientRestoreCameraFOV();
	}
	OwnerLocalZoneFOVPawn = NewCharacter;
	if (IsValid(NewCharacter)) NewCharacter->ReconcileOwnerLocalZoneFOV();
}

void AWoTPlayerController::ResetRetailCameraState(APawn* NewPawn)
{
	UE1CameraPawn = NewPawn;
	UE1WalkBob = FVector::ZeroVector;
	UE1BobTime = 0.0f;
	UE1LandBob = 0.0f;
	UE1AppliedBob = 0.0f;
	UE1PreviousMovementMode = MOVE_None;
	UE1PreviousPawnLocation = NewPawn
		? NewPawn->GetActorLocation() : FVector::ZeroVector;
	if (const AWoTCharacter* PawnCharacter = Cast<AWoTCharacter>(NewPawn))
	{
		UE1EyeHeight = PawnCharacter->BaseEyeHeight;
	}
	else
	{
		UE1EyeHeight = 42.0f;
	}
}

void AWoTPlayerController::Destroyed()
{
	UnbindProgressFailureDelegates();
	// FInputModeUIOnly leaves the shared game viewport ignoring input, and no
	// later controller re-applies game input on a game-to-game travel.
	RemoveChatInputWidget(/*bRestoreGameInput*/ true);
	RemoveHelpWidget(/*bRestoreGameInput*/ true);
	RemoveItemInfoWidget();
	RemoveInGameMenuWidget(/*bRestoreGameInput*/ true);
	InGameMenu.Reset();
	ClearCitadelSelectionPresentation();
	EndEditSession();
	ClearLocalCitadelEditStateForTeardown();
	Super::Destroyed();
}

void AWoTPlayerController::PlayerTick(float DeltaTime)
{
	if (GamepadLayer)
	{
		GamepadLayer->Tick(DeltaTime);
	}
	Super::PlayerTick(DeltaTime);
	TickItemInfoLifecycle();
	if (IsLocalController() && bForcedViewYaw)
	{
		FRotator Forced = GetControlRotation();
		Forced.Yaw += ForcedViewYawRateUnits * 360.0f / 65536.0f * DeltaTime;
		SetControlRotation(Forced);
		++ForcedViewApplyCountForTest;
	}
	if (IsLocalController())
	{
		TickUE1ViewShake(DeltaTime);
		TickUE1ViewFlash(DeltaTime);
		TickRetailCamera(DeltaTime);
	}
	TickPlayerState(DeltaTime);
	TickCitadelEditPresentation();
}

void AWoTPlayerController::TickItemInfoLifecycle()
{
	// WOTPlayer.ClientSelectItem calls UpdateInventoryInfo while this window
	// is active. Poll the same local view-model edge so hand/item selection
	// replaces the live content without coupling the model back to Slate.
	// (Separate from PlayerTick so specs can drive the seam on controllers
	// without an input stack — Super::PlayerTick asserts on null PlayerInput.)
	if (!bItemInfoOpen)
	{
		return;
	}
	AWoTInventory* SelectedItem = GetSelectedItemForInfo();
	if (!IsValid(SelectedItem))
	{
		RemoveItemInfoWidget();
	}
	else if (SelectedItem != ItemInfoItem.Get())
	{
		RemoveItemInfoWidget(/*bCloseWindow*/ false);
		ItemInfoItem = SelectedItem;
		CreateItemInfoWidget(SelectedItem);
	}
}

void AWoTPlayerController::TickCitadelEditPresentation()
{
	// Defensive phase reconciliation: no edit grant may survive a forced phase mutation
	// that bypasses AWoTBattleGameMode::EnableTeleporters.
	if (HasAuthority() && IsEditingCitadel())
	{
		AWoTBattleGameMode* BattleMode = GetWorld()
			? GetWorld()->GetAuthGameMode<AWoTBattleGameMode>() : nullptr;
		if (IsValid(BattleMode) && BattleMode->CanTeleport())
		{
			EndEditSession();
		}
	}
	if (!IsLocalController())
	{
		return;
	}
	ReconcileCitadelGhostPreview();
	if (AActor* GhostActor = CitadelGhostPreviewActor.Get())
	{
		if (AWoTTrap* Ghost = Cast<AWoTTrap>(GhostActor))
		{
			Ghost->SetPreviewVisible(false);
		}
		else
		{
			GhostActor->SetActorHiddenInGame(true);
		}
		FWoTCitadelEditorTraceResult PreviewHit;
		EditorTrace(PreviewHit, GhostActor);
		SyncCitadelGhostPreview(PreviewHit);
	}
	if (IsOwnerEditingCitadel())
	{
		// EditorHUD.SyncTempResourceWithCursor cadence: keep the toggle resources'
		// cursor traceability tracking the live viewer zone and active state.
		RefreshCitadelSelectTraceability();
	}
	if (SelectedCitadelAdapter.IsNone())
	{
		return;
	}
	const bool bZoneValid = HasValidLocalCitadelPresentationZone();
	if (!bZoneValid)
	{
		if (bCitadelPresentationZoneValid || RevealedCitadelResources.Num() > 0)
		{
			ClearCitadelPresentationActors();
		}
		bCitadelPresentationZoneValid = false;
		return;
	}
	bCitadelPresentationZoneValid = true;
	// Re-derive marker state from replicated actor state and pick up newly relevant actors.
	RefreshCitadelSelectionPresentation();
}

void AWoTPlayerController::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	// Fan the ended flag out to the owning client so it can gate restart input / scoreboard.
	DOREPLIFETIME(AWoTPlayerController, bMatchEnded);
	DOREPLIFETIME_CONDITION(AWoTPlayerController, PlayerStateMachineState, COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(AWoTPlayerController, JoinTeamTimeout, COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(AWoTPlayerController, bTeleportingDisabled, COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(AWoTPlayerController, bShowOverview, COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(AWoTPlayerController, OwnerEditSessionTeam,
		CitadelOwnerReplicationCondition());
	DOREPLIFETIME_CONDITION(AWoTPlayerController, OwnerEditBudget,
		CitadelOwnerReplicationCondition());
	DOREPLIFETIME_CONDITION(AWoTPlayerController, OwnerSealCardCount,
		COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(AWoTPlayerController, CitadelEditCameraMode,
		CitadelOwnerReplicationCondition());
	DOREPLIFETIME_CONDITION(AWoTPlayerController, CitadelEditZone,
		CitadelOwnerReplicationCondition());
	DOREPLIFETIME_CONDITION(AWoTPlayerController, bProhibitEditorExit,
		CitadelOwnerReplicationCondition());
}

bool AWoTPlayerController::ServerAssessBattlefield_Validate() { return true; }

void AWoTPlayerController::ServerAssessBattlefield_Implementation()
{
	// WOTPlayer.ServerAssessBattlefield: a pre-battle fire asks the authority to open the
	// battlefield. AssessBattlefield re-checks the budgets/editing state itself; the
	// original's Waiting* center messages ride the same call.
	AWoTBattleGameMode* BattleMode = GetWorld()
		? GetWorld()->GetAuthGameMode<AWoTBattleGameMode>() : nullptr;
	if (!IsValid(BattleMode) || BattleMode->CanTeleport())
	{
		return;
	}
	const uint8 Team = GetPlayerState<AWoTPlayerState>()
		? GetPlayerState<AWoTPlayerState>()->Team : 255;
	if (Team == 255)
	{
		return;
	}
	BattleMode->AssessBattlefield();
	if (!BattleMode->CanTeleport())
	{
		// Still waiting — tell the firing player why, in the original's order: unspent
		// seal budgets first, then an editor still active (WOTPlayer.uc:4370-4400).
		bool bSealsOutstanding = false;
		for (TActorIterator<AWoTBudgetInfo> It(GetWorld()); It; ++It)
		{
			if (IsValid(*It) && It->SealBudgetRemaining() > 0)
			{
				bSealsOutstanding = true;
				break;
			}
		}
		ClientCenterMessage(bSealsOutstanding
			? BattleMode->WaitingForSealsStr : BattleMode->WaitingForEditStr);
	}
}

void AWoTPlayerController::ClientCenterMessage_Implementation(const FString& Text,
                                                              bool bEcho)
{
	if (AWoTHUD* HUD = Cast<AWoTHUD>(GetHUD()))
	{
		HUD->AddCenterMessage(Text);
	}
	if (bEcho)
	{
		// BaseHUD.uc:933-936: bEcho copies the message to the plain console
		// queue ('Message' falls through RouteTypedMessage's default leg).
		RouteTypedMessage(Text, FName(TEXT("Message")),
			GetNetMode() == NM_Standalone);
	}
}

void AWoTPlayerController::ClientLeftMessage_Implementation(const FString& Text)
{
	// WOTPlayer.LeftMessage (WOTPlayer.uc:1787-1794): the owning client's left queue.
	if (AWoTHUD* HUD = Cast<AWoTHUD>(GetHUD()))
	{
		HUD->AddLeftMessage(Text);
	}
}

void AWoTPlayerController::ClientHandMessage_Implementation(
	const FString& Message, float Duration)
{
	if (AWoTHUD* HUD = Cast<AWoTHUD>(GetHUD()))
	{
		HUD->AddHandMessage(Message, Duration);
	}
}

void AWoTPlayerController::ClientTypedMessage_Implementation(const FString& S, FName Type)
{
	RouteTypedMessage(S, Type, GetNetMode() == NM_Standalone);
}

void AWoTPlayerController::RouteTypedMessage(const FString& S, FName Type, bool bStandalone)
{
	// WOTPlayer.ClientMessage (WOTPlayer.uc:1862-1890): 'Pickup' and 'FailedMessage' land
	// in the hand-message slot, 'WOTDeathMessage' in the right queue when the level is not
	// standalone, 'DeathMessage' is deprecated (swallowed), and everything else falls
	// through to the plain console message. Empty strings are dropped up front.
	if (S.IsEmpty())
	{
		return;
	}
	if (Type == FName(TEXT("Pickup")) || Type == FName(TEXT("FailedMessage")))
	{
		if (AWoTHUD* HUD = Cast<AWoTHUD>(GetHUD()))
		{
			HUD->AddHandMessage(S);
		}
	}
	else if (Type == FName(TEXT("WOTDeathMessage")))
	{
		if (!bStandalone)
		{
			if (AWoTHUD* HUD = Cast<AWoTHUD>(GetHUD()))
			{
				HUD->AddRightMessage(S);
			}
			// Onboarding seam, behavior-neutral: the death-recap aid reads
			// the same line the right queue just received.
			FWoTOnboardingSeams::Get().ClientDeathMessage.Broadcast(S);
		}
	}
	else if (Type == FName(TEXT("DeathMessage")))
	{
		// Deprecated in the shipped script — dropped.
	}
	else
	{
		ClientMessage(S);
	}
}

void AWoTPlayerController::ClientNotifyNewPickup_Implementation(
	FName FlashTextureId, float FadeTime, FName SoundSubPath)
{
	// AngrealInventory.uc:615-616 GiveTo: owner-local notify sound + the HUD flash.
	if (AWoTHUD* HUD = Cast<AWoTHUD>(GetHUD()))
	{
		HUD->SetFlashTexture(FlashTextureId, FadeTime);
	}
	ClientPlayUISound(SoundSubPath, 1.0f);
}

void AWoTPlayerController::ClientPlayUISound_Implementation(FName SoundSubPath, float Volume)
{
	// PlayerPawn.ClientPlaySound overlaps None/Interface/Misc/Talk at volume 16.
	// Galaxy clamps each voice to full scale; reproduce that effective four-voice
	// stack with UE5-normalized gain while keeping UI outside EffectsChannels.
	// Retail ClientPlaySound reaches only that player's own machine. A process
	// hosts extra local players only under the test harness (rig puppets); their
	// private cues (a seeker locking on, a seal changing hands) must not reach the
	// one recorded view, so only the primary local player's are audible.
	if (SoundSubPath.IsNone() || !IsPrimaryPlayer())
	{
		return;
	}
	if (USoundBase* S = WoTSound::LoadBySubPath(SoundSubPath))
	{
		const float VoiceGain = WoTSound::NormalizedUISoundVoiceGain(Volume);
		for (int32 Voice = 0; Voice < WoTSound::ClientUISoundVoiceCount; ++Voice)
		{
			UGameplayStatics::PlaySound2D(this, S, VoiceGain);
		}
	}
}

void AWoTPlayerController::ClientAdjustGlow_Implementation(float Scale, FVector Fog)
{
	// Engine PlayerPawn.ClientAdjustGlow: ConstantGlowScale += scale;
	// ConstantGlowFog += .001*fog. UE5's camera color scale is the nearest
	// presentation seat for the same additive RGB view treatment.
	ConstantGlowScale += Scale;
	ConstantGlowFog += Fog * 0.001f;
	if (PlayerCameraManager)
	{
		const FVector Desired = FVector(1.0f + ConstantGlowScale) + ConstantGlowFog;
		PlayerCameraManager->SetDesiredColorScale(FVector(
			FMath::Max(Desired.X, 0.0f), FMath::Max(Desired.Y, 0.0f),
			FMath::Max(Desired.Z, 0.0f)), 0.0f);
	}
}

void AWoTPlayerController::ClientSetCameraFOV_Implementation(float NewFOV)
{
	if (!bHasSavedCameraFOV)
	{
		SavedCameraFOV = PlayerCameraManager
			? PlayerCameraManager->GetFOVAngle() : CameraFOVForTest;
		bHasSavedCameraFOV = true;
	}
	CameraFOVForTest = NewFOV;
	if (PlayerCameraManager)
	{
		PlayerCameraManager->SetFOV(NewFOV);
	}
}

void AWoTPlayerController::ClientRestoreCameraFOV_Implementation()
{
	if (!bHasSavedCameraFOV)
	{
		return;
	}
	CameraFOVForTest = SavedCameraFOV;
	if (PlayerCameraManager)
	{
		PlayerCameraManager->SetFOV(SavedCameraFOV);
	}
	bHasSavedCameraFOV = false;
}

float AWoTPlayerController::GetCameraFOVForTest() const
{
	return bHasSavedCameraFOV ? CameraFOVForTest
		: (PlayerCameraManager ? PlayerCameraManager->GetFOVAngle()
			: CameraFOVForTest);
}

void AWoTPlayerController::ClientSetForcedViewYaw_Implementation(
	bool bEnable, float YawRateUnitsPerSecond)
{
	bForcedViewYaw = bEnable;
	ForcedViewYawRateUnits = bEnable ? YawRateUnitsPerSecond : 0.0f;
}

void AWoTPlayerController::ClientShakeView_Implementation(
	float ShakeTime, float RollMag, float VertMag)
{
	++ShakeRequestCountForTest;
	// PlayerPawn.ClientShake/ShakeView: replace only when the
	// requested roll is stronger or the existing shake is shorter.
	if (UE1ShakeMag < RollMag || UE1ShakeTimer <= ShakeTime)
	{
		UE1ShakeMag = RollMag;
		UE1ShakeTimer = ShakeTime;
		UE1MaxShakeVert = VertMag;
		UE1VertTimer = 0.0f;
		UE1ShakeVert = -1.1f * UE1MaxShakeVert;
	}
}

bool AWoTPlayerController::InputKey(const FInputKeyEventArgs& Params)
{
	if (Params.IsGamepad() && !IsJoystickInputEnabled())
	{
		return true;
	}
	// Retail Console state Typing consumes press-like input only. Slate owns
	// text editing; release and axis events continue through the gameplay route.
	if (bChatTyping && Params.Event != IE_Released && Params.Event != IE_Axis)
	{
		return true;
	}
	return Super::InputKey(Params);
}

FVector2D AWoTPlayerController::GetLegacyLookInputScale() const
{
	if (!GetDefault<UInputSettings>()->bEnableLegacyInputScales)
	{
		return FVector2D(1.0, 1.0);
	}
	return FVector2D(InputYawScale_DEPRECATED, InputPitchScale_DEPRECATED);
}

bool AWoTPlayerController::IsJoystickInputEnabled() const
{
#if WITH_AUTOMATION_TESTS
	if (JoystickEnabledOverrideForTest.IsSet())
	{
		return JoystickEnabledOverrideForTest.GetValue();
	}
#endif
	const UWoTPreferencesSubsystem* Preferences = GetGameInstance()
		? GetGameInstance()->GetSubsystem<UWoTPreferencesSubsystem>() : nullptr;
	return !Preferences || Preferences->GetGameSettings().bJoystickEnabled;
}

#if WITH_AUTOMATION_TESTS
bool AWoTPlayerController::IsJoystickInputEnabledForTest() const
{
	return IsJoystickInputEnabled();
}
#endif

void AWoTPlayerController::TickRetailCamera(float DeltaTime)
{
	AWoTCharacter* PawnCharacter = Cast<AWoTCharacter>(GetPawn());
	const UWoTPreferencesSubsystem* Preferences = GetGameInstance()
		? GetGameInstance()->GetSubsystem<UWoTPreferencesSubsystem>() : nullptr;
	if (!PawnCharacter || !Preferences || !PawnCharacter->GetCharacterMovement())
	{
		UE1WalkBob = FVector::ZeroVector;
		return;
	}
	if (UE1CameraPawn.Get() != PawnCharacter)
	{
		ResetRetailCameraState(PawnCharacter);
	}
	const FWoTGameSettings& Settings = Preferences->GetGameSettings();
	if (AWoTHUD* WoTHUD = Cast<AWoTHUD>(GetHUD()))
	{
		WoTHUD->Crosshair = Settings.Crosshair;
	}
	UCharacterMovementComponent* Move = PawnCharacter->GetCharacterMovement();
	const EMovementMode Mode = Move->MovementMode;
	const float Speed2D = Move->Velocity.Size2D();
	if (UE1PreviousMovementMode == MOVE_None)
	{
		UE1EyeHeight = PawnCharacter->BaseEyeHeight;
		UE1PreviousPawnLocation = PawnCharacter->GetActorLocation();
	}
	if (Mode == MOVE_Swimming)
	{
		// PlayerPawn's PlayerSwimming.PlayerMove.
		const FVector Y = GetControlRotation().RotateVector(FVector::RightVector);
		const float Time = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
		UE1WalkBob = Y * (Settings.ViewBob * 0.5f * Speed2D
			* FMath::Sin(4.0f * Time));
		UE1WalkBob.Z = Settings.ViewBob * 1.5f * Speed2D
			* FMath::Sin(8.0f * Time);
	}
	else if (Move->IsMovingOnGround())
	{
		// PlayerPawn.CheckBob: update the landing
		// contribution before composing either the idle or moving walk bob.
		UE1BobTime += Speed2D < 10.0f ? 0.2f * DeltaTime
			: DeltaTime * (0.3f + 0.7f * Speed2D
				/ FMath::Max(Move->MaxWalkSpeed, 1.0f));
		UE1AppliedBob = FMath::Min(1.0f, 16.0f * DeltaTime) * UE1LandBob
			+ UE1AppliedBob * (1.0f - FMath::Min(1.0f, 16.0f * DeltaTime));
		const FVector Y = GetControlRotation().RotateVector(FVector::RightVector);
		UE1WalkBob = Y * (0.65f * Settings.ViewBob * Speed2D
			* FMath::Sin(6.0f * UE1BobTime));
		UE1WalkBob.Z = UE1AppliedBob + Settings.ViewBob
			* (Speed2D < 10.0f ? 30.0f : Speed2D)
			* FMath::Sin(12.0f * UE1BobTime);
		UE1LandBob *= 1.0f - 8.0f * DeltaTime;
	}
	else
	{
		// Non-walking PlayerMove states reset BobTime and fade the last walk bob
		// instead of continuing CheckBob in mid-air.
		UE1BobTime = 0.0f;
		UE1WalkBob *= 1.0f - FMath::Min(1.0f, 8.0f * DeltaTime);
	}
	if (UE1PreviousMovementMode == MOVE_Falling
		&& (Mode == MOVE_Walking || Mode == MOVE_NavWalking))
	{
		UE1LandBob = FMath::Min(50.0f,
			0.055f * PawnCharacter->GetLastLandingImpactVelocity());
	}
	const float TargetEye = PawnCharacter->BaseEyeHeight;
	float Smooth = FMath::Min(1.0f, 10.0f * DeltaTime);
	const bool bGroundOrSwim = Move->IsMovingOnGround() || Mode == MOVE_Swimming;
	const bool bJustLanded = UE1PreviousMovementMode == MOVE_Falling
		&& Move->IsMovingOnGround();
	if (bGroundOrSwim && !bJustLanded)
	{
		UE1EyeHeight = (UE1EyeHeight - PawnCharacter->GetActorLocation().Z
			+ UE1PreviousPawnLocation.Z) * (1.0f - Smooth) + TargetEye * Smooth;
		const float Lower = -0.5f * PawnCharacter->GetSimpleCollisionHalfHeight();
		const float Upper = PawnCharacter->GetSimpleCollisionHalfHeight()
			+ FMath::Min(FMath::Max(0.0f, UE1PreviousPawnLocation.Z
				- PawnCharacter->GetActorLocation().Z), Move->MaxStepHeight);
		UE1EyeHeight = FMath::Clamp(UE1EyeHeight, Lower, Upper);
	}
	else
	{
		Smooth = FMath::Max(Smooth, 0.35f);
		UE1EyeHeight = UE1EyeHeight * (1.0f - Smooth) + TargetEye * Smooth;
	}
	UE1PreviousPawnLocation = PawnCharacter->GetActorLocation();
	UE1PreviousMovementMode = Mode;
}

void AWoTPlayerController::ClientFlash_Implementation(float Scale, FVector Fog)
{
	// PlayerPawn.ClientFlash assigns (rather than accumulates) the requested
	// pain flash; the next hit replaces the decaying target.
	DesiredFlashScale = Scale;
	DesiredFlashFog = Fog * 0.001f;
}

void AWoTPlayerController::TickUE1ViewFlash(float DeltaTime)
{
	// PlayerPawn.ViewFlash: damage flash decays at
	// 2/sec; the displayed value approaches constant glow + the head-zone fog.
	const float Delta = FMath::Min(0.1f, DeltaTime);
	FVector ZoneFog = FVector::ZeroVector;
	float ZoneFlashScale = 0.0f;
	if (const APawn* ViewedPawn = GetPawn())
	{
		if (const AWoTZoneVolume* Zone = AWoTZoneVolume::ZoneAt(GetWorld(),
			ViewedPawn->GetActorLocation() + FVector(0, 0, ViewedPawn->BaseEyeHeight)))
		{
			ZoneFog = Zone->ViewFog;
			ZoneFlashScale = Zone->ViewFlash.X;
		}
	}
	const float GoalScale = 1.0f + ConstantGlowScale + DesiredFlashScale
		+ ZoneFlashScale;
	const FVector GoalFog = ConstantGlowFog + DesiredFlashFog + ZoneFog;
	DesiredFlashScale -= DesiredFlashScale * 2.0f * Delta;
	DesiredFlashFog -= DesiredFlashFog * 2.0f * Delta;
	CurrentViewFlashScale += (GoalScale - CurrentViewFlashScale) * 10.0f * Delta;
	CurrentViewFlashFog += (GoalFog - CurrentViewFlashFog) * 10.0f * Delta;
	if (GoalScale > 0.981f && CurrentViewFlashScale > 0.981f
		&& CurrentViewFlashScale < 1.0f)
		CurrentViewFlashScale = 1.0f;
	else if (CurrentViewFlashScale < 0.019f)
		CurrentViewFlashScale = 0.0f;
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		if (CurrentViewFlashFog[Axis] > 0.0f && CurrentViewFlashFog[Axis] < 0.019f)
			CurrentViewFlashFog[Axis] = 0.0f;
	}
	// Compatibility/probe surface: presentation now separates UE1's multiplicative
	// scale from additive fog, while this field retains their final numeric sum.
	CurrentViewColorScale = FVector(CurrentViewFlashScale) + CurrentViewFlashFog;
	if (PlayerCameraManager)
	{
		PlayerCameraManager->SetDesiredColorScale(
			FVector(FMath::Max(0.0f, CurrentViewFlashScale)), 0.0f);
	}
}

void AWoTPlayerController::TickUE1ViewShake(float DeltaTime)
{
	FRotator View = GetControlRotation();
	float RollUnits = FMath::Fmod(View.Roll * 65536.0f / 360.0f, 65536.0f);
	if (RollUnits < 0.0f) { RollUnits += 65536.0f; }
	const float StepTime = FMath::Min(0.1f, DeltaTime);
	if (UE1ShakeTimer > 0.0f)
	{
		UE1ShakeTimer -= DeltaTime;
		if (UE1VertTimer == 0.0f)
		{
			UE1VertTimer = 0.1f;
			UE1ShakeVert = -1.1f * UE1MaxShakeVert;
		}
		else
		{
			UE1VertTimer -= DeltaTime;
			if (UE1VertTimer < 0.0f)
			{
				UE1VertTimer = 0.2f * WoTRandom::FRand();
				UE1ShakeVert = (2.0f * WoTRandom::FRand() - 1.0f) * UE1MaxShakeVert;
			}
		}
		if (bUE1ShakeDir)
		{
			RollUnits += static_cast<float>(FMath::TruncToInt(
				10.0f * UE1ShakeMag * StepTime));
			bUE1ShakeDir = RollUnits > 32768.0f
				|| RollUnits < (0.5f + WoTRandom::FRand()) * UE1ShakeMag;
			if (RollUnits < 32768.0f && RollUnits > 1.3f * UE1ShakeMag)
			{
				RollUnits = 1.3f * UE1ShakeMag;
				bUE1ShakeDir = false;
			}
			else if (WoTRandom::FRand() < 3.0f * DeltaTime) { bUE1ShakeDir = !bUE1ShakeDir; }
		}
		else
		{
			RollUnits -= static_cast<float>(FMath::TruncToInt(
				10.0f * UE1ShakeMag * StepTime));
			if (RollUnits < 0.0f) { RollUnits += 65536.0f; }
			bUE1ShakeDir = RollUnits > 32768.0f
				&& RollUnits < 65535.0f - (0.5f + WoTRandom::FRand()) * UE1ShakeMag;
			if (RollUnits > 32768.0f && RollUnits < 65535.0f - 1.3f * UE1ShakeMag)
			{
				RollUnits = 65535.0f - 1.3f * UE1ShakeMag;
				bUE1ShakeDir = true;
			}
			else if (WoTRandom::FRand() < 3.0f * DeltaTime) { bUE1ShakeDir = !bUE1ShakeDir; }
		}
	}
	else
	{
		UE1ShakeVert = 0.0f;
		if (RollUnits < 32768.0f)
		{
			if (RollUnits > 0.0f)
			{
				RollUnits = FMath::Max(0.0f, RollUnits
					- FMath::Max(RollUnits, 500.0f) * 10.0f * StepTime);
			}
		}
		else
		{
			RollUnits += (65536.0f - FMath::Max(500.0f, RollUnits))
				* 10.0f * StepTime;
			if (RollUnits > 65534.0f) { RollUnits = 0.0f; }
		}
	}
	View.Roll = RollUnits * 360.0f / 65536.0f;
	SetControlRotation(View);
}

// --- MP session console verbs. Thin wrappers so a test run drives sessions
// from the console; all real logic sits in the subsystem.
void AWoTPlayerController::WoTHost(bool bLan)
{
	if (UWoTSessionSubsystem* S =
	        GetGameInstance() ? GetGameInstance()->GetSubsystem<UWoTSessionSubsystem>()
	                          : nullptr)
	{
		// NEVER call inside UE_LOG arguments: NO_LOGGING builds skip them.
		const bool bHosting = S->Host(8, bLan);
		UE_LOG(LogNet, Display, TEXT("WoTHost -> %s"),
		       bHosting ? TEXT("creating") : TEXT("refused"));
	}
}

void AWoTPlayerController::WoTFind(bool bLan)
{
	if (UWoTSessionSubsystem* S =
	        GetGameInstance() ? GetGameInstance()->GetSubsystem<UWoTSessionSubsystem>()
	                          : nullptr)
	{
		const bool bFinding = S->Find(bLan);
		UE_LOG(LogNet, Display, TEXT("WoTFind -> %s"),
		       bFinding ? TEXT("searching") : TEXT("refused"));
	}
}

void AWoTPlayerController::WoTJoin(int32 Index)
{
	if (UWoTSessionSubsystem* S =
	        GetGameInstance() ? GetGameInstance()->GetSubsystem<UWoTSessionSubsystem>()
	                          : nullptr)
	{
		const bool bJoining = S->Join(Index);
		UE_LOG(LogNet, Display, TEXT("WoTJoin %d -> %s"), Index,
		       bJoining ? TEXT("joining") : TEXT("refused"));
	}
}

void AWoTPlayerController::WoTLeave()
{
	if (UWoTSessionSubsystem* S =
	        GetGameInstance() ? GetGameInstance()->GetSubsystem<UWoTSessionSubsystem>()
	                          : nullptr)
	{
		const bool bLeaving = S->Leave();
		UE_LOG(LogNet, Display, TEXT("WoTLeave -> %s"),
		       bLeaving ? TEXT("leaving") : TEXT("refused"));
	}
}
