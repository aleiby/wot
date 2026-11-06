#pragma once

#include "CoreMinimal.h"
#include "RetailData/WoTRetailBuildContext.h"
#include "GameFramework/Actor.h"
#include "WoTBootstrapCore/Hash.h"
#include "WoTMeshFX.generated.h"

class UMaterialInstanceDynamic;
class UInstancedStaticMeshComponent;
class UPointLightComponent;
class UTexture;
class UWoTMeshParticleRenderer;
class UWoTSpriteBillboardComponent;
class UWoTVertexAnimComponent;
class APawn;

enum class EWoTMeshEffectStyle : uint8
{
	Normal,
	Translucent,
	Modulated,
};

struct FWoTTextureAnimationRow
{
	TArray<FString> FrameIdentities;
	float MinFrameRate = 0.0f;
	float MaxFrameRate = 0.0f;
	float TexWidth = 0.0f;
	float TexHeight = 0.0f;
};

struct FWoTMeshEffectRow
{
	FName MeshName;
	FName SkinPackage;
	TArray<FName> SkinNames;
	TArray<FString> SkinIdentities;
	FName ProceduralMaterialName;
	float SkinStepSeconds = 0.0f;
	FName AnimName;
	float AnimRate = 1.0f;
	bool bPlayFrameSequence = false;
	bool bLoopAnim = false;
	bool bLoopSkinSeries = false;
	bool bRandomSkinEachTick = false;
	float LifeSpan = 1.0f;
	float DrawScale = 1.0f;
	float ScaleGlow = 1.0f;
	float AmbientGlow = 0.0f;
	// Retail bUnlit: the mesh renders at GUnlitColor =
	// clamp(ScaleGlow*0.5 + AmbientGlow/256) instead of scene lighting.
	bool bUnlit = false;
	bool bMeshCurvy = false;
	EWoTMeshEffectStyle Style = EWoTMeshEffectStyle::Normal;
	bool bFollow = false;
	bool bFadeEnvelope = false;
	float FadeInTime = 0.0f;
	float SustainTime = 0.0f;
	float FadeOutTime = 0.0f;
	float MeltTime = 0.0f;
	FName MeltAnim;
	FName MeltSoundSubPath;
	bool bEnvironmentMap = false;
	float MinSpinRate = 0.0f;
	float MaxSpinRate = 0.0f;
	float MinRiseRate = 0.0f;
	float MaxRiseRate = 0.0f;
	float MinGrowRate = 0.0f;
	float MaxGrowRate = 0.0f;
	bool bHasLight = false;
	FName LightRegistryKey;
	bool bOverrideLightColor = false;
	float LightBrightness = 0.0f;
	float LightHue = 0.0f;
	float LightSaturation = 255.0f;
	float LightRadius = 0.0f;
	TArray<TPair<FName, float>> ClassScaleOverrides;
};

struct FWoTPawnOverlayRow
{
	FName SkinPackage = TEXT("Angreal");
	TArray<FName> SkinNames;
	TArray<FString> SkinIdentities;
	float SkinStepSeconds = 0.0f;
	bool bLoopSkinSeries = false;
	bool bStepSkinEveryTick = false;
	float LifeSpan = 0.0f;
	float ScaleGlow = 0.5f;
	float AmbientGlow = 64.0f;
	float Fatness = 157.0f;
	bool bFadeEnvelope = false;
	bool bHasLight = false;
	FName LightRegistryKey;
	bool bOverrideLightColor = false;
	float LightBrightness = 0.0f;
	float LightHue = 0.0f;
	float LightSaturation = 255.0f;
	float LightRadius = 0.0f;
	bool bSpawnLightningRings = false;
};

struct FWoTSpriteEffectRow
{
	FName Package;
	TArray<FName> Frames;
	TArray<FString> FrameIdentities;
	float TexWidth = 64.0f;
	float TexHeight = 64.0f;
	float DrawScale = 1.0f;
	float ScaleGlow = 1.0f;
	float FrameRate = 30.0f;
	float MinFrameRate = 0.0f;
	float MaxFrameRate = 0.0f;
	float LifeSpan = 0.0f;
	bool bModulated = false;
};

struct FWoTMeshFXSet
{
	TMap<FName, FWoTMeshEffectRow> MeshEffects;
	TMap<FName, FWoTPawnOverlayRow> PawnOverlays;
	TMap<FName, FWoTSpriteEffectRow> SpriteEffects;
	TMap<FString, FWoTTextureAnimationRow> TextureAnimations;
	TMap<FName, FString> ClassTextureHeads;
	TArray<FString> LightningStreakTextures;
	// Retail leaf -> most-specific-first class chain, used by IceLeech's
	// ClassIsA scale exceptions after the decoded class graph is released.
	TMap<FName, TArray<FName>> ClassChains;
};

struct FWoTMeshFXBuilder
{
	static constexpr const TCHAR* FamilyName = TEXT("mesh-fx");

	static bool Build(const FWoTRetailBuildContext& Context,
		FWoTMeshFXSet& Out, FString& OutError);
	static FString CanonicalDigest(const FWoTMeshFXSet& Set);
	static void Install(FWoTMeshFXSet&& Set);
	static void SerializeCanonical(const FWoTMeshFXSet& Set, TArray<uint8>& Out);
	static bool AreEquivalent(const FWoTMeshFXSet& Built,
		const FWoTMeshFXSet& Expected, FString& OutError);

#if WITH_AUTOMATION_TESTS
	static void Reset();
#endif
};

UCLASS()
class AWoTMeshEffect : public AActor
{
	GENERATED_BODY()

public:
	AWoTMeshEffect();
	void Configure(FName Config, const FWoTMeshEffectRow& Row, AActor* Follow);
	virtual void Tick(float DeltaSeconds) override;

	UPROPERTY(VisibleAnywhere, Category = "WoT") FName ConfigName;
	UPROPERTY() TWeakObjectPtr<AActor> FollowActor;
	UPROPERTY(VisibleAnywhere, Category = "WoT") TObjectPtr<UWoTVertexAnimComponent> Mesh;
	UPROPERTY(VisibleAnywhere, Category = "WoT")
	TObjectPtr<UInstancedStaticMeshComponent> ParticleQuads;
	UPROPERTY(VisibleAnywhere, Category = "WoT") TObjectPtr<UPointLightComponent> Light;
	// UE1 actor render state (Actor.Style/bUnlit/ScaleGlow/AmbientGlow).
	// UWoTMeshLight reads these by name, as for AWoTDecal: in game every
	// effect mesh is baked against the map's light table, so without them a
	// lit effect took zone ambient alone (the Light Orb's glass globe went
	// unseen) and an unlit one lost GUnlitColor to the zone ambient.
	UPROPERTY(VisibleAnywhere, Category = "WoT|UE1") uint8 Style = 1;
	UPROPERTY(VisibleAnywhere, Category = "WoT|UE1") bool bUnlit = false;
	UPROPERTY(VisibleAnywhere, Category = "WoT|UE1") float ScaleGlow = 1.0f;
	UPROPERTY(VisibleAnywhere, Category = "WoT|UE1") float AmbientGlow = 0.0f;
	// Retail draws this effect as a bParticles mesh: GUnlitColor times
	// ScaleGlow once more (UE1's mesh renderer), never lit.
	UPROPERTY(VisibleAnywhere, Category = "WoT|UE1") bool bParticles = false;
	float CurrentFadeScalar = 1.0f;
	void SetEffectDrawScale(float DrawScale);
	void SetEffectGlow(float Glow);
#if WITH_AUTOMATION_TESTS
	void MarkDrawnForTesting();
	void MarkDrawnForTesting(float DrawTimeSeconds);
	UTexture* GetBoundTextureForTesting() const;
	UMaterialInstanceDynamic* GetParticleMaterialForTesting() const;
	FIntPoint GetParticleTextureSizeForTesting() const;
	int32 GetCurrentSkinIndexForTesting() const { return CurrentSkin; }
	const FString& GetCurrentTextureIdentityForTesting() const
		{ return CurrentTextureIdentity; }
#endif

private:
	void SetSkinFrame(int32 Index);
	void SetSkinTextureIdentity(const FString& Identity);
	void ApplyAnimatedSkin(int32 Index);
	UWoTMeshParticleRenderer* GetParticleRenderer() const;
	FWoTMeshEffectRow ActiveRow;
	UPROPERTY(Transient) TObjectPtr<UMaterialInstanceDynamic> SkinMaterial;
	// The skin is a mesh-lit family material: its glow is the lights'
	// Diffuse (MLScaleGlow), not GUnlitColor or the procedural Glow scalar.
	bool bLitSkin = false;
	// The unlit colour for the current ScaleGlow/AmbientGlow.
	float UnlitGlow() const;
	// Push the current ScaleGlow/AmbientGlow to the bound skin.
	void PushFadeGlow();
	float Elapsed = 0.0f;
	float FullLife = 1.0f;
	float FullLightIntensity = 0.0f;
	float SpinRate = 0.0f;
	float RiseRate = 0.0f;
	float GrowRate = 0.0f;
	float AccumYaw = 0.0f;
	float CurrentDrawScale = 1.0f;
	float LastObservedAnimationRenderTimeSeconds = -1.0f;
	float CurrentAnimationDrawTimeSeconds = -1.0f;
	FRandomStream CosmeticRandom;
	int32 CurrentSkin = INDEX_NONE;
	FString CurrentTextureIdentity;
	bool bMeltStarted = false;
};

// UE1 PHYS_Trailer+bAnimByOwner skin effect: redraw the owning pawn's current
// vertex-animation pose with a fattened, all-slot override skin.
UCLASS()
class AWoTPawnSkinOverlay : public AActor
{
	GENERATED_BODY()

public:
	AWoTPawnSkinOverlay();
	void Configure(FName Config, const FWoTPawnOverlayRow& Row, APawn* Pawn);
	virtual void Tick(float DeltaSeconds) override;
	virtual void Destroyed() override;

	UPROPERTY(VisibleAnywhere, Category = "WoT") FName ConfigName;
	UPROPERTY() TWeakObjectPtr<APawn> FollowPawn;
	UPROPERTY(VisibleAnywhere, Category = "WoT") TObjectPtr<UWoTVertexAnimComponent> Mesh;
	UPROPERTY(VisibleAnywhere, Category = "WoT") TObjectPtr<UPointLightComponent> Light;
	// UE1 actor render state (Actor.Style/bUnlit/ScaleGlow/AmbientGlow).
	// UWoTMeshLight reads these by name, as for AWoTDecal: in game every
	// effect mesh is baked against the map's light table, so without them a
	// lit effect took zone ambient alone (the Light Orb's glass globe went
	// unseen) and an unlit one lost GUnlitColor to the zone ambient.
	UPROPERTY(VisibleAnywhere, Category = "WoT|UE1") uint8 Style = 1;
	UPROPERTY(VisibleAnywhere, Category = "WoT|UE1") bool bUnlit = false;
	UPROPERTY(VisibleAnywhere, Category = "WoT|UE1") float ScaleGlow = 1.0f;
	UPROPERTY(VisibleAnywhere, Category = "WoT|UE1") float AmbientGlow = 0.0f;
	float CurrentFadeScalar = 1.0f;
	int32 LiveRingCount() const;

private:
	void SetSkinFrame(int32 Index);
	void SetSkinTextureIdentity(const FString& Identity);
	void ApplyAnimatedSkin(int32 Index);
	void TickRings(float DeltaSeconds);
	FWoTPawnOverlayRow ActiveRow;
	UPROPERTY(Transient) TArray<TObjectPtr<UMaterialInstanceDynamic>> SkinMaterials;
	UPROPERTY(Transient) TArray<TObjectPtr<AWoTMeshEffect>> Rings;
	TArray<float> RingRiseRates;
	TArray<float> RingHeights;
	float Elapsed = 0.0f;
	float FullLife = 0.0f;
	float FullLightIntensity = 0.0f;
	float NextRingSpawnTime = 0.0f;
	float LastObservedAnimationRenderTimeSeconds = -1.0f;
	float CurrentAnimationDrawTimeSeconds = -1.0f;
	FRandomStream CosmeticRandom;
	int32 RingIndex = 0;
	int32 CurrentSkin = INDEX_NONE;
	FString CurrentTextureIdentity;
	FName LastSequence;
};

UCLASS()
class AWoTWeaveSpriteEffect : public AActor
{
	GENERATED_BODY()

public:
	AWoTWeaveSpriteEffect();
	void Configure(FName Config, const FWoTSpriteEffectRow& Row, AActor* Follow);
	virtual void Tick(float DeltaSeconds) override;

	UPROPERTY(VisibleAnywhere, Category = "WoT") FName ConfigName;
	UPROPERTY() TWeakObjectPtr<AActor> FollowActor;
	UPROPERTY(VisibleAnywhere, Category = "WoT")
	TObjectPtr<UWoTSpriteBillboardComponent> Sprite;
};

UCLASS()
class AWoTMeshEffectBurst : public AActor
{
	GENERATED_BODY()

public:
	AWoTMeshEffectBurst();
	UPROPERTY(Replicated) FName Config;
	UPROPERTY(Replicated) TObjectPtr<AActor> FollowActor;
	virtual void BeginPlay() override;
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;
};

UCLASS()
class AWoTPawnOverlayBurst : public AActor
{
	GENERATED_BODY()

public:
	AWoTPawnOverlayBurst();
	UPROPERTY(Replicated) FName Config;
	UPROPERTY(Replicated) TObjectPtr<APawn> FollowPawn;
	virtual void BeginPlay() override;
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;
};

// Angreal.TaintVisual: one local cosmetic owner for its three ExpWard comets,
// center disk, inner glow, and pulsing point light.
UCLASS()
class AWoTTaintVisual : public AActor
{
	GENERATED_BODY()

public:
	AWoTTaintVisual();
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	int32 GetPartCount() const;

	UPROPERTY() TWeakObjectPtr<AActor> FollowActor;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UWoTVertexAnimComponent> CometA;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UWoTVertexAnimComponent> CometB;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UWoTVertexAnimComponent> CometC;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UWoTSpriteBillboardComponent> Center;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UWoTSpriteBillboardComponent> Glow;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UPointLightComponent> Light;
	// The comets' UE1 render state (TaintVisual.uc:32-40: STY_Translucent,
	// bUnlit, ScaleGlow 5), read by UWoTMeshLight as for AWoTMeshEffect.
	UPROPERTY(VisibleAnywhere, Category = "WoT|UE1") uint8 Style = 3;
	UPROPERTY(VisibleAnywhere, Category = "WoT|UE1") bool bUnlit = true;
	UPROPERTY(VisibleAnywhere, Category = "WoT|UE1") float ScaleGlow = 5.0f;
	UPROPERTY(VisibleAnywhere, Category = "WoT|UE1") float AmbientGlow = 0.0f;
};

namespace WoTMeshFX
{
	const FWoTMeshEffectRow* Find(FName Config);
	/** Resolves the row's retail ClassIsA draw-scale override for an actor. */
	float ResolveDrawScale(const FWoTMeshEffectRow& Row, const AActor* Follow);
	AWoTMeshEffect* SpawnLocal(UWorld& World, FName Config,
		const FVector& Location, const FRotator& Rotation = FRotator::ZeroRotator,
		AActor* Follow = nullptr);
	AWoTMeshEffectBurst* SpawnBurstMarker(UWorld& World, FName Config,
		const FVector& Location, const FRotator& Rotation = FRotator::ZeroRotator,
		AActor* Follow = nullptr);

	const FWoTPawnOverlayRow* FindPawnOverlay(FName Config);
	AWoTPawnSkinOverlay* SpawnPawnOverlayLocal(UWorld& World, FName Config,
		APawn* Pawn);
	AWoTPawnOverlayBurst* SpawnPawnOverlayBurst(UWorld& World, FName Config,
		APawn* Pawn);

	const FWoTSpriteEffectRow* FindSprite(FName Config);
	const FWoTTextureAnimationRow* FindTextureAnimation(FStringView Identity);
	const FString* FindClassTextureHead(FName Config);
	bool ConfigureClassSprite(UWoTSpriteBillboardComponent& Sprite, FName Config,
		float FixedFrameRate = 0.0f);
	TConstArrayView<FString> LightningStreakTextures();
	AWoTWeaveSpriteEffect* SpawnSpriteLocal(UWorld& World, FName Config,
		const FVector& Location, AActor* Follow = nullptr);

	AWoTTaintVisual* SpawnTaintVisual(UWorld& World, AActor* Follow);
}
