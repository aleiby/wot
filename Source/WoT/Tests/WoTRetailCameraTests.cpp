// The retail view effects (walk bob, smoothed eye height, vertical shake and
// the additive flash fog) must reach the view the player actually sees. These
// specs drive the real camera manager update rather than calling a
// controller's CalcCamera directly, because the engine reaches that function
// only while the controller is its own view target.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../Projectiles/WoTAMAPearl.h"
#include "../WoTCharacter.h"
#include "../WoTPlayerController.h"

#include "Camera/PlayerCameraManager.h"
#include "GameFramework/CharacterMovementComponent.h"
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailCameraAMADomeTintSpec,
	"WoT.Camera.AMADomeTintKeepsNeutralScale",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTRetailCameraAMADomeTintSpec::RunTest(const FString& Parameters)
{
	// A pawn inside an anti-magic dome gets ClientAdjustGlow(4.0, ColorVect)
	// (AMAPearl.uc:497-501, ColorVect=(150,100,200) at :598). The glow scale
	// feeds the view flash, whose scale the original snaps to exactly 1 once
	// it passes 0.981, so it never brightens the view (FlashScale 1 is
	// "normal brightness", TriggeredViewFog.uc:17). Only the fog,
	// 0.001 * ColorVect, tints it. A scale left free to reach 1 + 4 multiplied
	// the whole view by five and washed it out pink and white. The world ticks
	// at 60 Hz, where a per-frame fog snap held the 0.1 green at zero and
	// turned the lavender tint magenta (PORT_NOTES "View flash at high frame
	// rates").
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
	// The spec world has no floor: hold the pawn inside the dome.
	Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	APlayerCameraManager* Camera = Controller->PlayerCameraManager;
	if (!TestNotNull(TEXT("camera manager"), Camera))
	{
		return false;
	}
	Camera->bUseClientSideCameraUpdates = false;
	const FVector DomeFog = FVector(150.0f, 100.0f, 200.0f) * 0.001f;

	// Entering the glow changes only the accumulated glow; the presented colour
	// scale must not jump before the view flash has run.
	Controller->ClientAdjustGlow(4.0f, FVector(150.0f, 100.0f, 200.0f));
	Camera->UpdateCamera(0.0f);
	TestTrue(FString::Printf(TEXT("the glow itself leaves the presented colour scale neutral (got %s)"),
		*Camera->ColorScale.ToString()),
		Camera->ColorScale.Equals(FVector::OneVector, 0.001f));
	Controller->ClientAdjustGlow(-4.0f, FVector(-150.0f, -100.0f, -200.0f));
	WoTTest::TickWorld(World, 0.5f);

	// The real path: a dome dropped on the pawn affects it through its own poll.
	AWoTAMAPearl* Pearl = World->SpawnActor<AWoTAMAPearl>(
		AWoTAMAPearl::StaticClass(), Pawn->GetActorLocation(),
		FRotator::ZeroRotator, Spawn);
	if (!TestNotNull(TEXT("dome"), Pearl))
	{
		return false;
	}
	Pearl->Go();
	WoTTest::TickWorld(World, 1.5f);
	TestEqual(TEXT("the dome applied its glow to the pawn inside"),
		Controller->ConstantGlowScale, 4.0f);
	TestEqual(TEXT("inside the dome the view flash scale holds at neutral"),
		Controller->CurrentViewFlashScale, 1.0f, 0.001f);
	TestTrue(FString::Printf(TEXT("inside the dome the view fog is 0.001 * ColorVect (got %s)"),
		*Controller->CurrentViewFlashFog.ToString()),
		Controller->CurrentViewFlashFog.Equals(DomeFog, 0.002f));
	Camera->UpdateCamera(0.0f);
	TestTrue(FString::Printf(TEXT("the presented colour scale stays neutral inside the dome (got %s)"),
		*Camera->ColorScale.ToString()),
		Camera->ColorScale.Equals(FVector::OneVector, 0.001f));
	const FMinimalViewInfo& View = Camera->GetCameraCacheView();
	TestTrue(TEXT("the seen view carries the dome fog"),
		View.PostProcessSettings.bOverride_ColorOffset
		&& FVector(View.PostProcessSettings.ColorOffset).Equals(DomeFog, 0.002f));

	// The dome holds for Duration, fades, and its teardown removes the tint.
	const float DomeLife = Pearl->Duration + Pearl->FadeTime + 1.0f;
	const TWeakObjectPtr<AWoTAMAPearl> WeakPearl(Pearl);
	WoTTest::TickWorld(World, DomeLife);
	TestFalse(TEXT("the dome is gone"), WeakPearl.IsValid());
	TestEqual(TEXT("leaving the dome removes the glow"),
		Controller->ConstantGlowScale, 0.0f, 0.001f);
	TestEqual(TEXT("the view flash scale is neutral after the dome"),
		Controller->CurrentViewFlashScale, 1.0f, 0.001f);
	TestTrue(TEXT("the view fog clears after the dome"),
		Controller->CurrentViewFlashFog.IsNearlyZero(0.002f));
	return true;
}

#endif // WITH_AUTOMATION_TESTS
