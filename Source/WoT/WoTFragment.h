#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WoTRetailProjectileTarget.h"
#include "WoTFragment.generated.h"

class UCapsuleComponent;
class UWoTVertexAnimComponent;

UENUM(BlueprintType)
enum class EWoTFragmentType : uint8
{
	Wall,
	Wood,
	Glass,
};

USTRUCT()
struct FWoTFragmentRecipe
{
	GENERATED_BODY()

	UPROPERTY(EditDefaultsOnly, Category = "WoT") TArray<FName> MeshNames;
	UPROPERTY(EditDefaultsOnly, Category = "WoT") FName ImpactSound;
	UPROPERTY(EditDefaultsOnly, Category = "WoT") FName MiscSound;
	UPROPERTY(EditDefaultsOnly, Category = "WoT") float CollisionRadius = 18.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT") float CollisionHeight = 4.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT") bool bUpwardBoost = false;
};

namespace WoTFragmentData
{
	const FWoTFragmentRecipe* FindRecipe(EWoTFragmentType Type);
}

// Cosmetic debris shared by the wall, wood, and glass fragment families. The
// authority integrates the UE1 falling/bounce motion and replicates movement;
// display is a small replicated mesh-name payload, like creature chunks.
UCLASS()
class AWoTFragment : public AActor, public IWoTRetailProjectileTarget
{
	GENERATED_BODY()

public:
	AWoTFragment();

	// SkinName threads the mover's authored WallTexture/WoodTexture/GlassTexture
	// (ExplodingMover.uc:88,99,110 s.Skin = *Texture) into the replicated display
	// override; NAME_None keeps the fragment mesh's baked default look.
	void Init(EWoTFragmentType Type, float ExplosionSize,
	          const FVector& Momentum, float DrawScale, FName InSkinName = NAME_None);

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void NotifyActorBeginOverlap(AActor* OtherActor) override;
	virtual float TakeDamage(float DamageAmount, struct FDamageEvent const& DamageEvent,
		class AController* EventInstigator, AActor* DamageCauser) override;
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual bool IsRetailProjectileTarget() const override { return bDying; }

	UPROPERTY(VisibleAnywhere, Category = "WoT")
	TObjectPtr<UCapsuleComponent> Capsule;
	UPROPERTY(VisibleAnywhere, Category = "WoT")
	TObjectPtr<UWoTVertexAnimComponent> Mesh;

	UPROPERTY(ReplicatedUsing = OnRep_Display) FName MeshName;
	UPROPERTY(ReplicatedUsing = OnRep_Display) float MeshScale = 1.0f;
	UPROPERTY(ReplicatedUsing = OnRep_Display) FName SkinName;
	UFUNCTION() void OnRep_Display();

	// The three retail MP mover textures that can reach fragments. Their package
	// identities are required by the authenticated B3 texture resolver.
	static FSoftObjectPath ResolveSkinTexturePath(FName InSkinName);

	// Engine Fragment's Flying.Touch — a non-player pawn touching the fragment
	// destroys it; players are excluded. Kept as a callable leg (see .cpp on why
	// it stays dormant against live overlaps, matching retail bCollideActors=false).
	void FlyingTouch(AActor* Other);

	FVector GetFragmentVelocity() const { return Velocity; }
	// Test/spawn seam: override the randomized launch velocity (pairs with the
	// getter; used to drive the deterministic water-damp spec).
	void SetFragmentVelocity(const FVector& InVelocity) { Velocity = InVelocity; }
	bool IsResting() const { return !bFalling; }
	bool IsDying() const { return bDying; }
	bool WasWaterDamped() const { return bWaterDamped; }

private:
	void HitWall(const FVector& HitNormal);
	void StartCleanup();
	void HandleZoneTransition();

	FName ImpactSound;
	FName MiscSound;
	UPROPERTY() FVector Velocity = FVector::ZeroVector;
	FRotator TumbleRate = FRotator::ZeroRotator;
	bool bFalling = true;
	// Engine Fragment states: Flying authors bCollideActors=false and Dying calls
	// SetCollision(true,false,false), defines TakeDamage -> Destroy, and therefore
	// becomes a projectile target only after the transition.
	bool bDying = false;
	// Zone edge detectors. Retail's spawn-time eventZoneChange fires from
	// SetActorZone BEFORE eventSetInitialState, i.e.
	// while the fragment is still outside `auto state Flying` — so the global
	// (empty) Actor.ZoneChange handles it and spawning INSIDE a water/pain zone
	// does nothing. BeginPlay primes both flags from the spawn point to match;
	// only a genuine crossing afterwards is a rising edge.
	bool bInWater = false;
	bool bInPain = false;
	// Set on the water rising edge; doubles as the retail Dying-state disarm:
	// state Dying defines no ZoneChange and no Touch, so
	// after water entry the zone legs and FlyingTouch stay off for good
	// (once-EVER damp, no post-water pain destroy).
	bool bWaterDamped = false;
	FTimerHandle CleanupTimer;
};
