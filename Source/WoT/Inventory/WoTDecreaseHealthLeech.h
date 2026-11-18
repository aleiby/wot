#pragma once

#include "CoreMinimal.h"
#include "WoTLeech.h"
#include "WoTDecreaseHealthLeech.generated.h"

// Port of WOT.DecreaseHealthLeech (expands Leech) — drains the victim's health
// over time. Spec: exports/script/WOT/DecreaseHealthLeech.uc. Decay's 4th leech,
// and the base of EarthTremorLeech. Unblocked now that AWoTCharacter has health.
//
// Each AffectHost pass takes DamagePerHit per elapsed interval (DamagePerHit *
// Iterations, since the base coalesces missed intervals) off the host via the
// health/damage pipeline (TakeDamage), stopping when the host dies.
//
// The pain-react "double-over" feedback is a second zero-damage DamageEffect
// on a random cadence and uses ProcessEffect; the regular tick carries the
// authored Momentum through TakeDamage.
UCLASS()
class UWoTDecreaseHealthLeech : public UWoTLeech
{
	GENERATED_BODY()

public:
	UWoTDecreaseHealthLeech();

	// HP removed per elapsed AffectResolution interval.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Decay") float DamagePerHit = 1.0f;
	// DecreaseHealthLeech.Momentum; shipped defaults to vect(0,0,0).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Decay")
	FVector Momentum = FVector::ZeroVector;
	// Random pain-only ProcessEffect cadence (DecreaseHealthLeech.uc:15-19,81-82).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Decay")
	float MinPainReactPerMinute = 10.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Decay")
	float MaxPainReactPerMinute = 60.0f;

	virtual void AffectHost(int32 Iterations = 0) override;

private:
	float NextReact = 0.0f;
	float ReactTime = 0.0f;
};
