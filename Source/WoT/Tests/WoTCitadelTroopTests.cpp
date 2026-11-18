// G15: citadel pawn adapters, deployment economy, session lifecycle, and Mashadar focus wiring.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../AI/WoTBrainComponent.h"
#include "../AI/WoTCreatureData.h"
#include "../AI/WoTNPCCharacter.h"
#include "../AI/WoTNPCController.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../Inventory/WoTSeal.h"
#include "../Inventory/WoTTroopInventories.h"
#include "../Projectiles/WoTMashadarGuide.h"
#include "../WoTBattleGameMode.h"
#include "../WoTBattleInfo.h"
#include "../WoTBudgetInfo.h"
#include "../WoTCharacter.h"
#include "../WoTCitadelResources.h"
#include "../WoTPlayerController.h"
#include "../WoTPlayerState.h"
#include "../WoTSealAltar.h"
#include "../WoTZoneVolume.h"
#include "WoTTestBattleClasses.h"

#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SceneComponent.h"
#include "Components/ShapeComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/WorldSettings.h"

namespace
{
	FActorSpawnParameters TroopSpecAlwaysSpawnParams()
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		return Params;
	}

	UWorld* TroopSpecCreateWorld()
	{
		UWorld* World = WoTTest::CreateSpecWorld(AWoTBattleGameMode::StaticClass());
		if (!World)
		{
			return nullptr;
		}

		AWoTBattleInfo* Battle = World->SpawnActorDeferred<AWoTBattleInfo>(
			AWoTBattleInfo::StaticClass(), FTransform::Identity);
		if (IsValid(Battle))
		{
			Battle->NumTeams = 3;
			Battle->TeamClass[0] = AWoTTestTeamAlpha::StaticClass();
			Battle->TeamClass[1] = AWoTTestTeamBravo::StaticClass();
			Battle->TeamClass[2] = AWoTTestTeamAlpha::StaticClass();
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

	AWoTZoneVolume* TroopSpecSpawnEditZone(UWorld* World, uint8 Team)
	{
		if (!IsValid(World))
		{
			return nullptr;
		}
		const FTransform Transform(FRotator::ZeroRotator, FVector::ZeroVector);
		AWoTZoneVolume* Zone = World->SpawnActorDeferred<AWoTZoneVolume>(
			AWoTZoneVolume::StaticClass(), Transform);
		if (!IsValid(Zone))
		{
			return nullptr;
		}
		Zone->EditTeam = Team;
		Zone->bAllowEditing = true;
		if (IsValid(Zone->Collision))
		{
			Zone->Collision->SetBoxExtent(FVector(2000.0f));
		}
		Zone->FinishSpawning(Transform);
		return Zone;
	}

	AWoTSealAltar* TroopSpecSpawnAltar(UWorld* World, uint8 Team,
		const FVector& Location)
	{
		if (!IsValid(World))
		{
			return nullptr;
		}
		const FTransform Transform(FRotator::ZeroRotator, Location);
		AWoTSealAltar* Altar = World->SpawnActorDeferred<AWoTSealAltar>(
			AWoTSealAltar::StaticClass(), Transform);
		if (!IsValid(Altar))
		{
			return nullptr;
		}
		Altar->Team = Team;
		Altar->FinishSpawning(Transform);
		return Altar;
	}

	void TroopSpecAddBudgetRow(AWoTBudgetInfo* Budget, FName Adapter, int32 Count)
	{
		if (!IsValid(Budget))
		{
			return;
		}
		FWoTBudgetResource& Row = Budget->Budget.AddDefaulted_GetRef();
		Row.ResourcePackageName = Adapter == TEXT("SealInventory")
			? FName(TEXT("WOT")) : FName(TEXT("WoTPawns"));
		Row.ResourceAdapterName = Adapter;
		Row.ItemCount = Count;
	}

	AWoTPlayerController* TroopSpecSpawnEditor(UWorld* World, uint8 Team,
		const FVector& Location, const FRotator& Rotation)
	{
		if (!IsValid(World))
		{
			return nullptr;
		}
		AWoTPlayerController* Controller = World->SpawnActor<AWoTPlayerController>(
			TroopSpecAlwaysSpawnParams());
		if (!IsValid(Controller))
		{
			return nullptr;
		}
		Controller->InitPlayerState();
		if (AWoTPlayerState* PS = Controller->GetPlayerState<AWoTPlayerState>())
		{
			PS->Team = Team;
		}
		AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
			Location, Rotation, TroopSpecAlwaysSpawnParams());
		if (!IsValid(Pawn))
		{
			return Controller;
		}
		Controller->Possess(Pawn);
		return Controller;
	}

	struct FTroopEditFixture
	{
		UWorld* World = nullptr;
		AWoTBattleGameMode* Mode = nullptr;
		AWoTBudgetInfo* Budget = nullptr;
		AWoTPlayerController* Controller = nullptr;
		AWoTCharacter* PlayerPawn = nullptr;

		bool Initialize(FName Adapter, int32 Count = 1)
		{
			World = TroopSpecCreateWorld();
			if (!IsValid(World))
			{
				return false;
			}
			Mode = World ? World->GetAuthGameMode<AWoTBattleGameMode>() : nullptr;
			AWoTSealAltar* TeamAltar =
				TroopSpecSpawnAltar(World, 1, FVector::ZeroVector);
			AWoTSealAltar* ForeignAltar =
				TroopSpecSpawnAltar(World, 2, FVector(5000, 0, 0));
			AWoTZoneVolume* EditZone = TroopSpecSpawnEditZone(World, 1);
			Budget = World->SpawnActor<AWoTBudgetInfo>(TroopSpecAlwaysSpawnParams());
			if (IsValid(Budget))
			{
				Budget->Team = 1;
				TroopSpecAddBudgetRow(Budget, TEXT("SealInventory"),
					Adapter == FName(TEXT("SealInventory")) ? Count : 0);
				if (Adapter != FName(TEXT("SealInventory")))
				{
					TroopSpecAddBudgetRow(Budget, Adapter, Count);
				}
			}
			Controller = TroopSpecSpawnEditor(World, 1, FVector(0, 0, 100),
				FRotator(20.0f, 0.0f, 0.0f));
			PlayerPawn = IsValid(Controller)
				? Cast<AWoTCharacter>(Controller->GetPawn()) : nullptr;
			if (IsValid(Controller))
			{
				Controller->CitadelBeginEdit();
			}
			return IsValid(World) && IsValid(Mode) && IsValid(Budget)
				&& IsValid(Controller) && IsValid(PlayerPawn)
				&& IsValid(TeamAltar) && IsValid(ForeignAltar)
				&& IsValid(EditZone)
				&& Controller->IsEditingCitadel();
		}

		~FTroopEditFixture()
		{
			WoTTest::DestroySpecWorld(World);
		}
	};

	AWoTNPCCharacter* TroopSpecFindCreature(UWorld* World, FName RowName)
	{
		for (TActorIterator<AWoTNPCCharacter> It(World); It; ++It)
		{
			if (IsValid(*It) && It->CreatureRowName == RowName)
			{
				return *It;
			}
		}
		return nullptr;
	}

	AWoTMashadarGuide* TroopSpecSpawnCitadelGuide(UWorld* World,
		const FVector& Location)
	{
		if (!IsValid(World))
		{
			return nullptr;
		}
		const FTransform Transform(FRotator::ZeroRotator, Location);
		AWoTMashadarGuide* Guide = World->SpawnActorDeferred<AWoTMashadarGuide>(
			AWoTMashadarGuide::StaticClass(), Transform);
		if (!IsValid(Guide))
		{
			return nullptr;
		}
		Guide->Team = 1;
		Guide->SearchRadius = 1600.0f;
		Guide->bCitadelPlaced = true;
		Guide->FinishSpawning(Transform);
		return IsValid(Guide) ? Guide : nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelTroopRegistrySpec,
	"WoT.Citadel.Troops.RegistryCensus",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelTroopRegistrySpec::RunTest(const FString&)
{
	struct FExpected
	{
		const TCHAR* Adapter;
		const TCHAR* Tier;
		const TCHAR* Row;
	};
	const FExpected Expected[] = {
		{TEXT("WarderInventory"), TEXT("Grunt"), TEXT("Warder")},
		{TEXT("SisterInventory"), TEXT("Captain"), TEXT("Sister")},
		{TEXT("SitterInventory"), TEXT("Champion"), TEXT("Sitter")},
		{TEXT("TrollocInventory"), TEXT("Grunt"), TEXT("TrollocStrong")},
		{TEXT("MyrddraalInventory"), TEXT("Captain"), TEXT("Myrddraal")},
		{TEXT("BATrollocInventory"), TEXT("Champion"), TEXT("BATrolloc")},
		{TEXT("MinionInventory"), TEXT("Grunt"), TEXT("Minion")},
		{TEXT("MashadarInventory"), TEXT("Captain"), TEXT("MashadarFocusPawn")},
		{TEXT("LegionInventory"), TEXT("Champion"), TEXT("Legion")},
		{TEXT("SoldierInventory"), TEXT("Grunt"), TEXT("Soldier")},
		{TEXT("ArcherInventory"), TEXT("Captain"), TEXT("Archer")},
		{TEXT("QuestionerInventory"), TEXT("Champion"), TEXT("Questioner")},
	};
	TestEqual(TEXT("twelve troop adapters pinned"),
		static_cast<int32>(UE_ARRAY_COUNT(Expected)), 12);
	for (const FExpected& Entry : Expected)
	{
		const FName Adapter(Entry.Adapter);
		const FWoTResourceDescriptor& Descriptor =
			WoTCitadelResources::FindByAdapterName(Adapter);
		TestTrue(*FString::Printf(TEXT("%s registered"), Entry.Adapter),
			Descriptor.IsValidDescriptor());
		TestEqual(*FString::Printf(TEXT("%s tier"), Entry.Adapter),
			Descriptor.BaseResourceType, FName(Entry.Tier));
		TestEqual(*FString::Printf(TEXT("%s row"), Entry.Adapter),
			Descriptor.CreatureRow, FName(Entry.Row));
		TestEqual(*FString::Printf(TEXT("%s row reverse lookup"), Entry.Adapter),
			WoTCitadelResources::FindAdapterForCreatureRow(Entry.Row), Adapter);
		TestTrue(*FString::Printf(TEXT("%s tier predicate"), Entry.Adapter),
			WoTCitadelResources::IsAdapterForTier(Adapter, Entry.Tier));
	}
	const FWoTResourceDescriptor& Seal =
		WoTCitadelResources::FindByAdapterName(TEXT("SealInventory"));
	TestTrue(TEXT("thirteenth pawn-side adapter is the seal card"),
		Seal.IsValidDescriptor());
	TestEqual(TEXT("seal card has no creature row"), Seal.CreatureRow, NAME_None);
	TestEqual(TEXT("seal card has no tier"), Seal.BaseResourceType, NAME_None);
	TestEqual(TEXT("seal card resource class"), Seal.ResourceClass,
		static_cast<UClass*>(AWoTSeal::StaticClass()));
	const AWoTSealInventory* SealCDO =
		AWoTSealInventory::StaticClass()->GetDefaultObject<AWoTSealInventory>();
	TestTrue(TEXT("seal adapter CDO exists"), IsValid(SealCDO));
	if (IsValid(SealCDO))
	{
		TestEqual(TEXT("seal adapter authored count"), SealCDO->Count, 1);
		TestTrue(TEXT("seal adapter is hidden"), SealCDO->IsHidden());
	}
	TestEqual(TEXT("TrollocStrong row resolves its retail adapter"),
		WoTCitadelResources::FindAdapterForCreatureRow(TEXT("TrollocStrong")),
		FName(TEXT("TrollocInventory")));
	TestEqual(TEXT("unknown row has no adapter"),
		WoTCitadelResources::FindAdapterForCreatureRow(TEXT("NoSuchCreature")), NAME_None);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelTroopTierLookupSpec,
	"WoT.Citadel.Troops.TierLookup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelTroopTierLookupSpec::RunTest(const FString&)
{
	UWorld* World = TroopSpecCreateWorld();
	if (!TestNotNull(TEXT("tier lookup world created"), World))
	{
		return true;
	}
	AWoTBudgetInfo* Budget = World->SpawnActor<AWoTBudgetInfo>(
		TroopSpecAlwaysSpawnParams());
	if (!TestNotNull(TEXT("tier lookup budget spawned"), Budget))
	{
		WoTTest::DestroySpecWorld(World);
		return true;
	}
	TroopSpecAddBudgetRow(Budget, TEXT("SealInventory"), 1);
	TroopSpecAddBudgetRow(Budget, TEXT("WarderInventory"), 2);
	TroopSpecAddBudgetRow(Budget, TEXT("SisterInventory"), 1);
	TroopSpecAddBudgetRow(Budget, TEXT("SitterInventory"), 1);
	TestEqual(TEXT("budget-order grunt"),
		Budget->GetResourceAdapterForTier(TEXT("Grunt")), FName(TEXT("WarderInventory")));
	TestEqual(TEXT("budget-order captain"),
		Budget->GetResourceAdapterForTier(TEXT("Captain")), FName(TEXT("SisterInventory")));
	TestEqual(TEXT("budget-order champion"),
		Budget->GetResourceAdapterForTier(TEXT("Champion")), FName(TEXT("SitterInventory")));
	TestEqual(TEXT("none tier guard"),
		Budget->GetResourceAdapterForTier(NAME_None), NAME_None);

	AWoTPlayerController* Controller = World->SpawnActor<AWoTPlayerController>(
		TroopSpecAlwaysSpawnParams());
	if (!TestNotNull(TEXT("tier lookup controller spawned"), Controller))
	{
		WoTTest::DestroySpecWorld(World);
		return true;
	}
	for (int32 Index = 0; Index < Budget->GetArrayCount(); ++Index)
	{
		FWoTCitadelBudgetCount& Mirror = Controller->OwnerEditBudget.AddDefaulted_GetRef();
		Mirror.AdapterName = Budget->GetResourceAdapterName(Index);
		Mirror.ItemCount = Budget->GetItemCount(Index);
	}
	TestEqual(TEXT("owner mirror uses the same budget-order rule"),
		Controller->GetOwnerCitadelTroopAdapter(TEXT("Captain")),
		Budget->GetResourceAdapterForTier(TEXT("Captain")));
	TestEqual(TEXT("owner mirror none tier guard"),
		Controller->GetOwnerCitadelTroopAdapter(NAME_None), NAME_None);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelTroopCreateSpec,
	"WoT.Citadel.Troops.CreatePlacementTeamDormantSpend",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelTroopCreateSpec::RunTest(const FString&)
{
	FTroopEditFixture Fixture;
	if (!TestTrue(TEXT("troop edit fixture initialized"),
		Fixture.Initialize(TEXT("WarderInventory"))))
	{
		return true;
	}
	const FWoTCreatureRow* WarderRow = WoTCreatureData::Find(TEXT("Warder"));
	const float PlayerRadius = Fixture.PlayerPawn->GetCapsuleComponent()
		? Fixture.PlayerPawn->GetCapsuleComponent()->GetScaledCapsuleRadius() : 0.0f;
	const FVector ExpectedSpawn = Fixture.PlayerPawn->GetActorLocation()
		+ FVector::ForwardVector * (PlayerRadius
			+ (WarderRow ? WarderRow->CollisionRadius : 0.0f) + 50.0f)
		+ FVector(0, 0, 16.0f);
	Fixture.Controller->ServerCreateResource(
		TEXT("WarderInventory"), FVector(1900, 1900, 1900), FVector::UpVector);

	AWoTNPCCharacter* Warder = TroopSpecFindCreature(Fixture.World, TEXT("Warder"));
	if (TestNotNull(TEXT("warder spawned"), Warder))
	{
		TestTrue(TEXT("cursor hit ignored; pawn uses flattened player-front placement"),
			Warder->GetActorLocation().Equals(ExpectedSpawn, 0.25f));
		TestEqual(TEXT("placed troop team"), static_cast<int32>(Warder->AssignedTeam), 1);
		AWoTNPCController* NPCController = Cast<AWoTNPCController>(Warder->GetController());
		TestTrue(TEXT("placed troop has dormant controller"),
			IsValid(NPCController) && NPCController->IsDormant());
		TestTrue(TEXT("battle registry owns the placed troop"),
			Fixture.Mode->IsCitadelDeployedTroop(Warder, 1));
		const FWoTCitadelSelectableResource Selection =
			WoTCitadelResources::QuerySelectableResource(
				Warder, TEXT("WarderInventory"), 1);
		TestTrue(TEXT("deployed troop is selectable by its creature-row adapter"),
			Selection.IsSelectable());
		TestEqual(TEXT("deployed troop selectable state is active"),
			static_cast<int32>(Selection.State),
			static_cast<int32>(EWoTCitadelSelectableState::Active));
	}
	TestEqual(TEXT("successful troop create decrements budget"),
		Fixture.Budget->GetItemCount(
			Fixture.Budget->GetIndex(TEXT("WarderInventory"))), 0);
	TestEqual(TEXT("owner mirror decremented with authority budget"),
		Fixture.Controller->GetOwnerCitadelBudgetCount(TEXT("WarderInventory")), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelSealCardNoOpSpec,
	"WoT.Citadel.Troops.SealCardCreateNoOp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelSealCardNoOpSpec::RunTest(const FString&)
{
	FTroopEditFixture Fixture;
	if (!TestTrue(TEXT("seal edit fixture initialized"),
		Fixture.Initialize(TEXT("SealInventory"))))
	{
		return true;
	}
	Fixture.Controller->ServerCreateResource(
		TEXT("SealInventory"), FVector::ZeroVector, FVector::UpVector);
	int32 SealCount = 0;
	for (TActorIterator<AWoTSeal> It(Fixture.World); It; ++It)
	{
		if (IsValid(*It))
		{
			++SealCount;
		}
	}
	TestEqual(TEXT("seal adapter create leg spawns nothing"), SealCount, 0);
	TestEqual(TEXT("seal adapter create leg spends nothing"),
		Fixture.Budget->GetItemCount(
			Fixture.Budget->GetIndex(TEXT("SealInventory"))), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelTroopRemoveHardeningSpec,
	"WoT.Citadel.Troops.RemoveRefundAndHardening",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelTroopRemoveHardeningSpec::RunTest(const FString&)
{
	FTroopEditFixture Fixture;
	if (!TestTrue(TEXT("remove fixture initialized"),
		Fixture.Initialize(TEXT("WarderInventory"), 2)))
	{
		return true;
	}
	Fixture.Controller->ServerCreateResource(
		TEXT("WarderInventory"), FVector::ZeroVector, FVector::UpVector);
	AWoTNPCCharacter* Placed = TroopSpecFindCreature(Fixture.World, TEXT("Warder"));
	if (TestNotNull(TEXT("removable deployed troop spawned"), Placed))
	{
		Fixture.Controller->ServerRemoveResource(Placed);
		TestFalse(TEXT("deployed troop removed"), IsValid(Placed));
		TestEqual(TEXT("deployed troop refunded"), Fixture.Budget->GetItemCount(
			Fixture.Budget->GetIndex(TEXT("WarderInventory"))), 2);
	}

	AWoTNPCCharacter* NonDeployed = WoTCreatureData::SpawnCreature(
		*Fixture.World, TEXT("Warder"), FVector(200, 0, 100), 1, false);
	if (TestNotNull(TEXT("non-deployed fixture spawned"), NonDeployed))
	{
		Fixture.Controller->ServerRemoveResource(NonDeployed);
		TestTrue(TEXT("summon/map NPC is not editor-removable"), IsValid(NonDeployed));
		TestEqual(TEXT("non-deployed NPC is not refunded"), Fixture.Budget->GetItemCount(
			Fixture.Budget->GetIndex(TEXT("WarderInventory"))), 2);
	}

	AWoTNPCCharacter* Foreign = WoTCreatureData::SpawnCreature(
		*Fixture.World, TEXT("Warder"), FVector(300, 0, 100), 2, false);
	if (TestNotNull(TEXT("foreign deployed fixture spawned"), Foreign))
	{
		Fixture.Mode->RegisterCitadelDeployedTroop(Foreign, 2);
		Fixture.Controller->ServerRemoveResource(Foreign);
		TestTrue(TEXT("foreign-team deployed troop is not removed"), IsValid(Foreign));
		TestEqual(TEXT("foreign-team deployed troop is not refunded"),
			Fixture.Budget->GetItemCount(
				Fixture.Budget->GetIndex(TEXT("WarderInventory"))), 2);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelTroopSessionSpec,
	"WoT.Citadel.Troops.SessionWakeAndRefreeze",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelTroopSessionSpec::RunTest(const FString&)
{
	FTroopEditFixture Fixture;
	if (!TestTrue(TEXT("session fixture initialized"),
		Fixture.Initialize(TEXT("WarderInventory"))))
	{
		return true;
	}
	Fixture.Controller->ServerCreateResource(
		TEXT("WarderInventory"), FVector::ZeroVector, FVector::UpVector);
	AWoTNPCCharacter* Warder = TroopSpecFindCreature(Fixture.World, TEXT("Warder"));
	AWoTNPCController* NPCController = IsValid(Warder)
		? Cast<AWoTNPCController>(Warder->GetController()) : nullptr;
	if (!TestNotNull(TEXT("session troop spawned"), NPCController))
	{
		return true;
	}
	// G01 (WOTPlayer.uc:5146-5155): bProhibitEditorExit refuses the voluntary exit
	// and the session survives; clearing it lets the exit proceed.
	Fixture.Controller->bProhibitEditorExit = true;
	Fixture.Controller->CitadelEndEdit();
	TestTrue(TEXT("prohibited exit keeps the edit session"),
		Fixture.Controller->IsEditingCitadel());
	TestTrue(TEXT("prohibited exit keeps deployed troop frozen"),
		NPCController->IsDormant());
	Fixture.Controller->bProhibitEditorExit = false;
	Fixture.Controller->CitadelEndEdit();
	TestFalse(TEXT("cleared prohibition allows the exit"),
		Fixture.Controller->IsEditingCitadel());
	TestFalse(TEXT("session end wakes deployed troop"), NPCController->IsDormant());
	Fixture.Controller->CitadelBeginEdit();
	TestTrue(TEXT("re-edit freezes deployed troop again"), NPCController->IsDormant());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelMashadarFocusSpec,
	"WoT.Citadel.Troops.MashadarBindDeployRemoveAndSession",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelMashadarFocusSpec::RunTest(const FString&)
{
	FTroopEditFixture Fixture;
	if (!TestTrue(TEXT("mashadar fixture initialized"),
		Fixture.Initialize(TEXT("MashadarInventory"), 2)))
	{
		return true;
	}
	AWoTMashadarGuide* Guide = TroopSpecSpawnCitadelGuide(
		Fixture.World, FVector(200, 0, 100));
	if (!TestNotNull(TEXT("citadel guide spawned"), Guide))
	{
		return true;
	}
	Fixture.Controller->ServerCreateResource(
		TEXT("MashadarInventory"), FVector(1900, 0, 100), FVector::UpVector);
	AWoTNPCCharacter* Focus = TroopSpecFindCreature(
		Fixture.World, TEXT("MashadarFocusPawn"));
	if (TestNotNull(TEXT("focus pawn spawned"), Focus))
	{
		TestTrue(TEXT("focus binds nearest eligible guide"),
			Guide->GetFocusPawn() == Focus && Guide->FollowingFocusPawn());
		TestTrue(TEXT("focus activation deploys guide"), Guide->IsDeployed());
		TestFalse(TEXT("editing focus marker is visible"), Focus->IsHidden());

		Fixture.Controller->CitadelEndEdit();
		TestFalse(TEXT("session end unbinds guide"), Guide->FollowingFocusPawn());
		TestTrue(TEXT("session end hides editor-only focus marker"), Focus->IsHidden());
		// MashadarGuide.uc:1941 (bCollideWorld=False for life): the unbind retract must
		// not re-block world statics or a crack-parked guide can never get home.
		if (UShapeComponent* GuideShape =
			Guide->FindComponentByClass<UShapeComponent>())
		{
			TestEqual(TEXT("unbound guide keeps gliding through world statics"),
				static_cast<int32>(
					GuideShape->GetCollisionResponseToChannel(ECC_WorldStatic)),
				static_cast<int32>(ECR_Ignore));
		}
		// IsChasable (MashadarGuide.uc:1235): a FRIENDLY sighting must not assimilate
		// the deployed guide or take its destination.
		AWoTNPCCharacter* Friendly = WoTCreatureData::SpawnCreature(
			*Fixture.World, TEXT("Trolloc"), Guide->GetActorLocation(), 1, false);
		if (TestNotNull(TEXT("Mashadar friendly fixture spawned"), Friendly))
		{
			Guide->NotifySeePawn(Friendly);
			TestTrue(TEXT("friendly sighting does not assimilate the deployed guide"),
				Guide->IsDeployed());
		}
		AWoTNPCCharacter* Intruder = WoTCreatureData::SpawnCreature(
			*Fixture.World, TEXT("Warder"), Guide->GetActorLocation(), 2, false);
		if (TestNotNull(TEXT("Mashadar intruder fixture spawned"), Intruder))
		{
			Guide->NotifySeePawn(Intruder);
			TestFalse(TEXT("first live chase assimilates the deployed guide"),
				Guide->IsDeployed());
		}

		Fixture.Controller->CitadelBeginEdit();
		TestTrue(TEXT("re-edit rebinds the retained guide"), Guide->GetFocusPawn() == Focus);
		TestFalse(TEXT("re-edit shows focus marker"), Focus->IsHidden());
		Guide->Explode(Guide->GetActorLocation(), FVector::UpVector);
		TestTrue(TEXT("reaching a focus does not destroy the persistent guide"),
			IsValid(Guide) && Guide->FollowingFocusPawn());
		Guide->GuardLocation(FVector::ZeroVector);
		Fixture.Controller->ServerRemoveResource(Focus);
		TestFalse(TEXT("focus remove destroys marker"), IsValid(Focus));
		TestFalse(TEXT("focus remove unbinds guide"), Guide->FollowingFocusPawn());
		TestEqual(TEXT("focus remove unguards guide"),
			static_cast<int32>(Guide->GuardMode),
			static_cast<int32>(EWoTMashadarGuard::Free));
		TestEqual(TEXT("focus remove refunds budget"), Fixture.Budget->GetItemCount(
			Fixture.Budget->GetIndex(TEXT("MashadarInventory"))), 2);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelMashadarDeployFailureSpec,
	"WoT.Citadel.Troops.MashadarFailNoSpend",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelMashadarDeployFailureSpec::RunTest(const FString&)
{
	FTroopEditFixture Fixture;
	if (!TestTrue(TEXT("mashadar failure fixture initialized"),
		Fixture.Initialize(TEXT("MashadarInventory"))))
	{
		return true;
	}
	Fixture.Controller->ServerCreateResource(
		TEXT("MashadarInventory"), FVector::ZeroVector, FVector::UpVector);
	TestNull(TEXT("focus without eligible guide is destroyed"),
		TroopSpecFindCreature(Fixture.World, TEXT("MashadarFocusPawn")));
	TestEqual(TEXT("failed focus deploy does not spend"), Fixture.Budget->GetItemCount(
		Fixture.Budget->GetIndex(TEXT("MashadarInventory"))), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelSealInventorySelectableSpec,
	"WoT.Citadel.Troops.D19SealInventorySelectable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelSealInventorySelectableSpec::RunTest(const FString&)
{
	UWorld* World = TroopSpecCreateWorld();
	if (!TestNotNull(TEXT("seal selection world created"), World))
	{
		return true;
	}
	AActor* Owner = World->SpawnActor<AActor>(TroopSpecAlwaysSpawnParams());
	if (!TestNotNull(TEXT("seal selection owner spawned"), Owner))
	{
		WoTTest::DestroySpecWorld(World);
		return true;
	}
	UWoTInventoryComponent* Inventory = NewObject<UWoTInventoryComponent>(Owner);
	if (!TestNotNull(TEXT("seal selection inventory created"), Inventory))
	{
		WoTTest::DestroySpecWorld(World);
		return true;
	}
	Inventory->RegisterComponent();
	AWoTSealInventory* SealCard = World->SpawnActor<AWoTSealInventory>(
		TroopSpecAlwaysSpawnParams());
	if (!TestNotNull(TEXT("seal adapter spawned"), SealCard))
	{
		WoTTest::DestroySpecWorld(World);
		return true;
	}
	TestTrue(TEXT("seal adapter enters inventory"), Inventory->AddInventory(SealCard));
	TestEqual(TEXT("SealInventory adapter class name is selectable"),
		Inventory->SelectedItem.Get(), static_cast<AWoTInventory*>(SealCard));
	TestEqual(TEXT("class-name normalization pins SealInventory"),
		UWoTInventoryComponent::GetHandSetItemName(SealCard),
		FName(TEXT("SealInventory")));
	WoTTest::DestroySpecWorld(World);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
