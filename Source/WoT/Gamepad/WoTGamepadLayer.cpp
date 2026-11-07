#include "WoTGamepadLayer.h"

#include "../WoTCharacter.h"
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
		Contexts[Index] = NewObject<UInputMappingContext>(this,
			Index == 0 ? FName(TEXT("Gameplay")) : FName(TEXT("CitadelEditor")));
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

void UWoTGamepadLayer::Tick(float)
{
	const EWoTGamepadMode Desired = DesiredMode();
	if (Desired != Mode)
	{
		ApplyMode(Desired);
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
	return PC->IsOwnerEditingCitadel() ? EWoTGamepadMode::CitadelEditor : EWoTGamepadMode::Gameplay;
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
		const EWoTGamepadContext Context = NewMode == EWoTGamepadMode::Gameplay
			? EWoTGamepadContext::Gameplay : EWoTGamepadContext::CitadelEditor;
		Input->AddMappingContext(Contexts[static_cast<int32>(Context)], 0, Options);
	}
	Mode = NewMode;
}

void UWoTGamepadLayer::ReleaseHeldEdges()
{
	// The button task releases Fire, Jump, CenterView and the scoreboard hold here.
}

void UWoTGamepadLayer::OnPawnChanged()
{
	ReleaseHeldEdges();
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
