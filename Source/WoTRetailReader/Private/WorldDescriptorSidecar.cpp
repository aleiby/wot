#include "WoTRetailReader/WorldDescriptorSidecar.h"

#include "CanonicalObjectBuilder.h"
#include "WoTBootstrapCore/CanonicalJson.h"

namespace
{
constexpr int32 WoTWorldDescriptorCount = 19;
template <typename T>
TWoTBootstrapResult<T> WoTWorldSidecarError(
	EWoTBootstrapError Code, const TCHAR* Message)
{
	return TWoTBootstrapResult<T>::Error(
		MakeWoTBootstrapError(Code, Message));
}

TWoTBootstrapResult<FWoTCanonicalJsonValue> WoTWorldPartJson(
	const FWoTBSPPartDescriptor& Part)
{
	FWoTCanonicalObjectBuilder Builder;
	Builder.Add(TEXT("fact_id"),
		FWoTCanonicalJsonValue::MakeString(Part.FactId));
	Builder.Add(TEXT("index_count"),
		FWoTCanonicalJsonValue::MakeInteger(Part.IndexCount));
	Builder.Add(TEXT("logical_id"),
		FWoTCanonicalJsonValue::MakeString(Part.LogicalId));
	Builder.Add(TEXT("part_identity"),
		FWoTCanonicalJsonValue::MakeString(Part.PartIdentity));
	Builder.Add(TEXT("payload_sha256"),
		FWoTCanonicalJsonValue::MakeString(Part.PayloadSha256.ToHex()));
	Builder.Add(TEXT("procedural_material_slot"),
		FWoTCanonicalJsonValue::MakeString(Part.ProceduralMaterialSlot));
	Builder.Add(TEXT("role"), FWoTCanonicalJsonValue::MakeString(
		Part.Role == EWoTBSPPartRole::Portal
			? TEXT("portal") : TEXT("main")));
	Builder.Add(TEXT("schema"),
		FWoTCanonicalJsonValue::MakeString(Part.Schema));
	Builder.Add(TEXT("section_count"),
		FWoTCanonicalJsonValue::MakeInteger(Part.SectionCount));
	Builder.Add(TEXT("sha256"),
		FWoTCanonicalJsonValue::MakeString(Part.Sha256.ToHex()));
	Builder.Add(TEXT("vertex_count"),
		FWoTCanonicalJsonValue::MakeInteger(Part.VertexCount));
	return MoveTemp(Builder).Build(TEXT("world descriptor BSP part"));
}

TWoTBootstrapResult<FWoTCanonicalJsonValue> WoTWorldDescriptorJson(
	const FWoTWorldDescriptor& Descriptor)
{
	using FResult = TWoTBootstrapResult<FWoTCanonicalJsonValue>;
	FWoTCanonicalJsonValue Parts = FWoTCanonicalJsonValue::MakeArray();
	for (const FWoTBSPPartDescriptor& Part : Descriptor.BSPParts)
	{
		FResult Encoded = WoTWorldPartJson(Part);
		if (Encoded.IsError())
		{
			return FResult::Error(MoveTemp(Encoded).GetError());
		}
		Parts.AddElement(MoveTemp(Encoded).GetValue());
	}

	FWoTCanonicalObjectBuilder Builder;
	Builder.Add(TEXT("actor_count"),
		FWoTCanonicalJsonValue::MakeInteger(Descriptor.ActorCount));
	Builder.Add(TEXT("attachment_count"),
		FWoTCanonicalJsonValue::MakeInteger(Descriptor.AttachmentCount));
	Builder.Add(TEXT("bsp_fact_id"),
		FWoTCanonicalJsonValue::MakeString(Descriptor.BSPFactId));
	Builder.Add(TEXT("bsp_importer"),
		FWoTCanonicalJsonValue::MakeString(Descriptor.BSPImporter));
	Builder.Add(TEXT("bsp_node_count"),
		FWoTCanonicalJsonValue::MakeInteger(Descriptor.BSPNodeCount));
	Builder.Add(TEXT("bsp_parts"), MoveTemp(Parts));
	Builder.Add(TEXT("bsp_surface_count"),
		FWoTCanonicalJsonValue::MakeInteger(Descriptor.BSPSurfaceCount));
	Builder.Add(TEXT("light_count"),
		FWoTCanonicalJsonValue::MakeInteger(Descriptor.LightCount));
	Builder.Add(TEXT("map_identity"),
		FWoTCanonicalJsonValue::MakeString(Descriptor.MapIdentity));
	Builder.Add(TEXT("map_root"),
		FWoTCanonicalJsonValue::MakeString(Descriptor.MapRoot.ToHex()));
	Builder.Add(TEXT("mover_count"),
		FWoTCanonicalJsonValue::MakeInteger(Descriptor.MoverCount));
	Builder.Add(TEXT("navigation_seed_count"),
		FWoTCanonicalJsonValue::MakeInteger(Descriptor.NavigationSeedCount));
	Builder.Add(TEXT("neutral_map_slot"),
		FWoTCanonicalJsonValue::MakeInteger(Descriptor.NeutralMapSlot));
	Builder.Add(TEXT("package_sha256"),
		FWoTCanonicalJsonValue::MakeString(Descriptor.PackageSha256.ToHex()));
	Builder.Add(TEXT("portal_surface_count"),
		FWoTCanonicalJsonValue::MakeInteger(Descriptor.PortalSurfaceCount));
	Builder.Add(TEXT("reach_spec_count"),
		FWoTCanonicalJsonValue::MakeInteger(Descriptor.ReachSpecCount));
	Builder.Add(TEXT("semantic_object_count"),
		FWoTCanonicalJsonValue::MakeInteger(Descriptor.SemanticObjectCount));
	Builder.Add(TEXT("sound_count"),
		FWoTCanonicalJsonValue::MakeInteger(Descriptor.SoundCount));
	Builder.Add(TEXT("source_root"),
		FWoTCanonicalJsonValue::MakeString(Descriptor.SourceRoot.ToHex()));
	Builder.Add(TEXT("world_fact_id"),
		FWoTCanonicalJsonValue::MakeString(Descriptor.WorldFactId));
	Builder.Add(TEXT("world_importer"),
		FWoTCanonicalJsonValue::MakeString(Descriptor.WorldImporter));
	Builder.Add(TEXT("world_logical_id"),
		FWoTCanonicalJsonValue::MakeString(Descriptor.WorldLogicalId));
	Builder.Add(TEXT("world_schema"),
		FWoTCanonicalJsonValue::MakeString(Descriptor.WorldSchema));
	Builder.Add(TEXT("world_sha256"),
		FWoTCanonicalJsonValue::MakeString(Descriptor.WorldSha256.ToHex()));
	Builder.Add(TEXT("zone_count"),
		FWoTCanonicalJsonValue::MakeInteger(Descriptor.ZoneCount));
	return MoveTemp(Builder).Build(TEXT("world descriptor"));
}

TWoTBootstrapResult<FWoTCanonicalJsonValue> WoTWorldDescriptorArray(
	TConstArrayView<FWoTWorldDescriptor> Descriptors)
{
	using FResult = TWoTBootstrapResult<FWoTCanonicalJsonValue>;
	FWoTCanonicalJsonValue Encoded = FWoTCanonicalJsonValue::MakeArray();
	for (const FWoTWorldDescriptor& Descriptor : Descriptors)
	{
		FResult Item = WoTWorldDescriptorJson(Descriptor);
		if (Item.IsError())
		{
			return FResult::Error(MoveTemp(Item).GetError());
		}
		Encoded.AddElement(MoveTemp(Item).GetValue());
	}
	return FResult::Success(MoveTemp(Encoded));
}

TOptional<FWoTBootstrapError> WoTWorldReadDigest(
	const FWoTCanonicalJsonValue& Object, const TCHAR* Field,
	const TCHAR* What, FWoTSha256& Out)
{
	FString Hex;
	if (TOptional<FWoTBootstrapError> Error =
		ReadStringField(Object, Field, What, Hex); Error.IsSet())
	{
		return Error;
	}
	TWoTBootstrapResult<FWoTSha256> Parsed = FWoTSha256::FromHex(Hex);
	if (Parsed.IsError())
	{
		return MoveTemp(Parsed).GetError();
	}
	Out = MoveTemp(Parsed).GetValue();
	return {};
}

bool WoTWorldReadString(const FWoTCanonicalJsonValue& Object,
	const TCHAR* Field, const TCHAR* What, FString& Out)
{
	return !ReadStringField(Object, Field, What, Out).IsSet();
}

bool WoTWorldReadU32(const FWoTCanonicalJsonValue& Object,
	const TCHAR* Field, const TCHAR* What, uint32& Out)
{
	return !ReadUnsigned32Field(Object, Field, What, Out).IsSet();
}

bool WoTWorldReadSha(const FWoTCanonicalJsonValue& Object,
	const TCHAR* Field, const TCHAR* What, FWoTSha256& Out)
{
	return !WoTWorldReadDigest(Object, Field, What, Out).IsSet();
}

TWoTBootstrapResult<FWoTBSPPartDescriptor> WoTWorldParsePart(
	const FWoTCanonicalJsonValue& Value)
{
	using FResult = TWoTBootstrapResult<FWoTBSPPartDescriptor>;
	static const TCHAR* const Fields[] = {
		TEXT("fact_id"), TEXT("index_count"), TEXT("logical_id"),
		TEXT("part_identity"), TEXT("payload_sha256"),
		TEXT("procedural_material_slot"), TEXT("role"), TEXT("schema"),
		TEXT("section_count"), TEXT("sha256"), TEXT("vertex_count")};
	if (TOptional<FWoTBootstrapError> Error = RequireExactMembers(
			Value, Fields, TEXT("world descriptor BSP part")); Error.IsSet())
	{
		return FResult::Error(MoveTemp(Error.GetValue()));
	}
	FWoTBSPPartDescriptor Part;
	FString Role;
	if (!WoTWorldReadString(Value, TEXT("fact_id"),
			TEXT("world descriptor BSP part"), Part.FactId)
		|| !WoTWorldReadU32(Value, TEXT("index_count"),
			TEXT("world descriptor BSP part"), Part.IndexCount)
		|| !WoTWorldReadString(Value, TEXT("logical_id"),
			TEXT("world descriptor BSP part"), Part.LogicalId)
		|| !WoTWorldReadString(Value, TEXT("part_identity"),
			TEXT("world descriptor BSP part"), Part.PartIdentity)
		|| !WoTWorldReadSha(Value, TEXT("payload_sha256"),
			TEXT("world descriptor BSP part"), Part.PayloadSha256)
		|| !WoTWorldReadString(Value, TEXT("procedural_material_slot"),
			TEXT("world descriptor BSP part"), Part.ProceduralMaterialSlot)
		|| !WoTWorldReadString(Value, TEXT("role"),
			TEXT("world descriptor BSP part"), Role)
		|| !WoTWorldReadString(Value, TEXT("schema"),
			TEXT("world descriptor BSP part"), Part.Schema)
		|| !WoTWorldReadU32(Value, TEXT("section_count"),
			TEXT("world descriptor BSP part"), Part.SectionCount)
		|| !WoTWorldReadSha(Value, TEXT("sha256"),
			TEXT("world descriptor BSP part"), Part.Sha256)
		|| !WoTWorldReadU32(Value, TEXT("vertex_count"),
			TEXT("world descriptor BSP part"), Part.VertexCount)
		|| (Role != TEXT("main") && Role != TEXT("portal")))
	{
		return WoTWorldSidecarError<FWoTBSPPartDescriptor>(
			EWoTBootstrapError::UnknownField,
			TEXT("a world descriptor BSP part is malformed"));
	}
	Part.Role = Role == TEXT("portal")
		? EWoTBSPPartRole::Portal : EWoTBSPPartRole::Main;
	return FResult::Success(MoveTemp(Part));
}

TWoTBootstrapResult<FWoTWorldDescriptor> WoTWorldParseDescriptor(
	const FWoTCanonicalJsonValue& Value)
{
	using FResult = TWoTBootstrapResult<FWoTWorldDescriptor>;
	static const TCHAR* const Fields[] = {
		TEXT("actor_count"), TEXT("attachment_count"), TEXT("bsp_fact_id"),
		TEXT("bsp_importer"), TEXT("bsp_node_count"), TEXT("bsp_parts"),
		TEXT("bsp_surface_count"), TEXT("light_count"), TEXT("map_identity"),
		TEXT("map_root"), TEXT("mover_count"), TEXT("navigation_seed_count"),
		TEXT("neutral_map_slot"), TEXT("package_sha256"),
		TEXT("portal_surface_count"), TEXT("reach_spec_count"),
		TEXT("semantic_object_count"), TEXT("sound_count"), TEXT("source_root"),
		TEXT("world_fact_id"), TEXT("world_importer"), TEXT("world_logical_id"),
		TEXT("world_schema"), TEXT("world_sha256"), TEXT("zone_count")};
	if (TOptional<FWoTBootstrapError> Error = RequireExactMembers(
			Value, Fields, TEXT("world descriptor")); Error.IsSet())
	{
		return FResult::Error(MoveTemp(Error.GetValue()));
	}
	FWoTWorldDescriptor Descriptor;
	if (!WoTWorldReadU32(Value, TEXT("actor_count"), TEXT("world descriptor"),
			Descriptor.ActorCount)
		|| !WoTWorldReadU32(Value, TEXT("attachment_count"),
			TEXT("world descriptor"), Descriptor.AttachmentCount)
		|| !WoTWorldReadString(Value, TEXT("bsp_fact_id"),
			TEXT("world descriptor"), Descriptor.BSPFactId)
		|| !WoTWorldReadString(Value, TEXT("bsp_importer"),
			TEXT("world descriptor"), Descriptor.BSPImporter)
		|| !WoTWorldReadU32(Value, TEXT("bsp_node_count"),
			TEXT("world descriptor"), Descriptor.BSPNodeCount)
		|| !WoTWorldReadU32(Value, TEXT("bsp_surface_count"),
			TEXT("world descriptor"), Descriptor.BSPSurfaceCount)
		|| !WoTWorldReadU32(Value, TEXT("light_count"), TEXT("world descriptor"),
			Descriptor.LightCount)
		|| !WoTWorldReadString(Value, TEXT("map_identity"),
			TEXT("world descriptor"), Descriptor.MapIdentity)
		|| !WoTWorldReadSha(Value, TEXT("map_root"), TEXT("world descriptor"),
			Descriptor.MapRoot)
		|| !WoTWorldReadU32(Value, TEXT("mover_count"), TEXT("world descriptor"),
			Descriptor.MoverCount)
		|| !WoTWorldReadU32(Value, TEXT("navigation_seed_count"),
			TEXT("world descriptor"), Descriptor.NavigationSeedCount)
		|| !WoTWorldReadU32(Value, TEXT("neutral_map_slot"),
			TEXT("world descriptor"), Descriptor.NeutralMapSlot)
		|| !WoTWorldReadSha(Value, TEXT("package_sha256"),
			TEXT("world descriptor"), Descriptor.PackageSha256)
		|| !WoTWorldReadU32(Value, TEXT("portal_surface_count"),
			TEXT("world descriptor"), Descriptor.PortalSurfaceCount)
		|| !WoTWorldReadU32(Value, TEXT("reach_spec_count"),
			TEXT("world descriptor"), Descriptor.ReachSpecCount)
		|| !WoTWorldReadU32(Value, TEXT("semantic_object_count"),
			TEXT("world descriptor"), Descriptor.SemanticObjectCount)
		|| !WoTWorldReadU32(Value, TEXT("sound_count"), TEXT("world descriptor"),
			Descriptor.SoundCount)
		|| !WoTWorldReadSha(Value, TEXT("source_root"), TEXT("world descriptor"),
			Descriptor.SourceRoot)
		|| !WoTWorldReadString(Value, TEXT("world_fact_id"),
			TEXT("world descriptor"), Descriptor.WorldFactId)
		|| !WoTWorldReadString(Value, TEXT("world_importer"),
			TEXT("world descriptor"), Descriptor.WorldImporter)
		|| !WoTWorldReadString(Value, TEXT("world_logical_id"),
			TEXT("world descriptor"), Descriptor.WorldLogicalId)
		|| !WoTWorldReadString(Value, TEXT("world_schema"),
			TEXT("world descriptor"), Descriptor.WorldSchema)
		|| !WoTWorldReadSha(Value, TEXT("world_sha256"),
			TEXT("world descriptor"), Descriptor.WorldSha256)
		|| !WoTWorldReadU32(Value, TEXT("zone_count"), TEXT("world descriptor"),
			Descriptor.ZoneCount))
	{
		return WoTWorldSidecarError<FWoTWorldDescriptor>(
			EWoTBootstrapError::UnknownField,
			TEXT("a world descriptor is malformed"));
	}
	const FWoTCanonicalJsonValue* Parts = Value.FindMember(TEXT("bsp_parts"));
	if (Parts == nullptr || !Parts->IsArray() || Parts->Num() == 0)
	{
		return WoTWorldSidecarError<FWoTWorldDescriptor>(
			EWoTBootstrapError::UnknownField,
			TEXT("a world descriptor has no BSP parts"));
	}
	for (int32 Index = 0; Index < Parts->Num(); ++Index)
	{
		TWoTBootstrapResult<FWoTBSPPartDescriptor> Part =
			WoTWorldParsePart(Parts->GetElement(Index));
		if (Part.IsError())
		{
			return FResult::Error(MoveTemp(Part).GetError());
		}
		Descriptor.BSPParts.Add(MoveTemp(Part).GetValue());
	}
	return FResult::Success(MoveTemp(Descriptor));
}
}

TWoTBootstrapResult<FWoTCanonicalJsonValue> WriteCacheWorldDescriptors(
    TConstArrayView<FWoTWorldDescriptor> Descriptors)
{
    using FResult = TWoTBootstrapResult<FWoTCanonicalJsonValue>;
    if (Descriptors.IsEmpty())
        return FResult::Success(FWoTCanonicalJsonValue::MakeArray());
    if (Descriptors.Num() != WoTWorldDescriptorCount)
        return WoTWorldSidecarError<FWoTCanonicalJsonValue>(
            EWoTBootstrapError::CorruptInstallation,
            TEXT("world descriptor publication requires nineteen maps"));
    TArray<FWoTWorldDescriptor> Ordered(Descriptors);
    Ordered.Sort([](const FWoTWorldDescriptor& Left,
        const FWoTWorldDescriptor& Right)
    {
        return Left.NeutralMapSlot < Right.NeutralMapSlot;
    });
    for (int32 Index = 0; Index < Ordered.Num(); ++Index)
    {
        if (Ordered[Index].NeutralMapSlot != static_cast<uint32>(Index + 1))
            return WoTWorldSidecarError<FWoTCanonicalJsonValue>(
                EWoTBootstrapError::CorruptInstallation,
                TEXT("world descriptor publication has an invalid slot census"));
    }
    return WoTWorldDescriptorArray(Ordered);
}

TWoTBootstrapResult<TArray<FWoTWorldDescriptor>> ParseCacheWorldDescriptors(
    const FWoTCanonicalJsonValue& Descriptors)
{
    using FResult = TWoTBootstrapResult<TArray<FWoTWorldDescriptor>>;
    if (!Descriptors.IsArray()
        || (Descriptors.Num() != 0 && Descriptors.Num() != WoTWorldDescriptorCount))
        return WoTWorldSidecarError<TArray<FWoTWorldDescriptor>>(
            EWoTBootstrapError::UnknownField,
            TEXT("the manifest has the wrong world descriptor census"));
    TArray<FWoTWorldDescriptor> Result;
    Result.Reserve(Descriptors.Num());
    for (int32 Index = 0; Index < Descriptors.Num(); ++Index)
    {
        TWoTBootstrapResult<FWoTWorldDescriptor> Descriptor =
            WoTWorldParseDescriptor(Descriptors.GetElement(Index));
        if (Descriptor.IsError()) return FResult::Error(MoveTemp(Descriptor).GetError());
        if (Descriptor.GetValue().NeutralMapSlot != static_cast<uint32>(Index + 1))
            return WoTWorldSidecarError<TArray<FWoTWorldDescriptor>>(
                EWoTBootstrapError::CorruptInstallation,
                TEXT("manifest world descriptors are not in canonical slot order"));
        Result.Add(MoveTemp(Descriptor).GetValue());
    }
    return FResult::Success(MoveTemp(Result));
}
