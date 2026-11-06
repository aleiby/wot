#pragma once

#include "CoreMinimal.h"
#include "../Inventory/WoTLeech.h"
#include "WoTTestLeech.generated.h"

// Test-only concrete leech: records AffectHost() calls so the automation spec
// can verify the framework's cadence / iteration catch-up. Inert in play.
UCLASS()
class UWoTTestLeech : public UWoTLeech
{
	GENERATED_BODY()

public:
	int32 AffectHostCalls = 0;
	int32 TotalIterations = 0;

	virtual void AffectHost(int32 Iterations = 0) override
	{
		++AffectHostCalls;
		TotalIterations += Iterations;
	}
};
