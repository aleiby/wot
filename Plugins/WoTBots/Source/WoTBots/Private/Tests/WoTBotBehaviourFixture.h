#pragma once

#if WITH_AUTOMATION_TESTS
#include "WoTBotController.h"
#include "WoTBotNavigation.h"
#include "WoTBotSettings.h"
#include "Tests/WoTTestWorld.h"
#include "WoTCharacter.h"
#include "WoTGameInstance.h"
#include "WoTGameMode.h"
#include "WoTPlayerState.h"
#include "WoTRandom.h"
#include "Inventory/WoTAngrealInventory.h"
#include "Inventory/WoTAngrealSpawnPool.h"
#include "Inventory/WoTInventoryComponent.h"
#include "Components/BoxComponent.h"
#include "Engine/World.h"
#include "Engine/StaticMeshActor.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerStart.h"
#include "NavMesh/NavMeshBoundsVolume.h"
#include "Misc/ScopeExit.h"

namespace WoTBotTest
{
constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

inline void SeedDecisions(int32 Seed)
{
	// Gameplay decisions use the portable stream. Recast's random reachable-point
	// query still uses the engine stream; integration fixtures control both.
	WoTRandom::Seed(Seed);
	FMath::RandInit(Seed);
	FMath::SRandInit(Seed);
}

struct FSettings
{
	UWoTBotSettings* Value = GetMutableDefault<UWoTBotSettings>();
	float Reaction = Value->ReactionSeconds, Sight = Value->SightRange, Memory = Value->TargetMemorySeconds;
	float Error = Value->AimErrorDegrees, Engage = Value->EngageWhileSeekingRange, Stuck = Value->StuckRespawnSeconds;
	~FSettings()
	{
		Value->ReactionSeconds = Reaction; Value->SightRange = Sight; Value->TargetMemorySeconds = Memory;
		Value->AimErrorDegrees = Error; Value->EngageWhileSeekingRange = Engage; Value->StuckRespawnSeconds = Stuck;
	}
};

inline AActor* Box(UWorld* World, FVector Location, FVector Extent, bool bNav = true)
{
	AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(Location, FRotator::ZeroRotator);
	UBoxComponent* Shape = NewObject<UBoxComponent>(Actor);
	Shape->SetupAttachment(Actor->GetRootComponent());
	Shape->SetBoxExtent(Extent);
	Shape->SetMobility(EComponentMobility::Static);
	Shape->SetCollisionProfileName(TEXT("BlockAll"));
	Shape->SetCanEverAffectNavigation(bNav);
	Shape->RegisterComponent();
	return Actor;
}

inline UWorld* World()
{
	WoTTest::FSpecWorldOptions Options;
	Options.GameInstanceClass = UWoTGameInstance::StaticClass();
	Options.GameModeClass = AWoTGameMode::StaticClass();
	Options.URL = TEXT("/Engine/Maps/Entry?BotFill=0?FragLimit=0?TimeLimit=0");
	Options.BeforeInitializeActorsForPlay = [](UWorld* W)
	{
		Box(W, FVector(0, 0, -50), FVector(3000, 2000, 50));
		W->SpawnActor<APlayerStart>(FVector(-1000, 0, 50), FRotator::ZeroRotator);
		W->SpawnActor<APlayerStart>(FVector(1000, 0, 50), FRotator(0, 180, 0));
	};
	UWorld* W = WoTTest::CreateSpecWorld(Options);
	// Join an ongoing match so initial artifact group clocks (up to ten
	// seconds) cannot mask the controller's reaction and utility decisions.
	WoTTest::TickWorld(W, 12.0f);
	return W;
}

inline AWoTCharacter* Body(UWorld* W, FVector Location)
{
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTCharacter* Pawn = W->SpawnActor<AWoTCharacter>(W->GetAuthGameMode()->DefaultPawnClass, Location, FRotator::ZeroRotator, Params);
	// Standalone fixture worlds have no authenticated class-default payload.
	Pawn->MaxHealth = 100;
	return Pawn;
}

inline AWoTBotController* Bot(UWorld* W, FVector Location, uint8 Team = 0)
{
	AWoTBotController* Controller = W->SpawnActor<AWoTBotController>();
	AWoTCharacter* Pawn = Body(W, Location);
	Controller->InitializeIdentity(nullptr, Pawn->GetClass(), TEXT("Test bot"));
	Controller->GetPlayerState<AWoTPlayerState>()->Team = Team;
	Controller->Possess(Pawn);
	W->GetAuthGameMode()->SetPlayerDefaults(Pawn);
	return Controller;
}

inline AWoTCharacter* Enemy(UWorld* W, FVector Location, uint8 Team = 1)
{
	APlayerController* Controller = W->SpawnActor<APlayerController>(W->GetAuthGameMode()->PlayerControllerClass);
	Controller->GetPlayerState<AWoTPlayerState>()->Team = Team;
	AWoTCharacter* Pawn = Body(W, Location);
	Controller->Possess(Pawn);
	return Pawn;
}

inline AWoTAngrealInventory* Give(AWoTCharacter* Pawn, const TCHAR* ClassName)
{
	UClass* Class = FindObject<UClass>(nullptr, ClassName);
	check(Class);
	const FTransform Transform(Pawn->GetActorTransform());
	AWoTAngrealInventory* Item = Pawn->GetWorld()->SpawnActorDeferred<AWoTAngrealInventory>(Class,
		Transform, Pawn, Pawn, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	Item->bHeldItem = true;
	Item->FinishSpawning(Transform);
	Item->GiveTo(Pawn);
	Item->SetCharges();
	return Item;
}

inline AWoTAngrealInventory* Kit(AWoTBotController* Bot)
{
	AWoTCharacter* Pawn = CastChecked<AWoTCharacter>(Bot->GetPawn());
	for (AWoTInventory* Item : Pawn->InventoryComponent->Items)
		if (Item->GetClass() == Pawn->GetWorld()->GetAuthGameMode<AWoTGameMode>()->DefaultAngreal)
			return CastChecked<AWoTAngrealInventory>(Item);
	return nullptr;
}

inline AWoTAngrealSpawnPool* Pool(UWorld* W, FVector Location, const TCHAR* ItemClass)
{
	const FTransform Transform(Location);
	AWoTAngrealSpawnPool* Pool = W->SpawnActorDeferred<AWoTAngrealSpawnPool>(AWoTAngrealSpawnPool::StaticClass(), Transform);
	Pool->Pool.Add(FindObject<UClass>(nullptr, ItemClass));
	Pool->Weight.Add(1);
	Pool->bRespawn = false;
	Pool->FinishSpawning(Transform);
	return Pool;
}

inline UNavigationSystemV1* BuildNav(UWorld* W, FVector Extent = FVector(2900, 1900, 400))
{
	ANavMeshBoundsVolume* Bounds = W->SpawnActor<ANavMeshBoundsVolume>(FVector(0, 0, 150), FRotator::ZeroRotator);
	UBoxComponent* Shape = NewObject<UBoxComponent>(Bounds);
	Shape->SetupAttachment(Bounds->GetRootComponent());
	Shape->SetBoxExtent(Extent);
	Shape->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Shape->SetCanEverAffectNavigation(false);
	Shape->RegisterComponent();
	if (!W->GetNavigationSystem()) FNavigationSystem::AddNavigationSystemToWorld(*W, FNavigationSystemRunMode::GameMode);
	UNavigationSystemV1* Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(W);
	if (!Nav) return nullptr;
	Nav->OnNavigationBoundsUpdated(Bounds);
	Nav->Build();
	// Complete the real asynchronous tile work without advancing gameplay clocks.
	for (ANavigationData* Data : Nav->NavDataSet) if (Data) Data->EnsureBuildCompletion();
	return Nav;
}
}
#endif
