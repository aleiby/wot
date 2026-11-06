#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameSession.h"
#include "WoTGameSession.generated.h"

/** Preserves password capacity bypass through AGameModeBase::Login's own
 * GameSession approval gate. */
UCLASS()
class AWoTGameSession : public AGameSession
{
	GENERATED_BODY()

public:
	virtual FString ApproveLogin(const FString& Options) override;
};
