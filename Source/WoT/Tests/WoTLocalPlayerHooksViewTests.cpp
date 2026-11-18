#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS
#include "WoTLocalPlayerHooksTestFixture.h"
#include "../Settings/WoTGameSettings.h"
#include "../WoTCharacter.h"
#include "../WoTLocalPlayer.h"
#include "../WoTLocalPlayerHooks.h"
#include "../WoTPlayerController.h"
#include "../WoTPreferencesSubsystem.h"
#include "Components/CapsuleComponent.h"
#include "Components/InputComponent.h"
#include "Engine/DamageEvents.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "SceneView.h"
#include "Slate/SceneViewport.h"

namespace
{
constexpr auto ViewHookFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;
const FName ViewHookOwner(TEXT("ViewHookSpec"));

/** The largest distance between the pawn's camera view and its simulation eye
 * over Frames ticks of forward walking. */
double ViewHookWalkDeviation(WoTLocalPlayerHooksTest::FPlayerWorld& F, int32 Frames)
{
	double Worst = 0.0;
	for (int32 Frame = 0; Frame < Frames; ++Frame)
	{
		if (UInputComponent* Input = F.Pawn->InputComponent.Get())
		{
			for (FInputAxisBinding& Binding : Input->AxisBindings)
			{
				if (Binding.AxisName == TEXT("MoveForward"))
				{
					Binding.AxisDelegate.Execute(1.0f);
					break;
				}
			}
		}
		F.Frames(1);
		FMinimalViewInfo View;
		F.Pawn->CalcCamera(0.0f, View);
		FVector Eye;
		FRotator EyeRotation;
		F.Pawn->GetActorEyesViewPoint(Eye, EyeRotation);
		Worst = FMath::Max(Worst, (View.Location - Eye).Size());
	}
	return Worst;
}

double ViewHookDepth(const FMatrix& Projection, double Z)
{
	return (Projection.M[2][2] * Z + Projection.M[3][2]) / Z;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHookViewSpec, "WoT.VR.Hooks.H4.ViewOwnershipAndEffects", ViewHookFlags)
bool FWoTHookViewSpec::RunTest(const FString&)
{
	WoTLocalPlayerHooksTest::FPreferencesScope PreferencesScope;
	if (!PreferencesScope.Init(*this)) return false;
	WoTLocalPlayerHooksTest::FPlayerWorld F;
	if (!F.Init(*this)) return false;
	UWoTPreferencesSubsystem* Preferences =
		F.World->GetGameInstance()->GetSubsystem<UWoTPreferencesSubsystem>();
	if (!TestNotNull(TEXT("the real preferences subsystem"), Preferences)
		|| !TestNotNull(TEXT("floor"), F.SpawnBlock(FVector(0, 0, -10), FVector(8000, 8000, 10))))
	{
		return false;
	}
	FWoTGameSettings Settings = FWoTGameSettings::Defaults();
	Settings.ViewBob = 0.032f;
	Preferences->SetGameSettingsForTesting(Settings);
	const float HalfHeight = F.Pawn->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	F.Pawn->SetActorLocation(FVector(-6000, 0, HalfHeight + 1.0f), false, nullptr, ETeleportType::TeleportPhysics);
	F.Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
	F.PC->SetControlRotation(FRotator::ZeroRotator);
	F.Frames(5);
	TestTrue(TEXT("flat walking bobs the camera off the simulation eye"), ViewHookWalkDeviation(F, 30) > 0.1);

	FWoTLocalPlayerConsumer C;
	C.View.Emplace();
	C.View->bSuppressSyntheticMotion = true;
	C.View->Compose = [](const FWoTLocalPlayerContext&, const FMinimalViewInfo&, FVector& Location, FRotator& Rotation)
	{
		Location += FVector(1, 2, 3);
		Rotation = FRotator(12, 34, 0);
	};
	C.View->NearClip = [] { return 2.0f; };
	TestTrue(TEXT("view attached"), FWoTLocalPlayerHooks::Attach(*F.Player, ViewHookOwner, C));
	AddExpectedError(TEXT("exclusive local-player slot"), EAutomationExpectedErrorFlags::Contains, 1);
	TestFalse(TEXT("a second view owner is rejected"), FWoTLocalPlayerHooks::Attach(*F.Player, TEXT("Conflict"), C));

	F.PC->ClientShakeView(2.0f, 500.0f, 8.0f);
	F.PC->ClientFlash(-0.5f, FVector(250, 0, 0));
	F.PC->ClientAdjustGlow(0.0f, FVector(0, 120, 0));
	TestTrue(TEXT("the view owner removes walk bob, eye smoothing and vertical shake"),
		ViewHookWalkDeviation(F, 30) < 0.01);
	TestEqual(TEXT("the roll shake leaves the control rotation level"),
		static_cast<float>(F.PC->GetControlRotation().Roll), 0.0f, 0.001f);
	FMinimalViewInfo PawnView;
	F.Pawn->CalcCamera(0.0f, PawnView);
	TestTrue(TEXT("flash and fog still tint the view"), PawnView.PostProcessSettings.bOverride_ColorOffset
		&& !FVector(PawnView.PostProcessSettings.ColorOffset).IsNearlyZero());

	// Gameplay consequences are untouched.
	F.Pawn->MaxHealth = 100;
	F.Pawn->Health = 100;
	const int32 HealthBefore = F.Pawn->Health;
	F.Pawn->TakeDamage(10.0f, FDamageEvent(), nullptr, nullptr);
	TestTrue(TEXT("damage still applies"), F.Pawn->Health < HealthBefore);
	const float GroundZ = F.Pawn->GetActorLocation().Z;
	F.Pawn->LaunchCharacter(FVector(0, 0, 500), false, true);
	F.Frames(4);
	TestTrue(TEXT("an impulse still moves the body"), F.Pawn->GetActorLocation().Z > GroundZ + 1.0);
	const float YawBefore = F.PC->GetControlRotation().Yaw;
	F.PC->ClientSetForcedViewYaw(true, 16384.0f);   // 90 degrees per second
	F.Frames(10);
	F.PC->ClientSetForcedViewYaw(false, 0.0f);
	TestEqual(TEXT("a forced turn still turns the controller"),
		static_cast<float>(FMath::FindDeltaAngleDegrees(YawBefore, F.PC->GetControlRotation().Yaw)), 15.0f, 0.5f);
	F.PC->ClientSetCameraFOV(60.0f);
	F.Frames(1);

	// The owner composes the final view; gameplay field of view and tint remain.
	UWoTLocalPlayer* LocalPlayer = CastChecked<UWoTLocalPlayer>(F.Player);
	FMinimalViewInfo Final;
	LocalPlayer->GetViewPoint(Final);
	TestTrue(TEXT("the owner composes the final rotation"), Final.Rotation.Equals(FRotator(12, 34, 0), 0.01));
	TestEqual(TEXT("the owner's near plane is in the view"), Final.PerspectiveNearClipPlane, 2.0f);
	TestEqual(TEXT("a gameplay field of view survives composition"), Final.FOV, 60.0f, 0.01f);
	F.Viewport->LayoutPlayers();
	FSceneViewProjectionData Projection;
	if (TestTrue(TEXT("projection data"), LocalPlayer->GetProjectionData(F.Scene.Get(), Projection)))
	{
		TestEqual(TEXT("the projection starts at the owned near plane"),
			ViewHookDepth(Projection.ProjectionMatrix, 2.0), 1.0, 1e-6);
	}
	// A stereo device's own projection: asymmetric and infinite, or finite.
	FMatrix Stereo(FPlane(1.2, 0, 0, 0), FPlane(0, 1.5, 0, 0), FPlane(0.1, -0.05, 0, 1), FPlane(0, 0, 10, 0));
	FWoTLocalPlayerHooks::ApplyNearClip(*F.Player, Stereo);
	TestEqual(TEXT("an infinite stereo projection takes the owned near plane"), ViewHookDepth(Stereo, 2.0), 1.0, 1e-6);
	TestTrue(TEXT("each eye keeps its asymmetric frustum"),
		FMath::IsNearlyEqual(Stereo.M[2][0], 0.1) && FMath::IsNearlyEqual(Stereo.M[2][1], -0.05)
		&& FMath::IsNearlyEqual(Stereo.M[0][0], 1.2) && FMath::IsNearlyEqual(Stereo.M[1][1], 1.5));
	FMatrix Finite = FReversedZPerspectiveMatrix(PI / 4.0, PI / 4.0, 1.0, 1.0, 10.0, 5000.0);
	FWoTLocalPlayerHooks::ApplyNearClip(*F.Player, Finite);
	TestEqual(TEXT("a finite projection takes the owned near plane"), ViewHookDepth(Finite, 2.0), 1.0, 1e-6);
	TestEqual(TEXT("a finite projection keeps its far plane"), ViewHookDepth(Finite, 5000.0), 0.0, 1e-6);
	const FMatrix OrthoBefore = FReversedZOrthoMatrix(100.0, 100.0, 0.001, 0.0);
	FMatrix Ortho = OrthoBefore;
	FWoTLocalPlayerHooks::ApplyNearClip(*F.Player, Ortho);
	TestTrue(TEXT("an orthographic projection is left alone"), Ortho.Equals(OrthoBefore));

	FWoTLocalPlayerHooks::Detach(*F.Player, ViewHookOwner);
	F.Pawn->SetActorLocation(FVector(-6000, 0, HalfHeight + 1.0f), false, nullptr, ETeleportType::TeleportPhysics);
	F.Frames(10);
	TestTrue(TEXT("after detach the flat bob returns"), ViewHookWalkDeviation(F, 30) > 0.1);
	F.PC->ClientShakeView(1.0f, 500.0f, 8.0f);
	F.Frames(5);
	TestFalse(TEXT("after detach the roll shake rolls the view again"),
		FMath::IsNearlyZero(F.PC->GetControlRotation().Roll, 0.001));
	LocalPlayer->GetViewPoint(Final);
	TestFalse(TEXT("after detach the final view is the camera's own"), Final.Rotation.Equals(FRotator(12, 34, 0), 0.01));
	return !HasAnyErrors();
}
#endif
