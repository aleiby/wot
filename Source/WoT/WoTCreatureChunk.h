#pragma once

#include "CoreMinimal.h"
#include "RetailData/WoTRetailBuildContext.h"
#include "GameFramework/Actor.h"
#include "WoTBootstrapCore/Hash.h"
#include "WoTCreatureChunk.generated.h"

class UCapsuleComponent;
class UWoTVertexAnimComponent;
class AWoTCarcass;
class FWoTRuntimeClassDefaults;

// One gib body part of a carcass recipe (WOTCarcass.uc:163-175 BodyPartT +
// ZOffsets). Mesh/skin names are the bare imported asset names (the shipped
// "WOT." package prefix drops, like every mesh ref in the port). Index 0 of a
// recipe is the replacement chunk (WOTReplacementChunk: DrawScale x2, Mass 60);
// the rest are odds-gated extras.
USTRUCT()
struct FWoTBodyPart
{
	GENERATED_BODY()

	UPROPERTY(EditDefaultsOnly, Category = "WoT") FName MeshName;
	// Exact MultiSkins slot written by WOTCarcass.CreateChunk.
	UPROPERTY(EditDefaultsOnly, Category = "WoT") FName SkinTexName;
	UPROPERTY(EditDefaultsOnly, Category = "WoT") int32 SkinSlot = 0;
	UPROPERTY(EditDefaultsOnly, Category = "WoT") float Odds = 0.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT") float ZOffset = 0.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT") bool bNoBlood = false;
	UPROPERTY(EditDefaultsOnly, Category = "WoT") bool bNoDamage = false;
	UPROPERTY(EditDefaultsOnly, Category = "WoT") FName BounceSound;
	UPROPERTY(EditDefaultsOnly, Category = "WoT") bool bZeroYaw = false;
	UPROPERTY(EditDefaultsOnly, Category = "WoT") bool bZeroRoll = false;
	UPROPERTY(EditDefaultsOnly, Category = "WoT") bool bZeroPitch = false;
	UPROPERTY(EditDefaultsOnly, Category = "WoT") uint8 GoreLevel = 0;
};

struct FWoTCreatureChunkDefaults
{
	float MinBounceSpeed = 0.0f;
	float DeleteInitialDelay = 0.0f;
	float DeleteRecheckDelay = 0.0f;
	float SinkRateMin = 0.0f;
	float SinkRateMax = 0.0f;
	float BaseInitVelocity = 0.0f;
	float PainZoneDamageScale = 0.0f;
	float MinBleedTime = 0.0f;
	float MaxBleedTime = 0.0f;
	bool bShouldSink = false;
};

struct FWoTCreatureChunkSet
{
	// Exactly five authored recipes, keyed by their full retail carcass class.
	TMap<FName, TArray<FWoTBodyPart>> Recipes;
	// Exact retail leaf -> nearest recipe class, derived from the class graph.
	TMap<FName, FName> RecipeClassByCarcassClass;
	FWoTCreatureChunkDefaults Defaults;
};

struct FWoTCreatureChunkBuilder
{
	static constexpr const TCHAR* FamilyName = TEXT("creature-chunks");

	static bool Build(const FWoTRetailBuildContext& Context,
		FWoTCreatureChunkSet& Out, FString& OutError);
	static FString CanonicalDigest(const FWoTCreatureChunkSet& Set);
	static void Install(FWoTCreatureChunkSet&& Set);
	static void SerializeCanonical(const FWoTCreatureChunkSet& Set,
		TArray<uint8>& Out);
	static bool AreEquivalent(const FWoTCreatureChunkSet& Built,
		const FWoTCreatureChunkSet& Expected, FString& OutError);

#if WITH_AUTOMATION_TESTS
	static void Reset();
#endif
};

// Activated carcass recipes selected by the pawn's CarcassType class path.
namespace WoTGoreData
{
	const TArray<FWoTBodyPart>* FindRecipe(FName CarcassTypeName);
	const FWoTCreatureChunkDefaults* GetDefaults();
}

// A flying gib (WOTCreatureChunk.uc): tossed with randomized velocity + tumble,
// bounces with randomized damping, expires ~8-12s after coming to rest. Spawned
// by AWoTCarcass::ChunkUp on the authority and replicated (the shipped
// ClientExtraChunks spawned extras client-side — a bandwidth optimization the
// port trades for simplicity; PORT_NOTES). Blood trail/decal seams are 4d/4e.
UCLASS()
class AWoTCreatureChunk : public AActor
{
	GENERATED_BODY()

public:
	AWoTCreatureChunk();

	// Configure from the recipe part + the source carcass (InitFor,
	// WOTCreatureChunk.uc:156-192 + CreateChunk WOTCarcass.uc:264-307):
	// scale by source height / player height, randomized launch + tumble.
	// ScaleMultiplier is 2.0 for the replacement chunk.
	void InitFor(const AWoTCarcass& Source, const FWoTBodyPart& Part,
	             float ScaleMultiplier = 1.0f);

	virtual void Tick(float DeltaSeconds) override;
	void HandleZoneChange(class AWoTZoneVolume* NewZone,
		class AWoTZoneVolume* OldZone);
	virtual float TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent,
	                         AController* EventInstigator, AActor* DamageCauser) override;
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UPROPERTY(VisibleAnywhere, Category = "WoT")
	TObjectPtr<UCapsuleComponent> Capsule;
	UPROPERTY(VisibleAnywhere, Category = "WoT")
	TObjectPtr<UWoTVertexAnimComponent> Mesh;

	UPROPERTY(ReplicatedUsing = OnRep_Display) FName MeshName;
	UPROPERTY(ReplicatedUsing = OnRep_Display) FName SkinName;
	UPROPERTY(ReplicatedUsing = OnRep_Display) int32 SkinSlot = 0;
	UPROPERTY(Replicated) float MeshScale = 1.0f;
	// !Part.bNoBlood: this chunk bleeds a following BloodSpray01 trail (Gibs.uc:113
	// shape) — spawned locally on every machine, torn down with the chunk.
	UPROPERTY(Replicated) bool bTrailBlood = false;
	UPROPERTY(ReplicatedUsing = OnRep_Display) uint8 PresentationGoreLevel = 0;
	UPROPERTY(Replicated) float BleedDuration = 0.0f;
	UFUNCTION() void OnRep_Display();
	UFUNCTION(NetMulticast, Reliable)
	void MulticastSpawnBounceDecal(FName Type, FVector Location,
		FRotator Rotation, float Scale, FVector Normal);

	virtual void BeginPlay() override;
	virtual void Destroyed() override;

	// WOTCreatureChunk.uc defaultproperties.
	UPROPERTY(EditDefaultsOnly, Category = "WoT") float MinBounceSpeed = 120.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT") float DeleteChunkInitialDelaySecs = 8.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT") float BaseInitForVelocity = 200.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT") float Mass = 30.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Zone") float Buoyancy = 27.0f;

	bool IsResting() const { return !bFalling; }
	bool IsSinking() const { return bSinking; }
	float GetSinkDistance() const { return SinkDistance; }
	class AWoTParticleSprayer* GetTrailForTest() const { return Trail.Get(); }

#if WITH_AUTOMATION_TESTS
	void SetVelocityForTest(const FVector& InVelocity) { Velocity = InVelocity; }
	void SetTumbleRateForTest(const FRotator& InTumbleRate)
	{
		TumbleRate = InTumbleRate;
	}
	const FVector& GetVelocityForTest() const { return Velocity; }
	bool IsBouncingForTest() const { return bBounce; }
	void SetBouncingForTest(bool bInBounce) { bBounce = bInBounce; }
	int32 GetBlockingHitCountForTest() const
	{
		return BlockingHitCountForTesting;
	}
	int32 GetLandedCountForTest() const { return LandedCountForTesting; }
	bool HasZeroPitchForTest() const { return bZeroPitch; }
	bool HasZeroYawForTest() const { return bZeroYaw; }
	bool HasZeroRollForTest() const { return bZeroRoll; }
	const FRotator& GetLastPreLandedRotationForTest() const
	{
		return LastPreLandedRotationForTesting;
	}
	const FVector& GetLastLandedNormalForTest() const
	{
		return LastLandedNormalForTesting;
	}
	bool HasSplashedForTest() const { return bSplash; }
	bool WasWaterFlattenedForTest() const { return bWaterCollisionFlattened; }
	void ConfigureBounceDecalForTest(bool bBlack)
	{
		bTrailBlood = true;
		bBlackDecals = bBlack;
	}
	void HitWallForTest(const FVector& HitNormal) { HitWall(HitNormal); }
	void CheckCleanupGateForTest() { CheckCleanupGate(); }
	bool HasCleanupTimerForTest() const
	{
		return GetWorldTimerManager().IsTimerActive(CleanupTimer);
	}
	float GetCleanupTimerRateForTest() const
	{
		return GetWorldTimerManager().GetTimerRate(CleanupTimer);
	}
#endif

private:
	void HitWall(const FVector& HitNormal);
	void Landed(const FVector& HitNormal);
	void ApplyLocalGoreVisibility();

	void SpawnTrail();
	void StartSinking();
	void CheckCleanupGate();
	void ReleaseDeathZoneCount();
	void CheckZone(class AWoTZoneVolume* Zone, float DeltaSeconds);

	// Black bounce decals for the black family (set by InitFor; authority-only use —
	// the bounce decal itself replicates).
	bool bBlackDecals = false;
	// The recipe part's bounce sound (bones ring GibBounceBone1); NAME_None = the
	// generic GibBounce1-5 random pick.
	FName BounceSound;

	UPROPERTY() FVector Velocity = FVector::ZeroVector;
	TWeakObjectPtr<class AWoTParticleSprayer> Trail;
	FRotator TumbleRate = FRotator::ZeroRotator;
	bool bFalling = true;
	// WOTCreatureChunk keeps bounce response separate from falling physics. A
	// slow collision disables reflection while gravity and wall sliding continue.
	bool bBounce = true;
	bool bZeroYaw = false;
	bool bZeroRoll = false;
	bool bZeroPitch = false;
	bool bSinking = false;
	bool bSplash = false;
	bool bWaterCollisionFlattened = false;
	float SinkRate = 0.0f;
	float SinkDistance = 0.0f;
	float PainZoneDamageScale = 0.0f;
	float NextZoneCheckTime = 0.0f;
	float BleedRate = 0.0f;
	float DeleteChunkCheckAgainDelaySecs = 0.0f;
	float BaseSinkRatePerSecMin = 0.0f;
	float BaseSinkRatePerSecMax = 0.0f;
	bool bShouldSink = false;
	bool bDeathZoneCounted = false;
	TWeakObjectPtr<class AWoTZoneVolume> DeathZone;
	bool bTakesDamage = true;
	float CumulativeDamage = 0.0f;
	FTimerHandle CleanupTimer;
#if WITH_AUTOMATION_TESTS
	int32 BlockingHitCountForTesting = 0;
	int32 LandedCountForTesting = 0;
	FRotator LastPreLandedRotationForTesting = FRotator::ZeroRotator;
	FVector LastLandedNormalForTesting = FVector::ZeroVector;
#endif
};
