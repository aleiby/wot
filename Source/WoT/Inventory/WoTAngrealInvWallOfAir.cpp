#include "WoTAngrealInvWallOfAir.h"

#include "WoTDestroyOnDestroyLeech.h"
#include "../Projectiles/WoTWallOfAirProjectile.h"

#include "Engine/World.h"
#include "GameFramework/Pawn.h"

AWoTAngrealInvWallOfAir::AWoTAngrealInvWallOfAir()
{
	BindStatusIcons(TEXT("I_WallOfAir"), TEXT("M_WallOfAir"));
	PrimaryActorTick.bCanEverTick = true;
	SetActorTickEnabled(false);   // only ticks while casting

	// AngrealInvWallOfAir defaultproperties.
	MaxAngle = 22.5f;
	bElementWater = true;
	bElementEarth = true;
	bElementSpirit = true;
	bUncommon = true;
	bDefensive = true;
	bCombat = true;
	RoundsPerMinute = 40.0f;
	MinInitialCharges = 10;
	MaxInitialCharges = 20;
	MaxCharges = 40;
	MaxChargesInGroup = 20;
	MinChargesInGroup = 10;
	MaxChargeUsedInterval = 1.0f;
	DeactivateSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/DeActivateWA.DeActivateWA"));
}

void AWoTAngrealInvWallOfAir::CastArtifact()
{
	APawn* OwnerPawn = Cast<APawn>(GetOwner());
	if (!OwnerPawn)
	{
		return;   // original: only WOTPlayer/WOTPawn owners can cast
	}

	AActor* A = FindBestTarget(GetTrajectorySource(), OwnerPawn->GetViewRotation(), MaxAngle);
	if (!A)
	{
		Failed();
		return;
	}

	bCasting = true;

	// Plant the wall (spawns at the caster; its Tick re-plants it to the 1/3 point).
	FActorSpawnParameters Params;
	Params.Owner = OwnerPawn;
	Params.Instigator = OwnerPawn;
	Params.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	WOA = GetWorld()->SpawnActor<AWoTWallOfAirProjectile>(
		AWoTWallOfAirProjectile::StaticClass(),
		OwnerPawn->GetActorLocation(), FRotator::ZeroRotator, Params);
	if (WOA)
	{
		WOA->SetSourceAngreal(this);
		WOA->Source = OwnerPawn;
		WOA->Destination = A;
	}

	// The inventory installs both lifetime ties here; losing either endpoint
	// removes the wall (AngrealInvWallOfAir.uc:67).
	CastorLeech = InstallDestroyLeech(WOA, OwnerPawn, false);
	VictimLeech = InstallDestroyLeech(WOA, A, true);

	if (!CastorLeech || !VictimLeech)
	{
		bCasting = false;
		UnCastArtifact();
	}

	if (bCasting)
	{
		SetActorTickEnabled(true);
		Super::CastArtifact();   // activate sound + HUD hook
		UseCharge();
	}
	else
	{
		Failed();
	}
}

UWoTDestroyOnDestroyLeech* AWoTAngrealInvWallOfAir::InstallDestroyLeech(
	AActor* DestroyActor, AActor* Victim, bool bShowIcon)
{
	APawn* VictimPawn = Cast<APawn>(Victim);
	if (!VictimPawn)
	{
		return nullptr;
	}

	UWoTDestroyOnDestroyLeech* DODL = NewObject<UWoTDestroyOnDestroyLeech>(VictimPawn);
	DODL->SetSourceAngreal(this);
	DODL->bDisplayIcon = bShowIcon;
	DODL->SetDestroyActor(DestroyActor);

	// AttachLeechEffect inlined (cf. Whirlwind/Ice/Decay): hand the pre-initialized
	// leech to the victim. Its reflector chain may refuse it (a Water/Earth/Spirit
	// shield blocks this weave) — AttachTo DestroyComponents it, and the owner
	// check below reports the failure up to Cast.
	DODL->AttachTo(VictimPawn);

	// Check for failure (original: if DODL.Owner != Victim -> destroy, return None).
	if (!IsValid(DODL) || !DODL->IsRegistered() || DODL->GetOwner() != VictimPawn)
	{
		if (IsValid(DODL))
		{
			DODL->Unattach();
		}
		return nullptr;
	}

	return DODL;
}

void AWoTAngrealInvWallOfAir::UnCastArtifact()
{
	if (bInUnCast)
	{
		return;   // UnrealScript 'singular': drop the re-entrant call
	}
	bInUnCast = true;

	if (IsValid(WOA))
	{
		// Null the handle before Destroy() so the projectile's Destroyed() -> UnCast
		// re-entry (dropped by the guard above) never sees a dangling wall.
		AWoTWallOfAirProjectile* W = WOA;
		WOA = nullptr;
		W->Destroy();
	}
	WOA = nullptr;   // "Just to be safe."

	if (IsValid(CastorLeech))
	{
		CastorLeech->Unattach();
	}
	CastorLeech = nullptr;
	if (IsValid(VictimLeech))
	{
		VictimLeech->Unattach();
	}
	VictimLeech = nullptr;

	if (bCasting)
	{
		Super::UnCastArtifact();   // deactivate sound
	}
	bCasting = false;
	SetActorTickEnabled(false);

	bInUnCast = false;
}

void AWoTAngrealInvWallOfAir::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// While the wall is up, keep pumping charges into it (RPM 40 -> 1.5s each).
	// UseCharge auto-UnCasts + GoEmptys at zero, which destroys the wall.
	if (IsValid(WOA))
	{
		CastTimer -= DeltaTime;
		while (IsValid(WOA) && CastTimer <= 0.0f)
		{
			CastTimer += (RoundsPerMinute > 0.0f ? 60.0f / RoundsPerMinute : 1.0f);
			UseCharge();
		}
	}
}
