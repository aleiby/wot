#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "WoTHUDFontImportLibrary.generated.h"

class UFont;
class UTexture2D;

// Python cannot portably assign UFont's native-serialized FFontCharacter array
// across engine versions.  Keep that editor import seam small and typed here.
UCLASS()
class UWoTHUDFontImportLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
#if WITH_EDITOR
	UFUNCTION(BlueprintPure, Category = "WoT|Editor|HUD")
	static bool OfflineFontMatches(
		const UFont* TargetFont,
		const TArray<UTexture2D*>& AtlasTextures,
		const TArray<int32>& StartUs,
		const TArray<int32>& StartVs,
		const TArray<int32>& USizes,
		const TArray<int32>& VSizes,
		const TArray<int32>& TextureIndices);

	UFUNCTION(BlueprintCallable, Category = "WoT|Editor|HUD")
	static bool PopulateOfflineFont(
		UFont* TargetFont,
		const TArray<UTexture2D*>& AtlasTextures,
		const TArray<int32>& StartUs,
		const TArray<int32>& StartVs,
		const TArray<int32>& USizes,
		const TArray<int32>& VSizes,
		const TArray<int32>& TextureIndices);
#endif
};
