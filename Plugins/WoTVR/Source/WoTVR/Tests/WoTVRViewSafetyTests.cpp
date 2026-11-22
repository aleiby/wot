#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "WoTVRTestFixture.h"
#include "Tests/WoTLocalPlayerHooksTestFixture.h"
#include "WoTCharacter.h"
#include "WoTPlayerController.h"
#include "WoTVRPlayerSubsystem.h"
#include "WoTVRSyntheticSession.h"
#include "WoTVRTuning.h"

#include "Camera/PlayerCameraManager.h"
#include "Engine/LocalPlayer.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Misc/ScopeExit.h"

// View safety (design section 6): the rendered eyes never pass through walls.
// The view goes black the moment the swept path from the capsule's eye to a
// rendered eye comes within the probe's reach of a surface, beyond the follow
// gap from the capsule, or when the head rises above the standing eye or sinks
// into the floor; it clears gradually once the hazard ends.

namespace
{
struct FWoTVRSafetyCVar
{
	IConsoleVariable* Variable = nullptr;
	float Saved = 0.0f;

	FWoTVRSafetyCVar(const TCHAR* Name, float Value)
		: Variable(IConsoleManager::Get().FindConsoleVariable(Name))
	{
		if (Variable)
		{
			Saved = Variable->GetFloat();
			Variable->Set(Value, ECVF_SetByCode);
		}
	}

	~FWoTVRSafetyCVar()
	{
		if (Variable)
		{
			Variable->Set(Saved, ECVF_SetByCode);
		}
	}
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVRViewSafetyWallSpec, "WoT.VR.ViewSafety.WallsGapRiseAndFloor", WoTVRTest::Flags)
bool FWoTVRViewSafetyWallSpec::RunTest(const FString&)
{
	WoTVRTest::FScope Scope;
	WoTLocalPlayerHooksTest::FPlayerWorld F;
	if (!F.Init(*this)) return false;
	APlayerController* PC = F.PC;
	ACharacter* Pawn = F.Pawn;
	UWoTVRPlayerSubsystem* Rig = WoTVRTest::Rig(F.Player);
	if (!TestNotNull(TEXT("rig"), Rig)) return false;
	PC->SetControlRotation(FRotator::ZeroRotator);
	F.Frames(1);
	Scope.Session->SetHead(FVector::ZeroVector, FRotator::ZeroRotator);
	Rig->RequestMode(EWoTVRMode::VR, TEXT("spec"));
	F.Frames(30);
	TestEqual(TEXT("a clear neutral view"), Rig->GetFadeAlpha(), 0.0f, 0.001f);
	TestTrue(TEXT("VR fades through the camera manager"), PC->PlayerCameraManager != nullptr);

	// A wall 3 units beyond the capsule, in front of the eye.
	const FVector Eye = Pawn->GetPawnViewLocation();
	const double WallFace = Eye.X + 20.0;
	if (!TestNotNull(TEXT("wall"), WoTVRTest::SpawnBlock(F.World, FVector(WallFace + 5.0, Eye.Y, Eye.Z),
		FVector(5.0, 200.0, 200.0)))) return false;

	{
		// Only the wall may fade the view while the head walks into it.
		FWoTVRSafetyCVar Gap(TEXT("wot.vr.FollowGap"), 60.0f);
		const double Near = WoTVRTuning::NearClip();
		int32 FirstBlack = INDEX_NONE;
		double FirstBlackEye = 0.0;
		for (int32 Step = 0; Step <= 45; ++Step)
		{
			Scope.Session->SetHead(FVector(Step * 0.01, 0.0, 0.0), FRotator::ZeroRotator);
			F.Frames(1);
			const FVector Rendered = WoTVRTest::FinalView(*F.Player).Location;
			const float Alpha = Rig->GetFadeAlpha();
			if (Alpha >= 1.0f && FirstBlack == INDEX_NONE)
			{
				FirstBlack = Step;
				FirstBlackEye = Rendered.X;
			}
			if (Rendered.X + Near >= WallFace)
			{
				TestEqual(FString::Printf(TEXT("step %d: the near plane at the wall is never shown"), Step),
					Alpha, 1.0f, 0.0001f);
			}
			if (FirstBlack != INDEX_NONE)
			{
				TestEqual(FString::Printf(TEXT("step %d: deeper into the wall stays black"), Step), Alpha, 1.0f, 0.0001f);
			}
			TestEqual(FString::Printf(TEXT("step %d: the camera manager carries the fade"), Step),
				PC->PlayerCameraManager->bEnableFading ? PC->PlayerCameraManager->FadeAmount : 0.0f, Alpha, 0.0001f);
		}
		if (TestTrue(TEXT("the view went black before reaching the wall"), FirstBlack != INDEX_NONE))
		{
			TestTrue(FString::Printf(TEXT("black while the near plane was still short of the wall (eye %.2f, face %.2f)"),
				FirstBlackEye, WallFace), FirstBlackEye + Near < WallFace);
		}
		Scope.Session->SetHead(FVector::ZeroVector, FRotator::ZeroRotator);
		F.Frames(1);
		TestTrue(TEXT("stepping back clears the view gradually, not at once"), Rig->GetFadeAlpha() > 0.0f);
		F.Frames(30);
		TestEqual(TEXT("stepping back clears the view"), Rig->GetFadeAlpha(), 0.0f, 0.001f);
	}

	// The follow gap (8 units), away from the wall.
	Scope.Session->SetHead(FVector(0.0, -5.0 / 52.5, 0.0), FRotator::ZeroRotator);
	F.Frames(30);
	TestEqual(TEXT("a lean within the gap stays clear"), Rig->GetFadeAlpha(), 0.0f, 0.001f);
	Scope.Session->SetHead(FVector(0.0, -9.0 / 52.5, 0.0), FRotator::ZeroRotator);
	F.Frames(1);
	TestEqual(TEXT("a lean past the gap is black"), Rig->GetFadeAlpha(), 1.0f, 0.0001f);

	// Rising above the standing eye, and sinking into the floor.
	Scope.Session->SetHead(FVector(0.0, 0.0, 0.05), FRotator::ZeroRotator);
	F.Frames(30);
	TestEqual(TEXT("rising within the margin stays clear"), Rig->GetFadeAlpha(), 0.0f, 0.001f);
	Scope.Session->SetHead(FVector(0.0, 0.0, 0.15), FRotator::ZeroRotator);
	F.Frames(1);
	TestEqual(TEXT("rising past the margin is black"), Rig->GetFadeAlpha(), 1.0f, 0.0001f);
	Scope.Session->SetHead(FVector(0.0, 0.0, -1.5), FRotator::ZeroRotator);
	F.Frames(30);
	TestEqual(TEXT("crouching low stays clear"), Rig->GetFadeAlpha(), 0.0f, 0.001f);
	Scope.Session->SetHead(FVector(0.0, 0.0, -1.62), FRotator::ZeroRotator);
	F.Frames(1);
	TestEqual(TEXT("sinking into the floor is black"), Rig->GetFadeAlpha(), 1.0f, 0.0001f);

	// Leaving VR removes the fade.
	Rig->RequestMode(EWoTVRMode::Flat, TEXT("spec"));
	F.Frames(1);
	TestFalse(TEXT("flat leaves no fade"), PC->PlayerCameraManager->bEnableFading);
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
