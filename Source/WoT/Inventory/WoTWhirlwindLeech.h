#pragma once

#include "CoreMinimal.h"
#include "WoTLeech.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "WoTWhirlwindLeech.generated.h"

// Port of Angreal.WhirlwindLeech (expands Leech) — the effect installed by the
// Whirlwind ter'angreal. Spec: exports/script/Angreal/WhirlwindLeech.uc.
//
// Lifts the victim off the ground and drags it toward DesiredLocation (which the
// caster updates every tick to a point ahead of its view), spinning it. Movement
// is disabled (UE1 PHYS_None); each tick the victim is eased toward
// DesiredLocation at rate 1/AlignmentSpeed and its yaw advances by
// RotationPerSecond. bSingular — one whirlwind per victim.
//
// The replicated leech drives the local mesh-slice funnel, dust, light, loop,
// forced owner view, pawn spin, and the shipped signed-height ActorFits hold.
UCLASS()
class UWoTWhirlwindLeech : public UWoTLeech
{
	GENERATED_BODY()

public:
	UWoTWhirlwindLeech();

	// Where the caster wants the victim (updated from the launcher each tick).
	UPROPERTY(Replicated) FVector DesiredLocation = FVector::ZeroVector;

	// Seconds-scale for easing to DesiredLocation (UE1 AlignmentSpeed).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Whirlwind") float AlignmentSpeed = 2.0f;
	// Victim spin rate, degrees/sec (UE1 RotationPerSecond 120000 units/s).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Whirlwind") float RotationPerSecondDeg = 659.18f;

	void SetDesiredLocation(const FVector& NewLoc) { DesiredLocation = NewLoc; }

	virtual void AttachTo(APawn* NewHost) override;
	virtual void Unattach() override;
	virtual void OnComponentDestroyed(bool bDestroyingHierarchy) override;
	virtual void OnHostDetached(APawn* OldHost) override;   // restore host movement
	virtual void AffectHost(int32 Iterations = 0) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
	                           FActorComponentTickFunction* ThisTickFunction) override;
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// Legend.Util.ActorFits (:400): overlap-cylinder hold predicate. HeightDiff
	// is intentionally signed, matching shipped behavior.
	static bool ActorFits(AActor* MovingActor, const FVector& NewLocation,
		float SearchRadius);
	class AWoTWhirlwindFunnel* GetFunnelForTest() const { return Funnel.Get(); }
	class AWoTParticleSprayer* GetDustForTest() const { return Dust.Get(); }

private:
	bool bLifting = false;
	TEnumAsByte<EMovementMode> OldMovementMode = MOVE_Walking;
	uint8 OldCustomMovementMode = 0;
	UPROPERTY() TObjectPtr<class AWoTWhirlwindFunnel> Funnel;
	UPROPERTY() TObjectPtr<class AWoTParticleSprayer> Dust;
	UPROPERTY() TObjectPtr<class UPointLightComponent> WhirlwindLight;
	FVector LastTraceLocation = FVector::ZeroVector;
	FRotator WWRotation = FRotator::ZeroRotator;

	static void DisableLevitate(APawn* Host);
	void TickPresentation(APawn* Host, float DeltaTime);
	void DestroyPresentation();
};
