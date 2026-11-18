#pragma once

#include "CoreMinimal.h"
#include "RetailData/WoTRetailBuildContext.h"
#include "GameFramework/Actor.h"
#include "WoTBootstrapCore/Hash.h"
#include "WoTParticleSprayer.h"
#include "WoTWeaveFX.generated.h"

// The weave trail/impact sprayer bundles (Slice 6a-iii): the shipped
// ParticleSprayer leaf classes each active weave projectile spawns, chain-resolved
// offline from their .uc defaultproperties (the blood/flame registry pattern —
// Tools' scratch extractor emits the rows; verify against exports/script/Angreal).
// Sprayers are LOCAL COSMETICS: never replicated, skipped on dedicated servers;
// each machine re-derives them from the replicated projectile.

// One template row: the sim template + the material slug. Slug "Pkg__Tex" binds
// MI_PSAdd_/MI_PSMod_<slug>; "FB:Pkg__Tex" binds the flipbook particle variants
// (procedural textures — MI_PSAddFB_/MI_PSModFB_).
struct FWoTSprayerTemplateRow
{
	FWoTParticleTemplate Template;
	FName MaterialSlug;
};

struct FWoTSprayerBundleRow
{
	float Spread = 45.0f;
	float Volume = 10.0f;
	float MinVolume = 0.0f;
	float VolumeScalePct = 0.5f;
	float VisibilityRadius = 1600.0f;
	float VisibilityHeight = 1600.0f;
	FVector Gravity = FVector::ZeroVector;
	FName InitialState;
	float TimerDuration = 0.15f;
	bool bInitiallyOn = true;
	bool bOn = true;
	bool bAlignGravityToRotation = false;
	bool bLOSClip = false;
	bool bModulateOnly = false;
	bool bLinearDistribution = false;
	float PrimeCount = 0.0f;
	FRotator DefaultRotation = FRotator::ZeroRotator;
	bool bHasLight = false;
	FName LightRegistryKey;
	bool bOverrideLightHue = false;
	float LightBrightness = 0.0f;
	float LightHue = 0.0f;
	float LightSaturation = 255.0f;
	float LightRadius = 0.0f;
	EWoTSprayerLightProgram LightProgram = EWoTSprayerLightProgram::Steady;
	float LightDuration = 0.0f;
	float RotationYawRate = 0.0f;
	FRotator RotationRate = FRotator::ZeroRotator;
	float RotationGroupDelay = 0.0f;
	// The leaf's authored actor lifetime; zero means steady state.
	float SelfDestroySeconds = 0.0f;
	// Emit temporarily then stop (WaterSplash.uc:10-21); zero means the actor's
	// whole life.
	float BurstSeconds = 0.0f;
	TArray<FWoTSprayerTemplateRow> Templates;
};

// One WOT.Explosion leaf (Explosion.uc): an authored frame series played once
// over LifeSpan on a camera-facing quad, optionally with a light that dims as it burns out.
struct FWoTExplosionRow
{
	FName FramePackage = TEXT("Angreal");
	FName FramePrefix;   // e.g. "FBExp4" + 00..NN
	int32 FrameStart = 0;
	int32 FrameCount = 0;
	float LifeSpan = 1.5f;
	float DrawScale = 1.0f;
	float TexSize = 128.0f;
	float LightBrightness = 0.0f;
	float LightHue = 0.0f;
	float LightSaturation = 0.0f;
	float LightRadius = 0.0f;
	bool bGrowSustainShrink = false;
	float StopGrowPct = 0.0f;
	float ShrinkPct = 0.0f;
	// Base WOT.Explosion has LE_None (Incidence). Every currently ported leaf
	// explicitly authors LE_NonIncidence; retaining the default makes a future
	// base-derived row opt into the retail incidence shape.
	bool bNonIncidence = false;
};

struct FWoTWeaveFXSet
{
	TMap<FName, FWoTSprayerBundleRow> Bundles;
	TMap<FName, FWoTExplosionRow> Explosions;
};

struct FWoTWeaveFXBuilder
{
	static constexpr const TCHAR* FamilyName = TEXT("weave-fx");

	static bool Build(const FWoTRetailBuildContext& Context,
		FWoTWeaveFXSet& Out, FString& OutError);
	static FString CanonicalDigest(const FWoTWeaveFXSet& Set);
	static void Install(FWoTWeaveFXSet&& Set);
	static void SerializeCanonical(const FWoTWeaveFXSet& Set, TArray<uint8>& Out);
	static bool AreEquivalent(const FWoTWeaveFXSet& Built,
		const FWoTWeaveFXSet& Expected, FString& OutError);

#if WITH_AUTOMATION_TESTS
	static bool DecodeBundleForTesting(const FWoTRetailBuildContext& Context,
		FName Name, const FString& Identity, FWoTSprayerBundleRow& Out,
		FString& OutError);
	static void Reset();
#endif
};

// The local one-shot explosion actor (never replicated; every machine spawns its
// own from the projectile's Explode).
UCLASS()
class AWoTExplosionEffect : public AActor
{
	GENERATED_BODY()

public:
	AWoTExplosionEffect();

	void Configure(const FWoTExplosionRow& Row);
	virtual void Tick(float DeltaSeconds) override;

	UPROPERTY(VisibleAnywhere, Category = "WoT")
	TObjectPtr<class UWoTSpriteBillboardComponent> Sprite;
	UPROPERTY(VisibleAnywhere, Category = "WoT")
	TObjectPtr<class UPointLightComponent> Light;
	UPROPERTY(VisibleAnywhere, Category = "WoT") FName ConfigName;
	UPROPERTY() TWeakObjectPtr<AActor> FollowActor;

private:
	float FullIntensity = 0.0f;
	float FullLife = 1.5f;
	float BaseDrawScale = 1.0f;
	bool bGrowSustainShrink = false;
	float StopGrowPct = 0.0f;
	float ShrinkPct = 0.0f;
};

// The networked one-shot marker for weave sprayer bundles (the AWoTBloodBurst
// pattern): a thin bNetTemporary actor whose BeginPlay spawns the LOCAL bundle on
// every machine (Slice 7: the Mashadar pain burst).
UCLASS()
class AWoTWeaveBurst : public AActor
{
	GENERATED_BODY()

public:
	AWoTWeaveBurst();

	UPROPERTY(Replicated) FName Config;

	virtual void BeginPlay() override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
};

// ParticleSystems.SprayerBlower: a local, transient gravity impulse over the
// sprayers in an authored collision cylinder.
UCLASS()
class AWoTSprayerBlower : public AActor
{
	GENERATED_BODY()

public:
	AWoTSprayerBlower();
	void Configure(float Magnitude, float Duration, float Radius, float Height,
		const TArray<FName>& IgnoredTypes);
	virtual void Tick(float DeltaSeconds) override;

	UPROPERTY(VisibleAnywhere, Category = "WoT") FVector Wind = FVector::ZeroVector;
	UPROPERTY(VisibleAnywhere, Category = "WoT") FVector WindAccum = FVector::ZeroVector;
	UPROPERTY() TArray<TWeakObjectPtr<AWoTParticleSprayer>> AffectedSprayers;

private:
	FVector WindPerSecond = FVector::ZeroVector;
	bool bFinished = false;
};

namespace WoTWeaveFX
{
	AWoTSprayerBlower* BlowSprayers(UWorld& World, const FVector& Location,
		const FRotator& Rotation, float Magnitude = 250.0f,
		float Duration = 1.0f, float Radius = 60.0f, float Height = 60.0f,
		const TArray<FName>& IgnoredTypes = {});
	// Server-side: spawn the replicated burst marker (each machine then plays the
	// bundle locally).
	AWoTWeaveBurst* SpawnBurstMarker(UWorld& World, FName Config,
	                                 const FVector& Location,
	                                 const FRotator& Rotation = FRotator::ZeroRotator);

	// Explosion registry lookup by the shipped class name ("FireballExplode3").
	const FWoTExplosionRow* FindExplosion(FName Config);

	// Spawn the burst at the impact point (no-op on dedicated servers).
	AWoTExplosionEffect* SpawnExplosion(UWorld& World, FName Config,
	                                    const FVector& Location,
	                                    AActor* Follow = nullptr);

	// Registry lookup by the shipped class name (e.g. "TracerSprayer").
	const FWoTSprayerBundleRow* Find(FName Config);

	// Spawn one bundle as a local cosmetic (no-op on dedicated servers; returns
	// null). Follow re-pins the emitter to the actor each tick (the shipped
	// manual-drive SetLocation + the FollowActor model collapse to this);
	// bFollowRotation also copies the actor's rotation (the SetRotation(VeloDir)
	// drivers); bShiftWithFollow drags LIVE particles with the emitter delta
	// (UE1 ShiftParticles — the glued fireball head).
	AWoTParticleSprayer* SpawnLocal(UWorld& World, FName Config,
	                                const FVector& Location, const FRotator& Rotation,
	                                AActor* Follow = nullptr,
	                                bool bFollowRotation = false,
	                                bool bShiftWithFollow = false);

	// The shipped fade-out shape (bOn=false + actor LifeSpan) with a null guard —
	// every projectile Destroyed() path uses it.
	void FadeOut(AWoTParticleSprayer* Sprayer, float LifeSpan);
}
