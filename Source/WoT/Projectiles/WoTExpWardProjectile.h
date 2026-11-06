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
//  - Placement: on spawn it traces forward up to TraceDist and sticks to the
//    first surface (its RelativeUp = the surface normal); if none is found it
//    reports CreationSucceeded()=false and the cast fails.
//  - Arming: inert (no collision) for ActivationTime, then it arms (collision on).
//  - Detonation: once armed it explodes on pawn proximity (Touch), on taking
//    damage (shot), or — in MP — automatically after its LifeSpan (permanent in
//    standalone). Explode deals the base splash (DamageRadius 120, Damage 50,
//    bHurtsOwner) and fires the Event.
//
// Presentation keeps the shipped Waiting/FadeOut/FadeIn glow phases. The rune
// is translucent/unlit; ScaleGlow drives MLAmbient and the point-light ramp.
// BlastBits remains deferred; FireballExplode3 and the scorch use base effects.
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
	virtual void Explode(FVector HitLocation, FVector HitNormal) override;
	virtual void LifeSpanExpired() override;
	virtual float TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent,
	                         AController* EventInstigator, AActor* DamageCauser) override;
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// Force the ward live now (arming-timer callback; public for the spec).
	void Activate();

private:
	UFUNCTION() void OnRep_VisualPhase();
	void SetVisualPhase(EWoTExpWardVisualPhase NewPhase);
	void ApplyVisualPresentation();
	void PlayActivateSoundOnce();

	FTimerHandle ArmTimer;
	// One-shot Explode guard = retail's `singular` TakeDamage/Trigger (uc:119,125):
	// chained wards splash-detonate each other; without this that chain recurses.
	bool bDetonated = false;
	bool bActivateSoundPlayed = false;
	float FullLightIntensity = 0.0f;
	// UE1 SetCollision(true) does not synthesize Touch for actors already inside.
	// Actor -> actor-minus-ward separation at arming. Either participant moving
	// relative to the other must synthesize the deferred Touch.
	TMap<TWeakObjectPtr<AActor>, FVector> ArmingOccupants;
};
