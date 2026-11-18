// G34 citadel-editor ground-seal placement: validation, per-player card
// arithmetic, edit-end credit, retail-dead capture/removal, and preview isolation.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../AI/WoTPathNode.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../Inventory/WoTSeal.h"
#include "../Traps/WoTSpear.h"
#include "../WoTBattleGameMode.h"
#include "../WoTBattleInfo.h"
#include "../WoTBudgetInfo.h"
#include "../WoTCharacter.h"
#include "../WoTFactionCharacters.h"
#include "../WoTParticleSprayer.h"
#include "../WoTPlayerController.h"
#include "../WoTPlayerState.h"
#include "../WoTSealAltar.h"
#include "../WoTZoneVolume.h"
#include "WoTTestBattleClasses.h"

#include "Components/BoxComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/WorldSettings.h"

namespace
{
	FActorSpawnParameters G34AlwaysSpawnParams()
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		return Params;
	}

	UWorld* G34CreateWorld(bool bBattleMode = false)
	{
		UWorld* World = bBattleMode
			? WoTTest::CreateSpecWorld(AWoTBattleGameMode::StaticClass())
			: WoTTest::CreateSpecWorld();

		if (bBattleMode)
		{
			AWoTBattleInfo* Battle = World->SpawnActorDeferred<AWoTBattleInfo>(
				AWoTBattleInfo::StaticClass(), FTransform::Identity);
			Battle->NumTeams = 2;
			Battle->TeamClass[0] = AWoTTestTeamAlpha::StaticClass();
			Battle->TeamClass[1] = AWoTTestTeamBravo::StaticClass();
			Battle->FinishSpawning(FTransform::Identity);
			Battle->bTeleportersDisabled = true;
			if (AWoTBattleGameMode* Mode =
				World->GetAuthGameMode<AWoTBattleGameMode>())
			{
				Mode->Battle = Battle;
			}
		}
		return World;
	}

	AActor* G34SpawnFloor(UWorld* World, const FVector& Center,
		const FVector& Extent, ECollisionChannel ObjectType = ECC_WorldStatic)
	{
		AActor* Floor = World->SpawnActor<AActor>(
			Center, FRotator::ZeroRotator, G34AlwaysSpawnParams());
		UBoxComponent* Box = NewObject<UBoxComponent>(Floor);
		Box->SetBoxExtent(Extent);
		Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Box->SetCollisionObjectType(ObjectType);
		Box->SetCollisionResponseToAllChannels(ECR_Block);
		Floor->SetRootComponent(Box);
		Box->RegisterComponent();
		Floor->SetActorLocation(Center);
		return Floor;
	}

	AWoTPathNode* G34SpawnNode(UWorld* World, const FVector& FloorPoint,
		float BaseOffsetZ = 0.0f)
	{
		AWoTPathNode* Node = World->SpawnActor<AWoTPathNode>(
			FloorPoint + FVector(0, 0, 50.0f + BaseOffsetZ),
			FRotator::ZeroRotator, G34AlwaysSpawnParams());
		if (Node)
		{
			Node->NodeCollisionHeight = 50.0f;
		}
		return Node;
	}

	AWoTZoneVolume* G34SpawnEditZone(UWorld* World, uint8 Team)
	{
		AWoTZoneVolume* Zone = World->SpawnActorDeferred<AWoTZoneVolume>(
			AWoTZoneVolume::StaticClass(), FTransform::Identity);
		Zone->EditTeam = Team;
		Zone->bAllowEditing = true;
		if (IsValid(Zone->Collision))
		{
			Zone->Collision->SetBoxExtent(FVector(2500));
		}
		Zone->FinishSpawning(FTransform::Identity);
		return Zone;
	}

	AWoTSealAltar* G34SpawnAltar(UWorld* World, uint8 Team,
		const FVector& Location, int32 SealGoal = 4)
	{
		const FTransform Transform(FRotator::ZeroRotator, Location);
		AWoTSealAltar* Altar = World->SpawnActorDeferred<AWoTSealAltar>(
			AWoTSealAltar::StaticClass(), Transform);
		Altar->Team = Team;
		Altar->SealGoal = SealGoal;
		Altar->FinishSpawning(Transform);
		return Altar;
	}

	AWoTBudgetInfo* G34SpawnBudget(UWorld* World, uint8 Team, int32 Seals)
	{
		AWoTBudgetInfo* Budget = World->SpawnActor<AWoTBudgetInfo>(
			G34AlwaysSpawnParams());
		Budget->Team = Team;
		FWoTBudgetResource Row;
		Row.ResourcePackageName = TEXT("WOT");
		Row.ResourceAdapterName = TEXT("SealInventory");
		Row.ItemCount = Seals;
		Budget->Budget.Add(Row);
		return Budget;
	}

	AWoTTestSealNotifyController* G34SpawnController(UWorld* World, uint8 Team,
		const FVector& PawnLocation, bool bFactionPawn = false)
	{
		AWoTTestSealNotifyController* Controller =
			World->SpawnActor<AWoTTestSealNotifyController>(G34AlwaysSpawnParams());
		if (!IsValid(Controller))
		{
			return nullptr;
		}
		Controller->InitPlayerState();
		if (AWoTPlayerState* PS = Controller->GetPlayerState<AWoTPlayerState>())
		{
			PS->Team = Team;
		}
		AWoTCharacter* Pawn = bFactionPawn
			? World->SpawnActor<AWoTAesSedai>(PawnLocation, FRotator::ZeroRotator,
				G34AlwaysSpawnParams())
			: World->SpawnActor<AWoTCharacter>(PawnLocation, FRotator::ZeroRotator,
				G34AlwaysSpawnParams());
		if (IsValid(Pawn))
		{
			Pawn->Health = 100.0f;
			Controller->Possess(Pawn);
		}
		return Controller;
	}

	AWoTSeal* G34FindGroundSeal(UWorld* World)
	{
		for (TActorIterator<AWoTSeal> It(World); It; ++It)
		{
			if (IsValid(*It) && It->bGroundPlaced && !It->bPreviewSeal)
			{
				return *It;
			}
		}
		return nullptr;
	}

	int32 G34CountSeals(UWorld* World)
	{
		int32 Count = 0;
		for (TActorIterator<AWoTSeal> It(World); It; ++It)
		{
			if (IsValid(*It))
			{
				++Count;
			}
		}
		return Count;
	}

	int32 G34SoundCount(const AWoTTestSealNotifyController* Controller, FName Sound)
	{
		int32 Count = 0;
		if (Controller)
		{
			for (const FName& Played : Controller->UISounds)
			{
				Count += Played == Sound ? 1 : 0;
			}
		}
		return Count;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG34DeployValidationSpec,
	"WoT.Citadel.SealPlacement.DeployValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTG34DeployValidationSpec::RunTest(const FString&)
{
	UWorld* World = G34CreateWorld();
	G34SpawnFloor(World, FVector(0, 0, -10), FVector(1200, 1200, 10));

	AWoTPathNode* Node = G34SpawnNode(World, FVector::ZeroVector, 20.0f);
	AWoTSeal* Seal = World->SpawnActor<AWoTSeal>(G34AlwaysSpawnParams());
	TestFalse(TEXT("node base 20uu above the floor is rejected"),
		Seal->DeployResource(FVector(0, 0, 8), FVector::UpVector));
	Node->SetActorLocation(FVector(0, 0, 50));
	TestFalse(TEXT("normal below 0.9 is rejected"),
		Seal->DeployResource(FVector(0, 0, 8), FVector(0.6f, 0.0f, 0.8f)));

	// AWoTCharacter's capsule is retail-parity (17, 46): the summed cylinder is
	// XY 17+17=34 / Z 46+3=49, so the occupant stands 20uu off the spot — inside
	// the occupancy cylinder but clear of the 16uu down-trace line.
	AWoTCharacter* Occupant = World->SpawnActor<AWoTCharacter>(
		FVector(220, 0, 46), FRotator::ZeroRotator, G34AlwaysSpawnParams());
	Occupant->Health = 100.0f;
	APlayerController* OccupantController =
		World->SpawnActor<APlayerController>(G34AlwaysSpawnParams());
	OccupantController->Possess(Occupant);
	AWoTSeal* OccupiedSeal = World->SpawnActor<AWoTSeal>(G34AlwaysSpawnParams());
	TestFalse(TEXT("a standing player pawn blocks the summed cylinder"),
		OccupiedSeal->DeployResource(FVector(200, 0, 8), FVector::UpVector));

	AWoTParticleSprayer* HiddenSprayer = World->SpawnActor<AWoTParticleSprayer>(
		FVector(400, 0, 0), FRotator::ZeroRotator, G34AlwaysSpawnParams());
	HiddenSprayer->SetActorHiddenInGame(true);
	HiddenSprayer->SetActorEnableCollision(false);
	AWoTSeal* SprayerSeal = World->SpawnActor<AWoTSeal>(G34AlwaysSpawnParams());
	TestFalse(TEXT("a hidden collision-less ParticleSprayer still blocks"),
		SprayerSeal->DeployResource(FVector(400, 0, 8), FVector::UpVector));
	HiddenSprayer->Destroy();

	G34SpawnNode(World, FVector(650, 0, 0));
	AWoTSeal* ExistingSeal = World->SpawnActor<AWoTSeal>(
		FVector(650, 0, 3), FRotator::ZeroRotator, G34AlwaysSpawnParams());
	ExistingSeal->bGroundPlaced = true;
	ExistingSeal->SetActorEnableCollision(false);
	AWoTSeal* BesideSeal = World->SpawnActor<AWoTSeal>(G34AlwaysSpawnParams());
	TestTrue(TEXT("another ground seal is Inventory-ignored"),
		BesideSeal->DeployResource(FVector(650, 0, 8), FVector::UpVector));

	G34SpawnFloor(World, FVector(2000, 0, -10), FVector(100, 100, 10),
		ECC_WorldDynamic);
	G34SpawnNode(World, FVector(2000, 0, 0));
	AWoTSeal* DynamicFloorSeal = World->SpawnActor<AWoTSeal>(G34AlwaysSpawnParams());
	TestFalse(TEXT("down trace refuses non-WorldStatic geometry"),
		DynamicFloorSeal->DeployResource(FVector(2000, 0, 8), FVector::UpVector));

	G34SpawnNode(World, FVector(900, 0, 0));
	AWoTSeal* Accepted = World->SpawnActor<AWoTSeal>(G34AlwaysSpawnParams());
	TestTrue(TEXT("node in XY/Z range accepts a clear floor spot"),
		Accepted->DeployResource(FVector(900, 0, 8), FVector::UpVector));
	TestFalse(TEXT("accepted ground seal is visible"), Accepted->IsHidden());
	TestFalse(TEXT("accepted ground seal collision stays disabled"),
		Accepted->GetActorEnableCollision());
	TestTrue(TEXT("accepted seal is stamped ground-placed"), Accepted->bGroundPlaced);
	TestEqual(TEXT("accepted seal sits floor + CollisionHeight"),
		Accepted->GetActorLocation(), FVector(900, 0, 3));
	TestEqual(TEXT("accepted seal rotation is zero"),
		Accepted->GetActorRotation(), FRotator::ZeroRotator);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG34CardArithmeticSpec,
	"WoT.Citadel.SealPlacement.CardArithmetic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTG34CardArithmeticSpec::RunTest(const FString&)
{
	UWorld* World = G34CreateWorld(true);
	G34SpawnFloor(World, FVector(0, 0, -10), FVector(2000, 2000, 10));
	G34SpawnNode(World, FVector(400, 0, 0));
	G34SpawnEditZone(World, 0);
	AWoTSealAltar* Altar = G34SpawnAltar(World, 0, FVector::ZeroVector, 3);
	G34SpawnAltar(World, 1, FVector(5000, 0, 0), 3);
	AWoTBudgetInfo* Budget = G34SpawnBudget(World, 0, 3);
	FWoTBudgetResource TrapRow;
	TrapRow.ResourcePackageName = TEXT("WoTTraps");
	TrapRow.ResourceAdapterName = TEXT("SpearInventory");
	TrapRow.ItemCount = 0;
	Budget->Budget.Add(TrapRow);
	AWoTTestSealNotifyController* Controller =
		G34SpawnController(World, 0, FVector(-800, 0, 88));
	if (!TestNotNull(TEXT("controller spawned"), Controller))
	{
		WoTTest::DestroySpecWorld(World);
		return true;
	}

	TestEqual(TEXT("per-player seal card starts at one"),
		Controller->OwnerSealCardCount, 1);
	Controller->CitadelBeginEdit();
	Controller->ServerCreateResource(
		TEXT("SealInventory"), FVector(400, 0, 8), FVector::UpVector);
	TestEqual(TEXT("ground placement spends the card"),
		Controller->OwnerSealCardCount, 0);
	TestEqual(TEXT("ground placement spends team budget 3 to 2"),
		Budget->SealBudgetRemaining(), 2);
	TestEqual(TEXT("ground placement plays DeploySeal"),
		G34SoundCount(Controller, FName(TEXT("WOT/DeploySeal"))), 1);
	AWoTSpear* RefundedTrap = World->SpawnActor<AWoTSpear>(
		FVector(-400, 600, 0), FRotator::ZeroRotator, G34AlwaysSpawnParams());
	if (TestNotNull(TEXT("refund fixture trap spawned"), RefundedTrap))
	{
		RefundedTrap->Team = 0;
		Controller->ServerRemoveResource(RefundedTrap);
	}
	TestEqual(TEXT("a trap refund never changes the independent seal card"),
		Controller->OwnerSealCardCount, 0);
	TestEqual(TEXT("trap removal refunds only its own budget row"),
		Budget->GetItemCount(Budget->GetIndex(TEXT("SpearInventory"))), 1);

	// Review pin (edit-end ordering): retail runs every EndEditingResource sweep
	// BEFORE Budget.EndEdit's authored EndEditEvent (WOTPlayer.uc:5195-5211) —
	// the event is MP-live (Battle3_02 authors three), and a listener must never
	// observe a resource still in its editing state.
	AWoTSeal* GroundSeal = G34FindGroundSeal(World);
	if (TestNotNull(TEXT("ground seal exists before edit end"), GroundSeal))
	{
		TestTrue(TEXT("ground seal glows during the session"),
			GroundSeal->bEditingGlow);
	}
	Budget->EndEditEvent = TEXT("G34EndEditOrder");
	AWoTTestSealEditOrderSink* OrderSink =
		World->SpawnActorDeferred<AWoTTestSealEditOrderSink>(
			AWoTTestSealEditOrderSink::StaticClass(), FTransform::Identity);
	OrderSink->TagName = TEXT("G34EndEditOrder");
	OrderSink->WatchedSeal = GroundSeal;
	OrderSink->FinishSpawning(FTransform::Identity);
	Controller->CitadelEndEdit();
	TestEqual(TEXT("EndEditEvent fired once at edit end"),
		OrderSink->TriggerCount, 1);
	TestFalse(TEXT("EndEditEvent listener never saw the editing glow"),
		OrderSink->bSawEditingGlow);
	TestEqual(TEXT("edit end banks the remaining two seals"),
		Budget->SealBudgetRemaining(), 0);
	TestEqual(TEXT("two auto-placed seals drive the card negative"),
		Controller->OwnerSealCardCount, -2);
	TestTrue(TEXT("ground plus banked seals reach the goal"),
		Altar->CheckForWinner());

	// A team-budget refund does not revive the independent per-player card.
	Budget->Increment(TEXT("SealInventory"));
	const int32 SealCountBeforeRetry = G34CountSeals(World);
	const int32 SoundCountBeforeRetry = Controller->UISounds.Num();
	Controller->CitadelBeginEdit();
	Controller->ServerCreateResource(
		TEXT("SealInventory"), FVector(600, 0, 8), FVector::UpVector);
	TestEqual(TEXT("second-session create spawns nothing"),
		G34CountSeals(World), SealCountBeforeRetry);
	TestEqual(TEXT("second-session create plays no sound"),
		Controller->UISounds.Num(), SoundCountBeforeRetry);
	TestEqual(TEXT("budget refund never changes the owner card"),
		Controller->OwnerSealCardCount, -2);
	TestEqual(TEXT("seal HUD count reads the card, not team budget"),
		Controller->GetOwnerCitadelBudgetCount(TEXT("SealInventory")), -2);
	Controller->CitadelEndEdit();

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG34GroundCreditSpec,
	"WoT.Citadel.SealPlacement.GroundCredit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTG34GroundCreditSpec::RunTest(const FString&)
{
	UWorld* World = G34CreateWorld(true);
	G34SpawnFloor(World, FVector(0, 0, -10), FVector(2000, 2000, 10));
	G34SpawnNode(World, FVector(400, 0, 0));
	G34SpawnEditZone(World, 0);
	AWoTSealAltar* Altar = G34SpawnAltar(World, 0, FVector::ZeroVector, 3);
	G34SpawnAltar(World, 1, FVector(5000, 0, 0), 3);
	G34SpawnBudget(World, 0, 3);
	AWoTTestSealNotifyController* Controller =
		G34SpawnController(World, 0, FVector(-800, 0, 88), true);
	if (!TestNotNull(TEXT("faction controller spawned"), Controller))
	{
		WoTTest::DestroySpecWorld(World);
		return true;
	}
	Controller->CitadelBeginEdit();
	Controller->ServerCreateResource(
		TEXT("SealInventory"), FVector(400, 0, 8), FVector::UpVector);
	AWoTSeal* GroundSeal = G34FindGroundSeal(World);
	Controller->CitadelEndEdit();

	if (TestNotNull(TEXT("ground seal survives edit end"), GroundSeal))
	{
		TestEqual(TEXT("ground seal credited to session team"),
			GroundSeal->ControllingTeam, uint8(0));
		TestEqual(TEXT("credit sweep applies the placing faction"),
			GroundSeal->ControllingFactionName, FName(TEXT("AesSedai")));
	}
	AWoTBattleGameMode* Mode = World->GetAuthGameMode<AWoTBattleGameMode>();
	TestEqual(TEXT("ground plus two banked seals count for the team"),
		Mode->Battle->GetTeamSealCount(0), 3);
	TestTrue(TEXT("altar winner check includes the ground seal credit"),
		Altar->CheckForWinner());

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG34UncapturableSpec,
	"WoT.Citadel.SealPlacement.Uncapturable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTG34UncapturableSpec::RunTest(const FString&)
{
	UWorld* World = G34CreateWorld(true);
	G34SpawnFloor(World, FVector(0, 0, -10), FVector(2000, 2000, 10));
	G34SpawnNode(World, FVector(400, 0, 0));
	G34SpawnEditZone(World, 0);
	G34SpawnAltar(World, 0, FVector::ZeroVector);
	G34SpawnAltar(World, 1, FVector(5000, 0, 0));
	AWoTBudgetInfo* Budget = G34SpawnBudget(World, 0, 1);
	AWoTTestSealNotifyController* Controller =
		G34SpawnController(World, 0, FVector(-800, 0, 88));
	if (!TestNotNull(TEXT("placing controller spawned"), Controller))
	{
		WoTTest::DestroySpecWorld(World);
		return true;
	}
	Controller->CitadelBeginEdit();
	Controller->ServerCreateResource(
		TEXT("SealInventory"), FVector(400, 0, 8), FVector::UpVector);
	AWoTSeal* GroundSeal = G34FindGroundSeal(World);

	AWoTTestSealNotifyController* ToucherController =
		G34SpawnController(World, 1, FVector(400, 0, 88));
	if (!TestNotNull(TEXT("touching controller spawned"), ToucherController))
	{
		WoTTest::DestroySpecWorld(World);
		return true;
	}
	WoTTest::TickWorld(World, 2);
	AWoTCharacter* Toucher = Cast<AWoTCharacter>(ToucherController->GetPawn());
	if (TestNotNull(TEXT("overlapping pawn exists"), Toucher))
	{
		TestEqual(TEXT("collision-off ground seal grants no inventory"),
			Toucher->InventoryComponent->Items.Num(), 0);
	}
	const int32 SoundsBeforeRemove = Controller->UISounds.Num();
	Controller->ServerRemoveResource(GroundSeal);
	TestTrue(TEXT("remove RPC leaves ground seal alive"), IsValid(GroundSeal));
	TestEqual(TEXT("remove RPC gives no budget refund"),
		Budget->SealBudgetRemaining(), 0);
	TestEqual(TEXT("remove RPC plays no sound"),
		Controller->UISounds.Num(), SoundsBeforeRemove);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG34GhostNonInterferenceSpec,
	"WoT.Citadel.SealPlacement.GhostNonInterference",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTG34GhostNonInterferenceSpec::RunTest(const FString&)
{
	UWorld* World = G34CreateWorld(true);
	G34SpawnFloor(World, FVector(0, 0, -10), FVector(2000, 2000, 10));
	G34SpawnNode(World, FVector(400, 0, 0));
	G34SpawnEditZone(World, 0);
	G34SpawnAltar(World, 0, FVector::ZeroVector);
	G34SpawnAltar(World, 1, FVector(5000, 0, 0));
	G34SpawnBudget(World, 0, 1);
	AWoTTestSealNotifyController* Controller =
		G34SpawnController(World, 0, FVector(-800, 0, 88));
	if (!TestNotNull(TEXT("ghost fixture controller spawned"), Controller))
	{
		WoTTest::DestroySpecWorld(World);
		return true;
	}

	AWoTSeal* Preview = World->SpawnActor<AWoTSeal>(G34AlwaysSpawnParams());
	if (!TestNotNull(TEXT("preview seal spawned"), Preview))
	{
		WoTTest::DestroySpecWorld(World);
		return true;
	}
	Preview->bPreviewSeal = true;
	Preview->SetFlags(RF_Transient);
	Preview->SetReplicates(false);
	TestTrue(TEXT("preview can borrow the retail placement test"),
		Preview->DeployResource(FVector(400, 0, 8), FVector::UpVector));
	TestEqual(TEXT("preview deploy never stamps Team"), Preview->Team, uint8(255));
	TestFalse(TEXT("preview deploy never stamps ground state"), Preview->bGroundPlaced);

	Controller->CitadelBeginEdit();
	TestEqual(TEXT("begin-edit sweep skips preview Team stamp"),
		Preview->Team, uint8(255));
	Controller->ServerCreateResource(
		TEXT("SealInventory"), FVector(400, 0, 8), FVector::UpVector);
	TestNotNull(TEXT("preview seal does not block a real placement"),
		G34FindGroundSeal(World));
	Preview->Team = 0; // prove the end-credit exclusion independently of Team.
	Preview->SetControllingTeam(Controller);
	TestEqual(TEXT("preview rejects direct team credit"),
		Preview->ControllingTeam, uint8(255));
	Controller->CitadelEndEdit();
	TestEqual(TEXT("preview is excluded from edit-end team credit"),
		Preview->ControllingTeam, uint8(255));
	AWoTBattleGameMode* Mode = World->GetAuthGameMode<AWoTBattleGameMode>();
	TestEqual(TEXT("only the real ground seal contributes credit"),
		Mode->Battle->GetTeamSealCount(0), 1);

	WoTTest::DestroySpecWorld(World);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
