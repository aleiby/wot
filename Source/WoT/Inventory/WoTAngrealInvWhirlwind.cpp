#include "WoTAngrealInvWhirlwind.h"

#include "WoTWhirlwindLeech.h"

#include "GameFramework/Pawn.h"

AWoTAngrealInvWhirlwind::AWoTAngrealInvWhirlwind()
{
	BindStatusIcons(TEXT("I_Whirlwind"), TEXT("M_Whirlwind"));
	PrimaryActorTick.bCanEverTick = true;
	SetActorTickEnabled(false);   // only ticks while casting

	// AngrealInvWhirlwind defaultproperties
	MaxAngle = 22.5f;
	DurationType = EWoTDurationType::DT_Charge;
	bElementAir = true;
	bUncommon = true;
	bOffensive = true;
	bCombat = true;
	RoundsPerMinute = 20.0f;
	MinInitialCharges = 10;
	MaxInitialCharges = 20;
	MaxCharges = 40;
	bDisplayIcon = true;
	MaxChargesInGroup = 20;
	MinChargesInGroup = 10;
	MaxChargeUsedInterval = 3.0f;
	MinChargeGroupInterval = 4.0f;
	ActivateSound = FSoftObjectPath(
		TEXT("/Game/WoT/Sounds/Angreal/ActivateWW.ActivateWW"));
}

void AWoTAngrealInvWhirlwind::CastArtifact()
{
	APawn* OwnerPawn = Cast<APawn>(GetOwner());
	if (!OwnerPawn)
	{
		return;
	}

	AActor* BestTarget = FindBestTarget(GetTrajectorySource(),
		OwnerPawn->GetViewRotation(), MaxAngle);
	APawn* Victim = Cast<APawn>(BestTarget);
	if (!Victim)
	{
		Failed();
		return;
	}

	// Give the victim a WhirlwindLeech (AttachLeechEffect inlined, cf. Ice/Decay).
	UWoTWhirlwindLeech* Leech = NewObject<UWoTWhirlwindLeech>(Victim);
	Leech->SetSourceAngreal(this);
	Leech->AttachTo(Victim);

	if (!IsValid(Leech) || !Leech->IsRegistered() || Leech->GetOwner() != Victim)
	{
		Failed();
		return;
	}

	WWLeech = Leech;
	InitialDistance = FVector::Dist(Victim->GetActorLocation(), OwnerPawn->GetActorLocation());
	bCasting = true;
	SetActorTickEnabled(true);

	Super::CastArtifact();   // sound + HUD hook
}

void AWoTAngrealInvWhirlwind::UnCastArtifact()
{
	if (!bCasting)
	{
		return;
	}
	bCasting = false;
	SetActorTickEnabled(false);

	if (IsValid(WWLeech))
	{
		WWLeech->Unattach();
	}
	WWLeech = nullptr;

	Super::UnCastArtifact();
}

void AWoTAngrealInvWhirlwind::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (!bCasting)
	{
		return;
	}

	// Lost the leech (host died / detached) -> stop.
	if (!IsValid(WWLeech))
	{
		UnCastArtifact();
		return;
	}

	// Steer the victim to a point InitialDistance ahead of the caster's view.
	if (APawn* OwnerPawn = Cast<APawn>(GetOwner()))
	{
		const FVector TargetLoc = OwnerPawn->GetActorLocation()
			+ OwnerPawn->GetViewRotation().Vector() * InitialDistance;
		WWLeech->SetDesiredLocation(TargetLoc);
	}

	// Drain charges at RoundsPerMinute (UseCharge auto-UnCasts + GoEmpty at zero).
	ChargeTimer += DeltaTime;
	while (bCasting && RoundsPerMinute > 0.0f && ChargeTimer >= (60.0f / RoundsPerMinute))
	{
		ChargeTimer -= (60.0f / RoundsPerMinute);
		UseCharge();
	}
}

bool AWoTAngrealInvWhirlwind::PlayerIsBeingWhirlwinded(APawn* Other)
{
	if (!IsValid(Other))
	{
		return false;
	}
	return Other->FindComponentByClass<UWoTWhirlwindLeech>() != nullptr;
}
