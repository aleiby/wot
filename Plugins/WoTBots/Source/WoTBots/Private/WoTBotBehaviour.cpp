#include "WoTBotController.h"
#include "Testing/WoTSoakEvents.h"
#include "WoTBotFactionProfile.h"
#include "WoTBotSettings.h"
#include "WoTBotMetrics.h"
#include "WoTBotRoster.h"
#include "WoTBotDecision.h"
#include "AI/WoTAIUtil.h"
#include "AI/WoTCasterComponent.h"
#include "AI/WoTNPCCharacter.h"
#include "AI/WoTPathNodeIterator.h"
#include "AI/WoTSight.h"
#include "Inventory/WoTAngrealInventory.h"
#include "Inventory/WoTAngrealInvHeal.h"
#include "Inventory/WoTAngrealSpawnPool.h"
#include "Inventory/WoTInventoryComponent.h"
#include "Inventory/WoTProjectileLauncher.h"
#include "Inventory/WoTResourceSpawnerInv.h"
#include "Projectiles/WoTMashadarGuide.h"
#include "WoTCharacter.h"
#include "WoTGameMode.h"
#include "WoTPlayerState.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Navigation/PathFollowingComponent.h"
#include "NavigationSystem.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "WoTRandom.h"

bool AWoTBotController::CanAct() const
{
	const AWoTCharacter* Body = Cast<AWoTCharacter>(GetPawn());
	return HasAuthority() && !bFrozen && !bRetiring && !bDeparturePending && Body && Body->IsAlive();
}

bool AWoTBotController::IsHostile(const AWoTCharacter* Candidate) const
{
	const AWoTPlayerState* Self = GetPlayerState<AWoTPlayerState>();
	return IsValid(Candidate) && Candidate != GetPawn() && Candidate->IsAlive()
		&& Self && WoTAIUtil::ApparentTeamOf(Candidate) != Self->Team;
}

bool AWoTBotController::CanSee(const AWoTCharacter* Candidate) const
{
	const APawn* Body = GetPawn();
	if (!Body || !IsHostile(Candidate)) return false;
	const UWoTBotSettings* Settings = GetDefault<UWoTBotSettings>();
	if (FVector::DistSquared(Body->GetActorLocation(), Candidate->GetActorLocation())
		> FMath::Square(Settings->SightRange)) return false;
	const float Cone = FMath::Cos(FMath::DegreesToRadians(FMath::Clamp(Settings->SightConeDegrees, 1.0f, 359.0f) * .5f));
	if (!WoTSight::PawnCanSeeActor(Body, Candidate, Cone)) return false;
	float Radius, HalfHeight;
	Candidate->GetSimpleCollisionCylinder(Radius, HalfHeight);
	return !WoTVisBlockingTraceHits(GetWorld(), Body->GetPawnViewLocation(),
		Candidate->GetActorLocation() + FVector(0, 0, HalfHeight * .8f), Body);
}

void AWoTBotController::SetTarget(AWoTCharacter* NewTarget, bool bVisible)
{
	if (Target.Get() != NewTarget || bTargetVisible != bVisible)
	{
		ObserveCastUse();
		if (Caster.IsValid()) { Caster->Release(); Caster->ResetAdaptation(); }
		if (!CanAct()) return; // Releasing an artifact can kill or unpossess its owner.
		AimingItem.Reset();
		if (NewTarget && bVisible)
		{
			VisibleSince = GetWorld()->GetTimeSeconds();
			const float Angle = WoTRandom::FRand() * 2.0f * PI;
			AimDisk = FVector2D(FMath::Cos(Angle), FMath::Sin(Angle)) * FMath::Sqrt(WoTRandom::FRand());
		}
		NextRepathTime = 0.0f;
	}
	if (Target.Get() != NewTarget)
	{
		if (Target.IsValid()) Target->OnDamageApplied.RemoveAll(this);
		if (NewTarget) NewTarget->OnDamageApplied.AddUObject(this, &ThisClass::DamageApplied);
		LastCombatProgress = GetWorld()->GetTimeSeconds();
		LastSeenTargetHealth = 1.0f; // An unseen attacker supplies no health observation.
	}
	Target = NewTarget;
	bTargetVisible = NewTarget && bVisible;
	if (bTargetVisible)
	{
		LastKnownPosition = NewTarget->GetActorLocation();
		LastSensedTime = GetWorld()->GetTimeSeconds();
		LastCombatProgress = LastSensedTime;
		LastSeenTargetHealth = float(NewTarget->Health) / FMath::Max(1, NewTarget->MaxHealth);
	}
}

void AWoTBotController::DamageApplied(AWoTCharacter* Victim, AController* InstigatorController,
	AActor* Causer, float Amount)
{
	if (!CanAct() || Amount <= 0.0f) return;
	if (InstigatorController == this || Victim == GetPawn()) LastCombatProgress = GetWorld()->GetTimeSeconds();
	if (Victim != GetPawn()) return;
	APawn* InstigatorPawn = nullptr;
	if (InstigatorController) InstigatorPawn = InstigatorController->GetPawn();
	else if (Causer) InstigatorPawn = Causer->GetInstigator();
	AWoTCharacter* Attacker = Cast<AWoTCharacter>(InstigatorPawn);
	if (!IsHostile(Attacker)) return;
	RecentAttackers.Add(Attacker, GetWorld()->GetTimeSeconds());
	SetTarget(Attacker, CanSee(Attacker));
	if (!CanAct()) return;
	// Damage supplies one observation, never a live tracking reference through walls.
	LastKnownPosition = Attacker->GetActorLocation();
	LastSensedTime = GetWorld()->GetTimeSeconds();
	SetControlRotation((LastKnownPosition - Victim->GetPawnViewLocation()).Rotation());
}

void AWoTBotController::Perceive()
{
	const float Now = GetWorld()->GetTimeSeconds();
	const float Memory = FMath::Max(0.0f, GetDefault<UWoTBotSettings>()->TargetMemorySeconds);
	for (auto It = RecentAttackers.CreateIterator(); It; ++It)
		if (!It.Key().IsValid() || Now - It.Value() > Memory) It.RemoveCurrent();
	AWoTCharacter* Best = nullptr;
	float BestScore = TNumericLimits<float>::Max();
	TMap<const AWoTCharacter*, int32> Fighters;
	if (Roster.IsValid()) Roster->CountFighters(this, Fighters);
	for (TActorIterator<AWoTCharacter> It(GetWorld()); It; ++It)
	{
		AWoTCharacter* Candidate = *It;
		if (!CanSee(Candidate)) continue;
		float Score = FVector::Dist(GetPawn()->GetActorLocation(), Candidate->GetActorLocation());
		if (RecentAttackers.Contains(Candidate)) Score *= .6f;
		if (Candidate == Target.Get()) Score *= .8f;
		// A target two others already fight is a poor choice unless it is attacking us.
		if (!RecentAttackers.Contains(Candidate) && Fighters.FindRef(Candidate) >= 2) Score *= 4.0f;
		if (Score < BestScore) { BestScore = Score; Best = Candidate; }
	}
	if (Best) SetTarget(Best, true);
	else if (!Target.IsValid() || !Target->IsAlive() || Now - LastSensedTime >= Memory)
		SetTarget(nullptr, false);
	else SetTarget(Target.Get(), false);
}

void AWoTBotController::Think()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(WoTBotThink);
	WOT_BOT_METRICS_SCOPE();
	if (!CanAct()) return;
	++ThinkCount;
	ObserveCastUse();
	InventoryPulse();
	Perceive();
	if (!CanAct()) return;
	UpdateAim();
	UpdateCastCompletion();
	if (!CanAct()) return;
	RememberNearbyPools();
	const float Now = GetWorld()->GetTimeSeconds();
	if (State == EWoTBotState::Engage && !bTargetVisible
		&& Now - LastCombatProgress >= WoTBotDecision::NoProgressSeconds)
	{
		SetTarget(nullptr, false);
		if (!CanAct()) return;
	}
	// A body in the air has no path to anywhere: searching from it fails for every
	// pool. Keep the goal until it lands, still firing at what it sees.
	if (CastChecked<AWoTCharacter>(GetPawn())->GetCharacterMovement()->IsFalling())
	{
		CastArtifacts();
		return;
	}
	float CollectScore = 0;
	AWoTAngrealSpawnPool* Pool = BestPool(CollectScore);
	const bool CollectingItem = Pool != nullptr;
	if (!Pool)
	{
		// Running the map stays worthwhile when every reachable item is full or taken.
		Pool = BestPool(CollectScore, true);
		CollectScore *= WoTBotDecision::RoamWeight;
	}
	const bool Threat = Target.IsValid() && (RecentAttackers.Contains(Target)
		|| (bTargetVisible && FVector::DistSquared(GetPawn()->GetActorLocation(), LastKnownPosition)
			<= FMath::Square(FMath::Min(WoTBotDecision::CloseRange, GetDefault<UWoTBotSettings>()->EngageWhileSeekingRange))));
	const bool Fight = WoTBotDecision::ChooseFight(CollectScore, FightScore(),
		State == EWoTBotState::Engage, State == EWoTBotState::SeekItem, Threat);
	const EWoTBotState NextState = Fight ? EWoTBotState::Engage : CollectingItem ? EWoTBotState::SeekItem : EWoTBotState::Roam;
	if (NextState == EWoTBotState::Engage) Pool = nullptr;
	if (NextState != State || SeekingPool.Get() != Pool) NextRepathTime = 0.0f;
	if (NextState != State && Caster.IsValid())
	{
		Caster->Release();
		Caster->ResetAdaptation();
		if (!CanAct()) return;
	}
	State = NextState;
	SeekingPool = Pool;
	if (Roster.IsValid()) Roster->ClaimPool(this, Pool);
	CastArtifacts();
	if (!CanAct()) return;
	ChooseDestination();
}

void AWoTBotController::ReleasePoolGoal()
{
	if (Roster.IsValid()) Roster->ReleasePool(this);
	SeekingPool.Reset();
}

void AWoTBotController::RememberNearbyPools()
{
	const float Now = GetWorld()->GetTimeSeconds();
	for (auto It = PoolVisits.CreateIterator(); It; ++It)
		if (!It.Key().IsValid() || Now - It.Value() >= WoTBotDecision::VisitMemory) It.RemoveCurrent();
	for (TActorIterator<AWoTAngrealSpawnPool> It(GetWorld()); It; ++It)
		if ((!It->PickupItem || State == EWoTBotState::Roam)
			&& FVector::DistSquared(GetPawn()->GetActorLocation(), It->GetActorLocation()) <= FMath::Square(WoTBotDecision::ArrivalRadius))
		{
			PoolVisits.Add(*It, Now);
			if (SeekingPool.Get() == *It) ReleasePoolGoal();
		}
	if (!SeekingPool.IsValid()) return;
	// A stock change observed on our route counts as an empty visit too.
	const bool bTaken = State == EWoTBotState::SeekItem && !SeekingPool->PickupItem;
	// A pool placed off the navigation mesh is reached at its projection; an item
	// still there was not collectable from it, so move on rather than idle.
	const bool bRouteEnd = !bWantsMovement && !bNavigationUnavailable
		&& FVector::Dist2D(GetPawn()->GetActorLocation(), Destination) <= 30.0f
		&& FVector::DistSquared(Destination, SeekingPool->GetActorLocation()) <= FMath::Square(200.0f);
	if (bTaken || bRouteEnd)
	{
		PoolVisits.Add(SeekingPool, Now);
		ReleasePoolGoal();
	}
}

void AWoTBotController::OffensiveSupply(int32& Kinds, int32& Charges) const
{
	Kinds = Charges = 0;
	const AWoTCharacter* Body = Cast<AWoTCharacter>(GetPawn());
	TSet<UClass*> Classes;
	for (AWoTInventory* Held : Body->InventoryComponent->Items)
		if (const AWoTAngrealInventory* Item = Cast<AWoTAngrealInventory>(Held);
			Item && Item->bOffensive && Item->GetClass() != Body->DefaultAngrealInventory && Item->HaveEnoughCharges())
		{
			Classes.Add(Item->GetClass());
			Charges += Item->CurCharges / FMath::Max(1, Item->ChargeCost);
		}
	Kinds = Classes.Num();
}

float AWoTBotController::FightScore() const
{
	if (!Target.IsValid() || (!MovementOffence() && !ReadySummon())) return 0;
	const AWoTCharacter* Body = CastChecked<AWoTCharacter>(GetPawn());
	int32 Kinds, Charges;
	OffensiveSupply(Kinds, Charges);
	const float Distance = FVector::Dist(Body->GetActorLocation(), LastKnownPosition);
	return WoTBotDecision::Fight(Distance, bTargetVisible, RecentAttackers.Contains(Target),
		FMath::Clamp(float(Body->Health) / FMath::Max(1, Body->MaxHealth), 0.0f, 1.0f), Charges,
		LastSeenTargetHealth, Roster.IsValid() ? Roster->FightersFor(Target.Get(), this) : 0,
		bTargetVisible && Distance <= FMath::Min(WoTBotDecision::CloseRange, GetDefault<UWoTBotSettings>()->EngageWhileSeekingRange));
}

AWoTAngrealSpawnPool* AWoTBotController::BestPool(float& Score, bool bRoaming)
{
	Score = 0;
	const AWoTCharacter* Body = Cast<AWoTCharacter>(GetPawn());
	UNavigationSystemV1* Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
	const ANavigationData* Data = Nav ? Nav->GetNavDataForProps(Body->GetNavAgentPropertiesRef()) : nullptr;
	if (!Data) return nullptr;
	const float Now = GetWorld()->GetTimeSeconds();
	const FVector Start = Body->GetNavAgentLocation();
	const auto* Profile = WoTBotProfiles::Get().Find(Body->GetClass());
	int32 Kinds, Charges;
	OffensiveSupply(Kinds, Charges);
	struct FCandidate
	{
		AWoTAngrealSpawnPool* Pool = nullptr;
		FVector End = FVector::ZeroVector;
		float Priority = 0, ChargeFraction = 0, Rotation = 1, Bound = 0;
		bool bMissing = false, bOffensive = false, bClaimed = false;
	};
	const auto Value = [&](const FCandidate& C, float Length)
	{
		return bRoaming ? WoTBotDecision::Roam(Length, C.Rotation, C.bClaimed)
			: WoTBotDecision::Collect(C.Priority, C.bMissing, C.ChargeFraction, Kinds, C.bOffensive, Length, C.Rotation, C.bClaimed);
	};
	TArray<FCandidate> Candidates;
	for (TActorIterator<AWoTAngrealSpawnPool> It(GetWorld()); It; ++It)
	{
		const float* Visit = PoolVisits.Find(*It);
		if (Visit && Now - *Visit < WoTBotDecision::RevisitBlockSeconds) continue;
		if (const float* Failed = UnreachablePools.Find(*It); Failed && Now - *Failed < WoTBotDecision::UnreachableRetrySeconds) continue;
		const AWoTAngrealInventory* Item = Cast<AWoTAngrealInventory>(It->PickupItem);
		if (!bRoaming && !Item) continue;
		const AWoTAngrealInventory* Existing = nullptr;
		if (Item) for (AWoTInventory* Held : Body->InventoryComponent->Items)
			if (Held->GetClass() == Item->GetClass()) { Existing = Cast<AWoTAngrealInventory>(Held); break; }
		if (!bRoaming && Existing && Existing->CurCharges >= Existing->MaxCharges) continue;
		FNavLocation End;
		if (!Nav->ProjectPointToNavigation(It->GetActorLocation(), End, FVector(80, 80, 160), Data)) continue;
		FCandidate& C = Candidates.AddDefaulted_GetRef();
		C.Pool = *It;
		C.End = End.Location;
		C.Rotation = WoTBotDecision::Rotation(Visit ? Now - *Visit : WoTBotDecision::VisitMemory);
		C.bClaimed = Roster.IsValid() && Roster->IsPoolClaimed(*It, this);
		if (Item && !bRoaming)
		{
			const float* Priority = Profile ? Profile->Priorities.Find(Item->GetClass()) : nullptr;
			C.Priority = Priority ? *Priority : Item->Priority;
			C.bMissing = !Existing;
			C.bOffensive = Item->bOffensive;
			C.ChargeFraction = Existing ? FMath::Clamp(float(Existing->CurCharges) / FMath::Max(1, Existing->MaxCharges), 0.0f, 1.0f) : 0;
		}
		// Roaming measures "far" in a straight line. A path is never shorter than
		// that line (less a little start projection slack), so a collection value
		// at the line's length bounds the value at the path's length from above.
		const float Line = FVector::Dist(Start, C.End);
		C.Bound = Value(C, bRoaming ? Line : FMath::Max(0.0f, Line - 100.0f));
	}
	Candidates.Sort([](const FCandidate& A, const FCandidate& B) { return A.Bound > B.Bound; });
	TOptional<bool> bStartOnNavigation;
	const auto Reach = [&](const FCandidate& C, float& Length)
	{
		FPathFindingQuery Query(this, *Data, Start, C.End);
		Query.SetAllowPartialPaths(false);
		const FPathFindingResult Path = Nav->FindPathSync(Body->GetNavAgentPropertiesRef(), Query);
		if (Path.IsSuccessful() && Path.Path.IsValid()) { Length = Path.Path->GetLength(); return true; }
		// Don't search for it again every think, unless the start was the problem
		// (a body in the air has no path to anywhere).
		if (!bStartOnNavigation.IsSet())
		{
			FNavLocation Projected;
			bStartOnNavigation = Nav->ProjectPointToNavigation(Start, Projected, INVALID_NAVEXTENT, Data);
		}
		if (bStartOnNavigation.GetValue()) UnreachablePools.Add(C.Pool, Now);
		return false;
	};
	const FCandidate* Current = Candidates.FindByPredicate([this](const FCandidate& C) { return C.Pool == SeekingPool.Get(); });
	if (bRoaming && Current && !Current->bClaimed)
	{
		// Running the map keeps its pool until arrival records the visit: the pool's
		// remaining distance shrinks on the way, so re-scoring it would chase ever
		// farther pools without reaching one. Another bot's claim reopens the choice.
		// The best value on offer stands for running the map against a fight.
		Score = Candidates[0].Bound;
		return SeekingPool.Get();
	}
	// The current goal's exact value sets the bar a challenger must clear.
	float CurrentScore = 0;
	if (Current)
	{
		float Length = 0;
		if (bRoaming) CurrentScore = Current->Bound;
		else if (Reach(*Current, Length)) CurrentScore = Value(*Current, Length);
	}
	const float Bar = CurrentScore > 0 ? CurrentScore * WoTBotDecision::SwitchRatio + WoTBotDecision::SwitchMargin : 0.0f;
	AWoTAngrealSpawnPool* Best = nullptr;
	for (const FCandidate& C : Candidates)
	{
		// No remaining candidate can beat the best so far or clear the bar.
		if (C.Bound <= FMath::Max(Score, Bar)) break;
		if (C.Pool == SeekingPool.Get()) continue;
		float Length = 0;
		if (!Reach(C, Length)) continue;
		const float Candidate = bRoaming ? C.Bound : Value(C, Length);
		if (Candidate > Score) { Score = Candidate; Best = C.Pool; }
	}
	// Retain a reachable goal unless another pool is clearly worth changing route.
	if (CurrentScore > 0 && !WoTBotDecision::ClearlyBetter(Score, CurrentScore))
	{ Score = CurrentScore; return SeekingPool.Get(); }
	return Best;
}

bool AWoTBotController::HasLiveHelper() const
{
	for (TActorIterator<AWoTNPCCharacter> It(GetWorld()); It; ++It)
		if (It->IsAlive() && OwnedPawns.ContainsByPredicate([&It](const auto& Body)
			{ return !Body.IsExplicitlyNull() && Body.HasSameIndexAndSerialNumber(It->SummonOwner); })) return true;
	for (TActorIterator<AWoTMashadarGuide> It(GetWorld()); It; ++It)
		if (OwnedPawns.ContainsByPredicate([&It](const auto& Body)
			{ return !Body.IsExplicitlyNull() && Body.HasSameIndexAndSerialNumber(It->SummonOwner); })) return true;
	return false;
}

AWoTResourceSpawnerInv* AWoTBotController::ReadySummon() const
{
	const AWoTCharacter* Body = Cast<AWoTCharacter>(GetPawn());
	if (!Body || HasLiveHelper()) return nullptr;
	for (AWoTInventory* Held : Body->InventoryComponent->Items)
		if (AWoTResourceSpawnerInv* Summon = Cast<AWoTResourceSpawnerInv>(Held);
			Summon && Summon->HaveEnoughCharges() && Summon->GetPriority() > Summon->PriorityUnusable) return Summon;
	return nullptr;
}

void AWoTBotController::CastItem(AWoTAngrealInventory* Item)
{
	if (!Item || !Caster.IsValid() || !CanAct()) return;
	ObserveCastUse();
	UpdateCastCompletion();
	if (!CanAct()) return;
	// Release the previous item before changing aim or selecting its replacement.
	// The release itself may apply lethal self-damage.
	if (Caster->IsCasting())
	{
		if (AimingItem.Get() == Item) return;
		Caster->Release();
		if (!CanAct()) return;
	}
	AimingItem = Item;
	ObservedUses = Item->CastUseCount;
	UpdateAim();
	CastHold.Begin(Item, GetWorld()->GetTimeSeconds());
	Caster->BeginCast(Item);
	ObserveCastUse();
}

void AWoTBotController::UpdateCastCompletion()
{
	if (!CastHold.IsTracking()) return;
	if (!Caster.IsValid()) { CastHold.Clear(); return; }
	// The shared caster still owns charge-use release. Observe that completion
	// before selecting again, retaining its normal adapted-item exclusion.
	if (!Caster->IsCasting())
	{
		CastHold.Clear();
		return;
	}
	const float Now = GetWorld()->GetTimeSeconds();
	const bool bEffectEnded = CastHold.EffectEnded();
	const bool bBudgetExpired = CastHold.BudgetExpired(Now);
	const bool bSafetyExpired = CastHold.SafetyExpired(Now);
	if (!bEffectEnded && !bBudgetExpired && !bSafetyExpired) return;
	if (bSafetyExpired && !bEffectEnded && !bBudgetExpired)
	{
		if (const AWoTAngrealInventory* Item = CastHold.GetItem())
		{
			static TSet<FName> WarnedClasses;
			const FName ClassName = Item->GetClass()->GetFName();
			if (!WarnedClasses.Contains(ClassName))
			{
				WarnedClasses.Add(ClassName);
				UE_LOG(LogTemp, Warning, TEXT("Bot cast safety cap reached for class %s"), *ClassName.ToString());
			}
		}
	}
	// Release can fire a shot and synchronously kill/unpossess the bot. Consume
	// the hold first; callers recheck CanAct before using the pawn again.
	CastHold.Clear();
	Caster->Release();
	ObserveCastUse();
}

void AWoTBotController::ObserveCastUse()
{
	if (AimingItem.IsValid() && AimingItem->CastUseCount != ObservedUses)
	{
		ObservedUses = AimingItem->CastUseCount;
		if (AimingItem->bDefensive) LastDefensiveCastTime = GetWorld()->GetTimeSeconds();
		else LastOffensiveCastTime = GetWorld()->GetTimeSeconds();
	}
}

AWoTAngrealInventory* AWoTBotController::SelectArtifact(AActor* Victim, bool bDefensive)
{
	const auto Select = [&]()
	{
		return bDefensive ? Caster->SelectDefensiveWeave(Victim,
			[](const AWoTAngrealInventory* Item) { return !Item->IsA<AWoTAngrealInvHeal>(); })
			: Caster->SelectOffensiveWeave(Victim);
	};
	AWoTAngrealInventory* Selected = Select();
	const float LastUse = bDefensive ? LastDefensiveCastTime : LastOffensiveCastTime;
	const float Interval = bDefensive ? Caster->MinDefensiveUsageInterval : Caster->MinOffensiveUsageInterval;
	if (!Selected && !Caster->IsCasting() && AimingItem.IsValid() && AimingItem->bDefensive == bDefensive
		&& GetWorld()->GetTimeSeconds() > LastUse + Interval)
	{
		// Preserve the normal preference for another artifact. Only when that hand
		// is ready and no alternative is usable, retry without the old exclusion so
		// a single-item kit cannot lock out. Both selectors still enforce cadence.
		Caster->ResetAdaptation();
		Selected = Select();
	}
	return Selected;
}

void AWoTBotController::CastArtifacts()
{
	if (!CanAct()) return;
	AWoTCharacter* Body = Cast<AWoTCharacter>(GetPawn());
	if (!Caster.IsValid()) return;
	if (!Body->CanCastNow()) { Caster->Release(); Caster->ResetAdaptation(); return; }
	const float Now = GetWorld()->GetTimeSeconds();
	if (AActor* Incoming = Caster->FindIncomingWeave())
		if (AWoTAngrealInventory* Defence = SelectArtifact(Incoming, true))
		{ CastItem(Defence); return; }
	if (Body->Health < Body->MaxHealth * GetDefault<UWoTBotSettings>()->HealBelowHealth
		&& Now > LastDefensiveCastTime + Caster->MinDefensiveUsageInterval)
	{
		for (AWoTInventory* Held : Body->InventoryComponent->Items)
			if (AWoTAngrealInvHeal* Heal = Cast<AWoTAngrealInvHeal>(Held);
				Heal && Heal->HaveEnoughCharges() && Heal->GetPriority() > Heal->PriorityUnusable)
			{ Heal->SetVictim(Body); Heal->InitChargeGroup(); CastItem(Heal); return; }
	}
	if (!bTargetVisible || !Target.IsValid())
	{ Caster->Release(); Caster->ResetAdaptation(); return; }
	if (Now < VisibleSince + GetDefault<UWoTBotSettings>()->ReactionSeconds) return;
	if (Caster->IsCasting()) return;
	if (State == EWoTBotState::Engage && Now > LastOffensiveCastTime + Caster->MinOffensiveUsageInterval)
	{
		if (AWoTResourceSpawnerInv* Summon = ReadySummon())
		{ Summon->SetVictim(Target.Get()); Summon->InitChargeGroup(); CastItem(Summon); return; }
	}
	if (AWoTAngrealInventory* Attack = SelectArtifact(Target.Get(), false)) CastItem(Attack);
}

float AWoTBotController::GetAimErrorLimit() const
{
	return FMath::Max(0.0f, GetDefault<UWoTBotSettings>()->AimErrorDegrees)
		/ (1.0f + (bTargetVisible ? FMath::Max(0.0f, GetWorld()->GetTimeSeconds() - VisibleSince) : 0.0f));
}

void AWoTBotController::UpdateAim()
{
	APawn* Body = GetPawn();
	if (!Body) return;
	FVector AimPoint = bWantsMovement ? FVector(Destination.X, Destination.Y, Body->GetPawnViewLocation().Z)
		: Body->GetPawnViewLocation() + GetControlRotation().Vector() * 100;
	if (Target.IsValid()) AimPoint = LastKnownPosition;
	FRotator Aim = (AimPoint - Body->GetPawnViewLocation()).Rotation();
	if (bTargetVisible && Target.IsValid())
	{
		Aim = (Target->GetActorLocation() - Body->GetPawnViewLocation()).Rotation();
		if (AWoTProjectileLauncher* Launcher = Cast<AWoTProjectileLauncher>(AimingItem.Get());
			Launcher && Launcher->bOffensive && Launcher->IsLeadable())
		{
			Launcher->SetVictim(Target.Get());
			Aim = Launcher->GetBestTrajectory();
		}
		const FVector Forward = Aim.Vector();
		const FVector Right = FRotationMatrix(Aim).GetUnitAxis(EAxis::Y);
		const FVector Up = FRotationMatrix(Aim).GetUnitAxis(EAxis::Z);
		const FVector Axis = (Right * AimDisk.X + Up * AimDisk.Y).GetSafeNormal();
		Aim = FQuat(Axis, FMath::DegreesToRadians(GetAimErrorLimit() * AimDisk.Size())).RotateVector(Forward).Rotation();
	}
	SetControlRotation(Aim);
	Body->FaceRotation(FRotator(0, Aim.Yaw, 0), 0.0f);
}

void AWoTBotController::UpdateControlRotation(float, bool)
{
	// Tick owns aim, including projectile lead; the base focal-point update
	// would replace it with a look at the current path segment.
}

AWoTAngrealInventory* AWoTBotController::MovementOffence() const
{
	const AWoTCharacter* Body = Cast<AWoTCharacter>(GetPawn());
	if (!Body || !Body->InventoryComponent) return nullptr;
	AWoTAngrealInventory* Best = nullptr;
	for (AWoTInventory* Held : Body->InventoryComponent->Items)
	{
		AWoTAngrealInventory* Item = Cast<AWoTAngrealInventory>(Held);
		if (!Item || !Item->bOffensive || !Item->HaveEnoughCharges() || Item->Priority <= Item->PriorityUnusable) continue;
		// Preserve an active attack's range; otherwise approach for the best charged
		// item even before it is in selector range. Temporary cadence/group waits
		// must not make a stocked bot behave as though it has no offence.
		if (Item == AimingItem.Get() && Caster.IsValid() && Caster->IsCasting()) return Item;
		if (!Best || Item->Priority > Best->Priority) Best = Item;
	}
	return Best;
}

float AWoTBotController::StandoffRange() const
{
	AWoTAngrealInventory* Item = MovementOffence();
	if (!Item) return 0.0f;
	float Minimum, Maximum;
	Item->GetEffectiveRange(Minimum, Maximum);
	Maximum = FMath::Min(Maximum, GetDefault<UWoTBotSettings>()->SightRange * .75f);
	return FMath::Max(40.0f, FMath::Lerp(FMath::Min(Minimum, Maximum), Maximum, .65f));
}

bool AWoTBotController::NavigateTo(const FVector& Point)
{
	const AWoTCharacter* Body = Cast<AWoTCharacter>(GetPawn());
	UNavigationSystemV1* Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
	const ANavigationData* Data = Nav ? Nav->GetNavDataForProps(Body->GetNavAgentPropertiesRef()) : nullptr;
	FNavLocation Projected;
	if (!Data || !Nav->ProjectPointToNavigation(Point, Projected, FVector(100, 100, 200), Data))
	{ bNavigationUnavailable = true; return false; }
	bNavigationUnavailable = false;
	Destination = Projected.Location;
	bWantsMovement = FVector::Dist2D(Body->GetActorLocation(), Destination) > 30.0f;
	++RepathCount;
	return MoveToLocation(Destination, 20.0f, false, true, false, false, nullptr, false)
		!= EPathFollowingRequestResult::Failed;
}

void AWoTBotController::ChooseDestination(bool bForce)
{
	if (!CanAct()) return;
	const float Now = GetWorld()->GetTimeSeconds();
	if (bForce && SeekingPool.IsValid())
	{
		// Recovery abandons this route too; don't immediately reclaim the same corner.
		PoolVisits.Add(SeekingPool, Now);
		ReleasePoolGoal();
	}
	if (!bForce && Now < NextRepathTime) return;
	NextRepathTime = Now + FMath::Max(.1f, GetDefault<UWoTBotSettings>()->StrafeRepathSeconds);
	APawn* Body = GetPawn();
	if ((State == EWoTBotState::SeekItem || State == EWoTBotState::Roam) && SeekingPool.IsValid() && !bForce)
	{ NavigateTo(SeekingPool->GetActorLocation()); return; }
	if (State == EWoTBotState::Engage && !bForce)
	{
		if (!bTargetVisible) { NavigateTo(LastKnownPosition); return; }
		const FVector Away = (Body->GetActorLocation() - LastKnownPosition).GetSafeNormal2D();
		for (int32 Attempt = 0; Attempt < 4; ++Attempt)
		{
			const float Side = WoTRandom::RandBool() ? 1.0f : -1.0f;
			const FVector Offset = Away.RotateAngleAxis(Side * WoTRandom::FRandRange(25.0f, 65.0f), FVector::UpVector);
			if (NavigateTo(LastKnownPosition + Offset * StandoffRange())) return;
		}
	}
	UNavigationSystemV1* Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
	ANavigationData* Data = Nav ? Nav->GetNavDataForProps(Body->GetNavAgentPropertiesRef()) : nullptr;
	if (!Data) { bNavigationUnavailable = true; return; }
	// Unchanged fallback: no standoff point, no reachable pool, or recovery.
	// Engage relies on it in tight rooms; the Recast query keeps its engine
	// stream, which the integration fixtures seed (SeedDecisions).
	FVector Bias = Body->GetActorLocation();
	TArray<AWoTAngrealSpawnPool*> Pools;
	for (TActorIterator<AWoTAngrealSpawnPool> It(GetWorld()); It; ++It) Pools.Add(*It);
	if (!Pools.IsEmpty()) Bias = FMath::Lerp(Bias, Pools[WoTRandom::RandHelper(Pools.Num())]->GetActorLocation(), .5f);
	FNavLocation Point;
	if (Nav->GetRandomReachablePointInRadius(Bias, bForce ? 600.0f : 1600.0f, Point, Data)
		|| Nav->GetRandomReachablePointInRadius(Body->GetActorLocation(), 1600.0f, Point, Data)) NavigateTo(Point.Location);
	else bNavigationUnavailable = true;
}

void AWoTBotController::UpdateStuck()
{
	const float Now = GetWorld()->GetTimeSeconds();
	// A body falling through a gap in a floor's collision can fall until KillZ,
	// minutes away; that fall is not path progress, so the stuck rule ends it.
	const bool bFalling = CastChecked<AWoTCharacter>(GetPawn())->GetCharacterMovement()->IsFalling();
	if (!bFalling && !bNavigationUnavailable && (!bWantsMovement || FVector::Dist2D(GetPawn()->GetActorLocation(), Destination) <= 30.0f))
	{
		bWantsMovement = false;
		LastProgressTime = LastRecoveryTime = Now;
		ProgressPosition = GetPawn()->GetActorLocation();
		return;
	}
	// A capsule shuffling against a corner is not useful path progress.
	if (!bFalling && FVector::DistSquared(GetPawn()->GetActorLocation(), ProgressPosition) >= FMath::Square(75.0f))
	{
		ProgressPosition = GetPawn()->GetActorLocation();
		LastProgressTime = LastRecoveryTime = Now;
	}
	if (Now - LastProgressTime >= FMath::Max(3.0f, GetDefault<UWoTBotSettings>()->StuckRespawnSeconds))
	{ SilentRespawn(); return; }
	if (Now - LastProgressTime >= 3.0f && Now - LastRecoveryTime >= 3.0f)
	{ WOT_SOAK_EVENT(Recovery, this, GetPawn()); LastRecoveryTime = Now; ChooseDestination(true); }
}

void AWoTBotController::Tick(float DeltaSeconds)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(WoTBotTick);
	WOT_BOT_METRICS_SCOPE();
	Super::Tick(DeltaSeconds);
	if (!CanAct()) return;
	ObserveCastUse();
	if (bTargetVisible && !CanSee(Target.Get())) SetTarget(Target.Get(), false);
	if (!CanAct()) return;
	if (Caster.IsValid() && !CastChecked<AWoTCharacter>(GetPawn())->CanCastNow())
	{
		Caster->Release();
		// A landing interruption must not permanently exclude the remaining kit
		// when the same target stays visible. Usage and input cadence still apply.
		Caster->ResetAdaptation();
	}
	if (!CanAct()) return;
	UpdateAim();
	UpdateCastCompletion();
	if (!CanAct()) return;
	UpdateStuck();
}
