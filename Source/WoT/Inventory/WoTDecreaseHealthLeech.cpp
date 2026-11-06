#include "WoTDecreaseHealthLeech.h"
#include "../WoTRandom.h"

#include "WoTAngrealInventory.h"
#include "../WoTCharacter.h"
#include "../Projectiles/WoTProjectile.h"   // FWoTDamageEvent

#include "GameFramework/Controller.h"

UWoTDecreaseHealthLeech::UWoTDecreaseHealthLeech()
{
	// DecreaseHealthLeech defaultproperties (simulation subset).
	DamagePerHit = 1.0f;
	Momentum = FVector::ZeroVector;
	MinPainReactPerMinute = 10.0f;
	MaxPainReactPerMinute = 60.0f;
	AffectResolution = 0.2f;   // 1 HP / 0.2 s (Decay's player rate)
	bDeleterious = true;
	bDisplayIcon = true;
}

void UWoTDecreaseHealthLeech::AffectHost(int32 Iterations)
{
	AWoTCharacter* Host = Cast<AWoTCharacter>(GetOwner());
	if (!IsValid(Host) || !Host->IsAlive())
	{
		Unattach();
		return;
	}

	// DamageEffect.Invoke inlined: DamagePerHit per elapsed interval, via the
	// health/damage pipeline. The element-typed zero-damage pain reaction below
	// is delivered independently on its authored cadence.
	const int32 Damage = FMath::RoundToInt(DamagePerHit * FMath::Max(Iterations, 1));
	FWoTDamageEvent Ev;
	Ev.WoTDamageType = TEXT("continuous");
	Ev.UE1Momentum = Momentum;   // DecreaseHealthLeech.uc:42; default vect(0,0,0)
	Ev.HitInfo.ImpactPoint = Host->GetActorLocation();
	AController* Instigator = LeechInstigator ? LeechInstigator->GetController() : nullptr;
	// DamageCauser is the source artifact (a component can't be the causer).
	Host->TakeDamage((float)Damage, Ev, Instigator, SourceAngreal);

	if (!Host->IsAlive())
	{
		Unattach();
		return;
	}

	// DecreaseHealthLeech.uc:61-73: periodically send a second, zero-damage
	// DamageEffect through ProcessEffect solely to reach Pawn.PlayHit. The guarded
	// momentum block is why this never changes physics.
	const int32 ElapsedIterations = FMath::Max(Iterations, 1);
	ReactTime += AffectResolution * ElapsedIterations;
	if (ReactTime >= NextReact)
	{
		const float Rate = WoTRandom::FRandRange(
			MinPainReactPerMinute, MaxPainReactPerMinute);
		NextReact = Rate > 0.0f ? 60.0f / Rate : TNumericLimits<float>::Max();
		ReactTime = 0.0f;

		FWoTDamageEffectSpec Pain;
		Pain.Damage = 0;
		Pain.InstigatorPawn = LeechInstigator;
		Pain.HitLocation = Host->GetActorLocation();
		Pain.UE1Momentum = FVector::ZeroVector;
		Pain.DamageType = AWoTAngrealInventory::GetDamageType(SourceAngreal);
		Pain.SourceProjectile = SourceProjectile;
		Pain.SourceProjClass = SourceProjClass;
		Pain.SourceAngreal = SourceAngreal;
		Pain.bDeleterious = true;
		Host->ProcessEffect(Pain);
	}
}
