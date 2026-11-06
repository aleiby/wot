#include "WoTHUDTextures.h"

#include "Assets/WoTRuntimeAssets.h"
#include "Engine/Texture2D.h"

UTexture2D* UWoTHUDTextures::Resolve(FName Id) const
{
#if WOT_GUARDED_BOOTSTRAP
	const FString Name = Id.ToString();
	return UWoTRuntimeAssets::ResolveTexturePath(FSoftObjectPath(FString::Printf(
		TEXT("/Game/WoT/UI/%s.%s"), *Name, *Name)));
#else
	if (const TSoftObjectPtr<UTexture2D>* Texture = Textures.Find(Id))
	{
		return Texture->LoadSynchronous();
	}
	return nullptr;
#endif
}
