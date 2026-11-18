// Systems-wave parity regression specs (P13/P15/P19/P21/P25).
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../Inventory/WoTAngrealInvLevitate.h"
#include "../Inventory/WoTAngrealInvRemoveCurse.h"
#include "../Inventory/WoTAngrealInvWallOfAir.h"
#include "../Inventory/WoTDestroyOnDestroyLeech.h"
#include "../Inventory/WoTAngrealInvWhirlwind.h"
#include "../Inventory/WoTEarthTremorLeech.h"
#include "../Inventory/WoTTrapDetectLeech.h"
#include "../Inventory/WoTWhirlwindLeech.h"
#include "../Projectiles/WoTAMAPearl.h"
#include "../Projectiles/WoTArcherArrow.h"
#include "../WoTFractalData.h"
#include "../WoTFractalSubsystem.h"
#include "../WoTVertexAnimComponent.h"
#include "../Projectiles/WoTEarthTremorFX.h"
#include "../Traps/WoTSpear.h"
#include "../Projectiles/WoTEarthTremorProjectile.h"
#include "../Projectiles/WoTFireballProjectile.h"
#include "../WoTSpriteBillboard.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "../Projectiles/WoTMashadarGuide.h"
#include "../Projectiles/WoTWallOfAirProjectile.h"
#include "../Traps/WoTWall.h"
#include "../WoTBattleGameMode.h"
#include "../WoTCharacter.h"
#include "../WoTMeshFX.h"
#include "../WoTParticleSprayer.h"
#include "../WoTPlayerController.h"
#include "../WoTWeaveFX.h"
#include "../WoTWhirlwindFunnel.h"
#include "WoTTestTrap.h"

#include "Components/BoxComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"

namespace
{
	FActorSpawnParameters WeaveSystemsAlwaysSpawn()
	{
		FActorSpawnParameters P;
		P.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		return P;
	}

	AActor* SpawnBlock(UWorld* World, const FVector& Location,
		const FVector& Extent = FVector(100.0f))
	{
		FActorSpawnParameters P = WeaveSystemsAlwaysSpawn();
		// AStaticMeshActor is the headless stand-in for imported level geometry;
		// generic actors on WorldStatic are deliberately not TRACE_VisBlocking.
		AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(
			Location, FRotator::ZeroRotator, P);
		if (!Actor) { return nullptr; }
		Actor->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
		UBoxComponent* Box = NewObject<UBoxComponent>(Actor);
		if (!Box) { Actor->Destroy(); return nullptr; }
		Box->SetupAttachment(Actor->GetRootComponent());
		Box->SetMobility(EComponentMobility::Movable);
		Actor->AddInstanceComponent(Box);
		Box->SetBoxExtent(Extent);
		Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Box->SetCollisionResponseToAllChannels(ECR_Block);
		Box->RegisterComponent();
		return Actor;
	}

	AActor* SpawnActorBlock(UWorld* World, const FVector& Location,
		const FVector& Extent)
	{
		AActor* Actor = World->SpawnActor<AActor>(
			Location, FRotator::ZeroRotator, WeaveSystemsAlwaysSpawn());
		if (!Actor) { return nullptr; }
		UBoxComponent* Box = NewObject<UBoxComponent>(Actor);
		if (!Box) { Actor->Destroy(); return nullptr; }
		Actor->SetRootComponent(Box);
		Actor->AddInstanceComponent(Box);
		Box->SetBoxExtent(Extent);
		Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Box->SetCollisionResponseToAllChannels(ECR_Block);
		Box->RegisterComponent();
		Actor->SetActorLocation(Location);
		return Actor;
	}

	template <typename T> int32 WeaveSystemsCountActors(UWorld* World)
	{
		int32 Count = 0;
		for (TActorIterator<T> It(World); It; ++It) { ++Count; }
		return Count;
	}
}

#define SYSTEMS_TEST(ClassName, Path) \
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(ClassName, Path, \
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

SYSTEMS_TEST(FWoTWhirlwindFunnelAndFitSpec, "WoT.Whirlwind.FunnelAndFit")
bool FWoTWhirlwindFunnelAndFitSpec::RunTest(const FString&)
{
	TestFalse(TEXT("ActivateWW is bound"),
		GetDefault<AWoTAngrealInvWhirlwind>()->ActivateSound.IsNull());
	UWorld* World = WoTTest::CreateSpecWorld();
	FActorSpawnParameters P = WeaveSystemsAlwaysSpawn();
	AActor* Floor = SpawnBlock(World, FVector(0, 0, -40), FVector(1000, 1000, 40));
	if (!TestNotNull(TEXT("ground fixture spawned"), Floor))
	{
		WoTTest::DestroySpecWorld(World); return false;
	}
	AWoTCharacter* Victim = World->SpawnActor<AWoTCharacter>(
		AWoTCharacter::StaticClass(), FVector(0, 0, 100), FRotator::ZeroRotator, P);
	if (!TestNotNull(TEXT("victim spawned"), Victim))
	{
		WoTTest::DestroySpecWorld(World); return false;
	}
	Victim->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	AWoTPlayerController* PC = World->SpawnActor<AWoTPlayerController>(
		AWoTPlayerController::StaticClass(), FVector::ZeroVector,
		FRotator::ZeroRotator, P);
	if (!TestNotNull(TEXT("whirlwind controller spawned"), PC))
	{
		WoTTest::DestroySpecWorld(World); return false;
	}
	PC->SetAsLocalPlayerController();
	PC->Possess(Victim);
	UWoTWhirlwindLeech* Leech = NewObject<UWoTWhirlwindLeech>(Victim);
	if (!TestNotNull(TEXT("production leech created"), Leech))
	{
		WoTTest::DestroySpecWorld(World); return false;
	}
	Leech->AttachTo(Victim);
	TestTrue(TEXT("player victim forced-view enabled"), PC->bForcedViewYaw);
	TestEqual(TEXT("forced-view rate is retail yaw units per second"),
		PC->ForcedViewYawRateUnits, 120000.0f);
	Leech->SetDesiredLocation(FVector(0, 0, 300));
	WoTTest::TickWorld(World, 5);
	AWoTWhirlwindFunnel* Funnel = Leech->GetFunnelForTest();
	AWoTParticleSprayer* Dust = Leech->GetDustForTest();
	TestTrue(TEXT("forced-view rate is consumed by controller tick"),
		PC->GetForcedViewApplyCountForTest() > 0);
	TestNotNull(TEXT("production leech spawned mesh funnel"), Funnel);
	TestNotNull(TEXT("production leech spawned dust"), Dust);
	if (Funnel)
	{
		TestEqual(TEXT("funnel uses WhirlwindSlice"), Funnel->MeshName,
			FName(TEXT("WhirlwindSlice")));
		// WhirlwindLeech.uc:164 measures from the authored target, not the
		// victim's eased in-flight position.
		const float RetailHeight = FVector::Dist(
			Leech->DesiredLocation, Funnel->GetActorLocation()) + 250.0f;
		TestTrue(TEXT("funnel height reaches the desired location"),
			FMath::IsNearlyEqual(Funnel->GetHeight(), RetailHeight));
	}
	if (Dust)
	{
		TestEqual(TEXT("dust carries three retail templates"), Dust->Templates.Num(), 3);
		TestTrue(TEXT("dust is on while funnel touches traced ground"), Dust->bOn);
		TestEqual(TEXT("dust tracks funnel base"), Dust->GetActorLocation(),
			Funnel ? Funnel->GetActorLocation() : Dust->GetActorLocation());
	}

	Floor->SetActorLocation(FVector(0, 0, -140));
	WoTTest::TickWorld(World, 1);
	if (Dust)
	{
		TestFalse(TEXT("dust gates off while funnel descends toward new ground"), Dust->bOn);
	}
	Floor->SetActorEnableCollision(false);
	AActor* Above = SpawnActorBlock(
		World, FVector(0, 0, 220), FVector(40, 40, 40));
	if (!TestNotNull(TEXT("above blocker spawned"), Above))
	{
		WoTTest::DestroySpecWorld(World); return false;
	}
	TestFalse(TEXT("signed-height blocker above holds victim"),
		UWoTWhirlwindLeech::ActorFits(Victim, FVector(0, 0, 180), 1024.0f));
	Above->SetActorLocation(FVector(0, 0, 600));
	TestTrue(TEXT("blocker sufficiently above clears signed test"),
		UWoTWhirlwindLeech::ActorFits(Victim, FVector(0, 0, 180), 1024.0f));
	Above->SetActorLocation(FVector(0, 0, -400));
	TestFalse(TEXT("signed shipped quirk treats far-below blocker as overlapping"),
		UWoTWhirlwindLeech::ActorFits(Victim, FVector(0, 0, 180), 1024.0f));

	Leech->Unattach();
	WoTTest::DestroySpecWorld(World);
	return true;
}

SYSTEMS_TEST(FWoTLevitateLoopSoundsSpec, "WoT.Levitate.LoopSounds")
bool FWoTLevitateLoopSoundsSpec::RunTest(const FString&)
{
	const AWoTAngrealInvLevitate* CDO = GetDefault<AWoTAngrealInvLevitate>();
	TestFalse(TEXT("LoopLT is bound"), CDO->ActivateSound.IsNull());
	TestFalse(TEXT("DeActivateLT is bound"), CDO->DeactivateSound.IsNull());
	UWorld* World = WoTTest::CreateSpecWorld();
	FActorSpawnParameters P = WeaveSystemsAlwaysSpawn();
	AActor* Floor = SpawnBlock(World, FVector(0, 0, -40), FVector(1000, 1000, 40));
	if (!TestNotNull(TEXT("levitate floor spawned"), Floor))
	{
		WoTTest::DestroySpecWorld(World); return false;
	}
	AWoTCharacter* Caster = World->SpawnActor<AWoTCharacter>(
		AWoTCharacter::StaticClass(), FVector(0, 0, 100), FRotator::ZeroRotator, P);
	if (!TestNotNull(TEXT("levitate caster spawned"), Caster))
	{
		WoTTest::DestroySpecWorld(World); return false;
	}
	AWoTAngrealInvLevitate* Inv = World->SpawnActor<AWoTAngrealInvLevitate>(
		AWoTAngrealInvLevitate::StaticClass(), FVector(0, 0, 300), FRotator::ZeroRotator, P);
	if (!TestNotNull(TEXT("levitate inventory spawned"), Inv))
	{
		WoTTest::DestroySpecWorld(World); return false;
	}
	Inv->SetOwner(Caster);
	const int32 Before = WeaveSystemsCountActors<AWoTWhirlwindFunnel>(World);
	Inv->CastArtifact();
	WoTTest::TickWorld(World, 2);
	TestNotNull(TEXT("LoopLT ambient component starts"),
		Inv->GetLevitateLoopForTest());
	Inv->UnCastArtifact();
	TestNull(TEXT("LoopLT ambient component stops on uncast"),
		Inv->GetLevitateLoopForTest());
	TestEqual(TEXT("Levitate spawns no visual actor"),
		WeaveSystemsCountActors<AWoTWhirlwindFunnel>(World), Before);
	WoTTest::DestroySpecWorld(World);
	return true;
}

SYSTEMS_TEST(FWoTEarthTremorRingsAndLeechSpec, "WoT.EarthTremor.RingsAndLeech")
bool FWoTEarthTremorRingsAndLeechSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	FActorSpawnParameters P = WeaveSystemsAlwaysSpawn();
	AActor* Floor = SpawnBlock(World, FVector(0, 0, -40), FVector(1000, 1000, 40));
	if (!TestNotNull(TEXT("tremor floor spawned"), Floor))
	{
		WoTTest::DestroySpecWorld(World); return false;
	}
	AWoTEarthTremorProjectile* Projectile = World->SpawnActor<AWoTEarthTremorProjectile>(
		AWoTEarthTremorProjectile::StaticClass(), FVector(0, 0, 10), FRotator::ZeroRotator, P);
	if (!TestNotNull(TEXT("tremor projectile spawned"), Projectile))
	{
		WoTTest::DestroySpecWorld(World); return false;
	}
	Projectile->Explode(Projectile->GetActorLocation(), FVector::UpVector);
	TestNotNull(TEXT("LavaSpew row resolves"), WoTWeaveFX::Find(TEXT("LavaSpew")));
	WoTTest::TickWorld(World, 47);
	AWoTEarthTremor* Tremor = nullptr;
	if (TActorIterator<AWoTEarthTremor> It(World); It) { Tremor = *It; }
	if (!TestNotNull(TEXT("impact spawned one orchestrator"), Tremor))
	{
		WoTTest::DestroySpecWorld(World); return false;
	}
	TestEqual(TEXT("center plus first three rings made 21 clear slots"),
		Tremor->GetSpawnedRockCount(), 21);
	TestTrue(TEXT("first three authored radii observed before final cadence"),
		Tremor->GetSpawnedRingRadii().Contains(70.0f)
		&& Tremor->GetSpawnedRingRadii().Contains(140.0f)
		&& Tremor->GetSpawnedRingRadii().Contains(210.0f)
		&& !Tremor->GetSpawnedRingRadii().Contains(280.0f));
	AActor* RingBlocker = SpawnBlock(World, FVector(35, 0, 10), FVector(8));
	if (!TestNotNull(TEXT("ring trace blocker spawned"), RingBlocker))
	{
		WoTTest::DestroySpecWorld(World); return false;
	}
	TestFalse(TEXT("center-to-slot trace gates obstructed rock"),
		Tremor->IsRingSlotClearForTest(FVector(70, 0, 10)));
	RingBlocker->Destroy();
	AWoTETSoundProxy* Proxy = Tremor->GetSoundProxyForTest();
	AWoTEarthTremorRock* Rock = Tremor->GetAnyAuthorityRockForTest();
	TestNotNull(TEXT("center spawned sound proxy"), Proxy);
	if (Proxy)
	{
		TestTrue(TEXT("proxy fade scalar starts full"), Proxy->GetFadeScalar() > 0.99f);
		Proxy->SetLifeSpan(1.0f);
		WoTTest::TickWorld(World, 5);
		TestTrue(TEXT("proxy last-ten-percent fade is observable"),
			Proxy->GetFadeScalar() < 1.0f);
	}
	TestTrue(TEXT("orchestrator destroys after fourth ring"), !IsValid(Tremor));
	TestEqual(TEXT("center plus four rings made 31 clear slots"),
		WeaveSystemsCountActors<AWoTEarthTremorRock>(World), 31);
	TSet<int32> ObservedRadii;
	for (TActorIterator<AWoTEarthTremorRock> It(World); It; ++It)
	{
		ObservedRadii.Add(FMath::RoundToInt(FVector2D(
			It->GetActorLocation().X, It->GetActorLocation().Y).Size()));
	}
	TestTrue(TEXT("all four authored radii observed"),
		ObservedRadii.Contains(70) && ObservedRadii.Contains(140)
		&& ObservedRadii.Contains(210) && ObservedRadii.Contains(280));
	AWoTCharacter* Victim = World->SpawnActor<AWoTCharacter>(
		AWoTCharacter::StaticClass(), FVector(70, 0, 0), FRotator::ZeroRotator, P);
	if (!TestNotNull(TEXT("rock fixture exists"), Rock)
		|| !TestNotNull(TEXT("tremor victim spawned"), Victim))
	{
		WoTTest::DestroySpecWorld(World); return false;
	}
	AWoTPlayerController* PC = World->SpawnActor<AWoTPlayerController>(
		AWoTPlayerController::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, P);
	if (!TestNotNull(TEXT("victim controller spawned"), PC))
	{
		WoTTest::DestroySpecWorld(World); return false;
	}
	PC->SetAsLocalPlayerController();
	PC->Possess(Victim);
	Rock->TouchActor(Victim);
	UWoTEarthTremorLeech* Leech = Victim->FindComponentByClass<UWoTEarthTremorLeech>();
	if (TestNotNull(TEXT("production rock touch installed leech"), Leech))
	{
		Leech->AffectHost(1);
		TestEqual(TEXT("surviving continuous damage keeps raw and PlayHit shakes"),
			PC->GetShakeRequestCountForTest(), 2);
	}
	WoTTest::DestroySpecWorld(World);
	return true;
}

SYSTEMS_TEST(FWoTEarthTremorHitchCadenceSpec, "WoT.EarthTremor.HitchCadence")
bool FWoTEarthTremorHitchCadenceSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	FActorSpawnParameters P = WeaveSystemsAlwaysSpawn();
	if (!TestNotNull(TEXT("hitch cadence floor spawned"),
		SpawnBlock(World, FVector(0, 0, -40), FVector(1000, 1000, 40))))
	{
		WoTTest::DestroySpecWorld(World); return false;
	}
	AWoTEarthTremor* Tremor = World->SpawnActor<AWoTEarthTremor>(
		AWoTEarthTremor::StaticClass(), FVector(0, 0, 10),
		FRotator::ZeroRotator, P);
	if (!TestNotNull(TEXT("hitch cadence orchestrator spawned"), Tremor))
	{
		WoTTest::DestroySpecWorld(World); return false;
	}

	// EarthTremor.uc:115/138/149 restarts each 0.2 s wait at the state
	// transition. A single hitch may resume only one gameplay ring.
	Tremor->Tick(0.45f);
	TSet<int32> RingRadii;
	for (const float Radius : Tremor->GetSpawnedRingRadii())
	{
		RingRadii.Add(FMath::RoundToInt(Radius));
	}
	TestEqual(TEXT("one large tick emits exactly one ring"), RingRadii.Num(), 1);
	TestTrue(TEXT("the one emitted ring is the first authored ring"),
		RingRadii.Contains(70));

	WoTTest::DestroySpecWorld(World);
	return true;
}

SYSTEMS_TEST(FWoTAMAAbsorptionAndFeedbackSpec, "WoT.AMA.AbsorptionAndFeedback")
bool FWoTAMAAbsorptionAndFeedbackSpec::RunTest(const FString&)
{
	const AWoTAMAPearl* AMACDO = GetDefault<AWoTAMAPearl>();
	TestFalse(TEXT("replicated AMA pearl is persistent"), AMACDO->bNetTemporary);
	TestFalse(TEXT("ActivateAM is bound"), AMACDO->ActivateSound.IsNull());
	TestFalse(TEXT("DeActivateAM is bound"), AMACDO->DeActivateSound.IsNull());
	TestFalse(TEXT("PawnWalksThruDome is bound"), AMACDO->AffectSound.IsNull());
	TestFalse(TEXT("UnravelAM is bound"), AMACDO->AbsorbSound.IsNull());
	const AWoTRCAMAPearl* RCCDO = GetDefault<AWoTRCAMAPearl>();
	TestEqual(TEXT("RC final scale"), RCCDO->FinalScale, 3.0f);
	TestEqual(TEXT("RC duration"), RCCDO->Duration, 2.5f);
	TestEqual(TEXT("RC fade time"), RCCDO->FadeTime, 0.2f);
	TestEqual(TEXT("RC end light radius"), RCCDO->EndLightRadius, uint8(3));
	TestEqual(TEXT("RC priority"), RCCDO->Priority, uint8(0));
	TestEqual(TEXT("RC scale glow"), RCCDO->ScaleGlow, 0.3f);
	TestTrue(TEXT("RC translucent style"), RCCDO->bStyleTranslucent);
	UWorld* World = WoTTest::CreateSpecWorld();
	FActorSpawnParameters P = WeaveSystemsAlwaysSpawn();
	AWoTAMAPearl* Pearl = World->SpawnActor<AWoTAMAPearl>(
		AWoTAMAPearl::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, P);
	if (!TestNotNull(TEXT("AMA pearl spawned"), Pearl))
	{
		WoTTest::DestroySpecWorld(World); return false;
	}
	Pearl->Go();
	AWoTFireballProjectile* Weave = World->SpawnActor<AWoTFireballProjectile>(
		AWoTFireballProjectile::StaticClass(), FVector(10, 0, 0), FRotator::ZeroRotator, P);
	if (!TestNotNull(TEXT("absorbable weave spawned"), Weave))
	{
		WoTTest::DestroySpecWorld(World); return false;
	}
	Pearl->ProjTouch(Weave);
	TestTrue(TEXT("weave destroyed without explosion"), !IsValid(Weave));
	TestEqual(TEXT("absorption spawned sparks observable"), Pearl->GetAbsorptionCountForTest(), 1);

	AWoTMashadarGuide* Guide = World->SpawnActor<AWoTMashadarGuide>(
		AWoTMashadarGuide::StaticClass(), FVector(20, 0, 0), FRotator::ZeroRotator, P);
	if (!TestNotNull(TEXT("guide exemption fixture spawned"), Guide))
	{
		WoTTest::DestroySpecWorld(World); return false;
	}
	Pearl->ProjTouch(Guide);
	TestTrue(TEXT("MashadarGuide exempt"), IsValid(Guide));

	AWoTAMAPearl* Higher = World->SpawnActor<AWoTAMAPearl>(
		AWoTAMAPearl::StaticClass(), FVector(30, 0, 0), FRotator::ZeroRotator, P);
	if (!TestNotNull(TEXT("priority pearl spawned"), Higher))
	{
		WoTTest::DestroySpecWorld(World); return false;
	}
	Higher->Priority = Pearl->Priority + 1;
	Pearl->ProjTouch(Higher);
	TestTrue(TEXT("higher-priority pearl is yielded to"), IsValid(Higher));

	AWoTTestTrapEventSink* AuraTrigger = World->SpawnActor<AWoTTestTrapEventSink>(
		AWoTTestTrapEventSink::StaticClass(), FVector(40, 0, 0),
		FRotator::ZeroRotator, P);
	if (!TestNotNull(TEXT("anti-magic trigger fixture spawned"), AuraTrigger))
	{
		WoTTest::DestroySpecWorld(World); return false;
	}
	// AMAPearl.uc:196 reads UE1 Actor.Tag, whose module seat is TagName.
	AuraTrigger->TagName = TEXT("AntiMagicAura");
	Pearl->ProjTouch(AuraTrigger);
	TestEqual(TEXT("UE1-tagged trigger receives aura trigger"), AuraTrigger->TriggerCount, 1);
	TestEqual(TEXT("aura trigger request observable"),
		Pearl->GetAntiMagicTriggerCountForTest(), 1);
	AWoTTestTrapEventSink* CompatAuraTrigger = World->SpawnActor<AWoTTestTrapEventSink>(
		AWoTTestTrapEventSink::StaticClass(), FVector(45, 0, 0),
		FRotator::ZeroRotator, P);
	if (!TestNotNull(TEXT("anti-magic compatibility fixture spawned"), CompatAuraTrigger))
	{
		WoTTest::DestroySpecWorld(World); return false;
	}
	CompatAuraTrigger->Tags.Add(TEXT("AntiMagicAura"));
	Pearl->ProjTouch(CompatAuraTrigger);
	TestEqual(TEXT("UE5 ActorHasTag compatibility remains"),
		CompatAuraTrigger->TriggerCount, 1);
	TestEqual(TEXT("both aura tag surfaces are observable"),
		Pearl->GetAntiMagicTriggerCountForTest(), 2);

	AWoTCharacter* TintedPawn = World->SpawnActor<AWoTCharacter>(
		AWoTCharacter::StaticClass(), FVector(50, 0, 0), FRotator::ZeroRotator, P);
	if (!TestNotNull(TEXT("AMA tint pawn spawned"), TintedPawn))
	{
		WoTTest::DestroySpecWorld(World); return false;
	}
	AWoTPlayerController* TintPC = World->SpawnActor<AWoTPlayerController>(
		AWoTPlayerController::StaticClass(), FVector::ZeroVector,
		FRotator::ZeroRotator, P);
	if (!TestNotNull(TEXT("AMA tint controller spawned"), TintPC))
	{
		WoTTest::DestroySpecWorld(World); return false;
	}
	TintPC->SetAsLocalPlayerController();
	TintPC->Possess(TintedPawn);
	Pearl->ProjTouch(TintedPawn);
	TestEqual(TEXT("pawn enter applies owner glow"), TintPC->ConstantGlowScale, 4.0f);
	TintPC->UnPossess();
	Pearl->SetRole(ROLE_SimulatedProxy);
	Pearl->ProjUnTouch(TintedPawn);
	TestEqual(TEXT("client-side exit is an authority no-op"),
		TintPC->ConstantGlowScale, 4.0f);
	TestEqual(TEXT("client-side exit requests no sound or tint"),
		Pearl->GetPawnPassCountForTest(), 1);
	Pearl->SetRole(ROLE_Authority);
	Pearl->ProjUnTouch(TintedPawn);
	TestEqual(TEXT("pawn exit reverses glow through the stored controller"),
		TintPC->ConstantGlowScale, 0.0f);
	TestEqual(TEXT("enter and exit pass sounds requested"),
		Pearl->GetPawnPassCountForTest(), 2);

	WoTTest::TickWorld(World, 140);
	TestTrue(TEXT("light radius eases while expanding"), Pearl->GetCurrentLightRadiusForTest() > 0.0f);
	TestTrue(TEXT("duration starts on Expanded entry"), Pearl->GetEndTimeForTest() > 0.0f);
	WoTTest::DestroySpecWorld(World);
	return true;
}

SYSTEMS_TEST(FWoTTrapDetectClientOutputSpec, "WoT.TrapDetect.ClientOutput")
bool FWoTTrapDetectClientOutputSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	FActorSpawnParameters P = WeaveSystemsAlwaysSpawn();
	AWoTCharacter* Owner = World->SpawnActor<AWoTCharacter>(
		AWoTCharacter::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, P);
	if (!TestNotNull(TEXT("detector owner spawned"), Owner))
	{
		WoTTest::DestroySpecWorld(World); return false;
	}
	AWoTPlayerController* PC = World->SpawnActor<AWoTPlayerController>(
		AWoTPlayerController::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, P);
	if (!TestNotNull(TEXT("detector controller spawned"), PC))
	{
		WoTTest::DestroySpecWorld(World); return false;
	}
	PC->SetAsLocalPlayerController();
	PC->Possess(Owner);
	AWoTWallSlab* Aimed = World->SpawnActor<AWoTWallSlab>(
		AWoTWallSlab::StaticClass(), FVector(300, 0, 0), FRotator::ZeroRotator, P);
	if (!TestNotNull(TEXT("aimed trap spawned"), Aimed))
	{
		WoTTest::DestroySpecWorld(World); return false;
	}
	AWoTWallSlab* NearOffAxis = World->SpawnActor<AWoTWallSlab>(
		AWoTWallSlab::StaticClass(), FVector(100, 250, 0), FRotator::ZeroRotator, P);
	if (!TestNotNull(TEXT("off-axis trap spawned"), NearOffAxis))
	{
		WoTTest::DestroySpecWorld(World); return false;
	}
	UWoTTrapDetectLeech* Leech = NewObject<UWoTTrapDetectLeech>(Owner);
	if (!TestNotNull(TEXT("detector leech created"), Leech))
	{
		WoTTest::DestroySpecWorld(World); return false;
	}
	Leech->AttachTo(Owner);
	WoTTest::TickWorld(World, 3);
	TestEqual(TEXT("view-centered selection prefers aimed trap"), Leech->ClosestTrap.Get(),
		static_cast<AActor*>(Aimed));
	TestEqual(TEXT("all six retail trap rows bind"), Leech->GetTrapIconInfoCountForTest(), 6);
	for (const TCHAR* Type : {TEXT("WallSlab"), TEXT("Pit"), TEXT("Portcullis"),
		TEXT("Spear"), TEXT("Staircase"), TEXT("AngrealExpWardProjectile")})
	{
		TestNotNull(FString::Printf(TEXT("%s icon row resolves"), Type),
			WoTMeshFX::FindSprite(FName(*FString::Printf(TEXT("TrapIcon%s"), Type))));
		TestNotNull(FString::Printf(TEXT("%s sprayer row resolves"), Type),
			WoTWeaveFX::Find(FName(*FString::Printf(TEXT("TrapDetect%s"), Type))));
	}
	TestNotNull(TEXT("owner-only icon exists"), Leech->GetTrapIconForTest());
	TestNotNull(TEXT("owner-only sprayer exists"), Leech->GetIndicatorForTest());
	if (Leech->GetTrapIconForTest())
	{
		TestTrue(TEXT("icon is owner-only"),
			Leech->GetTrapIconForTest()->Sprite->IsOnlyOwnerSee());
	}
	if (Leech->GetIndicatorForTest())
	{
		TestTrue(TEXT("sprayer passes are owner-only"),
			Leech->GetIndicatorForTest()->AreInstancesOnlyOwnerSee());
	}
	TestTrue(TEXT("distance scales beat cadence"), Leech->AffectResolution > 0.3f
		&& Leech->AffectResolution < 3.0f);
	// The bare wall slabs above expire within seconds; give the heartbeat wait
	// a persistent deployed trap (a Spear never undeploys) so the beat has a
	// valid target for the whole window — standing on a floor, or it falls
	// into the void mid-wait and the beats fire trapless.
	if (!TestNotNull(TEXT("beat floor spawned"),
		SpawnBlock(World, FVector(250, 0, -140), FVector(400, 400, 40))))
	{
		WoTTest::DestroySpecWorld(World); return false;
	}
	AWoTSpear* BeatTrap = World->SpawnActor<AWoTSpear>(
		AWoTSpear::StaticClass(), FVector(250, 0, 0), FRotator::ZeroRotator, P);
	if (!TestNotNull(TEXT("beat trap spawned"), BeatTrap))
	{
		WoTTest::DestroySpecWorld(World); return false;
	}
	Leech->MinBeatTime = 0.5f;
	Leech->MaxBeatTime = 0.5f;
	Leech->AffectResolution = 0.5f;
	// 62 frames = 1.033 s, just over 2.05 x the known 0.5 s cadence.
	// TrapDetectLeech.uc's standalone authority path must produce exactly two
	// beats; a second subclass timer would double this count.
	WoTTest::TickWorld(World, 62);
	TestEqual(TEXT("standalone heartbeat uses one scheduler at exact cadence"),
		Leech->GetHeartbeatRequestCountForTest(), 2);
	WoTTest::DestroySpecWorld(World);
	return true;
}

SYSTEMS_TEST(FWoTWallOfAirSeverLifecycleSpec, "WoT.WallOfAir.SeverLifecycle")
bool FWoTWallOfAirSeverLifecycleSpec::RunTest(const FString&)
{
	const AWoTBattleGameMode* Battle = GetDefault<AWoTBattleGameMode>();
	TestEqual(TEXT("Battle Set2 slot 2 resolves Sever"), Battle->AngrealSet2[2].Get(),
		AWoTAngrealInvWallOfAir::StaticClass());
	const AWoTWallOfAirProjectile* WallCDO = GetDefault<AWoTWallOfAirProjectile>();
	TestNotNull(TEXT("barrier mesh component exists"), WallCDO->DisplayMesh.Get());
	TestNotNull(TEXT("barrier light exists"), WallCDO->Light.Get());
	TestFalse(TEXT("LaunchWA bound"), WallCDO->SpawnSound.IsNull());
	TestFalse(TEXT("LoopWA bound"), WallCDO->AmbientLoopSound.IsNull());
	TestNotNull(TEXT("WOTSparks row resolves"), WoTWeaveFX::Find(TEXT("WOTSparks")));

	UWorld* World = WoTTest::CreateSpecWorld();
	FActorSpawnParameters P = WeaveSystemsAlwaysSpawn();
	AWoTWallOfAirProjectile* Wall = World->SpawnActor<AWoTWallOfAirProjectile>(
		AWoTWallOfAirProjectile::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, P);
	if (!TestNotNull(TEXT("wall spawned"), Wall))
	{
		WoTTest::DestroySpecWorld(World); return false;
	}
	// Spawn OUTSIDE the wall's r75 overlap: inside it the production Touch
	// deflects (destroys) the weave during SpawnActor itself and the call
	// returns null — the deflection working, not a fixture success.
	AWoTFireballProjectile* Weave = World->SpawnActor<AWoTFireballProjectile>(
		AWoTFireballProjectile::StaticClass(), FVector(300, 0, 0), FRotator::ZeroRotator, P);
	if (!TestNotNull(TEXT("deflected weave spawned"), Weave))
	{
		WoTTest::DestroySpecWorld(World); return false;
	}
	Wall->TouchActor(Weave);
	TestTrue(TEXT("angreal projectile destroyed"), !IsValid(Weave));
	TestEqual(TEXT("deflection requested WOTSparks"), Wall->GetSparkBurstCountForTest(), 1);
	AWoTArcherArrow* Arrow = World->SpawnActor<AWoTArcherArrow>(
		AWoTArcherArrow::StaticClass(), FVector(20, 0, 0), FRotator::ZeroRotator, P);
	if (!TestNotNull(TEXT("slam projectile spawned"), Arrow))
	{
		WoTTest::DestroySpecWorld(World); return false;
	}
	Wall->TouchActor(Arrow);
	TestEqual(TEXT("ordinary projectile is slammed straight down"),
		Arrow->ProjectileMovement->Velocity, FVector(0, 0, -1));
	TestTrue(TEXT("slam enables retail gravity"),
		Arrow->ProjectileMovement->ProjectileGravityScale > 0.9f);
	Wall->Destroy();

	AWoTCharacter* Caster = World->SpawnActor<AWoTCharacter>(
		AWoTCharacter::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, P);
	if (!TestNotNull(TEXT("Sever caster spawned"), Caster))
	{
		WoTTest::DestroySpecWorld(World); return false;
	}
	AWoTPlayerController* PC = World->SpawnActor<AWoTPlayerController>(
		AWoTPlayerController::StaticClass(), FVector::ZeroVector,
		FRotator::ZeroRotator, P);
	if (!TestNotNull(TEXT("Sever caster controller spawned"), PC))
	{
		WoTTest::DestroySpecWorld(World); return false;
	}
	PC->SetAsLocalPlayerController();
	PC->Possess(Caster);
	PC->SetControlRotation(FRotator::ZeroRotator);
	// AFTER possession: Possess() restarts the movement mode (walking->falling),
	// silently discarding a pre-possession MOVE_Flying — the caster then falls
	// out of its own aim cone over the spec's ticks.
	Caster->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	Caster->GetCharacterMovement()->StopMovementImmediately();
	AWoTCharacter* Target = World->SpawnActor<AWoTCharacter>(
		AWoTCharacter::StaticClass(), FVector(500, 0, 0), FRotator::ZeroRotator, P);
	if (!TestNotNull(TEXT("Sever target spawned"), Target))
	{
		WoTTest::DestroySpecWorld(World); return false;
	}
	AWoTAngrealInvWallOfAir* Sever = World->SpawnActor<AWoTAngrealInvWallOfAir>(
		AWoTAngrealInvWallOfAir::StaticClass(), FVector(0, 100, 0),
		FRotator::ZeroRotator, P);
	if (!TestNotNull(TEXT("Sever inventory spawned"), Sever))
	{
		WoTTest::DestroySpecWorld(World); return false;
	}
	Sever->SetOwner(Caster);
	Sever->CurCharges = 10;
	Sever->CastArtifact();
	AWoTWallOfAirProjectile* CastWall = Sever->GetWallForTest();
	if (!TestNotNull(TEXT("Sever cast spawned barrier"), CastWall)
		|| !TestNotNull(TEXT("caster anchor installed"), Sever->GetCasterAnchorForTest())
		|| !TestNotNull(TEXT("victim anchor installed"), Sever->GetVictimAnchorForTest()))
	{
		WoTTest::DestroySpecWorld(World); return false;
	}
	TestFalse(TEXT("caster anchor suppresses icon"),
		Sever->GetCasterAnchorForTest()->bDisplayIcon);
	TestTrue(TEXT("victim anchor shows icon"),
		Sever->GetVictimAnchorForTest()->bDisplayIcon);
	WoTTest::TickWorld(World, 1);
	const int32 ChargesAfterInitialPump = Sever->CurCharges;
	WoTTest::TickWorld(World, 80);
	TestEqual(TEXT("charge pump waits for 1.5 second cadence"),
		Sever->CurCharges, ChargesAfterInitialPump);
	WoTTest::TickWorld(World, 15);
	TestEqual(TEXT("charge pump consumes one charge after cadence"),
		Sever->CurCharges, ChargesAfterInitialPump - 1);
	Target->Destroy();
	WoTTest::TickWorld(World, 2);
	TestTrue(TEXT("endpoint death destroys wall"), !IsValid(CastWall));
	TestFalse(TEXT("endpoint death ends cast"), Sever->IsCastingForTest());

	AWoTCharacter* Target2 = World->SpawnActor<AWoTCharacter>(
		AWoTCharacter::StaticClass(), FVector(500, 0, 0), FRotator::ZeroRotator, P);
	if (!TestNotNull(TEXT("Sever cleanup target spawned"), Target2))
	{
		WoTTest::DestroySpecWorld(World); return false;
	}
	Sever->CurCharges = 10;
	Sever->CastArtifact();
	AWoTWallOfAirProjectile* CastWall2 = Sever->GetWallForTest();
	if (!TestNotNull(TEXT("second Sever cast spawned barrier"), CastWall2))
	{
		WoTTest::DestroySpecWorld(World); return false;
	}
	Sever->UnCastArtifact();
	TestTrue(TEXT("UnCast destroys barrier"), !IsValid(CastWall2));
	TestNull(TEXT("UnCast removes caster anchor"), Sever->GetCasterAnchorForTest());
	TestNull(TEXT("UnCast removes victim anchor"), Sever->GetVictimAnchorForTest());
	WoTTest::DestroySpecWorld(World);
	return true;
}

SYSTEMS_TEST(FWoTAMALiveIceDomeSpec, "WoT.AMA.LiveIceDome")
bool FWoTAMALiveIceDomeSpec::RunTest(const FString&)
{
	// G43: the dome's AMApearl01 IceTexture must ANIMATE — the fractal
	// catalog carries its runtime record (AMAPearl.uc Skin=IceTexture
	// 'Angreal.AMAPearl.AMApearl01'; retail sim = fractalice.py's
	// MoveIce/BlitTexIce twins), and the subsystem's spawn hook rebinds the
	// dome's modulate MID (Tex = the imported AMApearl01 asset) to the live
	// sim texture on the next tick.
	UWoTFractalCatalog* Catalog = LoadObject<UWoTFractalCatalog>(nullptr,
		TEXT("/Game/WoT/Data/DA_WoTFractalCatalog.DA_WoTFractalCatalog"));
	if (!TestNotNull(TEXT("fractal catalog loads"), Catalog))
	{
		return false;
	}
	const FWoTFractalTextureRecord* IceRecord = Catalog->Textures.FindByPredicate(
		[](const FWoTFractalTextureRecord& Record)
		{
			return Record.QualifiedName() == TEXT("Angreal.AMAPearl.AMApearl01");
		});
	if (!TestNotNull(TEXT("catalog carries the AMApearl01 ice record"),
		IceRecord))
	{
		return false;
	}

	UWorld* World = WoTTest::CreateSpecWorld();
	UWoTFractalSubsystem* Subsystem = World->GetSubsystem<UWoTFractalSubsystem>();
	if (!TestNotNull(TEXT("fractal subsystem live"), Subsystem))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Subsystem->SetRenderGateBypassForTesting(true);
	FActorSpawnParameters P = WeaveSystemsAlwaysSpawn();
	AWoTAMAPearl* Pearl = World->SpawnActor<AWoTAMAPearl>(
		AWoTAMAPearl::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, P);
	if (!TestNotNull(TEXT("AMA pearl spawned"), Pearl))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	// The spawn hook defers to the next subsystem tick (FlushPendingBinds).
	Subsystem->Tick(0.05f);
	UTexture2D* LiveTexture =
		Subsystem->FindLiveTexture(TEXT("Angreal.AMAPearl.AMApearl01"));
	TestNotNull(TEXT("live ice sim texture exists after bind"), LiveTexture);
	UWoTVertexAnimComponent* Dome =
		Pearl->FindComponentByClass<UWoTVertexAnimComponent>();
	bool bAnySlotLive = false;
	if (TestNotNull(TEXT("dome visual exists"), Dome))
	{
		for (int32 Slot = 0; Slot < Dome->GetNumMaterials(); ++Slot)
		{
			UTexture* BoundTexture = nullptr;
			if (UMaterialInterface* Material = Dome->GetMaterial(Slot))
			{
				Material->GetTextureParameterValue(
					FHashedMaterialParameterInfo(FName(TEXT("Tex"))),
					BoundTexture);
			}
			bAnySlotLive |= (BoundTexture && BoundTexture == LiveTexture);
		}
	}
	TestTrue(TEXT("a dome slot binds the LIVE ice texture (not the static frame)"),
		bAnySlotLive);
	WoTTest::DestroySpecWorld(World);
	return true;
}

#undef SYSTEMS_TEST
#endif // WITH_AUTOMATION_TESTS
