#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "WoTFractalData.h"
#include "WoTFractalSim.h"
#include "Payload/WoTRuntimeProceduralResource.h"
#include "WoTFractalSubsystem.generated.h"

class AActor;
class ULevel;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UPrimitiveComponent;
class UTexture2D;

/** Current-world owner for exact live Fire/Wet/Wave/Ice simulations. */
UCLASS()
class WOT_API UWoTFractalSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickable() const override;

	bool ActivateRecord(const FWoTFractalTextureRecord& Record);
	/** Direct bind for runtime (cache-authenticated) consumers: rebinds the
	    component's slot to the live texture of the record whose "Pkg__Tex"
	    identity key matches. False when no such record is registered. */
	bool BindRuntimeProceduralSlot(UPrimitiveComponent* Component, int32 Slot,
		FName IdentityKey);
	/** Name prefix of the runtime placeholder textures the guarded materializers
	    bind before this subsystem activates: "WoTProc_<Pkg>__<Tex>". The name is
	    the discovery key exactly as a cooked /Game/WoT/Textures path is in
	    development worlds. */
	static const TCHAR* RuntimePlaceholderPrefix() { return TEXT("WoTProc_"); }
	static FName RuntimePlaceholderName(FName IdentityKey);
	int32 GetActiveTextureCount() const;
	int32 GetRegisteredRecordCount() const { return RecordsByIdentity.Num(); }
	UTexture2D* FindLiveTexture(FName QualifiedName) const;
	int32 GetStepCount(FName QualifiedName) const;
	bool IsFrozen() const { return bFrozen; }
	// Sheet-scrub retirement: every fractal-bearing master samples Tex at
	// identity UV — the one remaining root-coverage question is membership here.
	static bool UsesIdentityUVRoot(FName RootName);

#if WITH_DEV_AUTOMATION_TESTS
	/** Headless specs have no renderer, so no consumer ever reports a render
	    time; this lets them exercise the live-advance path past the gate. */
	void SetRenderGateBypassForTesting(const bool bBypass) { bRenderGateBypass = bBypass; }
	bool GetIcePositionForTesting(FName QualifiedName, float& OutMasterCount,
		float& OutUPosition, float& OutVPosition) const;
	bool BindMaterialSlotForTesting(UPrimitiveComponent* Component, int32 Slot,
		const FWoTFractalTextureRecord& Record);
	void RegisterRecordForTesting(const FWoTFractalTextureRecord& Record)
	{
		RegisterRecordMaps(Record);
	}
	/** Runs the guarded activation (authenticated cache-v3 procedural objects
	    -> records -> placeholder discovery) in any build with a ready world. */
	void ActivateGuardedCatalogForTesting() { ActivateGuardedCatalog(); }
	const FWoTFractalTextureRecord* FindRecordForMaterialForTesting(
		UMaterialInterface* Material) const
	{
		return FindRecordForMaterial(Material);
	}
#endif

	/** "Pkg__Tex" identity key ('-' folded to '_', case-insensitive via FName) —
	    the same spelling the importers use for material/texture asset names. */
	static FName MakeFractalIdentityKey(const FName PackageName, const FName TextureName);
	/** A record's identity key: MakeFractalIdentityKey for a name unique in its
	    package; for a guarded record whose authenticated identity is the exact
	    "Pkg.Group__Name" of a colliding name (WarrenT.Effects.Rain3 vs the
	    top-level WarrenT.Rain3), that base in object-name-safe spelling so the
	    two records never alias (WoTRetailIdentity.h). */
	static FName RecordIdentityKey(const FWoTFractalTextureRecord& Record);

private:
	struct FSourceBinding
	{
		FName Key;
		int32 Slot = 0;
	};

	struct FActiveState
	{
		TUniquePtr<IWoTFractalSim> Sim;
		UTexture2D* Texture = nullptr;
		TArray<FSourceBinding> Sources;
		double LastUpdateTime = 0.0;
		bool bDirectlyUsed = false;
	};

	bool ActivateRecordInternal(const FWoTFractalTextureRecord& Record,
		bool bDirectlyUsed, TSet<FName>& ActivationStack);
	void PrimeLiveState(FActiveState& State);
	int32 UpdateStateAtTime(FName Key, double TargetTime,
		TSet<FName>& UpdateStack);
	void PoseState(FName Key, TSet<FName>& Posed, TSet<FName>& PoseStack);
	void ApplyFrozenPose();
	void DiscoverAndBind();
	/** Guarded worlds: materialize every authenticated cache-v3 procedural
	    object into a runtime record, register it, and bind the placeholder-
	    textured consumers the materializers left behind. */
	void ActivateGuardedCatalog();
	void HandleGuardedWorldReadinessChanged(bool bReady);
	void RegisterConsumerHooks(UWorld& InWorld);
	void BindActor(AActor* Actor, int32& BoundCount);
	bool BindMaterialSlot(UPrimitiveComponent* Component, int32 Slot,
		UMaterialInterface* Original, const FWoTFractalTextureRecord& Record,
		int32& BoundCount);
	void HandleActorSpawned(AActor* Actor);
	void HandleLevelAddedToWorld(ULevel* Level, UWorld* OwningWorld);
	void FlushPendingBinds();
	bool IsRecordRenderedRecently(FName Key, float WorldNow);
	UMaterialInstanceDynamic* MakeLiveMaterial(
		UMaterialInterface* Original, UTexture2D* LiveTexture);
	void UploadState(FActiveState& State, bool bInitializeResource = false);
	const FWoTFractalTextureRecord* FindRecordForMaterial(
		UMaterialInterface* Material) const;
	/** Registers a record in the object-path + identity (+ unique-bare-name)
	    lookup maps; shared by the catalog scan and the test hook. */
	void RegisterRecordMaps(const FWoTFractalTextureRecord& Record);
	/** Identity from a bound texture's asset path: the standalone
	    /Game/WoT/Textures/<Pkg>/<Name> form or the (transitional) flipbook
	    sheet /Game/WoT/Textures/Flipbooks/<Pkg>/<Name>_sheet form. */
	const FWoTFractalTextureRecord* FindRecordByTexturePath(
		const FString& TexturePath) const;
	/** Identity from an importer-authored material name (MI_W_<Pkg>__<Tex>,
	    MI_PSAddFB_<Pkg>__<Tex>, ... — qualified) or a unique bare MI_<Tex>. */
	const FWoTFractalTextureRecord* FindRecordByMaterialName(
		const FName MaterialName) const;
	/** Returns the record whose seed arrays are populated: the input itself
	    when the catalog embeds them (canonical build), else a cached copy
	    rehydrated from the payload cache (stripped build). Null when a needed
	    seed can neither be found embedded nor rehydrated. */
	const FWoTFractalTextureRecord* ResolveSeededRecord(
		const FWoTFractalTextureRecord& Record);

	UPROPERTY(Transient)
	TObjectPtr<UWoTFractalCatalog> Catalog;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UTexture2D>> OwnedTextures;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> OwnedMaterials;

	TMap<FName, FActiveState> ActiveStates;
	TArray<FName> ActiveOrder;
	TMap<UMaterialInterface*, UMaterialInstanceDynamic*> MaterialReplacements;
	/** "Pkg__Tex" identity → record (the stable join; sheet assets are dying —
	    PORT_NOTES "Pregenerated flipbook excision", phase 2). */
	TMap<FName, const FWoTFractalTextureRecord*> RecordsByIdentity;
	/** Bare texture name → record, UNIQUE names only (colliding bare names —
	    Flames02-09, Fire_Torch_001-003, WetPart01 — are removed; qualified
	    lookups must win for those). */
	TMap<FName, const FWoTFractalTextureRecord*> RecordsByBareName;
	TMap<FName, const FWoTFractalTextureRecord*> RecordsByObjectPath;
	/** Stripped-build seed copies rehydrated from the payload cache, keyed by
	    QualifiedName. Owned here so record pointers stay stable. */
	TMap<FName, TUniquePtr<FWoTFractalTextureRecord>> RehydratedRecords;
	/** Guarded cache-v3 objects retained after strict first-use validation.
	    Their current v1 schema does not embed the palette bytes needed to
	    materialize a complete FWoTFractalTextureRecord. */
	TArray<TUniquePtr<FWoTRuntimeProceduralResource>>
		VerifiedProceduralResources;
	/** Guarded records built from the v2 objects (owned; pointers stable). */
	TArray<TUniquePtr<FWoTFractalTextureRecord>> GuardedRecords;
	FDelegateHandle GuardedReadinessHandle;
	bool bGuardedCatalogActive = false;
	/** Actors queued by the spawn/level-added hooks: gameplay code assigns
	    materials AFTER SpawnActor returns (WoTFireWall sprayers), so binding
	    is deferred to the next Tick. */
	TArray<TWeakObjectPtr<AActor>> PendingBindActors;
	/** Live consumers per record key — the render-recency gate's evidence
	    that some surface still locks this texture, as in retail. */
	TMap<FName, TArray<TWeakObjectPtr<UPrimitiveComponent>>> BoundConsumers;
	TSet<FName> WarnedRoots;
	FDelegateHandle ActorSpawnedHandle;
	FDelegateHandle LevelAddedHandle;
	double SimulationTime = 0.0;
	uint64 ActivationSerial = 0;
	bool bFrozen = false;
	bool bRenderGateBypass = false;
};
