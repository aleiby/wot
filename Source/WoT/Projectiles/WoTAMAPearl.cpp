#include "WoTAMAPearl.h"

#include "../Inventory/WoTAMAReflectors.h"
#include "../Inventory/WoTAngrealInventory.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../Inventory/WoTLeech.h"
#include "../Inventory/WoTReflector.h"
#include "../WoTCharacter.h"
#include "../WoTEvents.h"
#include "../WoTMeshModulate.h"
#include "../WoTPlayerController.h"
#include "../WoTSound.h"
#include "../WoTSoundTable.h"
#include "../WoTUE1Names.h"
#include "../WoTUE1LightRegistry.h"
#include "../WoTWeaveFX.h"
#include "../WoTVertexAnimComponent.h"
#include "WoTGenericProjectile.h"

#include "Components/SphereComponent.h"
#include "Components/PointLightComponent.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Net/UnrealNetwork.h"
#include "Kismet/GameplayStatics.h"
#include "UObject/SoftObjectPath.h"

AWoTAMAPearl::AWoTAMAPearl()
{
	DomeVisual = CreateDefaultSubobject<UWoTVertexAnimComponent>(TEXT("DomeVisual"));
	DomeVisual->SetupAttachment(RootComponent);
	DomeVisual->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	DomeVisual->bMeshCurvy = true;
	DomeLight = CreateDefaultSubobject<UPointLightComponent>(TEXT("DomeLight"));
	DomeLight->SetupAttachment(RootComponent);
	DomeLight->SetVisibility(false);

	// AMAPearl defaultproperties (simulation subset).
	InitialScale = 0.1f;
	FinalScale = 20.0f;
	InitialScaleRate = 40.0f;
	ScaleAccel = -20.0f;
	Duration = 8.0f;
	BaseAffectRadius = 28.0f;   // ProjCollisionRadius

	// Rides the base AngrealProjectile proximity poll, pawns only (Physics=PHYS_None
	// — the dome is stationary; the collision sphere is just the poll's anchor).
	bGenProjTouch = true;
	bTouchPawnsOnly = false;
	ProjTouchTime = 0.2f;   // AMAPearl poll interval (RCAMAPearl uses 0)
	Collision->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	ProjectileMovement->ProjectileGravityScale = 0.0f;
	bNetTemporary = false;
	bAlwaysRelevant = true;

	// AMAPearl.uc AmbientSound=AntiMagicAuraLoop4, SoundRadius=128 (volume/pitch
	// Actor defaults); the one-shots and eased LT_Steady/LE_Shell light bind below.
	AmbientLoopSound = FSoftObjectPath(
	    TEXT("/Game/WoT/Sounds/Angreal/AntiMagicAuraLoop4.AntiMagicAuraLoop4"));
	SoundRadius = 128;
	EndLightRadius = 25;
	Priority = 1;
	AffectSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/PawnWalksThruDome.PawnWalksThruDome"));
	UnAffectSound = AffectSound;
	AbsorbSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/UnravelAM.UnravelAM"));
	ActivateSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/ActivateAM.ActivateAM"));
	DeActivateSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/DeActivateAM.DeActivateAM"));
}

void AWoTAMAPearl::BeginPlay()
{
	Super::BeginPlay();
	if (GetNetMode() == NM_DedicatedServer || !IsValid(DomeVisual))
	{
		return;
	}
	FullLightIntensity = WoTUE1LightRegistry::Configure(
		DomeLight, TEXT("AMAPearl")) ? DomeLight->Intensity : 0.0f;

	// AMAPearl.uc defaults: Mesh=Angreal.AMAPearl, DrawScale=1. The #exec
	// MESHMAP SCALE is already baked into the imported geometry.
	BindWeaveMesh(DomeVisual, FName(TEXT("AMApearl")), 1.0f);
	DomeVisual->SetAllSkinOverrides(FSoftObjectPath(
		TEXT("/Game/WoT/Textures/Angreal/AMApearl01.AMApearl01")));
	const int32 MaterialCount = DomeVisual->GetNumMaterials();
	for (int32 Slot = 0; Slot < MaterialCount; ++Slot)
	{
		UMaterialInterface* OriginalMaterial = DomeVisual->GetMaterial(Slot);
		if (UMaterialInstanceDynamic* ModulatedMaterial =
			WoTMeshModulate::CreateInstance(this, OriginalMaterial))
		{
			// Missing generated content is tolerated: the MID retains the mesh's
			DomeVisual->SetMaterial(Slot, ModulatedMaterial);
		}
	}
}

void AWoTAMAPearl::Go()
{
	if (HasAuthority()) { bAutoGo = true; }
	// AMAPearl.Go: skip the Waiting state, start expanding from InitialScale.
	CurScale = InitialScale;
	SpawnTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
	ScaleRate = InitialScaleRate;
	ApplyAlpha(1.0f);
	ApplyScale();
	Phase = EPhase::Expanding;
	PlayOneShot(ActivateSound, GetActorLocation());
}

void AWoTAMAPearl::OnRep_AutoGo()
{
	if (bAutoGo && Phase == EPhase::Idle) { Go(); }
}

void AWoTAMAPearl::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWoTAMAPearl, bAutoGo);
}

void AWoTAMAPearl::ConfigureRemoveCurseVariant()
{
	FinalScale = 3.0f;
	InitialScaleRate = 20.0f;
	Duration = 2.5f;
	FadeTime = 0.2f;
	FadingExpandRate = 2.0f;
	EndLightRadius = 3;
	Priority = 0;
	ProjTouchTime = 0.0f;
	BaseScaleGlow = 0.3f;
	ScaleGlow = 0.3f;
	bNetTemporary = false;
	bStyleTranslucent = true;
}

AWoTRCAMAPearl::AWoTRCAMAPearl()
{
	ConfigureRemoveCurseVariant();
}

void AWoTAMAPearl::PlayOneShot(const TSoftObjectPtr<USoundBase>& Sound,
	const FVector& Location)
{
	// AMAPearl.uc:284/326: PlaySound inside simulated state code — each machine
	// plays its own copy (UE1's native PlaySound), volume 1, unset radius 1600.
	if (USoundBase* Loaded = WoTSound::Resolve(Sound))
	{
		WoTSound::PlayOneShot(this, Loaded, Location,
			WoTSound::DefaultPlaySoundRadius);
	}
}

void AWoTAMAPearl::PlayServerSound(const TSoftObjectPtr<USoundBase>& Sound,
	const FVector& Location)
{
	// AMAPearl.uc:182/506/544 (the authority-run absorb and AffectPawn /
	// UnAffectPawn paths here): one server play every in-range player hears.
	if (UWorld* World = GetWorld())
	{
		WoTSound::PlayReplicatedAsset(*World, Sound, Location,
			WoTSound::DefaultPlaySoundRadius);
	}
}

void AWoTAMAPearl::ApplyScale()
{
	// AMAPearl.SetSize: DrawScale and the affect radius track the same scalar.
	ProjCollisionRadius = BaseAffectRadius * CurScale;
	if (IsValid(DomeVisual))
	{
		DomeVisual->SetRelativeScale3D(FVector(CurScale));
	}
	CurrentLightRadius = (FinalScale > InitialScale)
		? EndLightRadius * (CurScale - InitialScale) / (FinalScale - InitialScale)
		: 0.0f;
	CurrentLightRadius = FMath::Clamp(CurrentLightRadius, 0.0f,
		static_cast<float>(EndLightRadius));
	if (DomeLight)
	{
		DomeLight->SetAttenuationRadius((CurrentLightRadius + 1.0f) * 25.0f);
	}
}

void AWoTAMAPearl::ApplyAlpha(float Scalar)
{
	CurrentAlphaScalar = FMath::Clamp(Scalar, 0.0f, 1.0f);
	if (DomeLight)
	{
		DomeLight->SetIntensity(FullLightIntensity * CurrentAlphaScalar);
	}
	if (DomeVisual)
	{
		for (int32 Slot = 0; Slot < DomeVisual->GetNumMaterials(); ++Slot)
		{
			if (UMaterialInstanceDynamic* MID =
				Cast<UMaterialInstanceDynamic>(DomeVisual->GetMaterial(Slot)))
			{
				MID->SetScalarParameterValue(TEXT("Glow"),
					BaseScaleGlow * CurrentAlphaScalar);
				MID->SetScalarParameterValue(TEXT("ScaleGlow"),
					BaseScaleGlow * CurrentAlphaScalar);
			}
		}
	}
}

void AWoTAMAPearl::Tick(float DeltaTime)
{
	// Advance the lifecycle first so the base proximity poll (Super::Tick) sees
	// this frame's affect radius.
	switch (Phase)
	{
	case EPhase::Expanding:
		ScaleRate += ScaleAccel * DeltaTime;
		if (ScaleAccel < 0.0f)
		{
			ScaleRate = FMath::Max(ScaleRate, 0.01f);   // never contract while growing
		}
		CurScale += ScaleRate * DeltaTime;
		if (CurScale >= FinalScale)
		{
			CurScale = FinalScale;
			EndTime = GetWorld()->GetTimeSeconds() + Duration;   // AMAPearl.uc:317
			HoldUntil = EndTime;
			Phase = EPhase::Expanded;
			for (TPair<TObjectPtr<APawn>, FWoTAMAAffectedPawnState>& Pair
				: AffectedPawns)
			{
				for (const TWeakObjectPtr<UWoTReflector>& Reflector
					: Pair.Value.InstalledReflectors)
				{
					if (Reflector.IsValid()) { Reflector->Lifespan = Duration; }
				}
			}
		}
		ApplyScale();
		break;

	case EPhase::Expanded:
		if (GetWorld()->GetTimeSeconds() >= HoldUntil)
		{
			PlayOneShot(DeActivateSound, GetActorLocation());
			// DT_Fade (both AMA and Remove Curse): fade out over FadeTime — the
			// glow ramps InitialAlpha->FinalAlpha in exactly FadeTime, so the phase
			// lasts FadeTime. HoldUntil is reused as the fade deadline.
			HoldUntil = GetWorld()->GetTimeSeconds() + FadeTime;
			Phase = EPhase::Fading;
		}
		break;

	case EPhase::Fading:
		// The dome keeps polling (Super::Tick) and GROWS as it fades, so a pawn that
		// steps in during the fade is still sealed — AMAPearl.uc:351-362.
		CurScale += FadingExpandRate * DeltaTime;
		CurrentAlphaScalar -= FadeTime > 0.0f ? DeltaTime / FadeTime : 1.0f;
		ApplyAlpha(CurrentAlphaScalar);
		ApplyScale();
		if (GetWorld()->GetTimeSeconds() >= HoldUntil)
		{
			Destroy();
			return;
		}
		break;

	default:
		break;
	}

	Super::Tick(DeltaTime);   // proximity poll -> ProjTouch/ProjUnTouch
}

void AWoTAMAPearl::ProjTouch(AActor* Other)
{
	if (!IsValid(Other) || Other == this) { return; }
	if (APawn* Victim = Cast<APawn>(Other))
	{
		AffectPawn(Victim);
	}
	else if (AWoTAngrealProjectile* Projectile = Cast<AWoTAngrealProjectile>(Other))
	{
		bool bYield = false;
		if (AWoTAMAPearl* OtherPearl = Cast<AWoTAMAPearl>(Projectile))
		{
			bYield = OtherPearl->Priority > Priority || OtherPearl->SpawnTime > SpawnTime;
		}
		const bool bExempt = WoTUE1Names::ActorIsA(Projectile, TEXT("MashadarGuide"))
			|| WoTUE1Names::ActorIsA(Projectile, TEXT("MachinShin"));
		if (HasAuthority() && !bYield && !bExempt)
		{
			const FVector At = Projectile->GetActorLocation();
			PlayServerSound(AbsorbSound, At);
			WoTWeaveFX::SpawnBurstMarker(*GetWorld(), TEXT("WOTSparks"), At,
				(At - GetActorLocation()).Rotation());
			if (AWoTGenericProjectile* Generic = Cast<AWoTGenericProjectile>(Projectile))
			{
				Generic->bExplode = false;
			}
			++AbsorptionCountForTest;
			Projectile->Destroy();
		}
	}
	if (HasAuthority())
	{
		if (IWoTTriggerable* Triggerable = Cast<IWoTTriggerable>(Other))
		{
			// AMAPearl.uc:196 checks UE1 Actor.Tag. GetUE1Tag is the
			// module's TagName surface; ActorHasTag remains a compatibility leg.
			if (Triggerable->GetUE1Tag() == FName(TEXT("AntiMagicAura"))
				|| Other->ActorHasTag(TEXT("AntiMagicAura")))
			{
				Triggerable->WoTTrigger(this, GetInstigator());
				++AntiMagicTriggerCountForTest;
			}
		}
	}
}

void AWoTAMAPearl::ProjUnTouch(AActor* Other)
{
	if (APawn* Victim = Cast<APawn>(Other))
	{
		UnAffectPawn(Victim);
	}
}

void AWoTAMAPearl::AffectPawn(APawn* Victim)
{
	if (!HasAuthority() || !IsValid(Victim))
	{
		return;
	}

	AWoTCharacter* Ch = Cast<AWoTCharacter>(Victim);

	// Stop the victim's current cast (before the NoCast seal goes up).
	if (Ch && Ch->InventoryComponent)
	{
		Ch->InventoryComponent->CeaseUsingAngreal();
	}

	// RemoveCurseEffect: strip the victim's bad weaves — every removable leech not
	// sourced by our own artifact — then untaint their artifacts.
	TArray<UWoTLeech*> Leeches;
	Victim->GetComponents<UWoTLeech>(Leeches);
	for (UWoTLeech* L : Leeches)
	{
		if (IsValid(L) && L->bRemovable
			&& (L->SourceAngreal == nullptr || L->SourceAngreal != SourceAngreal))
		{
			L->Unattach();
		}
	}

	// ...and every removable reflector not ours (bRemovable now on UWoTReflector).
	{
		TArray<UWoTReflector*> Reflectors;
		Victim->GetComponents<UWoTReflector>(Reflectors);
		for (UWoTReflector* R : Reflectors)
		{
			if (IsValid(R) && R->bRemovable
				&& (R->SourceAngreal == nullptr || R->SourceAngreal != SourceAngreal))
			{
				R->Uninstall();
			}
		}
	}

	// Untaint every artifact the victim carries.
	if (Ch && Ch->InventoryComponent)
	{
		for (const TObjectPtr<AWoTInventory>& Item : Ch->InventoryComponent->Items)
		{
			if (AWoTAngrealInventory* Ang = Cast<AWoTAngrealInventory>(Item))
			{
				Ang->bTainted = false;
			}
		}
	}

	// Seal the dead-zone: NoCast (can't cast) + NoBadEffects (no harmful effect
	// lands), Priority 255, for the dome's Duration. Track them for removal on exit.
	// Late entry (AMAPearl.uc:480-493): past the Expanding state the seal only
	// lasts the dome's REMAINING hold time (EndTime - now). A fade-phase entry is
	// already past EndTime — retail's negative LifeSpan expires next tick; our
	// Lifespan treats <=0 as permanent, so clamp to a hair above zero instead.
	float SealLife = Duration;
	if (Phase != EPhase::Expanding)
	{
		SealLife = FMath::Max(EndTime - GetWorld()->GetTimeSeconds(), KINDA_SMALL_NUMBER);
	}
	FWoTAMAAffectedPawnState& Affected = AffectedPawns.FindOrAdd(Victim);
	TArray<TWeakObjectPtr<UWoTReflector>>& Installed = Affected.InstalledReflectors;
	auto InstallOne = [&](TSubclassOf<UWoTReflector> Class)
	{
		UWoTReflector* R = NewObject<UWoTReflector>(Victim, Class);
		R->SetSourceAngreal(SourceAngreal);
		// Retail routes the seal through InstallReflectorEffect.InitializeWith
		// Projectile (AMAPearl.uc:461) — InstallReflectorEffect.uc:52 marks the
		// reflector bFromProjectile: Swap Places must not transfer the dome's seal.
		R->bFromProjectile = true;
		R->Lifespan = SealLife;
		R->Install(Victim);
		Installed.Add(R);
	};
	InstallOne(UWoTNoCastReflector::StaticClass());
	InstallOne(UWoTNoBadEffectsReflector::StaticClass());
	if (AWoTPlayerController* PC = Cast<AWoTPlayerController>(Victim->GetController()))
	{
		PC->ClientAdjustGlow(4.0f, FVector(150.0f, 100.0f, 200.0f));
		// AMAPearl.uc:497-523 makes +4/-4 an exact pair. Remember the
		// recipient because death can unpossess before dome exit
		// (WoTGameMode.cpp:302; WoTPlayerController.h:166-171).
		Affected.GlowController = PC;
	}
	++PawnPassCountForTest;
	PlayServerSound(AffectSound, Victim->GetActorLocation());
}

void AWoTAMAPearl::UnAffectPawn(APawn* Victim)
{
	// AMAPearl.uc:513 is non-simulated: clients must not apply the inverse
	// glow or exit sound in addition to the server RPC.
	if (!HasAuthority())
	{
		return;
	}
	FWoTAMAAffectedPawnState* Affected = AffectedPawns.Find(Victim);
	if (!Affected && !IsValid(Victim)) { return; }
	AWoTPlayerController* PC = Affected ? Affected->GlowController.Get() : nullptr;
	if (!IsValid(PC) && IsValid(Victim))
	{
		PC = Cast<AWoTPlayerController>(Victim->GetController());
	}
	if (IsValid(PC))
	{
		PC->ClientAdjustGlow(-4.0f, FVector(-150.0f, -100.0f, -200.0f));
	}
	++PawnPassCountForTest;
	if (IsValid(Victim)) { PlayServerSound(UnAffectSound, Victim->GetActorLocation()); }
	if (Affected)
	{
		for (const TWeakObjectPtr<UWoTReflector>& R : Affected->InstalledReflectors)
		{
			if (R.IsValid())
			{
				R->Uninstall();
			}
		}
	}
	AffectedPawns.Remove(Victim);
}

void AWoTAMAPearl::EndPlay(const EEndPlayReason::Type Reason)
{
	// Unaffect everyone still inside so reflectors, tint, and exit sound all unwind.
	TArray<TObjectPtr<APawn>> Victims;
	AffectedPawns.GetKeys(Victims);
	for (const TObjectPtr<APawn>& Victim : Victims)
	{
		UnAffectPawn(Victim.Get());
	}
	AffectedPawns.Empty();
	Super::EndPlay(Reason);
}
