// G22: ShieldUser full parity (Soldier deflect-away + Questioner lead-reflect). Like G20/
// G21 these specs use fixed 1/60 world steps and avoid exact-timer-boundary asserts. Helper
// names are file-unique (unity build folds all Tests/*.cpp into one TU).
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../WoTCharacter.h"
#include "../WoTSoundTable.h"
#include "../AI/WoTBrainComponent.h"
#include "../AI/WoTCreatureBehaviors.h"
#include "../AI/WoTCreatureData.h"
#include "../AI/WoTNPCCharacter.h"
#include "../AI/WoTNPCController.h"
#include "../AI/WoTNPCWeapon.h"
#include "../Inventory/WoTShieldReturnToSenderReflector.h"
#include "WoTTestAIClasses.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/ProjectileMovementComponent.h"

namespace
{
	AWoTNPCCharacter* SpawnG22Shielder(UWorld* World, FName Row, const FVector& Location)
	{
		AWoTNPCCharacter* NPC = WoTCreatureData::SpawnCreature(
			*World, Row, Location, 1, true, AWoTTestNPCCharacter::StaticClass());
		if (NPC)
		{
			NPC->SetActorRotation(FRotator::ZeroRotator);
			NPC->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
		}
		return NPC;
	}

	UWoTShieldDeflectBehavior* G22Shield(AWoTNPCCharacter* NPC)
	{
		return NPC ? NPC->FindComponentByClass<UWoTShieldDeflectBehavior>() : nullptr;
	}

	AWoTNPCController* G22Controller(AWoTNPCCharacter* NPC)
	{
		return NPC ? Cast<AWoTNPCController>(NPC->GetController()) : nullptr;
	}

	// A stationary shooter pawn (the projectile's instigator, so the reflect path takes the
	// deflect/lead branch rather than the seeker reverse).
	AWoTCharacter* SpawnG22Attacker(UWorld* World, const FVector& Location)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AWoTCharacter* Attacker = World->SpawnActor<AWoTCharacter>(
			Location, FRotator::ZeroRotator, Params);
		if (Attacker)
		{
			Attacker->Health = 100;
			if (AController* C = Attacker->GetController())
			{
				C->UnPossess();
			}
		}
		return Attacker;
	}

	// A concrete deflectable weave aimed by ProjectileMovement->Velocity, owned by Attacker.
	AWoTTestDeflectProjectile* SpawnG22Projectile(UWorld* World, const FVector& Location,
		const FVector& Velocity, float Damage, APawn* Attacker)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Params.Instigator = Attacker;
		AWoTTestDeflectProjectile* Proj = World->SpawnActor<AWoTTestDeflectProjectile>(
			AWoTTestDeflectProjectile::StaticClass(), Location, Velocity.Rotation(), Params);
		if (Proj)
		{
			// BeginPlay already seeded velocity from Speed*rotation; override post-spawn.
			Proj->Speed = Velocity.Size();
			Proj->Damage = Damage;
			Proj->bExplode = true;
			if (Proj->ProjectileMovement)
			{
				Proj->ProjectileMovement->Velocity = Velocity;
			}
		}
		return Proj;
	}
}

// --- 1. Deflect math: a Soldier knocks the weave back toward the side it came from, blended
//        by the MP difficulty weights (index 1: Aim 1, Velo 7) — NOT homed onto the shooter;
//        the Questioner instead leads the reflection at the shooter. --------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG22DeflectMathSpec,
	"WoT.AI.ShieldDeflectMath", EAutomationTestFlags::EditorContext |
	EAutomationTestFlags::ProductFilter)
bool FWoTG22DeflectMathSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	// Owner at origin; the weave approaches along -X but offset +30 in Y (the extended
	// shield cylinder catches it). -Velocity is +X -> rotator(zero), so the << / >> are
	// identity and the blend is hand-computable.
	AWoTNPCCharacter* Soldier = SpawnG22Shielder(World, TEXT("Soldier"), FVector::ZeroVector);
	UWoTShieldDeflectBehavior* Shield = G22Shield(Soldier);
	if (!TestNotNull(TEXT("Soldier"), Soldier) || !TestNotNull(TEXT("shield behavior"), Shield))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Shield->EnableDeflect();
	AWoTCharacter* Attacker = SpawnG22Attacker(World, FVector(300, -50, 0));
	AWoTTestDeflectProjectile* Proj = SpawnG22Projectile(
		World, FVector(100, 30, 0), FVector(-1000, 0, 0), /*Damage*/ 0.0f, Attacker);
	TestNotNull(TEXT("projectile"), Proj);
	Proj->DriveTouch(Soldier);

	// Hand-computed: AimDir=Normal(100,30,0)=(0.957826,0.287348,0); blend=(AimDir+7*(1,0,0))/8
	// = (0.994728,0.0359185,0); Out = 1000 * blend.
	const FVector V = Proj->ProjectileMovement->Velocity;
	TestTrue(TEXT("deflect X matches the difficulty-1 blend"),
		FMath::IsNearlyEqual(V.X, 994.728f, 0.6f));
	TestTrue(TEXT("deflect Y matches the difficulty-1 blend"),
		FMath::IsNearlyEqual(V.Y, 35.9185f, 0.2f));
	// It is knocked back the way it came (+X), not homed at the shooter (which sits at Y=-50,
	// so a homing re-aim would carry Y<0). The deflect's Y is positive.
	TestTrue(TEXT("Soldier does not home back onto the shooter (Y>0, not aimed at Y=-50)"),
		V.Y > 0.0f);

	// OFF-AXIS case (review-hardened): a diagonal incoming velocity makes the reverse-
	// frame rotation non-identity AND the projectile sits on the far side, so the
	// abs(X) mirror actually flips sign — this case fails if the << / >> handedness were
	// inverted or the mirror dropped, which the axis-aligned case above cannot catch.
	// Hand computation (owner at (0,0,10000), Difficulty 1: Aim 1 / Velo 7):
	//   V = (-1000,-600,0), speed = 1166.190; u = Normal(-V) = (0.857493, 0.514496, 0)
	//   AimDir = Normal(P-O), P-O = (-400,200,0) -> (-0.894427, 0.447214, 0)
	//   rev-frame: local = (-0.536823, 0.843659, 0); mirror -> +0.536823
	//   world: (0.026263, 0.999625, 0)
	//   blend = (1*mirror + 7*u)/8 = (0.753589, 0.575137, 0)  (|blend| = 0.948 — the
	//   verbatim non-renormalized quirk: outgoing speed 1105.5 < incoming 1166.2)
	//   Out = 1166.190 * blend = (878.83, 670.71, 0)
	{
		const FVector OBase(0, 0, 10000);
		AWoTNPCCharacter* Soldier2 = SpawnG22Shielder(World, TEXT("Soldier"), OBase);
		UWoTShieldDeflectBehavior* Shield2 = G22Shield(Soldier2);
		if (TestNotNull(TEXT("off-axis Soldier shield"), Shield2))
		{
			Shield2->EnableDeflect();
			AWoTCharacter* Attacker2 = SpawnG22Attacker(World, OBase + FVector(500, 300, 0));
			AWoTTestDeflectProjectile* Proj2 = SpawnG22Projectile(
				World, OBase + FVector(-400, 200, 0), FVector(-1000, -600, 0), 0.0f, Attacker2);
			Proj2->DriveTouch(Soldier2);
			const FVector V2 = Proj2->ProjectileMovement->Velocity;
			TestTrue(TEXT("off-axis deflect X (handedness + mirror)"),
				FMath::IsNearlyEqual(V2.X, 878.83f, 1.5f));
			TestTrue(TEXT("off-axis deflect Y (handedness + mirror)"),
				FMath::IsNearlyEqual(V2.Y, 670.71f, 1.5f));
			TestTrue(TEXT("verbatim blend is not re-normalized (speed 1105.5 < 1166.2)"),
				FMath::IsNearlyEqual(V2.Size(), 1105.53f, 2.0f));
		}
	}

	// The Questioner leads the reflection at the shooter instead (bDeflect false). Same
	// relative geometry, shifted +5000 X so the two pawns don't overlap.
	const FVector QBase(5000, 0, 0);
	AWoTNPCCharacter* Questioner = SpawnG22Shielder(World, TEXT("Questioner"), QBase);
	UWoTShieldDeflectBehavior* QShield = G22Shield(Questioner);
	if (TestNotNull(TEXT("Questioner shield behavior"), QShield))
	{
		QShield->EnableDeflect();
		AWoTCharacter* QAttacker = SpawnG22Attacker(World, QBase + FVector(300, -50, 0));
		AWoTTestDeflectProjectile* QProj = SpawnG22Projectile(
			World, QBase + FVector(100, 30, 0), FVector(-1000, 0, 0), 0.0f, QAttacker);
		QProj->DriveTouch(Questioner);
		const FVector QV = QProj->ProjectileMovement->Velocity;
		// CalculateTrajectory aims from the pawn at the stationary shooter (+300,-50) -> Y<0.
		TestTrue(TEXT("Questioner leads the reflection at the shooter (Y<0)"), QV.Y < 0.0f);
		TestTrue(TEXT("Questioner path differs from the Soldier deflect"),
			!FMath::IsNearlyEqual(QV.Y, V.Y, 1.0f));
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

// --- 2. Deflect damages the shield; enough deflects destroy it, and destruction stops the
//        deflect. ---------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG22DeflectDamageSpec,
	"WoT.AI.ShieldDeflectDamage", EAutomationTestFlags::EditorContext |
	EAutomationTestFlags::ProductFilter)
bool FWoTG22DeflectDamageSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* Soldier = SpawnG22Shielder(World, TEXT("Soldier"), FVector::ZeroVector);
	UWoTShieldDeflectBehavior* Shield = G22Shield(Soldier);
	if (!TestNotNull(TEXT("Soldier"), Soldier) || !TestNotNull(TEXT("shield behavior"), Shield))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	AWoTNPCWeapon* ShieldWeapon = Soldier->GetMeleeWeapon();
	if (!TestNotNull(TEXT("shield weapon"), ShieldWeapon))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	TestEqual(TEXT("SoldierShield starts at Health 150"), ShieldWeapon->Health, 150);
	Shield->EnableDeflect();
	TestTrue(TEXT("deflect enabled"), Shield->IsDeflectEnabled());

	AWoTCharacter* Attacker = SpawnG22Attacker(World, FVector(300, 0, 0));
	// 50-damage weaves: 150 -> 100 -> 50 -> destroyed.
	AWoTTestDeflectProjectile* P1 = SpawnG22Projectile(
		World, FVector(100, 30, 0), FVector(-1000, 0, 0), 50.0f, Attacker);
	P1->DriveTouch(Soldier);
	TestEqual(TEXT("first deflect drains 50"), Soldier->GetMeleeWeapon()->Health, 100);
	TestTrue(TEXT("still deflecting after one hit"), Shield->IsDeflectEnabled());

	AWoTTestDeflectProjectile* P2 = SpawnG22Projectile(
		World, FVector(100, 30, 0), FVector(-1000, 0, 0), 50.0f, Attacker);
	P2->DriveTouch(Soldier);
	TestEqual(TEXT("second deflect drains to 50"), Soldier->GetMeleeWeapon()->Health, 50);

	AWoTTestDeflectProjectile* P3 = SpawnG22Projectile(
		World, FVector(100, 30, 0), FVector(-1000, 0, 0), 50.0f, Attacker);
	P3->DriveTouch(Soldier);
	TestNull(TEXT("the shield weapon is destroyed"), Soldier->GetMeleeWeapon());
	TestFalse(TEXT("destruction stops the deflect"), Shield->IsDeflectEnabled());

	WoTTest::DestroySpecWorld(World);
	return true;
}

// --- 3. Shield-up equips the shield weapon; the shield-melee 'Slammed' row keys off it. --
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG22ShieldEquipSpec,
	"WoT.AI.ShieldEquip", EAutomationTestFlags::EditorContext |
	EAutomationTestFlags::ProductFilter)
bool FWoTG22ShieldEquipSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* Soldier = SpawnG22Shielder(World, TEXT("Soldier"), FVector::ZeroVector);
	UWoTShieldDeflectBehavior* Shield = G22Shield(Soldier);
	if (!TestNotNull(TEXT("Soldier"), Soldier) || !TestNotNull(TEXT("shield behavior"), Shield))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	// Body-attack first, then raise the shield: it selects the shield weapon.
	Soldier->EquipBodyAttack();
	TestNull(TEXT("body attack clears the held weapon"), Soldier->GetHeldWeapon());
	Shield->EnableDeflect();
	SpawnG22Projectile(World, FVector(300, 0, 0), FVector(-600, 0, 0),
		0.0f, nullptr);
	TestEqual(TEXT("shield-up equips the shield weapon"),
		Soldier->GetHeldWeapon(), Soldier->GetMeleeWeapon());
	TestTrue(TEXT("held weapon is valid"), IsValid(Soldier->GetHeldWeapon()));
	Shield->TickBehavior(1.0f / 60.0f);
	TestEqual(TEXT("shield-up holds the authored Crouching loop"),
		Soldier->GetReplicatedAnimName(), FName(TEXT("Crouching")));

	// The shield IS the melee weapon -> the weapon-attack slot resolves, and the 'Slammed'
	// shield-slam row is present (the shield-melee damage row keys off it).
	TestEqual(TEXT("effective melee slot is the weapon-attack slot"),
		Soldier->GetEffectiveMeleeAnimSlot(), FName(TEXT("MeleeWeaponAttack")));
	bool bHasSlammed = false;
	for (const FWoTMeleeAttack& Attack : Soldier->MeleeAttacks)
	{
		if (Attack.DamageTypeName == FName(TEXT("Slammed")))
		{
			bHasSlammed = true;
		}
	}
	TestTrue(TEXT("the shield-slam 'Slammed' damage row is present"), bHasSlammed);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// --- 4. Fallback: a Questioner whose shield is in flight dodges instead of deflecting; and
//        the deflect force-drops the moment the shield leaves. -------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG22ShieldlessDodgeSpec,
	"WoT.AI.ShieldlessDodge", EAutomationTestFlags::EditorContext |
	EAutomationTestFlags::ProductFilter)
bool FWoTG22ShieldlessDodgeSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* Questioner = SpawnG22Shielder(World, TEXT("Questioner"), FVector(0, 0, 100));
	UWoTShieldDeflectBehavior* Shield = G22Shield(Questioner);
	AWoTNPCController* Ctl = G22Controller(Questioner);
	if (!TestNotNull(TEXT("Questioner"), Questioner) || !TestNotNull(TEXT("shield"), Shield) ||
		!TestNotNull(TEXT("controller"), Ctl))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	TestTrue(TEXT("Questioner is dodge-armed"), Questioner->bDefensiveDodge);
	Ctl->SetBrainStateForG20Test(EWoTBrainState::Tracking);

	// An incoming weave 300uu ahead closing at 600uu/s -> arrival 0.5s (inside the window).
	SpawnG22Projectile(World, FVector(300, 0, 100), FVector(-600, 0, 0), 0.0f, nullptr);

	// Shield in hand: the dodge is gated shield-first (the shield behavior raises the shield
	// instead), so TryBeginDodge yields.
	TestTrue(TEXT("shield is available"), Shield->HasShieldAvailable());
	TestFalse(TEXT("shield-first: no dodge while the shield is up"), Ctl->TryBeginDodgeForTest());
	TestEqual(TEXT("stays in Tracking"), static_cast<int32>(Ctl->GetBrainState()),
		static_cast<int32>(EWoTBrainState::Tracking));

	// Raise the shield, then throw it (its projectile flies -> IsInFlight).
	Shield->EnableDeflect();
	TestTrue(TEXT("deflect enabled with the shield up"), Shield->IsDeflectEnabled());
	AWoTNPCThrowableWeapon* Throwable = Cast<AWoTNPCThrowableWeapon>(Questioner->GetMeleeWeapon());
	if (!TestNotNull(TEXT("QuestionerShield is throwable"), Throwable))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	// Hurl it straight up from well above the pawn so the shield projectile can neither
	// overlap the Questioner nor be picked as the incoming offender (no world tick runs
	// between here and the dodge scan, so it stays inert at its spawn point).
	Throwable->FireProjectile(Questioner, Questioner->GetActorLocation() + FVector(0, 0, 1000),
		FRotator(90, 0, 0)); // straight up
	TestTrue(TEXT("the thrown shield is in flight"), Throwable->IsInFlight());
	TestFalse(TEXT("shield no longer available in flight"), Shield->HasShieldAvailable());

	// The behavior drops the deflect at once when the shield leaves.
	Shield->TickBehavior(1.0f / 60.0f);
	TestFalse(TEXT("deflect force-drops the moment the shield leaves"), Shield->IsDeflectEnabled());

	// Now shieldless, the same incoming weave routes to the dodge path.
	TestTrue(TEXT("shieldless Questioner dodges the incoming weave"), Ctl->TryBeginDodgeForTest());
	TestEqual(TEXT("dodge enters the Dodging state"),
		static_cast<int32>(Ctl->GetBrainState()), static_cast<int32>(EWoTBrainState::Dodging));

	WoTTest::DestroySpecWorld(World);
	return true;
}

// --- 5. Stationary: a deflecting pawn holds position (forced-stationary), released on drop
//        and on unpossess (no leak). ------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG22ShieldStationarySpec,
	"WoT.AI.ShieldStationary", EAutomationTestFlags::EditorContext |
	EAutomationTestFlags::ProductFilter)
bool FWoTG22ShieldStationarySpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* Soldier = SpawnG22Shielder(World, TEXT("Soldier"), FVector::ZeroVector);
	UWoTShieldDeflectBehavior* Shield = G22Shield(Soldier);
	AWoTNPCController* Ctl = G22Controller(Soldier);
	if (!TestNotNull(TEXT("Soldier"), Soldier) || !TestNotNull(TEXT("shield"), Shield) ||
		!TestNotNull(TEXT("controller"), Ctl))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	TestFalse(TEXT("not stationary before shielding"), Ctl->IsForcedStationary());
	Shield->EnableDeflect();
	TestTrue(TEXT("deflecting pawn is forced stationary"), Ctl->IsForcedStationary());
	Shield->DisableDeflect();
	TestFalse(TEXT("drop releases the stationary hold"), Ctl->IsForcedStationary());

	// Re-raise, then unpossess: the gate must not leak.
	Shield->EnableDeflect();
	TestTrue(TEXT("forced stationary again"), Ctl->IsForcedStationary());
	Ctl->UnPossess();
	TestFalse(TEXT("unpossess releases the stationary hold"), Ctl->IsForcedStationary());

	WoTTest::DestroySpecWorld(World);
	return true;
}

// --- 6. Voice: the Questioner has a 'Reflected' voice slot (played per deflect); the Soldier
//        has none (its table stays silent). ------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG22ReflectedVoiceSpec,
	"WoT.AI.ReflectedVoice", EAutomationTestFlags::EditorContext |
	EAutomationTestFlags::ProductFilter)
bool FWoTG22ReflectedVoiceSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	// The 'Reflected' trigger (RedirectImpactDamage, uc:56) routes PlayVoiceSlot('Reflected')
	// on the host. The per-creature split falls out of the sound tables: SoundTableQuestioner
	// authors 'Reflected', SoundTableSoldier does not.
	TestNotNull(TEXT("Questioner has a 'Reflected' voice slot"),
		WoTSoundTable::PickSlotSound(TEXT("Questioner"), TEXT("Reflected"), World));
	TestNull(TEXT("Soldier has no 'Reflected' voice slot (silent)"),
		WoTSoundTable::PickSlotSound(TEXT("Soldier"), TEXT("Reflected"), World));

	WoTTest::DestroySpecWorld(World);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
