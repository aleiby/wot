#include "WoTCallbackProjectile.h"

#include "Components/SphereComponent.h"

AWoTCallbackProjectile::AWoTCallbackProjectile()
{
	// defaults: speed=600, MaxSpeed=600, LifeSpan=0 (no expiry),
	// DrawType=None (invisible), collision 6x12 cylinder
	Speed = 600.0f;
	MaxSpeed = 600.0f;
	InitialLifeSpan = 0.0f;
	Collision->InitSphereRadius(6.0f);
	SetActorHiddenInGame(true);
}

void AWoTCallbackProjectile::DestinationLost()
{
	Destroy();
}

void AWoTCallbackProjectile::Explode(FVector HitLocation, FVector HitNormal)
{
	bExploded = true;
	OnReachedDestination.ExecuteIfBound(HitActor);
	Super::Explode(HitLocation, HitNormal);
}

void AWoTCallbackProjectile::Destroyed()
{
	if (HasAuthority() && !bExploded)
	{
		OnDestinationLost.ExecuteIfBound();
	}
	Super::Destroyed();
}
