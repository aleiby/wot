#include "WoTRetailReader/FrontEndWorld.h"

#include "CanonicalObjectBuilder.h"
#include "TypedProperties.h"
#include "Misc/Paths.h"
#include "WoTBootstrapCore/CanonicalJson.h"
#include "WoTBootstrapCore/Hash.h"

namespace
{
constexpr TCHAR WoTFrontEndMapName[] = TEXT("WheelOfTime");
constexpr TCHAR WoTFrontEndSourcePath[] = TEXT("Maps/WheelOfTime.wot");
constexpr uint32 WoTFrontEndPackageTag = 0x9e2a83c1;
constexpr uint32 WoTFrontEndHasStack = 0x02000000;
constexpr uint64 WoTFrontEndMaxSourceBytes = 256ull * 1024 * 1024;
constexpr uint32 WoTFrontEndMaxTableItems = 1000000;
constexpr uint32 WoTFrontEndMaxNameBytes = 1024;
constexpr uint32 WoTFrontEndMaxActors = 1000000;
constexpr uint32 WoTFrontEndMaxProperties = 4000000;
constexpr uint32 WoTFrontEndMaxModelItems = 6000000;
constexpr double WoTFrontEndMaxCoordinate = 10000000.0;
constexpr double WoTFrontEndEyeHeight = 42.0;
constexpr double WoTFrontEndRotatorDegrees = 360.0 / 65536.0;

template <typename T>
TWoTBootstrapResult<T> WoTFrontEndError(EWoTBootstrapError Code,
	const TCHAR* Message, FString Path = FString())
{
	return TWoTBootstrapResult<T>::Error(
		MakeWoTBootstrapError(Code, Message, MoveTemp(Path)));
}

class FWoTFrontEndReader
{
public:
	FWoTFrontEndReader() = default;
	explicit FWoTFrontEndReader(TConstArrayView<uint8> InBytes)
		: Bytes(InBytes), End(static_cast<uint64>(InBytes.Num())) {}
	FWoTFrontEndReader(TConstArrayView<uint8> InBytes, uint64 Begin, uint64 InEnd)
		: Bytes(InBytes), Offset(Begin), End(InEnd)
	{
		bValid = Begin <= InEnd && InEnd <= static_cast<uint64>(InBytes.Num());
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
		uint16 Bits = 0;
		if (!ReadU16(Bits)) return false;
		Out = static_cast<int16>(Bits);
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
		uint32 Bits = 0;
		if (!ReadU32(Bits)) return false;
		Out = static_cast<int32>(Bits);
		return true;
	}
	bool ReadU64(uint64& Out)
	{
		uint32 Low = 0;
		uint32 High = 0;
		if (!ReadU32(Low) || !ReadU32(High)) return false;
		Out = static_cast<uint64>(Low) | (static_cast<uint64>(High) << 32);
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
			|| (bNegative && Magnitude == 0)) return false;
		Out = bNegative ? -static_cast<int32>(Magnitude)
			: static_cast<int32>(Magnitude);
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
	bool Skip(uint64 Count)
	{
		if (!Require(Count)) return false;
		Offset += Count;
		return true;
	}
	bool SetOffset(uint64 NewOffset)
	{
		if (NewOffset > End) return false;
		Offset = NewOffset;
		return true;
	}
	bool Require(uint64 Count) const
	{
		return bValid && Offset <= End && Count <= End - Offset;
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

struct FWoTFrontEndImport
{
	FString ClassPackage;
	FString ClassName;
	int32 Outer = 0;
	FString Name;
};

struct FWoTFrontEndExport
{
	int32 ClassReference = 0;
	int32 SuperReference = 0;
	int32 Outer = 0;
	FString Name;
	uint32 Flags = 0;
	uint64 Offset = 0;
	uint64 Size = 0;
};

struct FWoTFrontEndPackageView
{
	FString SourcePath;
	FString PackageName;
	TArray<uint8> Bytes;
	uint16 Version = 0;
	TArray<FString> Names;
	TArray<FWoTFrontEndImport> Imports;
	TArray<FWoTFrontEndExport> Exports;

	bool ReferenceValid(int32 Reference) const
	{
		if (Reference == 0) return true;
		if (Reference > 0) return Exports.IsValidIndex(Reference - 1);
		const int64 Index = -static_cast<int64>(Reference) - 1;
		return Index >= 0 && Index < Imports.Num();
	}
	const FString* NameAt(int32 Index) const
	{
		return Names.IsValidIndex(Index) ? &Names[Index] : nullptr;
	}
	const FString* ClassLeaf(int32 Reference) const
	{
		if (Reference < 0)
		{
			const int64 Index = -static_cast<int64>(Reference) - 1;
			return Imports.IsValidIndex(static_cast<int32>(Index))
				? &Imports[static_cast<int32>(Index)].Name : nullptr;
		}
		return Reference > 0 && Exports.IsValidIndex(Reference - 1)
			? &Exports[Reference - 1].Name : nullptr;
	}
};

struct FWoTFrontEndSemantics
{
	struct FResourceReference
	{
		EWoTFactDomain Domain = EWoTFactDomain::Texture;
		FString Identity;
	};
	FString MapName;
	TArray<TPair<FString, FString>> Actors;
	TArray<FResourceReference> ResourceReferences;
	TMap<uint32, FString> ThunderSoundOverrides;
	TArray<FString> ThunderSoundSourceExports;
	FVector CameraLocation = FVector::ZeroVector;
	FRotator CameraRotation = FRotator::ZeroRotator;
	float HorizontalFov = 90.0f;
	FWoTSha256 SourceRoot;
};

bool WoTFrontEndLatin1(TConstArrayView<uint8> Bytes, FString& Out)
{
	Out.Reset(Bytes.Num());
	for (uint8 Byte : Bytes)
	{
		if (Byte == 0) return false;
		Out.AppendChar(static_cast<TCHAR>(Byte));
	}
	return true;
}

FString WoTFrontEndFold(FStringView Value)
{
	FString Folded(Value);
	for (TCHAR& Character : Folded)
	{
		if (Character >= TEXT('A') && Character <= TEXT('Z'))
			Character += TEXT('a') - TEXT('A');
	}
	return Folded;
}

FString WoTFrontEndLeaf(FStringView Identity)
{
	const FString Value(Identity);
	int32 Dot = INDEX_NONE;
	return Value.FindLastChar(TEXT('.'), Dot) ? Value.Mid(Dot + 1) : Value;
}

const TCHAR* WoTFrontEndDomainName(EWoTFactDomain Domain)
{
	switch (Domain)
	{
	case EWoTFactDomain::Texture: return TEXT("texture");
	case EWoTFactDomain::Sound: return TEXT("sound");
	case EWoTFactDomain::Music: return TEXT("music");
	case EWoTFactDomain::Mesh: return TEXT("mesh");
	case EWoTFactDomain::Animation: return TEXT("animation");
	case EWoTFactDomain::BSP: return TEXT("bsp");
	case EWoTFactDomain::World: return TEXT("world");
	case EWoTFactDomain::Font: return TEXT("font");
	case EWoTFactDomain::Procedural: return TEXT("procedural");
	case EWoTFactDomain::Registry: return TEXT("registry");
	case EWoTFactDomain::Localization: return TEXT("localization");
	case EWoTFactDomain::ClassDefault: return TEXT("class_default");
	default: return TEXT("invalid");
	}
}

TOptional<EWoTFactDomain> WoTFrontEndParseDomain(FStringView Name)
{
	for (uint8 Index = 0;
		Index <= static_cast<uint8>(EWoTFactDomain::ClassDefault); ++Index)
	{
		const EWoTFactDomain Domain = static_cast<EWoTFactDomain>(Index);
		if (Name == WoTFrontEndDomainName(Domain)) return Domain;
	}
	return TOptional<EWoTFactDomain>();
}

struct FWoTFrontEndExpectedResource
{
	EWoTFactDomain Domain;
	const TCHAR* SourceExport;
};

const FWoTFrontEndExpectedResource WoTFrontEndExpectedResources[] = {
	{EWoTFactDomain::Animation, TEXT("Environment.RainCyd")},
	{EWoTFactDomain::Font, TEXT("WOT.F_WOTReg14")},
	{EWoTFactDomain::Font, TEXT("WOT.F_WOTReg30")},
	{EWoTFactDomain::Localization, TEXT("WoT.int")},
	{EWoTFactDomain::Mesh, TEXT("Environment.RainCyd")},
	{EWoTFactDomain::Procedural, TEXT("JHPFireEng.M01ModWet")},
	{EWoTFactDomain::Procedural, TEXT("WarrenT.Rain2")},
	{EWoTFactDomain::Sound, TEXT("AmbientA.Water.DIVE")},
	{EWoTFactDomain::Sound, TEXT("AmbientA.Water.Stream6")},
	{EWoTFactDomain::Sound, TEXT("AmbientA.Water.Stream_1")},
	{EWoTFactDomain::Sound, TEXT("AmbientA.Water.Trickle3")},
	{EWoTFactDomain::Sound, TEXT("AmbientA.Water.WaterFll5")},
	{EWoTFactDomain::Sound, TEXT("AmbientA.Wind.Wind")},
	{EWoTFactDomain::Sound, TEXT("Environment.Thunder.Thunder10")},
	{EWoTFactDomain::Sound, TEXT("Environment.Thunder.Thunder2")},
	{EWoTFactDomain::Sound, TEXT("Environment.Thunder.Thunder3")},
	{EWoTFactDomain::Sound, TEXT("Environment.Thunder.Thunder4")},
	{EWoTFactDomain::Sound, TEXT("Environment.Thunder.Thunder8")},
	{EWoTFactDomain::Sound, TEXT("Environment.Thunder.Thunder9")},
	{EWoTFactDomain::Sound, TEXT("WOT.MoveSelector")},
	{EWoTFactDomain::Sound, TEXT("WOT.Select")},
	{EWoTFactDomain::Texture, TEXT("Environment.Skins.RainDropsA")},
	{EWoTFactDomain::Texture, TEXT("ForsakenT.DetailArchitect.MarbBalc2720")},
	{EWoTFactDomain::Texture, TEXT("ForsakenT.Dev.TypeMask_003sky")},
	{EWoTFactDomain::Texture, TEXT("ForsakenT.SurfaceFloor.MarblStep2362")},
	{EWoTFactDomain::Texture, TEXT("JamesT.flare_blue01")},
	{EWoTFactDomain::Texture, TEXT("MordethT.DetailArchitect.MetlGril4002v02")},
	{EWoTFactDomain::Texture, TEXT("MordethT.DetailArchitect.StonArch4348")},
	{EWoTFactDomain::Texture, TEXT("MordethT.DetailArchitect.StonTrim4353")},
	{EWoTFactDomain::Texture, TEXT("MordethT.DetailArchitect.StonWind4347v1")},
	{EWoTFactDomain::Texture, TEXT("MordethT.SurfaceFloor.StonFlor4900")},
	{EWoTFactDomain::Texture, TEXT("MordethT.SurfaceWall.PlasWall4373v1")},
	{EWoTFactDomain::Texture, TEXT("MordethT.SurfaceWall.StonBlok4345")},
	{EWoTFactDomain::Texture, TEXT("MordethT.SurfaceWall.StonBlok4345v1")},
	{EWoTFactDomain::Texture, TEXT("MordethT.SurfaceWall.StonWall4018")},
	{EWoTFactDomain::Texture, TEXT("MordethT.SurfaceWall.StonWall4031v02")},
	{EWoTFactDomain::Texture, TEXT("MordethT.SurfaceWall.StonWall4333")},
	{EWoTFactDomain::Texture, TEXT("NatureT.Floor.DirtDirt_010")},
	{EWoTFactDomain::Texture, TEXT("NatureT.Sky.Sky_440")},
	{EWoTFactDomain::Texture, TEXT("NatureT.VerticalFace.StonFace_931")},
	{EWoTFactDomain::Texture, TEXT("ParticleSystems.Water.Splash02")},
	{EWoTFactDomain::Texture, TEXT("ScottT.mountain2")},
	{EWoTFactDomain::Texture, TEXT("WOT.F_WOTReg14.Texture9")},
	{EWoTFactDomain::Texture, TEXT("WOT.F_WOTReg30.Texture10")},
	{EWoTFactDomain::Texture, TEXT("WarrenT.Developer.Black")},
	{EWoTFactDomain::Texture, TEXT("WarrenT.MarcHigh.RckFlr_9011mt")},
};

FString WoTFrontEndExpectedSourcePath(FStringView SourceExport)
{
	const FString Identity(SourceExport);
	if (Identity == TEXT("WoT.int")) return TEXT("System/WoT.int");
	if (Identity.StartsWith(TEXT("WOT."))) return TEXT("System/WOT.u");
	if (Identity.StartsWith(TEXT("Environment.")))
		return TEXT("System/Environment.u");
	if (Identity.StartsWith(TEXT("ParticleSystems.")))
		return TEXT("System/ParticleSystems.u");
	if (Identity.StartsWith(TEXT("AmbientA."))) return TEXT("Sounds/AmbientA.uax");
	int32 Dot = INDEX_NONE;
	if (!Identity.FindChar(TEXT('.'), Dot)) return FString();
	return TEXT("Textures/") + Identity.Left(Dot) + TEXT(".utx");
}

FString WoTFrontEndResourceKey(EWoTFactDomain Domain, FStringView SourceExport)
{
	return FString(WoTFrontEndDomainName(Domain)) + TEXT("\n")
		+ WoTFrontEndFold(SourceExport);
}

TWoTBootstrapResult<FString> WoTFrontEndObjectPath(
	const FWoTFrontEndPackageView& Package, int32 Reference)
{
	using FResult = TWoTBootstrapResult<FString>;
	if (Reference == 0) return FResult::Success(FString());
	TArray<FString> Reverse;
	TSet<int32> Seen;
	int32 Current = Reference;
	for (uint32 Depth = 0; Current != 0 && Depth < 32; ++Depth)
	{
		if (!Package.ReferenceValid(Current) || Seen.Contains(Current))
			return WoTFrontEndError<FString>(EWoTBootstrapError::CorruptInstallation,
				TEXT("an entry object outer chain is invalid"));
		Seen.Add(Current);
		if (Current > 0)
		{
			const FWoTFrontEndExport& Export = Package.Exports[Current - 1];
			Reverse.Add(Export.Name);
			Current = Export.Outer;
			if (Current == 0) Reverse.Add(Package.PackageName);
		}
		else
		{
			const FWoTFrontEndImport& Import = Package.Imports[-Current - 1];
			Reverse.Add(Import.Name);
			Current = Import.Outer;
		}
	}
	if (Current != 0)
		return WoTFrontEndError<FString>(EWoTBootstrapError::SizeLimit,
			TEXT("an entry object outer chain exceeds its bound"));
	FString Result;
	for (int32 Index = Reverse.Num() - 1; Index >= 0; --Index)
	{
		if (!Result.IsEmpty()) Result += TEXT(".");
		Result += Reverse[Index];
	}
	return FResult::Success(MoveTemp(Result));
}

bool WoTFrontEndOpenExport(const FWoTFrontEndPackageView& Package,
	const FWoTFrontEndExport& Export, FWoTFrontEndReader& Out)
{
	Out = FWoTFrontEndReader(Package.Bytes, Export.Offset,
		Export.Offset + Export.Size);
	if ((Export.Flags & WoTFrontEndHasStack) != 0)
	{
		int32 Node = 0;
		int32 StateNode = 0;
		if (!Out.ReadCompact(Node) || !Out.ReadCompact(StateNode) || !Out.Skip(12)
			|| (Node != 0 && !Out.ReadCompact(StateNode))) return false;
	}
	return Out.IsValid();
}

bool WoTFrontEndReadName(FWoTFrontEndReader& Reader,
	const FWoTFrontEndPackageView& Package, FString& Out)
{
	int32 Index = 0;
	if (!Reader.ReadCompact(Index) || Package.NameAt(Index) == nullptr) return false;
	Out = *Package.NameAt(Index);
	return true;
}

bool WoTFrontEndReadProperties(FWoTFrontEndReader& Reader,
	const FWoTFrontEndPackageView& Package, TArray<FWoTTypedProperty>& Out,
	uint64& Count, bool bSavedClassDefaults = false)
{
	FWoTTypedPropertyReadOptions Options;
	Options.MaximumProperties = WoTFrontEndMaxProperties;
	Options.TotalPropertyCount = &Count;
	Options.bSavedClassDefaults = bSavedClassDefaults;
	Options.bRejectDuplicateIndexedFields = true;
	Options.bSortCanonical = true;
	return WoTReadTypedProperties(Reader,
		[&Package](FWoTFrontEndReader& From, FString& Name)
		{
			return WoTFrontEndReadName(From, Package, Name);
		},
		[&Package](int32 Reference) { return Package.ReferenceValid(Reference); },
		Out, Options);
}

bool WoTFrontEndReadDefaultString(const FWoTTypedProperty& Property, FString& Out);

TWoTBootstrapResult<FWoTFrontEndPackageView> WoTFrontEndParsePackage(
	TArray<uint8>&& Bytes, FString SourcePath, FString PackageName)
{
	using FResult = TWoTBootstrapResult<FWoTFrontEndPackageView>;
	if (Bytes.IsEmpty() || static_cast<uint64>(Bytes.Num()) > WoTFrontEndMaxSourceBytes)
		return WoTFrontEndError<FWoTFrontEndPackageView>(EWoTBootstrapError::SizeLimit,
			TEXT("the entry world source exceeds its byte bound"), WoTFrontEndSourcePath);
	FWoTFrontEndPackageView Package;
	Package.SourcePath = MoveTemp(SourcePath);
	Package.PackageName = MoveTemp(PackageName);
	Package.Bytes = MoveTemp(Bytes);
	FWoTFrontEndReader Reader(Package.Bytes);
	uint32 Tag = 0;
	uint32 PackageFlags = 0;
	uint16 Licensee = 0;
	int32 NameCount = 0;
	int32 NameOffset = 0;
	int32 ExportCount = 0;
	int32 ExportOffset = 0;
	int32 ImportCount = 0;
	int32 ImportOffset = 0;
	if (!Reader.ReadU32(Tag) || !Reader.ReadU16(Package.Version)
		|| !Reader.ReadU16(Licensee) || !Reader.ReadU32(PackageFlags)
		|| !Reader.ReadI32(NameCount) || !Reader.ReadI32(NameOffset)
		|| !Reader.ReadI32(ExportCount) || !Reader.ReadI32(ExportOffset)
		|| !Reader.ReadI32(ImportCount) || !Reader.ReadI32(ImportOffset)
		|| Tag != WoTFrontEndPackageTag || Package.Version < 61 || Package.Version > 69
		|| NameCount <= 0 || ExportCount <= 0 || ImportCount < 0
		|| NameCount > static_cast<int32>(WoTFrontEndMaxTableItems)
		|| ExportCount > static_cast<int32>(WoTFrontEndMaxTableItems)
		|| ImportCount > static_cast<int32>(WoTFrontEndMaxTableItems)
		|| NameOffset < 0 || ExportOffset < 0 || ImportOffset < 0)
	{
		return WoTFrontEndError<FWoTFrontEndPackageView>(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("the entry world source is not a bounded UE1 package"),
			WoTFrontEndSourcePath);
	}
	if (!Reader.SetOffset(static_cast<uint64>(NameOffset)))
		return WoTFrontEndError<FWoTFrontEndPackageView>(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("the entry name table is outside the package"));
	for (int32 Index = 0; Index < NameCount; ++Index)
	{
		TArray<uint8> Raw;
		if (Package.Version >= 64)
		{
			int32 Length = 0;
			if (!Reader.ReadCompact(Length) || Length <= 0
				|| Length > static_cast<int32>(WoTFrontEndMaxNameBytes)
				|| !Reader.ReadBytes(static_cast<uint64>(Length), Raw)
				|| Raw.Last() != 0)
				return WoTFrontEndError<FWoTFrontEndPackageView>(
					EWoTBootstrapError::CorruptInstallation,
					TEXT("the entry name table is malformed"));
			Raw.Pop(EAllowShrinking::No);
		}
		else
		{
			bool bTerminated = false;
			for (uint32 ByteIndex = 0; ByteIndex < WoTFrontEndMaxNameBytes; ++ByteIndex)
			{
				uint8 Byte = 0;
				if (!Reader.ReadU8(Byte)) break;
				if (Byte == 0) { bTerminated = true; break; }
				Raw.Add(Byte);
			}
			if (!bTerminated)
				return WoTFrontEndError<FWoTFrontEndPackageView>(
					EWoTBootstrapError::CorruptInstallation,
					TEXT("the entry name table is truncated"));
		}
		uint32 Flags = 0;
		FString Name;
		if (!Reader.ReadU32(Flags) || !WoTFrontEndLatin1(Raw, Name)
			|| Name.IsEmpty() || Name.Contains(TEXT(".")))
			return WoTFrontEndError<FWoTFrontEndPackageView>(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("the entry name table contains an unsafe name"));
		Package.Names.Add(MoveTemp(Name));
	}
	if (!Reader.SetOffset(static_cast<uint64>(ImportOffset)))
		return WoTFrontEndError<FWoTFrontEndPackageView>(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("the entry import table is outside the package"));
	for (int32 Index = 0; Index < ImportCount; ++Index)
	{
		int32 ClassPackage = 0;
		int32 ClassName = 0;
		int32 Outer = 0;
		int32 ObjectName = 0;
		if (!Reader.ReadCompact(ClassPackage) || !Reader.ReadCompact(ClassName)
			|| !Reader.ReadI32(Outer) || !Reader.ReadCompact(ObjectName)
			|| Package.NameAt(ClassPackage) == nullptr
			|| Package.NameAt(ClassName) == nullptr
			|| Package.NameAt(ObjectName) == nullptr)
			return WoTFrontEndError<FWoTFrontEndPackageView>(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("the entry import table is malformed"));
		Package.Imports.Add({*Package.NameAt(ClassPackage), *Package.NameAt(ClassName),
			Outer, *Package.NameAt(ObjectName)});
	}
	if (!Reader.SetOffset(static_cast<uint64>(ExportOffset)))
		return WoTFrontEndError<FWoTFrontEndPackageView>(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("the entry export table is outside the package"));
	for (int32 Index = 0; Index < ExportCount; ++Index)
	{
		int32 ClassReference = 0;
		int32 SuperReference = 0;
		int32 Outer = 0;
		int32 NameIndex = 0;
		uint32 Flags = 0;
		int32 Size = 0;
		int32 Offset = 0;
		if (!Reader.ReadCompact(ClassReference) || !Reader.ReadCompact(SuperReference)
			|| !Reader.ReadI32(Outer) || !Reader.ReadCompact(NameIndex)
			|| !Reader.ReadU32(Flags) || !Reader.ReadCompact(Size)
			|| Size < 0 || Package.NameAt(NameIndex) == nullptr
			|| (Size > 0 && !Reader.ReadCompact(Offset)) || Offset < 0
			|| static_cast<uint64>(Offset) > static_cast<uint64>(Package.Bytes.Num())
			|| static_cast<uint64>(Size) >
				static_cast<uint64>(Package.Bytes.Num()) - static_cast<uint64>(Offset))
			return WoTFrontEndError<FWoTFrontEndPackageView>(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("the entry export table is malformed"));
		Package.Exports.Add({ClassReference, SuperReference, Outer,
			*Package.NameAt(NameIndex), Flags,
			static_cast<uint64>(Offset), static_cast<uint64>(Size)});
	}
	for (int32 Index = 0; Index < Package.Imports.Num(); ++Index)
		if (!Package.ReferenceValid(Package.Imports[Index].Outer))
			return WoTFrontEndError<FWoTFrontEndPackageView>(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("the entry import table has an invalid outer"));
	for (int32 Index = 0; Index < Package.Exports.Num(); ++Index)
	{
		const FWoTFrontEndExport& Export = Package.Exports[Index];
		if (!Package.ReferenceValid(Export.ClassReference)
			|| !Package.ReferenceValid(Export.SuperReference)
			|| !Package.ReferenceValid(Export.Outer)
			|| WoTFrontEndObjectPath(Package, Index + 1).IsError())
			return WoTFrontEndError<FWoTFrontEndPackageView>(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("the entry export table has an invalid reference"));
	}
	return FResult::Success(MoveTemp(Package));
}

bool WoTFrontEndReadFString(FWoTFrontEndReader& Reader)
{
	int32 Length = 0;
	if (!Reader.ReadCompact(Length)) return false;
	if (Length == 0) return true;
	const uint64 Characters = static_cast<uint64>(FMath::Abs(static_cast<int64>(Length)));
	return Characters <= 1000000
		&& Reader.Skip(Characters * (Length < 0 ? 2ull : 1ull));
}

bool WoTFrontEndSkipURL(FWoTFrontEndReader& Reader)
{
	for (int32 Index = 0; Index < 4; ++Index)
		if (!WoTFrontEndReadFString(Reader)) return false;
	int32 Options = 0;
	if (!Reader.ReadCompact(Options) || Options < 0 || Options > 65536) return false;
	for (int32 Index = 0; Index < Options; ++Index)
		if (!WoTFrontEndReadFString(Reader)) return false;
	return Reader.Skip(8);
}

bool WoTFrontEndReadVector(const FWoTTypedProperty& Property, FVector& Out)
{
	if (Property.Type != 10 || !Property.StructName.Equals(
		TEXT("Vector"), ESearchCase::IgnoreCase) || Property.Value.Num() != 12)
		return false;
	float Values[3];
	FMemory::Memcpy(Values, Property.Value.GetData(), sizeof(Values));
	if (!FMath::IsFinite(Values[0]) || !FMath::IsFinite(Values[1])
		|| !FMath::IsFinite(Values[2])) return false;
	Out = FVector(Values[0], Values[1], Values[2]);
	return FMath::Abs(Out.X) <= WoTFrontEndMaxCoordinate
		&& FMath::Abs(Out.Y) <= WoTFrontEndMaxCoordinate
		&& FMath::Abs(Out.Z) <= WoTFrontEndMaxCoordinate;
}

bool WoTFrontEndReadRotator(const FWoTTypedProperty& Property, FRotator& Out)
{
	if (Property.Type != 10 || !Property.StructName.Equals(
		TEXT("Rotator"), ESearchCase::IgnoreCase) || Property.Value.Num() != 12)
		return false;
	int32 Values[3];
	FMemory::Memcpy(Values, Property.Value.GetData(), sizeof(Values));
	Out = FRotator(
		static_cast<double>(Values[0]) * WoTFrontEndRotatorDegrees,
		static_cast<double>(Values[1]) * WoTFrontEndRotatorDegrees,
		static_cast<double>(Values[2]) * WoTFrontEndRotatorDegrees);
	return true;
}

TWoTBootstrapResult<FWoTFrontEndSemantics> WoTFrontEndInspectPackage(
	FWoTFrontEndPackageView&& Package)
{
	using FResult = TWoTBootstrapResult<FWoTFrontEndSemantics>;
	int32 LevelReference = 0;
	for (int32 Index = 0; Index < Package.Exports.Num(); ++Index)
	{
		const FString* Class = Package.ClassLeaf(Package.Exports[Index].ClassReference);
		if (Class != nullptr && Class->Equals(TEXT("Level"), ESearchCase::CaseSensitive))
		{
			if (LevelReference != 0)
				return WoTFrontEndError<FWoTFrontEndSemantics>(
					EWoTBootstrapError::DuplicateField,
					TEXT("the entry package has duplicate Level exports"));
			LevelReference = Index + 1;
		}
	}
	if (LevelReference == 0)
		return WoTFrontEndError<FWoTFrontEndSemantics>(EWoTBootstrapError::UnknownField,
			TEXT("the entry package has no Level export"));
	FWoTFrontEndReader LevelReader;
	TArray<FWoTTypedProperty> LevelProperties;
	uint64 PropertyCount = 0;
	int32 ActorCount = 0;
	int32 ActorMaximum = 0;
	if (!WoTFrontEndOpenExport(Package, Package.Exports[LevelReference - 1], LevelReader)
		|| !WoTFrontEndReadProperties(LevelReader, Package, LevelProperties, PropertyCount)
		|| !LevelReader.ReadI32(ActorCount) || !LevelReader.ReadI32(ActorMaximum)
		|| ActorCount < 0 || ActorMaximum < ActorCount
		|| ActorCount > static_cast<int32>(WoTFrontEndMaxActors))
		return WoTFrontEndError<FWoTFrontEndSemantics>(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("the entry Level actor registry is malformed"));
	TArray<int32> ActorReferences;
	TSet<int32> ActorSet;
	for (int32 Index = 0; Index < ActorCount; ++Index)
	{
		int32 Reference = 0;
		if (!LevelReader.ReadCompact(Reference) || Reference < 0
			|| !Package.ReferenceValid(Reference))
			return WoTFrontEndError<FWoTFrontEndSemantics>(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("the entry Level actor registry has an invalid reference"));
		if (Reference > 0)
		{
			if (ActorSet.Contains(Reference))
				return WoTFrontEndError<FWoTFrontEndSemantics>(
					EWoTBootstrapError::DuplicateField,
					TEXT("the entry Level actor registry has a duplicate nonzero reference"));
			ActorSet.Add(Reference);
			ActorReferences.Add(Reference);
		}
	}
	if (!WoTFrontEndSkipURL(LevelReader))
		return WoTFrontEndError<FWoTFrontEndSemantics>(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("the entry Level URL is malformed"));
	int32 ModelReference = 0;
	if (!LevelReader.ReadCompact(ModelReference) || ModelReference <= 0
		|| !Package.Exports.IsValidIndex(ModelReference - 1)
		|| Package.ClassLeaf(
			Package.Exports[ModelReference - 1].ClassReference) == nullptr
		|| !Package.ClassLeaf(Package.Exports[ModelReference - 1].ClassReference)
			->Equals(TEXT("Model"), ESearchCase::CaseSensitive))
		return WoTFrontEndError<FWoTFrontEndSemantics>(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("the entry Level model reference is malformed"));

	FWoTFrontEndSemantics Semantics;
	Semantics.MapName = WoTFrontEndMapName;
	bool bCamera = false;
	TMap<FString, FWoTFrontEndSemantics::FResourceReference> Resources;
	auto AddTypedReference = [&Resources](EWoTFactDomain Domain,
		FString Identity)
	{
		const FString Key = WoTFrontEndResourceKey(Domain, Identity);
		if (!Resources.Contains(Key))
			Resources.Add(Key, {Domain, MoveTemp(Identity)});
	};
	auto AddReference = [&Package, &ActorSet, &AddTypedReference](int32 Reference)
		-> FWoTBootstrapStatus
	{
		if (Reference == 0 || ActorSet.Contains(Reference))
			return FWoTBootstrapStatus::Success();
		TWoTBootstrapResult<FString> Identity = WoTFrontEndObjectPath(Package, Reference);
		if (Identity.IsError())
			return FWoTBootstrapStatus::Error(MoveTemp(Identity).GetError());
		FString ObjectClassLeaf;
		if (Reference > 0)
		{
			const FString* Resolved =
				Package.ClassLeaf(Package.Exports[Reference - 1].ClassReference);
			if (Resolved != nullptr) ObjectClassLeaf = *Resolved;
		}
		else if (Package.Imports.IsValidIndex(-Reference - 1))
		{
			ObjectClassLeaf = Package.Imports[-Reference - 1].ClassName;
		}
		if (ObjectClassLeaf.IsEmpty())
			return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("an entry reference has no class"), Identity.GetValue()));
		const FString FoldedClass = WoTFrontEndFold(ObjectClassLeaf);
		if (FoldedClass == TEXT("texture"))
		{
			AddTypedReference(EWoTFactDomain::Texture, Identity.GetValue());
			return FWoTBootstrapStatus::Success();
		}
		if (FoldedClass == TEXT("sound"))
		{
			AddTypedReference(EWoTFactDomain::Sound, Identity.GetValue());
			return FWoTBootstrapStatus::Success();
		}
		if (FoldedClass == TEXT("mesh") || FoldedClass == TEXT("lodmesh"))
		{
			AddTypedReference(EWoTFactDomain::Mesh, Identity.GetValue());
			AddTypedReference(EWoTFactDomain::Animation, Identity.GetValue());
			return FWoTBootstrapStatus::Success();
		}
		if (FoldedClass == TEXT("font"))
		{
			AddTypedReference(EWoTFactDomain::Font, Identity.GetValue());
			return FWoTBootstrapStatus::Success();
		}
		if (FoldedClass == TEXT("firetexture")
			|| FoldedClass == TEXT("watertexture")
			|| FoldedClass == TEXT("wavetexture")
			|| FoldedClass == TEXT("wettexture")
			|| FoldedClass == TEXT("icetexture"))
		{
			AddTypedReference(EWoTFactDomain::Procedural, Identity.GetValue());
			return FWoTBootstrapStatus::Success();
		}
		// Internal non-resource references are admitted only by declared class.
		// The package-name prefix alone is not provenance.
		if (FoldedClass == TEXT("class") || FoldedClass == TEXT("levelinfo")
			|| FoldedClass == TEXT("level") || FoldedClass == TEXT("levelsummary")
			|| FoldedClass == TEXT("model"))
			return FWoTBootstrapStatus::Success();
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::UnknownField,
			TEXT("an entry reference has no semantic classification"),
			Identity.GetValue()));
	};

	for (int32 Reference : ActorReferences)
	{
		const FWoTFrontEndExport& Export = Package.Exports[Reference - 1];
		const FString* ClassLeaf = Package.ClassLeaf(Export.ClassReference);
		TWoTBootstrapResult<FString> Identity = WoTFrontEndObjectPath(Package, Reference);
		if (ClassLeaf == nullptr || Identity.IsError())
			return WoTFrontEndError<FWoTFrontEndSemantics>(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("an entry actor has no stable class or identity"));
		// UE1 serializes editor viewport Camera exports in the Level actor array.
		// They are not runtime scene actors; the canonical Level exporter applies
		// the same exclusion before producing the shipped semantic census.
		if (ClassLeaf->Equals(TEXT("Camera"), ESearchCase::IgnoreCase)) continue;
		Semantics.Actors.Add({*ClassLeaf, Identity.GetValue()});
		FWoTFrontEndReader ActorReader;
		TArray<FWoTTypedProperty> Properties;
		if (!WoTFrontEndOpenExport(Package, Export, ActorReader)
			|| !WoTFrontEndReadProperties(ActorReader, Package, Properties, PropertyCount))
			return WoTFrontEndError<FWoTFrontEndSemantics>(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("an entry actor property stream is malformed"), Identity.GetValue());
		FVector Location = FVector::ZeroVector;
		FRotator Rotation = FRotator::ZeroRotator;
		bool bHasLocation = false;
		bool bHasRotation = false;
		for (const FWoTTypedProperty& Property : Properties)
		{
			if (Property.ObjectReference.IsSet())
			{
				FWoTBootstrapStatus Added = AddReference(Property.ObjectReference.GetValue());
				if (Added.IsError())
					return FResult::Error(MoveTemp(Added).GetError());
			}
			if (Property.Name.Equals(TEXT("Location"), ESearchCase::IgnoreCase))
				bHasLocation = WoTFrontEndReadVector(Property, Location);
			if (Property.Name.Equals(TEXT("Rotation"), ESearchCase::IgnoreCase))
				bHasRotation = WoTFrontEndReadRotator(Property, Rotation);
			if (ClassLeaf->Equals(TEXT("RandomLightning"), ESearchCase::IgnoreCase)
				&& Property.Name.Equals(TEXT("ThunderSounds"), ESearchCase::IgnoreCase))
			{
				FString ShortIdentity;
				if (!WoTFrontEndReadDefaultString(Property, ShortIdentity)
					|| Semantics.ThunderSoundOverrides.Contains(Property.ArrayIndex))
					return WoTFrontEndError<FWoTFrontEndSemantics>(
						EWoTBootstrapError::CorruptInstallation,
						TEXT("a RandomLightning instance thunder override is malformed"));
				Semantics.ThunderSoundOverrides.Add(
					Property.ArrayIndex, MoveTemp(ShortIdentity));
			}
		}
		if (ClassLeaf->Equals(TEXT("PlayerStart"), ESearchCase::IgnoreCase))
		{
			if (bCamera || !bHasLocation || !bHasRotation)
				return WoTFrontEndError<FWoTFrontEndSemantics>(
					EWoTBootstrapError::CorruptInstallation,
					TEXT("the entry camera is missing, duplicate, or malformed"));
			bCamera = true;
			Semantics.CameraLocation = Location + FVector(0.0, 0.0,
				WoTFrontEndEyeHeight);
			Semantics.CameraRotation = Rotation;
		}
	}
	if (!bCamera)
		return WoTFrontEndError<FWoTFrontEndSemantics>(
			EWoTBootstrapError::UnknownField, TEXT("the entry camera is absent"));

	// Decode only the pure UModel prefix needed to close surface texture references.
	FWoTFrontEndReader ModelReader;
	TArray<FWoTTypedProperty> ModelProperties;
	if (!WoTFrontEndOpenExport(Package, Package.Exports[ModelReference - 1], ModelReader)
		|| !WoTFrontEndReadProperties(ModelReader, Package, ModelProperties, PropertyCount)
		|| !ModelReader.Skip(25 + 16))
		return WoTFrontEndError<FWoTFrontEndSemantics>(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("the entry BSP model header is malformed"));
	auto ReadCount = [&ModelReader](int32& Out)
	{
		return ModelReader.ReadCompact(Out) && Out >= 0
			&& static_cast<uint32>(Out) <= WoTFrontEndMaxModelItems;
	};
	int32 Count = 0;
	if (!ReadCount(Count) || !ModelReader.Skip(static_cast<uint64>(Count) * 12)
		|| !ReadCount(Count) || !ModelReader.Skip(static_cast<uint64>(Count) * 12)
		|| !ReadCount(Count))
		return WoTFrontEndError<FWoTFrontEndSemantics>(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("the entry BSP model vectors are malformed"));
	for (int32 Index = 0; Index < Count; ++Index)
	{
		int32 Compact = 0;
		uint64 ZoneMask = 0;
		uint8 Byte = 0;
		int32 Leaf = 0;
		if (!ModelReader.Skip(16) || !ModelReader.ReadU64(ZoneMask)
			|| !ModelReader.ReadU8(Byte))
			return WoTFrontEndError<FWoTFrontEndSemantics>(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("an entry BSP node is truncated"));
		for (int32 Field = 0; Field < 7; ++Field)
			if (!ModelReader.ReadCompact(Compact))
				return WoTFrontEndError<FWoTFrontEndSemantics>(
					EWoTBootstrapError::CorruptInstallation,
					TEXT("an entry BSP node is truncated"));
		if (!ModelReader.Skip(3) || !ModelReader.ReadI32(Leaf)
			|| !ModelReader.ReadI32(Leaf))
			return WoTFrontEndError<FWoTFrontEndSemantics>(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("an entry BSP node is truncated"));
	}
	if (!ReadCount(Count))
		return WoTFrontEndError<FWoTFrontEndSemantics>(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("the entry BSP surface count is malformed"));
	for (int32 Index = 0; Index < Count; ++Index)
	{
		int32 Texture = 0;
		int32 Compact = 0;
		uint32 Flags = 0;
		int16 Pan = 0;
		if (!ModelReader.ReadCompact(Texture) || !ModelReader.ReadU32(Flags))
			return WoTFrontEndError<FWoTFrontEndSemantics>(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("an entry BSP surface is truncated"));
		for (int32 Field = 0; Field < 6; ++Field)
			if (!ModelReader.ReadCompact(Compact))
				return WoTFrontEndError<FWoTFrontEndSemantics>(
					EWoTBootstrapError::CorruptInstallation,
					TEXT("an entry BSP surface is truncated"));
		if (!ModelReader.ReadI16(Pan) || !ModelReader.ReadI16(Pan)
			|| !ModelReader.ReadCompact(Compact))
			return WoTFrontEndError<FWoTFrontEndSemantics>(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("an entry BSP surface is truncated"));
		FWoTBootstrapStatus Added = AddReference(Texture);
		if (Added.IsError()) return FResult::Error(MoveTemp(Added).GetError());
	}
	Resources.GenerateValueArray(Semantics.ResourceReferences);
	Semantics.ResourceReferences.Sort(
		[](const FWoTFrontEndSemantics::FResourceReference& Left,
			const FWoTFrontEndSemantics::FResourceReference& Right)
		{
			const FString LK = WoTFrontEndResourceKey(Left.Domain, Left.Identity);
			const FString RK = WoTFrontEndResourceKey(Right.Domain, Right.Identity);
			return LK == RK ? Left.Identity < Right.Identity : LK < RK;
		});
	return FResult::Success(MoveTemp(Semantics));
}

bool WoTFrontEndSkipScript(FWoTFrontEndReader& Reader, int32 MemorySize)
{
	if (MemorySize < 0) return false;
	int64 MemoryOffset = 0;
	uint32 RecursionDepth = 0;
	auto Raw = [&Reader, &MemoryOffset](uint32 Size)
	{
		if (!Reader.Skip(Size)) return false;
		MemoryOffset += Size;
		return true;
	};
	auto Reference = [&Reader, &MemoryOffset](int32* Out)
	{
		int32 Value = 0;
		if (!Reader.ReadCompact(Value)) return false;
		MemoryOffset += 4;
		if (Out != nullptr) *Out = Value;
		return true;
	};
	TFunction<bool(uint8&)> Expression;
	TFunction<bool()> Parameters;
	Parameters = [&Expression]()
	{
		for (uint32 Index = 0; Index < WoTFrontEndMaxModelItems; ++Index)
		{
			uint8 Token = 0;
			if (!Expression(Token)) return false;
			if (Token == 0x16) return true;
		}
		return false;
	};
	Expression = [&Reader, &MemoryOffset, &RecursionDepth, &Raw,
		&Reference, &Expression, &Parameters](uint8& OutToken)
	{
		if (++RecursionDepth > 256) return false;
		uint8 Token = 0;
		if (!Reader.ReadU8(Token)) { --RecursionDepth; return false; }
		++MemoryOffset;
		bool bOk = true;
		auto OneExpression = [&Expression]()
		{
			uint8 Ignored = 0;
			return Expression(Ignored);
		};
		const bool bNoOperands = Token == 0x08 || Token == 0x0b || Token == 0x16
			|| Token == 0x17 || Token == 0x25 || Token == 0x26 || Token == 0x27
			|| Token == 0x28 || Token == 0x2a || Token == 0x2d || Token == 0x30
			|| Token == 0x31;
		const bool bObjectReference = Token == 0x00 || Token == 0x01
			|| Token == 0x02 || Token == 0x20 || Token == 0x29;
		if (Token >= 0x39 && Token < 0x60) bOk = OneExpression();
		else if (Token >= 0x70) bOk = Parameters();
		else if (Token >= 0x60) bOk = Raw(1) && Parameters();
		else if (Token == 0x0f || Token == 0x10
			|| Token == 0x14 || Token == 0x1a)
			bOk = OneExpression() && OneExpression();
		else if (Token == 0x06) bOk = Raw(2);
		else if (bObjectReference) bOk = Reference(nullptr);
		else if (bNoOperands) bOk = true;
		else if (Token == 0x04 || Token == 0x0d || Token == 0x0e)
			bOk = OneExpression();
		else if (Token == 0x1c) bOk = Reference(nullptr) && Parameters();
		else if (Token == 0x1b || Token == 0x38)
			bOk = Reference(nullptr) && Parameters();
		else if (Token == 0x12 || Token == 0x19)
			bOk = OneExpression() && Raw(3) && OneExpression();
		else if (Token == 0x11)
		{
			for (int32 Index = 0; bOk && Index < 4; ++Index)
				bOk = OneExpression();
		}
		else if (Token == 0x1d || Token == 0x1e) bOk = Raw(4);
		else if (Token == 0x1f)
		{
			bOk = false;
			for (uint64 Index = 0; Index < WoTFrontEndMaxSourceBytes; ++Index)
			{
				uint8 Byte = 0;
				if (!Reader.ReadU8(Byte)) break;
				++MemoryOffset;
				if (Byte == 0) { bOk = true; break; }
			}
		}
		else if (Token == 0x34)
		{
			bOk = false;
			for (uint64 Index = 0; Index < WoTFrontEndMaxSourceBytes / 2; ++Index)
			{
				uint16 Value = 0;
				if (!Reader.ReadU16(Value)) break;
				MemoryOffset += 2;
				if (Value == 0) { bOk = true; break; }
			}
		}
		else if (Token == 0x21) bOk = Reference(nullptr);
		else if (Token == 0x22 || Token == 0x23) bOk = Raw(12);
		else if (Token == 0x24 || Token == 0x2c) bOk = Raw(1);
		else if (Token == 0x13 || Token == 0x2e)
			bOk = Reference(nullptr) && OneExpression();
		else if (Token == 0x07) bOk = Raw(2) && OneExpression();
		else if (Token == 0x2f) bOk = OneExpression() && Raw(2);
		else if (Token == 0x05) bOk = Raw(1) && OneExpression();
		else if (Token == 0x09) bOk = Raw(2) && OneExpression();
		else if (Token == 0x0a)
		{
			uint16 Destination = 0;
			bOk = Reader.ReadU16(Destination);
			if (bOk) MemoryOffset += 2;
			if (bOk && Destination != 0xffff) bOk = OneExpression();
		}
		else if (Token == 0x0c)
		{
			bOk = (MemoryOffset & 3) == 0;
			for (uint32 Index = 0;
				bOk && Index < WoTFrontEndMaxModelItems; ++Index)
			{
				int32 Label = -1;
				bOk = Reference(&Label) && Raw(4);
				if (bOk && Label == 0) break;
				if (Index + 1 == WoTFrontEndMaxModelItems) bOk = false;
			}
		}
		else if (Token == 0x18) bOk = Raw(2) && OneExpression();
		else if (Token == 0x32 || Token == 0x33)
			bOk = Reference(nullptr) && OneExpression() && OneExpression();
		else if (Token == 0x36) bOk = Reference(nullptr) && OneExpression();
		else bOk = false;
		OutToken = Token;
		--RecursionDepth;
		return bOk;
	};
	for (uint32 Index = 0;
		MemoryOffset < MemorySize && Index < WoTFrontEndMaxModelItems; ++Index)
	{
		uint8 Token = 0;
		if (!Expression(Token)) return false;
	}
	return MemoryOffset == MemorySize;
}

TWoTBootstrapResult<TArray<FWoTTypedProperty>> WoTFrontEndClassDefaults(
	const FWoTFrontEndPackageView& Package, const TCHAR* ClassName,
	uint64& PropertyCount)
{
	using FResult = TWoTBootstrapResult<TArray<FWoTTypedProperty>>;
	const FWoTFrontEndExport* Match = nullptr;
	for (const FWoTFrontEndExport& Export : Package.Exports)
	{
		if (Export.ClassReference != 0 || Export.Name != ClassName) continue;
		if (Match != nullptr)
			return WoTFrontEndError<TArray<FWoTTypedProperty>>(
				EWoTBootstrapError::DuplicateField,
				TEXT("a required entry dependency class is ambiguous"), ClassName);
		Match = &Export;
	}
	if (Match == nullptr)
		return WoTFrontEndError<TArray<FWoTTypedProperty>>(
			EWoTBootstrapError::UnknownField,
			TEXT("a required entry dependency class is absent"), ClassName);
	FWoTFrontEndReader Reader;
	int32 Compact = 0;
	int32 FriendlyIndex = 0;
	int32 MemorySize = 0;
	int32 Ignored = 0;
	if (!WoTFrontEndOpenExport(Package, *Match, Reader)
		|| !Reader.ReadCompact(Compact) || !Reader.ReadCompact(Compact)
		|| !Reader.ReadCompact(Compact) || !Reader.ReadCompact(Compact)
		|| !Reader.ReadCompact(FriendlyIndex)
		|| Package.NameAt(FriendlyIndex) == nullptr
		|| !Reader.ReadI32(Ignored) || !Reader.ReadI32(Ignored)
		|| !Reader.ReadI32(MemorySize)
		|| !WoTFrontEndSkipScript(Reader, MemorySize) || !Reader.Skip(42))
		return WoTFrontEndError<TArray<FWoTTypedProperty>>(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("a required entry dependency class header is malformed"), ClassName);
	int32 DependencyCount = 0;
	if (!Reader.ReadCompact(DependencyCount) || DependencyCount < 0
		|| static_cast<uint32>(DependencyCount) > WoTFrontEndMaxModelItems)
		return WoTFrontEndError<TArray<FWoTTypedProperty>>(
			EWoTBootstrapError::SizeLimit,
			TEXT("an entry dependency class has too many dependencies"), ClassName);
	for (int32 Index = 0; Index < DependencyCount; ++Index)
	{
		uint32 Crc = 0;
		if (!Reader.ReadCompact(Compact) || !Reader.Skip(4) || !Reader.ReadU32(Crc))
			return WoTFrontEndError<TArray<FWoTTypedProperty>>(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("an entry dependency class list is malformed"), ClassName);
	}
	int32 ImportCount = 0;
	if (!Reader.ReadCompact(ImportCount) || ImportCount < 0
		|| static_cast<uint32>(ImportCount) > WoTFrontEndMaxModelItems)
		return WoTFrontEndError<TArray<FWoTTypedProperty>>(
			EWoTBootstrapError::SizeLimit,
			TEXT("an entry dependency class has too many imports"), ClassName);
	for (int32 Index = 0; Index < ImportCount; ++Index)
	{
		FString Imported;
		if (!WoTFrontEndReadName(Reader, Package, Imported))
			return WoTFrontEndError<TArray<FWoTTypedProperty>>(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("an entry dependency class import is malformed"), ClassName);
	}
	FString Config;
	TArray<FWoTTypedProperty> Properties;
	if (!Reader.ReadCompact(Compact) || !WoTFrontEndReadName(Reader, Package, Config)
		|| !WoTFrontEndReadProperties(
			Reader, Package, Properties, PropertyCount, true)
		|| Reader.Tell() != Reader.GetEnd())
		return WoTFrontEndError<TArray<FWoTTypedProperty>>(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("entry dependency class defaults are malformed"), ClassName);
	return FResult::Success(MoveTemp(Properties));
}

bool WoTFrontEndReadDefaultString(const FWoTTypedProperty& Property, FString& Out)
{
	if (Property.Type != 13) return false;
	FWoTFrontEndReader Reader(Property.Value);
	int32 Length = 0;
	TArray<uint8> Raw;
	if (!Reader.ReadCompact(Length) || Length <= 0 || Length > 1024
		|| !Reader.ReadBytes(static_cast<uint64>(Length), Raw)
		|| Reader.Tell() != Reader.GetEnd() || Raw.Last() != 0)
		return false;
	Raw.Pop(EAllowShrinking::No);
	return WoTFrontEndLatin1(Raw, Out) && !Out.IsEmpty();
}

FWoTBootstrapStatus WoTFrontEndAddGraphResource(
	FWoTFrontEndSemantics& Semantics, EWoTFactDomain Domain,
	FString Identity, const TCHAR* Context)
{
	const FString Key = WoTFrontEndResourceKey(Domain, Identity);
	for (const FWoTFrontEndSemantics::FResourceReference& Existing
		: Semantics.ResourceReferences)
	{
		if (WoTFrontEndResourceKey(Existing.Domain, Existing.Identity) == Key)
			return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::DuplicateField,
				TEXT("the typed entry source graph has a duplicate edge"), Context));
	}
	Semantics.ResourceReferences.Add({Domain, MoveTemp(Identity)});
	return FWoTBootstrapStatus::Success();
}

TWoTBootstrapResult<FWoTFrontEndSemantics> WoTFrontEndCompleteResourceGraph(
	FWoTFrontEndSemantics&& Semantics,
	const FWoTFrontEndPackageView& Environment,
	const FWoTFrontEndPackageView& WOT)
{
	using FResult = TWoTBootstrapResult<FWoTFrontEndSemantics>;
	if (Environment.PackageName != TEXT("Environment")
		|| WOT.PackageName != TEXT("WOT"))
		return WoTFrontEndError<FWoTFrontEndSemantics>(
			EWoTBootstrapError::UnknownField,
			TEXT("the entry dependency packages have the wrong identity"));
	uint64 PropertyCount = 0;
	TWoTBootstrapResult<TArray<FWoTTypedProperty>> Rain =
		WoTFrontEndClassDefaults(Environment, TEXT("RainCyd"), PropertyCount);
	if (Rain.IsError()) return FResult::Error(MoveTemp(Rain).GetError());
	for (const TCHAR* PropertyName : {TEXT("Mesh"), TEXT("Skin")})
	{
		const FWoTTypedProperty* Property = Rain.GetValue().FindByPredicate(
			[PropertyName](const FWoTTypedProperty& Candidate)
			{
				return Candidate.Name == PropertyName;
			});
		if (Property == nullptr || !Property->ObjectReference.IsSet()
			|| Property->ObjectReference.GetValue() == 0)
			return WoTFrontEndError<FWoTFrontEndSemantics>(
				EWoTBootstrapError::UnknownField,
				TEXT("RainCyd lacks a required typed mesh or skin"), PropertyName);
		const int32 Reference = Property->ObjectReference.GetValue();
		TWoTBootstrapResult<FString> Identity =
			WoTFrontEndObjectPath(Environment, Reference);
		const FString* Class = Reference > 0
			? Environment.ClassLeaf(Environment.Exports[Reference - 1].ClassReference)
			: Environment.ClassLeaf(Reference);
		if (Identity.IsError() || Class == nullptr)
			return WoTFrontEndError<FWoTFrontEndSemantics>(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("RainCyd has an invalid typed mesh or skin"), PropertyName);
		if (FString(PropertyName) == TEXT("Mesh")
			&& Class->Equals(TEXT("Mesh"), ESearchCase::CaseSensitive))
		{
			for (EWoTFactDomain Domain : {
				EWoTFactDomain::Animation, EWoTFactDomain::Mesh})
			{
				FWoTBootstrapStatus Added = WoTFrontEndAddGraphResource(
					Semantics, Domain, Identity.GetValue(), PropertyName);
				if (Added.IsError()) return FResult::Error(MoveTemp(Added).GetError());
			}
		}
		else if (FString(PropertyName) == TEXT("Skin")
			&& Class->Equals(TEXT("Texture"), ESearchCase::CaseSensitive))
		{
			FWoTBootstrapStatus Added = WoTFrontEndAddGraphResource(
				Semantics, EWoTFactDomain::Texture,
				MoveTemp(Identity).GetValue(), PropertyName);
			if (Added.IsError()) return FResult::Error(MoveTemp(Added).GetError());
		}
		else
			return WoTFrontEndError<FWoTFrontEndSemantics>(
				EWoTBootstrapError::UnknownField,
				TEXT("RainCyd mesh or skin has the wrong class"), PropertyName);
	}
	TWoTBootstrapResult<TArray<FWoTTypedProperty>> Lightning =
		WoTFrontEndClassDefaults(Environment, TEXT("RandomLightning"), PropertyCount);
	if (Lightning.IsError()) return FResult::Error(MoveTemp(Lightning).GetError());
	TArray<const FWoTTypedProperty*> Thunder;
	for (const FWoTTypedProperty& Property : Lightning.GetValue())
		if (Property.Name == TEXT("ThunderSounds")) Thunder.Add(&Property);
	Thunder.Sort([](const FWoTTypedProperty& Left, const FWoTTypedProperty& Right)
	{
		return Left.ArrayIndex < Right.ArrayIndex;
	});
	if (Thunder.Num() != 6)
		return WoTFrontEndError<FWoTFrontEndSemantics>(
			EWoTBootstrapError::UnknownField,
			TEXT("RandomLightning must declare six thunder sounds"));
	TArray<FString> EffectiveThunder;
	EffectiveThunder.SetNum(10);
	TSet<FString> DefaultThunder;
	auto ResolveThunder = [&Environment](FStringView ShortIdentity)
		-> TWoTBootstrapResult<FString>
	{
		const FString Leaf = WoTFrontEndLeaf(ShortIdentity);
		if (!FString(ShortIdentity).Equals(
			FString::Printf(TEXT("Environment.%s"), *Leaf),
			ESearchCase::CaseSensitive))
			return WoTFrontEndError<FString>(EWoTBootstrapError::UnknownField,
				TEXT("a thunder reference has the wrong source root"),
				FString(ShortIdentity));
		int32 Match = 0;
		for (int32 ExportIndex = 0; ExportIndex < Environment.Exports.Num(); ++ExportIndex)
		{
			const FWoTFrontEndExport& Export = Environment.Exports[ExportIndex];
			const FString* Class = Environment.ClassLeaf(Export.ClassReference);
			if (Export.Name.Equals(Leaf, ESearchCase::CaseSensitive)
				&& Class != nullptr
				&& Class->Equals(TEXT("Sound"), ESearchCase::CaseSensitive))
			{
				if (Match != 0)
					return WoTFrontEndError<FString>(EWoTBootstrapError::DuplicateField,
						TEXT("a thunder reference is ambiguous"), FString(ShortIdentity));
				Match = ExportIndex + 1;
			}
		}
		TWoTBootstrapResult<FString> Identity =
			WoTFrontEndObjectPath(Environment, Match);
		if (Match == 0 || Identity.IsError())
			return WoTFrontEndError<FString>(EWoTBootstrapError::UnknownField,
				TEXT("a thunder reference does not resolve to a Sound"),
				FString(ShortIdentity));
		return Identity;
	};
	for (int32 Index = 0; Index < Thunder.Num(); ++Index)
	{
		FString ShortIdentity;
		if (Thunder[Index]->ArrayIndex != static_cast<uint32>(Index)
			|| !WoTFrontEndReadDefaultString(*Thunder[Index], ShortIdentity))
			return WoTFrontEndError<FWoTFrontEndSemantics>(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("a RandomLightning thunder reference is malformed"));
		TWoTBootstrapResult<FString> Identity = ResolveThunder(ShortIdentity);
		if (Identity.IsError())
			return FResult::Error(MoveTemp(Identity).GetError());
		EffectiveThunder[Index] = Identity.GetValue();
		DefaultThunder.Add(WoTFrontEndFold(Identity.GetValue()));
		FWoTBootstrapStatus Added = WoTFrontEndAddGraphResource(
			Semantics, EWoTFactDomain::Sound,
			Identity.GetValue(), TEXT("RandomLightning.ThunderSounds"));
		if (Added.IsError()) return FResult::Error(MoveTemp(Added).GetError());
	}
	if (Semantics.ThunderSoundOverrides.Num() != 4)
		return WoTFrontEndError<FWoTFrontEndSemantics>(
			EWoTBootstrapError::UnknownField,
			TEXT("RandomLightning must declare four instance thunder overrides"));
	for (uint32 Index = 6; Index < 10; ++Index)
	{
		const FString* Override = Semantics.ThunderSoundOverrides.Find(Index);
		if (Override == nullptr)
			return WoTFrontEndError<FWoTFrontEndSemantics>(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("RandomLightning thunder overrides are not contiguous"));
		TWoTBootstrapResult<FString> Identity = ResolveThunder(*Override);
		if (Identity.IsError()) return FResult::Error(MoveTemp(Identity).GetError());
		if (!DefaultThunder.Contains(WoTFrontEndFold(Identity.GetValue())))
			return WoTFrontEndError<FWoTFrontEndSemantics>(
				EWoTBootstrapError::UnknownField,
				TEXT("a RandomLightning override is outside the closed thunder set"),
				Identity.GetValue());
		EffectiveThunder[Index] = MoveTemp(Identity).GetValue();
	}
	Semantics.ThunderSoundSourceExports = MoveTemp(EffectiveThunder);
	for (const TCHAR* FontIdentity : {TEXT("WOT.F_WOTReg14"),
		TEXT("WOT.F_WOTReg30")})
	{
		const FWoTFrontEndExport* Font = nullptr;
		for (int32 ExportIndex = 0; ExportIndex < WOT.Exports.Num(); ++ExportIndex)
		{
			const FWoTFrontEndExport& Export = WOT.Exports[ExportIndex];
			const FString* Class = WOT.ClassLeaf(Export.ClassReference);
			TWoTBootstrapResult<FString> Identity =
				WoTFrontEndObjectPath(WOT, ExportIndex + 1);
			if (Class != nullptr && *Class == TEXT("Font") && !Identity.IsError()
				&& Identity.GetValue() == FontIdentity)
			{
				if (Font != nullptr)
					return WoTFrontEndError<FWoTFrontEndSemantics>(
						EWoTBootstrapError::DuplicateField,
						TEXT("a required menu font is ambiguous"), FontIdentity);
				Font = &Export;
			}
		}
		if (Font == nullptr)
			return WoTFrontEndError<FWoTFrontEndSemantics>(
				EWoTBootstrapError::UnknownField,
				TEXT("a required menu font is absent"), FontIdentity);
		FWoTBootstrapStatus AddedFont = WoTFrontEndAddGraphResource(
			Semantics, EWoTFactDomain::Font, FontIdentity, TEXT("menu font"));
		if (AddedFont.IsError()) return FResult::Error(MoveTemp(AddedFont).GetError());
		FWoTFrontEndReader Reader;
		TArray<FWoTTypedProperty> FontProperties;
		int32 PageCount = 0;
		int32 TextureReference = 0;
		int32 CharacterCount = 0;
		if (!WoTFrontEndOpenExport(WOT, *Font, Reader)
			|| !WoTFrontEndReadProperties(
				Reader, WOT, FontProperties, PropertyCount)
			|| !Reader.ReadCompact(PageCount) || PageCount != 1
			|| !Reader.ReadCompact(TextureReference) || TextureReference <= 0
			|| !WOT.Exports.IsValidIndex(TextureReference - 1)
			|| WOT.ClassLeaf(WOT.Exports[TextureReference - 1].ClassReference) == nullptr
			|| *WOT.ClassLeaf(WOT.Exports[TextureReference - 1].ClassReference)
				!= TEXT("Texture")
			|| !Reader.ReadCompact(CharacterCount) || CharacterCount < 0
			|| CharacterCount > 65536
			|| !Reader.Skip(static_cast<uint64>(CharacterCount) * 16)
			|| !Reader.ReadI32(CharacterCount) || CharacterCount <= 0
			|| Reader.Tell() != Reader.GetEnd())
			return WoTFrontEndError<FWoTFrontEndSemantics>(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("a required menu font atlas graph is malformed"), FontIdentity);
		TWoTBootstrapResult<FString> TextureIdentity =
			WoTFrontEndObjectPath(WOT, TextureReference);
		if (TextureIdentity.IsError())
			return FResult::Error(MoveTemp(TextureIdentity).GetError());
		FWoTBootstrapStatus AddedTexture = WoTFrontEndAddGraphResource(
			Semantics, EWoTFactDomain::Texture,
			MoveTemp(TextureIdentity).GetValue(), TEXT("menu font atlas"));
		if (AddedTexture.IsError())
			return FResult::Error(MoveTemp(AddedTexture).GetError());
	}
	for (const FWoTFrontEndSemantics::FResourceReference& Required : {
		FWoTFrontEndSemantics::FResourceReference{
			EWoTFactDomain::Localization, TEXT("WoT.int")},
		FWoTFrontEndSemantics::FResourceReference{
			EWoTFactDomain::Sound, TEXT("WOT.MoveSelector")},
		FWoTFrontEndSemantics::FResourceReference{
			EWoTFactDomain::Sound, TEXT("WOT.Select")}})
	{
		FWoTBootstrapStatus Added = WoTFrontEndAddGraphResource(
			Semantics, Required.Domain, Required.Identity, TEXT("required menu root"));
		if (Added.IsError()) return FResult::Error(MoveTemp(Added).GetError());
	}
	Semantics.ResourceReferences.Sort(
		[](const FWoTFrontEndSemantics::FResourceReference& Left,
			const FWoTFrontEndSemantics::FResourceReference& Right)
		{
			const FString LK = WoTFrontEndResourceKey(Left.Domain, Left.Identity);
			const FString RK = WoTFrontEndResourceKey(Right.Domain, Right.Identity);
			return LK == RK ? Left.Identity < Right.Identity : LK < RK;
		});
	return FResult::Success(MoveTemp(Semantics));
}

bool WoTFrontEndSafeLogicalId(FStringView LogicalId)
{
	if (LogicalId.IsEmpty() || LogicalId.Len() > 1024
		|| !LogicalId.StartsWith(TEXT("cache/"), ESearchCase::CaseSensitive)
		|| LogicalId.Contains(TEXT("\\")) || LogicalId.Contains(TEXT("//")))
		return false;
	TArray<FString> Components;
	FString(LogicalId).ParseIntoArray(Components, TEXT("/"), false);
	if (Components.Num() < 2) return false;
	for (const FString& Component : Components)
	{
		if (Component.IsEmpty() || Component == TEXT(".") || Component == TEXT(".."))
			return false;
		for (TCHAR Character : Component)
			if (!(FChar::IsAlnum(Character) || Character == TEXT('_')
				|| Character == TEXT('-') || Character == TEXT('.'))) return false;
	}
	return true;
}

struct FWoTFrontEndContractClosure
{
	const FWoTRequiredFact* BSP = nullptr;
	const FWoTRequiredFact* World = nullptr;
	TArray<const FWoTRequiredFact*> Resources;
	TArray<FWoTFrontEndResourceRequirement> Requirements;
};

TWoTBootstrapResult<FWoTFrontEndContractClosure> WoTFrontEndCloseContract(
	const FWoTFrontEndSemantics& Semantics, const FWoTRetailContract& Contract)
{
	using FResult = TWoTBootstrapResult<FWoTFrontEndContractClosure>;
	FWoTFrontEndContractClosure Closure;
	TSet<FString> AllLogicalIds;
	for (const FWoTRequiredFact& Fact : Contract.Facts)
	{
		for (const FWoTRequiredFactOutput& Output : Fact.Outputs)
		{
			if (!WoTFrontEndSafeLogicalId(Output.LogicalId)
				|| AllLogicalIds.Contains(Output.LogicalId))
				return WoTFrontEndError<FWoTFrontEndContractClosure>(
					EWoTBootstrapError::DuplicateField,
					TEXT("the entry contract contains an unsafe or duplicate logical ID"),
					Output.LogicalId);
			AllLogicalIds.Add(Output.LogicalId);
		}
		if (!Fact.SourcePath.Equals(WoTFrontEndSourcePath,
			ESearchCase::IgnoreCase)) continue;
		if (Fact.Domain == EWoTFactDomain::BSP)
		{
			if (Closure.BSP != nullptr)
				return WoTFrontEndError<FWoTFrontEndContractClosure>(
					EWoTBootstrapError::DuplicateField,
					TEXT("the entry contract has duplicate BSP facts"));
			Closure.BSP = &Fact;
		}
		if (Fact.Domain == EWoTFactDomain::World)
		{
			if (Closure.World != nullptr)
				return WoTFrontEndError<FWoTFrontEndContractClosure>(
					EWoTBootstrapError::DuplicateField,
					TEXT("the entry contract has duplicate world facts"));
			Closure.World = &Fact;
		}
	}
	if (Closure.BSP == nullptr || Closure.World == nullptr
		|| Closure.BSP->Outputs.Num() != 1 || Closure.World->Outputs.Num() != 1
		|| !Closure.BSP->SourceExport.Equals(TEXT("WheelOfTime.Model"),
			ESearchCase::IgnoreCase)
		|| !Closure.World->SourceExport.Equals(TEXT("WheelOfTime.Level"),
			ESearchCase::IgnoreCase))
		return WoTFrontEndError<FWoTFrontEndContractClosure>(
			EWoTBootstrapError::UnknownField,
			TEXT("the entry contract lacks its singular BSP/world association"));
	TSet<FString> Expected;
	for (const FWoTFrontEndExpectedResource& Resource : WoTFrontEndExpectedResources)
		Expected.Add(WoTFrontEndResourceKey(Resource.Domain, Resource.SourceExport));
	TSet<FString> Observed;
	for (const FWoTFrontEndSemantics::FResourceReference& Reference
		: Semantics.ResourceReferences)
	{
		const FString Key = WoTFrontEndResourceKey(
			Reference.Domain, Reference.Identity);
		if (Observed.Contains(Key))
			return WoTFrontEndError<FWoTFrontEndContractClosure>(
				EWoTBootstrapError::DuplicateField,
				TEXT("the entry resource closure contains a duplicate typed edge"),
				Reference.Identity);
		Observed.Add(Key);
	}
	bool bExactGraph = Observed.Num() == Expected.Num();
	for (const FString& Key : Expected)
		bExactGraph &= Observed.Contains(Key);
	if (!bExactGraph)
		return WoTFrontEndError<FWoTFrontEndContractClosure>(
			EWoTBootstrapError::UnknownField,
			TEXT("the typed entry closure differs from the pinned source graph"));
	TSet<FString> SeenReferences;
	for (const FWoTFrontEndSemantics::FResourceReference& Reference
		: Semantics.ResourceReferences)
	{
		const FString Folded = WoTFrontEndResourceKey(
			Reference.Domain, Reference.Identity);
		if (SeenReferences.Contains(Folded))
			return WoTFrontEndError<FWoTFrontEndContractClosure>(
				EWoTBootstrapError::DuplicateField,
				TEXT("the entry resource closure contains a duplicate"),
				Reference.Identity);
		SeenReferences.Add(Folded);
		const FWoTRequiredFact* Match = nullptr;
		for (const FWoTRequiredFact& Fact : Contract.Facts)
		{
			if (Fact.Domain != Reference.Domain
				|| !Fact.SourceExport.Equals(
					Reference.Identity, ESearchCase::IgnoreCase)) continue;
			if (Match != nullptr)
				return WoTFrontEndError<FWoTFrontEndContractClosure>(
					EWoTBootstrapError::DuplicateField,
					TEXT("an entry resource has duplicate typed contract authorization"),
					Reference.Identity);
			Match = &Fact;
		}
		if (Match == nullptr || Match->Outputs.Num() != 1
			|| !Match->SourcePath.Equals(
				WoTFrontEndExpectedSourcePath(Reference.Identity),
				ESearchCase::CaseSensitive))
			return WoTFrontEndError<FWoTFrontEndContractClosure>(
				EWoTBootstrapError::UnknownField,
				TEXT("an entry resource lacks one exact typed contract fact"),
				Reference.Identity);
		Closure.Resources.Add(Match);
		Closure.Requirements.Add({Match->Domain, Match->SourcePath,
			Match->SourceExport, Match->Outputs[0].LogicalId});
	}
	Closure.Requirements.Add({EWoTFactDomain::BSP, Closure.BSP->SourcePath,
		Closure.BSP->SourceExport, Closure.BSP->Outputs[0].LogicalId});
	Closure.Requirements.Add({EWoTFactDomain::World, Closure.World->SourcePath,
		Closure.World->SourceExport, Closure.World->Outputs[0].LogicalId});
	Closure.Requirements.Sort([](const FWoTFrontEndResourceRequirement& Left,
		const FWoTFrontEndResourceRequirement& Right)
	{
		const FString LD = WoTFrontEndDomainName(Left.Domain);
		const FString RD = WoTFrontEndDomainName(Right.Domain);
		if (LD != RD) return LD < RD;
		const FString LE = WoTFrontEndFold(Left.SourceExport);
		const FString RE = WoTFrontEndFold(Right.SourceExport);
		if (LE != RE) return LE < RE;
		return Left.LogicalId < Right.LogicalId;
	});
	return FResult::Success(MoveTemp(Closure));
}

TWoTBootstrapResult<FWoTCanonicalJsonValue> WoTFrontEndDescriptorJson(
	const FWoTFrontEndDescriptor& Descriptor, bool bIncludeRoot)
{
	using FResult = TWoTBootstrapResult<FWoTCanonicalJsonValue>;
	auto FloatBits = [](float Value)
	{
		uint32 Bits = 0;
		FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
		return static_cast<int64>(Bits);
	};
	auto DoubleBits = [](double Value)
	{
		uint64 Bits = 0;
		FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
		return static_cast<int64>(Bits);
	};
	FWoTCanonicalJsonValue Location = FWoTCanonicalJsonValue::MakeArray();
	Location.AddElement(FWoTCanonicalJsonValue::MakeInteger(
		DoubleBits(Descriptor.CameraLocation.X)));
	Location.AddElement(FWoTCanonicalJsonValue::MakeInteger(
		DoubleBits(Descriptor.CameraLocation.Y)));
	Location.AddElement(FWoTCanonicalJsonValue::MakeInteger(
		DoubleBits(Descriptor.CameraLocation.Z)));
	FWoTCanonicalJsonValue Rotation = FWoTCanonicalJsonValue::MakeArray();
	Rotation.AddElement(FWoTCanonicalJsonValue::MakeInteger(
		DoubleBits(Descriptor.CameraRotation.Pitch)));
	Rotation.AddElement(FWoTCanonicalJsonValue::MakeInteger(
		DoubleBits(Descriptor.CameraRotation.Yaw)));
	Rotation.AddElement(FWoTCanonicalJsonValue::MakeInteger(
		DoubleBits(Descriptor.CameraRotation.Roll)));
	FWoTCanonicalObjectBuilder Camera;
	Camera.Add(TEXT("horizontal_fov_bits"),
		FWoTCanonicalJsonValue::MakeInteger(FloatBits(Descriptor.HorizontalFov)));
	Camera.Add(TEXT("location_bits"), MoveTemp(Location));
	Camera.Add(TEXT("rotation_bits"), MoveTemp(Rotation));
	FResult CameraJson = MoveTemp(Camera).Build(TEXT("front-end camera"));
	if (CameraJson.IsError()) return CameraJson;
	FWoTCanonicalObjectBuilder Census;
	Census.Add(TEXT("ambient_sounds"), FWoTCanonicalJsonValue::MakeInteger(
		Descriptor.Census.AmbientSounds));
	Census.Add(TEXT("lightning_actors"), FWoTCanonicalJsonValue::MakeInteger(
		Descriptor.Census.LightningActors));
	Census.Add(TEXT("lights"), FWoTCanonicalJsonValue::MakeInteger(
		Descriptor.Census.Lights));
	Census.Add(TEXT("rain_cylinders"), FWoTCanonicalJsonValue::MakeInteger(
		Descriptor.Census.RainCylinders));
	Census.Add(TEXT("random_lightning_controllers"),
		FWoTCanonicalJsonValue::MakeInteger(
			Descriptor.Census.RandomLightningControllers));
	Census.Add(TEXT("splash_emitters"), FWoTCanonicalJsonValue::MakeInteger(
		Descriptor.Census.SplashEmitters));
	FResult CensusJson = MoveTemp(Census).Build(TEXT("front-end census"));
	if (CensusJson.IsError()) return CensusJson;
	FWoTCanonicalJsonValue Required = FWoTCanonicalJsonValue::MakeArray();
	for (const FString& LogicalId : Descriptor.RequiredLogicalIds)
		Required.AddElement(FWoTCanonicalJsonValue::MakeString(LogicalId));
	FWoTCanonicalJsonValue Thunder = FWoTCanonicalJsonValue::MakeArray();
	for (const FString& SourceExport : Descriptor.ThunderSoundSourceExports)
		Thunder.AddElement(FWoTCanonicalJsonValue::MakeString(SourceExport));
	FWoTCanonicalJsonValue Resources = FWoTCanonicalJsonValue::MakeArray();
	for (const FWoTFrontEndResourceRequirement& Requirement
		: Descriptor.RequiredResources)
	{
		FWoTCanonicalObjectBuilder Resource;
		Resource.Add(TEXT("domain"), FWoTCanonicalJsonValue::MakeString(
			WoTFrontEndDomainName(Requirement.Domain)));
		Resource.Add(TEXT("logical_id"), FWoTCanonicalJsonValue::MakeString(
			Requirement.LogicalId));
		Resource.Add(TEXT("source_export"), FWoTCanonicalJsonValue::MakeString(
			Requirement.SourceExport));
		Resource.Add(TEXT("source_path"), FWoTCanonicalJsonValue::MakeString(
			Requirement.SourcePath));
		FResult ResourceJson = MoveTemp(Resource).Build(
			TEXT("front-end resource requirement"));
		if (ResourceJson.IsError()) return ResourceJson;
		Resources.AddElement(MoveTemp(ResourceJson).GetValue());
	}
	FWoTCanonicalObjectBuilder Root;
	Root.Add(TEXT("bsp_logical_id"), FWoTCanonicalJsonValue::MakeString(
		Descriptor.BspLogicalId));
	Root.Add(TEXT("camera"), MoveTemp(CameraJson).GetValue());
	Root.Add(TEXT("census"), MoveTemp(CensusJson).GetValue());
	if (bIncludeRoot)
		Root.Add(TEXT("descriptor_root"), FWoTCanonicalJsonValue::MakeString(
			Descriptor.DescriptorRoot.ToHex()));
	Root.Add(TEXT("map_name"), FWoTCanonicalJsonValue::MakeString(
		Descriptor.MapName));
	Root.Add(TEXT("required_logical_ids"), MoveTemp(Required));
	Root.Add(TEXT("required_resources"), MoveTemp(Resources));
	Root.Add(TEXT("source_root"), FWoTCanonicalJsonValue::MakeString(
		Descriptor.SourceRoot.ToHex()));
	Root.Add(TEXT("thunder_sound_source_exports"), MoveTemp(Thunder));
	Root.Add(TEXT("world_logical_id"), FWoTCanonicalJsonValue::MakeString(
		Descriptor.WorldLogicalId));
	return MoveTemp(Root).Build(TEXT("front-end descriptor"));
}

TWoTBootstrapResult<FWoTFrontEndDescriptor> WoTFrontEndBuild(
	FWoTFrontEndSemantics&& Semantics, const FWoTRetailContract& Contract)
{
	using FResult = TWoTBootstrapResult<FWoTFrontEndDescriptor>;
	if (Semantics.MapName != WoTFrontEndMapName)
		return WoTFrontEndError<FWoTFrontEndDescriptor>(EWoTBootstrapError::UnknownField,
			TEXT("the entry map identity is not WheelOfTime"), Semantics.MapName);
	TMap<FString, uint32> ClassCounts;
	TSet<FString> ActorIdentities;
	static const TSet<FString> AllowedClasses = {
		TEXT("levelinfo"), TEXT("brush"), TEXT("playerstart"),
		TEXT("wotzoneinfo"), TEXT("light"), TEXT("particlesprayer"),
		TEXT("ambientsound"), TEXT("randomlightning"), TEXT("lightning"),
		TEXT("waterzone"), TEXT("skyzoneinfo"), TEXT("raincyd")};
	for (const TPair<FString, FString>& Actor : Semantics.Actors)
	{
		const FString FoldedClass = WoTFrontEndFold(WoTFrontEndLeaf(Actor.Key));
		const FString FoldedIdentity = WoTFrontEndFold(Actor.Value);
		if (!AllowedClasses.Contains(FoldedClass))
			return WoTFrontEndError<FWoTFrontEndDescriptor>(
				EWoTBootstrapError::UnknownField,
				TEXT("the entry world contains an unclassified or campaign actor"),
				Actor.Value);
		if (Actor.Value.IsEmpty() || ActorIdentities.Contains(FoldedIdentity))
			return WoTFrontEndError<FWoTFrontEndDescriptor>(
				EWoTBootstrapError::DuplicateField,
				TEXT("the entry actor registry is missing or duplicate"), Actor.Value);
		ActorIdentities.Add(FoldedIdentity);
		++ClassCounts.FindOrAdd(FoldedClass);
	}
	const TPair<const TCHAR*, uint32> ExactClasses[] = {
		{TEXT("levelinfo"), 1}, {TEXT("brush"), 76}, {TEXT("playerstart"), 1},
		{TEXT("wotzoneinfo"), 1}, {TEXT("light"), 39},
		{TEXT("particlesprayer"), 4}, {TEXT("ambientsound"), 5},
		{TEXT("randomlightning"), 1}, {TEXT("lightning"), 5},
		{TEXT("waterzone"), 1}, {TEXT("skyzoneinfo"), 1}, {TEXT("raincyd"), 6}};
	for (const TPair<const TCHAR*, uint32>& Expected : ExactClasses)
		if (ClassCounts.FindRef(Expected.Key) != Expected.Value)
			return WoTFrontEndError<FWoTFrontEndDescriptor>(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("the entry semantic census disagrees with the pinned retail scene"),
				Expected.Key);
	if (!FMath::IsFinite(Semantics.CameraLocation.X)
		|| !FMath::IsFinite(Semantics.CameraLocation.Y)
		|| !FMath::IsFinite(Semantics.CameraLocation.Z)
		|| FMath::Abs(Semantics.CameraLocation.X) > WoTFrontEndMaxCoordinate
		|| FMath::Abs(Semantics.CameraLocation.Y) > WoTFrontEndMaxCoordinate
		|| FMath::Abs(Semantics.CameraLocation.Z) > WoTFrontEndMaxCoordinate
		|| Semantics.HorizontalFov != 90.0f)
		return WoTFrontEndError<FWoTFrontEndDescriptor>(
			EWoTBootstrapError::InvalidNumber,
			TEXT("the entry camera is malformed"));
	TWoTBootstrapResult<FWoTFrontEndContractClosure> Closure =
		WoTFrontEndCloseContract(Semantics, Contract);
	if (Closure.IsError()) return FResult::Error(MoveTemp(Closure).GetError());
	FWoTFrontEndDescriptor Descriptor;
	Descriptor.MapName = Semantics.MapName;
	Descriptor.BspLogicalId = Closure.GetValue().BSP->Outputs[0].LogicalId;
	Descriptor.WorldLogicalId = Closure.GetValue().World->Outputs[0].LogicalId;
	Descriptor.CameraLocation = Semantics.CameraLocation;
	Descriptor.CameraRotation = Semantics.CameraRotation;
	Descriptor.HorizontalFov = Semantics.HorizontalFov;
	Descriptor.Census.Lights = ClassCounts.FindRef(TEXT("light"));
	Descriptor.Census.AmbientSounds = ClassCounts.FindRef(TEXT("ambientsound"));
	Descriptor.Census.SplashEmitters = ClassCounts.FindRef(TEXT("particlesprayer"));
	Descriptor.Census.RainCylinders = ClassCounts.FindRef(TEXT("raincyd"));
	Descriptor.Census.LightningActors = ClassCounts.FindRef(TEXT("lightning"));
	Descriptor.Census.RandomLightningControllers =
		ClassCounts.FindRef(TEXT("randomlightning"));
	Descriptor.ThunderSoundSourceExports = Semantics.ThunderSoundSourceExports;
	Descriptor.RequiredResources = Closure.GetValue().Requirements;
	for (const FWoTFrontEndResourceRequirement& Requirement
		: Descriptor.RequiredResources)
		Descriptor.RequiredLogicalIds.Add(Requirement.LogicalId);
	Descriptor.RequiredLogicalIds.Sort();
	Descriptor.SourceRoot = Semantics.SourceRoot;
	TWoTBootstrapResult<FWoTSha256> Root = ComputeFrontEndDescriptorRoot(Descriptor);
	if (Root.IsError()) return FResult::Error(MoveTemp(Root).GetError());
	Descriptor.DescriptorRoot = MoveTemp(Root).GetValue();
	FWoTBootstrapStatus Validated = ValidateFrontEndDescriptor(Descriptor);
	if (Validated.IsError()) return FResult::Error(MoveTemp(Validated).GetError());
	return FResult::Success(MoveTemp(Descriptor));
}

class FWoTFrontEndCollectSink final : public IWoTByteSink
{
public:
	explicit FWoTFrontEndCollectSink(uint64 InLimit) : Limit(InLimit) {}
	FWoTBootstrapStatus Consume(TConstArrayView<uint8> Chunk) override
	{
		if (static_cast<uint64>(Chunk.Num()) > Limit - Written)
			return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::SizeLimit,
				TEXT("an entry dependency exceeds its contract bound")));
		Bytes.Append(Chunk.GetData(), Chunk.Num());
		Written += static_cast<uint64>(Chunk.Num());
		return FWoTBootstrapStatus::Success();
	}
	TArray<uint8> Bytes;
private:
	uint64 Limit = 0;
	uint64 Written = 0;
};
}

TWoTBootstrapResult<FWoTSha256> ComputeFrontEndDescriptorRoot(
	const FWoTFrontEndDescriptor& Descriptor)
{
	using FResult = TWoTBootstrapResult<FWoTSha256>;
	TWoTBootstrapResult<FWoTCanonicalJsonValue> Json =
		WoTFrontEndDescriptorJson(Descriptor, false);
	if (Json.IsError()) return FResult::Error(MoveTemp(Json).GetError());
	TWoTBootstrapResult<TArray<uint8>> Bytes = WriteCanonicalJson(Json.GetValue());
	if (Bytes.IsError()) return FResult::Error(MoveTemp(Bytes).GetError());
	FWoTSha256Hasher Hasher;
	static const ANSICHAR Domain[] = "WOT-FRONT-END-DESCRIPTOR-V2";
	Hasher.Update(TConstArrayView<uint8>(reinterpret_cast<const uint8*>(Domain),
		UE_ARRAY_COUNT(Domain)));
	Hasher.Update(Bytes.GetValue());
	return FResult::Success(Hasher.Finish());
}

FWoTBootstrapStatus ValidateFrontEndDescriptor(
	const FWoTFrontEndDescriptor& Descriptor)
{
	if (Descriptor.MapName != WoTFrontEndMapName
		|| !WoTFrontEndSafeLogicalId(Descriptor.BspLogicalId)
		|| !WoTFrontEndSafeLogicalId(Descriptor.WorldLogicalId)
		|| !Descriptor.BspLogicalId.StartsWith(TEXT("cache/bsp/"))
		|| !Descriptor.WorldLogicalId.StartsWith(TEXT("cache/world/"))
		|| Descriptor.Census.Lights != 39
		|| Descriptor.Census.AmbientSounds != 5
		|| Descriptor.Census.SplashEmitters != 4
		|| Descriptor.Census.RainCylinders != 6
		|| Descriptor.Census.LightningActors != 5
		|| Descriptor.Census.RandomLightningControllers != 1
		|| Descriptor.ThunderSoundSourceExports.Num() != 10
		|| !FMath::IsFinite(Descriptor.CameraLocation.X)
		|| !FMath::IsFinite(Descriptor.CameraLocation.Y)
		|| !FMath::IsFinite(Descriptor.CameraLocation.Z)
		|| FMath::Abs(Descriptor.CameraLocation.X) > WoTFrontEndMaxCoordinate
		|| FMath::Abs(Descriptor.CameraLocation.Y) > WoTFrontEndMaxCoordinate
		|| FMath::Abs(Descriptor.CameraLocation.Z) > WoTFrontEndMaxCoordinate
		|| !FMath::IsFinite(Descriptor.CameraRotation.Pitch)
		|| !FMath::IsFinite(Descriptor.CameraRotation.Yaw)
		|| !FMath::IsFinite(Descriptor.CameraRotation.Roll)
		|| Descriptor.HorizontalFov != 90.0f
		|| Descriptor.RequiredResources.Num() != 48
		|| Descriptor.RequiredLogicalIds.Num() != 48)
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("the front-end descriptor violates its pinned entry contract")));
	TSet<FString> ExpectedResources;
	for (const FWoTFrontEndExpectedResource& Resource
		: WoTFrontEndExpectedResources)
		ExpectedResources.Add(WoTFrontEndResourceKey(
			Resource.Domain, Resource.SourceExport));
	ExpectedResources.Add(WoTFrontEndResourceKey(
		EWoTFactDomain::BSP, TEXT("WheelOfTime.Model")));
	ExpectedResources.Add(WoTFrontEndResourceKey(
		EWoTFactDomain::World, TEXT("WheelOfTime.Level")));
	TSet<FString> ObservedResources;
	TSet<FString> ThunderResources;
	TArray<FString> ProjectedLogicalIds;
	FString PriorResource;
	for (const FWoTFrontEndResourceRequirement& Requirement
		: Descriptor.RequiredResources)
	{
		const FString Key = WoTFrontEndResourceKey(
			Requirement.Domain, Requirement.SourceExport);
		const FString SortKey = FString(WoTFrontEndDomainName(Requirement.Domain))
			+ TEXT("\n") + WoTFrontEndFold(Requirement.SourceExport)
			+ TEXT("\n") + Requirement.LogicalId;
		const FString ExpectedPath = Requirement.Domain == EWoTFactDomain::BSP
			|| Requirement.Domain == EWoTFactDomain::World
			? FString(WoTFrontEndSourcePath)
			: WoTFrontEndExpectedSourcePath(Requirement.SourceExport);
		const FString DomainPrefix = FString::Printf(TEXT("cache/%s/"),
			WoTFrontEndDomainName(Requirement.Domain));
		if (!ExpectedResources.Contains(Key) || ObservedResources.Contains(Key)
			|| Requirement.SourcePath != ExpectedPath
			|| !WoTFrontEndSafeLogicalId(Requirement.LogicalId)
			|| !Requirement.LogicalId.StartsWith(DomainPrefix)
			|| (!PriorResource.IsEmpty() && PriorResource >= SortKey))
			return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::DuplicateField,
				TEXT("the front-end typed resource graph is not exact, sorted, and unique"),
				Requirement.SourceExport));
		ObservedResources.Add(Key);
		if (Requirement.Domain == EWoTFactDomain::Sound
			&& Requirement.SourceExport.StartsWith(
				TEXT("Environment.Thunder."), ESearchCase::CaseSensitive))
			ThunderResources.Add(WoTFrontEndFold(Requirement.SourceExport));
		ProjectedLogicalIds.Add(Requirement.LogicalId);
		PriorResource = SortKey;
	}
	if (ObservedResources.Num() != ExpectedResources.Num())
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::UnknownField,
			TEXT("the front-end typed resource graph is incomplete")));
	TSet<FString> RoutedThunder;
	for (const FString& SourceExport : Descriptor.ThunderSoundSourceExports)
	{
		const FString Folded = WoTFrontEndFold(SourceExport);
		if (!ThunderResources.Contains(Folded))
			return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::UnknownField,
				TEXT("the ordered thunder route is outside its typed resources"),
				SourceExport));
		RoutedThunder.Add(Folded);
	}
	bool bMissingThunderResource = RoutedThunder.Num() != ThunderResources.Num();
	for (const FString& Resource : ThunderResources)
		bMissingThunderResource |= !RoutedThunder.Contains(Resource);
	if (bMissingThunderResource)
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::UnknownField,
			TEXT("the ordered thunder route omits a typed thunder resource")));
	ProjectedLogicalIds.Sort();
	if (ProjectedLogicalIds != Descriptor.RequiredLogicalIds)
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::UnknownField,
			TEXT("the front-end logical closure differs from its typed graph")));
	FString Prior;
	bool bHasBSP = false;
	bool bHasWorld = false;
	for (const FString& LogicalId : Descriptor.RequiredLogicalIds)
	{
		if (!WoTFrontEndSafeLogicalId(LogicalId)
			|| (!Prior.IsEmpty() && Prior >= LogicalId))
			return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::DuplicateField,
				TEXT("the front-end resource closure is not sorted and unique"),
				LogicalId));
		Prior = LogicalId;
		bHasBSP |= LogicalId == Descriptor.BspLogicalId;
		bHasWorld |= LogicalId == Descriptor.WorldLogicalId;
	}
	const FWoTFrontEndResourceRequirement* BSP = Descriptor.RequiredResources.FindByPredicate(
		[](const FWoTFrontEndResourceRequirement& Requirement)
		{
			return Requirement.Domain == EWoTFactDomain::BSP;
		});
	const FWoTFrontEndResourceRequirement* World = Descriptor.RequiredResources.FindByPredicate(
		[](const FWoTFrontEndResourceRequirement& Requirement)
		{
			return Requirement.Domain == EWoTFactDomain::World;
		});
	if (!bHasBSP || !bHasWorld || BSP == nullptr || World == nullptr
		|| BSP->LogicalId != Descriptor.BspLogicalId
		|| World->LogicalId != Descriptor.WorldLogicalId)
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::UnknownField,
			TEXT("the front-end resource closure omits its BSP or world")));
	TWoTBootstrapResult<FWoTSha256> Root = ComputeFrontEndDescriptorRoot(Descriptor);
	if (Root.IsError())
		return FWoTBootstrapStatus::Error(MoveTemp(Root).GetError());
	if (Root.GetValue() != Descriptor.DescriptorRoot)
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("the front-end descriptor root does not match its fields")));
	return FWoTBootstrapStatus::Success();
}

TWoTBootstrapResult<FWoTFrontEndDescriptor> BuildFrontEndDescriptor(
	const FWoTRetailPackage& Package, const FWoTRetailContract& Contract,
	IWoTProgressSink& Progress)
{
	using FResult = TWoTBootstrapResult<FWoTFrontEndDescriptor>;
	FWoTBootstrapStatus Initial = Progress.ReportProgress(TEXT("front-end-entry"), 0, 2);
	if (Initial.IsError()) return FResult::Error(MoveTemp(Initial).GetError());
	if (Progress.IsCancellationRequested())
		return WoTFrontEndError<FWoTFrontEndDescriptor>(
			EWoTBootstrapError::ServiceUnavailable,
			TEXT("front-end descriptor generation was cancelled"));
	auto LoadPackage = [&Package](const TCHAR* SourcePath, const TCHAR* PackageName)
		-> TWoTBootstrapResult<FWoTFrontEndPackageView>
	{
		using FLoadResult = TWoTBootstrapResult<FWoTFrontEndPackageView>;
		TWoTBootstrapResult<FWoTPackageEntry> Entry = Package.FindExact(SourcePath);
		if (Entry.IsError()) return FLoadResult::Error(MoveTemp(Entry).GetError());
		FWoTFrontEndCollectSink Sink(WoTFrontEndMaxSourceBytes);
		FWoTBootstrapStatus Streamed = Package.StreamExact(Entry.GetValue(), Sink);
		if (Streamed.IsError())
			return FLoadResult::Error(MoveTemp(Streamed).GetError());
		return WoTFrontEndParsePackage(
			MoveTemp(Sink.Bytes), SourcePath, PackageName);
	};
	TWoTBootstrapResult<FWoTFrontEndPackageView> Parsed =
		LoadPackage(WoTFrontEndSourcePath, TEXT("WheelOfTime"));
	if (Parsed.IsError()) return FResult::Error(MoveTemp(Parsed).GetError());
	TWoTBootstrapResult<FWoTFrontEndPackageView> Environment =
		LoadPackage(TEXT("System/Environment.u"), TEXT("Environment"));
	if (Environment.IsError())
		return FResult::Error(MoveTemp(Environment).GetError());
	TWoTBootstrapResult<FWoTFrontEndPackageView> WOT =
		LoadPackage(TEXT("System/WOT.u"), TEXT("WOT"));
	if (WOT.IsError()) return FResult::Error(MoveTemp(WOT).GetError());
	TWoTBootstrapResult<FWoTFrontEndSemantics> Inspected =
		WoTFrontEndInspectPackage(MoveTemp(Parsed).GetValue());
	if (Inspected.IsError()) return FResult::Error(MoveTemp(Inspected).GetError());
	Inspected = WoTFrontEndCompleteResourceGraph(
		MoveTemp(Inspected).GetValue(), Environment.GetValue(), WOT.GetValue());
	if (Inspected.IsError()) return FResult::Error(MoveTemp(Inspected).GetError());
	TWoTBootstrapResult<FWoTFrontEndContractClosure> Closure =
		WoTFrontEndCloseContract(Inspected.GetValue(), Contract);
	if (Closure.IsError()) return FResult::Error(MoveTemp(Closure).GetError());
	TSet<FString> SourcePaths;
	SourcePaths.Add(WoTFrontEndSourcePath);
	for (const FWoTRequiredFact* Fact : Closure.GetValue().Resources)
		SourcePaths.Add(Fact->SourcePath);
	TArray<FString> OrderedSources = SourcePaths.Array();
	OrderedSources.Sort();
	FWoTSha256Hasher SourceHasher;
	static const ANSICHAR Domain[] = "WOT-FRONT-END-SOURCE-V1";
	SourceHasher.Update(TConstArrayView<uint8>(reinterpret_cast<const uint8*>(Domain),
		UE_ARRAY_COUNT(Domain)));
	for (const FString& SourcePath : OrderedSources)
	{
		TWoTBootstrapResult<FWoTPackageEntry> Entry = Package.FindExact(SourcePath);
		if (Entry.IsError()) return FResult::Error(MoveTemp(Entry).GetError());
		FWoTFrontEndCollectSink Sink(WoTFrontEndMaxSourceBytes);
		FWoTBootstrapStatus Loaded = Package.StreamExact(Entry.GetValue(), Sink);
		if (Loaded.IsError()) return FResult::Error(MoveTemp(Loaded).GetError());
		FTCHARToUTF8 PathUtf8(*SourcePath);
		const uint64 PathLength = static_cast<uint64>(PathUtf8.Length());
		uint8 LengthBytes[8];
		for (uint32 Index = 0; Index < UE_ARRAY_COUNT(LengthBytes); ++Index)
			LengthBytes[Index] = static_cast<uint8>(PathLength >> (Index * 8));
		SourceHasher.Update(LengthBytes);
		SourceHasher.Update(TConstArrayView<uint8>(
			reinterpret_cast<const uint8*>(PathUtf8.Get()), PathUtf8.Length()));
		const FWoTSha256 Digest = Sha256(Sink.Bytes);
		SourceHasher.Update(Digest.Bytes);
	}
	Inspected.GetValue().SourceRoot = SourceHasher.Finish();
	FWoTBootstrapStatus Quarantined =
		Progress.ReportProgress(TEXT("front-end-entry"), 1, 2);
	if (Quarantined.IsError()) return FResult::Error(MoveTemp(Quarantined).GetError());
	if (Progress.IsCancellationRequested())
		return WoTFrontEndError<FWoTFrontEndDescriptor>(
			EWoTBootstrapError::ServiceUnavailable,
			TEXT("front-end descriptor generation was cancelled"));
	FResult Result = WoTFrontEndBuild(MoveTemp(Inspected).GetValue(), Contract);
	if (Result.IsError()) return Result;
	FWoTBootstrapStatus Complete =
		Progress.ReportProgress(TEXT("front-end-entry"), 2, 2);
	if (Complete.IsError()) return FResult::Error(MoveTemp(Complete).GetError());
	return Result;
}

#if WITH_AUTOMATION_TESTS
TWoTBootstrapResult<TArray<uint8>> MakeHostileFrontEndPackageBytesForTesting(
	TConstArrayView<uint8> PackageBytes,
	EWoTFrontEndPackageAttackForTesting Attack)
{
	using FResult = TWoTBootstrapResult<TArray<uint8>>;
	TArray<uint8> Owned;
	Owned.Append(PackageBytes.GetData(), PackageBytes.Num());
	TWoTBootstrapResult<FWoTFrontEndPackageView> Parsed =
		WoTFrontEndParsePackage(MoveTemp(Owned), WoTFrontEndSourcePath,
			TEXT("WheelOfTime"));
	if (Parsed.IsError()) return FResult::Error(MoveTemp(Parsed).GetError());
	FWoTFrontEndPackageView& Package = Parsed.GetValue();
	if (Attack == EWoTFrontEndPackageAttackForTesting::ThunderSourceRootMismatch
		|| Attack == EWoTFrontEndPackageAttackForTesting::ThunderSourceCaseMismatch)
	{
		const FTCHARToUTF8 From(TEXT("Environment.Thunder10"));
		const FTCHARToUTF8 To(
			Attack == EWoTFrontEndPackageAttackForTesting::ThunderSourceRootMismatch
				? TEXT("BogusRoot__.Thunder10") : TEXT("Environment.thunder10"));
		static_assert(sizeof(ANSICHAR) == sizeof(uint8));
		if (From.Length() != To.Length())
			return WoTFrontEndError<TArray<uint8>>(
				EWoTBootstrapError::UnknownField,
				TEXT("hostile thunder roots do not have equal encoded width"));
		int32 Matches = 0;
		for (int32 Offset = 0; Offset + From.Length() <= Package.Bytes.Num(); ++Offset)
		{
			if (FMemory::Memcmp(Package.Bytes.GetData() + Offset,
				From.Get(), From.Length()) != 0) continue;
			FMemory::Memcpy(Package.Bytes.GetData() + Offset, To.Get(), To.Length());
			++Matches;
		}
		if (Matches == 1) return FResult::Success(MoveTemp(Package.Bytes));
		return WoTFrontEndError<TArray<uint8>>(
			EWoTBootstrapError::UnknownField,
			TEXT("could not isolate one thunder source root for mutation"));
	}
	const FWoTFrontEndExport* Level = Package.Exports.FindByPredicate(
		[&Package](const FWoTFrontEndExport& Export)
		{
			const FString* Class = Package.ClassLeaf(Export.ClassReference);
			return Class != nullptr && *Class == TEXT("Level");
		});
	if (Level == nullptr)
		return WoTFrontEndError<TArray<uint8>>(EWoTBootstrapError::UnknownField,
			TEXT("the hostile fixture source has no Level"));
	FWoTFrontEndReader Reader;
	TArray<FWoTTypedProperty> Properties;
	uint64 PropertyCount = 0;
	int32 ActorCount = 0;
	int32 ActorMaximum = 0;
	if (!WoTFrontEndOpenExport(Package, *Level, Reader)
		|| !WoTFrontEndReadProperties(
			Reader, Package, Properties, PropertyCount)
		|| !Reader.ReadI32(ActorCount) || !Reader.ReadI32(ActorMaximum)
		|| ActorCount <= 1 || ActorMaximum < ActorCount)
		return WoTFrontEndError<TArray<uint8>>(EWoTBootstrapError::CorruptInstallation,
			TEXT("the hostile fixture Level actor registry is malformed"));
	struct FReferenceSpan
	{
		int32 Reference = 0;
		uint64 Begin = 0;
		uint64 End = 0;
	};
	TArray<FReferenceSpan> ActorSpans;
	TSet<int32> ActorReferences;
	for (int32 Index = 0; Index < ActorCount; ++Index)
	{
		FReferenceSpan& Span = ActorSpans.AddDefaulted_GetRef();
		Span.Begin = Reader.Tell();
		if (!Reader.ReadCompact(Span.Reference))
			return WoTFrontEndError<TArray<uint8>>(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("the hostile fixture has an invalid actor reference"));
		Span.End = Reader.Tell();
		if (Span.Reference > 0) ActorReferences.Add(Span.Reference);
	}
	auto CopySpan = [&Package](uint64 TargetBegin, uint64 TargetEnd,
		uint64 SourceBegin, uint64 SourceEnd)
	{
		if (TargetEnd - TargetBegin != SourceEnd - SourceBegin) return false;
		FMemory::Memcpy(Package.Bytes.GetData() + TargetBegin,
			Package.Bytes.GetData() + SourceBegin,
			static_cast<SIZE_T>(TargetEnd - TargetBegin));
		return true;
	};
	if (Attack == EWoTFrontEndPackageAttackForTesting::DuplicateLevelActorReference)
	{
		for (int32 Left = 0; Left < ActorSpans.Num(); ++Left)
			for (int32 Right = Left + 1; Right < ActorSpans.Num(); ++Right)
				if (ActorSpans[Left].Reference > 0
					&& ActorSpans[Right].Reference > 0
					&& ActorSpans[Left].Reference != ActorSpans[Right].Reference
					&& CopySpan(ActorSpans[Right].Begin, ActorSpans[Right].End,
						ActorSpans[Left].Begin, ActorSpans[Left].End))
					return FResult::Success(MoveTemp(Package.Bytes));
	}
	if (!WoTFrontEndSkipURL(Reader))
		return WoTFrontEndError<TArray<uint8>>(EWoTBootstrapError::CorruptInstallation,
			TEXT("the hostile fixture Level URL is malformed"));
	const uint64 ModelBegin = Reader.Tell();
	int32 ModelReference = 0;
	if (!Reader.ReadCompact(ModelReference))
		return WoTFrontEndError<TArray<uint8>>(EWoTBootstrapError::CorruptInstallation,
			TEXT("the hostile fixture Level model reference is malformed"));
	const uint64 ModelEnd = Reader.Tell();
	if (Attack == EWoTFrontEndPackageAttackForTesting::LevelModelTargetsNonModel)
	{
		for (const FReferenceSpan& Span : ActorSpans)
		{
			if (Span.Reference <= 0 || !Package.Exports.IsValidIndex(Span.Reference - 1))
				continue;
			const FString* Class = Package.ClassLeaf(
				Package.Exports[Span.Reference - 1].ClassReference);
			if (Class != nullptr && *Class != TEXT("Model")
				&& CopySpan(ModelBegin, ModelEnd, Span.Begin, Span.End))
				return FResult::Success(MoveTemp(Package.Bytes));
		}
	}
	auto EncodeCompact = [](int32 Value)
	{
		TArray<uint8> Encoded;
		const bool bNegative = Value < 0;
		uint32 Magnitude = bNegative ? static_cast<uint32>(-static_cast<int64>(Value))
			: static_cast<uint32>(Value);
		uint8 First = static_cast<uint8>(Magnitude & 0x3f);
		Magnitude >>= 6;
		if (bNegative) First |= 0x80;
		if (Magnitude != 0) First |= 0x40;
		Encoded.Add(First);
		while (Magnitude != 0)
		{
			uint8 Next = static_cast<uint8>(Magnitude & 0x7f);
			Magnitude >>= 7;
			if (Magnitude != 0) Next |= 0x80;
			Encoded.Add(Next);
		}
		return Encoded;
	};
	if (Attack == EWoTFrontEndPackageAttackForTesting::UnclassifiedInternalReference)
	{
		for (int32 TargetIndex = 0; TargetIndex < Package.Exports.Num(); ++TargetIndex)
		{
			const int32 TargetReference = TargetIndex + 1;
			if (ActorReferences.Contains(TargetReference)) continue;
			const FString* TargetClass = Package.ClassLeaf(
				Package.Exports[TargetIndex].ClassReference);
			if (TargetClass == nullptr) continue;
			const FString Folded = WoTFrontEndFold(*TargetClass);
			if (Folded == TEXT("class") || Folded == TEXT("levelinfo")
				|| Folded == TEXT("level") || Folded == TEXT("levelsummary")
				|| Folded == TEXT("model")
				|| Folded == TEXT("texture") || Folded == TEXT("sound")
				|| Folded == TEXT("mesh") || Folded == TEXT("lodmesh")
				|| Folded == TEXT("font") || Folded.EndsWith(TEXT("texture")))
				continue;
			const TArray<uint8> Encoded = EncodeCompact(TargetReference);
			for (const FReferenceSpan& Actor : ActorSpans)
			{
				if (Actor.Reference <= 0
					|| !Package.Exports.IsValidIndex(Actor.Reference - 1)) continue;
				FWoTFrontEndReader ActorReader;
				TArray<FWoTTypedProperty> ActorProperties;
				if (!WoTFrontEndOpenExport(Package,
					Package.Exports[Actor.Reference - 1], ActorReader)
					|| !WoTFrontEndReadProperties(ActorReader, Package,
						ActorProperties, PropertyCount)) continue;
				for (const FWoTTypedProperty& Property : ActorProperties)
				{
					if (!Property.ObjectReference.IsSet()
						|| Property.Value.Num() != Encoded.Num()) continue;
					const TArray<uint8> Original = EncodeCompact(
						Property.ObjectReference.GetValue());
					if (Original.Num() != Encoded.Num()) continue;
					const FWoTFrontEndExport& Export =
						Package.Exports[Actor.Reference - 1];
					for (uint64 Offset = Export.Offset;
						Offset + Original.Num() <= Export.Offset + Export.Size; ++Offset)
					{
						if (FMemory::Memcmp(Package.Bytes.GetData() + Offset,
							Original.GetData(), Original.Num()) != 0) continue;
						FMemory::Memcpy(Package.Bytes.GetData() + Offset,
							Encoded.GetData(), Encoded.Num());
						return FResult::Success(MoveTemp(Package.Bytes));
					}
				}
			}
		}
	}
	return WoTFrontEndError<TArray<uint8>>(EWoTBootstrapError::UnknownField,
		TEXT("could not construct the requested hostile package-byte fixture"));
}

TWoTBootstrapResult<FWoTFrontEndDescriptor>
BuildFrontEndDescriptorPackageBytesForTesting(
	TConstArrayView<FWoTFrontEndSourceBytesForTesting> Sources,
	const FWoTRetailContract& Contract)
{
	using FResult = TWoTBootstrapResult<FWoTFrontEndDescriptor>;
	if (Sources.Num() != 3)
		return WoTFrontEndError<FWoTFrontEndDescriptor>(
			EWoTBootstrapError::UnknownField,
			TEXT("the package-byte seam requires exactly three named sources"));
	auto ParseSource = [&Sources](const TCHAR* SourcePath, const TCHAR* PackageName)
		-> TWoTBootstrapResult<FWoTFrontEndPackageView>
	{
		using FParseResult = TWoTBootstrapResult<FWoTFrontEndPackageView>;
		const FWoTFrontEndSourceBytesForTesting* Match = nullptr;
		for (const FWoTFrontEndSourceBytesForTesting& Source : Sources)
		{
			if (Source.SourcePath != SourcePath) continue;
			if (Match != nullptr)
				return FParseResult::Error(MakeWoTBootstrapError(
					EWoTBootstrapError::DuplicateField,
					TEXT("the package-byte seam contains a duplicate source"), SourcePath));
			Match = &Source;
		}
		if (Match == nullptr)
			return FParseResult::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::UnknownField,
				TEXT("the package-byte seam lacks a required source"), SourcePath));
		TArray<uint8> Owned = Match->Bytes;
		return WoTFrontEndParsePackage(
			MoveTemp(Owned), SourcePath, PackageName);
	};
	TWoTBootstrapResult<FWoTFrontEndPackageView> Parsed =
		ParseSource(WoTFrontEndSourcePath, TEXT("WheelOfTime"));
	if (Parsed.IsError()) return FResult::Error(MoveTemp(Parsed).GetError());
	TWoTBootstrapResult<FWoTFrontEndPackageView> Environment =
		ParseSource(TEXT("System/Environment.u"), TEXT("Environment"));
	if (Environment.IsError())
		return FResult::Error(MoveTemp(Environment).GetError());
	TWoTBootstrapResult<FWoTFrontEndPackageView> WOT =
		ParseSource(TEXT("System/WOT.u"), TEXT("WOT"));
	if (WOT.IsError()) return FResult::Error(MoveTemp(WOT).GetError());
	TWoTBootstrapResult<FWoTFrontEndSemantics> Inspected =
		WoTFrontEndInspectPackage(MoveTemp(Parsed).GetValue());
	if (Inspected.IsError()) return FResult::Error(MoveTemp(Inspected).GetError());
	Inspected = WoTFrontEndCompleteResourceGraph(
		MoveTemp(Inspected).GetValue(), Environment.GetValue(), WOT.GetValue());
	if (Inspected.IsError()) return FResult::Error(MoveTemp(Inspected).GetError());
	FWoTSha256Hasher SourceHasher;
	static const ANSICHAR Domain[] = "WOT-FRONT-END-SOURCE-V1";
	SourceHasher.Update(TConstArrayView<uint8>(reinterpret_cast<const uint8*>(Domain),
		UE_ARRAY_COUNT(Domain)));
	TArray<const FWoTFrontEndSourceBytesForTesting*> OrderedSources;
	for (const FWoTFrontEndSourceBytesForTesting& Source : Sources)
		OrderedSources.Add(&Source);
	OrderedSources.Sort([](const FWoTFrontEndSourceBytesForTesting& Left,
		const FWoTFrontEndSourceBytesForTesting& Right)
	{
		return Left.SourcePath < Right.SourcePath;
	});
	for (const FWoTFrontEndSourceBytesForTesting* Source : OrderedSources)
	{
		FTCHARToUTF8 PathUtf8(*Source->SourcePath);
		const uint64 PathLength = PathUtf8.Length();
		uint8 LengthBytes[8];
		for (uint32 Index = 0; Index < UE_ARRAY_COUNT(LengthBytes); ++Index)
			LengthBytes[Index] = static_cast<uint8>(PathLength >> (Index * 8));
		SourceHasher.Update(LengthBytes);
		SourceHasher.Update(TConstArrayView<uint8>(
			reinterpret_cast<const uint8*>(PathUtf8.Get()), PathUtf8.Length()));
		const FWoTSha256 PackageSha256 = Sha256(Source->Bytes);
		SourceHasher.Update(PackageSha256.Bytes);
	}
	Inspected.GetValue().SourceRoot = SourceHasher.Finish();
	return WoTFrontEndBuild(MoveTemp(Inspected).GetValue(), Contract);
}

TWoTBootstrapResult<FWoTFrontEndDescriptor>
BuildFrontEndDescriptorSemanticFixtureForTesting(
	TConstArrayView<uint8> CanonicalBytes, const FWoTRetailContract& Contract)
{
	using FResult = TWoTBootstrapResult<FWoTFrontEndDescriptor>;
	FWoTCanonicalJsonLimits Limits;
	Limits.MaxBytes = 4 * 1024 * 1024;
	Limits.MaxMembers = 1000000;
	TWoTBootstrapResult<FWoTCanonicalJsonValue> Parsed =
		ParseCanonicalJson(CanonicalBytes, Limits);
	if (Parsed.IsError()) return FResult::Error(MoveTemp(Parsed).GetError());
	const FWoTCanonicalJsonValue& Root = Parsed.GetValue();
	static const TCHAR* const Fields[] = {
		TEXT("actors"), TEXT("camera"), TEXT("format"), TEXT("map_name"),
		TEXT("references"), TEXT("thunder_sound_source_exports")};
	if (TOptional<FWoTBootstrapError> Error = RequireExactMembers(
		Root, Fields, TEXT("front-end semantic fixture")); Error.IsSet())
		return FResult::Error(MoveTemp(Error.GetValue()));
	FString Format;
	FString MapName;
	if (ReadStringField(Root, TEXT("format"), TEXT("front-end semantic fixture"),
			Format).IsSet()
		|| ReadStringField(Root, TEXT("map_name"),
			TEXT("front-end semantic fixture"), MapName).IsSet()
		|| Format != TEXT("wot-front-end-semantic-fixture-v1"))
		return WoTFrontEndError<FWoTFrontEndDescriptor>(EWoTBootstrapError::UnknownField,
			TEXT("the front-end semantic fixture has the wrong protocol"));
	const FWoTCanonicalJsonValue* Actors = Root.FindMember(TEXT("actors"));
	const FWoTCanonicalJsonValue* Camera = Root.FindMember(TEXT("camera"));
	const FWoTCanonicalJsonValue* References = Root.FindMember(TEXT("references"));
	const FWoTCanonicalJsonValue* Thunder =
		Root.FindMember(TEXT("thunder_sound_source_exports"));
	if (Actors == nullptr || !Actors->IsArray() || Camera == nullptr
		|| !Camera->IsObject() || References == nullptr || !References->IsArray()
		|| Thunder == nullptr || !Thunder->IsArray() || Thunder->Num() != 10)
		return WoTFrontEndError<FWoTFrontEndDescriptor>(EWoTBootstrapError::UnknownField,
			TEXT("the front-end semantic fixture has malformed collections"));
	FWoTFrontEndSemantics Semantics;
	Semantics.MapName = MoveTemp(MapName);
	for (int32 Index = 0; Index < Thunder->Num(); ++Index)
	{
		const FWoTCanonicalJsonValue& Identity = Thunder->GetElement(Index);
		if (Identity.GetKind() != EWoTCanonicalJsonKind::String
			|| Identity.GetString().IsEmpty())
			return WoTFrontEndError<FWoTFrontEndDescriptor>(
				EWoTBootstrapError::UnknownField,
				TEXT("the semantic thunder route is malformed"));
		Semantics.ThunderSoundSourceExports.Add(Identity.GetString());
	}
	for (int32 Index = 0; Index < Actors->Num(); ++Index)
	{
		const FWoTCanonicalJsonValue& Actor = Actors->GetElement(Index);
		static const TCHAR* const ActorFields[] = {TEXT("class"), TEXT("identity")};
		FString Class;
		FString Identity;
		if (RequireExactMembers(Actor, ActorFields, TEXT("front-end actor")).IsSet()
			|| ReadStringField(Actor, TEXT("class"), TEXT("front-end actor"), Class).IsSet()
			|| ReadStringField(Actor, TEXT("identity"), TEXT("front-end actor"), Identity).IsSet())
			return WoTFrontEndError<FWoTFrontEndDescriptor>(
				EWoTBootstrapError::UnknownField,
				TEXT("a front-end actor fixture is malformed"));
		Semantics.Actors.Add({MoveTemp(Class), MoveTemp(Identity)});
	}
	for (int32 Index = 0; Index < References->Num(); ++Index)
	{
		const FWoTCanonicalJsonValue& Reference = References->GetElement(Index);
		static const TCHAR* const ReferenceFields[] = {
			TEXT("domain"), TEXT("identity"), TEXT("kind")};
		FString DomainName;
		FString Identity;
		FString Kind;
		if (RequireExactMembers(Reference, ReferenceFields,
				TEXT("front-end reference")).IsSet()
			|| ReadStringField(Reference, TEXT("domain"),
				TEXT("front-end reference"), DomainName).IsSet()
			|| ReadStringField(Reference, TEXT("identity"),
				TEXT("front-end reference"), Identity).IsSet()
			|| ReadStringField(Reference, TEXT("kind"),
				TEXT("front-end reference"), Kind).IsSet()
			|| Kind != TEXT("resource"))
			return WoTFrontEndError<FWoTFrontEndDescriptor>(
				EWoTBootstrapError::UnknownField,
				TEXT("a front-end reference is unclassified"));
		const TOptional<EWoTFactDomain> Domain =
			WoTFrontEndParseDomain(DomainName);
		if (!Domain.IsSet())
			return WoTFrontEndError<FWoTFrontEndDescriptor>(
				EWoTBootstrapError::UnknownField,
				TEXT("a front-end reference has an unknown domain"), DomainName);
		Semantics.ResourceReferences.Add({Domain.GetValue(), MoveTemp(Identity)});
	}
	static const TCHAR* const CameraFields[] = {
		TEXT("horizontal_fov"), TEXT("location"), TEXT("rotation")};
	uint32 Fov = 0;
	if (RequireExactMembers(*Camera, CameraFields, TEXT("front-end camera")).IsSet()
		|| ReadUnsigned32Field(*Camera, TEXT("horizontal_fov"),
			TEXT("front-end camera"), Fov).IsSet())
		return WoTFrontEndError<FWoTFrontEndDescriptor>(EWoTBootstrapError::UnknownField,
			TEXT("the front-end camera fixture is malformed"));
	const FWoTCanonicalJsonValue* Location = Camera->FindMember(TEXT("location"));
	const FWoTCanonicalJsonValue* Rotation = Camera->FindMember(TEXT("rotation"));
	if (Location == nullptr || !Location->IsArray() || Location->Num() != 3
		|| Rotation == nullptr || !Rotation->IsArray() || Rotation->Num() != 3)
		return WoTFrontEndError<FWoTFrontEndDescriptor>(EWoTBootstrapError::UnknownField,
			TEXT("the front-end camera fixture vectors are malformed"));
	double LocationValues[3];
	double RotationValues[3];
	for (int32 Index = 0; Index < 3; ++Index)
	{
		if (Location->GetElement(Index).GetKind() != EWoTCanonicalJsonKind::String
			|| Rotation->GetElement(Index).GetKind() != EWoTCanonicalJsonKind::String
			|| !LexTryParseString(LocationValues[Index],
				*Location->GetElement(Index).GetString())
			|| !LexTryParseString(RotationValues[Index],
				*Rotation->GetElement(Index).GetString()))
			return WoTFrontEndError<FWoTFrontEndDescriptor>(
				EWoTBootstrapError::InvalidNumber,
				TEXT("the front-end camera fixture contains an invalid number"));
	}
	Semantics.CameraLocation = FVector(
		LocationValues[0], LocationValues[1], LocationValues[2]);
	Semantics.CameraRotation = FRotator(
		RotationValues[0], RotationValues[1], RotationValues[2]);
	Semantics.HorizontalFov = static_cast<float>(Fov);
	FWoTSha256Hasher SourceHasher;
	static const ANSICHAR Domain[] = "WOT-FRONT-END-SYNTHETIC-SOURCE-V1";
	SourceHasher.Update(TConstArrayView<uint8>(reinterpret_cast<const uint8*>(Domain),
		UE_ARRAY_COUNT(Domain)));
	SourceHasher.Update(CanonicalBytes);
	Semantics.SourceRoot = SourceHasher.Finish();
	return WoTFrontEndBuild(MoveTemp(Semantics), Contract);
}
#endif
