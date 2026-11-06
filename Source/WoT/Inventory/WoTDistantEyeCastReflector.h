#pragma once

#include "CoreMinimal.h"
#include "WoTReflector.h"
#include "WoTDistantEyeCastReflector.generated.h"

// Port of Angreal.DistantEyeCastReflector. While the eye is open this
// non-removable reflector redirects the host's Use/Cease input to the eye.
// Priority 128 is intentionally below AMA NoCast's 255, so an anti-magic dome
// still suppresses both the selected artifact and the remote dart stream.
UCLASS()
class UWoTDistantEyeCastReflector : public UWoTReflector
{
	GENERATED_BODY()

public:
	UWoTDistantEyeCastReflector();

	UPROPERTY(Replicated) TObjectPtr<class AWoTDistantEyeProjectile> Eye;

	virtual bool InterceptUseAngreal() override;
	virtual bool InterceptCeaseUsingAngreal() override;
	virtual float GetInitialDuration() const override;
	virtual float GetDuration() const override;
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;
};
