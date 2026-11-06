#include "WoTDestroyOnDestroyLeech.h"

#include "GameFramework/Actor.h"

UWoTDestroyOnDestroyLeech::UWoTDestroyOnDestroyLeech()
{
	// DestroyOnDestroyLeech defaultproperties.
	AffectResolution = 1.0f;   // poll DestroyActor once a second
	bDeleterious = true;
}

void UWoTDestroyOnDestroyLeech::AffectHost(int32 /*Iterations*/)
{
	// Detach once the actor whose lifetime we track is gone
	// (DestroyOnDestroyLeech.uc:34-40).
	if (!IsValid(DestroyActor))
	{
		Unattach();   // UE1 Unattach() + Destroy() collapse to DestroyComponent()
	}
}

void UWoTDestroyOnDestroyLeech::OnComponentDestroyed(bool bDestroyingHierarchy)
{
	// UE1 DestroyOnDestroyLeech.Destroyed(): take the tied actor with us. Guard
	// against the reverse path (the wall destroying itself unattaches us) — Destroy
	// on an already-dying actor is a no-op in UE1 too, mirrored by IsValid here.
	if (IsValid(DestroyActor) && !DestroyActor->IsActorBeingDestroyed())
	{
		DestroyActor->Destroy();
	}
	DestroyActor = nullptr;

	Super::OnComponentDestroyed(bDestroyingHierarchy);
}
