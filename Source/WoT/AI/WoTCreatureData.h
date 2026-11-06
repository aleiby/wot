#pragma once

#include "CoreMinimal.h"
#include "../RetailData/WoTRetailBuildContext.h"
#include "Engine/DataTable.h"
#include "Templates/SubclassOf.h"
#include "WoTCreatureTypes.h"    // EWoTCreatureTier, EWoTCreatureFaction
#include "WoTMovementPattern.h"
#include "WoTNPCCharacter.h"     // FWoTMeleeAttack, AWoTNPCCharacter, EWoTOrders (via WoTOrders)
#include "WoTNPCController.h"    // EWoTBrainState
#include "WoTBootstrapCore/Hash.h"
#include "WoTCreatureData.generated.h"

class AWoTAngrealInventory;
class FWoTRuntimeClassDefaults;

// The 12 MP troop slots plus the retained base Trolloc row as data:
// the leaves are ~90%
// CDO defaults over Grunt/Captain/AngrealUser/ShieldUser, so the port folds each
// leaf into one FWoTCreatureRow of verbatim tuning + capability data applied to a
// bare AWoTNPCCharacter at spawn. The row carries tuning, capabilities, and the
// behavior identity used to create the live per-creature strategy component
// (Warder fade-cloak, Minion stealth/tentacles, Myrddraal teleport, Legion
// spirits/stomp, BATrolloc stagger, Archer kneel-bow, ShieldUser deflect).
//
// The rows live in a static C++ registry keyed by FName (no content-pipeline step
// — the pipeline forbids hand-authored assets). FWoTCreatureRow derives FTableRowBase
// so a CSV-backed UDataTable can replace the registry later without touching any
// consumer (ConfigureNPC / SpawnCreature / the troop lookup all take a row or a
// name, never the storage).

// EWoTCreatureTier + EWoTCreatureFaction moved to WoTCreatureTypes.h (above) so
// AWoTCharacter can name the faction enum without a WoTCreatureData.h include cycle.

// One weave in a caster's loadout, verbatim from the AngrealUser AII_* parallel
// arrays (AngrealUser.uc defaultproperties; the sentinel -1 = "keep the weave
// class's own default", SetValueWithSentinal pattern AngrealUser.uc:67-77). Only
// the AngrealUser-family casters (Sister, Sitter) populate this — Myrddraal/Archer
// are projectile fighters (RangedWeaponType), not weave casters.
USTRUCT()
struct FWoTCreatureWeave
{
	GENERATED_BODY()

	// The verbatim AII_AngrealInventoryStr path (e.g. "Angreal.AngrealInvSeeker"),
	// carried for traceability even where the ported class is bound below.
	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	FName SourcePath;

	// The ported weave class, bound in the registry where one exists.
	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	TSubclassOf<AWoTAngrealInventory> WeaveClass;

	// AII_Priority — the caster's preference order (higher = preferred).
	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	float Priority = -1.0f;

	// AII_MinChargesInGroup / AII_MaxChargesInGroup — the volley (charge-group) size.
	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	int32 MinChargesInGroup = -1;
	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	int32 MaxChargesInGroup = -1;

	// AII_MinChargeGroupInterval — minimum seconds between charge groups for this weave.
	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	float MinChargeGroupInterval = -1.0f;
};

// One creature leaf folded to data.
// Companion assets remain identities; movement-pattern cells and acquired
// handler bindings are decoded from the authenticated runtime class defaults.
USTRUCT()
struct FWoTCreatureRow : public FTableRowBase
{
	GENERATED_BODY()

	// --- Identity ---
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Identity")
	FName RowName;
	// Authored acquired handlers: reachable21, pathable23, visible component1.
	TArray<FWoTMovementBinding> MovementBindings;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Identity")
	EWoTCreatureTier Tier = EWoTCreatureTier::Grunt;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Identity")
	EWoTCreatureFaction Faction = EWoTCreatureFaction::AesSedai;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Identity")
	bool bIsFemale = false;
	// Retail texture identity bound onto the live NPC for Disguise/GetHealthIcon.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Identity")
	FName DisguiseIcon;

	// --- Stats ---
	// The MP band is authoritative — the pawn rolls Health from it at BeginPlay
	// (WOTPawn.uc:383). HealthPlaced is the engine Health= placed default, carried
	// for the divergences it names (Sitter 300-vs-425, Legion 300-vs-1000, BATrolloc
	// 200-vs-700); the band wins at runtime.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Stats")
	float HealthSPMin = 100.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Stats")
	float HealthSPMax = 100.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Stats")
	float HealthMPMin = 100.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Stats")
	float HealthMPMax = 100.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Stats")
	int32 HealthPlaced = 100;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Stats")
	float Mass = 100.0f;
	// Grunt CDO 25; Champion class 0; Warder 5; Minion 50. Morale is disabled in
	// shipped (Grunt.uc:897 IsHealthy true), so this is inert — carried faithful.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Stats")
	float BrokenMoralePct = 25.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Stats")
	float GibForSureFinalHealth = -40.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Stats")
	float GibSometimesFinalHealth = -30.0f;
	// Hit-reaction gates (WOTPawn.uc:1547,:1557 defaults; only Archer overrides,
	// Archer.uc:265,271). The hard ratio is never overridden and stays the pawn field
	// default. The ratios reference HealthPlaced (the authored default.health).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Stats")
	float ChanceOfHitAnim = 0.25f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Stats")
	float HitSoftHealthRatio = 0.05f;

	// --- Movement ---
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Movement")
	float GroundSpeed = 600.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Movement")
	float GroundSpeedMin = 400.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Movement")
	float GroundSpeedMax = 400.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Movement")
	float BaseWalkingSpeed = 162.5f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Movement")
	float WaterSpeed = 400.0f;
	// GoalSuggestedSpeeds fold: every leaf sets indices 0/2/3/4/5 to one value (the
	// chase hint, fed to the threat/refuge/guard/colleague/intermediate goal slots)
	// and index 6 to another (the waiting hint). Not always == GroundSpeed (Legion
	// chases at a 125 hint but GroundSpeed 50). ConfigureNPC writes these into the
	// brain's goal slots (they survive InitGoal — WoTBrainComponent.cpp:135).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Movement")
	float ChaseSuggestedSpeed = 320.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Movement")
	float WaitSuggestedSpeed = 160.0f;
	// 0 = engine default; Legion sets Yaw=10000 (slow turner for its bulk).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Movement")
	int32 RotationRateYaw = 0;

	// --- Combat ---
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Combat")
	float MeleeRange = 25.0f;
	// Each AttackXDamageTarget notify is one row, keyed to its attack sequence by
	// AnimName; the anim-table pick selects the sequence, and every row carrying it
	// fires at its authored fraction (multi-notify swings = several rows, one anim).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Combat")
	TArray<FWoTMeleeAttack> MeleeAttacks;
	// The shipped AnimationTableClass= ref (WoTAnimTable registry key) and which
	// melee slot this creature's swings use ('MeleeAttack' unarmed vs
	// 'MeleeWeaponAttack' for weapon wielders — the shipped state split as data).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Combat")
	FName AnimTable;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Combat")
	FName MeleeAnimSlot = FName(TEXT("MeleeAttack"));
	// Weapon-type class paths (placeholders — the weapon/adapter proxies are ported
	// separately; carried so the melee-vs-ranged band selection can bind them later).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Combat")
	FName DefaultWeaponType;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Combat")
	FName MeleeWeaponType;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Combat")
	FName RangedWeaponType;
	// RangeHandlerFactory* → the per-creature RangeBandSet (a config asset later).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Combat")
	FName HandlerFactoryClass;
	// The behavior hook this creature still needs; NAME_None for the
	// pure-data leaves (Trolloc, Sister, Sitter, MashadarFocusPawn).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Combat")
	FName BehaviorClass;

	// --- Defensive dodge (DefensiveDetector*.uc; census-verified LIVE for exactly
	// Warder/Trolloc/Minion in MP — every other creature either shields, teleports, has
	// the passive caster reflex, or never arms a real detector). The active-detection
	// window differs: Warder/Trolloc [0.05,1.5], Minion [0.01,1.5]. The perimeter factors
	// (Minion RadiusFactor 8 / HeightFactor 3) drive PASSIVE detection only — the active
	// scan uses CollectionRadius, so they are not carried (PORT_NOTES).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Combat")
	bool bDefensiveDodge = false;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Combat")
	float DodgeResponseMinTime = 0.05f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Combat")
	float DodgeResponseMaxTime = 1.5f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Combat")
	float DodgeCollectionRadius = 1024.0f;
	// DefaultInventoryTypes granted at spawn (Legion's seeker + stomp) — placeholders.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Combat")
	TArray<FName> DefaultInventoryTypes;

	// LegionTargetingReflector.uc:8 (TargetableTypes(4)=None): this creature's targeting
	// set EXCLUDES the LegionProjectile spirit family, so its weaves never lock onto a
	// spirit. Only the Legion sets it; every other creature/player keeps the base list.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Combat")
	bool bExcludesLegionProjectileTarget = false;

	// --- Caster (AngrealUser family only) ---
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Caster")
	TArray<FWoTCreatureWeave> Weaves;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Caster")
	float MinOffensiveAngrealUsageInterval = 0.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Caster")
	float MinDefensiveAngrealUsageInterval = 0.0f;
	// AngrealWeaponAdapter usage-window CDO. Selection currently gets its range
	// from each weave; these inert config fields preserve LegionWeaponAdapter's
	// 50..3200 / visibility-false surface for the behavior consumer.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Caster")
	float AdapterMinProjectileRange = 0.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Caster")
	float AdapterMaxProjectileRange = 10000.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Caster")
	bool bAdapterRequireTargetVisibility = false;
	// The standoff a caster's tracking bands hold at (UWoTCasterComponent::HoldRange).
	// Derived: the source per-band standoff comes from the RangeHandlerFactory, a
	// later data binding — 400 is the working stand-in (matches the caster ladder).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Caster")
	float HoldRange = 400.0f;

	// --- Orders / flags (Captain.uc; the "flag surface" fold) ---
	// bIsCaptain follows the CLASS layer (Captain or below), NOT the tier: Questioner
	// is a Champion-tier troop but a ShieldUser→Grunt, so it has no orders.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Orders")
	bool bIsCaptain = false;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Orders")
	EWoTOrders AssignedOrders = EWoTOrders::KillIntruder;
	// Champion-CLASS pawns (BATrolloc, Legion) skip Captain's MP Automatic override.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Orders")
	bool bChampionOrders = false;
	// Shipped initial state: LegendPawn Dormant, Grunt-family InactiveIdle
	// (→ Inactive here). MashadarFocusPawn re-sets Dormant. NOTE the controller's
	// bare default is Waiting; SpawnCreature's bCombatReady bridges the missing
	// activity-LOD wake path (see SpawnCreature + PORT_NOTES).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Orders")
	EWoTBrainState InitialState = EWoTBrainState::Inactive;

	// --- Assets / collision / perception (numeric values real; class refs placeholder) ---
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Assets")
	FName Mesh;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Assets")
	float DrawScale = 1.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Assets")
	float ScaleGlow = 1.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Assets")
	float CollisionRadius = 16.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Assets")
	float CollisionHeight = 48.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Assets")
	float BaseEyeHeight = 42.0f;
	// Engine.Pawn defaults. UE5 collapses all three retail
	// pawn/projectile blocking flags onto the capsule's ECC_Pawn response.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Assets")
	bool bBlockActors = true;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Assets")
	bool bBlockPlayers = true;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Assets")
	bool bProjTarget = true;
	// 0 = engine default sight cone (Myrddraal 120, BATrolloc 45, Legion 150).
	// Carried as data; the FovAngle→PeripheralVision mapping is a later perception pass.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Assets")
	float FovAngle = 0.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Assets")
	uint8 Visibility = 128;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Assets")
	FName CarcassType;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Assets")
	FName SoundTableClass;
	// Authored MultiSkins and Minion blink defaults retained as normalized retail
	// identities; runtime binding resolves them through the authenticated texture set.
	TMap<int32, FName> MaterialSkinIdentities;
	FName MinionEyesTexture;
	float MinionEyeOffMin = 0.0f;
	float MinionEyeOffMax = 0.0f;
	float MinionEyeOnMin = 0.0f;
	float MinionEyeOnMax = 0.0f;
	bool bMinionEyesEnabled = false;
	// The pawn's looping AmbientSound hum (only Legion authors one: Leg_Ambient
	// at radius byte 255, Legion.uc:524-525; vol/pitch inherit Actor 128/64).
	// Subpath under /Game/WoT/Sounds; NAME_None = silent.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Assets")
	FName AmbientSoundPath;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Assets")
	uint8 AmbientSoundRadius = 32;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Assets")
	uint8 AmbientSoundVolume = 128;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Assets")
	uint8 AmbientSoundPitch = 64;
	// Shield deflect (Soldier/Questioner). The flag marks the family.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Assets")
	bool bShieldUser = false;
	// DamageHelperBlackBlood (Minion/Myrddraal): black blood set (gore 4d).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Assets")
	bool bBlackBlood = false;

	// Apply this row's tuning + capability data to a spawned NPC: stats, movement, melee set, brain suggested-speeds, captain/
	// champion flags, caster loadout, collision, visibility. Pass the controller (if
	// possessed) to seed its shipped InitialState. Runs before the pawn's health
	// roll when called from SpawnCreature (deferred spawn), so the MP band takes hold.
	void ConfigureNPC(AWoTNPCCharacter& NPC, AWoTNPCController* Controller = nullptr) const;
};

struct FWoTPlayerTroopRow
{
	FName Faction;
	FName Tier;
	FName RowName;
};

struct FWoTCreatureSet
{
	// Includes disabled cells and authored but unbound classes (Warder0).
	TMap<FName, FWoTMovementPattern> MovementPatterns;
	TMap<FName, FWoTCreatureRow> Rows;
	TArray<FWoTPlayerTroopRow> PlayerTroops;
	float TracerSpinAccelerationRollDegPerSec2 = 0.0f;
};

struct FWoTCreatureDataBuilder
{
	static constexpr const TCHAR* FamilyName = TEXT("creature-data");

	static bool Build(const FWoTRetailBuildContext& Context,
		FWoTCreatureSet& Out, FString& OutError);
	static FString CanonicalDigest(const FWoTCreatureSet& Set);
	static void Install(FWoTCreatureSet&& Set);
	static void SerializeCanonical(const FWoTCreatureSet& Set, TArray<uint8>& Out);
	static bool AreEquivalent(const FWoTCreatureSet& Built,
		const FWoTCreatureSet& Expected, FString& OutError);

#if WITH_AUTOMATION_TESTS
	static void Reset();
#endif
};

// The registry + troop lookup + spawn helper (WoTAIUtil / WoTSight namespace shape).
namespace WoTCreatureData
{
	// Apply the row's three retail blocking flags to the port's shared pawn query
	// channel. World responses are deliberately left untouched.
	void ApplyBlockingColumns(AWoTNPCCharacter& NPC, const FWoTCreatureRow& Row);

	// The creature-row registry (built once). Null for an unknown name.
	const FWoTCreatureRow* Find(FName RowName);
	float TracerSpinAccelerationRollDegPerSec2();

	// Bind the row's body mesh + DrawScale onto the pawn (presentation, ALL roles:
	// the authority needs the bound sequence table for its notify/death clocks,
	// clients for rendering). The registry is static data in both binaries, so a
	// client re-derives the look from the replicated row NAME alone (the carcass
	// MeshName template). False for unknown rows / missing content.
	bool BindCreatureBody(AWoTNPCCharacter& NPC, FName RowName);

	// Every registered row name (12 troop leaves + retained base Trolloc), for specs/editors.
	void GetAllRowNames(TArray<FName>& OutNames);

	// GetTroop (WOTPlayer.uc:4076): linear-scan the 4x3 troop table for the
	// (Faction, Tier) tuple and return its creature row name. Faction is the source
	// player name ('AesSedai'/'Forsaken'/'Whitecloak'/'Hound'); Tier the keyword
	// ('Grunt'/'Captain'/'Champion'). NAME_None if the tuple is not in the table.
	FName GetTroopRow(FName FactionName, FName TierKeyword);

	// Spawn an AWoTNPCCharacter (or a PawnClass subclass — pass a test pawn for
	// forced-nav specs), apply its creature row, and possess it. Deferred-spawns so
	// ConfigureNPC's stats land before the BeginPlay health roll. bCombatReady=true
	// (specs/summons) seeds the controller Waiting so the pawn is immediately live;
	// false seeds the shipped InitialState (Inactive/Dormant), which now sticks and
	// wakes on proximity via the controller's activity-LOD poll.
	AWoTNPCCharacter* SpawnCreature(UWorld& World, FName RowName, const FVector& Location,
	                                uint8 Team, bool bCombatReady = true,
	                                TSubclassOf<AWoTNPCCharacter> PawnClass = nullptr);
}
