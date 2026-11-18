#include "WoTRetailReader/BSPWorld.h"

#include "Algo/Reverse.h"
#include "Algo/Sort.h"
#include "Async/ParallelFor.h"
#include "CanonicalObjectBuilder.h"
#include "TypedProperties.h"
#include "Containers/StringConv.h"
#include "Misc/Paths.h"
#include "WoTBootstrapCore/CanonicalJson.h"
#include "WoTBootstrapCore/Hash.h"
#include "WoTRetailReader/Generated/RequiredWorlds.generated.h"
#include "WoTRetailReader/Generated/WorldProperties.generated.h"
#include "WoTRetailReader/Generated/WorldTextures.generated.h"

namespace
{
constexpr uint64 WoTWorldMaxSourceBytes = 256ull * 1024 * 1024;
constexpr uint64 WoTWorldMaxTotalSourceBytes = 1024ull * 1024 * 1024;
constexpr uint32 WoTWorldMaxTableItems = 1000000;
constexpr uint32 WoTWorldMaxNameBytes = 1024;
constexpr uint32 WoTWorldMaxOuterDepth = 32;
constexpr uint32 WoTWorldMaxActors = 1000000;
constexpr uint32 WoTWorldMaxReferences = 4000000;
constexpr uint32 WoTWorldMaxProperties = 4000000;
constexpr uint32 WoTWorldMaxReachSpecs = 10000000;
constexpr uint32 WoTWorldMaxTravelInfoItems = 65536;
constexpr uint32 WoTWorldMaxBSPVertices = 2000000;
constexpr uint32 WoTWorldMaxBSPIndices = 6000000;
constexpr uint32 WoTWorldMaxBSPSections = 4096;
constexpr uint32 WoTWorldMaxSlotNameBytes = 1024;
constexpr int32 WoTWorldMaxSectionIdentityBytes = 1024;
constexpr uint32 WoTWorldMaxEntryPairs = 4096;
constexpr uint64 WoTWorldMaxEntryPairBytes = 4ull * 1024 * 1024;
constexpr float WoTWorldMaxCoordinateAbs = 10000000.0f;
constexpr uint32 WoTWorldPackageTag = 0x9e2a83c1;
constexpr uint32 WoTWorldHasStack = 0x02000000;
constexpr uint32 WoTWorldPFInvisible = 0x00000001;
constexpr uint32 WoTWorldPFNotSolid = 0x00000008;
constexpr uint32 WoTWorldPFMasked = 0x00000002;
constexpr uint32 WoTWorldPFTranslucent = 0x00000004;
constexpr uint32 WoTWorldPFModulated = 0x00000040;
constexpr uint32 WoTWorldPFFakeBackdrop = 0x00000080;
constexpr uint32 WoTWorldPFTwoSided = 0x00000100;
constexpr uint32 WoTWorldPFAutoUPan = 0x00000200;
constexpr uint32 WoTWorldPFAutoVPan = 0x00000400;
constexpr uint32 WoTWorldPFSpecialLit = 0x00100000;
constexpr uint32 WoTWorldPFUnlit = 0x00400000;
constexpr uint32 WoTWorldPFPortal = 0x04000000;
constexpr ANSICHAR WoTWorldObjectMagic[] = "WOTOBJ3";
constexpr ANSICHAR WoTWorldBSPMagic[] = "WOTPBSP";
constexpr ANSICHAR WoTWorldPayloadMagic[] = "WOTWRL3";
constexpr ANSICHAR WoTWorldSetDomain[] = "WOT-WORLD-SET-V3";
constexpr ANSICHAR WoTWorldMapDomain[] = "WOT-WORLD-MAP-V3";
constexpr ANSICHAR WoTWorldMapSourceDomain[] = "WOT-WORLD-MAP-SOURCE-V3";
constexpr TCHAR WoTWorldBSPEncoding[] = TEXT("wot-bsp-static-mesh-v1");
constexpr TCHAR WoTWorldBSPTangentAlgorithm[] =
	TEXT("ue-mikk-preserve-normals-v1");
constexpr uint32 WoTWorldBSPImporterVersion = 3;
constexpr uint32 WoTWorldEntryImporterVersion = 4;
constexpr uint32 WoTWorldImporterVersion = 6;
constexpr uint64 WoTWorldMaxMoverAtlasPixels = 4096ull * 4096ull;

// UE1's fixed per-level zone table size: UModel::Zones[FBspNode::MAX_ZONES]
// with MAX_ZONES = 64. NumZones counts only the
// records actually serialized; node iZone values still address all 64.
constexpr int32 WoTWorldMaxZones = 64;

FWoTBootstrapError WoTWorldError(EWoTBootstrapError Code,
	const TCHAR* Message, FString Path = FString())
{
	return MakeWoTBootstrapError(Code, Message, MoveTemp(Path));
}

class FWoTWorldReader
{
public:
	FWoTWorldReader() = default;
	explicit FWoTWorldReader(TConstArrayView<uint8> InBytes)
		: Bytes(InBytes), End(static_cast<uint64>(InBytes.Num())) {}
	FWoTWorldReader(TConstArrayView<uint8> InBytes, uint64 InOffset, uint64 InEnd)
		: Bytes(InBytes), Offset(InOffset), End(InEnd)
	{
		if (InOffset > InEnd || InEnd > static_cast<uint64>(InBytes.Num())) bValid = false;
	}

	bool ReadU8(uint8& Out)
	{
		if (!Require(1)) return false;
		Out = Bytes[static_cast<int32>(Offset++)];
		return true;
	}
	bool ReadU16(uint16& Out)
	{
		if (!Require(2)) return false;
		const uint8* Data = Bytes.GetData() + Offset;
		Out = static_cast<uint16>(Data[0])
			| static_cast<uint16>(static_cast<uint16>(Data[1]) << 8);
		Offset += 2;
		return true;
	}
	bool ReadI16(int16& Out)
	{
		uint16 Value = 0;
		if (!ReadU16(Value)) return false;
		Out = static_cast<int16>(Value);
		return true;
	}
	bool ReadU32(uint32& Out)
	{
		if (!Require(4)) return false;
		const uint8* Data = Bytes.GetData() + Offset;
		Out = static_cast<uint32>(Data[0])
			| (static_cast<uint32>(Data[1]) << 8)
			| (static_cast<uint32>(Data[2]) << 16)
			| (static_cast<uint32>(Data[3]) << 24);
		Offset += 4;
		return true;
	}
	bool ReadI32(int32& Out)
	{
		uint32 Value = 0;
		if (!ReadU32(Value)) return false;
		Out = static_cast<int32>(Value);
		return true;
	}
	bool ReadU64(uint64& Out)
	{
		uint32 Low = 0, High = 0;
		if (!ReadU32(Low) || !ReadU32(High)) return false;
		Out = static_cast<uint64>(Low) | (static_cast<uint64>(High) << 32);
		return true;
	}
	bool ReadFloat(float& Out)
	{
		uint32 Bits = 0;
		if (!ReadU32(Bits)) return false;
		FMemory::Memcpy(&Out, &Bits, sizeof(Out));
		return FMath::IsFinite(Out);
	}
	bool ReadVector(FVector3f& Out)
	{
		return ReadFloat(Out.X) && ReadFloat(Out.Y) && ReadFloat(Out.Z);
	}
	bool ReadCompact(int32& Out)
	{
		uint8 First = 0;
		if (!ReadU8(First)) return false;
		const bool bNegative = (First & 0x80) != 0;
		uint64 Magnitude = First & 0x3f;
		bool bMore = (First & 0x40) != 0;
		uint32 Shift = 6;
		for (uint32 Index = 0; bMore && Index < 4; ++Index)
		{
			uint8 Next = 0;
			if (!ReadU8(Next)) return false;
			Magnitude |= static_cast<uint64>(Next & 0x7f) << Shift;
			bMore = (Next & 0x80) != 0;
			Shift += 7;
		}
		if (bMore || Magnitude > static_cast<uint64>(MAX_int32)
			|| (bNegative && Magnitude == 0))
		{
			bValid = false;
			return false;
		}
		Out = bNegative ? -static_cast<int32>(Magnitude) : static_cast<int32>(Magnitude);
		return true;
	}
	bool Skip(uint64 Count)
	{
		if (!Require(Count)) return false;
		Offset += Count;
		return true;
	}
	bool SetOffset(uint64 NewOffset)
	{
		if (NewOffset > End)
		{
			bValid = false;
			return false;
		}
		Offset = NewOffset;
		return true;
	}
	bool ReadBytes(uint64 Count, TArray<uint8>& Out)
	{
		if (!Require(Count) || Count > static_cast<uint64>(MAX_int32)) return false;
		Out.Reset(static_cast<int32>(Count));
		Out.Append(Bytes.GetData() + Offset, static_cast<int32>(Count));
		Offset += Count;
		return true;
	}
	bool Require(uint64 Count)
	{
		if (!bValid || Offset > End || Count > End - Offset)
		{
			bValid = false;
			return false;
		}
		return true;
	}
	uint64 Tell() const { return Offset; }
	uint64 GetEnd() const { return End; }
	bool IsValid() const { return bValid; }

private:
	TConstArrayView<uint8> Bytes;
	uint64 Offset = 0;
	uint64 End = 0;
	bool bValid = true;
};

struct FWoTWorldImport
{
	FString ClassPackage;
	FString ClassName;
	int32 Outer = 0;
	FString Name;
};

struct FWoTWorldExport
{
	int32 ClassReference = 0;
	int32 SuperReference = 0;
	int32 Outer = 0;
	FString Name;
	uint32 Flags = 0;
	uint64 Offset = 0;
	uint64 Size = 0;
};

struct FWoTWorldActor
{
	int32 ExportReference = 0;
	FString Identity;
	FString ClassIdentity;
	TArray<FWoTTypedProperty> Properties;
	TOptional<int32> ParentActor;
};

struct FWoTWorldNode
{
	float Plane[4] = {};
	uint64 ZoneMask = 0;
	uint8 Flags = 0;
	int32 VertexPool = 0;
	int32 Surface = 0;
	int32 Back = -1;
	int32 Front = -1;
	int32 PlaneNode = -1;
	int32 CollisionBound = -1;
	int32 RenderBound = -1;
	uint8 Zones[2] = {};
	uint8 VertexCount = 0;
	int32 Leaves[2] = {};
};

struct FWoTWorldSurface
{
	int32 Texture = 0;
	uint32 Flags = 0;
	int32 Base = 0;
	int32 Normal = 0;
	int32 TextureU = 0;
	int32 TextureV = 0;
	int32 LightMap = -1;
	int32 BrushPoly = -1;
	int16 PanU = 0;
	int16 PanV = 0;
	int32 Actor = 0;
};

struct FWoTWorldVert
{
	int32 Point = 0;
	int32 Side = 0;
};

struct FWoTWorldZone
{
	int32 Actor = 0;
	uint64 Connectivity = 0;
	uint64 Visibility = 0;
};

struct FWoTWorldLeaf
{
	int32 Zone = 0;
	int32 Permeating = 0;
	int32 Volumetric = 0;
};

struct FWoTWorldLightMap
{
	int32 DataOffset = 0;
	FVector3f Pan = FVector3f::ZeroVector;
	int32 UClamp = 0;
	int32 VClamp = 0;
	float UScale = 0.0f;
	float VScale = 0.0f;
	int32 LightActors = 0;
};

struct FWoTWorldModel
{
	TArray<FVector3f> Vectors;
	TArray<FVector3f> Points;
	TArray<FWoTWorldNode> Nodes;
	TArray<FWoTWorldSurface> Surfaces;
	TArray<FWoTWorldVert> Vertices;
	TArray<FWoTWorldZone> Zones;
	TArray<FWoTWorldLeaf> Leaves;
	TArray<FWoTWorldLightMap> LightMaps;
	TArray<uint8> LightBits;
	TArray<int32> Lights;
	int32 PolysReference = 0;
	bool bRootOutside = false;
};

struct FWoTWorldPoly
{
	FVector3f Base = FVector3f::ZeroVector;
	FVector3f Normal = FVector3f::ZAxisVector;
	FVector3f TextureU = FVector3f::ZeroVector;
	FVector3f TextureV = FVector3f::ZeroVector;
	TArray<FVector3f> Vertices;
	uint32 Flags = 0;
	int32 Actor = 0;
	int32 Texture = 0;
	int32 Link = INDEX_NONE;
	int32 BrushPoly = INDEX_NONE;
	int16 PanU = 0;
	int16 PanV = 0;
};

struct FWoTWorldSemanticObject
{
	int32 PackageReference = 0;
	FString Identity;
	FString ClassIdentity;
	bool bActor = false;
};

struct FWoTWorldReachSpec
{
	int32 Distance = 0;
	int32 StartReference = 0;
	int32 EndReference = 0;
	int32 CollisionRadius = 0;
	int32 CollisionHeight = 0;
	uint32 ReachFlags = 0;
	bool bPruned = false;
	FString StartIdentity;
	FString EndIdentity;
};

struct FWoTWorldSource
{
	FString SourcePath;
	FString MapIdentity;
	TArray<uint8> Bytes;
	uint16 Version = 0;
	TArray<FString> Names;
	TArray<FWoTWorldImport> Imports;
	TArray<FWoTWorldExport> Exports;
	FWoTSha256 Sha256;
	int32 LevelReference = 0;
	int32 ModelReference = 0;
	TArray<FWoTWorldActor> Actors;
	TArray<FWoTWorldSemanticObject> SemanticObjects;
	TMap<int32, int32> SemanticObjectByReference;
	TMap<FString, int32> SemanticObjectByIdentity;
	TArray<FWoTWorldReachSpec> ReachSpecs;
	uint32 AttachmentCount = 0;
	uint32 MoverCount = 0;
	uint32 LightCount = 0;
	uint32 SoundCount = 0;
	uint32 NavigationSeedCount = 0;
	uint32 StaticLightmappedMoverCount = 0;
	uint32 MoverLitPolyCount = 0;
	uint32 MoverRenderTriangleCount = 0;
	uint32 MoverCollisionTriangleCount = 0;
	uint32 MoverUV1OutsidePaddedRectCount = 0;
	uint32 MoverPolyOwnershipViolationCount = 0;
	uint32 MoverAnimatedLightPolyCount = 0;
	uint32 MoverAtlasWidth = 0;
	uint32 MoverAtlasHeight = 0;
	TArray<uint8> MoverOracleRGB7;

	const FString* NameAt(int32 Index) const
	{
		return Names.IsValidIndex(Index) ? &Names[Index] : nullptr;
	}
	const FString* ClassName(int32 Reference) const
	{
		if (Reference < 0)
		{
			const int64 Index = -static_cast<int64>(Reference) - 1;
			return Imports.IsValidIndex(static_cast<int32>(Index))
				? &Imports[static_cast<int32>(Index)].Name : nullptr;
		}
		if (Reference > 0 && Exports.IsValidIndex(Reference - 1))
		{
			return &Exports[Reference - 1].Name;
		}
		return nullptr;
	}
};

struct FWoTWorldContractMap
{
	FString Identity;
	uint32 Slot = 0;
	const FWoTRequiredFact* BSPFact = nullptr;
	const FWoTRequiredFact* WorldFact = nullptr;
};

bool WoTWorldLatin1(TConstArrayView<uint8> Raw, FString& Out)
{
	Out.Reset(Raw.Num());
	for (uint8 Byte : Raw)
	{
		if (Byte == 0) return false;
		Out.AppendChar(static_cast<TCHAR>(Byte));
	}
	return true;
}

FString WoTWorldFold(FStringView Value)
{
	FString Result(Value);
	for (int32 Index = 0; Index < Result.Len(); ++Index)
	{
		if (Result[Index] >= TEXT('A') && Result[Index] <= TEXT('Z'))
		{
			Result[Index] += TEXT('a') - TEXT('A');
		}
	}
	return Result;
}

bool WoTWorldReferenceValid(const FWoTWorldSource& Source, int32 Reference)
{
	if (Reference == 0) return true;
	if (Reference > 0) return Source.Exports.IsValidIndex(Reference - 1);
	const int64 Index = -static_cast<int64>(Reference) - 1;
	return Index >= 0 && Index < Source.Imports.Num();
}

TWoTBootstrapResult<FString> WoTWorldObjectPath(
	const FWoTWorldSource& Source, int32 Reference)
{
	using FResult = TWoTBootstrapResult<FString>;
	if (Reference == 0) return FResult::Success(FString());
	TArray<FString> Reverse;
	TSet<int32> Seen;
	int32 Current = Reference;
	for (uint32 Depth = 0; Depth < WoTWorldMaxOuterDepth; ++Depth)
	{
		if (Current == 0) break;
		if (Seen.Contains(Current) || !WoTWorldReferenceValid(Source, Current))
		{
			return FResult::Error(WoTWorldError(EWoTBootstrapError::CorruptInstallation,
				TEXT("a world object outer chain is cyclic or invalid"), Source.SourcePath));
		}
		Seen.Add(Current);
		if (Current > 0)
		{
			const FWoTWorldExport& Export = Source.Exports[Current - 1];
			Reverse.Add(Export.Name);
			Current = Export.Outer;
			if (Current == 0) Reverse.Add(Source.MapIdentity);
		}
		else
		{
			const FWoTWorldImport& Import = Source.Imports[-Current - 1];
			Reverse.Add(Import.Name);
			Current = Import.Outer;
		}
	}
	if (Current != 0)
	{
		return FResult::Error(WoTWorldError(EWoTBootstrapError::SizeLimit,
			TEXT("a world object outer chain exceeds its depth bound"), Source.SourcePath));
	}
	FString Result;
	for (int32 Index = Reverse.Num() - 1; Index >= 0; --Index)
	{
		if (!Result.IsEmpty()) Result += TEXT(".");
		Result += Reverse[Index];
	}
	return FResult::Success(MoveTemp(Result));
}

TWoTBootstrapResult<FString> WoTWorldObjectClassIdentity(
	const FWoTWorldSource& Source, int32 Reference)
{
	using FResult = TWoTBootstrapResult<FString>;
	if (Reference == 0) return FResult::Success(FString());
	if (!WoTWorldReferenceValid(Source, Reference))
	{
		return FResult::Error(WoTWorldError(EWoTBootstrapError::CorruptInstallation,
			TEXT("a semantic object has an invalid package reference"),
			Source.SourcePath));
	}
	if (Reference < 0)
	{
		const FWoTWorldImport& Import = Source.Imports[-Reference - 1];
		return FResult::Success(Import.ClassPackage + TEXT(".") + Import.ClassName);
	}
	const int32 ClassReference = Source.Exports[Reference - 1].ClassReference;
	if (ClassReference == 0)
	{
		return FResult::Success(TEXT("Core.Class"));
	}
	return WoTWorldObjectPath(Source, ClassReference);
}

TWoTBootstrapResult<int32> WoTWorldBindSemanticObject(
	FWoTWorldSource& Source, int32 Reference, bool bActor)
{
	using FResult = TWoTBootstrapResult<int32>;
	if (Reference == 0) return FResult::Success(INDEX_NONE);
	if (const int32* Existing = Source.SemanticObjectByReference.Find(Reference))
	{
		if (bActor) Source.SemanticObjects[*Existing].bActor = true;
		return FResult::Success(*Existing);
	}
	TWoTBootstrapResult<FString> Identity = WoTWorldObjectPath(Source, Reference);
	TWoTBootstrapResult<FString> ClassIdentity =
		WoTWorldObjectClassIdentity(Source, Reference);
	if (Identity.IsError()) return FResult::Error(MoveTemp(Identity).GetError());
	if (ClassIdentity.IsError())
		return FResult::Error(MoveTemp(ClassIdentity).GetError());
	if (Identity.GetValue().IsEmpty() || ClassIdentity.GetValue().IsEmpty())
	{
		return FResult::Error(WoTWorldError(EWoTBootstrapError::CorruptInstallation,
			TEXT("a referenced world object lacks a stable typed identity"),
			Source.SourcePath));
	}
	const FString Folded = WoTWorldFold(Identity.GetValue());
	if (const int32* Existing = Source.SemanticObjectByIdentity.Find(Folded))
	{
		FWoTWorldSemanticObject& Prior = Source.SemanticObjects[*Existing];
		if (!Prior.ClassIdentity.Equals(
			ClassIdentity.GetValue(), ESearchCase::IgnoreCase))
		{
			return FResult::Error(WoTWorldError(EWoTBootstrapError::DuplicateField,
				TEXT("one semantic object identity resolves to conflicting classes"),
				Identity.GetValue()));
		}
		Prior.bActor |= bActor;
		Source.SemanticObjectByReference.Add(Reference, *Existing);
		return FResult::Success(*Existing);
	}
	FWoTWorldSemanticObject& Object = Source.SemanticObjects.AddDefaulted_GetRef();
	Object.PackageReference = Reference;
	Object.Identity = MoveTemp(Identity).GetValue();
	Object.ClassIdentity = MoveTemp(ClassIdentity).GetValue();
	Object.bActor = bActor;
	const int32 Index = Source.SemanticObjects.Num() - 1;
	Source.SemanticObjectByReference.Add(Reference, Index);
	Source.SemanticObjectByIdentity.Add(Folded, Index);
	return FResult::Success(Index);
}

bool WoTWorldOpenExport(const FWoTWorldSource& Source,
	const FWoTWorldExport& Export, FWoTWorldReader& Out)
{
	Out = FWoTWorldReader(Source.Bytes, Export.Offset, Export.Offset + Export.Size);
	if ((Export.Flags & WoTWorldHasStack) != 0)
	{
		int32 Node = 0, StateNode = 0;
		if (!Out.ReadCompact(Node) || !Out.ReadCompact(StateNode) || !Out.Skip(12)
			|| (Node != 0 && !Out.ReadCompact(StateNode))) return false;
	}
	return Out.IsValid();
}

bool WoTWorldReadName(FWoTWorldReader& Reader,
	const FWoTWorldSource& Source, FString& Out)
{
	int32 Index = 0;
	if (!Reader.ReadCompact(Index) || Source.NameAt(Index) == nullptr) return false;
	Out = *Source.NameAt(Index);
	return true;
}

bool WoTWorldReadProperties(FWoTWorldReader& Reader,
	const FWoTWorldSource& Source, TArray<FWoTTypedProperty>* OutProperties,
	uint64& InOutPropertyCount)
{
	TArray<FWoTTypedProperty> Ignored;
	FWoTTypedPropertyReadOptions Options;
	Options.MaximumProperties = WoTWorldMaxProperties;
	Options.TotalPropertyCount = &InOutPropertyCount;
	// The runtime world reader REQUIRES canonical property order (folded name,
	// then array index, then name) and rejects the whole world otherwise. This
	// path emitted retail source order, so the guarded client refused every
	// materialized world with "world actor table is malformed" -- the first
	// offender being Arena_01.LevelInfo0's 'Author' following 'Title'. Nothing
	// caught it because the option defaults off, the only other caller
	// (FontProcedural) sets it, and every world test builds synthetic blobs
	// that are already ordered.
	Options.bSortCanonical = true;
	// All nineteen shipped gameplay worlds were audited against the UE1 tags:
	// their 353,125 actor properties use only the runtime-supported types
	// (1,2,3,4,5,6,10,13). Refuse any other type at the source boundary instead
	// of carrying opaque bytes to a runtime consumer which cannot interpret them.
	Options.bRejectUnsupportedTypes = true;
	return WoTReadTypedProperties(Reader,
		[&Source](FWoTWorldReader& From, FString& Out)
		{
			return WoTWorldReadName(From, Source, Out);
		},
		[&Source](int32 Reference)
		{
			return WoTWorldReferenceValid(Source, Reference);
		},
		OutProperties != nullptr ? *OutProperties : Ignored, Options);
}

const WoT::Retail::Generated::FWorldProperty* WoTWorldPropertyReflection(
	FStringView ActorClass, FStringView PropertyName)
{
	const FString ActorFolded = WoTWorldFold(ActorClass);
	const FString PropertyFolded = WoTWorldFold(PropertyName);
	int32 Low = 0;
	int32 High = UE_ARRAY_COUNT(WoT::Retail::Generated::WorldProperties);
	while (Low < High)
	{
		const int32 Mid = Low + (High - Low) / 2;
		const WoT::Retail::Generated::FWorldProperty& Candidate =
			WoT::Retail::Generated::WorldProperties[Mid];
		int32 Comparison = ActorFolded.Compare(
			Candidate.ActorClassFolded, ESearchCase::CaseSensitive);
		if (Comparison == 0)
		{
			Comparison = PropertyFolded.Compare(
				Candidate.PropertyNameFolded, ESearchCase::CaseSensitive);
		}
		if (Comparison < 0) High = Mid;
		else if (Comparison > 0) Low = Mid + 1;
		else return &Candidate;
	}
	return nullptr;
}

FWoTBootstrapStatus WoTWorldValidateTypedProperties(
	const FWoTWorldActor& Actor)
{
	TSet<FString> IndexedFields;
	for (const FWoTTypedProperty& Property : Actor.Properties)
	{
		const WoT::Retail::Generated::FWorldProperty* Reflected =
			WoTWorldPropertyReflection(Actor.ClassIdentity, Property.Name);
		if (Reflected == nullptr || Reflected->TagType != Property.Type
			|| Property.ArrayIndex >= Reflected->ArrayDim
			|| !Property.StructName.Equals(
				Reflected->StructName, ESearchCase::IgnoreCase))
		{
			return FWoTBootstrapStatus::Error(WoTWorldError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("an actor property disagrees with canonical UE1 reflection"),
				Actor.Identity + TEXT(".") + Property.Name));
		}
		const FString IndexedKey = WoTWorldFold(Property.Name)
			+ FString::Printf(TEXT("[%u]"), Property.ArrayIndex);
		if (IndexedFields.Contains(IndexedKey))
		{
			return FWoTBootstrapStatus::Error(WoTWorldError(
				EWoTBootstrapError::DuplicateField,
				TEXT("an indexed actor property occurs more than once"),
				Actor.Identity + TEXT(".") + Property.Name));
		}
		IndexedFields.Add(IndexedKey);
		if (Property.Type == 5 && Property.ObjectReference.IsSet()
			&& Property.ObjectReference.GetValue() != 0)
		{
			if (Property.ObjectIdentity.IsEmpty()
				|| Property.ObjectClassIdentity.IsEmpty())
			{
				return FWoTBootstrapStatus::Error(WoTWorldError(
					EWoTBootstrapError::CorruptInstallation,
					TEXT("an object property lacks a stable semantic binding"),
					Actor.Identity + TEXT(".") + Property.Name));
			}
			TArray<FString> AllowedClasses;
			FString(Reflected->AllowedObjectClasses).ParseIntoArray(
				AllowedClasses, TEXT("|"), true);
			const bool bAllowed = AllowedClasses.ContainsByPredicate(
				[&Property](const FString& Allowed)
				{
					return Allowed.Equals(Property.ObjectClassIdentity,
						ESearchCase::IgnoreCase);
				});
			if (!bAllowed)
			{
				return FWoTBootstrapStatus::Error(WoTWorldError(
					EWoTBootstrapError::CorruptInstallation,
					TEXT("an object property resolves to the wrong semantic type"),
					Actor.Identity + TEXT(".") + Property.Name));
			}
		}
		if (Property.Type == 6 && Property.NameValue.IsEmpty())
		{
			return FWoTBootstrapStatus::Error(WoTWorldError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("a NameProperty lacks its resolved FName"),
				Actor.Identity + TEXT(".") + Property.Name));
		}
	}
	return FWoTBootstrapStatus::Success();
}

TWoTBootstrapResult<FWoTWorldSource> WoTWorldParsePackage(
	TArray<uint8>&& Bytes, FStringView SourcePath)
{
	using FResult = TWoTBootstrapResult<FWoTWorldSource>;
	if (Bytes.IsEmpty() || static_cast<uint64>(Bytes.Num()) > WoTWorldMaxSourceBytes)
	{
		return FResult::Error(WoTWorldError(EWoTBootstrapError::SizeLimit,
			TEXT("the world source exceeds its byte bound"), FString(SourcePath)));
	}
	FWoTWorldSource Source;
	Source.SourcePath = FString(SourcePath);
	Source.MapIdentity = FPaths::GetBaseFilename(Source.SourcePath);
	Source.Bytes = MoveTemp(Bytes);
	Source.Sha256 = Sha256(Source.Bytes);
	FWoTWorldReader Reader(Source.Bytes);
	uint32 Tag = 0, PackageFlags = 0;
	uint16 Licensee = 0;
	int32 NameCount = 0, NameOffset = 0, ExportCount = 0, ExportOffset = 0;
	int32 ImportCount = 0, ImportOffset = 0;
	if (!Reader.ReadU32(Tag) || !Reader.ReadU16(Source.Version)
		|| !Reader.ReadU16(Licensee) || !Reader.ReadU32(PackageFlags)
		|| !Reader.ReadI32(NameCount) || !Reader.ReadI32(NameOffset)
		|| !Reader.ReadI32(ExportCount) || !Reader.ReadI32(ExportOffset)
		|| !Reader.ReadI32(ImportCount) || !Reader.ReadI32(ImportOffset)
		|| Tag != WoTWorldPackageTag || Source.Version < 61 || Source.Version > 69
		|| Source.MapIdentity.IsEmpty() || Source.MapIdentity.Contains(TEXT("."))
		|| NameCount <= 0 || ExportCount <= 0 || ImportCount < 0
		|| NameCount > static_cast<int32>(WoTWorldMaxTableItems)
		|| ExportCount > static_cast<int32>(WoTWorldMaxTableItems)
		|| ImportCount > static_cast<int32>(WoTWorldMaxTableItems)
		|| NameOffset < 0 || ExportOffset < 0 || ImportOffset < 0)
	{
		return FResult::Error(WoTWorldError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the world source is not a bounded UE1 package"), Source.SourcePath));
	}
	if (!Reader.SetOffset(static_cast<uint64>(NameOffset)))
	{
		return FResult::Error(WoTWorldError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the world name table is outside the package"), Source.SourcePath));
	}
	for (int32 Index = 0; Index < NameCount; ++Index)
	{
		TArray<uint8> Raw;
		if (Source.Version >= 64)
		{
			int32 Length = 0;
			if (!Reader.ReadCompact(Length) || Length <= 0
				|| Length > static_cast<int32>(WoTWorldMaxNameBytes)
				|| !Reader.ReadBytes(static_cast<uint64>(Length), Raw) || Raw.Last() != 0)
			{
				return FResult::Error(WoTWorldError(EWoTBootstrapError::CorruptInstallation,
					TEXT("the world name table has an invalid counted name"), Source.SourcePath));
			}
			Raw.Pop(EAllowShrinking::No);
		}
		else
		{
			bool bTerminated = false;
			for (uint32 ByteIndex = 0; ByteIndex < WoTWorldMaxNameBytes; ++ByteIndex)
			{
				uint8 Byte = 0;
				if (!Reader.ReadU8(Byte)) break;
				if (Byte == 0) { bTerminated = true; break; }
				Raw.Add(Byte);
			}
			if (!bTerminated)
			{
				return FResult::Error(WoTWorldError(EWoTBootstrapError::CorruptInstallation,
					TEXT("the world name table is truncated"), Source.SourcePath));
			}
		}
		uint32 Flags = 0;
		FString Name;
		if (!Reader.ReadU32(Flags) || !WoTWorldLatin1(Raw, Name) || Name.IsEmpty()
			|| Name.Contains(TEXT(".")))
		{
			return FResult::Error(WoTWorldError(EWoTBootstrapError::CorruptInstallation,
				TEXT("the world name table contains an unsafe component"), Source.SourcePath));
		}
		Source.Names.Add(MoveTemp(Name));
	}
	if (!Reader.SetOffset(static_cast<uint64>(ImportOffset)))
	{
		return FResult::Error(WoTWorldError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the world import table is outside the package"), Source.SourcePath));
	}
	for (int32 Index = 0; Index < ImportCount; ++Index)
	{
		int32 ClassPackage = 0, ClassName = 0, Outer = 0, ObjectName = 0;
		if (!Reader.ReadCompact(ClassPackage) || !Reader.ReadCompact(ClassName)
			|| !Reader.ReadI32(Outer) || !Reader.ReadCompact(ObjectName)
			|| Source.NameAt(ClassPackage) == nullptr || Source.NameAt(ClassName) == nullptr
			|| Source.NameAt(ObjectName) == nullptr)
		{
			return FResult::Error(WoTWorldError(EWoTBootstrapError::CorruptInstallation,
				TEXT("the world import table is invalid"), Source.SourcePath));
		}
		FWoTWorldImport& Import = Source.Imports.AddDefaulted_GetRef();
		Import.ClassPackage = *Source.NameAt(ClassPackage);
		Import.ClassName = *Source.NameAt(ClassName);
		Import.Outer = Outer;
		Import.Name = *Source.NameAt(ObjectName);
	}
	if (!Reader.SetOffset(static_cast<uint64>(ExportOffset)))
	{
		return FResult::Error(WoTWorldError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the world export table is outside the package"), Source.SourcePath));
	}
	for (int32 Index = 0; Index < ExportCount; ++Index)
	{
		int32 ClassReference = 0, SuperReference = 0, Outer = 0, NameIndex = 0;
		uint32 ObjectFlags = 0;
		int32 SerialSize = 0, SerialOffset = 0;
		if (!Reader.ReadCompact(ClassReference) || !Reader.ReadCompact(SuperReference)
			|| !Reader.ReadI32(Outer) || !Reader.ReadCompact(NameIndex)
			|| !Reader.ReadU32(ObjectFlags) || !Reader.ReadCompact(SerialSize)
			|| SerialSize < 0 || Source.NameAt(NameIndex) == nullptr
			|| (SerialSize > 0 && !Reader.ReadCompact(SerialOffset)) || SerialOffset < 0
			|| static_cast<uint64>(SerialOffset) > static_cast<uint64>(Source.Bytes.Num())
			|| static_cast<uint64>(SerialSize)
				> static_cast<uint64>(Source.Bytes.Num()) - static_cast<uint64>(SerialOffset))
		{
			return FResult::Error(WoTWorldError(EWoTBootstrapError::CorruptInstallation,
				TEXT("the world export table is invalid"), Source.SourcePath));
		}
		FWoTWorldExport& Export = Source.Exports.AddDefaulted_GetRef();
		Export.ClassReference = ClassReference;
		Export.SuperReference = SuperReference;
		Export.Outer = Outer;
		Export.Name = *Source.NameAt(NameIndex);
		Export.Flags = ObjectFlags;
		Export.Offset = static_cast<uint64>(SerialOffset);
		Export.Size = static_cast<uint64>(SerialSize);
	}
	for (int32 Index = 0; Index < Source.Imports.Num(); ++Index)
	{
		if (!WoTWorldReferenceValid(Source, Source.Imports[Index].Outer))
		{
			return FResult::Error(WoTWorldError(EWoTBootstrapError::CorruptInstallation,
				TEXT("the world import table has an invalid outer"), Source.SourcePath));
		}
	}
	for (int32 Index = 0; Index < Source.Exports.Num(); ++Index)
	{
		const FWoTWorldExport& Export = Source.Exports[Index];
		if (!WoTWorldReferenceValid(Source, Export.ClassReference)
			|| !WoTWorldReferenceValid(Source, Export.SuperReference)
			|| !WoTWorldReferenceValid(Source, Export.Outer))
		{
			return FResult::Error(WoTWorldError(EWoTBootstrapError::CorruptInstallation,
				TEXT("the world export table has an invalid object reference"), Source.SourcePath));
		}
		TWoTBootstrapResult<FString> Path = WoTWorldObjectPath(Source, Index + 1);
		if (Path.IsError()) return FResult::Error(MoveTemp(Path).GetError());
	}
	return FResult::Success(MoveTemp(Source));
}

bool WoTWorldReadFString(FWoTWorldReader& Reader)
{
	int32 Length = 0;
	if (!Reader.ReadCompact(Length)) return false;
	if (Length == 0) return true;
	const int64 Characters = FMath::Abs(static_cast<int64>(Length));
	if (Characters > 1000000) return false;
	const uint64 Bytes = static_cast<uint64>(Characters) * (Length < 0 ? 2ull : 1ull);
	TArray<uint8> Raw;
	if (!Reader.ReadBytes(Bytes, Raw)) return false;
	return Length > 0
		? Raw.Last() == 0
		: Raw.Num() >= 2 && Raw[Raw.Num() - 1] == 0 && Raw[Raw.Num() - 2] == 0;
}

bool WoTWorldSkipURL(FWoTWorldReader& Reader)
{
	for (int32 Index = 0; Index < 4; ++Index)
	{
		if (!WoTWorldReadFString(Reader)) return false;
	}
	int32 Options = 0;
	if (!Reader.ReadCompact(Options) || Options < 0 || Options > 65536) return false;
	for (int32 Index = 0; Index < Options; ++Index)
	{
		if (!WoTWorldReadFString(Reader)) return false;
	}
	return Reader.Skip(8);
}

FWoTBootstrapStatus WoTWorldValidateAttachments(TConstArrayView<int32> Parents)
{
	TArray<uint8> State;
	State.SetNumZeroed(Parents.Num());
	for (int32 Start = 0; Start < Parents.Num(); ++Start)
	{
		int32 Current = Start;
		TArray<int32> Chain;
		while (Current >= 0)
		{
			if (Current < 0 || Current >= Parents.Num())
			{
				return FWoTBootstrapStatus::Error(WoTWorldError(
					EWoTBootstrapError::CorruptInstallation,
					TEXT("a world attachment parent is outside the actor registry")));
			}
			if (State[Current] == 1)
			{
				return FWoTBootstrapStatus::Error(WoTWorldError(
					EWoTBootstrapError::CorruptInstallation,
					TEXT("the world attachment graph contains a cycle")));
			}
			if (State[Current] == 2) break;
			State[Current] = 1;
			Chain.Add(Current);
			Current = Parents[Current];
		}
		for (int32 Actor : Chain) State[Actor] = 2;
	}
	return FWoTBootstrapStatus::Success();
}

struct FWoTWorldSemanticValidationObject
{
	FString Identity;
	FString ClassIdentity;
	bool bActor = false;
};

struct FWoTWorldSemanticValidationReference
{
	FString Owner;
	FString Target;
};

struct FWoTWorldSemanticValidationAttachment
{
	FString Child;
	FString Parent;
};

struct FWoTWorldSemanticValidationSound
{
	FString Actor;
	FString Asset;
};

struct FWoTWorldSemanticValidationNode
{
	int32 Surface = INDEX_NONE;
	int32 Back = INDEX_NONE;
	int32 Front = INDEX_NONE;
	TArray<int32> VertexIndices;
	int32 Zones[2] = {};
};

struct FWoTWorldSemanticValidationPortal
{
	int32 Surface = INDEX_NONE;
	int32 Zones[2] = {};
};

struct FWoTWorldSemanticValidationMap
{
	FString Identity;
	FWoTSha256 MapRoot;
	TArray<FWoTWorldSemanticValidationObject> Objects;
	TArray<FWoTWorldSemanticValidationReference> References;
	TArray<FWoTWorldSemanticValidationAttachment> Attachments;
	TArray<FString> Lights;
	TArray<FString> NavigationSeeds;
	TArray<FWoTWorldSemanticValidationSound> Sounds;
	FString SkyZone;
	TArray<uint32> SurfaceFlags;
	TArray<FWoTWorldSemanticValidationNode> Nodes;
	TArray<FString> ZoneActors;
	int32 ZoneLimit = 0;
	int32 MinimumNodeVertices = 0;
	TArray<FWoTWorldSemanticValidationPortal> Portals;
	TArray<int32> CollisionSurfaces;
};

FString WoTWorldClassLeaf(FStringView Identity)
{
	const FString Value(Identity);
	int32 Dot = INDEX_NONE;
	return Value.FindLastChar(TEXT('.'), Dot) ? Value.Mid(Dot + 1) : Value;
}

bool WoTWorldIsMoverClass(FStringView Identity)
{
	static constexpr const TCHAR* MoverClasses[] = {
		TEXT("Mover"), TEXT("DoorMover"), TEXT("LockableMover"),
		TEXT("PortcullisMover"), TEXT("ExplodingMover"),
		TEXT("RotatingMover"), TEXT("LoopMover"), TEXT("TimedMover"),
		TEXT("AssertMover"), TEXT("ElevatorMover"), TEXT("AttachMover"),
		TEXT("GradualMover"), TEXT("MixMover")};
	const FString Leaf = WoTWorldClassLeaf(Identity);
	for (const TCHAR* MoverClass : MoverClasses)
	{
		if (Leaf.Equals(MoverClass, ESearchCase::IgnoreCase)) return true;
	}
	return false;
}

bool WoTWorldIsNavigationClass(FStringView Identity)
{
	const FString Leaf = WoTWorldClassLeaf(Identity);
	return Leaf.Equals(TEXT("WotPathNode"), ESearchCase::IgnoreCase)
		|| Leaf.Equals(TEXT("PathNode"), ESearchCase::IgnoreCase)
		|| Leaf.Equals(TEXT("NavigationPoint"), ESearchCase::IgnoreCase);
}

FWoTBootstrapStatus WoTWorldValidateSemanticMaps(
	TConstArrayView<FWoTWorldSemanticValidationMap> Maps,
	bool bRequireCompleteSet)
{
	if (bRequireCompleteSet
		&& Maps.Num() != UE_ARRAY_COUNT(WoT::Retail::Generated::RequiredWorlds))
	{
		return FWoTBootstrapStatus::Error(WoTWorldError(
			EWoTBootstrapError::UnknownField,
			TEXT("the semantic world set is not the complete generated registry")));
	}
	TMap<FString, const FWoTWorldSemanticValidationObject*> Objects;
	TSet<FString> Actors;
	for (int32 MapIndex = 0; MapIndex < Maps.Num(); ++MapIndex)
	{
		const FWoTWorldSemanticValidationMap& Map = Maps[MapIndex];
		if (Map.Identity.IsEmpty()
			|| (bRequireCompleteSet
				&& Map.Identity
					!= WoT::Retail::Generated::RequiredWorlds[MapIndex].Identity))
		{
			return FWoTBootstrapStatus::Error(WoTWorldError(
				EWoTBootstrapError::UnknownField,
				TEXT("a semantic map identity is outside the generated registry"),
				Map.Identity));
		}
		for (const FWoTWorldSemanticValidationObject& Object : Map.Objects)
		{
			const FString Key = WoTWorldFold(Object.Identity);
			if (Object.Identity.IsEmpty() || Object.ClassIdentity.IsEmpty()
				|| Objects.Contains(Key))
			{
				return FWoTBootstrapStatus::Error(WoTWorldError(
					EWoTBootstrapError::DuplicateField,
					TEXT("the semantic object registry is missing or duplicate"),
					Object.Identity));
			}
			Objects.Add(Key, &Object);
			if (Object.bActor) Actors.Add(Key);
		}
	}
	auto FindObject = [&Objects](FStringView Identity)
		-> const FWoTWorldSemanticValidationObject*
	{
		const FWoTWorldSemanticValidationObject* const* Found =
			Objects.Find(WoTWorldFold(Identity));
		return Found != nullptr ? *Found : nullptr;
	};
	for (const FWoTWorldSemanticValidationMap& Map : Maps)
	{
		for (const FWoTWorldSemanticValidationReference& Reference : Map.References)
		{
			if (!Actors.Contains(WoTWorldFold(Reference.Owner))
				|| FindObject(Reference.Target) == nullptr)
			{
				return FWoTBootstrapStatus::Error(WoTWorldError(
					EWoTBootstrapError::CorruptInstallation,
					TEXT("a dangling actor/resource reference escaped semantic closure"),
					Reference.Owner + TEXT(" -> ") + Reference.Target));
			}
		}
		TMap<FString, int32> LocalActors;
		for (const FWoTWorldSemanticValidationObject& Object : Map.Objects)
			if (Object.bActor)
				LocalActors.Add(WoTWorldFold(Object.Identity), LocalActors.Num());
		TArray<int32> Parents;
		Parents.Init(INDEX_NONE, LocalActors.Num());
		for (const FWoTWorldSemanticValidationAttachment& Attachment : Map.Attachments)
		{
			const int32* Child = LocalActors.Find(WoTWorldFold(Attachment.Child));
			const int32* Parent = LocalActors.Find(WoTWorldFold(Attachment.Parent));
			if (Child == nullptr || Parent == nullptr || Parents[*Child] != INDEX_NONE)
			{
				return FWoTBootstrapStatus::Error(WoTWorldError(
					EWoTBootstrapError::CorruptInstallation,
					TEXT("an attachment has a missing/duplicate actor binding"),
					Attachment.Child));
			}
			Parents[*Child] = *Parent;
		}
		FWoTBootstrapStatus AttachmentStatus =
			WoTWorldValidateAttachments(Parents);
		if (AttachmentStatus.IsError()) return AttachmentStatus;
		for (const FString& Identity : Map.Lights)
		{
			const FWoTWorldSemanticValidationObject* Object = FindObject(Identity);
			if (Object == nullptr || !Object->bActor
				|| !WoTWorldClassLeaf(Object->ClassIdentity).Equals(
					TEXT("Light"), ESearchCase::IgnoreCase))
			{
				return FWoTBootstrapStatus::Error(WoTWorldError(
					EWoTBootstrapError::CorruptInstallation,
					TEXT("the light registry has a wrong-type binding"), Identity));
			}
		}
		for (const FString& Identity : Map.NavigationSeeds)
		{
			const FWoTWorldSemanticValidationObject* Object = FindObject(Identity);
			if (Object == nullptr || !Object->bActor
				|| !WoTWorldIsNavigationClass(Object->ClassIdentity))
			{
				return FWoTBootstrapStatus::Error(WoTWorldError(
					EWoTBootstrapError::CorruptInstallation,
					TEXT("the navigation seed registry has a wrong-type binding"),
					Identity));
			}
		}
		if (!Map.SkyZone.IsEmpty())
		{
			const FWoTWorldSemanticValidationObject* Sky =
				FindObject(Map.SkyZone);
			if (Sky == nullptr || !Sky->bActor
				|| !WoTWorldClassLeaf(Sky->ClassIdentity).Equals(
					TEXT("SkyZoneInfo"), ESearchCase::IgnoreCase))
			{
				return FWoTBootstrapStatus::Error(WoTWorldError(
					EWoTBootstrapError::CorruptInstallation,
					TEXT("the sky/zone binding has the wrong semantic type"),
					Map.SkyZone));
			}
		}
		for (const FWoTWorldSemanticValidationSound& Sound : Map.Sounds)
		{
			const FWoTWorldSemanticValidationObject* Actor =
				FindObject(Sound.Actor);
			const FWoTWorldSemanticValidationObject* Asset =
				FindObject(Sound.Asset);
			if (Actor == nullptr || !Actor->bActor
				|| !WoTWorldClassLeaf(Actor->ClassIdentity).Equals(
					TEXT("AmbientSound"), ESearchCase::IgnoreCase)
				|| Asset == nullptr || Asset->bActor
				|| !WoTWorldClassLeaf(Asset->ClassIdentity).Equals(
					TEXT("Sound"), ESearchCase::IgnoreCase))
			{
				return FWoTBootstrapStatus::Error(WoTWorldError(
					EWoTBootstrapError::CorruptInstallation,
					TEXT("a sound binding has the wrong actor/resource type"),
					Sound.Actor));
			}
		}
		if (Map.ZoneLimit <= 0 || Map.ZoneLimit > WoTWorldMaxZones)
		{
			return FWoTBootstrapStatus::Error(WoTWorldError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("the semantic BSP zone limit is invalid"), Map.Identity));
		}
		for (const FString& ZoneActor : Map.ZoneActors)
			if (!ZoneActor.IsEmpty()
				&& !Actors.Contains(WoTWorldFold(ZoneActor)))
			{
				return FWoTBootstrapStatus::Error(WoTWorldError(
					EWoTBootstrapError::CorruptInstallation,
					TEXT("a BSP zone actor is outside the actor registry"),
					ZoneActor));
			}
		for (const FWoTWorldSemanticValidationNode& Node : Map.Nodes)
		{
			if (!Map.SurfaceFlags.IsValidIndex(Node.Surface)
				|| (Node.Back != INDEX_NONE && !Map.Nodes.IsValidIndex(Node.Back))
				|| (Node.Front != INDEX_NONE && !Map.Nodes.IsValidIndex(Node.Front))
				|| Node.VertexIndices.Num() < Map.MinimumNodeVertices
				|| Node.VertexIndices.ContainsByPredicate(
					[](int32 Vertex) { return Vertex < 0; })
				|| Node.Zones[0] < 0 || Node.Zones[0] >= Map.ZoneLimit
				|| Node.Zones[1] < 0 || Node.Zones[1] >= Map.ZoneLimit)
			{
				return FWoTBootstrapStatus::Error(WoTWorldError(
					EWoTBootstrapError::CorruptInstallation,
					TEXT("a corrupt BSP node escaped semantic closure"),
					Map.Identity));
			}
		}
		for (const FWoTWorldSemanticValidationPortal& Portal : Map.Portals)
		{
			if (!Map.SurfaceFlags.IsValidIndex(Portal.Surface)
				|| (Map.SurfaceFlags[Portal.Surface] & WoTWorldPFPortal) == 0
				|| Portal.Zones[0] < 0 || Portal.Zones[0] >= Map.ZoneLimit
				|| Portal.Zones[1] < 0 || Portal.Zones[1] >= Map.ZoneLimit)
			{
				return FWoTBootstrapStatus::Error(WoTWorldError(
					EWoTBootstrapError::CorruptInstallation,
					TEXT("a corrupt BSP portal escaped semantic closure"),
					Map.Identity));
			}
		}
		for (int32 Surface : Map.CollisionSurfaces)
			if (!Map.SurfaceFlags.IsValidIndex(Surface))
			{
				return FWoTBootstrapStatus::Error(WoTWorldError(
					EWoTBootstrapError::CorruptInstallation,
					TEXT("an invalid collision surface escaped semantic closure"),
					Map.Identity));
			}
	}
	return FWoTBootstrapStatus::Success();
}

bool WoTWorldReadCount(FWoTWorldReader& Reader, uint32 Maximum, int32& Out);

bool WoTWorldReadObjectReference(FWoTWorldReader& Reader,
	const FWoTWorldSource& Source)
{
	int32 Reference = 0;
	return Reader.ReadCompact(Reference)
		&& WoTWorldReferenceValid(Source, Reference);
}

bool WoTWorldSkipTravelInfoMap(FWoTWorldReader& Reader)
{
	int32 PairCount = 0;
	if (!WoTWorldReadCount(Reader, WoTWorldMaxTravelInfoItems, PairCount))
		return false;
	for (int32 PairIndex = 0; PairIndex < PairCount; ++PairIndex)
	{
		if (!WoTWorldReadFString(Reader) || !WoTWorldReadFString(Reader))
			return false;
	}
	return true;
}

bool WoTWorldSkipFStringArray(FWoTWorldReader& Reader, int32& OutCount)
{
	if (!WoTWorldReadCount(Reader, WoTWorldMaxTravelInfoItems, OutCount))
		return false;
	for (int32 Index = 0; Index < OutCount; ++Index)
	{
		if (!WoTWorldReadFString(Reader)) return false;
	}
	return true;
}

bool WoTWorldSkipDestroyObserverMap(FWoTWorldReader& Reader,
	const FWoTWorldSource& Source)
{
	int32 PairCount = 0;
	if (!WoTWorldReadCount(Reader, WoTWorldMaxReferences, PairCount))
		return false;
	uint64 ReferenceCount = static_cast<uint64>(PairCount);
	for (int32 PairIndex = 0; PairIndex < PairCount; ++PairIndex)
	{
		int32 ObserverCount = 0;
		if (!WoTWorldReadObjectReference(Reader, Source)
			|| !WoTWorldReadCount(Reader, WoTWorldMaxReferences, ObserverCount)
			|| static_cast<uint64>(ObserverCount) >
				static_cast<uint64>(WoTWorldMaxReferences) - ReferenceCount)
		{
			return false;
		}
		ReferenceCount += static_cast<uint64>(ObserverCount);
		for (int32 ObserverIndex = 0; ObserverIndex < ObserverCount;
			++ObserverIndex)
		{
			if (!WoTWorldReadObjectReference(Reader, Source)) return false;
		}
	}
	return true;
}

bool WoTWorldSkipLevelTail(FWoTWorldReader& Reader,
	const FWoTWorldSource& Source)
{
	float ApproxTime = 0.0f;
	if (!Reader.ReadFloat(ApproxTime)
		|| !WoTWorldReadObjectReference(Reader, Source))
	{
		return false;
	}
	for (int32 TextBlockIndex = 0; TextBlockIndex < 16; ++TextBlockIndex)
	{
		if (!WoTWorldReadObjectReference(Reader, Source)) return false;
	}

	if (Source.Version > 62)
	{
		if (!WoTWorldSkipTravelInfoMap(Reader)) return false;
	}
	else if (Source.Version >= 61)
	{
		int32 NameCount = 0, ItemCount = 0;
		if (!WoTWorldSkipFStringArray(Reader, NameCount)
			|| !WoTWorldSkipFStringArray(Reader, ItemCount)
			|| NameCount != ItemCount)
		{
			return false;
		}
	}
	else
	{
		return false;
	}

	// Legend's saved v68 ULevel continues through UObject's destroy-observer
	// extension. Version 64 used a DWORD presence marker; version 2 stores the
	// TMap<UObject*, TArray<UObject*>> directly in versions above 67.
	if (Source.Version == 64)
	{
		uint32 HasDestroyObservers = 0;
		if (!Reader.ReadU32(HasDestroyObservers)
			|| (HasDestroyObservers != 0
				&& !WoTWorldSkipDestroyObserverMap(Reader, Source)))
		{
			return false;
		}
	}
	else if (Source.Version > 67
		&& !WoTWorldSkipDestroyObserverMap(Reader, Source))
	{
		return false;
	}
	return true;
}

TWoTBootstrapResult<FWoTWorldSource> WoTWorldDiscover(FWoTWorldSource&& Source)
{
	using FResult = TWoTBootstrapResult<FWoTWorldSource>;
	for (int32 Index = 0; Index < Source.Exports.Num(); ++Index)
	{
		const FString* ClassName = Source.ClassName(Source.Exports[Index].ClassReference);
		if (ClassName != nullptr && *ClassName == TEXT("Level"))
		{
			if (Source.LevelReference != 0)
			{
				return FResult::Error(WoTWorldError(EWoTBootstrapError::DuplicateField,
					TEXT("a world package contains more than one Level export"), Source.SourcePath));
			}
			Source.LevelReference = Index + 1;
		}
	}
	if (Source.LevelReference == 0)
	{
		return FResult::Error(WoTWorldError(EWoTBootstrapError::UnknownField,
			TEXT("the world package has no Level export"), Source.SourcePath));
	}
	FWoTWorldReader Reader;
	const FWoTWorldExport& Level = Source.Exports[Source.LevelReference - 1];
	uint64 PropertyCount = 0;
	int32 ActorCount = 0, ActorMaximum = 0;
	if (!WoTWorldOpenExport(Source, Level, Reader)
		|| !WoTWorldReadProperties(Reader, Source, nullptr, PropertyCount)
		|| !Reader.ReadI32(ActorCount) || !Reader.ReadI32(ActorMaximum)
		|| ActorCount < 0 || ActorMaximum < ActorCount
		|| ActorCount > static_cast<int32>(WoTWorldMaxActors))
	{
		return FResult::Error(WoTWorldError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the Level actor database is invalid"), Source.SourcePath));
	}
	TArray<int32> ActorReferences;
	ActorReferences.Reserve(ActorCount);
	for (int32 Index = 0; Index < ActorCount; ++Index)
	{
		int32 Reference = 0;
		if (!Reader.ReadCompact(Reference) || Reference < 0
			|| !WoTWorldReferenceValid(Source, Reference))
		{
			return FResult::Error(WoTWorldError(EWoTBootstrapError::CorruptInstallation,
				TEXT("the Level actor list contains an invalid export reference"),
				Source.SourcePath));
		}
		if (Reference > 0) ActorReferences.Add(Reference);
	}
	if (!WoTWorldSkipURL(Reader) || !Reader.ReadCompact(Source.ModelReference)
		|| Source.ModelReference <= 0
		|| !Source.Exports.IsValidIndex(Source.ModelReference - 1))
	{
		return FResult::Error(WoTWorldError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the Level model reference is invalid"), Source.SourcePath));
	}
	const FString* ModelClass =
		Source.ClassName(Source.Exports[Source.ModelReference - 1].ClassReference);
	if (ModelClass == nullptr || *ModelClass != TEXT("Model"))
	{
		return FResult::Error(WoTWorldError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the Level model reference is not a Model export"), Source.SourcePath));
	}
	int32 ReachSpecCount = 0;
	if (!WoTWorldReadCount(Reader, WoTWorldMaxReachSpecs, ReachSpecCount))
	{
		return FResult::Error(WoTWorldError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the Level ReachSpec count is invalid"), Source.SourcePath));
	}
	Source.ReachSpecs.Reserve(ReachSpecCount);
	for (int32 ReachSpecIndex = 0; ReachSpecIndex < ReachSpecCount; ++ReachSpecIndex)
	{
		FWoTWorldReachSpec& ReachSpec = Source.ReachSpecs.AddDefaulted_GetRef();
		uint8 Pruned = 0;
		if (!Reader.ReadI32(ReachSpec.Distance)
			|| !Reader.ReadCompact(ReachSpec.StartReference)
			|| !Reader.ReadCompact(ReachSpec.EndReference)
			|| !Reader.ReadI32(ReachSpec.CollisionRadius)
			|| !Reader.ReadI32(ReachSpec.CollisionHeight)
			|| !Reader.ReadU32(ReachSpec.ReachFlags)
			|| !Reader.ReadU8(Pruned)
			|| ReachSpec.StartReference < 0 || ReachSpec.EndReference < 0
			|| !WoTWorldReferenceValid(Source, ReachSpec.StartReference)
			|| !WoTWorldReferenceValid(Source, ReachSpec.EndReference)
			|| (Pruned != 0 && Pruned != 1))
		{
			return FResult::Error(WoTWorldError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("a Level ReachSpec record is invalid"), Source.SourcePath));
		}
		ReachSpec.bPruned = Pruned != 0;
	}
	if (!WoTWorldSkipLevelTail(Reader, Source))
	{
		return FResult::Error(WoTWorldError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the Level serialized tail is invalid"), Source.SourcePath));
	}
	if (Reader.Tell() != Reader.GetEnd())
	{
		return FResult::Error(WoTWorldError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the Level export has an unconsumed or trailing tail"),
			Source.SourcePath));
	}

	TMap<FString, int32> ActorByIdentity;
	TMap<int32, int32> RegistryByExport;
	for (int32 Reference : ActorReferences)
	{
		if (RegistryByExport.Contains(Reference)) continue;
		const FWoTWorldExport& Export = Source.Exports[Reference - 1];
		TWoTBootstrapResult<FString> Identity = WoTWorldObjectPath(Source, Reference);
		TWoTBootstrapResult<FString> ClassIdentity =
			WoTWorldObjectPath(Source, Export.ClassReference);
		if (Identity.IsError()) return FResult::Error(MoveTemp(Identity).GetError());
		if (ClassIdentity.IsError()) return FResult::Error(MoveTemp(ClassIdentity).GetError());
		const FString Folded = WoTWorldFold(Identity.GetValue());
		if (const int32* Prior = ActorByIdentity.Find(Folded))
		{
			if (*Prior != Reference)
			{
				return FResult::Error(WoTWorldError(EWoTBootstrapError::DuplicateField,
					TEXT("two actor exports resolve to the same world identity"),
					Identity.GetValue()));
			}
			continue;
		}
		FWoTWorldActor& Actor = Source.Actors.AddDefaulted_GetRef();
		Actor.ExportReference = Reference;
		Actor.Identity = MoveTemp(Identity).GetValue();
		Actor.ClassIdentity = MoveTemp(ClassIdentity).GetValue();
		TWoTBootstrapResult<int32> ActorBinding =
			WoTWorldBindSemanticObject(Source, Reference, true);
		if (ActorBinding.IsError())
			return FResult::Error(MoveTemp(ActorBinding).GetError());
		FWoTWorldReader ActorReader;
		if (!WoTWorldOpenExport(Source, Export, ActorReader)
			|| !WoTWorldReadProperties(
				ActorReader, Source, &Actor.Properties, PropertyCount))
		{
			return FResult::Error(WoTWorldError(EWoTBootstrapError::CorruptInstallation,
				TEXT("an actor property stream is invalid"), Actor.Identity));
		}
		bool bMoverProperties = false;
		for (const FWoTTypedProperty& Property : Actor.Properties)
		{
			if (Property.Name == TEXT("Base") && Property.ObjectReference.IsSet())
			{
				Actor.ParentActor = Property.ObjectReference.GetValue();
			}
			if (Property.Name.Contains(TEXT("Sound"))
				&& Property.ObjectReference.IsSet()
				&& Property.ObjectReference.GetValue() != 0)
			{
				++Source.SoundCount;
			}
			if (Property.Name == TEXT("KeyNum")
				|| Property.Name == TEXT("MoveTime")
				|| Property.Name == TEXT("MoverEncroachType")) bMoverProperties = true;
		}
		const FString* LeafClass = Source.ClassName(Export.ClassReference);
		if (LeafClass != nullptr)
		{
			if (*LeafClass == TEXT("Mover") || bMoverProperties) ++Source.MoverCount;
			if (*LeafClass == TEXT("Light")) ++Source.LightCount;
			if (*LeafClass == TEXT("WotPathNode")
				|| *LeafClass == TEXT("PathNode")
				|| *LeafClass == TEXT("NavigationPoint")) ++Source.NavigationSeedCount;
			if (*LeafClass == TEXT("AmbientSound")) ++Source.SoundCount;
		}
		ActorByIdentity.Add(Folded, Reference);
		RegistryByExport.Add(Reference, Source.Actors.Num() - 1);
	}
	// Pass two closes every property reference against the complete actor and
	// resource registry, then applies the generated ue1pkg reflection facts.
	// Nothing can reach the writer until this succeeds for all nineteen maps.
	for (FWoTWorldActor& Actor : Source.Actors)
	{
		FWoTBootstrapStatus Bound = WoTBindTypedPropertyReferences(
			Actor.Properties, [&Source](int32 Reference)
				-> TWoTBootstrapResult<FWoTTypedObjectBinding>
			{
				using FBindingResult = TWoTBootstrapResult<FWoTTypedObjectBinding>;
				TWoTBootstrapResult<int32> BoundObject =
					WoTWorldBindSemanticObject(Source, Reference, false);
				if (BoundObject.IsError())
					return FBindingResult::Error(MoveTemp(BoundObject).GetError());
				const FWoTWorldSemanticObject& Object =
					Source.SemanticObjects[BoundObject.GetValue()];
				return FBindingResult::Success(FWoTTypedObjectBinding{
					Object.Identity, Object.ClassIdentity});
			});
		if (Bound.IsError()) return FResult::Error(MoveTemp(Bound).GetError());
		FWoTBootstrapStatus Typed = WoTWorldValidateTypedProperties(Actor);
		if (Typed.IsError()) return FResult::Error(MoveTemp(Typed).GetError());
	}
	// MoverCount is the exact Brush->Engine.Model census, not a class-name
	// heuristic. It is also the number of embedded mover-model records.
	Source.MoverCount = 0;
	TSet<int32> MoverModelReferences;
	for (const FWoTWorldActor& Actor : Source.Actors)
	{
		if (!WoTWorldIsMoverClass(Actor.ClassIdentity)) continue;
		for (const FWoTTypedProperty& Property : Actor.Properties)
		{
			if (!Property.Name.Equals(TEXT("Brush"), ESearchCase::IgnoreCase)
				|| Property.Type != 5 || !Property.ObjectReference.IsSet()
				|| Property.ObjectReference.GetValue() <= 0
				|| !Property.ObjectClassIdentity.Equals(
					TEXT("Engine.Model"), ESearchCase::IgnoreCase)) continue;
			if (MoverModelReferences.Contains(Property.ObjectReference.GetValue()))
			{
				return FResult::Error(WoTWorldError(EWoTBootstrapError::DuplicateField,
					TEXT("two world actors share one mover Brush Model"), Actor.Identity));
			}
			MoverModelReferences.Add(Property.ObjectReference.GetValue());
			++Source.MoverCount;
		}
	}
	TArray<int32> Parents;
	Parents.Init(-1, Source.Actors.Num());
	for (int32 ActorIndex = 0; ActorIndex < Source.Actors.Num(); ++ActorIndex)
	{
		const FWoTWorldActor& Actor = Source.Actors[ActorIndex];
		if (!Actor.ParentActor.IsSet() || Actor.ParentActor.GetValue() == 0) continue;
		const int32* ParentIndex = RegistryByExport.Find(Actor.ParentActor.GetValue());
		if (ParentIndex == nullptr)
		{
			return FResult::Error(WoTWorldError(EWoTBootstrapError::CorruptInstallation,
				TEXT("an actor attachment points outside the Level actor registry"),
				Actor.Identity));
		}
		Parents[ActorIndex] = *ParentIndex;
		++Source.AttachmentCount;
	}
	FWoTBootstrapStatus Attachments = WoTWorldValidateAttachments(Parents);
	if (Attachments.IsError()) return FResult::Error(MoveTemp(Attachments).GetError());
	for (FWoTWorldReachSpec& ReachSpec : Source.ReachSpecs)
	{
		for (TPair<int32, FString*> Endpoint : {
			TPair<int32, FString*>(ReachSpec.StartReference, &ReachSpec.StartIdentity),
			TPair<int32, FString*>(ReachSpec.EndReference, &ReachSpec.EndIdentity)})
		{
			if (Endpoint.Key == 0) continue;
			if (!RegistryByExport.Contains(Endpoint.Key))
			{
				return FResult::Error(WoTWorldError(
					EWoTBootstrapError::CorruptInstallation,
					TEXT("a ReachSpec endpoint is outside the Level actor registry"),
					Source.SourcePath));
			}
			TWoTBootstrapResult<int32> Binding =
				WoTWorldBindSemanticObject(Source, Endpoint.Key, true);
			if (Binding.IsError())
				return FResult::Error(MoveTemp(Binding).GetError());
			*Endpoint.Value = Source.SemanticObjects[Binding.GetValue()].Identity;
		}
	}
	return FResult::Success(MoveTemp(Source));
}

bool WoTWorldReadCount(FWoTWorldReader& Reader, uint32 Maximum, int32& Out)
{
	return Reader.ReadCompact(Out) && Out >= 0 && static_cast<uint32>(Out) <= Maximum;
}

TWoTBootstrapResult<FWoTWorldModel> WoTWorldDecodeModel(
	FWoTWorldSource& Source, int32 ModelReference = INDEX_NONE)
{
	using FResult = TWoTBootstrapResult<FWoTWorldModel>;
	if (ModelReference == INDEX_NONE) ModelReference = Source.ModelReference;
	if (ModelReference <= 0 || !Source.Exports.IsValidIndex(ModelReference - 1))
	{
		return FResult::Error(WoTWorldError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the requested Model reference is invalid"), Source.SourcePath));
	}
	const FWoTWorldExport& Export = Source.Exports[ModelReference - 1];
	const FString* ModelClass = Source.ClassName(Export.ClassReference);
	FWoTWorldReader Reader;
	uint64 PropertyCount = 0;
	int32 Count = 0;
	FWoTWorldModel Model;
	if (ModelClass == nullptr || *ModelClass != TEXT("Model")
		|| !WoTWorldOpenExport(Source, Export, Reader)
		|| !WoTWorldReadProperties(Reader, Source, nullptr, PropertyCount)
		|| !Reader.Skip(25 + 16)
		|| !WoTWorldReadCount(Reader, WoTWorldMaxBSPVertices, Count))
	{
		return FResult::Error(WoTWorldError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the world Model header is invalid"), Source.SourcePath));
	}
	Model.Vectors.Reserve(Count);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		FVector3f& Vector = Model.Vectors.AddDefaulted_GetRef();
		if (!Reader.ReadVector(Vector))
		{
			return FResult::Error(WoTWorldError(EWoTBootstrapError::InvalidNumber,
				TEXT("a BSP vector is truncated or non-finite"), Source.SourcePath));
		}
	}
	if (!WoTWorldReadCount(Reader, WoTWorldMaxBSPVertices, Count))
	{
		return FResult::Error(WoTWorldError(EWoTBootstrapError::SizeLimit,
			TEXT("the BSP point vector exceeds its bound"), Source.SourcePath));
	}
	Model.Points.Reserve(Count);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		FVector3f& Point = Model.Points.AddDefaulted_GetRef();
		if (!Reader.ReadVector(Point)
			|| FMath::Abs(Point.X) > WoTWorldMaxCoordinateAbs
			|| FMath::Abs(Point.Y) > WoTWorldMaxCoordinateAbs
			|| FMath::Abs(Point.Z) > WoTWorldMaxCoordinateAbs)
		{
			return FResult::Error(WoTWorldError(EWoTBootstrapError::InvalidNumber,
				TEXT("a BSP point is non-finite or exceeds the coordinate cap"),
				Source.SourcePath));
		}
	}
	if (!WoTWorldReadCount(Reader, WoTWorldMaxBSPVertices, Count))
	{
		return FResult::Error(WoTWorldError(EWoTBootstrapError::SizeLimit,
			TEXT("the BSP node vector exceeds its bound"), Source.SourcePath));
	}
	Model.Nodes.Reserve(Count);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		FWoTWorldNode& Node = Model.Nodes.AddDefaulted_GetRef();
		if (!Reader.ReadFloat(Node.Plane[0]) || !Reader.ReadFloat(Node.Plane[1])
			|| !Reader.ReadFloat(Node.Plane[2]) || !Reader.ReadFloat(Node.Plane[3])
			|| !Reader.ReadU64(Node.ZoneMask) || !Reader.ReadU8(Node.Flags)
			|| !Reader.ReadCompact(Node.VertexPool) || !Reader.ReadCompact(Node.Surface)
			|| !Reader.ReadCompact(Node.Back) || !Reader.ReadCompact(Node.Front)
			|| !Reader.ReadCompact(Node.PlaneNode)
			|| !Reader.ReadCompact(Node.CollisionBound)
			|| !Reader.ReadCompact(Node.RenderBound)
			|| !Reader.ReadU8(Node.Zones[0]) || !Reader.ReadU8(Node.Zones[1])
			|| !Reader.ReadU8(Node.VertexCount)
			|| !Reader.ReadI32(Node.Leaves[0]) || !Reader.ReadI32(Node.Leaves[1]))
		{
			return FResult::Error(WoTWorldError(EWoTBootstrapError::CorruptInstallation,
				TEXT("a BSP node record is truncated"), Source.SourcePath));
		}
	}
	if (!WoTWorldReadCount(Reader, WoTWorldMaxBSPVertices, Count))
	{
		return FResult::Error(WoTWorldError(EWoTBootstrapError::SizeLimit,
			TEXT("the BSP surface vector exceeds its bound"), Source.SourcePath));
	}
	Model.Surfaces.Reserve(Count);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		FWoTWorldSurface& Surface = Model.Surfaces.AddDefaulted_GetRef();
		if (!Reader.ReadCompact(Surface.Texture) || !Reader.ReadU32(Surface.Flags)
			|| !Reader.ReadCompact(Surface.Base) || !Reader.ReadCompact(Surface.Normal)
			|| !Reader.ReadCompact(Surface.TextureU)
			|| !Reader.ReadCompact(Surface.TextureV)
			|| !Reader.ReadCompact(Surface.LightMap)
			|| !Reader.ReadCompact(Surface.BrushPoly)
			|| !Reader.ReadI16(Surface.PanU) || !Reader.ReadI16(Surface.PanV)
			|| !Reader.ReadCompact(Surface.Actor))
		{
			return FResult::Error(WoTWorldError(EWoTBootstrapError::CorruptInstallation,
				TEXT("a BSP surface record is truncated"), Source.SourcePath));
		}
	}
	if (!WoTWorldReadCount(Reader, WoTWorldMaxBSPIndices, Count))
	{
		return FResult::Error(WoTWorldError(EWoTBootstrapError::SizeLimit,
			TEXT("the BSP vertex pool exceeds its bound"), Source.SourcePath));
	}
	Model.Vertices.Reserve(Count);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		FWoTWorldVert& Vertex = Model.Vertices.AddDefaulted_GetRef();
		if (!Reader.ReadCompact(Vertex.Point) || !Reader.ReadCompact(Vertex.Side))
		{
			return FResult::Error(WoTWorldError(EWoTBootstrapError::CorruptInstallation,
				TEXT("a BSP vertex record is truncated"), Source.SourcePath));
		}
	}
	int32 SharedSides = 0, ZoneCount = 0;
	if (!Reader.ReadI32(SharedSides) || !Reader.ReadI32(ZoneCount)
		|| SharedSides < 0 || ZoneCount < 0 || ZoneCount > WoTWorldMaxZones)
	{
		return FResult::Error(WoTWorldError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the BSP zone header is invalid"), Source.SourcePath));
	}
	for (int32 Index = 0; Index < ZoneCount; ++Index)
	{
		FWoTWorldZone& Zone = Model.Zones.AddDefaulted_GetRef();
		if (!Reader.ReadCompact(Zone.Actor) || !Reader.ReadU64(Zone.Connectivity)
			|| !Reader.ReadU64(Zone.Visibility)
			|| !WoTWorldReferenceValid(Source, Zone.Actor))
		{
			return FResult::Error(WoTWorldError(EWoTBootstrapError::CorruptInstallation,
				TEXT("a BSP zone record is invalid"), Source.SourcePath));
		}
	}
	int32 LightMapCount = 0;
	if (!Reader.ReadCompact(Model.PolysReference)
		|| Model.PolysReference <= 0
		|| !WoTWorldReferenceValid(Source, Model.PolysReference)
		|| !WoTWorldReadCount(Reader, WoTWorldMaxBSPVertices, LightMapCount))
	{
		return FResult::Error(WoTWorldError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the BSP model tail is invalid"), Source.SourcePath));
	}
	for (int32 Index = 0; Index < LightMapCount; ++Index)
	{
		FWoTWorldLightMap& LightMap = Model.LightMaps.AddDefaulted_GetRef();
		if (!Reader.ReadI32(LightMap.DataOffset) || !Reader.ReadVector(LightMap.Pan)
			|| !Reader.ReadCompact(LightMap.UClamp)
			|| !Reader.ReadCompact(LightMap.VClamp)
			|| !Reader.ReadFloat(LightMap.UScale)
			|| !Reader.ReadFloat(LightMap.VScale)
			|| !Reader.ReadI32(LightMap.LightActors)
			|| LightMap.UClamp < 1 || LightMap.UClamp > 1024
			|| LightMap.VClamp < 1 || LightMap.VClamp > 1024
			|| LightMap.UScale < 0.1f || LightMap.UScale > 65536.0f
			|| LightMap.VScale < 0.1f || LightMap.VScale > 65536.0f)
		{
			return FResult::Error(WoTWorldError(EWoTBootstrapError::CorruptInstallation,
				TEXT("a BSP light-map record is invalid"), Source.SourcePath));
		}
	}
	if (!WoTWorldReadCount(Reader, WoTWorldMaxBSPIndices, Count)
		|| !Reader.ReadBytes(static_cast<uint64>(Count), Model.LightBits))
	{
		return FResult::Error(WoTWorldError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the BSP LightBits array is invalid"), Source.SourcePath));
	}
	auto SkipCompactArray = [&Reader](uint32 Maximum, uint32 ElementBytes)
	{
		int32 ItemCount = 0;
		return WoTWorldReadCount(Reader, Maximum, ItemCount)
			&& Reader.Skip(static_cast<uint64>(ItemCount) * ElementBytes);
	};
	if (!SkipCompactArray(WoTWorldMaxBSPVertices, 25)
		|| !SkipCompactArray(WoTWorldMaxBSPIndices, 4)
		|| !WoTWorldReadCount(Reader, WoTWorldMaxBSPVertices, Count))
	{
		return FResult::Error(WoTWorldError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the BSP light/bounds arrays are invalid"), Source.SourcePath));
	}
	Model.Leaves.Reserve(Count);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		FWoTWorldLeaf& Leaf = Model.Leaves.AddDefaulted_GetRef();
		if (!Reader.ReadCompact(Leaf.Zone) || !Reader.ReadCompact(Leaf.Permeating)
			|| !Reader.ReadCompact(Leaf.Volumetric) || !Reader.Skip(8)
			|| Leaf.Zone < 0 || Leaf.Zone >= WoTWorldMaxZones
			|| Leaf.Permeating < -1
			|| Leaf.Permeating >= static_cast<int32>(WoTWorldMaxReferences)
			|| Leaf.Volumetric < -1
			|| Leaf.Volumetric >= static_cast<int32>(WoTWorldMaxReferences))
		{
			return FResult::Error(WoTWorldError(EWoTBootstrapError::CorruptInstallation,
				TEXT("a BSP leaf record is invalid"),
				Source.SourcePath + TEXT(": ") + FString::Printf(
					TEXT("leaf=%d zone=%d permeating=%d "
					"volumetric=%d"), Index, Leaf.Zone, Leaf.Permeating,
					Leaf.Volumetric)));
		}
	}
	for (const FWoTWorldNode& Node : Model.Nodes)
	{
		for (int32 Leaf : Node.Leaves)
		{
			if (Leaf < INDEX_NONE || (Leaf != INDEX_NONE
				&& !Model.Leaves.IsValidIndex(Leaf)))
			{
				return FResult::Error(WoTWorldError(
					EWoTBootstrapError::CorruptInstallation,
					TEXT("a BSP node leaf index is outside Leaves"),
					Source.SourcePath));
			}
		}
	}
	if (!WoTWorldReadCount(Reader, WoTWorldMaxReferences, Count))
	{
		return FResult::Error(WoTWorldError(EWoTBootstrapError::SizeLimit,
			TEXT("the BSP light registry exceeds its bound"), Source.SourcePath));
	}
	for (int32 Index = 0; Index < Count; ++Index)
	{
		int32& Light = Model.Lights.AddDefaulted_GetRef();
		if (!Reader.ReadCompact(Light) || !WoTWorldReferenceValid(Source, Light))
		{
			return FResult::Error(WoTWorldError(EWoTBootstrapError::CorruptInstallation,
				TEXT("the BSP light registry contains an invalid reference"),
				Source.SourcePath));
		}
	}
	TBitArray<> LightRunTerminates(false, Model.Lights.Num());
	bool bHasSentinel = false;
	for (int32 Index = Model.Lights.Num() - 1; Index >= 0; --Index)
	{
		bHasSentinel = bHasSentinel || Model.Lights[Index] == 0;
		LightRunTerminates[Index] = bHasSentinel;
	}
	for (const FWoTWorldLeaf& Leaf : Model.Leaves)
	{
		for (int32 Run : {Leaf.Permeating, Leaf.Volumetric})
		{
			if (Run < 0) continue;
			if (!Model.Lights.IsValidIndex(Run))
			{
				return FResult::Error(WoTWorldError(
					EWoTBootstrapError::CorruptInstallation,
					TEXT("a BSP leaf light-run offset is outside Lights"),
					Source.SourcePath));
			}
			if (!LightRunTerminates[Run])
			{
				return FResult::Error(WoTWorldError(
					EWoTBootstrapError::CorruptInstallation,
					TEXT("a BSP leaf light run lacks its null sentinel"),
					Source.SourcePath));
			}
		}
	}
	for (int32 Index = 0; Index < Model.LightMaps.Num(); ++Index)
	{
		const FWoTWorldLightMap& LightMap = Model.LightMaps[Index];
		if (LightMap.DataOffset < 0
			|| LightMap.DataOffset > Model.LightBits.Num()
			|| LightMap.LightActors < -1
			|| LightMap.LightActors >= Model.Lights.Num())
		{
			return FResult::Error(WoTWorldError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("a BSP light-map points outside LightBits or Lights"),
				Source.SourcePath));
		}
	}
	int32 RootOutside = 0, Linked = 0;
	if (!Reader.ReadI32(RootOutside) || !Reader.ReadI32(Linked)
		|| (RootOutside != 0 && RootOutside != 1)
		|| (Linked != 0 && Linked != 1) || Reader.Tell() != Reader.GetEnd())
	{
		return FResult::Error(WoTWorldError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the BSP model has a corrupt or trailing tail"), Source.SourcePath));
	}
	Model.bRootOutside = RootOutside != 0;
	for (const int32 Light : Model.Lights)
	{
		if (Light == 0) continue;
		TWoTBootstrapResult<int32> Binding =
			WoTWorldBindSemanticObject(Source, Light, false);
		if (Binding.IsError()) return FResult::Error(MoveTemp(Binding).GetError());
		if (!Source.SemanticObjects[Binding.GetValue()].bActor)
		{
			return FResult::Error(WoTWorldError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("a BSP Model light is not an actor"), Source.SourcePath));
		}
	}
	for (int32 NodeIndex = 0; NodeIndex < Model.Nodes.Num(); ++NodeIndex)
	{
		const FWoTWorldNode& Node = Model.Nodes[NodeIndex];
		if (!Model.Surfaces.IsValidIndex(Node.Surface)
			|| Node.VertexPool < 0
			|| static_cast<uint64>(Node.VertexPool) + Node.VertexCount
				> static_cast<uint64>(Model.Vertices.Num())
			|| (Node.Back != -1 && !Model.Nodes.IsValidIndex(Node.Back))
			|| (Node.Front != -1 && !Model.Nodes.IsValidIndex(Node.Front))
			// Zone indices address UE1's FIXED 64-slot table
			// (UModel::Zones[FBspNode::MAX_ZONES], MAX_ZONES = 64), NOT the
			// count of zones actually serialized. Only NumZones records are
			// written, and a node may legitimately reference a higher slot,
			// which the engine reads as the default/empty zone: retail
			// Arena_01 node 256 references zone 63 while serializing 11.
			// Validating against the serialized count rejected a real map.
			|| Node.Zones[0] >= WoTWorldMaxZones || Node.Zones[1] >= WoTWorldMaxZones)
		{
			const FString Detail = FString::Printf(
				TEXT("node %d surf %d/%d pool %d+%d/%d back %d front %d zones %d,%d/%d"),
				NodeIndex, Node.Surface, Model.Surfaces.Num(),
				Node.VertexPool, Node.VertexCount, Model.Vertices.Num(),
				Node.Back, Node.Front, Node.Zones[0], Node.Zones[1],
				Model.Zones.Num());
			return FResult::Error(WoTWorldError(EWoTBootstrapError::CorruptInstallation,
				*Detail, Source.SourcePath));
		}
	}
	for (const FWoTWorldSurface& Surface : Model.Surfaces)
	{
		if (!Model.Points.IsValidIndex(Surface.Base)
			|| !Model.Vectors.IsValidIndex(Surface.Normal)
			|| !Model.Vectors.IsValidIndex(Surface.TextureU)
			|| !Model.Vectors.IsValidIndex(Surface.TextureV)
			|| !WoTWorldReferenceValid(Source, Surface.Texture)
			|| !WoTWorldReferenceValid(Source, Surface.Actor)
			|| (Surface.LightMap != -1
				&& !Model.LightMaps.IsValidIndex(Surface.LightMap)))
		{
			return FResult::Error(WoTWorldError(EWoTBootstrapError::CorruptInstallation,
				TEXT("a BSP surface points outside a decoded vector"), Source.SourcePath));
		}
		for (int32 Reference : {Surface.Texture, Surface.Actor})
		{
			if (Reference == 0) continue;
			TWoTBootstrapResult<int32> Binding =
				WoTWorldBindSemanticObject(Source, Reference, false);
			if (Binding.IsError())
				return FResult::Error(MoveTemp(Binding).GetError());
			if (Reference == Surface.Actor && Surface.Actor != Surface.Texture
				&& !Source.SemanticObjects[Binding.GetValue()].bActor)
			{
				return FResult::Error(WoTWorldError(
					EWoTBootstrapError::CorruptInstallation,
					TEXT("a BSP surface actor binding has the wrong semantic type"),
					Source.SourcePath));
			}
		}
	}
	for (const FWoTWorldZone& Zone : Model.Zones)
	{
		if (Zone.Actor == 0) continue;
		TWoTBootstrapResult<int32> Binding =
			WoTWorldBindSemanticObject(Source, Zone.Actor, false);
		if (Binding.IsError()) return FResult::Error(MoveTemp(Binding).GetError());
		if (!Source.SemanticObjects[Binding.GetValue()].bActor)
		{
			return FResult::Error(WoTWorldError(EWoTBootstrapError::CorruptInstallation,
				TEXT("a BSP zone binding is not an actor"), Source.SourcePath));
		}
	}
	// Only verts REACHABLE from live nodes may be dereferenced. UE1 keeps the
	// vert records of dead/unused nodes after a BSP rebuild and never reads
	// them, so their point indices are stale garbage. Measured on retail
	// Arena_01: 26571 reachable verts, ALL with valid point indices, versus
	// 56592 unreachable ones of which 8476 dangle. Validating every vert
	// rejected the real map; this still catches genuine corruption because a
	// node can only reach verts through its own pool span, already bounded
	// above.
	TBitArray<> ReachableVerts(false, Model.Vertices.Num());
	for (const FWoTWorldNode& Node : Model.Nodes)
	{
		for (int32 Offset = 0; Offset < Node.VertexCount; ++Offset)
		{
			ReachableVerts[Node.VertexPool + Offset] = true;
		}
	}
	for (int32 VertIndex = 0; VertIndex < Model.Vertices.Num(); ++VertIndex)
	{
		if (!ReachableVerts[VertIndex]) continue;
		const FWoTWorldVert& Vertex = Model.Vertices[VertIndex];
		if (!Model.Points.IsValidIndex(Vertex.Point))
		{
			return FResult::Error(WoTWorldError(EWoTBootstrapError::CorruptInstallation,
				TEXT("a reachable BSP vertex points outside the point vector"),
				Source.SourcePath));
		}
	}
	return FResult::Success(MoveTemp(Model));
}

TWoTBootstrapResult<TArray<FWoTWorldPoly>> WoTWorldDecodePolys(
	const FWoTWorldSource& Source, int32 PolysReference)
{
	using FResult = TWoTBootstrapResult<TArray<FWoTWorldPoly>>;
	if (PolysReference <= 0
		|| !Source.Exports.IsValidIndex(PolysReference - 1))
	{
		return FResult::Error(WoTWorldError(EWoTBootstrapError::CorruptInstallation,
			TEXT("a mover Model has no valid Polys export"), Source.SourcePath));
	}
	const FWoTWorldExport& Export = Source.Exports[PolysReference - 1];
	const FString* Class = Source.ClassName(Export.ClassReference);
	FWoTWorldReader Reader;
	uint64 PropertyCount = 0;
	int32 Count = 0, Maximum = 0;
	if (Class == nullptr || *Class != TEXT("Polys")
		|| !WoTWorldOpenExport(Source, Export, Reader)
		|| !WoTWorldReadProperties(Reader, Source, nullptr, PropertyCount)
		|| !Reader.ReadI32(Count) || !Reader.ReadI32(Maximum)
		|| Count < 0 || Maximum < Count
		|| Count > static_cast<int32>(WoTWorldMaxBSPVertices))
	{
		return FResult::Error(WoTWorldError(EWoTBootstrapError::CorruptInstallation,
			TEXT("a mover Polys database header is malformed"), Source.SourcePath));
	}
	TArray<FWoTWorldPoly> Polys;
	Polys.Reserve(Count);
	for (int32 PolyIndex = 0; PolyIndex < Count; ++PolyIndex)
	{
		FWoTWorldPoly& Poly = Polys.AddDefaulted_GetRef();
		int32 VertexCount = 0;
		FString Item;
		if (!Reader.ReadCompact(VertexCount) || VertexCount < 0 || VertexCount > 16
			|| !Reader.ReadVector(Poly.Base) || !Reader.ReadVector(Poly.Normal)
			|| !Reader.ReadVector(Poly.TextureU) || !Reader.ReadVector(Poly.TextureV))
		{
			return FResult::Error(WoTWorldError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("a mover FPoly header is malformed"), Source.SourcePath));
		}
		Poly.Vertices.Reserve(VertexCount);
		for (int32 VertexIndex = 0; VertexIndex < VertexCount; ++VertexIndex)
		{
			FVector3f& Vertex = Poly.Vertices.AddDefaulted_GetRef();
			if (!Reader.ReadVector(Vertex)
				|| Vertex.GetAbsMax() > WoTWorldMaxCoordinateAbs)
			{
				return FResult::Error(WoTWorldError(
					EWoTBootstrapError::InvalidNumber,
					TEXT("a mover FPoly vertex is malformed"), Source.SourcePath));
			}
		}
		if (!Reader.ReadU32(Poly.Flags) || !Reader.ReadCompact(Poly.Actor)
			|| !Reader.ReadCompact(Poly.Texture)
			|| !WoTWorldReadName(Reader, Source, Item)
			|| !Reader.ReadCompact(Poly.Link) || !Reader.ReadCompact(Poly.BrushPoly)
			|| !Reader.ReadI16(Poly.PanU) || !Reader.ReadI16(Poly.PanV)
			|| !WoTWorldReferenceValid(Source, Poly.Actor)
			|| !WoTWorldReferenceValid(Source, Poly.Texture))
		{
			return FResult::Error(WoTWorldError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("a mover FPoly tail is malformed"), Source.SourcePath));
		}
	}
	if (Reader.Tell() != Reader.GetEnd())
	{
		return FResult::Error(WoTWorldError(EWoTBootstrapError::CorruptInstallation,
			TEXT("a mover Polys export has trailing bytes"), Source.SourcePath));
	}
	return FResult::Success(MoveTemp(Polys));
}

FWoTWorldSemanticValidationMap WoTWorldSemanticValidationView(
	const FWoTWorldSource& Source, const FWoTWorldModel& Model)
{
	FWoTWorldSemanticValidationMap View;
	View.Identity = Source.MapIdentity;
	View.ZoneLimit = WoTWorldMaxZones;
	for (const FWoTWorldSemanticObject& Object : Source.SemanticObjects)
	{
		View.Objects.Add({Object.Identity, Object.ClassIdentity, Object.bActor});
	}
	auto IdentityFor = [&Source](int32 Reference) -> FString
	{
		const int32* Binding = Source.SemanticObjectByReference.Find(Reference);
		return Binding != nullptr
			? Source.SemanticObjects[*Binding].Identity : FString();
	};
	for (const FWoTWorldActor& Actor : Source.Actors)
	{
		const FString Leaf = WoTWorldClassLeaf(Actor.ClassIdentity);
		if (Leaf.Equals(TEXT("Light"), ESearchCase::IgnoreCase))
			View.Lights.Add(Actor.Identity);
		if (WoTWorldIsNavigationClass(Actor.ClassIdentity))
			View.NavigationSeeds.Add(Actor.Identity);
		if (Leaf.Equals(TEXT("SkyZoneInfo"), ESearchCase::IgnoreCase))
			View.SkyZone = Actor.Identity;
		if (Actor.ParentActor.IsSet() && Actor.ParentActor.GetValue() != 0)
		{
			View.Attachments.Add({
				Actor.Identity, IdentityFor(Actor.ParentActor.GetValue())});
		}
		for (const FWoTTypedProperty& Property : Actor.Properties)
		{
			auto AddReferences = [&View, &Actor](const FWoTTypedProperty& Root,
				auto&& AddReferencesRef) -> void
			{
				if (!Root.ObjectIdentity.IsEmpty())
					View.References.Add({Actor.Identity, Root.ObjectIdentity});
				for (const FWoTTypedProperty& Member : Root.StructMembers)
					AddReferencesRef(Member, AddReferencesRef);
			};
			AddReferences(Property, AddReferences);
			if (Leaf.Equals(TEXT("AmbientSound"), ESearchCase::IgnoreCase)
				&& Property.Name.Contains(TEXT("Sound"))
				&& !Property.ObjectIdentity.IsEmpty()
				&& WoTWorldClassLeaf(Property.ObjectClassIdentity).Equals(
					TEXT("Sound"), ESearchCase::IgnoreCase))
			{
				View.Sounds.Add({Actor.Identity, Property.ObjectIdentity});
			}
		}
	}
	for (const FWoTWorldSurface& Surface : Model.Surfaces)
		View.SurfaceFlags.Add(Surface.Flags);
	for (const FWoTWorldNode& Node : Model.Nodes)
	{
		FWoTWorldSemanticValidationNode& NodeView =
			View.Nodes.AddDefaulted_GetRef();
		NodeView.Surface = Node.Surface;
		NodeView.Back = Node.Back;
		NodeView.Front = Node.Front;
		NodeView.Zones[0] = Node.Zones[0];
		NodeView.Zones[1] = Node.Zones[1];
		for (int32 Offset = 0; Offset < Node.VertexCount; ++Offset)
			NodeView.VertexIndices.Add(
				Model.Vertices[Node.VertexPool + Offset].Point);
		View.CollisionSurfaces.Add(Node.Surface);
		if ((Model.Surfaces[Node.Surface].Flags & WoTWorldPFPortal) != 0)
		{
			FWoTWorldSemanticValidationPortal& Portal =
				View.Portals.AddDefaulted_GetRef();
			Portal.Surface = Node.Surface;
			Portal.Zones[0] = Node.Zones[0];
			Portal.Zones[1] = Node.Zones[1];
		}
	}
	for (const FWoTWorldZone& Zone : Model.Zones)
		View.ZoneActors.Add(IdentityFor(Zone.Actor));
	return View;
}

void WoTWorldAppendU16(TArray<uint8>& Bytes, uint16 Value)
{
	Bytes.Add(static_cast<uint8>(Value));
	Bytes.Add(static_cast<uint8>(Value >> 8));
}

void WoTWorldAppendU32(TArray<uint8>& Bytes, uint32 Value)
{
	Bytes.Add(static_cast<uint8>(Value));
	Bytes.Add(static_cast<uint8>(Value >> 8));
	Bytes.Add(static_cast<uint8>(Value >> 16));
	Bytes.Add(static_cast<uint8>(Value >> 24));
}

void WoTWorldAppendI32(TArray<uint8>& Bytes, int32 Value)
{
	WoTWorldAppendU32(Bytes, static_cast<uint32>(Value));
}

void WoTWorldAppendU64(TArray<uint8>& Bytes, uint64 Value)
{
	WoTWorldAppendU32(Bytes, static_cast<uint32>(Value));
	WoTWorldAppendU32(Bytes, static_cast<uint32>(Value >> 32));
}

uint32 WoTWorldFloatBits(float Value)
{
	uint32 Bits = 0;
	FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
	return Bits == 0x80000000u ? 0u : Bits;
}

void WoTWorldAppendFloat(TArray<uint8>& Bytes, float Value)
{
	WoTWorldAppendU32(Bytes, WoTWorldFloatBits(Value));
}

bool WoTWorldAppendString(TArray<uint8>& Bytes, FStringView Value)
{
	FTCHARToUTF8 Encoded(Value.GetData(), Value.Len());
	if (Encoded.Length() <= 0 || Encoded.Length() > static_cast<int32>(WoTWorldMaxNameBytes))
	{
		return false;
	}
	WoTWorldAppendU32(Bytes, static_cast<uint32>(Encoded.Length()));
	Bytes.Append(reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length());
	return true;
}

bool WoTWorldAppendOptionalString(TArray<uint8>& Bytes, FStringView Value)
{
	if (Value.IsEmpty())
	{
		WoTWorldAppendU32(Bytes, 0);
		return true;
	}
	return WoTWorldAppendString(Bytes, Value);
}

TWoTBootstrapResult<TArray<uint8>> WoTWorldFrameObject(
	const FWoTCanonicalJsonValue& Header, TConstArrayView<uint8> Payload)
{
	using FResult = TWoTBootstrapResult<TArray<uint8>>;
	TWoTBootstrapResult<TArray<uint8>> HeaderBytes = WriteCanonicalJson(Header);
	if (HeaderBytes.IsError()) return FResult::Error(MoveTemp(HeaderBytes).GetError());
	if (HeaderBytes.GetValue().IsEmpty() || HeaderBytes.GetValue().Num() > 1024 * 1024
		|| Payload.Num() > static_cast<int32>(WoTWorldMaxSourceBytes)
		|| HeaderBytes.GetValue().Num() > MAX_int32 - Payload.Num() - 12)
	{
		return FResult::Error(WoTWorldError(EWoTBootstrapError::SizeLimit,
			TEXT("a world typed object exceeds its framing bound")));
	}
	TArray<uint8> Object;
	Object.Reserve(12 + HeaderBytes.GetValue().Num() + Payload.Num());
	Object.Append(reinterpret_cast<const uint8*>(WoTWorldObjectMagic), 8);
	WoTWorldAppendU32(Object, static_cast<uint32>(HeaderBytes.GetValue().Num()));
	Object.Append(HeaderBytes.GetValue());
	Object.Append(Payload.GetData(), Payload.Num());
	return FResult::Success(MoveTemp(Object));
}

TWoTBootstrapResult<FWoTCanonicalJsonValue> WoTWorldSourceJson(
	const FWoTRequiredFact& Fact, const FWoTWorldSource& Source)
{
	FWoTCanonicalObjectBuilder Builder;
	Builder.Add(TEXT("export"), FWoTCanonicalJsonValue::MakeString(Fact.SourceExport));
	Builder.Add(TEXT("package"), FWoTCanonicalJsonValue::MakeString(Fact.SourcePath));
	Builder.Add(TEXT("sha256"), FWoTCanonicalJsonValue::MakeString(Source.Sha256.ToHex()));
	return MoveTemp(Builder).Build(TEXT("world source"));
}

const WoT::Retail::Generated::FWorldTexture* WoTWorldTexture(
	const FWoTWorldSource& Source, int32 Reference)
{
	const WoT::Retail::Generated::FWorldTexture* Texture = nullptr;
	if (Reference != 0 && WoTWorldReferenceValid(Source, Reference))
	{
		FString SourcePackage;
		FString SourceName;
		FString SourceRelative;
		if (Reference > 0)
		{
			SourcePackage = Source.MapIdentity;
			SourceName = Source.Exports[Reference - 1].Name;
		}
		else
		{
			const FWoTWorldImport& Import = Source.Imports[-Reference - 1];
			SourceName = Import.Name;
			int32 Outer = Import.Outer;
			SourcePackage = Source.MapIdentity;
			for (uint32 Depth = 0;
				Outer != 0 && Depth < WoTWorldMaxOuterDepth; ++Depth)
			{
				// Package.objref treats a positive outer as an object owned by
				// this map package, even after one or more imported group outers.
				if (Outer > 0)
				{
					SourcePackage = Source.MapIdentity;
					break;
				}
				const FWoTWorldImport& Parent = Source.Imports[-Outer - 1];
				SourcePackage = Parent.Name;
				Outer = Parent.Outer;
			}
		}
		TWoTBootstrapResult<FString> FullPath =
			WoTWorldObjectPath(Source, Reference);
		if (!FullPath.IsError())
		{
			SourceRelative = FullPath.GetValue();
			const FString Prefix = SourcePackage + TEXT(".");
			if (SourceRelative.StartsWith(Prefix, ESearchCase::IgnoreCase))
				SourceRelative.RightChopInline(Prefix.Len());
		}
		if (SourceRelative.IsEmpty()) SourceRelative = SourceName;
		const FString FoldedPackage = WoTWorldFold(SourcePackage);
		const FString FoldedRelative = WoTWorldFold(SourceRelative);
		auto FindTexture = [&FoldedPackage](FStringView Lookup)
			-> const WoT::Retail::Generated::FWorldTexture*
		{
			for (const WoT::Retail::Generated::FWorldTexture& Candidate
				: WoT::Retail::Generated::WorldTextures)
			{
				if (FoldedPackage == Candidate.SourcePackageFolded
					&& Lookup == FStringView(Candidate.SourceRelativeFolded))
				{
					return &Candidate;
				}
			}
			return nullptr;
		};
		// The generator applies bsp2obj.resolve_tex's group-qualified-relative
		// then leaf-name fallback and records its result under this exact source
		// identity. Native therefore performs one collision-free lookup rather
		// than reimplementing the transitional manifest tie-break at runtime.
		Texture = FindTexture(FoldedRelative);
	}
	return Texture;
}

bool WoTWorldSectionIdentityLengthsValid(TConstArrayView<int32> Lengths)
{
	if (Lengths.Num() != 4) return false;
	for (const int32 ByteCount : Lengths)
	{
		if (ByteCount < 0 || ByteCount > WoTWorldMaxSectionIdentityBytes)
			return false;
	}
	return true;
}

float WoTWorldTextureFriction(const FWoTWorldSource& Source, int32 Reference)
{
	const WoT::Retail::Generated::FWorldTexture* Texture =
		WoTWorldTexture(Source, Reference);
	return Texture == nullptr ? 1.0f : static_cast<float>(Texture->Friction);
}

FString WoTWorldMaterialSlot(const FWoTWorldSource& Source,
	const FWoTWorldSurface& Surface)
{
	const WoT::Retail::Generated::FWorldTexture* Texture =
		WoTWorldTexture(Source, Surface.Texture);
	FString Slot = Texture != nullptr
		? FString(Texture->CanonicalPackage) + TEXT("__") + Texture->CanonicalName
		: TEXT("__None");
	if ((Surface.Flags & WoTWorldPFMasked) != 0) Slot += TEXT("_M");
	if ((Surface.Flags & WoTWorldPFTranslucent) != 0) Slot += TEXT("_T");
	if ((Surface.Flags & WoTWorldPFModulated) != 0) Slot += TEXT("_Mod");
	if ((Surface.Flags & WoTWorldPFTwoSided) != 0) Slot += TEXT("_2S");
	if ((Surface.Flags & WoTWorldPFUnlit) != 0) Slot += TEXT("_U");
	if ((Surface.Flags & WoTWorldPFFakeBackdrop) != 0) Slot += TEXT("_Sky");
	if ((Surface.Flags & WoTWorldPFSpecialLit) != 0) Slot += TEXT("_SL");
	return Slot;
}

struct FWoTWorldTriangle
{
	FVector3f Positions[3];
	FVector3f Normal = FVector3f::ZeroVector;
	FVector2f UV0[3];
	FVector2f UV1[3];
	uint32 SurfaceIndex = 0;
};

struct FWoTWorldMoverSection
{
	FString MaterialSlot;
	FString CanonicalPackage;
	FString CanonicalName;
	FString SourceTextureIdentity;
	FString MacroCanonicalPackage;
	FString MacroCanonicalName;
	FString MacroSourceTextureIdentity;
	float MacroRatio = 0.0f;
	uint32 SurfaceFlags = 0;
	FVector3f PlaneOrigin = FVector3f::ZeroVector;
	FVector3f PlaneNormal = FVector3f::ZAxisVector;
	uint32 FirstIndex = 0;
	uint32 IndexCount = 0;
	uint32 MinVertex = 0;
	uint32 MaxVertex = 0;
	bool bAnimatedLightmap = false;
	// Cache-build provenance only. Sections may merge several FPolys with the
	// same material/plane; any animated member selects the response master and
	// black response texels leave its static neighbours unchanged.
	TArray<int32> PolyIndices;
};

struct FWoTWorldMoverModel
{
	FString ActorIdentity;
	FString ModelIdentity;
	bool bLightmapped = false;
	TArray<FWoTWorldMoverSection> Sections;
	TArray<FVector3f> Positions;
	TArray<FVector3f> Normals;
	TArray<FVector2f> UV0;
	TArray<FVector2f> UV1;
	TArray<uint32> Indices;
	TArray<FVector3f> CollisionPositions;
	TArray<uint32> CollisionIndices;
	// Cache-build provenance. These fields never cross the wire.
	FWoTWorldModel BrushModel;
	TArray<FWoTWorldPoly> Polys;
	TArray<int32> VertexPolys;
	TArray<FVector3f> UV1SourcePositions;
	int32 ActorIndex = INDEX_NONE;
	int32 PolyAtlasOffset = INDEX_NONE;
	uint32 LitPolyCount = 0;
	uint32 UV1OutsidePaddedRectCount = 0;
	uint32 PolyOwnershipViolationCount = 0;
	uint32 AnimatedLightPolyCount = 0;
};

double WoTWorldOrderedDot(const FVector3d& Delta, const FVector3f& Axis)
{
	// Accumulate binary64 products in stable source-axis order. This is the
	// destination arithmetic contract: X, then Y, then Z, retaining binary64
	// until the converter's float32-metres serialization boundary.
	double Result = Delta.X * static_cast<double>(Axis.X);
	Result += Delta.Y * static_cast<double>(Axis.Y);
	Result += Delta.Z * static_cast<double>(Axis.Z);
	return Result;
}

double WoTWorldOrderedDot(const FVector3d& Delta, const FVector3d& Axis)
{
	double Result = Delta.X * Axis.X;
	Result += Delta.Y * Axis.Y;
	Result += Delta.Z * Axis.Z;
	return Result;
}

float WoTWorldFloat(double Value)
{
	return static_cast<float>(Value);
}

float WoTWorldPosition(double Value)
{
	// Preserve the established static-BSP converter boundary: GLB POSITION is
	// float32 metres, then the importer restores UE1 centimetres.
	const double Metres = Value / 100.0;
	volatile float GLBValue = static_cast<float>(Metres);
	const double Centimetres = static_cast<double>(GLBValue) * 100.0;
	return WoTWorldFloat(Centimetres);
}

float WoTWorldMoverPosition(double Value)
{
	// Mover projection stays binary64 until bsp2obj's real serialization
	// boundary, then follows the same float32-metres/imported-centimetres path.
	const double Metres = Value / 100.0;
	volatile float GLBValue = static_cast<float>(Metres);
	const double Centimetres = static_cast<double>(GLBValue) * 100.0;
	return WoTWorldFloat(Centimetres);
}

int64 WoTWorldRoundTiesToEvenInt64(double Value)
{
	const double Lower = FMath::FloorToDouble(Value);
	const double Fraction = Value - Lower;
	if (Fraction < 0.5) return static_cast<int64>(Lower);
	if (Fraction > 0.5) return static_cast<int64>(Lower + 1.0);
	const int64 LowerInteger = static_cast<int64>(Lower);
	return (LowerInteger & 1ll) == 0 ? LowerInteger : LowerInteger + 1;
}

double WoTWorldTriangleAreaSquared(
	const FVector3f& A, const FVector3f& B, const FVector3f& C)
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
}

TWoTBootstrapResult<FWoTWorldModel> WoTWorldDecodeMoverModelPrefix(
	const FWoTWorldSource& Source, int32 ModelReference)
{
	using FResult = TWoTBootstrapResult<FWoTWorldModel>;
	if (ModelReference <= 0 || !Source.Exports.IsValidIndex(ModelReference - 1))
		return FResult::Error(WoTWorldError(EWoTBootstrapError::CorruptInstallation,
			TEXT("a mover Brush reference is not an export"), Source.SourcePath));
	const FWoTWorldExport& Export = Source.Exports[ModelReference - 1];
	const FString* Class = Source.ClassName(Export.ClassReference);
	FWoTWorldReader Reader;
	uint64 PropertyCount = 0;
	int32 Count = 0;
	FWoTWorldModel Model;
	if (Class == nullptr || *Class != TEXT("Model")
		|| !WoTWorldOpenExport(Source, Export, Reader)
		|| !WoTWorldReadProperties(Reader, Source, nullptr, PropertyCount)
		|| !Reader.Skip(25 + 16)
		|| !WoTWorldReadCount(Reader, WoTWorldMaxBSPVertices, Count))
	{
		return FResult::Error(WoTWorldError(EWoTBootstrapError::CorruptInstallation,
			TEXT("a mover Model prefix is malformed"), Source.SourcePath));
	}
	for (int32 Index = 0; Index < Count; ++Index)
	{
		FVector3f& Vector = Model.Vectors.AddDefaulted_GetRef();
		if (!Reader.ReadVector(Vector))
			return FResult::Error(WoTWorldError(EWoTBootstrapError::InvalidNumber,
				TEXT("a mover Model vector is malformed"), Source.SourcePath));
	}
	if (!WoTWorldReadCount(Reader, WoTWorldMaxBSPVertices, Count))
		return FResult::Error(WoTWorldError(EWoTBootstrapError::SizeLimit,
			TEXT("a mover Model point count exceeds its bound"), Source.SourcePath));
	for (int32 Index = 0; Index < Count; ++Index)
	{
		FVector3f& Point = Model.Points.AddDefaulted_GetRef();
		if (!Reader.ReadVector(Point) || Point.GetAbsMax() > WoTWorldMaxCoordinateAbs)
			return FResult::Error(WoTWorldError(EWoTBootstrapError::InvalidNumber,
				TEXT("a mover Model point is malformed"), Source.SourcePath));
	}
	if (!WoTWorldReadCount(Reader, WoTWorldMaxBSPVertices, Count))
		return FResult::Error(WoTWorldError(EWoTBootstrapError::SizeLimit,
			TEXT("a mover Model node count exceeds its bound"), Source.SourcePath));
	for (int32 Index = 0; Index < Count; ++Index)
	{
		FWoTWorldNode& Node = Model.Nodes.AddDefaulted_GetRef();
		if (!Reader.ReadFloat(Node.Plane[0]) || !Reader.ReadFloat(Node.Plane[1])
			|| !Reader.ReadFloat(Node.Plane[2]) || !Reader.ReadFloat(Node.Plane[3])
			|| !Reader.ReadU64(Node.ZoneMask) || !Reader.ReadU8(Node.Flags)
			|| !Reader.ReadCompact(Node.VertexPool) || !Reader.ReadCompact(Node.Surface)
			|| !Reader.ReadCompact(Node.Back) || !Reader.ReadCompact(Node.Front)
			|| !Reader.ReadCompact(Node.PlaneNode)
			|| !Reader.ReadCompact(Node.CollisionBound)
			|| !Reader.ReadCompact(Node.RenderBound)
			|| !Reader.ReadU8(Node.Zones[0]) || !Reader.ReadU8(Node.Zones[1])
			|| !Reader.ReadU8(Node.VertexCount)
			|| !Reader.ReadI32(Node.Leaves[0]) || !Reader.ReadI32(Node.Leaves[1]))
			return FResult::Error(WoTWorldError(EWoTBootstrapError::CorruptInstallation,
				TEXT("a mover Model node is malformed"), Source.SourcePath));
	}
	if (!WoTWorldReadCount(Reader, WoTWorldMaxBSPVertices, Count))
		return FResult::Error(WoTWorldError(EWoTBootstrapError::SizeLimit,
			TEXT("a mover Model surface count exceeds its bound"), Source.SourcePath));
	for (int32 Index = 0; Index < Count; ++Index)
	{
		FWoTWorldSurface& Surface = Model.Surfaces.AddDefaulted_GetRef();
		if (!Reader.ReadCompact(Surface.Texture) || !Reader.ReadU32(Surface.Flags)
			|| !Reader.ReadCompact(Surface.Base) || !Reader.ReadCompact(Surface.Normal)
			|| !Reader.ReadCompact(Surface.TextureU) || !Reader.ReadCompact(Surface.TextureV)
			|| !Reader.ReadCompact(Surface.LightMap) || !Reader.ReadCompact(Surface.BrushPoly)
			|| !Reader.ReadI16(Surface.PanU) || !Reader.ReadI16(Surface.PanV)
			|| !Reader.ReadCompact(Surface.Actor))
			return FResult::Error(WoTWorldError(EWoTBootstrapError::CorruptInstallation,
				TEXT("a mover Model surface is malformed"), Source.SourcePath));
	}
	if (!WoTWorldReadCount(Reader, WoTWorldMaxBSPIndices, Count))
		return FResult::Error(WoTWorldError(EWoTBootstrapError::SizeLimit,
			TEXT("a mover Model vertex count exceeds its bound"), Source.SourcePath));
	for (int32 Index = 0; Index < Count; ++Index)
	{
		FWoTWorldVert& Vertex = Model.Vertices.AddDefaulted_GetRef();
		if (!Reader.ReadCompact(Vertex.Point) || !Reader.ReadCompact(Vertex.Side))
			return FResult::Error(WoTWorldError(EWoTBootstrapError::CorruptInstallation,
				TEXT("a mover Model vertex is malformed"), Source.SourcePath));
	}
	for (const FWoTWorldNode& Node : Model.Nodes)
	{
		if (!Model.Surfaces.IsValidIndex(Node.Surface) || Node.VertexPool < 0
			|| static_cast<uint64>(Node.VertexPool) + Node.VertexCount
				> static_cast<uint64>(Model.Vertices.Num()))
			return FResult::Error(WoTWorldError(EWoTBootstrapError::CorruptInstallation,
				TEXT("a mover Model node range is invalid"), Source.SourcePath));
		const FWoTWorldSurface& Surface = Model.Surfaces[Node.Surface];
		if (!Model.Points.IsValidIndex(Surface.Base)
			|| !Model.Vectors.IsValidIndex(Surface.Normal)
			|| !Model.Vectors.IsValidIndex(Surface.TextureU)
			|| !Model.Vectors.IsValidIndex(Surface.TextureV)
			|| !WoTWorldReferenceValid(Source, Surface.Texture))
			return FResult::Error(WoTWorldError(EWoTBootstrapError::CorruptInstallation,
				TEXT("a mover Model surface range is invalid"), Source.SourcePath));
		for (int32 Offset = 0; Offset < Node.VertexCount; ++Offset)
			if (!Model.Points.IsValidIndex(Model.Vertices[Node.VertexPool + Offset].Point))
				return FResult::Error(WoTWorldError(
					EWoTBootstrapError::CorruptInstallation,
					TEXT("a mover Model vertex points outside its point table"),
					Source.SourcePath));
	}
	return FResult::Success(MoveTemp(Model));
}

TWoTBootstrapResult<TArray<FWoTWorldMoverModel>> WoTWorldBuildMoverModels(
	FWoTWorldSource& Source)
{
	using FResult = TWoTBootstrapResult<TArray<FWoTWorldMoverModel>>;
	struct FTriangle
	{
		FVector3f Positions[3];
		FVector3f Normal = FVector3f::ZAxisVector;
		FVector2f UV0[3];
		int32 PolyIndex = INDEX_NONE;
	};
	struct FBucket
	{
		FString Material;
		int64 Plane[4] = {};
		FVector3d Origin = FVector3d::ZeroVector;
		FVector3d Normal = FVector3d::ZAxisVector;
		TArray<FTriangle> Triangles;
	};
	struct FMaterialIdentity
	{
		FString Package;
		FString Name;
		FString SourceIdentity;
		FString MacroPackage;
		FString MacroName;
		FString MacroSourceIdentity;
		float MacroRatio = 0.0f;
		uint32 Flags = 0;
	};
	TArray<FWoTWorldMoverModel> Result;
	TSet<int32> SeenModels;
	for (int32 ActorIndex = 0; ActorIndex < Source.Actors.Num(); ++ActorIndex)
	{
		const FWoTWorldActor& Actor = Source.Actors[ActorIndex];
		if (!WoTWorldIsMoverClass(Actor.ClassIdentity)) continue;
		const FWoTTypedProperty* Brush = Actor.Properties.FindByPredicate(
			[](const FWoTTypedProperty& Property)
			{
				return Property.Name.Equals(TEXT("Brush"), ESearchCase::IgnoreCase)
					&& Property.Type == 5 && Property.ObjectReference.IsSet()
					&& Property.ObjectClassIdentity.Equals(
						TEXT("Engine.Model"), ESearchCase::IgnoreCase);
			});
		if (Brush == nullptr || Brush->ObjectReference.GetValue() <= 0) continue;
		const int32 ModelReference = Brush->ObjectReference.GetValue();
		if (SeenModels.Contains(ModelReference))
			return FResult::Error(WoTWorldError(EWoTBootstrapError::DuplicateField,
				TEXT("two mover actors share one Brush Model"), Actor.Identity));
		SeenModels.Add(ModelReference);
		TWoTBootstrapResult<FWoTWorldModel> Decoded =
			WoTWorldDecodeModel(Source, ModelReference);
		if (Decoded.IsError()) return FResult::Error(MoveTemp(Decoded).GetError());
		FWoTWorldModel Model = MoveTemp(Decoded).GetValue();
		TWoTBootstrapResult<TArray<FWoTWorldPoly>> DecodedPolys =
			WoTWorldDecodePolys(Source, Model.PolysReference);
		if (DecodedPolys.IsError())
			return FResult::Error(MoveTemp(DecodedPolys).GetError());
		TArray<FWoTWorldPoly> Polys = MoveTemp(DecodedPolys).GetValue();
		TArray<FBucket> Buckets;
		TMap<FString, FMaterialIdentity> MaterialIdentities;
		for (int32 PolyIndex = 0; PolyIndex < Polys.Num(); ++PolyIndex)
		{
			const FWoTWorldPoly& Poly = Polys[PolyIndex];
			if (Poly.Vertices.Num() < 3
				|| (Poly.Flags & (WoTWorldPFInvisible | WoTWorldPFPortal)) != 0)
				continue;
			const WoT::Retail::Generated::FWorldTexture* Texture =
				WoTWorldTexture(Source, Poly.Texture);
			if (Poly.Texture != 0 && Texture == nullptr)
				return FResult::Error(WoTWorldError(EWoTBootstrapError::CorruptInstallation,
					TEXT("a mover texture is absent from the canonical registry"),
					Actor.Identity));
			FWoTWorldSurface PolySurface;
			PolySurface.Texture = Poly.Texture;
			PolySurface.Flags = Poly.Flags;
			const FString Material = WoTWorldMaterialSlot(Source, PolySurface);
			const FString CanonicalPackage = Texture != nullptr
				? FString(Texture->CanonicalPackage) : FString();
			const FString CanonicalName = Texture != nullptr
				? FString(Texture->CanonicalName) : TEXT("None");
			FString SourceIdentity;
			if (Poly.Texture == 0) SourceIdentity = TEXT("None");
			else
			{
				TWoTBootstrapResult<FString> ResolvedSourceIdentity =
					WoTWorldObjectPath(Source, Poly.Texture);
				if (ResolvedSourceIdentity.IsError())
					return FResult::Error(
						MoveTemp(ResolvedSourceIdentity).GetError());
				SourceIdentity = MoveTemp(ResolvedSourceIdentity).GetValue();
			}
			const FString MacroPackage = Texture != nullptr
				? FString(Texture->MacroCanonicalPackage) : FString();
			const FString MacroName = Texture != nullptr
				? FString(Texture->MacroCanonicalName) : FString();
			const FString MacroSourceIdentity = Texture != nullptr
				? FString(Texture->MacroSourceTextureIdentity) : FString();
			const float MacroRatio = Texture != nullptr && Texture->MacroWidth != 0
				&& Texture->MacroDrawScale > 0.0
				? static_cast<float>((static_cast<double>(Texture->Width)
					* Texture->DrawScale) / (static_cast<double>(Texture->MacroWidth)
						* Texture->MacroDrawScale)) : 0.0f;
			FMaterialIdentity& Identity = MaterialIdentities.FindOrAdd(Material);
			if (!Identity.SourceIdentity.IsEmpty()
				&& (Identity.Package != CanonicalPackage
					|| Identity.Name != CanonicalName
					|| Identity.SourceIdentity != SourceIdentity
					|| Identity.MacroPackage != MacroPackage
					|| Identity.MacroName != MacroName
					|| Identity.MacroSourceIdentity != MacroSourceIdentity
					|| Identity.MacroRatio != MacroRatio))
			{
				return FResult::Error(WoTWorldError(
					EWoTBootstrapError::DuplicateField,
					TEXT("a mover material slot aliases more than one texture identity"),
					Actor.Identity));
			}
			Identity.Package = CanonicalPackage;
			Identity.Name = CanonicalName;
			Identity.SourceIdentity = MoveTemp(SourceIdentity);
			Identity.MacroPackage = MacroPackage;
			Identity.MacroName = MacroName;
			Identity.MacroSourceIdentity = MacroSourceIdentity;
			Identity.MacroRatio = MacroRatio;
			Identity.Flags = Poly.Flags;
			const FVector3f SourceOrigin = Poly.Base;
			const FVector3f SurfaceNormal = Poly.Normal;
			const FVector3d Origin(SourceOrigin.X, SourceOrigin.Y, SourceOrigin.Z);
			const FVector3d SourceNormal(
				SurfaceNormal.X, SurfaceNormal.Y, SurfaceNormal.Z);
			const double NormalLength = FMath::Sqrt(
				SourceNormal.X * SourceNormal.X + SourceNormal.Y * SourceNormal.Y
				+ SourceNormal.Z * SourceNormal.Z);
			if (!FMath::IsFinite(NormalLength) || NormalLength <= 0.0)
				return FResult::Error(WoTWorldError(EWoTBootstrapError::InvalidNumber,
					TEXT("a mover surface normal is zero"), Actor.Identity));
			const FVector3d Normal = SourceNormal / NormalLength;
			const double Distance = WoTWorldOrderedDot(Origin, Normal);
			const int64 Plane[4] = {
				WoTWorldRoundTiesToEvenInt64(Normal.X * 10000000.0),
				WoTWorldRoundTiesToEvenInt64(Normal.Y * 10000000.0),
				WoTWorldRoundTiesToEvenInt64(Normal.Z * 10000000.0),
				WoTWorldRoundTiesToEvenInt64(Distance * 10000000.0)};
			FBucket* Bucket = Buckets.FindByPredicate(
				[&Material, &Plane](const FBucket& Candidate)
				{
					return Candidate.Material == Material
						&& Candidate.Plane[0] == Plane[0]
						&& Candidate.Plane[1] == Plane[1]
						&& Candidate.Plane[2] == Plane[2]
						&& Candidate.Plane[3] == Plane[3];
				});
			if (Bucket == nullptr)
			{
				FBucket& Added = Buckets.AddDefaulted_GetRef();
				Added.Material = Material;
				for (int32 Axis = 0; Axis < 4; ++Axis) Added.Plane[Axis] = Plane[Axis];
				Added.Origin = Origin;
				Added.Normal = Normal;
				Bucket = &Added;
			}
			const FVector3f& Base = Poly.Base;
			const FVector3f& TU = Poly.TextureU;
			const FVector3f& TV = Poly.TextureV;
			const double Width = Texture != nullptr
				? static_cast<double>(Texture->Width) * Texture->DrawScale : 1.0;
			const double Height = Texture != nullptr
				? static_cast<double>(Texture->Height) * Texture->DrawScale : 1.0;
			TArray<FVector3f> Polygon = Poly.Vertices;
			TArray<FVector2f> UV;
			UV.Reserve(Polygon.Num());
			for (const FVector3f& Point : Polygon)
			{
				const FVector3d Delta(
					static_cast<double>(Point.X) - Base.X,
					static_cast<double>(Point.Y) - Base.Y,
					static_cast<double>(Point.Z) - Base.Z);
				UV.Add(FVector2f(
					WoTWorldFloat((WoTWorldOrderedDot(Delta, TU) + Poly.PanU) / Width),
					WoTWorldFloat((WoTWorldOrderedDot(Delta, TV) + Poly.PanV) / Height)));
			}
			FVector3d Newell = FVector3d::ZeroVector;
			for (int32 Vertex = 0; Vertex < Polygon.Num(); ++Vertex)
			{
				const FVector3f& A = Polygon[Vertex];
				const FVector3f& B = Polygon[(Vertex + 1) % Polygon.Num()];
				Newell.X += (static_cast<double>(A.Y) - B.Y) * (A.Z + B.Z);
				Newell.Y += (static_cast<double>(A.Z) - B.Z) * (A.X + B.X);
				Newell.Z += (static_cast<double>(A.X) - B.X) * (A.Y + B.Y);
			}
			if (WoTWorldOrderedDot(Newell, SurfaceNormal) < 0.0)
			{
				Algo::Reverse(Polygon); Algo::Reverse(UV);
			}
			for (int32 Vertex = 1; Vertex + 1 < Polygon.Num(); ++Vertex)
			{
				if (WoTWorldTriangleAreaSquared(
					Polygon[0], Polygon[Vertex], Polygon[Vertex + 1]) < 0.001) continue;
				FTriangle& Triangle = Bucket->Triangles.AddDefaulted_GetRef();
				Triangle.Normal = FVector3f(Normal);
				Triangle.PolyIndex = PolyIndex;
				const int32 Order[3] = {Vertex + 1, Vertex, 0};
				for (int32 Corner = 0; Corner < 3; ++Corner)
				{
					Triangle.Positions[Corner] = Polygon[Order[Corner]];
					Triangle.UV0[Corner] = UV[Order[Corner]];
				}
			}
		}
		TArray<FVector3f> CollisionPositions;
		TArray<uint32> CollisionIndices;
		for (const FWoTWorldNode& Node : Model.Nodes)
		{
			if (Node.VertexCount < 3) continue;
			const FWoTWorldSurface& Surface = Model.Surfaces[Node.Surface];
			if ((Surface.Flags & (WoTWorldPFPortal | WoTWorldPFNotSolid)) != 0)
				continue;
			const FVector3f& SurfaceNormal = Model.Vectors[Surface.Normal];
			const double CollisionNormalLength = FVector3d(SurfaceNormal).Length();
			if (!FMath::IsFinite(CollisionNormalLength)
				|| CollisionNormalLength <= 0.0)
			{
				return FResult::Error(WoTWorldError(
					EWoTBootstrapError::InvalidNumber,
					TEXT("a mover collision surface normal is zero"),
					Actor.Identity));
			}
			TArray<FVector3f> Polygon;
			Polygon.Reserve(Node.VertexCount);
			for (int32 Vertex = 0; Vertex < Node.VertexCount; ++Vertex)
			{
				Polygon.Add(Model.Points[
					Model.Vertices[Node.VertexPool + Vertex].Point]);
			}
			FVector3d Newell = FVector3d::ZeroVector;
			for (int32 Vertex = 0; Vertex < Polygon.Num(); ++Vertex)
			{
				const FVector3f& A = Polygon[Vertex];
				const FVector3f& B = Polygon[(Vertex + 1) % Polygon.Num()];
				Newell.X += (static_cast<double>(A.Y) - B.Y) * (A.Z + B.Z);
				Newell.Y += (static_cast<double>(A.Z) - B.Z) * (A.X + B.X);
				Newell.Z += (static_cast<double>(A.X) - B.X) * (A.Y + B.Y);
			}
			if (WoTWorldOrderedDot(Newell, SurfaceNormal) < 0.0)
				Algo::Reverse(Polygon);
			for (int32 Vertex = 1; Vertex + 1 < Polygon.Num(); ++Vertex)
			{
				if (WoTWorldTriangleAreaSquared(
					Polygon[0], Polygon[Vertex], Polygon[Vertex + 1]) < 0.001) continue;
				const int32 Order[3] = {Vertex + 1, Vertex, 0};
				for (int32 Corner = 0; Corner < 3; ++Corner)
				{
					const FVector3f& Position = Polygon[Order[Corner]];
					CollisionPositions.Add(FVector3f(
						WoTWorldMoverPosition(Position.X),
						WoTWorldMoverPosition(Position.Y),
						WoTWorldMoverPosition(Position.Z)));
					CollisionIndices.Add(CollisionIndices.Num());
				}
			}
		}
		for (int32 Index = Buckets.Num() - 1; Index >= 0; --Index)
			if (Buckets[Index].Triangles.IsEmpty()) Buckets.RemoveAt(Index);
		Buckets.Sort([](const FBucket& A, const FBucket& B)
		{
			const int32 MaterialOrder = FCString::Strcmp(*A.Material, *B.Material);
			if (MaterialOrder != 0) return MaterialOrder < 0;
			for (int32 Axis = 0; Axis < 4; ++Axis)
				if (A.Plane[Axis] != B.Plane[Axis]) return A.Plane[Axis] < B.Plane[Axis];
			return false;
		});
		if (Buckets.IsEmpty())
			return FResult::Error(WoTWorldError(EWoTBootstrapError::CorruptInstallation,
				TEXT("a mover Brush FPolys produce no render triangles"), Actor.Identity));
		struct FOrderedBucket
		{
			const FBucket* Bucket = nullptr;
			FString Slot;
		};
		TArray<FOrderedBucket> OrderedBuckets;
		OrderedBuckets.Reserve(Buckets.Num());
		for (int32 BucketIndex = 0; BucketIndex < Buckets.Num(); ++BucketIndex)
		{
			FOrderedBucket& Ordered = OrderedBuckets.AddDefaulted_GetRef();
			Ordered.Bucket = &Buckets[BucketIndex];
			Ordered.Slot = FString::Printf(TEXT("%s__MP%04d"),
				*Buckets[BucketIndex].Material, BucketIndex);
		}
		// groups_to_glb writes primitives by their final split-slot spelling,
		// after MP ordinals were assigned from (material, plane). Preserve both
		// ordering stages so section ranges match the development mover GLBs.
		OrderedBuckets.Sort([](const FOrderedBucket& A, const FOrderedBucket& B)
		{
			return FCString::Strcmp(*A.Slot, *B.Slot) < 0;
		});
		FWoTWorldMoverModel& Out = Result.AddDefaulted_GetRef();
		Out.ActorIdentity = Actor.Identity;
		Out.ModelIdentity = Brush->ObjectIdentity;
		Out.ActorIndex = ActorIndex;
		Out.BrushModel = MoveTemp(Model);
		Out.Polys = MoveTemp(Polys);
		Out.CollisionPositions = MoveTemp(CollisionPositions);
		Out.CollisionIndices = MoveTemp(CollisionIndices);
		bool bDynamicLightMover = false;
		for (const FWoTTypedProperty& Property : Actor.Properties)
		{
			if (!Property.Name.Equals(
					TEXT("bDynamicLightMover"), ESearchCase::IgnoreCase)) continue;
			if (Property.Type != 3 || Property.ArrayIndex != 0
				|| Property.Value.Num() != 1)
			{
				return FResult::Error(WoTWorldError(
					EWoTBootstrapError::CorruptInstallation,
					TEXT("a mover bDynamicLightMover property is malformed"),
					Actor.Identity));
			}
			bDynamicLightMover = Property.Value[0] != 0;
			break;
		}
		Out.bLightmapped = !bDynamicLightMover;
		for (const FOrderedBucket& Ordered : OrderedBuckets)
		{
			const FBucket& Bucket = *Ordered.Bucket;
			const FMaterialIdentity& Identity =
				MaterialIdentities.FindChecked(Bucket.Material);
			FWoTWorldMoverSection& Section = Out.Sections.AddDefaulted_GetRef();
			Section.MaterialSlot = Ordered.Slot;
			Section.CanonicalPackage = Identity.Package;
			Section.CanonicalName = Identity.Name;
			Section.SourceTextureIdentity = Identity.SourceIdentity;
			Section.MacroCanonicalPackage = Identity.MacroPackage;
			Section.MacroCanonicalName = Identity.MacroName;
			Section.MacroSourceTextureIdentity = Identity.MacroSourceIdentity;
			Section.MacroRatio = Identity.MacroRatio;
			Section.SurfaceFlags = Identity.Flags;
			Section.PlaneOrigin = FVector3f(Bucket.Origin);
			Section.PlaneNormal = FVector3f(Bucket.Normal);
			Section.FirstIndex = Out.Indices.Num();
			Section.MinVertex = Out.Positions.Num();
			for (const FTriangle& Triangle : Bucket.Triangles)
			{
				Section.PolyIndices.AddUnique(Triangle.PolyIndex);
				for (int32 Corner = 0; Corner < 3; ++Corner)
				{
					const FVector3f& SourcePosition = Triangle.Positions[Corner];
					Out.Positions.Add(FVector3f(
						WoTWorldMoverPosition(SourcePosition.X),
						WoTWorldMoverPosition(SourcePosition.Y),
						WoTWorldMoverPosition(SourcePosition.Z)));
					Out.Normals.Add(Triangle.Normal);
					Out.UV0.Add(Triangle.UV0[Corner]);
					Out.VertexPolys.Add(Triangle.PolyIndex);
					Out.UV1SourcePositions.Add(SourcePosition);
					Out.Indices.Add(Out.Indices.Num());
				}
			}
			Section.IndexCount = Out.Indices.Num() - Section.FirstIndex;
			Section.MaxVertex = Out.Positions.Num() - 1;
		}
		for (int32 Index = 0; Index < Out.VertexPolys.Num(); Index += 3)
		{
			if (!Out.Polys.IsValidIndex(Out.VertexPolys[Index])
				|| Out.VertexPolys[Index] != Out.VertexPolys[Index + 1]
				|| Out.VertexPolys[Index] != Out.VertexPolys[Index + 2])
			{
				++Out.PolyOwnershipViolationCount;
			}
		}
	}
	return FResult::Success(MoveTemp(Result));
}

struct FWoTWorldAtlasEntry
{
	int32 X = 0;
	int32 Y = 0;
	int32 W = 0;
	int32 H = 0;
	int32 Order = 0;
	bool bPresent = false;
	FVector2f PanUV = FVector2f::ZeroVector;
};

struct FWoTWorldAtlas
{
	int32 Width = 0;
	int32 Height = 0;
	FIntPoint FullbrightOuter = FIntPoint::ZeroValue;
	FVector2f FullbrightUV = FVector2f::ZeroVector;
	TArray<FWoTWorldAtlasEntry> Surfaces;
	TArray<uint8> BGRA;
};

bool WoTWorldPackAtlas(TConstArrayView<FIntPoint> Clamps,
	TConstArrayView<FIntPoint> Sizes, FWoTWorldAtlas& Out)
{
	struct FRect
	{
		int32 Surface = INDEX_NONE;
		int32 W = 0;
		int32 H = 0;
		int32 Order = 0;
	};
	TArray<FRect> Rects;
	Rects.Add({INDEX_NONE, 3, 3, 0});
	for (int32 SurfaceIndex = 0; SurfaceIndex < Clamps.Num(); ++SurfaceIndex)
	{
		if (Clamps[SurfaceIndex].X < 1 || Clamps[SurfaceIndex].Y < 1) continue;
		Rects.Add({SurfaceIndex, Clamps[SurfaceIndex].X + 2,
			Clamps[SurfaceIndex].Y + 2,
			Rects.Num()});
	}
	Rects.Sort([](const FRect& A, const FRect& B)
	{
		if (A.H != B.H) return A.H > B.H;
		if (A.W != B.W) return A.W > B.W;
		// Python's sorted() is stable; the explicit ordinal makes that rule
		// independent of TArray::Sort's implementation.
		return A.Order < B.Order;
	});
	for (const FIntPoint Size : Sizes)
	{
		TArray<FWoTWorldAtlasEntry> Candidate;
		Candidate.SetNum(Clamps.Num());
		int32 X = 0, Y = 0, Shelf = 0;
		bool bFits = true;
		FIntPoint FullbrightOuter = FIntPoint::ZeroValue;
		for (const FRect& Rect : Rects)
		{
			if (Rect.W > Size.X) { bFits = false; break; }
			if (X + Rect.W > Size.X)
			{
				Y += Shelf;
				X = 0;
				Shelf = 0;
			}
			Shelf = FMath::Max(Shelf, Rect.H);
			if (Y + Rect.H > Size.Y) { bFits = false; break; }
			if (Rect.Surface == INDEX_NONE)
			{
				FullbrightOuter = FIntPoint(X, Y);
			}
			else
			{
				FWoTWorldAtlasEntry& Entry = Candidate[Rect.Surface];
				Entry.X = X + 1;
				Entry.Y = Y + 1;
				Entry.W = Rect.W - 2;
				Entry.H = Rect.H - 2;
				Entry.Order = Rect.Order;
				Entry.bPresent = true;
			}
			X += Rect.W;
		}
		if (!bFits) continue;
		Out.Width = Size.X;
		Out.Height = Size.Y;
		Out.FullbrightOuter = FullbrightOuter;
		Out.Surfaces = MoveTemp(Candidate);
		Out.FullbrightUV = FVector2f(
			static_cast<float>(
				(static_cast<double>(FullbrightOuter.X + 1) + 0.5) / Size.X),
			static_cast<float>(
				(static_cast<double>(FullbrightOuter.Y + 1) + 0.5) / Size.Y));
		return true;
	}
	return false;
}

bool WoTWorldBuildAtlas(const FWoTWorldModel& Model, FWoTWorldAtlas& Out)
{
	TArray<FIntPoint> Clamps;
	Clamps.Init(FIntPoint::ZeroValue, Model.Surfaces.Num());
	for (int32 SurfaceIndex = 0; SurfaceIndex < Model.Surfaces.Num(); ++SurfaceIndex)
	{
		const int32 LightMapIndex = Model.Surfaces[SurfaceIndex].LightMap;
		if (LightMapIndex < 0) continue;
		if (!Model.LightMaps.IsValidIndex(LightMapIndex)) return false;
		const FWoTWorldLightMap& LightMap = Model.LightMaps[LightMapIndex];
		Clamps[SurfaceIndex] = FIntPoint(LightMap.UClamp, LightMap.VClamp);
	}
	const FIntPoint Sizes[] = {
		{1024, 1024}, {2048, 2048}, {4096, 4096}, {4096, 8192}};
	return WoTWorldPackAtlas(Clamps, MakeArrayView(Sizes), Out);
}

const FWoTTypedProperty* WoTWorldFindProperty(
	const FWoTWorldActor& Actor, FStringView Name)
{
	for (const FWoTTypedProperty& Property : Actor.Properties)
		if (FStringView(Property.Name).Equals(Name, ESearchCase::IgnoreCase)) return &Property;
	return nullptr;
}

FString WoTWorldActorLeafClass(const FWoTWorldActor& Actor)
{
	int32 Dot = INDEX_NONE;
	return Actor.ClassIdentity.FindLastChar(TEXT('.'), Dot)
		? Actor.ClassIdentity.Mid(Dot + 1) : Actor.ClassIdentity;
}

enum class EWoTWorldEntryLightEffect : uint8
{
	None = 0,
	TorchWaver = 1,
	FireWaver = 2,
	WateryShimmer = 3,
	StaticSpot = 8,
	Spotlight = 12,
	NonIncidence = 13,
	Cylinder = 17,
};

struct FWoTWorldEntryLight
{
	FString Identity;
	FString Tag;
	FVector3d Location = FVector3d::ZeroVector;
	FIntVector Rotation = FIntVector::ZeroValue;
	uint8 Brightness = 64;
	uint8 Hue = 0;
	uint8 Saturation = 255;
	uint8 RadiusByte = 64;
	uint8 Cone = 128;
	uint8 LightType = 1;
	EWoTWorldEntryLightEffect Effect = EWoTWorldEntryLightEffect::None;
	bool bInitiallyOff = false;
	double Radius = 0.0;
	double ReciprocalRadiusExact = 0.0;
	float ReciprocalRadius = 0.0f;
	float ReciprocalRadiusMultiplier = 0.0f;
	FVector3d View = FVector3d::ZeroVector;
	int64 FixedColor[3] = {};
};

struct FWoTWorldLightDefaults
{
	uint8 LightType = 0;
	uint8 Effect = 0;
	uint8 Brightness = 0;
	uint8 Hue = 0;
	uint8 Saturation = 0;
	uint8 Radius = 0;
	uint8 Cone = 0;
	// Engine.Light / SunLight.uc:27 set LightPeriod=32; Actor and the sprayer
	// chains leave the engine zero-init (LightPhase is zero everywhere).
	uint8 Period = 0;
	bool bSunLightRadius = false;
};

bool WoTWorldLightDefaults(FStringView LeafClass,
	FWoTWorldLightDefaults& OutDefaults)
{
	OutDefaults = FWoTWorldLightDefaults();
	if (LeafClass.Equals(TEXT("Light"), ESearchCase::IgnoreCase)
		|| LeafClass.Equals(TEXT("Spotlight"), ESearchCase::IgnoreCase))
	{
		// UE1 Engine.Light defaults. Engine script is
		// stripped from the shipped exports, matching the Python chain fallback.
		OutDefaults.LightType = 1;
		OutDefaults.Brightness = 64;
		OutDefaults.Saturation = 255;
		OutDefaults.Radius = 64;
		OutDefaults.Cone = 128;
		OutDefaults.Period = 32;
		// UE1 Engine.Spotlight: LightEffect=LE_Spotlight.
		if (LeafClass.Equals(TEXT("Spotlight"), ESearchCase::IgnoreCase))
			OutDefaults.Effect = 12;
	}
	else if (LeafClass.Equals(TEXT("SunLight"), ESearchCase::IgnoreCase))
	{
		// exports/script/Legend/SunLight.uc:8,14,16,18,23-28.
		OutDefaults.LightType = 1;
		OutDefaults.Brightness = 255;
		OutDefaults.Saturation = 255;
		OutDefaults.Radius = 64;
		OutDefaults.Cone = 128;
		OutDefaults.Period = 32;
		OutDefaults.bSunLightRadius = true;
	}
	else if (LeafClass.Equals(TEXT("PixieSprayerBlue"), ESearchCase::IgnoreCase))
	{
		// exports/script/Angreal/PixieSprayer.uc:12,23,38,46-51 and
		// exports/script/Angreal/PixieSprayerBlue.uc:12,19,25.
		OutDefaults.LightType = 1;
		OutDefaults.Effect = 13;
		OutDefaults.Brightness = 100;
		OutDefaults.Hue = 140;
		OutDefaults.Saturation = 64;
		OutDefaults.Radius = 25;
	}
	else if (LeafClass.Equals(TEXT("ParticleSprayer"), ESearchCase::IgnoreCase))
	{
		// exports/script/ParticleSystems/ParticleSprayer.uc:26,411,436:
		// its defaultproperties do not override Actor's zero light fields.
	}
	else if (LeafClass.Equals(TEXT("Flame01"), ESearchCase::IgnoreCase))
	{
		// exports/script/ParticleSystems/Flame01.uc:12,21,31 inherits the
		// ParticleSprayer zero light fields without a light override.
	}
	else if (LeafClass.Equals(TEXT("LampFlame01"), ESearchCase::IgnoreCase))
	{
		// exports/script/ParticleSystems/LampFlame01.uc:12,21,33 inherits the
		// ParticleSprayer zero light fields without a light override.
	}
	else if (LeafClass.Equals(TEXT("PitOpening"), ESearchCase::IgnoreCase))
	{
		// exports/script/WoTTraps/PitOpening.uc:7,44 inherits Actor's zero
		// light fields without a light override.
	}
	else if (LeafClass.Equals(TEXT("Cylinder"), ESearchCase::IgnoreCase))
	{
		// exports/script/WoTDecorations/Cylinder.uc:7,21 inherits the
		// WOTDecoration/Actor zero light fields without a light override.
	}
	else if (LeafClass.Equals(TEXT("twotriangles"), ESearchCase::IgnoreCase))
	{
		// exports/script/WoTDecorations/twotriangles.uc:4,18 inherits the
		// WOTDecoration/Actor zero light fields without a light override.
	}
	else
	{
		return false;
	}
	return true;
}

bool WoTWorldSupportedLightType(uint8 LightType)
{
	return LightType == 1 || LightType == 2 || LightType == 3 || LightType == 7;
}

bool WoTWorldSupportedLightEffect(uint8 Effect)
{
	return Effect == 0 || Effect == 1 || Effect == 2 || Effect == 3
		|| Effect == 8 || Effect == 12 || Effect == 13 || Effect == 17;
}

FString WoTWorldLightTypeName(uint8 LightType)
{
	switch (LightType)
	{
	case 0: return TEXT("LT_None");
	case 1: return TEXT("LT_Steady");
	case 2: return TEXT("LT_Pulse");
	case 3: return TEXT("LT_Blink");
	case 4: return TEXT("LT_Flicker");
	case 5: return TEXT("LT_Strobe");
	case 6: return TEXT("LT_BackdropLight");
	case 7: return TEXT("LT_SubtlePulse");
	case 8: return TEXT("LT_TexturePaletteOnce");
	case 9: return TEXT("LT_TexturePaletteLoop");
	default: return FString::Printf(TEXT("%u"), LightType);
	}
}

FString WoTWorldLightEffectName(uint8 Effect)
{
	switch (Effect)
	{
	case 4: return TEXT("LE_Searchlight");
	case 5: return TEXT("LE_SlowWave");
	case 6: return TEXT("LE_FastWave");
	case 7: return TEXT("LE_CloudCast");
	case 9: return TEXT("LE_Shock");
	case 10: return TEXT("LE_Disco");
	case 11: return TEXT("LE_Warp");
	case 14: return TEXT("LE_Shell");
	case 15: return TEXT("LE_OmniBumpMap");
	case 16: return TEXT("LE_Interference");
	case 18: return TEXT("LE_Rotor");
	case 19: return TEXT("LE_Unused");
	default: return FString::Printf(TEXT("%u"), Effect);
	}
}

FString WoTWorldClassifyLightSkip(bool bActorPresent,
	bool bRecognizedClass, bool bLightningOff, uint8 LightType,
	uint8 Brightness, uint8 Effect, bool bMaskInRange)
{
	if (!bActorPresent) return TEXT("missing_actor");
	if (!bRecognizedClass) return TEXT("unsupported_class");
	if (bLightningOff) return TEXT("lightning_off");
	if (!WoTWorldSupportedLightType(LightType))
		return TEXT("type:") + WoTWorldLightTypeName(LightType);
	if (Brightness == 0) return TEXT("brightness_0");
	if (!WoTWorldSupportedLightEffect(Effect))
		return TEXT("effect:") + WoTWorldLightEffectName(Effect);
	if (!bMaskInRange) return TEXT("mask_out_of_range");
	return {};
}

struct FWoTWorldEntryIllumination
{
	uint32 LightIndex = 0;
	uint32 SurfaceIndex = 0;
	TArray<uint8> Illumination;
};

struct FWoTWorldLightmapComposeStats
{
	TMap<FString, uint32> SkippedLights;
	uint32 PerTexelZoneSurfaceCount = 0;

	void Skip(FString Reason)
	{
		++SkippedLights.FindOrAdd(MoveTemp(Reason));
	}
};

// ---------------------------------------------------------------------------
// Animated light-map responses (Tools/convert/lightmaps.py animation_manifest +
// bake_animation_responses, lightmap_atlas.py): every Pulse/SubtlePulse light
// forms a (type, period, phase) waveform group; every TorchWaver/FireWaver/
// WateryShimmer light forms a shimmer effect class; a light that is both adds
// an intersection term. Each term's centered response is composed from the
// exact retail endpoint compositions (hi/lo wave scale; hi/lo shimmer factor
// floored into the illumination bytes before the fixed-point palette) and
// stored as unsigned scale-2 RGB bytes per lit surface.
// ---------------------------------------------------------------------------

bool WoTWorldEntryReadU8Property(const FWoTWorldActor& Actor,
	FStringView Name, uint8 DefaultValue, uint8& Out);
bool WoTWorldEntryReadNameProperty(const FWoTWorldActor& Actor,
	FStringView Name, FString& Out);

constexpr int32 WoTWorldMaxWaveformGroups = 4;
constexpr int32 WoTWorldMaxResponseTerms = 6;
constexpr uint16 WoTWorldEntryBSPPayloadVersion = 5;
constexpr uint16 WoTWorldBSPPayloadVersion = 10;
constexpr uint32 WoTWorldMaxResponseBytes = 64u * 1024u * 1024u;

struct FWoTWorldAnimationGroup
{
	uint8 Type = 0;
	uint8 Period = 0;
	uint8 Phase = 0;
	uint32 LightCount = 0;
};

struct FWoTWorldAnimationTerm
{
	uint8 Kind = 0; // 0 waveform, 1 effect, 2 intersection
	int32 Group = INDEX_NONE;
	uint8 Effect = 0; // 1 TorchWaver, 2 FireWaver, 3 WateryShimmer
	uint32 LightCount = 0;
};

struct FWoTWorldAnimationLight
{
	int32 Group = INDEX_NONE;
	uint8 Effect = 0;
};

struct FWoTWorldAnimationManifest
{
	TArray<FWoTWorldAnimationGroup> Groups;
	TArray<FWoTWorldAnimationTerm> Terms;
	TMap<int32, FWoTWorldAnimationLight> ByReference;
};

struct FWoTWorldResponseRect
{
	int32 SurfaceIndex = 0;
	TArray<uint8> RGB;
};

struct FWoTWorldResponseTerm
{
	TArray<FWoTWorldResponseRect> Rects;
};

struct FWoTWorldComposedAtlas
{
	FWoTWorldAtlas Atlas;
	/** BSP v8 sky-rest trailer bytes for the main map part (empty: none). */
	TArray<uint8> SkyRestTrailer;
	TArray<FWoTWorldEntryLight> LightningLights;
	TArray<FWoTWorldEntryIllumination> WeatherPairs;
	FWoTWorldLightmapComposeStats Stats;
	float LevelBrightness = 1.0f;
	FWoTWorldAnimationManifest Manifest;
	TArray<FWoTWorldResponseTerm> Responses;
};

double WoTWorldWaveMean(uint8 LightType)
{
	return LightType == 2 ? 0.60
		: (LightType == 3 ? 0.50 : (LightType == 7 ? 0.90 : 1.0));
}

double WoTWorldWaveAmplitude(uint8 LightType)
{
	return LightType == 2 ? 0.39
		: (LightType == 3 ? 0.50 : (LightType == 7 ? 0.09 : 0.0));
}

double WoTWorldEffectMean(uint8 Effect)
{
	return Effect == 1 ? 0.975 : (Effect == 2 ? 0.90 : (Effect == 3 ? 0.80 : 1.0));
}

double WoTWorldEffectAmplitude(uint8 Effect)
{
	return Effect == 1 ? 0.025 : (Effect == 2 ? 0.10 : (Effect == 3 ? 0.20 : 0.0));
}

/** animation_manifest(): every T3D actor whose light resolves, emits, uses a
    supported type/effect, and is not Lightning-folded; groups sorted by exact
    (type, period, phase); terms in waveform, effect, intersection order. */
FWoTBootstrapStatus WoTWorldBuildAnimationManifest(const FWoTWorldSource& Source,
	const TSet<FString>& LightningTags, FWoTWorldAnimationManifest& Out)
{
	Out = FWoTWorldAnimationManifest();
	struct FResolved
	{
		int32 Reference = 0;
		uint8 Type = 0;
		uint8 Period = 0;
		uint8 Phase = 0;
		uint8 Effect = 0;
	};
	TArray<FResolved> Relevant;
	for (const FWoTWorldActor& Actor : Source.Actors)
	{
		FWoTWorldLightDefaults Defaults;
		if (!WoTWorldLightDefaults(WoTWorldActorLeafClass(Actor), Defaults)) continue;
		FResolved Light;
		uint8 Brightness = 0;
		FString Tag;
		if (!WoTWorldEntryReadU8Property(Actor, TEXT("LightType"), Defaults.LightType, Light.Type)
			|| !WoTWorldEntryReadU8Property(Actor, TEXT("LightEffect"), Defaults.Effect, Light.Effect)
			|| !WoTWorldEntryReadU8Property(Actor, TEXT("LightBrightness"), Defaults.Brightness, Brightness)
			|| !WoTWorldEntryReadU8Property(Actor, TEXT("LightPeriod"), Defaults.Period, Light.Period)
			|| !WoTWorldEntryReadU8Property(Actor, TEXT("LightPhase"), 0, Light.Phase)
			|| !WoTWorldEntryReadNameProperty(Actor, TEXT("Tag"), Tag))
		{
			return FWoTBootstrapStatus::Error(WoTWorldError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("a light has malformed animation properties"), Actor.Identity));
		}
		if (Brightness == 0 || !WoTWorldSupportedLightType(Light.Type)
			|| !WoTWorldSupportedLightEffect(Light.Effect)
			|| LightningTags.Contains(WoTWorldFold(Tag))) continue;
		const bool bWave = Light.Type == 2 || Light.Type == 3 || Light.Type == 7;
		const bool bShimmer = Light.Effect >= 1 && Light.Effect <= 3;
		if (!bWave && !bShimmer) continue;
		Light.Reference = Actor.ExportReference;
		Relevant.Add(Light);
	}
	TArray<FWoTWorldAnimationGroup> Groups;
	for (const FResolved& Light : Relevant)
	{
		if (Light.Type != 2 && Light.Type != 3 && Light.Type != 7) continue;
		bool bFound = false;
		for (FWoTWorldAnimationGroup& Group : Groups)
		{
			if (Group.Type == Light.Type && Group.Period == Light.Period
				&& Group.Phase == Light.Phase)
			{
				++Group.LightCount;
				bFound = true;
				break;
			}
		}
		if (!bFound)
		{
			FWoTWorldAnimationGroup& Group = Groups.AddDefaulted_GetRef();
			Group.Type = Light.Type;
			Group.Period = Light.Period;
			Group.Phase = Light.Phase;
			Group.LightCount = 1;
		}
	}
	Groups.Sort([](const FWoTWorldAnimationGroup& A, const FWoTWorldAnimationGroup& B)
	{
		if (A.Type != B.Type) return A.Type < B.Type;
		if (A.Period != B.Period) return A.Period < B.Period;
		return A.Phase < B.Phase;
	});
	if (Groups.Num() > WoTWorldMaxWaveformGroups)
	{
		return FWoTBootstrapStatus::Error(WoTWorldError(EWoTBootstrapError::SizeLimit,
			TEXT("the map exceeds four waveform groups"), Source.SourcePath));
	}
	auto GroupOf = [&Groups](const FResolved& Light)
	{
		if (Light.Type != 2 && Light.Type != 3 && Light.Type != 7)
			return static_cast<int32>(INDEX_NONE);
		for (int32 Index = 0; Index < Groups.Num(); ++Index)
			if (Groups[Index].Type == Light.Type && Groups[Index].Period == Light.Period
				&& Groups[Index].Phase == Light.Phase) return Index;
		return static_cast<int32>(INDEX_NONE);
	};
	uint32 EffectCounts[4] = {};
	uint32 IntersectionCounts[WoTWorldMaxWaveformGroups][4] = {};
	for (const FResolved& Light : Relevant)
	{
		const int32 Group = GroupOf(Light);
		const uint8 Effect = (Light.Effect >= 1 && Light.Effect <= 3) ? Light.Effect : 0;
		Out.ByReference.Add(Light.Reference, FWoTWorldAnimationLight{Group, Effect});
		if (Effect != 0) ++EffectCounts[Effect];
		if (Effect != 0 && Group != INDEX_NONE) ++IntersectionCounts[Group][Effect];
	}
	Out.Groups = Groups;
	for (int32 Index = 0; Index < Groups.Num(); ++Index)
	{
		FWoTWorldAnimationTerm& Term = Out.Terms.AddDefaulted_GetRef();
		Term.Kind = 0;
		Term.Group = Index;
		Term.LightCount = Groups[Index].LightCount;
	}
	for (uint8 Effect = 1; Effect <= 3; ++Effect)
	{
		if (EffectCounts[Effect] == 0) continue;
		FWoTWorldAnimationTerm& Term = Out.Terms.AddDefaulted_GetRef();
		Term.Kind = 1;
		Term.Effect = Effect;
		Term.LightCount = EffectCounts[Effect];
	}
	for (int32 Group = 0; Group < Groups.Num(); ++Group)
	{
		for (uint8 Effect = 1; Effect <= 3; ++Effect)
		{
			if (IntersectionCounts[Group][Effect] == 0) continue;
			FWoTWorldAnimationTerm& Term = Out.Terms.AddDefaulted_GetRef();
			Term.Kind = 2;
			Term.Group = Group;
			Term.Effect = Effect;
			Term.LightCount = IntersectionCounts[Group][Effect];
		}
	}
	if (Out.Terms.Num() > WoTWorldMaxResponseTerms)
	{
		return FWoTBootstrapStatus::Error(WoTWorldError(EWoTBootstrapError::SizeLimit,
			TEXT("the map exceeds six light-map response terms"), Source.SourcePath));
	}
	return FWoTBootstrapStatus::Success();
}

/** One endpoint composition: the group at hi/lo, or the effect class at hi/lo
    (or both for an intersection); everything else at its exact mean. */
struct FWoTWorldResponseAccumulator
{
	int32 Group = INDEX_NONE;
	int32 Wave = 0;      // +1 hi, -1 lo, 0 mean
	uint8 Effect = 0;
	int32 EffectEnd = 0; // +1 hi, -1 lo, 0 mean
};

void WoTWorldResponseAccumulators(const FWoTWorldAnimationManifest& Manifest,
	TArray<FWoTWorldResponseAccumulator>& Out, TArray<TArray<int32>>& OutByTerm)
{
	Out.Reset();
	OutByTerm.Reset();
	for (const FWoTWorldAnimationTerm& Term : Manifest.Terms)
	{
		TArray<int32>& Slots = OutByTerm.AddDefaulted_GetRef();
		if (Term.Kind == 0)
		{
			Slots.Add(Out.Add({Term.Group, 1, 0, 0}));
			Slots.Add(Out.Add({Term.Group, -1, 0, 0}));
		}
		else if (Term.Kind == 1)
		{
			Slots.Add(Out.Add({INDEX_NONE, 0, Term.Effect, 1}));
			Slots.Add(Out.Add({INDEX_NONE, 0, Term.Effect, -1}));
		}
		else
		{
			Slots.Add(Out.Add({Term.Group, 1, Term.Effect, 1}));
			Slots.Add(Out.Add({Term.Group, 1, Term.Effect, -1}));
			Slots.Add(Out.Add({Term.Group, -1, Term.Effect, 1}));
			Slots.Add(Out.Add({Term.Group, -1, Term.Effect, -1}));
		}
	}
}


class FWoTWorldEntryPairBudget
{
public:
	bool Reserve(uint32 LightIndex, uint32 SurfaceIndex, uint32 ByteCount)
	{
		return ReserveKey(
			(static_cast<uint64>(LightIndex) << 32) | SurfaceIndex, ByteCount);
	}

	bool ReserveKey(uint64 PairKey, uint32 ByteCount)
	{
		if (ByteCount == 0
			|| PairKeys.Num() >= static_cast<int32>(WoTWorldMaxEntryPairs)
			|| PairKeys.Contains(PairKey)
			|| ByteCount > WoTWorldMaxEntryPairBytes - PairBytes)
		{
			return false;
		}
		PairKeys.Add(PairKey);
		PairBytes += ByteCount;
		return true;
	}

	int32 Num() const { return PairKeys.Num(); }
	uint64 GetPairBytes() const { return PairBytes; }

private:
	TSet<uint64> PairKeys;
	uint64 PairBytes = 0;
};

bool WoTWorldEntryReadU8Property(const FWoTWorldActor& Actor,
	FStringView Name, uint8 DefaultValue, uint8& Out)
{
	const FWoTTypedProperty* Property = WoTWorldFindProperty(Actor, Name);
	if (Property == nullptr)
	{
		Out = DefaultValue;
		return true;
	}
	if (Property->Type != 1 || Property->ArrayIndex != 0
		|| Property->Value.Num() != 1) return false;
	Out = Property->Value[0];
	return true;
}

bool WoTWorldEntryReadFloatProperty(const FWoTWorldActor& Actor,
	FStringView Name, float DefaultValue, float& Out)
{
	const FWoTTypedProperty* Property = WoTWorldFindProperty(Actor, Name);
	if (Property == nullptr)
	{
		Out = DefaultValue;
		return true;
	}
	if (Property->Type != 4 || Property->ArrayIndex != 0
		|| Property->Value.Num() != 4) return false;
	uint32 Bits = static_cast<uint32>(Property->Value[0])
		| (static_cast<uint32>(Property->Value[1]) << 8)
		| (static_cast<uint32>(Property->Value[2]) << 16)
		| (static_cast<uint32>(Property->Value[3]) << 24);
	FMemory::Memcpy(&Out, &Bits, sizeof(Out));
	return FMath::IsFinite(Out);
}

bool WoTWorldEntryReadNameProperty(const FWoTWorldActor& Actor,
	FStringView Name, FString& Out)
{
	const FWoTTypedProperty* Property = WoTWorldFindProperty(Actor, Name);
	if (Property == nullptr)
	{
		Out.Reset();
		return true;
	}
	if (Property->Type != 6 || Property->ArrayIndex != 0) return false;
	Out = Property->NameValue;
	return true;
}

uint32 WoTWorldEntryU32(const uint8* Bytes)
{
	return static_cast<uint32>(Bytes[0])
		| (static_cast<uint32>(Bytes[1]) << 8)
		| (static_cast<uint32>(Bytes[2]) << 16)
		| (static_cast<uint32>(Bytes[3]) << 24);
}

bool WoTWorldEntryReadVectorProperty(const FWoTWorldActor& Actor,
	FStringView Name, FVector3d& Out)
{
	const FWoTTypedProperty* Property = WoTWorldFindProperty(Actor, Name);
	if (Property == nullptr)
	{
		Out = FVector3d::ZeroVector;
		return true;
	}
	if (Property->Type != 10 || Property->ArrayIndex != 0
		|| Property->StructName != TEXT("Vector") || Property->Value.Num() != 12)
		return false;
	float Values[3] = {};
	for (int32 Component = 0; Component < 3; ++Component)
	{
		const uint32 Bits = WoTWorldEntryU32(Property->Value.GetData() + Component * 4);
		FMemory::Memcpy(&Values[Component], &Bits, sizeof(float));
		if (!FMath::IsFinite(Values[Component])) return false;
	}
	Out = FVector3d(Values[0], Values[1], Values[2]);
	return true;
}

bool WoTWorldEntryReadRotatorProperty(const FWoTWorldActor& Actor,
	FStringView Name, FIntVector& Out)
{
	const FWoTTypedProperty* Property = WoTWorldFindProperty(Actor, Name);
	if (Property == nullptr)
	{
		Out = FIntVector::ZeroValue;
		return true;
	}
	if (Property->Type != 10 || Property->ArrayIndex != 0
		|| Property->StructName != TEXT("Rotator") || Property->Value.Num() != 12)
		return false;
	Out = FIntVector(
		static_cast<int32>(WoTWorldEntryU32(Property->Value.GetData())),
		static_cast<int32>(WoTWorldEntryU32(Property->Value.GetData() + 4)),
		static_cast<int32>(WoTWorldEntryU32(Property->Value.GetData() + 8)));
	return true;
}

FVector3f WoTWorldEntryHSV(uint8 Hue, uint8 Saturation, uint8 Value)
{
	double Brightness = static_cast<double>(Value) * 1.4 / 255.0;
	Brightness *= 0.70 / (0.01 + FMath::Sqrt(Brightness));
	Brightness = FMath::Clamp(Brightness, 0.0, 1.0);
	double Channels[3] = {};
	if (Hue < 86)
	{
		Channels[0] = (85.0 - Hue) / 85.0;
		Channels[1] = static_cast<double>(Hue) / 85.0;
	}
	else if (Hue < 171)
	{
		Channels[1] = (170.0 - Hue) / 85.0;
		Channels[2] = (static_cast<double>(Hue) - 85.0) / 85.0;
	}
	else
	{
		Channels[0] = (static_cast<double>(Hue) - 170.0) / 85.0;
		Channels[2] = (255.0 - Hue) / 84.0;
	}
	const double SaturationScale = static_cast<double>(Saturation) / 255.0;
	return FVector3f(
		static_cast<float>((Channels[0]
			+ SaturationScale * (1.0 - Channels[0])) * Brightness),
		static_cast<float>((Channels[1]
			+ SaturationScale * (1.0 - Channels[1])) * Brightness),
		static_cast<float>((Channels[2]
			+ SaturationScale * (1.0 - Channels[2])) * Brightness));
}

FVector3d WoTWorldEntryRotationVector(const FIntVector& Rotation)
{
	auto SinTable = [](int64 Angle)
	{
		const uint32 Index = static_cast<uint32>(
			(static_cast<uint64>(Angle) >> 2) & 16383ull);
		return static_cast<float>(FMath::Sin(
			static_cast<double>(Index) * (2.0 * UE_PI / 16384.0)));
	};
	const float CosPitch = SinTable(static_cast<int64>(Rotation.X) + 16384);
	const float SinPitch = SinTable(Rotation.X);
	const float CosYaw = SinTable(static_cast<int64>(Rotation.Y) + 16384);
	const float SinYaw = SinTable(Rotation.Y);
	return FVector3d(
		static_cast<float>(CosPitch * CosYaw),
		static_cast<float>(CosPitch * SinYaw), SinPitch);
}

double WoTWorldProjectT3DLocation(double Value)
{
	const double Scaled = Value * 1000000.0;
	const double Rounded = Scaled >= 0.0
		? FMath::FloorToDouble(Scaled + 0.5)
		: FMath::CeilToDouble(Scaled - 0.5);
	return Rounded / 1000000.0;
}

bool WoTWorldResolveEntryLight(const FWoTWorldActor& Actor,
	float LevelBrightness, FWoTWorldEntryLight& Out)
{
	const FString LeafClass = WoTWorldActorLeafClass(Actor);
	FWoTWorldLightDefaults Defaults;
	if (!WoTWorldLightDefaults(LeafClass, Defaults)) return false;
	uint8 Effect = Defaults.Effect;
	if (!WoTWorldEntryReadU8Property(
			Actor, TEXT("LightType"), Defaults.LightType, Out.LightType)
		|| !WoTWorldEntryReadU8Property(
			Actor, TEXT("LightEffect"), Defaults.Effect, Effect)
		|| !WoTWorldEntryReadU8Property(
			Actor, TEXT("LightBrightness"), Defaults.Brightness, Out.Brightness)
		|| !WoTWorldEntryReadU8Property(
			Actor, TEXT("LightHue"), Defaults.Hue, Out.Hue)
		|| !WoTWorldEntryReadU8Property(
			Actor, TEXT("LightSaturation"), Defaults.Saturation, Out.Saturation)
		|| !WoTWorldEntryReadU8Property(
			Actor, TEXT("LightRadius"), Defaults.Radius, Out.RadiusByte)
		|| !WoTWorldEntryReadU8Property(
			Actor, TEXT("LightCone"), Defaults.Cone, Out.Cone)
		|| !WoTWorldEntryReadVectorProperty(Actor, TEXT("Location"), Out.Location)
		|| !WoTWorldEntryReadRotatorProperty(Actor, TEXT("Rotation"), Out.Rotation)
		|| !WoTWorldEntryReadNameProperty(Actor, TEXT("Tag"), Out.Tag))
	{
		return false;
	}
	Out.Identity = Actor.Identity;
	Out.Effect = static_cast<EWoTWorldEntryLightEffect>(Effect);
	// The reference consumes UCC's T3D actor projection, whose vector fields
	// are printed with six fractional decimal places. Project package floats to
	// that same universal boundary before spatial lighting math.
	Out.Location.X = WoTWorldProjectT3DLocation(Out.Location.X);
	Out.Location.Y = WoTWorldProjectT3DLocation(Out.Location.Y);
	Out.Location.Z = WoTWorldProjectT3DLocation(Out.Location.Z);
	Out.Radius = (Defaults.bSunLightRadius ? 250.0 : 25.0)
		* (static_cast<double>(Out.RadiusByte) + 1.0);
	const double ReciprocalRadius = 1.0 / FMath::Max(1.0, Out.Radius);
	Out.ReciprocalRadiusExact = ReciprocalRadius;
	Out.ReciprocalRadius = static_cast<float>(ReciprocalRadius);
	Out.ReciprocalRadiusMultiplier = static_cast<float>(
		4093.0 * ReciprocalRadius * ReciprocalRadius);
	Out.View = WoTWorldEntryRotationVector(Out.Rotation);
	const FVector3f HSV = WoTWorldEntryHSV(Out.Hue, Out.Saturation, 255);
	const float BrightnessScale = static_cast<float>(Out.Brightness / 255.0f);
	const float WaveScale = Out.LightType == 2 ? 0.60f
		: (Out.LightType == 7 ? 0.90f : 1.0f);
	const float EffectScale = Effect == 1 ? 0.975f
		: (Effect == 2 ? 0.90f : (Effect == 3 ? 0.80f : 1.0f));
	const float Values[3] = {HSV.X, HSV.Y, HSV.Z};
	for (int32 Channel = 0; Channel < 3; ++Channel)
	{
		float FloatColor = static_cast<float>(Values[Channel] * BrightnessScale);
		FloatColor = static_cast<float>(FloatColor * LevelBrightness);
		FloatColor = static_cast<float>(FloatColor * WaveScale);
		FloatColor = static_cast<float>(FloatColor * EffectScale);
		Out.FixedColor[Channel] = static_cast<int64>(
			FMath::FloorToDouble(static_cast<double>(FloatColor) * 65536.0));
	}
	return true;
}

double WoTWorldEntryDot(const FVector3d& Left, const FVector3d& Right)
{
	double Result = Left.X * Right.X;
	Result += Left.Y * Right.Y;
	Result += Left.Z * Right.Z;
	return Result;
}

int32 WoTWorldCompareUTF8(const FString& Left, const FString& Right)
{
	const FTCHARToUTF8 LeftBytes(*Left);
	const FTCHARToUTF8 RightBytes(*Right);
	const int32 SharedBytes = FMath::Min(LeftBytes.Length(), RightBytes.Length());
	const int32 SharedOrder = FMemory::Memcmp(
		LeftBytes.Get(), RightBytes.Get(), SharedBytes);
	return SharedOrder != 0 ? SharedOrder : LeftBytes.Length() - RightBytes.Length();
}

FVector3d WoTWorldEntryCross(const FVector3d& Left, const FVector3d& Right)
{
	return FVector3d(
		Left.Y * Right.Z - Left.Z * Right.Y,
		Left.Z * Right.X - Left.X * Right.Z,
		Left.X * Right.Y - Left.Y * Right.X);
}

bool WoTWorldEntryRoundHalfEven(double Value, int64& Out)
{
	if (!FMath::IsFinite(Value)
		|| Value < static_cast<double>(MIN_int64)
		|| Value >= static_cast<double>(MAX_int64)) return false;
	const double LowerDouble = FMath::FloorToDouble(Value);
	const double Fraction = Value - LowerDouble;
	const int64 Lower = static_cast<int64>(LowerDouble);
	if (Fraction < 0.5) Out = Lower;
	else if (Fraction > 0.5) Out = Lower + 1;
	else Out = (Lower & 1) == 0 ? Lower : Lower + 1;
	return true;
}

struct FWoTWorldEntryBasis
{
	FVector3d Base = FVector3d::ZeroVector;
	FVector3d DU = FVector3d::ZeroVector;
	FVector3d DV = FVector3d::ZeroVector;
	FVector3d Origin = FVector3d::ZeroVector;
	FVector3d TextureU = FVector3d::ZeroVector;
	FVector3d TextureV = FVector3d::ZeroVector;
	FVector3d Normal = FVector3d::ZeroVector;
};

bool WoTWorldEntryBasis(const FVector3d& Origin, const FVector3d& TextureU,
	const FVector3d& TextureV, const FVector3d& Normal,
	const FWoTWorldLightMap& LightMap, FWoTWorldEntryBasis& Out)
{
	Out.Origin = Origin;
	Out.TextureU = TextureU;
	Out.TextureV = TextureV;
	Out.Normal = Normal;
	// numpy.linalg.inv in the reference solves the three-row basis. Use the
	// same pivoted elimination shape instead of a cofactor identity: algebraic
	// cancellation in the latter differed by one ulp on Battle2_03 surface
	// 3007 and crossed an integer forward-difference rounding boundary.
	double Augmented[3][6] = {
		{Out.TextureU.X, Out.TextureU.Y, Out.TextureU.Z, 1.0, 0.0, 0.0},
		{Out.TextureV.X, Out.TextureV.Y, Out.TextureV.Z, 0.0, 1.0, 0.0},
		{Out.Normal.X, Out.Normal.Y, Out.Normal.Z, 0.0, 0.0, 1.0}};
	for (int32 PivotColumn = 0; PivotColumn < 3; ++PivotColumn)
	{
		int32 PivotRow = PivotColumn;
		for (int32 CandidateRow = PivotColumn + 1; CandidateRow < 3; ++CandidateRow)
		{
			if (FMath::Abs(Augmented[CandidateRow][PivotColumn])
				> FMath::Abs(Augmented[PivotRow][PivotColumn]))
			{
				PivotRow = CandidateRow;
			}
		}
		const double Pivot = Augmented[PivotRow][PivotColumn];
		if (!FMath::IsFinite(Pivot) || FMath::Abs(Pivot) < 1.0e-20) return false;
		if (PivotRow != PivotColumn)
		{
			for (int32 Column = 0; Column < 6; ++Column)
				Swap(Augmented[PivotRow][Column], Augmented[PivotColumn][Column]);
		}
		const double Divisor = Augmented[PivotColumn][PivotColumn];
		for (int32 Column = 0; Column < 6; ++Column)
			Augmented[PivotColumn][Column] /= Divisor;
		for (int32 Row = 0; Row < 3; ++Row)
		{
			if (Row == PivotColumn) continue;
			const double Multiplier = Augmented[Row][PivotColumn];
			for (int32 Column = 0; Column < 6; ++Column)
				Augmented[Row][Column] -= Multiplier * Augmented[PivotColumn][Column];
		}
	}
	const FVector3d InverseU(
		Augmented[0][3], Augmented[1][3], Augmented[2][3]);
	const FVector3d InverseV(
		Augmented[0][4], Augmented[1][4], Augmented[2][4]);
	Out.Base = Out.Origin + InverseU * static_cast<double>(LightMap.Pan.X)
		+ InverseV * static_cast<double>(LightMap.Pan.Y);
	Out.DU = InverseU * static_cast<double>(LightMap.UScale);
	Out.DV = InverseV * static_cast<double>(LightMap.VScale);
	return FMath::IsFinite(Out.Base.X) && FMath::IsFinite(Out.Base.Y)
		&& FMath::IsFinite(Out.Base.Z) && FMath::IsFinite(Out.DU.X)
		&& FMath::IsFinite(Out.DU.Y) && FMath::IsFinite(Out.DU.Z)
		&& FMath::IsFinite(Out.DV.X) && FMath::IsFinite(Out.DV.Y)
		&& FMath::IsFinite(Out.DV.Z);
}

bool WoTWorldEntrySurfaceBasis(const FWoTWorldModel& Model,
	const FWoTWorldSurface& Surface, const FWoTWorldLightMap& LightMap,
	FWoTWorldEntryBasis& Out)
{
	if (!Model.Points.IsValidIndex(Surface.Base)
		|| !Model.Vectors.IsValidIndex(Surface.TextureU)
		|| !Model.Vectors.IsValidIndex(Surface.TextureV)
		|| !Model.Vectors.IsValidIndex(Surface.Normal)) return false;
	return WoTWorldEntryBasis(FVector3d(Model.Points[Surface.Base]),
		FVector3d(Model.Vectors[Surface.TextureU]),
		FVector3d(Model.Vectors[Surface.TextureV]),
		FVector3d(Model.Vectors[Surface.Normal]), LightMap, Out);
}

struct FWoTWorldEntryClipRect
{
	int32 MinU = 0;
	int32 MinV = 0;
	int32 MaxU = 0;
	int32 MaxV = 0;
};

bool WoTWorldEntryClip(const FWoTWorldEntryLight& Light,
	const FWoTWorldEntryBasis& Basis, const FWoTWorldLightMap& LightMap,
	FWoTWorldEntryClipRect& Out)
{
	const FVector3d Relative = Light.Location - Basis.Origin;
	const double PlaneDot = WoTWorldEntryDot(Relative, Basis.Normal);
	const double PlaneRadiusSquared =
		Light.Radius * Light.Radius * 1.05 - PlaneDot * PlaneDot;
	const double PlaneRadius = FMath::Sqrt(FMath::Max(PlaneRadiusSquared, 0.0));
	const FVector3d Center = Light.Location - Basis.Normal * PlaneDot;
	const double CenterU = WoTWorldEntryDot(
		Center - Basis.Origin, Basis.TextureU) - LightMap.Pan.X;
	const double CenterV = WoTWorldEntryDot(
		Center - Basis.Origin, Basis.TextureV) - LightMap.Pan.Y;
	const double RadiusU = PlaneRadius * FMath::Sqrt(
		WoTWorldEntryDot(Basis.TextureU, Basis.TextureU));
	const double RadiusV = PlaneRadius * FMath::Sqrt(
		WoTWorldEntryDot(Basis.TextureV, Basis.TextureV));
	for (double Value : {PlaneRadius, CenterU, CenterV, RadiusU, RadiusV})
		if (!FMath::IsFinite(Value)) return false;
	int64 MinU = 0, MinV = 0, MaxU = 0, MaxV = 0;
	if (!WoTWorldEntryRoundHalfEven(
			(CenterU - RadiusU) / LightMap.UScale, MinU)
		|| !WoTWorldEntryRoundHalfEven(
			(CenterV - RadiusV) / LightMap.VScale, MinV)
		|| !WoTWorldEntryRoundHalfEven(
			(CenterU + RadiusU) / LightMap.UScale, MaxU)
		|| !WoTWorldEntryRoundHalfEven(
			(CenterV + RadiusV) / LightMap.VScale, MaxV)) return false;
	Out.MinU = static_cast<int32>(FMath::Clamp<int64>(MinU, 0, LightMap.UClamp));
	Out.MinV = static_cast<int32>(FMath::Clamp<int64>(MinV, 0, LightMap.VClamp));
	Out.MaxU = static_cast<int32>(FMath::Clamp<int64>(MaxU, 0, LightMap.UClamp));
	Out.MaxV = static_cast<int32>(FMath::Clamp<int64>(MaxV, 0, LightMap.VClamp));
	return true;
}

void WoTWorldEntryBuildLightSqrt(TArray<float>& Out)
{
	Out.SetNumUninitialized(4096);
	for (int32 Index = 0; Index < 4096; ++Index)
	{
		const double S = FMath::Sqrt((static_cast<double>(Index) + 1.0) / 4096.0);
		Out[Index] = static_cast<float>(((2.0 * S - 3.0) * S * S + 1.0) / S);
	}
}

void WoTWorldEntryShadowMap(TConstArrayView<uint8> Bits, int32 MaskU,
	int32 VClamp, TArray<uint8>& Out)
{
	const int32 Width = MaskU * 8;
	TArray<int32> Source;
	TArray<int32> HorizontalA;
	TArray<int32> HorizontalB;
	Source.SetNumUninitialized(Width * VClamp);
	HorizontalA.SetNumUninitialized(Width * VClamp);
	HorizontalB.SetNumUninitialized(Width * VClamp);
	for (int32 V = 0; V < VClamp; ++V)
	{
		for (int32 U = 0; U < Width; ++U)
		{
			Source[V * Width + U] =
				(Bits[V * MaskU + U / 8] >> (U & 7)) & 1;
		}
	}
	for (int32 V = 0; V < VClamp; ++V)
	{
		for (int32 U = 0; U < Width; ++U)
		{
			const int32 Center = Source[V * Width + U];
			const int32 Left = Source[V * Width + FMath::Max(U - 1, 0)];
			const int32 Right = Source[V * Width + FMath::Min(U + 1, Width - 1)];
			HorizontalA[V * Width + U] =
				(24 * Left + 40 * Center + 24 * Right) * 255 / 320;
			HorizontalB[V * Width + U] =
				(40 * Left + 64 * Center + 40 * Right) * 255 / 320;
		}
	}
	TArray<int32> Destination;
	Destination.Init(0, Width * VClamp);
	int32 Destination0 = 0;
	int32 Destination1 = 0;
	int32 Destination2 = 1;
	for (int32 V = 0; V < VClamp; ++V)
	{
		for (int32 U = 0; U < Width; ++U)
		{
			if (Destination0 < VClamp)
				Destination[Destination0 * Width + U] += HorizontalA[V * Width + U];
			if (Destination1 < VClamp)
				Destination[Destination1 * Width + U] += HorizontalB[V * Width + U];
			if (Destination2 < VClamp)
				Destination[Destination2 * Width + U] += HorizontalA[V * Width + U];
		}
		++Destination0;
		++Destination1;
		++Destination2;
		if (V == 0) --Destination0;
		if (V == VClamp - 2) --Destination2;
	}
	Out.SetNumUninitialized(Width * VClamp);
	for (int32 Index = 0; Index < Out.Num(); ++Index)
		Out[Index] = static_cast<uint8>(Destination[Index]);
}

bool WoTWorldEntryIlluminate(const FWoTWorldEntryLight& Light,
	const FWoTWorldEntryBasis& Basis, const FWoTWorldEntryClipRect& Rect,
	TConstArrayView<uint8> Shadow, int32 ShadowWidth,
	TConstArrayView<float> LightSqrt, int32 UClamp, int32 VClamp,
	TArray<uint8>& Out)
{
	Out.Init(0, UClamp * VClamp);
	if (Rect.MaxU <= Rect.MinU || Rect.MaxV <= Rect.MinV) return true;
	const FVector3d LightDelta = Light.Location - Basis.Origin;
	const float Diffuse = static_cast<float>(
		FMath::Abs(WoTWorldEntryDot(LightDelta, Basis.Normal))
		* Light.ReciprocalRadiusExact);
	if (!FMath::IsFinite(Diffuse)) return false;
	const bool bRadialEffect =
		Light.Effect == EWoTWorldEntryLightEffect::None
		|| Light.Effect == EWoTWorldEntryLightEffect::TorchWaver
		|| Light.Effect == EWoTWorldEntryLightEffect::FireWaver
		|| Light.Effect == EWoTWorldEntryLightEffect::WateryShimmer;
	int64 Distance = 0, DistanceU = 0, DistanceV = 0;
	int64 DistanceUU = 0, DistanceVV = 0, DistanceUV = 0;
	if (bRadialEffect)
	{
		const FVector3d V0 = Basis.Base - Light.Location
			+ Basis.DU * Rect.MinU + Basis.DV * Rect.MinV;
		const double Scale =
			static_cast<double>(Light.ReciprocalRadiusMultiplier) * 4096.0;
		if (!WoTWorldEntryRoundHalfEven(
				WoTWorldEntryDot(V0, V0) * Scale, Distance)
			|| !WoTWorldEntryRoundHalfEven(
				WoTWorldEntryDot(V0, Basis.DU) * Scale, DistanceU)
			|| !WoTWorldEntryRoundHalfEven(
				WoTWorldEntryDot(V0, Basis.DV) * Scale, DistanceV)
			|| !WoTWorldEntryRoundHalfEven(
				WoTWorldEntryDot(Basis.DU, Basis.DU) * Scale, DistanceUU)
			|| !WoTWorldEntryRoundHalfEven(
				WoTWorldEntryDot(Basis.DV, Basis.DV) * Scale, DistanceVV)
			|| !WoTWorldEntryRoundHalfEven(
				WoTWorldEntryDot(Basis.DU, Basis.DV) * Scale, DistanceUV))
		{
			return false;
		}
	}
	const double Sine = static_cast<double>(static_cast<float>(
		1.0 - static_cast<double>(Light.Cone) / 256.0));
	const double ReciprocalSine = Sine != 1.0
		? static_cast<double>(static_cast<float>(1.0 / (1.0 - Sine)))
		: TNumericLimits<double>::Max();
	const double SineReciprocal = static_cast<double>(
		static_cast<float>(Sine * ReciprocalSine));
	const double SineSquared = static_cast<double>(static_cast<float>(Sine * Sine));
	for (int32 V = Rect.MinV; V < Rect.MaxV; ++V)
	{
		for (int32 U = Rect.MinU; U < Rect.MaxU; ++U)
		{
			const uint8 Source = Shadow[V * ShadowWidth + U];
			if (Source == 0) continue;
			double Value = 0.0;
			if (bRadialEffect)
			{
				const int64 LocalU = U - Rect.MinU;
				const int64 LocalV = V - Rect.MinV;
				uint64 Inner = static_cast<uint64>(Distance);
				Inner += 2ull * static_cast<uint64>(LocalU)
					* static_cast<uint64>(DistanceU);
				Inner += 2ull * static_cast<uint64>(LocalV)
					* static_cast<uint64>(DistanceV);
				Inner += static_cast<uint64>(LocalU) * static_cast<uint64>(LocalU)
					* static_cast<uint64>(DistanceUU);
				Inner += static_cast<uint64>(LocalV) * static_cast<uint64>(LocalV)
					* static_cast<uint64>(DistanceVV);
				Inner += 2ull * static_cast<uint64>(LocalU)
					* static_cast<uint64>(LocalV) * static_cast<uint64>(DistanceUV);
				const uint32 InnerDWord = static_cast<uint32>(Inner);
				if (InnerDWord >= 4096u * 4096u) continue;
				Value = static_cast<double>(Source) * static_cast<double>(Diffuse)
					* static_cast<double>(LightSqrt[InnerDWord >> 12]);
				int64 RoundedValue = 0;
				if (!WoTWorldEntryRoundHalfEven(Value, RoundedValue)) return false;
				Value = static_cast<double>(RoundedValue);
			}
			else
			{
				const FVector3d Position = Basis.Base - Light.Location
					+ Basis.DU * U + Basis.DV * V;
				const double SizeSquared = WoTWorldEntryDot(Position, Position);
				if (!FMath::IsFinite(SizeSquared)) return false;
				if (Light.Effect == EWoTWorldEntryLightEffect::Cylinder)
				{
					const double RadiusSquared = Position.X * Position.X
						+ Position.Y * Position.Y;
					if (!FMath::IsFinite(RadiusSquared)) return false;
					Value = FMath::FloorToDouble(static_cast<double>(Source)
						* (1.0 - RadiusSquared * FMath::Square(
							static_cast<double>(Light.ReciprocalRadius))));
				}
				else
				{
					int64 SqrtOffset = 0;
					if (!WoTWorldEntryRoundHalfEven(SizeSquared
						* static_cast<double>(Light.ReciprocalRadiusMultiplier),
						SqrtOffset)) return false;
					if (SqrtOffset < 0 || SqrtOffset >= 4096) continue;
					if (Light.Effect == EWoTWorldEntryLightEffect::NonIncidence)
					{
						Value = FMath::FloorToDouble(static_cast<double>(Source)
							* (1.02 - FMath::Sqrt(SizeSquared)
								* static_cast<double>(Light.ReciprocalRadius)));
					}
					else
					{
						const double ViewDot = WoTWorldEntryDot(Position, Light.View);
						if (ViewDot <= 0.0
							|| ViewDot * ViewDot <= SineSquared * SizeSquared) continue;
						const double Dot = FMath::Square(
							ViewDot * ReciprocalSine / FMath::Sqrt(SizeSquared)
							- SineReciprocal);
						Value = FMath::FloorToDouble(Dot * static_cast<double>(Source)
							* static_cast<double>(Diffuse)
							* static_cast<double>(LightSqrt[SqrtOffset]));
					}
				}
			}
			if (!FMath::IsFinite(Value)) return false;
			Out[V * UClamp + U] = static_cast<uint8>(
				FMath::Clamp(Value, 0.0, 255.0));
		}
	}
	return true;
}

struct FWoTWorldEntrySurfaceLightSpan
{
	int32 MaskU = 0;
	int32 MaskSpace = 0;
	int32 MaskOffset = 0;
};

bool WoTWorldEntrySurfaceLightSpan(const FWoTWorldLightMap& LightMap,
	int32 LightIndex, TConstArrayView<uint8> LightBits,
	FWoTWorldEntrySurfaceLightSpan& Out)
{
	if (LightIndex < 0 || LightMap.UClamp < 1 || LightMap.VClamp < 1)
		return false;
	const int32 MaskU = (LightMap.UClamp + 7) >> 3;
	const int32 MaskSpace = MaskU * LightMap.VClamp;
	const uint64 MaskOffset = static_cast<uint64>(LightMap.DataOffset)
		+ static_cast<uint64>(LightIndex) * static_cast<uint64>(MaskSpace);
	if (MaskOffset > static_cast<uint64>(LightBits.Num())
		|| static_cast<uint64>(MaskSpace)
			> static_cast<uint64>(LightBits.Num()) - MaskOffset)
	{
		return false;
	}
	Out.MaskU = MaskU;
	Out.MaskSpace = MaskSpace;
	Out.MaskOffset = static_cast<int32>(MaskOffset);
	return true;
}

// World surfaces and static-mover FPolys intentionally share the complete
// shadow-filter -> clip -> illuminate path. Only their surface basis, source
// model, and ambient-zone seed differ.
bool WoTWorldEntryComposeSurfaceLight(TConstArrayView<uint8> LightBits,
	const FWoTWorldLightMap& LightMap,
	const FWoTWorldEntrySurfaceLightSpan& Span,
	const FWoTWorldEntryLight& Light, const FWoTWorldEntryBasis& Basis,
	TConstArrayView<float> LightSqrt, TArray<uint8>& Out)
{
	TArray<uint8> Shadow;
	WoTWorldEntryShadowMap(TConstArrayView<uint8>(
		LightBits.GetData() + Span.MaskOffset, Span.MaskSpace),
		Span.MaskU, LightMap.VClamp, Shadow);
	FWoTWorldEntryClipRect Rect;
	return WoTWorldEntryClip(Light, Basis, LightMap, Rect)
		&& WoTWorldEntryIlluminate(Light, Basis, Rect, Shadow,
			Span.MaskU * 8, LightSqrt, LightMap.UClamp,
			LightMap.VClamp, Out);
}

void WoTWorldEntryAddMeanLight(const FWoTWorldEntryLight& Light,
	TConstArrayView<uint8> Illumination, TArray<FIntVector>& InOutTotals)
{
	check(Illumination.Num() == InOutTotals.Num());
	for (int32 Texel = 0; Texel < Illumination.Num(); ++Texel)
	{
		const int64 IlluminationByte = Illumination[Texel];
		InOutTotals[Texel].X = FMath::Min(InOutTotals[Texel].X
			+ static_cast<int32>((IlluminationByte * Light.FixedColor[0]) >> 16), 127);
		InOutTotals[Texel].Y = FMath::Min(InOutTotals[Texel].Y
			+ static_cast<int32>((IlluminationByte * Light.FixedColor[1]) >> 16), 127);
		InOutTotals[Texel].Z = FMath::Min(InOutTotals[Texel].Z
			+ static_cast<int32>((IlluminationByte * Light.FixedColor[2]) >> 16), 127);
	}
}

// Compose one light into every response endpoint. World surfaces and static
// movers must use this exact path for every admitted light, including steady
// lights: endpoint saturation happens before differencing, so a static light
// can legitimately reduce an animated response to zero.
void WoTWorldEntryAddResponseLight(const FWoTWorldEntryLight& Light,
	TConstArrayView<uint8> Illumination, float LevelBrightness,
	const FWoTWorldAnimationLight* Animation,
	TConstArrayView<FWoTWorldResponseAccumulator> Accumulators,
	TArray<TArray<FIntVector>>& InOutAccumulatorTotals)
{
	check(Accumulators.Num() == InOutAccumulatorTotals.Num());
	const int32 LightGroup = Animation ? Animation->Group : INDEX_NONE;
	const uint8 LightEffect = Animation ? Animation->Effect : 0;
	const FVector3f HSV = WoTWorldEntryHSV(
		Light.Hue, Light.Saturation, 255);
	const float BrightnessScale = static_cast<float>(Light.Brightness / 255.0f);
	const float Values[3] = {HSV.X, HSV.Y, HSV.Z};
	TArray<uint8> EffectIllumination;
	for (int32 AccumulatorIndex = 0;
		AccumulatorIndex < Accumulators.Num(); ++AccumulatorIndex)
	{
		const FWoTWorldResponseAccumulator& Accumulator =
			Accumulators[AccumulatorIndex];
		const bool bSelectedWave = Accumulator.Wave != 0
			&& LightGroup != INDEX_NONE && Accumulator.Group == LightGroup;
		const bool bSelectedEffect = Accumulator.EffectEnd != 0
			&& LightEffect != 0 && Accumulator.Effect == LightEffect;
		const double WaveScale = bSelectedWave
			? WoTWorldWaveMean(Light.LightType)
				+ static_cast<double>(Accumulator.Wave)
					* WoTWorldWaveAmplitude(Light.LightType)
			: WoTWorldWaveMean(Light.LightType);
		const double EffectScale = bSelectedEffect
			? 1.0 : WoTWorldEffectMean(static_cast<uint8>(Light.Effect));
		int64 Fixed[3];
		for (int32 Channel = 0; Channel < 3; ++Channel)
		{
			float FloatColor = static_cast<float>(Values[Channel] * BrightnessScale);
			FloatColor = static_cast<float>(FloatColor * LevelBrightness);
			FloatColor = static_cast<float>(
				FloatColor * static_cast<float>(WaveScale));
			FloatColor = static_cast<float>(
				FloatColor * static_cast<float>(EffectScale));
			Fixed[Channel] = static_cast<int64>(FMath::FloorToDouble(
				static_cast<double>(FloatColor) * 65536.0));
		}
		const TArray<uint8>* IlluminationSource = nullptr;
		if (bSelectedEffect)
		{
			const float Factor = static_cast<float>(
				WoTWorldEffectMean(LightEffect)
				+ static_cast<double>(Accumulator.EffectEnd)
					* WoTWorldEffectAmplitude(LightEffect));
			EffectIllumination.SetNumUninitialized(Illumination.Num());
			for (int32 Texel = 0; Texel < Illumination.Num(); ++Texel)
			{
				EffectIllumination[Texel] = static_cast<uint8>(
					FMath::FloorToFloat(
						static_cast<float>(Illumination[Texel]) * Factor));
			}
			IlluminationSource = &EffectIllumination;
		}
		TArray<FIntVector>& Sum = InOutAccumulatorTotals[AccumulatorIndex];
		check(Sum.Num() == Illumination.Num());
		for (int32 Texel = 0; Texel < Illumination.Num(); ++Texel)
		{
			const int64 IlluminationByte = bSelectedEffect
				? (*IlluminationSource)[Texel] : Illumination[Texel];
			Sum[Texel].X = FMath::Min(Sum[Texel].X
				+ static_cast<int32>((IlluminationByte * Fixed[0]) >> 16), 127);
			Sum[Texel].Y = FMath::Min(Sum[Texel].Y
				+ static_cast<int32>((IlluminationByte * Fixed[1]) >> 16), 127);
			Sum[Texel].Z = FMath::Min(Sum[Texel].Z
				+ static_cast<int32>((IlluminationByte * Fixed[2]) >> 16), 127);
		}
	}
}

bool WoTWorldEntryAmbientSeed(const FWoTWorldActor* Actor, FIntVector& Out)
{
	// UE1's Engine.ZoneInfo defaults saturation to
	// 255; brightness and hue are engine zero-init. Tools/convert/lightmaps.py
	// documents the same fallback used by the independent oracle.
	uint8 Brightness = 0;
	uint8 Hue = 0;
	uint8 Saturation = 255;
	if (Actor != nullptr
		&& (!WoTWorldEntryReadU8Property(
				*Actor, TEXT("AmbientBrightness"), 0, Brightness)
			|| !WoTWorldEntryReadU8Property(*Actor, TEXT("AmbientHue"), 0, Hue)
			|| !WoTWorldEntryReadU8Property(
				*Actor, TEXT("AmbientSaturation"), 255, Saturation)))
	{
		return false;
	}
	const FVector3f Color = WoTWorldEntryHSV(Hue, Saturation, Brightness);
	Out = FIntVector(
		FMath::Clamp(static_cast<int32>(FMath::FloorToDouble(
			static_cast<double>(Color.X) * 64.0)), 0, 255),
		FMath::Clamp(static_cast<int32>(FMath::FloorToDouble(
			static_cast<double>(Color.Y) * 64.0)), 0, 255),
		FMath::Clamp(static_cast<int32>(FMath::FloorToDouble(
			static_cast<double>(Color.Z) * 64.0)), 0, 255));
	return true;
}

int32 WoTWorldEntrySurfaceZone(const FWoTWorldModel& Model, int32 SurfaceIndex)
{
	int32 FrontCounts[WoTWorldMaxZones] = {};
	int32 BackCounts[WoTWorldMaxZones] = {};
	TArray<uint8> FrontOrder;
	TArray<uint8> BackOrder;
	for (const FWoTWorldNode& Node : Model.Nodes)
	{
		if (Node.Surface != SurfaceIndex) continue;
		for (int32 Side = 0; Side < 2; ++Side)
		{
			const uint8 Zone = Node.Zones[Side];
			if (Zone == 0 || Zone >= WoTWorldMaxZones) continue;
			int32* Counts = Side == 1 ? FrontCounts : BackCounts;
			TArray<uint8>& Order = Side == 1 ? FrontOrder : BackOrder;
			if (Counts[Zone] == 0) Order.Add(Zone);
			++Counts[Zone];
		}
	}
	for (int32 Pass = 0; Pass < 2; ++Pass)
	{
		const int32* Counts = Pass == 0 ? FrontCounts : BackCounts;
		const TArray<uint8>& Order = Pass == 0 ? FrontOrder : BackOrder;
		int32 BestZone = 0;
		int32 BestCount = 0;
		for (uint8 Zone : Order)
		{
			if (Counts[Zone] > BestCount)
			{
				BestZone = Zone;
				BestCount = Counts[Zone];
			}
		}
		if (BestZone != 0) return BestZone;
	}
	return 0;
}

bool WoTWorldEntryPointZone(const FWoTWorldModel& Model,
	const FVector3d& Point, int32& OutZone)
{
	OutZone = 0;
	if (Model.Nodes.IsEmpty() || Model.Zones.IsEmpty()) return true;
	int32 NodeIndex = 0;
	for (int32 Depth = 0; Depth <= Model.Nodes.Num(); ++Depth)
	{
		if (NodeIndex == INDEX_NONE) return true;
		if (!Model.Nodes.IsValidIndex(NodeIndex)) return false;
		const FWoTWorldNode& Node = Model.Nodes[NodeIndex];
		const double PlaneDistance = Point.X * Node.Plane[0]
			+ Point.Y * Node.Plane[1] + Point.Z * Node.Plane[2]
			- Node.Plane[3];
		if (!FMath::IsFinite(PlaneDistance)) return false;
		const bool bFront = PlaneDistance >= 0.0;
		OutZone = Node.Zones[bFront ? 1 : 0];
		NodeIndex = bFront ? Node.Front : Node.Back;
	}
	return false;
}

bool WoTWorldEntrySurfaceNeedsPerTexelZones(const FWoTWorldModel& Model,
	int32 SurfaceIndex, TConstArrayView<FIntVector> ZoneSeeds)
{
	TArray<FIntVector> DistinctSeeds;
	for (const FWoTWorldNode& Node : Model.Nodes)
	{
		if (Node.Surface != SurfaceIndex) continue;
		for (uint8 Zone : Node.Zones)
		{
			if (Zone == 0 || !ZoneSeeds.IsValidIndex(Zone)) continue;
			const FIntVector Seed = ZoneSeeds[Zone];
			if (!DistinctSeeds.Contains(Seed)) DistinctSeeds.Add(Seed);
			if (DistinctSeeds.Num() > 1) return true;
		}
	}
	return false;
}

bool WoTWorldEntrySurfacePanUV(const FWoTWorldSource& Source,
	const FWoTWorldModel& Model, int32 SurfaceIndex, FVector2f& Out)
{
	Out = FVector2f::ZeroVector;
	if (!Model.Surfaces.IsValidIndex(SurfaceIndex)) return false;
	const FWoTWorldSurface& Surface = Model.Surfaces[SurfaceIndex];
	if ((Surface.Flags & (WoTWorldPFAutoUPan | WoTWorldPFAutoVPan)) == 0)
		return true;
	const WoT::Retail::Generated::FWorldTexture* Texture =
		WoTWorldTexture(Source, Surface.Texture);
	const double TextureWidth = Texture != nullptr
		? static_cast<double>(Texture->Width) * Texture->DrawScale : 1.0;
	const double TextureHeight = Texture != nullptr
		? static_cast<double>(Texture->Height) * Texture->DrawScale : 1.0;
	if (!FMath::IsFinite(TextureWidth) || !FMath::IsFinite(TextureHeight)
		|| TextureWidth <= 0.0 || TextureHeight <= 0.0) return false;
	float USpeed = 1.0f;
	float VSpeed = 1.0f;
	const int32 ZoneIndex = WoTWorldEntrySurfaceZone(Model, SurfaceIndex);
	if (Model.Zones.IsValidIndex(ZoneIndex) && Model.Zones[ZoneIndex].Actor != 0)
	{
		const int32 ZoneReference = Model.Zones[ZoneIndex].Actor;
		const FWoTWorldActor* ZoneActor = nullptr;
		for (const FWoTWorldActor& Actor : Source.Actors)
		{
			if (Actor.ExportReference == ZoneReference)
			{
				ZoneActor = &Actor;
				break;
			}
		}
		if (ZoneActor != nullptr
			&& (!WoTWorldEntryReadFloatProperty(
					*ZoneActor, TEXT("TexUPanSpeed"), 1.0f, USpeed)
				|| !WoTWorldEntryReadFloatProperty(
					*ZoneActor, TEXT("TexVPanSpeed"), 1.0f, VSpeed)))
		{
			return false;
		}
	}
	if ((Surface.Flags & WoTWorldPFAutoUPan) != 0)
		Out.X = static_cast<float>(35.0 * static_cast<double>(USpeed) / TextureWidth);
	if ((Surface.Flags & WoTWorldPFAutoVPan) != 0)
		Out.Y = static_cast<float>(35.0 * static_cast<double>(VSpeed) / TextureHeight);
	return FMath::IsFinite(Out.X) && FMath::IsFinite(Out.Y)
		&& FMath::Abs(Out.X) <= 1024.0f && FMath::Abs(Out.Y) <= 1024.0f;
}

FWoTBootstrapStatus WoTWorldComposeAtlas(const FWoTWorldSource& Source,
	const FWoTWorldModel& Model, FWoTWorldAtlas& Atlas,
	TArray<FWoTWorldEntryLight>& OutLightningLights,
	TArray<FWoTWorldEntryIllumination>& OutWeatherPairs,
	float& OutLevelBrightness, FWoTWorldLightmapComposeStats& OutStats,
	FWoTWorldAnimationManifest& OutManifest,
	TArray<FWoTWorldResponseTerm>& OutResponses)
{
	// Authenticated native port of
	// Tools/convert/lightmaps.py + lightmap_atlas.py. It deliberately consumes
	// the authored masks/effects instead of substituting modern point lights.
	auto Error = [&Source](const TCHAR* Message, FString Path = FString())
	{
		return FWoTBootstrapStatus::Error(WoTWorldError(
			EWoTBootstrapError::CorruptInstallation, Message,
			Path.IsEmpty() ? Source.SourcePath : MoveTemp(Path)));
	};
	const bool bEntryLighting = Source.MapIdentity == TEXT("WheelOfTime");
	const FWoTWorldActor* LevelInfo = nullptr;
	TMap<int32, const FWoTWorldActor*> ActorsByReference;
	TSet<FString> LightningTags;
	for (const FWoTWorldActor& Actor : Source.Actors)
	{
		ActorsByReference.Add(Actor.ExportReference, &Actor);
		const FString LeafClass = WoTWorldActorLeafClass(Actor);
		if (LeafClass == TEXT("LevelInfo")) LevelInfo = &Actor;
		if (LeafClass == TEXT("Lightning"))
		{
			FString LightTag;
			if (!WoTWorldEntryReadNameProperty(Actor, TEXT("LightTag"), LightTag))
				return Error(TEXT("an entry Lightning actor has an invalid LightTag"),
					Actor.Identity);
			if (!LightTag.IsEmpty() && LightTag != TEXT("None"))
				LightningTags.Add(WoTWorldFold(LightTag));
		}
	}
	if (LevelInfo == nullptr)
		return Error(TEXT("the map has no LevelInfo lighting context"));
	// LevelInfo.Brightness uses the shipped/runtime 1.0 fallback cited by the
	// independent Python reference.
	float LevelBrightness = 1.0f;
	if (!WoTWorldEntryReadFloatProperty(
			*LevelInfo, TEXT("Brightness"), 1.0f, LevelBrightness)
		|| LevelBrightness < 0.0f || LevelBrightness > 1024.0f)
	{
		return Error(TEXT("the LevelInfo brightness is invalid"),
			LevelInfo->Identity);
	}
	OutLevelBrightness = LevelBrightness;
	TMap<int32, FWoTWorldEntryLight> EntryLights;
	int32 EffectCounts[4] = {};
	if (bEntryLighting)
	{
		for (const FWoTWorldActor& Actor : Source.Actors)
		{
			if (WoTWorldActorLeafClass(Actor) != TEXT("Light")) continue;
			FWoTWorldEntryLight Light;
			if (!WoTWorldResolveEntryLight(Actor, LevelBrightness, Light)
				|| !WoTWorldSupportedLightType(Light.LightType)
				|| !WoTWorldSupportedLightEffect(static_cast<uint8>(Light.Effect)))
			{
				return Error(TEXT("an entry Light uses malformed or unsupported semantics"),
					Actor.Identity);
			}
			// Lightning.uc disables actors whose Tag matches Lightning.LightTag on
			// its initial tick, even though the editor retained them in light lists.
			Light.bInitiallyOff = LightningTags.Contains(WoTWorldFold(Light.Tag));
			switch (Light.Effect)
			{
			case EWoTWorldEntryLightEffect::None: ++EffectCounts[0]; break;
			case EWoTWorldEntryLightEffect::NonIncidence: ++EffectCounts[1]; break;
			case EWoTWorldEntryLightEffect::Cylinder: ++EffectCounts[2]; break;
			case EWoTWorldEntryLightEffect::StaticSpot: ++EffectCounts[3]; break;
			default:
				return Error(TEXT("an entry Light effect is unsupported"), Actor.Identity);
			}
			EntryLights.Add(Actor.ExportReference, MoveTemp(Light));
		}
		if (EntryLights.Num() != 39 || EffectCounts[0] != 13
			|| EffectCounts[1] != 24 || EffectCounts[2] != 1
			|| EffectCounts[3] != 1)
		{
			return Error(TEXT("the entry Light census disagrees with the authenticated map"));
		}
	}
	TArray<TPair<int32, FWoTWorldEntryLight>> OrderedLightning;
	if (bEntryLighting)
	{
		for (const TPair<int32, FWoTWorldEntryLight>& Pair : EntryLights)
		{
			if (Pair.Value.bInitiallyOff) OrderedLightning.Add(Pair);
		}
	}
	OrderedLightning.Sort([](const TPair<int32, FWoTWorldEntryLight>& A,
		const TPair<int32, FWoTWorldEntryLight>& B)
	{
		return WoTWorldCompareUTF8(A.Value.Identity, B.Value.Identity) < 0;
	});
	if (bEntryLighting && OrderedLightning.Num() != 5)
		return Error(TEXT("the entry Lightning routing does not select five lights"));
	TMap<int32, uint32> LightningIndexByReference;
	for (int32 Index = 0; Index < OrderedLightning.Num(); ++Index)
	{
		if (Index > 0 && OrderedLightning[Index - 1].Value.Identity.Equals(
			OrderedLightning[Index].Value.Identity, ESearchCase::IgnoreCase))
		{
			return Error(TEXT("the entry Lightning light identities are not unique"));
		}
		LightningIndexByReference.Add(
			OrderedLightning[Index].Key, static_cast<uint32>(Index));
		OutLightningLights.Add(OrderedLightning[Index].Value);
	}
	TArray<FIntVector> ZoneSeeds;
	const int32 ZoneSeedCount = FMath::Max(Model.Zones.Num(), 1);
	ZoneSeeds.SetNumUninitialized(ZoneSeedCount);
	for (int32 ZoneIndex = 0; ZoneIndex < ZoneSeedCount; ++ZoneIndex)
	{
		const FWoTWorldActor* ZoneActor = LevelInfo;
		if (Model.Zones.IsValidIndex(ZoneIndex) && Model.Zones[ZoneIndex].Actor != 0)
		{
			if (const FWoTWorldActor* const* Found =
				ActorsByReference.Find(Model.Zones[ZoneIndex].Actor)) ZoneActor = *Found;
		}
		if (!WoTWorldEntryAmbientSeed(ZoneActor, ZoneSeeds[ZoneIndex]))
		{
			return Error(TEXT("an entry zone has invalid ambient lighting"),
				ZoneActor != nullptr ? ZoneActor->Identity : FString());
		}
	}
	const uint64 AtlasPixels = static_cast<uint64>(Atlas.Width) * Atlas.Height;
	const uint64 AtlasBytes = AtlasPixels * 4;
	if (Atlas.Width < 1 || Atlas.Width > 8192 || Atlas.Height < 1
		|| Atlas.Height > 8192 || AtlasPixels > 4194304ull
		|| AtlasBytes > static_cast<uint64>(MAX_int32))
	{
		return Error(TEXT("the entry light-map atlas exceeds its wire bounds"));
	}
	Atlas.BGRA.Init(0, static_cast<int32>(AtlasBytes));
	for (uint64 Pixel = 0; Pixel < AtlasPixels; ++Pixel)
		Atlas.BGRA[static_cast<int32>(Pixel * 4 + 3)] = 255;
	TArray<float> LightSqrt;
	WoTWorldEntryBuildLightSqrt(LightSqrt);
	FWoTWorldEntryPairBudget WeatherBudget;
	// Multiplayer maps carry the animated light-map responses; the entry world
	// keeps its own Lightning trailer and no wave manifest.
	OutManifest = FWoTWorldAnimationManifest();
	OutResponses.Reset();
	if (!bEntryLighting)
	{
		FWoTBootstrapStatus ManifestStatus =
			WoTWorldBuildAnimationManifest(Source, LightningTags, OutManifest);
		if (ManifestStatus.IsError()) return ManifestStatus;
	}
	TArray<FWoTWorldResponseAccumulator> Accumulators;
	TArray<TArray<int32>> AccumulatorsByTerm;
	WoTWorldResponseAccumulators(OutManifest, Accumulators, AccumulatorsByTerm);
	OutResponses.SetNum(OutManifest.Terms.Num());
	uint64 ResponseBytes = 0;
	for (int32 SurfaceIndex = 0; SurfaceIndex < Model.Surfaces.Num(); ++SurfaceIndex)
	{
		const FWoTWorldSurface& Surface = Model.Surfaces[SurfaceIndex];
		if (Surface.LightMap < 0) continue;
		if (!Model.LightMaps.IsValidIndex(Surface.LightMap)
			|| !Atlas.Surfaces.IsValidIndex(SurfaceIndex)
			|| !Atlas.Surfaces[SurfaceIndex].bPresent)
		{
			return Error(TEXT("an entry surface has no bounded atlas light-map"));
		}
		const FWoTWorldLightMap& LightMap = Model.LightMaps[Surface.LightMap];
		const FWoTWorldAtlasEntry& Placement = Atlas.Surfaces[SurfaceIndex];
		const int32 TexelCount = LightMap.UClamp * LightMap.VClamp;
		FWoTWorldEntryBasis Basis;
		if (!WoTWorldEntrySurfaceBasis(Model, Surface, LightMap, Basis))
			return Error(TEXT("an entry surface light-map basis is singular"));
		int32 Zone = WoTWorldEntrySurfaceZone(Model, SurfaceIndex);
		if (!ZoneSeeds.IsValidIndex(Zone)) Zone = 0;
		TArray<FIntVector> Totals;
		if (WoTWorldEntrySurfaceNeedsPerTexelZones(
				Model, SurfaceIndex, ZoneSeeds))
		{
			Totals.SetNumUninitialized(TexelCount);
			for (int32 V = 0; V < LightMap.VClamp; ++V)
			{
				const FVector3d Row = Basis.Base + Basis.DV * V + Basis.Normal * 2.0;
				for (int32 U = 0; U < LightMap.UClamp; ++U)
				{
					int32 TexelZone = 0;
					if (!WoTWorldEntryPointZone(
							Model, Row + Basis.DU * U, TexelZone))
					{
						return Error(TEXT("a BSP per-texel zone walk is malformed"));
					}
					Totals[V * LightMap.UClamp + U] =
						TexelZone > 0 && ZoneSeeds.IsValidIndex(TexelZone)
						? ZoneSeeds[TexelZone] : ZoneSeeds[Zone];
				}
			}
			++OutStats.PerTexelZoneSurfaceCount;
		}
		else
		{
			Totals.Init(ZoneSeeds[Zone], TexelCount);
		}
		TArray<TArray<FIntVector>> AccumulatorTotals;
		AccumulatorTotals.Reserve(Accumulators.Num());
		for (int32 Index = 0; Index < Accumulators.Num(); ++Index)
			AccumulatorTotals.Add(Totals);
		int32 SurfaceLightCount = 0;
		if (LightMap.LightActors >= 0)
		{
			int32 Cursor = LightMap.LightActors;
			while (Model.Lights.IsValidIndex(Cursor)
				&& Model.Lights[Cursor] != 0)
			{
				++SurfaceLightCount;
				++Cursor;
			}
			if (!Model.Lights.IsValidIndex(Cursor) || Model.Lights[Cursor] != 0)
				return Error(TEXT("an entry surface light list has no null terminator"));
		}
		for (int32 LightIndex = 0; LightIndex < SurfaceLightCount; ++LightIndex)
		{
			const int32 Reference = Model.Lights[LightMap.LightActors + LightIndex];
			FWoTWorldEntryLight* Light = EntryLights.Find(Reference);
			if (Light == nullptr)
			{
				const FWoTWorldActor* const* Actor = ActorsByReference.Find(Reference);
				if (Actor == nullptr)
				{
					OutStats.Skip(WoTWorldClassifyLightSkip(false, false,
						false, 0, 0, 0, true));
					continue;
				}
				FWoTWorldLightDefaults ReviewedDefaults;
				if (!WoTWorldLightDefaults(
						WoTWorldActorLeafClass(**Actor), ReviewedDefaults))
				{
					OutStats.Skip(WoTWorldClassifyLightSkip(true, false,
						false, 0, 0, 0, true));
					continue;
				}
				FWoTWorldEntryLight Resolved;
				if (!WoTWorldResolveEntryLight(**Actor, LevelBrightness, Resolved))
				{
					return Error(TEXT("a referenced light has malformed properties"),
						(*Actor)->Identity);
				}
				Resolved.bInitiallyOff =
					LightningTags.Contains(WoTWorldFold(Resolved.Tag));
				EntryLights.Add(Reference, MoveTemp(Resolved));
				Light = EntryLights.Find(Reference);
			}
			FWoTWorldEntrySurfaceLightSpan LightSpan;
			const bool bMaskInRange = WoTWorldEntrySurfaceLightSpan(
				LightMap, LightIndex, Model.LightBits, LightSpan);
			if (!bEntryLighting)
			{
				const FString SkipReason = WoTWorldClassifyLightSkip(true, true,
					Light->bInitiallyOff, Light->LightType, Light->Brightness,
					static_cast<uint8>(Light->Effect), bMaskInRange);
				if (!SkipReason.IsEmpty())
				{
					OutStats.Skip(SkipReason);
					continue;
				}
			}
			else if (Light->Brightness == 0)
			{
				OutStats.Skip(TEXT("brightness_0"));
				continue;
			}
			if (!bMaskInRange)
			{
				if (bEntryLighting)
					return Error(TEXT("an entry surface shadow mask is truncated"));
				return Error(TEXT("a multiplayer shadow mask skip was not classified"));
			}
			uint32 OrderedLightningIndex = MAX_uint32;
			if (Light->bInitiallyOff)
			{
				const uint32* OrderedIndex = LightningIndexByReference.Find(Reference);
				if (OrderedIndex == nullptr)
					return Error(TEXT("an entry Lightning light has no ordered index"),
						Light->Identity);
				OrderedLightningIndex = *OrderedIndex;
				if (!WeatherBudget.Reserve(OrderedLightningIndex,
						static_cast<uint32>(SurfaceIndex),
						static_cast<uint32>(TexelCount)))
				{
					return Error(TEXT("the entry Lightning routing exceeds its preflight bounds"),
						Light->Identity);
				}
			}
			TArray<uint8> Illumination;
			if (!WoTWorldEntryComposeSurfaceLight(Model.LightBits, LightMap,
					LightSpan, *Light, Basis, LightSqrt, Illumination))
			{
				return Error(TEXT("an entry Light calculation exceeds numeric bounds"),
					Light->Identity);
			}
			if (Light->bInitiallyOff)
			{
				FWoTWorldEntryIllumination& Weather =
					OutWeatherPairs.AddDefaulted_GetRef();
				Weather.LightIndex = OrderedLightningIndex;
				Weather.SurfaceIndex = static_cast<uint32>(SurfaceIndex);
				Weather.Illumination = MoveTemp(Illumination);
				continue;
			}
			WoTWorldEntryAddMeanLight(*Light, Illumination, Totals);
			if (!Accumulators.IsEmpty())
			{
				// compose_map(wave=..., wave_group=..., effect=..., effect_class=...):
				// the selected group's lights at base+/-amp; the selected shimmer
				// class floored into the illumination bytes (UE1 FLightManager::Merge
				// ordering) with its FloatColor factor at exactly 1.0.
				const FWoTWorldAnimationLight* Animation =
					OutManifest.ByReference.Find(Reference);
				WoTWorldEntryAddResponseLight(*Light, Illumination,
					LevelBrightness, Animation, Accumulators, AccumulatorTotals);
			}
		}
		for (int32 TermIndex = 0; TermIndex < OutManifest.Terms.Num(); ++TermIndex)
		{
			// bake_animation_responses: waveform/effect (hi-lo)/2, intersection
			// (hh-hl-lh+ll)/4, encoded unsigned at scale 2 (lightmap_atlas.py
			// build_atlas: min(response*2, 255) truncated). A signed response is
			// the reference's ValueError; fail closed the same way.
			const TArray<int32>& Slots = AccumulatorsByTerm[TermIndex];
			TArray<uint8> RGB;
			RGB.SetNumUninitialized(TexelCount * 3);
			bool bAnyNonZero = false;
			for (int32 Texel = 0; Texel < TexelCount; ++Texel)
			{
				int32 Delta[3];
				if (Slots.Num() == 2)
				{
					const FIntVector& Hi = AccumulatorTotals[Slots[0]][Texel];
					const FIntVector& Lo = AccumulatorTotals[Slots[1]][Texel];
					Delta[0] = Hi.X - Lo.X; Delta[1] = Hi.Y - Lo.Y; Delta[2] = Hi.Z - Lo.Z;
				}
				else
				{
					const FIntVector& HH = AccumulatorTotals[Slots[0]][Texel];
					const FIntVector& HL = AccumulatorTotals[Slots[1]][Texel];
					const FIntVector& LH = AccumulatorTotals[Slots[2]][Texel];
					const FIntVector& LL = AccumulatorTotals[Slots[3]][Texel];
					const int32 N[3] = {HH.X - HL.X - LH.X + LL.X,
						HH.Y - HL.Y - LH.Y + LL.Y, HH.Z - HL.Z - LH.Z + LL.Z};
					for (int32 Channel = 0; Channel < 3; ++Channel)
					{
						if (N[Channel] < 0)
							return Error(TEXT("a light-map intersection response is signed"));
						Delta[Channel] = N[Channel] / 2;
					}
				}
				for (int32 Channel = 0; Channel < 3; ++Channel)
				{
					if (Delta[Channel] < 0)
						return Error(TEXT("a light-map waveform response is signed"));
					const uint8 Encoded = static_cast<uint8>(FMath::Min(Delta[Channel], 255));
					RGB[Texel * 3 + Channel] = Encoded;
					bAnyNonZero = bAnyNonZero || Encoded != 0;
				}
			}
			if (bAnyNonZero)
			{
				ResponseBytes += static_cast<uint64>(RGB.Num());
				if (ResponseBytes > WoTWorldMaxResponseBytes)
					return Error(TEXT("the light-map responses exceed their wire bound"));
				FWoTWorldResponseRect& Rect = OutResponses[TermIndex].Rects.AddDefaulted_GetRef();
				Rect.SurfaceIndex = SurfaceIndex;
				Rect.RGB = MoveTemp(RGB);
			}
		}
		for (int32 PaddedV = -1; PaddedV <= LightMap.VClamp; ++PaddedV)
		{
			const int32 SourceV = FMath::Clamp(PaddedV, 0, LightMap.VClamp - 1);
			for (int32 PaddedU = -1; PaddedU <= LightMap.UClamp; ++PaddedU)
			{
				const int32 SourceU = FMath::Clamp(PaddedU, 0, LightMap.UClamp - 1);
				const FIntVector& RGB = Totals[SourceV * LightMap.UClamp + SourceU];
				const int32 AtlasX = Placement.X + PaddedU;
				const int32 AtlasY = Placement.Y + PaddedV;
				const int32 ByteOffset = (AtlasY * Atlas.Width + AtlasX) * 4;
				Atlas.BGRA[ByteOffset] = static_cast<uint8>(FMath::Min(RGB.Z * 2, 255));
				Atlas.BGRA[ByteOffset + 1] =
					static_cast<uint8>(FMath::Min(RGB.Y * 2, 255));
				Atlas.BGRA[ByteOffset + 2] =
					static_cast<uint8>(FMath::Min(RGB.X * 2, 255));
			}
		}
	}
	OutWeatherPairs.Sort([](const FWoTWorldEntryIllumination& A,
		const FWoTWorldEntryIllumination& B)
	{
		return A.LightIndex == B.LightIndex
			? A.SurfaceIndex < B.SurfaceIndex : A.LightIndex < B.LightIndex;
	});
	if (OutWeatherPairs.Num() > static_cast<int32>(WoTWorldMaxEntryPairs)
		|| OutWeatherPairs.Num() != WeatherBudget.Num())
		return Error(TEXT("the entry Lightning pair count exceeds its wire bound"));
	uint64 PairBytes = 0;
	TArray<bool> RoutedLights;
	RoutedLights.Init(false, OutLightningLights.Num());
	for (int32 Index = 0; Index < OutWeatherPairs.Num(); ++Index)
	{
		const FWoTWorldEntryIllumination& Pair = OutWeatherPairs[Index];
		if (Index > 0
			&& Pair.LightIndex == OutWeatherPairs[Index - 1].LightIndex
			&& Pair.SurfaceIndex == OutWeatherPairs[Index - 1].SurfaceIndex)
		{
			return Error(TEXT("the entry Lightning routing contains a duplicate pair"));
		}
		if (!RoutedLights.IsValidIndex(static_cast<int32>(Pair.LightIndex))
			|| !Atlas.Surfaces.IsValidIndex(static_cast<int32>(Pair.SurfaceIndex))
			|| !Atlas.Surfaces[Pair.SurfaceIndex].bPresent)
		{
			return Error(TEXT("the entry Lightning routing targets invalid provenance"));
		}
		const FWoTWorldAtlasEntry& Placement = Atlas.Surfaces[Pair.SurfaceIndex];
		const uint64 ExpectedBytes = static_cast<uint64>(Placement.W) * Placement.H;
		if (ExpectedBytes != static_cast<uint64>(Pair.Illumination.Num()))
			return Error(TEXT("the entry Lightning illumination size is inconsistent"));
		if (PairBytes > WoTWorldMaxEntryPairBytes - ExpectedBytes)
			return Error(TEXT("the entry Lightning work exceeds its wire bound"));
		PairBytes += ExpectedBytes;
		RoutedLights[Pair.LightIndex] = true;
	}
	if (PairBytes != WeatherBudget.GetPairBytes())
		return Error(TEXT("the entry Lightning work disagrees with preflight"));
	for (bool bRouted : RoutedLights)
		if (!bRouted) return Error(TEXT("an entry Lightning light has no routed surface"));
	for (int32 V = 0; V < 3; ++V)
	{
		for (int32 U = 0; U < 3; ++U)
		{
			const int32 ByteOffset = ((Atlas.FullbrightOuter.Y + V) * Atlas.Width
				+ Atlas.FullbrightOuter.X + U) * 4;
			Atlas.BGRA[ByteOffset] = 128;
			Atlas.BGRA[ByteOffset + 1] = 128;
			Atlas.BGRA[ByteOffset + 2] = 128;
		}
	}
	return FWoTBootstrapStatus::Success();
}

struct FWoTWorldMoverPose
{
	FVector3d Location = FVector3d::ZeroVector;
	FIntVector Rotation = FIntVector::ZeroValue;
	FVector3d PrePivot = FVector3d::ZeroVector;
	FVector3d MainScale = FVector3d(1.0, 1.0, 1.0);
	FVector3d PostScale = FVector3d(1.0, 1.0, 1.0);
};

bool WoTWorldMoverReadOptionalVector(const FWoTWorldActor& Actor,
	FStringView Name, FVector3d& InOut)
{
	const FWoTTypedProperty* Property = WoTWorldFindProperty(Actor, Name);
	if (Property == nullptr) return true;
	if (Property->Type != 10 || Property->ArrayIndex != 0
		|| !Property->StructName.Equals(TEXT("Vector"), ESearchCase::IgnoreCase)
		|| Property->Value.Num() != 12) return false;
	float Values[3] = {};
	FMemory::Memcpy(Values, Property->Value.GetData(), sizeof(Values));
	for (float Value : Values)
		if (!FMath::IsFinite(Value)) return false;
	InOut = FVector3d(Values[0], Values[1], Values[2]);
	return true;
}

bool WoTWorldMoverReadScale(const FWoTWorldActor& Actor,
	FStringView Name, FVector3d& InOut)
{
	const FWoTTypedProperty* Property = WoTWorldFindProperty(Actor, Name);
	if (Property == nullptr) return true;
	if (Property->Type != 10 || Property->ArrayIndex != 0
		|| !Property->StructName.Equals(TEXT("Scale"), ESearchCase::IgnoreCase)
		|| Property->Value.Num() != 17) return false;
	float Values[4] = {};
	FMemory::Memcpy(Values, Property->Value.GetData(), sizeof(Values));
	for (float Value : Values)
		if (!FMath::IsFinite(Value)) return false;
	if (Values[3] != 0.0f) return false;
	InOut = FVector3d(Values[0], Values[1], Values[2]);
	return true;
}

bool WoTWorldMoverReadPose(const FWoTWorldActor& Actor,
	FWoTWorldMoverPose& Out)
{
	if (!WoTWorldMoverReadOptionalVector(Actor, TEXT("Location"), Out.Location))
		return false;
	if (WoTWorldFindProperty(Actor, TEXT("BasePos")) != nullptr
		&& !WoTWorldMoverReadOptionalVector(Actor, TEXT("BasePos"), Out.Location))
		return false;
	if (!WoTWorldEntryReadRotatorProperty(Actor, TEXT("BaseRot"), Out.Rotation)
		|| !WoTWorldMoverReadOptionalVector(
			Actor, TEXT("PrePivot"), Out.PrePivot)
		|| !WoTWorldMoverReadScale(Actor, TEXT("MainScale"), Out.MainScale)
		|| !WoTWorldMoverReadScale(Actor, TEXT("PostScale"), Out.PostScale))
	{
		return false;
	}
	for (double* Component : {&Out.Location.X, &Out.Location.Y, &Out.Location.Z,
		&Out.PrePivot.X, &Out.PrePivot.Y, &Out.PrePivot.Z,
		&Out.MainScale.X, &Out.MainScale.Y, &Out.MainScale.Z,
		&Out.PostScale.X, &Out.PostScale.Y, &Out.PostScale.Z})
	{
		*Component = WoTWorldProjectT3DLocation(*Component);
	}
	return Out.MainScale.X != 0.0 && Out.MainScale.Y != 0.0
		&& Out.MainScale.Z != 0.0 && Out.PostScale.X != 0.0
		&& Out.PostScale.Y != 0.0 && Out.PostScale.Z != 0.0;
}

void WoTWorldMoverRotationMatrix(const FIntVector& Rotation,
	double (&Out)[3][3])
{
	auto SinTable = [](int64 Angle)
	{
		const uint32 Index = static_cast<uint32>(
			(static_cast<uint64>(Angle) >> 2) & 16383ull);
		return FMath::Sin(static_cast<double>(Index)
			* (2.0 * UE_DOUBLE_PI / 16384.0));
	};
	const double CP = SinTable(static_cast<int64>(Rotation.X) + 16384);
	const double SP = SinTable(Rotation.X);
	const double CY = SinTable(static_cast<int64>(Rotation.Y) + 16384);
	const double SY = SinTable(Rotation.Y);
	const double CR = SinTable(static_cast<int64>(Rotation.Z) + 16384);
	const double SR = SinTable(Rotation.Z);
	Out[0][0] = CP * CY;
	Out[1][0] = CP * SY;
	Out[2][0] = SP;
	Out[0][1] = SR * SP * CY - CR * SY;
	Out[1][1] = SR * SP * SY + CR * CY;
	Out[2][1] = -SR * CP;
	Out[0][2] = -(CR * SP * CY + SR * SY);
	Out[1][2] = CY * SR - CR * SP * SY;
	Out[2][2] = CR * CP;
}

FVector3d WoTWorldMoverRotate(const double (&Rotation)[3][3],
	const FVector3d& Value)
{
	return FVector3d(
		Rotation[0][0] * Value.X + Rotation[0][1] * Value.Y
			+ Rotation[0][2] * Value.Z,
		Rotation[1][0] * Value.X + Rotation[1][1] * Value.Y
			+ Rotation[1][2] * Value.Z,
		Rotation[2][0] * Value.X + Rotation[2][1] * Value.Y
			+ Rotation[2][2] * Value.Z);
}

bool WoTWorldMoverTransformPoly(const FWoTWorldPoly& Poly,
	const FWoTWorldMoverPose& Pose, FVector3d& OutBase,
	FVector3d& OutTextureU, FVector3d& OutTextureV, FVector3d& OutNormal)
{
	double Rotation[3][3];
	WoTWorldMoverRotationMatrix(Pose.Rotation, Rotation);
	auto Point = [&Pose, &Rotation](const FVector3f& Value)
	{
		const FVector3d Local = (FVector3d(Value) - Pose.PrePivot)
			* Pose.MainScale;
		return WoTWorldMoverRotate(Rotation, Local) * Pose.PostScale
			+ Pose.Location;
	};
	auto Vector = [&Pose, &Rotation](const FVector3f& Value)
	{
		const FVector3d Local = FVector3d(Value) / Pose.MainScale;
		return WoTWorldMoverRotate(Rotation, Local) / Pose.PostScale;
	};
	OutBase = Point(Poly.Base);
	OutTextureU = Vector(Poly.TextureU);
	OutTextureV = Vector(Poly.TextureV);
	OutNormal = Vector(Poly.Normal);
	const double NormalLength = OutNormal.Length();
	if (!FMath::IsFinite(NormalLength) || NormalLength <= 0.0) return false;
	OutNormal /= NormalLength;
	return FMath::IsFinite(OutBase.X) && FMath::IsFinite(OutBase.Y)
		&& FMath::IsFinite(OutBase.Z) && FMath::IsFinite(OutTextureU.X)
		&& FMath::IsFinite(OutTextureU.Y) && FMath::IsFinite(OutTextureU.Z)
		&& FMath::IsFinite(OutTextureV.X) && FMath::IsFinite(OutTextureV.Y)
		&& FMath::IsFinite(OutTextureV.Z);
}

FWoTBootstrapStatus WoTWorldComposeMoverAtlas(FWoTWorldSource& Source,
	const FWoTWorldModel& WorldModel, TArray<FWoTWorldMoverModel>& Movers,
	FWoTWorldAtlas& Atlas, FWoTWorldAnimationManifest& OutManifest,
	TArray<FWoTWorldResponseTerm>& OutResponses)
{
	auto Error = [&Source](const TCHAR* Message, FString Path = FString())
	{
		return FWoTBootstrapStatus::Error(WoTWorldError(
			EWoTBootstrapError::CorruptInstallation, Message,
			Path.IsEmpty() ? Source.SourcePath : MoveTemp(Path)));
	};
	Source.StaticLightmappedMoverCount = 0;
	Source.MoverLitPolyCount = 0;
	Source.MoverRenderTriangleCount = 0;
	Source.MoverCollisionTriangleCount = 0;
	Source.MoverUV1OutsidePaddedRectCount = 0;
	Source.MoverPolyOwnershipViolationCount = 0;
	Source.MoverAnimatedLightPolyCount = 0;
	Source.MoverOracleRGB7.Reset();
	TArray<FIntPoint> Clamps;
	for (FWoTWorldMoverModel& Mover : Movers)
	{
		Mover.PolyAtlasOffset = Clamps.Num();
		Clamps.AddZeroed(Mover.Polys.Num());
		Source.MoverRenderTriangleCount += Mover.Indices.Num() / 3;
		Source.MoverCollisionTriangleCount += Mover.CollisionIndices.Num() / 3;
		Source.MoverPolyOwnershipViolationCount +=
			Mover.PolyOwnershipViolationCount;
		if (!Mover.bLightmapped) continue;
		++Source.StaticLightmappedMoverCount;
		for (int32 PolyIndex = 0; PolyIndex < Mover.Polys.Num(); ++PolyIndex)
		{
			const int32 LightMapIndex = Mover.Polys[PolyIndex].BrushPoly;
			if (!Mover.BrushModel.LightMaps.IsValidIndex(LightMapIndex)) continue;
			const FWoTWorldLightMap& LightMap =
				Mover.BrushModel.LightMaps[LightMapIndex];
			Clamps[Mover.PolyAtlasOffset + PolyIndex] =
				FIntPoint(LightMap.UClamp, LightMap.VClamp);
			++Mover.LitPolyCount;
			++Source.MoverLitPolyCount;
		}
	}
	const FIntPoint Sizes[] = {{256, 256}, {512, 512}, {1024, 1024},
		{2048, 2048}, {4096, 4096}};
	if (!WoTWorldPackAtlas(Clamps, MakeArrayView(Sizes), Atlas))
		return Error(TEXT("the mover light-map atlas does not fit its fixed sizes"));
	Source.MoverAtlasWidth = Atlas.Width;
	Source.MoverAtlasHeight = Atlas.Height;
	const uint64 AtlasPixels = static_cast<uint64>(Atlas.Width) * Atlas.Height;
	if (AtlasPixels > WoTWorldMaxMoverAtlasPixels) return Error(
		TEXT("the mover light-map atlas exceeds its pixel bound"));
	Atlas.BGRA.Init(0, static_cast<int32>(AtlasPixels * 4));
	for (uint64 Pixel = 0; Pixel < AtlasPixels; ++Pixel)
		Atlas.BGRA[static_cast<int32>(Pixel * 4 + 3)] = 255;

	TMap<int32, const FWoTWorldActor*> ActorsByReference;
	const FWoTWorldActor* LevelInfo = nullptr;
	TSet<FString> LightningTags;
	for (const FWoTWorldActor& Actor : Source.Actors)
	{
		ActorsByReference.Add(Actor.ExportReference, &Actor);
		const FString Leaf = WoTWorldActorLeafClass(Actor);
		if (Leaf.Equals(TEXT("LevelInfo"), ESearchCase::IgnoreCase))
			LevelInfo = &Actor;
		if (Leaf.Equals(TEXT("Lightning"), ESearchCase::IgnoreCase))
		{
			FString LightTag;
			if (!WoTWorldEntryReadNameProperty(Actor, TEXT("LightTag"), LightTag))
				return Error(TEXT("a Lightning actor has an invalid LightTag"),
					Actor.Identity);
			if (!LightTag.IsEmpty() && LightTag != TEXT("None"))
				LightningTags.Add(WoTWorldFold(LightTag));
		}
	}
	if (LevelInfo == nullptr)
		return Error(TEXT("the map has no LevelInfo mover-lighting context"));
	OutManifest = FWoTWorldAnimationManifest();
	OutResponses.Reset();
	if (!Source.MapIdentity.Equals(TEXT("WheelOfTime"), ESearchCase::CaseSensitive))
	{
		FWoTBootstrapStatus ManifestStatus =
			WoTWorldBuildAnimationManifest(Source, LightningTags, OutManifest);
		if (ManifestStatus.IsError()) return ManifestStatus;
	}
	TArray<FWoTWorldResponseAccumulator> Accumulators;
	TArray<TArray<int32>> AccumulatorsByTerm;
	WoTWorldResponseAccumulators(OutManifest, Accumulators, AccumulatorsByTerm);
	OutResponses.SetNum(OutManifest.Terms.Num());
	uint64 ResponseBytes = 0;
	float LevelBrightness = 1.0f;
	if (!WoTWorldEntryReadFloatProperty(
			*LevelInfo, TEXT("Brightness"), 1.0f, LevelBrightness)
		|| LevelBrightness < 0.0f || LevelBrightness > 1024.0f)
	{
		return Error(TEXT("the LevelInfo brightness is invalid"),
			LevelInfo->Identity);
	}
	TArray<FIntVector> ZoneSeeds;
	ZoneSeeds.SetNumUninitialized(FMath::Max(WorldModel.Zones.Num(), 1));
	for (int32 ZoneIndex = 0; ZoneIndex < ZoneSeeds.Num(); ++ZoneIndex)
	{
		const FWoTWorldActor* ZoneActor = LevelInfo;
		if (WorldModel.Zones.IsValidIndex(ZoneIndex)
			&& WorldModel.Zones[ZoneIndex].Actor != 0)
		{
			if (const FWoTWorldActor* const* Found =
				ActorsByReference.Find(WorldModel.Zones[ZoneIndex].Actor))
				ZoneActor = *Found;
		}
		if (!WoTWorldEntryAmbientSeed(ZoneActor, ZoneSeeds[ZoneIndex]))
			return Error(TEXT("a mover-lighting zone has invalid ambient"),
				ZoneActor != nullptr ? ZoneActor->Identity : FString());
	}
	TArray<float> LightSqrt;
	WoTWorldEntryBuildLightSqrt(LightSqrt);
	TMap<int32, FWoTWorldEntryLight> ResolvedLights;
	for (FWoTWorldMoverModel& Mover : Movers)
	{
		Mover.UV1.Init(FVector2f::ZeroVector, Mover.Positions.Num());
		if (!Mover.bLightmapped) continue;
		if (!Source.Actors.IsValidIndex(Mover.ActorIndex))
			return Error(TEXT("a mover-lighting actor index is invalid"),
				Mover.ActorIdentity);
		FWoTWorldMoverPose Pose;
		if (!WoTWorldMoverReadPose(Source.Actors[Mover.ActorIndex], Pose))
			return Error(TEXT("a mover has an invalid initial brush pose"),
				Mover.ActorIdentity);
		for (int32 PolyIndex = 0; PolyIndex < Mover.Polys.Num(); ++PolyIndex)
		{
			const FWoTWorldPoly& Poly = Mover.Polys[PolyIndex];
			if (!Mover.BrushModel.LightMaps.IsValidIndex(Poly.BrushPoly)) continue;
			const FWoTWorldLightMap& LightMap =
				Mover.BrushModel.LightMaps[Poly.BrushPoly];
			const FWoTWorldAtlasEntry& Placement =
				Atlas.Surfaces[Mover.PolyAtlasOffset + PolyIndex];
			if (!Placement.bPresent)
				return Error(TEXT("a lit mover FPoly lacks an atlas placement"),
					Mover.ActorIdentity);
			FVector3d Base, TextureU, TextureV, Normal;
			if (!WoTWorldMoverTransformPoly(
					Poly, Pose, Base, TextureU, TextureV, Normal))
				return Error(TEXT("a mover FPoly transform is singular"),
					Mover.ActorIdentity);
			int32 SurfaceLightCount = 0;
			if (LightMap.LightActors >= 0)
			{
				int32 Cursor = LightMap.LightActors;
				while (Mover.BrushModel.Lights.IsValidIndex(Cursor)
					&& Mover.BrushModel.Lights[Cursor] != 0)
				{
					++SurfaceLightCount;
					++Cursor;
				}
				if (!Mover.BrushModel.Lights.IsValidIndex(Cursor)
					|| Mover.BrushModel.Lights[Cursor] != 0)
					return Error(TEXT("a mover FPoly light run has no terminator"),
						Mover.ActorIdentity);
			}
			FWoTWorldEntryBasis Basis;
			const bool bHasBasis = WoTWorldEntryBasis(
				Base, TextureU, TextureV, Normal, LightMap, Basis);
			if (!bHasBasis && SurfaceLightCount != 0)
				return Error(TEXT("a mover FPoly light-map basis is singular"),
					Mover.ActorIdentity);
			const int32 TexelCount = LightMap.UClamp * LightMap.VClamp;
			TArray<FIntVector> Totals;
			Totals.SetNumUninitialized(TexelCount);
			if (!bHasBasis)
			{
				// Battle3_01 DoorMover2 poly 2 has TextureV parallel to its
				// Normal and an empty saved light run. With no spatial kernel to
				// evaluate, its base-zone ambient is constant over the thin face.
				int32 Zone = 0;
				if (!WoTWorldEntryPointZone(
						WorldModel, Base + Normal * 2.0, Zone))
					return Error(TEXT("a singular mover FPoly zone walk is malformed"),
						Mover.ActorIdentity);
				if (Zone <= 0 || !ZoneSeeds.IsValidIndex(Zone)) Zone = 0;
				for (FIntVector& Total : Totals) Total = ZoneSeeds[Zone];
			}
			else
			{
				for (int32 V = 0; V < LightMap.VClamp; ++V)
				{
					const FVector3d Row = Basis.Base + Basis.DV * V
						+ Basis.Normal * 2.0;
					for (int32 U = 0; U < LightMap.UClamp; ++U)
					{
						int32 Zone = 0;
						if (!WoTWorldEntryPointZone(
								WorldModel, Row + Basis.DU * U, Zone))
							return Error(TEXT("a mover per-texel zone walk is malformed"),
								Mover.ActorIdentity);
						if (Zone <= 0 || !ZoneSeeds.IsValidIndex(Zone)) Zone = 0;
						Totals[V * LightMap.UClamp + U] = ZoneSeeds[Zone];
					}
				}
			}
			TArray<TArray<FIntVector>> AccumulatorTotals;
			AccumulatorTotals.Reserve(Accumulators.Num());
			for (int32 Index = 0; Index < Accumulators.Num(); ++Index)
				AccumulatorTotals.Add(Totals);
			bool bAnimatedLight = false;
			for (int32 LightIndex = 0;
				LightIndex < SurfaceLightCount; ++LightIndex)
			{
				const int32 Reference = Mover.BrushModel.Lights[
					LightMap.LightActors + LightIndex];
				const FWoTWorldActor* const* LightActor =
					ActorsByReference.Find(Reference);
				if (LightActor == nullptr) continue;
				FWoTWorldLightDefaults Defaults;
				if (!WoTWorldLightDefaults(
						WoTWorldActorLeafClass(**LightActor), Defaults)) continue;
				FWoTWorldEntryLight* Light = ResolvedLights.Find(Reference);
				if (Light == nullptr)
				{
					FWoTWorldEntryLight Resolved;
					if (!WoTWorldResolveEntryLight(
							**LightActor, LevelBrightness, Resolved))
						return Error(TEXT("a mover light has malformed properties"),
							(*LightActor)->Identity);
					Resolved.bInitiallyOff = LightningTags.Contains(
						WoTWorldFold(Resolved.Tag));
					ResolvedLights.Add(Reference, MoveTemp(Resolved));
					Light = ResolvedLights.Find(Reference);
				}
				const FString Skip = WoTWorldClassifyLightSkip(true, true,
					Light->bInitiallyOff, Light->LightType, Light->Brightness,
					static_cast<uint8>(Light->Effect), true);
				if (!Skip.IsEmpty()) continue;
				FWoTWorldEntrySurfaceLightSpan LightSpan;
				if (!WoTWorldEntrySurfaceLightSpan(LightMap, LightIndex,
						Mover.BrushModel.LightBits, LightSpan))
					return Error(TEXT("a mover shadow mask is out of range"),
						Mover.ActorIdentity);
				TArray<uint8> Illumination;
				if (!WoTWorldEntryComposeSurfaceLight(
						Mover.BrushModel.LightBits, LightMap, LightSpan,
						*Light, Basis, LightSqrt, Illumination))
					return Error(TEXT("a mover light calculation is invalid"),
						Mover.ActorIdentity);
				WoTWorldEntryAddMeanLight(*Light, Illumination, Totals);
				const FWoTWorldAnimationLight* Animation =
					OutManifest.ByReference.Find(Reference);
				bAnimatedLight = bAnimatedLight || Animation != nullptr;
				if (!Accumulators.IsEmpty())
				{
					WoTWorldEntryAddResponseLight(*Light, Illumination,
						LevelBrightness, Animation, Accumulators,
						AccumulatorTotals);
				}
			}
			if (bAnimatedLight)
			{
				++Mover.AnimatedLightPolyCount;
				++Source.MoverAnimatedLightPolyCount;
				for (FWoTWorldMoverSection& Section : Mover.Sections)
					if (Section.PolyIndices.Contains(PolyIndex))
						Section.bAnimatedLightmap = true;
			}
			for (int32 TermIndex = 0;
				TermIndex < OutManifest.Terms.Num(); ++TermIndex)
			{
				const TArray<int32>& Slots = AccumulatorsByTerm[TermIndex];
				TArray<uint8> RGB;
				RGB.SetNumUninitialized(TexelCount * 3);
				bool bAnyNonZero = false;
				for (int32 Texel = 0; Texel < TexelCount; ++Texel)
				{
					int32 Delta[3];
					if (Slots.Num() == 2)
					{
						const FIntVector& Hi = AccumulatorTotals[Slots[0]][Texel];
						const FIntVector& Lo = AccumulatorTotals[Slots[1]][Texel];
						Delta[0] = Hi.X - Lo.X;
						Delta[1] = Hi.Y - Lo.Y;
						Delta[2] = Hi.Z - Lo.Z;
					}
					else
					{
						const FIntVector& HH = AccumulatorTotals[Slots[0]][Texel];
						const FIntVector& HL = AccumulatorTotals[Slots[1]][Texel];
						const FIntVector& LH = AccumulatorTotals[Slots[2]][Texel];
						const FIntVector& LL = AccumulatorTotals[Slots[3]][Texel];
						const int32 N[3] = {HH.X - HL.X - LH.X + LL.X,
							HH.Y - HL.Y - LH.Y + LL.Y,
							HH.Z - HL.Z - LH.Z + LL.Z};
						for (int32 Channel = 0; Channel < 3; ++Channel)
						{
							if (N[Channel] < 0)
								return Error(TEXT("a mover intersection response is signed"),
									Mover.ActorIdentity);
							Delta[Channel] = N[Channel] / 2;
						}
					}
					for (int32 Channel = 0; Channel < 3; ++Channel)
					{
						if (Delta[Channel] < 0)
							return Error(TEXT("a mover waveform response is signed"),
								Mover.ActorIdentity);
						const uint8 Encoded = static_cast<uint8>(
							FMath::Min(Delta[Channel], 255));
						RGB[Texel * 3 + Channel] = Encoded;
						bAnyNonZero = bAnyNonZero || Encoded != 0;
					}
				}
				if (bAnyNonZero)
				{
					ResponseBytes += static_cast<uint64>(RGB.Num());
					if (ResponseBytes > WoTWorldMaxResponseBytes)
						return Error(TEXT("the mover light-map responses exceed their wire bound"));
					FWoTWorldResponseRect& Rect =
						OutResponses[TermIndex].Rects.AddDefaulted_GetRef();
					Rect.SurfaceIndex = Mover.PolyAtlasOffset + PolyIndex;
					Rect.RGB = MoveTemp(RGB);
				}
			}
			if (Source.MapIdentity.Equals(
					TEXT("Battle2_02"), ESearchCase::CaseSensitive)
				&& Mover.ActorIdentity.EndsWith(
					TEXT(".Mover6"), ESearchCase::CaseSensitive)
				&& (PolyIndex == 2 || PolyIndex == 3))
			{
				// The readable Python oracle pins these five coordinates on both
				// throne-door faces. Keep the native RGB7 values as descriptor-only
				// evidence; they never enter the sealed object or descriptor roots.
				const FIntPoint Witnesses[] = {
					{0, 0}, {5, 0}, {4, 1}, {5, 1}, {9, 2}};
				for (const FIntPoint Witness : Witnesses)
				{
					if (Witness.X >= LightMap.UClamp
						|| Witness.Y >= LightMap.VClamp)
						return Error(TEXT("the mover oracle coordinate is out of range"),
							Mover.ActorIdentity);
					const FIntVector& RGB =
						Totals[Witness.Y * LightMap.UClamp + Witness.X];
					Source.MoverOracleRGB7.Add(static_cast<uint8>(RGB.X));
					Source.MoverOracleRGB7.Add(static_cast<uint8>(RGB.Y));
					Source.MoverOracleRGB7.Add(static_cast<uint8>(RGB.Z));
				}
			}
			for (int32 PaddedV = -1; PaddedV <= LightMap.VClamp; ++PaddedV)
			{
				const int32 SourceV = FMath::Clamp(PaddedV, 0, LightMap.VClamp - 1);
				for (int32 PaddedU = -1; PaddedU <= LightMap.UClamp; ++PaddedU)
				{
					const int32 SourceU =
						FMath::Clamp(PaddedU, 0, LightMap.UClamp - 1);
					const FIntVector& RGB =
						Totals[SourceV * LightMap.UClamp + SourceU];
					const int32 ByteOffset = ((Placement.Y + PaddedV) * Atlas.Width
						+ Placement.X + PaddedU) * 4;
					Atlas.BGRA[ByteOffset] = static_cast<uint8>(
						FMath::Min(RGB.Z * 2, 255));
					Atlas.BGRA[ByteOffset + 1] = static_cast<uint8>(
						FMath::Min(RGB.Y * 2, 255));
					Atlas.BGRA[ByteOffset + 2] = static_cast<uint8>(
						FMath::Min(RGB.X * 2, 255));
				}
			}
		}
		for (int32 VertexIndex = 0; VertexIndex < Mover.Positions.Num(); ++VertexIndex)
		{
			const int32 PolyIndex = Mover.VertexPolys[VertexIndex];
			FVector2f UV = Atlas.FullbrightUV;
			if (Mover.Polys.IsValidIndex(PolyIndex))
			{
				const FWoTWorldPoly& Poly = Mover.Polys[PolyIndex];
				if (Mover.BrushModel.LightMaps.IsValidIndex(Poly.BrushPoly))
				{
					const FWoTWorldLightMap& LightMap =
						Mover.BrushModel.LightMaps[Poly.BrushPoly];
					const FWoTWorldAtlasEntry& Placement =
						Atlas.Surfaces[Mover.PolyAtlasOffset + PolyIndex];
					const FVector3f& Position = Mover.UV1SourcePositions[VertexIndex];
					const FVector3d Delta = FVector3d(Position) - FVector3d(Poly.Base);
					const double FractionalU =
						(WoTWorldOrderedDot(Delta, Poly.TextureU) - LightMap.Pan.X)
							/ LightMap.UScale;
					const double FractionalV =
						(WoTWorldOrderedDot(Delta, Poly.TextureV) - LightMap.Pan.Y)
							/ LightMap.VScale;
					if (FractionalU < -1.0 || FractionalU > LightMap.UClamp
						|| FractionalV < -1.0 || FractionalV > LightMap.VClamp)
					{
						++Mover.UV1OutsidePaddedRectCount;
						++Source.MoverUV1OutsidePaddedRectCount;
					}
					UV = FVector2f(WoTWorldFloat((Placement.X + FractionalU + 0.5)
						/ Atlas.Width), WoTWorldFloat((Placement.Y + FractionalV + 0.5)
						/ Atlas.Height));
				}
			}
			if (!FMath::IsFinite(UV.X) || !FMath::IsFinite(UV.Y))
				return Error(TEXT("a mover light-map UV is non-finite"),
					Mover.ActorIdentity);
			Mover.UV1[VertexIndex] = UV;
		}
	}
	for (int32 V = 0; V < 3; ++V)
	{
		for (int32 U = 0; U < 3; ++U)
		{
			const int32 ByteOffset = ((Atlas.FullbrightOuter.Y + V) * Atlas.Width
				+ Atlas.FullbrightOuter.X + U) * 4;
			Atlas.BGRA[ByteOffset] = 128;
			Atlas.BGRA[ByteOffset + 1] = 128;
			Atlas.BGRA[ByteOffset + 2] = 128;
		}
	}
	return FWoTBootstrapStatus::Success();
}

/** skybake.py zone_sky_links + pick_winner, then the first model zone whose
 * zone actor is the winner (skygeo.sky_zone_of). Zone is INDEX_NONE without a
 * SkyZoneInfo or without a zone naming the winner. */
struct FWoTWorldSkyWinner
{
	const FWoTWorldActor* Actor = nullptr;
	int32 Zone = INDEX_NONE;
};

FWoTWorldSkyWinner WoTWorldSelectSkyWinner(
	const FWoTWorldSource& Source, const FWoTWorldModel& Model)
{
	FWoTWorldSkyWinner Result;
	TArray<const FWoTWorldActor*> Skies;
	TMap<FString, const FWoTWorldActor*> ActorsByLeafIdentity;
	for (const FWoTWorldActor& Actor : Source.Actors)
	{
		int32 Dot = INDEX_NONE;
		const FString Leaf = Actor.Identity.FindLastChar(TEXT('.'), Dot)
			? Actor.Identity.Mid(Dot + 1) : Actor.Identity;
		ActorsByLeafIdentity.Add(WoTWorldFold(Leaf), &Actor);
		if (WoTWorldActorLeafClass(Actor).Equals(
			TEXT("SkyZoneInfo"), ESearchCase::IgnoreCase))
		{
			Skies.Add(&Actor);
		}
	}
	if (Skies.IsEmpty()) return Result;
	const FWoTWorldActor* Default = Skies.Last();
	for (const FWoTWorldActor* Sky : Skies)
	{
		const FWoTTypedProperty* HighDetail =
			WoTWorldFindProperty(*Sky, TEXT("bHighDetail"));
		if (HighDetail != nullptr && HighDetail->Type == 3
			&& HighDetail->Value.Num() == 1 && HighDetail->Value[0] != 0)
		{
			Default = Sky;
		}
	}
	TArray<const FWoTWorldActor*> Links;
	for (const FWoTWorldZone& Zone : Model.Zones)
	{
		const FWoTWorldActor* ZoneActor = nullptr;
		if (const int32* Binding = Source.SemanticObjectByReference.Find(Zone.Actor))
		{
			const FString& Identity = Source.SemanticObjects[*Binding].Identity;
			int32 Dot = INDEX_NONE;
			const FString Leaf = Identity.FindLastChar(TEXT('.'), Dot)
				? Identity.Mid(Dot + 1) : Identity;
			ZoneActor = ActorsByLeafIdentity.FindRef(WoTWorldFold(Leaf));
		}
		const FWoTWorldActor* Sky = Default;
		if (ZoneActor != nullptr)
		{
			const FString ZoneClass = WoTWorldFold(WoTWorldActorLeafClass(*ZoneActor));
			if (ZoneClass == TEXT("wotzoneinfo") || ZoneClass == TEXT("waterzone")
				|| ZoneClass == TEXT("wayszone"))
			{
				const FWoTTypedProperty* Event =
					WoTWorldFindProperty(*ZoneActor, TEXT("Event"));
			const FString EventName = Event != nullptr ? Event->NameValue : FString();
				if (!EventName.IsEmpty() && EventName != TEXT("None"))
				{
					for (const FWoTWorldActor* Candidate : Skies)
					{
						const FWoTTypedProperty* Tag =
							WoTWorldFindProperty(*Candidate, TEXT("Tag"));
					if (Tag != nullptr && !Tag->NameValue.IsEmpty()
						&& Tag->NameValue.Equals(
								EventName, ESearchCase::IgnoreCase))
						{
							Sky = Candidate;
						}
					}
				}
				else
				{
					Sky = Skies.Last();
				}
			}
		}
		Links.Add(Sky);
	}
	const FWoTWorldActor* Winner = Skies.Last();
	TMap<const FWoTWorldActor*, int32> Tally;
	for (const FWoTWorldNode& Node : Model.Nodes)
	{
		if (Node.VertexCount < 3
			|| (Model.Surfaces[Node.Surface].Flags & WoTWorldPFFakeBackdrop) == 0)
			continue;
		for (int32 Side = 0; Side < 2; ++Side)
		{
			const uint8 Zone = Node.Zones[Side];
			if (Side == 1 && Zone == Node.Zones[0]) continue;
			if (Zone == 0 || Links.IsEmpty()) continue;
			const FWoTWorldActor* Sky =
				Links.IsValidIndex(Zone) ? Links[Zone] : Links[0];
			Tally.FindOrAdd(Sky)++;
		}
	}
	if (!Tally.IsEmpty())
	{
		int32 BestCount = -1;
		for (const FWoTWorldActor* Sky : Skies)
		{
			const int32 Count = Tally.FindRef(Sky);
			if (Count > BestCount)
			{
				Winner = Sky;
				BestCount = Count;
			}
		}
	}
	Result.Actor = Winner;
	for (int32 ZoneIndex = 0; ZoneIndex < Model.Zones.Num(); ++ZoneIndex)
	{
		const int32* Binding =
			Source.SemanticObjectByReference.Find(Model.Zones[ZoneIndex].Actor);
		if (Binding != nullptr && Source.SemanticObjects[*Binding].Identity.Equals(
			Winner->Identity, ESearchCase::IgnoreCase))
		{
			Result.Zone = ZoneIndex;
			break;
		}
	}
	return Result;
}

TSet<int32> WoTWorldSkySurfaces(
	const FWoTWorldSource& Source, const FWoTWorldModel& Model)
{
	const int32 SkyZone = WoTWorldSelectSkyWinner(Source, Model).Zone;
	TSet<int32> Result;
	if (SkyZone == INDEX_NONE) return Result;
	for (const FWoTWorldNode& Node : Model.Nodes)
	{
		if (Node.VertexCount >= 3
			&& (Node.Zones[0] == SkyZone || Node.Zones[1] == SkyZone))
		{
			Result.Add(Node.Surface);
		}
	}
	return Result;
}

struct FWoTWorldBSPPartData
{
	EWoTBSPPartRole Role = EWoTBSPPartRole::Main;
	FString PartIdentity;
	FString ProceduralMaterialSlot;
	TArray<uint8> Payload;
	uint32 VertexCount = 0;
	uint32 IndexCount = 0;
	uint32 SectionCount = 0;
};

struct FWoTWorldBSPSectionIdentity
{
	FString CanonicalPackage;
	FString CanonicalName;
	FString SourceTextureIdentity;
	FString MacroCanonicalPackage;
	FString MacroCanonicalName;
	FString MacroSourceTextureIdentity;
	float MacroRatio = 0.0f;
	float MacroOffsetU = 0.0f;
	float MacroOffsetV = 0.0f;
	uint32 SurfaceFlags = 0;
	TMap<FString, uint32> PanZoneVotes;
	TArray<FString> PanZoneOrder;
};

FWoTBootstrapStatus WoTWorldValidateAtlasCarrier(bool bAtlasComposed,
	FStringView MapIdentity, TConstArrayView<FString> PartIdentities)
{
	if (!bAtlasComposed) return FWoTBootstrapStatus::Success();
	for (const FString& PartIdentity : PartIdentities)
	{
		if (FStringView(PartIdentity).Equals(
			MapIdentity, ESearchCase::CaseSensitive))
			return FWoTBootstrapStatus::Success();
	}
	return FWoTBootstrapStatus::Error(WoTWorldError(
		EWoTBootstrapError::CorruptInstallation,
		TEXT("a composed BSP light-map atlas has no exact main part carrier"),
		FString(MapIdentity)));
}

/** One BSP node's render triangles exactly as the world encoder emits them:
 * bsp2obj fan triangulation with the native UV0 (texture) and UV1 (atlas)
 * equations, Newell winding agreement, degenerate triangles dropped. Shared
 * by WoTWorldEncodeBSP and the sky-rest geometry. */
FWoTBootstrapStatus WoTWorldBuildNodeTriangles(const FWoTWorldSource& Source,
	const FWoTWorldModel& Model, const FWoTWorldAtlas& Atlas,
	const FWoTWorldNode& Node, TArray<FWoTWorldTriangle>& Out)
{
	Out.Reset();
	const FWoTWorldSurface& Surface = Model.Surfaces[Node.Surface];
	const FVector3f& Base = Model.Points[Surface.Base];
	const FVector3f& TextureU = Model.Vectors[Surface.TextureU];
	const FVector3f& TextureV = Model.Vectors[Surface.TextureV];
	const FVector3f& Normal = Model.Vectors[Surface.Normal];
	const float NormalSquared = Normal.SizeSquared();
	if (!FMath::IsFinite(NormalSquared)
		|| NormalSquared < 0.98f || NormalSquared > 1.02f)
	{
		return FWoTBootstrapStatus::Error(WoTWorldError(EWoTBootstrapError::InvalidNumber,
			TEXT("a BSP surface normal is not finite/unit length"),
			Source.SourcePath));
	}
	const WoT::Retail::Generated::FWorldTexture* Texture =
		WoTWorldTexture(Source, Surface.Texture);
	const double TextureWidth = Texture != nullptr
		? static_cast<double>(Texture->Width) * Texture->DrawScale
		: 1.0;
	const double TextureHeight = Texture != nullptr
		? static_cast<double>(Texture->Height) * Texture->DrawScale
		: 1.0;
	if (!FMath::IsFinite(TextureWidth) || !FMath::IsFinite(TextureHeight)
		|| TextureWidth <= 0.0 || TextureHeight <= 0.0)
	{
		return FWoTBootstrapStatus::Error(WoTWorldError(EWoTBootstrapError::InvalidNumber,
			TEXT("a canonical BSP texture scale is invalid"),
			Source.SourcePath));
	}
	TArray<FVector3f> Polygon;
	TArray<FVector2f> UV0s;
	TArray<FVector2f> UV1s;
	Polygon.Reserve(Node.VertexCount);
	UV0s.Reserve(Node.VertexCount);
	UV1s.Reserve(Node.VertexCount);
	for (uint32 VertexIndex = 0; VertexIndex < Node.VertexCount; ++VertexIndex)
	{
		const FWoTWorldVert& Vertex = Model.Vertices[Node.VertexPool + VertexIndex];
		const FVector3f& Point = Model.Points[Vertex.Point];
		Polygon.Add(Point);
		const FVector3d Delta(
			static_cast<double>(Point.X) - Base.X,
			static_cast<double>(Point.Y) - Base.Y,
			static_cast<double>(Point.Z) - Base.Z);
		// Pan is applied after the ordered native dot, matching the authored
		// texture equation rather than becoming another accumulation term.
		const double RawU = WoTWorldOrderedDot(Delta, TextureU)
			+ static_cast<double>(Surface.PanU);
		const double RawV = WoTWorldOrderedDot(Delta, TextureV)
			+ static_cast<double>(Surface.PanV);
		const FVector2f UV0(
			WoTWorldFloat(RawU / TextureWidth),
			WoTWorldFloat(RawV / TextureHeight));
		FVector2f UV1 = Atlas.FullbrightUV;
		if (Surface.LightMap >= 0)
		{
			const FWoTWorldLightMap& LightMap = Model.LightMaps[Surface.LightMap];
			const FWoTWorldAtlasEntry& Entry = Atlas.Surfaces[Node.Surface];
			if (!Entry.bPresent)
			{
				return FWoTBootstrapStatus::Error(WoTWorldError(
					EWoTBootstrapError::CorruptInstallation,
					TEXT("a lit BSP surface lacks its canonical atlas placement"),
					Source.SourcePath));
			}
			const double FractionalU =
				(WoTWorldOrderedDot(Delta, TextureU) - LightMap.Pan.X)
				/ LightMap.UScale;
			const double FractionalV =
				(WoTWorldOrderedDot(Delta, TextureV) - LightMap.Pan.Y)
				/ LightMap.VScale;
			UV1 = FVector2f(
				WoTWorldFloat((static_cast<double>(Entry.X) + FractionalU + 0.5)
					/ Atlas.Width),
				WoTWorldFloat((static_cast<double>(Entry.Y) + FractionalV + 0.5)
					/ Atlas.Height));
		}
		if (!FMath::IsFinite(UV0.X) || !FMath::IsFinite(UV0.Y)
			|| !FMath::IsFinite(UV1.X) || !FMath::IsFinite(UV1.Y))
		{
			return FWoTBootstrapStatus::Error(WoTWorldError(EWoTBootstrapError::InvalidNumber,
				TEXT("a derived BSP UV is not finite"), Source.SourcePath));
		}
		UV0s.Add(UV0);
		UV1s.Add(UV1);
	}
	FVector3d Newell = FVector3d::ZeroVector;
	for (int32 VertexIndex = 0; VertexIndex < Polygon.Num(); ++VertexIndex)
	{
		const FVector3f& First = Polygon[VertexIndex];
		const FVector3f& Second = Polygon[(VertexIndex + 1) % Polygon.Num()];
		Newell.X += (static_cast<double>(First.Y) - Second.Y)
			* (static_cast<double>(First.Z) + Second.Z);
		Newell.Y += (static_cast<double>(First.Z) - Second.Z)
			* (static_cast<double>(First.X) + Second.X);
		Newell.Z += (static_cast<double>(First.X) - Second.X)
			* (static_cast<double>(First.Y) + Second.Y);
	}
	if (WoTWorldOrderedDot(Newell, Normal) < 0.0)
	{
		Algo::Reverse(Polygon);
		Algo::Reverse(UV0s);
		Algo::Reverse(UV1s);
	}
	for (int32 VertexIndex = 1; VertexIndex + 1 < Polygon.Num(); ++VertexIndex)
	{
		// groups_to_glb reverses each triangle for the Y/Z handedness swap;
		// decode_bsp_glb restores axes but retains that index order.
		const int32 Order[3] = {VertexIndex + 1, VertexIndex, 0};
		if (WoTWorldTriangleAreaSquared(
			Polygon[Order[0]], Polygon[Order[1]], Polygon[Order[2]]) < 0.001)
			continue;
		FWoTWorldTriangle& Triangle = Out.AddDefaulted_GetRef();
		Triangle.Normal = Normal;
		Triangle.SurfaceIndex = static_cast<uint32>(Node.Surface);
		for (int32 Corner = 0; Corner < 3; ++Corner)
		{
			const FVector3f& Position = Polygon[Order[Corner]];
			Triangle.Positions[Corner] = FVector3f(
				WoTWorldPosition(Position.X),
				WoTWorldPosition(Position.Y),
				WoTWorldPosition(Position.Z));
			Triangle.UV0[Corner] = UV0s[Order[Corner]];
			Triangle.UV1[Corner] = UV1s[Order[Corner]];
		}
	}
	return FWoTBootstrapStatus::Success();
}


// ---------------------------------------------------------------------------
// Sky rest: Tools/convert/skybake.py bake_rest (:438-652) ported
// exactly. Retail's sky pass is a child frame rooted at the SkyZoneInfo's zone
// (in the original renderer) whose zone traversal continues
// through PORTAL surfaces into any nonzero opposite zone. On a
// portal-unsealed sky zone (Arena_04, Arena_07, Battle3_02) retail therefore
// draws the rest of the map behind the sky's portal sheets. The development
// pipeline discovers that content with an 8.4M-ray census from the sky origin
// (this function) and ships it as live geometry in the sky mesh
// (skygeo.py:131-224, setup_childsky.py:313-329).
// ---------------------------------------------------------------------------
constexpr int32 WoTWorldSkyRestWidth = 4096;   // skybake.py:57 W
constexpr int32 WoTWorldSkyRestHeight = 2048;  // skybake.py:57 H
constexpr int32 WoTWorldSkyRestTile = 16;
constexpr uint32 WoTWorldSkyRestMaxTriangles = 2000000;

enum class EWoTWorldSkyRestRole : uint8
{
	Visible = 1,
	Blocker = 2,
	FakeBackdrop = 3,
};

struct FWoTWorldSkyRestNode
{
	int32 NodeIndex = 0;
	int32 Surface = 0;
	uint8 Kind = 0;          // 0 opaque, 1 translucent, 2 modulated
	bool bTwoSided = false;
	bool bFakeBackdrop = false;
	bool bSkipped = false;   // no loadable texture: skybake skips the hit
	FVector3d Normal = FVector3d::ZeroVector;
	double Numerator = 0.0;  // (base - origin) . normal
	int32 Keep0 = 0;
	int32 Keep1 = 1;
	int32 FirstPoint = 0;
	int32 PointCount = 0;
	FVector3d CapCenter = FVector3d::ZeroVector;
	double CapCos = -2.0;    // < -1: never culled
	double CapRadius = UE_DOUBLE_PI;
};

struct FWoTWorldSkyRestSurfaceCount
{
	uint32 Visible = 0;
	uint32 Blocker = 0;
	uint32 FakeBackdrop = 0;
};

struct FWoTWorldSkyRest
{
	bool bPresent = false;
	int32 SkyZone = 0;
	uint32 RayCount = 0;
	uint32 CoveredRays = 0;
	TMap<int32, FWoTWorldSkyRestSurfaceCount> Surfaces;
};

/** skybake._load_tex succeeds: a static texture the development exporter wrote
 * (tex/<pkg>/<name>.png). Procedural textures have no PNG and a dangling import
 * (the generator's 1x1 placeholder row, e.g. WOT.Watr_400) has no file. */
bool WoTWorldSkyRestTextureLoads(const FWoTWorldSource& Source, int32 Reference)
{
	if (Reference == 0) return false;
	const WoT::Retail::Generated::FWorldTexture* Texture =
		WoTWorldTexture(Source, Reference);
	return Texture != nullptr && !Texture->bProcedural
		&& (Texture->Width > 1 || Texture->Height > 1);
}

/** skybake._cast_nodes for one ray and one node: t, hit and back-face flag. */
bool WoTWorldSkyRestIntersect(const FWoTWorldSkyRestNode& Node,
	TConstArrayView<FVector2d> Points2D, const FVector3d& Origin,
	const FVector3d& Direction, double& OutT, bool& bOutBack)
{
	const double Denominator = Direction.X * Node.Normal.X
		+ Direction.Y * Node.Normal.Y + Direction.Z * Node.Normal.Z;
	const double T = Node.Numerator / Denominator;
	if (!FMath::IsFinite(T) || !(T > 1.0)) return false;
	const double Hit[3] = {
		Origin.X + Direction.X * T,
		Origin.Y + Direction.Y * T,
		Origin.Z + Direction.Z * T};
	const double HitU = Hit[Node.Keep0];
	const double HitV = Hit[Node.Keep1];
	double Sign = 0.0;
	for (int32 Index = 0; Index < Node.PointCount; ++Index)
	{
		const FVector2d& A = Points2D[Node.FirstPoint + Index];
		const FVector2d& B = Points2D[Node.FirstPoint + (Index + 1) % Node.PointCount];
		const double EdgeU = B.X - A.X;
		const double EdgeV = B.Y - A.Y;
		const double Cross = EdgeU * (HitV - A.Y) - EdgeV * (HitU - A.X);
		if (Index == 0) Sign = Cross >= 0.0 ? 1.0 : -1.0;
		if (Cross * Sign < -1e-6) return false;
	}
	OutT = T;
	bOutBack = Denominator > 0.0;
	return true;
}

/** The node polygons of a candidate list, projected on the plane that drops
 * the surface normal's dominant axis (numpy argmax: first maximum), plus the
 * angular cap skybake._node_theta_bounds derives (padded 1e-3 rad) that the
 * tile culling below uses. Culling never removes a hit: every hit direction
 * lies in the polygon's corner cone, inside the cap. */
void WoTWorldSkyRestPrepareNodes(const FWoTWorldSource& Source,
	const FWoTWorldModel& Model, const FVector3d& Origin,
	TArray<FWoTWorldSkyRestNode>& Nodes, TArray<FVector2d>& Points2D)
{
	for (FWoTWorldSkyRestNode& Node : Nodes)
	{
		const FWoTWorldNode& ModelNode = Model.Nodes[Node.NodeIndex];
		const FWoTWorldSurface& Surface = Model.Surfaces[Node.Surface];
		const FVector3f& Normal = Model.Vectors[Surface.Normal];
		const FVector3f& Base = Model.Points[Surface.Base];
		Node.Normal = FVector3d(Normal.X, Normal.Y, Normal.Z);
		const FVector3d BaseDelta(static_cast<double>(Base.X) - Origin.X,
			static_cast<double>(Base.Y) - Origin.Y,
			static_cast<double>(Base.Z) - Origin.Z);
		Node.Numerator = BaseDelta.X * Node.Normal.X + BaseDelta.Y * Node.Normal.Y
			+ BaseDelta.Z * Node.Normal.Z;
		const double Absolute[3] = {FMath::Abs(Node.Normal.X),
			FMath::Abs(Node.Normal.Y), FMath::Abs(Node.Normal.Z)};
		int32 Axis = 0;
		if (Absolute[1] > Absolute[Axis]) Axis = 1;
		if (Absolute[2] > Absolute[Axis]) Axis = 2;
		Node.Keep0 = Axis == 0 ? 1 : 0;
		Node.Keep1 = Axis == 2 ? 1 : 2;
		Node.FirstPoint = Points2D.Num();
		Node.PointCount = ModelNode.VertexCount;
		FVector3d Sum = FVector3d::ZeroVector;
		TArray<FVector3d, TInlineAllocator<16>> Units;
		bool bDegenerate = false;
		for (int32 Corner = 0; Corner < Node.PointCount; ++Corner)
		{
			const FVector3f& Point =
				Model.Points[Model.Vertices[ModelNode.VertexPool + Corner].Point];
			const double Coordinates[3] = {Point.X, Point.Y, Point.Z};
			Points2D.Add(FVector2d(Coordinates[Node.Keep0], Coordinates[Node.Keep1]));
			const FVector3d Delta(Coordinates[0] - Origin.X,
				Coordinates[1] - Origin.Y, Coordinates[2] - Origin.Z);
			const double Length = Delta.Size();
			if (Length < 1e-9)
			{
				bDegenerate = true;
				continue;
			}
			Units.Add(Delta / Length);
			Sum += Delta / Length;
		}
		const double SumLength = Sum.Size();
		if (bDegenerate || SumLength < 1e-9) continue;
		const FVector3d Center = Sum / SumLength;
		double MinimumDot = 1.0;
		for (const FVector3d& Unit : Units)
			MinimumDot = FMath::Min(MinimumDot, FVector3d::DotProduct(Unit, Center));
		const double Radius = FMath::Acos(FMath::Clamp(MinimumDot, -1.0, 1.0));
		if (Radius > 1.55) continue;   // cap not convex approaching 90 deg
		Node.CapCenter = Center;
		Node.CapRadius = Radius + 1e-3;
		Node.CapCos = FMath::Cos(Node.CapRadius);
	}
}

/** Bin nodes into WoTWorldSkyRestTile^2 sample tiles of the lat-long grid by
 * their padded caps. Samples are (row, column) of skybake's grid; a sample's
 * WORLD direction has the grid theta and phi + yaw (the SkyZoneInfo yaw is
 * baked into the sample directions, skybake.py:529-537). */
void WoTWorldSkyRestBinNodes(TConstArrayView<FWoTWorldSkyRestNode> Nodes,
	double YawRadians, TArray<TArray<int32>>& Tiles, TArray<int32>& Always)
{
	const int32 TileRows = WoTWorldSkyRestHeight / WoTWorldSkyRestTile;
	const int32 TileColumns = WoTWorldSkyRestWidth / WoTWorldSkyRestTile;
	Tiles.SetNum(TileRows * TileColumns);
	for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
	{
		const FWoTWorldSkyRestNode& Node = Nodes[NodeIndex];
		if (Node.CapCos < -1.0)
		{
			Always.Add(NodeIndex);
			continue;
		}
		const double CenterTheta = FMath::Acos(FMath::Clamp(Node.CapCenter.Z, -1.0, 1.0));
		const double ThetaLow = CenterTheta - Node.CapRadius;
		const double ThetaHigh = CenterTheta + Node.CapRadius;
		// theta_i = (i + 0.5) / H * pi; one extra row of margin each side.
		const int32 RowLow = FMath::Clamp(static_cast<int32>(FMath::FloorToDouble(
			ThetaLow / UE_DOUBLE_PI * WoTWorldSkyRestHeight - 0.5)) - 1,
			0, WoTWorldSkyRestHeight - 1);
		const int32 RowHigh = FMath::Clamp(static_cast<int32>(FMath::CeilToDouble(
			ThetaHigh / UE_DOUBLE_PI * WoTWorldSkyRestHeight - 0.5)) + 1,
			0, WoTWorldSkyRestHeight - 1);
		bool bAllColumns = ThetaLow <= 0.0 || ThetaHigh >= UE_DOUBLE_PI;
		double ColumnLow = 0.0;
		double ColumnHigh = 0.0;
		if (!bAllColumns)
		{
			const double SinCenter = FMath::Sin(CenterTheta);
			const double SinRadius = FMath::Sin(Node.CapRadius);
			if (SinRadius >= SinCenter) bAllColumns = true;
			else
			{
				const double HalfWidth = FMath::Asin(SinRadius / SinCenter) + 1e-3;
				// Sample phi = world phi - yaw; phi_j = ((j + 0.5)/W - 0.5) * 2pi.
				const double SamplePhi = FMath::Atan2(Node.CapCenter.Y, Node.CapCenter.X)
					- YawRadians;
				ColumnLow = ((SamplePhi - HalfWidth) / (2.0 * UE_DOUBLE_PI) + 0.5)
					* WoTWorldSkyRestWidth - 0.5 - 1.0;
				ColumnHigh = ((SamplePhi + HalfWidth) / (2.0 * UE_DOUBLE_PI) + 0.5)
					* WoTWorldSkyRestWidth - 0.5 + 1.0;
				if (ColumnHigh - ColumnLow >= WoTWorldSkyRestWidth - 2) bAllColumns = true;
			}
		}
		for (int32 TileRow = RowLow / WoTWorldSkyRestTile;
			TileRow <= RowHigh / WoTWorldSkyRestTile; ++TileRow)
		{
			if (bAllColumns)
			{
				for (int32 TileColumn = 0; TileColumn < TileColumns; ++TileColumn)
					Tiles[TileRow * TileColumns + TileColumn].Add(NodeIndex);
				continue;
			}
			const int64 First = static_cast<int64>(FMath::FloorToDouble(ColumnLow));
			const int64 Last = static_cast<int64>(FMath::CeilToDouble(ColumnHigh));
			int32 PreviousTile = INDEX_NONE;
			for (int64 Column = First; Column <= Last; ++Column)
			{
				const int32 Wrapped = static_cast<int32>(
					((Column % WoTWorldSkyRestWidth) + WoTWorldSkyRestWidth)
					% WoTWorldSkyRestWidth);
				const int32 TileColumn = Wrapped / WoTWorldSkyRestTile;
				if (TileColumn == PreviousTile) continue;
				PreviousTile = TileColumn;
				TArray<int32>& Tile = Tiles[TileRow * TileColumns + TileColumn];
				if (Tile.IsEmpty() || Tile.Last() != NodeIndex) Tile.Add(NodeIndex);
			}
		}
	}
}

/** skybake.bake_rest's per-ray classification (no preview colours):
 * covered = any opaque sky-zone hit; otherwise the rest-of-map hits sorted
 * near -> far (stable in node order) are walked to the terminator. */
FWoTWorldSkyRest WoTWorldCensusSkyRest(const FWoTWorldSource& Source,
	const FWoTWorldModel& Model)
{
	FWoTWorldSkyRest Census;
	const FWoTWorldSkyWinner Winner = WoTWorldSelectSkyWinner(Source, Model);
	if (Winner.Actor == nullptr || Winner.Zone == INDEX_NONE) return Census;
	// The development bake reads the SkyZoneInfo pose from the T3D export
	// (6-decimal Location, integer Rotation): WoTWorldProjectT3DLocation.
	FVector3d Location = FVector3d::ZeroVector;
	FIntVector Rotation = FIntVector::ZeroValue;
	if (!WoTWorldEntryReadVectorProperty(*Winner.Actor, TEXT("Location"), Location)
		|| !WoTWorldEntryReadRotatorProperty(*Winner.Actor, TEXT("Rotation"), Rotation))
		return Census;
	const FVector3d Origin(WoTWorldProjectT3DLocation(Location.X),
		WoTWorldProjectT3DLocation(Location.Y), WoTWorldProjectT3DLocation(Location.Z));
	const double YawRadians = static_cast<double>(Rotation.Y) * 2.0 * UE_DOUBLE_PI / 65536.0;

	TArray<FWoTWorldSkyRestNode> SkyNodes;
	TArray<FWoTWorldSkyRestNode> RestNodes;
	for (int32 NodeIndex = 0; NodeIndex < Model.Nodes.Num(); ++NodeIndex)
	{
		const FWoTWorldNode& Node = Model.Nodes[NodeIndex];
		if (Node.VertexCount < 3) continue;
		const FWoTWorldSurface& Surface = Model.Surfaces[Node.Surface];
		if ((Surface.Flags & WoTWorldPFInvisible) != 0) continue;
		FWoTWorldSkyRestNode Row;
		Row.NodeIndex = NodeIndex;
		Row.Surface = Node.Surface;
		Row.Kind = (Surface.Flags & WoTWorldPFTranslucent) != 0 ? 1
			: (Surface.Flags & WoTWorldPFModulated) != 0 ? 2 : 0;
		Row.bTwoSided = (Surface.Flags & WoTWorldPFTwoSided) != 0;
		Row.bFakeBackdrop = (Surface.Flags & WoTWorldPFFakeBackdrop) != 0;
		Row.bSkipped = !Row.bFakeBackdrop
			&& !WoTWorldSkyRestTextureLoads(Source, Surface.Texture);
		const bool bSky = Node.Zones[0] == Winner.Zone || Node.Zones[1] == Winner.Zone;
		(bSky ? SkyNodes : RestNodes).Add(Row);
	}
	if (SkyNodes.IsEmpty()) return Census;   // "no sky-zone nodes - no shell"
	Census.bPresent = true;
	Census.SkyZone = Winner.Zone;
	Census.RayCount = static_cast<uint32>(WoTWorldSkyRestWidth) * WoTWorldSkyRestHeight;

	TArray<FVector2d> SkyPoints;
	TArray<FVector2d> RestPoints;
	WoTWorldSkyRestPrepareNodes(Source, Model, Origin, SkyNodes, SkyPoints);
	WoTWorldSkyRestPrepareNodes(Source, Model, Origin, RestNodes, RestPoints);
	TArray<TArray<int32>> SkyTiles;
	TArray<TArray<int32>> RestTiles;
	TArray<int32> SkyAlways;
	TArray<int32> RestAlways;
	WoTWorldSkyRestBinNodes(SkyNodes, YawRadians, SkyTiles, SkyAlways);
	WoTWorldSkyRestBinNodes(RestNodes, YawRadians, RestTiles, RestAlways);

	const double CosYaw = FMath::Cos(YawRadians);
	const double SinYaw = FMath::Sin(YawRadians);
	const bool bRotated = Rotation.X != 0 || Rotation.Y != 0 || Rotation.Z != 0;
	const int32 TileColumns = WoTWorldSkyRestWidth / WoTWorldSkyRestTile;

	struct FRowResult
	{
		uint32 Covered = 0;
		TMap<int32, FWoTWorldSkyRestSurfaceCount> Surfaces;
	};
	TArray<FRowResult> Rows;
	Rows.SetNum(WoTWorldSkyRestHeight);
	ParallelFor(WoTWorldSkyRestHeight, [&](int32 RowIndex)
	{
		FRowResult& Out = Rows[RowIndex];
		const double V = (static_cast<double>(RowIndex) + 0.5) / WoTWorldSkyRestHeight;
		const double Theta = V * UE_DOUBLE_PI;
		const double SinTheta = FMath::Sin(Theta);
		const double CosTheta = FMath::Cos(Theta);
		struct FHit
		{
			double T;
			int32 Node;
			bool bBack;
		};
		TArray<FHit, TInlineAllocator<32>> Hits;
		for (int32 ColumnIndex = 0; ColumnIndex < WoTWorldSkyRestWidth; ++ColumnIndex)
		{
			const double U = (static_cast<double>(ColumnIndex) + 0.5) / WoTWorldSkyRestWidth;
			const double Phi = (U - 0.5) * 2.0 * UE_DOUBLE_PI;
			FVector3d Direction(SinTheta * FMath::Cos(Phi), SinTheta * FMath::Sin(Phi),
				CosTheta);
			if (bRotated)
			{
				const double X = Direction.X * CosYaw - Direction.Y * SinYaw;
				const double Y = Direction.X * SinYaw + Direction.Y * CosYaw;
				Direction.X = X;
				Direction.Y = Y;
			}
			const int32 Tile = (RowIndex / WoTWorldSkyRestTile) * TileColumns
				+ ColumnIndex / WoTWorldSkyRestTile;
			// Covered: an OPAQUE (not translucent/modulated) sky-zone hit, any
			// order, front or back (skybake.py:571-575).
			bool bCovered = false;
			auto TestSky = [&](int32 CandidateIndex)
			{
				const FWoTWorldSkyRestNode& Candidate = SkyNodes[CandidateIndex];
				if (Candidate.Kind != 0) return;
				if (Candidate.CapCos >= -1.0
					&& FVector3d::DotProduct(Direction, Candidate.CapCenter) < Candidate.CapCos)
					return;
				double T = 0.0;
				bool bBack = false;
				if (WoTWorldSkyRestIntersect(Candidate, SkyPoints, Origin, Direction, T, bBack))
					bCovered = true;
			};
			for (int32 CandidateIndex : SkyAlways)
			{
				if (bCovered) break;
				TestSky(CandidateIndex);
			}
			for (int32 CandidateIndex : SkyTiles[Tile])
			{
				if (bCovered) break;
				TestSky(CandidateIndex);
			}
			if (bCovered)
			{
				++Out.Covered;
				continue;
			}
			Hits.Reset();
			auto TestRest = [&](int32 CandidateIndex)
			{
				const FWoTWorldSkyRestNode& Candidate = RestNodes[CandidateIndex];
				if (Candidate.bSkipped) return;
				if (Candidate.CapCos >= -1.0
					&& FVector3d::DotProduct(Direction, Candidate.CapCenter) < Candidate.CapCos)
					return;
				double T = 0.0;
				bool bBack = false;
				if (WoTWorldSkyRestIntersect(Candidate, RestPoints, Origin, Direction, T, bBack))
					Hits.Add({T, CandidateIndex, bBack});
			};
			for (int32 CandidateIndex : RestAlways) TestRest(CandidateIndex);
			for (int32 CandidateIndex : RestTiles[Tile]) TestRest(CandidateIndex);
			if (Hits.IsEmpty()) continue;
			// Python's stable sort by t over node-order insertion.
			Algo::Sort(Hits, [&RestNodes](const FHit& A, const FHit& B)
			{
				if (A.T != B.T) return A.T < B.T;
				return RestNodes[A.Node].NodeIndex < RestNodes[B.Node].NodeIndex;
			});
			TArray<int32, TInlineAllocator<16>> Layers;
			for (const FHit& Hit : Hits)
			{
				const FWoTWorldSkyRestNode& Node = RestNodes[Hit.Node];
				if (Node.bFakeBackdrop)
				{
					++Out.Surfaces.FindOrAdd(Node.Surface).FakeBackdrop;
					break;                       // sky re-entry window
				}
				if (Hit.bBack && !Node.bTwoSided)
				{
					if (Node.Kind == 0)
					{
						++Out.Surfaces.FindOrAdd(Node.Surface).Blocker;
						break;                   // solid space behind: void
					}
					continue;                    // one-sided T/M seen from behind
				}
				if (Node.Kind == 0)
				{
					++Out.Surfaces.FindOrAdd(Node.Surface).Visible;
					break;
				}
				Layers.Add(Node.Surface);
			}
			for (const int32 Layer : Layers)
				++Out.Surfaces.FindOrAdd(Layer).Visible;
		}
	});
	for (const FRowResult& Row : Rows)
	{
		Census.CoveredRays += Row.Covered;
		for (const TPair<int32, FWoTWorldSkyRestSurfaceCount>& Pair : Row.Surfaces)
		{
			FWoTWorldSkyRestSurfaceCount& Total = Census.Surfaces.FindOrAdd(Pair.Key);
			Total.Visible += Pair.Value.Visible;
			Total.Blocker += Pair.Value.Blocker;
			Total.FakeBackdrop += Pair.Value.FakeBackdrop;
		}
	}
	Census.Surfaces.KeySort([](int32 A, int32 B) { return A < B; });
	return Census;
}

/** The sky-rest trailer (BSP v6, flag 32, the main part only): the census and
 * the live rest geometry skygeo.py:185-224 emits, grouped as its GLB groups -
 * "SkyRest__<world slot>" (visible; bound to that world slot's material),
 * "SkyRestFB" (hidden nested sky windows), "SkyRestBlock" (flipped-winding
 * opaque black solid-space blockers). Empty when the map has no sky zone. */
FWoTBootstrapStatus WoTWorldBuildSkyRestTrailer(const FWoTWorldSource& Source,
	const FWoTWorldModel& Model, const FWoTWorldAtlas& Atlas,
	FWoTWorldSkyRest& OutCensus, TArray<uint8>& OutTrailer)
{
	OutTrailer.Reset();
	OutCensus = WoTWorldCensusSkyRest(Source, Model);
	if (!OutCensus.bPresent) return FWoTBootstrapStatus::Success();
	struct FGroup
	{
		EWoTWorldSkyRestRole Role = EWoTWorldSkyRestRole::Visible;
		FString Slot;
		TArray<FWoTWorldTriangle> Triangles;
	};
	// skygeo.py:186-192: a surface touching the sky zone is a sky surface and
	// is emitted as its sky slot, never as rest content (the census still
	// counts its rest-zone nodes, exactly as skybake's JSON does).
	TSet<int32> SkySurfaces;
	for (const FWoTWorldNode& Node : Model.Nodes)
	{
		if (Node.VertexCount >= 3
			&& (Node.Zones[0] == OutCensus.SkyZone || Node.Zones[1] == OutCensus.SkyZone))
			SkySurfaces.Add(Node.Surface);
	}
	TMap<FString, FGroup> Groups;
	uint64 TriangleTotal = 0;
	for (int32 NodeIndex = 0; NodeIndex < Model.Nodes.Num(); ++NodeIndex)
	{
		const FWoTWorldNode& Node = Model.Nodes[NodeIndex];
		if (Node.VertexCount < 3 || SkySurfaces.Contains(Node.Surface)) continue;
		const FWoTWorldSkyRestSurfaceCount* Counts = OutCensus.Surfaces.Find(Node.Surface);
		if (Counts == nullptr) continue;
		const FWoTWorldSurface& Surface = Model.Surfaces[Node.Surface];
		if ((Surface.Flags & WoTWorldPFInvisible) != 0) continue;
		TArray<FWoTWorldTriangle> Triangles;
		FWoTBootstrapStatus Built =
			WoTWorldBuildNodeTriangles(Source, Model, Atlas, Node, Triangles);
		if (Built.IsError()) return Built;
		// skygeo.py:193-205: visible first, else a FakeBackdrop window.
		if (Counts->Visible > 0 || Counts->FakeBackdrop > 0)
		{
			const bool bVisible = Counts->Visible > 0;
			const FString Slot = WoTWorldMaterialSlot(Source, Surface);
			const FString Key = bVisible ? TEXT("SkyRest__") + Slot : TEXT("SkyRestFB");
			FGroup& Group = Groups.FindOrAdd(Key);
			Group.Role = bVisible ? EWoTWorldSkyRestRole::Visible
				: EWoTWorldSkyRestRole::FakeBackdrop;
			Group.Slot = bVisible ? Slot : FString();
			Group.Triangles.Append(Triangles);
			TriangleTotal += Triangles.Num();
		}
		// skygeo.py:206-213: a blocker also gets a flipped opaque black copy.
		if (Counts->Blocker > 0)
		{
			FGroup& Group = Groups.FindOrAdd(TEXT("SkyRestBlock"));
			Group.Role = EWoTWorldSkyRestRole::Blocker;
			for (FWoTWorldTriangle Triangle : Triangles)
			{
				Swap(Triangle.Positions[0], Triangle.Positions[1]);
				Swap(Triangle.UV0[0], Triangle.UV0[1]);
				Swap(Triangle.UV1[0], Triangle.UV1[1]);
				Triangle.Normal = -Triangle.Normal;
				Group.Triangles.Add(Triangle);
			}
			TriangleTotal += Triangles.Num();
		}
		if (TriangleTotal > WoTWorldSkyRestMaxTriangles)
		{
			return FWoTBootstrapStatus::Error(WoTWorldError(EWoTBootstrapError::SizeLimit,
				TEXT("the sky-rest geometry exceeds its wire bound"), Source.SourcePath));
		}
	}
	TArray<FString> Keys;
	Groups.GetKeys(Keys);
	Keys.Sort([](const FString& A, const FString& B)
	{
		return FCString::Strcmp(*A, *B) < 0;   // skygeo sorted(groups)
	});
	TArray<uint8>& Trailer = OutTrailer;
	WoTWorldAppendU32(Trailer, 0);
	WoTWorldAppendU32(Trailer, static_cast<uint32>(OutCensus.SkyZone));
	WoTWorldAppendU32(Trailer, OutCensus.RayCount);
	WoTWorldAppendU32(Trailer, OutCensus.CoveredRays);
	WoTWorldAppendU32(Trailer, static_cast<uint32>(OutCensus.Surfaces.Num()));
	for (const TPair<int32, FWoTWorldSkyRestSurfaceCount>& Pair : OutCensus.Surfaces)
	{
		WoTWorldAppendU32(Trailer, static_cast<uint32>(Pair.Key));
		WoTWorldAppendU32(Trailer, Pair.Value.Visible);
		WoTWorldAppendU32(Trailer, Pair.Value.Blocker);
		WoTWorldAppendU32(Trailer, Pair.Value.FakeBackdrop);
	}
	WoTWorldAppendU32(Trailer, static_cast<uint32>(Keys.Num()));
	for (const FString& Key : Keys)
	{
		const FGroup& Group = Groups[Key];
		FTCHARToUTF8 SlotBytes(*Group.Slot);
		if (SlotBytes.Length() > static_cast<int32>(WoTWorldMaxSlotNameBytes)
			|| (Group.Role == EWoTWorldSkyRestRole::Visible) != (SlotBytes.Length() > 0))
		{
			return FWoTBootstrapStatus::Error(WoTWorldError(EWoTBootstrapError::SizeLimit,
				TEXT("a sky-rest group slot violates its wire bound"), Key));
		}
		Trailer.Add(static_cast<uint8>(Group.Role));
		Trailer.Add(0);
		WoTWorldAppendU16(Trailer, static_cast<uint16>(SlotBytes.Length()));
		Trailer.Append(reinterpret_cast<const uint8*>(SlotBytes.Get()), SlotBytes.Length());
		WoTWorldAppendU32(Trailer, static_cast<uint32>(Group.Triangles.Num()));
		for (const FWoTWorldTriangle& Triangle : Group.Triangles)
		{
			for (float Value : {Triangle.Normal.X, Triangle.Normal.Y, Triangle.Normal.Z})
				WoTWorldAppendFloat(Trailer, Value);
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				const FVector3f& Position = Triangle.Positions[Corner];
				for (float Value : {Position.X, Position.Y, Position.Z,
					Triangle.UV0[Corner].X, Triangle.UV0[Corner].Y,
					Triangle.UV1[Corner].X, Triangle.UV1[Corner].Y})
					WoTWorldAppendFloat(Trailer, Value);
			}
		}
	}
	if (static_cast<uint64>(Trailer.Num()) > MAX_uint32)
	{
		return FWoTBootstrapStatus::Error(WoTWorldError(EWoTBootstrapError::SizeLimit,
			TEXT("the sky-rest trailer exceeds its wire bound"), Source.SourcePath));
	}
	const uint32 TrailerByteCount = static_cast<uint32>(Trailer.Num());
	Trailer[0] = static_cast<uint8>(TrailerByteCount);
	Trailer[1] = static_cast<uint8>(TrailerByteCount >> 8);
	Trailer[2] = static_cast<uint8>(TrailerByteCount >> 16);
	Trailer[3] = static_cast<uint8>(TrailerByteCount >> 24);
	return FWoTBootstrapStatus::Success();
}

TWoTBootstrapResult<TArray<FWoTWorldBSPPartData>> WoTWorldEncodeBSP(
	const FWoTWorldSource& Source, const FWoTWorldModel& Model,
	const FWoTWorldComposedAtlas& Lighting, bool bPortal, bool bSplitProcedural)
{
	using FResult = TWoTBootstrapResult<TArray<FWoTWorldBSPPartData>>;
	const FWoTWorldAtlas& Atlas = Lighting.Atlas;
	const bool bEntryLighting = Source.MapIdentity == TEXT("WheelOfTime");
	const TArray<FWoTWorldEntryLight>& LightningLights = Lighting.LightningLights;
	const TArray<FWoTWorldEntryIllumination>& WeatherPairs = Lighting.WeatherPairs;
	const float LevelBrightness = Lighting.LevelBrightness;
	for (const FWoTWorldNode& Node : Model.Nodes)
	{
		if (Node.VertexCount >= 3
			&& Model.Surfaces[Node.Surface].Texture != 0
			&& WoTWorldTexture(
				Source, Model.Surfaces[Node.Surface].Texture) == nullptr)
		{
			return FResult::Error(WoTWorldError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("a BSP texture is absent from the canonical resolver projection"),
				Source.SourcePath));
		}
	}
	const TSet<int32> SkySurfaces = WoTWorldSkySurfaces(Source, Model);
	TSet<FString> SkyMaterials;
	TSet<FString> WorldMaterials;
	for (const FWoTWorldNode& Node : Model.Nodes)
	{
		if (Node.VertexCount < 3) continue;
		const FWoTWorldSurface& Surface = Model.Surfaces[Node.Surface];
		if ((Surface.Flags & WoTWorldPFInvisible) != 0) continue;
		const FString Slot = WoTWorldMaterialSlot(Source, Surface);
		(SkySurfaces.Contains(Node.Surface) ? SkyMaterials : WorldMaterials).Add(Slot);
	}
	TSet<FString> SplitSkyMaterials;
	for (const FString& Slot : SkyMaterials)
		if (WorldMaterials.Contains(Slot)) SplitSkyMaterials.Add(Slot);
	// Most macro materials have one authored pan transform and retain their
	// canonical slot name. Split a slot only when this part actually contains
	// conflicting transforms; the section record then carries each exact pair.
	TMap<FString, TSet<int64>> MacroPansBySlot;
	for (const FWoTWorldNode& Node : Model.Nodes)
	{
		if (Node.VertexCount < 3) continue;
		const FWoTWorldSurface& Surface = Model.Surfaces[Node.Surface];
		if ((Surface.Flags & WoTWorldPFInvisible) != 0
			|| ((Surface.Flags & WoTWorldPFPortal) != 0) != bPortal) continue;
		const WoT::Retail::Generated::FWorldTexture* Texture =
			WoTWorldTexture(Source, Surface.Texture);
		if (Texture == nullptr || Texture->MacroWidth == 0
			|| Texture->MacroDrawScale <= 0.0) continue;
		FString Slot = WoTWorldMaterialSlot(Source, Surface);
		if (SkySurfaces.Contains(Node.Surface) && SplitSkyMaterials.Contains(Slot))
			Slot += TEXT("__SkyZone");
		const uint64 U = static_cast<uint16>(Surface.PanU);
		const uint64 V = static_cast<uint16>(Surface.PanV);
		MacroPansBySlot.FindOrAdd(Slot).Add(static_cast<int64>((U << 16) | V));
	}
	// bsp2obj accumulates pan-zone votes before it separates main and portal
	// geometry. Preserve that shared slot identity so a portal last-writer can
	// inherit the authored AutoU/VPan zone from a main surface of the same slot.
	TMap<FString, FWoTWorldBSPSectionIdentity> SharedPanZones;
	for (const FWoTWorldNode& Node : Model.Nodes)
	{
		if (Node.VertexCount < 3) continue;
		const FWoTWorldSurface& Surface = Model.Surfaces[Node.Surface];
		if ((Surface.Flags & WoTWorldPFInvisible) != 0
			|| (Surface.Flags & (WoTWorldPFAutoUPan | WoTWorldPFAutoVPan)) == 0)
			continue;
		FString Slot = WoTWorldMaterialSlot(Source, Surface);
		if (SkySurfaces.Contains(Node.Surface) && SplitSkyMaterials.Contains(Slot))
			Slot += TEXT("__SkyZone");
		FWoTWorldBSPSectionIdentity& PanZones = SharedPanZones.FindOrAdd(Slot);
		for (uint8 ZoneIndex : Node.Zones)
		{
			if (ZoneIndex == 0 || !Model.Zones.IsValidIndex(ZoneIndex)) continue;
			const int32 ZoneActor = Model.Zones[ZoneIndex].Actor;
			if (ZoneActor == 0) continue;
			TWoTBootstrapResult<FString> ZonePath =
				WoTWorldObjectPath(Source, ZoneActor);
			if (ZonePath.IsError())
				return FResult::Error(MoveTemp(ZonePath).GetError());
			FString ZoneIdentity = MoveTemp(ZonePath).GetValue();
			if (!PanZones.PanZoneVotes.Contains(ZoneIdentity))
				PanZones.PanZoneOrder.Add(ZoneIdentity);
			++PanZones.PanZoneVotes.FindOrAdd(ZoneIdentity);
		}
	}

	TMap<FString, TArray<FWoTWorldTriangle>> Groups;
	TMap<FString, FWoTWorldBSPSectionIdentity> SectionIdentities;
	TSet<FString> ProceduralSlots;
	for (const FWoTWorldNode& Node : Model.Nodes)
	{
		if (Node.VertexCount < 3) continue;
		const FWoTWorldSurface& Surface = Model.Surfaces[Node.Surface];
		if ((Surface.Flags & WoTWorldPFInvisible) != 0
			|| ((Surface.Flags & WoTWorldPFPortal) != 0) != bPortal) continue;
		TArray<FWoTWorldTriangle> NodeTriangles;
		FWoTBootstrapStatus BuiltTriangles =
			WoTWorldBuildNodeTriangles(Source, Model, Atlas, Node, NodeTriangles);
		if (BuiltTriangles.IsError())
			return FResult::Error(MoveTemp(BuiltTriangles).GetError());
		const WoT::Retail::Generated::FWorldTexture* Texture =
			WoTWorldTexture(Source, Surface.Texture);
		FString Slot = WoTWorldMaterialSlot(Source, Surface);
		if (SkySurfaces.Contains(Node.Surface) && SplitSkyMaterials.Contains(Slot))
			Slot += TEXT("__SkyZone");
		const FString SharedSlot = Slot;
		const FString CanonicalPackage = Texture != nullptr
			? FString(Texture->CanonicalPackage) : FString();
		const FString CanonicalName = Texture != nullptr
			? FString(Texture->CanonicalName) : TEXT("None");
		FString SourceTextureIdentity;
		if (Surface.Texture == 0)
		{
			SourceTextureIdentity = TEXT("None");
		}
		else
		{
			TWoTBootstrapResult<FString> SourceTexture =
				WoTWorldObjectPath(Source, Surface.Texture);
			if (SourceTexture.IsError())
				return FResult::Error(MoveTemp(SourceTexture).GetError());
			SourceTextureIdentity = MoveTemp(SourceTexture).GetValue();
		}
		const FString MacroCanonicalPackage = Texture != nullptr
			? FString(Texture->MacroCanonicalPackage) : FString();
		const FString MacroCanonicalName = Texture != nullptr
			? FString(Texture->MacroCanonicalName) : FString();
		const FString MacroSourceTextureIdentity = Texture != nullptr
			? FString(Texture->MacroSourceTextureIdentity) : FString();
		// The generated registry stores the UE1 macro-texture edge once because
		// every authored world macro texture is square.  Use that edge for both
		// axes, matching the single retail USize/DrawScale ratio already carried
		// by the payload.
		const float MacroRatio = Texture != nullptr && Texture->MacroWidth != 0
			&& Texture->MacroDrawScale > 0.0
			? static_cast<float>((static_cast<double>(Texture->Width)
				* Texture->DrawScale) / (static_cast<double>(Texture->MacroWidth)
					* Texture->MacroDrawScale)) : 0.0f;
		const float MacroOffsetU = MacroRatio > 0.0f
			? static_cast<float>(static_cast<double>(Surface.PanU)
				/ (static_cast<double>(Texture->MacroWidth)
					* Texture->MacroDrawScale)) : 0.0f;
		const float MacroOffsetV = MacroRatio > 0.0f
			? static_cast<float>(static_cast<double>(Surface.PanV)
				/ (static_cast<double>(Texture->MacroWidth)
					* Texture->MacroDrawScale)) : 0.0f;
		const TSet<int64>* SlotMacroPans = MacroPansBySlot.Find(SharedSlot);
		if (MacroRatio > 0.0f && SlotMacroPans != nullptr
			&& SlotMacroPans->Num() > 1)
		{
			const FString PanSuffix = FString::Printf(TEXT("__MacroPan_%d_%d"),
				static_cast<int32>(Surface.PanU), static_cast<int32>(Surface.PanV));
			const FString SkySuffix(TEXT("__SkyZone"));
			if (Slot.EndsWith(SkySuffix, ESearchCase::CaseSensitive))
			{
				Slot.LeftChopInline(SkySuffix.Len(), EAllowShrinking::No);
				Slot += PanSuffix + SkySuffix;
			}
			else
			{
				Slot += PanSuffix;
			}
		}
		if (bSplitProcedural && Texture != nullptr && Texture->bProcedural)
			ProceduralSlots.Add(Slot);
		FWoTWorldBSPSectionIdentity& Identity = SectionIdentities.FindOrAdd(Slot);
		constexpr uint32 MaterialFamilyMask = WoTWorldPFMasked
			| WoTWorldPFTranslucent | WoTWorldPFModulated | WoTWorldPFTwoSided
			| WoTWorldPFUnlit | WoTWorldPFFakeBackdrop | WoTWorldPFSpecialLit;
		if (!Identity.SourceTextureIdentity.IsEmpty()
			&& (Identity.CanonicalPackage != CanonicalPackage
				|| Identity.CanonicalName != CanonicalName
				|| Identity.SourceTextureIdentity != SourceTextureIdentity
				|| Identity.MacroCanonicalPackage != MacroCanonicalPackage
				|| Identity.MacroCanonicalName != MacroCanonicalName
				|| Identity.MacroSourceTextureIdentity != MacroSourceTextureIdentity
				|| Identity.MacroRatio != MacroRatio
				|| Identity.MacroOffsetU != MacroOffsetU
				|| Identity.MacroOffsetV != MacroOffsetV
				|| (Identity.SurfaceFlags & MaterialFamilyMask)
					!= (Surface.Flags & MaterialFamilyMask)))
		{
			return FResult::Error(WoTWorldError(
				EWoTBootstrapError::DuplicateField,
				TEXT("a BSP material slot aliases more than one identity/flag family"),
				Slot));
		}
		Identity.CanonicalPackage = CanonicalPackage;
		Identity.CanonicalName = CanonicalName;
		Identity.SourceTextureIdentity = MoveTemp(SourceTextureIdentity);
		Identity.MacroCanonicalPackage = MacroCanonicalPackage;
		Identity.MacroCanonicalName = MacroCanonicalName;
		Identity.MacroSourceTextureIdentity = MacroSourceTextureIdentity;
		Identity.MacroRatio = MacroRatio;
		Identity.MacroOffsetU = MacroOffsetU;
		Identity.MacroOffsetV = MacroOffsetV;
		Identity.SurfaceFlags = Surface.Flags;
		if (const FWoTWorldBSPSectionIdentity* PanZones = SharedPanZones.Find(SharedSlot))
		{
			Identity.PanZoneVotes = PanZones->PanZoneVotes;
			Identity.PanZoneOrder = PanZones->PanZoneOrder;
		}
		Groups.FindOrAdd(Slot).Append(MoveTemp(NodeTriangles));
	}
	TArray<FString> Slots;
	Groups.GetKeys(Slots);
	// Python's sorted() orders these ASCII identities by exact code point.
	// Spell that rule explicitly instead of inheriting a platform FString
	// collation default: section order is part of the byte contract.
	Slots.Sort([](const FString& A, const FString& B)
	{
		return FCString::Strcmp(*A, *B) < 0;
	});
	for (int32 Index = Slots.Num() - 1; Index >= 0; --Index)
	{
		if (Groups[Slots[Index]].IsEmpty()) Slots.RemoveAt(Index);
	}
	const FWoTWorldAnimationManifest& Manifest = Lighting.Manifest;
	const TArray<FWoTWorldResponseTerm>& Responses = Lighting.Responses;
	const TArray<uint8>& SkyRestTrailer = Lighting.SkyRestTrailer;
	auto EncodePart = [&Groups, &SectionIdentities, bPortal, &Source, &Model,
		&Atlas, bEntryLighting, &Manifest, &Responses,
		&LightningLights, &WeatherPairs, LevelBrightness, &SkyRestTrailer](
		const TArray<FString>& PartSlots, FString PartIdentity,
		FString ProceduralSlot) -> TWoTBootstrapResult<FWoTWorldBSPPartData>
	{
		using FPartResult = TWoTBootstrapResult<FWoTWorldBSPPartData>;
		uint64 TriangleCount = 0;
		for (const FString& Slot : PartSlots) TriangleCount += Groups[Slot].Num();
		const uint64 VertexCount64 = TriangleCount * 3;
		const uint64 IndexCount64 = TriangleCount * 3;
		if (PartSlots.IsEmpty()
			|| PartSlots.Num() > static_cast<int32>(WoTWorldMaxBSPSections)
			|| VertexCount64 > WoTWorldMaxBSPVertices
			|| IndexCount64 > WoTWorldMaxBSPIndices)
		{
			return FPartResult::Error(WoTWorldError(EWoTBootstrapError::SizeLimit,
				TEXT("a decoded BSP part exceeds the shared payload bounds"),
				Source.SourcePath));
		}
		const uint32 VertexCount = static_cast<uint32>(VertexCount64);
		const uint32 IndexCount = static_cast<uint32>(IndexCount64);
		FVector3f BoundsMin(MAX_flt, MAX_flt, MAX_flt);
		FVector3f BoundsMax(-MAX_flt, -MAX_flt, -MAX_flt);
		for (const FString& Slot : PartSlots)
			for (const FWoTWorldTriangle& Triangle : Groups[Slot])
				for (const FVector3f& Position : Triangle.Positions)
				{
					BoundsMin.X = FMath::Min(BoundsMin.X, Position.X);
					BoundsMin.Y = FMath::Min(BoundsMin.Y, Position.Y);
					BoundsMin.Z = FMath::Min(BoundsMin.Z, Position.Z);
					BoundsMax.X = FMath::Max(BoundsMax.X, Position.X);
					BoundsMax.Y = FMath::Max(BoundsMax.Y, Position.Y);
					BoundsMax.Z = FMath::Max(BoundsMax.Z, Position.Z);
				}
		const bool bHasAtlas = bEntryLighting
			|| (!bPortal && PartIdentity == Source.MapIdentity);
		FWoTWorldBSPPartData Part;
		Part.Role = bPortal ? EWoTBSPPartRole::Portal : EWoTBSPPartRole::Main;
		Part.PartIdentity = MoveTemp(PartIdentity);
		Part.ProceduralMaterialSlot = MoveTemp(ProceduralSlot);
		Part.VertexCount = VertexCount;
		Part.IndexCount = IndexCount;
		Part.SectionCount = static_cast<uint32>(PartSlots.Num());
		TArray<uint8>& Payload = Part.Payload;
		const bool bHasAnimation = bHasAtlas && !bEntryLighting
			&& !Manifest.Terms.IsEmpty();
		// Every multiplayer part carries exact MacroTexture records and the v10
		// triangle-to-model-surface provenance trailer. The entry map remains on
		// v5 because it already carries the same provenance in its weather trailer.
		const bool bHasSkyRest = bHasAtlas && !bEntryLighting
			&& !SkyRestTrailer.IsEmpty();
		Payload.Append(reinterpret_cast<const uint8*>(WoTWorldBSPMagic), 8);
		WoTWorldAppendU16(Payload, bEntryLighting
			? WoTWorldEntryBSPPayloadVersion : WoTWorldBSPPayloadVersion);
		WoTWorldAppendU16(Payload, static_cast<uint16>((bPortal ? 1 : 0) | 2
			| (bHasAtlas ? 4 : 0) | (bEntryLighting ? 8 : 0)
			| (bHasAnimation ? 16 : 0) | (bHasSkyRest ? 32 : 0)
			| (!bEntryLighting ? 64 : 0)));
		WoTWorldAppendU32(Payload, VertexCount);
		WoTWorldAppendU32(Payload, IndexCount);
		WoTWorldAppendU32(Payload, Part.SectionCount);
		WoTWorldAppendU16(Payload, 2);
		WoTWorldAppendU16(Payload, 1);
		for (float Value : {BoundsMin.X, BoundsMin.Y, BoundsMin.Z,
			BoundsMax.X, BoundsMax.Y, BoundsMax.Z}) WoTWorldAppendFloat(Payload, Value);
		uint32 FirstIndex = 0;
		uint32 BaseVertex = 0;
		for (const FString& Slot : PartSlots)
		{
			FTCHARToUTF8 Encoded(*Slot);
			const FWoTWorldBSPSectionIdentity* Identity =
				SectionIdentities.Find(Slot);
			const uint32 Count = static_cast<uint32>(Groups[Slot].Num() * 3);
			if (Identity == nullptr || Identity->CanonicalName.IsEmpty()
				|| Identity->SourceTextureIdentity.IsEmpty()
				|| Encoded.Length() <= 0
				|| Encoded.Length() > static_cast<int32>(WoTWorldMaxSlotNameBytes))
			{
				return FPartResult::Error(WoTWorldError(EWoTBootstrapError::SizeLimit,
					TEXT("a BSP material slot exceeds the shared payload bound"), Slot));
			}
			WoTWorldAppendU16(Payload, static_cast<uint16>(Encoded.Length()));
			WoTWorldAppendU16(Payload, 0);
			WoTWorldAppendU32(Payload, FirstIndex);
			WoTWorldAppendU32(Payload, Count);
			WoTWorldAppendU32(Payload, BaseVertex);
			WoTWorldAppendU32(Payload, BaseVertex + Count - 1);
			Payload.Add(bPortal ? 0 : 1);
			Payload.AddZeroed(3);
			Payload.Append(reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length());
			FString PanZoneIdentity;
			uint32 PanZoneVotes = 0;
			for (const FString& Candidate : Identity->PanZoneOrder)
			{
				const uint32 CandidateVotes =
					Identity->PanZoneVotes.FindRef(Candidate);
				if (CandidateVotes > PanZoneVotes)
				{
					PanZoneVotes = CandidateVotes;
					PanZoneIdentity = Candidate;
				}
			}
			FTCHARToUTF8 PackageBytes(*Identity->CanonicalPackage);
			FTCHARToUTF8 NameBytes(*Identity->CanonicalName);
			FTCHARToUTF8 SourceBytes(*Identity->SourceTextureIdentity);
			FTCHARToUTF8 PanBytes(*PanZoneIdentity);
			const int32 IdentityLengths[] = {PackageBytes.Length(), NameBytes.Length(),
				SourceBytes.Length(), PanBytes.Length()};
			if (!WoTWorldSectionIdentityLengthsValid(IdentityLengths))
			{
				return FPartResult::Error(WoTWorldError(
					EWoTBootstrapError::SizeLimit,
					TEXT("a BSP section identity exceeds its shared byte bound"), Slot));
			}
			WoTWorldAppendU16(Payload, static_cast<uint16>(PackageBytes.Length()));
			WoTWorldAppendU16(Payload, static_cast<uint16>(NameBytes.Length()));
			WoTWorldAppendU16(Payload, static_cast<uint16>(SourceBytes.Length()));
			WoTWorldAppendU16(Payload, static_cast<uint16>(PanBytes.Length()));
			WoTWorldAppendU32(Payload, Identity->SurfaceFlags);
			Payload.Append(reinterpret_cast<const uint8*>(PackageBytes.Get()),
				PackageBytes.Length());
			Payload.Append(reinterpret_cast<const uint8*>(NameBytes.Get()),
				NameBytes.Length());
			Payload.Append(reinterpret_cast<const uint8*>(SourceBytes.Get()),
				SourceBytes.Length());
			Payload.Append(reinterpret_cast<const uint8*>(PanBytes.Get()),
				PanBytes.Length());
			if (!bEntryLighting)
			{
				FTCHARToUTF8 MacroPackageBytes(*Identity->MacroCanonicalPackage);
				FTCHARToUTF8 MacroNameBytes(*Identity->MacroCanonicalName);
				FTCHARToUTF8 MacroSourceBytes(*Identity->MacroSourceTextureIdentity);
				const int32 MacroLengths[] = {MacroPackageBytes.Length(),
					MacroNameBytes.Length(), MacroSourceBytes.Length(), 0};
				const bool bHasMacro = MacroNameBytes.Length() != 0;
				if (!WoTWorldSectionIdentityLengthsValid(MacroLengths)
					|| bHasMacro != (MacroPackageBytes.Length() != 0)
					|| bHasMacro != (MacroSourceBytes.Length() != 0)
					|| (bHasMacro && (!FMath::IsFinite(Identity->MacroRatio)
						|| !FMath::IsFinite(Identity->MacroOffsetU)
						|| !FMath::IsFinite(Identity->MacroOffsetV)
						|| Identity->MacroRatio <= 0.0f))
					|| (!bHasMacro && (Identity->MacroRatio != 0.0f
						|| Identity->MacroOffsetU != 0.0f
						|| Identity->MacroOffsetV != 0.0f)))
				{
					return FPartResult::Error(WoTWorldError(
						EWoTBootstrapError::SizeLimit,
						TEXT("a BSP macro identity violates its wire contract"), Slot));
				}
				WoTWorldAppendU16(Payload,
					static_cast<uint16>(MacroPackageBytes.Length()));
				WoTWorldAppendU16(Payload,
					static_cast<uint16>(MacroNameBytes.Length()));
				WoTWorldAppendU16(Payload,
					static_cast<uint16>(MacroSourceBytes.Length()));
				WoTWorldAppendU16(Payload, 0);
				WoTWorldAppendFloat(Payload, Identity->MacroRatio);
				WoTWorldAppendFloat(Payload, Identity->MacroOffsetU);
				WoTWorldAppendFloat(Payload, Identity->MacroOffsetV);
				Payload.Append(reinterpret_cast<const uint8*>(MacroPackageBytes.Get()),
					MacroPackageBytes.Length());
				Payload.Append(reinterpret_cast<const uint8*>(MacroNameBytes.Get()),
					MacroNameBytes.Length());
				Payload.Append(reinterpret_cast<const uint8*>(MacroSourceBytes.Get()),
					MacroSourceBytes.Length());
			}
			FirstIndex += Count;
			BaseVertex += Count;
		}
		for (const FString& Slot : PartSlots)
			for (const FWoTWorldTriangle& Triangle : Groups[Slot])
				for (const FVector3f& Position : Triangle.Positions)
					for (float Value : {Position.X, Position.Y, Position.Z})
						WoTWorldAppendFloat(Payload, Value);
		for (const FString& Slot : PartSlots)
			for (const FWoTWorldTriangle& Triangle : Groups[Slot])
				for (int32 Corner = 0; Corner < 3; ++Corner)
					for (float Value : {Triangle.Normal.X, Triangle.Normal.Y, Triangle.Normal.Z})
						WoTWorldAppendFloat(Payload, Value);
		for (const FString& Slot : PartSlots)
			for (const FWoTWorldTriangle& Triangle : Groups[Slot])
				for (const FVector2f& UV : Triangle.UV0)
				{
					WoTWorldAppendFloat(Payload, UV.X);
					WoTWorldAppendFloat(Payload, UV.Y);
				}
		for (const FString& Slot : PartSlots)
			for (const FWoTWorldTriangle& Triangle : Groups[Slot])
				for (const FVector2f& UV : Triangle.UV1)
				{
					WoTWorldAppendFloat(Payload, UV.X);
					WoTWorldAppendFloat(Payload, UV.Y);
				}
		for (uint32 Index = 0; Index < IndexCount; ++Index)
			WoTWorldAppendU32(Payload, Index);
		if (bHasAtlas)
		{
			const uint64 ExpectedBytes =
				static_cast<uint64>(Atlas.Width) * Atlas.Height * 4;
			if (Atlas.Width < 1 || Atlas.Width > 8192
				|| Atlas.Height < 1 || Atlas.Height > 8192
				|| static_cast<uint64>(Atlas.Width) * Atlas.Height > 4194304ull
				|| ExpectedBytes != static_cast<uint64>(Atlas.BGRA.Num())
				|| ExpectedBytes > MAX_uint32)
			{
				return FPartResult::Error(WoTWorldError(
					EWoTBootstrapError::SizeLimit,
					TEXT("the BSP light-map atlas violates its wire bounds"),
					Source.SourcePath));
			}
			WoTWorldAppendU32(Payload, static_cast<uint32>(Atlas.Width));
			WoTWorldAppendU32(Payload, static_cast<uint32>(Atlas.Height));
			WoTWorldAppendU32(Payload, static_cast<uint32>(ExpectedBytes));
			Payload.Append(Atlas.BGRA);
		}

		if (bEntryLighting)
		{
			if (Model.Surfaces.IsEmpty() || Model.Surfaces.Num() > 65536
				|| LightningLights.Num() > 8 || WeatherPairs.Num() > 4096)
			{
				return FPartResult::Error(WoTWorldError(
					EWoTBootstrapError::SizeLimit,
					TEXT("the entry BSP weather data violates its v3 wire bounds"),
					Source.SourcePath));
			}
			TArray<uint8> Trailer;
			WoTWorldAppendU32(Trailer, 0);
			WoTWorldAppendU32(Trailer, static_cast<uint32>(TriangleCount));
			WoTWorldAppendU32(Trailer, static_cast<uint32>(Model.Surfaces.Num()));
			WoTWorldAppendU32(Trailer, static_cast<uint32>(LightningLights.Num()));
			WoTWorldAppendU32(Trailer, static_cast<uint32>(WeatherPairs.Num()));
			WoTWorldAppendU32(Trailer, Model.bRootOutside ? 1u : 0u);
			for (const FString& Slot : PartSlots)
				for (const FWoTWorldTriangle& Triangle : Groups[Slot])
					WoTWorldAppendU32(Trailer, Triangle.SurfaceIndex);
			for (int32 SurfaceIndex = 0;
				SurfaceIndex < Model.Surfaces.Num(); ++SurfaceIndex)
			{
				const FWoTWorldAtlasEntry& Entry = Atlas.Surfaces[SurfaceIndex];
				WoTWorldAppendU32(Trailer, static_cast<uint32>(SurfaceIndex));
				WoTWorldAppendU32(Trailer, Entry.bPresent
					? static_cast<uint32>(Entry.X) : 0u);
				WoTWorldAppendU32(Trailer, Entry.bPresent
					? static_cast<uint32>(Entry.Y) : 0u);
				WoTWorldAppendU32(Trailer, Entry.bPresent
					? static_cast<uint32>(Entry.W) : 0u);
				WoTWorldAppendU32(Trailer, Entry.bPresent
					? static_cast<uint32>(Entry.H) : 0u);
				WoTWorldAppendFloat(Trailer, Entry.PanUV.X);
				WoTWorldAppendFloat(Trailer, Entry.PanUV.Y);
			}
			for (const FWoTWorldEntryLight& Light : LightningLights)
			{
				FTCHARToUTF8 Encoded(*Light.Identity);
				if (Encoded.Length() <= 0 || Encoded.Length() > 1024)
				{
					return FPartResult::Error(WoTWorldError(
						EWoTBootstrapError::SizeLimit,
						TEXT("an entry Lightning identity violates its v3 wire bound"),
						Light.Identity));
				}
				WoTWorldAppendU16(Trailer, static_cast<uint16>(Encoded.Length()));
				WoTWorldAppendU16(Trailer, 0);
				Trailer.Append(reinterpret_cast<const uint8*>(Encoded.Get()),
					Encoded.Length());
				Trailer.Add(Light.Brightness);
				Trailer.Add(Light.Hue);
				Trailer.Add(Light.Saturation);
				Trailer.Add(0);
				WoTWorldAppendFloat(Trailer, LevelBrightness);
			}
			for (const FWoTWorldEntryIllumination& Pair : WeatherPairs)
			{
				WoTWorldAppendU32(Trailer, Pair.LightIndex);
				WoTWorldAppendU32(Trailer, Pair.SurfaceIndex);
				WoTWorldAppendU32(
					Trailer, static_cast<uint32>(Pair.Illumination.Num()));
				Trailer.Append(Pair.Illumination);
			}
			if (static_cast<uint64>(Trailer.Num()) > MAX_uint32
				|| static_cast<uint64>(Payload.Num()) + Trailer.Num()
					> WoTWorldMaxSourceBytes)
			{
				return FPartResult::Error(WoTWorldError(
					EWoTBootstrapError::SizeLimit,
					TEXT("the entry BSP v3 payload exceeds its overall wire bound"),
					Source.SourcePath));
			}
			const uint32 TrailerByteCount = static_cast<uint32>(Trailer.Num());
			Trailer[0] = static_cast<uint8>(TrailerByteCount);
			Trailer[1] = static_cast<uint8>(TrailerByteCount >> 8);
			Trailer[2] = static_cast<uint8>(TrailerByteCount >> 16);
			Trailer[3] = static_cast<uint8>(TrailerByteCount >> 24);
			Payload.Append(Trailer);
		}
		if (bHasAnimation)
		{
			// Animation trailer: u32 byte count; the atlas placement of every
			// model surface; the waveform group table; the response terms with
			// their sparse per-surface unsigned scale-2 RGB rects.
			if (Model.Surfaces.IsEmpty() || Model.Surfaces.Num() > 65536
				|| Manifest.Groups.Num() > WoTWorldMaxWaveformGroups
				|| Manifest.Terms.Num() > WoTWorldMaxResponseTerms
				|| Responses.Num() != Manifest.Terms.Num())
			{
				return FPartResult::Error(WoTWorldError(
					EWoTBootstrapError::SizeLimit,
					TEXT("the BSP animation data violates its v5 wire bounds"),
					Source.SourcePath));
			}
			TArray<uint8> Trailer;
			WoTWorldAppendU32(Trailer, 0);
			WoTWorldAppendU32(Trailer, static_cast<uint32>(Model.Surfaces.Num()));
			for (int32 SurfaceIndex = 0;
				SurfaceIndex < Model.Surfaces.Num(); ++SurfaceIndex)
			{
				const FWoTWorldAtlasEntry& Entry = Atlas.Surfaces[SurfaceIndex];
				WoTWorldAppendU32(Trailer, Entry.bPresent
					? static_cast<uint32>(Entry.X) : 0u);
				WoTWorldAppendU32(Trailer, Entry.bPresent
					? static_cast<uint32>(Entry.Y) : 0u);
				WoTWorldAppendU32(Trailer, Entry.bPresent
					? static_cast<uint32>(Entry.W) : 0u);
				WoTWorldAppendU32(Trailer, Entry.bPresent
					? static_cast<uint32>(Entry.H) : 0u);
			}
			WoTWorldAppendU32(Trailer, static_cast<uint32>(Manifest.Groups.Num()));
			for (const FWoTWorldAnimationGroup& Group : Manifest.Groups)
			{
				Trailer.Add(Group.Type);
				Trailer.Add(Group.Period);
				Trailer.Add(Group.Phase);
				Trailer.Add(0);
				WoTWorldAppendU32(Trailer, Group.LightCount);
			}
			WoTWorldAppendU32(Trailer, static_cast<uint32>(Manifest.Terms.Num()));
			for (int32 TermIndex = 0; TermIndex < Manifest.Terms.Num(); ++TermIndex)
			{
				const FWoTWorldAnimationTerm& Term = Manifest.Terms[TermIndex];
				const FWoTWorldResponseTerm& Response = Responses[TermIndex];
				Trailer.Add(Term.Kind);
				Trailer.Add(Term.Group == INDEX_NONE ? 255
					: static_cast<uint8>(Term.Group));
				Trailer.Add(Term.Effect);
				Trailer.Add(0);
				WoTWorldAppendU32(Trailer, Term.LightCount);
				WoTWorldAppendU32(Trailer, static_cast<uint32>(Response.Rects.Num()));
				for (const FWoTWorldResponseRect& Rect : Response.Rects)
				{
					const FWoTWorldAtlasEntry& Entry = Atlas.Surfaces[Rect.SurfaceIndex];
					if (!Entry.bPresent || static_cast<uint64>(Entry.W) * Entry.H * 3
						!= static_cast<uint64>(Rect.RGB.Num()))
					{
						return FPartResult::Error(WoTWorldError(
							EWoTBootstrapError::CorruptInstallation,
							TEXT("a light-map response rect disagrees with its placement"),
							Source.SourcePath));
					}
					WoTWorldAppendU32(Trailer, static_cast<uint32>(Rect.SurfaceIndex));
					Trailer.Append(Rect.RGB);
				}
			}
			if (static_cast<uint64>(Trailer.Num()) > MAX_uint32
				|| static_cast<uint64>(Payload.Num()) + Trailer.Num()
					> WoTWorldMaxSourceBytes)
			{
				return FPartResult::Error(WoTWorldError(
					EWoTBootstrapError::SizeLimit,
					TEXT("the BSP v5 payload exceeds its overall wire bound"),
					Source.SourcePath));
			}
			const uint32 TrailerByteCount = static_cast<uint32>(Trailer.Num());
			Trailer[0] = static_cast<uint8>(TrailerByteCount);
			Trailer[1] = static_cast<uint8>(TrailerByteCount >> 8);
			Trailer[2] = static_cast<uint8>(TrailerByteCount >> 16);
			Trailer[3] = static_cast<uint8>(TrailerByteCount >> 24);
			Payload.Append(Trailer);
		}
		if (bHasSkyRest)
		{
			if (static_cast<uint64>(Payload.Num()) + SkyRestTrailer.Num()
				> WoTWorldMaxSourceBytes)
			{
				return FPartResult::Error(WoTWorldError(
					EWoTBootstrapError::SizeLimit,
					TEXT("the BSP v6 payload exceeds its overall wire bound"),
					Source.SourcePath));
			}
			Payload.Append(SkyRestTrailer);
		}
		if (!bEntryLighting)
		{
			// Version 10 provenance trailer: exact rendered-triangle order, including
			// portal and procedural parts that do not carry the main light-map atlas.
			if (Model.Surfaces.IsEmpty() || Model.Surfaces.Num() > 65536
				|| TriangleCount > MAX_uint32
				|| static_cast<uint64>(Payload.Num()) + 12
					+ static_cast<uint64>(TriangleCount) * 4 > WoTWorldMaxSourceBytes)
			{
				return FPartResult::Error(WoTWorldError(
					EWoTBootstrapError::SizeLimit,
					TEXT("the BSP v10 surface provenance violates its wire bounds"),
					Source.SourcePath));
			}
			const uint32 TrailerByteCount = 12u
				+ static_cast<uint32>(TriangleCount) * 4u;
			WoTWorldAppendU32(Payload, TrailerByteCount);
			WoTWorldAppendU32(Payload, static_cast<uint32>(TriangleCount));
			WoTWorldAppendU32(Payload, static_cast<uint32>(Model.Surfaces.Num()));
			for (const FString& Slot : PartSlots)
				for (const FWoTWorldTriangle& Triangle : Groups[Slot])
				{
					if (Triangle.SurfaceIndex >= static_cast<uint32>(Model.Surfaces.Num()))
					{
						return FPartResult::Error(WoTWorldError(
							EWoTBootstrapError::CorruptInstallation,
							TEXT("a BSP triangle surface is outside the source model"),
							Source.SourcePath));
					}
					WoTWorldAppendU32(Payload, Triangle.SurfaceIndex);
				}
		}
		return FPartResult::Success(MoveTemp(Part));
	};

	TArray<FString> OrderedProceduralSlots;
	for (const FString& Slot : Slots)
		if (ProceduralSlots.Contains(Slot)) OrderedProceduralSlots.Add(Slot);
	OrderedProceduralSlots.Sort([](const FString& A, const FString& B)
	{
		const FString AFolded = WoTWorldFold(A);
		const FString BFolded = WoTWorldFold(B);
		const int32 FoldedOrder = FCString::Strcmp(*AFolded, *BFolded);
		return FoldedOrder == 0
			? FCString::Strcmp(*A, *B) < 0 : FoldedOrder < 0;
	});
	TArray<FString> BaseSlots;
	for (const FString& Slot : Slots)
		if (!ProceduralSlots.Contains(Slot)) BaseSlots.Add(Slot);
	const FString BaseIdentity =
		Source.MapIdentity + (bPortal ? TEXT("_portals") : TEXT(""));
	TArray<FWoTWorldBSPPartData> Parts;
	if (!BaseSlots.IsEmpty())
	{
		TWoTBootstrapResult<FWoTWorldBSPPartData> Base =
			EncodePart(BaseSlots, BaseIdentity, FString());
		if (Base.IsError()) return FResult::Error(MoveTemp(Base).GetError());
		Parts.Add(MoveTemp(Base).GetValue());
	}
	for (int32 Index = 0; Index < OrderedProceduralSlots.Num(); ++Index)
	{
		TArray<FString> OneSlot = {OrderedProceduralSlots[Index]};
		TWoTBootstrapResult<FWoTWorldBSPPartData> Procedural =
			EncodePart(OneSlot,
				FString::Printf(TEXT("%s_Fractal_%03d"), *BaseIdentity, Index),
				OrderedProceduralSlots[Index]);
		if (Procedural.IsError())
			return FResult::Error(MoveTemp(Procedural).GetError());
		Parts.Add(MoveTemp(Procedural).GetValue());
	}
	if (!bPortal)
	{
		TArray<FString> PartIdentities;
		PartIdentities.Reserve(Parts.Num());
		for (const FWoTWorldBSPPartData& Part : Parts)
			PartIdentities.Add(Part.PartIdentity);
		FWoTBootstrapStatus Carrier = WoTWorldValidateAtlasCarrier(
			!Atlas.BGRA.IsEmpty(), Source.MapIdentity, PartIdentities);
		if (Carrier.IsError())
			return FResult::Error(MoveTemp(Carrier).GetError());
	}
	return FResult::Success(MoveTemp(Parts));
}

FWoTCanonicalJsonValue WoTWorldBits3(const FVector3f& Value)
{
	FWoTCanonicalJsonValue Result = FWoTCanonicalJsonValue::MakeArray();
	Result.AddElement(FWoTCanonicalJsonValue::MakeInteger(WoTWorldFloatBits(Value.X)));
	Result.AddElement(FWoTCanonicalJsonValue::MakeInteger(WoTWorldFloatBits(Value.Y)));
	Result.AddElement(FWoTCanonicalJsonValue::MakeInteger(WoTWorldFloatBits(Value.Z)));
	return Result;
}

TWoTBootstrapResult<TArray<uint8>> WoTWorldBuildBSPObject(
	const FWoTRequiredFact& Fact, const FWoTWorldSource& Source,
	TConstArrayView<uint8> Payload, bool bPortal)
{
	using FResult = TWoTBootstrapResult<TArray<uint8>>;
	if (Payload.Num() < 52)
	{
		return FResult::Error(WoTWorldError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the shared BSP payload is truncated"), Source.SourcePath));
	}
	auto U32At = [Payload](int32 Offset)
	{
		return static_cast<uint32>(Payload[Offset])
			| (static_cast<uint32>(Payload[Offset + 1]) << 8)
			| (static_cast<uint32>(Payload[Offset + 2]) << 16)
			| (static_cast<uint32>(Payload[Offset + 3]) << 24);
	};
	auto FloatAt = [U32At](int32 Offset)
	{
		const uint32 Bits = U32At(Offset);
		float Value = 0.0f;
		FMemory::Memcpy(&Value, &Bits, sizeof(Value));
		return Value;
	};
	const uint32 VertexCount = U32At(12);
	const uint32 IndexCount = U32At(16);
	const uint32 SectionCount = U32At(20);
	const FVector3f BoundsMin(FloatAt(28), FloatAt(32), FloatAt(36));
	const FVector3f BoundsMax(FloatAt(40), FloatAt(44), FloatAt(48));
	TWoTBootstrapResult<FWoTCanonicalJsonValue> SourceJson =
		WoTWorldSourceJson(Fact, Source);
	if (SourceJson.IsError()) return FResult::Error(MoveTemp(SourceJson).GetError());
	FWoTCanonicalObjectBuilder Builder;
	Builder.Add(TEXT("bounds_max_bits"), WoTWorldBits3(BoundsMax));
	Builder.Add(TEXT("bounds_min_bits"), WoTWorldBits3(BoundsMin));
	Builder.Add(TEXT("encoding"), FWoTCanonicalJsonValue::MakeString(WoTWorldBSPEncoding));
	Builder.Add(TEXT("fact_id"), FWoTCanonicalJsonValue::MakeString(Fact.FactId));
	Builder.Add(TEXT("importer"), FWoTCanonicalJsonValue::MakeString(TEXT("bsp-v1")));
	Builder.Add(TEXT("importer_version"),
		FWoTCanonicalJsonValue::MakeInteger(WoTWorldBSPImporterVersion));
	Builder.Add(TEXT("index_count"), FWoTCanonicalJsonValue::MakeInteger(IndexCount));
	Builder.Add(TEXT("kind"), FWoTCanonicalJsonValue::MakeString(TEXT("bsp")));
	Builder.Add(TEXT("payload_byte_length"),
		FWoTCanonicalJsonValue::MakeInteger(Payload.Num()));
	Builder.Add(TEXT("role"), FWoTCanonicalJsonValue::MakeString(
		bPortal ? TEXT("portal") : TEXT("main")));
	Builder.Add(TEXT("section_count"), FWoTCanonicalJsonValue::MakeInteger(SectionCount));
	Builder.Add(TEXT("source"), MoveTemp(SourceJson).GetValue());
	Builder.Add(TEXT("tangent_algorithm"),
		FWoTCanonicalJsonValue::MakeString(WoTWorldBSPTangentAlgorithm));
	Builder.Add(TEXT("triangle_count"),
		FWoTCanonicalJsonValue::MakeInteger(IndexCount / 3));
	Builder.Add(TEXT("uv_channels"), FWoTCanonicalJsonValue::MakeInteger(2));
	Builder.Add(TEXT("vertex_count"), FWoTCanonicalJsonValue::MakeInteger(VertexCount));
	TWoTBootstrapResult<FWoTCanonicalJsonValue> Header =
		MoveTemp(Builder).Build(TEXT("BSP object header"));
	if (Header.IsError()) return FResult::Error(MoveTemp(Header).GetError());
	return WoTWorldFrameObject(Header.GetValue(), Payload);
}

TWoTBootstrapResult<TArray<uint8>> WoTWorldEncodePayload(
	FWoTWorldSource& Source, const FWoTWorldModel& Model)
{
	using FResult = TWoTBootstrapResult<TArray<uint8>>;
	TWoTBootstrapResult<TArray<FWoTWorldMoverModel>> MoverResult =
		WoTWorldBuildMoverModels(Source);
	if (MoverResult.IsError())
		return FResult::Error(MoveTemp(MoverResult).GetError());
	TArray<FWoTWorldMoverModel> MoverModels = MoveTemp(MoverResult).GetValue();
	FWoTWorldAtlas MoverAtlas;
	FWoTWorldAnimationManifest MoverAnimationManifest;
	TArray<FWoTWorldResponseTerm> MoverAnimationResponses;
	FWoTBootstrapStatus MoverLighting =
		WoTWorldComposeMoverAtlas(Source, Model, MoverModels, MoverAtlas,
			MoverAnimationManifest, MoverAnimationResponses);
	if (MoverLighting.IsError())
		return FResult::Error(MoveTemp(MoverLighting).GetError());
	TArray<uint8> Payload;
	Payload.Append(reinterpret_cast<const uint8*>(WoTWorldPayloadMagic), 8);
	// The authenticated entry/front-end pipeline remains byte-for-byte world-v4.
	// MacroTexture records are an MP-world addition and must not widen that
	// separately pinned payload or its materialization authority.
	const uint32 WorldImporterVersion = Source.MapIdentity == TEXT("WheelOfTime")
		? WoTWorldEntryImporterVersion : WoTWorldImporterVersion;
	WoTWorldAppendU16(Payload, WorldImporterVersion);
	WoTWorldAppendU16(Payload, 0);
	for (uint32 Count : {static_cast<uint32>(Source.Actors.Num()),
			Source.AttachmentCount, static_cast<uint32>(Model.Vectors.Num()),
			static_cast<uint32>(Model.Points.Num()), static_cast<uint32>(Model.Nodes.Num()),
			static_cast<uint32>(Model.Surfaces.Num()),
			static_cast<uint32>(Model.Vertices.Num()), static_cast<uint32>(Model.Zones.Num()),
			static_cast<uint32>(Source.SemanticObjects.Num()),
			static_cast<uint32>(Source.ReachSpecs.Num()),
			static_cast<uint32>(Model.Leaves.Num()),
			static_cast<uint32>(Model.Lights.Num()),
			static_cast<uint32>(MoverModels.Num())})
	{
		WoTWorldAppendU32(Payload, Count);
	}
	WoTWorldAppendU32(Payload, Model.bRootOutside ? 1u : 0u);
	for (const FVector3f& Vector : Model.Vectors)
		for (float Value : {Vector.X, Vector.Y, Vector.Z}) WoTWorldAppendFloat(Payload, Value);
	for (const FVector3f& Point : Model.Points)
		for (float Value : {Point.X, Point.Y, Point.Z}) WoTWorldAppendFloat(Payload, Value);
	for (const FWoTWorldNode& Node : Model.Nodes)
	{
		for (float Value : Node.Plane) WoTWorldAppendFloat(Payload, Value);
		WoTWorldAppendU64(Payload, Node.ZoneMask);
		Payload.Add(Node.Flags);
		for (int32 Value : {Node.VertexPool, Node.Surface, Node.Back, Node.Front,
			Node.PlaneNode, Node.CollisionBound, Node.RenderBound}) WoTWorldAppendI32(Payload, Value);
		Payload.Add(Node.Zones[0]); Payload.Add(Node.Zones[1]); Payload.Add(Node.VertexCount);
		WoTWorldAppendI32(Payload, Node.Leaves[0]);
		WoTWorldAppendI32(Payload, Node.Leaves[1]);
	}
	auto SemanticIdentity = [&Source](int32 Reference) -> FStringView
	{
		const int32* Binding = Source.SemanticObjectByReference.Find(Reference);
		return Binding != nullptr
			? FStringView(Source.SemanticObjects[*Binding].Identity) : FStringView();
	};
	for (const FWoTWorldSurface& Surface : Model.Surfaces)
	{
		for (int32 Value : {Surface.Base, Surface.Normal, Surface.TextureU,
			Surface.TextureV, Surface.LightMap, Surface.BrushPoly})
			WoTWorldAppendI32(Payload, Value);
		WoTWorldAppendU32(Payload, Surface.Flags);
		WoTWorldAppendU16(Payload, static_cast<uint16>(Surface.PanU));
		WoTWorldAppendU16(Payload, static_cast<uint16>(Surface.PanV));
		WoTWorldAppendFloat(Payload,
			WoTWorldTextureFriction(Source, Surface.Texture));
		if (!WoTWorldAppendOptionalString(
				Payload, SemanticIdentity(Surface.Texture))
			|| !WoTWorldAppendOptionalString(
				Payload, SemanticIdentity(Surface.Actor)))
		{
			return FResult::Error(WoTWorldError(EWoTBootstrapError::SizeLimit,
				TEXT("a BSP surface semantic identity exceeds its bound"),
				Source.SourcePath));
		}
	}
	for (const FWoTWorldVert& Vertex : Model.Vertices)
	{
		WoTWorldAppendI32(Payload, Vertex.Point);
		WoTWorldAppendI32(Payload, Vertex.Side);
	}
	for (const FWoTWorldZone& Zone : Model.Zones)
	{
		if (!WoTWorldAppendOptionalString(Payload, SemanticIdentity(Zone.Actor)))
		{
			return FResult::Error(WoTWorldError(EWoTBootstrapError::SizeLimit,
				TEXT("a BSP zone semantic identity exceeds its bound"),
				Source.SourcePath));
		}
		WoTWorldAppendU64(Payload, Zone.Connectivity);
		WoTWorldAppendU64(Payload, Zone.Visibility);
	}
	for (const FWoTWorldLeaf& Leaf : Model.Leaves)
	{
		WoTWorldAppendI32(Payload, Leaf.Zone);
		WoTWorldAppendI32(Payload, Leaf.Permeating);
		WoTWorldAppendI32(Payload, Leaf.Volumetric);
	}
	for (int32 Reference : Model.Lights)
	{
		if (Reference == 0)
		{
			WoTWorldAppendU32(Payload, MAX_uint32);
			continue;
		}
		const int32* Binding = Source.SemanticObjectByReference.Find(Reference);
		if (Binding == nullptr)
		{
			return FResult::Error(WoTWorldError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("a Model light lacks a semantic registry binding"),
				Source.SourcePath));
		}
		WoTWorldAppendU32(Payload, static_cast<uint32>(*Binding));
	}
	for (const FWoTWorldSemanticObject& Object : Source.SemanticObjects)
	{
		if (!WoTWorldAppendString(Payload, Object.Identity)
			|| !WoTWorldAppendString(Payload, Object.ClassIdentity))
		{
			return FResult::Error(WoTWorldError(EWoTBootstrapError::SizeLimit,
				TEXT("a semantic registry identity exceeds its bound"),
				Object.Identity));
		}
		Payload.Add(Object.bActor ? 1 : 0);
	}
	for (const FWoTWorldActor& Actor : Source.Actors)
	{
		if (!WoTWorldAppendString(Payload, Actor.Identity)
			|| !WoTWorldAppendString(Payload, Actor.ClassIdentity))
		{
			return FResult::Error(WoTWorldError(EWoTBootstrapError::SizeLimit,
				TEXT("a world actor identity exceeds its bound"), Actor.Identity));
		}
		if (!WoTAppendCanonicalTypedProperties(Payload, Actor.Properties, true))
		{
			return FResult::Error(WoTWorldError(EWoTBootstrapError::SizeLimit,
				TEXT("a typed world property exceeds its canonical bound"),
				Actor.Identity));
		}
		FStringView ParentIdentity;
		if (Actor.ParentActor.IsSet() && Actor.ParentActor.GetValue() != 0)
		{
			const int32* Binding =
				Source.SemanticObjectByReference.Find(Actor.ParentActor.GetValue());
			if (Binding == nullptr)
			{
				return FResult::Error(WoTWorldError(
					EWoTBootstrapError::CorruptInstallation,
					TEXT("an actor parent lacks a semantic registry binding"),
					Actor.Identity));
			}
			ParentIdentity = Source.SemanticObjects[*Binding].Identity;
		}
		if (!WoTWorldAppendOptionalString(Payload, ParentIdentity))
		{
			return FResult::Error(WoTWorldError(EWoTBootstrapError::SizeLimit,
				TEXT("an actor parent identity exceeds its bound"), Actor.Identity));
		}
	}
	for (const FWoTWorldReachSpec& ReachSpec : Source.ReachSpecs)
	{
		WoTWorldAppendI32(Payload, ReachSpec.Distance);
		WoTWorldAppendI32(Payload, ReachSpec.CollisionRadius);
		WoTWorldAppendI32(Payload, ReachSpec.CollisionHeight);
		WoTWorldAppendU32(Payload, ReachSpec.ReachFlags);
		Payload.Add(ReachSpec.bPruned ? 1 : 0);
		if (!WoTWorldAppendOptionalString(Payload, ReachSpec.StartIdentity)
			|| !WoTWorldAppendOptionalString(Payload, ReachSpec.EndIdentity))
		{
			return FResult::Error(WoTWorldError(EWoTBootstrapError::SizeLimit,
				TEXT("a ReachSpec endpoint identity exceeds its bound"),
				Source.SourcePath));
		}
	}
	for (const FWoTWorldMoverModel& Mover : MoverModels)
	{
		if (!WoTWorldAppendString(Payload, Mover.ActorIdentity)
			|| !WoTWorldAppendString(Payload, Mover.ModelIdentity))
		{
			return FResult::Error(WoTWorldError(EWoTBootstrapError::SizeLimit,
				TEXT("a mover identity exceeds its wire bound"), Mover.ActorIdentity));
		}
		WoTWorldAppendU32(Payload, static_cast<uint32>(Mover.Sections.Num()));
		WoTWorldAppendU32(Payload, static_cast<uint32>(Mover.Positions.Num()));
		WoTWorldAppendU32(Payload, static_cast<uint32>(Mover.Indices.Num()));
		WoTWorldAppendU32(Payload,
			static_cast<uint32>(Mover.CollisionPositions.Num()));
		WoTWorldAppendU32(Payload,
			static_cast<uint32>(Mover.CollisionIndices.Num()));
		Payload.Add(Mover.bLightmapped ? 1 : 0);
		for (const FWoTWorldMoverSection& Section : Mover.Sections)
		{
			WoTWorldAppendU32(Payload, Section.FirstIndex);
			WoTWorldAppendU32(Payload, Section.IndexCount);
			WoTWorldAppendU32(Payload, Section.MinVertex);
			WoTWorldAppendU32(Payload, Section.MaxVertex);
			Payload.Add(1);
			Payload.Add(
				WorldImporterVersion >= 6 && Section.bAnimatedLightmap ? 1 : 0);
			Payload.AddZeroed(2);
			WoTWorldAppendU32(Payload, Section.SurfaceFlags);
			for (float Value : {Section.PlaneOrigin.X, Section.PlaneOrigin.Y,
				Section.PlaneOrigin.Z, Section.PlaneNormal.X, Section.PlaneNormal.Y,
				Section.PlaneNormal.Z}) WoTWorldAppendFloat(Payload, Value);
			if (!WoTWorldAppendString(Payload, Section.MaterialSlot)
				|| !WoTWorldAppendOptionalString(Payload, Section.CanonicalPackage)
				|| !WoTWorldAppendString(Payload, Section.CanonicalName)
				|| !WoTWorldAppendString(Payload, Section.SourceTextureIdentity))
			{
				return FResult::Error(WoTWorldError(EWoTBootstrapError::SizeLimit,
					TEXT("a mover section identity exceeds its wire bound"),
					Mover.ActorIdentity));
			}
			if (WorldImporterVersion >= 5)
			{
				if (!WoTWorldAppendOptionalString(
						Payload, Section.MacroCanonicalPackage)
					|| !WoTWorldAppendOptionalString(
						Payload, Section.MacroCanonicalName)
					|| !WoTWorldAppendOptionalString(
						Payload, Section.MacroSourceTextureIdentity))
				{
					return FResult::Error(WoTWorldError(EWoTBootstrapError::SizeLimit,
						TEXT("a mover macro identity exceeds its wire bound"),
						Mover.ActorIdentity));
				}
				WoTWorldAppendFloat(Payload, Section.MacroRatio);
			}
		}
		for (const FVector3f& Position : Mover.Positions)
			for (float Value : {Position.X, Position.Y, Position.Z})
				WoTWorldAppendFloat(Payload, Value);
		for (const FVector3f& Normal : Mover.Normals)
			for (float Value : {Normal.X, Normal.Y, Normal.Z})
				WoTWorldAppendFloat(Payload, Value);
		for (const FVector2f& UV : Mover.UV0)
			for (float Value : {UV.X, UV.Y}) WoTWorldAppendFloat(Payload, Value);
		if (Mover.UV1.Num() != Mover.Positions.Num())
		{
			return FResult::Error(WoTWorldError(EWoTBootstrapError::CorruptInstallation,
				TEXT("a mover UV1 stream disagrees with its positions"),
				Mover.ActorIdentity));
		}
		for (const FVector2f& UV : Mover.UV1)
			for (float Value : {UV.X, UV.Y}) WoTWorldAppendFloat(Payload, Value);
		for (uint32 Index : Mover.Indices) WoTWorldAppendU32(Payload, Index);
		for (const FVector3f& Position : Mover.CollisionPositions)
			for (float Value : {Position.X, Position.Y, Position.Z})
				WoTWorldAppendFloat(Payload, Value);
		for (uint32 Index : Mover.CollisionIndices)
			WoTWorldAppendU32(Payload, Index);
	}
	const uint64 MoverAtlasBytes = static_cast<uint64>(MoverAtlas.Width)
		* MoverAtlas.Height * 4;
	if (MoverAtlas.Width < 1 || MoverAtlas.Width > 4096
		|| MoverAtlas.Height < 1 || MoverAtlas.Height > 4096
		|| MoverAtlasBytes != static_cast<uint64>(MoverAtlas.BGRA.Num())
		|| MoverAtlasBytes > MAX_uint32)
	{
		return FResult::Error(WoTWorldError(EWoTBootstrapError::SizeLimit,
			TEXT("the mover atlas violates its wire bounds"), Source.SourcePath));
	}
	WoTWorldAppendU32(Payload, static_cast<uint32>(MoverAtlas.Width));
	WoTWorldAppendU32(Payload, static_cast<uint32>(MoverAtlas.Height));
	WoTWorldAppendU32(Payload, static_cast<uint32>(MoverAtlasBytes));
	Payload.Append(MoverAtlas.BGRA);
	if (WorldImporterVersion >= 6)
	{
		if (MoverAnimationManifest.Groups.Num() > WoTWorldMaxWaveformGroups
			|| MoverAnimationManifest.Terms.Num() > WoTWorldMaxResponseTerms
			|| MoverAnimationResponses.Num()
				!= MoverAnimationManifest.Terms.Num())
		{
			return FResult::Error(WoTWorldError(EWoTBootstrapError::SizeLimit,
				TEXT("the mover animation data violates its v6 wire bounds"),
				Source.SourcePath));
		}
		WoTWorldAppendU32(Payload,
			static_cast<uint32>(MoverAnimationManifest.Groups.Num()));
		for (const FWoTWorldAnimationGroup& Group : MoverAnimationManifest.Groups)
		{
			Payload.Add(Group.Type);
			Payload.Add(Group.Period);
			Payload.Add(Group.Phase);
			Payload.Add(0);
			WoTWorldAppendU32(Payload, Group.LightCount);
		}
		WoTWorldAppendU32(Payload,
			static_cast<uint32>(MoverAnimationManifest.Terms.Num()));
		for (int32 TermIndex = 0;
			TermIndex < MoverAnimationManifest.Terms.Num(); ++TermIndex)
		{
			const FWoTWorldAnimationTerm& Term =
				MoverAnimationManifest.Terms[TermIndex];
			const FWoTWorldResponseTerm& Response =
				MoverAnimationResponses[TermIndex];
			Payload.Add(Term.Kind);
			Payload.Add(Term.Group == INDEX_NONE
				? 255 : static_cast<uint8>(Term.Group));
			Payload.Add(Term.Effect);
			Payload.Add(0);
			WoTWorldAppendU32(Payload, Term.LightCount);
			WoTWorldAppendU32(Payload, static_cast<uint32>(Response.Rects.Num()));
			for (const FWoTWorldResponseRect& Rect : Response.Rects)
			{
				if (!MoverAtlas.Surfaces.IsValidIndex(Rect.SurfaceIndex))
				{
					return FResult::Error(WoTWorldError(
						EWoTBootstrapError::CorruptInstallation,
						TEXT("a mover response references no atlas placement"),
						Source.SourcePath));
				}
				const FWoTWorldAtlasEntry& Entry =
					MoverAtlas.Surfaces[Rect.SurfaceIndex];
				if (!Entry.bPresent || static_cast<uint64>(Entry.W) * Entry.H * 3
					!= static_cast<uint64>(Rect.RGB.Num()))
				{
					return FResult::Error(WoTWorldError(
						EWoTBootstrapError::CorruptInstallation,
						TEXT("a mover response rect disagrees with its placement"),
						Source.SourcePath));
				}
				WoTWorldAppendU32(Payload, static_cast<uint32>(Entry.X));
				WoTWorldAppendU32(Payload, static_cast<uint32>(Entry.Y));
				WoTWorldAppendU32(Payload, static_cast<uint32>(Entry.W));
				WoTWorldAppendU32(Payload, static_cast<uint32>(Entry.H));
				WoTWorldAppendU32(Payload, static_cast<uint32>(Rect.RGB.Num()));
				Payload.Append(Rect.RGB);
			}
		}
	}
	if (static_cast<uint64>(Payload.Num()) > WoTWorldMaxSourceBytes)
	{
		return FResult::Error(WoTWorldError(EWoTBootstrapError::SizeLimit,
			TEXT("the decoded world payload exceeds its byte bound"), Source.SourcePath));
	}
	return FResult::Success(MoveTemp(Payload));
}

TWoTBootstrapResult<TArray<uint8>> WoTWorldBuildObject(
	const FWoTRequiredFact& Fact, FWoTWorldSource& Source,
	const FWoTWorldModel& Model)
{
	using FResult = TWoTBootstrapResult<TArray<uint8>>;
	TWoTBootstrapResult<TArray<uint8>> Payload = WoTWorldEncodePayload(Source, Model);
	if (Payload.IsError()) return FResult::Error(MoveTemp(Payload).GetError());
	TWoTBootstrapResult<FWoTCanonicalJsonValue> SourceJson =
		WoTWorldSourceJson(Fact, Source);
	if (SourceJson.IsError()) return FResult::Error(MoveTemp(SourceJson).GetError());
	uint32 PortalCount = 0;
	for (const FWoTWorldSurface& Surface : Model.Surfaces)
		if ((Surface.Flags & WoTWorldPFPortal) != 0) ++PortalCount;
	FWoTCanonicalObjectBuilder Builder;
	const uint32 WorldImporterVersion = Source.MapIdentity == TEXT("WheelOfTime")
		? WoTWorldEntryImporterVersion : WoTWorldImporterVersion;
	Builder.Add(TEXT("actor_count"), FWoTCanonicalJsonValue::MakeInteger(Source.Actors.Num()));
	Builder.Add(TEXT("attachment_count"),
		FWoTCanonicalJsonValue::MakeInteger(Source.AttachmentCount));
	Builder.Add(TEXT("bsp_node_count"), FWoTCanonicalJsonValue::MakeInteger(Model.Nodes.Num()));
	Builder.Add(TEXT("bsp_surface_count"),
		FWoTCanonicalJsonValue::MakeInteger(Model.Surfaces.Num()));
	Builder.Add(TEXT("encoding"),
		FWoTCanonicalJsonValue::MakeString(WorldImporterVersion >= 6
			? TEXT("ue1-world-records-le-v6")
			: WorldImporterVersion >= 5
				? TEXT("ue1-world-records-le-v5")
				: TEXT("ue1-world-records-le-v4")));
	Builder.Add(TEXT("fact_id"), FWoTCanonicalJsonValue::MakeString(Fact.FactId));
	Builder.Add(TEXT("importer"), FWoTCanonicalJsonValue::MakeString(TEXT("world-v1")));
	Builder.Add(TEXT("importer_version"),
		FWoTCanonicalJsonValue::MakeInteger(WorldImporterVersion));
	Builder.Add(TEXT("kind"), FWoTCanonicalJsonValue::MakeString(TEXT("world")));
	Builder.Add(TEXT("light_count"), FWoTCanonicalJsonValue::MakeInteger(Source.LightCount));
	Builder.Add(TEXT("map_identity"), FWoTCanonicalJsonValue::MakeString(Source.MapIdentity));
	Builder.Add(TEXT("mover_count"), FWoTCanonicalJsonValue::MakeInteger(Source.MoverCount));
	Builder.Add(TEXT("navigation_seed_count"),
		FWoTCanonicalJsonValue::MakeInteger(Source.NavigationSeedCount));
	Builder.Add(TEXT("payload_byte_length"),
		FWoTCanonicalJsonValue::MakeInteger(Payload.GetValue().Num()));
	Builder.Add(TEXT("portal_surface_count"),
		FWoTCanonicalJsonValue::MakeInteger(PortalCount));
	Builder.Add(TEXT("reach_spec_count"),
		FWoTCanonicalJsonValue::MakeInteger(Source.ReachSpecs.Num()));
	Builder.Add(TEXT("semantic_object_count"),
		FWoTCanonicalJsonValue::MakeInteger(Source.SemanticObjects.Num()));
	Builder.Add(TEXT("sound_count"), FWoTCanonicalJsonValue::MakeInteger(Source.SoundCount));
	Builder.Add(TEXT("source"), MoveTemp(SourceJson).GetValue());
	Builder.Add(TEXT("zone_count"), FWoTCanonicalJsonValue::MakeInteger(Model.Zones.Num()));
	TWoTBootstrapResult<FWoTCanonicalJsonValue> Header =
		MoveTemp(Builder).Build(TEXT("world object header"));
	if (Header.IsError()) return FResult::Error(MoveTemp(Header).GetError());
	return WoTWorldFrameObject(Header.GetValue(), Payload.GetValue());
}

int32 WoTWorldSlot(FStringView Identity)
{
	for (int32 Index = 0;
		Index < UE_ARRAY_COUNT(WoT::Retail::Generated::RequiredWorlds); ++Index)
	{
		if (Identity == WoT::Retail::Generated::RequiredWorlds[Index].Identity)
			return Index;
	}
	return INDEX_NONE;
}

TWoTBootstrapResult<TArray<FWoTWorldContractMap>> WoTWorldValidateContract(
	const FWoTRetailContract& Contract)
{
	using FResult = TWoTBootstrapResult<TArray<FWoTWorldContractMap>>;
	TArray<FWoTWorldContractMap> Maps;
	Maps.SetNum(UE_ARRAY_COUNT(WoT::Retail::Generated::RequiredWorlds));
	TSet<FString> LogicalIds;
	for (const FWoTRequiredFact& Fact : Contract.Facts)
	{
		if (Fact.Domain != EWoTFactDomain::BSP && Fact.Domain != EWoTFactDomain::World)
			continue;
		// The authenticated entry world is imported by its own one-map path and
		// never occupies or widens this exact nineteen-slot multiplayer census.
		if (Fact.SourcePath == TEXT("Maps/WheelOfTime.wot"))
			continue;
		const FString Identity = FPaths::GetBaseFilename(Fact.SourcePath);
		const int32 Slot = WoTWorldSlot(Identity);
		const FString ExpectedPath =
			Slot == INDEX_NONE ? FString()
			: FString(WoT::Retail::Generated::RequiredWorlds[Slot].SourcePath);
		const bool bBSP = Fact.Domain == EWoTFactDomain::BSP;
		if (Slot == INDEX_NONE || Fact.SourcePath != ExpectedPath
			|| Fact.FactId.IsEmpty() || Fact.SourceExport.IsEmpty()
			|| Fact.Importer != (bBSP ? TEXT("bsp-v1") : TEXT("world-v1"))
			|| (bBSP && Fact.Outputs.IsEmpty())
			|| (!bBSP && Fact.Outputs.Num() != 1))
		{
			return FResult::Error(WoTWorldError(EWoTBootstrapError::UnknownField,
				TEXT("a world fact does not match the closed importer/source contract"),
				Fact.FactId));
		}
		FWoTWorldContractMap& Map = Maps[Slot];
		if (Map.Identity.IsEmpty())
		{
			Map.Identity = Identity;
			Map.Slot = WoT::Retail::Generated::RequiredWorlds[Slot].NeutralSlot;
		}
		else if (Map.Identity != Identity)
		{
			return FResult::Error(WoTWorldError(EWoTBootstrapError::DuplicateField,
				TEXT("two world identities occupy the same neutral slot"), Identity));
		}
		const FWoTRequiredFact** Destination = bBSP ? &Map.BSPFact : &Map.WorldFact;
		if (*Destination != nullptr)
		{
			return FResult::Error(WoTWorldError(EWoTBootstrapError::DuplicateField,
				TEXT("a required map has duplicate BSP/world facts"), Identity));
		}
		*Destination = &Fact;
		for (int32 OutputIndex = 0; OutputIndex < Fact.Outputs.Num(); ++OutputIndex)
		{
			const FWoTRequiredFactOutput& Output = Fact.Outputs[OutputIndex];
			const FString ExpectedSchema = bBSP ? TEXT("wot-bsp-v3") : TEXT("wot-world-v3");
			if (Output.LogicalId.IsEmpty() || Output.Schema != ExpectedSchema
				|| Output.MaxDecodedSize == 0 || LogicalIds.Contains(Output.LogicalId))
			{
				return FResult::Error(WoTWorldError(
					LogicalIds.Contains(Output.LogicalId)
						? EWoTBootstrapError::DuplicateField : EWoTBootstrapError::UnknownField,
					TEXT("a world output is duplicate or outside its closed schema"),
					Output.LogicalId));
			}
			LogicalIds.Add(Output.LogicalId);
		}
	}
	for (int32 Slot = 0; Slot < Maps.Num(); ++Slot)
	{
		if (Maps[Slot].Identity.IsEmpty() || Maps[Slot].BSPFact == nullptr
			|| Maps[Slot].WorldFact == nullptr)
		{
			return FResult::Error(WoTWorldError(EWoTBootstrapError::UnknownField,
				TEXT("the contract does not contain the complete nineteen-world set"),
				FString::Printf(TEXT("world-slot[%d]"), Slot + 1)));
		}
	}
	return FResult::Success(MoveTemp(Maps));
}

TWoTBootstrapResult<TArray<FWoTWorldContractMap>>
WoTWorldValidateFrontEndContract(const FWoTRetailContract& Contract)
{
	using FResult = TWoTBootstrapResult<TArray<FWoTWorldContractMap>>;
	FWoTWorldContractMap Entry;
	Entry.Identity = TEXT("WheelOfTime");
	Entry.Slot = INDEX_NONE;
	for (const FWoTRequiredFact& Fact : Contract.Facts)
	{
		if (Fact.SourcePath != TEXT("Maps/WheelOfTime.wot")
			|| (Fact.Domain != EWoTFactDomain::BSP
				&& Fact.Domain != EWoTFactDomain::World))
		{
			continue;
		}
		const bool bBSP = Fact.Domain == EWoTFactDomain::BSP;
		const TCHAR* ExpectedFactId = bBSP
			? TEXT("retail.bsp.WheelOfTime") : TEXT("retail.world.WheelOfTime");
		const TCHAR* ExpectedExport = bBSP
			? TEXT("WheelOfTime.Model") : TEXT("WheelOfTime.Level");
		const TCHAR* ExpectedImporter = bBSP ? TEXT("bsp-v1") : TEXT("world-v1");
		const TCHAR* ExpectedSchema = bBSP ? TEXT("wot-bsp-v3") : TEXT("wot-world-v3");
		if (Fact.FactId != ExpectedFactId || Fact.SourceExport != ExpectedExport
			|| Fact.Importer != ExpectedImporter || Fact.Outputs.Num() != 1
			|| Fact.Outputs[0].Schema != ExpectedSchema
			|| Fact.Outputs[0].MaxDecodedSize == 0)
		{
			return FResult::Error(WoTWorldError(EWoTBootstrapError::UnknownField,
				TEXT("the entry world fact is outside its closed one-map contract"),
				Fact.FactId));
		}
		const FWoTRequiredFact** Destination = bBSP
			? &Entry.BSPFact : &Entry.WorldFact;
		if (*Destination != nullptr)
		{
			return FResult::Error(WoTWorldError(EWoTBootstrapError::DuplicateField,
				TEXT("the entry map has duplicate BSP/world facts"), Fact.FactId));
		}
		*Destination = &Fact;
	}
	if (Entry.BSPFact == nullptr || Entry.WorldFact == nullptr)
	{
		return FResult::Error(WoTWorldError(EWoTBootstrapError::UnknownField,
			TEXT("the contract does not contain the complete entry world pair"),
			TEXT("Maps/WheelOfTime.wot")));
	}
	TArray<FWoTWorldContractMap> Result;
	Result.Add(Entry);
	return FResult::Success(MoveTemp(Result));
}

void WoTWorldHashUpdate(FWoTSha256Hasher& Hasher, TConstArrayView<uint8> Bytes)
{
	TArray<uint8> Length;
	WoTWorldAppendU64(Length, static_cast<uint64>(Bytes.Num()));
	Hasher.Update(Length);
	Hasher.Update(Bytes);
}

void WoTWorldHashUpdateString(FWoTSha256Hasher& Hasher, FStringView Value)
{
	FTCHARToUTF8 Encoded(Value.GetData(), Value.Len());
	WoTWorldHashUpdate(Hasher, TConstArrayView<uint8>(
		reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length()));
}

FWoTSha256 WoTWorldMapRoot(const FWoTWorldSource& Source)
{
	FWoTSha256Hasher Hasher;
	Hasher.Update(TConstArrayView<uint8>(
		reinterpret_cast<const uint8*>(WoTWorldMapSourceDomain),
		static_cast<int32>(sizeof(WoTWorldMapSourceDomain))));
	WoTWorldHashUpdateString(Hasher, Source.MapIdentity);
	WoTWorldHashUpdate(Hasher, TConstArrayView<uint8>(
		Source.Sha256.GetData(), Source.Sha256.Num()));
	return Hasher.Finish();
}

TWoTBootstrapResult<FWoTSha256> WoTWorldDescriptorRoot(
	const FWoTWorldDescriptor& World)
{
	using FResult = TWoTBootstrapResult<FWoTSha256>;
	FWoTCanonicalJsonValue Parts = FWoTCanonicalJsonValue::MakeArray();
	for (const FWoTBSPPartDescriptor& Part : World.BSPParts)
	{
		FWoTCanonicalObjectBuilder PartBuilder;
		PartBuilder.Add(TEXT("fact_id"),
			FWoTCanonicalJsonValue::MakeString(Part.FactId));
		PartBuilder.Add(TEXT("index_count"),
			FWoTCanonicalJsonValue::MakeInteger(Part.IndexCount));
		PartBuilder.Add(TEXT("logical_id"),
			FWoTCanonicalJsonValue::MakeString(Part.LogicalId));
		PartBuilder.Add(TEXT("part_identity"),
			FWoTCanonicalJsonValue::MakeString(Part.PartIdentity));
		PartBuilder.Add(TEXT("payload_sha256"),
			FWoTCanonicalJsonValue::MakeString(Part.PayloadSha256.ToHex()));
		PartBuilder.Add(TEXT("procedural_material_slot"),
			FWoTCanonicalJsonValue::MakeString(Part.ProceduralMaterialSlot));
		PartBuilder.Add(TEXT("role"), FWoTCanonicalJsonValue::MakeString(
			Part.Role == EWoTBSPPartRole::Portal ? TEXT("portal") : TEXT("main")));
		PartBuilder.Add(TEXT("schema"),
			FWoTCanonicalJsonValue::MakeString(Part.Schema));
		PartBuilder.Add(TEXT("section_count"),
			FWoTCanonicalJsonValue::MakeInteger(Part.SectionCount));
		PartBuilder.Add(TEXT("sha256"),
			FWoTCanonicalJsonValue::MakeString(Part.Sha256.ToHex()));
		PartBuilder.Add(TEXT("vertex_count"),
			FWoTCanonicalJsonValue::MakeInteger(Part.VertexCount));
		TWoTBootstrapResult<FWoTCanonicalJsonValue> PartObject =
			MoveTemp(PartBuilder).Build(TEXT("world BSP part descriptor"));
		if (PartObject.IsError())
			return FResult::Error(MoveTemp(PartObject).GetError());
		Parts.AddElement(MoveTemp(PartObject).GetValue());
	}
	FWoTCanonicalObjectBuilder Builder;
	Builder.Add(TEXT("actor_count"),
		FWoTCanonicalJsonValue::MakeInteger(World.ActorCount));
	Builder.Add(TEXT("attachment_count"),
		FWoTCanonicalJsonValue::MakeInteger(World.AttachmentCount));
	Builder.Add(TEXT("bsp_fact_id"),
		FWoTCanonicalJsonValue::MakeString(World.BSPFactId));
	Builder.Add(TEXT("bsp_importer"),
		FWoTCanonicalJsonValue::MakeString(World.BSPImporter));
	Builder.Add(TEXT("bsp_node_count"),
		FWoTCanonicalJsonValue::MakeInteger(World.BSPNodeCount));
	Builder.Add(TEXT("bsp_parts"), MoveTemp(Parts));
	Builder.Add(TEXT("bsp_surface_count"),
		FWoTCanonicalJsonValue::MakeInteger(World.BSPSurfaceCount));
	Builder.Add(TEXT("importer_version"),
		FWoTCanonicalJsonValue::MakeInteger(
			World.MapIdentity == TEXT("WheelOfTime")
				? WoTWorldEntryImporterVersion : WoTWorldImporterVersion));
	Builder.Add(TEXT("light_count"),
		FWoTCanonicalJsonValue::MakeInteger(World.LightCount));
	Builder.Add(TEXT("map_identity"),
		FWoTCanonicalJsonValue::MakeString(World.MapIdentity));
	Builder.Add(TEXT("mover_count"),
		FWoTCanonicalJsonValue::MakeInteger(World.MoverCount));
	Builder.Add(TEXT("navigation_seed_count"),
		FWoTCanonicalJsonValue::MakeInteger(World.NavigationSeedCount));
	Builder.Add(TEXT("neutral_map_slot"),
		FWoTCanonicalJsonValue::MakeInteger(World.NeutralMapSlot));
	Builder.Add(TEXT("package_sha256"),
		FWoTCanonicalJsonValue::MakeString(World.PackageSha256.ToHex()));
	Builder.Add(TEXT("portal_surface_count"),
		FWoTCanonicalJsonValue::MakeInteger(World.PortalSurfaceCount));
	Builder.Add(TEXT("reach_spec_count"),
		FWoTCanonicalJsonValue::MakeInteger(World.ReachSpecCount));
	Builder.Add(TEXT("semantic_object_count"),
		FWoTCanonicalJsonValue::MakeInteger(World.SemanticObjectCount));
	Builder.Add(TEXT("sound_count"),
		FWoTCanonicalJsonValue::MakeInteger(World.SoundCount));
	Builder.Add(TEXT("source_root"),
		FWoTCanonicalJsonValue::MakeString(World.SourceRoot.ToHex()));
	Builder.Add(TEXT("world_fact_id"),
		FWoTCanonicalJsonValue::MakeString(World.WorldFactId));
	Builder.Add(TEXT("world_importer"),
		FWoTCanonicalJsonValue::MakeString(World.WorldImporter));
	Builder.Add(TEXT("world_logical_id"),
		FWoTCanonicalJsonValue::MakeString(World.WorldLogicalId));
	Builder.Add(TEXT("world_schema"),
		FWoTCanonicalJsonValue::MakeString(World.WorldSchema));
	Builder.Add(TEXT("world_sha256"),
		FWoTCanonicalJsonValue::MakeString(World.WorldSha256.ToHex()));
	Builder.Add(TEXT("zone_count"),
		FWoTCanonicalJsonValue::MakeInteger(World.ZoneCount));
	TWoTBootstrapResult<FWoTCanonicalJsonValue> Descriptor =
		MoveTemp(Builder).Build(TEXT("world descriptor root input"));
	if (Descriptor.IsError()) return FResult::Error(MoveTemp(Descriptor).GetError());
	TWoTBootstrapResult<TArray<uint8>> Encoded =
		WriteCanonicalJson(Descriptor.GetValue());
	if (Encoded.IsError()) return FResult::Error(MoveTemp(Encoded).GetError());
	FWoTSha256Hasher Hasher;
	Hasher.Update(TConstArrayView<uint8>(
		reinterpret_cast<const uint8*>(WoTWorldMapDomain),
		static_cast<int32>(sizeof(WoTWorldMapDomain))));
	WoTWorldHashUpdate(Hasher, Encoded.GetValue());
	return FResult::Success(Hasher.Finish());
}

FWoTSha256 WoTWorldSetRoot(TConstArrayView<FWoTWorldDescriptor> Worlds)
{
	FWoTSha256Hasher Hasher;
	Hasher.Update(TConstArrayView<uint8>(
		reinterpret_cast<const uint8*>(WoTWorldSetDomain),
		static_cast<int32>(sizeof(WoTWorldSetDomain))));
	TArray<uint8> Count;
	WoTWorldAppendU32(Count, static_cast<uint32>(Worlds.Num()));
	Hasher.Update(Count);
	for (const FWoTWorldDescriptor& World : Worlds)
	{
		WoTWorldHashUpdateString(Hasher, World.MapIdentity);
		WoTWorldHashUpdate(Hasher,
			TConstArrayView<uint8>(World.MapRoot.GetData(), World.MapRoot.Num()));
	}
	return Hasher.Finish();
}

class FWoTWorldCollectSink final : public IWoTByteSink
{
public:
	explicit FWoTWorldCollectSink(uint64 InLimit) : Limit(InLimit) {}
	FWoTBootstrapStatus Consume(TConstArrayView<uint8> Chunk) override
	{
		const uint64 Count = static_cast<uint64>(Chunk.Num());
		if (Count > Limit - Written
			|| Count > static_cast<uint64>(MAX_int32 - Bytes.Num()))
		{
			return FWoTBootstrapStatus::Error(WoTWorldError(
				EWoTBootstrapError::SizeLimit,
				TEXT("the world source exceeded its quarantine limit")));
		}
		Bytes.Append(Chunk.GetData(), Chunk.Num());
		Written += Count;
		return FWoTBootstrapStatus::Success();
	}
	TArray<uint8> Bytes;
private:
	uint64 Limit = 0;
	uint64 Written = 0;
};

using FWoTWorldSourceLoader =
	TFunction<TWoTBootstrapResult<TArray<uint8>>(FStringView)>;

FWoTBootstrapError WoTWorldCancelled()
{
	return WoTWorldError(EWoTBootstrapError::ServiceUnavailable,
		TEXT("the required world-set import was cancelled before completion"),
		TEXT("world-set"));
}

TWoTBootstrapResult<FWoTWorldSetResult> WoTWorldImportCore(
	const TArray<FWoTWorldContractMap>& ValidatedMaps, IWoTObjectWriter& Writer,
	IWoTProgressSink& Progress,
	FWoTWorldSourceLoader&& Loader,
	bool bSplitProceduralBSP, bool bIncludePortalBSP)
{
	using FResult = TWoTBootstrapResult<FWoTWorldSetResult>;
	const uint64 PendingWorldCount = static_cast<uint64>(ValidatedMaps.Num());
	FWoTBootstrapStatus Initial = Progress.ReportProgress(
		TEXT("world-set"), 0, PendingWorldCount);
	if (Initial.IsError()) return FResult::Error(MoveTemp(Initial).GetError());
	if (Progress.IsCancellationRequested()) return FResult::Error(WoTWorldCancelled());

	// Pass one: quarantine every source, close every identity/reference graph,
	// and retain only bounded source spans. No object writer is called here.
	TArray<FWoTWorldSource> Sources;
	Sources.Reserve(ValidatedMaps.Num());
	uint64 TotalSourceBytes = 0;
	for (const FWoTWorldContractMap& Map : ValidatedMaps)
	{
		if (Progress.IsCancellationRequested()) return FResult::Error(WoTWorldCancelled());
		TWoTBootstrapResult<TArray<uint8>> Loaded = Loader(Map.BSPFact->SourcePath);
		if (Loaded.IsError()) return FResult::Error(MoveTemp(Loaded).GetError());
		TotalSourceBytes += static_cast<uint64>(Loaded.GetValue().Num());
		if (TotalSourceBytes > WoTWorldMaxTotalSourceBytes)
		{
			return FResult::Error(WoTWorldError(EWoTBootstrapError::SizeLimit,
				TEXT("the nineteen-world quarantine exceeds its aggregate bound")));
		}
		TWoTBootstrapResult<FWoTWorldSource> Parsed =
			WoTWorldParsePackage(MoveTemp(Loaded).GetValue(), Map.BSPFact->SourcePath);
		if (Parsed.IsError()) return FResult::Error(MoveTemp(Parsed).GetError());
		if (Parsed.GetValue().MapIdentity != Map.Identity
			|| Map.WorldFact->SourcePath != Map.BSPFact->SourcePath)
		{
			return FResult::Error(WoTWorldError(EWoTBootstrapError::UnknownField,
				TEXT("BSP/world facts disagree with their package identity"), Map.Identity));
		}
		TWoTBootstrapResult<FWoTWorldSource> Discovered =
			WoTWorldDiscover(MoveTemp(Parsed).GetValue());
		if (Discovered.IsError()) return FResult::Error(MoveTemp(Discovered).GetError());
		Sources.Add(MoveTemp(Discovered).GetValue());
		FWoTBootstrapStatus DiscoveredProgress = Progress.ReportProgress(
			TEXT("world-set-discovery"), static_cast<uint64>(Sources.Num()),
			PendingWorldCount);
		if (DiscoveredProgress.IsError())
			return FResult::Error(MoveTemp(DiscoveredProgress).GetError());
		if (Progress.IsCancellationRequested()) return FResult::Error(WoTWorldCancelled());
	}

	// Pass two closes every geometry/resource binding and derives the complete
	// ordered split before the first writer call.  Thus a missing/wrong typed
	// binding or a contract/part mismatch cannot publish even a temporary object.
	TArray<FWoTWorldModel> Models;
	TArray<TArray<FWoTWorldBSPPartData>> BSPPartsByMap;
	TArray<FWoTWorldLightmapComposeStats> LightmapStatsByMap;
	Models.Reserve(Sources.Num());
	BSPPartsByMap.Reserve(Sources.Num());
	LightmapStatsByMap.Reserve(Sources.Num());
	int32 BSPObjectCount = 0;
	for (int32 MapIndex = 0; MapIndex < Sources.Num(); ++MapIndex)
	{
		FWoTWorldSource& Source = Sources[MapIndex];
		const FWoTWorldContractMap& Map = ValidatedMaps[MapIndex];
		TWoTBootstrapResult<FWoTWorldModel> Model = WoTWorldDecodeModel(Source);
		if (Model.IsError()) return FResult::Error(MoveTemp(Model).GetError());
		const FWoTWorldSemanticValidationMap SemanticView =
			WoTWorldSemanticValidationView(Source, Model.GetValue());
		FWoTBootstrapStatus SemanticStatus =
			WoTWorldValidateSemanticMaps(MakeArrayView(&SemanticView, 1), false);
		if (SemanticStatus.IsError())
			return FResult::Error(MoveTemp(SemanticStatus).GetError());
		TArray<FWoTWorldBSPPartData> Parts;
		FWoTWorldLightmapComposeStats LightmapStats;
		{
			FWoTWorldComposedAtlas Lighting;
			if (!WoTWorldBuildAtlas(Model.GetValue(), Lighting.Atlas))
			{
				return FResult::Error(WoTWorldError(EWoTBootstrapError::SizeLimit,
					TEXT("the canonical BSP light-map atlas does not fit its fixed sizes"),
					Source.SourcePath));
			}
			if (Source.MapIdentity == TEXT("WheelOfTime"))
			{
				for (int32 SurfaceIndex = 0;
					SurfaceIndex < Model.GetValue().Surfaces.Num(); ++SurfaceIndex)
				{
					if (!WoTWorldEntrySurfacePanUV(Source, Model.GetValue(), SurfaceIndex,
						Lighting.Atlas.Surfaces[SurfaceIndex].PanUV))
					{
						return FResult::Error(WoTWorldError(
							EWoTBootstrapError::InvalidNumber,
							TEXT("an entry BSP surface has invalid pan semantics"),
							Source.SourcePath));
					}
				}
			}
			FWoTBootstrapStatus Composed = WoTWorldComposeAtlas(Source,
				Model.GetValue(), Lighting.Atlas, Lighting.LightningLights,
				Lighting.WeatherPairs, Lighting.LevelBrightness, Lighting.Stats,
				Lighting.Manifest, Lighting.Responses);
			if (Composed.IsError())
				return FResult::Error(MoveTemp(Composed).GetError());
			if (Source.MapIdentity != TEXT("WheelOfTime"))
			{
				FWoTWorldSkyRest SkyRest;
				FWoTBootstrapStatus SkyRestBuilt = WoTWorldBuildSkyRestTrailer(Source,
					Model.GetValue(), Lighting.Atlas, SkyRest, Lighting.SkyRestTrailer);
				if (SkyRestBuilt.IsError())
					return FResult::Error(MoveTemp(SkyRestBuilt).GetError());
			}
			TWoTBootstrapResult<TArray<FWoTWorldBSPPartData>> Main =
				WoTWorldEncodeBSP(Source, Model.GetValue(), Lighting, false,
					bSplitProceduralBSP);
			TWoTBootstrapResult<TArray<FWoTWorldBSPPartData>> Portal =
				bIncludePortalBSP
				? WoTWorldEncodeBSP(Source, Model.GetValue(), Lighting, true,
					bSplitProceduralBSP)
				: TWoTBootstrapResult<TArray<FWoTWorldBSPPartData>>::Success(
					TArray<FWoTWorldBSPPartData>());
			if (Main.IsError()) return FResult::Error(MoveTemp(Main).GetError());
			if (Portal.IsError()) return FResult::Error(MoveTemp(Portal).GetError());
			Parts = MoveTemp(Main).GetValue();
			Parts.Append(MoveTemp(Portal).GetValue());
			if (Parts.Num() != Map.BSPFact->Outputs.Num())
			{
				return FResult::Error(WoTWorldError(EWoTBootstrapError::UnknownField,
					TEXT("the BSP output vector disagrees with the canonical split"),
					Source.MapIdentity));
			}
			for (int32 PartIndex = 0; PartIndex < Parts.Num(); ++PartIndex)
			{
				const FString ExpectedLogicalId = FString(TEXT("cache/bsp/"))
					+ Source.MapIdentity + TEXT("/") + Parts[PartIndex].PartIdentity;
				if (Map.BSPFact->Outputs[PartIndex].LogicalId != ExpectedLogicalId)
				{
					return FResult::Error(WoTWorldError(EWoTBootstrapError::UnknownField,
						TEXT("a BSP output logical ID/order disagrees with split_world_glb"),
						Map.BSPFact->Outputs[PartIndex].LogicalId));
				}
			}
			BSPObjectCount += Parts.Num();
			LightmapStats = MoveTemp(Lighting.Stats);
		}
		Models.Add(MoveTemp(Model).GetValue());
		BSPPartsByMap.Add(MoveTemp(Parts));
		LightmapStatsByMap.Add(MoveTemp(LightmapStats));
	}

	// All semantic closure is complete. Writes below are immutable temporary
	// objects; a later I/O/cancellation failure still returns no completed set.
	FWoTWorldSetResult Result;
	Result.Objects.Reserve(BSPObjectCount + Sources.Num());
	Result.Worlds.Reserve(Sources.Num());
	for (int32 MapIndex = 0; MapIndex < Sources.Num(); ++MapIndex)
	{
		if (Progress.IsCancellationRequested()) return FResult::Error(WoTWorldCancelled());
		FWoTWorldSource& Source = Sources[MapIndex];
		const FWoTWorldContractMap& Map = ValidatedMaps[MapIndex];
		const FWoTWorldModel& Model = Models[MapIndex];
		const TArray<FWoTWorldBSPPartData>& Parts = BSPPartsByMap[MapIndex];
		TArray<TArray<uint8>> BSPObjects;
		BSPObjects.SetNum(Parts.Num());
		for (int32 PartIndex = 0; PartIndex < Parts.Num(); ++PartIndex)
		{
			const FWoTWorldBSPPartData& Part = Parts[PartIndex];
			TWoTBootstrapResult<TArray<uint8>> Object = WoTWorldBuildBSPObject(
				*Map.BSPFact, Source, Part.Payload,
				Part.Role == EWoTBSPPartRole::Portal);
			if (Object.IsError()) return FResult::Error(MoveTemp(Object).GetError());
			BSPObjects[PartIndex] = MoveTemp(Object).GetValue();
		}
		TArray<uint8> WorldObject;
		{
			TWoTBootstrapResult<TArray<uint8>> BuiltWorld =
				WoTWorldBuildObject(*Map.WorldFact, Source, Model);
			if (BuiltWorld.IsError())
				return FResult::Error(MoveTemp(BuiltWorld).GetError());
			WorldObject = MoveTemp(BuiltWorld).GetValue();
		}
		TArray<const FWoTRequiredFactOutput*> Outputs;
		TArray<TArray<uint8>*> ObjectBytes;
		TArray<FString> FactIds;
		for (int32 PartIndex = 0; PartIndex < Parts.Num(); ++PartIndex)
		{
			Outputs.Add(&Map.BSPFact->Outputs[PartIndex]);
			ObjectBytes.Add(&BSPObjects[PartIndex]);
			FactIds.Add(Map.BSPFact->FactId);
		}
		{
			Outputs.Add(&Map.WorldFact->Outputs[0]);
			ObjectBytes.Add(&WorldObject);
			FactIds.Add(Map.WorldFact->FactId);
		}
		for (int32 OutputIndex = 0; OutputIndex < Outputs.Num(); ++OutputIndex)
		{
			const FWoTRequiredFactOutput& Output = *Outputs[OutputIndex];
			if (static_cast<uint64>(ObjectBytes[OutputIndex]->Num()) > Output.MaxDecodedSize)
			{
				return FResult::Error(WoTWorldError(EWoTBootstrapError::SizeLimit,
					TEXT("a BSP/world object exceeds its contract maximum"), Output.LogicalId));
			}
			const FWoTSha256 ObjectSha256 = Sha256(*ObjectBytes[OutputIndex]);
			{
				FWoTBootstrapStatus Written = Writer.WriteObject(Output.LogicalId,
					Output.Schema, *ObjectBytes[OutputIndex], ObjectSha256,
					Output.MaxDecodedSize);
				if (Written.IsError())
					return FResult::Error(MoveTemp(Written).GetError());
			}
			FWoTCacheBlob& Blob = Result.Objects.AddDefaulted_GetRef();
			Blob.LogicalId = Output.LogicalId;
			Blob.Schema = Output.Schema;
			Blob.Sha256 = ObjectSha256;
			Blob.Size = static_cast<uint64>(ObjectBytes[OutputIndex]->Num());
			Blob.Satisfies.AddDefaulted_GetRef().Value = FactIds[OutputIndex];
		}

		{
			FWoTWorldDescriptor& Descriptor =
				Result.Worlds.AddDefaulted_GetRef();
			Descriptor.MapIdentity = Source.MapIdentity;
			Descriptor.NeutralMapSlot = Map.Slot;
			Descriptor.BSPFactId = Map.BSPFact->FactId;
			Descriptor.BSPImporter = Map.BSPFact->Importer;
			Descriptor.WorldLogicalId = Map.WorldFact->Outputs[0].LogicalId;
			Descriptor.WorldSchema = Map.WorldFact->Outputs[0].Schema;
			Descriptor.WorldFactId = Map.WorldFact->FactId;
			Descriptor.WorldImporter = Map.WorldFact->Importer;
			Descriptor.PackageSha256 = Source.Sha256;
			for (int32 PartIndex = 0; PartIndex < Parts.Num(); ++PartIndex)
			{
				FWoTBSPPartDescriptor& PartDescriptor =
					Descriptor.BSPParts.AddDefaulted_GetRef();
				PartDescriptor.Role = Parts[PartIndex].Role;
				PartDescriptor.PartIdentity = Parts[PartIndex].PartIdentity;
				PartDescriptor.ProceduralMaterialSlot =
					Parts[PartIndex].ProceduralMaterialSlot;
				PartDescriptor.LogicalId =
					Map.BSPFact->Outputs[PartIndex].LogicalId;
				PartDescriptor.Schema = Map.BSPFact->Outputs[PartIndex].Schema;
				PartDescriptor.FactId = Map.BSPFact->FactId;
				PartDescriptor.PayloadSha256 = Sha256(Parts[PartIndex].Payload);
				PartDescriptor.Sha256 = Sha256(BSPObjects[PartIndex]);
				PartDescriptor.VertexCount = Parts[PartIndex].VertexCount;
				PartDescriptor.IndexCount = Parts[PartIndex].IndexCount;
				PartDescriptor.SectionCount = Parts[PartIndex].SectionCount;
			}
			Descriptor.WorldSha256 = Sha256(WorldObject);
			Descriptor.SourceRoot = WoTWorldMapRoot(Source);
			Descriptor.ActorCount = static_cast<uint32>(Source.Actors.Num());
			Descriptor.AttachmentCount = Source.AttachmentCount;
			Descriptor.BSPNodeCount = static_cast<uint32>(Model.Nodes.Num());
			Descriptor.BSPSurfaceCount = static_cast<uint32>(Model.Surfaces.Num());
			Descriptor.ZoneCount = static_cast<uint32>(Model.Zones.Num());
			for (const FWoTWorldSurface& Surface : Model.Surfaces)
			{
				if ((Surface.Flags & WoTWorldPFPortal) != 0)
				{
					++Descriptor.PortalSurfaceCount;
				}
			}
			Descriptor.MoverCount = Source.MoverCount;
			Descriptor.StaticLightmappedMoverCount =
				Source.StaticLightmappedMoverCount;
			Descriptor.MoverLitPolyCount = Source.MoverLitPolyCount;
			Descriptor.MoverRenderTriangleCount =
				Source.MoverRenderTriangleCount;
			Descriptor.MoverCollisionTriangleCount =
				Source.MoverCollisionTriangleCount;
			Descriptor.MoverUV1OutsidePaddedRectCount =
				Source.MoverUV1OutsidePaddedRectCount;
			Descriptor.MoverPolyOwnershipViolationCount =
				Source.MoverPolyOwnershipViolationCount;
			Descriptor.MoverAnimatedLightPolyCount =
				Source.MoverAnimatedLightPolyCount;
			Descriptor.MoverAtlasWidth = Source.MoverAtlasWidth;
			Descriptor.MoverAtlasHeight = Source.MoverAtlasHeight;
			Descriptor.MoverOracleRGB7 = Source.MoverOracleRGB7;
			Descriptor.LightCount = Source.LightCount;
			Descriptor.LightmapSkippedLights =
				LightmapStatsByMap[MapIndex].SkippedLights;
			Descriptor.LightmapPerTexelZoneSurfaceCount =
				LightmapStatsByMap[MapIndex].PerTexelZoneSurfaceCount;
			Descriptor.SoundCount = Source.SoundCount;
			Descriptor.NavigationSeedCount = Source.NavigationSeedCount;
			Descriptor.SemanticObjectCount =
				static_cast<uint32>(Source.SemanticObjects.Num());
			Descriptor.ReachSpecCount =
				static_cast<uint32>(Source.ReachSpecs.Num());
			TWoTBootstrapResult<FWoTSha256> DescriptorRoot =
				WoTWorldDescriptorRoot(Descriptor);
			if (DescriptorRoot.IsError())
				return FResult::Error(MoveTemp(DescriptorRoot).GetError());
			Descriptor.MapRoot = MoveTemp(DescriptorRoot).GetValue();
		}
		FWoTBootstrapStatus Reported = Progress.ReportProgress(
			TEXT("world-set"), static_cast<uint64>(MapIndex + 1),
			PendingWorldCount);
		if (Reported.IsError()) return FResult::Error(MoveTemp(Reported).GetError());
		if (Progress.IsCancellationRequested()) return FResult::Error(WoTWorldCancelled());
	}
	Result.SetRoot = WoTWorldSetRoot(Result.Worlds);
	return FResult::Success(MoveTemp(Result));
}
} // namespace

#if WITH_AUTOMATION_TESTS
bool ValidateEntryWeatherPairBudgetForTesting(
	TConstArrayView<uint64> PairKeys, TConstArrayView<uint32> PairByteCounts)
{
	if (PairKeys.Num() != PairByteCounts.Num()) return false;
	FWoTWorldEntryPairBudget Budget;
	for (int32 Index = 0; Index < PairKeys.Num(); ++Index)
	{
		if (!PairByteCounts.IsValidIndex(Index)
			|| !Budget.ReserveKey(PairKeys[Index], PairByteCounts[Index]))
		{
			return false;
		}
	}
	return true;
}

bool FilterWorldShadowMaskForTesting(TConstArrayView<uint8> Bits,
	int32 MaskU, int32 VClamp, TArray<uint8>& Out)
{
	if (MaskU < 1 || MaskU > 128 || VClamp < 1 || VClamp > 1024
		|| static_cast<uint64>(MaskU) * VClamp
			!= static_cast<uint64>(Bits.Num())) return false;
	WoTWorldEntryShadowMap(Bits, MaskU, VClamp, Out);
	return true;
}

bool ComposeSyntheticMoverLightmapForTesting(
	uint8& OutFilteredShadow, uint8& OutIllumination, FIntVector& OutRGB7)
{
	FWoTWorldLightMap LightMap;
	LightMap.Pan = FVector3f::ZeroVector;
	LightMap.UClamp = 1;
	LightMap.VClamp = 1;
	LightMap.UScale = 1.0f;
	LightMap.VScale = 1.0f;
	FWoTWorldEntryBasis Basis;
	if (!WoTWorldEntryBasis(FVector3d::ZeroVector,
		FVector3d::YAxisVector, FVector3d::ZAxisVector,
		FVector3d::XAxisVector, LightMap, Basis)) return false;

	FWoTWorldEntryLight Light;
	Light.Location = FVector3d(10.0, 0.0, 0.0);
	Light.Radius = 25.0;
	Light.ReciprocalRadiusExact = 1.0 / 25.0;
	Light.ReciprocalRadius = static_cast<float>(1.0 / 25.0);
	Light.ReciprocalRadiusMultiplier = static_cast<float>(4093.0 / 625.0);
	Light.FixedColor[0] = 13509;
	Light.FixedColor[1] = 0;
	Light.FixedColor[2] = 0;
	const TArray<uint8> Bits = {1};
	FWoTWorldEntrySurfaceLightSpan LightSpan;
	if (!WoTWorldEntrySurfaceLightSpan(LightMap, 0, Bits, LightSpan))
		return false;
	TArray<uint8> Shadow;
	WoTWorldEntryShadowMap(Bits, LightSpan.MaskU, LightMap.VClamp, Shadow);
	if (Shadow.IsEmpty()) return false;
	OutFilteredShadow = Shadow[0];
	TArray<float> LightSqrt;
	WoTWorldEntryBuildLightSqrt(LightSqrt);
	TArray<uint8> Illumination;
	if (!WoTWorldEntryComposeSurfaceLight(
		Bits, LightMap, LightSpan, Light, Basis, LightSqrt, Illumination)
		|| Illumination.IsEmpty()) return false;
	OutIllumination = Illumination[0];
	TArray<FIntVector> Totals = {FIntVector::ZeroValue};
	WoTWorldEntryAddMeanLight(Light, Illumination, Totals);
	OutRGB7 = Totals[0];
	return true;
}

bool ComposeSyntheticMoverAnimationResponseForTesting(const uint8 LightType,
	uint8& OutMeanRGB7, uint8& OutResponseRGB8, bool& bOutAnimated)
{
	if (LightType != 1 && LightType != 2 && LightType != 3 && LightType != 7)
		return false;
	constexpr double FullContribution = 64.0;
	const double Mean = WoTWorldWaveMean(LightType);
	const double Amplitude = WoTWorldWaveAmplitude(LightType);
	const int32 Hi = static_cast<int32>(FMath::FloorToDouble(
		FullContribution * (Mean + Amplitude)));
	const int32 Lo = static_cast<int32>(FMath::FloorToDouble(
		FullContribution * (Mean - Amplitude)));
	OutMeanRGB7 = static_cast<uint8>(FMath::Clamp(
		static_cast<int32>(FMath::FloorToDouble(FullContribution * Mean)), 0, 127));
	OutResponseRGB8 = static_cast<uint8>(FMath::Clamp(Hi - Lo, 0, 255));
	bOutAnimated = Amplitude != 0.0;
	return true;
}

bool ComposeSyntheticMixedMoverResponseForTesting(
	uint8& OutAnimatedOnlyRGB8, uint8& OutWorldMixedRGB8,
	uint8& OutMoverMixedRGB8)
{
	const TArray<FWoTWorldResponseAccumulator> Accumulators = {
		{0, 1, 0, 0}, {0, -1, 0, 0}};
	auto EmptyEndpoints = []()
	{
		TArray<TArray<FIntVector>> Result;
		Result.Add({FIntVector::ZeroValue});
		Result.Add({FIntVector::ZeroValue});
		return Result;
	};
	auto Response = [](const TArray<TArray<FIntVector>>& Endpoints)
	{
		return static_cast<uint8>(FMath::Clamp(
			Endpoints[0][0].X - Endpoints[1][0].X, 0, 255));
	};

	FWoTWorldEntryLight Static;
	Static.LightType = 1;
	Static.Brightness = 255;
	Static.Hue = 0;
	Static.Saturation = 255;
	FWoTWorldEntryLight Blink = Static;
	Blink.LightType = 3;
	const FWoTWorldAnimationLight BlinkAnimation{0, 0};
	const TArray<uint8> StaticIllumination = {255};
	const TArray<uint8> BlinkIllumination = {64};

	TArray<TArray<FIntVector>> AnimatedOnly = EmptyEndpoints();
	WoTWorldEntryAddResponseLight(Blink, BlinkIllumination, 1.0f,
		&BlinkAnimation, Accumulators, AnimatedOnly);
	OutAnimatedOnlyRGB8 = Response(AnimatedOnly);

	TArray<TArray<FIntVector>> WorldMixed = EmptyEndpoints();
	WoTWorldEntryAddResponseLight(Static, StaticIllumination, 1.0f,
		nullptr, Accumulators, WorldMixed);
	WoTWorldEntryAddResponseLight(Blink, BlinkIllumination, 1.0f,
		&BlinkAnimation, Accumulators, WorldMixed);
	OutWorldMixedRGB8 = Response(WorldMixed);

	TArray<TArray<FIntVector>> MoverMixed = EmptyEndpoints();
	WoTWorldEntryAddResponseLight(Static, StaticIllumination, 1.0f,
		nullptr, Accumulators, MoverMixed);
	WoTWorldEntryAddResponseLight(Blink, BlinkIllumination, 1.0f,
		&BlinkAnimation, Accumulators, MoverMixed);
	OutMoverMixedRGB8 = Response(MoverMixed);
	return true;
}

bool BuildLargeMoverAtlasForTesting(FIntPoint& OutSize)
{
	// Five maximum-size decoded lightmaps cannot fit the 2048 candidate (one
	// 1026x1026 padded rect per shelf), but do fit the 4096 candidate.
	TArray<FIntPoint> Clamps;
	Clamps.Init(FIntPoint(1024, 1024), 5);
	const FIntPoint Sizes[] = {{256, 256}, {512, 512}, {1024, 1024},
		{2048, 2048}, {4096, 4096}};
	FWoTWorldAtlas Atlas;
	if (!WoTWorldPackAtlas(Clamps, MakeArrayView(Sizes), Atlas)
		|| static_cast<uint64>(Atlas.Width) * Atlas.Height
			> WoTWorldMaxMoverAtlasPixels)
	{
		return false;
	}
	OutSize = FIntPoint(Atlas.Width, Atlas.Height);
	return true;
}

#endif

FWoTBootstrapStatus ValidateWorldDescriptorContractAssociations(
	TConstArrayView<FWoTWorldDescriptor> Descriptors,
	const FWoTRetailContract& Contract)
{
	TWoTBootstrapResult<TArray<FWoTWorldContractMap>> Validated =
		WoTWorldValidateContract(Contract);
	if (Validated.IsError())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(Validated).GetError());
	}
	if (Descriptors.Num() != Validated.GetValue().Num())
	{
		return FWoTBootstrapStatus::Error(WoTWorldError(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("the descriptor census disagrees with the production world contract")));
	}
	for (int32 Index = 0; Index < Descriptors.Num(); ++Index)
	{
		const FWoTWorldDescriptor& Descriptor = Descriptors[Index];
		const FWoTWorldContractMap& Map = Validated.GetValue()[Index];
		const FWoTRequiredFact& BSP = *Map.BSPFact;
		const FWoTRequiredFact& World = *Map.WorldFact;
		if (Descriptor.MapIdentity != Map.Identity
			|| Descriptor.NeutralMapSlot != Map.Slot
			|| Descriptor.BSPFactId != BSP.FactId
			|| Descriptor.BSPImporter != BSP.Importer
			|| Descriptor.WorldFactId != World.FactId
			|| Descriptor.WorldImporter != World.Importer
			|| Descriptor.WorldLogicalId != World.Outputs[0].LogicalId
			|| Descriptor.WorldSchema != World.Outputs[0].Schema
			|| Descriptor.BSPParts.Num() != BSP.Outputs.Num())
		{
			return FWoTBootstrapStatus::Error(WoTWorldError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("a descriptor slot or world/BSP fact association was substituted"),
				Descriptor.MapIdentity));
		}
		for (int32 PartIndex = 0;
			PartIndex < Descriptor.BSPParts.Num(); ++PartIndex)
		{
			const FWoTBSPPartDescriptor& Part =
				Descriptor.BSPParts[PartIndex];
			const FWoTRequiredFactOutput& Output = BSP.Outputs[PartIndex];
			if (Part.FactId != BSP.FactId
				|| Part.LogicalId != Output.LogicalId
				|| Part.Schema != Output.Schema)
			{
				return FWoTBootstrapStatus::Error(WoTWorldError(
					EWoTBootstrapError::CorruptInstallation,
					TEXT("a descriptor BSP part was substituted across contract slots"),
					Part.LogicalId));
			}
		}
	}
	return FWoTBootstrapStatus::Success();
}

TWoTBootstrapResult<FWoTWorldSetResult> ImportRequiredWorldSet(
	const FWoTRetailPackage& Package, const FWoTRetailContract& Contract,
	IWoTObjectWriter& Writer, IWoTProgressSink& Progress)
{
	FWoTWorldSourceLoader Loader = [&Package](FStringView SourcePath)
	{
		using FLoadResult = TWoTBootstrapResult<TArray<uint8>>;
		TWoTBootstrapResult<FWoTPackageEntry> Entry = Package.FindExact(SourcePath);
		if (Entry.IsError()) return FLoadResult::Error(MoveTemp(Entry).GetError());
		if (Entry.GetValue().Size == 0 || Entry.GetValue().Size > WoTWorldMaxSourceBytes)
		{
			return FLoadResult::Error(WoTWorldError(EWoTBootstrapError::SizeLimit,
				TEXT("the world source declaration exceeds its byte bound"),
				FString(SourcePath)));
		}
		FWoTWorldCollectSink Sink(WoTWorldMaxSourceBytes);
		FWoTBootstrapStatus Streamed = Package.StreamExact(Entry.GetValue(), Sink);
		if (Streamed.IsError()) return FLoadResult::Error(MoveTemp(Streamed).GetError());
		return FLoadResult::Success(MoveTemp(Sink.Bytes));
	};
	TWoTBootstrapResult<TArray<FWoTWorldContractMap>> Validated =
		WoTWorldValidateContract(Contract);
	if (Validated.IsError())
		return TWoTBootstrapResult<FWoTWorldSetResult>::Error(
			MoveTemp(Validated).GetError());
	return WoTWorldImportCore(
		Validated.GetValue(), Writer, Progress, MoveTemp(Loader), true, true);
}

TWoTBootstrapResult<FWoTWorldSetResult> ImportFrontEndWorld(
	const FWoTRetailPackage& Package, const FWoTRetailContract& Contract,
	IWoTObjectWriter& Writer, IWoTProgressSink& Progress)
{
	FWoTWorldSourceLoader Loader = [&Package](FStringView SourcePath)
	{
		using FLoadResult = TWoTBootstrapResult<TArray<uint8>>;
		TWoTBootstrapResult<FWoTPackageEntry> Entry = Package.FindExact(SourcePath);
		if (Entry.IsError()) return FLoadResult::Error(MoveTemp(Entry).GetError());
		if (Entry.GetValue().Size == 0 || Entry.GetValue().Size > WoTWorldMaxSourceBytes)
		{
			return FLoadResult::Error(WoTWorldError(EWoTBootstrapError::SizeLimit,
				TEXT("the entry world source declaration exceeds its byte bound"),
				FString(SourcePath)));
		}
		FWoTWorldCollectSink Sink(WoTWorldMaxSourceBytes);
		FWoTBootstrapStatus Streamed = Package.StreamExact(Entry.GetValue(), Sink);
		if (Streamed.IsError()) return FLoadResult::Error(MoveTemp(Streamed).GetError());
		return FLoadResult::Success(MoveTemp(Sink.Bytes));
	};
	TWoTBootstrapResult<TArray<FWoTWorldContractMap>> Validated =
		WoTWorldValidateFrontEndContract(Contract);
	if (Validated.IsError())
		return TWoTBootstrapResult<FWoTWorldSetResult>::Error(
			MoveTemp(Validated).GetError());
	return WoTWorldImportCore(Validated.GetValue(), Writer, Progress,
		MoveTemp(Loader), false, false);
}

#if WITH_AUTOMATION_TESTS
TWoTBootstrapResult<FWoTWorldSetResult> ImportRequiredWorldSetPackageBytesForTesting(
	TConstArrayView<FWoTWorldPackageBytesForTesting> Sources,
	const FWoTRetailContract& Contract, IWoTObjectWriter& Writer,
	IWoTProgressSink& Progress)
{
	FWoTWorldSourceLoader Loader = [Sources](FStringView SourcePath)
	{
		using FLoadResult = TWoTBootstrapResult<TArray<uint8>>;
		for (const FWoTWorldPackageBytesForTesting& Source : Sources)
		{
			if (Source.SourcePath == SourcePath)
			{
				TArray<uint8> Copy;
				Copy.Append(Source.Bytes);
				return FLoadResult::Success(MoveTemp(Copy));
			}
		}
		return FLoadResult::Error(WoTWorldError(EWoTBootstrapError::UnknownField,
			TEXT("the test world source set is missing a requested map"),
			FString(SourcePath)));
	};
	TWoTBootstrapResult<TArray<FWoTWorldContractMap>> Validated =
		WoTWorldValidateContract(Contract);
	if (Validated.IsError())
		return TWoTBootstrapResult<FWoTWorldSetResult>::Error(
			MoveTemp(Validated).GetError());
	return WoTWorldImportCore(
		Validated.GetValue(), Writer, Progress, MoveTemp(Loader), true, true);
}

TWoTBootstrapResult<FWoTWorldSetResult> ImportFrontEndWorldPackageBytesForTesting(
	TConstArrayView<FWoTWorldPackageBytesForTesting> Sources,
	const FWoTRetailContract& Contract, IWoTObjectWriter& Writer,
	IWoTProgressSink& Progress)
{
	FWoTWorldSourceLoader Loader = [Sources](FStringView SourcePath)
	{
		using FLoadResult = TWoTBootstrapResult<TArray<uint8>>;
		for (const FWoTWorldPackageBytesForTesting& Source : Sources)
		{
			if (Source.SourcePath == SourcePath)
			{
				TArray<uint8> Copy;
				Copy.Append(Source.Bytes);
				return FLoadResult::Success(MoveTemp(Copy));
			}
		}
		return FLoadResult::Error(WoTWorldError(EWoTBootstrapError::UnknownField,
			TEXT("the test entry source set is missing the requested map"),
			FString(SourcePath)));
	};
	TWoTBootstrapResult<TArray<FWoTWorldContractMap>> Validated =
		WoTWorldValidateFrontEndContract(Contract);
	if (Validated.IsError())
		return TWoTBootstrapResult<FWoTWorldSetResult>::Error(
			MoveTemp(Validated).GetError());
	return WoTWorldImportCore(Validated.GetValue(), Writer, Progress,
		MoveTemp(Loader), false, false);
}

FWoTBootstrapStatus ValidateWorldAttachmentsForTesting(
	TConstArrayView<int32> ParentByActor)
{
	return WoTWorldValidateAttachments(ParentByActor);
}

TWoTBootstrapResult<FWoTWorldSetResult>
ValidateWorldSemanticFixtureForTesting(TConstArrayView<uint8> CanonicalBytes)
{
	using FResult = TWoTBootstrapResult<FWoTWorldSetResult>;
	FWoTCanonicalJsonLimits Limits;
	Limits.MaxBytes = 4 * 1024 * 1024;
	Limits.MaxMembers = 1000000;
	TWoTBootstrapResult<FWoTCanonicalJsonValue> Parsed =
		ParseCanonicalJson(CanonicalBytes, Limits);
	if (Parsed.IsError()) return FResult::Error(MoveTemp(Parsed).GetError());
	const FWoTCanonicalJsonValue& Root = Parsed.GetValue();
	const TCHAR* RootKeys[] = {TEXT("format"), TEXT("maps"), TEXT("set_root")};
	if (RequireExactMembers(Root, MakeArrayView(RootKeys),
			TEXT("semantic world fixture")).IsSet())
	{
		return FResult::Error(WoTWorldError(EWoTBootstrapError::UnknownField,
			TEXT("the semantic fixture root has the wrong closed schema")));
	}
	const FWoTCanonicalJsonValue* MapsValue = Root.FindMember(TEXT("maps"));
	if (MapsValue == nullptr || !MapsValue->IsArray())
	{
		return FResult::Error(WoTWorldError(EWoTBootstrapError::UnknownField,
			TEXT("the semantic fixture has no map array")));
	}
	auto String = [](const FWoTCanonicalJsonValue& Object,
		const TCHAR* Name, FString& Out) -> bool
	{
		const FWoTCanonicalJsonValue* Value = Object.FindMember(Name);
		if (Value == nullptr
			|| Value->GetKind() != EWoTCanonicalJsonKind::String) return false;
		Out = Value->GetString();
		return !Out.IsEmpty();
	};
	auto Integer = [](const FWoTCanonicalJsonValue& Object,
		const TCHAR* Name, int32& Out) -> bool
	{
		const FWoTCanonicalJsonValue* Value = Object.FindMember(Name);
		if (Value == nullptr
			|| Value->GetKind() != EWoTCanonicalJsonKind::Integer
			|| Value->GetInteger() < MIN_int32
			|| Value->GetInteger() > MAX_int32) return false;
		Out = static_cast<int32>(Value->GetInteger());
		return true;
	};
	auto HashLength = [](FWoTSha256Hasher& Hasher, uint64 Value)
	{
		uint8 Bytes[8];
		for (uint32 ByteIndex = 0; ByteIndex < UE_ARRAY_COUNT(Bytes); ++ByteIndex)
			Bytes[ByteIndex] = static_cast<uint8>(Value >> (ByteIndex * 8));
		Hasher.Update(MakeArrayView(Bytes));
	};
	TArray<FWoTWorldSemanticValidationMap> Maps;
	Maps.Reserve(MapsValue->Num());
	TSet<FString> ResourceIdentities;
	for (int32 MapIndex = 0; MapIndex < MapsValue->Num(); ++MapIndex)
	{
		const FWoTCanonicalJsonValue& MapValue = MapsValue->GetElement(MapIndex);
		const TCHAR* MapKeys[] = {TEXT("actors"), TEXT("attachments"), TEXT("bsp"),
			TEXT("identity"), TEXT("lights"), TEXT("map_root"),
			TEXT("navigation_seeds"), TEXT("sky_zone"), TEXT("sounds")};
		if (RequireExactMembers(MapValue, MakeArrayView(MapKeys),
				TEXT("semantic map")).IsSet())
		{
			return FResult::Error(WoTWorldError(EWoTBootstrapError::UnknownField,
				TEXT("a semantic fixture map has the wrong closed schema")));
		}
		FWoTWorldSemanticValidationMap& Map = Maps.AddDefaulted_GetRef();
		if (!String(MapValue, TEXT("identity"), Map.Identity))
			return FResult::Error(WoTWorldError(EWoTBootstrapError::UnknownField,
				TEXT("a semantic fixture map has no identity")));
		FString MapRootHex;
		if (!String(MapValue, TEXT("map_root"), MapRootHex))
			return FResult::Error(WoTWorldError(EWoTBootstrapError::UnknownField,
				TEXT("a semantic fixture map has no event root"), Map.Identity));
		TWoTBootstrapResult<FWoTSha256> MapRoot =
			FWoTSha256::FromHex(MapRootHex);
		if (MapRoot.IsError())
			return FResult::Error(MoveTemp(MapRoot).GetError());
		Map.MapRoot = MapRoot.GetValue();
		TWoTBootstrapResult<TArray<uint8>> CanonicalMap = WriteCanonicalJson(MapValue);
		if (CanonicalMap.IsError())
			return FResult::Error(MoveTemp(CanonicalMap).GetError());
		const FUTF8ToTCHAR MapConverted(
			reinterpret_cast<const ANSICHAR*>(CanonicalMap.GetValue().GetData()),
			CanonicalMap.GetValue().Num());
		FString MapWithoutRoot(MapConverted.Length(), MapConverted.Get());
		const FString RootMember = TEXT(",\"map_root\":\"")
			+ MapRootHex + TEXT("\"");
		if (MapWithoutRoot.ReplaceInline(
				*RootMember, TEXT(""), ESearchCase::CaseSensitive) != 1)
		{
			return FResult::Error(WoTWorldError(
				EWoTBootstrapError::NonCanonicalJson,
				TEXT("a semantic fixture map root is not canonically framed"),
				Map.Identity));
		}
		const FTCHARToUTF8 RootInput(*MapWithoutRoot);
		FWoTSha256Hasher MapHasher;
		static const ANSICHAR MapDomain[] = "WOT-WORLD-MAP-V3";
		MapHasher.Update(TConstArrayView<uint8>(
			reinterpret_cast<const uint8*>(MapDomain), UE_ARRAY_COUNT(MapDomain)));
		HashLength(MapHasher, static_cast<uint64>(RootInput.Length()));
		MapHasher.Update(TConstArrayView<uint8>(
			reinterpret_cast<const uint8*>(RootInput.Get()), RootInput.Length()));
		if (MapHasher.Finish() != Map.MapRoot)
		{
			return FResult::Error(WoTWorldError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("a semantic fixture map event root does not match its events"),
				Map.Identity));
		}
		const FWoTCanonicalJsonValue* Actors = MapValue.FindMember(TEXT("actors"));
		if (Actors == nullptr || !Actors->IsArray() || Actors->Num() == 0)
			return FResult::Error(WoTWorldError(EWoTBootstrapError::UnknownField,
				TEXT("a semantic fixture map has no actors"), Map.Identity));
		for (int32 ActorIndex = 0; ActorIndex < Actors->Num(); ++ActorIndex)
		{
			const FWoTCanonicalJsonValue& ActorValue =
				Actors->GetElement(ActorIndex);
			const TCHAR* ActorKeys[] = {TEXT("class"), TEXT("identity"),
				TEXT("properties"), TEXT("references")};
			if (RequireExactMembers(ActorValue, MakeArrayView(ActorKeys),
					TEXT("semantic actor")).IsSet())
				return FResult::Error(WoTWorldError(
					EWoTBootstrapError::UnknownField,
					TEXT("a semantic fixture actor has the wrong closed schema")));
			FWoTWorldSemanticValidationObject& Actor =
				Map.Objects.AddDefaulted_GetRef();
			Actor.bActor = true;
			if (!String(ActorValue, TEXT("identity"), Actor.Identity)
				|| !String(ActorValue, TEXT("class"), Actor.ClassIdentity))
				return FResult::Error(WoTWorldError(
					EWoTBootstrapError::UnknownField,
					TEXT("a semantic fixture actor lacks a typed identity")));
			const FWoTCanonicalJsonValue* References =
				ActorValue.FindMember(TEXT("references"));
			if (References == nullptr || !References->IsArray())
				return FResult::Error(WoTWorldError(
					EWoTBootstrapError::UnknownField,
					TEXT("a semantic fixture actor lacks its reference array")));
			for (int32 Index = 0; Index < References->Num(); ++Index)
			{
				const FWoTCanonicalJsonValue& Target =
					References->GetElement(Index);
				if (Target.GetKind() != EWoTCanonicalJsonKind::String
					|| Target.GetString().IsEmpty())
					return FResult::Error(WoTWorldError(
						EWoTBootstrapError::UnknownField,
						TEXT("a semantic fixture reference is not a string")));
				Map.References.Add({Actor.Identity, Target.GetString()});
				if (Target.GetString().StartsWith(TEXT("sound:"))
					&& !ResourceIdentities.Contains(
						WoTWorldFold(Target.GetString())))
				{
					Map.Objects.Add({
						Target.GetString(), TEXT("Engine.Sound"), false});
					ResourceIdentities.Add(WoTWorldFold(Target.GetString()));
				}
			}
		}
		const FWoTCanonicalJsonValue* Attachments =
			MapValue.FindMember(TEXT("attachments"));
		if (Attachments == nullptr || !Attachments->IsArray())
			return FResult::Error(WoTWorldError(EWoTBootstrapError::UnknownField,
				TEXT("a semantic fixture lacks attachments")));
		for (int32 Index = 0; Index < Attachments->Num(); ++Index)
		{
			FWoTWorldSemanticValidationAttachment& Attachment =
				Map.Attachments.AddDefaulted_GetRef();
			const FWoTCanonicalJsonValue& Value =
				Attachments->GetElement(Index);
			if (!String(Value, TEXT("child"), Attachment.Child)
				|| !String(Value, TEXT("parent"), Attachment.Parent))
				return FResult::Error(WoTWorldError(
					EWoTBootstrapError::UnknownField,
					TEXT("a semantic fixture attachment is invalid")));
		}
		auto ReadIdentityArray = [&MapValue, &String](
			const TCHAR* Name, TArray<FString>& Out) -> bool
		{
			const FWoTCanonicalJsonValue* Values = MapValue.FindMember(Name);
			if (Values == nullptr || !Values->IsArray()) return false;
			for (int32 Index = 0; Index < Values->Num(); ++Index)
			{
				const FWoTCanonicalJsonValue& Value = Values->GetElement(Index);
				if (Value.GetKind() != EWoTCanonicalJsonKind::String
					|| Value.GetString().IsEmpty()) return false;
				Out.Add(Value.GetString());
			}
			return true;
		};
		if (!ReadIdentityArray(TEXT("lights"), Map.Lights)
			|| !ReadIdentityArray(
				TEXT("navigation_seeds"), Map.NavigationSeeds)
			|| !String(MapValue, TEXT("sky_zone"), Map.SkyZone))
			return FResult::Error(WoTWorldError(EWoTBootstrapError::UnknownField,
				TEXT("a semantic fixture typed actor registry is invalid")));
		const FWoTCanonicalJsonValue* Sounds = MapValue.FindMember(TEXT("sounds"));
		if (Sounds == nullptr || !Sounds->IsArray())
			return FResult::Error(WoTWorldError(EWoTBootstrapError::UnknownField,
				TEXT("a semantic fixture sound registry is invalid")));
		for (int32 Index = 0; Index < Sounds->Num(); ++Index)
		{
			FWoTWorldSemanticValidationSound& Sound =
				Map.Sounds.AddDefaulted_GetRef();
			const FWoTCanonicalJsonValue& Value = Sounds->GetElement(Index);
			if (!String(Value, TEXT("actor"), Sound.Actor)
				|| !String(Value, TEXT("asset"), Sound.Asset))
				return FResult::Error(WoTWorldError(
					EWoTBootstrapError::UnknownField,
					TEXT("a semantic fixture sound binding is invalid")));
			if (!ResourceIdentities.Contains(WoTWorldFold(Sound.Asset)))
			{
				Map.Objects.Add({Sound.Asset, TEXT("Engine.Sound"), false});
				ResourceIdentities.Add(WoTWorldFold(Sound.Asset));
			}
		}
		const FWoTCanonicalJsonValue* BSP = MapValue.FindMember(TEXT("bsp"));
		if (BSP == nullptr || !BSP->IsObject())
			return FResult::Error(WoTWorldError(EWoTBootstrapError::UnknownField,
				TEXT("a semantic fixture lacks BSP state")));
		const FWoTCanonicalJsonValue* Surfaces = BSP->FindMember(TEXT("surfaces"));
		const FWoTCanonicalJsonValue* Nodes = BSP->FindMember(TEXT("nodes"));
		const FWoTCanonicalJsonValue* Zones = BSP->FindMember(TEXT("zones"));
		const FWoTCanonicalJsonValue* Portals = BSP->FindMember(TEXT("portals"));
		const FWoTCanonicalJsonValue* Collision =
			BSP->FindMember(TEXT("collision_surfaces"));
		if (Surfaces == nullptr || !Surfaces->IsArray()
			|| Nodes == nullptr || !Nodes->IsArray()
			|| Zones == nullptr || !Zones->IsArray() || Zones->Num() == 0
			|| Portals == nullptr || !Portals->IsArray()
			|| Collision == nullptr || !Collision->IsArray())
			return FResult::Error(WoTWorldError(EWoTBootstrapError::UnknownField,
				TEXT("a semantic fixture has malformed BSP vectors")));
		for (int32 Index = 0; Index < Surfaces->Num(); ++Index)
		{
			int32 Flags = 0;
			if (!Integer(Surfaces->GetElement(Index), TEXT("flags"), Flags)
				|| Flags < 0)
				return FResult::Error(WoTWorldError(
					EWoTBootstrapError::InvalidNumber,
					TEXT("a semantic fixture BSP surface is invalid")));
			Map.SurfaceFlags.Add(static_cast<uint32>(Flags));
		}
		Map.ZoneLimit = Zones->Num();
		Map.MinimumNodeVertices = 3;
		for (int32 Index = 0; Index < Zones->Num(); ++Index)
		{
			FString Actor;
			if (!String(Zones->GetElement(Index), TEXT("actor"), Actor))
				return FResult::Error(WoTWorldError(
					EWoTBootstrapError::UnknownField,
					TEXT("a semantic fixture BSP zone is invalid")));
			Map.ZoneActors.Add(MoveTemp(Actor));
		}
		for (int32 Index = 0; Index < Nodes->Num(); ++Index)
		{
			const FWoTCanonicalJsonValue& Value = Nodes->GetElement(Index);
			FWoTWorldSemanticValidationNode& Node =
				Map.Nodes.AddDefaulted_GetRef();
			if (!Integer(Value, TEXT("surface"), Node.Surface)
				|| !Integer(Value, TEXT("back"), Node.Back)
				|| !Integer(Value, TEXT("front"), Node.Front))
				return FResult::Error(WoTWorldError(
					EWoTBootstrapError::UnknownField,
					TEXT("a semantic fixture BSP node is invalid")));
			const FWoTCanonicalJsonValue* Vertices =
				Value.FindMember(TEXT("vertex_indices"));
			const FWoTCanonicalJsonValue* NodeZones =
				Value.FindMember(TEXT("zones"));
			if (Vertices == nullptr || !Vertices->IsArray()
				|| NodeZones == nullptr || !NodeZones->IsArray()
				|| NodeZones->Num() != 2)
				return FResult::Error(WoTWorldError(
					EWoTBootstrapError::UnknownField,
					TEXT("a semantic fixture BSP node vector is invalid")));
			for (int32 Vertex = 0; Vertex < Vertices->Num(); ++Vertex)
			{
				if (Vertices->GetElement(Vertex).GetKind()
					!= EWoTCanonicalJsonKind::Integer)
					return FResult::Error(WoTWorldError(
						EWoTBootstrapError::InvalidNumber,
						TEXT("a semantic fixture BSP vertex index is invalid")));
				Node.VertexIndices.Add(
					static_cast<int32>(Vertices->GetElement(Vertex).GetInteger()));
			}
			for (int32 Side = 0; Side < 2; ++Side)
			{
				if (NodeZones->GetElement(Side).GetKind()
					!= EWoTCanonicalJsonKind::Integer)
					return FResult::Error(WoTWorldError(
						EWoTBootstrapError::InvalidNumber,
						TEXT("a semantic fixture BSP node zone is invalid")));
				Node.Zones[Side] =
					static_cast<int32>(NodeZones->GetElement(Side).GetInteger());
			}
		}
		for (int32 Index = 0; Index < Portals->Num(); ++Index)
		{
			const FWoTCanonicalJsonValue& Value = Portals->GetElement(Index);
			FWoTWorldSemanticValidationPortal& Portal =
				Map.Portals.AddDefaulted_GetRef();
			if (!Integer(Value, TEXT("surface"), Portal.Surface)
				|| !Integer(Value, TEXT("back_zone"), Portal.Zones[0])
				|| !Integer(Value, TEXT("front_zone"), Portal.Zones[1]))
				return FResult::Error(WoTWorldError(
					EWoTBootstrapError::UnknownField,
					TEXT("a semantic fixture BSP portal is invalid")));
		}
		for (int32 Index = 0; Index < Collision->Num(); ++Index)
		{
			const FWoTCanonicalJsonValue& Value =
				Collision->GetElement(Index);
			if (Value.GetKind() != EWoTCanonicalJsonKind::Integer)
				return FResult::Error(WoTWorldError(
					EWoTBootstrapError::InvalidNumber,
					TEXT("a semantic fixture collision surface is invalid")));
			Map.CollisionSurfaces.Add(static_cast<int32>(Value.GetInteger()));
		}
	}
	FWoTBootstrapStatus Status = WoTWorldValidateSemanticMaps(Maps, true);
	if (Status.IsError()) return FResult::Error(MoveTemp(Status).GetError());
	FString SetRootHex;
	if (!String(Root, TEXT("set_root"), SetRootHex))
		return FResult::Error(WoTWorldError(EWoTBootstrapError::UnknownField,
			TEXT("the semantic fixture has no world-set root")));
	TWoTBootstrapResult<FWoTSha256> SetRoot = FWoTSha256::FromHex(SetRootHex);
	if (SetRoot.IsError()) return FResult::Error(MoveTemp(SetRoot).GetError());
	FWoTSha256Hasher SetHasher;
	static const ANSICHAR SetDomain[] = "WOT-WORLD-SET-V3";
	SetHasher.Update(TConstArrayView<uint8>(
		reinterpret_cast<const uint8*>(SetDomain), UE_ARRAY_COUNT(SetDomain)));
	const uint32 MapCount = static_cast<uint32>(Maps.Num());
	uint8 CountBytes[4];
	for (uint32 ByteIndex = 0; ByteIndex < UE_ARRAY_COUNT(CountBytes); ++ByteIndex)
		CountBytes[ByteIndex] = static_cast<uint8>(MapCount >> (ByteIndex * 8));
	SetHasher.Update(MakeArrayView(CountBytes));
	for (const FWoTWorldSemanticValidationMap& Map : Maps)
	{
		const FTCHARToUTF8 Identity(*Map.Identity);
		HashLength(SetHasher, static_cast<uint64>(Identity.Length()));
		SetHasher.Update(TConstArrayView<uint8>(
			reinterpret_cast<const uint8*>(Identity.Get()), Identity.Length()));
		HashLength(SetHasher, UE_ARRAY_COUNT(Map.MapRoot.Bytes));
		SetHasher.Update(MakeArrayView(Map.MapRoot.Bytes));
	}
	if (SetHasher.Finish() != SetRoot.GetValue())
	{
		return FResult::Error(WoTWorldError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the semantic fixture world-set root does not match its map events")));
	}
	FWoTWorldSetResult Result;
	Result.Worlds.SetNum(Maps.Num());
	for (int32 Index = 0; Index < Maps.Num(); ++Index)
	{
		Result.Worlds[Index].MapIdentity = Maps[Index].Identity;
		Result.Worlds[Index].MapRoot = Maps[Index].MapRoot;
	}
	Result.SetRoot = SetRoot.GetValue();
	return FResult::Success(MoveTemp(Result));
}

bool ResolveWorldLightDefaultsForTesting(FStringView LeafClass,
	uint8& OutLightType, uint8& OutEffect, uint8& OutBrightness,
	uint8& OutHue, uint8& OutSaturation, uint8& OutRadius,
	uint8& OutCone, bool& bOutSunLightRadius)
{
	FWoTWorldLightDefaults Defaults;
	if (!WoTWorldLightDefaults(LeafClass, Defaults)) return false;
	OutLightType = Defaults.LightType;
	OutEffect = Defaults.Effect;
	OutBrightness = Defaults.Brightness;
	OutHue = Defaults.Hue;
	OutSaturation = Defaults.Saturation;
	OutRadius = Defaults.Radius;
	OutCone = Defaults.Cone;
	bOutSunLightRadius = Defaults.bSunLightRadius;
	return true;
}

FString ClassifyWorldLightSkipForTesting(bool bActorPresent,
	bool bRecognizedClass, bool bLightningOff, uint8 LightType,
	uint8 Brightness, uint8 Effect, bool bMaskInRange)
{
	return WoTWorldClassifyLightSkip(bActorPresent, bRecognizedClass,
		bLightningOff, LightType, Brightness, Effect, bMaskInRange);
}

double ProjectWorldLightLocationForTesting(double Value)
{
	return WoTWorldProjectT3DLocation(Value);
}

FWoTBootstrapStatus ValidateWorldAtlasCarrierForTesting(bool bAtlasComposed,
	FStringView MapIdentity, TConstArrayView<FString> PartIdentities)
{
	return WoTWorldValidateAtlasCarrier(
		bAtlasComposed, MapIdentity, PartIdentities);
}

bool EmptyWorldBSPEncodingFailsForComposedAtlasForTesting(bool bAtlasComposed)
{
	FWoTWorldSource Source;
	Source.MapIdentity = TEXT("EmptyWorld");
	FWoTWorldModel Model;
	FWoTWorldComposedAtlas Lighting;
	if (bAtlasComposed) Lighting.Atlas.BGRA.Add(0);
	return WoTWorldEncodeBSP(
		Source, Model, Lighting, false, true).IsError();
}

bool WorldBSPSectionIdentityLengthsValidForTesting(
	TConstArrayView<int32> Lengths)
{
	return WoTWorldSectionIdentityLengthsValid(Lengths);
}

bool ReadWorldPropertiesFixtureForTesting(TConstArrayView<FString> Names,
	TConstArrayView<uint8> Bytes)
{
	FWoTWorldSource Source;
	for (const FString& Name : Names) Source.Names.Add(Name);
	FWoTWorldReader Reader(Bytes);
	uint64 PropertyCount = 0;
	return WoTWorldReadProperties(Reader, Source, nullptr, PropertyCount)
		&& Reader.Tell() == static_cast<uint64>(Bytes.Num());
}
#endif
