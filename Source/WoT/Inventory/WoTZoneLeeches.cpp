#include "WoTZoneLeeches.h"
#include "../WoTRandom.h"

#include "../WoTCharacter.h"
#include "../WoTPlayerController.h"

namespace
{
AWoTPlayerController* ZoneLeechController(const UActorComponent* Leech)
{
	const APawn* Pawn = Leech ? Cast<APawn>(Leech->GetOwner()) : nullptr;
	return Pawn ? Cast<AWoTPlayerController>(Pawn->GetController()) : nullptr;
}
}

UWoTSmoothRandFOVLeech::UWoTSmoothRandFOVLeech()
{
	// SmoothRandFOVLeech.uc is simulated and explicitly client-side only.  Its
	// camera interpolation state is initialized by AttachTo, not replication.
	SetIsReplicatedByDefault(false);
	bRemovable = false;
}

void UWoTSmoothRandFOVLeech::AttachTo(APawn* NewHost)
{
	AWoTCharacter* Character = Cast<AWoTCharacter>(NewHost);
	if (!Character || Character->IsNPCPawn()) { DestroyComponent(); return; }
	Super::AttachTo(NewHost);
	if (!IsRegistered()) return;
	InitialFOV = CurrentFOV = Character->GetZoneFOVForLeech();
	SetNextFOV();
}

void UWoTSmoothRandFOVLeech::Unattach()
{
	if (AWoTPlayerController* PC = ZoneLeechController(this))
	{
		PC->ClientSetCameraFOV(InitialFOV);
	}
	Super::Unattach();
}

void UWoTSmoothRandFOVLeech::TickComponent(float DeltaTime, ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	APawn* Host = Cast<APawn>(GetOwner());
	if (Host && Host->IsLocallyControlled())
	{
		CurrentFOV += (bGrowing ? 1.0f : -1.0f) * FOVRate * DeltaTime;
		if ((bGrowing && CurrentFOV >= NextFOV) || (!bGrowing && CurrentFOV <= NextFOV))
		{
			SetNextFOV();
		}
		if (AWoTPlayerController* PC = ZoneLeechController(this))
		{
			PC->ClientSetCameraFOV(FMath::Clamp(CurrentFOV, 1.0f, 170.0f));
		}
	}
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
}

void UWoTSmoothRandFOVLeech::SetNextFOV()
{
	NextFOV = WoTRandom::FRandRange(InitialFOV - FluxFOV, InitialFOV + FluxFOV);
	bGrowing = NextFOV > CurrentFOV;
}

UWoTWaterFOVLeech::UWoTWaterFOVLeech()
{
	FluxFOV = 1.0f;
	FOVRate = 2.0f;
}

UWoTRandFOVLeech::UWoTRandFOVLeech()
{
	AffectResolution = 0.05f;
	bRemovable = false;
}

void UWoTRandFOVLeech::AttachTo(APawn* NewHost)
{
	AWoTCharacter* Character = Cast<AWoTCharacter>(NewHost);
	if (!Character || Character->IsNPCPawn()) { DestroyComponent(); return; }
	Super::AttachTo(NewHost);
	if (!IsRegistered()) return;
	InitialFOV = Character->GetZoneFOVForLeech();
	CurrentFOV = AverageFOV;
	if (AWoTPlayerController* PC = ZoneLeechController(this)) PC->ClientSetCameraFOV(CurrentFOV);
	RecalcDeltaFOV();
}

void UWoTRandFOVLeech::Unattach()
{
	if (AWoTPlayerController* PC = ZoneLeechController(this)) PC->ClientSetCameraFOV(InitialFOV);
	Super::Unattach();
}

void UWoTRandFOVLeech::AffectHost(int32 Iterations)
{
	if (!GetWorld()) return;
	if (GetWorld()->GetTimeSeconds() >= RecalcTime) RecalcDeltaFOV();
	CurrentFOV += DeltaFOV;
	if (AWoTPlayerController* PC = ZoneLeechController(this)) PC->ClientSetCameraFOV(CurrentFOV);
}

void UWoTRandFOVLeech::RecalcDeltaFOV()
{
	if (!GetWorld()) return;
	RecalcTime = GetWorld()->GetTimeSeconds() + FluxTime;
	const float Steps = FluxTime / FMath::Max(AffectResolution, KINDA_SMALL_NUMBER);
	DeltaFOV = (WoTRandom::FRandRange(AverageFOV - FluxFOV, AverageFOV + FluxFOV)
		- CurrentFOV) / Steps;
}

UWoTTimerLeech::UWoTTimerLeech()
{
	bDisplayIcon = true;
	bRemovable = false;
	Lifespan = 60.0f;
}
