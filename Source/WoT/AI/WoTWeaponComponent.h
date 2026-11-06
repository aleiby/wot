#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Templates/SubclassOf.h"
#include "WoTWeaponComponent.generated.h"

class AWoTProjectile;

// EWeaponUsage (WotWeapon.uc:10): what an NPC's equipped weapon does at a given
// range to its target. The controller's Offensive-pulse router reads this to pick
// melee vs the bow (Grunt.AttemptAttack, Grunt.uc:1343-1371).
UENUM()
enum class EWoTWeaponUsage : uint8
{
	None,
	Melee,
	Projectile
};

// The NPC weapon layer — the WOT.WotWeapon usage/aim/fire model as a component on
// the pawn (players never carry weapons in WoT MP; a weapon is NPC equipment). Port
// of WotWeapon's projectile half: DetermineWeaponUsage (uc:285), UseProjectileWeapon
// (uc:376), the aim-source + target-leading (uc:449-546) and AdjustProjectileWeapon-
// dAim (uc:568). The Archer's bow (WoTPawns.ArcherBowProxy) is the one in-scope
// configuration. G19's AWoTNPCWeapon actors reuse the static classification and aim
// helpers here while owning held-item/toss/damage-skin presentation; melee damage
// still rides the pawn's MeleeAttacks set. AngrealWeaponAdapter remains folded into
// UWoTCasterComponent.
UCLASS(ClassGroup = (WoT))
class UWoTWeaponComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UWoTWeaponComponent();

	// --- Usage-model config (WotWeapon.uc:21-29 + ArcherBowProxy.uc:11-19) ---
	// The bow has no melee capability (Min/MaxMeleeRange 0), so at any real gap it
	// is projectile-only; the *Effectiveness pair breaks a melee+projectile tie
	// (uc:300) and is inert for a single-mode weapon.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Weapon") float MinMeleeRange = 0.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Weapon") float MaxMeleeRange = 0.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Weapon") float MeleeEffectiveness = 0.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Weapon") float MinProjectileRange = 0.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Weapon") float MaxProjectileRange = 0.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Weapon") float ProjectileEffectiveness = 0.0f;

	// WotWeaponProjectileType (uc:28) — the arrow the bow fires.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Weapon")
	TSubclassOf<AWoTProjectile> ProjectileClass;

	// bRequireTargetVisibility (uc:29 / CDO :848 True): no usage without LOS.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Weapon")
	bool bRequireTargetVisibility = true;

	// MissOdds (uc:40 / ArcherBowProxy 0.05): passed as the AimError to the lead
	// math. It truncates to the int 0 there (uc:545 float->int arg) and the
	// RandomizeAimParams body is commented out (uc:484-500) — so an NPC bow never
	// actually perturbs its aim. Carried for fidelity; effectively inert.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Weapon")
	float MissOdds = 0.0f;

	// The kneel-fire cadence timing (Archer.uc PerformProjectileAttack). Stand-ins
	// until the creature anims bind, same convention as FWoTMeleeAttack: the arrow
	// releases at FireNotifyTime into the draw, the episode ends at DrawTime and the
	// 0.5s Offensive pulse paces the next shot.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Weapon") float FireNotifyTime = 0.4f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Weapon") float DrawTime = 0.6f;

	// --- Usage model (WotWeapon.uc:285 DetermineWeaponUsage) ---
	// Classify this weapon's usage against a target at GoalDistance (cylinder gap),
	// visible or not. Sets the cached DeterminedWeaponUsage.
	void DetermineWeaponUsage(float GoalDistance, bool bGoalVisible);
	// Shared WotWeapon.uc:285-326 ladder used by both the legacy Archer component
	// and the rendered AWoTNPCWeapon actor. Keeping it here prevents the arsenal
	// actors from growing a second, subtly different usage implementation.
	static EWoTWeaponUsage ClassifyWeaponUsage(float GoalDistance, bool bGoalVisible,
		float MinMelee, float MaxMelee, float MeleeEffectiveness,
		float MinProjectile, float MaxProjectile, float ProjectileEffectiveness,
		bool bHasProjectileClass, bool bRequireVisibility);
	EWoTWeaponUsage GetWeaponUsage() const { return DeterminedWeaponUsage; }
	bool CanUseAsMeleeWeapon(float GoalDistance) const;
	bool CanUseAsProjectileWeapon(float GoalDistance) const;

	// --- Fire (WotWeapon.uc:376 UseProjectileWeapon) ---
	// Aim at Target (leading it if it moves) and spawn the arrow. Authority-side.
	void UseProjectileWeapon(APawn* Invoker, AActor* Target);

	// --- Aim support (WotWeapon.uc:449-568), static + pure so specs can drive it ---

	// GetAimSource (uc:506): the muzzle point — just outside the shooter's cylinder
	// toward the target, at half the shooter's half-height.
	static FVector GetAimSource(const APawn* Invoker, const FVector& TargetLocation);

	// GetLeadSpeed (uc:550): the projectile's default Speed.
	float GetLeadSpeed() const;

	// The bare intercept lead (uc:592-594): where to aim so a projectile at
	// ProjectileSpeed meets a target moving at TargetVel. LeadRandom01 is the
	// FRand() in FMin(1, 0.7 + 0.6*FRand()); pass a fixed value for a deterministic
	// result, or a live roll in [0,1].
	static FVector ComputeLeadDestination(const FVector& TargetLoc, const FVector& TargetVel,
	                                      const FVector& ProjectileStart, float ProjectileSpeed,
	                                      float LeadRandom01);

	// AdjustProjectileWeapondAim (uc:568): the full lead + occlusion-refined aim for
	// an NPC bow (AimError 0, non-player, non-splash — the player/feet-aim branches
	// port inert). LeadRandom01 < 0 rolls FRand() live; >= 0 is injected (specs).
	static FRotator AdjustProjectileWeaponAim(APawn* Invoker, AActor* Target,
	                                          float ProjectileSpeed, const FVector& ProjectileStart,
	                                          float LeadRandom01 = -1.0f);

private:
	// FireProjectile (uc:396): spawn ProjectileClass at Source facing Direction,
	// owned by / instigated by Invoker.
	void FireProjectile(APawn* Invoker, const FVector& Source, const FRotator& Direction);

	// GetAimParams (uc:449): muzzle + aim direction, leading a moving target.
	void GetAimParams(APawn* Invoker, AActor* Target, FVector& OutSource,
	                  FRotator& OutDirection, float LeadRandom01) const;

	// ShouldLeadTarget (uc:526): Invoker is a pawn, Target is a moving actor.
	static bool ShouldLeadTarget(const APawn* Invoker, const AActor* Target);

	EWoTWeaponUsage DeterminedWeaponUsage = EWoTWeaponUsage::None;
};
