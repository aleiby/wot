#include "WoTKillingItem.h"

#include "WoTCharacter.h"
#include "GameFramework/Controller.h"
#include "Inventory/WoTAngrealInvBalefire.h"
#include "Inventory/WoTAngrealInvGuardian.h"
#include "Inventory/WoTAngrealInvTaint.h"
#include "Inventory/WoTAngrealInventory.h"
#include "Inventory/WoTInventory.h"
#include "Inventory/WoTInventoryComponent.h"
#include "Projectiles/WoTAngrealProjectile.h"
#include "Projectiles/WoTMashadarGuide.h"
#include "Traps/WoTFireWall.h"
#include "Traps/WoTPortcullis.h"
#include "Traps/WoTSpear.h"

namespace
{
/** Pieces that strike with no link back to the item that made them: the item
    class that places or summons each. A fire wall's slab and a portcullis's
    mover reach their trap through their owner. */
const UClass* WoTKillingItemPlacedBy(const AActor& Actor)
{
	if (Actor.IsA<AWoTFireWall>())
	{
		return AWoTFireWallInventory::StaticClass();
	}
	if (Actor.IsA<AWoTSpear>())
	{
		return AWoTSpearInventory::StaticClass();
	}
	if (Actor.IsA<AWoTPortcullis>())
	{
		return AWoTPortcullisInventory::StaticClass();
	}
	if (Actor.IsA<AWoTMashadarGuide>())
	{
		return AWoTAngrealInvGuardian::StaticClass();
	}
	return nullptr;
}
} // namespace

FName WoTKillingItem::Resolve(const AWoTCharacter& Victim, FName DamageType,
	const AActor* DamageCauser)
{
	if (DamageType == FName(TEXT("Balefire")))
	{
		return UWoTInventoryComponent::GetHandSetItemClassName(
			AWoTAngrealInvBalefire::StaticClass());
	}

	// Walk from the causer through its owners: a slab belongs to its wall, a
	// Mashadar segment to its guide, a portcullis's mover to the portcullis.
	// A pawn or a controller ends the walk, since nothing it holds struck the
	// blow. Four steps cover every chain above.
	const AActor* Actor = DamageCauser;
	for (int32 Step = 0; Step < 4 && IsValid(Actor); ++Step, Actor = Actor->GetOwner())
	{
		if (Actor->IsA<APawn>() || Actor->IsA<AController>())
		{
			break;
		}
		if (const AWoTInventory* Item = Cast<AWoTInventory>(Actor))
		{
			// A tainted item hurts its own holder with a 'SelfInflicted' blow
			// (AngrealInventory.uc:306/315). Soul Barb's blow carries the same
			// type but comes from the attacker's item.
			const AWoTAngrealInventory* Angreal = Cast<AWoTAngrealInventory>(Item);
			if (Angreal && Angreal->bTainted && Item->GetOwner() == &Victim
				&& DamageType == FName(TEXT("SelfInflicted")))
			{
				return UWoTInventoryComponent::GetHandSetItemClassName(
					AWoTAngrealInvTaint::StaticClass());
			}
			return UWoTInventoryComponent::GetHandSetItemName(Item);
		}
		if (const AWoTAngrealProjectile* Projectile =
			Cast<AWoTAngrealProjectile>(Actor))
		{
			if (IsValid(Projectile->SourceAngreal))
			{
				return UWoTInventoryComponent::GetHandSetItemName(
					Projectile->SourceAngreal);
			}
		}
		if (const UClass* PlacedBy = WoTKillingItemPlacedBy(*Actor))
		{
			return UWoTInventoryComponent::GetHandSetItemClassName(PlacedBy);
		}
	}
	return NAME_None;
}
