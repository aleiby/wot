#include "WoTArcherArrow.h"

#include "../WoTMeshFX.h"

#include "Components/SphereComponent.h"

AWoTArcherArrow::AWoTArcherArrow()
{
	// ArcherArrow.uc: speed=16384, damage inherited 10 (WotWeaponProjectile.uc:268).
	Speed = 16384.0f;
	MaxSpeed = 16384.0f;
	Damage = 10.0f;
	// Retail arrows are ZERO-EXTENT (Engine.Projectile CollisionRadius/
	// Height=0; neither WotWeaponProjectile.uc nor ArcherArrow.uc overrides),
	// so a missed arrow's origin stops ON the wall and the shaft embeds.
	// Without this the UE5 USphereComponent 32uu default floated arrows off
	// walls AND widened pawn hit detection vs retail's point trace
	// (divergence register #5, RESTORE-RULED 2026-07-24). 2uu approximates
	// the zero-extent sweep without degenerate-collision risk.
	Collision->InitSphereRadius(2.0f);
	ProjectileMeshAsset = FSoftObjectPath(TEXT("/Game/WoT/Meshes/MArcherArrow/MArcherArrow/SkeletalMeshes/MArcherArrow.MArcherArrow"));

	// ArcherArrow.uc AmbientSound=Arrow_Flight1 (Actor byte defaults 32/128/64);
	// HitWall goes quiet (StopAmbientHum — WotWeaponProjectile.uc:150).
	AmbientLoopSound =
	    FSoftObjectPath(TEXT("/Game/WoT/Sounds/WoTPawns/Arrow_Flight1.Arrow_Flight1"));
	SoundHitPawn =
		FSoftObjectPath(TEXT("/Game/WoT/Sounds/WoTPawns/Crossbow_HitPawn1.Crossbow_HitPawn1"));
	SoundHitWall =
		FSoftObjectPath(TEXT("/Game/WoT/Sounds/WoTPawns/Crossbow_HitWall1.Crossbow_HitWall1"));
	SpawnSound =
		FSoftObjectPath(TEXT("/Game/WoT/Sounds/WoTPawns/Crossbow_Shoot1.Crossbow_Shoot1"));

}

void AWoTArcherArrow::PlayExplodeCosmetics(const FVector& HitLocation,
	const FVector& HitNormal)
{
	if (GetWorld())
	{
		const FVector Direction = GetVelocity().IsNearlyZero()
			? -HitNormal : GetVelocity();
		WoTMeshFX::SpawnLocal(*GetWorld(), TEXT("ArrowTrail"), HitLocation,
			Direction.Rotation());
	}
}
