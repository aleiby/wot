#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "WoTVRTestFixture.h"
#include "Tests/WoTLocalPlayerHooksTestFixture.h"
#include "Tests/WoTTestWorld.h"
#include "WoTCharacter.h"
#include "WoTGameMode.h"
#include "WoTPlayerController.h"
#include "WoTVRPlayerSubsystem.h"
#include "WoTVRSyntheticSession.h"

#include "Camera/PlayerCameraManager.h"
#include "Engine/DamageEvents.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"
#include "Misc/ScopeExit.h"
#include "Slate/SceneViewport.h"
#include "UObject/UnrealType.h"
#include "Widgets/SViewport.h"

#if WITH_EDITOR
#include "Tests/NetTestHelpers.h"
#endif

// The lifecycle (design section 14): possession, teleports, death and respawn
// re-anchor the head over the body; a forced turn reaches the rendered head
// only once it ends; session loss holds the player still with input released
// until recovery; and the rig survives front end to match and back.

namespace
{
double WoTVRLifecycleYawError(double A, double B)
{
	return FMath::Abs(FMath::FindDeltaAngleDegrees(A, B));
}

bool WoTVRLifecycleForcedYaw(APlayerController& PC, bool bEnable, float RateUnitsPerSecond)
{
	UFunction* Function = PC.FindFunction(TEXT("ClientSetForcedViewYaw"));
	if (!Function)
	{
		return false;
	}
	TArray<uint8> Params;
	Params.SetNumZeroed(Function->ParmsSize);
	Function->InitializeStruct(Params.GetData());
	FBoolProperty* Enable = CastField<FBoolProperty>(Function->FindPropertyByName(TEXT("bEnable")));
	FFloatProperty* Rate = CastField<FFloatProperty>(Function->FindPropertyByName(TEXT("YawRateUnitsPerSecond")));
	if (Enable && Rate)
	{
		Enable->SetPropertyValue_InContainer(Params.GetData(), bEnable);
		Rate->SetPropertyValue_InContainer(Params.GetData(), RateUnitsPerSecond);
		PC.ProcessEvent(Function, Params.GetData());
	}
	Function->DestroyStruct(Params.GetData());
	return Enable && Rate;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVRLifecycleAnchorSpec, "WoT.VR.Lifecycle.TeleportDeathRespawnReanchor", WoTVRTest::Flags)
bool FWoTVRLifecycleAnchorSpec::RunTest(const FString&)
{
	WoTVRTest::FScope Scope;
	WoTVRTest::FMatchWorld F;
	if (!F.Init(*this)) return false;
	ACharacter* Pawn = Cast<ACharacter>(F.Restart(*this));
	UWoTVRPlayerSubsystem* Rig = WoTVRTest::Rig(F.Player);
	if (!TestNotNull(TEXT("pawn"), Pawn) || !TestNotNull(TEXT("rig"), Rig)) return false;
	WoTVRTest::FObserver Observer;
	Observer.Attach(*F.Player);
	F.Frames(20);
	Scope.Session->SetHead(FVector::ZeroVector, FRotator::ZeroRotator);
	Rig->RequestMode(EWoTVRMode::VR, TEXT("spec"));
	F.Frames(40);

	// Lean 0.10 m to the right: the rendered eye leads the body by 5.25 units.
	Scope.Session->SetHead(FVector(0.0, 0.10, 0.0), FRotator::ZeroRotator);
	F.Frames(2);
	FVector Offset = WoTVRTest::FinalView(*F.Player).Location - Pawn->GetPawnViewLocation();
	TestEqual(TEXT("the lean leads the body"), FVector2D(Offset.X, Offset.Y).Size(), 5.25, 0.05);

	// A teleport with an exit heading: the head goes back over the body, and the
	// heading takes the exit heading at once, behind a short fade.
	const double YawBefore = F.PC->GetControlRotation().Yaw;
	const int32 Teleports = Observer.Count(EWoTLocalPlayerEvent::Teleported);
	TestTrue(TEXT("teleport"), Pawn->TeleportTo(Pawn->GetActorLocation() + FVector(1500.0, 0.0, 0.0),
		Pawn->GetActorRotation(), false, true));
	F.PC->SetControlRotation(F.PC->GetControlRotation() + FRotator(0.0, 90.0, 0.0));
	F.Frames(1);
	TestEqual(TEXT("one teleport"), Observer.Count(EWoTLocalPlayerEvent::Teleported) - Teleports, 1);
	TestTrue(TEXT("a transition fade covers the teleport"), Rig->GetFadeAlpha() > 0.5f);
	Offset = WoTVRTest::FinalView(*F.Player).Location - Pawn->GetPawnViewLocation();
	TestTrue(TEXT("the teleport puts the head back over the body"), FVector2D(Offset.X, Offset.Y).IsNearlyZero(0.05));
	TestEqual(TEXT("the aim keeps the exit heading"),
		FMath::FindDeltaAngleDegrees(YawBefore, F.PC->GetControlRotation().Yaw), 90.0, 0.05);
	TestTrue(TEXT("the view took the exit heading"),
		WoTVRLifecycleYawError(WoTVRTest::FinalView(*F.Player).Rotation.Yaw, F.PC->GetControlRotation().Yaw) < 0.05);
	TestEqual(TEXT("no gameplay yaw is left over"), FWoTLocalPlayerHooks::PeekGameplayYaw(*F.Player), 0.0, 0.05);
	F.Frames(30);
	TestEqual(TEXT("the fade clears"), Rig->GetFadeAlpha(), 0.0f, 0.001f);

	// Death: a stable view held where the eye was.
	const FVector LastEye = F.PC->PlayerCameraManager->GetCameraCacheView().Location;
	WoTVRTest::SetIntProperty(Pawn, TEXT("MaxHealth"), 100);
	WoTVRTest::SetIntProperty(Pawn, TEXT("Health"), 25);
	Pawn->TakeDamage(26.0f, FDamageEvent(), nullptr, nullptr);
	F.Frames(5);
	TestNull(TEXT("dead"), F.PC->GetPawn());
	TestTrue(TEXT("death holds a stable anchor"), Rig->IsAnchorFrozen());
	TestTrue(TEXT("the dead view stays where the eye was"),
		WoTVRTest::FinalView(*F.Player).Location.Equals(LastEye, 0.05));
	TestTrue(TEXT("VR stays on while dead"), Rig->IsVR() && Rig->IsRigAttached());

	// Respawn: the new body becomes the anchor, head over it, facing taken.
	Observer.Command(EWoTLocalPlayerCommand::Fire);
	Observer.Command(EWoTLocalPlayerCommand::CeaseFire);
	F.Frames(2);
	ACharacter* Respawned = Cast<ACharacter>(F.PC->GetPawn());
	if (!TestNotNull(TEXT("respawned"), Respawned)) return false;
	TestFalse(TEXT("respawn releases the held death view"), Rig->IsAnchorFrozen());
	Offset = WoTVRTest::FinalView(*F.Player).Location - Respawned->GetPawnViewLocation();
	TestTrue(TEXT("respawn puts the head over the new body"), FVector2D(Offset.X, Offset.Y).IsNearlyZero(0.05));
	TestTrue(TEXT("the view takes the spawn facing"),
		WoTVRLifecycleYawError(WoTVRTest::FinalView(*F.Player).Rotation.Yaw, F.PC->GetControlRotation().Yaw) < 0.05);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVRLifecycleForcedTurnSpec, "WoT.VR.Lifecycle.ForcedTurnReachesViewAfterward", WoTVRTest::Flags)
bool FWoTVRLifecycleForcedTurnSpec::RunTest(const FString&)
{
	WoTVRTest::FScope Scope;
	WoTLocalPlayerHooksTest::FPlayerWorld F;
	if (!F.Init(*this)) return false;
	APlayerController* PC = F.PC;
	UWoTVRPlayerSubsystem* Rig = WoTVRTest::Rig(F.Player);
	if (!TestNotNull(TEXT("rig"), Rig)) return false;
	WoTVRTest::FObserver Observer;
	Observer.Attach(*F.Player);
	Scope.Session->SetHead(FVector::ZeroVector, FRotator::ZeroRotator);
	Rig->RequestMode(EWoTVRMode::VR, TEXT("spec"));
	F.Frames(2);
	const double ViewYaw = WoTVRTest::FinalView(*F.Player).Rotation.Yaw;
	const double AimYaw = PC->GetControlRotation().Yaw;
	if (!TestTrue(TEXT("forced turn starts"), WoTVRLifecycleForcedYaw(*PC, true, 16384.0f))) return false;
	F.Frames(10);
	TestEqual(TEXT("the forced turn started once"), Observer.Count(EWoTLocalPlayerEvent::ForcedTurnStarted), 1);
	TestTrue(TEXT("the aim shows the forced turn"), WoTVRLifecycleYawError(AimYaw, PC->GetControlRotation().Yaw) > 10.0);
	TestTrue(TEXT("the rendered head never takes the forced rotation during the turn"),
		WoTVRLifecycleYawError(ViewYaw, WoTVRTest::FinalView(*F.Player).Rotation.Yaw) < 0.01);
	WoTVRLifecycleForcedYaw(*PC, false, 0.0f);
	F.Frames(2);
	TestEqual(TEXT("the forced turn ended once"), Observer.Count(EWoTLocalPlayerEvent::ForcedTurnEnded), 1);
	TestTrue(TEXT("after the turn the view takes the aim's heading"),
		WoTVRLifecycleYawError(WoTVRTest::FinalView(*F.Player).Rotation.Yaw, PC->GetControlRotation().Yaw) < 0.05);
	TestEqual(TEXT("no gameplay yaw left over"), FWoTLocalPlayerHooks::PeekGameplayYaw(*F.Player), 0.0, 0.05);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVRLifecycleSessionLossSpec, "WoT.VR.Lifecycle.SessionLossHoldsStill", WoTVRTest::Flags)
bool FWoTVRLifecycleSessionLossSpec::RunTest(const FString&)
{
	WoTVRTest::FScope Scope;
	WoTVRTest::FMatchWorld F;
	if (!F.Init(*this)) return false;
	ACharacter* Pawn = Cast<ACharacter>(F.Restart(*this));
	UWoTVRPlayerSubsystem* Rig = WoTVRTest::Rig(F.Player);
	if (!TestNotNull(TEXT("pawn"), Pawn) || !TestNotNull(TEXT("rig"), Rig)) return false;
	WoTVRTest::FObserver Observer;
	Observer.Attach(*F.Player);
	F.Frames(20);
	Scope.Session->SetHead(FVector::ZeroVector, FRotator::ZeroRotator);
	Rig->RequestMode(EWoTVRMode::VR, TEXT("spec"));
	F.Frames(3);
	WoTVRTest::PressKey(*F.PC, EKeys::W);
	F.Frames(20);
	TestTrue(TEXT("walking"), Pawn->GetVelocity().Size2D() > 50.0);
	const int32 Contexts = Observer.Count(EWoTLocalPlayerEvent::InputContextChanged);
	AddExpectedError(TEXT("holding still with input released"), EAutomationExpectedErrorFlags::Contains, 1);
	Scope.Session->LoseSession();
	F.Frames(2);
	TestTrue(TEXT("loss holds the player still"), Rig->IsHoldingStill() && F.PC->IsMoveInputIgnored());
	TestEqual(TEXT("loss releases held input once"),
		Observer.Count(EWoTLocalPlayerEvent::InputContextChanged) - Contexts, 1);
	TestFalse(TEXT("the held key is released"), F.PC->PlayerInput->IsPressed(EKeys::W));
	const FRotator Held = F.PC->GetControlRotation();
	F.Frames(60);
	TestTrue(TEXT("the body comes to rest"), Pawn->GetVelocity().Size2D() < 1.0);
	TestTrue(TEXT("the aim holds (never handed to mouse aim)"), F.PC->GetControlRotation().Equals(Held, 0.01));
	TestTrue(TEXT("VR stays on through the loss"), Rig->IsVR() && Rig->IsRigAttached());

	// The head turned while the session was lost; recovery keeps the aim.
	Scope.Session->SetHead(FVector(0.05, 0.0, 0.0), FRotator(0.0, 60.0, 0.0));
	Scope.Session->RecoverSession();
	F.Frames(2);
	TestFalse(TEXT("recovery resumes play"), Rig->IsHoldingStill() || F.PC->IsMoveInputIgnored());
	TestTrue(TEXT("recovery continues from the held aim"),
		WoTVRLifecycleYawError(Held.Yaw, F.PC->GetControlRotation().Yaw) < 0.05);
	Scope.Session->SetHead(FVector(0.05, 0.0, 0.0), FRotator(0.0, 80.0, 0.0));
	F.Frames(2);
	TestEqual(TEXT("the head drives the aim again"),
		FMath::FindDeltaAngleDegrees(Held.Yaw, F.PC->GetControlRotation().Yaw), 20.0, 0.05);
	TestTrue(TEXT("no movement without a fresh press"), Pawn->GetVelocity().Size2D() < 1.0);
	WoTVRTest::PressKey(*F.PC, EKeys::W);
	F.Frames(20);
	TestTrue(TEXT("a fresh press walks again"), Pawn->GetVelocity().Size2D() > 50.0);
	WoTVRTest::ReleaseKey(*F.PC, EKeys::W);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVRLifecycleHoldSpec, "WoT.VR.Lifecycle.HoldSurvivesRespawnAndKeepsMatchEnd", WoTVRTest::Flags)
bool FWoTVRLifecycleHoldSpec::RunTest(const FString&)
{
	WoTVRTest::FScope Scope;
	WoTVRTest::FMatchWorld F;
	if (!F.Init(*this)) return false;
	ACharacter* Pawn = Cast<ACharacter>(F.Restart(*this));
	UWoTVRPlayerSubsystem* Rig = WoTVRTest::Rig(F.Player);
	UClass* GameClass = WoTVRTest::GameClass(TEXT("WoTGameMode"));
	// The plugin links no game class: the rules are reached through their
	// virtual interface once the type is checked by name.
	AWoTGameMode* Game = F.Game && GameClass && F.Game->IsA(GameClass) ? static_cast<AWoTGameMode*>(F.Game) : nullptr;
	if (!TestNotNull(TEXT("pawn"), Pawn) || !TestNotNull(TEXT("rig"), Rig) || !TestNotNull(TEXT("game rules"), Game)) return false;
	WoTVRTest::FObserver Observer;
	Observer.Attach(*F.Player);
	F.Frames(20);
	Scope.Session->SetHead(FVector::ZeroVector, FRotator::ZeroRotator);
	Rig->RequestMode(EWoTVRMode::VR, TEXT("spec"));
	F.Frames(3);
	AddExpectedError(TEXT("holding still with input released"), EAutomationExpectedErrorFlags::Contains, 0);
	Scope.Session->LoseSession();
	F.Frames(2);
	TestTrue(TEXT("the loss holds the player still"), Rig->IsHoldingStill() && F.PC->IsMoveInputIgnored());

	// Death and a respawn while held: the new pawn's restart resets the
	// controller's own ignore-input state, but the hold is the plugin's.
	WoTVRTest::SetIntProperty(Pawn, TEXT("MaxHealth"), 100);
	WoTVRTest::SetIntProperty(Pawn, TEXT("Health"), 25);
	Pawn->TakeDamage(26.0f, FDamageEvent(), nullptr, nullptr);
	F.Frames(2);
	TestNull(TEXT("dead"), F.PC->GetPawn());
	Observer.Command(EWoTLocalPlayerCommand::Fire);
	Observer.Command(EWoTLocalPlayerCommand::CeaseFire);
	F.Frames(2);
	ACharacter* Respawned = Cast<ACharacter>(F.PC->GetPawn());
	if (!TestNotNull(TEXT("respawned while held"), Respawned)) return false;
	TestTrue(TEXT("the hold survives the respawn"), Rig->IsHoldingStill() && F.PC->IsMoveInputIgnored());
	WoTVRTest::PressKey(*F.PC, EKeys::W);
	F.Frames(20);
	TestTrue(TEXT("the respawned body holds still while the session is lost"), Respawned->GetVelocity().Size2D() < 1.0);
	WoTVRTest::ReleaseKey(*F.PC, EKeys::W);
	F.Frames(2);

	// The match ends while held; the session recovers afterwards. The game's
	// own match-end freeze must stay.
	Game->EndGame(TEXT("spec"));
	F.Frames(2);
	TestTrue(TEXT("the match-end freeze applies"), F.PC->IsMoveInputIgnored());
	Scope.Session->RecoverSession();
	F.Frames(3);
	TestFalse(TEXT("the session recovered"), Rig->IsHoldingStill());
	TestTrue(TEXT("the recovery never lifts the match-end freeze"), F.PC->IsMoveInputIgnored());
	Rig->RequestMode(EWoTVRMode::Flat, TEXT("spec"));
	F.Frames(2);
	TestTrue(TEXT("switching to flat never lifts the match-end freeze"), F.PC->IsMoveInputIgnored());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVRLifecycleForcedTurnFoldSpec, "WoT.VR.Lifecycle.NoFoldDuringForcedTurn", WoTVRTest::Flags)
bool FWoTVRLifecycleForcedTurnFoldSpec::RunTest(const FString&)
{
	WoTVRTest::FScope Scope;
	WoTLocalPlayerHooksTest::FPlayerWorld F;
	if (!F.Init(*this)) return false;
	APlayerController* PC = F.PC;
	ACharacter* Pawn = F.Pawn;
	UWoTVRPlayerSubsystem* Rig = WoTVRTest::Rig(F.Player);
	if (!TestNotNull(TEXT("rig"), Rig)) return false;
	WoTVRTest::FObserver Observer;
	Observer.Attach(*F.Player);
	Scope.Session->SetHead(FVector::ZeroVector, FRotator::ZeroRotator);
	Rig->RequestMode(EWoTVRMode::VR, TEXT("spec"));
	F.Frames(2);
	const double ViewYaw = WoTVRTest::FinalView(*F.Player).Rotation.Yaw;
	const double AimYaw = PC->GetControlRotation().Yaw;
	if (!TestTrue(TEXT("forced turn starts"), WoTVRLifecycleForcedYaw(*PC, true, 16384.0f))) return false;
	F.Frames(10);
	TestTrue(TEXT("the aim shows the forced turn"), WoTVRLifecycleYawError(AimYaw, PC->GetControlRotation().Yaw) > 10.0);
	auto ExpectHeadUnturned = [&](const TCHAR* Label)
	{
		const double Now = WoTVRTest::FinalView(*F.Player).Rotation.Yaw;
		TestTrue(FString::Printf(TEXT("%s: the rendered head never takes the forced rotation (%.2f -> %.2f)"),
			Label, ViewYaw, Now), WoTVRLifecycleYawError(ViewYaw, Now) < 0.05);
	};
	ExpectHeadUnturned(TEXT("during the turn"));

	// A teleport during the turn re-anchors the head but folds nothing.
	const int32 Teleports = Observer.Count(EWoTLocalPlayerEvent::Teleported);
	Pawn->TeleportTo(Pawn->GetActorLocation() + FVector(1500.0, 0.0, 0.0), Pawn->GetActorRotation(), false, true);
	F.Frames(1);
	TestEqual(TEXT("one teleport"), Observer.Count(EWoTLocalPlayerEvent::Teleported) - Teleports, 1);
	ExpectHeadUnturned(TEXT("after a teleport in the turn"));

	// A loss that recovers during the turn.
	AddExpectedError(TEXT("holding still with input released"), EAutomationExpectedErrorFlags::Contains, 0);
	Scope.Session->LoseSession();
	F.Frames(2);
	Scope.Session->RecoverSession();
	F.Frames(2);
	TestFalse(TEXT("recovered during the turn"), Rig->IsHoldingStill());
	ExpectHeadUnturned(TEXT("after a hold ends in the turn"));

	// A recentre during the turn.
	TestTrue(TEXT("recentre during the turn"), Rig->Recentre(TEXT("spec")));
	F.Frames(1);
	ExpectHeadUnturned(TEXT("after a recentre in the turn"));
	TestTrue(TEXT("the aim still shows the forced turn"),
		WoTVRLifecycleYawError(AimYaw, PC->GetControlRotation().Yaw) > 10.0);

	// Once the turn ends the view takes the aim, with nothing left over.
	WoTVRLifecycleForcedYaw(*PC, false, 0.0f);
	F.Frames(2);
	TestTrue(TEXT("after the turn the view takes the aim's heading"),
		WoTVRLifecycleYawError(WoTVRTest::FinalView(*F.Player).Rotation.Yaw, PC->GetControlRotation().Yaw) < 0.05);
	TestEqual(TEXT("no gameplay yaw left over"), FWoTLocalPlayerHooks::PeekGameplayYaw(*F.Player), 0.0, 0.05);
	return !HasAnyErrors();
}

#if WITH_EDITOR
namespace
{
struct FWoTVRTravelViewport
{
	FWorldContext* Context = nullptr;
	UGameViewportClient* Viewport = nullptr;
	TSharedPtr<SViewport> Widget;
	TSharedPtr<FSceneViewport> Scene;

	bool Init(FAutomationTestBase& Test, UGameInstance& Instance)
	{
		Context = Instance.GetWorldContext();
		if (!Test.TestNotNull(TEXT("travel world context"), Context)) return false;
		Viewport = NewObject<UGameViewportClient>(GEngine, GEngine->GameViewportClientClass);
		Viewport->Init(*Context, &Instance);
		Context->GameViewport = Viewport;
		Widget = SNew(SViewport);
		Scene = Viewport->CreateViewport(Widget);
		Scene->SetInitialSize(FIntPoint(640, 480));
		Viewport->AddAssociation(*Scene);
		return true;
	}

	~FWoTVRTravelViewport()
	{
		if (Viewport && Scene) Viewport->RemoveAssociation(*Scene);
		Scene.Reset();
		Widget.Reset();
		if (Context) Context->GameViewport = nullptr;
	}
};

bool WoTVRTravelInit(FAutomationTestBase& Test, UE::Net::FTestWorldInstance& Instance)
{
	UGameInstance* GameInstance = NewObject<UGameInstance>(GEngine);
	if (!Test.TestNotNull(TEXT("travel game instance"), GameInstance)) return false;
	GameInstance->AddToRoot();
	FGameInstancePIEParameters Parameters;
	Parameters.bSimulateInEditor = false;
	Parameters.bAnyBlueprintErrors = false;
	Parameters.bStartInSpectatorMode = false;
	Parameters.bRunAsDedicated = false;
	Parameters.bIsPrimaryPIEClient = false;
	Parameters.WorldFeatureLevel = ERHIFeatureLevel::Num;
	Parameters.EditorPlaySettings = nullptr;
	Parameters.NetMode = PIE_Standalone;
	Parameters.OverrideMapURL = TEXT("/Engine/Maps/Entry");
	int32 PIEInstance = INDEX_NONE;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		PIEInstance = FMath::Max(PIEInstance, Context.PIEInstance);
	}
	UE::Net::FScopedNetTestPIERestoration Restoration;
	UE::SetPlayInEditorID(-1);
	const FGameInstancePIEResult Initialized = GameInstance->InitializeForPlayInEditor(PIEInstance + 1, Parameters);
	if (!Test.TestTrue(TEXT("travel instance initializes"), Initialized.IsSuccess()))
	{
		GameInstance->Shutdown();
		GameInstance->RemoveFromRoot();
		return false;
	}
	Instance.GameInstance = GameInstance;
	return true;
}

void WoTVRTravelTick(UE::Net::FTestWorldInstance& Instance, int32 Frames)
{
	for (int32 Frame = 0; Frame < Frames; ++Frame)
	{
		UE::Net::FScopedNetTestPIERestoration Restoration;
		UE::SetPlayInEditorID(-1);
		Instance.Tick(1.0f / 60.0f);
		++GFrameCounter;
	}
}

bool WoTVRTravel(FAutomationTestBase& Test, UE::Net::FTestWorldInstance& Instance, const TCHAR* URL,
	UClass* ModeClass)
{
	const TWeakObjectPtr<UWorld> Previous = Instance.GetWorld();
	{
		UE::Net::FScopedNetTestPIERestoration Restoration;
		UE::SetPlayInEditorID(-1);
		if (!Test.TestTrue(TEXT("server travel accepted"), Previous.IsValid() && Previous->ServerTravel(URL, true)))
		{
			return false;
		}
	}
	for (int32 Frame = 0; Frame < 180; ++Frame)
	{
		WoTVRTravelTick(Instance, 1);
		UWorld* Current = Instance.GetWorld();
		if (Current && Current != Previous.Get() && Current->HasBegunPlay() && Current->GetAuthGameMode()
			&& Current->GetAuthGameMode()->IsA(ModeClass))
		{
			return true;
		}
	}
	Test.AddError(TEXT("travel did not reach the requested game mode"));
	return false;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVRLifecycleTravelSpec, "WoT.VR.Lifecycle.FrontEndMatchAndBackInVR", WoTVRTest::Flags)
bool FWoTVRLifecycleTravelSpec::RunTest(const FString&)
{
	WoTVRTest::FScope Scope;
	UE::Net::FScopedTestSettings Settings;
	UE::Net::FTestWorldInstance Instance(true);
	if (!WoTVRTravelInit(*this, Instance)) return false;
	FWoTVRTravelViewport Viewport;
	if (!Viewport.Init(*this, *Instance.GameInstance)) return false;
	FString PlayerError;
	ULocalPlayer* Player = Instance.GameInstance->CreateLocalPlayer(0, PlayerError, false);
	if (!TestNotNull(TEXT("persistent local player"), Player)) return false;
	ON_SCOPE_EXIT { FWoTLocalPlayerHooks::DetachAll(*Player); };
	UWoTVRPlayerSubsystem* Rig = WoTVRTest::Rig(Player);
	if (!TestNotNull(TEXT("rig"), Rig)) return false;
	{
		UE::Net::FScopedNetTestPIERestoration Restoration;
		UE::SetPlayInEditorID(-1);
		FString Error;
		const FURL URL(nullptr, TEXT("/Engine/Maps/Entry?Game=/Script/WoT.WoTFrontEndGameMode"), TRAVEL_Absolute);
		Instance.bIsLoaded = GEngine->Browse(*Instance.GetWorldContext(), URL, Error) == EBrowseReturnVal::Success;
		if (!TestTrue(FString::Printf(TEXT("front end loads: %s"), *Error), Instance.IsLoaded())) return false;
	}
	APlayerController* FrontEnd = Player->PlayerController;
	if (!TestNotNull(TEXT("front-end controller"), FrontEnd)) return false;
	WoTVRTravelTick(Instance, 2);
	Scope.Session->SetHead(FVector::ZeroVector, FRotator::ZeroRotator);
	TestTrue(TEXT("switch to VR in the front end"), Rig->RequestMode(EWoTVRMode::VR, TEXT("spec")));
	WoTVRTravelTick(Instance, 2);
	TestTrue(TEXT("VR in the front end"), Rig->HasEnteredVR());
	const int32 Switches = Rig->GetSwitchCount();

	UClass* MatchMode = WoTVRTest::GameClass(TEXT("WoTGameMode"));
	if (!WoTVRTravel(*this, Instance, TEXT("/Engine/Maps/Entry?Game=/Script/WoT.WoTGameMode"), MatchMode)) return false;
	APlayerController* Controller = Player->PlayerController;
	if (!TestNotNull(TEXT("gameplay controller after travel"), Controller)) return false;
	UWorld* Match = Instance.GetWorld();
	AGameModeBase* Game = Match->GetAuthGameMode();
	WoTVRTest::SpawnBlock(Match, FVector(0, 0, -10), FVector(6000, 6000, 10));
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Match->SpawnActor<AActor>(WoTVRTest::GameClass(TEXT("WoTPlayerStart")), FVector(1000, 0, 48), FRotator::ZeroRotator, Params);
	if (!Controller->GetPawn())
	{
		Game->RestartPlayer(Controller);
	}
	APawn* Pawn = Controller->GetPawn();
	if (!TestNotNull(TEXT("the match possesses a pawn"), Pawn)) return false;
	WoTVRTravelTick(Instance, 3);
	TestTrue(TEXT("VR survives the travel"), Rig->IsVR() && Rig->IsRigAttached());
	TestEqual(TEXT("travel needs no switch or re-entry"), Rig->GetSwitchCount(), Switches);
	TestTrue(TEXT("the new controller's aim is the head's"), FWoTLocalPlayerHooks::OwnsAim(Controller));
	FMinimalViewInfo Final = WoTVRTest::FinalView(*Player);
	TestTrue(TEXT("the match view is anchored at the new body's eye"),
		FVector2D(Final.Location - Pawn->GetPawnViewLocation()).IsNearlyZero(0.05));
	TestEqual(TEXT("the match world runs at the VR scale"), Match->GetWorldSettings()->WorldToMeters, 52.5f, 0.001f);

	UClass* FrontEndMode = WoTVRTest::GameClass(TEXT("WoTFrontEndGameMode"));
	if (!WoTVRTravel(*this, Instance, TEXT("/Engine/Maps/Entry?Game=/Script/WoT.WoTFrontEndGameMode"), FrontEndMode)) return false;
	APlayerController* Returned = Player->PlayerController;
	if (!TestNotNull(TEXT("front-end controller after the return"), Returned)) return false;
	WoTVRTravelTick(Instance, 3);
	TestNull(TEXT("the front end is pawnless again"), Returned->GetPawn());
	TestTrue(TEXT("VR survives the return"), Rig->IsVR() && Rig->IsRigAttached());
	TestEqual(TEXT("still no re-entry"), Rig->GetSwitchCount(), Switches);
	Final = WoTVRTest::FinalView(*Player);
	TestTrue(TEXT("the returned front end uses the neutral anchor"),
		Final.Location.Equals(Returned->PlayerCameraManager->GetCameraCacheView().Location, 0.05));
	TestTrue(TEXT("switch to flat after the round trip"), Rig->RequestMode(EWoTVRMode::Flat, TEXT("spec")));
	TestFalse(TEXT("nothing stays attached"), FWoTLocalPlayerHooks::HasConsumers(Player));
	return !HasAnyErrors();
}
#endif // WITH_EDITOR

#endif // WITH_AUTOMATION_TESTS
