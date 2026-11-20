#include "WoTBotController.h"
#include "WoTBotDecision.h"
#include "WoTBotFactionProfile.h"
#include "WoTBotSettings.h"
#include "AI/WoTSight.h"
#include "AI/WoTPathNodeIterator.h"
#include "Inventory/WoTAngrealInventory.h"
#include "Inventory/WoTAngrealSpawnPool.h"
#include "Inventory/WoTInventoryComponent.h"
#include "WoTCharacter.h"
#include "WoTGameMode.h"
#include "WoTMapLightTable.h"
#include "WoTRandom.h"
#include "WoTSoundTable.h"
#include "World/WoTMaterializedWorldHost.h"
#include "EngineUtils.h"
#include "Misc/PackageName.h"
#include "UObject/UObjectHash.h"

namespace
{
constexpr float PoolMemorySeconds = 45.0f;
constexpr float PoolFreshSeconds = 3.0f;
constexpr int32 SightBudget = 4;
constexpr float TimingNoise = .2f;

FName SoundKey(const TSoftObjectPtr<USoundBase>& Sound)
{
	FString Path = Sound.ToSoftObjectPath().GetLongPackageName();
	return Path.RemoveFromStart(TEXT("/Game/WoT/Sounds/")) ? FName(*Path) : NAME_None;
}

uint8 RarityOf(const AWoTAngrealInventory* Item)
{ return Item->bCommon ? 1 : Item->bUncommon ? 2 : Item->bRare ? 3 : 0; }
}

void AWoTBotController::InitializePoolPerception()
{
	if (PoolSpawnHandle.IsValid()) return;
	// Independent portable stream, seeded only from the game's random source.
	PoolRandom.Initialize(int32(WoTRandom::GetUnsignedInt()));
	WardRandom.Initialize(PoolRandom.GetInitialSeed() ^ 0x57415244);
	static UClass* WardClass = FindObject<UClass>(nullptr, TEXT("/Script/WoT.WoTExpWardProjectile"));
	if (WardClass) for (TActorIterator<AActor> It(GetWorld(), WardClass); It; ++It) PerceptionWards.Add(*It);
	for (TActorIterator<AWoTAngrealSpawnPool> It(GetWorld()); It; ++It) PerceptionPools.Add(*It);
	PoolSpawnHandle = GetWorld()->AddOnActorSpawnedHandler(
		FOnActorSpawned::FDelegate::CreateUObject(this, &ThisClass::PoolActorSpawned));
}

void AWoTBotController::PoolActorSpawned(AActor* Actor)
{
	static UClass* WardClass = FindObject<UClass>(nullptr, TEXT("/Script/WoT.WoTExpWardProjectile"));
	if (WardClass && Actor->IsA(WardClass)) PerceptionWards.AddUnique(Actor);
	if (auto* Pool = Cast<AWoTAngrealSpawnPool>(Actor)) PerceptionPools.AddUnique(Pool);
	// The sound marker is a port type without a DLL class export. Resolve its
	// reflected class, as other optional port types are resolved by this plugin.
	static UClass* SoundClass = FindObject<UClass>(nullptr, TEXT("/Script/WoT.WoTSoundBurst"));
	if (SoundClass && Actor->IsA(SoundClass) && CanAct())
	{
		// Deferred spawning fills the payload after this callback. Capture the
		// listener now, then read the completed event during the next think.
		PendingPoolSounds.Add({Actor, GetPawn()->GetActorLocation(), float(GetWorld()->GetTimeSeconds()), ++PoolPerceptionOrder});
	}
}

float AWoTBotController::PoolRespawnDelay(const AWoTAngrealSpawnPool* Pool) const
{
	static UClass* TableClass = FindObject<UClass>(nullptr, TEXT("/Script/WoT.WoTMapLightTable"));
	const UWoTMapLightTable* Table = nullptr;
	for (TActorIterator<AWoTMaterializedWorldRoot> It(GetWorld()); It; ++It)
		if (It->MapLightTable) { Table = It->MapLightTable; break; }
	if (!Table && TableClass)
	{
		// Direct projection fixtures retain the registered table under the world
		// itself (WoTWorldProjector.cpp), without the materialized owner actor.
		ForEachObjectWithOuter(GetWorld(), [&Table](UObject* Object)
		{
			if (IsValid(Object) && Object->IsA(TableClass)) Table = static_cast<UWoTMapLightTable*>(Object);
		}, EGetObjectsFlags::None);
	}
	if (!Table)
	{
		const FString Map = UWorld::RemovePIEPrefix(FPackageName::GetShortName(GetWorld()->GetPackage()->GetName()));
		const FString Package = FString::Printf(TEXT("/Game/WoT/Maps/LightTables/LT_%s"), *Map);
		const FString Path = Package + TEXT(".LT_") + Map;
		UObject* Asset = FindObject<UObject>(nullptr, *Path);
		if (!Asset && FPackageName::DoesPackageExist(Package)) Asset = LoadObject<UObject>(nullptr, *Path);
		if (Asset && TableClass && Asset->IsA(TableClass)) Table = static_cast<UWoTMapLightTable*>(Asset);
	}
	const auto* Game = GetWorld()->GetAuthGameMode<AWoTGameMode>();
	return AWoTAngrealSpawnPool::ComputeRespawnDelay(Pool->RespawnTime,
		Table ? Table->IdealPlayerCount : 0, Game ? Game->GetNumCombatants() : 0);
}

bool AWoTBotController::CanSeePool(const AWoTAngrealSpawnPool* Pool) const
{
	if (!Pool) return false;
	return CanSeeStimulus(IsValid(Pool->PickupItem) ? static_cast<const AActor*>(Pool->PickupItem) : Pool);
}

bool AWoTBotController::CanSeeStimulus(const AActor* Point) const
{
	const APawn* Body = GetPawn();
	if (!Body || !Point) return false;
	const auto* Settings = GetDefault<UWoTBotSettings>();
	if (FVector::DistSquared(Body->GetActorLocation(), Point->GetActorLocation()) > FMath::Square(Settings->SightRange)) return false;
	const float Cone = FMath::Cos(FMath::DegreesToRadians(FMath::Clamp(Settings->SightConeDegrees, 1.0f, 359.0f) * .5f));
	return WoTSight::PawnCanSeeActor(Body, Point, Cone)
		&& !WoTVisBlockingTraceHits(GetWorld(), Body->GetPawnViewLocation(), Point->GetActorLocation(), Body);
}

void AWoTBotController::ObservePool(AWoTAngrealSpawnPool* Pool, bool bVisit)
{
	const float Now = GetWorld()->GetTimeSeconds();
	FWoTBotPoolBelief& Belief = PoolBeliefs.FindOrAdd(Pool);
	const auto* Item = Cast<AWoTAngrealInventory>(Pool->PickupItem);
	const float PreviousReady = Belief.EstimatedRestock >= 0 ? Belief.EstimatedRestock : Belief.EmptyRecovery;
	if (!Item && (!Belief.bEmpty || Now > PreviousReady))
	{
		// A consecutive, recent view (or the collector's own touch) witnesses a
		// disappearance. Rediscovering an empty pool later supplies no take time.
		const float Continuity = FMath::Max(.25f, GetDefault<UWoTBotSettings>()->ThinkInterval * 2.5f);
		const bool SawTake = Belief.Contents.IsValid() && Now - Belief.LastPerceived <= Continuity
			&& (bVisit || Belief.VisibleSince >= 0);
		Belief.Delay = PoolRespawnDelay(Pool);
		const float Ready = Now + Belief.Delay * PoolRandom.FRandRange(1-TimingNoise, 1+TimingNoise);
		Belief.EstimatedRestock = SawTake ? Ready : -1;
		Belief.EmptyRecovery = SawTake ? -1 : Ready;
	}
	if (Item) { Belief.EstimatedRestock = Belief.EmptyRecovery = -1; }
	Belief.bEmpty = !Item;
	Belief.Contents = Item ? Item->GetClass() : nullptr;
	Belief.Rarity = 0;
	Belief.LastPerceived = Now;
	Belief.ObservationOrder = ++PoolPerceptionOrder;
	if (!Item && SeekingPool.Get() == Pool) { PoolVisits.Add(Pool, Now); ReleasePoolGoal(); }
}

bool AWoTBotController::CanCollectPickups() const
{
	if (bIdle) return false;
	if (CanAct())
	{
		// The ordinary pickup eligibility hook runs during the real touch. This
		// is a visit, including a pickup collected between two think callbacks.
		TArray<AActor*> Touching;
		GetPawn()->GetOverlappingActors(Touching, AWoTAngrealSpawnPool::StaticClass());
		for (AActor* Actor : Touching)
			const_cast<AWoTBotController*>(this)->ObservePool(CastChecked<AWoTAngrealSpawnPool>(Actor), true);
	}
	return true;
}

void AWoTBotController::PerceivePools()
{
	const float Now = GetWorld()->GetTimeSeconds();
	for (const FPendingPoolSound& Event : PendingPoolSounds)
	{
		if (!Event.Actor.IsValid()) continue;
		const auto* Sound = static_cast<const AWoTSoundBurst*>(Event.Actor.Get());
		auto* Pool = Cast<AWoTAngrealSpawnPool>(Sound->Source);
		if (!Pool || Sound->SoundSubPath.IsNone() || Sound->Volume <= 0
			|| FVector::DistSquared(Event.Listener, Sound->GetActorLocation()) >= Sound->GetNetCullDistanceSquared()) continue;
		const uint8 Tier = Sound->SoundSubPath == SoundKey(Pool->CommonSound) ? 1
			: Sound->SoundSubPath == SoundKey(Pool->UncommonSound) ? 2
			: Sound->SoundSubPath == SoundKey(Pool->RareSound) ? 3 : 0;
		if (!Tier) continue;
		auto& Belief = PoolBeliefs.FindOrAdd(Pool);
		if (Belief.ObservationOrder > Event.Order) continue;
		Belief.LastPerceived = Event.Time;
		Belief.ObservationOrder = Event.Order;
		Belief.Contents.Reset(); Belief.bEmpty = false; Belief.Rarity = Tier;
		Belief.EstimatedRestock = Belief.EmptyRecovery = -1;
		// Fresh stock does not erase the recent visit that drives exploration.
	}
	PendingPoolSounds.Reset();
	const auto CheckSight = [this, Now](AWoTAngrealSpawnPool* Pool)
	{
		if (!Pool) return;
		auto& Belief = PoolBeliefs.FindOrAdd(Pool);
		if (!CanSeePool(Pool)) { Belief.VisibleSince = -1; return; }
		if (Belief.VisibleSince < 0) Belief.VisibleSince = Now;
		if (Now - Belief.VisibleSince >= GetDefault<UWoTBotSettings>()->ReactionSeconds) ObservePool(Pool, false);
	};
	// The route's pool gets prompt attention; other pools share a bounded budget.
	AWoTAngrealSpawnPool* Goal = SeekingPool.Get();
	CheckSight(Goal);
	for (int32 I = 0, Count = FMath::Min(SightBudget, PerceptionPools.Num()); I < Count; ++I)
	{
		if (PoolSightCursor >= PerceptionPools.Num()) PoolSightCursor = 0;
		auto* Pool = PerceptionPools[PoolSightCursor++].Get();
		if (Pool != Goal) CheckSight(Pool);
	}
	for (auto It = PoolBeliefs.CreateIterator(); It; ++It) if (!It.Key().IsValid()) It.RemoveCurrent();
	PerceptionPools.RemoveAll([](const auto& Pool) { return !Pool.IsValid(); });
}

float AWoTBotController::PoolStockProbability(const AWoTAngrealSpawnPool* Pool) const
{
	const auto* Belief = PoolBeliefs.Find(const_cast<AWoTAngrealSpawnPool*>(Pool));
	if (!Belief || !Belief->bEmpty) return 1;
	const float Now = GetWorld()->GetTimeSeconds();
	const float Ready = Belief->EstimatedRestock >= 0 ? Belief->EstimatedRestock : Belief->EmptyRecovery;
	if (Now <= Ready) return 0;
	const float Recovery = FMath::Clamp((Now-Ready) / FMath::Max(.3f, Belief->Delay*TimingNoise), 0.0f, 1.0f);
	const float Forget = FMath::Clamp((Now-Belief->LastPerceived) / PoolMemorySeconds, 0.0f, 1.0f);
	return FMath::Lerp(Recovery, 1.0f, Forget);
}

float AWoTBotController::PoolUtility(const AWoTAngrealSpawnPool* Pool, int32 OffensiveKinds) const
{
	const auto* Body = CastChecked<AWoTCharacter>(GetPawn());
	const auto* Profile = WoTBotProfiles::Get().Find(Body->GetClass());
	const auto Value = [&](UClass* Class)
	{
		const auto* Item = Class ? Cast<AWoTAngrealInventory>(Class->GetDefaultObject()) : nullptr;
		if (!Item) return 0.0f;
		const AWoTAngrealInventory* Existing = nullptr;
		for (AWoTInventory* Held : Body->InventoryComponent->Items)
			if (Held->GetClass() == Class) { Existing = Cast<AWoTAngrealInventory>(Held); break; }
		if (Existing && Existing->CurCharges >= Existing->MaxCharges) return 0.0f;
		const float* Priority = Profile ? Profile->Priorities.Find(Class) : nullptr;
		return WoTBotDecision::Collect(Priority ? *Priority : Item->Priority, !Existing,
			Existing ? FMath::Clamp(float(Existing->CurCharges)/FMath::Max(1,Existing->MaxCharges),0.0f,1.0f) : 0,
			OffensiveKinds, Item->bOffensive, 0, 1, false);
	};
	const auto* Belief = PoolBeliefs.Find(const_cast<AWoTAngrealSpawnPool*>(Pool));
	float Prior = 0, Total = 0, Heard = 0, HeardTotal = 0;
	for (int32 I = 0; I < Pool->Pool.Num() && Pool->Pool[I]; ++I)
	{
		const float Weight = Pool->Weight.IsValidIndex(I) ? FMath::Max(0, Pool->Weight[I]) : 0;
		const float Utility = Value(Pool->Pool[I]);
		Prior += Weight*Utility; Total += Weight;
		const auto* Item = Cast<AWoTAngrealInventory>(Pool->Pool[I]->GetDefaultObject());
		if (Belief && Item && Belief->Rarity && RarityOf(Item) == Belief->Rarity)
		{ Heard += Weight*Utility; HeardTotal += Weight; }
	}
	Prior = Total > 0 ? Prior/Total : 0;
	if (!Belief) return Prior;
	// Exact observations survive the gaps in the round-robin sight budget. In
	// particular, an unwanted item must not regain value between two thinks.
	const float Fresh = Belief->Contents.IsValid() ? PoolFreshSeconds : 0.0f;
	const float Confidence = 1-FMath::Clamp((GetWorld()->GetTimeSeconds()-Belief->LastPerceived-Fresh)/PoolMemorySeconds,0.0f,1.0f);
	const float Known = Belief->Contents.IsValid() ? Value(Belief->Contents.Get()) : HeardTotal > 0 ? Heard/HeardTotal : Prior;
	return FMath::Lerp(Prior, Known, Confidence)*PoolStockProbability(Pool);
}
