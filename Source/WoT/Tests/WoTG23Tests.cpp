// Class automation specs for G23 — the Myrddraal two-phase teleport blink
// (Myrddraal.uc:216-527), its four triggers + DestroySeekers, the shadow-node
// pick (uc:229-306), the MyrddraalSwordAngreal source identity, and the held-
// sword hide. Behavior per exports/script/WoTPawns/Myrddraal.uc + the census
// ruling (DefensiveDetectorMyrddraal, not any map notifier). Helper names are
// file-unique (the Tests/*.cpp unity-TU merge — no anonymous-namespace guard).
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../AI/WoTBrainComponent.h"
#include "../AI/WoTNPCCharacter.h"
#include "../AI/WoTNPCController.h"
#include "../AI/WoTNPCWeapon.h"
#include "../AI/WoTCreatureData.h"
#include "../AI/WoTCreatureBehaviors.h"
#include "../AI/WoTPathNode.h"
#include "../WoTCharacter.h"
#include "../Inventory/WoTDecreaseHealthLeech.h"
#include "../Inventory/WoTMyrddraalSwordAngreal.h"
#include "../Inventory/WoTAutoTargetReflector.h"
#include "../Projectiles/WoTProjectile.h"   // FWoTDamageEvent
#include "../WoTParticleSprayer.h"
#include "WoTTestAIClasses.h"

#include "EngineUtils.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/ProjectileMovementComponent.h"

namespace
{
	void SpawnG23ActivityAnchor(UWorld* World, const FVector& Location)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		if (AWoTCharacter* Anchor = World->SpawnActor<AWoTCharacter>(
			Location, FRotator::ZeroRotator, Params))
		{
			// Activity relevance is bIsPlayer-only; dead avoids perception/combat.
			Anchor->Health = 0;
			Anchor->SetActorEnableCollision(false);
			Anchor->SetActorHiddenInGame(true);
		}
	}

	// A flying, forced-navigable Myrddraal (the AI-spec convention: no floor needed,
	// every goal reachable headless).
	AWoTNPCCharacter* SpawnG23Myrddraal(UWorld* World, const FVector& Location)
	{
		AWoTNPCCharacter* NPC = WoTCreatureData::SpawnCreature(
			*World, TEXT("Myrddraal"), Location, 1, /*bCombatReady*/ true,
			AWoTTestNPCCharacter::StaticClass());
		if (NPC)
		{
			NPC->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
			SpawnG23ActivityAnchor(World, Location);
		}
		return NPC;
	}

	// A stationary enemy pawn (team 2) — the Myrddraal's threat, and a leech victim.
	AWoTTestNPCCharacter* SpawnG23Enemy(UWorld* World, const FVector& Location)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AWoTTestNPCCharacter* NPC =
			World->SpawnActor<AWoTTestNPCCharacter>(Location, FRotator::ZeroRotator, Params);
		if (NPC)
		{
			NPC->AssignedTeam = 2;
			NPC->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
			// Unpossess so the victim never chases/blinks on its own.
			if (AController* C = NPC->GetController()) { C->UnPossess(); }
		}
		return NPC;
	}

	AWoTPathNode* SpawnG23Node(UWorld* World, const FVector& Location, bool bDark)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AWoTPathNode* Node = World->SpawnActor<AWoTPathNode>(
			AWoTPathNode::StaticClass(), Location, FRotator::ZeroRotator, Params);
		if (Node)
		{
			Node->bDark = bDark;
		}
		return Node;
	}

	AWoTNPCController* G23Controller(AWoTNPCCharacter* NPC)
	{
		return NPC ? Cast<AWoTNPCController>(NPC->GetController()) : nullptr;
	}

	UWoTMyrddraalTeleportBehavior* G23Behavior(AWoTNPCCharacter* NPC)
	{
		return NPC ? NPC->FindComponentByClass<UWoTMyrddraalTeleportBehavior>() : nullptr;
	}
}

// --- 1. Shadow-node pick: closest DARK reachable wins; last excluded; fallback ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG23ShadowNodePickTest,
	"WoT.AI.G23.ShadowNodePick",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTG23ShadowNodePickTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* Myrddraal = SpawnG23Myrddraal(World, FVector(0, 0, 100));
	UWoTMyrddraalTeleportBehavior* Teleport = G23Behavior(Myrddraal);
	if (!TestNotNull(TEXT("Myrddraal + teleport behavior"), Teleport))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	// A non-dark node at 300 (closest of all), a dark node at 500, a dark node at 1400
	// (all inside the 1600 collection radius; anchor = self, no threat set).
	SpawnG23Node(World, FVector(300, 0, 100), /*bDark*/ false);
	AWoTPathNode* NearDark = SpawnG23Node(World, FVector(500, 0, 100), /*bDark*/ true);
	AWoTPathNode* FarDark = SpawnG23Node(World, FVector(1400, 0, 100), /*bDark*/ true);

	FVector Dest;
	TestTrue(TEXT("a dark node is found"), Teleport->FindTeleportPosition(Dest));
	TestTrue(TEXT("the CLOSEST DARK node wins (the closer non-dark is rejected)"),
	         FVector::Dist(Dest, NearDark->GetActorLocation()) < 1.0f);

	// LastTeleportPosition exclusion: land on NearDark, then the next pick must skip it
	// and take the next-closest dark node (FarDark).
	Teleport->SetPendingDestination(NearDark->GetActorLocation());
	Teleport->CommitReappear();
	FVector Dest2;
	TestTrue(TEXT("a second dark node is found"), Teleport->FindTeleportPosition(Dest2));
	TestTrue(TEXT("the last teleport spot is never chosen twice"),
	         FVector::Dist(Dest2, FarDark->GetActorLocation()) < 1.0f);

	// No-node path: retail has no synthetic destination.
	NearDark->Destroy();
	FarDark->Destroy();
	FVector Fallback;
	TestFalse(TEXT("the node-less retail path has no destination"),
	         Teleport->FindTeleportPosition(Fallback));

	WoTTest::DestroySpecWorld(World);
	return true;
}

// --- 2. Two-phase blink: melee-hit → navigate → Teleporting (invulnerable) → reappear ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG23TwoPhaseBlinkTest,
	"WoT.AI.G23.TwoPhaseBlink",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTG23TwoPhaseBlinkTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* Myrddraal = SpawnG23Myrddraal(World, FVector(0, 0, 100));
	AWoTNPCController* Controller = G23Controller(Myrddraal);
	UWoTMyrddraalTeleportBehavior* Teleport = G23Behavior(Myrddraal);
	if (!TestNotNull(TEXT("Myrddraal controller"), Controller) ||
	    !TestNotNull(TEXT("teleport behavior"), Teleport))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	AWoTTestNPCCharacter* Victim = SpawnG23Enemy(World, FVector(300, 0, 100));
	SpawnG23Node(World, FVector(0, 600, 100), /*bDark*/ true);
	const int32 VictimStart = Victim->Health;
	Myrddraal->Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Victim);

	const FVector PreHit = Myrddraal->GetActorLocation();
	const int32 BlinksBefore = Teleport->GetTeleportCount();
	const float AuthoredScaleGlow = Myrddraal->ScaleGlow;

	// Trigger 1 — a landed melee swing (SetupDelayedDamageToGoal, uc:554): starts the
	// episode AND drops the health-drain leech at the hit (before the pawn walks off).
	Teleport->OnMeleeHit(Victim);

	TestTrue(TEXT("a landed swing starts the teleport episode (phase 1)"),
	         Controller->IsTeleportEpisodeActive());
	TestFalse(TEXT("phase 1 has not yet blinked"), Controller->IsTeleporting());
	UWoTDecreaseHealthLeech* Leech = Victim->FindComponentByClass<UWoTDecreaseHealthLeech>();
	if (TestNotNull(TEXT("the struck victim carries a decaying health-drain leech"), Leech))
	{
		TestEqual(TEXT("leech rate = DelayedDamageRate 1.0"), Leech->AffectResolution, 1.0f);
		TestEqual(TEXT("leech lifespan = DelayedDamageLifeSpanSecs 5.0"), Leech->Lifespan, 5.0f);
	}

	// Phase 1: the pawn NAVIGATES toward the beacon — position converges on the chosen
	// destination while still in Tracking (not yet Teleporting).
	const FVector Beacon = Teleport->GetPendingDestination();
	const float StartGap = FVector::Dist2D(PreHit, Beacon);
	WoTTest::TickWorld(World, 0.5f);
	const float MidGap = FVector::Dist2D(Myrddraal->GetActorLocation(), Beacon);
	TestTrue(TEXT("the pawn walks toward the teleport beacon (phase 1 converges)"),
	         MidGap < StartGap - 1.0f);

	// Drive to phase 2 (Teleporting).
	for (int32 i = 0; i < 600 && !Controller->IsTeleporting(); ++i)
	{
		WoTTest::TickWorld(World, 1.0f / 60.0f);
	}
	TestTrue(TEXT("arrival at the beacon enters the Teleporting state"),
	         Controller->IsTeleporting());

	// Advance to the hidden/invulnerable window (past the wind-up + first crank-up).
	for (int32 i = 0; i < 600 && !Myrddraal->IsHidden(); ++i)
	{
		WoTTest::TickWorld(World, 1.0f / 60.0f);
	}
	TestTrue(TEXT("the vanish hides the pawn"), Myrddraal->IsHidden());
	TestFalse(TEXT("collision is disabled during the vanish"),
	          Myrddraal->GetActorEnableCollision());
	TestTrue(TEXT("the wind-up fades body ScaleGlow"),
		Myrddraal->ScaleGlow < AuthoredScaleGlow);
	int32 TeleportSprayers = 0;
	for (TActorIterator<AWoTParticleSprayer> It(World); It; ++It)
	{
		TeleportSprayers += It->ConfigName == TEXT("MyrddraalTeleportSprayer2") ? 1 : 0;
	}
	TestTrue(TEXT("departure starts the production teleport sprayer"),
		TeleportSprayers >= 1);

	// INVULNERABLE (TakeDamage stub, uc:361): a hit during the window does nothing and
	// fires no TakeHit detour.
	const int32 HealthBeforeHit = Myrddraal->Health;
	FWoTDamageEvent Dmg;
	Dmg.WoTDamageType = TEXT("direct");
	Myrddraal->TakeDamage(50.0f, Dmg, Victim->GetController(), Victim);
	TestEqual(TEXT("the Myrddraal is invulnerable mid-blink"),
	          Myrddraal->Health, HealthBeforeHit);
	TestTrue(TEXT("no TakeHit detour interrupts the blink"), Controller->IsTeleporting());

	// Finish the episode → reappear at the beacon, restored, facing the threat, Tracking.
	for (int32 i = 0; i < 900 && Controller->IsTeleporting(); ++i)
	{
		WoTTest::TickWorld(World, 1.0f / 60.0f);
	}
	TestFalse(TEXT("the blink completes"), Controller->IsTeleporting());
	TestTrue(TEXT("the blink is counted"), Teleport->GetTeleportCount() > BlinksBefore);
	TestFalse(TEXT("the pawn is visible again"), Myrddraal->IsHidden());
	TestTrue(TEXT("collision is restored"), Myrddraal->GetActorEnableCollision());
	TestEqual(TEXT("reappear restores authored ScaleGlow"),
		Myrddraal->ScaleGlow, AuthoredScaleGlow);
	TeleportSprayers = 0;
	for (TActorIterator<AWoTParticleSprayer> It(World); It; ++It)
	{
		TeleportSprayers += It->ConfigName == TEXT("MyrddraalTeleportSprayer2") ? 1 : 0;
	}
	TestTrue(TEXT("arrival owns the paired production teleport sprayer"),
		TeleportSprayers >= 2);
	TestTrue(TEXT("the pawn reappeared at the chosen node/beacon"),
	         FVector::Dist2D(Myrddraal->GetActorLocation(), Beacon) < 60.0f);

	// The sword leech drains the victim over its lifespan (chase off so only the leech acts).
	Myrddraal->Brain->InvalidateGoal(EWoTGoalIndex::Threat);
	WoTTest::TickWorld(World, 2.5f);
	TestTrue(TEXT("the sword leech drains the victim over time"),
	         Victim->Health < VictimStart);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// --- 3. Active detector window: a homing seeker in range + window blinks; else not ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG23ActiveDetectorTest,
	"WoT.AI.G23.ActiveDetectorWindow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTG23ActiveDetectorTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* Myrddraal = SpawnG23Myrddraal(World, FVector(0, 0, 100));
	AWoTNPCController* Controller = G23Controller(Myrddraal);
	UWoTMyrddraalTeleportBehavior* Teleport = G23Behavior(Myrddraal);
	if (!TestNotNull(TEXT("Myrddraal controller"), Controller) ||
	    !TestNotNull(TEXT("teleport behavior"), Teleport))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	// The active detector's Defensive pulse runs only in Tracking: give the Myrddraal a
	// distant threat in the same direction the seeker approaches from (so facing +X keeps
	// the seeker in the FOV cone) and settle into Tracking.
	AWoTTestNPCCharacter* Threat = SpawnG23Enemy(World, FVector(3000, 0, 100));
	// The teleport beacon collector is 1600uu around the threat (distinct from the
	// active detector's 2048uu radius).  This node is 1300uu from that anchor.
	SpawnG23Node(World, FVector(1800, 500, 100), /*bDark*/ true);
	Myrddraal->Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Threat);
	// Myrddraal now performs the shipped BowUp preparation before resuming this
	// acquired tracking state, so allow the real notify-driven episode to finish.
	for (int32 i = 0; i < 240 && Controller->GetBrainState() != EWoTBrainState::Tracking; ++i)
	{
		WoTTest::TickWorld(World, 1.0f / 60.0f);
	}
	TestEqual(TEXT("the Myrddraal is tracking its threat"),
	          static_cast<int32>(Controller->GetBrainState()),
	          static_cast<int32>(EWoTBrainState::Tracking));

	// Negative: a seeker in range but NOT targeting the Myrddraal never triggers.
	AWoTTestSeeker* Other = World->SpawnActor<AWoTTestSeeker>(
		FVector(1500, 0, 100), FRotator::ZeroRotator);
	Other->Destination = nullptr;
	Other->ProjectileMovement->Velocity = FVector(-500, 0, 0);
	TestFalse(TEXT("the active detector scan ignores a seeker not aimed at us"),
	          Teleport->DetectActiveTeleportOffender());

	// DefensiveDetectorMyrddraal.RejectOffender overrides the base rejection
	// chain: once Destination==Self, it accepts without the inherited FOV or
	// response-time filters.
	Other->Destination = Myrddraal;
	Other->SetActorLocation(FVector(200, 0, 100));
	TestTrue(TEXT("the Myrddraal override accepts an aimed close seeker"),
	          Teleport->DetectActiveTeleportOffender());

	Other->SetActorLocation(FVector(-1500, 0, 100));
	Other->ProjectileMovement->Velocity = FVector::ZeroVector;
	TestTrue(TEXT("the override also bypasses FOV and arrival-time rejection"),
	         Teleport->DetectActiveTeleportOffender());

	// The controller's Defensive pulse starts the episode from that detection.
	const int32 BlinksBefore = Teleport->GetTeleportCount();
	for (int32 i = 0; i < 180 && !Controller->IsTeleportEpisodeActive(); ++i)
	{
		WoTTest::TickWorld(World, 1.0f / 60.0f);
	}
	TestTrue(TEXT("the active detector starts a blink episode"),
	         Controller->IsTeleportEpisodeActive() ||
	         Teleport->GetTeleportCount() > BlinksBefore);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// --- 4. DestroySeekers: seekers un-lock; removable bad non-projectile leech/reflector shed ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG23DestroySeekersTest,
	"WoT.AI.G23.DestroySeekers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTG23DestroySeekersTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* Myrddraal = SpawnG23Myrddraal(World, FVector(0, 0, 100));
	UWoTMyrddraalTeleportBehavior* Teleport = G23Behavior(Myrddraal);
	if (!TestNotNull(TEXT("teleport behavior"), Teleport))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	// A seeker homing on us.
	AWoTTestSeeker* Seeker = World->SpawnActor<AWoTTestSeeker>(
		FVector(400, 0, 100), FRotator::ZeroRotator);
	Seeker->Destination = Myrddraal;

	// A removable, deleterious, non-projectile leech (shed) + a non-removable one (kept).
	UWoTDecreaseHealthLeech* BadLeech = NewObject<UWoTDecreaseHealthLeech>(Myrddraal);
	BadLeech->bRemovable = true;
	BadLeech->bDeleterious = true;
	BadLeech->bFromProjectile = false;
	BadLeech->Lifespan = 100.0f;
	BadLeech->AttachTo(Myrddraal);
	UWoTDecreaseHealthLeech* StickyLeech = NewObject<UWoTDecreaseHealthLeech>(Myrddraal);
	StickyLeech->bRemovable = false;   // a cleanse can't strip it
	StickyLeech->bDeleterious = true;
	StickyLeech->bFromProjectile = false;
	StickyLeech->Lifespan = 100.0f;
	StickyLeech->AttachTo(Myrddraal);

	// A removable, deleterious, non-projectile reflector (shed).
	UWoTAutoTargetReflector* BadReflector = NewObject<UWoTAutoTargetReflector>(Myrddraal);
	BadReflector->bRemovable = true;
	BadReflector->bDeleterious = true;
	BadReflector->bFromProjectile = false;
	BadReflector->Install(Myrddraal);

	Teleport->DestroySeekers();

	TestNull(TEXT("the seeker's lock is dropped"), Seeker->Destination.Get());
	TestFalse(TEXT("the removable bad leech is shed"), IsValid(BadLeech));
	TestTrue(TEXT("the non-removable leech is kept"), IsValid(StickyLeech));
	TestFalse(TEXT("the removable bad reflector is shed"), IsValid(BadReflector));

	WoTTest::DestroySpecWorld(World);
	return true;
}

// --- 5. Sword source: the melee leech carries the MyrddraalSwordAngreal (element Spirit) ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG23SwordSourceTest,
	"WoT.AI.G23.SwordSource",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTG23SwordSourceTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* Myrddraal = SpawnG23Myrddraal(World, FVector(0, 0, 100));
	UWoTMyrddraalTeleportBehavior* Teleport = G23Behavior(Myrddraal);
	if (!TestNotNull(TEXT("teleport behavior"), Teleport))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	// PreBeginPlay spawned the hidden Spirit source (uc:83).
	AWoTMyrddraalSwordAngreal* Sword = Teleport->GetSourceAngreal();
	if (TestNotNull(TEXT("the Myrddraal spawned its sword source angreal"), Sword))
	{
		TestTrue(TEXT("the sword source is element Spirit"), Sword->bElementSpirit);
	}

	AWoTTestNPCCharacter* Victim = SpawnG23Enemy(World, FVector(300, 0, 100));
	SpawnG23Node(World, FVector(0, 600, 100), /*bDark*/ true);
	Myrddraal->Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Victim);
	Teleport->OnMeleeHit(Victim);

	UWoTDecreaseHealthLeech* Leech = Victim->FindComponentByClass<UWoTDecreaseHealthLeech>();
	if (TestNotNull(TEXT("the struck victim carries the sword leech"), Leech))
	{
		TestTrue(TEXT("the leech's source is the MyrddraalSwordAngreal (SetSourceAngreal, uc:573)"),
		         Cast<AWoTMyrddraalSwordAngreal>(Leech->SourceAngreal.Get()) == Sword && Sword != nullptr);
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

// --- 6. Held sword hides WITH the pawn during the hidden phase and returns ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG23HeldSwordHidesTest,
	"WoT.AI.G23.HeldSwordHides",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTG23HeldSwordHidesTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* Myrddraal = SpawnG23Myrddraal(World, FVector(0, 0, 100));
	AWoTNPCController* Controller = G23Controller(Myrddraal);
	UWoTMyrddraalTeleportBehavior* Teleport = G23Behavior(Myrddraal);
	if (!TestNotNull(TEXT("controller"), Controller) ||
	    !TestNotNull(TEXT("teleport behavior"), Teleport))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	AWoTTestNPCCharacter* Victim = SpawnG23Enemy(World, FVector(300, 0, 100));
	SpawnG23Node(World, FVector(0, 600, 100), /*bDark*/ true);
	Myrddraal->Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Victim);

	// The G19 melee sword in hand (the held weapon that must hide with the pawn).
	Myrddraal->EquipMeleeWeapon();
	AWoTNPCWeapon* Held = Myrddraal->GetHeldWeapon();
	if (!TestNotNull(TEXT("the Myrddraal holds its sword"), Held))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	TestFalse(TEXT("the sword starts visible"), Held->IsHidden());

	Teleport->OnMeleeHit(Victim);
	for (int32 i = 0; i < 900 && !Myrddraal->IsHidden(); ++i)
	{
		WoTTest::TickWorld(World, 1.0f / 60.0f);
	}
	TestTrue(TEXT("the pawn is hidden mid-blink"), Myrddraal->IsHidden());
	TestTrue(TEXT("the held sword hides WITH the pawn"), Held->IsHidden());

	for (int32 i = 0; i < 900 && Controller->IsTeleporting(); ++i)
	{
		WoTTest::TickWorld(World, 1.0f / 60.0f);
	}
	TestFalse(TEXT("the pawn is visible after the blink"), Myrddraal->IsHidden());
	TestFalse(TEXT("the held sword returns with the pawn"), Held->IsHidden());

	WoTTest::DestroySpecWorld(World);
	return true;
}

// --- 7. Beacon drop mid-phase-1 ends the episode and re-arms the triggers (F1) ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG23BeaconDropReArmTest,
	"WoT.AI.G23.BeaconDropReArm",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTG23BeaconDropReArmTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* Myrddraal = SpawnG23Myrddraal(World, FVector(0, 0, 100));
	AWoTNPCController* Controller = G23Controller(Myrddraal);
	UWoTMyrddraalTeleportBehavior* Teleport = G23Behavior(Myrddraal);
	if (!TestNotNull(TEXT("controller"), Controller) ||
	    !TestNotNull(TEXT("teleport behavior"), Teleport))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	// A FAR threat: the Myrddraal is solidly in Tracking (so TickTracking's re-select runs),
	// but 3000uu away it never autonomously reaches melee range during the drop window — so
	// the "latch cleared" assertion isn't muddied by a fresh melee re-trigger.
	AWoTTestNPCCharacter* Victim = SpawnG23Enemy(World, FVector(3000, 0, 100));
	// Both authored dark nodes are inside the production 1600uu collection radius
	// around the threat while remaining off the direct chase line.
	SpawnG23Node(World, FVector(1800, 500, 100), /*bDark*/ true);
	SpawnG23Node(World, FVector(1800, -500, 100), /*bDark*/ true);
	Myrddraal->Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Victim);
	for (int32 i = 0; i < 60 && Controller->GetBrainState() != EWoTBrainState::Tracking; ++i)
	{
		WoTTest::TickWorld(World, 1.0f / 60.0f);
	}

	// A landed swing starts the walk-to-beacon (phase 1: Intermediate beacon outranks the
	// threat, not yet hidden).
	Teleport->OnMeleeHit(Victim);
	TestTrue(TEXT("phase 1 is up"), Controller->IsTeleportEpisodeActive());
	TestFalse(TEXT("phase 1 has not blinked"), Controller->IsTeleporting());

	// Force the beacon to drop before arrival (invalidate the Intermediate goal). The
	// goal-invalid re-select in TickTracking must clear the latch — without the fix it
	// stays set forever and blocks every future trigger.
	Myrddraal->Brain->InvalidateGoal(EWoTGoalIndex::Intermediate);
	WoTTest::TickWorld(World, 0.2f);
	TestFalse(TEXT("a dropped beacon ends the episode (latch cleared)"),
	          Controller->IsTeleportEpisodeActive());

	// A subsequent trigger starts a FRESH episode (the triggers re-armed).
	Teleport->OnMeleeHit(Victim);
	TestTrue(TEXT("a new trigger re-arms and starts a fresh episode"),
	         Controller->IsTeleportEpisodeActive());

	WoTTest::DestroySpecWorld(World);
	return true;
}

// --- 8. Reachability: the closest node marked unreachable yields to the next-closest (F2) ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG23ShadowNodeReachabilityTest,
	"WoT.AI.G23.ShadowNodeReachability",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTG23ShadowNodeReachabilityTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* Myrddraal = SpawnG23Myrddraal(World, FVector(0, 0, 100));
	UWoTMyrddraalTeleportBehavior* Teleport = G23Behavior(Myrddraal);
	if (!TestNotNull(TEXT("teleport behavior"), Teleport))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	// Two dark nodes inside the collection radius; the nearer one is marked unreachable
	// (the headless nav seam) so the pick must fall through to the next-closest reachable.
	AWoTPathNode* NearDark = SpawnG23Node(World, FVector(500, 0, 100), /*bDark*/ true);
	AWoTPathNode* FarDark = SpawnG23Node(World, FVector(1400, 0, 100), /*bDark*/ true);
	Teleport->UnreachableNodesForTest.Add(NearDark->GetActorLocation());

	FVector Dest;
	TestTrue(TEXT("a reachable dark node is found"), Teleport->FindTeleportPosition(Dest));
	TestTrue(TEXT("the unreachable closest node is skipped for the next-closest reachable one"),
	         FVector::Dist(Dest, FarDark->GetActorLocation()) < 1.0f);

	// Clear the block: the now-reachable NearDark is closest, and FarDark (just used) is the
	// excluded LastTeleportPosition — so the pick lands on NearDark.
	Teleport->UnreachableNodesForTest.Reset();
	FVector Dest2;
	TestTrue(TEXT("a second reachable dark node is found"), Teleport->FindTeleportPosition(Dest2));
	TestTrue(TEXT("with the block cleared the closest reachable node wins (last excluded)"),
	         FVector::Dist(Dest2, NearDark->GetActorLocation()) < 1.0f);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// --- 9. Force-exit mid-vanish restores collision/visibility (EndState(Teleporting), F3) ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG23ForceExitRestoreTest,
	"WoT.AI.G23.ForceExitRestore",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTG23ForceExitRestoreTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* Myrddraal = SpawnG23Myrddraal(World, FVector(0, 0, 100));
	AWoTNPCController* Controller = G23Controller(Myrddraal);
	UWoTMyrddraalTeleportBehavior* Teleport = G23Behavior(Myrddraal);
	if (!TestNotNull(TEXT("controller"), Controller) ||
	    !TestNotNull(TEXT("teleport behavior"), Teleport))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	AWoTTestNPCCharacter* Victim = SpawnG23Enemy(World, FVector(300, 0, 100));
	SpawnG23Node(World, FVector(0, 600, 100), /*bDark*/ true);
	Myrddraal->Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Victim);
	Myrddraal->EquipMeleeWeapon();
	AWoTNPCWeapon* Held = Myrddraal->GetHeldWeapon();
	if (!TestNotNull(TEXT("the Myrddraal holds its sword"), Held))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	// Drive into the hidden (phase-2) vanish window.
	Teleport->OnMeleeHit(Victim);
	for (int32 i = 0; i < 900 && !Myrddraal->IsHidden(); ++i)
	{
		WoTTest::TickWorld(World, 1.0f / 60.0f);
	}
	TestTrue(TEXT("the pawn is hidden mid-vanish"), Myrddraal->IsHidden());
	TestFalse(TEXT("collision is off mid-vanish"), Myrddraal->GetActorEnableCollision());

	// Force-exit the state before ExitMyrddraalTeleporting (a teardown/unpossess analog):
	// EndState(Teleporting) must restore collision + visibility of the pawn and its sword.
	Controller->SetBrainStateForG20Test(EWoTBrainState::Tracking);
	TestFalse(TEXT("a force-exit unhides the pawn"), Myrddraal->IsHidden());
	TestTrue(TEXT("a force-exit restores collision"), Myrddraal->GetActorEnableCollision());
	TestFalse(TEXT("a force-exit unhides the held sword"), Held->IsHidden());

	WoTTest::DestroySpecWorld(World);
	return true;
}

#endif  // WITH_AUTOMATION_TESTS
