// The controller (gamepad) input layer: Enhanced Input actions and two mapping
// contexts built in C++ from the generated layout, routed to the handlers the
// keyboard and mouse already use. One per local player controller. The
// keyboard/mouse pipeline never sees these actions, and these never see
// keyboard or mouse keys.
#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "WoTGamepadLayout.h"
#include "WoTGamepadLayer.generated.h"

class AWoTCharacter;
class AWoTPlayerController;
class UEnhancedInputComponent;
class UEnhancedInputLocalPlayerSubsystem;
class UInputAction;
class UInputMappingContext;
struct FInputActionValue;

enum class EWoTGamepadMode : uint8 { None, Gameplay, CitadelEditor };

UCLASS(Transient)
class UWoTGamepadLayer : public UObject
{
	GENERATED_BODY()

public:
	/** Build the actions and contexts and bind them on the controller's component. */
	void Initialize(AWoTPlayerController& InController, UEnhancedInputComponent& Input);
	/** Remove this layer's context and release every held edge. */
	void Shutdown();
	/** Install the context the controller's state calls for. */
	void Tick(float DeltaSeconds);
	/** A possession change ends the walk toggle and every held edge. */
	void OnPawnChanged();

	EWoTGamepadMode GetMode() const { return Mode; }
	const UInputAction* GetAction(EWoTGamepadAction Action) const;
	const UInputMappingContext* GetContext(EWoTGamepadContext Context) const;
#if WITH_AUTOMATION_TESTS
	/** Stand in for the player's stick settings (spec worlds have no preferences). */
	void SetStickSettingsForTest(float DeadZone, float Sensitivity, bool bInvertY);
	void ClearStickSettingsForTest();
#endif

private:
	struct FStickSettings
	{
		float DeadZone;
		float Sensitivity;
		bool bInvertY;
	};

	EWoTGamepadMode DesiredMode() const;
	void ApplyMode(EWoTGamepadMode NewMode);
	void ReleaseHeldEdges();
	UEnhancedInputLocalPlayerSubsystem* Subsystem() const;
	FStickSettings StickSettings() const;
	AWoTCharacter* Character() const;

	void OnMove(const FInputActionValue& Value);
	void OnLook(const FInputActionValue& Value);
	void BindButtons(UEnhancedInputComponent& Input);
	void OnFirePressed();
	void OnFireReleased();
	void OnJumpPressed();
	void OnJumpReleased();
	void OnCenterViewPressed();
	void OnCenterViewReleased();
	void OnScoreboardPressed();
	void OnScoreboardReleased();
	void OnWalkToggle();
	void OnButton(EWoTGamepadAction Action);

	TWeakObjectPtr<AWoTPlayerController> Controller;
	UPROPERTY()
	TArray<TObjectPtr<UInputAction>> Actions;            // by EWoTGamepadAction
	UPROPERTY()
	TArray<TObjectPtr<UInputMappingContext>> Contexts;   // by EWoTGamepadContext
	EWoTGamepadMode Mode = EWoTGamepadMode::None;
	bool bFireHeld = false;
	bool bJumpHeld = false;
	bool bScoreboardShownByHold = false;
	TWeakObjectPtr<AWoTCharacter> CenterViewPawn;
	TWeakObjectPtr<AWoTCharacter> WalkToggledPawn;
#if WITH_AUTOMATION_TESTS
	TOptional<FStickSettings> StickSettingsOverrideForTest;
#endif
};
