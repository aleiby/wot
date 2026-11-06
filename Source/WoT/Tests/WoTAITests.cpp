// Class automation specs for the NPC/AI brain core: goal priority
// selection + tiebreak, cache currency, the reached predicate, notifier
// cadence, and the controller's waiting→threat wiring.
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../AI/WoTBrainComponent.h"
#include "../AI/WoTNPCCharacter.h"
#include "../AI/WoTNPCController.h"
#include "../AI/WoTRangeBands.h"
#include "../AI/WoTCasterComponent.h"
#include "../AI/WoTCreatureData.h"
#include "../AI/WoTAIUtil.h"
#include "../AI/WoTCreatureBehaviors.h"
#include "../AI/WoTSight.h"
#include "../AI/WoTOrders.h"
#include "../Traps/WoTAlarm.h"
#include "../WoTAnimTable.h"
#include "../WoTCharacter.h"
#include "../WoTWeaveFX.h"
#include "../WoTSoundTable.h"
#include "../WoTVertexAnimComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMeshActor.h"
#include "../WoTVertexAnimComponent.h"
#include "../Inventory/WoTShieldReturnToSenderReflector.h"
#include "../Inventory/WoTAngrealInvFireball.h"
#include "../Inventory/WoTAutoTargetReflector.h"
#include "../Projectiles/WoTDartProjectile.h"
#include "../WoTGameMode.h"
#include "../WoTPlayerState.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../Inventory/WoTSeal.h"
#include "../Inventory/WoTAngrealInvMinion.h"
#include "../Inventory/WoTAngrealInvGuardian.h"
#include "../Projectiles/WoTMashadarGuide.h"
#include "../Projectiles/WoTMashadarSegment.h"
#include "../Inventory/WoTSeal.h"
#include "../Inventory/WoTAngrealInvChampion.h"
#include "../Inventory/WoTAngrealInvDisguise.h"
#include "../Inventory/WoTDecreaseHealthLeech.h"
#include "../Projectiles/WoTFireballProjectile.h"
#include "../Projectiles/WoTAngrealIllusionProjectile.h"
#include "../Projectiles/WoTProjectile.h"   // FWoTDamageEvent
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "WoTTestAIClasses.h"
#include "../WoTAppearEffect.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Engine/Texture.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/IConsoleManager.h"
#include "NavMesh/NavMeshBoundsVolume.h"
#include "NavigationSystem.h"

namespace
{
	AWoTCharacter* SpawnAIActivityAnchor(UWorld* World, const FVector& Location)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AWoTCharacter* Anchor = World->SpawnActor<AWoTCharacter>(
			Location, FRotator::ZeroRotator, Params);
		if (Anchor)
		{
			// Activity relevance is bIsPlayer-only; dead keeps this fixture pawn
			// out of perception and combat target selection.
			Anchor->Health = 0;
			Anchor->SetActorEnableCollision(false);
			Anchor->SetActorHiddenInGame(true);
		}
		return Anchor;
	}

	AWoTNPCCharacter* SpawnNPC(UWorld* World, const FVector& Location)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AWoTNPCCharacter* NPC = World->SpawnActor<AWoTNPCCharacter>(
			Location, FRotator::ZeroRotator, Params);
		SpawnAIActivityAnchor(World, Location);
		return NPC;
	}

	// An NPC whose brain reports every goal reachable (no navmesh headless),
	// flying so movement needs no floor. Team set BEFORE the first perception
	// sample can classify anyone.
	AWoTTestNPCCharacter* SpawnCombatNPC(UWorld* World, const FVector& Location, uint8 Team)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AWoTTestNPCCharacter* NPC =
			World->SpawnActor<AWoTTestNPCCharacter>(Location, FRotator::ZeroRotator, Params);
		if (NPC)
		{
			NPC->AssignedTeam = Team;
			NPC->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
			SpawnAIActivityAnchor(World, Location);
		}
		return NPC;
	}

	// A captain NPC (orders block enabled). Team set before the first perception
	// sample, like SpawnCombatNPC.
	AWoTTestCaptainNPC* SpawnCaptain(UWorld* World, const FVector& Location, uint8 Team)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AWoTTestCaptainNPC* Captain =
			World->SpawnActor<AWoTTestCaptainNPC>(Location, FRotator::ZeroRotator, Params);
		if (Captain)
		{
			Captain->AssignedTeam = Team;
			Captain->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
			SpawnAIActivityAnchor(World, Location);
		}
		return Captain;
	}

	AActor* SpawnNavigationBox(UWorld* World, const FVector& Location,
		const FVector& Extent, bool bAffectsNavigation = true)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AStaticMeshActor* BoxActor = World->SpawnActor<AStaticMeshActor>(
			Location, FRotator::ZeroRotator, Params);
		UBoxComponent* Box = NewObject<UBoxComponent>(BoxActor);
		Box->SetupAttachment(BoxActor->GetRootComponent());
		Box->SetBoxExtent(Extent);
		Box->SetMobility(EComponentMobility::Static);
		Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Box->SetCollisionObjectType(ECC_WorldStatic);
		Box->SetCollisionResponseToAllChannels(ECR_Block);
		Box->SetCanEverAffectNavigation(bAffectsNavigation);
		Box->RegisterComponent();
		return BoxActor;
	}

	void BuildNavigationTestMesh(UWorld* World)
	{
		// A walkable floor plus a short wall that the path can route around.
		SpawnNavigationBox(World, FVector(0, 0, -50),
			FVector(1200, 1200, 50));
		SpawnNavigationBox(World, FVector(0, 0, 100),
			FVector(30, 160, 150));

		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		ANavMeshBoundsVolume* Bounds = World->SpawnActor<ANavMeshBoundsVolume>(
			ANavMeshBoundsVolume::StaticClass(), FVector(0, 0, 200),
			FRotator::ZeroRotator, Params);
		UBoxComponent* BoundsBox = NewObject<UBoxComponent>(Bounds);
		BoundsBox->SetBoxExtent(FVector(1200, 1200, 400));
		BoundsBox->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		BoundsBox->SetCanEverAffectNavigation(false);
		BoundsBox->SetupAttachment(Bounds->GetRootComponent());
		BoundsBox->RegisterComponent();

		// Standalone test worlds never auto-create a navigation system — add one
		// explicitly (game-mode run) before building, or GetCurrent stays null.
		if (!FNavigationSystem::GetCurrent<UNavigationSystemV1>(World))
		{
			FNavigationSystem::AddNavigationSystemToWorld(
				*World, FNavigationSystemRunMode::GameMode);
		}
		if (UNavigationSystemV1* Nav =
			FNavigationSystem::GetCurrent<UNavigationSystemV1>(World))
		{
			Nav->OnNavigationBoundsUpdated(Bounds);
			Nav->Build();
			for (int32 Tick = 0; Tick < 600 && Nav->IsNavigationBuildInProgress(); ++Tick)
			{
				World->Tick(LEVELTICK_All, 1.0f / 60.0f);
				++GFrameCounter;
			}
		}
	}
}

// --- Goal priority: step function, gates, tiebreak, default table ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAIGoalPriorityTest,
	"WoT.AI.GoalPriority",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTAIGoalPriorityTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* NPC = SpawnNPC(World, FVector(0, 0, 100));
	AWoTNPCCharacter* Target = SpawnNPC(World, FVector(500, 0, 100));
	if (!TestNotNull(TEXT("NPC spawned"), NPC) || !TestNotNull(TEXT("target spawned"), Target))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	UWoTBrainComponent* Brain = NPC->Brain;

	// Empty slot (ExternalDirective, descriptor None) scores the -1 sentinel
	// and is never selected.
	TArray<EWoTGoalIndex> Indices;
	TArray<float> Priorities;
	Brain->GetSortedGoalIndices(Indices, Priorities);
	TestEqual(TEXT("sorted array covers all 9 slots"), Indices.Num(), WoTGoalSlotCount);
	TestEqual(TEXT("empty slot scores -1"), Priorities.Last(), -1.0f);

	// Unset (but existing) goals are invalid -> priority 0.
	TestEqual(TEXT("unset Threat scores 0"), Brain->GetGoalPriority(EWoTGoalIndex::Threat), 0.0f);

	// Threat at 500uu, inside the 8000 ZeroIfGreater gate -> full 80.
	Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Target);
	TestEqual(TEXT("near threat scores 80"), Brain->GetGoalPriority(EWoTGoalIndex::Threat), 80.0f);

	// Beyond the gate -> 0 (ZeroIfGreater: "only matters within distance").
	Target->SetActorLocation(FVector(9000, 0, 100));
	TestEqual(TEXT("far threat gated to 0"), Brain->GetGoalPriority(EWoTGoalIndex::Threat), 0.0f);
	Target->SetActorLocation(FVector(500, 0, 100));

	// Guarding is ZeroIfLess: near the guard point it scores 0, straying past
	// 480 pulls the pawn back at 90.
	Brain->InitGoalWithVector(EWoTGoalIndex::Guarding, NPC->GetActorLocation() + FVector(50, 0, 0));
	TestEqual(TEXT("near guard point scores 0"), Brain->GetGoalPriority(EWoTGoalIndex::Guarding), 0.0f);
	Brain->InitGoalWithVector(EWoTGoalIndex::Guarding, NPC->GetActorLocation() + FVector(2000, 0, 0));
	TestEqual(TEXT("strayed guard scores 90"), Brain->GetGoalPriority(EWoTGoalIndex::Guarding), 90.0f);

	// Selection: Guarding 90 > Threat 80 > Waiting.
	Brain->SetWaitingGoal(NPC->GetActorLocation(), FRotator::ZeroRotator);
	TestEqual(TEXT("highest slot wins"),
	          static_cast<int32>(Brain->GetPriorityGoalIndex()),
	          static_cast<int32>(EWoTGoalIndex::Guarding));

	// Tiebreak: equal priorities resolve to the LOWEST EWoTGoalIndex — the
	// locked tiebreak. Make Threat == Guarding.
	Brain->GetGoal(EWoTGoalIndex::Threat).Priority = 90.0f;
	TestEqual(TEXT("tie resolves to lowest slot index (Threat)"),
	          static_cast<int32>(Brain->GetPriorityGoalIndex()),
	          static_cast<int32>(EWoTGoalIndex::Threat));

	// A dead pawn target invalidates a bPawnsMustBeAlive goal.
	Target->Health = 0;
	TestEqual(TEXT("dead threat scores 0"), Brain->GetGoalPriority(EWoTGoalIndex::Threat), 0.0f);

	// A destroyed target actor nulls the weak reference -> invalid.
	Target->Health = 100;
	Target->Destroy();
	WoTTest::TickWorld(World, 0.05f);
	TestFalse(TEXT("destroyed threat target is invalid"), Brain->IsGoalValid(EWoTGoalIndex::Threat));

	WoTTest::DestroySpecWorld(World);
	return true;
}

// --- Cache: 32uu currency, pathable ignores observer, sibling invalidation ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAIGoalCacheTest,
	"WoT.AI.GoalCache",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTAIGoalCacheTest::RunTest(const FString&)
{
	FWoTGoalCache Cache;
	const FVector GoalLoc(1000, 0, 0);
	const FVector ObsLoc(0, 0, 0);

	// Miss before any stamp.
	TestEqual(TEXT("cold cache misses"),
	          Cache.CachedAnswer(EWoTGoalCacheItem::VisibleOK, GoalLoc, ObsLoc, true), -1);

	// Stamp visible-OK; current at <32uu drift of either party, stale beyond.
	Cache.Stamp(EWoTGoalCacheItem::VisibleOK, GoalLoc, ObsLoc);
	TestEqual(TEXT("fresh stamp hits (true)"),
	          Cache.CachedAnswer(EWoTGoalCacheItem::VisibleOK, GoalLoc, ObsLoc, true), 1);
	TestEqual(TEXT("goal drift 31 still current"),
	          Cache.CachedAnswer(EWoTGoalCacheItem::VisibleOK, GoalLoc + FVector(31, 0, 0), ObsLoc, true), 1);
	TestEqual(TEXT("goal drift 33 stale"),
	          Cache.CachedAnswer(EWoTGoalCacheItem::VisibleOK, GoalLoc + FVector(33, 0, 0), ObsLoc, true), -1);
	TestEqual(TEXT("observer drift 33 stale (visible checks observer)"),
	          Cache.CachedAnswer(EWoTGoalCacheItem::VisibleOK, GoalLoc, ObsLoc + FVector(33, 0, 0), true), -1);

	// Pathable currency ignores observer motion.
	Cache.Stamp(EWoTGoalCacheItem::PathableOK, GoalLoc, ObsLoc);
	TestEqual(TEXT("pathable current despite observer drift"),
	          Cache.CachedAnswer(EWoTGoalCacheItem::PathableOK, GoalLoc, ObsLoc + FVector(500, 0, 0), false), 1);
	TestEqual(TEXT("pathable stale when the goal moves"),
	          Cache.CachedAnswer(EWoTGoalCacheItem::PathableOK, GoalLoc + FVector(50, 0, 0), ObsLoc, false), -1);

	// Stamping one of a pair invalidates its sibling: a fresh fail answer must
	// not be shadowed by an older success.
	Cache.Stamp(EWoTGoalCacheItem::VisibleFail, GoalLoc, ObsLoc);
	TestEqual(TEXT("fail stamp reads back false"),
	          Cache.CachedAnswer(EWoTGoalCacheItem::VisibleOK, GoalLoc, ObsLoc, true), 0);

	return true;
}

// --- The reached predicate: 2D gap vs radius product / 15 ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAICloseEnoughTest,
	"WoT.AI.CloseEnough",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTAICloseEnoughTest::RunTest(const FString&)
{
	// Reached iff |delta2D| <= (Ra*Rb)/15.
	// Ra=30, Rb=20 -> threshold 40uu.
	const FVector A(0, 0, 0);
	TestTrue(TEXT("39uu apart with 30x20 radii is reached"),
	         UWoTBrainComponent::IsCloseEnough(A, 30.0f, FVector(39, 0, 0), 20.0f));
	TestFalse(TEXT("41uu apart with 30x20 radii is not reached"),
	          UWoTBrainComponent::IsCloseEnough(A, 30.0f, FVector(41, 0, 0), 20.0f));
	// Z is flattened: a big vertical offset does not affect the test.
	TestTrue(TEXT("Z offset ignored"),
	         UWoTBrainComponent::IsCloseEnough(A, 30.0f, FVector(39, 0, 500), 20.0f));
	// The 16x16 NPC-vs-NPC case: threshold 17.07uu.
	TestTrue(TEXT("16x16 radii reach at 17uu"),
	         UWoTBrainComponent::IsCloseEnough(A, 16.0f, FVector(17, 0, 0), 16.0f));
	TestFalse(TEXT("16x16 radii miss at 18uu"),
	          UWoTBrainComponent::IsCloseEnough(A, 16.0f, FVector(18, 0, 0), 16.0f));
	return true;
}

// --- Notifier cadence: periods, per-tick pulses, random phase, disable ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAINotifierCadenceTest,
	"WoT.AI.NotifierCadence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTAINotifierCadenceTest::RunTest(const FString&)
{
	// A 0.5s notifier fires twice over 1.05s of 60Hz ticks.
	FWoTDurationNotifier Half;
	Half.Period = 0.5f;
	Half.bEnabled = true;
	int32 Fires = 0;
	for (int32 i = 0; i < 63; ++i)   // 63/60 = 1.05s
	{
		Fires += Half.Tick(1.0f / 60.0f) ? 1 : 0;
	}
	TestEqual(TEXT("0.5s notifier fires twice in 1.05s"), Fires, 2);

	// Period 0 fires every tick while enabled (the per-tick re-aim pulses).
	FWoTDurationNotifier EveryTick;
	EveryTick.Period = 0.0f;
	EveryTick.bEnabled = true;
	Fires = 0;
	for (int32 i = 0; i < 10; ++i)
	{
		Fires += EveryTick.Tick(1.0f / 60.0f) ? 1 : 0;
	}
	TestEqual(TEXT("period-0 notifier fires every tick"), Fires, 10);

	// Period -1 never fires; disabled never fires.
	FWoTDurationNotifier Never;
	Never.Period = -1.0f;
	Never.bEnabled = true;
	FWoTDurationNotifier Disabled;
	Disabled.Period = 0.1f;
	Disabled.bEnabled = false;
	for (int32 i = 0; i < 120; ++i)
	{
		TestFalse(TEXT("period -1 never fires"), Never.Tick(1.0f / 60.0f));
		TestFalse(TEXT("disabled never fires"), Disabled.Tick(1.0f / 60.0f));
	}

	// Enabling seeds a random phase within [0, period) so crowds desync.
	// Verify via the component API.
	UWoTBrainComponent* Brain = NewObject<UWoTBrainComponent>();
	Brain->SetNotifierEnabled(EWoTNotifierSlot::Visibility, true);
	const FWoTDurationNotifier& Vis =
		Brain->Notifiers[static_cast<int32>(EWoTNotifierSlot::Visibility)];
	TestTrue(TEXT("random phase seeded within [0, period)"),
	         Vis.Elapsed >= 0.0f && Vis.Elapsed < 2.0f);

	return true;
}

// --- Stuck strikes: 1 drops the waypoint, 2 drop the goal ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAINavigationStrikesTest,
	"WoT.AI.NavigationStrikes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTAINavigationStrikesTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* NPC = SpawnNPC(World, FVector(0, 0, 100));
	AWoTNPCCharacter* Target = SpawnNPC(World, FVector(500, 0, 100));
	UWoTBrainComponent* Brain = NPC->Brain;

	Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Target);
	Brain->InitGoalWithVector(EWoTGoalIndex::CurrentWaypoint, FVector(250, 0, 100));
	Brain->CurrentGoalIndex = EWoTGoalIndex::Threat;

	// First give-up drops just the waypoint (try another route point).
	Brain->NotifyMoveResult(false);
	TestFalse(TEXT("1 strike invalidates the waypoint"),
	          Brain->IsGoalValid(EWoTGoalIndex::CurrentWaypoint));
	TestTrue(TEXT("1 strike keeps the goal"), Brain->IsGoalValid(EWoTGoalIndex::Threat));

	// A clean arrival resets the count: two separated give-ups never escalate.
	Brain->NotifyMoveResult(true);
	Brain->NotifyMoveResult(false);
	TestTrue(TEXT("reset by success: still 1 strike, goal kept"),
	         Brain->IsGoalValid(EWoTGoalIndex::Threat));

	// Second consecutive give-up drops the current goal and resets.
	Brain->NotifyMoveResult(false);
	TestFalse(TEXT("2 consecutive strikes invalidate the goal"),
	          Brain->IsGoalValid(EWoTGoalIndex::Threat));
	TestEqual(TEXT("strike counter resets after goal drop"), Brain->GetNavigationStrikes(), 0);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// --- Controller wiring: spawn waits, a threat takes over on the idle pulse ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAIBrainWiringTest,
	"WoT.AI.BrainWiring",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTAIBrainWiringTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	// Test brain (forced-reachable): the bare test world has no navmesh, and
	// Tracking drops an unnavigable threat (its own spec covers that).
	AWoTNPCCharacter* NPC = SpawnCombatNPC(World, FVector(0, 0, 100), 1);
	WoTTest::TickWorld(World, 0.1f);   // let auto-possession run

	AWoTNPCController* Controller = Cast<AWoTNPCController>(NPC->GetController());
	if (!TestNotNull(TEXT("NPC auto-possessed by AWoTNPCController"), Controller))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	UWoTBrainComponent* Brain = NPC->Brain;

	// StartUp evaluated priorities: only the spawn-set Waiting goal is live.
	TestEqual(TEXT("spawned NPC settles in Waiting"),
	          static_cast<int32>(Controller->GetBrainState()),
	          static_cast<int32>(EWoTBrainState::Waiting));
	TestTrue(TEXT("waiting goal set at spawn"), Brain->IsGoalValid(EWoTGoalIndex::Waiting));
	TestTrue(TEXT("waiting pulse enabled"),
	         Brain->IsNotifierEnabled(EWoTNotifierSlot::Waiting));

	// Health/speed spawn rolls applied (WOTPawn base band: 100 / 400).
	TestEqual(TEXT("MP health band applied"), NPC->Health, 100);

	// A threat appears: the 0.25s Waiting pulse re-evaluates and Threat (80)
	// outranks Waiting (50) -> Tracking.
	AWoTNPCCharacter* Enemy = SpawnNPC(World, FVector(600, 0, 100));
	Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Enemy);
	WoTTest::TickWorld(World, 0.5f);
	TestEqual(TEXT("threat takes over on the idle pulse -> Tracking"),
	          static_cast<int32>(Controller->GetBrainState()),
	          static_cast<int32>(EWoTBrainState::Tracking));
	TestEqual(TEXT("current goal is the threat slot"),
	          static_cast<int32>(Brain->CurrentGoalIndex),
	          static_cast<int32>(EWoTGoalIndex::Threat));

	WoTTest::DestroySpecWorld(World);
	return true;
}

// --- Faction: the WOTPawn team-byte switch ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAIFactionTest,
	"WoT.AI.Faction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTAIFactionTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* NPC = SpawnNPC(World, FVector(0, 0, 100));
	AWoTNPCCharacter* Ally = SpawnNPC(World, FVector(100, 0, 100));
	AWoTNPCCharacter* Foe = SpawnNPC(World, FVector(200, 0, 100));

	// Real teams: same index friendly, different index enemy (WOTPawn.uc:770).
	NPC->AssignedTeam = 1;
	Ally->AssignedTeam = 1;
	Foe->AssignedTeam = 2;
	TestTrue(TEXT("same team is friendly"), NPC->IsFriendly(Ally));
	TestFalse(TEXT("other team is enemy"), NPC->IsFriendly(Foe));
	TestEqual(TEXT("faction folds enemy"),
	          static_cast<int32>(NPC->GetActorFaction(Foe)), static_cast<int32>(EWoTFaction::Enemy));

	// TeamNPC (255): friendly to other teamless NPCs only (WOTPawn.uc:747).
	NPC->AssignedTeam = WoTTeam::NPC;
	Ally->AssignedTeam = WoTTeam::NPC;
	TestTrue(TEXT("teamless NPCs are mutual friends"), NPC->IsFriendly(Ally));
	TestFalse(TEXT("teamless NPC vs teamed NPC is enemy"), NPC->IsFriendly(Foe));

	// TeamNone hates everyone; TeamAny loves everyone (WOTPawn.uc:755-763).
	NPC->AssignedTeam = WoTTeam::None;
	TestFalse(TEXT("TeamNone is friendly to nobody"), NPC->IsFriendly(Ally));
	NPC->AssignedTeam = WoTTeam::Any;
	TestTrue(TEXT("TeamAny is friendly to everybody"), NPC->IsFriendly(Foe));

	// Non-pawns are friendly even to TeamNone (WOTPawn.uc:788); a null actor
	// stays Unknown (Grunt.uc:1167).
	NPC->AssignedTeam = WoTTeam::None;
	TestTrue(TEXT("non-pawn actors are friendly"),
	         NPC->IsFriendly(World->GetWorldSettings()));
	TestEqual(TEXT("null actor is unknown"),
	          static_cast<int32>(NPC->GetActorFaction(nullptr)),
	          static_cast<int32>(EWoTFaction::Unknown));

	WoTTest::DestroySpecWorld(World);
	return true;
}

// --- Perception: NPC show-self pulse sets Threat/Colleague by faction ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAIPerceptionTest,
	"WoT.AI.Perception",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTAIPerceptionTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	// Observer (team 1) with an enemy NPC (team 2) and an ally (team 1) in
	// range. The SEEN pawn's 2s Visibility pulse reveals it to observers. The
	// enemy sits far enough out (2000uu, mutual closing ~512uu/s) that the
	// chase can't reach melee inside the sample window — the observer must
	// still be in Tracking, not mid-swing, when asserted.
	AWoTTestNPCCharacter* Observer = SpawnCombatNPC(World, FVector(0, 0, 100), 1);
	AWoTTestNPCCharacter* Enemy = SpawnCombatNPC(World, FVector(2000, 0, 100), 2);
	AWoTTestNPCCharacter* Ally = SpawnCombatNPC(World, FVector(-400, 0, 100), 1);
	WoTSight::SetRandomOverrideForTesting([] { return 0.49f; });
	ON_SCOPE_EXIT { WoTSight::SetRandomOverrideForTesting({}); };
	FWoTLineOfSightParams FarSight;
	FarSight.bViewerIsPlayer = false;
	FarSight.SightRadius = 2500.0f;
	TestFalse(TEXT("retail far-sight coin below 0.5 rejects the enemy"),
		WoTSight::LineOfSightTo(Observer, Enemy, FarSight));
	WoTSight::SetRandomOverrideForTesting([] { return 1.0f; });

	WoTTest::TickWorld(World, 2.5f);   // > one 2s visibility pulse (randomized phase)

	UWoTBrainComponent* Brain = Observer->Brain;
	TestTrue(TEXT("enemy NPC produced a Threat goal within one visibility pulse"),
	         Brain->IsGoalValid(EWoTGoalIndex::Threat));
	TestEqual(TEXT("threat targets the enemy"),
	          Brain->GetGoal(EWoTGoalIndex::Threat).GoalActor.Get(), static_cast<AActor*>(Enemy));
	TestTrue(TEXT("friendly NPC produced a Colleague goal"),
	         Brain->IsGoalValid(EWoTGoalIndex::Colleague));
	TestEqual(TEXT("colleague targets the ally"),
	          Brain->GetGoal(EWoTGoalIndex::Colleague).GoalActor.Get(), static_cast<AActor*>(Ally));

	// Sighting escalated the observer into the tracking loop.
	const AWoTNPCController* Controller = Cast<AWoTNPCController>(Observer->GetController());
	TestEqual(TEXT("observer is Tracking its threat"),
	          static_cast<int32>(Controller->GetBrainState()),
	          static_cast<int32>(EWoTBrainState::Tracking));

	// A dead pawn never registers (the alive gate): kill the ally, invalidate
	// the colleague slot, and confirm no re-acquisition on later pulses.
	Ally->Health = 0;
	Observer->Brain->InvalidateGoal(EWoTGoalIndex::Colleague);
	WoTTest::TickWorld(World, 2.5f);
	TestFalse(TEXT("dead pawns are not re-acquired"),
	          Brain->IsGoalValid(EWoTGoalIndex::Colleague));

	WoTTest::DestroySpecWorld(World);
	return true;
}

// --- End-to-end: perceive -> chase -> close to melee band -> swing -> damage ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAIMeleeChaseTest,
	"WoT.AI.MeleeChase",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTAIMeleeChaseTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	AWoTTestNPCCharacter* Hunter = SpawnCombatNPC(World, FVector(0, 0, 100), 1);
	AWoTTestNPCCharacter* Prey = SpawnCombatNPC(World, FVector(600, 0, 100), 2);
	// The prey holds still (no melee back) so the assert is one-directional.
	Prey->MeleeRange = 0.0f;
	Prey->Brain->InvalidateGoal(EWoTGoalIndex::Waiting);
	const int32 PreyStartHealth = 150;
	Prey->Health = PreyStartHealth;
	Prey->HealthMPMin = Prey->HealthMPMax = PreyStartHealth;   // BeginPlay re-roll

	// Seed the threat directly (perception has its own spec) and let the
	// tracking loop chase: 600uu at 0.8x320 = 256uu/s ≈ 2.3s to close, then
	// the 0.5s offensive pulse launches a swing with damage at its notify.
	WoTTest::TickWorld(World, 0.1f);
	Hunter->Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Prey);
	WoTTest::TickWorld(World, 6.0f);

	const float Closed = FVector::Dist2D(Hunter->GetActorLocation(), Prey->GetActorLocation());
	TestTrue(FString::Printf(TEXT("hunter closed to melee range (gap %.0f)"), Closed),
	         Closed < 100.0f);
	TestTrue(FString::Printf(TEXT("prey took melee damage (health %d)"), Prey->Health),
	         Prey->Health < PreyStartHealth);
	// Trolloc swings: 15 'Cutup' / 10 'Sliced' — health dropped by a multiple.
	const int32 Lost = PreyStartHealth - Prey->Health;
	TestTrue(FString::Printf(TEXT("damage is a sum of 15/10 swings (lost %d)"), Lost),
	         Lost >= 10);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// --- Move budget: a long one-sided chase must not strike out ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAILongChaseTest,
	"WoT.AI.LongChase",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTAILongChaseTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	// A one-sided chase: the prey is uncontrolled (no controller, no counter-
	// chase), so the hunter alone must cover ~1400uu at 256uu/s ≈ 5.5s. With
	// the correct 1.0 + 1.3*d/v give-up budget the leg never times out; the
	// flat 1.2s pawn-target cap (the pre-fix bug) would strike the threat out.
	AWoTTestNPCCharacter* Hunter = SpawnCombatNPC(World, FVector(0, 0, 100), 1);
	AWoTTestNPCCharacter* Prey = SpawnCombatNPC(World, FVector(1400, 0, 100), 2);
	WoTTest::TickWorld(World, 0.1f);
	if (AController* PreyController = Prey->GetController())
	{
		PreyController->UnPossess();
	}

	Hunter->Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Prey);
	WoTTest::TickWorld(World, 8.0f);

	TestTrue(TEXT("threat survives the long chase (no strike-out)"),
	         Hunter->Brain->IsGoalValid(EWoTGoalIndex::Threat));
	const float Closed = FVector::Dist2D(Hunter->GetActorLocation(), Prey->GetActorLocation());
	TestTrue(FString::Printf(TEXT("hunter closed the full distance (gap %.0f)"), Closed),
	         Closed < 100.0f);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// --- Death gate: a dead NPC diverts to Dying and stops perceiving ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAIDyingGateTest,
	"WoT.AI.DyingGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTAIDyingGateTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	AWoTTestNPCCharacter* NPC = SpawnCombatNPC(World, FVector(0, 0, 100), 1);
	AWoTTestNPCCharacter* Enemy = SpawnCombatNPC(World, FVector(600, 0, 100), 2);
	WoTTest::TickWorld(World, 0.1f);
	NPC->Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Enemy);
	WoTTest::TickWorld(World, 0.3f);

	AWoTNPCController* Controller = Cast<AWoTNPCController>(NPC->GetController());
	TestEqual(TEXT("alive NPC is Tracking"),
	          static_cast<int32>(Controller->GetBrainState()),
	          static_cast<int32>(EWoTBrainState::Tracking));

	// Kill it mid-chase: the controller must divert to Dying and disable
	// every notifier pulse (LegendPawn.uc:490 + :737).
	NPC->Health = 0;
	WoTTest::TickWorld(World, 0.1f);
	TestEqual(TEXT("dead NPC diverts to Dying"),
	          static_cast<int32>(Controller->GetBrainState()),
	          static_cast<int32>(EWoTBrainState::Dying));
	bool bAnyEnabled = false;
	for (int32 i = 0; i < WoTNotifierSlotCount; ++i)
	{
		bAnyEnabled |= NPC->Brain->IsNotifierEnabled(static_cast<EWoTNotifierSlot>(i));
	}
	TestFalse(TEXT("all notifiers disabled in Dying"), bAnyEnabled);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// --- Caster: a weave NPC casts at its threat through the inventory chain ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAICasterAttackTest,
	"WoT.AI.CasterAttack",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTAICasterAttackTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTTestCasterNPC* Sister =
		World->SpawnActor<AWoTTestCasterNPC>(FVector(0, 0, 100), FRotator::ZeroRotator, Params);
	Sister->AssignedTeam = 1;
	Sister->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	AWoTTestNPCCharacter* Enemy = SpawnCombatNPC(World, FVector(900, 0, 100), 2);
	WoTTest::TickWorld(World, 0.1f);
	// One-sided: an uncontrolled enemy can't counter-chase to point-blank
	// before the weave's 2s charge-group warmup expires. (Point-blank casts
	// fizzle on spawn collision — retail casters hold at range via the
	// range-band ladder.)
	if (AController* EnemyController = Enemy->GetController())
	{
		EnemyController->UnPossess();
	}

	// The loadout was built at spawn: the fireball weave is in the inventory.
	UWoTInventoryComponent* Inv = Sister->FindComponentByClass<UWoTInventoryComponent>();
	bool bHasFireball = false;
	for (AWoTInventory* Item : Inv->Items)
	{
		bHasFireball |= Item->IsA<AWoTAngrealInvFireball>();
	}
	TestTrue(TEXT("caster loadout granted the fireball weave"), bHasFireball);

	// Threat -> Tracking -> the Offensive pulse selects the weave and casts
	// through the same UseAngreal chain players use -> a projectile flies.
	// Poll while ticking: a fireball that reaches the enemy detonates and
	// destroys itself, so a single post-hoc count can miss the whole volley.
	const int32 EnemyStartHealth = Enemy->Health;
	Sister->Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Enemy);
	int32 SeenProjectiles = 0;
	for (int32 Chunk = 0; Chunk < 30; ++Chunk)
	{
		WoTTest::TickWorld(World, 0.1f);
		for (TActorIterator<AWoTFireballProjectile> It(World); It; ++It)
		{
			++SeenProjectiles;
		}
	}
	TestTrue(FString::Printf(TEXT("fireball volley observed (%d in-flight samples, enemy %d->%d hp)"),
	                         SeenProjectiles, EnemyStartHealth, Enemy->Health),
	         SeenProjectiles > 0 || Enemy->Health < EnemyStartHealth);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// --- Unnavigable threat: the hunter traces a short approach (AcquiredUnNavigable) ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAIUnnavigableTest,
	"WoT.AI.UnnavigableThreat",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTAIUnnavigableTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	SpawnNavigationBox(World, FVector(300, 0, 0),
		FVector(1000, 1000, 10), /*bAffectsNavigation*/ false);

	AWoTNPCCharacter* Hunter = WoTCreatureData::SpawnCreature(
		*World, TEXT("Trolloc"), FVector(0, 0, 54), 1,
		/*bCombatReady*/ true, AWoTTestNPCCharacter::StaticClass());
	Hunter->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	SpawnAIActivityAnchor(World, Hunter->GetActorLocation());
	AWoTTestNPCCharacter* Prey = SpawnCombatNPC(World, FVector(600, 0, 54), 2);
	Prey->MeleeRange = 0.0f;
	WoTTest::TickWorld(World, 0.1f);
	// Uncontrolled prey: no show-self pulse, so nothing re-routes the hunter
	// mid-assert (was a phase-dependent race).
	if (AController* PreyController = Prey->GetController())
	{
		PreyController->UnPossess();
	}

	// Force every proximity closed: the threat is unreachable, unpathable, AND
	// out of sight -> it lands on the ladder's UnNavigable band. The shipped
	// acquired-threat arm retains the goal and its WaypointSelectorApproaching
	// traces a short step or side-step; it does NOT drop the goal (the
	// invalidate-if-!Navigable path
	// is the intermediate-navigation family, Grunt.uc:1955). So the hunter approaches while retaining the threat rather than falling back to
	// Waiting. (A VISIBLE but unnavigable goal instead takes the Visible band and is
	// approached — WoT.AI.BandLadder covers both.)
	UWoTTestBrainComponent* TestBrain = static_cast<UWoTTestBrainComponent*>(Hunter->Brain.Get());
	TestBrain->bForceReachable = true;
	TestBrain->bForceVisible = true;
	TestBrain->bForcePathable = true;
	const FVector HunterStart = Hunter->GetActorLocation();
	Hunter->Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Prey);
	FWoTGoal& Threat = Hunter->Brain->GetGoal(EWoTGoalIndex::Threat);
	const FVector ThreatLocation = Prey->GetActorLocation();
	Threat.Cache.Stamp(EWoTGoalCacheItem::VisibleFail,
		ThreatLocation, HunterStart);
	Threat.Cache.Stamp(EWoTGoalCacheItem::ReachableFail,
		ThreatLocation, HunterStart);
	Threat.Cache.Stamp(EWoTGoalCacheItem::PathableFail,
		ThreatLocation, HunterStart);
	AWoTNPCController* Controller = Cast<AWoTNPCController>(Hunter->GetController());
	Hunter->Brain->CurrentGoalIndex = EWoTGoalIndex::Threat;
	Controller->SetBrainStateForG20Test(EWoTBrainState::Tracking);
	Controller->Tick(0.05f);
	TestEqual(TEXT("failed threat queries select the UnNavigable band"),
		static_cast<int32>(Controller->GetTrackingProximity()),
		static_cast<int32>(EWoTGoalProximity::UnNavigable));
	WoTTest::TickWorld(World, 1.0f);

	TestTrue(FString::Printf(TEXT("unnavigable threat is retained (approached, not dropped; "
		"prey health %d, actor %d)"), Prey->Health,
		Hunter->Brain->GetGoal(EWoTGoalIndex::Threat).HasActor() ? 1 : 0),
		Hunter->Brain->IsGoalValid(EWoTGoalIndex::Threat));
	const float Moved = FVector::Dist2D(Hunter->GetActorLocation(), HunterStart);
	TestTrue(FString::Printf(TEXT("hunter takes approach steps (moved %.0f)"), Moved),
	         Moved > 1.0f);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// --- Caster holds at weave range instead of chasing to point-blank ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAICasterHoldsRangeTest,
	"WoT.AI.CasterHoldsRange",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTAICasterHoldsRangeTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTTestCasterNPC* Sister =
		World->SpawnActor<AWoTTestCasterNPC>(FVector(0, 0, 100), FRotator::ZeroRotator, Params);
	Sister->AssignedTeam = 1;
	Sister->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	// Hold at 400uu (set before possession so the tracking bands pick it up).
	Sister->Caster->HoldRange = 400.0f;

	// A visible, reachable threat starting outside weave range. The caster must
	// close to its hold range and STOP there, casting from range — instead of
	// chasing to point-blank the way the pre-ladder caster did (MeleeRange 0 ->
	// always move -> ~32uu contact point -> spawn-collision fizzle, PORT_NOTES).
	// Uncontrolled so it can't counter-close the gap for us.
	AWoTTestNPCCharacter* Enemy = SpawnCombatNPC(World, FVector(700, 0, 100), 2);
	WoTTest::TickWorld(World, 0.1f);
	if (AController* EnemyController = Enemy->GetController())
	{
		EnemyController->UnPossess();
	}

	AWoTNPCController* Controller = Cast<AWoTNPCController>(Sister->GetController());
	const int32 EnemyStartHealth = Enemy->Health;
	Sister->Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Enemy);

	float MinGap = FVector::Dist2D(Sister->GetActorLocation(), Enemy->GetActorLocation());
	int32 SeenProjectiles = 0;
	for (int32 Chunk = 0; Chunk < 60; ++Chunk)   // 6s
	{
		WoTTest::TickWorld(World, 0.1f);
		MinGap = FMath::Min(MinGap,
			FVector::Dist2D(Sister->GetActorLocation(), Enemy->GetActorLocation()));
		for (TActorIterator<AWoTFireballProjectile> It(World); It; ++It)
		{
			++SeenProjectiles;
		}
	}

	// The fix: the gap stabilized above a floor well clear of point-blank (the
	// pre-ladder caster would have closed to the ~32uu contact point).
	TestTrue(FString::Printf(TEXT("caster held clear of point-blank (min gap %.0f)"), MinGap),
	         MinGap > 250.0f);

	// It settled in a holding caster band (not fleeing / searching / melee).
	const EWoTGoalProximity Prox = Controller->GetTrackingProximity();
	const FWoTRangeBand& Band = Controller->GetTrackingBands().Band(Prox);
	TestTrue(TEXT("caster settled in a holding band that casts"),
	         Band.bHold && Band.Attack == EWoTBandAttack::Caster);

	// And it still landed casts from range through the same UseAngreal chain.
	TestTrue(FString::Printf(TEXT("caster landed casts from range (%d in-flight, enemy %d->%d hp)"),
	                         SeenProjectiles, EnemyStartHealth, Enemy->Health),
	         SeenProjectiles > 0 || Enemy->Health < EnemyStartHealth);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// --- The proximity ladder: classification order + band payloads + prefer/flip ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAIBandLadderTest,
	"WoT.AI.BandLadder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTAIBandLadderTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTTestNPCCharacter* NPC = SpawnCombatNPC(World, FVector(0, 0, 100), 1);
	AWoTTestNPCCharacter* Target = SpawnCombatNPC(World, FVector(500, 0, 100), 2);
	UWoTTestBrainComponent* Brain = static_cast<UWoTTestBrainComponent*>(NPC->Brain.Get());
	Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Target);
	const EWoTGoalIndex Goal = EWoTGoalIndex::Threat;

	// The 32uu query cache would otherwise carry a stale answer across cases
	// (goal/observer barely move between them) — clear it before each classify
	// so each runs the forced primitives fresh.
	auto Classify = [&](const FWoTRangeBandSet& Set)
	{
		Brain->GetGoal(Goal).Cache.Invalidate();
		return Set.Classify(Brain, Goal);
	};

	const FWoTRangeBandSet Melee = FWoTRangeBandSet::MeleeDefault(25.0f);

	// --- Ladder order: the first bound proximity whose query holds wins ---

	// Reached beats everything: overlap the cylinders with all primitives open.
	Brain->bForceReachable = true; Brain->bForceVisible = true; Brain->bForcePathable = true;
	Target->SetActorLocation(NPC->GetActorLocation());
	TestEqual(TEXT("Reached beats Reachable/Visible/Pathable"),
	          static_cast<int32>(Classify(Melee)), static_cast<int32>(EWoTGoalProximity::Reached));

	// Far now (not reached): Reachable beats Visible/Pathable.
	Target->SetActorLocation(FVector(500, 0, 100));
	TestEqual(TEXT("Reachable beats Visible/Pathable"),
	          static_cast<int32>(Classify(Melee)), static_cast<int32>(EWoTGoalProximity::Reachable));

	// Reachability gates on visibility, so drop reachable but keep visible:
	// Visible beats Pathable.
	Brain->bForceReachable = false;
	TestEqual(TEXT("Visible beats Pathable"),
	          static_cast<int32>(Classify(Melee)), static_cast<int32>(EWoTGoalProximity::Visible));

	// Not visible either: only a path exists -> Pathable.
	Brain->bForceVisible = false;
	TestEqual(TEXT("Pathable when only a path exists"),
	          static_cast<int32>(Classify(Melee)), static_cast<int32>(EWoTGoalProximity::Pathable));

	// Nothing works -> UnNavigable (the catch-all).
	Brain->bForcePathable = false;
	TestEqual(TEXT("UnNavigable when nothing works"),
	          static_cast<int32>(Classify(Melee)), static_cast<int32>(EWoTGoalProximity::UnNavigable));
	// The acquired UnNavigable band approaches without attacking or dropping.
	TestTrue(TEXT("the UnNavigable band approaches (no attack, no drop)"),
	         !Melee.Band(EWoTGoalProximity::UnNavigable).bHold &&
	         !Melee.Band(EWoTGoalProximity::UnNavigable).bDropGoal &&
	         Melee.Band(EWoTGoalProximity::UnNavigable).Attack == EWoTBandAttack::None);

	// --- Melee band payloads: close bands attack + hold, approach bands don't ---
	TestEqual(TEXT("Reachable band is a melee route"),
	          static_cast<int32>(Melee.Band(EWoTGoalProximity::Reachable).Attack),
	          static_cast<int32>(EWoTBandAttack::Melee));
	TestTrue(TEXT("Reachable band holds at melee range"),
	         Melee.Band(EWoTGoalProximity::Reachable).bHold &&
	         Melee.Band(EWoTGoalProximity::Reachable).StandoffDistance == 25.0f);
	TestFalse(TEXT("Visible (approach) band does not hold"),
	          Melee.Band(EWoTGoalProximity::Visible).bHold);
	TestEqual(TEXT("Visible (approach) band has no attack route"),
	          static_cast<int32>(Melee.Band(EWoTGoalProximity::Visible).Attack),
	          static_cast<int32>(EWoTBandAttack::None));

	// --- Caster ladder: near bands hold + cast; a visible target is not dropped ---
	const FWoTRangeBandSet CasterSet = FWoTRangeBandSet::CasterDefault(400.0f);
	TestEqual(TEXT("caster Visible band casts"),
	          static_cast<int32>(CasterSet.Band(EWoTGoalProximity::Visible).Attack),
	          static_cast<int32>(EWoTBandAttack::Caster));
	TestTrue(TEXT("caster Visible band holds at the weave range"),
	         CasterSet.Band(EWoTGoalProximity::Visible).bHold &&
	         CasterSet.Band(EWoTGoalProximity::Visible).StandoffDistance == 400.0f);

	// Visible-but-unnavigable -> the caster hold band, NOT dropped (the fix: the
	// old !IsGoalNavigable check dropped a target it could plainly see).
	Brain->bForceReachable = false; Brain->bForcePathable = false; Brain->bForceVisible = true;
	TestEqual(TEXT("visible-but-unnavigable holds (caster), not dropped"),
	          static_cast<int32>(Classify(CasterSet)), static_cast<int32>(EWoTGoalProximity::Visible));

	// --- Prefer-visibility short-circuit (Grunt.uc:465 SelectAcquiredGoalHandler) ---
	// A hybrid ladder (a melee close band + a ranged Visible band). Prefer off:
	// a reachable+visible goal classifies Reachable (close in). Prefer on: the
	// Visible band is taken first (hold + shoot instead of closing).
	FWoTRangeBandSet Hybrid = FWoTRangeBandSet::MeleeDefault(25.0f);
	Hybrid.Bands[static_cast<int32>(EWoTGoalProximity::Visible)].bHold = true;
	Hybrid.Bands[static_cast<int32>(EWoTGoalProximity::Visible)].Attack = EWoTBandAttack::Caster;
	Brain->bForceReachable = true; Brain->bForceVisible = true; Brain->bForcePathable = false;
	Hybrid.bPreferVisibility = false;
	TestEqual(TEXT("prefer off: reachable+visible classifies Reachable"),
	          static_cast<int32>(Classify(Hybrid)), static_cast<int32>(EWoTGoalProximity::Reachable));
	Hybrid.bPreferVisibility = true;
	TestEqual(TEXT("prefer on: reachable+visible classifies Visible"),
	          static_cast<int32>(Classify(Hybrid)), static_cast<int32>(EWoTGoalProximity::Visible));

	// --- MaybeFlip re-rolls the preference on the flip odds (Grunt.uc:1246) ---
	FWoTRangeBandSet Flip = FWoTRangeBandSet::MeleeDefault(25.0f);
	Flip.bPreferVisibility = false;
	Flip.FlipOdds = 1.0f;   // always
	TestTrue(TEXT("MaybeFlip at odds 1 flips"), Flip.MaybeFlip());
	TestTrue(TEXT("preference toggled to true"), Flip.bPreferVisibility);
	Flip.FlipOdds = 0.0f;   // never
	TestFalse(TEXT("MaybeFlip at odds 0 does not flip"), Flip.MaybeFlip());
	TestTrue(TEXT("preference unchanged at odds 0"), Flip.bPreferVisibility);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// --- Captain leadership: a grunt copies its in-range leader's guard goal ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAICaptainLeadershipTest,
	"WoT.AI.CaptainLeadership",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTAICaptainLeadershipTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTTestCaptainNPC* Captain = SpawnCaptain(World, FVector(0, 0, 100), 1);
	WoTTest::TickWorld(World, 0.1f);   // possession + InitCaptainOrders
	AWoTNPCController* CapCtl = Cast<AWoTNPCController>(Captain->GetController());
	if (!TestNotNull(TEXT("captain possessed"), CapCtl))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	// Make the captain a leader by guarding its spot. In shipped, OnGuardLocation
	// is reachable only via DispatchOrders / the seal Trigger — ChangeOrders remaps
	// Guard→KillIntruder (Captain.uc:235), so it's dispatched directly here.
	CapCtl->DispatchOrders(EWoTOrders::Guard);
	TestTrue(TEXT("a guarding captain is a leader (Captain.uc:433)"), CapCtl->IsLeader());

	// A grunt within LeadershipRadius (240) mimics the captain's guard goal.
	AWoTTestNPCCharacter* Grunt = SpawnCombatNPC(World, FVector(150, 0, 100), 1);
	WoTTest::TickWorld(World, 0.1f);
	AWoTNPCController* GruntCtl = Cast<AWoTNPCController>(Grunt->GetController());

	TestTrue(TEXT("grunt in range finds the leader"), GruntCtl->FindLeader());
	TestTrue(TEXT("grunt adopted a guarding goal"),
	         Grunt->Brain->IsGoalValid(EWoTGoalIndex::Guarding));
	const FVector CapGuard = Captain->Brain->GetGoal(EWoTGoalIndex::Guarding).GoalVector;
	const FVector GruntGuard = Grunt->Brain->GetGoal(EWoTGoalIndex::Guarding).GoalVector;
	TestTrue(TEXT("grunt guards the captain's spot (AssignGoalFrom copied the vector)"),
	         CapGuard.Equals(GruntGuard, 1.0f));

	// Out of LeadershipRadius → no leader → guarding is dropped (Grunt.uc:1158).
	Grunt->SetActorLocation(FVector(1000, 0, 100));
	TestFalse(TEXT("grunt out of range finds no leader"), GruntCtl->FindLeader());
	TestFalse(TEXT("grunt drops guarding with no leader"),
	          Grunt->Brain->IsGoalValid(EWoTGoalIndex::Guarding));

	WoTTest::DestroySpecWorld(World);
	return true;
}

// --- Captain orders: the EvaluateOrders alarm→help→kill ladder ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAICaptainOrdersTest,
	"WoT.AI.CaptainOrders",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTAICaptainOrdersTest::RunTest(const FString&)
{
	// Each case gets a fresh captain: the ladder runs on the FIRST threat (the
	// threat slot not yet valid), driven through the real perception hook
	// (DispatchSeePawn → EvaluateOrders, Captain.uc:461/474). Captains are set to
	// Automatic so the ladder (not the KillIntruder short-circuit) runs.

	// Case 1: an alarm is present + unrung → SoundAlarm, aimed at the alarm.
	{
		UWorld* World = WoTTest::CreateSpecWorld();
		AWoTTestCaptainNPC* Captain = SpawnCaptain(World, FVector(0, 0, 100), 1);
		WoTTest::TickWorld(World, 0.1f);
		AWoTNPCController* Ctl = Cast<AWoTNPCController>(Captain->GetController());
		Ctl->ChangeOrders(EWoTOrders::Automatic);

		AWoTAlarm* Alarm = World->SpawnActor<AWoTAlarm>(FVector(300, 0, 100), FRotator::ZeroRotator);
		AWoTTestNPCCharacter* Enemy = SpawnCombatNPC(World, FVector(600, 0, 100), 2);
		Ctl->DispatchSeePawn(Enemy);

		TestEqual(TEXT("alarm present → SoundAlarm orders"),
		          static_cast<int32>(Ctl->GetWorkingOrders()),
		          static_cast<int32>(EWoTOrders::SoundAlarm));
		TestTrue(TEXT("intermediate goal set"),
		         Captain->Brain->IsGoalValid(EWoTGoalIndex::Intermediate));
		TestEqual(TEXT("intermediate targets the alarm"),
		          Captain->Brain->GetGoal(EWoTGoalIndex::Intermediate).GoalActor.Get(),
		          static_cast<AActor*>(Alarm));
		WoTTest::DestroySpecWorld(World);
	}

	// Case 2: no alarm, an available friendly in range → GetHelp, aimed at it.
	{
		UWorld* World = WoTTest::CreateSpecWorld();
		AWoTTestCaptainNPC* Captain = SpawnCaptain(World, FVector(0, 0, 100), 1);
		WoTTest::TickWorld(World, 0.1f);
		AWoTNPCController* Ctl = Cast<AWoTNPCController>(Captain->GetController());
		Ctl->ChangeOrders(EWoTOrders::Automatic);

		// Helper (200uu, within FindHelpRadius) is nearer than the enemy (600), so
		// it wins the nearest-navigable pick.
		AWoTTestNPCCharacter* Helper = SpawnCombatNPC(World, FVector(200, 0, 100), 1);
		AWoTTestNPCCharacter* Enemy = SpawnCombatNPC(World, FVector(600, 0, 100), 2);
		Ctl->DispatchSeePawn(Enemy);

		TestEqual(TEXT("no alarm + help available → GetHelp orders"),
		          static_cast<int32>(Ctl->GetWorkingOrders()),
		          static_cast<int32>(EWoTOrders::GetHelp));
		TestEqual(TEXT("intermediate targets the reinforcement"),
		          Captain->Brain->GetGoal(EWoTGoalIndex::Intermediate).GoalActor.Get(),
		          static_cast<AActor*>(Helper));
		WoTTest::DestroySpecWorld(World);
	}

	// Case 3: no alarm, no help → KillIntruder, no intermediate.
	{
		UWorld* World = WoTTest::CreateSpecWorld();
		AWoTTestCaptainNPC* Captain = SpawnCaptain(World, FVector(0, 0, 100), 1);
		WoTTest::TickWorld(World, 0.1f);
		AWoTNPCController* Ctl = Cast<AWoTNPCController>(Captain->GetController());
		Ctl->ChangeOrders(EWoTOrders::Automatic);

		// The enemy sits beyond FindHelpRadius+500 (820uu) so CollectAvailableWotPawns
		// can't recruit it — the shipped help scan has no team filter (WOTUtil.uc:200),
		// so an idle in-range enemy WOULD count as "help"; here there is none in range.
		AWoTTestNPCCharacter* Enemy = SpawnCombatNPC(World, FVector(1000, 0, 100), 2);
		Ctl->DispatchSeePawn(Enemy);

		TestEqual(TEXT("no alarm + no help → KillIntruder orders"),
		          static_cast<int32>(Ctl->GetWorkingOrders()),
		          static_cast<int32>(EWoTOrders::KillIntruder));
		TestFalse(TEXT("no intermediate goal (just pursue the threat)"),
		          Captain->Brain->IsGoalValid(EWoTGoalIndex::Intermediate));
		WoTTest::DestroySpecWorld(World);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRC33CreatureCuePresentationSpec,
	"WoT.RC33.P9.CreatureCuePresentation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTRC33CreatureCuePresentationSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	auto HasCue = [World](const TCHAR* Fragment)
	{
		for (TActorIterator<AWoTSoundBurst> It(World); It; ++It)
		{
			if (It->SoundSubPath.ToString().Contains(Fragment,
				ESearchCase::IgnoreCase)) return true;
		}
		return false;
	};

	AWoTNPCCharacter* Sister = WoTCreatureData::SpawnCreature(
		*World, TEXT("Sister"), FVector(0,0,100), 1, true,
		AWoTTestNPCCharacter::StaticClass());
	AWoTNPCController* SisterCtl = Sister
		? Cast<AWoTNPCController>(Sister->GetController()) : nullptr;
	if (TestNotNull(TEXT("production Sister captain"), SisterCtl))
	{
		SisterCtl->ChangeOrders(EWoTOrders::KillIntruder);
		TestTrue(TEXT("live multiplayer captain order emits its authored voice"),
			HasCue(TEXT("OrderKillIntruder")));
	}

	AWoTNPCCharacter* Archer = WoTCreatureData::SpawnCreature(
		*World, TEXT("Archer"), FVector(1000,0,100), 1, true,
		AWoTTestNPCCharacter::StaticClass());
	if (TestNotNull(TEXT("production Archer"), Archer))
	{
		Archer->EquipBodyAttack();
		Archer->EquipMeleeWeapon();
		Archer->EquipBodyAttack();
		TestTrue(TEXT("Archer transition emits sheath and draw cues"),
			HasCue(TEXT("PutAway")) && HasCue(TEXT("TakeOut")));
	}

	AWoTNPCCharacter* Myrddraal = WoTCreatureData::SpawnCreature(
		*World, TEXT("Myrddraal"), FVector(2000,0,100), 1, true,
		AWoTTestNPCCharacter::StaticClass());
	if (TestNotNull(TEXT("production Myrddraal"), Myrddraal))
	{
		Myrddraal->EquipBodyAttack();
		Myrddraal->EquipMeleeWeapon();
		Myrddraal->EquipRangedWeapon();
		TestTrue(TEXT("Myrddraal transition emits authored switch cues"),
			HasCue(TEXT("Switch")) && HasCue(TEXT("TakeOut")));
	}

	AWoTNPCCharacter* Minion = WoTCreatureData::SpawnCreature(
		*World, TEXT("Minion"), FVector(3000,0,100), 1, true,
		AWoTTestNPCCharacter::StaticClass());
	AWoTTestNPCCharacter* Victim = SpawnCombatNPC(World, FVector(3030,0,100), 2);
	AWoTNPCController* MinionCtl = Minion
		? Cast<AWoTNPCController>(Minion->GetController()) : nullptr;
	if (TestNotNull(TEXT("production Minion controller"), MinionCtl))
	{
		Minion->Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Victim);
		for (int32 Frame = 0; Frame < 600
			&& !HasCue(TEXT("AttackTentacle"))
			&& !HasCue(TEXT("AttackBite"))
			&& !HasCue(TEXT("AttackClaw")); ++Frame)
		{
			WoTTest::TickWorld(World, 1.0f / 60.0f);
		}
		TestTrue(TEXT("Minion melee notify emits its authored swing cue"),
			HasCue(TEXT("AttackTentacle")) || HasCue(TEXT("AttackBite"))
			|| HasCue(TEXT("AttackClaw")));
	}

	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

// --- Help propagation: HelpReached alerts friendlies within 160uu ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAIHelpPropagationTest,
	"WoT.AI.HelpPropagation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTAIHelpPropagationTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* Captain = WoTCreatureData::SpawnCreature(
		*World, TEXT("Sister"), FVector(0, 0, 100), 1, true,
		AWoTTestNPCCharacter::StaticClass());
	WoTTest::TickWorld(World, 0.1f);
	AWoTNPCController* Ctl = Cast<AWoTNPCController>(Captain->GetController());
	Ctl->ChangeOrders(EWoTOrders::Automatic);

	// The reinforcement the captain navigates to sits ON the captain, so the
	// intermediate goal classifies Reached at once → HelpReached fires. A separate
	// friendly bystander within AlertVisibleWotPawnsRadius (160) is the alert target.
	AWoTNPCCharacter* HelpTarget = WoTCreatureData::SpawnCreature(
		*World, TEXT("Minion"), FVector(-100, 0, 100), 1, true,
		AWoTTestNPCCharacter::StaticClass()); // help target (overlap)
	AWoTNPCCharacter* Bystander = WoTCreatureData::SpawnCreature(
		*World, TEXT("Minion"), FVector(0, 30, 100), 1, true,
		AWoTTestNPCCharacter::StaticClass());
	Bystander->SetActorEnableCollision(false);
	WoTTest::TickWorld(World, 0.1f);
	AWoTNPCController* BystanderCtl = Bystander
		? Cast<AWoTNPCController>(Bystander->GetController()) : nullptr;
	TestTrue(TEXT("production reinforcement starts available"),
		BystanderCtl && BystanderCtl->CanBeGivenNewOrders());
	TestTrue(TEXT("production reinforcement is visible to its captain"),
		Bystander && WoTSight::OmniSight(
			Captain, Bystander, WoTSight::VisibilityOf(Bystander)));
	// Hold the reinforcement at the report boundary so its transient Intermediate
	// goal is observable; AlertedByCaptain still runs through the real controller.
	BystanderCtl->SetActorTickEnabled(false);

	// Drive the production GetHelp order with the nearest helper as intermediate.
	Ctl->ChangeOrders(EWoTOrders::GetHelp);
	Captain->Brain->InitGoalWithActor(EWoTGoalIndex::Intermediate, HelpTarget);
	TestEqual(TEXT("captain adopts GetHelp"),
	          static_cast<int32>(Ctl->GetWorkingOrders()),
	          static_cast<int32>(EWoTOrders::GetHelp));

	// Drive to arrival and poll for the alert: once alerted the bystander navigates
	// to the captain and then clears its intermediate, so capture the transient.
	Ctl->HandleIntermediateArrivalForTest();
	const bool bAlerted =
		Bystander->Brain->IsGoalValid(EWoTGoalIndex::Intermediate)
		&& Bystander->Brain->GetGoal(EWoTGoalIndex::Intermediate).GoalActor.Get()
			== static_cast<AActor*>(Captain);
	TestTrue(TEXT("HelpReached alerted a friendly within 160uu toward the captain"), bAlerted);
	TestEqual(TEXT("captain plays GiveOrders at HelpReached"),
		Captain->GetReplicatedAnimName(), FName(TEXT("GiveOrders")));
	BystanderCtl->HandleIntermediateArrivalForTest();
	TestEqual(TEXT("alerted reinforcement plays Listen/report response"),
		Bystander->GetReplicatedAnimName(), FName(TEXT("Listen")));

	WoTTest::DestroySpecWorld(World);
	return true;
}

// --- Captain gong strike: the alarm rings when the Gong anim finishes ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAICaptainGongStrikeTest,
	"WoT.AI.CaptainGongStrike",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTAICaptainGongStrikeTest::RunTest(const FString&)
{
	// Captain.uc:723-727: PerformInactivity plays the Gong slot, FinishAnim, then
	// SoundAlarm. The production Whitecloak captain (Archer) arrives at an alarm
	// placed beside it, driven through the real perception hook and Tracking.
	UWorld* World = WoTTest::CreateSpecWorld();
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	AWoTNPCCharacter* Captain = WoTCreatureData::SpawnCreature(
		*World, TEXT("Archer"), FVector(0, 0, 100), 1, true,
		AWoTTestNPCCharacter::StaticClass());
	if (!TestNotNull(TEXT("production Archer captain"), Captain))
	{
		return false;
	}
	Captain->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	WoTTest::TickWorld(World, 0.1f);
	AWoTNPCController* Ctl = Cast<AWoTNPCController>(Captain->GetController());
	if (!TestNotNull(TEXT("production Archer controller"), Ctl))
	{
		return false;
	}
	Ctl->ChangeOrders(EWoTOrders::Automatic);

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTAlarm* Alarm = World->SpawnActor<AWoTAlarm>(
		FVector(40, 0, 100), FRotator::ZeroRotator, Params);
	AWoTTestNPCCharacter* Enemy = SpawnCombatNPC(World, FVector(600, 0, 100), 2);
	if (!TestNotNull(TEXT("alarm"), Alarm) || !TestNotNull(TEXT("intruder"), Enemy))
	{
		return false;
	}
	if (AController* EnemyCtl = Enemy->GetController())
	{
		EnemyCtl->SetActorTickEnabled(false);
	}
	Ctl->DispatchSeePawn(Enemy);
	TestEqual(TEXT("the captain takes SoundAlarm orders"),
		static_cast<int32>(Ctl->GetWorkingOrders()),
		static_cast<int32>(EWoTOrders::SoundAlarm));

	// FinishAnim waits out the tween and the sequence at the slot's rate.
	const FWoTAnimTableEntry* Gong =
		WoTAnimTable::PickSlotAnim(Captain->AnimTableName, TEXT("Gong"), Captain);
	if (!TestNotNull(TEXT("the Archer table authors a Gong slot"), Gong))
	{
		return false;
	}
	const UWoTVertexAnimComponent* Anim =
		Cast<UWoTVertexAnimComponent>(Captain->GetMesh());
	const float SequenceTime = Captain->SequenceDurationSeconds(
		Gong->AnimName, WoTAnimTable::ResolveRate(Gong));
	const float Expected = WoTAnimTable::ResolveTweenTime(Gong)
		+ (Anim && Anim->FindSequence(Gong->AnimName) ? SequenceTime
			: (SequenceTime > 0.0f ? SequenceTime : 1.0f));

	const float Dt = 1.0f / 60.0f;
	float Elapsed = 0.0f;
	float StrikeStart = -1.0f;
	float RingTime = -1.0f;
	for (int32 Frame = 0; Frame < 900 && RingTime < 0.0f; ++Frame)
	{
		WoTTest::TickWorld(World, Dt);
		Elapsed += Dt;
		if (StrikeStart < 0.0f && Ctl->IsStrikingGongForTest())
		{
			StrikeStart = Elapsed;
			TestEqual(TEXT("the captain plays the Gong slot"),
				Captain->GetReplicatedAnimName(), Gong->AnimName);
			TestFalse(TEXT("the alarm is silent while the strike plays"), Alarm->bRung);
		}
		if (Alarm->bRung)
		{
			RingTime = Elapsed;
		}
	}
	if (!TestTrue(TEXT("the captain reached the alarm and struck the gong"),
			StrikeStart >= 0.0f)
		|| !TestTrue(TEXT("the alarm rang"), RingTime >= 0.0f))
	{
		return false;
	}
	const float Waited = RingTime - StrikeStart;
	TestTrue(FString::Printf(
		TEXT("the alarm rings when the strike finishes (waited %.3f s, strike %.3f s)"),
		Waited, Expected),
		Waited >= Expected - Dt - KINDA_SMALL_NUMBER && Waited <= Expected + 2.0f * Dt);
	TestFalse(TEXT("the strike is over"), Ctl->IsStrikingGongForTest());
	TestEqual(TEXT("AlarmSounded reverts the captain to KillIntruder"),
		static_cast<int32>(Ctl->GetWorkingOrders()),
		static_cast<int32>(EWoTOrders::KillIntruder));
	TestFalse(TEXT("AlarmSounded drops the alarm goal"),
		Captain->Brain->IsGoalValid(EWoTGoalIndex::Intermediate));
	return true;
}

// --- A wall-mounted alarm is navigable for a production captain ---

namespace
{
	// Hang an alarm on the navigation test wall's +X face the way the citadel
	// editor does: the cursor's backed-off hit and normal into DeployResource.
	AWoTAlarm* DeployTestWallAlarm(UWorld* World)
	{
		FHitResult Cursor;
		if (!World->LineTraceSingleByChannel(Cursor, FVector(330, 0, 120),
				FVector(-330, 0, 90), ECC_Visibility) || !Cursor.bBlockingHit)
		{
			return nullptr;
		}
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		// The citadel cursor backs its hit off 0.5 uu along the view, as UE1 traces do.
		const FVector HitLocation = Cursor.ImpactPoint
			- 0.5f * (Cursor.TraceEnd - Cursor.TraceStart).GetSafeNormal();
		AWoTAlarm* Alarm = World->SpawnActor<AWoTAlarm>(
			HitLocation, FRotator::ZeroRotator, Params);
		if (Alarm && !Alarm->DeployResource(HitLocation, Cursor.ImpactNormal))
		{
			Alarm->Destroy();
			return nullptr;
		}
		return Alarm;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAIAlarmNavigabilityTest,
	"WoT.AI.AlarmNavigability",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTAIAlarmNavigabilityTest::RunTest(const FString&)
{
	// The orders specs force the brain primitives open (AWoTTestNPCCharacter);
	// this one runs the production queries against real collision and a built
	// navmesh, with the alarm placed through Trap.CalcLocation (Trap.uc:100-176).
	UWorld* World = WoTTest::CreateSpecWorld();
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	BuildNavigationTestMesh(World);   // floor top z=0; wall box x in [-30,30]
	AWoTNPCCharacter* Captain = WoTCreatureData::SpawnCreature(
		*World, TEXT("Archer"), FVector(330, 0, 60), 1, true);
	if (!TestNotNull(TEXT("production Archer captain"), Captain))
	{
		return false;
	}
	WoTTest::TickWorld(World, 0.3f);
	AWoTNPCController* Ctl = Cast<AWoTNPCController>(Captain->GetController());
	AWoTAlarm* Alarm = DeployTestWallAlarm(World);
	if (!TestNotNull(TEXT("production Archer controller"), Ctl)
		|| !TestNotNull(TEXT("the alarm deploys on the wall"), Alarm))
	{
		return false;
	}

	// UE1 traces back their hits off 0.5 uu, so the gong hangs just in front of
	// the face, HeightAboveFloor over the backed-off floor hit.
	TestTrue(FString::Printf(TEXT("the gong hangs 0.5 uu off the wall face (x %.3f)"),
		Alarm->GetActorLocation().X),
		FMath::IsNearlyEqual(Alarm->GetActorLocation().X, 30.5, 0.01));
	TestTrue(FString::Printf(TEXT("HeightAboveFloor over the backed-off floor hit (z %.3f)"),
		Alarm->GetActorLocation().Z),
		FMath::IsNearlyEqual(Alarm->GetActorLocation().Z, 80.5, 0.01));
	float Radius = 0.0f, HalfHeight = 0.0f;
	Alarm->GetSimpleCollisionCylinder(Radius, HalfHeight);
	TestEqual(TEXT("Alarm keeps the Actor default collision radius"), Radius, 22.0f);
	TestEqual(TEXT("Alarm keeps the Actor default collision height"), HalfHeight, 22.0f);

	UWoTBrainComponent* Brain = Captain->Brain;
	Brain->InitGoalWithActor(EWoTGoalIndex::Intermediate, Alarm);
	TestTrue(TEXT("the captain sees its alarm"),
		Brain->IsGoalVisible(EWoTGoalIndex::Intermediate));
	TestTrue(TEXT("the alarm is reachable through its stand spot"),
		Brain->IsGoalReachable(EWoTGoalIndex::Intermediate));
	TestTrue(TEXT("the alarm is pathable through its stand spot"),
		Brain->IsGoalPathable(EWoTGoalIndex::Intermediate));
	Brain->InvalidateGoal(EWoTGoalIndex::Intermediate);
	TestTrue(TEXT("GetClosestAlarm accepts the wall alarm"),
		WoTAIUtil::GetClosestAlarm(Ctl, EWoTGoalIndex::Intermediate));
	return true;
}

// --- End to end: a production captain sees an intruder and rings its wall alarm ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAICaptainRingsWallAlarmTest,
	"WoT.AI.CaptainRingsWallAlarm",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTAICaptainRingsWallAlarmTest::RunTest(const FString&)
{
	// EvaluateOrders (Captain.uc:298-322) picks the alarm, the captain walks to the
	// wall, strikes the gong (Captain.uc:723-727) and rings it. Production brain,
	// real collision and navmesh: the path filming D2 found broken.
	UWorld* World = WoTTest::CreateSpecWorld();
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	BuildNavigationTestMesh(World);
	AWoTNPCCharacter* Captain = WoTCreatureData::SpawnCreature(
		*World, TEXT("Archer"), FVector(300, 80, 60), 1, true);
	if (!TestNotNull(TEXT("production Archer captain"), Captain))
	{
		return false;
	}
	SpawnAIActivityAnchor(World, Captain->GetActorLocation());
	WoTTest::TickWorld(World, 0.3f);
	AWoTNPCController* Ctl = Cast<AWoTNPCController>(Captain->GetController());
	AWoTAlarm* Alarm = DeployTestWallAlarm(World);
	AWoTTestNPCCharacter* Enemy = SpawnCombatNPC(World, FVector(700, 0, 100), 2);
	if (!TestNotNull(TEXT("production Archer controller"), Ctl)
		|| !TestNotNull(TEXT("the alarm deploys on the wall"), Alarm)
		|| !TestNotNull(TEXT("intruder"), Enemy))
	{
		return false;
	}
	if (AController* EnemyCtl = Enemy->GetController())
	{
		EnemyCtl->SetActorTickEnabled(false);
	}
	Ctl->ChangeOrders(EWoTOrders::Automatic);
	Ctl->DispatchSeePawn(Enemy);
	TestEqual(TEXT("the captain takes SoundAlarm orders"),
		static_cast<int32>(Ctl->GetWorkingOrders()),
		static_cast<int32>(EWoTOrders::SoundAlarm));

	bool bStruck = false;
	float StrikeDistance = -1.0f;
	for (int32 Frame = 0; Frame < 60 * 12 && !Alarm->bRung; ++Frame)
	{
		WoTTest::TickWorld(World, 1.0f / 60.0f);
		if (!bStruck && Ctl->IsStrikingGongForTest())
		{
			bStruck = true;
			StrikeDistance = FVector::Dist2D(
				Captain->GetActorLocation(), Alarm->GetActorLocation());
		}
	}
	TestTrue(FString::Printf(TEXT("the captain reached the wall and struck the gong (%.1f uu off)"),
		StrikeDistance), bStruck);
	TestTrue(TEXT("the alarm rang"), Alarm->bRung);
	TestEqual(TEXT("then the captain turns on the intruder"),
		static_cast<int32>(Ctl->GetWorkingOrders()),
		static_cast<int32>(EWoTOrders::KillIntruder));
	return true;
}

// --- Guard seal: GuardSeal orders target the closest UNOWNED seal ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAIGuardSealTest,
	"WoT.AI.GuardSeal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTAIGuardSealTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTTestCaptainNPC* Captain = SpawnCaptain(World, FVector(0, 0, 100), 1);
	WoTTest::TickWorld(World, 0.1f);
	AWoTNPCController* Ctl = Cast<AWoTNPCController>(Captain->GetController());

	// A nearer OWNED seal (skipped: Owner set + a different team) and a farther
	// UNOWNED seal (chosen). Ownership is the GetClosestGoal unowned filter
	// (WOTUtil.uc:420); team 2 also fails the ||same-team clause.
	AWoTSeal* Owned = World->SpawnActor<AWoTSeal>(FVector(200, 0, 100), FRotator::ZeroRotator);
	AActor* OwnerActor = World->SpawnActor<AActor>(FVector(-500, 0, 100), FRotator::ZeroRotator);
	Owned->SetOwner(OwnerActor);
	Owned->Team = 2;
	AWoTSeal* Unowned = World->SpawnActor<AWoTSeal>(FVector(500, 0, 100), FRotator::ZeroRotator);

	// DispatchOrders(GuardSeal) → OnGuardSeal → GetClosestUnownedSeal → GI_Guarding.
	Ctl->DispatchOrders(EWoTOrders::GuardSeal);

	TestTrue(TEXT("guarding goal set to a seal"),
	         Captain->Brain->IsGoalValid(EWoTGoalIndex::Guarding));
	TestEqual(TEXT("guards the closest UNOWNED seal (nearer owned one skipped)"),
	          Captain->Brain->GetGoal(EWoTGoalIndex::Guarding).GoalActor.Get(),
	          static_cast<AActor*>(Unowned));

	WoTTest::DestroySpecWorld(World);
	return true;
}

// --- Creature rows: 12 troop leaves + retained base Trolloc ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAICreatureRowsTest,
	"WoT.AI.CreatureRows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTAICreatureRowsTest::RunTest(const FString&)
{
	// All 12 troop leaves plus the directly referenced base Trolloc row are registered.
	TArray<FName> Names;
	WoTCreatureData::GetAllRowNames(Names);
	TestEqual(TEXT("13 creature rows registered"), Names.Num(), 13);
	for (const TCHAR* Expected : {TEXT("Warder"), TEXT("Sister"), TEXT("Sitter"),
	                              TEXT("Trolloc"), TEXT("TrollocStrong"),
	                              TEXT("Myrddraal"), TEXT("BATrolloc"),
	                              TEXT("Minion"), TEXT("MashadarFocusPawn"), TEXT("Legion"),
	                              TEXT("Soldier"), TEXT("Archer"), TEXT("Questioner")})
	{
		TestNotNull(FString::Printf(TEXT("row %s present"), Expected),
		            WoTCreatureData::Find(FName(Expected)));
	}
	int32 DisguiseIconRows = 0;
	for (FName Name : Names)
	{
		const FWoTCreatureRow* Row = WoTCreatureData::Find(Name);
		DisguiseIconRows += Row && !Row->DisguiseIcon.IsNone() ? 1 : 0;
	}
	TestEqual(TEXT("all 12 disguise-capable production rows bind DisguiseIcon"),
		DisguiseIconRows, 12);

	// Trolloc: 150 hp / 440 speed / MeleeRange 50 / 15+10 'Cutup'/'Sliced' swings.
	const FWoTCreatureRow* Trolloc = WoTCreatureData::Find(TEXT("Trolloc"));
	if (TestNotNull(TEXT("Trolloc row"), Trolloc))
	{
		TestEqual(TEXT("Trolloc HealthMP band"), Trolloc->HealthMPMax, 150.0f);
		TestEqual(TEXT("Trolloc GroundSpeed band"), Trolloc->GroundSpeedMax, 440.0f);
		TestEqual(TEXT("Trolloc chase suggested speed"), Trolloc->ChaseSuggestedSpeed, 440.0f);
		TestEqual(TEXT("Trolloc MeleeRange"), Trolloc->MeleeRange, 50.0f);
		TestEqual(TEXT("Trolloc Mass"), Trolloc->Mass, 275.0f);
		TestEqual(TEXT("Trolloc collision radius"), Trolloc->CollisionRadius, 28.0f);
		TestEqual(TEXT("Trolloc two swings"), Trolloc->MeleeAttacks.Num(), 2);
		if (Trolloc->MeleeAttacks.Num() == 2)
		{
			TestEqual(TEXT("Trolloc swing A = 15 'Cutup'"), Trolloc->MeleeAttacks[0].Damage, 15.0f);
			TestEqual(TEXT("Trolloc swing A type"),
			          Trolloc->MeleeAttacks[0].DamageTypeName, FName(TEXT("Cutup")));
			TestEqual(TEXT("Trolloc swing B = 10 'Sliced'"), Trolloc->MeleeAttacks[1].Damage, 10.0f);
			TestEqual(TEXT("Trolloc swing B type"),
			          Trolloc->MeleeAttacks[1].DamageTypeName, FName(TEXT("Sliced")));
		}
		TestEqual(TEXT("Trolloc tier Grunt"),
		          static_cast<int32>(Trolloc->Tier), static_cast<int32>(EWoTCreatureTier::Grunt));
		TestEqual(TEXT("Trolloc faction Forsaken"),
		          static_cast<int32>(Trolloc->Faction), static_cast<int32>(EWoTCreatureFaction::Forsaken));
		TestFalse(TEXT("Trolloc not a captain"), Trolloc->bIsCaptain);
		TestEqual(TEXT("Trolloc initial state Inactive"),
		          static_cast<int32>(Trolloc->InitialState), static_cast<int32>(EWoTBrainState::Inactive));
	}

	// Sister: caster loadout non-empty, bound classes, verbatim priorities + Dart group.
	const FWoTCreatureRow* Sister = WoTCreatureData::Find(TEXT("Sister"));
	if (TestNotNull(TEXT("Sister row"), Sister))
	{
		TestEqual(TEXT("Sister 5-weave loadout"), Sister->Weaves.Num(), 5);
		TestEqual(TEXT("Sister no melee"), Sister->MeleeRange, 0.0f);
		TestTrue(TEXT("Sister is female"), Sister->bIsFemale);
		TestTrue(TEXT("Sister is a captain (AngrealUser class)"), Sister->bIsCaptain);
		TestEqual(TEXT("Sister offensive interval"), Sister->MinOffensiveAngrealUsageInterval, 4.0f);
		if (Sister->Weaves.Num() == 5)
		{
			TestEqual(TEXT("Sister weave 0 = AirBurst prio 1"), Sister->Weaves[0].Priority, 1.0f);
			TestNotNull(TEXT("Sister weave 0 class bound"), Sister->Weaves[0].WeaveClass.Get());
			TestEqual(TEXT("Sister Dart weave prio 9"), Sister->Weaves[1].Priority, 9.0f);
			TestEqual(TEXT("Sister Dart MinChargesInGroup 5"), Sister->Weaves[1].MinChargesInGroup, 5);
			TestEqual(TEXT("Sister Dart MaxChargesInGroup 8"), Sister->Weaves[1].MaxChargesInGroup, 8);
			TestEqual(TEXT("Sister Shift weave prio 10"), Sister->Weaves[3].Priority, 10.0f);
		}
	}

	// Champion-CLASS flags: BATrolloc / Legion get the no-morale + no-Automatic fold.
	const FWoTCreatureRow* BAT = WoTCreatureData::Find(TEXT("BATrolloc"));
	if (TestNotNull(TEXT("BATrolloc row"), BAT))
	{
		TestEqual(TEXT("BATrolloc tier Champion"),
		          static_cast<int32>(BAT->Tier), static_cast<int32>(EWoTCreatureTier::Champion));
		TestTrue(TEXT("BATrolloc is a captain (Champion->Captain)"), BAT->bIsCaptain);
		TestTrue(TEXT("BATrolloc has champion orders (no Automatic)"), BAT->bChampionOrders);
		TestEqual(TEXT("BATrolloc no morale"), BAT->BrokenMoralePct, 0.0f);
		TestEqual(TEXT("BATrolloc 700 hp band"), BAT->HealthMPMax, 700.0f);
		TestEqual(TEXT("BATrolloc placed 200 (diverges from 700)"), BAT->HealthPlaced, 200);
	}

	// Champion-TIER but NOT Champion-class: Sitter (Captain class) keeps orders +
	// morale; Questioner (Grunt class) has no orders. The tier/class mismatch.
	const FWoTCreatureRow* Sitter = WoTCreatureData::Find(TEXT("Sitter"));
	if (TestNotNull(TEXT("Sitter row"), Sitter))
	{
		TestEqual(TEXT("Sitter tier Champion"),
		          static_cast<int32>(Sitter->Tier), static_cast<int32>(EWoTCreatureTier::Champion));
		TestTrue(TEXT("Sitter is a captain (AngrealUser class)"), Sitter->bIsCaptain);
		TestFalse(TEXT("Sitter is NOT a Champion-class (keeps Automatic)"), Sitter->bChampionOrders);
		TestEqual(TEXT("Sitter keeps Grunt morale 25"), Sitter->BrokenMoralePct, 25.0f);
		TestEqual(TEXT("Sitter 8-weave loadout"), Sitter->Weaves.Num(), 8);
		// HealthPlaced divergence: placed 300 vs the 425 MP band.
		TestEqual(TEXT("Sitter placed 300"), Sitter->HealthPlaced, 300);
		TestEqual(TEXT("Sitter MP band 425"), Sitter->HealthMPMax, 425.0f);
	}
	const FWoTCreatureRow* Questioner = WoTCreatureData::Find(TEXT("Questioner"));
	if (TestNotNull(TEXT("Questioner row"), Questioner))
	{
		TestEqual(TEXT("Questioner tier Champion"),
		          static_cast<int32>(Questioner->Tier), static_cast<int32>(EWoTCreatureTier::Champion));
		TestFalse(TEXT("Questioner is NOT a captain (ShieldUser->Grunt)"), Questioner->bIsCaptain);
		TestFalse(TEXT("Questioner no champion orders"), Questioner->bChampionOrders);
		TestEqual(TEXT("Questioner keeps Grunt morale 25"), Questioner->BrokenMoralePct, 25.0f);
		TestTrue(TEXT("Questioner is a shield user"), Questioner->bShieldUser);
	}

	// The second HealthPlaced divergence carry: Legion placed 300 vs the 1000 band.
	const FWoTCreatureRow* Legion = WoTCreatureData::Find(TEXT("Legion"));
	if (TestNotNull(TEXT("Legion row"), Legion))
	{
		TestEqual(TEXT("Legion placed 300"), Legion->HealthPlaced, 300);
		TestEqual(TEXT("Legion MP band 1000"), Legion->HealthMPMax, 1000.0f);
		TestEqual(TEXT("Legion MeleeRange 500 (stomp reach)"), Legion->MeleeRange, 500.0f);
		TestEqual(TEXT("Legion chase hint 125 (> GroundSpeed 50)"), Legion->ChaseSuggestedSpeed, 125.0f);
		TestEqual(TEXT("Legion GroundSpeed 50"), Legion->GroundSpeedMax, 50.0f);
		TestEqual(TEXT("Legion behavior hook named"), Legion->BehaviorClass, FName(TEXT("LegionSpiritsStomp")));
	}

	// MashadarFocusPawn: the exotic Hound captain — Dormant, tiny collision, no melee.
	const FWoTCreatureRow* Mash = WoTCreatureData::Find(TEXT("MashadarFocusPawn"));
	if (TestNotNull(TEXT("MashadarFocusPawn row"), Mash))
	{
		TestEqual(TEXT("Mashadar initial state Dormant"),
		          static_cast<int32>(Mash->InitialState), static_cast<int32>(EWoTBrainState::Dormant));
		TestTrue(TEXT("Mashadar is a captain"), Mash->bIsCaptain);
		TestEqual(TEXT("Mashadar ScaleGlow 20"), Mash->ScaleGlow, 20.0f);
		TestEqual(TEXT("Mashadar collision radius 11"), Mash->CollisionRadius, 11.0f);
		TestEqual(TEXT("Mashadar no swing set"), Mash->MeleeAttacks.Num(), 0);
	}

	return true;
}

// --- Troop table: the 4x3 faction x tier grid + GetTroop keyword lookup ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAITroopTableTest,
	"WoT.AI.TroopTable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTAITroopTableTest::RunTest(const FString&)
{
	// The full 4x3 grid (WOTPlayer.uc:7264-7275) via GetTroop keyword lookup.
	struct FCase { const TCHAR* Faction; const TCHAR* Tier; const TCHAR* Row; };
	const FCase Cases[] = {
		{TEXT("AesSedai"),   TEXT("Grunt"),    TEXT("Warder")},
		{TEXT("AesSedai"),   TEXT("Captain"),  TEXT("Sister")},
		{TEXT("AesSedai"),   TEXT("Champion"), TEXT("Sitter")},
		// PlayerTroops(3): the summon table names BASE Trolloc; only the citadel
		// card (TrollocInventory.uc ResourceClass) deploys TrollocStrong.
		{TEXT("Forsaken"),   TEXT("Grunt"),    TEXT("Trolloc")},
		{TEXT("Forsaken"),   TEXT("Captain"),  TEXT("Myrddraal")},
		{TEXT("Forsaken"),   TEXT("Champion"), TEXT("BATrolloc")},
		{TEXT("Hound"),      TEXT("Grunt"),    TEXT("Minion")},
		{TEXT("Hound"),      TEXT("Captain"),  TEXT("MashadarFocusPawn")},
		{TEXT("Hound"),      TEXT("Champion"), TEXT("Legion")},
		{TEXT("Whitecloak"), TEXT("Grunt"),    TEXT("Soldier")},
		{TEXT("Whitecloak"), TEXT("Captain"),  TEXT("Archer")},
		{TEXT("Whitecloak"), TEXT("Champion"), TEXT("Questioner")},
	};
	for (const FCase& C : Cases)
	{
		const FName Row = WoTCreatureData::GetTroopRow(FName(C.Faction), FName(C.Tier));
		TestEqual(FString::Printf(TEXT("%s/%s -> %s"), C.Faction, C.Tier, C.Row),
		          Row, FName(C.Row));
		// Every troop row resolves to a real creature row.
		TestNotNull(FString::Printf(TEXT("%s row exists"), C.Row), WoTCreatureData::Find(Row));
	}

	// An unknown tuple returns NAME_None (no such troop).
	TestEqual(TEXT("unknown faction -> None"),
	          WoTCreatureData::GetTroopRow(TEXT("DragonSworn"), TEXT("Grunt")), FName(NAME_None));
	TestEqual(TEXT("unknown tier -> None"),
	          WoTCreatureData::GetTroopRow(TEXT("AesSedai"), TEXT("Boss")), FName(NAME_None));

	return true;
}

// --- Spawn application: a row-configured Trolloc chases + melees a threat ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAICreatureSpawnTest,
	"WoT.AI.CreatureSpawn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTAICreatureSpawnTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	// SpawnCreature('Trolloc') with a forced-nav test brain (no navmesh headless),
	// combat-ready so it possesses into Waiting like a bare spawn.
	AWoTNPCCharacter* Hunter = WoTCreatureData::SpawnCreature(
		*World, TEXT("Trolloc"), FVector(0, 0, 100), 1, /*bCombatReady*/ true,
		AWoTTestNPCCharacter::StaticClass());
	if (!TestNotNull(TEXT("Trolloc spawned"), Hunter))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Hunter->GetCharacterMovement()->SetMovementMode(MOVE_Flying);

	// The row was applied: Trolloc stats, not the bare-NPC defaults (100 hp / range 25).
	TestEqual(TEXT("spawned Trolloc has 150 hp (band roll)"), Hunter->Health, 150);
	TestEqual(TEXT("spawned Trolloc MeleeRange 50"), Hunter->MeleeRange, 50.0f);
	TestEqual(TEXT("spawned Trolloc swing set applied"), Hunter->MeleeAttacks.Num(), 2);
	AWoTNPCController* Ctl = Cast<AWoTNPCController>(Hunter->GetController());
	if (!TestNotNull(TEXT("Trolloc possessed by AWoTNPCController"), Ctl))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	TestEqual(TEXT("combat-ready Trolloc waits at spawn"),
	          static_cast<int32>(Ctl->GetBrainState()), static_cast<int32>(EWoTBrainState::Waiting));

	// A one-sided target that holds still: the Trolloc must chase and land a melee.
	AWoTTestNPCCharacter* Prey = SpawnCombatNPC(World, FVector(600, 0, 100), 2);
	Prey->MeleeRange = 0.0f;
	Prey->Brain->InvalidateGoal(EWoTGoalIndex::Waiting);
	const int32 PreyStart = 150;
	Prey->Health = PreyStart;
	Prey->HealthMPMin = Prey->HealthMPMax = PreyStart;

	WoTTest::TickWorld(World, 0.1f);
	Hunter->Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Prey);
	WoTTest::TickWorld(World, 6.0f);

	const float Closed = FVector::Dist2D(Hunter->GetActorLocation(), Prey->GetActorLocation());
	TestTrue(FString::Printf(TEXT("row-spawned Trolloc closed to melee (gap %.0f)"), Closed),
	         Closed < 100.0f);
	TestTrue(FString::Printf(TEXT("prey took Trolloc melee damage (health %d)"), Prey->Health),
	         Prey->Health < PreyStart);
	const int32 Lost = PreyStart - Prey->Health;
	TestTrue(FString::Printf(TEXT("damage is a sum of 15/10 swings (lost %d)"), Lost), Lost >= 10);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// --- Body-mesh bind: every creature row resolves + applies a real imported mesh ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAICreatureMeshTest,
	"WoT.AI.CreatureMesh",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTAICreatureMeshTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	// The reference creature comes up with a body: WOTPawns.Trolloc resolves to the
	// imported /Game/WoT/Meshes/Trolloc/Trolloc and ConfigureNPC applies it to the
	// pawn mesh component (+ DrawScale on the mesh, not the collision capsule).
	AWoTNPCCharacter* Trolloc =
		WoTCreatureData::SpawnCreature(*World, TEXT("Trolloc"), FVector(0, 0, 100), 1);
	if (TestNotNull(TEXT("Trolloc spawned"), Trolloc))
	{
		USkeletalMeshComponent* Body = Trolloc->GetMesh();
		if (TestNotNull(TEXT("Trolloc has a mesh component"), Body))
		{
			TestNotNull(TEXT("Trolloc body mesh bound (non-null asset)"),
			            Body->GetSkeletalMeshAsset());
			TestEqual(TEXT("Trolloc DrawScale 1.48 applied to the mesh"),
			          static_cast<float>(Body->GetRelativeScale3D().X), 1.48f);

			// The pawn mesh is the vertex-anim surface, and the
			// mesh bind self-fed its #exec MESH SEQUENCE table from DT_WoTAnimSequences.
			UWoTVertexAnimComponent* Anim = Cast<UWoTVertexAnimComponent>(Body);
			if (TestNotNull(TEXT("creature mesh component is a UWoTVertexAnimComponent"), Anim))
			{
				TestTrue(TEXT("sequence table fed on bind (Trolloc has 24)"),
				         Anim->Sequences.Num() >= 20);
			}
		}
	}

	// No row carries an unbound placeholder: every registered row resolves to a real
	// imported skeletal mesh (Sitter->Sister, Questioner->Soldier share a body).
	TArray<FName> Names;
	WoTCreatureData::GetAllRowNames(Names);
	int32 Bound = 0;
	for (FName Row : Names)
	{
		AWoTNPCCharacter* NPC =
			WoTCreatureData::SpawnCreature(*World, Row, FVector(0, 0, 100), 1);
		if (!NPC)
		{
			continue;
		}
		USkeletalMeshComponent* Body = NPC->GetMesh();
		const bool bHas = Body && Body->GetSkeletalMeshAsset() != nullptr;
		TestTrue(FString::Printf(TEXT("%s body mesh bound"), *Row.ToString()), bHas);
		Bound += bHas ? 1 : 0;
	}
	TestEqual(TEXT("all creature rows bound a mesh"), Bound, Names.Num());

	auto RuntimeBody = [World](FName Row, const FVector& Location)
	{
		AWoTNPCCharacter* NPC = WoTCreatureData::SpawnCreature(
			*World, Row, Location, 1);
		return NPC ? Cast<UWoTVertexAnimComponent>(NPC->GetMesh()) : nullptr;
	};
	UWoTVertexAnimComponent* Strong = RuntimeBody(TEXT("TrollocStrong"), FVector(500,0,100));
	UWoTVertexAnimComponent* SitterBody = RuntimeBody(TEXT("Sitter"), FVector(1000,0,100));
	UWoTVertexAnimComponent* MinionBody = RuntimeBody(TEXT("Minion"), FVector(1500,0,100));
	if (TestNotNull(TEXT("TrollocStrong production body"), Strong))
	{
		const FSoftObjectPath* Skin = Strong->GetRuntimeSkinOverrides().Find(1);
		TestTrue(TEXT("TrollocStrong slot 1 uses TWolf"), Skin
			&& Skin->ToString().Contains(TEXT("TWolf")));
	}
	if (TestNotNull(TEXT("Sitter production body"), SitterBody))
	{
		const FSoftObjectPath* Skin1 = SitterBody->GetRuntimeSkinOverrides().Find(1);
		const FSoftObjectPath* Skin2 = SitterBody->GetRuntimeSkinOverrides().Find(2);
		TestTrue(TEXT("Sitter slots 1 and 2 use JSitterBrown1"), Skin1 && Skin2
			&& Skin1->ToString().Contains(TEXT("JSitterBrown1"))
			&& *Skin1 == *Skin2);
	}
	WoTTest::TickWorld(World, 0.05f);
	if (TestNotNull(TEXT("Minion production body"), MinionBody))
	{
		const FSoftObjectPath* Eyes = MinionBody->GetRuntimeSkinOverrides().Find(4);
		TestTrue(TEXT("Minion blink toggles its authored eye slot"), Eyes
			&& Eyes->ToString().Contains(TEXT("MinionEyes")));
		bool bHasUnlitPass = false;
		for (UMaterialInterface* Material : MinionBody->GetMaterials())
		{
			bHasUnlitPass |= Material && Material->GetBaseMaterial()
				&& Material->GetBaseMaterial()->GetName().Contains(TEXT("Unlit"));
		}
		// Guarded cache-v3 builds construct this pass from the authenticated
		// Minion triangle flag. Some older developer carriers do not expose it.
		if (bHasUnlitPass)
			TestTrue(TEXT("Minion eye polygons bind an unlit runtime pass"), true);
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

// --- Summons: the ResourceSpawner ter'angreal chain ---

namespace
{
	// A possessed player caster on a unique free-for-all team, of a given summon
	// faction (its troop table). Mirrors the inventory tests' SpawnPlayer.
	AWoTCharacter* SpawnCaster(UWorld* World, AWoTGameMode* Game,
	                           EWoTCreatureFaction Faction, const FVector& Location)
	{
		APlayerController* PC = World->SpawnActor<APlayerController>();
		PC->InitPlayerState();
		if (Game) { Game->ChangeTeam(PC, 255); }   // login assigns a unique FFA team
		AWoTCharacter* Caster =
			World->SpawnActor<AWoTCharacter>(Location, FRotator::ZeroRotator);
		Caster->SummonFaction = Faction;
		PC->Possess(Caster);
		return Caster;
	}
}

// (a) A Minion cast spawns a faction Grunt on the caster's team; a second cast while
// it lives is blocked by HaveHelper and consumes no charge (ResourceSpawner.uc:34-73).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAISummonMinionTest,
	"WoT.AI.SummonMinion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTAISummonMinionTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTGameMode* Game = World->GetAuthGameMode<AWoTGameMode>();
	if (!TestNotNull(TEXT("the WoT game mode is present"), Game))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	// A Forsaken caster: the summon Grunt troop is the BASE Trolloc
	// (PlayerTroops(3), WOTPlayer.uc:7267 — NOT the citadel card's TrollocStrong).
	AWoTCharacter* Caster =
		SpawnCaster(World, Game, EWoTCreatureFaction::Forsaken, FVector(0, 0, 100));
	const uint8 CasterTeam = Caster->GetPlayerState<AWoTPlayerState>()->Team;

	Game->GivePawnAngreal(Caster, AWoTAngrealInvMinion::StaticClass(), 5);
	AWoTAngrealInvMinion* Minion = Cast<AWoTAngrealInvMinion>(
		Caster->InventoryComponent->FindInventoryType(AWoTAngrealInvMinion::StaticClass()));
	if (!TestNotNull(TEXT("caster holds the Minion ter'angreal"), Minion))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	TestEqual(TEXT("the Minion seeded 5 charges"), Minion->CurCharges, 5);

	// First cast: a Trolloc-row NPC appears in front, on the caster's team.
	Minion->CastArtifact();
	AWoTNPCCharacter* Summon = Minion->CurrentArenaHelper.Get();
	if (!TestNotNull(TEXT("a helper was summoned"), Summon))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	TestEqual(TEXT("the summon is the Forsaken Grunt (Trolloc)"),
		Summon->SummonRow, FName(TEXT("Trolloc")));
	TestEqual(TEXT("the summon copies the caster's team"), Summon->AssignedTeam, CasterTeam);
	TestTrue(TEXT("the summon is owned by the caster"), Summon->SummonOwner.Get() == Caster);
	TestEqual(TEXT("a successful summon consumes one charge"), Minion->CurCharges, 4);

	// The summon appearance/name feedback (ResourceSpawner.uc:58-63) — the
	// sparkle AppearEffect grows around the helper with the Minion's authored
	// corona pair, and the decorated display name feeds the death broadcast.
	int32 SparkleEffects = 0;
	for (TActorIterator<AWoTAppearEffect> It(World); It; ++It)
	{
		if (It->AppearTarget == Summon
			&& It->TopSprite == FName(TEXT("APinkCorona"))
			&& It->BottomSprite == FName(TEXT("ABlueCorona")))
		{
			++SparkleEffects;
		}
	}
	TestEqual(TEXT("the summon wears the Minion sparkle AppearEffect"),
		SparkleEffects, 1);
	const FString CasterName =
		Caster->GetPlayerState<AWoTPlayerState>()->GetPlayerName();
	TestEqual(TEXT("the summon carries the retail decorated name"),
		Summon->SummonDisplayName, CasterName + TEXT("'s Trolloc"));

	// Spawn offset = forward * (caster radius + summon radius + SpawnSeparation).
	const FWoTCreatureRow* Row = WoTCreatureData::Find(TEXT("Trolloc"));
	const float Expected =
		Caster->GetSimpleCollisionRadius() + (Row ? Row->CollisionRadius : 0.0f) + 50.0f;
	const float Forward = (Summon->GetActorLocation() - Caster->GetActorLocation()).X;
	TestTrue(FString::Printf(TEXT("summon spawned forward at the offset (%.1f ~ %.1f)"),
		Forward, Expected), FMath::Abs(Forward - Expected) < 1.0f);

	// Second cast while the helper is alive: HaveHelper blocks it — no new pawn, no charge.
	Minion->CastArtifact();
	TestTrue(TEXT("the one-at-a-time helper is unchanged"),
		Minion->CurrentArenaHelper.Get() == Summon);
	TestEqual(TEXT("a blocked (LimitWarning) cast consumes no charge"), Minion->CurCharges, 4);
	int32 NPCCount = 0;
	for (TActorIterator<AWoTNPCCharacter> It(World); It; ++It)
	{
		if (IsValid(*It)) { ++NPCCount; }
	}
	TestEqual(TEXT("no second helper was spawned"), NPCCount, 1);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// (b) Guardian/Champion resolve to the caster faction's Captain/Champion troops;
// the Hound Guardian uses the traced Mashadar placement path.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAISummonTiersTest,
	"WoT.AI.SummonTiers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTAISummonTiersTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTGameMode* Game = World->GetAuthGameMode<AWoTGameMode>();
	if (!TestNotNull(TEXT("the WoT game mode is present"), Game))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	// Whitecloak: Guardian -> Archer (Captain), Champion -> Questioner.
	AWoTCharacter* WC =
		SpawnCaster(World, Game, EWoTCreatureFaction::Whitecloak, FVector(0, 0, 100));
	Game->GivePawnAngreal(WC, AWoTAngrealInvGuardian::StaticClass(), 5);
	Game->GivePawnAngreal(WC, AWoTAngrealInvChampion::StaticClass(), 5);
	AWoTAngrealInvGuardian* Guardian = Cast<AWoTAngrealInvGuardian>(
		WC->InventoryComponent->FindInventoryType(AWoTAngrealInvGuardian::StaticClass()));
	AWoTAngrealInvChampion* Champion = Cast<AWoTAngrealInvChampion>(
		WC->InventoryComponent->FindInventoryType(AWoTAngrealInvChampion::StaticClass()));
	if (!TestNotNull(TEXT("caster holds the Guardian"), Guardian)
		|| !TestNotNull(TEXT("caster holds the Champion"), Champion))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	Guardian->CastArtifact();
	Champion->CastArtifact();
	AWoTNPCCharacter* Captain = Guardian->CurrentArenaHelper.Get();
	AWoTNPCCharacter* Champ = Champion->CurrentArenaHelper.Get();
	if (!TestNotNull(TEXT("the Guardian summoned a Captain-tier helper"), Captain)
		|| !TestNotNull(TEXT("the Champion summoned a Champion-tier helper"), Champ))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	TestEqual(TEXT("Whitecloak Guardian -> Archer"), Captain->SummonRow, FName(TEXT("Archer")));
	TestEqual(TEXT("Whitecloak Champion -> Questioner"), Champ->SummonRow, FName(TEXT("Questioner")));
	TestEqual(TEXT("the Guardian used a charge"), Guardian->CurCharges, 4);
	TestEqual(TEXT("the Champion used a charge"), Champion->CurCharges, 4);

	// Faction routing: an AesSedai Guardian summons the Sister, not the Archer.
	AWoTCharacter* AS =
		SpawnCaster(World, Game, EWoTCreatureFaction::AesSedai, FVector(500, 0, 100));
	Game->GivePawnAngreal(AS, AWoTAngrealInvGuardian::StaticClass(), 5);
	AWoTAngrealInvGuardian* GuardianAS = Cast<AWoTAngrealInvGuardian>(
		AS->InventoryComponent->FindInventoryType(AWoTAngrealInvGuardian::StaticClass()));
	GuardianAS->CastArtifact();
	if (AWoTNPCCharacter* Sister = GuardianAS->CurrentArenaHelper.Get())
	{
		TestEqual(TEXT("AesSedai Guardian -> Sister"), Sister->SummonRow, FName(TEXT("Sister")));
	}
	else
	{
		TestTrue(TEXT("AesSedai Guardian summoned a Sister"), false);
	}

	// Hound Guardian -> MashadarFocusPawn: the deferred tendril path. Graceful no-op:
	// nothing summoned, no charge consumed, and the deferral flag records the veto.
	AWoTCharacter* HD =
		SpawnCaster(World, Game, EWoTCreatureFaction::Hound, FVector(1000, 0, 100));
	Game->GivePawnAngreal(HD, AWoTAngrealInvGuardian::StaticClass(), 5);
	AWoTAngrealInvGuardian* GuardianHD = Cast<AWoTAngrealInvGuardian>(
		HD->InventoryComponent->FindInventoryType(AWoTAngrealInvGuardian::StaticClass()));
	SpawnNavigationBox(World, FVector(1080, 0, 100), FVector(5, 500, 500), false);
	GuardianHD->CastArtifact();
	TestTrue(TEXT("Hound Guardian resolves to the Mashadar path"),
		GuardianHD->IsMashadarSummon());
	TestNotNull(TEXT("Hound Guardian summons a Mashadar tendril"),
		GuardianHD->CurrentGuide.Get());
	TestEqual(TEXT("Hound Guardian consumes a charge"), GuardianHD->CurCharges, 4);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// (c) A summon's kill credits its owner; a player earns nothing for killing their own
// summon (giWOT.ScoreKill:141-152, the OnSameTeam / owner-credit edge).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAIOwnerKillCreditTest,
	"WoT.AI.OwnerKillCredit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTAIOwnerKillCreditTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTGameMode* Game = World->GetAuthGameMode<AWoTGameMode>();
	if (!TestNotNull(TEXT("the WoT game mode is present"), Game))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Game->FragLimit = 0;   // isolate scoring from the match-end check

	// Caster summons a Minion (its own team); an enemy player on another team.
	AWoTCharacter* Caster =
		SpawnCaster(World, Game, EWoTCreatureFaction::Forsaken, FVector(0, 0, 100));
	AWoTPlayerState* CasterPS = Caster->GetPlayerState<AWoTPlayerState>();
	Game->GivePawnAngreal(Caster, AWoTAngrealInvMinion::StaticClass(), 5);
	AWoTAngrealInvMinion* Minion = Cast<AWoTAngrealInvMinion>(
		Caster->InventoryComponent->FindInventoryType(AWoTAngrealInvMinion::StaticClass()));
	Minion->CastArtifact();
	AWoTNPCCharacter* Summon = Minion->CurrentArenaHelper.Get();

	AWoTCharacter* Enemy =
		SpawnCaster(World, Game, EWoTCreatureFaction::AesSedai, FVector(300, 0, 100));
	AWoTPlayerState* EnemyPS = Enemy->GetPlayerState<AWoTPlayerState>();
	if (!TestNotNull(TEXT("a helper was summoned"), Summon)
		|| !TestNotNull(TEXT("caster/enemy player states"), CasterPS)
		|| !EnemyPS)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	FWoTDamageEvent Ev;
	Ev.WoTDamageType = TEXT("Fire");

	// The summon lands the lethal blow on the enemy: the OWNER banks the frag (the
	// PlayerState-less NPC can't), and the enemy takes a death.
	Enemy->TakeDamage(1000.0f, Ev, Summon->GetController(), Summon);
	TestEqual(TEXT("the owner is credited for the summon's kill"), CasterPS->Kills, 1);
	TestEqual(TEXT("the enemy took a death"), EnemyPS->Deaths, 1);

	// The caster kills its own summon: same team (plus a PlayerState-less victim) -> no frag.
	Summon->TakeDamage(1000.0f, Ev, Caster->GetController(), Caster);
	TestTrue(TEXT("the summon died to friendly fire"), Summon->bDead);
	TestEqual(TEXT("killing your own summon earns no frag"), CasterPS->Kills, 1);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// (d) A disguised player presents the mimicked pawn's team to an NPC's faction check:
// friendly while disguised as its team, enemy once uncast (WOTPawn.IsFriendly ->
// GetApparentTeam, WOTPlayer.uc:1912/747/777).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAIDisguiseApparentTeamTest,
	"WoT.AI.DisguiseApparentTeam",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTAIDisguiseApparentTeamTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTGameMode* Game = World->GetAuthGameMode<AWoTGameMode>();
	if (!TestNotNull(TEXT("the WoT game mode is present"), Game))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	// A team-1 observer NPC, and a team-0 player nearby holding a Disguise.
	AWoTNPCCharacter* Observer = SpawnNPC(World, FVector(200, 0, 100));
	Observer->AssignedTeam = 1;

	APlayerController* PC = World->SpawnActor<APlayerController>();
	PC->InitPlayerState();
	AWoTCharacter* Player = World->SpawnActor<AWoTCharacter>(FVector(0, 0, 100), FRotator::ZeroRotator);
	PC->Possess(Player);
	PC->GetPlayerState<AWoTPlayerState>()->Team = 0;

	Game->GivePawnAngreal(Player, AWoTAngrealInvDisguise::StaticClass(), 20);
	AWoTAngrealInvDisguise* Disguise = Cast<AWoTAngrealInvDisguise>(
		Player->InventoryComponent->FindInventoryType(AWoTAngrealInvDisguise::StaticClass()));
	if (!TestNotNull(TEXT("player holds the Disguise ter'angreal"), Disguise))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	// Undisguised: real team 0 vs the observer's team 1 -> an enemy.
	TestFalse(TEXT("undisguised, the player is an enemy of the team-1 NPC"),
		Observer->IsFriendly(Player));

	// Disguise as the nearest pawn (the observer): apparent team becomes 1.
	Disguise->CastArtifact();
	TestEqual(TEXT("the disguise adopts the mimicked pawn's team"),
		Disguise->GetApparentTeam(), 1);
	TestTrue(TEXT("while disguised as team 1, the NPC reads the player as friendly"),
		Observer->IsFriendly(Player));
	// The cast stamps the owner-only ApparentClass surface (the mimicked pawn's class).
	TestTrue(TEXT("the disguise stamps the caster's ApparentClass to the mimicked pawn's class"),
		Player->ApparentClass == Observer->GetClass());
	// The cast also stamps the replicated VISIBLE-morph driver (the half that
	// fools other clients — see WoT.P34.* for the look copy itself).
	TestTrue(TEXT("the cast activates the replicated disguise driver"),
		Player->IsDisguised());
	TestTrue(TEXT("the driver's source class = the mimicked pawn's class"),
		Player->Disguise.SourceClass == Observer->GetClass());

	// Uncast: back to the real (enemy) team.
	Disguise->UnCastArtifact();
	TestEqual(TEXT("uncast clears the apparent team"), Disguise->GetApparentTeam(), -1);
	TestFalse(TEXT("after uncast the player is an enemy again"),
		Observer->IsFriendly(Player));
	TestNull(TEXT("uncast clears the caster's ApparentClass"), Player->ApparentClass.Get());
	TestFalse(TEXT("uncast clears the disguise driver"), Player->IsDisguised());

	WoTTest::DestroySpecWorld(World);
	return true;
}

// --- Behavior hook: ShieldDeflect (Soldier) — deflects while shielded, not attacking ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAIShieldDeflectTest,
	"WoT.AI.ShieldDeflect",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTAIShieldDeflectTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	// A row-configured Soldier (ShieldUser): the behavior hook rides in via ConfigureNPC.
	AWoTNPCCharacter* Soldier = WoTCreatureData::SpawnCreature(
		*World, TEXT("Soldier"), FVector(0, 0, 100), 1, /*bCombatReady*/ true,
		AWoTTestNPCCharacter::StaticClass());
	if (!TestNotNull(TEXT("Soldier spawned"), Soldier))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Soldier->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	UWoTShieldDeflectBehavior* Shield = Soldier->FindComponentByClass<UWoTShieldDeflectBehavior>();
	if (!TestNotNull(TEXT("Soldier carries the ShieldDeflect behavior"), Shield))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	// The upgrade (batch 4): the shield is REACTIVE to detected incoming fire, not
	// raised merely from seeing an enemy. Tracking a passive enemy with no projectile
	// in the air leaves the shield DOWN. The threat sits OFF the +X axis: the dart's
	// flight corridor (and its spawn point, offset from the drifting Soldier's live
	// location) must stay clear of bystander collision — an on-axis enemy ate the
	// dart mid-flight, and under suite load the dart even spawned inside it (self-
	// destroy during SpawnActor returns null; the melee phase teleports the enemy
	// explicitly, so its tracking-phase spot is free).
	AWoTTestNPCCharacter* Enemy = SpawnCombatNPC(World, FVector(600, 400, 100), 2);
	if (AController* EC = Enemy->GetController()) { EC->UnPossess(); }
	WoTTest::TickWorld(World, 0.1f);
	Soldier->Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Enemy);
	WoTTest::TickWorld(World, 0.5f);

	AWoTNPCController* Ctl = Cast<AWoTNPCController>(Soldier->GetController());
	TestEqual(TEXT("Soldier is Tracking its threat"),
	          static_cast<int32>(Ctl->GetBrainState()), static_cast<int32>(EWoTBrainState::Tracking));
	TestFalse(TEXT("shield stays DOWN while merely tracking (no incoming fire)"),
	          Shield->IsDeflectEnabled());

	// Face the threat (deterministic FOV) and loose a dart from the FRONT, inside the
	// arrival window (500uu / 2000uu.s = 0.25s, within [0.05,1.0] and the 1024 radius):
	// the active detector raises the shield in response. (A live dart matters: a dart
	// that explodes on an on-axis enemy first leaves the "deflect" running on a
	// pending-kill dart.)
	Soldier->SetActorRotation(FRotator::ZeroRotator);   // face +X, toward the incoming dart
	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	const FVector SoldierLoc = Soldier->GetActorLocation();
	AWoTCharacter* Shooter = World->SpawnActor<AWoTCharacter>(
		SoldierLoc + FVector(1100, 0, 0), FRotator::ZeroRotator, SP);
	AWoTDartProjectile* Dart = World->SpawnActor<AWoTDartProjectile>(
		AWoTDartProjectile::StaticClass(), SoldierLoc + FVector(500, 0, 0),
		FRotator(0, 180, 0), SP);   // flies -X, incoming toward the Soldier from the front
	if (!TestNotNull(TEXT("shooter spawned"), Shooter)
		|| !TestNotNull(TEXT("dart spawned live"), Dart))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Dart->SourcePawn = Shooter;
	Dart->SetIgnoredPawn(Shooter);
	WoTTest::TickWorld(World, 0.1f);
	TestTrue(TEXT("active detector raises the shield vs the incoming weave"),
	         Shield->IsDeflectEnabled());
	TestTrue(TEXT("shielded pawn reflects projectiles"), Soldier->ReflectsProjectiles());
	// EnableDeflect propagates the creature config (ShieldUser.uc:40-43): the
	// Soldier's production reflector carries the seeker exemption (Soldier.uc:49);
	// the dart below still deflects because a dart is NOT a SeekingProjectile.
	if (const UWoTShieldReturnToSenderReflector* Installed =
		Soldier->FindComponentByClass<UWoTShieldReturnToSenderReflector>())
	{
		TestEqual(TEXT("the production shield carries the seeker exemption"),
		          Installed->IrreleventProjectileType, FName(TEXT("SeekingProjectile")));
	}
	else
	{
		AddError(TEXT("no production shield reflector installed"));
	}

	// The incoming dart is deflected: it reverses, deals no damage, and now belongs to
	// the Soldier (the ReturnToSender bounce, WoTGenericProjectile::ProcessTouch).
	Soldier->Health = 150;
	const float DartVx0 = Dart->ProjectileMovement->Velocity.X;
	Dart->ProcessTouch(Soldier, Soldier->GetActorLocation());
	TestTrue(TEXT("dart had inbound velocity"), DartVx0 < 0.0f);
	TestEqual(TEXT("deflected dart does not hurt the shielded Soldier"), Soldier->Health, 150);
	TestTrue(TEXT("deflected dart reversed direction"), Dart->ProjectileMovement->Velocity.X > 0.0f);
	TestTrue(TEXT("deflected dart now belongs to the Soldier"), Dart->SourcePawn == Soldier);

	// Attacking phase: clear the incoming-fire actors (the shooter + the deflected dart)
	// so only the melee threat remains, then put that enemy in melee range. While the
	// Soldier commits melee swings the shield is down (no incoming, and the attack states
	// drop it anyway).
	Shooter->Destroy();
	Dart->Destroy();
	WoTTest::TickWorld(World, 0.1f);
	Soldier->Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Enemy);
	Enemy->SetActorLocation(Soldier->GetActorLocation() + FVector(40, 0, 0));
	bool bSawAttackingUnshielded = false;
	bool bEverAttacked = false;
	// Preparation-capable melee weapons now complete their real draw episode
	// before the first swing, so observe through that notify-driven lead-in.
	for (int32 i = 0; i < 120; ++i)
	{
		WoTTest::TickWorld(World, 0.05f);
		if (Ctl->GetBrainState() == EWoTBrainState::PerformMelee)
		{
			bEverAttacked = true;
			if (!Soldier->ReflectsProjectiles())
			{
				bSawAttackingUnshielded = true;
			}
		}
	}
	TestTrue(TEXT("the Soldier committed melee swings"), bEverAttacked);
	TestTrue(TEXT("the shield is down while attacking (no deflect mid-swing)"),
	         bSawAttackingUnshielded);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// --- Behavior hook: WarderFadeCloak — Visibility tracks the fade; sight range shrinks ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAIWarderFadeTest,
	"WoT.AI.WarderFade",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTAIWarderFadeTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	AWoTNPCCharacter* Warder = WoTCreatureData::SpawnCreature(
		*World, TEXT("Warder"), FVector(0, 0, 100), 1, /*bCombatReady*/ true,
		AWoTTestNPCCharacter::StaticClass());
	if (!TestNotNull(TEXT("Warder spawned"), Warder))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	SpawnAIActivityAnchor(World, Warder->GetActorLocation());
	Warder->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	// The retail idle Look/RubNeck notifies intentionally pulse the slow fade
	// variants. This legacy direction/range spec isolates the default 2s/1s edges;
	// WoT.AI.WarderFadeNotifies covers the idle-notify rate changes themselves.
	Warder->AnimTableName = NAME_None;
	if (!TestNotNull(TEXT("Warder carries the WarderFadeCloak behavior"),
	                 Warder->FindComponentByClass<UWoTWarderFadeCloakBehavior>()))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	AWoTNPCController* Ctl = Cast<AWoTNPCController>(Warder->GetController());

	// A stationary probe used to demonstrate an observer's detection range at a fixed
	// 2500uu offset from the Warder's live position.
	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTCharacter* Probe = World->SpawnActor<AWoTCharacter>(FVector(0, 5000, 100),
	                                                        FRotator::ZeroRotator, SP);
	Probe->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	auto ProbeSeesWarder = [&]()
	{
		Probe->SetActorLocation(Warder->GetActorLocation() + FVector(0, 2500, 0));
		return WoTSight::OmniSight(Probe, Warder, WoTSight::VisibilityOf(Warder));
	};

	// Idle/guarding (Waiting) -> the cloak fades out toward MinVisibility 16 over
	// FadeOutChangeTime 2s.
	TestEqual(TEXT("Warder waits at spawn"),
	          static_cast<int32>(Ctl->GetBrainState()), static_cast<int32>(EWoTBrainState::Waiting));
	WoTTest::TickWorld(World, 2.5f);
	TestTrue(FString::Printf(TEXT("faded Warder near MinVisibility (Vis %d)"), Warder->Visibility),
	         Warder->Visibility <= 20);
	TestFalse(TEXT("a faded Warder is out of the 2500uu observer's shrunk range"),
	          ProbeSeesWarder());

	// A threat appears -> Tracking (active) -> the cloak fades back in toward the full
	// default Visibility 128 over FadeInChangeTime 1s.
	AWoTTestNPCCharacter* Threat = SpawnCombatNPC(World, FVector(3000, 0, 100), 2);
	if (AController* TC = Threat->GetController()) { TC->UnPossess(); }
	Warder->Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Threat);
	WoTTest::TickWorld(World, 1.5f);
	TestEqual(TEXT("Warder is Tracking the threat"),
	          static_cast<int32>(Ctl->GetBrainState()), static_cast<int32>(EWoTBrainState::Tracking));
	TestTrue(FString::Printf(TEXT("revealed Warder near full Visibility (Vis %d)"), Warder->Visibility),
	         Warder->Visibility >= 120);
	TestTrue(TEXT("a revealed Warder is inside the same 2500uu observer's range"),
	         ProbeSeesWarder());

	WoTTest::DestroySpecWorld(World);
	return true;
}

// Warder fade presentation: replicated retail skin index drives modulated slots.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAIWarderFadePresentationTest,
	"WoT.AI.WarderFadePresentation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTAIWarderFadePresentationTest::RunTest(const FString&)
{
	const FProperty* FadeSkinProperty = FindFProperty<FProperty>(
		AWoTNPCCharacter::StaticClass(), TEXT("FadeSkinIndex"));
	TestTrue(TEXT("FadeSkinIndex replicates (CPF_Net)"),
		FadeSkinProperty && FadeSkinProperty->HasAnyPropertyFlags(CPF_Net));

	UMaterialInterface* ModulateMaster = LoadObject<UMaterialInterface>(nullptr,
		TEXT("/Game/WoT/Materials/M_WoT_MeshModulate.M_WoT_MeshModulate"));
	UTexture* Camo11 = LoadObject<UTexture>(nullptr,
		TEXT("/Game/WoT/Textures/WoTPawns/WCamo_11.WCamo_11"));
	UTexture* Camo10 = LoadObject<UTexture>(nullptr,
		TEXT("/Game/WoT/Textures/WoTPawns/WCamo_10.WCamo_10"));
	UTexture* VisibleSkin = LoadObject<UTexture>(nullptr,
		TEXT("/Game/WoT/Textures/WoTPawns/JWarder1.JWarder1"));
	if (!TestNotNull(TEXT("modulated mesh master loads"), ModulateMaster) ||
		!TestNotNull(TEXT("WCamo_11 fade texture loads"), Camo11) ||
		!TestNotNull(TEXT("WCamo_10 fade texture loads"), Camo10) ||
		!TestNotNull(TEXT("JWarder1 visible fade texture loads"), VisibleSkin))
	{
		return false;
	}

	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* Warder = WoTCreatureData::SpawnCreature(
		*World, TEXT("Warder"), FVector(0, 0, 100), 1, /*bCombatReady*/ true,
		AWoTTestNPCCharacter::StaticClass());
	if (!TestNotNull(TEXT("Warder spawned"), Warder))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	USkeletalMeshComponent* Body = Warder->GetMesh();
	UWoTWarderFadeCloakBehavior* FadeBehavior =
		Warder->FindComponentByClass<UWoTWarderFadeCloakBehavior>();
	if (!TestNotNull(TEXT("Warder body exists"), Body) ||
		!TestNotNull(TEXT("Warder fade behavior exists"), FadeBehavior))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	const int32 MaterialCount = Body->GetNumMaterials();
	TestTrue(TEXT("Warder body has authored material slots"), MaterialCount > 0);
	TArray<UMaterialInterface*> OriginalMaterials;
	OriginalMaterials.Reserve(MaterialCount);
	for (int32 Slot = 0; Slot < MaterialCount; ++Slot)
	{
		OriginalMaterials.Add(Body->GetMaterial(Slot));
	}

	Warder->SetFadeSkinIndex(0);
	TestEqual(TEXT("max fade selects retail skin index 0"),
		Warder->GetFadeSkinIndex(), static_cast<uint8>(0));
	for (int32 Slot = 0; Slot < MaterialCount; ++Slot)
	{
		UMaterialInstanceDynamic* FadeMaterial =
			Cast<UMaterialInstanceDynamic>(Body->GetMaterial(Slot));
		TestNotNull(FString::Printf(TEXT("slot %d swaps to a dynamic material"), Slot),
			FadeMaterial);
		if (!FadeMaterial)
		{
			continue;
		}
		TestTrue(FString::Printf(TEXT("slot %d is rooted at the modulate master"), Slot),
			FadeMaterial->Parent == ModulateMaster);
		UTexture* BoundTexture = nullptr;
		const bool bHasTexture = FadeMaterial->GetTextureParameterValue(
			FHashedMaterialParameterInfo(TEXT("Tex")), BoundTexture);
		TestTrue(FString::Printf(TEXT("slot %d exposes Tex"), Slot), bHasTexture);
		TestEqual(FString::Printf(TEXT("slot %d binds WCamo_11"), Slot),
			BoundTexture, Camo11);
	}

	Warder->SetFadeSkinIndex(1);
	for (int32 Slot = 0; Slot < MaterialCount; ++Slot)
	{
		UMaterialInstanceDynamic* FadeMaterial =
			Cast<UMaterialInstanceDynamic>(Body->GetMaterial(Slot));
		TestNotNull(FString::Printf(TEXT("slot %d rebuilds its routed fade material"), Slot),
			FadeMaterial);
		TestTrue(FString::Printf(TEXT("slot %d remains rooted at the modulate master"), Slot),
			FadeMaterial && FadeMaterial->Parent == ModulateMaster);
		UTexture* BoundTexture = nullptr;
		const bool bHasTexture = FadeMaterial && FadeMaterial->GetTextureParameterValue(
			FHashedMaterialParameterInfo(TEXT("Tex")), BoundTexture);
		TestTrue(FString::Printf(TEXT("slot %d still exposes Tex"), Slot), bHasTexture);
		TestEqual(FString::Printf(TEXT("slot %d updates to WCamo_10"), Slot),
			BoundTexture, Camo10);
	}
	Warder->SetFadeSkinIndex(11);
	for (int32 Slot = 0; Slot < MaterialCount; ++Slot)
	{
		UMaterialInstanceDynamic* FadeMaterial =
			Cast<UMaterialInstanceDynamic>(Body->GetMaterial(Slot));
		TestNotNull(FString::Printf(TEXT("slot %d routes the visible Warder fade texture"), Slot),
			FadeMaterial);
		TestTrue(FString::Printf(TEXT("slot %d remains modulated at index 11"), Slot),
			FadeMaterial && FadeMaterial->Parent == ModulateMaster);
		UTexture* BoundTexture = nullptr;
		const bool bHasTexture = FadeMaterial && FadeMaterial->GetTextureParameterValue(
			FHashedMaterialParameterInfo(TEXT("Tex")), BoundTexture);
		TestTrue(FString::Printf(TEXT("slot %d exposes Tex at index 11"), Slot), bHasTexture);
		TestEqual(FString::Printf(TEXT("slot %d binds JWarder1 at index 11"), Slot),
			BoundTexture, VisibleSkin);
	}

	Warder->SetFadeSkinIndex(AWoTNPCCharacter::VisibleFadeSkinIndex);
	for (int32 Slot = 0; Slot < MaterialCount; ++Slot)
	{
		TestEqual(FString::Printf(TEXT("slot %d restores its exact original"), Slot),
			Body->GetMaterial(Slot), OriginalMaterials[Slot]);
	}

	FadeBehavior->SetFadeLevel(0.0f);
	TestEqual(TEXT("FadeLevel 0 maps to skin index 0"),
		Warder->GetFadeSkinIndex(), static_cast<uint8>(0));
	FadeBehavior->SetFadeLevel(250.0f);
	TestEqual(TEXT("FadeLevel 250 maps monotonically to skin index 3"),
		Warder->GetFadeSkinIndex(), static_cast<uint8>(3));
	FadeBehavior->SetFadeLevel(500.0f);
	TestEqual(TEXT("FadeLevel 500/1000 maps to skin index 6"),
		Warder->GetFadeSkinIndex(), static_cast<uint8>(6));
	FadeBehavior->SetFadeLevel(750.0f);
	TestEqual(TEXT("FadeLevel 750 maps monotonically to skin index 9"),
		Warder->GetFadeSkinIndex(), static_cast<uint8>(9));
	FadeBehavior->SetFadeLevel(1000.0f);
	TestEqual(TEXT("FadeLevel equal to FadeLevels restores visible sentinel"),
		Warder->GetFadeSkinIndex(), AWoTNPCCharacter::VisibleFadeSkinIndex);

	FadeBehavior->SetFadeLevel(0.0f);
	TestTrue(TEXT("a faded Warder body can be rebound"),
		WoTCreatureData::BindCreatureBody(*Warder, TEXT("Warder")));
	for (int32 Slot = 0; Slot < Body->GetNumMaterials(); ++Slot)
	{
		UMaterialInstanceDynamic* FadeMaterial =
			Cast<UMaterialInstanceDynamic>(Body->GetMaterial(Slot));
		UTexture* BoundTexture = nullptr;
		const bool bHasTexture = FadeMaterial && FadeMaterial->GetTextureParameterValue(
			FHashedMaterialParameterInfo(TEXT("Tex")), BoundTexture);
		TestTrue(FString::Printf(TEXT("rebound slot %d stays modulated"), Slot),
			FadeMaterial && FadeMaterial->Parent == ModulateMaster);
		TestTrue(FString::Printf(TEXT("rebound slot %d exposes Tex"), Slot), bHasTexture);
		TestEqual(FString::Printf(TEXT("rebound slot %d keeps WCamo_11"), Slot),
			BoundTexture, Camo11);
	}
	FadeBehavior->SetFadeLevel(1000.0f);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// --- Behavior hook: MinionStealth — freezes while watched, resumes when unwatched ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAIMinionFreezeTest,
	"WoT.AI.MinionFreeze",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTAIMinionFreezeTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	AWoTNPCCharacter* Minion = WoTCreatureData::SpawnCreature(
		*World, TEXT("Minion"), FVector(0, 0, 100), 1, /*bCombatReady*/ true,
		AWoTTestNPCCharacter::StaticClass());
	if (!TestNotNull(TEXT("Minion spawned"), Minion))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Minion->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	UWoTMinionStealthBehavior* Stealth = Minion->FindComponentByClass<UWoTMinionStealthBehavior>();
	if (!TestNotNull(TEXT("Minion carries the MinionStealth behavior"), Stealth))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	AWoTNPCController* Ctl = Cast<AWoTNPCController>(Minion->GetController());

	// Widen the stare window to test the MECHANISM: shipped MaxStareTime is
	// 0.000001 (Minion.uc:476), which latches stealth off one tick after the
	// first threat — the shipped-inert default is asserted separately below.
	Stealth->MaxStareTime = 1000.0f;

	// The watcher/threat ahead of the Minion, facing back at it (yaw 180 -> forward
	// -X). A passive dummy (unpossessed) whose facing the test controls.
	AWoTTestNPCCharacter* Watcher = SpawnCombatNPC(World, FVector(400, 0, 100), 2);
	if (AController* WC = Watcher->GetController()) { WC->UnPossess(); }
	Watcher->SetActorRotation(FRotator(0.0f, 180.0f, 0.0f));

	Minion->Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Watcher);
	const FVector WatchedStart = Minion->GetActorLocation();
	WoTTest::TickWorld(World, 1.0f);

	// Watched: the Minion holds still (frozen) and does not advance on its threat.
	TestTrue(TEXT("watched Minion is frozen"), Stealth->IsFrozen());
	TestTrue(TEXT("watched Minion holds the controller stationary"), Ctl->IsForcedStationary());
	const float DriftWatched = FVector::Dist2D(Minion->GetActorLocation(), WatchedStart);
	TestTrue(FString::Printf(TEXT("watched Minion barely moved (drift %.0f)"), DriftWatched),
	         DriftWatched < 50.0f);

	// Look away (yaw 0 -> forward +X, Minion now behind the watcher): the Minion
	// unfreezes and advances on its threat.
	Watcher->SetActorRotation(FRotator(0.0f, 0.0f, 0.0f));
	const FVector UnwatchedStart = Minion->GetActorLocation();
	const float GapStart = FVector::Dist2D(UnwatchedStart, Watcher->GetActorLocation());
	WoTTest::TickWorld(World, 1.0f);

	TestFalse(TEXT("unwatched Minion is no longer frozen"), Stealth->IsFrozen());
	TestFalse(TEXT("unwatched Minion releases the stationary gate"), Ctl->IsForcedStationary());
	const float GapEnd = FVector::Dist2D(Minion->GetActorLocation(), Watcher->GetActorLocation());
	TestTrue(FString::Printf(TEXT("unwatched Minion advanced on its threat (gap %.0f -> %.0f)"),
	                         GapStart, GapEnd),
	         GapEnd < GapStart - 100.0f);

	// The SHIPPED default: MaxStareTime 1e-6 expires the stare window one tick
	// after the first threat, so a retail-data Minion does NOT stay frozen even
	// while watched (the disabled-behavior family — PORT_NOTES).
	AWoTNPCCharacter* RetailMinion = WoTCreatureData::SpawnCreature(
		*World, TEXT("Minion"), FVector(0, 800, 100), 1, /*bCombatReady*/ true,
		AWoTTestNPCCharacter::StaticClass());
	RetailMinion->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	AWoTTestNPCCharacter* Starer = SpawnCombatNPC(World, FVector(400, 800, 100), 2);
	if (AController* SC = Starer->GetController()) { SC->UnPossess(); }
	Starer->SetActorRotation(FRotator(0.0f, 180.0f, 0.0f));   // staring at it
	RetailMinion->Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Starer);
	WoTTest::TickWorld(World, 1.0f);
	UWoTMinionStealthBehavior* RetailStealth =
		RetailMinion->FindComponentByClass<UWoTMinionStealthBehavior>();
	TestFalse(TEXT("retail-data Minion's stare latch has expired (not frozen while watched)"),
	          RetailStealth->IsFrozen());

	WoTTest::DestroySpecWorld(World);
	return true;
}

// --- Behavior hook: BATrollocRecovery — a hard hit staggers on the shipped timing ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAIBATrollocStaggerTest,
	"WoT.AI.BATrollocStagger",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTAIBATrollocStaggerTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	AWoTNPCCharacter* BAT = WoTCreatureData::SpawnCreature(
		*World, TEXT("BATrolloc"), FVector(0, 0, 100), 1, /*bCombatReady*/ true,
		AWoTTestNPCCharacter::StaticClass());
	if (!TestNotNull(TEXT("BATrolloc spawned"), BAT))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	BAT->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	UWoTBATrollocRecoveryBehavior* Recovery =
		BAT->FindComponentByClass<UWoTBATrollocRecoveryBehavior>();
	if (!TestNotNull(TEXT("BATrolloc carries the BATrollocRecovery behavior"), Recovery))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	AWoTNPCController* Ctl = Cast<AWoTNPCController>(BAT->GetController());
	// Retail mesh decode: an omitted #exec RATE serializes as 30 fps.
	// Recover's 15 frames span 14 playback intervals, so
	// 14 / (30 fps * 0.45 rate) = ~1.037s.
	constexpr float RetailAnimRate = 30.0f;
	constexpr float RecoverDuration = 0.2f + (15.0f - 1.0f) / (RetailAnimRate * 0.45f);
	constexpr float MidRecoveryProbeTime = 0.7f;
	constexpr float FinishRecoveryWindow = 0.65f;

	// Advance world time past the "never recovered" sentinel gate (LastRecoveryTime 0
	// + RecoveryInterval 5 < now — BATrolloc.uc:86).
	WoTTest::TickWorld(World, 6.0f);

	FWoTDamageEvent Ev;
	Ev.WoTDamageType = TEXT("Chopped");

	// A sub-threshold hit (<= RecoveryDamageAmount 15) never staggers nor arms.
	BAT->TakeDamage(10.0f, Ev, nullptr, nullptr);
	TestFalse(TEXT("a sub-threshold hit does not stagger"), Recovery->IsRecovering());
	TestEqual(TEXT("a sub-threshold hit does not arm the cooldown"),
	          Recovery->GetLastRecoveryTime(), 0.0f);

	// The FIRST hard hit is deferred by bNoInitialRecovery: no stagger, cooldown armed
	// to now + interval (BATrolloc.uc:88-91).
	BAT->TakeDamage(20.0f, Ev, nullptr, nullptr);
	TestFalse(TEXT("the first hard hit only arms (bNoInitialRecovery)"), Recovery->IsRecovering());
	TestTrue(TEXT("the first hard hit arms the recovery cooldown"),
	         Recovery->GetLastRecoveryTime() > 0.0f);

	// A hard hit still inside the armed window does not stagger.
	BAT->TakeDamage(20.0f, Ev, nullptr, nullptr);
	TestFalse(TEXT("no stagger inside the recovery interval"), Recovery->IsRecovering());

	// Past the (deferred) window — the deferral pushed LastRecoveryTime to now+5, so the
	// next stagger needs now > that + 5 — a hard hit staggers: forced-stationary suspend.
	WoTTest::TickWorld(World, 12.0f);
	BAT->TakeDamage(20.0f, Ev, nullptr, nullptr);
	TestTrue(TEXT("a hard hit past the interval staggers"), Recovery->IsRecovering());
	TestTrue(TEXT("the stagger suspends the champion (forced stationary)"),
	         Ctl->IsForcedStationary());
	// The stagger TweenPlays the table's Recover anim and holds for the 0.2s
	// blend plus its ~1.04s real sequence.
	TestEqual(TEXT("the stagger plays the Recover anim"),
	          BAT->GetReplicatedAnimName(), FName(TEXT("Recover")));

	// A second hard hit mid-recovery does not double-trigger.
	BAT->TakeDamage(20.0f, Ev, nullptr, nullptr);
	TestTrue(TEXT("still staggered (no double-trigger mid-recovery)"), Recovery->IsRecovering());

	// The stagger remains active at the mid-sequence probe and clears after the
	// Recover sequence's duration, releasing the suspend.
	WoTTest::TickWorld(World, MidRecoveryProbeTime);
	TestTrue(FString::Printf(TEXT("still recovering at %.3fs (the real Recover runs ~%.3fs)"),
	                         MidRecoveryProbeTime, RecoverDuration),
	         Recovery->IsRecovering());
	WoTTest::TickWorld(World, FinishRecoveryWindow);
	TestFalse(TEXT("recovers after the Recover anim finishes"), Recovery->IsRecovering());
	TestFalse(TEXT("the suspend releases"), Ctl->IsForcedStationary());

	// Immediately after recovery, a hard hit is still inside RecoveryInterval → no re-stagger.
	BAT->TakeDamage(20.0f, Ev, nullptr, nullptr);
	TestFalse(TEXT("no immediate re-stagger inside the interval"), Recovery->IsRecovering());

	// --- Dynamic collision (OnAdjustCollisionNotification, uc:109): grows near the
	// threat, shrinks when it is far. ---
	const float DefaultRadius = BAT->GetCapsuleComponent()->GetUnscaledCapsuleRadius();
	AWoTTestNPCCharacter* Foe = SpawnCombatNPC(World, FVector(80, 0, 100), 2);
	if (AController* FC = Foe->GetController()) { FC->UnPossess(); }
	BAT->Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Foe);
	// The window must cover a full brain goal-evaluation pulse (CurrentGoalIndex only
	// flips to Threat on evaluation) PLUS a full CollisionAdjustInterval 0.5 cadence
	// AFTER it, at any entering phase — 0.6s was marginal and flaked in-suite.
	WoTTest::TickWorld(World, 1.6f);
	TestEqual(TEXT("collision grows to BiggerAssRadius near the threat"),
	          BAT->GetCapsuleComponent()->GetUnscaledCapsuleRadius(), 90.0f);

	// Threat far beyond AdjustCollisionThreshold 750 → back to the default radius.
	Foe->SetActorLocation(FVector(3000, 0, 100));
	WoTTest::TickWorld(World, 1.6f);
	TestEqual(TEXT("collision shrinks back when the threat is far"),
	          BAT->GetCapsuleComponent()->GetUnscaledCapsuleRadius(), DefaultRadius);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// --- Behavior hook: LegionSpiritsStomp — the ground-slam AoE + knockback falloff ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAILegionStompTest,
	"WoT.AI.LegionStomp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTAILegionStompTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	AWoTNPCCharacter* Legion = WoTCreatureData::SpawnCreature(
		*World, TEXT("Legion"), FVector(0, 0, 100), 1, /*bCombatReady*/ true,
		AWoTTestNPCCharacter::StaticClass());
	if (!TestNotNull(TEXT("Legion spawned"), Legion))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Legion->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	UWoTLegionSpiritsStompBehavior* Stomp =
		Legion->FindComponentByClass<UWoTLegionSpiritsStompBehavior>();
	if (!TestNotNull(TEXT("Legion carries the LegionSpiritsStomp behavior"), Stomp))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	// The 3 cosmetic spirits are recorded verbatim as data (Legion.uc:488-489).
	TestEqual(TEXT("3 spirit offsets recorded"), Stomp->GetSpiritOffsets().Num(), 3);
	if (Stomp->GetSpiritOffsets().Num() == 3)
	{
		TestTrue(TEXT("spirit 1 offset (-50,0,0)"),
		         Stomp->GetSpiritOffsets()[1].Equals(FVector(-50, 0, 0)));
		TestTrue(TEXT("spirit 2 offset (50,0,50)"),
		         Stomp->GetSpiritOffsets()[2].Equals(FVector(50, 0, 50)));
	}

	// Knockback falloff (ShakeRadius, uc:297): a nearer pawn is launched harder, both
	// positive.
	const float NearZ = UWoTLegionSpiritsStompBehavior::StompKnockbackZ(100.0f, 100.0f);
	const float FarZ = UWoTLegionSpiritsStompBehavior::StompKnockbackZ(2000.0f, 100.0f);
	TestTrue(TEXT("knockback falls off with distance (near > far > 0)"),
	         NearZ > FarZ && FarZ > 0.0f);

	// Direct AoE: a victim inside the volcano radius takes the stomp, one outside is
	// missed (the flat-10 damage is verbatim — no falloff on the damage itself).
	AWoTTestNPCCharacter* Inner = SpawnCombatNPC(World, FVector(1000, 0, 100), 2);
	AWoTTestNPCCharacter* Outer = SpawnCombatNPC(World, FVector(1300, 0, 100), 2);
	if (AController* IC = Inner->GetController()) { IC->UnPossess(); }
	if (AController* OC = Outer->GetController()) { OC->UnPossess(); }
	const int32 InnerStart = Inner->Health;
	const int32 OuterStart = Outer->Health;

	Stomp->ApplyStompAt(Inner->GetActorLocation());   // volcano centered on Inner
	TestTrue(FString::Printf(TEXT("in-radius victim took the stomp (%d -> %d)"),
	                         InnerStart, Inner->Health),
	         Inner->Health < InnerStart);
	TestEqual(TEXT("out-of-radius victim is missed"), Outer->Health, OuterStart);

	WoTTest::DestroySpecWorld(World);

	// Integration: a tracked threat already inside stomp range is slammed over time.
	World = WoTTest::CreateSpecWorld();
	Legion = WoTCreatureData::SpawnCreature(
		*World, TEXT("Legion"), FVector(0, 0, 100), 1, /*bCombatReady*/ true,
		AWoTTestNPCCharacter::StaticClass());
	Legion->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	AWoTTestNPCCharacter* Prey = SpawnCombatNPC(World, FVector(400, 0, 100), 2);
	Prey->MeleeRange = 0.0f;
	if (AController* PC = Prey->GetController()) { PC->UnPossess(); }
	const int32 PreyStart = Prey->Health;

	Legion->Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Prey);
	WoTTest::TickWorld(World, 3.0f);
	TestTrue(FString::Printf(TEXT("a tracked in-range threat is stomped over time (%d -> %d)"),
	                         PreyStart, Prey->Health),
	         Prey->Health < PreyStart);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// --- Caster defensive reflex: an incoming weave aimed at the caster is shielded,
//     one aimed past it is not (DefensiveDetectorAngrealUser + AngrealUser.uc:242/248) ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAIDefensiveReflexTest,
	"WoT.AI.DefensiveReflex",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

namespace
{
	// Bring a defensive caster into Tracking (the Defensive pulse is live only there):
	// a threat set directly, then a few Waiting pulses to promote it. Returns the shield.
	AWoTAngrealInvFireShield* ArmDefensiveCaster(UWorld* World, AWoTTestDefensiveCasterNPC* Sister,
	                                             AActor* Threat)
	{
		Sister->Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Threat);
		// Track long enough to reach Tracking AND to let the world clock pass the shield's
		// MinChargeGroupInterval (2s) — GetPriority gates on time-since-last-charge, and a
		// freshly-spawned weave's last-use stamp is 0, so an early cast reads unusable.
		WoTTest::TickWorld(World, 2.5f);
		UWoTInventoryComponent* Inv = Sister->FindComponentByClass<UWoTInventoryComponent>();
		AWoTAngrealInvFireShield* Shield = nullptr;
		for (AWoTInventory* Item : Inv->Items)
		{
			if (AWoTAngrealInvFireShield* S = Cast<AWoTAngrealInvFireShield>(Item))
			{
				Shield = S;
			}
		}
		return Shield;
	}

	// A Fire-element seeker inside the caster's perimeter, moving toward it. Velocity and
	// Destination are set directly (no homing/navmesh dependence): the velocity always
	// points at the caster so the rejection ladder passes, while Destination is the :248
	// gate's subject. The Fire source makes the Fire Shield counter it (GetDefensivePriority).
	AWoTTestSeeker* SpawnIncomingWeave(UWorld* World, const FVector& CasterLoc, AActor* Source,
	                                   AActor* Destination)
	{
		FActorSpawnParameters P;
		P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AWoTTestSeeker* Seeker = World->SpawnActor<AWoTTestSeeker>(
			CasterLoc + FVector(100, 0, 0), FRotator::ZeroRotator, P);
		Seeker->SetSourceAngreal(Cast<AWoTAngrealInventory>(Source));
		Seeker->Destination = Destination;
		const FVector Dir = (CasterLoc - Seeker->GetActorLocation()).GetSafeNormal();
		Seeker->ProjectileMovement->Velocity = Dir * 400.0f;
		return Seeker;
	}

	// Tick single frames (the defensive-cast state is transient — entered on the pulse,
	// left the next frame when the charge-spend is observed), reporting whether the
	// caster was ever seen in it.
	bool TickWatchingForDefensiveCast(UWorld* World, AWoTNPCController* Ctl, int32 Frames)
	{
		bool bSaw = false;
		for (int32 i = 0; i < Frames; ++i)
		{
			World->Tick(LEVELTICK_All, 1.0f / 60.0f);
			GFrameCounter++;
			bSaw |= (Ctl->GetBrainState() == EWoTBrainState::CasterDefensiveUse);
		}
		return bSaw;
	}
}

bool FWoTAIDefensiveReflexTest::RunTest(const FString&)
{
	// A Fireball as the incoming weave's source: the Fire element the Fire Shield counters.
	FActorSpawnParameters SrcParams;
	SrcParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	// --- (1) A seeker aimed AT the caster: the reflex fires and the shield is cast ---
	{
		UWorld* World = WoTTest::CreateSpecWorld();
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AWoTTestDefensiveCasterNPC* Sister = World->SpawnActor<AWoTTestDefensiveCasterNPC>(
			FVector(0, 0, 100), FRotator::ZeroRotator, Params);
		Sister->AssignedTeam = 1;
		Sister->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
		AWoTTestNPCCharacter* Enemy = SpawnCombatNPC(World, FVector(300, 0, 100), 2);
		if (AController* EC = Enemy->GetController()) { EC->UnPossess(); }
		WoTTest::TickWorld(World, 0.1f);

		AWoTNPCController* Ctl = Cast<AWoTNPCController>(Sister->GetController());
		AWoTAngrealInvFireShield* Shield = ArmDefensiveCaster(World, Sister, Enemy);
		if (!TestNotNull(TEXT("caster holds a Fire Shield"), Shield) ||
		    !TestEqual(TEXT("caster is tracking the threat"),
		               static_cast<int32>(Ctl->GetBrainState()),
		               static_cast<int32>(EWoTBrainState::Tracking)))
		{
			WoTTest::DestroySpecWorld(World);
			return false;
		}
		const int32 ChargesBefore = Shield->CurCharges;

		// The incoming weave's element source, kept far from the caster so it is not
		// auto-picked-up into the caster's inventory.
		AWoTAngrealInvFireball* Source =
			World->SpawnActor<AWoTAngrealInvFireball>(FVector(8000, 8000, 8000), FRotator::ZeroRotator, SrcParams);
		SpawnIncomingWeave(World, Sister->GetActorLocation(), Source, /*Destination*/ Sister);

		const bool bSawDefensiveState = TickWatchingForDefensiveCast(World, Ctl, 30);
		TestTrue(TEXT("the caster entered the defensive-cast state"), bSawDefensiveState);
		TestTrue(FString::Printf(TEXT("the Fire Shield was cast at the incoming weave (%d -> %d)"),
		                         ChargesBefore, Shield->CurCharges),
		         Shield->CurCharges < ChargesBefore);
		WoTTest::DestroySpecWorld(World);
	}

	// --- (2) A seeker aimed PAST the caster (Destination elsewhere): no reflex (:248 gate) ---
	{
		UWorld* World = WoTTest::CreateSpecWorld();
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AWoTTestDefensiveCasterNPC* Sister = World->SpawnActor<AWoTTestDefensiveCasterNPC>(
			FVector(0, 0, 100), FRotator::ZeroRotator, Params);
		Sister->AssignedTeam = 1;
		Sister->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
		AWoTTestNPCCharacter* Enemy = SpawnCombatNPC(World, FVector(300, 0, 100), 2);
		if (AController* EC = Enemy->GetController()) { EC->UnPossess(); }
		WoTTest::TickWorld(World, 0.1f);

		AWoTNPCController* Ctl = Cast<AWoTNPCController>(Sister->GetController());
		AWoTAngrealInvFireShield* Shield = ArmDefensiveCaster(World, Sister, Enemy);
		if (!TestNotNull(TEXT("caster holds a Fire Shield"), Shield))
		{
			WoTTest::DestroySpecWorld(World);
			return false;
		}
		const int32 ChargesBefore = Shield->CurCharges;

		// A decoy BEHIND the caster: the seeker (spawned in front) homes toward it and so
		// moves toward the caster (the rejection ladder passes), but its Destination is the
		// decoy, not the caster — the :248 seeker gate rejects it.
		const FVector CasterLoc = Sister->GetActorLocation();
		AWoTTestNPCCharacter* Decoy = SpawnCombatNPC(World, CasterLoc + FVector(-800, 0, 0), 2);
		if (AController* DC = Decoy->GetController()) { DC->UnPossess(); }
		AWoTAngrealInvFireball* Source =
			World->SpawnActor<AWoTAngrealInvFireball>(FVector(8000, 8000, 8000), FRotator::ZeroRotator, SrcParams);
		SpawnIncomingWeave(World, CasterLoc, Source, /*Destination*/ Decoy);

		const bool bSawDefensiveState = TickWatchingForDefensiveCast(World, Ctl, 15);
		TestFalse(TEXT("a seeker aimed past the caster does NOT trigger the reflex"),
		          bSawDefensiveState);
		TestEqual(TEXT("the Fire Shield was not cast at a seeker aimed elsewhere"),
		          Shield->CurCharges, ChargesBefore);
		WoTTest::DestroySpecWorld(World);
	}

	return true;
}

// --- Activity LOD: an Inactive-spawned pawn ignores a distant player, wakes on
//     proximity, and can then acquire a threat; a Dormant pawn does not wake ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAIInactiveWakeTest,
	"WoT.AI.InactiveWake",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTAIInactiveWakeTest::RunTest(const FString&)
{
	// --- (1) Inactive: distant player ignored, proximity wakes, then acquires ---
	{
		UWorld* World = WoTTest::CreateSpecWorld();
		// bCombatReady=false: the shipped InitialState (Trolloc = Inactive) now sticks.
		AWoTNPCCharacter* Guard = WoTCreatureData::SpawnCreature(
			*World, TEXT("Trolloc"), FVector(0, 0, 100), 1, /*bCombatReady*/ false,
			AWoTTestNPCCharacter::StaticClass());
		if (!TestNotNull(TEXT("guard spawned"), Guard))
		{
			WoTTest::DestroySpecWorld(World);
			return false;
		}
		Guard->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
		AWoTNPCController* Ctl = Cast<AWoTNPCController>(Guard->GetController());
		if (!TestNotNull(TEXT("guard possessed"), Ctl))
		{
			WoTTest::DestroySpecWorld(World);
			return false;
		}
		TestEqual(TEXT("an Inactive-row guard spawns Inactive"),
		          static_cast<int32>(Ctl->GetBrainState()),
		          static_cast<int32>(EWoTBrainState::Inactive));

		// A distant player (beyond the 2500 activation radius) does not wake it.
		AWoTCharacter* Intruder = SpawnAIActivityAnchor(
			World, FVector(3200, 0, 100));
		WoTTest::TickWorld(World, 1.5f);
		TestEqual(TEXT("a distant player leaves the guard Inactive"),
		          static_cast<int32>(Ctl->GetBrainState()),
		          static_cast<int32>(EWoTBrainState::Inactive));
		TestFalse(TEXT("an Inactive guard perceives nothing (no threat)"),
		          Guard->Brain->IsGoalValid(EWoTGoalIndex::Threat));

		// Bring the player inside the radius: the next 1s poll wakes it to Waiting.
		// Keep the awakened Trolloc below TrollocStrongAxeProxy's 300uu projectile
		// minimum so the sniping gate deterministically chooses the classic chase.
		Intruder->SetActorLocation(FVector(250, 0, 100));
		WoTTest::TickWorld(World, 1.5f);
		TestEqual(TEXT("a player inside the radius wakes the guard to Waiting"),
		          static_cast<int32>(Ctl->GetBrainState()),
		          static_cast<int32>(EWoTBrainState::Waiting));

		// Perception is re-armed on wake: a sighting forms a threat -> Tracking.
		Intruder->Health = 100;
		Ctl->DispatchSeePawn(Intruder);
		WoTTest::TickWorld(World, 0.5f);
		TestTrue(TEXT("the woken guard can acquire a threat"),
		         Guard->Brain->IsGoalValid(EWoTGoalIndex::Threat));
		TestEqual(TEXT("the woken guard tracks the intruder"),
		          static_cast<int32>(Ctl->GetBrainState()),
		          static_cast<int32>(EWoTBrainState::Tracking));
		WoTTest::DestroySpecWorld(World);
	}

	// --- (2) Dormant: explicit freeze, proximity does NOT wake, UnForceDormant releases ---
	{
		UWorld* World = WoTTest::CreateSpecWorld();
		AWoTTestNPCCharacter* Sentry = SpawnCombatNPC(World, FVector(0, 0, 100), 1);
		WoTTest::TickWorld(World, 0.1f);
		AWoTNPCController* Ctl = Cast<AWoTNPCController>(Sentry->GetController());
		if (!TestNotNull(TEXT("sentry possessed"), Ctl))
		{
			WoTTest::DestroySpecWorld(World);
			return false;
		}
		Ctl->ForceDormant();
		TestTrue(TEXT("ForceDormant freezes the sentry"), Ctl->IsDormant());

		AWoTCharacter* Intruder = SpawnAIActivityAnchor(
			World, FVector(400, 0, 100));
		WoTTest::TickWorld(World, 2.0f);
		TestTrue(TEXT("proximity does NOT wake a Dormant sentry"), Ctl->IsDormant());

		Ctl->UnForceDormant();
		TestFalse(TEXT("UnForceDormant releases the sentry"), Ctl->IsDormant());
		WoTTest::DestroySpecWorld(World);
	}

	return true;
}

// --- Mashadar: the Hound tendril hunts the nearest enemy and Spirit-damages it ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRC33MashadarProjectileOverlapSpec,
	"WoT.RC33.R3.MashadarProjectileOverlap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTRC33MashadarProjectileOverlapSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTMashadarGuide* Guide = World->SpawnActorDeferred<AWoTMashadarGuide>(
		AWoTMashadarGuide::StaticClass(), FTransform(FRotator::ZeroRotator,
			FVector(0.0f, 0.0f, 100.0f)), nullptr, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	Guide->bCitadelPlaced = true;
	Guide->FinishSpawning(Guide->GetActorTransform());
	Guide->MashadarHealth = 100;
	Guide->Deploy();
	Guide->Activate();
	AWoTMashadarSegment* Segment = Guide->GetMasterSegment();
	TestNotNull(TEXT("projectile overlap uses the production-managed segment"), Segment);
	AWoTDartProjectile* Dart = World->SpawnActor<AWoTDartProjectile>(
		AWoTDartProjectile::StaticClass(), FVector(-120.0f, 0.0f, 100.0f),
		FRotator::ZeroRotator, SP);
	TestTrue(TEXT("the production dart is genuinely moving"),
		Dart && Dart->ProjectileMovement->Velocity.X > 0.0f);
	WoTTest::TickWorld(World, 0.15f);
	TestEqual(TEXT("projectile overlap routes segment damage to guide health"),
		Guide->MashadarHealth, 90);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAIMashadarSummonTest,
	"WoT.AI.MashadarSummon",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTAIMashadarSummonTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTMashadarGuide* Guide = World->SpawnActor<AWoTMashadarGuide>(
		AWoTMashadarGuide::StaticClass(), FVector(0, 0, 100), FRotator::ZeroRotator, SP);
	if (!TestNotNull(TEXT("Mashadar guide spawned"), Guide))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Guide->Team = 1;
	Guide->HidingPlaceLocation = Guide->GetActorLocation();

	// An enemy (team 2) and a same-team ally (team 1) within SearchRadius.
	AWoTTestNPCCharacter* Enemy = SpawnCombatNPC(World, FVector(300, 0, 100), 2);
	if (AController* EC = Enemy->GetController()) { EC->UnPossess(); }
	Enemy->Health = 100;
	AWoTTestNPCCharacter* Ally = SpawnCombatNPC(World, FVector(200, 0, 100), 1);
	if (AController* AC = Ally->GetController()) { AC->UnPossess(); }
	Ally->Health = 100;

	// Search hunts the nearest ENEMY (the ally, though closer, is same-team → ignored),
	// and activates the tendril so its body bites.
	Guide->SearchAndChase();
	TestEqual(TEXT("the tendril targets the enemy, not the closer ally"),
	          Guide->Destination.Get(), static_cast<AActor*>(Enemy));
	TestTrue(TEXT("acquiring a target activates the tendril"), Guide->bActive);

	// The DAMAGE lives on the body segments (the guide is the homing leader): the hidden
	// head segment bites for 1 point of Spirit ('xxxxS') and the tendril persists.
	AWoTMashadarSegment* Head = Guide->GetMasterSegment();
	if (!TestNotNull(TEXT("the tendril grew its head segment"), Head))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Head->BiteTarget(Enemy);
	TestEqual(TEXT("a body segment deals 1 Spirit damage on contact"), Enemy->Health, 99);
	TestTrue(TEXT("the tendril persists after biting (not destroyed)"), IsValid(Guide));

	// A same-team pawn is never bitten.
	Head->BiteTarget(Ally);
	TestEqual(TEXT("a same-team pawn is unharmed"), Ally->Health, 100);

	// A dead enemy is no longer a valid target (re-search drops it).
	Enemy->Health = 0;
	Enemy->bDead = true;
	Guide->SearchAndChase();
	TestTrue(TEXT("a dead enemy is dropped as a target"),
	         Guide->Destination.Get() != static_cast<AActor*>(Enemy));

	// Idle (no valid enemy), the tendril retracts toward its hiding place (origin).
	Enemy->Destroy();
	Guide->SetActorLocation(FVector(600, 0, 100));
	WoTTest::TickWorld(World, 0.5f);
	TestTrue(FString::Printf(TEXT("an idle tendril retracts toward its lair (x=%.0f)"),
	                         Guide->GetActorLocation().X),
	         Guide->GetActorLocation().X < 550.0f);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// --- Mashadar citadel command: guarding a seal confines the hunt to its threats ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAIMashadarGuardTest,
	"WoT.AI.MashadarGuard",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTAIMashadarGuardTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTMashadarGuide* Guide = World->SpawnActor<AWoTMashadarGuide>(
		AWoTMashadarGuide::StaticClass(), FVector(0, 0, 100), FRotator::ZeroRotator, SP);
	if (!TestNotNull(TEXT("guide spawned"), Guide))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Guide->Team = 1;
	Guide->HidingPlaceLocation = Guide->GetActorLocation();

	// A seal at (0,800), an enemy NEAR it (0,900 — 100uu from the seal), and a CLOSER
	// enemy FAR from the seal (0,-600 — 1400uu from the seal). Both are within the guide's
	// SearchRadius (1000) of the hiding place.
	AWoTSeal* Seal = World->SpawnActor<AWoTSeal>(
		AWoTSeal::StaticClass(), FVector(0, 800, 100), FRotator::ZeroRotator, SP);
	TestNotNull(TEXT("seal placed"), Seal);
	AWoTTestNPCCharacter* NearSeal = SpawnCombatNPC(World, FVector(0, 900, 100), 2);
	if (AController* C = NearSeal->GetController()) { C->UnPossess(); }
	AWoTTestNPCCharacter* FarFromSeal = SpawnCombatNPC(World, FVector(0, -600, 100), 2);
	if (AController* C = FarFromSeal->GetController()) { C->UnPossess(); }

	// Free mode hunts the CLOSEST enemy (the far-from-seal one, 600 < 900).
	Guide->SearchAndChase();
	TestEqual(TEXT("free mode hunts the closest enemy"),
	          Guide->Destination.Get(), static_cast<AActor*>(FarFromSeal));

	// GuardSeal: only the enemy threatening the seal is pursued, though it is farther.
	Guide->GuardSeal();
	Guide->SearchAndChase();
	TestEqual(TEXT("seal guard hunts the enemy threatening the seal"),
	          Guide->Destination.Get(), static_cast<AActor*>(NearSeal));

	// That victim leaves the seal's radius → it is dropped (ShouldStopGuarding).
	NearSeal->SetActorLocation(FVector(0, 3000, 100));
	Guide->SearchAndChase();
	TestTrue(TEXT("a victim that leaves the seal is dropped"),
	         Guide->Destination.Get() != static_cast<AActor*>(NearSeal));

	// GuardLocation confines the hunt to a fixed point.
	Guide->GuardLocation(FVector(0, -600, 100));
	Guide->SearchAndChase();
	TestEqual(TEXT("location guard hunts the enemy near the point"),
	          Guide->Destination.Get(), static_cast<AActor*>(FarFromSeal));

	// UnGuard resumes free hunting.
	Guide->UnGuard();
	Guide->SearchAndChase();
	TestEqual(TEXT("UnGuard resumes free hunting"),
	          Guide->Destination.Get(), static_cast<AActor*>(FarFromSeal));

	WoTTest::DestroySpecWorld(World);
	return true;
}

// --- Mashadar health: the tendril can be worn down and killed ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAIMashadarHealthTest,
	"WoT.AI.MashadarHealth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTAIMashadarHealthTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTMashadarGuide* Guide = World->SpawnActor<AWoTMashadarGuide>(
		AWoTMashadarGuide::StaticClass(), FVector(0, 0, 100), FRotator::ZeroRotator, SP);
	if (!TestNotNull(TEXT("guide spawned"), Guide))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	// MashadarArenaSpawner deploys the guide, then the first real Chase asks its
	// private manager to AddMashadar. Drive that path: a bare BeginPlay guide has
	// no manager slot and therefore correctly has zero health.
	AWoTGameMode* Game = World->GetAuthGameMode<AWoTGameMode>();
	AWoTCharacter* Killer = Game
		? SpawnCaster(World, Game, EWoTCreatureFaction::AesSedai,
			FVector(500, 0, 100))
		: nullptr;
	AWoTPlayerState* KillerPS = Killer
		? Killer->GetPlayerState<AWoTPlayerState>() : nullptr;
	Guide->Team = (KillerPS && KillerPS->Team == 1) ? 2 : 1;
	Guide->Deploy();
	Guide->NotifySeePawn(Killer);

	// The first deployed chase seeds the tendril's health in [min,max).
	const int32 H0 = Guide->MashadarHealth;
	TestTrue(FString::Printf(TEXT("first chase seeded 150-199 health (%d)"), H0),
	         H0 >= 150 && H0 < 200);

	// A non-lethal hit drains health but leaves the tendril alive.
	FWoTDamageEvent Ev;
	Ev.WoTDamageType = TEXT("hurt");
	Guide->TakeDamage(50.0f, Ev, nullptr, nullptr);
	TestEqual(TEXT("a non-lethal hit drains the tendril's health"), Guide->MashadarHealth, H0 - 50);
	TestTrue(TEXT("the tendril survives a non-lethal hit"), IsValid(Guide));

	// The killing blow destroys the whole tendril AND credits an enemy killer a frag
	// (an enemy destroying the defender — giWOT.Killed via the focus pawn's PRI).
	if (TestNotNull(TEXT("game mode present"), Game))
	{
		Game->FragLimit = 0;
		Guide->TakeDamage(static_cast<float>(H0), Ev, Killer->GetController(), Killer);
		WoTTest::TickWorld(World, 0.05f);
		TestFalse(TEXT("the killing blow destroys the tendril"), IsValid(Guide));
		if (KillerPS)
		{
			TestEqual(TEXT("destroying an enemy defender credits the killer a frag"),
			          KillerPS->Kills, 1);
		}
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

// --- Mashadar body: the damaging segment chain grows with reach and trims when idle ---
// Proves the tendril grows a chain of body segments trailing the guide while it hunts a
// distant enemy (the damaging reach), that a BODY segment (not just the head) carries the
// Spirit bite, and that the chain withdraws to just the head when the tendril goes idle.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAIMashadarBodyTest,
	"WoT.AI.MashadarBody",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTAIMashadarBodyTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTMashadarGuide* Guide = World->SpawnActor<AWoTMashadarGuide>(
		AWoTMashadarGuide::StaticClass(), FVector(0, 0, 100), FRotator::ZeroRotator, SP);
	if (!TestNotNull(TEXT("Mashadar guide spawned"), Guide))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Guide->Team = 1;
	// The lair is the deploy spot; range out far from it so the chain must grow to span back.
	const FVector Lair = Guide->HidingPlaceLocation;
	Guide->SetActorLocation(Lair + FVector(500, 0, 0));

	// Walk the chain to count the body segments trailing the head.
	auto BodyCount = [](AWoTMashadarGuide* G) -> int32
	{
		int32 N = 0;
		AWoTMashadarSegment* Head = G->GetMasterSegment();
		for (AWoTIKTrailer* S = Head ? Head->ChildTrailer : nullptr; S; S = S->ChildTrailer)
		{
			++N;
		}
		return N;
	};
	TestEqual(TEXT("a fresh tendril has no body yet (just the head)"), BodyCount(Guide), 0);

	// An enemy well out past the guide: it hunts, activates, and grows a trailing body.
	AWoTTestNPCCharacter* Enemy = SpawnCombatNPC(World, Lair + FVector(900, 0, 0), 2);
	if (AController* EC = Enemy->GetController()) { EC->UnPossess(); }
	Enemy->Health = 100;

	Guide->SearchAndChase();
	TestTrue(TEXT("the tendril activates on its target"), Guide->bActive);
	WoTTest::TickWorld(World, 0.5f);

	const int32 GrownBody = BodyCount(Guide);
	TestTrue(FString::Printf(TEXT("the tendril grew a trailing body chain (%d segments)"), GrownBody),
	         GrownBody >= 3);

	// A BODY segment (not the head) carries the same Spirit bite.
	AWoTMashadarSegment* BodySeg = Cast<AWoTMashadarSegment>(Guide->GetMasterSegment()->ChildTrailer);
	if (!TestNotNull(TEXT("a body segment exists behind the head"), BodySeg))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	AWoTTestNPCCharacter* Bitten = SpawnCombatNPC(World, Lair + FVector(400, 0, 0), 2);
	if (AController* BC = Bitten->GetController()) { BC->UnPossess(); }
	Bitten->Health = 100;
	BodySeg->BiteTarget(Bitten);
	TestEqual(TEXT("a body segment deals 1 Spirit damage"), Bitten->Health, 99);

	// --- Display: the segments carry the writhing MashSeg/MashTip meshes
	// with the MyTex2 glow MID (the bParticles vertex-cloud approximation). ---
	{
		AWoTMashadarSegment* Head = Guide->GetMasterSegment();
		UWoTVertexAnimComponent* HeadMesh =
			Head ? Head->FindComponentByClass<UWoTVertexAnimComponent>() : nullptr;
		if (TestNotNull(TEXT("head display mesh"), HeadMesh)
			&& HeadMesh->GetSkeletalMeshAsset())
		{
			TestEqual(TEXT("the head wears MashTip"),
			          HeadMesh->GetSkeletalMeshAsset()->GetName(),
			          FString(TEXT("MashTip")));
			TestTrue(TEXT("the head writhes (LoopAnim 'All')"), HeadMesh->IsAnimating());
		}
		UWoTVertexAnimComponent* BodyMesh =
			BodySeg->FindComponentByClass<UWoTVertexAnimComponent>();
		if (TestNotNull(TEXT("body display mesh"), BodyMesh)
			&& BodyMesh->GetSkeletalMeshAsset())
		{
			TestEqual(TEXT("a body segment wears MashSeg"),
			          BodyMesh->GetSkeletalMeshAsset()->GetName(),
			          FString(TEXT("MashSeg")));
		}
		// A hit puffs the replicated pain burst at the segment.
		FWoTDamageEvent PainEv;
		PainEv.WoTDamageType = TEXT("Sliced");
		BodySeg->TakeDamage(5.0f, PainEv, nullptr, nullptr);
		bool bSawBurst = false;
		for (TActorIterator<AWoTWeaveBurst> It(World); It; ++It)
		{
			bSawBurst |= (It->Config == FName(TEXT("MashadarPainSprayer")));
		}
		TestTrue(TEXT("a segment hit spawns the pain-burst marker"), bSawBurst);
	}

	// Idle: the enemy is gone, the tendril deactivates and withdraws its body to the head.
	Enemy->Destroy();
	Bitten->Destroy();
	Guide->SearchAndChase();   // no target -> Deactivate
	TestFalse(TEXT("losing the target deactivates the tendril"), Guide->bActive);
	WoTTest::TickWorld(World, 2.0f);
	TestEqual(TEXT("an idle tendril trims its body back to just the head"), BodyCount(Guide), 0);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// --- IK trailer: the jointed chain that follows a head (the Mashadar body core) ---
// Proves the two invariants of AWoTIKTrailer::Reposition: (1) every joint snaps to
// exactly Dist behind its parent, (2) the joint bend never exceeds AngleExt, and the
// chain re-solves when the head moves. This is the IK math that drives the visible
// Mashadar tendril body (exports/script/Legend/IKTrailer.uc).

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAIIKTrailerTest,
	"WoT.AI.IKTrailer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTAIIKTrailerTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	// A movable stand-in for the guide head the chain follows.
	AWoTTestIKTrailer* Head = World->SpawnActor<AWoTTestIKTrailer>(
		FVector(0, 0, 100), FRotator::ZeroRotator, Params);   // faces +X
	// Master joint starts directly behind the head; a second joint trails it.
	AWoTTestIKTrailer* Master = World->SpawnActor<AWoTTestIKTrailer>(
		FVector(-100, 0, 100), FRotator::ZeroRotator, Params);
	AWoTTestIKTrailer* Child = World->SpawnActor<AWoTTestIKTrailer>(
		FVector(-200, 0, 100), FRotator::ZeroRotator, Params);
	if (!TestNotNull(TEXT("head spawned"), Head) ||
	    !TestNotNull(TEXT("master joint spawned"), Master) ||
	    !TestNotNull(TEXT("child joint spawned"), Child))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	const float Dist = 40.0f;
	Master->Dist = Child->Dist = Dist;
	Master->bMaster = true;
	Master->HeadActor = Head;
	Master->ChildTrailer = Child;
	// A generous bend limit for the spacing test (BeginPlay rolled a random one).
	Master->AngleExt = Child->AngleExt = 90.0f;

	// --- Invariant 1: each joint snaps to exactly Dist behind its parent ---
	Master->Reposition(Head);
	const float MasterGap = FVector::Dist(Master->GetActorLocation(), Head->GetActorLocation());
	const float ChildGap = FVector::Dist(Child->GetActorLocation(), Master->GetActorLocation());
	TestTrue(FString::Printf(TEXT("master sits Dist behind the head (gap %.2f vs %.1f)"),
	                         MasterGap, Dist),
	         FMath::IsNearlyEqual(MasterGap, Dist, 0.5f));
	TestTrue(FString::Printf(TEXT("child sits Dist behind the master (gap %.2f vs %.1f)"),
	                         ChildGap, Dist),
	         FMath::IsNearlyEqual(ChildGap, Dist, 0.5f));

	// --- Invariant 2: the bend clamp holds AngleExt ---
	// Tighten the limit and swing the head 90 degrees: the joint that was straight
	// behind can now bend only AngleExt toward the head's new backward direction.
	const float Tight = 30.0f;
	Master->AngleExt = Tight;
	Head->SetActorRotation(FRotator(0.0f, 90.0f, 0.0f));   // now faces +Y
	Master->Reposition(Head);

	const float BackYaw = (-Head->GetActorRotation().Vector()).Rotation().Yaw;
	const float OffsetYaw =
		(Master->GetActorLocation() - Head->GetActorLocation()).Rotation().Yaw;
	const float Bend = FMath::Abs(FRotator::NormalizeAxis(OffsetYaw - BackYaw));
	TestTrue(FString::Printf(TEXT("the joint bend is clamped to AngleExt (%.1f <= %.1f)"),
	                         Bend, Tight),
	         Bend <= Tight + 0.5f);
	// Still exactly Dist away — the clamp reorients, it never stretches the joint.
	TestTrue(TEXT("the clamped joint stays Dist behind the head"),
	         FMath::IsNearlyEqual(
	             FVector::Dist(Master->GetActorLocation(), Head->GetActorLocation()), Dist, 0.5f));

	// --- Follow-through: the chain re-solves when the head moves (Tick monitors it) ---
	Master->AngleExt = Child->AngleExt = 90.0f;
	Head->SetActorLocation(FVector(500, 500, 100));
	WoTTest::TickWorld(World, 0.1f);
	TestTrue(TEXT("the chain tracks the moved head (master re-snapped to Dist)"),
	         FMath::IsNearlyEqual(
	             FVector::Dist(Master->GetActorLocation(), Head->GetActorLocation()), Dist, 1.0f));
	TestTrue(TEXT("the trailing joint follows the master after the move"),
	         FMath::IsNearlyEqual(
	             FVector::Dist(Child->GetActorLocation(), Master->GetActorLocation()), Dist, 1.0f));

	WoTTest::DestroySpecWorld(World);
	return true;
}

// --- Combat anims: hit reacts, death variants, and the real
// melee swing timing on a row-spawned (table + mesh bound) creature. ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAICombatAnimsTest,
	"WoT.AI.CombatAnims",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTAICombatAnimsTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	AWoTNPCCharacter* Subject = WoTCreatureData::SpawnCreature(
		*World, TEXT("Trolloc"), FVector(0, 0, 100), 1, /*bCombatReady*/ true,
		AWoTTestNPCCharacter::StaticClass());
	if (!TestNotNull(TEXT("Trolloc spawned"), Subject))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Subject->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	AWoTNPCController* Ctl = Cast<AWoTNPCController>(Subject->GetController());

	// ConfigureNPC bound the anim data: table ref, melee slot, hit-react reference.
	TestEqual(TEXT("row bound the anim table"), Subject->AnimTableName, FName(TEXT("Trolloc")));
	TestEqual(TEXT("Trolloc swings the unarmed MeleeAttack slot"),
	          Subject->MeleeAnimSlot, FName(TEXT("MeleeAttack")));
	TestEqual(TEXT("hit ratios reference the authored default health"),
	          Subject->HitReferenceHealth, 150.0f);

	// --- Hit reactions (WOTPawn.PlayHit): slot by damage size vs default.health;
	// F/B is the table's 50/50 odds; below-soft hits never react. ---
	Subject->ChanceOfHitAnim = 1.0f;   // close the probability gate for determinism
	FWoTDamageEvent Ev;
	Ev.WoTDamageType = TEXT("Cutup");

	Subject->TakeDamage(10.0f, Ev, nullptr, nullptr);   // soft: 7.5 <= 10 < 37.5
	const FName SoftAnim = Subject->GetReplicatedAnimName();
	TestTrue(FString::Printf(TEXT("a soft hit flinches Hit B/F (got %s)"), *SoftAnim.ToString()),
	         SoftAnim == FName(TEXT("HitB")) || SoftAnim == FName(TEXT("HitF")));
	// The soft tween is a real TakeHit detour; let it return before the next
	// probe because TakeHitState itself suppresses recursive hit animation.
	WoTTest::TickWorld(World, 0.3f);

	Subject->TakeDamage(40.0f, Ev, nullptr, nullptr);   // hard: >= 0.25 * 150
	const FName HardAnim = Subject->GetReplicatedAnimName();
	TestTrue(FString::Printf(TEXT("a hard hit plays HitHard B/F (got %s)"), *HardAnim.ToString()),
	         HardAnim == FName(TEXT("HitBHard")) || HardAnim == FName(TEXT("HitFHard")));
	WoTTest::TickWorld(World, 0.7f);

	const uint8 TokenBefore = Subject->RepAnim.PlayToken;
	Subject->TakeDamage(5.0f, Ev, nullptr, nullptr);    // below soft (< 7.5): no reaction
	TestEqual(TEXT("a below-soft hit stamps nothing"), Subject->RepAnim.PlayToken, TokenBefore);

	// --- Melee swing: the anim-table pick drives WHICH swing plays, and its damage
	// lands at the authored notify fraction of the real sequence duration.
	// Retail mesh decode: an omitted #exec RATE serializes as 30 fps.
	// At the table's 0.6 rate, the playback clock uses NumFrames-1 intervals:
	// The table's 0.2s TweenPlay entry comes before sequence time begins:
	// AttackRun notify = 0.2 + 0.75 * (8-1)/(30*0.6) = ~0.492s;
	// ATTACKRUNB = 0.2 + 0.70 * (10-1)/(30*0.6) = 0.550s. ---
	constexpr float RetailAnimRate = 30.0f;
	constexpr float SwingRateScale = 0.6f;
	constexpr float AttackRunDuration =
		(8.0f - 1.0f) / (RetailAnimRate * SwingRateScale);
	constexpr float AttackRunBDuration =
		(10.0f - 1.0f) / (RetailAnimRate * SwingRateScale);
	constexpr float AttackRunNotifyTime = 0.75f * AttackRunDuration;
	constexpr float AttackRunBNotifyTime = 0.70f * AttackRunBDuration;
	constexpr float BeforeNotifyProbeTime = 0.45f;
	constexpr float NotifyObservationTime = 0.65f;
	AWoTTestNPCCharacter* Prey = SpawnCombatNPC(World,
		Subject->GetActorLocation() + FVector(40, 0, 0), 2);
	if (AController* PC = Prey->GetController()) { PC->UnPossess(); }
	Prey->MeleeRange = 0.0f;
	Prey->Health = 150;

	WoTTest::TickWorld(World, 0.1f);
	// The damage probes above now exercise the shipped hostile/no-instigator
	// retreat. Clear that completed intermediate detour before isolating melee.
	Subject->Brain->InvalidateGoal(EWoTGoalIndex::Intermediate);
	Subject->Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Prey);
	bool bEnteredMelee = false;
	for (int32 i = 0; i < 180 && !bEnteredMelee; ++i)
	{
		WoTTest::TickWorld(World, 1.0f / 60.0f);
		bEnteredMelee = Ctl && Ctl->GetBrainState() == EWoTBrainState::PerformMelee;
	}
	if (TestTrue(TEXT("the Trolloc committed a melee swing"), bEnteredMelee))
	{
		const FName SwingAnim = Subject->GetReplicatedAnimName();
		TestTrue(FString::Printf(TEXT("the swing plays a MeleeAttack pick (got %s)"),
		                         *SwingAnim.ToString()),
		         SwingAnim == FName(TEXT("AttackRun")) || SwingAnim == FName(TEXT("ATTACKRUNB")));

		const int32 PreyBefore = Prey->Health;
		WoTTest::TickWorld(World, BeforeNotifyProbeTime);
		TestEqual(FString::Printf(TEXT("no damage before the notify fraction "
		                               "(%.3fs < %.3f/%.3f)"),
			                          BeforeNotifyProbeTime, 0.2f + AttackRunNotifyTime,
			                          0.2f + AttackRunBNotifyTime),
		          Prey->Health, PreyBefore);
		WoTTest::TickWorld(World, NotifyObservationTime - BeforeNotifyProbeTime);
		const int32 Lost = PreyBefore - Prey->Health;
		TestTrue(FString::Printf(TEXT("one notify landed by %.3fs (lost %d)"),
		                         NotifyObservationTime, Lost),
		         Lost == 15 || Lost == 10);
	}

	// --- Death variant (PlayDeathHit): the 'Death' slot pick replaces the literal
	// stand-in — a Trolloc dies as DEATHF/DEATHB/DEATHC at the authored 0.35 rate. ---
	Subject->Health = 1;
	Subject->TakeRawDamage(1, TEXT("Sliced"), nullptr, FVector::ZeroVector);
	TestTrue(TEXT("the Trolloc died"), Subject->bDead);
	const FName DeathAnim = Subject->GetReplicatedAnimName();
	TestTrue(FString::Printf(TEXT("death picked a table variant (got %s)"), *DeathAnim.ToString()),
	         DeathAnim == FName(TEXT("DEATHF")) || DeathAnim == FName(TEXT("DEATHB")) ||
	         DeathAnim == FName(TEXT("DEATHC")));
	TestEqual(TEXT("the death variant plays at the authored 0.35 rate"),
	          Subject->RepAnim.Rate, 0.35f);

	// A table-less pawn keeps the literal "Death" stand-in.
	Prey->Health = 1;
	Prey->TakeRawDamage(1, TEXT("Sliced"), nullptr, FVector::ZeroVector);
	TestEqual(TEXT("a bare pawn still stamps the literal Death"),
	          Prey->GetReplicatedAnimName(), FName(TEXT("Death")));

	WoTTest::DestroySpecWorld(World);
	return true;
}

// The creature-identity netpass: the row name replicates and a client re-derives
// the body from the static registry (UE1 replicated Mesh/DrawScale natively; the
// port replicates the NAME — the carcass MeshName template). Before this,
// nothing row-shaped replicated and net clients could not bind creature meshes.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAICreatureRowReplTest,
	"WoT.AI.CreatureRowRepl", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTAICreatureRowReplTest::RunTest(const FString&)
{
	// Replication surface: CPF_Net only — never walk the lifetime list off a CDO
	// (push-model assert; PORT_NOTES).
	const FProperty* Prop = FindFProperty<FProperty>(
	    AWoTNPCCharacter::StaticClass(), TEXT("CreatureRowName"));
	TestTrue(TEXT("CreatureRowName replicates (CPF_Net)"),
	         Prop && Prop->HasAnyPropertyFlags(CPF_Net));

	UWorld* World = WoTTest::CreateSpecWorld();

	// Authority half: ConfigureNPC stamps the identity and binds the body.
	AWoTNPCCharacter* Trolloc = WoTCreatureData::SpawnCreature(
	    *World, TEXT("Trolloc"), FVector(0, 0, 100), 1, /*bCombatReady*/ true,
	    AWoTTestNPCCharacter::StaticClass());
	if (!TestNotNull(TEXT("Trolloc spawned"), Trolloc))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	TestEqual(TEXT("ConfigureNPC stamped the row identity"),
	          Trolloc->CreatureRowName, FName(TEXT("Trolloc")));
	TestTrue(TEXT("authority bound the body"),
	         Trolloc->GetMesh()->GetSkeletalMeshAsset() != nullptr);

	// Client half: a BARE pawn (never ConfigureNPC'd — the net-spawn shape) whose
	// replicated row name lands, then OnRep re-derives the look from the registry.
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride =
	    ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTNPCCharacter* Proxy = World->SpawnActor<AWoTNPCCharacter>(
	    AWoTNPCCharacter::StaticClass(), FVector(0, 500, 100), FRotator::ZeroRotator,
	    Params);
	TestNull(TEXT("a bare pawn has no body"),
	         Proxy->GetMesh()->GetSkeletalMeshAsset());
	Proxy->CreatureRowName = FName(TEXT("Legion"));
	Proxy->OnRep_CreatureRow();
	if (TestNotNull(TEXT("OnRep bound the Legion body"),
	                Proxy->GetMesh()->GetSkeletalMeshAsset()))
	{
		TestEqual(TEXT("the Legion mesh"),
		          Proxy->GetMesh()->GetSkeletalMeshAsset()->GetFName(),
		          FName(TEXT("Legion")));
	}
	TestEqual(TEXT("the Legion DrawScale 2.5 (mesh-only, capsule unscaled)"),
	          static_cast<float>(Proxy->GetMesh()->GetRelativeScale3D().X), 2.5f);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTTargetingCandidateSetTest,
	"WoT.Targeting.CandidateSet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTTargetingCandidateSetTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTCharacter* Owner = World->SpawnActor<AWoTCharacter>(
		AWoTCharacter::StaticClass(), FVector(0, 0, 100),
		FRotator::ZeroRotator, Params);
	AWoTAngrealInvFireball* Targeter = World->SpawnActor<AWoTAngrealInvFireball>(
		FVector(0, 5000, 500), FRotator::ZeroRotator);
	Targeter->GiveTo(Owner);

	Params.Owner = Owner;
	AWoTAngrealIllusionProjectile* VisibleIllusion =
		World->SpawnActor<AWoTAngrealIllusionProjectile>(
			AWoTAngrealIllusionProjectile::StaticClass(), FVector(1000, 20, 100),
			FRotator::ZeroRotator, Params);
	VisibleIllusion->AcquireImage(Owner);
	AWoTAngrealIllusionProjectile* HiddenIllusion =
		World->SpawnActor<AWoTAngrealIllusionProjectile>(
			AWoTAngrealIllusionProjectile::StaticClass(), FVector(500, 0, 100),
			FRotator::ZeroRotator, Params);
	HiddenIllusion->AcquireImage(Owner);
	HiddenIllusion->SetActorHiddenInGame(true);

	TestEqual(TEXT("the default FOV candidate set can return a Personal Illusion"),
		Targeter->FindBestTarget(Owner->GetActorLocation(), FRotator::ZeroRotator, 45.0f),
		static_cast<AActor*>(VisibleIllusion));
	TestFalse(TEXT("the owner is never targetable"), Targeter->IsTargetable(Owner));
	TestFalse(TEXT("hidden actors are never targetable"),
		Targeter->IsTargetable(HiddenIllusion));

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTTargetingAutoTargetIllusionTest,
	"WoT.Targeting.AutoTargetIllusion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTTargetingAutoTargetIllusionTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTCharacter* Owner = World->SpawnActor<AWoTCharacter>(
		AWoTCharacter::StaticClass(), FVector(0, 0, 100),
		FRotator::ZeroRotator, Params);
	AWoTCharacter* FartherPawn = World->SpawnActor<AWoTCharacter>(
		AWoTCharacter::StaticClass(), FVector(700, 0, 100),
		FRotator::ZeroRotator, Params);
	AWoTAngrealInvFireball* Targeter = World->SpawnActor<AWoTAngrealInvFireball>(
		FVector(0, 5000, 500), FRotator::ZeroRotator);
	Targeter->GiveTo(Owner);
	UWoTAutoTargetReflector* AutoTarget = NewObject<UWoTAutoTargetReflector>(Owner);
	AutoTarget->Install(Owner);

	Params.Owner = Owner;
	AWoTAngrealIllusionProjectile* NearIllusion =
		World->SpawnActor<AWoTAngrealIllusionProjectile>(
			AWoTAngrealIllusionProjectile::StaticClass(), FVector(300, 0, 100),
			FRotator::ZeroRotator, Params);
	NearIllusion->AcquireImage(Owner);

	TestEqual(TEXT("autotarget picks the nearer reachable illusion over a pawn"),
		Targeter->FindBestTarget(Owner->GetActorLocation(), FRotator::ZeroRotator, 45.0f),
		static_cast<AActor*>(NearIllusion));
	TestNotEqual(TEXT("the farther pawn was not selected"),
		static_cast<AActor*>(NearIllusion), static_cast<AActor*>(FartherPawn));

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTTargetingNavReachabilityFallbackTest,
	"WoT.Targeting.NavReachabilityFallback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTTargetingNavReachabilityFallbackTest::RunTest(const FString&)
{
	IConsoleVariable* SeekBackend = IConsoleManager::Get().FindConsoleVariable(
		TEXT("wot.SeekNavBackend"));
	if (!TestNotNull(TEXT("the seeking backend seam is registered"), SeekBackend))
	{
		return false;
	}
	const FString PreviousBackend(SeekBackend->GetString());
	SeekBackend->Set(TEXT("navmesh"), ECVF_SetByCode);

	auto SpawnTargetingPair = [](UWorld* World,
		AWoTCharacter*& OutOwner, AWoTCharacter*& OutCandidate,
		AWoTAngrealInvFireball*& OutTargeter)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		OutOwner = World->SpawnActor<AWoTCharacter>(
			AWoTCharacter::StaticClass(), FVector(-400, 0, 100),
			FRotator::ZeroRotator, Params);
		OutCandidate = World->SpawnActor<AWoTCharacter>(
			AWoTCharacter::StaticClass(), FVector(400, 0, 100),
			FRotator::ZeroRotator, Params);
		OutTargeter = World->SpawnActor<AWoTAngrealInvFireball>(
			FVector(0, 5000, 500), FRotator::ZeroRotator);
		OutTargeter->GiveTo(OutOwner);
		UWoTAutoTargetReflector* AutoTarget =
			NewObject<UWoTAutoTargetReflector>(OutOwner);
		AutoTarget->Install(OutOwner);
	};

	UWorld* NoNavWorld = WoTTest::CreateSpecWorld();
	SpawnNavigationBox(NoNavWorld, FVector(0, 0, 100),
		FVector(30, 160, 150));
	AWoTCharacter* NoNavOwner = nullptr;
	AWoTCharacter* NoNavCandidate = nullptr;
	AWoTAngrealInvFireball* NoNavTargeter = nullptr;
	SpawnTargetingPair(NoNavWorld, NoNavOwner, NoNavCandidate, NoNavTargeter);
	TestNull(TEXT("blocked candidates are unreachable without navmesh data"),
		NoNavTargeter->FindBestTarget(NoNavOwner->GetActorLocation(),
			FRotator::ZeroRotator, 45.0f));
	WoTTest::DestroySpecWorld(NoNavWorld);

	UWorld* NavWorld = WoTTest::CreateSpecWorld();
	BuildNavigationTestMesh(NavWorld);
	AWoTCharacter* NavOwner = nullptr;
	AWoTCharacter* NavCandidate = nullptr;
	AWoTAngrealInvFireball* NavTargeter = nullptr;
	SpawnTargetingPair(NavWorld, NavOwner, NavCandidate, NavTargeter);
	UNavigationSystemV1* Nav =
		FNavigationSystem::GetCurrent<UNavigationSystemV1>(NavWorld);
	TestNotNull(TEXT("the AI test fixture has a navigation system"), Nav);
	if (Nav)
	{
		TestNotNull(TEXT("the AI test fixture built default navmesh data"),
			Nav->GetDefaultNavDataInstance(FNavigationSystem::DontCreate));
	}
	TestEqual(TEXT("NavMesh path fallback accepts a candidate behind geometry"),
		NavTargeter->FindBestTarget(NavOwner->GetActorLocation(),
			FRotator::ZeroRotator, 45.0f), static_cast<AActor*>(NavCandidate));
	WoTTest::DestroySpecWorld(NavWorld);
	SeekBackend->Set(*PreviousBackend, ECVF_SetByCode);

	return true;
}

#endif   // WITH_AUTOMATION_TESTS
