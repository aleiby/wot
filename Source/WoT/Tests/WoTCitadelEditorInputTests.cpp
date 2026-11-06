// Slice 2g: real editor selection state through the input-independent click dispatcher.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../Inventory/WoTHandInfo.h"
#include "../Inventory/WoTHandSet.h"
#include "../WoTBattleGameMode.h"
#include "../WoTBattleInfo.h"
#include "../WoTBudgetInfo.h"
#include "../WoTCharacter.h"
#include "../WoTPlayerController.h"
#include "../WoTPlayerState.h"
#include "../WoTSealAltar.h"
#include "../WoTZoneVolume.h"
#include "../Traps/WoTAlarm.h"
#include "../Traps/WoTPortcullis.h"
#include "../Traps/WoTSpear.h"
#include "../Traps/WoTTrap.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/BoxComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/WorldSettings.h"

namespace
{
	FActorSpawnParameters EditorInputSpecAlwaysSpawnParams()
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		return Params;
	}

	UWorld* EditorInputSpecCreateWorld()
	{
		UWorld* World = WoTTest::CreateSpecWorld(AWoTBattleGameMode::StaticClass());

		AWoTBattleInfo* Battle = World->SpawnActorDeferred<AWoTBattleInfo>(
			AWoTBattleInfo::StaticClass(), FTransform::Identity);
		if (IsValid(Battle))
		{
			Battle->NumTeams = 2;
			Battle->TeamClass[0] = AWoTCharacter::StaticClass();
			Battle->TeamClass[1] = AWoTCharacter::StaticClass();
			Battle->bTeleportersDisabled = true;
			Battle->FinishSpawning(FTransform::Identity);
			if (AWoTBattleGameMode* BattleMode =
				World->GetAuthGameMode<AWoTBattleGameMode>())
			{
				BattleMode->Battle = Battle;
			}
		}
		return World;
	}

	void EditorInputSpecAddBudget(
		AWoTBudgetInfo* Budget, FName AdapterName, int32 ItemCount)
	{
		FWoTBudgetResource Row;
		Row.ResourcePackageName = TEXT("WoTTraps");
		Row.ResourceAdapterName = AdapterName;
		Row.ItemCount = ItemCount;
		Budget->Budget.Add(Row);
	}

	AWoTZoneVolume* EditorInputSpecSpawnZone(UWorld* World)
	{
		const FTransform Transform(FRotator::ZeroRotator, FVector::ZeroVector);
		AWoTZoneVolume* Zone = World->SpawnActorDeferred<AWoTZoneVolume>(
			AWoTZoneVolume::StaticClass(), Transform);
		if (IsValid(Zone))
		{
			Zone->EditTeam = 1;
			Zone->bAllowEditing = true;
			Zone->Collision->SetBoxExtent(FVector(2000.0f));
			Zone->FinishSpawning(Transform);
		}
		return Zone;
	}

	AActor* EditorInputSpecSpawnFloor(UWorld* World)
	{
		AActor* Floor = World->SpawnActor<AActor>(
			FVector::ZeroVector, FRotator::ZeroRotator,
			EditorInputSpecAlwaysSpawnParams());
		if (!IsValid(Floor))
		{
			return nullptr;
		}
		UBoxComponent* Box = NewObject<UBoxComponent>(Floor);
		Box->SetBoxExtent(FVector(1000.0f, 1000.0f, 10.0f));
		Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Box->SetCollisionObjectType(ECC_WorldStatic);
		Box->SetCollisionResponseToAllChannels(ECR_Block);
		Floor->SetRootComponent(Box);
		Box->RegisterComponent();
		return Floor;
	}

	struct FEditorInputSpecFixture
	{
		UWorld* World = nullptr;
		AWoTPlayerController* Controller = nullptr;
		AWoTCharacter* Character = nullptr;
		AWoTBudgetInfo* Budget = nullptr;
		AWoTZoneVolume* Zone = nullptr;
	};

	FEditorInputSpecFixture EditorInputSpecCreateFixture(
		int32 SpearCount = 2, int32 PortcullisCount = 2)
	{
		FEditorInputSpecFixture Fixture;
		Fixture.World = EditorInputSpecCreateWorld();
		Fixture.Zone = EditorInputSpecSpawnZone(Fixture.World);
		EditorInputSpecSpawnFloor(Fixture.World);
		AWoTSealAltar* Altar = Fixture.World->SpawnActor<AWoTSealAltar>(
			FVector::ZeroVector, FRotator::ZeroRotator,
			EditorInputSpecAlwaysSpawnParams());
		if (IsValid(Altar))
		{
			Altar->Team = 1;
		}

		Fixture.Budget = Fixture.World->SpawnActor<AWoTBudgetInfo>(
			EditorInputSpecAlwaysSpawnParams());
		Fixture.Budget->Team = 1;
		EditorInputSpecAddBudget(
			Fixture.Budget, TEXT("SpearInventory"), SpearCount);
		EditorInputSpecAddBudget(
			Fixture.Budget, TEXT("AlarmInventory"), 1);
		EditorInputSpecAddBudget(
			Fixture.Budget, TEXT("WallInventory"), 1);
		EditorInputSpecAddBudget(
			Fixture.Budget, TEXT("PortcullisInventory"), PortcullisCount);

		Fixture.Controller = Fixture.World->SpawnActor<AWoTPlayerController>(
			EditorInputSpecAlwaysSpawnParams());
		Fixture.Controller->SetAsLocalPlayerController();
		Fixture.Controller->InitPlayerState();
		if (AWoTPlayerState* PlayerState =
			Fixture.Controller->GetPlayerState<AWoTPlayerState>())
		{
			PlayerState->Team = 1;
		}
		Fixture.Character = Fixture.World->SpawnActor<AWoTCharacter>(
			FVector(0.0f, 0.0f, 300.0f), FRotator::ZeroRotator,
			EditorInputSpecAlwaysSpawnParams());
		Fixture.Controller->Possess(Fixture.Character);
		Fixture.Controller->CitadelBeginEdit();
		return Fixture;
	}

	void EditorInputSpecAim(
		FEditorInputSpecFixture& Fixture, const FVector& Location,
		const FVector& Direction)
	{
		Fixture.Character->SetActorLocation(Location);
		Fixture.Controller->SetControlRotation(Direction.Rotation());
		// Headless GetPlayerViewPoint (used by EditorTrace) resolves to the pawn's
		// GetActorEyesViewPoint = actor location + ACTOR rotation, not the control rotation,
		// so aim the pawn's actor rotation too. (In real play the camera follows the control
		// rotation; the trace direction is the same either way.)
		Fixture.Character->SetActorRotation(Direction.Rotation());
		if (APlayerCameraManager* CameraManager = Fixture.Controller->PlayerCameraManager)
		{
			CameraManager->UpdateCamera(0.0f);
		}
	}

	int32 EditorInputSpecBudgetCount(
		const FEditorInputSpecFixture& Fixture, FName AdapterName)
	{
		return Fixture.Budget->GetItemCount(Fixture.Budget->GetIndex(AdapterName));
	}

	int32 EditorInputSpecRealSpearCount(UWorld* World)
	{
		int32 Count = 0;
		for (TActorIterator<AWoTSpear> It(World); It; ++It)
		{
			if (IsValid(*It) && !(*It)->IsPreviewMode())
			{
				++Count;
			}
		}
		return Count;
	}

	int32 EditorInputSpecActionId(EWoTCitadelEditorAction Action)
	{
		return static_cast<int32>(Action);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelEditorInputCreateRemoveSpec,
	"WoT.Citadel.EditorInput.CreateRemoveAndGuards",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelEditorInputCreateRemoveSpec::RunTest(const FString&)
{
	FEditorInputSpecFixture Fixture = EditorInputSpecCreateFixture(1, 1);
	if (!IsValid(Fixture.Controller) || !IsValid(Fixture.Character)
		|| !IsValid(Fixture.Budget) || !IsValid(Fixture.Zone))
	{
		WoTTest::DestroySpecWorld(Fixture.World);
		return false;
	}
	TestTrue(TEXT("edit edge activates editor input context"),
		Fixture.Character->IsCitadelEditorInputActive());
	Fixture.Controller->SetCitadelPaletteSelection(TEXT("SpearInventory"));
	EditorInputSpecAim(Fixture, FVector(0, 0, 300), FVector::DownVector);
	const EWoTCitadelEditorAction FirstAction =
		Fixture.Controller->HandleCitadelEditorClick();
	TestEqual(TEXT("valid floor resolves create"),
		EditorInputSpecActionId(FirstAction),
		EditorInputSpecActionId(EWoTCitadelEditorAction::Create));
	TestEqual(TEXT("create spends the last spear"),
		EditorInputSpecBudgetCount(Fixture, TEXT("SpearInventory")), 0);
	TestEqual(TEXT("create spawns one gameplay spear"),
		EditorInputSpecRealSpearCount(Fixture.World), 1);

	WoTTest::TickWorld(Fixture.World, 1);
	const EWoTCitadelEditorAction RepeatedAction =
		Fixture.Controller->HandleCitadelEditorClick();
	TestEqual(TEXT("repeated click on the placed spear resolves remove"),
		EditorInputSpecActionId(RepeatedAction),
		EditorInputSpecActionId(EWoTCitadelEditorAction::Remove));
	TestEqual(TEXT("remove refunds the spear"),
		EditorInputSpecBudgetCount(Fixture, TEXT("SpearInventory")), 1);
	TestEqual(TEXT("remove destroys the gameplay spear"),
		EditorInputSpecRealSpearCount(Fixture.World), 0);

	// A destroyed former target cannot be dispatched through a later real trace.
	AWoTSpear* Stale = Fixture.World->SpawnActor<AWoTSpear>(
		FVector(0, 0, 100), FRotator::ZeroRotator,
		EditorInputSpecAlwaysSpawnParams());
	Stale->BeginEditingResource(1);
	Stale->Destroy();
	Fixture.Controller->SetCitadelPaletteSelection(NAME_None);
	WoTTest::TickWorld(Fixture.World, 1);
	TestEqual(TEXT("destroyed hit actor produces no stale dispatch"),
		EditorInputSpecActionId(Fixture.Controller->HandleCitadelEditorClick()),
		EditorInputSpecActionId(EWoTCitadelEditorAction::None));

	Fixture.Controller->SetCitadelPaletteSelection(TEXT("SpearInventory"));
	Fixture.Zone->bAllowEditing = false;
	TestEqual(TEXT("invalid viewer zone rejects click before trace dispatch"),
		EditorInputSpecActionId(Fixture.Controller->HandleCitadelEditorClick()),
		EditorInputSpecActionId(EWoTCitadelEditorAction::None));
	Fixture.Zone->bAllowEditing = true;
	Fixture.Controller->CitadelEndEdit();
	TestFalse(TEXT("exit edge disables editor input context"),
		Fixture.Character->IsCitadelEditorInputActive());
	TestEqual(TEXT("exit before click wins over stale palette and view"),
		EditorInputSpecActionId(Fixture.Controller->HandleCitadelEditorClick()),
		EditorInputSpecActionId(EWoTCitadelEditorAction::None));
	TestEqual(TEXT("exit guard leaves budget unchanged"),
		EditorInputSpecBudgetCount(Fixture, TEXT("SpearInventory")), 1);

	WoTTest::TickWorld(Fixture.World, 1);
	WoTTest::DestroySpecWorld(Fixture.World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelEditorInputToggleSpec,
	"WoT.Citadel.EditorInput.ToggleRepeatedAndZeroCount",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelEditorInputToggleSpec::RunTest(const FString&)
{
	FEditorInputSpecFixture Fixture = EditorInputSpecCreateFixture(0, 1);
	AWoTPortcullis* Portcullis = Fixture.World->SpawnActor<AWoTPortcullis>(
		FVector(400, 0, 100), FRotator::ZeroRotator,
		EditorInputSpecAlwaysSpawnParams());
	Portcullis->BeginEditingResource(1);
	Fixture.Controller->SetCitadelPaletteSelection(TEXT("PortcullisInventory"));
	EditorInputSpecAim(Fixture, FVector(0, 0, 100), FVector::ForwardVector);

	TestEqual(TEXT("matching Portcullis resolves toggle"),
		EditorInputSpecActionId(Fixture.Controller->HandleCitadelEditorClick()),
		EditorInputSpecActionId(EWoTCitadelEditorAction::Toggle));
	TestTrue(TEXT("toggle activates Portcullis"), Portcullis->IsActive());
	TestEqual(TEXT("toggle spends last Portcullis count"),
		EditorInputSpecBudgetCount(Fixture, TEXT("PortcullisInventory")), 0);

	TestEqual(TEXT("repeated zero-count hit resolves deactivate"),
		EditorInputSpecActionId(Fixture.Controller->HandleCitadelEditorClick()),
		EditorInputSpecActionId(EWoTCitadelEditorAction::Deactivate));
	TestFalse(TEXT("deactivate turns Portcullis off"), Portcullis->IsActive());
	TestEqual(TEXT("deactivate refunds count"),
		EditorInputSpecBudgetCount(Fixture, TEXT("PortcullisInventory")), 1);

	Fixture.Controller->SetCitadelPaletteSelection(TEXT("SpearInventory"));
	EditorInputSpecAim(Fixture, FVector(0, 300, 300), FVector::DownVector);
	TestEqual(TEXT("zero-count create selection does nothing on floor"),
		EditorInputSpecActionId(Fixture.Controller->HandleCitadelEditorClick()),
		EditorInputSpecActionId(EWoTCitadelEditorAction::None));
	TestEqual(TEXT("zero-count click spawns no spear"),
		EditorInputSpecRealSpearCount(Fixture.World), 0);

	WoTTest::TickWorld(Fixture.World, 1);
	WoTTest::DestroySpecWorld(Fixture.World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelEditorInputPaletteNavigationSpec,
	"WoT.Citadel.EditorInput.PaletteNavigationSyncsGhost",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelEditorInputPaletteNavigationSpec::RunTest(const FString&)
{
	FEditorInputSpecFixture Fixture = EditorInputSpecCreateFixture(1, 1);
	if (!Fixture.Character->CitadelEditorHandSet)
	{
		AddError(TEXT("citadel editor hand set was not installed"));
		WoTTest::DestroySpecWorld(Fixture.World);
		return false;
	}

	Fixture.Character->SelectHand(3);
	TestEqual(TEXT("digit-equivalent hand selection syncs Alarm adapter"),
		Fixture.Controller->GetCitadelPaletteSelection(), FName(TEXT("AlarmInventory")));
	TestTrue(TEXT("Alarm selection creates matching ghost"),
		IsValid(Fixture.Controller->GetCitadelGhostPreview())
		&& Fixture.Controller->GetCitadelGhostPreview()->IsA<AWoTAlarm>());

	Fixture.Character->NextItem();
	TestEqual(TEXT("wheel-next syncs the next populated hand adapter"),
		Fixture.Controller->GetCitadelPaletteSelection(), FName(TEXT("SpearInventory")));
	TestTrue(TEXT("wheel-next replaces ghost with Spear"),
		IsValid(Fixture.Controller->GetCitadelGhostPreview())
		&& Fixture.Controller->GetCitadelGhostPreview()->IsA<AWoTSpear>());

	Fixture.Character->SelectHand(6);
	TestEqual(TEXT("digit selection syncs Wall adapter"),
		Fixture.Controller->GetCitadelPaletteSelection(), FName(TEXT("WallInventory")));
	TestTrue(TEXT("digit selection reconciles Wall ghost"),
		IsValid(Fixture.Controller->GetCitadelGhostPreview())
		&& Fixture.Controller->GetCitadelGhostPreview()->GetClass()
			== WoTCitadelResources::FindByAdapterName(TEXT("WallInventory")).ResourceClass);

	Fixture.Character->PrevItem();
	TestEqual(TEXT("wheel-previous syncs previous populated hand adapter"),
		Fixture.Controller->GetCitadelPaletteSelection(), FName(TEXT("FireWallInventory")));
	TestNull(TEXT("zero-count wheel selection destroys the old ghost"),
		Fixture.Controller->GetCitadelGhostPreview());

	WoTTest::TickWorld(Fixture.World, 1);
	WoTTest::DestroySpecWorld(Fixture.World);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
