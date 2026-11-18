#include "WoTWhirlwindFunnel.h"
#include "WoTRandom.h"

#include "WoTCarcass.h"
#include "WoTVertexAnimComponent.h"

#include "Engine/SkeletalMesh.h"

AWoTWhirlwindFunnel::AWoTWhirlwindFunnel()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

void AWoTWhirlwindFunnel::BeginPlay()
{
	Super::BeginPlay();
	if (GetNetMode() == NM_DedicatedServer)
	{
		SetActorTickEnabled(false);
		return;
	}

	SliceMeshes.Reserve(MaxSlices);
	for (int32 Index = 0; Index < MaxSlices; ++Index)
	{
		UWoTVertexAnimComponent* Slice = NewObject<UWoTVertexAnimComponent>(
			this, *FString::Printf(TEXT("WhirlwindSlice_%02d"), Index));
		if (!Slice) { continue; }
		AddInstanceComponent(Slice);
		Slice->SetupAttachment(RootComponent);
		Slice->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Slice->SetCastShadow(false);
		Slice->SetCullDistance(5000.0f);
		Slice->SetUsingAbsoluteLocation(true);
		Slice->SetUsingAbsoluteRotation(true);
		Slice->SetUsingAbsoluteScale(true);
		Slice->RegisterComponent();
		Slice->SetVertexMesh(MeshName);
		Slice->SetVisibility(false, true);
		SliceMeshes.Add(Slice);
	}
}

void AWoTWhirlwindFunnel::SetHeight(float Height)
{
	HeightUU = FMath::Max(0.0f, Height);
}

void AWoTWhirlwindFunnel::ShiftParticles(const FVector& Delta)
{
	if (Delta.IsNearlyZero()) { return; }
	for (int32 Index = 0; Index < MaxSlices; ++Index)
	{
		if (Particles[Index].RemainingLife > 0.0f)
		{
			Particles[Index].Location += Delta;
		}
	}
}

int32 AWoTWhirlwindFunnel::GetLiveSliceCount() const
{
	int32 Count = 0;
	for (const FSliceParticle& P : Particles)
	{
		if (P.RemainingLife > 0.0f) { ++Count; }
	}
	return Count;
}

void AWoTWhirlwindFunnel::SpawnSlice()
{
	FSliceParticle& P = Particles[Cursor];
	P.RemainingLife = ParticleLifeSpan;
	P.DrawScale = 0.0f;
	P.Location = GetActorLocation();
	P.Rotation = FRotator(
		WoTRandom::FRandRange(-1000.0f, 1000.0f) * 360.0f / 65536.0f,
		WoTRandom::FRandRange(-1000.0f, 1000.0f) * 360.0f / 65536.0f,
		WoTRandom::FRandRange(-1000.0f, 1000.0f) * 360.0f / 65536.0f);
	Cursor = (Cursor + 1) % MaxSlices;
}

void AWoTWhirlwindFunnel::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (bOn && Volume > 0.0f)
	{
		SpawnAccumulator += DeltaSeconds;
		const float Interval = 1.0f / Volume;
		while (SpawnAccumulator >= Interval)
		{
			SpawnAccumulator -= Interval;
			SpawnSlice();
		}
	}

	const float Velocity = ParticleLifeSpan > 0.0f ? HeightUU / ParticleLifeSpan : 0.0f;
	const float YawDegrees = RotationYawRateUnits * 360.0f / 65536.0f * DeltaSeconds;
	for (int32 Index = 0; Index < MaxSlices; ++Index)
	{
		FSliceParticle& P = Particles[Index];
		UWoTVertexAnimComponent* Slice = SliceMeshes.IsValidIndex(Index)
			? SliceMeshes[Index].Get() : nullptr;
		if (P.RemainingLife <= 0.0f)
		{
			if (Slice) { Slice->SetVisibility(false, true); }
			continue;
		}
		P.RemainingLife -= DeltaSeconds;
		if (P.RemainingLife <= 0.0f)
		{
			if (Slice) { Slice->SetVisibility(false, true); }
			continue;
		}
		P.Location += GetActorForwardVector() * Velocity * DeltaSeconds;
		P.Rotation.Yaw += YawDegrees;
		P.DrawScale += GrowRate * DeltaSeconds;
		if (Slice)
		{
			Slice->SetWorldLocationAndRotation(P.Location, P.Rotation);
			Slice->SetWorldScale3D(FVector(FMath::Max(P.DrawScale, 0.0001f)));
			Slice->SetVisibility(true, true);
		}
	}
}
