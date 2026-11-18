#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WoTAirBlastFX.generated.h"

class AWoTParticleSprayer;

// Port of Angreal.AirBlastProj + its AirBlastBall child (AirBlastProj.uc /
// AirBlastBall.uc) — the AirBurst cast's flying air ripple. The original is a
// replicated ParticleSprayer (RemoteRole=SimulatedProxy, RelativeActor
// replicated) that flies forward at the caster's view direction, keeping its
// offset relative to the caster; this port is a thin replicated one-shot marker
// (the AWoTWeaveBurst pattern) whose BeginPlay spawns the LOCAL sprayer pair
// from the WeaveFX registry ("AirBlastProj" ripple + "AirBlastBall" flare) on
// every machine and flies them for BlastDuration, then fades:
//   * Ball stops emitting 0.20 s before the end and is destroyed at it
//     (AirBlastProj.uc:97,101-109,124-128).
//   * The ripple stops at BlastDuration and fades over 1 s (Timer(): bOn=false,
//     LifeSpan=1.0).
// The original ShiftParticles drags live particles by the CASTER's frame delta
// (AirBlastProj.uc:71-79); the local sprayers expose the same translation-only
// shift while this replicated marker advances the position-parented emitter.
UCLASS()
class AWoTAirBlastFX : public AActor
{
	GENERATED_BODY()

public:
	AWoTAirBlastFX();

	// The caster: the blast's flight is relative to it (AirBlastProj.uc:88-90).
	UPROPERTY(Replicated) TObjectPtr<AActor> RelativeActor;
	// vector(ViewRotation) * 350 (AngrealInvAirBurst.uc:122).
	UPROPERTY(Replicated) FVector_NetQuantize10 BlastVelocity;
	UPROPERTY(Replicated) float SprayerBlowerOffset = 60.0f;

	// AirBlastProj.uc:133 BlastDuration.
	UPROPERTY(EditDefaultsOnly, Category = "WoT") float BlastDuration = 0.35f;

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

private:
	UPROPERTY() TObjectPtr<AWoTParticleSprayer> RippleSprayer;
	UPROPERTY() TObjectPtr<AWoTParticleSprayer> BallSprayer;

	FVector Offset = FVector::ZeroVector;   // from the caster; integrates velocity
	FVector LastRelativeActorLocation = FVector::ZeroVector;
	float Elapsed = 0.0f;
	bool bFollowingRelativeActor = false;
	bool bBallFaded = false;
	bool bRippleFaded = false;
};
