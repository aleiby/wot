// Production-site regression matrix for retail Trace/LineOfSight semantics.
// Each spec drives the real consumer (or a narrow test-only accessor) so a
// local query regression turns that consumer's spec red.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../AI/WoTPathNodeIterator.h"
#include "../AI/WoTPathNode.h"
#include "../AI/WoTSight.h"
#include "../AI/WoTBrainComponent.h"
#include "../AI/WoTCasterComponent.h"
#include "../AI/WoTCreatureBehaviors.h"
#include "../AI/WoTNPCCharacter.h"
#include "../AI/WoTNPCController.h"
#include "../AI/WoTWeaponComponent.h"
#include "../WoTCharacter.h"
#include "../WoTBeamEffect.h"
#include "../WoTEvents.h"
#include "../WoTMover.h"
#include "../WoTRainCyd.h"
#include "../Inventory/WoTAngrealInventory.h"
#include "../Inventory/WoTAngrealInvSpecial.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../Inventory/WoTAngrealInvBalefire.h"
#include "../Inventory/WoTAngrealInvLevitate.h"
#include "../Inventory/WoTAngrealInvLightning.h"
#include "../Inventory/WoTAngrealInvTracer.h"
#include "../Inventory/WoTSeal.h"
#include "../Inventory/WoTTrapDetectLeech.h"
#include "../Inventory/WoTWhirlwindLeech.h"
#include "../Projectiles/WoTEarthTremorFX.h"
#include "../Projectiles/WoTDartProjectile.h"
#include "../Projectiles/WoTExpWardProjectile.h"
#include "../Traps/WoTSpear.h"
#include "../Traps/WoTTrapBlocker.h"
#include "../WoTDecal.h"
#include "../WoTGameMode.h"
#include "../WoTPlayerController.h"
#include "../WoTWhirlwindFunnel.h"

#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SphereComponent.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/ProjectileMovementComponent.h"

namespace
{
	FActorSpawnParameters TraceParitySpawnParams()
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		return Params;
	}

	AActor* TraceParityBox(UWorld* World, UClass* Class,
		const FVector& At, ECollisionChannel ObjectType,
		const FCollisionResponseContainer& Responses,
		const FVector& Extent = FVector(12, 40, 40))
	{
		AActor* Actor = World->SpawnActor<AActor>(
			Class, At, FRotator::ZeroRotator, TraceParitySpawnParams());
		UBoxComponent* Box = NewObject<UBoxComponent>(Actor);
		if (Actor->GetRootComponent())
		{
			Box->SetupAttachment(Actor->GetRootComponent());
		}
		else
		{
			Actor->SetRootComponent(Box);
		}
		Box->SetBoxExtent(Extent);
		Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Box->SetCollisionObjectType(ObjectType);
		Box->SetCollisionResponseToChannels(Responses);
		Box->RegisterComponent();
		Actor->SetActorLocation(At);
		return Actor;
	}

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTraceParityOverlapOnlyActor,
	"WoT.TraceParity.TraceTrue.OverlapOnlyActor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTraceParityOverlapOnlyActor::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTTrapBlocker* Blocker = World->SpawnActor<AWoTTrapBlocker>(
		FVector(100, 0, 100), FRotator::ZeroRotator, TraceParitySpawnParams());
	Blocker->SetBlocking(true, false, false, 20.0f, 40.0f);
	AWoTCharacter* Target = World->SpawnActor<AWoTCharacter>(
		FVector(250, 0, 100), FRotator::ZeroRotator, TraceParitySpawnParams());
	WoTTest::TickWorld(World, 1);

	FHitResult Hit;
	TestTrue(TEXT("Trace(true) reaches the pawn behind an overlap-only TrapBlocker"),
		WoTRetailActorTrace(World, FVector(0, 0, 100), FVector(350, 0, 100),
			nullptr, Hit));
	TestTrue(TEXT("overlap-only TrapBlocker is not a retail actor-trace hit"),
		Hit.GetActor() == Target);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTraceParityExplicitProjectileTarget,
	"WoT.TraceParity.TraceTrue.ExplicitProjectileTarget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTraceParityExplicitProjectileTarget::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	FCollisionResponseContainer OverlapOnly;
	OverlapOnly.SetAllChannels(ECR_Overlap);
	AWoTTrigger* Proximity = Cast<AWoTTrigger>(TraceParityBox(
		World, AWoTTrigger::StaticClass(), FVector(100, 0, 100),
		ECC_WorldDynamic, OverlapOnly));
	AWoTTrigger* Shoot = Cast<AWoTTrigger>(TraceParityBox(
		World, AWoTTrigger::StaticClass(), FVector(250, 0, 100),
		ECC_WorldDynamic, OverlapOnly));
	Proximity->TriggerType = EWoTTriggerType::PawnProximity;
	Shoot->TriggerType = EWoTTriggerType::Shoot;
	FHitResult Hit;
	TestTrue(TEXT("Trace(true) finds an overlap-only explicit bProjTarget"),
		WoTRetailActorTrace(World, FVector(0, 0, 100), FVector(350, 0, 100),
			nullptr, Hit));
	TestTrue(TEXT("ordinary trigger is skipped and TT_Shoot trigger is accepted"),
		Hit.GetActor() == Shoot);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTraceParityProjectileBlockBoth,
	"WoT.TraceParity.TraceTrue.ProjectileBlockBoth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTraceParityProjectileBlockBoth::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	auto SpawnDart = [&](float X)
	{
		AWoTDartProjectile* Dart = World->SpawnActor<AWoTDartProjectile>(
			FVector(X, 0, 100), FRotator::ZeroRotator, TraceParitySpawnParams());
		Dart->ProjectileMovement->Deactivate();
		Dart->bProjTarget = false;
		Dart->Collision->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		Dart->Collision->SetCollisionResponseToAllChannels(ECR_Overlap);
		return Dart;
	};
	AWoTDartProjectile* Rejected = SpawnDart(100.0f);
	AWoTDartProjectile* Accepted = SpawnDart(220.0f);
	Rejected->Collision->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	Accepted->Collision->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
	Accepted->Collision->SetCollisionResponseToChannel(
		ECC_GameTraceChannel1, ECR_Block);
	FHitResult Hit;
	TestTrue(TEXT("a non-bProjTarget projectile blocking both retail flags qualifies"),
		WoTRetailActorTrace(World, FVector(0, 0, 100), FVector(350, 0, 100),
			nullptr, Hit));
	TestTrue(TEXT("overlap-only projectile is skipped before block-both projectile"),
		Hit.GetActor() == Accepted);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTraceParityRecursiveQueryBudget,
	"WoT.TraceParity.RecursiveQueryBudget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTraceParityRecursiveQueryBudget::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	FCollisionResponseContainer RejectedResponses;
	RejectedResponses.SetAllChannels(ECR_Ignore);
	RejectedResponses.SetResponse(ECC_Visibility, ECR_Block);
	for (int32 Index = 0; Index < 6; ++Index)
	{
		TraceParityBox(World, AActor::StaticClass(),
			FVector(150 + Index * 180, 0, 100), ECC_WorldDynamic,
			RejectedResponses);
	}
	AWoTCharacter* Target = World->SpawnActor<AWoTCharacter>(
		FVector(1500, 0, 100), FRotator::ZeroRotator, TraceParitySpawnParams());
	WoTTest::TickWorld(World, 1);

	WoTResetRetailTraceQueryCountForTesting();
	FHitResult Hit;
	TestTrue(TEXT("accepted hit beyond 1000uu is found behind rejected actors"),
		WoTRetailActorTraceRecursive(World, FVector(0, 0, 100),
			FVector(1, 0, 0), 2000.0f, nullptr, Hit));
	TestTrue(TEXT("ordered result matches the retail recursive walk"),
		Hit.GetActor() == Target);
	TestEqual(TEXT("recursive trace uses one scene query"),
		WoTGetRetailTraceQueryCountForTesting(), 1);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTraceParityBalefireNoActorCap,
	"WoT.TraceParity.BalefireNoActorCap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTraceParityBalefireNoActorCap::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	FCollisionResponseContainer Rejected;
	Rejected.SetAllChannels(ECR_Block);
	for (int32 Index = 0; Index < 40; ++Index)
	{
		TraceParityBox(World, AActor::StaticClass(),
			FVector(20 + Index * 8, 0, 100), ECC_WorldDynamic, Rejected,
			FVector(2, 20, 20));
	}
	TraceParityBox(World, AStaticMeshActor::StaticClass(), FVector(400, 0, 100),
		ECC_WorldStatic, Rejected, FVector(5, 40, 40));
	AWoTAngrealInvBalefire* Balefire = World->SpawnActor<AWoTAngrealInvBalefire>(
		FVector::ZeroVector, FRotator::ZeroRotator, TraceParitySpawnParams());
	WoTResetRetailTraceQueryCountForTesting();
	Balefire->PlaceBeamDecals(FVector(0, 0, 100), FVector::ForwardVector, 500.0f);
	int32 DecalCount = 0;
	for (TActorIterator<AWoTDecal> It(World); It; ++It) { ++DecalCount; }
	TestTrue(TEXT("Balefire crosses more than 32 rejected actors and reaches the wall"),
		DecalCount > 0);
	TestTrue(TEXT("Balefire placement delegates each segment to the shared helper"),
		WoTGetRetailTraceQueryCountForTesting() > 0);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTraceParityRainCydMoverGround,
	"WoT.TraceParity.RainCydMoverGround",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTraceParityRainCydMoverGround::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	FCollisionResponseContainer Blocks; Blocks.SetAllChannels(ECR_Block);
	TraceParityBox(World, AWoTMover::StaticClass(), FVector(0, 0, 500),
		ECC_WorldDynamic, Blocks, FVector(100, 100, 10));
	TraceParityBox(World, AStaticMeshActor::StaticClass(), FVector(0, 0, 0),
		ECC_WorldStatic, Blocks, FVector(100, 100, 10));
	WoTResetRetailTraceQueryCountForTesting();
	AWoTRainCyd* Rain = World->SpawnActor<AWoTRainCyd>(
		FVector(0, 0, 900), FRotator::ZeroRotator, TraceParitySpawnParams());
	TestEqual(TEXT("RainCyd ground selection uses one accepted-hit query"),
		WoTGetRetailTraceQueryCountForTesting(), 1);
	TestTrue(TEXT("RainCyd anchors to the nearer WorldDynamic mover"),
		Rain && Rain->GetActorLocation().Z > 300.0f);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTraceParityDecalAlignEdgeGap,
	"WoT.TraceParity.Decal.AlignEdgeGap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTraceParityDecalAlignEdgeGap::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	FCollisionResponseContainer Blocks;
	Blocks.SetAllChannels(ECR_Block);
	TraceParityBox(World, AStaticMeshActor::StaticClass(), FVector::ZeroVector,
		ECC_WorldStatic, Blocks, FVector(1, 200, 200));
	AWoTDecal* Decal = World->SpawnActor<AWoTDecal>(
		FVector(8, 0, 0), FRotator::ZeroRotator, TraceParitySpawnParams());
	TArray<float> QueryLengths;
	WoTSetRetailTraceObserverForTesting(
		[&QueryLengths](const FVector& Start, const FVector& End)
		{
			QueryLengths.Add(FVector::Distance(Start, End));
		});
	Decal->Align(FVector::ForwardVector);
	WoTSetRetailTraceObserverForTesting({});
	TestTrue(TEXT("Align issues retail five-unit corner rays"),
		QueryLengths.ContainsByPredicate([](float Length)
		{
			return FMath::IsNearlyEqual(Length, 5.0f, 0.01f);
		}));
	TestFalse(TEXT("Align never doubles the retail corner distance"),
		QueryLengths.ContainsByPredicate([](float Length)
		{
			return FMath::IsNearlyEqual(Length, 10.0f, 0.01f);
		}));
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTraceParityWhirlwindPostMovePresentation,
	"WoT.TraceParity.Whirlwind.PostMovePresentation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTraceParityWhirlwindPostMovePresentation::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	FCollisionResponseContainer FloorResponses;
	FloorResponses.SetAllChannels(ECR_Block);
	TraceParityBox(World, AStaticMeshActor::StaticClass(), FVector(0, 0, 0),
		ECC_WorldStatic, FloorResponses, FVector(300, 300, 10));
	AWoTCharacter* Host = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 1500), FRotator::ZeroRotator, TraceParitySpawnParams());
	UWoTWhirlwindLeech* Leech = NewObject<UWoTWhirlwindLeech>(Host);
	Leech->RegisterComponent();
	Leech->AttachTo(Host);
	Leech->SetDesiredLocation(FVector(100, 0, 1500));
	WoTResetRetailTraceQueryCountForTesting();
	Leech->TickComponent(1.0f, LEVELTICK_All, nullptr);
	AWoTWhirlwindFunnel* Funnel = Leech->GetFunnelForTest();
	TestNotNull(TEXT("production tick creates the funnel"), Funnel);
	if (Funnel)
	{
		TestTrue(TEXT("presentation starts from the post-authority-move host"),
			FMath::Abs(Funnel->GetActorLocation().X - Host->GetActorLocation().X)
				<= 11.0f);
		TestEqual(TEXT("presentation clamps its accepted floor anchor to 500 below host"),
			Funnel->GetActorLocation().Z, Host->GetActorLocation().Z - 500.0, 1.0);
	}
	TestEqual(TEXT("one whirlwind tick uses one recursive scene query"),
		WoTGetRetailTraceQueryCountForTesting(), 1);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTraceParityLineOfSightRange,
	"WoT.TraceParity.LineOfSight.Range",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTraceParityLineOfSightRange::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Viewer = World->SpawnActor<AWoTCharacter>(
		FVector::ZeroVector, FRotator::ZeroRotator, TraceParitySpawnParams());
	AActor* Target = TraceParityBox(World, AActor::StaticClass(),
		FVector(3500, 0, 0), ECC_WorldDynamic, FCollisionResponseContainer());
	WoTSight::SetTraceOverrideForTesting(
		[](const FVector&, const FVector&) { return true; });
	FWoTLineOfSightParams Params;
	Params.bViewerIsPlayer = true;
	TestTrue(TEXT("player no-cone non-pawn range is 4000"),
		WoTSight::LineOfSightTo(Viewer, Target, Params));
	WoTSight::ResetStateForTesting(Viewer);
	Params.bViewerIsPlayer = false;
	TestFalse(TEXT("NPC no-cone non-pawn range is 3000"),
		WoTSight::LineOfSightTo(Viewer, Target, Params));
	WoTSight::SetTraceOverrideForTesting({});
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTraceParityLineOfSightNearEndpoint,
	"WoT.TraceParity.LineOfSight.NearEndpoint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTraceParityLineOfSightNearEndpoint::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Viewer = World->SpawnActor<AWoTCharacter>(
		FVector::ZeroVector, FRotator::ZeroRotator, TraceParitySpawnParams());
	AActor* Target = TraceParityBox(World, AActor::StaticClass(),
		FVector(500, 0, 100), ECC_WorldDynamic, FCollisionResponseContainer(),
		FVector(20, 20, 50));
	TArray<FVector> Ends;
	WoTSight::SetTraceOverrideForTesting(
		[&Ends](const FVector&, const FVector& End)
		{
			Ends.Add(End);
			return true;
		});
	FWoTLineOfSightParams Params;
	Params.bViewerIsPlayer = true;
	TestTrue(TEXT("near non-pawn is visible at its native body endpoint"),
		WoTSight::LineOfSightTo(Viewer, Target, Params));
	TestEqual(TEXT("near non-pawn uses one ray"), Ends.Num(), 1);
	if (Ends.Num() == 1)
	{
		TestTrue(TEXT("endpoint is Location + 0.8 CollisionHeight"),
			Ends[0].Equals(Target->GetActorLocation() + FVector(0, 0, 40), 0.1f));
	}
	WoTSight::SetTraceOverrideForTesting({});
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTraceParityLineOfSightSkill,
	"WoT.TraceParity.LineOfSight.Skill",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTraceParityLineOfSightSkill::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
	AWoTGameMode* Game = World->GetAuthGameMode<AWoTGameMode>();
	TestNotNull(TEXT("spec world has the production game mode"), Game);
	if (Game)
	{
		FString Error;
		Game->InitGame(TEXT("TraceParity"), TEXT("?Difficulty=3"), Error);
	}
	AWoTNPCCharacter* Viewer = World->SpawnActor<AWoTNPCCharacter>(
		FVector::ZeroVector, FRotator::ZeroRotator, TraceParitySpawnParams());
	AWoTCharacter* Target = World->SpawnActor<AWoTCharacter>(
		FVector(1300, 0, 1300), FRotator::ZeroRotator, TraceParitySpawnParams());
	TestEqual(TEXT("Pawn PreBeginPlay difficulty contributes to runtime Skill"),
		Viewer->Skill, 3.0f);
	WoTSight::SetTraceOverrideForTesting(
		[](const FVector&, const FVector&) { return true; });
	WoTSight::SetRandomOverrideForTesting([]() { return 1.0f; });
	TestTrue(TEXT("production ConeSight passes effective Skill to LineOfSightTo"),
		WoTSight::ConeSight(Viewer, 0.0f, 2500.0f, Target, 128));
	Viewer->Skill = 0.0f;
	WoTSight::ResetStateForTesting(Viewer);
	TestFalse(TEXT("the same vertical cone check fails at zero Skill"),
		WoTSight::ConeSight(Viewer, 0.0f, 2500.0f, Target, 128));
	WoTSight::SetTraceOverrideForTesting({});
	WoTSight::SetRandomOverrideForTesting({});
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTraceParityLineOfSightEnemyAttempts,
	"WoT.TraceParity.LineOfSight.EnemyFirstAttempts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTraceParityLineOfSightEnemyAttempts::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Viewer = World->SpawnActor<AWoTCharacter>(
		FVector::ZeroVector, FRotator::ZeroRotator, TraceParitySpawnParams());
	AWoTCharacter* Enemy = World->SpawnActor<AWoTCharacter>(
		FVector(900, 0, 0), FRotator::ZeroRotator, TraceParitySpawnParams());
	TArray<FVector> Starts;
	WoTSight::SetTraceOverrideForTesting(
		[&Starts](const FVector& Start, const FVector&)
		{
			Starts.Add(Start);
			return Starts.Num() == 2;
		});
	FWoTLineOfSightParams Params;
	Params.bShowSelf = true;
	Params.bViewerIsPlayer = false;
	Params.Enemy = Enemy;
	TestTrue(TEXT("enemy second center attempt can establish sight"),
		WoTSight::LineOfSightTo(Viewer, Enemy, Params));
	TestEqual(TEXT("enemy tries exactly two center rays first"), Starts.Num(), 2);
	if (Starts.Num() == 2)
	{
		TestTrue(TEXT("first enemy attempt starts at eye height"),
			Starts[0].Z > Viewer->GetActorLocation().Z);
		TestTrue(TEXT("second enemy attempt starts at actor location"),
			Starts[1].Equals(Viewer->GetActorLocation(), 0.1f));
	}
	WoTSight::SetTraceOverrideForTesting({});
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTraceParityLineOfSightFlag,
	"WoT.TraceParity.LineOfSight.LOSFlag",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTraceParityLineOfSightFlag::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Viewer = World->SpawnActor<AWoTCharacter>(
		FVector::ZeroVector, FRotator::ZeroRotator, TraceParitySpawnParams());
	AWoTCharacter* Target = World->SpawnActor<AWoTCharacter>(
		FVector(1500, 0, 0), FRotator::ZeroRotator, TraceParitySpawnParams());
	int32 Rays = 0;
	WoTSight::SetTraceOverrideForTesting(
		[&Rays](const FVector&, const FVector&) { ++Rays; return true; });
	FWoTLineOfSightParams Params;
	Params.bShowSelf = true;
	Params.bViewerIsPlayer = true;
	Params.SightRadius = 2000.0f;
	Params.TargetVisibility = 128;
	WoTSight::ResetStateForTesting(Viewer);
	TestTrue(TEXT("first cone pulse toggles LOS flag on and traces"),
		WoTSight::LineOfSightTo(Viewer, Target, Params));
	TestEqual(TEXT("first pulse issued one far center ray"), Rays, 1);
	Rays = 0;
	TestFalse(TEXT("second cone pulse toggles off and thins outer-half pawn"),
		WoTSight::LineOfSightTo(Viewer, Target, Params));
	TestEqual(TEXT("thinned pulse consumes no trace"), Rays, 0);
	WoTSight::SetTraceOverrideForTesting({});
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTraceParityLineOfSightRandomThinning,
	"WoT.TraceParity.LineOfSight.RandomThinning",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTraceParityLineOfSightRandomThinning::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Viewer = World->SpawnActor<AWoTCharacter>(
		FVector::ZeroVector, FRotator::ZeroRotator, TraceParitySpawnParams());
	AWoTCharacter* Target = World->SpawnActor<AWoTCharacter>(
		FVector(1200, 0, 0), FRotator::ZeroRotator, TraceParitySpawnParams());
	int32 Rays = 0;
	WoTSight::SetTraceOverrideForTesting(
		[&Rays](const FVector&, const FVector&) { ++Rays; return true; });
	FWoTLineOfSightParams Params;
	Params.bViewerIsPlayer = false;
	WoTSight::SetRandomOverrideForTesting([] { return 0.49f; });
	TestFalse(TEXT("NPC far-pawn FRand below 0.5 thins the query"),
		WoTSight::LineOfSightTo(Viewer, Target, Params));
	TestEqual(TEXT("randomly thinned query emits no ray"), Rays, 0);
	WoTSight::SetRandomOverrideForTesting([] { return 0.5f; });
	TestTrue(TEXT("FRand exactly 0.5 proceeds to the center ray"),
		WoTSight::LineOfSightTo(Viewer, Target, Params));
	TestEqual(TEXT("accepted random branch emits one ray"), Rays, 1);
	WoTSight::SetRandomOverrideForTesting({});
	WoTSight::SetTraceOverrideForTesting({});
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTraceParitySightAndGoals,
	"WoT.TraceParity.SightAndGoals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTraceParitySightAndGoals::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Viewer = World->SpawnActor<AWoTCharacter>(
		FVector::ZeroVector, FRotator::ZeroRotator, TraceParitySpawnParams());
	AActor* Target = TraceParityBox(World, AActor::StaticClass(),
		FVector(500, 0, 0), ECC_WorldDynamic, FCollisionResponseContainer());
	UWoTBrainComponent* Brain = NewObject<UWoTBrainComponent>(Viewer);
	Brain->RegisterComponent();
	Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Target);
	int32 Rays = 0;
	WoTSight::SetTraceOverrideForTesting(
		[&Rays](const FVector&, const FVector&) { ++Rays; return true; });
	TestTrue(TEXT("actor-goal visibility uses shared native LineOfSightTo"),
		Brain->IsGoalVisible(EWoTGoalIndex::Threat));
	TestEqual(TEXT("near actor goal uses the native body ray"), Rays, 1);
	WoTSight::SetTraceOverrideForTesting({});
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTraceParityPlayerStartLineOfSight,
	"WoT.TraceParity.PlayerStartLineOfSight",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTraceParityPlayerStartLineOfSight::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Viewer = World->SpawnActor<AWoTCharacter>(
		FVector::ZeroVector, FRotator::ZeroRotator, TraceParitySpawnParams());
	AActor* Start = TraceParityBox(World, AActor::StaticClass(),
		FVector(500, 0, 100), ECC_WorldDynamic, FCollisionResponseContainer(),
		FVector(20, 20, 50));
	FVector End = FVector::ZeroVector;
	WoTSight::SetTraceOverrideForTesting(
		[&End](const FVector&, const FVector& InEnd) { End = InEnd; return true; });
	TestTrue(TEXT("player-start scoring uses the shared no-cone LOS"),
		AWoTGameMode::CanPawnSeeStartForScoring(Viewer, Start));
	TestTrue(TEXT("near player start uses Location + 0.8 CollisionHeight"),
		End.Equals(Start->GetActorLocation() + FVector(0, 0, 40), 0.1f));
	WoTSight::SetTraceOverrideForTesting({});
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTraceParityTracerReach,
	"WoT.TraceParity.TracerReach",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FTraceParityTracerReach::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Caster = World->SpawnActor<AWoTCharacter>(
		FVector::ZeroVector, FRotator::ZeroRotator, TraceParitySpawnParams());
	AWoTSeal* Seal = World->SpawnActor<AWoTSeal>(
		FVector(300, 0, 0), FRotator::ZeroRotator, TraceParitySpawnParams());
	AWoTAngrealInvTracer* Tracer = World->SpawnActor<AWoTAngrealInvTracer>(
		FVector::ZeroVector, FRotator::ZeroRotator, TraceParitySpawnParams());
	Tracer->SetOwner(Caster);
	WoTResetRetailTraceQueryCountForTesting();
	TestTrue(TEXT("production tracer selects the directly reachable seal"),
		Tracer->GetBestTarget() == Seal);
	TestEqual(TEXT("production tracer uses the VisBlocking helper"),
		WoTGetRetailTraceQueryCountForTesting(), 1);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTraceParityDefaultTargetingDirect,
	"WoT.TraceParity.DefaultTargetingDirect",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FTraceParityDefaultTargetingDirect::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Caster = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator, TraceParitySpawnParams());
	AWoTTrapBlocker* Blocker = World->SpawnActor<AWoTTrapBlocker>(
		FVector(100, 0, 100), FRotator::ZeroRotator, TraceParitySpawnParams());
	Blocker->SetBlocking(true, false, false, 30.0f, 80.0f);
	AWoTCharacter* Target = World->SpawnActor<AWoTCharacter>(
		FVector(300, 100, 100), FRotator::ZeroRotator, TraceParitySpawnParams());
	Target->GetCapsuleComponent()->SetCapsuleSize(120.0f, 46.0f);
	AWoTAngrealInventory* Inventory = World->SpawnActor<AWoTAngrealInvTracer>(
		FVector::ZeroVector, FRotator::ZeroRotator, TraceParitySpawnParams());
	Inventory->SetOwner(Caster);
	WoTResetRetailTraceQueryCountForTesting();
	TestTrue(TEXT("production crosshair trace skips the rejected proxy"),
		Inventory->FindBestTarget(Caster->GetActorLocation(),
			FRotator::ZeroRotator, 0.0f) == Target);
	TestEqual(TEXT("default targeting performs one ordered actor query"),
		WoTGetRetailTraceQueryCountForTesting(), 1);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTraceParityLevitateGround,
	"WoT.TraceParity.LevitateGround",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FTraceParityLevitateGround::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Caster = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 300), FRotator::ZeroRotator, TraceParitySpawnParams());
	FCollisionResponseContainer Blocks; Blocks.SetAllChannels(ECR_Block);
	TraceParityBox(World, AActor::StaticClass(), FVector(0, 0, -300),
		ECC_WorldDynamic, Blocks, FVector(80, 80, 10));
	TraceParityBox(World, AActor::StaticClass(), FVector(0, 0, -800),
		ECC_WorldDynamic, Blocks, FVector(80, 80, 10));
	TraceParityBox(World, AStaticMeshActor::StaticClass(), FVector(0, 0, -1500),
		ECC_WorldStatic, Blocks, FVector(200, 200, 10));
	AWoTAngrealInvLevitate* Levitate = World->SpawnActor<AWoTAngrealInvLevitate>(
		FVector::ZeroVector, FRotator::ZeroRotator, TraceParitySpawnParams());
	Levitate->SetOwner(Caster);
	Levitate->SetLevitatingForTest(true);
	WoTResetRetailTraceQueryCountForTesting();
	Levitate->Tick(0.01f);
	TestEqual(TEXT("one levitate tick performs one recursive scene query"),
		WoTGetRetailTraceQueryCountForTesting(), 1);
	TestTrue(TEXT("production levitate tick responds to the floor"),
		!FMath::IsNearlyZero(Caster->GetVelocity().Z));
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTraceParityLegionStompFloor,
	"WoT.TraceParity.LegionStompFloor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FTraceParityLegionStompFloor::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* Legion = World->SpawnActor<AWoTNPCCharacter>(
		FVector(-300, 0, 100), FRotator::ZeroRotator, TraceParitySpawnParams());
	AWoTNPCController* Controller = World->SpawnActor<AWoTNPCController>(
		FVector::ZeroVector, FRotator::ZeroRotator, TraceParitySpawnParams());
	Controller->Possess(Legion);
	AWoTCharacter* Target = World->SpawnActor<AWoTCharacter>(
		FVector(300, 0, 100), FRotator::ZeroRotator, TraceParitySpawnParams());
	FCollisionResponseContainer Blocks; Blocks.SetAllChannels(ECR_Block);
	TraceParityBox(World, AStaticMeshActor::StaticClass(), FVector(300, 0, 0),
		ECC_WorldStatic, Blocks, FVector(100, 100, 10));
	UWoTLegionSpiritsStompBehavior* Behavior =
		NewObject<UWoTLegionSpiritsStompBehavior>(Legion);
	Behavior->RegisterComponent();
	Behavior->OnPossessedBy(Controller);
	Behavior->SetPendingStompTargetForTest(Target);
	WoTResetRetailTraceQueryCountForTesting();
	const FVector Impact = Behavior->ResolvePendingStompImpactForTest();
	TestTrue(TEXT("production stomp resolves below the target capsule"), Impact.Z < 50.0f);
	TestEqual(TEXT("stomp floor resolution uses one VisBlocking query"),
		WoTGetRetailTraceQueryCountForTesting(), 1);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTraceParityLightningStreakDirect,
	"WoT.TraceParity.LightningStreakDirect",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FTraceParityLightningStreakDirect::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* StartActor = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator, TraceParitySpawnParams());
	AWoTBeamEffect* Beam = World->SpawnActor<AWoTBeamEffect>(TraceParitySpawnParams());
	Beam->Config = TEXT("LightningStreak02");
	Beam->StartActor = StartActor;
	Beam->End = FVector(300, 0, 100);
	WoTResetRetailTraceQueryCountForTesting();
	Beam->Tick(0.0f);
	TestEqual(TEXT("production streak direct check uses VisBlocking once"),
		WoTGetRetailTraceQueryCountForTesting(), 1);
	TestTrue(TEXT("direct streak lays its authored segments"),
		Beam->GetLastSegmentOrigins().Num() > 1);
	const int32 FirstAssignments = Beam->GetStreakTextureAssignmentCount();
	TestEqual(TEXT("every visible streak segment receives a decoded random skin"),
		FirstAssignments, Beam->GetVisibleSegmentCount());
	Beam->Tick(0.0f);
	TestEqual(TEXT("every visible streak segment is re-randomized on the next frame"),
		Beam->GetStreakTextureAssignmentCount() - FirstAssignments,
		Beam->GetVisibleSegmentCount());
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTraceParityEarthTremorRockGround,
	"WoT.TraceParity.EarthTremorRockGround",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FTraceParityEarthTremorRockGround::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	FCollisionResponseContainer Blocks; Blocks.SetAllChannels(ECR_Block);
	TraceParityBox(World, AActor::StaticClass(), FVector(0, 0, 50),
		ECC_WorldDynamic, Blocks, FVector(40));
	TraceParityBox(World, AStaticMeshActor::StaticClass(), FVector(0, 0, -100),
		ECC_WorldStatic, Blocks, FVector(100, 100, 10));
	WoTResetRetailTraceQueryCountForTesting();
	AWoTEarthTremorRock* Rock = World->SpawnActor<AWoTEarthTremorRock>(
		FVector(0, 0, 100), FRotator::ZeroRotator, TraceParitySpawnParams());
	TestTrue(TEXT("production rock grounds on level below an actor blocker"),
		IsValid(Rock) && Rock->GetActorLocation().Z < 0.0f);
	TestEqual(TEXT("rock grounding uses one accepted-hit query"),
		WoTGetRetailTraceQueryCountForTesting(), 1);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTraceParityExpWardPlacement,
	"WoT.TraceParity.ExpWardPlacement",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FTraceParityExpWardPlacement::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	FCollisionResponseContainer Blocks; Blocks.SetAllChannels(ECR_Block);
	TraceParityBox(World, AActor::StaticClass(), FVector(40, 0, 100),
		ECC_WorldDynamic, Blocks, FVector(10, 50, 50));
	TraceParityBox(World, AStaticMeshActor::StaticClass(), FVector(100, 0, 100),
		ECC_WorldStatic, Blocks, FVector(5, 50, 50));
	WoTResetRetailTraceQueryCountForTesting();
	AWoTExpWardProjectile* Ward = World->SpawnActor<AWoTExpWardProjectile>(
		FVector(0, 0, 100), FRotator::ZeroRotator, TraceParitySpawnParams());
	TestTrue(TEXT("production ward skips the actor and sticks to level"),
		Ward && Ward->CreationSucceeded() && Ward->GetActorLocation().X > 80.0f);
	TestEqual(TEXT("ward placement uses one accepted-hit query"),
		WoTGetRetailTraceQueryCountForTesting(), 1);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTraceParityLightningLatch,
	"WoT.TraceParity.LightningLatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FTraceParityLightningLatch::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Caster = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator, TraceParitySpawnParams());
	AWoTCharacter* Victim = World->SpawnActor<AWoTCharacter>(
		FVector(250, 0, 100), FRotator::ZeroRotator, TraceParitySpawnParams());
	AWoTTrapBlocker* Blocker = World->SpawnActor<AWoTTrapBlocker>(
		FVector(100, 0, 100), FRotator::ZeroRotator, TraceParitySpawnParams());
	Blocker->SetBlocking(true, false, false, 30.0f, 100.0f);
	AWoTAngrealInvLightning* Lightning = World->SpawnActor<AWoTAngrealInvLightning>(
		FVector::ZeroVector, FRotator::ZeroRotator, TraceParitySpawnParams());
	WoTResetRetailTraceQueryCountForTesting();
	TestTrue(TEXT("production latch sees the pawn beyond a rejected proxy"),
		Lightning->IsAttachableForTest(Victim, Caster));
	TestTrue(TEXT("latch exercised the retail actor query"),
		WoTGetRetailTraceQueryCountForTesting() >= 1);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTraceParityAutoTargetReach,
	"WoT.TraceParity.AutoTargetReach",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FTraceParityAutoTargetReach::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Owner = World->SpawnActor<AWoTCharacter>(
		FVector::ZeroVector, FRotator::ZeroRotator, TraceParitySpawnParams());
	AWoTCharacter* Candidate = World->SpawnActor<AWoTCharacter>(
		FVector(300, 0, 0), FRotator::ZeroRotator, TraceParitySpawnParams());
	WoTResetRetailTraceQueryCountForTesting();
	TestTrue(TEXT("production AutoTarget reach ignores actor capsules"),
		AWoTAngrealInventory::CanRetailAutoTargetReachForTest(
			World, Owner, nullptr, Owner->GetActorLocation(), Candidate));
	TestEqual(TEXT("AutoTarget reach uses the VisBlocking helper"),
		WoTGetRetailTraceQueryCountForTesting(), 1);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTraceParityWeaponAim,
	"WoT.TraceParity.WeaponAim",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FTraceParityWeaponAim::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Shooter = World->SpawnActor<AWoTCharacter>(
		FVector::ZeroVector, FRotator::ZeroRotator, TraceParitySpawnParams());
	AWoTCharacter* Target = World->SpawnActor<AWoTCharacter>(
		FVector(1000, 0, 0), FRotator::ZeroRotator, TraceParitySpawnParams());
	Target->GetCharacterMovement()->Velocity = FVector(0, 200, 0);
	WoTResetRetailTraceQueryCountForTesting();
	const FRotator Aim = UWoTWeaponComponent::AdjustProjectileWeaponAim(
		Shooter, Target, 1000.0f, Shooter->GetActorLocation(), 1.0f);
	TestTrue(TEXT("production weapon aim leads the moving target"), Aim.Yaw > 0.0f);
	TestTrue(TEXT("weapon refinement uses production VisBlocking traces"),
		WoTGetRetailTraceQueryCountForTesting() >= 2);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTraceParityPawnCanSeeActor,
	"WoT.TraceParity.PawnCanSeeActor.CenterRay",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FTraceParityPawnCanSeeActor::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Viewer = World->SpawnActor<AWoTCharacter>(
		FVector::ZeroVector, FRotator::ZeroRotator, TraceParitySpawnParams());
	AWoTCharacter* Target = World->SpawnActor<AWoTCharacter>(
		FVector(300, 0, 0), FRotator::ZeroRotator, TraceParitySpawnParams());
	WoTResetRetailTraceQueryCountForTesting();
	TestTrue(TEXT("production PawnCanSeeActor sees its unobstructed target"),
		WoTSight::PawnCanSeeActor(Viewer, Target, 0.5f, false));
	TestEqual(TEXT("PawnCanSeeActor uses its own VisBlocking center ray"),
		WoTGetRetailTraceQueryCountForTesting(), 1);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTraceParityCasterVolleySight,
	"WoT.TraceParity.PawnCanSeeActor.CasterVolley",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTraceParityCasterVolleySight::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* Caster = World->SpawnActor<AWoTNPCCharacter>(
		FVector::ZeroVector, FRotator::ZeroRotator, TraceParitySpawnParams());
	AWoTCharacter* Target = World->SpawnActor<AWoTCharacter>(
		FVector(3000, 0, 0), FRotator::ZeroRotator, TraceParitySpawnParams());
	UWoTCasterComponent* CasterComponent = NewObject<UWoTCasterComponent>(Caster);
	CasterComponent->RegisterComponent();
	TestTrue(TEXT("caster volley uses script PawnCanSeeActor without native sight range"),
		CasterComponent->CanContinueVolleyForTest(Target));
	TestFalse(TEXT("the prior 2500-unit ConeSight gate would reject this target"),
		WoTSight::ConeSight(Caster, 0.0f, 2500.0f, Target, 128));
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTraceParityDodgeExtent,
	"WoT.TraceParity.DodgeExtent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FTraceParityDodgeExtent::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* Pawn = World->SpawnActor<AWoTNPCCharacter>(
		FVector::ZeroVector, FRotator::ZeroRotator, TraceParitySpawnParams());
	AWoTNPCController* Controller = World->SpawnActor<AWoTNPCController>(
		FVector::ZeroVector, FRotator::ZeroRotator, TraceParitySpawnParams());
	Controller->Possess(Pawn);
	FCollisionResponseContainer Blocks; Blocks.SetAllChannels(ECR_Block);
	TraceParityBox(World, AWoTMover::StaticClass(), FVector(150, 0, 0),
		ECC_WorldDynamic, Blocks, FVector(10, 100, 100));
	WoTResetRetailTraceQueryCountForTesting();
	TestFalse(TEXT("production dodge extent sweep is stopped by a mover"),
		Controller->IsDodgeDestinationUnobstructedForTest(FVector::ForwardVector, 300.0f));
	TestEqual(TEXT("dodge check uses one accepted-hit extent query"),
		WoTGetRetailTraceQueryCountForTesting(), 1);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTraceParityTrapDetectDirect,
	"WoT.TraceParity.TrapDetectDirect",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FTraceParityTrapDetectDirect::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Host = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator, TraceParitySpawnParams());
	const float EyeZ = 100.0f + Host->BaseEyeHeight;
	AWoTTrapBlocker* Blocker = World->SpawnActor<AWoTTrapBlocker>(
		FVector(100, 0, EyeZ), FRotator::ZeroRotator, TraceParitySpawnParams());
	Blocker->SetBlocking(true, false, false, 30.0f, 80.0f);
	FCollisionResponseContainer TrapResponses; TrapResponses.SetAllChannels(ECR_Block);
	AWoTSpear* Spear = Cast<AWoTSpear>(TraceParityBox(World, AWoTSpear::StaticClass(),
		FVector(250, 60, EyeZ), ECC_WorldDynamic,
		TrapResponses, FVector(10, 70, 40)));
	AWoTSpear* FallbackSpear = Cast<AWoTSpear>(TraceParityBox(
		World, AWoTSpear::StaticClass(), FVector(400, 0, EyeZ),
		ECC_WorldDynamic, TrapResponses, FVector(10, 20, 40)));
	UWoTTrapDetectLeech* Detector = NewObject<UWoTTrapDetectLeech>(Host);
	Detector->RegisterComponent();
	float Distance = 0.0f; FVector Closest = FVector::ZeroVector;
	WoTResetRetailTraceQueryCountForTesting();
	TestTrue(TEXT("production direct branch selects the spear beyond the proxy"),
		Detector->GetClosestTrapForTest(Distance, Closest) == Spear);
	TestNotNull(TEXT("fallback competitor was created"), FallbackSpear);
	TestEqual(TEXT("TrapDetect direct branch performs one actor query"),
		WoTGetRetailTraceQueryCountForTesting(), 1);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTraceParityEarthTremorRing,
	"WoT.TraceParity.EarthTremorRing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTraceParityEarthTremorRing::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* StartOverlap = World->SpawnActor<AWoTCharacter>(
		FVector::ZeroVector, FRotator::ZeroRotator, TraceParitySpawnParams());
	AWoTEarthTremor* Tremor = World->SpawnActor<AWoTEarthTremor>(
		FVector::ZeroVector, FRotator::ZeroRotator, TraceParitySpawnParams());
	const FVector Start(0, 0, 0), Slot(140, 0, 0);
	FHitResult OldHit;
	TestTrue(TEXT("old ring query reports its start-inside pawn capsule"),
		World->LineTraceSingleByChannel(OldHit, Start, Slot,
			ECC_WorldStatic, FCollisionQueryParams()));
	TestTrue(TEXT("Chaos start overlap is distance zero"),
		OldHit.Distance <= UE_KINDA_SMALL_NUMBER);
	TestTrue(TEXT("production EarthTremor ring check ignores the capsule"),
		Tremor->IsRingSlotClearForTest(Slot));
	TestNotNull(TEXT("center overlap fixture exists"), StartOverlap);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTraceParitySealDropFit,
	"WoT.TraceParity.SealDropFit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTraceParitySealDropFit::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	FCollisionResponseContainer Blocks;
	Blocks.SetAllChannels(ECR_Block);
	TraceParityBox(World, AStaticMeshActor::StaticClass(), FVector(0, 0, 100),
		ECC_WorldStatic, Blocks, FVector(12, 100, 100));
	const FVector Requested(27, 0, 100);
	const FVector Fallback(600, 0, 100);
	World->SpawnActor<AWoTPathNode>(Fallback, FRotator::ZeroRotator,
		TraceParitySpawnParams());
	AWoTSeal* Seal = World->SpawnActor<AWoTSeal>(
		FVector(0, 0, 300), FRotator::ZeroRotator, TraceParitySpawnParams());
	WoTTest::TickWorld(World, 1);
	Seal->DropFrom(Requested);
	TestEqual(TEXT("a falling seal is solid to pawns like Inventory.DropFrom"),
		Seal->Collision->GetCollisionResponseToChannel(ECC_Pawn), ECR_Block);
	TestEqual(TEXT("a falling seal is solid to dynamic actors"),
		Seal->Collision->GetCollisionResponseToChannel(ECC_WorldDynamic), ECR_Block);
	TestTrue(TEXT("FindSpot nudges a shallow wall penetration nearby"),
		Seal->GetActorLocation().X > Requested.X
			&& FVector::Dist(Seal->GetActorLocation(), Requested) < 60.0f);
	TestTrue(TEXT("adjustable penetration does not use the path-node fallback"),
		FVector::Dist(Seal->GetActorLocation(), Requested)
			< FVector::Dist(Seal->GetActorLocation(), Fallback));

	// Exercise movement, not just response configuration: Seal.DropFrom's retail
	// SetCollision(true,true,true) makes a falling seal land on a blocking pawn.
	AWoTCharacter* PawnFloor = World->SpawnActor<AWoTCharacter>(
		FVector(1000, 0, 100), FRotator::ZeroRotator,
		TraceParitySpawnParams());
	PawnFloor->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	AWoTSeal* PawnFall = World->SpawnActor<AWoTSeal>(
		FVector(1000, 0, 300), FRotator::ZeroRotator,
		TraceParitySpawnParams());
	PawnFall->DropFrom(FVector(1000, 0, 300));
	WoTTest::TickWorld(World, 0.8f);
	TestFalse(TEXT("a real falling Seal stops on the pawn"), PawnFall->IsFalling());
	float PawnRadius = 0.0f;
	float PawnHalfHeight = 0.0f;
	PawnFloor->GetSimpleCollisionCylinder(PawnRadius, PawnHalfHeight);
	const float ExpectedPawnTop = PawnFloor->GetActorLocation().Z
		+ PawnHalfHeight + 3.0f;
	TestTrue(TEXT("the Seal lands on the pawn rather than passing through"),
		FMath::IsNearlyEqual(PawnFall->GetActorLocation().Z, ExpectedPawnTop, 3.0f));

	// Retail Projectile defaults collide/touch but do not block actors
	// (bBlockActors/bBlockPlayers are false), so a Seal must not invent a solid
	// landing on one.
	AWoTDartProjectile* Projectile = World->SpawnActor<AWoTDartProjectile>(
		FVector(1300, 0, 180), FRotator::ZeroRotator,
		TraceParitySpawnParams());
	Projectile->ProjectileMovement->StopMovementImmediately();
	Projectile->ProjectileMovement->Deactivate();
	AWoTSeal* ProjectileFall = World->SpawnActor<AWoTSeal>(
		FVector(1300, 0, 300), FRotator::ZeroRotator,
		TraceParitySpawnParams());
	ProjectileFall->DropFrom(FVector(1300, 0, 300));
	WoTTest::TickWorld(World, 0.7f);
	TestTrue(TEXT("falling Seal preserves retail projectile pass-through"),
		ProjectileFall->IsFalling()
			&& ProjectileFall->GetActorLocation().Z
				< Projectile->GetActorLocation().Z - 20.0f);

	// The live inventory path starts the flat seal exactly one pawn radius in
	// front of its carrier, so the two cylinders initially overlap laterally.
	// Its downward motion is not deeper into that overlap and must reach the floor.
	const FVector CarrierLocation(1700.0f, 0.0f, 100.0f);
	TraceParityBox(World, AStaticMeshActor::StaticClass(),
		FVector(1700.0f, 0.0f, -20.0f), ECC_WorldStatic, Blocks,
		FVector(300.0f, 300.0f, 20.0f));
	AWoTCharacter* Carrier = World->SpawnActor<AWoTCharacter>(
		CarrierLocation, FRotator::ZeroRotator, TraceParitySpawnParams());
	AWoTPlayerController* CarrierController = World->SpawnActor<AWoTPlayerController>(
		FVector::ZeroVector, FRotator::ZeroRotator, TraceParitySpawnParams());
	CarrierController->Possess(Carrier);
	Carrier->Health = 100;
	Carrier->bDead = false;
	Carrier->EnsureAngrealHands();
	Carrier->CurrentHandSet = Carrier->AngrealHandSet;
	Carrier->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	AWoTSeal* CarriedSeal = World->SpawnActor<AWoTSeal>(
		CarrierLocation, FRotator::ZeroRotator, TraceParitySpawnParams());
	CarriedSeal->GiveTo(Carrier);
	TestTrue(TEXT("fixture gives the real Seal inventory to its carrier"),
		CarriedSeal->GetOwner() == Carrier);
	TestTrue(TEXT("carrier inventory owns the Seal before the drop endpoint"),
		Carrier->InventoryComponent->Items.Contains(CarriedSeal));
	TestEqual(TEXT("the carried Seal is the selected artifact"),
		Carrier->InventoryComponent->SelectedItem.Get(),
		static_cast<AWoTInventory*>(CarriedSeal));
	TestFalse(TEXT("live carrier is not dead"), Carrier->bDead);
	TestTrue(TEXT("live carrier has authority"), Carrier->HasAuthority());
	TestTrue(TEXT("live carrier uses its artifact hand set"),
		Carrier->CurrentHandSet == Carrier->AngrealHandSet);
	TestFalse(TEXT("live carrier is not in the citadel editor"),
		CarrierController->IsOwnerEditingCitadel());
	TestTrue(TEXT("selected Seal remains a valid live actor"), IsValid(CarriedSeal));
	TestTrue(TEXT("selected Seal is not the inherent default artifact"),
		CarriedSeal->GetClass() != Carrier->DefaultAngrealInventory);
	TestFalse(TEXT("selected Seal is not the special non-droppable artifact"),
		CarriedSeal->IsA<AWoTAngrealInvSpecial>());
	// Drive the ordinary authoritative Drop endpoint with a selected carried Seal.
	// The all-seals command converges on this same DropSelectedInventory path.
	Carrier->ServerDrop_Implementation();
	TestNull(TEXT("real drop endpoint removes the carrier owner"), CarriedSeal->GetOwner());
	WoTTest::TickWorld(World, 1.0f);
	TestFalse(TEXT("carrier-overlapping flat Seal reaches the floor"),
		CarriedSeal->IsFalling());
	TestTrue(TEXT("carrier-overlapping drop did not hang at carrier height"),
		CarriedSeal->GetActorLocation().Z < CarrierLocation.Z - 40.0f);

	const FVector BlockedRequest(200, 300, 100);
	const FVector BlockedFallback(700, 300, 100);
	TraceParityBox(World, AStaticMeshActor::StaticClass(), BlockedRequest,
		ECC_WorldStatic, Blocks, FVector(80));
	World->SpawnActor<AWoTPathNode>(BlockedFallback, FRotator::ZeroRotator,
		TraceParitySpawnParams());
	AWoTSeal* BlockedSeal = World->SpawnActor<AWoTSeal>(
		FVector(0, 300, 300), FRotator::ZeroRotator, TraceParitySpawnParams());
	WoTTest::TickWorld(World, 1);
	BlockedSeal->DropFrom(BlockedRequest);
	TestTrue(TEXT("a genuinely blocked spot falls back to a path node"),
		FVector::Dist(BlockedSeal->GetActorLocation(), BlockedFallback)
			< FVector::Dist(BlockedSeal->GetActorLocation(), BlockedRequest));
	BlockedSeal->Landed(FVector::UpVector);
	TestEqual(TEXT("a landed seal returns to touch-only pickup collision"),
		BlockedSeal->Collision->GetCollisionResponseToChannel(ECC_Pawn), ECR_Overlap);
	WoTTest::DestroySpecWorld(World);
	return true;
}

#endif
