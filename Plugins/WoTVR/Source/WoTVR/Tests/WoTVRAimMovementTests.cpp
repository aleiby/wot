#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "WoTVRTestFixture.h"
#include "WoTCharacter.h"
#include "WoTVRPlayerSubsystem.h"
#include "WoTVRSyntheticSession.h"

#include "Camera/PlayerCameraManager.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"

// Aim and movement (design sections 8 and 9): head yaw and pitch, after the
// normal constraints, are the H2 aim that a real cast carries through the
// existing rotation RPC; ground movement is head-relative through the H3
// basis, keeping the last valid yaw near vertical; swimming keeps
// view-directed movement and held Jump rise; mouse and stick look are ignored.

namespace
{
bool WoTVRAimStartMatch(FAutomationTestBase& Test, WoTVRTest::FScope& Scope, WoTVRTest::FMatchWorld& F,
	UWoTVRPlayerSubsystem*& OutRig, ACharacter*& OutPawn)
{
	if (!F.Init(Test)) return false;
	OutPawn = Cast<ACharacter>(F.Restart(Test));
	OutRig = WoTVRTest::Rig(F.Player);
	if (!Test.TestNotNull(TEXT("pawn"), OutPawn) || !Test.TestNotNull(TEXT("rig"), OutRig)) return false;
	F.Frames(30);
	Scope.Session->SetHead(FVector::ZeroVector, FRotator::ZeroRotator);
	if (!Test.TestTrue(TEXT("switch to VR"), OutRig->RequestMode(EWoTVRMode::VR, TEXT("spec")))) return false;
	F.Frames(3);
	return Test.TestTrue(TEXT("VR entered"), OutRig->HasEnteredVR());
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVRHeadAimCastSpec, "WoT.VR.Aim.HeadAimDrivesRealCast", WoTVRTest::Flags)
bool FWoTVRHeadAimCastSpec::RunTest(const FString&)
{
	WoTVRTest::FScope Scope;
	WoTVRTest::FMatchWorld F;
	UWoTVRPlayerSubsystem* Rig = nullptr;
	ACharacter* Pawn = nullptr;
	if (!WoTVRAimStartMatch(*this, Scope, F, Rig, Pawn)) return false;
	WoTVRTest::FObserver Observer;
	Observer.Attach(*F.Player);
	// The dart launcher, the arena's default artifact.
	const FWoTSelectionSnapshot Selection = FWoTLocalPlayerHooks::Selection(*F.Player);
	for (const FWoTSelectionSlot& Slot : Selection.Slots)
	{
		if (Slot.bOwned && Slot.ClassName.ToString().Contains(TEXT("Dart")))
		{
			Observer.Command(EWoTLocalPlayerCommand::SelectGroup, Slot.Group);
			Observer.Command(EWoTLocalPlayerCommand::SelectSlot, Slot.Slot);
		}
	}
	F.Frames(70);
	const double Heading = F.PC->GetControlRotation().Yaw;
	Scope.Session->SetHead(FVector::ZeroVector, FRotator(18.0, 35.0, 0.0));
	F.Frames(2);
	const FRotator Aim = F.PC->GetControlRotation();
	TestEqual(TEXT("the aim's pitch is the head's"), Aim.Pitch, 18.0, 0.01);
	TestEqual(TEXT("the aim's yaw is the head's under the heading"),
		FMath::FindDeltaAngleDegrees(Heading, Aim.Yaw), 35.0, 0.01);
	UClass* ProjectileClass = WoTVRTest::GameClass(TEXT("WoTProjectile"));
	TSet<AActor*> Before;
	for (TActorIterator<AActor> It(F.World); It; ++It)
	{
		Before.Add(*It);
	}
	TestTrue(TEXT("the cast runs through the ordinary Fire command"),
		Observer.Command(EWoTLocalPlayerCommand::Fire));
	F.Frames(1);
	const AWoTCharacter* WoTPawn = static_cast<const AWoTCharacter*>(Pawn);
	TestTrue(TEXT("the cast reached the authority through the rotation RPC"), WoTPawn->IsPlayerAnimFireHeldForTesting());
	TestTrue(TEXT("the RPC's rotation is the head aim"), F.PC->GetControlRotation().Equals(Aim, 0.05));
	Observer.Command(EWoTLocalPlayerCommand::CeaseFire);
	AActor* Launched = nullptr;
	for (TActorIterator<AActor> It(F.World); It; ++It)
	{
		if (!Before.Contains(*It) && ProjectileClass && It->IsA(ProjectileClass))
		{
			Launched = *It;
		}
	}
	if (TestNotNull(TEXT("a real projectile was launched"), Launched))
	{
		const FVector Direction = Launched->GetVelocity().GetSafeNormal();
		const double Degrees = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(
			FVector::DotProduct(Direction, Aim.Vector()), -1.0, 1.0)));
		TestTrue(FString::Printf(TEXT("the projectile flies along the head aim (%.2f degrees off)"), Degrees),
			Degrees < 3.0);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVRAimConstraintsSpec, "WoT.VR.Aim.LimitsAndIgnoredLook", WoTVRTest::Flags)
bool FWoTVRAimConstraintsSpec::RunTest(const FString&)
{
	WoTVRTest::FScope Scope;
	WoTVRTest::FMatchWorld F;
	UWoTVRPlayerSubsystem* Rig = nullptr;
	ACharacter* Pawn = nullptr;
	if (!WoTVRAimStartMatch(*this, Scope, F, Rig, Pawn)) return false;
	// The view limits a mouse player has hold for the head too.
	Scope.Session->SetHead(FVector::ZeroVector, FRotator(89.99, 0.0, 0.0));
	F.Frames(2);
	TestTrue(TEXT("head pitch stays within the camera manager's pitch limit"),
		F.PC->GetControlRotation().Pitch <= F.PC->PlayerCameraManager->ViewPitchMax + 0.001);
	Scope.Session->SetHead(FVector::ZeroVector, FRotator(-89.99, 0.0, 0.0));
	F.Frames(2);
	TestTrue(TEXT("head pitch stays within the lower limit"),
		FRotator::NormalizeAxis(F.PC->GetControlRotation().Pitch) >= F.PC->PlayerCameraManager->ViewPitchMin - 0.001);
	// Mouse and stick look are ignored while the head owns the aim.
	Scope.Session->SetHead(FVector::ZeroVector, FRotator(0.0, 10.0, 0.0));
	F.Frames(2);
	const FRotator Held = F.PC->GetControlRotation();
	for (int32 Frame = 0; Frame < 10; ++Frame)
	{
		WoTVRTest::SetAxis(*F.PC, EKeys::MouseX, 25.0f);
		WoTVRTest::SetAxis(*F.PC, EKeys::MouseY, 25.0f);
		WoTVRTest::SetAxis(*F.PC, EKeys::Gamepad_RightY, 1.0f);
		F.Frames(1);
	}
	TestTrue(TEXT("mouse and stick look leave the head aim alone"), F.PC->GetControlRotation().Equals(Held, 0.01));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVRGroundMovementSpec, "WoT.VR.Movement.HeadRelativeGround", WoTVRTest::Flags)
bool FWoTVRGroundMovementSpec::RunTest(const FString&)
{
	WoTVRTest::FScope Scope;
	WoTVRTest::FMatchWorld F;
	UWoTVRPlayerSubsystem* Rig = nullptr;
	ACharacter* Pawn = nullptr;
	if (!WoTVRAimStartMatch(*this, Scope, F, Rig, Pawn)) return false;
	TestEqual(TEXT("walking on the floor"), static_cast<int32>(Pawn->GetCharacterMovement()->MovementMode.GetValue()),
		static_cast<int32>(MOVE_Walking));
	const double Heading = F.PC->GetControlRotation().Yaw;
	// The head looks 90 degrees right and slightly down; a gameplay turn leaves
	// the aim 45 degrees further round (not yet taken into the heading).
	Scope.Session->SetHead(FVector::ZeroVector, FRotator(-20.0, 90.0, 0.0));
	F.Frames(2);
	F.PC->SetControlRotation(F.PC->GetControlRotation() + FRotator(0.0, 45.0, 0.0));
	double Basis = 0.0;
	TestEqual(TEXT("the movement basis is the head's yaw, not the aim's"),
		FMath::FindDeltaAngleDegrees(Heading + 90.0, FWoTLocalPlayerHooks::GroundYaw(F.PC, Heading + 135.0)), 0.0, 0.01);
	WoTVRTest::PressKey(*F.PC, EKeys::W);
	F.Frames(4);
	const FVector Input = Pawn->GetLastMovementInputVector();
	TestTrue(TEXT("forward input arrives"), !Input.IsNearlyZero());
	Basis = Input.Rotation().Yaw;
	TestEqual(TEXT("walking forward follows the head's yaw"), FMath::FindDeltaAngleDegrees(Heading + 90.0, Basis), 0.0, 0.5);
	TestEqual(TEXT("ground input stays on the ground"), Input.Z, 0.0, 0.001);
	WoTVRTest::ReleaseKey(*F.PC, EKeys::W);
	F.Frames(2);
	// Near vertical the last valid yaw holds.
	Scope.Session->SetHead(FVector::ZeroVector, FRotator(-88.0, 170.0, 0.0));
	F.Frames(2);
	TestEqual(TEXT("looking straight down keeps the last valid yaw"),
		FMath::FindDeltaAngleDegrees(Heading + 90.0, FWoTLocalPlayerHooks::GroundYaw(F.PC, 0.0)), 0.0, 0.5);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVRSwimmingSpec, "WoT.VR.Movement.SwimmingUnchanged", WoTVRTest::Flags)
bool FWoTVRSwimmingSpec::RunTest(const FString&)
{
	WoTVRTest::FScope Scope;
	WoTVRTest::FMatchWorld F;
	UWoTVRPlayerSubsystem* Rig = nullptr;
	ACharacter* Pawn = nullptr;
	if (!WoTVRAimStartMatch(*this, Scope, F, Rig, Pawn)) return false;
	WoTVRTest::FObserver Observer;
	Observer.Attach(*F.Player);
	Pawn->SetActorLocation(Pawn->GetActorLocation() + FVector(0.0, 0.0, 600.0), false, nullptr,
		ETeleportType::TeleportPhysics);
	Scope.Session->SetHead(FVector::ZeroVector, FRotator(-30.0, 0.0, 0.0));
	F.Frames(2);
	// No water volume here, so the movement component would leave swimming after
	// each step; hold the mode for every frame the input is read in.
	WoTVRTest::PressKey(*F.PC, EKeys::W);
	for (int32 Frame = 0; Frame < 3; ++Frame)
	{
		Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Swimming);
		F.Frames(1);
	}
	const FVector Input = Pawn->GetLastMovementInputVector();
	const FVector View = F.PC->GetControlRotation().Vector();
	TestTrue(TEXT("swimming forward follows the full view, down included"),
		!Input.IsNearlyZero() && Input.GetSafeNormal().Equals(View, 0.01) && Input.Z < 0.0);
	WoTVRTest::ReleaseKey(*F.PC, EKeys::W);
	F.Frames(1);
	AWoTCharacter* WoTPawn = static_cast<AWoTCharacter*>(Pawn);
	Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Swimming);
	WoTPawn->SwimVerticalInputCountForTest = 0;
	Observer.Command(EWoTLocalPlayerCommand::Jump);
	for (int32 Frame = 0; Frame < 5; ++Frame)
	{
		Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Swimming);
		F.Frames(1);
	}
	TestEqual(TEXT("held Jump rises once per frame while swimming"), WoTPawn->SwimVerticalInputCountForTest, 5);
	Observer.Command(EWoTLocalPlayerCommand::StopJumping);
	F.Frames(2);
	TestEqual(TEXT("release stops the rise"), WoTPawn->SwimVerticalInputCountForTest, 5);
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
