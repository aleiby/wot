#include "WoTAttachMover.h"

#include "EngineUtils.h"

void AWoTAttachMover::BeginPlay()
{
	Super::BeginPlay();
	if (HasAuthority() && !AttachTag.IsNone())
	{
		AttachTaggedActors();
	}
}

void AWoTAttachMover::AttachTaggedActors()
{
	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		AActor* Actor = *It;
		if (!IsValid(Actor) || Actor == this)
		{
			continue;
		}
		AWoTMover* Mover = Cast<AWoTMover>(Actor);
		if (!ActorMatchesWoTTag(Actor, AttachTag))
		{
			continue;
		}
		if (!Mover)
		{
			Actor->AttachToActor(this, FAttachmentTransformRules::KeepWorldTransform);
		}
		else if (Mover->bSlave)
		{
			Mover->SetMoverFSMInert();
			Mover->AttachToActor(this, FAttachmentTransformRules::KeepWorldTransform);
		}
	}
}
