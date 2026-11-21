#pragma once
#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "WoTBotWardObserver.generated.h"

class AWoTCharacter;

// Optional soak observer; binds only in an automation fixture.
UCLASS()
class UWoTBotWardObserver : public UObject
{
	GENERATED_BODY()
public:
	void Watch(AActor* Actor);
	int32 Placed = 0, BotDeaths = 0, ShotByBots = 0;
private:
	TSet<TWeakObjectPtr<AActor>> Shot, Dead;
	UFUNCTION() void Damaged(AActor* Actor, float Damage, const UDamageType* Type, AController* Instigator, AActor* Causer);
	void BodyDamaged(AWoTCharacter* Body, AController* Instigator, AActor* Causer, float Damage);
};
