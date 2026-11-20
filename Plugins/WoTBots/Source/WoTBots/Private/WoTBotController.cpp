#include "WoTBotController.h"
#include "Testing/WoTSoakEvents.h"
#include "WoTBotFactionProfile.h"
#include "WoTBotRoster.h"
#include "WoTBotSettings.h"
#include "WoTBotNavigation.h"
#include "AI/WoTCasterComponent.h"
#include "AI/WoTNPCCharacter.h"
#include "Inventory/WoTAngrealInventory.h"
#include "Inventory/WoTInventory.h"
#include "Inventory/WoTInventoryComponent.h"
#include "Projectiles/WoTMashadarGuide.h"
#include "WoTCharacter.h"
#include "WoTGameMode.h"
#include "WoTPlayerState.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Navigation/PathFollowingComponent.h"
#include "TimerManager.h"
#include "WoTRandom.h"

namespace
{
bool HasMovementHold(const AWoTCharacter* Body)
{
	// These two attached effects disable live player movement until detached.
	// Use script classes as in WoTBotBehaviour.cpp; the effect types are not exported.
	static UClass* LiftClass = FindObject<UClass>(nullptr, TEXT("/Script/WoT.WoTWhirlwindLeech"));
	static UClass* IceClass = FindObject<UClass>(nullptr, TEXT("/Script/WoT.WoTIceLeech"));
	for (const UActorComponent* Component : Body->GetComponents())
		if (IsValid(Component) && Component->IsRegistered()
			&& ((LiftClass && Component->IsA(LiftClass)) || (IceClass && Component->IsA(IceClass)))) return true;
	return false;
}

void DestroyHeldItems(AWoTCharacter* Body)
{
	if (!Body || !Body->InventoryComponent) return;
	const auto Items = Body->InventoryComponent->Items;
	for (AWoTInventory* Item : Items)
	{
		if (!IsValid(Item)) continue;
		Body->InventoryComponent->DeleteInventory(Item);
		Item->Destroy();
	}
}
}

AWoTBotController::AWoTBotController()
{
	bWantsPlayerState = true;
	PrimaryActorTick.bCanEverTick = true;
	bSetControlRotationFromPawnOrientation = false;
}

void AWoTBotController::InitializeIdentity(UWoTBotRoster* InRoster, UClass* PlayerClass, const FString& Name)
{
	Roster = InRoster;
	if (AWoTPlayerState* Identity = GetPlayerState<AWoTPlayerState>())
	{
		Identity->SetIsABot(true);
		Identity->SetFactionClass(PlayerClass);
		// The ordinary name replication path also supplies the join message.
		Identity->SetPlayerName(Name);
	}
}

void AWoTBotController::OnPossess(APawn* InPawn)
{
	Super::OnPossess(InPawn);
	AWoTCharacter* Body = Cast<AWoTCharacter>(InPawn);
	if (!Body) return;
	if (!Roster.IsValid()) Roster = GetWorld()->GetSubsystem<UWoTBotRoster>();
	OwnedPawns.AddUnique(Body);
	UWoTCasterComponent* Component = Body->FindComponentByClass<UWoTCasterComponent>();
	if (!Component)
	{
		Component = NewObject<UWoTCasterComponent>(Body);
		Component->bBuildLoadout = false;
		Body->AddInstanceComponent(Component);
		Component->RegisterComponent();
	}
	Component->bBuildLoadout = false;
	Caster = Component;
	InitializePoolPerception();
	if (const auto* Profile = WoTBotProfiles::Get().Find(Body->GetClass()))
	{
		Component->MinOffensiveUsageInterval = Profile->MinOffensiveUsageInterval;
		Component->MinDefensiveUsageInterval = Profile->MinDefensiveUsageInterval;
	}
	UCharacterMovementComponent* Movement = Body->GetCharacterMovement();
	Movement->GetNavMovementProperties()->bUseAccelerationForPaths = true;
	// Player braking is friction-based (constant deceleration is 0.01).
	// The engine's v^2/(2*a) estimate otherwise brakes over millions of units.
	Movement->GetNavMovementProperties()->bUseFixedBrakingDistanceForPaths = true;
	Movement->GetNavMovementProperties()->FixedPathBrakingDistance = 80.0f;
	Movement->GetNavMovementProperties()->bUpdateNavAgentWithOwnersCollision = false;
	FNavAgentProperties& Agent = Movement->GetNavAgentPropertiesRef();
	Agent.AgentRadius = 17.0f;
	Agent.AgentHeight = 92.0f;
	Agent.AgentStepHeight = 25.0f;
	Agent.SetPreferredNavData(AWoTBotNavMesh::StaticClass());
	GetPathFollowingComponent()->UpdateCachedComponents();
	Body->OnDamageApplied.AddUObject(this, &ThisClass::DamageApplied);
	Body->OnCharacterMovementUpdated.AddDynamic(this, &ThisClass::BodyMovementUpdated);
	ResetBehaviour(Body);
	SetActorTickEnabled(!bIdle && !bFrozen);
	if (bIdle) SuspendMovement();
	InventoryPulse();
	StartThinking();
}

void AWoTBotController::ResetBehaviour(AWoTCharacter* Body)
{
	PendingPoolSounds.Reset();
	for (auto& Pair : PoolBeliefs) Pair.Value.VisibleSince = -1;
	State = EWoTBotState::Roam;
	if (Target.IsValid()) Target->OnDamageApplied.RemoveAll(this);
	Target.Reset();
	AimingItem.Reset();
	CastHold.Clear();
	ReleasePoolGoal();
	UnreachablePools.Reset(); // Reachability depends on where this body starts.
	LastCombatProgress = GetWorld()->GetTimeSeconds();
	LastDamagedTime = -1e9f;
	DamageSearchUntil = -1;
	DamageSightCandidate.Reset();
	FeltVelocity = Body->GetVelocity();
	bTargetVisible = bWantsMovement = false;
	bNavigationUnavailable = false;
	LastOffensiveCastTime = LastDefensiveCastTime = -1e9f;
	NextRepathTime = 0.0f;
	ClimbBlockedSince = -1.0f;
	bArrivedByTeleport = false;
	LastProgressTime = LastRecoveryTime = GetWorld()->GetTimeSeconds();
	ProgressPosition = LastBodyLocation = Body->GetActorLocation();
}

void AWoTBotController::StartThinking()
{
	if (!bFrozen && !bRetiring && !bIdle && GetPawn())
	{
		const float Interval = FMath::Max(.01f, GetDefault<UWoTBotSettings>()->ThinkInterval);
		GetWorldTimerManager().SetTimer(ThinkTimer, this, &ThisClass::Think,
			Interval, true, WoTRandom::FRandRange(.01f, Interval));
	}
}

void AWoTBotController::OnUnPossess()
{
	ReleasePoolGoal();
	if (Target.IsValid()) Target->OnDamageApplied.RemoveAll(this);
	CastHold.Clear();
	if (AWoTCharacter* Body = Cast<AWoTCharacter>(GetPawn()))
	{
		Body->OnDamageApplied.RemoveAll(this);
		Body->OnCharacterMovementUpdated.RemoveDynamic(this, &ThisClass::BodyMovementUpdated);
	}
	if (Caster.IsValid()) Caster->Release();
	StopMovement();
	GetWorldTimerManager().ClearTimer(ThinkTimer);
	Target.Reset();
	State = bFrozen ? EWoTBotState::Frozen : EWoTBotState::Dead;
	SetActorTickEnabled(false);
	Super::OnUnPossess();
}

void AWoTBotController::InventoryPulse()
{
	if (bFrozen || bRetiring || bIdle) return;
	const AWoTCharacter* Body = Cast<AWoTCharacter>(GetPawn());
	if (!Body || !Body->InventoryComponent) return;
	const auto* Profile = WoTBotProfiles::Get().Find(Body->GetClass());
	if (!Profile) return;
	for (AWoTInventory* Item : Body->InventoryComponent->Items)
	{
		AWoTAngrealInventory* Artifact = Cast<AWoTAngrealInventory>(Item);
		if (!Artifact) continue;
		if (const float* Priority = Profile->Priorities.Find(Artifact->GetClass()))
			Artifact->SetBotPriorityOverride(*Priority);
	}
}

void AWoTBotController::ScheduleRespawn(float Delay)
{
	if (!HasAuthority() || bFrozen || bRetiring || bDeparturePending || bIdle) return;
	GetWorldTimerManager().SetTimer(RespawnTimer, this, &ThisClass::Restart, FMath::Max(.001f, Delay));
}

void AWoTBotController::Restart()
{
	if (!bIdle) SpawnBody();
}

void AWoTBotController::Start(bool bInitiallyIdle)
{
	SetIdle(bInitiallyIdle);
	SpawnBody(); // The initial body exists even while the server is idle.
}

void AWoTBotController::SpawnBody()
{
	if (!HasAuthority() || bFrozen || bRetiring || bDeparturePending) return;
	AWoTGameMode* Game = GetWorld()->GetAuthGameMode<AWoTGameMode>();
	if (!Game || Game->bGameEnded)
	{
		OnMatchEnded();
		return;
	}
	GetWorldTimerManager().ClearTimer(RespawnTimer);
	// Consume the retained body immediately before the restart, like a player.
	DestroyRespawnCorpse();
	Game->RestartPlayer(this);
	if (!IsValid(GetPawn())) ScheduleRespawn(1.0f);
	else InventoryPulse(); // SetPlayerDefaults grants the human kit after possession.
}

void AWoTBotController::OnParticipantPawnDied(AWoTCharacter* DeadPawn, float RespawnDelaySeconds)
{
	if (Caster.IsValid()) Caster->Release();
	GetWorldTimerManager().ClearTimer(ThinkTimer);
	if (DeadPawn)
	{
		OwnedPawns.AddUnique(DeadPawn);
		RespawnCorpse = DeadPawn;
	}
	if (Roster.IsValid()) Roster->ParticipantDied(this);
	if (IsValid(this)) ScheduleRespawn(RespawnDelaySeconds + WoTRandom::FRand());
}

void AWoTBotController::SetDeparturePending(bool bPending)
{
	if (bDeparturePending == bPending) return;
	bDeparturePending = bPending;
	if (bPending) GetWorldTimerManager().ClearTimer(RespawnTimer);
	else if (!GetPawn() && !GetWorldTimerManager().IsTimerActive(RespawnTimer)) ScheduleRespawn(1.0f);
}

void AWoTBotController::SetIdle(bool bNewIdle)
{
	if (!HasAuthority() || bFrozen || bRetiring || bIdle == bNewIdle) return;
	bIdle = bNewIdle;
	if (bIdle)
	{
		GetWorldTimerManager().ClearTimer(ThinkTimer);
		GetWorldTimerManager().ClearTimer(RespawnTimer);
		SetActorTickEnabled(false);
		ReleasePoolGoal();
		CancelCasting();
		StopMovement();
		SuspendMovement();
		State = GetPawn() ? EWoTBotState::Roam : EWoTBotState::Dead;
	}
	else if (AWoTCharacter* Body = Cast<AWoTCharacter>(GetPawn()))
	{
		ResetBehaviour(Body);
		PoolVisits.Reset();
		UncollectablePools.Reset();
		HopPool.Reset(); HopCount = 0;
		NextJumpTime = 0;
		if (Caster.IsValid()) Caster->ResetAdaptation();
		UCharacterMovementComponent* Movement = Body->GetCharacterMovement();
		if (IdleMovementMode == MOVE_None && !HasMovementHold(Body)) Movement->SetDefaultMovementMode();
		else Movement->SetMovementMode(static_cast<EMovementMode>(IdleMovementMode), IdleCustomMovementMode);
		Movement->SetComponentTickEnabled(bIdleMovementTickEnabled);
		SetActorTickEnabled(true);
		InventoryPulse();
		StartThinking();
	}
	else ScheduleRespawn(1.0f);
}

void AWoTBotController::SuspendMovement()
{
	if (AWoTCharacter* Body = Cast<AWoTCharacter>(GetPawn()))
	{
		UCharacterMovementComponent* Movement = Body->GetCharacterMovement();
		IdleMovementMode = Movement->MovementMode;
		IdleCustomMovementMode = Movement->CustomMovementMode;
		bIdleMovementTickEnabled = Movement->IsComponentTickEnabled();
		Movement->StopMovementImmediately();
		Movement->DisableMovement();
		Movement->SetComponentTickEnabled(false);
	}
}

void AWoTBotController::DestroyLivePawn()
{
	CancelCasting();
	if (AWoTCharacter* Body = Cast<AWoTCharacter>(GetPawn()))
	{
		// Clear held items first: pawn destruction otherwise drops carried seals.
		DestroyHeldItems(Body);
		UnPossess();
		Body->Destroy();
	}
	GetWorldTimerManager().ClearTimer(ThinkTimer);
}

void AWoTBotController::DestroyRespawnCorpse()
{
	if (AWoTCharacter* Corpse = RespawnCorpse.Get())
	{
		if (Corpse != GetPawn()) Corpse->Destroy();
	}
	RespawnCorpse.Reset();
}

void AWoTBotController::SilentRespawn()
{
	WOT_SOAK_BODY_SCOPE("bot silent respawn");
	if (!HasAuthority() || bFrozen || bDeparturePending || bRetiring || bIdle) return;
	const AWoTGameMode* Game = GetWorld()->GetAuthGameMode<AWoTGameMode>();
	if (!Game || Game->bGameEnded) return;
	WOT_SOAK_EVENT(SilentRespawn, this, GetPawn());
	DestroyLivePawn();
	ScheduleRespawn(Game->RespawnDelay + WoTRandom::FRand());
}

void AWoTBotController::RetireSummons()
{
	for (TActorIterator<AWoTMashadarGuide> It(GetWorld()); It; ++It)
		if (OwnedPawns.ContainsByPredicate([&It](const auto& FormerBody)
			{ return !FormerBody.IsExplicitlyNull() && FormerBody.HasSameIndexAndSerialNumber(It->SummonOwner); }))
			It->Destroy();
	for (TActorIterator<AWoTNPCCharacter> It(GetWorld()); It; ++It)
	{
		AWoTNPCCharacter* Summon = *It;
		// Compare weak identities, including owners whose former bodies were destroyed.
		const bool bOwned = OwnedPawns.ContainsByPredicate([Summon](const auto& FormerBody)
		{
			return !FormerBody.IsExplicitlyNull()
				&& FormerBody.HasSameIndexAndSerialNumber(Summon->SummonOwner);
		});
		if (!Summon->bDead && bOwned)
		{
			AController* Controller = Summon->GetController();
			DestroyHeldItems(Summon);
			if (Controller) Controller->UnPossess();
			Summon->Destroy();
			if (Controller) Controller->Destroy();
		}
	}
}

void AWoTBotController::Retire()
{
	if (!HasAuthority() || bFrozen || bRetiring) return;
	bRetiring = true;
	ReleasePoolGoal();
	GetWorldTimerManager().ClearAllTimersForObject(this);
	RetireSummons();
	DestroyLivePawn();
	DestroyRespawnCorpse();
	Destroy(); // Controller destruction calls Logout and removes its player state.
}

void AWoTBotController::OnMatchEnded()
{
	if (bFrozen) return;
	bFrozen = true;
	ReleasePoolGoal();
	State = EWoTBotState::Frozen;
	SetActorTickEnabled(false);
	CancelCasting();
	StopMovement();
	if (AWoTCharacter* Body = Cast<AWoTCharacter>(GetPawn()))
	{
		Body->GetCharacterMovement()->StopMovementImmediately();
		Body->GetCharacterMovement()->DisableMovement();
		Body->GetCharacterMovement()->SetComponentTickEnabled(false);
	}
	GetWorldTimerManager().ClearAllTimersForObject(this);
	if (Roster.IsValid()) Roster->Freeze();
}

void AWoTBotController::CancelCasting()
{
	if (const AWoTCharacter* Body = Cast<AWoTCharacter>(GetPawn()))
	{
		// Item-owned delayed activations outlive controller timers. Cancel through
		// each item's silent abort path before release can arm or fire a shot.
		const auto Items = Body->InventoryComponent->Items;
		for (AWoTInventory* Held : Items)
			if (AWoTAngrealInventory* Item = Cast<AWoTAngrealInventory>(Held); IsValid(Item))
			{
				Item->AbortCast();
				Body->InventoryComponent->NotifyCastFailed(Item);
			}
	}
	if (Caster.IsValid()) Caster->Release();
	CastHold.Clear();
	AimingItem.Reset();
}

void AWoTBotController::EndPlay(const EEndPlayReason::Type Reason)
{
	if (GetWorld() && PoolSpawnHandle.IsValid()) GetWorld()->RemoveOnActorSpawnedHandler(PoolSpawnHandle);
	PendingPoolSounds.Reset();
	ReleasePoolGoal();
	if (Target.IsValid()) Target->OnDamageApplied.RemoveAll(this);
	GetWorldTimerManager().ClearAllTimersForObject(this);
	if (Caster.IsValid()) Caster->Release();
	if (Reason == EEndPlayReason::Destroyed) DestroyRespawnCorpse();
	Super::EndPlay(Reason);
}
