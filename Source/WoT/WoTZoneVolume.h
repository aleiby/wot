#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Subsystems/WorldSubsystem.h"
#include "WoTZoneVolume.generated.h"

// Per-world index of the placed zone volumes. The CMC's predicted path queries the
// zones 3-4 times per pawn per movement tick (GetMovementEffectAt / IsPointInWater);
// iterating this registry is O(#zones ~2-5) where a TActorIterator walk is O(all
// actors in the world). Volumes register in BeginPlay / deregister in EndPlay.
//
// It also re-evaluates every weave's water zone once per frame, after movement
// (UE1 SetActorZone is point-based and runs after every move whatever the
// collision; see AWoTGenericProjectile::UpdateWaterZone).
UCLASS()
class UWoTZoneIndex : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	UPROPERTY() TArray<TObjectPtr<class AWoTZoneVolume>> Zones;

	virtual void Tick(float DeltaTime) override;
	// ULevel::SetActorZone equivalent.  The null zone is LevelInfo/zone zero.
	// Region dispatch is simulated; pawn foot/head handlers are authority-only.
	void UpdateActorZones(AActor* Actor, bool bForceRefresh = false);
	// The UE5 port replaces player pawns. Dispatch the same ordered leave callbacks
	// as a retail SetActorZone before erasing a still-live tracked actor.
	void RemoveActor(AActor* Actor);
	// Explicit UE1 SetLocation/FarMove seam for non-tick relocations. Ordinary
	// movement is refreshed once by this subsystem's post-movement Tick.
	static void RefreshAfterRelocation(AActor* Actor);
	class AWoTZoneVolume* GetRegion(const AActor* Actor) const;
	class AWoTZoneVolume* GetFootRegion(const APawn* Pawn) const;
	class AWoTZoneVolume* GetHeadRegion(const APawn* Pawn) const;
	virtual TStatId GetStatId() const override
	{
		RETURN_QUICK_DECLARE_CYCLE_STAT(UWoTZoneIndex, STATGROUP_Tickables);
	}

	// R3a: corpses that die outside every authored zone are accounted to this
	// world-global bucket (Engine.ZoneInfo.MaxCarcasses default 3).
	int32 WorldCarcassCount = 0;
	int32 WorldMaxCarcasses = 3;

#if WITH_AUTOMATION_TESTS
	int32 TrackedActorCountForTest() const { return ActorZones.Num(); }
	void ResetUpdateCountsForTest() { UpdateCountsForTest.Reset(); }
	int32 UpdateCountForTest(const AActor* Actor) const
	{
		if (const int32* Count = UpdateCountsForTest.Find(Actor)) return *Count;
		return 0;
	}
#endif

private:
	struct FActorZones
	{
		TWeakObjectPtr<class AWoTZoneVolume> Region;
		TWeakObjectPtr<class AWoTZoneVolume> FootRegion;
		TWeakObjectPtr<class AWoTZoneVolume> HeadRegion;
		bool bInitialized = false;
		// Retained independently of the weak actor key so Tick can reconcile a
		// missed destruction after the pawn itself has become invalid.
		bool bCountedPlayer = false;
	};
	TMap<TWeakObjectPtr<AActor>, FActorZones> ActorZones;
#if WITH_AUTOMATION_TESTS
	TMap<TWeakObjectPtr<AActor>, int32> UpdateCountsForTest;
#endif
};

// The movement-affecting effect of the zones at a world point — the data-source view
// of AWoTZoneVolume consumed by UWoTCharacterMovement's predicted path. Plain struct
// (not replicated/reflected): computed each frame from the zone actors, which are
// present on both client and server (map-placed zones are level actors on both sides,
// so no replication is needed for the fix to be live in real gameplay).
struct FWoTZoneMovement
{
	FVector Current = FVector::ZeroVector;   // summed ZoneVelocity of the point's zones
	bool bOverrideGravity = false;            // a zone overrides gravity
	FVector Gravity = FVector::ZeroVector;    // summed ZoneGravity (applied instead of normal)
	float GroundFriction = 8.0f;              // the point's zone ZoneGroundFriction
	float FluidFriction = 1.2f;               // current ZoneFluidFriction (LevelInfo default)
	bool bWater = false;                      // a water zone here -> swim
};

// Port of Engine.ZoneInfo's gameplay effects (WOT.WOTZoneInfo) as an explicit
// UE5 volume. Spec: the original engine's ZoneInfo + its zone physics.
//
// UE1 zones are IMPLICIT — a ZoneInfo point actor tags a BSP region bounded by
// PF_Portal zone-partition polys, and every actor in that region reads the
// ZoneInfo's props. We fold that to an EXPLICIT box volume carrying the same
// props; entering it applies the effect. One volume = one effect-bearing
// ZoneInfo (water / pain / gravity / current can co-exist on one, exactly as the
// original ZoneInfo does).
//
// PHASE 1 (this): the effects + a box extent (specs author it; real maps get
// their extent in Phase 2). Self-managed rather than native APhysicsVolume so it
// is deterministic and testable headless — swimming is driven by MOVE_Swimming on
// entry rather than bWaterVolume (divergence; PORT_NOTES). PHASE 2: replace the
// box with the faithful per-zone convex geometry reconstructed from the compiled
// .wot BSP (Nodes[].iZone + Zones[].ZoneActor), placed by import_map.py.
UCLASS()
class AWoTZoneVolume : public AActor
{
	GENERATED_BODY()

public:
	AWoTZoneVolume();

	// The box extent of the zone (Phase 2 upgrades to convex-from-BSP).
	UPROPERTY(VisibleAnywhere, Category = "WoT|Zone")
	TObjectPtr<class UBoxComponent> Collision;

	// --- ZoneInfo effect props (parity-locked from the exported ZoneInfo) ---

	// bWaterZone: occupants swim; weaves entering fizzle (HitWater).
	UPROPERTY(EditAnywhere, Category = "WoT|Zone") bool bWaterZone = false;
	// WOTZoneInfo.bPitZone: dropped seals return to the linked pit opening.
	UPROPERTY(EditAnywhere, Category = "WoT|Zone") bool bPitZone = false;
	UPROPERTY(EditAnywhere, Category = "WoT|Zone") float Buoyancy = 1.0f;

	// ZoneInfo.EntrySound/ExitSound (imported-wave subpaths; Engine defaults are
	// None — only ~16 water zones across 8 MP maps author them, e.g.
	// AmbientA/DIVE + AmbientA/Emerge2). Played by the pawn's FootZoneChange
	// splash (AWoTCharacter::UpdateBreath). The shipped EntryActor/ExitActor
	// chain (WOT.WaterImpact -> WaterRing) renders NOTHING in retail 333B —
	// mesh-less shells — so the splash is sound-only (PORT_NOTES).
	UPROPERTY(EditAnywhere, Category = "WoT|Zone") FName EntrySoundSubPath;
	UPROPERTY(EditAnywhere, Category = "WoT|Zone") FName ExitSoundSubPath;

	// bFogZone: retail renders volumetric fog only while the VIEWER stands in
	// a fog zone — no gameplay effect; consumed by UWoTFogGateSubsystem.
	UPROPERTY(EditAnywhere, Category = "WoT|Zone") bool bFogZone = false;
	// ZoneInfo.ViewFog, added to PlayerPawn.ViewFlash while the viewer's head is
	// in this zone (water defaults carry the shipped blue/green tint).
	UPROPERTY(EditAnywhere, Category = "WoT|Zone") FVector ViewFog = FVector::ZeroVector;
	// ZoneInfo.ViewFlash contributes its X component to PlayerPawn's scalar
	// FlashScale. WaterZone defaults all axes to -0.078.
	UPROPERTY(EditAnywhere, Category = "WoT|Zone") FVector ViewFlash = FVector::ZeroVector;

	// WOTZoneInfo player-entry effects.
	UPROPERTY(EditAnywhere, Category = "WoT|Zone") bool bFOVZone = false;
	UPROPERTY(EditAnywhere, Category = "WoT|Zone") float ZoneFOV = 90.0f;
	UPROPERTY(EditAnywhere, Category = "WoT|Zone") bool bLeechZone = false;
	// Engine.ZoneInfo.ZonePlayerEvent: first player in triggers the tagged actors;
	// the last player out untriggers them.
	UPROPERTY(EditAnywhere, Category = "WoT|Zone") FName ZonePlayerEvent;
	UPROPERTY(EditAnywhere, Category = "WoT|Zone")
	TArray<TSubclassOf<class UWoTLeech>> LeechClasses;
	UPROPERTY(EditAnywhere, Category = "WoT|Zone|Timer")
	TArray<float> TimerLeechDuration;
	UPROPERTY(EditAnywhere, Category = "WoT|Zone|Timer")
	TSoftObjectPtr<class UTexture2D> TimerLeechIcon;
	UPROPERTY(EditAnywhere, Category = "WoT|Zone|Timer") bool bPersistentTimer = false;
	UPROPERTY(EditAnywhere, Category = "WoT|Zone|Timer") bool bRemoveTimerOnExit = false;
	UPROPERTY(EditAnywhere, Category = "WoT|Zone|Timer") bool bTimerOnRight = false;
	UPROPERTY(EditAnywhere, Category = "WoT|Zone|Timer") bool bOnlyTimeOnce = false;
	bool bAttachedTimer = false;

	// ZoneVelocity: a constant current/wind TRANSPORTING occupants — it displaces the move
	// (UWoTCharacterMovement's transport model), never accumulates into velocity.
	UPROPERTY(EditAnywhere, Category = "WoT|Zone") FVector ZoneVelocity = FVector::ZeroVector;

	// ZoneGravity: overrides gravity for occupants (arbitrary direction).
	// (Named bOverrideGravity, not bZoneGravity, so it doesn't collide with
	// ZoneGravity's pythonized name — UE strips the bool's `b` prefix.)
	UPROPERTY(EditAnywhere, Category = "WoT|Zone") bool bOverrideGravity = false;
	UPROPERTY(EditAnywhere, Category = "WoT|Zone") FVector ZoneGravity = FVector(0, 0, -950.0f);

	// bPainZone: DamagePerSec of DamageType per pain tick, scaled by immersion
	// (0.4 feet + 0.4 body + 0.2 head — UE1 Pawn.PainTimer). The per-pawn
	// timer arms 0.01s after the FEET enter (Pawn.FootZoneChange), then
	// re-arms every PainInterval (UE1 PainTime = 1.0).
	UPROPERTY(EditAnywhere, Category = "WoT|Zone") bool bPainZone = false;
	UPROPERTY(EditAnywhere, Category = "WoT|Zone") bool bDestructive = false;
	UPROPERTY(EditAnywhere, Category = "WoT|Zone") float DamagePerSec = 0.0f;
	// ZoneInfo.DamageType (AEFWS name — Arena pain pools author xxFxx, so a Fire
	// Shield eats the burn). Class default is None (UE1's ZoneInfo sets no
	// DamageType in defaultproperties); unauthored zone pain is untyped — the old
	// 'Drowned' hardcode was wrong.
	UPROPERTY(EditAnywhere, Category = "WoT|Zone") FName DamageType;
	UPROPERTY(EditAnywhere, Category = "WoT|Zone") float PainInterval = 1.0f;

	// bHealZone (WOTZoneInfo.uc): +HealPerSec health on the same PainTime cadence
	// while the feet are in the zone (WOTPlayer/WOTPawn.uc PainTimer), capped at
	// health 100 and hard-capped at 100. HealSoundPath is player-only
	// per WOTPawn.uc:513 (NPCs heal silently).
	UPROPERTY(EditAnywhere, Category = "WoT|Zone") bool bHealZone = false;
	UPROPERTY(EditAnywhere, Category = "WoT|Zone") int32 HealPerSec = 0;
	// Subpath under /Game/WoT/Sounds, e.g. "Angreal/ActivateHL".
	UPROPERTY(EditAnywhere, Category = "WoT|Zone") FName HealSoundPath;

	// R3a carcass budget (Engine.ZoneInfo.MaxCarcasses): the max corpses a zone holds
	// before eviction, and the live running count kept by AWoTCarcass InitFor/EndPlay.
	UPROPERTY(EditAnywhere, Category = "WoT|Zone") int32 MaxCarcasses = 3;
	int32 NumCarcasses = 0;

	// ZoneGroundFriction: UE1 walking friction while in the zone (ZoneInfo
	// default 8; some water zones author 0.6 — slippery wading). Pushed onto the
	// CMC on entry; the pawn's prior GroundFriction is restored on exit.
	UPROPERTY(EditAnywhere, Category = "WoT|Zone") float ZoneGroundFriction = 8.0f;

	// ZoneFluidFriction: retail physFlying/calcVelocity damping. Engine
	// ZoneInfo defaults to 1.2; authored water zones can override it (0.6).
	UPROPERTY(EditAnywhere, Category = "WoT|Zone") float ZoneFluidFriction = 1.2f;

	// WOTZoneInfo CitadelEditor metadata. UE1's EditViewAngle Pitch=-8192 is -45 degrees.
	UPROPERTY(EditAnywhere, Category = "WoT|Zone|CitadelEditor") uint8 EditTeam = 255;
	UPROPERTY(EditAnywhere, Category = "WoT|Zone|CitadelEditor") uint8 MinEditDistance = 0;
	UPROPERTY(EditAnywhere, Category = "WoT|Zone|CitadelEditor")
	FRotator EditViewAngle = FRotator(-45.0f, 0.0f, 0.0f);
	UPROPERTY(EditAnywhere, Category = "WoT|Zone|CitadelEditor")
	float EditViewDistance = 32.0f;
	UPROPERTY(EditAnywhere, Category = "WoT|Zone|CitadelEditor") bool bAllowEditing = true;

	// Compiled UModel zone number. INDEX_NONE means an editor/test volume and
	// deliberately selects the AABB fallback.
	UPROPERTY(VisibleAnywhere, Category = "WoT|Zone") int32 ProjectedZoneIndex = INDEX_NONE;

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;
	virtual void Tick(float DeltaTime) override;

#if WITH_AUTOMATION_TESTS
	int32 ZonePlayerCountForTest() const { return ZonePlayerCount; }
#endif

	// Is a world-space point inside this volume's box? (UE1 PointRegion-in-zone.)
	bool ContainsPoint(const FVector& Point) const;
	static AWoTZoneVolume* ZoneAt(const UWorld* World, const FVector& Point);

	// Simulated WOTZoneInfo events, called by UWoTZoneIndex in retail order.
	void ActorEntered(AActor* Other);
	void ActorLeaving(AActor* Other);
	// Camera-only SmoothRand leeches follow the owning controller, not authority.
	// These seams are deliberately separate from ActorEntered/Leaving so a
	// possession reconciliation cannot touch ZonePlayerEvent occupancy.
	void ReconcileOwnerLocalFOV(class AWoTCharacter* Player);
	void DetachOwnerLocalFOV(class AWoTCharacter* Player);

	// Is a world-space point inside any water zone? (The ZoneInfo Region.Zone.bWaterZone
	// test — used by Shift's force-fall skip and Chain Lightning's water shock.)
	static bool IsPointInWater(const UWorld* World, const FVector& Point);

	// Is a world-space point inside any pain zone? (UE1 Region.Zone.bPainZone — the
	// breath/PainTime arm-reset gates check the FLAG alone, not DamagePerSec;
	// UE1 Pawn.FootZoneChange/HeadZoneChange.)
	static bool IsPointInPainZone(const UWorld* World, const FVector& Point);

	// The water zone containing a point (null if none) — the splash needs the
	// ZONE for its Entry/Exit sounds, not just the flag.
	static AWoTZoneVolume* WaterZoneAt(const UWorld* World, const FVector& Point);

	// The pain zone containing a point (null if none) — callers that must read
	// DamagePerSec (UE1's Fragment gates its insta-destroy on DamagePerSec > 0,
	// not the bare flag) need the ZONE, not just IsPointInPainZone's flag test.
	static AWoTZoneVolume* PainZoneAt(const UWorld* World, const FVector& Point);

	// The pit zone containing a point (WOTPlayer.Drop's Region.Zone.bPitZone).
	static AWoTZoneVolume* PitZoneAt(const UWorld* World, const FVector& Point);

	// The innermost editor zone at Point. Smallest scaled box volume wins; equal-volume
	// overlaps retain registry order, approximating UE1's exactly-one BSP zone.
	static AWoTZoneVolume* EditZoneAt(const UWorld* World, const FVector& Point);

	// The zones' movement effect at a world point (data source for UWoTCharacterMovement's
	// predicted path): summed current, gravity override, ground/fluid friction, and swim flag of
	// the zones containing the point. Returns
	// true if the point is inside ANY zone (so the CMC knows to apply / revert). Static so
	// the CMC queries without holding a zone ref (mirrors IsPointInWater).
	static bool GetMovementEffectAt(const UWorld* World, const FVector& Point, FWoTZoneMovement& Out);
	/** Retail gravity for non-pawn falling bodies at Point. */
	static FVector GetGravityAt(const UWorld* World, const FVector& Point);

private:
	friend class UWoTZoneIndex;
	void RemoveCountedPlayer(class AWoTCharacter* Player);
	// Per-occupant state: the UE1 pain/heal timer only (Pawn.PainTime — armed by the feet, not
	// the capsule). ALL movement effects (current, gravity, ground friction, swim mode) are the
	// CMC's job now (UWoTCharacterMovement's predicted path), so no movement save/restore is
	// kept here.
	struct FOccupantState
	{
		float PainTime = 0.0f;           // counts down; <=0 while disarmed
		bool bFootIn = false;            // feet-in-zone edge detector
	};
	TMap<TWeakObjectPtr<APawn>, FOccupantState> Occupants;
	// Engine.ZoneInfo.ZonePlayerCount is authority-owned and gates only the
	// first-entry / last-exit event transitions.
	int32 ZonePlayerCount = 0;
};
