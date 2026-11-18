#pragma once

#include "CoreMinimal.h"
#include "WoTGenericProjectile.h"
#include "../WoTEvents.h"
#include "WoTExpWardProjectile.generated.h"

UENUM()
enum class EWoTExpWardVisualPhase : uint8
{
	Waiting,
	FadeOut,
	FadeIn,
};

// Port of Angreal.AngrealExpWardProjectile (expands GenericProjectile) — the
// Explosive Ward: a stationary proximity landmine. Spec: exports/script/Angreal/
// AngrealExpWardProjectile.uc.
//
// Lifecycle (simulation, parity-locked):
//  - Placement: the ward has no collision from its spawn until it arms
//    (bCollideActors=False, uc:288), so placing it touches nothing. On spawn it
//    traces forward up to TraceDist and sticks to the first surface (its
//    RelativeUp = the surface normal); only that successful placement plays
//    the placement sound (uc:78-96). If no surface is found it reports
//    CreationSucceeded()=false and the cast fails.
//  - Arming: inert for ActivationTime, then it arms (collision on). While
//    waiting nothing detonates it: Touch, ProcessTouch, TakeDamage and Explode
//    are all ignored (uc:179-188). Switching collision on raises no Touch
//    (uc:186): whatever is inside the cylinder then, colliding or not, touches
//    only by a later relative move, and a ward that arms inside an armed
//    ward's cylinder is that ward's occupant in the same way.
//  - Detonation: once armed it explodes when an actor that can raise Touch
//    meets its cylinder (the caster included, never the mover it is stuck
//    to), on taking damage (shot), on a Trigger, or — in MP — automatically
//    after its LifeSpan (permanent in standalone). Explode shows the blast at
//    the ward, aligned to its surface (uc:135-146), deals the base splash
//    (DamageRadius 120, Damage 50, bHurtsOwner) and fires the Event.
//  - Network: the server alone decides detonations and announces each one to
//    the clients; a client's copy shows exactly those (PORT_NOTES).
//  - Physics: PHYS_None with bCollideWorld=False (uc:279,289). It never moves
//    itself and its cylinder blocks nothing, so no world contact is a wall
//    impact; it only overlaps, and stays a projectile target (bProjTarget).
//
// Presentation keeps the shipped Waiting/FadeOut/FadeIn glow phases. The rune
// is translucent/unlit; ScaleGlow drives MLAmbient and the point-light ramp.
// Explode spawns BlastBits; FireballExplode3 and the scorch use base effects.
UCLASS()
class AWoTExpWardProjectile : public AWoTGenericProjectile, public IWoTTriggerable
{
	GENERATED_BODY()

public:

	// The floor rune (AngrealExpWardProjectile.uc:281-285): translucent, unlit
	// ExpWard at DrawScale 0.3 with a pure-red brightness-255/radius-1 light.
	UPROPERTY(VisibleAnywhere, Category = "WoT")
	TObjectPtr<class UWoTVertexAnimComponent> DisplayMesh;
	UPROPERTY(VisibleAnywhere, Category = "WoT")
	TObjectPtr<class UPointLightComponent> Light;
	AWoTExpWardProjectile();

	// How far forward to trace for a surface to stick to.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|ExpWard") float TraceDist = 200.0f;
	// Inert time after placement before the ward arms.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|ExpWard") float ActivationTime = 1.0f;
	// ScaleGlow units per second (AngrealExpWardProjectile.uc:31-32,264).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|ExpWard") float ScaleGlowRate = 1.0f;

	// UE1 Tag/Event: a level-placed ward can be detonated by a matching trigger (Tag),
	// and on Explode it fires its Event to chain-detonate other wards (TriggerOthers).
	// Player-thrown wards leave both empty (no effect) — set on level-scripted placements.
	UPROPERTY(EditAnywhere, Category = "WoT|ExpWard") FName TagName;
	UPROPERTY(EditAnywhere, Category = "WoT|ExpWard") FName EventName;

	// Surface normal we stuck to (== UE1 RelativeUp), replicated as in the original.
	UPROPERTY(Replicated) FVector RelativeUp = FVector::UpVector;

	// Set in BeginPlay: did we find a surface to place on?
	bool bCorrectlyPlaced = false;
	// Set when the activation delay elapses: live and dangerous.
	UPROPERTY(Replicated) bool bArmed = false;
	UPROPERTY(ReplicatedUsing = OnRep_VisualPhase)
	EWoTExpWardVisualPhase VisualPhase = EWoTExpWardVisualPhase::Waiting;

	UPROPERTY(EditDefaultsOnly, Category = "WoT|Sounds")
	TSoftObjectPtr<class USoundBase> ActivateSound;

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual bool CreationSucceeded() override;
	// AngrealExpWardProjectile.uc:125 — a matching Trigger detonates the ward.
	virtual void WoTTrigger(AActor* Other, APawn* EventInstigator) override;
	// AngrealExpWardProjectile.uc:165 — the ward fizzles out in water.
	// AngrealExpWardProjectile.uc:165-169. A ward that hits water was never
	// placed: CreationSucceeded() must report failure so no charge is used.
	virtual void HitWater() override
	{
		bCorrectlyPlaced = false;
		Super::HitWater();
		Destroy();
	}
	virtual void ProcessTouch(AActor* Other, FVector HitLocation) override;
	// PHYS_None: the original never calls HitWall on a ward (uc:279,289).
	virtual void NotifyHit(UPrimitiveComponent* MyComp, AActor* Other,
	                       UPrimitiveComponent* OtherComp, bool bSelfMoved,
	                       FVector HitLocation, FVector HitNormal,
	                       FVector NormalImpulse, const FHitResult& Hit) override;
	virtual void Explode(FVector HitLocation, FVector HitNormal) override;
	virtual void LifeSpanExpired() override;
	virtual float TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent,
	                         AController* EventInstigator, AActor* DamageCauser) override;
	// AI support: the downward trajectory helper (uc:246-253).
	virtual FRotator CalculateTrajectory(const AActor* Source,
		const AActor* Destination, float ProjectileSpeed) const override;
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// Force the ward live now (arming-timer callback; public for the spec).
	// Idempotent per machine: a client's copy also arms when the server's
	// arming reaches it first.
	void Activate();

protected:
	// AI support: a fixed 600-unit maximum range (uc:256-260).
	virtual float GetClassMaxRange() const override { return 600.0f; }
	// GenericProjectile.PreBeginPlay runs only after a successful placement
	// (uc:78-96), so BeginPlay calls the base ServerInit itself.
	virtual void ServerInit() override {}

private:
	// Could Other raise the original's Touch on this ward if it collided?
	// (Not level geometry, not the ward's base or an actor based on it, and a
	// Touch target.) The arming snapshot keeps such actors whatever their
	// collision, so one whose collision returns later is still an occupant.
	bool IsPotentialContact(const AActor* Other) const;
	// A potential contact that is in the collision hash now: it can raise
	// Touch. Shared by both contact paths and rechecked before each dispatch.
	bool IsEligibleContact(const AActor* Other) const;
	// Other's cylinder meets the ward's, sized as it is while Other collides.
	bool MeetsCylinder(const AActor* Other) const;
	// The server's detonation, shown on every client (the persistent channel
	// delivers it before the ward's teardown).
	UFUNCTION(NetMulticast, Reliable) void MulticastDetonation();
	UFUNCTION() void OnRep_VisualPhase();
	void SetVisualPhase(EWoTExpWardVisualPhase NewPhase);
	void ApplyVisualPresentation();
	void PlayActivateSoundOnce();

	FTimerHandle ArmTimer;
	// One-shot Explode guard = retail's `singular` TakeDamage/Trigger (uc:119,125):
	// chained wards splash-detonate each other; without this that chain recurses.
	bool bDetonated = false;
	// This machine's copy has switched its collision on (Activate ran).
	bool bActivated = false;
	// True only while Activate switches the collision on: overlaps that begin
	// then come from no movement and are no Touch (uc:186).
	bool bSwitchingCollisionOn = false;
	// Client copy: the server's detonation arrived (MulticastDetonation), the
	// only detonation a client shows.
	bool bServerDetonated = false;
	bool bActivateSoundPlayed = false;
	float FullLightIntensity = 0.0f;
	// UE1 SetCollision(true) does not synthesize Touch for actors already inside.
	// Potential contact -> actor-minus-ward separation at arming (or when it
	// began overlapping without a move). Either participant moving relative to
	// the other synthesizes the deferred Touch, provided the actor can raise
	// Touch at that moment. Server only: clients never detonate.
	TMap<TWeakObjectPtr<AActor>, FVector> ArmingOccupants;
};
