#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "WoTVRTestFixture.h"
#include "Tests/WoTLocalPlayerHooksTestFixture.h"
#include "WoTCharacter.h"
#include "WoTPlayerController.h"
#include "WoTVRPlayerSubsystem.h"
#include "WoTVRRig.h"
#include "WoTVRSyntheticSession.h"
#include "WoTZoneVolume.h"

#include "Camera/PlayerCameraManager.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"

// Special views (design section 6; PORT_NOTES "Optional VR play"): the
// remote-view artifact's camera and the citadel editor's orbit are not the
// player's head. Neither moves with the head's position, so the head never
// carries a remote camera through a wall. The remote camera looks where the
// head aims (the artifact follows the aim, as its casts do); the orbit keeps
// its own heading, the head looking around from it. Back in the body, the head
// is over the body again and the aim continues without a jump.

namespace
{
double WoTVRSpecialYawError(double A, double B)
{
	return FMath::Abs(FMath::FindDeltaAngleDegrees(A, B));
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVRSpecialRemoteSpec, "WoT.VR.SpecialViews.RemoteEyeKeepsItsPlace", WoTVRTest::Flags)
bool FWoTVRSpecialRemoteSpec::RunTest(const FString&)
{
	WoTVRTest::FScope Scope;
	WoTLocalPlayerHooksTest::FPlayerWorld F;
	if (!F.Init(*this)) return false;
	APlayerController* PC = F.PC;
	ACharacter* Pawn = F.Pawn;
	UWoTVRPlayerSubsystem* Rig = WoTVRTest::Rig(F.Player);
	UClass* EyeClass = WoTVRTest::GameClass(TEXT("WoTDistantEyeProjectile"));
	if (!TestNotNull(TEXT("rig"), Rig) || !TestNotNull(TEXT("remote-view artifact class"), EyeClass)) return false;
	Scope.Session->SetHead(FVector::ZeroVector, FRotator::ZeroRotator);
	Rig->RequestMode(EWoTVRMode::VR, TEXT("spec"));
	F.Frames(2);
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Params.Instigator = Pawn;
	AActor* Eye = F.World->SpawnActor<AActor>(EyeClass, FVector(800.0, 300.0, 600.0), FRotator::ZeroRotator, Params);
	if (!TestNotNull(TEXT("remote eye"), Eye)) return false;
	PC->SetViewTarget(Eye);
	F.Frames(2);
	const FVector EyeLocation = Eye->GetActorLocation();

	// The head steps and turns: the remote view stays at the eye and looks where
	// the head aims.
	Scope.Session->SetHead(FVector(0.10, 0.05, 0.02), FRotator(0.0, 25.0, 0.0));
	F.Frames(2);
	FMinimalViewInfo Final = WoTVRTest::FinalView(*F.Player);
	TestTrue(FString::Printf(TEXT("the remote view stays at the eye (%s, eye %s)"), *Final.Location.ToString(),
		*EyeLocation.ToString()), Final.Location.Equals(EyeLocation, 0.01));
	TestTrue(TEXT("the remote view looks where the head aims"),
		WoTVRSpecialYawError(Final.Rotation.Yaw, PC->GetControlRotation().Yaw) < 0.05);
	const double AimBefore = PC->GetControlRotation().Yaw;
	Scope.Session->SetHead(FVector(0.10, 0.05, 0.02), FRotator(0.0, 45.0, 0.0));
	F.Frames(2);
	Final = WoTVRTest::FinalView(*F.Player);
	TestEqual(TEXT("turning the head turns the aim once"),
		FMath::FindDeltaAngleDegrees(AimBefore, PC->GetControlRotation().Yaw), 20.0, 0.05);
	TestTrue(TEXT("and the remote view with it, once"),
		WoTVRSpecialYawError(Final.Rotation.Yaw, PC->GetControlRotation().Yaw) < 0.05);
	TestTrue(TEXT("the remote view never moves with the head"), Final.Location.Equals(EyeLocation, 0.01));
	TestEqual(TEXT("the remote view is never faded by the body's safety checks"), Rig->GetSafetyAlpha(), 0.0f, 0.0001f);

	// Back in the body: the head is over it again and the aim does not jump.
	const FRotator Aim = PC->GetControlRotation();
	PC->SetViewTarget(Pawn);
	F.Frames(2);
	TestTrue(TEXT("back in the body the aim continues"), PC->GetControlRotation().Equals(Aim, 0.05));
	const FVector Offset = WoTVRTest::FinalView(*F.Player).Location - Pawn->GetPawnViewLocation();
	TestTrue(FString::Printf(TEXT("back in the body the head is over it (%s)"), *Offset.ToString()),
		FVector2D(Offset.X, Offset.Y).IsNearlyZero(0.05));
	Eye->Destroy();
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVRSpecialOrbitSpec, "WoT.VR.SpecialViews.OrbitKeepsItsHeading", WoTVRTest::Flags)
bool FWoTVRSpecialOrbitSpec::RunTest(const FString&)
{
	WoTVRTest::FScope Scope;
	WoTLocalPlayerHooksTest::FPlayerWorld F;
	if (!F.Init(*this)) return false;
	AWoTPlayerController* PC = F.PC;
	ACharacter* Pawn = F.Pawn;
	UWoTVRPlayerSubsystem* Rig = WoTVRTest::Rig(F.Player);
	UClass* ZoneClass = WoTVRTest::GameClass(TEXT("WoTZoneVolume"));
	if (!TestNotNull(TEXT("rig"), Rig) || !TestNotNull(TEXT("zone class"), ZoneClass)) return false;
	static_cast<APlayerController*>(PC)->SetControlRotation(FRotator(0.0, 10.0, 0.0));
	F.Frames(1);
	Scope.Session->SetHead(FVector::ZeroVector, FRotator::ZeroRotator);
	Rig->RequestMode(EWoTVRMode::VR, TEXT("spec"));
	F.Frames(2);
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AActor* ZoneActor = F.World->SpawnActor<AActor>(ZoneClass, FVector(3000.0, 0.0, 500.0), FRotator::ZeroRotator, Params);
	if (!TestNotNull(TEXT("edit zone"), ZoneActor)) return false;
	AWoTZoneVolume* Zone = static_cast<AWoTZoneVolume*>(ZoneActor);
	Zone->EditViewAngle = FRotator(-45.0, 120.0, 0.0);
	Zone->EditViewDistance = 32.0f;

	// The orbit editor's camera, as entering the editor sets it up.
	const FRotator AimBefore = PC->GetControlRotation();
	PC->CitadelEditZone = Zone;
	PC->CitadelEditCameraMode = EWoTCitadelEditCameraMode::Orbit;
	PC->SetViewTarget(PC);
	Scope.Session->SetHead(FVector(0.08, -0.04, 0.0), FRotator::ZeroRotator);
	F.Frames(2);
	const FMinimalViewInfo& Orbit = PC->PlayerCameraManager->GetCameraCacheView();
	FMinimalViewInfo Final = WoTVRTest::FinalView(*F.Player);
	TestTrue(FString::Printf(TEXT("the orbit view stays at the orbit camera (%s, camera %s)"),
		*Final.Location.ToString(), *Orbit.Location.ToString()), Final.Location.Equals(Orbit.Location, 0.01));
	TestTrue(FString::Printf(TEXT("the orbit keeps its own heading (%.2f, camera %.2f)"), Final.Rotation.Yaw,
		Orbit.Rotation.Yaw), WoTVRSpecialYawError(Final.Rotation.Yaw, Orbit.Rotation.Yaw) < 0.05);
	TestEqual(TEXT("the orbit's tilt is not imposed on the headset: the head's own pitch"),
		Final.Rotation.Pitch, 0.0, 0.05);
	// The head looks around from the orbit's heading, once; it never moves the
	// orbit camera or the aim.
	Scope.Session->SetHead(FVector(0.08, -0.04, 0.0), FRotator(-30.0, 20.0, 0.0));
	F.Frames(2);
	Final = WoTVRTest::FinalView(*F.Player);
	TestTrue(TEXT("head look turns the orbit view once"),
		WoTVRSpecialYawError(Final.Rotation.Yaw, Orbit.Rotation.Yaw + 20.0) < 0.05);
	TestEqual(TEXT("head look pitches the orbit view"), Final.Rotation.Pitch, -30.0, 0.05);
	TestTrue(TEXT("the orbit view never moves with the head"), Final.Location.Equals(Orbit.Location, 0.01));
	TestTrue(TEXT("the orbit never moves the aim"), PC->GetControlRotation().Equals(AimBefore, 0.01));

	// Leaving the orbit: the aim continues where it was, the head over the body.
	PC->CitadelEditCameraMode = EWoTCitadelEditCameraMode::Inactive;
	PC->CitadelEditZone = nullptr;
	PC->SetViewTarget(Pawn);
	F.Frames(2);
	TestTrue(FString::Printf(TEXT("leaving the orbit keeps the aim (%s -> %s)"), *AimBefore.ToString(),
		*PC->GetControlRotation().ToString()), WoTVRSpecialYawError(PC->GetControlRotation().Yaw, AimBefore.Yaw) < 0.05);
	const FVector Offset = WoTVRTest::FinalView(*F.Player).Location - Pawn->GetPawnViewLocation();
	TestTrue(FString::Printf(TEXT("leaving the orbit puts the head over the body (%s)"), *Offset.ToString()),
		FVector2D(Offset.X, Offset.Y).IsNearlyZero(0.05));
	Scope.Session->SetHead(FVector(0.08, -0.04, 0.0), FRotator(0.0, 35.0, 0.0));
	F.Frames(2);
	TestEqual(TEXT("the head drives the aim again from there"),
		FMath::FindDeltaAngleDegrees(AimBefore.Yaw, PC->GetControlRotation().Yaw), 15.0, 0.05);
	ZoneActor->Destroy();
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
