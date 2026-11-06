#include "WoTBagHolding.h"

#include "WoTAngrealInventory.h"
#include "../WoTGameMode.h"

#include "GameFramework/Pawn.h"

AWoTBagHolding::AWoTBagHolding()
{
	PickupSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/WOT/Pickup.Pickup"));
}

bool AWoTBagHolding::AddItem(AWoTInventory* Item)
{
	if (!HasAuthority() || !IsValid(Item) || Items.Num() >= MaxItems)
	{
		return false;
	}
	Item->BecomeItem();
	Items.Add(Item);
	if (AWoTAngrealInventory* Angreal = Cast<AWoTAngrealInventory>(Item))
	{
		Angreal->NotifyPutInBag(this);
	}
	return true;
}

void AWoTBagHolding::GiveTo(APawn* Other)
{
	if (!HasAuthority() || !IsValid(Other) || bDestroyingBag)
	{
		return;
	}
	if (!bInitialized)
	{
		WaitingPawn = Other;
		return;
	}

	AWoTGameMode* Game = GetWorld()->GetAuthGameMode<AWoTGameMode>();
	for (AWoTInventory* Item : Items)
	{
		if (!IsValid(Item))
		{
			continue;
		}
		if (!Game || Game->PickupQuery(Other, Item))
		{
			Item->GiveTo(Other);
		}
		else
		{
			Item->Destroy();
		}
	}
	Items.Reset();

	bDestroyingBag = true;
	SetActorHiddenInGame(true);
	SetActorEnableCollision(false);
	SetActorTickEnabled(false);
	GetWorldTimerManager().SetTimer(
		DestroyTimer, this, &AWoTBagHolding::DestroyBag, 0.2f, false);
}

void AWoTBagHolding::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (!HasAuthority() || bDestroyingBag)
	{
		return;
	}
	if (!bInitialized)
	{
		bInitialized = true;
	}
	if (IsValid(WaitingPawn))
	{
		APawn* Recipient = WaitingPawn;
		WaitingPawn = nullptr;
		GiveTo(Recipient);
	}
}

void AWoTBagHolding::Destroyed()
{
	// A bag can leave the world unlooted (a mover's encroachment destroys unowned
	// pickups, WoTMover.cpp). Its contents are hidden held actors that nothing else
	// references, so they go with it. Seals are never bagged (Drop and Died drop
	// them directly), so this never destroys a match objective.
	if (HasAuthority())
	{
		const TArray<TObjectPtr<AWoTInventory>> Contents = MoveTemp(Items);
		Items.Reset();
		for (AWoTInventory* Item : Contents)
		{
			if (IsValid(Item) && !Item->IsActorBeingDestroyed())
			{
				Item->Destroy();
			}
		}
	}
	Super::Destroyed();
}

void AWoTBagHolding::DestroyBag()
{
	Destroy();
}
