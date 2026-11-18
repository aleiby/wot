#pragma once

#include "CoreMinimal.h"
#include "WoTLeech.h"
#include "WoTDistantEyeLeech.generated.h"

// Port of Angreal.DistantEyeLeech. The gameplay actor stays authoritative; its
// only job is routing the shipped FOV/glow presentation to the owning client.
UCLASS()
class UWoTDistantEyeLeech : public UWoTLeech
{
	GENERATED_BODY()

public:
	UWoTDistantEyeLeech();

	UPROPERTY(EditDefaultsOnly, Category = "WoT|DistantEye")
	float DistantEyeFOV = 120.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|DistantEye")
	float GlowScale = 0.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|DistantEye")
	FVector GlowFog = FVector(281.25f, 421.875f, 140.625f);

	virtual void AttachTo(APawn* NewHost) override;
	virtual void OnHostDetached(APawn* OldHost) override;
	virtual void Unattach() override;
	virtual void OnComponentDestroyed(bool bDestroyingHierarchy) override;

private:
	bool bViewAdjusted = false;
	UPROPERTY() TObjectPtr<class AWoTPlayerController> AdjustedController;
};
