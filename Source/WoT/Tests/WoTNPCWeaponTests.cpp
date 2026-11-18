// G19 arsenal actor specs. Helper names carry the G19 suffix because unity builds
// merge Tests/*.cpp and anonymous namespaces do not protect duplicate symbols.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../AI/WoTCreatureData.h"
#include "../AI/WoTNPCCharacter.h"
#include "../AI/WoTNPCController.h"
#include "../AI/WoTNPCWeapon.h"
#include "../AI/WoTNPCWeaponData.h"
#include "../AI/WoTRangeBands.h"
#include "../WoTCharacter.h"
#include "../Projectiles/WoTWeaponProjectile.h"
#include "WoTTestAIClasses.h"
#include "Components/SphereComponent.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"

namespace
{
	AWoTNPCCharacter* SpawnG19Creature(UWorld* World, FName Row)
	{
		return WoTCreatureData::SpawnCreature(*World, Row, FVector(0, 0, 100), 1, true,
			AWoTTestNPCCharacter::StaticClass());
	}

	void SpawnG19ActivityAnchor(UWorld* World)
	{
		AWoTCharacter* Anchor = World->SpawnActor<AWoTCharacter>(FVector(0, 0, 100), FRotator::ZeroRotator);
		Anchor->Health = 0;
		Anchor->bDead = true;
		Anchor->SetActorHiddenInGame(true);
		Anchor->SetActorEnableCollision(false);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG19TwoSlotRoutingTest,
	"WoT.Weapon.NPCArsenal.TwoSlotRouting",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTG19TwoSlotRoutingTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	SpawnG19ActivityAnchor(World);
	AWoTNPCCharacter* Myrddraal = SpawnG19Creature(World, TEXT("Myrddraal"));
	AWoTNPCController* Controller = Myrddraal ? Cast<AWoTNPCController>(Myrddraal->GetController()) : nullptr;
	if (TestNotNull(TEXT("Myrddraal"), Myrddraal) && TestNotNull(TEXT("controller"), Controller))
	{
		Controller->RouteWeaponForProximity(EWoTGoalProximity::Visible);
		TestEqual(TEXT("visible routing enters ranged preparation"),
			Controller->GetBrainState(), EWoTBrainState::PrepareWeapon);
		TestNotEqual(TEXT("ranged weapon waits for its authored notify"),
			Myrddraal->GetHeldWeapon(), Myrddraal->GetRangedWeapon());
		for (int32 Frame = 0;
			Frame < 360 && (Myrddraal->GetHeldWeapon() != Myrddraal->GetRangedWeapon()
				|| Controller->GetBrainState() == EWoTBrainState::PrepareWeapon);
			++Frame)
		{
			WoTTest::TickWorld(World, 1, 1.0f / 60.0f);
		}
		TestEqual(TEXT("visible routing equips ranged after preparation"),
			Myrddraal->GetHeldWeapon(), Myrddraal->GetRangedWeapon());
		TestNotEqual(TEXT("ranged preparation episode completes"),
			Controller->GetBrainState(), EWoTBrainState::PrepareWeapon);

		Controller->RouteWeaponForProximity(EWoTGoalProximity::Reachable);
		TestEqual(TEXT("reachable routing enters outgoing preparation"),
			Controller->GetBrainState(), EWoTBrainState::PrepareWeapon);
		TestEqual(TEXT("held bow waits for its outgoing notify"),
			Myrddraal->GetHeldWeapon(), Myrddraal->GetRangedWeapon());
		for (int32 Frame = 0;
			Frame < 600 && Myrddraal->GetHeldWeapon() != Myrddraal->GetMeleeWeapon();
			++Frame)
		{
			WoTTest::TickWorld(World, 1, 1.0f / 60.0f);
		}
		TestEqual(TEXT("reachable routing equips melee after preparation"),
			Myrddraal->GetHeldWeapon(), Myrddraal->GetMeleeWeapon());
	}

	AWoTNPCCharacter* Trolloc = SpawnG19Creature(World, TEXT("Trolloc"));
	AWoTNPCController* TrollocController = Trolloc ? Cast<AWoTNPCController>(Trolloc->GetController()) : nullptr;
	if (TestNotNull(TEXT("Trolloc"), Trolloc) && TestNotNull(TEXT("Trolloc controller"), TrollocController))
	{
		TrollocController->RouteWeaponForProximity(EWoTGoalProximity::Reachable);
		TestNull(TEXT("MeleeWeaponType=None keeps body attack (no held actor)"), Trolloc->GetHeldWeapon());
		TestTrue(TEXT("body melee range remains live"), Trolloc->MeleeRange > 0.0f);
	}
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG19SnipingGateTest,
	"WoT.Weapon.NPCArsenal.SnipingGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTG19SnipingGateTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* Minion = SpawnG19Creature(World, TEXT("Minion"));
	AWoTNPCController* MinionController = Minion ? Cast<AWoTNPCController>(Minion->GetController()) : nullptr;
	if (TestNotNull(TEXT("Minion controller"), MinionController))
	{
		const FWoTRangeBand& Visible = MinionController->GetTrackingBands().Band(EWoTGoalProximity::Visible);
		TestTrue(TEXT("weaponless visible band does not hold"), !Visible.bHold);
		TestTrue(TEXT("weaponless visible band cannot projectile attack"), Visible.Attack != EWoTBandAttack::Projectile);
	}
	AWoTNPCCharacter* Myrddraal = SpawnG19Creature(World, TEXT("Myrddraal"));
	AWoTNPCWeapon* Bow = Myrddraal ? Myrddraal->GetRangedWeapon() : nullptr;
	if (TestNotNull(TEXT("Myrddraal bow"), Bow))
	{
		Bow->DetermineWeaponUsage(249.0f, true);
		TestEqual(TEXT("below MinProjectileRange withholds"), Bow->GetWeaponUsage(), EWoTWeaponUsage::None);
	}
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG19MyrddraalUsageTest,
	"WoT.Weapon.NPCArsenal.MyrddraalUsage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTG19MyrddraalUsageTest::RunTest(const FString&)
{
	const FWoTNPCWeaponConfig* SwordConfig = WoTNPCWeaponData::Find(TEXT("WoTPawns.MyrddraalSword"));
	const FWoTNPCWeaponConfig* BowConfig = WoTNPCWeaponData::Find(TEXT("WoTPawns.MyrddraalBow"));
	TestNotNull(TEXT("sword config"), SwordConfig);
	TestNotNull(TEXT("bow config"), BowConfig);
	if (SwordConfig && BowConfig)
	{
		TestEqual(TEXT("gap 40 sword usage"), UWoTWeaponComponent::ClassifyWeaponUsage(
			40, true, SwordConfig->MinMeleeRange, SwordConfig->MaxMeleeRange,
			SwordConfig->MeleeEffectiveness, SwordConfig->MinProjectileRange,
			SwordConfig->MaxProjectileRange, SwordConfig->ProjectileEffectiveness,
			SwordConfig->ProjectileClass != nullptr, true), EWoTWeaponUsage::Melee);
		TestEqual(TEXT("gap 1000 bow usage"), UWoTWeaponComponent::ClassifyWeaponUsage(
			1000, true, BowConfig->MinMeleeRange, BowConfig->MaxMeleeRange,
			BowConfig->MeleeEffectiveness, BowConfig->MinProjectileRange,
			BowConfig->MaxProjectileRange, BowConfig->ProjectileEffectiveness,
			BowConfig->ProjectileClass != nullptr, true), EWoTWeaponUsage::Projectile);
	}
	const FWoTCreatureRow* Legion = WoTCreatureData::Find(TEXT("Legion"));
	if (TestNotNull(TEXT("Legion adapter config row"), Legion))
	{
		TestEqual(TEXT("Legion offensive interval"), Legion->MinOffensiveAngrealUsageInterval, 3.0f);
		TestEqual(TEXT("Legion projectile minimum"), Legion->AdapterMinProjectileRange, 50.0f);
		TestEqual(TEXT("Legion projectile hold max"), Legion->AdapterMaxProjectileRange, 3200.0f);
		TestEqual(TEXT("Legion tracking hold"), Legion->HoldRange, 3200.0f);
		TestFalse(TEXT("Legion does not require target visibility"), Legion->bAdapterRequireTargetVisibility);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG19TrollocAxeTest,
	"WoT.Weapon.NPCArsenal.TrollocAxe",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTG19TrollocAxeTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* Trolloc = SpawnG19Creature(World, TEXT("Trolloc"));
	AWoTNPCWeapon* Axe = Trolloc ? Trolloc->GetRangedWeapon() : nullptr;
	if (TestNotNull(TEXT("axe proxy"), Axe))
	{
		Axe->DetermineWeaponUsage(300.0f, true);
		TestEqual(TEXT("300 starts projectile window"), Axe->GetWeaponUsage(), EWoTWeaponUsage::Projectile);
		Axe->DetermineWeaponUsage(10000.0f, true);
		TestEqual(TEXT("10000 ends projectile window inclusively"), Axe->GetWeaponUsage(), EWoTWeaponUsage::Projectile);
		Axe->DetermineWeaponUsage(50.0f, true);
		TestEqual(TEXT("inside 65 selects effectiveness-2 melee"), Axe->GetWeaponUsage(), EWoTWeaponUsage::Melee);
	}
	const AWoTTrollocAxeProjectile* CDO = GetDefault<AWoTTrollocAxeProjectile>();
	TestEqual(TEXT("axe speed"), CDO->Speed, 800.0f);
	TestEqual(TEXT("axe damage"), CDO->Damage, 10.0f);
	TestEqual(TEXT("axe yaw spin"), CDO->YawSpinRate, 300000);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG19ThrowableCycleTest,
	"WoT.Weapon.NPCArsenal.ThrowableCycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTG19ThrowableCycleTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* BA = SpawnG19Creature(World, TEXT("BATrolloc"));
	AWoTNPCThrowableWeapon* Halberd = BA ? Cast<AWoTNPCThrowableWeapon>(BA->GetRangedWeapon()) : nullptr;
	if (TestNotNull(TEXT("throwable halberd"), Halberd))
	{
		Halberd->CurrentSkin = WoTNPCWeaponData::ResolveTexturePath(TEXT("WoTPawns.Skins.MMiscWeapons"));
		AWoTWeaponProjectile* First = Halberd->FireProjectile(BA, BA->GetActorLocation(), FRotator::ZeroRotator);
		if (TestNotNull(TEXT("halberd projectile"), First))
		{
			TestFalse(TEXT("weapon leaves hand"), Halberd->bEquipped);
			Halberd->UseProjectileWeapon(BA, BA);
			int32 ProjectileCount = 0;
			for (TActorIterator<AWoTWeaponProjectile> It(World); It; ++It)
			{
				if (IsValid(*It) && !It->IsActorBeingDestroyed()) { ++ProjectileCount; }
			}
			TestEqual(TEXT("second UseProjectileWeapon attempt spawns nothing"), ProjectileCount, 1);
			TestTrue(TEXT("weapon stays in flight after rejected second fire"), Halberd->IsInFlight());
			TestTrue(TEXT("projectile inherits skin"),
				First->SkinOverride.ToSoftObjectPath() == Halberd->CurrentSkin.ToSoftObjectPath());
			TestEqual(TEXT("projectile inherits style"), First->bStyleMasked, Halberd->bStyleMasked);
			First->Destroy();
			TestTrue(TEXT("weapon returns after projectile destruction"), Halberd->bEquipped);
		}
		AWoTWeaponProjectile* Second = Halberd->FireProjectile(BA, BA->GetActorLocation(), FRotator::ZeroRotator);
		TestNotNull(TEXT("death-cycle projectile"), Second);
		FDamageEvent FatalEvent;
		BA->TakeDamage(100000.0f, FatalEvent, nullptr, nullptr);
		int32 TossedCount = 0;
		for (TActorIterator<AWoTTossedWeapon> It(World); It; ++It)
		{
			if (IsValid(*It) && !It->IsActorBeingDestroyed()) { ++TossedCount; }
		}
		TestTrue(TEXT("real owner death destroys in-flight weapon"),
			!IsValid(Halberd) || Halberd->IsActorBeingDestroyed());
		TestEqual(TEXT("in-flight weapon creates no corpse-toss decoration"), TossedCount, 0);
	}
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG19QuestionerOddsTest,
	"WoT.Weapon.NPCArsenal.QuestionerOdds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTG19QuestionerOddsTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* Questioner = SpawnG19Creature(World, TEXT("Questioner"));
	AWoTNPCWeapon* Shield = Questioner ? Questioner->GetRangedWeapon() : nullptr;
	if (TestNotNull(TEXT("Questioner shield"), Shield))
	{
		Shield->DetermineWeaponUsage(1000.0f, true, 0.0f);
		TestEqual(TEXT("roll below odds keeps projectile"), Shield->GetWeaponUsage(), EWoTWeaponUsage::Projectile);
		Shield->DetermineWeaponUsage(1000.0f, true, 1.0f);
		TestEqual(TEXT("roll above odds withholds"), Shield->GetWeaponUsage(), EWoTWeaponUsage::None);
	}
	// Util.uc:1007-1038 with inverted odds: the interior lerps HIGH->LOW (close
	// throws are likely), but the shipped boundary branches return the post-swap
	// values — at/below MinDistance the LOW odds, at/beyond MaxDistance the HIGH.
	// Parity-locked edge quirk; see AWoTNPCWeapon::RandDistance.
	TestEqual(TEXT("odds at 128 (boundary quirk: low)"),
		AWoTNPCWeapon::RandDistance(128, 128, 4096, 0.5f, 0.01f), 0.01f);
	TestEqual(TEXT("odds just inside min (~high)"),
		AWoTNPCWeapon::RandDistance(129, 128, 4096, 0.5f, 0.01f), 0.49988f, 0.001f);
	TestEqual(TEXT("odds just inside max (~low)"),
		AWoTNPCWeapon::RandDistance(4095, 128, 4096, 0.5f, 0.01f), 0.01012f, 0.001f);
	TestEqual(TEXT("odds at 4096 (boundary quirk: high)"),
		AWoTNPCWeapon::RandDistance(4096, 128, 4096, 0.5f, 0.01f), 0.5f);
	TestEqual(TEXT("odds beyond max stay high (10000uu Questioner throws at 0.5)"),
		AWoTNPCWeapon::RandDistance(10000, 128, 4096, 0.5f, 0.01f), 0.5f);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG19ShieldGearTest,
	"WoT.Weapon.NPCArsenal.ShieldGear",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTG19ShieldGearTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* Soldier = SpawnG19Creature(World, TEXT("Soldier"));
	AWoTNPCWeapon* Shield = Soldier ? Soldier->GetMeleeWeapon() : nullptr;
	if (TestNotNull(TEXT("Soldier shield"), Shield))
	{
		FDamageEvent Event;
		Shield->TakeDamage(1, Event, nullptr, nullptr);
		TestTrue(TEXT("first damage skin"), Shield->CurrentSkin.ToSoftObjectPath() == Shield->DamageSkin0.ToSoftObjectPath());
		Shield->TakeDamage(50, Event, nullptr, nullptr);
		TestTrue(TEXT("second damage skin"), Shield->CurrentSkin.ToSoftObjectPath() == Shield->DamageSkin1.ToSoftObjectPath());
		Shield->TakeDamage(50, Event, nullptr, nullptr);
		TestTrue(TEXT("third damage skin"), Shield->CurrentSkin.ToSoftObjectPath() == Shield->DamageSkin2.ToSoftObjectPath());
		bool bDestroyedEvent = false;
		Shield->OnWeaponDestroyed.AddLambda([&bDestroyedEvent](AWoTNPCWeapon*) { bDestroyedEvent = true; });
		Shield->TakeDamage(100, Event, nullptr, nullptr);
		TestTrue(TEXT("owner-facing destroyed event fired"), bDestroyedEvent);
		TestNull(TEXT("owner removed destroyed gear from its slot"), Soldier->GetMeleeWeapon());
		TestTrue(TEXT("fatal damage destroys gear"), !IsValid(Shield) || Shield->IsActorBeingDestroyed());
	}
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG19WeaponProjectileBaseTest,
	"WoT.Weapon.NPCArsenal.WeaponProjectileBase",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTG19WeaponProjectileBaseTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTTestWeaponProjectile* Pass = World->SpawnActor<AWoTTestWeaponProjectile>(
		AWoTTestWeaponProjectile::StaticClass(), FVector(5000, 0, 100), FRotator::ZeroRotator, Params);
	AWoTTestNPCCharacter* A = World->SpawnActor<AWoTTestNPCCharacter>(
		AWoTTestNPCCharacter::StaticClass(), FVector(0, 0, 100), FRotator::ZeroRotator, Params);
	AWoTTestNPCCharacter* B = World->SpawnActor<AWoTTestNPCCharacter>(
		AWoTTestNPCCharacter::StaticClass(), FVector(300, 0, 100), FRotator::ZeroRotator, Params);
	if (TestNotNull(TEXT("pass-through projectile spawned"), Pass) &&
		TestNotNull(TEXT("first pawn spawned"), A) && TestNotNull(TEXT("second pawn spawned"), B))
	{
		A->Health = B->Health = 100;
		Pass->bPassThroughActors = true;
		Pass->DriveTouch(A);
		Pass->DriveTouch(A); // ClearOtherDelay dedup
		Pass->DriveTouch(B);
		TestEqual(TEXT("first pawn damaged once"), A->Health, 90);
		TestEqual(TEXT("second pawn damaged once"), B->Health, 90);
	}

	const AWoTWeaponProjectile* BaseCDO = GetDefault<AWoTWeaponProjectile>();
	const AWoTBATHalberdProjectile* HalberdCDO = GetDefault<AWoTBATHalberdProjectile>();
	const AWoTTrollocAxeProjectile* AxeCDO = GetDefault<AWoTTrollocAxeProjectile>();
	TestEqual(TEXT("base projectile lifespan follows Engine.Projectile"),
		BaseCDO->InitialLifeSpan, 140.0f);
	TestEqual(TEXT("halberd projectile radius"),
		HalberdCDO->Collision->GetUnscaledSphereRadius(), 5.0f);
	TestEqual(TEXT("axe inherited-base stay-stuck time"), AxeCDO->StayStuckTime, 2.0f);
	TestEqual(TEXT("axe inherited-base fade time"), AxeCDO->FadeAwayTime, 3.0f);

	// Flying.ProcessTouch (uc:170): the whole touch body is gated on
	// Projectile(Other) == None — a projectile touch neither damages nor resolves.
	AWoTTestWeaponProjectile* OtherProj = World->SpawnActor<AWoTTestWeaponProjectile>(
		AWoTTestWeaponProjectile::StaticClass(), FVector(6000, 0, 0), FRotator::ZeroRotator, Params);
	if (Pass && TestNotNull(TEXT("other projectile spawned"), OtherProj))
	{
		Pass->DriveTouch(OtherProj);
		TestTrue(TEXT("projectile-vs-projectile touch is ignored"), IsValid(OtherProj));
	}

	// Spawn clear of all fixtures: a projectile that overlaps a pawn during
	// SpawnActor explodes inside spawn and the call returns null.
	AWoTTestWeaponProjectile* Stick = World->SpawnActor<AWoTTestWeaponProjectile>(
		AWoTTestWeaponProjectile::StaticClass(), FVector(5000, 0, 0), FRotator::ZeroRotator, Params);
	if (TestNotNull(TEXT("stick projectile spawned"), Stick))
	{
		Stick->FadeAwayTime = 0.0f;
		Stick->DriveStick();
		TestTrue(TEXT("zero-fade stuck projectile destroys immediately"),
			!IsValid(Stick) || Stick->IsActorBeingDestroyed());
	}
	WoTTest::DestroySpecWorld(World);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
