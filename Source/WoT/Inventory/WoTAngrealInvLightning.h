#pragma once

#include "CoreMinimal.h"
#include "WoTAngrealInventory.h"
#include "WoTAngrealInvLightning.generated.h"

class UWoTDecreaseHealthLeech;

// Port of Angreal.AngrealInvLightning (expands AngrealInventory) — the Chain
// Lightning ter'angreal: a sustained, hold-to-cast aura that latches onto nearby
// pawns and drains their health, chaining to more victims the longer it's held.
// Spec: exports/script/Angreal/AngrealInvLightning.uc + LightningLeech.uc +
// NotifyInWaterReflector.uc.
//
// Hold-to-cast (press->CastArtifact, release->UnCastArtifact) drains charges over
// time and, every LatchDelay, tries to latch onto the nearest attachable pawn
// (within AttachmentRange, line-of-sight OR frozen-in-ice OR auto-targeted). Each
// victim gets a UWoTDecreaseHealthLeech whose rate the aura retunes each tick:
// damage scales with per-victim strain (PercentStrain^NumVictims) and distance
// (1 - dist/MaxEffectiveRange); victims past MaxEffectiveRange drop off. The
// LightningLeech wrapper folds away — the aura manages the health leeches directly.
//
// WATER (the zones payoff): a victim or the caster stepping into a water
// AWoTZoneVolume takes MaxWaterDamage and electrocutes every OTHER pawn standing in
// water within WaterDamageRadius (linear falloff). If the CASTER is electrocuted,
// the aura drops (you can't hold lightning underwater). This folds the
// NotifyInWaterReflector's FootRegion.bWaterZone poll to a per-pawn water-volume
// check here.
//
// DamageNotifier retaliation is wired via AWoTCharacter::TakeDamage ->
// NotifyOwnerDamaged: a victim who hurts the caster snaps their own link, and the
// caster hurting themselves drops the whole aura.
//
// Presentation: LaunchLS/EndLS, victim-owned LoopLS, and the persistent LSbolt
// streak, caster LightningSkinEffect/rings, and endpoint ornaments are bound.
UCLASS()
class AWoTAngrealInvLightning : public AWoTAngrealInventory
{
	GENERATED_BODY()

public:
	AWoTAngrealInvLightning();

	virtual void CastArtifact() override;    // Cast(): begin the aura
	virtual void UnCastArtifact() override;  // UnCast(): drop the aura
	virtual void Tick(float DeltaTime) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	// DamageNotifier: retaliation from a victim frees them; self-damage drops us.
	virtual void NotifyOwnerDamaged(APawn* Attacker) override;

	// defaultproperties (simulation subset).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Lightning") float AttachmentRange = 180.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Lightning") float MaxCollisionRadius = 250.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Lightning") float MaxEffectiveRange = 900.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Lightning") float MaxDamageRate = 25.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Lightning") float MaxWaterDamage = 50.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Lightning") float WaterDamageRadius = 1000.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Lightning") float PercentStrain = 0.9f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Lightning") float LatchDelay = 0.3f;

	// The health leeches on our current victims (each leech's owner == its victim).
	UPROPERTY() TArray<TObjectPtr<UWoTDecreaseHealthLeech>> Leeches;
	// The latch bolts (6a-iv): one LightningStreak02 beam per latched victim,
	// caster -> victim, torn down with the latch. Keyed by victim.
	UPROPERTY() TMap<TObjectPtr<APawn>, TObjectPtr<class AWoTBeamEffect>> Bolts;
	UPROPERTY(Transient) TObjectPtr<class AWoTPawnSkinOverlay> PlayerEffect;
	UPROPERTY(ReplicatedUsing = OnRep_Casting) bool bCasting = false;
	UFUNCTION() void OnRep_Casting();

	// AttemptLatch: latch onto one fresh nearby attachable pawn per LatchDelay.
	void AttemptLatch();
	// UpdateLeeches: retune each victim's drain by strain+distance; drop far ones.
	void UpdateLeeches();
	// A victim or the caster stepped into water — shock them and everyone in water.
	void NotifyPawnEnteredWater(APawn* WetPawn);
	// NotifyInWaterReflector.FootZoneChange: immediate production dispatch. The
	// artifact's 0.5-second poll remains the shipped backup heartbeat.
	static void NotifyFootZoneChange(APawn* WetPawn,
		const class AWoTZoneVolume* NewFootZone);
	static bool IsUsingLightning(const APawn* Pawn);

#if WITH_AUTOMATION_TESTS
	bool IsAttachableForTest(APawn* Other, APawn* Caster) const
	{
		return IsAttachable(Other, Caster);
	}
#endif

private:
	bool IsAttachable(APawn* Other, APawn* Caster) const;
	bool IsAlreadyAffected(APawn* Other) const;
	void AddVictim(APawn* Victim);
	void RemoveVictim(APawn* Victim);
	void RemoveAll();
	void DamagePawn(APawn* Victim, float Damage);
	void ElectrocutePawn(APawn* Source);
	static bool IsInWater(const APawn* Pawn);

	void ReconcileCastingPresentation();
	float ChargeTimer = 0.0f;
	float LatchTimer = 0.0f;
	float WaterPollTimer = 0.0f;
};
