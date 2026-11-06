#pragma once

#include "CoreMinimal.h"
#include "../WoTCharacter.h"
#include "WoTOrders.h"
#include "WoTNPCCharacter.generated.h"

enum class EWoTGoalIndex : uint8;

// Faction classification (LegendPawn.uc EFactionType; Grunt.uc:1167 folds
// Unknown into Friendly/Enemy via IsFriendly).
UENUM()
enum class EWoTFaction : uint8
{
	Unknown,
	Friendly,
	Enemy
};

// The special team bytes (WOTPawn.uc:43-46). Values 0..3 are real teams.
namespace WoTTeam
{
	constexpr uint8 NPC = 255;    // teamless-NPC team (also the unassigned default)
	constexpr uint8 PC = 254;     // friendly to all players
	constexpr uint8 None = 253;   // friendly to nobody
	constexpr uint8 Any = 252;    // friendly to everybody
}

// One melee damage notify: an `#exec MESH NOTIFY ... FUNCTION=<X>DamageTarget` on an
// attack sequence (e.g. Trolloc.uc:11 `SEQ=AttackRun TIME=0.75`). The anim table picks
// the swing's sequence; every attack row carrying that AnimName fires at its authored
// fraction of the sequence (multi-notify swings — Warder DbleSwipe, BATrolloc doubles —
// are several rows sharing one AnimName).
USTRUCT()
struct FWoTMeleeAttack
{
	GENERATED_BODY()

	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	float Damage = 0.0f;

	// Vertical hit offset as a fraction of CollisionHeight (Grunt.uc
	// MeleeDamageGoal's HitLocationModifier).
	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	float HeightModifier = 0.0f;

	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	FName DamageTypeName;

	// The attack sequence this notify belongs to (the anim-table pick joins on it).
	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	FName AnimName;

	// Fraction of the sequence at which the damage notify fires (the authored MESH
	// NOTIFY TIME). Real time = fraction x the sequence's duration at the table's
	// rate; without a bound sequence the stand-in swing length is 1s, which keeps
	// the old fraction-as-seconds behavior for table-less pawns.
	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	float NotifyFraction = 0.5f;

	// ShieldUser routes both its authored PlayMeleeHitSound notify and its
	// landed damage callback through the shield-specific slot. Ordinary attacks
	// retain Grunt's flesh-hit slot.
	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	FName HitSoundSlot = TEXT("WeaponMeleeHitEnemy");

	// Warder's SkewRip and DbleSwipe damage functions set bAnimFinished when a
	// notify misses, ending the rooted episode before any later notify can fire.
	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	bool bAbortSwingOnMiss = false;
};

// The NPC pawn body: WOTPawn's pawn half folded onto the ported AWoTCharacter
// (which already carries the health/damage chain, reflector fold, inventory
// component, and leech handling — WOTPawn's "reflected functions" block is
// inherited, not re-ported). The brain lives on the
// controller + UWoTBrainComponent; creature identity arrives as data.
UCLASS()
class AWoTNPCCharacter : public AWoTCharacter
{
	GENERATED_BODY()

public:
	AWoTNPCCharacter(const FObjectInitializer& ObjectInitializer);

	UPROPERTY(VisibleAnywhere, Category = "WoT")
	TObjectPtr<class UWoTBrainComponent> Brain;

	// Retail's two weapon inventory slots, materialized as replicated held actors.
	// Melee and ranged may intentionally point at the same throwable actor
	// (BATHalberd / QuestionerShield).
	UPROPERTY(Replicated, VisibleAnywhere, Category = "WoT|Weapon")
	TObjectPtr<class AWoTNPCWeapon> MeleeWeapon;
	UPROPERTY(Replicated, VisibleAnywhere, Category = "WoT|Weapon")
	TObjectPtr<class AWoTNPCWeapon> RangedWeapon;
	UPROPERTY(Replicated, VisibleAnywhere, Category = "WoT|Weapon")
	TObjectPtr<class AWoTNPCWeapon> HeldWeapon;

	void InitializeWeaponSlots(FName DefaultWeaponType, FName MeleeWeaponType,
		FName RangedWeaponType);
	void EquipMeleeWeapon();
	void EquipRangedWeapon();
	void EquipBodyAttack();
	void DropWeaponsOnDeath();
	AWoTNPCWeapon* GetMeleeWeapon() const { return MeleeWeapon; }
	AWoTNPCWeapon* GetRangedWeapon() const { return RangedWeapon; }
	AWoTNPCWeapon* GetHeldWeapon() const { return HeldWeapon; }
	// PerformMeleeAttack vs PerformMeleeWeaponAttack is selected from the actual
	// held inventory actor. A missing/destroyed/in-flight throwable means body attack.
	FName GetEffectiveMeleeAnimSlot() const;

	virtual bool IsNPCPawn() const override { return true; }
	virtual void Died(AController* Killer, FName WoTDamageType,
		const FVector& HitLocation) override;
	virtual void ShutdownCorpsePresentation() override;
	virtual void NotifyHit(UPrimitiveComponent* MyComp, AActor* Other,
		UPrimitiveComponent* OtherComp, bool bSelfMoved, FVector HitLocation,
		FVector HitNormal, FVector NormalImpulse, const FHitResult& Hit) override;
	void NotifyHitByAngrealProjectile(class AWoTAngrealProjectile* Projectile);
	void GoalInitialized(EWoTGoalIndex GoalIndex);

	// Grunt's authored per-instance event hooks. Empty/false are the retail
	// defaults; ordinary UE actor-property import supplies map-authored values.
	UPROPERTY(EditAnywhere, Category = "WoT|Events")
	FName InitThreatGoalEvent;
	UPROPERTY(EditAnywhere, Category = "WoT|Events")
	FName TakeDamageEvent;
	UPROPERTY(EditAnywhere, Category = "WoT|Events")
	bool bTakeDamageEventOnce = false;

	// Team byte; 255 = unassigned (WOTPawn.uc:48, default :1540). Summons copy
	// the caster's team (ResourceSpawner.uc:62); placed NPCs are configured by
	// the map/battle layer.
	UPROPERTY(EditAnywhere, Replicated, Category = "WoT")
	uint8 AssignedTeam = 255;

	// The creature-row identity, REPLICATED so a network client can re-derive the
	// body and blocking response from the static row registry (UE1 replicated
	// Mesh/DrawScale and Pawn flags natively; the port replicates the row NAME and
	// re-applies them). Stamped by ConfigureNPC; before this, nothing row-shaped
	// replicated and net clients could not bind creature state (PORT_NOTES).
	UPROPERTY(ReplicatedUsing = OnRep_CreatureRow)
	FName CreatureRowName;
	UFUNCTION() void OnRep_CreatureRow();

	// Warder.SetFadeSkins/SetStyleForVisible presentation. UE1 replicated Actor.Style
	// and MultiSkins natively; the port sends the authored FadeSkins index explicitly.
	// Indices 0..11 select the Warder's retail camouflage textures; 255 restores the
	// exact materials that were present before fading.
	static constexpr uint8 VisibleFadeSkinIndex = 255;
	UPROPERTY(ReplicatedUsing = OnRep_FadeSkinIndex)
	uint8 FadeSkinIndex = VisibleFadeSkinIndex;
	void SetFadeSkinIndex(uint8 NewIndex);
	uint8 GetFadeSkinIndex() const { return FadeSkinIndex; }
	UFUNCTION() void OnRep_FadeSkinIndex();

	// Myrddraal's authority-only brain publishes presentation edges on the pawn.
	// Every renderer creates its own particle systems from the replicated serial;
	// both authored locations remain in the initial bunch for late relevance.
	UPROPERTY(Replicated) FVector_NetQuantize TeleportEnterEffectLocation;
	UPROPERTY(Replicated) FVector_NetQuantize TeleportExitEffectLocation;
	UPROPERTY(Replicated) uint8 TeleportPresentationPhase = 0;
	UPROPERTY(ReplicatedUsing=OnRep_TeleportPresentation) uint16 TeleportPresentationSerial = 0;
	void EmitTeleportPresentation(uint8 Phase, const FVector& Location);
	UFUNCTION() void OnRep_TeleportPresentation();
#if WITH_AUTOMATION_TESTS
	uint16 GetTeleportPresentationSerialForTest() const { return TeleportPresentationSerial; }
	uint8 GetTeleportPresentationPhaseForTest() const { return TeleportPresentationPhase; }
	class AWoTParticleSprayer* GetTeleportEnterSprayerForTest() const { return TeleportEnterSprayer.Get(); }
	class AWoTParticleSprayer* GetTeleportExitSprayerForTest() const { return TeleportExitSprayer.Get(); }
#endif

	// WoTCreatureData's row bind can replace the body after fade replication arrives.
	// Bracket that rebind so old overrides cannot leak onto the new mesh and a faded
	// Warder immediately regenerates its modulated slots from the new body.
	void PrepareFadeSkinBodyRebind();
	void FinishFadeSkinBodyRebind();

	// --- Summon identity (ResourceSpawner.uc:54,62,89-101 — the 'AngrealSpawned' fold) ---
	// The caster who summoned this helper; null for a placed (map) NPC. Read by the
	// owner-kill-credit path (giWOT.ScoreKill:143) and ResourceSpawner::HaveHelper's
	// one-at-a-time scan. Server-side only (scoring + HaveHelper are authoritative),
	// exactly like AWoTCharacter::SuicideInstigator — a dedicated field rather than the
	// engine AActor::Owner, whose net-relevancy semantics would wrongly pin an
	// all-players-must-see AI pawn to one player's connection.
	UPROPERTY() TWeakObjectPtr<AWoTCharacter> SummonOwner;
	// The creature row this helper was summoned as — folds ResourceSpawner's
	// IsA(ResourceClass.Name) type test: every summon shares the AWoTNPCCharacter
	// class, so the row name is its distinguishing "type" (Grunt vs Captain vs Champion).
	UPROPERTY() FName SummonRow;
	// ResourceSpawner.uc:63: "<caster>'s <Class>" — retail wrote it onto the
	// summon's PRI; the port's NPCs carry no PlayerState, so the decorated name
	// lives here. Server-side only: its consumer (the giWOT.ScoreKill:147 death
	// broadcast) composes messages on the authority.
	UPROPERTY() FString SummonDisplayName;

	// Spawn-time randomized bands (WOTPawn.uc:379-386; MP band, WOTPawn.uc:55).
	// Creature rows override these; base WOTPawn is 100/100 + 400/400.
	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	float HealthMPMin = 100.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	float HealthMPMax = 100.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	float GroundSpeedMin = 400.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	float GroundSpeedMax = 400.0f;
	// The spawn-time WOTPawn GroundSpeed roll. DesiredSpeed may temporarily
	// lower CharacterMovement.MaxWalkSpeed, but FallingState.AdjustJump uses the
	// unscaled GroundSpeed property.
	UPROPERTY(Transient)
	float RetailGroundSpeed = 400.0f;
	// Grunt.LoopMovementAnim uses class-default speeds, not the randomized
	// movement component limit, to choose and scale Walk/Run.
	UPROPERTY(Transient) float AnimationGroundSpeed = 600.0f;
	UPROPERTY(Transient) float AnimationBaseWalkingSpeed = 162.5f;
	UPROPERTY(Transient) float AnimationWalkSpeedPct = 50.0f;

	// --- Sight (engine Pawn fields + defaults) ---

	// Maximum seeing distance (Pawn default 2500).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|AI")
	float SightRadius = 2500.0f;

	// Cosine FOV threshold for the player-sighting cone; the shipped classes
	// leave the engine default (0 = 90° half-angle, widened by the stimulus
	// rescale).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|AI")
	float PeripheralVision = 0.0f;

	// How visible this pawn is to observers (0 = invisible, 128 = normal).
	// Stealth creatures (Minion fade) scale this.
	// --- Melee (Grunt.uc; per-creature values are creature-row data) ---

	// Max cylinder gap for a melee attack (Grunt.uc:2432 default 25; 0 = no melee).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|AI")
	float MeleeRange = 25.0f;

	// The swing set; the anim-table pick selects which sequence swings and every
	// row with that AnimName fires at its notify fraction. Defaults are the Trolloc
	// pair (Trolloc.uc:100-101 + notify times :11,:14) until creature rows land.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|AI")
	TArray<FWoTMeleeAttack> MeleeAttacks;

	// Which anim-table slot this creature's melee swings come from: 'MeleeAttack'
	// (unarmed — Trolloc claws, Minion running claws, Legion stomps) or
	// 'MeleeWeaponAttack' (sword/halberd/shield wielders — Warder, Myrddraal, Archer,
	// Soldier, Questioner, BATrolloc). The shipped states PerformMeleeAttack vs
	// PerformMeleeWeaponAttack chose by weapon-in-hand (Grunt.uc:1484-1512); the port
	// carries the effective slot as creature data (PORT_NOTES).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|AI")
	FName MeleeAnimSlot = FName(TEXT("MeleeAttack"));

	// Defensive dodge (DefensiveDetector*.uc), armed by the creature row for exactly
	// Warder/Trolloc/Minion. The controller's Defensive pulse scans for an incoming
	// projectile arriving within [DodgeResponseMinTime,DodgeResponseMaxTime] inside
	// DodgeCollectionRadius and, when armed, sidesteps it (LegendPawn PerformDodge label).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|AI")
	bool bDefensiveDodge = false;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|AI")
	float DodgeResponseMinTime = 0.05f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|AI")
	float DodgeResponseMaxTime = 1.5f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|AI")
	float DodgeCollectionRadius = 1024.0f;
	// LegendPawn CDO (:2642-2643): the launch profile; no creature overrides it.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|AI")
	float DodgeVelocityFactor = 400.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|AI")
	float DodgeVelocityAltitude = 160.0f;

	// --- Orders / leadership (Captain.uc; the "flag surface" fold) ---

	// A Captain is a Grunt with orders + ranged: rather
	// than a pawn subclass, captain-ness is this flag + AssignedOrders. The
	// controller reads it at possession to enable its orders block, and the
	// leadership scans (WoTAIUtil::GetLeader) find captains by it. Champion is the
	// same flag + KillIntruder orders + no morale (see below).
	UPROPERTY(EditAnywhere, Category = "WoT|AI")
	bool bIsCaptain = false;

	// Captain.uc owns this editable radius on the pawn. Keeping it with the
	// flattened retail identity preserves per-instance values across possession.
	UPROPERTY(EditAnywhere, Category = "WoT|AI")
	float FindHelpRadius = 320.0f;

	// The order the map/GUI assigns (Captain.uc:45 var() AssignedOrders, CDO
	// OT_KillIntruder). The AI reads WorkingOrders (on the controller); this is
	// the placed default the controller copies at possession.
	UPROPERTY(EditAnywhere, Category = "WoT|AI")
	EWoTOrders AssignedOrders = EWoTOrders::KillIntruder;

	// Captain.SetInitialState forces AssignedOrders=OT_Automatic in MP (the
	// Captain.uc:87 hack); Champion.SetInitialState skips that and keeps
	// KillIntruder (Champion.uc:14-20). This flag suppresses the MP override —
	// set true for Champions (the "no Automatic" half of the Champion data fold).
	UPROPERTY(EditAnywhere, Category = "WoT|AI")
	bool bChampionOrders = false;

	// BrokenMoralePct (Grunt CDO 25, Champion 0). Morale is disabled in shipped
	// (Grunt.uc:897 IsHealthy early-returns true), so this is inert — carried for
	// the Champion data surface + a future morale pass (PORT_NOTES).
	UPROPERTY(EditAnywhere, Category = "WoT|AI")
	float BrokenMoralePct = 25.0f;

	// --- Faction (WOTPawn.uc:728 IsFriendly; Grunt.uc:1167 GetActorFaction) ---

	// The team byte another pawn presents to faction checks: an NPC presents
	// AssignedTeam (the original mirrored it into its PRI — WOTPawn.uc:429), a
	// player presents PlayerState.Team. Disguise's ApparentTeam substitution
	// hooks in here when its AI half lands.
	static uint8 TeamOf(const APawn* Other);

	bool IsFriendly(const AActor* Other) const;
	EWoTFaction GetActorFaction(const AActor* Other) const;

	virtual void PostInitializeComponents() override;
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void EndPlay(EEndPlayReason::Type EndPlayReason) override;
	// Legion.uc:95-150: three client-local orbiting spirit heads.  Components
	// inherit pawn relevancy/visibility without becoming collision roots.
	UPROPERTY(Transient) TArray<TObjectPtr<class UWoTVertexAnimComponent>> LegionSpirits;
	// Route the raw incoming damage to the creature behavior hook (BATrolloc recovery
	// stagger keys off a single hit's magnitude — BATrolloc.uc:83). Forwards to Super
	// (the AWoTCharacter reflector/health chain) unchanged, then notifies the hook.
	virtual float TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent,
	                         AController* EventInstigator, AActor* DamageCauser) override;
	// WOTPawn.Landed (WOTPawn.uc:1074-1096): NPCs DO take fall damage — same
	// -0.15*(VZ+1050) core as the player, but gated at MinLandedDamageVelocity-850
	// (-1700 by default, vs the player's -1100) and gibbed below
	// FatalLandedDamageVelocity.
	virtual void TakeFallingDamage(float VelocityZ) override;
	virtual void OnWalkingOffLedge_Implementation(const FVector& PreviousFloorImpactNormal,
		const FVector& PreviousFloorContactNormal, const FVector& PreviousLocation,
		float TimeDelta) override;
	virtual void Landed(const FHitResult& Hit) override;

	// Engine.Pawn defaults (UE1's native APawn::CanHear).
	UPROPERTY(EditAnywhere, Category = "WoT|NPC") float HearingThreshold = 1.0f;
	UPROPERTY(EditAnywhere, Category = "WoT|NPC") float Alertness = 0.0f;

	// var() tunables on WOTPawn (WOTPawn.uc:92-93, defaults :1561-1562). No shipped
	// subclass overrides either.
	UPROPERTY(EditAnywhere, Category = "WoT|NPC") float MinLandedDamageVelocity = -850.0f;
	UPROPERTY(EditAnywhere, Category = "WoT|NPC") float FatalLandedDamageVelocity = -2000.0f;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

private:
	void EnsureLegionSpirits();
	float LegionSpiritClock = 0.0f;
	float MinionEyeToggleAt = 0.0f;
	bool bMinionEyesOn = false;
	void ApplyMinionEyeState(bool bEyesOn);
	class AWoTNPCWeapon* SpawnWeapon(FName WeaponType);
	void EquipWeapon(class AWoTNPCWeapon* Weapon);
	void OnOwnedWeaponDestroyed(class AWoTNPCWeapon* Weapon);
	bool bDroppedDeathWeapons = false;
	void ApplyFadeSkinLook();
	void RestoreFadeSkinMaterials(bool bForceRuntimeBake);
	void ApplyTeleportPresentation();

	UPROPERTY(Transient)
	TArray<TObjectPtr<class UMaterialInterface>> FadeSkinOriginalMaterials;
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UMaterialInstanceDynamic>> FadeSkinMaterials;
	UPROPERTY(Transient) TObjectPtr<class AWoTParticleSprayer> TeleportEnterSprayer;
	UPROPERTY(Transient) TObjectPtr<class AWoTParticleSprayer> TeleportExitSprayer;
	UPROPERTY(Transient) TObjectPtr<class AWoTActorRotator> TeleportEnterSprayerRotator;
	UPROPERTY(Transient) TObjectPtr<class AWoTActorRotator> TeleportExitSprayerRotator;
};
