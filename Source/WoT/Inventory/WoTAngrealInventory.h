#pragma once

#include "CoreMinimal.h"
#include "WoTInventory.h"
#include "WoTAngrealInventory.generated.h"

// Used for icon Timers
UENUM()
enum class EWoTDurationType : uint8
{
	DT_Charge,
	DT_Lifespan,
	DT_Shield,
	DT_None,
};

// Port of WOT.AngrealInventory (1:1) — the artifact base: charge bookkeeping,
// taint, element/rarity/type identity, cast/uncast interface, and the
// elemental damage-type name statics. Spec: exports/script/WOT/
// AngrealInventory.uc. Cast()/UnCast() are named CastArtifact()/
// UnCastArtifact() (a member named Cast shadows UE's Cast<> template inside
// every artifact subclass). The pickup AppearEffect rings are live (P05);
// puzzle-cue sprayers are SP-scope (zero MP authors). The NPC respawn leech is
// live (GoEmpty -> UWoTLatentGiveToLeech). The bDisplayIcon cast-timer lifecycle
// feeds the owning character's replicated status-icon array.
// The Battle team filter and damage-effect dispatch have runtime folds; taint
// self-damage remains a direct TakeDamage per the ratified convention.
UCLASS(Abstract)
class AWoTAngrealInventory : public AWoTInventory
{
	GENERATED_BODY()

public:
	AWoTAngrealInventory();

	UPROPERTY(EditDefaultsOnly, Category = "WoT|Angreal")
	EWoTDurationType DurationType = EWoTDurationType::DT_None;

	// not per-subclass tunables in the original either — global taint damage
	int32 TaintCommonDamage = 5;
	int32 TaintUncommonDamage = 10;
	int32 TaintRareDamage = 20;

	// Any combination of the five elements.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Elements") bool bElementFire = false;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Elements") bool bElementWater = false;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Elements") bool bElementAir = false;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Elements") bool bElementEarth = false;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Elements") bool bElementSpirit = false;

	// Exactly one rarity.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Rarity") bool bCommon = false;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Rarity") bool bUncommon = false;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Rarity") bool bRare = false;

	// AngrealInventory.uc BecomePickup (uc:632-646): a visible pickup (fresh
	// place, pool respawn, drop) grows the rarity-colored AppearEffect rings;
	// taking the item destroys them (BecomeItem, uc:667-671). Netmode gate:
	// standalone uses bSPAppearEffect (base False), everything else
	// bMPAppearEffect (base True) — the flags below with the charge block.
	// The rarity corona pairs (uc:1332-1337; no shipped subclass overrides them).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Rarity")
	FName CommonTop = TEXT("CyanCorona");
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Rarity")
	FName CommonBottom = TEXT("PurpleCorona");
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Rarity")
	FName UnCommonTop = TEXT("AOrangeCorona");
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Rarity")
	FName UnCommonBottom = TEXT("ABlueCorona");
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Rarity")
	FName RareTop = TEXT("APinkCorona");
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Rarity")
	FName RareBottom = TEXT("APurpleCorona");

	virtual void BecomePickup() override;
	virtual void BecomeItem() override;

	// The live pickup rings (server-side handle; AE itself replicates).
	UPROPERTY(Transient) TObjectPtr<class AWoTAppearEffect> AppearEffect;

	// Any combination of hand types.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Types") bool bOffensive = false;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Types") bool bDefensive = false;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Types") bool bCombat = false;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Types") bool bTraps = false;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Types") bool bInfo = false;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Types") bool bMisc = false;

	UPROPERTY(EditDefaultsOnly, Category = "WoT|Angreal") float RoundsPerMinute = 60.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Angreal") bool bRestrictsUsage = false;

	// EditAnywhere (not DefaultsOnly): fixed MP authors override these bounds
	// (Arena_06/Battle2_05 T3Ds). G15 established that Python cannot stamp an
	// EditDefaultsOnly property into a generated map actor; this follows G38's
	// bMPAppearEffect precedent.
	UPROPERTY(EditAnywhere, Category = "WoT|Charges") int32 MinInitialCharges = 1;
	UPROPERTY(EditAnywhere, Category = "WoT|Charges") int32 MaxInitialCharges = 1;
	// Standalone charge overrides follow Difficulty's 0..2 ordering
	// (AngrealInventory.uc:95).
	// Battle2_03 authors all three slots on a fixed Heal; keep the same G15
	// per-instance Python visibility rule as the bounds above.
	UPROPERTY(EditAnywhere, Category = "WoT|Charges") int32 SPInitialCharges[3] = {0, 0, 0};
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Charges") int32 MaxCharges = 999;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Charges") int32 ChargeCost = 1;

	UPROPERTY(EditDefaultsOnly, Category = "WoT|Angreal") bool bSPAppearEffect = false;
	// G15 trap: EditDefaultsOnly fields are Python-invisible for per-instance umap
	// stamps. G38's fixed AndilayRoot importer must preserve authored False values.
	UPROPERTY(EditAnywhere, Category = "WoT|Angreal") bool bMPAppearEffect = true;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Angreal") bool bPuzzleRespawn = false;

	UPROPERTY(EditDefaultsOnly, Category = "WoT|AI") float Priority = 0.0f;
	// Localized (WoT.int [AngrealInventory] and each subclass section): FWoTRetailText.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Angreal") FString FailMessage;

	// class names we are never allowed to target (original UnrealScript names)
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Angreal") FName NonTargetableTypes[2];
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Angreal") bool bTargetsFriendlies = true;

	UPROPERTY(EditDefaultsOnly, Category = "WoT|Display") bool bDisplayIcon = false;

	// AngrealInventory.uc:106-108 — the first-new-artifact notification. FadeTime arms it
	// per placed instance (> 0 to take effect; the SP missions set 7.0 in map data) and is
	// zeroed after firing so it is one-shot. Texture/sound carry the shipped defaults:
	// Texture'WOT.NotifyPickup.I_NotifyPickup' as its HUD-registry id, and
	// "WOT.NotifyPickup.NotifyPickupSound" as the importer-flattened Pkg/Name sub-path.
	UPROPERTY(EditAnywhere, Category = "WoT|Display")
	float NotifyNewPickupFadeTime = 0.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Display")
	FName NotifyNewPickupTexture = FName(TEXT("I_NotifyPickup"));
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Sounds")
	FName NotifyNewPickupSound = FName(TEXT("WOT/NotifyPickupSound"));

	UPROPERTY(EditDefaultsOnly, Category = "WoT|Sounds")
	TSoftObjectPtr<USoundBase> UseChargeSound;
	// TaintedSoundName/FailSoundName lazy-loads collapse into the soft ptrs
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Sounds")
	TSoftObjectPtr<USoundBase> TaintedSound;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Sounds")
	TSoftObjectPtr<USoundBase> FailSound;

	// ---- status ----
	UPROPERTY(Replicated) bool bTainted = false;      // COND_OwnerOnly per original
	UPROPERTY() TObjectPtr<APawn> TaintInstigator;
	UPROPERTY(Replicated) int32 CurCharges = 0;
	// Monotonic count of successful uses (UseCharge). A caster observes this to detect a
	// shot even when the weave spends no charge (a ChargeCost=0 launcher — the Legion
	// seeker — never drops CurCharges), so its single-shot cast still ends. Server-side.
	UPROPERTY() int32 CastUseCount = 0;
	UPROPERTY() TObjectPtr<AActor> LastOwner;          // last pawn who held us
	UPROPERTY() TObjectPtr<AActor> Target;             // AI victim (Actor.Target)

	// AI charge-group pacing (AddDefaultInventoryItems tunables)
	UPROPERTY(EditDefaultsOnly, Category = "WoT|AI") float NPCRespawnTime = 15.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|AI") int32 MaxChargesInGroup = 1;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|AI") int32 MinChargesInGroup = 1;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|AI") float MaxChargeUsedInterval = 0.01f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|AI") float MinChargeGroupInterval = 2.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|AI") float MissOdds = 0.0f;

	static constexpr float PriorityUnusable = -1.0f;

	UPROPERTY() TArray<TObjectPtr<AWoTAngrealInventory>> PuzzleRespawns;   // max 5

	// ---- interface (spec names in comments) ----
	virtual void CastArtifact();     // Cast()
	virtual void UnCastArtifact();   // UnCast()
	// The owner just took damage from Attacker — held artifacts may react (Chain
	// Lightning's DamageNotifier: retaliation frees a victim, self-damage drops it).
	virtual void NotifyOwnerDamaged(APawn* Attacker) {}
	virtual void Reset();
	// BagHolding.AddItem notification. Most artifacts need no extra work;
	// Distant Eye tears down its placed/view state because DropFrom is bypassed.
	virtual void NotifyPutInBag(class AWoTBagHolding* Bag) {}
	bool HaveEnoughCharges() const;
	void UseCharge();
	void AddCharges(int32 NumCharges);
	virtual void Failed();
	void ResetCharges();
	void SetCharges();
	// virtual: sustained self-cast artifacts (e.g. Levitate) restore host state
	// on empty, and UseCharge() calls GoEmpty() at zero charges.
	virtual void GoEmpty();
	void PuzzleRespawn();
	float GetDuration() const;
	bool IsNotTargetable(const AActor* Other) const;
	void AddPuzzleRespawn(AWoTAngrealInventory* Item);

	virtual bool HandlePickupQuery(AWoTInventory* Item) override;
	virtual void InitializeCopy(AWoTInventory* Copy) override;
	virtual void GiveTo(APawn* Other) override;
	virtual void DropFrom(FVector StartLocation) override;
	virtual void SetRespawn() override;
	virtual void PostInitializeComponents() override;   // BeginPlay: SetCharges

	static AWoTAngrealInventory* CreateProxy(AActor* InOwner,
	                                         TSubclassOf<AWoTAngrealInventory> Type);
	// DamageTypes are AEFWS combos with 'x' spacers (e.g. Earth+Water = xExWx)
	static bool DamageTypeContains(FName DamageType, FName Element);
	static FName GetDamageType(const AWoTAngrealInventory* Item);

	// ---- AI support ----
	bool ShouldCast() const;
	void InitChargeGroup();
	void GetEffectiveRange(float& MinRange, float& MaxRange);
	virtual float GetMinRange();
	virtual float GetMaxRange();
	bool ShouldUncast() const;
	bool GetCastDuration(float& CastDuration);
	virtual float GetPriority() const;
	bool GetEffectivePriority(float& EffectivePriority);
	bool GetDefensivePriority(float& DefensivePriority,
	                          class AWoTAngrealProjectile* DefensiveTarget) const;
	virtual bool GetOffensivePriority(float& OffensivePriority, AActor* InTarget) const;
	void SetVictim(AActor* Victim) { Target = Victim; }
	AActor* GetVictim() const { return Target; }
	virtual FVector GetTrajectorySource() const;
	// DefaultTargetingReflector.FindBestTarget: direct-trace first, else the
	// retail targetable actor closest to view center within MaxAngleInDegrees. Lives on
	// the base (not just the launcher) so non-launcher artifacts (Whirlwind) can
	// target. Candidates are pawns, Personal Illusions, Mashadar tendrils, and
	// and the LegionProjectile spirit family (AWoTLegionProjectile). A pawn whose
	// bExcludesLegionProjectileTarget is set (the Legion, LegionTargetingReflector.uc:8)
	// drops the spirit family from its own targetable set.
	AActor* FindBestTarget(FVector Loc, FRotator ViewRot, float MaxAngleInDegrees);
	bool IsTargetable(const AActor* A) const;
#if WITH_AUTOMATION_TESTS
	static bool CanRetailAutoTargetReachForTest(UWorld* World,
		const AActor* QueryOwner, const AActor* IgnoredArtifact,
		const FVector& Start, const AActor* Candidate);
#endif
	virtual float GetLeadSpeed() { return -1.0f; }
	virtual bool IsLeadable() { return false; }
	virtual FRotator GetBestTrajectory() { return FRotator::ZeroRotator; }

	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

private:
	// AngrealAI cast pacing
	float LastChargeUsedTime = 0.0f;
	int32 ChargesUsedInGroup = 0;
	int32 IntendedChargesInGroup = 0;
};
