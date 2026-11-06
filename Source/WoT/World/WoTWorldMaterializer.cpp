#include "WoTWorldMaterializer.h"

#include "WoTMaterializedWorldHost.h"
#include "WoTRuntimeBSPRenderer.h"
#include "WoTRuntimeMapSky.h"
#include "WoTRuntimeMirror.h"
#include "WoTWorldProjector.h"

#include "../Assets/WoTRetailIdentity.h"
#include "../Payload/WoTCacheLease.h"
#include "../Payload/WoTPayloadActivation.h"
#include "../Payload/WoTRuntimeRegistry.h"
#include "../Payload/WoTRuntimeProceduralResource.h"
#include "../WoTFractalData.h"
#include "../Payload/WoTRuntimeWorldResource.h"
#include "../WoTBSPPayload.h"
#include "../WoTBootstrapHeartbeat.h"
#include "../WoTCorona.h"
#include "../WoTPayloadSubsystem.h"
#include "../WoTRainCyd.h"
#include "../WoTSkyBox.h"
#include "../WoTSessionSubsystem.h"
#include "../WoTVertexAnimComponent.h"
#include "WoTRetailReader/BSPWorld.h"
#include "WoTRetailReader/ProductionContract.h"
#include "WoTRetailReader/WorldDescriptorSidecar.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/Level.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Components/LightComponent.h"
#include "Components/PostProcessComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerStart.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "NavigationSystem.h"
#include "WoTBootstrapCore/Hash.h"
#include "WoTBootstrapCore/NativeHandles.h"
#include "Misc/App.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "HAL/PlatformTime.h"
#include "UObject/Package.h"
#include "Algo/Reverse.h"

namespace
{
class FWoTTryCriticalSectionGuard final
{
public:
	explicit FWoTTryCriticalSectionGuard(FCriticalSection& InGuard)
		: Guard(InGuard.TryLock() ? &InGuard : nullptr)
	{
	}

	~FWoTTryCriticalSectionGuard()
	{
		if (Guard != nullptr)
		{
			Guard->Unlock();
		}
	}

	FWoTTryCriticalSectionGuard(const FWoTTryCriticalSectionGuard&) = delete;
	FWoTTryCriticalSectionGuard& operator=(
		const FWoTTryCriticalSectionGuard&) = delete;

	bool IsLocked() const { return Guard != nullptr; }

private:
	FCriticalSection* Guard;
};

constexpr const TCHAR* WoTBootstrapPackage =
	TEXT("/Game/WoT/Maps/WoTBootstrap");
constexpr const TCHAR* WoTNeutralShellRoot = TEXT("/WoTNeutralShells/");

enum class EWoTBootstrapExposureKind : uint8
{
	None,
	Initial,
	Return,
};

struct FWoTWorldMaterializationAuthority final
{
	FCriticalSection Guard;
	TUniquePtr<FWoTCacheGenerationLease> NormalLease;
	TArray<FWoTWorldDescriptor> AuthenticatedWorlds;
	/** The separately attested front-end entry world/BSP logical ids, excluded
	 * from the normal-world census (they share the World/BSP resource kinds). */
	TArray<FString> FrontEndReservedWorldIds;
	TArray<FString> FrontEndReservedBSPIds;
	TSet<TWeakObjectPtr<UWorld>> MaterializedWorlds;
	TSet<TWeakObjectPtr<UWorld>> AwaitingExposureWorlds;
	TSet<TWeakObjectPtr<UWorld>> InFlightWorlds;
	TSet<TWeakObjectPtr<UWorld>> ExposedBootstrapWorlds;
	TWeakObjectPtr<UWorld> AwaitingBootstrapExposureWorld;
	EWoTBootstrapExposureKind BootstrapExposureKind =
		EWoTBootstrapExposureKind::None;
	EWoTWorldStartPath BootstrapExposurePath =
		EWoTWorldStartPath::NormalInitialLoad;
	bool bBootstrapSpent = false;
	bool bNormalAdoptionStarted = false;
	bool bNormalAuthorityAdopted = false;
	bool bReadinessPublished = false;
	bool bWorldReadinessPending = false;
#if WITH_AUTOMATION_TESTS
	bool bRejectNextCompletedCandidate = false;
	bool bNormalAuthorityAdoptedForTesting = false;
#endif
};

FWoTWorldMaterializationAuthority& WoTWorldAuthority()
{
	static FWoTWorldMaterializationAuthority Authority;
	return Authority;
}

#if WITH_AUTOMATION_TESTS
FCriticalSection WoTWorldAdoptionObserverGuard;
TFunction<void(FString, const FWoTSha256&)>
	WoTWorldAdoptionObserver;
#endif

template <typename T>
TWoTBootstrapResult<T> WoTWorldMaterializationError(const TCHAR* Message)
{
	return TWoTBootstrapResult<T>::Error(MakeWoTBootstrapError(
		EWoTBootstrapError::CorruptInstallation, Message));
}

FWoTBootstrapStatus WoTWorldMaterializationStatus(const TCHAR* Message)
{
	return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
		EWoTBootstrapError::CorruptInstallation, Message));
}

bool WoTWorldHasDigest(const FWoTSha256& Digest)
{
	for (const uint8 Byte : Digest.Bytes)
	{
		if (Byte != 0)
		{
			return true;
		}
	}
	return false;
}

bool WoTWorldStartPathKnown(EWoTWorldStartPath Path)
{
	switch (Path)
	{
	case EWoTWorldStartPath::NormalInitialLoad:
	case EWoTWorldStartPath::ListenHostTravel:
	case EWoTWorldStartPath::RemoteClientTravel:
	case EWoTWorldStartPath::ListenHostSeamlessTravel:
	case EWoTWorldStartPath::RemoteClientSeamlessTravel:
		return true;
	default:
		return false;
	}
}

bool WoTWorldReadNeutralSlot(const FString& PackageName, uint32& OutSlot)
{
	if (!PackageName.StartsWith(
		WoTNeutralShellRoot, ESearchCase::CaseSensitive))
	{
		return false;
	}
	const FString Leaf = PackageName.Mid(FCString::Strlen(WoTNeutralShellRoot));
	if (Leaf.Len() != 13
		|| !Leaf.StartsWith(TEXT("WoTMapShell"), ESearchCase::CaseSensitive))
	{
		return false;
	}
	int32 ParsedSlot = 0;
	if (!LexTryParseString(ParsedSlot, *Leaf.Right(2))
		|| ParsedSlot < 1 || ParsedSlot > 19)
	{
		return false;
	}
	OutSlot = static_cast<uint32>(ParsedSlot);
	return true;
}

FString WoTWorldCanonicalPackageName(const UWorld& World)
{
	const UPackage* Package = World.GetOutermost();
	const FString PackageName = Package->GetName();
#if WITH_EDITOR
	const int32 PIEInstance = Package->GetPIEInstanceID();
	if (Package->HasAnyPackageFlags(PKG_PlayInEditor)
		&& PIEInstance != INDEX_NONE)
	{
		return UWorld::StripPIEPrefixFromPackageName(
			PackageName, UWorld::BuildPIEPackagePrefix(PIEInstance));
	}
#endif
	return PackageName;
}

bool WoTWorldDescriptorComplete(const FWoTWorldDescriptor& Descriptor)
{
	if (Descriptor.NeutralMapSlot < 1 || Descriptor.NeutralMapSlot > 19
		|| Descriptor.MapIdentity.IsEmpty()
		|| Descriptor.BSPFactId.IsEmpty()
		|| Descriptor.BSPImporter != TEXT("bsp-v1")
		|| Descriptor.WorldLogicalId.IsEmpty()
		|| Descriptor.WorldSchema != TEXT("wot-world-v3")
		|| Descriptor.WorldFactId.IsEmpty()
		|| Descriptor.WorldImporter != TEXT("world-v1")
		|| !WoTWorldHasDigest(Descriptor.PackageSha256)
		|| !WoTWorldHasDigest(Descriptor.WorldSha256)
		|| !WoTWorldHasDigest(Descriptor.SourceRoot)
		|| !WoTWorldHasDigest(Descriptor.MapRoot))
	{
		return false;
	}
	EWoTBSPPartRole PreviousRole = EWoTBSPPartRole::Main;
	TSet<FString> PartIdentities;
	for (int32 PartIndex = 0;
		PartIndex < Descriptor.BSPParts.Num(); ++PartIndex)
	{
		const FWoTBSPPartDescriptor& Part = Descriptor.BSPParts[PartIndex];
		if (Part.PartIdentity.IsEmpty() || Part.LogicalId.IsEmpty()
			|| Part.Schema != TEXT("wot-bsp-v3")
			|| Part.FactId != Descriptor.BSPFactId
			|| !WoTWorldHasDigest(Part.PayloadSha256)
			|| !WoTWorldHasDigest(Part.Sha256)
			|| Part.VertexCount == 0 || Part.IndexCount == 0
			|| Part.SectionCount == 0
			|| (Part.Role != EWoTBSPPartRole::Main
				&& Part.Role != EWoTBSPPartRole::Portal)
			|| PartIdentities.Contains(Part.PartIdentity)
			|| (PartIndex > 0
				&& static_cast<uint8>(Part.Role)
					< static_cast<uint8>(PreviousRole)))
		{
			return false;
		}
		PartIdentities.Add(Part.PartIdentity);
		PreviousRole = Part.Role;
	}
	return true;
}

bool WoTWorldDescriptorSetComplete(
	TConstArrayView<FWoTWorldDescriptor> Descriptors)
{
	if (Descriptors.Num() != 19)
	{
		return false;
	}
	TSet<uint32> Slots;
	TSet<FString> MapIdentities;
	TSet<FString> WorldLogicalIds;
	TSet<FString> BSPLogicalIds;
	for (const FWoTWorldDescriptor& Descriptor : Descriptors)
	{
		if (!WoTWorldDescriptorComplete(Descriptor)
			|| Slots.Contains(Descriptor.NeutralMapSlot)
			|| MapIdentities.Contains(Descriptor.MapIdentity)
			|| WorldLogicalIds.Contains(Descriptor.WorldLogicalId))
		{
			return false;
		}
		Slots.Add(Descriptor.NeutralMapSlot);
		MapIdentities.Add(Descriptor.MapIdentity);
		WorldLogicalIds.Add(Descriptor.WorldLogicalId);
		for (const FWoTBSPPartDescriptor& Part : Descriptor.BSPParts)
		{
			if (BSPLogicalIds.Contains(Part.LogicalId))
			{
				return false;
			}
			BSPLogicalIds.Add(Part.LogicalId);
		}
	}
	for (uint32 Slot = 1; Slot <= 19; ++Slot)
	{
		if (!Slots.Contains(Slot))
		{
			return false;
		}
	}
	return true;
}

const FWoTWorldDescriptor* WoTWorldDescriptorForSlot(
	TConstArrayView<FWoTWorldDescriptor> Descriptors, uint32 Slot)
{
	return Descriptors.FindByPredicate(
		[Slot](const FWoTWorldDescriptor& Descriptor)
		{
			return Descriptor.NeutralMapSlot == Slot;
		});
}

bool WoTWorldRegistryMatchesDescriptors(
	TConstArrayView<FWoTWorldDescriptor> Descriptors,
	TConstArrayView<FString> ReservedWorldIds,
	TConstArrayView<FString> ReservedBSPIds,
	FString* OutWhy)
{
	const TArray<FWoTNeutralResourceId> Worlds =
		FWoTRuntimeRegistry::Get().ListResources(EWoTNeutralResourceKind::World);
	const int32 RegistryBSP = FWoTRuntimeRegistry::Get().ListResources(
		EWoTNeutralResourceKind::BSP).Num();
	int32 ReservedWorldsPresent = 0;
	for (const FString& Id : ReservedWorldIds)
	{
		if (FWoTRuntimeRegistry::Get().FindDescriptor(
			FWoTNeutralResourceId{Id, EWoTNeutralResourceKind::World}).IsSet())
		{
			++ReservedWorldsPresent;
		}
	}
	int32 ReservedBSPPresent = 0;
	for (const FString& Id : ReservedBSPIds)
	{
		if (FWoTRuntimeRegistry::Get().FindDescriptor(
			FWoTNeutralResourceId{Id, EWoTNeutralResourceKind::BSP}).IsSet())
		{
			++ReservedBSPPresent;
		}
	}
	auto Why = [&](const TCHAR* What, int32 DescriptorBSP)
	{
		if (OutWhy)
		{
			*OutWhy = FString::Printf(
				TEXT("%s worlds=%d/%d(+%d front-end) bsp=%d/%d(+%d front-end)"),
				What, Descriptors.Num(), Worlds.Num(), ReservedWorldsPresent,
				DescriptorBSP, RegistryBSP, ReservedBSPPresent);
		}
		return false;
	};
	if (Worlds.Num() - ReservedWorldsPresent != Descriptors.Num())
	{
		return Why(TEXT("world-count"), 0);
	}
	TSet<FString> SeenWorlds;
	TSet<FString> SeenBSP;
	for (const FWoTWorldDescriptor& Descriptor : Descriptors)
	{
		const FWoTNeutralResourceId WorldId{
			Descriptor.WorldLogicalId, EWoTNeutralResourceKind::World};
		const TOptional<FWoTCacheBlobDescriptor> World =
			FWoTRuntimeRegistry::Get().FindDescriptor(WorldId);
		if (!World.IsSet() || World->Schema != Descriptor.WorldSchema
			|| World->Sha256 != Descriptor.WorldSha256
			|| SeenWorlds.Contains(Descriptor.WorldLogicalId))
		{
			return Why(TEXT("world-identity"), 0);
		}
		SeenWorlds.Add(Descriptor.WorldLogicalId);
		for (const FWoTBSPPartDescriptor& Part : Descriptor.BSPParts)
		{
			const FWoTNeutralResourceId PartId{
				Part.LogicalId, EWoTNeutralResourceKind::BSP};
			const TOptional<FWoTCacheBlobDescriptor> BSP =
				FWoTRuntimeRegistry::Get().FindDescriptor(PartId);
			if (!BSP.IsSet() || BSP->Schema != Part.Schema
				|| BSP->Sha256 != Part.Sha256
				|| SeenBSP.Contains(Part.LogicalId))
			{
				return Why(TEXT("bsp-identity"), SeenBSP.Num());
			}
			SeenBSP.Add(Part.LogicalId);
		}
	}
	if (!WoTWorldCensusReconciles(Worlds.Num(), RegistryBSP, Descriptors.Num(),
		SeenBSP.Num(), ReservedWorldsPresent, ReservedBSPPresent))
	{
		return Why(TEXT("bsp-count"), SeenBSP.Num());
	}
	return true;
}

FWoTBootstrapStatus WoTWorldDecodeBSPPart(
	const FWoTBSPPartDescriptor& Descriptor,
	FWoTBSPPayloadData& OutData)
{
	const FWoTNeutralResourceId Resource{
		Descriptor.LogicalId, EWoTNeutralResourceKind::BSP};
	TWoTBootstrapResult<FWoTRuntimeVerifiedObject> Read =
		FWoTRuntimeRegistry::Get().ReadObject(
			Resource, 256ull * 1024ull * 1024ull,
			TEXT("wot-bsp-static-mesh-v1"), TEXT("bsp"));
	if (Read.IsError())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(Read).GetError());
	}
	FWoTRuntimeVerifiedObject Object = MoveTemp(Read).GetValue();
	const TConstArrayView<uint8> Payload = Object.Payload();
	FString FactId;
	FString Importer;
	FString Role;
	FString Tangent;
	uint32 ImporterVersion = 0;
	uint32 VertexCount = 0;
	uint32 IndexCount = 0;
	uint32 SectionCount = 0;
	const FWoTCanonicalJsonValue& Header = Object.Header();
	if (ReadStringField(Header, TEXT("fact_id"), TEXT("BSP header"), FactId).IsSet()
		|| ReadStringField(
			Header, TEXT("importer"), TEXT("BSP header"), Importer).IsSet()
		|| ReadStringField(Header, TEXT("role"), TEXT("BSP header"), Role).IsSet()
		|| ReadStringField(
			Header, TEXT("tangent_algorithm"), TEXT("BSP header"), Tangent).IsSet()
		|| ReadUnsigned32Field(Header, TEXT("importer_version"),
			TEXT("BSP header"), ImporterVersion).IsSet()
		|| ReadUnsigned32Field(Header, TEXT("vertex_count"),
			TEXT("BSP header"), VertexCount).IsSet()
		|| ReadUnsigned32Field(Header, TEXT("index_count"),
			TEXT("BSP header"), IndexCount).IsSet()
		|| ReadUnsigned32Field(Header, TEXT("section_count"),
			TEXT("BSP header"), SectionCount).IsSet()
		|| FactId != Descriptor.FactId || Importer != TEXT("bsp-v1")
		|| (ImporterVersion != 2 && ImporterVersion != 3)
		|| Tangent != TEXT("ue-mikk-preserve-normals-v1")
		|| Role != (Descriptor.Role == EWoTBSPPartRole::Portal
			? TEXT("portal") : TEXT("main"))
		|| VertexCount != Descriptor.VertexCount
		|| IndexCount != Descriptor.IndexCount
		|| SectionCount != Descriptor.SectionCount
		|| Sha256(Payload) != Descriptor.PayloadSha256)
	{
		return WoTWorldMaterializationStatus(
			TEXT("a BSP part header or payload digest mismatched its exact descriptor"));
	}
	FString DecodeError;
	if (!UWoTBSPPayloadLibrary::DecodePayload(Payload, OutData, DecodeError)
		|| OutData.Positions.Num() != static_cast<int32>(Descriptor.VertexCount)
		|| OutData.Indices.Num() != static_cast<int32>(Descriptor.IndexCount)
		|| OutData.Sections.Num() != static_cast<int32>(Descriptor.SectionCount)
		|| OutData.bPortal != (Descriptor.Role == EWoTBSPPartRole::Portal)
		|| (!Descriptor.ProceduralMaterialSlot.IsEmpty()
			&& (OutData.Sections.Num() != 1
				|| OutData.Sections[0].MaterialSlot
					!= Descriptor.ProceduralMaterialSlot)))
	{
		return WoTWorldMaterializationStatus(
			TEXT("a BSP part stream failed strict geometry/section validation"));
	}
	return FWoTBootstrapStatus::Success();
}

FString WoTWorldClassLeaf(const FString& ClassIdentity)
{
	int32 Dot = INDEX_NONE;
	return ClassIdentity.FindLastChar(TEXT('.'), Dot)
		? ClassIdentity.Mid(Dot + 1) : ClassIdentity;
}

bool WoTWorldReject(FString* Error, const FString& Message)
{
	if (Error != nullptr)
	{
		*Error = Message;
	}
	return false;
}

bool WoTWorldReject(FString* Error, FWoTBootstrapError&& Failure)
{
	return WoTWorldReject(Error, Failure.ToString());
}

bool WoTWorldRejectCandidate(
	UWorld* World, FString* Error, const FString& Message)
{
	// A rejection here fails LoadMap, and UE's browse-failure path then
	// requests engine exit -- which in the log-less guarded build is visible
	// only as a silent death (plus a secondary teardown SIGBUS). The heartbeat
	// is the sole channel that survives that, so record WHICH package was
	// refused and why. Package name is resolved first: it must not live inside
	// a UE_LOG argument, which the guarded build compiles out unevaluated.
	const FString RejectedPackage = IsValid(World)
		? WoTWorldCanonicalPackageName(*World) : FString(TEXT("<null-world>"));
	WoTRecordBootstrapHeartbeat(TEXT("world-rejected"),
		EWoTBootstrapPhase::Ready, 0, {}, {},
		FString::Printf(TEXT("package=%s reason=%s"),
			*RejectedPackage, *Message));
	// The engine owns the loaded package/handler references. It destroys and
	// marks the complete rejected hierarchy before returning from the route.
	(void)World;
	return WoTWorldReject(Error, Message);
}

bool WoTWorldRejectCandidate(
	UWorld* World, FString* Error, FWoTBootstrapError&& Failure)
{
	return WoTWorldRejectCandidate(World, Error, Failure.ToString());
}

FString WoTWorldFrontEndReturnRejection(
	const FWoTWorldMaterializationAuthority& Authority)
{
	if (!Authority.bBootstrapSpent)
	{
		return TEXT("the bundled bootstrap world has not completed");
	}
	if (!Authority.bNormalAuthorityAdopted)
	{
		return TEXT("normal world authority has not been adopted");
	}
	if (Authority.NormalLease.IsValid()
		&& Authority.NormalLease->HasRuntimeIntegrityFailure())
	{
		// The package digest was checked by the caller. This admission is solely
		// for that authenticated bootstrap package: normal shells still require a
		// live lease below and therefore remain closed.
		return FString();
	}
	if (!Authority.NormalLease.IsValid() || !Authority.NormalLease->IsValid())
	{
		return TEXT("the adopted normal-world lease is absent or revoked");
	}
	if (Authority.bReadinessPublished)
	{
		if (!IsWoTBootstrapWorldReady())
			return TEXT("normal-world readiness publication is no longer current");
	}
	else if (!Authority.bWorldReadinessPending)
	{
		return TEXT("unpublished normal-world authorization is absent or mismatched");
	}
	if (!IsWoTPayloadBootstrapActive())
	{
		return TEXT("the authenticated payload bootstrap is inactive");
	}
	return FString();
}

void WoTWorldRecordFrontEndReturn(const TCHAR* Event,
	const FString& PackageName, EWoTWorldStartPath Path,
	const FString& Reason = FString())
{
	const FString Detail = Reason.IsEmpty()
		? FString::Printf(TEXT("package=%s path=%d"), *PackageName,
			static_cast<int32>(Path))
		: FString::Printf(TEXT("package=%s path=%d reason=%s"), *PackageName,
			static_cast<int32>(Path), *Reason);
	WoTRecordBootstrapHeartbeat(Event, EWoTBootstrapPhase::Ready,
		0, {}, {}, Detail);
}

struct FWoTRuntimeBSPProjectionStats
{
	FWoTBSPTJunctionStats TJunctions;
	double BSPMilliseconds = 0;
	uint32 PartCount = 0;
	uint32 CollisionPartCount = 0;
	uint32 SectionCount = 0;
	uint32 UnresolvedSlotCount = 0;
	uint32 FallbackLightmapPartCount = 0;
	uint32 AnimatedSurfaceCount = 0;
	TArray<FString> UnresolvedSlotNames;
	// Sections whose retail texture is NULL or a dangling import: retail
	// renders Level->DefaultTexture there, so they bind
	// the authenticated Engine.DefaultTexture and are not unresolved.
	uint32 DefaultTextureSlotCount = 0;
	// Sky child scene and AutoU/VPan (WoTRuntimeMapSky.h).
	uint32 SkyActorCount = 0;
	FString SkyActorIdentity;
	int32 SkyZone = INDEX_NONE;
	uint32 SkySlotCount = 0;
	uint32 SkySectionCount = 0;
	uint32 SkyTriangleCount = 0;
	float SkyScale = 0.0f;
	bool bSkyRestResidual = false;
	// Sky rest (BSP v6 census): census surfaces, rest triangles drawn
	// in the sky box, rest groups without a world slot.
	uint32 SkyRestSurfaceCount = 0;
	uint32 SkyRestTriangleCount = 0;
	uint32 SkyRestMissingSlots = 0;
	// Translucent/modulated sky sections drawn as ordered capture-only layers,
	// and the translucent ones among them drawn as modulate + add passes.
	uint32 SkyLayerSections = 0;
	uint32 SkyTwoPassSections = 0;
	// Opaque/masked sections that bound their texture's UE1 DetailTexture.
	uint32 DetailSectionCount = 0;
	FString SkyError;
	uint32 PannedSlotCount = 0;
	uint32 SceneTimeMaterialCount = 0;
	uint32 MalformedPanZones = 0;
	// Retail PF_Mirrored child frames (WoTRuntimeMirror.h): staged planes and
	// the world sections they took, or why none were staged.
	FWoTRuntimeMirrorStageStats Mirror;
};

/** One BSP part after the material pass: every drawable (non-FakeBackdrop)
 * section, its index in the source part, and its material instance. Sky
 * sections are only removed from the world mesh once the sky box is staged
 * (setup_childsky binds the sky slot to this same world material instance). */
struct FWoTWorldStagedPart
{
	FWoTBSPPayloadData Visible;
	TArray<int32> SourceSections;
	TArray<UMaterialInstanceDynamic*> Materials;
};

/** Visible minus the sections whose Keep flag is false (ranges were validated
 * when Visible was built from the authenticated part). */
FWoTBSPPayloadData WoTWorldKeepVisibleSections(
	const FWoTBSPPayloadData& Visible, TConstArrayView<bool> Keep,
	const TSet<uint32>* RemovedSurfaces = nullptr,
	TArray<int32>* OutKeptSections = nullptr)
{
	FWoTBSPPayloadData Kept = Visible;
	Kept.Indices.Reset();
	Kept.Sections.Reset();
	Kept.TriangleSurfaces.Reset();
	if (OutKeptSections != nullptr) OutKeptSections->Reset();
	for (int32 SectionIndex = 0; SectionIndex < Visible.Sections.Num(); ++SectionIndex)
	{
		if (!Keep.IsValidIndex(SectionIndex) || !Keep[SectionIndex]) continue;
		const FWoTBSPPayloadSection& Section = Visible.Sections[SectionIndex];
		const uint32 FirstIndex = static_cast<uint32>(Kept.Indices.Num());
		uint32 Minimum = MAX_uint32;
		uint32 Maximum = 0;
		for (uint32 Offset = 0; Offset < Section.IndexCount; Offset += 3)
		{
			const int32 Triangle = static_cast<int32>((Section.FirstIndex + Offset) / 3);
			const bool bHasProvenance = Visible.TriangleSurfaces.IsValidIndex(Triangle);
			if (bHasProvenance && RemovedSurfaces != nullptr
				&& RemovedSurfaces->Contains(Visible.TriangleSurfaces[Triangle]))
			{
				continue;
			}
			for (uint32 Corner = 0; Corner < 3; ++Corner)
			{
				const uint32 Vertex = Visible.Indices[Section.FirstIndex + Offset + Corner];
				Kept.Indices.Add(Vertex);
				Minimum = FMath::Min(Minimum, Vertex);
				Maximum = FMath::Max(Maximum, Vertex);
			}
			if (bHasProvenance)
				Kept.TriangleSurfaces.Add(Visible.TriangleSurfaces[Triangle]);
		}
		const uint32 IndexCount = static_cast<uint32>(Kept.Indices.Num()) - FirstIndex;
		if (IndexCount == 0) continue;
		FWoTBSPPayloadSection& Added = Kept.Sections.Add_GetRef(Section);
		Added.FirstIndex = FirstIndex;
		Added.IndexCount = IndexCount;
		Added.MinVertex = Minimum;
		Added.MaxVertex = Maximum;
		if (OutKeptSections != nullptr) OutKeptSections->Add(SectionIndex);
	}
	return Kept;
}

bool WoTWorldReadObjectSourceExport(
	const FWoTRuntimeVerifiedObject& Object, FString& Out);

TOptional<FWoTNeutralResourceId> WoTWorldFindConverterResource(
	EWoTNeutralResourceKind Kind, FStringView MaterialSlot,
	TMap<FString, FString>& AuthenticatedSourceExports,
	TConstArrayView<FWoTRuntimeBSPTestConverterResource> TestResources,
	FString& OutBase)
{
	OutBase.Reset();
	const FString Prefix = Kind == EWoTNeutralResourceKind::Procedural
		? TEXT("cache/procedural/") : TEXT("cache/texture/");
	const FStringView ExpectedEncoding =
		Kind == EWoTNeutralResourceKind::Procedural
			? FStringView(TEXT("ue1-fractal-inputs-v2"))
			: FStringView(TEXT("ue1-p8-mips-rgba-palette-v1"));
	const FStringView ExpectedKind =
		Kind == EWoTNeutralResourceKind::Procedural
			? FStringView(TEXT("procedural")) : FStringView(TEXT("texture"));
	TArray<FWoTNeutralResourceId> Resources;
	TArray<FWoTRuntimeBSPConverterCandidate> Candidates;
	for (const FWoTNeutralResourceId& Resource :
		FWoTRuntimeRegistry::Get().ListResources(Kind))
	{
		if (!Resource.LogicalId.StartsWith(Prefix, ESearchCase::CaseSensitive))
			continue;
		const FString Base = Resource.LogicalId.Mid(Prefix.Len());
		if (!WoTRuntimeBSPConverterSlotMatchesFolded(
			MaterialSlot, Base)) continue;
		FString SourceExport;
		if (const FString* Cached =
			AuthenticatedSourceExports.Find(Resource.LogicalId))
		{
			SourceExport = *Cached;
		}
		else
		{
			auto Read = FWoTRuntimeRegistry::Get().ReadObject(
				Resource, 256ull * 1024ull * 1024ull,
				ExpectedEncoding, ExpectedKind);
			if (Read.IsError()
				|| !WoTWorldReadObjectSourceExport(Read.GetValue(), SourceExport))
				SourceExport.Reset();
			AuthenticatedSourceExports.Add(Resource.LogicalId, SourceExport);
		}
		Resources.Add(Resource);
		Candidates.Add({Base, MoveTemp(SourceExport)});
	}
	for (const FWoTRuntimeBSPTestConverterResource& Test : TestResources)
	{
		const bool bProcedural = Kind == EWoTNeutralResourceKind::Procedural;
		if (Test.bProcedural != bProcedural
			|| !WoTRuntimeBSPConverterSlotMatchesFolded(
				MaterialSlot, Test.LogicalBase)) continue;
		Resources.Add({FString::Printf(TEXT("cache/%s/%s"),
			bProcedural ? TEXT("procedural") : TEXT("texture"),
			*Test.LogicalBase), Kind});
		Candidates.Add({Test.LogicalBase, Test.SourceExport});
	}
	const int32 Match = WoTRuntimeBSPResolveAuthenticatedConverterSlot(
		MaterialSlot, Candidates);
	if (Match == INDEX_NONE) return {};
	OutBase = WoTRuntimeBSPConverterSlotBase(Candidates[Match].SourceExport);
	return Resources[Match];
}

/** The retail object path a v4+ BSP section names: its canonical package and
 * name (the generated world-texture row) with the outer GROUPS of its exact
 * source identity - the exact cache identity of a name that collides inside
 * its package needs them (WoTRetailIdentity.h). */
FWoTRetailObjectPath WoTWorldSectionObjectPath(const FWoTBSPPayloadSection& Section)
{
	FWoTRetailObjectPath Path;
	FWoTRetailObjectPath Source;
	if (WoTParseRetailObjectPath(Section.SourceTextureIdentity, Source))
		Path.Groups = MoveTemp(Source.Groups);
	Path.Package = Section.CanonicalPackage;
	Path.Name = Section.CanonicalName;
	return Path;
}

/** Exact-first logical base of a section in one domain: the grouped exact
 * identity when the authenticated registry (or a spec's test resources) holds
 * it, else Pkg__Name. */
FString WoTWorldSectionLogicalBase(EWoTNeutralResourceKind Kind,
	const FWoTBSPPayloadSection& Section,
	TConstArrayView<FWoTNeutralResourceId> Resources,
	TConstArrayView<FWoTRuntimeBSPTestConverterResource> TestResources)
{
	const bool bProcedural = Kind == EWoTNeutralResourceKind::Procedural;
	const FString Prefix = WoTRetailLogicalIdPrefix(Kind);
	return WoTRetailResolveLogicalBase(Kind, WoTWorldSectionObjectPath(Section),
		[&Resources, &TestResources, &Prefix, bProcedural](const FString& LogicalId)
		{
			for (const FWoTNeutralResourceId& Resource : Resources)
			{
				if (Resource.LogicalId.Equals(LogicalId, ESearchCase::IgnoreCase))
					return true;
			}
			for (const FWoTRuntimeBSPTestConverterResource& Test : TestResources)
			{
				if (Test.bProcedural == bProcedural && LogicalId.Equals(
					Prefix + Test.LogicalBase, ESearchCase::IgnoreCase)) return true;
			}
			return false;
		});
}

TOptional<FWoTNeutralResourceId> WoTWorldFindIdentityResource(
	EWoTNeutralResourceKind Kind, const FWoTBSPPayloadSection& Section,
	TMap<FString, FString>& AuthenticatedSourceExports,
	TConstArrayView<FWoTRuntimeBSPTestConverterResource> TestResources)
{
	if (Section.CanonicalPackage.IsEmpty() || Section.CanonicalName.IsEmpty()
		|| Section.SourceTextureIdentity.IsEmpty()) return {};
	const TArray<FWoTNeutralResourceId> Resources =
		FWoTRuntimeRegistry::Get().ListResources(Kind);
	const FString Base = WoTWorldSectionLogicalBase(
		Kind, Section, Resources, TestResources);
	const FString Prefix=WoTRetailLogicalIdPrefix(Kind);
	const FString LogicalId=Prefix+Base;
	const FStringView ExpectedEncoding=Kind==EWoTNeutralResourceKind::Procedural
		? FStringView(TEXT("ue1-fractal-inputs-v2"))
		: FStringView(TEXT("ue1-p8-mips-rgba-palette-v1"));
	const FStringView ExpectedKind=Kind==EWoTNeutralResourceKind::Procedural
		? FStringView(TEXT("procedural")) : FStringView(TEXT("texture"));
	for (const FWoTNeutralResourceId& Resource: Resources)
	{
		if (!Resource.LogicalId.Equals(LogicalId, ESearchCase::IgnoreCase)) continue;
		FString SourceExport;
		if (const FString* Cached=AuthenticatedSourceExports.Find(Resource.LogicalId))
		{
			SourceExport=*Cached;
		}
		else
		{
			auto Read=FWoTRuntimeRegistry::Get().ReadObject(Resource,
				256ull*1024ull*1024ull,ExpectedEncoding,ExpectedKind);
			if (!Read.IsError())
				WoTWorldReadObjectSourceExport(Read.GetValue(),SourceExport);
			AuthenticatedSourceExports.Add(Resource.LogicalId,SourceExport);
		}
		// UE1 names are FNames (case-insensitive): a map may spell a package
		// differently from the package's own export table (Arena_06 Naturet).
		return WoTRetailSourceExportMatches(SourceExport, Section.SourceTextureIdentity)
			? TOptional<FWoTNeutralResourceId>(Resource) : TOptional<FWoTNeutralResourceId>();
	}
	for (const FWoTRuntimeBSPTestConverterResource& Test:TestResources)
	{
		const bool bProcedural=Kind==EWoTNeutralResourceKind::Procedural;
		if (Test.bProcedural==bProcedural
			&& Test.LogicalBase.Equals(Base, ESearchCase::IgnoreCase)
			&& WoTRetailSourceExportMatches(Test.SourceExport, Section.SourceTextureIdentity))
		{
			return FWoTNeutralResourceId{Prefix+Test.LogicalBase,Kind};
		}
	}
	return {};
}

bool WoTWorldReadObjectSourceExport(
	const FWoTRuntimeVerifiedObject& Object, FString& Out)
{
	const FWoTCanonicalJsonValue* Source =
		Object.Header().FindMember(TEXT("source"));
	const FWoTCanonicalJsonValue* Export = Source != nullptr
		? Source->FindMember(TEXT("export")) : nullptr;
	if (Export == nullptr
		|| Export->GetKind() != EWoTCanonicalJsonKind::String)
	{
		return false;
	}
	Out = Export->GetString();
	return !Out.IsEmpty();
}

FWoTBSPPayloadData WoTWorldVisibleBSPPayload(
	const FWoTBSPPayloadData& Source, const FWoTBSPPartDescriptor& Descriptor,
	TArray<FWoTRuntimeBSPSectionStyle>& Styles,
	TArray<bool>& ProceduralSections,
	TArray<TOptional<FWoTNeutralResourceId>>& AuthenticatedResources,
	TMap<FString, FString>& AuthenticatedSourceExports,
	TConstArrayView<FWoTRuntimeBSPTestConverterResource> TestResources,
	FString& Error, TArray<int32>* OutSourceSections = nullptr)
{
	if (Source.FormatVersion == 10
		&& (Source.Indices.Num() % 3 != 0
			|| Source.TriangleSurfaces.Num() != Source.Indices.Num() / 3))
	{
		Error = TEXT("a version-10 runtime BSP part has incomplete surface provenance");
		return {};
	}
	FWoTBSPPayloadData Visible = Source;
	Visible.Indices.Reset();
	Visible.Sections.Reset();
	Visible.TriangleSurfaces.Reset();
	Styles.Reset();
	ProceduralSections.Reset();
	AuthenticatedResources.Reset();
	if (OutSourceSections != nullptr) OutSourceSections->Reset();
	for (int32 SectionIndex = 0; SectionIndex < Source.Sections.Num(); ++SectionIndex)
	{
		const FWoTBSPPayloadSection& Section = Source.Sections[SectionIndex];
		const bool bProcedural = !Descriptor.ProceduralMaterialSlot.IsEmpty()
			&& Section.MaterialSlot == Descriptor.ProceduralMaterialSlot;
		FWoTRuntimeBSPSectionStyle Style;
		FString AuthenticatedBase;
		const EWoTNeutralResourceKind Kind = bProcedural
			? EWoTNeutralResourceKind::Procedural
			: EWoTNeutralResourceKind::Texture;
		const bool bHasIdentityRecord=Source.FormatVersion>=4;
		const TOptional<FWoTNeutralResourceId> AuthenticatedResource = bHasIdentityRecord
			? WoTWorldFindIdentityResource(Kind,Section,
				AuthenticatedSourceExports,TestResources)
			: WoTWorldFindConverterResource(Kind, Section.MaterialSlot,
				AuthenticatedSourceExports, TestResources, AuthenticatedBase);
		const bool bHasAuthenticatedBase = AuthenticatedResource.IsSet();
		if (bHasIdentityRecord)
		{
			const bool bNullIdentity=Section.CanonicalPackage.IsEmpty()
				&& Section.CanonicalName.IsEmpty()
				&& Section.SourceTextureIdentity.IsEmpty();
			if (!bNullIdentity && !WoTRuntimeBSPSectionStyleForIdentity(
				Section.CanonicalPackage,Section.CanonicalName,
				Section.SurfaceFlags,Style))
			{
				Style=FWoTRuntimeBSPSectionStyle();
			}
		}
		else if (bHasAuthenticatedBase
			&& !WoTRuntimeBSPSectionStyleForBase(
				Section.MaterialSlot, AuthenticatedBase, Style))
		{
			Style = FWoTRuntimeBSPSectionStyle();
		}
		const uint64 End = static_cast<uint64>(Section.FirstIndex)
			+ Section.IndexCount;
		if (End > static_cast<uint64>(Source.Indices.Num()))
		{
			Error = TEXT("a runtime BSP section range exceeds its index stream");
			return {};
		}
		FWoTBSPPayloadSection& Added = Visible.Sections.AddDefaulted_GetRef();
		Added = Section;
		Added.FirstIndex = static_cast<uint32>(Visible.Indices.Num());
		Visible.Indices.Append(Source.Indices.GetData() + Section.FirstIndex,
			static_cast<int32>(Section.IndexCount));
		if (!Source.TriangleSurfaces.IsEmpty())
		{
			const int32 FirstTriangle = static_cast<int32>(Section.FirstIndex / 3);
			const int32 TriangleCount = static_cast<int32>(Section.IndexCount / 3);
			Visible.TriangleSurfaces.Append(
				Source.TriangleSurfaces.GetData() + FirstTriangle, TriangleCount);
		}
		Styles.Add(MoveTemp(Style));
		ProceduralSections.Add(bProcedural && bHasAuthenticatedBase);
		AuthenticatedResources.Add(AuthenticatedResource);
		if (OutSourceSections != nullptr) OutSourceSections->Add(SectionIndex);
	}
	return Visible;
}

UMaterialInterface* WoTWorldRuntimeBSPMasterImpl(
	const FWoTRuntimeBSPSectionStyle& Style, bool bProcedural,
	bool* bOutSceneTime = nullptr)
{
	if (Style.bSky)
	{
		if (bOutSceneTime != nullptr) *bOutSceneTime = false;
		return LoadObject<UMaterialInterface>(nullptr, WoTRuntimeSkyWindowMasterPath);
	}
	// A procedural (fractal) section routes by its authenticated last-writer
	// blend flags exactly like a static one (Tools/import/import_map.py:
	// the retired *Flip masters are gone; fractal surfaces ride the plain
	// blend masters and the fractal subsystem swaps the live texture in).
	(void)bProcedural;
	FString Name;
	if (Style.bTwoSided)
	{
		switch (Style.Blend)
		{
		case EWoTRuntimeBSPBlend::Masked:
			Name = TEXT("M_WoT_EntryMasked_TwoSided"); break;
		case EWoTRuntimeBSPBlend::Translucent:
			Name = TEXT("M_WoT_EntryTranslucent_TwoSided"); break;
		case EWoTRuntimeBSPBlend::Modulated:
			Name = TEXT("M_WoT_EntryModulated_TwoSided"); break;
		default:
			Name = TEXT("M_WoT_EntryWorld_TwoSided"); break;
		}
	}
	else
	{
		switch (Style.Blend)
		{
		case EWoTRuntimeBSPBlend::Masked:
			Name = TEXT("M_WoT_WorldMasked"); break;
		case EWoTRuntimeBSPBlend::Translucent:
			Name = TEXT("M_WoT_EntryTranslucent"); break;
		case EWoTRuntimeBSPBlend::Modulated:
			Name = TEXT("M_WoT_EntryModulated"); break;
		default:
			Name = TEXT("M_WoT_World"); break;
		}
	}
	// The Entry-family masters pan on the WoTSceneTime scalar instead of the
	// engine Time node (Tools/import/entry_weather_materials.py:28, 110-113).
	if (bOutSceneTime != nullptr)
		*bOutSceneTime = Name.StartsWith(TEXT("M_WoT_Entry"), ESearchCase::CaseSensitive);
	return LoadObject<UMaterialInterface>(nullptr,
		*FString::Printf(TEXT("/Game/WoT/Materials/%s.%s"), *Name, *Name));
}

/** One pass of a stacked translucent sky layer: the world-slot MID's
 * parameters on a generated sky-layer master (Tools/import/
 * sky_layer_materials.py: M_WoT_EntrySkyModulate multiplies the scene by
 * M*(1-Tex), M_WoT_EntrySkyAdd adds Tex*M; WoTSceneTime pan). Null when the
 * master is not cooked. */
UMaterialInstanceDynamic* WoTWorldMakeSkyLayerMaterial(
	UMaterialInstanceDynamic& WorldSlotMaterial, AWoTRuntimeBSPWorld& RuntimeWorld,
	const TCHAR* MasterPath)
{
	UMaterialInterface* LayerMaster = LoadObject<UMaterialInterface>(nullptr, MasterPath);
	if (LayerMaster == nullptr) return nullptr;
	UMaterialInstanceDynamic* LayerMaterial =
		UMaterialInstanceDynamic::Create(LayerMaster, &RuntimeWorld);
	if (LayerMaterial == nullptr) return nullptr;
	for (const TCHAR* TextureName : {TEXT("Tex"), TEXT("Lightmap")})
	{
		UTexture* Value = nullptr;
		if (WorldSlotMaterial.GetTextureParameterValue(
			FHashedMaterialParameterInfo(TextureName), Value) && Value != nullptr)
			LayerMaterial->SetTextureParameterValue(TextureName, Value);
	}
	for (const TCHAR* ScalarName : {TEXT("PanU"), TEXT("PanV"),
		TEXT("WoTSceneTime"), TEXT("DynLightEnable")})
	{
		float Value = 0.0f;
		if (WorldSlotMaterial.GetScalarParameterValue(
			FHashedMaterialParameterInfo(ScalarName), Value))
			LayerMaterial->SetScalarParameterValue(ScalarName, Value);
	}
	RuntimeWorld.RuntimeMaterials.Add(LayerMaterial);
	if (RuntimeWorld.SceneTimeMaterials.Contains(&WorldSlotMaterial))
		RuntimeWorld.SceneTimeMaterials.Add(LayerMaterial);
	return LayerMaterial;
}

UTexture2D* WoTWorldResolveRuntimeBSPTexture(
	const FWoTRuntimeBSPSectionStyle& Style, bool bProcedural,
	const TOptional<FWoTNeutralResourceId>& AuthenticatedResource,
	UWoTPayloadSubsystem* Payloads, FStringView ExpectedSourceExport,
	FIntPoint* OutSourceSize = nullptr, bool* bOutMasked = nullptr)
{
	if (OutSourceSize != nullptr) *OutSourceSize = FIntPoint::ZeroValue;
	if (bOutMasked != nullptr) *bOutMasked = false;
	if (Style.ConverterSlotBase.IsEmpty()
		|| !AuthenticatedResource.IsSet()) return nullptr;
	const FWoTNeutralResourceId& Resource = AuthenticatedResource.GetValue();
	if (bProcedural)
	{
		auto Read = FWoTRuntimeRegistry::Get().ReadObject(
			Resource, 256ull * 1024ull * 1024ull,
			TEXT("ue1-fractal-inputs-v2"), TEXT("procedural"));
		FString SourceExport;
		if (Read.IsError()
			|| !WoTWorldReadObjectSourceExport(Read.GetValue(), SourceExport)
			|| (!ExpectedSourceExport.IsEmpty()
				? !WoTRetailSourceExportMatches(SourceExport, ExpectedSourceExport)
				: !Style.ConverterSlotBase.Equals(
					WoTRuntimeBSPConverterSlotBase(SourceExport),
					ESearchCase::IgnoreCase)))
		{
			return nullptr;
		}
		FWoTRuntimeProceduralResource Procedural;
		FWoTBootstrapStatus Loaded = Procedural.LoadFirstUse(Resource);
		FWoTFractalTextureRecord Record;
		if (Loaded.IsError() || !Procedural.BuildRecord(Record)) return nullptr;
		// The retail-authored still frame through the object's own palette,
		// named for UWoTFractalSubsystem's discovery once the world is ready.
		UTexture2D* Placeholder = WoTRuntimeProceduralPlaceholderTexture(Record);
		// import_map._fractal_frame_dims: the pan rate divides by the record
		// size (BuildRecord takes Width/Height from the record's mip 0).
		if (Placeholder != nullptr && OutSourceSize != nullptr)
			*OutSourceSize = FIntPoint(Record.Width, Record.Height);
		if (Placeholder != nullptr && bOutMasked != nullptr)
			*bOutMasked = Procedural.Parameters().bMasked;
		return Placeholder;
	}
	if (Payloads == nullptr) return nullptr;
	const TSharedPtr<const FWoTTexturePayloadData> Payload =
		Payloads->LoadTexturePayload(Resource.LogicalId);
	if (!Payload.IsValid()
		|| (!ExpectedSourceExport.IsEmpty()
			? !WoTRetailSourceExportMatches(Payload->SourceExport, ExpectedSourceExport)
			: !Style.ConverterSlotBase.Equals(
				WoTRuntimeBSPConverterSlotBase(Payload->SourceExport),
				ESearchCase::IgnoreCase)))
	{
		return nullptr;
	}
	UTexture2D* Materialized = WoTRuntimeBSPMaterializeTexture(
		*Payload, true, Style.bNoSmooth);
	// import_map.py TEXINFO w/h: the static texture size WITHOUT DrawScale.
	if (Materialized != nullptr && OutSourceSize != nullptr && !Payload->Mips.IsEmpty())
	{
		*OutSourceSize = FIntPoint(static_cast<int32>(Payload->Mips[0].Width),
			static_cast<int32>(Payload->Mips[0].Height));
	}
	if (Materialized != nullptr && bOutMasked != nullptr)
		*bOutMasked = Payload->bMasked;
	return Materialized;
}

/** Retail renders Level->DefaultTexture for a surface whose Texture is NULL or
 * whose texture import failed to load: `Surf->Texture ? ... :
 * Level->DefaultTexture` (the original renderer's world and mover passes;
 * the dev importer does the same, import_map.py:1038-1047).
 * In a v4 BSP section that is the null identity ("None"), or a canonical
 * identity with NO authenticated object in either the texture or the
 * procedural domain (Battle3_02's WOT.WaterEffects.Water.Watr_400 is in no
 * shipped package). An identity whose object IS present but carries another
 * source export is not dangling: retail loads that exact export, the cache
 * keeps only the last same-named one (Battle3_01's AesSedaiT.Ornament
 * .PaprWall1441 vs the cached SurfaceWall export), and it stays unresolved.
 * Only called after resolution failed, so the registry scan is rare. */
bool WoTWorldSectionTextureIsRetailDangling(const FWoTBSPPayloadSection& Section,
	TConstArrayView<FWoTRuntimeBSPTestConverterResource> TestResources)
{
	const bool bNullName = Section.CanonicalName.IsEmpty()
		|| Section.CanonicalName.Equals(TEXT("None"), ESearchCase::IgnoreCase);
	const bool bNullSource = Section.SourceTextureIdentity.IsEmpty()
		|| Section.SourceTextureIdentity.Equals(TEXT("None"), ESearchCase::IgnoreCase);
	if (Section.CanonicalPackage.IsEmpty() && bNullName && bNullSource) return true;
	if (Section.CanonicalPackage.IsEmpty() || bNullName || bNullSource) return false;
	// Neither the exact grouped identity nor the plain one is present in any
	// texture domain (a name colliding inside its package has only exact ones).
	const FWoTRetailObjectPath Path = WoTWorldSectionObjectPath(Section);
	const FString Bases[] = {
		WoTRetailExactLogicalBase(Path), WoTRetailPlainLogicalBase(Path)};
	for (const FWoTRuntimeBSPTestConverterResource& Test : TestResources)
	{
		for (const FString& Base : Bases)
			if (Test.LogicalBase.Equals(Base, ESearchCase::IgnoreCase)) return false;
	}
	const EWoTNeutralResourceKind Domains[] = {
		EWoTNeutralResourceKind::Texture, EWoTNeutralResourceKind::Procedural};
	for (const EWoTNeutralResourceKind Domain : Domains)
	{
		const FString Prefix = WoTRetailLogicalIdPrefix(Domain);
		for (const FWoTNeutralResourceId& Resource :
			FWoTRuntimeRegistry::Get().ListResources(Domain))
		{
			for (const FString& Base : Bases)
				if (Resource.LogicalId.Equals(Prefix + Base, ESearchCase::IgnoreCase))
					return false;
		}
	}
	return true;
}

/** The authenticated Engine.DefaultTexture (cache/texture/Engine__DefaultTexture,
 * source export "Engine.DefaultTexture") for a retail-dangling section; the
 * section's own flags pick the filter exactly as for any resolved texture.
 * A synthetic materializable test resource answers with TestStandIn. */
UTexture2D* WoTWorldResolveRetailDefaultTexture(uint32 SurfaceFlags,
	UWoTPayloadSubsystem* Payloads,
	TMap<FString, FString>& AuthenticatedSourceExports,
	TConstArrayView<FWoTRuntimeBSPTestConverterResource> TestResources,
	UTexture2D* TestStandIn, FIntPoint* OutSourceSize, bool* bOutMasked)
{
	if (bOutMasked != nullptr) *bOutMasked = false;
	FWoTBSPPayloadSection DefaultIdentity;
	DefaultIdentity.CanonicalPackage = TEXT("Engine");
	DefaultIdentity.CanonicalName = TEXT("DefaultTexture");
	DefaultIdentity.SourceTextureIdentity = TEXT("Engine.DefaultTexture");
	const TOptional<FWoTNeutralResourceId> DefaultResource =
		WoTWorldFindIdentityResource(EWoTNeutralResourceKind::Texture,
			DefaultIdentity, AuthenticatedSourceExports, TestResources);
	if (!DefaultResource.IsSet()) return nullptr;
	const FString& DefaultLogicalId = DefaultResource.GetValue().LogicalId;
	const bool bTestStandIn = TestResources.ContainsByPredicate(
		[&DefaultLogicalId](const FWoTRuntimeBSPTestConverterResource& Test)
		{
			return Test.bMaterializable && !Test.bProcedural
				&& DefaultLogicalId == TEXT("cache/texture/") + Test.LogicalBase;
		});
	if (bTestStandIn) return TestStandIn;
	FWoTRuntimeBSPSectionStyle DefaultStyle;
	if (!WoTRuntimeBSPSectionStyleForIdentity(TEXT("Engine"),
		TEXT("DefaultTexture"), SurfaceFlags, DefaultStyle)) return nullptr;
	return WoTWorldResolveRuntimeBSPTexture(DefaultStyle, false, DefaultResource,
		Payloads, FStringView(DefaultIdentity.SourceTextureIdentity), OutSourceSize,
		bOutMasked);
}

bool WoTWorldBuildRuntimeBSP(UWorld& World,
	const FWoTWorldDescriptor& Descriptor,
	TConstArrayView<FWoTBSPPayloadData> Parts,
	AWoTRuntimeBSPWorld*& OutWorld, FWoTRuntimeBSPProjectionStats& OutStats,
	FString& Error,
	TConstArrayView<FWoTRuntimeBSPTestConverterResource> TestResources = {},
	const FWoTRetailSkyWorldView* WorldView = nullptr,
	EWoTRuntimeSkyStageFault SkyFault = EWoTRuntimeSkyStageFault::None,
	const FWoTRuntimeMirrorOptions* MirrorOptions = nullptr,
	TConstArrayView<uint32> SurfaceFlags = {},
	const FWoTRuntimeInvisibleBSPCollisionData* InvisibleCollision = nullptr)
{
	OutWorld = nullptr;
	OutStats = FWoTRuntimeBSPProjectionStats();
	const double BSPStarted = FPlatformTime::Seconds();
	if (Parts.Num() != Descriptor.BSPParts.Num())
	{
		Error = TEXT("runtime BSP descriptor and decoded part counts differ");
		return false;
	}
	// One static-world candidate set finds joins across part/material boundaries.
	// These decoded parts are render geometry; repair before sky extraction,
	// mirrors, or visibility filtering. Parts remains the collision authority.
	TArray<FWoTBSPPayloadData> RenderParts;
	RenderParts.Append(Parts.GetData(), Parts.Num());
	FString RepairError;
	if (!WoTRuntimeBSPRepairRenderTJunctions(
		RenderParts, OutStats.TJunctions, RepairError))
	{
		// Repair is presentation hardening, never world admission. The pure
		// transaction leaves RenderParts untouched, so render the authenticated
		// source geometry rather than reject or publish a blank world.
		OutStats.TJunctions.bFellBackToUnrepaired = true;
		OutStats.TJunctions.bWholePayloadFallback = true;
		OutStats.TJunctions.FallbackReason = RepairError;
		UE_LOG(LogTemp, Warning, TEXT("%s: %s; using unrepaired render geometry"),
			*Descriptor.MapIdentity, *RepairError);
	}
	else if (OutStats.TJunctions.bFellBackToUnrepaired)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("%s: runtime BSP repair retained %llu source triangles; first=%s"),
			*Descriptor.MapIdentity, OutStats.TJunctions.FallbackTriangles,
			*OutStats.TJunctions.FallbackReason);
	}
	FActorSpawnParameters Parameters;
	Parameters.OverrideLevel = World.PersistentLevel;
	Parameters.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Parameters.bDeferConstruction = true;
	Parameters.ObjectFlags |= RF_Transient;
	Parameters.Name = TEXT("WoTRuntimeBSPWorld");
	AWoTRuntimeBSPWorld* RuntimeWorld = World.SpawnActor<AWoTRuntimeBSPWorld>(
		AWoTRuntimeBSPWorld::StaticClass(), FTransform::Identity, Parameters);
	if (RuntimeWorld == nullptr)
	{
		Error = TEXT("runtime BSP world actor allocation failed");
		return false;
	}
	UWoTPayloadSubsystem* Payloads = World.GetGameInstance() != nullptr
		? World.GetGameInstance()->GetSubsystem<UWoTPayloadSubsystem>() : nullptr;
	UTexture2D* NeutralGrey = WoTRuntimeBSPMaterializeNeutralGreyTexture();
	if (NeutralGrey == nullptr)
	{
		Error = TEXT("runtime BSP neutral texture allocation failed");
		return false;
	}
	RuntimeWorld->RuntimeTextures.Add(NeutralGrey);
	const FWoTBSPPayloadData* AtlasPayload = nullptr;
	for (int32 PartIndex = 0; PartIndex < Parts.Num(); ++PartIndex)
	{
		const FWoTBSPPayloadData& Candidate = Parts[PartIndex];
		const bool bHasAtlas = Candidate.LightmapWidth != 0
			|| Candidate.LightmapHeight != 0 || !Candidate.LightmapBGRA.IsEmpty();
		if (!bHasAtlas) continue;
		if (Descriptor.BSPParts[PartIndex].PartIdentity != Descriptor.MapIdentity
			|| AtlasPayload != nullptr)
		{
			Error = TEXT("runtime BSP atlas ownership is not the singular main part");
			return false;
		}
		AtlasPayload = &Candidate;
	}
	UTexture2D* SharedLightmap = AtlasPayload != nullptr
		? WoTRuntimeBSPMaterializeLightmap(*AtlasPayload)
		: WoTRuntimeBSPMaterializeNeutralLightmap();
	if (SharedLightmap == nullptr)
	{
		Error = TEXT("runtime BSP lightmap allocation failed");
		return false;
	}
	RuntimeWorld->RuntimeTextures.Add(SharedLightmap);
	if (AtlasPayload == nullptr)
		OutStats.FallbackLightmapPartCount = static_cast<uint32>(Parts.Num());
	else if (AtlasPayload->HasLightmapAnimation())
	{
		// Version-5 centered responses animate the shared atlas on the CPU.
		RuntimeWorld->SetLightmapAnimation(*AtlasPayload, SharedLightmap);
		OutStats.AnimatedSurfaceCount =
			static_cast<uint32>(RuntimeWorld->GetAnimatedSurfaceCount());
	}
	TMap<FString, FString> AuthenticatedSourceExports;

	// The sky-zone child scene (skygeo.py + setup_childsky.py) and
	// AutoU/VPan (import_map.py:1124-1131), recomputed from world-v3 + BSP-v4.
	TArray<FWoTRuntimeMapSkyPlan> SkyPlans;
	TArray<int32> SkyPlanByZone;
	FWoTRetailSkySelection SkySelection;
	if (WorldView != nullptr && World.GetNetMode() != NM_DedicatedServer)
	{
		FString SkyError;
		if (!WoTSelectRetailSky(*WorldView, SkySelection, SkyError)
			|| !WoTBuildRuntimeMapSkyPlans(
				*WorldView, RenderParts, SkyPlans, SkyPlanByZone, SkyError))
		{
			// A plan failure keeps the pre-sky world (far sky slots visible in
			// place); it is reported in the heartbeat, never silently absorbed.
			OutStats.SkyError = SkyError.Left(160);
			SkyPlans.Reset();
			SkyPlanByZone.Reset();
		}
	}
	OutStats.SkyActorCount = static_cast<uint32>(SkySelection.SkyActors.Num());
	TArray<UStaticMesh*> SkyRenderMeshes;
	SkyRenderMeshes.SetNumZeroed(SkyPlans.Num());
	for (int32 PlanIndex = 0; PlanIndex < SkyPlans.Num(); ++PlanIndex)
	{
		SkyRenderMeshes[PlanIndex] = NewObject<UStaticMesh>(RuntimeWorld,
			FName(*FString::Printf(TEXT("RuntimeSkyMesh_%02d"), PlanIndex)), RF_Transient);
		FString SkyError;
		if (SkyRenderMeshes[PlanIndex] == nullptr
			|| !WoTRuntimeBSPBuildRenderMesh(
				*SkyRenderMeshes[PlanIndex], SkyPlans[PlanIndex].SkyMesh, SkyError))
		{
			OutStats.SkyError = (SkyError.IsEmpty()
				? FString(TEXT("runtime sky mesh allocation failed")) : SkyError).Left(160);
			SkyPlans.Reset();
			SkyPlanByZone.Reset();
			SkyRenderMeshes.Reset();
			break;
		}
	}
	bool bSkyActive = !SkyPlans.IsEmpty()
		&& SkyRenderMeshes.Num() == SkyPlans.Num();
	const TMap<FString, FWoTRuntimeMapSlotPan> SlotPans = WoTRuntimeMapSlotPans(Parts);
	const FWoTRuntimeMapPanSpeeds PanSpeeds = WorldView != nullptr
		? WoTRuntimeMapBuildPanSpeeds(WorldView->Actors) : FWoTRuntimeMapPanSpeeds();
	OutStats.MalformedPanZones = PanSpeeds.MalformedZones;
	TSet<FString> PannedSlots;
	// Ten detail textures serve 637 base textures: one materialized texture
	// per authenticated detail resource and filter mode, shared by sections.
	TMap<FString, UTexture2D*> DetailTextures;

	// One material instance per materialized section; world and sky sections
	// share this path so a sky slot binds the exact world-slot material.
	auto MakeSectionMaterial = [&](const FWoTBSPPayloadData& SourcePart,
		const FWoTBSPPayloadSection& SourceSection,
		const FWoTRuntimeBSPSectionStyle& SectionStyle, bool bSectionProcedural,
		const TOptional<FWoTNeutralResourceId>& SectionResource,
		UMaterialInstanceDynamic*& OutMaterial,
		EWoTRuntimeBSPBlend& OutEffectiveBlend) -> bool
	{
		OutMaterial = nullptr;
		OutEffectiveBlend = SectionStyle.Blend;
		if (SectionStyle.bSky)
		{
			OutEffectiveBlend = EWoTRuntimeBSPBlend::Opaque;
			UMaterialInterface* Master = WoTWorldRuntimeBSPMasterImpl(
				SectionStyle, false);
			UMaterialInstanceDynamic* WindowMaterial = Master != nullptr
				? UMaterialInstanceDynamic::Create(Master, RuntimeWorld) : nullptr;
			if (WindowMaterial == nullptr)
			{
				Error = TEXT("runtime BSP sky-window material master is absent");
				return false;
			}
			RuntimeWorld->RuntimeMaterials.Add(WindowMaterial);
			RuntimeWorld->SkyWindowMaterials.Add(WindowMaterial);
			OutMaterial = WindowMaterial;
			return true;
		}
		bool bTestMaterializable = false;
		if (SectionResource.IsSet())
		{
			const FString& LogicalId = SectionResource.GetValue().LogicalId;
			bTestMaterializable = TestResources.ContainsByPredicate(
				[&LogicalId, bSectionProcedural](
					const FWoTRuntimeBSPTestConverterResource& Test)
				{
					const FString Prefix = bSectionProcedural
						? TEXT("cache/procedural/") : TEXT("cache/texture/");
					return Test.bMaterializable
						&& Test.bProcedural == bSectionProcedural
						&& LogicalId == Prefix + Test.LogicalBase;
				});
		}
		FIntPoint SourceSize = FIntPoint::ZeroValue;
		bool bTextureMasked = false;
		UTexture2D* SectionTexture = bTestMaterializable ? NeutralGrey
			: WoTWorldResolveRuntimeBSPTexture(
				SectionStyle, bSectionProcedural, SectionResource, Payloads,
				SourcePart.FormatVersion >= 4
					? FStringView(SourceSection.SourceTextureIdentity)
					: FStringView(),
				&SourceSize, &bTextureMasked);
		if (SectionTexture == nullptr && !bTestMaterializable
			&& SourcePart.FormatVersion >= 4
			&& WoTWorldSectionTextureIsRetailDangling(SourceSection, TestResources))
		{
			// Retail: Surf->Texture ? ... : Level->DefaultTexture.
			SectionTexture = WoTWorldResolveRetailDefaultTexture(
				SourceSection.SurfaceFlags, Payloads, AuthenticatedSourceExports,
				TestResources, NeutralGrey, &SourceSize, &bTextureMasked);
			if (SectionTexture != nullptr) ++OutStats.DefaultTextureSlotCount;
		}
		if (SectionTexture == nullptr)
		{
			SectionTexture = NeutralGrey;
			++OutStats.UnresolvedSlotCount;
			const FString& UnresolvedSlot = SourceSection.MaterialSlot;
			if (OutStats.UnresolvedSlotNames.Num() < 4
				&& !OutStats.UnresolvedSlotNames.Contains(UnresolvedSlot))
			{
				OutStats.UnresolvedSlotNames.Add(UnresolvedSlot.Left(96));
			}
		}
		else
		{
			RuntimeWorld->RuntimeTextures.Add(SectionTexture);
		}
		FWoTRuntimeBSPSectionStyle EffectiveStyle = SectionStyle;
		WoTRuntimeBSPApplyTextureFlags(EffectiveStyle, bTextureMasked);
		OutEffectiveBlend = EffectiveStyle.Blend;
		bool bSceneTimeMaster = false;
		UMaterialInterface* Master = WoTWorldRuntimeBSPMasterImpl(
			EffectiveStyle, bSectionProcedural, &bSceneTimeMaster);
		UMaterialInstanceDynamic* SectionMaterial = Master != nullptr
			? UMaterialInstanceDynamic::Create(Master, RuntimeWorld) : nullptr;
		if (SectionMaterial == nullptr)
		{
			Error = TEXT("runtime BSP neutral material master is absent");
			return false;
		}
		// import_map.py:1124-1131: 35*Zone.TexU/VPanSpeed texels/sec over the
		// static texture size without DrawScale, the fractal record size, or
		// 32 for the Engine.DefaultTexture substitution of a dangling/NULL
		// texture; flags and pan zone are the merged slot last writer.
		const FWoTRuntimeMapSlotPan* SlotPan = SlotPans.Find(SourceSection.MaterialSlot);
		const uint32 PanWidth = SourceSize.X > 0
			? static_cast<uint32>(SourceSize.X) : WoTRuntimeMapDanglingPanTextureSize;
		const uint32 PanHeight = SourceSize.Y > 0
			? static_cast<uint32>(SourceSize.Y) : WoTRuntimeMapDanglingPanTextureSize;
		const FVector2f PanRate = SlotPan != nullptr
			? WoTRuntimeMapPanRate(SlotPan->Flags,
				PanSpeeds.Find(SlotPan->PanZoneActorIdentity), PanWidth, PanHeight)
			: FVector2f::ZeroVector;
		SectionMaterial->SetTextureParameterValue(TEXT("Tex"), SectionTexture);
		SectionMaterial->SetTextureParameterValue(TEXT("Lightmap"), SharedLightmap);
		SectionMaterial->SetScalarParameterValue(TEXT("PanU"), PanRate.X);
		SectionMaterial->SetScalarParameterValue(TEXT("PanV"), PanRate.Y);
		SectionMaterial->SetScalarParameterValue(TEXT("WoTSceneTime"), 0.0f);
		SectionMaterial->SetScalarParameterValue(
			TEXT("DynLightEnable"),
			WoTRuntimeBSPDynamicLightEnable(EffectiveStyle));
		// UE1 MacroTexture is an additional modulated world pass. The shipped
		// world masters reproduce it with a neutral-128 default. Every opaque or
		// masked section rides M_WoT_World/M_WoT_WorldMasked or their two-sided
		// Entry clones (entry_weather_materials.py MaterialSpec copies), and a
		// procedural section routes by the same blend (WoTWorldRuntimeBSPMasterImpl).
		const bool bMacroMaster =
			EffectiveStyle.Blend == EWoTRuntimeBSPBlend::Opaque
				|| EffectiveStyle.Blend == EWoTRuntimeBSPBlend::Masked;
		const bool bHasMacro = SourcePart.FormatVersion >= 7
			&& !SourceSection.MacroCanonicalPackage.IsEmpty()
			&& !SourceSection.MacroCanonicalName.IsEmpty()
			&& !SourceSection.MacroSourceTextureIdentity.IsEmpty()
			&& SourceSection.MacroRatio > 0.0f;
		if (bMacroMaster && bHasMacro)
		{
			FWoTBSPPayloadSection MacroSection;
			MacroSection.CanonicalPackage = SourceSection.MacroCanonicalPackage;
			MacroSection.CanonicalName = SourceSection.MacroCanonicalName;
			MacroSection.SourceTextureIdentity =
				SourceSection.MacroSourceTextureIdentity;
			const TOptional<FWoTNeutralResourceId> MacroResource =
				WoTWorldFindIdentityResource(EWoTNeutralResourceKind::Texture,
					MacroSection, AuthenticatedSourceExports, TestResources);
			bool bTestMacroMaterializable = false;
			if (MacroResource.IsSet())
			{
				const FString& LogicalId = MacroResource.GetValue().LogicalId;
				bTestMacroMaterializable = TestResources.ContainsByPredicate(
					[&LogicalId](const FWoTRuntimeBSPTestConverterResource& Test)
					{
						return Test.bMaterializable && !Test.bProcedural
							&& LogicalId == TEXT("cache/texture/") + Test.LogicalBase;
					});
			}
			// Give the automation seam its own object so the test can prove that
			// MacroTex was overridden instead of observing the master's neutral
			// default (or the base section's neutral stand-in).
			UTexture2D* MacroTexture = bTestMacroMaterializable
				? WoTRuntimeBSPMaterializeNeutralGreyTexture()
				: WoTWorldResolveRuntimeBSPTexture(EffectiveStyle, false,
					MacroResource, Payloads,
					FStringView(SourceSection.MacroSourceTextureIdentity));
			if (MacroTexture != nullptr)
			{
				SectionMaterial->SetTextureParameterValue(
					TEXT("MacroTex"), MacroTexture);
				SectionMaterial->SetScalarParameterValue(
					TEXT("MacroRatio"), SourceSection.MacroRatio);
				SectionMaterial->SetScalarParameterValue(
					TEXT("MacroOffsetU"), SourceSection.MacroOffsetU);
				SectionMaterial->SetScalarParameterValue(
					TEXT("MacroOffsetV"), SourceSection.MacroOffsetV);
				RuntimeWorld->RuntimeTextures.Add(MacroTexture);
			}
		}
		// UE1 DetailTexture is a near-faded modulated pass on every non-portal
		// surface whose texture authors one (the original Glide driver:
		// FB *= 2*lerp(0.5, Detail, a), a reaching 0
		// at 200 uu). The same opaque/masked masters carry that term behind a
		// neutral default. Shipped WoT.ini selected D3D7 with
		// DetailTextures=False; the Glide look is ruled on (PORT_NOTES).
		FWoTRuntimeBSPDetailTexture Detail;
		if (bMacroMaster && (SourceSection.SurfaceFlags & WoTRuntimeMapPFPortal) == 0
			&& WoTRuntimeBSPFindDetailTexture(SourceSection.CanonicalPackage,
				SourceSection.CanonicalName, Detail))
		{
			FWoTBSPPayloadSection DetailSection;
			DetailSection.CanonicalPackage = Detail.CanonicalPackage;
			DetailSection.CanonicalName = Detail.CanonicalName;
			DetailSection.SourceTextureIdentity = Detail.SourceTextureIdentity;
			const TOptional<FWoTNeutralResourceId> DetailResource =
				WoTWorldFindIdentityResource(EWoTNeutralResourceKind::Texture,
					DetailSection, AuthenticatedSourceExports, TestResources);
			UTexture2D* DetailTexture = nullptr;
			if (DetailResource.IsSet())
			{
				const FString& LogicalId = DetailResource.GetValue().LogicalId;
				const FString CacheKey = EffectiveStyle.bNoSmooth
					? LogicalId + TEXT("|NoSmooth") : LogicalId;
				if (UTexture2D** Cached = DetailTextures.Find(CacheKey))
				{
					DetailTexture = *Cached;
				}
				else
				{
					// The automation seam gets its own stand-in, as for MacroTex.
					const bool bTestDetailMaterializable =
						TestResources.ContainsByPredicate(
							[&LogicalId](const FWoTRuntimeBSPTestConverterResource& Test)
							{
								return Test.bMaterializable && !Test.bProcedural
									&& LogicalId == TEXT("cache/texture/") + Test.LogicalBase;
							});
					DetailTexture = bTestDetailMaterializable
						? WoTRuntimeBSPMaterializeNeutralGreyTexture()
						: WoTWorldResolveRuntimeBSPTexture(EffectiveStyle, false,
							DetailResource, Payloads,
							FStringView(Detail.SourceTextureIdentity));
					DetailTextures.Add(CacheKey, DetailTexture);
					if (DetailTexture != nullptr)
						RuntimeWorld->RuntimeTextures.Add(DetailTexture);
				}
			}
			if (DetailTexture != nullptr)
			{
				SectionMaterial->SetTextureParameterValue(
					TEXT("DetailTex"), DetailTexture);
				SectionMaterial->SetScalarParameterValue(
					TEXT("DetailRatio"), Detail.RatioU);
				SectionMaterial->SetScalarParameterValue(
					TEXT("DetailRatioV"), Detail.RatioV);
				++OutStats.DetailSectionCount;
			}
		}
		RuntimeWorld->RuntimeMaterials.Add(SectionMaterial);
		if (PanRate.X != 0.0f || PanRate.Y != 0.0f)
		{
			PannedSlots.Add(SourceSection.MaterialSlot);
			if (bSceneTimeMaster) RuntimeWorld->SceneTimeMaterials.Add(SectionMaterial);
		}
		OutMaterial = SectionMaterial;
		return true;
	};

	// Pass one: every drawable section of every part gets its material instance
	// before anything is committed, so the sky can be staged (and abandoned)
	// while the world meshes are still unfiltered.
	TArray<FWoTWorldStagedPart> StagedParts;
	StagedParts.SetNum(Parts.Num());
	TMap<FWoTRuntimeMapSectionRef, UMaterialInstanceDynamic*> MaterialsBySection;
	TMap<FWoTRuntimeMapSectionRef, EWoTRuntimeBSPBlend> BlendsBySection;
	for (int32 PartIndex = 0; PartIndex < Parts.Num(); ++PartIndex)
	{
		FWoTWorldStagedPart& Staged = StagedParts[PartIndex];
		TArray<FWoTRuntimeBSPSectionStyle> Styles;
		TArray<bool> ProceduralSections;
		TArray<TOptional<FWoTNeutralResourceId>> AuthenticatedResources;
		Staged.Visible =
			WoTWorldVisibleBSPPayload(RenderParts[PartIndex], Descriptor.BSPParts[PartIndex],
				Styles, ProceduralSections, AuthenticatedResources,
				AuthenticatedSourceExports, TestResources, Error,
				&Staged.SourceSections);
		if (!Error.IsEmpty()) return false;
		for (int32 SectionIndex = 0; SectionIndex < Styles.Num(); ++SectionIndex)
		{
			UMaterialInstanceDynamic* SectionMaterial = nullptr;
			EWoTRuntimeBSPBlend EffectiveBlend = Styles[SectionIndex].Blend;
			if (!Staged.SourceSections.IsValidIndex(SectionIndex)
				|| !MakeSectionMaterial(Staged.Visible,
					Staged.Visible.Sections[SectionIndex], Styles[SectionIndex],
					ProceduralSections[SectionIndex],
					AuthenticatedResources[SectionIndex], SectionMaterial,
					EffectiveBlend))
			{
				if (Error.IsEmpty())
					Error = TEXT("a runtime BSP section lost its source index");
				return false;
			}
			Staged.Materials.Add(SectionMaterial);
			FWoTRuntimeMapSectionRef SectionRef;
			SectionRef.Part = PartIndex;
			SectionRef.Section = Staged.SourceSections[SectionIndex];
			MaterialsBySection.Add(SectionRef, SectionMaterial);
			BlendsBySection.Add(SectionRef, EffectiveBlend);
		}
		OutStats.SectionCount += Styles.Num();
	}

	// Stage the sky box completely (setup_childsky.py:404-421: SkyZoneInfo
	// rotation, derived SkyScale, the exact world-slot materials) before any
	// world mesh drops its sky sections. Any failure here is skip-don't-reject:
	// the partial actor is destroyed, the world keeps every section in place and
	// the heartbeat carries sky_error.
	if (bSkyActive)
	{
		FString SkyStageError;
		UMaterialInstanceDynamic* SkyRestBlack = nullptr;
		for (int32 PlanIndex = 0; PlanIndex < SkyPlans.Num()
			&& SkyStageError.IsEmpty(); ++PlanIndex)
		{
			const FWoTRuntimeMapSkyPlan& SkyPlan = SkyPlans[PlanIndex];
			TArray<UMaterialInstanceDynamic*> SkySlotMaterials;
			for (int32 SlotIndex = 0;
				SlotIndex < SkyPlan.SkyMaterialSources.Num(); ++SlotIndex)
			{
				const bool bInjectedMissing = PlanIndex == 0
					&& SkyFault == EWoTRuntimeSkyStageFault::MaterialBinding
					&& SlotIndex == SkyPlan.SkyMaterialSources.Num() - 1;
				if (!bInjectedMissing
					&& SkyPlan.SkySectionKinds.IsValidIndex(SlotIndex)
					&& SkyPlan.SkySectionKinds[SlotIndex]
						== EWoTRuntimeMapSkySectionKind::RestBlocker)
				{
					// All child scenes share the same neutral black rest blocker.
					if (SkyRestBlack == nullptr)
					{
						UTexture2D* Black = WoTRuntimeBSPMaterializeSolidBlackTexture();
						UMaterialInterface* Master = WoTWorldRuntimeBSPMasterImpl(
							FWoTRuntimeBSPSectionStyle(), false);
						SkyRestBlack = Black != nullptr && Master != nullptr
							? UMaterialInstanceDynamic::Create(Master, RuntimeWorld) : nullptr;
						if (SkyRestBlack != nullptr)
						{
							RuntimeWorld->RuntimeTextures.Add(Black);
							SkyRestBlack->SetTextureParameterValue(TEXT("Tex"), Black);
							SkyRestBlack->SetTextureParameterValue(TEXT("Lightmap"), SharedLightmap);
							SkyRestBlack->SetScalarParameterValue(TEXT("PanU"), 0.0f);
							SkyRestBlack->SetScalarParameterValue(TEXT("PanV"), 0.0f);
							SkyRestBlack->SetScalarParameterValue(TEXT("WoTSceneTime"), 0.0f);
							SkyRestBlack->SetScalarParameterValue(TEXT("DynLightEnable"), 0.0f);
							RuntimeWorld->RuntimeMaterials.Add(SkyRestBlack);
						}
					}
					if (SkyRestBlack == nullptr)
					{
						SkyStageError = TEXT("the sky-rest blocker material is unavailable");
						break;
					}
					SkySlotMaterials.Add(SkyRestBlack);
					continue;
				}
				UMaterialInstanceDynamic* const* SlotMaterial = bInjectedMissing
					? nullptr : MaterialsBySection.Find(
						SkyPlan.SkyMaterialSources[SlotIndex]);
				if (SlotMaterial == nullptr || *SlotMaterial == nullptr)
				{
					SkyStageError = TEXT("a runtime sky section has no world-slot material");
					break;
				}
				SkySlotMaterials.Add(*SlotMaterial);
			}
			// Retail draws the child frame's non-opaque surfaces back to front.
			// Translucent and modulated sky sections leave the sky mesh for
			// capture-only layers ordered far to near (WoTRuntimeMapSky.h).
			TArray<bool> TranslucentSky;
			TArray<bool> LayerSky;
			TranslucentSky.Init(false, SkySlotMaterials.Num());
			LayerSky.Init(false, SkySlotMaterials.Num());
			if (SkyStageError.IsEmpty())
			{
				for (int32 SlotIndex = 0; SlotIndex < SkySlotMaterials.Num(); ++SlotIndex)
				{
					const EWoTRuntimeBSPBlend* SlotBlend =
						SkyPlan.SkyMaterialSources.IsValidIndex(SlotIndex)
							? BlendsBySection.Find(SkyPlan.SkyMaterialSources[SlotIndex])
							: nullptr;
					TranslucentSky[SlotIndex] = SlotBlend != nullptr
						&& *SlotBlend == EWoTRuntimeBSPBlend::Translucent;
					LayerSky[SlotIndex] = TranslucentSky[SlotIndex]
						|| (SlotBlend != nullptr
							&& *SlotBlend == EWoTRuntimeBSPBlend::Modulated);
				}
			}
			const TArray<int32> LayerOrder =
				WoTRuntimeMapSkyLayerOrder(SkyPlan.SkyMesh, LayerSky);
			TArray<int32> MainSections;
			for (int32 SlotIndex = 0; SlotIndex < SkySlotMaterials.Num(); ++SlotIndex)
				if (!LayerSky[SlotIndex]) MainSections.Add(SlotIndex);
			AWoTSkyBox* SkyBox = nullptr;
			UStaticMeshComponent* SkyComponent = nullptr;
			if (SkyStageError.IsEmpty()
				&& !(PlanIndex == 0
					&& SkyFault == EWoTRuntimeSkyStageFault::SkyBoxAllocation))
			{
				FActorSpawnParameters SkyParameters;
				SkyParameters.OverrideLevel = World.PersistentLevel;
				SkyParameters.SpawnCollisionHandlingOverride =
					ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
				SkyParameters.bDeferConstruction = true;
				SkyParameters.ObjectFlags |= RF_Transient;
				SkyParameters.Name = FName(*FString::Printf(
					TEXT("WoTRuntimeSkyBox_%02d"), PlanIndex));
				const FTransform SkyTransform(SkyPlan.Rotation, FVector::ZeroVector,
					FVector(SkyPlan.Scale));
				SkyBox = World.SpawnActor<AWoTSkyBox>(
					AWoTSkyBox::StaticClass(), SkyTransform, SkyParameters);
				SkyComponent = SkyBox != nullptr
					? SkyBox->GetStaticMeshComponent() : nullptr;
			}
			if (!SkyStageError.IsEmpty())
			{
				break;
			}
			if (SkyComponent == nullptr)
			{
				if (SkyBox != nullptr) SkyBox->Destroy();
				SkyStageError = TEXT("runtime sky box allocation failed");
				break;
			}
			SkyBox->SkyRotation = SkyPlan.Rotation;
			SkyBox->SkyScale = SkyPlan.Scale;
			// A translucent layer is two fixed-function passes, modulate by
			// M*(1-Tex) then add Tex*M, which stack in draw order; the former
			// SceneColor material overwrote whatever layer lay behind it.
			UStaticMesh* MainSkyMesh = SkyRenderMeshes[PlanIndex];
			if (!LayerOrder.IsEmpty())
			{
				MainSkyMesh = nullptr;
				if (!MainSections.IsEmpty())
				{
					FWoTBSPPayloadData MainPayload;
					MainSkyMesh = NewObject<UStaticMesh>(RuntimeWorld, FName(*FString::Printf(
						TEXT("RuntimeSkyMesh_%02d_Opaque"), PlanIndex)), RF_Transient);
					if (MainSkyMesh == nullptr
						|| !WoTRuntimeMapSkySubMesh(SkyPlan.SkyMesh, MainSections, MainPayload)
						|| !WoTRuntimeBSPBuildRenderMesh(*MainSkyMesh, MainPayload, SkyStageError))
					{
						if (SkyStageError.IsEmpty())
							SkyStageError = TEXT("the opaque runtime sky mesh could not be built");
					}
				}
			}
			for (int32 Rank = 0; Rank < LayerOrder.Num() && SkyStageError.IsEmpty(); ++Rank)
			{
				const int32 SectionIndex = LayerOrder[Rank];
				FWoTBSPPayloadData LayerPayload;
				UStaticMesh* LayerMesh = NewObject<UStaticMesh>(RuntimeWorld, FName(*FString::Printf(
					TEXT("RuntimeSkyLayer_%02d_%02d"), PlanIndex, Rank)), RF_Transient);
				if (LayerMesh == nullptr
					|| !WoTRuntimeMapSkySubMesh(SkyPlan.SkyMesh,
						MakeArrayView(&SectionIndex, 1), LayerPayload)
					|| !WoTRuntimeBSPBuildRenderMesh(*LayerMesh, LayerPayload, SkyStageError))
				{
					if (SkyStageError.IsEmpty())
						SkyStageError = TEXT("a runtime sky layer mesh could not be built");
					break;
				}
				UMaterialInstanceDynamic* SlotMaterial = SkySlotMaterials[SectionIndex];
				UMaterialInstanceDynamic* Modulate = TranslucentSky[SectionIndex]
					? WoTWorldMakeSkyLayerMaterial(
						*SlotMaterial, *RuntimeWorld, WoTRuntimeSkyModulateMasterPath)
					: nullptr;
				UMaterialInstanceDynamic* Add = Modulate != nullptr
					? WoTWorldMakeSkyLayerMaterial(
						*SlotMaterial, *RuntimeWorld, WoTRuntimeSkyAddMasterPath)
					: nullptr;
				const bool bTwoPass = Modulate != nullptr && Add != nullptr;
				// A closure without the generated masters keeps the slot's
				// Entry-family material (the max3 approximation), still ordered.
				const bool bAdded = bTwoPass
					? SkyBox->AddSkyLayer(LayerMesh, Modulate, 2 * Rank) != nullptr
						&& SkyBox->AddSkyLayer(LayerMesh, Add, 2 * Rank + 1) != nullptr
					: SkyBox->AddSkyLayer(LayerMesh, SlotMaterial, 2 * Rank) != nullptr;
				if (!bAdded)
				{
					SkyStageError = TEXT("a runtime sky layer component could not be created");
					break;
				}
				++OutStats.SkyLayerSections;
				OutStats.SkyTwoPassSections += bTwoPass ? 1u : 0u;
			}
			if (!SkyStageError.IsEmpty())
			{
				SkyBox->Destroy();
				break;
			}
			SkyComponent->SetStaticMesh(MainSkyMesh);
			for (int32 Slot = 0; Slot < MainSections.Num(); ++Slot)
				SkyComponent->SetMaterial(Slot, SkySlotMaterials[MainSections[Slot]]);
			if (!SkyBox->InitializeSkyWindowCapture())
			{
				SkyBox->Destroy();
				SkyStageError = TEXT("runtime sky-window cube capture allocation failed");
				break;
			}
			SkyBox->UnregisterAllComponents();
			RuntimeWorld->SkyBoxes.Add(SkyBox);
			if (PlanIndex == 0)
			{
				RuntimeWorld->SkyBox = SkyBox;
				OutStats.SkyActorIdentity = SkyPlan.SkyActorIdentity;
				OutStats.SkyZone = SkyPlan.Selection.Zone;
				OutStats.SkyScale = SkyPlan.Scale;
			}
			OutStats.SkySlotCount += static_cast<uint32>(SkyPlan.SkySlots.Num());
			OutStats.SkyTriangleCount += SkyPlan.SkyTriangleCount;
			OutStats.bSkyRestResidual |= SkyPlan.Selection.bTraversablePortal
				&& !SkyPlan.bSkyRestCensus;
			OutStats.SkyRestSurfaceCount += SkyPlan.SkyRestSurfaceCount;
			OutStats.SkyRestTriangleCount += SkyPlan.SkyRestTriangleCount;
			OutStats.SkyRestMissingSlots += SkyPlan.SkyRestMissingSlots;
		}
		if (!SkyStageError.IsEmpty())
		{
			for (AWoTSkyBox* SkyBox : RuntimeWorld->SkyBoxes)
				if (SkyBox != nullptr) SkyBox->Destroy();
			RuntimeWorld->SkyBoxes.Reset();
			RuntimeWorld->SkyBox = nullptr;
			OutStats.SkyError = SkyStageError;
			bSkyActive = false;
			SkyRenderMeshes.Reset();
		}
		else
		{
			RuntimeWorld->SkyPlanByZone = SkyPlanByZone;
			if (WorldView != nullptr)
				RuntimeWorld->SkyZoneNodes.Append(WorldView->Nodes.GetData(),
					WorldView->Nodes.Num());
			// Plan zero is the authenticated majority fallback until a local view
			// supplies a BSP zone. Every child scene keeps capturing independently.
			RuntimeWorld->BindSkyWindowsToPlan(0);
		}
	}

	// Keep masks. Only a staged sky removes its sections from the world render
	// mesh (setup_childsky.py:423-452 hides every sky slot on world meshes);
	// staged mirrors then take theirs onto their own surfaces. Collision always
	// cooks from the complete part.
	TArray<TArray<bool>> KeepInWorldByPart;
	TArray<uint32> MovedToSkyByPart;
	TSet<uint32> SkySurfaceIndices;
	if (bSkyActive)
	{
		for (const FWoTRuntimeMapSkyPlan& Plan : SkyPlans)
			for (const int32 Surface : Plan.Selection.Surfaces)
				if (Surface >= 0
					&& (!SurfaceFlags.IsValidIndex(Surface)
						|| (SurfaceFlags[Surface] & WoTRuntimeMapPFFakeBackdrop) == 0))
					SkySurfaceIndices.Add(static_cast<uint32>(Surface));
	}
	KeepInWorldByPart.SetNum(Parts.Num());
	MovedToSkyByPart.Init(0u, Parts.Num());
	for (int32 PartIndex = 0; PartIndex < Parts.Num(); ++PartIndex)
	{
		const FWoTWorldStagedPart& Staged = StagedParts[PartIndex];
		TArray<bool>& KeepInWorld = KeepInWorldByPart[PartIndex];
		KeepInWorld.Init(true, Staged.Visible.Sections.Num());
		if (bSkyActive)
		{
			for (int32 SectionIndex = 0; SectionIndex < KeepInWorld.Num(); ++SectionIndex)
			{
				const int32 SourceSection = Staged.SourceSections[SectionIndex];
				const bool bWholeSectionSky = SkyPlans.ContainsByPredicate(
					[PartIndex, SourceSection](const FWoTRuntimeMapSkyPlan& Plan)
					{
						return Plan.SkySections.IsValidIndex(PartIndex)
							&& Plan.SkySections[PartIndex].IsValidIndex(SourceSection)
							&& Plan.SkySections[PartIndex][SourceSection];
					});
				if (bWholeSectionSky)
				{
					KeepInWorld[SectionIndex] = false;
					++MovedToSkyByPart[PartIndex];
				}
			}
		}
	}

	// Retail PF_Mirrored child frames: client-only,
	// skip-not-fail (a skip keeps every section in the world as before).
	{
		TArray<FWoTRuntimeMirrorPartInput> MirrorParts;
		MirrorParts.SetNum(Parts.Num());
		for (int32 PartIndex = 0; PartIndex < Parts.Num(); ++PartIndex)
		{
			MirrorParts[PartIndex].Visible = &StagedParts[PartIndex].Visible;
			MirrorParts[PartIndex].Materials = StagedParts[PartIndex].Materials;
			MirrorParts[PartIndex].KeepInWorld = &KeepInWorldByPart[PartIndex];
		}
		WoTStageRuntimeMirrors(*RuntimeWorld, MirrorParts,
			MirrorOptions != nullptr ? *MirrorOptions : FWoTRuntimeMirrorOptions(),
			OutStats.Mirror);
	}

	// Pass two: commit each part.
	for (int32 PartIndex = 0; PartIndex < Parts.Num(); ++PartIndex)
	{
		const FWoTBSPPayloadData& Part = Parts[PartIndex];
		const FWoTWorldStagedPart& Staged = StagedParts[PartIndex];
		const TArray<bool>& KeepInWorld = KeepInWorldByPart[PartIndex];
		const uint32 MovedToSky = MovedToSkyByPart[PartIndex];
		const bool bFilterSurfaces = bSkyActive && !SkySurfaceIndices.IsEmpty()
			&& Staged.Visible.TriangleSurfaces.Num()
				== Staged.Visible.Indices.Num() / 3;
		const bool bFiltered = KeepInWorld.Contains(false) || bFilterSurfaces;
		FWoTBSPPayloadData Filtered;
		TArray<int32> RenderSectionSources;
		if (bFiltered)
			Filtered = WoTWorldKeepVisibleSections(Staged.Visible, KeepInWorld,
				bFilterSurfaces ? &SkySurfaceIndices : nullptr,
				&RenderSectionSources);
		else
			for (int32 SectionIndex = 0; SectionIndex < Staged.Materials.Num(); ++SectionIndex)
				RenderSectionSources.Add(SectionIndex);
		const FWoTBSPPayloadData& Visible = bFiltered ? Filtered : Staged.Visible;
		const bool bHasVisibleSections =
			!Visible.Sections.IsEmpty() && !Visible.Indices.IsEmpty();
		const FWoTBSPPayloadData& RenderPayload =
			bHasVisibleSections ? Visible : RenderParts[PartIndex];
		UStaticMesh* Mesh = NewObject<UStaticMesh>(RuntimeWorld,
			FName(*FString::Printf(TEXT("RuntimeBSPMesh_%03d"), PartIndex)),
			RF_Transient);
		if (Mesh == nullptr
			|| !WoTRuntimeBSPBuildRenderMesh(*Mesh, RenderPayload, Error)
			|| !WoTRuntimeBSPBuildCollision(*Mesh, Part, Error, SurfaceFlags))
		{
			if (Error.IsEmpty()) Error = TEXT("runtime BSP mesh allocation failed");
			return false;
		}
		UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>(
			RuntimeWorld, FName(*FString::Printf(
				TEXT("RuntimeBSPComponent_%03d"), PartIndex)), RF_Transient);
		if (Component == nullptr)
		{
			Error = TEXT("runtime BSP component allocation failed");
			return false;
		}
		RuntimeWorld->AddInstanceComponent(Component);
		RuntimeWorld->AddOwnedComponent(Component);
		Component->SetupAttachment(RuntimeWorld->GetRootComponent());
		Component->SetMobility(EComponentMobility::Static);
		Component->SetStaticMesh(Mesh);
		if (!bHasVisibleSections)
		{
			Component->SetVisibility(false, true);
			Component->SetHiddenInGame(true, true);
		}
		if (Visible.bPortal)
		{
			Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Component->SetGenerateOverlapEvents(false);
		}
		else
		{
			Component->SetCollisionProfileName(TEXT("BlockAll"));
			Component->SetCollisionObjectType(ECC_WorldStatic);
			++OutStats.CollisionPartCount;
		}
		for (int32 WorldSlot = 0; WorldSlot < RenderSectionSources.Num(); ++WorldSlot)
			Component->SetMaterial(WorldSlot,
				Staged.Materials[RenderSectionSources[WorldSlot]]);
		RuntimeWorld->GeometryComponents.Add(Component);
		RuntimeWorld->RuntimeMeshes.Add(Mesh);
		++OutStats.PartCount;
		OutStats.SkySectionCount += MovedToSky;
	}
	if (InvisibleCollision != nullptr
		&& !InvisibleCollision->Payload.Indices.IsEmpty())
	{
		const int32 TriangleCount = InvisibleCollision->Payload.Indices.Num() / 3;
		if (InvisibleCollision->Payload.Indices.Num() % 3 != 0
			|| InvisibleCollision->Payload.TriangleSurfaces.Num() != TriangleCount
			|| InvisibleCollision->TriangleTextureIdentities.Num() != TriangleCount)
		{
			Error = TEXT("runtime invisible-solid collision provenance is malformed");
			return false;
		}
		UStaticMesh* CollisionMesh = NewObject<UStaticMesh>(RuntimeWorld,
			TEXT("RuntimeBSPInvisibleSolidCollisionMesh"), RF_Transient);
		if (CollisionMesh == nullptr || !WoTRuntimeBSPBuildCollision(
			*CollisionMesh, InvisibleCollision->Payload, Error))
		{
			if (Error.IsEmpty())
				Error = TEXT("runtime invisible-solid collision mesh allocation failed");
			return false;
		}
		UStaticMeshComponent* CollisionComponent =
			NewObject<UStaticMeshComponent>(RuntimeWorld,
				TEXT("RuntimeBSPInvisibleSolidCollision"), RF_Transient);
		if (CollisionComponent == nullptr)
		{
			Error = TEXT("runtime invisible-solid collision component allocation failed");
			return false;
		}
		RuntimeWorld->AddInstanceComponent(CollisionComponent);
		RuntimeWorld->AddOwnedComponent(CollisionComponent);
		CollisionComponent->SetupAttachment(RuntimeWorld->GetRootComponent());
		CollisionComponent->SetMobility(EComponentMobility::Static);
		CollisionComponent->SetStaticMesh(CollisionMesh);
		CollisionComponent->SetVisibility(false, true);
		CollisionComponent->SetHiddenInGame(true, true);
		CollisionComponent->SetCollisionProfileName(TEXT("BlockAll"));
		CollisionComponent->SetCollisionObjectType(ECC_WorldStatic);
		RuntimeWorld->InvisibleSolidCollisionComponent = CollisionComponent;
		RuntimeWorld->InvisibleSolidCollisionMesh = CollisionMesh;
		RuntimeWorld->InvisibleSolidCollisionTriangleSurfaces =
			InvisibleCollision->Payload.TriangleSurfaces;
		RuntimeWorld->InvisibleSolidCollisionTriangleTextureIdentities =
			InvisibleCollision->TriangleTextureIdentities;
	}
	if (bSkyActive)
		for (UStaticMesh* SkyRenderMesh : SkyRenderMeshes)
			RuntimeWorld->RuntimeMeshes.Add(SkyRenderMesh);
	OutStats.PannedSlotCount = static_cast<uint32>(PannedSlots.Num());
	OutStats.SceneTimeMaterialCount =
		static_cast<uint32>(RuntimeWorld->SceneTimeMaterials.Num());
	// Light-map animation (adopted above) or scene-time panning: one predicate.
	RuntimeWorld->RefreshTickEnabled();
	// Dev maps carry two unbound volumes that guarded shell worlds lack: the
	// GammaBoundary PPV (Tools/import/import_map.py:4594-4603) and the
	// WoTGameplayLook PPV (Tools/import/import_gameplay_look.py:33-62). The
	// world masters emit display-space colour (GAMMA_SPACE); without the
	// boundary's sRGB decode the output encode runs twice, lifting the dark
	// retail lightmaps into a flat, near-fullbright wash. Same settings as the
	// entry camera (Source/WoT/FrontEnd/WoTFrontEndSceneMaterializer.cpp,
	// CommitStaged camera post-process).
	UMaterialInterface* GammaBoundary = LoadObject<UMaterialInterface>(nullptr,
		TEXT("/Game/WoT/Materials/Instances/MI_GammaBoundary.MI_GammaBoundary"));
	if (GammaBoundary == nullptr)
	{
		Error = TEXT("runtime world neutral gamma boundary is absent");
		return false;
	}
	UPostProcessComponent* PeriodLook = NewObject<UPostProcessComponent>(
		RuntimeWorld, TEXT("RuntimeWorldPeriodLook"), RF_Transient);
	if (PeriodLook == nullptr)
	{
		Error = TEXT("runtime world post-process allocation failed");
		return false;
	}
	PeriodLook->bUnbound = true;
	PeriodLook->bEnabled = true;
	PeriodLook->Priority = 1.0f;
	PeriodLook->BlendWeight = 1.0f;
	PeriodLook->Settings.bOverride_ToneCurveAmount = true;
	PeriodLook->Settings.ToneCurveAmount = 0.0f;
	PeriodLook->Settings.bOverride_ExpandGamut = true;
	PeriodLook->Settings.ExpandGamut = 0.0f;
	PeriodLook->Settings.WeightedBlendables.Array.Add(
		FWeightedBlendable(1.0f, GammaBoundary));
	RuntimeWorld->AddInstanceComponent(PeriodLook);
	RuntimeWorld->AddOwnedComponent(PeriodLook);
	PeriodLook->SetupAttachment(RuntimeWorld->GetRootComponent());
	RuntimeWorld->PeriodLook = PeriodLook;
	// SpawnActor registers native defaults even in a pre-InitWorld candidate,
	// which marks the actor complete and would make UpdateWorldComponents skip
	// the NewObject geometry. Clear that pre-scene registration state only
	// after the complete component graph exists; the engine then registers the
	// root and every owned geometry component after render/physics InitWorld.
	RuntimeWorld->UnregisterAllComponents();
	OutWorld = RuntimeWorld;
	OutStats.BSPMilliseconds = (FPlatformTime::Seconds() - BSPStarted) * 1000.0;
	return true;
}
}

bool WoTWorldBuildInvisibleSolidBSPCollision(
	const FWoTRuntimeBSPCollisionGeometryView& Geometry,
	FWoTRuntimeInvisibleBSPCollisionData& Out, FString& Error)
{
	Out = FWoTRuntimeInvisibleBSPCollisionData();
	Error.Reset();
	Out.Payload.FormatVersion = 10;
	Out.Payload.bPortal = false;

	for (const FWoTRuntimeWorldSurface& Surface : Geometry.Surfaces)
	{
		if ((Surface.Flags & WoTRuntimeMapPFPortal) != 0
			&& (Surface.Flags & WoTRuntimeMapPFNotSolid) == 0)
		{
			++Out.PortalWithoutNotSolidSurfaceCount;
		}
	}

	auto OrderedDot = [](const FVector3d& A, const FVector3d& B)
	{
		double Result = A.X * B.X;
		Result += A.Y * B.Y;
		Result += A.Z * B.Z;
		return Result;
	};
	auto TriangleAreaSquared = [](const FVector3f& A, const FVector3f& B,
		const FVector3f& C)
	{
		const double UX = static_cast<double>(B.X) - A.X;
		const double UY = static_cast<double>(B.Y) - A.Y;
		const double UZ = static_cast<double>(B.Z) - A.Z;
		const double VX = static_cast<double>(C.X) - A.X;
		const double VY = static_cast<double>(C.Y) - A.Y;
		const double VZ = static_cast<double>(C.Z) - A.Z;
		const double X = UY * VZ - UZ * VY;
		const double Y = UZ * VX - UX * VZ;
		const double Z = UX * VY - UY * VX;
		return X * X + Y * Y + Z * Z;
	};
	auto QuantizePosition = [](float Value)
	{
		const double Metres = static_cast<double>(Value) / 100.0;
		volatile float GLBValue = static_cast<float>(Metres);
		return static_cast<float>(static_cast<double>(GLBValue) * 100.0);
	};
	auto Fail = [&Error](const TCHAR* Message, int32 NodeIndex)
	{
		Error = FString::Printf(TEXT("%s (node %d)"), Message, NodeIndex);
		return false;
	};

	TSet<uint32> CollisionSurfaces;
	for (int32 NodeIndex = 0; NodeIndex < Geometry.Nodes.Num(); ++NodeIndex)
	{
		const FWoTRuntimeWorldNode& Node = Geometry.Nodes[NodeIndex];
		if (!Geometry.Surfaces.IsValidIndex(Node.Surface))
			return Fail(TEXT("runtime BSP node surface index is out of range"), NodeIndex);
		const FWoTRuntimeWorldSurface& Surface = Geometry.Surfaces[Node.Surface];
		if (Node.VertexCount < 3
			|| (Surface.Flags & WoTRuntimeMapPFInvisible) == 0
			|| (Surface.Flags & WoTRuntimeMapPFNotSolid) != 0
			|| (Surface.Flags & WoTRuntimeMapPFPortal) != 0)
		{
			continue;
		}
		CollisionSurfaces.Add(static_cast<uint32>(Node.Surface));
		const int64 VertexEnd = static_cast<int64>(Node.VertexPool) + Node.VertexCount;
		if (Node.VertexPool < 0 || VertexEnd > Geometry.Vertices.Num())
			return Fail(TEXT("runtime BSP node vertex range is out of bounds"), NodeIndex);

		if (!Geometry.Vectors.IsValidIndex(Surface.Normal))
			return Fail(TEXT("runtime BSP collision normal index is out of range"), NodeIndex);
		const FVector3f& SurfaceNormal = Geometry.Vectors[Surface.Normal];
		const FVector3d Normal(SurfaceNormal);
		const double NormalSquared = OrderedDot(Normal, Normal);
		if (!FMath::IsFinite(NormalSquared)
			|| NormalSquared < 0.98 || NormalSquared > 1.02)
		{
			return Fail(TEXT("runtime BSP collision normal is not finite/unit length"),
				NodeIndex);
		}
		const FVector3f NormalFloat = SurfaceNormal;
		TArray<FVector3f> Polygon;
		Polygon.Reserve(Node.VertexCount);
		for (int32 Corner = 0; Corner < Node.VertexCount; ++Corner)
		{
			const FWoTRuntimeWorldVertex& Vertex =
				Geometry.Vertices[Node.VertexPool + Corner];
			if (!Geometry.Points.IsValidIndex(Vertex.Point))
				return Fail(TEXT("runtime BSP collision point index is out of range"), NodeIndex);
			const FVector3f Point = Geometry.Points[Vertex.Point];
			if (!FMath::IsFinite(Point.X) || !FMath::IsFinite(Point.Y)
				|| !FMath::IsFinite(Point.Z))
			{
				return Fail(TEXT("runtime BSP collision point is not finite"), NodeIndex);
			}
			Polygon.Add(Point);
		}

		FVector3d Newell = FVector3d::ZeroVector;
		for (int32 Corner = 0; Corner < Polygon.Num(); ++Corner)
		{
			const FVector3f& A = Polygon[Corner];
			const FVector3f& B = Polygon[(Corner + 1) % Polygon.Num()];
			Newell.X += (static_cast<double>(A.Y) - B.Y) * (A.Z + B.Z);
			Newell.Y += (static_cast<double>(A.Z) - B.Z) * (A.X + B.X);
			Newell.Z += (static_cast<double>(A.X) - B.X) * (A.Y + B.Y);
		}
		if (OrderedDot(Newell, Normal) < 0.0) Algo::Reverse(Polygon);

		for (int32 Corner = 1; Corner + 1 < Polygon.Num(); ++Corner)
		{
			const int32 Order[3] = {Corner + 1, Corner, 0};
			if (TriangleAreaSquared(Polygon[Order[0]], Polygon[Order[1]],
				Polygon[Order[2]]) < 0.001)
			{
				continue;
			}
			for (int32 TriangleCorner = 0; TriangleCorner < 3; ++TriangleCorner)
			{
				const FVector3f& Point = Polygon[Order[TriangleCorner]];
				const FVector3f Quantized(QuantizePosition(Point.X),
					QuantizePosition(Point.Y), QuantizePosition(Point.Z));
				const uint32 VertexIndex =
					static_cast<uint32>(Out.Payload.Positions.Num());
				Out.Payload.Positions.Add(Quantized);
				Out.Payload.Normals.Add(NormalFloat);
				Out.Payload.UV0.Add(FVector2f::ZeroVector);
				Out.Payload.UV1.Add(FVector2f::ZeroVector);
				Out.Payload.Indices.Add(VertexIndex);
				if (VertexIndex == 0)
				{
					Out.Payload.BoundsMin = Quantized;
					Out.Payload.BoundsMax = Quantized;
				}
				else
				{
					Out.Payload.BoundsMin.X = FMath::Min(
						Out.Payload.BoundsMin.X, Quantized.X);
					Out.Payload.BoundsMin.Y = FMath::Min(
						Out.Payload.BoundsMin.Y, Quantized.Y);
					Out.Payload.BoundsMin.Z = FMath::Min(
						Out.Payload.BoundsMin.Z, Quantized.Z);
					Out.Payload.BoundsMax.X = FMath::Max(
						Out.Payload.BoundsMax.X, Quantized.X);
					Out.Payload.BoundsMax.Y = FMath::Max(
						Out.Payload.BoundsMax.Y, Quantized.Y);
					Out.Payload.BoundsMax.Z = FMath::Max(
						Out.Payload.BoundsMax.Z, Quantized.Z);
				}
			}
			Out.Payload.TriangleSurfaces.Add(static_cast<uint32>(Node.Surface));
			Out.TriangleTextureIdentities.Add(Surface.TextureIdentity);
		}
	}
	Out.SurfaceIndices = CollisionSurfaces.Array();
	Out.SurfaceIndices.Sort();
	if (!Out.Payload.Indices.IsEmpty())
	{
		FWoTBSPPayloadSection& Section = Out.Payload.Sections.AddDefaulted_GetRef();
		Section.MaterialSlot = TEXT("__InvisibleSolidCollision");
		Section.FirstIndex = 0;
		Section.IndexCount = static_cast<uint32>(Out.Payload.Indices.Num());
		Section.MinVertex = 0;
		Section.MaxVertex = static_cast<uint32>(Out.Payload.Positions.Num() - 1);
		Section.bCollisionEnabled = true;
	}
	return true;
}

UMaterialInterface* WoTWorldRuntimeBSPMaster(
	const FWoTRuntimeBSPSectionStyle& Style, bool bProcedural,
	bool* bOutSceneTime)
{
	return WoTWorldRuntimeBSPMasterImpl(Style, bProcedural, bOutSceneTime);
}

float WoTRuntimeBSPDynamicLightEnable(
	const FWoTRuntimeBSPSectionStyle& Style)
{
	return Style.bSpecialLit || Style.bUnlit ? 0.0f : 1.0f;
}

#if WITH_AUTOMATION_TESTS
bool WoTWorldBuildRuntimeBSPForTesting(UWorld& World,
	const FWoTWorldDescriptor& Descriptor,
	TConstArrayView<FWoTBSPPayloadData> Parts,
	AWoTRuntimeBSPWorld*& OutWorld, uint32& OutFallbackLightmapPartCount,
	FString& Error,
	TConstArrayView<FWoTRuntimeBSPTestConverterResource> AuthenticatedResources,
	uint32* OutUnresolvedSlotCount,
	TArray<FString>* OutUnresolvedSlotNames,
	const FWoTRetailSkyWorldView* WorldView,
	FString* OutSkyError,
	EWoTRuntimeSkyStageFault SkyFault,
	uint32* OutDefaultTextureSlotCount,
	TConstArrayView<uint32> SurfaceFlags,
	const FWoTRuntimeInvisibleBSPCollisionData* InvisibleCollision,
	FWoTBSPTJunctionStats* OutTJunctionStats, double* OutBSPMilliseconds)
{
	FWoTRuntimeBSPProjectionStats Stats;
	const bool bBuilt = WoTWorldBuildRuntimeBSP(
		World, Descriptor, Parts, OutWorld, Stats, Error,
		AuthenticatedResources, WorldView, SkyFault, nullptr, SurfaceFlags,
		InvisibleCollision);
	if (OutTJunctionStats) *OutTJunctionStats = Stats.TJunctions;
	if (OutBSPMilliseconds) *OutBSPMilliseconds = Stats.BSPMilliseconds;
	if (OutSkyError != nullptr) *OutSkyError = Stats.SkyError;
	OutFallbackLightmapPartCount = Stats.FallbackLightmapPartCount;
	if (OutUnresolvedSlotCount != nullptr)
		*OutUnresolvedSlotCount = Stats.UnresolvedSlotCount;
	if (OutUnresolvedSlotNames != nullptr)
		*OutUnresolvedSlotNames = Stats.UnresolvedSlotNames;
	if (OutDefaultTextureSlotCount != nullptr)
		*OutDefaultTextureSlotCount = Stats.DefaultTextureSlotCount;
	return bBuilt;
}

bool WoTWorldBuildRuntimeBSPWithMirrorsForTesting(UWorld& World,
	const FWoTWorldDescriptor& Descriptor,
	TConstArrayView<FWoTBSPPayloadData> Parts,
	AWoTRuntimeBSPWorld*& OutWorld, FString& Error,
	TConstArrayView<FWoTRuntimeBSPTestConverterResource> AuthenticatedResources,
	const FWoTRuntimeMirrorOptions& MirrorOptions,
	FWoTRuntimeMirrorStageStats& OutMirrorStats)
{
	FWoTRuntimeBSPProjectionStats Stats;
	const bool bBuilt = WoTWorldBuildRuntimeBSP(
		World, Descriptor, Parts, OutWorld, Stats, Error,
		AuthenticatedResources, nullptr, EWoTRuntimeSkyStageFault::None,
		&MirrorOptions);
	OutMirrorStats = Stats.Mirror;
	return bBuilt;
}

bool WoTWorldResolveBSPSectionTexturesForTesting(
	const FWoTWorldDescriptor& Descriptor,
	TConstArrayView<FWoTBSPPayloadData> Parts,
	TConstArrayView<FWoTRuntimeBSPTestConverterResource> AuthenticatedResources,
	TArray<FString>& OutUnresolvedSlots, TArray<FString>& OutDefaultTextureSlots,
	TMap<FString, FString>& OutResolved, FString& Error)
{
	OutUnresolvedSlots.Reset();
	OutDefaultTextureSlots.Reset();
	OutResolved.Reset();
	Error.Reset();
	if (Parts.Num() != Descriptor.BSPParts.Num())
	{
		Error = TEXT("runtime BSP descriptor and decoded part counts differ");
		return false;
	}
	TMap<FString, FString> AuthenticatedSourceExports;
	for (int32 PartIndex = 0; PartIndex < Parts.Num(); ++PartIndex)
	{
		TArray<FWoTRuntimeBSPSectionStyle> Styles;
		TArray<bool> ProceduralSections;
		TArray<TOptional<FWoTNeutralResourceId>> Resources;
		TArray<int32> SourceSections;
		WoTWorldVisibleBSPPayload(Parts[PartIndex], Descriptor.BSPParts[PartIndex],
			Styles, ProceduralSections, Resources, AuthenticatedSourceExports,
			AuthenticatedResources, Error, &SourceSections);
		if (!Error.IsEmpty()) return false;
		for (int32 Visible = 0; Visible < SourceSections.Num(); ++Visible)
		{
			const FWoTBSPPayloadSection& Section =
				Parts[PartIndex].Sections[SourceSections[Visible]];
			if (Resources[Visible].IsSet())
			{
				OutResolved.Add(Section.MaterialSlot, Resources[Visible]->LogicalId);
				continue;
			}
			const bool bDefault = Parts[PartIndex].FormatVersion >= 4
				&& WoTWorldSectionTextureIsRetailDangling(Section, AuthenticatedResources);
			TArray<FString>& Target = bDefault ? OutDefaultTextureSlots : OutUnresolvedSlots;
			Target.AddUnique(Section.MaterialSlot);
		}
	}
	return true;
}
#endif

#if WITH_AUTOMATION_TESTS
void FWoTWorldMaterializer::RejectNextCompletedCandidateForAutomationTests()
{
	FWoTWorldMaterializationAuthority& Authority = WoTWorldAuthority();
	FScopeLock Lock(&Authority.Guard);
	Authority.bRejectNextCompletedCandidate = true;
}

bool FWoTWorldMaterializer::PublishNormalAuthorityAdoptedForAutomationTests()
{
	if (!IsWoTPayloadBootstrapActive())
	{
		return false;
	}
	FWoTWorldMaterializationAuthority& Authority = WoTWorldAuthority();
	FScopeLock Lock(&Authority.Guard);
	if (Authority.bNormalAuthorityAdoptedForTesting
		|| Authority.bNormalAdoptionStarted
		|| Authority.bNormalAuthorityAdopted
		|| Authority.NormalLease.IsValid()
		|| Authority.bWorldReadinessPending
		|| !Authority.AuthenticatedWorlds.IsEmpty())
	{
		return false;
	}
	Authority.bNormalAuthorityAdoptedForTesting = true;
	return true;
}

void FWoTWorldMaterializer::ResetNormalAuthorityAdoptedForAutomationTests()
{
	FWoTWorldMaterializationAuthority& Authority = WoTWorldAuthority();
	FScopeLock Lock(&Authority.Guard);
	Authority.bNormalAuthorityAdoptedForTesting = false;
}

bool FWoTWorldMaterializer::RevokeNormalLeaseForAutomationTests()
{
	FWoTWorldMaterializationAuthority& Authority = WoTWorldAuthority();
	FScopeLock Lock(&Authority.Guard);
	if (!Authority.NormalLease.IsValid())
	{
		return false;
	}
	Authority.NormalLease->RevokeForAutomationTests();
	return true;
}

void FWoTWorldMaterializer::ResetAuthorityForAutomationTests()
{
	FWoTWorldMaterializationAuthority& Authority = WoTWorldAuthority();
	FScopeLock Lock(&Authority.Guard);
	Authority.NormalLease.Reset();
	Authority.AuthenticatedWorlds.Empty();
	Authority.FrontEndReservedWorldIds.Empty();
	Authority.FrontEndReservedBSPIds.Empty();
	Authority.MaterializedWorlds.Empty();
	Authority.AwaitingExposureWorlds.Empty();
	Authority.InFlightWorlds.Empty();
	Authority.ExposedBootstrapWorlds.Empty();
	Authority.AwaitingBootstrapExposureWorld.Reset();
	Authority.BootstrapExposureKind = EWoTBootstrapExposureKind::None;
	Authority.BootstrapExposurePath = EWoTWorldStartPath::NormalInitialLoad;
	Authority.bBootstrapSpent = false;
	Authority.bNormalAdoptionStarted = false;
	Authority.bNormalAuthorityAdopted = false;
	Authority.bReadinessPublished = false;
	Authority.bWorldReadinessPending = false;
	Authority.bRejectNextCompletedCandidate = false;
	Authority.bNormalAuthorityAdoptedForTesting = false;
}
#endif

FWoTBootstrapStatus WoTAuthorizeNormalWorldMaterialization(
	const FWoTCacheGenerationLease& Snapshot,
	TArray<FWoTWorldDescriptor>&& AuthenticatedWorlds)
{
	const FString SnapshotName = Snapshot.GetManifestSha256().ToHex();
	const FWoTSha256 SnapshotManifestSha256 = Snapshot.GetManifestSha256();
	// The retail entry world and its BSP live in the same cache generation
	// under the World/BSP kinds; they are attested by the front-end authority,
	// not the normal descriptor set, so the normal census must set them aside.
	TArray<FString> ReservedWorldIds;
	TArray<FString> ReservedBSPIds;
	if (Snapshot.HasFrontEndDescriptorAttestation())
	{
		auto FrontEnd =
			WoTDeriveFrontEndWorldManifestAuthorityFromActiveGeneration(Snapshot);
		if (FrontEnd.IsError())
		{
			return FWoTBootstrapStatus::Error(MoveTemp(FrontEnd).GetError());
		}
		const TArray<FWoTCacheLeaseManifestEntry>& Entries = FrontEnd.GetValue();
		if (Entries.Num() >= 1) ReservedWorldIds.Add(Entries[0].LogicalId);
		if (Entries.Num() >= 2) ReservedBSPIds.Add(Entries[1].LogicalId);
	}
	if (!Snapshot.IsValid()
		|| !WoTWorldDescriptorSetComplete(AuthenticatedWorlds))
	{
		return WoTWorldMaterializationStatus(
			TEXT("normal-world authority did not match the independent live lease"));
	}

	FWoTWorldMaterializationAuthority& Authority = WoTWorldAuthority();
	{
		FScopeLock Lock(&Authority.Guard);
		if (!Authority.bBootstrapSpent
			|| Authority.bNormalAdoptionStarted
			|| Authority.bNormalAuthorityAdopted
			|| Authority.NormalLease.IsValid()
			|| Authority.bWorldReadinessPending
			|| !Authority.AuthenticatedWorlds.IsEmpty())
		{
			return WoTWorldMaterializationStatus(
				TEXT("normal-world authority was early, replayed, or already adopted"));
		}
		// One racing adopter wins an UNPUBLISHED authorization. Readiness is
		// published only after the first complete candidate is exposed.
		Authority.bNormalAdoptionStarted = true;
		Authority.NormalLease =
			MakeUnique<FWoTCacheGenerationLease>(Snapshot);
		Authority.bWorldReadinessPending = true;
		Authority.AuthenticatedWorlds = MoveTemp(AuthenticatedWorlds);
		Authority.FrontEndReservedWorldIds = MoveTemp(ReservedWorldIds);
		Authority.FrontEndReservedBSPIds = MoveTemp(ReservedBSPIds);
		Authority.bNormalAuthorityAdopted = true;
	}
	// Every hosted map travels through a shell: make the root resolvable now,
	// outside the authority lock, before any host request can be dispatched.
	WoTEnsureNeutralShellMountPoint();
#if WITH_AUTOMATION_TESTS
	{
		FScopeLock Lock(&WoTWorldAdoptionObserverGuard);
		if (WoTWorldAdoptionObserver)
		{
			WoTWorldAdoptionObserver(SnapshotName,
				SnapshotManifestSha256);
		}
	}
#endif
	return FWoTBootstrapStatus::Success();
}

#if WITH_AUTOMATION_TESTS
void WoTSetWorldLeaseAdoptionObserverForTesting(
	TFunction<void(FString, const FWoTSha256&)> Observer)
{
	FScopeLock Lock(&WoTWorldAdoptionObserverGuard);
	WoTWorldAdoptionObserver = MoveTemp(Observer);
}
#endif

bool IsWoTNormalWorldAuthorityAdopted()
{
	FWoTWorldMaterializationAuthority& Authority = WoTWorldAuthority();
	FWoTTryCriticalSectionGuard Lock(Authority.Guard);
	if (!Lock.IsLocked())
	{
		return false;
	}
#if WITH_AUTOMATION_TESTS
	if (Authority.bNormalAuthorityAdoptedForTesting)
	{
		return IsWoTPayloadBootstrapActive();
	}
#endif
	return Authority.bNormalAuthorityAdopted
		&& Authority.NormalLease.IsValid()
		&& Authority.NormalLease->IsValid()
		&& WoTWorldDescriptorSetComplete(Authority.AuthenticatedWorlds)
		&& IsWoTPayloadBootstrapActive();
}

bool IsWoTFrontEndIntegrityRecoveryAuthorized()
{
	FWoTWorldMaterializationAuthority& Authority = WoTWorldAuthority();
	FWoTTryCriticalSectionGuard Lock(Authority.Guard);
	if (!Lock.IsLocked() || !Authority.bBootstrapSpent
		|| !Authority.bNormalAuthorityAdopted
		|| !Authority.NormalLease.IsValid()
		|| !Authority.NormalLease
			->CanRecoverFrontEndAfterRuntimeIntegrityFailure())
	{
		return false;
	}
	return true;
}

TOptional<FString> WoTGuardedShellPackageForMapIdentity(
	const FString& MapIdentity)
{
	if (MapIdentity.IsEmpty())
	{
		return {};
	}
	FWoTWorldMaterializationAuthority& Authority = WoTWorldAuthority();
	FWoTTryCriticalSectionGuard Lock(Authority.Guard);
	if (!Lock.IsLocked() || !Authority.bNormalAuthorityAdopted)
	{
		return {};
	}
	for (const FWoTWorldDescriptor& Descriptor : Authority.AuthenticatedWorlds)
	{
		if (Descriptor.MapIdentity == MapIdentity)
		{
			return FString::Printf(TEXT("%sWoTMapShell%02u"),
				WoTNeutralShellRoot, Descriptor.NeutralMapSlot);
		}
	}
	return {};
}

TOptional<FString> WoTMapIdentityForGuardedShellPackage(
	const FString& ShellPackageOrWorldName)
{
	if (ShellPackageOrWorldName.IsEmpty()) return {};
	FString QueryLeaf = ShellPackageOrWorldName;
	int32 Slash = INDEX_NONE;
	if (QueryLeaf.FindLastChar(TEXT('/'), Slash)) QueryLeaf.RightChopInline(Slash + 1);
	for (const FString& Identity : WoTAdoptedWorldMapIdentities())
	{
		const TOptional<FString> Shell =
			WoTGuardedShellPackageForMapIdentity(Identity);
		if (!Shell.IsSet()) continue;
		FString ShellLeaf = Shell.GetValue();
		if (ShellLeaf.FindLastChar(TEXT('/'), Slash)) ShellLeaf.RightChopInline(Slash + 1);
		if (Shell.GetValue().Equals(ShellPackageOrWorldName,
				ESearchCase::IgnoreCase)
			|| ShellLeaf.Equals(QueryLeaf, ESearchCase::IgnoreCase))
		{
			return Identity;
		}
	}
	return {};
}

bool WoTEnsureNeutralShellMountPoint()
{
	const FString Probe = FString(WoTNeutralShellRoot) + TEXT("WoTMapShell01");
	if (FPackageName::IsValidLongPackageName(Probe, /*bIncludeReadOnlyRoots*/ true))
	{
		return true;
	}
	// Same content path the cook writes: ../../../WoT/Plugins/WoTNeutralShells/Content/
	const FString ContentPath =
		FPaths::ProjectPluginsDir() / TEXT("WoTNeutralShells/Content/");
	FPackageName::RegisterMountPoint(WoTNeutralShellRoot, ContentPath);
	const bool bResolves = FPackageName::IsValidLongPackageName(Probe, true);
	WoTRecordBootstrapHeartbeat(TEXT("neutral-shell-mount"),
		EWoTBootstrapPhase::Ready, 0, {}, {},
		FString::Printf(TEXT("registered=%s resolves=%d"), *ContentPath, bResolves ? 1 : 0));
	return bResolves;
}

bool WoTWorldCensusReconciles(int32 RegistryWorlds, int32 RegistryBSP,
	int32 DescriptorWorlds, int32 DescriptorBSP,
	int32 ReservedWorldsPresent, int32 ReservedBSPPresent)
{
	return RegistryWorlds - ReservedWorldsPresent == DescriptorWorlds
		&& RegistryBSP - ReservedBSPPresent == DescriptorBSP;
}

TArray<FString> WoTAdoptedWorldMapIdentities()
{
	TArray<FString> Identities;
	FWoTWorldMaterializationAuthority& Authority = WoTWorldAuthority();
	FWoTTryCriticalSectionGuard Lock(Authority.Guard);
	if (!Lock.IsLocked() || !Authority.bNormalAuthorityAdopted)
	{
		return Identities;
	}
	Identities.Reserve(Authority.AuthenticatedWorlds.Num());
	for (const FWoTWorldDescriptor& Descriptor : Authority.AuthenticatedWorlds)
	{
		Identities.Add(Descriptor.MapIdentity);
	}
	return Identities;
}

TWoTBootstrapResult<TArray<FWoTWorldDescriptor>>
WoTDeriveWorldDescriptorsFromActiveGeneration(
	const FWoTCacheGenerationLease& Lease)
{
	using FResult = TWoTBootstrapResult<TArray<FWoTWorldDescriptor>>;
	if (!Lease.IsValid())
	{
		return WoTWorldMaterializationError<TArray<FWoTWorldDescriptor>>(
			TEXT("world descriptor derivation requires one live world-domain lease"));
	}
	TOptional<TArray<FWoTCacheLeaseManifestEntry>> FrontEndAuthority;
	if (Lease.HasFrontEndDescriptorAttestation())
	{
		auto DerivedFrontEnd =
			WoTDeriveFrontEndWorldManifestAuthorityFromActiveGeneration(Lease);
		if (DerivedFrontEnd.IsError())
		{
			return FResult::Error(MoveTemp(DerivedFrontEnd).GetError());
		}
		FrontEndAuthority.Emplace(MoveTemp(DerivedFrontEnd).GetValue());
	}
	FScopeLock VerifiedOperation(&Lease.GetVerifiedReadGuard());
	if (!Lease.MayBeginVerifiedReadUnderVerifiedReadGuard())
	{
		return WoTWorldMaterializationError<TArray<FWoTWorldDescriptor>>(
			TEXT("world descriptor derivation observed a revoked generation lease"));
	}
	TArray<FWoTWorldDescriptor> Descriptors(Lease.GetWorldDescriptors());
	if (!WoTWorldDescriptorSetComplete(Descriptors))
	{
		return WoTWorldMaterializationError<TArray<FWoTWorldDescriptor>>(
			TEXT("the sealed world descriptor set is incomplete"));
	}
	TWoTBootstrapResult<FWoTRetailContract> ProductionContract =
		WoTLoadProductionRetailContract();
	if (ProductionContract.IsError())
	{
		return FResult::Error(MoveTemp(ProductionContract).GetError());
	}
	FWoTBootstrapStatus ContractAssociations =
		ValidateWorldDescriptorContractAssociations(
			Descriptors, ProductionContract.GetValue());
	if (ContractAssociations.IsError())
	{
		return FResult::Error(MoveTemp(ContractAssociations).GetError());
	}

	TSet<FString> BoundWorlds;
	TSet<FString> BoundBSP;
	for (const FWoTWorldDescriptor& Descriptor : Descriptors)
	{
		const FWoTCacheLeaseManifestEntry* World =
			Lease.GetManifestTable().Find(Descriptor.WorldLogicalId);
		if (World == nullptr || World->Schema != Descriptor.WorldSchema
			|| World->Sha256 != Descriptor.WorldSha256
			|| World->Satisfies.Num() != 1
			|| World->Satisfies[0] != Descriptor.WorldFactId
			|| BoundWorlds.Contains(Descriptor.WorldLogicalId))
		{
			return WoTWorldMaterializationError<TArray<FWoTWorldDescriptor>>(
				TEXT("a sealed world descriptor is not bound by the held manifest"));
		}
		BoundWorlds.Add(Descriptor.WorldLogicalId);
		for (const FWoTBSPPartDescriptor& Part : Descriptor.BSPParts)
		{
			const FWoTCacheLeaseManifestEntry* BSP =
				Lease.GetManifestTable().Find(Part.LogicalId);
			if (BSP == nullptr || BSP->Schema != Part.Schema
				|| BSP->Sha256 != Part.Sha256
				|| BSP->Satisfies.Num() != 1
				|| BSP->Satisfies[0] != Descriptor.BSPFactId
				|| BoundBSP.Contains(Part.LogicalId))
			{
				return WoTWorldMaterializationError<TArray<FWoTWorldDescriptor>>(
					TEXT("a sealed BSP descriptor is not bound by the held manifest"));
			}
			BoundBSP.Add(Part.LogicalId);
		}
	}
	auto ContainsExact = [](const TSet<FString>& Values, const FString& Value)
	{
		for (const FString& Candidate : Values)
		{
			if (Candidate.Equals(Value, ESearchCase::CaseSensitive))
			{
				return true;
			}
		}
		return false;
	};
	for (const TPair<FString, FWoTCacheLeaseManifestEntry>& Pair :
		Lease.GetManifestTable())
	{
		const bool bFrontEndWorld = FrontEndAuthority.IsSet()
			&& Pair.Key.Equals((*FrontEndAuthority)[0].LogicalId,
				ESearchCase::CaseSensitive);
		const bool bFrontEndBSP = FrontEndAuthority.IsSet()
			&& Pair.Key.Equals((*FrontEndAuthority)[1].LogicalId,
				ESearchCase::CaseSensitive);
		if ((Pair.Value.Schema.Equals(
				TEXT("wot-world-v3"), ESearchCase::CaseSensitive)
				&& !ContainsExact(BoundWorlds, Pair.Key) && !bFrontEndWorld)
			|| (Pair.Value.Schema.Equals(
				TEXT("wot-bsp-v3"), ESearchCase::CaseSensitive)
				&& !ContainsExact(BoundBSP, Pair.Key) && !bFrontEndBSP))
		{
			return WoTWorldMaterializationError<TArray<FWoTWorldDescriptor>>(
				TEXT("the held manifest has an unbound world-domain object"));
		}
	}
	if (!Lease.IsValid())
	{
		return WoTWorldMaterializationError<TArray<FWoTWorldDescriptor>>(
			TEXT("world descriptor derivation cannot succeed after snapshot revocation"));
	}
	return FResult::Success(MoveTemp(Descriptors));
}

void WoTShutdownNormalWorldMaterializationAuthority()
{
	check(IsInGameThread());
	FWoTWorldMaterializationAuthority& Authority = WoTWorldAuthority();
	FScopeLock Lock(&Authority.Guard);
	Authority.bWorldReadinessPending = false;
	Authority.AuthenticatedWorlds.Empty();
	Authority.FrontEndReservedWorldIds.Empty();
	Authority.FrontEndReservedBSPIds.Empty();
	Authority.MaterializedWorlds.Empty();
	Authority.AwaitingExposureWorlds.Empty();
	Authority.InFlightWorlds.Empty();
	Authority.ExposedBootstrapWorlds.Empty();
	Authority.AwaitingBootstrapExposureWorld.Reset();
	Authority.BootstrapExposureKind = EWoTBootstrapExposureKind::None;
	Authority.BootstrapExposurePath = EWoTWorldStartPath::NormalInitialLoad;
	Authority.NormalLease.Reset();
	Authority.bNormalAdoptionStarted = false;
	Authority.bNormalAuthorityAdopted = false;
	Authority.bReadinessPublished = false;
#if WITH_AUTOMATION_TESTS
	Authority.bNormalAuthorityAdoptedForTesting = false;
#endif
}

TWoTBootstrapResult<FWoTMaterializedWorldReceipt>
FWoTWorldMaterializer::MaterializeBeforePlay(
	UWorld& World, const FWoTWorldDescriptor& Descriptor,
	const FWoTCacheGenerationLease& Lease)
{
	using FResult = TWoTBootstrapResult<FWoTMaterializedWorldReceipt>;
	if (!IsInGameThread() || !Lease.IsValid()
		|| !WoTWorldDescriptorComplete(Descriptor)
		|| World.HasBegunPlay() || World.AreActorsInitialized()
		|| World.GetNetDriver() != nullptr || World.GetAISystem() != nullptr
		|| World.GetPhysicsScene() != nullptr
		|| FNavigationSystem::GetCurrent<UNavigationSystemV1>(&World) != nullptr
		|| World.PersistentLevel == nullptr)
	{
		return WoTWorldMaterializationError<FWoTMaterializedWorldReceipt>(
			TEXT("world materialization requires a hidden uninitialized candidate and live lease"));
	}

	const FWoTNeutralResourceId WorldId{
		Descriptor.WorldLogicalId, EWoTNeutralResourceKind::World};
	const TOptional<FWoTCacheBlobDescriptor> WorldBlob =
		FWoTRuntimeRegistry::Get().FindDescriptor(WorldId);
	if (!WorldBlob.IsSet() || WorldBlob->Schema != Descriptor.WorldSchema
		|| WorldBlob->Sha256 != Descriptor.WorldSha256)
	{
		return WoTWorldMaterializationError<FWoTMaterializedWorldReceipt>(
			TEXT("the exact descriptor-authorized world object is absent"));
	}
	FWoTRuntimeWorldResource WorldResource;
	FWoTBootstrapStatus WorldLoaded = WorldResource.LoadFirstUse(WorldId);
	if (WorldLoaded.IsError())
	{
		return FResult::Error(MoveTemp(WorldLoaded).GetError());
	}
	TWoTBootstrapResult<FWoTSha256> SourceSha =
		FWoTSha256::FromHex(WorldResource.SourceSha256());
	if (SourceSha.IsError()
		|| SourceSha.GetValue() != Descriptor.PackageSha256
		|| Descriptor.MapIdentity != WorldResource.MapIdentity()
		|| Descriptor.WorldFactId != WorldResource.FactId()
		|| Descriptor.WorldImporter != WorldResource.Importer()
		|| (WorldResource.ImporterVersion() != 2
			&& WorldResource.ImporterVersion() != 3
			&& WorldResource.ImporterVersion() != 4
			&& WorldResource.ImporterVersion() != 5
			&& WorldResource.ImporterVersion() != 6)
		|| Descriptor.ActorCount != WorldResource.ActorCount()
		|| Descriptor.AttachmentCount != WorldResource.AttachmentCount()
		|| Descriptor.BSPNodeCount != WorldResource.BSPNodeCount()
		|| Descriptor.BSPSurfaceCount != WorldResource.BSPSurfaceCount()
		|| Descriptor.ZoneCount != WorldResource.ZoneCount()
		|| Descriptor.PortalSurfaceCount
			!= WorldResource.PortalSurfaceCount()
		|| Descriptor.MoverCount != WorldResource.MoverCount()
		|| Descriptor.LightCount != WorldResource.LightCount()
		|| Descriptor.SoundCount != WorldResource.SoundCount()
		|| Descriptor.NavigationSeedCount
			!= WorldResource.NavigationSeedCount()
		|| Descriptor.SemanticObjectCount
			!= WorldResource.SemanticObjectCount()
		|| Descriptor.ReachSpecCount != WorldResource.ReachSpecCount())
	{
		return WoTWorldMaterializationError<FWoTMaterializedWorldReceipt>(
			TEXT("the complete world descriptor binding or census mismatched"));
	}

	// Pass one creates every identity before a property/reference is applied.
	FActorSpawnParameters RootParameters;
	RootParameters.OverrideLevel = World.PersistentLevel;
	RootParameters.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	RootParameters.bDeferConstruction = true;
	RootParameters.ObjectFlags |= RF_Transient;
	RootParameters.Name = MakeUniqueObjectName(
		World.PersistentLevel, AWoTMaterializedWorldRoot::StaticClass(),
		FName(TEXT("WoTMaterializedWorldRoot")));
	AWoTMaterializedWorldRoot* Root =
		World.SpawnActor<AWoTMaterializedWorldRoot>(
			AWoTMaterializedWorldRoot::StaticClass(),
			FTransform::Identity, RootParameters);
	if (Root == nullptr)
	{
		return WoTWorldMaterializationError<FWoTMaterializedWorldReceipt>(
			TEXT("the candidate could not allocate its code-only world root"));
	}
	Root->MapIdentity = Descriptor.MapIdentity;
	Root->MapRootHex = Descriptor.MapRoot.ToHex();
	Root->WorldLogicalId = Descriptor.WorldLogicalId;

	TMap<FString, AWoTMaterializedActorHost*> ActorByIdentity;
	TMap<FString, UObject*> ObjectByIdentity;
	for (int32 RecordIndex = 0;
		RecordIndex < WorldResource.Actors().Num(); ++RecordIndex)
	{
		const FWoTRuntimeWorldActor& Record =
			WorldResource.Actors()[RecordIndex];
		FActorSpawnParameters ActorParameters;
		ActorParameters.OverrideLevel = World.PersistentLevel;
		ActorParameters.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		ActorParameters.bDeferConstruction = true;
		ActorParameters.ObjectFlags |= RF_Transient;
		ActorParameters.Name = MakeUniqueObjectName(
			World.PersistentLevel, AWoTMaterializedActorHost::StaticClass(),
			FName(*FString::Printf(TEXT("WoTSemanticActor_%06d"), RecordIndex)));
		AWoTMaterializedActorHost* Host =
			World.SpawnActor<AWoTMaterializedActorHost>(
				AWoTMaterializedActorHost::StaticClass(),
				FTransform::Identity, ActorParameters);
		if (Host == nullptr)
		{
			return WoTWorldMaterializationError<FWoTMaterializedWorldReceipt>(
				TEXT("an actor semantic record has no code-only host"));
		}
		Host->SemanticIdentity = Record.Identity;
		Host->SemanticClassIdentity = Record.ClassIdentity;
		Root->ActorHosts.Add(Host);
		ActorByIdentity.Add(Record.Identity.ToLower(), Host);
		ObjectByIdentity.Add(Record.Identity.ToLower(), Host);
	}
	for (int32 RecordIndex = 0;
		RecordIndex < WorldResource.SemanticObjects().Num(); ++RecordIndex)
	{
		const FWoTRuntimeWorldSemanticObject& Record =
			WorldResource.SemanticObjects()[RecordIndex];
		if (Record.bActor)
		{
			if (!ActorByIdentity.Contains(Record.Identity.ToLower()))
			{
				return WoTWorldMaterializationError<FWoTMaterializedWorldReceipt>(
					TEXT("an actor semantic identity has no pass-one host"));
			}
			continue;
		}
		UWoTMaterializedObjectHost* Host =
			NewObject<UWoTMaterializedObjectHost>(
				Root, UWoTMaterializedObjectHost::StaticClass(),
				FName(*FString::Printf(
					TEXT("WoTSemanticObject_%06d"), RecordIndex)),
				RF_Transient);
		if (Host == nullptr)
		{
			return WoTWorldMaterializationError<FWoTMaterializedWorldReceipt>(
				TEXT("a non-actor semantic record has no code-only host"));
		}
		Host->SemanticIdentity = Record.Identity;
		Host->SemanticClassIdentity = Record.ClassIdentity;
		Root->ObjectHosts.Add(Host);
		ObjectByIdentity.Add(Record.Identity.ToLower(), Host);
	}
	if (ObjectByIdentity.Num() != WorldResource.SemanticObjects().Num())
	{
		return WoTWorldMaterializationError<FWoTMaterializedWorldReceipt>(
			TEXT("the pass-one semantic identity census is incomplete"));
	}
	uint32 AppliedAttachments = 0;
	for (const FWoTRuntimeWorldActor& Record : WorldResource.Actors())
	{
		if (Record.ParentIdentity.IsEmpty())
		{
			continue;
		}
		AWoTMaterializedActorHost* const* Child =
			ActorByIdentity.Find(Record.Identity.ToLower());
		AWoTMaterializedActorHost* const* Parent =
			ActorByIdentity.Find(Record.ParentIdentity.ToLower());
		if (Child == nullptr || Parent == nullptr
			|| !(*Child)->AttachToActor(
				*Parent, FAttachmentTransformRules::KeepRelativeTransform))
		{
			return WoTWorldMaterializationError<FWoTMaterializedWorldReceipt>(
				TEXT("an authenticated attachment could not be applied"));
		}
		++AppliedAttachments;
	}
	if (AppliedAttachments != Descriptor.AttachmentCount)
	{
		return WoTWorldMaterializationError<FWoTMaterializedWorldReceipt>(
			TEXT("the applied attachment census mismatched"));
	}

	// Pass two applies the retained properties and every geometry/collision/
	// navigation source while the candidate remains hidden from engine observers.
	uint32 ActualMoverCount = 0;
	uint32 ActualLightCount = 0;
	uint32 ActualSoundCount = 0;
	uint32 ActualNavigationSeedCount = 0;
	TArray<FString> NavigationSeeds;
	for (const FWoTRuntimeWorldActor& Record : WorldResource.Actors())
	{
		AWoTMaterializedActorHost* const* Host =
			ActorByIdentity.Find(Record.Identity.ToLower());
		if (Host == nullptr)
		{
			return WoTWorldMaterializationError<FWoTMaterializedWorldReceipt>(
				TEXT("a pass-two actor identity was not created in pass one"));
		}
		(*Host)->AppliedProperties = Record.Properties;
		(*Host)->ResolvedPropertyObjects.Reserve(Record.Properties.Num());
		const FString Leaf = WoTWorldClassLeaf(Record.ClassIdentity);
		for (const FWoTRuntimeWorldProperty& Property : Record.Properties)
		{
			UObject* ResolvedObject = nullptr;
			if (!Property.ObjectIdentity.IsEmpty())
			{
				UObject* const* Found =
					ObjectByIdentity.Find(Property.ObjectIdentity.ToLower());
				if (Found == nullptr)
				{
					return WoTWorldMaterializationError<
						FWoTMaterializedWorldReceipt>(
						TEXT("a pass-two property object was not created in pass one"));
				}
				ResolvedObject = *Found;
			}
			(*Host)->ResolvedPropertyObjects.Add(ResolvedObject);
			if (Property.Name.Contains(TEXT("Sound"))
				&& !Property.ObjectIdentity.IsEmpty())
			{
				++ActualSoundCount;
			}
		}
		ActualLightCount += Leaf == TEXT("Light") ? 1u : 0u;
		ActualSoundCount += Leaf == TEXT("AmbientSound") ? 1u : 0u;
		if (Leaf == TEXT("WotPathNode") || Leaf == TEXT("PathNode")
			|| Leaf == TEXT("NavigationPoint"))
		{
			++ActualNavigationSeedCount;
			NavigationSeeds.Add(Record.Identity);
		}
	}

	// Every record landed as a host carrying exactly its record's properties
	// (checked above), so the record census is the landed census. A
	// class-name/KeyNum heuristic undercounted PortcullisMover and the other
	// Battle mover subclasses and rejected every Battle map.
	ActualMoverCount = WoTWorldCensusMoverCount(WorldResource);

	UWoTMaterializedGeometryHost* Geometry =
		NewObject<UWoTMaterializedGeometryHost>(
			Root, UWoTMaterializedGeometryHost::StaticClass(),
			TEXT("AuthenticatedGeometry"), RF_Transient);
	if (Geometry == nullptr)
	{
		return WoTWorldMaterializationError<FWoTMaterializedWorldReceipt>(
			TEXT("the decoded geometry record kind has no landed host"));
	}
	Geometry->Vectors = WorldResource.Vectors();
	Geometry->Points = WorldResource.Points();
	Geometry->Nodes = WorldResource.Nodes();
	Geometry->Surfaces = WorldResource.Surfaces();
	Geometry->Vertices = WorldResource.Vertices();
	Geometry->Zones = WorldResource.Zones();
	Geometry->ReachSpecs = WorldResource.ReachSpecs();
	Geometry->BSPParts.Reserve(Descriptor.BSPParts.Num());
	for (const FWoTBSPPartDescriptor& Part : Descriptor.BSPParts)
	{
		FWoTBSPPayloadData& BSP = Geometry->BSPParts.AddDefaulted_GetRef();
		FWoTBootstrapStatus Decoded = WoTWorldDecodeBSPPart(Part, BSP);
		if (Decoded.IsError())
		{
			return FResult::Error(MoveTemp(Decoded).GetError());
		}
	}
	Geometry->NavigationSeedIdentities = MoveTemp(NavigationSeeds);
	FWoTRuntimeBSPCollisionGeometryView CollisionGeometry;
	CollisionGeometry.Vectors = Geometry->Vectors;
	CollisionGeometry.Points = Geometry->Points;
	CollisionGeometry.Nodes = Geometry->Nodes;
	CollisionGeometry.Surfaces = Geometry->Surfaces;
	CollisionGeometry.Vertices = Geometry->Vertices;
	FWoTRuntimeInvisibleBSPCollisionData InvisibleCollision;
	FString InvisibleCollisionError;
	if (!WoTWorldBuildInvisibleSolidBSPCollision(CollisionGeometry,
		InvisibleCollision, InvisibleCollisionError))
	{
		return WoTWorldMaterializationError<FWoTMaterializedWorldReceipt>(
			*InvisibleCollisionError);
	}
	Geometry->CollisionSurfaceIndices = InvisibleCollision.SurfaceIndices;
	Root->Geometry = Geometry;

	uint32 ActualPortalCount = 0;
	for (const FWoTRuntimeWorldSurface& Surface : Geometry->Surfaces)
	{
		ActualPortalCount += (Surface.Flags & 0x04000000u) != 0 ? 1u : 0u;
	}
	if (Root->ActorHosts.Num() != static_cast<int32>(Descriptor.ActorCount)
		|| Root->ObjectHosts.Num()
			!= static_cast<int32>(Descriptor.SemanticObjectCount
				- Descriptor.ActorCount)
		|| Geometry->Nodes.Num() != static_cast<int32>(Descriptor.BSPNodeCount)
		|| Geometry->Surfaces.Num()
			!= static_cast<int32>(Descriptor.BSPSurfaceCount)
		|| Geometry->Zones.Num() != static_cast<int32>(Descriptor.ZoneCount)
		|| Geometry->ReachSpecs.Num()
			!= static_cast<int32>(Descriptor.ReachSpecCount)
		|| Geometry->BSPParts.Num() != Descriptor.BSPParts.Num()
		|| ActualPortalCount != Descriptor.PortalSurfaceCount
		|| ActualMoverCount != Descriptor.MoverCount
		|| ActualLightCount != Descriptor.LightCount
		|| ActualSoundCount != Descriptor.SoundCount
		|| ActualNavigationSeedCount != Descriptor.NavigationSeedCount)
	{
		return WoTWorldMaterializationError<FWoTMaterializedWorldReceipt>(
			TEXT("the completed materialized candidate failed its full census"));
	}

	const double ProjectionStarted = FPlatformTime::Seconds();
	AWoTRuntimeBSPWorld* RuntimeBSPWorld = nullptr;
	FWoTRuntimeBSPProjectionStats BSPStats;
	FString ProjectionError;
	FWoTRetailSkyWorldView SkyWorldView;
	SkyWorldView.Actors = WorldResource.Actors();
	SkyWorldView.Zones = WorldResource.Zones();
	SkyWorldView.Nodes = WorldResource.Nodes();
	SkyWorldView.Surfaces = WorldResource.Surfaces();
	TArray<uint32> SurfaceFlags;
	SurfaceFlags.Reserve(Geometry->Surfaces.Num());
	for (const FWoTRuntimeWorldSurface& Surface : Geometry->Surfaces)
		SurfaceFlags.Add(Surface.Flags);
	if (!WoTWorldBuildRuntimeBSP(World, Descriptor, Geometry->BSPParts,
		RuntimeBSPWorld, BSPStats, ProjectionError, {}, &SkyWorldView,
		EWoTRuntimeSkyStageFault::None, nullptr, SurfaceFlags,
		&InvisibleCollision))
	{
		return WoTWorldMaterializationError<FWoTMaterializedWorldReceipt>(
			*ProjectionError);
	}
	UTexture2D* MoverLightmap = nullptr;
	TArray<UTexture2D*> MoverLightResponses;
	UTexture2D* MoverBlackLightResponse = nullptr;
	if (WorldResource.MoverAtlasWidth() != 0
		|| WorldResource.MoverAtlasHeight() != 0
		|| !WorldResource.MoverAtlasBGRA().IsEmpty())
	{
		MoverLightmap = WoTRuntimeBSPMaterializeLightmap(
			WorldResource.MoverAtlasWidth(), WorldResource.MoverAtlasHeight(),
			WorldResource.MoverAtlasBGRA());
		if (MoverLightmap == nullptr)
		{
			return WoTWorldMaterializationError<FWoTMaterializedWorldReceipt>(
				TEXT("the authenticated mover light-map atlas could not be materialized"));
		}
		RuntimeBSPWorld->RuntimeTextures.Add(MoverLightmap);
		for (const FWoTRuntimeMoverAnimationTerm& Term :
			WorldResource.MoverAnimationTerms())
		{
			UTexture2D* Response = WoTRuntimeBSPMaterializeLightmap(
				WorldResource.MoverAtlasWidth(), WorldResource.MoverAtlasHeight(),
				Term.ResponseBGRA);
			if (Response == nullptr)
			{
				return WoTWorldMaterializationError<FWoTMaterializedWorldReceipt>(
					TEXT("an authenticated mover light-map response could not be materialized"));
			}
			RuntimeBSPWorld->RuntimeTextures.Add(Response);
			MoverLightResponses.Add(Response);
		}
		if (!MoverLightResponses.IsEmpty())
		{
			MoverBlackLightResponse = WoTRuntimeBSPMaterializeSolidBlackTexture();
			if (MoverBlackLightResponse == nullptr)
			{
				return WoTWorldMaterializationError<FWoTMaterializedWorldReceipt>(
					TEXT("the mover light-map black response could not be materialized"));
			}
			RuntimeBSPWorld->RuntimeTextures.Add(MoverBlackLightResponse);
		}
	}
	FWoTWorldProjectionResult Projection;
	FWoTRuntimeClassDefaults* ClassDefaults =
		UWoTPayloadSubsystem::ActiveRuntimeClassDefaults(ProjectionError);
	if (ClassDefaults == nullptr)
	{
		return WoTWorldMaterializationError<FWoTMaterializedWorldReceipt>(
			*ProjectionError);
	}
	FWoTWorldProjectionSource ProjectionSource;
	ProjectionSource.MapIdentity = WorldResource.MapIdentity();
	ProjectionSource.Records = WorldResource.Actors();
	ProjectionSource.Points = WorldResource.Points();
	ProjectionSource.Nodes = WorldResource.Nodes();
	ProjectionSource.Vertices = WorldResource.Vertices();
	ProjectionSource.Zones = WorldResource.Zones();
	ProjectionSource.ReachSpecs = WorldResource.ReachSpecs();
	ProjectionSource.Surfaces = WorldResource.Surfaces();
	ProjectionSource.Leaves = WorldResource.Leaves();
	ProjectionSource.ModelLights = WorldResource.ModelLights();
	ProjectionSource.MoverModels = WorldResource.MoverModels();
	ProjectionSource.MoverAnimationGroups =
		WorldResource.MoverAnimationGroups();
	ProjectionSource.MoverAnimationTerms =
		WorldResource.MoverAnimationTerms();
	ProjectionSource.MoverLightResponses = MoverLightResponses;
	ProjectionSource.MoverLightmap = MoverLightmap;
	ProjectionSource.MoverBlackLightResponse = MoverBlackLightResponse;
	ProjectionSource.SemanticObjects = WorldResource.SemanticObjects();
	ProjectionSource.bBSPRootOutside = WorldResource.BSPRootOutside();
	ProjectionSource.bHasDedicatedMoverCollision = WorldResource.FormatVersion() >= 4;
	UWoTPayloadSubsystem* ProjectionPayloads = World.GetGameInstance() != nullptr
		? World.GetGameInstance()->GetSubsystem<UWoTPayloadSubsystem>() : nullptr;
	ProjectionSource.FindTexture = [ProjectionPayloads](FStringView Identity,
		FWoTRetailTextureFacts& Out)
	{
		return ProjectionPayloads != nullptr
			&& ProjectionPayloads->FindRetailTextureFacts(Identity, Out);
	};
	if (!FWoTWorldProjector::ProjectDeferred(
		World, ProjectionSource, *ClassDefaults, Projection, ProjectionError))
	{
		return WoTWorldMaterializationError<FWoTMaterializedWorldReceipt>(
			*ProjectionError);
	}
	Root->MapLightTable = Projection.MapLightTable;
	Root->LevelTitle = Projection.LevelTitle;

	// The world itself is still hidden; finishing all deferred code-only actors
	// is the single exposure point for the already complete candidate graph.
	for (AWoTMaterializedActorHost* Host : Root->ActorHosts)
	{
		if (Host == nullptr)
		{
			return WoTWorldMaterializationError<FWoTMaterializedWorldReceipt>(
				TEXT("the completed actor host graph became incomplete"));
		}
		Host->FinishSpawning(FTransform::Identity, true);
	}
	RuntimeBSPWorld->FinishSpawning(FTransform::Identity, true);
	for (AWoTSkyBox* RuntimeSkyBox : RuntimeBSPWorld->SkyBoxes)
	{
		if (RuntimeSkyBox == nullptr) continue;
		// The deferred spawn transform stands; AWoTSkyBox::Tick camera-locks it.
		RuntimeSkyBox->FinishSpawning(FTransform::Identity, true);
	}
	for (AActor* ProjectedActor : Projection.DeferredActors)
	{
		if (ProjectedActor == nullptr)
		{
			return WoTWorldMaterializationError<FWoTMaterializedWorldReceipt>(
				TEXT("the completed projection graph became incomplete"));
		}
		ProjectedActor->FinishSpawning(
			ProjectedActor->GetActorTransform(), true);
		// Deferred projection can register a display component before its
		// authenticated runtime mesh binds. FinishSpawning is the graph exposure
		// boundary, so complete that late bind before gameplay or the first frame.
		if (UWoTVertexAnimComponent* Display =
			ProjectedActor->FindComponentByClass<UWoTVertexAnimComponent>())
		{
			Display->EnsureRuntimeRenderState();
		}
		if (AWoTCorona* Corona = Cast<AWoTCorona>(ProjectedActor))
			Corona->EnsureRuntimeRenderState();
		if (AWoTRainCyd* Rain = Cast<AWoTRainCyd>(ProjectedActor))
			Rain->EnsureRuntimeRenderState();
	}
	Root->FinishSpawning(FTransform::Identity, true);
	const uint64 ProjectionMilliseconds = static_cast<uint64>(FMath::Max(
		0.0, FMath::RoundToDouble(
			(FPlatformTime::Seconds() - ProjectionStarted) * 1000.0)));
	const FString UnresolvedSlotNames = BSPStats.UnresolvedSlotNames.IsEmpty()
		? TEXT("-") : FString::Join(BSPStats.UnresolvedSlotNames, TEXT(","));
	TArray<FName> ProjectionRuleNames;
	Projection.RuleCounts.GetKeys(ProjectionRuleNames);
	ProjectionRuleNames.Sort(FNameLexicalLess());
	TArray<FString> ProjectionRuleTallies;
	for (const FName Rule : ProjectionRuleNames)
		ProjectionRuleTallies.Add(FString::Printf(TEXT("%s:%u"), *Rule.ToString(),
			Projection.RuleCount(Rule)));
	TArray<FName> SkippedRuleNames;
	Projection.SkippedRuleCounts.GetKeys(SkippedRuleNames);
	SkippedRuleNames.Sort(FNameLexicalLess());
	TArray<FString> SkippedRuleTallies;
	for (const FName Rule : SkippedRuleNames)
		SkippedRuleTallies.Add(FString::Printf(TEXT("%s:%u"), *Rule.ToString(),
			Projection.SkippedRuleCount(Rule)));
	const FString SkippedProjectionRules = SkippedRuleTallies.IsEmpty()
		? TEXT("-") : FString::Join(SkippedRuleTallies, TEXT(","));
	const FString SkippedProjectionSamples = Projection.SkippedSamples.IsEmpty()
		? TEXT("-") : FString::Join(Projection.SkippedSamples, TEXT(","));
	const FString UnsupportedProjectionClasses = Projection.UnsupportedClassNames.IsEmpty()
		? TEXT("-") : FString::Join(Projection.UnsupportedClassNames, TEXT(","));
	WoTRecordBootstrapHeartbeat(TEXT("world-projected"),
		EWoTBootstrapPhase::Ready, 0, {}, {},
		FString::Printf(TEXT("map=%s title=%s parts=%u collision_parts=%u sections=%u unresolved_slots=%u unresolved_slot_names=%s default_texture_slots=%u detail_sections=%u sky=%s sky_actors=%u sky_zone=%d sky_slots=%u sky_sections=%u sky_triangles=%u sky_scale=%.0f sky_rest_residual=%d sky_rest_surfaces=%u sky_rest_triangles=%u sky_rest_missing=%u sky_error=%s panned_slots=%u scene_time_materials=%u malformed_pan_zones=%u mirror_planes=%u mirror_sections=%u mirror_triangles=%u mirror_ineligible=%u mirror_skip=%s fallback_lightmap_parts=%u player_starts=%u projection_rules=%s skipped=%s skipped_samples=%s inert_records=%u unsupported_records=%u unsupported_classes=%s dropped_pool_entries=%u empty_pools=%u skipped_sprayers=%u light_table_partial=%d build_ms=%llu bsp_ms=%.3f tjunction_repair_ms=%.3f tjunction_fan_pairs=%llu tjunction_edges=%llu tjunction_split=%llu tjunction_inserted=%llu tjunction_near_collapsed=%llu tjunction_fallback_triangles=%llu ribbon_surfaces=%llu ribbon_triangles=%llu"),
			*WorldResource.MapIdentity(),
			Projection.LevelTitle.IsEmpty() ? TEXT("-")
				: *Projection.LevelTitle.Replace(TEXT(" "), TEXT("%20")),
			BSPStats.PartCount,
			BSPStats.CollisionPartCount, BSPStats.SectionCount,
			BSPStats.UnresolvedSlotCount, *UnresolvedSlotNames,
			BSPStats.DefaultTextureSlotCount, BSPStats.DetailSectionCount,
			BSPStats.SkyActorIdentity.IsEmpty() ? TEXT("none")
				: *BSPStats.SkyActorIdentity.Replace(TEXT(" "), TEXT("%20")),
			BSPStats.SkyActorCount, BSPStats.SkyZone, BSPStats.SkySlotCount,
			BSPStats.SkySectionCount, BSPStats.SkyTriangleCount,
			BSPStats.SkyScale, BSPStats.bSkyRestResidual ? 1 : 0,
			BSPStats.SkyRestSurfaceCount, BSPStats.SkyRestTriangleCount,
			BSPStats.SkyRestMissingSlots,
			BSPStats.SkyError.IsEmpty() ? TEXT("-")
				: *BSPStats.SkyError.Replace(TEXT(" "), TEXT("%20")),
			BSPStats.PannedSlotCount, BSPStats.SceneTimeMaterialCount,
			BSPStats.MalformedPanZones,
			BSPStats.Mirror.PlaneCount, BSPStats.Mirror.SectionCount,
			BSPStats.Mirror.TriangleCount, BSPStats.Mirror.IneligibleSectionCount,
			BSPStats.Mirror.SkipReason.IsEmpty() ? TEXT("-") : *BSPStats.Mirror.SkipReason,
			BSPStats.FallbackLightmapPartCount,
			Projection.PlayerStartCount,
			*FString::Join(ProjectionRuleTallies, TEXT(",")),
			*SkippedProjectionRules, *SkippedProjectionSamples,
			Projection.InertRecordCount, Projection.UnsupportedRecordCount,
			*UnsupportedProjectionClasses, Projection.DroppedPoolEntries,
			Projection.EmptyPoolCount, Projection.SkippedSprayerCount,
			Projection.bLightTablePartial ? 1 : 0, ProjectionMilliseconds,
			BSPStats.BSPMilliseconds, BSPStats.TJunctions.RepairMilliseconds,
			BSPStats.TJunctions.RetessellatedFanSliverPairs,
			BSPStats.TJunctions.RepairedEdges, BSPStats.TJunctions.SplitTriangles,
			BSPStats.TJunctions.InsertedEdgeVertices,
			BSPStats.TJunctions.NearCollapsedChildEdges,
			BSPStats.TJunctions.FallbackTriangles,
			BSPStats.TJunctions.RepairedLightmapRibbonSurfaces,
			BSPStats.TJunctions.RepairedLightmapRibbonTriangles));

	FWoTMaterializedWorldReceipt Receipt;
	Receipt.MapIdentity = WorldResource.MapIdentity();
	Receipt.WorldLogicalId = Descriptor.WorldLogicalId;
	Receipt.MapRootHex = Descriptor.MapRoot.ToHex();
	Receipt.NeutralMapSlot = Descriptor.NeutralMapSlot;
	Receipt.ActorCount = WorldResource.ActorCount();
	Receipt.AttachmentCount = WorldResource.AttachmentCount();
	Receipt.BSPNodeCount = WorldResource.BSPNodeCount();
	Receipt.BSPSurfaceCount = WorldResource.BSPSurfaceCount();
	Receipt.NavigationSeedCount = WorldResource.NavigationSeedCount();
	Receipt.SemanticObjectCount = WorldResource.SemanticObjectCount();
	Receipt.ReachSpecCount = WorldResource.ReachSpecCount();
	Receipt.ZoneCount = WorldResource.ZoneCount();
	Receipt.PortalSurfaceCount = WorldResource.PortalSurfaceCount();
	Receipt.MoverCount = WorldResource.MoverCount();
	Receipt.LightCount = WorldResource.LightCount();
	Receipt.SoundCount = WorldResource.SoundCount();
	Receipt.RepairedLightmapRibbonSurfaces =
		BSPStats.TJunctions.RepairedLightmapRibbonSurfaces;
	Receipt.RepairedLightmapRibbonTriangles =
		BSPStats.TJunctions.RepairedLightmapRibbonTriangles;
	Receipt.bIdentityAndAttachmentPassComplete = true;
	Receipt.bPropertyGeometryAndNavigationPassComplete = true;
	Receipt.bFullCensusValidated = true;
	Receipt.bReadyForAtomicExposure = true;
	return FResult::Success(MoveTemp(Receipt));
}

uint32 WoTWorldCensusMoverCount(const FWoTRuntimeWorldResource& World)
{
	TMap<FString, const FWoTRuntimeWorldActor*> ActorsByIdentity;
	ActorsByIdentity.Reserve(World.Actors().Num());
	for (const FWoTRuntimeWorldActor& Actor : World.Actors())
	{
		ActorsByIdentity.Add(Actor.Identity.ToLower(), &Actor);
	}
	uint32 Count = 0;
	for (const FWoTRuntimeMoverModel& MoverModel : World.MoverModels())
	{
		const FWoTRuntimeWorldActor* const* Actor =
			ActorsByIdentity.Find(MoverModel.ActorIdentity.ToLower());
		if (Actor == nullptr) continue;
		for (const FWoTRuntimeWorldProperty& Property : (*Actor)->Properties)
		{
			if (Property.Name.Equals(TEXT("Brush"), ESearchCase::IgnoreCase)
				&& Property.ObjectClassIdentity.Equals(
					TEXT("Engine.Model"), ESearchCase::IgnoreCase)
				&& Property.ObjectIdentity.Equals(
					MoverModel.ModelIdentity, ESearchCase::IgnoreCase))
			{
				++Count;
				break;
			}
		}
	}
	return Count;
}

bool WoTMaterializeAuthenticatedWorld(
	UWorld* World, EWoTWorldStartPath Path, FString* Error)
{
	if (World == nullptr || !WoTWorldStartPathKnown(Path))
	{
		return WoTWorldRejectCandidate(World, Error,
			TEXT("the guarded callback received a null world or unknown start path"));
	}
	const FString PackageName = WoTWorldCanonicalPackageName(*World);
	const FWoTCacheGenerationLease* Lease = nullptr;
	uint32 NeutralSlot = 0;
	FString LockedRejection;
	bool bBootstrapApproved = false;
	bool bFrontEndReturnApproved = false;
	bool bFrontEndReturnRejected = false;
	const bool bBootstrapCandidate = PackageName == WoTBootstrapPackage;
	if (!bBootstrapCandidate
		&& !WoTWorldReadNeutralSlot(PackageName, NeutralSlot))
	{
		// Editor and diagnostic worlds are not materialization candidates. Reject
		// them before consulting materialization state: a route outside the exact
		// allow-list must never wait for a snapshot that cannot admit it.
		return WoTWorldRejectCandidate(World, Error,
			TEXT("the package is outside the exact guarded world routes"));
	}
	FWoTWorldMaterializationAuthority& Authority = WoTWorldAuthority();
	FWoTWorldDescriptor AuthenticatedDescriptor;
	{
		// Route admission is a fail-closed predicate, not a wait point. A racing
		// adopter/materializer owns the transition; this candidate gets an
		// immediate load failure instead of blocking or retrying behind it.
		FWoTTryCriticalSectionGuard Lock(Authority.Guard);
		if (!Lock.IsLocked())
		{
			return WoTWorldRejectCandidate(World, Error,
				TEXT("guarded world materialization authority is busy"));
		}
		if (bBootstrapCandidate)
		{
			const TWeakObjectPtr<UWorld> Candidate(World);
			if (Authority.ExposedBootstrapWorlds.Contains(Candidate)
				|| Authority.AwaitingExposureWorlds.Contains(Candidate)
				|| Authority.InFlightWorlds.Contains(Candidate)
				|| Authority.BootstrapExposureKind
					!= EWoTBootstrapExposureKind::None)
			{
				LockedRejection =
					TEXT("the bootstrap candidate was replayed or conflicts with a pending exposure");
				bFrontEndReturnRejected = Authority.bBootstrapSpent;
			}
			else if (Authority.bBootstrapSpent)
			{
				LockedRejection = WoTWorldFrontEndReturnRejection(Authority);
				bFrontEndReturnRejected = !LockedRejection.IsEmpty();
				bFrontEndReturnApproved = LockedRejection.IsEmpty();
				if (bFrontEndReturnApproved)
				{
					Authority.AwaitingBootstrapExposureWorld = Candidate;
					Authority.BootstrapExposureKind =
						EWoTBootstrapExposureKind::Return;
					Authority.BootstrapExposurePath = Path;
				}
			}
			else
			{
				if (Authority.bNormalAdoptionStarted
					|| PackageName != WoTBootstrapPackage)
				{
					LockedRejection =
						TEXT("the initial world was not the fixed bundled bootstrap map");
				}
				else
				{
					Authority.bBootstrapSpent = true;
					Authority.AwaitingBootstrapExposureWorld = Candidate;
					Authority.BootstrapExposureKind =
						EWoTBootstrapExposureKind::Initial;
					Authority.BootstrapExposurePath = Path;
					bBootstrapApproved = true;
				}
			}
		}
		else if (!Authority.bBootstrapSpent
			|| !Authority.bNormalAuthorityAdopted
			|| !Authority.NormalLease.IsValid()
			|| !Authority.NormalLease->IsValid()
			|| (!Authority.bReadinessPublished
				&& !Authority.bWorldReadinessPending)
			|| (Authority.bReadinessPublished
				&& !IsWoTBootstrapWorldReady())
			|| !IsWoTPayloadBootstrapActive())
		{
			LockedRejection =
				TEXT("normal shell materialization lacks both adopted authorities");
		}
		else
		{
			const TWeakObjectPtr<UWorld> Candidate(World);
			if (Authority.MaterializedWorlds.Contains(Candidate)
				|| Authority.AwaitingExposureWorlds.Contains(Candidate)
				|| Authority.InFlightWorlds.Contains(Candidate))
			{
				LockedRejection =
					TEXT("the candidate world was replayed or is already materializing");
			}
			else
			{
				const FWoTWorldDescriptor* Descriptor =
					WoTWorldDescriptorForSlot(
						Authority.AuthenticatedWorlds, NeutralSlot);
				if (Descriptor == nullptr)
				{
					LockedRejection =
						TEXT("the neutral slot has no authenticated descriptor");
				}
				else
				{
					Authority.InFlightWorlds.Add(Candidate);
					Lease = Authority.NormalLease.Get();
					AuthenticatedDescriptor = *Descriptor;
				}
			}
		}
	}
	if (!LockedRejection.IsEmpty())
	{
		if (bFrontEndReturnRejected)
		{
			WoTWorldRecordFrontEndReturn(TEXT("world-return-rejected"),
				PackageName, Path, LockedRejection);
		}
		return WoTWorldRejectCandidate(World, Error, LockedRejection);
	}
	if (bBootstrapApproved || bFrontEndReturnApproved)
	{
		if (bFrontEndReturnApproved)
		{
			WoTWorldRecordFrontEndReturn(TEXT("world-return-admitted"),
				PackageName, Path);
		}
		if (Error != nullptr)
		{
			Error->Reset();
		}
		return true;
	}

	TArray<FWoTWorldDescriptor> DescriptorSnapshot;
	TArray<FString> ReservedWorldIds;
	TArray<FString> ReservedBSPIds;
	{
		FScopeLock Lock(&Authority.Guard);
		DescriptorSnapshot = Authority.AuthenticatedWorlds;
		ReservedWorldIds = Authority.FrontEndReservedWorldIds;
		ReservedBSPIds = Authority.FrontEndReservedBSPIds;
	}
	FString CensusWhy;
	if (!WoTWorldRegistryMatchesDescriptors(DescriptorSnapshot,
		ReservedWorldIds, ReservedBSPIds, &CensusWhy))
	{
		FScopeLock Lock(&Authority.Guard);
		Authority.InFlightWorlds.Remove(TWeakObjectPtr<UWorld>(World));
		return WoTWorldRejectCandidate(World, Error, FString::Printf(
			TEXT("the active generation does not match the exact authenticated descriptor census (%s)"),
			*CensusWhy));
	}
	FWoTWorldMaterializer Materializer;
	TWoTBootstrapResult<FWoTMaterializedWorldReceipt> Materialized =
		Materializer.MaterializeBeforePlay(
			*World, AuthenticatedDescriptor, *Lease);
	bool bInjectedRejection = false;
	{
		FScopeLock Lock(&Authority.Guard);
		const TWeakObjectPtr<UWorld> Candidate(World);
		Authority.InFlightWorlds.Remove(Candidate);
		if (!Materialized.IsError())
		{
#if WITH_AUTOMATION_TESTS
			if (Authority.bRejectNextCompletedCandidate)
			{
				Authority.bRejectNextCompletedCandidate = false;
				bInjectedRejection = true;
			}
			else
#endif
			{
				Authority.AwaitingExposureWorlds.Add(Candidate);
			}
		}
	}
	if (Materialized.IsError())
	{
		return WoTWorldRejectCandidate(
			World, Error, MoveTemp(Materialized).GetError());
	}
	if (bInjectedRejection)
	{
		return WoTWorldRejectCandidate(World, Error,
			TEXT("automation rejected the completed candidate before exposure"));
	}
	{
		// Success leaves no other durable trace in the guarded client; a black
		// hosted world needs the receipt tallies to be diagnosable post-mortem.
		const FWoTMaterializedWorldReceipt& Receipt = Materialized.GetValue();
		WoTRecordBootstrapHeartbeat(TEXT("world-materialized"),
			EWoTBootstrapPhase::Ready, 0, {}, {},
			FString::Printf(TEXT("map=%s slot=%u actors=%u attachments=%u bsp_nodes=%u bsp_surfaces=%u zones=%u nav_seeds=%u"),
				*Receipt.MapIdentity, Receipt.NeutralMapSlot, Receipt.ActorCount,
				Receipt.AttachmentCount, Receipt.BSPNodeCount,
				Receipt.BSPSurfaceCount, Receipt.ZoneCount,
				Receipt.NavigationSeedCount));
	}
	if (Error != nullptr)
	{
		Error->Reset();
	}
	return true;
}

int32 WoTWorldCompleteRegisteredPhysicsState(UWorld& World)
{
	// A guarded candidate is materialized before UWorld::InitWorld creates its
	// physics scene (MaterializeBeforePlay requires GetPhysicsScene()==nullptr).
	// Deferred projected actors register their native components at spawn, so
	// every such primitive - trigger/altar/teleporter/pickup capsules, blocker
	// prisms, mover collision - was left with no physics body. The level's own
	// component update only registers UNREGISTERED components, so nothing ever
	// created those bodies: no overlaps, no blocking, no traces. Create them
	// once the scene exists, exactly as registration into a live world would.
	if (World.GetPhysicsScene() == nullptr)
	{
		return 0;
	}
	int32 Created = 0;
	for (TActorIterator<AActor> It(&World); It; ++It)
	{
		TInlineComponentArray<UPrimitiveComponent*> Primitives;
		It->GetComponents(Primitives);
		for (UPrimitiveComponent* Primitive : Primitives)
		{
			if (!IsValid(Primitive) || !Primitive->IsRegistered()
				|| Primitive->IsPhysicsStateCreated())
			{
				continue;
			}
			Primitive->RecreatePhysicsState();
			Created += Primitive->IsPhysicsStateCreated() ? 1 : 0;
		}
	}
	return Created;
}

int32 WoTWorldCompleteRegisteredRenderState(UWorld& World)
{
	// UActorComponent::ExecuteRegisterEvents (ActorComponent.cpp:2519) creates a
	// render state only when the world already has a Scene; a guarded candidate
	// is materialized before UWorld::InitWorld allocates it (World.cpp:2484
	// AllocateScene), so every deferred projected actor's default components -
	// the mover brush mesh above all - registered without one. InitWorld's
	// level component update only registers UNREGISTERED components, and
	// MarkRenderStateDirty only recreates an EXISTING state, so the late-bind
	// fixes were per class (vertex meshes, coronas, rain) and every
	// other class stayed invisible. Complete it once the scene exists, with the
	// engine's own registration gate, exactly as registration into a live world
	// would have.
	if (World.Scene == nullptr || !FApp::CanEverRender())
	{
		return 0;
	}
	int32 Created = 0;
	for (TActorIterator<AActor> It(&World); It; ++It)
	{
		TInlineComponentArray<UPrimitiveComponent*> Primitives;
		It->GetComponents(Primitives);
		for (UPrimitiveComponent* Primitive : Primitives)
		{
			// USceneComponent declares ShouldCreateRenderState public; the call
			// dispatches to the class override (UStaticMeshComponent: a mesh
			// must be bound and not compiling).
			if (!IsValid(Primitive) || !Primitive->IsRegistered()
				|| Primitive->IsRenderStateCreated()
				|| !static_cast<const USceneComponent*>(Primitive)->ShouldCreateRenderState())
			{
				continue;
			}
			Primitive->RecreateRenderState_Concurrent();
			Created += Primitive->IsRenderStateCreated() ? 1 : 0;
		}
	}
	return Created;
}

namespace
{
FDelegateHandle GWoTWorldPhysicsCompletionHandle;

void WoTWorldCompletePhysicsAfterInitialization(
	UWorld* World, const UWorld::InitializationValues IVS)
{
	(void)IVS;
	if (World == nullptr)
	{
		return;
	}
	bool bMaterialized = false;
	{
		FWoTWorldMaterializationAuthority& Authority = WoTWorldAuthority();
		FScopeLock Lock(&Authority.Guard);
		bMaterialized = Authority.MaterializedWorlds.Contains(
			TWeakObjectPtr<UWorld>(World));
	}
	if (!bMaterialized)
	{
		return;
	}
	const int32 Created = WoTWorldCompleteRegisteredPhysicsState(*World);
	WoTRecordBootstrapHeartbeat(TEXT("world-physics-completed"),
		EWoTBootstrapPhase::Ready, 0, {}, {},
		FString::Printf(TEXT("package=%s created=%d"),
			*WoTWorldCanonicalPackageName(*World), Created));
	// InitWorld allocated the scene before this broadcast (World.cpp:2484
	// AllocateScene, :2613 OnPostWorldInitialization).
	const int32 RenderCreated = WoTWorldCompleteRegisteredRenderState(*World);
	WoTRecordBootstrapHeartbeat(TEXT("world-render-completed"),
		EWoTBootstrapPhase::Ready, 0, {}, {},
		FString::Printf(TEXT("package=%s created=%d"),
			*WoTWorldCanonicalPackageName(*World), RenderCreated));
}

// InitWorld (which creates the physics scene) runs after the exposure commit
// in UEngine::LoadMap; its post-initialization broadcast is the first point at
// which the materialized components can receive bodies.
void WoTWorldEnsurePhysicsCompletionHook()
{
	if (!GWoTWorldPhysicsCompletionHandle.IsValid())
	{
		GWoTWorldPhysicsCompletionHandle =
			FWorldDelegates::OnPostWorldInitialization.AddStatic(
				&WoTWorldCompletePhysicsAfterInitialization);
	}
}
}

bool WoTCommitAuthenticatedWorldExposure(UWorld* World, FString* Error)
{
	if (World == nullptr)
	{
		return WoTWorldRejectCandidate(nullptr, Error,
			TEXT("a null world cannot commit guarded exposure"));
	}
	const FString PackageName = WoTWorldCanonicalPackageName(*World);
	if (PackageName == WoTBootstrapPackage)
	{
		FWoTWorldMaterializationAuthority& Authority = WoTWorldAuthority();
		const TWeakObjectPtr<UWorld> Candidate(World);
		FString Rejection;
		bool bReturnExposure = false;
		EWoTWorldStartPath ExposurePath =
			EWoTWorldStartPath::NormalInitialLoad;
		{
			FScopeLock Lock(&Authority.Guard);
			if (Authority.BootstrapExposureKind
					== EWoTBootstrapExposureKind::None
				|| Authority.AwaitingBootstrapExposureWorld != Candidate)
			{
				Rejection =
					TEXT("bootstrap exposure was not preceded by one matching admission");
			}
			else
			{
				bReturnExposure = Authority.BootstrapExposureKind
					== EWoTBootstrapExposureKind::Return;
				ExposurePath = Authority.BootstrapExposurePath;
				auto ConsumeExposureRecord = [&Authority]()
				{
					Authority.AwaitingBootstrapExposureWorld.Reset();
					Authority.BootstrapExposureKind =
						EWoTBootstrapExposureKind::None;
					Authority.BootstrapExposurePath =
						EWoTWorldStartPath::NormalInitialLoad;
				};
				if (bReturnExposure)
				{
					if (!Authority.NormalLease.IsValid())
					{
						ConsumeExposureRecord();
						Rejection =
							TEXT("the returning bootstrap exposure lost its adopted world lease");
					}
					else if (Authority.NormalLease
						->HasRuntimeIntegrityFailure())
					{
						// The runtime integrity path has already closed every normal
						// read/exposure gate. Recheck the fixed bootstrap route without
						// asking the revoked snapshot for new read admission.
						ConsumeExposureRecord();
						if (PackageName != WoTBootstrapPackage)
						{
							Rejection =
								TEXT("the returning world was not the fixed bundled bootstrap map");
						}
						else
						{
							Rejection =
								WoTWorldFrontEndReturnRejection(Authority);
						}
					}
					else
					{
						const FWoTCacheGenerationLease& Lease =
							*Authority.NormalLease;
						FScopeLock VerifiedRead(&Lease.GetVerifiedReadGuard());
						// This is the one-way exposure seam: once the lease's
						// verified-read gate is held, no competing revocation can
						// cross between record consumption and predicate recheck.
						ConsumeExposureRecord();
						if (Lease.IsRevocationInProgress())
						{
							Rejection =
								TEXT("the returning bootstrap exposure observed world-lease revocation in progress");
						}
						else if (!Lease.MayBeginVerifiedReadUnderVerifiedReadGuard())
						{
							Rejection =
								TEXT("the returning bootstrap exposure observed a revoked world lease");
						}
						else if (PackageName != WoTBootstrapPackage)
						{
							Rejection =
								TEXT("the returning world was not the fixed bundled bootstrap map");
						}
						else
						{
							Rejection = WoTWorldFrontEndReturnRejection(Authority);
						}
					}
				}
				else
				{
					ConsumeExposureRecord();
				}
				if (Rejection.IsEmpty())
				{
					Authority.ExposedBootstrapWorlds.Add(Candidate);
				}
			}
		}
		if (!Rejection.IsEmpty())
		{
			if (bReturnExposure)
			{
				WoTWorldRecordFrontEndReturn(TEXT("world-return-rejected"),
					PackageName, ExposurePath, Rejection);
			}
			return WoTWorldRejectCandidate(World, Error, Rejection);
		}
		if (Error != nullptr)
		{
			Error->Reset();
		}
		return true;
	}

	FWoTWorldMaterializationAuthority& Authority = WoTWorldAuthority();
	bool bPublishReadiness = false;
	const TWeakObjectPtr<UWorld> Candidate(World);
	{
		int32 PlayerStarts = 0;
		int32 Lights = 0;
		int32 Actors = 0;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			++Actors;
			if (It->IsA<APlayerStart>()) ++PlayerStarts;
			if (It->FindComponentByClass<ULightComponent>()) ++Lights;
		}
		WoTRecordBootstrapHeartbeat(TEXT("world-exposed"),
			EWoTBootstrapPhase::Ready, 0, {}, {},
			FString::Printf(TEXT("package=%s actors=%d player_starts=%d lit_actors=%d game_mode=%s net=%d"),
				*PackageName, Actors, PlayerStarts, Lights,
				World->GetAuthGameMode() ? *World->GetAuthGameMode()->GetClass()->GetName() : TEXT("none"),
				static_cast<int32>(World->GetNetMode())));
	}
	{
		FScopeLock Lock(&Authority.Guard);
		if (!Authority.AwaitingExposureWorlds.Remove(Candidate))
		{
			return WoTWorldRejectCandidate(World, Error,
				TEXT("engine exposure was not preceded by one completed materialization"));
		}
		if (!Authority.bReadinessPublished)
		{
			if (!Authority.bWorldReadinessPending)
			{
				return WoTWorldRejectCandidate(World, Error,
					TEXT("the first exposed world lost its pending readiness state"));
			}
			Authority.bWorldReadinessPending = false;
			bPublishReadiness = true;
		}
	}

	if (bPublishReadiness)
	{
		FWoTBootstrapStatus Published = PublishWoTBootstrapWorldReady();
		if (Published.IsError())
		{
			return WoTWorldRejectCandidate(
				World, Error, MoveTemp(Published).GetError());
		}
	}
	{
		FScopeLock Lock(&Authority.Guard);
		Authority.bReadinessPublished = true;
		Authority.MaterializedWorlds.Add(Candidate);
	}
	WoTWorldEnsurePhysicsCompletionHook();
	if (UGameInstance* GameInstance = World->GetGameInstance())
	{
		if (UWoTSessionSubsystem* Sessions =
			GameInstance->GetSubsystem<UWoTSessionSubsystem>())
		{
			Sessions->NotifySuccessfulHostDestinationExposure();
		}
	}
	if (Error != nullptr)
	{
		Error->Reset();
	}
	return true;
}
