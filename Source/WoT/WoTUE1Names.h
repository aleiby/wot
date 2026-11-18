#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

// Shared UnrealScript Actor.IsA(name) bridge. Native port classes normally map
// RetailName -> WoTRetailName; a few early projectile ports collapsed a leading
// "Angreal" as well (AngrealExpWardProjectile -> WoTExpWardProjectile). Keep
// that established alias here so every retail name list follows one rule.
namespace WoTUE1Names
{
	inline bool ActorIsA(const AActor* Actor, FName RetailName)
	{
		if (!IsValid(Actor) || RetailName.IsNone())
		{
			return false;
		}

		const FString Original = RetailName.ToString();
		const FName OriginalName(*Original);
		const FName PrefixedName(*(TEXT("WoT") + Original));
		FName CollapsedAngrealName = NAME_None;
		if (Original.StartsWith(TEXT("Angreal")))
		{
			// The collapse alias is only safe for LEAF names: collapsing a retail
			// FAMILY base collides with the port's own base classes ("AngrealProjectile"
			// -> "WoTProjectile" would match EVERY projectile incl. WotWeaponProjectile
			// ports like the Archer arrow, which retail excludes; same for
			// "AngrealInventory" -> "WoTInventory", the whole inventory base).
			const FString Collapsed = Original.RightChop(7);
			if (Collapsed != TEXT("Projectile") && Collapsed != TEXT("Inventory"))
			{
				CollapsedAngrealName = FName(*(TEXT("WoT") + Collapsed));
			}
		}

		for (const UClass* Class = Actor->GetClass(); IsValid(Class);
			 Class = Class->GetSuperClass())
		{
			const FName ClassName = Class->GetFName();
			if (ClassName == OriginalName || ClassName == PrefixedName
				|| (!CollapsedAngrealName.IsNone()
					&& ClassName == CollapsedAngrealName))
			{
				return true;
			}
		}
		return false;
	}
}

