#pragma once

#include "CoreMinimal.h"

class FWoTRuntimeClassDefaults;

struct FWoTBotFactionProfile
{
	TMap<UClass*, float> Priorities;
	float MinOffensiveUsageInterval = 0.0f;
	float MinDefensiveUsageInterval = 0.0f;
};

namespace WoTBotProfiles
{
	const TMap<UClass*, FWoTBotFactionProfile>& Get();
	bool Decode(const FWoTRuntimeClassDefaults& Defaults,
		TMap<UClass*, FWoTBotFactionProfile>& Out, FString& Error);
	FString DisplayName(UClass* PlayerClass);
}
