// The retail view effects (walk bob, smoothed eye height, vertical shake and
// the additive flash fog) must reach the view the player actually sees. These
// specs drive the real camera manager update rather than calling a
// controller's CalcCamera directly, because the engine reaches that function
// only while the controller is its own view target.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../WoTCharacter.h"
#include "../WoTPlayerController.h"

#include "Camera/PlayerCameraManager.h"
#include "Misc/ScopeExit.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailCameraEffectsReachPawnViewSpec,
	"WoT.Camera.RetailEffectsReachPawnView",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTRetailCameraEffectsReachPawnViewSpec::RunTest(const FString& Parameters)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	if (!TestNotNull(TEXT("spec world"), World))
	{
		return false;
	}

	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTPlayerController* Controller = World->SpawnActor<AWoTPlayerController>(
		FVector::ZeroVector, FRotator::ZeroRotator, Spawn);
	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator, Spawn);
	if (!TestNotNull(TEXT("controller"), Controller) || !TestNotNull(TEXT("pawn"), Pawn))
	{
		return false;
	}
	Controller->SetAsLocalPlayerController();
	Controller->Possess(Pawn);
	APlayerCameraManager* Camera = Controller->PlayerCameraManager;
	if (!TestNotNull(TEXT("camera manager"), Camera))
	{
		return false;
	}
	// Normal play: possession makes the pawn the view target, so the engine
	// asks the pawn (not the controller) for the view.
	TestTrue(TEXT("the possessed pawn is the view target"),
		Controller->GetViewTarget() == Pawn);
	// A spec controller has no ULocalPlayer; without this the camera manager
	// skips its update entirely.
	Camera->bUseClientSideCameraUpdates = false;

	// A mid-stride bob and a smoothed eye still settling 5 units low (as after
	// stepping down); WoT.Preferences.GameSettings.Effects covers how the
	// retail tick produces them.
	const FVector Bob(0.0f, 1.5f, 2.0f);
	Controller->SetRetailViewStateForTest(Bob, Pawn->BaseEyeHeight - 5.0f);
	Controller->UE1ShakeVert = 4.0f;
	Controller->CurrentViewFlashFog = FVector(0.25f, 0.0f, 0.0f);

	Camera->UpdateCamera(0.0f);
	const FMinimalViewInfo& View = Camera->GetCameraCacheView();

	FVector EyeLocation;
	FRotator EyeRotation;
	Pawn->GetActorEyesViewPoint(EyeLocation, EyeRotation);
	const FVector Expected = EyeLocation + Bob
		+ FVector(0, 0, Controller->RetailEyeHeightForTest() - Pawn->BaseEyeHeight)
		+ FVector(0, 0, 4.0f);
	TestTrue(FString::Printf(TEXT("the seen view carries bob, eye height and shake (got %s, want %s)"),
		*View.Location.ToString(), *Expected.ToString()),
		View.Location.Equals(Expected, 0.01f));
	TestTrue(TEXT("the seen view carries the additive flash fog"),
		View.PostProcessSettings.bOverride_ColorOffset
		&& FVector(View.PostProcessSettings.ColorOffset).Equals(
			FVector(0.25f, 0.0f, 0.0f), 0.001f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailCameraEffectsSkipForeignPawnSpec,
	"WoT.Camera.RetailEffectsSkipUnpossessedPawn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTRetailCameraEffectsSkipForeignPawnSpec::RunTest(const FString& Parameters)
{
	// A pawn with no player controller (a remote player's proxy, an NPC) keeps
	// the plain eye view: the effects belong to the local player's own view.
	UWorld* World = WoTTest::CreateSpecWorld();
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTCharacter* Pawn = World ? World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator, Spawn) : nullptr;
	if (!TestNotNull(TEXT("pawn"), Pawn))
	{
		return false;
	}
	FMinimalViewInfo View;
	Pawn->CalcCamera(0.0f, View);
	FVector EyeLocation;
	FRotator EyeRotation;
	Pawn->GetActorEyesViewPoint(EyeLocation, EyeRotation);
	TestTrue(TEXT("an uncontrolled pawn's view is its plain eye point"),
		View.Location.Equals(EyeLocation, 0.01f));
	TestFalse(TEXT("an uncontrolled pawn's view carries no flash fog"),
		View.PostProcessSettings.bOverride_ColorOffset);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
