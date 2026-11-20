#pragma once

#include "WoTGameMode.h"
#include "WoTPlayerState.h"
#include "GameFramework/PlayerController.h"
#include "WoTBotRosterTestTypes.generated.h"

UCLASS()
class AWoTBotRosterTestState : public AWoTPlayerState
{
	GENERATED_BODY()
public:
	// Exercise the network leave-message path in the standalone fixture world.
	virtual bool ShouldBroadCastWelcomeMessage(bool bExiting = false) override { return !IsInactive(); }
};

UCLASS()
class AWoTBotRosterTestGame : public AWoTGameMode
{
	GENERATED_BODY()
public:
	AWoTBotRosterTestGame() { PlayerStateClass = AWoTBotRosterTestState::StaticClass(); }
	TArray<FString> Messages;
	TArray<FName> MessageTypes;
	virtual void BroadcastTypedMessage(const FString& Message, FName Type) override
	{
		Messages.Add(Message);
		MessageTypes.Add(Type);
		Super::BroadcastTypedMessage(Message, Type);
	}
};

UCLASS()
class AWoTBotRosterTestViewer : public APlayerController
{
	GENERATED_BODY()
public:
	FVector ViewLocation = FVector(-500, 0, 90);
	FRotator ViewRotation = FRotator::ZeroRotator;
	int32 LeaveMessages = 0;
	TArray<FString> ClientMessages;
	virtual void ClientMessage_Implementation(const FString& Message, FName Type, float Lifetime) override
	{
		ClientMessages.Add(Message);
	}
	virtual void ClientReceiveLocalizedMessage_Implementation(TSubclassOf<ULocalMessage> Message,
		int32 Switch, APlayerState* First, APlayerState* Second, UObject* Optional) override
	{
		if (Switch == 4) ++LeaveMessages;
	}
	virtual void GetPlayerViewPoint(FVector& Location, FRotator& Rotation) const override
	{
		Location = ViewLocation;
		Rotation = ViewRotation;
	}
};
