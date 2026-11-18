#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../WoTParticleSprayer.h"
#include "../WoTPlayerController.h"

#include "Camera/CameraActor.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Components/BoxComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Camera/PlayerCameraManager.h"

namespace
{
	constexpr EAutomationTestFlags ParticleSprayerFlags =
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	FActorSpawnParameters ParticleSprayerAlwaysSpawn()
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		return Params;
	}

	struct FParticleCameraFixture
	{
		APlayerController* Controller = nullptr;
		ACameraActor* Camera = nullptr;

		bool Initialize(UWorld& World, const FVector& Location)
		{
			// The sprayer asks UGameplayStatics for player 0's camera manager, so
			// drive that controller; spawn one only if the world has none.
			Controller = UGameplayStatics::GetPlayerController(&World, 0);
			if (!Controller)
			{
				AWoTPlayerController* Spawned =
					World.SpawnActor<AWoTPlayerController>(ParticleSprayerAlwaysSpawn());
				if (Spawned) Spawned->SetAsLocalPlayerController();
				Controller = Spawned;
			}
			Camera = World.SpawnActor<ACameraActor>(Location,
				FRotator::ZeroRotator, ParticleSprayerAlwaysSpawn());
			if (!Controller || !Camera) return false;
			// MoveTo relocates the camera; a Static root would refuse it silently.
			Camera->GetRootComponent()->SetMobility(EComponentMobility::Movable);
			Controller->SetViewTarget(Camera);
			// A controller without a LocalPlayer skips DoUpdateCamera while client
			// camera updates are on; the spec drives the cache directly.
			if (Controller->PlayerCameraManager)
			{
				Controller->PlayerCameraManager->bUseClientSideCameraUpdates = false;
			}
			Update();
			return Controller->PlayerCameraManager != nullptr
				&& UGameplayStatics::GetPlayerCameraManager(&World, 0)
					== Controller->PlayerCameraManager;
		}

		bool MoveTo(const FVector& Location)
		{
			Camera->SetActorLocation(Location);
			Update();
			return Controller && Controller->PlayerCameraManager
				&& Controller->PlayerCameraManager->GetCameraLocation().Equals(Location, 1.0);
		}

		void Update() const
		{
			if (Controller && Controller->PlayerCameraManager)
			{
				Controller->PlayerCameraManager->UpdateCamera(0.0f);
			}
		}
	};

	AWoTParticleSprayer* SpawnSprayer(UWorld& World, const FVector& Location,
		bool bFreezeOutsideVisibility, bool bLOSClip = false)
	{
		const FTransform Transform(FRotator::ZeroRotator, Location);
		AWoTParticleSprayer* Sprayer = World.SpawnActorDeferred<AWoTParticleSprayer>(
			AWoTParticleSprayer::StaticClass(), Transform, nullptr, nullptr,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!Sprayer) return nullptr;
		FWoTParticleTemplate Template;
		Template.LifeSpan = 1.0f;
		Template.Weight = 1.0f;
		Template.MinInitialVelocity = 0.0f;
		Template.MaxInitialVelocity = 0.0f;
		Template.MinDrawScale = 1.0f;
		Template.MaxDrawScale = 1.0f;
		Sprayer->Templates.Add(Template);
		Sprayer->Volume = 60.0f;
		Sprayer->VolumeScalePct = 0.5f;
		Sprayer->VisibilityRadius = 500.0f;
		Sprayer->VisibilityHeight = 500.0f;
		Sprayer->bFreezeOutsideVisibility = bFreezeOutsideVisibility;
		Sprayer->bLOSClip = bLOSClip;
		Sprayer->FinishSpawning(Transform);
		return Sprayer;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPlacedSprayerVisibilityFreezeSpec,
	"WoT.Particles.PlacedVisibilityCylinderFreeze", ParticleSprayerFlags)

bool FWoTPlacedSprayerVisibilityFreezeSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	if (!TestNotNull(TEXT("standalone world"), World)) return false;
	FParticleCameraFixture View;
	if (!TestTrue(TEXT("local particle camera"),
		View.Initialize(*World, FVector::ZeroVector)))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	AWoTParticleSprayer* Placed = SpawnSprayer(
		*World, FVector(100, 0, 0), true);
	if (!TestNotNull(TEXT("placed sprayer"), Placed))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	WoTTest::TickWorld(World, 1.0f / 60.0f);
	const int32 SteadyCount = Placed->GetLiveParticleCountForTesting();
	TestTrue(TEXT("in-range warm-up reaches a live steady state"), SteadyCount > 20);

	TestTrue(TEXT("camera moved"), View.MoveTo(FVector(2000, 0, 0)));
	WoTTest::TickWorld(World, 3.0f);
	TestEqual(TEXT("out-of-range placed particles remain frozen"),
		Placed->GetLiveParticleCountForTesting(), SteadyCount);
	TestFalse(TEXT("out-of-range placed instances are hidden"),
		Placed->AreParticleInstancesVisibleForTesting());

	TestTrue(TEXT("camera moved"), View.MoveTo(FVector::ZeroVector));
	WoTTest::TickWorld(World, 1.0f / 60.0f);
	const int32 ReenteredCount = Placed->GetLiveParticleCountForTesting();
	TestTrue(TEXT("re-entry retains approximately the pre-exit steady state"),
		FMath::Abs(ReenteredCount - SteadyCount) <= FMath::Max(2, SteadyCount / 4));
	TestTrue(TEXT("re-entered placed instances are visible"),
		Placed->AreParticleInstancesVisibleForTesting());

	AWoTParticleSprayer* Runtime = SpawnSprayer(
		*World, FVector(100, 0, 0), false);
	if (TestNotNull(TEXT("runtime sprayer"), Runtime))
	{
		Runtime->SkipPriming();
		WoTTest::TickWorld(World, 1.25f);
		TestTrue(TEXT("runtime sprayer builds live particles in range"),
			Runtime->GetLiveParticleCountForTesting() > 20);
		TestTrue(TEXT("camera moved"), View.MoveTo(FVector(2000, 0, 0)));
		WoTTest::TickWorld(World, 3.0f);
		TestEqual(TEXT("unflagged runtime sprayer still ages out of range"),
			Runtime->GetLiveParticleCountForTesting(), 0);
	}

	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPlacedSprayerLOSClipFreezeSpec,
	"WoT.Particles.PlacedLOSClipFreeze", ParticleSprayerFlags)

bool FWoTPlacedSprayerLOSClipFreezeSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	if (!TestNotNull(TEXT("standalone world"), World)) return false;
	FParticleCameraFixture View;
	if (!TestTrue(TEXT("local particle camera"),
		View.Initialize(*World, FVector::ZeroVector)))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	AWoTParticleSprayer* Sprayer = SpawnSprayer(
		*World, FVector(100, 0, 0), true, true);
	if (!TestNotNull(TEXT("LOS-clipped placed sprayer"), Sprayer))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	WoTTest::TickWorld(World, 1.0f / 60.0f);
	const int32 LiveBeforeClip = Sprayer->GetLiveParticleCountForTesting();
	const double SimTimeBeforeClip = Sprayer->GetLastSimTimeSecondsForTesting();
	TestTrue(TEXT("unclipped sprayer primes before the LOS fixture"), LiveBeforeClip > 20);
	TestTrue(TEXT("unclipped placed instances are visible"),
		Sprayer->AreParticleInstancesVisibleForTesting());

	// Level geometry (AStaticMeshActor), the kind TRACE_VisBlocking stops on,
	// with a registered blocking box: the pattern WoTPathNodeIteratorTests uses.
	AStaticMeshActor* Wall = World->SpawnActor<AStaticMeshActor>(
		FVector(50, 0, 0), FRotator::ZeroRotator, ParticleSprayerAlwaysSpawn());
	if (TestNotNull(TEXT("LOS wall"), Wall))
	{
		UBoxComponent* Box = NewObject<UBoxComponent>(Wall, TEXT("LOSWall"));
		Box->SetupAttachment(Wall->GetRootComponent());
		Box->SetBoxExtent(FVector(10.0f, 200.0f, 200.0f));
		Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Box->SetCollisionObjectType(ECC_WorldStatic);
		Box->SetCollisionResponseToAllChannels(ECR_Block);
		Box->RegisterComponent();
		WoTTest::TickWorld(World, 1.0f);
		TestEqual(TEXT("LOS-clipped placed particles remain frozen"),
			Sprayer->GetLiveParticleCountForTesting(), LiveBeforeClip);
		TestEqual(TEXT("LOS clipping does not advance placed simulation time"),
			Sprayer->GetLastSimTimeSecondsForTesting(), SimTimeBeforeClip);
		TestFalse(TEXT("LOS-clipped placed instances draw nothing"),
			Sprayer->AreParticleInstancesVisibleForTesting());
	}

	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

#endif
