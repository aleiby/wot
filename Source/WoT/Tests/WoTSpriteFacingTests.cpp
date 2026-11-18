#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../WoTSpriteBillboard.h"
#include "../WoTParticleSprayer.h"
#include "../WoTBeamEffect.h"
#include "../WoTPlayerController.h"
#include "../Projectiles/WoTMashadarSegment.h"
#include "Camera/CameraActor.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"

namespace
{
const FRotator FacingViews[] = {
	FRotator::ZeroRotator, FRotator(0, 37, 0), FRotator(-60, 0, 0),
	FRotator(0, 0, 20), FRotator(89, 37, 20), FRotator(-89, 37, 20)};

bool CheckCorners(FAutomationTestBase& Test, const AActor* Owner,
	const FRotator& View)
{
	const FRotationMatrix Basis(View);
	const FVector Right = Basis.GetUnitAxis(EAxis::Y);
	const FVector Up = Basis.GetUnitAxis(EAxis::Z);
	int32 Checked = 0;
	TInlineComponentArray<UInstancedStaticMeshComponent*> Quads(Owner);
	for (const UInstancedStaticMeshComponent* Quad : Quads)
	{
		for (int32 Index = 0; Index < Quad->GetInstanceCount(); ++Index)
		{
			FTransform Instance;
			if (!Quad->GetInstanceTransform(Index, Instance, true)
				|| Instance.GetScale3D().X < 0.001) continue;
			// Engine Plane: (-50,-50)=UV(0,0), (+50,-50)=UV(1,0),
			// (-50,+50)=UV(0,1). Project relative to its screen center.
			const FVector Corners[] = {
				FVector(-50,-50,0), FVector(50,-50,0), FVector(-50,50,0)};
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				const FVector Offset = Instance.TransformVector(Corners[Corner]);
				const double ScreenX = FVector::DotProduct(Offset, Right);
				const double ScreenY = FVector::DotProduct(Offset, Up);
				Test.TestTrue(*FString::Printf(TEXT("%s view %s UV corner %d is upright"),
					*Quad->GetName(), *View.ToString(), Corner),
					(Corner == 1 ? ScreenX > 0 : ScreenX < 0)
					&& (Corner == 2 ? ScreenY < 0 : ScreenY > 0));
			}
			++Checked;
		}
	}
	return Test.TestTrue(TEXT("real component emitted visible instances"), Checked > 0);
}

bool RunFacingComponent(FAutomationTestBase& Test, const int32 Kind)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	APlayerController* Controller = UGameplayStatics::GetPlayerController(World, 0);
	if (!Controller)
	{
		auto* Spawned = World->SpawnActor<AWoTPlayerController>();
		Spawned->SetAsLocalPlayerController();
		Controller = Spawned;
	}
	ACameraActor* Camera = World->SpawnActor<ACameraActor>();
	Camera->GetRootComponent()->SetMobility(EComponentMobility::Movable);
	Controller->SetViewTarget(Camera);
	Controller->PlayerCameraManager->bUseClientSideCameraUpdates = false;
	AActor* Subject = nullptr;
	if (Kind == 0)
	{
		// The citadel marker is a private controller/UI path. Build the same
		// transient actor and component with its real StairActive texture.
		Subject = World->SpawnActor<AActor>();
		auto* Sprite = NewObject<UWoTSpriteBillboardComponent>(Subject);
		Subject->SetRootComponent(Sprite);
		Subject->AddInstanceComponent(Sprite);
		Sprite->RegisterComponent();
		Sprite->SetSpriteSeries(TEXT("WoTTraps"), {TEXT("StairActive")}, 64, 64);
	}
	else if (Kind == 1)
	{
		auto* Sprayer = World->SpawnActorDeferred<AWoTParticleSprayer>(
			AWoTParticleSprayer::StaticClass(), FTransform::Identity);
		FWoTParticleTemplate Particle;
		Particle.LifeSpan = 10;
		Sprayer->Templates.Add(Particle);
		Sprayer->Volume = 60;
		Sprayer->VisibilityRadius = Sprayer->VisibilityHeight = 10000;
		Sprayer->FinishSpawning(FTransform::Identity);
		Subject = Sprayer;
	}
	else if (Kind == 2)
	{
		const FTransform At(FVector(100,0,50));
		auto* Segment = World->SpawnActorDeferred<AWoTMashadarSegment>(
			AWoTMashadarSegment::StaticClass(), At, nullptr, nullptr,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		Segment->FinishSpawning(At);
		Subject = Segment;
	}
	else
	{
		auto* Beam = World->SpawnActor<AWoTBeamEffect>();
		Beam->Config = TEXT("BalefireLine");
		Beam->Start = FVector(0,0,0);
		Beam->End = FVector(100,0,0);
		Subject = Beam;
	}
	for (const FRotator& View : FacingViews)
	{
		Camera->SetActorLocationAndRotation(-View.Vector() * 1000, View);
		Controller->PlayerCameraManager->UpdateCamera(0);
		Test.TestTrue(TEXT("camera cache follows driven view"),
			Controller->PlayerCameraManager->GetCameraRotation().Equals(View, 1.e-4));
		WoTTest::TickWorld(World, 2);
		CheckCorners(Test, Subject, View);
	}
	WoTTest::DestroySpecWorld(World);
	return !Test.HasAnyErrors();
}
}

#define FACING_SPEC(Class, Name, Kind) \
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(Class, "WoT.Render.SpriteFacing." Name, \
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter) \
	bool Class::RunTest(const FString&) { return RunFacingComponent(*this, Kind); }

FACING_SPEC(FWoTSpriteFacingBillboard, "Billboard", 0)
FACING_SPEC(FWoTSpriteFacingSprayer, "ParticleSprayer", 1)
FACING_SPEC(FWoTSpriteFacingMesh, "MeshParticles", 2)
FACING_SPEC(FWoTSpriteFacingBeam, "Beam", 3)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSpriteFacingAxes, "WoT.Render.SpriteFacing.HelperAxes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTSpriteFacingAxes::RunTest(const FString&)
{
	for (const FRotator& View : FacingViews)
	{
		const FRotationMatrix CameraBasis(View);
		const FQuat Face = WoTSpriteFacing::ScreenAlignedQuat(View);
		TestTrue(TEXT("quad X is camera right"), Face.GetAxisX().Equals(CameraBasis.GetUnitAxis(EAxis::Y), 1.e-4));
		TestTrue(TEXT("quad Y is camera down"), Face.GetAxisY().Equals(-CameraBasis.GetUnitAxis(EAxis::Z), 1.e-4));
		TestTrue(TEXT("quad Z faces camera"), Face.GetAxisZ().Equals(-CameraBasis.GetUnitAxis(EAxis::X), 1.e-4));
	}
	return !HasAnyErrors();
}

#endif
