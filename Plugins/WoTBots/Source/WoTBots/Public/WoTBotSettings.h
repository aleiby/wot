#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "WoTBotSettings.generated.h"

UCLASS(Config=Game, DefaultConfig)
class WOTBOTS_API UWoTBotSettings : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(Config)
	int32 DefaultBotFill = 0;

	UPROPERTY(Config)
	float ThinkInterval = 0.1f;

	UPROPERTY(Config)
	float SightRange = 4000.0f;

	UPROPERTY(Config)
	float SightConeDegrees = 140.0f;

	UPROPERTY(Config)
	float ReactionSeconds = 0.35f;

	UPROPERTY(Config)
	float AimErrorDegrees = 4.0f;

	UPROPERTY(Config)
	float TargetMemorySeconds = 4.0f;

	UPROPERTY(Config)
	float StrafeRepathSeconds = 1.5f;

	UPROPERTY(Config)
	float EngageWhileSeekingRange = 1500.0f;

	UPROPERTY(Config)
	float HealBelowHealth = 0.5f;

	UPROPERTY(Config)
	float LeaveTimeoutSeconds = 60.0f;

	UPROPERTY(Config)
	float DepartureViewHalfAngleDegrees = 80.0f;

	UPROPERTY(Config)
	float DepartureSeenGraceSeconds = 0.5f;

	UPROPERTY(Config)
	float DepartureVisibilityIntervalSeconds = 0.25f;

	UPROPERTY(Config)
	float StuckRespawnSeconds = 20.0f;

	UPROPERTY(Config)
	float AddIntervalSeconds = 0.5f;
};
