#include "WoTFractalSubsystem.h"
#include "Assets/WoTRetailIdentity.h"
#include "WoTPresentationPolicy.h"
#include "Payload/WoTPayloadActivation.h"
#include "Payload/WoTRuntimeProceduralResource.h"
#include "Payload/WoTRuntimeRegistry.h"

#include "WoTFractalSim.h"
#include "WoTPayloadSubsystem.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/Level.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialParameters.h"
#include "TextureResource.h"
#include "UObject/UObjectGlobals.h"

static TAutoConsoleVariable<int32> CVarWoTFractalFreeze(
	TEXT("wot.FractalFreeze"), 0,
	TEXT("Reset live fractal textures, advance a fixed 32-step pose, and hold."),
	ECVF_Default);

namespace
{
constexpr int32 FrozenPoseSteps = 32;
constexpr double TextureConstructionEpsilon = 0.000001;
#if !WOT_GUARDED_BOOTSTRAP
const TCHAR* CatalogObjectPath =
	TEXT("/Game/WoT/Data/DA_WoTFractalCatalog.DA_WoTFractalCatalog");
#endif
FWoTFractalRandomStream GlobalFractalRandomStream;
bool bGlobalFractalRandomInitialized = false;

bool FractalWorldReady()
{
#if WOT_GUARDED_BOOTSTRAP
	return IsWoTBootstrapWorldReady();
#else
	// The editor catalog is the development authority and does not run through
	// payload bootstrap. Guarded worlds retain the authenticated readiness gate.
	return true;
#endif
}

bool IsRuntimeFractalClass(const EWoTFractalClass FractalClass)
{
	return FractalClass == EWoTFractalClass::Fire
		|| FractalClass == EWoTFractalClass::Wet
		|| FractalClass == EWoTFractalClass::Wave
		|| FractalClass == EWoTFractalClass::Ice;
}

TUniquePtr<IWoTFractalSim> MakeFractalSim(
	const EWoTFractalClass FractalClass)
{
	switch (FractalClass)
	{
	case EWoTFractalClass::Fire:
		return MakeUnique<FWoTFireFractalSim>();
	case EWoTFractalClass::Wet:
	case EWoTFractalClass::Wave:
		return MakeUnique<FWoTWaterFractalSim>();
	case EWoTFractalClass::Ice:
		return MakeUnique<FWoTIceFractalSim>();
	default:
		return nullptr;
	}
}

void PaletteMapBGRA(const IWoTFractalSim& Sim, TArray<uint8>& OutBGRA)
{
	const FWoTFractalTextureRecord& Definition = Sim.GetDefinition();
	const TArray<uint8>& Indices = Sim.GetOutputIndices();
	OutBGRA.SetNumUninitialized(Indices.Num() * 4);
	for (int32 Pixel = 0; Pixel < Indices.Num(); ++Pixel)
	{
		const int32 PaletteOffset = static_cast<int32>(Indices[Pixel]) * 4;
		const int32 Dest = Pixel * 4;
		OutBGRA[Dest + 0] = Definition.PaletteRGBA[PaletteOffset + 2];
		OutBGRA[Dest + 1] = Definition.PaletteRGBA[PaletteOffset + 1];
		OutBGRA[Dest + 2] = Definition.PaletteRGBA[PaletteOffset + 0];
		OutBGRA[Dest + 3] = Definition.PaletteRGBA[PaletteOffset + 3];
	}
}
}

void UWoTFractalSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	// A dedicated server never samples a texture: no catalog, no simulation.
	if (!WoTPresentationAnimationEnabled(&InWorld))
	{
		return;
	}
#if WOT_GUARDED_BOOTSTRAP
	// The authenticated world receipt is published at engine exposure; a
	// world that begins play before it waits for the readiness event.
	if (!IsWoTBootstrapWorldReady())
	{
		GuardedReadinessHandle = RegisterWoTBootstrapWorldReadinessListener(
			FWoTBootstrapWorldReadinessEvent::FDelegate::CreateUObject(
				this, &UWoTFractalSubsystem::HandleGuardedWorldReadinessChanged));
		return;
	}
#endif
	if (!FractalWorldReady())
	{
		return;
	}
	SimulationTime = 0.0;
	ActivationSerial = 0;
	if (!bGlobalFractalRandomInitialized)
	{
		GlobalFractalRandomStream.Initialize(
			static_cast<uint32>(FPlatformTime::Cycles64()));
		bGlobalFractalRandomInitialized = true;
	}
	RecordsByIdentity.Reset();
	RecordsByBareName.Reset();
	RecordsByObjectPath.Reset();
	RehydratedRecords.Reset();
	VerifiedProceduralResources.Reset();
	GuardedRecords.Reset();
#if WOT_GUARDED_BOOTSTRAP
	ActivateGuardedCatalog();
#else
	Catalog = LoadObject<UWoTFractalCatalog>(nullptr, CatalogObjectPath);
	if (!Catalog)
	{
		UE_LOG(LogTemp, Verbose, TEXT("WoT fractal catalog is not imported; flipbooks remain active"));
		return;
	}
	for (const FWoTFractalTextureRecord& Record : Catalog->Textures)
	{
		if (IsRuntimeFractalClass(Record.FractalClass))
		{
			RegisterRecordMaps(Record);
		}
	}
	DiscoverAndBind();
	RegisterConsumerHooks(InWorld);
#endif
}

void UWoTFractalSubsystem::RegisterConsumerHooks(UWorld& InWorld)
{
	// Consumers created after the discovery scan (FireWall sprayers,
	// projectile impact effects, streamed levels) bind through these hooks on
	// the next Tick (adversarial-review finding 2).
	if (!ActorSpawnedHandle.IsValid())
	{
		ActorSpawnedHandle = InWorld.AddOnActorSpawnedHandler(
			FOnActorSpawned::FDelegate::CreateUObject(
				this, &UWoTFractalSubsystem::HandleActorSpawned));
	}
	if (!LevelAddedHandle.IsValid())
	{
		LevelAddedHandle = FWorldDelegates::LevelAddedToWorld.AddUObject(
			this, &UWoTFractalSubsystem::HandleLevelAddedToWorld);
	}
}

void UWoTFractalSubsystem::HandleGuardedWorldReadinessChanged(const bool bReady)
{
	if (!bReady || bGuardedCatalogActive || !IsWoTBootstrapWorldReady())
	{
		return;
	}
	UnregisterWoTBootstrapWorldReadinessListener(GuardedReadinessHandle);
	SimulationTime = 0.0;
	ActivationSerial = 0;
	if (!bGlobalFractalRandomInitialized)
	{
		GlobalFractalRandomStream.Initialize(
			static_cast<uint32>(FPlatformTime::Cycles64()));
		bGlobalFractalRandomInitialized = true;
	}
	ActivateGuardedCatalog();
}

void UWoTFractalSubsystem::ActivateGuardedCatalog()
{
	UWorld* World = GetWorld();
	if (!World || bGuardedCatalogActive || !IsWoTBootstrapWorldReady()
		|| !WoTPresentationAnimationEnabled(World))
	{
		return;
	}
	// Every authenticated cache-v3 procedural object (ue1-fractal-inputs-v2)
	// carries its palette bytes and simulation parameters; a strict-parse
	// failure of any object keeps the whole catalog dormant (fail closed).
	TArray<TUniquePtr<FWoTFractalTextureRecord>> Records;
	TArray<TUniquePtr<FWoTRuntimeProceduralResource>> Resources;
	for (const FWoTNeutralResourceId& Resource
		: FWoTRuntimeRegistry::Get().ListResources(
			EWoTNeutralResourceKind::Procedural))
	{
		TUniquePtr<FWoTRuntimeProceduralResource> Parsed =
			MakeUnique<FWoTRuntimeProceduralResource>();
		if (Parsed->LoadFirstUse(Resource).IsError())
		{
			UE_LOG(LogTemp, Error,
				TEXT("A verified procedural cache object failed strict parsing: %s"),
				*Resource.LogicalId);
			return;
		}
		TUniquePtr<FWoTFractalTextureRecord> Record =
			MakeUnique<FWoTFractalTextureRecord>();
		if (!Parsed->BuildRecord(*Record))
		{
			UE_LOG(LogTemp, Error,
				TEXT("A verified procedural cache object has no runtime record: %s"),
				*Resource.LogicalId);
			return;
		}
		Records.Add(MoveTemp(Record));
		Resources.Add(MoveTemp(Parsed));
	}
	bGuardedCatalogActive = true;
	GuardedRecords = MoveTemp(Records);
	VerifiedProceduralResources = MoveTemp(Resources);
	for (const TUniquePtr<FWoTFractalTextureRecord>& Record : GuardedRecords)
	{
		if (IsRuntimeFractalClass(Record->FractalClass))
		{
			RegisterRecordMaps(*Record);
		}
	}
	DiscoverAndBind();
	RegisterConsumerHooks(*World);
}

FName UWoTFractalSubsystem::RuntimePlaceholderName(const FName IdentityKey)
{
	return FName(*(FString(RuntimePlaceholderPrefix()) + IdentityKey.ToString()));
}

bool UWoTFractalSubsystem::BindRuntimeProceduralSlot(
	UPrimitiveComponent* Component, const int32 Slot, const FName IdentityKey)
{
	const FWoTFractalTextureRecord* const* Record = RecordsByIdentity.Find(IdentityKey);
	if (!Component || !Record || !*Record
		|| Slot < 0 || Slot >= Component->GetNumMaterials())
	{
		return false;
	}
	int32 BoundCount = 0;
	return BindMaterialSlot(
		Component, Slot, Component->GetMaterial(Slot), **Record, BoundCount);
}

void UWoTFractalSubsystem::Deinitialize()
{
	if (UWorld* World = GetWorld(); World && ActorSpawnedHandle.IsValid())
	{
		World->RemoveOnActorSpawnedHandler(ActorSpawnedHandle);
	}
	ActorSpawnedHandle.Reset();
	if (LevelAddedHandle.IsValid())
	{
		FWorldDelegates::LevelAddedToWorld.Remove(LevelAddedHandle);
		LevelAddedHandle.Reset();
	}
	if (GuardedReadinessHandle.IsValid())
	{
		UnregisterWoTBootstrapWorldReadinessListener(GuardedReadinessHandle);
	}
	bGuardedCatalogActive = false;
	PendingBindActors.Reset();
	BoundConsumers.Reset();
	WarnedRoots.Reset();
	MaterialReplacements.Reset();
	RecordsByIdentity.Reset();
	RecordsByBareName.Reset();
	RecordsByObjectPath.Reset();
	RehydratedRecords.Reset();
	VerifiedProceduralResources.Reset();
	GuardedRecords.Reset();
	ActiveStates.Reset();
	ActiveOrder.Reset();
	OwnedMaterials.Reset();
	OwnedTextures.Reset();
	Catalog = nullptr;
	bFrozen = false;
	SimulationTime = 0.0;
	ActivationSerial = 0;
	Super::Deinitialize();
}

TStatId UWoTFractalSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UWoTFractalSubsystem, STATGROUP_Tickables);
}

bool UWoTFractalSubsystem::IsTickable() const
{
	if (!FractalWorldReady() || !WoTPresentationAnimationEnabled(GetWorld()))
	{
		return false;
	}
	// PendingBindActors keeps the tick alive on maps whose FIRST fractal
	// consumer is a runtime spawn (no world-surface record ever activated).
	return (ActiveStates.Num() > 0 || PendingBindActors.Num() > 0) && !IsTemplate();
}

bool UWoTFractalSubsystem::UsesIdentityUVRoot(const FName RootName)
{
	// Masters whose Tex sampler reads the surface's own UVs (UV0, possibly
	// through the PanU/PanV=0 panner — panning applies to a live fractal
	// texture exactly as UE1 pans a procedural surface, so keep-parent is
	// correct for panned surfaces too).
	static const TSet<FName> Roots = {
		TEXT("M_WoT_WorldTranslucent"), TEXT("M_WoT_WorldModulated"),
		TEXT("M_WoT_World"), TEXT("M_WoT_WorldMasked"),
		TEXT("M_WoT_WorldWave"), TEXT("M_WoT_WorldWaveMasked"),
		TEXT("M_WoT_WorldWaveTranslucent"), TEXT("M_WoT_WorldWaveModulated"),
		TEXT("M_WoT_WorldT_SceneBlend"), TEXT("M_WoT_WorldWaveT_SceneBlend"),
		TEXT("M_WoT_MoverLit"), TEXT("M_WoT_MoverLitMasked"),
		TEXT("M_WoT_ParticleAdd"), TEXT("M_WoT_ParticleMod"),
		TEXT("M_WoT_Master"), TEXT("M_WoT_Masked"),
		TEXT("M_WoT_Unlit"), TEXT("M_WoT_UnlitMasked"),
		TEXT("M_WoT_MeshLit"), TEXT("M_WoT_MeshLit2S"),
		TEXT("M_WoT_MeshLitMasked"), TEXT("M_WoT_MeshLitT"),
		TEXT("M_WoT_MeshTranslucent"), TEXT("M_WoT_MeshModulate"),
		// The retired-sheet families (PORT_NOTES "Procedural textures animate when recently seen"):
		// the shared MI_<Tex> instances parent on the FractalLive pair, and
		// the FB particle/mesh masters were regenerated as identity graphs —
		// every fractal consumer now keeps its parent and swaps Tex only.
		TEXT("M_WoT_FractalLiveLit"), TEXT("M_WoT_FractalLiveAdd"),
		TEXT("M_WoT_ParticleAddFB"), TEXT("M_WoT_ParticleModFB"),
		TEXT("M_WoT_MeshFBAddGlow"),
		// The Entry family duplicates the World masters graph for graph
		// (Tools/import/entry_weather_materials.py: identity UV0 through the
		// same PanU/PanV panner); guarded runtime BSP sections ride them.
		TEXT("M_WoT_EntryWorld"), TEXT("M_WoT_EntryWorld_TwoSided"),
		TEXT("M_WoT_EntryMasked"), TEXT("M_WoT_EntryMasked_TwoSided"),
		TEXT("M_WoT_EntryTranslucent"), TEXT("M_WoT_EntryTranslucent_TwoSided"),
		TEXT("M_WoT_EntryModulated"), TEXT("M_WoT_EntryModulated_TwoSided"),
		// Sky-box translucent sheets rebound to the per-channel SceneColor copy,
		// and the two passes of a stacked sky layer (sky_layer_materials.py).
		TEXT("M_WoT_EntryTranslucent_SceneBlend"),
		TEXT("M_WoT_EntrySkyModulate"), TEXT("M_WoT_EntrySkyAdd"),
		// PF_Mirrored translucent sections (WoTRuntimeMirror.h): Tex rides the
		// same identity UV0 + PanU/PanV panner (Tools/import/mirror_material.py);
		// the Battle2_01/Battle3_01 pond sheet is the WetTexture water_lily_01.
		TEXT("M_WoT_EntryTranslucent_Mirror"),
	};
	return Roots.Contains(RootName);
}

bool UWoTFractalSubsystem::ActivateRecord(const FWoTFractalTextureRecord& Record)
{
	TSet<FName> ActivationStack;
	return ActivateRecordInternal(Record, true, ActivationStack);
}

void UWoTFractalSubsystem::PrimeLiveState(FActiveState& State)
{
	if (!State.Sim)
	{
		return;
	}
	const FWoTFractalTextureRecord& Definition = State.Sim->GetDefinition();
	if (Definition.FractalClass == EWoTFractalClass::Ice
		&& Definition.TimeMethod != 0)
	{
		// UIceTexture::Tick is a full raw-tick override for real-time scroll
		// in UE1. It never enters UTexture's authored prime
		// or fixed-step path, so activation/unfreeze preserves the saved phase.
		return;
	}
	const IWoTFractalSim::FBeforeStep BeforeStep = [this, &State]()
	{
		// UIceTexture's tick paths lock SourceTexture then GlassTexture
		// in UE1. Activation separately follows
		// the outer Lock's glass-then-source order.
		for (int32 Slot = 0; Slot < 2; ++Slot)
		{
			for (const FSourceBinding& Binding : State.Sources)
			{
				if (Binding.Slot == Slot)
				{
					if (const FActiveState* Source = ActiveStates.Find(Binding.Key))
					{
						State.Sim->UpdateSourceIndices(
							Source->Sim->GetOutputIndices(), Binding.Slot);
					}
				}
			}
		}
	};
	State.Sim->Prime(BeforeStep);
	// Retail primes inside the first Lock before sampling. We prime at bind so
	// UE5 can never expose the serialized initial bitmap for one visible frame,
	// then perform the same first constant-time texture step.
	State.Sim->ConstantTimeTick(BeforeStep);
}

const FWoTFractalTextureRecord* UWoTFractalSubsystem::ResolveSeededRecord(
	const FWoTFractalTextureRecord& Record)
{
	// Canonical catalogs embed the retail seed; stripped catalogs omit it (the
	// final packaged-data leak — PORT_NOTES "Built from the player's own copy of the game")
	// and the seed rehydrates from the payload cache at bind time.
	const bool bNeedsPalette = Record.PaletteRGBA.Num() != 256 * 4;
	const bool bNeedsInitial = Record.InitialBitmap.IsEmpty();
	const bool bNeedsSource = Record.SourceIndices.IsEmpty()
		&& !Record.bSourceIsFractal && !Record.SourceTextureRef.IsEmpty();
	const bool bNeedsGlass = Record.GlassIndices.IsEmpty()
		&& !Record.bGlassIsFractal && !Record.GlassTextureRef.IsEmpty();
	if (!bNeedsPalette && !bNeedsInitial && !bNeedsSource && !bNeedsGlass)
	{
		return &Record;
	}
	const FName Key(*Record.QualifiedName());
	if (const TUniquePtr<FWoTFractalTextureRecord>* Cached = RehydratedRecords.Find(Key))
	{
		return Cached->Get();
	}
	UWorld* World = GetWorld();
	UWoTPayloadSubsystem* Payloads = World && World->GetGameInstance()
		? World->GetGameInstance()->GetSubsystem<UWoTPayloadSubsystem>() : nullptr;
	if (!Payloads)
	{
		return nullptr;
	}
	// "Package[.Group].Texture" object path → "texture:Package__Texture" cache
	// key: manifest keys (build_payload_cache) are package + object name with
	// any UE1 group segments DROPPED. Renamed copies are not a concern here —
	// fractal refs are always the retail object path.
	// An activated payload lease serves cache-v3 logical ids; the developer
	// cache-v2 store keys "texture:". Canonical identities may carry a
	// "#export-N" collision suffix on the leaf, never part of the key.
	const FString KeyPrefix = IsWoTPayloadBootstrapActive()
		? FString(TEXT("cache/texture/")) : FString(TEXT("texture:"));
	auto CacheKeyFor = [&KeyPrefix](const FString& ObjectPath)
	{
		TArray<FString> Segments;
		ObjectPath.ParseIntoArray(Segments, TEXT("."));
		if (Segments.Num() >= 2)
		{
			FString Leaf = Segments.Last();
			int32 Hash = INDEX_NONE;
			if (Leaf.FindChar(TEXT('#'), Hash) && Hash > 0) Leaf.LeftInline(Hash);
			return KeyPrefix + Segments[0] + TEXT("__") + Leaf;
		}
		return KeyPrefix + ObjectPath;
	};
	// Mip-0 p8: the blob stores raw palette indices per mip (the payload
	// injector's own decode contract, WoTPayload_ExpandMip).
	auto Mip0Indices = [](const FWoTTexturePayloadData& Payload, TArray<uint8>& Out)
	{
		if (Payload.Mips.IsEmpty())
		{
			return false;
		}
		const FWoTTexturePayloadMip& Mip = Payload.Mips[0];
		const TConstArrayView<uint8> PayloadBytes = Payload.PayloadBytes();
		const uint64 End = Mip.DataOffset + Mip.StoredByteLength;
		if (End > static_cast<uint64>(PayloadBytes.Num())
			|| Mip.StoredByteLength > static_cast<uint64>(MAX_int32))
		{
			return false;
		}
		Out.SetNumUninitialized(static_cast<int32>(Mip.StoredByteLength));
		FMemory::Memcpy(Out.GetData(),
			PayloadBytes.GetData() + Mip.DataOffset,
			static_cast<int32>(Mip.StoredByteLength));
		return true;
	};
	auto PaletteFrom = [](const FWoTTexturePayloadData& Payload,
		TArray<uint8>& Out)
	{
		if (Payload.Palette.Num() != 256)
		{
			return false;
		}
		// bake_flipbooks._palette_rgba: RGBA quads, alpha 255 except a masked
		// texture's palette slot 0.
		Out.SetNumUninitialized(256 * 4);
		for (int32 Index = 0; Index < 256; ++Index)
		{
			const FColor& Color = Payload.Palette[Index];
			uint8* Quad = Out.GetData() + Index * 4;
			Quad[0] = Color.R;
			Quad[1] = Color.G;
			Quad[2] = Color.B;
			Quad[3] = (Payload.bMasked && Index == 0) ? 0 : 255;
		}
		return true;
	};
	TUniquePtr<FWoTFractalTextureRecord> Seeded =
		MakeUnique<FWoTFractalTextureRecord>(Record);
	bool bOk = true;
	// Per-class sourcing (bake_flipbooks): Wet/Ice records carry the SOURCE
	// texture's palette (`spal`) and a derived/serialized composite as their
	// initial state, so both rehydrate from the SOURCE payload — plain
	// `Texture` entries the cache builder already covers. Fire/Wave have no
	// source ref; their palette/initial are the fractal texture's own, which
	// the cache builder does not yet carry (it collects class `Texture` only)
	// — they fail closed until the builder widens (PORT_NOTES rider).
	const bool bSourcePaletteClass =
		Record.FractalClass == EWoTFractalClass::Wet
		|| Record.FractalClass == EWoTFractalClass::Ice;
	TSharedPtr<const FWoTTexturePayloadData> SourcePayload;
	if (!Record.SourceTextureRef.IsEmpty())
	{
		SourcePayload = Payloads->LoadTexturePayload(
			CacheKeyFor(Record.SourceTextureRef));
	}
	if (bNeedsPalette)
	{
		if (bSourcePaletteClass && SourcePayload)
		{
			bOk &= PaletteFrom(*SourcePayload, Seeded->PaletteRGBA);
		}
		else
		{
			const TSharedPtr<const FWoTTexturePayloadData> Own =
				Payloads->LoadTexturePayload(CacheKeyFor(Record.QualifiedName()));
			bOk = Own.IsValid() && PaletteFrom(*Own, Seeded->PaletteRGBA);
		}
	}
	if (bOk && bNeedsSource)
	{
		bOk = SourcePayload.IsValid()
			&& Mip0Indices(*SourcePayload, Seeded->SourceIndices);
	}
	if (bOk && bNeedsGlass)
	{
		const TSharedPtr<const FWoTTexturePayloadData> Glass =
			Payloads->LoadTexturePayload(CacheKeyFor(Record.GlassTextureRef));
		bOk = Glass.IsValid() && Mip0Indices(*Glass, Seeded->GlassIndices);
	}
	if (bOk && bNeedsInitial)
	{
		if (Record.FractalClass == EWoTFractalClass::Ice
			&& Seeded->SourceIndices.Num() == Record.Width * Record.Height)
		{
			// Ice's serialized initial is a cached compose of source+glass and
			// is fully rewritten by the first tick; the source copy matches the
			// retail at-rest look (and the audit's initial==source observation).
			Seeded->InitialBitmap = Seeded->SourceIndices;
		}
		else if (Record.FractalClass == EWoTFractalClass::Wet
			&& Record.SourceWidth > 0 && Record.SourceHeight > 0
			&& Seeded->SourceIndices.Num()
				== Record.SourceWidth * Record.SourceHeight
			&& Record.Width > 0 && Record.Height > 0)
		{
			// Still-water initial: the source imagery nearest-upscaled to the
			// texture size. Retail's serialized wave PHASE is retail data and
			// stays stripped; the wave equation rebuilds motion from drops
			// within seconds (divergence recorded in PORT_NOTES).
			Seeded->InitialBitmap.SetNumUninitialized(Record.Width * Record.Height);
			for (int32 Y = 0; Y < Record.Height; ++Y)
			{
				const int32 SrcY = Y * Record.SourceHeight / Record.Height;
				for (int32 X = 0; X < Record.Width; ++X)
				{
					const int32 SrcX = X * Record.SourceWidth / Record.Width;
					Seeded->InitialBitmap[Y * Record.Width + X] =
						Seeded->SourceIndices[SrcY * Record.SourceWidth + SrcX];
				}
			}
		}
		else
		{
			const TSharedPtr<const FWoTTexturePayloadData> Own =
				Payloads->LoadTexturePayload(CacheKeyFor(Record.QualifiedName()));
			bOk = Own.IsValid() && Mip0Indices(*Own, Seeded->InitialBitmap);
		}
	}
	if (!bOk)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("Fractal seed for %s is stripped and could not rehydrate from the payload cache; fallback remains active"),
			*Record.QualifiedName());
		return nullptr;
	}
	return RehydratedRecords.Add(Key, MoveTemp(Seeded)).Get();
}

bool UWoTFractalSubsystem::ActivateRecordInternal(
	const FWoTFractalTextureRecord& CatalogRecord, const bool bDirectlyUsed,
	TSet<FName>& ActivationStack)
{
	if (!IsRuntimeFractalClass(CatalogRecord.FractalClass)
		|| !WoTPresentationAnimationEnabled(GetWorld()))
	{
		return false;
	}
	const FWoTFractalTextureRecord* Seeded = ResolveSeededRecord(CatalogRecord);
	if (!Seeded)
	{
		return false;
	}
	const FWoTFractalTextureRecord& Record = *Seeded;
	if (Record.PaletteRGBA.Num() != 256 * 4)
	{
		return false;
	}
	if (Record.FractalClass == EWoTFractalClass::Fire)
	{
		for (const FWoTFractalSpark& Spark : Record.Sparks)
		{
			if (!FWoTFireFractalSim::IsAuthoredSparkTypeSupported(Spark.Type))
			{
				UE_LOG(LogTemp, Warning,
					TEXT("Unsupported authored fire spark type %u in %s; fallback sheet remains active"),
					Spark.Type, *Record.QualifiedName());
				return false;
			}
		}
	}
	const FName Key(*Record.QualifiedName());
	if (FActiveState* Existing = ActiveStates.Find(Key))
	{
		if (bDirectlyUsed && !Existing->bDirectlyUsed)
		{
			Existing->bDirectlyUsed = true;
			UploadState(*Existing);
		}
		return true;
	}
	if (ActivationStack.Contains(Key))
	{
		UE_LOG(LogTemp, Warning, TEXT("Cyclic live fractal source at %s"),
			*Record.QualifiedName());
		return false;
	}
	ActivationStack.Add(Key);

	TArray<FSourceBinding> Sources;
	auto ActivateSource = [this, &Record, &ActivationStack, &Sources](
		const FString& Reference, const int32 Slot, const TCHAR* Role)
	{
		const FName SourceKey(*Reference);
		const FWoTFractalTextureRecord* const* SourceRecord =
			RecordsByObjectPath.Find(SourceKey);
		if (!SourceRecord
			|| !ActivateRecordInternal(**SourceRecord, false, ActivationStack))
		{
			UE_LOG(LogTemp, Warning,
				TEXT("Missing procedural %s %s for %s; fallback sheet remains active"),
				Role, *Reference, *Record.QualifiedName());
			return false;
		}
		FSourceBinding& Binding = Sources.AddDefaulted_GetRef();
		Binding.Key = SourceKey;
		Binding.Slot = Slot;
		return true;
	};
	if (Record.FractalClass == EWoTFractalClass::Wet && Record.bSourceIsFractal)
	{
		if (!ActivateSource(Record.SourceTextureRef, 0, TEXT("source")))
		{
			ActivationStack.Remove(Key);
			return false;
		}
	}
	if (Record.FractalClass == EWoTFractalClass::Ice)
	{
		// UIceTexture::Lock locks GlassTexture then SourceTexture before its own
		// update; preserve that dependency/RNG order.
		if (Record.bGlassIsFractal
			&& !ActivateSource(Record.GlassTextureRef, 1, TEXT("glass")))
		{
			ActivationStack.Remove(Key);
			return false;
		}
		if (Record.bSourceIsFractal
			&& !ActivateSource(Record.SourceTextureRef, 0, TEXT("source")))
		{
			ActivationStack.Remove(Key);
			return false;
		}
	}
	ActivationStack.Remove(Key);
	if (!bGlobalFractalRandomInitialized)
	{
		GlobalFractalRandomStream.Initialize(
			static_cast<uint32>(FPlatformTime::Cycles64()));
		bGlobalFractalRandomInitialized = true;
	}

	FActiveState State;
	State.Sim = MakeFractalSim(Record.FractalClass);
	if (!State.Sim)
	{
		return false;
	}
	State.Sim->SetRandomStream(&GlobalFractalRandomStream);
	if (!State.Sim->Initialize(Record))
	{
		UE_LOG(LogTemp, Warning,
			TEXT("Invalid live fractal payload for %s; fallback sheet remains active"),
			*Record.QualifiedName());
		return false;
	}
	State.Sources = MoveTemp(Sources);
	// UTexture constructors sample appSeconds independently.  Preserve that
	// non-equality so a dependent texture's first Lock performs Update/Prime.
	State.LastUpdateTime = SimulationTime
		+ static_cast<double>(++ActivationSerial) * TextureConstructionEpsilon;
	State.bDirectlyUsed = bDirectlyUsed;
	PrimeLiveState(State);
	TArray<uint8> InitialBGRA;
	PaletteMapBGRA(*State.Sim, InitialBGRA);
	const FName TextureName = MakeUniqueObjectName(
		GetTransientPackage(), UTexture2D::StaticClass(),
		FName(*FString::Printf(TEXT("WoTLive_%s_%s"),
			*Record.PackageName.ToString(), *Record.TextureName.ToString())));
	State.Texture = UTexture2D::CreateTransient(
		Record.Width, Record.Height, PF_B8G8R8A8,
		TextureName,
		TConstArrayView64<uint8>(InitialBGRA.GetData(), InitialBGRA.Num()));
	if (!State.Texture)
	{
		return false;
	}
	// WoT's material roots consume palette-expanded display bytes through
	// LinearColor samplers. Keep the live texture in that same raw-byte space;
	// in particular, STY_Modulated depends on palette 128 remaining the exact
	// 2x-blend identity rather than being sRGB-decoded to roughly 0.216.
	State.Texture->SRGB = false;
	State.Texture->Filter = TF_Bilinear;
	State.Texture->AddressX = TA_Wrap;
	State.Texture->AddressY = TA_Wrap;
	State.Texture->UpdateResource();
	OwnedTextures.Add(State.Texture);
	ActiveStates.Add(Key, MoveTemp(State));
	ActiveOrder.Add(Key);
	if (bFrozen)
	{
		ApplyFrozenPose();
	}
	return true;
}

int32 UWoTFractalSubsystem::GetActiveTextureCount() const
{
	return ActiveStates.Num();
}

UTexture2D* UWoTFractalSubsystem::FindLiveTexture(const FName QualifiedName) const
{
	const FActiveState* State = ActiveStates.Find(QualifiedName);
	return State ? State->Texture : nullptr;
}

int32 UWoTFractalSubsystem::GetStepCount(const FName QualifiedName) const
{
	const FActiveState* State = ActiveStates.Find(QualifiedName);
	return State && State->Sim ? State->Sim->GetStepCount() : INDEX_NONE;
}

#if WITH_DEV_AUTOMATION_TESTS
bool UWoTFractalSubsystem::GetIcePositionForTesting(
	const FName QualifiedName, float& OutMasterCount,
	float& OutUPosition, float& OutVPosition) const
{
	const FActiveState* State = ActiveStates.Find(QualifiedName);
	if (!State || !State->Sim
		|| State->Sim->GetDefinition().FractalClass != EWoTFractalClass::Ice)
	{
		return false;
	}
	const FWoTIceFractalSim* Ice = static_cast<const FWoTIceFractalSim*>(
		State->Sim.Get());
	OutMasterCount = Ice->GetMasterCount();
	OutUPosition = Ice->GetUPosition();
	OutVPosition = Ice->GetVPosition();
	return true;
}
#endif

void UWoTFractalSubsystem::Tick(const float DeltaTime)
{
	// Late binds first: a record activated here under freeze receives the
	// frozen pose via ActivateRecordInternal before the edge logic below.
	FlushPendingBinds();
	const bool bFreezeNow = CVarWoTFractalFreeze.GetValueOnGameThread() != 0;
	if (bFreezeNow != bFrozen)
	{
		bFrozen = bFreezeNow;
		if (bFrozen)
		{
			ApplyFrozenPose();
		}
		else
		{
			GlobalFractalRandomStream.Initialize(
				static_cast<uint32>(FPlatformTime::Cycles64()));
			uint64 ResetOrder = 0;
			for (const FName Key : ActiveOrder)
			{
				if (FActiveState* State = ActiveStates.Find(Key))
				{
					State->Sim->Reset();
					State->LastUpdateTime = SimulationTime
						+ static_cast<double>(++ResetOrder)
							* TextureConstructionEpsilon;
				}
			}
			// A falling edge is a fresh live activation. Sources precede their
			// dependants in ActiveOrder, so every Wet parent sees its source's
			// newly primed output before its own authored prime + first step.
			for (const FName Key : ActiveOrder)
			{
				if (FActiveState* State = ActiveStates.Find(Key))
				{
					PrimeLiveState(*State);
				}
			}
			for (const FName Key : ActiveOrder)
			{
				if (FActiveState* State = ActiveStates.Find(Key))
				{
					UploadState(*State);
				}
			}
		}
	}
	if (bFrozen)
	{
		return;
	}
	SimulationTime += DeltaTime;
	TArray<FName> DirectKeys;
	for (const FName Key : ActiveOrder)
	{
		if (const FActiveState* State = ActiveStates.Find(Key);
			State && State->bDirectlyUsed)
		{
			DirectKeys.Add(Key);
		}
	}
	const UWorld* World = GetWorld();
	const float WorldNow = World ? World->GetTimeSeconds() : 0.0f;
	for (const FName Key : DirectKeys)
	{
		// Retail semantics: a texture no rendered surface locks does not
		// simulate (adversarial-review finding 3). Its LastUpdateTime stays
		// put, so the eventual catch-up delta matches an equally long gap
		// between retail locks.
		if (!bRenderGateBypass && !IsRecordRenderedRecently(Key, WorldNow))
		{
			continue;
		}
		TSet<FName> UpdateStack;
		UpdateStateAtTime(Key, SimulationTime, UpdateStack);
	}
}

int32 UWoTFractalSubsystem::UpdateStateAtTime(
	const FName Key, const double TargetTime, TSet<FName>& UpdateStack)
{
	FActiveState* State = ActiveStates.Find(Key);
	if (!State || TargetTime == State->LastUpdateTime || UpdateStack.Contains(Key))
	{
		return 0;
	}
	UpdateStack.Add(Key);
	const double OldTime = State->LastUpdateTime;
	const FWoTFractalTextureRecord& Definition = State->Sim->GetDefinition();
	// UIceTexture's raw real-time Tick locks both inputs at time 0.0, and
	// UTexture::Lock deliberately skips Update at zero.
	// It still samples their current mip bytes. Frame-synced ConstantTimeTick
	// instead locks at LastUpdateTime and advances them normally.
	const bool bAdvanceSources = Definition.FractalClass != EWoTFractalClass::Ice
		|| Definition.TimeMethod == 0;
	const IWoTFractalSim::FBeforeStep BeforeStep = [this, State, OldTime,
		bAdvanceSources, &UpdateStack]()
	{
		for (int32 Slot = 0; Slot < 2; ++Slot)
		{
			for (const FSourceBinding& Binding : State->Sources)
			{
				if (Binding.Slot == Slot)
				{
					if (bAdvanceSources)
					{
						UpdateStateAtTime(Binding.Key, OldTime, UpdateStack);
					}
					if (const FActiveState* Source = ActiveStates.Find(Binding.Key))
					{
						State->Sim->UpdateSourceIndices(
							Source->Sim->GetOutputIndices(), Binding.Slot);
					}
				}
			}
		}
	};
	const int32 Steps = State->Sim->TickTexture(
		static_cast<float>(TargetTime - OldTime), BeforeStep);
	State->LastUpdateTime = TargetTime;
	UpdateStack.Remove(Key);
	if (Steps > 0 && State->bDirectlyUsed)
	{
		UploadState(*State);
	}
	return Steps;
}

void UWoTFractalSubsystem::PoseState(
	const FName Key, TSet<FName>& Posed, TSet<FName>& PoseStack)
{
	if (Posed.Contains(Key) || PoseStack.Contains(Key))
	{
		return;
	}
	FActiveState* State = ActiveStates.Find(Key);
	if (!State)
	{
		return;
	}
	PoseStack.Add(Key);
	for (const FSourceBinding& Binding : State->Sources)
	{
		PoseState(Binding.Key, Posed, PoseStack);
	}
	const IWoTFractalSim::FBeforeStep BeforeStep = [this, State]()
	{
		for (int32 Slot = 0; Slot < 2; ++Slot)
		{
			for (const FSourceBinding& Binding : State->Sources)
			{
				if (Binding.Slot == Slot)
				{
					if (const FActiveState* Source = ActiveStates.Find(Binding.Key))
					{
						State->Sim->UpdateSourceIndices(
							Source->Sim->GetOutputIndices(), Binding.Slot);
					}
				}
			}
		}
	};
	State->Sim->Prime(BeforeStep);
	for (int32 Step = 0; Step < FrozenPoseSteps; ++Step)
	{
		State->Sim->ConstantTimeTick(BeforeStep);
	}
	PoseStack.Remove(Key);
	Posed.Add(Key);
}

void UWoTFractalSubsystem::ApplyFrozenPose()
{
	GlobalFractalRandomStream.Initialize(1u);
	for (TPair<FName, FActiveState>& Pair : ActiveStates)
	{
		Pair.Value.Sim->Reset();
		Pair.Value.LastUpdateTime = SimulationTime;
	}
	TArray<FName> PoseKeys;
	for (const TPair<FName, FActiveState>& Pair : ActiveStates)
	{
		if (Pair.Value.bDirectlyUsed)
		{
			PoseKeys.Add(Pair.Key);
		}
	}
	PoseKeys.Sort(FNameLexicalLess());
	TSet<FName> Posed;
	TSet<FName> PoseStack;
	for (const FName Key : PoseKeys)
	{
		PoseState(Key, Posed, PoseStack);
	}
	for (TPair<FName, FActiveState>& Pair : ActiveStates)
	{
		if (Pair.Value.bDirectlyUsed)
		{
			UploadState(Pair.Value);
		}
	}
}

void UWoTFractalSubsystem::UploadState(FActiveState& State, const bool bInitializeResource)
{
	if (!State.Texture || !State.Sim)
	{
		return;
	}
	TArray<uint8> BGRA;
	PaletteMapBGRA(*State.Sim, BGRA);
	if (bInitializeResource || !State.Texture->GetResource())
	{
		FTexturePlatformData* PlatformData = State.Texture->GetPlatformData();
		if (PlatformData && PlatformData->Mips.Num() == 1)
		{
			FTexture2DMipMap& Mip = PlatformData->Mips[0];
			void* Dest = Mip.BulkData.Lock(LOCK_READ_WRITE);
			FMemory::Memcpy(Dest, BGRA.GetData(), BGRA.Num());
			Mip.BulkData.Unlock();
			State.Texture->UpdateResource();
		}
		return;
	}

	uint8* Upload = new uint8[BGRA.Num()];
	FMemory::Memcpy(Upload, BGRA.GetData(), BGRA.Num());
	FUpdateTextureRegion2D* Region = new FUpdateTextureRegion2D(
		0, 0, 0, 0, State.Sim->GetDefinition().Width, State.Sim->GetDefinition().Height);
	State.Texture->UpdateTextureRegions(
		0, 1, Region, State.Sim->GetDefinition().Width * 4, 4, Upload,
		[](uint8* Data, const FUpdateTextureRegion2D* Regions)
		{
			delete[] Data;
			delete Regions;
		});
}

FName UWoTFractalSubsystem::MakeFractalIdentityKey(const FName PackageName,
	const FName TextureName)
{
	// The importers' asset-name spelling: '-' folds to '_' (import_map.py
	// `safe = mat.replace("-","_")`); FName comparison is case-insensitive.
	FString Key = PackageName.ToString() + TEXT("__") + TextureName.ToString();
	Key.ReplaceCharInline(TEXT('-'), TEXT('_'));
	return FName(*Key);
}

FName UWoTFractalSubsystem::RecordIdentityKey(const FWoTFractalTextureRecord& Record)
{
	const int32 Separator = Record.CacheLogicalBase.Find(
		TEXT("__"), ESearchCase::CaseSensitive);
	const bool bExact = Separator > 0
		&& Record.CacheLogicalBase.Left(Separator).Contains(
			TEXT("."), ESearchCase::CaseSensitive);
	return bExact
		? FName(*WoTRetailObjectNameSafe(Record.CacheLogicalBase))
		: MakeFractalIdentityKey(Record.PackageName, Record.TextureName);
}

void UWoTFractalSubsystem::RegisterRecordMaps(const FWoTFractalTextureRecord& Record)
{
	RecordsByObjectPath.Add(FName(*Record.QualifiedName()), &Record);
	RecordsByIdentity.Add(RecordIdentityKey(Record), &Record);
	// Bare names are a LAST-RESORT key: cross-package collisions (Flames02-09,
	// Fire_Torch_001-003, WetPart01) poison the bare entry so only qualified
	// lookups can resolve those records.
	const FName Bare = Record.TextureName;
	if (const FWoTFractalTextureRecord* const* Existing = RecordsByBareName.Find(Bare))
	{
		if (*Existing != &Record)
		{
			RecordsByBareName.Add(Bare, nullptr);
		}
	}
	else
	{
		RecordsByBareName.Add(Bare, &Record);
	}
}

const FWoTFractalTextureRecord* UWoTFractalSubsystem::FindRecordByTexturePath(
	const FString& TexturePath) const
{
	// The importer layout puts the package as the parent folder and the
	// texture as the asset name: /Game/WoT/Textures/<Pkg>/<Name>. (The retired
	// Flipbooks/<Pkg>/<Name>_sheet form is no longer accepted — the sheet lane
	// is gone.)
	static const FString Root = TEXT("/Game/WoT/Textures/");
	if (!TexturePath.StartsWith(Root))
	{
		return nullptr;
	}
	FString Pkg, Rest;
	if (!TexturePath.Mid(Root.Len()).Split(TEXT("/"), &Pkg, &Rest))
	{
		return nullptr;
	}
	// "<Name>.<Name>" object path → asset name; tolerate a bare name too.
	FString Name = Rest;
	int32 DotIndex = INDEX_NONE;
	if (Name.FindChar(TEXT('.'), DotIndex))
	{
		Name.LeftInline(DotIndex);
	}
	const FWoTFractalTextureRecord* const* Found = RecordsByIdentity.Find(
		MakeFractalIdentityKey(FName(*Pkg), FName(*Name)));
	return Found ? *Found : nullptr;
}

namespace
{
	// Per-triangle-type/blend material variants (mesh2glb/bsp2obj split per
	// (texture, type), portal route stacking blend + sidedness): a suffixed
	// instance carries the SAME fractal identity as its base. Exact names
	// take precedence; the strip is a fallback only. Longest first so "_Mod"
	// never half-matches as "_M". Suffixes CHAIN (Battle2_02 pond portals:
	// s_rainwater_T_2S), so callers peel one layer per call until a hit.
	const TCHAR* WoTFractal_TypeSuffixes[] = {
		TEXT("_Mod"), TEXT("_2S"), TEXT("_M"), TEXT("_T")};

	FString WoTFractal_StripTypeSuffix(const FString& Name)
	{
		for (const TCHAR* Suffix : WoTFractal_TypeSuffixes)
		{
			if (Name.EndsWith(Suffix, ESearchCase::CaseSensitive)
				&& Name.Len() > FCString::Strlen(Suffix))
			{
				return Name.LeftChop(FCString::Strlen(Suffix));
			}
		}
		return FString();
	}
}

const FWoTFractalTextureRecord* UWoTFractalSubsystem::FindRecordByMaterialName(
	const FName MaterialName) const
{
	FString Name = MaterialName.ToString();
	// Importer-authored instance prefixes that carry a QUALIFIED Pkg__Tex tail
	// (import_map MI_W_<slug>, import_flame_fx MI_PSAdd[FB]_/MI_PSMod[FB]_<slug>
	// plus its identity-only MI_MeshFB_ children (import_flame_fx.py:265-267),
	// mesh pipeline MI_MeshT_/MI_Skin_<slug>).
	static const TCHAR* QualifiedPrefixes[] = {
		TEXT("MI_W_"), TEXT("MI_PSAddFB_"), TEXT("MI_PSModFB_"), TEXT("MI_MeshFB_"),
		TEXT("MI_PSAdd_"), TEXT("MI_PSMod_"), TEXT("MI_PSMod2x_"),
		TEXT("MI_MeshT_"), TEXT("MI_Skin_")};
	for (const TCHAR* Prefix : QualifiedPrefixes)
	{
		if (Name.StartsWith(Prefix))
		{
			FString Tail = Name.Mid(FCString::Strlen(Prefix));
			if (const FWoTFractalTextureRecord* const* Found =
				RecordsByIdentity.Find(FName(*Tail)))
			{
				return *Found;
			}
			// Type-suffixed variant (Pkg__Tex_2S, chained Pkg__Tex_T_2S):
			// same identity. The retired sheet-referencer discovery was
			// suffix-blind; without this strip, suffixed world/portal
			// surfaces lose their live binding (Battle2_02 rain + pond,
			// 2026-07-19 stripped fleet boot-smoke).
			for (FString Stripped = WoTFractal_StripTypeSuffix(Tail);
				!Stripped.IsEmpty();
				Stripped = WoTFractal_StripTypeSuffix(Stripped))
			{
				if (const FWoTFractalTextureRecord* const* Found =
					RecordsByIdentity.Find(FName(*Stripped)))
				{
					return *Found;
				}
			}
			return nullptr;
		}
	}
	// The direct flipbook instances are bare MI_<Tex> (pre-identity naming);
	// only unique bare names resolve (collisions are poisoned to null).
	if (Name.RemoveFromStart(TEXT("MI_")))
	{
		if (const FWoTFractalTextureRecord* const* Found =
			RecordsByBareName.Find(FName(*Name)))
		{
			return *Found;   // null for poisoned collisions
		}
		for (FString Stripped = WoTFractal_StripTypeSuffix(Name);
			!Stripped.IsEmpty();
			Stripped = WoTFractal_StripTypeSuffix(Stripped))
		{
			if (const FWoTFractalTextureRecord* const* Found =
				RecordsByBareName.Find(FName(*Stripped)))
			{
				return *Found;
			}
		}
	}
	return nullptr;
}

const FWoTFractalTextureRecord* UWoTFractalSubsystem::FindRecordForMaterial(
	UMaterialInterface* Material) const
{
	if (!Material)
	{
		return nullptr;
	}
	// 1) A bound Tex texture names its package/texture by ASSET PATH. This is
	//    the only signal a gameplay MID carries (Mashadar GlowMid). An
	//    already-rebound slot instead holds one of our transient "WoTLive_*"
	//    textures — that is the DONE signal, and it must return null before the
	//    name walk below (the MID's PARENT is the importer-named instance, so
	//    the chain walk alone would re-resolve a bound slot forever).
	{
		UTexture* BoundTexture = nullptr;
		if (Material->GetTextureParameterValue(
			FHashedMaterialParameterInfo(FName(TEXT("Tex"))), BoundTexture)
			&& BoundTexture)
		{
			const FString BoundName = BoundTexture->GetFName().ToString();
			if (BoundName.StartsWith(TEXT("WoTLive_")))
			{
				return nullptr;   // already live
			}
			// Guarded materializers bind a "WoTProc_<Pkg>__<Tex>" placeholder
			// (the authenticated mip-0 seed through its own palette) whose
			// name is the identity key.
			if (BoundName.StartsWith(RuntimePlaceholderPrefix()))
			{
				const int32 PrefixLength = FCString::Strlen(RuntimePlaceholderPrefix());
				const FWoTFractalTextureRecord* const* Found = RecordsByIdentity.Find(
					FName(*BoundName.Mid(PrefixLength)));
				if (!Found)
				{
					// MakeUniqueObjectName numbers repeated placeholders through
					// the FName number part; the plain string is the base name.
					Found = RecordsByIdentity.Find(FName(
						*BoundTexture->GetFName().GetPlainNameString().Mid(PrefixLength)));
				}
				return Found ? *Found : nullptr;
			}
			if (const FWoTFractalTextureRecord* Found = FindRecordByTexturePath(
				BoundTexture->GetPathName()))
			{
				return Found;
			}
		}
	}
	// 2) The importer-authored material NAME (walk the instance chain so a MID
	//    made from MI_W_<slug> still resolves after the sheet asset is gone).
	for (const UMaterialInterface* Chain = Material; Chain;)
	{
		if (const FWoTFractalTextureRecord* Found =
			FindRecordByMaterialName(Chain->GetFName()))
		{
			return Found;
		}
		const UMaterialInstance* Instance = Cast<UMaterialInstance>(Chain);
		Chain = Instance ? ToRawPtr(Instance->Parent) : nullptr;
	}
	return nullptr;
}

UMaterialInstanceDynamic* UWoTFractalSubsystem::MakeLiveMaterial(
	UMaterialInterface* Original, UTexture2D* LiveTexture)
{
	if (!Original || !LiveTexture)
	{
		return nullptr;
	}
	UMaterialInstanceDynamic* DynamicOriginal =
		Cast<UMaterialInstanceDynamic>(Original);
	if (!DynamicOriginal)
	{
		if (UMaterialInstanceDynamic** Existing = MaterialReplacements.Find(Original))
		{
			return *Existing;
		}
	}
	// Sheet-scrub retirement: every fractal-bearing master samples Tex at plain
	// frac(UV0) (the importers author identity parents; the FB masters were
	// regenerated in place), so the live swap is uniformly keep-parent + Tex.
	UMaterial* Root = Original->GetMaterial();
	if (!Root || !UsesIdentityUVRoot(Root->GetFName()))
	{
		// The record's sim was already activated; leaving this silent hides
		// a surface stuck on its authored fallback (adversarial-review finding 1).
		const FName RootName = Root ? Root->GetFName() : NAME_None;
		if (!WarnedRoots.Contains(RootName))
		{
			WarnedRoots.Add(RootName);
			UE_LOG(LogTemp, Warning,
				TEXT("Fractal-textured material %s has uncovered root %s; authored fallback remains active"),
				*Original->GetPathName(), *RootName.ToString());
		}
		return nullptr;
	}
	if (DynamicOriginal)
	{
		// Gameplay owns this component-local handle (Mashadar's GlowMid is the
		// canonical case) and continues writing it after binding. Reparenting or
		// snapshotting into a child would detach those writes from the renderer.
		DynamicOriginal->SetTextureParameterValue(TEXT("Tex"), LiveTexture);
		return DynamicOriginal;
	}
	UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(Original, this);
	if (!MID)
	{
		return nullptr;
	}
	MID->SetTextureParameterValue(TEXT("Tex"), LiveTexture);
	OwnedMaterials.Add(MID);
	MaterialReplacements.Add(Original, MID);
	return MID;
}

void UWoTFractalSubsystem::DiscoverAndBind()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	int32 Bound = 0;
	for (TActorIterator<AActor> ActorIt(World); ActorIt; ++ActorIt)
	{
		BindActor(*ActorIt, Bound);
	}
	UE_LOG(LogTemp, Display,
		TEXT("WOTFRACTAL: %d live sims, %d material slots bound"),
		ActiveStates.Num(), Bound);
}

bool UWoTFractalSubsystem::BindMaterialSlot(
	UPrimitiveComponent* Component, const int32 Slot,
	UMaterialInterface* Original, const FWoTFractalTextureRecord& Record,
	int32& BoundCount)
{
	if (!Component || !Original || !ActivateRecord(Record))
	{
		return false;
	}
	const FName RecordKey(*Record.QualifiedName());
	UTexture2D* LiveTexture = FindLiveTexture(RecordKey);
	if (UMaterialInstanceDynamic* MID = MakeLiveMaterial(Original, LiveTexture))
	{
		Component->SetMaterial(Slot, MID);
		BoundConsumers.FindOrAdd(RecordKey).AddUnique(Component);
		++BoundCount;
		return true;
	}
	return false;
}

#if WITH_DEV_AUTOMATION_TESTS
bool UWoTFractalSubsystem::BindMaterialSlotForTesting(
	UPrimitiveComponent* Component, const int32 Slot,
	const FWoTFractalTextureRecord& Record)
{
	int32 BoundCount = 0;
	return Component && BindMaterialSlot(
		Component, Slot, Component->GetMaterial(Slot), Record, BoundCount);
}
#endif

void UWoTFractalSubsystem::BindActor(AActor* Actor, int32& BoundCount)
{
	if (!IsValid(Actor))
	{
		return;
	}
	TInlineComponentArray<UPrimitiveComponent*> Components(Actor);
	for (UPrimitiveComponent* Component : Components)
	{
		if (!IsValid(Component))
		{
			continue;
		}
		const int32 NumMaterials = Component->GetNumMaterials();
		for (int32 Slot = 0; Slot < NumMaterials; ++Slot)
		{
			// Idempotent: a slot we already rebound holds a MID whose
			// Tex/SheetTex is a transient "WoTLive_*" texture — the explicit
			// already-live early-out in FindRecordForMaterial.
			UMaterialInterface* Original = Component->GetMaterial(Slot);
			const FWoTFractalTextureRecord* Record = FindRecordForMaterial(Original);
			if (!Record)
			{
				continue;
			}
			BindMaterialSlot(Component, Slot, Original, *Record, BoundCount);
		}
	}
}

void UWoTFractalSubsystem::HandleActorSpawned(AActor* Actor)
{
	if (Actor && RecordsByIdentity.Num() > 0)
	{
		PendingBindActors.Add(Actor);
	}
}

void UWoTFractalSubsystem::HandleLevelAddedToWorld(ULevel* Level, UWorld* OwningWorld)
{
	if (!Level || OwningWorld != GetWorld() || RecordsByIdentity.Num() == 0)
	{
		return;
	}
	for (AActor* Actor : Level->Actors)
	{
		if (Actor)
		{
			PendingBindActors.Add(Actor);
		}
	}
}

void UWoTFractalSubsystem::FlushPendingBinds()
{
	if (PendingBindActors.Num() == 0)
	{
		return;
	}
	int32 Bound = 0;
	TArray<TWeakObjectPtr<AActor>> Pending = MoveTemp(PendingBindActors);
	PendingBindActors.Reset();
	for (const TWeakObjectPtr<AActor>& WeakActor : Pending)
	{
		if (AActor* Actor = WeakActor.Get())
		{
			BindActor(Actor, Bound);
		}
	}
	if (Bound > 0)
	{
		UE_LOG(LogTemp, Verbose,
			TEXT("WOTFRACTAL: late-bound %d material slots"), Bound);
	}
}

bool UWoTFractalSubsystem::IsRecordRenderedRecently(const FName Key, const float WorldNow)
{
	// Retail advances a fractal texture only when a rendered surface locks it
	// (via UTexture::Lock). The grace window is the closest
	// stable analogue available before UE5 draw submission, not a tunable.
	constexpr float RenderLockGraceSeconds = 0.1f;
	TArray<TWeakObjectPtr<UPrimitiveComponent>>* Consumers = BoundConsumers.Find(Key);
	if (!Consumers)
	{
		return false;
	}
	for (int32 Index = Consumers->Num() - 1; Index >= 0; --Index)
	{
		UPrimitiveComponent* Component = (*Consumers)[Index].Get();
		if (!Component)
		{
			Consumers->RemoveAtSwap(Index);
			continue;
		}
		if (WorldNow - Component->GetLastRenderTimeOnScreen() <= RenderLockGraceSeconds)
		{
			return true;
		}
	}
	return false;
}
