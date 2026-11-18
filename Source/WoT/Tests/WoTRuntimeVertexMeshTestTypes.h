#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "WoTRuntimeVertexMeshTestTypes.generated.h"

/** Dynamic-delegate observer used by the carrier-free animation parity spec. */
UCLASS()
class UWoTRuntimeVertexMeshAnimEndReceiver final : public UObject
{
	GENERATED_BODY()

public:
	UFUNCTION()
	void HandleAnimEnd(FName Sequence)
	{
		++CallCount;
		LastSequence = Sequence;
	}

	int32 CallCount = 0;
	FName LastSequence = NAME_None;
};
