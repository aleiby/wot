#include "WoTAngrealInvLightGlobe.h"

#include "../WoTCharacter.h"
#include "../WoTMeshFX.h"
#include "../WoTPlayerColorFX.h"

#include "Engine/World.h"
#include "Net/UnrealNetwork.h"

AWoTAngrealInvLightGlobe::AWoTAngrealInvLightGlobe()
{
	BindStatusIcons(TEXT("I_LtGlobe"), TEXT("M_LtGlobe"));

	// AngrealInvLightGlobe defaultproperties.
	LightType = AWoTLightGlobe::StaticClass();
	LightGlobeGlowOffset = FVector(0.0f, 7.0f, 3.0f);
	bElementFire = true;
	bElementAir = true;
	bRare = true;
	MinInitialCharges = 999;
	MaxInitialCharges = 999;
	ChargeCost = 0;
	Priority = 100.0f;
	ActivateSound = FSoftObjectPath(
		TEXT("/Game/WoT/Sounds/Angreal/ActivateLG.ActivateLG"));
	InventoryGroup = 51;
}

void AWoTAngrealInvLightGlobe::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWoTAngrealInvLightGlobe, LightType);
}

void AWoTAngrealInvLightGlobe::BeginPlay()
{
	Super::BeginPlay();
	if (!TagName.IsNone())
	{
		GetWorld()->GetSubsystem<UWoTEventSubsystem>()->RegisterTag(TagName, this);
	}
}

void AWoTAngrealInvLightGlobe::CastArtifact()
{
	if (!Light)
	{
		UWorld* World = GetWorld();
		if (!World)
		{
			return;
		}
		UClass* Type = LightType ? *LightType : AWoTLightGlobe::StaticClass();
		AWoTLightGlobe* NewLight = World->SpawnActor<AWoTLightGlobe>(Type);
		if (!NewLight)
		{
			return;
		}
		Light = NewLight;
		Light->Follow(GetOwner());
		Super::CastArtifact();
		UseCharge();
	}
	else
	{
		// Second activation unravels: Follow(None) destroys the globe on the
		// authority, then the retail explicit Super.UnCast().
		Light->Follow(nullptr);
		Light = nullptr;
		Super::UnCastArtifact();
	}
}

void AWoTAngrealInvLightGlobe::UnCastArtifact()
{
	// The light survives artifact switching; only a dead owner turns it off
	// (via the Cast toggle). Deliberately no Super — retail UnCast comments
	// out the base call.
	const AWoTCharacter* OwnerCh = Cast<AWoTCharacter>(GetOwner());
	if (OwnerCh && OwnerCh->Health <= 0 && Light)
	{
		CastArtifact();
	}
}

void AWoTAngrealInvLightGlobe::WoTTrigger(AActor*, APawn*)
{
	// The SP tutorial's Trigger->Tag author: a lit globe is unraveled.
	if (Light)
	{
		CastArtifact();
	}
}

void AWoTAngrealInvLightGlobe::GoEmpty()
{
	if (Light)
	{
		Light->Destroy();
		Light = nullptr;
	}
	Super::GoEmpty();
}

void AWoTAngrealInvLightGlobe::DropFrom(FVector StartLocation)
{
	if (Light)
	{
		Light->Destroy();
		Light = nullptr;
	}
	// Retail DropFrom calls the BASE UnCast (not this class's dead-owner leg).
	Super::UnCastArtifact();
	Super::DropFrom(StartLocation);
}

void AWoTAngrealInvLightGlobe::GiveTo(APawn* Other)
{
	Super::GiveTo(Other);
	// Retail: WOTPlayer.PlayerColor, else the mesh-name faction map
	// (AesSedai Blue / Forsaken Red / Whitecloak Gold / Hound Purple / else
	// Green) — exactly the shared Dart seam (Gold and Yellow fold together in
	// SetColor).
	SetColor(WoTPlayerColorFX::ResolveDart(Other));
}

void AWoTAngrealInvLightGlobe::SetColor(FName ColorName)
{
	const FString Name = ColorName.ToString();
	if (Name == TEXT("Red") || Name == TEXT("PC_Red"))
	{
		LightType = AWoTLightGlobeRed::StaticClass();
	}
	else if (Name == TEXT("Yellow") || Name == TEXT("PC_Gold") || Name == TEXT("Gold"))
	{
		LightType = AWoTLightGlobeYellow::StaticClass();
	}
	else if (Name == TEXT("PC_Green") || Name == TEXT("Green"))
	{
		LightType = AWoTLightGlobeGreen::StaticClass();
	}
	else if (Name == TEXT("PC_Purple") || Name == TEXT("Purple"))
	{
		LightType = AWoTLightGlobePurple::StaticClass();
	}
	else
	{
		// PC_Blue / Blue / anything else — the retail default case.
		LightType = AWoTLightGlobeBlue::StaticClass();
	}
}

void AWoTAngrealInvLightGlobe::BecomePickup()
{
	Super::BecomePickup();
	UpdateGlobeStar();
}

void AWoTAngrealInvLightGlobe::BecomeItem()
{
	Super::BecomeItem();
	UpdateGlobeStar();
}

void AWoTAngrealInvLightGlobe::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	// The star orbits the spinning pickup at the rotated offset.
	if (Globe && GetItemState() == EWoTInventoryState::Pickup)
	{
		Globe->SetActorLocation(StarLocation());
	}
}

void AWoTAngrealInvLightGlobe::Destroyed()
{
	if (Globe)
	{
		Globe->Destroy();
		Globe = nullptr;
	}
	if (HasAuthority() && Light)
	{
		Light->Follow(nullptr);
		Light = nullptr;
	}
	Super::Destroyed();
}

float AWoTAngrealInvLightGlobe::GetPriority() const
{
	// Retail gates the on/off legs on WaysZone occupancy; WaysZone is authored
	// only by the SP Mission_08 maps and no NPC MP loadout carries this
	// artifact, so the MP-reachable behavior is exactly the every-other-case
	// return: don't do anything (SP rider in PORT_NOTES).
	return -static_cast<float>(MAX_int32);
}

FVector AWoTAngrealInvLightGlobe::StarLocation() const
{
	return GetActorLocation()
		+ GetActorRotation().RotateVector(LightGlobeGlowOffset);
}

void AWoTAngrealInvLightGlobe::UpdateGlobeStar()
{
	// Retail BecomePickup/BecomeItem + the client bAmPickup dance collapse to
	// this per-machine state read (ApplyItemStatePresentation already routes
	// both authority and proxy through the Become* hooks). Sleeping keeps the
	// star hidden — the same visible-pickup gate as the AppearEffect rings.
	const bool bVisiblePickup = GetItemState() == EWoTInventoryState::Pickup;
	if (bVisiblePickup)
	{
		if (!Globe)
		{
			if (UWorld* World = GetWorld())
			{
				Globe = WoTMeshFX::SpawnSpriteLocal(*World,
					TEXT("LightGlobeStar"), StarLocation());
			}
		}
		if (Globe)
		{
			Globe->SetActorHiddenInGame(false);
			Globe->SetActorLocation(StarLocation());
		}
	}
	else if (Globe)
	{
		Globe->SetActorHiddenInGame(true);
	}
}

AWoTAngrealInvLightGlobe* AWoTAngrealInvLightGlobe::ReplaceWithBaseColor(
	FName VariantColor)
{
	if (!HasAuthority())
	{
		return nullptr;
	}
	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}
	FActorSpawnParameters Params;
	Params.Owner = GetOwner();
	AWoTAngrealInvLightGlobe* Replacement =
		World->SpawnActor<AWoTAngrealInvLightGlobe>(
			AWoTAngrealInvLightGlobe::StaticClass(), GetActorLocation(),
			GetActorRotation(), Params);
	if (!Replacement)
	{
		return nullptr;
	}
	Replacement->SetColor(VariantColor);
	Replacement->MinInitialCharges = MinInitialCharges;
	Replacement->MaxInitialCharges = MaxInitialCharges;
	Replacement->MaxCharges = MaxCharges;
	Replacement->ChargeCost = ChargeCost;
	// Retail passes the placed Tag into Spawn; the replacement's BeginPlay has
	// already run, so register the carried tag directly.
	Replacement->TagName = TagName;
	if (!TagName.IsNone())
	{
		World->GetSubsystem<UWoTEventSubsystem>()->RegisterTag(TagName, Replacement);
	}
	Destroy();
	return Replacement;
}
