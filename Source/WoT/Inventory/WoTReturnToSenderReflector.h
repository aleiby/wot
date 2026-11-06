#pragma once

#include "CoreMinimal.h"
#include "WoTReflector.h"
#include "../WoTUE1Names.h"
#include "WoTReturnToSenderReflector.generated.h"

// Port of Angreal.ReturnToSenderReflector (expands Reflector) — the Reflect
// effect. Spec: exports/script/Angreal/ReturnToSenderReflector.uc. While
// installed, any angreal weave that strikes the host bounces back at its
// originator and the host takes no hit. A marker reflector: the bounce lives in
// AWoTGenericProjectile::ProcessTouch's ordered reflector walk — reverse the
// weave, reseek the sender if it was seeking, and hand it to the reflecting pawn.
//
// The production return path emits its ReflectSkinEffect + deflect sound. The
// shipped UnAffectedTypes gate is enforced by ProcessTouch.
UCLASS()
class UWoTReturnToSenderReflector : public UWoTReflector
{
	GENERATED_BODY()

public:
	UWoTReturnToSenderReflector()
	{
		Priority = 160;   // ReturnToSenderReflector.uc:307 (below Absorb 170)
		bRemoveExisting = true;
		bDeleterious = false;
		// ReturnToSenderReflector.uc:298-306, exact shipped defaults.
		UnAffectedTypes[0] = TEXT("MashadarGuide");
		UnAffectedTypes[1] = TEXT("MachinShin");
		UnAffectedTypes[2] = TEXT("AMAPearl");
		UnAffectedTypes[3] = TEXT("AngrealAMAProjectile");
		UnAffectedTypes[4] = TEXT("AngrealDistantEyeProjectile");
		UnAffectedTypes[5] = TEXT("AngrealExpWardProjectile");
		UnAffectedTypes[6] = TEXT("AngrealTracerProjectile");
		UnAffectedTypes[7] = TEXT("AngrealWallOfAirProjectile");
		UnAffectedTypes[8] = TEXT("EarthTremorRock");
	}

	UPROPERTY(EditDefaultsOnly, Category = "WoT|Reflect")
	FName UnAffectedTypes[9];

	bool IsAffectable(const AActor* Other) const
	{
		if (!IsValid(Other))
		{
			return false;
		}
		for (const FName& TypeName : UnAffectedTypes)
		{
			if (WoTUE1Names::ActorIsA(Other, TypeName))
			{
				return false;
			}
		}
		return true;
	}

	virtual bool ReflectsProjectiles() const override { return true; }
	virtual bool IsProjectileAffectable(const AActor* Proj) const override
	{
		return IsAffectable(Proj);
	}
	virtual bool BuildTouchReflectorDescription(
		FWoTTouchReflectorDescription& Out) const override;
	virtual void OnProjectileFeedback() override;
	virtual bool UsesMottledFeedback() const { return false; }
	virtual FName ReflectSoundPath() const { return TEXT("WOT/Reflected"); }
};
