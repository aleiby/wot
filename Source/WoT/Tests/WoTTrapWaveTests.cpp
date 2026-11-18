// P27+G30+G34 trap wave: adapter status-icon binds, citadel-editor deploy/remove UI
// sounds, PortcullisMover phase sounds (+ the quiet HideMover snap), the Trap.uc
// AmbientGlow editing cue, and the Pit no-opening client feedback.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../AI/WoTCreatureData.h"
#include "../AI/WoTNPCCharacter.h"
#include "../AI/WoTNPCController.h"
#include "../AI/WoTPathNode.h"
#include "../Inventory/WoTSeal.h"
#include "../Inventory/WoTTroopInventories.h"
#include "../WoTMapLightTable.h"
#include "../WoTMeshLight.h"
#include "../Traps/WoTAlarm.h"
#include "../Traps/WoTFireWall.h"
#include "../Traps/WoTPit.h"
#include "../Traps/WoTPortcullis.h"
#include "../Traps/WoTSpear.h"
#include "../Traps/WoTStaircase.h"
#include "../Traps/WoTWall.h"
#include "../WoTBattleGameMode.h"
#include "../WoTBattleInfo.h"
#include "../WoTBudgetInfo.h"
#include "../WoTCharacter.h"
#include "../WoTCitadelResources.h"
#include "../WoTMover.h"
#include "../WoTPlayerController.h"
#include "../WoTPlayerState.h"
#include "../WoTSealAltar.h"
#include "../WoTZoneVolume.h"
#include "WoTTestBattleClasses.h"

#include "Components/BoxComponent.h"
#include "Engine/DamageEvents.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/WorldSettings.h"

namespace
{
	FActorSpawnParameters TrapWaveSpecAlwaysSpawnParams()
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		return Params;
	}

	UWorld* TrapWaveSpecCreateWorld(bool bBattleMode)
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

	// Recording controller (WoTTestBattleClasses.h): ClientPlayUISound_Implementation
	// captures instead of playing. No pawn unless the caller possesses one.
	AWoTTestSealNotifyController* TrapWaveSpecSpawnRecordingController(
		UWorld* World, uint8 InTeam, const FVector& ViewerLocation)
	{
		AWoTTestSealNotifyController* Controller =
			World->SpawnActor<AWoTTestSealNotifyController>(
				TrapWaveSpecAlwaysSpawnParams());
		if (!IsValid(Controller))
		{
			return nullptr;
		}
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

	AWoTZoneVolume* TrapWaveSpecSpawnEditZone(
		UWorld* World, const FVector& Center, const FVector& Extent, uint8 EditTeam)
	{
		const FTransform Transform(FRotator::ZeroRotator, Center);
		AWoTZoneVolume* Zone = World->SpawnActorDeferred<AWoTZoneVolume>(
			AWoTZoneVolume::StaticClass(), Transform);
		Zone->EditTeam = EditTeam;
		Zone->bAllowEditing = true;
		if (IsValid(Zone->Collision))
		{
			Zone->Collision->SetBoxExtent(Extent);
		}
		Zone->FinishSpawning(Transform);
		return Zone;
	}

	AWoTSealAltar* TrapWaveSpecSpawnAltar(
		UWorld* World, uint8 InTeam, const FVector& Location)
	{
		const FTransform Transform(FRotator::ZeroRotator, Location);
		AWoTSealAltar* Altar = World->SpawnActorDeferred<AWoTSealAltar>(
			AWoTSealAltar::StaticClass(), Transform);
		Altar->Team = InTeam;
		Altar->FinishSpawning(Transform);
		return Altar;
	}

	void TrapWaveSpecAddBudgetRow(
		AWoTBudgetInfo* BudgetInfo, FName AdapterName, int32 Count)
	{
		FWoTBudgetResource Resource;
		Resource.ResourcePackageName = TEXT("WoTTraps");
		Resource.ResourceAdapterName = AdapterName;
		Resource.ItemCount = Count;
		BudgetInfo->Budget.Add(Resource);
	}

	AActor* TrapWaveSpecSpawnFloor(
		UWorld* World, const FVector& Center, const FVector& Extent)
	{
		AActor* Floor = World->SpawnActor<AActor>(
			Center, FRotator::ZeroRotator, TrapWaveSpecAlwaysSpawnParams());
		UBoxComponent* Box = NewObject<UBoxComponent>(Floor);
		Box->SetBoxExtent(Extent);
		Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Box->SetCollisionObjectType(ECC_WorldStatic);
		Box->SetCollisionResponseToAllChannels(ECR_Block);
		Floor->SetRootComponent(Box);
		Box->RegisterComponent();
		Floor->SetActorLocation(Center);
		return Floor;
	}

	int32 TrapWaveSpecSoundCount(
		const AWoTTestSealNotifyController* Controller, FName SubPath)
	{
		int32 Count = 0;
		for (const FName& Sound : Controller->UISounds)
		{
			if (Sound == SubPath)
			{
				++Count;
			}
		}
		return Count;
	}
}

// -------------------- 1) Adapter status-icon binds (CDO census) ---------------- //

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTTrapIconsAdapterBindsSpec,
	"WoT.Trap.Icons.AdapterBinds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTTrapIconsAdapterBindsSpec::RunTest(const FString&)
{
	struct FExpectedIcons
	{
		UClass* AdapterClass;
		const TCHAR* Directory;
		const TCHAR* IconSuffix;   // I_S<X> / M_S<X>
	};
	// The authored defaultproperties pairs: each *Inventory.uc supplies
	// StatusIcon=Texture'<Pkg>.UI.I_S<X>' + StatusIconFrame=Texture'<Pkg>.UI.M_S<X>'.
	// FireWallInventory.uc authors the FIRE pair; MashadarInventory.uc pairs
	// I_SMashadar with the MashadarFocusPawn resource row.
	const FExpectedIcons Expected[] = {
		{AWoTAlarmInventory::StaticClass(), TEXT("WoTTraps"), TEXT("Alarm")},
		{AWoTFireWallInventory::StaticClass(), TEXT("WoTTraps"), TEXT("Fire")},
		{AWoTPitInventory::StaticClass(), TEXT("WoTTraps"), TEXT("Pit")},
		{AWoTPortcullisInventory::StaticClass(), TEXT("WoTTraps"), TEXT("Portcullis")},
		{AWoTSpearInventory::StaticClass(), TEXT("WoTTraps"), TEXT("Spear")},
		{AWoTStaircaseInventory::StaticClass(), TEXT("WoTTraps"), TEXT("Staircase")},
		{AWoTWallInventory::StaticClass(), TEXT("WoTTraps"), TEXT("Wall")},
		{AWoTWarderInventory::StaticClass(), TEXT("WoTPawns"), TEXT("Warder")},
		{AWoTSisterInventory::StaticClass(), TEXT("WoTPawns"), TEXT("Sister")},
		{AWoTSitterInventory::StaticClass(), TEXT("WoTPawns"), TEXT("Sitter")},
		{AWoTTrollocInventory::StaticClass(), TEXT("WoTPawns"), TEXT("Trolloc")},
		{AWoTMyrddraalInventory::StaticClass(), TEXT("WoTPawns"), TEXT("Myrddraal")},
		{AWoTBATrollocInventory::StaticClass(), TEXT("WoTPawns"), TEXT("BATrolloc")},
		{AWoTMinionInventory::StaticClass(), TEXT("WoTPawns"), TEXT("Minion")},
		{AWoTMashadarInventory::StaticClass(), TEXT("WoTPawns"), TEXT("Mashadar")},
		{AWoTLegionInventory::StaticClass(), TEXT("WoTPawns"), TEXT("Legion")},
		{AWoTSoldierInventory::StaticClass(), TEXT("WoTPawns"), TEXT("Soldier")},
		{AWoTArcherInventory::StaticClass(), TEXT("WoTPawns"), TEXT("Archer")},
		{AWoTQuestionerInventory::StaticClass(), TEXT("WoTPawns"), TEXT("Questioner")},
		{AWoTSealInventory::StaticClass(), TEXT("WOT"), TEXT("Seal")},
	};
	TestEqual(TEXT("twenty adapter icon pairs pinned"),
		static_cast<int32>(UE_ARRAY_COUNT(Expected)), 20);
	for (const FExpectedIcons& Entry : Expected)
	{
		const AWoTInventory* CDO =
			Entry.AdapterClass->GetDefaultObject<AWoTInventory>();
		if (!TestNotNull(*FString::Printf(TEXT("%s CDO"),
			*Entry.AdapterClass->GetName()), CDO))
		{
			continue;
		}
		// Soft-path (load-free) equality against the imported texture location.
		const FString ExpectedIcon = FString::Printf(
			TEXT("/Game/WoT/Textures/%s/I_S%s.I_S%s"),
			Entry.Directory, Entry.IconSuffix, Entry.IconSuffix);
		const FString ExpectedFrame = FString::Printf(
			TEXT("/Game/WoT/Textures/%s/M_S%s.M_S%s"),
			Entry.Directory, Entry.IconSuffix, Entry.IconSuffix);
		TestFalse(*FString::Printf(TEXT("%s StatusIcon bound"),
			*Entry.AdapterClass->GetName()), CDO->StatusIcon.IsNull());
		TestFalse(*FString::Printf(TEXT("%s StatusIconFrame bound"),
			*Entry.AdapterClass->GetName()), CDO->StatusIconFrame.IsNull());
		TestEqual(*FString::Printf(TEXT("%s StatusIcon path"),
			*Entry.AdapterClass->GetName()),
			CDO->StatusIcon.ToString(), ExpectedIcon);
		TestEqual(*FString::Printf(TEXT("%s StatusIconFrame path"),
			*Entry.AdapterClass->GetName()),
			CDO->StatusIconFrame.ToString(), ExpectedFrame);
	}
	return true;
}

// ---------------- 2) Citadel-editor deploy/remove sounds: trap legs ------------ //

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelEditorSoundsTrapSpec,
	"WoT.Citadel.EditorSounds.TrapLegs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelEditorSoundsTrapSpec::RunTest(const FString&)
{
	UWorld* World = TrapWaveSpecCreateWorld(true);
	TrapWaveSpecSpawnAltar(World, 1, FVector::ZeroVector);
	TrapWaveSpecSpawnAltar(World, 2, FVector(5000, 0, 0));
	TrapWaveSpecSpawnFloor(World, FVector::ZeroVector, FVector(500, 500, 10));
	AWoTBudgetInfo* BudgetInfo = World->SpawnActor<AWoTBudgetInfo>(
		TrapWaveSpecAlwaysSpawnParams());
	BudgetInfo->Team = 1;
	TrapWaveSpecAddBudgetRow(BudgetInfo, TEXT("SpearInventory"), 1);
	TrapWaveSpecAddBudgetRow(BudgetInfo, TEXT("PortcullisInventory"), 1);
	TrapWaveSpecSpawnEditZone(World, FVector::ZeroVector, FVector(2000), 1);
	AWoTTestSealNotifyController* Controller =
		TrapWaveSpecSpawnRecordingController(World, 1, FVector::ZeroVector);
	if (!TestNotNull(TEXT("recording controller spawned"), Controller))
	{
		WoTTest::DestroySpecWorld(World);
		return true;
	}
	Controller->CitadelBeginEdit();
	TestTrue(TEXT("edit session open"), Controller->IsEditingCitadel());
	TestEqual(TEXT("no sound from session bring-up"),
		Controller->UISounds.Num(), 0);

	const FName DeployTrap(TEXT("WoTBase/DeployTrap"));
	const FName RemoveTrap(TEXT("WoTBase/RemoveTrap"));

	// Deploy REFUSAL first (WOTPlayer.uc:4926-4931: DeployResource false -> Destroy,
	// return — before the ClientPlaySound): cursor off the floor fails CalcLocation.
	Controller->ServerCreateResource(
		TEXT("SpearInventory"), FVector(600, 0, 200), FVector(0, 0, 1));
	TestEqual(TEXT("deploy refusal plays nothing"), Controller->UISounds.Num(), 0);

	// Create success (WOTPlayer.uc:4938): GetDeploySound -> WOTBase.Editor.DeployTrap.
	Controller->ServerCreateResource(
		TEXT("SpearInventory"), FVector(0, 0, 200), FVector(0, 0, 1));
	TestEqual(TEXT("trap create plays the deploy-trap sound once"),
		TrapWaveSpecSoundCount(Controller, DeployTrap), 1);
	TestEqual(TEXT("trap create plays nothing else"), Controller->UISounds.Num(), 1);

	// No-budget refusal: the count gate returns before any spawn or sound.
	Controller->ServerCreateResource(
		TEXT("SpearInventory"), FVector(200, 0, 200), FVector(0, 0, 1));
	TestEqual(TEXT("no-budget create plays nothing"), Controller->UISounds.Num(), 1);

	// Remove success (WOTPlayer.uc:4965): GetRemoveSound -> WOTBase.Editor.RemoveTrap.
	AWoTSpear* Placed = nullptr;
	for (TActorIterator<AWoTSpear> It(World); It; ++It)
	{
		if (IsValid(*It))
		{
			Placed = *It;
		}
	}
	if (TestNotNull(TEXT("placed spear found"), Placed))
	{
		Controller->ServerRemoveResource(Placed);
		TestEqual(TEXT("trap remove plays the remove-trap sound once"),
			TrapWaveSpecSoundCount(Controller, RemoveTrap), 1);
		TestEqual(TEXT("trap remove plays nothing else"),
			Controller->UISounds.Num(), 2);
	}

	// Toggle legs (WOTPlayer.uc:5014/:5043): activate -> deploy, deactivate -> remove.
	AWoTPortcullis* Gate = World->SpawnActor<AWoTPortcullis>(
		FVector(100, 0, 0), FRotator::ZeroRotator, TrapWaveSpecAlwaysSpawnParams());
	Controller->ServerToggleResource(Gate);
	TestTrue(TEXT("toggle armed the gate"), Gate->IsActive());
	TestEqual(TEXT("toggle-activate plays the deploy-trap sound"),
		TrapWaveSpecSoundCount(Controller, DeployTrap), 2);
	Controller->ServerToggleResource(Gate);
	TestFalse(TEXT("toggle disarmed the gate"), Gate->IsActive());
	TestEqual(TEXT("toggle-deactivate plays the remove-trap sound"),
		TrapWaveSpecSoundCount(Controller, RemoveTrap), 2);
	TestEqual(TEXT("four success sounds total"), Controller->UISounds.Num(), 4);

	// Toggle refusal: a foreign-owned gate cannot be activated — no sound.
	AWoTPortcullis* Foreign = World->SpawnActor<AWoTPortcullis>(
		FVector(150, 0, 0), FRotator::ZeroRotator, TrapWaveSpecAlwaysSpawnParams());
	Foreign->Team = 2;
	Controller->ServerToggleResource(Foreign);
	TestFalse(TEXT("foreign gate stays inactive"), Foreign->IsActive());
	TestEqual(TEXT("toggle refusal plays nothing"), Controller->UISounds.Num(), 4);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// ------------- 2b) Citadel-editor deploy/remove sounds: troop + seal ----------- //

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelEditorSoundsTroopSealSpec,
	"WoT.Citadel.EditorSounds.TroopSealLegs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelEditorSoundsTroopSealSpec::RunTest(const FString&)
{
	UWorld* World = TrapWaveSpecCreateWorld(true);
	TrapWaveSpecSpawnAltar(World, 1, FVector::ZeroVector);
	TrapWaveSpecSpawnAltar(World, 2, FVector(5000, 0, 0));
	AWoTBudgetInfo* BudgetInfo = World->SpawnActor<AWoTBudgetInfo>(
		TrapWaveSpecAlwaysSpawnParams());
	BudgetInfo->Team = 1;
	TrapWaveSpecAddBudgetRow(BudgetInfo, TEXT("SealInventory"), 1);
	TrapWaveSpecAddBudgetRow(BudgetInfo, TEXT("WarderInventory"), 1);
	TrapWaveSpecAddBudgetRow(BudgetInfo, TEXT("MashadarInventory"), 1);
	TrapWaveSpecSpawnFloor(World, FVector(0, 0, -10), FVector(1000, 1000, 10));
	AWoTPathNode* SealNode = World->SpawnActor<AWoTPathNode>(
		FVector(500, 0, 50), FRotator::ZeroRotator,
		TrapWaveSpecAlwaysSpawnParams());
	TestNotNull(TEXT("seal placement path node stamped"), SealNode);
	TrapWaveSpecSpawnEditZone(World, FVector::ZeroVector, FVector(2000), 1);
	AWoTTestSealNotifyController* Controller =
		TrapWaveSpecSpawnRecordingController(World, 1, FVector(0, 0, 100));
	if (!TestNotNull(TEXT("recording controller spawned"), Controller))
	{
		WoTTest::DestroySpecWorld(World);
		return true;
	}
	// Troop placement reads the possessed pawn (player-front spot).
	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator, TrapWaveSpecAlwaysSpawnParams());
	Controller->Possess(Pawn);
	Controller->CitadelBeginEdit();
	TestTrue(TEXT("edit session open"), Controller->IsEditingCitadel());

	// G34: the first-session SealInventory card creates one validated ground seal,
	// spends the owner card, and plays the owner-only deploy sound
	// (WOTPlayer.uc:4897-4939; Seal.uc:254-300).
	Controller->ServerCreateResource(
		TEXT("SealInventory"), FVector(500, 0, 8), FVector::UpVector);
	TestEqual(TEXT("seal-card create plays DeploySeal once"),
		TrapWaveSpecSoundCount(Controller, FName(TEXT("WOT/DeploySeal"))), 1);
	TestEqual(TEXT("seal-card count decremented"),
		Controller->OwnerSealCardCount, 0);

	// Troop deploy FAILURE (MashadarFocusPawn without an eligible guide): destroy,
	// no spend, no sound.
	Controller->ServerCreateResource(
		TEXT("MashadarInventory"), FVector::ZeroVector, FVector::UpVector);
	TestEqual(TEXT("failed troop deploy adds no sound"),
		Controller->UISounds.Num(), 1);

	// Troop deploy success (WOTPlayer.uc:4938 via WOTPawn.uc:538-542).
	Controller->ServerCreateResource(
		TEXT("WarderInventory"), FVector::ZeroVector, FVector::UpVector);
	AWoTNPCCharacter* Warder = nullptr;
	for (TActorIterator<AWoTNPCCharacter> It(World); It; ++It)
	{
		if (IsValid(*It) && It->CreatureRowName == FName(TEXT("Warder")))
		{
			Warder = *It;
		}
	}
	if (TestNotNull(TEXT("warder deployed"), Warder))
	{
		TestEqual(TEXT("troop deploy plays WOT/DeployPawn"),
			Controller->UISounds.Num(), 2);
		TestEqual(TEXT("troop deploy sound subpath"),
			Controller->UISounds.Num() > 0 ? Controller->UISounds.Last() : FName(),
			FName(TEXT("WOT/DeployPawn")));

		// Troop remove (WOTPlayer.uc:4965 via WOTPawn.uc:546-548).
		Controller->ServerRemoveResource(Warder);
		TestEqual(TEXT("troop remove plays WOT/RemovePawn"),
			Controller->UISounds.Num(), 3);
		TestEqual(TEXT("troop remove sound subpath"),
			Controller->UISounds.Num() > 0 ? Controller->UISounds.Last() : FName(),
			FName(TEXT("WOT/RemovePawn")));
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

// --------- 3) PortcullisMover phase sounds + the quiet HideMover snap ---------- //

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPortcullisMoverSoundsSpec,
	"WoT.Mover.PortcullisSounds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTPortcullisMoverSoundsSpec::RunTest(const FString&)
{
	// CDO: PortcullisMover.uc:93-96 authors the MoverSounds defaults.
	const AWoTPortcullisMover* CDO = GetDefault<AWoTPortcullisMover>();
	TestEqual(TEXT("OpeningSound=OpenPT"), CDO->OpeningSoundPath,
		FName(TEXT("WoTTraps/OpenPT")));
	TestEqual(TEXT("OpenedSound=OpenPT"), CDO->OpenedSoundPath,
		FName(TEXT("WoTTraps/OpenPT")));
	TestEqual(TEXT("ClosingSound=ClosePT"), CDO->ClosingSoundPath,
		FName(TEXT("WoTTraps/ClosePT")));
	TestEqual(TEXT("ClosedSound=ClosedPT"), CDO->ClosedSoundPath,
		FName(TEXT("WoTTraps/ClosedPT")));

	UWorld* World = TrapWaveSpecCreateWorld(false);
	const FTransform Transform(FRotator::ZeroRotator, FVector::ZeroVector);
	AWoTPortcullisMover* Mover = World->SpawnActorDeferred<AWoTPortcullisMover>(
		AWoTPortcullisMover::StaticClass(), Transform);
	Mover->KeyPos = {FVector::ZeroVector, FVector(0, 0, 100)};
	Mover->KeyRot = {FRotator::ZeroRotator, FRotator::ZeroRotator};
	Mover->MoveTime = 0.3f;
	Mover->FinishSpawning(Transform);
	WoTTest::TickWorld(World, 2);

	// PreBeginPlay HideMover: the retail InterpolateTo(1,0) snap is silent
	// (movement-only; DoOpen/FinishedOpening never run in Engine.Mover).
	TestEqual(TEXT("start-raised snap is at the open pose"),
		static_cast<int32>(Mover->GetMoverPhase()),
		static_cast<int32>(EWoTMoverPhase::Open));
	TestTrue(TEXT("start-raised snap applied alpha 1"),
		FMath::IsNearlyEqual(Mover->GetActorLocation().Z, 100.0, 0.5));
	TestEqual(TEXT("start-raised snap plays no edge sound"),
		Mover->PhaseSoundPlayCount, 0);

	// Activate re-runs the snap AFTER BeginPlay: still silent.
	Mover->Activate();
	TestEqual(TEXT("activate re-snap plays no edge sound"),
		Mover->PhaseSoundPlayCount, 0);

	// The drop: DoClose edge (ClosePT), then FinishedClosing edge (ClosedPT).
	Mover->WoTTrigger(nullptr, nullptr);
	TestEqual(TEXT("drop start plays ClosePT"),
		Mover->LastPhaseSound, FName(TEXT("WoTTraps/ClosePT")));
	TestEqual(TEXT("one edge so far"), Mover->PhaseSoundPlayCount, 1);
	WoTTest::TickWorld(World, 30);   // 0.5s > MoveTime
	TestEqual(TEXT("gate landed closed"),
		static_cast<int32>(Mover->GetMoverPhase()),
		static_cast<int32>(EWoTMoverPhase::Closed));
	TestEqual(TEXT("drop end plays ClosedPT"),
		Mover->LastPhaseSound, FName(TEXT("WoTTraps/ClosedPT")));
	TestEqual(TEXT("two edges after the drop"), Mover->PhaseSoundPlayCount, 2);

	// Destroyed re-raise (PortcullisMover.uc:76-81): HideMover snap again — the
	// authored OpenPT stays unreachable (the derived quiet-snap behavior).
	Mover->TakeDamage(200.0f, FDamageEvent(), nullptr, nullptr);
	TestEqual(TEXT("destroyed gate re-raised"),
		static_cast<int32>(Mover->GetMoverPhase()),
		static_cast<int32>(EWoTMoverPhase::Open));
	TestTrue(TEXT("re-raise snapped to alpha 1"),
		FMath::IsNearlyEqual(Mover->GetActorLocation().Z, 100.0, 0.5));
	TestEqual(TEXT("re-raise plays no Open edge — count still two"),
		Mover->PhaseSoundPlayCount, 2);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// ------------------- 4) Editing glow cue (Trap.uc AmbientGlow) ----------------- //

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTTrapEditingGlowSpec,
	"WoT.Trap.EditingGlow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTTrapEditingGlowSpec::RunTest(const FString&)
{
	UWorld* World = TrapWaveSpecCreateWorld(false);
	AWoTAlarm* Trap = World->SpawnActor<AWoTAlarm>(
		FVector::ZeroVector, FRotator::ZeroRotator, TrapWaveSpecAlwaysSpawnParams());
	if (!TestNotNull(TEXT("trap spawned"), Trap))
	{
		WoTTest::DestroySpecWorld(World);
		return true;
	}
	TestEqual(TEXT("glow starts clear"), Trap->AmbientGlow, 0.0f);

	// The production path is the runtime mesh-light bake (WoTMeshLight.cpp
	// BakeRuntime: Ambient += AmbientGlow/255 per channel) — attach one with a
	// synthetic table and assert the BAKED result, not just the actor property
	// (review-caught: the property-only assert survived a broken bake).
	UWoTMapLightTable* Table = NewObject<UWoTMapLightTable>(World);
	UWoTMeshLight* MeshLight = UWoTMeshLight::AttachRuntime(Trap, Table);
	if (!TestNotNull(TEXT("runtime mesh light attached"), MeshLight))
	{
		WoTTest::DestroySpecWorld(World);
		return true;
	}
	const FLinearColor Baseline = MeshLight->Ambient;
	const float GlowAdd = 64.0f / 255.0f;

	// Authority transitions (Trap.uc:71,76) — the bake must move with them.
	Trap->BeginEditingResource(1);
	TestEqual(TEXT("BeginEditingResource sets AmbientGlow=64"),
		Trap->AmbientGlow, 64.0f);
	TestTrue(TEXT("begin-edit bake adds 64/255 per ambient channel"),
		MeshLight->Ambient.Equals(
			Baseline + FLinearColor(GlowAdd, GlowAdd, GlowAdd, 0.0f),
			KINDA_SMALL_NUMBER));
	Trap->EndEditingResource();
	TestEqual(TEXT("EndEditingResource clears AmbientGlow"),
		Trap->AmbientGlow, 0.0f);
	TestTrue(TEXT("end-edit bake restores the baseline ambient"),
		MeshLight->Ambient.Equals(Baseline, KINDA_SMALL_NUMBER));

	// Client half: the replicated bEditing flag re-derives the glow in OnRep_Editing
	// (driven directly — the headless suite has no second connection).
	Trap->bEditing = true;
	Trap->OnRep_Editing();
	TestEqual(TEXT("OnRep_Editing(true) sets the glow"), Trap->AmbientGlow, 64.0f);
	TestTrue(TEXT("OnRep bake adds the glow"),
		MeshLight->Ambient.Equals(
			Baseline + FLinearColor(GlowAdd, GlowAdd, GlowAdd, 0.0f),
			KINDA_SMALL_NUMBER));
	Trap->bEditing = false;
	Trap->OnRep_Editing();
	TestEqual(TEXT("OnRep_Editing(false) clears the glow"), Trap->AmbientGlow, 0.0f);
	TestTrue(TEXT("OnRep clear restores the baseline ambient"),
		MeshLight->Ambient.Equals(Baseline, KINDA_SMALL_NUMBER));

	WoTTest::DestroySpecWorld(World);
	return true;
}

// ------------------ 5) Pit no-opening client feedback (G30) -------------------- //

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTTrapPitFeedbackSpec,
	"WoT.Trap.PitFeedback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTTrapPitFeedbackSpec::RunTest(const FString&)
{
	UWorld* World = TrapWaveSpecCreateWorld(false);
	AWoTTestMessageCaptureController* Controller =
		World->SpawnActor<AWoTTestMessageCaptureController>(
			TrapWaveSpecAlwaysSpawnParams());
	if (!TestNotNull(TEXT("capture controller spawned"), Controller))
	{
		WoTTest::DestroySpecWorld(World);
		return true;
	}

	// One free opening: the first pit pairs successfully — no message.
	World->SpawnActor<AWoTPitOpening>(
		FVector(500, 0, 0), FRotator::ZeroRotator, TrapWaveSpecAlwaysSpawnParams());
	FActorSpawnParameters OwnedSpawn = TrapWaveSpecAlwaysSpawnParams();
	OwnedSpawn.Owner = Controller;
	AWoTPit* FirstPit = World->SpawnActor<AWoTPit>(
		FVector::ZeroVector, FRotator::ZeroRotator, OwnedSpawn);
	if (!TestNotNull(TEXT("first pit spawned"), FirstPit))
	{
		WoTTest::DestroySpecWorld(World);
		return true;
	}
	TestTrue(TEXT("first pit claimed the opening"),
		IsValid(FirstPit->ThePitTrap) && IsValid(FirstPit->ThePitTrap->ThePitOpening));
	TestEqual(TEXT("successful pairing delivers no message"),
		Controller->CapturedMessages.Num(), 0);

	// No opening left: the second pit destroys its surface and tells the
	// placing owner (Pit.uc:46 behavior, project-authored wording).
	AWoTPit* SecondPit = World->SpawnActor<AWoTPit>(
		FVector(300, 0, 0), FRotator::ZeroRotator, OwnedSpawn);
	if (!TestNotNull(TEXT("second pit spawned"), SecondPit))
	{
		WoTTest::DestroySpecWorld(World);
		return true;
	}
	TestFalse(TEXT("pairing failure released the surface"),
		IsValid(SecondPit->ThePitTrap));
	TestEqual(TEXT("pairing failure delivers exactly one message"),
		Controller->CapturedMessages.Num(), 1);
	if (Controller->CapturedMessages.Num() == 1)
	{
		TestEqual(TEXT("the project-authored pit message"),
			Controller->CapturedMessages[0],
			FString(TEXT("No pit opening is free in this level.")));
		TestEqual(TEXT("typeless (the plain console route)"),
			Controller->CapturedTypes[0], FName(NAME_None));
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
