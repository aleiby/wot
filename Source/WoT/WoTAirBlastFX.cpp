#include "WoTAirBlastFX.h"

#include "WoTParticleSprayer.h"
#include "WoTWeaveFX.h"

#include "Engine/World.h"
#include "Net/UnrealNetwork.h"

AWoTAirBlastFX::AWoTAirBlastFX()
{
	PrimaryActorTick.bCanEverTick = true;
	bReplicates = true;
	// One initial bunch carries RelativeActor/BlastVelocity; every machine then
	// simulates the short flight locally (the original replicated the sprayer
	// actor the same way and tick-simulated everywhere).
	bNetTemporary = true;
	SetReplicatingMovement(false);
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	NetPriority = 6.0f;
	// BlastDuration emit + the 1 s particle fade, with margin (InitialLifeSpan:
	// a ctor SetLifeSpan would not arm the timer — that happens in BeginPlay).
	InitialLifeSpan = 2.0f;
}

void AWoTAirBlastFX::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWoTAirBlastFX, RelativeActor);
	DOREPLIFETIME(AWoTAirBlastFX, BlastVelocity);
	DOREPLIFETIME(AWoTAirBlastFX, SprayerBlowerOffset);
}

void AWoTAirBlastFX::BeginPlay()
{
	Super::BeginPlay();
	// AngrealInvAirBurst.uc:114-116 spawns SprayerBlower before its own visual,
	// 60uu forward from the caster. SprayerBlower defaults supply magnitude 250,
	// duration 1.0, radius/height 60; only RespawnFire is ignored.
	if (IsValid(RelativeActor) && !FVector(BlastVelocity).IsNearlyZero())
	{
		TArray<FName> Ignored{FName(TEXT("RespawnFire"))};
		const FRotator WindRot = FVector(BlastVelocity).Rotation();
		WoTWeaveFX::BlowSprayers(*GetWorld(), RelativeActor->GetActorLocation()
			+ WindRot.Vector() * SprayerBlowerOffset, WindRot,
			250.0f, 1.0f, 60.0f, 60.0f,
			Ignored);
	}
	// Both sprayers ride this actor (FollowActor): the original SetLocation()s
	// itself along the relative flight and the Ball to its own Location each
	// tick (AirBlastProj.uc:90,113). SpawnLocal no-ops on dedicated servers.
	RippleSprayer = WoTWeaveFX::SpawnLocal(*GetWorld(), TEXT("AirBlastProj"),
		GetActorLocation(), GetActorRotation(), /*Follow*/ this);
	// The Ball faces the flight direction (spawned rotator(Velocity),
	// AirBlastProj.uc:96) and sprays its 350 uu/s flare along it.
	BallSprayer = WoTWeaveFX::SpawnLocal(*GetWorld(), TEXT("AirBlastBall"),
		GetActorLocation(), FVector(BlastVelocity).Rotation(), /*Follow*/ this);
	if (IsValid(RippleSprayer))
	{
		RippleSprayer->AddTickPrerequisiteActor(this);
	}
	if (IsValid(BallSprayer))
	{
		BallSprayer->AddTickPrerequisiteActor(this);
	}
	if (IsValid(RelativeActor))
	{
		LastRelativeActorLocation = RelativeActor->GetActorLocation();
		Offset = GetActorLocation() - LastRelativeActorLocation;
		bFollowingRelativeActor = true;
	}
}

void AWoTAirBlastFX::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	Elapsed += DeltaSeconds;

	// Fly forward relative to the caster (AirBlastProj.uc:82-90). Live particles
	// inherit the caster's translation too (uc:71-79), but not the blast's own
	// forward step. If the actor reference is unresolved, continue in world space;
	// a later resolution rebases the offset without snapping.
	if (IsValid(RelativeActor))
	{
		const FVector RelativeLocation = RelativeActor->GetActorLocation();
		if (!bFollowingRelativeActor)
		{
			Offset = GetActorLocation() - RelativeLocation;
			bFollowingRelativeActor = true;
		}
		else
		{
			const FVector RelativeDelta = RelativeLocation - LastRelativeActorLocation;
			if (IsValid(RippleSprayer))
			{
				RippleSprayer->ShiftParticles(RelativeDelta);
			}
			if (IsValid(BallSprayer))
			{
				BallSprayer->ShiftParticles(RelativeDelta);
			}
		}
		LastRelativeActorLocation = RelativeLocation;
		Offset += FVector(BlastVelocity) * DeltaSeconds;
		SetActorLocation(RelativeLocation + Offset);
	}
	else
	{
		bFollowingRelativeActor = false;
		SetActorLocation(GetActorLocation() + FVector(BlastVelocity) * DeltaSeconds);
	}

	// Ball off 0.20 s before the end (AirBlastProj.uc:97,101-109); ripple off +
	// 1 s fade at BlastDuration (Timer(), :119-129 — which also drops the Ball).
	if (!bBallFaded && Elapsed >= BlastDuration - 0.20f)
	{
		bBallFaded = true;
		WoTWeaveFX::FadeOut(BallSprayer, 1.0f);
	}
	if (!bRippleFaded && Elapsed >= BlastDuration)
	{
		bRippleFaded = true;
		WoTWeaveFX::FadeOut(RippleSprayer, 1.0f);
		if (IsValid(BallSprayer))
		{
			BallSprayer->Destroy();   // Timer(): Ball.Destroy()
		}
	}
}
