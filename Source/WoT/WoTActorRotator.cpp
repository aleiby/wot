#include "WoTActorRotator.h"
#include "WoTDecoration.h"

#include "Components/SceneComponent.h"
#include "Math/RotationMatrix.h"
#include "Misc/Crc.h"

namespace
{
constexpr double UE1RotatorUnitsToDegrees = 360.0 / 65536.0;

FRotator ToDegrees(const FRotator& RotatorUnits)
{
	return FRotator(
		RotatorUnits.Pitch * UE1RotatorUnitsToDegrees,
		RotatorUnits.Yaw * UE1RotatorUnitsToDegrees,
		RotatorUnits.Roll * UE1RotatorUnitsToDegrees);
}

void SetDrivenRotation(AActor* Actor, const FRotator& Rotation)
{
	if (AWoTDecoration* Decoration = Cast<AWoTDecoration>(Actor))
	{
		// UE1's cylinder is world-axis aligned. Projected decorations therefore keep
		// an identity actor/root while SetRotation replaces (rather than composes
		// with) the authored mesh orientation.
		Decoration->SetActorRotation(FRotator::ZeroRotator);
		if (Decoration->Mesh)
		{
			Decoration->Mesh->SetWorldRotation(Rotation);
		}
		return;
	}
	Actor->SetActorRotation(Rotation);
}
}

AWoTActorRotator::AWoTActorRotator()
{
	PrimaryActorTick.bCanEverTick = true;
	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Root")));
	bReplicates = false;
}

void AWoTActorRotator::BeginPlay()
{
	Super::BeginPlay();
	Initialize();
	if (!HasResolvedDrivenActor())
	{
		SetActorTickEnabled(false);
	}
}

void AWoTActorRotator::Initialize()
{
	const FString SeedName = GetFName().ToString();
	RandomStream.Initialize(static_cast<int32>(FCrc::StrCrc32(*SeedName)));

	CurrentRadius = static_cast<float>(RandomStream.FRandRange(MinRadius, MaxRadius));
	CurrentRotationRate = static_cast<float>(
		RandomStream.FRandRange(MinRotationRate, MaxRotationRate));
	CurrentHeight = static_cast<float>(RandomStream.FRandRange(MinHeight, MaxHeight));

	CalcNextRadius();
	CalcNextRotationRate();
	CalcNextHeight();

	FRoll = 65535.0f * RandomStream.FRand();

	constexpr double DegreesToUE1Units = 65536.0 / 360.0;
	const FRotator Spawn = GetActorRotation();
	PhysPitchUnits = static_cast<float>(Spawn.Pitch * DegreesToUE1Units);
	PhysYawUnits = static_cast<float>(Spawn.Yaw * DegreesToUE1Units);
}

void AWoTActorRotator::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (!HasResolvedDrivenActor())
	{
		SetActorTickEnabled(false);
		return;
	}

	UpdateRadius(DeltaSeconds);
	UpdateRotationRate(DeltaSeconds);
	UpdateHeight(DeltaSeconds);

	FRoll += CurrentRotationRate * DeltaSeconds;
	const int32 RollUnits = static_cast<int32>(FRoll) & 0xFFFF;
	FRotator OwnRotation = GetActorRotation();
	if (!PhysRotationRateUnits.IsZero())
	{
		PhysPitchUnits += PhysRotationRateUnits.Pitch * DeltaSeconds;
		PhysYawUnits += PhysRotationRateUnits.Yaw * DeltaSeconds;
		OwnRotation.Pitch = static_cast<double>(static_cast<int32>(PhysPitchUnits) & 0xFFFF)
			* UE1RotatorUnitsToDegrees;
		OwnRotation.Yaw = static_cast<double>(static_cast<int32>(PhysYawUnits) & 0xFFFF)
			* UE1RotatorUnitsToDegrees;
	}
	OwnRotation.Roll = static_cast<double>(RollUnits) * UE1RotatorUnitsToDegrees;
	SetActorRotation(OwnRotation);

	const FRotationMatrix RotationFrame(GetActorRotation());
	const FVector AxisX = RotationFrame.GetUnitAxis(EAxis::X);
	const FVector AxisY = RotationFrame.GetUnitAxis(EAxis::Y);
	const FVector AxisZ = RotationFrame.GetUnitAxis(EAxis::Z);
	const FVector Offset = CurrentHeight * AxisX + CurrentRadius * AxisY;
	const bool bUseDrivenOrigins = DrivenOrigins.Num() > 0;
	const FRotator UpdatedActorRotation =
		(CurrentRotationRate > 0.0f ? (-AxisZ).Rotation() : AxisZ.Rotation())
		+ ToDegrees(ActorRotationOffset);

	for (int32 ActorIndex = 0; ActorIndex < DrivenActors.Num(); ++ActorIndex)
	{
		AActor* DrivenActor = DrivenActors[ActorIndex].Get();
		if (!IsValid(DrivenActor) || HasStaticRoot(DrivenActor))
		{
			continue;
		}

		const FVector OrbitCenter = bUseDrivenOrigins && DrivenOrigins.IsValidIndex(ActorIndex)
			? DrivenOrigins[ActorIndex]
			: GetActorLocation();
		DrivenActor->SetActorLocation(OrbitCenter + Offset, false);

		if (bFaceIn)
		{
			SetDrivenRotation(DrivenActor,
				(OrbitCenter + CurrentHeight * AxisX
					- DrivenActor->GetActorLocation()).Rotation());
		}
		else if (bFaceOut)
		{
			SetDrivenRotation(DrivenActor,
				(DrivenActor->GetActorLocation()
					- (OrbitCenter + CurrentHeight * AxisX)).Rotation());
		}
		else if (bUpdateActorRotation)
		{
			SetDrivenRotation(DrivenActor, UpdatedActorRotation);
		}
	}
}

void AWoTActorRotator::UpdateRadius(float DeltaSeconds)
{
	if (CurrentRadius < NextRadius)
	{
		CurrentRadius += RadiusShiftRate * DeltaSeconds;
		if (CurrentRadius >= NextRadius)
		{
			CalcNextRadius();
		}
	}
	else
	{
		CurrentRadius -= RadiusShiftRate * DeltaSeconds;
		if (CurrentRadius <= NextRadius)
		{
			CalcNextRadius();
		}
	}
}

void AWoTActorRotator::UpdateRotationRate(float DeltaSeconds)
{
	if (CurrentRotationRate < NextRotationRate)
	{
		CurrentRotationRate += RotationShiftRate * DeltaSeconds;
		if (CurrentRotationRate >= NextRotationRate)
		{
			CalcNextRotationRate();
		}
	}
	else
	{
		CurrentRotationRate -= RotationShiftRate * DeltaSeconds;
		if (CurrentRotationRate <= NextRotationRate)
		{
			CalcNextRotationRate();
		}
	}
}

void AWoTActorRotator::UpdateHeight(float DeltaSeconds)
{
	if (CurrentHeight < NextHeight)
	{
		CurrentHeight += HeightShiftRate * DeltaSeconds;
		if (CurrentHeight >= NextHeight)
		{
			CalcNextHeight();
		}
	}
	else
	{
		CurrentHeight -= HeightShiftRate * DeltaSeconds;
		if (CurrentHeight <= NextHeight)
		{
			CalcNextHeight();
		}
	}
}

void AWoTActorRotator::CalcNextRadius()
{
	if (bBounceRadius)
	{
		NextRadius = NextRadius == MinRadius ? MaxRadius : MinRadius;
	}
	else
	{
		NextRadius = static_cast<float>(RandomStream.FRandRange(MinRadius, MaxRadius));
	}
}

void AWoTActorRotator::CalcNextRotationRate()
{
	if (bBounceRotationRate)
	{
		NextRotationRate = NextRotationRate == MinRotationRate
			? MaxRotationRate : MinRotationRate;
	}
	else
	{
		NextRotationRate = static_cast<float>(
			RandomStream.FRandRange(MinRotationRate, MaxRotationRate));
	}
}

void AWoTActorRotator::CalcNextHeight()
{
	if (bBounceHeight)
	{
		NextHeight = NextHeight == MinHeight ? MaxHeight : MinHeight;
	}
	else
	{
		NextHeight = static_cast<float>(RandomStream.FRandRange(MinHeight, MaxHeight));
	}
}

bool AWoTActorRotator::HasResolvedDrivenActor() const
{
	for (const TSoftObjectPtr<AActor>& DrivenActor : DrivenActors)
	{
		if (IsValid(DrivenActor.Get()))
		{
			return true;
		}
	}
	return false;
}

bool AWoTActorRotator::HasStaticRoot(AActor* DrivenActor)
{
	const USceneComponent* DrivenRoot = DrivenActor->GetRootComponent();
	if (!DrivenRoot || DrivenRoot->Mobility != EComponentMobility::Static)
	{
		return false;
	}

	const TWeakObjectPtr<AActor> ActorKey(DrivenActor);
	if (!WarnedStaticActors.Contains(ActorKey))
	{
		WarnedStaticActors.Add(ActorKey);
		UE_LOG(LogTemp, Warning,
			TEXT("ActorRotator %s cannot move %s because its root component is static."),
			*GetName(), *DrivenActor->GetName());
	}
	return true;
}

