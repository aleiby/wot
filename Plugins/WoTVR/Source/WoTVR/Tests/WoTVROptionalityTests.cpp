#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "WoTVRTestFixture.h"
#include "Tests/WoTLocalPlayerHooksTestFixture.h"
#include "WoTVRGate.h"
#include "WoTVRModule.h"
#include "WoTVRPlayerSubsystem.h"
#include "WoTVRSyntheticSession.h"

#include "Engine/Engine.h"
#include "Engine/LocalPlayer.h"
#include "Features/IModularFeatures.h"
#include "IHeadMountedDisplayModule.h"
#include "IOpenXRHMDModule.h"
#include "Modules/ModuleManager.h"

// Optionality (design sections 5.1, 5.3 and 15): with the plugin switched off
// nothing attaches to the game and no XR instance exists; the preference and
// the launch flags choose the startup mode with -novr winning; Auto falls back
// to flat without a session, and an explicit request reports its failure.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVRGateDecisionSpec, "WoT.VR.Optionality.XRGateDecision", WoTVRTest::Flags)
bool FWoTVRGateDecisionSpec::RunTest(const FString&)
{
	auto Decide = [](bool bMaster, bool bVR, bool bNoVR, bool bCommandlet, bool bServer, bool bEditor)
	{
		FWoTVRGateInputs Inputs;
		Inputs.bMasterEnable = bMaster;
		Inputs.bFlagVR = bVR;
		Inputs.bFlagNoVR = bNoVR;
		Inputs.bCommandlet = bCommandlet;
		Inputs.bDedicatedServer = bServer;
		Inputs.bEditor = bEditor;
		return FWoTVRGate::Decide(Inputs).bXRAllowed;
	};
	TestFalse(TEXT("master enable off, no flag: no XR"), Decide(false, false, false, false, false, false));
	TestTrue(TEXT("master enable on in a game process: XR at startup"), Decide(true, false, false, false, false, false));
	TestTrue(TEXT("-vr opens XR with the master enable off"), Decide(false, true, false, false, false, false));
	TestFalse(TEXT("-novr: no XR"), Decide(true, false, true, false, false, false));
	TestFalse(TEXT("-novr wins over -vr"), Decide(true, true, true, false, false, false));
	TestFalse(TEXT("a commandlet never creates XR, even with -vr"), Decide(true, true, false, true, false, false));
	TestFalse(TEXT("a dedicated server never creates XR, even with -vr"), Decide(true, true, false, false, true, false));
	TestFalse(TEXT("an editor process ignores the saved master enable"), Decide(true, false, false, false, false, true));
	TestTrue(TEXT("an editor process honours -vr"), Decide(false, true, false, false, false, true));
	TestTrue(TEXT("every decision carries a reason"), !FWoTVRGate::Decide(FWoTVRGateInputs()).Reason.IsEmpty());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVRGateClosedInProcessSpec, "WoT.VR.Optionality.GateClosedWithoutVR", WoTVRTest::Flags)
bool FWoTVRGateClosedInProcessSpec::RunTest(const FString&)
{
	// This spec process loads the plugin, so the stock OpenXR plugin is loaded
	// too. Unless it was launched with -vr, the gate must have withheld
	// OpenXR's HMD module from the engine before HMD pre-initialisation: no HMD
	// module is visible, no XR system or stereo device exists and OpenXR holds
	// no instance.
	const FWoTVRGateDecision& Decision = FWoTVRGate::GetProcessDecision();
	if (Decision.bXRAllowed)
	{
		AddWarning(FString::Printf(TEXT("this process opened the XR gate (%s); the closed-gate checks need a launch without -vr"),
			*Decision.Reason));
		return true;
	}
	TestTrue(TEXT("the stock OpenXR HMD module is loaded (the plugin depends on it)"),
		FModuleManager::Get().IsModuleLoaded(TEXT("OpenXRHMD")));
	TestTrue(TEXT("the gate withheld at least OpenXR's HMD registration"),
		FWoTVRGate::GetWithheldRegistrationCount() >= 1);
	// Only the engine's own placeholder (HeadMountedDisplay's "Default", which
	// has no runtime) may remain; no XR-capable HMD module is visible.
	TArray<IHeadMountedDisplayModule*> Visible = IModularFeatures::Get()
		.GetModularFeatureImplementations<IHeadMountedDisplayModule>(IHeadMountedDisplayModule::GetModularFeatureName());
	for (IHeadMountedDisplayModule* Module : Visible)
	{
		TestEqual(TEXT("no XR-capable HMD module is visible to the engine"), Module->GetModuleKeyName(), FString(TEXT("Default")));
	}
	TestFalse(TEXT("no XR system exists"), GEngine && GEngine->XRSystem.IsValid());
	TestFalse(TEXT("no stereo rendering device exists"), GEngine && GEngine->StereoRenderingDevice.IsValid());
	if (FModuleManager::Get().IsModuleLoaded(TEXT("OpenXRHMD")))
	{
		TestTrue(TEXT("OpenXR holds no instance"), IOpenXRHMDModule::Get().GetInstance() == XR_NULL_HANDLE);
	}
	TestFalse(TEXT("the plugin is inactive in this launch"), FWoTVRModule::Get().IsActive());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVRInactiveAttachesNothingSpec, "WoT.VR.Optionality.MasterOffAttachesNothing", WoTVRTest::Flags)
bool FWoTVRInactiveAttachesNothingSpec::RunTest(const FString&)
{
	// The master enable off (the plugin inactive): a session is even available,
	// yet the rig attaches nothing, registers no shortcut and starts no stereo.
	WoTVRTest::FScope Scope(/*bActive*/ false, EWoTVRStartupMode::VR, /*bFlagVR*/ true);
	WoTLocalPlayerHooksTest::FPlayerWorld F;
	if (!F.Init(*this)) return false;
	UWoTVRPlayerSubsystem* Rig = WoTVRTest::Rig(F.Player);
	if (!TestNotNull(TEXT("the rig exists for the local player"), Rig)) return false;
	TestFalse(TEXT("the rig is inactive"), Rig->IsActive());
	TestFalse(TEXT("nothing attaches to the local-player hooks"), FWoTLocalPlayerHooks::HasConsumers(F.Player));
	TestFalse(TEXT("no shortcut is registered"), FWoTVRModule::Get().AreShortcutsRegistered());
	TestEqual(TEXT("no stereo request was made"), Scope.Session->StereoOnRequests, 0);
	TestFalse(TEXT("a switch request is refused"), Rig->RequestMode(EWoTVRMode::VR, TEXT("spec")));
	F.Frames(5);
	TestFalse(TEXT("still nothing attached"), FWoTLocalPlayerHooks::HasConsumers(F.Player));
	TestEqual(TEXT("still no stereo request"), Scope.Session->StereoOnRequests, 0);
	TestEqual(TEXT("the mode stays flat"), Rig->GetMode(), EWoTVRMode::Flat);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVRStartupPrecedenceSpec, "WoT.VR.Optionality.StartupPrecedence", WoTVRTest::Flags)
bool FWoTVRStartupPrecedenceSpec::RunTest(const FString&)
{
	auto Resolve = [](bool bActive, EWoTVRStartupMode Preference, bool bVR, bool bNoVR)
	{
		FWoTVRStartupInputs Inputs;
		Inputs.bActive = bActive;
		Inputs.Preference = Preference;
		Inputs.bFlagVR = bVR;
		Inputs.bFlagNoVR = bNoVR;
		return FWoTVRModule::ResolveStartup(Inputs).Request;
	};
	using EMode = EWoTVRStartupMode;
	using ERequest = EWoTVRStartupRequest;
	TestEqual(TEXT("inactive starts flat whatever the preference"), Resolve(false, EMode::VR, true, false), ERequest::Flat);
	TestEqual(TEXT("Auto asks for VR when a session starts"), Resolve(true, EMode::Auto, false, false), ERequest::AutoVR);
	TestEqual(TEXT("the Flat preference starts flat"), Resolve(true, EMode::Flat, false, false), ERequest::Flat);
	TestEqual(TEXT("the VR preference is explicit"), Resolve(true, EMode::VR, false, false), ERequest::ExplicitVR);
	TestEqual(TEXT("-vr overrides the Flat preference"), Resolve(true, EMode::Flat, true, false), ERequest::ExplicitVR);
	TestEqual(TEXT("-novr overrides the VR preference"), Resolve(true, EMode::VR, false, true), ERequest::Flat);
	TestEqual(TEXT("-novr wins over -vr"), Resolve(true, EMode::Auto, true, true), ERequest::Flat);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVRStartupBootSpec, "WoT.VR.Optionality.BootFallsBackToFlat", WoTVRTest::Flags)
bool FWoTVRStartupBootSpec::RunTest(const FString&)
{
	{
		// Auto without a headset session: flat, quietly.
		WoTVRTest::FScope Scope(true, EWoTVRStartupMode::Auto);
		Scope.Session->bSystemAvailable = false;
		WoTLocalPlayerHooksTest::FPlayerWorld F;
		if (!F.Init(*this)) return false;
		UWoTVRPlayerSubsystem* Rig = WoTVRTest::Rig(F.Player);
		if (!TestNotNull(TEXT("rig"), Rig)) return false;
		TestTrue(TEXT("Auto: the rig is active"), Rig->IsActive());
		TestEqual(TEXT("Auto without a session starts flat"), Rig->GetMode(), EWoTVRMode::Flat);
		TestFalse(TEXT("Auto without a session attaches nothing"), FWoTLocalPlayerHooks::HasConsumers(F.Player));
		TestTrue(TEXT("an active rig registers the shortcuts"), FWoTVRModule::Get().AreShortcutsRegistered());
	}
	TestFalse(TEXT("the shortcuts go with the last active rig"), FWoTVRModule::Get().AreShortcutsRegistered());
	{
		// Auto with a session: VR at startup, attached in place.
		WoTVRTest::FScope Scope(true, EWoTVRStartupMode::Auto);
		WoTLocalPlayerHooksTest::FPlayerWorld F;
		if (!F.Init(*this)) return false;
		UWoTVRPlayerSubsystem* Rig = WoTVRTest::Rig(F.Player);
		if (!TestNotNull(TEXT("rig"), Rig)) return false;
		TestEqual(TEXT("Auto with a session starts in VR"), Rig->GetMode(), EWoTVRMode::VR);
		TestTrue(TEXT("Auto with a session has entered VR"), Rig->HasEnteredVR());
		TestTrue(TEXT("VR owns the aim"), FWoTLocalPlayerHooks::OwnsAim(F.PC));
		TestTrue(TEXT("stereo started"), Scope.Session->IsStereoEnabled());
	}
	{
		// -vr without a usable session: an error names the failure, then flat.
		WoTVRTest::FScope Scope(true, EWoTVRStartupMode::Flat, /*bFlagVR*/ true);
		Scope.Session->bSystemAvailable = false;
		AddExpectedError(TEXT("VR requested by -vr, but no XR system is available"), EAutomationExpectedErrorFlags::Contains, 1);
		WoTLocalPlayerHooksTest::FPlayerWorld F;
		if (!F.Init(*this)) return false;
		UWoTVRPlayerSubsystem* Rig = WoTVRTest::Rig(F.Player);
		if (!TestNotNull(TEXT("rig"), Rig)) return false;
		TestEqual(TEXT("an explicit failure continues flat"), Rig->GetMode(), EWoTVRMode::Flat);
		TestFalse(TEXT("an explicit failure attaches nothing"), FWoTLocalPlayerHooks::HasConsumers(F.Player));
	}
	{
		// -novr wins over -vr and over the VR preference.
		WoTVRTest::FScope Scope(true, EWoTVRStartupMode::VR, /*bFlagVR*/ true, /*bFlagNoVR*/ true);
		WoTLocalPlayerHooksTest::FPlayerWorld F;
		if (!F.Init(*this)) return false;
		UWoTVRPlayerSubsystem* Rig = WoTVRTest::Rig(F.Player);
		if (!TestNotNull(TEXT("rig"), Rig)) return false;
		TestEqual(TEXT("-novr starts flat"), Rig->GetMode(), EWoTVRMode::Flat);
		TestEqual(TEXT("-novr makes no stereo request"), Scope.Session->StereoOnRequests, 0);
	}
	{
		// A session that never starts: an explicit request gives up after the
		// start timeout and continues flat.
		WoTVRTest::FScope Scope(true, EWoTVRStartupMode::VR);
		Scope.Session->bStartsImmediately = false;
		AddExpectedError(TEXT("VR start failed"), EAutomationExpectedErrorFlags::Contains, 1);
		WoTLocalPlayerHooksTest::FPlayerWorld F;
		if (!F.Init(*this)) return false;
		UWoTVRPlayerSubsystem* Rig = WoTVRTest::Rig(F.Player);
		if (!TestNotNull(TEXT("rig"), Rig)) return false;
		TestEqual(TEXT("a pending start is in VR mode"), Rig->GetMode(), EWoTVRMode::VR);
		TestFalse(TEXT("a pending start has not entered VR"), Rig->HasEnteredVR());
		TestTrue(TEXT("a pending start holds the player still"), F.PC->IsMoveInputIgnored());
		F.Frames(60 * 11);
		TestEqual(TEXT("the start times out to flat"), Rig->GetMode(), EWoTVRMode::Flat);
		TestFalse(TEXT("the timed-out start detached"), FWoTLocalPlayerHooks::HasConsumers(F.Player));
		TestFalse(TEXT("the timed-out start releases the player"), F.PC->IsMoveInputIgnored());
		TestFalse(TEXT("stereo stopped"), Scope.Session->IsStereoEnabled());
	}
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
