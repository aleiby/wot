#include "WoTMeshModulate.h"

#include "Engine/Texture.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "UObject/SoftObjectPath.h"

namespace
{
	UTexture* WoTMeshModulateTexture(UMaterialInterface* Material)
	{
		if (!IsValid(Material))
		{
			return nullptr;
		}
		const FName TextureParameters[] = {
			FName(TEXT("Tex")), FName(TEXT("BaseColorTexture")),
			FName(TEXT("DiffuseTexture"))
		};
		for (const FName Parameter : TextureParameters)
		{
			UTexture* Texture = nullptr;
			if (Material->GetTextureParameterValue(
				FHashedMaterialParameterInfo(Parameter), Texture) && IsValid(Texture))
			{
				return Texture;
			}
		}
		return nullptr;
	}
}

UMaterialInterface* WoTMeshModulate::GetMaster()
{
	static TWeakObjectPtr<UMaterialInterface> Cached;
	if (!Cached.IsValid())
	{
		Cached = Cast<UMaterialInterface>(FSoftObjectPath(
			TEXT("/Game/WoT/Materials/M_WoT_MeshModulate."
				"M_WoT_MeshModulate")).TryLoad());
	}
	return Cached.Get();
}

UMaterialInstanceDynamic* WoTMeshModulate::CreateInstance(
	UObject* Outer, UMaterialInterface* OriginalMaterial)
{
	UMaterialInterface* Master = GetMaster();
	if (!IsValid(Outer) || !IsValid(Master))
	{
		return nullptr;
	}
	UMaterialInstanceDynamic* Instance = UMaterialInstanceDynamic::Create(Master, Outer);
	if (IsValid(Instance))
	{
		if (UTexture* Texture = WoTMeshModulateTexture(OriginalMaterial);
			IsValid(Texture))
		{
			Instance->SetTextureParameterValue(FName(TEXT("Tex")), Texture);
		}
	}
	return Instance;
}
