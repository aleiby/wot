#pragma once

#include "CoreMinimal.h"

class UMaterialInstanceDynamic;
class UMaterialInterface;

namespace WoTMeshModulate
{
	UMaterialInterface* GetMaster();
	UMaterialInstanceDynamic* CreateInstance(UObject* Outer,
		UMaterialInterface* OriginalMaterial);
}
