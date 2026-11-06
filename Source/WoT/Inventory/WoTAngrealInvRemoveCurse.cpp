#include "WoTAngrealInvRemoveCurse.h"

#include "../Projectiles/WoTAMAPearl.h"

#include "Engine/World.h"
#include "GameFramework/Pawn.h"

AWoTAngrealInvRemoveCurse::AWoTAngrealInvRemoveCurse()
{
	BindStatusIcons(TEXT("I_RemoveCurse"), TEXT("M_RemoveCurse"));
	// AngrealInvRemoveCurse defaultproperties.
	bElementFire = true;
	bElementWater = true;
	bElementAir = true;
	bElementEarth = true;
	bElementSpirit = true;
	bRare = true;
	bDefensive = true;
	bCombat = true;
	MinInitialCharges = 3;
	MaxInitialCharges = 5;
	MaxCharges = 10;
	Priority = 4.0f;
	MinChargeGroupInterval = 3.0f;
	InventoryGroup = 59;
}

void AWoTAngrealInvRemoveCurse::CastArtifact()
{
	APawn* OwnerPawn = Cast<APawn>(GetOwner());
	UWorld* World = GetWorld();
	if (!OwnerPawn || !World || !HasAuthority())
	{
		return;
	}

	// Fold RemoveCurseLeech->RCAMAPearl: drop the anti-magic dome on the caster.
	FActorSpawnParameters Params;
	Params.Owner = OwnerPawn;
	Params.Instigator = OwnerPawn;
	Params.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTAMAPearl* Pearl = World->SpawnActor<AWoTRCAMAPearl>(
		AWoTRCAMAPearl::StaticClass(), OwnerPawn->GetActorLocation(),
		FRotator::ZeroRotator, Params);
	if (!Pearl)
	{
		Failed();
		return;
	}

	// RCAMAPearl overrides: a smaller, shorter dome that rides the caster.
	Pearl->SetSourceAngreal(this);
	Pearl->AttachToActor(OwnerPawn, FAttachmentTransformRules::KeepWorldTransform);
	Pearl->Go();

	Super::CastArtifact();   // ActivateSound
	UseCharge();
}
