#include "WoTHUDFontImportLibrary.h"

#if WITH_EDITOR

#include "Engine/Font.h"
#include "Engine/Texture2D.h"

namespace
{
	bool WoTHUDFontImportLibrary_HasValidInput(
		const UFont* TargetFont,
		const TArray<UTexture2D*>& AtlasTextures,
		const TArray<int32>& StartUs,
		const TArray<int32>& StartVs,
		const TArray<int32>& USizes,
		const TArray<int32>& VSizes,
		const TArray<int32>& TextureIndices)
	{
		if (!TargetFont || AtlasTextures.IsEmpty())
		{
			return false;
		}
		const int32 CharacterCount = StartUs.Num();
		if (StartVs.Num() != CharacterCount || USizes.Num() != CharacterCount ||
			VSizes.Num() != CharacterCount || TextureIndices.Num() != CharacterCount)
		{
			return false;
		}
		for (UTexture2D* AtlasTexture : AtlasTextures)
		{
			if (!AtlasTexture)
			{
				return false;
			}
		}
		for (const int32 TextureIndex : TextureIndices)
		{
			if (TextureIndex < 0 || TextureIndex >= AtlasTextures.Num()
				|| TextureIndex > MAX_uint8)
			{
				return false;
			}
		}
		return true;
	}
}

bool UWoTHUDFontImportLibrary::OfflineFontMatches(
	const UFont* TargetFont,
	const TArray<UTexture2D*>& AtlasTextures,
	const TArray<int32>& StartUs,
	const TArray<int32>& StartVs,
	const TArray<int32>& USizes,
	const TArray<int32>& VSizes,
	const TArray<int32>& TextureIndices)
{
	if (!WoTHUDFontImportLibrary_HasValidInput(
			TargetFont, AtlasTextures, StartUs, StartVs, USizes, VSizes,
			TextureIndices)
		|| TargetFont->FontCacheType != EFontCacheType::Offline
		|| TargetFont->Textures.Num() != AtlasTextures.Num()
		|| TargetFont->Characters.Num() != StartUs.Num()
		|| TargetFont->IsRemapped != 0 || TargetFont->Kerning != 0
		|| !TargetFont->CharRemap.IsEmpty())
	{
		return false;
	}
	for (int32 TextureIndex = 0; TextureIndex < AtlasTextures.Num(); ++TextureIndex)
	{
		if (TargetFont->Textures[TextureIndex] != AtlasTextures[TextureIndex])
		{
			return false;
		}
	}
	for (int32 CharacterIndex = 0; CharacterIndex < StartUs.Num(); ++CharacterIndex)
	{
		const FFontCharacter& Character = TargetFont->Characters[CharacterIndex];
		if (Character.StartU != StartUs[CharacterIndex]
			|| Character.StartV != StartVs[CharacterIndex]
			|| Character.USize != USizes[CharacterIndex]
			|| Character.VSize != VSizes[CharacterIndex]
			|| Character.TextureIndex != TextureIndices[CharacterIndex]
			|| Character.VerticalOffset != 0)
		{
			return false;
		}
	}
	return true;
}

bool UWoTHUDFontImportLibrary::PopulateOfflineFont(
	UFont* TargetFont,
	const TArray<UTexture2D*>& AtlasTextures,
	const TArray<int32>& StartUs,
	const TArray<int32>& StartVs,
	const TArray<int32>& USizes,
	const TArray<int32>& VSizes,
	const TArray<int32>& TextureIndices)
{
	if (!WoTHUDFontImportLibrary_HasValidInput(
			TargetFont, AtlasTextures, StartUs, StartVs, USizes, VSizes,
			TextureIndices))
	{
		return false;
	}
	if (OfflineFontMatches(TargetFont, AtlasTextures, StartUs, StartVs, USizes,
			VSizes, TextureIndices))
	{
		return true;
	}

	TargetFont->Modify();
	const int32 CharacterCount = StartUs.Num();
	TargetFont->FontCacheType = EFontCacheType::Offline;
	TargetFont->Textures.Reset(AtlasTextures.Num());
	for (UTexture2D* AtlasTexture : AtlasTextures)
	{
		TargetFont->Textures.Add(AtlasTexture);
	}

	TargetFont->Characters.SetNum(CharacterCount);
	for (int32 CharacterIndex = 0; CharacterIndex < CharacterCount; ++CharacterIndex)
	{
		const int32 TextureIndex = TextureIndices[CharacterIndex];
		FFontCharacter& Character = TargetFont->Characters[CharacterIndex];
		Character.StartU = StartUs[CharacterIndex];
		Character.StartV = StartVs[CharacterIndex];
		Character.USize = USizes[CharacterIndex];
		Character.VSize = VSizes[CharacterIndex];
		Character.TextureIndex = static_cast<uint8>(TextureIndex);
		Character.VerticalOffset = 0;
	}
	TargetFont->IsRemapped = 0;
	TargetFont->Kerning = 0;
	TargetFont->CharRemap.Reset();
	TargetFont->CacheCharacterCountAndMaxCharHeight();
	TargetFont->MarkPackageDirty();
	return true;
}

#endif
