#pragma once

#include "CoreMinimal.h"
#include "WoTDecoration.h"
#include "WoTFragment.h"
#include "WoTBreakableDecoration.generated.h"


// Port of BreakableDecoration (exports/script/WOT/BreakableDecoration.uc, G36)
// — the Frag()-based breakable prop family. The only MP authors are Arena_06's
// BarrelTap x8 (WoodFragments debris). Unlike the exploding wall/mover pair,
// TakeDamage has NO damage-type gate (uc:76-110 — no ActivatedBy): weaves,
// melee, anything chips Health, so these ARE genuinely breakable in MP. On a
// fatal hit the engine Frag helper (Decoration.Frag)
// sheds exactly 12 fragments of the authored family at the actor location; a
// non-fatal hit knocks the prop loose (PHYS_Falling). Breakables get real
// blocking collision (retail bBlockActors/bBlockPlayers/bProjTarget all true);
// plain decorations apply their own effective retail collision flags.
UCLASS()
class AWoTBreakableDecoration : public AWoTDecoration
{
	GENERATED_BODY()

public:
	AWoTBreakableDecoration();
	virtual void ConfigureRuntimeCollision(bool bInCollideActors,
		bool bInBlockActors, bool bInBlockPlayers, float Radius,
		float HalfHeight) override;

	// The inherited flat-ended cylinder carries the effective retail radius and
	// half-height without a capsule clamp or box-corner over-block.
	// BreakableDecoration CDO Health=10 (uc:119); BarrelTap stamps 20.
	UPROPERTY(EditAnywhere, Category = "WoT|Break") int32 Health = 10;
	// fragmentClass folded to the fragment-family enum (BarrelTap authors
	// WOT.WoodFragments); fragmentSize is the per-fragment DrawScale base.
	UPROPERTY(EditAnywhere, Category = "WoT|Break")
	EWoTFragmentType FragmentType = EWoTFragmentType::Wood;
	UPROPERTY(EditAnywhere, Category = "WoT|Break") float FragmentSize = 1.0f;
	// BaseChange crush threshold (uc:13,23-27): the ONLY landing damage for an
	// UNKNOCKED fall (floor HitWall is bBounce-gated in UE1 — see Tick).
	UPROPERTY(EditAnywhere, Category = "WoT|Break") int32 MinDamageVelocityZ = 500;
	// Read by Decoration.Frag: gates Frag ONLY (damage still chips Health).
	UPROPERTY(EditAnywhere, Category = "WoT|Break") bool bOnlyTriggerable = false;
	UPROPERTY(EditAnywhere, Category = "WoT|Break") FName EventName;
	// Retail Mass=50, BarrelTap 60. Only the note-skipped BaseChange damage math
	// consumed it; carried as authored data.
	UPROPERTY(EditAnywhere, Category = "WoT|Break") float Mass = 50.0f;
	UPROPERTY(EditAnywhere, Category = "WoT|Break") bool bPushable = true;

	virtual void PostInitializeComponents() override;
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual float TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent,
		AController* EventInstigator, AActor* DamageCauser) override;
	virtual void NotifyHit(UPrimitiveComponent* MyComp, AActor* Other,
		UPrimitiveComponent* OtherComp, bool bSelfMoved, FVector HitLocation,
		FVector HitNormal, FVector NormalImpulse,
		const FHitResult& Hit) override;

	// Decoration.Frag with NumFrags=12
	// (BreakableDecoration.uc:102).
	void Frag(APawn* EventInstigator, const FVector& Momentum);
	// Enter the falling mode (retail SetPhysics(PHYS_Falling)) with an explicit
	// launch velocity; the authority integrates gravity in Tick.
	void StartFalling(const FVector& InVelocity);
	// Projection happens before a joining world's client net mode is established.
	// Delay an authored initial PHYS_Falling until BeginPlay can make that choice.
	void ConfigureInitialFalling(bool bInStartsFalling)
	{
		bStartsFalling = bInStartsFalling;
	}

	bool HasFragged() const { return bFragged; }
	bool IsFallingLoose() const { return bFallingLoose; }
	bool IsKnocked() const { return bKnocked; }
	FVector GetFallVelocity() const { return FallVelocity; }

private:
	void ApplyBlockingCollision();
	// Port fold of the retail Base/BaseChange support chain: when this prop is
	// destroyed, anything resting on top of it starts falling.
	void ReleaseSupportedDecorations();

	FVector FallVelocity = FVector::ZeroVector;
	bool bFallingLoose = false;
	bool bStartsFalling = false;
	// The retail bBounce: set by the non-fatal knock (uc:108), cleared by the
	// first blocking contact (uc:73). Floor contacts run the HitWall shatter
	// gate ONLY while set (UE1 physFalling); unknocked falls Land
	// damage-free bar the BaseChange crush gate.
	bool bKnocked = false;
	// One-shot latch: Frag ends in Destroy(); guards re-entry until the pending
	// kill lands (the AWoTExplodingWall bExploded precedent).
	bool bFragged = false;
};
