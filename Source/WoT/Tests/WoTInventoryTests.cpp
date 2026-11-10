// Class automation spec for the artifact layer:
// pickup → inventory → cast gate → launcher → projectile, against the
// AngrealInventory.uc / ProjectileLauncher.uc spec behaviors.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../Inventory/WoTAngrealInvFireball.h"
#include "../Inventory/WoTAngrealInvSeeker.h"
#include "../Inventory/WoTAngrealInvTaint.h"
#include "../Inventory/WoTAngrealInventory.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../Projectiles/WoTFireballProjectile.h"
#include "../Projectiles/WoTSeekerProjectile.h"
#include "../Projectiles/WoTTaintProjectile.h"
#include "../WoTAirBlastFX.h"
#include "../WoTBeamEffect.h"
#include "../WoTCharacter.h"
#include "../WoTCollisionBlocker.h"
#include "../WoTGameMode.h"
#include "../WoTWeaveFX.h"
#include "../WoTGameState.h"
#include "../WoTCitadelGameState.h"
#include "../WoTBattleInfo.h"
#include "../WoTBattleGameMode.h"
#include "../WoTSealAltar.h"
#include "../WoTBudgetInfo.h"
#include "../WoTPlayerStart.h"
#include "../WoTLeafMap.h"
#include "../Inventory/WoTSeal.h"
#include "WoTTestBattleClasses.h"
#include "../WoTPlayerController.h"
#include "../WoTPlayerState.h"
#include "../AI/WoTCreatureData.h"
#include "../AI/WoTPathNode.h"
#include "../AI/WoTNPCCharacter.h"
#include "../WoTSoundTable.h"
#include "Components/AudioComponent.h"
#include "../WoTMover.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerStart.h"
#include "../Inventory/WoTLeech.h"
#include "../Inventory/WoTAngrealSpawnPool.h"
#include "../Inventory/WoTAngrealInvIce.h"
#include "../Inventory/WoTIceLeech.h"
#include "../Projectiles/WoTIceProjectile.h"
#include "../Inventory/WoTAngrealInvDecay.h"
#include "../Inventory/WoTDecreaseChargesLeech.h"
#include "../Projectiles/WoTDecayProjectile.h"
#include "../Inventory/WoTAngrealInvSoulBarb.h"
#include "../Projectiles/WoTSoulBarbProjectile.h"
#include "../Inventory/WoTAngrealInvDart.h"
#include "../Projectiles/WoTDartProjectile.h"
#include "../Inventory/WoTAngrealInvExpWard.h"
#include "../Projectiles/WoTExpWardProjectile.h"
#include "../Projectiles/WoTFireworksProjectile.h"
#include "../Inventory/WoTAngrealInvLevitate.h"
#include "../Inventory/WoTAngrealInvWhirlwind.h"
#include "../Inventory/WoTWhirlwindLeech.h"
#include "../Inventory/WoTAngrealInvSpecial.h"
#include "../Inventory/WoTAngrealInvHeal.h"
#include "../Inventory/WoTDecreaseHealthLeech.h"
#include "../Inventory/WoTAngrealInvShields.h"
#include "../Inventory/WoTReflector.h"
#include "../Inventory/WoTIgnoreElementReflector.h"
#include "../Inventory/WoTRarityBasedDamageReflector.h"
#include "../Inventory/WoTShieldReflector.h"
#include "../Inventory/WoTAngrealInvTarget.h"
#include "../Inventory/WoTAutoTargetReflector.h"
#include "../Inventory/WoTAngrealInvReflect.h"
#include "../Inventory/WoTAngrealInvFork.h"
#include "../Inventory/WoTAngrealInvAbsorb.h"
#include "../Inventory/WoTAngrealInvAirBurst.h"
#include "../Inventory/WoTAngrealInvShift.h"
#include "../Inventory/WoTAngrealInvEarthTremor.h"
#include "../Inventory/WoTEarthTremorLeech.h"
#include "../Projectiles/WoTEarthTremorProjectile.h"
#include "../Inventory/WoTAngrealInvBalefire.h"
#include "../Inventory/WoTAngrealInvSwapPlaces.h"
#include "../Projectiles/WoTSwapPlacesProjectile.h"
#include "../Inventory/WoTAngrealInvRemoveCurse.h"
#include "../Projectiles/WoTAMAPearl.h"
#include "../Inventory/WoTAngrealInvAMA.h"
#include "../Projectiles/WoTAngrealAMAProjectile.h"
#include "../WoTZoneVolume.h"
#include "../Inventory/WoTAngrealInvLightning.h"
#include "../WoTTeleporter.h"
#include "../Inventory/WoTAMAReflectors.h"
#include "../Inventory/WoTAngrealInvTrapDetect.h"
#include "../Inventory/WoTTrapDetectLeech.h"
#include "../Traps/WoTAlarm.h"
#include "../Traps/WoTFireWall.h"
#include "../Traps/WoTSpear.h"
#include "../Traps/WoTStaircase.h"
#include "../Inventory/WoTAngrealInvDisguise.h"
#include "../Inventory/WoTAngrealInvTracer.h"
#include "../Projectiles/WoTTracerProjectile.h"
#include "../Inventory/WoTAngrealInvWallOfAir.h"
#include "../Projectiles/WoTWallOfAirProjectile.h"
#include "../Inventory/WoTDestroyOnDestroyLeech.h"
#include "WoTTestWallProjectile.h"
#include "../WoTSound.h"
#include "../WoTAmbientSound.h"
#include "../WoTMapLightTable.h"
#include "../WoTMeshLight.h"
#include "../WoTVertexAnimComponent.h"
#include "../WoTBreakableDecoration.h"
#include "../WoTCarcass.h"
#include "../WoTCreatureChunk.h"
#include "../WoTFragment.h"
#include "../WoTExplodingMover.h"
#include "../Projectiles/WoTLegionProjectile.h"
#include "../Projectiles/WoTMashadarGuide.h"
#include "../Projectiles/WoTMashadarSegment.h"
#include "../Traps/WoTPortcullis.h"
#include "../Traps/WoTWall.h"
#include "Sound/SoundAttenuation.h"
#include "../Projectiles/WoTProjectile.h"   // FWoTDamageEvent
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "WoTTestLeech.h"

#include "Components/InputComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerInput.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "GameFramework/WorldSettings.h"
#include "InputCoreTypes.h"
#include "InputKeyEventArgs.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/Texture.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAngrealActivateSoundBindingsSpec,
	"WoT.Inventory.ActivateSoundBindings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTAngrealActivateSoundBindingsSpec::RunTest(const FString&)
{
	TestFalse(TEXT("Heal ActivateHL is bound"),
		GetDefault<AWoTAngrealInvHeal>()->ActivateSound.IsNull());
	TestFalse(TEXT("Absorb ActivateABS is bound"),
		GetDefault<AWoTAngrealInvAbsorb>()->ActivateSound.IsNull());
	TestFalse(TEXT("Fork ActivateFK is bound"),
		GetDefault<AWoTAngrealInvFork>()->ActivateSound.IsNull());
	TestFalse(TEXT("Reflect ActivateRF is bound"),
		GetDefault<AWoTAngrealInvReflect>()->ActivateSound.IsNull());
	TestFalse(TEXT("Shift ActivateSF is bound"),
		GetDefault<AWoTAngrealInvShift>()->ActivateSound.IsNull());
	TestFalse(TEXT("Find Target ActivateTA is bound"),
		GetDefault<AWoTAngrealInvTarget>()->ActivateSound.IsNull());
	TestFalse(TEXT("Fire Shield ActivateFS is bound"),
		GetDefault<AWoTAngrealInvFireShield>()->ActivateSound.IsNull());
	TestFalse(TEXT("Water Shield ActivateWS is bound"),
		GetDefault<AWoTAngrealInvWaterShield>()->ActivateSound.IsNull());
	TestFalse(TEXT("Air Shield ActivateAS is bound"),
		GetDefault<AWoTAngrealInvAirShield>()->ActivateSound.IsNull());
	TestFalse(TEXT("Earth Shield ActivateES is bound"),
		GetDefault<AWoTAngrealInvEarthShield>()->ActivateSound.IsNull());
	TestFalse(TEXT("Spirit Shield ActivateSS is bound"),
		GetDefault<AWoTAngrealInvSpiritShield>()->ActivateSound.IsNull());
	TestFalse(TEXT("Personal Shield ActivateSD is bound"),
		GetDefault<AWoTAngrealInvShield>()->ActivateSound.IsNull());
	return true;
}

namespace
{
	bool InventorySpecMaterialParentChainContains(
		const UMaterialInterface* Material, const UMaterialInterface* ExpectedParent)
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

	// minimal ticking game world with the project game mode (NM_Standalone,
	// so items take the non-respawn path and SP charge seeding)
	// As above, but with the Battle game mode as the authority (for the citadel/siege classes,
	// which reach it via GetAuthGameMode<AWoTBattleGameMode>). Setting the world settings'
	// DefaultGameMode before SetGameMode makes GetGameModeForURL pick it.
	UWorld* CreateBattleTestWorld()
	{
		return WoTTest::CreateSpecWorld(AWoTBattleGameMode::StaticClass());
	}

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInventoryFireballTest,
	"WoT.Inventory.FireballEndToEnd",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTInventoryFireballTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	// This spec explicitly asserts the shipped Easy initial-charge value.
	World->GetAuthGameMode<AWoTGameMode>()->Difficulty = 0;

	// pawn flies — no floor in the test world
	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	UWoTInventoryComponent* Inv = Pawn->InventoryComponent;

	// a placed fireball artifact, out of reach
	AWoTAngrealInvFireball* Ground = World->SpawnActor<AWoTAngrealInvFireball>(
		FVector(500, 0, 100), FRotator::ZeroRotator);
	WoTTest::TickWorld(World, 0.1f);

	// standalone world → SP-Easy seeding: SPInitialCharges unset, so
	// AddCharges(MaxInitialCharges=20); MP path is RandRange(10,20)
	TestEqual(TEXT("ground item seeded initial charges"), Ground->CurCharges, 20);
	TestNull(TEXT("nothing selected before pickup"), Inv->SelectedItem.Get());

	// walk over it → Touch → pickup (no game mode: the ground actor itself
	// becomes the held item, as UE1 without ShouldRespawn)
	const int32 GroundCharges = Ground->CurCharges;
	Pawn->SetActorLocation(Ground->GetActorLocation());
	WoTTest::TickWorld(World, 0.1f);

	AWoTAngrealInventory* Held = Cast<AWoTAngrealInventory>(Inv->SelectedItem.Get());
	TestNotNull(TEXT("pickup auto-selected the first artifact"), Held);
	if (!Held)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	TestTrue(TEXT("held item is the ground actor (non-respawn path)"),
		Held == Ground);
	TestEqual(TEXT("charges survive the pickup"), Held->CurCharges, GroundCharges);
	TestTrue(TEXT("held item owned by the pawn"), Held->GetOwner() == Pawn);
	TestTrue(TEXT("held item hidden"), Held->IsHidden());

	// UseAngreal → cast gate → launcher → fireball projectile
	Inv->UseAngreal();
	AWoTFireballProjectile* Proj = nullptr;
	for (TActorIterator<AWoTFireballProjectile> It(World); It; ++It)
	{
		Proj = *It;
	}
	TestNotNull(TEXT("fireball projectile spawned"), Proj);
	TestEqual(TEXT("one charge used"), Held->CurCharges, GroundCharges - 1);
	if (Proj)
	{
		TestTrue(TEXT("projectile source angreal set"), Proj->SourceAngreal == Held);
		TestTrue(TEXT("shooter is the ignored pawn"), Proj->IgnoredPawn == Pawn);

		// PHYS_Projectile: Speed 1000, gravity 0
		const FVector Start = Proj->GetActorLocation();
		WoTTest::TickWorld(World, 0.25f);
		const float Travelled = FVector::Dist(Start, Proj->GetActorLocation());
		TestTrue(FString::Printf(TEXT("projectile travels at Speed (moved %.0f)"), Travelled),
			Travelled > 200.0f && Travelled < 300.0f);
	}

	// bRestrictsUsage: RoundsPerMinute 90 → 0.667s between casts
	Inv->CeaseUsingAngreal();
	Inv->UseAngreal();
	TestEqual(TEXT("immediate re-cast blocked by the cast gate"),
		Held->CurCharges, GroundCharges - 1);
	WoTTest::TickWorld(World, 0.7f);
	Inv->CeaseUsingAngreal();
	Inv->UseAngreal();
	TestEqual(TEXT("re-cast allowed after 60/RoundsPerMinute"),
		Held->CurCharges, GroundCharges - 2);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInventoryChargeStealTest,
	"WoT.Inventory.PickupQueryChargeSteal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTInventoryChargeStealTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	UWoTInventoryComponent* Inv = Pawn->InventoryComponent;

	AWoTAngrealInvFireball* First = World->SpawnActor<AWoTAngrealInvFireball>(
		Pawn->GetActorLocation(), FRotator::ZeroRotator);
	WoTTest::TickWorld(World, 0.1f);
	AWoTAngrealInventory* Held = Cast<AWoTAngrealInventory>(Inv->SelectedItem.Get());
	TestNotNull(TEXT("first fireball picked up"), Held);
	if (!Held)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	// second fireball of the same class: charges absorbed, item not taken
	Held->CurCharges = 12;
	AWoTAngrealInvFireball* Second = World->SpawnActor<AWoTAngrealInvFireball>(
		FVector(500, 0, 100), FRotator::ZeroRotator);
	Second->CurCharges = 30;   // stolen; MaxCharges=35 clamps 12+30 → 35
	Second->bTainted = true;
	Pawn->SetActorLocation(Second->GetActorLocation());
	WoTTest::TickWorld(World, 0.1f);

	TestEqual(TEXT("charges stolen and clamped to MaxCharges"), Held->CurCharges, 35);
	TestTrue(TEXT("taint transfers with the steal"), Held->bTainted);
	TestEqual(TEXT("inventory still holds exactly one item"), Inv->Items.Num(), 1);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSeekerTargetingTest,
	"WoT.Inventory.SeekerTargetingAndTaint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTSeekerTargetingTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	AWoTCharacter* Shooter = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	Shooter->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	UWoTInventoryComponent* Inv = Shooter->InventoryComponent;

	World->SpawnActor<AWoTAngrealInvSeeker>(
		Shooter->GetActorLocation(), FRotator::ZeroRotator);
	WoTTest::TickWorld(World, 0.1f);
	AWoTAngrealInventory* Seeker = Cast<AWoTAngrealInventory>(Inv->SelectedItem.Get());
	TestNotNull(TEXT("seeker picked up"), Seeker);
	if (!Seeker)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	const int32 Charges = Seeker->CurCharges;

	// bRequiresDestination + no target in view → Failed(), no charge used
	Inv->UseAngreal();
	TestEqual(TEXT("seeker without a target fails without using a charge"),
		Seeker->CurCharges, Charges);

	// a victim 500uu down the view axis, holding a fireball
	AWoTCharacter* Victim = World->SpawnActor<AWoTCharacter>(
		FVector(500, 0, 100), FRotator::ZeroRotator);
	Victim->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	AWoTAngrealInvFireball* VictimFireball = World->SpawnActor<AWoTAngrealInvFireball>(
		Victim->GetActorLocation(), FRotator::ZeroRotator);
	WoTTest::TickWorld(World, 0.1f);
	TestFalse(TEXT("victim's fireball starts untainted"), VictimFireball->bTainted);

	Inv->CeaseUsingAngreal();
	Inv->UseAngreal();
	AWoTSeekerProjectile* Proj = nullptr;
	for (TActorIterator<AWoTSeekerProjectile> It(World); It; ++It)
	{
		Proj = *It;
	}
	TestNotNull(TEXT("seeker projectile launched at the found target"), Proj);
	TestEqual(TEXT("seeker used a charge"), Seeker->CurCharges, Charges - 1);
	if (Proj)
	{
		TestTrue(TEXT("destination targeting found the victim"),
			Proj->Destination == Victim);
	}

	// switch to Taint, hit the victim, verify their artifacts taint
	World->SpawnActor<AWoTAngrealInvTaint>(
		Shooter->GetActorLocation(), FRotator::ZeroRotator);
	WoTTest::TickWorld(World, 0.1f);
	AWoTInventory* Taint = Inv->FindInventoryName(FName(TEXT("AngrealInvTaint")));
	TestNotNull(TEXT("taint picked up (FindInventoryName on original name)"), Taint);
	if (!Taint)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Inv->SelectItem(Taint);
	WoTTest::TickWorld(World, 1.0f);   // clear the cast gate
	Inv->UseAngreal();
	AWoTTaintProjectile* TaintProj = nullptr;
	for (TActorIterator<AWoTTaintProjectile> It(World); It; ++It)
	{
		TaintProj = *It;
	}
	TestNotNull(TEXT("taint projectile launched"), TaintProj);

	// seek across the 500uu: Speed 150 + AccelPerSec ramp
	WoTTest::TickWorld(World, 4.0f);
	TestTrue(TEXT("victim's fireball is tainted after the hit"),
		VictimFireball->bTainted);
	TestTrue(TEXT("taint instigator is the shooter"),
		VictimFireball->TaintInstigator == Shooter);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTLeechFrameworkTest,
	"WoT.Inventory.Leech",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTLeechFrameworkTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	auto CountLeeches = [](AActor* Host)
	{
		TArray<UWoTLeech*> A;
		Host->GetComponents<UWoTLeech>(A);
		return A.Num();
	};

	AWoTCharacter* Host = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	Host->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	WoTTest::TickWorld(World, 0.1f);

	// 1) AffectHost fires once per AffectResolution, with iteration catch-up.
	UWoTTestLeech* L = NewObject<UWoTTestLeech>(Host);
	L->AffectResolution = 0.1f;   // Lifespan 0 => permanent
	L->AttachTo(Host);
	TestEqual(TEXT("attach registered the leech on the host"), CountLeeches(Host), 1);
	WoTTest::TickWorld(World, 0.55f);
	TestTrue(FString::Printf(TEXT("AffectHost ran ~once per 0.1s over 0.55s (got %d)"),
		L->TotalIterations), L->TotalIterations >= 5 && L->TotalIterations <= 6);
	TestTrue(TEXT("AffectHost calls coalesced (<= total iterations)"),
		L->AffectHostCalls >= 1 && L->AffectHostCalls <= L->TotalIterations);

	// 2) bSingular refuses a second leech of the same class.
	UWoTTestLeech* Dup = NewObject<UWoTTestLeech>(Host);
	Dup->bSingular = true;
	Dup->AttachTo(Host);
	TestEqual(TEXT("bSingular refused (host already carries the class)"),
		CountLeeches(Host), 1);

	// 3) bRemoveExisting replaces the existing leech of the same class.
	UWoTTestLeech* Replacer = NewObject<UWoTTestLeech>(Host);
	Replacer->bRemoveExisting = true;
	Replacer->AttachTo(Host);
	TArray<UWoTLeech*> AfterReplace;
	Host->GetComponents<UWoTLeech>(AfterReplace);
	TestEqual(TEXT("bRemoveExisting leaves exactly one leech"), AfterReplace.Num(), 1);
	TestTrue(TEXT("the survivor is the replacement"),
		AfterReplace.Num() == 1 && AfterReplace[0] == Replacer);

	// 4) AffectResolutionScale > 1 decelerates the cadence.
	AWoTCharacter* Host2 = World->SpawnActor<AWoTCharacter>(
		FVector(300, 0, 100), FRotator::ZeroRotator);
	Host2->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	UWoTTestLeech* Scaled = NewObject<UWoTTestLeech>(Host2);
	Scaled->AffectResolution = 0.1f;
	Scaled->AffectResolutionScale = 2.0f;
	Scaled->AttachTo(Host2);
	WoTTest::TickWorld(World, 0.35f);
	TestTrue(TEXT("scale>1 grew AffectResolution"), Scaled->AffectResolution > 0.1f);
	TestTrue(FString::Printf(TEXT("scale>1 fired fewer times than unscaled (got %d)"),
		Scaled->TotalIterations), Scaled->TotalIterations < 3);

	// 5) Lifespan expiry detaches the leech (Expired -> Unattach).
	AWoTCharacter* Host3 = World->SpawnActor<AWoTCharacter>(
		FVector(600, 0, 100), FRotator::ZeroRotator);
	Host3->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	UWoTTestLeech* Mortal = NewObject<UWoTTestLeech>(Host3);
	Mortal->Lifespan = 0.3f;
	Mortal->AttachTo(Host3);
	TestEqual(TEXT("mortal leech attached"), CountLeeches(Host3), 1);
	WoTTest::TickWorld(World, 0.4f);
	TestEqual(TEXT("leech detached when its lifespan expired"), CountLeeches(Host3), 0);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSpawnPoolTest,
	"WoT.Inventory.SpawnPool.TakeAndRespawn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTSpawnPoolTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	UWoTInventoryComponent* Inv = Pawn->InventoryComponent;

	// a pool that spawns a Fireball, respawning 0.2s after it's taken.
	// Deferred spawn so Pool/Weight/RespawnTime are set before BeginPlay runs.
	const FTransform Xform(FRotator::ZeroRotator, FVector(500, 0, 100));
	AWoTAngrealSpawnPool* Pool = World->SpawnActorDeferred<AWoTAngrealSpawnPool>(
		AWoTAngrealSpawnPool::StaticClass(), Xform);
	Pool->Pool = { AWoTAngrealInvFireball::StaticClass() };
	Pool->Weight = { 1 };
	Pool->RespawnTime = 0.2f;
	Pool->bRespawn = true;
	Pool->FinishSpawning(Xform);
	WoTTest::TickWorld(World, 0.1f);

	AWoTAngrealInvFireball* Spawned = Cast<AWoTAngrealInvFireball>(Pool->PickupItem.Get());
	TestNotNull(TEXT("pool spawned a fireball pickup"), Spawned);
	TestNull(TEXT("nothing in the pawn's inventory yet"), Inv->SelectedItem.Get());

	// walk onto the (hidden) pool -> Touch handoff -> fireball into inventory
	Pawn->SetActorLocation(Pool->GetActorLocation());
	WoTTest::TickWorld(World, 0.1f);

	AWoTAngrealInventory* Held = Cast<AWoTAngrealInventory>(Inv->SelectedItem.Get());
	TestNotNull(TEXT("touching the pool handed the fireball to the pawn"), Held);
	TestTrue(TEXT("the held item is the pool's spawned fireball"), Held == Spawned);
	TestNull(TEXT("pool cleared its PickupItem after handoff"), Pool->PickupItem.Get());

	// step off and wait out the respawn -> a fresh pickup appears
	Pawn->SetActorLocation(FVector(0, 0, 100));
	WoTTest::TickWorld(World, 0.6f);
	TestNotNull(TEXT("pool respawned a new pickup after RespawnTime"),
		Pool->PickupItem.Get());
	TestTrue(TEXT("the respawn is a fresh actor, not the held one"),
		Pool->PickupItem.Get() != Held);

	// Touch the respawned pool while ALREADY holding its class: the pool must route
	// the handoff through the item's pickup query, so charges are stolen into the held
	// item instead of a second fireball being granted (AngrealSpawnPool.uc:132 ->
	// AngrealInventory.uc:528). Regression guard for the direct-GiveTo bypass.
	AWoTAngrealInvFireball* Respawned = Cast<AWoTAngrealInvFireball>(Pool->PickupItem.Get());
	if (Respawned)
	{
		Held->CurCharges = 5;
		Respawned->CurCharges = 7;
		Pawn->SetActorLocation(Pool->GetActorLocation());
		WoTTest::TickWorld(World, 0.1f);
		TestEqual(TEXT("inventory still holds exactly one item after a duplicate pool touch"),
			Inv->Items.Num(), 1);
		TestEqual(TEXT("the duplicate pool pickup's charges were stolen into the held item"),
			Held->CurCharges, 12);
		TestNull(TEXT("pool consumed its pickup on the charge-steal touch"),
			Pool->PickupItem.Get());
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTDefaultInventoryTest,
	"WoT.Inventory.DefaultInventory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTDefaultInventoryTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	UWoTInventoryComponent* Inv = Pawn->InventoryComponent;

	AWoTGameMode* Game = World->GetAuthGameMode<AWoTGameMode>();
	TestNotNull(TEXT("test world has the WoT game mode"), Game);
	if (!Game)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	// giMPArena.AddDefaultInventory: a fresh player starts with 25 charges of Dart.
	Game->GivePawnAngreal(Pawn, AWoTAngrealInvDart::StaticClass(), 25);
	AWoTAngrealInvDart* Dart = Cast<AWoTAngrealInvDart>(
		Inv->FindInventoryType(AWoTAngrealInvDart::StaticClass()));
	TestNotNull(TEXT("player was given the default Dart"), Dart);
	if (Dart)
	{
		TestEqual(TEXT("Dart starts with the requested 25 charges"), Dart->CurCharges, 25);
	}
	TestEqual(TEXT("exactly one item held"), Inv->Items.Num(), 1);

	// giMP.GivePawnAngreal skips a class the pawn already carries — no duplicate.
	Game->GivePawnAngreal(Pawn, AWoTAngrealInvDart::StaticClass(), 25);
	TestEqual(TEXT("giving a duplicate default is a no-op"), Inv->Items.Num(), 1);

	// giWOT.AddDefaultInventory: SetPlayerDefaults grants the pawn's
	// DefaultAngrealInventory — Air Pulse, effectively-infinite at 999 charges
	// from ITS class seed (AngrealInvAirBurst.uc:372-373), never a grant-site
	// constant. Second call adds nothing (existing-item check).
	Game->SetPlayerDefaults(Pawn);
	AWoTAngrealInvAirBurst* Air = Cast<AWoTAngrealInvAirBurst>(
		Inv->FindInventoryType(AWoTAngrealInvAirBurst::StaticClass()));
	TestNotNull(TEXT("player was given the faction default Air Pulse"), Air);
	if (Air)
	{
		TestEqual(TEXT("Air Pulse seeds 999 charges from its own defaults"),
			Air->CurCharges, 999);
	}
	TestEqual(TEXT("loadout is Dart + Air Pulse"), Inv->Items.Num(), 2);
	Game->SetPlayerDefaults(Pawn);
	TestEqual(TEXT("re-running defaults adds nothing"), Inv->Items.Num(), 2);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTDeathRespawnTest,
	"WoT.Rules.DeathRespawn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTDeathRespawnTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	AWoTGameMode* Game = World->GetAuthGameMode<AWoTGameMode>();
	TestNotNull(TEXT("test world has the WoT game mode"), Game);
	if (!Game)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Game->RespawnDelay = 0.2f;   // keep the test brisk

	// A place to respawn.
	World->SpawnActor<APlayerStart>(FVector(500, 500, 100), FRotator::ZeroRotator);

	// A possessed player + its killer.
	AWoTPlayerController* PC = World->SpawnActor<AWoTPlayerController>();
	AWoTCharacter* Victim = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	PC->Possess(Victim);
	AWoTCharacter* Killer = World->SpawnActor<AWoTCharacter>(
		FVector(200, 0, 100), FRotator::ZeroRotator);
	TestTrue(TEXT("the player possesses the victim pawn"), PC->GetPawn() == Victim);

	// Lethal hit -> Died -> PlayerKilled -> UnPossess + Dying fire gate.
	FWoTDamageEvent Ev;
	Ev.WoTDamageType = TEXT("Fire");
	Ev.HitInfo.ImpactPoint = Victim->GetActorLocation();
	Victim->TakeDamage(1000.0f, Ev, Killer->GetController(), Killer);
	TestTrue(TEXT("the victim is dead"), Victim->bDead);
	TestNull(TEXT("the controller left the corpse on death"), PC->GetPawn());
	TestEqual(TEXT("the controller entered the Dying FSM state"),
		static_cast<int32>(PC->GetPlayerStateMachineState()),
		static_cast<int32>(EWoTPlayerStateMachineState::Dying));

	// Fire before OKToFireTime is ignored. Crossing the delay does not auto-respawn;
	// the first Fire after it is what asks the authority for a fresh pawn.
	TWeakObjectPtr<AWoTCharacter> Corpse = Victim;
	PC->Fire();
	WoTTest::TickWorld(World, 0.1f);
	TestNull(TEXT("an early fire does not respawn the player"), PC->GetPawn());
	WoTTest::TickWorld(World, 0.2f);
	TestNull(TEXT("the player stays dead after the delay without another fire"), PC->GetPawn());
	PC->Fire();
	WoTTest::TickWorld(World, 0.02f);
	APawn* Reborn = PC->GetPawn();
	TestNotNull(TEXT("the player respawned into a new pawn"), Reborn);
	TestTrue(TEXT("the respawn is a fresh pawn, not the corpse"),
		Reborn != nullptr && Reborn != Victim);
	TestTrue(TEXT("the reborn pawn is alive"),
		Cast<AWoTCharacter>(Reborn) && !Cast<AWoTCharacter>(Reborn)->bDead);

	// Retail reuses ONE PlayerPawn (Engine GameInfo.RestartPlayer) so corpses never
	// accumulate; the port destroys the old body at respawn — a leaked corpse
	// would poison ChoosePlayerStart's occupied/LOS scoring forever.
	TestFalse(TEXT("the corpse was destroyed at respawn"), Corpse.IsValid());
	int32 LivePawns = 0;
	for (TActorIterator<AWoTCharacter> It(World); It; ++It)
	{
		if (IsValid(*It)) { ++LivePawns; }
	}
	TestEqual(TEXT("exactly reborn + killer remain"), LivePawns, 2);

	// --- Retail Dying also respawns on the JUMP key: Engine.PlayerPawn's
	// Dying.PlayerMove routes bPressedJump into Fire(0) (and Dying.ServerMove fires
	// on the jump-status edge; WOTPlayer keeps both) — the same OKToFireTime gate as
	// Fire. Drive the REAL input stack (the WoT.Input.FirePressReleaseThroughStack
	// pattern) so the controller-owned Jump binding is what is exercised, and assert
	// a LIVE pawn still jumps through the forwarding (no regression).
	AWoTCharacter* RebornChar = Cast<AWoTCharacter>(Reborn);
	if (!TestNotNull(TEXT("the reborn pawn is a WoT character"), RebornChar))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	PC->PlayerInput = NewObject<UPlayerInput>(PC);
	PC->SetupInputComponent();
	UInputComponent* PawnInput = NewObject<UInputComponent>(RebornChar);
	RebornChar->SetupPlayerInputComponent(PawnInput);
	auto SendJumpEdge = [&PC](UInputComponent* PawnInputComponent, EInputEvent Edge)
	{
		PC->PlayerInput->InputKey(FInputKeyEventArgs::CreateSimulated(
			EKeys::SpaceBar, Edge, Edge == IE_Pressed ? 1.0f : 0.0f));
		// APlayerController::BuildInputStack order: pawn first, controller last
		// (top of stack); no pawn component while dead.
		TArray<UInputComponent*> Stack;
		if (PawnInputComponent)
		{
			Stack.Push(PawnInputComponent);
		}
		Stack.Push(PC->InputComponent);
		PC->PlayerInput->ProcessInputStack(Stack, 1.0f / 60.0f, false);
	};

	SendJumpEdge(PawnInput, IE_Pressed);
	TestTrue(TEXT("a live jump press reaches the pawn through the stack"),
		RebornChar->bPressedJump);
	SendJumpEdge(PawnInput, IE_Released);
	TestFalse(TEXT("the jump release reaches the pawn too"),
		RebornChar->bPressedJump);

	// Kill the reborn pawn: a jump inside the respawn-delay gate is ignored; the
	// first one after it asks the authority for a fresh pawn, exactly like Fire.
	RebornChar->TakeDamage(1000.0f, Ev, Killer->GetController(), Killer);
	TestNull(TEXT("the second death leaves the controller pawnless"), PC->GetPawn());
	SendJumpEdge(nullptr, IE_Pressed);
	SendJumpEdge(nullptr, IE_Released);
	WoTTest::TickWorld(World, 0.1f);
	TestNull(TEXT("an early jump does not respawn the player"), PC->GetPawn());
	WoTTest::TickWorld(World, 0.2f);
	SendJumpEdge(nullptr, IE_Pressed);
	SendJumpEdge(nullptr, IE_Released);
	WoTTest::TickWorld(World, 0.02f);
	TestTrue(TEXT("a dead player's jump respawns through the fire gate"),
		PC->GetPawn() != nullptr);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTScoreKillTest,
	"WoT.Rules.ScoreKill",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTScoreKillTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	World->SpawnActor<APlayerStart>(FVector(500, 500, 100), FRotator::ZeroRotator);
	AWoTGameMode* Game = World->GetAuthGameMode<AWoTGameMode>();

	auto SpawnPlayer = [&](const FVector& Loc) -> APlayerController*
	{
		APlayerController* PC = World->SpawnActor<APlayerController>();
		PC->InitPlayerState();   // create the AWoTPlayerState (login flow shortcut)
		if (Game) { Game->ChangeTeam(PC, 255); }   // login assigns a unique free-for-all team
		AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(Loc, FRotator::ZeroRotator);
		PC->Possess(Pawn);
		return PC;
	};

	APlayerController* KillerPC = SpawnPlayer(FVector(200, 0, 100));
	APlayerController* VictimPC = SpawnPlayer(FVector(0, 0, 100));
	AWoTPlayerState* KillerPS = KillerPC->GetPlayerState<AWoTPlayerState>();
	AWoTPlayerState* VictimPS = VictimPC->GetPlayerState<AWoTPlayerState>();
	TestNotNull(TEXT("killer has a WoT player state"), KillerPS);
	TestNotNull(TEXT("victim has a WoT player state"), VictimPS);
	if (!KillerPS || !VictimPS)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	// A frag: killer's pawn lands the lethal blow.
	FWoTDamageEvent Ev;
	Ev.WoTDamageType = TEXT("Fire");
	VictimPC->GetPawn()->TakeDamage(1000.0f, Ev, KillerPC, KillerPC->GetPawn());
	TestEqual(TEXT("killer scored a frag"), KillerPS->Kills, 1);
	TestEqual(TEXT("victim took a death"), VictimPS->Deaths, 1);
	TestEqual(TEXT("a real frag is not a suicide"), VictimPS->Suicides, 0);
	TestEqual(TEXT("CalcScore = Kills - Suicides"), KillerPS->CalcScore(), 1);

	// An environment death (no killer) scores the victim a suicide, not a frag.
	AWoTCharacter* Loner = World->SpawnActor<AWoTCharacter>(
		FVector(-200, 0, 100), FRotator::ZeroRotator);
	APlayerController* LonerPC = World->SpawnActor<APlayerController>();
	LonerPC->InitPlayerState();
	if (Game) { Game->ChangeTeam(LonerPC, 255); }
	LonerPC->Possess(Loner);
	AWoTPlayerState* LonerPS = LonerPC->GetPlayerState<AWoTPlayerState>();
	Loner->TakeDamage(1000.0f, Ev, nullptr, nullptr);
	TestEqual(TEXT("an environment death is a suicide"), LonerPS->Suicides, 1);
	TestEqual(TEXT("a suicide is not a frag"), LonerPS->Kills, 0);
	TestEqual(TEXT("a suicide drags CalcScore negative"), LonerPS->CalcScore(), -1);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFragLimitTest,
	"WoT.Rules.FragLimit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTFragLimitTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	World->SpawnActor<APlayerStart>(FVector(500, 500, 100), FRotator::ZeroRotator);

	AWoTGameMode* Game = World->GetAuthGameMode<AWoTGameMode>();
	if (!Game)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Game->FragLimit = 3;
	Game->RespawnDelay = 0.1f;

	APlayerController* KillerPC = World->SpawnActor<APlayerController>();
	KillerPC->InitPlayerState();
	Game->ChangeTeam(KillerPC, 255);   // login assigns unique free-for-all teams (enemies)
	KillerPC->Possess(World->SpawnActor<AWoTCharacter>(FVector(200, 0, 100), FRotator::ZeroRotator));
	APlayerController* VictimPC = World->SpawnActor<APlayerController>();
	VictimPC->InitPlayerState();
	Game->ChangeTeam(VictimPC, 255);
	VictimPC->Possess(World->SpawnActor<AWoTCharacter>(FVector(0, 0, 100), FRotator::ZeroRotator));

	AWoTPlayerState* KillerPS = KillerPC->GetPlayerState<AWoTPlayerState>();
	// Already at the brink: one frag short of the limit.
	KillerPS->Kills = 2;

	FWoTDamageEvent Ev;
	Ev.WoTDamageType = TEXT("Fire");
	VictimPC->GetPawn()->TakeDamage(1000.0f, Ev, KillerPC, KillerPC->GetPawn());

	TestEqual(TEXT("the killer reached the frag limit"), KillerPS->Kills, 3);
	TestTrue(TEXT("hitting FragLimit ends the match"), Game->bGameEnded);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInitGameOptionsTest,
	"WoT.Rules.InitGameOptions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTInitGameOptionsTest::RunTest(const FString&)
{
	// giCombatBase.InitGame (giCombatBase.uc:20-27): the host URL overrides the match
	// rules — ?FragLimit= / ?TimeLimit= / ?Cycle= (bChangeLevels) — and absent options
	// keep the class defaults (GetIntOption's fallback semantics).
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTGameMode* Game = World->GetAuthGameMode<AWoTGameMode>();
	if (!Game)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	TestEqual(TEXT("GameInfo shipped Difficulty default is 1"), Game->Difficulty, 1);

	FString Err;
	Game->InitGame(TEXT("Arena_01"),
		TEXT("?Difficulty=3?FragLimit=2?TimeLimit=5?Cycle=0"), Err);
	TestEqual(TEXT("?Difficulty=3 overrides the shipped default"), Game->Difficulty, 3);
	TestEqual(TEXT("?FragLimit=2 overrides the default"), Game->FragLimit, 2);
	TestEqual(TEXT("?TimeLimit=5 overrides the default"), Game->TimeLimit, 5);
	TestFalse(TEXT("?Cycle=0 clears bChangeLevels"), Game->bChangeLevels);

	Game->InitGame(TEXT("Arena_01"), TEXT("?Cycle=1"), Err);
	TestEqual(TEXT("absent Difficulty keeps the prior value"), Game->Difficulty, 3);
	TestEqual(TEXT("absent FragLimit keeps the prior value"), Game->FragLimit, 2);
	TestEqual(TEXT("absent TimeLimit keeps the prior value"), Game->TimeLimit, 5);
	TestTrue(TEXT("?Cycle=1 sets bChangeLevels"), Game->bChangeLevels);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAirBurstCastFXTest,
	"WoT.Weaves.AirBurstCastFX",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTAirBurstCastFXTest::RunTest(const FString&)
{
	// AirBurst's cast presentation must be
	// bound. Registry first: both sprayer bundles exist with the shipped shapes
	// (AirBlastProj.uc: 1 APulseA ripple template; AirBlastBall.uc: 3 flare
	// templates).
	const FWoTSprayerBundleRow* Ripple = WoTWeaveFX::Find(TEXT("AirBlastProj"));
	const FWoTSprayerBundleRow* Ball = WoTWeaveFX::Find(TEXT("AirBlastBall"));
	TestNotNull(TEXT("AirBlastProj bundle registered"), Ripple);
	TestNotNull(TEXT("AirBlastBall bundle registered"), Ball);
	if (Ripple)
	{
		TestEqual(TEXT("ripple has the single APulseA template"),
			Ripple->Templates.Num(), 1);
	}
	if (Ball)
	{
		TestEqual(TEXT("ball has the three flare templates"), Ball->Templates.Num(), 3);
	}

	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	AWoTGameMode* Game = World->GetAuthGameMode<AWoTGameMode>();
	if (!Game || !Pawn)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Game->GivePawnAngreal(Pawn, AWoTAngrealInvAirBurst::StaticClass());
	AWoTAngrealInvAirBurst* Air = Cast<AWoTAngrealInvAirBurst>(
		Pawn->InventoryComponent->FindInventoryType(
			AWoTAngrealInvAirBurst::StaticClass()));
	if (!TestNotNull(TEXT("pawn holds the Air Pulse"), Air))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	TestFalse(TEXT("ActivateAB cast sound is bound (AngrealInvAirBurst.uc:376)"),
		Air->ActivateSound.IsNull());
	AWoTBreakableDecoration* Breakable =
		World->SpawnActor<AWoTBreakableDecoration>(FVector(45, 0, 142),
			FRotator::ZeroRotator);
	Breakable->Health = 100;
	AWoTCarcass* Carcass = World->SpawnActor<AWoTCarcass>(
		FVector(70, 0, 142), FRotator::ZeroRotator);
	AWoTWallSlab* Wall = World->SpawnActor<AWoTWallSlab>(
		FVector(85, 0, 142), FRotator::ZeroRotator);
	AWoTExplodingMover* Exploding = World->SpawnActor<AWoTExplodingMover>(
		FVector(95, 0, 142), FRotator::ZeroRotator);
	Exploding->Health = 100;
	FActorSpawnParameters LegionSpawn;
	LegionSpawn.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTLegionProjectile* Legion = World->SpawnActor<AWoTLegionProjectile>(
		AWoTLegionProjectile::StaticClass(), FVector(110, 0, 142),
		FRotator::ZeroRotator, LegionSpawn);
	// A launched Legion spirit normally acquires movement on the first world tick;
	// hold this production collision cylinder in the authored beam for the cast.
	if (TestNotNull(TEXT("production Legion spirit spawns"), Legion))
	{
		Legion->ProjectileMovement->StopMovementImmediately();
		Legion->ProjectileMovement->Deactivate();
	}
	const int32 WallHealth = Wall->WithstandDamage;

	// Exercise the exact production AffectedTypes selector with production CDO
	// identities. A bare ExplodingMover has no authored brush geometry (its mesh is
	// map data), so selection—not a synthetic collision shape—is the faithful unit
	// assertion here; collision/damage is exercised by colliding production families
	// below.
	auto Selected = [Air](UClass* Class)
	{
		return Air->IsAffectable(Class ? Class->GetDefaultObject<AActor>() : nullptr);
	};
	TestTrue(TEXT("Air Burst selects the production ExplodingMover family"),
		Selected(AWoTExplodingMover::StaticClass()));
	TestTrue(TEXT("Air Burst selects the production ExpWard family"),
		Selected(AWoTExpWardProjectile::StaticClass()));
	TestTrue(TEXT("Air Burst selects the production Legion spirit family"),
		Selected(AWoTLegionProjectile::StaticClass()));
	TestTrue(TEXT("Air Burst selects the production PortcullisMover family"),
		Selected(AWoTPortcullisMover::StaticClass()));
	TestTrue(TEXT("Air Burst selects the production WallSlab family"),
		Selected(AWoTWallSlab::StaticClass()));
	TestFalse(TEXT("Air Burst excludes the production MashadarGuide seeker"),
		Selected(AWoTMashadarGuide::StaticClass()));
	TestTrue(TEXT("Air Burst selects the production MashadarTrailer segment"),
		Selected(AWoTMashadarSegment::StaticClass()));

	// Cast: the replicated AWoTAirBlastFX marker appears at Owner.Location +
	// EffectOffset (unrotated, :121), flying vector(ViewRotation)*350. One tick
	// first: the refire gate is TimeSeconds > NextCastTime (both 0.0 in a fresh
	// test world — the shipped Level.TimeSeconds > NextCastTime edge).
	WoTTest::TickWorld(World, 1.0f / 60.0f);
	Air->CastArtifact();
	TestTrue(TEXT("Air Burst damages a production breakable decoration"),
		Breakable->LastDamageTaken > 0.0f);
	TestTrue(TEXT("Air Burst pushes the breakable through its falling path"),
		!Breakable->GetFallVelocity().IsNearlyZero());
	TestTrue(TEXT("Air Burst pushes the production Carcass type"),
		!Carcass->GetVelocityForTest().IsNearlyZero());
	TestTrue(TEXT("Air Burst damages the production WallSlab type"),
		Wall->WithstandDamage < WallHealth);
	TestEqual(TEXT("a bare mover without authored brush data is not given synthetic collision"),
		Exploding->Health, 100);
	if (Legion)
	{
		TestTrue(TEXT("Air Burst damages the production Legion spirit type"),
			Legion->Health < 30 || Legion->IsVeryDead());
	}
	AWoTAirBlastFX* FX = nullptr;
	int32 Count = 0;
	for (TActorIterator<AWoTAirBlastFX> It(World); It; ++It)
	{
		FX = *It;
		++Count;
	}
	TestEqual(TEXT("one blast FX marker spawned"), Count, 1);
	if (FX)
	{
		TestEqual(TEXT("the blast is relative to the caster"),
			FX->RelativeActor.Get(), Cast<AActor>(Pawn));
		TestTrue(TEXT("blast velocity is the 350 uu/s view vector"),
			FVector(FX->BlastVelocity).Equals(
				Pawn->GetViewRotation().Vector() * 350.0f, 1.0f));
		TestTrue(TEXT("blast starts at the caster + EffectOffset"),
			FX->GetActorLocation().Equals(
				Pawn->GetActorLocation() + FVector(0, 0, 20), 1.0f));
	}

	// The marker is a one-shot: emit + fade, then the 2 s lifespan reaps it.
	// WoTTest::TickWorld, not a raw World->Tick loop — its GFrameCounter++ is what lets
	// the timer manager advance across steps (the Ward auto-detonation pattern).
	WoTTest::TickWorld(World, 2.5f);
	Count = 0;
	for (TActorIterator<AWoTAirBlastFX> It(World); It; ++It)
	{
		++Count;
	}
	TestEqual(TEXT("the blast FX marker reaped itself"), Count, 0);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAirBurstCasterFollowSpec,
	"WoT.Weaves.AirBurstCasterFollow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTAirBurstCasterFollowSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AWoTCharacter* Caster = World->SpawnActor<AWoTCharacter>(
		FVector(100.0f, 200.0f, 300.0f), FRotator::ZeroRotator, SpawnParams);
	Caster->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	const FVector InitialOffset(10.0f, -20.0f, 30.0f);
	const FVector FollowVelocity(0.0f, 350.0f, 0.0f);
	const FTransform FollowTransform(
		FRotator::ZeroRotator, Caster->GetActorLocation() + InitialOffset);
	AWoTAirBlastFX* Following = World->SpawnActorDeferred<AWoTAirBlastFX>(
		AWoTAirBlastFX::StaticClass(), FollowTransform);
	Following->RelativeActor = Caster;
	Following->BlastVelocity = FollowVelocity;
	Following->FinishSpawning(FollowTransform);

	Following->Tick(0.1f);
	TestTrue(TEXT("resolved caster keeps the initial offset plus Velocity*dt"),
		Following->GetActorLocation().Equals(
			Caster->GetActorLocation() + InitialOffset + FollowVelocity * 0.1f,
			KINDA_SMALL_NUMBER));

	const FVector MovedCasterLocation(175.0f, 150.0f, 325.0f);
	Caster->SetActorLocation(MovedCasterLocation);
	Following->Tick(0.2f);
	TestTrue(TEXT("moving caster translation is inherited between ticks"),
		Following->GetActorLocation().Equals(
			MovedCasterLocation + InitialOffset + FollowVelocity * 0.3f,
			KINDA_SMALL_NUMBER));

	const FVector WorldStart(1000.0f, 2000.0f, 3000.0f);
	const FVector WorldVelocity(350.0f, 0.0f, 0.0f);
	const FTransform WorldTransform(FRotator::ZeroRotator, WorldStart);
	AWoTAirBlastFX* Unresolved = World->SpawnActorDeferred<AWoTAirBlastFX>(
		AWoTAirBlastFX::StaticClass(), WorldTransform);
	Unresolved->RelativeActor = nullptr;
	Unresolved->BlastVelocity = WorldVelocity;
	Unresolved->FinishSpawning(WorldTransform);
	Unresolved->Tick(0.25f);
	TestTrue(TEXT("unresolved caster falls back to plain world motion"),
		Unresolved->GetActorLocation().Equals(
			WorldStart + WorldVelocity * 0.25f, KINDA_SMALL_NUMBER));

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRC33SplashIntegerDamageSpec,
	"WoT.RC33.G13.SplashIntegerDamage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTRC33SplashIntegerDamageSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTFireballProjectile* Projectile = World->SpawnActor<AWoTFireballProjectile>(
		FVector::ZeroVector, FRotator::ZeroRotator, Params);
	Projectile->Damage = 10.0f;
	Projectile->DamageRadius = 100.0f;
	AWoTBreakableDecoration* OneDamage =
		World->SpawnActor<AWoTBreakableDecoration>(
			FVector(85, 0, 0), FRotator::ZeroRotator, Params);
	AWoTBreakableDecoration* BelowOne =
		World->SpawnActor<AWoTBreakableDecoration>(
			FVector(91, 0, 0), FRotator::ZeroRotator, Params);
	OneDamage->Health = BelowOne->Health = 100;
	Projectile->ProcessSplashDamage();
	TestEqual(TEXT("1.5 splash truncates to one at the damage boundary"),
		OneDamage->LastDamageTaken, 1.0f);
	TestEqual(TEXT("0.9 splash truncates to zero and skips the target"),
		BelowOne->LastDamageTaken, 0.0f);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTServerChangeTeamTest,
	"WoT.Rules.ServerChangeTeam",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTServerChangeTeamTest::RunTest(const FString&)
{
	// WOTPlayer.ServerChangeTeam (a client-initiated team switch) routes to the authority game
	// mode's ChangeTeam. Two players driving the RPC in the FFA base each land on a DISTINCT team
	// (giCombatBase.ChangeTeam assigns a unique free-for-all team) — proving the RPC forwarded.
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTGameMode* Game = World->GetAuthGameMode<AWoTGameMode>();
	if (!Game) { WoTTest::DestroySpecWorld(World); return false; }

	AWoTPlayerController* P1 = World->SpawnActor<AWoTPlayerController>();
	AWoTPlayerController* P2 = World->SpawnActor<AWoTPlayerController>();
	P1->InitPlayerState();
	P2->InitPlayerState();
	// Drive the assignment through the RPC (its _Implementation runs directly on the authority).
	P1->ServerChangeTeam_Implementation(255);
	P2->ServerChangeTeam_Implementation(255);

	AWoTPlayerState* PS1 = P1->GetPlayerState<AWoTPlayerState>();
	AWoTPlayerState* PS2 = P2->GetPlayerState<AWoTPlayerState>();
	if (TestNotNull(TEXT("P1 player state"), PS1) && TestNotNull(TEXT("P2 player state"), PS2))
	{
		TestTrue(TEXT("ServerChangeTeam routed to ChangeTeam (FFA assigned distinct teams)"),
			PS1->Team != PS2->Team);
	}
	TestTrue(TEXT("ServerChangeTeam validates"), P1->ServerChangeTeam_Validate(0));

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTTimeLimitTest,
	"WoT.Rules.TimeLimit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTTimeLimitTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTGameMode* Game = World->GetAuthGameMode<AWoTGameMode>();
	AWoTGameState* GS = World->GetGameState<AWoTGameState>();
	TestNotNull(TEXT("test world has the WoT game mode"), Game);
	TestNotNull(TEXT("test world has the WoT game state"), GS);
	if (!Game || !GS)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	// A spawned mode's TimeLimit is fixed at construction, so choose one and re-seed the
	// clock (exactly what giCombatBase.PostBeginPlay does at match start).
	Game->TimeLimit = 5;   // 5-minute match
	Game->SeedMatchClock();

	// giCombatBase.PostBeginPlay: RemainingTime = 60*TimeLimit; display the remaining clock.
	TestEqual(TEXT("clock seeded to 60*TimeLimit"), GS->RemainingTime, 300.0f);
	TestTrue(TEXT("a TimeLimit match displays the remaining clock"), GS->bDisplayRemaining);

	// GameReplicationInfo.Tick counts real seconds down.
	WoTTest::TickWorld(World, 0.5f);
	TestTrue(TEXT("the clock ticks down"),
		GS->RemainingTime < 300.0f && GS->RemainingTime > 0.0f);

	// bStopCountDown freezes both counters (pre-match / intermission).
	GS->bStopCountDown = true;
	const float Frozen = GS->RemainingTime;
	WoTTest::TickWorld(World, 0.5f);
	TestEqual(TEXT("bStopCountDown halts the clock"), GS->RemainingTime, Frozen);
	GS->bStopCountDown = false;

	// Timer at 0:00 ends the match (giCombatBase.Timer -> EndGame "timelimit").
	GS->RemainingTime = 0.0f;
	Game->Timer();
	TestTrue(TEXT("the clock running out ends the match"), Game->bGameEnded);
	TestEqual(TEXT("EndGame drives RemainingTime to -1"), GS->RemainingTime, -1.0f);

	// The match then lingers for GameRestartDelay (15s) before rotating — a poll inside
	// that window does not restart yet.
	Game->SetInstalledMapsForTesting({ TEXT("Arena_02"), TEXT("Arena_03") });
	GS->RemainingTime = -5.0f;   // still inside the restart-delay window
	Game->Timer();
	TestTrue(TEXT("no rotation inside the restart-delay window"), Game->PendingMapURL.IsEmpty());

	// Past the delay, Timer restarts the match -> rotate to the next map.
	GS->RemainingTime = -20.0f;
	Game->Timer();
	TestEqual(TEXT("timelimit expiry rotates to the next map"),
		Game->Map, FString(TEXT("Arena_02")));
	TestEqual(TEXT("the rotation travels to the map package (dev build)"),
		Game->PendingMapURL, FString(TEXT("/Game/WoT/Maps/Arena_02")));

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMapRotationTest,
	"WoT.Rules.MapRotation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTMapRotationTest::RunTest(const FString&)
{
	// UE1's native Actor.GetMapName over the shipped Maps dir.
	const TArray<FString> Installed = {
		TEXT("Battle2_01"), TEXT("Arena_12"), TEXT("Arena_02"), TEXT("Arena_01"),
		TEXT("Arena_11"), TEXT("Arena_03"), TEXT("Battle3_01"), TEXT("Battle2_02"),
		TEXT("Battle4_01"), TEXT("Entry"), TEXT("ARENA_02") };
	TestEqual(TEXT("next map in directory order"),
		AWoTGameMode::GetMapName(TEXT("Arena_"), TEXT("Arena_02"), 1, Installed),
		FString(TEXT("Arena_03")));
	TestEqual(TEXT("Arena_03 skips the missing Arena_10 on the way to 11"),
		AWoTGameMode::GetMapName(TEXT("Arena_"), TEXT("Arena_03"), 1, Installed),
		FString(TEXT("Arena_11")));
	TestEqual(TEXT("the last map wraps to the first"),
		AWoTGameMode::GetMapName(TEXT("Arena_"), TEXT("Arena_12"), 1, Installed),
		FString(TEXT("Arena_01")));
	TestEqual(TEXT("PrevMap from the first wraps to the last"),
		AWoTGameMode::GetMapName(TEXT("Arena_"), TEXT("Arena_01"), -1, Installed),
		FString(TEXT("Arena_12")));
	TestEqual(TEXT("SetMap (Dir 0) keeps a listed map"),
		AWoTGameMode::GetMapName(TEXT("Arena_"), TEXT("Arena_11"), 0, Installed),
		FString(TEXT("Arena_11")));
	// appStrcmp is case-sensitive, and an unlisted map yields the first listed one:
	// exactly why a relaunched Battle3/4 server (MapPrefix is not config, so it is
	// giMPBattle's "Battle2_") rotates to Battle2_01.
	TestEqual(TEXT("a case-variant name is not the listed map"),
		AWoTGameMode::GetMapName(TEXT("Arena_"), TEXT("arena_02"), 1, Installed),
		FString(TEXT("Arena_01")));
	TestEqual(TEXT("Battle3_01 with giMPBattle's prefix rotates to Battle2_01"),
		AWoTGameMode::GetMapName(TEXT("Battle2_"), TEXT("Battle3_01"), 1, Installed),
		FString(TEXT("Battle2_01")));
	TestEqual(TEXT("the prefix scan is case-insensitive and de-duplicated"),
		AWoTGameMode::GetMapName(TEXT("arena_"), TEXT("Arena_01"), 1, Installed),
		FString(TEXT("Arena_02")));
	TestEqual(TEXT("no installed map yields empty"),
		AWoTGameMode::GetMapName(TEXT("Arena_"), TEXT("Arena_02"), 1, {}), FString());

	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTGameMode* Game = World->GetAuthGameMode<AWoTGameMode>();
	if (!Game)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	TestEqual(TEXT("giMPArena MapPrefix"), Game->MapPrefix, FString(TEXT("Arena_")));
	TestTrue(TEXT("giMPArena bChangeLevels (Default.ini:452)"), Game->bChangeLevels);
	TestFalse(TEXT("BeginPlay seeded Map from the running world"), Game->Map.IsEmpty());
	Game->SetInstalledMapsForTesting(Installed);

	// A host of Arena_02 (the retail menu saved Map=Arena_02 before launching it).
	Game->Map = TEXT("Arena_02");
	Game->NextMap();
	TestEqual(TEXT("NextMap -> Arena_03"), Game->Map, FString(TEXT("Arena_03")));
	TestEqual(TEXT("dev travel goes to the map package"),
		Game->PendingMapURL, FString(TEXT("/Game/WoT/Maps/Arena_03")));

	// giCombatBase.RestartGame rotates exactly once per decided match (the
	// bAlreadyChanged latch).
	Game->Map = TEXT("Arena_02");
	Game->RestartGame();
	TestEqual(TEXT("RestartGame advances the rotation once"),
		Game->Map, FString(TEXT("Arena_03")));
	Game->RestartGame();
	TestEqual(TEXT("RestartGame latches - one rotation per match"),
		Game->Map, FString(TEXT("Arena_03")));

	// No installed map: RestartGame reloads (?Restart) and stages nothing.
	UWorld* World2 = WoTTest::CreateSpecWorld();
	if (AWoTGameMode* Empty = World2->GetAuthGameMode<AWoTGameMode>())
	{
		Empty->SetInstalledMapsForTesting({});
		Empty->RestartGame();
		TestTrue(TEXT("no map to rotate to stages no travel"), Empty->PendingMapURL.IsEmpty());
	}
	WoTTest::DestroySpecWorld(World2);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPlayerStartInSolidTest,
	"WoT.Rules.PlayerStartInSolidFails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTPlayerStartInSolidTest::RunTest(const FString&)
{
	// UE1 FindSpot fails a start whose center is in solid BSP (Battle3_01
	// PlayerStart27/28 under a semisolid floor); the spawn fails and the
	// retry picks another start instead of dropping the player out of the world.
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTGameMode* Game = World ? World->GetAuthGameMode<AWoTGameMode>() : nullptr;
	APlayerController* PC = World ? World->SpawnActor<APlayerController>() : nullptr;
	if (!TestNotNull(TEXT("game mode"), Game) || !TestNotNull(TEXT("controller"), PC))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	PC->SetAsLocalPlayerController();
	// One csg node on z=0: front (above) is outside, back (below) is solid.
	AWoTLeafMap* LeafMap = World->SpawnActor<AWoTLeafMap>();
	LeafMap->NodePlanes = {0, 0, 1, 0};
	LeafMap->NodeChildren = {INDEX_NONE, INDEX_NONE};
	LeafMap->NodeLeaves = {0, 1};
	LeafMap->NodeCsg = {1};
	LeafMap->NumLeaves = 2;
	TestTrue(TEXT("a point under the floor is in solid"),
		LeafMap->IsPointInSolid(FVector(0, 0, -44)));
	TestFalse(TEXT("a point over the floor is outside"),
		LeafMap->IsPointInSolid(FVector(0, 0, 48)));

	auto Restart = [&](APlayerStart* Start) -> APawn*
	{
		if (APawn* OldPawn = PC->GetPawn())
		{
			PC->UnPossess();
			OldPawn->Destroy();
		}
		Game->RestartPlayerAtPlayerStart(PC, Start);
		return PC->GetPawn();
	};
	APlayerStart* Embedded = World->SpawnActor<AWoTPlayerStart>(
		FVector(0, 0, -44), FRotator::ZeroRotator);
	APlayerStart* Free = World->SpawnActor<AWoTPlayerStart>(
		FVector(3000, 0, 48), FRotator::ZeroRotator);
	APawn* EmbeddedPawn = Restart(Embedded);
	TestNull(TEXT("a start embedded in solid does not spawn"), EmbeddedPawn);
	APawn* FreePawn = Restart(Free);
	TestNotNull(TEXT("a start in open space spawns"), FreePawn);
	// Retail would still pick the embedded start and reject the login
	// (GameInfo.Login FailedSpawnMessage); the port never chooses it.
	if (APawn* OldPawn = PC->GetPawn())
	{
		PC->UnPossess();
		OldPawn->Destroy();
	}
	bool bNeverEmbedded = true;
	for (int32 Attempt = 0; Attempt < 16; ++Attempt)
	{
		bNeverEmbedded &= Game->ChoosePlayerStart(PC) == Free;
	}
	TestTrue(TEXT("a start embedded in solid is never chosen"), bNeverEmbedded);
	Game->RestartPlayer(PC);
	APawn* JoinedPawn = PC->GetPawn();
	TestNotNull(TEXT("a join spawns at the spawnable start"), JoinedPawn);

	// Without a csg bake the check fails open: the authored start spawns.
	LeafMap->NodeCsg.Reset();
	TestFalse(TEXT("no csg bake classifies nothing as solid"),
		LeafMap->IsPointInSolid(FVector(0, 0, -44)));
	APawn* UnbakedPawn = Restart(Embedded);
	TestNotNull(TEXT("without a csg bake the start spawns as before"), UnbakedPawn);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPlayerStartTest,
	"WoT.Rules.PlayerStart",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTPlayerStartTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTGameMode* Game = World->GetAuthGameMode<AWoTGameMode>();
	if (!Game)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	// Two starts far apart, and a controller to spawn.
	APlayerStart* StartA = World->SpawnActor<AWoTPlayerStart>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	APlayerStart* StartB = World->SpawnActor<AWoTPlayerStart>(
		FVector(5000, 0, 100), FRotator::ZeroRotator);
	APlayerController* PC = World->SpawnActor<APlayerController>();
	if (PC) PC->SetAsLocalPlayerController();
	TestNotNull(TEXT("start A spawned"), StartA);
	TestNotNull(TEXT("start B spawned"), StartB);

	// With no one else around, ChoosePlayerStart returns one of the arena's starts.
	AActor* Chosen = Game->ChoosePlayerStart(PC);
	TestTrue(TEXT("chooses one of the player starts"), Chosen == StartA || Chosen == StartB);

	// Occupy start A with a pawn: the -1,000,000 overlap penalty dominates the ±1000 random
	// base, so the scored selection must avoid it every time (giCombatBase.FindPlayerStart).
	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTCharacter* Occupier = World->SpawnActor<AWoTCharacter>(
		AWoTCharacter::StaticClass(), FVector(0, 0, 100), FRotator::ZeroRotator, SpawnParams);
	TestNotNull(TEXT("occupier pawn spawned on start A"), Occupier);

	bool bAlwaysAvoidedA = true;
	for (int32 i = 0; i < 8; ++i)
	{
		bAlwaysAvoidedA &= (Game->ChoosePlayerStart(PC) == StartB);
	}
	TestTrue(TEXT("never spawns on top of an occupying player"), bAlwaysAvoidedA);

	// Production-audit witnesses: Battle2_02 authors starts at floor+14,
	// ordinary maps at floor+40, and Battle4_01 at floor+83.  Native FindSpot
	// lifts only embedded cylinders; it never pulls a free high cylinder down.
	struct FSeatCase
	{
		const TCHAR* Label;
		float X;
		float AuthoredHeight;
		bool bFalls;
	};
	const FSeatCase Cases[] = {
		{TEXT("Battle2_02 floor+14"), 10000.0f, 14.0f, false},
		{TEXT("typical floor+40"), 12000.0f, 40.0f, false},
		{TEXT("Battle4_01 floor+83"), 14000.0f, 83.0f, true},
	};
	for (const FSeatCase& Seat : Cases)
	{
		// Level geometry (a StaticMeshActor, as the dev world meshes are): FindSpot
		// fits and traces against the level only, never a BlockAll actor.
		AStaticMeshActor* FloorActor = World->SpawnActor<AStaticMeshActor>(
			FVector(Seat.X, 0, -10), FRotator::ZeroRotator, SpawnParams);
		UBoxComponent* FloorBox = NewObject<UBoxComponent>(FloorActor, TEXT("SeatFloor"));
		FloorBox->SetBoxExtent(FVector(300.0f, 300.0f, 10.0f));
		FloorBox->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		FloorBox->SetCollisionObjectType(ECC_WorldStatic);
		FloorBox->SetCollisionResponseToAllChannels(ECR_Block);
		FloorBox->SetupAttachment(FloorActor->GetRootComponent());
		FloorBox->RegisterComponent();
		APlayerStart* ProductionStart = World->SpawnActor<AWoTPlayerStart>(
			FVector(Seat.X, 0, Seat.AuthoredHeight), FRotator::ZeroRotator);

		for (int32 Pass = 0; Pass < 2; ++Pass)
		{
			if (APawn* OldPawn = PC->GetPawn())
			{
				PC->UnPossess();
				OldPawn->Destroy();
			}
			Game->RestartPlayerAtPlayerStart(PC, ProductionStart);
			AWoTCharacter* Spawned = Cast<AWoTCharacter>(PC->GetPawn());
			const FString Prefix = FString::Printf(TEXT("%s %s"), Seat.Label,
				Pass == 0 ? TEXT("login") : TEXT("restart"));
			if (!TestNotNull(Prefix + TEXT(" spawns a production pawn"), Spawned))
			{
				continue;
			}
			const double InitialZ = Spawned->GetActorLocation().Z;
			if (Seat.bFalls)
			{
				TestEqual(Prefix + TEXT(" native FindSpot leaves a free start authored"),
					InitialZ, static_cast<double>(Seat.AuthoredHeight), 0.1);
				Spawned->GetCharacterMovement()->SetMovementMode(MOVE_Falling);
			}
			else
			{
				TestTrue(Prefix + TEXT(" native FindSpot seats without penetration"),
					InitialZ >= 46.0 && InitialZ <= 49.0);
			}
			WoTTest::TickWorld(World, Seat.bFalls ? 0.8f : 0.1f);
			if (Seat.bFalls)
			{
				const double FallDistance =
					InitialZ - Spawned->GetActorLocation().Z;
				TestTrue(Prefix + FString::Printf(
					TEXT(" performs the retail-scale fall to seating (distance %.3f)"),
					FallDistance),
					FallDistance >= 34.0 && FallDistance <= 37.5);
				TestFalse(Prefix + TEXT(" lands after the free-start fall"),
					Spawned->GetCharacterMovement()->IsFalling());
			}
			else
			{
				TestFalse(Prefix + TEXT(" seats flush without falling"),
					Spawned->GetCharacterMovement()->IsFalling());
				TestFalse(Prefix + TEXT(" does not synthesize LandingFreeze"),
					Spawned->IsHardLanding());
			}
		}
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMatchRestartTest,
	"WoT.Rules.MatchRestart",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTMatchRestartTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTGameMode* Game = World->GetAuthGameMode<AWoTGameMode>();
	if (!Game)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Game->bChangeLevels = true;
	Game->SetInstalledMapsForTesting({ TEXT("Arena_02"), TEXT("Arena_03") });

	AWoTPlayerController* PC = World->SpawnActor<AWoTPlayerController>();
	TestNotNull(TEXT("WoT player controller spawned"), PC);
	if (!PC)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	PC->PlayerRestartGameDelay = 0.1f;   // keep the GameEnded freeze brief for the test
	PC->ArmRespawnFire(nullptr, 0.0f);
	TestEqual(TEXT("precondition: controller is awaiting respawn Fire"),
		static_cast<int32>(PC->GetPlayerStateMachineState()),
		static_cast<int32>(EWoTPlayerStateMachineState::Dying));

	// A FragLimit decision supersedes even an armed Dying gate: EndGame notifies the
	// controller, freezes it, and enters GameEnded rather than allowing a player respawn.
	Game->EndGame(TEXT("fraglimit"));
	TestTrue(TEXT("match end enters the controller's ended state"), PC->IsMatchEnded());
	TestEqual(TEXT("match end supersedes respawn in the FSM"),
		static_cast<int32>(PC->GetPlayerStateMachineState()),
		static_cast<int32>(EWoTPlayerStateMachineState::GameEnded));
	TestFalse(TEXT("the player is frozen right after match end"), PC->IsRestartAllowed());

	// A fire while frozen must not restart (WOTPlayer.GameEnded.Fire gates on !bFrozen).
	PC->Fire();
	TestTrue(TEXT("a frozen fire does not rotate the map"), Game->PendingMapURL.IsEmpty());

	// Wait out PlayerRestartGameDelay — the freeze lifts.
	WoTTest::TickWorld(World, 0.2f);
	TestTrue(TEXT("the freeze lifts after PlayerRestartGameDelay"), PC->IsRestartAllowed());

	// Now a fire restarts the match (PlayerPawn.ServerReStartGame -> Level.Game.RestartGame),
	// which for this bChangeLevels match rotates to the next map.
	PC->Fire();
	TestEqual(TEXT("a fire after the delay rotates the match"),
		Game->Map, FString(TEXT("Arena_02")));

	WoTTest::DestroySpecWorld(World);
	return true;
}

// Input-freeze presentation (WOTPlayer.GameEnded): once the match is decided the owning
// player's movement freezes and a fire stops casting — it becomes the restart request. Proves
// the freeze applies + the fire→restart re-route flips, through controller-owned Fire input.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMatchEndFreezeTest,
	"WoT.Rules.MatchEndFreeze",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTMatchEndFreezeTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTGameMode* Game = World->GetAuthGameMode<AWoTGameMode>();
	if (!Game)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Game->bChangeLevels = true;
	Game->SetInstalledMapsForTesting({ TEXT("Arena_02") });

	AWoTPlayerController* PC = World->SpawnActor<AWoTPlayerController>();
	if (!PC)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	PC->SetAsLocalPlayerController();
	PC->InitPlayerState();
	PC->PlayerRestartGameDelay = 0.1f;   // keep the GameEnded freeze brief for the test

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		AWoTCharacter::StaticClass(), FVector(0, 0, 100), FRotator::ZeroRotator, SpawnParams);
	PC->Possess(Pawn);

	// During play: movement is free, and a fire is the normal cast path (never a restart).
	TestFalse(TEXT("movement is free during play"), PC->IsMoveInputIgnored());
	PC->Fire();
	WoTTest::TickWorld(World, 0.02f);
	TestTrue(TEXT("a fire during play does not restart the match"), Game->PendingMapURL.IsEmpty());

	// Match decided: the controller freezes movement and Fire re-routes to the restart
	// request instead of casting.
	Game->EndGame(TEXT("fraglimit"));
	TestTrue(TEXT("controller enters the ended state"), PC->IsMatchEnded());
	TestTrue(TEXT("movement is frozen once the match ends"), PC->IsMoveInputIgnored());

	// A fire while still frozen requests a restart but is ignored (the delay has not lifted).
	PC->Fire();
	WoTTest::TickWorld(World, 0.02f);
	TestTrue(TEXT("a frozen fire does not restart"), Game->PendingMapURL.IsEmpty());

	// After PlayerRestartGameDelay the freeze lifts; now a fire rotates the match via the
	// controller FSM re-route (not a direct ServerRestartGame call).
	WoTTest::TickWorld(World, 0.2f);
	TestTrue(TEXT("the freeze lifts after the delay"), PC->IsRestartAllowed());
	PC->Fire();
	TestEqual(TEXT("a fire after the delay rotates the match"),
		Game->Map, FString(TEXT("Arena_02")));

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSuicideInstigatorTest,
	"WoT.Rules.SuicideInstigator",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTSuicideInstigatorTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	World->SpawnActor<APlayerStart>(FVector(500, 500, 100), FRotator::ZeroRotator);
	AWoTGameMode* Game = World->GetAuthGameMode<AWoTGameMode>();
	if (!Game)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	auto SpawnPlayer = [&](const FVector& Loc) -> APlayerController*
	{
		APlayerController* PC = World->SpawnActor<APlayerController>();
		PC->InitPlayerState();
		Game->ChangeTeam(PC, 255);   // login assigns a unique free-for-all team (enemies)
		PC->Possess(World->SpawnActor<AWoTCharacter>(Loc, FRotator::ZeroRotator));
		return PC;
	};

	APlayerController* CasterPC = SpawnPlayer(FVector(200, 0, 100));
	APlayerController* VictimPC = SpawnPlayer(FVector(0, 0, 100));
	AWoTPlayerState* CasterPS = CasterPC->GetPlayerState<AWoTPlayerState>();
	AWoTPlayerState* VictimPS = VictimPC->GetPlayerState<AWoTPlayerState>();
	AWoTCharacter* Caster = Cast<AWoTCharacter>(CasterPC->GetPawn());
	AWoTCharacter* Victim = Cast<AWoTCharacter>(VictimPC->GetPawn());
	if (!CasterPS || !VictimPS || !Caster || !Victim)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	FWoTDamageEvent Ev;
	Ev.WoTDamageType = TEXT("Fire");

	// A recent displacement (Whirlwind / SwapPlaces set the instigator): a subsequent
	// killer-less death credits the caster, not a suicide (giWOT.ScoreKill).
	Victim->SetSuicideInstigator(Caster);
	Victim->TakeDamage(1000.0f, Ev, nullptr, nullptr);   // no killer — fell in a hazard
	TestEqual(TEXT("a recent instigator is credited the frag"), CasterPS->Kills, 1);
	TestEqual(TEXT("the victim takes a death, not a suicide"), VictimPS->Deaths, 1);
	TestEqual(TEXT("an instigated death is not a suicide"), VictimPS->Suicides, 0);

	// A stale instigation (older than SuicideInstigationRelevance) scores a plain suicide.
	APlayerController* Victim2PC = SpawnPlayer(FVector(-200, 0, 100));
	AWoTPlayerState* Victim2PS = Victim2PC->GetPlayerState<AWoTPlayerState>();
	AWoTCharacter* Victim2 = Cast<AWoTCharacter>(Victim2PC->GetPawn());
	Victim2->SetSuicideInstigator(Caster);
	Victim2->SuicideInstigationTime -= (Game->SuicideInstigationRelevance + 1.0f);
	Victim2->TakeDamage(1000.0f, Ev, nullptr, nullptr);
	TestEqual(TEXT("a stale instigation is not credited"), CasterPS->Kills, 1);
	TestEqual(TEXT("a stale-instigation death is a suicide"), Victim2PS->Suicides, 1);

	// The credit survives the caster's own pawn being unpossessed/replaced within the window
	// (the port unpossesses dead pawns before respawn). SuicideInstigator stores the caster's
	// controller, so the frag still lands on their PlayerState.
	APlayerController* Victim3PC = SpawnPlayer(FVector(300, 0, 100));
	AWoTPlayerState* Victim3PS = Victim3PC->GetPlayerState<AWoTPlayerState>();
	AWoTCharacter* Victim3 = Cast<AWoTCharacter>(Victim3PC->GetPawn());
	Victim3->SetSuicideInstigator(Caster);   // caster still possessed here
	CasterPC->UnPossess();                    // caster died and left its pawn (respawn pending)
	Victim3->TakeDamage(1000.0f, Ev, nullptr, nullptr);
	TestEqual(TEXT("credit survives the caster being unpossessed"), CasterPS->Kills, 2);
	TestEqual(TEXT("an instigated death is still not a suicide"), Victim3PS->Suicides, 0);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTTeamScoringTest,
	"WoT.Rules.TeamScoring",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTTeamScoringTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	World->SpawnActor<APlayerStart>(FVector(500, 500, 100), FRotator::ZeroRotator);
	AWoTGameMode* Game = World->GetAuthGameMode<AWoTGameMode>();
	if (!Game)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	auto SpawnPlayer = [&](const FVector& Loc) -> APlayerController*
	{
		APlayerController* PC = World->SpawnActor<APlayerController>();
		PC->InitPlayerState();
		PC->Possess(World->SpawnActor<AWoTCharacter>(Loc, FRotator::ZeroRotator));
		return PC;
	};

	// giCombatBase.ChangeTeam free-for-all: three players get three distinct team numbers.
	APlayerController* A = SpawnPlayer(FVector(0, 0, 100));
	APlayerController* B = SpawnPlayer(FVector(200, 0, 100));
	APlayerController* C = SpawnPlayer(FVector(400, 0, 100));
	Game->ChangeTeam(A, 0);
	Game->ChangeTeam(B, 0);
	Game->ChangeTeam(C, 0);
	AWoTPlayerState* PA = A->GetPlayerState<AWoTPlayerState>();
	AWoTPlayerState* PB = B->GetPlayerState<AWoTPlayerState>();
	AWoTPlayerState* PC2 = C->GetPlayerState<AWoTPlayerState>();
	TestTrue(TEXT("free-for-all ChangeTeam assigns distinct teams"),
		PA->Team != PB->Team && PB->Team != PC2->Team && PA->Team != PC2->Team);

	FWoTDamageEvent Ev;
	Ev.WoTDamageType = TEXT("Fire");

	// Cross-team kill (A and B are on different teams): A scores a frag, B takes a death.
	B->GetPawn()->TakeDamage(1000.0f, Ev, A, A->GetPawn());
	TestEqual(TEXT("a cross-team kill scores a frag"), PA->Kills, 1);
	TestEqual(TEXT("a cross-team victim takes a death"), PB->Deaths, 1);

	// Same-team kill: a fresh victim on A's team. A gets NO frag; the victim still dies.
	APlayerController* D = SpawnPlayer(FVector(-200, 0, 100));
	AWoTPlayerState* PD = D->GetPlayerState<AWoTPlayerState>();
	PD->Team = PA->Team;
	D->GetPawn()->TakeDamage(1000.0f, Ev, A, A->GetPawn());
	TestEqual(TEXT("a friendly kill gives the killer no frag"), PA->Kills, 1);
	TestEqual(TEXT("a friendly-killed victim still takes a death"), PD->Deaths, 1);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// --- Citadel / siege (giMPBattle) block ------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelGameStateTest,
	"WoT.Battle.CitadelGameState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelGameStateTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	// CitadelGameReplicationInfo is a straight extension of the Arena match clock, so a
	// spawned instance carries the SealGoal target AND inherits the counting-down clock.
	AWoTCitadelGameState* GS = World->SpawnActor<AWoTCitadelGameState>();
	TestNotNull(TEXT("citadel game state spawned"), GS);
	if (!GS)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	// CitadelGameReplicationInfo.SealGoal default is 4 (defaultproperties).
	TestEqual(TEXT("SealGoal defaults to 4"), GS->SealGoal, 4);

	// It IS-A AWoTGameState: the base GameReplicationInfo match clock still ticks down.
	TestTrue(TEXT("extends the base match clock"), GS->IsA<AWoTGameState>());
	GS->RemainingTime = 100.0f;
	GS->bDisplayRemaining = true;
	WoTTest::TickWorld(World, 0.5f);
	TestTrue(TEXT("inherits the counting-down clock"),
		GS->RemainingTime < 100.0f && GS->RemainingTime > 0.0f);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBattleSealSettingsTest,
	"WoT.Battle.SealSettings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTBattleSealSettingsTest::RunTest(const FString&)
{
	auto RunCase = [this](const FString& Options, int32 ExpectedGoal,
		int32 ExpectedBudget, const FString& Label)
	{
		AWoTSealAltar* Altar = nullptr;
		AWoTBudgetInfo* Budget = nullptr;
		WoTTest::FSpecWorldOptions WorldOptions;
		WorldOptions.GameModeClass = AWoTBattleGameMode::StaticClass();
		WorldOptions.URL = TEXT("Battle2_01") + Options;
		WorldOptions.BeforeInitializeActorsForPlay =
			[&](UWorld* World)
			{
				AWoTBattleInfo* Battle = World->SpawnActorDeferred<AWoTBattleInfo>(
					AWoTBattleInfo::StaticClass(), FTransform::Identity);
				Battle->NumTeams = 2;
				Battle->TeamClass[0] = AWoTTestTeamAlpha::StaticClass();
				Battle->TeamClass[1] = AWoTTestTeamBravo::StaticClass();
				Battle->FinishSpawning(FTransform::Identity);

				Altar = World->SpawnActorDeferred<AWoTSealAltar>(
					AWoTSealAltar::StaticClass(), FTransform::Identity);
				Altar->Team = 0;
				Altar->FinishSpawning(FTransform::Identity);

				Budget = World->SpawnActorDeferred<AWoTBudgetInfo>(
					AWoTBudgetInfo::StaticClass(), FTransform::Identity);
				Budget->Team = 0;
				FWoTBudgetResource SealLine;
				SealLine.ResourcePackageName = TEXT("Angreal");
				SealLine.ResourceAdapterName = TEXT("SealInventory");
				Budget->Budget.Add(SealLine);
				Budget->FinishSpawning(FTransform::Identity);
			};

		UWorld* World = WoTTest::CreateSpecWorld(WorldOptions);
		AWoTBattleGameMode* Mode = World
			? World->GetAuthGameMode<AWoTBattleGameMode>() : nullptr;
		AWoTCitadelGameState* GameState = World
			? World->GetGameState<AWoTCitadelGameState>() : nullptr;
		if (TestNotNull(Label + TEXT(" mode"), Mode)
			&& TestNotNull(Label + TEXT(" game state"), GameState)
			&& TestNotNull(Label + TEXT(" altar"), Altar)
			&& TestNotNull(Label + TEXT(" budget"), Budget))
		{
			TestEqual(Label + TEXT(" game-state goal"),
				GameState->SealGoal, ExpectedGoal);
			TestEqual(Label + TEXT(" mode budget"),
				Mode->SealBudget, ExpectedBudget);
			TestEqual(Label + TEXT(" altar reset goal"),
				Altar->SealGoal, ExpectedGoal);
			TestEqual(Label + TEXT(" budget reset count"),
				Budget->GetItemCount(Budget->GetIndex(TEXT("SealInventory"))),
				ExpectedBudget);
		}
		WoTTest::DestroySpecWorld(World);
	};

	RunCase(TEXT("?SealGoal=3?SealBudget=2"), 3, 2, TEXT("custom"));
	RunCase(TEXT("?SealGoal=1?SealBudget=99"), 2, 1, TEXT("low goal"));
	RunCase(TEXT("?SealGoal=99?SealBudget=-5"), 4, 2, TEXT("high goal"));
	RunCase(FString(), 4, 3, TEXT("absent options"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBattleInfoTest,
	"WoT.Battle.BattleInfo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTBattleInfoTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	// BattleInfo is placed map data (NumTeams + per-team pawn class set before PreBeginPlay).
	// Defer the spawn so those are set before BeginPlay's validation runs — the map-placement
	// path, and the Fireworks pattern for pre-BeginPlay setup.
	AWoTBattleInfo* BI = World->SpawnActorDeferred<AWoTBattleInfo>(
		AWoTBattleInfo::StaticClass(), FTransform::Identity);
	TestNotNull(TEXT("battle info spawned"), BI);
	if (!BI)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	BI->NumTeams = 2;
	BI->TeamClass[0] = AWoTTestTeamAlpha::StaticClass();
	BI->TeamClass[1] = AWoTTestTeamBravo::StaticClass();
	BI->FinishSpawning(FTransform::Identity);

	// Seal bookkeeping (BattleInfo.uc:59-80): starts empty, increments/decrements, resets.
	TestEqual(TEXT("team 0 starts with no seals"), BI->GetTeamSealCount(0), 0);
	BI->IncrementTeamSealCount(0);
	BI->IncrementTeamSealCount(0);
	BI->IncrementTeamSealCount(0);
	TestEqual(TEXT("three increments -> 3 seals"), BI->GetTeamSealCount(0), 3);
	BI->DecrementTeamSealCount(0);
	TestEqual(TEXT("a decrement -> 2 seals"), BI->GetTeamSealCount(0), 2);
	BI->ResetTeamSealCount(0);
	TestEqual(TEXT("reset -> 0 seals"), BI->GetTeamSealCount(0), 0);
	// A per-team count, not global: team 1 is untouched.
	BI->IncrementTeamSealCount(1);
	TestEqual(TEXT("seal counts are per team"), BI->GetTeamSealCount(1), 1);
	// The spectator sentinel holds no seals.
	TestEqual(TEXT("spectator (255) holds no seals"), BI->GetTeamSealCount(255), 0);

	// GetTeamDescription reads TeamClass[Team]'s CDO TeamDescription (BattleInfo.uc:82-88).
	TestEqual(TEXT("team 0 description from its pawn CDO"),
		BI->GetTeamDescription(0), FString(TEXT("Alpha")));
	TestEqual(TEXT("team 1 description from its pawn CDO"),
		BI->GetTeamDescription(1), FString(TEXT("Bravo")));
	TestEqual(TEXT("spectator (255) description"),
		BI->GetTeamDescription(255), FString(TEXT("Spectator")));

	// GetTeamStatus (BattleInfo.uc:90-96): Editing > Playing (teleporters live) > Ready > Waiting.
	TestEqual(TEXT("teleporters live -> Playing"), BI->GetTeamStatus(0), BI->PlayingStr);
	BI->bTeleportersDisabled = true;
	BI->bEditingComplete[0] = 1;
	TestEqual(TEXT("teleporters down + edit complete -> Ready"), BI->GetTeamStatus(0), BI->ReadyStr);
	BI->bEditingComplete[0] = 0;
	TestEqual(TEXT("teleporters down + not complete -> Waiting"), BI->GetTeamStatus(0), BI->WaitingStr);
	BI->bEditing[0] = 1;
	TestEqual(TEXT("actively editing -> Editing"), BI->GetTeamStatus(0), BI->EditingStr);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBattleTeamManagementTest,
	"WoT.Battle.TeamManagement",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTBattleTeamManagementTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	// Place the BattleInfo (map data) first so the mode's PostBeginPlay lookup finds it.
	AWoTBattleInfo* BI = World->SpawnActorDeferred<AWoTBattleInfo>(
		AWoTBattleInfo::StaticClass(), FTransform::Identity);
	BI->NumTeams = 2;
	BI->TeamClass[0] = AWoTTestTeamAlpha::StaticClass();
	BI->TeamClass[1] = AWoTTestTeamBravo::StaticClass();
	BI->FinishSpawning(FTransform::Identity);

	// The Battle mode's BeginPlay (giMPBattle.PostBeginPlay) locates the BattleInfo, pushes
	// MaxTeamSize onto it, and ResetBattle-disables teleporters (the editing-phase start state).
	AWoTBattleGameMode* Mode = World->SpawnActor<AWoTBattleGameMode>();
	TestNotNull(TEXT("battle mode spawned"), Mode);
	if (!Mode)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	TestTrue(TEXT("PostBeginPlay located the placed BattleInfo"), Mode->Battle == BI);
	TestEqual(TEXT("PostBeginPlay pushed MaxTeamSize onto the BattleInfo"),
		BI->MaxTeamSize, Mode->MaxTeamSize);
	TestTrue(TEXT("ResetBattle disables teleporters at start"), BI->bTeleportersDisabled);

	auto MakePlayer = [&]() -> APlayerController*
	{
		APlayerController* PC = World->SpawnActor<APlayerController>();
		PC->InitPlayerState();
		PC->GetPlayerState<AWoTPlayerState>()->Team = 255;   // start neutral
		return PC;
	};
	auto TeamOf = [](APlayerController* PC) -> int32
	{
		return static_cast<int32>(PC->GetPlayerState<AWoTPlayerState>()->Team);
	};

	APlayerController* A = MakePlayer();
	APlayerController* B = MakePlayer();
	APlayerController* C = MakePlayer();

	// Honor a valid requested team + count it (unlike the free-for-all Arena base).
	TestTrue(TEXT("join team 0"), Mode->ChangeTeam(A, 0));
	TestEqual(TEXT("A is on team 0"), TeamOf(A), 0);
	TestEqual(TEXT("team 0 has 1 player"), BI->NumPlayers[0], 1);
	Mode->ChangeTeam(B, 0);
	Mode->ChangeTeam(C, 0);
	TestEqual(TEXT("team 0 fills to MaxTeamSize (3)"), BI->NumPlayers[0], 3);

	// A fourth is rejected (the only false path) and left untouched.
	APlayerController* D = MakePlayer();
	TestFalse(TEXT("a full team rejects the join"), Mode->ChangeTeam(D, 0));
	TestEqual(TEXT("a rejected join does not overfill"), BI->NumPlayers[0], 3);
	TestEqual(TEXT("a rejected player stays neutral"), TeamOf(D), 255);

	// Switching teams moves the headcount (leave old, join new).
	TestTrue(TEXT("A switches to team 1"), Mode->ChangeTeam(A, 1));
	TestEqual(TEXT("team 0 drops to 2"), BI->NumPlayers[0], 2);
	TestEqual(TEXT("team 1 gains 1"), BI->NumPlayers[1], 1);
	TestEqual(TEXT("A is on team 1"), TeamOf(A), 1);

	// Re-requesting the team you already hold is a no-op (returns true, no double count).
	TestTrue(TEXT("re-request current team is a no-op"), Mode->ChangeTeam(A, 1));
	TestEqual(TEXT("team 1 still 1"), BI->NumPlayers[1], 1);

	// Out-of-range team -> rejected as a no-op (returns true, no change).
	TestTrue(TEXT("out-of-range team returns true"), Mode->ChangeTeam(B, 5));
	TestEqual(TEXT("B stays on team 0"), TeamOf(B), 0);

	// GetSmallestTeam: team 1 has 1 (A), team 0 has 2 (B,C) -> smallest is team 1.
	TestEqual(TEXT("smallest team is team 1"), Mode->GetSmallestTeam(), 1);

	// Neutral (255): leave the team, decrement the count.
	TestTrue(TEXT("C leaves to neutral"), Mode->ChangeTeam(C, 255));
	TestEqual(TEXT("team 0 drops to 1"), BI->NumPlayers[0], 1);
	TestEqual(TEXT("C is neutral"), TeamOf(C), 255);

	// Logout drops the player from their team's headcount and neutralizes them.
	Mode->Logout(B);
	TestEqual(TEXT("logout decrements team 0 to 0"), BI->NumPlayers[0], 0);
	TestEqual(TEXT("logged-out player set neutral"), TeamOf(B), 255);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSealAltarTest,
	"WoT.Battle.SealAltar",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTSealAltarTest::RunTest(const FString&)
{
	// The win economy reaches the Battle mode via the authority game mode.
	UWorld* World = CreateBattleTestWorld();
	AWoTBattleGameMode* Mode = World->GetAuthGameMode<AWoTBattleGameMode>();
	TestNotNull(TEXT("battle mode is the authority"), Mode);
	if (!Mode)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	// Place the BattleInfo and wire it onto the mode (the mode's PostBeginPlay lookup ran during
	// world start, before this actor existed — step 3 covers the lookup itself).
	AWoTBattleInfo* BI = World->SpawnActorDeferred<AWoTBattleInfo>(
		AWoTBattleInfo::StaticClass(), FTransform::Identity);
	BI->NumTeams = 2;
	BI->TeamClass[0] = AWoTTestTeamAlpha::StaticClass();
	BI->TeamClass[1] = AWoTTestTeamBravo::StaticClass();
	BI->FinishSpawning(FTransform::Identity);
	Mode->Battle = BI;

	// A team-0 altar that wins at 2 seals. Spawned after the BattleInfo so its BeginPlay finds it.
	AWoTSealAltar* Altar = World->SpawnActor<AWoTSealAltar>();
	TestNotNull(TEXT("altar spawned"), Altar);
	Altar->Team = 0;
	Altar->SealGoal = 2;
	TestTrue(TEXT("altar located the BattleInfo"), Altar->Battle == BI);

	// A team-0 player and a team-1 player, each with a possessed pawn (the seal's owner).
	auto MakePlayer = [&](uint8 TeamNum) -> AWoTCharacter*
	{
		APlayerController* PC = World->SpawnActor<APlayerController>();
		PC->InitPlayerState();
		PC->GetPlayerState<AWoTPlayerState>()->Team = TeamNum;
		AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(FVector::ZeroVector, FRotator::ZeroRotator);
		PC->Possess(Pawn);
		return Pawn;
	};
	auto MakeSeal = [&](AWoTCharacter* Owner) -> AWoTSeal*
	{
		FActorSpawnParameters P;
		P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AWoTSeal* S = World->SpawnActor<AWoTSeal>(AWoTSeal::StaticClass(),
			FVector::ZeroVector, FRotator::ZeroRotator, P);
		S->SetOwner(Owner);   // Player = WOTPlayer(Seal.Owner)
		return S;
	};

	AWoTCharacter* Ally = MakePlayer(0);
	AWoTCharacter* Enemy = MakePlayer(1);

	// A team-0 player banks a seal: credited to team 0, not yet a winner.
	Altar->PlaceSeal(MakeSeal(Ally));
	TestEqual(TEXT("banking credits team 0's seal count"), BI->GetTeamSealCount(0), 1);
	TestFalse(TEXT("one seal short of the goal"), Altar->CheckForWinner());

	// A team-1 player is refused at a team-0 altar (non-forced): no credit, no bank.
	Altar->PlaceSeal(MakeSeal(Enemy));
	TestEqual(TEXT("the enemy's seal is refused"), BI->GetTeamSealCount(0), 1);
	TestEqual(TEXT("the refused seal credits no one"), BI->GetTeamSealCount(1), 0);

	// A forced bank bypasses the team gate (the budget/editor path): team 1 gets credited.
	Altar->PlaceSeal(MakeSeal(Enemy), /*bForce*/ true);
	TestEqual(TEXT("a forced bank bypasses the team gate"), BI->GetTeamSealCount(1), 1);

	// The team-0 player banks the second seal -> the goal is reached.
	Altar->PlaceSeal(MakeSeal(Ally));
	TestEqual(TEXT("team 0 reaches 2 seals"), BI->GetTeamSealCount(0), 2);
	TestTrue(TEXT("the goal is met -> a winner"), Altar->CheckForWinner());

	// AnnounceWinningTeam records the winner on the mode and ends the game.
	Altar->AnnounceWinningTeam();
	TestEqual(TEXT("the winning team is recorded on the mode"), (int32)Mode->WinningTeam, 0);
	TestTrue(TEXT("the match ends"), Mode->bGameEnded);
	TestTrue(TEXT("the altar enters its ended state"), Altar->bEnded);

	// Reset clears the banked seals and the team count, and re-arms the goal.
	Altar->Reset(3);
	TestEqual(TEXT("reset zeroes the team seal count"), BI->GetTeamSealCount(0), 0);
	TestEqual(TEXT("reset re-arms the goal"), Altar->SealGoal, 3);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFriendlyFireTest,
	"WoT.Battle.FriendlyFire",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTFriendlyFireTest::RunTest(const FString&)
{
	UWorld* World = CreateBattleTestWorld();
	AWoTBattleGameMode* Mode = World->GetAuthGameMode<AWoTBattleGameMode>();
	if (!Mode)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	AWoTBattleInfo* BI = World->SpawnActorDeferred<AWoTBattleInfo>(
		AWoTBattleInfo::StaticClass(), FTransform::Identity);
	BI->NumTeams = 2;
	BI->TeamClass[0] = AWoTTestTeamAlpha::StaticClass();
	BI->TeamClass[1] = AWoTTestTeamBravo::StaticClass();
	BI->FinishSpawning(FTransform::Identity);
	Mode->Battle = BI;
	Mode->FriendlyFireScale = 0.25f;

	auto MakePawn = [&](uint8 TeamNum) -> AWoTCharacter*
	{
		APlayerController* PC = World->SpawnActor<APlayerController>();
		PC->InitPlayerState();
		PC->GetPlayerState<AWoTPlayerState>()->Team = TeamNum;
		AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(FVector::ZeroVector, FRotator::ZeroRotator);
		PC->Possess(Pawn);
		return Pawn;
	};

	AWoTCharacter* AllyA = MakePawn(0);
	AWoTCharacter* AllyB = MakePawn(0);
	AWoTCharacter* Enemy = MakePawn(1);
	AWoTCharacter* Neutral = MakePawn(255);

	// --- Play has begun (teleporters enabled): the team damage rules apply. ---
	BI->bTeleportersDisabled = false;

	// Same-team damage is scaled by FriendlyFireScale (0.25).
	TestEqual(TEXT("same-team damage is friendly-fire scaled"),
		Mode->ReduceDamage(100, TEXT("Fire"), AllyB, AllyA), 25);
	// Cross-team damage is full.
	TestEqual(TEXT("cross-team damage is unmodified"),
		Mode->ReduceDamage(100, TEXT("Fire"), Enemy, AllyA), 100);
	// A neutral victim takes no damage; a neutral instigator deals none.
	TestEqual(TEXT("a neutral victim is immune"),
		Mode->ReduceDamage(100, TEXT("Fire"), Neutral, AllyA), 0);
	TestEqual(TEXT("a neutral instigator deals nothing"),
		Mode->ReduceDamage(100, TEXT("Fire"), AllyB, Neutral), 0);
	// Instigator-less (environmental) damage bypasses the team rules -> full.
	TestEqual(TEXT("environmental damage is unmodified"),
		Mode->ReduceDamage(100, TEXT("Fire"), Enemy, nullptr), 100);

	// --- Editing phase (teleporters disabled): no inter-pawn damage. ---
	BI->bTeleportersDisabled = true;
	TestEqual(TEXT("no pawn-instigated damage while editing"),
		Mode->ReduceDamage(100, TEXT("Fire"), Enemy, AllyA), 0);
	// but environmental (no instigator) damage still lands.
	TestEqual(TEXT("environmental damage still lands while editing"),
		Mode->ReduceDamage(100, TEXT("Fire"), Enemy, nullptr), 100);

	// --- Integration: TakeDamage routes through the game-mode reduction first. ---
	BI->bTeleportersDisabled = false;
	FWoTDamageEvent Ev;
	Ev.WoTDamageType = TEXT("Fire");
	const int32 Before = AllyB->Health;
	AllyB->TakeDamage(100.0f, Ev, AllyA->GetController(), AllyA);
	TestEqual(TEXT("a friendly hit through TakeDamage lands scaled (25)"),
		AllyB->Health, Before - 25);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTTeamAreaStartTest,
	"WoT.Battle.TeamAreaStart",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTTeamAreaStartTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTGameMode* Game = World->GetAuthGameMode<AWoTGameMode>();
	if (!Game)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	// bSpawnInTeamArea lives on the base mode; turn it on to exercise the team weighting.
	Game->bSpawnInTeamArea = true;

	// A team-0 player, unpossessed (no pawn) so the scoring is purely the team-area weighting.
	APlayerController* PC = World->SpawnActor<APlayerController>();
	PC->InitPlayerState();
	PC->GetPlayerState<AWoTPlayerState>()->Team = 0;

	auto MakeStart = [&](const FVector& Loc, uint8 TeamNum) -> AWoTPlayerStart*
	{
		AWoTPlayerStart* S = World->SpawnActor<AWoTPlayerStart>(Loc, FRotator::ZeroRotator);
		S->TeamNumber = TeamNum;
		return S;
	};

	AWoTPlayerStart* Own = MakeStart(FVector(0, 0, 100), 0);        // the player's team area
	AWoTPlayerStart* EnemyStart = MakeStart(FVector(5000, 0, 100), 1);

	// The player always spawns in their own team area (the -20000 enemy penalty dominates).
	bool bAlwaysOwn = true;
	for (int32 i = 0; i < 8; ++i)
	{
		bAlwaysOwn &= (Game->ChoosePlayerStart(PC) == Own);
	}
	TestTrue(TEXT("spawns in the player's own team area"), bAlwaysOwn);

	// With no own-team start, a neutral start (-4000) is preferred over an enemy one (-20000).
	Own->TeamNumber = 255;   // Own is now a neutral start; EnemyStart is still team 1
	bool bAlwaysNeutral = true;
	for (int32 i = 0; i < 8; ++i)
	{
		bAlwaysNeutral &= (Game->ChoosePlayerStart(PC) == Own);
	}
	TestTrue(TEXT("a neutral start is preferred over an enemy one"), bAlwaysNeutral);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// Engine.Teleporter port as placed by the Battle maps:
// URL->Tag pairing, Accept teleport semantics, the Battle editing-phase gate,
// and the no-bounce-back guard on arrival.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTTeleporterTest,
	"WoT.Battle.Teleporter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTTeleporterTest::RunTest(const FString&)
{
	UWorld* World = CreateBattleTestWorld();
	AWoTBattleGameMode* Mode = World->GetAuthGameMode<AWoTBattleGameMode>();
	if (!Mode)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	AWoTBattleInfo* BI = World->SpawnActorDeferred<AWoTBattleInfo>(
		AWoTBattleInfo::StaticClass(), FTransform::Identity);
	BI->NumTeams = 2;
	BI->TeamClass[0] = AWoTTestTeamAlpha::StaticClass();
	BI->TeamClass[1] = AWoTTestTeamBravo::StaticClass();
	BI->FinishSpawning(FTransform::Identity);
	Mode->Battle = BI;

	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTTeleporter* A = World->SpawnActor<AWoTTeleporter>(
		AWoTTeleporter::StaticClass(), FVector(0, 0, 100), FRotator::ZeroRotator, SP);
	A->URL = TEXT("side_b");
	A->TeleTag = FName(TEXT("side_a"));
	AWoTTeleporter* B = World->SpawnActor<AWoTTeleporter>(
		AWoTTeleporter::StaticClass(), FVector(5000, 0, 100), FRotator(0, 90, 0), SP);
	B->URL = TEXT("side_a");
	B->TeleTag = FName(TEXT("side_b"));

	// Editing phase: teleporters frozen (giMPBattle.CanTeleport).
	BI->bTeleportersDisabled = true;
	AWoTCharacter* P = World->SpawnActor<AWoTCharacter>(
		FVector(2500, 0, 100), FRotator::ZeroRotator, SP);
	P->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	P->SetActorLocation(A->GetActorLocation());
	WoTTest::TickWorld(World, 0.1f);
	TestTrue(TEXT("editing phase: the teleporter is frozen"),
		P->GetActorLocation().X < 2500.0f);

	// Play begins: touching A lands the pawn on B.
	P->SetActorLocation(FVector(2500, 0, 100));
	WoTTest::TickWorld(World, 0.1f);
	BI->bTeleportersDisabled = false;
	P->SetActorLocation(A->GetActorLocation());
	WoTTest::TickWorld(World, 0.1f);
	TestTrue(TEXT("teleported to the paired destination"),
		P->GetActorLocation().Equals(B->GetActorLocation(), 5.0f));

	// ...and does NOT bounce straight back from the arrival overlap.
	WoTTest::TickWorld(World, 0.3f);
	TestTrue(TEXT("no ping-pong from the arrival overlap"),
		P->GetActorLocation().Equals(B->GetActorLocation(), 5.0f));

	// Walking off and coming back re-arms the destination.
	P->SetActorLocation(FVector(2500, 0, 100));
	WoTTest::TickWorld(World, 0.1f);
	P->SetActorLocation(B->GetActorLocation());
	WoTTest::TickWorld(World, 0.1f);
	TestTrue(TEXT("return trip works after re-arm"),
		P->GetActorLocation().Equals(A->GetActorLocation(), 5.0f));

	WoTTest::DestroySpecWorld(World);
	return true;
}

// giMPBattle.AddDefaultInventory (uc:184-195): no loadout while editing;
// in play, Dart + one pick from each AngrealSet + the Air Pulse default.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBattleLoadoutTest,
	"WoT.Battle.Loadout",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTBattleLoadoutTest::RunTest(const FString&)
{
	UWorld* World = CreateBattleTestWorld();
	AWoTBattleGameMode* Mode = World->GetAuthGameMode<AWoTBattleGameMode>();
	if (!Mode)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	AWoTBattleInfo* BI = World->SpawnActorDeferred<AWoTBattleInfo>(
		AWoTBattleInfo::StaticClass(), FTransform::Identity);
	BI->NumTeams = 2;
	BI->TeamClass[0] = AWoTTestTeamAlpha::StaticClass();
	BI->TeamClass[1] = AWoTTestTeamBravo::StaticClass();
	BI->FinishSpawning(FTransform::Identity);
	Mode->Battle = BI;

	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	// Editing phase: nothing granted.
	BI->bTeleportersDisabled = true;
	AWoTCharacter* Editor = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator, SP);
	Mode->SetPlayerDefaults(Editor);
	TestEqual(TEXT("editing phase grants no loadout"),
		Editor->InventoryComponent->Items.Num(), 0);

	// Play: Dart + a Set1 pick + a Set2 pick (Shift/Fork/WallOfAir, all now ported)
	// + Air Pulse — in that retail order. Every Set2 slot grants, so always 4 items.
	BI->bTeleportersDisabled = false;
	AWoTCharacter* Player = World->SpawnActor<AWoTCharacter>(
		FVector(500, 0, 100), FRotator::ZeroRotator, SP);
	Mode->SetPlayerDefaults(Player);
	const TArray<TObjectPtr<AWoTInventory>>& Items = Player->InventoryComponent->Items;
	TestEqual(TEXT("battle loadout granted (Dart + Set1 + Set2 + Air Pulse)"),
		Items.Num(), 4);
	TestNotNull(TEXT("has the Dart"),
		Player->InventoryComponent->FindInventoryType(AWoTAngrealInvDart::StaticClass()));
	TestNotNull(TEXT("has the Air Pulse default"),
		Player->InventoryComponent->FindInventoryType(AWoTAngrealInvAirBurst::StaticClass()));
	if (Items.Num() >= 1)
	{
		// Grants PREPEND (UE1 inventory-chain semantics, Items.Insert at 0), so
		// the first-granted Dart sits at the array's tail.
		TestTrue(TEXT("Dart granted first (selection order; chain tail)"),
			Items.Last() && Items.Last()->IsA<AWoTAngrealInvDart>());
	}
	// Every granted item must come from the shipped sets.
	for (const TObjectPtr<AWoTInventory>& It : Items)
	{
		const bool bKnown = It && (It->IsA<AWoTAngrealInvDart>()
			|| It->IsA<AWoTAngrealInvAirBurst>()
			|| It->IsA<AWoTAngrealInvFireball>()
			|| It->IsA<AWoTAngrealInvLightning>()
			|| It->IsA<AWoTAngrealInvSoulBarb>()
			|| It->IsA<AWoTAngrealInvShift>()
			|| It->IsA<AWoTAngrealInvFork>()
			|| It->IsA<AWoTAngrealInvWallOfAir>());
		TestTrue(TEXT("loadout item comes from the giMPBattle sets"), bKnown);
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTEditingPhaseTest,
	"WoT.Battle.EditingPhase",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTEditingPhaseTest::RunTest(const FString&)
{
	UWorld* World = CreateBattleTestWorld();
	AWoTBattleGameMode* Mode = World->GetAuthGameMode<AWoTBattleGameMode>();
	if (!Mode)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	AWoTBattleInfo* BI = World->SpawnActorDeferred<AWoTBattleInfo>(
		AWoTBattleInfo::StaticClass(), FTransform::Identity);
	BI->NumTeams = 2;
	BI->TeamClass[0] = AWoTTestTeamAlpha::StaticClass();
	BI->TeamClass[1] = AWoTTestTeamBravo::StaticClass();
	BI->FinishSpawning(FTransform::Identity);
	Mode->Battle = BI;

	// ResetBattle establishes the editing-phase start state (the mode's own BeginPlay ran before
	// this BattleInfo existed): teleporters off, match clock frozen.
	Mode->ResetBattle();
	AWoTGameState* GS = World->GetGameState<AWoTGameState>();
	TestTrue(TEXT("editing phase starts with teleporters disabled"), BI->bTeleportersDisabled);
	TestFalse(TEXT("cannot teleport while editing"), Mode->CanTeleport());
	TestTrue(TEXT("citadels are editable"), Mode->CanEditCitadel());
	TestTrue(TEXT("the match clock is frozen while editing"), GS && GS->bStopCountDown);

	// SetEditing / IsEditing shadow a team's in-progress edit flag.
	TestTrue(TEXT("no team editing initially"),
		!Mode->IsEditing(0) && !Mode->IsEditing(1));
	Mode->SetEditing(0, true);
	TestTrue(TEXT("team 0 is now editing"), Mode->IsEditing(0));
	Mode->SetEditing(0, false);
	TestFalse(TEXT("team 0 stopped editing"), Mode->IsEditing(0));

	// Readying a team clears the "battlefield clear" flag.
	TestTrue(TEXT("battlefield clear before anyone readies"), Mode->IsBattlefieldClear());
	Mode->EnableTeamReady(0);
	TestEqual(TEXT("team 0 marked ready"), (int32)BI->bEditingComplete[0], 1);
	TestFalse(TEXT("battlefield no longer clear once a team readies"), Mode->IsBattlefieldClear());

	// EnableTeleporters ends the editing phase: teleporters on, clock resumes.
	Mode->EnableTeleporters();
	TestFalse(TEXT("teleporters enabled after editing"), BI->bTeleportersDisabled);
	TestTrue(TEXT("can teleport once play begins"), Mode->CanTeleport());
	TestFalse(TEXT("the match clock resumes"), GS && GS->bStopCountDown);

	// PickupQuery gate.
	auto MakePawn = [&](uint8 TeamNum) -> AWoTCharacter*
	{
		APlayerController* PC = World->SpawnActor<APlayerController>();
		PC->InitPlayerState();
		PC->GetPlayerState<AWoTPlayerState>()->Team = TeamNum;
		AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(FVector::ZeroVector, FRotator::ZeroRotator);
		PC->Possess(Pawn);
		return Pawn;
	};
	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	// Spawn the pickup clear of the pawns: a PRI-less pawn reads team 0
	// (pickup-ALLOWED, the retail Accessed-None convention), so a pawn spawned
	// overlapping the seal would legitimately grab it pre-possession.
	AWoTSeal* Item = World->SpawnActor<AWoTSeal>(AWoTSeal::StaticClass(),
		FVector(1000, 0, 0), FRotator::ZeroRotator, SP);

	AWoTCharacter* Member = MakePawn(0);
	AWoTCharacter* Neutral = MakePawn(255);

	// Play has begun and the player is a non-editing team member: pickups delegate to the base.
	TestTrue(TEXT("pickups allowed once play begins"), Mode->PickupQuery(Member, Item));
	// A neutral player never picks up.
	TestFalse(TEXT("neutral players get no pickups"), Mode->PickupQuery(Neutral, Item));
	// An editing team member gets no pickups.
	Mode->SetEditing(0, true);
	TestFalse(TEXT("an editing player gets no pickups"), Mode->PickupQuery(Member, Item));
	Mode->SetEditing(0, false);
	// Back in the editing phase (teleporters disabled): no pickups for anyone.
	BI->bTeleportersDisabled = true;
	TestFalse(TEXT("no pickups before play begins"), Mode->PickupQuery(Member, Item));

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBudgetTest,
	"WoT.Battle.Budget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTBudgetTest::RunTest(const FString&)
{
	UWorld* World = CreateBattleTestWorld();
	AWoTBattleGameMode* Mode = World->GetAuthGameMode<AWoTBattleGameMode>();
	if (!Mode)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	AWoTBattleInfo* BI = World->SpawnActorDeferred<AWoTBattleInfo>(
		AWoTBattleInfo::StaticClass(), FTransform::Identity);
	BI->NumTeams = 2;
	BI->TeamClass[0] = AWoTTestTeamAlpha::StaticClass();
	BI->TeamClass[1] = AWoTTestTeamBravo::StaticClass();
	BI->FinishSpawning(FTransform::Identity);
	Mode->Battle = BI;

	// A team-0 budget carrying a seal line (map data) + one other resource.
	AWoTBudgetInfo* Budget = World->SpawnActor<AWoTBudgetInfo>();
	Budget->Team = 0;
	auto AddLine = [&](const TCHAR* Adapter, int32 Count)
	{
		FWoTBudgetResource R;
		R.ResourcePackageName = TEXT("Angreal");
		R.ResourceAdapterName = Adapter;
		R.ItemCount = Count;
		Budget->Budget.Add(R);
	};
	AddLine(TEXT("SealInventory"), 0);
	AddLine(TEXT("TrapInventory"), 2);

	// GetIndex / GetItemCount / GetArrayCount.
	TestEqual(TEXT("two budget lines"), Budget->GetArrayCount(), 2);
	const int32 SealIdx = Budget->GetIndex(TEXT("SealInventory"));
	TestEqual(TEXT("the seal line is index 0"), SealIdx, 0);
	TestEqual(TEXT("an absent resource -> INDEX_NONE"), Budget->GetIndex(TEXT("Nope")), (int32)INDEX_NONE);

	// Reset seeds the seal line; Increment/Decrement move a named line.
	Budget->Reset(3);
	TestEqual(TEXT("Reset seeds the seal budget to 3"), Budget->GetItemCount(SealIdx), 3);
	Budget->Increment(TEXT("TrapInventory"));
	TestEqual(TEXT("Increment bumps the trap line to 3"),
		Budget->GetItemCount(Budget->GetIndex(TEXT("TrapInventory"))), 3);
	Budget->Decrement(TEXT("TrapInventory"));
	Budget->Decrement(TEXT("TrapInventory"));
	TestEqual(TEXT("two decrements -> 1"),
		Budget->GetItemCount(Budget->GetIndex(TEXT("TrapInventory"))), 1);

	// Edit reference count: PlayerEditLimit is 1, so a second concurrent editor is refused.
	APlayerController* Editor = World->SpawnActor<APlayerController>();
	Editor->InitPlayerState();
	Editor->GetPlayerState<AWoTPlayerState>()->Team = 0;
	TestTrue(TEXT("first editor allowed"), Budget->BeginEdit(Editor));
	TestEqual(TEXT("edit count 1"), Budget->PlayerEditCount, 1);
	TestFalse(TEXT("a second concurrent editor is refused"), Budget->BeginEdit(Editor));
	Budget->EndEdit(Editor);
	TestEqual(TEXT("edit count back to 0"), Budget->PlayerEditCount, 0);

	// PlaceSeals spends the whole seal budget onto an altar (forced banks credit the team).
	AWoTSealAltar* Altar = World->SpawnActor<AWoTSealAltar>();
	Altar->Team = 0;
	Altar->SealGoal = 4;
	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(FVector::ZeroVector, FRotator::ZeroRotator);
	Editor->Possess(Pawn);
	const int32 Placed = Budget->PlaceSeals(Altar, Editor);   // budget holds 3 seals
	TestEqual(TEXT("PlaceSeals spends the whole seal budget"), Placed, 3);
	TestEqual(TEXT("the seal budget is now empty"), Budget->GetItemCount(SealIdx), 0);
	TestEqual(TEXT("all 3 seals credit team 0"), BI->GetTeamSealCount(0), 3);

	// ResetBattle resets altars to the goal + budgets to SealBudget and destroys stray seals.
	Mode->SealBudget = 3;
	Mode->ResetBattle();
	TestEqual(TEXT("ResetBattle re-seeds the budget to SealBudget"), Budget->GetItemCount(SealIdx), 3);
	TestEqual(TEXT("ResetBattle clears the team seal count"), BI->GetTeamSealCount(0), 0);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTLockableMoverTest,
	"WoT.World.LockableMover",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTLockableMoverTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	// A bump-open door, locked, that rises 100uu when opened. Deferred so the lock
	// fields + keys are set before BeginPlay caches BaseLoc.
	const FTransform Xform(FRotator::ZeroRotator, FVector(1000, 0, 0));
	AWoTMover* Door = World->SpawnActorDeferred<AWoTMover>(
		AWoTMover::StaticClass(), Xform);
	Door->InitialState = EWoTMoverState::BumpOpenTimed;
	Door->KeyPos = { FVector::ZeroVector, FVector(0, 0, 100) };
	Door->KeyRot = { FRotator::ZeroRotator, FRotator::ZeroRotator };
	Door->MoveTime = 0.5f;
	Door->StayOpenTime = 10.0f;
	Door->BumpType = EWoTMoverBumpType::PawnBump;
	Door->bLockable = true;
	Door->TagName = TEXT("DoorKey");
	Door->FinishSpawning(Xform);
	const double BaseZ = Door->GetActorLocation().Z;

	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(1000, 0, 0), FRotator::ZeroRotator);
	UWoTInventoryComponent* Inv = Pawn->InventoryComponent;

	FHitResult Hit;
	// Keyless bump: the locked door must NOT open.
	Door->NotifyHit(nullptr, Pawn, nullptr, false, FVector::ZeroVector,
		FVector::ZeroVector, FVector::ZeroVector, Hit);
	WoTTest::TickWorld(World, 0.6f);
	TestEqual(TEXT("a locked door stays shut for a keyless pawn"),
		Door->GetActorLocation().Z, BaseZ);

	// Give the pawn the key (an item whose Event matches the door's Tag).
	const FTransform KeyXform(FRotator::ZeroRotator, FVector(1000, 0, 0));
	AWoTAngrealInvFireball* Key = World->SpawnActorDeferred<AWoTAngrealInvFireball>(
		AWoTAngrealInvFireball::StaticClass(), KeyXform);
	Key->bHeldItem = true;
	Key->EventName = TEXT("DoorKey");
	Key->FinishSpawning(KeyXform);
	Inv->AddInventory(Key);

	// Bump again with the key in hand: now it opens (rises toward +100).
	Door->NotifyHit(nullptr, Pawn, nullptr, false, FVector::ZeroVector,
		FVector::ZeroVector, FVector::ZeroVector, Hit);
	WoTTest::TickWorld(World, 0.6f);
	TestTrue(TEXT("the key-holder's bump opens the door"),
		Door->GetActorLocation().Z > BaseZ + 50.0f);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTIceTest,
	"WoT.Inventory.Ice",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTIceTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	auto IceLeeches = [](AActor* Host)
	{
		TArray<UWoTIceLeech*> A;
		Host->GetComponents<UWoTIceLeech>(A);
		return A;
	};

	AWoTCharacter* Victim = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("victim pawn spawned"), Victim))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Victim->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	WoTTest::TickWorld(World, 0.1f);

	// 1) The Ice projectile installs one IceLeech on a live pawn it hits and
	//    freezes it — movement disabled, no hit-point damage (ProcessDamage
	//    does not call Super). Spawn away from the victim so the projectile's
	//    sphere never overlaps the capsule, AlwaysSpawn to be safe.
	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTIceProjectile* Proj = World->SpawnActor<AWoTIceProjectile>(
		AWoTIceProjectile::StaticClass(), FVector(200, 0, 100),
		FRotator::ZeroRotator, SpawnParams);
	if (!TestNotNull(TEXT("ice projectile spawned"), Proj))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Proj->ProcessDamage(Victim, 0.0f, nullptr, Victim->GetActorLocation(),
		FVector::ZeroVector);

	TArray<UWoTIceLeech*> Leeches = IceLeeches(Victim);
	TestEqual(TEXT("ice projectile installed one IceLeech"), Leeches.Num(), 1);
	if (Leeches.Num() != 1)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	UWoTIceLeech* Leech = Leeches[0];
	TestEqual(TEXT("leech lifespan seeded from projectile Duration"),
		Leech->Lifespan, 7.0f);
	TestEqual(TEXT("victim movement disabled while frozen"),
		(int32)Victim->GetCharacterMovement()->MovementMode.GetValue(),
		(int32)MOVE_None);

	// 2) The freeze pins the victim: displacing it snaps back next tick.
	const FVector Frozen = Victim->GetActorLocation();
	Victim->SetActorLocation(Frozen + FVector(300, 0, 0));
	WoTTest::TickWorld(World, 0.1f);
	TestTrue(TEXT("frozen victim is pinned to OriginalLocation"),
		FVector::Dist(Victim->GetActorLocation(), Frozen) < 1.0f);

	// 3) bRemoveExisting: a second freeze replaces the first (still one leech).
	Proj->ProcessDamage(Victim, 0.0f, nullptr, Victim->GetActorLocation(),
		FVector::ZeroVector);
	TArray<UWoTIceLeech*> Refrozen = IceLeeches(Victim);
	TestEqual(TEXT("re-freeze leaves exactly one IceLeech (bRemoveExisting)"),
		Refrozen.Num(), 1);
	if (Refrozen.Num() != 1)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	// 4) On expiry the victim thaws — leech gone, movement restored.
	Refrozen[0]->Lifespan = 0.15f;
	WoTTest::TickWorld(World, 0.25f);
	TestEqual(TEXT("leech detached on expiry"), IceLeeches(Victim).Num(), 0);
	TestEqual(TEXT("victim movement restored after thaw"),
		(int32)Victim->GetCharacterMovement()->MovementMode.GetValue(),
		(int32)MOVE_Flying);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTDecayTest,
	"WoT.Inventory.Decay",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTDecayTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	auto ChargeLeeches = [](AActor* Host)
	{
		TArray<UWoTDecreaseChargesLeech*> A;
		Host->GetComponents<UWoTDecreaseChargesLeech>(A);
		return A;
	};

	AWoTCharacter* Victim = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("victim pawn spawned"), Victim))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Victim->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	UWoTInventoryComponent* Inv = Victim->InventoryComponent;

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	// Give the victim one artifact of each rarity (Fireball=common, Ice=uncommon,
	// Decay=rare) to prove per-rarity draining.
	AWoTAngrealInvFireball* Common = World->SpawnActor<AWoTAngrealInvFireball>(
		AWoTAngrealInvFireball::StaticClass(), FVector(400, 0, 100),
		FRotator::ZeroRotator, SpawnParams);
	AWoTAngrealInvIce* Uncommon = World->SpawnActor<AWoTAngrealInvIce>(
		AWoTAngrealInvIce::StaticClass(), FVector(450, 0, 100),
		FRotator::ZeroRotator, SpawnParams);
	AWoTAngrealInvDecay* Rare = World->SpawnActor<AWoTAngrealInvDecay>(
		AWoTAngrealInvDecay::StaticClass(), FVector(500, 0, 100),
		FRotator::ZeroRotator, SpawnParams);
	if (!Common || !Uncommon || !Rare)
	{
		TestTrue(TEXT("all three victim artifacts spawned"), false);
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	WoTTest::TickWorld(World, 0.1f);
	Common->CurCharges = 10;
	Uncommon->CurCharges = 10;
	Rare->CurCharges = 10;
	Inv->Items.Add(Common);
	Inv->Items.Add(Uncommon);
	Inv->Items.Add(Rare);

	TestTrue(TEXT("fireball is common"), Common->bCommon);
	TestTrue(TEXT("ice is uncommon"), Uncommon->bUncommon);
	TestTrue(TEXT("decay is rare"), Rare->bRare);

	// The Decay projectile installs the three charge-drain leeches on the victim.
	AWoTDecayProjectile* Proj = World->SpawnActor<AWoTDecayProjectile>(
		AWoTDecayProjectile::StaticClass(), FVector(200, 0, 100),
		FRotator::ZeroRotator, SpawnParams);
	if (!TestNotNull(TEXT("decay projectile spawned"), Proj))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Proj->ProcessDamage(Victim, 0.0f, nullptr, Victim->GetActorLocation(),
		FVector::ZeroVector);
	TArray<UWoTDecreaseChargesLeech*> Leeches = ChargeLeeches(Victim);
	TestEqual(TEXT("decay installed the three charge-drain leeches"),
		Leeches.Num(), 3);
	if (Leeches.Num() != 3)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	// Decay's 4th leech drains the victim's health over time.
	TArray<UWoTDecreaseHealthLeech*> HealthLeeches;
	Victim->GetComponents<UWoTDecreaseHealthLeech>(HealthLeeches);
	TestEqual(TEXT("decay also installs a health-drain leech"), HealthLeeches.Num(), 1);
	// Projectile-installed payloads are marked from-projectile (so they can't be
	// Swap-transferred / cleansed as non-projectile weaves).
	if (HealthLeeches.Num() == 1)
	{
		TestTrue(TEXT("decay's payload leech is marked from-projectile"),
			HealthLeeches[0]->bFromProjectile);
	}
	const int32 VictimHP0 = Victim->Health;

	// Speed all three up so each fires once, then verify each drains ONLY its
	// own rarity by exactly one charge (isolation: a wrong-rarity leech would
	// drop an artifact by 2).
	for (UWoTDecreaseChargesLeech* L : Leeches)
	{
		L->AffectResolution = 0.1f;
	}
	WoTTest::TickWorld(World, 0.15f);
	TestEqual(TEXT("common artifact lost exactly one charge"), Common->CurCharges, 9);
	TestEqual(TEXT("uncommon artifact lost exactly one charge"), Uncommon->CurCharges, 9);
	TestEqual(TEXT("rare artifact lost exactly one charge"), Rare->CurCharges, 9);

	// Lifespan expiry detaches all three (Expired -> Unattach).
	for (UWoTDecreaseChargesLeech* L : ChargeLeeches(Victim))
	{
		L->Lifespan = 0.15f;
	}
	WoTTest::TickWorld(World, 0.2f);
	TestEqual(TEXT("charge leeches detached on expiry"),
		ChargeLeeches(Victim).Num(), 0);

	// Over that same ticking the health leech (0.2s) took hit points off.
	TestTrue(FString::Printf(TEXT("decay's health leech drained the victim (%d -> %d)"),
		VictimHP0, Victim->Health), Victim->Health < VictimHP0);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSoulBarbTest,
	"WoT.Inventory.SoulBarb",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTSoulBarbTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTSoulBarbProjectile* Proj = World->SpawnActor<AWoTSoulBarbProjectile>(
		AWoTSoulBarbProjectile::StaticClass(), FVector(0, 0, 100),
		FRotator::ZeroRotator, SpawnParams);
	if (!TestNotNull(TEXT("soul barb projectile spawned"), Proj))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	// PHYS_Projectile flight: velocity = rotation * Speed (300).
	const FVector Start = Proj->GetActorLocation();
	WoTTest::TickWorld(World, 0.25f);
	const float Travelled = FVector::Dist(Start, Proj->GetActorLocation());
	TestTrue(FString::Printf(TEXT("soul barb travels at Speed (moved %.0f)"), Travelled),
		Travelled > 40.0f && Travelled < 120.0f);

	// Hitting a pawn installs the RarityBasedDamageReflector on it (via Explode,
	// driven through ProcessTouch which sets HitActor).
	AWoTCharacter* Victim = World->SpawnActor<AWoTCharacter>(
		FVector(500, 0, 100), FRotator::ZeroRotator);
	Victim->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	Proj->ProcessTouch(Victim, Victim->GetActorLocation());
	TArray<UWoTRarityBasedDamageReflector*> Refs;
	Victim->GetComponents<UWoTRarityBasedDamageReflector>(Refs);
	TestEqual(TEXT("soul barb installs the rarity-damage reflector on the victim"),
		Refs.Num(), 1);

	// Now the victim takes rarity-scaled self-damage whenever it uses a charge.
	Victim->Health = 100;
	AWoTAngrealInvDecay* Rare = World->SpawnActor<AWoTAngrealInvDecay>(
		AWoTAngrealInvDecay::StaticClass(), FVector(520, 0, 100),
		FRotator::ZeroRotator, SpawnParams);   // bRare -> TaintRareDamage 20
	Rare->CurCharges = 10;
	Rare->SetOwner(Victim);
	Rare->UseCharge();
	TestEqual(TEXT("using a rare artifact while soul-barbed self-damages by 20"),
		Victim->Health, 80);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTDartTest,
	"WoT.Inventory.Dart",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTDartTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	auto CountDarts = [World]()
	{
		int32 N = 0;
		for (TActorIterator<AWoTDartProjectile> It(World); It; ++It)
		{
			++N;
		}
		return N;
	};

	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	UWoTInventoryComponent* Inv = Pawn->InventoryComponent;

	AWoTAngrealInvDart* Ground = World->SpawnActor<AWoTAngrealInvDart>(
		FVector(500, 0, 100), FRotator::ZeroRotator);
	WoTTest::TickWorld(World, 0.1f);
	Pawn->SetActorLocation(Ground->GetActorLocation());
	WoTTest::TickWorld(World, 0.1f);

	AWoTAngrealInvDart* Held = Cast<AWoTAngrealInvDart>(Inv->SelectedItem.Get());
	if (!TestNotNull(TEXT("dart picked up and selected"), Held))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	TestTrue(TEXT("dart is an auto-firing launcher"), Held->bAutoFire);
	// Direct-damage projectile (first port to keep the base ProcessDamage path):
	// verify the CDO carries Dart's damage numbers.
	const AWoTDartProjectile* DartCDO =
		Held->ProjectileClass->GetDefaultObject<AWoTDartProjectile>();
	TestNotNull(TEXT("projectile class is the dart projectile"), DartCDO);
	if (DartCDO)
	{
		TestEqual(TEXT("dart deals Damage=10"), DartCDO->Damage, 10.0f);
		TestEqual(TEXT("dart DamageType is AxFxx"), DartCDO->DamageType, FName("AxFxx"));
	}

	// A single fire-press engages auto-fire: the first cast launches one dart and
	// uses one charge immediately, and HOLDING (no cease) sprays more via the
	// launcher's Tick at 320 RPM. Re-pressing would hit the component's per-cast
	// gate, so drive it as one held press.
	const int32 ChargesBefore = Held->CurCharges;
	Inv->UseAngreal();
	TestEqual(TEXT("first cast fired one dart"), CountDarts(), 1);
	TestEqual(TEXT("first cast used one charge"), Held->CurCharges, ChargesBefore - 1);

	AWoTDartProjectile* Dart = nullptr;
	for (TActorIterator<AWoTDartProjectile> It(World); It; ++It)
	{
		Dart = *It;
	}
	const FVector Start = IsValid(Dart) ? Dart->GetActorLocation() : FVector::ZeroVector;

	WoTTest::TickWorld(World, 0.5f);   // keep holding — spray + flight
	Inv->CeaseUsingAngreal();

	TestTrue(FString::Printf(TEXT("auto-fire sprayed more darts while held (now %d)"),
		CountDarts()), CountDarts() > 1);
	if (IsValid(Dart))
	{
		const float Travelled = FVector::Dist(Start, Dart->GetActorLocation());
		TestTrue(FString::Printf(TEXT("dart flies at Speed 2000 (moved %.0f over 0.5s)"), Travelled),
			Travelled > 400.0f);
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTExpWardTest,
	"WoT.Inventory.ExpWard",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTExpWardTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	// A wall for the ward to stick to: a WorldStatic-blocking box at x=100 that
	// the ward's forward (+X) placement trace will hit.
	auto SpawnWall = [&](const FVector& At)
	{
		AStaticMeshActor* Wall = World->SpawnActor<AStaticMeshActor>(
			At, FRotator::ZeroRotator, SpawnParams);
		UBoxComponent* Box = NewObject<UBoxComponent>(Wall);
		Box->SetupAttachment(Wall->GetRootComponent());
		Box->InitBoxExtent(FVector(50, 200, 200));
		Box->SetCollisionProfileName(TEXT("BlockAll"));
		Box->RegisterComponent();
		return Wall;
	};

	auto SpawnWard = [&](const FVector& At)
	{
		return World->SpawnActor<AWoTExpWardProjectile>(
			AWoTExpWardProjectile::StaticClass(), At, FRotator::ZeroRotator, SpawnParams);
	};

	SpawnWall(FVector(100, 0, 100));

	// 1) Placement: the ward traces to the wall and sticks; CreationSucceeded.
	AWoTExpWardProjectile* Ward = SpawnWard(FVector(0, 0, 100));
	if (!TestNotNull(TEXT("ward spawned"), Ward))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	TestTrue(TEXT("ward found the surface and placed"), Ward->bCorrectlyPlaced);
	TestTrue(TEXT("CreationSucceeded reports the placement"), Ward->CreationSucceeded());
	TestTrue(TEXT("ward stuck near the wall (x within trace range)"),
		Ward->GetActorLocation().X > 40.0f && Ward->GetActorLocation().X < 60.0f);

	// 2) Inert during the activation delay.
	TestFalse(TEXT("ward is not armed immediately"), Ward->bArmed);
	WoTTest::TickWorld(World, 0.5f);
	TestFalse(TEXT("ward still inert before ActivationTime (1s)"), Ward->bArmed);

	// 3) Arms after ActivationTime.
	WoTTest::TickWorld(World, 0.7f);   // total ~1.2s > 1.0s
	TestTrue(TEXT("ward armed after ActivationTime"), Ward->bArmed);

	// 4) An armed ward detonates when shot (TakeDamage) — destroyed.
	Ward->TakeDamage(10.0f, FDamageEvent(), nullptr, nullptr);
	TestFalse(TEXT("shot ward detonated (destroyed)"), IsValid(Ward));

	// 5) Auto-detonation: an armed ward blows after its LifeSpan expires.
	SpawnWall(FVector(400, 0, 100));
	AWoTExpWardProjectile* Ward2 = SpawnWard(FVector(300, 0, 100));
	if (TestNotNull(TEXT("second ward spawned"), Ward2))
	{
		WoTTest::TickWorld(World, 1.2f);   // arm it
		TestTrue(TEXT("second ward armed"), Ward2->bArmed);
		Ward2->SetLifeSpan(0.2f);
		WoTTest::TickWorld(World, 0.35f);
		TestFalse(TEXT("ward auto-detonated after its lifespan"), IsValid(Ward2));
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTExpWardVisualFlowSpec,
	"WoT.Inventory.ExpWardVisualFlow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTExpWardVisualFlowSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTExpWardProjectile* Ward = World->SpawnActor<AWoTExpWardProjectile>(
		AWoTExpWardProjectile::StaticClass(), FVector(0.0f, 0.0f, 100.0f),
		FRotator::ZeroRotator, SpawnParams);
	if (!TestNotNull(TEXT("ward spawned for visual-state flow"), Ward))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	UWoTMapLightTable* Table = NewObject<UWoTMapLightTable>(World);
	UWoTMeshLight* MeshLight = UWoTMeshLight::AttachRuntime(Ward, Table);
	if (!TestNotNull(TEXT("ward runtime mesh-light component"), MeshLight))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	// AngrealExpWardProjectile.uc:176-226: Waiting is visible, FadeOut reaches
	// zero, and FadeIn immediately shows the rune as its glow rises again.
	const FProperty* ArmedProperty = FindFProperty<FProperty>(
		AWoTExpWardProjectile::StaticClass(), TEXT("bArmed"));
	const FProperty* VisualPhaseProperty = FindFProperty<FProperty>(
		AWoTExpWardProjectile::StaticClass(), TEXT("VisualPhase"));
	TestTrue(TEXT("armed state replicates"),
		ArmedProperty && ArmedProperty->HasAnyPropertyFlags(CPF_Net));
	TestTrue(TEXT("visual phase replicates"),
		VisualPhaseProperty && VisualPhaseProperty->HasAnyPropertyFlags(CPF_Net));
	TestEqual(TEXT("arming wait exposes the Waiting phase"), Ward->VisualPhase,
		EWoTExpWardVisualPhase::Waiting);
	TestFalse(TEXT("ward is visible while arming"), Ward->IsHidden());
	TestTrue(TEXT("waiting MLAmbient starts at GUnlitColor 0.5"),
		MeshLight->Ambient.Equals(FLinearColor(0.5f, 0.5f, 0.5f, 1.0f),
			KINDA_SMALL_NUMBER));

	Ward->Activate();
	TestTrue(TEXT("activation marks the ward armed"), Ward->bArmed);
	TestEqual(TEXT("armed ward enters FadeOut"), Ward->VisualPhase,
		EWoTExpWardVisualPhase::FadeOut);
	Ward->Tick(1.0f);
	TestEqual(TEXT("FadeOut endpoint advances to FadeIn"), Ward->VisualPhase,
		EWoTExpWardVisualPhase::FadeIn);
	TestEqual(TEXT("FadeOut endpoint pins ScaleGlow to zero"), Ward->ScaleGlow, 0.0f);
	TestTrue(TEXT("armed ward is hidden at the FadeOut endpoint"), Ward->IsHidden());
	TestTrue(TEXT("FadeOut endpoint pins MLAmbient to black"),
		MeshLight->Ambient.Equals(FLinearColor(0.0f, 0.0f, 0.0f, 1.0f),
			KINDA_SMALL_NUMBER));

	Ward->Tick(0.25f);
	TestFalse(TEXT("FadeIn re-shows the armed ward"), Ward->IsHidden());
	TestEqual(TEXT("FadeIn advances ScaleGlow at one unit per second"),
		Ward->ScaleGlow, 0.25f);
	TestTrue(TEXT("FadeIn advances MLAmbient with ScaleGlow*0.5"),
		MeshLight->Ambient.Equals(FLinearColor(0.125f, 0.125f, 0.125f, 1.0f),
			KINDA_SMALL_NUMBER));

	Ward->Tick(0.75f);
	TestEqual(TEXT("FadeIn endpoint returns to FadeOut"), Ward->VisualPhase,
		EWoTExpWardVisualPhase::FadeOut);
	TestEqual(TEXT("FadeIn endpoint pins ScaleGlow to its default"),
		Ward->ScaleGlow, 1.0f);
	TestTrue(TEXT("FadeIn endpoint restores MLAmbient to 0.5"),
		MeshLight->Ambient.Equals(FLinearColor(0.5f, 0.5f, 0.5f, 1.0f),
			KINDA_SMALL_NUMBER));

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFireworksTest,
	"WoT.Inventory.Fireworks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTFireworksTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	// Deferred spawn so the fuse bounds (MinExpTime/MaxExpTime, read in BeginPlay)
	// can be set first — mirrors the SpawnPool test.
	auto SpawnFirework = [&](const FVector& At, float Fuse)
	{
		const FTransform Xform(FRotator::ZeroRotator, At);
		AWoTFireworksProjectile* F = World->SpawnActorDeferred<AWoTFireworksProjectile>(
			AWoTFireworksProjectile::StaticClass(), Xform);
		F->MinExpTime = Fuse;
		F->MaxExpTime = Fuse;
		F->FinishSpawning(Xform);
		return F;
	};

	// 1) It flies forward at Speed and falls under gravity (PHYS_Falling). Long
	//    fuse so it doesn't detonate during the check.
	AWoTFireworksProjectile* Flyer = SpawnFirework(FVector(0, 0, 500), 10.0f);
	if (!TestNotNull(TEXT("firework spawned"), Flyer))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	const FVector Start = Flyer->GetActorLocation();
	WoTTest::TickWorld(World, 0.2f);
	const FVector Now = Flyer->GetActorLocation();
	TestTrue(FString::Printf(TEXT("firework flies forward (dx %.0f)"), Now.X - Start.X),
		Now.X - Start.X > 100.0f);
	TestTrue(TEXT("firework falls under gravity (z decreased)"), Now.Z < Start.Z);

	// 2) It detonates on its fuse timer.
	TWeakObjectPtr<AWoTFireworksProjectile> FusePtr = SpawnFirework(FVector(300, 0, 500), 0.2f);
	TestTrue(TEXT("fuse firework spawned"), FusePtr.IsValid());
	WoTTest::TickWorld(World, 0.35f);
	TestFalse(TEXT("firework detonated when its fuse expired"), FusePtr.IsValid());

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTLevitateTest,
	"WoT.Inventory.Levitate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTLevitateTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	// A floor with its top at z=0 for the hover trace.
	AStaticMeshActor* Floor = World->SpawnActor<AStaticMeshActor>(
		FVector(0, 0, -50), FRotator::ZeroRotator, SpawnParams);
	UBoxComponent* FloorBox = NewObject<UBoxComponent>(Floor);
	FloorBox->SetupAttachment(Floor->GetRootComponent());
	FloorBox->InitBoxExtent(FVector(1000, 1000, 50));
	FloorBox->SetCollisionProfileName(TEXT("BlockAll"));
	FloorBox->RegisterComponent();

	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 90), FRotator::ZeroRotator);
	UCharacterMovementComponent* Move = Pawn->GetCharacterMovement();
	Move->SetMovementMode(MOVE_Walking);
	UWoTInventoryComponent* Inv = Pawn->InventoryComponent;

	// Pick up the Levitate ter'angreal.
	AWoTAngrealInvLevitate* Ground = World->SpawnActor<AWoTAngrealInvLevitate>(
		FVector(0, 0, 90), FRotator::ZeroRotator);
	WoTTest::TickWorld(World, 0.1f);
	Pawn->SetActorLocation(Ground->GetActorLocation());
	WoTTest::TickWorld(World, 0.1f);
	AWoTAngrealInvLevitate* Held = Cast<AWoTAngrealInvLevitate>(Inv->SelectedItem.Get());
	if (!TestNotNull(TEXT("levitate picked up and selected"), Held))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	const TEnumAsByte<EMovementMode> InitMode = Move->MovementMode;

	// 1) Casting floats the caster (MOVE_Flying) once its Tick runs.
	Inv->UseAngreal();
	WoTTest::TickWorld(World, 0.05f);
	TestEqual(TEXT("levitating switches the caster to flying"),
		(int32)Move->MovementMode.GetValue(), (int32)MOVE_Flying);
	TestTrue(TEXT("levitate cast adds its good status icon"),
		Pawn->StatusIcons.Num() == 1 && Pawn->StatusIcons[0].bGood &&
		Pawn->StatusIcons[0].IconID == Held->GetFName());

	// 2) The hover drives vertical velocity toward RiseHeight above the floor:
	//    above it descends, below it rises. Direct Tick keeps this deterministic.
	Pawn->SetActorLocation(FVector(0, 0, 400));
	Held->Tick(0.016f);
	TestTrue(TEXT("above hover height -> descends (Velocity.Z < 0)"), Move->Velocity.Z < 0.0f);
	Pawn->SetActorLocation(FVector(0, 0, 40));
	Held->Tick(0.016f);
	TestTrue(TEXT("below hover height -> rises (Velocity.Z > 0)"), Move->Velocity.Z > 0.0f);

	// 3) It drains charges over time (RoundsPerMinute 60 -> ~1/s).
	const int32 Before = Held->CurCharges;
	WoTTest::TickWorld(World, 1.1f);
	const int32 Drained = Before - Held->CurCharges;
	TestTrue(FString::Printf(TEXT("levitate drains charges over time (drained %d)"), Drained),
		Drained >= 1 && Drained <= 2);

	// 4) Releasing restores the caster's movement mode.
	Inv->CeaseUsingAngreal();
	TestEqual(TEXT("release restores the caster's movement mode"),
		(int32)Move->MovementMode.GetValue(), (int32)InitMode.GetValue());
	TestEqual(TEXT("levitate release removes its status icon"),
		Pawn->StatusIcons.Num(), 0);

	// 5) Running out of charges auto-releases (UseCharge -> GoEmpty -> UnCast).
	Held->CurCharges = 2;
	Inv->UseAngreal();
	WoTTest::TickWorld(World, 0.05f);
	TestEqual(TEXT("re-cast floats again"),
		(int32)Move->MovementMode.GetValue(), (int32)MOVE_Flying);
	// Levitate carries its partial drain interval across releases, so allow the
	// retained remainder plus both final one-second charge periods to elapse.
	WoTTest::TickWorld(World, 2.2f);
	TestNotEqual(TEXT("emptied levitate auto-released (no longer flying)"),
		(int32)Move->MovementMode.GetValue(), (int32)MOVE_Flying);
	TestTrue(TEXT("levitate charges emptied"), Held->CurCharges <= 0);
	TestEqual(TEXT("empty auto-release removes the levitate icon"),
		Pawn->StatusIcons.Num(), 0);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTWhirlwindTest,
	"WoT.Inventory.Whirlwind",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTWhirlwindTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	AWoTCharacter* Caster = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator);   // faces +X
	Caster->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	UWoTInventoryComponent* Inv = Caster->InventoryComponent;

	AWoTAngrealInvWhirlwind* Ground = World->SpawnActor<AWoTAngrealInvWhirlwind>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	WoTTest::TickWorld(World, 0.1f);
	Caster->SetActorLocation(Ground->GetActorLocation());
	WoTTest::TickWorld(World, 0.1f);
	AWoTAngrealInvWhirlwind* Held = Cast<AWoTAngrealInvWhirlwind>(Inv->SelectedItem.Get());
	if (!TestNotNull(TEXT("whirlwind picked up and selected"), Held))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	// 1) With no target in view the cast fails — nobody gets whirlwinded.
	const int32 ChargesAtStart = Held->CurCharges;
	Inv->UseAngreal();
	TestFalse(TEXT("no-target cast whirlwinds nobody"),
		AWoTAngrealInvWhirlwind::PlayerIsBeingWhirlwinded(Caster));
	Inv->CeaseUsingAngreal();

	// A target directly ahead (within the 22.5 FOV, line of sight clear).
	AWoTCharacter* Target = World->SpawnActor<AWoTCharacter>(
		FVector(400, 0, 100), FRotator::ZeroRotator);
	UCharacterMovementComponent* TMove = Target->GetCharacterMovement();
	TMove->SetMovementMode(MOVE_Flying);
	WoTTest::TickWorld(World, 0.1f);

	// 2) Casting whirlwinds the target: leech installed, movement seized.
	Inv->UseAngreal();
	TestTrue(TEXT("casting whirlwinds the target ahead"),
		AWoTAngrealInvWhirlwind::PlayerIsBeingWhirlwinded(Target));
	TestEqual(TEXT("whirlwinded target's movement is seized"),
		(int32)TMove->MovementMode.GetValue(), (int32)MOVE_None);

	// 3) Pointing up lifts the victim off the ground (steer point rises).
	Caster->SetActorRotation(FRotator(45.0f, 0.0f, 0.0f));   // pitch up
	const float TargetZ0 = Target->GetActorLocation().Z;
	WoTTest::TickWorld(World, 0.3f);
	TestTrue(FString::Printf(TEXT("victim is lifted when pointed up (z %.0f -> %.0f)"),
		TargetZ0, Target->GetActorLocation().Z),
		Target->GetActorLocation().Z > TargetZ0 + 10.0f);

	// 4) It drains charges while held (RoundsPerMinute 20 -> ~1 per 3s).
	const int32 Before = Held->CurCharges;
	WoTTest::TickWorld(World, 3.2f);
	TestTrue(FString::Printf(TEXT("whirlwind drains charges while held (drained %d)"),
		Before - Held->CurCharges), Held->CurCharges < Before);
	(void)ChargesAtStart;

	// 5) Releasing frees the victim (leech gone, movement restored).
	Inv->CeaseUsingAngreal();
	TestFalse(TEXT("release ends the whirlwind"),
		AWoTAngrealInvWhirlwind::PlayerIsBeingWhirlwinded(Target));
	TestNotEqual(TEXT("freed victim's movement restored"),
		(int32)TMove->MovementMode.GetValue(), (int32)MOVE_None);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTWallOfAirTest,
	"WoT.Inventory.WallOfAir",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTWallOfAirTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AWoTCharacter* Caster = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator);   // faces +X
	Caster->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	UWoTInventoryComponent* Inv = Caster->InventoryComponent;

	// Pick up + select the Sever ter'angreal.
	AWoTAngrealInvWallOfAir* Ground = World->SpawnActor<AWoTAngrealInvWallOfAir>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	WoTTest::TickWorld(World, 0.1f);
	Caster->SetActorLocation(Ground->GetActorLocation());
	WoTTest::TickWorld(World, 0.1f);
	AWoTAngrealInvWallOfAir* Held = Cast<AWoTAngrealInvWallOfAir>(Inv->SelectedItem.Get());
	if (!TestNotNull(TEXT("wall of air picked up and selected"), Held))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	auto FindWall = [&]() -> AWoTWallOfAirProjectile*
	{
		TActorIterator<AWoTWallOfAirProjectile> It(World);
		return It ? *It : nullptr;
	};

	// 1) No target in view -> the cast fails: no wall, no charge spent.
	const int32 ChargesFull = Held->CurCharges;
	Inv->UseAngreal();
	TestNull(TEXT("no-target cast plants no wall"), FindWall());
	TestEqual(TEXT("failed cast spends no charge"), Held->CurCharges, ChargesFull);
	Inv->CeaseUsingAngreal();

	// A living target far ahead (within the 22.5 cone, line of sight clear).
	AWoTCharacter* Target = World->SpawnActor<AWoTCharacter>(
		FVector(900, 0, 100), FRotator::ZeroRotator);
	Target->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	WoTTest::TickWorld(World, 0.1f);

	// 2) Casting plants the wall a third of the way to the target, spending a charge.
	Inv->UseAngreal();
	AWoTWallOfAirProjectile* Wall = FindWall();
	if (!TestNotNull(TEXT("casting plants the wall"), Wall))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	TestTrue(TEXT("cast spends the initial charge"), Held->CurCharges < ChargesFull);
	WoTTest::TickWorld(World, 0.1f);   // its authority Tick re-plants it to the 1/3 point
	// PctDistFromSource 1/3 of (0..900) on X == 300.
	TestTrue(FString::Printf(TEXT("wall sits a third of the way to the target (x=%.0f)"),
		Wall->GetActorLocation().X),
		FMath::Abs(Wall->GetActorLocation().X - 300.0f) < 30.0f);

	// 3) It unravels a weave that strikes it: a Dart driven into the wall is destroyed.
	AWoTDartProjectile* Weave = World->SpawnActor<AWoTDartProjectile>(
		AWoTDartProjectile::StaticClass(), FVector(150, 0, 100), FRotator::ZeroRotator, SP);
	TWeakObjectPtr<AWoTDartProjectile> WeavePtr = Weave;
	WoTTest::TickWorld(World, 0.25f);   // it flies +X into the wall
	TestFalse(TEXT("wall unravels the weave that strikes it"), WeavePtr.IsValid());

	// 4) A non-angreal projectile is knocked to the ground instead of unravelled.
	AWoTTestKnockdownProjectile* Phys = World->SpawnActor<AWoTTestKnockdownProjectile>(
		AWoTTestKnockdownProjectile::StaticClass(), FVector(150, 0, 100), FRotator::ZeroRotator, SP);
	Phys->ProjectileMovement->Velocity = FVector(2000, 0, 0);   // drive it at the wall
	WoTTest::TickWorld(World, 0.25f);
	if (TestTrue(TEXT("non-angreal projectile survives the wall"), IsValid(Phys)))
	{
		TestTrue(TEXT("wall knocks the projectile downward (velocity.z < 0)"),
			Phys->ProjectileMovement->Velocity.Z < 0.0f);
		TestTrue(TEXT("wall gives the projectile gravity to fall"),
			Phys->ProjectileMovement->ProjectileGravityScale > 0.0f);
	}

	// 5) It drains charges while held (RoundsPerMinute 40 -> a charge every 1.5s).
	const int32 BeforeDrain = Held->CurCharges;
	WoTTest::TickWorld(World, 3.2f);
	TestTrue(FString::Printf(TEXT("wall drains charges while held (drained %d)"),
		BeforeDrain - Held->CurCharges), Held->CurCharges < BeforeDrain);

	// 6) Releasing removes the wall.
	Inv->CeaseUsingAngreal();
	TestNull(TEXT("release unravels the wall"), FindWall());

	// 7) The wall dies with its target: re-cast, then kill the target.
	Inv->UseAngreal();
	if (TestNotNull(TEXT("re-cast plants a fresh wall"), FindWall()))
	{
		Target->Health = 0;   // the target dies
		WoTTest::TickWorld(World, 0.1f);
		TestNull(TEXT("the target's death unravels the wall"), FindWall());
	}
	Inv->CeaseUsingAngreal();

	// 8) Running out of charges auto-releases the wall (UseCharge -> GoEmpty -> UnCast).
	Target->Health = 100;   // revive so it can be targeted again
	Inv->UseAngreal();
	if (TestNotNull(TEXT("cast before emptying plants a wall"), FindWall()))
	{
		Held->CurCharges = 2;
		// RoundsPerMinute 40 -> 1.5s/charge; tick past two drains regardless of the
		// carried-over CastTimer phase.
		WoTTest::TickWorld(World, 5.0f);
		TestNull(TEXT("emptying the charges unravels the wall"), FindWall());
		TestTrue(TEXT("wall of air charges emptied"), Held->CurCharges <= 0);
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSpecialTest,
	"WoT.Inventory.Special",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTSpecialTest::RunTest(const FString&)
{
	// The "Unknown" stub: costs nothing, does nothing, can't be dropped.
	UWorld* World = WoTTest::CreateSpecWorld();

	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	UWoTInventoryComponent* Inv = Pawn->InventoryComponent;

	AWoTAngrealInvSpecial* Ground = World->SpawnActor<AWoTAngrealInvSpecial>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	WoTTest::TickWorld(World, 0.1f);
	Pawn->SetActorLocation(Ground->GetActorLocation());
	WoTTest::TickWorld(World, 0.1f);
	AWoTAngrealInvSpecial* Held = Cast<AWoTAngrealInvSpecial>(Inv->SelectedItem.Get());
	if (!TestNotNull(TEXT("special picked up and selected"), Held))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	TestEqual(TEXT("special costs no charge"), Held->ChargeCost, 0);

	// Can't be dropped (empty DropFrom) — stays held.
	Held->DropFrom(FVector::ZeroVector);
	TestTrue(TEXT("special can't be dropped (still held)"),
		Inv->SelectedItem.Get() == Held && Held->GetOwner() == Pawn);

	// Casting is a harmless no-op (no crash, no charge change).
	const int32 Charges = Held->CurCharges;
	Inv->UseAngreal();
	Inv->CeaseUsingAngreal();
	TestEqual(TEXT("casting the stub changes nothing"), Held->CurCharges, Charges);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHealthTest,
	"WoT.Combat.Health",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTHealthTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);

	// 1) Starts at full health, alive.
	TestEqual(TEXT("pawn starts at 100 health"), Pawn->Health, 100);
	TestTrue(TEXT("pawn starts alive"), Pawn->IsAlive());

	// 2) Damage reduces health by the (unmodified) amount; still alive.
	auto Hurt = [&](int32 Amount)
	{
		FWoTDamageEvent Ev;
		Ev.WoTDamageType = TEXT("xxFxx");
		Ev.Damage = Amount;
		Pawn->TakeDamage((float)Amount, Ev, nullptr, nullptr);
	};
	Hurt(30);
	TestEqual(TEXT("30 damage -> 70 health"), Pawn->Health, 70);
	TestTrue(TEXT("still alive at 70"), Pawn->IsAlive());

	// 3) Healing clamps to the cap and never overheals.
	Pawn->IncreaseHealth(20);
	TestEqual(TEXT("heal 20 -> 90"), Pawn->Health, 90);
	Pawn->IncreaseHealth(50);
	TestEqual(TEXT("heal clamps to MaxHealth (100)"), Pawn->Health, 100);

	// 4) A projectile drives the same path end-to-end (Dart Damage 10).
	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTDartProjectile* Dart = World->SpawnActor<AWoTDartProjectile>(
		AWoTDartProjectile::StaticClass(), FVector(300, 0, 100), FRotator::ZeroRotator, SP);
	if (TestNotNull(TEXT("dart spawned"), Dart))
	{
		Dart->ProcessDamage(Pawn, 10.0f, nullptr, Pawn->GetActorLocation(), FVector::ZeroVector);
		TestEqual(TEXT("dart hit knocked off 10 health via TakeDamage"), Pawn->Health, 90);
	}

	// 5) Lethal damage kills: not alive, flagged dead, collision off.
	Hurt(200);
	TestTrue(TEXT("lethal damage drops health to <=0"), Pawn->Health <= 0);
	TestFalse(TEXT("dead pawn is not alive"), Pawn->IsAlive());
	TestTrue(TEXT("dead pawn flagged bDead"), Pawn->bDead);
	TestEqual(TEXT("dead pawn's collision disabled"),
		(int32)Pawn->GetCapsuleComponent()->GetCollisionEnabled(),
		(int32)ECollisionEnabled::NoCollision);

	// 6) The dead don't heal (DefaultHealthReflector only heals the living).
	const int32 DeadHealth = Pawn->Health;
	Pawn->IncreaseHealth(50);
	TestEqual(TEXT("healing a corpse does nothing"), Pawn->Health, DeadHealth);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHealTest,
	"WoT.Inventory.Heal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTHealTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	UWoTInventoryComponent* Inv = Pawn->InventoryComponent;

	AWoTAngrealInvHeal* Ground = World->SpawnActor<AWoTAngrealInvHeal>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	WoTTest::TickWorld(World, 0.1f);
	Pawn->SetActorLocation(Ground->GetActorLocation());
	WoTTest::TickWorld(World, 0.1f);
	AWoTAngrealInvHeal* Held = Cast<AWoTAngrealInvHeal>(Inv->SelectedItem.Get());
	if (!TestNotNull(TEXT("heal picked up and selected"), Held))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	// Hurt the caster, then a cast heals HealingAmount and spends one charge.
	FWoTDamageEvent Ev;
	Ev.WoTDamageType = TEXT("xxFxx");
	Ev.Damage = 30;
	Pawn->TakeDamage(30.0f, Ev, nullptr, nullptr);
	TestEqual(TEXT("caster hurt to 70"), Pawn->Health, 70);

	const int32 Charges = Held->CurCharges;
	Inv->UseAngreal();
	Inv->CeaseUsingAngreal();
	TestEqual(TEXT("heal raised health by HealingAmount (10)"), Pawn->Health, 80);
	TestEqual(TEXT("heal spent one charge"), Held->CurCharges, Charges - 1);

	// At full health it's a no-op — no heal, no charge spent.
	Pawn->IncreaseHealth(100);   // -> clamped to 100
	TestEqual(TEXT("caster back to full"), Pawn->Health, 100);
	const int32 FullCharges = Held->CurCharges;
	WoTTest::TickWorld(World, 1.1f);      // clear the cast gate
	Inv->UseAngreal();
	Inv->CeaseUsingAngreal();
	TestEqual(TEXT("healing at full does nothing"), Pawn->Health, 100);
	TestEqual(TEXT("healing at full spends no charge"), Held->CurCharges, FullCharges);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTShieldTest,
	"WoT.Inventory.Shield",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTShieldTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	UWoTInventoryComponent* Inv = Pawn->InventoryComponent;

	auto Leeches = [](AActor* H) { TArray<UWoTLeech*> A; H->GetComponents<UWoTLeech>(A); return A.Num(); };
	auto Hurt = [&](const TCHAR* Type, int32 Amount)
	{
		FWoTDamageEvent Ev;
		Ev.WoTDamageType = FName(Type);
		Ev.Damage = Amount;
		Pawn->TakeDamage((float)Amount, Ev, nullptr, nullptr);
	};

	// Cast a Fire Shield on the caster.
	AWoTAngrealInvFireShield* Ground = World->SpawnActor<AWoTAngrealInvFireShield>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	WoTTest::TickWorld(World, 0.1f);
	Pawn->SetActorLocation(Ground->GetActorLocation());
	WoTTest::TickWorld(World, 0.1f);
	AWoTAngrealInvFireShield* Held = Cast<AWoTAngrealInvFireShield>(Inv->SelectedItem.Get());
	if (!TestNotNull(TEXT("fire shield picked up"), Held))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Inv->UseAngreal();
	Inv->CeaseUsingAngreal();

	TArray<UWoTIgnoreFireElementReflector*> Shields;
	Pawn->GetComponents<UWoTIgnoreFireElementReflector>(Shields);
	TestEqual(TEXT("casting installed the fire-element reflector"), Shields.Num(), 1);

	// 1) Fire damage is deflected; non-fire damage passes through.
	Hurt(TEXT("xxFxx"), 30);
	TestEqual(TEXT("fire shield negates fire damage"), Pawn->Health, 100);
	Hurt(TEXT("xxxWx"), 30);
	TestEqual(TEXT("fire shield lets water damage through"), Pawn->Health, 70);

	// 2) It also throws away same-element DELETERIOUS weaves: a fire-source bad
	//    leech can't land (element shields only block deleterious, non-self effects
	//    — IgnoreElementReflector.uc:96).
	AWoTAngrealInvFireball* FireSrc = World->SpawnActor<AWoTAngrealInvFireball>(
		FVector(600, 0, 100), FRotator::ZeroRotator);   // bElementFire
	UWoTTestLeech* FireLeech = NewObject<UWoTTestLeech>(Pawn);
	FireLeech->bDeleterious = true;
	FireLeech->SetSourceAngreal(FireSrc);
	FireLeech->AttachTo(Pawn);
	TestEqual(TEXT("fire shield blocks a deleterious fire-element leech"), Leeches(Pawn), 0);

	// ...but a non-fire weave still lands.
	AWoTAngrealInvIce* WaterSrc = World->SpawnActor<AWoTAngrealInvIce>(
		FVector(650, 0, 100), FRotator::ZeroRotator);   // bElementWater
	UWoTTestLeech* WaterLeech = NewObject<UWoTTestLeech>(Pawn);
	WaterLeech->bDeleterious = true;
	WaterLeech->SetSourceAngreal(WaterSrc);
	WaterLeech->AttachTo(Pawn);
	TestEqual(TEXT("non-fire leech still lands through the fire shield"), Leeches(Pawn), 1);

	// ...and a BENEFICIAL same-element weave passes through untouched.
	UWoTTestLeech* GoodFire = NewObject<UWoTTestLeech>(Pawn);
	GoodFire->bDeleterious = false;
	GoodFire->SetSourceAngreal(FireSrc);
	GoodFire->AttachTo(Pawn);
	TestEqual(TEXT("a beneficial fire weave is not blocked"), Leeches(Pawn), 2);

	// 3) The shield expires (Duration) — fire damage lands again afterward.
	if (Shields.Num() == 1)
	{
		Shields[0]->Lifespan = 0.15f;
	}
	WoTTest::TickWorld(World, 0.25f);
	TArray<UWoTIgnoreFireElementReflector*> AfterExpiry;
	Pawn->GetComponents<UWoTIgnoreFireElementReflector>(AfterExpiry);
	TestEqual(TEXT("fire shield uninstalled on expiry"), AfterExpiry.Num(), 0);
	Hurt(TEXT("xxFxx"), 30);
	TestEqual(TEXT("fire damage lands once the shield is gone"), Pawn->Health, 40);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPersonalShieldTest,
	"WoT.Inventory.PersonalShield",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTPersonalShieldTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	UWoTInventoryComponent* Inv = Pawn->InventoryComponent;

	auto Pool = [&]() { TArray<UWoTShieldReflector*> A; Pawn->GetComponents<UWoTShieldReflector>(A); return A; };
	auto Hurt = [&](int32 Amount)
	{
		FWoTDamageEvent Ev;
		Ev.WoTDamageType = TEXT("xxxxx");
		Ev.Damage = Amount;
		Pawn->TakeDamage((float)Amount, Ev, nullptr, nullptr);
	};

	AWoTAngrealInvShield* Ground = World->SpawnActor<AWoTAngrealInvShield>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	WoTTest::TickWorld(World, 0.1f);
	Pawn->SetActorLocation(Ground->GetActorLocation());
	WoTTest::TickWorld(World, 0.1f);
	AWoTAngrealInvShield* Held = Cast<AWoTAngrealInvShield>(Inv->SelectedItem.Get());
	if (!TestNotNull(TEXT("personal shield picked up"), Held))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Inv->UseAngreal();
	Inv->CeaseUsingAngreal();

	TestEqual(TEXT("cast installed the hit-point shield"), Pool().Num(), 1);
	if (Pool().Num() != 1)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	TestEqual(TEXT("shield starts with a full pool (50)"), Pool()[0]->ShieldHitPoints, 50);

	// Recasting at a FULL pool fails without spending a charge
	// (AngrealInvShield.uc:33-43 Cast -> Failed()).
	const int32 ChargesAtFull = Held->CurCharges;
	Inv->UseAngreal();
	Inv->CeaseUsingAngreal();
	TestEqual(TEXT("full-pool recast spends no charge"), Held->CurCharges, ChargesAtFull);
	TestEqual(TEXT("full-pool recast installs nothing new"), Pool().Num(), 1);

	// Half of each hit goes to the pool, half to the player.
	Hurt(20);
	TestEqual(TEXT("shield absorbs half: player takes 10"), Pawn->Health, 90);
	TestEqual(TEXT("pool took the other 10"), Pool()[0]->ShieldHitPoints, 40);
	Hurt(20);
	TestEqual(TEXT("still halving damage"), Pawn->Health, 80);
	TestEqual(TEXT("pool down to 30"), Pool()[0]->ShieldHitPoints, 30);

	// A 60 hit depletes the 30-point pool (30 to it) and breaks the shield; the
	// player takes its 30 share (no overflow this time).
	Hurt(60);
	TestEqual(TEXT("depleted shield uninstalls"), Pool().Num(), 0);
	TestEqual(TEXT("player took the un-absorbed share"), Pawn->Health, 50);

	// With the shield gone, damage lands in full.
	Hurt(20);
	TestEqual(TEXT("full damage after the shield breaks"), Pawn->Health, 30);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFindTargetTest,
	"WoT.Inventory.FindTarget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTFindTargetTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	// Preserve this legacy spec's Easy charge-table assumption now that
	// Normal is the production default.
	World->GetAuthGameMode<AWoTGameMode>()->Difficulty = 0;

	AWoTCharacter* Shooter = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator);   // faces +X
	Shooter->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	UWoTInventoryComponent* Inv = Shooter->InventoryComponent;

	// One target dead ahead (far, in the FOV cone), one to the side (near, out
	// of the cone).
	AWoTCharacter* Ahead = World->SpawnActor<AWoTCharacter>(
		FVector(800, 0, 100), FRotator::ZeroRotator);
	Ahead->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	AWoTCharacter* Beside = World->SpawnActor<AWoTCharacter>(
		FVector(0, -400, 100), FRotator::ZeroRotator);
	Beside->GetCharacterMovement()->SetMovementMode(MOVE_Flying);

	AWoTAngrealInvTarget* Ground = World->SpawnActor<AWoTAngrealInvTarget>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	WoTTest::TickWorld(World, 0.1f);
	Shooter->SetActorLocation(Ground->GetActorLocation());
	WoTTest::TickWorld(World, 0.1f);
	AWoTAngrealInvTarget* Held = Cast<AWoTAngrealInvTarget>(Inv->SelectedItem.Get());
	if (!TestNotNull(TEXT("find target picked up"), Held))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	// Without Find Target: normal targeting locks the one dead ahead (in the FOV).
	AActor* Before = Held->FindBestTarget(Shooter->GetActorLocation(),
		FRotator::ZeroRotator, 22.5f);
	TestTrue(TEXT("normal targeting picks the one in the view cone"), Before == Ahead);

	// Cast Find Target -> AutoTargetReflector installed.
	Inv->UseAngreal();
	Inv->CeaseUsingAngreal();
	TArray<UWoTAutoTargetReflector*> Auto;
	Shooter->GetComponents<UWoTAutoTargetReflector>(Auto);
	TestEqual(TEXT("Find Target installed the auto-target reflector"), Auto.Num(), 1);

	// With Find Target: locks the NEAREST reachable, ignoring the FOV cone.
	AActor* After = Held->FindBestTarget(Shooter->GetActorLocation(),
		FRotator::ZeroRotator, 22.5f);
	TestTrue(TEXT("Find Target locks the nearest victim regardless of facing"),
		After == Beside);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTReflectTest,
	"WoT.Inventory.Reflect",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTReflectTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AWoTCharacter* Shooter = World->SpawnActor<AWoTCharacter>(
		FVector(-500, 0, 100), FRotator::ZeroRotator);
	Shooter->GetCharacterMovement()->SetMovementMode(MOVE_Flying);

	// A victim who casts Reflect.
	AWoTCharacter* Guarded = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	Guarded->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	UWoTInventoryComponent* Inv = Guarded->InventoryComponent;
	AWoTAngrealInvReflect* Ground = World->SpawnActor<AWoTAngrealInvReflect>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	WoTTest::TickWorld(World, 0.1f);
	Guarded->SetActorLocation(Ground->GetActorLocation());
	WoTTest::TickWorld(World, 0.1f);
	if (!TestNotNull(TEXT("reflect picked up"), Cast<AWoTAngrealInvReflect>(Inv->SelectedItem.Get())))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Inv->UseAngreal();
	Inv->CeaseUsingAngreal();
	TestTrue(TEXT("reflect is active on the guarded pawn"), Guarded->ReflectsProjectiles());

	// A dart flying at the guarded pawn bounces back and doesn't hurt them.
	AWoTDartProjectile* Dart = World->SpawnActor<AWoTDartProjectile>(
		AWoTDartProjectile::StaticClass(), FVector(-100, 0, 100),
		FRotator::ZeroRotator, SP);   // velocity +X (toward the pawn)
	Dart->SourcePawn = Shooter;
	Dart->SetIgnoredPawn(Shooter);
	Guarded->Health = 100;
	const float Vx0 = Dart->ProjectileMovement->Velocity.X;
	Dart->ProcessTouch(Guarded, Guarded->GetActorLocation());
	TestTrue(TEXT("dart had forward velocity"), Vx0 > 0.0f);
	TestEqual(TEXT("reflected weave doesn't hurt the guarded pawn"), Guarded->Health, 100);
	TestTrue(TEXT("reflected weave reversed direction"),
		Dart->ProjectileMovement->Velocity.X < 0.0f);
	TestTrue(TEXT("reflected weave now belongs to the reflecting pawn"),
		Dart->SourcePawn == Guarded);

	// Control: an unguarded pawn takes the dart's damage normally.
	AWoTCharacter* Exposed = World->SpawnActor<AWoTCharacter>(
		FVector(400, 0, 100), FRotator::ZeroRotator);
	Exposed->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	Exposed->Health = 100;
	AWoTDartProjectile* Dart2 = World->SpawnActor<AWoTDartProjectile>(
		AWoTDartProjectile::StaticClass(), FVector(300, 0, 100),
		FRotator::ZeroRotator, SP);
	Dart2->SourcePawn = Shooter;
	Dart2->SetIgnoredPawn(Shooter);
	Dart2->ProcessTouch(Exposed, Exposed->GetActorLocation());
	TestEqual(TEXT("an unguarded pawn takes the dart hit (10)"), Exposed->Health, 90);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTForkTest,
	"WoT.Inventory.Fork",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTForkTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AWoTCharacter* Shooter = World->SpawnActor<AWoTCharacter>(
		FVector(-500, 0, 100), FRotator::ZeroRotator);
	Shooter->GetCharacterMovement()->SetMovementMode(MOVE_Flying);

	// A victim who casts Fork.
	AWoTCharacter* Forker = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	Forker->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	UWoTInventoryComponent* Inv = Forker->InventoryComponent;
	AWoTAngrealInvFork* Ground = World->SpawnActor<AWoTAngrealInvFork>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	WoTTest::TickWorld(World, 0.1f);
	Forker->SetActorLocation(Ground->GetActorLocation());
	WoTTest::TickWorld(World, 0.1f);
	if (!TestNotNull(TEXT("fork picked up"), Cast<AWoTAngrealInvFork>(Inv->SelectedItem.Get())))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Inv->UseAngreal();
	Inv->CeaseUsingAngreal();
	TestTrue(TEXT("fork is active on the pawn"), Forker->ForksProjectiles());

	// A dart flying at the Fork pawn still hits them AND spawns a duplicate back.
	AWoTDartProjectile* Dart = World->SpawnActor<AWoTDartProjectile>(
		AWoTDartProjectile::StaticClass(), FVector(-100, 0, 100),
		FRotator::ZeroRotator, SP);   // velocity +X toward the pawn
	Dart->SourcePawn = Shooter;
	Dart->SetIgnoredPawn(Shooter);
	Forker->Health = 100;
	Dart->ProcessTouch(Forker, Forker->GetActorLocation());

	TestEqual(TEXT("Fork lets the weave still hit you (10)"), Forker->Health, 90);

	// The duplicate belongs to the Fork pawn and flies back toward the sender.
	AWoTDartProjectile* Copy = nullptr;
	for (TActorIterator<AWoTDartProjectile> It(World); It; ++It)
	{
		if (IsValid(*It) && It->SourcePawn == Forker)
		{
			Copy = *It;
		}
	}
	if (TestNotNull(TEXT("a fork duplicate was launched"), Copy))
	{
		TestTrue(TEXT("the fork duplicate flies back toward the sender"),
			Copy->ProjectileMovement->Velocity.X < 0.0f);
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

// Regression for the fork-copy overlap spawn: the copy spawns AT the touch point,
// overlapping its host, and begin-overlap fires DURING spawn on this codebase — a
// plain SpawnActor ran the copy's ProcessTouch(host) before bIsForkCopy/IgnoredPawn
// were set, re-forking unbounded (one host hit per recursion level). The deferred
// spawn sets every guard pre-FinishSpawning (retail: ForkRTSReflector.uc:27-43
// IgnoredProj). Also pins the copy's Instigator = the fork HOST (kill credit,
// ReturnToSenderReflector.uc:72 via ForkRTSReflector.uc:39).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTForkSpawnOverlapTest,
	"WoT.Inventory.ForkSpawnOverlap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTForkSpawnOverlapTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AWoTCharacter* Forker = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	Forker->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	UWoTInventoryComponent* Inv = Forker->InventoryComponent;
	World->SpawnActor<AWoTAngrealInvFork>(FVector(0, 0, 100), FRotator::ZeroRotator);
	WoTTest::TickWorld(World, 0.2f);
	if (!TestNotNull(TEXT("fork picked up"), Cast<AWoTAngrealInvFork>(Inv->SelectedItem.Get())))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Inv->UseAngreal();
	Inv->CeaseUsingAngreal();
	TestTrue(TEXT("fork is active on the pawn"), Forker->ForksProjectiles());

	// Spawn the dart ON the fork host: its own begin-overlap ProcessTouch(host)
	// fires during SpawnActor, and the fork copy in turn spawns overlapping the
	// host too. Pre-fix this recursed to a stack overflow.
	Forker->Health = 100;
	World->SpawnActor<AWoTDartProjectile>(AWoTDartProjectile::StaticClass(),
		Forker->GetActorLocation(), FRotator::ZeroRotator, SP);
	WoTTest::TickWorld(World, 0.1f);

	TestEqual(TEXT("host hit exactly once (no per-level re-hits)"), Forker->Health, 90);

	int32 AliveDarts = 0;
	AWoTDartProjectile* Copy = nullptr;
	for (TActorIterator<AWoTDartProjectile> It(World); It; ++It)
	{
		if (IsValid(*It)) { ++AliveDarts; Copy = *It; }
	}
	TestEqual(TEXT("exactly one dart survives (the copy; the original exploded)"),
		AliveDarts, 1);
	if (TestNotNull(TEXT("the fork copy exists"), Copy))
	{
		TestEqual(TEXT("copy belongs to the fork host"), Copy->SourcePawn.Get(), Cast<APawn>(Forker));
		TestEqual(TEXT("copy Instigator = fork host (kill credit)"),
			Copy->GetInstigator(), Cast<APawn>(Forker));
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

// CDO parity spot-checks.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAuditCDODefaultsTest,
	"WoT.Audit.CDODefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTAuditCDODefaultsTest::RunTest(const FString&)
{
	// Dart expires like every projectile (Engine.Projectile LifeSpan=140).
	TestEqual(TEXT("Dart InitialLifeSpan = 140"),
		GetDefault<AWoTDartProjectile>()->InitialLifeSpan, 140.0f);
	// Seal.uc:442 — a carried seal cycles ahead of every angreal (lowest group 50).
	TestEqual(TEXT("Seal InventoryGroup = 40"),
		GetDefault<AWoTSeal>()->InventoryGroup, 40);
	// Engine.PlayerStart byte default 0; T3D omits default-valued props, so an
	// untagged start must read team 0, not neutral.
	TestEqual(TEXT("PlayerStart TeamNumber defaults 0"),
		(int32)GetDefault<AWoTPlayerStart>()->TeamNumber, 0);
	// Every MP faction pawn defaults its signature weave to Air Pulse
	// (AesSedai.uc:88 etc.); the port's single character stands in.
	TestEqual(TEXT("DefaultAngrealInventory = AirBurst"),
		GetDefault<AWoTCharacter>()->DefaultAngrealInventory.Get(),
		AWoTAngrealInvAirBurst::StaticClass());
	// The infinite-ammo mechanism is ChargeCost=0 (AngrealInvAirBurst.uc:374):
	// UseCharge subtracts 0, GoEmpty is unreachable, and the HUD draws the
	// infinity glyph off this exact property (BaseHUD.uc:297). The 999 seed is
	// cosmetic backing only.
	TestEqual(TEXT("Air Pulse ChargeCost = 0 (truly infinite)"),
		GetDefault<AWoTAngrealInvAirBurst>()->ChargeCost, 0);

	return true;
}

// Weave-vs-weave collision: retail projectiles pass through each other (the
// Touch filter rejects a non-bProjTarget projectile), while an armed ward IS
// bProjTarget and detonates from a weave's touch. As WorldDynamic the port's
// projectiles mutually BLOCKED — two crossing weaves exploded on each other;
// the WoTProjectile object channel (overlap) fixes it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTProjectilePassThroughTest,
	"WoT.Inventory.ProjectilePassThrough",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTProjectilePassThroughTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	// Two darts flying head-on cross without touching each other.
	AWoTDartProjectile* A = World->SpawnActor<AWoTDartProjectile>(
		AWoTDartProjectile::StaticClass(), FVector(0, 0, 100), FRotator::ZeroRotator, SP);
	AWoTDartProjectile* B = World->SpawnActor<AWoTDartProjectile>(
		AWoTDartProjectile::StaticClass(), FVector(400, 0, 100),
		FRotator(0, 180, 0), SP);
	TWeakObjectPtr<AWoTDartProjectile> APtr = A, BPtr = B;
	WoTTest::TickWorld(World, 0.3f);   // 2000 uu/s each — they meet at ~0.1s, well past by 0.3
	TestTrue(TEXT("crossing weaves pass through each other"),
		APtr.IsValid() && BPtr.IsValid());

	// A dart flying into an armed ward detonates it (bProjTarget touch), and
	// itself explodes on the contact.
	AWoTExpWardProjectile* Ward = World->SpawnActor<AWoTExpWardProjectile>(
		AWoTExpWardProjectile::StaticClass(), FVector(0, 2000, 50),
		FRotator::ZeroRotator, SP);
	Ward->ProjectileMovement->StopMovementImmediately();   // placed, not lobbed
	Ward->Activate();
	AWoTDartProjectile* Shot = World->SpawnActor<AWoTDartProjectile>(
		AWoTDartProjectile::StaticClass(), FVector(-300, 2000, 50),
		FRotator::ZeroRotator, SP);
	TWeakObjectPtr<AWoTExpWardProjectile> WardPtr = Ward;
	TWeakObjectPtr<AWoTDartProjectile> ShotPtr = Shot;
	WoTTest::TickWorld(World, 0.4f);
	TestFalse(TEXT("a dart detonates an armed ward"), WardPtr.IsValid());
	TestFalse(TEXT("the dart exploded on the ward"), ShotPtr.IsValid());

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAbsorbTest,
	"WoT.Inventory.Absorb.StealsProjectileWeapon",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTAbsorbTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	// An attacker holding the artifact that will fire the weave.
	AWoTCharacter* Attacker = World->SpawnActor<AWoTCharacter>(
		FVector(-500, 0, 100), FRotator::ZeroRotator);
	Attacker->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	AWoTAngrealInvFireball* Weapon = World->SpawnActor<AWoTAngrealInvFireball>(
		AWoTAngrealInvFireball::StaticClass(), FVector(-500, 0, 100),
		FRotator::ZeroRotator, SP);
	Weapon->GiveTo(Attacker);
	Weapon->CurCharges = 10;

	// A victim who casts Absorb.
	AWoTCharacter* Victim = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	Victim->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	UWoTInventoryComponent* VInv = Victim->InventoryComponent;
	AWoTAngrealInvAbsorb* Ground = World->SpawnActor<AWoTAngrealInvAbsorb>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	WoTTest::TickWorld(World, 0.1f);
	Victim->SetActorLocation(Ground->GetActorLocation());
	WoTTest::TickWorld(World, 0.1f);
	if (!TestNotNull(TEXT("absorb picked up"), Cast<AWoTAngrealInvAbsorb>(VInv->SelectedItem.Get())))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	VInv->UseAngreal();
	VInv->CeaseUsingAngreal();

	UWoTInventoryComponent* AInv = Attacker->InventoryComponent;
	TestTrue(TEXT("attacker starts holding the weapon"), AInv->Items.Contains(Weapon));

	// The attacker's weave strikes the Absorb-shielded victim.
	AWoTFireballProjectile* Proj = World->SpawnActor<AWoTFireballProjectile>(
		AWoTFireballProjectile::StaticClass(), FVector(-100, 0, 100),
		FRotator::ZeroRotator, SP);
	Proj->SetSourceAngreal(Weapon);   // SourceAngreal = weapon, SourcePawn = attacker
	Victim->Health = 100;
	Proj->ProcessTouch(Victim, Victim->GetActorLocation());

	// The victim isn't hurt, and the firing artifact changed hands (+1 charge).
	TestEqual(TEXT("absorb victim takes no damage"), Victim->Health, 100);
	TestFalse(TEXT("weapon snatched from the attacker"), AInv->Items.Contains(Weapon));
	TestTrue(TEXT("weapon now in the victim's inventory"), VInv->Items.Contains(Weapon));
	TestTrue(TEXT("stolen weapon is now owned by the victim"), Weapon->GetOwner() == Victim);
	TestEqual(TEXT("the spent charge was refunded on steal"), Weapon->CurCharges, 11);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAirBurstTest,
	"WoT.Inventory.AirBurst",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTAirBurstTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	// Caster at eye height 142 (100 + BaseEyeHeight 42), facing +X.
	AWoTCharacter* Caster = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	Caster->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	UWoTInventoryComponent* Inv = Caster->InventoryComponent;

	// One victim on the beam axis just ahead, one off to the side.
	AWoTCharacter* InBeam = World->SpawnActor<AWoTCharacter>(
		FVector(81.8f, 0, 142), FRotator::ZeroRotator);
	InBeam->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	AWoTCharacter* Protected = World->SpawnActor<AWoTCharacter>(
		FVector(105, 0, 142), FRotator::ZeroRotator);
	Protected->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	UWoTNoBadEffectsReflector* Dome =
		NewObject<UWoTNoBadEffectsReflector>(Protected);
	Dome->Install(Protected);
	AWoTCharacter* OffBeam = World->SpawnActor<AWoTCharacter>(
		FVector(0, 300, 142), FRotator::ZeroRotator);
	OffBeam->GetCharacterMovement()->SetMovementMode(MOVE_Flying);

	// The ground item is co-located with the caster — it's picked up on overlap.
	World->SpawnActor<AWoTAngrealInvAirBurst>(FVector(0, 0, 100), FRotator::ZeroRotator);
	WoTTest::TickWorld(World, 0.2f);
	if (!TestNotNull(TEXT("air pulse picked up"), Cast<AWoTAngrealInvAirBurst>(Inv->SelectedItem.Get())))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	InBeam->Health = 100;
	Protected->Health = 100;
	OffBeam->Health = 100;
	const FVector InPos0 = InBeam->GetActorLocation();
	const FVector ProtectedPos0 = Protected->GetActorLocation();
	FHitResult ExpectedHit;
	const FVector BeamStart = Caster->GetActorLocation()
		+ FVector(0, 0, Caster->BaseEyeHeight);
	TestTrue(TEXT("fractional-damage pawn has a real production capsule hit"),
		InBeam->GetCapsuleComponent()->LineTraceComponent(ExpectedHit, BeamStart,
			BeamStart + FVector(120, 0, 0),
			FCollisionQueryParams(SCENE_QUERY_STAT(AirBurstFractionSpec), false)));
	const float ContinuousDamage = 20.0f - 10.0f
		* ((ExpectedHit.ImpactPoint.X - BeamStart.X) / 120.0f);
	TestTrue(TEXT("fixture exercises a .5-or-greater fractional Air Burst result"),
		FMath::Frac(ContinuousDamage) >= 0.5f);
	const int32 ExpectedTruncatedDamage = FMath::TruncToInt(ContinuousDamage);

	Inv->UseAngreal();
	Inv->CeaseUsingAngreal();

	// The one in the beam is damaged; the one off to the side is untouched.
	TestEqual(TEXT("fractional Air Burst damage truncates like int DamageEffect.Initialize"),
		InBeam->Health, 100 - ExpectedTruncatedDamage);
	TestEqual(TEXT("off-beam victim unaffected"), OffBeam->Health, 100);
	TestEqual(TEXT("NoBadEffects blocks the production Air Burst effect"),
		Protected->Health, 100);

	// ...and NEVER pushed: shipped Air Pulse damages pawns with zero momentum —
	// PushableTypes is Carcass/Bounceable/BreakableDecoration only, Pawn is not
	// in it (AngrealInvAirBurst.uc:232,254,362-364).
	WoTTest::TickWorld(World, 0.15f);
	TestTrue(FString::Printf(TEXT("in-beam victim not pushed (dx %.1f)"),
		InBeam->GetActorLocation().X - InPos0.X),
		FMath::Abs(InBeam->GetActorLocation().X - InPos0.X) < 1.0f);
	TestTrue(TEXT("protected pawn receives no Air Burst push"),
		Protected->GetActorLocation().Equals(ProtectedPos0, 1.0f));

	// MashadarGuide is a SeekingProjectile, not the MashadarTrailer family.
	// Its production head segment occupies the same ray, so a guide+segment
	// alias bug would drain shared health twice.
	InBeam->Destroy();
	Protected->Destroy();
	OffBeam->Destroy();
	WoTTest::TickWorld(World, 0.6f);
	FTransform GuideTransform(FRotator::ZeroRotator, FVector(30, 0, 142));
	AWoTMashadarGuide* Guide = World->SpawnActorDeferred<AWoTMashadarGuide>(
		AWoTMashadarGuide::StaticClass(), GuideTransform, nullptr, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!TestNotNull(TEXT("production Mashadar guide spawns"), Guide))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Guide->bCitadelPlaced = true;
	Guide->FinishSpawning(GuideTransform);
	// This subfixture isolates Air Burst's guide/segment alias suppression. A
	// citadel guide's health normally lives in the BattleGameMode team pool and
	// an arena guide receives it on its first deployed Chase; neither lifecycle
	// is under test here, so provide one non-lethal hit's health explicitly.
	Guide->MashadarHealth = 100;
	Guide->Activate();
	AWoTMashadarSegment* Segment = Guide->GetMasterSegment();
	if (!TestNotNull(TEXT("production Mashadar guide creates its head segment"), Segment))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	TestFalse(TEXT("MashadarGuide is not an Air Burst target"),
		Cast<AWoTAngrealInvAirBurst>(Inv->SelectedItem.Get())->IsAffectable(Guide));
	TestTrue(TEXT("production MashadarTrailer segment is an Air Burst target"),
		Cast<AWoTAngrealInvAirBurst>(Inv->SelectedItem.Get())->IsAffectable(Segment));
	FHitResult SegmentHit;
	float SegmentHitDistanceSq = TNumericLimits<float>::Max();
	TInlineComponentArray<UPrimitiveComponent*> SegmentPrimitives(Segment);
	for (UPrimitiveComponent* Primitive : SegmentPrimitives)
	{
		FHitResult Candidate;
		if (Primitive && Primitive->IsCollisionEnabled()
			&& Primitive->LineTraceComponent(Candidate, BeamStart,
				BeamStart + FVector(120, 0, 0),
				FCollisionQueryParams(SCENE_QUERY_STAT(AirBurstMashadarSpec), false)))
		{
			const float DistanceSq = FVector::DistSquared(
				BeamStart, Candidate.ImpactPoint);
			if (DistanceSq < SegmentHitDistanceSq)
			{
				SegmentHitDistanceSq = DistanceSq;
				SegmentHit = Candidate;
			}
		}
	}
	TestTrue(TEXT("guide-created production segment intersects the cast ray"),
		SegmentHitDistanceSq < TNumericLimits<float>::Max());
	const FVector SegmentRel = SegmentHit.ImpactPoint - BeamStart;
	const float SegmentScalar = FMath::Clamp(
		(1.0f - SegmentRel.X / 120.0f)
		* (1.0f - FMath::Abs(SegmentRel.Y) / 10.0f)
		* (1.0f - FMath::Abs(SegmentRel.Z) / 10.0f), 0.0f, 1.0f);
	const int32 ExpectedMashadarDamage = FMath::TruncToInt(
		10.0f + 10.0f * SegmentScalar);
	const int32 MashadarHealthBefore = Guide->MashadarHealth;
	Inv->UseAngreal();
	Inv->CeaseUsingAngreal();
	const int32 MashadarDamage = MashadarHealthBefore - Guide->MashadarHealth;
	TestEqual(TEXT("guide+segment ray applies exactly the one segment hit"),
		MashadarDamage, ExpectedMashadarDamage);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTShiftTest,
	"WoT.Inventory.Shift",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTShiftTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator);   // faces +X
	Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	UWoTInventoryComponent* Inv = Pawn->InventoryComponent;

	World->SpawnActor<AWoTAngrealInvShift>(FVector(0, 0, 100), FRotator::ZeroRotator);
	WoTTest::TickWorld(World, 0.2f);
	AWoTAngrealInvShift* Held = Cast<AWoTAngrealInvShift>(Inv->SelectedItem.Get());
	if (!TestNotNull(TEXT("shift picked up"), Held))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	// A seeker off to the side, locked onto the caster.
	AWoTSeekerProjectile* Seeker = World->SpawnActor<AWoTSeekerProjectile>(
		AWoTSeekerProjectile::StaticClass(), FVector(500, 0, 100), FRotator::ZeroRotator, SP);
	Seeker->SetDestination(Pawn);
	TestTrue(TEXT("seeker locked onto the caster"), Seeker->Destination == Pawn);

	const FVector Loc0 = Pawn->GetActorLocation();
	const int32 Charges = Held->CurCharges;
	Inv->UseAngreal();
	Inv->CeaseUsingAngreal();

	// Teleported ShiftDistance (160) straight ahead...
	const float Moved = FVector::Dist(Pawn->GetActorLocation(), Loc0);
	TestTrue(FString::Printf(TEXT("shift teleported ~160 (moved %.0f)"), Moved),
		Moved > 140.0f && Moved < 180.0f);
	TestTrue(TEXT("shift moved forward (+X)"), Pawn->GetActorLocation().X > Loc0.X + 100.0f);
	TestEqual(TEXT("shift spent a charge"), Held->CurCharges, Charges - 1);

	// ...and the seeker lost its lock.
	TestTrue(TEXT("the seeker lost its target on the shift"),
		Seeker->Destination == nullptr);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTEarthTremorTest,
	"WoT.Inventory.EarthTremor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTEarthTremorTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	// 1) Inventory config: an uncommon, Earth-element launcher for the projectile.
	AWoTAngrealInvEarthTremor* Inv = World->SpawnActor<AWoTAngrealInvEarthTremor>(
		AWoTAngrealInvEarthTremor::StaticClass(), FVector(0, 0, 100), FRotator::ZeroRotator, SP);
	if (!TestNotNull(TEXT("earth tremor artifact spawned"), Inv))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	TestTrue(TEXT("earth tremor is Earth-element"), Inv->bElementEarth);
	TestTrue(TEXT("earth tremor is uncommon"), Inv->bUncommon);
	TestTrue(TEXT("earth tremor launches the EarthTremor projectile"),
		Inv->ProjectileClass == AWoTEarthTremorProjectile::StaticClass());

	// 2) Launch arc: fired flat (+X), it flies forward AND rises first (the
	//    (0,0,0.3) launch tilt), then falls back under gravity (PHYS_Falling).
	AWoTEarthTremorProjectile* Arc = World->SpawnActor<AWoTEarthTremorProjectile>(
		AWoTEarthTremorProjectile::StaticClass(), FVector(0, 0, 500), FRotator::ZeroRotator, SP);
	if (!TestNotNull(TEXT("arc projectile spawned"), Arc))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	const FVector Start = Arc->GetActorLocation();
	WoTTest::TickWorld(World, 0.1f);
	const FVector Early = Arc->GetActorLocation();
	TestTrue(FString::Printf(TEXT("tremor flies forward (dx %.0f)"), Early.X - Start.X),
		Early.X - Start.X > 30.0f);
	TestTrue(TEXT("tremor arcs up on launch (z rose)"), Early.Z > Start.Z);
	WoTTest::TickWorld(World, 0.5f);   // total ~0.6s: past the apex
	TestTrue(TEXT("tremor falls back under gravity (z below launch)"),
		IsValid(Arc) && Arc->GetActorLocation().Z < Start.Z);
	TestTrue(TEXT("tremor keeps travelling forward"),
		IsValid(Arc) && Arc->GetActorLocation().X > Early.X);

	// 3) Impact splash: eruption deals wide radial damage (Damage 30, radius 280).
	AWoTCharacter* Victim = World->SpawnActor<AWoTCharacter>(
		FVector(2000, 0, 100), FRotator::ZeroRotator, SP);
	Victim->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	// Spawn the projectile 150uu above the victim: within DamageRadius (280) so
	// the splash reaches it, but clear of the capsule so it doesn't touch-explode
	// on spawn (which would self-destruct it before we drive Explode).
	const FVector BoomLoc = Victim->GetActorLocation() + FVector(0, 0, 150);
	AWoTEarthTremorProjectile* Boom = World->SpawnActor<AWoTEarthTremorProjectile>(
		AWoTEarthTremorProjectile::StaticClass(), BoomLoc, FRotator::ZeroRotator, SP);
	if (TestNotNull(TEXT("splash projectile spawned"), Boom))
	{
		const int32 HP0 = Victim->Health;
		Boom->Explode(BoomLoc, FVector::UpVector);
		TestTrue(FString::Printf(TEXT("eruption splashed the victim (%d -> %d)"),
			HP0, Victim->Health), Victim->Health < HP0);
		TestFalse(TEXT("projectile destroyed on explode"), IsValid(Boom));
	}

	// 4) EarthTremorLeech: the (ring-installed, deferred) health drain — a hidden
	//    DecreaseHealthLeech that bleeds HP over time once attached.
	AWoTCharacter* Bled = World->SpawnActor<AWoTCharacter>(
		FVector(2500, 0, 100), FRotator::ZeroRotator, SP);
	Bled->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	UWoTEarthTremorLeech* Leech = NewObject<UWoTEarthTremorLeech>(Bled);
	TestTrue(TEXT("EarthTremorLeech is a DecreaseHealthLeech"),
		Leech->IsA<UWoTDecreaseHealthLeech>());
	TestFalse(TEXT("EarthTremorLeech hides its HUD icon"), Leech->bDisplayIcon);
	Leech->AffectResolution = 0.1f;
	Leech->AttachTo(Bled);
	const int32 BledHP0 = Bled->Health;
	WoTTest::TickWorld(World, 0.35f);
	TestTrue(FString::Printf(TEXT("tremor leech drained the victim (%d -> %d)"),
		BledHP0, Bled->Health), Bled->Health < BledHP0);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBalefireTest,
	"WoT.Inventory.Balefire",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTBalefireTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	// Caster at eye height 142 (100 + BaseEyeHeight 42), facing +X.
	AWoTCharacter* Caster = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	Caster->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	UWoTInventoryComponent* Inv = Caster->InventoryComponent;

	// One victim squarely on the beam axis, one well off to the side.
	AWoTCharacter* OnBeam = World->SpawnActor<AWoTCharacter>(
		FVector(300, 0, 142), FRotator::ZeroRotator);
	OnBeam->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	AWoTCharacter* OffBeam = World->SpawnActor<AWoTCharacter>(
		FVector(0, 400, 142), FRotator::ZeroRotator);
	OffBeam->GetCharacterMovement()->SetMovementMode(MOVE_Flying);

	// Pick up Balefire (co-located ground item).
	World->SpawnActor<AWoTAngrealInvBalefire>(FVector(0, 0, 100), FRotator::ZeroRotator);
	WoTTest::TickWorld(World, 0.2f);
	AWoTAngrealInvBalefire* Held = Cast<AWoTAngrealInvBalefire>(Inv->SelectedItem.Get());
	if (!TestNotNull(TEXT("balefire picked up"), Held))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	// 1) Config.
	TestTrue(TEXT("balefire is rare"), Held->bRare);
	TestTrue(TEXT("balefire is fire+spirit"), Held->bElementFire && Held->bElementSpirit);

	// 2) Full charge: hold past PrimeTime, then release. The instantaneous beam
	//    removes BalefireDamage (400) from the pawn in front and leaves the one
	//    off-axis untouched.
	OnBeam->Health = 1000;
	OffBeam->Health = 1000;
	Caster->GetCharacterMovement()->Velocity = FVector::ZeroVector;
	AWoTLegionProjectile* Legion = World->SpawnActor<AWoTLegionProjectile>(
		FVector(2000, 0, 142), FRotator::ZeroRotator);
	Inv->UseAngreal();          // begin charging
	WoTTest::TickWorld(World, 2.5f);     // > PrimeTime (2.0) => full charge (Scalar 1)
	Inv->CeaseUsingAngreal();   // release => fire
	TestEqual(TEXT("full-charge balefire deals full damage"), OnBeam->Health, 600);
	TestEqual(TEXT("off-beam pawn is untouched"), OffBeam->Health, 1000);
	TestEqual(TEXT("balefire recoil adds the exact retail kickback"),
		static_cast<float>(Caster->GetCharacterMovement()->Velocity.X),
		-Held->Kickback, 0.1f);
	TestEqual(TEXT("AddVelocity retains a flying caster's movement mode"),
		Caster->GetCharacterMovement()->MovementMode, MOVE_Flying);
	WoTTest::TickWorld(World, 0.6f);
	TestTrue(TEXT("balefire erases a Legion spirit on the retail whitelist"),
		!IsValid(Legion) || Legion->IsActorBeingDestroyed());
	const int32 FullDrop = 1000 - OnBeam->Health;   // 400

	// 3) Released before half-prime: it doesn't fire immediately, then auto-
	//    finishes to ~half charge on the FinishCharging timer (~half damage).
	OnBeam->Health = 1000;
	Inv->UseAngreal();
	Inv->CeaseUsingAngreal();   // immediate release => deferred to FinishCharging
	TestEqual(TEXT("early release hasn't fired yet"), OnBeam->Health, 1000);
	WoTTest::TickWorld(World, 1.2f);     // > PrimeTime/2 (1.0): the timer fires
	const int32 HalfDrop = 1000 - OnBeam->Health;
	TestTrue(FString::Printf(TEXT("auto-primed shot fired at ~half charge (drop %d)"), HalfDrop),
		HalfDrop > 150 && HalfDrop < FullDrop);

	// 4) "Breaks all rules" = skips only ProcessEffect. DamageEffect.Invoke() is a
	//    full TakeDamage (DamageEffect.uc:56-62), so the Personal Shield — a
	//    TakeDamage-chain reflector (ShieldReflector.uc:78-131) — DOES blunt
	//    balefire: the pool absorbs its share before the rest lands.
	UWoTShieldReflector* Shield = NewObject<UWoTShieldReflector>(OnBeam);
	Shield->Install(OnBeam);
	// Control — a normal hit IS absorbed (half to the pool, half to the player).
	OnBeam->Health = 1000;
	{
		FWoTDamageEvent Ev;
		Ev.WoTDamageType = TEXT("xxxxx");
		Ev.Damage = 40;
		OnBeam->TakeDamage(40.0f, Ev, nullptr, nullptr);
	}
	TestEqual(TEXT("control: shield absorbs half of a normal hit"), OnBeam->Health, 980);
	TestTrue(TEXT("control: the shield pool drained"), Shield->ShieldHitPoints < 50);
	// Balefire 400 vs the 30 left in the pool: the shield eats its remaining 30,
	// the other 370 land on the player, and the depleted shield uninstalls.
	OnBeam->Health = 1000;
	Inv->UseAngreal();
	WoTTest::TickWorld(World, 2.5f);
	Inv->CeaseUsingAngreal();
	TestEqual(TEXT("shield blunts balefire: pool-limited share absorbed"),
		OnBeam->Health, 630);
	TestTrue(TEXT("balefire drained the remaining shield pool"),
		!IsValid(Shield) || Shield->ShieldHitPoints == 0);

	// 5) What balefire DOES bypass is ProcessEffect: an anti-magic dome
	//    (NoBadEffectsReflector — ProcessEffect-only, no TakeDamage override)
	//    neither blocks balefire nor direct damage like zone pain.
	UWoTNoBadEffectsReflector* Dome = NewObject<UWoTNoBadEffectsReflector>(OnBeam);
	Dome->Install(OnBeam);
	OnBeam->Health = 1000;
	OnBeam->TakeRawDamage(100, TEXT("Balefire"), nullptr, OnBeam->GetActorLocation());
	TestEqual(TEXT("dome does not blunt balefire"), OnBeam->Health, 900);
	{
		FWoTDamageEvent Ev;
		Ev.WoTDamageType = TEXT("Drowned");
		Ev.Damage = 25;
		OnBeam->TakeDamage(25.0f, Ev, nullptr, nullptr);
	}
	TestEqual(TEXT("dome does not block direct (zone-pain-style) damage"),
		OnBeam->Health, 875);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSwapPlacesTest,
	"WoT.Inventory.SwapPlaces",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTSwapPlacesTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	// 1) Config.
	AWoTAngrealInvSwapPlaces* Inv = World->SpawnActor<AWoTAngrealInvSwapPlaces>(
		AWoTAngrealInvSwapPlaces::StaticClass(), FVector(0, 0, 50), FRotator::ZeroRotator, SP);
	if (!TestNotNull(TEXT("swap places artifact spawned"), Inv))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	TestTrue(TEXT("swap places is uncommon"), Inv->bUncommon);
	TestTrue(TEXT("swap places is Spirit-element"), Inv->bElementSpirit);
	TestTrue(TEXT("swap places is defensive"), Inv->bDefensive);
	TestTrue(TEXT("swap places launches the SwapPlaces projectile"),
		Inv->ProjectileClass == AWoTSwapPlacesProjectile::StaticClass());
	AWoTMashadarGuide* Mashadar = World->SpawnActor<AWoTMashadarGuide>(
		AWoTMashadarGuide::StaticClass(), FVector(0, 500, 100),
		FRotator::ZeroRotator, SP);
	TestTrue(TEXT("Mashadar guide aliases the retail non-targetable trailer family"),
		Inv->IsNotTargetable(Mashadar));

	// Caster faces +X; target 500 ahead facing back (-X, Yaw 180).
	AWoTCharacter* Caster = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	Caster->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	AWoTCharacter* Target = World->SpawnActor<AWoTCharacter>(
		FVector(500, 0, 100), FRotator(0.0f, 180.0f, 0.0f));
	Target->GetCharacterMovement()->SetMovementMode(MOVE_Flying);

	// One seeker chasing the caster, one chasing the target.
	AWoTSeekerProjectile* SeekerA = World->SpawnActor<AWoTSeekerProjectile>(
		AWoTSeekerProjectile::StaticClass(), FVector(250, 150, 100), FRotator::ZeroRotator, SP);
	AWoTSeekerProjectile* SeekerB = World->SpawnActor<AWoTSeekerProjectile>(
		AWoTSeekerProjectile::StaticClass(), FVector(250, -150, 100), FRotator::ZeroRotator, SP);
	if (!SeekerA || !SeekerB)
	{
		TestTrue(TEXT("both seekers spawned"), false);
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	SeekerA->SetDestination(Caster);
	SeekerB->SetDestination(Target);

	AWoTSwapPlacesProjectile* Proj = World->SpawnActor<AWoTSwapPlacesProjectile>(
		AWoTSwapPlacesProjectile::StaticClass(), FVector(250, 0, 100), FRotator::ZeroRotator, SP);
	if (!TestNotNull(TEXT("swap projectile spawned"), Proj))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	// 2) One swap moves both position and facing, and flips the seeker locks.
	Proj->PerformSwap(Caster, Target);
	TestTrue(FString::Printf(TEXT("caster took the target's spot (x %.0f)"),
		Caster->GetActorLocation().X),
		FMath::Abs(Caster->GetActorLocation().X - 500.0f) < 2.0f);
	TestTrue(FString::Printf(TEXT("target took the caster's spot (x %.0f)"),
		Target->GetActorLocation().X),
		FMath::Abs(Target->GetActorLocation().X - 0.0f) < 2.0f);
	TestTrue(TEXT("caster now faces where the target faced (Yaw ~180)"),
		FMath::Abs(FRotator::NormalizeAxis(Caster->GetActorRotation().Yaw - 180.0f)) < 1.0f);
	TestTrue(TEXT("target now faces where the caster faced (Yaw ~0)"),
		FMath::Abs(FRotator::NormalizeAxis(Target->GetActorRotation().Yaw - 0.0f)) < 1.0f);
	TestTrue(TEXT("the seeker on the caster now chases the target"),
		SeekerA->Destination == Target);
	TestTrue(TEXT("the seeker on the target now chases the caster"),
		SeekerB->Destination == Caster);

	// 3) Self-swap and a missing target are safe no-ops.
	const FVector CasterAt = Caster->GetActorLocation();
	Proj->PerformSwap(Caster, Caster);
	Proj->PerformSwap(Caster, nullptr);
	TestTrue(TEXT("self / null swap leaves the caster put"),
		FVector::Dist(Caster->GetActorLocation(), CasterAt) < 1.0f);

	// 4) Explode drives the swap end-to-end and consumes the weave.
	AWoTSwapPlacesProjectile* Proj2 = World->SpawnActor<AWoTSwapPlacesProjectile>(
		AWoTSwapPlacesProjectile::StaticClass(), FVector(250, 0, 100), FRotator::ZeroRotator, SP);
	if (TestNotNull(TEXT("second swap projectile spawned"), Proj2))
	{
		Proj2->InitiatingPawn = Caster;
		Proj2->SetDestination(Target);
		const FVector CasterPre = Caster->GetActorLocation();
		const FVector TargetPre = Target->GetActorLocation();
		Proj2->Explode(Proj2->GetActorLocation(), FVector::UpVector);
		TestTrue(TEXT("explode swapped caster and target back"),
			FVector::Dist(Caster->GetActorLocation(), TargetPre) < 2.0f
			&& FVector::Dist(Target->GetActorLocation(), CasterPre) < 2.0f);
		TestFalse(TEXT("the weave is consumed on explode"), IsValid(Proj2));
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAMAPearlModulatedVisualSpec,
	"WoT.Inventory.AMAPearlModulatedVisual",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTAMAPearlModulatedVisualSpec::RunTest(const FString&)
{
	UMaterialInterface* ModulateMaster = LoadObject<UMaterialInterface>(nullptr,
		TEXT("/Game/WoT/Materials/M_WoT_MeshModulate.M_WoT_MeshModulate"));
	if (!TestNotNull(TEXT("committed Pearl modulate master loads"), ModulateMaster))
	{
		return false;
	}

	UWorld* World = WoTTest::CreateSpecWorld();
	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTAMAPearl* Pearl = World->SpawnActor<AWoTAMAPearl>(
		AWoTAMAPearl::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, SP);
	UWoTVertexAnimComponent* Visual = Pearl
		? Pearl->FindComponentByClass<UWoTVertexAnimComponent>() : nullptr;
	if (!TestNotNull(TEXT("spawned Pearl owns a visual"), Visual))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	USkeletalMesh* Mesh = Visual->GetSkeletalMeshAsset();
	TestNotNull(TEXT("Pearl binds the AMApearl mesh"), Mesh);
	if (Mesh)
	{
		TestEqual(TEXT("Pearl mesh name"), Mesh->GetName(), FString(TEXT("AMApearl")));
	}
	TestTrue(TEXT("Pearl mesh has material slots"), Visual->GetNumMaterials() > 0);
	UTexture* StaticIceFrame = LoadObject<UTexture>(nullptr,
		TEXT("/Game/WoT/Textures/Angreal/AMApearl01.AMApearl01"));
	for (int32 Slot = 0; Slot < Visual->GetNumMaterials(); ++Slot)
	{
		UMaterialInterface* Material = Visual->GetMaterial(Slot);
		TestNotNull(FString::Printf(TEXT("Pearl slot %d binds a material"), Slot), Material);
		TestNotNull(FString::Printf(TEXT("Pearl slot %d is a MID"), Slot),
			Cast<UMaterialInstanceDynamic>(Material));
		TestTrue(FString::Printf(
			TEXT("Pearl slot %d is rooted at M_WoT_MeshModulate"), Slot),
			InventorySpecMaterialParentChainContains(Material, ModulateMaster));
		if (StaticIceFrame && Material)
		{
			UTexture* BoundTexture = nullptr;
			Material->GetTextureParameterValue(
				FHashedMaterialParameterInfo(FName(TEXT("Tex"))), BoundTexture);
			TestTrue(FString::Printf(TEXT("Pearl slot %d uses AMApearl01"), Slot),
				BoundTexture == StaticIceFrame);
		}
	}

	Pearl->Go();
	TestTrue(TEXT("Go applies the InitialScale endpoint to the visual"),
		Visual->GetRelativeScale3D().Equals(FVector(Pearl->InitialScale), KINDA_SMALL_NUMBER));
	WoTTest::TickWorld(World, 0.05f);
	const float ExpandingScale = Visual->GetRelativeScale3D().X;
	TestTrue(TEXT("Expanding advances between scale endpoints"),
		ExpandingScale > Pearl->InitialScale && ExpandingScale < Pearl->FinalScale);
	WoTTest::TickWorld(World, 1.0f);
	TestTrue(TEXT("Expanded holds the FinalScale endpoint on the visual"),
		Visual->GetRelativeScale3D().Equals(FVector(Pearl->FinalScale), KINDA_SMALL_NUMBER));
	TestTrue(TEXT("the affect radius reaches the matching FinalScale endpoint"),
		FMath::IsNearlyEqual(Pearl->ProjCollisionRadius,
			Pearl->BaseAffectRadius * Pearl->FinalScale));

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRemoveCurseTest,
	"WoT.Inventory.RemoveCurse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTRemoveCurseTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	auto LeechCount = [](AActor* H)
	{
		TArray<UWoTLeech*> A;
		H->GetComponents<UWoTLeech>(A);
		return A.Num();
	};
	auto PearlCount = [&]()
	{
		int32 N = 0;
		for (TActorIterator<AWoTAMAPearl> It(World); It; ++It) { N++; }
		return N;
	};
	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AWoTCharacter* Caster = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	Caster->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	UWoTInventoryComponent* Inv = Caster->InventoryComponent;

	// Pick up Unravel (co-located ground item).
	World->SpawnActor<AWoTAngrealInvRemoveCurse>(FVector(0, 0, 100), FRotator::ZeroRotator);
	WoTTest::TickWorld(World, 0.2f);
	AWoTAngrealInvRemoveCurse* Held = Cast<AWoTAngrealInvRemoveCurse>(Inv->SelectedItem.Get());
	if (!TestNotNull(TEXT("unravel picked up"), Held))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	// 1) Config.
	TestTrue(TEXT("unravel is rare"), Held->bRare);
	TestTrue(TEXT("unravel is all-element"),
		Held->bElementFire && Held->bElementWater && Held->bElementAir
		&& Held->bElementEarth && Held->bElementSpirit);
	TestTrue(TEXT("unravel is defensive"), Held->bDefensive);

	// A cursed victim just inside the dome's reach, plus a tainted artifact.
	AWoTCharacter* Victim = World->SpawnActor<AWoTCharacter>(
		FVector(50, 0, 100), FRotator::ZeroRotator, SP);
	Victim->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	UWoTTestLeech* Curse = NewObject<UWoTTestLeech>(Victim);   // removable, permanent
	Curse->AttachTo(Victim);
	TestEqual(TEXT("victim starts cursed"), LeechCount(Victim), 1);

	AWoTAngrealInvFireball* Tainted = World->SpawnActor<AWoTAngrealInvFireball>(
		FVector(0, 500, 100), FRotator::ZeroRotator, SP);   // far off, held in Items only
	Tainted->bTainted = true;
	Victim->InventoryComponent->Items.Add(Tainted);

	// The caster is cleansed too ("any weave currently affecting you").
	UWoTTestLeech* SelfCurse = NewObject<UWoTTestLeech>(Caster);
	SelfCurse->AttachTo(Caster);
	TestEqual(TEXT("caster starts cursed"), LeechCount(Caster), 1);

	// 2) Cast — the dome drops on the caster, expands, and strips the curses.
	Inv->UseAngreal();
	Inv->CeaseUsingAngreal();
	TestTrue(TEXT("casting dropped an anti-magic dome"), PearlCount() >= 1);
	WoTTest::TickWorld(World, 0.4f);   // let it expand out to the victim (~radius 50+)

	TestEqual(TEXT("the dome stripped the caster's curse"), LeechCount(Caster), 0);
	TestEqual(TEXT("the dome stripped the nearby victim's curse"), LeechCount(Victim), 0);
	TestFalse(TEXT("the victim's artifact was untainted"), Tainted->bTainted);

	// 3) The dome winds down and destroys itself after its lifecycle.
	WoTTest::TickWorld(World, 4.0f);
	TestEqual(TEXT("the dome self-destructs after its lifecycle"), PearlCount(), 0);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAMATest,
	"WoT.Inventory.AMA",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTAMATest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	auto LeechCount = [](AActor* H)
	{
		TArray<UWoTLeech*> A;
		H->GetComponents<UWoTLeech>(A);
		return A.Num();
	};
	auto PearlCount = [&]()
	{
		int32 N = 0;
		for (TActorIterator<AWoTAMAPearl> It(World); It; ++It) { N++; }
		return N;
	};
	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	// 1) Config.
	AWoTAngrealInvAMA* Inv = World->SpawnActor<AWoTAngrealInvAMA>(
		AWoTAngrealInvAMA::StaticClass(), FVector(0, 0, 50), FRotator::ZeroRotator, SP);
	if (!TestNotNull(TEXT("aura of unraveling spawned"), Inv))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	TestTrue(TEXT("AMA is all-element"),
		Inv->bElementFire && Inv->bElementWater && Inv->bElementAir
		&& Inv->bElementEarth && Inv->bElementSpirit);
	TestTrue(TEXT("AMA is defensive"), Inv->bDefensive);
	TestTrue(TEXT("AMA lobs the AMA land-mine"),
		Inv->ProjectileClass == AWoTAngrealAMAProjectile::StaticClass());

	// 2) The dead-zone: a pawn caught in the dome can't cast and no bad weave lands.
	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);

	AWoTAMAPearl* Dome = World->SpawnActor<AWoTAMAPearl>(
		AWoTAMAPearl::StaticClass(), FVector(0, 0, 100), FRotator::ZeroRotator, SP);
	Dome->SetSourceAngreal(Inv);
	Dome->Go();
	WoTTest::TickWorld(World, 0.1f);   // dome catches the pawn at its centre

	TestTrue(TEXT("inside the dome the pawn can't cast"), Pawn->IsCastingBlocked());

	// A deleterious weave can't attach; a benign one still can.
	UWoTTestLeech* Bad = NewObject<UWoTTestLeech>(Pawn);
	Bad->bDeleterious = true;
	Bad->AttachTo(Pawn);
	TestEqual(TEXT("the dome blocks a deleterious weave"), LeechCount(Pawn), 0);

	UWoTTestLeech* Benign = NewObject<UWoTTestLeech>(Pawn);
	Benign->bDeleterious = false;
	Benign->AttachTo(Pawn);
	TestEqual(TEXT("a benign weave still lands"), LeechCount(Pawn), 1);

	// 3) Leaving the dome lifts the dead-zone.
	Pawn->SetActorLocation(FVector(5000, 0, 100));
	WoTTest::TickWorld(World, 0.3f);   // poll notices the pawn left -> UnAffectPawn
	TestFalse(TEXT("outside the dome the pawn can cast again"), Pawn->IsCastingBlocked());
	UWoTTestLeech* NowOk = NewObject<UWoTTestLeech>(Pawn);
	NowOk->bDeleterious = true;
	NowOk->AttachTo(Pawn);
	TestEqual(TEXT("a deleterious weave lands once clear of the dome"), LeechCount(Pawn), 2);

	// 4) The land-mine: a pawn that walks onto the placed mine (its own 12uu
	//    collision, AngrealAMAProjectile.uc:33-43/:180) trips it and a dome is
	//    planted where the mine sat. (The 150uu TriggerRadius is never applied:
	//    WoT.AMA.RetailTouchRadius.)
	const int32 PearlsBefore = PearlCount();
	// (Far from step 2's dome, which absorbs any AngrealProjectile it reaches.)
	AWoTAngrealAMAProjectile* Mine = World->SpawnActor<AWoTAngrealAMAProjectile>(
		AWoTAngrealAMAProjectile::StaticClass(), FVector(8000, 0, 300),
		FRotator::ZeroRotator, SP);
	if (TestNotNull(TEXT("mine spawned"), Mine))
	{
		Mine->SetSourceAngreal(Inv);
		Mine->Place();   // land (PrimeTime 0)
		AWoTCharacter* Tripper = World->SpawnActor<AWoTCharacter>(
			FVector(8000, 200, 300), FRotator::ZeroRotator, SP);
		Tripper->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
		WoTTest::TickWorld(World, 0.1f);
		TestEqual(TEXT("a pawn 200uu away does not trip the mine"), PearlCount(), PearlsBefore);
		Tripper->SetActorLocation(FVector(8000, 0, 300));   // onto the mine
		WoTTest::TickWorld(World, 0.3f);
		// Retail consumption is the Fading dissolve (ScaleGlow -= 1.5/s,
		// AngrealAMAProjectile.uc Fading state), not an instant destroy.
		TestTrue(TEXT("the tripped mine dissolves before dying"),
			!IsValid(Mine) || Mine->IsFading());
		TestTrue(TEXT("tripping the mine planted a dome"), PearlCount() > PearlsBefore);
		WoTTest::TickWorld(World, 1.2f);
		TestFalse(TEXT("the mine is consumed after its dissolve"), IsValid(Mine));
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTZoneVolumeTest,
	"WoT.Zone.Volume",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTZoneVolumeTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	auto SpawnZone = [&](const FVector& At)
	{
		AWoTZoneVolume* Z = World->SpawnActor<AWoTZoneVolume>(
			AWoTZoneVolume::StaticClass(), At, FRotator::ZeroRotator, SP);
		Z->Collision->SetBoxExtent(FVector(300.0f));
		return Z;
	};

	// 1) Water: a pawn inside swims; a weave that enters fizzles (HitWater).
	AWoTZoneVolume* Water = SpawnZone(FVector(0, 0, 100));
	Water->bWaterZone = true;
	AWoTCharacter* Swimmer = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator, SP);
	Swimmer->GetCharacterMovement()->SetMovementMode(MOVE_Falling);
	// Swim entry is the CMC's predicted path, driven from
	// UpdateCharacterStateBeforeMovement — so an uncontrolled test pawn needs
	// bRunPhysicsWithNoController to run PerformMovement (the repo convention; the current /
	// gravity sub-tests below do the same). Real player/AI pawns are controlled, so they always
	// get it.
	Swimmer->GetCharacterMovement()->bRunPhysicsWithNoController = true;
	WoTTest::TickWorld(World, 0.1f);
	UCharacterMovementComponent* SwimMove = Swimmer->GetCharacterMovement();
	TestEqual(TEXT("a pawn in the water zone starts swimming"),
		(int32)SwimMove->MovementMode.GetValue(), (int32)MOVE_Swimming);

	// Inside the water box but clear of the swimmer's capsule (else it touch-explodes).
	AWoTFireballProjectile* Weave = World->SpawnActor<AWoTFireballProjectile>(
		AWoTFireballProjectile::StaticClass(), FVector(0, 150, 100), FRotator::ZeroRotator, SP);
	if (TestNotNull(TEXT("weave spawned"), Weave))
	{
		TWeakObjectPtr<AWoTFireballProjectile> WeavePtr = Weave;
		WoTTest::TickWorld(World, 0.05f);
		// HitWater fizzles the fire weave out (AngrealFireballProjectile.uc:190).
		TestFalse(TEXT("a fire weave entering the water fizzles out"), WeavePtr.IsValid());
	}

	// Leaving the water drops the swimmer back to falling.
	Swimmer->SetActorLocation(FVector(5000, 0, 100));
	WoTTest::TickWorld(World, 0.1f);
	TestEqual(TEXT("leaving the water stops swimming"),
		(int32)SwimMove->MovementMode.GetValue(), (int32)MOVE_Falling);

	// 2) Pain: a pawn inside bleeds DamagePerSec every PainInterval.
	AWoTZoneVolume* Lava = SpawnZone(FVector(2000, 0, 100));
	Lava->bPainZone = true;
	Lava->DamagePerSec = 30.0f;
	Lava->PainInterval = 0.2f;
	AWoTCharacter* Burned = World->SpawnActor<AWoTCharacter>(
		FVector(2000, 0, 100), FRotator::ZeroRotator, SP);
	Burned->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	const int32 HP0 = Burned->Health;
	WoTTest::TickWorld(World, 0.5f);   // >= 2 pain intervals
	TestTrue(FString::Printf(TEXT("the pain zone burned the pawn (%d -> %d)"), HP0, Burned->Health),
		Burned->Health < HP0);

	// 3) Current: retail ZoneVelocity is TRANSPORT — the pawn is displaced along it while
	// its velocity stays un-accumulated. Applied in the CMC's predicted Phys* path, so it
	// runs only while the CMC simulates movement; an unpossessed test pawn needs
	// bRunPhysicsWithNoController to run PerformMovement (the repo convention). Real
	// player/AI pawns are controlled, so they always get it.
	AWoTZoneVolume* Current = SpawnZone(FVector(4000, 0, 100));
	Current->ZoneVelocity = FVector(0, 400, 0);
	AWoTCharacter* Drifter = World->SpawnActor<AWoTCharacter>(
		FVector(4000, 0, 100), FRotator::ZeroRotator, SP);
	Drifter->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	Drifter->GetCharacterMovement()->bRunPhysicsWithNoController = true;
	const FVector DriftStart = Drifter->GetActorLocation();
	WoTTest::TickWorld(World, 0.2f);
	const float DriftY = Drifter->GetActorLocation().Y - DriftStart.Y;
	TestTrue(FString::Printf(TEXT("the current transports the pawn +Y (drift %.0f)"), DriftY),
		DriftY > 40.0f);   // 400 uu/s * 0.2s = 80
	TestTrue(FString::Printf(TEXT("transport does not accumulate into velocity (vel %.0f)"),
		Drifter->GetCharacterMovement()->Velocity.Y),
		FMath::Abs(Drifter->GetCharacterMovement()->Velocity.Y) < 25.0f);

	// 3b) Updraft (the Arena_11 shaft authors ZoneVelocity (-256,0,768)): a FALLING pawn is
	// carried up by the transport while gravity still owns its velocity. Regression
	// guard: a current accumulated inside CalcVelocity has its Z
	// discarded by PhysFalling, so the updraft was dead and the pawn just fell.
	AWoTZoneVolume* Shaft = SpawnZone(FVector(16000, 0, 100));
	Shaft->ZoneVelocity = FVector(-256, 0, 768);
	AWoTCharacter* Lifted = World->SpawnActor<AWoTCharacter>(
		FVector(16000, 0, 100), FRotator::ZeroRotator, SP);
	Lifted->GetCharacterMovement()->SetMovementMode(MOVE_Falling);
	Lifted->GetCharacterMovement()->bRunPhysicsWithNoController = true;
	const float LiftStartZ = Lifted->GetActorLocation().Z;
	WoTTest::TickWorld(World, 0.25f);
	const float Rise = Lifted->GetActorLocation().Z - LiftStartZ;
	// transport +768*0.25 = +192, gravity arc -0.5*950*0.25^2 = -30 -> ~ +160 net
	TestTrue(FString::Printf(TEXT("a falling pawn rides the updraft up (rise %.0f)"), Rise),
		Rise > 80.0f);
	TestTrue(FString::Printf(TEXT("gravity still owns the airborne velocity (vz %.0f)"),
		Lifted->GetCharacterMovement()->Velocity.Z),
		Lifted->GetCharacterMovement()->Velocity.Z < -100.0f);

	// 4) Gravity: a gravity zone REPLACES gravity while FALLING — GetGravityZ hands the
	// zone's vertical gravity to the engine's own falling integrator (the path whose Z
	// survives PhysFalling), and the lateral part of an arbitrary-direction zone gravity
	// folds in the falling override. Vertical number from the shipped low-grav zone
	// (Battle2_03 authors (0,0,-500) vs the -950 default).
	AWoTZoneVolume* Grav = SpawnZone(FVector(6000, 0, 100));
	Grav->bOverrideGravity = true;
	Grav->ZoneGravity = FVector(500, 0, -500);   // low-grav fall + a sideways component
	AWoTCarcass* FallingCarcass = World->SpawnActor<AWoTCarcass>(
		FVector(6000, -180, 100), FRotator::ZeroRotator, SP);
	FallingCarcass->SetVelocityForTest(FVector::ZeroVector);
	AWoTCreatureChunk* FallingGib = World->SpawnActor<AWoTCreatureChunk>(
		FVector(6000, -90, 100), FRotator::ZeroRotator, SP);
	FallingGib->SetVelocityForTest(FVector::ZeroVector);
	AWoTFragment* FallingFragment = World->SpawnActor<AWoTFragment>(
		FVector(6000, 90, 100), FRotator::ZeroRotator, SP);
	FallingFragment->Init(EWoTFragmentType::Wall, 200.0f,
		FVector::ZeroVector, 1.0f);
	FallingFragment->SetFragmentVelocity(FVector::ZeroVector);
	AWoTEarthTremorProjectile* FallingTremor =
		World->SpawnActor<AWoTEarthTremorProjectile>(
			FVector(6000, 150, 100), FRotator::ZeroRotator, SP);
	FallingTremor->ProjectileMovement->Velocity = FVector::ZeroVector;
	AWoTFireworksProjectile* FallingFireworks =
		World->SpawnActor<AWoTFireworksProjectile>(
			FVector(6000, 220, 100), FRotator::ZeroRotator, SP);
	FallingFireworks->ProjectileMovement->Velocity = FVector::ZeroVector;
	AWoTAngrealAMAProjectile* FallingAMA =
		World->SpawnActor<AWoTAngrealAMAProjectile>(
			FVector(6000, -260, 100), FRotator::ZeroRotator, SP);
	FallingAMA->ProjectileMovement->Velocity = FVector::ZeroVector;
	AWoTCharacter* Faller = World->SpawnActor<AWoTCharacter>(
		FVector(6000, 0, 100), FRotator::ZeroRotator, SP);
	Faller->GetCharacterMovement()->SetMovementMode(MOVE_Falling);
	Faller->GetCharacterMovement()->bRunPhysicsWithNoController = true;
	WoTTest::TickWorld(World, 0.2f);
	TestTrue(FString::Printf(TEXT("zone gravity replaces the fall (vz %.0f, want ~-100 not -190)"),
		Faller->GetCharacterMovement()->Velocity.Z),
		FMath::Abs(Faller->GetCharacterMovement()->Velocity.Z - (-100.0f)) < 20.0f);
	TestTrue(FString::Printf(TEXT("lateral zone gravity pulls +X while falling (vx %.0f)"),
		Faller->GetCharacterMovement()->Velocity.X),
		Faller->GetCharacterMovement()->Velocity.X > 50.0f);
	auto UsesZoneGravity = [this](const TCHAR* Label, const FVector& Velocity)
	{
		TestTrue(FString::Printf(TEXT("%s uses lateral zone gravity (vx %.1f)"),
			Label, Velocity.X), Velocity.X > 50.0f);
		TestTrue(FString::Printf(TEXT("%s uses -500 zone gravity (vz %.1f)"),
			Label, Velocity.Z), Velocity.Z < -80.0f && Velocity.Z > -120.0f);
	};
	UsesZoneGravity(TEXT("carcass"), FallingCarcass->GetVelocityForTest());
	const FVector GibVelocity = FallingGib->GetVelocityForTest();
	TestTrue(FString::Printf(TEXT("gib uses lateral half-step zone gravity (vx %.1f)"),
		GibVelocity.X), GibVelocity.X > 25.0f);
	TestTrue(FString::Printf(TEXT("gib uses retail half-step -500 zone gravity (vz %.1f)"),
		GibVelocity.Z), GibVelocity.Z < -40.0f && GibVelocity.Z > -60.0f);
	UsesZoneGravity(TEXT("fragment"), FallingFragment->GetFragmentVelocity());
	UsesZoneGravity(TEXT("EarthTremor"), FallingTremor->ProjectileMovement->Velocity);
	UsesZoneGravity(TEXT("Fireworks"), FallingFireworks->ProjectileMovement->Velocity);
	UsesZoneGravity(TEXT("AMA"), FallingAMA->ProjectileMovement->Velocity);

	// ...and leaving hands gravity back to the pawn's OWN parity arc — the -950 fall
	// (950/980 scale), with the scale itself never clobbered.
	Faller->SetActorLocation(FVector(9000, 0, 100));
	AWoTAngrealAMAProjectile* DefaultGravityAMA =
		World->SpawnActor<AWoTAngrealAMAProjectile>(
			FVector(9000, 200, 100), FRotator::ZeroRotator, SP);
	DefaultGravityAMA->ProjectileMovement->Velocity = FVector::ZeroVector;
	WoTTest::TickWorld(World, 0.1f);
	TestEqual(TEXT("zone exit leaves the pawn's own gravity scale untouched"),
		Faller->GetCharacterMovement()->GravityScale, 950.0f / 980.0f);
	TestTrue(FString::Printf(TEXT("zone exit restores the -950 arc (GetGravityZ %.0f)"),
		Faller->GetCharacterMovement()->GetGravityZ()),
		FMath::Abs(Faller->GetCharacterMovement()->GetGravityZ() + 950.0f) < 1.0f);
	TestTrue(FString::Printf(TEXT("AMA uses default retail -950 gravity (vz %.1f)"),
		DefaultGravityAMA->ProjectileMovement->Velocity.Z),
		DefaultGravityAMA->ProjectileMovement->Velocity.Z < -80.0f
			&& DefaultGravityAMA->ProjectileMovement->Velocity.Z > -110.0f);

	// 5) Heal (WOTZoneInfo bHealZone/HealPerSec): +HealPerSec on the pain-timer
	// cadence, capped at full health.
	AWoTZoneVolume* Spring = SpawnZone(FVector(12000, 0, 100));
	Spring->bHealZone = true;
	Spring->HealPerSec = 5;
	Spring->PainInterval = 0.2f;
	AWoTCharacter* Bather = World->SpawnActor<AWoTCharacter>(
		FVector(12000, 0, 100), FRotator::ZeroRotator, SP);
	Bather->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	Bather->Health = 50;
	WoTTest::TickWorld(World, 0.5f);
	TestTrue(FString::Printf(TEXT("the heal zone healed the pawn (50 -> %d)"), Bather->Health),
		Bather->Health > 50);
	Bather->Health = 100;
	WoTTest::TickWorld(World, 0.5f);
	TestEqual(TEXT("healing caps at full health"), Bather->Health, 100);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTLightningTest,
	"WoT.Inventory.Lightning",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTLightningTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	auto Leeches = [](AActor* H)
	{
		TArray<UWoTDecreaseHealthLeech*> A;
		H->GetComponents<UWoTDecreaseHealthLeech>(A);
		return A.Num();
	};
	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AWoTCharacter* Caster = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	Caster->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	UWoTInventoryComponent* Inv = Caster->InventoryComponent;

	World->SpawnActor<AWoTAngrealInvLightning>(FVector(0, 0, 100), FRotator::ZeroRotator);
	WoTTest::TickWorld(World, 0.2f);
	AWoTAngrealInvLightning* Held = Cast<AWoTAngrealInvLightning>(Inv->SelectedItem.Get());
	if (!TestNotNull(TEXT("chain lightning picked up"), Held))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	// 1) Config.
	TestTrue(TEXT("lightning is uncommon"), Held->bUncommon);
	TestTrue(TEXT("lightning is fire+water+air"),
		Held->bElementFire && Held->bElementWater && Held->bElementAir);

	// A victim in range (LOS clear — no geometry) and one far out of range.
	AWoTCharacter* Near = World->SpawnActor<AWoTCharacter>(
		FVector(120, 0, 100), FRotator::ZeroRotator, SP);   // within AttachmentRange 180
	Near->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	Near->Health = 1000;
	AWoTCharacter* Far = World->SpawnActor<AWoTCharacter>(
		FVector(2000, 0, 100), FRotator::ZeroRotator, SP);  // way beyond range
	Far->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	Far->Health = 1000;

	// 2) Cast: the aura latches the nearby pawn and drains it; the far one is safe.
	Inv->UseAngreal();
	WoTTest::TickWorld(World, 0.5f);
	TestEqual(TEXT("the nearby pawn is latched"), Leeches(Near), 1);
	TestEqual(TEXT("the far pawn is not latched"), Leeches(Far), 0);
	TestTrue(FString::Printf(TEXT("the latched pawn is drained (%d)"), Near->Health),
		Near->Health < 1000);
	TestEqual(TEXT("the far pawn is undamaged"), Far->Health, 1000);

	// 3) A victim dragged past MaxEffectiveRange (900) snaps free.
	Near->SetActorLocation(FVector(3000, 0, 100));
	WoTTest::TickWorld(World, 0.1f);
	TestEqual(TEXT("an out-of-range victim's link snaps"), Leeches(Near), 0);

	// 4) Releasing the aura drops every remaining link.
	AWoTCharacter* V2 = World->SpawnActor<AWoTCharacter>(
		FVector(100, 0, 100), FRotator::ZeroRotator, SP);
	V2->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	WoTTest::TickWorld(World, 0.35f);
	TestEqual(TEXT("a fresh victim latched"), Leeches(V2), 1);
	Inv->CeaseUsingAngreal();
	TestEqual(TEXT("uncasting drops the link"), Leeches(V2), 0);

	// 5) Water: a victim wading in is shocked (removed + hit), and it electrocutes
	//    others standing in the same water.
	AWoTZoneVolume* Pool = World->SpawnActor<AWoTZoneVolume>(
		AWoTZoneVolume::StaticClass(), FVector(0, 4000, 100), FRotator::ZeroRotator, SP);
	Pool->Collision->SetBoxExtent(FVector(400.0f));
	Pool->bWaterZone = true;

	// re-cast, latch a pawn standing in the pool
	AWoTCharacter* Wader = World->SpawnActor<AWoTCharacter>(
		FVector(0, 4000, 100), FRotator::ZeroRotator, SP);
	Wader->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	Wader->Health = 1000;
	AWoTCharacter* Bystander = World->SpawnActor<AWoTCharacter>(
		FVector(200, 4000, 100), FRotator::ZeroRotator, SP);   // also in the pool
	Bystander->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	Bystander->Health = 1000;
	if (Caster->SetActorLocation(FVector(0, 4050, 100)))
	{
		// Harness teleport: match production SetLocation seams explicitly now
		// that AWoTCharacter no longer shadows AActor::SetActorLocation.
		UWoTZoneIndex::RefreshAfterRelocation(Caster);
	}
	Inv->UseAngreal();
	WoTTest::TickWorld(World, 0.35f);
	TestEqual(TEXT("the wader is latched"), Leeches(Wader), 1);
	const int32 WaderHP = Wader->Health;
	const int32 ByHP = Bystander->Health;
	WoTTest::TickWorld(World, 0.6f);   // > water poll (0.5s)
	TestTrue(FString::Printf(TEXT("the wader is shocked in water (%d->%d)"), WaderHP, Wader->Health),
		Wader->Health < WaderHP);
	TestEqual(TEXT("the shocked wader's link snapped"), Leeches(Wader), 0);
	TestTrue(FString::Printf(TEXT("the shock arcs to a bystander in the water (%d->%d)"),
		ByHP, Bystander->Health), Bystander->Health < ByHP);

	// 6) Retaliation (DamageNotifier): a victim that hurts the caster snaps its own
	//    link; the caster hurting itself drops the whole aura.
	AWoTCharacter* RC = World->SpawnActor<AWoTCharacter>(
		FVector(0, 6000, 100), FRotator::ZeroRotator, SP);
	RC->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	AWoTCharacter* RV = World->SpawnActor<AWoTCharacter>(
		FVector(120, 6000, 100), FRotator::ZeroRotator, SP);
	RV->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	RV->Health = 1000;
	AWoTAngrealInvLightning* RAura = World->SpawnActor<AWoTAngrealInvLightning>(
		AWoTAngrealInvLightning::StaticClass(), FVector(0, 6000, 100), FRotator::ZeroRotator, SP);
	RAura->SetOwner(RC);
	RC->InventoryComponent->Items.Add(RAura);   // so the damage hook finds it
	RAura->CastArtifact();
	WoTTest::TickWorld(World, 0.4f);
	TestEqual(TEXT("retaliation setup: victim latched"), Leeches(RV), 1);

	FWoTDamageEvent Retal;
	Retal.WoTDamageType = TEXT("hurt");
	RC->TakeDamage(10.0f, Retal, nullptr, RV);   // RV fights back (causer = RV)
	TestEqual(TEXT("a victim that hits the caster snaps its own link"), Leeches(RV), 0);

	WoTTest::TickWorld(World, 0.4f);   // aura re-latches the still-close victim
	TestEqual(TEXT("the aura re-latches the freed victim"), Leeches(RV), 1);
	RC->TakeDamage(10.0f, Retal, nullptr, RC);   // caster hurts itself
	TestEqual(TEXT("self-damage drops the whole aura"), Leeches(RV), 0);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTLightningLoopLifecycleSpec,
	"WoT.Inventory.LightningLoopLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTLightningLoopLifecycleSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTCharacter* Caster = World->SpawnActor<AWoTCharacter>(
		FVector(1000.0f, 1000.0f, 100.0f), FRotator::ZeroRotator, SpawnParams);
	AWoTCharacter* Victim = World->SpawnActor<AWoTCharacter>(
		FVector(1120.0f, 1000.0f, 100.0f), FRotator::ZeroRotator, SpawnParams);
	AWoTAngrealInvLightning* Aura = World->SpawnActor<AWoTAngrealInvLightning>(
		AWoTAngrealInvLightning::StaticClass(), Caster->GetActorLocation(),
		FRotator::ZeroRotator, SpawnParams);
	if (!Caster || !Victim || !TestNotNull(TEXT("lightning aura spawned"), Aura))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Caster->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	Victim->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	Victim->Health = 1000;
	Aura->SetOwner(Caster);
	Aura->CurCharges = 30;

	auto CountVictimLoops = [Victim]()
	{
		TArray<UWoTAmbientSoundComponent*> Components;
		Victim->GetComponents<UWoTAmbientSoundComponent>(Components);
		return Components.Num();
	};

	Aura->CastArtifact();
	WoTTest::TickWorld(World, 0.05f);
	if (!TestEqual(TEXT("first cast attaches one lightning leech"),
		Aura->Leeches.Num(), 1))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	UWoTDecreaseHealthLeech* FirstLeech = Aura->Leeches[0];
	TWeakObjectPtr<UWoTAmbientSoundComponent> FirstHum = FirstLeech->AmbientHum;
	TestEqual(TEXT("lightning leech stamps the LoopLS path"),
		FirstLeech->AmbientSoundPath, FName(TEXT("Angreal/LoopLS")));
	TestTrue(TEXT("leech attach starts its LoopLS component"), FirstHum.IsValid());
	if (FirstHum.IsValid())
	{
		TestNotNull(TEXT("LoopLS component resolves its sound"), FirstHum->Sound.Get());
	}
	TestEqual(TEXT("leech attach starts exactly one victim loop"),
		CountVictimLoops(), 1);

	Aura->CastArtifact();
	TestEqual(TEXT("a duplicate cast while active keeps one leech"),
		Aura->Leeches.Num(), 1);
	TestEqual(TEXT("a duplicate cast while active keeps one audio component"),
		CountVictimLoops(), 1);
	TestEqual(TEXT("a duplicate cast does not replace/restart the loop"),
		Aura->Leeches[0]->AmbientHum.Get(), FirstHum.Get());

	Aura->UnCastArtifact();
	TestTrue(TEXT("detach/uncast stops and destroys the first loop"),
		!FirstHum.IsValid() || FirstHum->IsBeingDestroyed());
	TestEqual(TEXT("detach/uncast leaves no victim loop"), CountVictimLoops(), 0);

	Aura->CastArtifact();
	WoTTest::TickWorld(World, 0.05f);
	TestEqual(TEXT("second cast on the same victim attaches one leech"),
		Aura->Leeches.Num(), 1);
	TestEqual(TEXT("second cast on the same victim starts only one loop"),
		CountVictimLoops(), 1);

	UWoTDecreaseHealthLeech* ExpiringLeech = Aura->Leeches[0];
	TWeakObjectPtr<UWoTAmbientSoundComponent> ExpiringHum = ExpiringLeech->AmbientHum;
	ExpiringLeech->Lifespan = 0.01f;
	ExpiringLeech->TickComponent(0.02f, LEVELTICK_All, nullptr);
	TestTrue(TEXT("leech expiry stops and destroys its loop"),
		!ExpiringHum.IsValid() || ExpiringHum->IsBeingDestroyed());
	Aura->UpdateLeeches();
	TestEqual(TEXT("expired leech is pruned from the aura"), Aura->Leeches.Num(), 0);

	Aura->AttemptLatch();
	if (TestEqual(TEXT("victim-death setup reattaches one leech"),
		Aura->Leeches.Num(), 1))
	{
		TWeakObjectPtr<UWoTAmbientSoundComponent> DeathHum =
			Aura->Leeches[0]->AmbientHum;
		FWoTDamageEvent DeathEvent;
		DeathEvent.WoTDamageType = FName(TEXT("xEFWA"));
		Victim->TakeDamage(5000.0f, DeathEvent, nullptr, Caster);
		TestTrue(TEXT("victim death stops and destroys the loop"),
			!DeathHum.IsValid() || DeathHum->IsBeingDestroyed());
		TestEqual(TEXT("victim death leaves no ambient component"),
			CountVictimLoops(), 0);
		Aura->UpdateLeeches();
		TestEqual(TEXT("victim-death leech is pruned from the aura"),
			Aura->Leeches.Num(), 0);
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTLightningBeamLifecycleSpec,
	"WoT.Inventory.LightningBeamLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTLightningBeamLifecycleSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTCharacter* Caster = World->SpawnActor<AWoTCharacter>(
		FVector(2000.0f, 3000.0f, 100.0f), FRotator::ZeroRotator, SpawnParams);
	AWoTCharacter* Victim = World->SpawnActor<AWoTCharacter>(
		FVector(2120.0f, 3000.0f, 100.0f), FRotator::ZeroRotator, SpawnParams);
	AWoTAngrealInvLightning* Aura = World->SpawnActor<AWoTAngrealInvLightning>(
		AWoTAngrealInvLightning::StaticClass(), Caster->GetActorLocation(),
		FRotator::ZeroRotator, SpawnParams);
	if (!Caster || !Victim || !TestNotNull(TEXT("beam-test aura spawned"), Aura))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Caster->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	Victim->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	Victim->Health = 1000;
	Aura->SetOwner(Caster);
	Aura->CurCharges = 30;
	Aura->CastArtifact();
	WoTTest::TickWorld(World, 0.05f);

	if (!TestEqual(TEXT("one latched victim owns one bolt"), Aura->Bolts.Num(), 1))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	AWoTBeamEffect* Bolt = Aura->Bolts.FindRef(Victim).Get();
	if (!TestNotNull(TEXT("victim bolt is tracked"), Bolt))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	TWeakObjectPtr<AWoTBeamEffect> BoltWeak = Bolt;
	TestEqual(TEXT("latched bolt uses LightningStreak02"), Bolt->Config,
		FName(TEXT("LightningStreak02")));
	TestEqual(TEXT("bolt start follows the caster"), Bolt->StartActor.Get(),
		static_cast<AActor*>(Caster));
	TestEqual(TEXT("bolt end follows the victim"), Bolt->EndActor.Get(),
		static_cast<AActor*>(Victim));

	const FVector StartAnchor =
		Caster->GetActorLocation() + FVector(0.0f, 0.0f, Caster->BaseEyeHeight);
	const FVector EndAnchor = Victim->GetActorLocation();
	const FVector Closest = FMath::ClosestPointOnSegment(
		Bolt->GetActorLocation(), StartAnchor, EndAnchor);
	TestFalse(TEXT("bolt relevancy origin is not the world origin"),
		Bolt->GetActorLocation().IsNearlyZero());
	TestTrue(TEXT("bolt relevancy origin lies between its anchors"),
		Bolt->GetActorLocation().Equals(Closest, 50.0f));

	int32 VisibleSegmentCount = 0;
	for (UActorComponent* Component : Bolt->GetComponents())
	{
		UWoTVertexAnimComponent* Segment = Cast<UWoTVertexAnimComponent>(Component);
		if (!Segment || !Segment->IsVisible())
		{
			continue;
		}
		++VisibleSegmentCount;
		if (USkeletalMesh* SegmentMesh = Segment->GetSkeletalMeshAsset())
		{
			TestEqual(TEXT("visible bolt segment binds LSbolt"),
				SegmentMesh->GetName(), FString(TEXT("LSbolt")));
		}
		else
		{
			AddError(TEXT("visible bolt segment has no bound mesh"));
		}
		TestFalse(TEXT("visible segment is not owner-hidden"), Segment->bOwnerNoSee);
		TestFalse(TEXT("visible segment is not at the world origin"),
			Segment->GetComponentLocation().IsNearlyZero());
	}
	TestTrue(TEXT("spawned bolt binds at least one visible segment"),
		VisibleSegmentCount > 0);

	if (Aura->Leeches.Num() == 1)
	{
		Aura->Leeches[0]->Unattach();
	}
	Aura->UpdateLeeches();
	TestEqual(TEXT("dropping the victim removes its bolt from the set"),
		Aura->Bolts.Num(), 0);
	TestTrue(TEXT("dropping the victim destroys the spawned bolt"),
		!BoltWeak.IsValid() || BoltWeak->IsActorBeingDestroyed());

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSwapPlacesTransferTest,
	"WoT.Inventory.SwapPlacesTransfer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTSwapPlacesTransferTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	auto Leeches = [](AActor* H)
	{
		TArray<UWoTLeech*> A;
		H->GetComponents<UWoTLeech>(A);
		return A.Num();
	};
	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AWoTCharacter* Caster = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	Caster->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	AWoTCharacter* Target = World->SpawnActor<AWoTCharacter>(
		FVector(500, 0, 100), FRotator::ZeroRotator, SP);
	Target->GetCharacterMovement()->SetMovementMode(MOVE_Flying);

	// Target carries a bad (removable, non-projectile) weave + a projectile weave;
	// the caster carries a bad weave.
	UWoTTestLeech* TgtBad = NewObject<UWoTTestLeech>(Target);
	TgtBad->bDeleterious = true;
	TgtBad->bDisplayIcon = true;
	TgtBad->StatusIconFrame = FSoftObjectPath(
		TEXT("/Game/WoT/Textures/Angreal/M_Whirlwind.M_Whirlwind"));
	TgtBad->AttachTo(Target);
	UWoTTestLeech* TgtProj = NewObject<UWoTTestLeech>(Target);
	TgtProj->bDeleterious = true;
	TgtProj->bFromProjectile = true;   // stays put — projectile weaves don't swap
	TgtProj->AttachTo(Target);
	UWoTTestLeech* CasterBad = NewObject<UWoTTestLeech>(Caster);
	CasterBad->bDeleterious = true;
	CasterBad->AttachTo(Caster);
	TestEqual(TEXT("target starts with two weaves"), Leeches(Target), 2);
	TestEqual(TEXT("caster starts with one weave"), Leeches(Caster), 1);
	TestTrue(TEXT("the target starts with the displayed leech icon"),
		Target->StatusIcons.Num() == 1 &&
		Target->StatusIcons[0].IconID == TgtBad->GetFName());

	AWoTSwapPlacesProjectile* Proj = World->SpawnActor<AWoTSwapPlacesProjectile>(
		AWoTSwapPlacesProjectile::StaticClass(), FVector(250, 0, 100), FRotator::ZeroRotator, SP);
	Proj->PerformSwap(Caster, Target);

	// The bad weaves swapped hosts (identity preserved); the projectile one stayed.
	TestTrue(TEXT("the target's bad weave moved onto the caster"),
		TgtBad->GetOwner() == Caster);
	TestTrue(TEXT("the caster's bad weave moved onto the target"),
		CasterBad->GetOwner() == Target);
	TestTrue(TEXT("the projectile weave stayed on the target"),
		TgtProj->GetOwner() == Target);
	TestEqual(TEXT("caster now carries one weave"), Leeches(Caster), 1);
	TestEqual(TEXT("target now carries two weaves"), Leeches(Target), 2);
	TestEqual(TEXT("the displayed leech left no stale icon on its old host"),
		Target->StatusIcons.Num(), 0);
	TestTrue(TEXT("the displayed leech added exactly one icon on its new host"),
		Caster->StatusIcons.Num() == 1 &&
		Caster->StatusIcons[0].IconID == TgtBad->GetFName());

	// A bad removable non-projectile reflector transfers too.
	UWoTNoCastReflector* Ref = NewObject<UWoTNoCastReflector>(Target);
	Ref->bDeleterious = true;
	Ref->StatusIconFrame = FSoftObjectPath(
		TEXT("/Game/WoT/Textures/Angreal/M_Reflect.M_Reflect"));
	Ref->Install(Target);
	AWoTSwapPlacesProjectile* Proj2 = World->SpawnActor<AWoTSwapPlacesProjectile>(
		AWoTSwapPlacesProjectile::StaticClass(), FVector(250, 0, 100), FRotator::ZeroRotator, SP);
	Proj2->PerformSwap(Caster, Target);
	TestTrue(TEXT("a bad reflector swapped hosts too"), Ref->GetOwner() == Caster);
	TestTrue(TEXT("reflector transfer leaves only its new-host icon"),
		Caster->StatusIcons.Num() == 1 &&
		Caster->StatusIcons[0].IconID == Ref->GetFName());
	TestTrue(TEXT("reflector transfer removes its old-host icon before rename"),
		Target->StatusIcons.Num() == 1 &&
		Target->StatusIcons[0].IconID == TgtBad->GetFName());

	// Integration: a live Chain Lightning aura's drain link follows the victim it's
	// swapped with, and keeps being tracked (same leech object, new host).
	AWoTCharacter* LCaster = World->SpawnActor<AWoTCharacter>(
		FVector(0, 3000, 100), FRotator::ZeroRotator, SP);
	LCaster->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	AWoTCharacter* Victim = World->SpawnActor<AWoTCharacter>(
		FVector(120, 3000, 100), FRotator::ZeroRotator, SP);   // within AttachmentRange
	Victim->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	Victim->Health = 1000;
	AWoTAngrealInvLightning* Aura = World->SpawnActor<AWoTAngrealInvLightning>(
		AWoTAngrealInvLightning::StaticClass(), FVector(0, 3000, 100), FRotator::ZeroRotator, SP);
	Aura->SetOwner(LCaster);
	Aura->CastArtifact();
	WoTTest::TickWorld(World, 0.4f);
	TestEqual(TEXT("the aura latched the victim"), Leeches(Victim), 1);

	AWoTCharacter* Bystander = World->SpawnActor<AWoTCharacter>(
		FVector(400, 3000, 100), FRotator::ZeroRotator, SP);   // out of the aura's latch range
	Bystander->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	AWoTSwapPlacesProjectile* Proj3 = World->SpawnActor<AWoTSwapPlacesProjectile>(
		AWoTSwapPlacesProjectile::StaticClass(), FVector(250, 3000, 100), FRotator::ZeroRotator, SP);
	Proj3->PerformSwap(Bystander, Victim);   // swap the drained victim with the bystander
	TestEqual(TEXT("the lightning link left the victim"), Leeches(Victim), 0);
	TestEqual(TEXT("the lightning link followed onto the swapped-in pawn"),
		Leeches(Bystander), 1);

	// A STATEFUL leech transfer must restore the old host: a whirlwind seizes
	// movement (MOVE_None), and retail UnAttaches before re-attaching
	// (SwapPlacesEffect.uc:294 -> WhirlwindLeech.uc:83-97 restores physics). A
	// transfer that skipped the restore left the old victim frozen forever.
	AWoTCharacter* Spun = World->SpawnActor<AWoTCharacter>(
		FVector(0, 6000, 100), FRotator::ZeroRotator, SP);
	Spun->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	AWoTCharacter* Fresh = World->SpawnActor<AWoTCharacter>(
		FVector(500, 6000, 100), FRotator::ZeroRotator, SP);
	Fresh->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	UWoTWhirlwindLeech* Spin = NewObject<UWoTWhirlwindLeech>(Spun);
	Spin->AttachTo(Spun);
	WoTTest::TickWorld(World, 0.1f);
	TestEqual(TEXT("whirlwind seized the victim's movement"),
		(int32)Spun->GetCharacterMovement()->MovementMode.GetValue(), (int32)MOVE_None);
	AWoTSwapPlacesProjectile* Proj4 = World->SpawnActor<AWoTSwapPlacesProjectile>(
		AWoTSwapPlacesProjectile::StaticClass(), FVector(250, 6000, 100), FRotator::ZeroRotator, SP);
	Proj4->PerformSwap(Spun, Fresh);
	TestTrue(TEXT("the whirlwind moved to the swapped-in pawn"),
		Spin->GetOwner() == Fresh);
	TestTrue(TEXT("the old host's movement was restored (not frozen forever)"),
		Spun->GetCharacterMovement()->MovementMode != MOVE_None);
	TestEqual(TEXT("the new host is seized instead"),
		(int32)Fresh->GetCharacterMovement()->MovementMode.GetValue(), (int32)MOVE_None);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTTrapDetectTest,
	"WoT.Inventory.TrapDetect",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTTrapDetectTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	auto Detector = [](AActor* H) { return H->FindComponentByClass<UWoTTrapDetectLeech>(); };
	auto Ward = [&](const FVector& At, bool bArmed)
	{
		AWoTExpWardProjectile* W = World->SpawnActor<AWoTExpWardProjectile>(
			AWoTExpWardProjectile::StaticClass(), At, FRotator::ZeroRotator, SP);
		W->bArmed = bArmed;
		return W;
	};

	AWoTCharacter* Caster = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	Caster->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	UWoTInventoryComponent* Inv = Caster->InventoryComponent;

	World->SpawnActor<AWoTAngrealInvTrapDetect>(FVector(0, 0, 100), FRotator::ZeroRotator);
	WoTTest::TickWorld(World, 0.2f);
	AWoTAngrealInvTrapDetect* Held = Cast<AWoTAngrealInvTrapDetect>(Inv->SelectedItem.Get());
	if (!TestNotNull(TEXT("trap detect picked up"), Held))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	// 1) Config.
	TestTrue(TEXT("trap detect is common"), Held->bCommon);
	TestTrue(TEXT("trap detect is a traps artifact"), Held->bTraps);

	// 2) Cast attaches the detector.
	Inv->UseAngreal();
	UWoTTrapDetectLeech* Leech = Detector(Caster);
	if (!TestNotNull(TEXT("casting attached the detector"), Leech))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	// 3) It flags an armed ward in range, ignores an unarmed one, and prefers the
	//    closer trap (with a quicker heartbeat).
	AWoTExpWardProjectile* Far = Ward(FVector(500, 0, 100), true);
	Ward(FVector(300, 0, 100), false);   // unarmed — ignored
	WoTTest::TickWorld(World, 0.1f);
	TestTrue(TEXT("the armed ward in range is detected"), Leech->ClosestTrap == Far);

	// Retail selection is VIEW-CENTERED (max ViewDist/LocDist), not
	// nearest-first: equally-centered candidates tie on the ratio and the
	// FIRST iterated wins (order-dependent in retail too). Make the case
	// deterministic: the near ward sits ON the view axis, the far ward is
	// pushed OFF-axis, so the centered pick IS the near one.
	Far->SetActorLocation(FVector(500, 400, 100));
	AWoTExpWardProjectile* Near = Ward(FVector(150, 0, 100), true);
	WoTTest::TickWorld(World, 0.1f);
	TestTrue(TEXT("the view-centered armed ward wins"), Leech->ClosestTrap == Near);
	TestTrue(FString::Printf(TEXT("the heartbeat quickens up close (res %.2f)"),
		Leech->AffectResolution), Leech->AffectResolution < 1.0f);

	// 4) Nothing within DetectRadius → no trap flagged.
	Far->SetActorLocation(FVector(6000, 0, 100));
	Near->SetActorLocation(FVector(7000, 0, 100));
	WoTTest::TickWorld(World, 0.1f);
	TestNull(TEXT("nothing is flagged out of range"), Leech->ClosestTrap.Get());

	// 5) Deployed AWoTTrap actors are detected by TrapType (the TrapDetectLeech.uc:286-291 table;
	//    the "fix on landing" scan extension). A Spear is always deployed.
	AWoTSpear* Spear = World->SpawnActor<AWoTSpear>(
		AWoTSpear::StaticClass(), FVector(400, 0, 100), FRotator::ZeroRotator, SP);
	WoTTest::TickWorld(World, 0.1f);
	TestTrue(TEXT("a deployed spear trap is detected"), Leech->ClosestTrap == Spear);

	// An INACTIVE staircase is undeployed (IsTrapDeployed == Active) — invisible to the detector
	// even when nearer; activating it makes it the closest detectable trap.
	AWoTStaircase* Stairs = World->SpawnActor<AWoTStaircase>(
		AWoTStaircase::StaticClass(), FVector(200, 0, 100), FRotator::ZeroRotator, SP);
	// Keep the spear off the view axis so the centered pick is deterministic
	// (see the view-centered note above).
	Spear->SetActorLocation(FVector(400, 350, 100));
	WoTTest::TickWorld(World, 0.1f);
	TestTrue(TEXT("an inactive staircase is not detected"), Leech->ClosestTrap == Spear);
	Stairs->Activate();
	WoTTest::TickWorld(World, 0.1f);
	TestTrue(TEXT("the activated staircase wins as the view-centered trap"), Leech->ClosestTrap == Stairs);

	// Types OUTSIDE the table stay invisible: the Alarm ('Alarm' not tabled) and a FireWallSlab
	// ('FireWallSlab' not tabled — shipped firewalls were NOT detectable; the class-name fallback,
	// not the WallSlab tag, is the spec).
	Spear->Destroy();
	Stairs->Destroy();
	World->SpawnActor<AWoTAlarm>(AWoTAlarm::StaticClass(),
	                             FVector(250, 0, 100), FRotator::ZeroRotator, SP);
	World->SpawnActor<AWoTFireWallSlab>(AWoTFireWallSlab::StaticClass(),
	                                    FVector(220, 0, 100), FRotator::ZeroRotator, SP);
	WoTTest::TickWorld(World, 0.1f);
	TestNull(TEXT("alarm + firewall are not in the detect table"), Leech->ClosestTrap.Get());

	// 6) The detector wears off after its lifespan.
	Leech->Lifespan = 0.2f;
	WoTTest::TickWorld(World, 0.3f);
	TestNull(TEXT("the detector expires"), Detector(Caster));

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTDisguiseTest,
	"WoT.Inventory.Disguise",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTDisguiseTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AWoTCharacter* Caster = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	Caster->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	UWoTInventoryComponent* Inv = Caster->InventoryComponent;

	World->SpawnActor<AWoTAngrealInvDisguise>(FVector(0, 0, 100), FRotator::ZeroRotator);
	WoTTest::TickWorld(World, 0.2f);
	AWoTAngrealInvDisguise* Held = Cast<AWoTAngrealInvDisguise>(Inv->SelectedItem.Get());
	if (!TestNotNull(TEXT("disguise picked up"), Held))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	// 1) Config.
	TestTrue(TEXT("disguise is rare"), Held->bRare);
	TestTrue(TEXT("disguise is Spirit-element"), Held->bElementSpirit);

	// 2) With no one else around there's nothing to mimic — the cast fails.
	Inv->UseAngreal();
	TestNull(TEXT("nothing to mimic -> not disguised"), Held->DisguisedAs.Get());
	WoTTest::TickWorld(World, 0.1f);

	// 3) Cast near another pawn: take its form.
	AWoTNPCCharacter* Mimic = WoTCreatureData::SpawnCreature(
		*World, TEXT("Minion"), FVector(300, 0, 100), 2);
	if (!TestNotNull(TEXT("production Minion disguise target spawned"), Mimic))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Mimic->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	if (AController* C = Mimic->GetController()) C->UnPossess();
	Mimic->Visibility = 47;
	Mimic->SetNeutralGhost(true);
	const float MimicWalkSpeed = Mimic->GetCharacterMovement()->MaxWalkSpeed;
	const UWoTVertexAnimComponent* MimicBody =
		Cast<UWoTVertexAnimComponent>(Mimic->GetMesh());
	const float CasterWalkSpeed = Caster->GetCharacterMovement()->MaxWalkSpeed;
	const UWoTVertexAnimComponent* CasterBody =
		Cast<UWoTVertexAnimComponent>(Caster->GetMesh());
	const FString OriginalMeshId = CasterBody ? CasterBody->GetVertexMeshLogicalId() : FString();
	AWoTCharacter* Farther = World->SpawnActor<AWoTCharacter>(
		FVector(2000, 0, 100), FRotator::ZeroRotator, SP);
	Farther->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	Inv->UseAngreal();
	TestTrue(TEXT("disguises as the nearest pawn"), Held->DisguisedAs == Mimic);
	TestEqual(TEXT("Standalone copies the production target's GroundSpeed"),
		Caster->GetCharacterMovement()->MaxWalkSpeed, MimicWalkSpeed);
	TestEqual(TEXT("disguise copies runtime AI Visibility"), Caster->Visibility, uint8(47));
	TestTrue(TEXT("disguise carries the source ghost/translucency bit"),
		Caster->Disguise.bSourceNeutralGhost != 0);
	TestTrue(TEXT("production NPC DisguiseIcon is bound"),
		Caster->Disguise.SourceDisguiseIcon.IsValid());
	TestTrue(TEXT("disguise swaps to the production NPC runtime mesh"),
		CasterBody && MimicBody
		&& CasterBody->GetVertexMeshLogicalId() == MimicBody->GetVertexMeshLogicalId());
	Caster->SetAnimIntent(TEXT("AttackRunL"), 1.0f, true);
	TestEqual(TEXT("SubstituteAnimName maps player attack-run to NPC Run"),
		Caster->GetReplicatedAnimName(), FName(TEXT("Run")));

	// 4) Holding it drains charges over time.
	const int32 Charges0 = Held->CurCharges;
	WoTTest::TickWorld(World, 1.2f);
	TestTrue(FString::Printf(TEXT("holding disguise drains charges (%d -> %d)"),
		Charges0, Held->CurCharges), Held->CurCharges < Charges0);

	// 5) Release reverts.
	Inv->CeaseUsingAngreal();
	TestNull(TEXT("releasing disguise reverts"), Held->DisguisedAs.Get());
	TestEqual(TEXT("releasing restores the standalone movement speed"),
		Caster->GetCharacterMovement()->MaxWalkSpeed, CasterWalkSpeed);
	TestEqual(TEXT("releasing restores Visibility"), Caster->Visibility, uint8(128));
	TestTrue(TEXT("releasing restores the original runtime mesh"),
		CasterBody && CasterBody->GetVertexMeshLogicalId() == OriginalMeshId);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// Feel validation. Locks the movement constants against the
// reference footage (exports/reference/feel + measurements.json): the parity
// values from WOTPlayer.uc / Engine.Pawn / Engine.ZoneInfo, the gravity that reproduces
// the measured 41-frame jump airtime (-950, not UE5's default -980), and UE1's
// pure-exponential-friction braking (no constant deceleration term).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFeelMovementTest,
	"WoT.Feel.Movement",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTFeelMovementTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	TestNotNull(TEXT("spawned the WoT pawn"), Pawn);
	if (!Pawn)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	UCharacterMovementComponent* Move = Pawn->GetCharacterMovement();

	// --- parity constants (spec-locked from the exported .uc) ---
	TestEqual(TEXT("MaxWalkSpeed = GroundSpeed 400"), Move->MaxWalkSpeed, 400.0f);
	TestEqual(TEXT("MaxFlySpeed = AirSpeed 400"), Move->MaxFlySpeed, 400.0f);
	TestEqual(TEXT("MaxSwimSpeed = WaterSpeed 200"), Move->MaxSwimSpeed, 200.0f);
	TestEqual(TEXT("MaxAcceleration = AccelRate 2048"), Move->MaxAcceleration, 2048.0f);
	TestEqual(TEXT("JumpZVelocity = JumpZ 325"), Move->JumpZVelocity, 325.0f);
	TestEqual(TEXT("AirControl = 1.0 (full)"), Move->AirControl, 1.0f);

	// --- gravity: reproduces UE1 ZoneGravity -950 (validated by 41-frame airtime,
	//     discriminates the UE5 default -980) ---
	const float G = Move->GetGravityZ();   // includes GravityScale
	TestTrue(FString::Printf(TEXT("effective gravity ~ -950 (UE1 ZoneGravity); got %.1f"), G),
		G > -955.0f && G < -945.0f);

	// --- braking: pure exponential friction, NO constant deceleration term ---
	TestEqual(TEXT("GroundFriction = ZoneGroundFriction 8"), Move->GroundFriction, 8.0f);
	TestEqual(TEXT("BrakingFrictionFactor = 2 (UE1 '2*friction')"),
		Move->BrakingFrictionFactor, 2.0f);
	// 0.01, not 0: CMC's speed<10 stop-snap (BRAKE_TO_STOP_VELOCITY) only fires
	// when braking deceleration is non-zero; the epsilon re-arms UE1's hard stop
	// below speed 10 while adding no perceptible constant decel.
	TestEqual(TEXT("BrakingDecelerationWalking = 0.01 (no constant term; snap armed)"),
		Move->BrakingDecelerationWalking, 0.01f);
	TestFalse(TEXT("braking uses GroundFriction (not a separate braking friction)"),
		Move->bUseSeparateBrakingFriction);

	// --- behavioral: a jump launched at JumpZ reaches apex in ~20.5 frames
	//     (airtime 41f / 2). Drive falling physics directly (no floor needed). ---
	Move->bRunPhysicsWithNoController = true;
	Move->SetMovementMode(MOVE_Falling);
	Move->Velocity = FVector(0.0f, 0.0f, 325.0f);
	const float Step = 1.0f / 60.0f;
	int32 ApexFrames = 0;
	for (int32 F = 0; F < 60; ++F)   // cap well past the ~20-frame apex
	{
		World->Tick(LEVELTICK_All, Step);
		GFrameCounter++;
		++ApexFrames;
		if (Move->Velocity.Z <= 0.0f)
		{
			break;
		}
	}
	// 325 / 950 = 0.342s = 20.5 frames; allow ±1.5 frames for the integrator.
	TestTrue(FString::Printf(TEXT("jump apex ~20.5 frames (measured 41f airtime/2); got %d"),
		ApexFrames), ApexFrames >= 19 && ApexFrames <= 22);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// Tracer (AngrealInvTracer.uc): launches a seeking weave at the NEAREST seal —
// a world-pickup seal or one carried by another pawn — and follows it. Covers
// GetBestTarget's nearest-reachable selection and the projectile's seal tracking
// (SetDestination recording TrackingSeal off both a seal and a carrier pawn).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTTracerTargetingTest,
	"WoT.Inventory.Tracer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTTracerTargetingTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	AWoTCharacter* Shooter = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	Shooter->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	UWoTInventoryComponent* Inv = Shooter->InventoryComponent;

	World->SpawnActor<AWoTAngrealInvTracer>(
		Shooter->GetActorLocation(), FRotator::ZeroRotator);
	WoTTest::TickWorld(World, 0.1f);
	AWoTAngrealInvTracer* Tracer = Cast<AWoTAngrealInvTracer>(Inv->SelectedItem.Get());
	TestNotNull(TEXT("tracer picked up"), Tracer);
	if (!Tracer)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	const int32 Charges = Tracer->CurCharges;

	auto GetProj = [&]() -> AWoTTracerProjectile*
	{
		AWoTTracerProjectile* P = nullptr;
		for (TActorIterator<AWoTTracerProjectile> It(World); It; ++It)
		{
			P = *It;
		}
		return P;
	};

	// 1) No seal anywhere -> bRequiresDestination fails, no charge spent.
	Inv->UseAngreal();
	TestEqual(TEXT("tracer with no seal fails without using a charge"),
		Tracer->CurCharges, Charges);
	TestNull(TEXT("no projectile launched with no seal"), (AActor*)GetProj());

	// 2) Two world seals -> the nearer is targeted and tracked.
	AWoTSeal* Far = World->SpawnActor<AWoTSeal>(
		FVector(800, 0, 100), FRotator::ZeroRotator);
	AWoTSeal* Near = World->SpawnActor<AWoTSeal>(
		FVector(400, 0, 100), FRotator::ZeroRotator);
	WoTTest::TickWorld(World, 0.05f);
	TestTrue(TEXT("both seals sit in Pickup state"),
		Far->GetItemState() == EWoTInventoryState::Pickup &&
		Near->GetItemState() == EWoTInventoryState::Pickup);

	Inv->CeaseUsingAngreal();
	Inv->UseAngreal();
	AWoTTracerProjectile* Proj = GetProj();
	TestNotNull(TEXT("tracer projectile launched at a seal"), Proj);
	TestEqual(TEXT("tracer used a charge"), Tracer->CurCharges, Charges - 1);
	if (Proj)
	{
		TestTrue(TEXT("targets the NEARER world seal"), Proj->Destination == Near);
		TestTrue(TEXT("tracks that seal"), Proj->TrackingSeal == Near);
		Proj->Destroy();
	}

	// 3) A seal carried by a nearer pawn -> target the carrier, track the seal.
	AWoTCharacter* Carrier = World->SpawnActor<AWoTCharacter>(
		FVector(200, 0, 100), FRotator::ZeroRotator);
	Carrier->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	AWoTSeal* Carried = World->SpawnActor<AWoTSeal>(
		FVector(-500, 0, 100), FRotator::ZeroRotator);
	Carried->GiveTo(Carrier);
	// Clear the RoundsPerMinute=60 refire gate (NextCastTime = last fire + 1.0s).
	WoTTest::TickWorld(World, 1.2f);
	TestTrue(TEXT("carried seal left Pickup (now Held)"),
		Carried->GetItemState() == EWoTInventoryState::Held);

	Inv->CeaseUsingAngreal();
	Inv->UseAngreal();
	Proj = GetProj();
	TestNotNull(TEXT("tracer projectile launched at the carrier"), Proj);
	if (Proj)
	{
		TestTrue(TEXT("targets the carrier pawn (nearest seal is carried)"),
			Proj->Destination == Carrier);
		TestTrue(TEXT("tracks the carried seal via the carrier"),
			Proj->TrackingSeal == Carried);
		const float Spin0 = Proj->GetSpinRollDegPerSec();
		WoTTest::TickWorld(World, 0.1f);
		const float Spin1 = Proj->GetSpinRollDegPerSec();
		WoTTest::TickWorld(World, 0.1f);
		const float Spin2 = Proj->GetSpinRollDegPerSec();
		TestTrue(TEXT("Tracer roll speed accelerates every tick"),
			Spin1 > Spin0 && Spin2 > Spin1);
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

// Audio attenuation (WoTSound): the UE1 sound model — WorldSoundRadius byte
// conversion, the cached linear-falloff attenuation, and per-class projectile
// sound-radius overrides. See PORT_NOTES "Default sound radius".
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSoundAttenuationTest,
	"WoT.Feel.SoundAttenuation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTSoundAttenuationTest::RunTest(const FString&)
{
	// UE1 world sound radius = 25*(byte+1)
	TestEqual(TEXT("WorldSoundRadius(32) = 825"), WoTSound::WorldSoundRadius(32), 825.0f);
	TestEqual(TEXT("WorldSoundRadius(64) = 1625"), WoTSound::WorldSoundRadius(64), 1625.0f);
	TestEqual(TEXT("WorldSoundRadius(255) = 6400"), WoTSound::WorldSoundRadius(255), 6400.0f);

	// Linear-falloff attenuation reaching zero at the radius.
	USoundAttenuation* Att = WoTSound::LinearAttenuation(2500.0f);
	TestNotNull(TEXT("LinearAttenuation builds an attenuation"), Att);
	if (Att)
	{
		TestTrue(TEXT("attenuates"), Att->Attenuation.bAttenuate);
		TestEqual(TEXT("falloff distance = radius"), Att->Attenuation.FalloffDistance, 2500.0f);
		TestTrue(TEXT("linear distance model"),
			Att->Attenuation.DistanceAlgorithm == EAttenuationDistanceModel::Linear);
		TestTrue(TEXT("sphere shape"),
			Att->Attenuation.AttenuationShape == EAttenuationShape::Sphere);
	}
	// Cached per radius; zero radius -> no attenuation.
	TestTrue(TEXT("attenuations are cached per radius"),
		WoTSound::LinearAttenuation(2500.0f) == Att);
	TestNull(TEXT("zero radius -> null (no attenuation)"),
		WoTSound::LinearAttenuation(0.0f));

	// Per-class overrides on a spawned projectile (Tracer: SoundRadius 64,
	// ImpactSoundRadius 700; GenericProjectile defaults elsewhere).
	UWorld* World = WoTTest::CreateSpecWorld();
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTTracerProjectile* T = World->SpawnActor<AWoTTracerProjectile>(
		AWoTTracerProjectile::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, Params);
	TestNotNull(TEXT("tracer projectile spawned"), T);
	if (T)
	{
		TestEqual(TEXT("Tracer SoundRadius = 64"), (int32)T->SoundRadius, 64);
		TestEqual(TEXT("Tracer ImpactSoundRadius = 700"), T->ImpactSoundRadius, 700.0f);
		TestEqual(TEXT("Tracer SpawnSoundRadius default = 2500"), T->SpawnSoundRadius, 2500.0f);
	}
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSoundSlotReplacementTest,
	"WoT.Sound.OwnerSlotReplacement",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTSoundSlotReplacementTest::RunTest(const FString& Parameters)
{
	// UE1 Owner.PlaySound SLOT_Misc semantics on AWoTSoundBurst: a later slot
	// sound on the same owner STOPS the previous component (how retail ended
	// the looping LaunchLS cast hum with EndLS), and the component attaches to
	// the owner so pawn teardown bounds it (the stuck-hum defect class).
	UWorld* World = WoTTest::CreateSpecWorld();
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AActor* SlotOwner = World->SpawnActor<AActor>(
		AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, Params);
	USceneComponent* Root = NewObject<USceneComponent>(SlotOwner);
	SlotOwner->SetRootComponent(Root);
	Root->RegisterComponent();
	APlayerController* Listener = World->SpawnActor<APlayerController>(
		APlayerController::StaticClass(), FVector::ZeroVector,
		FRotator::ZeroRotator, Params);
	Listener->SetViewTarget(SlotOwner);

	WoTSound::PlayReplicated(*World, FName(TEXT("Angreal/LaunchLS")),
		FVector::ZeroVector, 1600.0f, 1.0f, 1.0f, SlotOwner);
	WoTTest::TickWorld(World, 1.0f / 60.0f);
	TArray<UAudioComponent*> Attached;
	SlotOwner->GetComponents<UAudioComponent>(Attached);
	if (!TestEqual(TEXT("slot play attaches one audio component to the owner"),
		Attached.Num(), 1))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	TWeakObjectPtr<UAudioComponent> First = Attached[0];

	WoTSound::PlayReplicated(*World, FName(TEXT("Angreal/EndLS")),
		FVector::ZeroVector, 1600.0f, 1.0f, 1.0f, SlotOwner);
	WoTTest::TickWorld(World, 1.0f / 60.0f);
	TestTrue(TEXT("second slot sound stops the first component"),
		!First.IsValid() || !First->IsPlaying());

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTParityASeekerTeleporterSpec,
	"WoT.Parity.A.SeekerTeleporter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTParityASeekerTeleporterSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	auto SpawnNode = [&](const FVector& At)
	{
		AWoTPathNode* N = World->SpawnActor<AWoTPathNode>(At, FRotator::ZeroRotator, SP);
		N->SourceClass = TEXT("Teleporter");
		return N;
	};
	auto Wire = [](AWoTPathNode* A, AWoTPathNode* B)
	{
		FWoTPathEdge Out; Out.Other = B; Out.Distance = FVector::Distance(A->GetActorLocation(), B->GetActorLocation()); A->OutEdges.Add(Out);
		FWoTPathEdge In; In.Other = A; In.Distance = Out.Distance; B->InEdges.Add(In);
	};
	AWoTPathNode* EntryNode = SpawnNode(FVector(250, 0, 200));
	AWoTPathNode* ExitNode = SpawnNode(FVector(750, 0, 300));
	Wire(EntryNode, ExitNode); Wire(ExitNode, EntryNode);
	AWoTTeleporter* Entry = World->SpawnActor<AWoTTeleporter>(AWoTTeleporter::StaticClass(), EntryNode->GetActorLocation(), FRotator::ZeroRotator, SP);
	Entry->URL = TEXT("parity_exit");
	AWoTTeleporter* Exit = World->SpawnActor<AWoTTeleporter>(AWoTTeleporter::StaticClass(), ExitNode->GetActorLocation(), FRotator::ZeroRotator, SP);
	Exit->TeleTag = TEXT("parity_exit");
	Exit->bChangesYaw = true;
	Exit->SetActorRotation(FRotator(0, 90, 0));

	AWoTCharacter* Caster = World->SpawnActor<AWoTCharacter>(FVector(0, 0, 100), FRotator::ZeroRotator, SP);
	Caster->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	AWoTCharacter* Target = World->SpawnActor<AWoTCharacter>(FVector(750, 500, 500), FRotator::ZeroRotator, SP);
	Target->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	World->SpawnActor<AWoTAngrealInvSeeker>(Caster->GetActorLocation(), FRotator::ZeroRotator, SP);
	WoTTest::TickWorld(World, 0.1f);
	Caster->SetActorRotation((Target->GetActorLocation()
		- Caster->GetActorLocation()).Rotation());
	Caster->InventoryComponent->UseAngreal();
	AWoTSeekerProjectile* Seeker = nullptr;
	for (TActorIterator<AWoTSeekerProjectile> It(World); It; ++It) { Seeker = *It; }
	if (!TestNotNull(TEXT("real Seeker cast launched a projectile"), Seeker)) { WoTTest::DestroySpecWorld(World); return false; }

	AStaticMeshActor* Occluder = World->SpawnActor<AStaticMeshActor>(FVector(400, 200, 250), FRotator::ZeroRotator, SP);
	UBoxComponent* Box = NewObject<UBoxComponent>(Occluder, TEXT("ParityASeekerWall"));
	Box->SetupAttachment(Occluder->GetRootComponent()); Box->SetBoxExtent(FVector(20, 400, 400));
	Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics); Box->SetCollisionObjectType(ECC_WorldStatic);
	Box->SetCollisionResponseToAllChannels(ECR_Block); Box->RegisterComponent();

	FVector Previous = Seeker->GetActorLocation();
	FVector VelocityAfterTeleport = FVector::ZeroVector;
	bool bTeleported = false;
	for (int32 Step = 0; Step < 480 && IsValid(Seeker); ++Step)
	{
		WoTTest::TickWorld(World, 1, 1.0f / 60.0f);
		const FVector Now = Seeker->GetActorLocation();
		if (!bTeleported && FVector::Distance(Previous, Now) > 300.0f)
		{
			bTeleported = true;
			VelocityAfterTeleport = Seeker->GetVelocity();
		}
		Previous = Now;
		if (bTeleported && Now.X > 700.0f) { break; }
	}
	TestTrue(TEXT("the pathing seeker traverses the real teleporter in one discontinuous move"), bTeleported);
	TestTrue(TEXT("the seeker arrives on the far side and continues its path"), IsValid(Seeker) && Seeker->GetActorLocation().X > 700.0f);
	const FVector ExpectedExitDirection = (Target->GetActorLocation()
		- Exit->GetActorLocation()).GetSafeNormal();
	TestTrue(TEXT("yaw-changing teleport preserves the pitched seeker's velocity component"),
		!VelocityAfterTeleport.IsNearlyZero()
		&& FVector::DotProduct(VelocityAfterTeleport.GetSafeNormal(),
			ExpectedExitDirection) > 0.985f);
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTParityAWallOfAirCylinderSpec,
	"WoT.Parity.A.WallOfAirCylinder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTParityAWallOfAirCylinderSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	FActorSpawnParameters SP; SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTCharacter* Caster = World->SpawnActor<AWoTCharacter>(FVector(0,0,100), FRotator::ZeroRotator, SP);
	Caster->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	World->SpawnActor<AWoTAngrealInvWallOfAir>(Caster->GetActorLocation(), FRotator::ZeroRotator, SP);
	WoTTest::TickWorld(World, 0.1f);
	AWoTCharacter* Target = World->SpawnActor<AWoTCharacter>(FVector(900,0,100), FRotator::ZeroRotator, SP);
	Target->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	Caster->InventoryComponent->UseAngreal(); WoTTest::TickWorld(World, 0.1f);
	AWoTWallOfAirProjectile* Wall = nullptr;
	for (TActorIterator<AWoTWallOfAirProjectile> It(World); It; ++It) { Wall = *It; }
	if (!TestNotNull(TEXT("real Wall of Air cast plants the wall"), Wall)) { WoTTest::DestroySpecWorld(World); return false; }
	const FVector W = Wall->GetActorLocation();
	AWoTDartProjectile* Corner = World->SpawnActor<AWoTDartProjectile>(AWoTDartProjectile::StaticClass(), FVector(W.X-150,W.Y+70,W.Z+70), FRotator::ZeroRotator, SP);
	AWoTDartProjectile* Outside = World->SpawnActor<AWoTDartProjectile>(AWoTDartProjectile::StaticClass(), FVector(W.X-150,W.Y+90,W.Z+70), FRotator::ZeroRotator, SP);
	TWeakObjectPtr<AWoTDartProjectile> CornerPtr = Corner, OutsidePtr = Outside;
	WoTTest::TickWorld(World, 0.2f);
	TestFalse(TEXT("inside the 75x75 cylinder but outside the old sphere is stopped"), CornerPtr.IsValid());
	TestTrue(TEXT("a projectile outside the cylinder radius is not stopped"), OutsidePtr.IsValid());
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTParityAExplosiveWardCylinderSpec,
	"WoT.Parity.A.ExplosiveWardCylinder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTParityAExplosiveWardCylinderSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	FActorSpawnParameters SP; SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AStaticMeshActor* Floor = World->SpawnActor<AStaticMeshActor>(FVector::ZeroVector, FRotator::ZeroRotator, SP);
	UBoxComponent* FloorBox = NewObject<UBoxComponent>(Floor, TEXT("ParityAWardFloor"));
	FloorBox->SetupAttachment(Floor->GetRootComponent()); FloorBox->SetBoxExtent(FVector(500,500,10));
	FloorBox->SetCollisionObjectType(ECC_WorldStatic); FloorBox->SetCollisionResponseToAllChannels(ECR_Block);
	FloorBox->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics); FloorBox->RegisterComponent();
	AWoTCharacter* Caster = World->SpawnActor<AWoTCharacter>(FVector(0,0,150), FRotator(-90,0,0), SP);
	Caster->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	World->SpawnActor<AWoTAngrealInvExpWard>(Caster->GetActorLocation(), FRotator::ZeroRotator, SP);
	WoTTest::TickWorld(World, 0.1f); Caster->SetActorRotation(FRotator(-90,0,0));
	Caster->InventoryComponent->UseAngreal();
	AWoTExpWardProjectile* Ward = nullptr;
	for (TActorIterator<AWoTExpWardProjectile> It(World); It; ++It) { Ward = *It; }
	if (!TestNotNull(TEXT("real Explosive Ward cast places the ward"), Ward)) { WoTTest::DestroySpecWorld(World); return false; }
	auto TinyPawn = [&](const FVector& At)
	{
		AWoTCharacter* P = World->SpawnActor<AWoTCharacter>(At, FRotator::ZeroRotator, SP);
		P->GetCapsuleComponent()->SetCapsuleSize(1.0f, 1.0f);
		P->GetCapsuleComponent()->SetCollisionResponseToAllChannels(ECR_Ignore);
		P->GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_GameTraceChannel1, ECR_Overlap);
		P->GetCharacterMovement()->DisableMovement();
		P->SetActorLocation(At, false, nullptr, ETeleportType::TeleportPhysics);
		return P;
	};
	AWoTCharacter* Occupant = TinyPawn(Ward->GetActorLocation());
	AWoTCharacter* Above = TinyPawn(Ward->GetActorLocation() + FVector(100,0,12));
	TWeakObjectPtr<AWoTExpWardProjectile> WardPtr = Ward;
	WoTTest::TickWorld(World, 1.2f);
	TestTrue(TEXT("arming does not synthesize a touch for an existing occupant"), WardPtr.IsValid());
	if (WardPtr.IsValid())
	{
		Above->SetActorLocation(Ward->GetActorLocation() + FVector(0,0,12));
		WoTTest::TickWorld(World, 0.1f);
		TestTrue(TEXT("movement 12uu above crosses the old sphere but stays outside the flat cylinder"), WardPtr.IsValid());
	}
	if (WardPtr.IsValid())
	{
		Occupant->SetActorLocation(Occupant->GetActorLocation() + FVector(1,0,0)); WoTTest::TickWorld(World, 0.1f);
		TestFalse(TEXT("later native movement while inside detonates the armed ward"), WardPtr.IsValid());
	}
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
