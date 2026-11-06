// Damage/effect dispatch parity specs. These are headless:
// they exercise movement state, reflector folds, class-name gates, and actor
// lifecycle only; no rendered assets are required.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "WoTTestDamageDispatch.h"
#include "../AI/WoTNPCCharacter.h"
#include "../Inventory/WoTAbsorbReflector.h"
#include "../Inventory/WoTAMAReflectors.h"
#include "../Inventory/WoTAngrealInvAirBurst.h"
#include "../Inventory/WoTAngrealInvFireball.h"
#include "../Inventory/WoTAngrealInvIllusion.h"
#include "../Inventory/WoTAngrealInvSeeker.h"
#include "../Inventory/WoTDecreaseHealthLeech.h"
#include "../Inventory/WoTForkRTSReflector.h"
#include "../Inventory/WoTIgnoreElementReflector.h"
#include "../Inventory/WoTRarityBasedDamageReflector.h"
#include "../Inventory/WoTReflector.h"
#include "../Inventory/WoTReturnToSenderReflector.h"
#include "../Inventory/WoTShieldReflector.h"
#include "../Inventory/WoTShieldReturnToSenderReflector.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../Inventory/WoTIllusionTimerLeech.h"
#include "../Projectiles/WoTAMAPearl.h"
#include "../Projectiles/WoTAngrealIllusionProjectile.h"
#include "../Projectiles/WoTArcherArrow.h"
#include "../Projectiles/WoTDartProjectile.h"
#include "../Projectiles/WoTExpWardProjectile.h"
#include "../Projectiles/WoTFireballProjectile.h"
#include "../Projectiles/WoTMashadarGuide.h"
#include "../Projectiles/WoTProjectile.h"
#include "../Projectiles/WoTSeekerProjectile.h"
#include "../Traps/WoTSpear.h"
#include "../WoTBattleGameMode.h"
#include "../WoTGameMode.h"
#include "../WoTPlayerController.h"
#include "../WoTPlayerState.h"
#include "../WoTUE1Names.h"
#include "../WoTZoneVolume.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"
#include "Components/BoxComponent.h"

namespace
{
	UWorld* CreateDamageDispatchWorld(bool bBattle = false)
	{
		return bBattle
			? WoTTest::CreateSpecWorld(AWoTBattleGameMode::StaticClass())
			: WoTTest::CreateSpecWorld();
	}

	AWoTCharacter* SpawnDamagePawn(UWorld* World, const FVector& Location)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		return World->SpawnActor<AWoTCharacter>(
			AWoTCharacter::StaticClass(), Location, FRotator::ZeroRotator, Params);
	}

	FWoTDamageEvent MomentumEvent(const FVector& Momentum,
	                              FName DamageType = TEXT("hurt"))
	{
		FWoTDamageEvent Event;
		Event.WoTDamageType = DamageType;
		Event.UE1Momentum = Momentum;
		return Event;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMomentumKnockbackTest,
	"WoT.Damage.MomentumKnockback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTMomentumKnockbackTest::RunTest(const FString&)
{
	UWorld* World = CreateDamageDispatchWorld();

	AWoTCharacter* Victim = SpawnDamagePawn(World, FVector(0, 0, 100));
	UCharacterMovementComponent* Move = Victim->GetCharacterMovement();
	Move->Velocity = FVector::ZeroVector;
	Move->SetMovementMode(MOVE_Walking);
	FWoTDamageEvent Event = MomentumEvent(FVector(1750.0f, 0.0f, 0.0f));
	Event.HitInfo.ImpactPoint = Victim->GetActorLocation();
	Victim->TakeDamage(1.0f, Event, nullptr, nullptr);
	TestTrue(TEXT("walking momentum uses the 0.4 Z floor then divides by Mass"),
		Move->Velocity.Equals(FVector(10.0f, 0.0f, 4.0f), KINDA_SMALL_NUMBER));
	TestEqual(TEXT("walking knockback enters falling"),
		(int32)Move->MovementMode.GetValue(), (int32)MOVE_Falling);

	AWoTCharacter* SelfVictim = SpawnDamagePawn(World, FVector(300, 0, 100));
	APlayerController* SelfController = World->SpawnActor<APlayerController>();
	SelfController->InitPlayerState();
	SelfController->Possess(SelfVictim);
	UCharacterMovementComponent* SelfMove = SelfVictim->GetCharacterMovement();
	SelfMove->Velocity = FVector::ZeroVector;
	SelfMove->SetMovementMode(MOVE_Walking);
	Event.HitInfo.ImpactPoint = SelfVictim->GetActorLocation();
	SelfVictim->TakeDamage(1.0f, Event, SelfController, SelfVictim);
	TestTrue(TEXT("self-instigated momentum is scaled by 0.6 after the floor"),
		SelfMove->Velocity.Equals(FVector(6.0f, 0.0f, 2.4f), KINDA_SMALL_NUMBER));

	AWoTCharacter* ZeroVictim = SpawnDamagePawn(World, FVector(600, 0, 100));
	UCharacterMovementComponent* ZeroMove = ZeroVictim->GetCharacterMovement();
	ZeroMove->Velocity = FVector::ZeroVector;
	ZeroMove->SetMovementMode(MOVE_Walking);
	FWoTDamageEvent ZeroEvent = MomentumEvent(FVector::ZeroVector);
	ZeroVictim->TakeDamage(1.0f, ZeroEvent, nullptr, nullptr);
	TestEqual(TEXT("zero momentum does not change walking mode"),
		(int32)ZeroMove->MovementMode.GetValue(), (int32)MOVE_Walking);
	TestTrue(TEXT("zero momentum does not change velocity"),
		ZeroMove->Velocity.IsZero());

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMomentumZ380DampingTest,
	"WoT.Damage.MomentumZ380Damping",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTMomentumZ380DampingTest::RunTest(const FString&)
{
	UWorld* World = CreateDamageDispatchWorld();
	AWoTCharacter* Victim = SpawnDamagePawn(World, FVector(0, 0, 100));
	UCharacterMovementComponent* Move = Victim->GetCharacterMovement();
	Move->SetMovementMode(MOVE_Falling);
	Move->Velocity = FVector(0.0f, 0.0f, 400.0f);
	const FWoTDamageEvent Event = MomentumEvent(FVector(0.0f, 0.0f, 1750.0f));
	Victim->TakeDamage(1.0f, Event, nullptr, nullptr);
	TestTrue(TEXT("upward incoming Z is halved when current Z exceeds 380"),
		FMath::IsNearlyEqual(Move->Velocity.Z, 405.0f, KINDA_SMALL_NUMBER));
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMomentumChainStopTest,
	"WoT.Damage.MomentumChainStop",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTMomentumChainStopTest::RunTest(const FString&)
{
	UWorld* World = CreateDamageDispatchWorld();
	const FVector Momentum(1750.0f, 0.0f, 0.0f);

	AWoTCharacter* ElementVictim = SpawnDamagePawn(World, FVector(0, 0, 100));
	ElementVictim->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
	UWoTIgnoreFireElementReflector* FireEat =
		NewObject<UWoTIgnoreFireElementReflector>(ElementVictim);
	FireEat->Install(ElementVictim);
	const int32 ElementHealth = ElementVictim->Health;
	ElementVictim->TakeDamage(10.0f, MomentumEvent(Momentum, TEXT("xxFxx")),
		nullptr, nullptr);
	TestEqual(TEXT("element eat stops health damage"),
		ElementVictim->Health, ElementHealth);
	TestTrue(TEXT("element eat stops momentum"),
		ElementVictim->GetCharacterMovement()->Velocity.IsZero());

	AWoTCharacter* FullShieldVictim = SpawnDamagePawn(World, FVector(300, 0, 100));
	FullShieldVictim->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
	UWoTShieldReflector* FullShield = NewObject<UWoTShieldReflector>(FullShieldVictim);
	FullShield->Install(FullShieldVictim);
	const int32 FullShieldHealth = FullShieldVictim->Health;
	FullShieldVictim->TakeDamage(1.0f, MomentumEvent(Momentum), nullptr, nullptr);
	TestEqual(TEXT("full shield absorb stops health damage"),
		FullShieldVictim->Health, FullShieldHealth);
	TestTrue(TEXT("full shield absorb stops momentum"),
		FullShieldVictim->GetCharacterMovement()->Velocity.IsZero());

	AWoTCharacter* PartialVictim = SpawnDamagePawn(World, FVector(600, 0, 100));
	PartialVictim->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
	UWoTShieldReflector* PartialShield = NewObject<UWoTShieldReflector>(PartialVictim);
	PartialShield->Install(PartialVictim);
	PartialVictim->TakeDamage(10.0f, MomentumEvent(Momentum), nullptr, nullptr);
	TestEqual(TEXT("partial shield passes its player-damage share"),
		PartialVictim->Health, 95);
	TestTrue(TEXT("partial shield passes the full original momentum"),
		PartialVictim->GetCharacterMovement()->Velocity.Equals(
			FVector(10.0f, 0.0f, 4.0f), KINDA_SMALL_NUMBER));

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSpearMomentumViaPipelineTest,
	"WoT.Damage.SpearMomentumViaPipeline",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTSpearMomentumViaPipelineTest::RunTest(const FString&)
{
	UWorld* World = CreateDamageDispatchWorld();
	AWoTSpear* Spear = World->SpawnActor<AWoTSpear>(
		FVector::ZeroVector, FRotator::ZeroRotator);
	// Trap.IsInactive: a PlayerState-less pawn reads team 255, matching the trap's
	// neutral default — the spear would never spring on it. Arm it hostile.
	Spear->Team = 5;
	AWoTCharacter* Victim = SpawnDamagePawn(World, FVector(52, 0, 0));
	UCharacterMovementComponent* Move = Victim->GetCharacterMovement();
	Move->Velocity = FVector::ZeroVector;
	Move->SetMovementMode(MOVE_Walking);
	Spear->TrapTouch(Victim);
	// SetCollision does not synthesize Touch. Move within the armed tip so the
	// damage-pipeline unit enters through a post-movement Touch delivery.
	Victim->SetActorLocation(Victim->GetActorLocation() + FVector(-1, 0, 0));
	Spear->DamageActor(Victim);
	const FVector Expected = FVector(20000.0f, 0.0f, 8000.0f) / Move->Mass;
	TestTrue(TEXT("spear shove equals the former local 0.4-floor/Mass fold"),
		Move->Velocity.Equals(Expected, KINDA_SMALL_NUMBER));
	TestEqual(TEXT("spear shove enters falling through the shared pipeline"),
		(int32)Move->MovementMode.GetValue(), (int32)MOVE_Falling);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTReflectorPriorityOrderTest,
	"WoT.Reflector.PriorityOrder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTReflectorPriorityOrderTest::RunTest(const FString&)
{
	UWorld* World = CreateDamageDispatchWorld();
	AWoTCharacter* Host = SpawnDamagePawn(World, FVector::ZeroVector);

	UWoTNoCastReflector* Older = NewObject<UWoTNoCastReflector>(Host);
	Older->Install(Host);
	UWoTNoBadEffectsReflector* Newer = NewObject<UWoTNoBadEffectsReflector>(Host);
	Newer->Install(Host);
	TArray<UWoTReflector*> Ordered;
	Host->GetOrderedReflectors(Ordered);
	TestTrue(TEXT("newest equal-priority reflector runs first"),
		Ordered.Num() >= 2 && Ordered[0] == Newer && Ordered[1] == Older);

	AWoTCharacter* SpreadHost = SpawnDamagePawn(World, FVector(300, 0, 0));
	UWoTIgnoreFireElementReflector* P255 =
		NewObject<UWoTIgnoreFireElementReflector>(SpreadHost);
	UWoTShieldReflector* P225 = NewObject<UWoTShieldReflector>(SpreadHost);
	UWoTAbsorbReflector* P170 = NewObject<UWoTAbsorbReflector>(SpreadHost);
	UWoTReturnToSenderReflector* P160 =
		NewObject<UWoTReturnToSenderReflector>(SpreadHost);
	UWoTForkRTSReflector* P150 = NewObject<UWoTForkRTSReflector>(SpreadHost);
	UWoTRarityBasedDamageReflector* P64 =
		NewObject<UWoTRarityBasedDamageReflector>(SpreadHost);
	// Production shape: Soul Barb delivers the rarity curse via projectile
	// (bFromProjectile=true) — without it, Absorb's retail install-steal
	// (AbsorbReflector.uc:71-133) would correctly eat the P64 curse AND
	// consume itself, gutting the spread this spec measures.
	P64->bFromProjectile = true;
	const TArray<UWoTReflector*> InstallOrder{
		P64, P160, P225, P150, P255, P170};
	for (UWoTReflector* Reflector : InstallOrder)
	{
		Reflector->Install(SpreadHost);
	}
	Ordered.Reset();
	SpreadHost->GetOrderedReflectors(Ordered);
	const TArray<uint8> Expected{255, 225, 170, 160, 150, 64};
	bool bMatches = Ordered.Num() == Expected.Num();
	for (int32 Index = 0; bMatches && Index < Expected.Num(); ++Index)
	{
		bMatches = Ordered[Index]->Priority == Expected[Index];
	}
	TestTrue(TEXT("reflectors follow the shipped priority spread"), bMatches);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTProcessEffectDispatchTest,
	"WoT.Reflector.ProcessEffectDispatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTProcessEffectDispatchTest::RunTest(const FString&)
{
	UWorld* World = CreateDamageDispatchWorld();
	AWoTTestDamageDispatchCharacter* Victim =
		World->SpawnActor<AWoTTestDamageDispatchCharacter>(
			FVector::ZeroVector, FRotator::ZeroRotator);
	Victim->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
	AWoTAngrealInvFireball* Source = World->SpawnActor<AWoTAngrealInvFireball>(
		FVector(1000, 0, 0), FRotator::ZeroRotator);

	FWoTDamageEffectSpec Effect;
	Effect.Damage = 10;
	Effect.HitLocation = Victim->GetActorLocation();
	Effect.UE1Momentum = FVector(1750.0f, 0.0f, 0.0f);
	Effect.DamageType = TEXT("xxFxx");
	Effect.SourceAngreal = Source;
	Effect.bDeleterious = true;

	UWoTNoBadEffectsReflector* Dome = NewObject<UWoTNoBadEffectsReflector>(Victim);
	Dome->Install(Victim);
	TestTrue(TEXT("pre-wave blocker query reports the dome outcome"),
		Victim->IsEffectBlocked(Source, true));
	const int32 HealthBefore = Victim->Health;
	TestEqual(TEXT("dome throws away the ProcessEffect"), Victim->ProcessEffect(Effect), 0);
	TestEqual(TEXT("blocked effect does not change health"), Victim->Health, HealthBefore);
	TestTrue(TEXT("blocked effect does not apply momentum"),
		Victim->GetCharacterMovement()->Velocity.IsZero());

	Dome->Uninstall();
	TestFalse(TEXT("pre-wave blocker query clears with the dome"),
		Victim->IsEffectBlocked(Source, true));
	TestEqual(TEXT("unblocked ProcessEffect reaches TakeDamage"),
		Victim->ProcessEffect(Effect), 10);
	TestEqual(TEXT("unblocked effect lands damage"), Victim->Health, HealthBefore - 10);
	TestTrue(TEXT("unblocked effect lands momentum"),
		Victim->GetCharacterMovement()->Velocity.Equals(
			FVector(10.0f, 0.0f, 4.0f), KINDA_SMALL_NUMBER));

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTReflectorUnAffectedTypesTest,
	"WoT.Reflector.UnAffectedTypes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTReflectorUnAffectedTypesTest::RunTest(const FString&)
{
	UWorld* World = CreateDamageDispatchWorld();
	AWoTCharacter* Host = SpawnDamagePawn(World, FVector::ZeroVector);
	UWoTReturnToSenderReflector* Return =
		NewObject<UWoTReturnToSenderReflector>(Host);
	UWoTAbsorbReflector* Absorb = NewObject<UWoTAbsorbReflector>(Host);
	UWoTShieldReturnToSenderReflector* ShieldReturn =
		NewObject<UWoTShieldReturnToSenderReflector>(Host);
	ShieldReturn->IrreleventProjectileType = TEXT("SeekingProjectile");

	AWoTExpWardProjectile* Ward = World->SpawnActor<AWoTExpWardProjectile>(
		FVector(500, 0, 0), FRotator::ZeroRotator);
	AWoTAMAPearl* Pearl = World->SpawnActor<AWoTAMAPearl>(
		FVector(700, 0, 0), FRotator::ZeroRotator);
	AWoTFireballProjectile* Fireball = World->SpawnActor<AWoTFireballProjectile>(
		FVector(900, 0, 0), FRotator::ZeroRotator);
	AWoTSeekerProjectile* Seeker = World->SpawnActor<AWoTSeekerProjectile>(
		FVector(1100, 0, 0), FRotator::ZeroRotator);

	TestFalse(TEXT("RTS ignores ExpWard through its retail name list"),
		Return->IsAffectable(Ward));
	TestFalse(TEXT("RTS ignores AMAPearl by UE1 class name"),
		Return->IsAffectable(Pearl));
	TestTrue(TEXT("RTS reflects Fireball"), Return->IsAffectable(Fireball));
	TestFalse(TEXT("Absorb ignores AMAPearl"), Absorb->IsAffectable(Pearl));
	TestTrue(TEXT("Absorb accepts Fireball; unported LavaRock name is harmless"),
		Absorb->IsAffectable(Fireball));
	TestFalse(TEXT("Shield RTS applies its additional irrelevant-projectile gate"),
		ShieldReturn->IsAffectable(Seeker));

	// The UE1-name bridge collapses the Angreal prefix only for LEAF names: the
	// FAMILY base "AngrealProjectile" must NOT swallow every AWoTProjectile port —
	// retail's WotWeaponProjectile arrows are not angreal weaves.
	AWoTArcherArrow* Arrow = World->SpawnActor<AWoTArcherArrow>(
		FVector(1300, 0, 0), FRotator::ZeroRotator);
	AWoTDartProjectile* Dart = World->SpawnActor<AWoTDartProjectile>(
		FVector(1500, 0, 0), FRotator::ZeroRotator);
	TestFalse(TEXT("an Archer arrow is not an AngrealProjectile by retail name"),
		WoTUE1Names::ActorIsA(Arrow, FName(TEXT("AngrealProjectile"))));
	TestTrue(TEXT("a Dart matches its collapsed retail leaf name"),
		WoTUE1Names::ActorIsA(Dart, FName(TEXT("AngrealDartProjectile"))));

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAbsorbNonStealableTest,
	"WoT.Inventory.Absorb.NonStealable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTAbsorbNonStealableTest::RunTest(const FString&)
{
	UWorld* World = CreateDamageDispatchWorld();
	AWoTCharacter* Host = SpawnDamagePawn(World, FVector::ZeroVector);
	AWoTCharacter* Attacker = SpawnDamagePawn(World, FVector(1000, 0, 0));
	AWoTAngrealInvAirBurst* AirBurst = World->SpawnActor<AWoTAngrealInvAirBurst>(
		FVector(0, 5000, 500), FRotator::ZeroRotator);
	AirBurst->GiveTo(Attacker);

	UWoTAbsorbReflector* Absorb = NewObject<UWoTAbsorbReflector>(Host);
	Absorb->Install(Host);
	const int32 HealthBefore = Host->Health;
	AWoTFireballProjectile* AirHit = World->SpawnActor<AWoTFireballProjectile>(
		FVector(0, 6000, 500), FRotator::ZeroRotator);
	AirHit->SetSourceAngreal(AirBurst);
	// Production delivery: AbsorbReflector.uc:32-42 eats ANY affectable weave and
	// spends the one-shot — the steal silently no-ops for a non-stealable
	// artifact, so the host cancels the hit but gains nothing.
	AirHit->ProcessTouch(Host, Host->GetActorLocation());
	TestEqual(TEXT("the eaten AirBurst hit does no damage"),
		Host->Health, HealthBefore);
	TestEqual(TEXT("the attacker keeps the non-stealable AirBurst"),
		Attacker->InventoryComponent->FindInventoryType(
			AWoTAngrealInvAirBurst::StaticClass()), static_cast<AWoTInventory*>(AirBurst));
	TestNull(TEXT("the host gains nothing from a non-stealable eat"),
		Host->InventoryComponent->FindInventoryType(
			AWoTAngrealInvAirBurst::StaticClass()));
	TestFalse(TEXT("eating the hit consumes the one-shot Absorb"),
		Absorb->IsRegistered());

	// With the absorb spent, the SAME kind of hit passes through the touch
	// dispatch and damages — the false path continues to production damage.
	AWoTFireballProjectile* FollowUp = World->SpawnActor<AWoTFireballProjectile>(
		FVector(0, 6500, 500), FRotator::ZeroRotator);
	FollowUp->SetSourceAngreal(AirBurst);
	FollowUp->ProcessTouch(Host, Host->GetActorLocation());
	TestTrue(TEXT("with no absorb the hit damages the host"),
		Host->Health < HealthBefore);

	AWoTTestFixedChargeAngreal* Stealable =
		World->SpawnActor<AWoTTestFixedChargeAngreal>(
			FVector(0, 5000, 500), FRotator::ZeroRotator);
	Stealable->GiveTo(Attacker);
	UWoTAbsorbReflector* Absorb2 = NewObject<UWoTAbsorbReflector>(Host);
	Absorb2->Install(Host);
	const int32 HealthBeforeSteal = Host->Health;
	AWoTFireballProjectile* FireHit = World->SpawnActor<AWoTFireballProjectile>(
		FVector(0, 6000, 500), FRotator::ZeroRotator);
	FireHit->SetSourceAngreal(Stealable);
	FireHit->ProcessTouch(Host, Host->GetActorLocation());
	TestEqual(TEXT("a stealable projectile hit is cancelled"),
		Host->Health, HealthBeforeSteal);
	TestNull(TEXT("the attacker loses the stealable artifact"),
		Attacker->InventoryComponent->FindInventoryType(
			AWoTTestFixedChargeAngreal::StaticClass()));
	TestEqual(TEXT("the host receives the stolen artifact"),
		Host->InventoryComponent->FindInventoryType(
			AWoTTestFixedChargeAngreal::StaticClass()),
		static_cast<AWoTInventory*>(Stealable));
	TestFalse(TEXT("the steal consumes the one-shot"), Absorb2->IsRegistered());

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAbsorbNPCCopyTest,
	"WoT.Inventory.Absorb.NPCCopy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTAbsorbNPCCopyTest::RunTest(const FString&)
{
	UWorld* World = CreateDamageDispatchWorld();
	AWoTNPCCharacter* NPC = World->SpawnActor<AWoTNPCCharacter>(
		FVector(1000, 0, 0), FRotator::ZeroRotator);
	AWoTCharacter* NPCHost = SpawnDamagePawn(World, FVector::ZeroVector);
	AWoTTestFixedChargeAngreal* NPCArtifact =
		World->SpawnActor<AWoTTestFixedChargeAngreal>(
			FVector(0, 5000, 500), FRotator::ZeroRotator);
	NPCArtifact->GiveTo(NPC);
	UWoTAbsorbReflector* NPCAbsorb = NewObject<UWoTAbsorbReflector>(NPCHost);
	NPCAbsorb->Install(NPCHost);
	AWoTFireballProjectile* NPCHit = World->SpawnActor<AWoTFireballProjectile>(
		FVector(0, 6000, 500), FRotator::ZeroRotator);
	NPCHit->SetSourceAngreal(NPCArtifact);
	TestTrue(TEXT("the NPC-sourced hit is absorbed"), NPCHost->AbsorbProjectile(NPCHit));
	TestEqual(TEXT("an NPC keeps its original artifact"),
		NPC->InventoryComponent->FindInventoryType(
			AWoTTestFixedChargeAngreal::StaticClass()),
		static_cast<AWoTInventory*>(NPCArtifact));
	AWoTTestFixedChargeAngreal* NPCCopy = Cast<AWoTTestFixedChargeAngreal>(
		NPCHost->InventoryComponent->FindInventoryType(
			AWoTTestFixedChargeAngreal::StaticClass()));
	TestNotNull(TEXT("the host gains a fresh NPC artifact copy"), NPCCopy);
	TestNotEqual(TEXT("the NPC copy is a distinct actor"), NPCCopy, NPCArtifact);
	if (NPCCopy)
	{
		TestEqual(TEXT("the fresh copy receives the spent-charge refund"),
			NPCCopy->CurCharges, 5);
	}

	AWoTCharacter* Player = SpawnDamagePawn(World, FVector(2000, 0, 0));
	AWoTCharacter* PlayerHost = SpawnDamagePawn(World, FVector(3000, 0, 0));
	AWoTTestFixedChargeAngreal* PlayerArtifact =
		World->SpawnActor<AWoTTestFixedChargeAngreal>(
			FVector(0, 5000, 500), FRotator::ZeroRotator);
	PlayerArtifact->GiveTo(Player);
	UWoTAbsorbReflector* PlayerAbsorb = NewObject<UWoTAbsorbReflector>(PlayerHost);
	PlayerAbsorb->Install(PlayerHost);
	AWoTFireballProjectile* PlayerHit = World->SpawnActor<AWoTFireballProjectile>(
		FVector(0, 6000, 500), FRotator::ZeroRotator);
	PlayerHit->SetSourceAngreal(PlayerArtifact);
	TestTrue(TEXT("the player-sourced hit is absorbed"),
		PlayerHost->AbsorbProjectile(PlayerHit));
	TestNull(TEXT("a player loses the stolen artifact"),
		Player->InventoryComponent->FindInventoryType(
			AWoTTestFixedChargeAngreal::StaticClass()));
	TestEqual(TEXT("the player's artifact actor moves to the host"),
		PlayerHost->InventoryComponent->FindInventoryType(
			AWoTTestFixedChargeAngreal::StaticClass()),
		static_cast<AWoTInventory*>(PlayerArtifact));

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAbsorbInstallStealTest,
	"WoT.Inventory.Absorb.InstallSteal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTAbsorbInstallStealTest::RunTest(const FString&)
{
	UWorld* World = CreateDamageDispatchWorld();
	AWoTCharacter* Attacker = SpawnDamagePawn(World, FVector(1000, 0, 0));
	AWoTCharacter* Host = SpawnDamagePawn(World, FVector::ZeroVector);
	AWoTTestFixedChargeAngreal* Source =
		World->SpawnActor<AWoTTestFixedChargeAngreal>(
			FVector(0, 5000, 500), FRotator::ZeroRotator);
	Source->GiveTo(Attacker);
	UWoTDecreaseHealthLeech* Leech = NewObject<UWoTDecreaseHealthLeech>(Host);
	Leech->SetSourceAngreal(Source);
	Leech->bFromProjectile = false;
	Leech->AttachTo(Host);
	UWoTAbsorbReflector* Absorb = NewObject<UWoTAbsorbReflector>(Host);
	Absorb->Install(Host);
	TestFalse(TEXT("the eligible leech is removed"), Leech->IsRegistered());
	TestFalse(TEXT("install-steal consumes Absorb"), Absorb->IsRegistered());
	TestEqual(TEXT("the leech's source artifact is stolen"),
		Host->InventoryComponent->FindInventoryType(
			AWoTTestFixedChargeAngreal::StaticClass()),
		static_cast<AWoTInventory*>(Source));

	AWoTCharacter* ProjectileHost = SpawnDamagePawn(World, FVector(2000, 0, 0));
	UWoTDecreaseHealthLeech* ProjectileLeech =
		NewObject<UWoTDecreaseHealthLeech>(ProjectileHost);
	// Production provenance: a projectile installer stamps bFromProjectile
	// (InitializeWithProjectile, AttachLeechEffect.uc:94-95) — the spec must
	// break if that propagation regresses.
	AWoTFireballProjectile* LeechCarrier = World->SpawnActor<AWoTFireballProjectile>(
		FVector(0, 7000, 500), FRotator::ZeroRotator);
	ProjectileLeech->InitializeWithProjectile(LeechCarrier);
	ProjectileLeech->AttachTo(ProjectileHost);
	UWoTAbsorbReflector* ProjectileAbsorb =
		NewObject<UWoTAbsorbReflector>(ProjectileHost);
	ProjectileAbsorb->Install(ProjectileHost);
	TestTrue(TEXT("a projectile-sourced leech is not install-stolen"),
		ProjectileLeech->IsRegistered());
	TestTrue(TEXT("Absorb remains after skipping projectile leeches"),
		ProjectileAbsorb->IsRegistered());

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAbsorbProcessEffectStealTest,
	"WoT.Inventory.Absorb.ProcessEffectSteal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTAbsorbProcessEffectStealTest::RunTest(const FString&)
{
	UWorld* World = CreateDamageDispatchWorld();
	AWoTCharacter* Attacker = SpawnDamagePawn(World, FVector(1000, 0, 0));
	auto MakeEffect = [](AWoTCharacter* Victim, AWoTAngrealInventory* Source)
	{
		FWoTDamageEffectSpec Effect;
		Effect.Damage = 10;
		Effect.HitLocation = Victim->GetActorLocation();
		Effect.DamageType = TEXT("xxFxx");
		Effect.SourceAngreal = Source;
		Effect.bDeleterious = true;
		return Effect;
	};

	AWoTCharacter* Host = SpawnDamagePawn(World, FVector::ZeroVector);
	AWoTTestFixedChargeAngreal* Source =
		World->SpawnActor<AWoTTestFixedChargeAngreal>(
			FVector(0, 5000, 500), FRotator::ZeroRotator);
	Source->GiveTo(Attacker);
	UWoTAbsorbReflector* Absorb = NewObject<UWoTAbsorbReflector>(Host);
	Absorb->Install(Host);
	const int32 HealthBefore = Host->Health;
	FWoTDamageEffectSpec NonProjectile = MakeEffect(Host, Source);
	TestEqual(TEXT("a non-projectile effect is consumed"),
		Host->ProcessEffect(NonProjectile), 0);
	TestEqual(TEXT("consumed effect does no damage"), Host->Health, HealthBefore);
	TestFalse(TEXT("ProcessEffect steal consumes Absorb"), Absorb->IsRegistered());
	TestEqual(TEXT("the non-projectile effect's artifact is stolen"),
		Host->InventoryComponent->FindInventoryType(
			AWoTTestFixedChargeAngreal::StaticClass()),
		static_cast<AWoTInventory*>(Source));

	AWoTCharacter* ProjectileHost = SpawnDamagePawn(World, FVector(2000, 0, 0));
	AWoTTestFixedChargeAngreal* ProjectileSource =
		World->SpawnActor<AWoTTestFixedChargeAngreal>(
			FVector(0, 5000, 500), FRotator::ZeroRotator);
	ProjectileSource->GiveTo(Attacker);
	UWoTAbsorbReflector* ProjectileAbsorb =
		NewObject<UWoTAbsorbReflector>(ProjectileHost);
	ProjectileAbsorb->Install(ProjectileHost);
	// Production provenance: deliver through the projectile's own ProcessDamage
	// so its SourceProjClass stamp (WoTGenericProjectile.cpp) is what exempts
	// the effect — the spec breaks if that propagation regresses.
	AWoTFireballProjectile* EffectCarrier = World->SpawnActor<AWoTFireballProjectile>(
		FVector(0, 8000, 500), FRotator::ZeroRotator);
	EffectCarrier->SetSourceAngreal(ProjectileSource);
	const int32 ProjectileHealthBefore = ProjectileHost->Health;
	EffectCarrier->ProcessDamage(ProjectileHost, 10.0f, nullptr,
		ProjectileHost->GetActorLocation(), FVector::ZeroVector);
	TestEqual(TEXT("a projectile-sourced effect passes to damage"),
		ProjectileHost->Health, ProjectileHealthBefore - 10);
	TestTrue(TEXT("projectile ProcessEffect leaves Absorb installed"),
		ProjectileAbsorb->IsRegistered());
	TestEqual(TEXT("the projectile-sourced artifact is not stolen"),
		Attacker->InventoryComponent->FindInventoryType(
			AWoTTestFixedChargeAngreal::StaticClass()),
		static_cast<AWoTInventory*>(ProjectileSource));

	AWoTCharacter* OwnHost = SpawnDamagePawn(World, FVector(3000, 0, 0));
	AWoTTestFixedChargeAngreal* OwnSource =
		World->SpawnActor<AWoTTestFixedChargeAngreal>(
			FVector(0, 5000, 500), FRotator::ZeroRotator);
	OwnSource->GiveTo(OwnHost);
	UWoTAbsorbReflector* OwnAbsorb = NewObject<UWoTAbsorbReflector>(OwnHost);
	OwnAbsorb->SetSourceAngreal(OwnSource);
	OwnAbsorb->Install(OwnHost);
	FWoTDamageEffectSpec OwnEffect = MakeEffect(OwnHost, OwnSource);
	TestEqual(TEXT("the host's own-artifact effect passes"),
		OwnHost->ProcessEffect(OwnEffect), 10);
	TestEqual(TEXT("own-artifact damage lands"), OwnHost->Health, 90);
	TestTrue(TEXT("own-artifact effects leave Absorb installed"),
		OwnAbsorb->IsRegistered());

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTProcessEffectOrderingTest,
	"WoT.Reflector.ProcessEffectOrdering",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTProcessEffectOrderingTest::RunTest(const FString&)
{
	UWorld* World = CreateDamageDispatchWorld();
	AWoTCharacter* Attacker = SpawnDamagePawn(World, FVector(1000, 0, 0));
	AWoTTestFixedChargeAngreal* Source =
		World->SpawnActor<AWoTTestFixedChargeAngreal>(
			FVector(0, 5000, 500), FRotator::ZeroRotator);
	Source->GiveTo(Attacker);
	FWoTDamageEffectSpec Effect;
	Effect.Damage = 10;
	Effect.DamageType = TEXT("xxFxx");
	Effect.SourceAngreal = Source;
	Effect.bDeleterious = true;

	AWoTCharacter* BlockedHost = SpawnDamagePawn(World, FVector::ZeroVector);
	Effect.HitLocation = BlockedHost->GetActorLocation();
	UWoTAbsorbReflector* LowerAbsorb =
		NewObject<UWoTAbsorbReflector>(BlockedHost);
	LowerAbsorb->Install(BlockedHost);
	UWoTNoBadEffectsReflector* P255Blocker =
		NewObject<UWoTNoBadEffectsReflector>(BlockedHost);
	P255Blocker->Install(BlockedHost);
	TestEqual(TEXT("the priority-255 blocker stops before Absorb 170"),
		BlockedHost->ProcessEffect(Effect), 0);
	TestEqual(TEXT("the blocker prevents stealing"),
		Attacker->InventoryComponent->FindInventoryType(
			AWoTTestFixedChargeAngreal::StaticClass()),
		static_cast<AWoTInventory*>(Source));
	TestTrue(TEXT("the lower-priority Absorb was not consumed"),
		LowerAbsorb->IsRegistered());

	AWoTCharacter* AbsorbHost = SpawnDamagePawn(World, FVector(2000, 0, 0));
	Effect.HitLocation = AbsorbHost->GetActorLocation();
	UWoTAbsorbReflector* OnlyAbsorb =
		NewObject<UWoTAbsorbReflector>(AbsorbHost);
	OnlyAbsorb->Install(AbsorbHost);
	const int32 HealthBefore = AbsorbHost->Health;
	TestEqual(TEXT("Absorb consumes before the default bottom"),
		AbsorbHost->ProcessEffect(Effect), 0);
	TestEqual(TEXT("the consumed effect never invokes damage"),
		AbsorbHost->Health, HealthBefore);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTDeadProjectileTouchInertTest,
	"WoT.Damage.DeadProjectileTouchInert",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTDeadProjectileTouchInertTest::RunTest(const FString&)
{
	// A pending-kill projectile touches nothing: without the ProcessTouch liveness
	// gate it would skip every reflector interception (IsProjectileAffectable
	// rejects a dying projectile) yet still deal its damage a second time.
	UWorld* World = CreateDamageDispatchWorld();
	AWoTCharacter* Victim = SpawnDamagePawn(World, FVector(200, 0, 100));
	Victim->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
	AWoTDartProjectile* Dart = World->SpawnActor<AWoTDartProjectile>(
		FVector::ZeroVector, FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("dart spawned"), Dart))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	const int32 HealthBefore = Victim->Health;
	Dart->Destroy();
	Dart->ProcessTouch(Victim, Victim->GetActorLocation());
	TestEqual(TEXT("a destroyed projectile deals no damage"),
		Victim->Health, HealthBefore);
	TestTrue(TEXT("a destroyed projectile imparts no momentum"),
		Victim->GetCharacterMovement()->Velocity.IsZero());
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTIllusionCastReplacesTest,
	"WoT.Inventory.Illusion.CastReplaces",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTIllusionCastReplacesTest::RunTest(const FString&)
{
	UWorld* World = CreateDamageDispatchWorld();
	// This pre-R1 fixture verifies two casts from the Easy charge table.
	World->GetAuthGameMode<AWoTGameMode>()->Difficulty = 0;
	AWoTCharacter* Caster = SpawnDamagePawn(World, FVector(0, 0, 100));
	Caster->SetActorRotation(FRotator(0.0f, 90.0f, 0.0f));
	Caster->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	World->SpawnActor<AWoTAngrealInvIllusion>(
		Caster->GetActorLocation(), FRotator::ZeroRotator);
	WoTTest::TickWorld(World, 0.2f);
	AWoTAngrealInvIllusion* Held = Cast<AWoTAngrealInvIllusion>(
		Caster->InventoryComponent->SelectedItem.Get());
	if (!TestNotNull(TEXT("Personal Illusion picked up"), Held))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	Held->CastArtifact();
	AWoTAngrealIllusionProjectile* First = Held->GetIllusion();
	if (TestNotNull(TEXT("first cast creates an illusion"), First))
	{
		TestTrue(TEXT("SpawnOffset X=48 rotates with the owner"),
			First->GetActorLocation().Equals(FVector(0, 48, 100), 1.0f));
		TestTrue(TEXT("illusion identity is the caster instigator"),
			First->GetInstigator() == Caster);
		TestTrue(TEXT("illusion adds one good caster countdown icon"),
			Caster->StatusIcons.Num() == 1 && Caster->StatusIcons[0].bGood &&
			IsValid(First->Timer) &&
			Caster->StatusIcons[0].IconID == First->Timer->GetFName());
		TestTrue(TEXT("illusion timer is caster-owned and non-removable"),
			IsValid(First->Timer) && First->Timer->GetOwner() == Caster &&
			!First->Timer->bRemovable);
		TestTrue(TEXT("illusion timer copies the projectile's remaining lifespan"),
			IsValid(First->Timer) &&
			FMath::IsNearlyEqual(First->Timer->Lifespan, First->GetLifeSpan(), 0.1f));
	}
	TWeakObjectPtr<UWoTIllusionTimerLeech> FirstTimer;
	if (First)
	{
		FirstTimer = First->Timer;
	}
	Held->CastArtifact();
	AWoTAngrealIllusionProjectile* Second = Held->GetIllusion();
	TestTrue(TEXT("second cast destroys the first caster-owned illusion"),
		!IsValid(First));
	TestTrue(TEXT("second cast leaves one new caster-owned illusion"),
		IsValid(Second) && Second != First && Second->GetInstigator() == Caster);
	TestFalse(TEXT("replacing the illusion destroys its old countdown leech"),
		FirstTimer.IsValid());
	TestTrue(TEXT("replacement leaves exactly one new countdown icon"),
		Caster->StatusIcons.Num() == 1 && IsValid(Second) && IsValid(Second->Timer) &&
		Caster->StatusIcons[0].IconID == Second->Timer->GetFName());
	if (IsValid(Second))
	{
		Second->Destroy();
	}
	TestEqual(TEXT("illusion destruction removes its countdown icon"),
		Caster->StatusIcons.Num(), 0);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTIllusionBattleTeamFilterTest,
	"WoT.Inventory.Illusion.BattleTeamFilter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTIllusionBattleTeamFilterTest::RunTest(const FString&)
{
	UWorld* World = CreateDamageDispatchWorld(true);
	APlayerController* Controller = World->SpawnActor<APlayerController>();
	Controller->InitPlayerState();
	AWoTCharacter* OwnerPawn = SpawnDamagePawn(World, FVector::ZeroVector);
	Controller->Possess(OwnerPawn);
	AWoTPlayerState* PlayerState = Controller->GetPlayerState<AWoTPlayerState>();
	if (!TestNotNull(TEXT("Battle owner has a WoT player state"), PlayerState))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	PlayerState->Team = 2;
	AWoTAngrealInvSeeker* Targeter = World->SpawnActor<AWoTAngrealInvSeeker>(
		FVector(2000, 0, 0), FRotator::ZeroRotator);
	Targeter->SetOwner(OwnerPawn);
	AWoTAngrealIllusionProjectile* Illusion =
		World->SpawnActor<AWoTAngrealIllusionProjectile>(
			FVector(500, 0, 0), FRotator::ZeroRotator);
	Illusion->Team = 2;
	TestTrue(TEXT("same-team illusion is not targetable in Battle"),
		Targeter->IsNotTargetable(Illusion));
	Illusion->Team = 3;
	TestFalse(TEXT("enemy illusion remains targetable in Battle"),
		Targeter->IsNotTargetable(Illusion));

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSeekerTargetLifecycleTest,
	"WoT.Inventory.Seeker.TargetLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTSeekerTargetLifecycleTest::RunTest(const FString&)
{
	// SeekingProjectile.uc:688-715 — a dead target no longer exists
	// to the seeker (uc:705; retail's player NumDeaths snapshot is subsumed by the
	// port's fresh-pawn-per-respawn actor identity, PORT_NOTES).
	UWorld* World = CreateDamageDispatchWorld();
	AWoTCharacter* Target = SpawnDamagePawn(World, FVector(500, 0, 0));
	AWoTSeekerProjectile* Seeker = World->SpawnActor<AWoTSeekerProjectile>(
		FVector::ZeroVector, FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("target"), Target) || !TestNotNull(TEXT("seeker"), Seeker))
	{
		WoTTest::DestroySpecWorld(World);
		return true;
	}
	Seeker->SetDestination(Target);
	TestTrue(TEXT("live target exists to the seeker"), Seeker->DestinationExists());
	Target->Health = 0;   // the dead window before the corpse swap
	TestFalse(TEXT("a dead target no longer exists to the seeker"),
		Seeker->DestinationExists());
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTIllusionSeekerDecoyTest,
	"WoT.Inventory.Illusion.SeekerDecoy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTIllusionSeekerDecoyTest::RunTest(const FString&)
{
	// The PRODUCTION decoy consumer: the Mashadar tendril is retail's only
	// bNotifySeePlayer=True projectile (MashadarGuide.uc:1934); its SeePlayer
	// chases whatever notified it (uc:504-552) and Search only re-targets when a
	// closest PAWN exists (uc:1345-1382) — an illusion holds the empty-scan chase.
	UWorld* World = CreateDamageDispatchWorld();
	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTMashadarGuide* Guide = World->SpawnActor<AWoTMashadarGuide>(
		AWoTMashadarGuide::StaticClass(), FVector(0, 0, 100),
		FRotator::ZeroRotator, SP);
	if (!TestNotNull(TEXT("Mashadar guide spawned"), Guide))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	TestTrue(TEXT("the guide ships the retail SeePlayer notification default"),
		Guide->bNotifySeePlayer);
	TestEqual(TEXT("the SeePlayer cylinder is the hunt radius"),
		Guide->SeePlayerRadius, Guide->SearchRadius);
	// SeePlayer -> Chase is gated by IsChasable (uc:550 -> 1235): the illusion leg
	// (uc:1292) requires a DIFFERING team. Author the cross-team shape the arena
	// produces (the caster stamps the summon's team); a teamless pair is unchasable
	// in retail too.
	Guide->Team = 1;

	AWoTAngrealIllusionProjectile* Illusion =
		World->SpawnActor<AWoTAngrealIllusionProjectile>(
			AWoTAngrealIllusionProjectile::StaticClass(), FVector(400, 0, 100),
			FRotator::ZeroRotator, SP);
	if (!TestNotNull(TEXT("illusion spawned"), Illusion))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Illusion->MyShowSelf();
	TestTrue(TEXT("a tendril inside the ShowSelf cylinder chases the decoy"),
		Guide->Destination.Get() == Illusion);
	TestTrue(TEXT("chasing the decoy activates the tendril"), Guide->bActive);

	// The search cadence with no pawn in radius must NOT drop the decoy.
	Guide->SearchAndChase();
	TestTrue(TEXT("an empty pawn scan keeps the decoy chase"),
		Guide->Destination.Get() == Illusion);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTDoTPainReactTest,
	"WoT.Damage.DoTPainReact",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTDoTPainReactTest::RunTest(const FString&)
{
	UWorld* World = CreateDamageDispatchWorld();
	AWoTTestDamageDispatchCharacter* Victim =
		World->SpawnActor<AWoTTestDamageDispatchCharacter>(
			FVector::ZeroVector, FRotator::ZeroRotator);
	Victim->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	Victim->GetCharacterMovement()->Velocity = FVector::ZeroVector;
	UWoTDecreaseHealthLeech* Leech = NewObject<UWoTDecreaseHealthLeech>(Victim);
	Leech->MinPainReactPerMinute = 60.0f;
	Leech->MaxPainReactPerMinute = 60.0f;
	Leech->AttachTo(Victim);
	const int32 HealthBefore = Victim->Health;
	Leech->AffectHost(1);
	TestEqual(TEXT("the normal DoT tick still removes one health"),
		Victim->Health, HealthBefore - 1);
	TestEqual(TEXT("the cadence fires one zero-damage ProcessEffect"),
		Victim->ProcessEffectCalls, 1);
	TestEqual(TEXT("the pain-react effect carries zero damage"),
		Victim->LastEffect.Damage, 0);
	TestTrue(TEXT("the pain-react effect carries zero momentum"),
		Victim->LastEffect.UE1Momentum.IsZero());
	TestTrue(TEXT("the pain-react dispatch has no momentum side effect"),
		Victim->GetCharacterMovement()->Velocity.IsZero());
	TestEqual(TEXT("zero-damage bottom still reaches PlayHit"),
		Victim->LastHitDamage, 0);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPresentationAPlayerHitFeedbackSpec,
	"WoT.RC33.PresentationA.P1.PlayerHitFeedback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTPresentationAPlayerHitFeedbackSpec::RunTest(const FString&)
{
	UWorld* World = CreateDamageDispatchWorld();
	AWoTCharacter* Victim = SpawnDamagePawn(World, FVector(0, 0, 100));
	AWoTPlayerController* Owner = World->SpawnActor<AWoTPlayerController>();
	AWoTPlayerController* Observer = World->SpawnActor<AWoTPlayerController>();
	Owner->SetAsLocalPlayerController();
	Owner->Possess(Victim);
	Victim->Health = Victim->MaxHealth = 100;
	FWoTDamageEvent Fire = MomentumEvent(FVector::ZeroVector, TEXT("xxFxx"));
	Fire.HitInfo.ImpactPoint = Victim->GetActorLocation();
	Victim->TakeDamage(20.0f, Fire, nullptr, nullptr);
	TestEqual(TEXT("damage requests retail TakeDamage and PlayHit owner shakes"),
		Owner->GetShakeRequestCountForTest(), 2);
	TestEqual(TEXT("non-owning controller receives no damage shake"),
		Observer->GetShakeRequestCountForTest(), 0);
	TestEqual(TEXT("retail fire flash scale reaches the owner"),
		Owner->DesiredFlashScale, -0.1875f, 0.0001f);
	TestTrue(TEXT("retail fire flash is yellow-weighted"),
		Owner->DesiredFlashFog.X > Owner->DesiredFlashFog.Y
			&& Owner->DesiredFlashFog.Y > Owner->DesiredFlashFog.Z);
	const int32 BeforeContinuous = Owner->GetShakeRequestCountForTest();
	FWoTDamageEvent Continuous = MomentumEvent(
		FVector::ZeroVector, TEXT("ContinuousDamage"));
	Continuous.HitInfo.ImpactPoint = Victim->GetActorLocation();
	Victim->TakeDamage(10.0f, Continuous, nullptr, nullptr);
	TestEqual(TEXT("continuous damage keeps both the raw and generic PlayHit shakes"),
		Owner->GetShakeRequestCountForTest(), BeforeContinuous + 2);

	Owner->DesiredFlashScale = 0.0f;
	Owner->DesiredFlashFog = FVector::ZeroVector;
	AWoTZoneVolume* Water = World->SpawnActor<AWoTZoneVolume>(
		Victim->GetActorLocation(), FRotator::ZeroRotator);
	Water->bWaterZone = true;
	Water->ViewFog = FVector(0.0f, 0.2f, 0.35f);
	Water->Collision->SetBoxExtent(FVector(500.0f));
	WoTTest::TickWorld(World, 0.2f);
	TestTrue(TEXT("water ViewFog contributes to the owning view color"),
		Owner->CurrentViewColorScale.Z > Owner->CurrentViewColorScale.Y
			&& Owner->CurrentViewColorScale.Y > Owner->CurrentViewColorScale.X);
	TestTrue(TEXT("underwater presentation stays owner-only"),
		Observer->CurrentViewColorScale.Equals(FVector::OneVector, KINDA_SMALL_NUMBER));
	const int32 BeforeLethal = Owner->GetShakeRequestCountForTest();
	Victim->TakeDamage(1000.0f, Fire, nullptr, nullptr);
	TestEqual(TEXT("lethal damage keeps the raw shake but skips post-health PlayHit"),
		Owner->GetShakeRequestCountForTest(), BeforeLethal + 1);
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
