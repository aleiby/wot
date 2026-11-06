// Citadel Deploy Core Slice 1: server-authoritative edit sessions, toggle economy,
// resource liveness, and edit-to-play assessment.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../WoTBattleGameMode.h"
#include "../WoTBattleInfo.h"
#include "../WoTBudgetInfo.h"
#include "../WoTCharacter.h"
#include "../WoTCitadelResources.h"
#include "../WoTCollisionBlocker.h"
#include "../WoTMover.h"
#include "../WoTPlayerController.h"
#include "../WoTPlayerState.h"
#include "../WoTRetailText.h"
#include "../WoTSealAltar.h"
#include "../WoTTeleporter.h"
#include "../WoTZoneVolume.h"
#include "../Traps/WoTAlarm.h"
#include "../Traps/WoTFireWall.h"
#include "../Traps/WoTPit.h"
#include "../Traps/WoTPortcullis.h"
#include "../Traps/WoTSpear.h"
#include "../Traps/WoTStaircase.h"
#include "../Traps/WoTTrap.h"
#include "../Traps/WoTTrapBlocker.h"
#include "../Traps/WoTWall.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../Inventory/WoTSeal.h"
#include "WoTTestBattleClasses.h"

#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "CollisionQueryParams.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/WorldSettings.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInterface.h"

#include <limits>

namespace
{
	bool CitadelSpecMaterialParentChainContains(const UMaterialInterface* Material,
		const UMaterialInterface* ExpectedParent)
	{
		const UMaterialInterface* Current = Material;
		while (IsValid(Current))
		{
			if (Current == ExpectedParent)
			{
				return true;
			}
			const UMaterialInstance* Instance = Cast<UMaterialInstance>(Current);
			Current = Instance ? Instance->Parent : nullptr;
		}
		return false;
	}

	FActorSpawnParameters CitadelSpecAlwaysSpawnParams()
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		return Params;
	}

	UWorld* CitadelSpecCreateWorld(bool bBattleMode = false)
	{
		UWorld* World = bBattleMode
			? WoTTest::CreateSpecWorld(AWoTBattleGameMode::StaticClass())
			: WoTTest::CreateSpecWorld();

		if (bBattleMode)
		{
			AWoTBattleInfo* Battle = World->SpawnActorDeferred<AWoTBattleInfo>(
				AWoTBattleInfo::StaticClass(), FTransform::Identity);
			Battle->NumTeams = 3;
			Battle->TeamClass[0] = AWoTTestTeamAlpha::StaticClass();
			Battle->TeamClass[1] = AWoTTestTeamBravo::StaticClass();
			Battle->TeamClass[2] = AWoTTestTeamAlpha::StaticClass();
			Battle->FinishSpawning(FTransform::Identity);
			Battle->bTeleportersDisabled = true;
			if (AWoTBattleGameMode* BattleMode =
				World->GetAuthGameMode<AWoTBattleGameMode>())
			{
				BattleMode->Battle = Battle;
			}
		}
		return World;
	}

	AWoTPlayerController* CitadelSpecSpawnController(
		UWorld* World, uint8 InTeam,
		const FVector& ViewerLocation = FVector::ZeroVector)
	{
		AWoTPlayerController* Controller = World->SpawnActor<AWoTPlayerController>(
			CitadelSpecAlwaysSpawnParams());
		if (!IsValid(Controller))
		{
			return nullptr;
		}
		// AController::SetActorLocation is private in UE5; position its root transform
		// (the pawnless viewer position GetCitadelViewerLocation falls back to).
		if (USceneComponent* Root = Controller->GetRootComponent())
		{
			Root->SetWorldLocation(ViewerLocation);
		}
		Controller->InitPlayerState();
		if (AWoTPlayerState* PlayerState =
			Controller->GetPlayerState<AWoTPlayerState>())
		{
			PlayerState->Team = InTeam;
		}
		return Controller;
	}

	void CitadelSpecSetViewerLocation(
		AWoTPlayerController* Controller, const FVector& ViewerLocation)
	{
		if (IsValid(Controller))
		{
			if (USceneComponent* Root = Controller->GetRootComponent())
			{
				Root->SetWorldLocation(ViewerLocation);
			}
		}
	}

	AWoTZoneVolume* CitadelSpecSpawnEditZone(
		UWorld* World, const FVector& Center, const FVector& Extent,
		uint8 EditTeam, bool bAllowEditing = true)
	{
		const FTransform Transform(FRotator::ZeroRotator, Center);
		AWoTZoneVolume* Zone = World->SpawnActorDeferred<AWoTZoneVolume>(
			AWoTZoneVolume::StaticClass(), Transform);
		if (!IsValid(Zone))
		{
			return nullptr;
		}
		Zone->EditTeam = EditTeam;
		Zone->bAllowEditing = bAllowEditing;
		if (IsValid(Zone->Collision))
		{
			Zone->Collision->SetBoxExtent(Extent);
		}
		Zone->FinishSpawning(Transform);
		return Zone;
	}

	AWoTSealAltar* CitadelSpecSpawnAltar(
		UWorld* World, uint8 InTeam, const FVector& Location)
	{
		const FTransform Transform(FRotator::ZeroRotator, Location);
		AWoTSealAltar* Altar = World->SpawnActorDeferred<AWoTSealAltar>(
			AWoTSealAltar::StaticClass(), Transform);
		Altar->Team = InTeam;
		Altar->FinishSpawning(Transform);
		return Altar;
	}

	void CitadelSpecSetResourceBudget(
		AWoTBudgetInfo* BudgetInfo, FName AdapterName, int32 Count)
	{
		const int32 ExistingIndex = BudgetInfo->GetIndex(AdapterName);
		if (ExistingIndex != INDEX_NONE)
		{
			BudgetInfo->Budget[ExistingIndex].ItemCount = Count;
			return;
		}
		FWoTBudgetResource Resource;
		Resource.ResourcePackageName = TEXT("WoTTraps");
		Resource.ResourceAdapterName = AdapterName;
		Resource.ItemCount = Count;
		BudgetInfo->Budget.Add(Resource);
	}

	void CitadelSpecSetSealBudget(AWoTBudgetInfo* BudgetInfo, int32 Count)
	{
		CitadelSpecSetResourceBudget(BudgetInfo, TEXT("SealInventory"), Count);
	}

	AWoTPortcullis* CitadelSpecSpawnPortcullis(
		UWorld* World, const FVector& Location)
	{
		return World->SpawnActor<AWoTPortcullis>(
			Location, FRotator::ZeroRotator, CitadelSpecAlwaysSpawnParams());
	}

	AWoTPortcullis* CitadelSpecSpawnPortcullisWithMover(
		UWorld* World, const FVector& Location, FName MoverTag)
	{
		const FTransform Transform(FRotator::ZeroRotator, Location);
		AWoTPortcullisMover* Mover = World->SpawnActorDeferred<AWoTPortcullisMover>(
			AWoTPortcullisMover::StaticClass(), Transform);
		Mover->KeyPos = {FVector::ZeroVector, FVector(0, 0, 100)};
		Mover->KeyRot = {FRotator::ZeroRotator, FRotator::ZeroRotator};
		Mover->MoveTime = 0.3f;
		Mover->TagName = MoverTag;
		Mover->FinishSpawning(Transform);

		AWoTPortcullis* Portcullis = World->SpawnActorDeferred<AWoTPortcullis>(
			AWoTPortcullis::StaticClass(), Transform);
		Portcullis->MoverTag = MoverTag;
		Portcullis->FinishSpawning(Transform);
		return Portcullis;
	}

	AWoTCharacter* CitadelSpecSpawnPawn(UWorld* World, uint8 InTeam)
	{
		AWoTPlayerController* Controller = CitadelSpecSpawnController(World, InTeam);
		AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
			FVector(100, 0, 300), FRotator::ZeroRotator,
			CitadelSpecAlwaysSpawnParams());
		Controller->Possess(Pawn);
		return Pawn;
	}

	void CitadelSpecOverlap(AWoTPortcullis* Portcullis, AActor* Other)
	{
		// Deliver the touch from under the arch (inside the UE1 cylinder).
		WoTTest::PlaceWithoutOverlapEvents(Other,
			Portcullis->GetActorLocation() + FVector(0, 0, 60));
		Portcullis->OnPortcullisOverlap(
			nullptr, Other, nullptr, 0, false, FHitResult());
	}

	bool CitadelSpecGateRaised(const AWoTPortcullis* Portcullis)
	{
		return IsValid(Portcullis) && IsValid(Portcullis->GetMover())
			&& Portcullis->GetMover()->GetActorLocation().Z
				> Portcullis->GetActorLocation().Z + 50.0;
	}

	AActor* CitadelSpecSpawnFloor(
		UWorld* World, FVector Center, FVector Extent)
	{
		AActor* Floor = World->SpawnActor<AActor>(
			Center, FRotator::ZeroRotator, CitadelSpecAlwaysSpawnParams());
		UBoxComponent* Box = NewObject<UBoxComponent>(Floor);
		Box->SetBoxExtent(Extent);
		Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Box->SetCollisionObjectType(ECC_WorldStatic);
		Box->SetCollisionResponseToAllChannels(ECR_Block);
		Floor->SetRootComponent(Box);
		Box->RegisterComponent();
		// A plain AActor has no root at spawn, so SpawnActor's location is dropped;
		// apply Center only now that the box IS the root (else every fixture stacks
		// at the origin and traces start inside the wrong box). Matches SpawnBox.
		Floor->SetActorLocation(Center);
		return Floor;
	}

	AActor* CitadelSpecSpawnBox(
		UWorld* World, const FVector& Center, const FVector& Extent,
		const FRotator& Rotation = FRotator::ZeroRotator)
	{
		AActor* BoxActor = World->SpawnActor<AActor>(
			Center, Rotation, CitadelSpecAlwaysSpawnParams());
		UBoxComponent* Box = NewObject<UBoxComponent>(BoxActor);
		Box->SetBoxExtent(Extent);
		Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Box->SetCollisionObjectType(ECC_WorldStatic);
		Box->SetCollisionResponseToAllChannels(ECR_Block);
		BoxActor->SetRootComponent(Box);
		Box->RegisterComponent();
		BoxActor->SetActorLocationAndRotation(Center, Rotation);
		return BoxActor;
	}

	AActor* CitadelSpecSpawnDynamicOccupant(
		UWorld* World, const FVector& Center, const FVector& Extent)
	{
		AActor* Occupant = World->SpawnActor<AActor>(
			Center, FRotator::ZeroRotator, CitadelSpecAlwaysSpawnParams());
		UBoxComponent* OccupantBox = NewObject<UBoxComponent>(Occupant);
		OccupantBox->SetBoxExtent(Extent);
		OccupantBox->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		OccupantBox->SetCollisionObjectType(ECC_WorldDynamic);
		OccupantBox->SetCollisionResponseToAllChannels(ECR_Block);
		Occupant->SetRootComponent(OccupantBox);
		OccupantBox->RegisterComponent();
		return Occupant;
	}

	void CitadelSpecSpawnWallFixture(UWorld* World, float CenterY)
	{
		// The left face is X=190. Placement aims at X=189.5: the downward
		// trace misses the wall and reaches the floor, while the -Normal and
		// both side-fit traces subsequently hit the wall within 0.5 UU in X.
		CitadelSpecSpawnBox(
			World, FVector(200, CenterY, 100), FVector(10, 100, 200));
	}

	void CitadelSpecSpawnSideFitFixture(
		UWorld* World, float CenterY, float NormalDot)
	{
		// A narrow center strip services only the hanging trace. Each side ray
		// instead meets an angled panel whose face normal has the requested dot
		// product with placement Normal=(-X).
		CitadelSpecSpawnBox(
			World, FVector(200, CenterY, 100), FVector(10, 4, 200));

		const float SinAngle = FMath::Sqrt(1.0f - NormalDot * NormalDot);
		const FVector TempLocation(221.5f, CenterY, 90.0f);
		const FVector TestPoint(157.5f, CenterY, 90.0f);
		for (int32 SideSign : {-1, 1})
		{
			const FVector SidePoint =
				TempLocation + FVector(0, SideSign * 56.0f, 0);
			const FVector PanelPoint = FMath::Lerp(TestPoint, SidePoint, 0.5f);
			const FVector FaceNormal(
				-NormalDot, -SideSign * SinAngle, 0.0f);
			const FVector PanelCenter = PanelPoint - FaceNormal;
			CitadelSpecSpawnBox(World, PanelCenter, FVector(1, 20, 200),
				FaceNormal.Rotation());
		}
	}

	template <typename ActorType>
	int32 CitadelSpecCountValidActors(UWorld* World)
	{
		int32 Count = 0;
		for (TActorIterator<ActorType> It(World); It; ++It)
		{
			if (IsValid(*It))
			{
				++Count;
			}
		}
		return Count;
	}

	AWoTSpear* CitadelSpecFindValidSpear(UWorld* World)
	{
		for (TActorIterator<AWoTSpear> It(World); It; ++It)
		{
			if (IsValid(*It))
			{
				return *It;
			}
		}
		return nullptr;
	}

	AWoTPitOpening* CitadelSpecSpawnPitOpening(
		UWorld* World, const FVector& Location)
	{
		return World->SpawnActor<AWoTPitOpening>(
			Location, FRotator::ZeroRotator, CitadelSpecAlwaysSpawnParams());
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelRegistrySpec,
	"WoT.Citadel.Registry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelRegistrySpec::RunTest(const FString&)
{
	const FWoTResourceDescriptor& Portcullis =
		WoTCitadelResources::FindByAdapterName(TEXT("PortcullisInventory"));
	TestTrue(TEXT("portcullis adapter resolves"), Portcullis.IsValidDescriptor());
	TestEqual(TEXT("portcullis resource class"), Portcullis.ResourceClass,
		static_cast<UClass*>(AWoTPortcullis::StaticClass()));
	TestFalse(TEXT("portcullis is not spawn-temp"), Portcullis.bSpawnTempResource);

	const FWoTResourceDescriptor& Staircase =
		WoTCitadelResources::FindByAdapterName(TEXT("StaircaseInventory"));
	TestEqual(TEXT("staircase resource class"), Staircase.ResourceClass,
		static_cast<UClass*>(AWoTStaircase::StaticClass()));
	TestFalse(TEXT("staircase is not spawn-temp"), Staircase.bSpawnTempResource);

	const FWoTResourceDescriptor& Wall =
		WoTCitadelResources::FindByAdapterName(TEXT("WallInventory"));
	TestTrue(TEXT("wall adapter resolves"), Wall.IsValidDescriptor());
	TestEqual(TEXT("wall resource class"), Wall.ResourceClass,
		static_cast<UClass*>(AWoTWall::StaticClass()));
	TestTrue(TEXT("wall is spawn-temp"), Wall.bSpawnTempResource);
	TestEqual(TEXT("reverse lookup wall"),
		WoTCitadelResources::FindAdapterForResourceClass(AWoTWall::StaticClass()),
		FName(TEXT("WallInventory")));

	const FWoTResourceDescriptor& Trolloc =
		WoTCitadelResources::FindByAdapterName(TEXT("TrollocInventory"));
	TestTrue(TEXT("creature adapter resolves"), Trolloc.IsValidDescriptor());
	TestEqual(TEXT("Trolloc adapter maps to its retail TrollocStrong row"),
		Trolloc.CreatureRow, FName(TEXT("TrollocStrong")));
	TestEqual(TEXT("reverse lookup portcullis"),
		WoTCitadelResources::FindAdapterForResourceClass(AWoTPortcullis::StaticClass()),
		FName(TEXT("PortcullisInventory")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelTrapEditLifecycleSpec,
	"WoT.Citadel.TrapEditLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelTrapEditLifecycleSpec::RunTest(const FString&)
{
	UWorld* World = CitadelSpecCreateWorld();
	AWoTPortcullis* Trap = World->SpawnActor<AWoTPortcullis>(
		CitadelSpecAlwaysSpawnParams());
	Trap->BeginEditingResource(1);
	TestEqual(TEXT("BeginEditingResource assigns team"), static_cast<int32>(Trap->Team), 1);
	TestTrue(TEXT("editing resource is inactive"), Trap->IsInactive(nullptr));
	Trap->EndEditingResource();
	TestFalse(TEXT("ended-editing resource is no longer editing-inactive"),
		Trap->IsInactive(nullptr));
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelEditCollisionClientSpec,
	"WoT.Citadel.EditCollisionOnClients",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelEditCollisionClientSpec::RunTest(const FString&)
{
	// The editor cursor (EditorTrace) traces ECC_Visibility on the LOCAL client, but
	// Begin/EndEditingResource runs on the authority only — UE1 got the client half for
	// free because bCollideActors replicated. The port replicates AWoTTrap::bEditing and
	// OnRep_Editing applies the collision half. The citadel suite is otherwise entirely
	// authority-local, so this spec exercises role EXPLICITLY: it forces a
	// simulated-proxy role and drives the OnRep directly — the machine-role the
	// deployed-resources-unselectable-on-clients gap lived on.
	UWorld* World = CitadelSpecCreateWorld();

	AWoTSpear* Spear = World->SpawnActor<AWoTSpear>(CitadelSpecAlwaysSpawnParams());
	UCapsuleComponent* SpearCapsule =
		IsValid(Spear) ? Cast<UCapsuleComponent>(Spear->GetRootComponent()) : nullptr;
	TestNotNull(TEXT("spear proxy capsule exists"), SpearCapsule);
	if (IsValid(Spear) && SpearCapsule)
	{
		Spear->SetRole(ROLE_SimulatedProxy);
		Spear->bEditing = true;
		Spear->OnRep_Editing();
		TestEqual(TEXT("proxy spear blocks the editor Visibility cursor while editing"),
			SpearCapsule->GetCollisionResponseToChannel(ECC_Visibility), ECR_Block);
		TestEqual(TEXT("proxy spear capsule is query-enabled while editing"),
			SpearCapsule->GetCollisionEnabled(), ECollisionEnabled::QueryOnly);
		Spear->bEditing = false;
		Spear->OnRep_Editing();
		TestEqual(TEXT("proxy spear drops collision at play (Spear.uc:100)"),
			SpearCapsule->GetCollisionEnabled(), ECollisionEnabled::NoCollision);
	}

	AWoTPortcullis* Gate = CitadelSpecSpawnPortcullis(World, FVector(500, 0, 0));
	UCapsuleComponent* GateCapsule =
		IsValid(Gate) ? Cast<UCapsuleComponent>(Gate->GetRootComponent()) : nullptr;
	UWoTFlatCylinderComponent* GateCursor =
		IsValid(Gate) ? Gate->GetCursorTraceCylinder() : nullptr;
	TestNotNull(TEXT("portcullis proxy capsule exists"), GateCapsule);
	TestNotNull(TEXT("portcullis proxy exact cursor cylinder exists"), GateCursor);
	if (IsValid(Gate) && GateCapsule && GateCursor)
	{
		// The toggle pair's traceability is palette-dependent (Portcullis.uc:50-77):
		// while editing, an INACTIVE gate stays untraceable until its class is the
		// palette selection; an ACTIVE one keeps its cursor collision even deselected
		// (EndSelectResource's sprite-only hide). The full owner-local flow lives in
		// WoT.Citadel.EditorSelect.ToggleTraceabilityMatrix — this proxy half pins the
		// OnRep-driven pieces.
		Gate->SetRole(ROLE_SimulatedProxy);
		Gate->bEditing = true;
		Gate->OnRep_Editing();
		TestEqual(TEXT("proxy inactive portcullis stays untraceable while editing (unselected)"),
			GateCursor->GetCollisionResponseToChannel(ECC_Visibility), ECR_Ignore);
		Gate->SetCitadelSelectTraceability(true);
		TestEqual(TEXT("palette-selected proxy portcullis blocks the editor cursor"),
			GateCursor->GetCollisionResponseToChannel(ECC_Visibility), ECR_Block);
		TestEqual(TEXT("portcullis broad phase remains overlap-only while selected"),
			GateCapsule->GetCollisionResponseToChannel(ECC_Visibility), ECR_Ignore);
		Gate->SetCitadelSelectTraceability(false);
		TestEqual(TEXT("deselected inactive proxy portcullis hides from the cursor again"),
			GateCursor->GetCollisionResponseToChannel(ECC_Visibility), ECR_Ignore);
		Gate->Activate();
		TestEqual(TEXT("ACTIVE proxy portcullis stays traceable while deselected"),
			GateCursor->GetCollisionResponseToChannel(ECC_Visibility), ECR_Block);
		Gate->bEditing = false;
		Gate->OnRep_Editing();
		TestEqual(TEXT("proxy portcullis ignores the Visibility cursor at play"),
			GateCursor->GetCollisionResponseToChannel(ECC_Visibility), ECR_Ignore);
	}
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelBudgetGateSpec,
	"WoT.Citadel.BudgetGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelBudgetGateSpec::RunTest(const FString&)
{
	UWorld* World = CitadelSpecCreateWorld();
	AWoTBudgetInfo* BudgetInfo = World->SpawnActor<AWoTBudgetInfo>(
		CitadelSpecAlwaysSpawnParams());
	BudgetInfo->Team = 1;
	AWoTPlayerController* First = CitadelSpecSpawnController(World, 1);
	AWoTPlayerController* Second = CitadelSpecSpawnController(World, 1);
	AWoTPlayerController* Enemy = CitadelSpecSpawnController(World, 2);

	TestFalse(TEXT("wrong-team BeginEdit rejected"), BudgetInfo->BeginEdit(Enemy));
	TestTrue(TEXT("first same-team BeginEdit granted"), BudgetInfo->BeginEdit(First));
	TestFalse(TEXT("second concurrent BeginEdit rejected (limit 1)"),
		BudgetInfo->BeginEdit(Second));
	BudgetInfo->EndEdit(First);
	TestTrue(TEXT("slot reusable after EndEdit"), BudgetInfo->BeginEdit(Second));
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelAssessSpec,
	"WoT.Citadel.AssessBattlefield",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelAssessSpec::RunTest(const FString&)
{
	UWorld* World = CitadelSpecCreateWorld(true);
	AWoTBattleGameMode* BattleMode =
		World->GetAuthGameMode<AWoTBattleGameMode>();
	AWoTBudgetInfo* BudgetInfo = World->SpawnActor<AWoTBudgetInfo>(
		CitadelSpecAlwaysSpawnParams());
	BudgetInfo->Team = 1;
	CitadelSpecSetSealBudget(BudgetInfo, 1);

	BattleMode->SetEditing(1, true);
	BattleMode->AssessBattlefield();
	TestFalse(TEXT("editing team blocks play"), BattleMode->CanTeleport());

	BattleMode->SetEditing(1, false);
	BattleMode->AssessBattlefield();
	TestFalse(TEXT("remaining seal budget blocks play"), BattleMode->CanTeleport());

	CitadelSpecSetSealBudget(BudgetInfo, 0);
	BattleMode->AssessBattlefield();
	TestTrue(TEXT("seals spent + no editors enables play"), BattleMode->CanTeleport());

	CitadelSpecSpawnAltar(World, 1, FVector::ZeroVector);
	CitadelSpecSpawnAltar(World, 2, FVector(5000, 0, 0));
	TestEqual(TEXT("nearest altar team"),
		static_cast<int32>(BattleMode->NearestAltarTeam(FVector(100, 0, 0))), 1);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelEmptyServerRestartSpec,
	"WoT.Citadel.EmptyServerRestart",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelEmptyServerRestartSpec::RunTest(const FString&)
{
	// giMPBattle.Timer (giMPBattle.uc:235-250): independent of CanTeleport, a disrupted
	// battlefield (some team readied up — traps placed, etc.) on an EMPTY server restarts
	// the game. The restart is observed through its ResetBattle side effect (the edit
	// phase re-enters: teleporters disabled again) plus the staged rotation URL — the
	// actual ServerTravel is suppressed under automation.
	UWorld* World = CitadelSpecCreateWorld(true);
	AWoTBattleGameMode* BattleMode = World->GetAuthGameMode<AWoTBattleGameMode>();

	// Battlefield clear + empty server: no restart.
	BattleMode->EnableTeleporters();
	BattleMode->Timer();
	TestTrue(TEXT("clear battlefield does not restart an empty server"),
		BattleMode->CanTeleport());

	// Disrupted battlefield but a player present: no restart.
	BattleMode->EnableTeamReady(1);
	AWoTPlayerController* Lingerer = CitadelSpecSpawnController(World, 1);
	BattleMode->Timer();
	TestTrue(TEXT("a present player holds off the empty-server restart"),
		BattleMode->CanTeleport());

	// Disrupted battlefield + zero players: RestartGame.
	BattleMode->SetInstalledMapsForTesting({ TEXT("Battle2_02") });
	if (IsValid(Lingerer))
	{
		Lingerer->Destroy();
	}
	WoTTest::TickWorld(World, 1);
	BattleMode->Timer();
	TestFalse(TEXT("empty server with a disrupted battlefield restarts (edit phase re-enters)"),
		BattleMode->CanTeleport());
	TestEqual(TEXT("restart staged the rotation's next map"),
		BattleMode->Map, FString(TEXT("Battle2_02")));
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelPreBeginPlayLoginSpec,
	"WoT.Citadel.PreBeginPlayLoginLocatesBattle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelPreBeginPlayLoginSpec::RunTest(const FString&)
{
	// Direct -game LoadMap logs the local player in BEFORE world BeginPlay (PIE begins play
	// first, which is why Battle-PIE validation never saw this): giMPBattle's PostBeginPlay
	// locate has not run when the login chain reaches ChangeTeam, so the mode must locate the
	// level-placed BattleInfo on demand instead of tripping ensure(Battle).
	WoTTest::FSpecWorldOptions Options;
	Options.GameModeClass = AWoTBattleGameMode::StaticClass();
	Options.bBeginPlay = false;
	UWorld* World = WoTTest::CreateSpecWorld(Options);
	// Deliberately NO World->BeginPlay() yet — this is the direct-boot login window.

	AWoTBattleInfo* Battle = World->SpawnActorDeferred<AWoTBattleInfo>(
		AWoTBattleInfo::StaticClass(), FTransform::Identity);
	Battle->NumTeams = 2;
	Battle->TeamClass[0] = AWoTTestTeamAlpha::StaticClass();
	Battle->TeamClass[1] = AWoTTestTeamBravo::StaticClass();
	Battle->FinishSpawning(FTransform::Identity);

	AWoTBattleGameMode* BattleMode = World->GetAuthGameMode<AWoTBattleGameMode>();
	AWoTPlayerController* PC = CitadelSpecSpawnController(World, 255);
	TestTrue(TEXT("pre-BeginPlay ChangeTeam succeeds"), BattleMode->ChangeTeam(PC, 0));
	TestTrue(TEXT("the placed BattleInfo was located on demand"),
		BattleMode->Battle == Battle);
	TestEqual(TEXT("MaxTeamSize was pushed onto the located BattleInfo"),
		Battle->MaxTeamSize, BattleMode->MaxTeamSize);
	const AWoTPlayerState* PS = PC->GetPlayerState<AWoTPlayerState>();
	TestEqual(TEXT("the login-window join landed on team 0"),
		PS ? static_cast<int32>(PS->Team) : -1, 0);
	TestEqual(TEXT("team 0 headcount counts the joiner"), Battle->NumPlayers[0], 1);

	// World BeginPlay afterwards keeps the same instance (the locate is idempotent).
	World->BeginPlay();
	TestTrue(TEXT("BeginPlay keeps the on-demand located BattleInfo"),
		BattleMode->Battle == Battle);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelSessionSpec,
	"WoT.Citadel.EditSession",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelSessionSpec::RunTest(const FString&)
{
	UWorld* World = CitadelSpecCreateWorld(true);
	AWoTBattleGameMode* BattleMode =
		World->GetAuthGameMode<AWoTBattleGameMode>();
	AWoTBudgetInfo* BudgetInfo = World->SpawnActor<AWoTBudgetInfo>(
		CitadelSpecAlwaysSpawnParams());
	BudgetInfo->Team = 1;
	CitadelSpecSetSealBudget(BudgetInfo, 1);
	AWoTPlayerController* First = CitadelSpecSpawnController(World, 1);
	CitadelSpecSpawnEditZone(World, FVector::ZeroVector, FVector(100), 1);

	First->CitadelBeginEdit();
	TestTrue(TEXT("begin-edit opens a session"), First->IsEditingCitadel());
	TestTrue(TEXT("battle shadow set editing"), BattleMode->IsEditing(1));
	TestFalse(TEXT("no team change while editing"), First->CanChangeTeamNow());
	BattleMode->ChangeTeam(First, 0);
	TestEqual(TEXT("game-mode team change entry is locked"),
		static_cast<int32>(First->GetPlayerState<AWoTPlayerState>()->Team), 1);
	First->ServerChangeTeam(0);
	TestEqual(TEXT("controller team-change RPC is locked"),
		static_cast<int32>(First->GetPlayerState<AWoTPlayerState>()->Team), 1);

	AWoTPlayerController* Second = CitadelSpecSpawnController(World, 1);
	Second->CitadelBeginEdit();
	TestFalse(TEXT("second editor rejected (limit 1)"), Second->IsEditingCitadel());

	AWoTPlayerController* OutOfRange = CitadelSpecSpawnController(
		World, 3, FVector(1000, 0, 0));
	CitadelSpecSpawnEditZone(World, FVector(1000, 0, 0), FVector(100), 3);
	OutOfRange->CitadelBeginEdit();
	TestFalse(TEXT("out-of-range team rejected"), OutOfRange->IsEditingCitadel());
	AWoTPlayerController* Neutral = CitadelSpecSpawnController(
		World, 255, FVector(2000, 0, 0));
	CitadelSpecSpawnEditZone(World, FVector(2000, 0, 0), FVector(100), 255);
	Neutral->CitadelBeginEdit();
	TestFalse(TEXT("neutral team rejected"), Neutral->IsEditingCitadel());

	// Real disconnect path: Logout runs the shared teardown before Super::Logout
	// (calling Destroyed() directly would half-run teardown on a still-registered actor).
	BattleMode->Logout(First);
	TestFalse(TEXT("disconnect clears the battle editing shadow"),
		BattleMode->IsEditing(1));
	Second->CitadelBeginEdit();
	TestTrue(TEXT("disconnect releases the budget edit slot"), Second->IsEditingCitadel());
	Second->CitadelEndEdit();

	BattleMode->EnableTeleporters();
	AWoTPlayerController* Third = CitadelSpecSpawnController(World, 1);
	Third->CitadelBeginEdit();
	TestFalse(TEXT("no begin-edit after play"), Third->IsEditingCitadel());
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelToggleSpec,
	"WoT.Citadel.Toggle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelToggleSpec::RunTest(const FString&)
{
	UWorld* World = CitadelSpecCreateWorld(true);
	CitadelSpecSpawnAltar(World, 1, FVector::ZeroVector);
	CitadelSpecSpawnAltar(World, 2, FVector(5000, 0, 0));
	AWoTBudgetInfo* BudgetInfo = World->SpawnActor<AWoTBudgetInfo>(
		CitadelSpecAlwaysSpawnParams());
	BudgetInfo->Team = 1;
	CitadelSpecSetResourceBudget(BudgetInfo, TEXT("PortcullisInventory"), 2);
	CitadelSpecSetResourceBudget(BudgetInfo, TEXT("StaircaseInventory"), 1);
	AWoTPortcullis* Gate =
		CitadelSpecSpawnPortcullis(World, FVector(100, 0, 0));
	AWoTPlayerController* Controller = CitadelSpecSpawnController(World, 1);
	// Small viewer zone at the origin that EXCLUDES the gate at x=100, so begin-edit
	// arming does not pre-claim the neutral gate — ServerToggleResource must be what
	// changes Gate->Team from 255 to 1 (keeps the ownership assertion meaningful).
	CitadelSpecSpawnEditZone(World, FVector::ZeroVector, FVector(50), 1);
	TestEqual(TEXT("neutral gate starts unowned"), static_cast<int32>(Gate->Team), 255);
	Controller->CitadelBeginEdit();
	TestEqual(TEXT("out-of-zone gate is not armed on entry"),
		static_cast<int32>(Gate->Team), 255);

	Controller->ServerToggleResource(Gate);
	TestTrue(TEXT("toggle activates the gate"), Gate->IsActive());
	TestEqual(TEXT("toggle spends 1"), BudgetInfo->GetItemCount(
		BudgetInfo->GetIndex(TEXT("PortcullisInventory"))), 1);
	TestEqual(TEXT("gate now owned by team 1"), static_cast<int32>(Gate->Team), 1);

	Controller->ServerToggleResource(Gate);
	TestFalse(TEXT("toggle deactivates the gate"), Gate->IsActive());
	TestEqual(TEXT("toggle-off refunds"), BudgetInfo->GetItemCount(
		BudgetInfo->GetIndex(TEXT("PortcullisInventory"))), 2);
	AWoTPlayerController* Teammate = CitadelSpecSpawnController(World, 1);
	Teammate->ServerToggleResource(Gate);
	TestFalse(TEXT("same-team controller without a session cannot toggle"),
		Gate->IsActive());

	AWoTStaircase* Staircase = World->SpawnActor<AWoTStaircase>(
		FVector(120, 0, 0), FRotator::ZeroRotator,
		CitadelSpecAlwaysSpawnParams());
	Controller->ServerToggleResource(Staircase);
	TestTrue(TEXT("toggle activates staircase"), Staircase->IsActive());
	TestEqual(TEXT("staircase toggle spends 1"), BudgetInfo->GetItemCount(
		BudgetInfo->GetIndex(TEXT("StaircaseInventory"))), 0);

	AWoTPortcullis* ForeignOwned =
		CitadelSpecSpawnPortcullis(World, FVector(150, 0, 0));
	ForeignOwned->Team = 2;
	Controller->ServerToggleResource(ForeignOwned);
	TestFalse(TEXT("foreign-owned gate cannot be activated"), ForeignOwned->IsActive());

	AWoTPortcullis* ForeignCitadel =
		CitadelSpecSpawnPortcullis(World, FVector(4900, 0, 0));
	Controller->ServerToggleResource(ForeignCitadel);
	TestFalse(TEXT("foreign-citadel gate cannot be activated"), ForeignCitadel->IsActive());

	AWoTPlayerController* Enemy = CitadelSpecSpawnController(
		World, 2, FVector(1000, 0, 0));
	CitadelSpecSpawnEditZone(World, FVector(1000, 0, 0), FVector(100), 2);
	Enemy->CitadelBeginEdit();
	Enemy->ServerToggleResource(Gate);
	TestFalse(TEXT("enemy without a session cannot toggle"), Gate->IsActive());
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelPortcullisLiveSpec,
	"WoT.Citadel.PortcullisLive",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelPortcullisLiveSpec::RunTest(const FString&)
{
	UWorld* World = CitadelSpecCreateWorld(true);
	AWoTBattleGameMode* BattleMode =
		World->GetAuthGameMode<AWoTBattleGameMode>();
	CitadelSpecSpawnAltar(World, 1, FVector::ZeroVector);
	AWoTBudgetInfo* BudgetInfo = World->SpawnActor<AWoTBudgetInfo>(
		CitadelSpecAlwaysSpawnParams());
	BudgetInfo->Team = 1;
	CitadelSpecSetResourceBudget(BudgetInfo, TEXT("PortcullisInventory"), 1);
	CitadelSpecSetSealBudget(BudgetInfo, 0);
	AWoTPortcullis* Arch = CitadelSpecSpawnPortcullisWithMover(
		World, FVector(100, 0, 0), TEXT("PMoverLive"));
	AWoTPlayerController* Controller = CitadelSpecSpawnController(World, 1);
	AWoTCharacter* Enemy = CitadelSpecSpawnPawn(World, 2);
	CitadelSpecSpawnEditZone(World, FVector::ZeroVector, FVector(500), 1);

	Controller->CitadelBeginEdit();
	Controller->ServerToggleResource(Arch);
	CitadelSpecOverlap(Arch, Enemy);
	WoTTest::TickWorld(World, 5);
	TestTrue(TEXT("gate stays raised during editing"), CitadelSpecGateRaised(Arch));

	Controller->CitadelEndEdit();
	// Retail: CitadelEditor.EndState readies the team but never assesses — play starts
	// only via the fire path (WOTPlayer.UseAngreal -> ServerAssessBattlefield). Drive the
	// assessment directly; this spec's intent is the readiness predicate, not the trigger.
	BattleMode->AssessBattlefield();
	TestTrue(TEXT("play began"), BattleMode->CanTeleport());
	CitadelSpecOverlap(Arch, Enemy);
	WoTTest::TickWorld(World, 30);
	TestFalse(TEXT("gate drops on enemy after play"), CitadelSpecGateRaised(Arch));
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelCalcLocationSpec,
	"WoT.Citadel.CreateCalcLocation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelCalcLocationSpec::RunTest(const FString&)
{
	UWorld* World = CitadelSpecCreateWorld(true);
	CitadelSpecSpawnFloor(World, FVector(0, 0, 0), FVector(500, 500, 10));
	AWoTSpear* Trap = World->SpawnActor<AWoTSpear>(
		FVector(0, 0, 200), FRotator::ZeroRotator, CitadelSpecAlwaysSpawnParams());
	FVector Loc(0, 0, 200);
	FVector Normal(0, 0, 1);
	TestTrue(TEXT("valid floor point resolves"), Trap->CalcLocation(Loc, Normal));
	TestTrue(TEXT("Loc snapped onto the floor top"),
		FMath::IsNearlyEqual(Loc.Z, 10.0f, 1.0f));

	AWoTSpear* Trap2 = World->SpawnActor<AWoTSpear>(
		FVector(9000, 9000, 200), FRotator::ZeroRotator,
		CitadelSpecAlwaysSpawnParams());
	FVector AirLoc(9000, 9000, 200);
	FVector AirNormal(0, 0, 1);
	TestFalse(TEXT("no surface -> reject"), Trap2->CalcLocation(AirLoc, AirNormal));
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelSpearDeploySpec,
	"WoT.Citadel.CreateSpear",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelSpearDeploySpec::RunTest(const FString&)
{
	UWorld* World = CitadelSpecCreateWorld(true);
	CitadelSpecSpawnFloor(World, FVector(0, 0, 0), FVector(500, 500, 10));
	AWoTSpear* Spear = World->SpawnActor<AWoTSpear>(
		FVector(0, 0, 200), FRotator::ZeroRotator, CitadelSpecAlwaysSpawnParams());
	FVector Loc(0, 0, 200);
	FVector Normal(0, 0, 1);
	TestTrue(TEXT("spear deploys on a clear floor"), Spear->DeployResource(Loc, Normal));

	AWoTSpear* Near = World->SpawnActor<AWoTSpear>(
		Spear->GetActorLocation() + FVector(10, 0, 0), FRotator::ZeroRotator,
		CitadelSpecAlwaysSpawnParams());
	FVector NearLoc = Spear->GetActorLocation() + FVector(10, 0, 50);
	FVector NearNormal(0, 0, 1);
	TestFalse(TEXT("a spear within SeparationDistance is rejected"),
		Near->DeployResource(NearLoc, NearNormal));

	// Regression (parity wave 2026-07-13): a deployed spear's TrapBlockers must never
	// read as occupants — TrapBlocker extends Keypoint (bHidden=True; the Trap.uc:91
	// occupancy filter requires !bHidden), so even ARMED blockers (post-edit, collision
	// back on) are invisible to AnyActorsInArea. Pre-fix they projected a phantom
	// no-place zone along the spear's reach. 50 UU clears the 32+10 manager separation
	// but sits well inside the armed blockers' 32-radius capsules. The rejected Near
	// actor above is a REAL occupant 40 UU out — remove it so only blockers remain.
	Near->Destroy();
	WoTTest::TickWorld(World, 1);
	Spear->BeginEditingResource(1);   // rebuilds blockers at the deployed transform
	Spear->EndEditingResource();      // re-arms them (Spear.uc:101) — play-time state
	AWoTSpear* Clear = World->SpawnActor<AWoTSpear>(
		Spear->GetActorLocation() + FVector(50, 0, 0), FRotator::ZeroRotator,
		CitadelSpecAlwaysSpawnParams());
	FVector ClearLoc = Spear->GetActorLocation() + FVector(50, 0, 50);
	FVector ClearNormal(0, 0, 1);
	TestTrue(TEXT("armed blockers of a deployed spear do not block a clear placement"),
		Clear->DeployResource(ClearLoc, ClearNormal));

	TestTrue(TEXT("unlocked spear is removable"), Spear->RemoveResource());
	Spear->bLocked = true;
	TestFalse(TEXT("locked spear is not removable"), Spear->RemoveResource());
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelAlarmDeploySpec,
	"WoT.Citadel.CreateAlarm",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelAlarmDeploySpec::RunTest(const FString&)
{
	UWorld* World = CitadelSpecCreateWorld(true);
	CitadelSpecSpawnFloor(World, FVector(200, 0, 0), FVector(500, 500, 10));
	CitadelSpecSpawnWallFixture(World, 0.0f);
	AWoTAlarm* Alarm = World->SpawnActor<AWoTAlarm>(
		FVector::ZeroVector, FRotator::ZeroRotator, CitadelSpecAlwaysSpawnParams());
	FVector WallLoc(189.5f, 0, 200);
	FVector WallNormal(-1, 0, 0);
	TestTrue(TEXT("alarm accepts a vertical wall"),
		Alarm->CalcLocation(WallLoc, WallNormal));
	FVector FloorNormal(0, 0, 1);
	FVector FloorLoc(0, 0, 50);
	TestFalse(TEXT("alarm rejects a floor normal (Z != 0)"),
		Alarm->CalcLocation(FloorLoc, FloorNormal));
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelCalcLocationToleranceSpec,
	"WoT.Citadel.CreateCalcLocationTolerances",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelCalcLocationToleranceSpec::RunTest(const FString&)
{
	UWorld* World = CitadelSpecCreateWorld(true);
	CitadelSpecSpawnFloor(World, FVector(200, 450, 0), FVector(500, 1000, 10));
	CitadelSpecSpawnWallFixture(World, 0.0f);
	CitadelSpecSpawnWallFixture(World, 300.0f);
	CitadelSpecSpawnSideFitFixture(World, 600.0f, 0.995f);
	CitadelSpecSpawnSideFitFixture(World, 900.0f, 0.985f);

	AWoTAlarm* Alarm = World->SpawnActor<AWoTAlarm>(
		FVector::ZeroVector, FRotator::ZeroRotator, CitadelSpecAlwaysSpawnParams());
	FVector Normal(-1, 0, 0);
	FVector HangingInside(189.01f, 0, 200);
	TestTrue(TEXT("hanging XY delta just inside 1.0 is accepted"),
		Alarm->CalcLocation(HangingInside, Normal));
	FVector HangingOutside(188.99f, 300, 200);
	TestFalse(TEXT("hanging XY delta just outside 1.0 is rejected"),
		Alarm->CalcLocation(HangingOutside, Normal));

	FVector SideInside(189.5f, 600, 200);
	TestTrue(TEXT("side-fit normal dot just inside 0.99 is accepted"),
		Alarm->CalcLocation(SideInside, Normal));
	FVector SideOutside(189.5f, 900, 200);
	TestFalse(TEXT("side-fit normal dot just outside 0.99 is rejected"),
		Alarm->CalcLocation(SideOutside, Normal));
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelAlarmNormalToleranceSpec,
	"WoT.Citadel.CreateAlarmNormalTolerance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelAlarmNormalToleranceSpec::RunTest(const FString&)
{
	UWorld* World = CitadelSpecCreateWorld(true);
	CitadelSpecSpawnFloor(World, FVector(200, 0, 0), FVector(500, 500, 10));
	CitadelSpecSpawnWallFixture(World, 0.0f);
	AWoTAlarm* Alarm = World->SpawnActor<AWoTAlarm>(
		FVector::ZeroVector, FRotator::ZeroRotator, CitadelSpecAlwaysSpawnParams());

	const float InsideZ = KINDA_SMALL_NUMBER * 0.5f;
	const float OutsideZ = KINDA_SMALL_NUMBER * 2.0f;
	FVector InsideNormal(-FMath::Sqrt(1.0f - InsideZ * InsideZ), 0, InsideZ);
	FVector InsideLoc(189.5f, 0, 200);
	TestTrue(TEXT("alarm accepts Normal.Z just inside KINDA_SMALL_NUMBER"),
		Alarm->CalcLocation(InsideLoc, InsideNormal));

	FVector OutsideNormal(-FMath::Sqrt(1.0f - OutsideZ * OutsideZ), 0, OutsideZ);
	FVector OutsideLoc(189.5f, 0, 200);
	TestFalse(TEXT("alarm rejects Normal.Z just outside KINDA_SMALL_NUMBER"),
		Alarm->CalcLocation(OutsideLoc, OutsideNormal));
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelCreateRpcSpec,
	"WoT.Citadel.CreateRpc",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelCreateRpcSpec::RunTest(const FString&)
{
	UWorld* World = CitadelSpecCreateWorld(true);
	AWoTBattleGameMode* BattleMode = World->GetAuthGameMode<AWoTBattleGameMode>();
	CitadelSpecSpawnAltar(World, 1, FVector(0, 0, 0));
	CitadelSpecSpawnAltar(World, 2, FVector(5000, 0, 0));
	CitadelSpecSpawnFloor(World, FVector(0, 0, 0), FVector(500, 500, 10));
	AWoTBudgetInfo* BudgetInfo = World->SpawnActor<AWoTBudgetInfo>(
		CitadelSpecAlwaysSpawnParams());
	BudgetInfo->Team = 1;
	CitadelSpecSetResourceBudget(BudgetInfo, TEXT("SpearInventory"), 1);
	AWoTPlayerController* Controller = CitadelSpecSpawnController(World, 1);
	CitadelSpecSpawnEditZone(World, FVector::ZeroVector, FVector(1000), 1);
	Controller->CitadelBeginEdit();

	const FWoTResourceDescriptor& Descriptor =
		WoTCitadelResources::FindByAdapterName(TEXT("SpearInventory"));
	TestTrue(TEXT("spear adapter resolves + spawn-temp"),
		Descriptor.IsValidDescriptor() && Descriptor.bSpawnTempResource);

	Controller->ServerCreateResource(
		TEXT("SpearInventory"), FVector(0, 0, 200), FVector(0, 0, 1));
	TestEqual(TEXT("create spends 1"), BudgetInfo->GetItemCount(
		BudgetInfo->GetIndex(TEXT("SpearInventory"))), 0);
	int32 SpearCount = 0;
	for (TActorIterator<AWoTSpear> It(World); It; ++It)
	{
		++SpearCount;
	}
	TestEqual(TEXT("one spear created"), SpearCount, 1);

	Controller->ServerCreateResource(
		TEXT("SpearInventory"), FVector(50, 0, 200), FVector(0, 0, 1));
	TestEqual(TEXT("no overspend"), BudgetInfo->GetItemCount(
		BudgetInfo->GetIndex(TEXT("SpearInventory"))), 0);

	AWoTSpear* Placed = nullptr;
	for (TActorIterator<AWoTSpear> It(World); It; ++It)
	{
		Placed = *It;
	}
	Controller->ServerRemoveResource(Placed);
	TestEqual(TEXT("remove refunds 1"), BudgetInfo->GetItemCount(
		BudgetInfo->GetIndex(TEXT("SpearInventory"))), 1);

	BattleMode->EnableTeleporters();
	Controller->ServerCreateResource(
		TEXT("SpearInventory"), FVector(0, 0, 200), FVector(0, 0, 1));
	TestEqual(TEXT("no create after play"), BudgetInfo->GetItemCount(
		BudgetInfo->GetIndex(TEXT("SpearInventory"))), 1);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelCreatedSpearBlockersSpec,
	"WoT.Citadel.CreatedSpearBlockers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelCreatedSpearBlockersSpec::RunTest(const FString&)
{
	UWorld* World = CitadelSpecCreateWorld(true);
	CitadelSpecSpawnAltar(World, 1, FVector::ZeroVector);
	CitadelSpecSpawnAltar(World, 2, FVector(5000, 0, 0));
	CitadelSpecSpawnFloor(World, FVector::ZeroVector, FVector(500, 500, 10));
	AWoTBudgetInfo* BudgetInfo = World->SpawnActor<AWoTBudgetInfo>(
		CitadelSpecAlwaysSpawnParams());
	BudgetInfo->Team = 1;
	CitadelSpecSetResourceBudget(BudgetInfo, TEXT("SpearInventory"), 1);
	AWoTPlayerController* Controller = CitadelSpecSpawnController(World, 1);
	CitadelSpecSpawnEditZone(World, FVector::ZeroVector, FVector(1000), 1);
	Controller->CitadelBeginEdit();
	Controller->ServerCreateResource(
		TEXT("SpearInventory"), FVector(0, 0, 200), FVector(0, 0, 1));

	AWoTSpear* Spear = CitadelSpecFindValidSpear(World);
	TestNotNull(TEXT("created spear exists"), Spear);
	if (Spear)
	{
		const float Increment = Spear->ImpaleRange / 5.0f;
		const FVector Forward = Spear->GetActorForwardVector();
		uint8 FoundIndices = 0;
		int32 OwnedBlockerCount = 0;
		for (TActorIterator<AWoTTrapBlocker> It(World); It; ++It)
		{
			AWoTTrapBlocker* Blocker = *It;
			if (!IsValid(Blocker) || Blocker->GetOwner() != Spear)
			{
				continue;
			}
			++OwnedBlockerCount;
			const float Along =
				(Blocker->GetActorLocation() - Spear->GetActorLocation()) | Forward;
			const int32 Index = FMath::RoundToInt(Along / Increment) - 1;
			const bool bValidIndex = Index >= 0 && Index < 4;
			TestTrue(TEXT("blocker occupies one deployed-transform slot"), bValidIndex);
			if (bValidIndex)
			{
				FoundIndices |= static_cast<uint8>(1 << Index);
				const FTransform Expected(
					FRotator::ZeroRotator,
					Spear->GetActorLocation() + Increment * (Index + 1) * Forward);
				TestTrue(TEXT("blocker final transform follows deployed spear"),
					Blocker->GetActorTransform().Equals(Expected, 0.1f));
			}
		}
		TestEqual(TEXT("created spear owns four blockers"), OwnedBlockerCount, 4);
		TestEqual(TEXT("all four deployed-transform slots are occupied"),
			static_cast<int32>(FoundIndices), 0x0f);
	}
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelCreateDeployFailureSpec,
	"WoT.Citadel.CreateDeployFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelCreateDeployFailureSpec::RunTest(const FString&)
{
	UWorld* World = CitadelSpecCreateWorld(true);
	CitadelSpecSpawnAltar(World, 1, FVector::ZeroVector);
	CitadelSpecSpawnAltar(World, 2, FVector(5000, 0, 0));
	AWoTBudgetInfo* BudgetInfo = World->SpawnActor<AWoTBudgetInfo>(
		CitadelSpecAlwaysSpawnParams());
	BudgetInfo->Team = 1;
	CitadelSpecSetResourceBudget(BudgetInfo, TEXT("SpearInventory"), 1);
	AWoTPlayerController* Controller = CitadelSpecSpawnController(World, 1);
	CitadelSpecSpawnEditZone(World, FVector::ZeroVector, FVector(1000), 1);
	Controller->CitadelBeginEdit();

	Controller->ServerCreateResource(
		TEXT("SpearInventory"), FVector(100, 0, 200), FVector(0, 0, 1));
	TestEqual(TEXT("deploy-false create does not spend"), BudgetInfo->GetItemCount(
		BudgetInfo->GetIndex(TEXT("SpearInventory"))), 1);
	TestEqual(TEXT("deploy-false create leaves no spear"),
		CitadelSpecCountValidActors<AWoTSpear>(World), 0);
	TestEqual(TEXT("deploy-false create leaves no blocker"),
		CitadelSpecCountValidActors<AWoTTrapBlocker>(World), 0);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelCreateFinalLocationSpec,
	"WoT.Citadel.CreateFinalLocationRecheck",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelCreateFinalLocationSpec::RunTest(const FString&)
{
	UWorld* World = CitadelSpecCreateWorld(true);
	AWoTBattleGameMode* BattleMode = World->GetAuthGameMode<AWoTBattleGameMode>();
	CitadelSpecSpawnAltar(World, 1, FVector(0, 0, 200));
	CitadelSpecSpawnAltar(World, 2, FVector::ZeroVector);
	CitadelSpecSpawnFloor(World, FVector::ZeroVector, FVector(500, 500, 10));
	AWoTBudgetInfo* BudgetInfo = World->SpawnActor<AWoTBudgetInfo>(
		CitadelSpecAlwaysSpawnParams());
	BudgetInfo->Team = 1;
	CitadelSpecSetResourceBudget(BudgetInfo, TEXT("SpearInventory"), 1);
	AWoTPlayerController* Controller = CitadelSpecSpawnController(World, 1);
	CitadelSpecSpawnEditZone(World, FVector::ZeroVector, FVector(1000), 1);
	Controller->CitadelBeginEdit();

	const FVector ClientLoc(0, 0, 160);
	TestEqual(TEXT("client location begins in session citadel"),
		static_cast<int32>(BattleMode->NearestAltarTeam(ClientLoc)), 1);
	TestEqual(TEXT("deployed floor location belongs to other citadel"),
		static_cast<int32>(BattleMode->NearestAltarTeam(FVector(0, 0, 10))), 2);
	Controller->ServerCreateResource(
		TEXT("SpearInventory"), ClientLoc, FVector(0, 0, 1));
	TestEqual(TEXT("final-location rejection does not spend"), BudgetInfo->GetItemCount(
		BudgetInfo->GetIndex(TEXT("SpearInventory"))), 1);
	TestEqual(TEXT("final-location rejection leaves no spear"),
		CitadelSpecCountValidActors<AWoTSpear>(World), 0);
	TestEqual(TEXT("final-location rejection leaves no blocker"),
		CitadelSpecCountValidActors<AWoTTrapBlocker>(World), 0);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelRemoveGuardsSpec,
	"WoT.Citadel.RemoveGuards",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelRemoveGuardsSpec::RunTest(const FString&)
{
	UWorld* World = CitadelSpecCreateWorld(true);
	CitadelSpecSpawnAltar(World, 1, FVector::ZeroVector);
	CitadelSpecSpawnAltar(World, 2, FVector(5000, 0, 0));
	AWoTBudgetInfo* BudgetInfo = World->SpawnActor<AWoTBudgetInfo>(
		CitadelSpecAlwaysSpawnParams());
	BudgetInfo->Team = 1;
	CitadelSpecSetResourceBudget(BudgetInfo, TEXT("SpearInventory"), 0);
	CitadelSpecSetResourceBudget(BudgetInfo, TEXT("PortcullisInventory"), 1);
	AWoTPlayerController* Controller = CitadelSpecSpawnController(World, 1);
	CitadelSpecSpawnEditZone(World, FVector::ZeroVector, FVector(1000), 1);
	Controller->CitadelBeginEdit();

	AWoTSpear* Locked = World->SpawnActor<AWoTSpear>(
		FVector(100, 0, 10), FRotator::ZeroRotator, CitadelSpecAlwaysSpawnParams());
	Locked->Team = 1;
	Locked->bLocked = true;
	Controller->ServerRemoveResource(Locked);
	TestTrue(TEXT("locked spear is not destroyed"), IsValid(Locked));
	TestEqual(TEXT("locked spear is not refunded"), BudgetInfo->GetItemCount(
		BudgetInfo->GetIndex(TEXT("SpearInventory"))), 0);

	AWoTSpear* Foreign = World->SpawnActor<AWoTSpear>(
		FVector(150, 0, 10), FRotator::ZeroRotator, CitadelSpecAlwaysSpawnParams());
	Foreign->Team = 2;
	Controller->ServerRemoveResource(Foreign);
	TestTrue(TEXT("foreign-team spear is not destroyed"), IsValid(Foreign));
	TestEqual(TEXT("foreign-team spear is not refunded"), BudgetInfo->GetItemCount(
		BudgetInfo->GetIndex(TEXT("SpearInventory"))), 0);

	AWoTPortcullis* Toggle =
		CitadelSpecSpawnPortcullis(World, FVector(200, 0, 10));
	Toggle->Team = 1;
	Controller->ServerRemoveResource(Toggle);
	TestTrue(TEXT("pre-placed toggle resource is not destroyed"), IsValid(Toggle));
	TestEqual(TEXT("pre-placed toggle resource is not refunded"),
		BudgetInfo->GetItemCount(
			BudgetInfo->GetIndex(TEXT("PortcullisInventory"))), 1);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelCreatedSpearLiveSpec,
	"WoT.Citadel.CreatedSpearLive",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelCreatedSpearLiveSpec::RunTest(const FString&)
{
	UWorld* World = CitadelSpecCreateWorld(true);
	AWoTBattleGameMode* BattleMode = World->GetAuthGameMode<AWoTBattleGameMode>();
	CitadelSpecSpawnAltar(World, 1, FVector::ZeroVector);
	CitadelSpecSpawnAltar(World, 2, FVector(5000, 0, 0));
	CitadelSpecSpawnFloor(World, FVector::ZeroVector, FVector(500, 500, 10));
	AWoTBudgetInfo* BudgetInfo = World->SpawnActor<AWoTBudgetInfo>(
		CitadelSpecAlwaysSpawnParams());
	BudgetInfo->Team = 1;
	CitadelSpecSetResourceBudget(BudgetInfo, TEXT("SpearInventory"), 1);
	CitadelSpecSetSealBudget(BudgetInfo, 0);
	AWoTPlayerController* Controller = CitadelSpecSpawnController(World, 1);
	CitadelSpecSpawnEditZone(World, FVector::ZeroVector, FVector(1000), 1);
	Controller->CitadelBeginEdit();
	Controller->ServerCreateResource(
		TEXT("SpearInventory"), FVector(0, 0, 200), FVector(0, 0, 1));

	AWoTSpear* Spear = CitadelSpecFindValidSpear(World);
	TestNotNull(TEXT("created spear exists"), Spear);
	if (Spear)
	{
		TestTrue(TEXT("created spear is editing-dormant"),
			Spear->bEditing && Spear->IsInactive(nullptr));
	}
	Controller->CitadelEndEdit();
	// Retail play-start is the fire path only (see PortcullisLive); assess directly.
	BattleMode->AssessBattlefield();
	TestTrue(TEXT("edit-to-play begins play"), BattleMode->CanTeleport());
	if (Spear)
	{
		TestFalse(TEXT("created spear is live after edit-to-play"),
			Spear->bEditing || Spear->IsInactive(nullptr));
	}
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelCreateGuardsSpec,
	"WoT.Citadel.CreateGuards",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelCreateGuardsSpec::RunTest(const FString&)
{
	UWorld* World = CitadelSpecCreateWorld(true);
	CitadelSpecSpawnAltar(World, 1, FVector(0, 0, 0));
	CitadelSpecSpawnAltar(World, 2, FVector(5000, 0, 0));
	CitadelSpecSpawnFloor(World, FVector(0, 0, 0), FVector(200, 200, 10));
	AWoTBudgetInfo* BudgetInfo = World->SpawnActor<AWoTBudgetInfo>(
		CitadelSpecAlwaysSpawnParams());
	BudgetInfo->Team = 1;
	CitadelSpecSetResourceBudget(BudgetInfo, TEXT("SpearInventory"), 2);
	AWoTPlayerController* Controller = CitadelSpecSpawnController(World, 1);
	CitadelSpecSpawnEditZone(World, FVector::ZeroVector, FVector(1000), 1);
	Controller->CitadelBeginEdit();
	const float NaN = std::numeric_limits<float>::quiet_NaN();
	Controller->ServerCreateResource(
		TEXT("SpearInventory"), FVector(0, 0, 200), FVector(NaN, 0, 1));
	TestEqual(TEXT("non-finite normal rejected"), BudgetInfo->GetItemCount(
		BudgetInfo->GetIndex(TEXT("SpearInventory"))), 2);
	TestEqual(TEXT("non-finite normal leaves no spear"),
		CitadelSpecCountValidActors<AWoTSpear>(World), 0);

	Controller->ServerCreateResource(
		TEXT("SpearInventory"), FVector(4900, 0, 200), FVector(0, 0, 1));
	TestEqual(TEXT("foreign-citadel create rejected"), BudgetInfo->GetItemCount(
		BudgetInfo->GetIndex(TEXT("SpearInventory"))), 2);

	Controller->ServerCreateResource(
		TEXT("SpearInventory"), FVector(0, 0, 200), FVector::ZeroVector);
	TestEqual(TEXT("degenerate normal rejected"), BudgetInfo->GetItemCount(
		BudgetInfo->GetIndex(TEXT("SpearInventory"))), 2);

	Controller->ServerCreateResource(
		TEXT("SpearInventory"), FVector(0, 0, 200), FVector(0, 0, 1));
	AWoTSpear* Placed = nullptr;
	for (TActorIterator<AWoTSpear> It(World); It; ++It)
	{
		Placed = *It;
	}
	Controller->ServerRemoveResource(Placed);
	TestFalse(TEXT("successful remove destroys the actor"), IsValid(Placed));
	Controller->ServerRemoveResource(Placed);
	TestEqual(TEXT("no double refund"), BudgetInfo->GetItemCount(
		BudgetInfo->GetIndex(TEXT("SpearInventory"))), 2);
	TestEqual(TEXT("successful remove destroys all owned blockers"),
		CitadelSpecCountValidActors<AWoTTrapBlocker>(World), 0);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelWallCalcSpec,
	"WoT.Citadel.CreateWallCalc",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelWallCalcSpec::RunTest(const FString&)
{
	UWorld* World = CitadelSpecCreateWorld(true);
	CitadelSpecSpawnFloor(World, FVector::ZeroVector, FVector(1000, 1000, 10));
	CitadelSpecSpawnFloor(
		World, FVector(600, 0, 100), FVector(10, 100, 200));
	AWoTWall* Wall = World->SpawnActor<AWoTWall>(
		FVector(0, 0, 50), FRotator::ZeroRotator, CitadelSpecAlwaysSpawnParams());
	TestNotNull(TEXT("wall calc fixture spawned"), Wall);
	if (IsValid(Wall))
	{
		FVector StartLocation(0, 0, 50);
		FVector StartNormal(1, 0, 0);
		TestTrue(TEXT("valid room resolves"),
			Wall->CalcLocation(StartLocation, StartNormal));
		TestEqual(TEXT("590 UU room uses 22 slabs"), Wall->GetNumSlabsForTest(), 22);
		TestTrue(TEXT("manager Z uses capsule half-height divided by two"),
			FMath::IsNearlyEqual(StartLocation.Z, 81.0f, 0.1f));
		const float ExpectedYaws[] = {0.0f, 90.0f, -90.0f, 180.0f};
		for (int32 SlabIndex = 0; SlabIndex < 4; ++SlabIndex)
		{
			TestTrue(TEXT("base slab yaw cycle is 0/+90/-90/180 degrees"),
				FMath::IsNearlyEqual(
					Wall->GetSlabRotation(SlabIndex, StartNormal).Yaw,
					ExpectedYaws[SlabIndex], 0.1f));
		}

		FVector VerticalLocation(0, 0, 50);
		FVector VerticalNormal(0, 0, 1);
		TestFalse(TEXT("non-horizontal normal rejected"),
			Wall->CalcLocation(VerticalLocation, VerticalNormal));
	}

	AWoTWall* MissWall = World->SpawnActor<AWoTWall>(
		FVector(0, 500, 50), FRotator::ZeroRotator,
		CitadelSpecAlwaysSpawnParams());
	FVector MissLocation(0, 500, 50);
	FVector MissNormal(1, 0, 0);
	TestNotNull(TEXT("forward-miss wall fixture spawned"), MissWall);
	if (IsValid(MissWall))
	{
		TestFalse(TEXT("forward trace must hit level geometry"),
			MissWall->CalcLocation(MissLocation, MissNormal));
	}
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelWallSlabHeightSpec,
	"WoT.Citadel.WallSlabHeightFromTemplate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelWallSlabHeightSpec::RunTest(const FString&)
{
	// Wall.uc places the manager at SlabClass.default.CollisionHeight / 2 above the floor.
	// The slab template's capsule is never registered; packaged builds answer 0 from
	// AActor::GetSimpleCollisionHalfHeight there (only WITH_EDITOR ignores registration),
	// so the height must come from the capsule itself.
	const AWoTWallSlab* SlabCDO = GetDefault<AWoTWallSlab>();
	TestEqual(TEXT("slab template reads WallSlab.default.CollisionHeight"),
		SlabCDO->GetSlabCollisionHalfHeight(), 142.0f);
	const AWoTWall* WallCDO = GetDefault<AWoTWall>();
	TestEqual(TEXT("wall placement height comes from the slab template capsule"),
		WallCDO->GetSlabHalfHeight(), 142.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelWallFloorGuardsSpec,
	"WoT.Citadel.CreateWallFloorGuards",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelWallFloorGuardsSpec::RunTest(const FString&)
{
	UWorld* World = CitadelSpecCreateWorld(true);

	// At Y=0 the first advance-before-trace sample (X=42) meets a foreign actor.
	CitadelSpecSpawnFloor(World, FVector(0, 0, 0), FVector(20, 100, 10));
	CitadelSpecSpawnFloor(World, FVector(320, 0, 0), FVector(260, 100, 10));
	CitadelSpecSpawnFloor(World, FVector(600, 0, 100), FVector(10, 100, 200));
	CitadelSpecSpawnDynamicOccupant(
		World, FVector(42, 0, -5), FVector(5, 5, 5));
	AWoTWall* ForeignFloorWall = World->SpawnActor<AWoTWall>(
		FVector(0, 0, 50), FRotator::ZeroRotator, CitadelSpecAlwaysSpawnParams());
	FVector ForeignFloorLocation(0, 0, 50);
	FVector HorizontalNormal(1, 0, 0);
	TestNotNull(TEXT("foreign-floor wall fixture spawned"), ForeignFloorWall);
	if (IsValid(ForeignFloorWall))
	{
		TestFalse(TEXT("foreign actor under slab run rejected"),
			ForeignFloorWall->CalcLocation(ForeignFloorLocation, HorizontalNormal));
	}

	// At Y=400, sampled floor is 101 UU below the initial floor: outside tolerance.
	CitadelSpecSpawnFloor(World, FVector(0, 400, 0), FVector(20, 100, 10));
	CitadelSpecSpawnFloor(World, FVector(310, 400, -101), FVector(290, 100, 10));
	CitadelSpecSpawnFloor(World, FVector(600, 400, 100), FVector(10, 100, 200));
	AWoTWall* LowFloorWall = World->SpawnActor<AWoTWall>(
		FVector(0, 400, 50), FRotator::ZeroRotator, CitadelSpecAlwaysSpawnParams());
	FVector LowFloorLocation(0, 400, 50);
	TestNotNull(TEXT("low-floor wall fixture spawned"), LowFloorWall);
	if (IsValid(LowFloorWall))
	{
		TestFalse(TEXT("floor lower than FloorTolerance rejected"),
			LowFloorWall->CalcLocation(LowFloorLocation, HorizontalNormal));
	}

	// A higher run is accepted; min-Z remains the initial floor top at Z=10.
	CitadelSpecSpawnFloor(World, FVector(0, 800, 0), FVector(20, 100, 10));
	// The run volume encloses Z=10 so a downward ray begins inside its higher floor.
	CitadelSpecSpawnFloor(World, FVector(310, 800, 0), FVector(290, 100, 20));
	CitadelSpecSpawnFloor(World, FVector(600, 800, 100), FVector(10, 100, 200));
	AWoTWall* HighFloorWall = World->SpawnActor<AWoTWall>(
		FVector(0, 800, 50), FRotator::ZeroRotator, CitadelSpecAlwaysSpawnParams());
	FVector HighFloorLocation(0, 800, 50);
	TestNotNull(TEXT("high-floor wall fixture spawned"), HighFloorWall);
	if (IsValid(HighFloorWall))
	{
		TestTrue(TEXT("higher floor is accepted asymmetrically"),
			HighFloorWall->CalcLocation(HighFloorLocation, HorizontalNormal));
		TestTrue(TEXT("higher floor does not raise manager"),
			FMath::IsNearlyEqual(HighFloorLocation.Z, 81.0f, 0.1f));
	}

	// A lower run within tolerance is accepted and supplies the manager's lowest Z.
	CitadelSpecSpawnFloor(World, FVector(0, 1200, 0), FVector(20, 100, 10));
	// Extend past the far wall so the last slab sample (advance-before-trace lands
	// ~one slab beyond WallStop, the ported off-by-one) still hits this lower floor.
	CitadelSpecSpawnFloor(World, FVector(340, 1200, -50), FVector(340, 100, 10));
	CitadelSpecSpawnFloor(World, FVector(600, 1200, 100), FVector(10, 100, 200));
	AWoTWall* ToleratedFloorWall = World->SpawnActor<AWoTWall>(
		FVector(0, 1200, 50), FRotator::ZeroRotator,
		CitadelSpecAlwaysSpawnParams());
	FVector ToleratedFloorLocation(0, 1200, 50);
	TestNotNull(TEXT("tolerated-floor wall fixture spawned"), ToleratedFloorWall);
	if (IsValid(ToleratedFloorWall))
	{
		TestTrue(TEXT("lower floor within tolerance accepted"),
			ToleratedFloorWall->CalcLocation(ToleratedFloorLocation, HorizontalNormal));
		TestTrue(TEXT("manager uses lowest sampled floor plus 71"),
			FMath::IsNearlyEqual(ToleratedFloorLocation.Z, 31.0f, 0.1f));
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelWallCreateSpec,
	"WoT.Citadel.CreateWallDeploy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelWallCreateSpec::RunTest(const FString&)
{
	UWorld* World = CitadelSpecCreateWorld(true);
	AWoTBattleGameMode* BattleMode = World->GetAuthGameMode<AWoTBattleGameMode>();
	CitadelSpecSpawnAltar(World, 1, FVector::ZeroVector);
	CitadelSpecSpawnAltar(World, 2, FVector(5000, 0, 0));
	CitadelSpecSpawnFloor(World, FVector::ZeroVector, FVector(1000, 1000, 10));
	CitadelSpecSpawnFloor(
		World, FVector(600, 0, 100), FVector(10, 100, 200));
	AWoTBudgetInfo* BudgetInfo = World->SpawnActor<AWoTBudgetInfo>(
		CitadelSpecAlwaysSpawnParams());
	BudgetInfo->Team = 1;
	CitadelSpecSetResourceBudget(BudgetInfo, TEXT("WallInventory"), 1);
	CitadelSpecSetSealBudget(BudgetInfo, 0);
	AWoTPlayerController* Controller = CitadelSpecSpawnController(World, 1);
	CitadelSpecSpawnEditZone(World, FVector::ZeroVector, FVector(1000), 1);
	Controller->CitadelBeginEdit();
	Controller->ServerCreateResource(
		TEXT("WallInventory"), FVector(0, 0, 50), FVector(1, 0, 0));

	TestEqual(TEXT("wall create spends 1"), BudgetInfo->GetItemCount(
		BudgetInfo->GetIndex(TEXT("WallInventory"))), 0);
	AWoTWall* Wall = nullptr;
	for (TActorIterator<AWoTWall> It(World); It; ++It)
	{
		if (IsValid(*It))
		{
			Wall = *It;
			break;
		}
	}
	TestNotNull(TEXT("wall manager created"), Wall);
	if (Wall)
	{
		TestTrue(TEXT("manager positioned before final ownership recheck"),
			Wall->GetActorLocation().Equals(FVector(0, 0, 81), 0.1f));
		TestTrue(TEXT("production wall manager credits its placing pawn"),
			Wall->GetInstigator() == Controller->GetPawn());
	}

	uint32 FoundSlots = 0;
	int32 OwnedSlabCount = 0;
	for (TActorIterator<AWoTWallSlab> It(World); It; ++It)
	{
		AWoTWallSlab* Slab = *It;
		if (!IsValid(Slab) || Slab->GetOwner() != Wall)
		{
			continue;
		}
		++OwnedSlabCount;
		const int32 SlabIndex = FMath::RoundToInt((Slab->GetActorLocation().X - 14.0f) / 28.0f);
		const bool bValidIndex = SlabIndex >= 0 && SlabIndex < 22;
		TestTrue(TEXT("slab occupies an expected center slot"), bValidIndex);
		if (bValidIndex)
		{
			FoundSlots |= static_cast<uint32>(1) << SlabIndex;
			TestTrue(TEXT("slab center follows Normal * SlabWidth"),
				Slab->GetActorLocation().Equals(
					FVector(14.0f + SlabIndex * 28.0f, 0, 81), 0.1f));
			TestTrue(TEXT("slab uses base rotation cycle"),
				Slab->GetActorRotation().Equals(
					Wall->GetSlabRotation(SlabIndex, FVector(1, 0, 0)), 0.1f));
		}
		TestEqual(TEXT("slab inherits editing team"),
			static_cast<int32>(Slab->Team), 1);
		TestTrue(TEXT("production wall slab inherits the placing pawn for kill credit"),
			Slab->GetInstigator() == Controller->GetPawn());
		TestTrue(TEXT("slab is dormant while editing"),
			Slab->bEditing && Slab->IsInactive(nullptr));
	}
	TestEqual(TEXT("all 22 slabs spawned"), OwnedSlabCount, 22);
	TestEqual(TEXT("all slab center slots occupied"), FoundSlots, 0x003fffffu);

	Controller->CitadelEndEdit();
	// Retail play-start is the fire path only (see PortcullisLive); assess directly.
	BattleMode->AssessBattlefield();
	TestTrue(TEXT("edit-to-play begins play"), BattleMode->CanTeleport());
	for (TActorIterator<AWoTWallSlab> It(World); It; ++It)
	{
		AWoTWallSlab* Slab = *It;
		if (IsValid(Slab) && Slab->GetOwner() == Wall)
		{
			TestFalse(TEXT("slab is live after edit-to-play"),
				Slab->bEditing || Slab->IsInactive(nullptr));
		}
	}
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelWallGuardsSpec,
	"WoT.Citadel.CreateWallGuards",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelWallGuardsSpec::RunTest(const FString&)
{
	UWorld* World = CitadelSpecCreateWorld(true);
	CitadelSpecSpawnAltar(World, 1, FVector::ZeroVector);
	CitadelSpecSpawnAltar(World, 2, FVector(5000, 0, 0));
	CitadelSpecSpawnFloor(World, FVector(450, 250, 0), FVector(1000, 1000, 10));
	CitadelSpecSpawnFloor(World, FVector(600, 0, 100), FVector(10, 100, 200));
	CitadelSpecSpawnFloor(World, FVector(910, 500, 100), FVector(10, 100, 200));
	CitadelSpecSpawnFloor(World, FVector(878, 1000, 100), FVector(10, 100, 200));
	CitadelSpecSpawnDynamicOccupant(
		World, FVector(14, 0, 10), FVector(2, 2, 2));
	AWoTBudgetInfo* BudgetInfo = World->SpawnActor<AWoTBudgetInfo>(
		CitadelSpecAlwaysSpawnParams());
	BudgetInfo->Team = 1;
	CitadelSpecSetResourceBudget(BudgetInfo, TEXT("WallInventory"), 1);
	AWoTPlayerController* Controller = CitadelSpecSpawnController(World, 1);
	CitadelSpecSpawnEditZone(World, FVector::ZeroVector, FVector(1000), 1);
	Controller->CitadelBeginEdit();
	AWoTWall* BoundaryWall = World->SpawnActor<AWoTWall>(
		FVector(0, 1000, 50), FRotator::ZeroRotator,
		CitadelSpecAlwaysSpawnParams());
	FVector BoundaryLocation(0, 1000, 50);
	FVector BoundaryNormal(1, 0, 0);
	TestNotNull(TEXT("MaxSlabs boundary wall fixture spawned"), BoundaryWall);
	if (IsValid(BoundaryWall))
	{
		TestTrue(TEXT("exact MaxSlabs boundary is accepted"),
			BoundaryWall->CalcLocation(BoundaryLocation, BoundaryNormal));
		TestEqual(TEXT("boundary room uses MaxSlabs"),
			BoundaryWall->GetNumSlabsForTest(), 32);
		BoundaryWall->Destroy();
	}

	Controller->ServerCreateResource(
		TEXT("WallInventory"), FVector(0, 0, 50), FVector(1, 0, 0));
	TestEqual(TEXT("occupied footprint does not spend"), BudgetInfo->GetItemCount(
		BudgetInfo->GetIndex(TEXT("WallInventory"))), 1);
	TestEqual(TEXT("occupied footprint leaves no wall"),
		CitadelSpecCountValidActors<AWoTWall>(World), 0);
	TestEqual(TEXT("occupied footprint leaves no slabs"),
		CitadelSpecCountValidActors<AWoTWallSlab>(World), 0);

	Controller->ServerCreateResource(
		TEXT("WallInventory"), FVector(0, 500, 50), FVector(1, 0, 0));
	TestEqual(TEXT("NumSlabs above MaxSlabs does not spend"),
		BudgetInfo->GetItemCount(BudgetInfo->GetIndex(TEXT("WallInventory"))), 1);
	TestEqual(TEXT("too-wide room leaves no wall"),
		CitadelSpecCountValidActors<AWoTWall>(World), 0);
	TestEqual(TEXT("too-wide room leaves no slabs"),
		CitadelSpecCountValidActors<AWoTWallSlab>(World), 0);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelWallRemoveSpec,
	"WoT.Citadel.CreateWallRemove",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelWallRemoveSpec::RunTest(const FString&)
{
	UWorld* World = CitadelSpecCreateWorld(true);
	CitadelSpecSpawnAltar(World, 1, FVector::ZeroVector);
	CitadelSpecSpawnAltar(World, 2, FVector(5000, 0, 0));
	CitadelSpecSpawnFloor(World, FVector::ZeroVector, FVector(1000, 1000, 10));
	CitadelSpecSpawnFloor(
		World, FVector(400, 0, 100), FVector(10, 100, 200));
	AWoTBudgetInfo* BudgetInfo = World->SpawnActor<AWoTBudgetInfo>(
		CitadelSpecAlwaysSpawnParams());
	BudgetInfo->Team = 1;
	CitadelSpecSetResourceBudget(BudgetInfo, TEXT("WallInventory"), 1);
	AWoTPlayerController* Controller = CitadelSpecSpawnController(World, 1);
	CitadelSpecSpawnEditZone(World, FVector::ZeroVector, FVector(1000), 1);
	Controller->CitadelBeginEdit();
	Controller->ServerCreateResource(
		TEXT("WallInventory"), FVector(0, 0, 50), FVector(1, 0, 0));

	AWoTWall* Wall = nullptr;
	AWoTWallSlab* AnySlab = nullptr;
	for (TActorIterator<AWoTWall> It(World); It; ++It)
	{
		if (IsValid(*It))
		{
			Wall = *It;
			break;
		}
	}
	for (TActorIterator<AWoTWallSlab> It(World); It; ++It)
	{
		if (IsValid(*It))
		{
			AnySlab = *It;
			break;
		}
	}
	TestNotNull(TEXT("created wall exists"), Wall);
	TestNotNull(TEXT("created slab exists"), AnySlab);
	if (Wall && AnySlab)
	{
		Wall->Team = 2;
		Controller->ServerRemoveResource(AnySlab);
		TestTrue(TEXT("foreign-team wall is not removed through its slab"),
			IsValid(Wall) && IsValid(AnySlab));
		Wall->Team = 1;

		const FVector HomeLocation = Wall->GetActorLocation();
		Wall->SetActorLocation(FVector(5000, 0, HomeLocation.Z));
		Controller->ServerRemoveResource(AnySlab);
		TestTrue(TEXT("foreign-citadel wall is not removed through its slab"),
			IsValid(Wall) && IsValid(AnySlab));
		Wall->SetActorLocation(HomeLocation);

		Controller->ServerRemoveResource(AnySlab);
		TestFalse(TEXT("removing a slab destroys its wall manager"), IsValid(Wall));
		TestEqual(TEXT("removing a slab refunds one wall"),
			BudgetInfo->GetItemCount(BudgetInfo->GetIndex(TEXT("WallInventory"))), 1);
		TestEqual(TEXT("wall destroy cascade removes every slab"),
			CitadelSpecCountValidActors<AWoTWallSlab>(World), 0);
	}
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelFireWallFlatRotationSpec,
	"WoT.Citadel.CreateFireWallFlatRotation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelFireWallFlatRotationSpec::RunTest(const FString&)
{
	UWorld* World = CitadelSpecCreateWorld(true);
	CitadelSpecSpawnAltar(World, 1, FVector::ZeroVector);
	CitadelSpecSpawnAltar(World, 2, FVector(5000, 0, 0));
	CitadelSpecSpawnFloor(World, FVector::ZeroVector, FVector(1000, 1000, 10));
	CitadelSpecSpawnFloor(
		World, FVector(600, 0, 100), FVector(10, 100, 200));
	AWoTBudgetInfo* BudgetInfo = World->SpawnActor<AWoTBudgetInfo>(
		CitadelSpecAlwaysSpawnParams());
	AWoTPlayerController* Controller = CitadelSpecSpawnController(World, 1);
	TestNotNull(TEXT("fire wall budget fixture spawned"), BudgetInfo);
	TestNotNull(TEXT("fire wall controller fixture spawned"), Controller);
	if (IsValid(BudgetInfo) && IsValid(Controller))
	{
		BudgetInfo->Team = 1;
		CitadelSpecSetResourceBudget(BudgetInfo, TEXT("FireWallInventory"), 1);
		CitadelSpecSpawnEditZone(World, FVector::ZeroVector, FVector(1000), 1);
		Controller->CitadelBeginEdit();
		Controller->ServerCreateResource(
			TEXT("FireWallInventory"), FVector(0, 0, 50), FVector(1, 0, 0));

		TestEqual(TEXT("fire wall create spends 1"), BudgetInfo->GetItemCount(
			BudgetInfo->GetIndex(TEXT("FireWallInventory"))), 0);
		AWoTFireWall* FireWall = nullptr;
		for (TActorIterator<AWoTFireWall> It(World); It; ++It)
		{
			if (IsValid(*It))
			{
				FireWall = *It;
				break;
			}
		}
		TestNotNull(TEXT("fire wall manager created"), FireWall);
		if (IsValid(FireWall))
		{
			const FVector RunNormal(1, 0, 0);
			TestTrue(TEXT("rotation ignores slab index"), FMath::IsNearlyEqual(
				FireWall->GetSlabRotation(1, RunNormal).Yaw,
				FireWall->GetSlabRotation(0, RunNormal).Yaw));

			int32 OwnedSlabCount = 0;
			float FirstYaw = 0.0f;
			for (TActorIterator<AWoTFireWallSlab> It(World); It; ++It)
			{
				AWoTFireWallSlab* Slab = *It;
				if (!IsValid(Slab) || Slab->GetOwner() != FireWall)
				{
					continue;
				}
				if (OwnedSlabCount == 0)
				{
					FirstYaw = Slab->GetActorRotation().Yaw;
				}
				else
				{
					TestTrue(TEXT("every fire wall slab has the same yaw"),
						FMath::IsNearlyEqual(Slab->GetActorRotation().Yaw, FirstYaw));
				}
				++OwnedSlabCount;
			}
			TestTrue(TEXT("flat-rotation fixture spawned at least two slabs"),
				OwnedSlabCount >= 2);
		}
	}
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelFireWallTeleporterRejectedSpec,
	"WoT.Citadel.CreateFireWallTeleporterRejected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelFireWallTeleporterRejectedSpec::RunTest(const FString&)
{
	UWorld* World = CitadelSpecCreateWorld(true);
	CitadelSpecSpawnAltar(World, 1, FVector::ZeroVector);
	CitadelSpecSpawnAltar(World, 2, FVector(5000, 0, 0));
	CitadelSpecSpawnFloor(World, FVector::ZeroVector, FVector(1000, 1000, 10));
	CitadelSpecSpawnFloor(
		World, FVector(600, 0, 100), FVector(10, 100, 200));
	AWoTTeleporter* Teleporter = World->SpawnActor<AWoTTeleporter>(
		FVector(19, 60, 50), FRotator::ZeroRotator, CitadelSpecAlwaysSpawnParams());
	AWoTBudgetInfo* BudgetInfo = World->SpawnActor<AWoTBudgetInfo>(
		CitadelSpecAlwaysSpawnParams());
	AWoTPlayerController* Controller = CitadelSpecSpawnController(World, 1);
	TestNotNull(TEXT("near teleporter fixture spawned"), Teleporter);
	TestNotNull(TEXT("teleporter reject budget fixture spawned"), BudgetInfo);
	TestNotNull(TEXT("teleporter reject controller fixture spawned"), Controller);
	if (IsValid(Teleporter) && IsValid(BudgetInfo) && IsValid(Controller))
	{
		// FireWall.uc's teleporter pass does not repeat the base bHidden/collision
		// filter, and UE1 RadiusActors walks actors regardless of collision state
		// (a destination-only teleporter runs with collision off). Hide AND disable
		// collision so this stays a reject — an overlap-query port would miss it.
		Teleporter->SetActorHiddenInGame(true);
		if (Teleporter->Collision)
		{
			Teleporter->Collision->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		}
		BudgetInfo->Team = 1;
		CitadelSpecSetResourceBudget(BudgetInfo, TEXT("FireWallInventory"), 1);
		CitadelSpecSpawnEditZone(World, FVector::ZeroVector, FVector(1000), 1);
		Controller->CitadelBeginEdit();
		Controller->ServerCreateResource(
			TEXT("FireWallInventory"), FVector(0, 0, 50), FVector(1, 0, 0));

		TestEqual(TEXT("near teleporter does not spend"), BudgetInfo->GetItemCount(
			BudgetInfo->GetIndex(TEXT("FireWallInventory"))), 1);
		TestEqual(TEXT("near teleporter leaves no fire wall slabs"),
			CitadelSpecCountValidActors<AWoTFireWallSlab>(World), 0);
	}
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelFireWallTeleporterFarAllowedSpec,
	"WoT.Citadel.CreateFireWallTeleporterFarAllowed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelFireWallTeleporterFarAllowedSpec::RunTest(const FString&)
{
	UWorld* World = CitadelSpecCreateWorld(true);
	CitadelSpecSpawnAltar(World, 1, FVector::ZeroVector);
	CitadelSpecSpawnAltar(World, 2, FVector(5000, 0, 0));
	CitadelSpecSpawnFloor(World, FVector::ZeroVector, FVector(1000, 1000, 10));
	CitadelSpecSpawnFloor(
		World, FVector(600, 0, 100), FVector(10, 100, 200));
	AWoTTeleporter* Teleporter = World->SpawnActor<AWoTTeleporter>(
		FVector(19, 1500, 50), FRotator::ZeroRotator, CitadelSpecAlwaysSpawnParams());
	AWoTBudgetInfo* BudgetInfo = World->SpawnActor<AWoTBudgetInfo>(
		CitadelSpecAlwaysSpawnParams());
	AWoTPlayerController* Controller = CitadelSpecSpawnController(World, 1);
	TestNotNull(TEXT("far teleporter fixture spawned"), Teleporter);
	TestNotNull(TEXT("teleporter allow budget fixture spawned"), BudgetInfo);
	TestNotNull(TEXT("teleporter allow controller fixture spawned"), Controller);
	if (IsValid(Teleporter) && IsValid(BudgetInfo) && IsValid(Controller))
	{
		BudgetInfo->Team = 1;
		CitadelSpecSetResourceBudget(BudgetInfo, TEXT("FireWallInventory"), 1);
		CitadelSpecSpawnEditZone(World, FVector::ZeroVector, FVector(1000), 1);
		Controller->CitadelBeginEdit();
		Controller->ServerCreateResource(
			TEXT("FireWallInventory"), FVector(0, 0, 50), FVector(1, 0, 0));

		TestEqual(TEXT("far teleporter allows spend"), BudgetInfo->GetItemCount(
			BudgetInfo->GetIndex(TEXT("FireWallInventory"))), 0);
		TestTrue(TEXT("far teleporter allows fire wall slabs"),
			CitadelSpecCountValidActors<AWoTFireWallSlab>(World) > 0);
	}
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelFireWallMaxSlabsSpec,
	"WoT.Citadel.CreateFireWallMaxSlabs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelFireWallMaxSlabsSpec::RunTest(const FString&)
{
	UWorld* World = CitadelSpecCreateWorld(true);
	CitadelSpecSpawnAltar(World, 1, FVector::ZeroVector);
	CitadelSpecSpawnAltar(World, 2, FVector(5000, 0, 0));
	CitadelSpecSpawnFloor(World, FVector(350, 250, 0), FVector(1000, 1000, 10));
	CitadelSpecSpawnFloor(World, FVector(656, 0, 100), FVector(10, 100, 200));
	CitadelSpecSpawnFloor(World, FVector(694, 500, 100), FVector(10, 100, 200));
	AWoTFireWall* BoundaryFireWall = World->SpawnActor<AWoTFireWall>(
		FVector(0, 0, 50), FRotator::ZeroRotator, CitadelSpecAlwaysSpawnParams());
	FVector BoundaryLocation(0, 0, 50);
	FVector BoundaryNormal(1, 0, 0);
	TestNotNull(TEXT("MaxSlabs fire wall fixture spawned"), BoundaryFireWall);
	if (IsValid(BoundaryFireWall))
	{
		TestTrue(TEXT("exact FireWall MaxSlabs boundary is accepted"),
			BoundaryFireWall->CalcLocation(BoundaryLocation, BoundaryNormal));
		TestEqual(TEXT("FireWall boundary room uses MaxSlabs"),
			BoundaryFireWall->GetNumSlabsForTest(), 18);
		BoundaryFireWall->Destroy();
	}

	AWoTBudgetInfo* BudgetInfo = World->SpawnActor<AWoTBudgetInfo>(
		CitadelSpecAlwaysSpawnParams());
	AWoTPlayerController* Controller = CitadelSpecSpawnController(World, 1);
	TestNotNull(TEXT("MaxSlabs budget fixture spawned"), BudgetInfo);
	TestNotNull(TEXT("MaxSlabs controller fixture spawned"), Controller);
	if (IsValid(BudgetInfo) && IsValid(Controller))
	{
		BudgetInfo->Team = 1;
		CitadelSpecSetResourceBudget(BudgetInfo, TEXT("FireWallInventory"), 1);
		CitadelSpecSpawnEditZone(World, FVector::ZeroVector, FVector(1000), 1);
		Controller->CitadelBeginEdit();
		Controller->ServerCreateResource(
			TEXT("FireWallInventory"), FVector(0, 500, 50), FVector(1, 0, 0));

		TestEqual(TEXT("FireWall above MaxSlabs does not spend"),
			BudgetInfo->GetItemCount(
				BudgetInfo->GetIndex(TEXT("FireWallInventory"))), 1);
		TestEqual(TEXT("too-wide room leaves no fire wall slabs"),
			CitadelSpecCountValidActors<AWoTFireWallSlab>(World), 0);
	}
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelFireWallRemoveRefundSpec,
	"WoT.Citadel.CreateFireWallRemoveRefund",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelFireWallRemoveRefundSpec::RunTest(const FString&)
{
	UWorld* World = CitadelSpecCreateWorld(true);
	CitadelSpecSpawnAltar(World, 1, FVector::ZeroVector);
	CitadelSpecSpawnAltar(World, 2, FVector(5000, 0, 0));
	CitadelSpecSpawnFloor(World, FVector::ZeroVector, FVector(1000, 1000, 10));
	CitadelSpecSpawnFloor(
		World, FVector(400, 0, 100), FVector(10, 100, 200));
	AWoTBudgetInfo* BudgetInfo = World->SpawnActor<AWoTBudgetInfo>(
		CitadelSpecAlwaysSpawnParams());
	AWoTPlayerController* Controller = CitadelSpecSpawnController(World, 1);
	TestNotNull(TEXT("remove budget fixture spawned"), BudgetInfo);
	TestNotNull(TEXT("remove controller fixture spawned"), Controller);
	if (IsValid(BudgetInfo) && IsValid(Controller))
	{
		BudgetInfo->Team = 1;
		CitadelSpecSetResourceBudget(BudgetInfo, TEXT("FireWallInventory"), 1);
		CitadelSpecSpawnEditZone(World, FVector::ZeroVector, FVector(1000), 1);
		Controller->CitadelBeginEdit();
		Controller->ServerCreateResource(
			TEXT("FireWallInventory"), FVector(0, 0, 50), FVector(1, 0, 0));

		AWoTFireWall* FireWall = nullptr;
		AWoTFireWallSlab* AnySlab = nullptr;
		for (TActorIterator<AWoTFireWall> It(World); It; ++It)
		{
			if (IsValid(*It))
			{
				FireWall = *It;
				break;
			}
		}
		for (TActorIterator<AWoTFireWallSlab> It(World); It; ++It)
		{
			if (IsValid(*It))
			{
				AnySlab = *It;
				break;
			}
		}
		TestNotNull(TEXT("created fire wall exists"), FireWall);
		TestNotNull(TEXT("created fire wall slab exists"), AnySlab);
		if (IsValid(FireWall) && IsValid(AnySlab))
		{
			TestEqual(TEXT("fire wall slab resolves its manager"),
				AnySlab->GetBaseResource(), static_cast<AActor*>(FireWall));
			Controller->ServerRemoveResource(AnySlab);
			TestFalse(TEXT("removing a fire wall slab destroys its manager"),
				IsValid(FireWall));
			TestEqual(TEXT("removing a fire wall slab refunds one"),
				BudgetInfo->GetItemCount(
					BudgetInfo->GetIndex(TEXT("FireWallInventory"))), 1);
			TestEqual(TEXT("fire wall destroy cascade removes every slab"),
				CitadelSpecCountValidActors<AWoTFireWallSlab>(World), 0);
		}
	}
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelFireWallRegisteredSpec,
	"WoT.Citadel.CreateFireWallRegistered",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelFireWallRegisteredSpec::RunTest(const FString&)
{
	const FWoTResourceDescriptor& FireWall =
		WoTCitadelResources::FindByAdapterName(TEXT("FireWallInventory"));
	TestTrue(TEXT("fire wall adapter resolves"), FireWall.IsValidDescriptor());
	TestEqual(TEXT("fire wall resource class"), FireWall.ResourceClass,
		static_cast<UClass*>(AWoTFireWall::StaticClass()));
	TestTrue(TEXT("fire wall is spawn-temp"), FireWall.bSpawnTempResource);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelPitDeploySpec,
	"WoT.Citadel.CreatePitDeploy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelPitDeploySpec::RunTest(const FString&)
{
	UWorld* World = CitadelSpecCreateWorld(true);
	CitadelSpecSpawnAltar(World, 1, FVector::ZeroVector);
	CitadelSpecSpawnAltar(World, 2, FVector(5000, 0, 0));
	CitadelSpecSpawnFloor(World, FVector::ZeroVector, FVector(1000, 1000, 10));
	AWoTPitOpening* Opening =
		CitadelSpecSpawnPitOpening(World, FVector(1000, 1000, -200));
	AWoTBudgetInfo* BudgetInfo = World->SpawnActor<AWoTBudgetInfo>(
		CitadelSpecAlwaysSpawnParams());
	AWoTPlayerController* Controller = CitadelSpecSpawnController(World, 1);
	TestNotNull(TEXT("pit opening fixture spawned"), Opening);
	TestNotNull(TEXT("pit deploy budget fixture spawned"), BudgetInfo);
	TestNotNull(TEXT("pit deploy controller fixture spawned"), Controller);
	if (IsValid(Opening) && IsValid(BudgetInfo) && IsValid(Controller))
	{
		BudgetInfo->Team = 1;
		CitadelSpecSetResourceBudget(BudgetInfo, TEXT("PitInventory"), 1);
		CitadelSpecSetSealBudget(BudgetInfo, 0);
		CitadelSpecSpawnEditZone(World, FVector::ZeroVector, FVector(1000), 1);
		Controller->CitadelBeginEdit();
		const FVector AimLocation(0, 0, 10);
		Controller->ServerCreateResource(
			TEXT("PitInventory"), AimLocation, FVector::UpVector);

		TestEqual(TEXT("pit create spends 1"), BudgetInfo->GetItemCount(
			BudgetInfo->GetIndex(TEXT("PitInventory"))), 0);
		AWoTPit* Pit = nullptr;
		for (TActorIterator<AWoTPit> It(World); It; ++It)
		{
			if (IsValid(*It))
			{
				Pit = *It;
				break;
			}
		}
		TestNotNull(TEXT("pit manager created"), Pit);
		TestEqual(TEXT("exactly one pit manager created"),
			CitadelSpecCountValidActors<AWoTPit>(World), 1);
		if (IsValid(Pit))
		{
			TestNull(TEXT("final pit ownership is cleared"), Pit->GetOwner());
			TestTrue(TEXT("pit manager uses supplied floor aim"),
				Pit->GetActorLocation().Equals(AimLocation, 0.1f));
			TestTrue(TEXT("pit is dormant while editing"), Pit->bEditing);
			TestTrue(TEXT("pit surface was claimed"), IsValid(Pit->ThePitTrap));
			if (IsValid(Pit->ThePitTrap))
			{
				TestTrue(TEXT("pit surface positioned with manager"),
					Pit->ThePitTrap->GetActorLocation().Equals(AimLocation, 0.1f));
				TestNotNull(TEXT("pit surface collision component exists"),
					Pit->ThePitTrap->Collision.Get());
				if (IsValid(Pit->ThePitTrap->Collision))
				{
					TestEqual(TEXT("pit surface collision is off while editing"),
						Pit->ThePitTrap->Collision->GetCollisionEnabled(),
						ECollisionEnabled::NoCollision);
				}
			}
			UCapsuleComponent* ManagerCapsule =
				Cast<UCapsuleComponent>(Pit->GetRootComponent());
			TestNotNull(TEXT("pit manager capsule exists"), ManagerCapsule);
			if (IsValid(ManagerCapsule))
			{
				TestEqual(TEXT("pit manager collision is on while editing"),
					ManagerCapsule->GetCollisionEnabled(), ECollisionEnabled::QueryOnly);
			}

			Controller->CitadelEndEdit();
			TestFalse(TEXT("pit leaves editing state at play"), Pit->bEditing);
			if (IsValid(ManagerCapsule))
			{
				TestEqual(TEXT("pit manager collision turns off at play"),
					ManagerCapsule->GetCollisionEnabled(), ECollisionEnabled::NoCollision);
			}
			if (IsValid(Pit->ThePitTrap) && IsValid(Pit->ThePitTrap->Collision))
			{
				TestEqual(TEXT("pit surface collision turns on at play"),
					Pit->ThePitTrap->Collision->GetCollisionEnabled(),
					ECollisionEnabled::QueryOnly);
			}
		}
	}
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelPitNoOpeningSpec,
	"WoT.Citadel.CreatePitNoOpening",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelPitNoOpeningSpec::RunTest(const FString&)
{
	UWorld* World = CitadelSpecCreateWorld(true);
	CitadelSpecSpawnAltar(World, 1, FVector::ZeroVector);
	CitadelSpecSpawnAltar(World, 2, FVector(5000, 0, 0));
	CitadelSpecSpawnFloor(World, FVector::ZeroVector, FVector(1000, 1000, 10));
	AWoTBudgetInfo* BudgetInfo = World->SpawnActor<AWoTBudgetInfo>(
		CitadelSpecAlwaysSpawnParams());
	AWoTPlayerController* Controller = CitadelSpecSpawnController(World, 1);
	TestNotNull(TEXT("no-opening budget fixture spawned"), BudgetInfo);
	TestNotNull(TEXT("no-opening controller fixture spawned"), Controller);
	if (IsValid(BudgetInfo) && IsValid(Controller))
	{
		BudgetInfo->Team = 1;
		CitadelSpecSetResourceBudget(BudgetInfo, TEXT("PitInventory"), 1);
		CitadelSpecSpawnEditZone(World, FVector::ZeroVector, FVector(1000), 1);
		Controller->CitadelBeginEdit();
		Controller->ServerCreateResource(
			TEXT("PitInventory"), FVector(0, 0, 10), FVector::UpVector);

		TestEqual(TEXT("no opening does not spend"), BudgetInfo->GetItemCount(
			BudgetInfo->GetIndex(TEXT("PitInventory"))), 1);
		TestEqual(TEXT("no opening leaves no pit manager"),
			CitadelSpecCountValidActors<AWoTPit>(World), 0);
		TestEqual(TEXT("no opening leaves no pit surface"),
			CitadelSpecCountValidActors<AWoTPitTrap>(World), 0);
	}
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelPitDeployFailFreesOpeningSpec,
	"WoT.Citadel.CreatePitDeployFailFreesOpening",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelPitDeployFailFreesOpeningSpec::RunTest(const FString&)
{
	UWorld* World = CitadelSpecCreateWorld(true);
	CitadelSpecSpawnAltar(World, 1, FVector::ZeroVector);
	CitadelSpecSpawnAltar(World, 2, FVector(5000, 0, 0));
	AWoTPitOpening* Opening =
		CitadelSpecSpawnPitOpening(World, FVector(1000, 1000, -200));
	AWoTBudgetInfo* BudgetInfo = World->SpawnActor<AWoTBudgetInfo>(
		CitadelSpecAlwaysSpawnParams());
	AWoTPlayerController* Controller = CitadelSpecSpawnController(World, 1);
	TestNotNull(TEXT("deploy-fail opening fixture spawned"), Opening);
	TestNotNull(TEXT("deploy-fail budget fixture spawned"), BudgetInfo);
	TestNotNull(TEXT("deploy-fail controller fixture spawned"), Controller);
	if (IsValid(Opening) && IsValid(BudgetInfo) && IsValid(Controller))
	{
		BudgetInfo->Team = 1;
		CitadelSpecSetResourceBudget(BudgetInfo, TEXT("PitInventory"), 1);
		CitadelSpecSpawnEditZone(World, FVector::ZeroVector, FVector(1000), 1);
		Controller->CitadelBeginEdit();
		Controller->ServerCreateResource(
			TEXT("PitInventory"), FVector(0, 0, 10), FVector(1, 0, 0));

		TestEqual(TEXT("failed pit deploy does not spend"), BudgetInfo->GetItemCount(
			BudgetInfo->GetIndex(TEXT("PitInventory"))), 1);
		TestEqual(TEXT("failed pit deploy leaves no manager"),
			CitadelSpecCountValidActors<AWoTPit>(World), 0);
		TestEqual(TEXT("failed pit deploy destroys claimed surface"),
			CitadelSpecCountValidActors<AWoTPitTrap>(World), 0);
		TestNull(TEXT("failed pit deploy frees opening"), Opening->ThePitTrap.Get());
	}
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelPitClearanceSpec,
	"WoT.Citadel.CreatePitClearance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelPitClearanceSpec::RunTest(const FString&)
{
	UWorld* World = CitadelSpecCreateWorld(true);
	CitadelSpecSpawnFloor(World, FVector::ZeroVector, FVector(1000, 1000, 10));
	CitadelSpecSpawnPitOpening(World, FVector(1000, 1000, -200));
	AWoTPit* Pit = World->SpawnActor<AWoTPit>(
		FVector(0, 0, 10), FRotator::ZeroRotator, CitadelSpecAlwaysSpawnParams());
	TestNotNull(TEXT("clearance pit fixture spawned"), Pit);
	if (IsValid(Pit))
	{
		FVector AimLocation(0, 0, 10);
		FVector UpNormal = FVector::UpVector;
		AActor* AxialBox = CitadelSpecSpawnBox(
			World, FVector(28, 0, 12), FVector(2, 2, 2));
		TestFalse(TEXT("axial world-static obstruction rejects pit"),
			Pit->CalcLocation(AimLocation, UpNormal));
		if (IsValid(AxialBox))
		{
			AxialBox->Destroy();
			WoTTest::TickWorld(World, 1);
		}

		AActor* DiagonalBox = CitadelSpecSpawnBox(
			World, FVector(28, 28, 12), FVector(2, 2, 2));
		TestFalse(TEXT("diagonal-only world-static obstruction rejects pit"),
			Pit->CalcLocation(AimLocation, UpNormal));
		if (IsValid(DiagonalBox))
		{
			DiagonalBox->Destroy();
			WoTTest::TickWorld(World, 1);
		}

		const FVector SuppliedLocation = AimLocation;
		TestTrue(TEXT("same pit aim succeeds after obstructions clear"),
			Pit->CalcLocation(AimLocation, UpNormal));
		TestTrue(TEXT("pit clearance trusts and preserves supplied hit location"),
			AimLocation.Equals(SuppliedLocation, 0.0f));
	}
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelPitSteepNormalSpec,
	"WoT.Citadel.CreatePitSteepNormal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelPitSteepNormalSpec::RunTest(const FString&)
{
	UWorld* World = CitadelSpecCreateWorld(true);
	CitadelSpecSpawnPitOpening(World, FVector(1000, 1000, -200));
	AWoTPit* Pit = World->SpawnActor<AWoTPit>(
		FVector::ZeroVector, FRotator::ZeroRotator, CitadelSpecAlwaysSpawnParams());
	TestNotNull(TEXT("steep-normal pit fixture spawned"), Pit);
	if (IsValid(Pit))
	{
		FVector BoundaryLocation(0, 0, 100);
		FVector BoundaryNormal(FMath::Sqrt(1.0f - 0.95f * 0.95f), 0, 0.95f);
		TestTrue(TEXT("pit accepts exact Normal.Z 0.95 boundary"),
			Pit->CalcLocation(BoundaryLocation, BoundaryNormal));

		FVector RejectedLocation(0, 0, 100);
		FVector RejectedNormal(FMath::Sqrt(1.0f - 0.94f * 0.94f), 0, 0.94f);
		TestFalse(TEXT("pit rejects Normal.Z just below boundary"),
			Pit->CalcLocation(RejectedLocation, RejectedNormal));
	}
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelPitMoverExclusionSpec,
	"WoT.Citadel.CreatePitMoverExclusion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelPitMoverExclusionSpec::RunTest(const FString&)
{
	UWorld* World = CitadelSpecCreateWorld(true);
	CitadelSpecSpawnPitOpening(World, FVector(5000, 5000, -200));
	AWoTPit* Pit = World->SpawnActor<AWoTPit>(
		FVector::ZeroVector, FRotator::ZeroRotator, CitadelSpecAlwaysSpawnParams());
	AWoTMover* NearMover = World->SpawnActor<AWoTMover>(
		FVector::ZeroVector, FRotator::ZeroRotator, CitadelSpecAlwaysSpawnParams());
	AWoTMover* BoundaryMover = World->SpawnActor<AWoTMover>(
		FVector(1000, 0, 0), FRotator::ZeroRotator, CitadelSpecAlwaysSpawnParams());
	AWoTMover* HighMover = World->SpawnActor<AWoTMover>(
		FVector(2000, 0, 513), FRotator::ZeroRotator, CitadelSpecAlwaysSpawnParams());
	AWoTMover* DisabledMover = World->SpawnActor<AWoTMover>(
		FVector(3000, 0, 0), FRotator::ZeroRotator, CitadelSpecAlwaysSpawnParams());
	TestNotNull(TEXT("mover-exclusion pit fixture spawned"), Pit);
	TestNotNull(TEXT("near mover fixture spawned"), NearMover);
	TestNotNull(TEXT("boundary mover fixture spawned"), BoundaryMover);
	TestNotNull(TEXT("high mover fixture spawned"), HighMover);
	TestNotNull(TEXT("disabled mover fixture spawned"), DisabledMover);
	if (IsValid(Pit) && IsValid(NearMover) && IsValid(BoundaryMover)
		&& IsValid(HighMover) && IsValid(DisabledMover))
	{
		TestEqual(TEXT("horizontal delta below 120 rejects mover"),
			Pit->AnyActorsInArea(FVector(119, 0, 0), 1.0f),
			static_cast<AActor*>(NearMover));
		TestNull(TEXT("horizontal delta exactly 120 is allowed"),
			Pit->AnyActorsInArea(FVector(1120, 0, 0), 1.0f));
		// RadiusActors idiom: outer bound is 512 + the mover's CollisionRadius analog
		// (mesh bounds radius — 0 for these bare fixtures), so 513 sits just outside.
		TestNull(TEXT("same-XY mover beyond 512 + bounds radius is allowed"),
			Pit->AnyActorsInArea(FVector(2000, 0, 0), 1.0f));

		DisabledMover->SetActorHiddenInGame(true);
		DisabledMover->SetActorEnableCollision(false);
		TestEqual(TEXT("hidden collision-disabled near mover still rejects"),
			Pit->AnyActorsInArea(FVector(3050, 0, 0), 1.0f),
			static_cast<AActor*>(DisabledMover));
	}
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelPitRemoveRefundSpec,
	"WoT.Citadel.CreatePitRemoveRefund",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelPitRemoveRefundSpec::RunTest(const FString&)
{
	UWorld* World = CitadelSpecCreateWorld(true);
	CitadelSpecSpawnAltar(World, 1, FVector::ZeroVector);
	CitadelSpecSpawnAltar(World, 2, FVector(5000, 0, 0));
	CitadelSpecSpawnFloor(World, FVector::ZeroVector, FVector(1000, 1000, 10));
	AWoTPitOpening* Opening =
		CitadelSpecSpawnPitOpening(World, FVector(1000, 1000, -200));
	AWoTBudgetInfo* BudgetInfo = World->SpawnActor<AWoTBudgetInfo>(
		CitadelSpecAlwaysSpawnParams());
	AWoTPlayerController* Controller = CitadelSpecSpawnController(World, 1);
	TestNotNull(TEXT("pit remove opening fixture spawned"), Opening);
	TestNotNull(TEXT("pit remove budget fixture spawned"), BudgetInfo);
	TestNotNull(TEXT("pit remove controller fixture spawned"), Controller);
	if (IsValid(Opening) && IsValid(BudgetInfo) && IsValid(Controller))
	{
		BudgetInfo->Team = 1;
		CitadelSpecSetResourceBudget(BudgetInfo, TEXT("PitInventory"), 1);
		CitadelSpecSpawnEditZone(World, FVector::ZeroVector, FVector(1000), 1);
		Controller->CitadelBeginEdit();
		Controller->ServerCreateResource(
			TEXT("PitInventory"), FVector(0, 0, 10), FVector::UpVector);

		AWoTPit* Pit = nullptr;
		for (TActorIterator<AWoTPit> It(World); It; ++It)
		{
			if (IsValid(*It))
			{
				Pit = *It;
				break;
			}
		}
		TestNotNull(TEXT("removable pit manager created"), Pit);
		if (IsValid(Pit))
		{
			FHitResult TargetHit;
			FCollisionQueryParams Params(SCENE_QUERY_STAT(CitadelPitRemoveTrace), false);
			const bool bHit = World->LineTraceSingleByChannel(
				TargetHit, FVector(0, 0, 100), FVector(0, 0, -100),
				ECC_Visibility, Params);
			TestTrue(TEXT("visibility targeting trace hits during editing"), bHit);
			TestEqual(TEXT("visibility targeting trace selects pit manager"),
				TargetHit.GetActor(), static_cast<AActor*>(Pit));
			AWoTTrap* TargetedTrap = Cast<AWoTTrap>(TargetHit.GetActor());
			TestNotNull(TEXT("targeted pit is removable as a trap"), TargetedTrap);
			if (IsValid(TargetedTrap))
			{
				Controller->ServerRemoveResource(TargetedTrap);
			}
			TestFalse(TEXT("pit removal destroys manager"), IsValid(Pit));
			TestEqual(TEXT("pit removal refunds one"), BudgetInfo->GetItemCount(
				BudgetInfo->GetIndex(TEXT("PitInventory"))), 1);
			TestEqual(TEXT("pit removal destroys surface"),
				CitadelSpecCountValidActors<AWoTPitTrap>(World), 0);
			TestNull(TEXT("pit removal frees opening"), Opening->ThePitTrap.Get());
		}
	}
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelPitRegisteredSpec,
	"WoT.Citadel.CreatePitRegistered",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelPitRegisteredSpec::RunTest(const FString&)
{
	const FWoTResourceDescriptor& Pit =
		WoTCitadelResources::FindByAdapterName(TEXT("PitInventory"));
	TestTrue(TEXT("pit adapter resolves"), Pit.IsValidDescriptor());
	TestEqual(TEXT("pit resource class"), Pit.ResourceClass,
		static_cast<UClass*>(AWoTPit::StaticClass()));
	TestTrue(TEXT("pit is spawn-temp"), Pit.bSpawnTempResource);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelEditZoneLookupSpec,
	"WoT.Citadel.EditZone.Lookup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelEditZoneLookupSpec::RunTest(const FString&)
{
	UWorld* World = CitadelSpecCreateWorld();
	AWoTZoneVolume* Defaults = World->SpawnActor<AWoTZoneVolume>(
		FVector(1000, 0, 0), FRotator::ZeroRotator,
		CitadelSpecAlwaysSpawnParams());
	TestNotNull(TEXT("default metadata zone spawned"), Defaults);
	if (IsValid(Defaults))
	{
		TestEqual(TEXT("default EditTeam"), static_cast<int32>(Defaults->EditTeam), 255);
		TestEqual(TEXT("default MinEditDistance"),
			static_cast<int32>(Defaults->MinEditDistance), 0);
		TestTrue(TEXT("default EditViewAngle pitch is -45 degrees"),
			FMath::IsNearlyEqual(Defaults->EditViewAngle.Pitch, -45.0f));
		TestTrue(TEXT("default EditViewDistance is 32"),
			FMath::IsNearlyEqual(Defaults->EditViewDistance, 32.0f));
		TestTrue(TEXT("default bAllowEditing is true"), Defaults->bAllowEditing);
	}

	AWoTZoneVolume* Large = CitadelSpecSpawnEditZone(
		World, FVector::ZeroVector, FVector(100), 1);
	AWoTZoneVolume* SmallFirst = CitadelSpecSpawnEditZone(
		World, FVector::ZeroVector, FVector(10), 2);
	AWoTZoneVolume* SmallTie = CitadelSpecSpawnEditZone(
		World, FVector::ZeroVector, FVector(10), 3);
	TestEqual(TEXT("outside all editor zones returns null"),
		AWoTZoneVolume::EditZoneAt(World, FVector(5000, 0, 0)),
		static_cast<AWoTZoneVolume*>(nullptr));
	TestEqual(TEXT("inside one editor zone resolves that zone"),
		AWoTZoneVolume::EditZoneAt(World, FVector(50, 0, 0)), Large);
	TestEqual(TEXT("smallest overlap wins and equal tie keeps earliest"),
		AWoTZoneVolume::EditZoneAt(World, FVector::ZeroVector), SmallFirst);
	TestNotEqual(TEXT("later equal-volume zone does not replace earliest"),
		AWoTZoneVolume::EditZoneAt(World, FVector::ZeroVector), SmallTie);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelEditZoneEntrySpec,
	"WoT.Citadel.EditZone.Entry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelEditZoneEntrySpec::RunTest(const FString&)
{
	UWorld* World = CitadelSpecCreateWorld(true);
	AWoTBudgetInfo* TeamOneBudget = World->SpawnActor<AWoTBudgetInfo>(
		CitadelSpecAlwaysSpawnParams());
	TeamOneBudget->Team = 1;
	AWoTBudgetInfo* TeamTwoBudget = World->SpawnActor<AWoTBudgetInfo>(
		CitadelSpecAlwaysSpawnParams());
	TeamTwoBudget->Team = 2;
	CitadelSpecSpawnEditZone(
		World, FVector::ZeroVector, FVector(100), 1, false);

	AWoTPlayerController* First = CitadelSpecSpawnController(World, 1);
	First->CitadelBeginEdit();
	TestTrue(TEXT("matching zone grants entry even when editing is disallowed"),
		First->IsEditingCitadel());
	AWoTPlayerController* Exhausted = CitadelSpecSpawnController(World, 1);
	Exhausted->CitadelBeginEdit();
	TestFalse(TEXT("exhausted edit slot rejects entry"), Exhausted->IsEditingCitadel());

	AWoTPlayerController* Absent = CitadelSpecSpawnController(
		World, 2, FVector(1000, 0, 0));
	Absent->CitadelBeginEdit();
	TestFalse(TEXT("absent zone rejects entry"), Absent->IsEditingCitadel());
	CitadelSpecSpawnEditZone(World, FVector(2000, 0, 0), FVector(100), 1);
	AWoTPlayerController* Foreign = CitadelSpecSpawnController(
		World, 2, FVector(2000, 0, 0));
	Foreign->CitadelBeginEdit();
	TestFalse(TEXT("foreign-team zone rejects entry"), Foreign->IsEditingCitadel());
	CitadelSpecSpawnEditZone(World, FVector(3000, 0, 0), FVector(100), 0);
	AWoTPlayerController* MissingBudget = CitadelSpecSpawnController(
		World, 0, FVector(3000, 0, 0));
	MissingBudget->CitadelBeginEdit();
	TestFalse(TEXT("missing budget rejects entry"), MissingBudget->IsEditingCitadel());
	CitadelSpecSpawnEditZone(World, FVector(4000, 0, 0), FVector(100), 255);
	AWoTPlayerController* Neutral = CitadelSpecSpawnController(
		World, 255, FVector(4000, 0, 0));
	Neutral->CitadelBeginEdit();
	TestFalse(TEXT("neutral player rejects even in neutral zone"), Neutral->IsEditingCitadel());

	First->CitadelEndEdit();
	AWoTBattleGameMode* BattleMode = World->GetAuthGameMode<AWoTBattleGameMode>();
	BattleMode->EnableTeleporters();
	AWoTPlayerController* Playing = CitadelSpecSpawnController(World, 1);
	Playing->CitadelBeginEdit();
	TestFalse(TEXT("play phase rejects entry"), Playing->IsEditingCitadel());
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelEditZoneArmingSpec,
	"WoT.Citadel.EditZone.Arming",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelEditZoneArmingSpec::RunTest(const FString&)
{
	UWorld* World = CitadelSpecCreateWorld(true);
	AWoTBudgetInfo* BudgetInfo = World->SpawnActor<AWoTBudgetInfo>(
		CitadelSpecAlwaysSpawnParams());
	BudgetInfo->Team = 1;
	CitadelSpecSpawnEditZone(World, FVector::ZeroVector, FVector(100), 1);
	CitadelSpecSpawnEditZone(World, FVector(1000, 0, 0), FVector(300), 1);
	CitadelSpecSpawnEditZone(World, FVector(2000, 0, 0), FVector(100), 2);
	AWoTPortcullis* Matching = CitadelSpecSpawnPortcullis(
		World, FVector(1000, 0, 0));
	AWoTPortcullis* Foreign = CitadelSpecSpawnPortcullis(
		World, FVector(2000, 0, 0));
	AWoTWall* Wall = World->SpawnActor<AWoTWall>(
		FVector(1100, 0, 0), FRotator::ZeroRotator, CitadelSpecAlwaysSpawnParams());
	AWoTWallSlab* Slab = World->SpawnActor<AWoTWallSlab>(
		FVector(1120, 0, 0), FRotator::ZeroRotator, CitadelSpecAlwaysSpawnParams());
	Slab->SetOwner(Wall);

	AWoTPlayerController* Controller = CitadelSpecSpawnController(World, 1);
	Controller->CitadelBeginEdit();
	TestTrue(TEXT("session-team-zone root is armed"), Matching->bEditing);
	TestFalse(TEXT("foreign-zone root is not armed"), Foreign->bEditing);
	TestTrue(TEXT("logical wall root is armed"), Wall->bEditing);
	TestTrue(TEXT("wall root propagates arming to its child"), Slab->bEditing);
	// Once-only arming is structural, not asserted via counters: CitadelBeginEdit early-
	// returns when IsEditingCitadel() (no re-arm), the arming loop visits each logical root
	// exactly once (TActorIterator filtered to GetBaseResource()==self, skipping slabs), and
	// composites cascade to children once — so a duplicate begin/end notification cannot occur.
	Controller->CitadelBeginEdit();
	TestTrue(TEXT("repeated begin is a no-op (still editing)"),
		Controller->IsEditingCitadel());
	TestFalse(TEXT("repeated entry does not arm foreign-zone roots"), Foreign->bEditing);

	CitadelSpecSpawnEditZone(World, FVector(3000, 0, 0), FVector(100), 2);
	AWoTPortcullis* FailedEntryRoot = CitadelSpecSpawnPortcullis(
		World, FVector(3000, 0, 0));
	AWoTPlayerController* NoBudget = CitadelSpecSpawnController(
		World, 2, FVector(3000, 0, 0));
	NoBudget->CitadelBeginEdit();
	TestFalse(TEXT("failed entry arms no roots"), FailedEntryRoot->bEditing);

	Controller->CitadelEndEdit();
	Controller->CitadelEndEdit();
	TestFalse(TEXT("session-team-zone root is disarmed once"), Matching->bEditing);
	TestFalse(TEXT("logical wall root is disarmed"), Wall->bEditing);
	TestFalse(TEXT("wall root propagates disarming to its child"), Slab->bEditing);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelEditZoneInteractionSpec,
	"WoT.Citadel.EditZone.Interaction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelEditZoneInteractionSpec::RunTest(const FString&)
{
	UWorld* World = CitadelSpecCreateWorld(true);
	CitadelSpecSpawnAltar(World, 1, FVector::ZeroVector);
	CitadelSpecSpawnAltar(World, 2, FVector(5000, 0, 0));
	CitadelSpecSpawnFloor(World, FVector::ZeroVector, FVector(500, 500, 10));
	AWoTBudgetInfo* BudgetInfo = World->SpawnActor<AWoTBudgetInfo>(
		CitadelSpecAlwaysSpawnParams());
	BudgetInfo->Team = 1;
	CitadelSpecSetResourceBudget(BudgetInfo, TEXT("SpearInventory"), 1);
	CitadelSpecSetResourceBudget(BudgetInfo, TEXT("PortcullisInventory"), 1);
	CitadelSpecSpawnEditZone(
		World, FVector::ZeroVector, FVector(100), 1, false);
	CitadelSpecSpawnEditZone(World, FVector(1000, 0, 0), FVector(100), 1, true);
	AWoTPortcullis* Gate = CitadelSpecSpawnPortcullis(World, FVector(50, 0, 0));
	AWoTSpear* Removable = World->SpawnActor<AWoTSpear>(
		FVector(60, 0, 10), FRotator::ZeroRotator, CitadelSpecAlwaysSpawnParams());
	Removable->Team = 1;
	AWoTPlayerController* Controller = CitadelSpecSpawnController(World, 1);
	Controller->CitadelBeginEdit();
	TestTrue(TEXT("interaction fixture entered edit session"), Controller->IsEditingCitadel());

	Controller->ServerToggleResource(Gate);
	Controller->ServerRemoveResource(Removable);
	Controller->ServerCreateResource(
		TEXT("SpearInventory"), FVector(-200, 0, 200), FVector::UpVector);
	TestFalse(TEXT("disallowed viewer zone blocks toggle"), Gate->IsActive());
	TestTrue(TEXT("disallowed viewer zone blocks remove"), IsValid(Removable));
	TestEqual(TEXT("disallowed viewer zone preserves spear budget"),
		BudgetInfo->GetItemCount(BudgetInfo->GetIndex(TEXT("SpearInventory"))), 1);
	TestEqual(TEXT("disallowed viewer zone creates no spear"),
		CitadelSpecCountValidActors<AWoTSpear>(World), 1);

	CitadelSpecSetViewerLocation(Controller, FVector(2000, 0, 0));
	Controller->ServerToggleResource(Gate);
	Controller->ServerRemoveResource(Removable);
	Controller->ServerCreateResource(
		TEXT("SpearInventory"), FVector(-200, 0, 200), FVector::UpVector);
	TestFalse(TEXT("missing viewer zone blocks toggle"), Gate->IsActive());
	TestTrue(TEXT("missing viewer zone blocks remove"), IsValid(Removable));
	TestEqual(TEXT("missing viewer zone preserves spear budget"),
		BudgetInfo->GetItemCount(BudgetInfo->GetIndex(TEXT("SpearInventory"))), 1);

	CitadelSpecSetViewerLocation(Controller, FVector(1000, 0, 0));
	Controller->ServerToggleResource(Gate);
	Controller->ServerRemoveResource(Removable);
	Controller->ServerCreateResource(
		TEXT("SpearInventory"), FVector(-200, 0, 200), FVector::UpVector);
	TestTrue(TEXT("allowed viewer zone permits toggle"), Gate->IsActive());
	TestFalse(TEXT("allowed viewer zone permits remove"), IsValid(Removable));
	TestEqual(TEXT("allowed remove and create preserve net spear budget"),
		BudgetInfo->GetItemCount(BudgetInfo->GetIndex(TEXT("SpearInventory"))), 1);
	TestEqual(TEXT("allowed viewer zone creates replacement spear"),
		CitadelSpecCountValidActors<AWoTSpear>(World), 1);
	WoTTest::DestroySpecWorld(World);
	return true;
}

namespace
{
	int32 CitadelSpecInvCount(const APawn* Pawn)
	{
		const UWoTInventoryComponent* Inv =
			Pawn ? Pawn->FindComponentByClass<UWoTInventoryComponent>() : nullptr;
		return Inv ? Inv->Items.Num() : -1;
	}

	ECollisionResponse CitadelSpecPawnResponse(const AWoTCharacter* Pawn)
	{
		const UCapsuleComponent* Capsule = Pawn ? Pawn->GetCapsuleComponent() : nullptr;
		return Capsule ? Capsule->GetCollisionResponseToChannel(ECC_Pawn) : ECR_MAX;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelEnableTeleportersRearmSpec,
	"WoT.Citadel.EnableTeleportersRearm",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelEnableTeleportersRearmSpec::RunTest(const FString&)
{
	// giMPBattle.EnableTeleporters (giMPBattle.uc:153-177): teamed players are un-frozen,
	// drop the overview, and get the battle loadout; NEUTRAL players stay frozen and
	// unarmed; everyone's JoinTeamTimeout re-arms.
	UWorld* World = CitadelSpecCreateWorld(true);
	AWoTBattleGameMode* BattleMode = World->GetAuthGameMode<AWoTBattleGameMode>();

	AWoTCharacter* TeamPawn = CitadelSpecSpawnPawn(World, 1);
	AWoTCharacter* NeutralPawn = CitadelSpecSpawnPawn(World, 255);
	AWoTPlayerController* TeamPC =
		Cast<AWoTPlayerController>(TeamPawn->GetController());
	AWoTPlayerController* NeutralPC =
		Cast<AWoTPlayerController>(NeutralPawn->GetController());

	BattleMode->DisableTeleporters();
	TestTrue(TEXT("edit phase freezes the teamed player"), TeamPC->bTeleportingDisabled);
	TestTrue(TEXT("edit phase freezes the neutral player"), NeutralPC->bTeleportingDisabled);
	TestEqual(TEXT("no loadout during the edit phase"), CitadelSpecInvCount(TeamPawn), 0);

	BattleMode->EnableTeleporters();
	TestFalse(TEXT("play un-freezes the teamed player"), TeamPC->bTeleportingDisabled);
	TestFalse(TEXT("play drops the teamed overview"), TeamPC->bShowOverview);
	TestTrue(TEXT("neutral player stays frozen after play begins"),
		NeutralPC->bTeleportingDisabled);
	TestEqual(TEXT("teamed pawn re-armed with the battle loadout (Dart + set1 + set2 + "
		"faction default)"), CitadelSpecInvCount(TeamPawn), 4);
	TestEqual(TEXT("neutral pawn stays unarmed"), CitadelSpecInvCount(NeutralPawn), 0);
	TestEqual(TEXT("teamed join-nag timer re-armed"), TeamPC->JoinTeamTimeout,
		BattleMode->MaxSpectatorPeriod);
	TestEqual(TEXT("neutral join-nag timer re-armed"), NeutralPC->JoinTeamTimeout,
		BattleMode->MaxSpectatorPeriod);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelTeamJoinLeaveGhostSpec,
	"WoT.Citadel.TeamJoinLeaveGhost",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelTeamJoinLeaveGhostSpec::RunTest(const FString&)
{
	// giMPBattle.JoinTeam/LeaveTeam (giMPBattle.uc:253-302): a neutral player is a frozen,
	// overview-watching, non-blocking ghost with no inventory; joining during the edit
	// phase keeps the freeze+overview (no loadout); joining once play has begun un-freezes,
	// drops the overview, and re-arms immediately.
	UWorld* World = CitadelSpecCreateWorld(true);
	AWoTBattleGameMode* BattleMode = World->GetAuthGameMode<AWoTBattleGameMode>();

	AWoTCharacter* Pawn = CitadelSpecSpawnPawn(World, 255);
	AWoTPlayerController* PC = Cast<AWoTPlayerController>(Pawn->GetController());
	USkeletalMesh* TestMesh = Cast<USkeletalMesh>(FSoftObjectPath(
		TEXT("/Game/WoT/Meshes/AesSedai/AesSedai/SkeletalMeshes/"
			"AesSedai.AesSedai")).TryLoad());
	UMaterialInterface* ModulateMaster = LoadObject<UMaterialInterface>(nullptr,
		TEXT("/Game/WoT/Materials/M_WoT_MeshModulate.M_WoT_MeshModulate"));
	TestNotNull(TEXT("committed neutral-ghost modulate master loads"), ModulateMaster);
	TestTrue(TEXT("ghost spec fixture loads a faction mesh with materials"),
		IsValid(TestMesh) && TestMesh->GetMaterials().Num() > 0);
	if (!IsValid(ModulateMaster) || !IsValid(TestMesh) || TestMesh->GetMaterials().Num() == 0)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Pawn->GetMesh()->SetSkeletalMeshAsset(TestMesh);
	UMaterialInterface* OriginalMaterial = Pawn->GetMesh()->GetMaterial(0);
	if (!IsValid(OriginalMaterial))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	BattleMode->DisableTeleporters();
	const FProperty* NeutralGhostProperty = FindFProperty<FProperty>(
		AWoTCharacter::StaticClass(), TEXT("bNeutralGhost"));
	TestTrue(TEXT("neutral ghost state is a replicated property"),
		NeutralGhostProperty &&
		NeutralGhostProperty->HasAnyPropertyFlags(CPF_Net));

	// Login flow: a new joiner goes neutral (PostLogin -> ChangeTeam(255) -> LeaveTeam).
	BattleMode->ChangeTeam(PC, 255);
	TestTrue(TEXT("neutral is frozen"), PC->bTeleportingDisabled);
	TestTrue(TEXT("neutral watches the overview"), PC->bShowOverview);
	TestTrue(TEXT("LeaveTeam sets the replicated neutral ghost flag"),
		Pawn->IsNeutralGhost());
	TestTrue(TEXT("LeaveTeam swaps the mesh material to the ghost look"),
		IsValid(Pawn->GetMesh()->GetMaterial(0)) &&
		Pawn->GetMesh()->GetMaterial(0) != OriginalMaterial);
	TestTrue(TEXT("neutral ghost material is rooted at M_WoT_MeshModulate"),
		CitadelSpecMaterialParentChainContains(
			Pawn->GetMesh()->GetMaterial(0), ModulateMaster));
	TestEqual(TEXT("neutral is a non-blocking ghost"),
		CitadelSpecPawnResponse(Pawn), ECR_Overlap);

	// Join during the edit phase: still frozen, overview still up, no loadout yet.
	BattleMode->ChangeTeam(PC, 1);
	TestTrue(TEXT("edit-phase join keeps the freeze"), PC->bTeleportingDisabled);
	TestTrue(TEXT("edit-phase join keeps the overview"), PC->bShowOverview);
	TestFalse(TEXT("JoinTeam clears the replicated neutral ghost flag"),
		Pawn->IsNeutralGhost());
	TestTrue(TEXT("JoinTeam restores the exact original mesh material"),
		Pawn->GetMesh()->GetMaterial(0) == OriginalMaterial);
	TestEqual(TEXT("edit-phase join blocks pawns again"),
		CitadelSpecPawnResponse(Pawn), ECR_Block);
	TestEqual(TEXT("edit-phase join grants no loadout"), CitadelSpecInvCount(Pawn), 0);

	// Open play, then leave: back to the frozen overview ghost, inventory stripped
	// EXCEPT a carried seal (WOTPlayer.uc:4438 !Inv.IsA('Seal') — live since G13).
	BattleMode->EnableTeleporters();
	TestFalse(TEXT("play begun un-freezes the member"), PC->bTeleportingDisabled);
	TestEqual(TEXT("play begun re-arms the member"), CitadelSpecInvCount(Pawn), 4);
	AWoTSeal* CarriedSeal = nullptr;
	{
		FActorSpawnParameters SealParams;
		SealParams.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		CarriedSeal = World->SpawnActor<AWoTSeal>(
			Pawn->GetActorLocation() + FVector(0, 0, 500), FRotator::ZeroRotator,
			SealParams);
		TestTrue(TEXT("seal fixture granted"),
			CarriedSeal && CarriedSeal->TryPickupBy(Pawn));
	}
	BattleMode->ChangeTeam(PC, 255);
	TestTrue(TEXT("leaver is frozen"), PC->bTeleportingDisabled);
	TestTrue(TEXT("leaver watches the overview"), PC->bShowOverview);
	TestTrue(TEXT("mid-match LeaveTeam reapplies the neutral ghost look"),
		Pawn->IsNeutralGhost() &&
		Pawn->GetMesh()->GetMaterial(0) != OriginalMaterial);
	TestEqual(TEXT("leaver is a non-blocking ghost"),
		CitadelSpecPawnResponse(Pawn), ECR_Overlap);
	TestEqual(TEXT("leaver keeps ONLY the carried seal (LeaveCitadel)"),
		CitadelSpecInvCount(Pawn), 1);
	TestTrue(TEXT("the surviving item IS the seal"),
		IsValid(CarriedSeal) && CarriedSeal->GetOwner() == Pawn);
	TestEqual(TEXT("post-begin leaver gets the join nag"), PC->JoinTeamTimeout,
		BattleMode->MaxSpectatorPeriod);

	// Rejoin mid-battle: immediate un-freeze + re-arm, overview off; the seal
	// rides along (4 loadout + 1 seal).
	BattleMode->ChangeTeam(PC, 2);
	TestFalse(TEXT("mid-battle join un-freezes"), PC->bTeleportingDisabled);
	TestFalse(TEXT("mid-battle join drops the overview"), PC->bShowOverview);
	TestFalse(TEXT("mid-battle join restores normal style"),
		Pawn->IsNeutralGhost());
	TestTrue(TEXT("mid-battle join restores the original material"),
		Pawn->GetMesh()->GetMaterial(0) == OriginalMaterial);
	TestEqual(TEXT("mid-battle join blocks pawns"),
		CitadelSpecPawnResponse(Pawn), ECR_Block);
	TestEqual(TEXT("mid-battle join re-arms immediately (+ the kept seal)"),
		CitadelSpecInvCount(Pawn), 5);

	// UE1 keeps WOTPlayer across death; UE5 RestartPlayer replaces it. Leave again,
	// replace the pawn through the real spawn/defaults seam, and prove the persistent
	// neutral player state is re-derived while the controller-owned flags survive.
	BattleMode->ChangeTeam(PC, 255);
	TestTrue(TEXT("neutral respawn precondition has the overview up"),
		PC->bShowOverview);
	TestTrue(TEXT("neutral respawn precondition is frozen"),
		PC->bTeleportingDisabled);
	World->SpawnActor<APlayerStart>(FVector(2000, 0, 300),
		FRotator::ZeroRotator, CitadelSpecAlwaysSpawnParams());
	AWoTCharacter* ReplacedNeutralPawn = Pawn;
	BattleMode->RespawnDelay = 0.0f;
	BattleMode->PlayerKilled(nullptr, ReplacedNeutralPawn,
		FName(TEXT("NeutralRespawnSpec")));
	TestNull(TEXT("neutral death leaves the controller awaiting respawn"),
		PC->GetPawn());
	PC->Fire();
	AWoTCharacter* RespawnedNeutralPawn = Cast<AWoTCharacter>(PC->GetPawn());
	TestTrue(TEXT("neutral respawn creates and possesses a fresh pawn"),
		IsValid(RespawnedNeutralPawn) && RespawnedNeutralPawn != ReplacedNeutralPawn);
	if (IsValid(RespawnedNeutralPawn))
	{
		TestTrue(TEXT("fresh neutral pawn regains the ghost look state"),
			RespawnedNeutralPawn->IsNeutralGhost());
		TestEqual(TEXT("fresh neutral pawn remains non-blocking"),
			CitadelSpecPawnResponse(RespawnedNeutralPawn), ECR_Overlap);
	}
	TestTrue(TEXT("controller-owned teleport freeze survives pawn replacement"),
		PC->bTeleportingDisabled);
	TestTrue(TEXT("controller-owned overview survives pawn replacement"),
		PC->bShowOverview);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailCitadelEditInputSpec,
	"WoT.Battle.Citadel.RetailEditInput",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTRetailCitadelEditInputSpec::RunTest(const FString&)
{
	if (!FWoTRetailText::EnsureLoaded())
	{
		AddWarning(TEXT("retail localization is unavailable; skipping loaded edit refusal"));
		return true;
	}
	const FString* CantEditLevel = FWoTRetailText::FindSectionValue(
		TEXT("WOTPlayer"), TEXT("CantEditLevelStr"));
	if (!TestNotNull(TEXT("CantEditLevelStr is loaded"), CantEditLevel))
	{
		return false;
	}
	UWorld* BattleWorld = CitadelSpecCreateWorld(/*bBattleMode=*/true);
	AWoTBudgetInfo* Budget = BattleWorld->SpawnActor<AWoTBudgetInfo>(
		FVector::ZeroVector, FRotator::ZeroRotator, CitadelSpecAlwaysSpawnParams());
	Budget->Team = 1;
	CitadelSpecSetSealBudget(Budget, 1);
	CitadelSpecSpawnEditZone(BattleWorld, FVector::ZeroVector, FVector(100), 1);
	AWoTPlayerController* BattleController =
		CitadelSpecSpawnController(BattleWorld, 1, FVector::ZeroVector);
	TestTrue(TEXT("edit-phase Battle exposes the pause-menu action"),
		BattleController->CanRequestCitadelEdit());
	BattleController->ServerEditCitadel(/*Mode=*/1);
	TestTrue(TEXT("F10 semantics enter the existing Battle editor"),
		BattleController->IsEditingCitadel());
	TestEqual(TEXT("retail Mode reaches the existing orbit editor"),
		BattleController->GetCitadelEditCameraMode(),
		EWoTCitadelEditCameraMode::Orbit);
	BattleController->ServerEditCitadel();
	TestFalse(TEXT("F10 in CitadelEditor state exits like the retail override"),
		BattleController->IsEditingCitadel());
	WoTTest::DestroySpecWorld(BattleWorld);

	UWorld* ArenaWorld = CitadelSpecCreateWorld();
	AWoTTestSealNotifyController* ArenaController =
		ArenaWorld->SpawnActor<AWoTTestSealNotifyController>(
			CitadelSpecAlwaysSpawnParams());
	ArenaController->ServerEditCitadel();
	TestFalse(TEXT("Arena never enters the citadel editor"),
		ArenaController->IsEditingCitadel());
	TestTrue(TEXT("Arena refusal uses retail CantEditLevelStr"),
		ArenaController->CenterMessages.Contains(*CantEditLevel));
	TestFalse(TEXT("Arena omits the pause-menu citadel action"),
		ArenaController->CanRequestCitadelEdit());
	WoTTest::DestroySpecWorld(ArenaWorld);
	return !HasAnyErrors();
}

#endif
