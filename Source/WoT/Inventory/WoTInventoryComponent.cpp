#include "WoTInventoryComponent.h"

#include "WoTAngrealInventory.h"
#include "WoTReflector.h"
#include "WoTSeal.h"
#include "WoTInventory.h"
#include "../WoTCharacter.h"
#include "../WoTOnboardingSeams.h"

#include "GameFramework/Pawn.h"
#include "Net/UnrealNetwork.h"

UWoTInventoryComponent::UWoTInventoryComponent()
{
	SetIsReplicatedByDefault(true);
}

void UWoTInventoryComponent::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME_CONDITION(UWoTInventoryComponent, Items, COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(UWoTInventoryComponent, SelectedItem, COND_OwnerOnly);
}

FName UWoTInventoryComponent::GetHandSetItemName(const AWoTInventory* Item)
{
	if (!Item)
	{
		return NAME_None;
	}
	// Generated pickup Blueprints carry presentation defaults but the shipped hand
	// table names the native AngrealInv* leaf. Peel generated classes before applying
	// the port's WoT-prefix normalization. IsNative() rather than ClassGeneratedBy:
	// the latter is editor-only data (absent in game targets and cooked builds).
	const UClass* ItemClass = Item->GetClass();
	while (ItemClass && !ItemClass->IsNative() && ItemClass->GetSuperClass())
	{
		ItemClass = ItemClass->GetSuperClass();
	}
	FString ItemClassName = ItemClass ? ItemClass->GetName() : FString();
	if (ItemClassName.StartsWith(TEXT("WoT")))
	{
		ItemClassName.RightChopInline(3);
	}
	return FName(*ItemClassName);
}

void UWoTInventoryComponent::BroadcastOnboardingItemsAdded()
{
	// Onboarding seam, behavior-neutral (the broadcast and its private dedup
	// set are the only effects): announce item classes newly present on the
	// owning client. OnRep_Inventory covers remote clients; the server-side
	// add path covers listen hosts, whose arrays never rep back to themselves.
	// An Items entry can be null while its actor channel is still opening;
	// the next OnRep after it resolves lands here again, so skipping it loses
	// nothing.
	const AWoTCharacter* CharacterOwner = Cast<AWoTCharacter>(GetOwner());
	if (!CharacterOwner || !CharacterOwner->IsLocallyControlled())
	{
		return;
	}
	for (const AWoTInventory* Item : Items)
	{
		if (!IsValid(Item))
		{
			continue;
		}
		const FName ClassName = GetHandSetItemName(Item);
		if (ClassName.IsNone()
			|| OnboardingAnnouncedClasses.Contains(ClassName))
		{
			continue;
		}
		OnboardingAnnouncedClasses.Add(ClassName);
		FWoTOnboardingSeams::Get().ClientItemAdded.Broadcast(ClassName);
	}
}

void UWoTInventoryComponent::OnRep_Inventory()
{
	if (AWoTCharacter* CharacterOwner = Cast<AWoTCharacter>(GetOwner());
		CharacterOwner && CharacterOwner->IsLocallyControlled())
	{
		CharacterOwner->ReconcileHandSet();
	}
	BroadcastOnboardingItemsAdded();
}

void UWoTInventoryComponent::OnRep_SelectedItem(AWoTInventory* PreviousSelectedItem)
{
	if (SelectedItem == PreviousSelectedItem)
	{
		return;
	}
	if (AWoTCharacter* CharacterOwner = Cast<AWoTCharacter>(GetOwner());
		CharacterOwner && CharacterOwner->IsLocallyControlled())
	{
		CharacterOwner->NotifyHandSelected();
	}
}

bool UWoTInventoryComponent::AddInventory(AWoTInventory* NewItem)
{
	// Pawn.AddInventory: skip if already in the chain; add to FRONT.
	if (!NewItem || Items.Contains(NewItem))
	{
		return false;
	}
	NewItem->SetOwner(GetOwner());
	Items.Insert(NewItem, 0);

	// WOTPlayer.AddInventory: select the first selectable pickup automatically.
	if (!SelectedItem && IsSelectable(NewItem))
	{
		SelectItem(NewItem);
	}
	if (AWoTCharacter* CharacterOwner = Cast<AWoTCharacter>(GetOwner());
		CharacterOwner && CharacterOwner->IsLocallyControlled())
	{
		CharacterOwner->NotifyItemAdded(GetHandSetItemName(NewItem));
	}
	BroadcastOnboardingItemsAdded();
	return true;
}

bool UWoTInventoryComponent::DeleteInventory(AWoTInventory* Item)
{
	if (!Item || !Items.Contains(Item))
	{
		return false;
	}
	// WOTPlayer.DeleteInventory: uncast + reset a removed angreal.
	if (AWoTAngrealInventory* Angreal = Cast<AWoTAngrealInventory>(Item))
	{
		if (Item == SelectedItem)
		{
			CeaseUsingAngreal();
		}
		Angreal->Reset();
	}
	const bool bSelectionChanged = Item == SelectedItem;
	if (bSelectionChanged)
	{
		SelectedItem = nullptr;
	}
	Items.Remove(Item);
	Item->SetOwner(nullptr);
	if (AWoTCharacter* CharacterOwner = Cast<AWoTCharacter>(GetOwner());
		CharacterOwner && CharacterOwner->IsLocallyControlled())
	{
		if (bSelectionChanged)
		{
			CharacterOwner->NotifyHandSelected();
		}
		else
		{
			CharacterOwner->ReconcileHandSet();
		}
	}
	return true;
}

AWoTInventory* UWoTInventoryComponent::FindInventoryType(
	TSubclassOf<AWoTInventory> Type) const
{
	for (AWoTInventory* Item : Items)
	{
		if (Item && Item->GetClass() == Type)
		{
			return Item;
		}
	}
	return nullptr;
}

AWoTInventory* UWoTInventoryComponent::FindInventoryName(FName ClassName) const
{
	// Matches native leaves and their generated pickup Blueprint subclasses.
	for (AWoTInventory* Item : Items)
	{
		if (!Item)
		{
			continue;
		}
		if (GetHandSetItemName(Item) == ClassName)
		{
			return Item;
		}
	}
	return nullptr;
}

bool UWoTInventoryComponent::HandlePickupQuery(AWoTInventory* Item) const
{
	// Inventory.HandlePickupQuery chain, in chain order.
	for (AWoTInventory* Existing : Items)
	{
		if (Existing && Existing->HandlePickupQuery(Item))
		{
			return true;
		}
	}
	return false;
}

void UWoTInventoryComponent::SelectItem(AWoTInventory* Item)
{
	// WOTPlayer.ServerSelectItem: stop casting, then switch.
	CeaseUsingAngreal();
	if (SelectedItem == Item)
	{
		return;
	}
	SelectedItem = Item;
	if (AWoTCharacter* CharacterOwner = Cast<AWoTCharacter>(GetOwner());
		CharacterOwner && CharacterOwner->IsLocallyControlled())
	{
		CharacterOwner->NotifyHandSelected();
	}
}

bool UWoTInventoryComponent::IsSelectable(const AWoTInventory* Item) const
{
	// WOTPlayer.uc:3931: artifacts, carried Seals, and the distinct SealInventory
	// citadel hand card are selectable; keys and other inventory are not (D19 rider).
	return Cast<const AWoTAngrealInventory>(Item) != nullptr ||
		Cast<const AWoTSeal>(Item) != nullptr ||
		Cast<const AWoTSealInventory>(Item) != nullptr;
}

void UWoTInventoryComponent::CycleSelection(int32 Direction)
{
	if (Items.Num() == 0)
	{
		return;
	}
	const int32 Start = SelectedItem ? Items.IndexOfByKey(SelectedItem) : INDEX_NONE;
	for (int32 Step = 1; Step <= Items.Num(); ++Step)
	{
		const int32 Index =
			(Start + Direction * Step + Items.Num() * Step) % Items.Num();
		if (IsSelectable(Items[Index]))
		{
			SelectItem(Items[Index]);
			return;
		}
	}
}

void UWoTInventoryComponent::NextInventory()
{
	CycleSelection(+1);
}

void UWoTInventoryComponent::PrevInventory()
{
	CycleSelection(-1);
}

bool UWoTInventoryComponent::UseAngreal()
{
	// NoCastReflector (anti-magic dome) aborts UseAngreal entirely.
	if (const AWoTCharacter* Ch = Cast<AWoTCharacter>(GetOwner()))
	{
		if (Ch->IsCastingBlocked())
		{
			return false;
		}
	}
	// The blocking pass above keeps AMA NoCast (priority 255) ahead of the
	// Distant Eye consumer (128); only an allowed cast reaches interception.
	if (DispatchCastInterception(true))
	{
		return false;
	}
	if (Cast<AWoTSeal>(SelectedItem))
	{
		if (AWoTCharacter* Character = Cast<AWoTCharacter>(GetOwner()))
		{
			Character->DropSelectedInventory();
			return true;
		}
		return false;
	}

	// DefaultCastReflector.UseAngreal
	AWoTAngrealInventory* Item = Cast<AWoTAngrealInventory>(SelectedItem.Get());
	if (Item && GetWorld()->GetTimeSeconds() > NextFireTime)
	{
		// End any cast already in progress before beginning this one
		// (DefaultCastReflector.uc:33).
		CeaseUsingAngreal();

		LastCastAngrealInventory = Item;
		bLastCastSuccess = true;   // invalidate

		Item->CastArtifact();
		// Pawn(Owner).PlayFiring() — anim arrives with the pawn-anim pass

		// Arm the rate limit only after the cast succeeds
		// (DefaultCastReflector.uc:43).
		if (bLastCastSuccess)
		{
			NextFireTime = GetWorld()->GetTimeSeconds();   // once per tick
			if (Item->bRestrictsUsage)
			{
				NextFireTime += 60.0f / Item->RoundsPerMinute;
			}
		}
		return bLastCastSuccess;
	}
	return false;
}

void UWoTInventoryComponent::CeaseUsingAngreal()
{
	// NoCastReflector aborts CeaseUsingAngreal too.
	if (const AWoTCharacter* Ch = Cast<AWoTCharacter>(GetOwner()))
	{
		if (Ch->IsCastingBlocked())
		{
			return;
		}
	}
	// Same retail ordering on release: NoCast 255, then Distant Eye 128.
	if (DispatchCastInterception(false))
	{
		return;
	}

	if (LastCastAngrealInventory)
	{
		LastCastAngrealInventory->UnCastArtifact();
	}
}

bool UWoTInventoryComponent::DispatchCastInterception(bool bUse)
{
	AWoTCharacter* Ch = Cast<AWoTCharacter>(GetOwner());
	if (!Ch)
	{
		return false;
	}

	TArray<UWoTReflector*> Reflectors;
	Ch->GetOrderedReflectors(Reflectors);
	for (UWoTReflector* Reflector : Reflectors)
	{
		if (IsValid(Reflector)
			&& (bUse ? Reflector->InterceptUseAngreal()
				: Reflector->InterceptCeaseUsingAngreal()))
		{
			return true;
		}
	}
	return false;
}

void UWoTInventoryComponent::NotifyCastFailed(AWoTAngrealInventory*)
{
	bLastCastSuccess = false;
}

void UWoTInventoryComponent::ChargeUsed(AWoTAngrealInventory* Artifact)
{
	// Reflectable ChargeUsed: fan out to the pawn's reflectors (RarityBasedDamage
	// self-damages the user).
	if (AWoTCharacter* Ch = Cast<AWoTCharacter>(GetOwner()))
	{
		Ch->NotifyChargeUsed(Artifact);
	}
}
