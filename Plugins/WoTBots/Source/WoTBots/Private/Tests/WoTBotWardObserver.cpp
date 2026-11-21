#include "WoTBotWardObserver.h"
#include "WoTBotController.h"
#include "WoTCharacter.h"
#include "Projectiles/WoTExpWardProjectile.h"

namespace
{
bool IsWard(const AActor* Actor)
{
	static UClass* Class = FindObject<UClass>(nullptr, TEXT("/Script/WoT.WoTExpWardProjectile"));
	return Actor && Class && Actor->IsA(Class);
}
}
void UWoTBotWardObserver::Watch(AActor* Actor)
{
	if (IsWard(Actor))
	{
		if (static_cast<AWoTExpWardProjectile*>(Actor)->bCorrectlyPlaced) ++Placed;
		Actor->OnTakeAnyDamage.AddUniqueDynamic(this, &ThisClass::Damaged);
	}
	if (auto* Body = Cast<AWoTCharacter>(Actor)) Body->OnDamageApplied.AddUObject(this, &ThisClass::BodyDamaged);
}
void UWoTBotWardObserver::Damaged(AActor* Actor, float Damage, const UDamageType*, AController* Instigator, AActor* Causer)
{
	if (Damage > 0 && static_cast<AWoTExpWardProjectile*>(Actor)->bArmed
		&& Cast<AWoTBotController>(Instigator) && !IsWard(Causer) && !Shot.Contains(Actor))
	{ Shot.Add(Actor); ++ShotByBots; }
}
void UWoTBotWardObserver::BodyDamaged(AWoTCharacter* Body, AController*, AActor* Causer, float Damage)
{
	if (Damage > 0 && !Body->IsAlive() && Cast<AWoTBotController>(Body->GetController())
		&& IsWard(Causer) && !Dead.Contains(Body)) { Dead.Add(Body); ++BotDeaths; }
}
