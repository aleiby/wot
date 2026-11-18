#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "WoTBootstrapGameMode.generated.h"

// The controller projects bootstrap Slate. This dedicated payload-free mode
// may diverge from the normal front end later without changing the boot world.
UCLASS()
class AWoTBootstrapGameMode final : public AGameModeBase
{
	GENERATED_BODY()

public:
	AWoTBootstrapGameMode();
};
