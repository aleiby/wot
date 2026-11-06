#include "WoTNPCController.h"
#include "../WoTRandom.h"

#include "WoTNPCCharacter.h"
#include "WoTSight.h"
#include "WoTPathNodeIterator.h"
#include "WoTPathNode.h"
#include "WoTCasterComponent.h"
#include "WoTWeaponComponent.h"
#include "WoTNPCWeapon.h"
#include "WoTCreatureBehaviors.h"
#include "WoTCreatureData.h"
#include "../Traps/WoTAlarm.h"
#include "WoTAIUtil.h"
#include "../WoTAnimTable.h"
#include "../WoTVertexAnimComponent.h"
#include "../Inventory/WoTAngrealInventory.h"
#include "../Inventory/WoTIceLeech.h"
#include "../Projectiles/WoTProjectile.h"   // FWoTDamageEvent
#include "../WoTSound.h"
#include "../WoTPlayerController.h"
#include "../WoTWeaveFX.h"
#include "../WoTParticleSprayer.h"
#include "../WoTActorRotator.h"
#include "../WoTPayloadSubsystem.h"
#include "../World/WoTRuntimeClassDefaults.h"
#include "../World/WoTRuntimeBSPRenderer.h"
#include "../RetailData/WoTDevRetailFamilies.h"
#include "Engine/World.h"
#include "Engine/StaticMeshActor.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "Components/PrimitiveComponent.h"
#include "NavigationSystem.h"
#include "NavigationData.h"
#include "Navigation/PathFollowingComponent.h"

namespace
{
	// Player-sighting sample cadence: the engine's per-observer SightCounter
	// cycle.
	constexpr float SightSamplePeriod = 0.2f;
	// Direct-move arrival tolerances: XY 16uu,
	// Z within max(48, CollisionHeight).
	constexpr float ArrivalRadiusXYSquared = 256.0f;
	// NPC move speed fraction (players use MaxDesiredSpeed = 1.0, NPCs 0.8).
	constexpr float NPCSpeedFraction = 0.8f;

	bool HasValidReturnNavigationStart(const AWoTNPCCharacter& Self)
	{
		const UCharacterMovementComponent* Movement =
			Self.GetCharacterMovement();
		const bool bStableSupportedLocomotion = Movement
			&& (Movement->IsMovingOnGround() || Movement->IsSwimming());
		if (!bStableSupportedLocomotion || Movement->bJustTeleported
			|| !Movement->PendingLaunchVelocity.IsNearlyZero())
		{
			return false;
		}
		UNavigationSystemV1* Nav =
			FNavigationSystem::GetCurrent<UNavigationSystemV1>(Self.GetWorld());
		const ANavigationData* NavData = Nav
			? Nav->GetNavDataForProps(Self.GetNavAgentPropertiesRef(),
				Self.GetActorLocation())
			: nullptr;
		if (!NavData)
		{
			return false;
		}
		FNavLocation Projected;
		return Nav->ProjectPointToNavigation(Self.GetActorLocation(), Projected,
			NavData->GetConfig().DefaultQueryExtent, NavData);
	}

	struct FWoTNoiseSlot
	{
		float Time = 0.0f;
		FVector Spot = FVector::ZeroVector;
		float Loudness = 0.0f;
	};
	struct FWoTInstigatorNoise
	{
		FWoTNoiseSlot Slots[2];
	};
	// Native stores these on APawn. Weak keys support any instigating pawn and
	// cannot carry a destroyed pawn's history into another world/spawn.
	TMap<TWeakObjectPtr<APawn>, FWoTInstigatorNoise> InstigatorNoises;

	bool IsNoisePlayer(const APawn* Pawn)
	{
		const AWoTCharacter* Character = Cast<AWoTCharacter>(Pawn);
		return Pawn && (Character ? !Character->IsNPCPawn() : Pawn->IsPlayerControlled());
	}

	bool RelatedNoiseClasses(const AWoTNPCCharacter* Observer, const AActor* Maker,
		const FWoTRuntimeClassDefaults* Defaults)
	{
		const AWoTNPCCharacter* MakerNPC = Cast<AWoTNPCCharacter>(Maker);
		if (MakerNPC && !MakerNPC->CreatureRowName.IsNone()
			&& !Observer->CreatureRowName.IsNone())
		{
			if (MakerNPC->CreatureRowName == Observer->CreatureRowName) return true;
			return Defaults
				&& Defaults->ClassesAreRelatedByLeaf(
					MakerNPC->CreatureRowName, Observer->CreatureRowName);
		}
		return Observer->IsA(Maker->GetClass()) || Maker->IsA(Observer->GetClass());
	}
}

AWoTNPCController::AWoTNPCController()
{
	PrimaryActorTick.bCanEverTick = true;
}

float AWoTNPCController::GetIntendedMovementSpeedForAnimation() const
{
	if (!Brain) return 0.0f;
	if (State == EWoTBrainState::Returning)
	{
		return Brain->GetGoal(EWoTGoalIndex::Waiting).SuggestedSpeed;
	}
	if (bMovementCycleActive && MovementManager.SuggestedSpeed > 0.0f)
	{
		return MovementManager.SuggestedSpeed;
	}
	if (Brain->CurrentGoalIndex != EWoTGoalIndex::None)
	{
		return Brain->GetGoal(Brain->CurrentGoalIndex).SuggestedSpeed;
	}
	return 0.0f;
}

bool AWoTNPCController::HasActiveMovementIntentForAnimation() const
{
	return bDirectMoveActive || bPathMoveActive
		|| (bMovementCycleActive && bMovementCycleMoving);
}

void AWoTNPCController::OnPossess(APawn* InPawn)
{
	Super::OnPossess(InPawn);

	Brain = InPawn ? InPawn->FindComponentByClass<UWoTBrainComponent>() : nullptr;
	if (!Brain)
	{
		return;
	}
	Brain->OnNotifierPulse.AddUObject(this, &AWoTNPCController::OnNotifierPulse);

	// Materialize the row's melee/ranged inventory before behavior hooks and band
	// construction inspect capabilities. Authority owns the usage model; the weapon
	// actors and their attachments replicate for client presentation.
	if (AWoTNPCCharacter* NPC = Cast<AWoTNPCCharacter>(InPawn); NPC && HasAuthority())
	{
		if (const FWoTCreatureRow* Row = WoTCreatureData::Find(NPC->CreatureRowName))
		{
			NPC->InitializeWeaponSlots(Row->DefaultWeaponType, Row->MeleeWeaponType,
				Row->RangedWeaponType);
		}
	}

	// The creature behavior hook, created on the pawn by ConfigureNPC. The
	// controller drives it each frame (see Tick) after its own state update.
	Behavior = InPawn->FindComponentByClass<UWoTCreatureBehavior>();
	if (Behavior)
	{
		Behavior->OnPossessedBy(this);
	}

	// The goal-pursuit ladder for this creature (RangeHandlerFactory, folded to
	// data): caster pawns hold + cast, melee pawns close + swing. The intermediate
	// ladder is the shared alarm/help/seal navigation set.
	TrackingBands = BuildTrackingBands();
	MovementManager = FWoTMovementManager();
	bMovementCycleActive = false;
	if (const AWoTNPCCharacter* NPC = Cast<AWoTNPCCharacter>(InPawn))
	{
		if (const auto* Row = WoTCreatureData::Find(NPC->CreatureRowName))
		{
			// Slot20 is disabled in the shipped Grunt factories. Contact remains
			// in reachable21 and must continue running its pattern.
			TrackingBands.Bands[static_cast<int32>(EWoTGoalProximity::Reached)].bActive = false;
			for (const auto& Binding : Row->MovementBindings)
			{
				const EWoTGoalProximity Proximity = Binding.bComponent
					? EWoTGoalProximity::Visible
					: Binding.HandlerIndex == 21
						? EWoTGoalProximity::Reachable
						: Binding.HandlerIndex == 24
							? EWoTGoalProximity::UnNavigable
							: EWoTGoalProximity::Pathable;
				TrackingBands.Bands[static_cast<int32>(Proximity)].MovementBinding = Binding;
			}
			// The shipped acquired selector-only handlers carry no movement
			// pattern. They were absent from the cached row because the cache stores
			// pattern bindings only; restore their immutable retail templates here.
			auto EnsureSelectorBinding = [this](EWoTGoalProximity Proximity,
				int32 Handler, const TCHAR* Waypoint, const TCHAR* Focus)
			{
				FWoTMovementBinding& Binding = TrackingBands.Bands[
					static_cast<int32>(Proximity)].MovementBinding;
				if (Binding.WaypointSelector.IsNone()
					&& Binding.FocusSelector.IsNone())
				{
					Binding.HandlerIndex = Handler;
					Binding.WaypointSelector = FName(Waypoint);
					Binding.FocusSelector = FName(Focus);
				}
			};
			EnsureSelectorBinding(EWoTGoalProximity::Reachable, 21,
				TEXT("Legend.WaypointSelector"),
				TEXT("Legend.FocusSelectorOnGoal"));
			EnsureSelectorBinding(EWoTGoalProximity::Pathable, 23,
				TEXT("Legend.WaypointSelector"),
				TEXT("Legend.FocusSelectorOnWaypoint"));
			EnsureSelectorBinding(EWoTGoalProximity::UnNavigable, 24,
				TEXT("Legend.WaypointSelectorApproaching"),
				TEXT("Legend.FocusSelectorOnWaypoint"));
		}
	}
	IntermediateBands = FWoTRangeBandSet::IntermediateNavDefault();

	// SetInitialState orders block (Captain.uc:83): issue the placed orders and
	// ping the leadership range BEFORE the base waiting/StartUp init (the .uc runs
	// ChangeOrders + InvokeWotPawnFindLeader ahead of Super.SetInitialState).
	InitCaptainOrders();

	// SetInitialState: an idle pawn waits at its spawn spot with its spawn
	// facing (LegendPawn.uc:2504 → SetWaitingGoal).
	Brain->SetWaitingGoal(InPawn->GetActorLocation(), InPawn->GetActorRotation());

	// Randomize the sight-sampler phase like the notifier pulses, so a crowd's
	// samples spread across frames.
	SightSampleTimer = WoTRandom::FRand() * SightSamplePeriod;

	// Seed the activity-LOD poll phase (staggers a crowd's evaluations; the shipped
	// manager randomizes its first fire).
	ActivityEvalTimer = WoTRandom::FRand() * ActivityEvalPeriod;

	// StartUp (auto state, LegendPawn.uc:1605): evaluate priorities once and
	// act; fall back to the configured initial state if nothing accepts.
	State = EWoTBrainState::StartUp;
	// The rest states (Inactive/Dormant) are entered DIRECTLY and stick — the shipped
	// SetInitialState GotoStates them without a priority pass. They leave only via the
	// activity-LOD wake poll (Inactive) or an explicit UnForceDormant (Dormant); a
	// spawn-time TransitionOnGoalPriority would otherwise always pick Waiting (its goal
	// carries a standing priority 50), so the shipped Inactive/Dormant start never took.
	if (InitialState == EWoTBrainState::Inactive || InitialState == EWoTBrainState::Dormant)
	{
		EnterState(InitialState);
	}
	else if (!TransitionOnGoalPriority(true))
	{
		EnterState(InitialState);
	}

	// An accepted Ice component outlives possession. Adopt its suspension only
	// after this controller has materialized its brain/state shell.
	if (UWoTIceLeech* Ice = InPawn->FindComponentByClass<UWoTIceLeech>())
	{
		Ice->RefreshNPCControllerSuspension();
	}
}

void AWoTNPCController::OnUnPossess()
{
	// End any in-flight teleport episode: a pawn unpossessed mid-walk-to-beacon or
	// mid-vanish must not leave the latch set, and a mid-vanish pawn must get its
	// collision/visibility restored (EndState(Teleporting), F3).
	bTeleportBeaconActive = false;
	if (State == EWoTBrainState::Teleporting)
	{
		EndState(EWoTBrainState::Teleporting);
	}
	if (Behavior)
	{
		Behavior->OnUnpossessed();
		Behavior = nullptr;
	}
	if (Brain)
	{
		Brain->OnNotifierPulse.RemoveAll(this);
		Brain = nullptr;
	}
	Super::OnUnPossess();
}

void AWoTNPCController::Tick(float DeltaSeconds)
{
	// Notifier pulses resolve at the top of the frame, before any state work.
	if (Brain)
	{
		Brain->ReflectTick(DeltaSeconds);
	}
	Super::Tick(DeltaSeconds);

	// A dead pawn diverts to Dying no matter what it was doing — the original
	// gates TransitionOnGoalPriority on Health (LegendPawn.uc:490) and Died
	// disables all notifiers (:737); the Dying state no-ops every perception
	// callback (:2530).
	if (State != EWoTBrainState::Dying)
	{
		if (AWoTNPCCharacter* Self = Cast<AWoTNPCCharacter>(GetPawn()); Self && Self->Health <= 0)
		{
			Self->DropWeaponsOnDeath();
			EnterState(EWoTBrainState::Dying);
		}
	}

	// Prefer-visibility flip cadence (MaybeFlipPreferAcquiredVisibilityHandler,
	// Grunt.uc:1244) — re-roll the ranged-vs-melee preference on the DNI_Misc7
	// period. Dormant unless a creature arms it (FlipPeriod > 0).
	if (TrackingBands.FlipPeriod > 0.0f && State == EWoTBrainState::Tracking)
	{
		FlipTimer -= DeltaSeconds;
		if (FlipTimer <= 0.0f)
		{
			FlipTimer = TrackingBands.FlipPeriod;
			TrackingBands.MaybeFlip();
		}
	}

	// Player-sighting sampler runs in every perceiving state (the engine path
	// had no per-state gate beyond the pawn being conscious); Dormant/Inactive
	// and Dying don't perceive.
	if (Brain && State != EWoTBrainState::Dormant && State != EWoTBrainState::Inactive &&
	    State != EWoTBrainState::Dying)
	{
		SightSampleTimer -= DeltaSeconds;
		if (SightSampleTimer < 0.0f)
		{
			SightSampleTimer = SightSamplePeriod;
			SamplePlayerSight();
		}
	}

	// Fold the ActivityManager's 1s observation pulse: wake Inactive pawns near a
	// relevant player and force active pawns Inactive when no player remains near.
	TickActivityLOD(DeltaSeconds);

	TickState(DeltaSeconds);

	// Pump the creature behavior hook AFTER the state update, so it reads the settled
	// brain state this frame (shield posture / watched-freeze / fade direction).
	if (Behavior && !bSuspendedForIceFreeze)
	{
		Behavior->TickBehavior(DeltaSeconds);
	}
}

// ---------------------------------------------------------------- perception

void AWoTNPCController::SamplePlayerSight()
{
	// The engine player-sighting test: cone
	// stimulus rescale, range scaled by the target's Visibility byte, LOS.
	AWoTNPCCharacter* Self = Cast<AWoTNPCCharacter>(GetPawn());
	if (!Self)
	{
		return;
	}
	for (TActorIterator<AWoTCharacter> It(GetWorld()); It; ++It)
	{
		AWoTCharacter* Target = *It;
		// Only player pawns show themselves on this path; NPCs show themselves
		// on their own Visibility pulse (ShowSelfToNPCs).
		if (Target == Self || Target->IsA<AWoTNPCCharacter>() || Target->Health <= 0)
		{
			continue;
		}
		// The full cone view test: stimulus rescale, height + off-axis
		// distance penalties, Visibility-scaled range, then the trace.
		if (WoTSight::ConeSight(Self, Self->PeripheralVision, Self->SightRadius,
		                        Target, WoTSight::VisibilityOf(Target)))
		{
			DispatchSeePawn(Target);
		}
	}
}

void AWoTNPCController::ShowSelfToNPCs()
{
	// The inverted Legend path: the SEEN pawn broadcasts to NPC observers with
	// a 360° range+LOS test, no cone. Cadence is
	// entirely our own 2s Visibility pulse.
	APawn* Self = GetPawn();
	if (!Self)
	{
		return;
	}
	for (TActorIterator<AWoTNPCCharacter> It(GetWorld()); It; ++It)
	{
		AWoTNPCCharacter* Observer = *It;
		if (Observer == Self)
		{
			continue;
		}
		AWoTNPCController* ObserverController = Cast<AWoTNPCController>(Observer->GetController());
		if (!ObserverController)
		{
			continue;
		}
		// The observer's no-cone view test: 4000uu Visibility-scaled gate +
		// the trace (one-arg LineOfSightTo).
		if (WoTSight::OmniSight(Observer, Self, WoTSight::VisibilityOf(Self)))
		{
			ObserverController->DispatchSeePawn(Self);
		}
	}
	// The projectile-notify branch (self-acquiring seekers) rides this loop
	// when a projectile needs it — none of the ported set does yet
	// (PORT_NOTES).
}

void AWoTNPCController::DispatchSeePawn(APawn* SeenPawn, bool bAllowInactive)
{
	// Alive-gate: the engine generates sightings for dead pawns too
	// (LegendPawn.uc:880). Classify and route (LegendPawn.uc:632-651).
	AWoTNPCCharacter* Self = Cast<AWoTNPCCharacter>(GetPawn());
	if (!Self || !Brain || !SeenPawn)
	{
		return;
	}
	if (const AWoTCharacter* SeenChar = Cast<AWoTCharacter>(SeenPawn))
	{
		if (SeenChar->Health <= 0)
		{
			return;
		}
	}
	switch (Self->GetActorFaction(SeenPawn))
	{
	case EWoTFaction::Enemy:
	{
		// State-local SeeEnemy stubs must also cover the inverted NPC show-self
		// callback. That path calls DispatchSeePawn on the observer directly and
		// does not pass through this controller's player-sight sampler.
		if (Self->Health <= 0 || bSuspendedForIceFreeze
			|| State == EWoTBrainState::Dormant
			|| (State == EWoTBrainState::Inactive && !bAllowInactive)
			|| State == EWoTBrainState::Dying
			|| State == EWoTBrainState::Falling
			|| State == EWoTBrainState::InitialAcquisition)
		{
			return;
		}
		// Acquired* stubs ignore new sightings. Tracking is the folded Acquired
		// shell whenever it owns a live Threat. An alarm/refuge operation can
		// temporarily outrank that slot, but it is still an Acquired state in the
		// shipped hierarchy and must not let a sight sample replace the lock.
		if (State == EWoTBrainState::Tracking
			&& Brain->IsGoalValid(EWoTGoalIndex::Threat))
		{
			return;
		}
		// Captain.InitGoalWith* (Captain.uc:461/474): on the FIRST threat (the
		// threat slot not yet valid), evaluate orders before adopting it — that is
		// what sets the alarm/help GI_Intermediate that then outranks the threat.
		if (bIsCaptain && !Brain->IsGoalValid(EWoTGoalIndex::Threat))
		{
			EvaluateOrders();
		}
		// OnSeeEnemy -> Threat (the visibility pre-seed hint shipped disabled).
		Brain->InitGoalWithActor(EWoTGoalIndex::Threat, SeenPawn);
		break;
	}
	case EWoTFaction::Friendly:
		Brain->InitGoalWithActor(EWoTGoalIndex::Colleague, SeenPawn);
		break;
	default:
		break;   // unknown: no goal (base LegendPawn behavior)
	}
}

void AWoTNPCController::HandleGruntTakeDamage(float RawDamage, APawn* Attacker)
{
	AWoTNPCCharacter* Self = Cast<AWoTNPCCharacter>(GetPawn());
	if (!Self || !Brain || bSuspendedForIceFreeze)
	{
		return;
	}
	// Grunt.uc:765 SeePlayer/SeePawn happens before the movement response.
	if (Attacker)
	{
		// Grunt.TakeDamage invokes SeePlayer/SeePawn directly even while the
		// Inactive state's periodic sight notifiers are disabled. This seeds the
		// retail Threat goal without changing state or waking the pawn.
		DispatchSeePawn(Attacker, true);
	}
	const bool bFriendly = Attacker && Self->IsFriendly(Attacker);
	// Champion.uc:26 disables the inherited hostile-damage retreat with -1.
	// bChampionOrders is the generated exact Champion-class ancestry bit (BAT
	// and Legion); champion-tier Sitter/Questioner still inherit Grunt's 20.
	if (!bFriendly && (Self->bChampionOrders || RawDamage < 20.0f))
	{
		return;
	}
	// Myrddraal.uc:133-143 overrides the heavy-damage escape: when there is no
	// existing intermediate destination it starts PerformTeleportation instead.
	if (!bFriendly && Self->CreatureRowName == TEXT("Myrddraal")
		&& !Brain->IsGoalValid(EWoTGoalIndex::Intermediate))
	{
		BeginTeleportEpisode();
		return;
	}
	MoveAwayFromAreaDirected(160.0f, Self->GetActorRotation());
}

void AWoTNPCController::HandleGruntBump(APawn* Other)
{
	AWoTNPCCharacter* Self = Cast<AWoTNPCCharacter>(GetPawn());
	const AWoTCharacter* OtherCharacter = Cast<AWoTCharacter>(Other);
	if (!Self || !Brain || !OtherCharacter || OtherCharacter->IsNPCPawn()
		|| !Self->IsFriendly(Other) || Other->GetVelocity().IsNearlyZero()
		|| Brain->IsGoalValid(EWoTGoalIndex::Intermediate))
	{
		return;
	}
	// Grunt.uc:699-708: only the moving friendly-player arm reaches the
	// MoveAway call. Both friendly-NPC speed comparisons return early in shipped.
	MoveAwayFromAreaDirected(160.0f, Other->GetActorRotation());
}

void AWoTNPCController::MoveAwayFromAreaDirected(float MoveDistance,
	const FRotator& PreferredRotation, bool bOverrideCurrent)
{
	AWoTNPCCharacter* Self = Cast<AWoTNPCCharacter>(GetPawn());
	if (!Self || !Brain || (!bOverrideCurrent
		&& Brain->IsGoalValid(EWoTGoalIndex::Intermediate)))
	{
		return;
	}
	// LegendPawn.uc:1122-1159 uses UE1 rotation units and integer division.
	// Preserve its nine-attempt sequence exactly; there is deliberately no RNG.
	constexpr int32 YawInterval = 32768 / 8;
	int32 DeltaYaw = 0;
	for (int32 Index = 0; Index <= 8; ++Index)
	{
		if ((Index % 8) == 0)
		{
			DeltaYaw *= -1;
		}
		else
		{
			DeltaYaw += (YawInterval / 2)
				+ (((Index - 1) / 2) * YawInterval);
		}
		FRotator Attempt = PreferredRotation;
		Attempt.Yaw += static_cast<float>(DeltaYaw) * (360.0f / 65536.0f);
		Brain->InitGoalWithVector(EWoTGoalIndex::Intermediate,
			Self->GetActorLocation() + Attempt.Vector() * MoveDistance);
		if (Brain->IsGoalNavigable(EWoTGoalIndex::Intermediate))
		{
			TransitionOnGoalPriority(true);
			return;
		}
		Brain->InvalidateGoal(EWoTGoalIndex::Intermediate);
	}
}

// ------------------------------------------------------------------- hearing

bool AWoTNPCController::IsHearingLive() const
{
	if (!Brain)
	{
		return false;
	}
	if (State == EWoTBrainState::Waiting)
	{
		return true;
	}
	// The folded Tracking state serves acquired/investigating Threat and refuge
	// navigation as well as ordinary returning/guard/alarm tracking. Route by the
	// active operation, not by whether some unrelated Threat slot happens to exist.
	return (State == EWoTBrainState::Tracking || State == EWoTBrainState::Returning)
		&& Brain->CurrentGoalIndex != EWoTGoalIndex::Threat
		&& Brain->CurrentGoalIndex != EWoTGoalIndex::Refuge;
}

void AWoTNPCController::DispatchHearNoise(float Loudness, AActor* NoiseMaker)
{
	// Grunt.OnHearNoise (Grunt.uc:545): a non-hidden noisemaker either transfers a
	// friendly NPC's threat or is acquired as an enemy (SeeEnemyPawn). The state gate
	// mirrors the retail stub set (IsHearingLive); a stubbed state consumes the notify.
	AWoTNPCCharacter* Self = Cast<AWoTNPCCharacter>(GetPawn());
	if (!Self || !Brain || !NoiseMaker || !IsHearingLive())
	{
		return;
	}
	if (NoiseMaker->IsHidden())
	{
		return;   // Grunt.uc:548 !NoiseMaker.bHidden
	}
	// Grunt.GetActorFaction resolves every actor to friendly or enemy (no neutral),
	// so the retail `if IsFriendly ... else if IsA('Pawn')` split is faction-clean.
	if (Self->GetActorFaction(NoiseMaker) == EWoTFaction::Friendly)
	{
		// Grunt.uc:552: only a friendly Grunt (an NPC) hands off its threat.
		if (AWoTNPCCharacter* FriendlyNPC = Cast<AWoTNPCCharacter>(NoiseMaker))
		{
			if (AWoTNPCController* FriendCtl =
			        Cast<AWoTNPCController>(FriendlyNPC->GetController()))
			{
				Brain->TransferThreatGoalsFrom(FriendCtl->GetBrain());
			}
		}
	}
	else if (APawn* NoisePawn = Cast<APawn>(NoiseMaker))
	{
		// Grunt.uc:559 SeeEnemyPawn -> DispatchSeePawn's enemy arm sets the Threat goal.
		DispatchSeePawn(NoisePawn);
	}
}

void AWoTNPCController::BroadcastNoise(UWorld* World, AActor* NoiseMaker, float Loudness,
	APawn* NoiseInstigator)
{
	if (!World || !NoiseMaker)
	{
		return;
	}
	APawn* NoiseOwner = NoiseInstigator ? NoiseInstigator : NoiseMaker->GetInstigator();
	if (!NoiseOwner)
	{
		return;
	}
	for (auto It = InstigatorNoises.CreateIterator(); It; ++It)
	{
		if (!It.Key().IsValid()) It.RemoveCurrent();
	}
	FWoTInstigatorNoise& History = InstigatorNoises.FindOrAdd(NoiseOwner);
	const float Now = World->GetTimeSeconds();
	const FVector NoiseLoc = NoiseMaker->GetActorLocation();
	for (const FWoTNoiseSlot& Slot : History.Slots)
	{
		if (Slot.Time > Now - 0.2 && FVector::DistSquared(Slot.Spot, NoiseLoc) < 2500.0f
			&& Slot.Loudness >= 0.9 * Loudness)
		{
			return;
		}
	}
	FWoTNoiseSlot* Replace = nullptr;
	if (History.Slots[0].Time < Now - 0.18) Replace = &History.Slots[0];
	else if (History.Slots[1].Time < Now - 0.18) Replace = &History.Slots[1];
	else if (FVector::DistSquared(History.Slots[0].Spot, NoiseLoc) < 2500.0f
		&& History.Slots[0].Loudness <= Loudness) Replace = &History.Slots[0];
	// Shipped native quirk: testing slot 2 loudness still overwrites slot 1.
	else if (History.Slots[1].Loudness <= Loudness) Replace = &History.Slots[0];
	if (Replace)
	{
		Replace->Time = Now;
		Replace->Spot = NoiseLoc;
		Replace->Loudness = Loudness;
	}

	bool bPlayerRelated = IsNoisePlayer(NoiseOwner);
	if (const AWoTNPCCharacter* OwnerNPC = Cast<AWoTNPCCharacter>(NoiseOwner))
	{
		bPlayerRelated |= OwnerNPC->Brain && IsNoisePlayer(Cast<APawn>(
			OwnerNPC->Brain->GetGoal(EWoTGoalIndex::Threat).GoalActor.Get()));
	}
	const FWoTRuntimeClassDefaults* NoiseDefaults = nullptr;
	if (!bPlayerRelated && Cast<AWoTNPCCharacter>(NoiseMaker))
	{
		FString Error;
		NoiseDefaults = UWoTPayloadSubsystem::ActiveRuntimeClassDefaults(Error);
#if !WOT_GUARDED_BOOTSTRAP
		if (!NoiseDefaults) NoiseDefaults = WoTDevRetailClassDefaults();
#endif
	}
	for (TActorIterator<AWoTNPCCharacter> It(World); It; ++It)
	{
		AWoTNPCCharacter* Observer = *It;
		if (Observer == NoiseOwner)
		{
			continue;   // UE1 CheckNoiseHearing: next != this->Instigator
		}
		AWoTNPCController* Ctl = Cast<AWoTNPCController>(Observer->GetController());
		if (!Ctl || !Ctl->IsHearingLive())
		{
			continue;   // IsProbing(NAME_HearNoise): stubbed/non-perceiving states skip
		}
		const float DistSq = static_cast<float>(FVector::DistSquared(Observer->GetActorLocation(), NoiseLoc));
		if (DistSq > 4000000.0f * Loudness * Loudness)
		{
			continue;
		}
		if (!bPlayerRelated
			&& !RelatedNoiseClasses(Observer, NoiseMaker, NoiseDefaults))
		{
			continue; // Non-player-related noise only reaches related pawn classes.
		}
		if (DistSq != 0.0f)
		{
			const float Perceived = FMath::Min(1200000.0f / DistSq, 2.0f);
			const float Stimulus = Loudness * Perceived
				+ Observer->Alertness * FMath::Min(0.5f, Perceived);
			if (Stimulus < Observer->HearingThreshold
				|| WoTVisBlockingTraceHits(World, Observer->GetActorLocation(), NoiseLoc, Observer))
			{
				continue;
			}
		}
		Ctl->DispatchHearNoise(Loudness, NoiseMaker);
	}
}

void AWoTNPCController::HandleWalkingOffLedge()
{
	AWoTNPCCharacter* Self = Cast<AWoTNPCCharacter>(GetPawn());
	if (!HasAuthority() || !Self || Self->Health <= 0 || State == EWoTBrainState::Falling)
	{
		return;
	}
	if (UCharacterMovementComponent* Movement = Self->GetCharacterMovement();
		Movement && !Movement->Velocity.IsNearlyZero())
	{
		// Grunt.FallingState.AdjustJump followed by native EAdjustJump /
		// APawn::SuggestJumpVelocity. DesiredSpeed may have reduced the walking
		// velocity; the airborne arc deliberately restores full GroundSpeed.
		const float GroundSpeed = FMath::Max(0.0f, Self->RetailGroundSpeed);
		const FVector Destination = GetActiveLatentDestination();
		const float CurrentVelocityZ = Movement->Velocity.Z;
		const FVector AnticipatedGroundVelocity =
			Movement->Velocity.GetSafeNormal() * GroundSpeed;
		auto SuggestJumpVelocity = [&](FVector Velocity)
		{
			float GravityZ = Movement->GetGravityZ();
			if (GravityZ >= 0.0f) GravityZ = -100.0f;
			const float StartVelocityZ = Velocity.Z;
			const float Floor = static_cast<float>(
				Destination.Z - Self->GetActorLocation().Z);
			float CurrentZ = 0.0f;
			float Ticks = 0.0f;
			while (CurrentZ > Floor || Velocity.Z > 0.0f)
			{
				Velocity.Z += GravityZ * 0.05f;
				Ticks += 0.05f;
				CurrentZ += Velocity.Z * 0.05f;
			}
			if (FMath::Abs(Velocity.Z) > 1.0f)
			{
				Ticks -= (CurrentZ - Floor) / Velocity.Z;
			}
			Velocity = Destination - Self->GetActorLocation();
			Velocity.Z = 0.0f;
			if (Ticks > 0.0f)
			{
				const float Distance = Velocity.Size();
				if (Distance > 0.0f) Velocity /= Distance;
				Velocity *= FMath::Min(GroundSpeed, Distance / Ticks);
			}
			else
			{
				Velocity = Velocity.GetSafeNormal() * GroundSpeed;
			}
			Velocity.Z = StartVelocityZ;
			return Velocity;
		};

		FVector AdjustedVelocity;
		if (Self->GetActorLocation().Z > Destination.Z
			+ Self->GetSimpleCollisionHalfHeight()
			+ 2.0f * Movement->MaxStepHeight)
		{
			AdjustedVelocity = AnticipatedGroundVelocity;
			AdjustedVelocity.Z = CurrentVelocityZ;
			AdjustedVelocity = SuggestJumpVelocity(AdjustedVelocity);
			AdjustedVelocity.Z = 0.0f;
			if (AdjustedVelocity.Size() < 0.9f * GroundSpeed)
			{
				AdjustedVelocity.Z = CurrentVelocityZ;
				Movement->Velocity = AdjustedVelocity;
				FallingReturnState = GetStateForReturn();
				bFallingLanded = false;
				FallingLandingRemaining = 0.0f;
				EnterState(EWoTBrainState::Falling);
				return;
			}
		}
		AdjustedVelocity = AnticipatedGroundVelocity;
		AdjustedVelocity.Z = CurrentVelocityZ + Movement->JumpZVelocity;
		Movement->Velocity = SuggestJumpVelocity(AdjustedVelocity);
	}
	FallingReturnState = GetStateForReturn();
	bFallingLanded = false;
	FallingLandingRemaining = 0.0f;
	EnterState(EWoTBrainState::Falling);
}

void AWoTNPCController::HandleFallingLanded(float ImpactVelocityZ, const FHitResult& Hit,
	bool bOriginatedInFalling)
{
	AWoTNPCCharacter* Self = Cast<AWoTNPCCharacter>(GetPawn());
	if (!Self || !HasAuthority()) return;
	if (Self->Health <= 0)
	{
		EnterState(EWoTBrainState::Dying);
		return;
	}
	if (!bOriginatedInFalling) return;
	// The Falling-local Landed handler continues after Super even if damage
	// briefly entered TakeHit. Re-enter its Landed continuation with the original
	// outer state still owned by FallingReturnState.
	EnterState(EWoTBrainState::Falling);
	const bool bFullAnimation = ImpactVelocityZ <= -800.0f;
	const FWoTAnimTableEntry* Pick = bFullAnimation
		? Self->StampSlotAnim(TEXT("Land")) : Self->TweenSlotAnim(TEXT("Land"));
	FallingLandingRemaining = PickedAnimTotalTime(Pick, bFullAnimation);
	bFallingLanded = true;
	// Retail's level-base landing stops movement before FinishAnim.
	// UE installs the movement base after Character::Landed returns. Use the
	// actual landing hit; an airborne null base says nothing about this surface.
	const UPrimitiveComponent* LandingBase = Hit.GetComponent();
	const AActor* LandingOwner = Hit.GetActor();
	// Runtime BSP retains Base==Level semantics regardless of its component
	// mobility. Imported level BSP uses static StaticMeshActors. A movable
	// platform/mover is separate support, even on the WorldStatic channel.
	const bool bLandedOnLevel = LandingOwner
		&& (LandingOwner->IsA<AWoTRuntimeBSPWorld>()
			|| (LandingOwner->IsA<AStaticMeshActor>() && LandingBase
				&& LandingBase->Mobility == EComponentMobility::Static));
	if (bLandedOnLevel)
	{
		StopMovement();
		Self->GetCharacterMovement()->StopMovementImmediately();
	}
}

void AWoTNPCController::HandleFallingSplash()
{
	const AWoTNPCCharacter* Self = Cast<AWoTNPCCharacter>(GetPawn());
	if (HasAuthority() && State == EWoTBrainState::Falling && Self
		&& Self->GetCharacterMovement()->CanEverSwim())
	{
		// FallingState.ZoneChange takes Splash after the global water handling.
		// Its FinishAnim waits for the remaining Fall tween, with no Land pose.
		bFallingLanded = true;
		FallingLandingRemaining = FallingPoseRemaining;
	}
}

// ------------------------------------------------------------- activity LOD

void AWoTNPCController::TickActivityLOD(float DeltaSeconds)
{
	// Dormant is explicit-wake only; every other live state participates in the
	// ActivityManager pulse so active pawns can also be forced Inactive.
	if (State == EWoTBrainState::Dormant || State == EWoTBrainState::Dying)
	{
		return;
	}
	ActivityEvalTimer -= DeltaSeconds;
	if (ActivityEvalTimer > 0.0f)
	{
		return;
	}
	ActivityEvalTimer = ActivityEvalPeriod;
	bool bHasPlayers = false;
	const bool bRelevantPlayer = HasWakerInRange(bHasPlayers);
	// FindRelevantPlayers leaves DirectObserver's notification at None when the
	// iterator finds no player pawn; OnActivityManagerNotification then does nothing.
	if (!bHasPlayers)
	{
		return;
	}
	if (State == EWoTBrainState::Inactive)
	{
		// ForceActive (LegendPawn.uc:1220 -> TransitionOnGoalPriority(true)): re-select
		// a goal and leave the off-state. Nothing is acquired yet (perception was off),
		// so this falls back to Waiting; the now-armed Visibility/sight pulses acquire
		// the intruder on the next pass -> Tracking (no missed-sighting gap on wake).
		if (bRelevantPlayer && !TransitionOnGoalPriority(true))
		{
			EnterState(EWoTBrainState::Waiting);
		}
		return;
	}
	if (!bRelevantPlayer)
	{
		StopMovement();
		if (ACharacter* CharacterPawn = Cast<ACharacter>(GetPawn()))
		{
			CharacterPawn->GetCharacterMovement()->StopMovementImmediately();
		}
		EnterState(EWoTBrainState::Inactive);
	}
}

bool AWoTNPCController::HasWakerInRange(bool& bOutHasPlayers) const
{
	bOutHasPlayers = false;
	const AWoTNPCCharacter* Self = Cast<AWoTNPCCharacter>(GetPawn());
	if (!Self)
	{
		return false;
	}
	const FVector Origin = Self->GetActorLocation();
	const float RadiusSq = ActivationRadius * ActivationRadius;
	for (TActorIterator<AWoTCharacter> It(GetWorld()); It; ++It)
	{
		AWoTCharacter* Other = *It;
		if (Other == Self || Other->IsA<AWoTNPCCharacter>())
		{
			continue;
		}
		bOutHasPlayers = true;
		const FVector Delta = Other->GetActorLocation() - Origin;
		if (Delta.Z > ActivationRadius || Delta.SizeSquared() > RadiusSq)
		{
			continue;
		}
		// Shipped EO_Distance/DR_LessOrEqual: 3D distance plus the signed delta-Z
		// threshold, no LOS. FindRelevantPlayers admits bIsPlayer pawns only.
		return true;
	}
	return false;
}

void AWoTNPCController::ForceDormant()
{
	// Freeze in place: halt movement, then Dormant (which disables all pulses +
	// perception in BeginState). The activity poll skips Dormant, so proximity will
	// not wake it until UnForceDormant.
	StopMovement();
	if (ACharacter* Char = Cast<ACharacter>(GetPawn()))
	{
		Char->GetCharacterMovement()->StopMovementImmediately();
	}
	EnterState(EWoTBrainState::Dormant);
}

void AWoTNPCController::UnForceDormant()
{
	// UnForceDormant (LegendPawn.uc:2497): clear the freeze and re-select a goal,
	// falling back to Waiting when nothing else is pending.
	if (bSuspendedForIceFreeze || State != EWoTBrainState::Dormant)
	{
		return;
	}
	if (!TransitionOnGoalPriority(true))
	{
		EnterState(EWoTBrainState::Waiting);
	}
}

void AWoTNPCController::SuspendForIceFreeze(bool bHostWasDormantBeforeFreeze)
{
	if (bSuspendedForIceFreeze)
	{
		return;
	}
	bSuspendedForIceFreeze = true;
	bWasDormantBeforeIceFreeze = bHostWasDormantBeforeFreeze;

	StopMovement();
	if (ACharacter* IcePawn = Cast<ACharacter>(GetPawn()))
	{
		IcePawn->GetCharacterMovement()->StopMovementImmediately();
	}
	if (AWoTCharacter* WoTCharacter = Cast<AWoTCharacter>(GetPawn()))
	{
		WoTCharacter->SuspendNPCAnimationForIce();
	}
	// GotoState('') + DisableNotifiers: use the already parity-locked Dormant
	// shell, whose entry tears down the interrupted episode and silences pulses.
	if (State != EWoTBrainState::Dormant)
	{
		EnterState(EWoTBrainState::Dormant);
	}
	else if (Brain)
	{
		Brain->SetActiveNotifiers({});
	}
}

void AWoTNPCController::ReleaseIceFreezeOwnership()
{
	bSuspendedForIceFreeze = false;
	bWasDormantBeforeIceFreeze = false;
}

void AWoTNPCController::ResumeFromIceFreeze()
{
	if (!bSuspendedForIceFreeze)
	{
		return;
	}
	const bool bKeepDormant = bWasDormantBeforeIceFreeze;
	bSuspendedForIceFreeze = false;
	bWasDormantBeforeIceFreeze = false;
	if (AWoTCharacter* WoTCharacter = Cast<AWoTCharacter>(GetPawn()))
	{
		WoTCharacter->ResumeNPCAnimationAfterIce();
	}
	if (bKeepDormant)
	{
		return;
	}
	// IceLeech.UnAttach -> TransitionOnGoalPriority(true): re-enter through goal
	// selection rather than resuming any stale attack/preparation continuation.
	if (!TransitionOnGoalPriority(true))
	{
		EnterState(EWoTBrainState::Waiting);
	}
}

// ---------------------------------------------------------------- FSM core

bool AWoTNPCController::TransitionOnGoalPriority(bool bEvaluate)
{
	if (!Brain)
	{
		return false;
	}
	// A dead pawn diverts to Dying before any goal work (LegendPawn.uc:490).
	if (const AWoTCharacter* Self = Cast<AWoTCharacter>(GetPawn()); Self && Self->Health <= 0)
	{
		EnterState(EWoTBrainState::Dying);
		return true;
	}
	TArray<EWoTGoalIndex> Indices;
	TArray<float> Priorities;
	Brain->GetSortedGoalIndices(Indices, Priorities);
	for (int32 i = 0; i < Indices.Num(); ++i)
	{
		if (Priorities[i] == -1.0f)
		{
			continue;   // empty slot
		}
		if (GoalIndexTransition(Indices[i]))
		{
			Brain->CurrentGoalIndex = Indices[i];
			return true;
		}
	}
	return false;
}

bool AWoTNPCController::GoalIndexTransition(EWoTGoalIndex Index)
{
	// Base LegendPawn accepts Threat / Waiting; Grunt/Captain add Guarding and
	// Intermediate. A zero-priority goal is not acted on. (The caller sets
	// CurrentGoalIndex to this slot after we return true, so ActiveBands() will
	// pick the right ladder next tick.)
	if (!Brain || Brain->GetGoalPriority(Index) <= 0.0f)
	{
		return false;
	}
	switch (Index)
	{
	case EWoTGoalIndex::Threat:
	{
		// Grunt.GoalIndexTransitionThreatWhileHealthyUnFriendlyActor selects the
		// non-reentrant InitialAcquisition component only from the normal VISIBLE
		// acquired-handler path. Raw sight/hearing merely initializes the goal;
		// sorted priority selection reaches this point after Captain orders and
		// higher-priority Intermediate goals have had their turn.
		if (TryEnterInitialAcquisitionForThreat(Brain->IsGoalVisible(Index))) return true;
		EnterState(EWoTBrainState::Tracking);
		return true;
	}
	case EWoTGoalIndex::Guarding:
		// GoalIndexTransitionGuarding (Grunt.uc:414): return toward the guard spot
		// (TI_Returning). A strayed pawn (Guarding scores only beyond 480) navigates
		// back and holds — pursued through the same tracking loop (vector goal, no
		// attack fires). A grunt copies its leader's guard goal via FindLeader.
		AcquisitionHandlerThreat.Reset();
		bAcquisitionHandlerWasVisible = false;
		EnterState(EWoTBrainState::Tracking);
		return true;
	case EWoTGoalIndex::Intermediate:
		// GoalIndexTransitionIntermediate (Grunt.uc:444 / Captain.uc:487): navigate
		// to the alarm/help/seal. The .uc's route-by-actor-type picks WHICH
		// transitioner; the fold uses one navigation ladder + the on-arrival
		// dispatch keyed by actor type (HandleIntermediateArrival).
		AcquisitionHandlerThreat.Reset();
		bAcquisitionHandlerWasVisible = false;
		EnterState(EWoTBrainState::Tracking);
		return true;
	case EWoTGoalIndex::Waiting:
		// GI_Waiting binds TI_Returning. Reached completes immediately into Waiting;
		// reachable/pathable walk at the waiting goal's authored speed first.
		AcquisitionHandlerThreat.Reset();
		bAcquisitionHandlerWasVisible = false;
		EnterState(EWoTBrainState::Returning);
		return true;
	default:
		return false;
	}
}

bool AWoTNPCController::TryEnterInitialAcquisitionForThreat(bool bVisibleHandler)
{
	AWoTNPCCharacter* Self = Cast<AWoTNPCCharacter>(GetPawn());
	const FWoTGoal& Threat = Brain->GetGoal(EWoTGoalIndex::Threat);
	AActor* ThreatActor = Threat.GoalActor.Get();
	const bool bEligibleVisibleHandler = Self && ThreatActor
		&& !Self->IsFriendly(ThreatActor) && bVisibleHandler;
	const bool bEnteringVisibleHandler = bEligibleVisibleHandler
		&& (AcquisitionHandlerThreat.Get() != ThreatActor
			|| !bAcquisitionHandlerWasVisible);
	// Consume the handler edge even when its 60-second component gate is
	// closed. Merely remaining visible until the deadline must not synthesize
	// another range-handler entry.
	AcquisitionHandlerThreat = ThreatActor;
	bAcquisitionHandlerWasVisible = bEligibleVisibleHandler;
	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
	if (!bEnteringVisibleHandler
		|| (NextInitialAcquisitionTime >= 0.0f && Now < NextInitialAcquisitionTime))
	{
		return false;
	}
	NextInitialAcquisitionTime = Now + 60.0f;
	EnterState(EWoTBrainState::InitialAcquisition);
	return true;
}

void AWoTNPCController::EnterState(EWoTBrainState NewState)
{
	if (State == NewState)
	{
		return;
	}
	EndState(State);
	State = NewState;
	BeginState(State);
}

bool AWoTNPCController::IsTakeHitSuppressed() const
{
	// Retail state-local empty stubs suppress only weapon preparation (both melee
	// and projectile through inheritance), AttemptAttack, TakeHit itself, and the
	// BATrolloc Recovering state (Grunt.uc:1266,1327,2119; BATrolloc.uc:200).
	if (bSuspendedForIceFreeze || State == EWoTBrainState::PrepareWeapon ||
		State == EWoTBrainState::AttemptAttack || State == EWoTBrainState::TakeHit)
	{
		return true;
	}
	// TeleporterReached's TakeDamage stub makes the whole blink INVULNERABLE
	// (Myrddraal.uc:361) — suppress the hit-react detour too.
	if (State == EWoTBrainState::Teleporting)
	{
		return true;
	}
	if (const UWoTBATrollocRecoveryBehavior* Recovery =
		Cast<UWoTBATrollocRecoveryBehavior>(Behavior))
	{
		return Recovery->IsRecovering();
	}
	return false;
}

float AWoTNPCController::PickedAnimTotalTime(const FWoTAnimTableEntry* Pick,
	bool bIncludeSequence) const
{
	if (!Pick)
	{
		return 0.0f;
	}
	const float TweenTime = WoTAnimTable::ResolveTweenTime(Pick);
	if (!bIncludeSequence)
	{
		return TweenTime;
	}
	const AWoTCharacter* Self = Cast<AWoTCharacter>(GetPawn());
	const float SequenceTime = Self
		? Self->SequenceDurationSeconds(Pick->AnimName, WoTAnimTable::ResolveRate(Pick))
		: 0.0f;
	if (const UWoTVertexAnimComponent* Anim =
		Self ? Cast<UWoTVertexAnimComponent>(Self->GetMesh()) : nullptr;
		Anim && Anim->FindSequence(Pick->AnimName))
	{
		return TweenTime + SequenceTime; // a one-frame sequence legitimately adds zero
	}
	return TweenTime + (SequenceTime > 0.0f ? SequenceTime : 1.0f);
}

EWoTBrainState AWoTNPCController::GetStateForReturn() const
{
	// Retail TakeHitState and FallingState leave the shared outer continuation
	// untouched. Nesting these detours must never save the detour itself.
	if (State == EWoTBrainState::TakeHit) return TakeHitReturnState;
	if (State == EWoTBrainState::Falling) return FallingReturnState;
	return State;
}

bool AWoTNPCController::TryEnterTakeHit(FName HitSlot, bool bHardHit)
{
	AWoTNPCCharacter* Self = Cast<AWoTNPCCharacter>(GetPawn());
	if (!Self || HitSlot.IsNone() || IsTakeHitSuppressed())
	{
		return false;
	}
	// FallingState's GetNextStateAndLabelForReturn is empty: a hit detour keeps
	// the state saved by Falling(), rather than returning to an already-landed fall.
	const EWoTBrainState InterruptedState = GetStateForReturn();
	const FWoTAnimTableEntry* Pick = bHardHit
		? Self->StampSlotAnim(HitSlot) : Self->TweenSlotAnim(HitSlot);
	if (!Pick)
	{
		return false;
	}
	TakeHitReturnState = InterruptedState;
	TakeHitElapsed = 0.0f;
	TakeHitDuration = PickedAnimTotalTime(Pick, bHardHit);
	if (bHardHit)
	{
		// TakeHitState's hard arm calls StopMovement before TweenPlay (Grunt.uc:2133).
		StopMovement();
		if (UCharacterMovementComponent* Move = Self->GetCharacterMovement())
		{
			Move->StopMovementImmediately();
		}
	}
	// Retail's soft arm never stops movement (Grunt.uc:2128-2136) and a UE1
	// GotoState leaves Velocity/Acceleration flowing through physics — a
	// soft-flinching grunt keeps moving. Latch so EndState(Tracking) preserves
	// the active leg for a SOFT detour only.
	bSoftTakeHitTransition = !bHardHit;
	EnterState(EWoTBrainState::TakeHit);
	bSoftTakeHitTransition = false;
	return true;
}

bool AWoTNPCController::IsArcherKneeling() const
{
	const AWoTNPCCharacter* Self = Cast<AWoTNPCCharacter>(GetPawn());
	return Self && Self->CreatureRowName == FName(TEXT("Archer")) &&
		State == EWoTBrainState::PerformProjectile && ArcherPhase != 0;
}

FName AWoTNPCController::GetArcherAttackPhaseName() const
{
	static const FName Names[] = {NAME_None, TEXT("Kneel"), TEXT("Draw"),
		TEXT("Hold"), TEXT("RickyPause"), TEXT("Release"), TEXT("GetUp")};
	return ArcherPhase < UE_ARRAY_COUNT(Names) ? Names[ArcherPhase] : NAME_None;
}

void AWoTNPCController::BeginState(EWoTBrainState InState)
{
	if (!Brain)
	{
		return;
	}
	// Per-state notifier sets.
	switch (InState)
	{
	case EWoTBrainState::Waiting:
		Brain->SetActiveNotifiers({EWoTNotifierSlot::Visibility, EWoTNotifierSlot::Waiting});
		if (const FWoTGoal& Waiting = Brain->GetGoal(EWoTGoalIndex::Waiting);
			Waiting.HasVector())
		{
			WaitingGoalSnapshot = Waiting.GoalVector;
			WaitingFacingSnapshot = Waiting.AssociatedRotation;
		}
		if (ACharacter* Char = Cast<ACharacter>(GetPawn()))
		{
			Char->GetCharacterMovement()->StopMovementImmediately();
		}
		UpdateWaitingCower();
		break;
	case EWoTBrainState::Returning:
		Brain->SetActiveNotifiers({EWoTNotifierSlot::Visibility, EWoTNotifierSlot::Defensive});
		BeginReturning();
		break;
	case EWoTBrainState::InitialAcquisition:
		// InitialAcquisition inherits the acquired visibility/rotation machinery but
		// explicitly disables the offensive notifier.
		Brain->SetActiveNotifiers({EWoTNotifierSlot::Visibility, EWoTNotifierSlot::Rotation});
		BeginInitialAcquisition();
		break;
	case EWoTBrainState::Tracking:
		Brain->SetActiveNotifiers({EWoTNotifierSlot::Visibility, EWoTNotifierSlot::Defensive,
		                           EWoTNotifierSlot::Offensive});
		break;
	case EWoTBrainState::AttemptAttack:
	case EWoTBrainState::PerformMelee:
		// Attack states re-aim continuously: Rotation (and Movement for melee)
		// pulses on, offensive cadence off while mid-swing.
		Brain->SetActiveNotifiers({EWoTNotifierSlot::Rotation, EWoTNotifierSlot::Movement});
		break;
	case EWoTBrainState::CasterUse:
	case EWoTBrainState::CasterDefensiveUse:
		// PerformUse / AttemptDefensiveAngrealUse: re-aim on, Defensive disabled while
		// mid-cast (AngrealUser.uc:372-383 disable DNI_Defensive during the attack). A
		// caster stands still to cast (PerformUse's StopMovement) — kill any residual
		// approach velocity so a cast that started mid-approach doesn't coast to
		// point-blank.
		Brain->SetActiveNotifiers({EWoTNotifierSlot::Rotation});
		if (ACharacter* Char = Cast<ACharacter>(GetPawn()))
		{
			Char->GetCharacterMovement()->StopMovementImmediately();
		}
		if (Brain->GetGoal(Brain->CurrentGoalIndex).HasActor())
		{
			SetFocus(Brain->GetGoal(Brain->CurrentGoalIndex).GoalActor.Get());
		}
		break;
	case EWoTBrainState::PerformProjectile:
		// PerformProjectileAttack: the Archer kneels (StopMovement, Archer.uc:201) and
		// re-aims at the target while drawing. Same stand-still-and-face posture as the
		// caster; the arrow fires from TickProjectile at the weapon's FireNotifyTime.
		Brain->SetActiveNotifiers({EWoTNotifierSlot::Rotation});
		if (ACharacter* Char = Cast<ACharacter>(GetPawn()))
		{
			Char->GetCharacterMovement()->StopMovementImmediately();
		}
		if (Brain->GetGoal(Brain->CurrentGoalIndex).HasActor())
		{
			SetFocus(Brain->GetGoal(Brain->CurrentGoalIndex).GoalActor.Get());
		}
		break;
	case EWoTBrainState::PrepareWeapon:
		Brain->SetActiveNotifiers({EWoTNotifierSlot::Rotation});
		if (ACharacter* Char = Cast<ACharacter>(GetPawn()))
		{
			Char->GetCharacterMovement()->StopMovementImmediately();
		}
		break;
	case EWoTBrainState::TakeHit:
		Brain->SetActiveNotifiers({});
		break;
	case EWoTBrainState::Falling:
		Brain->SetActiveNotifiers({});
		if (AWoTNPCCharacter* Self = Cast<AWoTNPCCharacter>(GetPawn()))
		{
			FallingPoseRemaining = PickedAnimTotalTime(Self->TweenSlotAnim(TEXT("Fall")), false);
			SetFocalPoint(Self->GetActorLocation() + Self->GetActorForwardVector() * 10000.0f);
		}
		break;
	case EWoTBrainState::Dodging:
		// PerformDodge disables the Defensive notifier (LegendPawn.uc:2277); promoting
		// the label to a state, the whole set goes quiet for the sub-second episode (the
		// pawn is blocked in WaitForLanding). Only the Defensive-off is load-bearing; the
		// other notifiers being briefly silent is a minor divergence (PORT_NOTES).
		Brain->SetActiveNotifiers({});
		break;
	case EWoTBrainState::Teleporting:
		// TeleporterReached disables Tick and drives the blink off its own timers
		// (Myrddraal.uc:365) — the whole notifier set goes quiet for the episode.
		Brain->SetActiveNotifiers({});
		break;
	case EWoTBrainState::Inactive:
	case EWoTBrainState::Dormant:
	case EWoTBrainState::Dying:
		Brain->SetActiveNotifiers({});
		break;
	default:
		break;
	}

	if (InState == EWoTBrainState::PerformMelee)
	{
		StartMeleeSwing();
	}
	if (InState == EWoTBrainState::PerformProjectile)
	{
		if (bResumeArcherGetUpAfterPreparation)
		{
			bResumeArcherGetUpAfterPreparation = false;
			BeginArcherPhase(6);
		}
		else
		{
			StartProjectileAttack();
		}
	}
	if (InState == EWoTBrainState::Dodging)
	{
		StartDodge();
	}
	if (InState == EWoTBrainState::Teleporting)
	{
		EnterTeleportState();
	}
	if (InState == EWoTBrainState::Dying)
	{
		// A pawn that dies mid-walk-to-beacon ends the episode; EndState(Teleporting) ran
		// first (EnterState) to restore a mid-vanish pawn's collision/visibility (F3).
		bTeleportBeaconActive = false;
	}
}

void AWoTNPCController::EndState(EWoTBrainState InState)
{
	if (InState == EWoTBrainState::PrepareWeapon
		&& !bCompletingWeaponPreparationEpisode)
	{
		// The Archer's post-release GetUp continuation belongs only to the
		// preparation it armed. Ice and every other forced exit abandon it.
		bResumeArcherGetUpAfterPreparation = false;
	}
	if (InState == EWoTBrainState::Waiting)
	{
		EndWaitingCower();
	}
	if (InState == EWoTBrainState::Returning || InState == EWoTBrainState::InitialAcquisition)
	{
		bDirectMoveActive = false;
		if (bPathMoveActive)
		{
			StopMovement();
			bPathMoveActive = false;
		}
		ClearFocus(EAIFocusPriority::Gameplay);
	}
	if (InState == EWoTBrainState::Falling)
	{
		ClearFocus(EAIFocusPriority::Gameplay);
	}
	if (InState == EWoTBrainState::Tracking)
	{
		bAcquiredVoiceHandlerActive = false;
		AcquiredVoiceProximity = EWoTGoalProximity::None;
		// A gong strike cut short leaves the alarm unrung; the return to Tracking
		// re-runs the arrival (retail TakeHit resumes the state at Begin).
		bGongStrikeActive = false;
		GongAlarm.Reset();
		// Stop any in-flight legs (the FSM's InterruptMovement) — EXCEPT into a
		// soft TakeHit detour, which retail rides out while still moving
		// (Grunt.uc:2128-2136 soft arm; no StopMovement). The leg resumes when
		// the detour returns and Tracking re-begins.
		if (!bSoftTakeHitTransition)
		{
			bDirectMoveActive = false;
			bMovementCycleMoving = false;
			if (bPathMoveActive)
			{
				StopMovement();
				bPathMoveActive = false;
			}
		}
	}
	if (InState == EWoTBrainState::PerformMelee)
	{
		PendingSwingAttacks.Reset();
		PendingSwingTimes.Reset();
		PendingSlashTimes.Reset();
		// A grab episode cut short (a TakeHit detour, a lost target) must not leave the
		// Movement notifier disabled — restore it the way the completed episode does.
		if (bGrabSwing && Brain)
		{
			Brain->SetNotifierEnabled(EWoTNotifierSlot::Movement, true);
		}
		bGrabSwing = false;
		bStationaryMeleeSwing = false;
		if (ACharacter* Char = Cast<ACharacter>(GetPawn()))
		{
			// PerformMeleeAttack.EndState calls StopMovement. No acceleration from
			// the last zero-duration pulse may leak into the next state.
			Char->GetCharacterMovement()->StopMovementImmediately();
		}
		ClearFocus(EAIFocusPriority::Gameplay);
	}
	if (InState == EWoTBrainState::CasterUse || InState == EWoTBrainState::CasterDefensiveUse)
	{
		if (UWoTCasterComponent* Caster =
		        GetPawn() ? GetPawn()->FindComponentByClass<UWoTCasterComponent>() : nullptr)
		{
			Caster->Release();
		}
		ClearFocus(EAIFocusPriority::Gameplay);
	}
	if (InState == EWoTBrainState::PerformProjectile)
	{
		ClearFocus(EAIFocusPriority::Gameplay);
		ArcherPhase = 0;
	}
	if (InState == EWoTBrainState::Teleporting)
	{
		// A blink force-exited before ExitMyrddraalTeleporting (unpossess, death, teardown,
		// a test force-transition) must not leave the pawn hidden, non-collidable, or dimmed.
		// Idempotent with the normal reappear (TeleportPhase 3 CommitReappear already
		// restored these) — SetActorHiddenInGame(false)/EnableCollision(true) are no-ops
		// when already applied. Guard the hide-restore on the phase that hid it: Enter
		// MyrddraalTeleporting (case 1) hid the pawn + sword, advancing to TeleportPhase >= 2.
		if (AWoTNPCCharacter* Self = Cast<AWoTNPCCharacter>(GetPawn()))
		{
			if (TeleportPhase >= 2)
			{
				Self->SetActorEnableCollision(true);
				Self->SetActorHiddenInGame(false);
				if (AWoTNPCWeapon* Held = Self->GetHeldWeapon())
				{
					Held->SetActorHiddenInGame(false);
				}
			}
			// ScaleGlow was dimmed at the wind-up (EnterTeleportState) — restore the row default.
			Self->SetScaleGlowPresentation(WoTCreatureData::Find(Self->CreatureRowName)
				? WoTCreatureData::Find(Self->CreatureRowName)->ScaleGlow : 1.0f);
		}
	}
}

void AWoTNPCController::TickState(float DeltaSeconds)
{
	switch (State)
	{
	case EWoTBrainState::Waiting:
		if (Brain)
		{
			const FWoTGoal& Waiting = Brain->GetGoal(EWoTGoalIndex::Waiting);
			if (Waiting.HasVector() && (!Waiting.GoalVector.Equals(WaitingGoalSnapshot, 0.01f)
				|| !Waiting.AssociatedRotation.Equals(WaitingFacingSnapshot, 0.01f)))
			{
				EnterState(EWoTBrainState::Returning);
				break;
			}
		}
		TickWaitingCower(DeltaSeconds);
		break;
	case EWoTBrainState::Returning:
		TickReturning(DeltaSeconds);
		break;
	case EWoTBrainState::InitialAcquisition:
		TickInitialAcquisition();
		break;
	case EWoTBrainState::Tracking:
		TickTracking(DeltaSeconds);
		break;
	case EWoTBrainState::Falling:
		if (!bFallingLanded)
		{
			FallingPoseRemaining = FMath::Max(0.0f, FallingPoseRemaining - DeltaSeconds);
		}
		if (bFallingLanded)
		{
			FallingLandingRemaining -= DeltaSeconds;
			if (FallingLandingRemaining <= 0.0f)
			{
				EnterState(FallingReturnState);
			}
		}
		break;
	case EWoTBrainState::PerformMelee:
		TickMelee(DeltaSeconds);
		break;
	case EWoTBrainState::PerformProjectile:
		TickProjectile(DeltaSeconds);
		break;
	case EWoTBrainState::PrepareWeapon:
		TickWeaponPreparation(DeltaSeconds);
		break;
	case EWoTBrainState::Dodging:
		// WaitForLanding (LegendPawn.uc:2279): hold until the falling physics settles,
		// then finish the dodge. A pawn that never entered Falling (already grounded when
		// launched) completes on the first poll — the same immediate return retail's
		// WaitForLanding gives.
		if (const ACharacter* Char = Cast<ACharacter>(GetPawn());
		    !Char || Char->GetCharacterMovement()->MovementMode != MOVE_Falling)
		{
			CompleteDodge();
		}
		break;
	case EWoTBrainState::Teleporting:
		// The blink timeline (TeleporterReached's SetTimer chain, Myrddraal.uc:480-526):
		// each phase runs for its crank-up/duration, then AdvanceTeleportPhase steps to
		// the next (vanish, relocate, reappear) and finally resumes Tracking.
		TeleportPhaseElapsed += DeltaSeconds;
		if (TeleportPhase <= 1)
		{
			if (AWoTNPCCharacter* Self=Cast<AWoTNPCCharacter>(GetPawn()))
			{
				Self->SetScaleGlowPresentation(Self->ScaleGlow > 0.25f
					? Self->ScaleGlow - 0.025f : 0.0f);
				if (AWoTNPCWeapon* Held=Self->GetHeldWeapon())
					Held->ScaleGlow=FMath::Max(0.0f,Held->ScaleGlow-0.025f);
			}
		}
		if (TeleportPhaseElapsed >= TeleportPhaseDuration)
		{
			AdvanceTeleportPhase();
		}
		break;
	case EWoTBrainState::TakeHit:
		TakeHitElapsed += DeltaSeconds;
		if (TakeHitElapsed >= TakeHitDuration)
		{
			// Retail returns to (State,'Begin') — GetNextStateAndLabelForReturn's
			// default — which RESTARTS the interrupted state's Begin code. For a
			// caster state that re-runs the cast decision (the weave was released
			// when the detour left the state), so restart a fresh cast; if no
			// weave selects, fall back to Tracking like the state's own exit.
			if (TakeHitReturnState == EWoTBrainState::CasterUse ||
				TakeHitReturnState == EWoTBrainState::CasterDefensiveUse)
			{
				UWoTCasterComponent* Caster =
					GetPawn() ? GetPawn()->FindComponentByClass<UWoTCasterComponent>() : nullptr;
				AActor* Victim = Brain && Brain->GetGoal(Brain->CurrentGoalIndex).HasActor()
					? Brain->GetGoal(Brain->CurrentGoalIndex).GoalActor.Get() : nullptr;
				AWoTAngrealInventory* Weave = Caster && Victim
					? Caster->SelectOffensiveWeave(Victim) : nullptr;
				if (Weave)
				{
					EnterState(TakeHitReturnState);
					Caster->BeginCast(Weave);
				}
				else
				{
					EnterState(EWoTBrainState::Tracking);
				}
				break;
			}
			EnterState(TakeHitReturnState);
		}
		break;
	case EWoTBrainState::CasterUse:
	case EWoTBrainState::CasterDefensiveUse:
		// The caster component runs the usage cycle; when the volley ends
		// (released, exhausted, or target lost) return to the tracking loop.
		if (UWoTCasterComponent* Caster =
		        GetPawn() ? GetPawn()->FindComponentByClass<UWoTCasterComponent>() : nullptr;
		    !Caster || !Caster->IsCasting())
		{
			EnterState(EWoTBrainState::Tracking);
		}
		break;
	default:
		break;
	}
}

void AWoTNPCController::OnNotifierPulse(EWoTNotifierSlot Slot)
{
	switch (Slot)
	{
	case EWoTNotifierSlot::Waiting:
		// Idle re-evaluation (LegendPawn.uc Waiting state → OnWaitingNotification):
		// a goal that outranks Waiting (a fresh threat) takes over here.
		if (State == EWoTBrainState::Waiting)
		{
			UpdateWaitingCower();
			// Captain OnWaitingNotification (Captain.uc:448): Global runs first —
			// the idle heal (inert; morale disabled). Then the priority check.
			CaptainIdleHeal();
			// A captain that STAYS idle re-runs its waiting tracking operation,
			// whose WaitingIdle override resets orders (Captain.uc:591-596). Only on
			// continue-waiting, never on the transition out (which would clobber a
			// just-run EvaluateOrders' WorkingOrders).
			TArray<EWoTGoalIndex> Indices;
			TArray<float> Priorities;
			Brain->GetSortedGoalIndices(Indices, Priorities);
			const EWoTGoalIndex PriorityGoal = Indices.Num() > 0 && Priorities[0] > 0.0f
				? Indices[0]
				: EWoTGoalIndex::Waiting;
			// LegendPawn.Waiting.ShouldContinueWaiting consumes Waiting and
			// Colleague locally. In particular, it must not re-enter Returning for
			// its own Waiting goal on every notifier pulse: doing so interrupts
			// ShowRespect and starts a spurious trip back to the unchanged post.
			if (PriorityGoal == EWoTGoalIndex::Waiting
				|| PriorityGoal == EWoTGoalIndex::Colleague)
			{
				if (bIsCaptain)
				{
					ResetOrders();
				}
			}
			else
			{
				TransitionOnGoalPriority(true);
			}
		}
		break;
	case EWoTNotifierSlot::Movement:
		// PerformMeleeAttack enables a zero-duration movement notifier. Each pulse
		// applies AccelRate toward the live goal; rooted subclass attacks disable it.
		if (State == EWoTBrainState::PerformMelee)
		{
			MatchGoalAcceleration();
		}
		break;
	case EWoTNotifierSlot::Visibility:
		// Our show-self pulse: reveal ourselves to NPC observers. Only in
		// show-self states (Tracking family + Waiting).
		if (State == EWoTBrainState::Waiting || State == EWoTBrainState::Returning
			|| State == EWoTBrainState::InitialAcquisition
			|| State == EWoTBrainState::Tracking)
		{
			ShowSelfToNPCs();
		}
		break;
	case EWoTNotifierSlot::Defensive:
		// The caster defensive reflex (AngrealUser.uc:242 DefensivePerimiterCompromised
		// → :257 AttemptDefensiveAngrealUse): on the 0.05s pulse, if an incoming weave
		// is inside the perimeter and aimed at us (the detector + seeker gate), select a
		// defensive weave and cast it. The Defensive slot is live only in Tracking, so a
		// caster reacts while engaged (parity: DNI_Defensive on in TrackingBase only).
		if (State == EWoTBrainState::Tracking && GetPawn())
		{
			if (UWoTCasterComponent* Caster =
			        GetPawn()->FindComponentByClass<UWoTCasterComponent>())
			{
				if (AActor* Offender = Caster->FindIncomingWeave())
				{
					if (AWoTAngrealInventory* Weave = Caster->SelectDefensiveWeave(Offender))
					{
						EnterState(EWoTBrainState::CasterDefensiveUse);
						Caster->BeginCast(Weave);
					}
				}
			}
		}
		// The melee active-detection dodge (LegendPawn PendingOffenderCollision ->
		// CurrentMovementManager.DefensiveDodge -> PerformDodge, MovementManager.uc):
		// armed for exactly Warder/Trolloc/Minion by their creature row. A dodge-armed
		// pawn is never a caster, so this never fires alongside the reflex above; the
		// state guard also skips it if the reflex just left Tracking.
		if (State == EWoTBrainState::Tracking)
		{
			TryBeginDodge();
		}
		// The Myrddraal blink's ACTIVE detector (DefensiveDetectorMyrddraal, Myrddraal.uc
		// :618): on the Defensive pulse, a seeker homing on us within 2048 + FOV whose
		// arrival is inside the response window fires PerformTeleportation. The Myrddraal
		// is bDefensiveDodge=false (census: it blinks, it does not dodge), so this never
		// races the dodge above; the state guard keeps it Tracking-only.
		if (State == EWoTBrainState::Tracking)
		{
			if (const UWoTMyrddraalTeleportBehavior* Teleport =
			        Cast<UWoTMyrddraalTeleportBehavior>(Behavior))
			{
				if (Teleport->DetectActiveTeleportOffender())
				{
					BeginTeleportEpisode();
				}
			}
		}
		break;
	case EWoTNotifierSlot::Offensive:
		// Attack-attempt cadence (LegendPawn.uc:2035 → AcquiredBase's offensive
		// notifier, Grunt.uc:1624). Route by the active band's attack: a melee band
		// swings once the goal is inside its standoff; a caster band drives the weave
		// (which enforces its own range); a projectile band kneel-fires the NPC's
		// ranged weapon (the Archer's bow, gated by DetermineWeaponUsage).
		if (State == EWoTBrainState::Tracking && !bForcedStationary && Brain && GetPawn() &&
		    Brain->IsGoalValid(Brain->CurrentGoalIndex) &&
		    Brain->GetGoal(Brain->CurrentGoalIndex).HasActor())
		{
			// Grunt.uc:1244-1252: the offensive-cadence notifier may flip the
			// acquired-visible preference with 1/3 odds. Archer.uc:87 pins ranged
			// preference; its existing ArcherBow behavior remains the sole switch.
			if (const AWoTNPCCharacter* NPC = Cast<AWoTNPCCharacter>(GetPawn());
				NPC && NPC->GetRangedWeapon() && NPC->CreatureRowName != TEXT("Archer"))
			{
				TrackingBands.MaybeFlip();
			}
			const EWoTGoalIndex Goal = Brain->CurrentGoalIndex;
			const FWoTRangeBandSet& Bands = ActiveBands();
			if (AWoTNPCCharacter* NPC = Cast<AWoTNPCCharacter>(GetPawn()))
			{
				if (AWoTNPCWeapon* Weapon = NPC->GetRangedWeapon();
					Weapon && !Weapon->IsInFlight())
				{
					// One handler roll per offensive evaluation. TickTracking reuses it
					// until this pulse instead of flickering per frame (RangeHandlerSniping.uc:39).
					Weapon->RefreshOddsRoll();
				}
			}
			const EWoTGoalProximity Proximity = ClassifyWithWeaponGate(Bands, Goal);
			RouteWeaponForProximity(Proximity);
			if (State != EWoTBrainState::Tracking)
			{
				return; // preparation owns the episode before any attack can start
			}
			const FWoTRangeBand& Band = Bands.Band(Proximity);
			AActor* Victim = Brain->GetGoal(Goal).GoalActor.Get();
			if (Band.Attack == EWoTBandAttack::Melee)
			{
				const AWoTNPCCharacter* Self = Cast<AWoTNPCCharacter>(GetPawn());
				float Gap = 0.0f;
				bool bInAttackReach = Self && Self->MeleeAttacks.Num() > 0 &&
				    Brain->GetGoalDistance(Goal, Gap) && Gap <= Band.StandoffDistance;
				// The Minion's tentacle grab reaches past the melee standoff (its
				// WithinMaxMeleeDistance override, Minion.uc:220-223 → GetPendingWeapon,
				// Grunt.uc:1728): enter the attack when the goal is in the tentacle band.
				if (!bInAttackReach && Self && Self->MeleeAttacks.Num() > 0)
				{
					if (const UWoTMinionStealthBehavior* Minion =
					        Cast<UWoTMinionStealthBehavior>(Behavior))
					{
						bInAttackReach = Minion->InUseTentacleDistance(Gap);
					}
				}
				if (bInAttackReach)
				{
					EnterState(EWoTBrainState::PerformMelee);
				}
			}
			else if (Band.Attack == EWoTBandAttack::Caster)
			{
				// Legion seeker/stomp arbitration (Legion.uc:209-226 GetAttackActor): a
				// VISIBLE threat inside the volcano stomp reach is a melee (stomp) attack,
				// not a seeker — WithinMaxMeleeDistance is true only when the goal is
				// visible AND in range (uc:197-206), and the stomp behavior lands it on
				// its own cadence. Withhold the seeker so the pawn stays in Tracking for
				// the stomp; a not-visible or out-of-reach threat falls through to the
				// seeker (Super.GetAttackActor, uc:222).
				bool bStompClaims = false;
				if (const UWoTLegionSpiritsStompBehavior* Stomp =
				        Cast<UWoTLegionSpiritsStompBehavior>(Behavior))
				{
					float StompGap = 0.0f;
					bStompClaims = Brain->IsGoalVisible(Goal) &&
					    Brain->GetGoalDistance(Goal, StompGap) && StompGap <= Stomp->StompRange;
				}
				if (!bStompClaims)
				{
					if (UWoTCasterComponent* Caster =
					        GetPawn()->FindComponentByClass<UWoTCasterComponent>())
					{
						if (AWoTAngrealInventory* Weave = Caster->SelectOffensiveWeave(Victim))
						{
							EnterState(EWoTBrainState::CasterUse);
							Caster->BeginCast(Weave);
						}
					}
				}
			}
			else if (Band.Attack == EWoTBandAttack::Projectile)
			{
				// The bow (Archer): DetermineWeaponUsage gates the shot on the
				// projectile-range window + LOS (WotWeapon.uc:285). WU_Projectile ->
				// kneel and fire; WU_None (target too close for the bow's
				// MinProjectileRange, or out of LOS) withholds — the ArcherBow
				// behavior's melee switch commits to melee when it stays close.
				AWoTNPCCharacter* NPC = Cast<AWoTNPCCharacter>(GetPawn());
				AWoTNPCWeapon* Weapon = NPC ? NPC->GetRangedWeapon() : nullptr;
				if (Weapon)
				{
					if (!Weapon->IsInFlight())
					{
						float Gap = 0.0f;
						Brain->GetGoalDistance(Goal, Gap);
						// Retail evaluates DetermineWeaponUsage again when choosing the attack
						// actor (Grunt.uc:1343), so this is intentionally a second, fresh roll.
						Weapon->DetermineWeaponUsage(Gap, Brain->IsGoalVisible(Goal));
						if (Weapon->GetWeaponUsage() == EWoTWeaponUsage::Projectile)
						{
							EnterState(EWoTBrainState::PerformProjectile);
						}
					}
				}
				else if (UWoTWeaponComponent* LegacyWeapon =
					GetPawn()->FindComponentByClass<UWoTWeaponComponent>())
				{
					float Gap = 0.0f;
					Brain->GetGoalDistance(Goal, Gap);
					LegacyWeapon->DetermineWeaponUsage(Gap, Brain->IsGoalVisible(Goal));
					if (LegacyWeapon->GetWeaponUsage() == EWoTWeaponUsage::Projectile)
					{
						EnterState(EWoTBrainState::PerformProjectile);
					}
				}
			}
		}
		break;
	default:
		break;
	}
}

// ---------------------------------------------------------------- tracking

FWoTRangeBandSet AWoTNPCController::BuildTrackingBands() const
{
	// A caster pawn holds + casts; a ranged-weapon pawn holds + shoots (the Archer);
	// a melee pawn closes + swings. (The per-creature RangeHandlerFactory, folded to
	// capability data — the creature rows author the full set.) The weapon
	// component is created by the ArcherBow behavior's OnPossessedBy, which runs
	// BEFORE this in OnPossess, so a bow pawn is detectable here.
	const AWoTNPCCharacter* Self = Cast<AWoTNPCCharacter>(GetPawn());
	// The Legion hybrid (Legion.uc:193-207): a caster+melee champion. Arming its seeker
	// weave gives it a CasterComponent, which would otherwise hand it CasterDefault and
	// hold it at seek range forever — never closing to its volcano stomp. The
	// LegionSpiritsStomp behavior marks it; route to the visibility-arbitrated ladder
	// that closes to the stomp when it can SEE the threat and holds+seeks when it cannot.
	if (const UWoTLegionSpiritsStompBehavior* Stomp = Cast<UWoTLegionSpiritsStompBehavior>(Behavior))
	{
		const UWoTCasterComponent* Caster =
			GetPawn() ? GetPawn()->FindComponentByClass<UWoTCasterComponent>() : nullptr;
		const float SeekRange = Caster ? Caster->HoldRange : 3200.0f;
		return FWoTRangeBandSet::LegionDefault(Stomp->StompRange, SeekRange);
	}
	if (const UWoTWeaponComponent* Weapon =
	        GetPawn() ? GetPawn()->FindComponentByClass<UWoTWeaponComponent>() : nullptr)
	{
		return FWoTRangeBandSet::ProjectileFighterDefault(
			Self ? Self->MeleeRange : 25.0f, Weapon->MaxProjectileRange);
	}
	if (const UWoTCasterComponent* Caster =
	        GetPawn() ? GetPawn()->FindComponentByClass<UWoTCasterComponent>() : nullptr)
	{
		return FWoTRangeBandSet::CasterDefault(Caster->HoldRange);
	}
	if (const AWoTNPCWeapon* Weapon = Self ? Self->GetRangedWeapon() : nullptr)
	{
		return FWoTRangeBandSet::ProjectileFighterDefault(
			Self->MeleeRange, Weapon->MaxProjectileRange);
	}
	return FWoTRangeBandSet::MeleeDefault(Self ? Self->MeleeRange : 25.0f);
}

void AWoTNPCController::RouteWeaponForProximity(EWoTGoalProximity Proximity)
{
	AWoTNPCCharacter* Self = Cast<AWoTNPCCharacter>(GetPawn());
	if (!Self || !HasAuthority() || State == EWoTBrainState::PrepareWeapon)
	{
		return;
	}
	if (Proximity == EWoTGoalProximity::Visible)
	{
		if (AWoTNPCWeapon* Ranged = Self->GetRangedWeapon(); Ranged && !Ranged->IsInFlight())
		{
			BeginWeaponSwitch(Ranged, true);
		}
	}
	else if (Proximity == EWoTGoalProximity::Reached ||
		Proximity == EWoTGoalProximity::Reachable)
	{
		if (AWoTNPCWeapon* Melee = Self->GetMeleeWeapon(); Melee && !Melee->IsInFlight())
		{
			BeginWeaponSwitch(Melee, false);
		}
		else if ((!Self->GetMeleeWeapon() || Self->GetMeleeWeapon()->IsInFlight()) &&
			Self->MeleeRange > 0.0f)
		{
			BeginWeaponSwitch(nullptr, false, true); // Grunt.uc:986-993 body-attack fallback
		}
	}
}

void AWoTNPCController::RouteToComponentBackedRangedWeapon()
{
	if (State != EWoTBrainState::PrepareWeapon)
	{
		BeginWeaponSwitch(nullptr, true, true);
	}
}

void AWoTNPCController::BeginReturning()
{
	// LegendPawn's Tracking.PerformInactivity enforces MinTrackingDuration=0.01
	// between completed iterations. A fresh/changed waiting post owns a fresh
	// bounded set of UE navigation submissions.
	ReturningPathRetryRemaining = 0.0f;
	ReturningPathFailureCount = 0;
	AWoTNPCCharacter* Self = Cast<AWoTNPCCharacter>(GetPawn());
	if (!Brain || !Self)
	{
		return;
	}
	const FWoTGoal& Waiting = Brain->GetGoal(EWoTGoalIndex::Waiting);
	if (!Waiting.HasVector())
	{
		Brain->SetWaitingGoal(Self->GetActorLocation(), Self->GetActorRotation());
	}
	const FWoTGoal& Authored = Brain->GetGoal(EWoTGoalIndex::Waiting);
	ReturningTarget = Authored.GoalVector;
	MoveDestination = ReturningTarget;
	ReturningFacing = Authored.AssociatedRotation;
	bReturningFacing = false;
	bDirectMoveActive = false;
	if (bPathMoveActive)
	{
		StopMovement();
		bPathMoveActive = false;
	}
	Self->GetCharacterMovement()->StopMovementImmediately();
	// The normal spawn post is already reached and already faces its authored
	// rotation. Retail's Returning label falls through that zero-length episode
	// immediately, without executing a movement leg or replacing the pawn's
	// authored GroundSpeed. Do the same instead of exposing Returning for one frame.
	if (Brain->IsGoalReached(EWoTGoalIndex::Waiting))
	{
		FinishReturning();
		const float Error = FMath::Abs(FMath::FindDeltaAngleDegrees(
			Self->GetActorRotation().Yaw, ReturningFacing.Yaw));
		// UE's desired-rotation driver can settle a couple of degrees short of
		// the focal yaw. Retail's rotation notification is tolerant; once inside
		// that window, stamp the authored post rotation exactly before idling.
		if (Error <= 5.0f)
		{
			Self->SetActorRotation(ReturningFacing);
			EnterState(EWoTBrainState::Waiting);
		}
		return;
	}
	const float ReturnSpeed = NPCSpeedFraction
		* Brain->GetGoal(EWoTGoalIndex::Waiting).SuggestedSpeed;
	Self->GetCharacterMovement()->MaxWalkSpeed = ReturnSpeed;
	Self->GetCharacterMovement()->MaxFlySpeed = ReturnSpeed;
}

void AWoTNPCController::FinishReturning()
{
	AWoTNPCCharacter* Self = Cast<AWoTNPCCharacter>(GetPawn());
	if (!Self)
	{
		return;
	}
	bDirectMoveActive = false;
	if (bPathMoveActive)
	{
		StopMovement();
		bPathMoveActive = false;
	}
	Self->GetCharacterMovement()->StopMovementImmediately();
	bReturningFacing = true;
	SetFocalPoint(ReturningTarget + ReturningFacing.Vector() * 200.0f);
}

void AWoTNPCController::TickReturning(float DeltaSeconds)
{
	AWoTNPCCharacter* Self = Cast<AWoTNPCCharacter>(GetPawn());
	if (!Brain || !Self)
	{
		return;
	}
	// Returning expands Tracking in retail. Poll the same sorted goal set every
	// iteration so Threat/Intermediate/Guarding can preempt the trip home. The
	// unchanged Waiting goal simply selects Returning again; EnterState's
	// same-state guard prevents a restart.
	TransitionOnGoalPriority(true);
	if (State != EWoTBrainState::Returning)
	{
		return;
	}
	const FWoTGoal& Waiting = Brain->GetGoal(EWoTGoalIndex::Waiting);
	if (!Waiting.HasVector())
	{
		Brain->SetWaitingGoal(Self->GetActorLocation(), Self->GetActorRotation());
		EnterState(EWoTBrainState::Waiting);
		return;
	}
	if (!Waiting.GoalVector.Equals(ReturningTarget, 0.01f)
		|| !Waiting.AssociatedRotation.Equals(ReturningFacing, 0.01f))
	{
		BeginReturning(); // Waiting.SetWaitingGoal restarts the returning operation.
	}

	if (bReturningFacing)
	{
		SetFocalPoint(ReturningTarget + ReturningFacing.Vector() * 200.0f);
		const float Error = FMath::Abs(FMath::FindDeltaAngleDegrees(
			Self->GetActorRotation().Yaw, ReturningFacing.Yaw));
		if (Error <= 5.0f)
		{
			Self->SetActorRotation(ReturningFacing);
			EnterState(EWoTBrainState::Waiting);
		}
		return;
	}

	if (Brain->IsGoalReached(EWoTGoalIndex::Waiting))
	{
		FinishReturning();
		return;
	}
	if (ReturningPathRetryRemaining > 0.0f)
	{
		ReturningPathRetryRemaining -= DeltaSeconds;
		if (ReturningPathRetryRemaining > 0.0f)
		{
			return;
		}
		// Keep the prior Pathable result consistent for the whole attempt/backoff.
		// Reclassify exactly once when the next retail-cadence attempt begins.
		Brain->GetGoal(EWoTGoalIndex::Waiting).Cache.InvalidatePair(
			EWoTGoalCacheItem::PathableOK,
			EWoTGoalCacheItem::PathableFail);
	}

	const bool bReachable = Brain->IsGoalReachable(EWoTGoalIndex::Waiting);
	if (!bReachable && !Brain->IsGoalPathable(EWoTGoalIndex::Waiting))
	{
		// UnableToReturn.InitiateTrackingOperation adopts the stranded transform as
		// the new post. Do this only after both navigation classifications fail.
		Brain->SetWaitingGoal(Self->GetActorLocation(), Self->GetActorRotation());
		ReturningTarget = Self->GetActorLocation();
		ReturningFacing = Self->GetActorRotation();
		EnterState(EWoTBrainState::Waiting);
		return;
	}

	Self->GetCharacterMovement()->MaxWalkSpeed = 160.0f;
	Self->GetCharacterMovement()->MaxFlySpeed = 160.0f;
	SetFocalPoint(ReturningTarget);
	if (bReachable)
	{
		ReturningPathFailureCount = 0;
		ReturningPathRetryRemaining = 0.0f;
		bDirectMoveActive = true;
		Self->AddMovementInput((ReturningTarget - Self->GetActorLocation()).GetSafeNormal());
	}
	else if (!bPathMoveActive)
	{
		bDirectMoveActive = false;
		MoveDestination = ReturningTarget;
		const EPathFollowingRequestResult::Type Result = MoveToLocation(
			ReturningTarget, 16.0f, true, true);
		bPathMoveActive = Result == EPathFollowingRequestResult::RequestSuccessful;
		if (Result == EPathFollowingRequestResult::RequestSuccessful)
		{
			ReturningPathFailureCount = 0;
			ReturningPathRetryRemaining = 0.0f;
		}
		if (Result == EPathFollowingRequestResult::AlreadyAtGoal)
		{
			ReturningPathFailureCount = 0;
			ReturningPathRetryRemaining = 0.0f;
			FinishReturning();
		}
		else if (Result == EPathFollowingRequestResult::Failed)
		{
			const bool bCountFailure = HasValidReturnNavigationStart(*Self);
			StopMovement();
			bPathMoveActive = false;
			bDirectMoveActive = false;
			Self->GetCharacterMovement()->StopMovementImmediately();
			// Transient locomotion and starts outside this agent's nav data can make
			// UE reject a request before the guard has a meaningful chance to return.
			// They retry at retail Tracking cadence without consuming the UE-only bound.
			if (bCountFailure)
			{
				++ReturningPathFailureCount;
			}
			if (bCountFailure && ReturningPathFailureCount >= 3)
			{
				// UE's path follower has contradicted the same Pathable classification
				// repeatedly. Finish through retail UnableToReturn rather than freezing
				// the guard in a per-frame path-request loop forever.
				Brain->SetWaitingGoal(
					Self->GetActorLocation(), Self->GetActorRotation());
				ReturningTarget = Self->GetActorLocation();
				ReturningFacing = Self->GetActorRotation();
				ReturningPathRetryRemaining = 0.0f;
				EnterState(EWoTBrainState::Waiting);
			}
			else
			{
				ReturningPathRetryRemaining = 0.01f;
			}
		}
	}
}

void AWoTNPCController::BeginInitialAcquisition()
{
	AWoTNPCCharacter* Self = Cast<AWoTNPCCharacter>(GetPawn());
	if (!Brain || !Self)
	{
		return;
	}
	bDirectMoveActive = false;
	if (bPathMoveActive)
	{
		StopMovement();
		bPathMoveActive = false;
	}
	Self->GetCharacterMovement()->StopMovementImmediately();
	if (const FWoTGoal& Threat = Brain->GetGoal(EWoTGoalIndex::Threat); Threat.HasActor())
	{
		SetFocus(Threat.GoalActor.Get());
	}
	// SeeEnemySoundOdds is 1.0 for Grunt and all current MP troop leaves.
	Self->PlayVoiceSlot(TEXT("SeeEnemy"));
}

void AWoTNPCController::TickInitialAcquisition()
{
	AWoTNPCCharacter* Self = Cast<AWoTNPCCharacter>(GetPawn());
	if (!Brain || !Self || !Brain->IsGoalValid(EWoTGoalIndex::Threat))
	{
		if (!TransitionOnGoalPriority(true))
		{
			EnterState(EWoTBrainState::Returning);
		}
		return;
	}
	const FWoTGoal& Threat = Brain->GetGoal(EWoTGoalIndex::Threat);
	if (!Threat.HasActor())
	{
		return;
	}
	SetFocus(Threat.GoalActor.Get());
	const FVector Delta = Threat.GoalActor->GetActorLocation() - Self->GetActorLocation();
	const float Error = Delta.IsNearlyZero() ? 0.0f : FMath::Abs(
		FMath::FindDeltaAngleDegrees(Self->GetActorRotation().Yaw, Delta.Rotation().Yaw));
	if (Error <= 2.0f)
	{
		TransitionOnGoalPriority(true);
	}
}

void AWoTNPCController::EnterAcquiredVoiceHandler()
{
	if (!Brain || !GetWorld() || !Brain->IsGoalValid(EWoTGoalIndex::Threat))
	{
		return;
	}
	const float Now = GetWorld()->GetTimeSeconds();
	// Grunt.PlayAcquiredSound (Grunt.uc:1646-1658) is called from
	// Acquired.BeginState. The first call only initializes the deadline.
	if (NextAcquiredSoundTime < 0.0f)
	{
		NextAcquiredSoundTime = Now + 5.0f;
		return;
	}
	if (Now < NextAcquiredSoundTime)
	{
		return;
	}
	if (AWoTNPCCharacter* Self = Cast<AWoTNPCCharacter>(GetPawn()))
	{
		Self->PlayVoiceSlot(TEXT("Acquired"));
	}
	NextAcquiredSoundTime = Now + 5.0f;
}

void AWoTNPCController::BeginWaitingCower()
{
	if (bWaitingCower)
	{
		return;
	}
	AWoTNPCCharacter* Self = Cast<AWoTNPCCharacter>(GetPawn());
	if (!Self)
	{
		return;
	}
	bWaitingCower = true;
	bWaitingCowerLoop = false;
	bWaitingCowerNotifyFired = false;
	WaitingCowerElapsed = 0.0f;
	WaitingCowerNotifyTime = -1.0f;
	WaitingCowerLoopElapsed = 0.0f;
	WaitingCowerLoopDuration = 0.0f;
	WaitingCowerLoopNotifyTime = -1.0f;
	Self->GetCharacterMovement()->StopMovementImmediately();
	const FWoTAnimTableEntry* Pick = Self->StampSlotAnim(TEXT("ShowRespect"));
	WaitingCowerRemaining = PickedAnimTotalTime(Pick, true);
	if (WaitingCowerRemaining <= 0.0f)
	{
		WaitingCowerRemaining = 1.0f;
	}
	if (Pick)
	{
		const float TweenTime = WoTAnimTable::ResolveTweenTime(Pick);
		const float SequenceTime = Self->SequenceDurationSeconds(
			Pick->AnimName, WoTAnimTable::ResolveRate(Pick));
		if (const UWoTVertexAnimComponent* Anim =
			Cast<UWoTVertexAnimComponent>(Self->GetMesh()))
		{
			const float Fraction = Anim->FindNotifyFraction(
				Pick->AnimName, TEXT("PlayAnimSound"));
			if (Fraction >= 0.0f)
			{
				WaitingCowerNotifyTime = TweenTime + Fraction * SequenceTime;
			}
		}
	}
}

void AWoTNPCController::EndWaitingCower()
{
	if (!bWaitingCower)
	{
		return;
	}
	bWaitingCower = false;
	bWaitingCowerLoop = false;
	WaitingCowerRemaining = 0.0f;
	WaitingCowerElapsed = 0.0f;
	WaitingCowerNotifyTime = -1.0f;
	bWaitingCowerNotifyFired = false;
	WaitingCowerLoopElapsed = 0.0f;
	WaitingCowerLoopDuration = 0.0f;
	WaitingCowerLoopNotifyTime = -1.0f;
	if (AWoTNPCCharacter* Self = Cast<AWoTNPCCharacter>(GetPawn()))
	{
		Self->LoopSlotAnim(TEXT("Breath"));
	}
}

void AWoTNPCController::UpdateWaitingCower()
{
	if (State != EWoTBrainState::Waiting || !Brain)
	{
		return;
	}
	const FWoTGoal& Colleague = Brain->GetGoal(EWoTGoalIndex::Colleague);
	const AWoTCharacter* Player = Colleague.HasActor()
		? Cast<AWoTCharacter>(Colleague.GoalActor.Get()) : nullptr;
	float Gap = 0.0f;
	const bool bShouldCower = Player && !Player->IsNPCPawn()
		&& Brain->GetGoalDistance(EWoTGoalIndex::Colleague, Gap) && Gap < 256.0f;
	if (bShouldCower)
	{
		BeginWaitingCower();
	}
	else
	{
		EndWaitingCower();
	}
}

void AWoTNPCController::TickWaitingCower(float DeltaSeconds)
{
	if (!bWaitingCower)
	{
		return;
	}
	AWoTNPCCharacter* Self = Cast<AWoTNPCCharacter>(GetPawn());
	if (!Self)
	{
		return;
	}
	if (bWaitingCowerLoop)
	{
		const float Previous = WaitingCowerLoopElapsed;
		WaitingCowerLoopElapsed += DeltaSeconds;
		if (WaitingCowerLoopDuration > KINDA_SMALL_NUMBER
			&& WaitingCowerLoopNotifyTime >= 0.0f)
		{
			const int32 FirstCycle = FMath::Max(0, FMath::FloorToInt(
				(Previous - WaitingCowerLoopNotifyTime) / WaitingCowerLoopDuration) + 1);
			const int32 LastCycle = FMath::FloorToInt(
				(WaitingCowerLoopElapsed - WaitingCowerLoopNotifyTime)
				/ WaitingCowerLoopDuration);
			for (int32 Cycle = FirstCycle; Cycle <= LastCycle; ++Cycle)
			{
				Self->PlayVoiceSlot(TEXT("ShowRespect"));
			}
		}
		return;
	}
	WaitingCowerElapsed += DeltaSeconds;
	WaitingCowerRemaining -= DeltaSeconds;
	if (!bWaitingCowerNotifyFired && WaitingCowerNotifyTime >= 0.0f
		&& WaitingCowerElapsed >= WaitingCowerNotifyTime)
	{
		bWaitingCowerNotifyFired = true;
		Self->PlayVoiceSlot(TEXT("ShowRespect"));
	}
	if (WaitingCowerRemaining <= 0.0f)
	{
		bWaitingCowerLoop = true;
		WaitingCowerLoopElapsed = 0.0f;
		if (const FWoTAnimTableEntry* Pick = Self->LoopSlotAnim(TEXT("ShowRespectLoop")))
		{
			WaitingCowerLoopDuration = Self->LoopSequenceDurationSeconds(
				Pick->AnimName, WoTAnimTable::ResolveRate(Pick));
			if (const UWoTVertexAnimComponent* Anim =
				Cast<UWoTVertexAnimComponent>(Self->GetMesh()))
			{
				const float Fraction = Anim->FindNotifyFraction(
					Pick->AnimName, TEXT("PlayAnimSound"));
				if (Fraction >= 0.0f)
				{
					WaitingCowerLoopNotifyTime =
						Fraction * WaitingCowerLoopDuration;
				}
			}
		}
	}
}

void AWoTNPCController::BeginWeaponSwitch(AWoTNPCWeapon* Destination,
	bool bRanged, bool bDestinationIsBodyAttack)
{
	AWoTNPCCharacter* Self = Cast<AWoTNPCCharacter>(GetPawn());
	if (!Self || State == EWoTBrainState::PrepareWeapon)
	{
		return;
	}
	AWoTNPCWeapon* Held = Self->GetHeldWeapon();
	if ((!bDestinationIsBodyAttack && Held == Destination)
		|| (bDestinationIsBodyAttack && Held == nullptr))
	{
		return;
	}

	PreparationDestination = Destination;
	bPreparationDestinationRanged = bRanged;
	bPreparationDestinationBodyAttack = bDestinationIsBodyAttack;
	PreparationReturnState = GetStateForReturn();

	// WotWeapon.DownWeapon.TweenDown calls PrepareInventoryItem on the CURRENT
	// weapon before ChangedWeapon. This is independent of whether the destination
	// itself needs a bring-up preparation (WotWeapon.uc:794-823).
	if (Held && Held != Destination && Held->ShouldBePreparedBy(Self))
	{
		EnterState(EWoTBrainState::PrepareWeapon);
		const bool bHeldIsRanged = Held == Self->GetRangedWeapon()
			&& Held != Self->GetMeleeWeapon();
		StartWeaponPreparationPhase(Held, bHeldIsRanged, true);
		return;
	}
	if (Destination && Destination->ShouldBePreparedBy(Self))
	{
		EnterState(EWoTBrainState::PrepareWeapon);
		StartWeaponPreparationPhase(Destination, bRanged, false);
		return;
	}
	ApplyPreparedWeaponDestination();
	PreparationDestination.Reset();
	bPreparationDestinationBodyAttack = false;
}

void AWoTNPCController::StartWeaponPreparationPhase(AWoTNPCWeapon* Weapon,
	bool bRanged, bool bPuttingAway)
{
	AWoTNPCCharacter* Self = Cast<AWoTNPCCharacter>(GetPawn());
	if (!Self || !Weapon)
	{
		return;
	}
	PreparationWeapon = Weapon;
	bPreparationRanged = bRanged;
	bPreparationPuttingAway = bPuttingAway;
	bPreparationSwitched = false;
	PreparationElapsed = 0.0f;
	PreparationSwitchTime = 0.0f;
	PreparationEndTime = 0.0f;
	// Retail PrepareInventoryItem selects by weapon CLASS, not by direction.
	// Thus DownWeapon reuses WeaponTakeOutRanged/Melee too (Grunt.uc:1050-1069,
	// :1299-1318); preserve that odd but shipped animation choice.
	const FWoTAnimTableEntry* Pick = Self->StampSlotAnim(
		bRanged ? FName(TEXT("WeaponTakeOutRanged")) : FName(TEXT("WeaponTakeOutMelee")));
	PreparationEndTime = PickedAnimTotalTime(Pick, true);
	if (PreparationEndTime <= 0.0f)
	{
		PreparationEndTime = 1.0f;
	}
	PreparationSwitchTime = 0.5f * PreparationEndTime;
	if (Pick)
	{
		const float TweenTime = WoTAnimTable::ResolveTweenTime(Pick);
		const float SequenceTime = Self->SequenceDurationSeconds(
			Pick->AnimName, WoTAnimTable::ResolveRate(Pick));
		if (const UWoTVertexAnimComponent* Anim =
			Cast<UWoTVertexAnimComponent>(Self->GetMesh()))
		{
			const float Fraction = Anim->FindNotifyFraction(
				Pick->AnimName, TEXT("WeaponSwitchAnimNotification"));
			if (Fraction >= 0.0f)
			{
				PreparationSwitchTime = TweenTime + Fraction * SequenceTime;
			}
		}
	}
	if (Brain)
	{
		if (const FWoTGoal& Threat = Brain->GetGoal(EWoTGoalIndex::Threat);
			Threat.HasActor())
		{
			SetFocus(Threat.GoalActor.Get());
		}
	}
}

void AWoTNPCController::ApplyPreparedWeaponDestination()
{
	if (AWoTNPCCharacter* Self = Cast<AWoTNPCCharacter>(GetPawn()))
	{
		if (bPreparationDestinationBodyAttack)
		{
			Self->EquipBodyAttack();
		}
		else if (AWoTNPCWeapon* Destination = PreparationDestination.Get())
		{
			if (Destination == Self->GetRangedWeapon())
			{
				Self->EquipRangedWeapon();
			}
			else if (Destination == Self->GetMeleeWeapon())
			{
				Self->EquipMeleeWeapon();
			}
		}
	}
}

void AWoTNPCController::CompleteWeaponPreparation()
{
	if (bPreparationSwitched)
	{
		return;
	}
	bPreparationSwitched = true;
	if (!bPreparationPuttingAway)
	{
		ApplyPreparedWeaponDestination();
		return;
	}

	// The down-weapon notify first makes Pawn.Weapon=None. ChangedWeapon then
	// immediately begins a preparable destination, or assigns a plain/body arm.
	if (AWoTNPCCharacter* Self = Cast<AWoTNPCCharacter>(GetPawn()))
	{
		Self->EquipBodyAttack();
	}
	if (AWoTNPCWeapon* Destination = PreparationDestination.Get();
		Destination && Destination->ShouldBePreparedBy(GetPawn()))
	{
		StartWeaponPreparationPhase(
			Destination, bPreparationDestinationRanged, false);
		return;
	}
	ApplyPreparedWeaponDestination();
}

void AWoTNPCController::FinishWeaponPreparationEpisode()
{
	PreparationWeapon.Reset();
	PreparationDestination.Reset();
	bPreparationDestinationBodyAttack = false;
	bPreparationPuttingAway = false;
	bPreparationSwitched = false;
	ClearFocus(EAIFocusPriority::Gameplay);
	bCompletingWeaponPreparationEpisode = true;
	EnterState(PreparationReturnState);
	bCompletingWeaponPreparationEpisode = false;
}

void AWoTNPCController::TickWeaponPreparation(float DeltaSeconds)
{
	if (!PreparationWeapon.IsValid())
	{
		if (bPreparationPuttingAway)
		{
			if (AWoTNPCWeapon* Destination = PreparationDestination.Get();
				Destination && Destination->ShouldBePreparedBy(GetPawn()))
			{
				StartWeaponPreparationPhase(
					Destination, bPreparationDestinationRanged, false);
				return;
			}
			ApplyPreparedWeaponDestination();
		}
		FinishWeaponPreparationEpisode();
		return;
	}
	AWoTNPCWeapon* PhaseWeapon = PreparationWeapon.Get();
	const bool bWasPuttingAway = bPreparationPuttingAway;
	PreparationElapsed += DeltaSeconds;
	if (!bPreparationSwitched && PreparationElapsed >= PreparationSwitchTime)
	{
		CompleteWeaponPreparation();
	}
	// A down notify may have synchronously started the destination preparation.
	if (PreparationWeapon.Get() != PhaseWeapon
		|| bPreparationPuttingAway != bWasPuttingAway)
	{
		return;
	}
	if (PreparationElapsed >= PreparationEndTime)
	{
		CompleteWeaponPreparation();
		if (PreparationWeapon.Get() != PhaseWeapon
			|| bPreparationPuttingAway != bWasPuttingAway)
		{
			return;
		}
		FinishWeaponPreparationEpisode();
	}
}

EWoTGoalProximity AWoTNPCController::ClassifyWithWeaponGate(
	const FWoTRangeBandSet& Bands, EWoTGoalIndex Goal)
{
	EWoTGoalProximity Proximity = Bands.Classify(Brain, Goal);
	if (Proximity != EWoTGoalProximity::Visible ||
		Bands.Band(Proximity).Attack != EWoTBandAttack::Projectile || !Brain)
	{
		return Proximity;
	}

	float Gap = 0.0f;
	if (!Brain->GetGoalDistance(Goal, Gap))
	{
		return Proximity;
	}
	bool bProjectileCapable = false;
	if (AWoTNPCCharacter* NPC = Cast<AWoTNPCCharacter>(GetPawn()))
	{
		if (AWoTNPCWeapon* Weapon = NPC->GetRangedWeapon())
		{
			if (!Weapon->IsInFlight())
			{
				// The handler's stochastic gate is sticky until the next offensive pulse;
				// distance and LOS still classify afresh each tracking frame.
				Weapon->DetermineWeaponUsage(
					Gap, Brain->IsGoalVisible(Goal), Weapon->GetCachedOddsRoll());
				bProjectileCapable = Weapon->GetWeaponUsage() == EWoTWeaponUsage::Projectile;
			}
		}
		else if (UWoTWeaponComponent* LegacyWeapon = NPC->FindComponentByClass<UWoTWeaponComponent>())
		{
			LegacyWeapon->DetermineWeaponUsage(Gap, Brain->IsGoalVisible(Goal));
			bProjectileCapable = LegacyWeapon->GetWeaponUsage() == EWoTWeaponUsage::Projectile;
		}
	}
	if (bProjectileCapable)
	{
		return Proximity;
	}

	// RangeHandlerSniping.uc:10-49: withhold the hold handler, then let the
	// ordinary Reached/Reachable/Visible/Pathable ladder choose the close arm.
	FWoTRangeBandSet WithoutSniping = Bands;
	WithoutSniping.bPreferVisibility = false;
	WithoutSniping.Bands[static_cast<int32>(EWoTGoalProximity::Visible)].bActive = false;
	return WithoutSniping.Classify(Brain, Goal);
}

void AWoTNPCController::TickTracking(float DeltaSeconds)
{
	AWoTNPCCharacter* Self = Cast<AWoTNPCCharacter>(GetPawn());
	if (!Brain || !Self)
	{
		return;
	}
	const EWoTGoalIndex Goal = Brain->CurrentGoalIndex;

	// Goal gone (killed, dropped by strikes, gated out) → re-select; nothing
	// acceptable falls back through the separate Returning family to Waiting.
	if (!Brain->IsGoalValid(Goal) || Brain->GetGoalPriority(Goal) <= 0.0f)
	{
		// The teleport beacon IS the Intermediate goal (BeginTeleportEpisode); if it drops
		// before arrival, end the episode so a fresh trigger can re-arm (IsTeleportEpisode
		// Active would otherwise latch forever). The failed node stays in LastTeleport
		// Position, so the retry picks a different one (uc:283 selection-time stamp).
		if (bTeleportBeaconActive && Goal == EWoTGoalIndex::Intermediate)
		{
			bTeleportBeaconActive = false;
		}
		bDirectMoveActive = false;
		if (!TransitionOnGoalPriority(true))
		{
			EnterState(EWoTBrainState::Waiting);
		}
		return;
	}

	// A captain mid gong strike holds at the alarm until FinishAnim returns.
	if (TickGongStrike(DeltaSeconds))
	{
		return;
	}

	// Behavior ForceStationary (Minion stealth freeze, Minion.uc:176): hold position —
	// stop any leg, kill residual velocity, keep facing the goal, and don't advance.
	if (bForcedStationary)
	{
		MovementManager.PreHint = MovementManager.PostHint = NAME_None;
		bMovementCycleActive = false;
		bDirectMoveActive = false;
		if (bPathMoveActive)
		{
			StopMovement();
			bPathMoveActive = false;
		}
		Self->GetCharacterMovement()->StopMovementImmediately();
		if (const FWoTGoal& G = Brain->GetGoal(Goal); G.HasActor())
		{
			SetFocus(G.GoalActor.Get());
		}
		return;
	}

	// The range-band ladder (RangeTransitioner.SelectGoalProximityHandler,
	// RangeTransitioner.uc:207): classify the goal's proximity, act on that
	// band's data. This is the fold of the ~90 Acquired*/tracking states into a
	// per-creature band set, replacing a simpler
	// binary melee-band-vs-chase check. ActiveBands() picks the intermediate-
	// navigation ladder while acting on GI_Intermediate, else the threat ladder.
	const FWoTRangeBandSet& Bands = ActiveBands();
	TrackingProximity = ClassifyWithWeaponGate(Bands, Goal);
	// A held Threat can change acquired handlers without changing goal or
	// controller state (most importantly, hearing's nonvisible handler becoming
	// visible). Detect that edge at the range-handler classification boundary.
	if (Goal == EWoTGoalIndex::Threat
		&& TryEnterInitialAcquisitionForThreat(Brain->IsGoalVisible(Goal)))
	{
		return;
	}
	// Each retail Acquired* handler inherits Acquired.BeginState, which calls
	// PlayAcquiredSound exactly on entry. The port folds those states into one
	// Tracking state, so reproduce the same edge on first handler selection and
	// on a proximity-handler transition — never by polling a timer each tick.
	if (Goal == EWoTGoalIndex::Threat)
	{
		if (!bAcquiredVoiceHandlerActive || AcquiredVoiceProximity != TrackingProximity)
		{
			EnterAcquiredVoiceHandler();
			bAcquiredVoiceHandlerActive = true;
			AcquiredVoiceProximity = TrackingProximity;
		}
	}
	else
	{
		bAcquiredVoiceHandlerActive = false;
		AcquiredVoiceProximity = EWoTGoalProximity::None;
	}
	const FWoTRangeBand& Band = Bands.Band(TrackingProximity);
	RouteWeaponForProximity(TrackingProximity);
	if (State != EWoTBrainState::Tracking)
	{
		return; // a preparation-capable weapon owns the named-step episode now
	}
	// GI_Intermediate on-arrival (SuccessfullyNavigatedToGoal, Grunt.uc:1905 /
	// Captain arrival states): the moment the alarm/help/seal goal classifies
	// Reached, run the by-actor-type dispatch and re-select. Stop any leg first.
	if (Goal == EWoTGoalIndex::Intermediate && TrackingProximity == EWoTGoalProximity::Reached)
	{
		bDirectMoveActive = false;
		if (bPathMoveActive)
		{
			StopMovement();
			bPathMoveActive = false;
		}
		// SuccessfullyNavigatedToTeleportProxy (Myrddraal.uc:323-338): the Intermediate
		// beacon that a Myrddraal blink placed IS the teleport destination — arriving
		// runs the vanish/reappear (state TeleporterReached), not the alarm/help dispatch.
		if (bTeleportBeaconActive)
		{
			bTeleportBeaconActive = false;
			Brain->InvalidateGoal(EWoTGoalIndex::Intermediate);   // uc:486
			EnterState(EWoTBrainState::Teleporting);
			return;
		}
		HandleIntermediateArrival();
		return;
	}

	// A drop band (or no band bound at all): invalidate the goal and re-select
	// next iteration. The acquired-threat UnNavigable band uses the approaching
	// waypoint selector and remains active, so a bare threat never drops here; the
	// intermediate-navigation bands (SuccessfullyNavigatedTo* → invalidate
	// if !Navigable, Grunt.uc:1955) arm bDropGoal. A VISIBLE goal never lands on
	// UnNavigable either — it takes the Visible band and is approached (melee) or
	// held-and-cast (caster), the faithful improvement over the old
	// "!IsGoalNavigable → drop" which dropped a target it could plainly see.
	if (Band.bDropGoal || !Band.bActive)
	{
		// A dropped beacon goal ends the episode (as the goal-invalid branch above).
		if (bTeleportBeaconActive && Goal == EWoTGoalIndex::Intermediate)
		{
			bTeleportBeaconActive = false;
		}
		bDirectMoveActive = false;
		if (bPathMoveActive)
		{
			StopMovement();
			bPathMoveActive = false;
		}
		Brain->InvalidateGoal(Goal);
		return;
	}

	if (TickPatternMovement(DeltaSeconds, Band)) return;

	// A holding band inside its standoff: stop and face; the Offensive pulse
	// launches the attack (the acceptance band, NOT the 16uu primitive radius).
	// Melee bands hold at MeleeRange, the caster
	// band at the weave range — so a caster casts from range instead of chasing
	// to point-blank (PORT_NOTES).
	float Gap = 0.0f;
	if (Band.bHold &&
	    Brain->GetGoalDistance(Goal, Gap) && Gap <= Band.StandoffDistance)
	{
		bDirectMoveActive = false;
		if (bPathMoveActive)
		{
			StopMovement();
			bPathMoveActive = false;
		}
		// Zero residual velocity so the pawn actually stops at the standoff:
		// direct-move drives with AddMovementInput and flying has no braking, so
		// clearing the input alone lets it coast on through the band (and into
		// point-blank for a caster). This is the StopMovement the .uc hold/attack
		// states issue on arrival (e.g. Grunt InitialAcquisition :1793).
		Self->GetCharacterMovement()->StopMovementImmediately();
		if (const FWoTGoal& G = Brain->GetGoal(Goal); G.HasActor())
		{
			SetFocus(G.GoalActor.Get());
		}
		return;
	}

	// Otherwise close toward the navigation goal (an approach/route band, or a
	// holding band still outside its standoff).
	if (!bDirectMoveActive && !bPathMoveActive)
	{
		StartMoveLeg();
	}
	if (bDirectMoveActive)
	{
		TickDirectMove(DeltaSeconds);
	}
}

bool AWoTNPCController::SelectApproachingWaypoint(EWoTGoalIndex Goal,
	FVector& OutDestination)
{
	AWoTNPCCharacter* Self = Cast<AWoTNPCCharacter>(GetPawn());
	FVector GoalLocation;
	float GoalRadius = 0.0f, GoalHeight = 0.0f;
	if (!Self || !Brain
		|| !Brain->GetGoalCylinder(Goal, GoalLocation, GoalRadius, GoalHeight))
	{
		return false;
	}
	const FVector Origin = Self->GetActorLocation();
	FVector GoalVector = GoalLocation - Origin;
	const float Projected = GoalVector.Size2D();
	if (Projected <= KINDA_SMALL_NUMBER && FMath::Abs(GoalVector.Z) <= KINDA_SMALL_NUMBER)
	{
		return false;
	}
	const float OriginalDeltaZ = GoalVector.Z;
	float TraceDistance = 160.0f;
	if (OriginalDeltaZ >= Projected)
	{
		GoalVector.Z = Projected;
		TraceDistance = FMath::Sqrt(2.0f * FMath::Square(160.0f));
	}
	else if (Projected > KINDA_SMALL_NUMBER)
	{
		TraceDistance = 160.0f / FMath::Cos(
			FMath::Atan(OriginalDeltaZ / Projected));
	}
	const FVector Direction = GoalVector.GetSafeNormal();
	if (Direction.IsNearlyZero()) return false;
	const FVector OffsetAnchor = FVector::CrossProduct(Direction,
		FVector::UpVector);
	const float Offset = WoTRandom::FRandRange(128.0f, 192.0f);
#if WITH_AUTOMATION_TESTS
	ApproachingOffsetDistanceForTesting = Offset;
	ApproachingLateralOffsetForTesting = (OffsetAnchor * Offset).Size();
#endif
	float Radius = 0.0f, Height = 0.0f;
	Self->GetSimpleCollisionCylinder(Radius, Height);
	const float HeightAdjust = FMath::Abs(GoalVector.Z) + 2.0f * Height;
	const FVector Candidates[] = {
		Origin + Direction * TraceDistance,
		Origin + Direction * TraceDistance + OffsetAnchor * Offset,
		Origin + Direction * TraceDistance - OffsetAnchor * Offset};
	const FCollisionShape TraceExtent = FCollisionShape::MakeBox(
		FVector(Radius, Radius, 1.0f));
	for (const FVector& Candidate : Candidates)
	{
		FHitResult HorizontalHit;
		if (WoTVisBlockingSweep(GetWorld(), Origin, Candidate,
			FQuat::Identity, TraceExtent, Self, HorizontalHit))
		{
			continue;
		}
		FHitResult FloorHit;
		if (!WoTVisBlockingSweep(GetWorld(), Candidate,
			Candidate - FVector(0, 0, HeightAdjust), FQuat::Identity,
			TraceExtent, Self, FloorHit))
		{
			continue;
		}
		Brain->InitGoalWithVector(EWoTGoalIndex::CurrentWaypoint,
			FloorHit.Location);
		if (Brain->IsGoalNavigable(EWoTGoalIndex::CurrentWaypoint))
		{
			OutDestination = FloorHit.Location;
			return true;
		}
	}
	return false;
}

bool AWoTNPCController::SelectRouteWaypoint(EWoTGoalIndex Goal,
	FVector& OutDestination)
{
	AWoTNPCCharacter* Self = Cast<AWoTNPCCharacter>(GetPawn());
	FVector UltimateDestination;
	if (!Self || !Brain
		|| !Brain->GetNavigationGoalLocation(Goal, UltimateDestination))
	{
		return false;
	}

	// WaypointSelector.GetRawMovementDestination asks the UE1 navigation layer
	// for the next navigation goal, not the ultimate actor location. UE5 path
	// following normally hides that hop inside MoveToLocation, so recover the
	// first non-start path point for the selector/focus pair.
	if (UNavigationSystemV1* Nav = FNavigationSystem::GetCurrent<
		UNavigationSystemV1>(GetWorld()))
	{
		if (const ANavigationData* NavData = Nav->GetDefaultNavDataInstance())
		{
			FPathFindingQuery Query(Self, *NavData, Self->GetActorLocation(),
				UltimateDestination);
			Query.SetAllowPartialPaths(true);
			const FPathFindingResult Result = Nav->FindPathSync(Query);
			if (Result.IsSuccessful() && Result.Path.IsValid())
			{
				for (const FNavPathPoint& Point : Result.Path->GetPathPoints())
				{
					if (!Point.Location.Equals(Self->GetActorLocation(), 1.0f))
					{
						OutDestination = Point.Location;
						return true;
					}
				}
			}
		}
	}

	// Automation worlds have no navigation data. Preserve the selector's
	// next-hop contract there with one anticipated-travel-sized leg; production
	// worlds take the authored navigation route above.
	const FVector Delta = UltimateDestination - Self->GetActorLocation();
	OutDestination = Delta.Size() > 160.0f
		? Self->GetActorLocation() + Delta.GetSafeNormal() * 160.0f
		: UltimateDestination;
	return true;
}

bool AWoTNPCController::TickPatternMovement(float DeltaSeconds, const FWoTRangeBand& Band)
{
	AWoTNPCCharacter* Self = Cast<AWoTNPCCharacter>(GetPawn());
	const FWoTMovementBinding& Binding = Band.MovementBinding;
	if (!Self || !Brain || (Binding.Pattern.ClassIdentity.IsNone()
		&& Binding.WaypointSelector.IsNone()
		&& Binding.FocusSelector.IsNone()))
	{
		bMovementCycleActive = false;
		return false;
	}
	const EWoTGoalIndex Goal = Brain->CurrentGoalIndex;
	const FWoTGoal& G = Brain->GetGoal(Goal);
	const bool bSelectorOnly = Binding.Pattern.ClassIdentity.IsNone();
	float HoldGap = 0.0f;
	if (bSelectorOnly && Band.bHold
		&& Brain->GetGoalDistance(Goal, HoldGap)
		&& HoldGap <= Band.StandoffDistance)
	{
		bMovementCycleActive = false;
		bDirectMoveActive = false;
		if (bPathMoveActive)
		{
			StopMovement();
			bPathMoveActive = false;
		}
		return false;
	}
	auto ApplySelectedFocus = [this, Self, &Binding, &G]()
	{
		if (Binding.FocusSelector.ToString().EndsWith(
			TEXT("FocusSelectorOnWaypoint"))
			&& !MovementManager.Destination.Equals(Self->GetActorLocation()))
		{
			SetFocalPoint(MovementManager.Destination);
		}
		else if (Binding.FocusSelector.ToString().EndsWith(
			TEXT("FocusSelectorOnGreatestLOS")))
		{
			SetFocalPoint(FVector::ZeroVector);
		}
		else if (G.HasActor())
		{
			SetFocus(G.GoalActor.Get());
		}
		else
		{
			SetFocalPoint(G.GoalVector);
		}
	};
	if (bMovementCycleActive && (MovementCycleGoal != Goal || MovementCycleActor != G.GoalActor
		|| MovementCycleProximity != TrackingProximity))
	{
		bMovementCycleActive = false; bDirectMoveActive = false;
		if (bPathMoveActive) { StopMovement(); bPathMoveActive = false; }
	}
	if (!bMovementCycleActive)
	{
		MovementCycleGoal = Goal; MovementCycleActor = G.GoalActor; MovementCycleProximity = TrackingProximity;
		MovementCycleStart = GetWorld()->GetTimeSeconds();
		MovementManager.InitMovement(*Brain, *Self, Goal, &Binding, [&](FVector& Dest)
		{
			if (Binding.WaypointSelector.ToString().EndsWith(TEXT("WaypointSelectorHolding")))
			{ Dest = Self->GetActorLocation(); return true; }
			if (Binding.WaypointSelector.ToString().EndsWith(
				TEXT("WaypointSelectorApproaching")))
			{
				return SelectApproachingWaypoint(Goal, Dest);
			}
			return SelectRouteWaypoint(Goal, Dest);
		});
		bMovementCycleActive = true; bMovementCycleTurning = true;
		bMovementCycleMoving = false; bMovementCyclePostHandled = false;
		// Focus selection is a stub in shipped MovementPattern. Always fall through
		// to the handler focus selector; these bindings use goal or waypoint.
		ApplySelectedFocus();
		const bool bPreAttack = MovementManager.PreHint == TEXT("AttemptAttack")
			&& Brain->Notifiers[static_cast<int32>(EWoTNotifierSlot::Offensive)].bEnabled;
		HandleMovementHint(MovementManager.PreHint);
		if (bPreAttack)
		{
			// TrackingBase.OnOffensiveNotification enters AttemptAttack with
			// return label PostTrackGoal, even if no weapon can start an attack.
			// Selection already consumed the timestamp/cursor; this cycle's
			// turn, move, post hint and inactivity must never execute on return.
			bMovementCycleActive = false;
			bMovementCycleTurning = bMovementCycleMoving = false;
			bDirectMoveActive = false;
			if (bPathMoveActive) { StopMovement(); bPathMoveActive = false; }
			if (State == EWoTBrainState::Tracking) TransitionOnGoalPriority();
			return true;
		}
		if (State != EWoTBrainState::Tracking) return true;
	}
	// Attack continuations clear focus in EndState. Restore the selected focus
	// when resuming the same cycle, without selecting/stamping another element.
	ApplySelectedFocus();
	if (bMovementCycleTurning)
	{
		const FVector Focus = GetFocalPoint() - Self->GetActorLocation();
		if (!Focus.IsNearlyZero() && FMath::Abs(FMath::FindDeltaAngleDegrees(Self->GetActorRotation().Yaw, Focus.Rotation().Yaw)) > 1.0f) return true;
		bMovementCycleTurning = false;
	}
	if (!bMovementCyclePostHandled)
	{
		if (!bDirectMoveActive && !bPathMoveActive && !bMovementCycleMoving)
		{
			MoveDestination = MovementManager.Destination;
			const float Speed = NPCSpeedFraction * MovementManager.SuggestedSpeed;
			// Explicit zero is authored too (e.g. TrollocMed1.CloseIn).
			Self->GetCharacterMovement()->MaxWalkSpeed = FMath::Max(0.0f, Speed);
			Self->GetCharacterMovement()->MaxFlySpeed = FMath::Max(0.0f, Speed);
			MoveTimer = 1 + 1.3f * FVector::Dist(Self->GetActorLocation(), MoveDestination) / FMath::Max(1.0f, Speed);
			bDirectMoveActive = true;
			bMovementCycleMoving = true;
			const bool bSelectedWaypoint = !Binding.WaypointSelector.IsNone();
			if (!MovementManager.bAppliedPattern && !bSelectedWaypoint
				&& !Brain->IsGoalReachable(Goal))
			{
				bDirectMoveActive = false;
				const auto Result = MoveToLocation(MoveDestination, 16.0f, true, true);
				bPathMoveActive = Result == EPathFollowingRequestResult::RequestSuccessful;
				if (!bPathMoveActive) Brain->NotifyMoveResult(Result == EPathFollowingRequestResult::AlreadyAtGoal);
			}
		}
		if (bDirectMoveActive) TickDirectMove(DeltaSeconds);
		if (bDirectMoveActive || bPathMoveActive) return true;
		Self->GetCharacterMovement()->StopMovementImmediately();
		bMovementCyclePostHandled = true;
		HandleMovementHint(MovementManager.PostHint);
		if (State != EWoTBrainState::Tracking) return true;
	}
	// Acquired.PerformingSleepInactivity: only the remainder of MinTrackingDuration.
	if (GetWorld()->GetTimeSeconds() - MovementCycleStart >= 0.01f)
	{
		bMovementCycleActive = false;
		TransitionOnGoalPriority(); // PostTrackGoal polling, including Snipe's new Intermediate
	}
	return true;
}

void AWoTNPCController::HandleMovementHint(FName Hint)
{
	if (Hint == TEXT("AttemptAttack"))
	{
		// DurationNotifier.Notify checks enabled; it does not restart the clock.
		if (Brain && Brain->Notifiers[static_cast<int32>(EWoTNotifierSlot::Offensive)].bEnabled)
			Brain->OnNotifierPulse.Broadcast(EWoTNotifierSlot::Offensive);
	}
	else if (Hint == TEXT("ForceStationary")) SetForcedStationary(true);
	else if (Hint == TEXT("UnForceStationary")) SetForcedStationary(false);
	else if (Hint == TEXT("Snipe") && Brain && GetPawn())
	{
		const AWoTNPCCharacter* NPC = Cast<AWoTNPCCharacter>(GetPawn());
		AActor* Target = Brain->GetGoal(EWoTGoalIndex::Threat).GoalActor.Get();
		if (!NPC || NPC->CreatureRowName != TEXT("Myrddraal") || !Target) return;
		// Myrddraal.PerformSnipe -> WaypointSelectorSniping: nearest flagged
		// WotPathNode within2250 with an actor trace from node +60 hitting threat.
		AWoTPathNode* Best = nullptr; float BestDistance = FMath::Square(2250.0f);
		for (TActorIterator<AWoTPathNode> It(GetWorld()); It; ++It)
		{
			const float Distance = FVector::DistSquared(NPC->GetActorLocation(), It->GetActorLocation());
			if (!It->bSnipe || Distance > BestDistance) continue;
			FHitResult Hit;
			if (WoTRetailActorTrace(GetWorld(), It->GetActorLocation() + FVector(0, 0, 60), Target->GetActorLocation(), *It, Hit)
				&& Hit.GetActor() == Target) { Best = *It; BestDistance = Distance; }
		}
		if (Best) Brain->InitGoalWithActor(EWoTGoalIndex::Intermediate, Best);
	}
	// Unknown hints, including Questioner1.BackOff, are deliberately inert.
}

void AWoTNPCController::StartMoveLeg()
{
	AWoTNPCCharacter* Self = Cast<AWoTNPCCharacter>(GetPawn());
	if (!Brain || !Self)
	{
		return;
	}
	const EWoTGoalIndex Goal = Brain->CurrentGoalIndex;
	FVector Dest;
	if (!Brain->GetNavigationGoalLocation(Goal, Dest))
	{
		Brain->NotifyMoveResult(false);
		return;
	}

	// Chase speed: 0.8 x the goal's suggested speed (the NPC DesiredSpeed
	// fraction).
	const float Suggested = Brain->GetGoal(Goal).SuggestedSpeed;
	if (Suggested > 0.0f)
	{
		Self->GetCharacterMovement()->MaxWalkSpeed = NPCSpeedFraction * Suggested;
		Self->GetCharacterMovement()->MaxFlySpeed = NPCSpeedFraction * Suggested;
	}
	if (const FWoTGoal& G = Brain->GetGoal(Goal); G.HasActor())
	{
		SetFocus(G.GoalActor.Get());
	}
	else
	{
		SetFocalPoint(Dest);
	}
	MoveDestination = Dest;

	if (Brain->IsGoalReachable(Goal))
	{
		// Straight at it: the moveToward primitive. The tracking loop drives
		// StrafeTo-style VECTOR moves (the destination is a computed contact
		// point, re-read per tick), so the give-up budget is always the full
		// 1.0 + 1.3·dist/speed — the flat 1.2s re-assess cap belongs only to
		// the MoveToward-at-a-Pawn latent, which Tracking never uses
		// (LegendPawn.uc:2237-2239).
		const float Speed = FMath::Max(1.0f, Self->GetCharacterMovement()->MaxWalkSpeed);
		const float Dist = FVector::Dist(Self->GetActorLocation(), Dest);
		MoveTimer = 1.0f + 1.3f * Dist / Speed;
		bDirectMoveActive = true;
	}
	else
	{
		// Routed: hand the corridor to path following; acceptance is the goal
		// band's concern (we stop the move when the band check trips), so use
		// a tight radius here.
		const EPathFollowingRequestResult::Type Result =
			MoveToLocation(Dest, /*AcceptanceRadius*/ 16.0f,
			               /*bStopOnOverlap*/ true, /*bUsePathfinding*/ true);
		if (Result == EPathFollowingRequestResult::RequestSuccessful)
		{
			bPathMoveActive = true;
		}
		else if (Result == EPathFollowingRequestResult::AlreadyAtGoal)
		{
			Brain->NotifyMoveResult(true);
		}
		else
		{
			Brain->NotifyMoveResult(false);
		}
	}
}

FVector AWoTNPCController::GetActiveLatentDestination() const
{
	if (bPathMoveActive)
	{
		if (const UPathFollowingComponent* Path = GetPathFollowingComponent())
		{
			const FVector SegmentDestination = Path->GetCurrentTargetLocation();
			if (!SegmentDestination.ContainsNaN())
			{
				return SegmentDestination;
			}
		}
	}
	return MoveDestination;
}

void AWoTNPCController::TickDirectMove(float DeltaSeconds)
{
	AWoTNPCCharacter* Self = Cast<AWoTNPCCharacter>(GetPawn());
	if (!Brain || !Self)
	{
		bDirectMoveActive = false;
		return;
	}
	// Actor goals re-read the target's position every tick (a chase follows
	// the runner, not a stale snapshot).
	const FWoTGoal& G = Brain->GetGoal(Brain->CurrentGoalIndex);
	if (G.HasActor() && !bMovementCycleActive)
	{
		FVector Fresh;
		if (Brain->GetNavigationGoalLocation(Brain->CurrentGoalIndex, Fresh))
		{
			MoveDestination = Fresh;
		}
	}

	const FVector Delta = MoveDestination - Self->GetActorLocation();
	const float ZTolerance = FMath::Max(48.0f, Self->GetSimpleCollisionHalfHeight());
	if (Delta.SizeSquared2D() < ArrivalRadiusXYSquared && FMath::Abs(Delta.Z) < ZTolerance)
	{
		bDirectMoveActive = false;
		Brain->NotifyMoveResult(true);
		return;
	}
	MoveTimer -= DeltaSeconds;
	if (MoveTimer < 0.0f)
	{
		// Give-up: a strike (the nugget model). The leg ends looking like a
		// normal completion; the goal quietly degrades.
		bDirectMoveActive = false;
		Brain->NotifyMoveResult(false);
		return;
	}
	Self->AddMovementInput(Delta.GetSafeNormal2D());
}

void AWoTNPCController::OnMoveCompleted(FAIRequestID RequestID, const FPathFollowingResult& Result)
{
	Super::OnMoveCompleted(RequestID, Result);
	if (bPathMoveActive && Brain)
	{
		bPathMoveActive = false;
		// Success within acceptance resets strikes; Blocked/Invalid/OffPath
		// count as one strike.
		Brain->NotifyMoveResult(Result.IsSuccess());
	}
}

// ---------------------------------------------------------------- melee

void AWoTNPCController::MatchGoalAcceleration()
{
	AWoTNPCCharacter* Self = Cast<AWoTNPCCharacter>(GetPawn());
	if (!Self || !Brain || bStationaryMeleeSwing
		|| !Brain->GetGoal(Brain->CurrentGoalIndex).HasActor())
	{
		return;
	}
	AActor* GoalActor = Brain->GetGoal(Brain->CurrentGoalIndex).GoalActor.Get();
	if (!GoalActor)
	{
		return;
	}
	// AddMovementInput is the CharacterMovement equivalent of assigning
	// Acceleration=AccelRate*Normal(Goal-Location); it keeps the full live goal
	// direction and is re-issued on every zero-duration notifier pulse.
	Self->AddMovementInput((GoalActor->GetActorLocation() - Self->GetActorLocation()).GetSafeNormal());
}

void AWoTNPCController::StartMeleeSwing()
{
	AWoTNPCCharacter* Self = Cast<AWoTNPCCharacter>(GetPawn());
	if (!Self || Self->MeleeAttacks.Num() == 0)
	{
		EnterState(EWoTBrainState::Tracking);
		return;
	}
	PendingSwingAttacks.Reset();
	PendingSwingTimes.Reset();
	PendingSlashTimes.Reset();
	PendingSlashSlots.Reset();
	SwingElapsed = 0.0f;
#if WITH_AUTOMATION_TESTS
	MeleeNotifyAttemptsForTest = 0;
	FirstMeleeNotifyFractionForTest = -1.0f;
#endif
	bGrabSwing = false;
	bStationaryMeleeSwing = false;
	bGrabDrawInApplied = false;
	bGrabReleased = false;
	GrabVictim = nullptr;
	bBATHeadTossSwing = false;
	bBATHeadTossHit = false;
	bBATHeadTossApplied = false;
	BATHeadTossVictim = nullptr;

	// PlayStateAnimation (Grunt.uc:1484-1496): pick the swing sequence from the
	// creature's anim table (its melee slot, weighted odds), stamp it on the pawn,
	// and schedule every MeleeAttacks notify that rides that sequence at its
	// authored fraction of the real duration; the swing ends when the sequence does
	// (the latent FinishAnim). SwingDuration falls back to the 1s stand-in whenever
	// the table/sequence isn't bound (bare pawns, headless without content), which
	// preserves the old fraction-as-seconds cadence exactly.
	float SwingDuration = 0.0f;
	float SwingTweenTime = 0.0f;
	// The Minion's PerformAttack picks the standing/tentacle/running slot by goal
	// speed + distance (Minion.uc:287-327); every other creature uses its effective
	// melee slot. SelectSwingSlot also sets bGrabSwing when the tentacle grab is chosen.
	const FName EffectiveMeleeSlot = SelectSwingSlot(Self, bGrabSwing);
	LastSwingSlot = EffectiveMeleeSlot;
	if (const FWoTAnimTableEntry* Pick = Self->StampSlotAnim(EffectiveMeleeSlot))
	{
		SwingTweenTime = WoTAnimTable::ResolveTweenTime(Pick);
		SwingDuration = Self->SequenceDurationSeconds(
		    Pick->AnimName, WoTAnimTable::ResolveRate(Pick));
		for (int32 i = 0; i < Self->MeleeAttacks.Num(); ++i)
		{
			if (Self->MeleeAttacks[i].AnimName == Pick->AnimName)
			{
				PendingSwingAttacks.Add(i);
			}
		}
	}
	if (PendingSwingAttacks.Num() == 0)
	{
		// No table / no rows riding the picked sequence: the legacy random pick over
		// the attack set, on the 1s stand-in clock (don't time a random row's
		// fraction against some other sequence's duration).
		PendingSwingAttacks.Add(WoTRandom::RandHelper(Self->MeleeAttacks.Num()));
		SwingDuration = 0.0f;
	}
	// Warder.PlayStateAnimation roots only SkewRip and DbleSwipe. Like the
	// Minion grab below, InterruptMovement runs before disabling DNI_Movement.
	const FName SwingSequence = Self->GetReplicatedAnimName();
	if (Self->CreatureRowName == TEXT("Warder")
		&& (SwingSequence == TEXT("SkewRip") || SwingSequence == TEXT("DbleSwipe")))
	{
		bStationaryMeleeSwing = true;
		StopMovement();
		bDirectMoveActive = false;
		if (bPathMoveActive)
		{
			StopMovement();
			bPathMoveActive = false;
		}
		Self->GetCharacterMovement()->StopMovementImmediately();
		if (Brain)
		{
			Brain->SetNotifierEnabled(EWoTNotifierSlot::Movement, false);
		}
	}
	// The swing whoosh fires at the sequence's authored PlaySlashSound fraction
	// (Trolloc AttackRun @0.65 etc. — sound 5d; the mesh-notify timing, like the
	// damage rows). Sequence-less fallbacks whoosh at the swing start.
	if (SwingDuration > 0.0f)
	{
		if (const UWoTVertexAnimComponent* Anim =
		        Cast<UWoTVertexAnimComponent>(Self->GetMesh()))
		{
			if (const FWoTAnimSequence* Seq =
			        Anim->FindSequence(Self->GetReplicatedAnimName()))
			{
				for (const FWoTAnimNotify& N : Seq->Notifies)
				{
					FName SoundSlot;
					if (N.Function == TEXT("PlaySlashSound")) SoundSlot=TEXT("WeaponMeleeAttack");
					else if (N.Function == TEXT("PlayMeleeHitSound"))
					{
						SoundSlot = TEXT("WeaponMeleeHitEnemy");
						for (const int32 AttackIndex : PendingSwingAttacks)
						{
							if (Self->MeleeAttacks.IsValidIndex(AttackIndex))
							{
								SoundSlot = Self->MeleeAttacks[AttackIndex].HitSoundSlot;
								break;
							}
						}
					}
					else if (N.Function == TEXT("AttackBiteSound")) SoundSlot=TEXT("AttackBite");
					else if (N.Function == TEXT("AttackClawSound")) SoundSlot=TEXT("AttackClaw");
					else if (N.Function == TEXT("TentacleWhipAttackSound")
						|| N.Function == TEXT("TentacleGrabAttackSound")) SoundSlot=TEXT("AttackTentacle");
					if (!SoundSlot.IsNone())
					{
						PendingSlashTimes.Add(SwingTweenTime + N.Fraction * SwingDuration);
						PendingSlashSlots.Add(SoundSlot);
					}
				}
			}
		}
	}
	if (PendingSlashTimes.Num() == 0)
	{
		FName Slot=TEXT("WeaponMeleeAttack");
		if(Self->CreatureRowName==TEXT("Minion"))
		{
			const FName Seq=Self->GetReplicatedAnimName();
			Slot=Seq.ToString().Contains(TEXT("Bite"))?FName(TEXT("AttackBite")):
				Seq.ToString().Contains(TEXT("Tent"))?FName(TEXT("AttackTentacle")):FName(TEXT("AttackClaw"));
		}
		Self->PlayVoiceSlot(Slot);
	}
	if (SwingDuration <= 0.0f)
	{
		SwingDuration = 1.0f;
	}
	SwingEndTime = SwingTweenTime + SwingDuration;
	for (const int32 AttackIndex : PendingSwingAttacks)
	{
		PendingSwingTimes.Add(
		    SwingTweenTime + Self->MeleeAttacks[AttackIndex].NotifyFraction * SwingDuration);
		if (Self->CreatureRowName == TEXT("BATrolloc")
			&& Self->MeleeAttacks[AttackIndex].DamageTypeName == TEXT("Tossed"))
		{
			bBATHeadTossSwing = true;
		}
	}
	BATHeadTossTime = SwingTweenTime + 0.45f * SwingDuration;

	if (bGrabSwing && Brain)
	{
		// PerformTentacleGrabAttack (Minion.uc:405-410): interrupt movement + disable the
		// Movement notifier so the Minion doesn't drift while the tentacle reels the
		// victim; the @0.50 release notify (TentacleGrabAttackDamage2) restores it. The
		// draw-in itself rides the @0.40 damage notify, handled in TickMelee.
		StopMovement();
		bDirectMoveActive = false;
		bPathMoveActive = false;
		Brain->SetNotifierEnabled(EWoTNotifierSlot::Movement, false);
		GrabReleaseTime = SwingTweenTime + 0.50f * SwingDuration;
		if (const FWoTGoal& G = Brain->GetGoal(Brain->CurrentGoalIndex); G.HasActor())
		{
			GrabVictim = G.GoalActor;
		}
	}

	if (Brain)
	{
		if (const FWoTGoal& G = Brain->GetGoal(Brain->CurrentGoalIndex); G.HasActor())
		{
			SetFocus(G.GoalActor.Get());
		}
	}
}

void AWoTNPCController::TickMelee(float DeltaSeconds)
{
	const AWoTNPCCharacter* Self = Cast<AWoTNPCCharacter>(GetPawn());
	if (!Self || !Brain)
	{
		EnterState(EWoTBrainState::Tracking);
		return;
	}
	SwingElapsed += DeltaSeconds;

	// A hitch may cross several mesh notifies in one Tick. Merge slash sounds and
	// damage callbacks into one chronological stream, independent of decoder row
	// order, so a Warder opener can abort every later notify in the sequence.
	while (true)
	{
		int32 EarliestDue = INDEX_NONE;
		float EarliestTime = TNumericLimits<float>::Max();
		for (int32 Index = 0; Index < PendingSwingAttacks.Num(); ++Index)
		{
			if (PendingSwingTimes.IsValidIndex(Index)
				&& SwingElapsed >= PendingSwingTimes[Index]
				&& PendingSwingTimes[Index] < EarliestTime)
			{
				EarliestDue = Index;
				EarliestTime = PendingSwingTimes[Index];
			}
		}
		int32 EarliestSlash = INDEX_NONE;
		float EarliestSlashTime = TNumericLimits<float>::Max();
		for (int32 Index = 0; Index < PendingSlashTimes.Num(); ++Index)
		{
			if (SwingElapsed >= PendingSlashTimes[Index]
				&& PendingSlashTimes[Index] < EarliestSlashTime)
			{
				EarliestSlash = Index;
				EarliestSlashTime = PendingSlashTimes[Index];
			}
		}
		if (EarliestSlash != INDEX_NONE
			&& (EarliestDue == INDEX_NONE || EarliestSlashTime < EarliestTime))
		{
			const FName Slot = PendingSlashSlots.IsValidIndex(EarliestSlash)
				? PendingSlashSlots[EarliestSlash] : FName(TEXT("WeaponMeleeAttack"));
			PendingSlashTimes.RemoveAt(EarliestSlash);
			if (PendingSlashSlots.IsValidIndex(EarliestSlash))
			{
				PendingSlashSlots.RemoveAt(EarliestSlash);
			}
			if (AWoTNPCCharacter* Voice = Cast<AWoTNPCCharacter>(GetPawn()))
			{
				Voice->PlayVoiceSlot(Slot);
			}
			continue;
		}
		if (EarliestDue == INDEX_NONE)
		{
			break;
		}
		const int32 AttackIndex = PendingSwingAttacks[EarliestDue];
		PendingSwingAttacks.RemoveAt(EarliestDue);
		PendingSwingTimes.RemoveAt(EarliestDue);
		if (Self->MeleeAttacks.IsValidIndex(AttackIndex))
		{
			const FWoTMeleeAttack Attack = Self->MeleeAttacks[AttackIndex];
#if WITH_AUTOMATION_TESTS
			if (FirstMeleeNotifyFractionForTest < 0.0f)
			{
				FirstMeleeNotifyFractionForTest = Attack.NotifyFraction;
			}
			++MeleeNotifyAttemptsForTest;
#endif
			const bool bLanded = MeleeDamageGoal(Attack);
			if (!bLanded && Attack.bAbortSwingOnMiss)
			{
				// Warder.AttackSkewDamageTarget / Attack3DamageTarget set
				// bAnimFinished on a miss. Leaving PerformMelee cancels every later
				// notify and returns the rooted Warder to its tracking decision.
				EnterState(EWoTBrainState::Tracking);
				return;
			}
			if (bBATHeadTossSwing && Attack.DamageTypeName == TEXT("Tossed")
				&& bLanded)
			{
				bBATHeadTossHit = true;
				if (const FWoTGoal& Goal = Brain->GetGoal(Brain->CurrentGoalIndex);
					Goal.HasActor())
				{
					BATHeadTossVictim = Cast<AWoTCharacter>(Goal.GoalActor.Get());
				}
			}
			// TentacleGrabAttackDamage1 (Minion.uc:423-438): the draw-in rides the grab's
			// @0.40 damage notify and only when the damage landed (retail gates the reel
			// on AttackDamageTarget returning true). The grab swing schedules exactly the
			// ATTACKTENT row, so any landed hit during it is the grab hit.
			if (bGrabSwing && bLanded && !bGrabDrawInApplied)
			{
				bGrabDrawInApplied = true;
				if (UWoTMinionStealthBehavior* Minion =
				        Cast<UWoTMinionStealthBehavior>(Behavior))
				{
					float Gap = 0.0f;
					Brain->GetGoalDistance(Brain->CurrentGoalIndex, Gap);
					Minion->ApplyGrabDrawIn(GrabVictim.Get(), Gap);
				}
			}
			// A landed swing's OnMeleeHit may switch us out of the swing (the Myrddraal
			// blink starts PerformTeleportation, Myrddraal.uc:557). EndState(PerformMelee)
			// has already reset the swing arrays, so stop touching them this tick.
			if (State != EWoTBrainState::PerformMelee)
			{
				return;
			}
		}
	}

	if (bBATHeadTossSwing && !bBATHeadTossApplied
		&& SwingElapsed >= BATHeadTossTime)
	{
		bBATHeadTossApplied = true;
		AWoTCharacter* Victim = BATHeadTossVictim.Get();
		if (bBATHeadTossHit && Victim)
		{
			const UCharacterMovementComponent* Move = Victim->GetCharacterMovement();
			const float Mass = Move ? FMath::Max(Move->Mass, 1.0f) : 100.0f;
			const FVector Dir =
				(Victim->GetActorLocation() - Self->GetActorLocation()).GetSafeNormal();
			const FVector Delta = 200000.0f * (Dir + FVector::UpVector) / Mass;
			FRotator TossRotation = Victim->GetActorRotation();
			// BATrolloc.uc:140 assigns HeadTossPitch=4096 absolutely.
			TossRotation.Pitch = 22.5f;
			ApplyCreatureImpulse(Victim, Delta, &TossRotation,
				0.2f, 2000.0f, -10.0f);
		}
	}

	// The @0.50 release notify (TentacleGrabAttackDamage2, Minion.uc:443-464): no damage;
	// a victim that has reached walking/swimming leaves the constrained state.
	if (bGrabSwing && !bGrabReleased && SwingElapsed >= GrabReleaseTime)
	{
		bGrabReleased = true;
		if (UWoTMinionStealthBehavior* Minion = Cast<UWoTMinionStealthBehavior>(Behavior))
		{
			Minion->ReleaseGrabVictim(GrabVictim.Get());
		}
	}

	if (SwingElapsed >= SwingEndTime)
	{
		// PostPerform: the grab re-enables the Movement notifier it disabled for the
		// episode (Minion.uc's DNI_Movement restore), then back to the tracking loop
		// (whose notifier set the Offensive pulse paces the next attempt from).
		if (bGrabSwing && Brain)
		{
			Brain->SetNotifierEnabled(EWoTNotifierSlot::Movement, true);
		}
		bGrabSwing = false;
		EnterState(EWoTBrainState::Tracking);
	}
}

FName AWoTNPCController::SelectSwingSlot(AWoTNPCCharacter* Self, bool& bOutGrabSwing)
{
	bOutGrabSwing = false;
	if (!Self)
	{
		return FName(TEXT("MeleeAttack"));
	}
	// The Minion attack selector (Minion.uc PerformAttack :287-327), driven off the
	// stealth behavior (the row that owns the tentacle band). Every other creature keeps
	// its effective melee slot (weapon-in-hand vs body — GetEffectiveMeleeAnimSlot).
	UWoTMinionStealthBehavior* Minion = Cast<UWoTMinionStealthBehavior>(Behavior);
	if (!Minion || !Brain)
	{
		return Self->GetEffectiveMeleeAnimSlot();
	}
	// PerformAttack clears bStealthAttacks (Minion.uc:291) — committing to an attack ends
	// the stealth freeze regardless of which swing follows.
	Minion->ClearStealthAttacks();

	const FWoTGoal& G = Brain->GetGoal(Brain->CurrentGoalIndex);
	// A standing target (goal speed <= 1) gets the standing/tentacle set; a moving target
	// keeps the running claws/whip (the generic MeleeAttack slot, already ported).
	const bool bStandingTarget = G.HasActor() && G.GoalActor->GetVelocity().Size() <= 1.0f;
	if (!bStandingTarget)
	{
		return Self->GetEffectiveMeleeAnimSlot();
	}
	float Gap = 0.0f;
	const bool bHaveGap = Brain->GetGoalDistance(Brain->CurrentGoalIndex, Gap);
	if (bHaveGap && Minion->InUseTentacleDistance(Gap))
	{
		bOutGrabSwing = true;
		return FName(TEXT("TentacleGrab"));   // PerformTentacleGrabAttack
	}
	return FName(TEXT("MeleeStandingAttack"));   // PerformStandingAttack (bite/claws)
}

// ---------------------------------------------------------------- defensive dodge

bool AWoTNPCController::TryBeginDodge()
{
	AWoTNPCCharacter* Self = Cast<AWoTNPCCharacter>(GetPawn());
	if (!Self || !Self->bDefensiveDodge || !Brain)
	{
		return false;
	}
	// Shield-first (ShieldUser.PendingOffenderCollision, uc:137-151): a ShieldUser only
	// falls to the base dodge when its shield is GONE — with the shield in hand it raises
	// it (the ShieldDeflect behavior) instead. A shield present here means no dodge.
	// A shield user's SINGLE detector also carries its class rejections into the base
	// dodge (retail runs the fallback off the same DefensiveDetectorSoldier/Questioner):
	// both reject WotWeaponProjectile (arrows are neither deflected NOR dodged), the
	// Soldier also rejects seekers (DefensiveDetectorSoldier.uc:10-18 / Questioner :10-17).
	bool bRejectWeaponProjectiles = false;
	bool bRejectSeekers = false;
	if (const UWoTShieldDeflectBehavior* Shield = Cast<UWoTShieldDeflectBehavior>(Behavior))
	{
		if (Shield->HasShieldAvailable())
		{
			return false;
		}
		bRejectWeaponProjectiles = true;
		bRejectSeekers = Shield->bRejectSeekers;
	}
	// The active detector poll (CheckForOffending): the WotProjectile family inside the
	// collection radius + FOV cone, moving toward us, arriving in the response window —
	// minus the shield-user rejections above (Warder/Trolloc/Minion detectors reject
	// nothing beyond the base Fragment no-op).
	AWoTProjectile* Offender = WoTDefensiveScan::FindIncomingOffender(
		Self, Self->DodgeCollectionRadius, Self->PeripheralVision,
		Self->DodgeResponseMinTime, Self->DodgeResponseMaxTime,
		bRejectWeaponProjectiles, bRejectSeekers);
	if (!Offender)
	{
		return false;
	}
	// RespondedToActor runs with bRecollect=TRUE at the dodge site (LegendPawn.uc:1376),
	// so bRejectResponded = !bRecollect = FALSE (DefensiveDetector.uc:186): retail
	// EXPLICITLY allows re-dodging the same offender on the next poll — a homing seeker
	// that re-acquires after the sidestep gets dodged again every cycle. The only
	// double-response guard retail needs is the Defensive notifier being off through
	// the dodge+landing episode, which this port preserves (Dodging state). A permanent
	// last-offender skip here would be a seeker-eating divergence.

	// SelectDodgeWaypointGoal → FindDodgeDestination (MovementManager.uc:363-372): if no
	// clear sidestep exists, bHandleDefensive stays false and no dodge happens.
	bool bLeftFirst = WoTRandom::FRand() < 0.5f;
#if WITH_AUTOMATION_TESTS
	if (DodgeForcedSidePick == 0) { bLeftFirst = true; }
	else if (DodgeForcedSidePick == 1) { bLeftFirst = false; }
#endif
	FVector Destination;
	if (!FindDodgeDestination(Destination, Offender, bLeftFirst))
	{
		return false;
	}

	// PendingOffenderCollision (LegendPawn.uc:1371-1377): store the response + interrupt.
	DodgeOffender = Offender;
	DodgeDestination = Destination;
	bMovementCycleActive = false;
	MovementManager.PreHint = MovementManager.PostHint = NAME_None;
	// PerformDefensiveDodge clears the Minion's stealth (Minion.uc:188); harmless on the
	// Warder/Trolloc (no stealth behavior).
	if (UWoTMinionStealthBehavior* Minion = Cast<UWoTMinionStealthBehavior>(Behavior))
	{
		Minion->ClearStealthAttacks();
	}
	EnterState(EWoTBrainState::Dodging);
	return true;
}

bool AWoTNPCController::FindDodgeDestination(FVector& OutDestination, AActor* ActorToDodge,
	bool bLeftFirst) const
{
	if (!ActorToDodge)
	{
		return false;
	}
	const APawn* Self = GetPawn();
	if (!Self)
	{
		return false;
	}
	// HorizontalDistance = Inquirer.CollisionRadius + ActorToDodge.CollisionRadius (:445).
	const float HorizontalDistance =
		Self->GetSimpleCollisionRadius() + ActorToDodge->GetSimpleCollisionRadius();

	// DodgeDirection = the XY-perpendicular of Normal(Offender.Velocity) (:456-460).
	// Read the projectile's movement velocity (the actor's ComponentVelocity lags a tick).
	FVector OffenderVelocity = ActorToDodge->GetVelocity();
	if (const AWoTProjectile* Proj = Cast<AWoTProjectile>(ActorToDodge);
	    Proj && Proj->ProjectileMovement)
	{
		OffenderVelocity = Proj->ProjectileMovement->Velocity;
	}
	const FVector PursuerDirection = OffenderVelocity.GetSafeNormal();
	FVector DodgeDirection(-PursuerDirection.Y, PursuerDirection.X, 0.0f);
	DodgeDirection = DodgeDirection.GetSafeNormal();

	// MovementManager.uc:464-484 tests the requested side first, then its opposite.
	const FVector OppositeDirection = DodgeDirection * -1.0f;
	const FVector OrderedDirections[2] = {
		bLeftFirst ? DodgeDirection : OppositeDirection,
		bLeftFirst ? OppositeDirection : DodgeDirection,
	};
	bool bUnobstructed = false;
	for (const FVector& CandidateDirection : OrderedDirections)
	{
		DodgeDirection = CandidateDirection;
		if (IsDodgeDestinationUnobstructed(DodgeDirection, HorizontalDistance))
		{
			bUnobstructed = true;
			break;
		}
	}

	if (bUnobstructed)
	{
		// Destination = Location + DodgeDirection * HorizontalDistance (:489).
		OutDestination = Self->GetActorLocation() + DodgeDirection * HorizontalDistance;
	}
	return bUnobstructed;
}

bool AWoTNPCController::IsDodgeDestinationUnobstructed(const FVector& ShiftDir,
	float ShiftDistance) const
{
#if WITH_AUTOMATION_TESTS
	// Headless worlds have no BSP to trace; a spec marks the ShiftDirs it wants blocked.
	for (const FVector& Blocked : DodgeBlockedDirsForTest)
	{
		if (FVector::DotProduct(ShiftDir.GetSafeNormal(), Blocked.GetSafeNormal()) > 0.99f)
		{
			return false;
		}
	}
#endif
	const APawn* Self = GetPawn();
	const UWorld* World = GetWorld();
	if (!Self || !World)
	{
		return true;
	}
	// A box sweep of the pawn's cylinder from its location ShiftDistance along ShiftDir
	// (IsActorDestinationUnobstructed, MovementManager.uc:515-525). Retail's Trace runs
	// with bTraceActors=false — GEOMETRY ONLY; actors (pawns, projectiles) never block a
	// dodge. The accepted-hit sweep queries both world object types so movers block,
	// then retraces past non-VisBlocking actor capsules. Only accepted level/mover
	// geometry returned by that filtering can obstruct the destination.
	const float Radius = Self->GetSimpleCollisionRadius();
	const float HalfHeight = Self->GetSimpleCollisionHalfHeight();
	const FVector Start = Self->GetActorLocation();
	const FVector End = Start + ShiftDir.GetSafeNormal() * ShiftDistance;
	FHitResult Hit;
	// MovementManager.uc:523 Trace(..., false, Extent): level and movers only.
	// Accept-and-retrace preserves the extent while skipping actor capsules.
	const bool bHit = WoTVisBlockingSweep(const_cast<UWorld*>(World), Start, End,
		FQuat::Identity, FCollisionShape::MakeBox(FVector(Radius, Radius, HalfHeight)),
		Self, Hit);
	return !bHit || FVector::Dist(Hit.Location, Start) >= ShiftDistance;
}

void AWoTNPCController::StartDodge()
{
	AWoTNPCCharacter* Self = Cast<AWoTNPCCharacter>(GetPawn());
	if (!Self)
	{
		EnterState(EWoTBrainState::Tracking);
		return;
	}
	// bAnimFinished/InterruptMovement (LegendPawn.uc:1374-1375): drop the tracking leg.
	StopMovement();
	bDirectMoveActive = false;
	bPathMoveActive = false;

	UCharacterMovementComponent* Move = Self->GetCharacterMovement();
	const FVector Dir = (DodgeDestination - Self->GetActorLocation()).GetSafeNormal();

	// Grunt.PerformDefensiveDodge (Grunt.uc:211-224): DodgeDirection unrotated into
	// actor-local space; local Y < 0 → DodgeLeft, Y > 0 → DodgeRight.
	const FVector LocalDir = Self->GetActorRotation().UnrotateVector(Dir);
	LastDodgeSlot = NAME_None;
	if (LocalDir.Y < 0.0f)
	{
		LastDodgeSlot = FName(TEXT("DodgeLeft"));
	}
	else if (LocalDir.Y > 0.0f)
	{
		LastDodgeSlot = FName(TEXT("DodgeRight"));
	}
	if (!LastDodgeSlot.IsNone())
	{
		Self->StampSlotAnim(LastDodgeSlot);
	}

	// TrackingBase.PerformDefensiveDodge (LegendPawn.uc:2121-2132): only if not already
	// falling, set the launch profile and enter falling physics.
	if (Move && Move->MovementMode != MOVE_Falling)
	{
		FVector Launch = Dir * Self->DodgeVelocityFactor;
		Launch.Z = Self->DodgeVelocityAltitude;
		Move->Velocity = Launch;
		Move->SetMovementMode(MOVE_Falling);
	}
}

void AWoTNPCController::CompleteDodge()
{
	// PerformDodge tail (LegendPawn.uc:2280-2282): zero the residual vertical speed, then
	// resume Tracking — whose notifier set re-enables the Defensive slot.
	if (AWoTNPCCharacter* Self = Cast<AWoTNPCCharacter>(GetPawn()))
	{
		if (UCharacterMovementComponent* Move = Self->GetCharacterMovement())
		{
			Move->Velocity.Z = 0.0f;
		}
	}
	DodgeOffender = nullptr;
	EnterState(EWoTBrainState::Tracking);
}

// ---------------------------------------------------- Myrddraal teleport episode

bool AWoTNPCController::BeginTeleportEpisode()
{
	// PerformTeleportation (Myrddraal.uc:216): find a shadow-node destination, set it as
	// the Intermediate-goal beacon, and let Tracking walk there (phase 1). No-op unless
	// the pawn is a Myrddraal, a destination was found, and no episode is already up.
	if (IsTeleportEpisodeActive() || !Brain)
	{
		return false;
	}
	UWoTMyrddraalTeleportBehavior* Teleport = Cast<UWoTMyrddraalTeleportBehavior>(Behavior);
	if (!Teleport)
	{
		return false;
	}
	FVector Destination;
	if (!Teleport->FindTeleportPosition(Destination))
	{
		return false;   // FindTeleportPosition false → no blink (uc:220)
	}
	Teleport->SetPendingDestination(Destination);
	// InitGoalWithObject(GI_Intermediate, MyrddraalTeleporter) (uc:223): the beacon is
	// the Intermediate goal (a vector — the port folds the TeleportProxy actor into the
	// goal location, PORT_NOTES). Intermediate (priority 85) outranks Threat (80), so
	// the re-select drops the chase and navigates to the beacon.
	Brain->InitGoalWithVector(EWoTGoalIndex::Intermediate, Destination);
	bTeleportBeaconActive = true;
	TransitionOnGoalPriority(true);
	return true;
}

void AWoTNPCController::EnterTeleportState()
{
	// TeleporterReached Begin (Myrddraal.uc:480-489): stop moving, play the visible
	// PreTeleport wind-up, sound the Teleport slot, and start the crank-up timer. The
	// ScaleGlow fade and the paired teleport sprayers follow retail presentation.
	AWoTNPCCharacter* Self = Cast<AWoTNPCCharacter>(GetPawn());
	if (!Self)
	{
		EnterState(EWoTBrainState::Tracking);
		return;
	}
	StopMovement();
	bDirectMoveActive = false;
	bPathMoveActive = false;
	if (UCharacterMovementComponent* Move = Self->GetCharacterMovement())
	{
		Move->StopMovementImmediately();
	}
	// PlayAnim('PreTeleport', 0.75) (uc:484) — the wind-up the victim sees.
	Self->SetAnimIntent(FName(TEXT("PreTeleport")), 0.75f, false);
	// ScaleGlow body+weapon fade (uc:404-424): folded to the 0.25 floor for the episode,
	// restored at reappear.
	SpawnTeleportSprayer(false,Self->GetActorLocation());
	// Phase 0 = the PreTeleport wind-up; a sequence-less headless pawn uses a small
	// non-zero stand-in so the timeline still advances (as the swing/hit-react folds do).
	const float Windup = Self->SequenceDurationSeconds(FName(TEXT("PreTeleport")), 0.75f);
	TeleportPhase = 0;
	TeleportPhaseElapsed = 0.0f;
	TeleportPhaseDuration = Windup > 0.0f ? Windup : 0.05f;
}

void AWoTNPCController::AdvanceTeleportPhase()
{
	AWoTNPCCharacter* Self = Cast<AWoTNPCCharacter>(GetPawn());
	UWoTMyrddraalTeleportBehavior* Teleport = Cast<UWoTMyrddraalTeleportBehavior>(Behavior);
	if (!Self || !Teleport)
	{
		EnterState(EWoTBrainState::Tracking);
		return;
	}
	TeleportPhaseElapsed = 0.0f;
	switch (TeleportPhase)
	{
	case 0:
		// Wind-up done → CrankUp (uc:488: SetTimer(TeleportEffectCrankupDuration)).
		TeleportPhase = 1;
		TeleportPhaseDuration = Teleport->TeleportEffectCrankupDuration;
		break;
	case 1:
		// EnterMyrddraalTeleporting (uc:491-499): DestroySeekers, collision off, freeze
		// the MyrdTeleport vanish pose, and hide the pawn (+ its held sword). Hidden runs
		// for TeleportDuration.
		Teleport->DestroySeekers();
		Self->SetActorEnableCollision(false);
		Self->SetAnimIntent(FName(TEXT("MyrdTeleport")), 0.75f, false);
		Self->SetActorHiddenInGame(true);
		// The held sword must hide WITH the pawn — attachment does not propagate
		// bHidden across separate actors, so force it (verified: separate AWoTNPCWeapon).
		if (AWoTNPCWeapon* Held = Self->GetHeldWeapon())
		{
			Held->SetActorHiddenInGame(true);
		}
		TeleportPhase = 2;
		TeleportPhaseDuration = Teleport->TeleportDuration;
		break;
	case 2:
		SpawnTeleportSprayer(true,Teleport->GetPendingDestination());
		// MyrddraalTeleporting (uc:501-506): start the exit sprayer, sound the
		// Teleport slot, and run the exit crank-up.
		TeleportPhase = 3;
		TeleportPhaseDuration = Teleport->TeleportEffectCrankupDuration;
		break;
	case 3:
	{
		// ExitMyrddraalTeleporting (uc:508-522): Reappear sound, relocate to the chosen
		// node + face the threat, restore collision/visibility/ScaleGlow, then the 0.05s
		// fade-in before resuming.
		Self->EmitTeleportPresentation(3, Teleport->GetPendingDestination());
		Teleport->CommitReappear();
		Self->SetActorEnableCollision(true);
		Self->SetActorHiddenInGame(false);
		if (AWoTNPCWeapon* Held = Self->GetHeldWeapon())
		{
			Held->SetActorHiddenInGame(false);
			Held->ScaleGlow = 1.0f;
		}
		// Default.ScaleGlow (uc:513): the row's authored value (BindCreatureBody stamps it).
		Self->SetScaleGlowPresentation(WoTCreatureData::Find(Self->CreatureRowName)
			? WoTCreatureData::Find(Self->CreatureRowName)->ScaleGlow : 1.0f);
		TeleportPhase = 4;
		TeleportPhaseDuration = 0.05f;   // SetTimer(0.05) fade-in (uc:520)
		break;
	}
	default:
		// ResumeBehavior (uc:524): TransitionOnGoalPriority(true) → back to Tracking.
		if (!TransitionOnGoalPriority(true))
		{
			EnterState(EWoTBrainState::Waiting);
		}
		break;
	}
}

void AWoTNPCController::SpawnTeleportSprayer(bool bExit,const FVector& Location)
{
	if (AWoTNPCCharacter* Self = Cast<AWoTNPCCharacter>(GetPawn()))
	{
		Self->EmitTeleportPresentation(bExit ? 2 : 1, Location);
	}
}

void AWoTNPCController::StartProjectileAttack()
{
	DrawElapsed = 0.0f;
	bArrowReleased = false;
	if (AWoTNPCCharacter* Self = Cast<AWoTNPCCharacter>(GetPawn()))
	{
		if (Self->CreatureRowName == FName(TEXT("Archer")))
		{
			// PrePerformingAttack: StopMovement + TweenPlay KNEEL + FinishAnim
			// (Archer.uc:199-205). The label chain continues in TickProjectile.
			ArcherStateElapsed = 0.0f;
			BeginArcherPhase(1);
		}
		else
		{
			// Every non-Archer ranged creature retains the generic FireNotifyTime /
			// DrawTime episode and its ProjectileAttack table pick.
			Self->StampSlotAnim(FName(TEXT("ProjectileAttack")));
		}
	}
	if (Brain)
	{
		if (const FWoTGoal& G = Brain->GetGoal(Brain->CurrentGoalIndex); G.HasActor())
		{
			SetFocus(G.GoalActor.Get());
		}
	}
}

void AWoTNPCController::FireProjectileAtCurrentGoal()
{
	if (!Brain)
	{
		return;
	}
	const FWoTGoal& Goal = Brain->GetGoal(Brain->CurrentGoalIndex);
	AActor* Target = Goal.HasActor() ? Goal.GoalActor.Get() : nullptr;
	if (!IsValid(Target))
	{
		return;
	}
	AWoTNPCCharacter* NPC = Cast<AWoTNPCCharacter>(GetPawn());
	if (AWoTNPCWeapon* ActorWeapon = NPC ? NPC->GetRangedWeapon() : nullptr)
	{
		ActorWeapon->UseProjectileWeapon(GetPawn(), Target);
	}
	else if (UWoTWeaponComponent* Weapon =
		GetPawn() ? GetPawn()->FindComponentByClass<UWoTWeaponComponent>() : nullptr)
	{
		Weapon->UseProjectileWeapon(GetPawn(), Target);
	}
}

void AWoTNPCController::BeginArcherPhase(uint8 NewPhase)
{
	AWoTNPCCharacter* Self = Cast<AWoTNPCCharacter>(GetPawn());
	if (!Self)
	{
		return;
	}
	ArcherPhase = NewPhase;
	ArcherPhaseElapsed = 0.0f;
	ArcherPhaseDuration = 0.0f;
	const FWoTAnimTableEntry* Pick = nullptr;
	switch (ArcherPhase)
	{
	case 1: // PrePerformingAttack / Kneel
		Pick = Self->StampSlotAnim(FName(TEXT("Kneel")));
		break;
	case 2: // PerformingAttack / Draw
		Pick = Self->StampSlotAnim(FName(TEXT("Draw")));
		break;
	case 3: // drawn hold; the Draw pose remains on its final frame
		ArcherVisibilityPollRemaining = ArcherBowDrawnTimerDuration;
		ArcherHoldElapsed = 0.0f;
		return;
	case 4: // Timer saw the goal; retail sleeps RickyPauseTime before release
		ArcherPhaseDuration = ArcherRickyPauseTime;
		return;
	case 5: // PostReleaseBow fires first, then TweenPlay RELEASE + FinishAnim
		FireProjectileAtCurrentGoal();
		bArrowReleased = true;
		Pick = Self->StampSlotAnim(FName(TEXT("Release")));
		break;
	case 6: // PostPerformingAttack / GetUp
		Pick = Self->StampSlotAnim(FName(TEXT("GetUp")));
		break;
	default:
		return;
	}
	ArcherPhaseDuration = PickedAnimTotalTime(Pick, true);
	if (ArcherPhaseDuration <= 0.0f)
	{
		ArcherPhaseDuration = 0.01f;
	}
}

void AWoTNPCController::TickProjectile(float DeltaSeconds)
{
	UWoTWeaponComponent* Weapon =
	    GetPawn() ? GetPawn()->FindComponentByClass<UWoTWeaponComponent>() : nullptr;
	AWoTNPCCharacter* NPC = Cast<AWoTNPCCharacter>(GetPawn());
	AWoTNPCWeapon* ActorWeapon = NPC ? NPC->GetRangedWeapon() : nullptr;
	if ((!Weapon && !ActorWeapon) || !Brain)
	{
		EnterState(EWoTBrainState::Tracking);
		return;
	}
	const EWoTGoalIndex Goal = Brain->CurrentGoalIndex;
	const FWoTGoal& G = Brain->GetGoal(Goal);
	if (NPC && NPC->CreatureRowName == FName(TEXT("Archer")))
	{
		if (!G.HasActor() && ArcherPhase != 6)
		{
			// A vanished goal skips the remaining draw/release chain once. Do not
			// restart GetUp every tick: FinishAnim must be allowed to complete.
			BeginArcherPhase(6);
		}
		ArcherStateElapsed += DeltaSeconds;
		ArcherPhaseElapsed += DeltaSeconds;
		switch (ArcherPhase)
		{
		case 1:
			if (ArcherPhaseElapsed >= ArcherPhaseDuration) { BeginArcherPhase(2); }
			break;
		case 2:
			if (ArcherPhaseElapsed >= ArcherPhaseDuration)
			{
				// A visible goal releases immediately; an unseen one arms the repeating
				// 0.15s visibility poll and holds drawn for at most five seconds
				// (Archer.uc:208-235).
				BeginArcherPhase(Brain->IsGoalVisible(Goal) ? 5 : 3);
			}
			break;
		case 3:
			ArcherHoldElapsed += DeltaSeconds;
			ArcherVisibilityPollRemaining -= DeltaSeconds;
			if (ArcherVisibilityPollRemaining <= 0.0f)
			{
				ArcherVisibilityPollRemaining += ArcherBowDrawnTimerDuration;
				if (Brain->IsGoalVisible(Goal))
				{
					BeginArcherPhase(4);
					break;
				}
			}
			if (ArcherHoldElapsed >= ArcherBowDrawnSleepDuration)
			{
				// Sleep(BowDrawnSleepDuration) expiring calls the PerformingAttack
				// FUNCTION (Archer.uc:236-238), whose inherited body jumps to
				// PostPerformingAttack/GetUp (LegendPawn.uc:1734-1738) — a target
				// unseen for the full five seconds ENDS the attack, no redraw.
				BeginArcherPhase(6);
			}
			break;
		case 4:
			if (ArcherPhaseElapsed >= ArcherPhaseDuration) { BeginArcherPhase(5); }
			break;
		case 5:
			if (ArcherPhaseElapsed >= ArcherPhaseDuration)
			{
				float Gap = TNumericLimits<float>::Max();
				Brain->GetGoalDistance(Goal, Gap);
				UWoTArcherBowBehavior* BowBehavior =
					Cast<UWoTArcherBowBehavior>(Behavior);
				const float ExitInterval = BowBehavior
					? BowBehavior->MinProjectileAttackingStateExitInterval
					: ArcherMinProjectileExitInterval;
				const float SwitchDistance = BowBehavior
					? BowBehavior->SwitchToMeleeDistance : ArcherSwitchToMeleeDistance;
				if (ArcherStateElapsed > ExitInterval && Gap < SwitchDistance)
				{
					if (BowBehavior)
					{
						BowBehavior->CommitToMelee();
					}
					bResumeArcherGetUpAfterPreparation = true;
					BeginWeaponSwitch(NPC->GetMeleeWeapon(), false);
				}
				else
				{
					BeginArcherPhase(2);
				}
			}
			break;
		case 6:
			if (ArcherPhaseElapsed >= ArcherPhaseDuration)
			{
				EnterState(EWoTBrainState::Tracking);
			}
			break;
		default:
			break;
		}
		return;
	}
	// The target fled or broke LOS during the draw: abandon the shot (Archer.uc:210
	// gates the release on IsGoalVisible).
	if (!G.HasActor() || !Brain->IsGoalVisible(Goal))
	{
		EnterState(EWoTBrainState::Tracking);
		return;
	}
	DrawElapsed += DeltaSeconds;
	// Release the arrow at the draw's notify point (Archer PostReleaseBow ->
	// ShootRangedAmmo -> UseProjectileWeapon, Grunt.uc:1529).
	const float FireTime = ActorWeapon ? ActorWeapon->FireNotifyTime : Weapon->FireNotifyTime;
	const float EndTime = ActorWeapon ? ActorWeapon->DrawTime : Weapon->DrawTime;
	if (!bArrowReleased && DrawElapsed >= FireTime)
	{
		bArrowReleased = true;
		FireProjectileAtCurrentGoal();
	}
	// Episode ends -> back to the tracking loop; the 0.5s Offensive pulse paces the
	// next shot (or the band re-classifies to melee once the behavior commits).
	if (DrawElapsed >= EndTime)
	{
		EnterState(EWoTBrainState::Tracking);
	}
}

bool AWoTNPCController::MeleeDamageGoal(const FWoTMeleeAttack& Attack)
{
	AWoTNPCCharacter* Self = Cast<AWoTNPCCharacter>(GetPawn());
	if (!Self || !Brain)
	{
		return false;
	}
	const EWoTGoalIndex GoalIdx = Brain->CurrentGoalIndex;
	const FWoTGoal& G = Brain->GetGoal(GoalIdx);

	// Still an actor goal, still in melee range? (The goal may have fled
	// between the swing start and the notify — the miss case.) WithinMaxMeleeDistance
	// (Grunt.uc:1444): a plain gap<=MeleeRange, EXTENDED for the Minion into the tentacle
	// band [Min,Max] (Minion.uc:220-223 overrides WithinMaxMeleeDistance to OR in
	// InUseTentacleDistance) so the standing/grab reach past MeleeRange lands (PORT_NOTES).
	float Gap = 0.0f;
	if (!G.HasActor() || !Brain->GetGoalDistance(GoalIdx, Gap))
	{
		return false;
	}
	bool bInReach = Gap <= Self->MeleeRange;
	if (!bInReach)
	{
		if (const UWoTMinionStealthBehavior* Minion = Cast<UWoTMinionStealthBehavior>(Behavior))
		{
			bInReach = Minion->InUseTentacleDistance(Gap);
		}
	}
	if (!bInReach)
	{
		return false;
	}
	AActor* GoalActor = G.GoalActor.Get();

	// Trace from our height-offset location to the offset point on the goal
	// cylinder, clamped to its half-height; the hit must be the goal itself.
	const float HalfHeight = Self->GetSimpleCollisionHalfHeight();
	FVector TraceStart = Self->GetActorLocation();
	TraceStart.Z += HalfHeight * Attack.HeightModifier;

	float GoalRadius = 0.0f, GoalHalf = 0.0f;
	FVector GoalLoc = GoalActor->GetActorLocation();
	GoalActor->GetSimpleCollisionCylinder(GoalRadius, GoalHalf);
	FVector DamagePoint = GoalLoc;
	DamagePoint.Z += HalfHeight * Attack.HeightModifier;
	DamagePoint.Z = FMath::Clamp(DamagePoint.Z, GoalLoc.Z - GoalHalf, GoalLoc.Z + GoalHalf);

	FHitResult Hit;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(WoTNPCMelee), /*bTraceComplex*/ true, Self);
	const bool bHit = GetWorld()->LineTraceSingleByChannel(Hit, TraceStart, DamagePoint,
	                                                       ECC_Pawn, Params);
	if (!bHit || Hit.GetActor() != GoalActor)
	{
		return false;
	}

	// GoalActor.TakeDamage(Damage, self, HitLocation, zero momentum, type)
	// (Grunt.uc:1463) — through the ported damage chain, type name carried. The
	// creature behavior may scale the swing (Warder conscription 2x, Warder.uc:255).
	const float Multiplier = Behavior ? Behavior->GetMeleeDamageMultiplier() : 1.0f;
	FWoTDamageEvent DamageEvent;
	DamageEvent.WoTDamageType = Attack.DamageTypeName;
	DamageEvent.UE1Momentum = FVector::ZeroVector;   // Grunt.uc:1463
	DamageEvent.HitInfo = Hit;
	GoalActor->TakeDamage(Attack.Damage * Multiplier, DamageEvent, this, Self);
	if (Self->CreatureRowName == TEXT("Warder")
		&& Attack.DamageTypeName == TEXT("Ripped"))
	{
		if (AWoTCharacter* Pulled = Cast<AWoTCharacter>(GoalActor);
			Pulled && Pulled->GetVelocity().Z <= 0.0f)
		{
			const UCharacterMovementComponent* Move = Pulled->GetCharacterMovement();
			const float Mass = Move ? FMath::Max(Move->Mass, 1.0f) : 100.0f;
			FVector Delta = 75000.0f
				* (Self->GetActorLocation() - Pulled->GetActorLocation()).GetSafeNormal()
				/ Mass;
			Delta.Z += 250.0f;
			ApplyCreatureImpulse(Pulled, Delta);
		}
	}
	// A landed swing sounds off: the weapon's flesh-hit
	// (PlayMeleeHitSound -> WeaponMeleeHitEnemy, Grunt.uc:1408), then the taunt —
	// MeleeKilledEnemyTaunt on a kill (odds 1.0) else MeleeHitEnemyTaunt (authored
	// odds 0.2 gate in-table) — WOTPawn MeleeDamageGoal's split.
	Self->PlayVoiceSlot(Attack.HitSoundSlot);
	const AWoTCharacter* Victim = Cast<AWoTCharacter>(GoalActor);
	Self->PlayVoiceSlot((Victim && !Victim->IsAlive())
	                        ? FName(TEXT("MeleeKilledEnemyTaunt"))
	                        : FName(TEXT("MeleeHitEnemyTaunt")));
	// A landed swing: the behavior may act on it (Myrddraal delayed-damage leech +
	// teleport, SetupDelayedDamageToGoal, Myrddraal.uc:554).
	if (Behavior)
	{
		Behavior->OnMeleeHit(GoalActor);
	}
	return true;
}

void AWoTNPCController::ApplyCreatureImpulse(AWoTCharacter* Victim,
	const FVector& Delta, const FRotator* NewRotation, float ShakeTime,
	float RollMag, float VertMag)
{
	if (!Victim || !HasAuthority())
	{
		return;
	}
	if (NewRotation)
	{
		Victim->SetCreaturePresentationPitch(NewRotation->Pitch);
	}
	Victim->RecordAuthorityCreatureImpulseForTest(Delta);
	Victim->AddUE1Velocity(Delta);
	// Creature hit momentum is an authority-side external velocity change. Make
	// the normal CharacterMovement response carry that velocity before newer
	// autonomous saved moves can continue from their pre-hit state. This is the
	// built-in correction channel, not a second event/impulse RPC.
	if (UCharacterMovementComponent* Movement = Victim->GetCharacterMovement())
	{
		Movement->ForceClientAdjustment();
	}
	if (ShakeTime > 0.0f)
	{
		if (AWoTPlayerController* PC =
			Cast<AWoTPlayerController>(Victim->GetController()))
		{
			PC->ClientShakeView(ShakeTime, RollMag, VertMag);
		}
	}
}

#if WITH_AUTOMATION_TESTS
bool AWoTNPCController::MeleeDamageGoalForTest(const FWoTMeleeAttack& Attack)
{
	return MeleeDamageGoal(Attack);
}

void AWoTNPCController::StartMeleeSwingForTest()
{
	StartMeleeSwing();
}

void AWoTNPCController::TickMeleeForTest(float DeltaSeconds)
{
	TickMelee(DeltaSeconds);
}
#endif

// ---------------------------------------------------------------- orders layer

const FWoTRangeBandSet& AWoTNPCController::ActiveBands() const
{
	// Navigating to an alarm/help/seal uses the navigation ladder; everything else
	// (threat/guard) uses the creature's threat ladder.
	if (Brain && Brain->CurrentGoalIndex == EWoTGoalIndex::Intermediate)
	{
		return IntermediateBands;
	}
	return TrackingBands;
}

bool AWoTNPCController::IsHealthy() const
{
	// Grunt.uc:897: the morale check hard-returns true before the real test (the
	// //xxxrlo hack — morale disabled in shipped). Ported faithfully-inert; the
	// Refuge states AND the captain idle heal (both gated on !IsHealthy) are
	// therefore dead in retail. The shipped dead branch, preserved here as the
	// documented threshold for a future morale pass (it would be unreachable
	// after the early return — MSVC C4702), is:
	//     const AWoTNPCCharacter* Self = Cast<AWoTNPCCharacter>(GetPawn());
	//     const float Threshold = Self->BrokenMoralePct * MaxHealth / 100.0f;
	//     return Self->Health > Threshold;   // "broken morale" below the pct of max
	return true;
}

bool AWoTNPCController::CanBeGivenNewOrders() const
{
	// Grunt.uc:831: available iff not occupied by a Threat/Guarding/Intermediate/
	// Refuge goal.
	if (!Brain || State == EWoTBrainState::Falling)
	{
		return false;
	}
	const bool bOccupied =
		Brain->IsGoalValid(EWoTGoalIndex::Threat) ||
		Brain->IsGoalValid(EWoTGoalIndex::Guarding) ||
		Brain->IsGoalValid(EWoTGoalIndex::Intermediate) ||
		Brain->IsGoalValid(EWoTGoalIndex::Refuge);
	return !bOccupied;
}

bool AWoTNPCController::IsLeader() const
{
	// Captain.uc:433: a leader is a captain with a valid guarding goal.
	return Brain && Brain->IsGoalValid(EWoTGoalIndex::Guarding);
}

bool AWoTNPCController::FindLeader()
{
	// Captain.uc:440: captains don't follow leaders.
	if (bIsCaptain || !Brain)
	{
		return false;
	}
	// Grunt.uc:1148: only act if a guarding goal descriptor exists (every ported
	// NPC carries the slot — the check is always true, kept for fidelity).
	if (!Brain->GetGoal(EWoTGoalIndex::Guarding).bSlotUsed)
	{
		return false;
	}
	AWoTNPCController* Leader = WoTAIUtil::GetLeader(this);
	if (Leader && Leader->GetBrain())
	{
		// Mimic the leader's guarding characteristics (Grunt.uc:1154).
		Brain->AssignGoalFrom(EWoTGoalIndex::Guarding, Leader->GetBrain(), EWoTGoalIndex::Guarding);
		return true;
	}
	// No leader in range → drop the guarding goal (Grunt.uc:1158).
	Brain->InvalidateGoal(EWoTGoalIndex::Guarding);
	return false;
}

void AWoTNPCController::InitCaptainOrders()
{
	AWoTNPCCharacter* Self = Cast<AWoTNPCCharacter>(GetPawn());
	if (!Self)
	{
		return;
	}
	bIsCaptain = Self->bIsCaptain;
	if (!bIsCaptain)
	{
		return;
	}
	AssignedOrders = Self->AssignedOrders;
	// Captain.uc:87 MP hack: networked play forces Automatic. Champion skips this
	// (Champion.uc:14-20 keeps KillIntruder via Super(Grunt).SetInitialState).
	if (!Self->bChampionOrders && GetWorld() && GetWorld()->GetNetMode() != NM_Standalone)
	{
		AssignedOrders = EWoTOrders::Automatic;
	}
	ChangeOrders(AssignedOrders);
	WoTAIUtil::InvokeWotPawnFindLeader(this);
}

EWoTOrders AWoTNPCController::MapOrders(FName OrdersName)
{
	// Captain.uc:176.
	if (OrdersName == FName(TEXT("Guard")))        return EWoTOrders::Guard;
	if (OrdersName == FName(TEXT("KillIntruder"))) return EWoTOrders::KillIntruder;
	if (OrdersName == FName(TEXT("GuardSeal")))    return EWoTOrders::GuardSeal;
	if (OrdersName == FName(TEXT("SoundAlarm")))   return EWoTOrders::SoundAlarm;
	if (OrdersName == FName(TEXT("GetHelp")))      return EWoTOrders::GetHelp;
	if (OrdersName == FName(TEXT("Automatic")))    return EWoTOrders::Automatic;
	return EWoTOrders::KillIntruder;
}

void AWoTNPCController::ResetOrders()
{
	// Captain.uc:208.
	WorkingOrders = AssignedOrders;
}

void AWoTNPCController::ChangeOrders(EWoTOrders NewOrders)
{
	// Captain.uc:235: guard-location and guard-seal "do not exist" via ChangeOrders
	// — they collapse to KillIntruder. (OnGuardLocation/OnGuardSeal remain
	// reachable through DispatchOrders + the seal Trigger, Captain.uc:112.)
	if (NewOrders == EWoTOrders::Guard || NewOrders == EWoTOrders::GuardSeal)
	{
		NewOrders = EWoTOrders::KillIntruder;
	}
	DispatchOrders(NewOrders);
	WorkingOrders = NewOrders;
	if (AssignedOrders != NewOrders)
	{
		// Orders changed → let nearby grunts re-pick their leader (Captain.uc:247).
		AssignedOrders = NewOrders;
		WoTAIUtil::InvokeWotPawnFindLeader(this);
	}
	if (AWoTNPCCharacter* Self=Cast<AWoTNPCCharacter>(GetPawn()))
	{
		static const FName Slots[] = {TEXT("OrderGuard"),TEXT("OrderKillIntruder"),
			TEXT("OrderGuardSeal"),TEXT("OrderSoundAlarm"),TEXT("OrderGetHelp"),NAME_None};
		const int32 I=static_cast<int32>(NewOrders);
		if(I>=0&&I<UE_ARRAY_COUNT(Slots)&&!Slots[I].IsNone()) Self->PlayVoiceSlot(Slots[I]);
	}
}

void AWoTNPCController::DispatchOrders(EWoTOrders Orders)
{
	// Captain.uc:332.
	switch (Orders)
	{
	case EWoTOrders::Guard:        OnGuardLocation();   break;
	case EWoTOrders::KillIntruder: OnKillIntruder();    break;
	case EWoTOrders::GuardSeal:    OnGuardSeal();        break;
	case EWoTOrders::SoundAlarm:   OnSoundAlarm();       break;
	case EWoTOrders::GetHelp:      OnGetHelp();          break;
	case EWoTOrders::Automatic:    OnAutomaticOrders();  break;
	default:                       OnInvalidOrder();     break;
	}
}

void AWoTNPCController::OnGuardLocation()
{
	// Captain.uc:368: guard the current location.
	if (Brain && GetPawn())
	{
		Brain->InitGoalWithVector(EWoTGoalIndex::Guarding, GetPawn()->GetActorLocation());
	}
}

void AWoTNPCController::OnKillIntruder()
{
	// Captain.uc:375: no guarding — act as a Grunt.
	if (Brain)
	{
		Brain->InvalidateGoal(EWoTGoalIndex::Guarding);
	}
}

void AWoTNPCController::OnGuardSeal()
{
	// Captain.uc:382: guard the nearest unowned seal; none → drop guarding.
	// (GetClosestUnownedSeal already invalidates on failure — the explicit call
	// mirrors the .uc.)
	if (!WoTAIUtil::GetClosestUnownedSeal(this, EWoTGoalIndex::Guarding) && Brain)
	{
		Brain->InvalidateGoal(EWoTGoalIndex::Guarding);
	}
}

void AWoTNPCController::NotifySealTriggered()
{
	++SealTriggerNotifiesForTest;
	// Captain.uc:108-119: only captains route Trigger to the seal branch, and
	// only under standing GuardSeal orders (AssignedOrders, not Working).
	if (bIsCaptain && AssignedOrders == EWoTOrders::GuardSeal)
	{
		OnGuardSeal();
	}
}

void AWoTNPCController::OnSoundAlarm()   { if (Brain) Brain->InvalidateGoal(EWoTGoalIndex::Guarding); } // Captain.uc:392
void AWoTNPCController::OnGetHelp()      { if (Brain) Brain->InvalidateGoal(EWoTGoalIndex::Guarding); } // Captain.uc:399
void AWoTNPCController::OnAutomaticOrders() { if (Brain) Brain->InvalidateGoal(EWoTGoalIndex::Guarding); } // Captain.uc:406
void AWoTNPCController::OnInvalidOrder() { if (Brain) Brain->InvalidateGoal(EWoTGoalIndex::Guarding); } // Captain.uc:413

void AWoTNPCController::EvaluateOrders()
{
	// Captain.uc:298-322: the live alarm→help→kill ladder (the old switch at
	// :266-296 is commented out in source). Mutates WorkingOrders + GI_Intermediate.
	if (!Brain)
	{
		return;
	}
	AWoTNPCCharacter* Self = Cast<AWoTNPCCharacter>(GetPawn());
	if (WorkingOrders == EWoTOrders::KillIntruder)
	{
		// Set to kill → she kills: no intermediate, just pursue the threat.
		Brain->InvalidateGoal(EWoTGoalIndex::Intermediate);
	}
	else if (WoTAIUtil::GetClosestAlarm(this, EWoTGoalIndex::Intermediate))
	{
		// An alarm exists → ring it.
		WorkingOrders = EWoTOrders::SoundAlarm;
	}
	else if (Self && WoTAIUtil::CollectAvailableWotPawns(
		this, Self->FindHelpRadius, ReinforcementCandidates))
	{
		// Help available → go get it.
		WoTAIUtil::GetNextAvailableWotPawn(this, EWoTGoalIndex::Intermediate, ReinforcementCandidates);
		WorkingOrders = EWoTOrders::GetHelp;
	}
	else
	{
		// Otherwise she kills.
		WorkingOrders = EWoTOrders::KillIntruder;
		Brain->InvalidateGoal(EWoTGoalIndex::Intermediate);
	}
	if (Self)
	{
		static const FName Slots[] = {TEXT("OrderGuard"),TEXT("OrderKillIntruder"),
			TEXT("OrderGuardSeal"),TEXT("OrderSoundAlarm"),TEXT("OrderGetHelp"),NAME_None};
		const int32 I=static_cast<int32>(WorkingOrders);
		if(I>=0&&I<UE_ARRAY_COUNT(Slots)&&!Slots[I].IsNone()) Self->PlayVoiceSlot(Slots[I]);
	}
}

void AWoTNPCController::CaptainIdleHeal()
{
	// Captain.uc:448 OnWaitingNotification: Health++ while unhealthy. IsHealthy is
	// always true (morale disabled), so this never fires in shipped — carried
	// structurally faithful (PORT_NOTES: captain idle heal inert in retail).
	if (!bIsCaptain || IsHealthy())
	{
		return;
	}
	if (AWoTCharacter* Self = Cast<AWoTCharacter>(GetPawn()))
	{
		Self->Health++;
	}
}

void AWoTNPCController::AlarmSounded(AWoTAlarm* Alarm)
{
	// Grunt.uc:567: an available pawn adopts the rung alarm as an intermediate goal.
	if (Brain && Alarm && CanBeGivenNewOrders())
	{
		Brain->InitGoalWithActor(EWoTGoalIndex::Intermediate, Alarm);
	}
}

void AWoTNPCController::AlertedByCaptain(AWoTNPCController* AlertingCaptain)
{
	// Grunt.uc:578: an available pawn navigates to the alerting captain.
	if (Brain && AlertingCaptain && AlertingCaptain->GetPawn() && CanBeGivenNewOrders())
	{
		Brain->InitGoalWithActor(EWoTGoalIndex::Intermediate, AlertingCaptain->GetPawn());
	}
}

void AWoTNPCController::AlertVisibleWotPawns()
{
	// Captain.uc:775: alert every LOS'd, alertable WotPawn within CollisionRadius +
	// AlertVisibleWotPawnsRadius (no team filter — matches the .uc RadiusActors scan;
	// occupied/enemy pawns naturally fail CanBeGivenNewOrders).
	AWoTNPCCharacter* Self = Cast<AWoTNPCCharacter>(GetPawn());
	if (!Self)
	{
		return;
	}
	float Radius = 0.0f, HalfHeight = 0.0f;
	Self->GetSimpleCollisionCylinder(Radius, HalfHeight);
	const float Reach = Radius + AlertVisibleWotPawnsRadius;
	const float ReachSquared = Reach * Reach;
	const FVector Origin = Self->GetActorLocation();
	for (TActorIterator<AWoTNPCCharacter> It(GetWorld()); It; ++It)
	{
		AWoTNPCCharacter* Other = *It;
		if (Other == Self ||
		    FVector::DistSquared(Other->GetActorLocation(), Origin) > ReachSquared)
		{
			continue;
		}
		AWoTNPCController* OtherController = Cast<AWoTNPCController>(Other->GetController());
		if (!OtherController || !OtherController->CanBeGivenNewOrders())
		{
			continue;
		}
		// LineOfSightTo (Captain.uc:780): the no-cone view test.
		if (WoTSight::OmniSight(Self, Other, WoTSight::VisibilityOf(Other)))
		{
			OtherController->AlertedByCaptain(this);
		}
	}
}

void AWoTNPCController::HandleIntermediateArrival()
{
	// The GI_Intermediate on-arrival dispatch, keyed by
	// goal actor type + the arriver's identity/orders. Fired from TickTracking on
	// the Reached classification.
	if (!Brain)
	{
		return;
	}
	const EWoTGoalIndex Slot = EWoTGoalIndex::Intermediate;
	AActor* GoalActor = Brain->GetGoal(Slot).GoalActor.Get();

	// --- Alarm ---
	if (AWoTAlarm* AlarmActor = Cast<AWoTAlarm>(GoalActor))
	{
		if (bIsCaptain && WorkingOrders == EWoTOrders::SoundAlarm)
		{
			// Captain SuccessfullyNavigatedToAlarmToSound (Captain.uc:666-727): strike
			// the gong; the alarm rings when the strike finishes (TickGongStrike).
			BeginGongStrike(AlarmActor);
		}
		else
		{
			// Grunt SuccessfullyNavigatedToAlarm (Grunt.uc:1946): adopt the alarm's
			// threat, then drop the alarm goal.
			Brain->InvalidateGoal(Slot);
			if (AlarmActor->ThreatActor.IsValid())
			{
				Brain->InitGoalWithActor(EWoTGoalIndex::Threat, AlarmActor->ThreatActor.Get());
				if (!Brain->IsGoalNavigable(EWoTGoalIndex::Threat))
				{
					Brain->InvalidateGoal(EWoTGoalIndex::Threat);
				}
			}
		}
		return;
	}

	// NOTE: there is no Seal arrival here. The NavigatingToSeal/Seal* states
	// (Captain.uc:800-825) and the Seal/SealAltar branches of
	// GoalIndexTransitionIntermediate (Captain.uc:494-535) are inside a /*...*/
	// block in shipped — only the Alarm (SoundAlarm) and WotPawn (GetHelp) routes
	// are live, so a seal is never a GI_Intermediate goal. GuardSeal works entirely
	// through OnGuardSeal → GI_Guarding = closest unowned seal, pulled back by the
	// Guarding slot's ZeroIfLess/480 gate — no seal navigation state. (Out-of-scope
	// dead code, per the seal-carry deferral.)

	// --- Captain / help WotPawn ---
	if (AWoTNPCCharacter* OtherPawn = Cast<AWoTNPCCharacter>(GoalActor))
	{
		if (bIsCaptain && WorkingOrders == EWoTOrders::GetHelp)
		{
			if(AWoTNPCCharacter* Self=Cast<AWoTNPCCharacter>(GetPawn()))
			{
				Self->StampSlotAnim(TEXT("GiveOrders"));
				Self->PlayVoiceSlot(TEXT("GiveOrders"));
			}
			// Captain HelpReached (Captain.uc:749): alert nearby friendlies, then aim
			// at the next reinforcement; none left → revert to kill.
			AlertVisibleWotPawns();
			if (!WoTAIUtil::GetNextAvailableWotPawn(this, Slot, ReinforcementCandidates))
			{
				WorkingOrders = EWoTOrders::KillIntruder;
				Brain->InvalidateGoal(Slot);
			}
			return;
		}
		// Grunt SuccessfullyNavigatedToCaptain (Grunt.uc:1967): copy the captain's
		// threat, then drop the intermediate.
		if (AWoTNPCCharacter* Self = Cast<AWoTNPCCharacter>(GetPawn()))
		{
			// Grunt.SuccessfullyNavigatedToCaptain plays the literal vertex-mesh
			// sequence Listen; it is not an AnimationTable slot. Its authored
			// PlayAnimSound notify later dispatches the AcceptOrders voice slot.
			Self->SetTweenAnimIntent(TEXT("Listen"), 1.0f, 0.1f, false);
			Self->HoldLocomotionFor(1.1f);
			Self->PlayVoiceSlot(TEXT("AwaitingOrders"));
		}
		Brain->InvalidateGoal(Slot);
		const AWoTNPCController* OtherController = Cast<AWoTNPCController>(OtherPawn->GetController());
		if (OtherController && OtherController->GetBrain())
		{
			Brain->AssignGoalFrom(EWoTGoalIndex::Threat, OtherController->GetBrain(),
			                      EWoTGoalIndex::Threat);
			if (!Brain->IsGoalNavigable(EWoTGoalIndex::Threat))
			{
				Brain->InvalidateGoal(EWoTGoalIndex::Threat);
			}
		}
		return;
	}

	// --- Plain location / unknown intermediate: just drop it ---
	// (SuccessfullyNavigatedToLocation, Grunt.uc:1935.)
	Brain->InvalidateGoal(Slot);
}

void AWoTNPCController::BeginGongStrike(AWoTAlarm* AlarmActor)
{
	// BeginState's StopMovement (Captain.uc:668-672) already ran on arrival.
	// PerformInactivity: TweenPlaySlotAnim(Self, GongAnimSlot); FinishAnim()
	// (Captain.uc:723-725). A captain whose table has no Gong slot (only Sister,
	// Myrddraal and Archer author one) plays nothing and rings at once.
	AWoTNPCCharacter* Self = Cast<AWoTNPCCharacter>(GetPawn());
	const FWoTAnimTableEntry* Pick = Self ? Self->StampSlotAnim(TEXT("Gong")) : nullptr;
	const float StrikeTime = PickedAnimTotalTime(Pick, true);
	if (StrikeTime <= 0.0f)
	{
		SoundAlarmAtGoal(AlarmActor);
		return;
	}
	bGongStrikeActive = true;
	GongAlarm = AlarmActor;
	GongStrikeRemaining = StrikeTime;
	SetFocus(AlarmActor);
}

bool AWoTNPCController::TickGongStrike(float DeltaSeconds)
{
	if (!bGongStrikeActive)
	{
		return false;
	}
	AWoTNPCCharacter* Self = Cast<AWoTNPCCharacter>(GetPawn());
	AWoTAlarm* AlarmActor = GongAlarm.Get();
	const EWoTGoalIndex Slot = EWoTGoalIndex::Intermediate;
	if (!Self || !Brain || !AlarmActor || WorkingOrders != EWoTOrders::SoundAlarm
		|| Brain->CurrentGoalIndex != Slot
		|| Brain->GetGoal(Slot).GoalActor.Get() != AlarmActor)
	{
		// The state's latent code was abandoned before FinishAnim returned.
		bGongStrikeActive = false;
		GongAlarm.Reset();
		return false;
	}
	bDirectMoveActive = false;
	if (bPathMoveActive)
	{
		StopMovement();
		bPathMoveActive = false;
	}
	Self->GetCharacterMovement()->StopMovementImmediately();
	SetFocus(AlarmActor);
	GongStrikeRemaining -= DeltaSeconds;
	if (GongStrikeRemaining > 0.0f)
	{
		return true;
	}
	bGongStrikeActive = false;
	GongAlarm.Reset();
	SoundAlarmAtGoal(AlarmActor);
	return true;
}

void AWoTNPCController::SoundAlarmAtGoal(AWoTAlarm* AlarmActor)
{
	if (!Brain || !AlarmActor)
	{
		return;
	}
	// SoundAlarm (Captain.uc:679-713): stamp the alarm with our threat and ring it.
	const FWoTGoal& ThreatGoal = Brain->GetGoal(EWoTGoalIndex::Threat);
	if (ThreatGoal.HasActor())
	{
		AlarmActor->ThreatActor = ThreatGoal.GoalActor;
		AlarmActor->ThreatLocation = FVector::ZeroVector;
	}
	else
	{
		FVector LastSeen;
		if (Brain->GetLastVisibleLocation(EWoTGoalIndex::Threat, LastSeen))
		{
			AlarmActor->ThreatActor = nullptr;
			AlarmActor->ThreatLocation = LastSeen;
		}
	}
	AlarmActor->ActivateAlarm(GetPawn());
	// AlarmSounded (Captain.uc:716): drop the alarm goal, revert to kill.
	Brain->InvalidateGoal(EWoTGoalIndex::Intermediate);
	WorkingOrders = EWoTOrders::KillIntruder;
}
