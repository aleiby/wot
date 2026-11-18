#pragma once

#include "CoreMinimal.h"
#include "RetailData/WoTRetailBuildContext.h"
#include "GameFramework/Actor.h"
#include "WoTBootstrapCore/Hash.h"
#include "WoTParticleSprayer.h"
#include "WoTBloodFX.generated.h"

// One resolved blood ParticleSprayer config (the ParticleSystems.Blood* leaf chains,
// mechanically resolved through the .uc default chain — gore Slice 4d). All blood is
// STY_Modulated; BurstSeconds/SelfDestroySeconds carry the one-shot lifecycle
// (TriggerTimed TimerDuration + the PreBeginPlay LifeSpan). The DecalType emission
// (3%/1%) is the 4e seam.
USTRUCT()
struct FWoTBloodConfig
{
	GENERATED_BODY()

	UPROPERTY(EditDefaultsOnly, Category = "WoT") float Spread = 45.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT") float Volume = 10.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT") FVector Gravity = FVector::ZeroVector;
	UPROPERTY(EditDefaultsOnly, Category = "WoT") float PrimeCount = 0.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT") float BurstSeconds = 0.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT") float SelfDestroySeconds = 0.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT") TArray<FWoTParticleTemplate> Templates;
	// The DecalType/DecalPercent seam is decoded from the activated row.
	UPROPERTY(EditDefaultsOnly, Category = "WoT") FName DecalTypeName;
	UPROPERTY(EditDefaultsOnly, Category = "WoT") float DecalPercent = 0.0f;
	// Actor.GoreLevel inherited by the retail sprayer class.
	UPROPERTY(EditDefaultsOnly, Category = "WoT") uint8 GoreLevel = 0;
	// Per-template ParticleSystems texture names bound directly to the neutral
	// M_WoT_MeshModulate 2x master.
	UPROPERTY(EditDefaultsOnly, Category = "WoT") TArray<FName> TextureNames;
};

struct FWoTBloodSet
{
	TMap<FName, FWoTBloodConfig> Configs;
};

class FWoTRuntimeClassDefaults;

struct FWoTBloodFXBuilder
{
	static constexpr const TCHAR* FamilyName = TEXT("blood-fx");

	static bool Build(const FWoTRetailBuildContext& Context,
		FWoTBloodSet& Out, FString& OutError);
	static FString CanonicalDigest(const FWoTBloodSet& Set);
	static void Install(FWoTBloodSet&& Set);
	static void SerializeCanonical(const FWoTBloodSet& Set, TArray<uint8>& Out);
	static bool AreEquivalent(const FWoTBloodSet& Built,
		const FWoTBloodSet& Expected, FString& OutError);

#if WITH_AUTOMATION_TESTS
	static void Reset();
#endif
};

namespace WoTBloodFX
{
	// Null for unknown or unavailable activated names.
	const FWoTBloodConfig* Find(FName Config);

	// Spawn a LOCAL (non-replicated) configured sprayer — cosmetic, per-machine.
	// Missing materials degrade quietly (headless testing). Follow tracks a moving
	// owner (the chunk trail). Returns null when the config is unknown.
	AWoTParticleSprayer* SpawnLocal(UWorld& World, FName Config, const FVector& Location,
	                                const FRotator& Rotation, float VolumeScale = 1.0f,
	                                AActor* Follow = nullptr);

	// Authority: spawn the replicated AWoTBloodBurst marker (deferred so the payload
	// lands before BeginPlay fires the local sprayer). No-op off authority/teardown.
	class AWoTBloodBurst* SpawnBurstMarker(UWorld& World, FName Config,
	                                       const FVector& Location,
	                                       const FRotator& Rotation,
	                                       float VolumeScale = 1.0f);
}

// The replicated one-shot blood event: a thin marker the authority spawns at the
// bleed point (bNetTemporary, like the shipped BloodBase); every machine's BeginPlay
// spawns the local cosmetic sprayer and the marker expires. This replicates ONE tiny
// actor instead of the sprayer's whole config surface.
UCLASS()
class AWoTBloodBurst : public AActor
{
	GENERATED_BODY()

public:
	AWoTBloodBurst();

	UPROPERTY(Replicated) FName Config;
	UPROPERTY(Replicated) float VolumeScale = 1.0f;

	virtual void BeginPlay() override;
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;
};
