#pragma once

#include "CoreMinimal.h"

/** Release-pinned canonical SHA-256 digest for every production family. */
WOT_API const TMap<FString, FString>& WoTRetailFamilyPins();
