#include "WoTResourceSpawnerInv.h"

#include "../WoTAppearEffect.h"
#include "../WoTCharacter.h"
#include "../WoTPlayerState.h"
#include "../AI/WoTCreatureData.h"
#include "../AI/WoTNPCCharacter.h"

#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"

namespace
{
	// The troop table is keyed by the source PLAYER name (WOTPlayer.uc:7264). Map the
	// faction-class CDO identity back to that name for GetTroopRow.
	FName FactionToName(EWoTCreatureFaction Faction)
	{
		switch (Faction)
		{
		case EWoTCreatureFaction::AesSedai:   return TEXT("AesSedai");
		case EWoTCreatureFaction::Forsaken:   return TEXT("Forsaken");
		case EWoTCreatureFaction::Whitecloak: return TEXT("Whitecloak");
		case EWoTCreatureFaction::Hound:      return TEXT("Hound");
		}
		return NAME_None;
	}

	// The caster's REAL team — the value the original copies into the summon's PRI
	// (ResourceSpawner.uc:62 reads Pawn(Owner).PlayerReplicationInfo.Team, not the
	// disguised apparent team). 255 (neutral/unassigned) if the caster has no state.
	uint8 CasterTeam(const APawn* OwnerPawn)
	{
		if (const AWoTPlayerState* PS =
			    OwnerPawn ? OwnerPawn->GetPlayerState<AWoTPlayerState>() : nullptr)
		{
			return PS->Team;
		}
		return uint8(255);
	}
}

AWoTResourceSpawnerInv::AWoTResourceSpawnerInv()
{
	// ResourceSpawner defaultproperties (ResourceSpawner.uc:117-121).
	SpawnSeparation = 50.0f;
	Priority = 50.0f;
}

FName AWoTResourceSpawnerInv::GetResourceRow() const
{
	// GetResourceClass(): the troop for the caster's faction + this spawner's tier.
	const FName Tier = GetTierKeyword();
	if (Tier.IsNone())
	{
		return NAME_None;
	}
	const AWoTCharacter* OwnerCh = Cast<AWoTCharacter>(GetOwner());
	if (!OwnerCh)
	{
		return NAME_None;   // no caster -> nothing to summon
	}
	return WoTCreatureData::GetTroopRow(FactionToName(OwnerCh->GetSummonFaction()), Tier);
}

bool AWoTResourceSpawnerInv::HaveHelper() const
{
	// ResourceSpawner.HaveHelper (ResourceSpawner.uc:76-104): scan for a helper that is
	// OURS (SummonOwner), of THIS type (SummonRow == our resource row, folding
	// IsA(ResourceClass.Name)), still alive (Health > 0), and on our CURRENT team
	// (which lets a caller who changed teams summon a fresh one — ResourceSpawner.uc:96).
	const FName RowName = GetResourceRow();
	if (RowName.IsNone())
	{
		return false;
	}
	const APawn* OwnerPawn = Cast<APawn>(GetOwner());
	if (!OwnerPawn || !GetWorld())
	{
		return false;
	}
	const uint8 OwnerTeam = CasterTeam(OwnerPawn);
	for (TActorIterator<AWoTNPCCharacter> It(GetWorld()); It; ++It)
	{
		const AWoTNPCCharacter* NPC = *It;
		if (NPC->SummonOwner.Get() == OwnerPawn
			&& NPC->SummonRow == RowName
			&& NPC->IsAlive()
			&& NPC->AssignedTeam == OwnerTeam)
		{
			return true;
		}
	}
	return false;
}

void AWoTResourceSpawnerInv::CastArtifact()
{
	// ResourceSpawner.Cast (ResourceSpawner.uc:34-73).
	if (HaveHelper())
	{
		// One at a time: warn, and DON'T consume a charge (no UseCharge on this path).
		ClientMessageToOwner(LimitWarning);
		return;
	}

	const FName RowName = GetResourceRow();
	if (RowName.IsNone())
	{
		// ResourceClass == None (ResourceSpawner.uc:51) — e.g. the Guardian's deferred
		// Mashadar path. Graceful no-op: no spawn, no charge, no crash.
		return;
	}

	APawn* OwnerPawn = Cast<APawn>(GetOwner());
	UWorld* World = GetWorld();
	if (!OwnerPawn || !World)
	{
		return;
	}

	// Offset = forward * (owner radius + summon radius + separation) (ResourceSpawner.uc:53).
	const FWoTCreatureRow* Row = WoTCreatureData::Find(RowName);
	const float SummonRadius = Row ? Row->CollisionRadius : 16.0f;
	const FVector Forward = OwnerPawn->GetActorForwardVector().GetSafeNormal();
	const FVector SpawnLocation = OwnerPawn->GetActorLocation()
		+ Forward * (OwnerPawn->GetSimpleCollisionRadius() + SummonRadius + SpawnSeparation);

	// Team copied from the caster — "makes him our team's friend" (ResourceSpawner.uc:62).
	const uint8 OwnerTeam = CasterTeam(OwnerPawn);

	AWoTNPCCharacter* Helper = WoTCreatureData::SpawnCreature(
		*World, RowName, SpawnLocation, OwnerTeam, /*bCombatReady*/ true);

	if (!Helper)
	{
		Failed();   // ResourceSpawner.uc:69
		return;
	}

	CurrentArenaHelper = Helper;
	// Fold of Spawn(..., Owner, 'AngrealSpawned', ...): mark the summon as this caster's
	// so HaveHelper + owner-kill-credit find it.
	Helper->SummonOwner = Cast<AWoTCharacter>(OwnerPawn);
	Helper->SummonRow = RowName;
	// ResourceSpawner.uc:63: PRI.PlayerName = "<caster>'s <ResourceClass.Name>" —
	// retail names the TROOP class (the port's row name), not the artifact.
	if (const APlayerState* OwnerState = OwnerPawn->GetPlayerState())
	{
		Helper->SummonDisplayName = OwnerState->GetPlayerName() + TEXT("'s ")
			+ RowName.ToString();
	}
	// ResourceSpawner.uc:58-61: the sparkle AppearEffect grows around the summon.
	AWoTAppearEffect::SpawnWithSprites(*World, Helper, TopSparkle, BottomSparkle);

	Super::CastArtifact();   // AngrealInventory.Cast() — ActivateSound
	UseCharge();             // ResourceSpawner.uc:65 — one charge per successful summon
}
