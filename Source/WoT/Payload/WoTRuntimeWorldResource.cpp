#include "WoTRuntimeWorldResource.h"

#include "../World/WoTRuntimeWorldProperties.h"

#include "Containers/StringConv.h"

namespace
{
FWoTBootstrapStatus WoTRuntimeWorldError(const TCHAR* Message)
{
	return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
		EWoTBootstrapError::CorruptInstallation, Message));
}

// Overload for composed diagnostics; the caller's FString outlives the call,
// so this avoids relying on a temporary's lifetime through a raw pointer.
FWoTBootstrapStatus WoTRuntimeWorldError(const FString& Message)
{
	return WoTRuntimeWorldError(*Message);
}

class FWoTRuntimeWorldCursor final
{
public:
	explicit FWoTRuntimeWorldCursor(TConstArrayView<uint8> InBytes)
		: Bytes(InBytes)
	{
	}

	bool Skip(uint64 Count)
	{
		if (Count > static_cast<uint64>(MAX_int32)
			|| static_cast<uint64>(Offset) + Count
				> static_cast<uint64>(Bytes.Num()))
		{
			return false;
		}
		Offset += static_cast<int32>(Count);
		return true;
	}

	bool ReadU8(uint8& Out)
	{
		if (Offset >= Bytes.Num())
		{
			return false;
		}
		Out = Bytes[Offset++];
		return true;
	}

	bool ReadU16(uint16& Out)
	{
		if (Offset + 2 > Bytes.Num())
		{
			return false;
		}
		Out = static_cast<uint16>(Bytes[Offset])
			| (static_cast<uint16>(Bytes[Offset + 1]) << 8);
		Offset += 2;
		return true;
	}

	bool ReadU32(uint32& Out)
	{
		if (Offset + 4 > Bytes.Num())
		{
			return false;
		}
		Out = static_cast<uint32>(Bytes[Offset])
			| (static_cast<uint32>(Bytes[Offset + 1]) << 8)
			| (static_cast<uint32>(Bytes[Offset + 2]) << 16)
			| (static_cast<uint32>(Bytes[Offset + 3]) << 24);
		Offset += 4;
		return true;
	}

	bool ReadI32(int32& Out)
	{
		uint32 Bits = 0;
		if (!ReadU32(Bits))
		{
			return false;
		}
		FMemory::Memcpy(&Out, &Bits, sizeof(Out));
		return true;
	}

	bool ReadU64(uint64& Out)
	{
		uint32 Low = 0;
		uint32 High = 0;
		if (!ReadU32(Low) || !ReadU32(High))
		{
			return false;
		}
		Out = static_cast<uint64>(Low)
			| (static_cast<uint64>(High) << 32);
		return true;
	}

	bool ReadFloat(float& Out)
	{
		uint32 Bits = 0;
		if (!ReadU32(Bits))
		{
			return false;
		}
		FMemory::Memcpy(&Out, &Bits, sizeof(Out));
		return FMath::IsFinite(Out);
	}

	bool ReadBytes(uint32 Count, TArray<uint8>& Out)
	{
		if (static_cast<uint64>(Offset) + Count
			> static_cast<uint64>(Bytes.Num()))
		{
			return false;
		}
		Out.Reset(static_cast<int32>(Count));
		Out.Append(Bytes.GetData() + Offset, static_cast<int32>(Count));
		Offset += static_cast<int32>(Count);
		return true;
	}

	bool ReadString(FString& Out, bool bOptional = false)
	{
		uint32 Count = 0;
		if (!ReadU32(Count) || Count > 1024u * 1024u
			|| (!bOptional && Count == 0)
			|| static_cast<uint64>(Offset) + Count
				> static_cast<uint64>(Bytes.Num()))
		{
			return false;
		}
		if (Count == 0)
		{
			Out.Empty();
			return true;
		}
		const FUTF8ToTCHAR Converted(
			reinterpret_cast<const ANSICHAR*>(Bytes.GetData() + Offset),
			static_cast<int32>(Count));
		if (Converted.Length() <= 0)
		{
			return false;
		}
		Out = FString(Converted.Length(), Converted.Get());
		FTCHARToUTF8 RoundTrip(*Out);
		if (RoundTrip.Length() != static_cast<int32>(Count)
			|| FMemory::Memcmp(
				RoundTrip.Get(), Bytes.GetData() + Offset, Count) != 0)
		{
			return false;
		}
		Offset += static_cast<int32>(Count);
		return true;
	}

	bool AtEnd() const { return Offset == Bytes.Num(); }
	int32 Tell() const { return Offset; }
	TConstArrayView<uint8> View() const { return Bytes; }
	bool SetOffset(int32 NewOffset)
	{
		if (NewOffset < 0 || NewOffset > Bytes.Num()) return false;
		Offset = NewOffset;
		return true;
	}

private:
	TConstArrayView<uint8> Bytes;
	int32 Offset = 0;
};

bool WoTRuntimeWorldReadU32(
	const FWoTCanonicalJsonValue& Header, const TCHAR* Field, uint32& Out)
{
	return !ReadUnsigned32Field(
		Header, Field, TEXT("world header"), Out).IsSet();
}

bool WoTRuntimeWorldReadString(
	const FWoTCanonicalJsonValue& Header, const TCHAR* Field, FString& Out)
{
	return !ReadStringField(
		Header, Field, TEXT("world header"), Out).IsSet();
}

bool WoTRuntimeWorldSourceValid(
	const FWoTCanonicalJsonValue& Header, FString& OutExport,
	FString& OutPackage, FString& OutHash)
{
	const FWoTCanonicalJsonValue* Source = nullptr;
	static const TCHAR* Fields[] = {
		TEXT("export"), TEXT("package"), TEXT("sha256")
	};
	return !ReadObjectField(
			Header, TEXT("source"), TEXT("world header"), Source).IsSet()
		&& Source != nullptr
		&& !RequireExactMembers(
			*Source, MakeArrayView(Fields), TEXT("world source")).IsSet()
		&& WoTRuntimeWorldReadString(*Source, TEXT("export"), OutExport)
		&& WoTRuntimeWorldReadString(*Source, TEXT("package"), OutPackage)
		&& WoTRuntimeWorldReadString(*Source, TEXT("sha256"), OutHash)
		&& !OutExport.IsEmpty() && !OutPackage.IsEmpty()
		&& OutHash.Len() == 64;
}

struct FWoTRuntimeWorldSemanticIdentity
{
	FString ClassIdentity;
	bool bActor = false;
};

bool WoTRuntimeWorldProperties(
	FWoTRuntimeWorldCursor& Cursor,
	const TMap<FString, FString>& SemanticClasses,
	TArray<FWoTRuntimeWorldProperty>& OutProperties,
	FString* OutReason, bool bDecodeStructMembers)
{
	int32 Offset = Cursor.Tell();
	FString Error;
	if (!WoTRuntimeWorldDecodeProperties(Cursor.View(), Offset,
		OutProperties, Error, &SemanticClasses, bDecodeStructMembers,
		EWoTStructMemberPolicy::WorldResolvedSchema)
		|| !Cursor.SetOffset(Offset))
	{
		if (OutReason != nullptr) *OutReason = MoveTemp(Error);
		return false;
	}
	return true;
}

bool WoTRuntimeWorldAttachmentsValid(
	TConstArrayView<int32> ParentByActor)
{
	TArray<uint8> State;
	State.SetNumZeroed(ParentByActor.Num());
	for (int32 Start = 0; Start < ParentByActor.Num(); ++Start)
	{
		int32 Current = Start;
		TArray<int32> Chain;
		while (Current >= 0)
		{
			if (Current >= ParentByActor.Num() || State[Current] == 1)
			{
				return false;
			}
			if (State[Current] == 2)
			{
				break;
			}
			State[Current] = 1;
			Chain.Add(Current);
			Current = ParentByActor[Current];
		}
		for (const int32 ActorIndex : Chain)
		{
			State[ActorIndex] = 2;
		}
	}
	return true;
}

bool WoTRuntimeWorldOptionalIndex(int32 Index, int32 Count)
{
	return Index == INDEX_NONE || (Index >= 0 && Index < Count);
}

bool WoTRuntimeWorldResponseRectFits(uint32 X, uint32 Y, uint32 W, uint32 H,
	uint32 AtlasWidth, uint32 AtlasHeight)
{
	return X >= 1 && Y >= 1 && W >= 1 && H >= 1
		&& static_cast<uint64>(X) + W < AtlasWidth
		&& static_cast<uint64>(Y) + H < AtlasHeight;
}
}

#if WITH_AUTOMATION_TESTS
bool WoTRuntimeWorldResponseRectFitsForTesting(uint32 X, uint32 Y,
	uint32 W, uint32 H, uint32 AtlasWidth, uint32 AtlasHeight)
{
	return WoTRuntimeWorldResponseRectFits(
		X, Y, W, H, AtlasWidth, AtlasHeight);
}
#endif

FWoTBootstrapStatus FWoTRuntimeWorldResource::LoadFirstUse(
	const FWoTNeutralResourceId& Resource)
{
	if (Object.IsSet() || Resource.Kind != EWoTNeutralResourceKind::World)
	{
		return WoTRuntimeWorldError(
			TEXT("world resource was reused or has the wrong semantic kind"));
	}
	TWoTBootstrapResult<FWoTRuntimeVerifiedObject> Read =
		FWoTRuntimeRegistry::Get().ReadObject(
			Resource, 1024ull * 1024ull * 1024ull,
			TEXT("ue1-world-records-le-v6"), TEXT("world"),
			FStringView(),
			TEXT("ue1-world-records-le-v5"),
			TEXT("ue1-world-records-le-v4"),
			TEXT("ue1-world-records-le-v3"),
			TEXT("ue1-world-records-le-v2"));
	if (Read.IsError())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(Read).GetError());
	}
	return LoadVerifiedObject(MoveTemp(Read).GetValue());
}

#if WITH_AUTOMATION_TESTS
FWoTBootstrapStatus FWoTRuntimeWorldResource::LoadObjectForTesting(
	FWoTRuntimeVerifiedObject&& InObject)
{
	if (Object.IsSet())
	{
		return WoTRuntimeWorldError(TEXT("world resource was reused"));
	}
	return LoadVerifiedObject(MoveTemp(InObject));
}
#endif

FWoTBootstrapStatus FWoTRuntimeWorldResource::LoadVerifiedObject(
	FWoTRuntimeVerifiedObject&& Verified)
{
	const FWoTCanonicalJsonValue& Header = Verified.Header();
	static const TCHAR* HeaderFields[] = {
		TEXT("actor_count"), TEXT("attachment_count"),
		TEXT("bsp_node_count"), TEXT("bsp_surface_count"),
		TEXT("encoding"), TEXT("fact_id"), TEXT("importer"),
		TEXT("importer_version"), TEXT("kind"), TEXT("light_count"),
		TEXT("map_identity"), TEXT("mover_count"),
		TEXT("navigation_seed_count"), TEXT("payload_byte_length"),
		TEXT("portal_surface_count"), TEXT("reach_spec_count"),
		TEXT("semantic_object_count"), TEXT("sound_count"),
		TEXT("source"), TEXT("zone_count")
	};
	uint32 ActorCount = 0;
	uint32 AttachmentCount = 0;
	uint32 NodeCount = 0;
	uint32 SurfaceCount = 0;
	uint32 ReachCount = 0;
	uint32 SemanticCount = 0;
	uint32 ZoneCount = 0;
	uint32 ImporterVersion = 0;
	uint32 LightCount = 0;
	uint32 MoverCount = 0;
	uint32 NavigationSeedCount = 0;
	uint32 PortalSurfaceCount = 0;
	uint32 SoundCount = 0;
	FString FactId;
	FString Importer;
	FString MapIdentity;
	FString SourceExport;
	FString SourcePackage;
	FString SourceHash;
	if (RequireExactMembers(
			Header, MakeArrayView(HeaderFields), TEXT("world header")).IsSet()
		|| !WoTRuntimeWorldReadU32(
			Header, TEXT("actor_count"), ActorCount)
		|| !WoTRuntimeWorldReadU32(
			Header, TEXT("attachment_count"), AttachmentCount)
		|| !WoTRuntimeWorldReadU32(
			Header, TEXT("bsp_node_count"), NodeCount)
		|| !WoTRuntimeWorldReadU32(
			Header, TEXT("bsp_surface_count"), SurfaceCount)
		|| !WoTRuntimeWorldReadU32(
			Header, TEXT("reach_spec_count"), ReachCount)
		|| !WoTRuntimeWorldReadU32(
			Header, TEXT("semantic_object_count"), SemanticCount)
		|| !WoTRuntimeWorldReadU32(
			Header, TEXT("zone_count"), ZoneCount)
		|| !WoTRuntimeWorldReadU32(
			Header, TEXT("importer_version"), ImporterVersion)
		|| !WoTRuntimeWorldReadU32(
			Header, TEXT("light_count"), LightCount)
		|| !WoTRuntimeWorldReadU32(
			Header, TEXT("mover_count"), MoverCount)
		|| !WoTRuntimeWorldReadU32(
			Header, TEXT("navigation_seed_count"), NavigationSeedCount)
		|| !WoTRuntimeWorldReadU32(
			Header, TEXT("portal_surface_count"), PortalSurfaceCount)
		|| !WoTRuntimeWorldReadU32(
			Header, TEXT("sound_count"), SoundCount)
		|| !WoTRuntimeWorldReadString(Header, TEXT("fact_id"), FactId)
		|| !WoTRuntimeWorldReadString(Header, TEXT("importer"), Importer)
		|| !WoTRuntimeWorldReadString(
			Header, TEXT("map_identity"), MapIdentity)
		|| ActorCount > 1000000 || AttachmentCount > 1000000
		|| NodeCount > 2000000
		|| SurfaceCount > 2000000 || ReachCount > 10000000
		|| SemanticCount > 4000000 || ZoneCount > 64
		|| LightCount > ActorCount || MoverCount > ActorCount
		|| NavigationSeedCount > ActorCount || SoundCount > ActorCount
		|| PortalSurfaceCount > SurfaceCount
		|| FactId.IsEmpty() || MapIdentity.IsEmpty()
		|| Importer != TEXT("world-v1")
		|| (ImporterVersion != 2 && ImporterVersion != 3
			&& ImporterVersion != 4 && ImporterVersion != 5
			&& ImporterVersion != 6)
		|| !WoTRuntimeWorldSourceValid(
			Header, SourceExport, SourcePackage, SourceHash))
	{
		return WoTRuntimeWorldError(
			TEXT("world canonical header is malformed"));
	}

	const TConstArrayView<uint8> Payload = Verified.Payload();
	FWoTRuntimeWorldCursor Cursor(Payload);
	uint16 Version = 0;
	uint16 Flags = 0;
	uint32 Counts[13] = {};
	uint32 RootOutside = 0;
	if (Payload.Num() < 52
		|| FMemory::Memcmp(Payload.GetData(), "WOTWRL3", 8) != 0
		|| !Cursor.Skip(8) || !Cursor.ReadU16(Version)
		|| !Cursor.ReadU16(Flags))
	{
		return WoTRuntimeWorldError(
			TEXT("world payload framing is malformed"));
	}
	const uint32 CountFields = Version == 2 ? 10u
		: (Version >= 3 && Version <= 6) ? 13u : 0u;
	for (uint32 Index = 0; Index < CountFields; ++Index)
	{
		if (!Cursor.ReadU32(Counts[Index]))
		{
			return WoTRuntimeWorldError(
				TEXT("world payload count table is truncated"));
		}
	}
	if ((Version >= 3 && !Cursor.ReadU32(RootOutside))
		|| CountFields == 0 || Flags != 0 || RootOutside > 1
		|| ImporterVersion != Version
		|| Counts[0] != ActorCount
		|| Counts[1] != AttachmentCount || Counts[4] != NodeCount
		|| Counts[5] != SurfaceCount || Counts[7] != ZoneCount
		|| Counts[8] != SemanticCount || Counts[9] != ReachCount
		|| Counts[2] > 2000000 || Counts[3] > 2000000
		|| Counts[6] > 6000000 || Counts[10] > 2000000
		|| Counts[11] > 4000000
		|| (Version >= 3 && Counts[12] != MoverCount))
	{
		return WoTRuntimeWorldError(
			TEXT("world payload counts or fixed tables are malformed"));
	}

	TArray<FVector3f> Vectors;
	Vectors.Reserve(static_cast<int32>(Counts[2]));
	for (uint32 Index = 0; Index < Counts[2]; ++Index)
	{
		FVector3f& Vector = Vectors.AddDefaulted_GetRef();
		if (!Cursor.ReadFloat(Vector.X) || !Cursor.ReadFloat(Vector.Y)
			|| !Cursor.ReadFloat(Vector.Z))
		{
			return WoTRuntimeWorldError(
				TEXT("world vector table is malformed"));
		}
	}
	TArray<FVector3f> Points;
	Points.Reserve(static_cast<int32>(Counts[3]));
	for (uint32 Index = 0; Index < Counts[3]; ++Index)
	{
		FVector3f& Point = Points.AddDefaulted_GetRef();
		if (!Cursor.ReadFloat(Point.X) || !Cursor.ReadFloat(Point.Y)
			|| !Cursor.ReadFloat(Point.Z))
		{
			return WoTRuntimeWorldError(
				TEXT("world point table is malformed"));
		}
	}
	TArray<FWoTRuntimeWorldNode> Nodes;
	Nodes.Reserve(static_cast<int32>(Counts[4]));
	for (uint32 Index = 0; Index < Counts[4]; ++Index)
	{
		FWoTRuntimeWorldNode& Node = Nodes.AddDefaulted_GetRef();
		if (!Cursor.ReadFloat(Node.Plane[0])
			|| !Cursor.ReadFloat(Node.Plane[1])
			|| !Cursor.ReadFloat(Node.Plane[2])
			|| !Cursor.ReadFloat(Node.Plane[3])
			|| !Cursor.ReadU64(Node.ZoneMask)
			|| !Cursor.ReadU8(Node.Flags)
			|| !Cursor.ReadI32(Node.VertexPool)
			|| !Cursor.ReadI32(Node.Surface)
			|| !Cursor.ReadI32(Node.Back)
			|| !Cursor.ReadI32(Node.Front)
			|| !Cursor.ReadI32(Node.PlaneNode)
			|| !Cursor.ReadI32(Node.CollisionBound)
			|| !Cursor.ReadI32(Node.RenderBound)
			|| !Cursor.ReadU8(Node.Zones[0])
			|| !Cursor.ReadU8(Node.Zones[1])
			|| !Cursor.ReadU8(Node.VertexCount)
			|| !Cursor.ReadI32(Node.Leaves[0])
			|| !Cursor.ReadI32(Node.Leaves[1]))
		{
			return WoTRuntimeWorldError(
				TEXT("world BSP node table is malformed"));
		}
	}
	TArray<FWoTRuntimeWorldSurface> Surfaces;
	Surfaces.Reserve(static_cast<int32>(Counts[5]));
	for (uint32 Index = 0; Index < Counts[5]; ++Index)
	{
		FWoTRuntimeWorldSurface& Surface =
			Surfaces.AddDefaulted_GetRef();
		uint16 PanU = 0;
		uint16 PanV = 0;
		if (!Cursor.ReadI32(Surface.Base)
			|| !Cursor.ReadI32(Surface.Normal)
			|| !Cursor.ReadI32(Surface.TextureU)
			|| !Cursor.ReadI32(Surface.TextureV)
			|| !Cursor.ReadI32(Surface.LightMap)
			|| !Cursor.ReadI32(Surface.BrushPoly)
			|| !Cursor.ReadU32(Surface.Flags)
			|| !Cursor.ReadU16(PanU) || !Cursor.ReadU16(PanV)
			|| (Version >= 3 && !Cursor.ReadFloat(Surface.Friction))
			|| !Cursor.ReadString(Surface.TextureIdentity, true)
			|| !Cursor.ReadString(Surface.ActorIdentity, true))
		{
			return WoTRuntimeWorldError(
				TEXT("world surface table is malformed"));
		}
		Surface.PanU = static_cast<int16>(PanU);
		Surface.PanV = static_cast<int16>(PanV);
	}
	TArray<FWoTRuntimeWorldVertex> Vertices;
	Vertices.Reserve(static_cast<int32>(Counts[6]));
	for (uint32 Index = 0; Index < Counts[6]; ++Index)
	{
		FWoTRuntimeWorldVertex& Vertex = Vertices.AddDefaulted_GetRef();
		if (!Cursor.ReadI32(Vertex.Point) || !Cursor.ReadI32(Vertex.Side))
		{
			return WoTRuntimeWorldError(
				TEXT("world vertex table is malformed"));
		}
	}
	TArray<FWoTRuntimeWorldZone> Zones;
	Zones.Reserve(static_cast<int32>(Counts[7]));
	for (uint32 Index = 0; Index < Counts[7]; ++Index)
	{
		FWoTRuntimeWorldZone& Zone = Zones.AddDefaulted_GetRef();
		if (!Cursor.ReadString(Zone.ActorIdentity, true)
			|| !Cursor.ReadU64(Zone.Connectivity)
			|| !Cursor.ReadU64(Zone.Visibility))
		{
			return WoTRuntimeWorldError(
				TEXT("world zone table is malformed"));
		}
	}
	TArray<FWoTRuntimeWorldLeaf> Leaves;
	Leaves.Reserve(static_cast<int32>(Counts[10]));
	for (uint32 Index = 0; Index < Counts[10]; ++Index)
	{
		FWoTRuntimeWorldLeaf& Leaf = Leaves.AddDefaulted_GetRef();
		if (!Cursor.ReadI32(Leaf.Zone)
			|| !Cursor.ReadI32(Leaf.Permeating)
			|| !Cursor.ReadI32(Leaf.Volumetric)
			|| Leaf.Zone < 0 || Leaf.Zone >= 64
			|| !WoTRuntimeWorldOptionalIndex(
				Leaf.Permeating, static_cast<int32>(Counts[11]))
			|| !WoTRuntimeWorldOptionalIndex(
				Leaf.Volumetric, static_cast<int32>(Counts[11])))
		{
			return WoTRuntimeWorldError(TEXT("world BSP leaf table is malformed"));
		}
	}
	TArray<uint32> ModelLights;
	ModelLights.Reserve(static_cast<int32>(Counts[11]));
	for (uint32 Index = 0; Index < Counts[11]; ++Index)
	{
		uint32& Light = ModelLights.AddDefaulted_GetRef();
		if (!Cursor.ReadU32(Light)
			|| (Light != MAX_uint32 && Light >= Counts[8]))
		{
			return WoTRuntimeWorldError(TEXT("world model light table is malformed"));
		}
	}
	TBitArray<> LightRunTerminates(false, ModelLights.Num());
	bool bHasSentinel = false;
	for (int32 Index = ModelLights.Num() - 1; Index >= 0; --Index)
	{
		bHasSentinel = bHasSentinel || ModelLights[Index] == MAX_uint32;
		LightRunTerminates[Index] = bHasSentinel;
	}
	for (const FWoTRuntimeWorldLeaf& Leaf : Leaves)
	{
		for (int32 Run : {Leaf.Permeating, Leaf.Volumetric})
		{
			if (Run < 0) continue;
			if (!LightRunTerminates[Run])
			{
				return WoTRuntimeWorldError(
					TEXT("world BSP leaf light run lacks a null sentinel"));
			}
		}
	}
	TMap<FString, FWoTRuntimeWorldSemanticIdentity> SemanticIdentities;
	TArray<FWoTRuntimeWorldSemanticObject> SemanticObjects;
	SemanticObjects.Reserve(static_cast<int32>(Counts[8]));
	uint32 SemanticActorCount = 0;
	for (uint32 Index = 0; Index < Counts[8]; ++Index)
	{
		FString Identity;
		FString ClassIdentity;
		uint8 bActor = 0;
		FString FoldedIdentity;
		if (!Cursor.ReadString(Identity)
			|| !Cursor.ReadString(ClassIdentity)
			|| !Cursor.ReadU8(bActor) || bActor > 1
			|| (FoldedIdentity = Identity.ToLower()).IsEmpty()
			|| SemanticIdentities.Contains(FoldedIdentity))
		{
			return WoTRuntimeWorldError(
				TEXT("world semantic registry is malformed"));
		}
		FWoTRuntimeWorldSemanticIdentity Semantic;
		Semantic.ClassIdentity = ClassIdentity;
		Semantic.bActor = bActor != 0;
		SemanticActorCount += Semantic.bActor ? 1u : 0u;
		SemanticIdentities.Add(FoldedIdentity, Semantic);
		FWoTRuntimeWorldSemanticObject& Record =
			SemanticObjects.AddDefaulted_GetRef();
		Record.Identity = MoveTemp(Identity);
		Record.ClassIdentity = MoveTemp(ClassIdentity);
		Record.bActor = Semantic.bActor;
	}
	TMap<FString, FString> SemanticClasses;
	SemanticClasses.Reserve(SemanticIdentities.Num());
	for (const TPair<FString, FWoTRuntimeWorldSemanticIdentity>& Pair
		: SemanticIdentities)
	{
		SemanticClasses.Add(Pair.Key, Pair.Value.ClassIdentity);
	}
	for (uint32 Light : ModelLights)
	{
		if (Light == MAX_uint32) continue;
		if (!SemanticObjects.IsValidIndex(static_cast<int32>(Light))
			|| !SemanticObjects[static_cast<int32>(Light)].bActor)
		{
			return WoTRuntimeWorldError(
				TEXT("world model light is not an actor semantic object"));
		}
	}
	TMap<FString, int32> ActorIdentities;
	TArray<FWoTRuntimeWorldActor> Actors;
	Actors.Reserve(static_cast<int32>(Counts[0]));
	for (uint32 Index = 0; Index < Counts[0]; ++Index)
	{
		FWoTRuntimeWorldActor& Actor = Actors.AddDefaulted_GetRef();
		// Split rather than one fused predicate: this parse runs against the
		// NATIVE cache builder's real output, which no test covers (every
		// suite builds synthetic blobs via WoTWorldMaterializerTestData), and
		// a single "malformed" verdict cannot say which invariant real retail
		// data violates. The index/identity make the failing record locatable.
		if (!Cursor.ReadString(Actor.Identity)
			|| !Cursor.ReadString(Actor.ClassIdentity))
		{
			return WoTRuntimeWorldError(FString::Printf(
				TEXT("world actor table is malformed: actor %u identity/class read failed"),
				Index));
		}
		if (ActorIdentities.Contains(Actor.Identity.ToLower()))
		{
			return WoTRuntimeWorldError(FString::Printf(
				TEXT("world actor table is malformed: actor %u identity '%s' duplicates a prior actor when case-folded"),
				Index, *Actor.Identity));
		}
		FString PropertyReason;
		if (!WoTRuntimeWorldProperties(
			Cursor, SemanticClasses, Actor.Properties, &PropertyReason,
			Version >= 3))
		{
			return WoTRuntimeWorldError(FString::Printf(
				TEXT("world actor table is malformed: actor %u '%s' (class %s) typed properties: %s"),
				Index, *Actor.Identity, *Actor.ClassIdentity,
				*PropertyReason));
		}
		if (!Cursor.ReadString(Actor.ParentIdentity, true))
		{
			return WoTRuntimeWorldError(FString::Printf(
				TEXT("world actor table is malformed: actor %u '%s' parent identity read failed"),
				Index, *Actor.Identity));
		}
		if (!Actor.ParentIdentity.IsEmpty()
			&& !SemanticIdentities.Contains(Actor.ParentIdentity.ToLower()))
		{
			return WoTRuntimeWorldError(FString::Printf(
				TEXT("world actor table is malformed: actor %u '%s' names parent '%s' with no semantic registry entry"),
				Index, *Actor.Identity, *Actor.ParentIdentity));
		}
		const FWoTRuntimeWorldSemanticIdentity* Semantic =
			SemanticIdentities.Find(Actor.Identity.ToLower());
		if (Semantic == nullptr || !Semantic->bActor
			|| Semantic->ClassIdentity != Actor.ClassIdentity)
		{
			return WoTRuntimeWorldError(
				TEXT("world actor/semantic identity binding is malformed"));
		}
		ActorIdentities.Add(
			Actor.Identity.ToLower(), static_cast<int32>(Index));
	}
	TArray<int32> ParentByActor;
	ParentByActor.Init(INDEX_NONE, Actors.Num());
	uint32 ActualAttachmentCount = 0;
	for (int32 ActorIndex = 0; ActorIndex < Actors.Num(); ++ActorIndex)
	{
		const FString& Parent = Actors[ActorIndex].ParentIdentity;
		if (Parent.IsEmpty())
		{
			continue;
		}
		const int32* ParentIndex = ActorIdentities.Find(Parent.ToLower());
		if (ParentIndex == nullptr)
		{
			return WoTRuntimeWorldError(
				TEXT("world attachment parent is not an actor identity"));
		}
		ParentByActor[ActorIndex] = *ParentIndex;
		++ActualAttachmentCount;
	}
	if (SemanticActorCount != ActorCount
		|| ActualAttachmentCount != AttachmentCount
		|| !WoTRuntimeWorldAttachmentsValid(ParentByActor))
	{
		return WoTRuntimeWorldError(
			TEXT("world actor or attachment census is malformed"));
	}
	// Close every geometry and semantic boundary after the complete pass-one
	// identity registry exists. No string or fixed-table index is left dangling.
	for (const FWoTRuntimeWorldNode& Node : Nodes)
	{
		const int64 VertexEnd = static_cast<int64>(Node.VertexPool)
			+ static_cast<int64>(Node.VertexCount);
		if (!Surfaces.IsValidIndex(Node.Surface)
			|| Node.VertexPool < 0 || VertexEnd > Vertices.Num()
			|| !WoTRuntimeWorldOptionalIndex(Node.Back, Nodes.Num())
			|| !WoTRuntimeWorldOptionalIndex(Node.Front, Nodes.Num())
			|| !WoTRuntimeWorldOptionalIndex(Node.PlaneNode, Nodes.Num())
			|| Node.CollisionBound < INDEX_NONE
			|| Node.RenderBound < INDEX_NONE
			|| Node.Leaves[0] < INDEX_NONE || Node.Leaves[1] < INDEX_NONE
			|| (Version >= 3
				&& (!WoTRuntimeWorldOptionalIndex(Node.Leaves[0], Leaves.Num())
					|| !WoTRuntimeWorldOptionalIndex(Node.Leaves[1], Leaves.Num())))
			// UE1 nodes address the fixed 64-slot zone table, including empty
			// slots above the number of serialized zone records.
			|| Node.Zones[0] >= 64 || Node.Zones[1] >= 64)
		{
			return WoTRuntimeWorldError(
				TEXT("world BSP node reference is outside its decoded table"));
		}
	}
	uint32 ActualPortalCount = 0;
	for (const FWoTRuntimeWorldSurface& Surface : Surfaces)
	{
		const FWoTRuntimeWorldSemanticIdentity* Texture =
			Surface.TextureIdentity.IsEmpty() ? nullptr
			: SemanticIdentities.Find(Surface.TextureIdentity.ToLower());
		const FWoTRuntimeWorldSemanticIdentity* Actor =
			Surface.ActorIdentity.IsEmpty() ? nullptr
			: SemanticIdentities.Find(Surface.ActorIdentity.ToLower());
		if (!Points.IsValidIndex(Surface.Base)
			|| !Vectors.IsValidIndex(Surface.Normal)
			|| !Vectors.IsValidIndex(Surface.TextureU)
			|| !Vectors.IsValidIndex(Surface.TextureV)
			|| Surface.LightMap < INDEX_NONE
			|| Surface.BrushPoly < INDEX_NONE
			|| (!Surface.TextureIdentity.IsEmpty()
				&& (Texture == nullptr || Texture->bActor))
			|| (!Surface.ActorIdentity.IsEmpty()
				&& (Actor == nullptr || !Actor->bActor)))
		{
			return WoTRuntimeWorldError(
				TEXT("world BSP surface reference is outside its identity registry"));
		}
		ActualPortalCount += (Surface.Flags & 0x04000000u) != 0 ? 1u : 0u;
	}
	TBitArray<> ReachableVertices(false, Vertices.Num());
	for (const FWoTRuntimeWorldNode& Node : Nodes)
	{
		for (int32 Offset = 0; Offset < Node.VertexCount; ++Offset)
		{
			ReachableVertices[Node.VertexPool + Offset] = true;
		}
	}
	for (int32 VertexIndex = 0; VertexIndex < Vertices.Num(); ++VertexIndex)
	{
		const FWoTRuntimeWorldVertex& Vertex = Vertices[VertexIndex];
		if (Vertex.Side < INDEX_NONE
			|| (ReachableVertices[VertexIndex]
				&& !Points.IsValidIndex(Vertex.Point)))
		{
			return WoTRuntimeWorldError(
				TEXT("world BSP vertex reference is outside its decoded table"));
		}
	}
	for (const FWoTRuntimeWorldZone& Zone : Zones)
	{
		const FWoTRuntimeWorldSemanticIdentity* Actor =
			Zone.ActorIdentity.IsEmpty() ? nullptr
			: SemanticIdentities.Find(Zone.ActorIdentity.ToLower());
		if (!Zone.ActorIdentity.IsEmpty()
			&& (Actor == nullptr || !Actor->bActor))
		{
			return WoTRuntimeWorldError(
				TEXT("world BSP zone actor is outside its actor registry"));
		}
	}
	if (ActualPortalCount != PortalSurfaceCount)
	{
		return WoTRuntimeWorldError(
			TEXT("world BSP portal census does not match its header"));
	}

	TArray<FWoTRuntimeWorldReachSpec> ReachSpecs;
	ReachSpecs.Reserve(static_cast<int32>(Counts[9]));
	for (uint32 Index = 0; Index < Counts[9]; ++Index)
	{
		FWoTRuntimeWorldReachSpec& Reach =
			ReachSpecs.AddDefaulted_GetRef();
		uint8 bPruned = 0;
		if (!Cursor.ReadI32(Reach.Distance)
			|| !Cursor.ReadI32(Reach.CollisionRadius)
			|| !Cursor.ReadI32(Reach.CollisionHeight)
			|| !Cursor.ReadU32(Reach.ReachFlags)
			|| !Cursor.ReadU8(bPruned) || bPruned > 1
			|| !Cursor.ReadString(Reach.StartIdentity, true)
			|| !Cursor.ReadString(Reach.EndIdentity, true))
		{
			return WoTRuntimeWorldError(
				TEXT("world reach-spec table is malformed"));
		}
		Reach.bPruned = bPruned != 0;
		for (const FString* Endpoint : {
				&Reach.StartIdentity, &Reach.EndIdentity})
		{
			if (!Endpoint->IsEmpty())
			{
				const FWoTRuntimeWorldSemanticIdentity* Bound =
					SemanticIdentities.Find(Endpoint->ToLower());
				if (Bound == nullptr || !Bound->bActor)
				{
					return WoTRuntimeWorldError(
						TEXT("world reach-spec endpoint is not an actor identity"));
				}
			}
		}
	}
	TArray<FWoTRuntimeMoverModel> MoverModels;
	MoverModels.Reserve(static_cast<int32>(Counts[12]));
	TSet<FString> MoverActors;
	TSet<FString> MoverModelIdentities;
	for (uint32 MoverIndex = 0; MoverIndex < Counts[12]; ++MoverIndex)
	{
		FWoTRuntimeMoverModel& Mover = MoverModels.AddDefaulted_GetRef();
		uint32 SectionCount = 0, VertexCount = 0, IndexCount = 0;
		uint32 CollisionVertexCount = 0, CollisionIndexCount = 0;
		if (!Cursor.ReadString(Mover.ActorIdentity)
			|| !Cursor.ReadString(Mover.ModelIdentity)
			|| !Cursor.ReadU32(SectionCount) || !Cursor.ReadU32(VertexCount)
			|| !Cursor.ReadU32(IndexCount)
			|| (Version >= 4 && (!Cursor.ReadU32(CollisionVertexCount)
				|| !Cursor.ReadU32(CollisionIndexCount)))
			|| SectionCount == 0 || SectionCount > 4096
			|| VertexCount == 0 || VertexCount > 2000000
			|| IndexCount < 3 || IndexCount > 6000000 || IndexCount % 3
			|| (Version >= 4 && (
				(CollisionVertexCount == 0) != (CollisionIndexCount == 0)
				|| CollisionVertexCount > 2000000
				|| CollisionIndexCount > 6000000
				|| CollisionIndexCount % 3
				|| (CollisionIndexCount != 0 && CollisionIndexCount < 3)))
			|| !ActorIdentities.Contains(Mover.ActorIdentity.ToLower())
			|| MoverActors.Contains(Mover.ActorIdentity.ToLower()))
		{
			return WoTRuntimeWorldError(TEXT("world mover-model header is malformed"));
		}
		if (Version >= 4)
		{
			uint8 Lightmapped = 0;
			if (!Cursor.ReadU8(Lightmapped) || Lightmapped > 1)
				return WoTRuntimeWorldError(
					TEXT("world mover lightmapped flag is malformed"));
			Mover.bLightmapped = Lightmapped != 0;
		}
		const FWoTRuntimeWorldSemanticIdentity* ModelSemantic =
			SemanticIdentities.Find(Mover.ModelIdentity.ToLower());
		if (ModelSemantic == nullptr || ModelSemantic->bActor
			|| MoverModelIdentities.Contains(Mover.ModelIdentity.ToLower())
			|| !ModelSemantic->ClassIdentity.Equals(
				TEXT("Engine.Model"), ESearchCase::IgnoreCase))
		{
			return WoTRuntimeWorldError(
				TEXT("world mover model is outside the semantic registry"));
		}
		const int32* MoverActorIndex = ActorIdentities.Find(
			Mover.ActorIdentity.ToLower());
		const FWoTRuntimeWorldProperty* Brush = MoverActorIndex == nullptr
			? nullptr : WoTRuntimeWorldFindProperty(
				Actors[*MoverActorIndex], TEXT("Brush"));
		if (Brush == nullptr
			|| Brush->Type != EWoTRuntimeWorldPropertyType::Object
			|| !Brush->ObjectIdentity.Equals(
				Mover.ModelIdentity, ESearchCase::IgnoreCase)
			|| !Brush->ObjectClassIdentity.Equals(
				TEXT("Engine.Model"), ESearchCase::IgnoreCase))
		{
			return WoTRuntimeWorldError(
				TEXT("world mover actor Brush does not match its mover model"));
		}
		MoverActors.Add(Mover.ActorIdentity.ToLower());
		MoverModelIdentities.Add(Mover.ModelIdentity.ToLower());
		uint32 ExpectedFirst = 0;
		TSet<FString> Slots;
		Mover.Sections.Reserve(static_cast<int32>(SectionCount));
		for (uint32 SectionIndex = 0; SectionIndex < SectionCount; ++SectionIndex)
		{
			FWoTRuntimeMoverSection& Section =
				Mover.Sections.AddDefaulted_GetRef();
			uint8 Collision = 0, Animated = 0, Reserved[2] = {};
			if (!Cursor.ReadU32(Section.FirstIndex)
				|| !Cursor.ReadU32(Section.IndexCount)
				|| !Cursor.ReadU32(Section.MinVertex)
				|| !Cursor.ReadU32(Section.MaxVertex)
				|| !Cursor.ReadU8(Collision)
				|| !Cursor.ReadU8(Animated) || !Cursor.ReadU8(Reserved[0])
				|| !Cursor.ReadU8(Reserved[1])
				|| !Cursor.ReadU32(Section.SurfaceFlags)
				|| !Cursor.ReadFloat(Section.PlaneOrigin.X)
				|| !Cursor.ReadFloat(Section.PlaneOrigin.Y)
				|| !Cursor.ReadFloat(Section.PlaneOrigin.Z)
				|| !Cursor.ReadFloat(Section.PlaneNormal.X)
				|| !Cursor.ReadFloat(Section.PlaneNormal.Y)
				|| !Cursor.ReadFloat(Section.PlaneNormal.Z)
				|| !Cursor.ReadString(Section.MaterialSlot)
				|| !Cursor.ReadString(Section.CanonicalPackage, true)
				|| !Cursor.ReadString(Section.CanonicalName)
				|| !Cursor.ReadString(Section.SourceTextureIdentity)
				|| (Version >= 5 && (
					!Cursor.ReadString(Section.MacroCanonicalPackage, true)
					|| !Cursor.ReadString(Section.MacroCanonicalName, true)
					|| !Cursor.ReadString(Section.MacroSourceTextureIdentity, true)
					|| !Cursor.ReadFloat(Section.MacroRatio)))
				|| Collision > 1 || Animated > (Version >= 6 ? 1 : 0)
				|| Reserved[0] != 0 || Reserved[1] != 0
				|| Section.FirstIndex != ExpectedFirst
				|| Section.IndexCount == 0 || Section.IndexCount % 3
				|| Section.IndexCount > IndexCount - ExpectedFirst
				|| Section.MinVertex > Section.MaxVertex
				|| Section.MaxVertex >= VertexCount
				|| !FMath::IsNearlyEqual(
					Section.PlaneNormal.SquaredLength(), 1.0f, 0.02f)
				|| Slots.Contains(Section.MaterialSlot))
			{
				return WoTRuntimeWorldError(
					TEXT("world mover-model section is malformed"));
			}
			const bool bHasMacro = !Section.MacroCanonicalPackage.IsEmpty()
				|| !Section.MacroCanonicalName.IsEmpty()
				|| !Section.MacroSourceTextureIdentity.IsEmpty()
				|| Section.MacroRatio != 0.0f;
			if (bHasMacro && (Section.MacroCanonicalPackage.IsEmpty()
				|| Section.MacroCanonicalName.IsEmpty()
				|| Section.MacroSourceTextureIdentity.IsEmpty()
				|| !FMath::IsFinite(Section.MacroRatio)
				|| Section.MacroRatio <= 0.0f))
			{
				return WoTRuntimeWorldError(
					TEXT("world mover macro identity is malformed"));
			}
			Section.bCollisionEnabled = Collision != 0;
			Section.bAnimatedLightmap = Animated != 0;
			Slots.Add(Section.MaterialSlot);
			ExpectedFirst += Section.IndexCount;
		}
		if (ExpectedFirst != IndexCount)
			return WoTRuntimeWorldError(TEXT("world mover sections do not cover indices"));
		Mover.Positions.SetNumUninitialized(static_cast<int32>(VertexCount));
		Mover.Normals.SetNumUninitialized(static_cast<int32>(VertexCount));
		Mover.UV0.SetNumUninitialized(static_cast<int32>(VertexCount));
		for (FVector3f& Position : Mover.Positions)
			if (!Cursor.ReadFloat(Position.X) || !Cursor.ReadFloat(Position.Y)
				|| !Cursor.ReadFloat(Position.Z)
				|| Position.GetAbsMax() > 10000000.0f)
				return WoTRuntimeWorldError(TEXT("world mover position stream is malformed"));
		for (FVector3f& Normal : Mover.Normals)
			if (!Cursor.ReadFloat(Normal.X) || !Cursor.ReadFloat(Normal.Y)
				|| !Cursor.ReadFloat(Normal.Z)
				|| !FMath::IsNearlyEqual(Normal.SquaredLength(), 1.0f, 0.02f))
				return WoTRuntimeWorldError(TEXT("world mover normal stream is malformed"));
		for (FVector2f& UV : Mover.UV0)
			if (!Cursor.ReadFloat(UV.X) || !Cursor.ReadFloat(UV.Y))
				return WoTRuntimeWorldError(TEXT("world mover UV stream is malformed"));
		if (Version >= 4)
		{
			Mover.UV1.SetNumUninitialized(static_cast<int32>(VertexCount));
			for (FVector2f& UV : Mover.UV1)
				if (!Cursor.ReadFloat(UV.X) || !Cursor.ReadFloat(UV.Y)
					|| UV.X < 0.0f || UV.X > 1.0f
					|| UV.Y < 0.0f || UV.Y > 1.0f)
					return WoTRuntimeWorldError(
						TEXT("world mover UV1 stream is malformed"));
		}
		Mover.Indices.SetNumUninitialized(static_cast<int32>(IndexCount));
		for (uint32& Vertex : Mover.Indices)
			if (!Cursor.ReadU32(Vertex) || Vertex >= VertexCount)
				return WoTRuntimeWorldError(TEXT("world mover index stream is malformed"));
		if (Version >= 4)
		{
			Mover.CollisionPositions.SetNumUninitialized(
				static_cast<int32>(CollisionVertexCount));
			for (FVector3f& Position : Mover.CollisionPositions)
			{
				if (!Cursor.ReadFloat(Position.X) || !Cursor.ReadFloat(Position.Y)
					|| !Cursor.ReadFloat(Position.Z)
					|| !FMath::IsFinite(Position.X) || !FMath::IsFinite(Position.Y)
					|| !FMath::IsFinite(Position.Z)
					|| Position.GetAbsMax() > 10000000.0f)
				{
					return WoTRuntimeWorldError(
						TEXT("world mover collision position stream is malformed"));
				}
			}
			Mover.CollisionIndices.SetNumUninitialized(
				static_cast<int32>(CollisionIndexCount));
			for (uint32& Vertex : Mover.CollisionIndices)
			{
				if (!Cursor.ReadU32(Vertex) || Vertex >= CollisionVertexCount)
				{
					return WoTRuntimeWorldError(
						TEXT("world mover collision index stream is malformed"));
				}
			}
		}
		for (const FWoTRuntimeMoverSection& Section : Mover.Sections)
		{
			uint32 Minimum = MAX_uint32, Maximum = 0;
			for (uint32 At = Section.FirstIndex;
				At < Section.FirstIndex + Section.IndexCount; ++At)
			{
				const uint32 Vertex = Mover.Indices[At];
				Minimum = FMath::Min(Minimum, Vertex);
				Maximum = FMath::Max(Maximum, Vertex);
				if (Version < 4)
				{
					const float PlaneDistance = FVector3f::DotProduct(
						Mover.Positions[Vertex] - Section.PlaneOrigin,
						Section.PlaneNormal);
					if (FMath::Abs(PlaneDistance) > 0.05f)
						return WoTRuntimeWorldError(
							TEXT("world mover vertex is outside its section plane"));
				}
			}
			if (Minimum != Section.MinVertex || Maximum != Section.MaxVertex)
				return WoTRuntimeWorldError(
					TEXT("world mover section min/max disagrees with indices"));
		}
	}
	uint32 MoverAtlasWidth = 0, MoverAtlasHeight = 0;
	TArray<uint8> MoverAtlasBGRA;
	TArray<FWoTRuntimeMoverAnimationGroup> MoverAnimationGroups;
	TArray<FWoTRuntimeMoverAnimationTerm> MoverAnimationTerms;
	if (Version >= 4)
	{
		uint32 MoverAtlasBytes = 0;
		if (!Cursor.ReadU32(MoverAtlasWidth)
			|| !Cursor.ReadU32(MoverAtlasHeight)
			|| !Cursor.ReadU32(MoverAtlasBytes)
			|| MoverAtlasWidth < 1 || MoverAtlasWidth > 4096
			|| MoverAtlasHeight < 1 || MoverAtlasHeight > 4096
			|| static_cast<uint64>(MoverAtlasWidth) * MoverAtlasHeight * 4
				!= MoverAtlasBytes
			|| !Cursor.ReadBytes(MoverAtlasBytes, MoverAtlasBGRA))
		{
			return WoTRuntimeWorldError(
				TEXT("world mover light-map atlas is malformed"));
		}
	}
	if (Version >= 6)
	{
		uint32 GroupCount = 0;
		if (!Cursor.ReadU32(GroupCount) || GroupCount > 4)
			return WoTRuntimeWorldError(
				TEXT("world mover animation group table is malformed"));
		MoverAnimationGroups.Reserve(static_cast<int32>(GroupCount));
		for (uint32 GroupIndex = 0; GroupIndex < GroupCount; ++GroupIndex)
		{
			FWoTRuntimeMoverAnimationGroup& Group =
				MoverAnimationGroups.AddDefaulted_GetRef();
			uint8 Reserved = 0;
			if (!Cursor.ReadU8(Group.Type) || !Cursor.ReadU8(Group.Period)
				|| !Cursor.ReadU8(Group.Phase) || !Cursor.ReadU8(Reserved)
				|| !Cursor.ReadU32(Group.LightCount)
				|| (Group.Type != 2 && Group.Type != 3 && Group.Type != 7)
				|| Reserved != 0 || Group.LightCount == 0)
			{
				return WoTRuntimeWorldError(
					TEXT("world mover animation group is malformed"));
			}
		}
		uint32 TermCount = 0;
		if (!Cursor.ReadU32(TermCount) || TermCount > 6)
			return WoTRuntimeWorldError(
				TEXT("world mover animation term table is malformed"));
		MoverAnimationTerms.Reserve(static_cast<int32>(TermCount));
		uint64 ResponseBytes = 0;
		const uint64 AtlasPixels =
			static_cast<uint64>(MoverAtlasWidth) * MoverAtlasHeight;
		for (uint32 TermIndex = 0; TermIndex < TermCount; ++TermIndex)
		{
			FWoTRuntimeMoverAnimationTerm& Term =
				MoverAnimationTerms.AddDefaulted_GetRef();
			uint8 Group = 255, Reserved = 0;
			uint32 RectCount = 0;
			if (!Cursor.ReadU8(Term.Kind) || !Cursor.ReadU8(Group)
				|| !Cursor.ReadU8(Term.Effect) || !Cursor.ReadU8(Reserved)
				|| !Cursor.ReadU32(Term.LightCount) || !Cursor.ReadU32(RectCount)
				|| Term.Kind > 2 || Reserved != 0 || Term.LightCount == 0
				|| RectCount > 1000000)
			{
				return WoTRuntimeWorldError(
					TEXT("world mover animation term is malformed"));
			}
			Term.Group = Group == 255 ? INDEX_NONE : static_cast<int32>(Group);
			const bool bGroupKind = Term.Kind == 0 || Term.Kind == 2;
			const bool bEffectKind = Term.Kind == 1 || Term.Kind == 2;
			if ((bGroupKind && !MoverAnimationGroups.IsValidIndex(Term.Group))
				|| (!bGroupKind && Term.Group != INDEX_NONE)
				|| (bEffectKind && (Term.Effect < 1 || Term.Effect > 3))
				|| (!bEffectKind && Term.Effect != 0))
			{
				return WoTRuntimeWorldError(
					TEXT("world mover animation term routing is malformed"));
			}
			Term.ResponseBGRA.Init(0, static_cast<int32>(AtlasPixels * 4));
			for (uint64 Pixel = 0; Pixel < AtlasPixels; ++Pixel)
				Term.ResponseBGRA[static_cast<int32>(Pixel * 4 + 3)] = 255;
			for (uint32 RectIndex = 0; RectIndex < RectCount; ++RectIndex)
			{
				uint32 X = 0, Y = 0, W = 0, H = 0, ByteCount = 0;
				TArray<uint8> RGB;
				if (!Cursor.ReadU32(X) || !Cursor.ReadU32(Y)
					|| !Cursor.ReadU32(W) || !Cursor.ReadU32(H)
					|| !Cursor.ReadU32(ByteCount)
					|| !WoTRuntimeWorldResponseRectFits(
						X, Y, W, H, MoverAtlasWidth, MoverAtlasHeight)
					|| static_cast<uint64>(W) * H * 3 != ByteCount
					|| !Cursor.ReadBytes(ByteCount, RGB))
				{
					return WoTRuntimeWorldError(
						TEXT("world mover animation response rect is malformed"));
				}
				ResponseBytes += ByteCount;
				if (ResponseBytes > 64ull * 1024ull * 1024ull)
					return WoTRuntimeWorldError(
						TEXT("world mover animation responses exceed their bound"));
				for (int32 PaddedV = -1; PaddedV <= static_cast<int32>(H); ++PaddedV)
				{
					const uint32 SourceV = static_cast<uint32>(FMath::Clamp(
						PaddedV, 0, static_cast<int32>(H) - 1));
					for (int32 PaddedU = -1;
						PaddedU <= static_cast<int32>(W); ++PaddedU)
					{
						const uint32 SourceU = static_cast<uint32>(FMath::Clamp(
							PaddedU, 0, static_cast<int32>(W) - 1));
						const int32 SourceOffset =
							static_cast<int32>((SourceV * W + SourceU) * 3);
						const int32 DestinationOffset = static_cast<int32>((
							(static_cast<uint64>(Y + PaddedV) * MoverAtlasWidth)
								+ X + PaddedU) * 4);
						Term.ResponseBGRA[DestinationOffset] = RGB[SourceOffset + 2];
						Term.ResponseBGRA[DestinationOffset + 1] = RGB[SourceOffset + 1];
						Term.ResponseBGRA[DestinationOffset + 2] = RGB[SourceOffset];
					}
				}
			}
		}
	}
	if (!Cursor.AtEnd())
	{
		return WoTRuntimeWorldError(
			TEXT("world payload has trailing structural bytes"));
	}
	ParsedMapIdentity = MoveTemp(MapIdentity);
	ParsedFactId = MoveTemp(FactId);
	ParsedImporter = MoveTemp(Importer);
	ParsedImporterVersion = ImporterVersion;
	ParsedFormatVersion = Version;
	ParsedSourceExport = MoveTemp(SourceExport);
	ParsedSourcePackage = MoveTemp(SourcePackage);
	ParsedSourceSha256 = MoveTemp(SourceHash);
	ParsedActorCount = ActorCount;
	ParsedAttachmentCount = AttachmentCount;
	ParsedBSPNodeCount = NodeCount;
	ParsedBSPSurfaceCount = SurfaceCount;
	ParsedNavigationSeedCount = NavigationSeedCount;
	ParsedSemanticObjectCount = SemanticCount;
	ParsedReachSpecCount = ReachCount;
	ParsedZoneCount = ZoneCount;
	ParsedPortalSurfaceCount = PortalSurfaceCount;
	ParsedMoverCount = MoverCount;
	ParsedLightCount = LightCount;
	ParsedSoundCount = SoundCount;
	ParsedVectors = MoveTemp(Vectors);
	ParsedPoints = MoveTemp(Points);
	ParsedNodes = MoveTemp(Nodes);
	ParsedSurfaces = MoveTemp(Surfaces);
	ParsedVertices = MoveTemp(Vertices);
	ParsedZones = MoveTemp(Zones);
	ParsedLeaves = MoveTemp(Leaves);
	ParsedModelLights = MoveTemp(ModelLights);
	bParsedRootOutside = RootOutside != 0;
	ParsedMoverModels = MoveTemp(MoverModels);
	ParsedMoverAtlasWidth = MoverAtlasWidth;
	ParsedMoverAtlasHeight = MoverAtlasHeight;
	ParsedMoverAtlasBGRA = MoveTemp(MoverAtlasBGRA);
	ParsedMoverAnimationGroups = MoveTemp(MoverAnimationGroups);
	ParsedMoverAnimationTerms = MoveTemp(MoverAnimationTerms);
	ParsedSemanticObjects = MoveTemp(SemanticObjects);
	ParsedActors = MoveTemp(Actors);
	ParsedReachSpecs = MoveTemp(ReachSpecs);
	Object.Emplace(MoveTemp(Verified));
	return FWoTBootstrapStatus::Success();
}
