#include "WoTDistantEyeLeech.h"

#include "../WoTPlayerController.h"

#include "GameFramework/Pawn.h"

UWoTDistantEyeLeech::UWoTDistantEyeLeech()
{
	DistantEyeFOV = 120.0f;
	GlowScale = 0.0f;
	GlowFog = FVector(281.25f, 421.875f, 140.625f);
	Lifespan = 0.0f;
}

void UWoTDistantEyeLeech::AttachTo(APawn* NewHost)
{
	Super::AttachTo(NewHost);
	if (!IsRegistered() || GetOwnerRole() != ROLE_Authority)
	{
		return;
	}

	APawn* Host = Cast<APawn>(GetOwner());
	AWoTPlayerController* PC = Host
		? Cast<AWoTPlayerController>(Host->GetController()) : nullptr;
	if (PC && !bViewAdjusted)
	{
		bViewAdjusted = true;
		AdjustedController = PC;
		PC->ClientSetCameraFOV(DistantEyeFOV);
		PC->ClientAdjustGlow(GlowScale, GlowFog);
	}
}

void UWoTDistantEyeLeech::OnHostDetached(APawn* OldHost)
{
	if (!bViewAdjusted)
	{
		return;
	}
	bViewAdjusted = false;
	AWoTPlayerController* PC = IsValid(AdjustedController)
		? AdjustedController.Get()
		: (OldHost ? Cast<AWoTPlayerController>(OldHost->GetController()) : nullptr);
	if (PC)
	{
		PC->ClientAdjustGlow(-GlowScale, -GlowFog);
		PC->ClientRestoreCameraFOV();
	}
	AdjustedController = nullptr;
}

void UWoTDistantEyeLeech::Unattach()
{
	OnHostDetached(Cast<APawn>(GetOwner()));
	Super::Unattach();
}

void UWoTDistantEyeLeech::OnComponentDestroyed(bool bDestroyingHierarchy)
{
	OnHostDetached(Cast<APawn>(GetOwner()));
	Super::OnComponentDestroyed(bDestroyingHierarchy);
}
