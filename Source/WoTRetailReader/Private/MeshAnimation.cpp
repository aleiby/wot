#include "WoTRetailReader/MeshAnimation.h"

#include "CanonicalObjectBuilder.h"
#include "Misc/Paths.h"
#include "WoTBootstrapCore/CanonicalJson.h"
#include "WoTBootstrapCore/Hash.h"

namespace
{
constexpr uint64 WoTMeshMaxSourceBytes = 256ull * 1024 * 1024;
constexpr uint32 WoTMeshMaxTableItems = 65536;
constexpr uint32 WoTMeshMaxNameBytes = 1024;
constexpr uint32 WoTMeshMaxOuterDepth = 32;
constexpr uint32 WoTMeshMaxVertices = 1000000;
constexpr uint32 WoTMeshMaxFrames = 1000000;
constexpr uint32 WoTMeshMaxSequences = 65536;
constexpr uint32 WoTMeshMaxNotifies = 65536;
constexpr uint64 WoTMeshMaxPackedVertices = 64000000;
constexpr uint32 WoTMeshMaxHeaderBytes = 1024 * 1024;
constexpr uint64 WoTMeshCanonicalHeaderBaseBytes = 32ull * 1024;
constexpr uint64 WoTMeshCanonicalHeaderBytesPerTriangle = 668;
constexpr uint64 WoTMeshCanonicalPayloadBaseBytes = 104;
constexpr uint64 WoTMeshCanonicalPayloadBytesPerTriangle = 122;
constexpr uint64 WoTMeshMaxWorkingSetBytes = 512ull * 1024 * 1024;
constexpr uint64 WoTMeshWorkingBytesPerTriangle = 16ull * 1024;
constexpr uint32 WoTMeshMaxSerializedTriangles = 10000000;
constexpr uint32 WoTMeshMaxTriangles = static_cast<uint32>(
	(WoTMeshMaxHeaderBytes - WoTMeshCanonicalHeaderBaseBytes)
	/ WoTMeshCanonicalHeaderBytesPerTriangle);
static_assert(WoTMeshMaxTriangles == 1520);
constexpr uint32 WoTMeshPackageTag = 0x9e2a83c1;
constexpr uint32 WoTMeshHasStack = 0x02000000;
constexpr uint32 WoTMeshInvisible = 0x00000001;
constexpr uint32 WoTMeshSupportedPolyFlags =
	0x00000001 | 0x00000002 | 0x00000004 | 0x00000010
	| 0x00000040 | 0x00000100 | 0x00000800 | 0x00004000 | 0x00400000;
constexpr ANSICHAR WoTMeshObjectMagic[] = "WOTOBJ3";
constexpr ANSICHAR WoTMeshPayloadMagic[] = "WOTMSH3";
constexpr ANSICHAR WoTAnimationPayloadMagic[] = "WOTANI3";
constexpr TCHAR WoTMeshTangentRule[] =
	TEXT("unweighted-unit-face-all-triangles-corner-uv-v1");

FWoTBootstrapError WoTMeshError(EWoTBootstrapError Code,
	const TCHAR* Message, FString Path = FString())
{
	return MakeWoTBootstrapError(Code, Message, MoveTemp(Path));
}

class FWoTMeshReader
{
public:
	FWoTMeshReader() = default;
	explicit FWoTMeshReader(TConstArrayView<uint8> InBytes)
		: Bytes(InBytes), End(static_cast<uint64>(InBytes.Num())) {}
	FWoTMeshReader(TConstArrayView<uint8> InBytes, uint64 InOffset, uint64 InEnd)
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
	bool ReadFloat(float& Out)
	{
		uint32 Bits = 0;
		if (!ReadU32(Bits)) return false;
		FMemory::Memcpy(&Out, &Bits, sizeof(Out));
		return true;
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
	bool ReadBytes(uint64 Count, TArray<uint8>& Out)
	{
		if (!Require(Count) || Count > static_cast<uint64>(MAX_int32)) return false;
		Out.Reset(static_cast<int32>(Count));
		Out.Append(Bytes.GetData() + Offset, static_cast<int32>(Count));
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
	uint64 Remaining() const { return Offset <= End ? End - Offset : 0; }
	bool IsValid() const { return bValid; }

private:
	TConstArrayView<uint8> Bytes;
	uint64 Offset = 0;
	uint64 End = 0;
	bool bValid = true;
};

struct FWoTMeshImport
{
	FString ObjectName;
	int32 Outer = 0;
};

struct FWoTMeshExport
{
	int32 ClassReference = 0;
	int32 Outer = 0;
	FString Name;
	uint32 Flags = 0;
	uint64 Offset = 0;
	uint64 Size = 0;
};

struct FWoTMeshSource
{
	TArray<uint8> Bytes;
	uint16 Version = 0;
	FString PackageName;
	TArray<FString> Names;
	TArray<FWoTMeshImport> Imports;
	TArray<FWoTMeshExport> Exports;
	FWoTSha256 Sha256;

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
				? &Imports[static_cast<int32>(Index)].ObjectName : nullptr;
		}
		if (Reference > 0 && Exports.IsValidIndex(Reference - 1))
		{
			return &Exports[Reference - 1].Name;
		}
		return nullptr;
	}
};

bool WoTMeshLatin1(TConstArrayView<uint8> Raw, FString& Out)
{
	Out.Reset(Raw.Num());
	for (uint8 Byte : Raw)
	{
		if (Byte == 0) return false;
		Out.AppendChar(static_cast<TCHAR>(Byte));
	}
	return true;
}

TWoTBootstrapResult<FWoTMeshSource> WoTMeshParseSource(
	TConstArrayView<uint8> SourceBytes, FStringView Path)
{
	using FResult = TWoTBootstrapResult<FWoTMeshSource>;
	if (SourceBytes.IsEmpty()
		|| static_cast<uint64>(SourceBytes.Num()) > WoTMeshMaxSourceBytes)
	{
		return FResult::Error(WoTMeshError(EWoTBootstrapError::SizeLimit,
			TEXT("the mesh source package exceeds its byte bound"), FString(Path)));
	}
	FWoTMeshSource Source;
	Source.Bytes.Append(SourceBytes.GetData(), SourceBytes.Num());
	Source.PackageName = FPaths::GetBaseFilename(FString(Path));
	Source.Sha256 = Sha256(Source.Bytes);
	FWoTMeshReader Reader(Source.Bytes);
	uint32 Tag = 0;
	uint16 Licensee = 0;
	uint32 PackageFlags = 0;
	int32 NameCount = 0, NameOffset = 0;
	int32 ExportCount = 0, ExportOffset = 0;
	int32 ImportCount = 0, ImportOffset = 0;
	if (!Reader.ReadU32(Tag) || !Reader.ReadU16(Source.Version)
		|| !Reader.ReadU16(Licensee) || !Reader.ReadU32(PackageFlags)
		|| !Reader.ReadI32(NameCount) || !Reader.ReadI32(NameOffset)
		|| !Reader.ReadI32(ExportCount) || !Reader.ReadI32(ExportOffset)
		|| !Reader.ReadI32(ImportCount) || !Reader.ReadI32(ImportOffset)
		|| Tag != WoTMeshPackageTag || Source.Version < 61 || Source.Version > 69
		|| Source.PackageName.IsEmpty()
		|| Source.PackageName.Contains(TEXT("."), ESearchCase::CaseSensitive)
		|| NameCount <= 0 || ExportCount <= 0 || ImportCount < 0
		|| NameCount > static_cast<int32>(WoTMeshMaxTableItems)
		|| ExportCount > static_cast<int32>(WoTMeshMaxTableItems)
		|| ImportCount > static_cast<int32>(WoTMeshMaxTableItems)
		|| NameOffset < 0 || ExportOffset < 0 || ImportOffset < 0)
	{
		return FResult::Error(WoTMeshError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the mesh source is not a bounded UE1 package"), FString(Path)));
	}

	if (!Reader.SetOffset(static_cast<uint64>(NameOffset)))
	{
		return FResult::Error(WoTMeshError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the mesh name table is outside the source"), FString(Path)));
	}
	for (int32 Index = 0; Index < NameCount; ++Index)
	{
		TArray<uint8> Raw;
		if (Source.Version >= 64)
		{
			int32 Length = 0;
			if (!Reader.ReadCompact(Length) || Length <= 0
				|| Length > static_cast<int32>(WoTMeshMaxNameBytes)
				|| !Reader.ReadBytes(static_cast<uint64>(Length), Raw)
				|| Raw.Last() != 0)
			{
				return FResult::Error(WoTMeshError(
					EWoTBootstrapError::CorruptInstallation,
					TEXT("the mesh name table has an invalid counted name"), FString(Path)));
			}
			Raw.Pop(EAllowShrinking::No);
		}
		else
		{
			bool bTerminated = false;
			for (uint32 ByteIndex = 0; ByteIndex < WoTMeshMaxNameBytes; ++ByteIndex)
			{
				uint8 Byte = 0;
				if (!Reader.ReadU8(Byte)) break;
				if (Byte == 0)
				{
					bTerminated = true;
					break;
				}
				Raw.Add(Byte);
			}
			if (!bTerminated)
			{
				return FResult::Error(WoTMeshError(
					EWoTBootstrapError::CorruptInstallation,
					TEXT("the mesh name table is truncated"), FString(Path)));
			}
		}
		uint32 NameFlags = 0;
		FString Name;
		if (!Reader.ReadU32(NameFlags) || !WoTMeshLatin1(Raw, Name)
			|| Name.IsEmpty() || Name.Contains(TEXT("."), ESearchCase::CaseSensitive))
		{
			return FResult::Error(WoTMeshError(EWoTBootstrapError::CorruptInstallation,
				TEXT("the mesh name table contains an unsafe component"),
				FString::Printf(TEXT("source.package.name_table[%d]"), Index)));
		}
		Source.Names.Add(MoveTemp(Name));
	}

	if (!Reader.SetOffset(static_cast<uint64>(ImportOffset)))
	{
		return FResult::Error(WoTMeshError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the mesh import table is outside the source"), FString(Path)));
	}
	for (int32 Index = 0; Index < ImportCount; ++Index)
	{
		int32 ClassPackage = 0, ClassName = 0, Outer = 0, ObjectName = 0;
		if (!Reader.ReadCompact(ClassPackage) || !Reader.ReadCompact(ClassName)
			|| !Reader.ReadI32(Outer) || !Reader.ReadCompact(ObjectName)
			|| Source.NameAt(ClassPackage) == nullptr
			|| Source.NameAt(ClassName) == nullptr
			|| Source.NameAt(ObjectName) == nullptr)
		{
			return FResult::Error(WoTMeshError(EWoTBootstrapError::CorruptInstallation,
				TEXT("the mesh import table is invalid"), FString(Path)));
		}
		FWoTMeshImport& Import = Source.Imports.AddDefaulted_GetRef();
		Import.ObjectName = *Source.NameAt(ObjectName);
		Import.Outer = Outer;
	}

	if (!Reader.SetOffset(static_cast<uint64>(ExportOffset)))
	{
		return FResult::Error(WoTMeshError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the mesh export table is outside the source"), FString(Path)));
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
			return FResult::Error(WoTMeshError(EWoTBootstrapError::CorruptInstallation,
				TEXT("the mesh export table is invalid"), FString(Path)));
		}
		FWoTMeshExport& Export = Source.Exports.AddDefaulted_GetRef();
		Export.ClassReference = ClassReference;
		Export.Outer = Outer;
		Export.Name = *Source.NameAt(NameIndex);
		Export.Flags = ObjectFlags;
		Export.Offset = static_cast<uint64>(SerialOffset);
		Export.Size = static_cast<uint64>(SerialSize);
	}
	// Deliberately NOT validating that every export resolves to a known class.
	// A real script package is full of exports we have no business
	// understanding: WoTPawns.u has 2098 exports, 227 of which carry a NULL
	// class reference because in UE1 a Class object's own class is null.
	// Requiring universal resolution made this reader reject every genuine
	// .u file while passing a synthetic corpus that contained only meshes.
	// A package is not corrupt because it contains something we do not want.
	// Both consumers already handle an unresolved class correctly: mesh
	// selection skips it, and decoding a specific non-mesh export errors on
	// that export alone.
	return FResult::Success(MoveTemp(Source));
}

FString WoTMeshFold(FStringView Value)
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

TWoTBootstrapResult<const FWoTMeshExport*> WoTMeshFindExport(
	const FWoTMeshSource& Source, FStringView SourceExport)
{
	using FResult = TWoTBootstrapResult<const FWoTMeshExport*>;
	TArray<FString> Expected;
	FString(SourceExport).ParseIntoArray(Expected, TEXT("."), false);
	if (Expected.Num() < 2 || WoTMeshFold(Expected[0]) != WoTMeshFold(Source.PackageName))
	{
		return FResult::Error(WoTMeshError(EWoTBootstrapError::UnknownField,
			TEXT("the mesh export is not qualified by its package"), FString(SourceExport)));
	}
	for (const FString& Component : Expected)
	{
		if (Component.IsEmpty())
		{
			return FResult::Error(WoTMeshError(EWoTBootstrapError::UnknownField,
				TEXT("the mesh export contains an empty identity component"),
				TEXT("source.export")));
		}
	}
	TArray<const FWoTMeshExport*> Matches;
	for (int32 ExportIndex = 0; ExportIndex < Source.Exports.Num(); ++ExportIndex)
	{
		const FWoTMeshExport& Export = Source.Exports[ExportIndex];
		const FString* ClassName = Source.ClassName(Export.ClassReference);
		if (ClassName == nullptr
			|| (*ClassName != TEXT("Mesh") && *ClassName != TEXT("LodMesh"))) continue;
		TArray<FString> Reverse;
		TSet<int32> Seen;
		int32 Reference = ExportIndex + 1;
		bool bComplete = false;
		for (uint32 Depth = 0; Depth < WoTMeshMaxOuterDepth; ++Depth)
		{
			if (Reference == 0)
			{
				bComplete = true;
				break;
			}
			if (Seen.Contains(Reference))
			{
				return FResult::Error(WoTMeshError(
					EWoTBootstrapError::CorruptInstallation,
					TEXT("the mesh export outer chain contains a cycle"), FString(SourceExport)));
			}
			Seen.Add(Reference);
			if (Reference > 0)
			{
				if (!Source.Exports.IsValidIndex(Reference - 1)) break;
				const FWoTMeshExport& Outer = Source.Exports[Reference - 1];
				Reverse.Add(Outer.Name);
				Reference = Outer.Outer;
				if (Reference == 0)
				{
					Reverse.Add(Source.PackageName);
					bComplete = true;
					break;
				}
			}
			else
			{
				const int64 ImportIndex = -static_cast<int64>(Reference) - 1;
				if (ImportIndex < 0 || ImportIndex >= Source.Imports.Num()) break;
				const FWoTMeshImport& Import =
					Source.Imports[static_cast<int32>(ImportIndex)];
				Reverse.Add(Import.ObjectName);
				Reference = Import.Outer;
				if (Reference == 0)
				{
					bComplete = true;
					break;
				}
			}
		}
		if (!bComplete)
		{
			return FResult::Error(WoTMeshError(EWoTBootstrapError::CorruptInstallation,
				TEXT("the mesh export outer chain is invalid"), FString(SourceExport)));
		}
		TArray<FString> Ordered;
		for (int32 Index = Reverse.Num() - 1; Index >= 0; --Index) Ordered.Add(Reverse[Index]);
		if (Ordered.Num() != Expected.Num()) continue;
		bool bEqual = true;
		for (int32 Index = 0; Index < Ordered.Num(); ++Index)
		{
			if (WoTMeshFold(Ordered[Index]) != WoTMeshFold(Expected[Index]))
			{
				bEqual = false;
				break;
			}
		}
		if (bEqual) Matches.Add(&Export);
	}
	if (Matches.Num() > 1)
	{
		return FResult::Error(WoTMeshError(EWoTBootstrapError::DuplicateField,
			TEXT("the mesh export identity is ambiguous"), FString(SourceExport)));
	}
	if (Matches.Num() == 1) return FResult::Success(Matches[0]);
	return FResult::Error(WoTMeshError(EWoTBootstrapError::UnknownField,
		TEXT("the exact Mesh/LodMesh export was not found"), FString(SourceExport)));
}

bool WoTMeshReadName(FWoTMeshReader& Reader, const FWoTMeshSource& Source,
	FString& Out)
{
	int32 Index = 0;
	if (!Reader.ReadCompact(Index) || Source.NameAt(Index) == nullptr) return false;
	Out = *Source.NameAt(Index);
	return true;
}

bool WoTMeshSkipProperties(FWoTMeshReader& Reader, const FWoTMeshSource& Source)
{
	for (uint32 PropertyIndex = 0; PropertyIndex < WoTMeshMaxTableItems; ++PropertyIndex)
	{
		FString Name;
		if (!WoTMeshReadName(Reader, Source, Name)) return false;
		if (Name == TEXT("None")) return true;
		uint8 Info = 0;
		if (!Reader.ReadU8(Info)) return false;
		const uint8 Type = Info & 0x0f;
		if (Type == 10)
		{
			FString StructName;
			if (!WoTMeshReadName(Reader, Source, StructName)) return false;
		}
		uint64 Size = 0;
		switch (Info & 0x70)
		{
		case 0x00: Size = 1; break;
		case 0x10: Size = 2; break;
		case 0x20: Size = 4; break;
		case 0x30: Size = 12; break;
		case 0x40: Size = 16; break;
		case 0x50:
		{
			uint8 Value = 0;
			if (!Reader.ReadU8(Value)) return false;
			Size = Value;
			break;
		}
		case 0x60:
		{
			uint16 Value = 0;
			if (!Reader.ReadU16(Value)) return false;
			Size = Value;
			break;
		}
		case 0x70:
		{
			uint32 Value = 0;
			if (!Reader.ReadU32(Value)) return false;
			Size = Value;
			break;
		}
		default: return false;
		}
		if ((Info & 0x80) != 0 && Type != 3)
		{
			uint8 First = 0;
			if (!Reader.ReadU8(First)) return false;
			if ((First & 0x80) != 0
				&& !Reader.Skip((First & 0xc0) == 0x80 ? 1 : 3)) return false;
		}
		if (Type != 3 && !Reader.Skip(Size)) return false;
	}
	return false;
}

bool WoTMeshOpenExport(const FWoTMeshSource& Source, const FWoTMeshExport& Export,
	FWoTMeshReader& Out)
{
	Out = FWoTMeshReader(Source.Bytes, Export.Offset, Export.Offset + Export.Size);
	if ((Export.Flags & WoTMeshHasStack) != 0)
	{
		int32 Node = 0, StateNode = 0;
		if (!Out.ReadCompact(Node) || !Out.ReadCompact(StateNode) || !Out.Skip(12)
			|| (Node != 0 && !Out.ReadCompact(StateNode))) return false;
	}
	return Out.IsValid();
}

bool WoTMeshBeginArray(FWoTMeshReader& Reader, uint16 Version, bool bLazy,
	uint32 Maximum, int32& OutCount, TOptional<uint64>& OutEnd)
{
	OutEnd.Reset();
	if (bLazy && Version > 61)
	{
		int32 End = 0;
		if (!Reader.ReadI32(End) || End < 0) return false;
		OutEnd = static_cast<uint64>(End);
	}
	return Reader.ReadCompact(OutCount) && OutCount >= 0
		&& static_cast<uint32>(OutCount) <= Maximum;
}

bool WoTMeshEndArray(const FWoTMeshReader& Reader, const TOptional<uint64>& End)
{
	return !End.IsSet() || Reader.Tell() == End.GetValue();
}

bool WoTMeshSkipArray(FWoTMeshReader& Reader, uint16 Version, bool bLazy,
	uint32 Maximum, uint32 ElementBytes)
{
	int32 Count = 0;
	TOptional<uint64> End;
	if (!WoTMeshBeginArray(Reader, Version, bLazy, Maximum, Count, End)
		|| !Reader.Skip(static_cast<uint64>(Count) * ElementBytes)) return false;
	return WoTMeshEndArray(Reader, End);
}

struct FWoTMeshRawTriangle
{
	uint32 Indices[3] = {};
	uint8 UVs[3][2] = {};
	uint32 PolyFlags = 0;
	int32 TextureIndex = 0;
	bool bFieldsAvailable = true;
	bool bLodAttachment = false;
};

struct FWoTMeshFace { uint16 Values[4] = {}; };
struct FWoTMeshWedge { uint16 Vertex = 0; uint8 U = 0; uint8 V = 0; };
struct FWoTMeshMaterial { uint32 PolyFlags = 0; int32 TextureIndex = 0; };

struct FWoTMeshDecoded
{
	FWoTVertexMeshObjectHeader Header;
	TArray<uint32> PackedVertices;
};

bool WoTMeshFinite(float Value) { return FMath::IsFinite(Value); }
bool WoTMeshFinite(double Value) { return FMath::IsFinite(Value); }

bool WoTMeshTopologyPreflight(uint64 TriangleCount)
{
	const uint64 WorstHeader = WoTMeshCanonicalHeaderBaseBytes
		+ TriangleCount * WoTMeshCanonicalHeaderBytesPerTriangle;
	const uint64 WorstPayload = WoTMeshCanonicalPayloadBaseBytes
		+ TriangleCount * WoTMeshCanonicalPayloadBytesPerTriangle;
	const uint64 WorstWorking = TriangleCount * WoTMeshWorkingBytesPerTriangle;
	return TriangleCount <= WoTMeshMaxTriangles
		&& WorstHeader <= WoTMeshMaxHeaderBytes
		&& WorstPayload <= WoTMeshMaxSourceBytes
		&& WorstWorking <= WoTMeshMaxWorkingSetBytes;
}

int32 WoTMeshSignExtend(uint32 Value, uint32 Bits)
{
	const uint32 Sign = 1u << (Bits - 1u);
	const uint32 Mask = (1u << Bits) - 1u;
	Value &= Mask;
	return static_cast<int32>((Value ^ Sign) - Sign);
}

void WoTMeshRotationSinCos(int32 Units, double& OutSin, double& OutCos)
{
	const uint32 Wrapped = static_cast<uint32>(Units) & 0xffffu;
	switch (Wrapped)
	{
	case 0: OutSin = 0.0; OutCos = 1.0; return;
	case 16384: OutSin = 1.0; OutCos = 0.0; return;
	case 32768: OutSin = 0.0; OutCos = -1.0; return;
	case 49152: OutSin = -1.0; OutCos = 0.0; return;
	default: break;
	}
	constexpr double TwoPiOverUnits =
		6.283185307179586476925286766559 / 65536.0;
	const double Angle = static_cast<double>(Wrapped) * TwoPiOverUnits;
	OutSin = FMath::Sin(Angle);
	OutCos = FMath::Cos(Angle);
}

FVector3f WoTMeshRotateOrigin(const FVector3f& Point,
	const int32 RotationOrigin[3])
{
	double X = Point.X;
	double Y = Point.Y;
	double Z = Point.Z;
	double Sin = 0.0, Cos = 1.0;
	const int32 Pitch = RotationOrigin[0];
	const int32 Yaw = RotationOrigin[1];
	const int32 Roll = RotationOrigin[2];
	if (Roll != 0)
	{
		WoTMeshRotationSinCos(Roll, Sin, Cos);
		const double NewY = Y * Cos + Z * Sin;
		const double NewZ = -Y * Sin + Z * Cos;
		Y = NewY;
		Z = NewZ;
	}
	if (Pitch != 0)
	{
		WoTMeshRotationSinCos(Pitch, Sin, Cos);
		const double NewX = X * Cos - Z * Sin;
		const double NewZ = X * Sin + Z * Cos;
		X = NewX;
		Z = NewZ;
	}
	if (Yaw != 0)
	{
		WoTMeshRotationSinCos(Yaw, Sin, Cos);
		const double NewX = X * Cos - Y * Sin;
		const double NewY = X * Sin + Y * Cos;
		X = NewX;
		Y = NewY;
	}
	return FVector3f(static_cast<float>(X), static_cast<float>(Y),
		static_cast<float>(Z));
}

FVector3f WoTMeshPosition(uint32 Packed, const FVector3f& Origin,
	const FVector3f& Scale, const int32 RotationOrigin[3])
{
	const float X = static_cast<float>(WoTMeshSignExtend(Packed, 11));
	const float Y = static_cast<float>(WoTMeshSignExtend(Packed >> 11, 11));
	const float Z = static_cast<float>(WoTMeshSignExtend(Packed >> 22, 10));
	const FVector3f Point((X - Origin.X) * Scale.X,
		(Y - Origin.Y) * Scale.Y, (Z - Origin.Z) * Scale.Z);
	return RotationOrigin[0] != 0 || RotationOrigin[1] != 0
		|| RotationOrigin[2] != 0
		? WoTMeshRotateOrigin(Point, RotationOrigin) : Point;
}

bool WoTMeshSemanticSkin(const FWoTMeshSource& Source, int32 Reference,
	FWoTSemanticSkinId& Out)
{
	if (Reference == 0) return true;
	FString PackageName = Source.PackageName;
	FString ObjectName;
	if (Reference > 0)
	{
		if (!Source.Exports.IsValidIndex(Reference - 1)) return false;
		ObjectName = Source.Exports[Reference - 1].Name;
	}
	else
	{
		const int64 ImportIndex = -static_cast<int64>(Reference) - 1;
		if (ImportIndex < 0 || ImportIndex >= Source.Imports.Num()) return false;
		const FWoTMeshImport& Target = Source.Imports[static_cast<int32>(ImportIndex)];
		ObjectName = Target.ObjectName;
		int32 Outer = Target.Outer;
		for (uint32 Depth = 0; Outer != 0 && Depth < WoTMeshMaxOuterDepth; ++Depth)
		{
			if (Outer > 0)
			{
				if (!Source.Exports.IsValidIndex(Outer - 1)) return false;
				PackageName = Source.PackageName;
				Outer = 0;
			}
			else
			{
				const int64 OuterIndex = -static_cast<int64>(Outer) - 1;
				if (OuterIndex < 0 || OuterIndex >= Source.Imports.Num()) return false;
				const FWoTMeshImport& OuterImport =
					Source.Imports[static_cast<int32>(OuterIndex)];
				PackageName = OuterImport.ObjectName;
				Outer = OuterImport.Outer;
			}
		}
		if (Outer != 0) return false;
	}
	if (PackageName.IsEmpty() || ObjectName.IsEmpty()
		|| PackageName.Contains(TEXT("."), ESearchCase::CaseSensitive)
		|| ObjectName.Contains(TEXT("."), ESearchCase::CaseSensitive)
		|| PackageName.Contains(TEXT("__"), ESearchCase::CaseSensitive)) return false;
	Out.Package = PackageName;
	Out.Name = ObjectName;
	Out.LogicalId = PackageName + TEXT("__") + ObjectName;
	return true;
}

struct FWoTMeshRenderKey
{
	uint32 SourceVertex = 0;
	int32 U = -1;
	int32 V = -1;
	int32 TextureIndex = -1;
	uint32 PolyFlags = 0;
	bool bFieldsAvailable = false;
	bool bLodAttachment = false;
	bool operator==(const FWoTMeshRenderKey& Other) const
	{
		return SourceVertex == Other.SourceVertex && U == Other.U && V == Other.V
			&& TextureIndex == Other.TextureIndex && PolyFlags == Other.PolyFlags
			&& bFieldsAvailable == Other.bFieldsAvailable
			&& bLodAttachment == Other.bLodAttachment;
	}
};

uint32 GetTypeHash(const FWoTMeshRenderKey& Key)
{
	uint32 Hash = HashCombine(::GetTypeHash(Key.SourceVertex), ::GetTypeHash(Key.U));
	Hash = HashCombine(Hash, ::GetTypeHash(Key.V));
	Hash = HashCombine(Hash, ::GetTypeHash(Key.TextureIndex));
	Hash = HashCombine(Hash, ::GetTypeHash(Key.PolyFlags));
	Hash = HashCombine(Hash, ::GetTypeHash(Key.bFieldsAvailable));
	return HashCombine(Hash, ::GetTypeHash(Key.bLodAttachment));
}

TWoTBootstrapResult<FWoTMeshDecoded> WoTMeshDecode(
	const FWoTMeshSource& Source, const FWoTMeshExport& Export,
	FStringView SourcePath)
{
	using FResult = TWoTBootstrapResult<FWoTMeshDecoded>;
	const FString* ClassName = Source.ClassName(Export.ClassReference);
	const bool bLod = ClassName != nullptr && *ClassName == TEXT("LodMesh");
	if (ClassName == nullptr || (*ClassName != TEXT("Mesh") && !bLod))
	{
		return FResult::Error(WoTMeshError(EWoTBootstrapError::UnsupportedProtocol,
			TEXT("the selected export is not a UE1 Mesh/LodMesh"), FString(SourcePath)));
	}
	FWoTMeshReader Reader;
	if (!WoTMeshOpenExport(Source, Export, Reader)
		|| !WoTMeshSkipProperties(Reader, Source) || !Reader.Skip(41))
	{
		return FResult::Error(WoTMeshError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the vertex-mesh export header is malformed"), FString(SourcePath)));
	}

	TArray<uint32> SavedVertices;
	int32 SavedCount = 0;
	TOptional<uint64> SavedEnd;
	if (!WoTMeshBeginArray(Reader, Source.Version, true,
			static_cast<uint32>(WoTMeshMaxPackedVertices), SavedCount, SavedEnd))
	{
		return FResult::Error(WoTMeshError(EWoTBootstrapError::SizeLimit,
			TEXT("the packed vertex stream exceeds its decompression bound"),
			FString(SourcePath)));
	}
	if (static_cast<uint64>(SavedCount) * 4 > Reader.Remaining())
	{
		return FResult::Error(WoTMeshError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the packed vertex count exceeds the export bytes"), FString(SourcePath)));
	}
	SavedVertices.Reserve(SavedCount);
	for (int32 Index = 0; Index < SavedCount; ++Index)
	{
		uint32 Packed = 0;
		if (!Reader.ReadU32(Packed))
		{
			return FResult::Error(WoTMeshError(EWoTBootstrapError::CorruptInstallation,
				TEXT("the packed vertex stream is truncated"), FString(SourcePath)));
		}
		SavedVertices.Add(Packed);
	}
	if (!WoTMeshEndArray(Reader, SavedEnd))
	{
		return FResult::Error(WoTMeshError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the packed vertex lazy offset is corrupt"), FString(SourcePath)));
	}

	TArray<FWoTMeshRawTriangle> RawTriangles;
	int32 SerializedTriangleCount = 0;
	TOptional<uint64> TriangleEnd;
	if (!WoTMeshBeginArray(Reader, Source.Version, true, WoTMeshMaxSerializedTriangles,
		SerializedTriangleCount, TriangleEnd))
	{
		return FResult::Error(WoTMeshError(EWoTBootstrapError::SizeLimit,
			TEXT("the serialized triangle array exceeds its bound"), FString(SourcePath)));
	}
	if (!WoTMeshTopologyPreflight(static_cast<uint32>(SerializedTriangleCount)))
	{
		return FResult::Error(WoTMeshError(EWoTBootstrapError::SizeLimit,
			TEXT("the serialized topology exceeds canonical header/payload/working-set bounds"),
			FString(SourcePath)));
	}
	if (static_cast<uint64>(SerializedTriangleCount) * 20 > Reader.Remaining())
	{
		return FResult::Error(WoTMeshError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the serialized triangle count exceeds the export bytes"),
			FString(SourcePath)));
	}
	RawTriangles.Reserve(SerializedTriangleCount);
	for (int32 TriangleIndex = 0; TriangleIndex < SerializedTriangleCount; ++TriangleIndex)
	{
		FWoTMeshRawTriangle& Triangle = RawTriangles.AddDefaulted_GetRef();
		for (int32 Corner = 0; Corner < 3; ++Corner)
		{
			uint16 Index = 0;
			if (!Reader.ReadU16(Index))
			{
				return FResult::Error(WoTMeshError(
					EWoTBootstrapError::CorruptInstallation,
					TEXT("the serialized triangle indices are truncated"), FString(SourcePath)));
			}
			Triangle.Indices[Corner] = Index;
		}
		for (int32 Corner = 0; Corner < 3; ++Corner)
		{
			if (!Reader.ReadU8(Triangle.UVs[Corner][0])
				|| !Reader.ReadU8(Triangle.UVs[Corner][1]))
			{
				return FResult::Error(WoTMeshError(
					EWoTBootstrapError::CorruptInstallation,
					TEXT("the serialized triangle UVs are truncated"), FString(SourcePath)));
			}
		}
		if (!Reader.ReadU32(Triangle.PolyFlags)
			|| !Reader.ReadI32(Triangle.TextureIndex))
		{
			return FResult::Error(WoTMeshError(EWoTBootstrapError::CorruptInstallation,
				TEXT("the serialized triangle material is truncated"), FString(SourcePath)));
		}
	}
	if (!WoTMeshEndArray(Reader, TriangleEnd))
	{
		return FResult::Error(WoTMeshError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the triangle lazy offset is corrupt"), FString(SourcePath)));
	}

	TArray<FWoTVertexMeshSequence> Sequences;
	TSet<FString> SequenceNames;
	uint32 NotificationTotal = 0;
	int32 SequenceCount = 0;
	TOptional<uint64> UnusedEnd;
	if (!WoTMeshBeginArray(Reader, Source.Version, false, WoTMeshMaxSequences,
		SequenceCount, UnusedEnd))
	{
		return FResult::Error(WoTMeshError(EWoTBootstrapError::SizeLimit,
			TEXT("the animation sequence array exceeds its bound"), FString(SourcePath)));
	}
	struct FSequenceRange { int32 First = 0; int32 Count = 0; };
	TArray<FSequenceRange> SequenceRanges;
	Sequences.Reserve(SequenceCount);
	SequenceRanges.Reserve(SequenceCount);
	for (int32 SequenceIndex = 0; SequenceIndex < SequenceCount; ++SequenceIndex)
	{
		FWoTVertexMeshSequence& Sequence = Sequences.AddDefaulted_GetRef();
		FSequenceRange& Range = SequenceRanges.AddDefaulted_GetRef();
		if (!WoTMeshReadName(Reader, Source, Sequence.Name)
			|| !WoTMeshReadName(Reader, Source, Sequence.Group)
			|| !Reader.ReadI32(Range.First) || !Reader.ReadI32(Range.Count)
			|| Sequence.Name.IsEmpty() || Sequence.Group.IsEmpty())
		{
			return FResult::Error(WoTMeshError(EWoTBootstrapError::CorruptInstallation,
				TEXT("an animation sequence header is malformed"), FString(SourcePath)));
		}
		const FString FoldedName = WoTMeshFold(Sequence.Name);
		if (SequenceNames.Contains(FoldedName))
		{
			return FResult::Error(WoTMeshError(EWoTBootstrapError::DuplicateField,
				TEXT("overlapping animation sequences reuse one semantic name"),
				Sequence.Name));
		}
		SequenceNames.Add(FoldedName);
		int32 NotifyCount = 0;
		if (!WoTMeshBeginArray(Reader, Source.Version, false, WoTMeshMaxNotifies,
			NotifyCount, UnusedEnd)
			|| NotificationTotal > WoTMeshMaxNotifies - static_cast<uint32>(NotifyCount))
		{
			return FResult::Error(WoTMeshError(EWoTBootstrapError::SizeLimit,
				TEXT("the animation notifications exceed their bound"), FString(SourcePath)));
		}
		NotificationTotal += static_cast<uint32>(NotifyCount);
		Sequence.Notifies.Reserve(NotifyCount);
		for (int32 NotifyIndex = 0; NotifyIndex < NotifyCount; ++NotifyIndex)
		{
			FWoTVertexMeshNotify& Notify = Sequence.Notifies.AddDefaulted_GetRef();
			if (!Reader.ReadFloat(Notify.Time)
				|| !WoTMeshReadName(Reader, Source, Notify.Function)
				|| !WoTMeshFinite(Notify.Time) || Notify.Time < 0.0f || Notify.Time > 1.0f
				|| Notify.Function.IsEmpty())
			{
				return FResult::Error(WoTMeshError(EWoTBootstrapError::CorruptInstallation,
					TEXT("an animation notification is outside its normalized range"),
					Sequence.Name));
			}
		}
		if (!Reader.ReadFloat(Sequence.Rate) || !WoTMeshFinite(Sequence.Rate)
			|| Sequence.Rate <= 0.0f)
		{
			return FResult::Error(WoTMeshError(EWoTBootstrapError::CorruptInstallation,
				TEXT("an animation sequence has an invalid rate"), Sequence.Name));
		}
	}

	if (!WoTMeshSkipArray(Reader, Source.Version, true, WoTMeshMaxVertices, 8)
		|| !Reader.Skip(41)
		|| !WoTMeshSkipArray(Reader, Source.Version, true, 30000000, 4))
	{
		return FResult::Error(WoTMeshError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the mesh connectivity arrays are malformed"), FString(SourcePath)));
	}
	TArray<int32> TextureReferences;
	int32 TextureCount = 0;
	if (!WoTMeshBeginArray(Reader, Source.Version, false, WoTMeshMaxTableItems,
		TextureCount, UnusedEnd))
	{
		return FResult::Error(WoTMeshError(EWoTBootstrapError::SizeLimit,
			TEXT("the mesh skin array exceeds its bound"), FString(SourcePath)));
	}
	TextureReferences.Reserve(TextureCount);
	for (int32 TextureIndex = 0; TextureIndex < TextureCount; ++TextureIndex)
	{
		int32 Reference = 0;
		if (!Reader.ReadCompact(Reference))
		{
			return FResult::Error(WoTMeshError(EWoTBootstrapError::CorruptInstallation,
				TEXT("the mesh skin references are truncated"), FString(SourcePath)));
		}
		TextureReferences.Add(Reference);
	}
	if (!WoTMeshSkipArray(Reader, Source.Version, false, WoTMeshMaxFrames, 25)
		|| !WoTMeshSkipArray(Reader, Source.Version, false, WoTMeshMaxFrames, 16))
	{
		return FResult::Error(WoTMeshError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the per-frame serialized bounds are malformed"), FString(SourcePath)));
	}

	int32 VertexCountSigned = 0, FrameCountSigned = 0;
	uint32 AndFlags = 0, OrFlags = 0;
	FVector3f Scale, Origin;
	int32 RotationOrigin[3] = {};
	int32 CurrentPoly = 0, CurrentVertex = 0;
	if (!Reader.ReadI32(VertexCountSigned) || !Reader.ReadI32(FrameCountSigned)
		|| !Reader.ReadU32(AndFlags) || !Reader.ReadU32(OrFlags)
		|| !Reader.ReadFloat(Scale.X) || !Reader.ReadFloat(Scale.Y)
		|| !Reader.ReadFloat(Scale.Z) || !Reader.ReadFloat(Origin.X)
		|| !Reader.ReadFloat(Origin.Y) || !Reader.ReadFloat(Origin.Z)
		|| !Reader.ReadI32(RotationOrigin[0]) || !Reader.ReadI32(RotationOrigin[1])
		|| !Reader.ReadI32(RotationOrigin[2]) || !Reader.ReadI32(CurrentPoly)
		|| !Reader.ReadI32(CurrentVertex)
		|| VertexCountSigned <= 0 || FrameCountSigned <= 0
		|| VertexCountSigned > static_cast<int32>(WoTMeshMaxVertices)
		|| FrameCountSigned > static_cast<int32>(WoTMeshMaxFrames)
		|| !WoTMeshFinite(Scale.X) || !WoTMeshFinite(Scale.Y) || !WoTMeshFinite(Scale.Z)
		|| !WoTMeshFinite(Origin.X) || !WoTMeshFinite(Origin.Y) || !WoTMeshFinite(Origin.Z))
	{
		return FResult::Error(WoTMeshError(EWoTBootstrapError::SizeLimit,
			TEXT("the mesh dimensions or origin/scale are invalid"), FString(SourcePath)));
	}
	if (Source.Version == 65)
	{
		float UnusedFloat = 0.0f;
		if (!Reader.ReadFloat(UnusedFloat))
		{
			return FResult::Error(WoTMeshError(EWoTBootstrapError::CorruptInstallation,
				TEXT("the version-65 mesh tail is truncated"), FString(SourcePath)));
		}
	}
	if (Source.Version >= 66
		&& !WoTMeshSkipArray(Reader, Source.Version, false, WoTMeshMaxTableItems, 4))
	{
		return FResult::Error(WoTMeshError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the mesh texture-LOD array is malformed"), FString(SourcePath)));
	}

	TArray<FWoTMeshFace> Faces;
	TArray<FWoTMeshWedge> Wedges;
	TArray<FWoTMeshMaterial> Materials;
	TArray<FWoTMeshFace> SpecialFaces;
	TArray<uint16> Remap;
	int32 ModelVertices = 0, SpecialVertices = 0, OldFrameVertices = 0;
	if (bLod)
	{
		if (!WoTMeshSkipArray(Reader, Source.Version, false, WoTMeshMaxVertices, 2)
			|| !WoTMeshSkipArray(Reader, Source.Version, false,
				WoTMeshMaxSerializedTriangles, 2))
		{
			return FResult::Error(WoTMeshError(EWoTBootstrapError::CorruptInstallation,
				TEXT("the LodMesh collapse arrays are malformed"), FString(SourcePath)));
		}
		int32 FaceCount = 0;
		if (!WoTMeshBeginArray(Reader, Source.Version, false,
			WoTMeshMaxSerializedTriangles, FaceCount, UnusedEnd))
		{
			return FResult::Error(WoTMeshError(EWoTBootstrapError::SizeLimit,
				TEXT("the LodMesh face array exceeds its bound"), FString(SourcePath)));
		}
		if (!WoTMeshTopologyPreflight(static_cast<uint32>(FaceCount)))
		{
			return FResult::Error(WoTMeshError(EWoTBootstrapError::SizeLimit,
				TEXT("the LodMesh topology exceeds canonical header/payload/working-set bounds"),
				FString(SourcePath)));
		}
		if (static_cast<uint64>(FaceCount) * 8 > Reader.Remaining())
		{
			return FResult::Error(WoTMeshError(EWoTBootstrapError::CorruptInstallation,
				TEXT("the LodMesh face count exceeds the export bytes"), FString(SourcePath)));
		}
		Faces.SetNumUninitialized(FaceCount);
		for (FWoTMeshFace& Face : Faces)
		{
			for (uint16& Value : Face.Values) if (!Reader.ReadU16(Value))
			{
				return FResult::Error(WoTMeshError(EWoTBootstrapError::CorruptInstallation,
					TEXT("the LodMesh face array is truncated"), FString(SourcePath)));
			}
		}
		if (!WoTMeshSkipArray(Reader, Source.Version, false,
			WoTMeshMaxTriangles * 3, 2))
		{
			return FResult::Error(WoTMeshError(EWoTBootstrapError::CorruptInstallation,
				TEXT("the LodMesh wedge-collapse array is malformed"), FString(SourcePath)));
		}
		int32 WedgeCount = 0;
		if (!WoTMeshBeginArray(Reader, Source.Version, false,
			WoTMeshMaxTriangles * 3, WedgeCount, UnusedEnd))
		{
			return FResult::Error(WoTMeshError(EWoTBootstrapError::SizeLimit,
				TEXT("the LodMesh wedge array exceeds its bound"), FString(SourcePath)));
		}
		if (static_cast<uint64>(WedgeCount) * 4 > Reader.Remaining())
		{
			return FResult::Error(WoTMeshError(EWoTBootstrapError::CorruptInstallation,
				TEXT("the LodMesh wedge count exceeds the export bytes"), FString(SourcePath)));
		}
		Wedges.SetNumUninitialized(WedgeCount);
		for (FWoTMeshWedge& Wedge : Wedges)
		{
			if (!Reader.ReadU16(Wedge.Vertex) || !Reader.ReadU8(Wedge.U)
				|| !Reader.ReadU8(Wedge.V))
			{
				return FResult::Error(WoTMeshError(EWoTBootstrapError::CorruptInstallation,
					TEXT("the LodMesh wedge array is truncated"), FString(SourcePath)));
			}
		}
		int32 MaterialCount = 0;
		if (!WoTMeshBeginArray(Reader, Source.Version, false, WoTMeshMaxTableItems,
			MaterialCount, UnusedEnd))
		{
			return FResult::Error(WoTMeshError(EWoTBootstrapError::SizeLimit,
				TEXT("the LodMesh material array exceeds its bound"), FString(SourcePath)));
		}
		if (static_cast<uint64>(MaterialCount) * 8 > Reader.Remaining())
		{
			return FResult::Error(WoTMeshError(EWoTBootstrapError::CorruptInstallation,
				TEXT("the LodMesh material count exceeds the export bytes"), FString(SourcePath)));
		}
		Materials.SetNumUninitialized(MaterialCount);
		for (FWoTMeshMaterial& Material : Materials)
		{
			if (!Reader.ReadU32(Material.PolyFlags)
				|| !Reader.ReadI32(Material.TextureIndex))
			{
				return FResult::Error(WoTMeshError(EWoTBootstrapError::CorruptInstallation,
					TEXT("the LodMesh material array is truncated"), FString(SourcePath)));
			}
		}
		int32 SpecialFaceCount = 0;
		if (!WoTMeshBeginArray(Reader, Source.Version, false, WoTMeshMaxTriangles,
			SpecialFaceCount, UnusedEnd))
		{
			return FResult::Error(WoTMeshError(EWoTBootstrapError::SizeLimit,
				TEXT("the LodMesh attachment face array exceeds its bound"),
				FString(SourcePath)));
		}
		if (!WoTMeshTopologyPreflight(
			static_cast<uint64>(FaceCount) + static_cast<uint32>(SpecialFaceCount)))
		{
			return FResult::Error(WoTMeshError(EWoTBootstrapError::SizeLimit,
				TEXT("the combined LodMesh topology exceeds canonical ")
					TEXT("header/payload/working-set bounds"),
				FString(SourcePath)));
		}
		if (static_cast<uint64>(SpecialFaceCount) * 8 > Reader.Remaining())
		{
			return FResult::Error(WoTMeshError(EWoTBootstrapError::CorruptInstallation,
				TEXT("the LodMesh attachment count exceeds the export bytes"),
				FString(SourcePath)));
		}
		SpecialFaces.SetNumUninitialized(SpecialFaceCount);
		for (FWoTMeshFace& Face : SpecialFaces)
		{
			for (uint16& Value : Face.Values) if (!Reader.ReadU16(Value))
			{
				return FResult::Error(WoTMeshError(EWoTBootstrapError::CorruptInstallation,
					TEXT("the LodMesh attachment faces are truncated"), FString(SourcePath)));
			}
		}
		float MeshScaleMax = 0.0f, Hysteresis = 0.0f, Strength = 0.0f;
		int32 MinVertices = 0;
		float Morph = 0.0f, ZDisplace = 0.0f;
		if (!Reader.ReadI32(ModelVertices) || !Reader.ReadI32(SpecialVertices)
			|| !Reader.ReadFloat(MeshScaleMax) || !Reader.ReadFloat(Hysteresis)
			|| !Reader.ReadFloat(Strength) || !Reader.ReadI32(MinVertices)
			|| !Reader.ReadFloat(Morph) || !Reader.ReadFloat(ZDisplace))
		{
			return FResult::Error(WoTMeshError(EWoTBootstrapError::CorruptInstallation,
				TEXT("the LodMesh scalar tail is truncated"), FString(SourcePath)));
		}
		int32 RemapCount = 0;
		if (!WoTMeshBeginArray(Reader, Source.Version, false, WoTMeshMaxVertices,
			RemapCount, UnusedEnd))
		{
			return FResult::Error(WoTMeshError(EWoTBootstrapError::SizeLimit,
				TEXT("the LodMesh animation remap exceeds its bound"), FString(SourcePath)));
		}
		if (static_cast<uint64>(RemapCount) * 2 > Reader.Remaining())
		{
			return FResult::Error(WoTMeshError(EWoTBootstrapError::CorruptInstallation,
				TEXT("the LodMesh animation remap count exceeds the export bytes"),
				FString(SourcePath)));
		}
		Remap.SetNumUninitialized(RemapCount);
		for (uint16& Value : Remap) if (!Reader.ReadU16(Value))
		{
			return FResult::Error(WoTMeshError(EWoTBootstrapError::CorruptInstallation,
				TEXT("the LodMesh animation remap is truncated"), FString(SourcePath)));
		}
		if (!Reader.ReadI32(OldFrameVertices))
		{
			return FResult::Error(WoTMeshError(EWoTBootstrapError::CorruptInstallation,
				TEXT("the LodMesh old frame size is absent"), FString(SourcePath)));
		}
	}
	if (Reader.Tell() != Reader.GetEnd())
	{
		return FResult::Error(WoTMeshError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the vertex-mesh export has trailing serialized bytes"), FString(SourcePath)));
	}

	const uint32 VertexCount = static_cast<uint32>(VertexCountSigned);
	const uint32 FrameCount = static_cast<uint32>(FrameCountSigned);
	if (static_cast<uint64>(VertexCount) > WoTMeshMaxPackedVertices / FrameCount)
	{
		return FResult::Error(WoTMeshError(EWoTBootstrapError::SizeLimit,
			TEXT("the decoded frame product overflows its decompression limit"),
			FString(SourcePath)));
	}
	const uint64 ExpectedPacked = static_cast<uint64>(VertexCount) * FrameCount;
	TArray<uint32> PackedVertices;
	if (bLod && !Remap.IsEmpty())
	{
		if (Remap.Num() != VertexCountSigned || OldFrameVertices <= 0
			|| static_cast<uint64>(OldFrameVertices) > WoTMeshMaxPackedVertices / FrameCount
			|| static_cast<uint64>(SavedVertices.Num())
				!= static_cast<uint64>(OldFrameVertices) * FrameCount)
		{
			return FResult::Error(WoTMeshError(EWoTBootstrapError::CorruptInstallation,
				TEXT("the LodMesh animation remap dimensions disagree"), FString(SourcePath)));
		}
		PackedVertices.Reserve(static_cast<int32>(ExpectedPacked));
		for (uint32 Frame = 0; Frame < FrameCount; ++Frame)
		{
			for (uint32 Vertex = 0; Vertex < VertexCount; ++Vertex)
			{
				if (Remap[static_cast<int32>(Vertex)] >= OldFrameVertices)
				{
					return FResult::Error(WoTMeshError(
						EWoTBootstrapError::CorruptInstallation,
						TEXT("the LodMesh animation remap index is out of range"),
						FString(SourcePath)));
				}
				PackedVertices.Add(SavedVertices[
					static_cast<int32>(Frame * static_cast<uint32>(OldFrameVertices)
						+ Remap[static_cast<int32>(Vertex)])]);
			}
		}
	}
	else
	{
		PackedVertices = MoveTemp(SavedVertices);
	}
	if (static_cast<uint64>(PackedVertices.Num()) != ExpectedPacked)
	{
		return FResult::Error(WoTMeshError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the packed vertex count disagrees with FrameVerts*AnimFrames"),
			FString(SourcePath)));
	}

	if (bLod)
	{
		if (Faces.Num() > static_cast<int32>(WoTMeshMaxTriangles)
			- SpecialFaces.Num())
		{
			return FResult::Error(WoTMeshError(EWoTBootstrapError::SizeLimit,
				TEXT("the combined LodMesh topology exceeds its bound"), FString(SourcePath)));
		}
		RawTriangles.Reset(Faces.Num() + SpecialFaces.Num());
		for (int32 FaceIndex = 0; FaceIndex < Faces.Num(); ++FaceIndex)
		{
			const FWoTMeshFace& Face = Faces[FaceIndex];
			if (!Materials.IsValidIndex(Face.Values[3]))
			{
				return FResult::Error(WoTMeshError(EWoTBootstrapError::CorruptInstallation,
					TEXT("a LodMesh face material is out of range"), FString(SourcePath)));
			}
			const FWoTMeshMaterial& Material = Materials[Face.Values[3]];
			FWoTMeshRawTriangle& Triangle = RawTriangles.AddDefaulted_GetRef();
			Triangle.PolyFlags = Material.PolyFlags;
			Triangle.TextureIndex = Material.TextureIndex;
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				if (!Wedges.IsValidIndex(Face.Values[Corner]))
				{
					return FResult::Error(WoTMeshError(
						EWoTBootstrapError::CorruptInstallation,
						TEXT("a LodMesh face wedge is out of range"), FString(SourcePath)));
				}
				const FWoTMeshWedge& Wedge = Wedges[Face.Values[Corner]];
				Triangle.Indices[Corner] = static_cast<uint32>(Wedge.Vertex)
					+ static_cast<uint32>(SpecialVertices);
				Triangle.UVs[Corner][0] = Wedge.U;
				Triangle.UVs[Corner][1] = Wedge.V;
			}
		}
		for (const FWoTMeshFace& Face : SpecialFaces)
		{
			FWoTMeshRawTriangle& Triangle = RawTriangles.AddDefaulted_GetRef();
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				Triangle.Indices[Corner] = Face.Values[Corner];
			}
			Triangle.bFieldsAvailable = false;
			Triangle.bLodAttachment = true;
		}
	}
	if (RawTriangles.IsEmpty()
		|| RawTriangles.Num() > static_cast<int32>(WoTMeshMaxTriangles))
	{
		return FResult::Error(WoTMeshError(EWoTBootstrapError::SizeLimit,
			TEXT("the runtime triangle array is empty or oversized"), FString(SourcePath)));
	}

	FWoTMeshDecoded Decoded;
	FWoTVertexMeshObjectHeader& Header = Decoded.Header;
	Header.VertexCount = VertexCount;
	Header.TriangleCount = static_cast<uint32>(RawTriangles.Num());
	Header.FrameCount = FrameCount;
	Header.Origin = Origin;
	Header.Scale = Scale;
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		Header.RotationOrigin[Axis] = RotationOrigin[Axis];
	}
	Header.ClassName = *ClassName;
	Header.Sequences = MoveTemp(Sequences);
	for (int32 SequenceIndex = 0; SequenceIndex < Header.Sequences.Num(); ++SequenceIndex)
	{
		const FSequenceRange& Range = SequenceRanges[SequenceIndex];
		if (Range.First < 0 || Range.Count <= 0
			|| static_cast<uint32>(Range.First) >= FrameCount
			|| static_cast<uint32>(Range.Count)
				> FrameCount + 1u - static_cast<uint32>(Range.First))
		{
			return FResult::Error(WoTMeshError(EWoTBootstrapError::CorruptInstallation,
				TEXT("an animation sequence range is outside the frame stream"),
				Header.Sequences[SequenceIndex].Name));
		}
		Header.Sequences[SequenceIndex].FirstFrame = static_cast<uint32>(Range.First);
		Header.Sequences[SequenceIndex].NumFrames = static_cast<uint32>(Range.Count);
	}
	Header.Skins.Reserve(TextureReferences.Num());
	for (int32 SkinIndex = 0; SkinIndex < TextureReferences.Num(); ++SkinIndex)
	{
		FWoTSemanticSkinId& Skin = Header.Skins.AddDefaulted_GetRef();
		if (!WoTMeshSemanticSkin(Source, TextureReferences[SkinIndex], Skin))
		{
			return FResult::Error(WoTMeshError(EWoTBootstrapError::CorruptInstallation,
				TEXT("a mesh skin has an invalid semantic object identity"),
				FString::Printf(TEXT("skins[%d]"), SkinIndex)));
		}
	}
	if (bLod)
	{
		if (ModelVertices < 0 || SpecialVertices < 0 || OldFrameVertices < 0
			|| SpecialVertices > VertexCountSigned)
		{
			return FResult::Error(WoTMeshError(EWoTBootstrapError::CorruptInstallation,
				TEXT("the LodMesh dimensions are invalid"), FString(SourcePath)));
		}
		Header.LodModelVertices = static_cast<uint32>(ModelVertices);
		Header.LodSpecialVertices = static_cast<uint32>(SpecialVertices);
		Header.LodOldFrameVertices = static_cast<uint32>(OldFrameVertices);
		Header.bLodRemapPresent = !Remap.IsEmpty();
	}

	TMap<FWoTMeshRenderKey, uint32> RenderLookup;
	Header.Triangles.Reserve(RawTriangles.Num());
	for (int32 TriangleIndex = 0; TriangleIndex < RawTriangles.Num(); ++TriangleIndex)
	{
		const FWoTMeshRawTriangle& Raw = RawTriangles[TriangleIndex];
		for (uint32 Index : Raw.Indices)
		{
			if (Index >= VertexCount)
			{
				return FResult::Error(WoTMeshError(EWoTBootstrapError::CorruptInstallation,
					TEXT("a mesh triangle vertex is out of range"),
					FString::Printf(TEXT("triangles[%d]"), TriangleIndex)));
			}
		}
		if (Raw.bFieldsAvailable
			&& ((Raw.PolyFlags & ~WoTMeshSupportedPolyFlags) != 0
				|| Raw.TextureIndex < 0 || Raw.TextureIndex >= Header.Skins.Num()))
		{
			return FResult::Error(WoTMeshError(EWoTBootstrapError::UnsupportedProtocol,
				TEXT("a mesh triangle material/flag is outside the supported set"),
				FString::Printf(TEXT("triangles[%d]"), TriangleIndex)));
		}

		bool bNewSection = Header.Sections.IsEmpty();
		if (!bNewSection)
		{
			const FWoTVertexMeshSection& Previous = Header.Sections.Last();
			bNewSection = Previous.bLodAttachment != Raw.bLodAttachment
				|| Previous.PolyFlags.IsSet() != Raw.bFieldsAvailable
				|| Previous.SkinIndex.IsSet() != Raw.bFieldsAvailable
				|| (Raw.bFieldsAvailable
					&& (Previous.PolyFlags.GetValue() != Raw.PolyFlags
						|| Previous.SkinIndex.GetValue()
							!= static_cast<uint32>(Raw.TextureIndex)));
		}
		if (bNewSection)
		{
			FWoTVertexMeshSection& Section = Header.Sections.AddDefaulted_GetRef();
			Section.FirstTriangle = static_cast<uint32>(TriangleIndex);
			Section.bLodAttachment = Raw.bLodAttachment;
			if (Raw.bFieldsAvailable)
			{
				Section.PolyFlags = Raw.PolyFlags;
				Section.SkinIndex = static_cast<uint32>(Raw.TextureIndex);
			}
		}
		Header.Sections.Last().TriangleCount++;
		FWoTVertexMeshTriangle& Triangle = Header.Triangles.AddDefaulted_GetRef();
		Triangle.SectionIndex = static_cast<uint32>(Header.Sections.Num() - 1);
		Triangle.bLodAttachment = Raw.bLodAttachment;
		if (Raw.bFieldsAvailable)
		{
			Triangle.PolyFlags = Raw.PolyFlags;
			Triangle.TextureIndex = Raw.TextureIndex;
		}
		for (int32 Corner = 0; Corner < 3; ++Corner)
		{
			Triangle.VertexIndices[Corner] = Raw.Indices[Corner];
			int32 U = -1, V = -1;
			if (Raw.bFieldsAvailable)
			{
				FWoTVertexMeshUV& UV = Header.UVs.AddDefaulted_GetRef();
				UV.U = Raw.UVs[Corner][0];
				UV.V = Raw.UVs[Corner][1];
				Triangle.UVIndices[Corner] = static_cast<uint32>(Header.UVs.Num() - 1);
				U = UV.U;
				V = UV.V;
			}
			FWoTMeshRenderKey Key;
			Key.SourceVertex = Raw.Indices[Corner];
			Key.U = U;
			Key.V = V;
			Key.TextureIndex = Raw.bFieldsAvailable ? Raw.TextureIndex : -1;
			Key.PolyFlags = Raw.bFieldsAvailable ? Raw.PolyFlags : 0;
			Key.bFieldsAvailable = Raw.bFieldsAvailable;
			Key.bLodAttachment = Raw.bLodAttachment;
			uint32* Existing = RenderLookup.Find(Key);
			if (Existing == nullptr)
			{
				const uint32 NewIndex = static_cast<uint32>(Header.RenderVertices.Num());
				RenderLookup.Add(Key, NewIndex);
				FWoTVertexMeshRenderVertex& Render =
					Header.RenderVertices.AddDefaulted_GetRef();
				Render.SourceVertex = Raw.Indices[Corner];
				if (Raw.bFieldsAvailable)
				{
					Render.UVIndex = Triangle.UVIndices[Corner].GetValue();
					Render.SkinIndex = static_cast<uint32>(Raw.TextureIndex);
				}
				Triangle.RenderVertexIndices[Corner] = NewIndex;
			}
			else
			{
				Triangle.RenderVertexIndices[Corner] = *Existing;
			}
		}
		if (Raw.bFieldsAvailable && (Raw.PolyFlags & WoTMeshInvisible) == 0)
		{
			Header.CollisionTriangles.Add(static_cast<uint32>(TriangleIndex));
		}
	}

	Header.Bounds.Min = FVector3f(MAX_flt, MAX_flt, MAX_flt);
	Header.Bounds.Max = FVector3f(-MAX_flt, -MAX_flt, -MAX_flt);
	for (uint32 Packed : PackedVertices)
	{
		const FVector3f Point = WoTMeshPosition(Packed, Origin, Scale, RotationOrigin);
		if (!WoTMeshFinite(Point.X) || !WoTMeshFinite(Point.Y)
			|| !WoTMeshFinite(Point.Z))
		{
			return FResult::Error(WoTMeshError(EWoTBootstrapError::InvalidNumber,
				TEXT("a transformed mesh vertex is not finite"), FString(SourcePath)));
		}
		Header.Bounds.Min.X = FMath::Min(Header.Bounds.Min.X, Point.X);
		Header.Bounds.Min.Y = FMath::Min(Header.Bounds.Min.Y, Point.Y);
		Header.Bounds.Min.Z = FMath::Min(Header.Bounds.Min.Z, Point.Z);
		Header.Bounds.Max.X = FMath::Max(Header.Bounds.Max.X, Point.X);
		Header.Bounds.Max.Y = FMath::Max(Header.Bounds.Max.Y, Point.Y);
		Header.Bounds.Max.Z = FMath::Max(Header.Bounds.Max.Z, Point.Z);
	}
	if (!WoTMeshFinite(Header.Bounds.Min.X) || !WoTMeshFinite(Header.Bounds.Min.Y)
		|| !WoTMeshFinite(Header.Bounds.Min.Z) || !WoTMeshFinite(Header.Bounds.Max.X)
		|| !WoTMeshFinite(Header.Bounds.Max.Y) || !WoTMeshFinite(Header.Bounds.Max.Z))
	{
		return FResult::Error(WoTMeshError(EWoTBootstrapError::InvalidNumber,
			TEXT("the transformed mesh bounds are not finite"), FString(SourcePath)));
	}

	auto BasePosition = [&PackedVertices, &Origin, &Scale, &RotationOrigin](
		uint32 Vertex) -> FVector3d
	{
		const FVector3f Point = WoTMeshPosition(
			PackedVertices[static_cast<int32>(Vertex)], Origin, Scale, RotationOrigin);
		return FVector3d(Point.X, Point.Y, Point.Z);
	};

	// Match mesh2glb.smooth_normals exactly: every face, including LodMesh
	// attachment/placeholder faces, is normalized before one contribution per
	// corner. Tangents then consume the surviving corner UVs and are
	// Gram-Schmidt orthogonalized against that source-vertex normal.
	TArray<FVector3d> SourceNormals;
	SourceNormals.SetNumZeroed(VertexCountSigned);
	for (const FWoTVertexMeshTriangle& Triangle : Header.Triangles)
	{
		const FVector3d A = BasePosition(Triangle.VertexIndices[0]);
		const FVector3d B = BasePosition(Triangle.VertexIndices[1]);
		const FVector3d C = BasePosition(Triangle.VertexIndices[2]);
		const FVector3d Cross = FVector3d::CrossProduct(B - A, C - A);
		const double Length = FMath::Sqrt(Cross.SizeSquared() + 0.001);
		const FVector3d FaceNormal = Cross / Length;
		if (!WoTMeshFinite(FaceNormal.X) || !WoTMeshFinite(FaceNormal.Y)
			|| !WoTMeshFinite(FaceNormal.Z))
		{
			return FResult::Error(WoTMeshError(EWoTBootstrapError::InvalidNumber,
				TEXT("a mesh face normal is not finite"), FString(SourcePath)));
		}
		for (uint32 Vertex : Triangle.VertexIndices)
		{
			SourceNormals[static_cast<int32>(Vertex)] += FaceNormal;
		}
	}
	for (FVector3d& Normal : SourceNormals)
	{
		const double Length = FMath::Sqrt(Normal.SizeSquared());
		if (Length > 0.0) Normal /= Length;
		if (!WoTMeshFinite(Normal.X) || !WoTMeshFinite(Normal.Y)
			|| !WoTMeshFinite(Normal.Z))
		{
			return FResult::Error(WoTMeshError(EWoTBootstrapError::InvalidNumber,
				TEXT("a mesh source-vertex normal is not finite"), FString(SourcePath)));
		}
	}

	TArray<FVector3d> TangentSums;
	TArray<FVector3d> BitangentSums;
	TangentSums.SetNumZeroed(Header.RenderVertices.Num());
	BitangentSums.SetNumZeroed(Header.RenderVertices.Num());
	for (const FWoTVertexMeshTriangle& Triangle : Header.Triangles)
	{
		if (!Triangle.UVIndices[0].IsSet()) continue;
		const FVector3d A = BasePosition(Triangle.VertexIndices[0]);
		const FVector3d B = BasePosition(Triangle.VertexIndices[1]);
		const FVector3d C = BasePosition(Triangle.VertexIndices[2]);
		const FWoTVertexMeshUV& UVA =
			Header.UVs[static_cast<int32>(Triangle.UVIndices[0].GetValue())];
		const FWoTVertexMeshUV& UVB =
			Header.UVs[static_cast<int32>(Triangle.UVIndices[1].GetValue())];
		const FWoTVertexMeshUV& UVC =
			Header.UVs[static_cast<int32>(Triangle.UVIndices[2].GetValue())];
		const double DU1 = (static_cast<double>(UVB.U) - UVA.U) / 255.0;
		const double DV1 = (static_cast<double>(UVB.V) - UVA.V) / 255.0;
		const double DU2 = (static_cast<double>(UVC.U) - UVA.U) / 255.0;
		const double DV2 = (static_cast<double>(UVC.V) - UVA.V) / 255.0;
		const double Determinant = DU1 * DV2 - DU2 * DV1;
		if (Determinant == 0.0) continue;
		const double Reciprocal = 1.0 / Determinant;
		const FVector3d Edge1 = B - A;
		const FVector3d Edge2 = C - A;
		const FVector3d Tangent = (Edge1 * DV2 - Edge2 * DV1) * Reciprocal;
		const FVector3d Bitangent = (Edge2 * DU1 - Edge1 * DU2) * Reciprocal;
		if (!WoTMeshFinite(Tangent.X) || !WoTMeshFinite(Tangent.Y)
			|| !WoTMeshFinite(Tangent.Z) || !WoTMeshFinite(Bitangent.X)
			|| !WoTMeshFinite(Bitangent.Y) || !WoTMeshFinite(Bitangent.Z))
		{
			return FResult::Error(WoTMeshError(EWoTBootstrapError::InvalidNumber,
				TEXT("a mesh corner-UV tangent is not finite"), FString(SourcePath)));
		}
		for (uint32 RenderIndex : Triangle.RenderVertexIndices)
		{
			TangentSums[static_cast<int32>(RenderIndex)] += Tangent;
			BitangentSums[static_cast<int32>(RenderIndex)] += Bitangent;
		}
	}
	for (int32 RenderIndex = 0; RenderIndex < Header.RenderVertices.Num(); ++RenderIndex)
	{
		const FWoTVertexMeshRenderVertex& Render = Header.RenderVertices[RenderIndex];
		const FVector3d& Normal = SourceNormals[static_cast<int32>(Render.SourceVertex)];
		FVector3d Tangent = TangentSums[RenderIndex]
			- Normal * FVector3d::DotProduct(Normal, TangentSums[RenderIndex]);
		double Length = FMath::Sqrt(Tangent.SizeSquared());
		if (Length == 0.0)
		{
			if (Normal.SizeSquared() == 0.0)
			{
				Tangent = FVector3d(1.0, 0.0, 0.0);
			}
			else
			{
				const FVector3d Axis = FMath::Abs(Normal.X) < 0.9
					? FVector3d(1.0, 0.0, 0.0) : FVector3d(0.0, 1.0, 0.0);
				Tangent = FVector3d::CrossProduct(Axis, Normal);
				Length = FMath::Sqrt(Tangent.SizeSquared());
				Tangent /= Length;
			}
		}
		else
		{
			Tangent /= Length;
		}
		const double Handedness =
			FVector3d::DotProduct(FVector3d::CrossProduct(Normal, Tangent),
				BitangentSums[RenderIndex]) < 0.0 ? -1.0 : 1.0;
		if (!WoTMeshFinite(Tangent.X) || !WoTMeshFinite(Tangent.Y)
			|| !WoTMeshFinite(Tangent.Z) || !WoTMeshFinite(Handedness))
		{
			return FResult::Error(WoTMeshError(EWoTBootstrapError::InvalidNumber,
				TEXT("a derived mesh tangent basis is not finite"), FString(SourcePath)));
		}
	}
	Decoded.PackedVertices = MoveTemp(PackedVertices);
	return FResult::Success(MoveTemp(Decoded));
}

void WoTMeshAppendU16(TArray<uint8>& Bytes, uint16 Value)
{
	Bytes.Add(static_cast<uint8>(Value));
	Bytes.Add(static_cast<uint8>(Value >> 8));
}

void WoTMeshAppendU32(TArray<uint8>& Bytes, uint32 Value)
{
	Bytes.Add(static_cast<uint8>(Value));
	Bytes.Add(static_cast<uint8>(Value >> 8));
	Bytes.Add(static_cast<uint8>(Value >> 16));
	Bytes.Add(static_cast<uint8>(Value >> 24));
}

void WoTMeshAppendI32(TArray<uint8>& Bytes, int32 Value)
{
	WoTMeshAppendU32(Bytes, static_cast<uint32>(Value));
}

uint32 WoTMeshFloatBits(float Value)
{
	uint32 Bits = 0;
	FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
	// Canonicalize negative zero. IEEE-754 has two spellings of zero and
	// -0.0 == +0.0, so which one a bound ends up holding depends purely on
	// the arithmetic path: this encoder produced +0.0 for a bound where the
	// Python encoder produced -0.0, and byte-exact parity failed on a value
	// that is not numerically different. The canonical JSON grammar already
	// refuses "-0" for integers for the same reason; this is that rule
	// applied to the float-bit encoding.
	if (Bits == 0x80000000u)
	{
		Bits = 0u;
	}
	return Bits;
}

FWoTCanonicalJsonValue WoTMeshUIntArray3(const uint32 Values[3])
{
	FWoTCanonicalJsonValue Result = FWoTCanonicalJsonValue::MakeArray();
	for (int32 Index = 0; Index < 3; ++Index)
	{
		Result.AddElement(FWoTCanonicalJsonValue::MakeInteger(Values[Index]));
	}
	return Result;
}

FWoTCanonicalJsonValue WoTMeshIntArray3(const int32 Values[3])
{
	FWoTCanonicalJsonValue Result = FWoTCanonicalJsonValue::MakeArray();
	for (int32 Index = 0; Index < 3; ++Index)
	{
		Result.AddElement(FWoTCanonicalJsonValue::MakeInteger(Values[Index]));
	}
	return Result;
}

FWoTCanonicalJsonValue WoTMeshFloatBitsArray3(const FVector3f& Value)
{
	const uint32 Values[3] = {
		WoTMeshFloatBits(Value.X), WoTMeshFloatBits(Value.Y), WoTMeshFloatBits(Value.Z)};
	return WoTMeshUIntArray3(Values);
}

FWoTCanonicalJsonValue WoTMeshOptionalUnsigned(const TOptional<uint32>& Value)
{
	return Value.IsSet() ? FWoTCanonicalJsonValue::MakeInteger(Value.GetValue())
		: FWoTCanonicalJsonValue::MakeNull();
}

FWoTCanonicalJsonValue WoTMeshOptionalSigned(const TOptional<int32>& Value)
{
	return Value.IsSet() ? FWoTCanonicalJsonValue::MakeInteger(Value.GetValue())
		: FWoTCanonicalJsonValue::MakeNull();
}

FWoTCanonicalJsonValue WoTMeshOptionalString(const TOptional<FString>& Value)
{
	return Value.IsSet() ? FWoTCanonicalJsonValue::MakeString(Value.GetValue())
		: FWoTCanonicalJsonValue::MakeNull();
}

TWoTBootstrapResult<FWoTCanonicalJsonValue> WoTMeshSourceJson(
	const FWoTRequiredFact& Fact, const FWoTMeshSource& Source)
{
	FWoTCanonicalObjectBuilder Builder;
	Builder.Add(TEXT("export"), FWoTCanonicalJsonValue::MakeString(Fact.SourceExport));
	Builder.Add(TEXT("package"), FWoTCanonicalJsonValue::MakeString(Fact.SourcePath));
	Builder.Add(TEXT("sha256"), FWoTCanonicalJsonValue::MakeString(Source.Sha256.ToHex()));
	return MoveTemp(Builder).Build(TEXT("mesh source"));
}

TWoTBootstrapResult<TArray<uint8>> WoTMeshFrameObject(
	const FWoTCanonicalJsonValue& Header, TConstArrayView<uint8> Payload)
{
	using FResult = TWoTBootstrapResult<TArray<uint8>>;
	TWoTBootstrapResult<TArray<uint8>> HeaderBytes = WriteCanonicalJson(Header);
	if (HeaderBytes.IsError()) return FResult::Error(MoveTemp(HeaderBytes).GetError());
	if (HeaderBytes.GetValue().IsEmpty()
		|| HeaderBytes.GetValue().Num() > static_cast<int32>(WoTMeshMaxHeaderBytes)
		|| static_cast<uint64>(Payload.Num()) > WoTMeshMaxSourceBytes
		|| HeaderBytes.GetValue().Num() > MAX_int32 - Payload.Num() - 12)
	{
		return FResult::Error(WoTMeshError(EWoTBootstrapError::SizeLimit,
			TEXT("the mesh typed object exceeds its framing bound")));
	}
	TArray<uint8> Object;
	Object.Reserve(12 + HeaderBytes.GetValue().Num() + Payload.Num());
	Object.Append(reinterpret_cast<const uint8*>(WoTMeshObjectMagic), 8);
	WoTMeshAppendU32(Object, static_cast<uint32>(HeaderBytes.GetValue().Num()));
	Object.Append(HeaderBytes.GetValue());
	Object.Append(Payload.GetData(), Payload.Num());
	return FResult::Success(MoveTemp(Object));
}

TWoTBootstrapResult<TArray<uint8>> WoTMeshEncodePayload(
	const FWoTMeshDecoded& Decoded)
{
	using FResult = TWoTBootstrapResult<TArray<uint8>>;
	const FWoTVertexMeshObjectHeader& Header = Decoded.Header;
	uint64 Size = 104ull + static_cast<uint64>(Header.VertexCount) * 4
		+ static_cast<uint64>(Header.UVs.Num()) * 2
		+ static_cast<uint64>(Header.Triangles.Num()) * 56
		+ static_cast<uint64>(Header.Sections.Num()) * 20
		+ static_cast<uint64>(Header.RenderVertices.Num()) * 12
		+ static_cast<uint64>(Header.CollisionTriangles.Num()) * 4;
	if (Size > WoTMeshMaxSourceBytes || Size > static_cast<uint64>(MAX_int32)
		|| Decoded.PackedVertices.Num() < static_cast<int32>(Header.VertexCount))
	{
		return FResult::Error(WoTMeshError(EWoTBootstrapError::SizeLimit,
			TEXT("the canonical mesh stream exceeds its byte bound")));
	}
	TArray<uint8> Payload;
	Payload.Reserve(static_cast<int32>(Size));
	Payload.Append(reinterpret_cast<const uint8*>(WoTMeshPayloadMagic), 8);
	WoTMeshAppendU16(Payload, 2);
	WoTMeshAppendU16(Payload, Header.ClassName == TEXT("LodMesh") ? 1 : 0);
	WoTMeshAppendU32(Payload, Header.VertexCount);
	WoTMeshAppendU32(Payload, Header.TriangleCount);
	WoTMeshAppendU32(Payload, static_cast<uint32>(Header.UVs.Num()));
	WoTMeshAppendU32(Payload, static_cast<uint32>(Header.Sections.Num()));
	WoTMeshAppendU32(Payload, static_cast<uint32>(Header.Skins.Num()));
	WoTMeshAppendU32(Payload, static_cast<uint32>(Header.RenderVertices.Num()));
	WoTMeshAppendU32(Payload, static_cast<uint32>(Header.CollisionTriangles.Num()));
	WoTMeshAppendU32(Payload, Header.FrameCount);
	for (float Value : {Header.Origin.X, Header.Origin.Y, Header.Origin.Z,
		Header.Scale.X, Header.Scale.Y, Header.Scale.Z})
	{
		WoTMeshAppendU32(Payload, WoTMeshFloatBits(Value));
	}
	for (int32 Value : Header.RotationOrigin)
	{
		WoTMeshAppendI32(Payload, Value);
	}
	for (float Value : {Header.Bounds.Min.X, Header.Bounds.Min.Y, Header.Bounds.Min.Z,
		Header.Bounds.Max.X, Header.Bounds.Max.Y, Header.Bounds.Max.Z})
	{
		WoTMeshAppendU32(Payload, WoTMeshFloatBits(Value));
	}
	for (uint32 Vertex = 0; Vertex < Header.VertexCount; ++Vertex)
	{
		WoTMeshAppendU32(Payload, Decoded.PackedVertices[static_cast<int32>(Vertex)]);
	}
	for (const FWoTVertexMeshUV& UV : Header.UVs)
	{
		Payload.Add(UV.U);
		Payload.Add(UV.V);
	}
	for (const FWoTVertexMeshTriangle& Triangle : Header.Triangles)
	{
		for (uint32 Value : Triangle.VertexIndices) WoTMeshAppendU32(Payload, Value);
		for (const TOptional<uint32>& Value : Triangle.UVIndices)
		{
			WoTMeshAppendI32(Payload, Value.IsSet()
				? static_cast<int32>(Value.GetValue()) : -1);
		}
		for (uint32 Value : Triangle.RenderVertexIndices) WoTMeshAppendU32(Payload, Value);
		WoTMeshAppendU32(Payload, Triangle.SectionIndex);
		const uint32 Available = (Triangle.UVIndices[0].IsSet() ? 1u : 0u)
			| (Triangle.PolyFlags.IsSet() ? 2u : 0u)
			| (Triangle.TextureIndex.IsSet() ? 4u : 0u)
			| (Triangle.bLodAttachment ? 8u : 0u);
		WoTMeshAppendU32(Payload, Available);
		WoTMeshAppendU32(Payload,
			Triangle.PolyFlags.IsSet() ? Triangle.PolyFlags.GetValue() : 0);
		WoTMeshAppendU32(Payload, 0);
		WoTMeshAppendI32(Payload,
			Triangle.TextureIndex.IsSet() ? Triangle.TextureIndex.GetValue() : 0);
	}
	for (const FWoTVertexMeshSection& Section : Header.Sections)
	{
		WoTMeshAppendU32(Payload, Section.FirstTriangle);
		WoTMeshAppendU32(Payload, Section.TriangleCount);
		const uint32 Available = (Section.PolyFlags.IsSet() ? 1u : 0u)
			| (Section.SkinIndex.IsSet() ? 2u : 0u)
			| (Section.bLodAttachment ? 4u : 0u);
		WoTMeshAppendU32(Payload, Available);
		WoTMeshAppendU32(Payload,
			Section.PolyFlags.IsSet() ? Section.PolyFlags.GetValue() : 0);
		WoTMeshAppendI32(Payload,
			Section.SkinIndex.IsSet() ? static_cast<int32>(Section.SkinIndex.GetValue()) : 0);
	}
	for (const FWoTVertexMeshRenderVertex& Render : Header.RenderVertices)
	{
		WoTMeshAppendU32(Payload, Render.SourceVertex);
		WoTMeshAppendI32(Payload,
			Render.UVIndex.IsSet() ? static_cast<int32>(Render.UVIndex.GetValue()) : -1);
		WoTMeshAppendI32(Payload,
			Render.SkinIndex.IsSet() ? static_cast<int32>(Render.SkinIndex.GetValue()) : -1);
	}
	for (uint32 TriangleIndex : Header.CollisionTriangles)
	{
		WoTMeshAppendU32(Payload, TriangleIndex);
	}
	if (static_cast<uint64>(Payload.Num()) != Size)
	{
		return FResult::Error(WoTMeshError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the canonical mesh stream length is inconsistent")));
	}
	return FResult::Success(MoveTemp(Payload));
}

TWoTBootstrapResult<TArray<uint8>> WoTAnimationEncodePayload(
	const FWoTMeshDecoded& Decoded)
{
	using FResult = TWoTBootstrapResult<TArray<uint8>>;
	const uint64 Size = 28ull + static_cast<uint64>(Decoded.PackedVertices.Num()) * 4;
	if (Size > WoTMeshMaxSourceBytes || Size > static_cast<uint64>(MAX_int32))
	{
		return FResult::Error(WoTMeshError(EWoTBootstrapError::SizeLimit,
			TEXT("the canonical animation stream exceeds its byte bound")));
	}
	TArray<uint8> Payload;
	Payload.Reserve(static_cast<int32>(Size));
	Payload.Append(reinterpret_cast<const uint8*>(WoTAnimationPayloadMagic), 8);
	WoTMeshAppendU16(Payload, 1);
	WoTMeshAppendU16(Payload, 0);
	WoTMeshAppendU32(Payload, Decoded.Header.VertexCount);
	WoTMeshAppendU32(Payload, Decoded.Header.FrameCount);
	WoTMeshAppendU32(Payload, static_cast<uint32>(Decoded.PackedVertices.Num()));
	WoTMeshAppendU32(Payload, Decoded.Header.VertexCount * 4);
	for (uint32 Packed : Decoded.PackedVertices) WoTMeshAppendU32(Payload, Packed);
	return FResult::Success(MoveTemp(Payload));
}

TWoTBootstrapResult<TArray<uint8>> WoTMeshBuildObject(
	const FWoTRequiredFact& Fact, const FWoTMeshSource& Source,
	const FWoTMeshDecoded& Decoded)
{
	using FResult = TWoTBootstrapResult<TArray<uint8>>;
	TWoTBootstrapResult<TArray<uint8>> Payload = WoTMeshEncodePayload(Decoded);
	if (Payload.IsError()) return FResult::Error(MoveTemp(Payload).GetError());
	const FWoTVertexMeshObjectHeader& Mesh = Decoded.Header;

	FWoTCanonicalObjectBuilder BoundsBuilder;
	BoundsBuilder.Add(TEXT("max_bits"), WoTMeshFloatBitsArray3(Mesh.Bounds.Max));
	BoundsBuilder.Add(TEXT("min_bits"), WoTMeshFloatBitsArray3(Mesh.Bounds.Min));
	TWoTBootstrapResult<FWoTCanonicalJsonValue> Bounds =
		MoveTemp(BoundsBuilder).Build(TEXT("mesh bounds"));
	if (Bounds.IsError()) return FResult::Error(MoveTemp(Bounds).GetError());

	FWoTCanonicalJsonValue CollisionIndices = FWoTCanonicalJsonValue::MakeArray();
	for (uint32 Index : Mesh.CollisionTriangles)
	{
		CollisionIndices.AddElement(FWoTCanonicalJsonValue::MakeInteger(Index));
	}
	FWoTCanonicalObjectBuilder CollisionBuilder;
	CollisionBuilder.Add(TEXT("triangle_indices"), MoveTemp(CollisionIndices));
	CollisionBuilder.Add(TEXT("uses_base_frame"), FWoTCanonicalJsonValue::MakeBool(true));
	TWoTBootstrapResult<FWoTCanonicalJsonValue> Collision =
		MoveTemp(CollisionBuilder).Build(TEXT("mesh collision"));
	if (Collision.IsError()) return FResult::Error(MoveTemp(Collision).GetError());

	FWoTCanonicalJsonValue Lod = FWoTCanonicalJsonValue::MakeNull();
	if (Mesh.ClassName == TEXT("LodMesh"))
	{
		FWoTCanonicalObjectBuilder LodBuilder;
		LodBuilder.Add(TEXT("model_vertices"), WoTMeshOptionalUnsigned(Mesh.LodModelVertices));
		LodBuilder.Add(TEXT("old_frame_vertices"),
			WoTMeshOptionalUnsigned(Mesh.LodOldFrameVertices));
		LodBuilder.Add(TEXT("remap_present"), Mesh.bLodRemapPresent.IsSet()
			? FWoTCanonicalJsonValue::MakeBool(Mesh.bLodRemapPresent.GetValue())
			: FWoTCanonicalJsonValue::MakeNull());
		LodBuilder.Add(TEXT("special_vertices"),
			WoTMeshOptionalUnsigned(Mesh.LodSpecialVertices));
		TWoTBootstrapResult<FWoTCanonicalJsonValue> BuiltLod =
			MoveTemp(LodBuilder).Build(TEXT("LodMesh facts"));
		if (BuiltLod.IsError()) return FResult::Error(MoveTemp(BuiltLod).GetError());
		Lod = MoveTemp(BuiltLod).GetValue();
	}

	FWoTCanonicalJsonValue RenderVertices = FWoTCanonicalJsonValue::MakeArray();
	for (const FWoTVertexMeshRenderVertex& Render : Mesh.RenderVertices)
	{
		FWoTCanonicalObjectBuilder Builder;
		Builder.Add(TEXT("skin_index"), WoTMeshOptionalUnsigned(Render.SkinIndex));
		Builder.Add(TEXT("source_vertex"),
			FWoTCanonicalJsonValue::MakeInteger(Render.SourceVertex));
		Builder.Add(TEXT("uv_index"), WoTMeshOptionalUnsigned(Render.UVIndex));
		TWoTBootstrapResult<FWoTCanonicalJsonValue> Built =
			MoveTemp(Builder).Build(TEXT("mesh render vertex"));
		if (Built.IsError()) return FResult::Error(MoveTemp(Built).GetError());
		RenderVertices.AddElement(MoveTemp(Built).GetValue());
	}

	FWoTCanonicalJsonValue Sections = FWoTCanonicalJsonValue::MakeArray();
	for (const FWoTVertexMeshSection& Section : Mesh.Sections)
	{
		FWoTCanonicalObjectBuilder Builder;
		Builder.Add(TEXT("first_triangle"),
			FWoTCanonicalJsonValue::MakeInteger(Section.FirstTriangle));
		Builder.Add(TEXT("lod_attachment"),
			FWoTCanonicalJsonValue::MakeBool(Section.bLodAttachment));
		Builder.Add(TEXT("poly_flags"), WoTMeshOptionalUnsigned(Section.PolyFlags));
		Builder.Add(TEXT("skin_index"), WoTMeshOptionalUnsigned(Section.SkinIndex));
		Builder.Add(TEXT("triangle_count"),
			FWoTCanonicalJsonValue::MakeInteger(Section.TriangleCount));
		TWoTBootstrapResult<FWoTCanonicalJsonValue> Built =
			MoveTemp(Builder).Build(TEXT("mesh section"));
		if (Built.IsError()) return FResult::Error(MoveTemp(Built).GetError());
		Sections.AddElement(MoveTemp(Built).GetValue());
	}

	FWoTCanonicalJsonValue Skins = FWoTCanonicalJsonValue::MakeArray();
	for (const FWoTSemanticSkinId& Skin : Mesh.Skins)
	{
		FWoTCanonicalObjectBuilder Builder;
		Builder.Add(TEXT("logical_id"), WoTMeshOptionalString(Skin.LogicalId));
		Builder.Add(TEXT("name"), WoTMeshOptionalString(Skin.Name));
		Builder.Add(TEXT("package"), WoTMeshOptionalString(Skin.Package));
		TWoTBootstrapResult<FWoTCanonicalJsonValue> Built =
			MoveTemp(Builder).Build(TEXT("mesh skin"));
		if (Built.IsError()) return FResult::Error(MoveTemp(Built).GetError());
		Skins.AddElement(MoveTemp(Built).GetValue());
	}

	FWoTCanonicalJsonValue Triangles = FWoTCanonicalJsonValue::MakeArray();
	for (const FWoTVertexMeshTriangle& Triangle : Mesh.Triangles)
	{
		FWoTCanonicalJsonValue UVIndices = FWoTCanonicalJsonValue::MakeNull();
		if (Triangle.UVIndices[0].IsSet())
		{
			UVIndices = FWoTCanonicalJsonValue::MakeArray();
			for (const TOptional<uint32>& Value : Triangle.UVIndices)
			{
				UVIndices.AddElement(FWoTCanonicalJsonValue::MakeInteger(Value.GetValue()));
			}
		}
		FWoTCanonicalObjectBuilder Builder;
		Builder.Add(TEXT("indices"), WoTMeshUIntArray3(Triangle.VertexIndices));
		Builder.Add(TEXT("lod_attachment"),
			FWoTCanonicalJsonValue::MakeBool(Triangle.bLodAttachment));
		Builder.Add(TEXT("poly_flags"), WoTMeshOptionalUnsigned(Triangle.PolyFlags));
		Builder.Add(TEXT("render_indices"),
			WoTMeshUIntArray3(Triangle.RenderVertexIndices));
		Builder.Add(TEXT("section_index"),
			FWoTCanonicalJsonValue::MakeInteger(Triangle.SectionIndex));
		Builder.Add(TEXT("texture_index"), WoTMeshOptionalSigned(Triangle.TextureIndex));
		Builder.Add(TEXT("uv_indices"), MoveTemp(UVIndices));
		TWoTBootstrapResult<FWoTCanonicalJsonValue> Built =
			MoveTemp(Builder).Build(TEXT("mesh triangle"));
		if (Built.IsError()) return FResult::Error(MoveTemp(Built).GetError());
		Triangles.AddElement(MoveTemp(Built).GetValue());
	}

	FWoTCanonicalJsonValue UVs = FWoTCanonicalJsonValue::MakeArray();
	for (const FWoTVertexMeshUV& UV : Mesh.UVs)
	{
		FWoTCanonicalObjectBuilder Builder;
		Builder.Add(TEXT("u"), FWoTCanonicalJsonValue::MakeInteger(UV.U));
		Builder.Add(TEXT("v"), FWoTCanonicalJsonValue::MakeInteger(UV.V));
		TWoTBootstrapResult<FWoTCanonicalJsonValue> Built =
			MoveTemp(Builder).Build(TEXT("mesh UV"));
		if (Built.IsError()) return FResult::Error(MoveTemp(Built).GetError());
		UVs.AddElement(MoveTemp(Built).GetValue());
	}
	TWoTBootstrapResult<FWoTCanonicalJsonValue> SourceJson = WoTMeshSourceJson(Fact, Source);
	if (SourceJson.IsError()) return FResult::Error(MoveTemp(SourceJson).GetError());

	FWoTCanonicalObjectBuilder HeaderBuilder;
	HeaderBuilder.Add(TEXT("bounds"), MoveTemp(Bounds).GetValue());
	HeaderBuilder.Add(TEXT("class_name"), FWoTCanonicalJsonValue::MakeString(Mesh.ClassName));
	HeaderBuilder.Add(TEXT("collision"), MoveTemp(Collision).GetValue());
	HeaderBuilder.Add(TEXT("encoding"),
		FWoTCanonicalJsonValue::MakeString(TEXT("ue1-vertex-mesh-le-v2")));
	HeaderBuilder.Add(TEXT("fact_id"), FWoTCanonicalJsonValue::MakeString(Fact.FactId));
	HeaderBuilder.Add(TEXT("frame_count"),
		FWoTCanonicalJsonValue::MakeInteger(Mesh.FrameCount));
	HeaderBuilder.Add(TEXT("importer"), FWoTCanonicalJsonValue::MakeString(TEXT("mesh-v1")));
	HeaderBuilder.Add(TEXT("importer_version"), FWoTCanonicalJsonValue::MakeInteger(1));
	HeaderBuilder.Add(TEXT("kind"), FWoTCanonicalJsonValue::MakeString(TEXT("mesh")));
	HeaderBuilder.Add(TEXT("lod"), MoveTemp(Lod));
	HeaderBuilder.Add(TEXT("origin_bits"), WoTMeshFloatBitsArray3(Mesh.Origin));
	HeaderBuilder.Add(TEXT("payload_byte_length"),
		FWoTCanonicalJsonValue::MakeInteger(Payload.GetValue().Num()));
	HeaderBuilder.Add(TEXT("render_vertices"), MoveTemp(RenderVertices));
	HeaderBuilder.Add(TEXT("rotation_origin"), WoTMeshIntArray3(Mesh.RotationOrigin));
	HeaderBuilder.Add(TEXT("scale_bits"), WoTMeshFloatBitsArray3(Mesh.Scale));
	HeaderBuilder.Add(TEXT("sections"), MoveTemp(Sections));
	HeaderBuilder.Add(TEXT("skins"), MoveTemp(Skins));
	HeaderBuilder.Add(TEXT("source"), MoveTemp(SourceJson).GetValue());
	HeaderBuilder.Add(TEXT("tangent_rule"),
		FWoTCanonicalJsonValue::MakeString(WoTMeshTangentRule));
	HeaderBuilder.Add(TEXT("triangle_count"),
		FWoTCanonicalJsonValue::MakeInteger(Mesh.TriangleCount));
	HeaderBuilder.Add(TEXT("triangles"), MoveTemp(Triangles));
	HeaderBuilder.Add(TEXT("uv_count"),
		FWoTCanonicalJsonValue::MakeInteger(Mesh.UVs.Num()));
	HeaderBuilder.Add(TEXT("uvs"), MoveTemp(UVs));
	HeaderBuilder.Add(TEXT("vertex_count"),
		FWoTCanonicalJsonValue::MakeInteger(Mesh.VertexCount));
	TWoTBootstrapResult<FWoTCanonicalJsonValue> Header =
		MoveTemp(HeaderBuilder).Build(TEXT("mesh header"));
	if (Header.IsError()) return FResult::Error(MoveTemp(Header).GetError());
	return WoTMeshFrameObject(Header.GetValue(), Payload.GetValue());
}

TWoTBootstrapResult<TArray<uint8>> WoTAnimationBuildObject(
	const FWoTRequiredFact& Fact, const FWoTMeshSource& Source,
	const FWoTMeshDecoded& Decoded)
{
	using FResult = TWoTBootstrapResult<TArray<uint8>>;
	TWoTBootstrapResult<TArray<uint8>> Payload = WoTAnimationEncodePayload(Decoded);
	if (Payload.IsError()) return FResult::Error(MoveTemp(Payload).GetError());
	FWoTCanonicalJsonValue Sequences = FWoTCanonicalJsonValue::MakeArray();
	for (const FWoTVertexMeshSequence& Sequence : Decoded.Header.Sequences)
	{
		FWoTCanonicalJsonValue Notifies = FWoTCanonicalJsonValue::MakeArray();
		for (const FWoTVertexMeshNotify& Notify : Sequence.Notifies)
		{
			FWoTCanonicalObjectBuilder NotifyBuilder;
			NotifyBuilder.Add(TEXT("function"),
				FWoTCanonicalJsonValue::MakeString(Notify.Function));
			NotifyBuilder.Add(TEXT("time_bits"),
				FWoTCanonicalJsonValue::MakeInteger(WoTMeshFloatBits(Notify.Time)));
			TWoTBootstrapResult<FWoTCanonicalJsonValue> BuiltNotify =
				MoveTemp(NotifyBuilder).Build(TEXT("mesh animation notify"));
			if (BuiltNotify.IsError())
			{
				return FResult::Error(MoveTemp(BuiltNotify).GetError());
			}
			Notifies.AddElement(MoveTemp(BuiltNotify).GetValue());
		}
		FWoTCanonicalObjectBuilder SequenceBuilder;
		SequenceBuilder.Add(TEXT("first_frame"),
			FWoTCanonicalJsonValue::MakeInteger(Sequence.FirstFrame));
		SequenceBuilder.Add(TEXT("group"), FWoTCanonicalJsonValue::MakeString(Sequence.Group));
		SequenceBuilder.Add(TEXT("name"), FWoTCanonicalJsonValue::MakeString(Sequence.Name));
		SequenceBuilder.Add(TEXT("notifies"), MoveTemp(Notifies));
		SequenceBuilder.Add(TEXT("num_frames"),
			FWoTCanonicalJsonValue::MakeInteger(Sequence.NumFrames));
		SequenceBuilder.Add(TEXT("rate_bits"),
			FWoTCanonicalJsonValue::MakeInteger(WoTMeshFloatBits(Sequence.Rate)));
		TWoTBootstrapResult<FWoTCanonicalJsonValue> BuiltSequence =
			MoveTemp(SequenceBuilder).Build(TEXT("mesh animation sequence"));
		if (BuiltSequence.IsError())
		{
			return FResult::Error(MoveTemp(BuiltSequence).GetError());
		}
		Sequences.AddElement(MoveTemp(BuiltSequence).GetValue());
	}
	TWoTBootstrapResult<FWoTCanonicalJsonValue> SourceJson = WoTMeshSourceJson(Fact, Source);
	if (SourceJson.IsError()) return FResult::Error(MoveTemp(SourceJson).GetError());
	FWoTCanonicalObjectBuilder HeaderBuilder;
	HeaderBuilder.Add(TEXT("encoding"),
		FWoTCanonicalJsonValue::MakeString(TEXT("ue1-packed-vertex-frames-le-v1")));
	HeaderBuilder.Add(TEXT("fact_id"), FWoTCanonicalJsonValue::MakeString(Fact.FactId));
	HeaderBuilder.Add(TEXT("frame_byte_length"),
		FWoTCanonicalJsonValue::MakeInteger(Decoded.Header.VertexCount * 4));
	HeaderBuilder.Add(TEXT("frame_count"),
		FWoTCanonicalJsonValue::MakeInteger(Decoded.Header.FrameCount));
	HeaderBuilder.Add(TEXT("importer"),
		FWoTCanonicalJsonValue::MakeString(TEXT("animation-v1")));
	HeaderBuilder.Add(TEXT("importer_version"), FWoTCanonicalJsonValue::MakeInteger(1));
	HeaderBuilder.Add(TEXT("kind"), FWoTCanonicalJsonValue::MakeString(TEXT("animation")));
	HeaderBuilder.Add(TEXT("payload_byte_length"),
		FWoTCanonicalJsonValue::MakeInteger(Payload.GetValue().Num()));
	HeaderBuilder.Add(TEXT("sequence_count"),
		FWoTCanonicalJsonValue::MakeInteger(Decoded.Header.Sequences.Num()));
	HeaderBuilder.Add(TEXT("sequences"), MoveTemp(Sequences));
	HeaderBuilder.Add(TEXT("source"), MoveTemp(SourceJson).GetValue());
	HeaderBuilder.Add(TEXT("vertex_count"),
		FWoTCanonicalJsonValue::MakeInteger(Decoded.Header.VertexCount));
	TWoTBootstrapResult<FWoTCanonicalJsonValue> Header =
		MoveTemp(HeaderBuilder).Build(TEXT("animation header"));
	if (Header.IsError()) return FResult::Error(MoveTemp(Header).GetError());
	return WoTMeshFrameObject(Header.GetValue(), Payload.GetValue());
}

class FWoTMeshCollectSink final : public IWoTByteSink
{
public:
	explicit FWoTMeshCollectSink(uint64 InLimit) : Limit(InLimit) {}
	FWoTBootstrapStatus Consume(TConstArrayView<uint8> Chunk) override
	{
		const uint64 Count = static_cast<uint64>(Chunk.Num());
		if (Written > Limit || Count > Limit - Written
			|| Chunk.Num() > MAX_int32 - Bytes.Num())
		{
			return FWoTBootstrapStatus::Error(WoTMeshError(
				EWoTBootstrapError::SizeLimit,
				TEXT("the mesh source exceeded its quarantine limit")));
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

struct FWoTMeshFactType
{
	const TCHAR* Importer = nullptr;
	const TCHAR* Schema = nullptr;
	bool bAnimation = false;
};

bool WoTMeshSelectFact(const FWoTRequiredFact& Fact, FWoTMeshFactType& Out)
{
	switch (Fact.Domain)
	{
	case EWoTFactDomain::Mesh:
		Out = {TEXT("mesh-v1"), TEXT("wot-mesh-v3"), false};
		return true;
	case EWoTFactDomain::Animation:
		Out = {TEXT("animation-v1"), TEXT("wot-animation-v3"), true};
		return true;
	default:
		return false;
	}
}

FWoTBootstrapError WoTMeshCancelled()
{
	return WoTMeshError(EWoTBootstrapError::ServiceUnavailable,
		TEXT("mesh/animation import was cancelled before completion"),
		TEXT("mesh-animation"));
}

using FWoTMeshSourceLoader =
	TFunction<TWoTBootstrapResult<TArray<uint8>>(FStringView)>;
using FWoTMeshPublicationVerifier = TFunction<FWoTBootstrapStatus()>;

TWoTBootstrapResult<TArray<FWoTCacheBlob>> WoTMeshImportCore(
	const FWoTRetailContract& Contract, IWoTObjectWriter& Writer,
	IWoTProgressSink& Progress,
	FWoTMeshSourceLoader&& Loader,
	FWoTMeshPublicationVerifier&& ReverifyBeforePublication)
{
	using FResult = TWoTBootstrapResult<TArray<FWoTCacheBlob>>;
	uint64 Total = 0;
	for (const FWoTRequiredFact& Fact : Contract.Facts)
	{
		FWoTMeshFactType Type;
		if (WoTMeshSelectFact(Fact, Type))
		{
			++Total;
		}
	}
	if (Total > static_cast<uint64>(MAX_int32))
	{
		return FResult::Error(WoTMeshError(EWoTBootstrapError::SizeLimit,
			TEXT("the mesh/animation fact set exceeds its bound")));
	}
	FWoTBootstrapStatus Initial = Progress.ReportProgress(TEXT("mesh-animation"), 0, Total);
	if (Initial.IsError()) return FResult::Error(MoveTemp(Initial).GetError());
	if (Progress.IsCancellationRequested()) return FResult::Error(WoTMeshCancelled());

	TArray<FWoTCacheBlob> Blobs;
	Blobs.Reserve(static_cast<int32>(Total));
	TSet<FString> LogicalIds;
	TOptional<FWoTMeshSource> CachedSource;
	FString CachedSourcePath;
	uint64 Completed = 0;
	for (const FWoTRequiredFact& Fact : Contract.Facts)
	{
		FWoTMeshFactType Type;
		if (!WoTMeshSelectFact(Fact, Type)) continue;
		if (Progress.IsCancellationRequested()) return FResult::Error(WoTMeshCancelled());
		if (Fact.FactId.IsEmpty() || Fact.SourcePath.IsEmpty()
			|| Fact.SourceExport.IsEmpty() || Fact.Importer != Type.Importer
			|| Fact.Outputs.Num() != 1 || Fact.Outputs[0].LogicalId.IsEmpty()
			|| Fact.Outputs[0].Schema != Type.Schema
			|| Fact.Outputs[0].MaxDecodedSize == 0)
		{
			return FResult::Error(WoTMeshError(EWoTBootstrapError::UnknownField,
				TEXT("the mesh fact does not match its closed importer/output schema"),
				Fact.FactId));
		}
		const FWoTRequiredFactOutput& Output = Fact.Outputs[0];
		if (LogicalIds.Contains(Output.LogicalId))
		{
			return FResult::Error(WoTMeshError(EWoTBootstrapError::DuplicateField,
				TEXT("two mesh facts emit the same logical object"), Output.LogicalId));
		}
		LogicalIds.Add(Output.LogicalId);

		if (!CachedSource.IsSet() || CachedSourcePath != Fact.SourcePath)
		{
			TWoTBootstrapResult<TArray<uint8>> Loaded = Loader(Fact.SourcePath);
			if (Loaded.IsError()) return FResult::Error(MoveTemp(Loaded).GetError());
			TWoTBootstrapResult<FWoTMeshSource> Parsed =
				WoTMeshParseSource(Loaded.GetValue(), Fact.SourcePath);
			if (Parsed.IsError()) return FResult::Error(MoveTemp(Parsed).GetError());
			CachedSource.Emplace(MoveTemp(Parsed).GetValue());
			CachedSourcePath = Fact.SourcePath;
		}
		const FWoTMeshSource& Source = CachedSource.GetValue();
		TWoTBootstrapResult<const FWoTMeshExport*> Export =
			WoTMeshFindExport(Source, Fact.SourceExport);
		if (Export.IsError()) return FResult::Error(MoveTemp(Export).GetError());
		TWoTBootstrapResult<FWoTMeshDecoded> Decoded = WoTMeshDecode(
			Source, *Export.GetValue(), Fact.SourcePath);
		if (Decoded.IsError()) return FResult::Error(MoveTemp(Decoded).GetError());
		TWoTBootstrapResult<TArray<uint8>> Object = Type.bAnimation
			? WoTAnimationBuildObject(Fact, Source, Decoded.GetValue())
			: WoTMeshBuildObject(Fact, Source, Decoded.GetValue());
		if (Object.IsError()) return FResult::Error(MoveTemp(Object).GetError());
		if (static_cast<uint64>(Object.GetValue().Num()) > Output.MaxDecodedSize)
		{
			return FResult::Error(WoTMeshError(EWoTBootstrapError::SizeLimit,
				TEXT("the mesh typed object exceeds its contract maximum"), Output.LogicalId));
		}
		if (Progress.IsCancellationRequested()) return FResult::Error(WoTMeshCancelled());
		FWoTBootstrapStatus Reverified = ReverifyBeforePublication();
		if (Reverified.IsError())
		{
			return FResult::Error(MoveTemp(Reverified).GetError());
		}
		const FWoTSha256 ObjectSha256 = Sha256(Object.GetValue());
		FWoTBootstrapStatus Written = Writer.WriteObject(Output.LogicalId,
			Output.Schema, Object.GetValue(), ObjectSha256, Output.MaxDecodedSize);
		if (Written.IsError()) return FResult::Error(MoveTemp(Written).GetError());
		FWoTCacheBlob& Blob = Blobs.AddDefaulted_GetRef();
		Blob.LogicalId = Output.LogicalId;
		Blob.Schema = Output.Schema;
		Blob.Sha256 = ObjectSha256;
		Blob.Size = static_cast<uint64>(Object.GetValue().Num());
		FWoTRequiredFactId& Satisfied = Blob.Satisfies.AddDefaulted_GetRef();
		Satisfied.Value = Fact.FactId;
		++Completed;
		FWoTBootstrapStatus Reported = Progress.ReportProgress(
			TEXT("mesh-animation"), Completed, Total);
		if (Reported.IsError()) return FResult::Error(MoveTemp(Reported).GetError());
		if (Progress.IsCancellationRequested()) return FResult::Error(WoTMeshCancelled());
	}
	return FResult::Success(MoveTemp(Blobs));
}
} // namespace

#if !WOT_GUARDED_BOOTSTRAP
struct FWoTDevelopmentMeshPackage::FImpl
{
	FWoTMeshSource Source;
	FString SourcePath;
};

FWoTDevelopmentMeshPackage::FWoTDevelopmentMeshPackage(
	TUniquePtr<FImpl> InImpl)
	: Impl(MoveTemp(InImpl))
{
}

FWoTDevelopmentMeshPackage::~FWoTDevelopmentMeshPackage() = default;

TWoTBootstrapResult<TUniquePtr<FWoTDevelopmentMeshPackage>>
FWoTDevelopmentMeshPackage::Parse(
	TConstArrayView<uint8> SourceBytes, FStringView SourcePath)
{
	using FResult = TWoTBootstrapResult<TUniquePtr<FWoTDevelopmentMeshPackage>>;
	TWoTBootstrapResult<FWoTMeshSource> Parsed =
		WoTMeshParseSource(SourceBytes, SourcePath);
	if (Parsed.IsError())
	{
		return FResult::Error(MoveTemp(Parsed).GetError());
	}
	TUniquePtr<FImpl> State = MakeUnique<FImpl>();
	State->Source = MoveTemp(Parsed).GetValue();
	State->SourcePath = FString(SourcePath);
	return FResult::Success(TUniquePtr<FWoTDevelopmentMeshPackage>(
		new FWoTDevelopmentMeshPackage(MoveTemp(State))));
}

bool FWoTDevelopmentMeshPackage::FindMeshSequences(
	FStringView SourceExport, TArray<FWoTVertexMeshSequence>& Out) const
{
	Out.Reset();
	if (!Impl.IsValid())
	{
		return false;
	}
	TWoTBootstrapResult<const FWoTMeshExport*> Export =
		WoTMeshFindExport(Impl->Source, SourceExport);
	if (Export.IsError())
	{
		return false;
	}
	TWoTBootstrapResult<FWoTMeshDecoded> Decoded = WoTMeshDecode(
		Impl->Source, *Export.GetValue(), Impl->SourcePath);
	if (Decoded.IsError())
	{
		return false;
	}
	Out = Decoded.GetValue().Header.Sequences;
	return true;
}

bool FWoTDevelopmentMeshPackage::FindUnreferencedMeshFrames(
	FStringView SourceExport, TArray<TArray<FVector>>& Out) const
{
	Out.Reset();
	if (!Impl.IsValid()) return false;
	auto Export = WoTMeshFindExport(Impl->Source, SourceExport);
	if (Export.IsError()) return false;
	auto Decoded = WoTMeshDecode(Impl->Source, *Export.GetValue(), Impl->SourcePath);
	if (Decoded.IsError()) return false;
	const FWoTMeshDecoded& Mesh = Decoded.GetValue();
	const FWoTVertexMeshObjectHeader& Header = Mesh.Header;
	TSet<uint32> Referenced;
	for (const FWoTVertexMeshTriangle& Triangle : Header.Triangles)
		for (const uint32 Vertex : Triangle.VertexIndices) Referenced.Add(Vertex);
	for (uint32 Frame = 0; Frame < Header.FrameCount; ++Frame)
	{
		TArray<FVector>& Positions = Out.AddDefaulted_GetRef();
		for (uint32 Vertex = 0; Vertex < Header.VertexCount; ++Vertex)
		{
			if (!Referenced.Contains(Vertex))
				Positions.Add(FVector(WoTMeshPosition(
					Mesh.PackedVertices[Frame * Header.VertexCount + Vertex],
					Header.Origin, Header.Scale, Header.RotationOrigin)));
		}
	}
	return true;
}
#endif

TWoTBootstrapResult<TArray<FWoTCacheBlob>> ImportMeshAnimation(
	const FWoTRetailPackage& Package, const FWoTRetailContract& Contract,
	IWoTObjectWriter& Writer, IWoTProgressSink& Progress)
{
	FWoTMeshSourceLoader Loader = [&Package](FStringView SourcePath)
	{
		using FLoadResult = TWoTBootstrapResult<TArray<uint8>>;
		TWoTBootstrapResult<FWoTPackageEntry> Entry = Package.FindExact(SourcePath);
		if (Entry.IsError()) return FLoadResult::Error(MoveTemp(Entry).GetError());
		if (Entry.GetValue().Size == 0 || Entry.GetValue().Size > WoTMeshMaxSourceBytes)
		{
			return FLoadResult::Error(WoTMeshError(EWoTBootstrapError::SizeLimit,
				TEXT("the mesh source declaration exceeds its byte bound"),
				FString(SourcePath)));
		}
		FWoTMeshCollectSink Sink(WoTMeshMaxSourceBytes);
		FWoTBootstrapStatus Streamed = Package.StreamExact(Entry.GetValue(), Sink);
		if (Streamed.IsError()) return FLoadResult::Error(MoveTemp(Streamed).GetError());
		return FLoadResult::Success(MoveTemp(Sink.Bytes));
	};
	FWoTMeshPublicationVerifier ReverifyBeforePublication = [&Package]()
	{
		return Package.ReverifyHeldSnapshotAfterExtraction();
	};
	return WoTMeshImportCore(
		Contract, Writer, Progress, MoveTemp(Loader),
		MoveTemp(ReverifyBeforePublication));
}

#if WITH_AUTOMATION_TESTS
TWoTBootstrapResult<TArray<FWoTCacheBlob>>
ImportMeshAnimationPackageBytesForTesting(TConstArrayView<uint8> SourceBytes,
	FStringView SourcePath, const FWoTRetailContract& Contract,
	IWoTObjectWriter& Writer, IWoTProgressSink& Progress)
{
	const FString ExpectedPath(SourcePath);
	FWoTMeshSourceLoader Loader = [SourceBytes, ExpectedPath](FStringView RequestedPath)
	{
		using FLoadResult = TWoTBootstrapResult<TArray<uint8>>;
		if (FString(RequestedPath) != ExpectedPath)
		{
			return FLoadResult::Error(WoTMeshError(EWoTBootstrapError::UnknownField,
				TEXT("the test mesh source path does not match the supplied package"),
				FString(RequestedPath)));
		}
		TArray<uint8> Copy;
		Copy.Append(SourceBytes.GetData(), SourceBytes.Num());
		return FLoadResult::Success(MoveTemp(Copy));
	};
	FWoTMeshPublicationVerifier ReverifyBeforePublication = []()
	{
		return FWoTBootstrapStatus::Success();
	};
	return WoTMeshImportCore(
		Contract, Writer, Progress, MoveTemp(Loader),
		MoveTemp(ReverifyBeforePublication));
}
#endif
