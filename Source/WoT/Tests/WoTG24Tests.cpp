// G24: the Legion (Hound champion) ranged seeker + targeting + hybrid seeker/stomp
// band arbitration. Fixed 1/60 world steps (the G20 convention): UWorld clamps timers
// at frame granularity, so exact boundaries are not portable across the Mac/Windows lanes.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../WoTCharacter.h"
#include "../AI/WoTBrainComponent.h"
#include "../AI/WoTCasterComponent.h"
#include "../AI/WoTCreatureBehaviors.h"
#include "../AI/WoTCreatureData.h"
#include "../AI/WoTNPCCharacter.h"
#include "../AI/WoTNPCController.h"
#include "../AI/WoTRangeBands.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../WoTSpriteBillboard.h"
#include "../Inventory/WoTLegionInvSeeker.h"
#include "../Projectiles/WoTLegionProjectile.h"
#include "../WoTSoundTable.h"
#include "../WoTVertexAnimComponent.h"
#include "WoTTestAIClasses.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SphereComponent.h"

namespace
{
	void SpawnG24ActivityAnchor(UWorld* World, const FVector& Location)
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

	AWoTNPCCharacter* SpawnG24Legion(UWorld* World, const FVector& Location)
	{
		AWoTNPCCharacter* NPC = WoTCreatureData::SpawnCreature(
			*World, TEXT("Legion"), Location, 1, true, AWoTTestNPCCharacter::StaticClass());
		if (NPC)
		{
			NPC->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
			SpawnG24ActivityAnchor(World, Location);
		}
		return NPC;
	}

	AWoTTestNPCCharacter* SpawnG24Target(UWorld* World, const FVector& Location, uint8 Team)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AWoTTestNPCCharacter* Target = World->SpawnActor<AWoTTestNPCCharacter>(
			Location, FRotator::ZeroRotator, Params);
		if (Target)
		{
			Target->AssignedTeam = Team;
			Target->MeleeRange = 0.0f;
			Target->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
			if (AController* Controller = Target->GetController())
			{
				Controller->UnPossess();
			}
		}
		return Target;
	}

	AWoTLegionProjectile* SpawnG24Spirit(UWorld* World, const FVector& Location)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		return World->SpawnActor<AWoTLegionProjectile>(Location, FRotator::ZeroRotator, Params);
	}

	AWoTLegionInvSeeker* GetG24Launcher(AWoTNPCCharacter* Legion)
	{
		if (const UWoTInventoryComponent* Inv =
			Legion ? Legion->FindComponentByClass<UWoTInventoryComponent>() : nullptr)
		{
			for (const TObjectPtr<AWoTInventory>& Item : Inv->Items)
			{
				if (AWoTLegionInvSeeker* Seeker = Cast<AWoTLegionInvSeeker>(Item))
				{
					return Seeker;
				}
			}
		}
		return nullptr;
	}

	int32 CountG24Spirits(UWorld* World)
	{
		int32 Count = 0;
		for (TActorIterator<AWoTLegionProjectile> It(World); It; ++It)
		{
			if (IsValid(*It))
			{
				++Count;
			}
		}
		return Count;
	}

	AWoTLegionProjectile* NewestG24Spirit(UWorld* World)
	{
		AWoTLegionProjectile* Newest = nullptr;
		for (TActorIterator<AWoTLegionProjectile> It(World); It; ++It)
		{
			if (IsValid(*It))
			{
				Newest = *It;   // one-shot specs spawn a single spirit at a time
			}
		}
		return Newest;
	}
}

// ---------------------------------------------------------------------------
// F1 — the seeker leaf CDO + shootable put-down.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG24SeekerCDOSpec,
	"WoT.AI.G24LegionSeekerCDO", EAutomationTestFlags::EditorContext |
	EAutomationTestFlags::ProductFilter)
bool FWoTG24SeekerCDOSpec::RunTest(const FString&)
{
	// LegionProjectile.uc defaultproperties (:322-361). Acceleration=20 (uc:336, the
	// leaf override of the base 16) and LifeSpan=0 (uc:346, infinite — GetMaxRange=MaxInt).
	const AWoTLegionProjectile* CDO = GetDefault<AWoTLegionProjectile>();
	TestEqual(TEXT("Speed"), CDO->Speed, 250.0f);
	TestEqual(TEXT("MaxSpeed"), CDO->MaxSpeed, 450.0f);
	TestEqual(TEXT("Acceleration"), CDO->Acceleration, 20.0f);
	TestEqual(TEXT("Damage"), CDO->Damage, 25.0f);
	TestEqual(TEXT("MomentumTransfer"), CDO->MomentumTransfer, 200);
	TestEqual(TEXT("Health"), CDO->Health, 30);
	TestTrue(TEXT("bProjTarget (shootable)"), CDO->bProjTarget);
	TestFalse(TEXT("bNotifiesDestination off (no seeker HUD)"), CDO->bNotifiesDestination);
	TestFalse(TEXT("bRequiresDestination off (straight if no target)"), CDO->bRequiresDestination);
	TestTrue(TEXT("bAlwaysRelevant"), CDO->bAlwaysRelevant);
	TestEqual(TEXT("LaunchOffset X"), CDO->LaunchOffset.X, 170.0);
	TestEqual(TEXT("LaunchOffset Z"), CDO->LaunchOffset.Z, -110.0);
	TestFalse(TEXT("retail spawn sound is bound"), CDO->SpawnSound.IsNull());
	TestFalse(TEXT("retail impact sound is bound"), CDO->ImpactSound.IsNull());
	TestFalse(TEXT("retail ambient sound is bound"), CDO->AmbientLoopSound.IsNull());

	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTLegionProjectile* Spirit = SpawnG24Spirit(World, FVector(0, 0, 100));
	AWoTTestNPCCharacter* Homed = SpawnG24Target(World, FVector(1000, 0, 100), 2);
	AWoTTestNPCCharacter* Bystander = SpawnG24Target(World, FVector(60, 0, 100), 2);
	if (!TestNotNull(TEXT("spirit"), Spirit) || !TestNotNull(TEXT("bystander"), Bystander))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	TestEqual(TEXT("infinite lifespan (LegionProjectile.uc:346 LifeSpan=0)"),
		Spirit->GetLifeSpan(), 0.0f);
	TestTrue(TEXT("spirit head is attached to the rotating presentation root"),
		Spirit->DisplayMesh->GetAttachParent() == Spirit->GetProjectilePresentationRoot());
	TestEqual(TEXT("production LegionSpiritHead mesh is bound"),
		Spirit->DisplayMesh->GetDisplayMeshName(), FName(TEXT("LegionSpiritHead")));
	TestTrue(TEXT("spirit light is visible outside a dedicated server"),
		Spirit->SpiritLight->IsVisible());
	TestTrue(TEXT("retail two-sprite spirit tail requests both cache-backed wet textures"),
		Spirit->TailSpriteA->HasConfiguredFrames()
		&& Spirit->TailSpriteB->HasConfiguredFrames());

	Spirit->SetDestination(Homed);   // seeking (bSeeking) mid-flight
	TestTrue(TEXT("spirit is seeking after SetDestination"), Spirit->bSeeking);
	const int32 BystanderBefore = Bystander->Health;

	// 30 damage kills it into the VeryDead dive — no explosion damage to bystanders.
	FDamageEvent Shot;
	Spirit->TakeDamage(30.0f, Shot, nullptr, nullptr);
	TestTrue(TEXT("30 damage puts the spirit down (VeryDead)"), Spirit->IsVeryDead());
	TestNull(TEXT("VeryDead drops the seek lock"), Spirit->Destination.Get());
	TestEqual(TEXT("no explosion damage to a bystander"), Bystander->Health, BystanderBefore);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// ---------------------------------------------------------------------------
// F2 — the launcher: config, homing-with-target, straight-without, cadence gates.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG24LauncherSpec,
	"WoT.AI.G24LegionLauncher", EAutomationTestFlags::EditorContext |
	EAutomationTestFlags::ProductFilter)
bool FWoTG24LauncherSpec::RunTest(const FString&)
{
	// LegionInvSeeker.uc defaultproperties.
	const AWoTLegionInvSeeker* CDO = GetDefault<AWoTLegionInvSeeker>();
	TestEqual(TEXT("ProjectileClass is the spirit"),
		CDO->ProjectileClass.Get(), AWoTLegionProjectile::StaticClass());
	TestEqual(TEXT("RoundsPerMinute 120 (0.5s/shot)"), CDO->RoundsPerMinute, 120.0f);
	TestEqual(TEXT("ChargeCost 0 (infinite)"), CDO->ChargeCost, 0);
	TestEqual(TEXT("MaxChargeUsedInterval 0.1"), CDO->MaxChargeUsedInterval, 0.1f);
	TestTrue(TEXT("bElementSpirit"), CDO->bElementSpirit);
	TestTrue(TEXT("bOffensive"), CDO->bOffensive);

	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* Legion = SpawnG24Legion(World, FVector(0, 0, 100));
	AWoTLegionInvSeeker* Launcher = GetG24Launcher(Legion);
	if (!TestNotNull(TEXT("Legion"), Legion) || !TestNotNull(TEXT("granted launcher"), Launcher))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	WoTTest::TickWorld(World, 0.05f);
	TestEqual(TEXT("Legion owns three client-local orbiting spirit heads"),
		Legion->LegionSpirits.Num(), 3);
	for (const UWoTVertexAnimComponent* Orbit : Legion->LegionSpirits)
	{
		TestTrue(TEXT("orbiting spirit uses production LegionSpiritHead"), Orbit
			&& Orbit->GetDisplayMeshName() == TEXT("LegionSpiritHead"));
	}
	// The Legion's caster carries the adapter's 3s selection gate (Legion.uc:511).
	if (const UWoTCasterComponent* Caster = Legion->FindComponentByClass<UWoTCasterComponent>())
	{
		TestEqual(TEXT("caster MinOffensiveUsageInterval 3"),
			Caster->MinOffensiveUsageInterval, 3.0f);
	}
	// TimeSeconds must be > NextCastTime(0) before the launcher will fire.
	WoTTest::TickWorld(World, 0.2f);

	// With a victim -> the seeker homes at it (Destination == victim).
	AWoTTestNPCCharacter* Victim = SpawnG24Target(World, FVector(1500, 0, 100), 2);
	Launcher->SetVictim(Victim);
	Launcher->CastArtifact();
	Launcher->UnCastArtifact();
	AWoTLegionProjectile* Homing = NewestG24Spirit(World);
	bool bShootSpiritCue = false;
	for (TActorIterator<AWoTSoundBurst> It(World); It; ++It)
	{
		bShootSpiritCue |= It->SoundSubPath.ToString().Contains(TEXT("ShootSpirit"),
			ESearchCase::IgnoreCase);
	}
	TestFalse(TEXT("retail ShootSpirit slot-name mismatch remains silent"), bShootSpiritCue);
	if (TestNotNull(TEXT("a seeker launched with a target"), Homing))
	{
		TestEqual(TEXT("seeker homes at the victim"), Homing->Destination.Get(),
			static_cast<AActor*>(Victim));
		TestTrue(TEXT("launched seeker is seeking"), Homing->bSeeking);
		Homing->Destroy();
	}
	// The 0.5s refire gate stamped after the shot (RoundsPerMinute 120).
	TestTrue(TEXT("NextCastTime advanced ~0.5s after the shot"),
		Launcher->NextCastTime > World->GetTimeSeconds());

	// With no victim and no candidate in the world -> a straight launch (no Destination).
	Launcher->NextCastTime = 0.0f;   // clear the refire gate for this leg
	Launcher->SetVictim(nullptr);
	Victim->Destroy();
	Launcher->CastArtifact();
	Launcher->UnCastArtifact();
	AWoTLegionProjectile* Straight = NewestG24Spirit(World);
	if (TestNotNull(TEXT("a seeker launched with no target"), Straight))
	{
		TestNull(TEXT("no target -> straight launch, no Destination"),
			Straight->Destination.Get());
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

// ---------------------------------------------------------------------------
// F2b — the caster single-shot release + 3s selection gate (the 0-cost weave path).
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG24SelectionGateSpec,
	"WoT.AI.G24LegionSelectionGate", EAutomationTestFlags::EditorContext |
	EAutomationTestFlags::ProductFilter)
bool FWoTG24SelectionGateSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* Legion = SpawnG24Legion(World, FVector(0, 0, 100));
	AWoTTestNPCCharacter* Victim = SpawnG24Target(World, FVector(1500, 0, 100), 2);
	UWoTCasterComponent* Caster = Legion
		? Legion->FindComponentByClass<UWoTCasterComponent>() : nullptr;
	if (!TestNotNull(TEXT("Legion"), Legion) || !TestNotNull(TEXT("caster"), Caster))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	// GetPriority's rounds-per-minute cooldown (60/120 = 0.5s) measures from
	// LastChargeUsedTime=0 against a world clock that also starts near zero, so the
	// weave is unusable for the first half second (retail has the same startup
	// block, Level.TimeSeconds-based). Tick past it before the first selection.
	WoTTest::TickWorld(World, 0.6f);

	// The first selection is ungated (LastOffensiveUsageTime starts -inf).
	AWoTAngrealInventory* First = Caster->SelectOffensiveWeave(Victim);
	if (!TestNotNull(TEXT("first offensive selection succeeds"), First))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Caster->BeginCast(First);
	TestTrue(TEXT("caster is casting after BeginCast"), Caster->IsCasting());
	// A ChargeCost=0 launcher fires once and the cast ENDS (the CastUseCount signal),
	// rather than the volley hanging forever waiting on a charge that never drops.
	WoTTest::TickWorld(World, 0.1f);
	TestFalse(TEXT("single-shot seeker released the cast"), Caster->IsCasting());
	TestTrue(TEXT("one seeker fired"), CountG24Spirits(World) >= 1);

	// The 3s adapter interval gate now blocks the next selection.
	TestNull(TEXT("re-selection within 3s is gated"),
		Caster->SelectOffensiveWeave(Victim));

	WoTTest::DestroySpecWorld(World);
	return true;
}

// ---------------------------------------------------------------------------
// F3 — the hybrid band arbitration (Legion.uc:193-207).
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG24ArbitrationSpec,
	"WoT.AI.G24LegionArbitration", EAutomationTestFlags::EditorContext |
	EAutomationTestFlags::ProductFilter)
bool FWoTG24ArbitrationSpec::RunTest(const FString&)
{
	// 3a — the band shape: a Legion with the seeker weave is handed LegionDefault, NOT
	// CasterDefault. Visible/Reachable/Reached HOLD at the stomp reach (500) so beyond it
	// the pawn CLOSES to the stomp band; Pathable/UnNavigable HOLD at the seek range
	// (3200) and cast even without LOS. Every bound band casts (Attack=Caster).
	{
		UWorld* World = WoTTest::CreateSpecWorld();
		AWoTNPCCharacter* Legion = SpawnG24Legion(World, FVector(0, 0, 100));
		AWoTNPCController* Controller = Legion
			? Cast<AWoTNPCController>(Legion->GetController()) : nullptr;
		if (TestNotNull(TEXT("Legion"), Legion) && TestNotNull(TEXT("controller"), Controller))
		{
			const FWoTRangeBandSet& Bands = Controller->GetTrackingBands();
			const FWoTRangeBand& Visible = Bands.Band(EWoTGoalProximity::Visible);
			const FWoTRangeBand& UnNav = Bands.Band(EWoTGoalProximity::UnNavigable);
			TestTrue(TEXT("Visible band holds at the stomp reach"), Visible.bHold);
			TestEqual(TEXT("Visible standoff == StompRange 500 (closes when farther)"),
				Visible.StandoffDistance, 500.0f);
			TestEqual(TEXT("Visible band casts (seeker while closing)"),
				static_cast<int32>(Visible.Attack), static_cast<int32>(EWoTBandAttack::Caster));
			TestEqual(TEXT("no-LOS band holds/seeks at the 3200 seek range"),
				UnNav.StandoffDistance, 3200.0f);
			TestEqual(TEXT("no-LOS band casts (seeker even at range)"),
				static_cast<int32>(UnNav.Attack), static_cast<int32>(EWoTBandAttack::Caster));
			TestFalse(TEXT("no prefer-visibility flip"), Bands.bPreferVisibility);
		}
		WoTTest::DestroySpecWorld(World);
	}

	// 3b — threat NOT visible at 2000: the seeker fires (no stomp — out of reach and blind).
	{
		UWorld* World = WoTTest::CreateSpecWorld();
		AWoTNPCCharacter* Legion = SpawnG24Legion(World, FVector(0, 0, 100));
		AWoTTestNPCCharacter* Threat = SpawnG24Target(World, FVector(2000, 0, 100), 2);
		UWoTTestBrainComponent* Brain = Legion
			? Cast<UWoTTestBrainComponent>(Legion->Brain.Get()) : nullptr;
		if (TestNotNull(TEXT("Legion"), Legion) && TestNotNull(TEXT("brain"), Brain))
		{
			Brain->bForceVisible = false;    // behind the LOS gate
			Brain->bForceReachable = false;
			Brain->bForcePathable = true;    // pathable -> the seek-hold band
			Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Threat);
			for (int32 Frame = 0; Frame < 900 && CountG24Spirits(World) == 0; ++Frame)
			{
				WoTTest::TickWorld(World, 1.0f / 60.0f);
			}
			AWoTNPCController* Controller = Cast<AWoTNPCController>(Legion->GetController());
			UWoTCasterComponent* Caster = Legion->FindComponentByClass<UWoTCasterComponent>();
			TestTrue(FString::Printf(TEXT("a not-visible threat draws a seeker "
				"(state %d, proximity %d, threat %d, casting %d)"),
				Controller ? static_cast<int32>(Controller->GetBrainState()) : -1,
				Controller ? static_cast<int32>(Controller->GetTrackingProximity()) : -1,
				Brain->IsGoalValid(EWoTGoalIndex::Threat) ? 1 : 0,
				Caster && Caster->IsCasting() ? 1 : 0),
				CountG24Spirits(World) >= 1);
		}
		WoTTest::DestroySpecWorld(World);
	}

	// 3c — threat VISIBLE at 400 (inside the 500 stomp reach): the volcano stomp lands and
	// the seeker is withheld (WithinMaxMeleeDistance -> melee arm, uc:213-219).
	{
		UWorld* World = WoTTest::CreateSpecWorld();
		AWoTNPCCharacter* Legion = SpawnG24Legion(World, FVector(0, 0, 100));
		AWoTTestNPCCharacter* Threat = SpawnG24Target(World, FVector(400, 0, 100), 2);
		UWoTLegionSpiritsStompBehavior* Stomp = Legion
			? Legion->FindComponentByClass<UWoTLegionSpiritsStompBehavior>() : nullptr;
		UWoTTestBrainComponent* Brain = Legion
			? Cast<UWoTTestBrainComponent>(Legion->Brain.Get()) : nullptr;
		if (TestNotNull(TEXT("Legion"), Legion) && TestNotNull(TEXT("stomp behavior"), Stomp) &&
			TestNotNull(TEXT("brain"), Brain))
		{
			Brain->bForceVisible = true;     // visible AND inside melee reach -> stomp
			Brain->bForceReachable = true;
			Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Threat);
			for (int32 Frame = 0; Frame < 300 && !Stomp->IsStompPending(); ++Frame)
			{
				WoTTest::TickWorld(World, 1.0f / 60.0f);
			}
			TestTrue(TEXT("the stomp episode starts"), Stomp->IsStompPending());
			TestEqual(TEXT("the seeker is withheld at visible melee range"),
				CountG24Spirits(World), 0);
		}
		WoTTest::DestroySpecWorld(World);
	}

	return true;
}

// ---------------------------------------------------------------------------
// F4 — the per-pawn targetable set (LegionTargetingReflector.uc:8).
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG24TargetableSetSpec,
	"WoT.AI.G24LegionTargetableSet", EAutomationTestFlags::EditorContext |
	EAutomationTestFlags::ProductFilter)
bool FWoTG24TargetableSetSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* Legion = SpawnG24Legion(World, FVector(0, 0, 100));
	AWoTTestNPCCharacter* Player = SpawnG24Target(World, FVector(500, 0, 100), 2);
	AWoTLegionProjectile* Decoy = SpawnG24Spirit(World, FVector(200, 0, 100));
	if (!TestNotNull(TEXT("Legion"), Legion) || !TestNotNull(TEXT("player pawn"), Player) ||
		!TestNotNull(TEXT("spirit decoy"), Decoy))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	TestTrue(TEXT("Legion excludes the spirit family"),
		Legion->bExcludesLegionProjectileTarget);
	TestFalse(TEXT("a plain pawn keeps the base list"),
		Player->bExcludesLegionProjectileTarget);

	FActorSpawnParameters LegionOwned;
	LegionOwned.Owner = Legion;
	LegionOwned.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTLegionInvSeeker* LegionWeave =
		World->SpawnActor<AWoTLegionInvSeeker>(FVector::ZeroVector, FRotator::ZeroRotator, LegionOwned);

	FActorSpawnParameters PlayerOwned;
	PlayerOwned.Owner = Player;
	PlayerOwned.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTLegionInvSeeker* PlayerWeave =
		World->SpawnActor<AWoTLegionInvSeeker>(FVector::ZeroVector, FRotator::ZeroRotator, PlayerOwned);

	if (TestNotNull(TEXT("Legion-owned weave"), LegionWeave) &&
		TestNotNull(TEXT("player-owned weave"), PlayerWeave))
	{
		TestFalse(TEXT("the Legion's weave ignores a spirit decoy"),
			LegionWeave->IsTargetable(Decoy));
		TestTrue(TEXT("a player-owned weave still targets a spirit (base list)"),
			PlayerWeave->IsTargetable(Decoy));
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

// ---------------------------------------------------------------------------
// F5/F6 — seeker lifecycle: Myrddraal shake-off + killed-victim cleanup.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG24SeekerLifecycleSpec,
	"WoT.AI.G24LegionSeekerLifecycle", EAutomationTestFlags::EditorContext |
	EAutomationTestFlags::ProductFilter)
bool FWoTG24SeekerLifecycleSpec::RunTest(const FString&)
{
	// F5 — a Legion spirit homing on a Myrddraal loses its lock when the Myrddraal blinks
	// (DestroySeekers -> ClearSeekersTargetingSelf iterates the SeekingProjectile BASE, so
	// the leaf is covered, Myrddraal.uc:465-478).
	{
		UWorld* World = WoTTest::CreateSpecWorld();
		AWoTNPCCharacter* Myrddraal = WoTCreatureData::SpawnCreature(
			*World, TEXT("Myrddraal"), FVector(0, 0, 100), 1, true,
			AWoTTestNPCCharacter::StaticClass());
		AWoTLegionProjectile* Spirit = SpawnG24Spirit(World, FVector(300, 0, 100));
		UWoTMyrddraalTeleportBehavior* Teleport = Myrddraal
			? Myrddraal->FindComponentByClass<UWoTMyrddraalTeleportBehavior>() : nullptr;
		if (TestNotNull(TEXT("Myrddraal"), Myrddraal) && TestNotNull(TEXT("spirit"), Spirit) &&
			TestNotNull(TEXT("teleport behavior"), Teleport))
		{
			Spirit->SetDestination(Myrddraal);
			TestEqual(TEXT("spirit locked on the Myrddraal"),
				Spirit->Destination.Get(), static_cast<AActor*>(Myrddraal));
			Teleport->DestroySeekers();
			TestNull(TEXT("the blink shakes the spirit off (base-class iteration covers the leaf)"),
				Spirit->Destination.Get());
		}
		WoTTest::DestroySpecWorld(World);
	}

	// F6 — the Legion's victim dies -> every spirit homing on it is put down (Legion.Killed,
	// uc:469-484 -> TargetDestroyed -> VeryDead), so none hunt the respawn.
	{
		UWorld* World = WoTTest::CreateSpecWorld();
		AWoTTestNPCCharacter* Victim = SpawnG24Target(World, FVector(1000, 0, 100), 2);
		AWoTLegionProjectile* Hunter = SpawnG24Spirit(World, FVector(0, 0, 100));
		AWoTLegionProjectile* Other = SpawnG24Spirit(World, FVector(0, 200, 100));
		AWoTTestNPCCharacter* Innocent = SpawnG24Target(World, FVector(600, 0, 100), 2);
		if (TestNotNull(TEXT("victim"), Victim) && TestNotNull(TEXT("hunter spirit"), Hunter) &&
			TestNotNull(TEXT("other spirit"), Other))
		{
			Hunter->SetDestination(Victim);
			Other->SetDestination(Innocent);
			AWoTLegionProjectile::PutDownSeekersTargeting(World, Victim);
			TestTrue(TEXT("the victim's hunter is put down (VeryDead)"), Hunter->IsVeryDead());
			TestFalse(TEXT("a spirit chasing someone else is untouched"), Other->IsVeryDead());
		}
		WoTTest::DestroySpecWorld(World);
	}

	return true;
}

#endif // WITH_AUTOMATION_TESTS
