#pragma once

#include "CoreMinimal.h"
#include "WoTLeech.h"
#include "WoTAndilayRootLeech.generated.h"

// Port of Angreal.AndilayRootLeech — the singular heal-over-time component
// installed automatically by an Andilay Root pickup. Spec:
// exports/script/Angreal/AndilayRootLeech.uc.
UCLASS()
class UWoTAndilayRootLeech : public UWoTLeech
{
	GENERATED_BODY()

public:
	UWoTAndilayRootLeech();

	UPROPERTY(EditDefaultsOnly, Category = "WoT|AndilayRoot")
	float MaxAffectResolution = 0.8f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|AndilayRoot")
	float MinAffectResolution = 0.2f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|AndilayRoot")
	float MaxHealSpeed = 400.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|AndilayRoot")
	int32 HealingAmount = 1;
	UPROPERTY(Replicated, VisibleAnywhere, Category = "WoT|AndilayRoot")
	int32 NumCharges = 0;
	UPROPERTY(VisibleAnywhere, Category = "WoT|AndilayRoot")
	int32 PeakCharges = 0;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|AndilayRoot")
	int32 MaxCharges = 100;

	void GiveCharges(int32 Amount);
	int32 GetCharges();

	virtual void AffectHost(int32 Iterations = 0) override;
	virtual float GetInitialDuration() const override { return 10000.0f; }
	virtual float GetDuration() const override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
	                           FActorComponentTickFunction* ThisTickFunction) override;
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;
};
