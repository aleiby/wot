#include "WoTAngrealInvIllusion.h"

#include "../Projectiles/WoTAngrealIllusionProjectile.h"

#include "EngineUtils.h"
#include "GameFramework/Pawn.h"

AWoTAngrealInvIllusion::AWoTAngrealInvIllusion()
{
	BindStatusIcons(TEXT("I_PersonalIllusion"), TEXT("M_PersonalIllusion"));
	// AngrealInvIllusion.uc defaultproperties.
	SpawnOffset = FVector(48.0f, 0.0f, 0.0f);
	DurationType = EWoTDurationType::DT_Lifespan;
	bElementAir = true;
	bElementSpirit = true;
	bCommon = true;
	bCombat = true;
	MaxInitialCharges = 3;
	MaxCharges = 5;
	Priority = 0.5f;
	MinChargeGroupInterval = 10.0f;
	InventoryGroup = 65;
	ActivateSound = FSoftObjectPath(
		TEXT("/Game/WoT/Sounds/Angreal/ActivatePI.ActivatePI"));
}

AWoTAngrealIllusionProjectile* AWoTAngrealInvIllusion::GetIllusion() const
{
	const APawn* OwnerPawn = Cast<APawn>(GetOwner());
	if (!IsValid(OwnerPawn) || !GetWorld())
	{
		return nullptr;
	}
	for (TActorIterator<AWoTAngrealIllusionProjectile> It(GetWorld()); It; ++It)
	{
		AWoTAngrealIllusionProjectile* Candidate = *It;
		if (IsValid(Candidate) && Candidate->GetInstigator() == OwnerPawn)
		{
			return Candidate;
		}
	}
	return nullptr;
}

bool AWoTAngrealInvIllusion::HaveIllusion() const
{
	return IsValid(GetIllusion());
}

float AWoTAngrealInvIllusion::GetPriority() const
{
	// AngrealInvIllusion.uc:89-93 deliberately returns the authored Priority
	// directly when no decoy exists (it does not call the base cooldown fold).
	return HaveIllusion() ? 0.0f : Priority;
}

void AWoTAngrealInvIllusion::CastArtifact()
{
	APawn* OwnerPawn = Cast<APawn>(GetOwner());
	if (!HasAuthority() || !IsValid(OwnerPawn) || !GetWorld())
	{
		return;
	}
	if (AWoTAngrealIllusionProjectile* Previous = GetIllusion())
	{
		Previous->Destroy();
	}

	const FRotator SpawnRotation = OwnerPawn->GetActorRotation();
	const FVector SpawnLocation = OwnerPawn->GetActorLocation()
		+ SpawnRotation.RotateVector(SpawnOffset);
	FActorSpawnParameters Params;
	Params.Owner = OwnerPawn;
	Params.Instigator = OwnerPawn;
	Params.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Illusion = GetWorld()->SpawnActor<AWoTAngrealIllusionProjectile>(
		AWoTAngrealIllusionProjectile::StaticClass(), SpawnLocation,
		SpawnRotation, Params);
	if (!IsValid(Illusion))
	{
		Failed();
		return;
	}
	Illusion->AcquireImage(OwnerPawn);
	Illusion->SetSourceAngreal(this);

	Super::CastArtifact();
	UseCharge();
}
