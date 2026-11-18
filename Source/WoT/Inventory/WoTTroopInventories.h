#pragma once

#include "CoreMinimal.h"
#include "WoTInventory.h"
#include "WoTTroopInventories.generated.h"

// The twelve WoTPawns *Inventory citadel cards. Their resource leaves are folded into
// AWoTNPCCharacter + CreatureRowName; all authored HUD copy remains on these data-only CDOs.
UCLASS()
class AWoTWarderInventory : public AWoTInventory
{
	GENERATED_BODY()
public:
	AWoTWarderInventory();
};

UCLASS()
class AWoTSisterInventory : public AWoTInventory
{
	GENERATED_BODY()
public:
	AWoTSisterInventory();
};

UCLASS()
class AWoTSitterInventory : public AWoTInventory
{
	GENERATED_BODY()
public:
	AWoTSitterInventory();
};

UCLASS()
class AWoTTrollocInventory : public AWoTInventory
{
	GENERATED_BODY()
public:
	AWoTTrollocInventory();
};

UCLASS()
class AWoTMyrddraalInventory : public AWoTInventory
{
	GENERATED_BODY()
public:
	AWoTMyrddraalInventory();
};

UCLASS()
class AWoTBATrollocInventory : public AWoTInventory
{
	GENERATED_BODY()
public:
	AWoTBATrollocInventory();
};

UCLASS()
class AWoTMinionInventory : public AWoTInventory
{
	GENERATED_BODY()
public:
	AWoTMinionInventory();
};

UCLASS()
class AWoTMashadarInventory : public AWoTInventory
{
	GENERATED_BODY()
public:
	AWoTMashadarInventory();
};

UCLASS()
class AWoTLegionInventory : public AWoTInventory
{
	GENERATED_BODY()
public:
	AWoTLegionInventory();
};

UCLASS()
class AWoTSoldierInventory : public AWoTInventory
{
	GENERATED_BODY()
public:
	AWoTSoldierInventory();
};

UCLASS()
class AWoTArcherInventory : public AWoTInventory
{
	GENERATED_BODY()
public:
	AWoTArcherInventory();
};

UCLASS()
class AWoTQuestionerInventory : public AWoTInventory
{
	GENERATED_BODY()
public:
	AWoTQuestionerInventory();
};
