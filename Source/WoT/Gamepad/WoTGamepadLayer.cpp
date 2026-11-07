#include "WoTGamepadLayer.h"

#include "../WoTCharacter.h"
#include "../WoTHUD.h"
#include "../WoTPlayerController.h"
#include "../WoTPreferencesSubsystem.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "InputTriggers.h"
#include "WoTStickShaping.h"

void UWoTGamepadLayer::Initialize(AWoTPlayerController& InController, UEnhancedInputComponent& Input)
{
	Controller = &InController;
	const FWoTGamepadTuning& Tuning = WoTGamepadTuning();
	const int32 NumActions = static_cast<int32>(EWoTGamepadAction::Count);
	Actions.SetNum(NumActions);
	for (int32 Index = 0; Index < NumActions; ++Index)
	{
		const EWoTGamepadAction Id = static_cast<EWoTGamepadAction>(Index);
		UInputAction* Action = NewObject<UInputAction>(this, FName(WoTGamepadActionName(Id)));
		Action->ValueType = WoTGamepadActionIsStick(Id)
			? EInputActionValueType::Axis2D : EInputActionValueType::Boolean;
		// Mirrors the legacy bindings' bExecuteWhenPaused: a paused game still
		// takes the menu, the respawn jump and the weave release.
		Action->bTriggerWhenPaused = Id == EWoTGamepadAction::Menu
			|| Id == EWoTGamepadAction::Jump || Id == EWoTGamepadAction::Fire;
		Actions[Index] = Action;
	}
	const int32 NumContexts = static_cast<int32>(EWoTGamepadContext::Count);
	Contexts.SetNum(NumContexts);
	for (int32 Index = 0; Index < NumContexts; ++Index)
	{
		static const TCHAR* const ContextNames[] = {
			TEXT("Gameplay"), TEXT("CitadelEditor"), TEXT("TeamSelect")};
		static_assert(UE_ARRAY_COUNT(ContextNames) == static_cast<int32>(EWoTGamepadContext::Count));
		Contexts[Index] = NewObject<UInputMappingContext>(this, FName(ContextNames[Index]));
	}
	for (const FWoTGamepadBinding& Row : WoTGamepadLayout())
	{
		UInputMappingContext* Context = Contexts[static_cast<int32>(Row.Context)];
		UInputAction* Action = Actions[static_cast<int32>(Row.Action)];
		FEnhancedActionKeyMapping& Mapping = Context->MapKey(Action, WoTGamepadKey(Row.Input));
		if (Row.Trigger == EWoTGamepadTrigger::Tap)
		{
			UInputTriggerTap* Tap = NewObject<UInputTriggerTap>(Context);
			Tap->TapReleaseTimeThreshold = Tuning.DropHoldSeconds;
			Mapping.Triggers.Add(Tap);
		}
		else if (Row.Trigger == EWoTGamepadTrigger::Hold)
		{
			UInputTriggerHold* Hold = NewObject<UInputTriggerHold>(Context);
			Hold->HoldTimeThreshold = Tuning.DropHoldSeconds;
			Hold->bIsOneShot = true;
			Mapping.Triggers.Add(Hold);
		}
		const FKey StickY = WoTGamepadStickYKey(Row.Input);
		if (StickY.IsValid())
		{
			// The stick's Y axis feeds the 2D value's Y.
			FEnhancedActionKeyMapping& YMapping = Context->MapKey(Action, StickY);
			UInputModifierSwizzleAxis* Swizzle = NewObject<UInputModifierSwizzleAxis>(Context);
			Swizzle->Order = EInputAxisSwizzle::YXZ;
			YMapping.Modifiers.Add(Swizzle);
		}
	}
	Input.BindAction(Actions[static_cast<int32>(EWoTGamepadAction::Move)],
		ETriggerEvent::Triggered, this, &UWoTGamepadLayer::OnMove);
	Input.BindAction(Actions[static_cast<int32>(EWoTGamepadAction::Look)],
		ETriggerEvent::Triggered, this, &UWoTGamepadLayer::OnLook);
	BindButtons(Input);
}

void UWoTGamepadLayer::BindButtons(UEnhancedInputComponent& Input)
{
	auto Action = [this](EWoTGamepadAction Id) { return Actions[static_cast<int32>(Id)].Get(); };
	Input.BindAction(Action(EWoTGamepadAction::Fire), ETriggerEvent::Started, this, &UWoTGamepadLayer::OnFirePressed);
	Input.BindAction(Action(EWoTGamepadAction::Fire), ETriggerEvent::Completed, this, &UWoTGamepadLayer::OnFireReleased);
	Input.BindAction(Action(EWoTGamepadAction::Jump), ETriggerEvent::Started, this, &UWoTGamepadLayer::OnJumpPressed);
	Input.BindAction(Action(EWoTGamepadAction::Jump), ETriggerEvent::Completed, this, &UWoTGamepadLayer::OnJumpReleased);
	Input.BindAction(Action(EWoTGamepadAction::CenterView), ETriggerEvent::Started, this, &UWoTGamepadLayer::OnCenterViewPressed);
	Input.BindAction(Action(EWoTGamepadAction::CenterView), ETriggerEvent::Completed, this, &UWoTGamepadLayer::OnCenterViewReleased);
	Input.BindAction(Action(EWoTGamepadAction::Scoreboard), ETriggerEvent::Started, this, &UWoTGamepadLayer::OnScoreboardPressed);
	Input.BindAction(Action(EWoTGamepadAction::Scoreboard), ETriggerEvent::Completed, this, &UWoTGamepadLayer::OnScoreboardReleased);
	Input.BindAction(Action(EWoTGamepadAction::WalkToggle), ETriggerEvent::Started, this, &UWoTGamepadLayer::OnWalkToggle);
	Input.BindAction(Action(EWoTGamepadAction::TurnAround), ETriggerEvent::Started, this, &UWoTGamepadLayer::OnTurnAround);
	for (EWoTGamepadAction Id : {EWoTGamepadAction::EditCitadel, EWoTGamepadAction::Drop,
		EWoTGamepadAction::DropTainted, EWoTGamepadAction::ItemInfo, EWoTGamepadAction::PrevItem,
		EWoTGamepadAction::NextItem, EWoTGamepadAction::PrevHand, EWoTGamepadAction::NextHand,
		EWoTGamepadAction::ViewUp, EWoTGamepadAction::ViewDown, EWoTGamepadAction::RandomTaunt,
		EWoTGamepadAction::Menu, EWoTGamepadAction::JoinTeam1, EWoTGamepadAction::JoinTeam2,
		EWoTGamepadAction::JoinTeam3, EWoTGamepadAction::JoinTeam4})
	{
		// A plain press acts on its press edge; a tap or hold acts once when its
		// trigger completes (Started would fire a tap on the press).
		const ETriggerEvent Event = WoTGamepadActionTrigger(Id) == EWoTGamepadTrigger::Press
			? ETriggerEvent::Started : ETriggerEvent::Triggered;
		Input.BindAction(Action(Id), Event, this, &UWoTGamepadLayer::OnButton, Id);
	}
}

void UWoTGamepadLayer::Shutdown()
{
	ReleaseHeldEdges();
	if (UEnhancedInputLocalPlayerSubsystem* Input = Subsystem())
	{
		FModifyContextOptions Options;
		Options.bForceImmediately = true;
		for (UInputMappingContext* Context : Contexts) { Input->RemoveMappingContext(Context, Options); }
	}
	Mode = EWoTGamepadMode::None;
}

const UInputAction* UWoTGamepadLayer::GetAction(EWoTGamepadAction Action) const
{
	const int32 Index = static_cast<int32>(Action);
	return Actions.IsValidIndex(Index) ? Actions[Index].Get() : nullptr;
}

const UInputMappingContext* UWoTGamepadLayer::GetContext(EWoTGamepadContext Context) const
{
	const int32 Index = static_cast<int32>(Context);
	return Contexts.IsValidIndex(Index) ? Contexts[Index].Get() : nullptr;
}

void UWoTGamepadLayer::Tick(float DeltaSeconds)
{
	const EWoTGamepadMode Desired = DesiredMode();
	if (Desired != Mode)
	{
		ApplyMode(Desired);
	}
	// The quick turn spreads 180 degrees over TurnAroundSeconds through the same
	// look path as the stick, so the last step lands exactly on 180.
	AWoTCharacter* Pawn = TurnAroundPawn.Get();
	const UWorld* World = Pawn ? Pawn->GetWorld() : nullptr;
	if (TurnAroundRemaining > 0.0f && Pawn && World && !World->IsPaused())
	{
		const float Seconds = FMath::Max(WoTGamepadTuning().TurnAroundSeconds, UE_KINDA_SMALL_NUMBER);
		const float Step = FMath::Min(TurnAroundRemaining, 180.0f * DeltaSeconds / Seconds);
		Pawn->ApplyStickLook(Step, 0.0f);
		TurnAroundRemaining -= Step;
	}
	else if (!Pawn)
	{
		TurnAroundRemaining = 0.0f;
	}
}

EWoTGamepadMode UWoTGamepadLayer::DesiredMode() const
{
	const AWoTPlayerController* PC = Controller.Get();
	if (!PC || PC->IsChatTyping())
	{
		// The chat line drops gamepad presses; a stick must not walk the player either.
		return EWoTGamepadMode::None;
	}
	if (PC->IsOwnerEditingCitadel())
	{
		return EWoTGamepadMode::CitadelEditor;
	}
	const AWoTHUD* HUD = Cast<AWoTHUD>(PC->GetHUD());
	return HUD && HUD->IsChoosingTeam() ? EWoTGamepadMode::TeamSelect : EWoTGamepadMode::Gameplay;
}

void UWoTGamepadLayer::ApplyMode(EWoTGamepadMode NewMode)
{
	UEnhancedInputLocalPlayerSubsystem* Input = Subsystem();
	if (!Input)
	{
		return;   // no local player yet: try again next tick
	}
	ReleaseHeldEdges();
	// Rebuild now, not at the input module's next frame tick: the new context
	// then applies to this frame's input, which Tick precedes.
	FModifyContextOptions Options;
	Options.bForceImmediately = true;
	for (UInputMappingContext* Context : Contexts) { Input->RemoveMappingContext(Context, Options); }
	if (NewMode != EWoTGamepadMode::None)
	{
		const EWoTGamepadContext Context = NewMode == EWoTGamepadMode::CitadelEditor
			? EWoTGamepadContext::CitadelEditor : EWoTGamepadContext::Gameplay;
		Input->AddMappingContext(Contexts[static_cast<int32>(Context)], 0, Options);
	}
	if (NewMode == EWoTGamepadMode::TeamSelect)
	{
		// Above Gameplay: its D-pad mappings consume the keys the hands and taunt use.
		Input->AddMappingContext(
			Contexts[static_cast<int32>(EWoTGamepadContext::TeamSelect)], 1, Options);
	}
	Mode = NewMode;
}

void UWoTGamepadLayer::ReleaseHeldEdges()
{
	// Every held edge gets its release exactly once, whatever removed the press:
	// a mode change, a key flush or a possession change.
	OnFireReleased();
	OnJumpReleased();
	OnCenterViewReleased();
	OnScoreboardReleased();
}

void UWoTGamepadLayer::OnPawnChanged()
{
	ReleaseHeldEdges();
	TurnAroundRemaining = 0.0f;
	TurnAroundPawn.Reset();
	if (AWoTCharacter* Walker = WalkToggledPawn.Get())
	{
		Walker->WalkingReleased();
	}
	WalkToggledPawn.Reset();
}

void UWoTGamepadLayer::OnFirePressed()
{
	AWoTPlayerController* PC = Controller.Get();
	// Fire triggers while paused only for its release (the keyboard's Fire press
	// binding does not run when paused): a paused press must not cast or respawn.
	if (!PC || (PC->GetWorld() && PC->GetWorld()->IsPaused()))
	{
		return;
	}
	bFireHeld = true;
	PC->Fire();
}

void UWoTGamepadLayer::OnFireReleased()
{
	if (!bFireHeld) { return; }
	bFireHeld = false;
	if (AWoTPlayerController* PC = Controller.Get()) { PC->CeaseFire(); }
}

void UWoTGamepadLayer::OnJumpPressed()
{
	if (AWoTPlayerController* PC = Controller.Get())
	{
		bJumpHeld = true;
		PC->Jump();
	}
}

void UWoTGamepadLayer::OnJumpReleased()
{
	if (!bJumpHeld) { return; }
	bJumpHeld = false;
	if (AWoTPlayerController* PC = Controller.Get()) { PC->StopJumping(); }
}

void UWoTGamepadLayer::OnCenterViewPressed()
{
	if (AWoTCharacter* Pawn = Character())
	{
		CenterViewPawn = Pawn;
		Pawn->CenterViewPressed();
	}
}

void UWoTGamepadLayer::OnCenterViewReleased()
{
	if (AWoTCharacter* Pawn = CenterViewPawn.Get()) { Pawn->CenterViewReleased(); }
	CenterViewPawn.Reset();
}

void UWoTGamepadLayer::OnScoreboardPressed()
{
	if (AWoTPlayerController* PC = Controller.Get())
	{
		PC->ShowScores();
		bScoreboardShownByHold = PC->bShowScores;
	}
}

void UWoTGamepadLayer::OnScoreboardReleased()
{
	AWoTPlayerController* PC = Controller.Get();
	if (bScoreboardShownByHold && PC && PC->bShowScores)
	{
		PC->ShowScores();
	}
	bScoreboardShownByHold = false;
}

void UWoTGamepadLayer::OnTurnAround()
{
	// A press during a turn is ignored; the turn in progress finishes.
	AWoTCharacter* Pawn = Character();
	if (!Pawn || TurnAroundRemaining > 0.0f)
	{
		return;
	}
	TurnAroundPawn = Pawn;
	TurnAroundRemaining = 180.0f;
}

void UWoTGamepadLayer::OnWalkToggle()
{
	AWoTCharacter* Pawn = Character();
	if (!Pawn) { return; }
	if (WalkToggledPawn.Get() == Pawn)
	{
		Pawn->WalkingReleased();
		WalkToggledPawn.Reset();
	}
	else
	{
		Pawn->WalkingPressed();
		WalkToggledPawn = Pawn;
	}
}

void UWoTGamepadLayer::OnButton(EWoTGamepadAction Action)
{
	AWoTPlayerController* PC = Controller.Get();
	AWoTCharacter* Pawn = Character();
	if (!PC) { return; }
	switch (Action)
	{
	case EWoTGamepadAction::EditCitadel: PC->ServerEditCitadelInput(); break;
	case EWoTGamepadAction::ItemInfo: PC->ShowInventoryInfo(); break;
	case EWoTGamepadAction::ViewUp: PC->ViewUp(); break;
	case EWoTGamepadAction::ViewDown: PC->ViewDown(); break;
	case EWoTGamepadAction::RandomTaunt: PC->PlayTaunt(-1); break;
	case EWoTGamepadAction::Menu: PC->ToggleInGameMenu(); break;
	// The number keys' team choice (BattleHUD.KeyEvent), on the D-pad.
	case EWoTGamepadAction::JoinTeam1:
	case EWoTGamepadAction::JoinTeam2:
	case EWoTGamepadAction::JoinTeam3:
	case EWoTGamepadAction::JoinTeam4:
		if (AWoTHUD* HUD = Cast<AWoTHUD>(PC->GetHUD()))
		{
			HUD->HandleTeamSelectDigit(static_cast<int32>(Action)
				- static_cast<int32>(EWoTGamepadAction::JoinTeam1));
		}
		break;
	// Pawn-owned actions do nothing without a pawn, as their keys do.
	case EWoTGamepadAction::Drop: if (Pawn) { Pawn->Drop(); } break;
	case EWoTGamepadAction::DropTainted: if (Pawn) { Pawn->DropTainted(); } break;
	case EWoTGamepadAction::PrevItem: if (Pawn) { Pawn->PrevItem(); } break;
	case EWoTGamepadAction::NextItem: if (Pawn) { Pawn->NextItem(); } break;
	case EWoTGamepadAction::PrevHand: if (Pawn) { Pawn->PrevHand(); } break;
	case EWoTGamepadAction::NextHand: if (Pawn) { Pawn->NextHand(); } break;
	default: break;
	}
}

UEnhancedInputLocalPlayerSubsystem* UWoTGamepadLayer::Subsystem() const
{
	const AWoTPlayerController* PC = Controller.Get();
	const ULocalPlayer* LocalPlayer = PC ? PC->GetLocalPlayer() : nullptr;
	return LocalPlayer ? LocalPlayer->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>() : nullptr;
}

#if WITH_AUTOMATION_TESTS
void UWoTGamepadLayer::SetStickSettingsForTest(float DeadZone, float Sensitivity, bool bInvertY)
{
	StickSettingsOverrideForTest = FStickSettings{DeadZone, Sensitivity, bInvertY};
}

void UWoTGamepadLayer::ClearStickSettingsForTest()
{
	StickSettingsOverrideForTest.Reset();
}
#endif

UWoTGamepadLayer::FStickSettings UWoTGamepadLayer::StickSettings() const
{
#if WITH_AUTOMATION_TESTS
	if (StickSettingsOverrideForTest.IsSet())
	{
		return StickSettingsOverrideForTest.GetValue();
	}
#endif
	static const FWoTGameSettings Fallback;
	const AWoTPlayerController* PC = Controller.Get();
	const UGameInstance* GameInstance = PC ? PC->GetGameInstance() : nullptr;
	const UWoTPreferencesSubsystem* Preferences = GameInstance
		? GameInstance->GetSubsystem<UWoTPreferencesSubsystem>() : nullptr;
	const FWoTGameSettings& Settings = Preferences ? Preferences->GetGameSettings() : Fallback;
	return {Settings.ControllerDeadZone, Settings.ControllerLookSensitivity, Settings.bControllerInvertY};
}

AWoTCharacter* UWoTGamepadLayer::Character() const
{
	const AWoTPlayerController* PC = Controller.Get();
	return PC ? Cast<AWoTCharacter>(PC->GetPawn()) : nullptr;
}

void UWoTGamepadLayer::OnMove(const FInputActionValue& Value)
{
	AWoTCharacter* Pawn = Character();
	if (!Pawn)
	{
		return;
	}
	const FStickSettings Settings = StickSettings();
	const FWoTGamepadTuning& Tuning = WoTGamepadTuning();
	const FVector2D Input = FWoTStickShaping::Move(Value.Get<FVector2D>(),
		Settings.DeadZone, Tuning.OuterZone, Tuning.MoveGain);
	// The keyboard path's rules (orbit zoom, FreeLook, swimming) without its
	// per-frame swim-up, which the keyboard axis pass already adds.
	Pawn->ApplyForwardAxis(static_cast<float>(Input.Y));
	Pawn->ApplyStrafeAxis(static_cast<float>(Input.X));
}

void UWoTGamepadLayer::OnLook(const FInputActionValue& Value)
{
	AWoTCharacter* Pawn = Character();
	const UWorld* World = Pawn ? Pawn->GetWorld() : nullptr;
	if (!Pawn || !World)
	{
		return;
	}
	const FStickSettings Settings = StickSettings();
	const FWoTGamepadTuning& Tuning = WoTGamepadTuning();
	const FVector2D Degrees = FWoTStickShaping::LookDegrees(Value.Get<FVector2D>(),
		Settings.DeadZone, Tuning.OuterZone, Tuning.LookExponent, Tuning.YawRate,
		Tuning.PitchRate, Settings.Sensitivity, Settings.bInvertY,
		Pawn->RetailFOVScale(), World->GetDeltaSeconds());
	Pawn->ApplyStickLook(static_cast<float>(Degrees.X), static_cast<float>(Degrees.Y));
}
