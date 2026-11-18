#pragma once

#include "CoreMinimal.h"
#include "../WoTCharacter.h"
#include "WoTTestFellOut.generated.h"

// Test-only player pawn that records how the ordinary death path was entered.
UCLASS()
class AWoTTestFellRecorderCharacter : public AWoTCharacter
{
	GENERATED_BODY()

public:
	int32 DiedCalls = 0;
	FName DiedDamageType;
	bool bDiedWithKiller = false;
	int32 HealthAtDeath = 0;

	virtual void Died(AController* Killer, FName WoTDamageType,
		const FVector& HitLocation) override
	{
		++DiedCalls;
		DiedDamageType = WoTDamageType;
		bDiedWithKiller = Killer != nullptr;
		HealthAtDeath = Health;
		Super::Died(Killer, WoTDamageType, HitLocation);
	}
};
