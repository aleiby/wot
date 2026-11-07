#include "WoTHandInfo.h"

#include "WoTInventoryComponent.h"
#include "WoTInventory.h"
#include "Engine/World.h"

void UWoTHandInfo::AddClassName(FName NewClassName, int32 Slot)
{
	check(Slot >= 0 && Slot < NumSlots);
	for (int32 Index = 0; Index < NumSlots; ++Index)
	{
		check(ClassName[Index] != NewClassName);
	}
	ClassName[Slot] = NewClassName;
}

void UWoTHandInfo::AddItem(FName ItemName, bool bSelect, UWoTInventoryComponent* Inv)
{
	const bool bPreviouslyEmpty = IsEmpty();
	for (int32 Index = 0; Index < NumSlots; ++Index)
	{
		if (ClassName[Index] == ItemName)
		{
			bHaveThis[Index] = true;
			Item[Index] = Inv ? Inv->FindInventoryName(ItemName) : nullptr;
			ItemAdded = ItemName;
			ItemAddedTime = Inv && Inv->GetWorld() ? Inv->GetWorld()->GetTimeSeconds() : 0.0f;
			if (bPreviouslyEmpty || bSelect)
			{
				Select(Index);
			}
			break;
		}
	}
}

void UWoTHandInfo::RemoveItem(FName ItemName)
{
	for (int32 Index = 0; Index < NumSlots; ++Index)
	{
		if (ClassName[Index] == ItemName)
		{
			Item[Index] = nullptr;
			bHaveThis[Index] = false;
			break;
		}
	}
}

void UWoTHandInfo::Update(UWoTInventoryComponent* Inv)
{
	for (int32 Index = 0; Index < NumSlots; ++Index)
	{
		if (bHaveThis[Index])
		{
			Item[Index] = Inv ? Inv->FindInventoryName(ClassName[Index]) : nullptr;
		}
	}
}

void UWoTHandInfo::SetDisplayCount(FName ItemName, int32 NewCount)
{
	for (int32 Index = 0; Index < NumSlots; ++Index)
	{
		if (ClassName[Index] == ItemName)
		{
			DisplayCount[Index] = FMath::Max(NewCount, 0);
			bHasDisplayCount[Index] = true;
			return;
		}
	}
}

int32 UWoTHandInfo::GetDisplayCount(int32 Index) const
{
	check(Index >= 0 && Index < NumSlots);
	return bHasDisplayCount[Index] ? DisplayCount[Index] : INDEX_NONE;
}

void UWoTHandInfo::Select(int32 Index)
{
	check(Index >= 0 && Index < NumSlots);
	Selected = Index;
}

void UWoTHandInfo::SelectItem(FName ItemName)
{
	for (int32 Index = 0; Index < NumSlots; ++Index)
	{
		if (ClassName[Index] == ItemName)
		{
			Select(Index);
			break;
		}
	}
}

void UWoTHandInfo::SelectFirst()
{
	int32 Index = 0;
	if (!IsEmpty())
	{
		while (Index < NumSlots && GetClassName(Index).IsNone())
		{
			++Index;
		}
	}
	Select(Index);
}

void UWoTHandInfo::SelectLast()
{
	int32 Index = NumSlots - 1;
	if (!IsEmpty())
	{
		while (Index > 0 && GetClassName(Index).IsNone())
		{
			--Index;
		}
	}
	Select(Index);
}

void UWoTHandInfo::SelectNext()
{
	int32 Index = Selected;
	do
	{
		Index = (Index + 1) % NumSlots;
	}
	while (Index != Selected && GetClassName(Index).IsNone());
	Select(Index);
}

void UWoTHandInfo::SelectPrevious()
{
	int32 Index = Selected;
	do
	{
		--Index;
		if (Index < 0)
		{
			Index = NumSlots - 1;
		}
	}
	while (Index != Selected && GetClassName(Index).IsNone());
	Select(Index);
}

FName UWoTHandInfo::GetClassName(int32 Index) const
{
	check(Index >= 0 && Index < NumSlots);
	return bHaveThis[Index] ? ClassName[Index] : NAME_None;
}

FName UWoTHandInfo::GetSelectedClassName() const
{
	return GetClassName(Selected);
}

AWoTInventory* UWoTHandInfo::GetItem(int32 Index) const
{
	check(Index >= 0 && Index < NumSlots);
	return Item[Index].Get();
}

AWoTInventory* UWoTHandInfo::GetSelectedItem() const
{
	return GetItem(Selected);
}

int32 UWoTHandInfo::GetItemCount() const
{
	int32 ItemCount = 0;
	for (int32 Index = 0; Index < NumSlots; ++Index)
	{
		ItemCount += bHaveThis[Index] ? 1 : 0;
	}
	return ItemCount;
}

bool UWoTHandInfo::IsEmpty() const
{
	return GetItemCount() == 0;
}

bool UWoTHandInfo::IsIn(FName ItemName) const
{
	for (int32 Index = 0; Index < NumSlots; ++Index)
	{
		if (GetClassName(Index) == ItemName)
		{
			return true;
		}
	}
	return false;
}
