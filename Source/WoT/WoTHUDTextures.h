#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "WoTHUDTextures.generated.h"

class UTexture2D;

UCLASS()
class UWoTHUDTextures : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "WoT|HUD")
	TMap<FName, TSoftObjectPtr<UTexture2D>> Textures;

	UTexture2D* Resolve(FName Id) const;
};
