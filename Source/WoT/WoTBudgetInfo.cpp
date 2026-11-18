#include "WoTBudgetInfo.h"

#include "WoTCitadelResources.h"
#include "WoTPlayerState.h"
#include "WoTSealAltar.h"
#include "WoTEvents.h"
#include "Inventory/WoTSeal.h"

#include "Engine/World.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"

namespace
{
	// The seal budget line's adapter name (BudgetInfo.Reset / PlaceSeals key off 'SealInventory').
	const FName SealInventoryName(TEXT("SealInventory"));
}

int32 AWoTBudgetInfo::GetItemCount(int32 Index) const
{
	return Budget.IsValidIndex(Index) ? Budget[Index].ItemCount : 0;
}

FName AWoTBudgetInfo::GetResourceAdapterName(int32 Index) const
{
	return Budget.IsValidIndex(Index) ? Budget[Index].ResourceAdapterName : FName();
}

FName AWoTBudgetInfo::GetResourceAdapterForTier(FName TierKeyword) const
{
	// BudgetInfo.uc:70-88 GetResourceType. Retail asserts on a miss; the port's
	// Accessed-None convention returns NAME_None, including for the empty keyword.
	if (TierKeyword.IsNone())
	{
		return NAME_None;
	}
	for (int32 Index = 0; Index < Budget.Num(); ++Index)
	{
		const FName AdapterName = Budget[Index].ResourceAdapterName;
		if (!AdapterName.IsNone()
			&& WoTCitadelResources::IsAdapterForTier(AdapterName, TierKeyword))
		{
			return AdapterName;
		}
	}
	return NAME_None;
}

int32 AWoTBudgetInfo::GetIndex(FName ResourceAdapterName) const
{
	// BudgetInfo.GetIndex: the original asserts on a miss; the port returns INDEX_NONE and the
	// callers guard (Reset / PlaceSeals skip; Increment / Decrement no-op).
	for (int32 i = 0; i < Budget.Num(); ++i)
	{
		if (Budget[i].ResourceAdapterName == ResourceAdapterName)
		{
			return i;
		}
	}
	return INDEX_NONE;
}

void AWoTBudgetInfo::Reset(int32 SealCount)
{
	// BudgetInfo.Reset: seed the seal-budget line to SealCount.
	const int32 i = GetIndex(SealInventoryName);
	if (i != INDEX_NONE)
	{
		Budget[i].ItemCount = SealCount;
	}
}

void AWoTBudgetInfo::Increment(FName ResourceName)
{
	const int32 i = GetIndex(ResourceName);
	if (i != INDEX_NONE)
	{
		Budget[i].ItemCount++;
	}
}

void AWoTBudgetInfo::Decrement(FName ResourceName)
{
	const int32 i = GetIndex(ResourceName);
	if (i != INDEX_NONE)
	{
		Budget[i].ItemCount--;
	}
}

bool AWoTBudgetInfo::BeginEdit(AController* Player)
{
	// BudgetInfo.BeginEdit: only this team may edit; cap concurrent editors at PlayerEditLimit.
	const AWoTPlayerState* PS = Player ? Player->GetPlayerState<AWoTPlayerState>() : nullptr;
	if (!HasAuthority() || !IsValid(PS) || PS->Team != Team
		|| PlayerEditCount >= PlayerEditLimit)
	{
		return false;
	}
	PlayerEditCount++;
	if (!BeginEditEvent.IsNone())
	{
		GetWorld()->GetSubsystem<UWoTEventSubsystem>()->FireEvent(
			BeginEditEvent, this, Player->GetPawn());
	}
	return true;
}

int32 AWoTBudgetInfo::SealBudgetRemaining() const
{
	const int32 Index = GetIndex(SealInventoryName);
	return Index != INDEX_NONE ? GetItemCount(Index) : 0;
}

void AWoTBudgetInfo::EndEdit(AController* Player)
{
	// BudgetInfo.EndEdit: release an editor.
	if (!HasAuthority())
	{
		return;
	}
	const AWoTPlayerState* PS = Player ? Player->GetPlayerState<AWoTPlayerState>() : nullptr;
	ensureMsgf(IsValid(PS) && PS->Team == Team,
		TEXT("BudgetInfo %s: EndEdit by a non-owning team"), *GetName());
	ensureMsgf(PlayerEditCount > 0, TEXT("BudgetInfo %s: EndEdit with no active editor"), *GetName());
	if (PlayerEditCount > 0)
	{
		PlayerEditCount--;
		if (!EndEditEvent.IsNone())
		{
			GetWorld()->GetSubsystem<UWoTEventSubsystem>()->FireEvent(
				EndEditEvent, this, Player ? Player->GetPawn() : nullptr);
		}
	}
}

int32 AWoTBudgetInfo::PlaceSeals(AWoTSealAltar* Altar, AController* Player)
{
	// BudgetInfo.PlaceSeals: spend the whole seal budget onto the altar (forced banks credit the
	// player's team). Seals are owned by the placing player's pawn so PlaceSeal reads the placer.
	if (!Altar || !Player)
	{
		return 0;
	}
	const int32 i = GetIndex(SealInventoryName);
	if (i == INDEX_NONE)
	{
		return 0;
	}
	const int32 SealCount = Budget[i].ItemCount;
	Budget[i].ItemCount = 0;

	APawn* OwnerPawn = Player->GetPawn();
	for (int32 k = 0; k < SealCount; ++k)
	{
		FActorSpawnParameters Params;
		Params.Owner = OwnerPawn;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AWoTSeal* S = GetWorld()->SpawnActor<AWoTSeal>(
			AWoTSeal::StaticClass(), GetActorLocation(), FRotator::ZeroRotator, Params);
		if (S)
		{
			Altar->PlaceSeal(S, /*bForce*/ true);
		}
	}
	return SealCount;
}
