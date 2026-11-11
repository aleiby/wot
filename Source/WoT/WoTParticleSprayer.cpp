#include "WoTParticleSprayer.h"
#include "AI/WoTPathNodeIterator.h"

#include "WoTDecal.h"
#include "WoTUE1LightRegistry.h"
#include "WoTPreferencesSubsystem.h"
#include "WoTSpriteBillboard.h"
#include "Assets/WoTRuntimeAssets.h"
#include "Payload/WoTPayloadActivation.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
UStaticMesh* GPlaneMesh = nullptr;

int32 LocalCosmeticSeed(const UObject* Object)
{
	const uint64 Entropy = FPlatformTime::Cycles64()
		^ static_cast<uint64>(reinterpret_cast<UPTRINT>(Object));
	return static_cast<int32>(Entropy ^ (Entropy >> 32));
}
#if WITH_AUTOMATION_TESTS
TFunction<void(const TCHAR*, const AActor&, const UActorComponent*)>
	GParticleSprayerLifecycleObserver;
#endif
}

void UWoTParticleSprayerRenderComponent::OnRegister()
{
#if WITH_AUTOMATION_TESTS
	if (GParticleSprayerLifecycleObserver && GetOwner() != nullptr)
		GParticleSprayerLifecycleObserver(TEXT("OnRegister"), *GetOwner(), this);
#endif
	Super::OnRegister();
}

#if WITH_AUTOMATION_TESTS
void WoTSetParticleSprayerLifecycleObserverForTesting(
	TFunction<void(const TCHAR*, const AActor&, const UActorComponent*)> Observer)
{
	GParticleSprayerLifecycleObserver = MoveTemp(Observer);
}
#endif

AWoTParticleSprayer::AWoTParticleSprayer()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Light = CreateDefaultSubobject<UPointLightComponent>(TEXT("Light"));
	Light->SetupAttachment(RootComponent);
	Light->SetVisibility(false);
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Plane(
		TEXT("/Engine/BasicShapes/Plane.Plane"));
	if (Plane.Succeeded())
	{
		GPlaneMesh = Plane.Object;
	}
}

void AWoTParticleSprayer::BeginPlay()
{
	Super::BeginPlay();
	LastSimTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
	if (IsWoTBootstrapWorldReady() && !TagName.IsNone())
	{
		GetWorld()->GetSubsystem<UWoTEventSubsystem>()->RegisterTag(TagName, this);
	}
#if WITH_AUTOMATION_TESTS
	if (GParticleSprayerLifecycleObserver)
		GParticleSprayerLifecycleObserver(TEXT("BeginPlay"), *this, nullptr);
#endif

	// One-shot lifecycle (gore 4d): the actor LifeSpan, and NO steady-state warm-up —
	// a burst is an event; priming would fast-forward it before first render. The
	// UpdateInstances null-guard makes the skipped Add pass safe.
	if (SelfDestroySeconds > 0.0f)
	{
		SetLifeSpan(SelfDestroySeconds);
	}
	if (BurstSeconds > 0.0f && PrimeCount <= 0.0f)
	{
		bPrimed = true;
	}
	if (GetNetMode() == NM_DedicatedServer)
	{
		SetActorTickEnabled(false);
		return;
	}
	CosmeticRandom.Initialize(LocalCosmeticSeed(this));

	// Per-template ISM pairs; instances allocated lazily as slots get used.
	// STY_Modulated configs (blood) draw only the modulate pass.
	const int32 N = Templates.Num();
	AddISM.SetNum(N);
	ModISM.SetNum(N);
	AnimatedAddMids.SetNum(N);
	AnimatedModMids.SetNum(N);
	for (int32 T = 0; T < N; ++T)
	{
		for (int32 Pass = 0; Pass < (bModulateOnly ? 1 : 2); ++Pass)
		{
			UInstancedStaticMeshComponent* C =
				NewObject<UWoTParticleSprayerRenderComponent>(
				this, *FString::Printf(TEXT("PS_%s_%d"), Pass ? TEXT("Add") : TEXT("Mod"), T));
			C->SetupAttachment(RootComponent);
			C->SetStaticMesh(GPlaneMesh);
			C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			C->SetGenerateOverlapEvents(false);
			C->SetCastShadow(false);
			C->SetOnlyOwnerSee(bOnlyOwnerSee);
			C->NumCustomDataFloats = 1;
			C->SetUsingAbsoluteLocation(true);   // particles live in world space
			C->SetUsingAbsoluteRotation(true);
			C->SetWorldLocationAndRotation(FVector::ZeroVector, FQuat::Identity);
			// modulate before additive: FB = Dst*(1-Src) + Src
			C->SetTranslucentSortPriority(Pass ? 91 : 90);
			TArray<TObjectPtr<UMaterialInterface>>& Mats = Pass ? AddMaterials : ModMaterials;
			if (Mats.IsValidIndex(T) && Mats[T])
			{
				UMaterialInterface* Material = Mats[T];
				if (TemplateAnimations.IsValidIndex(T)
					&& TemplateAnimations[T].FrameIdentities.Num() > 1)
				{
					// Guarded builds hand the sprayer MIDs; UE rejects a MID as a
					// parent (the result renders the default material), so
					// re-parent onto its parent and keep its overrides.
					UMaterialInstanceDynamic* SourceMID =
						Cast<UMaterialInstanceDynamic>(Material);
					UMaterialInterface* Parent =
						SourceMID ? SourceMID->Parent.Get() : Material;
					UMaterialInstanceDynamic* MID = Parent
						? UMaterialInstanceDynamic::Create(Parent, this) : nullptr;
					if (MID && SourceMID) MID->CopyParameterOverrides(SourceMID);
					(Pass ? AnimatedAddMids : AnimatedModMids)[T] = MID;
					#if WITH_AUTOMATION_TESTS
					if (MID) WoTCountTextureAnimationMIDForTesting();
					#endif
					Material = MID;
				}
				C->SetMaterial(0, Material);
			}
			C->RegisterComponent();
			(Pass ? AddISM : ModISM)[T] = C;
		}
	}

	// Priming waits for the first tick with nonzero distance-scaled volume:
	// UE1 primed when the render iterator was constructed — first time SEEN,
	// camera nearby. Priming here at BeginPlay would run with the camera far
	// away, where VisibilityRadius scales the volume to zero.
	UpdateInstances();
	UpdateLocalGoreVisibility();
}

void AWoTParticleSprayer::ConfigureTemplateAnimation(const int32 TemplateIndex,
	const FString& TextureIdentity, TArray<FString> FrameIdentities,
	TArray<TObjectPtr<UTexture2D>> FrameTextures,
	const float MinRate, const float MaxRate)
{
	if (TemplateIndex < 0) return;
	TemplateAnimations.SetNum(FMath::Max(TemplateAnimations.Num(), TemplateIndex + 1));
	FTemplateAnimation& Animation = TemplateAnimations[TemplateIndex];
	Animation.TextureIdentity = TextureIdentity;
	Animation.FrameIdentities = MoveTemp(FrameIdentities);
	Animation.FrameTextures = MoveTemp(FrameTextures);
	Animation.MinFrameRate = MinRate;
	Animation.MaxFrameRate = MaxRate;
	Animation.LastObservedRenderTimeSeconds = -1.0f;
}

int32 AWoTParticleSprayer::GetTemplateAnimationFrameCount(
	const int32 TemplateIndex) const
{
	return TemplateAnimations.IsValidIndex(TemplateIndex)
		? TemplateAnimations[TemplateIndex].FrameIdentities.Num() : 0;
}

#if WITH_AUTOMATION_TESTS
void AWoTParticleSprayer::MarkTemplateDrawnForTesting(const int32 TemplateIndex)
{
	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
	MarkTemplateDrawnForTesting(TemplateIndex, Now);
}

void AWoTParticleSprayer::MarkTemplateDrawnForTesting(
	const int32 TemplateIndex, const float DrawTimeSeconds)
{
	for (const TArray<TObjectPtr<UInstancedStaticMeshComponent>>* Components :
		{&AddISM, &ModISM})
	{
		if (Components->IsValidIndex(TemplateIndex) && (*Components)[TemplateIndex])
		{
			(*Components)[TemplateIndex]->GetSceneData().SetLastRenderTimeOnScreen(
				DrawTimeSeconds);
		}
	}
}

bool AWoTParticleSprayer::AreParticleInstancesVisibleForTesting() const
{
	for (const TArray<TObjectPtr<UInstancedStaticMeshComponent>>* Components :
		{&AddISM, &ModISM})
	{
		for (const UInstancedStaticMeshComponent* Component : *Components)
		{
			if (Component && Component->IsVisible()) return true;
		}
	}
	return false;
}

UTexture* AWoTParticleSprayer::GetTemplateBoundTextureForTesting(
	const int32 TemplateIndex) const
{
	const UMaterialInstanceDynamic* MID = AnimatedAddMids.IsValidIndex(TemplateIndex)
		? AnimatedAddMids[TemplateIndex].Get() : nullptr;
	if (!MID && AnimatedModMids.IsValidIndex(TemplateIndex))
	{
		MID = AnimatedModMids[TemplateIndex].Get();
	}
	UTexture* Texture = nullptr;
	return MID && MID->GetTextureParameterValue(
		FHashedMaterialParameterInfo(TEXT("Tex")), Texture) ? Texture : nullptr;
}

FString AWoTParticleSprayer::GetTemplateFrameIdentityForTesting(
	const int32 TemplateIndex, const int32 FrameIndex) const
{
	return TemplateAnimations.IsValidIndex(TemplateIndex)
		&& TemplateAnimations[TemplateIndex].FrameIdentities.IsValidIndex(FrameIndex)
		? TemplateAnimations[TemplateIndex].FrameIdentities[FrameIndex]
		: FString();
}
#endif

void AWoTParticleSprayer::ConfigureLocalGoreLevel(uint8 InGoreLevel)
{
	bHasLocalGoreLevel = true;
	LocalGoreLevel = InGoreLevel;
	UpdateLocalGoreVisibility();
}

void AWoTParticleSprayer::UpdateLocalGoreVisibility()
{
	if (!bHasLocalGoreLevel || !GetWorld())
	{
		return;
	}
	const UWoTPreferencesSubsystem* Preferences = GetGameInstance()
		? GetGameInstance()->GetSubsystem<UWoTPreferencesSubsystem>() : nullptr;
	const bool bVisible = !Preferences
		|| LocalGoreLevel <= Preferences->GetGameSettings().GoreDetailLevel;
	bLocalGoreVisible = bVisible;
	SetParticleInstancesVisible(bParticleInstancesVisible);
	if (Light) Light->SetVisibility(bVisible && FullLightIntensity > 0.0f);
}

void AWoTParticleSprayer::SetParticleInstancesVisible(const bool bVisible)
{
	bParticleInstancesVisible = bVisible;
	const bool bCombinedVisible = bVisible && bLocalGoreVisible;
	for (UInstancedStaticMeshComponent* Component : AddISM)
	{
		if (Component) Component->SetVisibility(bCombinedVisible, true);
	}
	for (UInstancedStaticMeshComponent* Component : ModISM)
	{
		if (Component) Component->SetVisibility(bCombinedVisible, true);
	}
}

void AWoTParticleSprayer::WoTTrigger(AActor*, APawn*)
{
	switch (TriggerMode)
	{
	case EWoTParticleTriggerMode::Toggle:
		bOn = !bOn;
		break;
	case EWoTParticleTriggerMode::Control:
		bOn = !bInitiallyOn;
		break;
	case EWoTParticleTriggerMode::Timed:
		bOn = !bInitiallyOn;
		GetWorldTimerManager().ClearTimer(TriggerRestoreTimer);
		GetWorldTimerManager().SetTimer(TriggerRestoreTimer, this,
			&AWoTParticleSprayer::RestoreTriggeredState,
			FMath::Max(0.001f, TriggerSeconds), false);
		break;
	default:
		break;
	}
}

void AWoTParticleSprayer::WoTUnTrigger(AActor*, APawn*)
{
	if (TriggerMode == EWoTParticleTriggerMode::Control)
	{
		bOn = bInitiallyOn;
	}
}

void AWoTParticleSprayer::RestoreTriggeredState()
{
	bOn = bInitiallyOn;
}

int32 AWoTParticleSprayer::PickTemplate() const
{
	// DIST_Random weighted pick. DIST_Linear
	// cycles deterministically with the same long-run frequencies; weighted
	// random is used for both (statistically identical steady state).
	float TotalWeight = 0.0f;
	for (const FWoTParticleTemplate& T : Templates)
	{
		TotalWeight += T.Weight;
	}
	if (TotalWeight <= 0.0f)
	{
		return INDEX_NONE;
	}
	float WeightedVal = TotalWeight * CosmeticRandom.FRand();
	int32 Index = 0;
	float Acc = Templates[0].Weight;
	while (Acc < WeightedVal && Index + 1 < Templates.Num())
	{
		Acc += Templates[++Index].Weight;
	}
	return Templates[Index].Weight > 0.0f ? Index : INDEX_NONE;
}

void AWoTParticleSprayer::ParticleTick(float DeltaTime)
{
#if WITH_AUTOMATION_TESTS
	WoTCountParticleSprayerSubstepForTesting();
#endif
	// The Legend sprayer tick, ParticleDensity assumed 255 (max).
	float ScaledVolume = Volume;

	// Distance-scaled volume within the visibility cylinder.
	if (APlayerCameraManager* Cam = UGameplayStatics::GetPlayerCameraManager(this, 0))
	{
		const float Pct = FMath::Clamp(VolumeScalePct, 0.0f, 1.0f);
		if (bOn && Pct > 0.0f)
		{
			const FVector D = GetActorLocation() - Cam->GetCameraLocation();
			if (VisibilityRadius > 0.0f)
			{
				const float R = D.Size2D();
				if (R < VisibilityRadius)
				{
					const float Scaled = VisibilityRadius * Pct;
					ScaledVolume *= (R > VisibilityRadius - Scaled)
						? (VisibilityRadius - R) / Scaled : 1.0f;
				}
				else
				{
					ScaledVolume = 0.0f;
				}
			}
			if (VisibilityHeight > 0.0f)
			{
				const float H = FMath::Abs(D.Z);
				if (H < VisibilityHeight)
				{
					const float Scaled = VisibilityHeight * Pct;
					ScaledVolume *= (H > VisibilityHeight - Scaled)
						? (VisibilityHeight - H) / Scaled : 1.0f;
				}
				else
				{
					ScaledVolume = 0.0f;
				}
			}
		}
	}

	// Spawn.
	if (bOn && Templates.Num() > 0 && ScaledVolume > 0.0f)
	{
		ParticleTimer += DeltaTime;
		while (ParticleTimer >= 1.0f / ScaledVolume)
		{
			ParticleTimer -= 1.0f / ScaledVolume;
			const int32 TIdx = PickTemplate();
			if (TIdx == INDEX_NONE)
			{
				break;
			}
			const FWoTParticleTemplate& T = Templates[TIdx];
			FParticle& P = Pool[Cursor];
			if (P.LifeSpan > 0.0f && !P.bHidden)
			{
				--NumParticles;   // overwriting a live particle (ring wrap)
			}

			// Square spawn cone: uniform +/-tan(Spread/2) on the rotation
			// frame's Y/Z axes.
			const float Radius = FMath::Tan(FMath::DegreesToRadians(Spread / 2.0f));
			const FRotationMatrix Frame(GetActorRotation());
			const FVector Dir =
				(Frame.GetScaledAxis(EAxis::X)
				 + (Radius - 2.0f * Radius * CosmeticRandom.FRand()) * Frame.GetScaledAxis(EAxis::Z)
				 + (Radius - 2.0f * Radius * CosmeticRandom.FRand()) * Frame.GetScaledAxis(EAxis::Y))
					.GetSafeNormal();

			P.TemplateIndex = TIdx;
			P.Location = GetActorLocation();
			P.Velocity = Dir * CosmeticRandom.FRandRange(
				T.MinInitialVelocity, T.MaxInitialVelocity);
			P.LifeSpan = T.LifeSpan;
			P.DrawScale = CosmeticRandom.FRandRange(T.MinDrawScale, T.MaxDrawScale);
			P.ScaleGlow = CosmeticRandom.FRandRange(T.MinScaleGlow, T.MaxScaleGlow);
			P.GrowRate = T.GrowPhase
				? CosmeticRandom.FRandRange(T.MinGrowRate, T.MaxGrowRate) : 0.0f;
			P.FadeRate = T.FadePhase
				? CosmeticRandom.FRandRange(T.MinFadeRate, T.MaxFadeRate) : 0.0f;
			P.GrowTime = T.GrowPhase ? T.LifeSpan / T.GrowPhase : 0.0f;
			P.FadeTime = T.FadePhase ? T.LifeSpan / T.FadePhase : 0.0f;
			P.GrowTimer = 0.0f;
			P.FadeTimer = 0.0f;
			P.bHidden = false;
			++NumParticles;
			Cursor = (Cursor + 1) % MaxParticles;
		}
	}

	// Update.
	if (NumParticles > 0)
	{
		for (FParticle& P : Pool)
		{
			if (P.LifeSpan <= 0.0f)
			{
				continue;
			}
			P.LifeSpan -= DeltaTime;
			if (P.LifeSpan <= 0.0f)
			{
				if (!P.bHidden)
				{
					P.bHidden = true;
					--NumParticles;
				}
				continue;
			}
			P.Velocity += Gravity * DeltaTime;
			P.Location += P.Velocity * DeltaTime;
			if (P.GrowRate != 0.0f && P.GrowTime > 0.0f)
			{
				P.GrowTimer += DeltaTime;
				while (P.GrowTimer >= P.GrowTime)
				{
					P.GrowRate *= -1.0f;
					P.GrowTimer -= P.GrowTime;
				}
				P.DrawScale += P.GrowRate * DeltaTime;
				P.bHidden = P.DrawScale <= 0.0f;
			}
			if (P.FadeRate != 0.0f && P.FadeTime > 0.0f)
			{
				P.FadeTimer += DeltaTime;
				while (P.FadeTimer >= P.FadeTime)
				{
					P.FadeRate *= -1.0f;
					P.FadeTimer -= P.FadeTime;
				}
				P.ScaleGlow += P.FadeRate * DeltaTime;
				P.bHidden = P.ScaleGlow <= 0.0f;
			}
		}
	}
}

void AWoTParticleSprayer::Tick(float DeltaSeconds)
{
	if (GetNetMode() == NM_DedicatedServer) return;
	Super::Tick(DeltaSeconds);
	UpdateLocalGoreVisibility();
	if (!ActorRotationRateUnits.IsNearlyZero())
	{
		RotationProgramElapsed += DeltaSeconds;
		const FRotator DeltaRotation = ActorRotationRateUnits
			* (360.0f / 65536.0f) * DeltaSeconds;
		AddActorLocalRotation(DeltaRotation);
		if (RotationProgramElapsed >= RotationGroupDelay)
		{
			RotateLiveParticles(DeltaRotation.Quaternion());
		}
	}

	if (LightProgram != EWoTSprayerLightProgram::Steady)
	{
		LightProgramElapsed += DeltaSeconds;
		float LightScale = 1.0f;
		bool bRotate = false;
		if (LightProgram == EWoTSprayerLightProgram::LinearFade)
		{
			LightScale = LightProgramDuration > 0.0f
				? FMath::Clamp(1.0f - LightProgramElapsed / LightProgramDuration, 0.0f, 1.0f)
				: 0.0f;
			bRotate = LightRotationYawRate != 0.0f;
		}
		else if (LightProgram == EWoTSprayerLightProgram::Firework07Stages)
		{
			// Firework07.uc:39-73: one-second in/out/in/out stages. Rotation
			// pauses only for the first fade-out stage.
			if (LightProgramElapsed < 1.0f)
			{
				LightScale = LightProgramElapsed;
				bRotate = true;
			}
			else if (LightProgramElapsed < 2.0f)
			{
				LightScale = 2.0f - LightProgramElapsed;
			}
			else if (LightProgramElapsed < 3.0f)
			{
				LightScale = LightProgramElapsed - 2.0f;
				bRotate = true;
			}
			else
			{
				LightScale = FMath::Max(4.0f - LightProgramElapsed, 0.0f);
				bRotate = true;
			}
		}
		if (Light && Light->IsVisible())
		{
			Light->SetIntensity(FullLightIntensity * FMath::Clamp(LightScale, 0.0f, 1.0f));
		}
		if (bRotate && LightRotationYawRate != 0.0f)
		{
			const float YawDegrees =
				LightRotationYawRate * (360.0f / 65536.0f) * DeltaSeconds;
			FRotator Rotation = GetActorRotation();
			Rotation.Yaw += YawDegrees;
			SetActorRotation(Rotation);
			RotateLiveParticles(YawDegrees);
		}
	}

	// Runtime one-shot plumbing (gore 4d): track a moving owner (the chunk blood
	// trail) and drop bOn when the burst window closes (TriggerTimed's Timer).
	if (FollowActor.IsValid())
	{
		const FVector FollowTarget = FollowActor->GetActorLocation() + FollowOffset;
		const FVector Delta = FollowTarget - GetActorLocation();
		SetActorLocation(FollowTarget);
		if (bFollowRotation)
		{
			// The manual-drive SetRotation(VeloDir) callers (fireball head/cone/
			// smoke, snow, seeker smoke) — the emission cone tracks the flier.
			SetActorRotation(FollowActor->GetActorRotation());
		}
		if (bShiftWithFollow && !Delta.IsNearlyZero())
		{
			// UE1 ShiftParticles: live particles ride the emitter (the glued
			// fireball head, vs the default world-anchored trail).
			ShiftParticles(Delta);
		}
	}
	if (bAlignGravityToRotation)
	{
		Gravity = GetActorRotation().RotateVector(AuthoredGravity);
	}
	if (BurstSeconds > 0.0f && bOn)
	{
		BurstElapsed += DeltaSeconds;
		if (BurstElapsed >= BurstSeconds)
		{
			bOn = false;
		}
	}

	// Decal emission (ParticleSprayer.uc:315-335): Volume x DecalPercent thrown
	// decals/sec along the spray cone while on. Local-only, like every UE1 decal.
	if (!DecalTypeName.IsNone() && bOn && Templates.Num() > 0 &&
	    Volume * DecalPercent > 0.0f)
	{
		DecalTimer += DeltaSeconds;
		const float Interval = 1.0f / (Volume * DecalPercent);
		while (DecalTimer >= Interval)
		{
			DecalTimer -= Interval;
			const FWoTParticleTemplate& T =
				Templates[CosmeticRandom.RandHelper(Templates.Num())];
			const float Radius = FMath::Tan(FMath::DegreesToRadians(Spread / 2.0f));
			const FRotationMatrix Frame(GetActorRotation());
			const FVector Dir =
				(Frame.GetScaledAxis(EAxis::X)
				 + (Radius - 2.0f * Radius * CosmeticRandom.FRand()) * Frame.GetScaledAxis(EAxis::Z)
				 + (Radius - 2.0f * Radius * CosmeticRandom.FRand()) * Frame.GetScaledAxis(EAxis::Y))
					.GetSafeNormal();
			WoTDecalFX::SpawnFlying(
			    *GetWorld(), DecalTypeName, GetActorLocation(),
			    Dir * CosmeticRandom.FRandRange(
					T.MinInitialVelocity, T.MaxInitialVelocity), &CosmeticRandom);
		}
	}

	const double Now = GetWorld() ? GetWorld()->GetTimeSeconds()
		: LastSimTimeSeconds + static_cast<double>(DeltaSeconds);
	APlayerCameraManager* Cam = UGameplayStatics::GetPlayerCameraManager(this, 0);
	if (bFreezeOutsideVisibility)
	{
		const FVector CameraDelta = Cam
			? GetActorLocation() - Cam->GetCameraLocation() : FVector::ZeroVector;
		const bool bInsideVisibility = Cam
			&& (VisibilityRadius <= 0.0f
				|| CameraDelta.Size2D() < VisibilityRadius)
			&& (VisibilityHeight <= 0.0f
				|| FMath::Abs(CameraDelta.Z) < VisibilityHeight);
		SetParticleInstancesVisible(bInsideVisibility);
		if (!bInsideVisibility)
		{
			return;
		}
	}

	// Retail LOS clipping suppresses both the render iterator and its simulation
	// clock for projected map sprayers. Runtime sprayers retain the old pause.
	if (bLOSClip && Cam)
	{
		FHitResult Hit;
		if (WoTVisBlockingTrace(GetWorld(), GetActorLocation(),
			Cam->GetCameraLocation(), this, Hit))
		{
			if (bFreezeOutsideVisibility)
			{
				SetParticleInstancesVisible(false);
			}
			return;
		}
	}

	// First time the camera comes within range: warm up to steady state.
	// UE1 primed only PrimeCount ticks (0 for every shipped flame class) at
	// render-iterator construction and built the plume from empty on first
	// sight — but the golden references were captured minutes into a live
	// session, so every plume they show is steady-state (oldest sprites at
	// the top, already shrunk/faded out). Warming a full max-lifespan of
	// 30fps ticks reproduces the state golden depicts; the skipped
	// first-sight buildup is a PORT_NOTES divergence.
	if (!bPrimed && Cam)
	{
		const FVector D = GetActorLocation() - Cam->GetCameraLocation();
		if ((VisibilityRadius <= 0.0f || D.Size2D() < VisibilityRadius)
			&& (VisibilityHeight <= 0.0f || FMath::Abs(D.Z) < VisibilityHeight))
		{
			bPrimed = true;
			float MaxLife = 0.0f;
			for (const FWoTParticleTemplate& T : Templates)
			{
				MaxLife = FMath::Max(MaxLife, T.LifeSpan);
			}
			const int32 Warm = FMath::Max(FMath::FloorToInt(PrimeCount),
										  FMath::CeilToInt(MaxLife * 30.0f) + 1);
			for (int32 i = 0; i < Warm; ++i)
			{
				ParticleTick(1.0f / 30.0f);
			}
			if (bFreezeOutsideVisibility)
			{
				LastSimTimeSeconds = Now;
			}
		}
	}

	// dt capped at 1s, sub-stepped at 1/120s.
	float Dt = bFreezeOutsideVisibility
		? static_cast<float>(FMath::Min(
			FMath::Max(Now - LastSimTimeSeconds, 0.0),
			static_cast<double>(MaxFrameInterval)))
		: FMath::Min(DeltaSeconds, MaxFrameInterval);
	while (Dt > MaxTickInterval)
	{
		Dt -= MaxTickInterval;
		ParticleTick(MaxTickInterval);
	}
	ParticleTick(Dt);
	if (bFreezeOutsideVisibility)
	{
		LastSimTimeSeconds = Now;
	}

	UpdateInstances();
	UpdateTextureAnimations(DeltaSeconds);
}

void AWoTParticleSprayer::UpdateTextureAnimations(const float DeltaSeconds)
{
#if WITH_AUTOMATION_TESTS
	WoTCountParticleSprayerAnimationUpdateForTesting();
#endif
	UWorld* World = GetWorld();
	UWoTTextureAnimationSubsystem* Subsystem = World
		? World->GetSubsystem<UWoTTextureAnimationSubsystem>() : nullptr;
	if (Subsystem == nullptr) return;
	for (int32 Index = 0; Index < TemplateAnimations.Num(); ++Index)
	{
		FTemplateAnimation& Animation = TemplateAnimations[Index];
		if (Animation.FrameIdentities.Num() <= 1) continue;
		float DrawTime = -1.0f;
		bool bRendered = AddISM.IsValidIndex(Index)
			&& WoTObservePrimitiveDraw(AddISM[Index],
				Animation.LastObservedRenderTimeSeconds, DrawTime);
		bRendered |= ModISM.IsValidIndex(Index)
			&& WoTObservePrimitiveDraw(ModISM[Index],
				Animation.LastObservedRenderTimeSeconds, DrawTime);
		if (!bRendered) continue;
		const int32 Frame = Subsystem->RequestFrame(Animation.TextureIdentity,
			Animation.FrameIdentities.Num(), Animation.MinFrameRate,
			Animation.MaxFrameRate, DrawTime, GFrameCounter);
		UTexture* Texture = Animation.FrameTextures.IsValidIndex(Frame)
			? Animation.FrameTextures[Frame].Get() : nullptr;
		if (Texture == nullptr)
		{
			const FString& Identity = Animation.FrameIdentities[Frame];
			int32 Dot = INDEX_NONE;
			if (!Identity.FindLastChar(TEXT('.'), Dot)) continue;
			FString Directory = Identity.Left(Dot);
			Directory.ReplaceInline(TEXT("."), TEXT("/"));
			const FString Name = Identity.RightChop(Dot + 1);
#if WITH_AUTOMATION_TESTS
			WoTCountTextureAnimationResolutionForTesting();
#endif
			Texture = UWoTRuntimeAssets::ResolveTexturePath(FSoftObjectPath(
				FString::Printf(TEXT("/Game/WoT/Textures/%s/%s.%s"),
					*Directory, *Name, *Name)),
				UWoTSpriteBillboardComponent::RuntimeTextureOptions());
		}
		if (Texture == nullptr) continue;
		if (AnimatedAddMids.IsValidIndex(Index) && AnimatedAddMids[Index])
			AnimatedAddMids[Index]->SetTextureParameterValue(TEXT("Tex"), Texture);
		if (AnimatedModMids.IsValidIndex(Index) && AnimatedModMids[Index])
			AnimatedModMids[Index]->SetTextureParameterValue(TEXT("Tex"), Texture);
	}
}

void AWoTParticleSprayer::SetOnlyOwnerSee(bool bOnlyOwner)
{
	bOnlyOwnerSee = bOnlyOwner;
	for (UInstancedStaticMeshComponent* Component : AddISM)
	{
		if (Component) { Component->SetOnlyOwnerSee(bOnlyOwner); }
	}
	for (UInstancedStaticMeshComponent* Component : ModISM)
	{
		if (Component) { Component->SetOnlyOwnerSee(bOnlyOwner); }
	}
}

bool AWoTParticleSprayer::AreInstancesOnlyOwnerSee() const
{
	bool bFound = false;
	for (const UInstancedStaticMeshComponent* Component : AddISM)
	{
		if (Component) { bFound = true; if (!Component->bOnlyOwnerSee) { return false; } }
	}
	for (const UInstancedStaticMeshComponent* Component : ModISM)
	{
		if (Component) { bFound = true; if (!Component->bOnlyOwnerSee) { return false; } }
	}
	return bFound;
}

void AWoTParticleSprayer::PrimeAuthoredCount()
{
	if (bPrimed || GetNetMode() == NM_DedicatedServer) { return; }
	bPrimed = true;
	for (int32 i = 0; i < FMath::FloorToInt(PrimeCount); ++i)
	{
		ParticleTick(1.0f / 30.0f);
	}
	UpdateInstances();
}

void AWoTParticleSprayer::ConfigureUE1Light(float Brightness, float Hue,
	                                         float Saturation, float Radius)
{
	if (!Light || Brightness <= 0.0f)
	{
		if (Light) { Light->SetVisibility(false); }
		return;
	}
	Light->SetVisibility(true);
	FWoTUE1LightRow Row;
	Row.Brightness = static_cast<uint8>(Brightness);
	Row.Hue = static_cast<uint8>(Hue);
	Row.Saturation = static_cast<uint8>(Saturation);
	Row.Radius = Radius;
	Row.Shape = EWoTUE1LightShape::NonIncidence;
	ConfigureUE1Light(Row);
}

void AWoTParticleSprayer::ConfigureUE1Light(const FWoTUE1LightRow& Row)
{
	if (!Light || Row.Brightness == 0)
	{
		if (Light) { Light->SetVisibility(false); }
		return;
	}
	Light->SetVisibility(true);
	FullLightIntensity = Row.Brightness / 64.0f * 5.0f;
	WoTUE1LightRegistry::Configure(Light, Row);
}

void AWoTParticleSprayer::ConfigureLightProgram(EWoTSprayerLightProgram Program,
	float Duration, float RotationYawRate)
{
	LightProgram = Program;
	LightProgramDuration = Duration;
	LightRotationYawRate = RotationYawRate;
	LightProgramElapsed = 0.0f;
}

void AWoTParticleSprayer::ConfigureRotationProgram(
	const FRotator& RotationRateUnits, float GroupDelay)
{
	ActorRotationRateUnits = RotationRateUnits;
	RotationGroupDelay = FMath::Max(0.0f, GroupDelay);
	RotationProgramElapsed = 0.0f;
}

void AWoTParticleSprayer::ShiftParticles(const FVector& Delta)
{
	if (Delta.IsNearlyZero())
	{
		return;
	}
	for (FParticle& P : Pool)
	{
		if (P.LifeSpan > 0.0f)
		{
			P.Location += Delta;
		}
	}
}

void AWoTParticleSprayer::RotateLiveParticles(float YawDegrees)
{
	// Firework07/09 author bRotationGrouped with PHYS_Rotating: the burst that
	// has already left the emitter rotates as one group, not just future spray.
	const FVector Origin = GetActorLocation();
	const FQuat Delta = FRotator(0.0f, YawDegrees, 0.0f).Quaternion();
	for (FParticle& P : Pool)
	{
		if (P.LifeSpan > 0.0f)
		{
			P.Location = Origin + Delta.RotateVector(P.Location - Origin);
			P.Velocity = Delta.RotateVector(P.Velocity);
		}
	}
}

void AWoTParticleSprayer::RotateLiveParticles(const FQuat& DeltaRotation)
{
	const FVector Origin = GetActorLocation();
	for (FParticle& P : Pool)
	{
		if (P.LifeSpan > 0.0f)
		{
			P.Location = Origin + DeltaRotation.RotateVector(P.Location - Origin);
			P.Velocity = DeltaRotation.RotateVector(P.Velocity);
		}
	}
}

void AWoTParticleSprayer::UpdateInstances()
{
	APlayerCameraManager* Cam = UGameplayStatics::GetPlayerCameraManager(this, 0);
	const FQuat Face = Cam
		? WoTSpriteFacing::ScreenAlignedQuat(Cam->GetCameraRotation())
		: FQuat::Identity;

	// Slot i of every ISM mirrors Pool[i]; non-matching templates collapse
	// to zero scale so slots never need re-homing between components.
	for (int32 T = 0; T < AddISM.Num(); ++T)
	{
		for (UInstancedStaticMeshComponent* C : {ModISM[T].Get(), AddISM[T].Get()})
		{
			if (!C)
			{
				continue;
			}
			while (C->GetInstanceCount() < MaxParticles)
			{
				C->AddInstance(FTransform(FQuat::Identity, FVector::ZeroVector,
										  FVector(0.0001f)), /*bWorldSpace*/ true);
			}
			for (int32 i = 0; i < MaxParticles; ++i)
			{
				const FParticle& P = Pool[i];
				// DrawScale > 0: UE1's fade block clobbers the grow block's
				// bHidden, so a shrunk-past-
				// zero particle that still glows is "visible" with negative
				// DrawScale — UE1's sprite rasterizer drew nothing for it,
				// but an ISM instance at |scale| would render a growing
				// mirrored quad (the too-large sprites above candle flames).
				const bool bLive = P.LifeSpan > 0.0f && !P.bHidden
					&& P.DrawScale > 0.0f && P.TemplateIndex == T;
				if (bLive)
				{
					const FWoTParticleTemplate& Tp = Templates[T];
					// world size = DrawScale * USize/VSize; plane is 100uu
					const FVector Scale(P.DrawScale * Tp.TexWidth / 100.0f,
										P.DrawScale * Tp.TexHeight / 100.0f, 1.0f);
					C->UpdateInstanceTransform(i, FTransform(Face, P.Location, Scale),
											   /*bWorldSpace*/ true);
					C->SetCustomDataValue(i, 0, FMath::Clamp(P.ScaleGlow, 0.0f, 1.0f));
				}
				else
				{
					// Zero-scale at the actor keeps ISM bounds local to the emitter;
					// visibility-driven simulation now uses the retail cylinder directly.
					C->UpdateInstanceTransform(i, FTransform(FQuat::Identity, GetActorLocation(),
															 FVector(0.0001f)), true);
				}
			}
			C->MarkRenderStateDirty();
		}
	}
}
