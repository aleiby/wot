#include "WoTDistantEyeCastReflector.h"

#include "../Projectiles/WoTDistantEyeProjectile.h"

#include "Net/UnrealNetwork.h"

UWoTDistantEyeCastReflector::UWoTDistantEyeCastReflector()
{
	Priority = 128;
	bRemovable = false;
}

bool UWoTDistantEyeCastReflector::InterceptUseAngreal()
{
	if (IsValid(Eye))
	{
		Eye->Fire();
	}
	// A stale eye-owned reflector must never leak this key to the held artifact.
	return true;
}

bool UWoTDistantEyeCastReflector::InterceptCeaseUsingAngreal()
{
	if (IsValid(Eye))
	{
		Eye->UnFire();
	}
	return true;
}

float UWoTDistantEyeCastReflector::GetInitialDuration() const
{
	const AWoTDistantEyeProjectile* DefaultEye =
		IsValid(Eye) ? Eye->GetClass()->GetDefaultObject<AWoTDistantEyeProjectile>()
			: GetDefault<AWoTDistantEyeProjectile>();
	return DefaultEye ? static_cast<float>(DefaultEye->Health) : 0.0f;
}

float UWoTDistantEyeCastReflector::GetDuration() const
{
	return IsValid(Eye) ? static_cast<float>(Eye->Health) : 0.0f;
}

void UWoTDistantEyeCastReflector::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UWoTDistantEyeCastReflector, Eye);
}
