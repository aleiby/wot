// G42: current-pose CPU vertices and retail Mashadar bParticles rendering.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../Projectiles/WoTMashadarSegment.h"
#include "../Projectiles/WoTMashadarGuide.h"
#include "../WoTFractalData.h"
#include "../WoTFractalSubsystem.h"
#include "../WoTVertexAnimComponent.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"

namespace
{
	AWoTMashadarSegment* G42SpawnSegment(UWorld* World, const bool bHead,
		const FVector& Location)
	{
		AWoTMashadarSegment* Segment =
			World->SpawnActorDeferred<AWoTMashadarSegment>(
				AWoTMashadarSegment::StaticClass(),
				FTransform(FRotator::ZeroRotator, Location),
				nullptr, nullptr,
				ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (Segment)
		{
			Segment->bIsHead = bHead;
			Segment->FinishSpawning(
				FTransform(FRotator::ZeroRotator, Location));
		}
		return Segment;
	}

	bool G42QuatsEqual(const FQuat& A, const FQuat& B)
	{
		return FMath::Abs(A | B) >= 1.0f - KINDA_SMALL_NUMBER;
	}
}

#define G42_TEST(ClassName, Path) \
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(ClassName, Path, \
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

G42_TEST(FWoTG42CurrentPoseVerticesSpec, "WoT.Mashadar.CurrentPoseVertices")
bool FWoTG42CurrentPoseVerticesSpec::RunTest(const FString&)
{
	UWoTVertexAnimComponent* Component =
		NewObject<UWoTVertexAnimComponent>(GetTransientPackage());
	const TArray<FVector3f> Base = {
		FVector3f(1.0f, 2.0f, 3.0f),
		FVector3f(-2.0f, 4.0f, 8.0f)};
	TMap<int32, TArray<FVector3f>> Deltas;
	Deltas.Add(3, {
		FVector3f(8.0f, 0.0f, 4.0f),
		FVector3f(0.0f, 12.0f, -4.0f)});
	Deltas.Add(4, {
		FVector3f(-4.0f, 4.0f, 0.0f),
		FVector3f(8.0f, 0.0f, 4.0f)});
	// Publish playback first, then build geometry. The FIRST pose read must keep
	// these live weights across the lazy geometry-cache initialization.
	Component->SetFrameMorphsForTesting(3, 0.25f, 4, 0.75f);
	Component->SetPoseCacheForTesting(Base, Deltas);

	TArray<FVector> Pose;
	if (!TestTrue(TEXT("fixture current pose resolves"),
		Component->GetCurrentPoseVertexPositions(Pose))
		|| !TestEqual(TEXT("fixture vertex count"), Pose.Num(), 2))
	{
		return false;
	}
	TestTrue(TEXT("vertex zero combines both active morph frames"),
		Pose[0].Equals(FVector(0.0, 5.0, 4.0), KINDA_SMALL_NUMBER));
	TestTrue(TEXT("vertex one combines both active morph frames"),
		Pose[1].Equals(FVector(4.0, 7.0, 10.0), KINDA_SMALL_NUMBER));

	// Adjacent samples can collapse at a one-frame sequence edge. The engine
	// morph seam is last-write-wins, so the CPU pose must follow WeightB.
	Component->SetFrameMorphsForTesting(3, 0.2f, 3, 0.8f);
	Component->GetCurrentPoseVertexPositions(Pose);
	TestTrue(TEXT("duplicate frame uses the second published weight"),
		Pose[0].Equals(FVector(7.4, 2.0, 6.2), KINDA_SMALL_NUMBER));

	const TArray<FVector3f> CornerSplitBase = {
		FVector3f(1.0f, 2.0f, 3.0f),
		FVector3f(1.0f, 2.0f, 3.0f),
		FVector3f(1.0f, 2.0f, 3.0f)};
	TMap<int32, TArray<FVector3f>> CornerSplitDeltas;
	CornerSplitDeltas.Add(3, {
		FVector3f(4.0f, 5.0f, 6.0f),
		FVector3f(4.0f, 5.0f, 6.0f),
		FVector3f(4.0f, 5.0f, 7.0f)});
	Component->SetFrameMorphsForTesting(3, 1.0f, INDEX_NONE, 0.0f);
	Component->SetPoseCacheForTesting(CornerSplitBase, CornerSplitDeltas);
	Component->GetCurrentPoseVertexPositions(Pose);
	TestEqual(TEXT("identical base plus every-frame delta collapses corner splits"),
		Pose.Num(), 2);
	return true;
}

G42_TEST(FWoTG42HeadBodyQuadPoseSpec, "WoT.Mashadar.HeadBodyQuadPose")
bool FWoTG42HeadBodyQuadPoseSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTMashadarSegment* Head =
		G42SpawnSegment(World, true, FVector(100.0, 0.0, 50.0));
	AWoTMashadarSegment* Body =
		G42SpawnSegment(World, false, FVector(-100.0, 0.0, 50.0));
	if (!TestNotNull(TEXT("head segment spawned"), Head)
		|| !TestNotNull(TEXT("body segment spawned"), Body))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	WoTTest::TickWorld(World, 1);

	AWoTMashadarSegment* Segments[] = {Head, Body};
	const float DrawScales[] = {0.2f, 0.3f};
	const int32 ExpectedDeduplicatedCounts[] = {16, 15};
	const int32 ExpectedParticleCounts[] = {17, 15};
	for (int32 CaseIndex = 0; CaseIndex < UE_ARRAY_COUNT(Segments); ++CaseIndex)
	{
		AWoTMashadarSegment* Segment = Segments[CaseIndex];
		UWoTVertexAnimComponent* Mesh = Segment->DisplayMesh;
		UInstancedStaticMeshComponent* Quads = Segment->ParticleQuads;
		TArray<FVector> Pose;
		if (!TestNotNull(TEXT("hidden animation driver exists"), Mesh)
			|| !TestNotNull(TEXT("particle quad pool exists"), Quads)
			|| !TestTrue(TEXT("current mesh pose is CPU-readable"),
				Mesh->GetCurrentPoseVertexPositions(Pose)))
		{
			continue;
		}
		TestFalse(TEXT("solid morph mesh is not rendered"), Mesh->IsVisible());
		TestTrue(TEXT("hidden morph driver keeps ticking"),
			Mesh->IsComponentTickEnabled());
		TestEqual(TEXT("pose API exposes the pinned deduplicated vertex count"),
			Pose.Num(), ExpectedDeduplicatedCounts[CaseIndex]);
		TestEqual(TEXT("quad pool restores the complete retail mote count"),
			Quads->GetInstanceCount(), ExpectedParticleCounts[CaseIndex]);
		if (!TestTrue(TEXT("bound Mashadar mesh has LOD0 vertices"),
			!Pose.IsEmpty()))
		{
			continue;
		}
		FTransform QuadTransform;
		if (TestTrue(TEXT("first quad transform resolves"),
			Quads->GetInstanceTransform(0, QuadTransform, true)))
		{
			const FIntPoint Dims = Segment->GetParticleTextureSize();
			const FVector ExpectedScale(
				DrawScales[CaseIndex] * static_cast<float>(Dims.X) / 100.0f,
				DrawScales[CaseIndex] * static_cast<float>(Dims.Y) / 100.0f,
				1.0f);
			TestTrue(TEXT("quad scale follows DrawScale times texture dimensions"),
				QuadTransform.GetScale3D().Equals(
					ExpectedScale, KINDA_SMALL_NUMBER));
			APlayerCameraManager* Camera =
				UGameplayStatics::GetPlayerCameraManager(Segment, 0);
			const FQuat ExpectedFace = Camera
				? FRotationMatrix::MakeFromZ(
					-Camera->GetCameraRotation().Vector()).ToQuat()
				: FQuat::Identity;
			TestTrue(TEXT("quad uses the sprayer camera-facing quaternion"),
				G42QuatsEqual(
					QuadTransform.GetRotation(), ExpectedFace));
			TestTrue(TEXT("quad center follows the component-space current vertex"),
				QuadTransform.GetLocation().Equals(
					Mesh->GetComponentTransform().TransformPosition(Pose[0]),
					KINDA_SMALL_NUMBER));
		}
	}
	WoTTest::DestroySpecWorld(World);
	return true;
}

G42_TEST(FWoTG42GuideHeadLifecycleSpec, "WoT.Mashadar.GuideHeadLifecycle")
bool FWoTG42GuideHeadLifecycleSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTMashadarGuide* Guide = World->SpawnActor<AWoTMashadarGuide>(
		FVector::ZeroVector, FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("guide spawned"), Guide))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	WoTTest::TickWorld(World, 2);
	AWoTMashadarSegment* Head = Guide->GetMasterSegment();
	if (!TestNotNull(TEXT("guide built its head"), Head))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	TestTrue(TEXT("resting guide starts with its head cloud hidden"),
		Head->IsHidden());

	// The lifecycle is keyed to non-null destination acquisition; no target
	// behavior is under test, so the guide itself is a stable sentinel actor.
	Guide->SetDestination(Guide);
	TestFalse(TEXT("destination acquire reveals the guide-driven head cloud"),
		Head->IsHidden());
	TestFalse(TEXT("solid head display remains never-rendered"),
		Head->DisplayMesh->IsVisible());
	TestTrue(TEXT("head quad component is the visible presentation"),
		Head->ParticleQuads->IsVisible());
	TestEqual(TEXT("guide-driven head uses the pinned MashTip quad count"),
		Head->ParticleQuads->GetInstanceCount(), 17);

	Guide->SetDestination(nullptr);
	WoTTest::TickWorld(World, 30, 1.0f / 60.0f);
	TestTrue(TEXT("return-home reset hides the head cloud again"),
		Head->IsHidden());
	WoTTest::DestroySpecWorld(World);
	return true;
}

G42_TEST(FWoTG42LiveMyTex2Spec, "WoT.Mashadar.LiveMyTex2")
bool FWoTG42LiveMyTex2Spec::RunTest(const FString&)
{
	UWoTFractalCatalog* Catalog = LoadObject<UWoTFractalCatalog>(
		nullptr,
		TEXT("/Game/WoT/Data/DA_WoTFractalCatalog.DA_WoTFractalCatalog"));
	if (!TestNotNull(TEXT("fractal catalog loads"), Catalog))
	{
		return false;
	}
	TestNotNull(TEXT("catalog carries WoTPawns.Mashadar.MyTex2"),
		Catalog->Textures.FindByPredicate(
			[](const FWoTFractalTextureRecord& Record)
			{
				return Record.QualifiedName()
					== TEXT("WoTPawns.Mashadar.MyTex2");
			}));
	UTexture2D* Imported = LoadObject<UTexture2D>(
		nullptr,
		TEXT("/Game/WoT/Textures/WoTPawns/MyTex2.MyTex2"));
	if (!TestNotNull(TEXT("representative imported MyTex2 asset"), Imported))
	{
		return false;
	}

	UWorld* World = WoTTest::CreateSpecWorld();
	UWoTFractalSubsystem* Subsystem =
		World->GetSubsystem<UWoTFractalSubsystem>();
	if (!TestNotNull(TEXT("fractal subsystem exists"), Subsystem))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Subsystem->SetRenderGateBypassForTesting(true);
	AWoTMashadarSegment* Segment =
		G42SpawnSegment(World, false, FVector::ZeroVector);
	UMaterialInstanceDynamic* MID =
		Segment ? Segment->GetParticleMaterial() : nullptr;
	if (!TestNotNull(TEXT("segment particle MID exists"), MID))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	UTexture* Bound = nullptr;
	MID->GetTextureParameterValue(
		FHashedMaterialParameterInfo(FName(TEXT("Tex"))), Bound);
	TestTrue(TEXT("MID Tex starts on the imported MyTex2 asset"),
		Bound == Imported);

	// HandleActorSpawned queued the segment; the subsystem owns the next-tick
	// in-place mutation from imported Tex to the current live wet simulation.
	Subsystem->Tick(0.05f);
	UTexture2D* Live =
		Subsystem->FindLiveTexture(TEXT("WoTPawns.Mashadar.MyTex2"));
	TestNotNull(TEXT("live MyTex2 simulation exists after subsystem tick"), Live);
	Bound = nullptr;
	MID->GetTextureParameterValue(
		FHashedMaterialParameterInfo(FName(TEXT("Tex"))), Bound);
	TestTrue(TEXT("segment MID Tex becomes WoTLive_MyTex2"),
		Bound == Live && Bound
			&& Bound->GetFName().ToString().StartsWith(TEXT("WoTLive_")));
	WoTTest::DestroySpecWorld(World);
	return true;
}

G42_TEST(FWoTG42RetailFadeColorSpec, "WoT.Mashadar.RetailFadeColor")
bool FWoTG42RetailFadeColorSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTMashadarSegment* Segment =
		G42SpawnSegment(World, false, FVector::ZeroVector);
	if (!TestNotNull(TEXT("fade segment spawned"), Segment))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	WoTTest::TickWorld(World, 40, 1.0f / 30.0f);
	const float ScaleGlow = Segment->GetCurrentScaleGlow();
	TestTrue(TEXT("FadeIn settles in the authored 0.2-0.4 window"),
		ScaleGlow >= 0.2f && ScaleGlow <= 0.4f);
	const float Expected =
		FMath::Clamp(FMath::Clamp(ScaleGlow * 0.5f, 0.0f, 1.0f)
			* ScaleGlow, 0.0f, 1.0f);
	TestEqual(TEXT("retail color helper double-applies ScaleGlow"),
		AWoTMashadarSegment::ComputeRetailParticleColor(ScaleGlow),
		Expected);
	if (Segment->ParticleQuads
		&& TestTrue(TEXT("fade segment has live quads"),
			Segment->ParticleQuads->GetInstanceCount() > 0)
		&& TestTrue(TEXT("quad pool carries one custom-data float"),
			Segment->ParticleQuads->PerInstanceSMCustomData.Num() > 0))
	{
		TestEqual(TEXT("custom data zero carries retail color scalar"),
			Segment->ParticleQuads->PerInstanceSMCustomData[0], Expected);
	}
	WoTTest::DestroySpecWorld(World);
	return true;
}

#undef G42_TEST

#endif
