#pragma once

#include "CoreMinimal.h"
#include "Engine/DataTable.h"
#include "WoTAngrealHandRow.generated.h"

// One populated cell of WOTPlayer.AngrealHandOrder[100].
USTRUCT()
struct FWoTAngrealHandRow : public FTableRowBase
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "WoT") int32 Hand = 0;
	UPROPERTY(EditAnywhere, Category = "WoT") int32 Slot = 0;
	UPROPERTY(EditAnywhere, Category = "WoT") FName AngrealClass;
};
