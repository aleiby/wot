#include "WoTBloodFX.h"
#include "Assets/WoTRuntimeAssets.h"
#include "WoTPreferencesSubsystem.h"

#include "Engine/World.h"
#include "Materials/MaterialInterface.h"
#include "Net/UnrealNetwork.h"

// Activated blood rows are payload-owned. One-shot lifecycle behavior remains
// native (BloodBase.uc:16-26, TriggerTimed.uc:28-42, BloodSpray02.uc:20-31).
namespace
{
	TUniquePtr<FWoTBloodSet>& InstalledBloodRegistry()
	{
		static TUniquePtr<FWoTBloodSet> Set;
		return Set;
	}

	const TMap<FName, FWoTBloodConfig>& ActiveBloodRegistry()
	{
		if (const TUniquePtr<FWoTBloodSet>& Installed = InstalledBloodRegistry())
		{
			return Installed->Configs;
		}
		static const TMap<FName, FWoTBloodConfig> Empty;
		return Empty;
	}
}

void FWoTBloodFXBuilder::Install(FWoTBloodSet&& Set)
{
	InstalledBloodRegistry() = MakeUnique<FWoTBloodSet>(MoveTemp(Set));
}

#if WITH_AUTOMATION_TESTS
void FWoTBloodFXBuilder::Reset()
{
	InstalledBloodRegistry().Reset();
}
#endif

namespace WoTBloodFX
{
	const FWoTBloodConfig* Find(FName Config)
	{
		return Config.IsNone() ? nullptr : ActiveBloodRegistry().Find(Config);
	}

	AWoTParticleSprayer* SpawnLocal(UWorld& World, FName Config, const FVector& Location,
	                                const FRotator& Rotation, float VolumeScale,
	                                AActor* Follow)
	{
		const FWoTBloodConfig* C = Find(Config);
		// Dedicated servers never render — skip the local sim (the replicated
		// marker still reaches every client; review perf note).
		if (!C || World.bIsTearingDown || World.GetNetMode() == NM_DedicatedServer)
		{
			return nullptr;
		}
		// DEFERRED spawn: the sprayer's BeginPlay builds its ISM passes and arms the
		// one-shot lifecycle from these fields — they must land before it runs.
		const FTransform At(Rotation, Location);
		AWoTParticleSprayer* S = World.SpawnActorDeferred<AWoTParticleSprayer>(
		    AWoTParticleSprayer::StaticClass(), At, nullptr, nullptr,
		    ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!S)
		{
			return nullptr;
		}
		S->Spread = C->Spread;
		// Volume scales with damage (GenericDamageHelper.uc:127).
		S->Volume = C->Volume * VolumeScale;
		S->Gravity = C->Gravity;
		S->PrimeCount = C->PrimeCount;
		S->BurstSeconds = C->BurstSeconds;
		S->SelfDestroySeconds = C->SelfDestroySeconds;
		S->bModulateOnly = true;   // all blood is STY_Modulated (PF_Modulated 2x)
		S->ConfigureLocalGoreLevel(C->GoreLevel);
		S->DecalTypeName = C->DecalTypeName;
		S->DecalPercent = C->DecalPercent;
		S->Templates = C->Templates;
		S->ModMaterials.SetNum(C->TextureNames.Num());
		for (int32 i = 0; i < C->TextureNames.Num(); ++i)
		{
			const FSoftObjectPath Path(FString::Printf(
				TEXT("/Game/WoT/Textures/ParticleSystems/%s.%s"),
				*C->TextureNames[i].ToString(), *C->TextureNames[i].ToString()));
			S->ModMaterials[i] =
				UWoTRuntimeAssets::CreateModulatedSpriteMaterial(S, Path);
		}
		if (Follow)
		{
			S->FollowActor = Follow;
		}
		S->FinishSpawning(At);
		if (S->PrimeCount > 0.0f)
		{
			S->PrimeAuthoredCount();
		}
		return S;
	}
}

AWoTBloodBurst* WoTBloodFX::SpawnBurstMarker(UWorld& World, FName Config,
                                             const FVector& Location,
                                             const FRotator& Rotation,
                                             float VolumeScale)
{
	if (World.bIsTearingDown || World.GetNetMode() == NM_Client)
	{
		return nullptr;
	}
	const FTransform At(Rotation, Location);
	AWoTBloodBurst* Marker = World.SpawnActorDeferred<AWoTBloodBurst>(
	    AWoTBloodBurst::StaticClass(), At, nullptr, nullptr,
	    ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (Marker)
	{
		Marker->Config = Config;
		Marker->VolumeScale = VolumeScale;
		Marker->FinishSpawning(At);
	}
	return Marker;
}

AWoTBloodBurst::AWoTBloodBurst()
{
	bReplicates = true;
	// One replication bunch then client-side autonomy — the shipped BloodBase
	// bNetTemporary model.
	bNetTemporary = true;
	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Root")));
}

void AWoTBloodBurst::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWoTBloodBurst, Config);
	DOREPLIFETIME(AWoTBloodBurst, VolumeScale);
}

void AWoTBloodBurst::BeginPlay()
{
	Super::BeginPlay();
	// Every machine (the authority immediately; each client when the marker
	// replicates in, its payload applied before BeginPlay) runs the cosmetic
	// sprayer locally, then the marker expires.
	if (GetWorld())
	{
		WoTBloodFX::SpawnLocal(*GetWorld(), Config, GetActorLocation(),
		                       GetActorRotation(), VolumeScale);
	}
	SetLifeSpan(1.0f);
}
