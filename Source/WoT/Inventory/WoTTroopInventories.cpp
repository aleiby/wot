#include "WoTTroopInventories.h"

#include "../AI/WoTNPCCharacter.h"

namespace
{
	void ConfigureTroopAdapter(AWoTInventory& Adapter, const TCHAR* Tier,
		const TCHAR* CreatureRow, const TCHAR* IconSuffix)
	{
		// The twelve concrete UnrealScript pawn classes are one native pawn plus a row
		// (WoTCreatureData.h:13-26). ResourceClass retains that folded spawn class while
		// CreatureRowName preserves the original leaf identity.
		Adapter.ResourceClass = AWoTNPCCharacter::StaticClass();
		Adapter.BaseResourceType = FName(Tier);
		Adapter.CreatureRowName = FName(CreatureRow);
		// Each <X>Inventory.uc authors StatusIcon=Texture'WoTPawns.UI.I_S<X>' +
		// StatusIconFrame=Texture'WoTPawns.UI.M_S<X>'. The suffix is the shipped icon
		// name, which is NOT always the creature row (MashadarInventory.uc pairs
		// I_SMashadar with the MashadarFocusPawn resource).
		Adapter.BindStatusIcons(TEXT("WoTPawns"),
			*FString::Printf(TEXT("I_S%s"), IconSuffix),
			*FString::Printf(TEXT("M_S%s"), IconSuffix));
	}
}

AWoTWarderInventory::AWoTWarderInventory()
{
	// WarderInventory.uc defaultproperties.
	ConfigureTroopAdapter(*this, TEXT("Grunt"), TEXT("Warder"), TEXT("Warder"));
}

AWoTSisterInventory::AWoTSisterInventory()
{
	// SisterInventory.uc defaultproperties.
	ConfigureTroopAdapter(*this, TEXT("Captain"), TEXT("Sister"), TEXT("Sister"));
}

AWoTSitterInventory::AWoTSitterInventory()
{
	// SitterInventory.uc defaultproperties; Quote is intentionally unauthored.
	ConfigureTroopAdapter(*this, TEXT("Champion"), TEXT("Sitter"), TEXT("Sitter"));
}

AWoTTrollocInventory::AWoTTrollocInventory()
{
	// TrollocInventory.uc: ResourceClass=Class'WoTPawns.TrollocStrong'.
	ConfigureTroopAdapter(*this, TEXT("Grunt"), TEXT("TrollocStrong"), TEXT("Trolloc"));
}

AWoTMyrddraalInventory::AWoTMyrddraalInventory()
{
	// MyrddraalInventory.uc defaultproperties.
	ConfigureTroopAdapter(*this, TEXT("Captain"), TEXT("Myrddraal"), TEXT("Myrddraal"));
}

AWoTBATrollocInventory::AWoTBATrollocInventory()
{
	// BATrollocInventory.uc defaultproperties.
	ConfigureTroopAdapter(*this, TEXT("Champion"), TEXT("BATrolloc"), TEXT("BATrolloc"));
}

AWoTMinionInventory::AWoTMinionInventory()
{
	// MinionInventory.uc defaultproperties.
	ConfigureTroopAdapter(*this, TEXT("Grunt"), TEXT("Minion"), TEXT("Minion"));
}

AWoTMashadarInventory::AWoTMashadarInventory()
{
	// MashadarInventory.uc defaultproperties.
	ConfigureTroopAdapter(*this, TEXT("Captain"), TEXT("MashadarFocusPawn"), TEXT("Mashadar"));
}

AWoTLegionInventory::AWoTLegionInventory()
{
	// LegionInventory.uc defaultproperties.
	ConfigureTroopAdapter(*this, TEXT("Champion"), TEXT("Legion"), TEXT("Legion"));
}

AWoTSoldierInventory::AWoTSoldierInventory()
{
	// SoldierInventory.uc defaultproperties.
	ConfigureTroopAdapter(*this, TEXT("Grunt"), TEXT("Soldier"), TEXT("Soldier"));
}

AWoTArcherInventory::AWoTArcherInventory()
{
	// ArcherInventory.uc defaultproperties.
	ConfigureTroopAdapter(*this, TEXT("Captain"), TEXT("Archer"), TEXT("Archer"));
}

AWoTQuestionerInventory::AWoTQuestionerInventory()
{
	// QuestionerInventory.uc defaultproperties.
	ConfigureTroopAdapter(*this, TEXT("Champion"), TEXT("Questioner"), TEXT("Questioner"));
}
