#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "WoTMenuStrings.generated.h"

// Generated from original/System/WoT.int by import_menu_strings.py. Retail
// strings stay out of C++ call sites; port-only labels may still use literals.
UCLASS()
class UWoTMenuStrings : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "WoT|Menus")
	TMap<FName, FString> Strings;

	UPROPERTY(EditAnywhere, Category = "WoT|Menus")
	TArray<FName> Sections;

	FText Resolve(FName Id) const;
};
