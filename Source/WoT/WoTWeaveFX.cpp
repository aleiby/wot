#include "WoTWeaveFX.h"
#include "Assets/WoTRuntimeAssets.h"

#include "Net/UnrealNetwork.h"

#include "WoTSpriteBillboard.h"
#include "WoTUE1LightRegistry.h"

#include "Components/PointLightComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Materials/MaterialInterface.h"

DEFINE_LOG_CATEGORY_STATIC(LogWoTWeaveFX, Log, All);

// Activated weave rows are payload-owned. Generic material resolution and the
// cited script-method behavior remain native.
namespace
{
	TUniquePtr<FWoTWeaveFXSet>& InstalledWeaveFXRegistry()
	{
		static TUniquePtr<FWoTWeaveFXSet> Set;
		return Set;
	}

	const TMap<FName, FWoTSprayerBundleRow>& ActiveBundleRegistry()
	{
		if (const TUniquePtr<FWoTWeaveFXSet>& Installed =
			InstalledWeaveFXRegistry()) return Installed->Bundles;
		static const TMap<FName, FWoTSprayerBundleRow> Empty;
		return Empty;
	}

	UMaterialInterface* ResolveSlug(FName Slug, bool bAdd)
	{
		if (Slug.IsNone())
		{
			return nullptr;
		}
		FString Name = Slug.ToString();
		const TCHAR* Prefix = bAdd ? TEXT("MI_PSAdd_") : TEXT("MI_PSMod_");
		if (Name.RemoveFromStart(TEXT("FB:")))
		{
			Prefix = bAdd ? TEXT("MI_PSAddFB_") : TEXT("MI_PSModFB_");
		}
		const FSoftObjectPath Path(FString::Printf(
			TEXT("/Game/WoT/Materials/Instances/%s%s.%s%s"),
			Prefix, *Name, Prefix, *Name));
		return UWoTRuntimeAssets::ResolveMaterialPath(Path);
	}
}

const FWoTSprayerBundleRow* WoTWeaveFX::Find(FName Config)
{
	return ActiveBundleRegistry().Find(Config);
}

AWoTParticleSprayer* WoTWeaveFX::SpawnLocal(UWorld& World, FName Config,
                                            const FVector& Location,
                                            const FRotator& Rotation, AActor* Follow,
                                            bool bFollowRotation, bool bShiftWithFollow)
{
	// Cosmetic registries are not touched on dedicated servers.
	if (World.bIsTearingDown || World.GetNetMode() == NM_DedicatedServer)
	{
		return nullptr;
	}
	const FWoTSprayerBundleRow* Row = Find(Config);
	if (!Row) return nullptr;
	// DEFERRED spawn: BeginPlay builds the ISM passes and arms the lifecycle.
	const FTransform At(Rotation, Location);
	AWoTParticleSprayer* S = World.SpawnActorDeferred<AWoTParticleSprayer>(
		AWoTParticleSprayer::StaticClass(), At, nullptr, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!S)
	{
		return nullptr;
	}
	S->Spread = Row->Spread;
	S->Volume = Row->Volume;
	S->MinVolume = Row->MinVolume;
	S->VolumeScalePct = Row->VolumeScalePct;
	S->VisibilityRadius = Row->VisibilityRadius;
	S->VisibilityHeight = Row->VisibilityHeight;
	S->Gravity = Row->Gravity;
	S->AuthoredGravity = Row->Gravity;
	S->bInitiallyOn = Row->bInitiallyOn;
	S->bOn = Row->bOn;
	S->TriggerSeconds = Row->TimerDuration;
	if (Row->InitialState == TEXT("TriggerToggle"))
		S->TriggerMode = EWoTParticleTriggerMode::Toggle;
	else if (Row->InitialState == TEXT("TriggerControl"))
		S->TriggerMode = EWoTParticleTriggerMode::Control;
	else if (Row->InitialState == TEXT("TriggerTimed"))
		S->TriggerMode = EWoTParticleTriggerMode::Timed;
	S->bAlignGravityToRotation = Row->bAlignGravityToRotation;
	S->bLOSClip = Row->bLOSClip;
	S->bModulateOnly = Row->bModulateOnly;
	S->bLinearDistribution = Row->bLinearDistribution;
	S->PrimeCount = Row->PrimeCount;
	S->ConfigName = Config;
	S->SelfDestroySeconds = Row->SelfDestroySeconds;
	S->BurstSeconds = Row->BurstSeconds;
	for (int32 TemplateIndex = 0; TemplateIndex < Row->Templates.Num(); ++TemplateIndex)
	{
		const FWoTSprayerTemplateRow& TRow = Row->Templates[TemplateIndex];
		S->Templates.Add(TRow.Template);
		UMaterialInterface* AddMaterial = Row->bModulateOnly
			? nullptr : ResolveSlug(TRow.MaterialSlug, true);
		UMaterialInterface* ModMaterial = ResolveSlug(TRow.MaterialSlug, false);
		S->AddMaterials.Add(AddMaterial);
		S->ModMaterials.Add(ModMaterial);
		if (Config.ToString().StartsWith(TEXT("LegionStompSprayer"))
			&& ((!Row->bModulateOnly && !AddMaterial) || !ModMaterial))
		{
			// Keep any unexpected asset gap observable while still binding whichever
			// authored pass exists.
			UE_LOG(LogWoTWeaveFX, Display,
				TEXT("%s template %d has an incomplete material set for %s (add=%s mod=%s)"),
				*Config.ToString(), TemplateIndex,
				*TRow.MaterialSlug.ToString(),
				AddMaterial ? TEXT("bound") : TEXT("missing"),
				ModMaterial ? TEXT("bound") : TEXT("missing"));
		}
	}
	if (Follow)
	{
		S->FollowActor = Follow;
		S->bFollowRotation = bFollowRotation;
		S->bShiftWithFollow = bShiftWithFollow;
	}
	// Shipped trails build from empty at cast (PrimeCount=0 on every leaf); the
	// first-sight warm-up is a map-placed-plume calibration — skip it here
	// (review 6a fix: a warmed FireballFlame lays a flame lance AHEAD of the rock).
	if (Row->PrimeCount <= 0.0f) { S->SkipPriming(); }
	S->FinishSpawning(At);
	if (Row->PrimeCount > 0.0f) { S->PrimeAuthoredCount(); }
	if (Row->bHasLight)
	{
		if (S->GetNetMode() == NM_DedicatedServer)
		{
			if (S->Light != nullptr) S->Light->SetVisibility(false);
		}
		else if (!Row->LightRegistryKey.IsNone())
		{
			const FWoTUE1LightRow* Base =
				WoTUE1LightRegistry::Find(Row->LightRegistryKey);
			if (Base != nullptr)
			{
				FWoTUE1LightRow LightRow = *Base;
				if (Row->bOverrideLightHue)
				{
					LightRow.Hue = static_cast<uint8>(Row->LightHue);
				}
				S->ConfigureUE1Light(LightRow);
			}
		}
		else
		{
			S->ConfigureUE1Light(Row->LightBrightness, Row->LightHue,
				Row->LightSaturation, Row->LightRadius);
		}
	}
	// NL leaves disable only LightType; Firework07/09 rotation remains active.
	S->ConfigureLightProgram(Row->LightProgram, Row->LightDuration,
		Row->RotationYawRate);
	S->ConfigureRotationProgram(Row->RotationRate, Row->RotationGroupDelay);
	return S;
}

void WoTWeaveFX::FadeOut(AWoTParticleSprayer* Sprayer, float LifeSpan)
{
	if (IsValid(Sprayer))
	{
		Sprayer->bOn = false;
		Sprayer->FollowActor = nullptr;
		// AngrealFireworksProjectile.uc:73 and AngrealSwapPlacesProjectile.uc:123:
		// the lingering particles keep their five-second lifespan, but LT_None is
		// immediate at detonation.
		if (Sprayer->Light)
		{
			Sprayer->Light->SetIntensity(0.0f);
			Sprayer->Light->SetVisibility(false);
		}
		Sprayer->SetLifeSpan(LifeSpan);
	}
}

AWoTSprayerBlower::AWoTSprayerBlower()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

void AWoTSprayerBlower::Configure(float Magnitude, float Duration,
	float Radius, float Height, const TArray<FName>& IgnoredTypes)
{
	Wind = GetActorForwardVector() * Magnitude;
	WindPerSecond = Duration > SMALL_NUMBER ? Wind / Duration : Wind;
	for (TActorIterator<AWoTParticleSprayer> It(GetWorld()); It; ++It)
	{
		AWoTParticleSprayer* Sprayer = *It;
		if (!IsValid(Sprayer) || Sprayer->bIsWindResistant
			|| IgnoredTypes.Contains(Sprayer->ConfigName)
			|| FMath::Abs(Sprayer->GetActorLocation().Z - GetActorLocation().Z) > Height)
		{
			continue;
		}
		FVector Projection = Sprayer->GetActorLocation() - GetActorLocation();
		Projection.Z = 0.0f;
		if (Projection.Size() <= Radius && AffectedSprayers.Num() < 64)
		{
			AffectedSprayers.Add(Sprayer);
			Sprayer->Gravity += Wind;
		}
	}
	if (bFinished) { Destroy(); }
}

void AWoTSprayerBlower::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (bFinished) { return; }
	FVector DeltaWind = WindPerSecond * DeltaSeconds;
	WindAccum += DeltaWind;
	if (WindAccum.Size() > Wind.Size())
	{
		DeltaWind -= WindAccum - Wind;
		bFinished = true;
	}
	for (const TWeakObjectPtr<AWoTParticleSprayer>& WeakSprayer : AffectedSprayers)
	{
		if (AWoTParticleSprayer* Sprayer = WeakSprayer.Get())
		{
			Sprayer->Gravity -= DeltaWind;
		}
	}
	if (bFinished) { Destroy(); }
}

AWoTSprayerBlower* WoTWeaveFX::BlowSprayers(UWorld& World,
	const FVector& Location, const FRotator& Rotation, float Magnitude,
	float Duration, float Radius, float Height, const TArray<FName>& IgnoredTypes)
{
	if (World.bIsTearingDown || World.GetNetMode() == NM_DedicatedServer)
	{
		return nullptr;
	}
	const FTransform At(Rotation, Location);
	AWoTSprayerBlower* Blower = World.SpawnActorDeferred<AWoTSprayerBlower>(
		AWoTSprayerBlower::StaticClass(), At, nullptr, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (Blower)
	{
		Blower->FinishSpawning(At);
		Blower->Configure(Magnitude, Duration, Radius, Height, IgnoredTypes);
	}
	return Blower;
}

namespace
{
	const TMap<FName, FWoTExplosionRow>& ActiveExplosionRegistry()
	{
		if (const TUniquePtr<FWoTWeaveFXSet>& Installed =
			InstalledWeaveFXRegistry()) return Installed->Explosions;
		static const TMap<FName, FWoTExplosionRow> Empty;
		return Empty;
	}
}

void FWoTWeaveFXBuilder::Install(FWoTWeaveFXSet&& Set)
{
	InstalledWeaveFXRegistry() = MakeUnique<FWoTWeaveFXSet>(MoveTemp(Set));
}

#if WITH_AUTOMATION_TESTS
void FWoTWeaveFXBuilder::Reset()
{
	InstalledWeaveFXRegistry().Reset();
}
#endif

AWoTExplosionEffect::AWoTExplosionEffect()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Sprite = CreateDefaultSubobject<UWoTSpriteBillboardComponent>(TEXT("Sprite"));
	Sprite->SetupAttachment(RootComponent);
	Light = CreateDefaultSubobject<UPointLightComponent>(TEXT("Light"));
	Light->SetupAttachment(RootComponent);
	Light->SetVisibility(false);
}

void AWoTExplosionEffect::Configure(const FWoTExplosionRow& Row)
{
	const FWoTUE1LightRow* ExplosionBase = GetNetMode() == NM_DedicatedServer
		? nullptr : WoTUE1LightRegistry::Find(TEXT("Explosion"));
	FWoTUE1LightRow LightRow;
	if (ExplosionBase != nullptr) LightRow = *ExplosionBase;
	// Every port row is a WOT.Explosion leaf. Seed the inherited defaults from
	// the registry, then apply only that leaf row's cited overrides.
	LightRow.Brightness = static_cast<uint8>(Row.LightBrightness);
	LightRow.Hue = static_cast<uint8>(Row.LightHue);
	LightRow.Saturation = static_cast<uint8>(Row.LightSaturation);
	LightRow.Radius = Row.LightRadius;
	LightRow.Shape = Row.bNonIncidence
		? EWoTUE1LightShape::NonIncidence : EWoTUE1LightShape::None;
	FullLife = Row.LifeSpan;
	BaseDrawScale = Row.DrawScale;
	bGrowSustainShrink = Row.bGrowSustainShrink;
	StopGrowPct = Row.StopGrowPct;
	ShrinkPct = Row.ShrinkPct;
	SetLifeSpan(Row.LifeSpan);
	Sprite->DrawScale = Row.DrawScale;
	Sprite->PlayOnceSeconds = Row.LifeSpan;
	TArray<FString> Names;
	for (int32 i = Row.FrameStart; i < Row.FrameStart + Row.FrameCount; ++i)
	{
		Names.Add(FString::Printf(TEXT("%s%02d"),
			*Row.FramePrefix.ToString(), i));
	}
	const FString FramePackage = Row.FramePackage.ToString();
	Sprite->SetSpriteSeries(*FramePackage, Names, Row.TexSize, Row.TexSize);
	// MyrddraalElectric.uc:27 is the sole ported Explosion leaf that explicitly
	// disables inherited Explosion.LightType. Keep this cited behavior policy
	// native so adding it does not mutate the authenticated weave-family schema.
	const bool bHasLight = ConfigName != TEXT("MyrddraalElectric");
	FullIntensity = ExplosionBase != nullptr && bHasLight
		? LightRow.Brightness / 64.0f * 5.0f : 0.0f;
	if (ExplosionBase != nullptr && bHasLight)
	{
		WoTUE1LightRegistry::Configure(Light, LightRow);
	}
}

void AWoTExplosionEffect::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (FollowActor.IsValid())
	{
		SetActorLocation(FollowActor->GetActorLocation());
	}
	// Explosion.uc Tick: LightBrightness = default x (LifeSpan remaining / full).
	if (FullLife > 0.0f && Light)
	{
		const float RemainingPct = FMath::Clamp(GetLifeSpan() / FullLife, 0.0f, 1.0f);
		if (bGrowSustainShrink && Sprite)
		{
			float Envelope = 1.0f;
			if (StopGrowPct < 1.0f && RemainingPct > StopGrowPct)
			{
				Envelope = (1.0f - RemainingPct) / (1.0f - StopGrowPct);
			}
			else if (ShrinkPct > SMALL_NUMBER && RemainingPct <= ShrinkPct)
			{
				Envelope = RemainingPct / ShrinkPct;
			}
			Sprite->DrawScale = BaseDrawScale * FMath::Clamp(Envelope, 0.0f, 1.0f);
		}
		Light->SetIntensity(FullIntensity
		                    * FMath::Clamp(GetLifeSpan() / FullLife, 0.0f, 1.0f));
	}
}

const FWoTExplosionRow* WoTWeaveFX::FindExplosion(FName Config)
{
	return ActiveExplosionRegistry().Find(Config);
}

AWoTExplosionEffect* WoTWeaveFX::SpawnExplosion(UWorld& World, FName Config,
	                                                const FVector& Location,
	                                                AActor* Follow)
{
	if (World.bIsTearingDown || World.GetNetMode() == NM_DedicatedServer)
	{
		return nullptr;
	}
	const FWoTExplosionRow* Row = ActiveExplosionRegistry().Find(Config);
	if (!Row) return nullptr;
	const FTransform At(FRotator::ZeroRotator, Location);
	AWoTExplosionEffect* E = World.SpawnActorDeferred<AWoTExplosionEffect>(
		AWoTExplosionEffect::StaticClass(), At, nullptr, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (E)
	{
		E->ConfigName = Config;
		E->FollowActor = Follow;
		E->Configure(*Row);
		E->FinishSpawning(At);
	}
	return E;
}

AWoTWeaveBurst::AWoTWeaveBurst()
{
	bReplicates = true;
	bNetTemporary = true;
	SetReplicatingMovement(false);
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	NetPriority = 6.0f;
	SetLifeSpan(3.0f);   // outlives the longest bundle burst comfortably
}

void AWoTWeaveBurst::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWoTWeaveBurst, Config);
}

void AWoTWeaveBurst::BeginPlay()
{
	Super::BeginPlay();
	WoTWeaveFX::SpawnLocal(*GetWorld(), Config, GetActorLocation(), GetActorRotation());
}

AWoTWeaveBurst* WoTWeaveFX::SpawnBurstMarker(UWorld& World, FName Config,
                                             const FVector& Location,
                                             const FRotator& Rotation)
{
	if (World.bIsTearingDown || World.GetNetMode() == NM_Client)
	{
		return nullptr;
	}
	const FTransform At(Rotation, Location);
	AWoTWeaveBurst* Marker = World.SpawnActorDeferred<AWoTWeaveBurst>(
		AWoTWeaveBurst::StaticClass(), At, nullptr, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (Marker)
	{
		Marker->Config = Config;
		Marker->FinishSpawning(At);
	}
	return Marker;
}
