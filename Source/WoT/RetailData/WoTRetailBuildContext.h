#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"
#include "WoTRetailReader/MeshAnimation.h"

class FWoTRuntimeClassDefaults;

struct FWoTRetailTextureFacts
{
	uint32 USize = 0;
	uint32 VSize = 0;
	uint8 Format = 0;
	FString AnimNext;
	float MinFrameRate = 0.0f;
	float MaxFrameRate = 0.0f;
	uint8 PrimeCount = 0;
	bool bProcedural = false;
};

struct FWoTRetailBuildContext
{
	const FWoTRuntimeClassDefaults& ClassDefaults;
	TFunction<bool(FStringView TextureIdentity, FWoTRetailTextureFacts& Out)>
		FindTexture;
	TFunction<bool(FStringView MeshIdentity,
		TArray<FWoTVertexMeshSequence>& Out)> FindMeshSequences;
};
