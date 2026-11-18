#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "WoTTouchReflectorDescription.h"
#include "WoTReflector.generated.h"

struct FWoTDamageEffectSpec;

// Port of WOT.Reflector (LegendActorComponent : Actor) — the base for the
// effect-interception units the original chains over a pawn's reflectable
// functions (TakeDamage, ProcessEffect, …). Spec: exports/script/WOT/Reflector.uc.
//
// UE5 mapping: a reflector is a UActorComponent on the host pawn, mirroring the
// UWoTLeech component model (source refs, lifespan/expiry, bRemoveExisting). The
// host consults its priority/install-ordered chain at every reflectable fold
// point. Concrete reflectors override only the hook they intercept.
//
// The bDisplayIcon status-icon lifecycle and the retail fading expiry warning
// are both live.
UCLASS(Abstract, Blueprintable)
class UWoTReflector : public UActorComponent
{
	GENERATED_BODY()

public:
	UWoTReflector();

	// The angreal responsible for installing us (its elements drive IgnoresEffect).
	UPROPERTY(Replicated) TObjectPtr<class AWoTAngrealInventory> SourceAngreal;
	UPROPERTY() TObjectPtr<APawn> ReflectorInstigator;

	// Live remaining seconds; 0 = permanent (Actor.LifeSpan in the original).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Reflector") float Lifespan = 0.0f;
	// Execution order among reflectors (higher runs first).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Reflector") uint8 Priority = 128;
	// Reflector.uc:264-271 equal-priority insertion rule: a later Install runs
	// earlier. Stamped from a process-monotonic counter by Install().
	UPROPERTY(Replicated) uint64 InstallSeq = 0;
	// If set, Install first uninstalls all reflectors of this class.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Reflector") bool bRemoveExisting = false;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Reflector") bool bDeleterious = false;
	// May a cleanse (Remove Curse) strip us / may Swap Places transfer us?
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Reflector") bool bRemovable = true;
	// Reflector.uc base default is True; invisible internal reflectors override it.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Reflector") bool bDisplayIcon = true;
	// Reflector.uc mirrors Leech's owner-only warning during the final two
	// seconds, repeating once per second at half the previous volume.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Reflector") float NotifyExpireTime = 2.0f;
	float NotifyExpireTimer = 0.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Reflector") float NotifyExpireResolution = 1.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Reflector") float NotifyExpireVolume = 1.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Reflector")
	TSoftObjectPtr<class USoundBase> NotifyExpireSound;
	// Optional frame override; otherwise AddIconInfo uses SourceAngreal's frame.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Reflector")
	TSoftObjectPtr<class UTexture2D> StatusIconFrame;
	// True if a projectile installed us (SwapPlaces only transfers non-projectile ones).
	UPROPERTY() bool bFromProjectile = false;
	// Set by Swap Places around a transfer re-install: retail moves a reflector with
	// a direct Install (no ProcessEffect), so the new host's shields don't re-check.
	bool bTransferring = false;

	// --- lifecycle (spec names) ---
	virtual void Install(APawn* NewHost);
	// Remove only this producer's HUD entry from its current host. Swap Places
	// uses this before reparenting, while retaining the live reflector component.
	void RemoveStatusIconFromCurrentHost();
	void Uninstall();
	virtual void Expired();
	void SetSourceAngreal(class AWoTAngrealInventory* Source);
	void SetPriority(uint8 NewPriority);
	virtual bool BuildTouchReflectorDescription(
		FWoTTouchReflectorDescription& Out) const { return false; }

	// --- fold points (override the one you intercept) ---
	// TakeDamage interception: return the (possibly reduced) damage. Non-const —
	// a hit-point shield mutates its pool and may Uninstall on depletion.
	virtual int32 ModifyDamage(int32 Damage, FName WoTDamageType) { return Damage; }
	// True only when this invocation consumed the chain instead of passing a
	// (possibly zero) value onward. Pawn.TakeDamage must not run in that case.
	virtual bool StopsDamageChain(int32 DamageAfterMe) const { return false; }
	// ProcessEffect interception: true if an effect from Source should be thrown
	// away. bDeleterious flags whether the effect harms the host — an anti-magic
	// dome (NoBadEffects) blocks ALL harmful effects; element shields key off Source.
	virtual bool IgnoresEffect(const class AWoTAngrealInventory* Source,
	                           bool bEffectDeleterious) const { return false; }
	// ProcessEffect interception: true when this reflector handles the complete
	// effect and the priority chain must stop before the default Invoke bottom.
	virtual bool ConsumesEffect(const FWoTDamageEffectSpec& /*Effect*/) { return false; }
	// UseAngreal/CeaseUsingAngreal interception: true if this reflector forbids the
	// host from casting at all (NoCast — the anti-magic dome). Default: allowed.
	virtual bool BlocksCasting() const { return false; }
	// Non-blocking cast interceptors may consume Use/Cease and perform another
	// action. Distant Eye runs at priority 128 after NoCast's priority-255 gate;
	// first true in AWoTCharacter's ordered reflector chain wins.
	virtual bool InterceptUseAngreal() { return false; }
	virtual bool InterceptCeaseUsingAngreal() { return false; }
	// HUD duration surface. Distant Eye reports eye health.
	virtual float GetInitialDuration() const { return GetDuration(); }
	virtual float GetDuration() const { return Lifespan; }
	// ChargeUsed interception: the host just spent a charge on Ang. Default: no-op.
	virtual void OnChargeUsed(class AWoTAngrealInventory* Ang) {}
	// FindBestTarget interception: true if this reflector overrides targeting
	// (AutoTarget: nearest reachable, no FOV/LOS). Default: no override.
	virtual bool OverridesTargeting() const { return false; }
	// Projectile-touch interception: true if a weave that strikes the host bounces
	// back at its originator instead of hitting (ReturnToSender). Default: no.
	virtual bool ReflectsProjectiles() const { return false; }
	// Fork: a weave still hits the host, but a duplicate is launched back at the
	// originator (ForkRTS). Default: no.
	virtual bool ForksProjectiles() const { return false; }
	// NotifyHitByAngrealProjectile interception: a weave just struck the host.
	// Return true to absorb it (Absorb steals the firing artifact + cancels the
	// hit). Default: not handled.
	virtual bool OnHitByProjectile(class AWoTAngrealProjectile* Proj) { return false; }
	// Retail IsAffectable name lists live on the concrete touch interceptor.
	virtual bool IsProjectileAffectable(const AActor* Proj) const { return IsValid(Proj); }
	// Production-path presentation hooks. The dispatch calls these only when the
	// reflector actually consumed/reflected/forked the incoming effect.
	virtual void OnProjectileFeedback() {}
	virtual void OnEffectIgnoredFeedback() {}

	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
	                           FActorComponentTickFunction* ThisTickFunction) override;
	virtual void OnComponentDestroyed(bool bDestroyingHierarchy) override;
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

protected:
	void NotifyOwnerExpiring();
};
