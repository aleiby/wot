#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "WoTVRTestFixture.h"
#include "Tests/WoTLocalPlayerHooksTestFixture.h"
#include "WoTVRPlayerSubsystem.h"
#include "WoTVRSyntheticSession.h"

#include "Engine/Engine.h"
#include "Engine/LocalPlayer.h"

// Diagnostics for the first headset session (design section 16): the runtime
// and its version, the system, the enabled OpenXR extensions and each hand's
// interaction profile, logged when the session starts and on the console
// command, and every profile change logged as it happens.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVRDiagnosticsSpec, "WoT.VR.Diagnostics.SessionStartProfilesAndCommand", WoTVRTest::Flags)
bool FWoTVRDiagnosticsSpec::RunTest(const FString&)
{
	WoTVRTest::FScope Scope;
	Scope.Session->Diagnostics.RuntimeName = TEXT("Spec Runtime");
	Scope.Session->Diagnostics.RuntimeVersion = TEXT("2.7.4");
	Scope.Session->Diagnostics.SystemName = TEXT("Spec Headset");
	Scope.Session->Diagnostics.EnabledExtensions = {TEXT("XR_EXT_spec_one"), TEXT("XR_KHR_spec_two")};
	const FString FrameProfile = TEXT("/interaction_profiles/valve/frame_controller_valve");
	AddExpectedMessagePlain(TEXT("VR diagnostics (session start): Synthetic session running; runtime 'Spec Runtime' version 2.7.4; system 'Spec Headset'"),
		ELogVerbosity::Display, EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(TEXT("VR diagnostics: 2 enabled OpenXR extensions: XR_EXT_spec_one, XR_KHR_spec_two"),
		ELogVerbosity::Display, EAutomationExpectedMessageFlags::Contains, 2);
	AddExpectedMessagePlain(TEXT("VR diagnostics: interaction profiles: left hand none, right hand none"),
		ELogVerbosity::Display, EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(FString::Printf(TEXT("VR interaction profile changed: left hand %s, right hand %s"), *FrameProfile, *FrameProfile),
		ELogVerbosity::Display, EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(TEXT("VR diagnostics (requested): Synthetic session running"),
		ELogVerbosity::Display, EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(FString::Printf(TEXT("VR diagnostics: interaction profiles: left hand %s, right hand %s"), *FrameProfile, *FrameProfile),
		ELogVerbosity::Display, EAutomationExpectedMessageFlags::Contains, 1);

	WoTLocalPlayerHooksTest::FPlayerWorld F;
	if (!F.Init(*this)) return false;
	UWoTVRPlayerSubsystem* Rig = WoTVRTest::Rig(F.Player);
	if (!TestNotNull(TEXT("rig"), Rig)) return false;
	Rig->RequestMode(EWoTVRMode::VR, TEXT("spec"));
	F.Frames(3);
	Scope.Session->ChangeInteractionProfile(FrameProfile, FrameProfile);
	F.Frames(2);
	TestTrue(TEXT("the diagnostics command runs"), GEngine->Exec(F.World, TEXT("WoT.VR.Diagnostics")));
	F.Frames(1);
	Rig->RequestMode(EWoTVRMode::Flat, TEXT("spec"));
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
