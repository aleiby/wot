#include "WoTAngrealInvTrapDetect.h"

#include "WoTTrapDetectLeech.h"

#include "GameFramework/Pawn.h"

AWoTAngrealInvTrapDetect::AWoTAngrealInvTrapDetect()
{
	BindStatusIcons(TEXT("I_TrapDetection"), TEXT("M_TrapDetection"));
	// AngrealInvTrapDetect defaultproperties.
	Duration = 15.0f;
	DurationType = EWoTDurationType::DT_Lifespan;
	Priority = 20.0f;   // inherited LeechAttacher.uc default (AI priority)
	bElementWater = true;
	bElementAir = true;
	bElementEarth = true;
	bCommon = true;
	bTraps = true;
	MinInitialCharges = 5;
	MaxInitialCharges = 15;
	MaxCharges = 20;
	MaxChargesInGroup = 20;
	MinChargesInGroup = 10;
	MinChargeGroupInterval = 6.0f;
	InventoryGroup = 55;

	ActivateSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/ActivateTD.ActivateTD"));
}

void AWoTAngrealInvTrapDetect::CastArtifact()
{
	APawn* OwnerPawn = Cast<APawn>(GetOwner());
	if (!OwnerPawn || !HasAuthority())
	{
		return;
	}

	// LeechAttacher fold: attach the detector to the caster for Duration.
	UWoTTrapDetectLeech* Leech = NewObject<UWoTTrapDetectLeech>(OwnerPawn);
	Leech->SetSourceAngreal(this);
	Leech->Lifespan = Duration;
	Leech->AttachTo(OwnerPawn);

	Super::CastArtifact();   // ActivateSound
	UseCharge();
}
