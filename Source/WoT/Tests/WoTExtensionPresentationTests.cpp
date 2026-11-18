#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "WoTTestWorld.h"
#include "../WoTExtensionSeams.h"
#include "../WoTCharacter.h"
#include "../WoTPlayerController.h"
#include "../WoTPlayerState.h"
#include "../Inventory/WoTSeal.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../Inventory/WoTAngrealInvFireball.h"
#include "AIController.h"
#include "Engine/World.h"
#include "GameFramework/SpectatorPawn.h"

namespace
{
constexpr EAutomationTestFlags ExtensionFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

struct FExtensionWorld
{
	FWoTExtensionSeams Saved = FWoTExtensionSeams::Get();
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTPlayerController* Controller = nullptr;
	AWoTCharacter* Character = nullptr;

	template <typename T> T* Spawn()
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		return World->SpawnActor<T>(FVector(0, 0, 100), FRotator::ZeroRotator, Params);
	}

	FExtensionWorld()
	{
		Controller = World->SpawnActorDeferred<AWoTPlayerController>(
			AWoTPlayerController::StaticClass(), FTransform::Identity);
		Controller->SetAsLocalPlayerController();
		Controller->FinishSpawning(FTransform::Identity);
		Character = Spawn<AWoTCharacter>();
		Character->Health = 100;
		Controller->Possess(Character);
		Controller->SetViewTarget(Character);
	}

	~FExtensionWorld()
	{
		WoTTest::DestroySpecWorld(World);
		FWoTExtensionSeams::Get() = MoveTemp(Saved);
	}

	void AddItem()
	{
		Character->InventoryComponent->AddInventory(Spawn<AWoTAngrealInvFireball>());
		Character->SelectHand(0);
	}

	bool ViewActive() const
	{
		const auto& Query = FWoTExtensionSeams::Get().IsFirstPersonViewActive;
		return Query && Query();
	}
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTExtensionPresentationUnboundSpec,
	"WoT.Extension.PresentationUnbound", ExtensionFlags)
bool FWoTExtensionPresentationUnboundSpec::RunTest(const FString&)
{
	FWoTExtensionSeams Seams;
	TestFalse(TEXT("press has no listeners"), Seams.LocalFirePressed.IsBound());
	TestFalse(TEXT("release has no listeners"), Seams.LocalFireReleased.IsBound());
	Seams.LocalFirePressed.Broadcast(TEXT("FixtureItem"));
	Seams.LocalFireReleased.Broadcast();
	TestFalse(TEXT("unbound view query is inactive"),
		Seams.IsFirstPersonViewActive && Seams.IsFirstPersonViewActive());
	TestNull(TEXT("unbound factory creates nothing"), Seams.SpawnItemPresentation
		? Seams.SpawnItemPresentation(nullptr, NAME_None, FTransform::Identity) : nullptr);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTExtensionLocalFireSpec,
	"WoT.Extension.LocalPlayerFire", ExtensionFlags)
bool FWoTExtensionLocalFireSpec::RunTest(const FString&)
{
	FExtensionWorld Fixture;
	Fixture.AddItem();
	int32 Presses = 0, Releases = 0;
	FName PressedClass;
	auto& Seams = FWoTExtensionSeams::Get();
	Seams.LocalFirePressed.AddLambda([&](FName ClassName) { ++Presses; PressedClass = ClassName; });
	Seams.LocalFireReleased.AddLambda([&]() { ++Releases; });
	const FName Selected = Seams.SelectedItemClass ? Seams.SelectedItemClass() : NAME_None;
	TestFalse(TEXT("fixture selected an item"), Selected.IsNone());
	TestEqual(TEXT("selection uses the shipped class name"), Selected, FName(TEXT("AngrealInvFireball")));
	Fixture.Character->UseAngreal();
	Fixture.Character->CeaseUsingAngreal();
	TestEqual(TEXT("local press broadcasts once"), Presses, 1);
	TestEqual(TEXT("local release broadcasts once"), Releases, 1);
	TestEqual(TEXT("press uses the selected script class key"), PressedClass, Selected);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTExtensionAIFireSpec,
	"WoT.Extension.AIFireIgnored", ExtensionFlags)
bool FWoTExtensionAIFireSpec::RunTest(const FString&)
{
	FExtensionWorld Fixture;
	Fixture.AddItem();
	Fixture.Spawn<AAIController>()->Possess(Fixture.Character);
	int32 Events = 0;
	FWoTExtensionSeams::Get().LocalFirePressed.AddLambda([&](FName) { ++Events; });
	FWoTExtensionSeams::Get().LocalFireReleased.AddLambda([&]() { ++Events; });
	Fixture.Character->UseAngreal();
	Fixture.Character->CeaseUsingAngreal();
	TestEqual(TEXT("a locally controlled AI publishes no player events"), Events, 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTExtensionUnpossessedFireSpec,
	"WoT.Extension.UnpossessedFireIgnored", ExtensionFlags)
bool FWoTExtensionUnpossessedFireSpec::RunTest(const FString&)
{
	FExtensionWorld Fixture;
	Fixture.AddItem();
	Fixture.Controller->UnPossess();
	int32 Events = 0;
	FWoTExtensionSeams::Get().LocalFirePressed.AddLambda([&](FName) { ++Events; });
	FWoTExtensionSeams::Get().LocalFireReleased.AddLambda([&]() { ++Events; });
	Fixture.Character->UseAngreal();
	Fixture.Character->CeaseUsingAngreal();
	TestEqual(TEXT("an unpossessed pawn publishes no player events"), Events, 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTExtensionObjectiveFireSpec,
	"WoT.Extension.ObjectivePressIgnored", ExtensionFlags)
bool FWoTExtensionObjectiveFireSpec::RunTest(const FString&)
{
	FExtensionWorld Fixture;
	Fixture.Character->InventoryComponent->AddInventory(Fixture.Spawn<AWoTSeal>());
	Fixture.Character->SelectHand(9);
	TestNotNull(TEXT("objective item selected"), Cast<AWoTSeal>(Fixture.Character->InventoryComponent->SelectedItem));
	int32 Presses = 0;
	FWoTExtensionSeams::Get().LocalFirePressed.AddLambda([&](FName) { ++Presses; });
	Fixture.Character->UseAngreal();
	TestEqual(TEXT("placing the objective never broadcasts a cast attempt"), Presses, 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTExtensionRemoteFireSpec,
	"WoT.Extension.RemotePlayerFireIgnored", ExtensionFlags)
bool FWoTExtensionRemoteFireSpec::RunTest(const FString&)
{
	FExtensionWorld Fixture;
	Fixture.AddItem();
	AWoTPlayerController* Remote = Fixture.Spawn<AWoTPlayerController>();
	Remote->Possess(Fixture.Character);
	TestFalse(TEXT("fixture controller is remote"), Remote->IsLocalController());
	int32 Events = 0;
	FWoTExtensionSeams::Get().LocalFirePressed.AddLambda([&](FName) { ++Events; });
	FWoTExtensionSeams::Get().LocalFireReleased.AddLambda([&]() { ++Events; });
	Fixture.Character->UseAngreal();
	Fixture.Character->CeaseUsingAngreal();
	TestEqual(TEXT("remote player publishes no local fire events"), Events, 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTExtensionFireGatesSpec,
	"WoT.Extension.FireEarlyExits", ExtensionFlags)
bool FWoTExtensionFireGatesSpec::RunTest(const FString&)
{
	FExtensionWorld Fixture;
	int32 Presses = 0;
	FWoTExtensionSeams::Get().LocalFirePressed.AddLambda([&](FName) { ++Presses; });
	Fixture.Character->UseAngreal();
	TestEqual(TEXT("empty hand consumes the press without broadcasting"), Presses, 0);
	Fixture.AddItem();
	Fixture.Controller->bTeleportingDisabled = true;
	Fixture.Character->UseAngreal();
	TestEqual(TEXT("pre-match gate publishes no fire"), Presses, 0);
	Fixture.Controller->bTeleportingDisabled = false;
	Fixture.Character->BeginHardLanding();
	Fixture.Character->UseAngreal();
	TestEqual(TEXT("landing gate publishes no fire"), Presses, 0);
	Fixture.Character->EndHardLanding();
	Fixture.Controller->OwnerEditSessionTeam = 0;
	Fixture.Character->HandleCitadelEditStateChanged(true);
	Fixture.Character->UseAngreal();
	TestEqual(TEXT("editor consumes the press without publishing fire"), Presses, 0);
	Fixture.Character->CeaseUsingAngreal();
	Fixture.Character->HandleCitadelEditStateChanged(false);
	Fixture.Controller->OwnerEditSessionTeam = 255;
	Fixture.Controller->NotifyMatchEnded();
	Fixture.Character->UseAngreal();
	TestEqual(TEXT("match-end gate publishes no fire"), Presses, 0);
	return !HasAnyErrors();
}

#define WOT_VIEW_SPEC(Type, Leaf, Setup) \
IMPLEMENT_SIMPLE_AUTOMATION_TEST(Type, "WoT.Extension.View." Leaf, ExtensionFlags) \
bool Type::RunTest(const FString&) \
{ \
	FExtensionWorld Fixture; \
	TestTrue(TEXT("living local pawn starts in first person"), Fixture.ViewActive()); \
	Setup; \
	TestFalse(TEXT("view is inactive for this state"), Fixture.ViewActive()); \
	return !HasAnyErrors(); \
}

WOT_VIEW_SPEC(FWoTExtensionViewNoPawnSpec, "NoPawn", Fixture.Controller->UnPossess())
WOT_VIEW_SPEC(FWoTExtensionViewDeadSpec, "DeadPawn", Fixture.Character->bDead = true)
WOT_VIEW_SPEC(FWoTExtensionViewHealthSpec, "ZeroHealth", Fixture.Character->Health = 0)
WOT_VIEW_SPEC(FWoTExtensionViewTargetSpec, "OtherTarget", Fixture.Controller->SetViewTarget(Fixture.Spawn<AActor>()))
WOT_VIEW_SPEC(FWoTExtensionViewOrbitSpec, "EditorOrbit", Fixture.Controller->OwnerEditSessionTeam = 0; Fixture.Controller->CitadelEditCameraMode = EWoTCitadelEditCameraMode::Orbit)
WOT_VIEW_SPEC(FWoTExtensionViewRoamSpec, "EditorRoam", Fixture.Controller->OwnerEditSessionTeam = 0; Fixture.Controller->CitadelEditCameraMode = EWoTCitadelEditCameraMode::Roam)
WOT_VIEW_SPEC(FWoTExtensionViewEditSpec, "EditorSession", Fixture.Controller->OwnerEditSessionTeam = 0)
WOT_VIEW_SPEC(FWoTExtensionViewEndSpec, "MatchEnded", Fixture.Controller->NotifyMatchEnded())
WOT_VIEW_SPEC(FWoTExtensionViewDyingSpec, "DyingState", Fixture.Controller->ArmRespawnFire(Fixture.Character, 1.0f))
WOT_VIEW_SPEC(FWoTExtensionViewSpectatingSpec, "SpectatingState", Fixture.Controller->ChangeState(NAME_Spectating))
WOT_VIEW_SPEC(FWoTExtensionViewSpectatorPawnSpec, "NonCharacterPawn", Fixture.Controller->Possess(Fixture.Spawn<ASpectatorPawn>()))
WOT_VIEW_SPEC(FWoTExtensionViewSpectatorFlagSpec, "SpectatorFlag", Fixture.Controller->GetPlayerState<APlayerState>()->SetIsSpectator(true))
WOT_VIEW_SPEC(FWoTExtensionViewOnlySpectatorSpec, "OnlySpectatorFlag", Fixture.Controller->GetPlayerState<APlayerState>()->SetIsOnlyASpectator(true))
WOT_VIEW_SPEC(FWoTExtensionViewAuthorityEditSpec, "AuthorityEditorSession", Fixture.Controller->EditSessionTeam = 0)
WOT_VIEW_SPEC(FWoTExtensionViewCameraModeSpec, "EditorCameraModeOnly", Fixture.Controller->CitadelEditCameraMode = EWoTCitadelEditCameraMode::Roam)

#undef WOT_VIEW_SPEC

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTExtensionViewTeardownSpec,
	"WoT.Extension.View.ControllerTeardown", ExtensionFlags)
bool FWoTExtensionViewTeardownSpec::RunTest(const FString&)
{
	FExtensionWorld Fixture;
	TestTrue(TEXT("controller binds the query"), Fixture.ViewActive());
	Fixture.Controller->Destroy();
	TestFalse(TEXT("controller teardown clears query"),
		static_cast<bool>(FWoTExtensionSeams::Get().IsFirstPersonViewActive));
	return !HasAnyErrors();
}

#endif
