#include "WoTRarityBasedDamageReflector.h"

#include "WoTAngrealInventory.h"
#include "../WoTCharacter.h"
#include "../Projectiles/WoTProjectile.h"   // FWoTDamageEvent

#include "GameFramework/Controller.h"

UWoTRarityBasedDamageReflector::UWoTRarityBasedDamageReflector()
{
	Priority = 64;
	bDeleterious = true;
}

void UWoTRarityBasedDamageReflector::OnChargeUsed(AWoTAngrealInventory* Ang)
{
	if (!Ang)
	{
		return;
	}

	// Damage scales with the used artifact's rarity (TAINT_*_DAMAGE).
	int32 Damage = 0;
	if      (Ang->bCommon)   { Damage = Ang->TaintCommonDamage; }
	else if (Ang->bUncommon) { Damage = Ang->TaintUncommonDamage; }
	else if (Ang->bRare)     { Damage = Ang->TaintRareDamage; }
	if (Damage <= 0)
	{
		return;
	}

	AWoTCharacter* Host = Cast<AWoTCharacter>(GetOwner());
	if (!IsValid(Host))
	{
		return;
	}

	// The rarity damage is a DamageEffect routed through ProcessEffect
	// (RarityBasedDamageReflector.uc:40-59, SourceAngreal = the Soul Barb): a
	// Spirit shield on the host throws it away.
	if (Host->IsEffectBlocked(SourceAngreal, /*bDeleterious*/ true,
		/*bEmitFeedback*/ true))
	{
		return;
	}

	FWoTDamageEvent Ev;
	Ev.WoTDamageType = TEXT("SelfInflicted");
	Ev.UE1Momentum = FVector::ZeroVector;   // RarityBasedDamageReflector.uc:51
	Ev.HitInfo.ImpactPoint = Host->GetActorLocation();
	AController* Instigator = ReflectorInstigator ? ReflectorInstigator->GetController() : nullptr;
	Host->TakeDamage((float)Damage, Ev, Instigator, SourceAngreal);
}
