#pragma once

#include "CoreMinimal.h"

class AActor;
class AWoTZoneVolume;
class UCapsuleComponent;

// WOTCarcassBase.uc's shared ZoneChange / CheckZone behavior.  Both the full
// carcass and WOTCreatureChunk inherit this implementation in retail.
namespace WoTCarcassZoneBehavior
{
	void CheckZone(AActor& Actor, AWoTZoneVolume* Zone, bool bTakesDamage,
		float PainZoneDamageScale, float DeltaSeconds);

	void HandleZoneChange(AActor& Actor, UCapsuleComponent* Capsule,
		FVector& Velocity, FRotator& RotationRate, float Mass, float Buoyancy,
		bool bTakesDamage, float PainZoneDamageScale, bool& bSplash,
		bool& bWaterCollisionFlattened, AWoTZoneVolume* NewZone,
		AWoTZoneVolume* OldZone);
}
