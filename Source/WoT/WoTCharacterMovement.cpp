#include "WoTCharacterMovement.h"

#include "AI/WoTNPCCharacter.h"
#include "WoTCharacter.h"
#include "WoTMapLightTable.h"
#include "WoTMeshLight.h"
#include "WoTMover.h"
#include "WoTZoneVolume.h"

#include "Components/PrimitiveComponent.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementReplication.h"

namespace
{
constexpr float ClimbableFriction = 10.0f;
constexpr float FallingTooFast = -150.0f;
constexpr float UE1PitchBiasDegrees = 22.5f; // 4096 / 65536 of one turn.

class FSavedMove_WoT final : public FSavedMove_Character
{
public:
	virtual void Clear() override
	{
		FSavedMove_Character::Clear();
		bSavedWantsToWalk = false;
	}

	virtual uint8 GetCompressedFlags() const override
	{
		return FSavedMove_Character::GetCompressedFlags()
			| (bSavedWantsToWalk ? FLAG_Custom_0 : 0);
	}

	virtual bool CanCombineWith(const FSavedMovePtr& NewMove,
		ACharacter* CharacterPawn, float MaxDelta) const override
	{
		const FSavedMove_WoT* Other = static_cast<const FSavedMove_WoT*>(NewMove.Get());
		return Other && bSavedWantsToWalk == Other->bSavedWantsToWalk
			&& FSavedMove_Character::CanCombineWith(NewMove, CharacterPawn, MaxDelta);
	}

	virtual void SetMoveFor(ACharacter* CharacterPawn, float InDeltaTime,
		FVector const& NewAccel,
		FNetworkPredictionData_Client_Character& ClientData) override
	{
		FSavedMove_Character::SetMoveFor(
			CharacterPawn, InDeltaTime, NewAccel, ClientData);
		// HeadToss pitch is a one-update presentation event. Do not combine its
		// saved move with a neighbor: rollback/replay of the combined move can
		// restore the event rotation after the normal update already cleared it.
		if (const AWoTCharacter* WoTCharacter =
			Cast<AWoTCharacter>(CharacterPawn);
			WoTCharacter && WoTCharacter->IsCreaturePresentationPitchActive())
		{
			bForceNoCombine = true;
		}
		const UWoTCharacterMovement* Movement = CharacterPawn
			? Cast<UWoTCharacterMovement>(CharacterPawn->GetCharacterMovement()) : nullptr;
		bSavedWantsToWalk = Movement && Movement->IsWalkingRequested();
	}

	virtual void PrepMoveFor(ACharacter* CharacterPawn) override
	{
		FSavedMove_Character::PrepMoveFor(CharacterPawn);
		if (UWoTCharacterMovement* Movement = CharacterPawn
			? Cast<UWoTCharacterMovement>(CharacterPawn->GetCharacterMovement()) : nullptr)
		{
			Movement->SetWalkingRequested(bSavedWantsToWalk);
		}
	}

private:
	bool bSavedWantsToWalk = false;
};

class FNetworkPredictionData_Client_WoT final
	: public FNetworkPredictionData_Client_Character
{
public:
	explicit FNetworkPredictionData_Client_WoT(
		const UCharacterMovementComponent& ClientMovement)
		: FNetworkPredictionData_Client_Character(ClientMovement)
	{
	}

	virtual FSavedMovePtr AllocateNewMove() override
	{
		return FSavedMovePtr(new FSavedMove_WoT());
	}
};

bool MoverOccludesBspHit(UWorld* World, const AActor* SourceActor,
	const FVector& Start, const FVector& End, double BspTime,
	const FVector& BspFacingNormal)
{
	if (!World)
	{
		return false;
	}
	const FVector WorldHitLocation = Start + (End - Start) * BspTime;

	FCollisionQueryParams QueryParams(
		SCENE_QUERY_STAT(WoTWalkSurfaceMover), true);
	if (SourceActor)
	{
		QueryParams.AddIgnoredActor(SourceActor);
	}

	// The port's movers are AWoTMover-family actors whose BlockAll collision
	// lives on primitive components (normally AWoTMover::Mesh). Trace those
	// components directly: querying BlockAll's WorldStatic object type through
	// the world would let the imported BSP stop the query before a coplanar
	// mover could win. Retail TRACE_VisBlocking explicitly compares level and
	// mover hits, then selects the nearest.
	for (TActorIterator<AWoTMover> It(World); It; ++It)
	{
		AWoTMover* Mover = *It;
		if (!IsValid(Mover) || Mover == SourceActor
			|| (SourceActor && SourceActor->IsOwnedBy(Mover)))
		{
			continue;
		}

		TInlineComponentArray<UPrimitiveComponent*> Components;
		Mover->GetComponents(Components);
		for (UPrimitiveComponent* Component : Components)
		{
			if (!IsValid(Component) || !Component->IsQueryCollisionEnabled()
				|| Component->GetCollisionResponseToChannel(ECC_Visibility) != ECR_Block)
			{
				continue;
			}
			FHitResult MoverHit(1.0f);
			// LineTraceComponent is a body-instance query: its return value IS
			// the hit verdict; it fills the geometric fields but never stamps
			// bBlockingHit (a world-scene-query concept) — do not filter on it.
			if (!Component->LineTraceComponent(MoverHit, Start, End, QueryParams))
			{
				continue;
			}
			if (MoverHit.Time < BspTime)
			{
				return true;
			}
			// A mover hit sharing the world hit's normal and lying within 2uu is
			// biased 2uu forward so it takes precedence over the coincident BSP
			// result, as in UE1. The port adds a normal tolerance
			// because Chaos and the BSP plane derive the normal independently.
			if (FVector::DistSquared(MoverHit.Location, WorldHitLocation) < 4.0
				&& FVector::DotProduct(FVector(MoverHit.Normal), BspFacingNormal) > 0.99)
			{
				return true;
			}
		}
	}
	return false;
}
}

bool UWoTCharacterMovement::IsRetailWalking() const
{
	return bWantsToWalk && !InWaterZone();
}

void UWoTCharacterMovement::CalcVelocity(float DeltaTime, float Friction,
	bool bFluid, float BrakingDeceleration)
{
	if (!IsRetailWalking()
		|| (MovementMode != MOVE_Walking && MovementMode != MOVE_NavWalking))
	{
		Super::CalcVelocity(DeltaTime, Friction, bFluid, BrakingDeceleration);
		return;
	}

	// UE1's APawn::calcVelocity: braking retains the
	// established UE5 port, while accelerating follows retail's vector update
	// directly and deliberately has no hard max-speed clamp.
	const float EffectiveFriction = FMath::Max(bFluid ? 1.0f : 0.0f, Friction);
	if (Acceleration.IsZero())
	{
		Super::CalcVelocity(DeltaTime, Friction, bFluid, BrakingDeceleration);
	}
	else
	{
		const FVector AccelDir = Acceleration.GetSafeNormal();
		const float AccelRate = GetMaxAcceleration();
		if (Acceleration.SizeSquared() > 0.09f * AccelRate * AccelRate)
		{
			Acceleration = AccelDir * AccelRate * 0.3f;
		}
		const float VelSize = Velocity.Size();
		Velocity -= (Velocity - AccelDir * VelSize)
			* DeltaTime * EffectiveFriction;
		Velocity = Velocity * (1.0f - (bFluid ? 1.0f : 0.0f)
			* Friction * DeltaTime) + Acceleration * DeltaTime;
	}

	const float MaxSpeed = GetMaxSpeed();
	if (Velocity.SizeSquared() > 0.09f * MaxSpeed * MaxSpeed)
	{
		const float Speed = Velocity.Size();
		Velocity = Velocity / Speed * FMath::Max(0.3f * MaxSpeed,
			Speed * (1.0f - DeltaTime * 2.0f * EffectiveFriction));
	}
}

bool UWoTCharacterMovement::CanWalkOffLedges() const
{
	return !IsRetailWalking() && Super::CanWalkOffLedges();
}

void UWoTCharacterMovement::UpdateFromCompressedFlags(uint8 Flags)
{
	Super::UpdateFromCompressedFlags(Flags);
	bWantsToWalk = (Flags & FSavedMove_Character::FLAG_Custom_0) != 0;
}

FNetworkPredictionData_Client* UWoTCharacterMovement::GetPredictionData_Client() const
{
	if (!ClientPredictionData)
	{
		UWoTCharacterMovement* MutableThis = const_cast<UWoTCharacterMovement*>(this);
		MutableThis->ClientPredictionData =
			new FNetworkPredictionData_Client_WoT(*this);
	}
	return ClientPredictionData;
}

bool UWoTCharacterMovement::TraceWalkSurface(const UWoTMapLightTable* Table,
	UWorld* World, const AActor* SourceActor, const FVector& Start,
	const FVector& End, FWoTWalkSurfaceHit& OutHit)
{
	const int32 NodeIndex = UWoTMeshLight::FirstHitNode(Table, Start, End);
	if (NodeIndex == INDEX_NONE || !Table->ZoneBspPlanes.IsValidIndex(NodeIndex))
	{
		return false;
	}
	const FVector4& Plane = Table->ZoneBspPlanes[NodeIndex];
	const FVector Normal(Plane.X, Plane.Y, Plane.Z);
	const FVector Delta = End - Start;
	const double Denominator = FVector::DotProduct(Delta, Normal);
	double Time = 0.0;
	if (!FMath::IsNearlyZero(Denominator))
	{
		Time = (Plane.W - FVector::DotProduct(Start, Normal)) / Denominator;
	}
	Time = FMath::Clamp(Time, 0.0, 1.0);
	// Model::LineCheck reports the impact-side (start-facing) normal; the
	// coplanar comparison must use that orientation, not the raw node plane.
	const FVector FacingNormal = Denominator > 0.0
		? FVector(-Normal.GetSafeNormal()) : Normal.GetSafeNormal();
	if (MoverOccludesBspHit(World, SourceActor, Start, End, Time, FacingNormal))
	{
		return false;
	}

	// UModel::LineCheck backs the raw split hit up by 0.5uu along the ray
	// in UE1. Compare movers BEFORE this presentation offset so
	// an actually coplanar mover and BSP node retain retail's mover-wins tie.
	const double Length = Delta.Size();
	if (Length > UE_SMALL_NUMBER)
	{
		Time -= 0.5 / Length;
	}
	Time = FMath::Clamp(Time, 0.0, 1.0);
	OutHit.NodeIndex = NodeIndex;
	OutHit.Friction = UWoTMeshLight::FrictionForNode(Table, NodeIndex);
	OutHit.Location = Start + Delta * Time;
	// Retail copies Model.Nodes(iHit).Plane directly; it does not face-flip it.
	OutHit.Normal = Normal.GetSafeNormal();
	return true;
}

FVector UWoTCharacterMovement::ClimbVelocity(const FVector& CurrentVelocity,
	const FRotator& ViewRotation, float GroundSpeed)
{
	FRotator BiasedRotation = ViewRotation;
	BiasedRotation.Pitch += UE1PitchBiasDegrees;
	FVector Result = CurrentVelocity;
	Result.Z = GroundSpeed * BiasedRotation.Vector().Z;
	if (BiasedRotation.Pitch > 0.0f)
	{
		Result.X *= 0.8f;
		Result.Y *= 0.8f;
	}
	return Result;
}

FVector UWoTCharacterMovement::RetailFlyingVelocity(
	const FVector& CurrentVelocity, const FVector& InputAcceleration,
	float AccelerationLimit, float SpeedLimit, float FluidFriction, float DeltaTime,
	const FVector& BuoyantAcceleration)
{
	// Exact relevant APawn::calcVelocity branch for physFlying's
	// bFluid=1,bBrake=0 call:
	// cap Acceleration to AccelRate, turn the current velocity toward
	// AccelDir with max(1,friction), apply the fluid multiplier and
	// acceleration step, then cap to AirSpeed.
	const FVector AccelDir = InputAcceleration.IsZero()
		? FVector::ZeroVector : InputAcceleration.GetSafeNormal();
	FVector LimitedAcceleration = InputAcceleration;
	if (LimitedAcceleration.SizeSquared()
		> AccelerationLimit * AccelerationLimit)
	{
		LimitedAcceleration = AccelDir * AccelerationLimit;
	}

	FVector Result = CurrentVelocity;
	const double Speed = Result.Size();
	const double EffectiveFriction = FMath::Max(1.0f, FluidFriction);
	Result -= (Result - AccelDir * Speed) * DeltaTime * EffectiveFriction;
	Result = Result * (1.0 - FluidFriction * DeltaTime)
		+ LimitedAcceleration * DeltaTime;
	// Swimming's bBuoyant branch follows the fluid multiply, before clamping
	// in UE1; flying supplies zero and shares all other math.
	Result += BuoyantAcceleration * DeltaTime;

	const double SpeedLimitSquared = SpeedLimit * SpeedLimit;
	if (Result.SizeSquared() > SpeedLimitSquared)
	{
		Result = Result.GetSafeNormal() * SpeedLimit;
	}
	return Result;
}


void UWoTCharacterMovement::UpdateCharacterStateBeforeMovement(float DeltaSeconds)
{
	// Zone movement-STATE effects on the predicted path (role-agnostic — this hook runs
	// identically on the authority and in the client's replayed saved moves):
	//   - ground friction: swap in the zone's ZoneGroundFriction while inside, restore the
	//     pawn's own outside;
	//   - swim mode: a water zone puts the pawn in MOVE_Swimming, leaving drops it to
	//     MOVE_Falling (mirrors the retail unconditional force, now decided on both sides).
	// Zone GRAVITY rides GetGravityZ() and zone CURRENT is move transport
	// (ApplyZoneEffectsPostPhys) — neither touches this hook.
	// The base value is captured lazily on the first update, before any zone can override it.
	// Non-movement zone effects (pain/heal) stay in AWoTZoneVolume::Tick.
	if (!bCapturedBase)
	{
		BaseGroundFriction = GroundFriction;
		bCapturedBase = true;
	}

	FWoTZoneMovement Eff;
	const bool bInZone = UpdatedComponent
		&& AWoTZoneVolume::GetMovementEffectAt(GetWorld(),
			UpdatedComponent->GetComponentLocation(), Eff);

	GroundFriction = bInZone ? Eff.GroundFriction : BaseGroundFriction;

	// Swim enter/exit. Stateless — derived purely from the current water flag and the current
	// mode (both part of the rewound/replayed prediction state), so a correction replay stays
	// consistent. MOVE_Swimming only ever comes from a water zone in this port, so any
	// out-of-water swimmer is one we should drop to falling.
	if (bInZone && Eff.bWater)
	{
		if (MovementMode != MOVE_Swimming)
		{
			SetMovementMode(MOVE_Swimming);
		}
	}
	else if (MovementMode == MOVE_Swimming)
	{
		SetMovementMode(MOVE_Falling);
	}

	Super::UpdateCharacterStateBeforeMovement(DeltaSeconds);
}

float UWoTCharacterMovement::GetGravityZ() const
{
	// A gravity zone REPLACES gravity for its occupants (retail integrates the zone's own
	// gravity in falling physics; the non-zone default is the same -950 the pawn's
	// GravityScale parity arc reproduces). Returning it here drives the engine's own
	// falling/swimming integration — the path whose Z survives PhysFalling (a Z
	// contribution made inside CalcVelocity is discarded there).
	FWoTZoneMovement Eff;
	if (UpdatedComponent
		&& AWoTZoneVolume::GetMovementEffectAt(GetWorld(),
			UpdatedComponent->GetComponentLocation(), Eff)
		&& Eff.bOverrideGravity)
	{
		return Eff.Gravity.Z;
	}
	return Super::GetGravityZ();
}

float UWoTCharacterMovement::ComputeAnalogInputModifier() const
{
	// The original's velocity update has no analog top-speed cap: a partial
	// stick lowers the acceleration, never the speed it reaches. Keyboard input
	// is always full scale, so only analog sticks see a difference. NPC pawns
	// keep the engine's rule, which their path following relies on.
	const AWoTCharacter* WoTCharacter = Cast<AWoTCharacter>(CharacterOwner);
	if (!WoTCharacter || WoTCharacter->IsNPCPawn())
	{
		return Super::ComputeAnalogInputModifier();
	}
	return Acceleration.SizeSquared() > 0.0f ? 1.0f : 0.0f;
}

#if WITH_AUTOMATION_TESTS
float UWoTCharacterMovement::ComputeAnalogInputModifierForTest(const FVector& InAcceleration)
{
	Acceleration = InAcceleration;
	return ComputeAnalogInputModifier();
}
#endif

float UWoTCharacterMovement::GetMaxSpeed() const
{
	const AWoTCharacter* WoTCharacter = Cast<AWoTCharacter>(CharacterOwner);
	if (WoTCharacter && !WoTCharacter->IsNPCPawn()
		&& WoTCharacter->IsPlayerMovementConstrained())
	{
		return 0.0f;
	}
	return Super::GetMaxSpeed();
}

void UWoTCharacterMovement::ClientHandleMoveResponse(
	const FCharacterMoveResponseDataContainer& MoveResponse)
{
	const bool bCorrection = MoveResponse.IsCorrection();
	const double PresentationPitch = CharacterOwner
		? CharacterOwner->GetActorRotation().Pitch : 0.0;
	Super::ClientHandleMoveResponse(MoveResponse);
	if (bCorrection)
	{
		// Character corrections own simulation location/yaw, but retail creature
		// reactions may pitch the pawn for presentation (BAT HeadToss). Preserve
		// that non-simulation axis across a velocity/position adjustment.
		if (CharacterOwner)
		{
			FRotator Rotation = CharacterOwner->GetActorRotation();
			Rotation.Pitch = PresentationPitch;
			CharacterOwner->SetActorRotation(Rotation);
		}
		++ClientCorrectionCountForTest;
		if (AWoTCharacter* WoTCharacter = Cast<AWoTCharacter>(CharacterOwner))
		{
			WoTCharacter->ReconcileEffectiveConstraintFromServerCorrection();
			WoTCharacter->ApplyCreaturePresentationPitch();
		}
	}
}

bool UWoTCharacterMovement::StepUp(const FVector& GravDir, const FVector& Delta,
	const FHitResult& Hit, FStepDownResult* OutStepDownResult)
{
	float HoverDistance = 0.0f;
	if (IsMovingOnGround() && CurrentFloor.IsWalkableFloor())
	{
		HoverDistance = FMath::Clamp(
			CurrentFloor.GetDistanceToFloor(), 0.0f, 2.4f);
	}

	// UE1 physWalking maintains a 1.9-2.4 uu hover and APawn::stepUp moves
	// MaxStepHeight upward from that location.
	// Role-free CurrentFloor state keeps authority, prediction and replay on the
	// same path. The guard leaves the retail MaxStepHeight visible everywhere else.
	TGuardValue<float> StepHeightGuard(
		MaxStepHeight, MaxStepHeight + HoverDistance);
	return Super::StepUp(GravDir, Delta, Hit, OutStepDownResult);
}

void UWoTCharacterMovement::PhysWalking(float DeltaTime, int32 Iterations)
{
	// CheckSurfaces is literally the first operation in retail physWalking.
	if (CheckWalkSurfaces(DeltaTime, Iterations))
	{
		return;
	}
	Super::PhysWalking(DeltaTime, Iterations);
	ApplyZoneEffectsPostPhys(DeltaTime, MOVE_Walking);
}

void UWoTCharacterMovement::PhysFalling(float DeltaTime, int32 Iterations)
{
	// UE1's late-added jump-onto-ladder branch can observe PHYS_Falling from
	// CheckSurfaces and call Landed before switching to walking
	// mode. UE5 dispatches falling to a separate Phys*, so
	// invoke the same check here to keep that shipped branch reachable.
	if (CheckWalkSurfaces(DeltaTime, Iterations))
	{
		return;
	}
	Super::PhysFalling(DeltaTime, Iterations);
	ApplyZoneEffectsPostPhys(DeltaTime, MOVE_Falling);
}

void UWoTCharacterMovement::PhysFlying(float DeltaTime, int32 Iterations)
{
	Super::PhysFlying(DeltaTime, Iterations);
	ApplyZoneEffectsPostPhys(DeltaTime, MOVE_Flying);
}

void UWoTCharacterMovement::PhysLadder(float DeltaTime, int32 Iterations)
{
	if (DeltaTime < MIN_TICK_TIME)
	{
		return;
	}

	// Engine ZoneInfo supplies 1.2 for the LevelInfo/default zone. A placed
	// zone replaces it with its authored value at the pawn's CURRENT location.
	FWoTZoneMovement ZoneEffect;
	if (UpdatedComponent)
	{
		AWoTZoneVolume::GetMovementEffectAt(GetWorld(),
			UpdatedComponent->GetComponentLocation(), ZoneEffect);
	}

	// Keep UE5's root-motion contract around the retail integrator, but do not
	// call Super::PhysFlying: it would apply 0.5*PhysicsVolume.FluidFriction a
	// second time (CharacterMovementComponent.cpp:4429-48).
	RestorePreAdditiveRootMotionVelocity();
	if (!HasAnimRootMotion() && !CurrentRootMotion.HasOverrideVelocity())
	{
		Velocity = RetailFlyingVelocity(Velocity, Acceleration,
			GetMaxAcceleration(), MaxFlySpeed, ZoneEffect.FluidFriction, DeltaTime);
	}
	ApplyRootMotionToVelocity(DeltaTime);

	// Collision half of UE5 PhysFlying (CharacterMovementComponent.cpp:4450-86),
	// retained without its CalcVelocity call: safe move, step-or-slide, then
	// derive the collision-limited final velocity from actual displacement.
	Iterations++;
	bJustTeleported = false;
	FVector OldLocation = UpdatedComponent->GetComponentLocation();
	const FVector Adjusted = Velocity * DeltaTime;
	FHitResult Hit(1.0f);
	SafeMoveUpdatedComponent(
		Adjusted, UpdatedComponent->GetComponentQuat(), true, Hit);
	if (Hit.Time < 1.0f)
	{
		const FVector VelDir = Velocity.GetSafeNormal();
		const float UpDown = VelDir | GetGravityDirection();
		bool bSteppedUp = false;
		if (FMath::Abs(GetGravitySpaceZ(Hit.ImpactNormal)) < 0.2f
			&& UpDown < 0.5f && UpDown > -0.2f && CanStepUp(Hit))
		{
			const FVector::FReal StepZ =
				GetGravitySpaceZ(UpdatedComponent->GetComponentLocation());
			bSteppedUp = StepUp(
				GetGravityDirection(), Adjusted * (1.0f - Hit.Time), Hit);
			if (bSteppedUp)
			{
				const FVector::FReal LocationZ = GetGravitySpaceZ(
					UpdatedComponent->GetComponentLocation())
					+ (GetGravitySpaceZ(OldLocation) - StepZ);
				SetGravitySpaceZ(OldLocation, LocationZ);
			}
		}
		if (!bSteppedUp)
		{
			HandleImpact(Hit, DeltaTime, Adjusted);
			SlideAlongSurface(
				Adjusted, 1.0f - Hit.Time, Hit.Normal, Hit, true);
		}
	}
	if (!bJustTeleported && !HasAnimRootMotion()
		&& !CurrentRootMotion.HasOverrideVelocity())
	{
		Velocity = (UpdatedComponent->GetComponentLocation() - OldLocation)
			/ DeltaTime;
	}
}

void UWoTCharacterMovement::ApplyZoneEffectsPostPhys(float DeltaTime,
	EMovementMode ForMode, bool bUseFlyingSemantics)
{
	// Retail zone-current semantics: ZoneVelocity is TRANSPORT — it displaces the move and
	// is never accumulated into Velocity (this port's earlier `Velocity += Current*dt`
	// accumulation was a divergence, and its Z was silently discarded while falling —
	// PhysFalling zeroes/restores Velocity.Z around CalcVelocity — which killed updrafts).
	// Falling/flying transport the full vector; walking/swimming transport
	// ZoneVelocity*25*dt (the retail frame-scaled form, kept as-is including its tick-rate
	// dependence); walking transport is horizontal-only. Player-class pawns are always
	// affected; other pawns only above a strength threshold (falling 200 uu/s, else
	// 300 uu/s). Runs identically in authority and prediction replay (position + zone in,
	// displacement out).
	// ForMode: a mid-physics mode change hands the remaining time to the new mode's Phys*
	// (whose own override applies its transport), so only the CURRENT mode's wrapper acts.
	if (MovementMode != ForMode || DeltaTime < KINDA_SMALL_NUMBER || !UpdatedComponent
		|| !CharacterOwner)
	{
		return;
	}
	FWoTZoneMovement Eff;
	if (!AWoTZoneVolume::GetMovementEffectAt(GetWorld(),
			UpdatedComponent->GetComponentLocation(), Eff))
	{
		return;
	}

	// The lateral part of an arbitrary-direction zone gravity integrates while falling
	// (its Z rides GetGravityZ through the engine's falling integrator).
	if (ForMode == MOVE_Falling && Eff.bOverrideGravity
		&& (Eff.Gravity.X != 0.0f || Eff.Gravity.Y != 0.0f))
	{
		Velocity += FVector(Eff.Gravity.X, Eff.Gravity.Y, 0.0f) * DeltaTime;
	}

	if (Eff.Current.IsNearlyZero())
	{
		return;
	}
	const bool bPlayerClass = !CharacterOwner->IsA<AWoTNPCCharacter>();
	const float GateSq = (ForMode == MOVE_Falling) ? 40000.0f : 90000.0f;
	if (!bPlayerClass && Eff.Current.SizeSquared() <= GateSq)
	{
		return;
	}
	FVector TransportVel = Eff.Current;
	if (!bUseFlyingSemantics
		&& (ForMode == MOVE_Walking || ForMode == MOVE_Swimming))
	{
		TransportVel *= 25.0f * DeltaTime;
	}
	if (!bUseFlyingSemantics && ForMode == MOVE_Walking)
	{
		TransportVel.Z = 0.0f;
	}
	const FVector Delta = TransportVel * DeltaTime;
	FHitResult Hit(1.0f);
	SafeMoveUpdatedComponent(Delta, UpdatedComponent->GetComponentQuat(), true, Hit);
	if (Hit.IsValidBlockingHit())
	{
		SlideAlongSurface(Delta, 1.0f - Hit.Time, Hit.Normal, Hit, true);
	}
}

UWoTMapLightTable* UWoTCharacterMovement::GetWalkSurfaceTable()
{
	if (!IsValid(WalkSurfaceTable))
	{
		WalkSurfaceTable = UWoTMeshLight::FindMapLightTable(GetWorld());
	}
	return WalkSurfaceTable;
}

bool UWoTCharacterMovement::CheckWalkSurfaces(float DeltaTime, int32 Iterations)
{
	UWoTMapLightTable* Table = GetWalkSurfaceTable();
	AWoTCharacter* WoTCharacter = Cast<AWoTCharacter>(CharacterOwner);
	if (!IsValid(Table) || !Table->bCheckWalkSurfaces
		|| !UpdatedComponent || !WoTCharacter)
	{
		return false;
	}

	FWoTWalkSurfaceHit Hit;
	bool bClimbable = false;
	// CheckClimbSurface rejects fast falls before issuing any trace.
	if (Velocity.Z >= FallingTooFast)
	{
		const float CollisionHeight = CharacterOwner->GetSimpleCollisionHalfHeight();
		const float CollisionRadius = CharacterOwner->GetSimpleCollisionRadius();
		const FVector Start = UpdatedComponent->GetComponentLocation()
			- FVector(0.0f, 0.0f, 0.7f * CollisionHeight);
		const float Yaw = UpdatedComponent->GetComponentRotation().Yaw;
		// Forward, left, right, behind: retail mutates yaw by 0, -16384,
		// +32768, +16384 UE1 units.
		const float YawOffsets[] = {0.0f, -90.0f, 90.0f, 180.0f};
		for (const float YawOffset : YawOffsets)
		{
			const FVector End = Start
				+ 2.0f * CollisionRadius
				* FRotator(0.0f, Yaw + YawOffset, 0.0f).Vector();
			FWoTWalkSurfaceHit Candidate;
			if (TraceWalkSurface(Table, GetWorld(), CharacterOwner,
				Start, End, Candidate)
				&& Candidate.Friction == ClimbableFriction)
			{
				Hit = Candidate;
				bClimbable = true;
				break;
			}
		}
	}

	if (bClimbable)
	{
		if (MovementMode == MOVE_Falling)
		{
			FHitResult LadderHit;
			LadderHit.Location = Hit.Location;
			LadderHit.ImpactPoint = Hit.Location;
			LadderHit.Normal = Hit.Normal;
			LadderHit.ImpactNormal = Hit.Normal;
			LadderHit.Item = Hit.NodeIndex;
			CharacterOwner->Landed(LadderHit);
			SetMovementMode(MOVE_Walking);
		}
		WoTCharacter->NotifyWalkSurface(
			Hit.NodeIndex, Hit.Friction, Hit.Location, Hit.Normal);
		if (!Acceleration.IsZero())
		{
			Velocity = ClimbVelocity(Velocity,
				CharacterOwner->GetViewRotation(), MaxWalkSpeed);
		}
		// Retail invokes physFlying for this tick without changing the persistent
		// walking mode. PhysLadder ports that velocity
		// integration and reuses only UE5's collision half, avoiding UE5's own
		// PhysicsVolume fluid damping.
		const EMovementMode PersistentMode = MovementMode;
		PhysLadder(DeltaTime, Iterations);
		ApplyZoneEffectsPostPhys(DeltaTime, PersistentMode, true);
		return true;
	}

	// CheckWalkSurface traces straight down by CollisionHeight + 2*Radius
	// and fires eventWalkTexture even for a null texture.
	const FVector WalkStart = UpdatedComponent->GetComponentLocation();
	const FVector WalkEnd = WalkStart - FVector(0.0f, 0.0f,
		CharacterOwner->GetSimpleCollisionHalfHeight()
		+ 2.0f * CharacterOwner->GetSimpleCollisionRadius());
	const bool bWalkHit = TraceWalkSurface(
		Table, GetWorld(), CharacterOwner, WalkStart, WalkEnd, Hit);
	WoTCharacter->NotifyWalkSurface(
		bWalkHit ? Hit.NodeIndex : INDEX_NONE,
		bWalkHit ? Hit.Friction : 1.0f,
		bWalkHit ? Hit.Location : WalkEnd,
		bWalkHit ? Hit.Normal : FVector::UpVector);
	if (MovementMode == MOVE_Walking && bWalkHit && Hit.Friction < 1.0f)
	{
		FVector ZoneGravity(0.0f, 0.0f, GetGravityZ());
		FWoTZoneMovement Effect;
		if (AWoTZoneVolume::GetMovementEffectAt(
			GetWorld(), UpdatedComponent->GetComponentLocation(), Effect)
			&& Effect.bOverrideGravity)
		{
			ZoneGravity = Effect.Gravity;
		}
		// The original engine's slide step, including its late-added 0.02 clamp.
		const FVector Slide = DeltaTime * ZoneGravity
			/ (0.5f * FMath::Max(0.02f, 4.0f * Hit.Friction)) * DeltaTime;
		const FVector Delta = Slide - Hit.Normal
			* FVector::DotProduct(Slide, Hit.Normal);
		if (FVector::DotProduct(Delta, Slide) >= 0.0f)
		{
			FHitResult MoveHit(1.0f);
			SafeMoveUpdatedComponent(
				Delta, UpdatedComponent->GetComponentQuat(), true, MoveHit);
		}
	}
	return false;
}

bool UWoTCharacterMovement::InWaterZone() const
{
	FWoTZoneMovement Eff;
	return UpdatedComponent
		&& AWoTZoneVolume::GetMovementEffectAt(GetWorld(),
			UpdatedComponent->GetComponentLocation(), Eff)
		&& Eff.bWater;
}

bool UWoTCharacterMovement::IsInWater() const
{
	// A WoT water zone counts as water even though there is no real APhysicsVolume, so every
	// engine water consumer (and our PhysSwimming guard) agrees with the zone.
	return Super::IsInWater() || InWaterZone();
}

bool UWoTCharacterMovement::CheckRetailWaterJump() const
{
	// Engine Pawn.CheckWaterJump. (No CarriedDecoration in this port: that check is moot.)
	// A wall within one radius ahead along the pawn's yaw, swept with the pawn's
	// cylinder extent, and a clear line 1.1*MaxStepHeight up across two radii.
	const float Radius = CharacterOwner->GetSimpleCollisionRadius();
	const float Height = CharacterOwner->GetSimpleCollisionHalfHeight();
	FVector CheckNorm = UpdatedComponent->GetComponentRotation().Vector();
	CheckNorm.Z = 0.0;
	CheckNorm = CheckNorm.GetSafeNormal();
	if (CheckNorm.IsZero())
	{
		return false;
	}
	const FVector Location = UpdatedComponent->GetComponentLocation();
	FCollisionQueryParams Params(SCENE_QUERY_STAT(WoTRetailWaterJump), false,
		CharacterOwner);
	FCollisionResponseParams Response;
	InitCollisionParams(Params, Response);
	const ECollisionChannel Channel = UpdatedComponent->GetCollisionObjectType();
	FHitResult Wall;
	if (!GetWorld()->SweepSingleByChannel(Wall, Location,
			Location + Radius * CheckNorm, FQuat::Identity, Channel,
			FCollisionShape::MakeBox(FVector(Radius, Radius, Height)), Params, Response)
		|| Cast<APawn>(Wall.GetActor()) != nullptr)
	{
		return false;
	}
	const FVector LipStart = Location + FVector(0.0, 0.0, 1.1 * MaxStepHeight);
	FHitResult Lip;
	return !GetWorld()->LineTraceSingleByChannel(Lip, LipStart,
		LipStart + 2.0f * Radius * CheckNorm, Channel, Params, Response);
}

void UWoTCharacterMovement::TryRetailWaterJump()
{
	// The engine PlayerPawn's PlayerSwimming.ZoneChange that WOTPlayer's
	// PlayerSwimming inherits: leaving the water while bUpAndOut, against a wall
	// with a clear lip, sets Velocity.Z = 330 + 2*CollisionRadius "so physics uses
	// this for remainder of tick". Retail raises it inside the swim MoveActor, so
	// physSwimming's changed-Z exception keeps it.
	// PlayDuck is presentation; the PlayerWalking state change has no port state.
	const AWoTCharacter* WoTCharacter = Cast<AWoTCharacter>(CharacterOwner);
	if (!WoTCharacter || WoTCharacter->IsNPCPawn())
	{
		return;
	}
	// WOTPlayer.uc:3196: bUpAndOut from the view axes and this move's acceleration.
	// UE1 pitch is 0..65535 with looking down above 49152, so "> 2048" holds for
	// every pitch except 0..11.25 degrees up; ClampAxis keeps that same domain.
	const FRotator View = CharacterOwner->GetControlRotation();
	const double PitchUnits = FRotator::ClampAxis(View.Pitch) * (65536.0 / 360.0);
	const bool bUpAndOut = FVector::DotProduct(View.Vector(), Acceleration) > 0.0
		&& (Acceleration.Z > 0.0 || PitchUnits > 2048.0);
	if (bUpAndOut && CheckRetailWaterJump())
	{
		Velocity.Z = 330.0f + 2.0f * CharacterOwner->GetSimpleCollisionRadius();
	}
}

float UWoTCharacterMovement::SwimInZone(const FVector& Delta, FHitResult& Hit)
{
	const FVector Start = UpdatedComponent->GetComponentLocation();
	const bool bStartedInWater = AWoTZoneVolume::IsPointInWater(GetWorld(), Start);
	SafeMoveUpdatedComponent(Delta, UpdatedComponent->GetComponentQuat(), true, Hit);
	if (InWaterZone() || Delta.IsNearlyZero())
	{
		return 0.0f;
	}
	if (bStartedInWater)
	{
		// The move changed the pawn's zone: retail's ZoneChange runs here, inside
		// MoveActor and before findWaterLine.
		TryRetailWaterJump();
	}

	// UE1's APawn::Swim/findWaterLine. Keep the dry end
	// of the bracket and stop within one unit, exactly as retail's zone probe.
	const FVector MovedLocation = UpdatedComponent->GetComponentLocation();
	FVector Wet = Start;
	FVector Dry = MovedLocation;
	while (FVector::DistSquared(Wet, Dry) >= 1.0)
	{
		const FVector Midpoint = (Wet + Dry) * 0.5;
		if (AWoTZoneVolume::IsPointInWater(GetWorld(), Midpoint))
		{
			Wet = Midpoint;
		}
		else
		{
			Dry = Midpoint;
		}
	}
	const float AirFraction = FVector::Distance(Dry, MovedLocation) / Delta.Size();
	if (Dry != MovedLocation)
	{
		SafeMoveUpdatedComponent(Dry - MovedLocation,
			UpdatedComponent->GetComponentQuat(), true, Hit);
	}
	return AirFraction;
}

void UWoTCharacterMovement::PhysSwimming(float DeltaTime, int32 Iterations)
{
	// Swimming explicitly participates in Legend CheckSurfaces, before even
	// the head-zone damping.
	if (CheckWalkSurfaces(DeltaTime, Iterations))
	{
		return;
	}
	if (DeltaTime < MIN_TICK_TIME || !HasValidData())
	{
		return;
	}

	RestorePreAdditiveRootMotionVelocity();
	FVector OldLocation = UpdatedComponent->GetComponentLocation();
	// UE1's head-zone probe samples Location + EyeHeight. Use the steady swim
	// target from WOTPlayer.PlaySwimming (WOTPlayer.uc:2503), not the visual
	// tween's actor-clock-dependent BaseEyeHeight: movement replay does not
	// replay animation ticks. NPC eye height is an authored class tunable.
	// The omitted presentation tween/smoothing is documented in PORT_NOTES.
	const AWoTCharacter* WoTCharacter = Cast<AWoTCharacter>(CharacterOwner);
	const float HeadHeight = WoTCharacter && !WoTCharacter->IsNPCPawn()
		? 0.5f * WoTCharacter->GetRetailStandingBaseEyeHeight()
		: CharacterOwner->BaseEyeHeight;
	const FVector HeadLocation = OldLocation
		+ FVector(0.0f, 0.0f, HeadHeight);
	if (!AWoTZoneVolume::IsPointInWater(GetWorld(), HeadLocation) && Velocity.Z > 100.0)
	{
		Velocity.Z *= 1.0f - DeltaTime;
	}

	++Iterations;
	bJustTeleported = false;
	FWoTZoneMovement ZoneEffect;
	AWoTZoneVolume::GetMovementEffectAt(GetWorld(), OldLocation, ZoneEffect);
	// Same position query/default as GetGravityZ; retain the entire zone vector
	// for retail's buoyant gravity.
	const FVector ZoneGravity = ZoneEffect.bOverrideGravity
		? ZoneEffect.Gravity : FVector(0.0f, 0.0f, GetGravityZ());
	if (!HasAnimRootMotion() && !CurrentRootMotion.HasOverrideVelocity())
	{
		// WOTPlayer.HandleWalking clears bIsWalking in water (WOTPlayer.uc:3124-3131),
		// so calcVelocity's player-only 0.3 cap is inapplicable.
		// Retail physSwimming's call: WaterSpeed, ZoneFluidFriction, fluid/no brake/buoyant.
		const FVector BuoyantAcceleration = ZoneGravity * (1.0f - Buoyancy / Mass);
		Velocity = RetailFlyingVelocity(Velocity, Acceleration, GetMaxAcceleration(),
			GetMaxSpeed(), ZoneEffect.FluidFriction, DeltaTime, BuoyantAcceleration);
	}
	ApplyRootMotionToVelocity(DeltaTime);
	const double IntegratedZ = Velocity.Z;

	// Retail folds ZoneVel into Adjusted. The existing port seam
	// transports it once in ApplyZoneEffectsPostPhys below (PORT_NOTES).
	const FVector Adjusted = Velocity * DeltaTime;
	FHitResult Hit(1.0f);
	float RemainingTime = DeltaTime * SwimInZone(Adjusted, Hit);
	if (Hit.Time < 1.0f && HasValidData())
	{
		// Retail physSwimming: vertical gravity sign and the exact step gate.
		const FVector GravDir(0.0f, 0.0f, ZoneGravity.Z > 0.0 ? 1.0f : -1.0f);
		const double UpDown = FVector::DotProduct(GravDir, Velocity.GetSafeNormal());
		bool bSteppedUp = false;
		if (FMath::Abs(Hit.Normal.Z) < 0.2 && UpDown > -0.2 && UpDown < 0.5
			&& CanStepUp(Hit))
		{
			const double StepZ = UpdatedComponent->GetComponentLocation().Z;
			bSteppedUp = StepUp(GravDir, Adjusted * (1.0f - Hit.Time), Hit);
			if (bSteppedUp)
			{
				OldLocation.Z += UpdatedComponent->GetComponentLocation().Z - StepZ;
			}
		}
		if (!bSteppedUp)
		{
			// The engine SlideAlongSurface collision operations, but each sweep
			// must use SwimInZone: base SlideAlongSurface cannot return waterline
			// air time. Retail order: processHitWall/slide/TwoWallAdjust.
			HandleImpact(Hit, DeltaTime, Adjusted);
			const FVector OldHitNormal = Hit.Normal;
			const float SlideTime = 1.0f - Hit.Time;
			FVector SlideDelta = ComputeSlideVector(Adjusted, SlideTime, Hit.Normal, Hit);
			if (FVector::DotProduct(SlideDelta, Adjusted) >= 0.0 && HasValidData())
			{
				RemainingTime *= SlideTime * SwimInZone(SlideDelta, Hit);
				if (Hit.Time < 1.0f && HasValidData())
				{
					HandleImpact(Hit, DeltaTime * SlideTime, SlideDelta);
					const float SecondSlideTime = 1.0f - Hit.Time;
					TwoWallAdjust(SlideDelta, Hit, OldHitNormal);
					RemainingTime *= SecondSlideTime * SwimInZone(SlideDelta, Hit);
				}
			}
		}
	}
	if (!HasValidData())
	{
		return;
	}

	if (!bJustTeleported && !HasAnimRootMotion()
		&& !CurrentRootMotion.HasOverrideVelocity() && RemainingTime < DeltaTime)
	{
		// Retail physSwimming preserves Z ONLY when changed during the move.
		// WOTPlayer.uc:3198-3202 supplies the script water jump in retail; this
		// port has no CheckWaterJump handler. Movement/impact/overlap callbacks
		// can still change velocity, so compare Z rather than PhysicsVolume.
		const bool bWaterJump = Velocity.Z != IntegratedZ;
		const double JumpZ = Velocity.Z;
		Velocity = (UpdatedComponent->GetComponentLocation() - OldLocation)
			/ (DeltaTime - RemainingTime);
		if (bWaterJump)
		{
			Velocity.Z = JumpZ;
		}
	}

	if (!InWaterZone())
	{
		// Retail physSwimming: retain callback-selected modes and bob only
		// for a gentle upward exit; a genuine water-jump velocity is untouched.
		if (IsSwimming())
		{
			SetMovementMode(MOVE_Falling);
		}
		if (Velocity.Z > 0.0 && Velocity.Z < 160.0)
		{
			Velocity.Z = 40.0 + Velocity.Size2D() * 0.4;
		}
	}
	// Retail only dispatches remaining air time above 0.01s, to fall or fly
	// (physSwimming); preserve that gate with UE5's mode dispatcher.
	if (RemainingTime > 0.01f && (IsFalling() || IsFlying()))
	{
		StartNewPhysics(RemainingTime, Iterations);
	}

	ApplyZoneEffectsPostPhys(DeltaTime, MOVE_Swimming);
}

// ---------------------------------------------------------------------------
// D3 fix: never send a base-relative correction the client cannot resolve.
// ---------------------------------------------------------------------------
bool UWoTCharacterMovement::IsNetResolvableMovementBase(const UObject* Base)
{
	if (Base == nullptr)
	{
		return true;
	}
	const UActorComponent* Component = Cast<UActorComponent>(Base);
	if (Component == nullptr)
	{
		return Base->IsSupportedForNetworking();
	}
	if (Component->GetIsReplicated())
	{
		return true;
	}
	const AActor* Owner = Component->GetOwner();
	return Owner != nullptr && Component->IsNameStableForNetworking()
		&& (Owner->GetIsReplicated() || Owner->IsNameStableForNetworking());
}

bool UWoTCharacterMovement::ShouldReadClientLocationThroughServerBase(
	EMovementMode ClientMode, bool bClientBaseValid, const UObject* ServerBase,
	bool bServerBaseDynamic)
{
	return ClientMode == MOVE_Walking && !bClientBaseValid && ServerBase != nullptr
		&& bServerBaseDynamic && !IsNetResolvableMovementBase(ServerBase);
}

bool UWoTCharacterMovement::AbsolutizeUnresolvableAdjustment(
	FClientAdjustment& Adjustment, const UObject* Base,
	const FVector& WorldLocationZeroOrigin, const FVector& WorldVelocity)
{
	if (Adjustment.bAckGoodMove || !Adjustment.bBaseRelativePosition
		|| IsNetResolvableMovementBase(Base))
	{
		return false;
	}
	// ClientAdjustPosition_Implementation applies an absolute location even when
	// it cannot resolve the (serialized-null) base: "could not resolve the new
	// absolute movement base actor, but WILL use the position!".
	Adjustment.NewLoc = WorldLocationZeroOrigin;
	Adjustment.NewVel = WorldVelocity;
	Adjustment.bBaseRelativePosition = false;
	Adjustment.bBaseRelativeVelocity = false;
	return true;
}

void UWoTCharacterMovement::ServerMoveHandleClientError(float ClientTimeStamp,
	float DeltaTime, const FVector& Accel, const FVector& RelativeClientLocation,
	FMovementBaseInterfaceData* ClientMovementBaseInterfaceData,
	FName ClientBaseBoneName, uint8 ClientMovementMode)
{
	// Client -> server: a client standing on its own copy of the locally
	// projected geometry sends a location relative to it, but the base arrives
	// null (it has no NetGUID), so UE reads that location as absolute. Both
	// machines project the same geometry, so express it through the server's
	// copy (a no-op for the identity-placed world BSP).
	FVector ClientLocation = RelativeClientLocation;
	const FMovementBaseInterfaceData* ServerBaseData = GetMovementBaseInterfaceData();
	FVector ClientWorldLocation;
	// Only a WALKING client stands on a base (UE substitutes the server's base
	// for a null client base in exactly that case, ServerMoveHandleClientError);
	// an airborne client sends an absolute location with no base.
	TEnumAsByte<EMovementMode> ClientMode(MOVE_None);
	TEnumAsByte<EMovementMode> ClientGroundMode(MOVE_None);
	uint8 ClientCustomMode = 0;
	UnpackNetworkMovementMode(ClientMovementMode, ClientMode, ClientCustomMode,
		ClientGroundMode);
	if (CharacterOwner != nullptr && ServerBaseData != nullptr
		&& ShouldReadClientLocationThroughServerBase(ClientMode.GetValue(),
			MovementBaseUtility::IsMovementBaseDataValid(ClientMovementBaseInterfaceData),
			ServerBaseData->GetMovementBaseObject(),
			MovementBaseUtility::UseRelativeLocation(ServerBaseData))
		&& MovementBaseUtility::TransformLocationToWorld(ServerBaseData,
			CharacterOwner->GetBasedMovement().BoneName, RelativeClientLocation,
			ClientWorldLocation))
	{
		ClientLocation = FRepMovement::RebaseOntoZeroOrigin(ClientWorldLocation, this);
	}
	Super::ServerMoveHandleClientError(ClientTimeStamp, DeltaTime, Accel,
		ClientLocation, ClientMovementBaseInterfaceData, ClientBaseBoneName,
		ClientMovementMode);

	// Server -> client: the correction Super just staged for this move.
	FNetworkPredictionData_Server_Character* ServerData =
		GetPredictionData_Server_Character();
	if (ServerData != nullptr && UpdatedComponent != nullptr
		&& ServerData->PendingAdjustment.TimeStamp == ClientTimeStamp)
	{
		AbsolutizeUnresolvableAdjustment(ServerData->PendingAdjustment,
			ServerData->PendingAdjustment.NewMovementBasePhysicsObjectOwner.Get(),
			FRepMovement::RebaseOntoZeroOrigin(
				UpdatedComponent->GetComponentLocation(), this),
			Velocity);
	}
}
