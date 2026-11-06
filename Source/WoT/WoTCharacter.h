#pragma once

#include "CoreMinimal.h"
#include "Inventory/WoTTouchReflectorDescription.h"
#include "GameFramework/Character.h"
#include "Net/UnrealNetwork.h"
#include "AI/WoTCreatureTypes.h"   // EWoTCreatureFaction (SummonFaction)
#include "WoTCharacter.generated.h"

// The replicated vertex-anim channel: a compact sequence/rate/mode
// intent that a simulated proxy replays on its UWoTVertexAnimComponent. A
// bumped PlayToken re-fires the same sequence (the parity of re-triggering an
// anim). The FName sequence is UWoTVertexAnimComponent-API-native and stays
// observable without a mesh bound (the meta.json id mapping is presentation).
USTRUCT()
struct FWoTRepAnimState
{
	GENERATED_BODY()

	UPROPERTY() FName Sequence = NAME_None;
	UPROPERTY() float Rate = 1.0f;
	UPROPERTY() uint8 bLoop = 0;
	// A positive TweenTime blends from the current rendered pose first;
	// bTweenOnly stops on frame zero instead of playing the sequence.
	UPROPERTY() float TweenTime = 0.0f;
	UPROPERTY() uint8 bTweenOnly = 0;
	// Ice changes this only on freeze/thaw. Quantizing the [0,1] pose fraction
	// keeps the extra replicated state to three bytes before struct padding.
	UPROPERTY() uint16 HeldFrame = 0;
	UPROPERTY() uint8 bHoldFrame = 0;
	UPROPERTY() uint8 PlayToken = 0;
};

// WOTPlayer.FirstIcon + IconInfo (WOTPlayer.uc:77,1660-1744;
// IconInfo.uc:19-24). The retail owner-only linked list folds to a replicated
// array while preserving its head-first order (index zero is the newest icon).
USTRUCT()
struct FWoTStatusIconEntry
{
	GENERATED_BODY()

	UPROPERTY() FName IconID = NAME_None;
	UPROPERTY() FName IconTexture = NAME_None;
	UPROPERTY() bool bGood = false;
	UPROPERTY() float InitialDuration = 0.0f;
	UPROPERTY() float RemainingDuration = 0.0f;
};

enum class EWoTFootstepGroup : uint8;
class USkeletalMesh;
class UMaterialInterface;
class AWoTCharacter;
class FWoTRuntimeClassDefaults;

/** Decode WOTPlayer's inherited Engine.Pawn Health through WOTDEF3. */
WOT_API bool WoTDecodePlayerHealthDefault(
	const FWoTRuntimeClassDefaults& Defaults, int32& OutHealth, FString& OutError);

// AngrealInvDisguise CopyDisguisePawnProperties (uc:285-384): the replicated
// driver that reproduces a mimicked pawn's LIVE look on every client. UE1
// replicated the raw Mesh/MultiSkins/DrawScale/collision natively; UE5 does not,
// so the port replicates the source's effective appearance INPUTS (its class +
// personal skin + the live capsule/eye/scale/speed scalars) and every machine
// rebuilds the look through the same seams as ApplyFactionAppearance. bActive=0
// means not disguised. Replicated COND_None: what fools OTHER players must reach
// every client (unlike the owner-only ApparentClass anim/HUD surface).
USTRUCT()
struct FWoTDisguiseSkinIdentity
{
	GENERATED_BODY()
	UPROPERTY() int32 Slot = INDEX_NONE;
	UPROPERTY() FSoftObjectPath Texture;
};

USTRUCT()
struct FWoTDisguiseState
{
	GENERATED_BODY()

	UPROPERTY() uint8 bActive = 0;
	// The mimicked pawn's EFFECTIVE appearance class — its own disguise/appearance
	// override chased through (uc:281-283 "someone who is disguised themselves").
	// Drives the body mesh, draw scale and DisguiseIcon via the CDO.
	UPROPERTY() TSubclassOf<AWoTCharacter> SourceClass = nullptr;
	// The source's live personal ?Skin= variant (retail copied MultiSkins live, uc:323).
	UPROPERTY() FName SourcePersonalSkin = NAME_None;
	// The source's live mesh relative scale (retail DrawScale, uc:293).
	UPROPERTY() FVector SourceMeshScale = FVector(1.0f);
	// The source's live capsule (retail CollisionRadius/Height, uc:314).
	UPROPERTY() float SourceCapsuleRadius = 0.0f;
	UPROPERTY() float SourceCapsuleHalfHeight = 0.0f;
	// The source's live BaseEyeHeight (uc:332) and anim table (uc:365/369) — so a
	// remote client picks the source's sequences off the RepAnim channel.
	UPROPERTY() float SourceBaseEyeHeight = 0.0f;
	UPROPERTY() FName SourceAnimTableName = NAME_None;
	UPROPERTY() FString SourceRuntimeMeshLogicalId;
	UPROPERTY() FString SourceRuntimeAnimationLogicalId;
	UPROPERTY() FSoftObjectPath SourceDisguiseIcon;
	UPROPERTY() uint8 SourceVisibility = 128;
	// Normalized live MultiSkins, plus the UE1 Actor presentation fields copied
	// by AngrealInvDisguise. Style: 0 normal, 1 translucent, 2 modulated.
	UPROPERTY() TArray<FWoTDisguiseSkinIdentity> SourceRuntimeSkins;
	UPROPERTY() uint8 SourceStyle = 0;
	UPROPERTY() uint8 SourceAmbientGlow = 0;
	UPROPERTY() float SourceScaleGlow = 1.0f;
	// ScaleSpeedSettings(1.0) inputs — the apparent class's live movement speeds
	// (uc:1926-1931). Replicated so every client predicts at the disguised speed.
	UPROPERTY() float SourceWalkSpeed = 0.0f;
	// ApparentClass.default.GroundSpeed, kept separate from the source's live
	// GroundSpeed so WOTPlayer's locomotion rate multiplier remains faithful.
	UPROPERTY() float SourceDefaultWalkSpeed = 0.0f;
	UPROPERTY() float SourceSwimSpeed = 0.0f;
	UPROPERTY() float SourceAcceleration = 0.0f;
	UPROPERTY() float SourceAirControl = 0.0f;
	// Retail Style/Visibility copy (uc:329-330): a faded/ghost source makes you
	// faded too — the one translucency system the port models.
	UPROPERTY() uint8 bSourceNeutralGhost = 0;
};

// The per-machine snapshot RestoreDisguisePawnProperties restores (uc:389-458).
// Captured locally on the apply edge (each client owns its material instances),
// restored on the revert edge. Runtime-only — never replicated.
USTRUCT()
struct FWoTSavedDisguise
{
	GENERATED_BODY()

	UPROPERTY() uint8 bValid = 0;
	UPROPERTY() TObjectPtr<USkeletalMesh> Mesh = nullptr;
	UPROPERTY() FString RuntimeMeshLogicalId;
	UPROPERTY() FString RuntimeAnimationLogicalId;
	UPROPERTY() TMap<int32, FSoftObjectPath> RuntimeSkinOverrides;
	UPROPERTY() TArray<TObjectPtr<UMaterialInterface>> OverrideMaterials;
	UPROPERTY() FVector MeshScale = FVector(1.0f);
	UPROPERTY() float CapsuleRadius = 0.0f;
	UPROPERTY() float CapsuleHalfHeight = 0.0f;
	UPROPERTY() float BaseEyeHeight = 0.0f;
	UPROPERTY() FName AnimTableName = NAME_None;
	UPROPERTY() uint8 Visibility = 128;
	UPROPERTY() uint8 AmbientGlow = 0;
	UPROPERTY() float ScaleGlow = 1.0f;
	UPROPERTY() float WalkSpeed = 0.0f;
	UPROPERTY() float SwimSpeed = 0.0f;
	UPROPERTY() float Acceleration = 0.0f;
	UPROPERTY() float AirControl = 0.0f;
};

// WOT.DamageEffect's data payload at the ProcessEffect seam
// (DamageEffect.uc / Invokable.uc / DefaultProcessEffectReflector.uc). The
// ordered reflector chain may discard it; the default bottom invokes
// TakeDamage with these values.
struct FWoTDamageEffectSpec
{
	int32 Damage = 0;
	APawn* InstigatorPawn = nullptr;
	FVector HitLocation = FVector::ZeroVector;
	FVector UE1Momentum = FVector::ZeroVector;
	FName DamageType = NAME_None;
	class AWoTAngrealProjectile* SourceProjectile = nullptr;
	TSubclassOf<class AWoTAngrealProjectile> SourceProjClass;
	class AWoTAngrealInventory* SourceAngreal = nullptr;
	bool bDeleterious = true;
};

UENUM()
enum class EWoTPlayerMovementConstraint : uint8
{
	None,
	Immobilized,
	Grabbed,
	Impaled,
	LandingFreeze
};

// The MP player pawn, movement tuned to the shipped 333B spec
// (exports/script/WOT/WOTPlayer.uc defaultproperties; Engine PlayerPawn
// inherited values from the original game):
//   GroundSpeed=400  AirSpeed=400  WaterSpeed=200(inh)  AccelRate=2048
//   JumpZ=325(inh)   AirControl=1.0  MaxStepHeight=25(inh)
//   CollisionRadius=17  CollisionHeight=46(half)  BaseEyeHeight=42  Mass=175
//   Gravity=-950 (ZoneGravity)  GroundFriction=8 (ZoneGroundFriction)
// CMC is configured, not replaced — prediction/reconciliation
// for free. Feel validation against exports/reference/feel confirmed the
// jump arc (41-frame airtime => gravity -950, GravityScale set accordingly) and
// derived UE1's pure-exponential-friction braking (no constant decel term). The
// CMC retains that braking model; UWoTCharacterMovement ports retail's distinct
// held-Walking acceleration and 30% decay branch directly. See PORT_NOTES
// "Feel validation" + exports/reference/measurements.json.
// Artifact use: exec UseAngreal/CeaseUsingAngreal → server RPCs → the
// inventory component's cast gate (WOTPlayer's replication block routed
// ServerUseAngreal/CeaseUsingAngreal the same way).
UCLASS()
class AWoTCharacter : public ACharacter
{
	GENERATED_BODY()

public:
	AWoTCharacter(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
	static float DecayCenterViewPitch(float PitchDegrees, float DeltaSeconds);
	float CalculateStairLookPitch(float DeltaSeconds) const;

	UPROPERTY(VisibleAnywhere, Category = "WoT")
	TObjectPtr<class UWoTInventoryComponent> InventoryComponent;

	// HandSet/HandInfo are RemoteRole=None in the shipped game: this is an
	// owning-client view-model rebuilt over the replicated flat inventory.
	UPROPERTY(Transient) TObjectPtr<class UWoTHandSet> AngrealHandSet;
	// WOTPlayer.uc keeps both palettes and CurrentHandSet on the pawn. They are local
	// view-models: the controller supplies only owner-replicated edit state/count rows.
	UPROPERTY(Transient) TObjectPtr<class UWoTHandSet> CitadelEditorHandSet;
	UPROPERTY(Transient) TObjectPtr<class UWoTHandSet> CurrentHandSet;

	void EnsureAngrealHands();
	void ReconcileHandSet();
	void EnsureCitadelEditorHands();
	void ReconcileCitadelEditorHandSet();
	void HandleCitadelEditStateChanged(bool bEditing);
	bool IsCitadelEditorInputActive() const { return bCitadelEditorInputActive; }
	void NotifyItemAdded(FName ItemClassName);
	void NotifyHandSelected();

	// CitadelOrbitMode.BeginState (WOTPlayer.uc:5262-5272): while the player orbits the
	// citadel, the body runs the Ghost() entry code — no collision, hidden, PHYS_None,
	// UnderWaterTime=-1 — so teammates see no body and it cannot wander (or drag the
	// GetCitadelViewerLocation zone gates with it). EndState (WOTPlayer.uc:5277-5289)
	// restores collision/visibility/PHYS_Falling and the default UnderWaterTime (the
	// location restore is the separate EditStart path). Set on the AUTHORITY at the
	// controller's orbit-session edges; bHidden replicates natively, the collision +
	// movement halves reach clients through the OnRep (the AWoTTrap::bEditing pattern —
	// collision enable does not replicate).
	void SetCitadelOrbitGhost(bool bGhosted);
	bool IsCitadelOrbitGhosted() const { return bCitadelOrbitGhosted; }
	UPROPERTY(ReplicatedUsing = OnRep_CitadelOrbitGhosted)
	bool bCitadelOrbitGhosted = false;
	UFUNCTION() void OnRep_CitadelOrbitGhosted();

	// giMPBattle LeaveTeam/JoinTeam's replicated Actor.Style transition. This is the
	// visible, non-blocking neutral-player ghost and is deliberately independent from
	// the hidden/no-collision CitadelOrbitMode body ghost above. Authority sets it;
	// every client swaps/restores its local mesh materials through the RepNotify.
	void SetNeutralGhost(bool bGhosted);
	bool IsNeutralGhost() const { return bNeutralGhost; }
	UPROPERTY(ReplicatedUsing = OnRep_NeutralGhost, BlueprintReadOnly,
		Category = "WoT|Battle")
	bool bNeutralGhost = false;
	UFUNCTION() void OnRep_NeutralGhost();

	// WOTPlayer.DefaultAngrealInventory (WOTPlayer.uc:213): the pawn class's inherent
	// artifact, granted on every (re)spawn by giWOT.AddDefaultInventory (giWOT.uc:74-97).
	// Base WOTPlayer leaves it None; every MP faction pawn sets AngrealInvAirBurst
	// (WoTPawns/AesSedai.uc:88, Whitecloak.uc:82, Hound.uc:85, Forsaken.uc:84) — the
	// port's single AWoTCharacter stands in for all four, so its CDO carries AirBurst.
	// Charges seed from the granted class's own defaults (999/999 for Air Pulse,
	// AngrealInvAirBurst.uc:372-373), not a grant-site constant.
	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	TSubclassOf<class AWoTAngrealInventory> DefaultAngrealInventory;

	// The faction whose troop table a summon (ResourceSpawner) draws from. Each faction
	// class CDO owns its shipped identity; ApplyFactionAppearance copies it when Battle
	// morphs an existing pawn, matching WOTPlayer.GetTroop's PlayerClass.Name lookup.
	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	EWoTCreatureFaction SummonFaction = EWoTCreatureFaction::AesSedai;
	EWoTCreatureFaction GetSummonFaction() const { return SummonFaction; }

	// UE1 Actor.ScaleGlow, consumed by the runtime vertex-mesh light bake.
	// Data-driven creature rows override it when their leaf .uc authors a value.
	UPROPERTY(ReplicatedUsing=OnRep_ScaleGlow, EditDefaultsOnly, Category = "WoT|Display")
	float ScaleGlow = 1.0f;
	void SetScaleGlowPresentation(float NewScaleGlow);
	UFUNCTION() void OnRep_ScaleGlow();
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Display") uint8 AmbientGlow = 0;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Display") uint8 Visibility = 128;

	// --- Health / damage ---
	// Folds the default health reflectors the original routes damage through:
	// DefaultTakeDamageReflector (Pawn.TakeDamage: Health -= actualDamage, then
	// Died at <=0) and DefaultHealthReflector (IncreaseHealth clamps to 100).
	// Damage-modifying reflectors (shields, RarityBasedDamage) + armor plug into
	// ReduceDamage() when the reflector layer lands.
	UPROPERTY(Replicated, EditDefaultsOnly, Category = "WoT|Health") int32 Health = 0;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Health") int32 MaxHealth = 0;
	UPROPERTY(ReplicatedUsing = OnRep_bDead) bool bDead = false;
	// Engine.Pawn.Died's killing-blow Gibbed result.  Replicated so simulated
	// proxies suppress the ordinary death animation while the gib carcass marker
	// and chunks arrive.
	UPROPERTY(Replicated) bool bGibbedDeath = false;
	bool WasGibbedOnKillingBlow() const { return bGibbedDeath; }

	// Per-class CDO constants for MainHUD.DrawHealth. If the deferred runtime
	// disguise morph makes these mutable, that pass must add owner-only replication.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|HUD")
	TSoftObjectPtr<class UTexture2D> HealthIcons[4];

	// WOTPawn.uc:41 / WOTPlayer.uc:271 DisguiseIcon: the health icon shown to a
	// player who Disguises AS this pawn (GetDisguiseIcon, uc:2984). Per-class CDO
	// asset (Forsaken H_ForsakenDisguise, Archer H_ArcherDisguise, ...). Consumed
	// by GetHealthIcon's ApparentIcon short-circuit while disguised.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|HUD")
	TSoftObjectPtr<class UTexture2D> DisguiseIcon;

	static int32 HealthIconIndex(int32 InHealth, int32 InDefaultHealth);
	UTexture2D* GetHealthIcon() const;

	// IconInfo/FirstIcon fold. Only the authority mutates this list; the owning
	// client receives it in retail head-first order.
	UPROPERTY(Replicated)
	TArray<FWoTStatusIconEntry> StatusIcons;
	static constexpr ELifetimeCondition StatusIconsReplicationCondition()
	{
		return COND_OwnerOnly;
	}
	void AddIconInfo(UObject* Source, bool bGood, FName IconID);
	void RemoveIconInfo(FName IconID);

	// WOTPlayer.SeekerCount: only the owning client needs the HUD overlay.
	UPROPERTY(Replicated)
	int32 SeekerCount = 0;

	void IncrementSeekerCount();
	void DecrementSeekerCount();

	bool IsAlive() const { return Health > 0 && !bDead; }

	// Pawn.UnderWaterTime — seconds of breath once the head submerges. Base
	// Engine.Pawn leaves it UNSET (0 = never drowns); WOTPlayer authors 30
	// (WOTPlayer.uc:7283), WOTPawn 20 for every creature (WOTPawn.uc:1577 — the
	// NPC ctor overrides).
	UPROPERTY(EditAnywhere, Category = "WoT|Health") float UnderWaterTime = 30.0f;

	// The drowning half of UE1's shared Pawn.PainTime countdown (the engine tick
	// decrements it, PainTimer fires under 0.001). The port's
	// pain/heal zones keep their own per-zone occupant timers (AWoTZoneVolume), so
	// this pawn-level timer carries ONLY the breath/drown cadence; the pain-zone
	// interaction rules in UpdateBreath (arm-suppress / entry-clobber / underwater
	// pinning) reproduce the shared-timer observables. <=0 disarmed. Authority-only.
	float BreathTime = -1.0f;
	// Head submerge/surface (HeadZoneChange) + feet pain-zone entry (FootZoneChange
	// PainTime clobber) edge detectors. Authority-only.
	bool bWasHeadInWater = false;
	bool bWasFeetInPainZone = false;
	// FootZoneChange's splash half: the feet-in-water edge, the pawn's shared
	// 0.25s SplashTime throttle, and the zone whose
	// ExitSound plays on the way out. Authority-only.
	bool bWasFeetInWater = false;
	float LastSplashTime = -1.0f;
	TWeakObjectPtr<class AWoTZoneVolume> LastFootWaterZone;
	// True from the first drown tick until re-arm/disarm. UE1's bDrowning was
	// vestigial (cleared, never set); this drives the port's HUD
	// DROWNING display only.
	bool bDrowning = false;

	// Authority breath/drown update — called at the top of Tick (before the
	// locomotion driver's early-outs). Head probe = Location + BaseEyeHeight
	// (UE1 probed the live bob-smoothed EyeHeight; the port has no eye bob —
	// PORT_NOTES divergence).
	void UpdateBreath(float DeltaSeconds);

	// Pawn.Gasp override (WOTPlayer.uc:2187-2193 / WOTPawn.uc:985-991): the 'Gasp'
	// voice slot, authority-only. Reached only via the player-gated surface edge in
	// UpdateBreath (bIsPlayer — shipped NPCs never set it, so WOTPawn.Gasp is
	// unreachable in shipped play).
	void Gasp();

	// The noise seam every emission site routes through (engine AActor::MakeNoise for
	// any future PawnSensing listener + the Grunt.OnHearNoise AI-hearing fan-out,
	// AWoTNPCController::BroadcastNoise). The engine call runs everywhere the sites
	// did (landing prediction fires on clients too); the AI fan-out is authority-only.
	void EmitNoise(float Loudness);

	// WOTPlayer.TeamDescription (WOTPlayer.uc:307): the team name shown in the Citadel game
	// — a per-team-pawn-class default the faction subclasses set. In the WOTPlayer ->
	// controller+character split it lives on the pawn (the class varies per team). BattleInfo
	// reads it off TeamClass[Team]'s CDO (BattleInfo.GetTeamDescription). Runtime replication
	// of the live value is subsumed by the pawn-class replication (divergence, PORT_NOTES).
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "WoT|Team") FString TeamDescription;

	// WOTPlayer.uc:218-219 localized selection-feedback fragments (original/System/
	// WoT.int:45-46 — both keep their retail leading/trailing spaces). Composed by
	// the ClientSelectItemMessage port on every selection change.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|UI")
	FString SelectedStr;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|UI")
	FString TerAngrealStr;

	// --- Faction appearance ---
	// The four WoTPawns player classes are thin data-only subclasses (WoTFactionCharacters.h);
	// their shipped defaultproperties live on the CDO: the imported body mesh (soft path — the
	// vertex-anim component self-feeds its sequence table on bind), UE1 DrawScale (applied to
	// the MESH component; collision stays the capsule), and bIsFemale (voice selection).
	// Base *_Base skins ride the imported mesh's own materials; the
	// personal color-variant axis layers over them via PersonalSkin.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Team") FSoftObjectPath BodyMeshPath;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Team") float BodyDrawScale = 1.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Team") bool bIsFemale = false;

	// giMPBattle's mid-match faction morph (JoinTeam → ServerChangeClass →
	// ChangeClassModelStuff copies the new class's appearance onto the EXISTING pawn — UE1
	// replicated Mesh/Skin natively). When set, this class's CDO appearance overrides our
	// own class's; replicated so the morph reaches every client. A fresh spawn needs no
	// override — its class (TeamClass[Team] via GetDefaultPawnClassForController) already
	// carries the appearance.
	UPROPERTY(ReplicatedUsing = OnRep_AppearanceClass)
	TSubclassOf<AWoTCharacter> AppearanceClass;

	// Authority: morph to NewClass's appearance (no-op off authority).
	void SetAppearanceClass(TSubclassOf<AWoTCharacter> NewClass);
	UFUNCTION() void OnRep_AppearanceClass();
	// Bind the CDO appearance (AppearanceClass's if set, else our class's) onto this pawn.
	// Cosmetic + idempotent; runs on every role from BeginPlay/OnRep.
	void ApplyFactionAppearance();

	// WOTPlayer.PlayerColor (WOTPlayer.uc:280): the class-fixed identity tint darts,
	// LightGlobe, AppearEffects, reflectors, and the Seal indicator read. A per-class CDO
	// default (WOTPlayer PC_Blue :7263; Forsaken PC_Red; Hound PC_Purple; Whitecloak
	// PC_Gold) copied with the rest of the class identity on a faction morph
	// (ChangeClassModelStuff, uc:4112) — in the port that copy is subsumed by reading the
	// appearance class's CDO (WoTPlayerColorFX::Resolve), so no live copy is stored.
	// Never affected by the personal ?Skin= axis.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Team") FName PlayerColor = TEXT("PC_Blue");

	// --- The personal ?Skin= color-variant axis ---
	// WOTPlayer.SetSkin (uc:2733-2758): the accepted variant texture's OBJECT name
	// ("Aes_Blue"); NAME_None = the class-default Base skin. UE1 replicated Actor.Skin/
	// MultiSkins natively; the port replicates the name and every machine rebinds the
	// convention-named variant materials (MI_<Name> / MI_<Name>_2S mirroring the two body
	// slots — SetSkin filled EVERY MultiSkins slot with the one texture).
	UPROPERTY(ReplicatedUsing = OnRep_PersonalSkin)
	FName PersonalSkin;

	// WOTPlayer.SetMultiSkin (uc:4208-4220) + SetSkin folded: authority-only. Accepts
	// SkinName only when the game allows skin changes and the package-qualified name
	// starts with the pawn's UE1 mesh name (Left(SkinName, Len(MeshName)) ~= MeshName —
	// the mesh name is the imported body-mesh asset name, e.g. "AesSedai"). An accepted
	// name that fails to resolve resets to the class default (SetSkin(None)), exactly like
	// DynamicLoadObject returning None. Updates PlayerState->Color by the substring rules.
	void ServerSetPersonalSkin(const FString& SkinName);
	UFUNCTION() void OnRep_PersonalSkin();
	// Rebind the body slots' override materials for the current PersonalSkin (clears to
	// the authored Base materials when NAME_None). Cosmetic + idempotent, every role.
	void ApplyPersonalSkin();
	// WOTPlayer.SetSkin's PlayerReplicationInfo.Color classifier (uc:2752-2757): substring
	// of the texture name, first match wins — Black 1, Blue 2, Red 3, Green 4, else 0.
	static uint8 ClassifySkinColor(const FString& SkinTextureName);

	// This pawn's AnimationTable*.uc name in the WoTAnimTable registry (the shipped
	// AnimationTableClass= default). Faction subclasses set it in their ctor (players);
	// ConfigureNPC stamps the creature row's table (NPCs). NAME_None = no table — combat
	// anims fall back to their literal stand-ins. Not replicated: the authority picks
	// concrete variants and the picks ride the RepAnim channel.
	UPROPERTY(EditAnywhere, Category = "WoT|Anim")
	FName AnimTableName;

	// NPC hit-reaction tunables (WOTPawn.uc defaults :1547,:1556-1557; Archer overrides
	// via its creature row). A surviving NPC hit plays the 'Hit'/'HitHard' slot with
	// probability ChanceOfHitAnim; hard vs soft compares damage against ratios of the
	// pawn's authored default health (WOTPawn.uc:834-844 uses default.health, NOT the
	// rolled spawn value). WOTPlayer has its own deterministic eligibility below.
	UPROPERTY(EditAnywhere, Category = "WoT|Anim")
	float ChanceOfHitAnim = 0.25f;
	UPROPERTY(EditAnywhere, Category = "WoT|Anim")
	float HitHardHealthRatio = 0.25f;
	UPROPERTY(EditAnywhere, Category = "WoT|Anim")
	float HitSoftHealthRatio = 0.05f;
	// The default.health the hit ratios reference — creature rows stamp their
	// authored value; WOTPlayer decodes its inherited class default at startup.
	UPROPERTY(EditAnywhere, Category = "WoT|Anim")
	float HitReferenceHealth = 0.0f;
	bool ApplyRetailPlayerHealthDefaults(
		const FWoTRuntimeClassDefaults& Defaults, FString& OutError);
	bool PlayerHitQualifies(int32 Damage, float MomentumZ) const;

	// Pick and stamp the damage-sized hit-react slot. Production damage passes true
	// for bEnforceProductionEligibility: WOTPlayer.PlayHit (:2225-2251) then requires
	// Damage > 25% of default health or MomentumZ > 300, while NPCs retain
	// WOTPawn.PlayHit's ChanceOfHitAnim/soft-threshold rules (:968-1027). Direct calls
	// omit the flag and are presentation seams used by state-transition specs.
	virtual void PlayHitReactAnim(int32 Damage, FName WoTDamageType,
		float MomentumZ = 0.0f, bool bEnforceProductionEligibility = false);
	// Pawn.PlayTakeHitSound without the animation half. Mover encroachment uses
	// Damage=0, which deliberately selects HitSoft through the same pain gate.
	void PlayTakeHitVoice(float Damage, FName WoTDamageType);

	// AnimationTableImpl.TweenPlaySlotAnim: blend for the row's TweenTime (0.2
	// fallback), then play at its authored rate. Returns the concrete pick.
	const struct FWoTAnimTableEntry* StampSlotAnim(FName SlotName);
	// AnimationTableImpl.TweenLoopSlotAnim: resolve the slot and hold it as a
	// replicated loop. ShieldUser uses this for its crouching defensive idle.
	const struct FWoTAnimTableEntry* LoopSlotAnim(FName SlotName);
	// AnimationTableImpl.TweenSlotAnim: blend to frame zero and hold. Retail uses
	// this for the soft TakeHit flinch while movement continues.
	const struct FWoTAnimTableEntry* TweenSlotAnim(FName SlotName);

	// Seconds a sequence runs on this pawn's mesh at the given rate multiplier —
	// (NumFrames-1) / (authored fps * rate), the vertex-anim component's clock. <= 0
	// when the sequence isn't bound (no mesh / no content).
	float SequenceDurationSeconds(FName Sequence, float RateMultiplier) const;

	// Seconds between wraps for a looping sequence. Unlike one-shot playback,
	// the vertex animator advances through all NumFrames before fmod wraps to zero.
	float LoopSequenceDurationSeconds(FName Sequence, float RateMultiplier) const;

	// Hold the locomotion driver off while a one-shot (hit react / swing / teleport
	// wind-up) plays, then force a re-stamp of Run/Breath when it expires.
	void HoldLocomotionFor(float Seconds);

	virtual void BeginPlay() override;
	virtual void PostInitializeComponents() override;
	virtual void PossessedBy(AController* NewController) override;
	virtual void UnPossessed() override;
	virtual void OnRep_Controller() override;
	virtual void OnMovementModeChanged(EMovementMode PrevMovementMode,
		uint8 PreviousCustomMode = 0) override;
	virtual void TeleportSucceeded(bool bIsATest) override;
	// WOTZoneInfo ActorEntered/Leaving owner-view FOV seam.  The desired value is
	// retained until a controller exists, covering spawn-before-possession.
	void SetZoneFOV(float NewFOV);
	void ClearZoneFOV();
	float GetZoneFOVForLeech() const;
	void ReconcileOwnerLocalZoneFOV();
	void TeardownOwnerLocalZoneFOV();

	virtual float TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent,
	                         AController* EventInstigator, AActor* DamageCauser) override;
	// WOTPlayer.Landed (WOTPlayer.uc:2362): the side-normal ladder hook, then
	// normal-floor noise + fall damage ('fell', authority-only, impact <= -1100).
	// NPCs share the callback but skip the player-only ladder-normal gate and
	// override TakeFallingDamage with WOTPawn's thresholds.
	virtual void Landed(const FHitResult& Hit) override;

	// WOTPlayer's mutually-exclusive movement states. The replicated value is
	// authority-owned; an autonomous proxy predicts only through the transient
	// effective latch below and never writes this property.
	UPROPERTY(ReplicatedUsing = OnRep_PlayerMovementConstraint)
	EWoTPlayerMovementConstraint PlayerMovementConstraint =
		EWoTPlayerMovementConstraint::None;
	void SetPlayerMovementConstraint(EWoTPlayerMovementConstraint NewState);
	EWoTPlayerMovementConstraint GetEffectivePlayerMovementConstraint() const
	{
		return GetLocalRole() == ROLE_AutonomousProxy
			? EffectivePlayerMovementConstraint : PlayerMovementConstraint;
	}
	bool IsPlayerMovementConstrained() const
	{
		return GetEffectivePlayerMovementConstraint()
			!= EWoTPlayerMovementConstraint::None;
	}
	bool IsHardLanding() const
	{
		return GetEffectivePlayerMovementConstraint()
			== EWoTPlayerMovementConstraint::LandingFreeze;
	}
	float GetLastLandingImpactVelocity() const { return LastLandingImpactVelocityForTest; }
	float GetRetailStandingBaseEyeHeight() const { return RetailStandingBaseEyeHeight; }
	float GetLastSwimmingTweenSecondsForTest() const
	{
		return LastSwimmingTweenSecondsForTest;
	}
	UFUNCTION() void OnRep_PlayerMovementConstraint();
	// CharacterMovement calls this after applying a server correction. It is the
	// retail-equivalent re-convergence path when a client predicted an early exit
	// but authority still owns Grabbed/Impaled/LandingFreeze.
	void ReconcileEffectiveConstraintFromServerCorrection();
	// Impaled.UpdateRotation: called by the player controller after it has applied
	// the frame's actual RotationInput.
	void ApplyImpaledRotationAfterViewUpdate(float AppliedPitchInput);
	// Normal PlayerPawn.UpdateRotation restores actor pitch after BAT HeadToss's
	// one-shot presentation pulse. This must not depend on observing both edges
	// of a short replicated value, which network coalescing does not guarantee.
	void ClearCreaturePitchAfterNormalViewUpdate();
	// WOTPlayer.PlayerLanded: a major impact suppresses movement and casting until
	// the production Land animation's AnimEnd, using the same enum/latch mechanism.
	UFUNCTION() void HandleHardLandingAnimEnd(FName Sequence);
	UFUNCTION() void HandleSwimmingTweenAnimEnd(FName Sequence);
	void BeginHardLanding();
	void EndHardLanding();
	UPROPERTY(Transient) int32 AuthorityCreatureImpulseCountForTest = 0;
	UPROPERTY(Transient) FVector LastAuthorityCreatureImpulseForTest = FVector::ZeroVector;
	UPROPERTY(Transient) int32 ImpaledConstraintApplyCountForTest = 0;
	UPROPERTY(Transient) float LastLandingImpactVelocityForTest = 0.0f;
	UPROPERTY(Transient) int32 HardLandingEnterCountForTest = 0;
	UPROPERTY(Transient) float LastHardLandingCompletionSecondsForTest = 0.0f;
	UPROPERTY(Transient) float LastSwimmingTweenSecondsForTest = 0.0f;
	// PlayerPawn/WOTPlayer BaseEyeHeight state target. Captured before jump so
	// disguise-specific apparent eye heights restore after jump/landing.
	float RetailStandingBaseEyeHeight = 42.0f;

	// BAT HeadToss's authoritative actor-pitch presentation. CharacterMovement
	// carries simulation velocity; this value preserves the retail visual pitch
	// because character corrections serialize yaw but not actor pitch.
	UPROPERTY(Transient) float CreaturePresentationPitch = 0.0f;
	void SetCreaturePresentationPitch(float PitchDegrees);
	void ClearCreaturePresentationPitch();
	void ApplyCreaturePresentationPitch();
	bool IsCreaturePresentationPitchActive() const
	{
		return !FMath::IsNearlyZero(CreaturePresentationPitch);
	}
	UFUNCTION(NetMulticast, Reliable)
	void MulticastCreaturePresentationPitch(float PitchDegrees);
#if WITH_AUTOMATION_TESTS
	float LastCreaturePresentationPitchForTest = 0.0f;
	int32 CreaturePresentationPitchObserveCountForTest = 0;
	float LastCreaturePresentationRetainedActorPitchForTest = 0.0f;
	int32 CreaturePresentationPitchRetainCountForTest = 0;
	float LastCreaturePresentationClearedActorPitchForTest = 0.0f;
	int32 CreaturePresentationPitchClearCountForTest = 0;
#endif
	void RecordAuthorityCreatureImpulseForTest(const FVector& Delta)
	{
		++AuthorityCreatureImpulseCountForTest;
		LastAuthorityCreatureImpulseForTest = Delta;
	}
	// The Jump key held, for PlayerSwimming's aUp (User.ini "Jump | Axis aUp
	// Speed=+300.0"): set by AWoTPlayerController::Jump/StopJumping on the owning
	// machine; each frame's swim input turns it into upward acceleration.
	void SetSwimUpHeld(bool bHeld) { bSwimUpHeld = bHeld; }
#if WITH_AUTOMATION_TESTS
	void AddForwardInputForTest(float Value);
	void AddStrafeInputForTest(float Value) { AddStrafeInput(Value); }
	void AddSwimVerticalInputForTest() { AddSwimVerticalInput(); }
	void AddPitchInputForTest(float Value) { AddPitchInput(Value, true); }
#endif
	virtual void OnJumped_Implementation() override;

	// WOTPlayer defaults (:7157-7158), used by the side-normal ladder Landed
	// branch (:2348-2368). The shipped code intentionally makes noise below
	// the velocity threshold despite the variable's contradictory comment.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Movement")
	float LadderLandingNoiseVelocity = 200.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Movement")
	float LadderLandedHitNormalZ = 0.001f;
	static bool ShouldMakeLadderLandingNoise(
		const FVector& LandingVelocity, float NoiseVelocity);

	// eventWalkTexture equivalent driven by the compiled world-node trace.
	// The current sound path resolves its own material at notify time, while
	// this cache preserves the movement event and its exact BSP node for future
	// texture-event consumers.
	virtual void NotifyWalkSurface(int32 NodeIndex, float Friction,
		const FVector& HitLocation, const FVector& HitNormal);
	int32 LastWalkSurfaceNode = INDEX_NONE;
	float LastWalkSurfaceFriction = 1.0f;
	FVector LastWalkSurfaceLocation = FVector::ZeroVector;
	FVector LastWalkSurfaceNormal = FVector::UpVector;
	virtual void TakeFallingDamage(float VelocityZ);
	// Engine.Pawn.AddVelocity: a walking pawn enters Falling,
	// high upward speed halves an upward addition, then velocity is added.
	void AddUE1Velocity(FVector NewVelocity);
	// DefaultHealthReflector.IncreaseHealth — heal while alive, clamp to MaxHealth.
	void IncreaseHealth(int32 Amount);
	// Pawn.TakeDamage's ReduceDamage seam: shield/armor/damage reflectors reduce
	// incoming damage here (folds the reflector chain's TakeDamage interception).
	virtual int32 ReduceDamage(int32 Damage, FName WoTDamageType,
	                           const FVector& HitLocation,
	                           bool* bOutChainStopped = nullptr);
	// Reflector.uc:264-271: Priority descending, newest install first for ties.
	void GetOrderedReflectors(TArray<class UWoTReflector*>& OutReflectors) const;
	void RefreshTouchReflectorDescriptions(
		const class UWoTReflector* Excluded = nullptr);
	bool ShouldSilentlyRetireProjectile(
		const class AWoTGenericProjectile* Projectile) const;
	// The folded WOTPlayer TakeDamage reflector chain: element/shield fold ->
	// damage notifier (only when damage survives the shields) -> game-mode
	// ReduceDamage at the chain bottom -> health/Died. Both TakeDamage and
	// TakeRawDamage land here.
	int32 ApplyDamageChain(int32 Damage, FName WoTDamageType,
	                       AController* EventInstigator, AActor* DamageCauser,
	                       const FVector& HitLocation,
	                       const FVector& UE1Momentum = FVector::ZeroVector);
	// "Breaks all rules" damage that skips only the delivery-side ProcessEffect
	// interception (dome / element block / Absorb) — the original's
	// DamageEffect.Invoke() shortcut (Balefire). Invoke() is a full TakeDamage,
	// so the whole reflector chain above still applies.
	int32 TakeRawDamage(int32 Damage, FName WoTDamageType,
	                    AController* EventInstigator, const FVector& HitLocation,
	                    const FVector& UE1Momentum = FVector::ZeroVector);
	// DamageEffect/ProcessEffect: one ordered block/consume/pass walk, then the
	// default bottom invokes the complete TakeDamage pipeline.
	virtual int32 ProcessEffect(const FWoTDamageEffectSpec& Effect);
	// ProcessEffect interception: an installed reflector may throw away an incoming
	// effect — element shields block same-element weaves/leeches; an anti-magic dome
	// (NoBadEffects) blocks every deleterious one.
	// Set bEmitFeedback for a real incoming effect. Pure callers may leave it false
	// when they only need to query the reflector chain.
	bool IsEffectBlocked(const class AWoTAngrealInventory* Source, bool bDeleterious,
		bool bEmitFeedback = false);
	// UseAngreal interception: true if a reflector forbids casting (NoCast dome).
	bool IsCastingBlocked() const;
	// ChargeUsed interception: fan a charge-use notification out to the reflectors
	// (RarityBasedDamage self-damages on use). Called from the inventory component.
	void NotifyChargeUsed(class AWoTAngrealInventory* Ang);
	// LegionTargetingReflector.uc:8 (TargetableTypes(4)=None): when set, this pawn's
	// weaves EXCLUDE the LegionProjectile spirit family from their targetable set — the
	// Legion never locks a seeker onto a spirit. Set by ConfigureNPC from the creature
	// row; default false keeps the base DefaultTargetingReflector list for players and
	// every other creature.
	UPROPERTY() bool bExcludesLegionProjectileTarget = false;
	// True if an installed reflector overrides targeting (AutoTarget / Find Target).
	bool HasTargetingOverride() const;
	// True if an installed reflector bounces incoming weaves back (Reflect).
	bool ReflectsProjectiles() const;
	// True if an installed reflector forks incoming weaves back at the sender (Fork).
	bool ForksProjectiles() const;
	// A weave just hit us — offer it to the reflectors. True if one absorbed it
	// (Absorb: steal the firing artifact + cancel the hit).
	bool AbsorbProjectile(class AWoTAngrealProjectile* Proj);
	// Pawn.Died — death handling (full death anim + game-mode respawn deferred).
	virtual void Died(AController* Killer, FName WoTDamageType, const FVector& HitLocation);

	// The client-safe half of Died() — stop the corpse + disable capsule collision.
	// Callable on any role: Died() runs it authority-side, OnRep_bDead mirrors it to
	// simulated proxies (Died() itself is authority-only).
	virtual void ShutdownCorpsePresentation();

	// --- Carcass swap (WOTPawn.uc Dying 851-964) ---
	// The shipped CarcassType class path (WOTPawn default WOT.WOTCarcass; humanoids/
	// players WOTCarcassHumanoid; creature rows override). Carried onto the carcass
	// for the recipe/black-blood split; the swap spawns the one AWoTCarcass regardless.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Gore")
	FName CarcassTypeName = FName(TEXT("WOT.WOTCarcassHumanoid"));
	// WOTPawn gib thresholds (:1565-1568); creature rows override (BATrolloc/Legion).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Gore")
	float GibForSureFinalHealth = -40.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Gore")
	float GibSometimesFinalHealth = -30.0f;
	// Sleep(DestroyDelaySecs) before the hidden dead pawn actor goes away (:1559).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Gore")
	float DestroyDelaySecs = 10.0f;
	// DamageHelperBlackBlood (Minion.uc:497, Myrddraal.uc:606): this pawn bleeds
	// black — the damage spurt/carcass/gib configs swap to the Black set.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Gore")
	bool bBlackBlood = false;

	// The pawn's velocity at the instant of death, captured before the corpse halt —
	// the carcass inherits it (the tumble UE1 kept; PORT_NOTES corpse-halt note).
	UPROPERTY() FVector DeathVelocity = FVector::ZeroVector;

	// Spawn the carcass + hide this pawn now (PawnToCarcassTransition + DoSpawnCarcass,
	// WOTPawn.uc:864-881,:1148-1167). Authority-only, idempotent; bGibbed chunks the
	// fresh carcass immediately (the mid-Dying overkill path). Normally fired by the
	// death anim's carcass-notify timer; Destroyed() runs it early if Fire respawns the
	// player before that notify.
	void SpawnCarcassNow(bool bGibbed = false);
	virtual void Destroyed() override;

	// How far through its death anim this corpse is (0..1) — the carcass pose. Time-
	// derived, NOT the local mesh frame: a headless authority never plays the anim
	// locally, while clients are exactly this far through it when the swap replicates.
	float GetDeathPoseFraction() const;

	// NPC pawns override true: the corpse-window bookkeeping differs (NPCs linger
	// DestroyDelaySecs then self-destroy; players wait for respawn Fire). The old
	// IsPlayerControlled() check read false for an already-UnPossessed dead player.
	virtual bool IsNPCPawn() const { return false; }

	// Engine.Pawn.Skill: PreBeginPlay adds GameInfo.Difficulty
	// and clamps the effective value to 0..3. Native LineOfSightTo uses the
	// resulting value for its vertical-separation penalty.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|AI")
	float Skill = 0.0f;

	// Engine.Pawn's native LineOfSightTo scratch belongs to the observing
	// pawn. Keeping it here gives each pawn its own lifetime/GC boundary instead
	// of the former process-global weak-key map.
	bool bSightLOSFlag = false;
	float SightStimulus = 0.0f;
	FVector SightLastSeeingPos = FVector::ZeroVector;
	FVector SightLastSeenPos = FVector::ZeroVector;

	// --- Voice ---
	// The shipped SoundTableClass ref (WoTSoundTable registry key). Faction ctors set
	// theirs; ConfigureNPC stamps the creature row's. The three multipliers scale
	// every slot play (all 1.0 in shipped; the Warder cloak ducks volume via these).
	UPROPERTY(EditAnywhere, Category = "WoT|Sound")
	FName SoundTableName;
	// The pawn's looping AmbientSound hum (only Legion authors one — Leg_Ambient
	// 255/128/64; the carcass inherits the sound at Actor-default bytes).
	// Replicated so clients attach the local loop (the leech-hum surface shape);
	// lazily attached from Tick on every machine.
	UPROPERTY(Replicated) FName AmbientHumPath;
	UPROPERTY(Replicated) uint8 AmbientHumRadius = 32;
	UPROPERTY(Replicated) uint8 AmbientHumVolume = 128;
	UPROPERTY(Replicated) uint8 AmbientHumPitch = 64;
	UPROPERTY() TObjectPtr<class UWoTAmbientSoundComponent> AmbientHum;
	void UpdateAmbientHum();

	UPROPERTY(EditAnywhere, Category = "WoT|Sound") float SoundVolumeMultiplier = 1.0f;
	UPROPERTY(EditAnywhere, Category = "WoT|Sound") float SoundRadiusMultiplier = 1.0f;
	UPROPERTY(EditAnywhere, Category = "WoT|Sound") float SoundPitchMultiplier = 1.0f;
	// SoundSlotTimerList per-pawn state (mutable odds, NextSoundTime, and the
	// bReset odds->timed flip).
	TMap<FName, float> SoundSlotNextTime;
	TMap<FName, float> SoundSlotCurrentOdds;
	TSet<FName> SoundSlotsSwitchedToTimed;
	// PlayTakeHitSound's own pain gate (WOTPawn.uc:996-1015; 1-3s random window).
	float NextPainSoundTime = 0.0f;

	// Play a voice slot through the table + timer list + multipliers (authority;
	// networked). Convenience over WoTSoundTable::PlaySlotSound.
	bool PlayVoiceSlot(FName Slot, float VolumeMult = 1.0f);
	// bDead ReplicatedUsing handler: a proxy runs the corpse shutdown locally when it
	// learns the pawn died, so a killed pawn stops colliding/moving on every client.
	UFUNCTION() void OnRep_bDead();

	// --- Replicated vertex-anim channel (owner-gated) ---
	// Non-owners get server-driven anim; the owning client self-animates (the UE1
	// !bNetOwner owner gate -> COND_SkipOwner). Stamped on authority, replayed on a
	// proxy's UWoTVertexAnimComponent via OnRep_Anim; the probe reads the intent so
	// anim is observable regardless of mesh binding.
	UPROPERTY(ReplicatedUsing = OnRep_Anim) FWoTRepAnimState RepAnim;

	// WOTPlayer.ApparentClass (WOTPlayer.uc:267) — what a disguised player looks like. Replicated
	// OWNER-ONLY (`reliable if( Role==ROLE_Authority && bNetOwner )`, WOTPlayer.uc replication block):
	// it drives owning-client-LOCAL concerns (anim/movement scaling + HUD), NOT what fools OTHER
	// players — that rides the standard Actor display props (Mesh/skins/scale) the Disguise artifact
	// copies directly, which is the deferred presentation/AI-character-swap half. No OnRep yet (the
	// mesh/anim swap it would drive is that deferred half). Stamped/cleared by AWoTAngrealInvDisguise.
	// (The apparent TEAM stays server-only on the artifact — it's a `travel int`, not replicated.)
	UPROPERTY(Replicated) TSubclassOf<APawn> ApparentClass;

	// --- Disguise visible morph (AngrealInvDisguise, uc:179-458) ---
	// The replicated driver reproducing a mimicked pawn's LIVE look on every
	// client (see FWoTDisguiseState). COND_None: the visible morph fools OTHER
	// players (unlike the owner-only ApparentClass). Stamped/cleared by
	// AWoTAngrealInvDisguise via ApplyDisguiseFrom / RevertDisguise.
	UPROPERTY(ReplicatedUsing = OnRep_Disguise) FWoTDisguiseState Disguise;
	UFUNCTION() void OnRep_Disguise();

	// AcquireAppearance + CopyDisguisePawnProperties (uc:179-384): morph THIS pawn
	// to look like Source's LIVE appearance. Authority-only; builds the replicated
	// driver (every client rebuilds the look) after capturing the local revert
	// snapshot. Returns false on the won't-fit collision abort (uc:317-321) WITHOUT
	// morphing — the artifact then Fails the cast and drains no charge.
	bool ApplyDisguiseFrom(APawn* Source);
	// RevertAppearance / RestoreDisguisePawnProperties (uc:197-200,389-458): clear
	// the driver and restore the snapshot. Authority-only.
	void RevertDisguise();
	bool IsDisguised() const { return Disguise.bActive != 0; }
#if WITH_AUTOMATION_TESTS
	const FWoTDisguiseState& GetDisguiseStateForTest() const { return Disguise; }
	void SetPlayerAnimFireHeldForTesting(bool bHeld)
	{
		if (bPlayerAnimFireHeld != bHeld)
		{
			bPlayerAnimFireHeld = bHeld;
			DrivenLocomotionAnim = NAME_None;
		}
	}
	bool IsPlayerAnimFireHeldForTesting() const { return bPlayerAnimFireHeld; }
#endif
	// WOTPawn.SubstituteAnimName: translate player sequence names for the
	// apparent NPC mesh while disguised.
	static FName SubstituteDisguiseAnimName(FName PlayerAnim);
	// The pawn's effective appearance class chasing its own disguise/appearance
	// override (uc:281-283). Used when THIS pawn is itself a disguise source.
	TSubclassOf<AWoTCharacter> GetEffectiveAppearanceClass() const;
	FName GetEffectivePersonalSkin() const;

	// Owner-only breath mirror for the debug HUD (the ApparentClass COND_OwnerOnly
	// pattern): -1 = disarmed/head dry; N = whole seconds of breath left; 0 = the
	// drown-damage phase. Stamped by UpdateBreath; changes at most ~1/s.
	UPROPERTY(Replicated) int32 BreathDisplay = -1;

	// Authority: stamp the anim intent + bump the re-fire token. No-op off authority.
	void SetAnimIntent(FName Sequence, float Rate, bool bLoop);
	void SetTweenAnimIntent(FName Sequence, float Rate, float TweenTime,
	                       bool bTweenOnly);
	// Proxy: replay the replicated intent on the pawn's vertex-anim component if one
	// is present (headless / pre-mesh-bind: a safe no-op).
	UFUNCTION() void OnRep_Anim();
	// The replicated intent's sequence name (NAME_None if none) — the anim probe.
	FName GetReplicatedAnimName() const { return RepAnim.Sequence; }
	// IceLeech's Owner.AnimRate=0 / bAnimLoop=false fold. The accepted leech owns
	// the animation channel until thaw, after which normal NPC locomotion reselects.
	void SuspendNPCAnimationForIce();
	void ResumeNPCAnimationAfterIce();
	void RefreshNPCLocomotionAnimation();
	bool IsNPCAnimationSuspendedForIce() const { return bNPCAnimationSuspendedForIce; }

	// WOTPawn/WOTPlayer.SuicideInstigator: who to credit with the kill if this pawn dies soon
	// after being displaced into a hazard (Whirlwind / SwapPlaces). Consulted by
	// AWoTGameMode::ScoreKill within SuicideInstigationRelevance seconds of the displacement.
	// Stored as the instigator's *controller* — the persistent player identity in the UE5
	// split — not the pawn: the port replaces pawns on respawn (unlike UE1's persistent
	// PlayerPawn), so a stored pawn would lose its PlayerState if the caster died inside the
	// window; the original credits Killer.PlayerReplicationInfo, which here is the controller's
	// PlayerState. Server-side only (scoring is authoritative); weak so it nulls if they leave.
	UPROPERTY() TWeakObjectPtr<AController> SuicideInstigator;
	UPROPERTY() float SuicideInstigationTime = 0.0f;
	// WOTPawn.SetSuicideInstigator: record Other's controller as the instigator, stamped at
	// Level.TimeSeconds. Other is the caster's pawn (alive + possessed at the call site).
	void SetSuicideInstigator(APawn* Other);

	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	UPROPERTY(Replicated)
	TArray<FWoTTouchReflectorDescription> TouchReflectorDescriptions;
	bool bNPCAnimationSuspendedForIce = false;

	// Retail player animation driver: authority selects the shipped directional
	// movement/idle/swim state and stamps the existing replicated channel only on
	// transitions. Jump/fall/land remain one-shot tween poses owned by their event
	// edges. NPCs retain their pre-existing three-state locomotion driver.
	virtual void Tick(float DeltaSeconds) override;

	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;

	void UseAngreal();          // exec UseAngreal (fire pressed)
	void CeaseUsingAngreal();   // exec CeaseUsingAngreal (fire released)
	void NextInventory();
	void PrevInventory();
	UFUNCTION(Exec) void SelectHand(int32 Index);
	UFUNCTION(Exec) void NextHand();
	UFUNCTION(Exec) void PrevHand();
	UFUNCTION(Exec) void NextItem();
	UFUNCTION(Exec) void PrevItem();
	UFUNCTION(Exec) void Drop();
	UFUNCTION(Exec) void DropTainted();
	UFUNCTION(Exec) void DropSeals();

	// Retail cheat surface (WOTPlayer.uc:5924-5993, OKToCheat-gated: any time in
	// SP/standalone, MP needs admin). Remote bulk grants execute on the authority;
	// GiveAngreal is the SA()/UseSpecifiedAngreal-family single grant, taking
	// "Lightning" or "AngrealInvLightning".
	UFUNCTION(Exec) void AllAngreal(bool bInfiniteCharges = false);
	UFUNCTION(Exec) void AllAmmo();
	UFUNCTION(Exec) void GiveAngreal(const FString& Name);
	UFUNCTION(Exec) void SpawnAngreal(const FString& Name);
	bool OKToCheat() const;

	// WithValidation: the RPC framework calls _Validate on the authority before the
	// _Implementation and disconnects a client that fails. The aim
	// carries client data, so it is checked for NaN/Inf; the rest are parameterless.
	UFUNCTION(Server, Reliable, WithValidation) void ServerUseAngreal(FRotator AimRotation);
	UFUNCTION(Server, Reliable, WithValidation) void ServerCeaseUsingAngreal();
	UFUNCTION(Server, Reliable, WithValidation) void ServerNextInventory();
	UFUNCTION(Server, Reliable, WithValidation) void ServerPrevInventory();
	UFUNCTION(Server, Reliable, WithValidation) void ServerSelectAngreal(FName ClassName);
	UFUNCTION(Server, Reliable, WithValidation) void ServerDrop();
	UFUNCTION(Server, Reliable, WithValidation) void ServerDropTainted();
	UFUNCTION(Server, Reliable, WithValidation) void ServerDropSeals();
	UFUNCTION(Server, Reliable) void ServerAllAngreal(bool bInfiniteCharges);
	UFUNCTION(Server, Reliable) void ServerGiveAngreal(const FString& Name);
	UFUNCTION(Server, Reliable) void ServerSpawnAngreal(const FString& Name);

private:
	friend class UWoTInventoryComponent;
	void ApplyPendingZoneFOV();
	bool bHasZoneFOV = false;
	float PendingZoneFOV = 90.0f;

	enum class EStatusIconSourceKind : uint8
	{
		Angreal,
		Leech,
		Reflector
	};

	struct FStatusIconSourceRef
	{
		TWeakObjectPtr<UObject> Source;
		EStatusIconSourceKind Kind = EStatusIconSourceKind::Angreal;
	};

	// Server-side twin of StatusIcons. IconInfo.Tick polled the live source every
	// frame; weak refs preserve its stale-source -> RemainingDuration=0 behavior.
	TArray<FStatusIconSourceRef> StatusIconSources;
	void UpdateStatusIconDurations();
	void GrantAllAngreal(bool bInfiniteCharges);
	void DropSelectedInventory(bool bIgnoreInteractionGuards = false);
	// WOTPlayer.DropSeals' body: authority-only, also run by Died after bDead.
	void DropAllSeals(bool bIgnoreInteractionGuards = false);
	FVector GetPitDropLocation() const;

	void LandedOnLadder();
	void TweenToRunning(float TweenTime);
	// WOTPlayer.PerformSelection (WOTPlayer.uc:1009-1027): selection RPC + the owner-
	// local feedback — SelectHand/SelectArtifact sound (bNewHand picks which) and the
	// "Selected the ..." hand message.
	void DriveHandSelection(bool bNewHand);
	// WOTPlayer.ClientSelectItemMessage (WOTPlayer.uc:1176-1210).
	void SendSelectItemHandMessage(FName ItemName);
	void MoveForward(float Value);
	void MoveRight(float Value);
	void TurnInput(float Value);
	void KeyboardLookInput(float Value);
	void MouseXInput(float Value);
	void MouseYInput(float Value);
	void WalkingPressed();
	void WalkingReleased();
	void ResetWalkingInputState();
	void LookPressed();
	void LookReleased();
	void LookTogglePressed();
	void FreeLookPressed();
	void FreeLookReleased();
	void CenterViewPressed();
	void CenterViewReleased();
	void StrafePressed();
	void StrafeReleased();
	void AddForwardInput(float Value);
	void AddStrafeInput(float Value);
	void AddSwimVerticalInput();
	bool IsSwimmingForInput() const;
	bool bSwimUpHeld = false;
	void AddPitchInput(float Value, bool bFromLookDevice);
	float RetailFOVScale() const;
	bool IsLookActive() const { return bLookHeld || bLookToggled; }
	void CitadelOrbitZoom(float Value);
	// The shared collision/movement half of the orbit ghost — applied on the authority
	// from SetCitadelOrbitGhost and re-applied locally from OnRep_CitadelOrbitGhosted.
	void ApplyCitadelOrbitGhost();
	// Neutral Battle ghost presentation. Each slot gets a dynamic instance of the
	// shared skeletal modulate master carrying the original skin texture.
	void ApplyNeutralGhostLook();
	void RestoreNeutralGhostMaterials();

	// --- Disguise apply/revert (local, every role) ---
	// Capture this pawn's current look into SavedDisguise, then rebuild the driver's
	// source look on top; returns false on the won't-fit capsule abort (uc:317-321).
	bool ApplyDisguiseLook();
	// Restore the SavedDisguise snapshot (uc:389-458). Idempotent.
	void RestoreDisguiseLook();
	// SetSkin/SetMultiSkin against an explicit variant name (uc:4208) — lets the
	// disguise bind the SOURCE's skin instead of our own PersonalSkin member.
	void ApplySkinVariant(FName SkinName);
	// Set the capsule to the disguise size and lift/lower feet-on-ground by the
	// half-height delta, returning whether the pawn fits (Util.ActorFits, uc:205-222).
	bool AdjustCapsuleForDisguise(float NewRadius, float NewHalfHeight);
	// Local apply/revert edge latch + the restore snapshot.
	bool bDisguiseLookActive = false;
	UPROPERTY(Transient) FWoTSavedDisguise SavedDisguise;

	UPROPERTY(Transient)
	TArray<TObjectPtr<class UMaterialInterface>> NeutralGhostOriginalMaterials;
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UMaterialInstanceDynamic>> NeutralGhostMaterials;
	// Owner-local input context latch, driven by/reconciled from the owner edit-session edge.
	bool bCitadelEditorInputActive = false;
	// Suppresses Fire release when its corresponding press was consumed by the editor,
	// even if the edit session exits between those two hardware edges.
	bool bCitadelEditorFireHeld = false;
	// Retail PlayerPawn look state, projected from menuOptions preferences.
	bool bAlwaysMouseLook = true;
	bool bLookUpStairs = false;
	bool bSnapToLevel = false;
	bool bKeyboardLook = true;
	bool bExplicitLookInputThisFrame = false;
	bool bLookHeld = false;
	bool bLookToggled = false;
	bool bFreeLookHeld = false;
	bool bCenterView = false;
	bool bCenterViewHeld = false;
	bool bStrafeHeld = false;
	int32 WalkingPressCount = 0;
	FName RetailSwimTweenSequence = NAME_None;
	float RetailSwimLoopRate = 1.0f;

	// Last locomotion SLOT this authority/local-owner driver played, so it only
	// replays on a retail state/direction/fire change (role-local bookkeeping).
	FName DrivenLocomotionAnim = NAME_None;
	float DrivenLocomotionRateScale = 1.0f;
	float NPCIdleAnimRemaining = 0.0f;
	// TweenPlay/Tween edges are non-looping in UE1. Their AnimEnd enters the
	// corresponding loop; this countdown provides the same edge for the existing
	// compact replicated animation channel.
	float PlayerLocomotionTransitionRemaining = 0.0f;
	// WOTPlayer.bFire presentation latch. The server owns it alongside RepAnim;
	// press/release RPC edges invalidate DrivenLocomotionAnim so attack-movement
	// variants replace their non-firing counterparts exactly once.
	bool bPlayerAnimFireHeld = false;

	// Resolve WOTPlayer.SetMotionDirection + PlayRunning/PlayWalking/PlayWaiting/
	// PlaySwimming to the shipped animation-table slot. OutRateScale is the
	// GroundSpeed/ApparentClass.default.GroundSpeed multiplier used only by
	// grounded movement loops.
	FName SelectPlayerLocomotionSlot(
		const class UCharacterMovementComponent* Move,
		float& OutRateScale) const;
	// WOTPlayer's TweenSlotAnim event path for jump, walk-off fall, minor land,
	// and swim entry. Plays locally where a mesh exists and stamps RepAnim on the
	// authority; the locomotion hold prevents the next Tick from replacing it.
	void PlayPlayerTweenPose(FName SlotName, float CallerTween = 0.0f);

	// World seconds until which the locomotion driver stays quiet so a one-shot combat
	// anim (hit react / swing / death pick) isn't stomped mid-play. The driver re-stamps
	// the locomotion loop once the hold expires (the FinishAnim -> LoopMovementAnim
	// shape of the original's states). Authority-only bookkeeping.
	float LocomotionHoldUntil = 0.0f;

	// The 5d loop-notify scheduler (authority): the looping locomotion sequence's
	// sound notifies (footsteps, idle barks) fire cyclically at their fractions.
	TArray<struct FWoTAnimNotify> LoopNotifies;
	float LoopDuration = 0.0f;
	float LoopElapsed = 0.0f;
	FName LoopSequence;
	void FireLoopNotify(FName Function);

	// The texture-keyed footstep / landing thud (GenericTextureHelper port,
	// WoTFootsteps registry): trace the floor, key the surface off the hit
	// material's imported name (fallback Stone), pick + perturb + play the
	// networked one-shot + MakeNoise. Authority-only.
	void PlaySurfaceSound(EWoTFootstepGroup Group, float VolumeMult);

	// Carcass-swap bookkeeping (authority-only).
	bool bCarcassSpawned = false;
	float DeathAnimStartTime = 0.0f;
	float DeathAnimDuration = 0.0f;
	FTimerHandle CarcassSwapTimer;
	FTimerHandle CorpseDestroyTimer;
	FTimerHandle HardLandingTimer;
	uint8 CreaturePresentationPitchUpdatesRemaining = 0;
	FName HardLandingSequence;
	UPROPERTY(Transient)
	EWoTPlayerMovementConstraint EffectivePlayerMovementConstraint =
		EWoTPlayerMovementConstraint::None;
	void SetPredictedPlayerMovementConstraint(
		EWoTPlayerMovementConstraint NewState);
	void ExitPlayerMovementConstraint();
	void ApplyPlayerMovementConstraintTransition(
		EWoTPlayerMovementConstraint OldState,
		EWoTPlayerMovementConstraint NewState);
	void ApplyPlayerMovementConstraint(EWoTPlayerMovementConstraint State);
	void StartHardLandingPresentation();
	void StopHardLandingPresentation();
	void TrackAuthorityLandingAnimation(FName Sequence, float Rate,
		float TweenTime, bool bTweenOnly, bool bLoop);
};
