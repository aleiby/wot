#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "WoTVRTestFixture.h"
#include "Tests/WoTLocalPlayerHooksTestFixture.h"
#include "WoTCharacter.h"
#include "WoTPlayerController.h"
#include "WoTVRPlayerSubsystem.h"
#include "WoTVRSyntheticSession.h"
#include "WoTVRTuning.h"

#include "Engine/Engine.h"
#include "Engine/LocalPlayer.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"

// The session around a switch or a loss (design section 14), as the engine's
// OpenXR plugin runs it: a stopped session takes a few frames to end and
// writes its own frame-rate cap on the way; a quick switch back to VR must wait
// for it; and a session the runtime ends must come back by itself once the
// runtime offers one again, with the player held still meanwhile and never
// handed to mouse aim.

namespace
{
/** The engine's frame-rate cap and smoothing, restored after a spec. */
struct FWoTVRFrameRateScope
{
	float MaxFPS = 0.0f;
	bool bForceDisableSmoothing = false;

	FWoTVRFrameRateScope()
		: MaxFPS(GEngine->GetMaxFPS())
		, bForceDisableSmoothing(GEngine->bForceDisableFrameRateSmoothing)
	{
	}

	~FWoTVRFrameRateScope()
	{
		if (GEngine->GetMaxFPS() != MaxFPS)
		{
			GEngine->SetMaxFPS(MaxFPS);
		}
		GEngine->bForceDisableFrameRateSmoothing = bForceDisableSmoothing;
	}
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVRRecoveryFrameRateSpec, "WoT.VR.Recovery.FrameRateCapSurvivesSessionEnd", WoTVRTest::Flags)
bool FWoTVRRecoveryFrameRateSpec::RunTest(const FString&)
{
	FWoTVRFrameRateScope FrameRate;
	WoTVRTest::FScope Scope;
	Scope.Session->bEmulateOpenXRFrameRate = true;
	Scope.Session->EndingFrames = 4;
	WoTLocalPlayerHooksTest::FPlayerWorld F;
	if (!F.Init(*this)) return false;
	UWoTVRPlayerSubsystem* Rig = WoTVRTest::Rig(F.Player);
	if (!TestNotNull(TEXT("rig"), Rig)) return false;
	GEngine->SetMaxFPS(72.0f);
	GEngine->bForceDisableFrameRateSmoothing = false;
	for (int32 Cycle = 0; Cycle < 2; ++Cycle)
	{
		const FString Label = FString::Printf(TEXT("cycle %d"), Cycle + 1);
		TestTrue(Label + TEXT(": switch to VR"), Rig->RequestMode(EWoTVRMode::VR, TEXT("spec")));
		F.Frames(3);
		TestTrue(Label + TEXT(": VR entered"), Rig->HasEnteredVR());
		TestEqual(Label + TEXT(": the runtime uncaps the rate in VR"), GEngine->GetMaxFPS(), 0.0f);
		TestTrue(Label + TEXT(": switch to flat"), Rig->RequestMode(EWoTVRMode::Flat, TEXT("spec")));
		// The session stops (OpenXR caps the rate at its paused idle rate) and
		// then exits (OpenXR uncaps it) a few frames later. The next frame is
		// timed with the cap a frame ends on, so every frame ends on the
		// player's.
		int32 WrongFrames = 0;
		for (int32 Frame = 0; Frame < 8; ++Frame)
		{
			F.Frames(1);
			WrongFrames += GEngine->GetMaxFPS() != 72.0f ? 1 : 0;
		}
		TestEqual(Label + TEXT(": no frame ends on the runtime's cap"), WrongFrames, 0);
		TestFalse(Label + TEXT(": the old session has ended"), Scope.Session->IsEnding());
		TestEqual(Label + TEXT(": the player's frame-rate cap survives the session's end"), GEngine->GetMaxFPS(), 72.0f);
		TestFalse(Label + TEXT(": frame-rate smoothing is the player's again"), GEngine->bForceDisableFrameRateSmoothing);
		F.Frames(5);
		TestEqual(Label + TEXT(": and stays"), GEngine->GetMaxFPS(), 72.0f);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVRRecoveryReentrySpec, "WoT.VR.Recovery.QuickReentryWaitsForTheOldSession", WoTVRTest::Flags)
bool FWoTVRRecoveryReentrySpec::RunTest(const FString&)
{
	WoTVRTest::FScope Scope;
	Scope.Session->EndingFrames = 5;
	WoTLocalPlayerHooksTest::FPlayerWorld F;
	if (!F.Init(*this)) return false;
	UWoTVRPlayerSubsystem* Rig = WoTVRTest::Rig(F.Player);
	if (!TestNotNull(TEXT("rig"), Rig)) return false;
	for (int32 Cycle = 0; Cycle < 2; ++Cycle)
	{
		const FString Label = FString::Printf(TEXT("cycle %d"), Cycle + 1);
		TestTrue(Label + TEXT(": switch to VR"), Rig->RequestMode(EWoTVRMode::VR, TEXT("spec")));
		F.Frames(3);
		TestTrue(Label + TEXT(": VR entered"), Rig->HasEnteredVR());
		TestTrue(Label + TEXT(": switch to flat"), Rig->RequestMode(EWoTVRMode::Flat, TEXT("spec")));
		F.Frames(1);
		TestTrue(Label + TEXT(": the old session is still ending"), Scope.Session->IsEnding());
		// Straight back: the runtime refuses a second session until the old one
		// has ended, so the switch waits for it instead of failing.
		TestTrue(Label + TEXT(": the switch back to VR is accepted"), Rig->RequestMode(EWoTVRMode::VR, TEXT("spec")));
		TestEqual(Label + TEXT(": in VR mode while the old session ends"), Rig->GetMode(), EWoTVRMode::VR);
		TestTrue(Label + TEXT(": the player holds still while VR starts"), F.PC->IsMoveInputIgnored());
		F.Frames(10);
		TestFalse(Label + TEXT(": the old session ended"), Scope.Session->IsEnding());
		TestTrue(Label + TEXT(": stereo started again"), Scope.Session->IsStereoEnabled());
		TestTrue(Label + TEXT(": VR entered once the old session ended"), Rig->HasEnteredVR());
		TestFalse(Label + TEXT(": play resumes"), F.PC->IsMoveInputIgnored());
		TestTrue(Label + TEXT(": the head owns the aim"), FWoTLocalPlayerHooks::OwnsAim(F.PC));
		TestTrue(Label + TEXT(": back to flat"), Rig->RequestMode(EWoTVRMode::Flat, TEXT("spec")));
		F.Frames(8);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVRRecoverySessionSpec, "WoT.VR.Recovery.RecoversWhenTheRuntimeOffersASession", WoTVRTest::Flags)
bool FWoTVRRecoverySessionSpec::RunTest(const FString&)
{
	WoTVRTest::FScope Scope;
	WoTVRTest::FMatchWorld F;
	if (!F.Init(*this)) return false;
	ACharacter* Pawn = Cast<ACharacter>(F.Restart(*this));
	UWoTVRPlayerSubsystem* Rig = WoTVRTest::Rig(F.Player);
	if (!TestNotNull(TEXT("pawn"), Pawn) || !TestNotNull(TEXT("rig"), Rig)) return false;
	F.Frames(20);
	Scope.Session->SetHead(FVector::ZeroVector, FRotator::ZeroRotator);
	if (!TestTrue(TEXT("switch to VR"), Rig->RequestMode(EWoTVRMode::VR, TEXT("spec")))) return false;
	F.Frames(3);
	AddExpectedError(TEXT("holding still with input released"), EAutomationExpectedErrorFlags::Contains, 0);
	AddExpectedError(TEXT("The VR runtime ended the session"), EAutomationExpectedErrorFlags::Contains, 0);

	// The headset is lost: OpenXR destroys the session and the system with it,
	// and leaves stereo on.
	const int32 Switches = Rig->GetSwitchCount();
	Scope.Session->EndSessionByRuntime(/*bLoseSystem*/ true);
	F.Frames(5);
	TestTrue(TEXT("headset lost: the player holds still"), Rig->IsHoldingStill() && F.PC->IsMoveInputIgnored());
	const FRotator Held = F.PC->GetControlRotation();
	for (int32 Frame = 0; Frame < 10; ++Frame)
	{
		WoTVRTest::SetAxis(*F.PC, EKeys::MouseX, 25.0f);
		F.Frames(1);
	}
	TestTrue(TEXT("headset lost: the aim is never handed to the mouse"), F.PC->GetControlRotation().Equals(Held, 0.01));
	TestTrue(TEXT("headset lost: VR mode stays"), Rig->IsVR());
	// The runtime offers the headset again: a new session starts by itself.
	Scope.Session->OfferSystem();
	F.Frames(10);
	TestEqual(TEXT("no switch was needed"), Rig->GetSwitchCount(), Switches);
	TestTrue(TEXT("headset back: stereo runs again"), Scope.Session->IsStereoEnabled()
		&& Scope.Session->GetState() == EWoTVRSessionState::Running);
	TestFalse(TEXT("headset back: play resumes by itself"), Rig->IsHoldingStill() || F.PC->IsMoveInputIgnored());
	Scope.Session->SetHead(FVector::ZeroVector, FRotator(0.0, 30.0, 0.0));
	F.Frames(2);
	TestEqual(TEXT("headset back: the head drives the aim from where it was held"),
		FMath::FindDeltaAngleDegrees(Held.Yaw, F.PC->GetControlRotation().Yaw), 30.0, 0.05);

	// The runtime ends the session but keeps the system (it left VR on its
	// own): the plugin starts a new session at once.
	Scope.Session->EndSessionByRuntime(/*bLoseSystem*/ false);
	F.Frames(10);
	TestTrue(TEXT("session ended by the runtime: a new one runs"), Scope.Session->GetState() == EWoTVRSessionState::Running);
	TestFalse(TEXT("session ended by the runtime: play resumes by itself"), Rig->IsHoldingStill());

	// A runtime that refuses new sessions is asked again at a modest rate, not
	// every frame, until it accepts.
	Scope.Session->bFailStereo = true;
	Scope.Session->EndSessionByRuntime(/*bLoseSystem*/ false);
	const int32 Requests = Scope.Session->StereoOnRequests;
	F.Frames(60 * 4);
	const int32 Attempts = Scope.Session->StereoOnRequests - Requests;
	TestTrue(FString::Printf(TEXT("a refusing runtime is retried every %.1f s (%d attempts in 4 s)"),
		WoTVRTuning::SessionRetrySeconds(), Attempts),
		Attempts >= 2 && Attempts <= FMath::CeilToInt(4.0f / WoTVRTuning::SessionRetrySeconds()) + 1);
	TestTrue(TEXT("still held while refused"), Rig->IsHoldingStill());
	Scope.Session->bFailStereo = false;
	F.Frames(FMath::CeilToInt(60.0f * WoTVRTuning::SessionRetrySeconds()) + 5);
	TestFalse(TEXT("accepted at the next attempt: play resumes"), Rig->IsHoldingStill());
	TestEqual(TEXT("still no switch"), Rig->GetSwitchCount(), Switches);
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
