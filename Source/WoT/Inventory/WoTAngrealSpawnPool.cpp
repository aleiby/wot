#include "WoTAngrealSpawnPool.h"
#include "../WoTRandom.h"

#include "WoTAngrealInventory.h"
#include "../WoTMapLightTable.h"
#include "../WoTMeshLight.h"
#include "../WoTSound.h"
#include "../WoTSoundTable.h"
#include "Engine/World.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/Pawn.h"
#include "TimerManager.h"

AWoTAngrealSpawnPool::AWoTAngrealSpawnPool()
{
	bRotatingPickup = false;   // the pool is invisible; no pickup spin
	CommonSound = TSoftObjectPtr<USoundBase>(FSoftObjectPath(
		TEXT("/Game/WoT/Sounds/Angreal/RespawnA.RespawnA")));
	UncommonSound = TSoftObjectPtr<USoundBase>(FSoftObjectPath(
		TEXT("/Game/WoT/Sounds/Angreal/RespawnB.RespawnB")));
	RareSound = TSoftObjectPtr<USoundBase>(FSoftObjectPath(
		TEXT("/Game/WoT/Sounds/Angreal/RespawnC.RespawnC")));
}

float AWoTAngrealSpawnPool::ComputeRespawnDelay(float InRespawnTime,
	int32 IdealPlayerCount, int32 NumPlayers)
{
	const int32 Divisor = FMath::Max(1,
		NumPlayers / FMath::Max(IdealPlayerCount, 1));
	return FMath::Max(InRespawnTime, 0.0f) / Divisor + 0.3f;
}

void AWoTAngrealSpawnPool::BeginPlay()
{
	Super::BeginPlay();
	// UE1 bHidden — the pool never draws, but its collision stays live so the co-located
	// spawned pickup is collected by touching the pool. Hide on ALL roles: UE5 does not re-hide
	// on the native bHidden bit when it replicates (same reason ItemState needs an OnRep), so a
	// server-only hide leaves the pool visible on clients.
	SetActorHiddenInGame(true);
	// A locally materialized net-startup actor can enter BeginPlay before
	// InitializeNetworkActors demotes its client copy to SimulatedProxy. NetMode
	// already identifies that world, so include it in the authority gate and do
	// not create a one-frame ghost pickup on joining clients.
	if (HasAuthority() && GetNetMode() != NM_Client)
	{
		// The level-startup spawn is silent (see SpawnFromPool's declaration).
		SpawnFromPool(/*bAnnounce*/ false);
	}
}

int32 AWoTAngrealSpawnPool::GetWeightedSelection() const
{
	int32 TotalWeight = 0;
	for (int32 i = 0; i < Pool.Num(); ++i)
	{
		if (!Pool[i])
		{
			break;
		}
		TotalWeight += Weight.IsValidIndex(i) ? Weight[i] : 0;
	}
	if (TotalWeight <= 0)
	{
		return INDEX_NONE;   // pool empty / all angreal depleted -> shut down
	}

	const int32 RequiredWeight = WoTRandom::RandRange(0, TotalWeight - 1);
	int32 Accum = 0;
	for (int32 i = 0; i < Pool.Num(); ++i)
	{
		if (!Pool[i])
		{
			break;
		}
		Accum += Weight.IsValidIndex(i) ? Weight[i] : 0;
		if (Accum > RequiredWeight)
		{
			return i;
		}
	}
	return INDEX_NONE;
}

void AWoTAngrealSpawnPool::SpawnFromPool(bool bAnnounce)
{
	LastAnnouncedSoundForTest = NAME_None;
	const int32 Index = GetWeightedSelection();
	if (Index == INDEX_NONE)
	{
		Destroy();   // Warn "is empty. Shutting down."
		return;
	}

	// AngrealSpawnPool.uc:69-75: Spawn, RespawnTime = 0, SetCollision(false,...)
	// — both before anything can touch the item (deferred: the initial overlap
	// UE5 raises at spawn would otherwise hand the item to a pawn standing in
	// the pool before the pool's own handoff; see bCollisionHeldByPool).
	const FTransform SpawnTransform(GetActorRotation(), GetActorLocation());
	AWoTInventory* Item = GetWorld()->SpawnActorDeferred<AWoTInventory>(
		Pool[Index], SpawnTransform, nullptr, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!Item)
	{
		return;
	}
	Item->RespawnTime = 0.0f;               // the pool owns respawn, not the item
	Item->bCollisionHeldByPool = true;      // the pool owns the Touch
	Item->FinishSpawning(SpawnTransform);
	if (!IsValid(Item))
	{
		return;
	}

	PickupItem = Item;
	Item->SetActorEnableCollision(false);
	if (bAnnounce && HasAuthority())
	{
		// Retail's variable names spell Commmon/UnCommmon; these clean names map
		// Common -> RespawnA, Uncommon -> RespawnB, Rare -> RespawnC.
		if (const AWoTAngrealInventory* Angreal = Cast<AWoTAngrealInventory>(Item))
		{
			const TSoftObjectPtr<USoundBase>* Sound = Angreal->bCommon ? &CommonSound
				: Angreal->bUncommon ? &UncommonSound
				: Angreal->bRare ? &RareSound : nullptr;
			const FName SubPath = Sound ? WoTSound::SubPathOf(*Sound) : NAME_None;
			if (!SubPath.IsNone())
			{
				// AngrealSpawnPool.uc:78-91 PlaySound(sound): a server PlaySound,
				// heard by each player in range (UE1's native PlaySound) at volume 1
				// with the unset radius 1600 and UE1's linear falloff.
				WoTSound::PlayReplicated(*GetWorld(), SubPath, GetActorLocation(),
					WoTSound::DefaultPlaySoundRadius);
				LastAnnouncedSoundForTest = SubPath;
			}
		}
	}

	// A camper standing in the pool gets the fresh spawn instantly — begin-overlap
	// never re-fires for someone already inside.
	CheckTouching();
}

void AWoTAngrealSpawnPool::CheckTouching()
{
	// AngrealSpawnPool.uc:55-62: Touch() every pawn already in our Touching list.
	TArray<AActor*> Overlapping;
	GetOverlappingActors(Overlapping, APawn::StaticClass());
	for (AActor* Other : Overlapping)
	{
		if (!IsValid(PickupItem))
		{
			break;   // handed off (or pool destroyed) — nothing left to collect
		}
		HandlePoolTouch(Other);
	}
}

void AWoTAngrealSpawnPool::OnPickupOverlap(UPrimitiveComponent*, AActor* Other,
                                           UPrimitiveComponent*, int32, bool,
                                           const FHitResult&)
{
	if (!IsValid(PickupItem))
	{
		return;
	}
	HandlePoolTouch(Other);
}

void AWoTAngrealSpawnPool::HandlePoolTouch(AActor* Other)
{
	// state Active, function Touch: hand the pickup off to a valid player toucher.
	if (!HasAuthority() || !IsValid(PickupItem))
	{
		return;
	}
	if (!ValidTouch(Other))
	{
		return;
	}

	AWoTInventory* Item = PickupItem;
	PickupItem = nullptr;
	Item->RespawnTime = 0.0f;
	// Hand the touch to the ITEM (retail PickupItem.Touch(Other)) so its own
	// ValidTouch/PickupQuery runs — an AngrealInventory the toucher already holds
	// steals the duplicate's charges instead of granting a second copy. We leave the
	// item's collision disabled (as the pool set it): re-enabling here would fire the
	// item's own overlap first, self-picking-up, so this call would then see a
	// duplicate and charge-steal+destroy the just-granted item.
	Item->TryPickupBy(Other);

	if (bRespawn)
	{
		// AngrealSpawnPool.uc:151 divides by the integer player-load ratio.
		const UWoTMapLightTable* Table = UWoTMeshLight::FindMapLightTable(GetWorld());
		AGameModeBase* GameMode = GetWorld()->GetAuthGameMode();
		const int32 Ideal = Table ? Table->IdealPlayerCount : 0;
		const int32 NumPlayers = GameMode ? GameMode->GetNumPlayers() : 0;
		const float Delay = ComputeRespawnDelay(RespawnTime, Ideal, NumPlayers);
		GetWorldTimerManager().SetTimer(RespawnTimer, this,
			&AWoTAngrealSpawnPool::RespawnPool, Delay);
	}
	else
	{
		Destroy();
	}
}

void AWoTAngrealSpawnPool::RespawnPool()
{
	SpawnFromPool(/*bAnnounce*/ true);
}
