#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "WoTMeshLight.generated.h"

class AActor;
class UWoTMapLightTable;
struct FWoTLightRec;

// One converted dynamic-mover material section. movers2glb guarantees that a
// slot contains exactly one oriented retail BSP plane; the importer stamps the
// local plane and its base-pose selection, while runtime reselects from the
// mover origin's current leaf on the shared M2 movement cadence.
USTRUCT(BlueprintType)
struct FWoTMoverSurfaceLight
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	FName MaterialSlot;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	FVector LocalOrigin = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	FVector LocalNormal = FVector::UpVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	TArray<FVector4> LightPosR;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	TArray<FLinearColor> LightColor;

	// Parallel static MID selectors. INDEX_NONE means the corresponding
	// waveform-mean LightColor remains steady.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	TArray<int32> LightWaveGroup;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	TArray<FVector4> LightDirCone;
};

// UE1 actor and mover lighting, display-space (gamma-space scheme). Ordinary
// meshes retain the actor-wide SetupForActor approximation; dynamic movers use
// the per-planar-section SetupMoverSurface rows below. BeginPlay pushes them
// into MIDs on every sibling mesh component. The actor material
// (M_WoT_MeshLit family) evaluates the exact vertex formula:
// G = max(0,(1+cos)^2-1.5) + view-space specular,
// linear radial falloff, min(ScaleGlow*1.4*Sum + Ambient, 1), modulating
// the raw display-space skin. Both placed and runtime bakes use the table's
// display-ready waveform-mean colors; runtime actors reselect as they move.
UCLASS(ClassGroup = (WoT), meta = (BlueprintSpawnableComponent))
class UWoTMeshLight : public UActorComponent
{
	GENERATED_BODY()

public:
	// The measured retail maximum is 17 across the 19 MP maps. The material
	// contract carries three guard slots; overflows are reported before the
	// presentation-only truncation and are never silent.
	static constexpr int32 MaxMoverSurfaceLights = 20;
	// Called by UWoTVertexAnimComponent at BeginPlay. A missing per-map table is
	// completely inert: no component is created and no material is touched.
	static UWoTMeshLight* AttachRuntime(AActor* Owner,
		UWoTMapLightTable* TableOverride = nullptr);
	// Public pure lookup seam for synthetic table specs.
	static FLinearColor ZoneAmbientAt(const UWoTMapLightTable* Table,
		const FVector& Point);
	// Shared UE1 UModel::PointRegion classifier. False means the converted BSP
	// payload is absent or malformed; outside-world callers deliberately treat
	// that bounded adaptation as "inside".
	static bool ClassifyRegion(const UWoTMapLightTable* Table,
		const FVector& Point, int32& OutZone, int32& OutLeaf);
	static int32 LeafAt(const UWoTMapLightTable* Table, const FVector& Point);
	static bool BspLineCheck(const UWoTMapLightTable* Table,
		const FVector& Start, const FVector& End);
	// Retail zero-extent UModel::LineCheck Hit.Item: first blocking world BSP
	// node along Start->End, or INDEX_NONE when clear/invalid. This is the
	// TraceTexture seam used by walk-surface friction and climbing.
	static int32 FirstHitNode(const UWoTMapLightTable* Table,
		const FVector& Start, const FVector& End);
	// Sparse Texture.Friction lookup. Invalid/unlisted nodes use UE1 Texture's
	// default 1.0.
	static float FrictionForNode(const UWoTMapLightTable* Table, int32 NodeIndex);
	// Shared per-map table resolver for runtime systems beside mesh lighting.
	static UWoTMapLightTable* FindMapLightTable(const UWorld* World);
	// Runtime maps have no cooked LT_<Map> asset. Register the generation-owned
	// transient table before projected actors are exposed to BeginPlay.
	static void RegisterRuntimeMapLightTable(
		const UWorld* World, UWoTMapLightTable* Table);
#if WITH_AUTOMATION_TESTS
	static void ResetMapLightTableCacheForTesting();
	static int32 MapLightTableLoadAttemptsForTesting();
#endif
	static int32 LightingTagAt(const FWoTLightRec& Light,
		const FVector& Location);
	// Visibility is injected only at this pure boundary so synthetic specs can
	// pin retail filtering independently; production passes BspLineCheck.
	static TArray<int32> SelectLeafLights(const UWoTMapLightTable* Table,
		const FVector& Location, int32 LeafIndex, int32 MaxLights,
		bool bSpecialLit, TFunctionRef<bool(int32)> IsVisible);
	// Retail SetupMoverSurface (UE1 light manager). Shipped permeating order;
	// plane-facing + bSpecialLit are the only consider gates. Deliberately no
	// radius, LightingTag/priority, LineCheck, or Min/MaxLightCount cap.
	static TArray<int32> SelectMoverSurfaceLights(
		const UWoTMapLightTable* Table, int32 LeafIndex,
		const FVector& SurfaceOrigin, const FVector& SurfaceNormal,
		bool bSpecialLit);

	// Synthetic-table seam used by headless specs; also starts the movement timer.
	void StartRuntimeBake(UWoTMapLightTable* Table);
	void ForceRuntimeBake();
	void NotifyOwnerTeleported();

	// xyz = world position (UE5 units), w = 1/WorldLightRadius (UE5 units).
	// Stationary proxy selection is capped by the actor zone's MaxLightCount
	// (1..16; default/shipped MP max is 6). Retail's live cap instead comes from
	// the viewer zone and frame poly count. The material has six authored slots.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	TArray<FVector4> LightPosR;

	// FloatColor at waveform mean: FGetHSV(hue,sat,255)*(Brightness/255)
	// *Level.Brightness — display-space 0..1.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	TArray<FLinearColor> LightColor;

	// Parallel to LightColor. Each entry selects Wave0..Wave3; INDEX_NONE is
	// steady. Runtime selection rebakes this alongside the other light arrays.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	TArray<int32> LightWaveGroup;

	// Per-light spotlight data, parallel to LightPosR: xyz = unit cone direction
	// (world), w = Sine = 1 - LightCone/256. w < 0 marks a non-spotlight (omni).
	// Consumed only by the surface master (M_WoT_MoverLit); the mesh master
	// ignores MLDir. Empty/absent entries default to omni.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	TArray<FVector4> LightDirCone;

	// Map group constants stamped once on every MID. The live MPC changes only
	// Wave0..Wave3; no per-frame MID updates are required.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	FVector4 WaveMeans = FVector4(1.0, 1.0, 1.0, 1.0);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	FVector4 WaveAmplitudes = FVector4(0.0, 0.0, 0.0, 0.0);

	// Dynamic movers replace the actor-wide arrays above with one selection per
	// planar material section. Empty for the unchanged actor/non-dynamic-mover
	// path, which retains the six-slot M_WoT_MeshLit contract.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	TArray<FWoTMoverSurfaceLight> MoverSurfaces;

	// Static BSP movers still use SetupMoverSurface's zone ambient (x0.5),
	// rather than ordinary actor/Gouraud AmbientGlow lighting. Their light list
	// remains actor-wide because no movement-time plane reselection is needed.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	bool bStaticMoverSurfaceLighting = false;

	// AmbientGlow/255 + zone FGetHSV ambient; for bUnlit actors the whole
	// flat GUnlitColor is baked here and LightPosR is empty.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	FLinearColor Ambient = FLinearColor::Black;

	// UE1 mesh diffuse light comes from the actor's ScaleGlow.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	float ScaleGlow = 1.0f;

	// Set by the importer when the bake ran; BeginPlay is a no-op otherwise
	// so the component is inert on maps still using the linear scheme.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	bool bBaked = false;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	void BakeRuntime();
	void CheckRuntimeMovement();
	void PushMaterialParameters(bool bReparent);
	void ArmRuntimeTimer();

	UPROPERTY(Transient)
	TObjectPtr<UWoTMapLightTable> RuntimeTable;

	FTimerHandle RuntimeBakeTimer;
	FVector LastBakeLocation = FVector::ZeroVector;
	FRotator LastBakeRotation = FRotator::ZeroRotator;
	int32 LastBakeZone = INDEX_NONE;
	int32 LastBakeLeaf = INDEX_NONE;
	bool bRuntime = false;
	bool bPostSpawnRebakeDone = false;
};
