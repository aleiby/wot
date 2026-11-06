#pragma once

#include "CoreMinimal.h"
#include "../Inventory/WoTLeech.h"
#include "../Inventory/WoTReflector.h"
#include "WoTTestStatusIcons.generated.h"

// Test-only duration sources. UnrealScript methods are virtual by default; the
// distinct initial/current values make AddIconInfo's retail asymmetry observable.
UCLASS()
class UWoTTestStatusLeech : public UWoTLeech
{
	GENERATED_BODY()

public:
	float StatusInitialDuration = 0.0f;
	float StatusCurrentDuration = 0.0f;

	virtual float GetInitialDuration() const override
	{
		return StatusInitialDuration;
	}
	virtual float GetDuration() const override
	{
		return StatusCurrentDuration;
	}
};

UCLASS()
class UWoTTestStatusReflector : public UWoTReflector
{
	GENERATED_BODY()

public:
	float StatusInitialDuration = 0.0f;
	float StatusCurrentDuration = 0.0f;

	virtual float GetInitialDuration() const override
	{
		return StatusInitialDuration;
	}
	virtual float GetDuration() const override
	{
		return StatusCurrentDuration;
	}
};
