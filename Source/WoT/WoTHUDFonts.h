#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "WoTHUDFonts.generated.h"

class UFont;

UCLASS()
class UWoTHUDFonts : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "WoT|HUD")
	TMap<FName, TSoftObjectPtr<UFont>> Fonts;

	UFont* Resolve(FName Id) const;
	/** Guarded first-use validation path. A runtime UFont is returned only once
	    every referenced atlas page can be joined to an exact texture logical id. */
	static UFont* ResolveRuntime(FName Id);
	/** Guarded canvas path backed by the authenticated nearest-filtered atlases. */
	static UFont* ResolveCanvasRuntime(FName Id);
};
