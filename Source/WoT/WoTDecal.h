#pragma once

#include "CoreMinimal.h"
#include "RetailData/WoTRetailBuildContext.h"
#include "GameFramework/Actor.h"
#include "WoTBootstrapCore/Hash.h"
#include "WoTDecal.generated.h"

class UMaterialInstanceDynamic;
class UWoTVertexAnimComponent;

// Legend.DecalManager.  MP maps place one actor tagged DecalMan and author the
// live-decal cap and expiry/shrink window on it.  The actor is replicated in
// retail so both authority-observed and locally-created decals use one policy.
UCLASS()
class AWoTDecalManager : public AActor
{
	GENERATED_BODY()

public:
	AWoTDecalManager();
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UPROPERTY(Replicated, EditAnywhere, Category = "WoT") FName TagName = TEXT("DecalMan");
	UPROPERTY(Replicated, EditAnywhere, Category = "WoT") int32 MaxDecals = 1000;
	UPROPERTY(Replicated, EditAnywhere, Category = "WoT") float ShrinkTime = 0.0f;
	UPROPERTY(Replicated, EditAnywhere, Category = "WoT") float DecalExpireTime = 300.0f;

	static AWoTDecalManager* Find(UWorld* World, FName ManagerTag = TEXT("DecalMan"));
};

// The actor Style a decal class renders with (Decal.uc Style=STY_Modulated;
// BalefireDecal.uc overrides STY_Translucent). UE1 drew every decal as the
// Legend.DecalTRI mesh through URender::DrawMesh with PF_Unlit (bUnlit=True):
// GetPolyFlags maps the Style to the blend.
UENUM()
enum class EWoTDecalStyle : uint8
{
	// PF_Modulated: FB = 2 * Tex * Dst, texture only (the original renderers'
	// DECAL_TEXTURE path; the D3D driver whitens the vertex colour), no fog.
	Modulated,
	// PF_Translucent: FB = Src + Dst * (1 - Src) with Src = Tex * GUnlitColor
	// (the original renderers' TEXTURE_TIMES_ITRGB path).
	Translucent,
};

// One decal class's config (the Decal-family defaultproperties, census in
// WoTDecal.cpp): the texture list (BloodDecal's weighted-by-repetition list, or
// BalefireDecal's ordered AnimTextures), the base DrawScale, and the render
// Style/AmbientGlow. World size = the DecalTRI mesh (61.5uu square at DrawScale
// 1) x DrawScale; the Align fit square is EdgeRadius 30 x DrawScale.
USTRUCT()
struct FWoTDecalConfig
{
	GENERATED_BODY()

	UPROPERTY(EditDefaultsOnly, Category = "WoT") TArray<FName> Textures;
	UPROPERTY(EditDefaultsOnly, Category = "WoT") TArray<FName> FlyingTextures;
	UPROPERTY(EditDefaultsOnly, Category = "WoT") float DrawScale = 1.0f;
	// The texture package (/Game/WoT/Textures/<Package>/<Tex>).
	UPROPERTY(EditDefaultsOnly, Category = "WoT") FName Package = FName("ParticleSystems");
	UPROPERTY(EditDefaultsOnly, Category = "WoT") EWoTDecalStyle Style = EWoTDecalStyle::Modulated;
	// Actor AmbientGlow byte (feeds GUnlitColor for the translucent style).
	UPROPERTY(EditDefaultsOnly, Category = "WoT") uint8 AmbientGlow = 0;
	UPROPERTY(EditDefaultsOnly, Category = "WoT") uint8 DetailLevel = 0;
	UPROPERTY(EditDefaultsOnly, Category = "WoT") uint8 GoreLevel = 0;
	UPROPERTY(EditDefaultsOnly, Category = "WoT") float VisibilityRadius = 2000.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT") float VisibilityHeight = 2000.0f;
	// > 0: Textures is an ordered burn-in series stepped every FrameTime, then
	// AmbientGlow drops to 0 and ScaleGlow fades over FadeTime (BalefireDecal).
	UPROPERTY(EditDefaultsOnly, Category = "WoT") float FrameTime = 0.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT") float FadeTime = 0.0f;
	// Decal.AttemptRegistration hands the decal to the DecalManager (the port's
	// live-count budget); BalefireDecal/MashadarCrack override it to not register.
	UPROPERTY(EditDefaultsOnly, Category = "WoT") bool bRegisters = true;
	// A decal class every machine spawns where this one lands (BalefireDecal's
	// first Tick spawns BFHole at its Location and aligns it, BalefireDecal.uc).
	UPROPERTY(EditDefaultsOnly, Category = "WoT") FName CompanionDecal;
};

struct FWoTDecalSet
{
	TMap<FName, FWoTDecalConfig> Configs;
};

struct FWoTDecalBuilder
{
	static constexpr const TCHAR* FamilyName = TEXT("decal");

	static bool Build(const FWoTRetailBuildContext& Context,
		FWoTDecalSet& Out, FString& OutError);
	static FString CanonicalDigest(const FWoTDecalSet& Set);
	static void Install(FWoTDecalSet&& Set);
	static void SerializeCanonical(const FWoTDecalSet& Set, TArray<uint8>& Out);
	static bool AreEquivalent(const FWoTDecalSet& Built,
		const FWoTDecalSet& Expected, FString& OutError);

#if WITH_AUTOMATION_TESTS
	static void Reset();
#endif
};

// Legend's Decal actor (Legend/Decal.uc): the DecalTRI quad mesh, unlit, drawn
// with the class Style, placed 1uu off the surface. Spawned at an impact
// (aligned immediately against the known normal) or thrown from a sprayer (flies
// under gravity, aligns where it lands). Align reproduces the shipped fit: flush
// back-trace, random roll, 4-corner fit loop shrinking x0.85 down to size 5 over
// 10 attempts, destroy on failure. Registered decals use DecalManager's fixed
// map-authored expiry/shrink and budget; special decals retain 180-300s. UE1 decals
// never replicated
// (RemoteRole=None) except BalefireDecal/MashadarCrack (SimulatedProxy) — the
// port spawns them locally where every machine observes the trigger (simulated
// projectile Explode, per-machine sprayers), and replicates the thin placed
// payload only for authority-observed events (chunk bounces, the balefire beam).
UCLASS()
class AWoTDecal : public AActor
{
	GENERATED_BODY()

public:
	AWoTDecal();

	// Place against a known surface normal (the impact path: Spawn + Align(HitNormal)).
	void Align(const FVector& Normal);
	// Apply a fit already validated by FindAlignedFit. This path deliberately does
	// not trace or randomize again.
	void ApplyAlignedFit(const FVector& Location, const FRotator& Rotation,
		float Scale, const FVector& Normal);

	virtual void Tick(float DeltaSeconds) override;
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UPROPERTY(Replicated) FName DecalType;
	UPROPERTY(Replicated) uint8 TextureIndex = 0;
	UPROPERTY(Replicated) float FinalScale = 1.0f;    // post-fit-shrink
	UPROPERTY(Replicated) FVector PlacedNormal = FVector::UpVector;
	// The actor spawn bunch compresses rotation. Preserve the authoritative fit's
	// full float components so simulated proxies never perform or approximate it.
	UPROPERTY(Replicated) FVector PlacedLocation = FVector::ZeroVector;
	UPROPERTY(Replicated) FVector PlacedRotationEuler = FVector::ZeroVector;
	UPROPERTY(ReplicatedUsing = OnRep_Placed) uint8 bPlaced = 0;
	UFUNCTION() void OnRep_Placed();

	// Decal.uc defaults.
	UPROPERTY(EditDefaultsOnly, Category = "WoT") float EdgeRadius = 30.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT") float MinimumSize = 5.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT") int32 MaxNumAttempts = 10;
	UPROPERTY(EditDefaultsOnly, Category = "WoT") float RetryScale = 0.85f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT") float TraceDist = 5.0f;

	// The UE1 display properties the unlit mesh colour reads (Decal.uc
	// bUnlit=True; UE1's GUnlitColor = clamp(ScaleGlow*0.5 +
	// AmbientGlow/256)). UWoTMeshLight reads these same names by reflection, so
	// its runtime bake pushes the identical flat colour.
	UPROPERTY(VisibleAnywhere, Category = "WoT") bool bUnlit = true;
	UPROPERTY(VisibleAnywhere, Category = "WoT") float ScaleGlow = 1.0f;
	UPROPERTY(VisibleAnywhere, Category = "WoT") uint8 AmbientGlow = 0;

	// Flight state for sprayer-thrown decals (local-only).
	UPROPERTY() FVector Velocity = FVector::ZeroVector;
	UPROPERTY(VisibleAnywhere) TObjectPtr<class UWoTSpriteBillboardComponent> FlyingSprite;
	bool bFlying = false;

	bool IsPlaced() const { return bPlaced != 0; }

	// Presentation probes (specs): the DecalTRI mesh, its bound decal material,
	// the current GUnlitColor, and the burn-in frame.
	UWoTVertexAnimComponent* GetDecalMesh() const { return Mesh; }
	UMaterialInstanceDynamic* GetDecalMaterial() const { return DecalMaterial; }
	float GetUnlitColor() const;
	int32 GetAnimFrame() const { return AnimFrame; }
#if WITH_AUTOMATION_TESTS
	void ExpireManagerForTest()
	{
		ManagerRemaining = 0.0f;
		TickDecalManager(0.0f);
	}
#endif

private:
	// Bind the DecalTRI mesh + the Style material at the aligned transform.
	void Project();
	// Shared by the authority (Align) and replicated clients (OnRep_Placed).
	void OnPlaced();
	void ApplyTexture(int32 Index);
	void ApplyUnlitColor();
	void TickBurnIn(float DeltaSeconds);
	void TickDecalManager(float DeltaSeconds);
	// DecalManager MaxDecals: too many registered live decals -> the oldest goes.
	void EnforceBudget();

	UPROPERTY(VisibleAnywhere, Category = "WoT") TObjectPtr<UWoTVertexAnimComponent> Mesh;
	UPROPERTY(Transient) TObjectPtr<UMaterialInstanceDynamic> DecalMaterial;

	// BalefireDecal.uc burn-in state (TextureIndex/FrameTimer/bLastFrame).
	int32 AnimFrame = 0;
	float FrameTimer = 0.0f;
	float FadeRemaining = 0.0f;
	bool bLastFrame = false;
	bool bPlacedPresentation = false;
	// BalefireDecal.uc bSpawnedHole: the companion spawns on the first Tick.
	bool bCompanionPending = false;
	float ManagerRemaining = 300.0f;
	float ManagerShrinkTime = 0.0f;
	int32 ManagerMaxDecals = 1000;
	TWeakObjectPtr<AWoTDecalManager> RegisteredManager;
};

namespace WoTDecalFX
{
	// The Decal-family registry (census + citations in WoTDecal.cpp). Null for
	// unknown names.
	const FWoTDecalConfig* Find(FName Type);

	// UE1's GUnlitColor: Clamp(ScaleGlow*0.5 + AmbientGlow/256, 0, 1).
	float UnlitColor(float InScaleGlow, uint8 InAmbientGlow);

	// Decal.Align's authoritative geometry decision without allocating a
	// presentation actor. Dedicated servers need this for gameplay whose retail
	// success gate was a decal fit (MashadarArenaSpawner): decals themselves are
	// presentation-only and are intentionally absent there.
	bool FindAlignedFit(UWorld& World, const FVector& Location,
		const FVector& Normal, float InitialScale, FVector& OutLocation,
		FRotator& OutRotation, float& OutScale, AActor* Ignore = nullptr);

	// Impact path: spawn + align immediately against the known normal. bReplicated
	// for authority-observed events (chunk bounce, balefire beam); local otherwise.
	// DrawScaleOverride > 0 replaces the class DrawScale (the caller-set
	// BurnMark.DrawScale of AngrealFireballProjectile.uc:152 / ScorchSize of
	// AngrealExpWardProjectile.uc:140). Returns null when the config is unknown or
	// the fit failed (actor destroyed).
	AWoTDecal* SpawnAligned(UWorld& World, FName Type, const FVector& Location,
	                        const FVector& Normal, bool bReplicated = false,
	                        float DrawScaleOverride = -1.0f);

	// Spawn presentation from the exact authoritative fit returned above.
	AWoTDecal* SpawnAlignedFit(UWorld& World, FName Type,
		const FVector& Location, const FRotator& Rotation, float Scale,
		const FVector& Normal, bool bReplicated = false);

	// Sprayer path (local-only): a thrown decal that flies under gravity and aligns
	// where it lands.
	AWoTDecal* SpawnFlying(UWorld& World, FName Type, const FVector& Location,
	                       const FVector& InVelocity,
	                       FRandomStream* CosmeticRandom = nullptr);
}
