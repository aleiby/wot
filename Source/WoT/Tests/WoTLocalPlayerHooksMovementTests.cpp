#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS
#include "WoTLocalPlayerHooksTestFixture.h"
#include "../Settings/WoTGameSettings.h"
#include "../WoTCharacter.h"
#include "../WoTLocalPlayerHooks.h"
#include "../WoTPlayerController.h"
#include "../WoTPreferencesSubsystem.h"
#include "../WoTZoneVolume.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/InputComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"

namespace
{
constexpr auto MovementHookFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;
const FName MovementHookOwner(TEXT("MovementHookSpec"));

/** Drive an axis binding of the pawn's input component, as the key would. */
bool MovementHookAxis(const AWoTCharacter& Pawn, FName Axis, float Value)
{
	if (UInputComponent* Input = Pawn.InputComponent.Get())
	{
		for (FInputAxisBinding& Binding : Input->AxisBindings)
		{
			if (Binding.AxisName == Axis)
			{
				Binding.AxisDelegate.Execute(Value);
				return true;
			}
		}
	}
	return false;
}

/** Stand the pawn at rest on the fixture floor, walking. */
void MovementHookStand(WoTLocalPlayerHooksTest::FPlayerWorld& F, const FVector& Where)
{
	const float HalfHeight = F.Pawn->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	F.Pawn->SetActorLocation(FVector(Where.X, Where.Y, HalfHeight + 1.0f), false, nullptr,
		ETeleportType::TeleportPhysics);
	UCharacterMovementComponent* Move = F.Pawn->GetCharacterMovement();
	Move->StopMovementImmediately();
	Move->SetMovementMode(MOVE_Walking);
	F.PC->SetControlRotation(FRotator::ZeroRotator);
	F.Frames(5);
	Move->StopMovementImmediately();
}

/** Peak ground speed over Frames ticks, with or without a held forward stick. */
double MovementHookPeakSpeed(WoTLocalPlayerHooksTest::FPlayerWorld& F, bool bStick, int32 Frames)
{
	double Peak = 0.0;
	for (int32 Frame = 0; Frame < Frames; ++Frame)
	{
		if (bStick)
		{
			MovementHookAxis(*F.Pawn, TEXT("MoveForward"), 1.0f);
		}
		F.Frames(1);
		Peak = FMath::Max(Peak, F.Pawn->GetCharacterMovement()->Velocity.Size2D());
	}
	return Peak;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHookMovementBasisSpec, "WoT.VR.Hooks.H3.MovementBasisAndRestore", MovementHookFlags)
bool FWoTHookMovementBasisSpec::RunTest(const FString&)
{
	WoTLocalPlayerHooksTest::FPlayerWorld F;
	if (!F.Init(*this)) return false;
	F.PC->SetControlRotation(FRotator::ZeroRotator);
	TestTrue(TEXT("forward binding"), MovementHookAxis(*F.Pawn, TEXT("MoveForward"), 1.0f));
	TestTrue(TEXT("flat forward follows the control yaw"),
		F.Pawn->ConsumeMovementInputVector().Equals(FVector::ForwardVector, 0.01));
	TestTrue(TEXT("strafe binding"), MovementHookAxis(*F.Pawn, TEXT("MoveRight"), 1.0f));
	TestTrue(TEXT("flat strafe follows the control yaw"),
		F.Pawn->ConsumeMovementInputVector().Equals(FVector::RightVector, 0.01));
	// With nothing attached the direction is the control yaw's own, in double,
	// bit for bit (a yaw a float cannot hold exactly).
	F.PC->SetControlRotation(FRotator(0.0, 123.456789012345678, 0.0));
	const double FlatYaw = F.PC->GetControlRotation().Yaw;
	MovementHookAxis(*F.Pawn, TEXT("MoveForward"), 1.0f);
	TestTrue(TEXT("flat forward is exactly the control yaw's direction"),
		F.Pawn->ConsumeMovementInputVector() == FRotator(0, FlatYaw, 0).Vector());
	MovementHookAxis(*F.Pawn, TEXT("MoveRight"), 1.0f);
	TestTrue(TEXT("flat strafe is exactly the control yaw's right"),
		F.Pawn->ConsumeMovementInputVector()
			== FRotationMatrix(FRotator(0, FlatYaw, 0)).GetScaledAxis(EAxis::Y));
	F.PC->SetControlRotation(FRotator::ZeroRotator);

	double Basis = 90.0;
	bool bSupplyBasis = true;
	FWoTLocalPlayerConsumer C;
	C.GroundMovementYaw = [&](double& Out) { Out = Basis; return bSupplyBasis; };
	TestTrue(TEXT("movement basis attached"), FWoTLocalPlayerHooks::Attach(*F.Player, MovementHookOwner, C));
	FWoTLocalPlayerConsumer Rival;
	Rival.ExtraMovement = [] { return FVector::ForwardVector; };
	AddExpectedError(TEXT("exclusive local-player slot"), EAutomationExpectedErrorFlags::Contains, 1);
	TestFalse(TEXT("an extra source shares the occupied movement slot"),
		FWoTLocalPlayerHooks::Attach(*F.Player, TEXT("Rival"), Rival));

	MovementHookAxis(*F.Pawn, TEXT("MoveForward"), 1.0f);
	TestTrue(TEXT("forward follows the supplied basis"),
		F.Pawn->ConsumeMovementInputVector().Equals(FVector(0, 1, 0), 0.01));
	MovementHookAxis(*F.Pawn, TEXT("MoveRight"), 1.0f);
	TestTrue(TEXT("strafe follows the supplied basis"),
		F.Pawn->ConsumeMovementInputVector().Equals(FVector(-1, 0, 0), 0.01));
	TestTrue(TEXT("the aim is untouched by the basis"),
		F.PC->GetControlRotation().Equals(FRotator::ZeroRotator, 0.01));
	bSupplyBasis = false;
	MovementHookAxis(*F.Pawn, TEXT("MoveForward"), 1.0f);
	TestTrue(TEXT("a declined basis falls back to the control yaw"),
		F.Pawn->ConsumeMovementInputVector().Equals(FVector::ForwardVector, 0.01));
	bSupplyBasis = true;

	// Swimming keeps the full view direction, whatever the ground basis.
	F.Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Swimming);
	const FRotator Dive(-30, 0, 0);
	F.PC->SetControlRotation(Dive);
	MovementHookAxis(*F.Pawn, TEXT("MoveForward"), 1.0f);
	TestTrue(TEXT("swimming forward follows the view, not the basis"),
		F.Pawn->ConsumeMovementInputVector().GetSafeNormal().Equals(Dive.Vector(), 0.01));
	F.Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	F.PC->SetControlRotation(FRotator::ZeroRotator);

	FWoTLocalPlayerHooks::Detach(*F.Player, MovementHookOwner);
	MovementHookAxis(*F.Pawn, TEXT("MoveForward"), 1.0f);
	TestTrue(TEXT("detach restores the flat basis"),
		F.Pawn->ConsumeMovementInputVector().Equals(FVector::ForwardVector, 0.01));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHookMovementHoldSpec, "WoT.VR.Hooks.H3.MovementHoldIsTheOwners", MovementHookFlags)
bool FWoTHookMovementHoldSpec::RunTest(const FString&)
{
	WoTLocalPlayerHooksTest::FPlayerWorld F;
	if (!F.Init(*this)) return false;
	const FName ObserverOwner(TEXT("HoldObserver"));
	TestFalse(TEXT("nothing holds the body without a consumer"), FWoTLocalPlayerHooks::IsMovementHeld(F.PC));
	FWoTLocalPlayerHooks::HoldMovement(*F.Player, MovementHookOwner, true);
	TestFalse(TEXT("a hold needs an attached owner"), F.PC->IsMoveInputIgnored());
	FWoTLocalPlayerConsumer Observer;
	Observer.Lifecycle = [](EWoTLocalPlayerEvent, const FWoTLocalPlayerContext&) {};
	TestTrue(TEXT("observer attached"), FWoTLocalPlayerHooks::Attach(*F.Player, ObserverOwner, Observer));
	FWoTLocalPlayerHooks::HoldMovement(*F.Player, ObserverOwner, true);
	TestFalse(TEXT("a consumer without the movement slot cannot hold"), F.PC->IsMoveInputIgnored());

	FWoTLocalPlayerConsumer C;
	C.GroundMovementYaw = [](double& Out) { Out = 0.0; return true; };
	TestTrue(TEXT("movement owner attached"), FWoTLocalPlayerHooks::Attach(*F.Player, MovementHookOwner, C));
	TestFalse(TEXT("attaching holds nothing"), F.PC->IsMoveInputIgnored());
	FWoTLocalPlayerHooks::HoldMovement(*F.Player, MovementHookOwner, true);
	TestTrue(TEXT("the owner's hold ignores the body's movement input"),
		F.PC->IsMoveInputIgnored() && F.Pawn->IsMoveInputIgnored());
	MovementHookAxis(*F.Pawn, TEXT("MoveForward"), 1.0f);
	TestTrue(TEXT("held: the stick moves nothing"), F.Pawn->ConsumeMovementInputVector().IsNearlyZero());
	// A respawn's restart resets the controller's own ignore state; the hold is
	// the owner's and stays.
	F.PC->ResetIgnoreInputFlags();
	TestTrue(TEXT("a restart's input reset leaves the hold"), F.PC->IsMoveInputIgnored());
	// The game's own freeze (the match end) survives the hold's release.
	F.PC->SetIgnoreMoveInput(true);
	FWoTLocalPlayerHooks::HoldMovement(*F.Player, MovementHookOwner, false);
	TestTrue(TEXT("lifting the hold keeps the game's own ignore count"), F.PC->IsMoveInputIgnored());
	F.PC->SetIgnoreMoveInput(false);
	TestFalse(TEXT("the game's own release frees the body"), F.PC->IsMoveInputIgnored());
	MovementHookAxis(*F.Pawn, TEXT("MoveForward"), 1.0f);
	TestFalse(TEXT("free: the stick moves again"), F.Pawn->ConsumeMovementInputVector().IsNearlyZero());
	FWoTLocalPlayerHooks::HoldMovement(*F.Player, MovementHookOwner, true);
	FWoTLocalPlayerHooks::Detach(*F.Player, MovementHookOwner);
	TestFalse(TEXT("detach lifts the hold"), F.PC->IsMoveInputIgnored());
	FWoTLocalPlayerHooks::Detach(*F.Player, ObserverOwner);
	TestFalse(TEXT("nothing left attached"), FWoTLocalPlayerHooks::HasConsumers(F.Player));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHookExtraMovementSpec, "WoT.VR.Hooks.H3.ExtraMovementClampAndCollision", MovementHookFlags)
bool FWoTHookExtraMovementSpec::RunTest(const FString&)
{
	WoTLocalPlayerHooksTest::FPlayerWorld F;
	if (!F.Init(*this)) return false;
	if (!TestNotNull(TEXT("floor"), F.SpawnBlock(FVector(0, 0, -10), FVector(6000, 6000, 10)))) return false;

	MovementHookStand(F, FVector::ZeroVector);
	const double StickPeak = MovementHookPeakSpeed(F, /*bStick*/ true, 90);
	TestTrue(TEXT("the stick alone walks at an ordinary speed"), StickPeak > 100.0);

	FVector Extra = FVector::ForwardVector;
	int32 Samples = 0;
	FWoTLocalPlayerConsumer C;
	C.ExtraMovement = [&] { ++Samples; return Extra; };
	TestTrue(TEXT("extra movement attached"), FWoTLocalPlayerHooks::Attach(*F.Player, MovementHookOwner, C));

	MovementHookStand(F, FVector::ZeroVector);
	const FVector Start = F.Pawn->GetActorLocation();
	const double ExtraPeak = MovementHookPeakSpeed(F, /*bStick*/ false, 90);
	TestTrue(TEXT("the extra input alone walks the body"), F.Pawn->GetActorLocation().X > Start.X + 100.0);
	TestTrue(TEXT("the extra input alone is no faster than the stick"), ExtraPeak <= StickPeak + 0.5);

	for (const FVector& Direction : {FVector::ForwardVector, FVector::RightVector, FVector(1, 1, 0)})
	{
		Extra = Direction * 5.0;   // oversized on purpose
		MovementHookStand(F, FVector::ZeroVector);
		const double CombinedPeak = MovementHookPeakSpeed(F, /*bStick*/ true, 90);
		TestTrue(FString::Printf(TEXT("stick plus extra (%s) never exceeds the stick's speed"),
			*Direction.ToString()), CombinedPeak <= StickPeak + 0.5);
	}

	// A wall stops the body; the input only steers ordinary movement.
	if (!TestNotNull(TEXT("wall"), F.SpawnBlock(FVector(420, 0, 100), FVector(20, 2000, 200)))) return false;
	Extra = FVector::ForwardVector;
	MovementHookStand(F, FVector::ZeroVector);
	F.Frames(240);
	const float Radius = F.Pawn->GetCapsuleComponent()->GetScaledCapsuleRadius();
	TestTrue(TEXT("the extra input carries the body to the wall"), F.Pawn->GetActorLocation().X > 300.0);
	TestTrue(TEXT("the wall stops the body"), F.Pawn->GetActorLocation().X + Radius <= 400.0 + 1.0);

	// In water the strokes stay below the acceleration clamp; the extra input is
	// scaled like a full forward stroke and never exceeds the stick's own.
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	const FVector Pool(0, 6000, 3000);
	AWoTZoneVolume* Water = F.World->SpawnActor<AWoTZoneVolume>(Pool, FRotator::ZeroRotator, Params);
	if (!TestNotNull(TEXT("water"), Water)) return false;
	Water->Collision->SetBoxExtent(FVector(2000.0f));
	Water->bWaterZone = true;
	UCharacterMovementComponent* Move = F.Pawn->GetCharacterMovement();
	auto SwimAcceleration = [&](bool bStick, const FVector& ExtraInput)
	{
		Extra = ExtraInput;
		F.Pawn->SetActorLocation(Pool, false, nullptr, ETeleportType::TeleportPhysics);
		Move->StopMovementImmediately();
		Move->SetMovementMode(MOVE_Swimming);
		F.PC->SetControlRotation(FRotator::ZeroRotator);
		if (bStick)
		{
			MovementHookAxis(*F.Pawn, TEXT("MoveForward"), 1.0f);
		}
		F.Frames(1);
		TestEqual(TEXT("the body swims"), static_cast<int32>(Move->MovementMode.GetValue()),
			static_cast<int32>(MOVE_Swimming));
		return Move->GetCurrentAcceleration().Size();
	};
	const double StrokeAccel = SwimAcceleration(/*bStick*/ true, FVector::ZeroVector);
	TestTrue(TEXT("a full forward stroke stays below the clamp"),
		StrokeAccel > 0.0 && StrokeAccel < Move->GetMaxAcceleration() - 1.0);
	TestTrue(TEXT("the extra input alone accelerates no harder than a stroke"),
		SwimAcceleration(/*bStick*/ false, FVector::ForwardVector * 5.0) <= StrokeAccel + 0.5);
	TestTrue(TEXT("stroke plus extra accelerates no harder than the stroke"),
		SwimAcceleration(/*bStick*/ true, FVector::ForwardVector * 5.0) <= StrokeAccel + 0.5);
	TestTrue(TEXT("stroke plus sideways extra accelerates no harder than the stroke"),
		SwimAcceleration(/*bStick*/ true, FVector::RightVector * 5.0) <= StrokeAccel + 0.5);

	// Ignored wherever the stick is, such as behind the in-game menu.
	Extra = FVector::ForwardVector;
	MovementHookStand(F, FVector(-3000, 0, 0));
	F.PC->ToggleInGameMenu();
	const FVector Blocked = F.Pawn->GetActorLocation();
	F.Frames(30);
	TestTrue(TEXT("the open menu stops the extra input"),
		F.Pawn->GetActorLocation().Equals(Blocked, 1.0));
	F.PC->ResumeFromInGameMenu();

	// An onboarding window holds UI-only input through the controller's overlay
	// seam: no key or stick reaches the game, and neither does the extra input.
	MovementHookStand(F, FVector(-3000, 0, 0));
	if (TestTrue(TEXT("the controller's overlay seam is installed"), F.SetOverlayInput(true)))
	{
		TestTrue(TEXT("the window holds UI-only input"), F.ViewportIgnoresInput());
		const FVector Behind = F.Pawn->GetActorLocation();
		F.Frames(30);
		TestTrue(TEXT("an onboarding window stops the extra input"),
			F.Pawn->GetActorLocation().Equals(Behind, 1.0));
		F.SetOverlayInput(false);
		TestFalse(TEXT("closing the window restores game input"), F.ViewportIgnoresInput());
		F.Frames(30);
		TestTrue(TEXT("the extra input moves the body again once the window closes"),
			F.Pawn->GetActorLocation().X > Behind.X + 20.0);
	}

	FWoTLocalPlayerHooks::Detach(*F.Player, MovementHookOwner);
	const int32 DetachedSamples = Samples;
	MovementHookStand(F, FVector(-3000, 0, 0));
	const FVector Rest = F.Pawn->GetActorLocation();
	F.Frames(30);
	TestEqual(TEXT("a detached source is never sampled"), Samples, DetachedSamples);
	TestTrue(TEXT("after detach nothing moves the body"), F.Pawn->GetActorLocation().Equals(Rest, 1.0));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHookLookAssistSpec, "WoT.VR.Hooks.H3.LookAssistsKeepProviderPitch", MovementHookFlags)
bool FWoTHookLookAssistSpec::RunTest(const FString&)
{
	WoTLocalPlayerHooksTest::FPreferencesScope PreferencesScope;
	if (!PreferencesScope.Init(*this)) return false;
	WoTLocalPlayerHooksTest::FPlayerWorld F;
	if (!F.Init(*this)) return false;
	UWoTPreferencesSubsystem* Preferences =
		F.World->GetGameInstance()->GetSubsystem<UWoTPreferencesSubsystem>();
	if (!TestNotNull(TEXT("the real preferences subsystem"), Preferences)
		|| !TestNotNull(TEXT("floor"), F.SpawnBlock(FVector(0, 0, -10), FVector(6000, 6000, 10))))
	{
		return false;
	}
	MovementHookStand(F, FVector::ZeroVector);
	auto Configure = [&](bool bLookSpring, bool bStairLook)
	{
		FWoTGameSettings Settings = FWoTGameSettings::Defaults();
		Settings.bLookSpring = bLookSpring;
		Settings.bAutoSlopeLook = bStairLook;
		Settings.bAlwaysMouseLook = false;
		Preferences->SetGameSettingsForTesting(Settings);
	};
	auto Pitch = [&] { return static_cast<float>(FRotator::NormalizeAxis(F.PC->GetControlRotation().Pitch)); };

	// Flat: each assistant pulls a raised, still view back toward level.
	for (const bool bStairs : {false, true})
	{
		Configure(!bStairs, bStairs);
		F.PC->SetControlRotation(FRotator(20, 0, 0));
		F.Frames(15);
		TestTrue(bStairs ? TEXT("flat stair look levels the view") : TEXT("flat look spring levels the view"),
			Pitch() < 10.0f);
	}

	const FRotator Aim(20, 30, 0);
	FWoTLocalPlayerConsumer C;
	C.AimRotation = [&](FRotator& Out) { Out = Aim; return true; };
	TestTrue(TEXT("aim attached"), FWoTLocalPlayerHooks::Attach(*F.Player, MovementHookOwner, C));
	struct FAssistCase { bool bLookSpring; bool bStairLook; const TCHAR* Label; };
	const FAssistCase Cases[] = {
		{true, true, TEXT("look spring and stair look")},
		{true, false, TEXT("look spring")},
		{false, true, TEXT("stair look")}
	};
	for (const FAssistCase& Case : Cases)
	{
		Configure(Case.bLookSpring, Case.bStairLook);
		float WorstError = 0.0f;
		for (int32 Frame = 0; Frame < 30; ++Frame)
		{
			F.Frames(1);
			// The pawn's own tick last, so nothing after it could restore the pitch.
			F.Pawn->Tick(1.0f / 60.0f);
			WorstError = FMath::Max(WorstError, FMath::Abs(Pitch() - 20.0f));
		}
		TestTrue(FString::Printf(TEXT("%s never writes a still provider's pitch"), Case.Label),
			WorstError < 0.01f);
	}
	Configure(true, true);
	TestTrue(TEXT("center-view binding"), F.Binding(TEXT("CenterView")));
	float HeldError = 0.0f;
	for (int32 Frame = 0; Frame < 15; ++Frame)
	{
		F.Frames(1);
		F.Pawn->Tick(1.0f / 60.0f);
		HeldError = FMath::Max(HeldError, FMath::Abs(Pitch() - 20.0f));
	}
	TestTrue(TEXT("held center view never writes a provider's pitch"), HeldError < 0.01f);
	F.Binding(TEXT("CenterView"), IE_Released);
	const FWoTGameSettings& Saved = Preferences->GetGameSettings();
	TestTrue(TEXT("the provider leaves the saved look preferences as they were"),
		Saved.bLookSpring && Saved.bAutoSlopeLook && !Saved.bAlwaysMouseLook);

	FWoTLocalPlayerHooks::Detach(*F.Player, MovementHookOwner);
	F.PC->SetControlRotation(FRotator(20, 30, 0));
	F.Frames(15);
	TestTrue(TEXT("after detach the saved assistants level the view again"), Pitch() < 10.0f);
	return !HasAnyErrors();
}
#endif
