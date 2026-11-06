#include "WoTHandSet.h"

#include "WoTAngrealHandRow.h"
#include "WoTHandInfo.h"
#include "Engine/DataTable.h"
#include "../World/WoTRuntimeClassDefaults.h"

void UWoTHandSet::Init(UObject* Outer)
{
	UObject* HandOuter = Outer ? Outer : this;
	for (int32 Index = 0; Index < NumHands; ++Index)
	{
		if (!Hands[Index])
		{
			Hands[Index] = NewObject<UWoTHandInfo>(HandOuter);
		}
	}
}

void UWoTHandSet::SetHand(int32 Index, UWoTHandInfo* Hand)
{
	check(Index >= 0 && Index < NumHands);
	check(!Hands[Index]);
	Hands[Index] = Hand;
}

UWoTHandInfo* UWoTHandSet::GetHand(int32 Index) const
{
	check(Index >= 0 && Index < NumHands);
	return Hands[Index];
}

UWoTHandInfo* UWoTHandSet::GetSelectedHand() const
{
	return GetHand(Selected);
}

void UWoTHandSet::Select(int32 Index)
{
	check(Index >= 0 && Index < NumHands);
	UWoTHandInfo* RequestedHand = GetHand(Index);
	check(RequestedHand);
	if (RequestedHand->IsEmpty())
	{
		return;
	}
	if (Selected != Index)
	{
		Selected = Index;
		if (GetSelectedHand()->GetSelectedClassName().IsNone())
		{
			GetSelectedHand()->SelectNext();
		}
	}
	else
	{
		GetSelectedHand()->SelectNext();
	}
}

void UWoTHandSet::SelectItem(FName ItemName)
{
	for (int32 Index = 0; Index < NumHands; ++Index)
	{
		if (Hands[Index] && Hands[Index]->IsIn(ItemName))
		{
			Select(Index);
			Hands[Index]->SelectItem(ItemName);
			break;
		}
	}
}

int32 UWoTHandSet::GetNext() const
{
	int32 Index = Selected;
	do
	{
		Index = (Index + 1) % NumHands;
	}
	while (Index != Selected && Hands[Index] && Hands[Index]->IsEmpty());
	return Index;
}

int32 UWoTHandSet::GetPrevious() const
{
	int32 Index = Selected;
	do
	{
		--Index;
		if (Index < 0)
		{
			Index = NumHands - 1;
		}
	}
	while (Index != Selected && Hands[Index] && Hands[Index]->IsEmpty());
	return Index;
}

void UWoTHandSet::AddItem(FName ItemName, UWoTInventoryComponent* Inv)
{
	for (int32 Index = 0; Index < NumHands; ++Index)
	{
		if (Hands[Index])
		{
			Hands[Index]->AddItem(ItemName, Index != Selected, Inv);
		}
	}
}

void UWoTHandSet::RemoveItem(FName ItemName)
{
	for (UWoTHandInfo* Hand : Hands)
	{
		if (Hand)
		{
			Hand->RemoveItem(ItemName);
		}
	}
}

void UWoTHandSet::Update(UWoTInventoryComponent* Inv)
{
	for (UWoTHandInfo* Hand : Hands)
	{
		if (Hand)
		{
			Hand->Update(Inv);
		}
	}
}

void UWoTHandSet::PopulateFromTable(const UDataTable* Table)
{
	if (!Table)
	{
		return;
	}
	TArray<FWoTAngrealHandRow*> Rows;
	Table->GetAllRows(TEXT("UWoTHandSet::PopulateFromTable"), Rows);
	for (const FWoTAngrealHandRow* Row : Rows)
	{
		if (Row && Row->Hand >= 0 && Row->Hand < NumHands &&
			Row->Slot >= 0 && Row->Slot < UWoTHandInfo::NumSlots &&
			!Row->AngrealClass.IsNone())
		{
			check(Hands[Row->Hand]);
			Hands[Row->Hand]->AddClassName(Row->AngrealClass, Row->Slot);
		}
	}
}

int32 UWoTHandSet::PopulateFromRuntimeClassDefaults(
	const FWoTRuntimeClassDefaults& Defaults)
{
	// WOTPlayer.uc:237 `var globalconfig name AngrealHandOrder[100]`;
	// CreateAngrealHands reads AngrealHandOrder[i * Hand.GetArrayCount() + j]
	// for hand i, slot j, skipping ''.
	int32 Added = 0;
	for (int32 HandIndex = 0; HandIndex < NumHands; ++HandIndex)
	{
		for (int32 SlotIndex = 0; SlotIndex < UWoTHandInfo::NumSlots; ++SlotIndex)
		{
			const FWoTRuntimeWorldProperty* Entry = Defaults.FindClassProperty(
				TEXT("WOT.WOTPlayer"), TEXT("AngrealHandOrder"),
				static_cast<uint32>(HandIndex * UWoTHandInfo::NumSlots + SlotIndex));
			if (Entry == nullptr
				|| Entry->Type != EWoTRuntimeWorldPropertyType::Name
				|| Entry->NameValue.IsEmpty()
				|| Entry->NameValue.Equals(TEXT("None"), ESearchCase::IgnoreCase))
			{
				continue;
			}
			check(Hands[HandIndex]);
			Hands[HandIndex]->AddClassName(FName(*Entry->NameValue), SlotIndex);
			++Added;
		}
	}
	return Added;
}
