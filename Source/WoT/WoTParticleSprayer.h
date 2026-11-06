#pragma once

#include "CoreMinimal.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "GameFramework/Actor.h"
#include "Math/RandomStream.h"
#include "WoTEvents.h"
#include "WoTParticleSprayer.generated.h"

class UPointLightComponent;
class UMaterialInstanceDynamic;
class UTexture;
class UTexture2D;
struct FWoTUE1LightRow;

UCLASS(Transient)
class WOT_API UWoTParticleSprayerRenderComponent final
	: public UInstancedStaticMeshComponent
{
	GENERATED_BODY()

protected:
	virtual void OnRegister() override;
};

enum class EWoTSprayerLightProgram : uint8
{
	Steady,
	LinearFade,
	Firework07Stages,
};

UENUM()
enum class EWoTParticleTriggerMode : uint8
{
	None,
	Toggle,
	Control,
	Timed,
};

#if WITH_AUTOMATION_TESTS
WOT_API void WoTSetParticleSprayerLifecycleObserverForTesting(
	TFunction<void(const TCHAR*, const AActor&, const UActorComponent*)> Observer);
#endif

// One entry of ParticleSprayer.Templates[16] (ParticleSprayer.uc:43-65),
// resolved through the .uc default chain + T3D overrides at import.
USTRUCT(BlueprintType)
struct FWoTParticleTemplate
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere) float LifeSpan = 1.0f;
	UPROPERTY(EditAnywhere) float Weight = 1.0f;
	UPROPERTY(EditAnywhere) float MaxInitialVelocity = 0.0f;
	UPROPERTY(EditAnywhere) float MinInitialVelocity = 0.0f;
	UPROPERTY(EditAnywhere) float MaxDrawScale = 1.0f;
	UPROPERTY(EditAnywhere) float MinDrawScale = 1.0f;
	UPROPERTY(EditAnywhere) float MaxScaleGlow = 1.0f;
	UPROPERTY(EditAnywhere) float MinScaleGlow = 1.0f;
	UPROPERTY(EditAnywhere) uint8 GrowPhase = 0;
	UPROPERTY(EditAnywhere) float MaxGrowRate = 0.0f;
	UPROPERTY(EditAnywhere) float MinGrowRate = 0.0f;
	UPROPERTY(EditAnywhere) uint8 FadePhase = 0;
	UPROPERTY(EditAnywhere) float MaxFadeRate = 0.0f;
	UPROPERTY(EditAnywhere) float MinFadeRate = 0.0f;
	// Sprite world size = DrawScale * texture USize/VSize.
	UPROPERTY(EditAnywhere) float TexWidth = 64.0f;
	UPROPERTY(EditAnywhere) float TexHeight = 64.0f;
};

// Legend's ParticleSprayer — the sim behavior-matched in full: 128-slot ring
// buffer, spawn timer at
// ScaledVolume/sec, square spawn cone (uniform +/-tan(Spread/2) offsets on
// the rotation frame's Y/Z axes), Gravity integration, Grow/Fade rates that
// invert every LifeSpan/Phase seconds, distance-scaled volume within
// VisibilityRadius/Height, optional line-of-sight clip, PrimeCount warm-up
// at 30fps, dt capped at 1s and sub-stepped at 1/120s. Projected map sprayers
// freeze outside the retail render cylinder; runtime-spawned sprayers retain
// their authored lifecycle behavior.
// Rendering: per-template camera-facing ISMs (modulate pass then additive pass
// for the exact STY_Translucent split, see AWoTCorona),
// per-instance custom data 0 = ScaleGlow. STY_Modulated uses the 2x mesh master,
// whose graph deliberately ignores that value (UE1 PF_Modulated ignored the
// sprite vertex color); ScaleGlow still controls the native visibility cutoff.
UCLASS()
class AWoTParticleSprayer : public AActor, public IWoTTriggerable
{
	GENERATED_BODY()

public:
	AWoTParticleSprayer();
	UPROPERTY(VisibleAnywhere, Category = "WoT") FName ConfigName;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT") float Spread = 45.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT") float Volume = 10.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT") float MinVolume = 0.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT") FVector Gravity = FVector::ZeroVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	bool bIsWindResistant = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	bool bAlignGravityToRotation = false;
	FVector AuthoredGravity = FVector::ZeroVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT") float PrimeCount = 0.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT") float VolumeScalePct = 0.5f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT") float VisibilityRadius = 1600.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT") float VisibilityHeight = 1600.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	bool bFreezeOutsideVisibility = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT") bool bOn = true;
	// ParticleSprayer TriggerToggle/TriggerControl/TriggerTimed states. These are
	// deliberately local: SpecialEffectsTrigger ran on every UE1 machine and
	// drove purely presentational emitters without replicated state.
	UPROPERTY(EditAnywhere, Category = "WoT|Trigger") FName TagName;
	UPROPERTY(EditAnywhere, Category = "WoT|Trigger")
	EWoTParticleTriggerMode TriggerMode = EWoTParticleTriggerMode::None;
	UPROPERTY(EditAnywhere, Category = "WoT|Trigger") bool bInitiallyOn = true;
	UPROPERTY(EditAnywhere, Category = "WoT|Trigger") float TriggerSeconds = 0.15f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT") bool bLOSClip = false;
	// false = DIST_Random (weighted); DIST_Linear approximated as weighted
	// random (statistically identical over a second of spawns — PORT_NOTES).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT") bool bLinearDistribution = false;

	// --- Runtime one-shot lifecycle (gore Slice 4d; map-placed plumes leave these
	// at 0 so the golden-calibrated behavior is untouched). ---
	// Emit for this long after BeginPlay, then bOn drops (the TriggerTimed
	// TimerDuration / BloodSpray02 Duration shape). 0 = steady state.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT") float BurstSeconds = 0.0f;
	// Destroy the actor this long after BeginPlay (the blood LifeSpan). 0 = never.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT") float SelfDestroySeconds = 0.0f;
	// STY_Modulated configs draw ONLY the neutral 2x-modulate ISM pass — the
	// two-pass mod+add split is the STY_Translucent look and double-brightens
	// modulated sprites.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT") bool bModulateOnly = false;
	// Track a moving owner each tick (the chunk blood trail).
	UPROPERTY() TWeakObjectPtr<AActor> FollowActor;
	// World-space offset from the followed actor (UE1 PHYS_Trailer PrePivot —
	// the CarrySealSprayer's Z=75 over-the-head seat). Zero for every other
	// follow consumer.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	FVector FollowOffset = FVector::ZeroVector;
	// Also copy the owner's rotation (the shipped manual-drive SetRotation(VeloDir)
	// projectile trails; 6a-iii). Off for the blood trail (chunks tumble).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT") bool bFollowRotation = false;
	// Drag LIVE particles with the emitter delta (UE1 ShiftParticles — the glued
	// fireball head; 6a-iii). Off = particles stay world-anchored (trails).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT") bool bShiftWithFollow = false;
	UPROPERTY(VisibleAnywhere, Category = "WoT") TObjectPtr<UPointLightComponent> Light;
	void ConfigureUE1Light(float Brightness, float Hue, float Saturation, float Radius);
	void ConfigureUE1Light(const FWoTUE1LightRow& Row);
	void ConfigureLightProgram(EWoTSprayerLightProgram Program, float Duration,
		float RotationYawRate = 0.0f);
	void ConfigureRotationProgram(const FRotator& RotationRateUnits,
		float GroupDelay);
	// Move every live particle without moving the emitter. AirBlastProj uses this
	// to inherit only its caster's translation while its own forward velocity
	// continues to advance the emitter (AirBlastProj.uc:71-90).
	void ShiftParticles(const FVector& Delta);
	// Decal emission (ParticleSprayer.uc:315-335): while on, throw
	// Volume x DecalPercent flying decals/sec along the spray cone. None/0 = off
	// (every map-placed sprayer — golden-safe).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT") FName DecalTypeName;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT") float DecalPercent = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	TArray<FWoTParticleTemplate> Templates;

	// Per-template materials: translucent uses MI_PSAdd_/MI_PSMod_; modulated
	// users bind a MID of M_WoT_MeshModulate directly to the authenticated Tex.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	TArray<TObjectPtr<UMaterialInterface>> AddMaterials;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	TArray<TObjectPtr<UMaterialInterface>> ModMaterials;
	void ConfigureTemplateAnimation(int32 TemplateIndex,
		const FString& TextureIdentity, TArray<FString> FrameIdentities,
		TArray<TObjectPtr<UTexture2D>> FrameTextures,
		float MinFrameRate, float MaxFrameRate);
	int32 GetTemplateAnimationFrameCount(int32 TemplateIndex) const;
#if WITH_AUTOMATION_TESTS
	void MarkTemplateDrawnForTesting(int32 TemplateIndex);
	void MarkTemplateDrawnForTesting(int32 TemplateIndex, float DrawTimeSeconds);
	int32 GetLiveParticleCountForTesting() const { return NumParticles; }
	bool AreParticleInstancesVisibleForTesting() const;
	double GetLastSimTimeSecondsForTesting() const { return LastSimTimeSeconds; }
	UTexture* GetTemplateBoundTextureForTesting(int32 TemplateIndex) const;
	FString GetTemplateFrameIdentityForTesting(int32 TemplateIndex,
		int32 FrameIndex) const;
#endif

	// Cast-spawned bundles build from empty like shipped (PrimeCount=0): skip the
	// first-sight steady-state warm-up (a map-placed-plume golden calibration).
	void SkipPriming() { bPrimed = true; }
	// Runtime bundles with an authored nonzero PrimeCount (BFExpSprayer) prime
	// exactly that many 30Hz simulation ticks, rather than the placed-plume
	// steady-state warm-up.
	void PrimeAuthoredCount();
	void SetOnlyOwnerSee(bool bOnlyOwner);
	bool AreInstancesOnlyOwnerSee() const;
	// Blood render policy is local and reversible. The particle simulation keeps
	// ticking while its render components are hidden by the current preference.
	void ConfigureLocalGoreLevel(uint8 InGoreLevel);
	bool IsLocalGoreVisibleForTesting() const { return bLocalGoreVisible; }
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	bool bOnlyOwnerSee = false;

	virtual void Tick(float DeltaSeconds) override;
	virtual void WoTTrigger(AActor* Other, APawn* EventInstigator) override;
	virtual void WoTUnTrigger(AActor* Other, APawn* EventInstigator) override;
	virtual FName GetUE1Tag() const override { return TagName; }

protected:
	virtual void BeginPlay() override;

private:
	static constexpr int32 MaxParticles = 128;          // original sprayer's sprite cap
	static constexpr float MaxFrameInterval = 1.0f;     // max seconds between frames
	static constexpr float MaxTickInterval = 1.0f / 120.0f;  // smooths frame intervals

	struct FParticle
	{
		int32 TemplateIndex = 0;
		FVector Location = FVector::ZeroVector;
		FVector Velocity = FVector::ZeroVector;
		float LifeSpan = 0.0f;
		float DrawScale = 1.0f;
		float ScaleGlow = 1.0f;
		float GrowRate = 0.0f, GrowTime = 0.0f, GrowTimer = 0.0f;
		float FadeRate = 0.0f, FadeTime = 0.0f, FadeTimer = 0.0f;
		bool bHidden = true;
	};

	FParticle Pool[MaxParticles];
	int32 Cursor = 0;
	int32 NumParticles = 0;
	float ParticleTimer = 0.0f;
	bool bPrimed = false;
	float BurstElapsed = 0.0f;
	float DecalTimer = 0.0f;
	FRandomStream CosmeticRandom;
	EWoTSprayerLightProgram LightProgram = EWoTSprayerLightProgram::Steady;
	float LightProgramDuration = 0.0f;
	float LightProgramElapsed = 0.0f;
	float FullLightIntensity = 0.0f;
	float LightRotationYawRate = 0.0f;
	FRotator ActorRotationRateUnits = FRotator::ZeroRotator;
	float RotationGroupDelay = 0.0f;
	float RotationProgramElapsed = 0.0f;
	FTimerHandle TriggerRestoreTimer;
	void RestoreTriggeredState();
	void UpdateLocalGoreVisibility();
	bool bHasLocalGoreLevel = false;
	bool bLocalGoreVisible = true;
	bool bParticleInstancesVisible = true;
	uint8 LocalGoreLevel = 0;
	double LastSimTimeSeconds = 0.0;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UInstancedStaticMeshComponent>> AddISM;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UInstancedStaticMeshComponent>> ModISM;
	struct FTemplateAnimation
	{
		FString TextureIdentity;
		TArray<FString> FrameIdentities;
		TArray<TObjectPtr<UTexture2D>> FrameTextures;
		float MinFrameRate = 0.0f;
		float MaxFrameRate = 0.0f;
		float LastObservedRenderTimeSeconds = -1.0f;
	};
	TArray<FTemplateAnimation> TemplateAnimations;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> AnimatedAddMids;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> AnimatedModMids;

	int32 PickTemplate() const;
	void ParticleTick(float DeltaTime);
	void SetParticleInstancesVisible(bool bVisible);
	void RotateLiveParticles(float YawDegrees);
	void RotateLiveParticles(const FQuat& DeltaRotation);
	void UpdateInstances();
	void UpdateTextureAnimations(float DeltaSeconds);
};
