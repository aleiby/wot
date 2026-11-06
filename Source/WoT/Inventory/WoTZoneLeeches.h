#pragma once

#include "CoreMinimal.h"
#include "WoTLeech.h"
#include "WoTZoneLeeches.generated.h"

// WOT.SmoothRandFOVLeech.  Retail marks this simulated and explicitly notes
// that its FOV mutation is client-side only.
UCLASS()
class UWoTSmoothRandFOVLeech : public UWoTLeech
{
	GENERATED_BODY()
public:
	UWoTSmoothRandFOVLeech();
	UPROPERTY(EditDefaultsOnly, Category="WoT|Zone") float FluxFOV = 3.0f;
	UPROPERTY(EditDefaultsOnly, Category="WoT|Zone") float FOVRate = 1.0f;
	virtual void AttachTo(APawn* NewHost) override;
	virtual void Unattach() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;
protected:
	void SetNextFOV();
	float InitialFOV = 90.0f;
	float CurrentFOV = 90.0f;
	float NextFOV = 90.0f;
	bool bGrowing = false;
};

UCLASS()
class UWoTWaterFOVLeech : public UWoTSmoothRandFOVLeech
{
	GENERATED_BODY()
public:
	UWoTWaterFOVLeech();
};

// WOT.RandFOVLeech: authority cadence, with owner-client FOV delivery.
UCLASS()
class UWoTRandFOVLeech : public UWoTLeech
{
	GENERATED_BODY()
public:
	UWoTRandFOVLeech();
	UPROPERTY(EditDefaultsOnly, Category="WoT|Zone") float AverageFOV = 110.0f;
	UPROPERTY(EditDefaultsOnly, Category="WoT|Zone") float FluxFOV = 10.0f;
	UPROPERTY(EditDefaultsOnly, Category="WoT|Zone") float FluxTime = 1.0f;
	virtual void AttachTo(APawn* NewHost) override;
	virtual void Unattach() override;
	virtual void AffectHost(int32 Iterations = 0) override;
private:
	void RecalcDeltaFOV();
	float InitialFOV = 90.0f;
	float CurrentFOV = 110.0f;
	float DeltaFOV = 0.0f;
	float RecalcTime = 0.0f;
};

UCLASS()
class UWoTTimerLeech : public UWoTLeech
{
	GENERATED_BODY()
public:
	UWoTTimerLeech();
};
