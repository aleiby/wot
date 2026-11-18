#include "WoTRetailReader/TextureSound.h"

#include "CanonicalObjectBuilder.h"
#include "Misc/Paths.h"
#include "WoTBootstrapCore/CanonicalJson.h"
#include "WoTBootstrapCore/Hash.h"

namespace
{
constexpr uint64 WoTTextureSoundMaxSourceBytes = 256ull * 1024 * 1024;
constexpr uint32 WoTTextureSoundMaxTableItems = 65536;
constexpr uint32 WoTTextureSoundMaxNameBytes = 1024;
constexpr uint32 WoTTextureSoundMaxMips = 32;
constexpr uint32 WoTTextureSoundMaxOuterDepth = 32;
constexpr uint32 WoTTextureSoundMaxSamplerLoops = 16;
constexpr int32 WoTTextureSoundMaxDimension = 8192;
constexpr uint32 WoTTextureSoundMaxHeaderBytes = 1024 * 1024;
constexpr uint32 WoTTextureSoundPackageTag = 0x9e2a83c1;
constexpr uint32 WoTTextureSoundHasStack = 0x02000000;
constexpr ANSICHAR WoTTextureSoundObjectMagic[] = "WOTOBJ3";
constexpr ANSICHAR WoTTextureSoundTextureMagic[] = "WOTPTEX";

FWoTBootstrapError WoTTextureSoundError(EWoTBootstrapError Code,
	const TCHAR* Message, FString Path = FString())
{
	return MakeWoTBootstrapError(Code, Message, MoveTemp(Path));
}

class FWoTTextureSoundReader
{
public:
	explicit FWoTTextureSoundReader(TConstArrayView<uint8> InBytes)
		: Bytes(InBytes), End(static_cast<uint64>(InBytes.Num()))
	{
	}

	FWoTTextureSoundReader(TConstArrayView<uint8> InBytes, uint64 InOffset,
		uint64 InEnd)
		: Bytes(InBytes), Offset(InOffset), End(InEnd)
	{
		if (InOffset > InEnd || InEnd > static_cast<uint64>(InBytes.Num()))
		{
			bValid = false;
		}
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
		if (bMore || Magnitude > static_cast<uint64>(TNumericLimits<int32>::Max())
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
		if (!Require(Count) || Count > static_cast<uint64>(TNumericLimits<int32>::Max()))
		{
			return false;
		}
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

struct FWoTTextureSoundImport
{
	FString ClassName;
	FString ObjectName;
	int32 Outer = 0;
};

struct FWoTTextureSoundExport
{
	int32 ClassReference = 0;
	int32 Group = 0;
	FString Name;
	uint32 Flags = 0;
	uint64 Offset = 0;
	uint64 Size = 0;
};

struct FWoTTextureSoundSource
{
	TArray<uint8> Bytes;
	FString PackageName;
	uint16 Version = 0;
	TArray<FString> Names;
	TArray<FWoTTextureSoundImport> Imports;
	TArray<FWoTTextureSoundExport> Exports;
	FWoTSha256 Sha256;

	const FString* NameAt(int32 Index) const
	{
		return Names.IsValidIndex(Index) ? &Names[Index] : nullptr;
	}

	const FString* ClassName(int32 Reference) const
	{
		if (Reference < 0)
		{
			const int64 ImportIndex = -static_cast<int64>(Reference) - 1;
			return Imports.IsValidIndex(static_cast<int32>(ImportIndex))
				? &Imports[static_cast<int32>(ImportIndex)].ObjectName : nullptr;
		}
		if (Reference > 0 && Exports.IsValidIndex(Reference - 1))
		{
			return &Exports[Reference - 1].Name;
		}
		return nullptr;
	}
};

bool WoTTextureSoundLatin1(TConstArrayView<uint8> Raw, FString& Out)
{
	Out.Reset(Raw.Num());
	for (const uint8 Byte : Raw)
	{
		if (Byte == 0)
		{
			return false;
		}
		Out.AppendChar(static_cast<TCHAR>(Byte));
	}
	return true;
}

TWoTBootstrapResult<FWoTTextureSoundSource> WoTTextureSoundParseSource(
	TArray<uint8>&& Bytes, FStringView Path)
{
	using FResult = TWoTBootstrapResult<FWoTTextureSoundSource>;
	if (Bytes.IsEmpty() || static_cast<uint64>(Bytes.Num()) > WoTTextureSoundMaxSourceBytes)
	{
		return FResult::Error(WoTTextureSoundError(EWoTBootstrapError::SizeLimit,
			TEXT("the UE1 source package exceeds the typed-import bound"), FString(Path)));
	}
	FWoTTextureSoundSource Source;
	Source.Bytes = MoveTemp(Bytes);
	Source.PackageName = FPaths::GetBaseFilename(FString(Path));
	Source.Sha256 = Sha256(Source.Bytes);
	FWoTTextureSoundReader Reader(Source.Bytes);
	uint32 Tag = 0;
	uint16 Licensee = 0;
	uint32 PackageFlags = 0;
	int32 NameCount = 0;
	int32 NameOffset = 0;
	int32 ExportCount = 0;
	int32 ExportOffset = 0;
	int32 ImportCount = 0;
	int32 ImportOffset = 0;
	if (!Reader.ReadU32(Tag) || !Reader.ReadU16(Source.Version)
		|| !Reader.ReadU16(Licensee) || !Reader.ReadU32(PackageFlags)
		|| !Reader.ReadI32(NameCount) || !Reader.ReadI32(NameOffset)
		|| !Reader.ReadI32(ExportCount) || !Reader.ReadI32(ExportOffset)
		|| !Reader.ReadI32(ImportCount) || !Reader.ReadI32(ImportOffset)
		|| Tag != WoTTextureSoundPackageTag || Source.Version < 61 || Source.Version > 69
		|| NameCount <= 0 || ExportCount <= 0 || ImportCount < 0
		|| NameCount > static_cast<int32>(WoTTextureSoundMaxTableItems)
		|| ExportCount > static_cast<int32>(WoTTextureSoundMaxTableItems)
		|| ImportCount > static_cast<int32>(WoTTextureSoundMaxTableItems)
		|| NameOffset < 0 || ExportOffset < 0 || ImportOffset < 0)
	{
		return FResult::Error(WoTTextureSoundError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the typed source is not a bounded UE1 package"), FString(Path)));
	}

	if (!Reader.SetOffset(static_cast<uint64>(NameOffset)))
	{
		return FResult::Error(WoTTextureSoundError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the UE1 name table is outside the source"), FString(Path)));
	}
	Source.Names.Reserve(NameCount);
	for (int32 Index = 0; Index < NameCount; ++Index)
	{
		TArray<uint8> RawName;
		if (Source.Version >= 64)
		{
			int32 StoredLength = 0;
			if (!Reader.ReadCompact(StoredLength) || StoredLength <= 0
				|| StoredLength > static_cast<int32>(WoTTextureSoundMaxNameBytes)
				|| !Reader.ReadBytes(static_cast<uint64>(StoredLength), RawName)
				|| RawName.Last() != 0)
			{
				return FResult::Error(WoTTextureSoundError(
					EWoTBootstrapError::CorruptInstallation,
					TEXT("the UE1 name table has an invalid counted name"), FString(Path)));
			}
			RawName.Pop(EAllowShrinking::No);
		}
		else
		{
			bool bTerminated = false;
			for (uint32 NameByte = 0; NameByte < WoTTextureSoundMaxNameBytes; ++NameByte)
			{
				uint8 Byte = 0;
				if (!Reader.ReadU8(Byte)) break;
				if (Byte == 0)
				{
					bTerminated = true;
					break;
				}
				RawName.Add(Byte);
			}
			if (!Reader.IsValid() || !bTerminated)
			{
				return FResult::Error(WoTTextureSoundError(
					EWoTBootstrapError::CorruptInstallation,
					TEXT("the UE1 name table is truncated"), FString(Path)));
			}
		}
		uint32 NameFlags = 0;
		FString Name;
		if (!Reader.ReadU32(NameFlags) || !WoTTextureSoundLatin1(RawName, Name))
		{
			return FResult::Error(WoTTextureSoundError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("the UE1 name table contains an invalid name"), FString(Path)));
		}
		if (Name.IsEmpty() || Name.Contains(TEXT("."), ESearchCase::CaseSensitive))
		{
			return FResult::Error(WoTTextureSoundError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("a serialized UE1 name component is empty or contains a dot"),
				FString::Printf(TEXT("source.package.name_table[%d]"), Index)));
		}
		Source.Names.Add(MoveTemp(Name));
	}

	if (!Reader.SetOffset(static_cast<uint64>(ImportOffset)))
	{
		return FResult::Error(WoTTextureSoundError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the UE1 import table is outside the source"), FString(Path)));
	}
	Source.Imports.Reserve(ImportCount);
	for (int32 Index = 0; Index < ImportCount; ++Index)
	{
		int32 ClassPackage = 0;
		int32 ClassName = 0;
		int32 Outer = 0;
		int32 ObjectName = 0;
		if (!Reader.ReadCompact(ClassPackage) || !Reader.ReadCompact(ClassName)
			|| !Reader.ReadI32(Outer) || !Reader.ReadCompact(ObjectName)
			|| Source.NameAt(ClassPackage) == nullptr || Source.NameAt(ClassName) == nullptr
			|| Source.NameAt(ObjectName) == nullptr)
		{
			return FResult::Error(WoTTextureSoundError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("the UE1 import table is invalid"), FString(Path)));
		}
		FWoTTextureSoundImport& Import = Source.Imports.AddDefaulted_GetRef();
		Import.ClassName = *Source.NameAt(ClassName);
		Import.ObjectName = *Source.NameAt(ObjectName);
		Import.Outer = Outer;
	}

	if (!Reader.SetOffset(static_cast<uint64>(ExportOffset)))
	{
		return FResult::Error(WoTTextureSoundError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the UE1 export table is outside the source"), FString(Path)));
	}
	Source.Exports.Reserve(ExportCount);
	for (int32 Index = 0; Index < ExportCount; ++Index)
	{
		int32 ClassReference = 0;
		int32 SuperReference = 0;
		int32 Group = 0;
		int32 NameIndex = 0;
		uint32 ObjectFlags = 0;
		int32 SerialSize = 0;
		int32 SerialOffset = 0;
		if (!Reader.ReadCompact(ClassReference) || !Reader.ReadCompact(SuperReference)
			|| !Reader.ReadI32(Group) || !Reader.ReadCompact(NameIndex)
			|| !Reader.ReadU32(ObjectFlags) || !Reader.ReadCompact(SerialSize)
			|| SerialSize < 0 || Source.NameAt(NameIndex) == nullptr
			|| (SerialSize > 0 && !Reader.ReadCompact(SerialOffset)) || SerialOffset < 0
			|| static_cast<uint64>(SerialOffset) > static_cast<uint64>(Source.Bytes.Num())
			|| static_cast<uint64>(SerialSize)
				> static_cast<uint64>(Source.Bytes.Num()) - static_cast<uint64>(SerialOffset))
		{
			return FResult::Error(WoTTextureSoundError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("the UE1 export table is invalid"), FString(Path)));
		}
		FWoTTextureSoundExport& Export = Source.Exports.AddDefaulted_GetRef();
		Export.ClassReference = ClassReference;
		Export.Group = Group;
		Export.Name = *Source.NameAt(NameIndex);
		Export.Flags = ObjectFlags;
		Export.Offset = static_cast<uint64>(SerialOffset);
		Export.Size = static_cast<uint64>(SerialSize);
	}
	for (const FWoTTextureSoundExport& Export : Source.Exports)
	{
		const int64 ImportIndex =
			-static_cast<int64>(Export.ClassReference) - 1;
		if ((Export.ClassReference < 0
				&& (ImportIndex < 0 || ImportIndex >= Source.Imports.Num()))
			|| (Export.ClassReference > 0
				&& !Source.Exports.IsValidIndex(Export.ClassReference - 1)))
		{
			return FResult::Error(WoTTextureSoundError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("a UE1 export class reference is outside its table"),
				FString(Path)));
		}
	}
	return FResult::Success(MoveTemp(Source));
}

bool WoTTextureSoundReadName(FWoTTextureSoundReader& Reader,
	const FWoTTextureSoundSource& Source, FString& Out)
{
	int32 Index = 0;
	if (!Reader.ReadCompact(Index)) return false;
	const FString* Name = Source.NameAt(Index);
	if (Name == nullptr) return false;
	Out = *Name;
	return true;
}

struct FWoTTextureSoundProperties
{
	bool bMasked = false;
	bool bParametric = false;
	int32 Palette = 0;
	uint8 Format = 0;
	int32 AnimNext = 0;
	float MinFrameRate = 0.0f;
	float MaxFrameRate = 0.0f;
	uint8 PrimeCount = 0;
	bool bSawMasked = false;
	bool bSawParametric = false;
	bool bSawPalette = false;
	bool bSawFormat = false;
	bool bSawAnimNext = false;
	bool bSawMinFrameRate = false;
	bool bSawMaxFrameRate = false;
	bool bSawPrimeCount = false;
};

bool WoTTextureSoundReadProperties(FWoTTextureSoundReader& Reader,
	const FWoTTextureSoundSource& Source, FWoTTextureSoundProperties& Out)
{
	for (uint32 PropertyIndex = 0; PropertyIndex < WoTTextureSoundMaxTableItems;
		++PropertyIndex)
	{
		FString Name;
		if (!WoTTextureSoundReadName(Reader, Source, Name)) return false;
		if (Name == TEXT("None")) return true;
		uint8 Info = 0;
		if (!Reader.ReadU8(Info)) return false;
		const uint8 PropertyType = Info & 0x0f;
		if (PropertyType == 10)
		{
			FString StructName;
			if (!WoTTextureSoundReadName(Reader, Source, StructName)) return false;
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
		uint32 ArrayIndex = 0;
		if ((Info & 0x80) != 0 && PropertyType != 3)
		{
			uint8 First = 0;
			if (!Reader.ReadU8(First)) return false;
			if ((First & 0x80) != 0)
			{
				if ((First & 0xc0) == 0x80)
				{
					uint8 Second = 0;
					if (!Reader.ReadU8(Second)) return false;
					ArrayIndex = (static_cast<uint32>(First & 0x7f) << 8) | Second;
				}
				else
				{
					uint8 High = 0, Middle = 0, Low = 0;
					if (!Reader.ReadU8(High) || !Reader.ReadU8(Middle)
						|| !Reader.ReadU8(Low)) return false;
					ArrayIndex = (static_cast<uint32>(High) << 16)
						| (static_cast<uint32>(Middle) << 8) | Low;
				}
			}
			else ArrayIndex = First;
		}
		const bool bAnimationProperty = Name == TEXT("AnimNext")
			|| Name == TEXT("MinFrameRate") || Name == TEXT("MaxFrameRate")
			|| Name == TEXT("PrimeCount");
		const bool bAnimationEncodingValid = ArrayIndex == 0
			&& ((Name == TEXT("AnimNext") && PropertyType == 5
				&& Size >= 1 && Size <= 5)
				|| ((Name == TEXT("MinFrameRate") || Name == TEXT("MaxFrameRate"))
					&& PropertyType == 4 && Size == 4)
				|| (Name == TEXT("PrimeCount") && PropertyType == 1 && Size == 1));
		if (bAnimationProperty && !bAnimationEncodingValid) return false;
		if (PropertyType == 3)
		{
			if (Name == TEXT("bMasked"))
			{
				if (Out.bSawMasked) return false;
				Out.bSawMasked = true;
				Out.bMasked = (Info & 0x80) != 0;
			}
			else if (Name == TEXT("bParametric"))
			{
				if (Out.bSawParametric) return false;
				Out.bSawParametric = true;
				Out.bParametric = (Info & 0x80) != 0;
			}
			continue;
		}
		if (!Reader.Require(Size)) return false;
		const uint64 ValueStart = Reader.Tell();
		if (Name == TEXT("Palette") && PropertyType == 5)
		{
			if (Out.bSawPalette || !Reader.ReadCompact(Out.Palette)) return false;
			Out.bSawPalette = true;
		}
		else if (Name == TEXT("Format") && PropertyType == 1)
		{
			if (Out.bSawFormat || !Reader.ReadU8(Out.Format)) return false;
			Out.bSawFormat = true;
		}
		else if (Name == TEXT("AnimNext") && PropertyType == 5)
		{
			if (Out.bSawAnimNext || !Reader.ReadCompact(Out.AnimNext)) return false;
			Out.bSawAnimNext = true;
		}
		else if ((Name == TEXT("MinFrameRate") || Name == TEXT("MaxFrameRate"))
			&& PropertyType == 4 && Size == 4)
		{
			uint32 Bits = 0;
			float Value = 0.0f;
			bool& bSaw = Name == TEXT("MinFrameRate")
				? Out.bSawMinFrameRate : Out.bSawMaxFrameRate;
			if (bSaw || !Reader.ReadU32(Bits)) return false;
			FMemory::Memcpy(&Value, &Bits, sizeof(Value));
			if (!FMath::IsFinite(Value) || Value < 0.0f) return false;
			bSaw = true;
			(Name == TEXT("MinFrameRate") ? Out.MinFrameRate : Out.MaxFrameRate) = Value;
		}
		else if (Name == TEXT("PrimeCount") && PropertyType == 1 && Size == 1)
		{
			if (Out.bSawPrimeCount || !Reader.ReadU8(Out.PrimeCount)) return false;
			Out.bSawPrimeCount = true;
		}
		if ((Name == TEXT("Palette") || Name == TEXT("Format")
			|| Name == TEXT("AnimNext") || Name == TEXT("MinFrameRate")
			|| Name == TEXT("MaxFrameRate") || Name == TEXT("PrimeCount"))
			&& Reader.Tell() != ValueStart + Size) return false;
		if (!Reader.SetOffset(ValueStart + Size)) return false;
	}
	return false;
}

bool WoTTextureSoundCanonicalTypedIdentity(const FWoTTextureSoundSource& Source,
	int32 Reference, FStringView ExpectedClass, FString& Out)
{
	Out.Reset();
	if (Reference == 0) return true;
	const FString* ActualClass = nullptr;
	if (Reference > 0 && Source.Exports.IsValidIndex(Reference - 1))
		ActualClass = Source.ClassName(Source.Exports[Reference - 1].ClassReference);
	else if (Reference < 0)
	{
		const int64 ImportIndex = -static_cast<int64>(Reference) - 1;
		if (ImportIndex >= 0 && ImportIndex < Source.Imports.Num())
			ActualClass = &Source.Imports[static_cast<int32>(ImportIndex)].ClassName;
	}
	if (ActualClass == nullptr
		|| !ActualClass->Equals(FString(ExpectedClass), ESearchCase::IgnoreCase))
		return false;

	TArray<FString> Components;
	TSet<int32> Seen;
	for (uint32 Depth = 0; Depth < WoTTextureSoundMaxOuterDepth; ++Depth)
	{
		if (Reference == 0) break;
		if (Seen.Contains(Reference)) return false;
		Seen.Add(Reference);
		if (Reference > 0)
		{
			if (!Source.Exports.IsValidIndex(Reference - 1)) return false;
			const FWoTTextureSoundExport& Export = Source.Exports[Reference - 1];
			Components.Add(Export.Name);
			Reference = Export.Group;
			if (Reference == 0) Components.Add(Source.PackageName);
		}
		else
		{
			const int64 ImportIndex = -static_cast<int64>(Reference) - 1;
			if (ImportIndex < 0 || ImportIndex >= Source.Imports.Num()) return false;
			const FWoTTextureSoundImport& Import =
				Source.Imports[static_cast<int32>(ImportIndex)];
			Components.Add(Import.ObjectName);
			Reference = Import.Outer;
		}
		if (Reference == 0)
		{
			for (int32 Index = Components.Num() - 1; Index >= 0; --Index)
			{
				if (!Out.IsEmpty()) Out += TEXT(".");
				Out += Components[Index];
			}
			return !Out.IsEmpty();
		}
	}
	return false;
}

bool WoTTextureSoundOpenExportReader(const FWoTTextureSoundSource& Source,
	const FWoTTextureSoundExport& Export, FWoTTextureSoundReader& Out)
{
	Out = FWoTTextureSoundReader(Source.Bytes, Export.Offset,
		Export.Offset + Export.Size);
	if ((Export.Flags & WoTTextureSoundHasStack) != 0)
	{
		int32 Node = 0;
		int32 StateNode = 0;
		if (!Out.ReadCompact(Node) || !Out.ReadCompact(StateNode) || !Out.Skip(12)
			|| (Node != 0 && !Out.ReadCompact(StateNode)))
		{
			return false;
		}
	}
	return Out.IsValid();
}

TWoTBootstrapResult<const FWoTTextureSoundExport*> WoTTextureSoundFindExport(
	const FWoTTextureSoundSource& Source, FStringView SourcePath,
	FStringView SourceExport, const TCHAR* ExpectedClass)
{
	using FResult = TWoTBootstrapResult<const FWoTTextureSoundExport*>;
	const FString PackageName = FPaths::GetBaseFilename(FString(SourcePath));
	auto FoldComponent = [](FStringView Component)
	{
		FString Folded(Component);
		for (int32 CharacterIndex = 0; CharacterIndex < Folded.Len(); ++CharacterIndex)
		{
			TCHAR& Character = Folded[CharacterIndex];
			if (Character >= TEXT('A') && Character <= TEXT('Z'))
			{
				Character += TEXT('a') - TEXT('A');
			}
		}
		return Folded;
	};
	auto FoldComponents = [&FoldComponent](const TArray<FString>& Components)
	{
		TArray<FString> Folded;
		Folded.Reserve(Components.Num());
		for (const FString& Component : Components)
		{
			Folded.Add(FoldComponent(Component));
		}
		return Folded;
	};
	auto JoinComponents = [](const TArray<FString>& Components)
	{
		FString Joined;
		for (const FString& Component : Components)
		{
			if (!Joined.IsEmpty()) Joined += TEXT(".");
			Joined += Component;
		}
		return Joined;
	};

	if (PackageName.IsEmpty()
		|| PackageName.Contains(TEXT("."), ESearchCase::CaseSensitive))
	{
		return FResult::Error(WoTTextureSoundError(EWoTBootstrapError::UnknownField,
			TEXT("the source package has an invalid UE1 identity component"),
			TEXT("source.package")));
	}
	TArray<FString> ExpectedComponents;
	FString(SourceExport).ParseIntoArray(ExpectedComponents, TEXT("."), false);
	if (ExpectedComponents.Num() < 2)
	{
		return FResult::Error(WoTTextureSoundError(EWoTBootstrapError::UnknownField,
			TEXT("the source export is not a component-qualified UE1 identity"),
			FString(SourceExport)));
	}
	for (const FString& Component : ExpectedComponents)
	{
		if (Component.IsEmpty())
		{
			return FResult::Error(WoTTextureSoundError(EWoTBootstrapError::UnknownField,
				TEXT("the source export contains an empty UE1 identity component"),
				TEXT("source.export")));
		}
	}
	const TArray<FString> ExpectedKey = FoldComponents(ExpectedComponents);
	if (ExpectedKey[0] != FoldComponent(PackageName))
	{
		return FResult::Error(WoTTextureSoundError(EWoTBootstrapError::UnknownField,
			TEXT("the source export is not qualified by its UE1 package identity"),
			FString(SourceExport)));
	}
	const FString ExpectedClassKey = FoldComponent(ExpectedClass);

	struct FMatch
	{
		const FWoTTextureSoundExport* Export = nullptr;
		int32 ExportIndex = INDEX_NONE;
		FString DisplayPath;
	};
	TArray<FMatch> Matches;
	TArray<FMatch> BareMatches;
	for (int32 ExportIndex = 0; ExportIndex < Source.Exports.Num(); ++ExportIndex)
	{
		const FWoTTextureSoundExport& Export = Source.Exports[ExportIndex];
		const FString* ClassName = Source.ClassName(Export.ClassReference);
		if (ClassName == nullptr || FoldComponent(*ClassName) != ExpectedClassKey)
		{
			continue;
		}

		TArray<FString> Components;
		TSet<int32> Seen;
		int32 Reference = ExportIndex + 1;
		bool bComplete = false;
		for (uint32 Depth = 0; Depth < WoTTextureSoundMaxOuterDepth; ++Depth)
		{
			if (Reference == 0)
			{
				bComplete = true;
				break;
			}
			if (Seen.Contains(Reference))
			{
				return FResult::Error(WoTTextureSoundError(
					EWoTBootstrapError::CorruptInstallation,
					TEXT("the UE1 export outer chain contains a cycle"),
					FString(SourceExport)));
			}
			Seen.Add(Reference);
			if (Reference > 0)
			{
				if (!Source.Exports.IsValidIndex(Reference - 1))
				{
					return FResult::Error(WoTTextureSoundError(
						EWoTBootstrapError::CorruptInstallation,
						TEXT("the UE1 export outer reference is outside the export table"),
						FString(SourceExport)));
				}
				const FWoTTextureSoundExport& OuterExport =
					Source.Exports[Reference - 1];
				Components.Add(OuterExport.Name);
				Reference = OuterExport.Group;
				if (Reference == 0)
				{
					Components.Add(PackageName);
					bComplete = true;
					break;
				}
			}
			else
			{
				const int64 ImportIndex = -static_cast<int64>(Reference) - 1;
				if (ImportIndex < 0 || ImportIndex >= Source.Imports.Num())
				{
					return FResult::Error(WoTTextureSoundError(
						EWoTBootstrapError::CorruptInstallation,
						TEXT("the UE1 import outer reference is outside the import table"),
						FString(SourceExport)));
				}
				const FWoTTextureSoundImport& Import =
					Source.Imports[static_cast<int32>(ImportIndex)];
				Components.Add(Import.ObjectName);
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
			return FResult::Error(WoTTextureSoundError(
				EWoTBootstrapError::SizeLimit,
				TEXT("the UE1 export outer chain exceeds its depth bound"),
				FString(SourceExport)));
		}
		TArray<FString> OrderedComponents;
		OrderedComponents.Reserve(Components.Num());
		for (int32 ComponentIndex = Components.Num() - 1; ComponentIndex >= 0;
			--ComponentIndex)
		{
			OrderedComponents.Add(Components[ComponentIndex]);
		}
		const TArray<FString> CandidateKey = FoldComponents(OrderedComponents);
		FMatch Candidate;
		Candidate.Export = &Export;
		Candidate.ExportIndex = ExportIndex + 1;
		Candidate.DisplayPath = JoinComponents(OrderedComponents);
		if (CandidateKey == ExpectedKey)
		{
			Matches.Add(Candidate);
		}
		if (ExpectedComponents.Num() == 2 && !CandidateKey.IsEmpty()
			&& CandidateKey.Last() == ExpectedKey.Last())
		{
			BareMatches.Add(MoveTemp(Candidate));
		}
	}
	auto AmbiguousError = [SourceExport](const TArray<FMatch>& Candidates,
		const TCHAR* Prefix)
	{
		const FMatch& First = Candidates[0];
		const FMatch& Second = Candidates[1];
		const FString Message = FString::Printf(
			TEXT("%s between export[%d] '%s' and export[%d] '%s'"), Prefix,
			First.ExportIndex, *First.DisplayPath, Second.ExportIndex, *Second.DisplayPath);
		return WoTTextureSoundError(EWoTBootstrapError::DuplicateField,
			*Message, FString(SourceExport));
	};
	if (Matches.Num() > 1)
	{
		return FResult::Error(AmbiguousError(Matches,
			TEXT("the source export identity is ambiguous")));
	}
	if (Matches.Num() == 1)
	{
		return FResult::Success(Matches[0].Export);
	}
	if (ExpectedComponents.Num() == 2 && BareMatches.Num() > 1)
	{
		return FResult::Error(AmbiguousError(BareMatches,
			TEXT("the unqualified source export identity is ambiguous")));
	}
	return FResult::Error(WoTTextureSoundError(EWoTBootstrapError::UnknownField,
		TEXT("the exact typed source export was not found"), FString(SourceExport)));
}

void WoTTextureSoundAppendU16(TArray<uint8>& Bytes, uint16 Value)
{
	Bytes.Add(static_cast<uint8>(Value));
	Bytes.Add(static_cast<uint8>(Value >> 8));
}

void WoTTextureSoundAppendU32(TArray<uint8>& Bytes, uint32 Value)
{
	Bytes.Add(static_cast<uint8>(Value));
	Bytes.Add(static_cast<uint8>(Value >> 8));
	Bytes.Add(static_cast<uint8>(Value >> 16));
	Bytes.Add(static_cast<uint8>(Value >> 24));
}

TWoTBootstrapResult<TArray<uint8>> WoTTextureSoundFrameObject(
	const FWoTCanonicalJsonValue& Header, TConstArrayView<uint8> Payload)
{
	using FResult = TWoTBootstrapResult<TArray<uint8>>;
	TWoTBootstrapResult<TArray<uint8>> HeaderBytes = WriteCanonicalJson(Header);
	if (HeaderBytes.IsError())
	{
		return FResult::Error(MoveTemp(HeaderBytes).GetError());
	}
	if (HeaderBytes.GetValue().IsEmpty()
		|| HeaderBytes.GetValue().Num() > static_cast<int32>(WoTTextureSoundMaxHeaderBytes)
		|| Payload.Num() > static_cast<int32>(WoTTextureSoundMaxSourceBytes))
	{
		return FResult::Error(WoTTextureSoundError(EWoTBootstrapError::SizeLimit,
			TEXT("the cache-v3 typed object exceeds its framing bound")));
	}
	TArray<uint8> Object;
	Object.Reserve(12 + HeaderBytes.GetValue().Num() + Payload.Num());
	Object.Append(reinterpret_cast<const uint8*>(WoTTextureSoundObjectMagic), 8);
	WoTTextureSoundAppendU32(Object, static_cast<uint32>(HeaderBytes.GetValue().Num()));
	Object.Append(HeaderBytes.GetValue());
	Object.Append(Payload.GetData(), Payload.Num());
	return FResult::Success(MoveTemp(Object));
}

TWoTBootstrapResult<FWoTCanonicalJsonValue> WoTTextureSoundSourceJson(
	const FWoTRequiredFact& Fact,
	const FWoTTextureSoundSource& Source)
{
	FWoTCanonicalObjectBuilder Builder;
	Builder.Add(TEXT("export"),
		FWoTCanonicalJsonValue::MakeString(Fact.SourceExport));
	Builder.Add(TEXT("package"),
		FWoTCanonicalJsonValue::MakeString(Fact.SourcePath));
	Builder.Add(TEXT("sha256"),
		FWoTCanonicalJsonValue::MakeString(Source.Sha256.ToHex()));
	return MoveTemp(Builder).Build(TEXT("typed object source"));
}

struct FWoTTextureSoundMip
{
	uint32 Width = 0;
	uint32 Height = 0;
	TArray<uint8> Indices;
};

struct FWoTTextureSoundTexture
{
	bool bMasked = false;
	FString AnimNext;
	float MinFrameRate = 0.0f;
	float MaxFrameRate = 0.0f;
	uint8 PrimeCount = 0;
	TArray<uint8> Palette;
	TArray<FWoTTextureSoundMip> Mips;
};

TWoTBootstrapResult<FWoTTextureSoundTexture> WoTTextureSoundDecodeTexture(
	const FWoTTextureSoundSource& Source, const FWoTTextureSoundExport& Export,
	FStringView SourcePath)
{
	using FResult = TWoTBootstrapResult<FWoTTextureSoundTexture>;
	FWoTTextureSoundReader Reader(Source.Bytes);
	if (!WoTTextureSoundOpenExportReader(Source, Export, Reader))
	{
		return FResult::Error(WoTTextureSoundError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the texture export stack is malformed"), FString(SourcePath)));
	}
	FWoTTextureSoundProperties Properties;
	if (!WoTTextureSoundReadProperties(Reader, Source, Properties)
		|| Properties.bParametric || !Properties.bSawPalette || Properties.Palette <= 0
		|| Properties.Format != 0)
	{
		return FResult::Error(WoTTextureSoundError(
			Properties.Format != 0 ? EWoTBootstrapError::UnsupportedProtocol
				: EWoTBootstrapError::CorruptInstallation,
			Properties.Format != 0 ? TEXT("the texture format is not UE1 P8")
				: TEXT("the texture properties are incomplete or unsupported"),
			FString(SourcePath)));
	}
	if (!Source.Exports.IsValidIndex(Properties.Palette - 1))
	{
		return FResult::Error(WoTTextureSoundError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the texture palette reference is outside the export table"),
			FString(SourcePath)));
	}
	const FWoTTextureSoundExport& PaletteExport = Source.Exports[Properties.Palette - 1];
	const FString* PaletteClass = Source.ClassName(PaletteExport.ClassReference);
	FWoTTextureSoundReader PaletteReader(Source.Bytes);
	FWoTTextureSoundProperties PaletteProperties;
	int32 PaletteCount = 0;
	FWoTTextureSoundTexture Texture;
	if (!WoTTextureSoundCanonicalTypedIdentity(Source, Properties.AnimNext,
			TEXT("Texture"), Texture.AnimNext)
		|| Properties.MaxFrameRate < Properties.MinFrameRate)
	{
		return FResult::Error(WoTTextureSoundError(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("the texture animation properties are malformed"), FString(SourcePath)));
	}
	Texture.MinFrameRate = Properties.MinFrameRate;
	Texture.MaxFrameRate = Properties.MaxFrameRate;
	Texture.PrimeCount = Properties.PrimeCount;
	if (PaletteClass == nullptr || *PaletteClass != TEXT("Palette")
		|| !WoTTextureSoundOpenExportReader(Source, PaletteExport, PaletteReader)
		|| !WoTTextureSoundReadProperties(PaletteReader, Source, PaletteProperties)
		|| !PaletteReader.ReadCompact(PaletteCount) || PaletteCount != 256
		|| !PaletteReader.ReadBytes(256 * 4, Texture.Palette)
		|| PaletteReader.Tell() != PaletteReader.GetEnd())
	{
		return FResult::Error(WoTTextureSoundError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the texture palette is not exactly 256 RGBA colors"),
			FString(SourcePath)));
	}

	int32 MipCount = 0;
	if (!Reader.ReadCompact(MipCount) || MipCount <= 0
		|| MipCount > static_cast<int32>(WoTTextureSoundMaxMips))
	{
		return FResult::Error(WoTTextureSoundError(EWoTBootstrapError::SizeLimit,
			TEXT("the texture has an invalid mip count"), FString(SourcePath)));
	}
	Texture.bMasked = Properties.bMasked;
	Texture.Mips.Reserve(MipCount);
	int32 PreviousWidth = 0;
	int32 PreviousHeight = 0;
	for (int32 MipIndex = 0; MipIndex < MipCount; ++MipIndex)
	{
		int32 LazyEnd = 0;
		int32 IndexCount = 0;
		int32 Width = 0;
		int32 Height = 0;
		uint8 UBits = 0;
		uint8 VBits = 0;
		if ((Source.Version > 61 && (!Reader.ReadI32(LazyEnd) || LazyEnd < 0))
			|| !Reader.ReadCompact(IndexCount) || IndexCount < 0
			|| static_cast<uint64>(IndexCount)
				> static_cast<uint64>(WoTTextureSoundMaxDimension)
					* static_cast<uint64>(WoTTextureSoundMaxDimension))
		{
			return FResult::Error(WoTTextureSoundError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("the texture mip tuple is malformed"), FString(SourcePath)));
		}
		FWoTTextureSoundMip& Mip = Texture.Mips.AddDefaulted_GetRef();
		if (!Reader.ReadBytes(static_cast<uint64>(IndexCount), Mip.Indices))
		{
			return FResult::Error(WoTTextureSoundError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("the texture mip indices are truncated"), FString(SourcePath)));
		}
		if (Source.Version > 61
			&& static_cast<uint64>(LazyEnd) != Reader.Tell())
		{
			return FResult::Error(WoTTextureSoundError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("the texture mip lazy end does not match its serialized indices"),
				FString(SourcePath)));
		}
		if (!Reader.ReadI32(Width) || !Reader.ReadI32(Height)
			|| !Reader.ReadU8(UBits) || !Reader.ReadU8(VBits)
			|| Width <= 0 || Height <= 0
			|| Width > WoTTextureSoundMaxDimension
			|| Height > WoTTextureSoundMaxDimension)
		{
			return FResult::Error(WoTTextureSoundError(EWoTBootstrapError::SizeLimit,
				TEXT("the texture mip dimensions are outside the v2 bound"),
				FString(SourcePath)));
		}
		const uint64 Expected = static_cast<uint64>(Width) * static_cast<uint64>(Height);
		if (Expected != static_cast<uint64>(IndexCount)
			|| UBits >= 31 || VBits >= 31
			|| Width != (1 << UBits) || Height != (1 << VBits))
		{
			return FResult::Error(WoTTextureSoundError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("the texture mip count/dimensions/log2 bits disagree"),
				FString(SourcePath)));
		}
		if (MipIndex > 0
			&& (Width != FMath::Max(PreviousWidth >> 1, 1)
				|| Height != FMath::Max(PreviousHeight >> 1, 1)))
		{
			return FResult::Error(WoTTextureSoundError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("the texture mip successor dimensions are not contiguous"),
				FString(SourcePath)));
		}
		Mip.Width = static_cast<uint32>(Width);
		Mip.Height = static_cast<uint32>(Height);
		PreviousWidth = Width;
		PreviousHeight = Height;
	}
	if (Reader.Tell() != Reader.GetEnd())
	{
		return FResult::Error(WoTTextureSoundError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the texture export has trailing serialized bytes"), FString(SourcePath)));
	}
	return FResult::Success(MoveTemp(Texture));
}

TArray<uint8> WoTTextureSoundEncodeTexturePayload(
	const FWoTTextureSoundTexture& Texture)
{
	TArray<uint8> Payload;
	uint64 PayloadSize = 20 + 256 * 4;
	for (const FWoTTextureSoundMip& Mip : Texture.Mips)
	{
		PayloadSize += 12 + static_cast<uint64>(Mip.Indices.Num());
	}
	if (PayloadSize > static_cast<uint64>(TNumericLimits<int32>::Max())) return {};
	Payload.Reserve(static_cast<int32>(PayloadSize));
	Payload.Append(reinterpret_cast<const uint8*>(WoTTextureSoundTextureMagic), 8);
	WoTTextureSoundAppendU16(Payload, 1);
	WoTTextureSoundAppendU16(Payload, Texture.bMasked ? 1 : 0);
	WoTTextureSoundAppendU32(Payload, 256);
	WoTTextureSoundAppendU32(Payload, static_cast<uint32>(Texture.Mips.Num()));
	Payload.Append(Texture.Palette);
	for (const FWoTTextureSoundMip& Mip : Texture.Mips)
	{
		WoTTextureSoundAppendU32(Payload, Mip.Width);
		WoTTextureSoundAppendU32(Payload, Mip.Height);
		WoTTextureSoundAppendU32(Payload, static_cast<uint32>(Mip.Indices.Num()));
		Payload.Append(Mip.Indices);
	}
	return Payload;
}

TWoTBootstrapResult<TArray<uint8>> WoTTextureSoundBuildTextureObject(
	const FWoTRequiredFact& Fact, const FWoTTextureSoundSource& Source,
	const FWoTTextureSoundExport& Export)
{
	using FResult = TWoTBootstrapResult<TArray<uint8>>;
	TWoTBootstrapResult<FWoTTextureSoundTexture> Decoded =
		WoTTextureSoundDecodeTexture(Source, Export, Fact.SourcePath);
	if (Decoded.IsError()) return FResult::Error(MoveTemp(Decoded).GetError());
	const FWoTTextureSoundTexture& Texture = Decoded.GetValue();
	TArray<uint8> Payload = WoTTextureSoundEncodeTexturePayload(Texture);
	if (Payload.IsEmpty())
	{
		return FResult::Error(WoTTextureSoundError(EWoTBootstrapError::SizeLimit,
			TEXT("the texture payload cannot be represented"), Fact.SourcePath));
	}

	FWoTCanonicalObjectBuilder ColorBuilder;
	ColorBuilder.Add(TEXT("alpha"),
		FWoTCanonicalJsonValue::MakeString(TEXT("index-zero-if-masked")));
	ColorBuilder.Add(TEXT("channel_order"),
		FWoTCanonicalJsonValue::MakeString(TEXT("rgba")));
	ColorBuilder.Add(TEXT("palette_colors"),
		FWoTCanonicalJsonValue::MakeInteger(256));
	ColorBuilder.Add(TEXT("stored_palette_alpha"),
		FWoTCanonicalJsonValue::MakeString(
			TEXT("preserved-but-non-semantic")));
	TWoTBootstrapResult<FWoTCanonicalJsonValue> BuiltColor =
		MoveTemp(ColorBuilder).Build(TEXT("texture color"));
	if (BuiltColor.IsError()) return FResult::Error(MoveTemp(BuiltColor).GetError());
	FWoTCanonicalJsonValue Mips = FWoTCanonicalJsonValue::MakeArray();
	for (const FWoTTextureSoundMip& Mip : Texture.Mips)
	{
		FWoTCanonicalObjectBuilder MipBuilder;
		MipBuilder.Add(TEXT("expanded_rgba_byte_length"),
			FWoTCanonicalJsonValue::MakeInteger(
				static_cast<int64>(Mip.Width) * static_cast<int64>(Mip.Height) * 4));
		MipBuilder.Add(TEXT("height"),
			FWoTCanonicalJsonValue::MakeInteger(Mip.Height));
		MipBuilder.Add(TEXT("index_byte_length"),
			FWoTCanonicalJsonValue::MakeInteger(Mip.Indices.Num()));
		MipBuilder.Add(TEXT("width"),
			FWoTCanonicalJsonValue::MakeInteger(Mip.Width));
		TWoTBootstrapResult<FWoTCanonicalJsonValue> BuiltMip =
			MoveTemp(MipBuilder).Build(TEXT("texture mip"));
		if (BuiltMip.IsError()) return FResult::Error(MoveTemp(BuiltMip).GetError());
		Mips.AddElement(MoveTemp(BuiltMip).GetValue());
	}
	FWoTCanonicalObjectBuilder LayoutBuilder;
	LayoutBuilder.Add(TEXT("mip_count"),
		FWoTCanonicalJsonValue::MakeInteger(Texture.Mips.Num()));
	LayoutBuilder.Add(TEXT("mips"), MoveTemp(Mips));
	LayoutBuilder.Add(TEXT("texel_format"),
		FWoTCanonicalJsonValue::MakeString(TEXT("p8")));
	TWoTBootstrapResult<FWoTCanonicalJsonValue> BuiltLayout =
		MoveTemp(LayoutBuilder).Build(TEXT("texture decoded layout"));
	if (BuiltLayout.IsError()) return FResult::Error(MoveTemp(BuiltLayout).GetError());
	TWoTBootstrapResult<FWoTCanonicalJsonValue> BuiltSource =
		WoTTextureSoundSourceJson(Fact, Source);
	if (BuiltSource.IsError()) return FResult::Error(MoveTemp(BuiltSource).GetError());
	FWoTCanonicalObjectBuilder HeaderBuilder;
	HeaderBuilder.Add(TEXT("anim_next"),
		FWoTCanonicalJsonValue::MakeString(Texture.AnimNext));
	HeaderBuilder.Add(TEXT("color"), MoveTemp(BuiltColor).GetValue());
	HeaderBuilder.Add(TEXT("decoded_layout"), MoveTemp(BuiltLayout).GetValue());
	HeaderBuilder.Add(TEXT("encoding"), FWoTCanonicalJsonValue::MakeString(
		TEXT("ue1-p8-mips-rgba-palette-v1")));
	HeaderBuilder.Add(TEXT("fact_id"), FWoTCanonicalJsonValue::MakeString(Fact.FactId));
	HeaderBuilder.Add(TEXT("importer"),
		FWoTCanonicalJsonValue::MakeString(TEXT("texture-v2")));
	HeaderBuilder.Add(TEXT("importer_version"), FWoTCanonicalJsonValue::MakeInteger(2));
	HeaderBuilder.Add(TEXT("kind"),
		FWoTCanonicalJsonValue::MakeString(TEXT("texture")));
	HeaderBuilder.Add(TEXT("masked"), FWoTCanonicalJsonValue::MakeBool(Texture.bMasked));
	uint32 MinFrameRateBits = 0, MaxFrameRateBits = 0;
	FMemory::Memcpy(&MinFrameRateBits, &Texture.MinFrameRate, sizeof(uint32));
	FMemory::Memcpy(&MaxFrameRateBits, &Texture.MaxFrameRate, sizeof(uint32));
	HeaderBuilder.Add(TEXT("max_frame_rate_bits"),
		FWoTCanonicalJsonValue::MakeInteger(MaxFrameRateBits));
	HeaderBuilder.Add(TEXT("min_frame_rate_bits"),
		FWoTCanonicalJsonValue::MakeInteger(MinFrameRateBits));
	HeaderBuilder.Add(TEXT("payload_byte_length"),
		FWoTCanonicalJsonValue::MakeInteger(Payload.Num()));
	HeaderBuilder.Add(TEXT("prime_count"),
		FWoTCanonicalJsonValue::MakeInteger(Texture.PrimeCount));
	HeaderBuilder.Add(TEXT("source"), MoveTemp(BuiltSource).GetValue());
	TWoTBootstrapResult<FWoTCanonicalJsonValue> BuiltHeader =
		MoveTemp(HeaderBuilder).Build(TEXT("texture header"));
	if (BuiltHeader.IsError()) return FResult::Error(MoveTemp(BuiltHeader).GetError());
	return WoTTextureSoundFrameObject(BuiltHeader.GetValue(), Payload);
}

struct FWoTTextureSoundStream
{
	FString DeclaredFormat;
	TArray<uint8> Bytes;
};

TWoTBootstrapResult<FWoTTextureSoundStream> WoTTextureSoundDecodeStream(
	const FWoTTextureSoundSource& Source, const FWoTTextureSoundExport& Export,
	FStringView SourcePath)
{
	using FResult = TWoTBootstrapResult<FWoTTextureSoundStream>;
	FWoTTextureSoundReader Reader(Source.Bytes);
	FWoTTextureSoundProperties Properties;
	FWoTTextureSoundStream Stream;
	if (!WoTTextureSoundOpenExportReader(Source, Export, Reader)
		|| !WoTTextureSoundReadProperties(Reader, Source, Properties)
		|| !WoTTextureSoundReadName(Reader, Source, Stream.DeclaredFormat))
	{
		return FResult::Error(WoTTextureSoundError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the UE1 stream export header is malformed"), FString(SourcePath)));
	}
	if (Source.Version > 61)
	{
		int32 DataEnd = 0;
		if (!Reader.ReadI32(DataEnd) || DataEnd < 0
			|| static_cast<uint64>(DataEnd) != Reader.GetEnd())
		{
			return FResult::Error(WoTTextureSoundError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("the UE1 lazy stream end does not match the export"), FString(SourcePath)));
		}
	}
	int32 DataSize = 0;
	if (!Reader.ReadCompact(DataSize) || DataSize < 0
		|| static_cast<uint64>(DataSize) != Reader.Remaining()
		|| !Reader.ReadBytes(static_cast<uint64>(DataSize), Stream.Bytes)
		|| Stream.Bytes.IsEmpty())
	{
		return FResult::Error(WoTTextureSoundError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the UE1 stream byte count is invalid"), FString(SourcePath)));
	}
	return FResult::Success(MoveTemp(Stream));
}

uint16 WoTTextureSoundU16At(TConstArrayView<uint8> Bytes, uint64 Offset)
{
	return static_cast<uint16>(Bytes[static_cast<int32>(Offset)])
		| static_cast<uint16>(
			static_cast<uint16>(Bytes[static_cast<int32>(Offset + 1)]) << 8);
}

uint32 WoTTextureSoundU32At(TConstArrayView<uint8> Bytes, uint64 Offset)
{
	return static_cast<uint32>(Bytes[static_cast<int32>(Offset)])
		| (static_cast<uint32>(Bytes[static_cast<int32>(Offset + 1)]) << 8)
		| (static_cast<uint32>(Bytes[static_cast<int32>(Offset + 2)]) << 16)
		| (static_cast<uint32>(Bytes[static_cast<int32>(Offset + 3)]) << 24);
}

bool WoTTextureSoundChunkIs(TConstArrayView<uint8> Bytes, uint64 Offset,
	const ANSICHAR* Id)
{
	return Offset + 4 <= static_cast<uint64>(Bytes.Num())
		&& FMemory::Memcmp(Bytes.GetData() + Offset, Id, 4) == 0;
}

struct FWoTTextureSoundWaveLoop
{
	uint32 Start = 0;
	uint32 EndExclusive = 0;
	uint32 PlayCount = 0;
};

struct FWoTTextureSoundWave
{
	uint16 FormatTag = 0;
	uint16 Channels = 0;
	uint32 SampleRate = 0;
	uint16 BitsPerSample = 0;
	uint64 FrameCount = 0;
	uint64 DataByteLength = 0;
	TArray<FWoTTextureSoundWaveLoop> Loops;
};

TWoTBootstrapResult<FWoTTextureSoundWave> WoTTextureSoundParseWave(
	TConstArrayView<uint8> Bytes, FStringView Path)
{
	using FResult = TWoTBootstrapResult<FWoTTextureSoundWave>;
	if (Bytes.Num() < 12 || !WoTTextureSoundChunkIs(Bytes, 0, "RIFF")
		|| !WoTTextureSoundChunkIs(Bytes, 8, "WAVE")
		|| WoTTextureSoundU32At(Bytes, 4) != static_cast<uint32>(Bytes.Num() - 8))
	{
		return FResult::Error(WoTTextureSoundError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the sound has an invalid WAV RIFF header"), FString(Path)));
	}
	FWoTTextureSoundWave Wave;
	bool bSawFormat = false;
	bool bSawData = false;
	bool bSawSampler = false;
	uint64 Offset = 12;
	while (Offset < static_cast<uint64>(Bytes.Num()))
	{
		if (Offset + 8 > static_cast<uint64>(Bytes.Num()))
		{
			return FResult::Error(WoTTextureSoundError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("the WAV chunk header is truncated"), FString(Path)));
		}
		const uint32 ChunkSize = WoTTextureSoundU32At(Bytes, Offset + 4);
		const uint64 ChunkStart = Offset + 8;
		const uint64 ChunkEnd = ChunkStart + static_cast<uint64>(ChunkSize);
		const uint64 PaddedEnd = ChunkEnd + (ChunkSize & 1u);
		if (ChunkEnd < ChunkStart || PaddedEnd > static_cast<uint64>(Bytes.Num()))
		{
			return FResult::Error(WoTTextureSoundError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("a WAV chunk extends outside the source"), FString(Path)));
		}
		if (WoTTextureSoundChunkIs(Bytes, Offset, "fmt "))
		{
			if (bSawFormat || ChunkSize < 16)
			{
				return FResult::Error(WoTTextureSoundError(
					EWoTBootstrapError::CorruptInstallation,
					TEXT("the WAV fmt chunk is duplicate or short"), FString(Path)));
			}
			bSawFormat = true;
			Wave.FormatTag = WoTTextureSoundU16At(Bytes, ChunkStart);
			Wave.Channels = WoTTextureSoundU16At(Bytes, ChunkStart + 2);
			Wave.SampleRate = WoTTextureSoundU32At(Bytes, ChunkStart + 4);
			const uint32 ByteRate = WoTTextureSoundU32At(Bytes, ChunkStart + 8);
			const uint16 BlockAlign = WoTTextureSoundU16At(Bytes, ChunkStart + 12);
			Wave.BitsPerSample = WoTTextureSoundU16At(Bytes, ChunkStart + 14);
			const uint32 BytesPerSample = Wave.BitsPerSample / 8;
			const uint32 ExpectedAlign = static_cast<uint32>(Wave.Channels) * BytesPerSample;
			if (Wave.FormatTag != 1 || Wave.Channels < 1 || Wave.Channels > 8
				|| Wave.SampleRate < 1 || Wave.SampleRate > 384000
				|| (Wave.BitsPerSample != 8 && Wave.BitsPerSample != 16)
				|| BlockAlign != ExpectedAlign
				|| ByteRate != Wave.SampleRate * ExpectedAlign)
			{
				return FResult::Error(WoTTextureSoundError(
					EWoTBootstrapError::UnsupportedProtocol,
					TEXT("the WAV PCM format/rate/channel alignment is unsupported"),
					FString(Path)));
			}
		}
		else if (WoTTextureSoundChunkIs(Bytes, Offset, "data"))
		{
			if (bSawData)
			{
				return FResult::Error(WoTTextureSoundError(
					EWoTBootstrapError::CorruptInstallation,
					TEXT("the WAV data chunk is duplicated"), FString(Path)));
			}
			bSawData = true;
			Wave.DataByteLength = ChunkSize;
		}
		else if (WoTTextureSoundChunkIs(Bytes, Offset, "smpl"))
		{
			if (bSawSampler || ChunkSize < 36)
			{
				return FResult::Error(WoTTextureSoundError(
					EWoTBootstrapError::CorruptInstallation,
					TEXT("the WAV sampler chunk is duplicate or short"), FString(Path)));
			}
			bSawSampler = true;
			const uint32 LoopCount = WoTTextureSoundU32At(Bytes, ChunkStart + 28);
			const uint32 SamplerBytes = WoTTextureSoundU32At(Bytes, ChunkStart + 32);
			const uint64 Required = 36ull + static_cast<uint64>(LoopCount) * 24ull
				+ static_cast<uint64>(SamplerBytes);
			if (LoopCount > WoTTextureSoundMaxSamplerLoops || Required > ChunkSize)
			{
				return FResult::Error(WoTTextureSoundError(
					EWoTBootstrapError::UnsupportedProtocol,
					TEXT("the WAV sampler loop set is unsupported"), FString(Path)));
			}
			Wave.Loops.Reserve(static_cast<int32>(LoopCount));
			for (uint32 LoopIndex = 0; LoopIndex < LoopCount; ++LoopIndex)
			{
				const uint64 LoopOffset = ChunkStart + 36
					+ static_cast<uint64>(LoopIndex) * 24;
				const uint32 LoopType = WoTTextureSoundU32At(Bytes, LoopOffset + 4);
				FWoTTextureSoundWaveLoop& Loop =
					Wave.Loops.AddDefaulted_GetRef();
				Loop.Start = WoTTextureSoundU32At(Bytes, LoopOffset + 8);
				const uint32 InclusiveEnd = WoTTextureSoundU32At(Bytes, LoopOffset + 12);
				Loop.PlayCount = WoTTextureSoundU32At(Bytes, LoopOffset + 20);
				if (LoopType != 0 || InclusiveEnd == TNumericLimits<uint32>::Max())
				{
					return FResult::Error(WoTTextureSoundError(
						EWoTBootstrapError::UnsupportedProtocol,
						TEXT("the WAV loop type/range is unsupported"), FString(Path)));
				}
				Loop.EndExclusive = InclusiveEnd + 1;
			}
		}
		Offset = PaddedEnd;
	}
	const uint32 BlockAlign = static_cast<uint32>(Wave.Channels)
		* static_cast<uint32>(Wave.BitsPerSample / 8);
	if (!bSawFormat || !bSawData || BlockAlign == 0
		|| Wave.DataByteLength % BlockAlign != 0)
	{
		return FResult::Error(WoTTextureSoundError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the WAV requires frame-aligned fmt and data chunks"), FString(Path)));
	}
	Wave.FrameCount = Wave.DataByteLength / BlockAlign;
	for (const FWoTTextureSoundWaveLoop& Loop : Wave.Loops)
	{
		if (Loop.Start >= Loop.EndExclusive
			|| static_cast<uint64>(Loop.EndExclusive) > Wave.FrameCount)
		{
			return FResult::Error(WoTTextureSoundError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("the WAV loop points are outside the decoded frames"),
				FString(Path)));
		}
	}
	return FResult::Success(Wave);
}

TWoTBootstrapResult<TArray<uint8>> WoTTextureSoundBuildSoundObject(
	const FWoTRequiredFact& Fact, const FWoTTextureSoundSource& Source,
	const FWoTTextureSoundExport& Export)
{
	using FResult = TWoTBootstrapResult<TArray<uint8>>;
	TWoTBootstrapResult<FWoTTextureSoundStream> Decoded =
		WoTTextureSoundDecodeStream(Source, Export, Fact.SourcePath);
	if (Decoded.IsError()) return FResult::Error(MoveTemp(Decoded).GetError());
	if (!Decoded.GetValue().DeclaredFormat.Equals(TEXT("WAV"), ESearchCase::IgnoreCase))
	{
		return FResult::Error(WoTTextureSoundError(EWoTBootstrapError::UnsupportedProtocol,
			TEXT("the sound export is not declared as WAV"), Fact.SourcePath));
	}
	TWoTBootstrapResult<FWoTTextureSoundWave> Parsed =
		WoTTextureSoundParseWave(Decoded.GetValue().Bytes, Fact.SourcePath);
	if (Parsed.IsError()) return FResult::Error(MoveTemp(Parsed).GetError());
	const FWoTTextureSoundWave& Wave = Parsed.GetValue();

	FWoTCanonicalObjectBuilder AudioBuilder;
	AudioBuilder.Add(TEXT("bits_per_sample"),
		FWoTCanonicalJsonValue::MakeInteger(Wave.BitsPerSample));
	AudioBuilder.Add(TEXT("channels"),
		FWoTCanonicalJsonValue::MakeInteger(Wave.Channels));
	AudioBuilder.Add(TEXT("format_tag"),
		FWoTCanonicalJsonValue::MakeInteger(Wave.FormatTag));
	AudioBuilder.Add(TEXT("sample_rate"),
		FWoTCanonicalJsonValue::MakeInteger(Wave.SampleRate));
	TWoTBootstrapResult<FWoTCanonicalJsonValue> BuiltAudio =
		MoveTemp(AudioBuilder).Build(TEXT("sound audio"));
	if (BuiltAudio.IsError()) return FResult::Error(MoveTemp(BuiltAudio).GetError());
	FWoTCanonicalObjectBuilder LayoutBuilder;
	LayoutBuilder.Add(TEXT("data_byte_length"),
		FWoTCanonicalJsonValue::MakeInteger(static_cast<int64>(Wave.DataByteLength)));
	LayoutBuilder.Add(TEXT("frame_count"),
		FWoTCanonicalJsonValue::MakeInteger(static_cast<int64>(Wave.FrameCount)));
	LayoutBuilder.Add(TEXT("interleaved"), FWoTCanonicalJsonValue::MakeBool(true));
	TWoTBootstrapResult<FWoTCanonicalJsonValue> BuiltLayout =
		MoveTemp(LayoutBuilder).Build(TEXT("sound decoded layout"));
	if (BuiltLayout.IsError()) return FResult::Error(MoveTemp(BuiltLayout).GetError());
	FWoTCanonicalObjectBuilder DurationBuilder;
	DurationBuilder.Add(TEXT("frames"),
		FWoTCanonicalJsonValue::MakeInteger(static_cast<int64>(Wave.FrameCount)));
	DurationBuilder.Add(TEXT("sample_rate"),
		FWoTCanonicalJsonValue::MakeInteger(Wave.SampleRate));
	TWoTBootstrapResult<FWoTCanonicalJsonValue> BuiltDuration =
		MoveTemp(DurationBuilder).Build(TEXT("sound duration"));
	if (BuiltDuration.IsError()) return FResult::Error(MoveTemp(BuiltDuration).GetError());
	FWoTCanonicalJsonValue Loops = FWoTCanonicalJsonValue::MakeArray();
	for (const FWoTTextureSoundWaveLoop& Loop : Wave.Loops)
	{
		FWoTCanonicalObjectBuilder LoopBuilder;
		LoopBuilder.Add(TEXT("end_frame_exclusive"),
			FWoTCanonicalJsonValue::MakeInteger(Loop.EndExclusive));
		LoopBuilder.Add(TEXT("play_count"),
			FWoTCanonicalJsonValue::MakeInteger(Loop.PlayCount));
		LoopBuilder.Add(TEXT("start_frame"),
			FWoTCanonicalJsonValue::MakeInteger(Loop.Start));
		TWoTBootstrapResult<FWoTCanonicalJsonValue> BuiltLoop =
			MoveTemp(LoopBuilder).Build(TEXT("sound loop"));
		if (BuiltLoop.IsError()) return FResult::Error(MoveTemp(BuiltLoop).GetError());
		Loops.AddElement(MoveTemp(BuiltLoop).GetValue());
	}
	TWoTBootstrapResult<FWoTCanonicalJsonValue> BuiltSource =
		WoTTextureSoundSourceJson(Fact, Source);
	if (BuiltSource.IsError()) return FResult::Error(MoveTemp(BuiltSource).GetError());
	FWoTCanonicalObjectBuilder HeaderBuilder;
	HeaderBuilder.Add(TEXT("audio"), MoveTemp(BuiltAudio).GetValue());
	HeaderBuilder.Add(TEXT("decoded_layout"), MoveTemp(BuiltLayout).GetValue());
	HeaderBuilder.Add(TEXT("duration"), MoveTemp(BuiltDuration).GetValue());
	HeaderBuilder.Add(TEXT("encoding"),
		FWoTCanonicalJsonValue::MakeString(TEXT("wav-v1")));
	HeaderBuilder.Add(TEXT("fact_id"), FWoTCanonicalJsonValue::MakeString(Fact.FactId));
	HeaderBuilder.Add(TEXT("importer"),
		FWoTCanonicalJsonValue::MakeString(TEXT("sound-v1")));
	HeaderBuilder.Add(TEXT("importer_version"), FWoTCanonicalJsonValue::MakeInteger(1));
	HeaderBuilder.Add(TEXT("kind"),
		FWoTCanonicalJsonValue::MakeString(TEXT("sound")));
	HeaderBuilder.Add(TEXT("loops"), MoveTemp(Loops));
	HeaderBuilder.Add(TEXT("payload_byte_length"),
		FWoTCanonicalJsonValue::MakeInteger(Decoded.GetValue().Bytes.Num()));
	HeaderBuilder.Add(TEXT("source"), MoveTemp(BuiltSource).GetValue());
	TWoTBootstrapResult<FWoTCanonicalJsonValue> BuiltHeader =
		MoveTemp(HeaderBuilder).Build(TEXT("sound header"));
	if (BuiltHeader.IsError()) return FResult::Error(MoveTemp(BuiltHeader).GetError());
	return WoTTextureSoundFrameObject(BuiltHeader.GetValue(), Decoded.GetValue().Bytes);
}

class FWoTTextureSoundCollectSink final : public IWoTByteSink
{
public:
	explicit FWoTTextureSoundCollectSink(uint64 InLimit) : Limit(InLimit) {}

	FWoTBootstrapStatus Consume(TConstArrayView<uint8> Chunk) override
	{
		const uint64 Count = static_cast<uint64>(Chunk.Num());
		if (Count > Limit - Written
			|| Count > static_cast<uint64>(TNumericLimits<int32>::Max() - Bytes.Num()))
		{
			return FWoTBootstrapStatus::Error(WoTTextureSoundError(
				EWoTBootstrapError::SizeLimit,
				TEXT("the typed source exceeded its quarantine limit")));
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

struct FWoTTextureSoundFactType
{
	const TCHAR* Importer = nullptr;
	const TCHAR* Schema = nullptr;
	const TCHAR* ClassName = nullptr;
};

bool WoTTextureSoundSelectFactType(const FWoTRequiredFact& Fact,
	FWoTTextureSoundFactType& Out)
{
	switch (Fact.Domain)
	{
	case EWoTFactDomain::Texture:
		Out = {TEXT("texture-v2"), TEXT("wot-texture-v3"), TEXT("Texture")};
		return true;
	case EWoTFactDomain::Sound:
		Out = {TEXT("sound-v1"), TEXT("wot-sound-v3"), TEXT("Sound")};
		return true;
	default:
		return false;
	}
}

FWoTBootstrapError WoTTextureSoundCancelledError()
{
	return WoTTextureSoundError(EWoTBootstrapError::ServiceUnavailable,
		TEXT("texture/sound import was cancelled before completion"),
		TEXT("texture-sound"));
}

}  // namespace

#if !WOT_GUARDED_BOOTSTRAP
struct FWoTDevelopmentTexturePackage::FImpl
{
	FWoTTextureSoundSource Source;
	FString SourcePath;
};

FWoTDevelopmentTexturePackage::FWoTDevelopmentTexturePackage(
	TUniquePtr<FImpl> InImpl)
	: Impl(MoveTemp(InImpl))
{
}

FWoTDevelopmentTexturePackage::~FWoTDevelopmentTexturePackage() = default;

TWoTBootstrapResult<TUniquePtr<FWoTDevelopmentTexturePackage>>
FWoTDevelopmentTexturePackage::Parse(
	TArray<uint8> SourceBytes, FStringView SourcePath)
{
	using FResult =
		TWoTBootstrapResult<TUniquePtr<FWoTDevelopmentTexturePackage>>;
	TWoTBootstrapResult<FWoTTextureSoundSource> Parsed =
		WoTTextureSoundParseSource(MoveTemp(SourceBytes), SourcePath);
	if (Parsed.IsError())
	{
		return FResult::Error(MoveTemp(Parsed).GetError());
	}
	TUniquePtr<FImpl> State = MakeUnique<FImpl>();
	State->Source = MoveTemp(Parsed).GetValue();
	State->SourcePath = FString(SourcePath);
	return FResult::Success(
		TUniquePtr<FWoTDevelopmentTexturePackage>(
			new FWoTDevelopmentTexturePackage(MoveTemp(State))));
}

bool FWoTDevelopmentTexturePackage::FindTexture(
	FStringView SourceExport, uint32& OutUSize,
	uint32& OutVSize, uint8& OutFormat, FString& OutAnimNext,
	float& OutMinFrameRate, float& OutMaxFrameRate, uint8& OutPrimeCount) const
{
	OutUSize = 0;
	OutVSize = 0;
	OutFormat = 0;
	OutAnimNext.Reset();
	OutMinFrameRate = 0.0f;
	OutMaxFrameRate = 0.0f;
	OutPrimeCount = 0;
	if (!Impl.IsValid())
	{
		return false;
	}
	TWoTBootstrapResult<const FWoTTextureSoundExport*> Export =
		WoTTextureSoundFindExport(
			Impl->Source, Impl->SourcePath, SourceExport, TEXT("Texture"));
	if (Export.IsError())
	{
		return false;
	}
	TWoTBootstrapResult<FWoTTextureSoundTexture> Decoded =
		WoTTextureSoundDecodeTexture(
			Impl->Source, *Export.GetValue(), Impl->SourcePath);
	if (Decoded.IsError() || Decoded.GetValue().Mips.IsEmpty())
	{
		return false;
	}
	OutUSize = Decoded.GetValue().Mips[0].Width;
	OutVSize = Decoded.GetValue().Mips[0].Height;
	// The production decoder accepts only UE1 P8 (Format 0).
	OutFormat = 0;
	OutAnimNext = Decoded.GetValue().AnimNext;
	OutMinFrameRate = Decoded.GetValue().MinFrameRate;
	OutMaxFrameRate = Decoded.GetValue().MaxFrameRate;
	OutPrimeCount = Decoded.GetValue().PrimeCount;
	return true;
}
#endif

TWoTBootstrapResult<TArray<FWoTCacheBlob>> ImportTextureSound(
	const FWoTRetailPackage& Package, const FWoTRetailContract& Contract,
	IWoTObjectWriter& Writer, IWoTProgressSink& Progress)
{
	using FResult = TWoTBootstrapResult<TArray<FWoTCacheBlob>>;
	uint64 Total = 0;
	for (const FWoTRequiredFact& Fact : Contract.Facts)
	{
		FWoTTextureSoundFactType FactType;
		if (WoTTextureSoundSelectFactType(Fact, FactType))
		{
			++Total;
		}
	}
	FWoTBootstrapStatus InitialProgress = Progress.ReportProgress(
		TEXT("texture-sound"), 0, Total);
	if (InitialProgress.IsError())
	{
		return FResult::Error(MoveTemp(InitialProgress).GetError());
	}
	if (Progress.IsCancellationRequested())
	{
		return FResult::Error(WoTTextureSoundCancelledError());
	}

	TArray<FWoTCacheBlob> Blobs;
	Blobs.Reserve(static_cast<int32>(Total));
	TSet<FString> LogicalIds;
	TOptional<FWoTTextureSoundSource> CachedSource;
	FString CachedSourcePath;
	uint64 Completed = 0;
	for (const FWoTRequiredFact& Fact : Contract.Facts)
	{
		FWoTTextureSoundFactType FactType;
		if (!WoTTextureSoundSelectFactType(Fact, FactType)) continue;
		if (Progress.IsCancellationRequested())
		{
			return FResult::Error(WoTTextureSoundCancelledError());
		}
		if (Fact.FactId.IsEmpty() || Fact.SourcePath.IsEmpty()
			|| Fact.SourceExport.IsEmpty() || Fact.Importer != FactType.Importer
			|| Fact.Outputs.Num() != 1 || Fact.Outputs[0].LogicalId.IsEmpty()
			|| Fact.Outputs[0].Schema != FactType.Schema
			|| Fact.Outputs[0].MaxDecodedSize == 0)
		{
			return FResult::Error(WoTTextureSoundError(EWoTBootstrapError::UnknownField,
				TEXT("the typed fact does not match its closed importer/output schema"),
				Fact.FactId));
		}
		const FWoTRequiredFactOutput& Output = Fact.Outputs[0];
		if (LogicalIds.Contains(Output.LogicalId))
		{
			return FResult::Error(WoTTextureSoundError(EWoTBootstrapError::DuplicateField,
				TEXT("two typed facts emit the same logical object"), Output.LogicalId));
		}
		LogicalIds.Add(Output.LogicalId);

		if (!CachedSource.IsSet() || CachedSourcePath != Fact.SourcePath)
		{
			TWoTBootstrapResult<FWoTPackageEntry> Entry =
				Package.FindExact(Fact.SourcePath);
			if (Entry.IsError()) return FResult::Error(MoveTemp(Entry).GetError());
			if (Entry.GetValue().Size == 0
				|| Entry.GetValue().Size > WoTTextureSoundMaxSourceBytes)
			{
				return FResult::Error(WoTTextureSoundError(
					EWoTBootstrapError::SizeLimit,
					TEXT("the typed source declaration exceeds the source byte bound"),
					Fact.SourcePath));
			}
			FWoTTextureSoundCollectSink Sink(WoTTextureSoundMaxSourceBytes);
			FWoTBootstrapStatus Streamed =
				Package.StreamExact(Entry.GetValue(), Sink);
			if (Streamed.IsError())
			{
				return FResult::Error(MoveTemp(Streamed).GetError());
			}
			TWoTBootstrapResult<FWoTTextureSoundSource> Parsed =
				WoTTextureSoundParseSource(MoveTemp(Sink.Bytes), Fact.SourcePath);
			if (Parsed.IsError())
			{
				return FResult::Error(MoveTemp(Parsed).GetError());
			}
			CachedSource.Emplace(MoveTemp(Parsed).GetValue());
			CachedSourcePath = Fact.SourcePath;
		}
		const FWoTTextureSoundSource& Source = CachedSource.GetValue();
		TWoTBootstrapResult<const FWoTTextureSoundExport*> Export =
			WoTTextureSoundFindExport(Source, Fact.SourcePath,
				Fact.SourceExport, FactType.ClassName);
		if (Export.IsError()) return FResult::Error(MoveTemp(Export).GetError());
		TWoTBootstrapResult<TArray<uint8>> Object =
			Fact.Domain == EWoTFactDomain::Texture
			? WoTTextureSoundBuildTextureObject(
				Fact, Source, *Export.GetValue())
			: WoTTextureSoundBuildSoundObject(
				Fact, Source, *Export.GetValue());
		if (Object.IsError()) return FResult::Error(MoveTemp(Object).GetError());
		if (static_cast<uint64>(Object.GetValue().Num()) > Output.MaxDecodedSize)
		{
			return FResult::Error(WoTTextureSoundError(EWoTBootstrapError::SizeLimit,
				TEXT("the typed object exceeds its contract maximum"), Output.LogicalId));
		}
		if (Progress.IsCancellationRequested())
		{
			return FResult::Error(WoTTextureSoundCancelledError());
		}
		FWoTBootstrapStatus Reverified =
			Package.ReverifyHeldSnapshotAfterExtraction();
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
		FWoTRequiredFactId& SatisfiedFact = Blob.Satisfies.AddDefaulted_GetRef();
		SatisfiedFact.Value = Fact.FactId;
		++Completed;
		FWoTBootstrapStatus Reported = Progress.ReportProgress(
			TEXT("texture-sound"), Completed, Total);
		if (Reported.IsError()) return FResult::Error(MoveTemp(Reported).GetError());
		if (Progress.IsCancellationRequested())
		{
			return FResult::Error(WoTTextureSoundCancelledError());
		}
	}
	return FResult::Success(MoveTemp(Blobs));
}

#if WITH_AUTOMATION_TESTS
TWoTBootstrapResult<TArray<FWoTCacheBlob>> ImportTexturePackageBytesForTesting(
	TConstArrayView<uint8> SourceBytes, FStringView SourcePath,
	const FWoTRetailContract& Contract, IWoTObjectWriter& Writer,
	IWoTProgressSink& Progress)
{
	using FResult = TWoTBootstrapResult<TArray<FWoTCacheBlob>>;
	TArray<uint8> Owned;
	Owned.Append(SourceBytes.GetData(), SourceBytes.Num());
	TWoTBootstrapResult<FWoTTextureSoundSource> Parsed =
		WoTTextureSoundParseSource(MoveTemp(Owned), SourcePath);
	if (Parsed.IsError()) return FResult::Error(MoveTemp(Parsed).GetError());
	const FWoTTextureSoundSource& Source = Parsed.GetValue();
	uint64 Total = 0;
	for (const FWoTRequiredFact& Fact : Contract.Facts)
		if (Fact.Domain == EWoTFactDomain::Texture
			&& Fact.SourcePath == SourcePath) ++Total;
	FWoTBootstrapStatus Reported = Progress.ReportProgress(
		TEXT("texture-sound"), 0, Total);
	if (Reported.IsError()) return FResult::Error(MoveTemp(Reported).GetError());
	TArray<FWoTCacheBlob> Blobs;
	Blobs.Reserve(static_cast<int32>(Total));
	uint64 Completed = 0;
	for (const FWoTRequiredFact& Fact : Contract.Facts)
	{
		if (Fact.Domain != EWoTFactDomain::Texture
			|| Fact.SourcePath != SourcePath) continue;
		if (Fact.Importer != TEXT("texture-v2") || Fact.Outputs.Num() != 1
			|| Fact.Outputs[0].Schema != TEXT("wot-texture-v3")
			|| Fact.Outputs[0].LogicalId.IsEmpty()
			|| Fact.Outputs[0].MaxDecodedSize == 0)
		{
			return FResult::Error(WoTTextureSoundError(
				EWoTBootstrapError::UnknownField,
				TEXT("the texture oracle fact is malformed"), Fact.FactId));
		}
		TWoTBootstrapResult<const FWoTTextureSoundExport*> Export =
			WoTTextureSoundFindExport(Source, Fact.SourcePath,
				Fact.SourceExport, TEXT("Texture"));
		if (Export.IsError()) return FResult::Error(MoveTemp(Export).GetError());
		TWoTBootstrapResult<TArray<uint8>> Object =
			WoTTextureSoundBuildTextureObject(Fact, Source, *Export.GetValue());
		if (Object.IsError()) return FResult::Error(MoveTemp(Object).GetError());
		const FWoTRequiredFactOutput& Output = Fact.Outputs[0];
		if (static_cast<uint64>(Object.GetValue().Num()) > Output.MaxDecodedSize)
		{
			return FResult::Error(WoTTextureSoundError(
				EWoTBootstrapError::SizeLimit,
				TEXT("the texture oracle object exceeds its contract maximum"),
				Output.LogicalId));
		}
		const FWoTSha256 Digest = Sha256(Object.GetValue());
		FWoTBootstrapStatus Written = Writer.WriteObject(Output.LogicalId,
			Output.Schema, Object.GetValue(), Digest, Output.MaxDecodedSize);
		if (Written.IsError()) return FResult::Error(MoveTemp(Written).GetError());
		FWoTCacheBlob& Blob = Blobs.AddDefaulted_GetRef();
		Blob.LogicalId = Output.LogicalId;
		Blob.Schema = Output.Schema;
		Blob.Sha256 = Digest;
		Blob.Size = static_cast<uint64>(Object.GetValue().Num());
		Blob.Satisfies.AddDefaulted_GetRef().Value = Fact.FactId;
		Reported = Progress.ReportProgress(
			TEXT("texture-sound"), ++Completed, Total);
		if (Reported.IsError()) return FResult::Error(MoveTemp(Reported).GetError());
	}
	return FResult::Success(MoveTemp(Blobs));
}
#endif
