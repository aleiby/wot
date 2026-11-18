#pragma once

#include "CoreMinimal.h"
#include "../RetailData/WoTRetailBuildContext.h"
#include "WoTBootstrapCore/Hash.h"
#include "../WoTParticleSprayer.h"
#include "WoTWall.h"
#include "../WoTRetailProjectileTarget.h"
#include "WoTFireWall.generated.h"

class AWoTParticleSprayer;
class FWoTRuntimeClassDefaults;

struct FWoTFireWallTemplate
{
	FWoTParticleTemplate Template;
	FName TextureIdentity;
};

struct FWoTFireWallSprayer
{
	FName ClassIdentity;
	FVector Offset = FVector::ZeroVector;
	float Spread = 45.0f;
	float Volume = 10.0f;
	float MinVolume = 0.0f;
	float VolumeScalePct = 0.5f;
	float VisibilityRadius = 1600.0f;
	float VisibilityHeight = 1600.0f;
	FVector Gravity = FVector::ZeroVector;
	bool bModulateOnly = false;
	float FadeLifeSpan = 1.0f;
	TArray<FWoTFireWallTemplate> Templates;
};

struct FWoTFireWallDefaults
{
	int32 Health = 0;
	float DamageAmount = 0.0f;
	float DamageInterval = 0.0f;
	int32 SlabWidth = 0;
	FName SlabMesh;
	float SlabDrawScale = 1.0f;
	float CollisionRadius = 0.0f;
	float CollisionHeight = 0.0f;
	int32 MaxSlabs = 0;
	int32 InventoryCount = 0;
};

struct FWoTFireWallSet
{
	TArray<FWoTFireWallSprayer> Sprayers;
	FWoTFireWallDefaults Defaults;
};

struct FWoTFireWallBuilder
{
	static constexpr const TCHAR* FamilyName = TEXT("fire-wall");

	static bool Build(const FWoTRetailBuildContext& Context,
		FWoTFireWallSet& Out, FString& OutError);
	static FString CanonicalDigest(const FWoTFireWallSet& Set);
	static void Install(FWoTFireWallSet&& Set);
	static void SerializeCanonical(const FWoTFireWallSet& Set, TArray<uint8>& Out);
	static bool AreEquivalent(const FWoTFireWallSet& Built,
		const FWoTFireWallSet& Expected, FString& OutError);

#if WITH_AUTOMATION_TESTS
	static void Reset();
#endif
};

// Port of WoTTraps/FireWall{,Slab}.uc + the FireWallSprayer1 damage engine.
// FireWallSlab replaces WallSlab's staged WithstandDamage with its own health
// pool and periodically burns touchers (FireWallSlab.uc:58-105). Health
// replicates — the one genuine client consumer in the family: the original's simulated Tick scaled
// the three flame sprayers by Health/default.Health (OnRep_Health + the authority TakeDamage path
// drive ApplyFlameScale; the three FireWallSprayer1/2/3 bundles spawn as local cosmetic
// AWoTParticleSprayers on every rendering machine — Slice 6b-ii). The port folds
// FireWallSprayer1's Touch/DamageInterval engine into a server-side FlameVolume capsule on the slab
// (UE1 hung it on the sprayer's own 22x22 Actor cylinder at the slab's FlameSprayerOffset1) — same
// cadence, same DamageActor routing. FireTrapFlame is shipped dead code (fully commented out) — not
// ported.
UCLASS()
class AWoTFireWallSlab : public AWoTWallSlab, public IWoTRetailProjectileTarget
{
	GENERATED_BODY()

public:
	AWoTFireWallSlab();

	// FireWallSlab.Health (replication{} unreliable if(Role==Authority)) — the flame's fuel.
	UPROPERTY(ReplicatedUsing = OnRep_Health, EditDefaultsOnly, Category = "WoT|Trap")
	int32 Health = 0;

	// FireWallSprayer1.DamageInterval — the re-burn cadence for actors standing in the flame.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Trap") float DamageInterval = 0.0f;

	// FireWallSlab.DamageActor: owner-trap gate, then burn — the slab damages ITSELF ('Touched')
	// and the toucher ('xxFxx' Fire) by DamageAmount.
	virtual void DamageActor(AActor* Other) override;

	// FireWallSlab.TakeDamage — REPLACES the WallSlab stage/burst logic (the .uc override has no
	// Super call): owner-trap gate on the instigator, Health = max(Health - Damage, 0), destroy
	// at 0.
	virtual float TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent,
	                         AController* EventInstigator, AActor* DamageCauser) override;

	virtual void BeginPlay() override;
	// FireWallSlab.Destroyed: the flames stop emitting and fade out according to
	// their activated rows (FireWallSlab.uc:164-182).
	virtual void Destroyed() override;
	// The pawn entered the flame (the blocking log base never overlaps — any actor-overlap here
	// is FlameVolume contact): burn immediately, as FireWallSprayer1.Touch did.
	virtual void NotifyActorBeginOverlap(AActor* Other) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// The three local flame sprayers (FlameSprayer1/2/3): largest flame, smoke, the
	// upward jet. Local cosmetics — never replicated, skipped on dedicated servers.
	UPROPERTY() TObjectPtr<AWoTParticleSprayer> FlameSprayers[3];
	bool HasActiveBurnTimerForTest() const;

protected:
	// FireWallSprayer1's touch cylinder at its activated sprayer offset.
	UPROPERTY(VisibleAnywhere, Category = "WoT|Trap")
	TObjectPtr<UCapsuleComponent> FlameVolume;

private:
	// FireWallSprayer1.Tick's DamageInterval loop: re-burn everyone standing in the flame.
	void BurnTouching();

	// The shipped Tick's lazy sprayer spawn, done once at BeginPlay (slabs don't move).
	void SpawnFlameSprayers();
	// FireWallSlab.ScaleFlameSprayer x3: Volume/MinVolume + every template's
	// Min/MaxScaleGlow + Min/MaxDrawScale scale by Health/default.Health (recomputed
	// from the config rows — live particles keep their spawn values, like UE1's
	// template mutation).
	void ApplyFlameScale();

	UFUNCTION()
	void OnRep_Health() { ApplyFlameScale(); }

	FTimerHandle BurnTimer;
};

// FireWall.uc: a Wall whose slabs are FireWallSlabs (MaxSlabs=18). Flat slab rotation +
// a teleporter-exclusion occupancy check are the FireWall's create-placement policy.
UCLASS()
class AWoTFireWall : public AWoTWall
{
	GENERATED_BODY()

public:
	AWoTFireWall();
	virtual void BeginPlay() override;

	// FireWall.GetSlabRotation: FireWall slabs are uniform — no per-slab yaw cycle.
	virtual FRotator GetSlabRotation(int32 SlabIndex, const FVector& Normal) const override;
	// FireWallSlab's width and log-base height come from the activated family.
	virtual int32 GetSlabWidth() const override;
	virtual float GetSlabHalfHeight() const override;

	// FireWall.AnyActorsInArea: base occupancy, then reject placement near a Teleporter
	// (a second pass at 2*Radius returning the first Teleporter within 1000 UU).
	virtual AActor* AnyActorsInArea(const FVector& Loc, float Radius) const override;
};

// Citadel-editor placement adapter (FireWallInventory.uc). Data-only leaf.
UCLASS()
class AWoTFireWallInventory : public AWoTInventory
{
	GENERATED_BODY()

public:
	AWoTFireWallInventory();
	virtual void BeginPlay() override;
};
