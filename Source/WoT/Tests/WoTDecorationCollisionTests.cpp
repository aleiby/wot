#include "Misc/AutomationTest.h"

#include "WoTTestWorld.h"
#include "../AI/WoTPathNodeIterator.h"
#include "../WoTDecoration.h"

#include "Engine/World.h"

#if WITH_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTDecorationRetailCollisionSpec,
	"WoT.World.Decoration.RetailCylinderCollision",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTDecorationRetailCollisionSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	auto PawnSweep = [World]()
	{
		FHitResult Hit;
		return World->SweepSingleByChannel(Hit, FVector(-100, 0, 0),
			FVector(100, 0, 0), FQuat::Identity, ECC_Pawn,
			FCollisionShape::MakeSphere(10.0f));
	};

	AWoTDecoration* Blocking = World->SpawnActor<AWoTDecoration>();
	Blocking->ConfigureRuntimeCollision(true, true, true, 22.0f, 22.0f);
	TestTrue(TEXT("blocking decoration stops a pawn sweep"), PawnSweep());
	TestEqual(TEXT("blocking decoration overlaps the projectile channel for Touch"),
		Blocking->Collision->GetCollisionResponseToChannel(ECC_GameTraceChannel1),
		ECR_Overlap);
	TestTrue(TEXT("blocking decoration is a retail projectile Touch target"),
		Blocking->IsRetailProjectileTouchTarget());
	FHitResult VisibilityHit;
	TestFalse(TEXT("decoration never participates in VisBlocking"),
		WoTVisBlockingTrace(World, FVector(-100,0,0), FVector(100,0,0),
			nullptr, VisibilityHit));
	Blocking->Destroy();

	AWoTDecoration* NonBlocking = World->SpawnActor<AWoTDecoration>();
	NonBlocking->ConfigureRuntimeCollision(false, true, true, 22.0f, 22.0f);
	TestFalse(TEXT("bCollideActors false permits pawn movement"), PawnSweep());
	NonBlocking->Destroy();

	AWoTDecoration* Override = World->SpawnActor<AWoTDecoration>();
	Override->ConfigureRuntimeCollision(true, false, false, 64.0f, 48.0f);
	TestEqual(TEXT("instance radius override wins"),
		Override->Collision->GetUnscaledCapsuleRadius(), 64.0f);
	TestEqual(TEXT("instance height override wins"),
		Override->Collision->GetUnscaledCapsuleHalfHeight(), 48.0f);
	TestFalse(TEXT("instance blocking overrides permit pawn movement"), PawnSweep());
	TestFalse(TEXT("non-target non-blocking decoration rejects projectile Touch"),
		Override->IsRetailProjectileTouchTarget());

	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

#endif
