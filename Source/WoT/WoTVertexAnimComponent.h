#pragma once

#include "CoreMinimal.h"
#include "Components/ExternalMorphSet.h"
#include "Components/SkeletalMeshComponent.h"
#include "WoTVertexAnimComponent.generated.h"

class UWoTPayloadSubsystem;
class UTexture;
class UMaterialInstanceDynamic;
class FWoTRuntimeVertexMeshData;
struct FWoTRuntimeVertexMeshPose;
struct FWoTMeshPayloadCPUData;
enum class EWoTRuntimeVertexMeshStyle : uint8;

/** Retail DT_Mesh blend precedence over triangle PolyFlags | Actor ExtraFlags. */
WOT_API EWoTRuntimeVertexMeshStyle WoTRuntimeEffectiveMeshStyle(
	EWoTRuntimeVertexMeshStyle SlotStyle, uint8 ActorStyle);

// One #exec MESH NOTIFY of the original (Tools/convert emits them into
// <Mesh>.meta.json): a named hook at a fraction of its sequence. Consumers look
// notifies up by Function name (TransitionToCarcassNotification drives the
// pawn->carcass swap; the sound notifies land with the sound slice).
USTRUCT(BlueprintType)
struct FWoTAnimNotify
{
	GENERATED_BODY()

	// Fraction [0,1] through the sequence at which the notify fires.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	float Fraction = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	FName Function;
};

// One entry of the original #exec MESH SEQUENCE table (Tools/convert emits
// these into <Mesh>.meta.json; leaf assets/spawners feed them in here).
USTRUCT(BlueprintType)
struct FWoTAnimSequence
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	FName Name;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	FName Group;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	int32 StartFrame = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	int32 NumFrames = 1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	float Rate = 15.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	TArray<FWoTAnimNotify> Notifies;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FWoTAnimEndSignature, FName, Sequence);

struct FWoTVertexMeshDrawState
{
	TAtomic<uint64> LastSubmittedFrame {0};
};

struct FWoTVertexMeshDiagnostics
{
	FString BoundMeshId;
	FName ProxyKind = TEXT("none");
	int32 SectionCount = 0;
	int32 MaterialCount = 0;
	FVector BoundsExtent = FVector::ZeroVector;
	bool bSubmittedDrawLastFrame = false;
};

// UE1 vertex-animation semantics on a frame-per-morph-target skeletal mesh.
// Ordinary playback activates frame i at 1-t and frame
// i+1 at t; a pose-to-sequence tween temporarily carries the complete sparse
// source pose until it reaches the target frame. Morph naming
// convention from mesh2glb.py: global frame g>=1 is morph "F<g>"; frame 0 is
// the base pose.
UCLASS(ClassGroup = (WoT), meta = (BlueprintSpawnableComponent))
class UWoTVertexAnimComponent : public USkeletalMeshComponent
{
	GENERATED_BODY()

public:
	UWoTVertexAnimComponent();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	TArray<FWoTAnimSequence> Sequences;

	/**
	 * Exact Task-26 logical object id for this component's cache-v3 animation
	 * object. Guarded builds never infer it from the mesh asset name.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT|Payload")
	FString AnimationLogicalId;

	UPROPERTY(BlueprintAssignable, Category = "WoT")
	FWoTAnimEndSignature OnAnimEnd;

	UFUNCTION(BlueprintCallable, Category = "WoT")
	void PlayAnim(FName Sequence, float Rate = 1.0f);

	UFUNCTION(BlueprintCallable, Category = "WoT")
	void LoopAnim(FName Sequence, float Rate = 1.0f);

	// Blend from the current pose into the first frame of Sequence over Time
	// seconds, then stop there (UE1 TweenAnim semantics).
	UFUNCTION(BlueprintCallable, Category = "WoT")
	void TweenAnim(FName Sequence, float Time);

	// Blend from the currently rendered pose to frame zero, then run the requested
	// one-shot. This is PlayAnim(Sequence, Rate, TweenTime) in the native UE1 API,
	// exposed separately here so the intent channel can preserve the operation.
	UFUNCTION(BlueprintCallable, Category = "WoT")
	void TweenPlayAnim(FName Sequence, float Rate, float Time);

	UFUNCTION(BlueprintPure, Category = "WoT")
	bool IsAnimating() const { return bPlaying || bTweening; }

	// The sequence currently playing/tweening (NAME_None if none). Read-only
	// query added for the net-test snapshot serializer (Source/WoT/Testing);
	// behavior-free.
	UFUNCTION(BlueprintPure, Category = "WoT")
	FName GetCurrentSequence() const { return (bPlaying || bTweening) ? Current : NAME_None; }

	// UE1 AnimFrame equivalent: 0..1 fraction through the sequence.
	UFUNCTION(BlueprintPure, Category = "WoT")
	float GetAnimFrame() const;

	// Read-only sequence lookup (null when unknown) — notify/duration consumers.
	const FWoTAnimSequence* FindSequence(FName Sequence) const { return Find(Sequence); }

	// The fraction of Sequence at which the named notify fires, or -1 when the
	// sequence/notify isn't present (first match; shipped meshes author one
	// TransitionToCarcassNotification per death sequence).
	float FindNotifyFraction(FName Sequence, FName Function) const;

	// Freeze on an exact frame of Sequence and stop playing (the carcass death-pose
	// copy: UE1 CopyDisplaySettings carried AnimSequence+AnimFrame onto the corpse).
	// Fraction is the UE1 AnimFrame 0..1; no anim-intent stamp (presentation-local).
	void SetFrameAndStop(FName Sequence, float Fraction);

	// Component-space LOD-0 positions for the pose currently published by the
	// frame-morph player. The cache is built once when the mesh binds; this call
	// only combines the cached base with the active frame weights. Retail's
	// bParticles renderer used these CURRENT vertices. bRandomFrame is deliberately
	// absent: MashadarSegment/MashadarTip never author it.
	bool GetCurrentPoseVertexPositions(TArray<FVector>& OutPositions) const;
	// Opt in before mesh assignment when a caller will sample every tick. A
	// one-off GetCurrentPoseVertexPositions call also requests the cache lazily.
	void RequestCurrentPoseCPUAccess();

	/** UE1 actor flag: select the fixed two-level rounded presentation variant. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT|Mesh")
	bool bMeshCurvy = false;

	/** UE1 mesh distortion byte: 128 is neutral; each step is 1/16 world unit. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WoT|Mesh")
	uint8 Fatness = 128;

	UFUNCTION(BlueprintCallable, Category = "WoT|Mesh")
	void SetFatness(uint8 InFatness);

	/** UE1 Actor.Style byte applied as mesh ExtraFlags. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT|Mesh")
	uint8 Style = 1;

	/** Bind an authenticated cache-v3 vertex mesh in guarded runtime builds. */
	bool SetVertexMesh(FName MeshName, FName PackageHint = NAME_None);
	bool SetVertexMeshPath(const FSoftObjectPath& MeshPath,
		FName PackageHint = NAME_None);
	bool SetVertexMeshLogicalIds(FStringView MeshLogicalId,
		FStringView AnimationLogicalId);
	bool HasDisplayMesh() const;
	/**
	 * Complete a runtime-mesh late bind after a deferred actor is exposed.
	 * UWorld's deferred-construction path can register this component before its
	 * authenticated mesh is available, so no primitive render state is created.
	 */
	void EnsureRuntimeRenderState();
	FName GetDisplayMeshName() const;
	const FString& GetVertexMeshLogicalId() const { return VertexMeshLogicalId; }
	const FString& GetVertexAnimationLogicalId() const { return AnimationLogicalId; }
	FWoTVertexMeshDiagnostics GetMeshDiagnostics() const;
	/** UE1 Actor.Skin fallback, distinct from the indexed MultiSkins array. */
	void SetActorSkin(const FSoftObjectPath& TexturePath);
	const FSoftObjectPath& GetRuntimeActorSkin() const
	{
		return RuntimeActorSkin;
	}
	/** UE1 MultiSkins index: replaces exactly one material slot. */
	bool SetSkinOverride(int32 SlotIndex, const FSoftObjectPath& TexturePath);
	bool SetAllSkinOverrides(const FSoftObjectPath& TexturePath);
	void ClearSkinOverrides();
	const TMap<int32, FSoftObjectPath>& GetRuntimeSkinOverrides() const
	{
		return RuntimeSkinOverrides;
	}
	void SetRuntimeSkinOverrides(
		const TMap<int32, FSoftObjectPath>& SkinOverrides);
	void CopyDisplayMeshFrom(const UWoTVertexAnimComponent* Source);

	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
	                           FActorComponentTickFunction* ThisTickFunction) override;

	// Binding an imported mesh swaps in its cache-v3 #exec MESH SEQUENCE set;
	// a CDO-bound mesh feeds it at registration.
	virtual void SetSkeletalMesh(USkeletalMesh* NewMesh, bool bReinitPose = true) override;
	virtual void OnRegister() override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void BeginDestroy() override;
	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual void SendRenderDynamicData_Concurrent() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
	virtual int32 GetNumMaterials() const override;
	virtual UMaterialInterface* GetMaterial(int32 ElementIndex) const override;
	virtual UMaterialInterface* GetMaterialByName(FName MaterialSlotName) const override;
	virtual void SetMaterial(int32 ElementIndex, UMaterialInterface* Material) override;
	virtual void SetMaterialByName(FName MaterialSlotName,
		UMaterialInterface* Material) override;
	virtual int32 GetMaterialIndex(FName MaterialSlotName) const override;
	virtual bool IsMaterialSlotNameValid(FName MaterialSlotName) const override;
	virtual TArray<FName> GetMaterialSlotNames() const override;
	virtual void GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials,
		bool bGetDebugMaterials = false) const override;
	virtual void GetPrimitiveStats(FPrimitiveStats& PrimitiveStats) const override;

	bool IsExternalMorphInjectionActive() const
	{
		return bExternalMorphInjectionActive;
	}

#if WITH_AUTOMATION_TESTS
	float GetPlayRateForTesting() const { return PlayRate; }
	void SetPoseCacheForTesting(
		TConstArrayView<FVector3f> BasePositions,
		const TMap<int32, TArray<FVector3f>>& FrameDeltas);
	void SetFrameMorphsForTesting(
		int32 GlobalA, float WeightA, int32 GlobalB, float WeightB)
	{
		SetFrameMorphs(GlobalA, WeightA, GlobalB, WeightB);
	}
	bool BindRuntimeVertexMeshForTesting(
		TSharedPtr<const FWoTRuntimeVertexMeshData, ESPMode::ThreadSafe> Data)
	{
		return BindRuntimeVertexMesh(SelectRuntimeVertexMesh(MoveTemp(Data)));
	}
	bool InstallHeadlessExternalMorphSetForTesting(int32 FrameCount)
	{
		TSharedRef<FExternalMorphSet> Set = MakeShared<FExternalMorphSet>();
		Set->Name = TEXT("RuntimeVertexMeshParity");
		return InstallExternalMorphSet(Set, FrameCount);
	}
	bool HasSceneProxyForTesting() const { return SceneProxy != nullptr; }
	const FWoTRuntimeVertexMeshPose* GetRuntimePoseForTesting() const
	{
		return RuntimePoseStorage;
	}
	bool IsRuntimeMeshCurvyForTesting() const;
	void DestroyRenderStateForTesting()
	{
		if (IsRenderStateCreated()) DestroyRenderState_Concurrent();
	}
#endif

protected:
	virtual void OnUpdateTransform(EUpdateTransformFlags UpdateTransformFlags,
		ETeleportType Teleport = ETeleportType::None) override;
	virtual void OnComponentCollisionSettingsChanged(
		bool bUpdateOverlaps = true) override;

private:
	const FWoTAnimSequence* Find(FName Sequence) const;
	void StartSequence(FName Sequence, float Rate, bool bInLoop);
	void StartTween(FName Sequence, float Time, bool bPlayAfter, float Rate);
	// Publish this playback as the owning AWoTCharacter's replicated anim intent when
	// driven on authority — the parity seam so any driver reaches simulated proxies.
	void StampOwnerAnimIntent(FName Sequence, float Rate, bool bLoopIntent);
	void ApplyFrame();
	void SetFrameMorphs(int32 GlobalA, float WeightA, int32 GlobalB, float WeightB);
	// The single read/write seam for ALL frame morph access. An unset value is a
	// read; a set value is a write. Native F<n> targets remain the OFF path and
	// the registered external-set weight array is the ON path.
	float AccessFrameMorph(int32 GlobalFrame, TOptional<float> NewWeight);
	bool InstallExternalMorphSet(TSharedPtr<FExternalMorphSet> MorphSet,
		int32 FrameCount);
	bool InstallPayloadPoseCache(const FWoTMeshPayloadCPUData& CPUData);
	void ClearExternalMorphInjection();
	void TryBindPayloadMesh();
	void CacheCurrentMeshPoseData();
	void DeduplicatePoseCache();
	void ClearPoseGeometryCache();
	void ClearPoseCache();
	void SetCurrentPoseWeights(
		int32 GlobalA, float WeightA, int32 GlobalB, float WeightB);
	bool BindRuntimeVertexMesh(
		TSharedPtr<const FWoTRuntimeVertexMeshData, ESPMode::ThreadSafe> Data);
	void RebakeOwnerMeshLightAfterLateBind();
	TSharedPtr<const FWoTRuntimeVertexMeshData, ESPMode::ThreadSafe>
		SelectRuntimeVertexMesh(
			TSharedPtr<const FWoTRuntimeVertexMeshData, ESPMode::ThreadSafe> Data) const;
	void ClearRuntimeVertexMesh();
	void PublishRuntimePose();
	FVector FatnessLocalScale() const;
	void BuildRuntimeMaterials();
	void RefreshRuntimeMaterials();
	bool ApplyRuntimeSkinOverride(int32 SlotIndex,
		const FSoftObjectPath& TexturePath);

	FName Current;
	float LocalFrame = 0.0f;   // 0..NumFrames, fractional
	float PlayRate = 1.0f;
	bool bLoop = false;
	bool bPlaying = false;

	// tween state: blend base->target-first-frame
	bool bTweening = false;
	float TweenTotal = 0.0f;
	float TweenElapsed = 0.0f;
	// Sparse morph weights of the complete pose captured at the tween edge. A
	// second animation may interrupt an in-progress crossfade, so this cannot be
	// limited to the ordinary two adjacent playback frames.
	TMap<int32, float> TweenSourceWeights;
	bool bPlayAfterTween = false;
	float TweenPlayRate = 1.0f;

	TArray<FName, TInlineAllocator<4>> ActiveMorphs;

	// CPU copy used by GetCurrentPoseVertexPositions. Frame keys are the global
	// F<n> indices used by SetFrameMorphs; each delta array is dense in LOD-0
	// render-vertex order. Native meshes fill it from UMorphTarget's CPU delta
	// iterator. Stripped builds fill it from the payload decoder before the same
	// deltas are compressed for the external GPU morph set.
	TWeakObjectPtr<USkeletalMesh> PoseCacheMesh;
	TArray<FVector3f> PoseBasePositions;
	TMap<int32, TArray<FVector3f>> PoseFrameDeltas;
	TMap<int32, float> CurrentPoseWeights;
	int32 CurrentPoseGlobalA = INDEX_NONE;
	int32 CurrentPoseGlobalB = INDEX_NONE;
	float CurrentPoseWeightA = 0.0f;
	float CurrentPoseWeightB = 0.0f;
	bool bCurrentPoseCPUAccessRequested = false;
	bool bPayloadPoseCacheInstalled = false;
	bool bPoseCacheDeduplicated = false;

	TSharedPtr<const FWoTRuntimeVertexMeshData, ESPMode::ThreadSafe>
		RuntimeVertexMesh;
	TSharedPtr<FWoTVertexMeshDrawState, ESPMode::ThreadSafe> RuntimeDrawState;
	FWoTRuntimeVertexMeshPose* RuntimePoseStorage = nullptr;
	FVector LastPublishedFatnessScale = FVector::OneVector;
	FString VertexMeshLogicalId;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInterface>> RuntimeDefaultMaterials;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> RuntimeMaterialInstances;
	UPROPERTY(Transient)
	TMap<int32, FSoftObjectPath> RuntimeSkinOverrides;
	FSoftObjectPath RuntimeActorSkin;

	static constexpr int32 ExternalMorphSetID = 0x574f544d; // 'WOTM'
	bool bExternalMorphInjectionActive = false;
	int32 ExternalMorphFrameCount = 0;
#if WITH_AUTOMATION_TESTS
	// -nullrhi cannot create a skeletal LOD/external GPU set. This is the exact
	// engine external-weight type used only to exercise routing in headless specs.
	FExternalMorphSetWeights HeadlessExternalMorphWeights;
#endif

	friend class UWoTPayloadSubsystem;
};
