#include "WoTAngrealInvAirBurst.h"

#include "../WoTAirBlastFX.h"
#include "../WoTCharacter.h"
#include "../WoTBreakableDecoration.h"
#include "../WoTBounceableDecoration.h"
#include "../WoTCarcass.h"
#include "../WoTExplodingMover.h"
#include "../AI/WoTNPCWeapon.h"
#include "../Projectiles/WoTProjectile.h"   // FWoTDamageEvent
#include "../Projectiles/WoTExpWardProjectile.h"
#include "../Projectiles/WoTLegionProjectile.h"
#include "../Projectiles/WoTMashadarSegment.h"
#include "../Traps/WoTPortcullis.h"
#include "../Traps/WoTWall.h"

#include "Components/CapsuleComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/OverlapResult.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/ProjectileMovementComponent.h"

namespace
{
	bool AirBurstLineCheck(AActor& Actor, const FVector& Start,
		const FVector& End, FHitResult& OutHit)
	{
		TInlineComponentArray<UPrimitiveComponent*> Primitives(&Actor);
		bool bHit = false;
		float BestDistanceSq = TNumericLimits<float>::Max();
		FCollisionQueryParams Params(SCENE_QUERY_STAT(AirBurstActorLineCheck), false);
		for (UPrimitiveComponent* Primitive : Primitives)
		{
			FHitResult Hit;
			bool bPrimitiveHit = false;
			if (const UWoTFlatCylinderComponent* Flat =
				Cast<UWoTFlatCylinderComponent>(Primitive))
			{
				bPrimitiveHit = Flat->ExactLineTrace(Start, End, Hit);
			}
			else
			{
				bPrimitiveHit = Primitive && Primitive->IsCollisionEnabled()
					&& Primitive->LineTraceComponent(Hit, Start, End, Params);
			}
			if (bPrimitiveHit)
			{
				const float DistanceSq = FVector::DistSquared(Start, Hit.ImpactPoint);
				if (!bHit || DistanceSq < BestDistanceSq)
				{
					bHit = true;
					BestDistanceSq = DistanceSq;
					OutHit = Hit;
				}
			}
		}
		if (!bHit)
		{
			if (const AWoTBounceableDecoration* Bounceable =
				Cast<AWoTBounceableDecoration>(&Actor);
				Bounceable && Bounceable->Collision)
			{
				bHit = Bounceable->Collision->ExactLineTrace(Start, End, OutHit);
			}
		}
		return bHit;
	}
}

bool AWoTAngrealInvAirBurst::IsAffectable(const AActor* Actor) const
{
	// IsAffectable walks the authored AffectedTypes names (uc:286-307,352-361).
	// The concrete production families preserve IsA ancestry, including the
	// MashadarTrailer segment/tip leaf and the port's tossed-weapon bounceable leaf.
	return Actor && Actor != GetOwner()
		&& (Actor->IsA<APawn>() || Actor->IsA<AWoTCarcass>()
			|| Actor->IsA<AWoTMashadarSegment>()
			|| Actor->IsA<AWoTExplodingMover>() || Actor->IsA<AWoTExpWardProjectile>()
			|| Actor->IsA<AWoTLegionProjectile>()
			|| Actor->IsA<AWoTPortcullisMover>() || Actor->IsA<AWoTWallSlab>()
			|| Actor->IsA<AWoTTossedWeapon>()
			|| Actor->IsA<AWoTBounceableDecoration>()
			|| Actor->IsA<AWoTBreakableDecoration>());
}

AWoTAngrealInvAirBurst::AWoTAngrealInvAirBurst()
{
	BindStatusIcons(TEXT("I_AirBurst"), TEXT("M_AirBurst"));
	// AngrealInvAirBurst defaultproperties
	RangeX = 120.0f;
	RangeY = 20.0f;
	MaxDamage = 20.0f;
	MinDamage = 10.0f;
	MomentumTransfer = 20000.0f;
	LargestCollisionRadius = 250.0f;
	RoundsPerMinute = 120.0f;
	bElementAir = true;
	bCommon = true;
	bOffensive = true;
	bCombat = true;
	bRestrictsUsage = true;
	// Retail configures this artifact as inexhaustible: a fixed initial count of 999
	// avoids randomization, and zero cost prevents Burst from reaching GoEmpty
	// (AngrealInvAirBurst.uc:379; Angreal.int:31).
	MinInitialCharges = 999;
	MaxInitialCharges = 999;
	MaxCharges = 999;
	ChargeCost = 0;
	Priority = 5.0f;
	MaxChargeUsedInterval = 0.5f;
	InventoryGroup = 51;
	// ActivateSoundName="Angreal.ActivateAB" (AngrealInvAirBurst.uc:376).
	ActivateSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/ActivateAB.ActivateAB"));
}

void AWoTAngrealInvAirBurst::CastArtifact()
{
	if (GetWorld()->GetTimeSeconds() <= NextCastTime)
	{
		return;
	}
	Super::CastArtifact();   // sound + HUD hook
	NextCastTime = GetWorld()->GetTimeSeconds() + (60.0f / RoundsPerMinute);
	Burst();
	UseCharge();
}

void AWoTAngrealInvAirBurst::Burst()
{
	APawn* Caster = Cast<APawn>(GetOwner());
	if (!Caster || !HasAuthority())
	{
		return;
	}

	const FRotator ViewRot = Caster->GetViewRotation();
	const FVector Start = Caster->GetActorLocation()
		+ FVector(0, 0, Caster->BaseEyeHeight);
	const FVector End = Start + ViewRot.Vector() * RangeX;
	const float HalfRangeY = RangeY / 2.0f;

	// Shipped Burst() visuals (AngrealInvAirBurst.uc:114-124): the flying air
	// ripple + flare, spawned at Owner.Location + EffectOffset (unrotated, :121)
	// facing the view frame's Y axis (rotator(vect(0,1,0) >> ViewRotation)) and
	// flying vector(ViewRotation) * 350 relative to the caster. The replicated
	// AWoTAirBlastFX marker re-derives the local sprayers on every machine.
	// The marker applies SprayerBlower (:114-116) locally in BeginPlay.
	{
		const FVector FXLoc = Caster->GetActorLocation() + EffectOffset;
		const FRotator FXRot =
			FRotationMatrix(ViewRot).GetScaledAxis(EAxis::Y).Rotation();
		const FTransform FXAt(FXRot, FXLoc);
		if (AWoTAirBlastFX* FX = GetWorld()->SpawnActorDeferred<AWoTAirBlastFX>(
				AWoTAirBlastFX::StaticClass(), FXAt, this, Caster,
				ESpawnActorCollisionHandlingMethod::AlwaysSpawn))
		{
			FX->RelativeActor = Caster;
			FX->BlastVelocity = ViewRot.Vector() * 350.0f;
			FX->SprayerBlowerOffset = RangeX * 0.5f;
			FX->FinishSpawning(FXAt);
		}
	}

	const FName DmgType = AWoTAngrealInventory::GetDamageType(this);
	AController* InstigatorCtrl = Caster->GetController();

	// RadiusActors(class'Actor', RangeX + LargestCollisionRadius, Start), followed
	// by each candidate's own LineCheck. The scan deliberately spans all object
	// channels: the retail AffectedTypes list is not pawn-only.
	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		AActor* Victim = *It;
		if (!IsAffectable(Victim)
			|| FVector::DistSquared(Victim->GetActorLocation(), Start)
				> FMath::Square(RangeX + LargestCollisionRadius))
		{
			continue;
		}
		FHitResult H;
		if (!AirBurstLineCheck(*Victim, Start, End, H))
		{
			continue;
		}

		// Offset of the entry point in the caster's local frame (X fwd, Y, Z).
		const FVector Rel = ViewRot.UnrotateVector(H.ImpactPoint - Start);

		// Falloff scalar: 1 at point-blank centre, 0 at the extents.
		float Scalar = (1.0f - Rel.X / RangeX)
			* (1.0f - FMath::Abs(Rel.Y) / HalfRangeY)
			* (1.0f - FMath::Abs(Rel.Z) / HalfRangeY);
		Scalar = FMath::Clamp(Scalar, 0.0f, 1.0f);

		// Damage (MinDamage..MaxDamage) — zero momentum: PushActor damages pawns with
		// vect(0,0,0) (AngrealInvAirBurst.uc:232) and the push is gated on IsPushable
		// (:254), whose PushableTypes are Carcass/BounceableDecoration/
		// BreakableDecoration ONLY (:362-364) — pawns are NEVER knocked back. Those
		// actor types use their production falling/movement paths below.
		const int32 Damage = FMath::TruncToInt(
			MinDamage + (MaxDamage - MinDamage) * Scalar);
		if (AWoTCharacter* Character = Cast<AWoTCharacter>(Victim))
		{
			// WOTPlayer/WOTPawn receive the retail DamageEffect through
			// ProcessEffect, so NoBadEffects, element shields, Reflect and Absorb
			// get the same interception opportunity as every other weave.
			FWoTDamageEffectSpec Effect;
			Effect.Damage = Damage;
			Effect.InstigatorPawn = Caster;
			Effect.HitLocation = H.ImpactPoint;
			Effect.UE1Momentum = FVector::ZeroVector;
			Effect.DamageType = DmgType;
			Effect.SourceAngreal = this;
			Effect.bDeleterious = true;
			Character->ProcessEffect(Effect);
		}
		else
		{
			// Non-WoT pawns and non-pawns have no reflector chain in retail.
			FWoTDamageEvent Ev;
			Ev.WoTDamageType = DmgType;
			Ev.UE1Momentum = FVector::ZeroVector;   // AngrealInvAirBurst.uc:232
			Ev.HitInfo.ImpactPoint = H.ImpactPoint;
			Ev.Damage = static_cast<float>(Damage);
			Victim->TakeDamage(static_cast<float>(Damage), Ev,
				InstigatorCtrl, this);
		}

		FVector Momentum = Caster->GetActorForwardVector().GetSafeNormal();
		Momentum.Z = 0.5f;
		if (AWoTCarcass* Carcass = Cast<AWoTCarcass>(Victim))
		{
			Carcass->AddUE1Velocity(Momentum
				* (MomentumTransfer / FMath::Max(Carcass->Mass, 0.01f)) * Scalar);
		}
		else if (AWoTBreakableDecoration* Breakable =
			Cast<AWoTBreakableDecoration>(Victim))
		{
			Breakable->StartFalling(Breakable->GetFallVelocity() + Momentum
				* (MomentumTransfer / FMath::Max(Breakable->Mass, 0.01f)) * Scalar);
		}
		else if (AWoTBounceableDecoration* Bounceable =
			Cast<AWoTBounceableDecoration>(Victim))
		{
			Bounceable->AddBounceVelocity(Momentum
				* (MomentumTransfer / FMath::Max(Bounceable->Mass, 0.01f)) * Scalar);
		}
		else if (AWoTTossedWeapon* Tossed = Cast<AWoTTossedWeapon>(Victim))
		{
			if (Tossed->Movement)
			{
				Tossed->Movement->Velocity += Momentum
					* (MomentumTransfer / FMath::Max(Tossed->Mass, 0.01f)) * Scalar;
			}
		}
	}
}
