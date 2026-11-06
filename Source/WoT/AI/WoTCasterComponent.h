#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Templates/SubclassOf.h"
#include "WoTCasterComponent.generated.h"

class AWoTAngrealInventory;
class UWoTInventoryComponent;

// One weave in a caster's loadout (AngrealUser.uc AII_* parallel arrays,
// folded to a struct per slot). Sentinel -1 = keep the weave class's own
// default (the SetValueWithSentinal pattern, AngrealUser.uc:67-77).
USTRUCT()
struct FWoTWeaveSlot
{
	GENERATED_BODY()

	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	TSubclassOf<AWoTAngrealInventory> WeaveClass;

	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	float Priority = -1.0f;

	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	int32 MinInitialCharges = -1;

	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	int32 MaxInitialCharges = -1;

	// AII_Min/MaxChargesInGroup are independent from initial stock. Sentinel -1
	// preserves the inventory class defaults, exactly like the parallel retail
	// arrays (AngrealUser.AddDefaultInventoryItems).
	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	int32 MinChargesInGroup = -1;

	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	int32 MaxChargesInGroup = -1;

	// AngrealUser.uc:119-137 AII_MinChargeGroupInterval. -1 preserves the
	// inventory class default; any authored value replaces it per caster slot.
	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	float MinChargeGroupInterval = -1.0f;
};

// The weave-casting half of a caster NPC (Sister/Myrddraal/…): builds the
// angreal loadout at spawn and drives the SAME inventory cast chain players
// use. Port of AngrealUser's policy + the AngrealWeaponAdapter selection/
// usage cycle (AngrealUser.uc:87 AddDefaultInventoryItems, :214
// UseSpecificAngreal; AngrealWeaponAdapter.uc:239 DetermineOffensiveUsage,
// :319 SelectOffensiveAngreal, :81 ChargeUsed). The defensive reflex and
// continuous-cast presentation are live. UseMeteor and NPC-RespawnLeech remain
// outside this component's implemented policy surface (PORT_NOTES).
UCLASS(ClassGroup = (WoT))
class UWoTCasterComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UWoTCasterComponent();

	// The loadout (per-creature data in slice 5; tests configure directly).
	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	TArray<FWoTWeaveSlot> Weaves;

	// Minimum seconds between weave selections (config on AngrealUser;
	// per-creature data — 0 = no gate).
	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	float MinOffensiveUsageInterval = 0.0f;

	// AngrealWeaponAdapter WotWeapon-compatible usage config. These are carried
	// independently of HoldRange so LegionWeaponAdapter's 3200 CDO is expressible
	// without implementing its seeker/stomp inventory behavior (G24).
	UPROPERTY(EditDefaultsOnly, Category = "WoT") float MinProjectileRange = 0.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT") float MaxProjectileRange = 10000.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT") bool bRequireTargetVisibility = false;

	// Weave hold range: the standoff the caster's tracking bands hold at (the
	// Visible/Reachable band StandoffDistance). Once the goal's cylinder gap is
	// within this, the caster stops closing and casts from range rather than
	// chasing to point-blank (PORT_NOTES slice 3a). Per-creature data in slice 5.
	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	float HoldRange = 400.0f;

	// Minimum seconds between DEFENSIVE weave selections (AngrealUser config,
	// copied to AngrealWeaponAdapter.MinDefensiveUsageInterval; 0 = no gate).
	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	float MinDefensiveUsageInterval = 0.0f;

	// The AngrealUser passive defensive detector (DefensiveDetectorAngrealUser CDO):
	// a CollisionRadius*8 x CollisionHeight*3 perimeter cylinder scanning for incoming
	// AngrealProjectiles. Only casters run passive detection + the weave reflex, so the
	// detector folds onto the caster rather than a standalone notifier object. Shipped
	// detection is Touch-driven; the port polls this cylinder on the 0.05s Defensive
	// pulse (PORT_NOTES divergence — observationally equivalent within one pulse).
	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	float DefensivePerimeterRadiusFactor = 8.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	float DefensivePerimeterHeightFactor = 3.0f;

	// Highest-effective-priority offensive weave for this victim, honoring
	// the usage interval; on success the weave's charge group is initialized
	// and it becomes the pending cast item.
	AWoTAngrealInventory* SelectOffensiveWeave(AActor* Victim);

	// The nearest incoming AngrealProjectile inside the perimeter that survives the
	// rejection ladder (the native DefensiveDetector's RejectOffender) AND the seeker gate
	// (AngrealUser.uc:248 — a seeker offends only when it targets us, or has none).
	// Null when nothing qualifies. This is CheckForOffending folded to a poll.
	AActor* FindIncomingWeave() const;

	// Highest-effective-priority DEFENSIVE weave against this offender, honoring the
	// defensive usage interval (SelectDefensiveAngreal, AngrealWeaponAdapter.uc:282).
	// The victim is the incoming projectile itself, so GetEffectivePriority routes to
	// GetDefensivePriority (element-counter test). On success inits the charge group.
	AWoTAngrealInventory* SelectDefensiveWeave(AActor* Offender);

	// Press the button: select the weave in the inventory and UseAngreal —
	// the UseSpecificAngreal flow (AngrealUser.uc:214).
	void BeginCast(AWoTAngrealInventory* Weave);

	// Release the button.
	void Release();

	bool IsCasting() const { return ActiveWeave != nullptr; }
	// AngrealWeaponAdapter.IsCasting: only zero-interval automatic items own the
	// sustained AttackRun presentation while their usage monitor is active.
	bool IsContinuousCasting() const;

	// AngrealUser.ChargeUsed. Inventory dispatch calls this synchronously from the
	// real UseCharge path, before the adapter's next polling tick.
	void NotifyChargeUsed(AWoTAngrealInventory* Weave);

	// AngrealUser.NotifyHitByAngrealProjectile: Ice contact interrupts the active
	// charge group before reflectors decide whether the projectile is accepted.
	void InterruptForIceContact();
	// AngrealUser.Killed: release a live volley when its victim dies.
	void NotifyTargetKilled(AActor* Victim);

#if WITH_AUTOMATION_TESTS
	bool CanContinueVolleyForTest(AActor* Victim) const
	{
		return CanContinueVolley(Victim);
	}
#endif

	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
	                           FActorComponentTickFunction* ThisTickFunction) override;

private:
	// Spawn + grant each weave slot, applying sentinel overrides.
	void BuildLoadout();

	UWoTInventoryComponent* GetInventory() const;
	bool CanContinueVolley(AActor* Victim) const;
	void UpdateNPCProjectileAim();
	void BeginContinuousCastPresentation();
	void StampChargeUsedPresentation();
	void EndCastPresentation();

	// RejectOffender ladder for the AngrealUser detector (native DefensiveDetector):
	// true if the offender is rejected. The AngrealUser variant ships bRejectResponded
	// false (its dodge path uses bRecollect=true) and bRestrictCollectionToFOV false,
	// so rules 1 (responded) and 3 (FOV) are inert here; the live rules are 4 (moving
	// toward us) and 5 (arrival time in the response window). Rule 2's Engine.Fragment
	// rejection is subsumed by the AngrealProjectile collect filter.
	bool RejectOffender(AActor* Offender) const;

	// The response window [DR_MinResponseTime, DR_MaxResponseTime] from the
	// DefensiveDetectorAngrealUser DefensiveResponses[0] CDO.
	static constexpr float DR_MinResponseTime = 0.01f;
	static constexpr float DR_MaxResponseTime = 5.0f;

	UPROPERTY()
	TObjectPtr<AWoTAngrealInventory> ActiveWeave;
	// AngrealWeaponAdapter.AdaptedItem survives button release. Selection skips
	// it until ChargeUsed discovers that it cannot currently cast again.
	UPROPERTY()
	TObjectPtr<AWoTAngrealInventory> AdaptedWeave;

	int32 LastSeenCharges = 0;
	// Snapshot of the active weave's CastUseCount at BeginCast — a shot fired when it
	// changes, even for a 0-cost launcher that never drops CurCharges (the Legion seeker).
	int32 LastSeenUseCount = 0;
	float LastOffensiveUsageTime = -1e9f;
	float LastDefensiveUsageTime = -1e9f;
	bool bOwnsAttackRunPresentation = false;
};
