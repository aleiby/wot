#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "WoTVRTestFixture.h"
#include "Tests/WoTLocalPlayerHooksTestFixture.h"
#include "WoTCharacter.h"
#include "WoTPlayerController.h"
#include "WoTVRModule.h"
#include "WoTVRPlayerSubsystem.h"
#include "WoTVRRig.h"
#include "WoTVRSyntheticSession.h"
#include "WoTVRTuning.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/InputComponent.h"
#include "Engine/DamageEvents.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"
#include "GameFramework/WorldSettings.h"
#include "SceneView.h"
#include "Slate/SceneViewport.h"

// In-place switching (design sections 5.3 and 14, decision F): repeatedly, in
// a live match, in the front end and while dead, with no travel, rejoin or
// possession change. Each switch releases held input exactly once through the
// hooks, leaks no registration, and switching to flat restores every flat
// policy: aim, look assistants, view effects and the near plane.

namespace
{
double WoTVRSwitchDepth(const FMatrix& Projection, double Z)
{
	return (Projection.M[2][2] * Z + Projection.M[3][2]) / Z;
}

/** Counts the game's own Fire release binding reaching the game. */
int32 WoTVRSwitchSpyFireReleases(APlayerController& PC, TSharedRef<int32> Count)
{
	if (!PC.InputComponent)
	{
		return INDEX_NONE;
	}
	FInputActionBinding Binding(TEXT("Fire"), IE_Released);
	Binding.bConsumeInput = false;
	Binding.ActionDelegate.GetDelegateForManualSet().BindLambda([Count]() { ++*Count; });
	PC.InputComponent->AddActionBinding(Binding);
	return 0;
}

/** Checks every VR policy is attached, or that every flat one is back. */
void WoTVRSwitchExpectPolicies(FAutomationTestBase& Test, const FString& Label, bool bVR,
	ULocalPlayer& Player, APlayerController& PC, FWoTVRSyntheticSession& Session,
	FSceneViewport* Scene, UGameViewportClient* Viewport, float FlatWorldToMeters)
{
	Test.TestEqual(Label + TEXT(": the head owns the aim"), FWoTLocalPlayerHooks::OwnsAim(&PC), bVR);
	Test.TestEqual(Label + TEXT(": synthetic camera motion removed"),
		FWoTLocalPlayerHooks::SuppressSyntheticMotion(&PC), bVR);
	const TOptional<float> Near = FWoTLocalPlayerHooks::NearClip(Player);
	Test.TestEqual(Label + TEXT(": VR owns the near plane"), Near.IsSet(), bVR);
	if (bVR && Near.IsSet())
	{
		Test.TestEqual(Label + TEXT(": the stereo near plane"), *Near, WoTVRTuning::NearClip(), 0.001f);
	}
	Test.TestEqual(Label + TEXT(": stereo"), Session.IsStereoEnabled(), bVR);
	Test.TestFalse(Label + TEXT(": the player is not held"), PC.IsMoveInputIgnored());
	const AWorldSettings* Settings = PC.GetWorld() ? PC.GetWorld()->GetWorldSettings() : nullptr;
	if (Settings)
	{
		Test.TestEqual(Label + TEXT(": world scale"), Settings->WorldToMeters,
			bVR ? 52.5f : FlatWorldToMeters, 0.001f);
	}
	if (Viewport && Scene)
	{
		Viewport->LayoutPlayers();
		FSceneViewProjectionData Projection;
		if (Test.TestTrue(Label + TEXT(": projection data"), Player.GetProjectionData(Scene, Projection)))
		{
			const double Depth = WoTVRSwitchDepth(Projection.ProjectionMatrix, WoTVRTuning::NearClip());
			if (bVR)
			{
				Test.TestEqual(Label + TEXT(": the projection starts at the stereo near plane"), Depth, 1.0, 1e-6);
			}
			else
			{
				Test.TestTrue(Label + TEXT(": the flat projection keeps its own near plane"),
					!FMath::IsNearlyEqual(Depth, 1.0, 1e-3));
			}
		}
	}
	if (!bVR && PC.PlayerCameraManager)
	{
		Test.TestFalse(Label + TEXT(": no VR fade remains"), PC.PlayerCameraManager->bEnableFading);
		const FMinimalViewInfo Final = WoTVRTest::FinalView(Player);
		const FMinimalViewInfo& Camera = PC.PlayerCameraManager->GetCameraCacheView();
		Test.TestTrue(Label + TEXT(": the final view is the camera's own"),
			Final.Location.Equals(Camera.Location, 0.01) && Final.Rotation.Equals(Camera.Rotation, 0.01));
	}
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVRSwitchInMatchSpec, "WoT.VR.Switch.RepeatedInMatch", WoTVRTest::Flags)
bool FWoTVRSwitchInMatchSpec::RunTest(const FString&)
{
	WoTVRTest::FScope Scope;
	WoTLocalPlayerHooksTest::FPlayerWorld F;
	if (!F.Init(*this)) return false;
	APlayerController* PC = F.PC;
	ACharacter* Pawn = F.Pawn;
	UWoTVRPlayerSubsystem* Rig = WoTVRTest::Rig(F.Player);
	if (!TestNotNull(TEXT("rig"), Rig) || !TestTrue(TEXT("rig active"), Rig->IsActive())) return false;
	TestEqual(TEXT("the Flat preference starts flat"), Rig->GetMode(), EWoTVRMode::Flat);
	TestFalse(TEXT("flat attaches nothing"), FWoTLocalPlayerHooks::HasConsumers(F.Player));

	// Another optional consumer (read-only) observes the hooks' release contract.
	WoTVRTest::FObserver Observer;
	if (!TestTrue(TEXT("observer attached"), Observer.Attach(*F.Player))) return false;
	TSharedRef<int32> FireReleases = MakeShared<int32>(0);
	if (!TestEqual(TEXT("spy on the Fire release binding"), WoTVRSwitchSpyFireReleases(*PC, FireReleases), 0)) return false;
	const float FlatScale = PC->GetWorld()->GetWorldSettings()->WorldToMeters;
	Scope.Session->SetHead(FVector(0.0, 0.0, 0.0), FRotator(10.0, 40.0, 0.0));
	const FVector PawnStart = Pawn->GetActorLocation();

	for (int32 Cycle = 0; Cycle < 3; ++Cycle)
	{
		const FString Label = FString::Printf(TEXT("cycle %d"), Cycle + 1);
		WoTVRTest::PressKey(*PC, EKeys::LeftMouseButton);
		F.Frames(2);
		TestTrue(Label + TEXT(": the cast key is held"), PC->PlayerInput->IsPressed(EKeys::LeftMouseButton));
		int32 Contexts = Observer.Count(EWoTLocalPlayerEvent::InputContextChanged);
		int32 Releases = *FireReleases;
		const FRotator FlatAim = PC->GetControlRotation();
		TestTrue(Label + TEXT(": switch to VR"), Rig->RequestMode(EWoTVRMode::VR, TEXT("spec")));
		F.Frames(3);
		TestTrue(Label + TEXT(": VR entered"), Rig->HasEnteredVR());
		TestEqual(Label + TEXT(": one hooks release for the switch"),
			Observer.Count(EWoTLocalPlayerEvent::InputContextChanged) - Contexts, 1);
		TestEqual(Label + TEXT(": the held key released exactly once"), *FireReleases - Releases, 1);
		TestFalse(Label + TEXT(": no key is left held"), PC->PlayerInput->IsPressed(EKeys::LeftMouseButton));
		TestEqual(Label + TEXT(": entering VR keeps the aim's yaw"),
			FMath::FindDeltaAngleDegrees(FlatAim.Yaw, PC->GetControlRotation().Yaw), 0.0, 0.01);
		TestEqual(Label + TEXT(": the head's pitch is the aim's"), PC->GetControlRotation().Pitch, 10.0, 0.01);
		WoTVRSwitchExpectPolicies(*this, Label + TEXT(" VR"), true, *F.Player, *PC, *Scope.Session,
			F.Scene.Get(), F.Viewport, FlatScale);
		TestTrue(Label + TEXT(": the same pawn, still possessed"), PC->GetPawn() == Pawn);
		TestTrue(Label + TEXT(": the same controller"), F.Player->PlayerController == PC);

		// Look around, then leave VR with the cast held again.
		Scope.Session->SetHead(FVector(0.0, 0.0, 0.0), FRotator(-15.0, 70.0, 0.0));
		F.Frames(2);
		const FRotator VRAim = PC->GetControlRotation();
		WoTVRTest::PressKey(*PC, EKeys::LeftMouseButton);
		F.Frames(2);
		Contexts = Observer.Count(EWoTLocalPlayerEvent::InputContextChanged);
		Releases = *FireReleases;
		TestTrue(Label + TEXT(": switch to flat"), Rig->RequestMode(EWoTVRMode::Flat, TEXT("spec")));
		F.Frames(3);
		TestEqual(Label + TEXT(": one hooks release for the switch back"),
			Observer.Count(EWoTLocalPlayerEvent::InputContextChanged) - Contexts, 1);
		TestEqual(Label + TEXT(": the held key released exactly once again"), *FireReleases - Releases, 1);
		TestTrue(Label + TEXT(": flat continues from the head's aim without a jump"),
			PC->GetControlRotation().Equals(VRAim, 0.01));
		WoTVRSwitchExpectPolicies(*this, Label + TEXT(" flat"), false, *F.Player, *PC, *Scope.Session,
			F.Scene.Get(), F.Viewport, FlatScale);
		Scope.Session->SetHead(FVector(0.0, 0.0, 0.0), FRotator(10.0, 40.0, 0.0));
	}
	TestTrue(TEXT("no switch moved the body"), Pawn->GetActorLocation().Equals(PawnStart, 1.0));
	TestEqual(TEXT("six switches"), Rig->GetSwitchCount(), 6);
	Observer.Detach();
	TestFalse(TEXT("no registration leaked"), FWoTLocalPlayerHooks::HasConsumers(F.Player));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVRSwitchFrontEndSpec, "WoT.VR.Switch.FrontEnd", WoTVRTest::Flags)
bool FWoTVRSwitchFrontEndSpec::RunTest(const FString&)
{
	WoTVRTest::FScope Scope;
	WoTVRTest::FMatchWorld F;
	if (!F.Init(*this, TEXT("WoTFrontEndGameMode"), /*bWithController*/ false)) return false;
	UClass* FrontEndClass = WoTVRTest::GameClass(TEXT("WoTFrontEndPlayerController"));
	if (!TestNotNull(TEXT("front-end controller class"), FrontEndClass)) return false;
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	APlayerController* FrontEnd = F.World->SpawnActor<APlayerController>(FrontEndClass, Params);
	if (!TestNotNull(TEXT("real front-end controller"), FrontEnd)) return false;
	FrontEnd->SetPlayer(F.Player);
	TestNull(TEXT("the front end is pawnless"), FrontEnd->GetPawn());
	UWoTVRPlayerSubsystem* Rig = WoTVRTest::Rig(F.Player);
	if (!TestNotNull(TEXT("rig"), Rig)) return false;
	WoTVRTest::FObserver Observer;
	Observer.Attach(*F.Player);
	F.Frames(2);
	for (int32 Cycle = 0; Cycle < 3; ++Cycle)
	{
		const FString Label = FString::Printf(TEXT("front end %d"), Cycle + 1);
		const int32 Contexts = Observer.Count(EWoTLocalPlayerEvent::InputContextChanged);
		Scope.Session->SetHead(FVector(0.0, 0.0, 0.0), FRotator(0.0, 25.0, 0.0));
		TestTrue(Label + TEXT(": switch to VR in the front end"), Rig->RequestMode(EWoTVRMode::VR, TEXT("spec")));
		F.Frames(2);
		TestTrue(Label + TEXT(": VR entered without a pawn"), Rig->HasEnteredVR());
		TestTrue(Label + TEXT(": the rig is attached"), Rig->IsRigAttached());
		const FMinimalViewInfo& Camera = FrontEnd->PlayerCameraManager->GetCameraCacheView();
		FMinimalViewInfo Final = WoTVRTest::FinalView(*F.Player);
		TestTrue(Label + TEXT(": the neutral anchor is the front-end camera's eye"),
			Final.Location.Equals(Camera.Location, 0.01));
		TestEqual(Label + TEXT(": the view starts along the camera's direction"),
			FMath::FindDeltaAngleDegrees(Final.Rotation.Yaw, Camera.Rotation.Yaw), 0.0, 0.01);
		// Looking around the front end turns the view with the head.
		Scope.Session->SetHead(FVector(0.0, 0.0, 0.0), FRotator(0.0, 55.0, 0.0));
		F.Frames(1);
		Final = WoTVRTest::FinalView(*F.Player);
		TestEqual(Label + TEXT(": the head turns the front-end view"),
			FMath::FindDeltaAngleDegrees(Camera.Rotation.Yaw, Final.Rotation.Yaw), 30.0, 0.01);
		TestTrue(Label + TEXT(": switch to flat in the front end"), Rig->RequestMode(EWoTVRMode::Flat, TEXT("spec")));
		F.Frames(2);
		TestEqual(Label + TEXT(": one release per switch pair, each through the hooks"),
			Observer.Count(EWoTLocalPlayerEvent::InputContextChanged) - Contexts, 2);
		Final = WoTVRTest::FinalView(*F.Player);
		TestTrue(Label + TEXT(": flat shows the camera's own view again"),
			Final.Location.Equals(Camera.Location, 0.01) && Final.Rotation.Equals(Camera.Rotation, 0.01));
		TestFalse(Label + TEXT(": stereo off"), Scope.Session->IsStereoEnabled());
	}
	Observer.Detach();
	TestFalse(TEXT("no registration leaked in the front end"), FWoTLocalPlayerHooks::HasConsumers(F.Player));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVRSwitchWhileDeadSpec, "WoT.VR.Switch.WhileDead", WoTVRTest::Flags)
bool FWoTVRSwitchWhileDeadSpec::RunTest(const FString&)
{
	WoTVRTest::FScope Scope;
	WoTVRTest::FMatchWorld F;
	if (!F.Init(*this)) return false;
	APawn* Pawn = F.Restart(*this);
	if (!Pawn) return false;
	UWoTVRPlayerSubsystem* Rig = WoTVRTest::Rig(F.Player);
	if (!TestNotNull(TEXT("rig"), Rig)) return false;
	WoTVRTest::FObserver Observer;
	Observer.Attach(*F.Player);
	F.Frames(2);
	WoTVRTest::SetIntProperty(Pawn, TEXT("MaxHealth"), 100);
	WoTVRTest::SetIntProperty(Pawn, TEXT("Health"), 25);
	Pawn->TakeDamage(26.0f, FDamageEvent(), nullptr, nullptr);
	F.Frames(2);
	if (!TestNull(TEXT("death releases possession"), F.PC->GetPawn())) return false;
	TestTrue(TEXT("the observer heard the death"), Observer.Count(EWoTLocalPlayerEvent::Death) > 0);
	for (int32 Cycle = 0; Cycle < 3; ++Cycle)
	{
		const FString Label = FString::Printf(TEXT("dead %d"), Cycle + 1);
		const int32 Contexts = Observer.Count(EWoTLocalPlayerEvent::InputContextChanged);
		TestTrue(Label + TEXT(": switch to VR while dead"), Rig->RequestMode(EWoTVRMode::VR, TEXT("spec")));
		F.Frames(2);
		TestTrue(Label + TEXT(": VR entered while dead"), Rig->HasEnteredVR());
		TestTrue(Label + TEXT(": the head owns the aim while dead"), FWoTLocalPlayerHooks::OwnsAim(F.PC));
		TestNull(Label + TEXT(": switching does not respawn"), F.PC->GetPawn());
		TestTrue(Label + TEXT(": switch to flat while dead"), Rig->RequestMode(EWoTVRMode::Flat, TEXT("spec")));
		F.Frames(2);
		TestFalse(Label + TEXT(": the flat aim is back"), FWoTLocalPlayerHooks::OwnsAim(F.PC));
		TestNull(Label + TEXT(": switching back does not respawn"), F.PC->GetPawn());
		TestEqual(Label + TEXT(": one release per switch"),
			Observer.Count(EWoTLocalPlayerEvent::InputContextChanged) - Contexts, 2);
	}
	// The ordinary respawn gate still works in VR.
	TestTrue(TEXT("back to VR for the respawn"), Rig->RequestMode(EWoTVRMode::VR, TEXT("spec")));
	F.Frames(2);
	Observer.Command(EWoTLocalPlayerCommand::Fire);
	Observer.Command(EWoTLocalPlayerCommand::CeaseFire);
	F.Frames(2);
	APawn* Respawned = F.PC->GetPawn();
	TestTrue(TEXT("Fire respawns through the existing gate in VR"), Respawned && Respawned != Pawn);
	TestTrue(TEXT("the rig stays attached across the respawn"), Rig->IsRigAttached() && Rig->IsVR());
	TestFalse(TEXT("the respawned view is the new body's, not the held death view"), Rig->IsAnchorFrozen());
	Rig->RequestMode(EWoTVRMode::Flat, TEXT("spec"));
	Observer.Detach();
	TestFalse(TEXT("no registration leaked across death and respawn"), FWoTLocalPlayerHooks::HasConsumers(F.Player));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVRSwitchShortcutSpec, "WoT.VR.Switch.ShortcutKeys", WoTVRTest::Flags)
bool FWoTVRSwitchShortcutSpec::RunTest(const FString&)
{
	if (!FSlateApplication::IsInitialized())
	{
		AddWarning(TEXT("Slate is not initialised; the shortcut spec needs it"));
		return true;
	}
	WoTVRTest::FScope Scope;
	WoTLocalPlayerHooksTest::FPlayerWorld F;
	if (!F.Init(*this)) return false;
	UWoTVRPlayerSubsystem* Rig = WoTVRTest::Rig(F.Player);
	if (!TestNotNull(TEXT("rig"), Rig)) return false;
	TestTrue(TEXT("the shortcuts are registered"), FWoTVRModule::Get().AreShortcutsRegistered());
	FSlateApplication& Slate = FSlateApplication::Get();
	auto Press = [&Slate](const FKey& Key, const FModifierKeysState& Modifiers)
	{
		const FKeyEvent Down(Key, Modifiers, 0, false, 0, 0);
		Slate.ProcessKeyDownEvent(Down);
		const FKeyEvent Up(Key, Modifiers, 0, false, 0, 0);
		Slate.ProcessKeyUpEvent(Up);
	};
	Press(EKeys::F7, FModifierKeysState());
	F.Frames(2);
	TestEqual(TEXT("F7 switches to VR"), Rig->GetMode(), EWoTVRMode::VR);
	Scope.Session->SetHead(FVector(0.05, 0.02, -0.03), FRotator(0.0, 20.0, 0.0));
	F.Frames(1);
	Press(EKeys::F8, FModifierKeysState());
	F.Frames(1);
	TestTrue(TEXT("F8 recentres: the head is back over the anchor"),
		Rig->GetRig().RelativeOffset(FWoTVRHeadPose{Scope.Session->Pose.Orientation,
			Scope.Session->Pose.PositionMeters, true}).IsNearlyZero(0.001));
	const FModifierKeysState Alt(false, false, false, false, true, false, false, false, false);
	Press(EKeys::F7, Alt);
	F.Frames(1);
	TestEqual(TEXT("Alt+F7 is not the shortcut"), Rig->GetMode(), EWoTVRMode::VR);
	Press(EKeys::F7, FModifierKeysState());
	F.Frames(2);
	TestEqual(TEXT("F7 switches back to flat"), Rig->GetMode(), EWoTVRMode::Flat);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVRSwitchKeysSpec, "WoT.VR.Switch.KeysActOnlyInVR", WoTVRTest::Flags)
bool FWoTVRSwitchKeysSpec::RunTest(const FString&)
{
	WoTVRTest::FScope Scope;
	WoTLocalPlayerHooksTest::FPlayerWorld F;
	if (!F.Init(*this)) return false;
	UWoTVRPlayerSubsystem* Rig = WoTVRTest::Rig(F.Player);
	if (!TestNotNull(TEXT("rig"), Rig)) return false;
	APlayerController* PC = F.PC;
	// The shortcut handler sees every key before any widget; true consumes it.
	auto Shortcut = [](const FKey& Key)
	{
		const FKeyEvent Event(Key, FModifierKeysState(), 0, false, 0, 0);
		return FWoTVRModule::Get().HandleShortcutKey(Event);
	};
	TestFalse(TEXT("in flat the recentre key stays the game's"), Shortcut(EKeys::F8));
	Scope.Session->bSystemAvailable = false;
	TestFalse(TEXT("in flat without a headset the switch key stays the game's"), Shortcut(EKeys::F7));
	TestEqual(TEXT("and switches nothing"), Rig->GetMode(), EWoTVRMode::Flat);
	Scope.Session->bSystemAvailable = true;
	TestTrue(TEXT("in flat with a headset the switch key enters VR"), Shortcut(EKeys::F7));
	F.Frames(2);
	TestTrue(TEXT("VR entered by the key"), Rig->HasEnteredVR());
	TestTrue(TEXT("in VR the recentre key recentres"), Shortcut(EKeys::F8));

	// A turn key the player has bound to a game action is that action's.
	PC->PlayerInput->AddActionMapping(FInputActionKeyMapping(TEXT("WoTVRSpecAction"), EKeys::C));
	F.Frames(1);
	const int32 Snaps = Rig->GetSnapTurnCount();
	WoTVRTest::PressKey(*PC, EKeys::C);
	F.Frames(2);
	WoTVRTest::ReleaseKey(*PC, EKeys::C);
	F.Frames(2);
	TestEqual(TEXT("a turn key bound to a game action does not turn"), Rig->GetSnapTurnCount(), Snaps);
	PC->PlayerInput->RemoveActionMapping(FInputActionKeyMapping(TEXT("WoTVRSpecAction"), EKeys::C));
	F.Frames(1);

	// Rebinding in the plugin's settings takes effect at once (B and F6: keys
	// the game's default bindings leave free).
	TestTrue(TEXT("the turn key rebinds"), GEngine->Exec(F.World, TEXT("WoT.VR.Key TurnLeft B")));
	TestTrue(TEXT("the setting holds the new key"), UWoTVRSettings::Get().TurnLeftKey == EKeys::B);
	WoTVRTest::PressKey(*PC, EKeys::C);
	F.Frames(2);
	WoTVRTest::ReleaseKey(*PC, EKeys::C);
	F.Frames(2);
	TestEqual(TEXT("the old turn key no longer turns"), Rig->GetSnapTurnCount(), Snaps);
	WoTVRTest::PressKey(*PC, EKeys::B);
	F.Frames(2);
	WoTVRTest::ReleaseKey(*PC, EKeys::B);
	F.Frames(2);
	TestEqual(TEXT("the new turn key turns"), Rig->GetSnapTurnCount(), Snaps + 1);
	// A rebind onto a key the game binds (Q selects a hand) leaves it the game's.
	TestTrue(TEXT("the turn key rebinds onto a game key"), GEngine->Exec(F.World, TEXT("WoT.VR.Key TurnLeft Q")));
	WoTVRTest::PressKey(*PC, EKeys::Q);
	F.Frames(2);
	WoTVRTest::ReleaseKey(*PC, EKeys::Q);
	F.Frames(2);
	TestEqual(TEXT("a game-bound key does not turn"), Rig->GetSnapTurnCount(), Snaps + 1);
	GEngine->Exec(F.World, TEXT("WoT.VR.Key TurnLeft B"));
	TestTrue(TEXT("the switch key rebinds"), GEngine->Exec(F.World, TEXT("WoT.VR.Key ModeSwitch F6")));
	TestFalse(TEXT("the old switch key is the game's again"), Shortcut(EKeys::F7));
	TestEqual(TEXT("still in VR"), Rig->GetMode(), EWoTVRMode::VR);
	TestTrue(TEXT("the new switch key switches"), Shortcut(EKeys::F6));
	F.Frames(2);
	TestEqual(TEXT("flat by the new key"), Rig->GetMode(), EWoTVRMode::Flat);
	WoTVRTest::PressKey(*PC, EKeys::B);
	F.Frames(2);
	WoTVRTest::ReleaseKey(*PC, EKeys::B);
	F.Frames(2);
	TestEqual(TEXT("in flat the turn keys are the game's"), Rig->GetSnapTurnCount(), Snaps + 1);
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
