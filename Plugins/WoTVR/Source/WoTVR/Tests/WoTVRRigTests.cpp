#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "WoTVRTestFixture.h"
#include "Tests/WoTLocalPlayerHooksTestFixture.h"
#include "WoTCharacter.h"
#include "WoTPlayerController.h"
#include "WoTVRPlayerSubsystem.h"
#include "WoTVRRig.h"
#include "WoTVRSettings.h"
#include "WoTVRSyntheticSession.h"

#include "Components/CapsuleComponent.h"
#include "Engine/LocalPlayer.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"

// The rig's pose arithmetic and calibration (design sections 6 and 9).

namespace
{
FWoTVRHeadPose WoTVRRigPose(const FVector& Meters, const FRotator& Rotation)
{
	FWoTVRHeadPose Pose;
	Pose.PositionMeters = Meters;
	Pose.Orientation = Rotation.Quaternion();
	Pose.bValid = true;
	return Pose;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVRRigConversionSpec, "WoT.VR.Rig.ConversionOnce", WoTVRTest::Flags)
bool FWoTVRRigConversionSpec::RunTest(const FString&)
{
	FWoTVRRig Rig;
	Rig.Calibrate(WoTVRRigPose(FVector::ZeroVector, FRotator::ZeroRotator));
	TestEqual(TEXT("0.10 m forward is 5.25 units"),
		Rig.RelativeOffset(WoTVRRigPose(FVector(0.10, 0.0, 0.0), FRotator::ZeroRotator)).X, 5.25, 1e-9);
	TestEqual(TEXT("0.10 m up is 5.25 units"),
		Rig.RelativeOffset(WoTVRRigPose(FVector(0.0, 0.0, 0.10), FRotator::ZeroRotator)).Z, 5.25, 1e-9);
	Rig.HeadingYaw = 90.0;
	const FVector World = Rig.WorldOffset(WoTVRRigPose(FVector(0.10, 0.0, 0.0), FRotator::ZeroRotator));
	TestTrue(TEXT("the heading turns the converted offset, no second scale"),
		World.Equals(FVector(0.0, 5.25, 0.0), 1e-6));

	// Through a real local player: the rendered eye moves 5.25 units for a
	// 0.10 m step of the head, once.
	WoTVRTest::FScope Scope;
	WoTLocalPlayerHooksTest::FPlayerWorld F;
	if (!F.Init(*this)) return false;
	UWoTVRPlayerSubsystem* VR = WoTVRTest::Rig(F.Player);
	if (!TestNotNull(TEXT("rig"), VR)) return false;
	APlayerController* PC = F.PC;
	ACharacter* Pawn = F.Pawn;
	PC->SetControlRotation(FRotator::ZeroRotator);
	F.Frames(1);
	Scope.Session->SetHead(FVector::ZeroVector, FRotator::ZeroRotator);
	VR->RequestMode(EWoTVRMode::VR, TEXT("spec"));
	F.Frames(2);
	const FVector Eye = Pawn->GetPawnViewLocation();
	FMinimalViewInfo Final = WoTVRTest::FinalView(*F.Player);
	TestTrue(TEXT("at the neutral pose the rendered eye is the simulation eye (zero model offset)"),
		Final.Location.Equals(Eye, 0.01));
	Scope.Session->SetHead(FVector(0.10, 0.0, 0.0), FRotator::ZeroRotator);
	F.Frames(1);
	Final = WoTVRTest::FinalView(*F.Player);
	TestEqual(TEXT("a 0.10 m head step moves the rendered eye 5.25 units"),
		(Final.Location - Pawn->GetPawnViewLocation()).X, 5.25, 0.01);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVRRigPostureSpec, "WoT.VR.Rig.PostureKeepsCapsuleAndEye", WoTVRTest::Flags)
bool FWoTVRRigPostureSpec::RunTest(const FString&)
{
	WoTVRTest::FScope Scope;
	WoTLocalPlayerHooksTest::FPlayerWorld F;
	if (!F.Init(*this)) return false;
	UWoTVRPlayerSubsystem* VR = WoTVRTest::Rig(F.Player);
	if (!TestNotNull(TEXT("rig"), VR)) return false;
	ACharacter* Pawn = F.Pawn;
	const float Radius = Pawn->GetCapsuleComponent()->GetUnscaledCapsuleRadius();
	const float HalfHeight = Pawn->GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight();
	const float EyeHeight = Pawn->BaseEyeHeight;
	for (const EWoTVRPosture Posture : {EWoTVRPosture::Standing, EWoTVRPosture::Seated})
	{
		const FString Label = Posture == EWoTVRPosture::Seated ? TEXT("seated") : TEXT("standing");
		UWoTVRSettings::Get().Posture = Posture;
		// A seated user's head is about half a metre lower; the neutral pose they
		// confirm still maps to the avatar's standing simulation eye.
		const double HeadHeight = Posture == EWoTVRPosture::Seated ? -0.55 : 0.0;
		Scope.Session->SetHead(FVector(0.0, 0.0, HeadHeight), FRotator::ZeroRotator);
		VR->RequestMode(EWoTVRMode::VR, TEXT("spec"));
		F.Frames(2);
		VR->Recentre(TEXT("spec"));
		F.Frames(1);
		TestEqual(Label + TEXT(": capsule radius unchanged"),
			Pawn->GetCapsuleComponent()->GetUnscaledCapsuleRadius(), Radius);
		TestEqual(Label + TEXT(": capsule half-height unchanged"),
			Pawn->GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight(), HalfHeight);
		TestEqual(Label + TEXT(": simulation eye height unchanged"), Pawn->BaseEyeHeight, EyeHeight);
		const FMinimalViewInfo Final = WoTVRTest::FinalView(*F.Player);
		TestTrue(Label + TEXT(": the neutral rendered eye is the standing simulation eye"),
			Final.Location.Equals(Pawn->GetPawnViewLocation(), 0.01));
		VR->RequestMode(EWoTVRMode::Flat, TEXT("spec"));
		F.Frames(1);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVRRigSnapTurnSpec, "WoT.VR.Rig.SnapTurnAboutDisplacedHead", WoTVRTest::Flags)
bool FWoTVRRigSnapTurnSpec::RunTest(const FString&)
{
	// Pure: the head's world position stays put and its yaw changes once.
	FWoTVRRig Rig;
	Rig.Calibrate(WoTVRRigPose(FVector(0.3, -0.2, 1.1), FRotator(0.0, 15.0, 0.0)));
	Rig.HeadingYaw = 40.0;
	const FWoTVRHeadPose Displaced = WoTVRRigPose(FVector(0.45, 0.05, 1.05), FRotator(5.0, 35.0, 0.0));
	const FVector Before = Rig.WorldOffset(Displaced);
	const double YawBefore = Rig.WorldYaw(Displaced);
	Rig.Turn(Displaced, 30.0);
	TestTrue(TEXT("a snap keeps the displaced head's world position"),
		Rig.WorldOffset(Displaced).Equals(Before, 1e-6));
	TestEqual(TEXT("a snap adds its yaw once"),
		FMath::FindDeltaAngleDegrees(YawBefore, Rig.WorldYaw(Displaced)), 30.0, 1e-6);
	TestEqual(TEXT("a snap injects no pitch"), Rig.Aim(Displaced).Pitch, 5.0, 1e-6);

	// Through the real input: the turn key snaps once per press, re-arming only
	// after release, and the eyes do not orbit.
	WoTVRTest::FScope Scope;
	WoTLocalPlayerHooksTest::FPlayerWorld F;
	if (!F.Init(*this)) return false;
	UWoTVRPlayerSubsystem* VR = WoTVRTest::Rig(F.Player);
	if (!TestNotNull(TEXT("rig"), VR)) return false;
	APlayerController* PC = F.PC;
	ACharacter* Pawn = F.Pawn;
	Scope.Session->SetHead(FVector::ZeroVector, FRotator::ZeroRotator);
	VR->RequestMode(EWoTVRMode::VR, TEXT("spec"));
	F.Frames(2);
	Scope.Session->SetHead(FVector(0.0, 0.10, 0.0), FRotator::ZeroRotator);
	F.Frames(2);
	// Relative to the body's simulation eye, which follows the game's own
	// state-dependent eye height.
	const FVector EyeBefore = WoTVRTest::FinalView(*F.Player).Location - Pawn->GetPawnViewLocation();
	const double AimBefore = PC->GetControlRotation().Yaw;
	WoTVRTest::PressKey(*PC, EKeys::V);
	F.Frames(6);
	TestEqual(TEXT("one press, one snap"), VR->GetSnapTurnCount(), 1);
	TestEqual(TEXT("the aim turned by the snap angle once"),
		FMath::FindDeltaAngleDegrees(AimBefore, PC->GetControlRotation().Yaw), 30.0, 0.01);
	const FVector EyeAfter = WoTVRTest::FinalView(*F.Player).Location - Pawn->GetPawnViewLocation();
	TestTrue(FString::Printf(TEXT("the displaced eye kept its place beside the body (%s -> %s)"),
		*EyeBefore.ToString(), *EyeAfter.ToString()), EyeAfter.Equals(EyeBefore, 0.01));
	TestEqual(TEXT("the displaced eye is still 5.25 units out"), EyeAfter.Size(), 5.25, 0.01);
	WoTVRTest::ReleaseKey(*PC, EKeys::V);
	F.Frames(2);
	WoTVRTest::PressKey(*PC, EKeys::C);
	F.Frames(2);
	WoTVRTest::ReleaseKey(*PC, EKeys::C);
	F.Frames(2);
	TestEqual(TEXT("the other key snaps back"), VR->GetSnapTurnCount(), 2);
	TestEqual(TEXT("left and right cancel"),
		FMath::FindDeltaAngleDegrees(AimBefore, PC->GetControlRotation().Yaw), 0.0, 0.01);
	// The pad's right stick snaps past 0.7 and re-arms under 0.3.
	for (int32 Frame = 0; Frame < 4; ++Frame) { WoTVRTest::SetAxis(*PC, EKeys::Gamepad_RightX, 0.8f); F.Frames(1); }
	TestEqual(TEXT("the stick snaps once while held"), VR->GetSnapTurnCount(), 3);
	for (int32 Frame = 0; Frame < 3; ++Frame) { WoTVRTest::SetAxis(*PC, EKeys::Gamepad_RightX, 0.5f); F.Frames(1); }
	for (int32 Frame = 0; Frame < 3; ++Frame) { WoTVRTest::SetAxis(*PC, EKeys::Gamepad_RightX, 0.8f); F.Frames(1); }
	TestEqual(TEXT("no re-arm above 0.3"), VR->GetSnapTurnCount(), 3);
	for (int32 Frame = 0; Frame < 3; ++Frame) { WoTVRTest::SetAxis(*PC, EKeys::Gamepad_RightX, 0.1f); F.Frames(1); }
	for (int32 Frame = 0; Frame < 3; ++Frame) { WoTVRTest::SetAxis(*PC, EKeys::Gamepad_RightX, -0.9f); F.Frames(1); }
	TestEqual(TEXT("re-armed under 0.3, the next deflection snaps"), VR->GetSnapTurnCount(), 4);
	WoTVRTest::SetAxis(*PC, EKeys::Gamepad_RightX, 0.0f);
	F.Frames(1);
	TestEqual(TEXT("the stick's snaps cancel too"),
		FMath::FindDeltaAngleDegrees(AimBefore, PC->GetControlRotation().Yaw), 0.0, 0.01);
	TestTrue(TEXT("turning never moved the body"), Pawn->GetVelocity().IsNearlyZero(1.0));

	// Smooth: 90 degrees per second while held.
	UWoTVRSettings::Get().TurnMode = EWoTVRTurnMode::Smooth;
	const double SmoothStart = PC->GetControlRotation().Yaw;
	WoTVRTest::PressKey(*PC, EKeys::V);
	F.Frames(60);
	WoTVRTest::ReleaseKey(*PC, EKeys::V);
	F.Frames(2);
	const double Turned = FMath::FindDeltaAngleDegrees(SmoothStart, PC->GetControlRotation().Yaw);
	TestTrue(FString::Printf(TEXT("smooth turning held for a second turns about 90 degrees (%.1f)"), Turned),
		Turned > 80.0 && Turned < 95.0);
	TestEqual(TEXT("smooth turning never pitches"), PC->GetControlRotation().Pitch, 0.0, 0.01);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVRRigRecentreSpec, "WoT.VR.Rig.RecentreKeepsPawnAndAim", WoTVRTest::Flags)
bool FWoTVRRigRecentreSpec::RunTest(const FString&)
{
	// Pure.
	FWoTVRRig Rig;
	Rig.Calibrate(WoTVRRigPose(FVector::ZeroVector, FRotator::ZeroRotator));
	Rig.HeadingYaw = 120.0;
	const FWoTVRHeadPose Moved = WoTVRRigPose(FVector(0.2, -0.1, -0.15), FRotator(12.0, 33.0, 0.0));
	const FRotator AimBefore = Rig.Aim(Moved);
	Rig.Recentre(Moved);
	TestTrue(TEXT("recentre keeps the world aim"), Rig.Aim(Moved).Equals(AimBefore, 1e-6));
	TestTrue(TEXT("recentre puts the head over the anchor"), Rig.WorldOffset(Moved).IsNearlyZero(1e-9));

	// Through a real pawn.
	WoTVRTest::FScope Scope;
	WoTLocalPlayerHooksTest::FPlayerWorld F;
	if (!F.Init(*this)) return false;
	UWoTVRPlayerSubsystem* VR = WoTVRTest::Rig(F.Player);
	if (!TestNotNull(TEXT("rig"), VR)) return false;
	APlayerController* PC = F.PC;
	ACharacter* Pawn = F.Pawn;
	VR->RequestMode(EWoTVRMode::VR, TEXT("spec"));
	F.Frames(2);
	Scope.Session->SetHead(FVector(0.12, 0.06, -0.2), FRotator(-8.0, 50.0, 4.0));
	F.Frames(2);
	const FVector PawnBefore = Pawn->GetActorLocation();
	const FRotator WorldAim = PC->GetControlRotation();
	TestTrue(TEXT("recentre in VR"), VR->Recentre(TEXT("spec")));
	F.Frames(3);
	TestTrue(TEXT("recentre keeps the pawn where it was"), Pawn->GetActorLocation().Equals(PawnBefore, 0.01));
	TestTrue(TEXT("recentre keeps the world aim"), PC->GetControlRotation().Equals(WorldAim, 0.01));
	TestTrue(TEXT("recentre puts the rendered eye at the simulation eye"),
		WoTVRTest::FinalView(*F.Player).Location.Equals(Pawn->GetPawnViewLocation(), 0.01));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVRRigReferenceResetSpec, "WoT.VR.Rig.ReferenceResetNeedsRecentre", WoTVRTest::Flags)
bool FWoTVRRigReferenceResetSpec::RunTest(const FString&)
{
	WoTVRTest::FScope Scope;
	WoTLocalPlayerHooksTest::FPlayerWorld F;
	if (!F.Init(*this)) return false;
	UWoTVRPlayerSubsystem* VR = WoTVRTest::Rig(F.Player);
	if (!TestNotNull(TEXT("rig"), VR)) return false;
	APlayerController* PC = F.PC;
	ACharacter* Pawn = F.Pawn;
	WoTVRTest::FObserver Observer;
	Observer.Attach(*F.Player);
	VR->RequestMode(EWoTVRMode::VR, TEXT("spec"));
	F.Frames(2);
	const int32 Contexts = Observer.Count(EWoTLocalPlayerEvent::InputContextChanged);
	AddExpectedError(TEXT("reference space was reset"), EAutomationExpectedErrorFlags::Contains, 1);
	Scope.Session->ResetReferenceSpace();
	Scope.Session->SetHead(FVector(1.5, -0.7, 0.4), FRotator(0.0, 90.0, 0.0));
	F.Frames(2);
	TestTrue(TEXT("a reset invalidates the calibration"), VR->IsCalibrationInvalid());
	TestTrue(TEXT("the player is held still"), PC->IsMoveInputIgnored());
	TestEqual(TEXT("input released once"), Observer.Count(EWoTLocalPlayerEvent::InputContextChanged) - Contexts, 1);
	const FRotator Held = PC->GetControlRotation();
	Scope.Session->SetHead(FVector(1.5, -0.7, 0.4), FRotator(20.0, 120.0, 0.0));
	F.Frames(3);
	TestTrue(TEXT("the aim holds until the user recentres"), PC->GetControlRotation().Equals(Held, 0.01));
	TestTrue(TEXT("the stale calibration never moves the rendered eye off the body"),
		WoTVRTest::FinalView(*F.Player).Location.Equals(Pawn->GetPawnViewLocation(), 0.01));
	TestTrue(TEXT("recentre"), VR->Recentre(TEXT("spec")));
	F.Frames(2);
	TestFalse(TEXT("recentring validates the calibration"), VR->IsCalibrationInvalid());
	TestFalse(TEXT("play resumes"), PC->IsMoveInputIgnored());
	Scope.Session->SetHead(FVector(1.5, -0.7, 0.4), FRotator(0.0, 150.0, 0.0));
	F.Frames(2);
	TestEqual(TEXT("the head drives the aim again"),
		FMath::FindDeltaAngleDegrees(Held.Yaw, PC->GetControlRotation().Yaw), 30.0, 0.01);
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
