#pragma once

#include "CoreMinimal.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "WoTCharacterMovement.generated.h"

// Native result seam for the retail CheckSurfaces trace. It is transient
// movement data rather than reflected asset state.
struct FWoTWalkSurfaceHit
{
	int32 NodeIndex = INDEX_NONE;
	float Friction = 1.0f;
	FVector Location = FVector::ZeroVector;
	FVector Normal = FVector::UpVector;
};

// The port's CharacterMovementComponent subclass: ALL zone movement effects live on the
// predicted path here, where they run IDENTICALLY on the authority and in the client's
// replayed saved moves — so an owned pawn in a zone computes the same move on both sides
// and does not rubber-band.
//
// Retail zone semantics (UE1 zone physics; every effect below is derived from the retail
// movement code, not invented):
//   - ZoneVelocity is TRANSPORT: it displaces the pawn's move each tick and is NEVER
//     accumulated into Velocity. Falling and flying add the full vector to the move;
//     walking and swimming add ZoneVelocity*25*dt (the retail frame-scaled form — kept
//     as-is, including its tick-rate dependence), walking horizontal-only. Player-class
//     pawns are always transported; other pawns only when the current is strong
//     (falling > 200 uu/s, otherwise > 300 uu/s). Applied post-physics per mode in the
//     Phys* overrides (ApplyZoneEffectsPostPhys).
//   - ZoneGravity REPLACES gravity for occupants of a gravity zone: GetGravityZ() returns
//     the zone's vertical gravity, which the engine's own falling integrator consumes
//     (this is the path whose Z survives PhysFalling — a Z contribution made inside
//     CalcVelocity is discarded there, the bug the transport rework fixed). The lateral
//     part of an arbitrary-direction zone gravity integrates in the falling override.
//   - ZoneGroundFriction swaps onto the CMC while inside (some water zones author 0.6 —
//     slippery wading); the pawn's own friction is restored outside.
//   - ZoneFluidFriction drives the retail non-braking calcVelocity branch used by
//     ladders and swimming (default 1.2; authored water zones can use 0.6).
//   - Swim mode: a water zone puts the pawn in MOVE_Swimming, leaving drops it to
//     MOVE_Falling. Retail swimming queries WoT zones for buoyancy, head damping
//     and waterline exits; collision-limited displacement determines velocity.
//     CMC's Buoyancy stores retail pawn buoyancy (mass units, divided by Mass),
//     not UE5's dimensionless immersion multiplier. No PhysicsVolume is needed.
//   - Legend CheckSurfaces runs before walking physics (and at UE5's separate falling
//     dispatch so the retail jump-onto-ladder branch remains reachable): world-BSP texture
//     traces occluded by nearer/equal movers, pitch-biased fly-for-this-tick motion, and
//     low-friction gravity slide.
//
// Map-placed zones are level actors on both sides, so no replication is needed for the
// effects to be live in real gameplay (a runtime-spawned zone would need replication to
// reach a client, but that is only a test convenience — PORT_NOTES).
UCLASS()
class UWoTCharacterMovement : public UCharacterMovementComponent
{
	GENERATED_BODY()

public:
	// Pure retail ladder-direction seam for specs. Caller invokes it only when
	// Acceleration is nonzero: ViewRotation gets the +4096 UE1-unit (+22.5 deg)
	// pitch bias, Z comes from GroundSpeed, and positive biased pitch damps XY.
	static FVector ClimbVelocity(const FVector& CurrentVelocity,
		const FRotator& ViewRotation, float GroundSpeed);

	// APawn::calcVelocity's bFluid=1, bBrake=0 branch used by physFlying
	// in UE1. AccelerationLimit is the retail
	// AccelRate and SpeedLimit is AirSpeed (already DesiredSpeed-adjusted for this
	// port's NPCs). Swimming supplies buoyant gravity, added AFTER fluid damping
	// and BEFORE the speed cap. Flying leaves it zero.
	// Exposed so one-tick specs can pin the integration directly.
	static FVector RetailFlyingVelocity(const FVector& CurrentVelocity,
		const FVector& InputAcceleration, float AccelerationLimit,
		float SpeedLimit, float FluidFriction, float DeltaTime,
		const FVector& BuoyantAcceleration = FVector::ZeroVector);

	// Retail TraceTexture seam, including the TRACE_VisBlocking mover gate.
	// Tests call this exact path with a spawned blocking mover component.
	static bool TraceWalkSurface(const class UWoTMapLightTable* Table,
		UWorld* World, const AActor* SourceActor, const FVector& Start,
		const FVector& End, FWoTWalkSurfaceHit& OutHit);
#if WITH_AUTOMATION_TESTS
	void SetAccelerationForTesting(const FVector& Value) { Acceleration=Value; }
	FVector GetAccelerationForTesting() const { return Acceleration; }
	/** Sets the acceleration and returns the analog modifier it implies. */
	float ComputeAnalogInputModifierForTest(const FVector& InAcceleration);
#endif
	void SetWalkingRequested(bool bRequested) { bWantsToWalk = bRequested; }
	bool IsWalkingRequested() const { return bWantsToWalk; }
	bool IsRetailWalking() const;

	virtual void UpdateCharacterStateBeforeMovement(float DeltaSeconds) override;
	virtual void CalcVelocity(float DeltaTime, float Friction, bool bFluid,
		float BrakingDeceleration) override;
	virtual bool CanWalkOffLedges() const override;
	virtual void UpdateFromCompressedFlags(uint8 Flags) override;
	virtual class FNetworkPredictionData_Client* GetPredictionData_Client() const override;
	virtual float GetGravityZ() const override;
	virtual float GetMaxSpeed() const override;
	virtual float ComputeAnalogInputModifier() const override;
	virtual void ClientHandleMoveResponse(
		const FCharacterMoveResponseDataContainer& MoveResponse) override;
	virtual bool StepUp(const FVector& GravDir, const FVector& Delta,
		const FHitResult& Hit,
		FStepDownResult* OutStepDownResult = nullptr) override;
	int32 GetClientCorrectionCountForTest() const
	{
		return ClientCorrectionCountForTest;
	}
	virtual void PhysWalking(float DeltaTime, int32 Iterations) override;
	virtual void PhysFalling(float DeltaTime, int32 Iterations) override;
	virtual void PhysFlying(float DeltaTime, int32 Iterations) override;
	virtual void PhysSwimming(float DeltaTime, int32 Iterations) override;
	virtual bool IsInWater() const override;

	// A movement base the remote side can name: a replicated component, or a
	// name-stable component of a replicated / net-addressable actor. The guarded
	// runtime world BSP (AWoTRuntimeBSPWorld's RuntimeBSPComponent_NNN) is built
	// locally on every machine and is neither, yet it is Movable, so UE sends
	// base-RELATIVE corrections against it that the client can never resolve
	// (ClientAdjustPosition_Implementation then drops every one: the D3
	// SwapPlaces / teleport desync).
	static bool IsNetResolvableMovementBase(const UObject* Base);
	// Rewrites a pending base-relative correction whose base the client cannot
	// resolve into the absolute form UE already uses for static/no bases.
	// Returns true when it changed the adjustment. Pure: specs drive it.
	static bool AbsolutizeUnresolvableAdjustment(FClientAdjustment& Adjustment,
		const UObject* Base, const FVector& WorldLocationZeroOrigin,
		const FVector& WorldVelocity);

	// The client -> server half: a WALKING client whose base arrived null (it has
	// no NetGUID) sent its location relative to its own copy of the locally
	// projected geometry; read it through the server's copy when the server's base
	// is that kind of base. An airborne client sends an absolute location.
	static bool ShouldReadClientLocationThroughServerBase(
		EMovementMode ClientMode, bool bClientBaseValid, const UObject* ServerBase,
		bool bServerBaseDynamic);

	using UCharacterMovementComponent::ServerMoveHandleClientError;
	virtual void ServerMoveHandleClientError(float ClientTimeStamp, float DeltaTime,
		const FVector& Accel, const FVector& RelativeClientLocation,
		FMovementBaseInterfaceData* ClientMovementBaseInterfaceData,
		FName ClientBaseBoneName, uint8 ClientMovementMode) override;

private:
	int32 ClientCorrectionCountForTest = 0;
	// Legend CheckSurfaces. True means a climbable
	// surface ran flying physics for this tick and the caller must return.
	bool CheckWalkSurfaces(float DeltaTime, int32 Iterations);
	class UWoTMapLightTable* GetWalkSurfaceTable();
	void PhysLadder(float DeltaTime, int32 Iterations);
	// Retail Swim/findWaterLine: sweep, rewind to the
	// dry side of the waterline, return the fraction of the move spent in air.
	float SwimInZone(const FVector& Delta, FHitResult& Hit);
	// Retail swim-exit water jump: PlayerSwimming.ZoneChange's bUpAndOut +
	// CheckWaterJump test (engine PlayerPawn and Pawn).
	void TryRetailWaterJump();
	bool CheckRetailWaterJump() const;

	// True while the pawn's location is inside a WoT water zone (bWaterZone). Drives the swim
	// enter/exit decision and the PhysSwimming self-exit guard; shared by IsInWater().
	bool InWaterZone() const;

	// The per-mode zone transport (+ lateral zone gravity while falling), run after the
	// mode's physics. ForMode guards against a mid-physics mode change double-applying.
	void ApplyZoneEffectsPostPhys(float DeltaTime, EMovementMode ForMode,
		bool bUseFlyingSemantics = false);

	// The pawn's OWN ground friction (8), captured LAZILY on the first movement update —
	// before any zone overrides it — and restored when the pawn is outside all zones. Lazy
	// (not InitializeComponent) so it is robust to component-init ordering: a wrong base
	// would clobber the parity defaults.
	bool bCapturedBase = false;
	float BaseGroundFriction = 8.0f;
	bool bWantsToWalk = false;

	// Per-pawn strong reference avoids a synchronous asset lookup every physics
	// tick after success. Failed/GC-expired loads retry, matching the shared
	// light-table resolver, so an early tick cannot permanently disable surfaces.
	UPROPERTY(Transient)
	TObjectPtr<class UWoTMapLightTable> WalkSurfaceTable;
};
