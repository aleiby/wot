#pragma once

#include "CoreMinimal.h"

#if WITH_AUTOMATION_TESTS

class AActor;
class AGameModeBase;
class UGameInstance;
class UWorld;

namespace WoTTest
{
	// The standalone automation worlds share one lifecycle. Options expose only
	// the ordering differences that are load-bearing in the existing fixtures.
	struct FSpecWorldOptions
	{
		TSubclassOf<UGameInstance> GameInstanceClass;
		TSubclassOf<AGameModeBase> GameModeClass;
		/** Optional map/options string passed through the real SetGameMode lifecycle. */
		FString URL;
		TFunction<void(UGameInstance*)> BeforeInitializeStandalone;
		TFunction<void(UWorld*)> BeforeInitializeActorsForPlay;
		bool bStartPlay = true;
		bool bBeginPlay = true;
		// Shipping subsystems (zones, events, lights, sounds, ...) are gated
		// behind IsWoTBootstrapWorldReady(); a dormant world is inert. Gameplay
		// specs need a ready world, so the fixture publishes a test activation
		// and adopted normal-world authority by default. Suites that exercise the
		// activation machinery itself must opt out and manage the global
		// publication state themselves.
		bool bActivatePayload = true;
	};

	UWorld* CreateSpecWorld(const FSpecWorldOptions& Options = FSpecWorldOptions());
	UWorld* CreateSpecWorld(TSubclassOf<AGameModeBase> GameModeClass);
	void DestroySpecWorld(UWorld* World);

	void TickWorld(UWorld* World, float Duration, float Step = 1.0f / 60.0f);
	void TickWorld(UWorld* World, int32 NumTicks, float Step = 1.0f / 60.0f);

	// Teleport Actor without generating engine begin-overlaps on the way, so a
	// spec that invokes an overlap handler directly delivers exactly one touch
	// from a physically meaningful position (UE1 touch handlers now require the
	// toucher to meet the authored flat cylinder).
	void PlaceWithoutOverlapEvents(AActor* Actor, const FVector& Location);
}

#endif
