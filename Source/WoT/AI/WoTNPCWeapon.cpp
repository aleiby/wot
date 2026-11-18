#include "WoTNPCWeapon.h"
#include "../WoTRandom.h"

#include "WoTNPCCharacter.h"
#include "WoTNPCWeaponData.h"
#include "../Projectiles/WoTWeaponProjectile.h"
#include "../WoTCharacter.h"
#include "../WoTCollisionBlocker.h"
#include "../WoTFallingPhysics.h"
#include "../WoTMover.h"
#include "../WoTPayloadSubsystem.h"
#include "../World/WoTRuntimeClassDefaults.h"
#include "../World/WoTRuntimeWorldProperties.h"
#include "../WoTSound.h"
#include "../WoTSoundTable.h"
#include "../WoTVertexAnimComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/AssetManager.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Engine/Texture.h"
#include "Net/UnrealNetwork.h"
#if !WOT_GUARDED_BOOTSTRAP
#include "../RetailData/WoTDevRetailFamilies.h"
#endif

AWoTNPCWeapon::AWoTNPCWeapon()
{
	PrimaryActorTick.bCanEverTick = false;
	WeaponMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("WeaponMesh"));
	WeaponMesh->SetCollisionProfileName(TEXT("NoCollision"));
	WeaponMesh->SetGenerateOverlapEvents(false);
	RootComponent = WeaponMesh;
	DisplayMesh = CreateDefaultSubobject<UWoTVertexAnimComponent>(TEXT("DisplayMesh"));
	DisplayMesh->SetupAttachment(WeaponMesh);
	DisplayMesh->SetCollisionProfileName(TEXT("NoCollision"));
	DisplayMesh->SetGenerateOverlapEvents(false);

	bReplicates = true;
	SetReplicateMovement(true); // attachment + relative transform must be seen by clients
}

void AWoTNPCWeapon::PostInitializeComponents()
{
	Super::PostInitializeComponents();
	// PostLoad/profile application may clobber constructor-only collision setup.
	// WotWeapon is held inventory, never a collision participant (brief A).
	if (WeaponMesh)
	{
		WeaponMesh->SetCollisionProfileName(TEXT("NoCollision"));
		WeaponMesh->SetGenerateOverlapEvents(false);
	}
	if (DisplayMesh)
	{
		DisplayMesh->SetCollisionProfileName(TEXT("NoCollision"));
		DisplayMesh->SetGenerateOverlapEvents(false);
	}
}

void AWoTNPCWeapon::ConfigureFromType(FName WeaponType)
{
	WeaponTypeName = WeaponType;
	if (const FWoTNPCWeaponConfig* Config = WoTNPCWeaponData::Find(WeaponType))
	{
		ApplyConfig(*Config);
	}
}

void AWoTNPCWeapon::OnRep_WeaponType()
{
	if (const FWoTNPCWeaponConfig* Config = WoTNPCWeaponData::Find(WeaponTypeName))
	{
		ApplyConfig(*Config);
	}
}

void AWoTNPCWeapon::ApplyConfig(const FWoTNPCWeaponConfig& Config)
{
	MinMeleeRange = Config.MinMeleeRange;
	MaxMeleeRange = Config.MaxMeleeRange;
	MeleeEffectiveness = Config.MeleeEffectiveness;
	MinProjectileRange = Config.MinProjectileRange;
	MaxProjectileRange = Config.MaxProjectileRange;
	ProjectileEffectiveness = Config.ProjectileEffectiveness;
	ProjectileClass = Config.ProjectileClass;
	bRequireTargetVisibility = Config.bRequireTargetVisibility;
	MissOdds = Config.MissOdds;
	TypeCapableOfPreparation = Config.TypeCapableOfPreparation;
	DefaultHealth = Config.Health;
	if (HasAuthority() || Health == 0)
	{
		Health = Config.Health;
	}
	DamageSkin0 = WoTNPCWeaponData::ResolveTexturePath(Config.DamageSkin0);
	DamageSkin1 = WoTNPCWeaponData::ResolveTexturePath(Config.DamageSkin1);
	DamageSkin2 = WoTNPCWeaponData::ResolveTexturePath(Config.DamageSkin2);
	DestroyedSound = WoTNPCWeaponData::ResolveSoundPath(Config.DestroyedSound);
	WeaponMeshAsset = WoTNPCWeaponData::ResolveMeshPath(Config.Mesh);
	SkinOverrides.Reset(Config.SkinOverrides.Num());
	TArray<FSoftObjectPath> SkinPaths;
	for (const FName SkinOverride : Config.SkinOverrides)
	{
		TSoftObjectPtr<UTexture> Resolved(WoTNPCWeaponData::ResolveTexturePath(SkinOverride));
		if (!Resolved.IsNull())
		{
			SkinPaths.Add(Resolved.ToSoftObjectPath());
		}
		SkinOverrides.Add(MoveTemp(Resolved));
	}
	bStyleMasked = Config.bStyleMasked;
	if (HasAuthority())
	{
		ScaleGlow = Config.ScaleGlow;
	}
	TossedDecorationType = Config.TossedDecorationType;
	MPTossedLifeSpan = Config.MPTossedLifeSpan;
	if (!DestroyedSound.IsNull())
	{
		UAssetManager::GetStreamableManager().RequestAsyncLoad(DestroyedSound.ToSoftObjectPath());
	}
	if (SkinPaths.Num() > 0)
	{
		const TWeakObjectPtr<AWoTNPCWeapon> WeakThis(this);
		UAssetManager::GetStreamableManager().RequestAsyncLoad(
			SkinPaths, FStreamableDelegate::CreateLambda([WeakThis]()
			{
				if (AWoTNPCWeapon* Weapon = WeakThis.Get())
				{
					Weapon->ApplyCurrentSkinVisual();
				}
			}));
	}
	if (const AWoTWeaponProjectile* ProjectileCDO = ProjectileClass
		? ProjectileClass->GetDefaultObject<AWoTWeaponProjectile>() : nullptr)
	{
		TArray<FSoftObjectPath> ProjectileSounds;
		if (!ProjectileCDO->SpawnSound.IsNull()) { ProjectileSounds.Add(ProjectileCDO->SpawnSound.ToSoftObjectPath()); }
		if (!ProjectileCDO->SoundHitPawn.IsNull()) { ProjectileSounds.Add(ProjectileCDO->SoundHitPawn.ToSoftObjectPath()); }
		if (!ProjectileCDO->SoundHitWall.IsNull()) { ProjectileSounds.Add(ProjectileCDO->SoundHitWall.ToSoftObjectPath()); }
		if (!ProjectileCDO->AmbientLoopSound.IsNull()) { ProjectileSounds.Add(ProjectileCDO->AmbientLoopSound.ToSoftObjectPath()); }
		if (ProjectileSounds.Num() > 0)
		{
			UAssetManager::GetStreamableManager().RequestAsyncLoad(ProjectileSounds);
		}
	}
	ApplyVisuals();
	if (HasAuthority())
	{
		RefreshOddsRoll();
	}
	if (DefaultHealth > 0 && Health < DefaultHealth)
	{
		ApplyDamageSkin();
	}
}

void AWoTNPCWeapon::ApplyVisuals()
{
	if (!WeaponMesh)
	{
		return;
	}
	WeaponMesh->SetRelativeScale3D(FVector(1.0f));
	if (const FWoTNPCWeaponConfig* Config = WoTNPCWeaponData::Find(WeaponTypeName))
	{
		WeaponMesh->SetRelativeScale3D(FVector(Config->DrawScale));
	}
#if WOT_GUARDED_BOOTSTRAP
	if (DisplayMesh && DisplayMesh->SetVertexMeshPath(
		WeaponMeshAsset.ToSoftObjectPath()))
	{
		ApplyCurrentSkinVisual();
	}
#else
	if (WeaponMeshAsset.IsValid())
	{
		DisplayMesh->SetSkeletalMeshAsset(WeaponMeshAsset.Get());
		ApplyCurrentSkinVisual();
	}
	else if (!WeaponMeshAsset.IsNull())
	{
		// Soft-reference policy: request streaming and never block gameplay on a
		// hard object load. The weak capture tolerates destruction during IO.
		const TWeakObjectPtr<AWoTNPCWeapon> WeakThis(this);
		UAssetManager::GetStreamableManager().RequestAsyncLoad(
			WeaponMeshAsset.ToSoftObjectPath(), FStreamableDelegate::CreateLambda([WeakThis]()
			{
				if (AWoTNPCWeapon* Weapon = WeakThis.Get(); Weapon && Weapon->DisplayMesh &&
					Weapon->WeaponMeshAsset.IsValid())
				{
					Weapon->DisplayMesh->SetSkeletalMeshAsset(Weapon->WeaponMeshAsset.Get());
					Weapon->ApplyCurrentSkinVisual();
				}
			}));
	}
#endif
	WeaponMesh->SetVisibility(bEquipped && !WeaponMeshAsset.IsNull(), true);
	ApplyFadePresentation();
}

bool AWoTNPCWeapon::CanUseAsMeleeWeapon(float GoalDistance) const
{
	return GoalDistance >= MinMeleeRange && GoalDistance <= MaxMeleeRange; // uc:348
}

bool AWoTNPCWeapon::CanUseAsProjectileWeapon(float GoalDistance) const
{
	return GoalDistance >= MinProjectileRange && GoalDistance <= MaxProjectileRange &&
		ProjectileClass != nullptr; // uc:358-366
}

// Util.uc:1007-1038 behavior-exact, INCLUDING the shipped edge quirk: with inverted odds
// (MaxOdds < MinOdds, the QuestionerShield case) the pair is swapped and the interior
// interpolates from the high odds at MinDistance down to the low odds at MaxDistance,
// but the boundary branches still return the POST-SWAP values — so exactly at/below
// MinDistance the roll gets the LOW odds and at/beyond MaxDistance the HIGH odds
// (a 4096+uu Questioner throws at 0.5, not 0.01). Parity-locked; do not smooth.
float AWoTNPCWeapon::RandDistance(float Distance, float MinDistance, float MaxDistance,
	float MinOdds, float MaxOdds)
{
	const bool bInverted = MaxOdds < MinOdds;
	const float LowOdds = bInverted ? MaxOdds : MinOdds;
	const float HighOdds = bInverted ? MinOdds : MaxOdds;
	const float DistanceFromLowOdds = bInverted
		? MaxDistance - Distance
		: Distance - MinDistance;

	if (Distance >= MaxDistance)
	{
		return HighOdds;
	}
	if (Distance <= MinDistance)
	{
		return LowOdds;
	}
	const float OddsSpan = HighOdds - LowOdds;
	const float DistanceSpan = MaxDistance - MinDistance;
	const float ClampedDistance = FMath::IsNaN(DistanceFromLowOdds)
		? DistanceFromLowOdds
		: FMath::Clamp(DistanceFromLowOdds, 0.0f, DistanceSpan);
	const float OddsPerDistance = OddsSpan / DistanceSpan;
	return ClampedDistance * OddsPerDistance + LowOdds;
}

void AWoTNPCWeapon::DetermineWeaponUsage(float GoalDistance, bool bGoalVisible, float Random01)
{
	if (GetWorld() && !HasAuthority())
	{
		DeterminedWeaponUsage = EWoTWeaponUsage::None;
		return;
	}
	DeterminedWeaponUsage = UWoTWeaponComponent::ClassifyWeaponUsage(
		GoalDistance, bGoalVisible, MinMeleeRange, MaxMeleeRange, MeleeEffectiveness,
		MinProjectileRange, MaxProjectileRange, ProjectileEffectiveness,
		ProjectileClass != nullptr, bRequireTargetVisibility);

	// QuestionerShield.uc:43-67: a projectile classification is only retained
	// when the distance-shaped throw roll succeeds. Random01 is injectable.
	const FWoTNPCWeaponConfig* Config = WoTNPCWeaponData::Find(WeaponTypeName);
	if (DeterminedWeaponUsage == EWoTWeaponUsage::Projectile && Config &&
		Config->bQuestionerOddsGate)
	{
		const float Roll = Random01 >= 0.0f ? Random01 : WoTRandom::FRand();
		const float Odds = RandDistance(GoalDistance, 128.0f, 4096.0f, 0.5f, 0.01f);
		if (Roll > Odds)
		{
			DeterminedWeaponUsage = EWoTWeaponUsage::None;
		}
	}
}

void AWoTNPCWeapon::RefreshOddsRoll(float Random01)
{
	CachedOddsRoll = Random01 >= 0.0f ? Random01 : WoTRandom::FRand();
}

bool AWoTNPCWeapon::ShouldBePreparedBy(const AActor* PreparingActor) const
{
	// WotWeapon.uc:431-439. Creature actors are data-folded onto one UE class, so
	// compare the creature row name as the shipped IsA leaf identity.
	if (!PreparingActor || TypeCapableOfPreparation.IsNone())
	{
		return false;
	}
	if (const AWoTNPCCharacter* NPC = Cast<AWoTNPCCharacter>(PreparingActor))
	{
		return NPC->CreatureRowName == TypeCapableOfPreparation;
	}
	return PreparingActor->GetClass()->GetFName() == TypeCapableOfPreparation;
}

void AWoTNPCWeapon::Equip(AWoTNPCCharacter* OwnerPawn)
{
	if (!IsValid(OwnerPawn) || IsInFlight())
	{
		return;
	}
	SetOwner(OwnerPawn);
	const FName SocketName = OwnerPawn->GetMesh() && OwnerPawn->GetMesh()->DoesSocketExist(TEXT("Weapon"))
		? FName(TEXT("Weapon")) : NAME_None;
	USceneComponent* Parent = OwnerPawn->GetMesh() ? static_cast<USceneComponent*>(OwnerPawn->GetMesh())
		: OwnerPawn->GetRootComponent();
	if (Parent)
	{
		AttachToComponent(Parent, FAttachmentTransformRules::SnapToTargetNotIncludingScale, SocketName);
		if (SocketName.IsNone())
		{
			SetActorRelativeLocation(FVector(20.0f, 10.0f, 20.0f));
		}
	}
	bEquipped = true;
	OnRep_Equipped();
}

void AWoTNPCWeapon::Unequip()
{
	bEquipped = false;
	OnRep_Equipped();
}

void AWoTNPCWeapon::OnRep_Equipped()
{
	if (WeaponMesh)
	{
		WeaponMesh->SetVisibility(bEquipped && !WeaponMeshAsset.IsNull(), true);
	}
	ApplyFadePresentation();
}

void AWoTNPCWeapon::GetAimParams(APawn* Invoker, AActor* Target, FVector& OutSource,
	FRotator& OutDirection) const
{
	OutSource = UWoTWeaponComponent::GetAimSource(Invoker, Target->GetActorLocation());
	const AWoTWeaponProjectile* CDO = ProjectileClass
		? ProjectileClass->GetDefaultObject<AWoTWeaponProjectile>() : nullptr;
	const float LeadSpeed = CDO ? CDO->Speed : 0.0f;
	if (!FMath::IsNearlyZero(Target->GetVelocity().Size()))
	{
		OutDirection = UWoTWeaponComponent::AdjustProjectileWeaponAim(
			Invoker, Target, LeadSpeed, OutSource, -1.0f);
	}
	else
	{
		OutDirection = (Target->GetActorLocation() - Invoker->GetActorLocation()).Rotation();
	}
}

void AWoTNPCWeapon::UseProjectileWeapon(APawn* Invoker, AActor* Target)
{
	if (!IsValid(Invoker) || !IsValid(Target) || !ProjectileClass || !GetWorld() ||
		!HasAuthority())
	{
		return;
	}
	FVector Source;
	FRotator Direction;
	GetAimParams(Invoker, Target, Source, Direction); // shared static aim implementation
	FireProjectile(Invoker, Source, Direction);
}

AWoTWeaponProjectile* AWoTNPCWeapon::FireProjectile(APawn* Invoker,
	const FVector& Source, const FRotator& Direction)
{
	if (!GetWorld() || !ProjectileClass || !IsValid(Invoker))
	{
		return nullptr;
	}
	FActorSpawnParameters Params;
	Params.Owner = this;            // WotWeapon.uc:399: Spawn(..., Self, ...)
	Params.Instigator = Invoker;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	return GetWorld()->SpawnActor<AWoTWeaponProjectile>(ProjectileClass, Source, Direction, Params);
}

float AWoTNPCWeapon::TakeDamage(float DamageAmount, const FDamageEvent&,
	AController*, AActor*)
{
	if (!HasAuthority() || DamageAmount <= 0.0f || IsActorBeingDestroyed())
	{
		return 0.0f;
	}
	Health -= FMath::RoundToInt(DamageAmount); // WotWeapon.uc:92-99
	if (Health <= 0)
	{
		if (AActor* OwnerActor = GetOwner(); IsValid(OwnerActor))
		{
			// WotWeapon.uc:249: the owner's PlaySound in SLOT_Interact on the
			// server, heard by every player in range.
			WoTSound::PlayReplicatedAssetFrom(*OwnerActor, DestroyedSound,
				WoTSound::ESlot::Interact);
		}
		Destroy(); // OnWeaponTakeFatalDamage, uc:245-254
	}
	else
	{
		ApplyDamageSkin();
	}
	return DamageAmount;
}

void AWoTNPCWeapon::ApplyDamageSkin()
{
	// WotWeapon.uc:256-281: strict '<' checks and retail skin2/skin1/skin0 order.
	if (Health < DefaultHealth / 3 && !DamageSkin2.IsNull())
	{
		CurrentSkin = DamageSkin2;
	}
	else if (Health < 2 * DefaultHealth / 3 && !DamageSkin1.IsNull())
	{
		CurrentSkin = DamageSkin1;
	}
	else if (Health < DefaultHealth && !DamageSkin0.IsNull())
	{
		CurrentSkin = DamageSkin0;
	}
	ApplyCurrentSkinVisual();
}

void AWoTNPCWeapon::ApplyCurrentSkinVisual()
{
	if (!DisplayMesh)
	{
		return;
	}

#if !WOT_GUARDED_BOOTSTRAP
	TArray<FSoftObjectPath> PendingPaths;
	if (!CurrentSkin.IsNull() && !CurrentSkin.IsValid())
	{
		PendingPaths.Add(CurrentSkin.ToSoftObjectPath());
	}
	else if (CurrentSkin.IsNull())
	{
		for (const TSoftObjectPtr<UTexture>& BaseSkin : SkinOverrides)
		{
			if (!BaseSkin.IsNull() && !BaseSkin.IsValid())
			{
				PendingPaths.AddUnique(BaseSkin.ToSoftObjectPath());
			}
		}
	}
	if (PendingPaths.Num() > 0)
	{
		const TWeakObjectPtr<AWoTNPCWeapon> WeakThis(this);
		UAssetManager::GetStreamableManager().RequestAsyncLoad(
			PendingPaths, FStreamableDelegate::CreateLambda([WeakThis]()
			{
				if (AWoTNPCWeapon* Weapon = WeakThis.Get())
				{
					Weapon->ApplyCurrentSkinVisual();
				}
			}));
		return;
	}
#endif

	// Actor.Skin replaces the authored MultiSkins; without it, MultiSkins(i)
	// maps to material slot i (WotWeapon.uc:256-281 and leaf defaultproperties).
	// Development keeps the resident-only async policy above; both builds use
	// the B3 slot override once those bytes are available.
	if (!CurrentSkin.IsNull())
	{
		DisplayMesh->SetAllSkinOverrides(CurrentSkin.ToSoftObjectPath());
	}
	else
	{
		DisplayMesh->ClearSkinOverrides();
		for (int32 SlotIndex = 0; SlotIndex < SkinOverrides.Num(); ++SlotIndex)
		{
			if (!SkinOverrides[SlotIndex].IsNull())
				DisplayMesh->SetSkinOverride(SlotIndex,
					SkinOverrides[SlotIndex].ToSoftObjectPath());
		}
	}
	ApplyFadePresentation();
}

void AWoTNPCWeapon::ApplyFadePresentation()
{
	if (!DisplayMesh)
	{
		return;
	}

	bool bHasOpacityParameter = false;
	for (int32 Slot = 0; Slot < DisplayMesh->GetNumMaterials(); ++Slot)
	{
		UMaterialInterface* Material = DisplayMesh->GetMaterial(Slot);
		if (!Material)
		{
			continue;
		}
		float Ignored = 0.0f;
		const bool bHasOpacity = Material->GetScalarParameterValue(
			FHashedMaterialParameterInfo(TEXT("Opacity")), Ignored);
		const bool bHasAlpha = Material->GetScalarParameterValue(
			FHashedMaterialParameterInfo(TEXT("Alpha")), Ignored);
		const bool bHasGlow = Material->GetScalarParameterValue(
			FHashedMaterialParameterInfo(TEXT("Glow")), Ignored);
		const bool bHasScaleGlow = Material->GetScalarParameterValue(
			FHashedMaterialParameterInfo(TEXT("ScaleGlow")), Ignored);
		if (!bHasOpacity && !bHasAlpha && !bHasGlow && !bHasScaleGlow)
		{
			continue;
		}
		UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(Material);
		if (!MID)
		{
			MID = DisplayMesh->CreateDynamicMaterialInstance(Slot);
		}
		if (!MID)
		{
			continue;
		}
		const float FadeScalar = bStyleTranslucent ? FMath::Clamp(ScaleGlow, 0.0f, 1.0f) : 1.0f;
		if (bHasOpacity) { MID->SetScalarParameterValue(TEXT("Opacity"), FadeScalar); }
		if (bHasAlpha) { MID->SetScalarParameterValue(TEXT("Alpha"), FadeScalar); }
		if (bHasGlow) { MID->SetScalarParameterValue(TEXT("Glow"), ScaleGlow); }
		if (bHasScaleGlow) { MID->SetScalarParameterValue(TEXT("ScaleGlow"), ScaleGlow); }
		bHasOpacityParameter |= bHasOpacity || bHasAlpha;
	}

	// Imported weapon masters generally expose Glow but no opacity control. In
	// that case mirror the cloak at the fully-faded endpoint by hiding the mesh;
	// intermediate levels still visibly dim through Glow where available.
	const bool bApproxFullyFaded = bStyleTranslucent && !bHasOpacityParameter && ScaleGlow <= 0.05f;
	DisplayMesh->SetVisibility(
		bEquipped && !WeaponMeshAsset.IsNull() && !bApproxFullyFaded, true);
}

void AWoTNPCWeapon::OnRep_FadePresentation()
{
	ApplyFadePresentation();
}

void AWoTNPCWeapon::OnRep_Health()
{
	ApplyDamageSkin();
}

void AWoTNPCWeapon::DropFrom(const FVector& StartLocation)
{
	if (HasAuthority() && GetWorld())
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		if (AWoTTossedWeapon* Tossed = GetWorld()->SpawnActor<AWoTTossedWeapon>(
			AWoTTossedWeapon::StaticClass(), StartLocation, GetActorRotation(), Params))
		{
			Tossed->InitFromWeapon(*this);
		}
	}
	Destroy(); // WotWeapon.uc:153-157
}

void AWoTNPCWeapon::Destroyed()
{
	if (!bDestroyedEventSent)
	{
		bDestroyedEventSent = true;
		OnWeaponDestroyed.Broadcast(this); // DestroyedEvent=WeaponDestroyed, uc:80-85
	}
	Super::Destroyed();
}

void AWoTNPCWeapon::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWoTNPCWeapon, WeaponTypeName);
	DOREPLIFETIME(AWoTNPCWeapon, bEquipped);
	DOREPLIFETIME(AWoTNPCWeapon, Health);
	DOREPLIFETIME(AWoTNPCWeapon, bStyleTranslucent);
	DOREPLIFETIME(AWoTNPCWeapon, ScaleGlow);
}

AWoTWeaponProjectile* AWoTNPCThrowableWeapon::FireProjectile(APawn* Invoker,
	const FVector& Source, const FRotator& Direction)
{
	// Retail removes the weapon from inventory while it flies
	// (WotThrowableWeapon.uc:21-24); a second fire attempt therefore finds none.
	if (IsInFlight())
	{
		return nullptr;
	}
	// WotThrowableWeapon.uc:10-31: leave the hand, keep the pawn as owner of the
	// weapon actor, then let the projectile inherit skin/style.
	Unequip();
	if (AWoTNPCCharacter* OwnerPawn = Cast<AWoTNPCCharacter>(GetOwner()))
	{
		// Retail removes the throwable from Inventory and Pawn.Weapon while the
		// projectile flies. The explicit held-actor seam is the equivalent state.
		OwnerPawn->EquipBodyAttack();
	}
	AWoTWeaponProjectile* Projectile = Super::FireProjectile(Invoker, Source, Direction);
	if (!Projectile)
	{
		if (AWoTNPCCharacter* OwnerPawn = Cast<AWoTNPCCharacter>(GetOwner()))
		{
			OwnerPawn->EquipMeleeWeapon();
		}
		return nullptr;
	}
	Projectile->SkinOverride = CurrentSkin;
	Projectile->bStyleMasked = bStyleMasked;
	Projectile->ApplyProjectileSkin();
	ActiveProjectile = Projectile;
	Projectile->OnDestroyed.AddDynamic(this, &AWoTNPCThrowableWeapon::OnThrownProjectileDestroyed);
	return Projectile;
}

bool AWoTNPCThrowableWeapon::IsInFlight() const
{
	return IsValid(ActiveProjectile);
}

void AWoTNPCThrowableWeapon::OnThrownProjectileDestroyed(AActor* DestroyedActor)
{
	if (DestroyedActor != ActiveProjectile)
	{
		return;
	}
	ActiveProjectile = nullptr;
	AWoTNPCCharacter* OwnerPawn = Cast<AWoTNPCCharacter>(GetOwner());
	if (!IsValid(OwnerPawn) || OwnerPawn->Health <= 0)
	{
		Destroy(); // owner died while the projectile was in flight
		return;
	}
	OwnerPawn->EquipMeleeWeapon(); // OnWotProjectileDestroyed, WotThrowableWeapon.uc:36-54
}

AWoTTossedWeapon::AWoTTossedWeapon()
{
	PrimaryActorTick.bCanEverTick = true;
	// BounceableDecoration.uc:185-190 supplies a world-blocking cylinder.
	// Collision must remain available before any presentation asset is loaded.
	Collision = CreateDefaultSubobject<UWoTFlatCylinderComponent>(TEXT("Collision"));
	Collision->SetCylinderSize(12.0f, 4.0f);
	Collision->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Collision->SetCollisionObjectType(ECC_WorldDynamic);
	Collision->SetCollisionResponseToAllChannels(ECR_Ignore);
	Collision->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Block);
	// Movers use WorldDynamic too. BeginPlay filters ordinary actor contacts
	// in both sweep directions (BounceableDecoration.uc:188-189).
	Collision->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Block);
	// BounceableDecoration.uc:190 keeps the decoration a projectile target.
	Collision->SetCollisionResponseToChannel(ECC_GameTraceChannel1, ECR_Overlap);
	Collision->SetGenerateOverlapEvents(true);
	RootComponent = Collision;
	DisplayMesh = CreateDefaultSubobject<UWoTVertexAnimComponent>(TEXT("DisplayMesh"));
	DisplayMesh->SetupAttachment(Collision);
	// A later replicated root transform must not turn the settled visual child.
	DisplayMesh->SetUsingAbsoluteRotation(true);
	DisplayMesh->SetCollisionProfileName(TEXT("NoCollision"));
	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	Mesh->SetupAttachment(DisplayMesh);
	Mesh->SetCollisionProfileName(TEXT("NoCollision"));
	Movement = CreateDefaultSubobject<UProjectileMovementComponent>(TEXT("Movement"));
	Movement->bShouldBounce = true;
	Movement->Bounciness = 0.35f;
	Movement->Friction = 0.6f;
	Movement->ProjectileGravityScale = 1.0f;
	// Deliberately bound tossed weapons even while they bounce (PORT_NOTES).
	Movement->MaxSpeed = WoTFallingPhysics::TerminalSpeed;
	Movement->SetUpdatedComponent(Collision);
	Movement->OnProjectileStop.AddDynamic(this, &AWoTTossedWeapon::OnTossedStopped);
	bReplicates = true;
	SetReplicateMovement(true);
}

void AWoTTossedWeapon::PostInitializeComponents()
{
	Super::PostInitializeComponents();
	// Original collision cylinders are world-up, even when a weapon is tossed
	// from a pitched or rolled attachment. Replicate that rotation as visual data.
	TossedVisualRotation = GetActorRotation();
	SetActorRotation(FRotator(0.0, TossedVisualRotation.Yaw, 0.0));
	DisplayMesh->SetWorldRotation(TossedVisualRotation);
}

void AWoTTossedWeapon::BeginPlay()
{
	Super::BeginPlay();
	for (TActorIterator<AActor> It(GetWorld()); It; ++It) IgnoreNonWorldActor(*It);
	ActorSpawnedHandle = GetWorld()->AddOnActorSpawnedHandler(
		FOnActorSpawned::FDelegate::CreateUObject(this, &AWoTTossedWeapon::IgnoreNonWorldActor));
}

void AWoTTossedWeapon::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (ActorSpawnedHandle.IsValid() && GetWorld())
	{
		GetWorld()->RemoveOnActorSpawnedHandler(ActorSpawnedHandle);
		ActorSpawnedHandle.Reset();
	}
	for (const TWeakObjectPtr<UPrimitiveComponent>& Component : NonBlockingComponents)
	{
		if (Component.IsValid()) Component->IgnoreComponentWhenMoving(Collision, false);
	}
	NonBlockingComponents.Reset();
	Super::EndPlay(EndPlayReason);
}

void AWoTTossedWeapon::IgnoreNonWorldActor(AActor* Actor)
{
	if (!IsValid(Actor) || Actor == this || Actor->IsA<AWoTMover>()) return;
	// Preserve static world geometry and projectile overlaps. Ordinary dynamic
	// bodies must neither deflect a drop nor land on it, including later carcasses.
	TInlineComponentArray<UPrimitiveComponent*> Components(Actor);
	for (UPrimitiveComponent* Component : Components)
	{
		if (Component->GetCollisionObjectType() != ECC_WorldDynamic) continue;
		Collision->IgnoreComponentWhenMoving(Component, true);
		Component->IgnoreComponentWhenMoving(Collision, true);
		NonBlockingComponents.AddUnique(Component);
	}
}

void AWoTTossedWeapon::OnTossedStopped(const FHitResult& Hit)
{
	if (!HasAuthority() || Hit.ImpactNormal.Z <= 0.7f) return;
	// BounceableDecoration.uc:135-158 and Util.uc:190-205: preserve unauthored
	// axes; choose the nearer authored angle or its half-turn opposite.
	auto LandAxis = [](double Current, double Desired)
	{
		if (Desired < 0.0) return Current;
		const int32 CurrentUnits = static_cast<int32>(Current * 65536.0 / 360.0) & 0xffff;
		const int32 DesiredUnits = static_cast<int32>(Desired);
		const int32 Difference = FMath::Abs(CurrentUnits - DesiredUnits);
		const int32 FinalUnits = (Difference < 16384 || Difference >= 49152)
			? DesiredUnits : DesiredUnits + 32768;
		return (FinalUnits & 0xffff) * 360.0 / 65536.0;
	};
	TossedVisualRotation = FRotator(
		LandAxis(TossedVisualRotation.Pitch, LandedAngles.Pitch),
		LandAxis(TossedVisualRotation.Yaw, LandedAngles.Yaw),
		LandAxis(TossedVisualRotation.Roll, LandedAngles.Roll));
	TossedVisualRotation += (FVector::UpVector - Hit.ImpactNormal).GetSafeNormal().Rotation();
	DisplayMesh->SetWorldRotation(TossedVisualRotation);
	ForceNetUpdate(); // the existing replicated visual payload also carries rest
}

void AWoTTossedWeapon::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (HasAuthority() && Movement && Movement->IsActive())
	{
		WoTFallingPhysics::DestroyOutsideWorld(*this);
	}
}

void AWoTTossedWeapon::InitFromWeapon(const AWoTNPCWeapon& SourceWeapon)
{
	WeaponTypeName = SourceWeapon.WeaponTypeName;
	SkinOverride = SourceWeapon.CurrentSkin;
	bStyleMasked = SourceWeapon.bStyleMasked;
	ApplyTossedVisual();
	if (Movement)
	{
		Movement->Velocity = FVector(WoTRandom::FRandRange(-100.0f, 100.0f),
			WoTRandom::FRandRange(-100.0f, 100.0f), 260.0f);
	}
	SetLifeSpan(SourceWeapon.MPTossedLifeSpan); // MP always, WotWeapon.uc:136-143
}

void AWoTTossedWeapon::OnRep_TossedVisual()
{
	ApplyTossedVisual();
}

void AWoTTossedWeapon::ApplyTossedVisual()
{
	if (DisplayMesh) DisplayMesh->SetWorldRotation(TossedVisualRotation);
	const FWoTNPCWeaponConfig* Config = WoTNPCWeaponData::Find(WeaponTypeName);
	if (!Config || !DisplayMesh)
	{
		return;
	}
	// WotWeapon.uc:129-135 selects the tossed decoration class, whose effective
	// cylinder dimensions can override BounceableDecoration's 12 by 4 default.
	FString DefaultsError;
	const FWoTRuntimeClassDefaults* Defaults =
		UWoTPayloadSubsystem::ActiveRuntimeClassDefaults(DefaultsError);
#if !WOT_GUARDED_BOOTSTRAP
	if (!Defaults) Defaults = WoTDevRetailClassDefaults();
#endif
	if (Defaults)
	{
		const FString ClassIdentity = Config->TossedDecorationType.ToString();
		float Radius = Collision->GetUnscaledCapsuleRadius();
		float Height = Collision->GetUnscaledCapsuleHalfHeight();
		if (const FWoTRuntimeWorldProperty* Property =
			Defaults->FindClassProperty(ClassIdentity, TEXT("CollisionRadius")))
		{
			WoTRuntimeWorldPropertyFloat(*Property, Radius);
		}
		if (const FWoTRuntimeWorldProperty* Property =
			Defaults->FindClassProperty(ClassIdentity, TEXT("CollisionHeight")))
		{
			WoTRuntimeWorldPropertyFloat(*Property, Height);
		}
		Collision->SetCylinderSize(Radius, Height);
		auto ReadLandingAngle = [&](const TCHAR* Name)
		{
			float Angle = -1.0f; // BounceableDecoration.uc:180-182 leaves each axis free
			if (const FWoTRuntimeWorldProperty* Property = Defaults->FindClassProperty(ClassIdentity, Name))
			{
				WoTRuntimeWorldPropertyFloat(*Property, Angle);
			}
			return Angle;
		};
		LandedAngles = FRotator(ReadLandingAngle(TEXT("LandedPitch")),
			ReadLandingAngle(TEXT("LandedYaw")), ReadLandingAngle(TEXT("LandedRoll")));
	}
	DisplayMesh->SetRelativeScale3D(FVector(Config->DrawScale));
	const TSoftObjectPtr<USkeletalMesh> Asset(WoTNPCWeaponData::ResolveMeshPath(Config->Mesh));
#if WOT_GUARDED_BOOTSTRAP
	DisplayMesh->SetVertexMeshPath(Asset.ToSoftObjectPath());
#else
	if (Asset.IsValid())
	{
		DisplayMesh->SetSkeletalMeshAsset(Asset.Get());
	}
	else if (!Asset.IsNull())
	{
		const TWeakObjectPtr<AWoTTossedWeapon> WeakThis(this);
		UAssetManager::GetStreamableManager().RequestAsyncLoad(
			Asset.ToSoftObjectPath(), FStreamableDelegate::CreateLambda([WeakThis, Asset]()
			{
				if (AWoTTossedWeapon* Tossed = WeakThis.Get(); Tossed && Tossed->DisplayMesh && Asset.IsValid())
				{
					Tossed->DisplayMesh->SetSkeletalMeshAsset(Asset.Get());
					Tossed->ApplyTossedVisual();
				}
			}));
	}
#endif
#if !WOT_GUARDED_BOOTSTRAP
	if (!SkinOverride.IsNull() && !SkinOverride.IsValid())
	{
		const TWeakObjectPtr<AWoTTossedWeapon> WeakThis(this);
		UAssetManager::GetStreamableManager().RequestAsyncLoad(
			SkinOverride.ToSoftObjectPath(), FStreamableDelegate::CreateLambda([WeakThis]()
			{
				if (AWoTTossedWeapon* Tossed = WeakThis.Get())
				{
					Tossed->ApplyTossedVisual();
				}
			}));
		return;
	}
#endif
	if (!SkinOverride.IsNull())
	{
		DisplayMesh->SetAllSkinOverrides(SkinOverride.ToSoftObjectPath());
	}
}

void AWoTTossedWeapon::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWoTTossedWeapon, WeaponTypeName);
	DOREPLIFETIME(AWoTTossedWeapon, SkinOverride);
	DOREPLIFETIME(AWoTTossedWeapon, bStyleMasked);
	DOREPLIFETIME(AWoTTossedWeapon, TossedVisualRotation);
}
