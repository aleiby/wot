// Specs for the RainCyd rain column (Environment/RainCyd.uc): the master
// ground-trace anchor (uc:90-92), the StackNum stacked segments (uc:94-113),
// and the literal per-segment, one-jump wrap (uc:130-133).
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../WoTRainCyd.h"
#include "../WoTCollisionBlocker.h"

#include "Components/BoxComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"

#include <limits>

namespace
{
	// Level geometry stand-in: RainCyd's ground trace is TRACE_VisBlocking
	// (level + movers), and imported BSP is a StaticMeshActor in editor maps.
	AActor* SpawnRainFloor(UWorld* World, const float TopZ)
	{
		AStaticMeshActor* Floor = World->SpawnActor<AStaticMeshActor>();
		UBoxComponent* Box = NewObject<UBoxComponent>(Floor);
		Box->SetBoxExtent(FVector(1000.0f, 1000.0f, 10.0f));
		Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Box->SetCollisionObjectType(ECC_WorldStatic);
		Box->SetCollisionResponseToAllChannels(ECR_Block);
		Box->SetupAttachment(Floor->GetRootComponent());
		Box->SetWorldLocation(FVector(0, 0, TopZ - 10.0f));
		Box->RegisterComponent();
		return Floor;
	}

	AWoTRainCyd* SpawnRainCyd(UWorld* World, const FVector& Location,
		const int32 StackNum, const float Scale, const float BuryDistance)
	{
		const FTransform Transform(
			FRotator::ZeroRotator, Location, FVector(Scale));
		AWoTRainCyd* Rain = World->SpawnActorDeferred<AWoTRainCyd>(
			AWoTRainCyd::StaticClass(), Transform);
		Rain->StackNum = StackNum;
		Rain->TraceDepth = 5000.0f;
		Rain->BuryDistance = BuryDistance;
		// pin the authored-random fall rate so ticking is exact
		Rain->MinFallRate = 600.0f;
		Rain->MaxFallRate = 600.0f;
		Rain->FinishSpawning(Transform);
		return Rain;
	}

	AWoTRainCyd* SpawnFrontEndRainCyd(UWorld* World, const FVector& Location,
		const FRotator& Rotation, const FVector& Hit, const int32 StackNum,
		const float Scale, const float BuryDistance, bool& bHitAccepted)
	{
		const FTransform Transform(Rotation, Location, FVector(Scale));
		AWoTRainCyd* Rain = World->SpawnActorDeferred<AWoTRainCyd>(
			AWoTRainCyd::StaticClass(), Transform);
		Rain->StackNum = StackNum;
		Rain->TraceDepth = 5000.0f;
		Rain->BuryDistance = BuryDistance;
		Rain->MinFallRate = 600.0f;
		Rain->MaxFallRate = 600.0f;
		Rain->SetFrontEndStagedMotion(0.0f, 600.0f);
		bHitAccepted = Rain->SetFrontEndGroundHit(Hit);
		Rain->FinishSpawning(Transform);
		return Rain;
	}

	void TestSegmentScalars(FAutomationTestBase& Test, AWoTRainCyd& Rain,
		const FVector& Hit, const FVector& Down,
		TConstArrayView<double> ExpectedScalars, const TCHAR* Phase)
	{
		TArray<FVector> Actual;
		Actual.Add(Rain.GetActorLocation());
		for (USkeletalMeshComponent* Segment : Rain.ExtraSegments)
			Actual.Add(Segment->GetComponentLocation());
		Test.TestEqual(*FString::Printf(TEXT("%s segment count"), Phase),
			Actual.Num(), ExpectedScalars.Num());
		for (int32 Index = 0;
			Index < Actual.Num() && Index < ExpectedScalars.Num(); ++Index)
		{
			const FVector Expected = Hit + Down * ExpectedScalars[Index];
			Test.TestTrue(*FString::Printf(TEXT("%s segment %d literal position"),
				Phase, Index), Actual[Index].Equals(Expected, 0.02));
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRainCydStackTest,
	"WoT.Environment.RainCyd.StackAnchor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTRainCydStackTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	SpawnRainFloor(World, 10.0f);
	// DrawScale 2 -> segment height 96; authored high above the floor.
	AWoTRainCyd* Rain = SpawnRainCyd(World, FVector(0, 0, 500), 3, 2.0f, 0.0f);

	// uc:92 - the master anchors one segment height above the traced hit.
	TestEqual(TEXT("master anchor = hit + CylinderHeight*scale"),
		float(Rain->GetActorLocation().Z), 106.0f, 0.5f);
	// uc:94-113 - StackNum-1 stacked clones, one segment height apart.
	TestEqual(TEXT("extra segments"), Rain->ExtraSegments.Num(), 2);
	if (Rain->ExtraSegments.Num() == 2)
	{
		TestEqual(TEXT("segment 1 stacked one height above"),
			float(Rain->ExtraSegments[0]->GetComponentLocation().Z),
			202.0f, 0.5f);
		TestEqual(TEXT("segment 2 stacked two heights above"),
			float(Rain->ExtraSegments[1]->GetComponentLocation().Z),
			298.0f, 0.5f);
	}

	// The column falls at the pinned rate and wraps every segment height:
	// 6 ticks at 1/60 = 0.1s -> 60uu down.
	WoTTest::TickWorld(World, 6);
	TestEqual(TEXT("column fell 60uu"),
		float(Rain->GetActorLocation().Z), 46.0f, 0.5f);
	// 0.2s total = 120uu -> wraps past 96 to 24uu below the anchor.
	WoTTest::TickWorld(World, 6);
	TestEqual(TEXT("column wrapped seamlessly"),
		float(Rain->GetActorLocation().Z), 82.0f, 0.5f);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRainCydNoGroundTest,
	"WoT.Environment.RainCyd.NoGroundFallback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTRainCydNoGroundTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	// No floor within TraceDepth: the authored anchor is kept (retail's
	// unhit Trace is a degenerate case no shipped placement exercises).
	AWoTRainCyd* Rain = SpawnRainCyd(World, FVector(0, 0, 500), 2, 1.0f, 0.0f);
	TestEqual(TEXT("authored anchor kept"),
		float(Rain->GetActorLocation().Z), 500.0f, 0.5f);
	TestEqual(TEXT("stack still built"), Rain->ExtraSegments.Num(), 1);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRainCydStagedGroundTest,
	"WoT.Environment.RainCyd.StagedGroundAnchor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTRainCydStagedGroundTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	const FTransform Transform(
		FRotator::ZeroRotator, FVector(0, 0, 500), FVector(6.0f));
	AWoTRainCyd* Rain = World->SpawnActorDeferred<AWoTRainCyd>(
		AWoTRainCyd::StaticClass(), Transform);
	Rain->StackNum = 1;
	Rain->TraceDepth = 5000.0f;
	Rain->BuryDistance = -128.0f;
	Rain->MinFallRate = 600.0f;
	Rain->MaxFallRate = 600.0f;
	Rain->SetFrontEndStagedMotion(0.0f, 600.0f);
	const double NaN = std::numeric_limits<double>::quiet_NaN();
	TestFalse(TEXT("nonfinite staged hit is rejected"),
		Rain->SetFrontEndGroundHit(FVector(NaN, 0, 10)));
	TestTrue(TEXT("finite staged hit is accepted before BeginPlay"),
		Rain->SetFrontEndGroundHit(FVector(0, 0, 10)));
	Rain->FinishSpawning(Transform);
	// Hit - Down*(48*6) + Down*(-128) = Z 426. No collider exists.
	TestEqual(TEXT("negative bury anchors above staged floor"),
		Rain->GetActorLocation().Z, 426.0, 0.001);
	TestFalse(TEXT("running actor rejects replacement staged hit"),
		Rain->SetFrontEndGroundHit(FVector(0, 0, 200)));
	TestEqual(TEXT("rejected replacement leaves anchor unchanged"),
		Rain->GetActorLocation().Z, 426.0, 0.001);
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRainCydFrontEndMotionTest,
	"WoT.Environment.RainCyd.FrontEndPerSegmentMotion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTRainCydFrontEndMotionTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	const FVector Hit(17.0, -23.0, 31.0);
	bool bHitAccepted = false;
	AWoTRainCyd* Rain = SpawnFrontEndRainCyd(World, FVector(50, 60, 500),
		FRotator(31.0, 17.0, 0.0), Hit, 4, 2.0f, -32.0f, bHitAccepted);
	TestTrue(TEXT("tilted front-end hit is staged"), bHitAccepted);
	const FVector Down = -Rain->GetActorUpVector();
	const double Initial[] = {-128.0, -224.0, -320.0, -416.0};
	TestSegmentScalars(*this, *Rain, Hit, Down, Initial, TEXT("initial"));

	// The literal RainCyd.uc update moves every label, then permits exactly
	// one StackNum*CylinderHeight jump for that label during this Tick.
	Rain->Tick(0.2f); // +120uu; only segment 0 crosses BuryDistance.
	const double FirstWrap[] = {-392.0, -104.0, -200.0, -296.0};
	TestSegmentScalars(*this, *Rain, Hit, Down, FirstWrap, TEXT("first wrap"));
	Rain->Tick(0.64f); // +384uu; every label jumps once.
	const double SecondWrap[] = {-392.0, -104.0, -200.0, -296.0};
	TestSegmentScalars(*this, *Rain, Hit, Down, SecondWrap, TEXT("second wrap"));
	Rain->Tick(2.0f); // +1200uu; retail still performs only one 384uu jump.
	const double LargeDelta[] = {424.0, 712.0, 616.0, 520.0};
	TestSegmentScalars(*this, *Rain, Hit, Down, LargeDelta, TEXT("large delta"));

	bool bSingleAccepted = false;
	AWoTRainCyd* Single = SpawnFrontEndRainCyd(World, FVector(0, 0, 500),
		FRotator::ZeroRotator, FVector(0, 0, 10), 1, 6.0f, -128.0f,
		bSingleAccepted);
	TestTrue(TEXT("single-segment front-end hit is staged"), bSingleAccepted);
	const FVector SingleDown = -Single->GetActorUpVector();
	const double SingleInitial[] = {-416.0};
	TestSegmentScalars(*this, *Single, FVector(0, 0, 10), SingleDown,
		SingleInitial, TEXT("scale six initial"));
	Single->Tick(0.2f);
	const double SingleNoWrap[] = {-296.0};
	TestSegmentScalars(*this, *Single, FVector(0, 0, 10), SingleDown,
		SingleNoWrap, TEXT("scale six falling"));
	Single->Tick(0.4f);
	const double SingleFirstWrap[] = {-344.0};
	TestSegmentScalars(*this, *Single, FVector(0, 0, 10), SingleDown,
		SingleFirstWrap, TEXT("scale six first wrap"));
	Single->Tick(0.5f);
	const double SingleSecondWrap[] = {-332.0};
	TestSegmentScalars(*this, *Single, FVector(0, 0, 10), SingleDown,
		SingleSecondWrap, TEXT("scale six second wrap"));
	Single->Tick(2.0f);
	const double SingleLargeDelta[] = {580.0};
	TestSegmentScalars(*this, *Single, FVector(0, 0, 10), SingleDown,
		SingleLargeDelta, TEXT("scale six large delta"));

	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRainCydRuntimeMaterialTest,
	"WoT.Environment.RainCyd.RuntimeMaterialsPerSegment",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTRainCydRuntimeMaterialTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	const FTransform Transform(
		FRotator::ZeroRotator, FVector(0, 0, 500), FVector::OneVector);
	AWoTRainCyd* Rain = World->SpawnActorDeferred<AWoTRainCyd>(
		AWoTRainCyd::StaticClass(), Transform);
	Rain->StackNum = 4;
	Rain->MinFallRate = Rain->MaxFallRate = 600.0f;
	UStaticMesh* Carrier = NewObject<UStaticMesh>(Rain);
	UMaterial* Material = NewObject<UMaterial>(Rain);
	FStaticMaterial Slot;
	Slot.MaterialInterface = Material;
	Slot.MaterialSlotName = TEXT("Rain");
#if WITH_EDITORONLY_DATA
	Slot.ImportedMaterialSlotName = Slot.MaterialSlotName;
#endif
	Carrier->GetStaticMaterials().Add(Slot);
	TArray<UMaterialInterface*> Materials = {Material};
	Rain->SetRuntimeRenderMesh(Carrier, Materials);
	Rain->FinishSpawning(Transform);

	TInlineComponentArray<UStaticMeshComponent*> Segments;
	Rain->GetComponents(Segments);
	Segments.RemoveAll([Carrier](const UStaticMeshComponent* Segment)
	{
		return Segment == nullptr || Segment->GetStaticMesh() != Carrier;
	});
	TestEqual(TEXT("runtime renderer exists for every stacked segment"),
		Segments.Num(), 4);
	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		TestTrue(*FString::Printf(TEXT("runtime segment %d inherits actor material"),
			Index), Segments[Index]->GetMaterial(0) == Material);
	}
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

// Retail rain draws at UE1 GUnlitColor: the placed Arena_01
// columns (ScaleGlow 0.1, AmbientGlow 0) are 0.05 x texture. The mesh
// translucent master's MLAmbient is a VECTOR parameter; the guarded runtime
// used to write a scalar of that name (silently ignored -> 1.0, 20x retail).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRainCydUnlitColorTest,
	"WoT.Environment.RainCyd.UnlitColorIsVectorMLAmbient",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTRainCydUnlitColorTest::RunTest(const FString&)
{
	TestEqual(TEXT("placed rain 0.1 glow -> 0.05"),
		WoTRainCydUnlitColor(0.1f, 0), 0.05f, 1.0e-6f);
	TestEqual(TEXT("AmbientGlow adds /256"),
		WoTRainCydUnlitColor(0.0f, 128), 0.5f, 1.0e-6f);
	TestEqual(TEXT("clamped to one"),
		WoTRainCydUnlitColor(1.5f, 200), 1.0f, 1.0e-6f);
	UMaterialInterface* Master = Cast<UMaterialInterface>(FSoftObjectPath(
		TEXT("/Game/WoT/Materials/M_WoT_MeshTranslucent.M_WoT_MeshTranslucent")).TryLoad());
	if (!TestNotNull(TEXT("mesh translucent master loads"), Master))
		return false;
	UMaterialInstanceDynamic* Material =
		UMaterialInstanceDynamic::Create(Master, GetTransientPackage());
	WoTRainCydApplyUnlitColor(Material, 0.1f, 0);
	FLinearColor Ambient = FLinearColor::White;
	TestTrue(TEXT("MLAmbient is a vector parameter of the master"),
		Material->GetVectorParameterValue(
			FHashedMaterialParameterInfo(TEXT("MLAmbient")), Ambient));
	TestTrue(TEXT("MLAmbient carries GUnlitColor"),
		Ambient.Equals(FLinearColor(0.05f, 0.05f, 0.05f, 1.0f), 1.0e-6f));
	float Scalar = 0.0f;
	TestFalse(TEXT("no scalar MLAmbient exists to absorb a mistyped write"),
		Material->GetScalarParameterValue(
			FHashedMaterialParameterInfo(TEXT("MLAmbient")), Scalar));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRainCydPerSegmentCullTest,
	"WoT.Environment.RainCyd.PerSegmentVisibilityCull",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// RainCyd.uc:94-109 spawns each stack segment as its own RainCyd carrying the
// master's VisibilityRadius/Height, so each segment is culled from its own
// location. A viewer level with one segment sees that segment only, and the
// column is never hidden as a whole while any segment is in range.
bool FWoTRainCydPerSegmentCullTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	SpawnRainFloor(World, 10.0f);
	// Scale 2: segments at Z 106 / 202 / 298.
	AWoTRainCyd* Rain = SpawnRainCyd(World, FVector(0, 0, 500), 3, 2.0f, 0.0f);
	if (!TestEqual(TEXT("extra segments"), Rain->ExtraSegments.Num(), 2))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	Rain->CullSegmentsFrom(FVector(0, 0, 2000));
	TestFalse(TEXT("zero limits never cull a segment"),
		Rain->Mesh->bHiddenInGame || Rain->ExtraSegments[0]->bHiddenInGame
		|| Rain->ExtraSegments[1]->bHiddenInGame);

	Rain->VisibilityHeight = 60.0f;
	Rain->CullSegmentsFrom(FVector(0, 0, 110));
	TestFalse(TEXT("viewer level with the master sees it"),
		Rain->Mesh->bHiddenInGame);
	TestTrue(TEXT("segment 1 beyond the height limit is culled"),
		Rain->ExtraSegments[0]->bHiddenInGame);
	TestTrue(TEXT("segment 2 beyond the height limit is culled"),
		Rain->ExtraSegments[1]->bHiddenInGame);

	Rain->CullSegmentsFrom(FVector(0, 0, 300));
	TestTrue(TEXT("master far below the viewer is culled"),
		Rain->Mesh->bHiddenInGame);
	TestTrue(TEXT("segment 1 still beyond the limit"),
		Rain->ExtraSegments[0]->bHiddenInGame);
	TestFalse(TEXT("segment 2 level with the viewer is visible"),
		Rain->ExtraSegments[1]->bHiddenInGame);
	TestFalse(TEXT("the column is never hidden as a whole by the segment cull"),
		Rain->IsHidden());

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRainCydVisibilityGateTest,
	"WoT.Environment.RainCyd.RetailVisibilityGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWoTRainCydVisibilityGateTest::RunTest(const FString&)
{
	const FVector Rain(100.0f, 200.0f, 300.0f);
	TestTrue(TEXT("zero limits disable culling"),
		WoTRainCydVisibleFrom(Rain, FVector(10000.0f), 0.0f, 0.0f));
	TestTrue(TEXT("viewer inside both retail limits is visible"),
		WoTRainCydVisibleFrom(Rain, Rain + FVector(299.0f, 400.0f, 99.0f),
			500.0f, 100.0f));
	TestFalse(TEXT("horizontal radius boundary is excluded"),
		WoTRainCydVisibleFrom(Rain, Rain + FVector(300.0f, 400.0f, 0.0f),
			500.0f, 100.0f));
	TestFalse(TEXT("vertical height boundary is excluded"),
		WoTRainCydVisibleFrom(Rain, Rain + FVector(0.0f, 0.0f, 100.0f),
			500.0f, 100.0f));
	return true;
}


// RainCyd.uc:90 Trace(..., bTraceActors=False) is TRACE_VisBlocking (level +
// movers in UE1): a BlockPlayer volume between the column
// and the floor (Arena_01 BlockPlayer0 under RainCyd22/23) must not anchor it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRainCydIgnoresActorBlockersTest,
	"WoT.Environment.RainCyd.GroundTraceIgnoresActorBlockers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTRainCydIgnoresActorBlockersTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	SpawnRainFloor(World, 10.0f);
	const FTransform BlockerTransform(FRotator::ZeroRotator, FVector(0, 0, 300));
	AWoTCollisionBlocker* Blocker = World->SpawnActorDeferred<AWoTCollisionBlocker>(
		AWoTCollisionBlocker::StaticClass(), BlockerTransform);
	Blocker->Configure(EWoTCollisionBlockerMode::Players, 128.0f, 40.0f);
	Blocker->FinishSpawning(BlockerTransform);
	TestFalse(TEXT("an actor blocker is not VisBlocking"),
		WoTRainCydIsVisBlockingActor(Blocker));
	AWoTRainCyd* Rain = SpawnRainCyd(World, FVector(0, 0, 500), 3, 2.0f, 0.0f);
	// Floor top 10 -> master = 10 + 48*2 = 106 (the blocker top at 340 is skipped).
	TestEqual(TEXT("column anchors on the level floor, not the blocker"),
		float(Rain->GetActorLocation().Z), 106.0f, 0.5f);
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

#endif
