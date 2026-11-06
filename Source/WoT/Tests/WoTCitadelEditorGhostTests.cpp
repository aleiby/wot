// Owner-local transient resource ghost lifecycle, placement agreement, and zero-sim.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../Inventory/WoTTrapDetectLeech.h"
#include "../WoTBattleGameMode.h"
#include "../WoTBattleInfo.h"
#include "../WoTBudgetInfo.h"
#include "../WoTCharacter.h"
#include "../WoTCitadelResources.h"
#include "../WoTMover.h"
#include "../WoTPlayerController.h"
#include "../WoTPlayerState.h"
#include "../WoTParticleSprayer.h"
#include "../WoTSealAltar.h"
#include "../WoTZoneVolume.h"
#include "../Traps/WoTAlarm.h"
#include "../Traps/WoTFireWall.h"
#include "../Traps/WoTPit.h"
#include "../Traps/WoTPortcullis.h"
#include "../Traps/WoTSpear.h"
#include "../Traps/WoTStaircase.h"
#include "../Traps/WoTWall.h"

#include "Components/BoxComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/Engine.h"
#include "Engine/DamageEvents.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/WorldSettings.h"

namespace
{
	FActorSpawnParameters EditorGhostSpecAlwaysSpawnParams()
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		return Params;
	}

	UWorld* EditorGhostSpecCreateWorld(bool bServerCreateWorld = false)
	{
		UWorld* World = bServerCreateWorld
			? WoTTest::CreateSpecWorld(AWoTBattleGameMode::StaticClass())
			: WoTTest::CreateSpecWorld();
		if (bServerCreateWorld)
		{
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
		}
		return World;
	}

	AActor* EditorGhostSpecSpawnWorldBox(
		UWorld* World, const FVector& Center, const FVector& Extent)
	{
		AActor* BoxActor = World->SpawnActor<AActor>(
			Center, FRotator::ZeroRotator, EditorGhostSpecAlwaysSpawnParams());
		if (!IsValid(BoxActor))
		{
			return nullptr;
		}
		UBoxComponent* Box = NewObject<UBoxComponent>(BoxActor);
		Box->SetBoxExtent(Extent);
		Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Box->SetCollisionObjectType(ECC_WorldStatic);
		Box->SetCollisionResponseToAllChannels(ECR_Block);
		BoxActor->SetRootComponent(Box);
		Box->RegisterComponent();
		BoxActor->SetActorLocation(Center);
		return BoxActor;
	}

	AWoTZoneVolume* EditorGhostSpecSpawnZone(UWorld* World, bool bAllowEditing = true)
	{
		const FTransform Transform(FRotator::ZeroRotator, FVector::ZeroVector);
		AWoTZoneVolume* Zone = World->SpawnActorDeferred<AWoTZoneVolume>(
			AWoTZoneVolume::StaticClass(), Transform);
		if (!IsValid(Zone))
		{
			return nullptr;
		}
		Zone->EditTeam = 1;
		Zone->bAllowEditing = bAllowEditing;
		Zone->Collision->SetBoxExtent(FVector(2000.0f));
		Zone->FinishSpawning(Transform);
		return Zone;
	}

	AWoTSealAltar* EditorGhostSpecSpawnTeamAltar(UWorld* World, uint8 Team = 1)
	{
		const FTransform Transform(FRotator::ZeroRotator, FVector::ZeroVector);
		AWoTSealAltar* Altar = World->SpawnActorDeferred<AWoTSealAltar>(
			AWoTSealAltar::StaticClass(), Transform);
		if (IsValid(Altar))
		{
			Altar->Team = Team;
			Altar->FinishSpawning(Transform);
		}
		return Altar;
	}

	AWoTPlayerController* EditorGhostSpecSpawnController(UWorld* World)
	{
		AWoTPlayerController* Controller = World->SpawnActor<AWoTPlayerController>(
			EditorGhostSpecAlwaysSpawnParams());
		if (!IsValid(Controller))
		{
			return nullptr;
		}
		Controller->SetAsLocalPlayerController();
		if (!Controller->PlayerState)
		{
			Controller->InitPlayerState();
		}
		if (AWoTPlayerState* State =
			Controller->GetPlayerState<AWoTPlayerState>())
		{
			State->Team = 1;
		}
		USceneComponent* ViewerRoot = Controller->GetRootComponent();
		if (!ViewerRoot)
		{
			ViewerRoot = NewObject<USceneComponent>(
				Controller, TEXT("EditorGhostSpecViewerRoot"));
			Controller->SetRootComponent(ViewerRoot);
			ViewerRoot->RegisterComponent();
		}
		ViewerRoot->SetWorldLocation(FVector::ZeroVector);
		Controller->OwnerEditSessionTeam = 1;
		return Controller;
	}

	void EditorGhostSpecSetCount(
		AWoTPlayerController* Controller, FName AdapterName, int32 ItemCount)
	{
		for (FWoTCitadelBudgetCount& Entry : Controller->OwnerEditBudget)
		{
			if (Entry.AdapterName == AdapterName)
			{
				Entry.ItemCount = ItemCount;
				return;
			}
		}
		FWoTCitadelBudgetCount& Entry =
			Controller->OwnerEditBudget.AddDefaulted_GetRef();
		Entry.AdapterName = AdapterName;
		Entry.ItemCount = ItemCount;
	}

	AWoTBudgetInfo* EditorGhostSpecGrantServerCreate(
		UWorld* World, AWoTPlayerController* Controller,
		FName AdapterName, int32 ItemCount)
	{
		AWoTBudgetInfo* Budget = World->SpawnActor<AWoTBudgetInfo>(
			EditorGhostSpecAlwaysSpawnParams());
		if (!IsValid(Budget) || !IsValid(Controller))
		{
			return nullptr;
		}
		FWoTBudgetResource Row;
		Row.ResourceAdapterName = AdapterName;
		Row.ItemCount = ItemCount;
		Budget->Team = 1;
		Budget->Budget.Add(Row);
		// Balance the edit-session count the way CitadelBeginEdit does, so the teardown
		// EndEditSession -> Budget->EndEdit does not trip the PlayerEditCount>0 ensure.
		Budget->BeginEdit(Controller);
		Controller->EditSessionBudget = Budget;
		Controller->EditSessionTeam = 1;
		return Budget;
	}

	AWoTTrap* EditorGhostSpecFindCreatedResource(
		UWorld* World, UClass* ResourceClass)
	{
		for (TActorIterator<AWoTTrap> It(World); It; ++It)
		{
			if (IsValid(*It) && (*It)->IsA(ResourceClass) && !(*It)->IsPreviewMode())
			{
				return *It;
			}
		}
		return nullptr;
	}

	FWoTCitadelEditorTraceResult EditorGhostSpecWorldHit(
		const FVector& Location, const FVector& Normal)
	{
		FWoTCitadelEditorTraceResult Hit;
		Hit.HitLocation = Location;
		Hit.HitNormal = Normal;
		Hit.bBlockingHit = true;
		Hit.bHitWorldGeometry = true;
		return Hit;
	}

	void EditorGhostSpecSpawnPlacementFixture(UWorld* World, FName AdapterName)
	{
		if (AdapterName == TEXT("PitInventory"))
		{
			World->SpawnActor<AWoTPitOpening>(FVector(1500, 1500, -200),
				FRotator::ZeroRotator, EditorGhostSpecAlwaysSpawnParams());
		}
		if (AdapterName == TEXT("AlarmInventory"))
		{
			EditorGhostSpecSpawnWorldBox(
				World, FVector(200, 0, 0), FVector(500, 500, 10));
			EditorGhostSpecSpawnWorldBox(
				World, FVector(200, 0, 100), FVector(10, 100, 200));
			return;
		}
		EditorGhostSpecSpawnWorldBox(
			World, FVector::ZeroVector, FVector(1000, 1000, 10));
		if (AdapterName == TEXT("WallInventory")
			|| AdapterName == TEXT("FireWallInventory"))
		{
			EditorGhostSpecSpawnWorldBox(
				World, FVector(600, 0, 100), FVector(10, 100, 200));
		}
	}

	void EditorGhostSpecAim(FName AdapterName,
		FVector& OutValidLocation, FVector& OutValidNormal,
		FVector& OutInvalidLocation, FVector& OutInvalidNormal)
	{
		OutInvalidLocation = FVector(9000, 9000, 200);
		OutInvalidNormal = FVector::UpVector;
		if (AdapterName == TEXT("AlarmInventory"))
		{
			OutValidLocation = FVector(189.5f, 0, 200);
			OutValidNormal = FVector(-1, 0, 0);
			return;
		}
		if (AdapterName == TEXT("WallInventory")
			|| AdapterName == TEXT("FireWallInventory"))
		{
			OutValidLocation = FVector(0, 0, 50);
			OutValidNormal = FVector(1, 0, 0);
			return;
		}
		if (AdapterName == TEXT("PitInventory"))
		{
			OutValidLocation = FVector(0, 0, 10);
			OutValidNormal = FVector::UpVector;
			OutInvalidNormal = FVector::ForwardVector;
			return;
		}
		OutValidLocation = FVector(0, 0, 200);
		OutValidNormal = FVector::UpVector;
	}

	bool EditorGhostSpecAllPrimitivesAreLocalAndInert(AActor* Actor)
	{
		if (!IsValid(Actor) || Actor->GetActorEnableCollision())
		{
			return false;
		}
		TArray<UPrimitiveComponent*> Primitives;
		Actor->GetComponents<UPrimitiveComponent>(Primitives);
		for (UPrimitiveComponent* Primitive : Primitives)
		{
			if (!IsValid(Primitive) || !Primitive->bOnlyOwnerSee
				|| Primitive->GetGenerateOverlapEvents()
				|| Primitive->GetCollisionEnabled() != ECollisionEnabled::NoCollision)
			{
				return false;
			}
		}
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelEditorGhostLifecycleSpec,
	"WoT.Citadel.EditorGhost.Lifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelEditorGhostLifecycleSpec::RunTest(const FString&)
{
	UWorld* World = EditorGhostSpecCreateWorld();
	AWoTZoneVolume* Zone = EditorGhostSpecSpawnZone(World);
	EditorGhostSpecSpawnTeamAltar(World);
	AWoTPlayerController* Controller = EditorGhostSpecSpawnController(World);
	if (!IsValid(Zone) || !IsValid(Controller))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	EditorGhostSpecSetCount(Controller, TEXT("AlarmInventory"), 1);
	Controller->SetCitadelPaletteSelection(TEXT("AlarmInventory"));
	Controller->ReconcileCitadelGhostPreview();
	AWoTTrap* FirstGhost = Controller->GetCitadelGhostPreview();
	TestNotNull(TEXT("spawn-temp selection with count and valid zone creates ghost"), FirstGhost);
	TestTrue(TEXT("ghost class follows selection"), FirstGhost && FirstGhost->IsA<AWoTAlarm>());

	EditorGhostSpecSetCount(Controller, TEXT("SpearInventory"), 1);
	Controller->SetCitadelPaletteSelection(TEXT("SpearInventory"));
	Controller->ReconcileCitadelGhostPreview();
	AWoTTrap* SecondGhost = Controller->GetCitadelGhostPreview();
	TestTrue(TEXT("class change recreates ghost"),
		IsValid(SecondGhost) && SecondGhost != FirstGhost && SecondGhost->IsA<AWoTSpear>());

	EditorGhostSpecSetCount(Controller, TEXT("SpearInventory"), 0);
	Controller->ReconcileCitadelGhostPreview();
	TestNull(TEXT("zero count destroys spawn ghost"), Controller->GetCitadelGhostPreview());

	EditorGhostSpecSetCount(Controller, TEXT("PortcullisInventory"), 2);
	Controller->SetCitadelPaletteSelection(TEXT("PortcullisInventory"));
	Controller->ReconcileCitadelGhostPreview();
	TestNull(TEXT("Portcullis toggle selection never creates ghost"),
		Controller->GetCitadelGhostPreview());
	EditorGhostSpecSetCount(Controller, TEXT("StaircaseInventory"), 2);
	Controller->SetCitadelPaletteSelection(TEXT("StaircaseInventory"));
	Controller->ReconcileCitadelGhostPreview();
	TestNull(TEXT("Staircase toggle selection never creates ghost"),
		Controller->GetCitadelGhostPreview());

	EditorGhostSpecSetCount(Controller, TEXT("AlarmInventory"), 1);
	Controller->SetCitadelPaletteSelection(TEXT("AlarmInventory"));
	Controller->ReconcileCitadelGhostPreview();
	Zone->bAllowEditing = false;
	Controller->ReconcileCitadelGhostPreview();
	TestNull(TEXT("invalid edit zone destroys ghost"), Controller->GetCitadelGhostPreview());
	Zone->bAllowEditing = true;
	Zone->EditTeam = 2;
	Controller->ReconcileCitadelGhostPreview();
	TestNotNull(TEXT("allowed create zone need not match the edit-session team"),
		Controller->GetCitadelGhostPreview());
	Controller->SetCitadelPaletteSelection(NAME_None);
	Controller->ReconcileCitadelGhostPreview();
	TestNull(TEXT("no selection keeps ghost destroyed"), Controller->GetCitadelGhostPreview());

	Controller->SetCitadelPaletteSelection(TEXT("AlarmInventory"));
	Controller->ReconcileCitadelGhostPreview();
	Controller->OwnerEditSessionTeam = 255;
	Controller->OnRep_OwnerEditSessionTeam();
	TestNull(TEXT("edit exit destroys ghost"), Controller->GetCitadelGhostPreview());
	WoTTest::TickWorld(World, 1);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelEditorGhostAgreementSpec,
	"WoT.Citadel.EditorGhost.PlacementAgreementAllResources",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelEditorGhostAgreementSpec::RunTest(const FString&)
{
	static const FName SpawnAdapters[] = {
		TEXT("AlarmInventory"), TEXT("SpearInventory"), TEXT("WallInventory"),
		TEXT("FireWallInventory"), TEXT("PitInventory")};
	for (FName AdapterName : SpawnAdapters)
	{
		UWorld* World = EditorGhostSpecCreateWorld(true);
		AWoTZoneVolume* Zone = EditorGhostSpecSpawnZone(World);
		AWoTSealAltar* Altar = EditorGhostSpecSpawnTeamAltar(World, 2);
		EditorGhostSpecSpawnPlacementFixture(World, AdapterName);
		AWoTPlayerController* Controller = EditorGhostSpecSpawnController(World);
		AWoTBudgetInfo* Budget = EditorGhostSpecGrantServerCreate(
			World, Controller, AdapterName, 1);
		if (!IsValid(Zone) || !IsValid(Altar) || !IsValid(Controller)
			|| !IsValid(Budget))
		{
			AddError(FString::Printf(TEXT("%s server-create fixture failed"),
				*AdapterName.ToString()));
			WoTTest::DestroySpecWorld(World);
			continue;
		}
		Zone->EditTeam = 2; // The create RPC intentionally does not require a team match.
		EditorGhostSpecSetCount(Controller, AdapterName, 1);
		Controller->SetCitadelPaletteSelection(AdapterName);
		Controller->ReconcileCitadelGhostPreview();
		AWoTTrap* Ghost = Controller->GetCitadelGhostPreview();
		const FWoTResourceDescriptor& Descriptor =
			WoTCitadelResources::FindByAdapterName(AdapterName);
		TestNotNull(FString::Printf(TEXT("%s ghost exists"), *AdapterName.ToString()), Ghost);
		if (Ghost == nullptr)
		{
			// Without this, a missing ghost SEGVs the editor at the transform
			// read below and takes the whole suite run with it (as when the
			// dormancy gate leaves the preview subsystem inert): a spec
			// must fail, never crash the harness.
			return false;
		}

		FVector ValidLocation;
		FVector ValidNormal;
		FVector InvalidLocation;
		FVector InvalidNormal;
		EditorGhostSpecAim(AdapterName, ValidLocation, ValidNormal,
			InvalidLocation, InvalidNormal);

		// Keep placement geometry valid and vary only altar ownership. Both preview and the
		// complete server-create gate must reject the foreign altar without spending budget.
		Controller->SyncCitadelGhostPreview(
			EditorGhostSpecWorldHit(ValidLocation, ValidNormal));
		TestFalse(FString::Printf(TEXT("%s preview rejects foreign altar only"),
			*AdapterName.ToString()), Controller->IsCitadelGhostPlacementValid());
		Controller->ServerCreateResource(AdapterName, ValidLocation, ValidNormal);
		const int32 BudgetIndex = Budget->GetIndex(AdapterName);
		TestEqual(FString::Printf(TEXT("%s server rejects the same foreign altar"),
			*AdapterName.ToString()), Budget->GetItemCount(BudgetIndex), 1);
		TestNull(FString::Printf(TEXT("%s foreign-altar create spawns nothing"),
			*AdapterName.ToString()),
			EditorGhostSpecFindCreatedResource(World, Descriptor.ResourceClass));

		Altar->Team = 1;
		Controller->SyncCitadelGhostPreview(
			EditorGhostSpecWorldHit(ValidLocation, ValidNormal));
		const bool bPreviewAccepted = Controller->IsCitadelGhostPlacementValid();
		const FTransform KeptPreviewTransform = Ghost->GetActorTransform();
		FWoTCitadelEditorTraceResult OwnLogicalHit;
		OwnLogicalHit.HitActor = Ghost;
		OwnLogicalHit.bBlockingHit = true;
		Controller->SyncCitadelGhostPreview(OwnLogicalHit);
		TestEqual(FString::Printf(TEXT("%s own logical hit keeps validity"),
			*AdapterName.ToString()), Controller->IsCitadelGhostPlacementValid(),
			bPreviewAccepted);
		TestTrue(FString::Printf(TEXT("%s own logical hit keeps transform"),
			*AdapterName.ToString()),
			Ghost->GetActorTransform().Equals(KeptPreviewTransform, 0.1f));
		if (AWoTPit* PreviewPit = Cast<AWoTPit>(Ghost))
		{
			for (TActorIterator<AWoTPitOpening> It(World); It; ++It)
			{
				TestNull(TEXT("preview Pit never claims an opening"), It->ThePitTrap.Get());
			}
			TestEqual(TEXT("preview Pit remains owned by local controller"),
				PreviewPit->GetOwner(), static_cast<AActor*>(Controller));
		}
		Controller->ServerCreateResource(AdapterName, ValidLocation, ValidNormal);
		AWoTTrap* Real = EditorGhostSpecFindCreatedResource(
			World, Descriptor.ResourceClass);
		const bool bServerAccepted = IsValid(Real)
			&& Budget->GetItemCount(BudgetIndex) == 0;
		TestEqual(FString::Printf(TEXT("%s valid preview agrees with complete server create"),
			*AdapterName.ToString()), bPreviewAccepted, bServerAccepted);
		if (bPreviewAccepted && bServerAccepted)
		{
			TestTrue(FString::Printf(TEXT("%s snapped location agrees"),
				*AdapterName.ToString()),
				Ghost->GetActorLocation().Equals(Real->GetActorLocation(), 0.1f));
			TestTrue(FString::Printf(TEXT("%s snapped rotation agrees"),
				*AdapterName.ToString()),
				Ghost->GetActorRotation().Equals(Real->GetActorRotation(), 0.1f));
		}
		if (AdapterName == TEXT("PitInventory") && bServerAccepted)
		{
			Controller->SyncCitadelGhostPreview(
				EditorGhostSpecWorldHit(ValidLocation, ValidNormal));
			TestFalse(TEXT("preview Pit invalidates when the last opening is claimed"),
				Controller->IsCitadelGhostPlacementValid());
		}
		WoTTest::TickWorld(World, 1);
		WoTTest::DestroySpecWorld(World);
	}
	for (FName ToggleAdapter : {FName(TEXT("PortcullisInventory")),
		FName(TEXT("StaircaseInventory"))})
	{
		UWorld* World = EditorGhostSpecCreateWorld();
		EditorGhostSpecSpawnZone(World);
		EditorGhostSpecSpawnTeamAltar(World);
		AWoTPlayerController* Controller = EditorGhostSpecSpawnController(World);
		EditorGhostSpecSetCount(Controller, ToggleAdapter, 1);
		Controller->SetCitadelPaletteSelection(ToggleAdapter);
		Controller->ReconcileCitadelGhostPreview();
		const FWoTResourceDescriptor& Descriptor =
			WoTCitadelResources::FindByAdapterName(ToggleAdapter);
		TestFalse(FString::Printf(TEXT("%s has no spawn placement path"),
			*ToggleAdapter.ToString()), Descriptor.bSpawnTempResource);
		TestNull(FString::Printf(TEXT("%s therefore has no ghost transform"),
			*ToggleAdapter.ToString()), Controller->GetCitadelGhostPreview());
		WoTTest::TickWorld(World, 1);
		WoTTest::DestroySpecWorld(World);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelEditorGhostZeroSimulationSpec,
	"WoT.Citadel.EditorGhost.ZeroSimulation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelEditorGhostZeroSimulationSpec::RunTest(const FString&)
{
	static const FName SpawnAdapters[] = {
		TEXT("AlarmInventory"), TEXT("SpearInventory"), TEXT("WallInventory"),
		TEXT("FireWallInventory"), TEXT("PitInventory")};
	for (FName AdapterName : SpawnAdapters)
	{
		UWorld* World = EditorGhostSpecCreateWorld();
		EditorGhostSpecSpawnZone(World);
		EditorGhostSpecSpawnTeamAltar(World);
		EditorGhostSpecSpawnPlacementFixture(World, AdapterName);
		AWoTPlayerController* Controller = EditorGhostSpecSpawnController(World);
		EditorGhostSpecSetCount(Controller, AdapterName, 3);
		AWoTBudgetInfo* Budget = World->SpawnActor<AWoTBudgetInfo>(
			EditorGhostSpecAlwaysSpawnParams());
		FWoTBudgetResource BudgetRow;
		BudgetRow.ResourceAdapterName = AdapterName;
		BudgetRow.ItemCount = 3;
		Budget->Budget.Add(BudgetRow);
		Controller->SetCitadelPaletteSelection(AdapterName);
		Controller->ReconcileCitadelGhostPreview();
		AWoTTrap* Ghost = Controller->GetCitadelGhostPreview();
		FVector ValidLocation;
		FVector ValidNormal;
		FVector InvalidLocation;
		FVector InvalidNormal;
		EditorGhostSpecAim(AdapterName, ValidLocation, ValidNormal,
			InvalidLocation, InvalidNormal);
		Controller->SyncCitadelGhostPreview(
			EditorGhostSpecWorldHit(ValidLocation, ValidNormal));
		AWoTCharacter* Enemy = World->SpawnActor<AWoTCharacter>(
			FVector(100, 100, 200), FRotator::ZeroRotator,
			EditorGhostSpecAlwaysSpawnParams());
		if (!IsValid(Enemy))
		{
			AddError(FString::Printf(TEXT("%s enemy-pawn fixture failed"),
				*AdapterName.ToString()));
			WoTTest::DestroySpecWorld(World);
			continue;
		}
		const int32 EnemyHealthBeforePreview = Enemy->Health;
		TArray<TWeakObjectPtr<AActor>> PreviewChildren;

		TestTrue(FString::Printf(TEXT("%s ghost is transient"), *AdapterName.ToString()),
			IsValid(Ghost) && Ghost->HasAnyFlags(RF_Transient));
		TestFalse(FString::Printf(TEXT("%s ghost is non-replicated"), *AdapterName.ToString()),
			IsValid(Ghost) && Ghost->GetIsReplicated());
		TestTrue(FString::Printf(TEXT("%s ghost primitives are owner-only/inert"),
			*AdapterName.ToString()), EditorGhostSpecAllPrimitivesAreLocalAndInert(Ghost));
		TestEqual(FString::Printf(TEXT("%s owner mirror unchanged"), *AdapterName.ToString()),
			Controller->GetOwnerCitadelBudgetCount(AdapterName), 3);
		TestEqual(FString::Printf(TEXT("%s authoritative budget unchanged"),
			*AdapterName.ToString()), Budget->GetItemCount(Budget->GetIndex(AdapterName)), 3);

		if (AWoTAlarm* Alarm = Cast<AWoTAlarm>(Ghost))
		{
			Alarm->ActivateAlarm(nullptr);
			TestFalse(TEXT("preview Alarm never rings"), Alarm->bRung);
			TestFalse(TEXT("preview Alarm has no sound timer"),
				Alarm->HasActiveSoundTimerForTest());
		}
		if (AWoTSpear* Spear = Cast<AWoTSpear>(Ghost))
		{
			Spear->TrapTouch(Enemy);
			Spear->DamageActor(Enemy);
			TestEqual(TEXT("preview Spear cannot damage a real enemy pawn"),
				Enemy->Health, EnemyHealthBeforePreview);
			TestEqual(TEXT("preview Spear spawns no blockers"),
				Spear->GetBlockerCountForTest(), 0);
			TestEqual(TEXT("preview Spear stays waiting"),
				static_cast<int32>(Spear->GetPhase()),
				static_cast<int32>(EWoTSpearPhase::Waiting));
			TestFalse(TEXT("preview Spear has no phase timer"),
				Spear->HasActivePhaseTimerForTest());
		}

		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Child = *It;
			if (!IsValid(Child) || Child->GetOwner() != Ghost)
			{
				continue;
			}
			PreviewChildren.Add(Child);
			TestTrue(TEXT("preview child is transient"), Child->HasAnyFlags(RF_Transient));
			TestFalse(TEXT("preview child is non-replicated"), Child->GetIsReplicated());
			TestTrue(TEXT("preview child primitives are owner-only/inert"),
				EditorGhostSpecAllPrimitivesAreLocalAndInert(Child));
			if (AWoTWallSlab* WallSlab = Cast<AWoTWallSlab>(Child))
			{
				const int32 WithstandBeforeDamage = WallSlab->WithstandDamage;
				WallSlab->TakeDamage(100.0f, FDamageEvent(), nullptr, Ghost);
				TestEqual(TEXT("preview Wall slab ignores direct damage"),
					WallSlab->WithstandDamage, WithstandBeforeDamage);
			}
			if (AWoTFireWallSlab* FireSlab = Cast<AWoTFireWallSlab>(Child))
			{
				FireSlab->DamageActor(Enemy);
				TestEqual(TEXT("preview FireWall cannot damage a real enemy pawn"),
					Enemy->Health, EnemyHealthBeforePreview);
				const int32 HealthBeforeDamage = FireSlab->Health;
				FireSlab->TakeDamage(100.0f, FDamageEvent(), nullptr, Ghost);
				TestEqual(TEXT("preview FireWall ignores direct damage"),
					FireSlab->Health, HealthBeforeDamage);
				TestFalse(TEXT("preview FireWall slab has no burn timer"),
					FireSlab->HasActiveBurnTimerForTest());
				for (const TObjectPtr<AWoTParticleSprayer>& Sprayer
					: FireSlab->FlameSprayers)
				{
					TestNull(TEXT("preview FireWall spawns no flame sprayer"), Sprayer.Get());
				}
			}
		}
		if (Cast<AWoTPit>(Ghost))
		{
			for (TActorIterator<AWoTPitOpening> It(World); It; ++It)
			{
				TestNull(TEXT("zero-sim PitOpening remains unclaimed"), It->ThePitTrap.Get());
				TestNull(TEXT("zero-sim PitOpening ownership remains unchanged"), It->GetOwner());
			}
		}

		WoTTest::TickWorld(World, 1);
		TestEqual(TEXT("1/60 s tick does not mutate authoritative budget"),
			Budget->GetItemCount(Budget->GetIndex(AdapterName)), 3);
		const TWeakObjectPtr<AWoTTrap> DestroyedGhost(Ghost);
		Controller->SetCitadelPaletteSelection(NAME_None);
		WoTTest::TickWorld(World, 1);
		TestFalse(TEXT("selection teardown destroys the preview parent"),
			DestroyedGhost.IsValid());
		for (const TWeakObjectPtr<AActor>& PreviewChild : PreviewChildren)
		{
			TestFalse(TEXT("preview parent destruction cleans up every child"),
				PreviewChild.IsValid());
		}
		WoTTest::DestroySpecWorld(World);
	}

	// Defensive preview mode also suppresses the external mover claims of the two
	// preplaced/toggle resources, even though normal lifecycle never ghosts them.
	{
		UWorld* World = EditorGhostSpecCreateWorld();
		const FTransform Transform = FTransform::Identity;
		AWoTPortcullisMover* PortMover = World->SpawnActorDeferred<AWoTPortcullisMover>(
			AWoTPortcullisMover::StaticClass(), Transform);
		PortMover->TagName = TEXT("EditorGhostPortMover");
		PortMover->FinishSpawning(Transform);
		AWoTPortcullis* PreviewPort = World->SpawnActorDeferred<AWoTPortcullis>(
			AWoTPortcullis::StaticClass(), Transform);
		PreviewPort->MoverTag = TEXT("EditorGhostPortMover");
		PreviewPort->SetPreviewMode(true);
		PreviewPort->FinishSpawning(Transform);
		TestNull(TEXT("preview Portcullis does not claim mover"), PortMover->GetOwner());
		TestNull(TEXT("preview Portcullis stores no mover"), PreviewPort->GetMover());
		PreviewPort->Activate();
		TestFalse(TEXT("active preview Portcullis is never deployed"),
			PreviewPort->IsTrapDeployed());

		AWoTMover* StairMover = World->SpawnActorDeferred<AWoTMover>(
			AWoTMover::StaticClass(), Transform);
		StairMover->TagName = TEXT("EditorGhostStairMover");
		StairMover->StayOpenTime = 2.5f;
		StairMover->FinishSpawning(Transform);
		AWoTStaircase* PreviewStair = World->SpawnActorDeferred<AWoTStaircase>(
			AWoTStaircase::StaticClass(), Transform);
		PreviewStair->MoverTag = TEXT("EditorGhostStairMover");
		PreviewStair->SetPreviewMode(true);
		PreviewStair->FinishSpawning(Transform);
		TestEqual(TEXT("preview Staircase does not mutate mover dwell"),
			StairMover->StayOpenTime, 2.5f);
		PreviewStair->Activate();
		TestFalse(TEXT("active preview Staircase is never deployed"),
			PreviewStair->IsTrapDeployed());
		WoTTest::TickWorld(World, 1);
		WoTTest::DestroySpecWorld(World);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelEditorGhostTrapDetectSpec,
	"WoT.Citadel.EditorGhost.TrapDetectIgnoresPreviews",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelEditorGhostTrapDetectSpec::RunTest(const FString&)
{
	static const FName SpawnAdapters[] = {
		TEXT("AlarmInventory"), TEXT("SpearInventory"), TEXT("WallInventory"),
		TEXT("FireWallInventory"), TEXT("PitInventory")};
	for (FName AdapterName : SpawnAdapters)
	{
		UWorld* World = EditorGhostSpecCreateWorld();
		EditorGhostSpecSpawnZone(World);
		EditorGhostSpecSpawnTeamAltar(World);
		EditorGhostSpecSpawnPlacementFixture(World, AdapterName);
		AWoTPlayerController* Editor = EditorGhostSpecSpawnController(World);
		if (!IsValid(Editor))
		{
			AddError(FString::Printf(TEXT("%s editor fixture failed"),
				*AdapterName.ToString()));
			WoTTest::DestroySpecWorld(World);
			continue;
		}
		EditorGhostSpecSetCount(Editor, AdapterName, 1);
		Editor->SetCitadelPaletteSelection(AdapterName);
		Editor->ReconcileCitadelGhostPreview();

		FVector ValidLocation;
		FVector ValidNormal;
		FVector InvalidLocation;
		FVector InvalidNormal;
		EditorGhostSpecAim(AdapterName, ValidLocation, ValidNormal,
			InvalidLocation, InvalidNormal);
		Editor->SyncCitadelGhostPreview(
			EditorGhostSpecWorldHit(ValidLocation, ValidNormal));
		AWoTTrap* Ghost = Editor->GetCitadelGhostPreview();

		AWoTPlayerController* ObserverController =
			World->SpawnActor<AWoTPlayerController>(EditorGhostSpecAlwaysSpawnParams());
		AWoTCharacter* Observer = World->SpawnActor<AWoTCharacter>(
			IsValid(Ghost) ? Ghost->GetActorLocation() + FVector(50, 0, 0)
				: FVector::ZeroVector,
			FRotator::ZeroRotator,
			EditorGhostSpecAlwaysSpawnParams());
		if (!IsValid(Editor) || !IsValid(Ghost) || !IsValid(ObserverController)
			|| !IsValid(Observer))
		{
			AddError(FString::Printf(TEXT("%s trap-detector fixture failed"),
				*AdapterName.ToString()));
			WoTTest::DestroySpecWorld(World);
			continue;
		}
		ObserverController->Possess(Observer);
		UWoTTrapDetectLeech* Detector = NewObject<UWoTTrapDetectLeech>(Observer);
		if (!IsValid(Detector))
		{
			AddError(FString::Printf(TEXT("%s detector creation failed"),
				*AdapterName.ToString()));
			WoTTest::DestroySpecWorld(World);
			continue;
		}
		Detector->AttachTo(Observer);
		const float InitialCadence = Detector->AffectResolution;
		TestTrue(TEXT("trap-detect observer is authoritative"), Observer->HasAuthority());

		int32 PreviewChildTrapCount = 0;
		for (TActorIterator<AWoTTrap> It(World); It; ++It)
		{
			AWoTTrap* PreviewTrap = *It;
			if (PreviewTrap == Ghost || PreviewTrap->GetOwner() == Ghost)
			{
				PreviewChildTrapCount += PreviewTrap->GetOwner() == Ghost ? 1 : 0;
				TestFalse(TEXT("preview parent and slab report undeployed"),
					PreviewTrap->IsTrapDeployed());
			}
		}
		if (AdapterName == TEXT("WallInventory")
			|| AdapterName == TEXT("FireWallInventory"))
		{
			TestTrue(TEXT("wall preview detector fixture includes slab children"),
				PreviewChildTrapCount > 0);
		}
		Detector->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
		TestNull(FString::Printf(TEXT("%s preview parent/slabs never become ClosestTrap"),
			*AdapterName.ToString()), Detector->ClosestTrap.Get());
		TestEqual(FString::Printf(TEXT("%s preview does not change detector cadence"),
			*AdapterName.ToString()), Detector->AffectResolution, InitialCadence);
		Detector->AffectHost();
		TestEqual(FString::Printf(TEXT("%s preview requests no heartbeat sound"),
			*AdapterName.ToString()), Detector->GetHeartbeatRequestCountForTest(), 0);

		WoTTest::DestroySpecWorld(World);
	}
	return true;
}

#endif // WITH_AUTOMATION_TESTS
