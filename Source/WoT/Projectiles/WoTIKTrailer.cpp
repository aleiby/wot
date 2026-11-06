#include "WoTIKTrailer.h"
#include "../WoTRandom.h"

AWoTIKTrailer::AWoTIKTrailer()
{
	PrimaryActorTick.bCanEverTick = true;
	// Not a replicated actor (IKTrailer.uc defaults RemoteRole=ROLE_None): each machine
	// spawns its own local trailer chain and solves the IK from the guide's replicated
	// movement — the server chain is authoritative for collision, clients rebuild visuals.

	// A movable scene root so the joint can be positioned each solve; subclasses attach
	// their mesh / collision to it.
	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Root->SetMobility(EComponentMobility::Movable);
	SetRootComponent(Root);
}

void AWoTIKTrailer::BeginPlay()
{
	Super::BeginPlay();
	// Each joint gets its own bend limit, so a chain drapes irregularly rather than in
	// a uniform arc.
	AngleExt = WoTRandom::FRandRange(MinAngleExt, MaxAngleExt);
}

void AWoTIKTrailer::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// Only re-solve when the head has actually moved (a static chain costs nothing).
	if (IsValid(HeadActor))
	{
		if (HeadActor->GetActorLocation() != LastHALocation ||
		    HeadActor->GetActorRotation() != LastHARotation)
		{
			LastHALocation = HeadActor->GetActorLocation();
			LastHARotation = HeadActor->GetActorRotation();
			Reposition(HeadActor);
		}
	}
	else if (bMaster)
	{
		// A master with no head means whatever we were following is gone.
		NotifyLostHeadActor();
	}
}

void AWoTIKTrailer::Reposition(AActor* Parent)
{
	if (!IsValid(Parent))
	{
		return;
	}
	AngleExtents2(Parent);

	// Walk the chain outward: our solved transform is the next joint's parent. The null
	// terminator (a well-formed chain ends in a null ChildTrailer) breaks the recursion.
	if (ChildTrailer && ChildTrailer != this)
	{
		ChildTrailer->Reposition(this);
	}
}

void AWoTIKTrailer::AngleExtents2(AActor* Parent)
{
	const FVector ParentLoc = Parent->GetActorLocation();

	// --- Position ---
	// Direction from the parent out to us, then clamp its bend toward the parent's
	// backward direction so the joint can't fold past AngleExt.
	FRotator DeltaRotation = (GetActorLocation() - ParentLoc).Rotation();

	const FVector BackwardNormal = -Parent->GetActorRotation().Vector();
	const FRotator NormalRotation = BackwardNormal.Rotation();

	DeltaRotation.Pitch = AdjustAngle(DeltaRotation.Pitch, NormalRotation.Pitch);
	DeltaRotation.Yaw = AdjustAngle(DeltaRotation.Yaw, NormalRotation.Yaw);
	// Roll doesn't affect position.

	SetActorLocation(ParentLoc + DeltaRotation.Vector() * Dist);

	// --- Rotation ---
	// Face the parent; clamp our roll toward the parent's roll.
	DeltaRotation = (ParentLoc - GetActorLocation()).Rotation();
	DeltaRotation.Roll = AdjustAngle(GetActorRotation().Roll, Parent->GetActorRotation().Roll);
	SetActorRotation(DeltaRotation);
}

float AWoTIKTrailer::AdjustAngle(float DeltaAngle, float NormalAngle) const
{
	// Shortest signed separation between the joint's angle and the reference angle.
	const float Diff = FRotator::NormalizeAxis(DeltaAngle - NormalAngle);
	if (FMath::Abs(Diff) > AngleExt)
	{
		// Past the bend limit: pin to exactly AngleExt on the side it was leaning.
		DeltaAngle = NormalAngle + AngleExt * FMath::Sign(Diff);
	}
	return FRotator::ClampAxis(DeltaAngle);
}
