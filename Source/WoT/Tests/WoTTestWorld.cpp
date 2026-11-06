#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../Payload/WoTPayloadActivation.h"
#include "../World/WoTWorldMaterializer.h"
#include "../WoTZoneVolume.h"
#include "../WoTRandom.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/WorldSettings.h"
#include "Components/PrimitiveComponent.h"

namespace WoTTest
{
	namespace
	{
		// Worlds whose creation published the test activation; their destruction
		// resets it. Suites that self-manage publication never enter this set.
		TSet<TWeakObjectPtr<UWorld>> GSpecWorldsHoldingActivation;
		TSet<TWeakObjectPtr<UWorld>> GSpecWorldsHoldingNormalAuthority;

		bool GrantSpecPayloadActivation()
		{
			if (IsWoTPayloadBootstrapActive())
			{
				return false;   // a live publication (spec-owned) stays untouched
			}
			return !PublishWoTPayloadBootstrapActivationForTesting().IsError()
				&& !PublishWoTBootstrapWorldReadyForTesting().IsError();
		}
	}

	UWorld* CreateSpecWorld(const FSpecWorldOptions& Options)
	{
		// T05 rider: every standalone spec starts with the same global RNG state,
		// making each spec deterministic both alone and in suite order.
		FMath::RandInit(0x7051);
		FMath::SRandInit(0x7051);
		WoTRandom::Seed(0x7051);

		const bool bGrantedActivation =
			Options.bActivatePayload && GrantSpecPayloadActivation();
		const bool bGrantedNormalAuthority = Options.bActivatePayload
			&& FWoTWorldMaterializer::
				PublishNormalAuthorityAdoptedForAutomationTests();

		UClass* GameInstanceClass = Options.GameInstanceClass
			? Options.GameInstanceClass.Get() : UGameInstance::StaticClass();
		UGameInstance* GameInstance = NewObject<UGameInstance>(GEngine, GameInstanceClass);
		if (!IsValid(GameInstance))
		{
			if (bGrantedNormalAuthority)
			{
				FWoTWorldMaterializer::
					ResetNormalAuthorityAdoptedForAutomationTests();
			}
			if (bGrantedActivation)
			{
				ResetWoTPayloadActivationForTesting();
			}
			return nullptr;
		}
		if (Options.BeforeInitializeStandalone)
		{
			Options.BeforeInitializeStandalone(GameInstance);
		}
		GameInstance->InitializeStandalone();

		UWorld* World = GameInstance->GetWorld();
		if (!IsValid(World))
		{
			GameInstance->Shutdown();
			if (bGrantedNormalAuthority)
			{
				FWoTWorldMaterializer::
					ResetNormalAuthorityAdoptedForAutomationTests();
			}
			if (bGrantedActivation)
			{
				ResetWoTPayloadActivationForTesting();
			}
			return nullptr;
		}
		if (bGrantedActivation)
		{
			GSpecWorldsHoldingActivation.Add(World);
		}
		if (bGrantedNormalAuthority)
		{
			GSpecWorldsHoldingNormalAuthority.Add(World);
		}
		if (!Options.bStartPlay)
		{
			return World;
		}

		if (Options.GameModeClass)
		{
			World->GetWorldSettings()->DefaultGameMode = Options.GameModeClass;
		}
		FURL URL;
		if (!Options.URL.IsEmpty())
		{
			URL = FURL(nullptr, *Options.URL, TRAVEL_Absolute);
		}
		World->SetGameMode(URL);
		if (Options.BeforeInitializeActorsForPlay)
		{
			Options.BeforeInitializeActorsForPlay(World);
		}
		World->InitializeActorsForPlay(URL);
		if (!Options.bBeginPlay)
		{
			return World;
		}
		World->BeginPlay();
		return World;
	}

	UWorld* CreateSpecWorld(TSubclassOf<AGameModeBase> GameModeClass)
	{
		FSpecWorldOptions Options;
		Options.GameModeClass = GameModeClass;
		return CreateSpecWorld(Options);
	}

	void DestroySpecWorld(UWorld* World)
	{
		if (!World)
		{
			return;
		}
		// Teardown runs with the activation still published (actors' EndPlay may
		// legitimately consult readiness); the reset follows the destruction.
		const bool bReleaseActivation = GSpecWorldsHoldingActivation.Remove(World) > 0;
		const bool bReleaseNormalAuthority =
			GSpecWorldsHoldingNormalAuthority.Remove(World) > 0;
		if (World->HasBegunPlay())
		{
			World->BeginTearingDown();
			World->EndPlay(EEndPlayReason::Quit);
		}
		if (UGameInstance* GameInstance = World->GetGameInstance())
		{
			GameInstance->Shutdown();
		}
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
		if (bReleaseNormalAuthority)
		{
			FWoTWorldMaterializer::
				ResetNormalAuthorityAdoptedForAutomationTests();
		}
		if (bReleaseActivation)
		{
			ResetWoTPayloadActivationForTesting();
		}
	}

	void TickWorld(UWorld* World, float Duration, float Step)
	{
		if (!World || Duration <= 0.0f || Step <= 0.0f)
		{
			return;
		}
		for (float Elapsed = 0.0f; Elapsed < Duration; Elapsed += Step)
		{
			World->Tick(LEVELTICK_All, Step);
			++GFrameCounter;
		}
	}

	void PlaceWithoutOverlapEvents(AActor* Actor, const FVector& Location)
	{
		if (!Actor)
		{
			return;
		}
		UPrimitiveComponent* Root = Cast<UPrimitiveComponent>(Actor->GetRootComponent());
		const bool bGenerated = Root && Root->GetGenerateOverlapEvents();
		if (Root)
		{
			Root->SetGenerateOverlapEvents(false);
		}
		if (Actor->SetActorLocation(Location, false, nullptr,
			ETeleportType::TeleportPhysics))
		{
			UWoTZoneIndex::RefreshAfterRelocation(Actor);
		}
		if (Root)
		{
			Root->SetGenerateOverlapEvents(bGenerated);
		}
	}

	void TickWorld(UWorld* World, int32 NumTicks, float Step)
	{
		if (!World || NumTicks <= 0 || Step <= 0.0f)
		{
			return;
		}
		for (int32 TickIndex = 0; TickIndex < NumTicks; ++TickIndex)
		{
			World->Tick(LEVELTICK_All, Step);
			++GFrameCounter;
		}
	}
}

#endif
