#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../Inventory/WoTAngrealInvAirBurst.h"
#include "../Inventory/WoTAngrealInvFireball.h"
#include "../Inventory/WoTAngrealInvHeal.h"
#include "../Inventory/WoTAngrealInvSpecial.h"
#include "../Inventory/WoTBagHolding.h"
#include "../Inventory/WoTHandSet.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../Inventory/WoTSeal.h"
#include "../Traps/WoTPit.h"
#include "../AI/WoTPathNode.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "../WoTBattleGameMode.h"
#include "../WoTCharacter.h"
#include "../WoTGameMode.h"
#include "../WoTPlayerState.h"
#include "../WoTZoneVolume.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "EngineUtils.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/PlayerController.h"
#if WITH_EDITOR
#include "Tests/NetTestHelpers.h"
#endif

namespace
{
constexpr EAutomationTestFlags DropFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

AWoTCharacter* SpawnDropPlayer(UWorld* World, const FVector& At, uint8 Team)
{
	APlayerController* Controller = World->SpawnActor<APlayerController>();
	Controller->InitPlayerState();
	if (AWoTPlayerState* State = Controller->GetPlayerState<AWoTPlayerState>())
	{
		State->Team = Team;
	}
	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(At, FRotator::ZeroRotator);
	Controller->Possess(Pawn);
	return Pawn;
}

template <typename T>
T* GiveDropItem(UWorld* World, AWoTCharacter* Pawn, const FVector& SpawnAt)
{
	T* Item = World->SpawnActor<T>(SpawnAt, FRotator::ZeroRotator);
	if (Item)
	{
		Item->GiveTo(Pawn);
	}
	return Item;
}

AStaticMeshActor* SpawnSealFloor(UWorld* World)
{
	// A +-2000 x +-2000 slab whose top face is z = 0 (engine cube, 100 uu).
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AStaticMeshActor* Floor = World->SpawnActor<AStaticMeshActor>(
		AStaticMeshActor::StaticClass(), FVector(0, 0, -50), FRotator::ZeroRotator, Params);
	if (!Floor)
	{
		return nullptr;
	}
	Floor->SetMobility(EComponentMobility::Movable);
	Floor->GetStaticMeshComponent()->SetStaticMesh(Cast<UStaticMesh>(
		FSoftObjectPath(TEXT("/Engine/BasicShapes/Cube.Cube")).TryLoad()));
	Floor->GetStaticMeshComponent()->SetCollisionProfileName(TEXT("BlockAll"));
	Floor->SetActorScale3D(FVector(40.0f, 40.0f, 1.0f));
	return Floor;
}

AWoTPathNode* SpawnSealPathNode(UWorld* World, const FVector& At, float Height)
{
	AWoTPathNode* Node = World->SpawnActor<AWoTPathNode>(At, FRotator::ZeroRotator);
	if (Node)
	{
		Node->NodeCollisionHeight = Height;
	}
	return Node;
}

TArray<AWoTBagHolding*> BagsIn(UWorld* World)
{
	TArray<AWoTBagHolding*> Bags;
	for (TActorIterator<AWoTBagHolding> It(World); It; ++It)
	{
		if (IsValid(*It))
		{
			Bags.Add(*It);
		}
	}
	return Bags;
}

#if WITH_EDITOR
UE::Net::FTestWorldInstance CreateDropListenServer(const TCHAR* GameClass)
{
	using namespace UE::Net;
	FTestWorldInstance Instance(/*bDelayedInit*/ true);
	UGameInstance* GameInstance = NewObject<UGameInstance>(GEngine);
	GameInstance->AddToRoot();
	FGameInstancePIEParameters Parameters;
	Parameters.bSimulateInEditor = false;
	Parameters.bAnyBlueprintErrors = false;
	Parameters.bStartInSpectatorMode = false;
	Parameters.bRunAsDedicated = false;
	Parameters.bIsPrimaryPIEClient = false;
	Parameters.WorldFeatureLevel = ERHIFeatureLevel::Num;
	Parameters.EditorPlaySettings = nullptr;
	Parameters.NetMode = PIE_ListenServer;
	Parameters.OverrideMapURL = TEXT("/Engine/Maps/Entry");
	int32 PIEInstance = INDEX_NONE;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		PIEInstance = FMath::Max(PIEInstance, Context.PIEInstance);
	}
	++PIEInstance;
	{
		FScopedNetTestPIERestoration Restoration;
		UE::SetPlayInEditorID(-1);
		const FGameInstancePIEResult Initialized =
			GameInstance->InitializeForPlayInEditor(PIEInstance, Parameters);
		if (!Initialized.IsSuccess())
		{
			GameInstance->Shutdown();
			GameInstance->RemoveFromRoot();
			return Instance;
		}
	}
	Instance.GameInstance = GameInstance;
	{
		FScopedNetTestPIERestoration Restoration;
		UE::SetPlayInEditorID(-1);
		const FString URL = FString::Printf(
			TEXT("/Engine/Maps/Entry?listen?Game=/Script/WoT.%s"), GameClass);
		FURL Parsed(nullptr, *URL, TRAVEL_Absolute);
		FString Error;
		Instance.bIsLoaded = GEngine->Browse(
			*GameInstance->GetWorldContext(), Parsed, Error)
			== EBrowseReturnVal::Success;
	}
	return Instance;
}
#endif
}

#if WITH_EDITOR
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBattleSealDeathDropSpec,
	"WoT.Battle.SealDeathDrop", DropFlags)

bool FWoTBattleSealDeathDropSpec::RunTest(const FString&)
{
	UE::Net::FTestWorldInstance Server =
		CreateDropListenServer(TEXT("WoTBattleGameMode"));
	if (!TestTrue(TEXT("listen Battle world loads"), Server.IsLoaded()))
	{
		return false;
	}
	UWorld* World = Server.GetWorld();
	AWoTCharacter* Carrier = SpawnDropPlayer(World, FVector(0, 0, 200), 0);
	AWoTCharacter* Enemy = SpawnDropPlayer(World, FVector(500, 0, 200), 1);
	AWoTSeal* Seal = GiveDropItem<AWoTSeal>(
		World, Carrier, FVector(5000, 0, 200));
	if (!TestNotNull(TEXT("seal carrier"), Carrier)
		|| !TestNotNull(TEXT("enemy"), Enemy)
		|| !TestNotNull(TEXT("carried seal"), Seal))
	{
		return false;
	}
	Carrier->Health = 0;
	Carrier->Died(Enemy->GetController(), NAME_None, Carrier->GetActorLocation());
	TestEqual(TEXT("death drops the seal into pickup state"),
		Seal->GetItemState(), EWoTInventoryState::Pickup);
	TestTrue(TEXT("seal drops one pawn radius forward"),
		Seal->GetActorLocation().Equals(
			FVector(Carrier->GetCapsuleComponent()->GetScaledCapsuleRadius(),
				0, 200), 0.1f));
	TestTrue(TEXT("enemy can take the released seal"), Seal->TryPickupBy(Enemy));
	TestTrue(TEXT("enemy owns the seal after pickup"), Seal->GetOwner() == Enemy);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInventoryMultiplayerDeathBagSpec,
	"WoT.Inventory.Drop.MultiplayerDeathBag", DropFlags)

bool FWoTInventoryMultiplayerDeathBagSpec::RunTest(const FString&)
{
	UE::Net::FTestWorldInstance Server =
		CreateDropListenServer(TEXT("WoTGameMode"));
	if (!TestTrue(TEXT("listen Arena world loads"), Server.IsLoaded()))
	{
		return false;
	}
	UWorld* World = Server.GetWorld();
	AWoTCharacter* Victim = SpawnDropPlayer(World, FVector(0, 0, 200), 0);
	AWoTCharacter* Looter = SpawnDropPlayer(World, FVector(800, 0, 200), 1);
	AWoTAngrealInvFireball* Fireball = GiveDropItem<AWoTAngrealInvFireball>(
		World, Victim, FVector(5000, 0, 200));
	AWoTAngrealInvHeal* Heal = GiveDropItem<AWoTAngrealInvHeal>(
		World, Victim, FVector(5200, 0, 200));
	AWoTAngrealInvAirBurst* Default = GiveDropItem<AWoTAngrealInvAirBurst>(
		World, Victim, FVector(5400, 0, 200));
	Fireball->CurCharges = Fireball->MaxInitialCharges + 9;
	Heal->CurCharges = Heal->MaxInitialCharges;
	Victim->Health = 0;
	Victim->Died(nullptr, NAME_None, Victim->GetActorLocation());

	const TArray<AWoTBagHolding*> Bags = BagsIn(World);
	TestEqual(TEXT("death creates exactly one bag"), Bags.Num(), 1);
	if (Bags.Num() != 1)
	{
		return false;
	}
	AWoTBagHolding* Bag = Bags[0];
	TestEqual(TEXT("bag holds the two non-default angreal"), Bag->GetItems().Num(), 2);
	TestEqual(TEXT("overfull charges clamp to MaxInitialCharges"),
		Fireball->CurCharges, Fireball->MaxInitialCharges);
	TestTrue(TEXT("second angreal remains in the bag"), Bag->GetItems().Contains(Heal));
	TestFalse(TEXT("default angreal is destroyed, not bagged"), IsValid(Default));

	WoTTest::TickWorld(World, 0.05f);
	Bag->GiveTo(Looter);
	TestNotNull(TEXT("looter receives fireball through PickupQuery"),
		Looter->InventoryComponent->FindInventoryType(AWoTAngrealInvFireball::StaticClass()));
	TestNotNull(TEXT("looter receives heal through PickupQuery"),
		Looter->InventoryComponent->FindInventoryType(AWoTAngrealInvHeal::StaticClass()));
	TestTrue(TEXT("looted bag hides during its destroy delay"),
		Bag->IsHidden() && Bag->IsDestroyingBag());
	WoTTest::TickWorld(World, 0.25f);
	TestFalse(TEXT("looted bag destroys after 0.2 seconds"), IsValid(Bag));
	return !HasAnyErrors();
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBattlePitSealDropSpec,
	"WoT.Battle.SealPitDrop", DropFlags)

bool FWoTBattlePitSealDropSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld(AWoTBattleGameMode::StaticClass());
	AWoTCharacter* Carrier = SpawnDropPlayer(World, FVector(0, 0, 200), 0);
	AWoTZoneVolume* Zone = World->SpawnActor<AWoTZoneVolume>(
		FVector::ZeroVector, FRotator::ZeroRotator);
	Zone->Collision->SetBoxExtent(FVector(500));
	Zone->bPitZone = true;
	AWoTPitOpening* Opening = World->SpawnActor<AWoTPitOpening>(
		FVector(100, 50, 0), FRotator::ZeroRotator);
	AWoTPit* Pit = World->SpawnActor<AWoTPit>(
		FVector(900, 700, -100), FRotator::ZeroRotator);
	AWoTSeal* Seal = GiveDropItem<AWoTSeal>(
		World, Carrier, FVector(5000, 0, 200));
	if (!TestNotNull(TEXT("pit opening"), Opening)
		|| !TestNotNull(TEXT("linked pit"), Pit)
		|| !TestNotNull(TEXT("pit seal"), Seal))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Carrier->InventoryComponent->SelectItem(Seal);
	Carrier->Drop();
	TestTrue(TEXT("pit-zone seal moves to pit owner plus 16 Z"),
		Seal->GetActorLocation().Equals(
			Pit->GetActorLocation() + FVector(0, 0, 16), 0.1f));
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBattleSealDropKeepsFloorSpotSpec,
	"WoT.Battle.SealDropKeepsFloorSpot", DropFlags)

bool FWoTBattleSealDropKeepsFloorSpotSpec::RunTest(const FString&)
{
	// Seal.uc:338: SetLocation keeps the drop spot unless the seal's 17 x 17 x 3
	// extent does not fit. 12 uu above a floor fits; a 17-unit sphere did not, and
	// the 2026-10-03 cross-machine run saw a seal leave a Hound's corpse for a
	// point 25,000 uu away.
	UWorld* World = WoTTest::CreateSpecWorld(AWoTBattleGameMode::StaticClass());
	AStaticMeshActor* Floor = SpawnSealFloor(World);
	AWoTPathNode* Far = SpawnSealPathNode(World, FVector(1500, 0, 50), 50.0f);
	AWoTSeal* Seal = World->SpawnActor<AWoTSeal>(FVector(0, 0, 500), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("floor"), Floor) || !TestNotNull(TEXT("path node"), Far)
		|| !TestNotNull(TEXT("seal"), Seal))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	const FVector DropSpot(100, 40, 12);
	Seal->DropFrom(DropSpot);
	TestTrue(TEXT("a seal that fits its drop spot stays there (no path-node fallback)"),
		FVector::Dist2D(Seal->GetActorLocation(), DropSpot) < 1.0f);
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBattleSealDropNearestPathNodeSpec,
	"WoT.Battle.SealDropBlockedUsesNearestPathNode", DropFlags)

bool FWoTBattleSealDropNearestPathNodeSpec::RunTest(const FString&)
{
	// Seal.uc:336-346: only a spot that native FindSpot cannot adjust walks the
	// NavigationPointList and rests on the closest node's cylinder base.
	UWorld* World = WoTTest::CreateSpecWorld(AWoTBattleGameMode::StaticClass());
	AStaticMeshActor* Floor = SpawnSealFloor(World);
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AStaticMeshActor* Blocker = World->SpawnActor<AStaticMeshActor>(
		AStaticMeshActor::StaticClass(), FVector(100, 0, 100),
		FRotator::ZeroRotator, Params);
	if (Blocker)
	{
		Blocker->SetMobility(EComponentMobility::Movable);
		Blocker->GetStaticMeshComponent()->SetStaticMesh(Cast<UStaticMesh>(
			FSoftObjectPath(TEXT("/Engine/BasicShapes/Cube.Cube")).TryLoad()));
		Blocker->GetStaticMeshComponent()->SetCollisionProfileName(TEXT("BlockAll"));
		Blocker->SetActorScale3D(FVector(1.6f));
	}
	AWoTPathNode* Near = SpawnSealPathNode(World, FVector(300, 0, 40), 40.0f);
	AWoTPathNode* Far = SpawnSealPathNode(World, FVector(1800, 0, 50), 50.0f);
	AWoTSeal* Seal = World->SpawnActor<AWoTSeal>(FVector(0, 0, 500), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("floor"), Floor) || !TestNotNull(TEXT("solid blocker"), Blocker)
		|| !TestNotNull(TEXT("near node"), Near)
		|| !TestNotNull(TEXT("far node"), Far) || !TestNotNull(TEXT("seal"), Seal))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	WoTTest::TickWorld(World, 1);
	// The slab must really block the seal's channel, or this spec proves nothing.
	FHitResult FloorHit;
	TestTrue(TEXT("the floor slab blocks a downward WorldStatic trace"),
		World->LineTraceSingleByChannel(FloorHit, FVector(100, 0, 200),
			FVector(100, 0, -200), ECC_WorldStatic));
	// A floor penetration alone is adjustable by retail FindSpot. A solid cube
	// containing the complete axis/corner search neighborhood is genuinely
	// unplaceable and therefore exercises the NavigationPoint fallback.
	int32 NodeCount = 0;
	for (TActorIterator<AWoTPathNode> It(World); It; ++It)
	{
		++NodeCount;
	}
	TestEqual(TEXT("both stamped path nodes are visible to the scan"), NodeCount, 2);
	Seal->DropFrom(FVector(100, 0, 100));
	const FVector Expected(300, 0, 40 - 40 + 3);
	TestTrue(*FString::Printf(TEXT("a blocked drop rests on the nearest node's base (got %s)"),
		*Seal->GetActorLocation().ToString()),
		Seal->GetActorLocation().Equals(Expected, 1.0f));
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInventoryStandaloneDeathSpec,
	"WoT.Inventory.Drop.StandaloneDeathNoBag", DropFlags)

bool FWoTInventoryStandaloneDeathSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
	AWoTCharacter* Victim = SpawnDropPlayer(World, FVector(0, 0, 200), 0);
	AWoTAngrealInvFireball* Fireball = GiveDropItem<AWoTAngrealInvFireball>(
		World, Victim, FVector(5000, 0, 200));
	Victim->Health = 0;
	Victim->Died(nullptr, NAME_None, Victim->GetActorLocation());
	TestTrue(TEXT("standalone creates no death bag"), BagsIn(World).IsEmpty());
	TestFalse(TEXT("standalone discard destroys held artifacts"), IsValid(Fireball));
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInventoryDropReuseSpec,
	"WoT.Inventory.Drop.SelectedAndReuse", DropFlags)

bool FWoTInventoryDropReuseSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
	AWoTCharacter* Pawn = SpawnDropPlayer(World, FVector(0, 0, 200), 0);
	AWoTAngrealInvFireball* Fireball = GiveDropItem<AWoTAngrealInvFireball>(
		World, Pawn, FVector(5000, 0, 200));
	AWoTAngrealInvHeal* Heal = GiveDropItem<AWoTAngrealInvHeal>(
		World, Pawn, FVector(5200, 0, 200));
	Pawn->InventoryComponent->SelectItem(Fireball);
	Pawn->Drop();
	TArray<AWoTBagHolding*> Bags = BagsIn(World);
	TestEqual(TEXT("first selected drop creates one bag"), Bags.Num(), 1);
	Pawn->InventoryComponent->SelectItem(Heal);
	Pawn->Drop();
	Bags = BagsIn(World);
	TestEqual(TEXT("second drop within 160 reuses the bag"), Bags.Num(), 1);
	if (Bags.Num() == 1)
	{
		TestEqual(TEXT("reused bag holds both angreal"), Bags[0]->GetItems().Num(), 2);
	}
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInventoryDropNoOpSpec,
	"WoT.Inventory.Drop.NoOpGuards", DropFlags)

bool FWoTInventoryDropNoOpSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
	AWoTCharacter* Pawn = SpawnDropPlayer(World, FVector(0, 0, 200), 0);
	AWoTAngrealInvAirBurst* Default = GiveDropItem<AWoTAngrealInvAirBurst>(
		World, Pawn, FVector(5000, 0, 200));
	AWoTAngrealInvSpecial* Special = GiveDropItem<AWoTAngrealInvSpecial>(
		World, Pawn, FVector(5200, 0, 200));
	AWoTAngrealInvFireball* Fireball = GiveDropItem<AWoTAngrealInvFireball>(
		World, Pawn, FVector(5400, 0, 200));

	Pawn->InventoryComponent->SelectItem(Default);
	Pawn->Drop();
	TestTrue(TEXT("default angreal cannot be dropped"),
		Pawn->InventoryComponent->Items.Contains(Default));
	Pawn->InventoryComponent->SelectItem(Special);
	Pawn->Drop();
	TestTrue(TEXT("AngrealInvSpecial cannot be dropped"),
		Pawn->InventoryComponent->Items.Contains(Special));
	UWoTHandSet* EditorHands = NewObject<UWoTHandSet>(Pawn);
	EditorHands->Init(Pawn);
	Pawn->CurrentHandSet = EditorHands;
	Pawn->InventoryComponent->SelectItem(Fireball);
	Pawn->Drop();
	TestTrue(TEXT("editor hands suppress Drop"),
		Pawn->InventoryComponent->Items.Contains(Fireball));
	TestTrue(TEXT("guarded drops create no bag"), BagsIn(World).IsEmpty());
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInventoryDropDeadRejectsSpec,
	"WoT.Inventory.Drop.DeadRejectsRPC", DropFlags)

bool FWoTInventoryDropDeadRejectsSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
	AWoTCharacter* Pawn = SpawnDropPlayer(World, FVector(0, 0, 200), 0);
	AWoTAngrealInvFireball* Fireball = GiveDropItem<AWoTAngrealInvFireball>(
		World, Pawn, FVector(5000, 0, 200));
	AWoTSeal* Seal = GiveDropItem<AWoTSeal>(World, Pawn, FVector(5200, 0, 200));
	if (!TestNotNull(TEXT("fireball"), Fireball) || !TestNotNull(TEXT("seal"), Seal))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Fireball->bTainted = true;
	// A dead pawn that is still possessed (match end) and somehow holds items.
	Pawn->bDead = true;
	Pawn->InventoryComponent->SelectItem(Fireball);
	Pawn->ServerDrop_Implementation();
	Pawn->ServerDropTainted_Implementation();
	Pawn->ServerDropSeals_Implementation();
	TestTrue(TEXT("a dead pawn's Drop RPC drops nothing"),
		Pawn->InventoryComponent->Items.Contains(Fireball));
	TestTrue(TEXT("a dead pawn's DropSeals RPC keeps the seal"),
		Pawn->InventoryComponent->Items.Contains(Seal));
	TestTrue(TEXT("rejected drops create no bag"), BagsIn(World).IsEmpty());
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInventoryBagTouchPickupSpec,
	"WoT.Inventory.Drop.BagTouchPickup", DropFlags)

bool FWoTInventoryBagTouchPickupSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
	AWoTCharacter* Dropper = SpawnDropPlayer(World, FVector(0, 0, 200), 0);
	AWoTCharacter* Looter = SpawnDropPlayer(World, FVector(900, 0, 200), 1);
	AWoTAngrealInvFireball* Fireball = GiveDropItem<AWoTAngrealInvFireball>(
		World, Dropper, FVector(5000, 0, 200));
	if (!TestNotNull(TEXT("fireball"), Fireball))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Dropper->InventoryComponent->SelectItem(Fireball);
	Dropper->Drop();
	const TArray<AWoTBagHolding*> Bags = BagsIn(World);
	if (!TestEqual(TEXT("drop makes one bag"), Bags.Num(), 1))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	AWoTBagHolding* Bag = Bags[0];
	TestFalse(TEXT("the dropper no longer holds the bagged angreal"),
		Dropper->InventoryComponent->Items.Contains(Fireball));
	TestTrue(TEXT("the bagged angreal is a hidden held item"),
		Fireball->IsHidden() && Fireball->GetOwner() == nullptr);

	// BagHolding.GiveTo before its first Tick parks the toucher in WaitingPawn.
	TestTrue(TEXT("a touch before the bag's first tick is accepted"),
		Bag->TryPickupBy(Looter));
	TestTrue(TEXT("the deferred touch waits for initialization"),
		Bag->GetItems().Contains(Fireball) && !Bag->IsDestroyingBag());
	WoTTest::TickWorld(World, 0.05f);
	TestTrue(TEXT("the first tick hands the bag to the waiting pawn"),
		Looter->InventoryComponent->Items.Contains(Fireball));
	TestTrue(TEXT("the looted bag starts its destroy delay"), Bag->IsDestroyingBag());
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInventoryBagDestroyedSpec,
	"WoT.Inventory.Drop.DestroyedBagTakesContents", DropFlags)

bool FWoTInventoryBagDestroyedSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
	AWoTCharacter* Dropper = SpawnDropPlayer(World, FVector(0, 0, 200), 0);
	AWoTAngrealInvFireball* Fireball = GiveDropItem<AWoTAngrealInvFireball>(
		World, Dropper, FVector(5000, 0, 200));
	if (!TestNotNull(TEXT("fireball"), Fireball))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Dropper->InventoryComponent->SelectItem(Fireball);
	Dropper->Drop();
	const TArray<AWoTBagHolding*> Bags = BagsIn(World);
	if (!TestEqual(TEXT("drop makes one bag"), Bags.Num(), 1))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	// A mover's encroachment destroys unowned pickups such as an unlooted bag.
	Bags[0]->Destroy();
	TestFalse(TEXT("an unlooted bag's contents go with it"), IsValid(Fireball));
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInventoryDropTaintedSpec,
	"WoT.Inventory.Drop.TaintedOnly", DropFlags)

bool FWoTInventoryDropTaintedSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
	AWoTCharacter* Pawn = SpawnDropPlayer(World, FVector(0, 0, 200), 0);
	AWoTAngrealInvFireball* Tainted = GiveDropItem<AWoTAngrealInvFireball>(
		World, Pawn, FVector(5000, 0, 200));
	AWoTAngrealInvHeal* Clean = GiveDropItem<AWoTAngrealInvHeal>(
		World, Pawn, FVector(5200, 0, 200));
	Tainted->bTainted = true;
	Clean->bTainted = false;
	Pawn->DropTainted();
	TestFalse(TEXT("tainted angreal leaves inventory"),
		Pawn->InventoryComponent->Items.Contains(Tainted));
	TestTrue(TEXT("clean angreal remains held"),
		Pawn->InventoryComponent->Items.Contains(Clean));
	const TArray<AWoTBagHolding*> Bags = BagsIn(World);
	TestTrue(TEXT("tainted drop creates a bag containing only tainted angreal"),
		Bags.Num() == 1 && Bags[0]->GetItems().Num() == 1
		&& Bags[0]->GetItems().Contains(Tainted));
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

#if WITH_EDITOR
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInventoryDropConnectedSpec,
	"WoT.Inventory.Drop.ListenClientRPC", DropFlags)

bool FWoTInventoryDropConnectedSpec::RunTest(const FString&)
{
	using namespace UE::Net;
	FTestWorlds Worlds;
	Worlds.Server = CreateDropListenServer(TEXT("WoTGameMode"));
	Worlds.SetDescription(TEXT("WoT connected inventory Drop"));
	if (!TestTrue(TEXT("listen server loads"), Worlds.Server.IsLoaded())
		|| !TestTrue(TEXT("owner client connects"), Worlds.CreateAndConnectClient()))
	{
		return false;
	}
	AWoTCharacter* ServerPawn = Cast<AWoTCharacter>(
		Worlds.GetServerPlayerControllerOfClient(0)->GetPawn());
	if (!TestNotNull(TEXT("server copy of owner pawn"), ServerPawn))
	{
		return false;
	}
	// Entry has one PlayerStart. Clear it before the second real login so the
	// retail actor-encroachment rejection can succeed on its next candidate.
	ServerPawn->SetActorLocation(FVector(5000, 0, 200));
	ServerPawn->ForceNetUpdate();
	if (!TestTrue(TEXT("peer client connects"), Worlds.CreateAndConnectClient()))
	{
		return false;
	}
	AWoTCharacter* ServerPeer = Cast<AWoTCharacter>(
		Worlds.GetServerPlayerControllerOfClient(1)->GetPawn());
	if (!TestNotNull(TEXT("server copy of peer pawn"), ServerPeer))
	{
		return false;
	}
	ServerPawn->SetActorLocation(FVector(0, 0, 200));
	ServerPeer->SetActorLocation(FVector(1200, 0, 200));
	ServerPawn->ForceNetUpdate();
	ServerPeer->ForceNetUpdate();
	AWoTAngrealInvFireball* ServerFireball =
		GiveDropItem<AWoTAngrealInvFireball>(Worlds.Server.GetWorld(), ServerPawn,
			FVector(5000, 0, 200));
	ServerPawn->InventoryComponent->SelectItem(ServerFireball);
	AWoTCharacter* ClientPawn = Cast<AWoTCharacter>(
		Worlds.Clients[0].GetWorld()->GetFirstPlayerController()->GetPawn());
	if (!TestTrue(TEXT("owner inventory replicates"), Worlds.TickUntil([ClientPawn]()
		{
			return ClientPawn && ClientPawn->InventoryComponent
				&& ClientPawn->InventoryComponent->SelectedItem != nullptr;
		}, 180)))
	{
		return false;
	}
	ClientPawn->Drop();
	const bool bDropReplicated = Worlds.TickUntil([&Worlds]()
		{
			return BagsIn(Worlds.Server.GetWorld()).Num() == 1
				&& BagsIn(Worlds.Clients[1].GetWorld()).Num() == 1;
		}, 180);
	if (!bDropReplicated)
	{
		AddError(FString::Printf(
			TEXT("client Drop did not settle: server bags=%d owner bags=%d peer bags=%d server items=%d server selected=%s server hands=%s/%s client role=%d local=%d controller=%s controller-local=%d"),
			BagsIn(Worlds.Server.GetWorld()).Num(),
			BagsIn(Worlds.Clients[0].GetWorld()).Num(),
			BagsIn(Worlds.Clients[1].GetWorld()).Num(),
			ServerPawn->InventoryComponent->Items.Num(),
			*GetNameSafe(ServerPawn->InventoryComponent->SelectedItem),
			*GetNameSafe(ServerPawn->CurrentHandSet),
			*GetNameSafe(ServerPawn->AngrealHandSet),
			static_cast<int32>(ClientPawn->GetLocalRole()),
			ClientPawn->IsLocallyControlled() ? 1 : 0,
			*GetNameSafe(ClientPawn->GetController()),
			ClientPawn->GetController()
				&& ClientPawn->GetController()->IsLocalController() ? 1 : 0));
	}
	return !HasAnyErrors();
}
#endif

#endif
