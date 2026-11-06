#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Math/RandomStream.h"
#include "AI/WoTPathNodeIterator.h"
#include "WoTBeamEffect.generated.h"

class UInstancedStaticMeshComponent;
class USkeletalMesh;
class UWoTVertexAnimComponent;

// Legend's Line/Streak beams (Line.uc/Streak.uc + their native render iterators,
// re-derived): a chain of segments laid every SegmentLength from Start to End,
// re-laid each tick (the endpoints can be actors — the lightning latch follows
// caster and victim). Two shipped configs:
//  - "BalefireLine"      (Line):   Sparks01 sprite segments every 18uu at
//    DrawScale 2, fading ScaleGlow = LifeSpan/1.2 (BalefireLine.uc). One-shot.
//  - "LightningStreak02" (Streak): LSbolt mesh segments every 64uu, each with a
//    per-render-frame random skin from its decoded retail Textures[] array;
//    lives while the latch holds.
// The actor replicates (endpoints + config); every machine lays its own segments
// locally. The shipped path-node bending (UStreakRI CanDirectlyConnect fallback,
// arcing blocked bolts through nav nodes) is retained for LightningStreak02.
UCLASS()
class AWoTBeamEffect : public AActor
{
	GENERATED_BODY()

public:
	AWoTBeamEffect();

	UPROPERTY(Replicated) FName Config;
	UPROPERTY(Replicated) FVector_NetQuantize Start;
	UPROPERTY(Replicated) FVector_NetQuantize End;
	// The latch followers (Line.uc StartActor/EndActor): when set they override
	// Start/End each tick (caster eye -> victim center).
	UPROPERTY(Replicated) TObjectPtr<AActor> StartActor;
	UPROPERTY(Replicated) TObjectPtr<AActor> EndActor;

	// UE1 display-lighting properties consumed by UWoTMeshLight through
	// reflection for the LightningStreak02 vertex-animated segment owner.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Display") float ScaleGlow = 1.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Display") float AmbientGlow = 250.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Display") bool bUnlit = true;

	virtual void Tick(float DeltaSeconds) override;
	virtual void Destroyed() override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	int32 GetVisibleSegmentCount() const { return VisibleSegmentCount; }
	int32 GetStreakTextureAssignmentCount() const
		{ return StreakTextureAssignmentCount; }
	const TArray<FVector>& GetLastSegmentOrigins() const { return LastSegmentOrigins; }

protected:
	virtual void BeginPlay() override;

private:
	void LayLine(const FVector& A, const FVector& B);
	void LayStreak(const FVector& A, const FVector& B);

	// Sprite line: one ISM pair (the sprayer/billboard render path — the particle
	// masters read PerInstanceCustomData 0 as ScaleGlow).
	UPROPERTY(Transient) TObjectPtr<UInstancedStaticMeshComponent> AddQuads;
	UPROPERTY(Transient) TObjectPtr<UInstancedStaticMeshComponent> ModQuads;
	// Mesh streak: pooled segment components (the RI's reusable node list).
	UPROPERTY(Transient) TArray<TObjectPtr<UWoTVertexAnimComponent>> Segments;
	UPROPERTY(Transient) TObjectPtr<class AWoTMeshEffect> EndpointImpact;
	UPROPERTY(Transient) TObjectPtr<class AWoTParticleSprayer> EndpointSparks;
	float ImpactRollUnits = 0.0f;
	FRandomStream CosmeticRandom;

	bool bConfigured = false;    // config resolves on first Tick (set post-spawn)
	bool bStreak = false;        // mesh segments vs sprite quads
	float SegmentLength = 18.0f;
	float SpriteScale = 2.0f;    // BalefireLine DrawScale
	float FadeTime = 0.0f;       // >0: ScaleGlow = remaining/FadeTime (balefire)
	float Remaining = 0.0f;
	int32 VisibleSegmentCount = 0;
	int32 StreakTextureAssignmentCount = 0;
	TArray<FVector> LastSegmentOrigins;
	bool bLoggedStreakOverflow = false;
	// Per-tick lay scratch, retained across ticks so the every-frame streak
	// re-lay (retail RI behavior) does not reallocate or re-resolve assets.
	TArray<FRotator> PlannedRotations;
	FWoTPathNodeIterator StreakIterator;
	// Only the component-heavy mesh streak is capped. Lightning's shipped
	// 64uu segments span at most MaxEffectiveRange=900uu (15 segments); 512
	// leaves ample headroom without truncating the uncapped sprite Line path.
	static constexpr int32 MaxStreakSegments = 512;
};
