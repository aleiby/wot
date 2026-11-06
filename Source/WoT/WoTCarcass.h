#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WoTRetailProjectileTarget.h"
#include "WoTCarcass.generated.h"

class UCapsuleComponent;
class UWoTVertexAnimComponent;
class AWoTCharacter;

// The dynamic corpse (WOTCarcass.uc): spawned at the death anim's carcass notify, it
// COPIES the dying pawn's display (mesh, scale, frozen death pose — the original's
// CopyDisplaySettings) and inherits its death velocity, then falls/bounces, lies flat,
// flattens its collision cylinder, and cleans up on the corpse-count-scaled timer.
// Damage accumulates toward the gib threshold; ChunkUp is the 4c seam (chunks/blood),
// a plain destroy until then. Presentation Slice 4a; per-creature *decorative* carcass
// leaves are level props, not this class (plan Slice 4 premise 1).
UCLASS()
class AWoTCarcass : public AActor, public IWoTRetailProjectileTarget
{
	GENERATED_BODY()

public:
	AWoTCarcass();

	// InitFor (WOTCarcass.uc:96-165): copy the dead pawn's display + physics seeds.
	// Authority-only (the display payload replicates; OnRep re-derives presentation).
	void InitFor(AWoTCharacter& Dead);

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;
	virtual float TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent,
	                         AController* EventInstigator, AActor* DamageCauser) override;
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// The gib (WOTCarcass.uc:339-369): spawn the recipe's replacement chunk +
	// odds-gated extras (4c), then vanish. The gib blood burst is the 4d seam.
	virtual void ChunkUp(float Damage);

	// The imported-mesh layout resolver (shared with the chunks).
	static USkeletalMesh* ResolveMeshByName(FName MeshName);

	float GetSourceCollisionHeight() const { return SourceCollisionHeight; }

	// The WOTCarcassBlack family (Minion/Myrddraal): black blood + black gib skins.
	bool IsBlackBloodFamily() const
	{
		const FString Name = CarcassTypeName.ToString();
		return Name.Contains(TEXT("Black")) || Name.Contains(TEXT("Minion")) ||
		       Name.Contains(TEXT("Myrddraal"));
	}

	UPROPERTY(VisibleAnywhere, Category = "WoT")
	TObjectPtr<UCapsuleComponent> Capsule;

	UPROPERTY(VisibleAnywhere, Category = "WoT")
	TObjectPtr<UWoTVertexAnimComponent> Mesh;

	// --- Replicated display payload (replicate the values, re-derive the
	// presentation — the ItemState template). Set once by InitFor. ---
	UPROPERTY(ReplicatedUsing = OnRep_Display)
	FName MeshName;              // imported mesh asset name ("Trolloc")
	UPROPERTY(Replicated)
	FName PoseSequence;          // the death variant that was playing
	UPROPERTY(Replicated)
	float PoseFraction = 1.0f;   // UE1 AnimFrame at the swap instant
	UPROPERTY(Replicated)
	float MeshScale = 1.0f;
	UPROPERTY(Replicated)
	uint8 MeshFatness = 128;
	UPROPERTY(Replicated)
	FSoftObjectPath MeshActorSkin;
	// Parallel replicated arrays keep the exact UE1 MultiSkins indices; TMap is
	// reconstructed locally after the display mesh binds.
	UPROPERTY(Replicated)
	TArray<int32> MeshSkinIndices;
	UPROPERTY(Replicated)
	TArray<FSoftObjectPath> MeshSkinPaths;
	// The dead pawn's looping hum (WOTCarcassLegion.uc:113 keeps Leg_Ambient) —
	// attached in OnRep_Display at Actor-DEFAULT bytes 32/128/64 (the carcass
	// authors none, unlike the live pawn's radius 255).
	UPROPERTY(Replicated)
	FName AmbientHumPath;
	UPROPERTY() TObjectPtr<class UWoTAmbientSoundComponent> AmbientHum;
	UFUNCTION() void OnRep_Display();

	// AdjustCylinder's mesh counter-offset (the capsule shifts under the body; the
	// mesh stays put). Authority-set; replicated so client corpses render where the
	// server's do (review note — movement replication moves the ROOT only).
	UPROPERTY(ReplicatedUsing = OnRep_MeshOffset)
	FVector MeshRelativeOffset = FVector::ZeroVector;
	UFUNCTION() void OnRep_MeshOffset();

	// --- Carried source data (server-side; 4c reads the recipe split off these) ---
	// The shipped CarcassType class path from the pawn/row ("WOT.WOTCarcassHumanoid").
	UPROPERTY() FName CarcassTypeName;
	// The pawn's AnimationTable (GetAnimEndVector drives the cylinder shift).
	UPROPERTY() FName AnimTableName;

	// --- Tuning (WOTCarcass.uc defaultproperties + Engine.Carcass Dead) ---
	UPROPERTY(EditDefaultsOnly, Category = "WoT") float BaseGibDamage = 40.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT") float GibForSureCumulativeDamage = 40.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT") float CarcassCCRadiusMultiplier = 1.2f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT") float CarcassCCHeightMultiplier = 0.4f;

	bool IsLyingStill() const { return !bFalling; }
	bool WasChunkedUp() const { return bChunkedUp; }
	virtual bool IsRetailProjectileTarget() const override { return !bChunkedUp && !bSinking; }

	// WOTCarcass Mass = Other.Mass (the dead pawn's CMC Mass — the same source the
	// grab draw-in reads). Only the Legion's carcass reaches the >= 500 LandThump
	// gate (ThrowOthers). Public so headless specs can seed a heavy carcass directly.
	UPROPERTY(EditAnywhere, Category = "WoT") float Mass = 175.0f;
	UPROPERTY(EditAnywhere, Category = "WoT|Zone") float Buoyancy = 105.0f;
	UPROPERTY(EditAnywhere, Category = "WoT|Zone") float TimeBetweenZoneChecks = 1.0f;
	UPROPERTY(EditAnywhere, Category = "WoT|Zone") float PainZoneDamageScale = 1.0f;
	UPROPERTY(EditAnywhere, Category = "WoT|Zone") bool bTakesDamage = true;
	void AddUE1Velocity(const FVector& DeltaVelocity);
	// Simulated WOTCarcassBase.ZoneChange. Gameplay damage inside it remains
	// authority-only, matching the called CheckZone function.
	void HandleZoneChange(class AWoTZoneVolume* NewZone,
		class AWoTZoneVolume* OldZone);

	// Tag == 'Balefired' (WOTCarcass.uc:154/185/343): the corpse of a balefired
	// pawn is inert — it never falls, never bleeds/gibs on damage — and is removed
	// by the Balefire sweep, not the normal cleanup. Carried through Actor Tags.
	bool IsBalefired() const { return Tags.Contains(FName(TEXT("Balefired"))); }

private:
	// PHYS_Falling: manual gravity + sweep; HitWall reflect x0.7 / Vz x0.9 / stop
	// under 120 (WOTCarcass.uc:575-584); Landed gibs under -1000, else lies flat
	// (:505-532) and flattens/shifts the cylinder (AdjustCylinder, :536-571).
	void Landed(const FHitResult& Hit);
	void HitWall(const FVector& HitNormal);
	void AdjustCylinder();
	void StartLegionSinking(AActor* LandedBase);
	void ResetLegionSinkForBaseChange();
	UFUNCTION() void OnRep_Sinking();
	void ScheduleCleanup();
	void CleanupTimerFired();
	bool PlayerCanSeeMe() const;

	// LandThump (WOTCarcass.uc:241-260) fires once when the corpse comes to rest;
	// a Mass >= 500 corpse (the Legion) throws every nearby player — ThrowOthers
	// (:212-237): a distance-scaled view shake on each, plus an AddVelocity kick to
	// walking players within 1500uu. Authority-side sim + owner-client shake RPC.
	void LandThump();
	void ThrowOthers();
	void CheckZone(class AWoTZoneVolume* Zone, float DeltaSeconds);

	// R3a zone-scoped carcass budget (WOTCarcass.uc:57/173/433-469): the corpse is
	// accounted to the zone it died in (or a world-global bucket if in no authored
	// zone); when the zone exceeds MaxCarcasses a corpse is evicted, and the cleanup
	// cadence scales with the ZONE count (max(12, 30 - 2 * count)).
	void CheckZoneCarcasses();
	int32 GetZoneCarcassCount() const;
	int32 GetZoneMaxCarcasses() const;
	void AdjustZoneCount(int32 Delta);
	bool SameZoneBucket(const AWoTCarcass& Other) const;

public:
#if WITH_AUTOMATION_TESTS
	// Specs bypass InitFor (no dead pawn) and seed the fall directly.
	void SetVelocityForTest(const FVector& InVelocity) { Velocity = InVelocity; }
	const FVector& GetVelocityForTest() const { return Velocity; }
	bool WasWaterFlattenedForTest() const { return bWaterCollisionFlattened; }
	bool HasSplashedForTest() const { return bSplash; }
	bool IsSinkingForTest() const { return bSinking; }
	float GetSinkRateForTest() const { return ActiveSinkRatePerSec; }
	float GetSinkDistanceForTest() const { return TotalSinkDistance; }
	AActor* GetSinkingBaseForTest() const { return SinkingBase.Get(); }
#endif

private:
	UPROPERTY() FVector Velocity = FVector::ZeroVector;
	bool bFalling = true;
	bool bChunkedUp = false;
	bool bThumped = false;
	bool bSplash = false;
	bool bWaterCollisionFlattened = false;
	// WOTCarcassLegion sinking state. Start/reset dispatch is added with the
	// engine-event parity implementation; these defaults deliberately do nothing.
	UPROPERTY(ReplicatedUsing = OnRep_Sinking)
	bool bSinking = false;
	float ActiveSinkRatePerSec = 0.0f;
	// Physical displacement never resets; TotalSinkDistance is the retail
	// BaseChange-reset destruction budget.
	float PhysicalSinkDistance = 0.0f;
	float TotalSinkDistance = 0.0f;
	float LogicalLandedHalfHeight = 0.0f;
	TWeakObjectPtr<AActor> SinkingBase;
	bool bHadSinkingBase = false;
	FVector LastSinkingBaseLocation = FVector::ZeroVector;
	FRotator LastSinkingBaseRotation = FRotator::ZeroRotator;
	FRotator RotationRate = FRotator::ZeroRotator;
	float NextZoneCheckTime = 0.0f;
	bool bCountedInZone = false;
	float CumulativeDamage = 0.0f;
	float SourceCollisionRadius = 17.0f;
	float SourceCollisionHeight = 46.0f;
	// The zone this corpse is accounted to (the death zone). Null == world bucket.
	TWeakObjectPtr<class AWoTZoneVolume> DeathZone;
	FTimerHandle CleanupTimer;
};
