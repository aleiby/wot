#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "WoTVRTestFixture.h"
#include "WoTVRSpecHMD.h"
#include "Tests/WoTLocalPlayerHooksTestFixture.h"
#include "WoTCharacter.h"
#include "WoTPlayerController.h"
#include "WoTVRPlayerSubsystem.h"
#include "WoTVRRig.h"
#include "WoTVRSyntheticSession.h"
#include "WoTVRTuning.h"

#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "DefaultXRCamera.h"
#include "Engine/Engine.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/IConsoleManager.h"
#include "RenderGraphBuilder.h"
#include "RenderingThread.h"
#include "SceneView.h"
#include "Slate/SceneViewport.h"
#include "StereoRendering.h"

// The engine's own stereo pipeline (design section 6): with a stand-in display
// in place of OpenXR, the engine's XR camera places every eye, on the game
// thread (ULocalPlayer::GetProjectionData) and in the render-thread late update
// (FDefaultXRCamera::PreRenderView_RenderThread). Each eye must sit at the
// simulation eye plus the tracked head offset (turned by the heading) plus the
// eye's own offset from the head, with the head counted exactly once; every
// projection must start at the VR near plane; the front end's scene camera must
// keep its authored transform; and view safety must guard the eyes rendered.

namespace
{
/** What the engine renders for both eyes this frame. */
struct FWoTVRSpecEyes
{
	FVector Origin[2];
	FMatrix Projection[2];
	FVector LateOrigin[2];
	FMatrix LateProjection[2];
	FRotator LateRotation[2];
};

/** Builds both eyes' scene views as ULocalPlayer::CalcSceneView does
 * (GetProjectionData, the view point again, the stereo pass, the XR camera's
 * SetupView), then runs the XR camera's render-thread late update on them with
 * the display's late pose set LateDeltaMeters from the game thread's. */
bool WoTVRSpecRenderEyes(FAutomationTestBase& Test, ULocalPlayer& Player, FSceneViewport& Viewport,
	FWoTVRSpecHMD& HMD, const FVector& LateDeltaMeters, FWoTVRSpecEyes& Out)
{
	UWorld* World = Player.GetWorld();
	const TSharedPtr<FDefaultXRCamera, ESPMode::ThreadSafe> Camera = HMD.GetDefaultCamera();
	if (!Test.TestNotNull(TEXT("the engine's XR camera"), Camera.Get()) || !World
		|| !Test.TestTrue(TEXT("the engine renders stereo"), GEngine->IsStereoscopic3D(&Viewport)))
	{
		return false;
	}
	FSceneViewFamilyContext Family(FSceneViewFamily::ConstructionValues(&Viewport, World->Scene,
		FEngineShowFlags(ESFIM_Game)));
	TArray<FSceneView*> Views;
	for (int32 Eye = 0; Eye < 2; ++Eye)
	{
		FSceneViewInitOptions Init;
		if (!Test.TestTrue(FString::Printf(TEXT("eye %d projection data"), Eye),
			Player.GetProjectionData(&Viewport, Init, Eye)))
		{
			return false;
		}
		Out.Origin[Eye] = Init.ViewOrigin;
		Out.Projection[Eye] = Init.ProjectionMatrix;
		const FMinimalViewInfo ViewInfo = WoTVRTest::FinalView(Player);
		Init.ViewLocation = ViewInfo.Location;
		Init.ViewRotation = ViewInfo.Rotation;
		Init.FOV = ViewInfo.FOV;
		Init.DesiredFOV = ViewInfo.DesiredFOV;
		Init.ViewFamily = &Family;
		Init.StereoPass = GEngine->StereoRenderingDevice->GetViewPassForIndex(true, Eye);
		Init.StereoViewIndex = Eye;
		Init.WorldToMetersScale = World->GetWorldSettings()->WorldToMeters;
		Init.ViewActor = Player.PlayerController ? Player.PlayerController->GetViewTarget() : nullptr;
		FSceneView* View = new FSceneView(Init);
		Family.Views.Add(View);
		Views.Add(View);
		Camera->SetupView(Family, *View);
	}
	HMD.LateDeltaMeters = LateDeltaMeters;
	FWoTVRSpecHMD* Display = &HMD;
	FDefaultXRCamera* XRCamera = Camera.Get();
	ENQUEUE_RENDER_COMMAND(WoTVRSpecLateUpdate)([Display, XRCamera, &Views](FRHICommandListImmediate& RHICmdList)
	{
		Display->bLatePose = true;
		FRDGBuilder GraphBuilder(RHICmdList);
		for (FSceneView* View : Views)
		{
			XRCamera->PreRenderView_RenderThread(GraphBuilder, *View);
		}
		GraphBuilder.Execute();
		Display->bLatePose = false;
	});
	FlushRenderingCommands();
	HMD.LateDeltaMeters = FVector::ZeroVector;
	for (int32 Eye = 0; Eye < 2; ++Eye)
	{
		Out.LateOrigin[Eye] = Views[Eye]->ViewMatrices.GetViewOrigin();
		Out.LateProjection[Eye] = Views[Eye]->ViewMatrices.GetViewToClip();
		Out.LateRotation[Eye] = Views[Eye]->ViewRotation;
	}
	return true;
}

/** The eye's offset from the head the display reports (the engine's default
 * eye pose: half of a 0.064 m separation, left eye first), in world units. */
FVector WoTVRSpecEyeOffset(int32 Eye, double WorldToMeters)
{
	return FVector(0.0, (Eye == 0 ? -0.5 : 0.5) * 0.064 * WorldToMeters, 0.0);
}

/** The near plane of a reversed-Z perspective projection with an infinite far plane. */
double WoTVRSpecNear(const FMatrix& Projection)
{
	return Projection.M[3][2];
}

FQuat WoTVRSpecYaw(double Degrees)
{
	return FQuat(FVector::UpVector, FMath::DegreesToRadians(Degrees));
}

/** A tuning value for one spec, restored afterwards. */
struct FWoTVRSpecCVar
{
	IConsoleVariable* Variable = nullptr;
	float Saved = 0.0f;

	FWoTVRSpecCVar(const TCHAR* Name, float Value)
		: Variable(IConsoleManager::Get().FindConsoleVariable(Name))
	{
		if (Variable)
		{
			Saved = Variable->GetFloat();
			Variable->Set(Value, ECVF_SetByCode);
		}
	}

	~FWoTVRSpecCVar()
	{
		if (Variable)
		{
			Variable->Set(Saved, ECVF_SetByCode);
		}
	}
};

/** Checks both eyes, game thread and late update with no newer pose, against
 * the expected centre eye and head orientation. */
void WoTVRSpecExpectEyes(FAutomationTestBase& Test, const FString& Label, const FWoTVRSpecEyes& Eyes,
	const FVector& Centre, const FQuat& Orientation, double WorldToMeters)
{
	for (int32 Eye = 0; Eye < 2; ++Eye)
	{
		const FVector Expected = Centre + Orientation.RotateVector(WoTVRSpecEyeOffset(Eye, WorldToMeters));
		const TCHAR* Name = Eye == 0 ? TEXT("left") : TEXT("right");
		Test.TestTrue(FString::Printf(TEXT("%s: the %s eye holds the head once (%s, expected %s)"), *Label, Name,
			*Eyes.Origin[Eye].ToString(), *Expected.ToString()), Eyes.Origin[Eye].Equals(Expected, 0.01));
		Test.TestTrue(FString::Printf(TEXT("%s: the late update keeps the %s eye where the frame put it (%s)"),
			*Label, Name, *Eyes.LateOrigin[Eye].ToString()), Eyes.LateOrigin[Eye].Equals(Expected, 0.01));
		Test.TestEqual(FString::Printf(TEXT("%s: the %s eye's projection starts at the VR near plane"), *Label, Name),
			WoTVRSpecNear(Eyes.Projection[Eye]), static_cast<double>(WoTVRTuning::NearClip()), 1e-4);
		Test.TestEqual(FString::Printf(TEXT("%s: the late update's %s projection keeps the VR near plane"), *Label, Name),
			WoTVRSpecNear(Eyes.LateProjection[Eye]), static_cast<double>(WoTVRTuning::NearClip()), 1e-4);
	}
}
/** Checks that a newer pose in the late update moved both eyes by its change
 * alone, turned from tracking space as the view is. */
void WoTVRSpecExpectLateMove(FAutomationTestBase& Test, const FString& Label, const FWoTVRSpecEyes& Eyes,
	const FVector& Move)
{
	for (int32 Eye = 0; Eye < 2; ++Eye)
	{
		const FVector Moved = Eyes.LateOrigin[Eye] - Eyes.Origin[Eye];
		Test.TestTrue(FString::Printf(TEXT("%s: the late update moves eye %d by the newer pose's change once, turned as the view is (%s, expected %s)"),
			*Label, Eye, *Moved.ToString(), *Move.ToString()), Moved.Equals(Move, 0.01));
	}
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVRStereoEyesSpec, "WoT.VR.Stereo.EyesHoldTheHeadOnce", WoTVRTest::Flags)
bool FWoTVRStereoEyesSpec::RunTest(const FString&)
{
	WoTVRTest::FScope Scope;
	WoTLocalPlayerHooksTest::FPlayerWorld F;
	if (!F.Init(*this)) return false;
	WoTVRTest::FStereoScope Stereo(Scope.Session, F.Widget);
	UWoTVRPlayerSubsystem* Rig = WoTVRTest::Rig(F.Player);
	if (!TestNotNull(TEXT("rig"), Rig) || !TestNotNull(TEXT("scene viewport"), F.Scene.Get())) return false;
	APlayerController* PC = F.PC;
	ACharacter* Pawn = F.Pawn;
	const float FlatNear = GNearClippingPlane;
	PC->SetControlRotation(FRotator(0.0, 20.0, 0.0));
	F.Frames(1);
	Scope.Session->SetHead(FVector::ZeroVector, FRotator::ZeroRotator);
	if (!TestTrue(TEXT("switch to VR"), Rig->RequestMode(EWoTVRMode::VR, TEXT("spec")))) return false;
	F.Frames(3);
	if (!TestTrue(TEXT("VR entered"), Rig->HasEnteredVR())) return false;
	const double Scale = Stereo.HMD->GetWorldToMetersScale();
	TestEqual(TEXT("the engine runs at the VR scale"), Scale, WoTVR::UnitsPerMeter, 1e-6);

	// The head steps 0.10 m forward, 0.04 m right and 0.02 m down, turned 15
	// degrees: every eye moves with it once, turned by the 20 degree heading.
	const FVector Head(0.10, 0.04, -0.02);
	Scope.Session->SetHead(Head, FRotator(0.0, 15.0, 0.0));
	F.Frames(1);
	FWoTVRSpecEyes Eyes;
	if (!WoTVRSpecRenderEyes(*this, *F.Player, *F.Scene, *Stereo.HMD, FVector::ZeroVector, Eyes)) return false;
	const FVector Centre = Pawn->GetPawnViewLocation() + WoTVRSpecYaw(20.0).RotateVector(Head * WoTVR::UnitsPerMeter);
	WoTVRSpecExpectEyes(*this, TEXT("displaced head"), Eyes, Centre, WoTVRSpecYaw(35.0), Scale);

	// A newer pose in the late update moves each eye by the change alone,
	// turned by the 20 degree heading like the head itself.
	const FVector Late(0.02, 0.0, 0.0);
	if (!WoTVRSpecRenderEyes(*this, *F.Player, *F.Scene, *Stereo.HMD, Late, Eyes)) return false;
	WoTVRSpecExpectLateMove(*this, TEXT("displaced head"), Eyes, WoTVRSpecYaw(20.0).RotateVector(Late * Scale));

	// A snap turn about the displaced head: the eyes stay where they were (no
	// orbit) and turn 30 degrees, still holding the head once.
	WoTVRTest::PressKey(*PC, EKeys::V);
	F.Frames(2);
	WoTVRTest::ReleaseKey(*PC, EKeys::V);
	F.Frames(1);
	TestEqual(TEXT("one snap"), Rig->GetSnapTurnCount(), 1);
	if (!WoTVRSpecRenderEyes(*this, *F.Player, *F.Scene, *Stereo.HMD, FVector::ZeroVector, Eyes)) return false;
	WoTVRSpecExpectEyes(*this, TEXT("after a snap turn"), Eyes, Centre, WoTVRSpecYaw(65.0), Scale);
	if (!WoTVRSpecRenderEyes(*this, *F.Player, *F.Scene, *Stereo.HMD, Late, Eyes)) return false;
	WoTVRSpecExpectLateMove(*this, TEXT("after a snap turn"), Eyes, WoTVRSpecYaw(50.0).RotateVector(Late * Scale));

	// A teleport with an exit heading folds the gameplay yaw into the heading
	// and puts the head back over the body (its height calibration kept).
	Pawn->TeleportTo(Pawn->GetActorLocation() + FVector(1500.0, 0.0, 0.0), Pawn->GetActorRotation(), false, true);
	PC->SetControlRotation(PC->GetControlRotation() + FRotator(0.0, 90.0, 0.0));
	F.Frames(1);
	const double Aim = PC->GetControlRotation().Yaw;
	TestEqual(TEXT("the teleport's exit heading reached the aim"), FMath::FindDeltaAngleDegrees(65.0, Aim), 90.0, 0.05);
	if (!WoTVRSpecRenderEyes(*this, *F.Player, *F.Scene, *Stereo.HMD, FVector::ZeroVector, Eyes)) return false;
	const FVector Folded = Pawn->GetPawnViewLocation() + FVector(0.0, 0.0, Head.Z * WoTVR::UnitsPerMeter);
	WoTVRSpecExpectEyes(*this, TEXT("after a teleport's yaw fold"), Eyes, Folded, WoTVRSpecYaw(Aim), Scale);
	if (!WoTVRSpecRenderEyes(*this, *F.Player, *F.Scene, *Stereo.HMD, Late, Eyes)) return false;
	WoTVRSpecExpectLateMove(*this, TEXT("after a teleport's yaw fold"), Eyes,
		WoTVRSpecYaw(Aim - 15.0).RotateVector(Late * Scale));

	// Flat again: the engine's own near plane comes back.
	TestTrue(TEXT("switch to flat"), Rig->RequestMode(EWoTVRMode::Flat, TEXT("spec")));
	F.Frames(1);
	FlushRenderingCommands();
	TestEqual(TEXT("flat restores the engine's near plane"), GNearClippingPlane, FlatNear);
	TestEqual(TEXT("flat restores the render thread's near plane"), GNearClippingPlane_RenderThread, FlatNear);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVRStereoFrontEndSpec, "WoT.VR.Stereo.FrontEndSceneCamera", WoTVRTest::Flags)
bool FWoTVRStereoFrontEndSpec::RunTest(const FString&)
{
	WoTVRTest::FScope Scope;
	WoTVRTest::FMatchWorld F;
	if (!F.Init(*this, TEXT("WoTFrontEndGameMode"), /*bWithController*/ false)) return false;
	WoTVRTest::FStereoScope Stereo(Scope.Session, F.Widget);
	UClass* FrontEndClass = WoTVRTest::GameClass(TEXT("WoTFrontEndPlayerController"));
	if (!TestNotNull(TEXT("front-end controller class"), FrontEndClass)) return false;
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	APlayerController* FrontEnd = F.World->SpawnActor<APlayerController>(FrontEndClass, Params);
	if (!TestNotNull(TEXT("real front-end controller"), FrontEnd)) return false;
	FrontEnd->SetPlayer(F.Player);
	// The front end's real scene, committed by the scene materializer with the
	// controller's view on it: its own view camera at the authored eye.
	const FVector AuthoredLocation(125.5, -64.25, 42.0);
	const FRotator AuthoredRotation(11.25, 73.5, 0.0);
	WoTLocalPlayerHooksTest::FFrontEndScene Scene;
	if (!Scene.Commit(*this, *F.World, *FrontEnd, AuthoredLocation, AuthoredRotation)) return false;
	AActor* Root = Scene.Root;
	UCameraComponent* Camera = Scene.Camera;
	F.Frames(2);
	UWoTVRPlayerSubsystem* Rig = WoTVRTest::Rig(F.Player);
	if (!TestNotNull(TEXT("rig"), Rig)) return false;
	const FTransform Authored = Camera->GetRelativeTransform();
	const FMinimalViewInfo FlatView = WoTVRTest::FinalView(*F.Player);
	TestTrue(TEXT("flat shows the authored eye"), FlatView.Location.Equals(AuthoredLocation, 0.01));

	Scope.Session->SetHead(FVector(0.3, -0.2, 0.1), FRotator::ZeroRotator);
	if (!TestTrue(TEXT("switch to VR in the front end"), Rig->RequestMode(EWoTVRMode::VR, TEXT("spec")))) return false;
	F.Frames(3);
	if (!TestTrue(TEXT("VR entered without a pawn"), Rig->HasEnteredVR())) return false;
	const double Scale = Stereo.HMD->GetWorldToMetersScale();
	const FVector Head(0.10, 0.03, 0.0);
	Scope.Session->SetHead(FVector(0.3, -0.2, 0.1) + Head, FRotator(0.0, 10.0, 0.0));
	F.Frames(2);
	TestTrue(FString::Printf(TEXT("the scene camera keeps its authored transform under the headset (%s)"),
		*Camera->GetRelativeTransform().ToString()), Camera->GetRelativeTransform().Equals(Authored, 0.01));
	FWoTVRSpecEyes Eyes;
	if (!WoTVRSpecRenderEyes(*this, *F.Player, *F.Scene, *Stereo.HMD, FVector::ZeroVector, Eyes)) return false;
	// The heading took the camera's yaw at entry; the camera's pitch is never
	// imposed on the head.
	const FVector Centre = AuthoredLocation + WoTVRSpecYaw(73.5).RotateVector(Head * WoTVR::UnitsPerMeter);
	WoTVRSpecExpectEyes(*this, TEXT("front end"), Eyes, Centre, WoTVRSpecYaw(83.5), Scale);
	// The late update still runs in the front end and moves the eyes by the
	// newer pose's change, turned by the heading like the head itself.
	const FVector Late(0.02, 0.0, 0.0);
	if (!WoTVRSpecRenderEyes(*this, *F.Player, *F.Scene, *Stereo.HMD, Late, Eyes)) return false;
	const FVector LateMove = WoTVRSpecYaw(73.5).RotateVector(Late * Scale);
	for (int32 Eye = 0; Eye < 2; ++Eye)
	{
		TestTrue(FString::Printf(TEXT("the front end's late update moves eye %d with the head (%s)"), Eye,
			*(Eyes.LateOrigin[Eye] - Eyes.Origin[Eye]).ToString()),
			(Eyes.LateOrigin[Eye] - Eyes.Origin[Eye]).Equals(LateMove, 0.01));
	}

	// Leaving the scene camera for a view without one (the pawnless controller
	// view): the late update keeps running there too.
	FrontEnd->SetViewTarget(FrontEnd);
	F.Frames(2);
	if (!WoTVRSpecRenderEyes(*this, *F.Player, *F.Scene, *Stereo.HMD, Late, Eyes)) return false;
	WoTVRSpecExpectLateMove(*this, TEXT("the pawnless view after the scene camera"), Eyes, LateMove);
	FrontEnd->SetViewTarget(Root);
	F.Frames(2);

	// Flat again: the front end is exactly as it was.
	TestTrue(TEXT("switch to flat in the front end"), Rig->RequestMode(EWoTVRMode::Flat, TEXT("spec")));
	F.Frames(2);
	TestTrue(TEXT("flat leaves the scene camera's authored transform"),
		Camera->GetRelativeTransform().Equals(Authored, 0.01));
	const FMinimalViewInfo Back = WoTVRTest::FinalView(*F.Player);
	TestTrue(TEXT("the flat front end renders from the authored eye again"),
		Back.Location.Equals(FlatView.Location, 0.01) && Back.Rotation.Equals(FlatView.Rotation, 0.01));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVRStereoSafetySpec, "WoT.VR.Stereo.SafetyGuardsRenderedEyes", WoTVRTest::Flags)
bool FWoTVRStereoSafetySpec::RunTest(const FString&)
{
	WoTVRTest::FScope Scope;
	WoTLocalPlayerHooksTest::FPlayerWorld F;
	if (!F.Init(*this)) return false;
	WoTVRTest::FStereoScope Stereo(Scope.Session, F.Widget);
	UWoTVRPlayerSubsystem* Rig = WoTVRTest::Rig(F.Player);
	if (!TestNotNull(TEXT("rig"), Rig)) return false;
	APlayerController* PC = F.PC;
	ACharacter* Pawn = F.Pawn;
	PC->SetControlRotation(FRotator::ZeroRotator);
	F.Frames(1);
	Scope.Session->SetHead(FVector::ZeroVector, FRotator::ZeroRotator);
	Rig->RequestMode(EWoTVRMode::VR, TEXT("spec"));
	F.Frames(30);
	const FVector Eye = Pawn->GetPawnViewLocation();
	const double WallFace = Eye.X + 20.0;
	if (!TestNotNull(TEXT("wall"), WoTVRTest::SpawnBlock(F.World, FVector(WallFace + 5.0, Eye.Y, Eye.Z),
		FVector(5.0, 200.0, 200.0)))) return false;
	// Only the wall may fade the view while the head walks into it.
	FWoTVRSpecCVar Gap(TEXT("wot.vr.FollowGap"), 60.0f);
	const double Near = WoTVRTuning::NearClip();
	int32 Checked = 0;
	for (int32 Step = 0; Step <= 45; ++Step)
	{
		Scope.Session->SetHead(FVector(Step * 0.01, 0.0, 0.0), FRotator::ZeroRotator);
		F.Frames(1);
		FWoTVRSpecEyes Eyes;
		if (!WoTVRSpecRenderEyes(*this, *F.Player, *F.Scene, *Stereo.HMD, FVector::ZeroVector, Eyes)) return false;
		for (int32 Index = 0; Index < 2; ++Index)
		{
			if (Eyes.Origin[Index].X + Near >= WallFace)
			{
				++Checked;
				TestEqual(FString::Printf(TEXT("step %d: a rendered eye whose near plane reaches the wall is never shown (eye %.2f, face %.2f)"),
					Step, Eyes.Origin[Index].X, WallFace), Rig->GetFadeAlpha(), 1.0f, 0.0001f);
			}
		}
	}
	TestTrue(TEXT("the walk reached the wall"), Checked > 0);
	Rig->RequestMode(EWoTVRMode::Flat, TEXT("spec"));
	F.Frames(1);
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
