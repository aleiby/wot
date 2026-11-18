#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "WoTFrontEndGameMode.generated.h"

// Menu-world rules only: create the local front-end controller, but never a
// gameplay pawn or HUD behind the Slate menu.
UCLASS()
class AWoTFrontEndGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AWoTFrontEndGameMode();
};
