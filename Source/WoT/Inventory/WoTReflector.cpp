#include "WoTReflector.h"

#include "WoTAngrealInventory.h"
#include "../WoTCharacter.h"
#include "../WoTPlayerController.h"

#include "GameFramework/Pawn.h"
#include "Net/UnrealNetwork.h"

namespace
{
	uint64 GWoTReflectorInstallSequence = 0;
}

UWoTReflector::UWoTReflector()
{
	PrimaryComponentTick.bCanEverTick = true;
	SetIsReplicatedByDefault(true);
}

void UWoTReflector::Install(APawn* NewHost)
{
	if (!IsValid(NewHost))
	{
		return;
	}

	// Retail installs a reflector by routing an InstallReflectorEffect through the
	// victim's ProcessEffect (InstallReflectorEffect.uc:44-56, bDeleterious
	// inherited from the reflector class) — so a NoBadEffects dome or a matching
	// element shield throws a deleterious install away (a Soul Barb never lands on
	// a Spirit-shielded victim). A Swap Places transfer re-installs directly
	// without re-processing — exempt it, mirroring UWoTLeech::AttachTo.
	if (!bTransferring)
	{
		if (AWoTCharacter* Ch = Cast<AWoTCharacter>(NewHost))
		{
			if (Ch->IsEffectBlocked(SourceAngreal, bDeleterious,
				/*bEmitFeedback*/ true))
			{
				DestroyComponent();
				return;
			}
		}
	}

	// Remove existing reflectors of our class (bRemoveExisting).
	if (bRemoveExisting)
	{
		TArray<UWoTReflector*> Existing;
		NewHost->GetComponents<UWoTReflector>(Existing);
		for (UWoTReflector* R : Existing)
		{
			if (R != this && IsValid(R) && R->GetClass() == GetClass())
			{
				R->Uninstall();
			}
		}
	}

	InstallSeq = ++GWoTReflectorInstallSequence;

	if (!IsRegistered())
	{
		RegisterComponent();
	}
	NotifyExpireTimer = Lifespan - NotifyExpireTime;
	if (GetOwnerRole() == ROLE_Authority)
	{
		if (AWoTCharacter* Character = Cast<AWoTCharacter>(NewHost))
			Character->RefreshTouchReflectorDescriptions();
	}

	// Reflector.uc:306-315: authority-only, with the base-authored
	// bDisplayIcon=True unless a concrete internal reflector suppresses it.
	if (GetOwnerRole() == ROLE_Authority && bDisplayIcon)
	{
		if (AWoTCharacter* Character = Cast<AWoTCharacter>(NewHost);
			IsValid(Character) && !Character->IsNPCPawn())
		{
			Character->AddIconInfo(this, !bDeleterious, GetFName());
		}
	}
}

void UWoTReflector::Uninstall()
{
	RemoveStatusIconFromCurrentHost();
	if (GetOwnerRole() == ROLE_Authority)
	{
		if (AWoTCharacter* Character = Cast<AWoTCharacter>(GetOwner()))
			Character->RefreshTouchReflectorDescriptions(this);
	}
	DestroyComponent();
}

void UWoTReflector::SetPriority(uint8 NewPriority)
{
	if (Priority == NewPriority) return;
	Priority = NewPriority;
	if (GetOwnerRole() == ROLE_Authority && IsRegistered())
	{
		if (AWoTCharacter* Character = Cast<AWoTCharacter>(GetOwner()))
			Character->RefreshTouchReflectorDescriptions();
	}
}

void UWoTReflector::OnComponentDestroyed(bool bDestroyingHierarchy)
{
	if (GetOwnerRole() == ROLE_Authority)
	{
		if (AWoTCharacter* Character = Cast<AWoTCharacter>(GetOwner()))
			Character->RefreshTouchReflectorDescriptions(this);
	}
	Super::OnComponentDestroyed(bDestroyingHierarchy);
}

void UWoTReflector::RemoveStatusIconFromCurrentHost()
{
	if (GetOwnerRole() == ROLE_Authority && bDisplayIcon)
	{
		if (AWoTCharacter* Character = Cast<AWoTCharacter>(GetOwner());
			IsValid(Character) && !Character->IsNPCPawn())
		{
			Character->RemoveIconInfo(GetFName());
		}
	}
}

void UWoTReflector::Expired()
{
	Uninstall();
}

void UWoTReflector::SetSourceAngreal(AWoTAngrealInventory* Source)
{
	SourceAngreal = Source;
	if (Source)
	{
		ReflectorInstigator = Cast<APawn>(Source->GetOwner());
	}
}

void UWoTReflector::TickComponent(float DeltaTime, ELevelTick TickType,
                                  FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (GetOwnerRole() == ROLE_Authority && Lifespan > 0.0f)
	{
		Lifespan -= DeltaTime;
		if (Lifespan <= 0.0f)
		{
			Expired();
			return;
		}
		NotifyExpireTimer -= DeltaTime;
		if (NotifyExpireTimer < 0.0f)
		{
			NotifyExpireTimer += NotifyExpireResolution;
			NotifyOwnerExpiring();
		}
	}
}

void UWoTReflector::NotifyOwnerExpiring()
{
	if (!bDisplayIcon)
	{
		return;
	}
	if (const APawn* Host = Cast<APawn>(GetOwner()))
	{
		if (AWoTPlayerController* PC =
			Cast<AWoTPlayerController>(Host->GetController()))
		{
			PC->ClientPlayUISound(TEXT("WOT/DefaultExpiring"), NotifyExpireVolume);
		}
	}
	NotifyExpireVolume *= 0.5f;
}

void UWoTReflector::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UWoTReflector, SourceAngreal);
	DOREPLIFETIME(UWoTReflector, InstallSeq);
}
