#pragma once

#include "CoreMinimal.h"
#include "../Inventory/WoTAngrealInvFireball.h"
#include "../WoTCharacter.h"
#include "WoTTestDamageDispatch.generated.h"

// Test-only pawn that exposes the two observable bottoms of the damage-effect
// pipeline without depending on random sound/animation assets.
UCLASS()
class AWoTTestDamageDispatchCharacter : public AWoTCharacter
{
	GENERATED_BODY()

public:
	int32 ProcessEffectCalls = 0;
	int32 HitReactCalls = 0;
	int32 LastHitDamage = INDEX_NONE;
	float LastHitMomentumZ = 0.0f;
	FWoTDamageEffectSpec LastEffect;

	virtual int32 ProcessEffect(const FWoTDamageEffectSpec& Effect) override
	{
		++ProcessEffectCalls;
		LastEffect = Effect;
		return Super::ProcessEffect(Effect);
	}

	virtual void PlayHitReactAnim(int32 Damage, FName WoTDamageType,
		float MomentumZ, bool bEnforceProductionEligibility = false) override
	{
		++HitReactCalls;
		LastHitDamage = Damage;
		LastHitMomentumZ = MomentumZ;
	}
};

// Deterministic fresh-copy charge fixture for Absorb's NPC-owner branch.
UCLASS()
class AWoTTestFixedChargeAngreal : public AWoTAngrealInvFireball
{
	GENERATED_BODY()

public:
	AWoTTestFixedChargeAngreal()
	{
		MinInitialCharges = 4;
		MaxInitialCharges = 4;
		MaxCharges = 20;
	}
};
