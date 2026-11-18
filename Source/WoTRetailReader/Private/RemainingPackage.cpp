#include "RemainingPackage.h"

#include "Misc/Paths.h"

const uint64 WoTRemainingMaxSourceBytes = 256ull * 1024 * 1024;
const uint32 WoTRemainingMaxItems = 1000000;

namespace
{
constexpr uint32 WoTRemainingMaxNames = 65536;
constexpr uint32 WoTRemainingMaxNameBytes = 1024;
constexpr uint32 WoTRemainingMaxOuterDepth = 32;
constexpr uint32 WoTRemainingPackageTag = 0x9e2a83c1;
constexpr uint32 WoTRemainingHasStack = 0x02000000;

bool WoTRemainingLatin1(TConstArrayView<uint8> Raw, FString& Out)
{
	Out.Reset(Raw.Num());
	for (uint8 Byte : Raw)
	{
		if (Byte == 0) return false;
		Out.AppendChar(static_cast<TCHAR>(Byte));
	}
	return true;
}

FWoTBootstrapStatus WoTRemainingBuildCanonicalIdentityTable(
	FWoTRemainingSource& Source)
{
	TMap<int32, FString> Raw;
	TMap<FString, uint32> Counts;
	for (int32 ImportIndex = 0; ImportIndex < Source.Imports.Num(); ++ImportIndex)
	{
		const int32 Reference = -ImportIndex - 1;
		TWoTBootstrapResult<FString> Identity =
			WoTRemainingObjectPath(Source, Reference);
		if (Identity.IsError())
			return FWoTBootstrapStatus::Error(MoveTemp(Identity).GetError());
		Raw.Add(Reference, MoveTemp(Identity).GetValue());
	}
	for (int32 ExportIndex = 0; ExportIndex < Source.Exports.Num(); ++ExportIndex)
	{
		const int32 Reference = ExportIndex + 1;
		const FWoTRemainingExport& Export = Source.Exports[ExportIndex];
		FString Identity;
		if (Export.ClassReference == 0 && Export.Size == 0 && Export.Name == TEXT("None"))
			Identity = Source.PackageName
				+ FString::Printf(TEXT(".#export-%08d"), Reference);
		else
		{
			TWoTBootstrapResult<FString> Path =
				WoTRemainingObjectPath(Source, Reference);
			if (Path.IsError())
				return FWoTBootstrapStatus::Error(MoveTemp(Path).GetError());
			Identity = MoveTemp(Path).GetValue();
		}
		Raw.Add(Reference, MoveTemp(Identity));
	}
	for (const TPair<int32, FString>& Item : Raw)
		++Counts.FindOrAdd(WoTRemainingFold(Item.Value));
	TSet<FString> Seen;
	for (const TPair<int32, FString>& Item : Raw)
	{
		FString Identity = Item.Value;
		if (Counts[WoTRemainingFold(Identity)] > 1)
		{
			const TCHAR* Kind = Item.Key > 0 ? TEXT("export") : TEXT("import");
			const int32 Ordinal = Item.Key > 0 ? Item.Key : -Item.Key;
			Identity += FString::Printf(TEXT("#%s-%08d"), Kind, Ordinal);
		}
		const FString Folded = WoTRemainingFold(Identity);
		if (Seen.Contains(Folded))
			return FWoTBootstrapStatus::Error(WoTRemainingError(
				EWoTBootstrapError::DuplicateField,
				TEXT("canonical package identities are not injective"), Identity));
		Seen.Add(Folded);
		Source.CanonicalIdentityByReference.Add(Item.Key, MoveTemp(Identity));
	}
	// This is only an O(1) inverse index over the final reference-owned table;
	// CanonicalIdentityByReference remains the single source of every suffix.
	for (const TPair<int32, FString>& Item : Source.CanonicalIdentityByReference)
		Source.CanonicalReferenceByIdentity.Add(
			WoTRemainingFold(Item.Value), Item.Key);
	for (int32 ImportIndex = 0; ImportIndex < Source.Imports.Num(); ++ImportIndex)
	{
		const FWoTRemainingImport& Import = Source.Imports[ImportIndex];
		Source.CanonicalClassByReference.Add(
			-ImportIndex - 1, Import.ClassPackage + TEXT(".") + Import.ClassName);
	}
	for (int32 ExportIndex = 0; ExportIndex < Source.Exports.Num(); ++ExportIndex)
	{
		const int32 ClassReference = Source.Exports[ExportIndex].ClassReference;
		FString ClassIdentity = TEXT("Core.Class");
		if (ClassReference != 0)
		{
			const FString* Canonical =
				Source.CanonicalIdentityByReference.Find(ClassReference);
			if (Canonical == nullptr)
				return FWoTBootstrapStatus::Error(WoTRemainingError(
					EWoTBootstrapError::CorruptInstallation,
					TEXT("an export class lacks a canonical registry row"), Source.Path));
			ClassIdentity = *Canonical;
		}
		Source.CanonicalClassByReference.Add(ExportIndex + 1, MoveTemp(ClassIdentity));
	}
	return FWoTBootstrapStatus::Success();
}
} // namespace

FWoTBootstrapError WoTRemainingError(EWoTBootstrapError Code,
	const TCHAR* Message, FString Path)
{
	return MakeWoTBootstrapError(Code, Message, MoveTemp(Path));
}

FWoTRemainingReader::FWoTRemainingReader(TConstArrayView<uint8> InBytes)
	: Bytes(InBytes), End(static_cast<uint64>(InBytes.Num())) {}

FWoTRemainingReader::FWoTRemainingReader(TConstArrayView<uint8> InBytes,
	uint64 InOffset, uint64 InEnd)
	: Bytes(InBytes), Offset(InOffset), End(InEnd)
{
	if (InOffset > InEnd || InEnd > static_cast<uint64>(InBytes.Num())) bValid = false;
}

bool FWoTRemainingReader::ReadU8(uint8& Out)
{
	if (!Require(1)) return false;
	Out = Bytes[static_cast<int32>(Offset++)];
	return true;
}

bool FWoTRemainingReader::ReadU16(uint16& Out)
{
	if (!Require(2)) return false;
	const uint8* Data = Bytes.GetData() + Offset;
	Out = static_cast<uint16>(Data[0])
		| static_cast<uint16>(static_cast<uint16>(Data[1]) << 8);
	Offset += 2;
	return true;
}

bool FWoTRemainingReader::ReadU32(uint32& Out)
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

bool FWoTRemainingReader::ReadI32(int32& Out)
{
	uint32 Value = 0;
	if (!ReadU32(Value)) return false;
	Out = static_cast<int32>(Value);
	return true;
}

bool FWoTRemainingReader::ReadCompact(int32& Out)
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

bool FWoTRemainingReader::ReadBytes(uint64 Count, TArray<uint8>& Out)
{
	if (!Require(Count) || Count > static_cast<uint64>(MAX_int32)) return false;
	Out.Reset(static_cast<int32>(Count));
	Out.Append(Bytes.GetData() + Offset, static_cast<int32>(Count));
	Offset += Count;
	return true;
}

bool FWoTRemainingReader::Skip(uint64 Count)
{
	if (!Require(Count)) return false;
	Offset += Count;
	return true;
}

bool FWoTRemainingReader::SetOffset(uint64 Value)
{
	if (Value > End)
	{
		bValid = false;
		return false;
	}
	Offset = Value;
	return true;
}

bool FWoTRemainingReader::Require(uint64 Count)
{
	if (!bValid || Offset > End || Count > End - Offset)
	{
		bValid = false;
		return false;
	}
	return true;
}

uint64 FWoTRemainingReader::Tell() const { return Offset; }
uint64 FWoTRemainingReader::GetEnd() const { return End; }
uint64 FWoTRemainingReader::Remaining() const { return Offset <= End ? End - Offset : 0; }
bool FWoTRemainingReader::IsValid() const { return bValid; }

const FString* FWoTRemainingSource::NameAt(int32 Index) const
{
	return Names.IsValidIndex(Index) ? &Names[Index] : nullptr;
}

const FString* FWoTRemainingSource::ObjectName(int32 Reference) const
{
	if (Reference > 0 && Exports.IsValidIndex(Reference - 1))
		return &Exports[Reference - 1].Name;
	if (Reference < 0)
	{
		const int64 Index = -static_cast<int64>(Reference) - 1;
		return Imports.IsValidIndex(static_cast<int32>(Index))
			? &Imports[static_cast<int32>(Index)].Name : nullptr;
	}
	return nullptr;
}

FString WoTRemainingFold(FStringView Value)
{
	FString Result(Value);
	for (int32 Index = 0; Index < Result.Len(); ++Index)
	{
		if (Result[Index] >= TEXT('A') && Result[Index] <= TEXT('Z'))
			Result[Index] += TEXT('a') - TEXT('A');
	}
	return Result;
}

bool WoTRemainingReferenceValid(const FWoTRemainingSource& Source, int32 Reference)
{
	if (Reference == 0) return true;
	if (Reference > 0) return Source.Exports.IsValidIndex(Reference - 1);
	const int64 Index = -static_cast<int64>(Reference) - 1;
	return Index >= 0 && Index < Source.Imports.Num();
}

TWoTBootstrapResult<FString> WoTRemainingObjectPath(
	const FWoTRemainingSource& Source, int32 Reference, bool bOptional)
{
	using FResult = TWoTBootstrapResult<FString>;
	if (Reference == 0)
	{
		return bOptional ? FResult::Success(FString())
			: FResult::Error(WoTRemainingError(EWoTBootstrapError::CorruptInstallation,
				TEXT("a required object reference is null"), Source.Path));
	}
	TArray<FString> Reverse;
	TSet<int32> Seen;
	int32 Current = Reference;
	bool bLocalExport = Reference > 0;
	for (uint32 Depth = 0; Current != 0 && Depth < WoTRemainingMaxOuterDepth; ++Depth)
	{
		if (!WoTRemainingReferenceValid(Source, Current) || Seen.Contains(Current))
		{
			return FResult::Error(WoTRemainingError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("an object reference has an invalid or cyclic outer chain"), Source.Path));
		}
		Seen.Add(Current);
		if (Current > 0)
		{
			const FWoTRemainingExport& Export = Source.Exports[Current - 1];
			Reverse.Add(Export.Name);
			Current = Export.Outer;
		}
		else
		{
			const FWoTRemainingImport& Import = Source.Imports[-Current - 1];
			Reverse.Add(Import.Name);
			Current = Import.Outer;
		}
	}
	if (Current != 0)
	{
		return FResult::Error(WoTRemainingError(EWoTBootstrapError::SizeLimit,
			TEXT("an object outer chain exceeds its depth bound"), Source.Path));
	}
	if (bLocalExport) Reverse.Add(Source.PackageName);
	FString Identity;
	for (int32 Index = Reverse.Num() - 1; Index >= 0; --Index)
	{
		if (Reverse[Index].IsEmpty())
		{
			return FResult::Error(WoTRemainingError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("an object identity contains an empty component"), Source.Path));
		}
		if (!Identity.IsEmpty()) Identity += TEXT(".");
		Identity += Reverse[Index];
	}
	return FResult::Success(MoveTemp(Identity));
}

TWoTBootstrapResult<FString> WoTRemainingCanonicalIdentity(
	const FWoTRemainingSource& Source, int32 Reference, bool bOptional)
{
	using FResult = TWoTBootstrapResult<FString>;
	if (Reference == 0)
	{
		return bOptional ? FResult::Success(FString())
			: FResult::Error(WoTRemainingError(EWoTBootstrapError::CorruptInstallation,
				TEXT("a required canonical reference is null"), Source.Path));
	}
	const FString* Identity = Source.CanonicalIdentityByReference.Find(Reference);
	if (Identity == nullptr)
		return FResult::Error(WoTRemainingError(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("an object lacks a canonical registry identity"), Source.Path));
	return FResult::Success(*Identity);
}

TWoTBootstrapResult<FString> WoTRemainingObjectClass(
	const FWoTRemainingSource& Source, int32 Reference)
{
	using FResult = TWoTBootstrapResult<FString>;
	if (Reference == 0) return FResult::Success(FString());
	if (!WoTRemainingReferenceValid(Source, Reference))
		return FResult::Error(WoTRemainingError(EWoTBootstrapError::CorruptInstallation,
			TEXT("an object reference is outside the complete registry"), Source.Path));
	const FString* ClassIdentity = Source.CanonicalClassByReference.Find(Reference);
	if (ClassIdentity == nullptr)
		return FResult::Error(WoTRemainingError(EWoTBootstrapError::CorruptInstallation,
			TEXT("an object lacks a canonical class binding"), Source.Path));
	return FResult::Success(*ClassIdentity);
}

FWoTBootstrapStatus WoTRemainingRequireSemanticReference(
	const FWoTRemainingSource& Source, FStringView Identity,
	FStringView ExpectedClass, FStringView Context)
{
	int32 MatchReference = 0;
	uint32 MatchCount = 0;
	for (const TPair<int32, FString>& Item : Source.CanonicalIdentityByReference)
	{
		if (FStringView(Item.Value).Equals(Identity, ESearchCase::IgnoreCase))
		{
			MatchReference = Item.Key;
			++MatchCount;
		}
	}
	const FString* ActualClass =
		Source.CanonicalClassByReference.Find(MatchReference);
	if (MatchCount != 1 || ActualClass == nullptr
		|| !FStringView(*ActualClass).Equals(ExpectedClass, ESearchCase::IgnoreCase))
	{
		return FWoTBootstrapStatus::Error(WoTRemainingError(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("an emitted semantic reference does not resolve to exactly one registry row with its expected class"),
			FString(Context)));
	}
	return FWoTBootstrapStatus::Success();
}

TWoTBootstrapResult<FWoTRemainingSource> WoTRemainingParsePackage(
	TConstArrayView<uint8> SourceBytes, FStringView Path)
{
	using FResult = TWoTBootstrapResult<FWoTRemainingSource>;
	if (SourceBytes.IsEmpty()
		|| static_cast<uint64>(SourceBytes.Num()) > WoTRemainingMaxSourceBytes)
	{
		return FResult::Error(WoTRemainingError(EWoTBootstrapError::SizeLimit,
			TEXT("the remaining-domain UE1 package exceeds its source bound"), FString(Path)));
	}
	FWoTRemainingSource Source;
	Source.Path = FString(Path);
	Source.PackageName = FPaths::GetBaseFilename(Source.Path);
	Source.Bytes.Append(SourceBytes.GetData(), SourceBytes.Num());
	Source.Sha256 = Sha256(Source.Bytes);
	FWoTRemainingReader Reader(Source.Bytes);
	uint32 Tag = 0, PackageFlags = 0;
	uint16 Licensee = 0;
	int32 NameCount = 0, NameOffset = 0, ExportCount = 0, ExportOffset = 0;
	int32 ImportCount = 0, ImportOffset = 0;
	if (!Reader.ReadU32(Tag) || !Reader.ReadU16(Source.Version)
		|| !Reader.ReadU16(Licensee) || !Reader.ReadU32(PackageFlags)
		|| !Reader.ReadI32(NameCount) || !Reader.ReadI32(NameOffset)
		|| !Reader.ReadI32(ExportCount) || !Reader.ReadI32(ExportOffset)
		|| !Reader.ReadI32(ImportCount) || !Reader.ReadI32(ImportOffset)
		|| Tag != WoTRemainingPackageTag || Source.Version < 61 || Source.Version > 69
		|| NameCount <= 0 || NameCount > static_cast<int32>(WoTRemainingMaxNames)
		|| ExportCount <= 0 || ExportCount > static_cast<int32>(WoTRemainingMaxItems)
		|| ImportCount < 0 || ImportCount > static_cast<int32>(WoTRemainingMaxItems)
		|| NameOffset < 0 || ExportOffset < 0 || ImportOffset < 0)
	{
		return FResult::Error(WoTRemainingError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the remaining-domain source is not a bounded UE1 package"), Source.Path));
	}
	if (!Reader.SetOffset(static_cast<uint64>(NameOffset)))
		return FResult::Error(WoTRemainingError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the UE1 name table is outside the package"), Source.Path));
	Source.Names.Reserve(NameCount);
	TMap<FString, FString> SeenNames;
	SeenNames.Reserve(NameCount);
	for (int32 Index = 0; Index < NameCount; ++Index)
	{
		TArray<uint8> Raw;
		if (Source.Version >= 64)
		{
			int32 Length = 0;
			if (!Reader.ReadCompact(Length) || Length <= 0
				|| Length > static_cast<int32>(WoTRemainingMaxNameBytes)
				|| !Reader.ReadBytes(static_cast<uint64>(Length), Raw)
				|| Raw.Last() != 0)
			{
				return FResult::Error(WoTRemainingError(
					EWoTBootstrapError::CorruptInstallation,
					TEXT("the UE1 name table has an invalid counted name"), Source.Path));
			}
			Raw.Pop(EAllowShrinking::No);
		}
		else
		{
			bool bTerminated = false;
			for (uint32 ByteIndex = 0; ByteIndex < WoTRemainingMaxNameBytes; ++ByteIndex)
			{
				uint8 Byte = 0;
				if (!Reader.ReadU8(Byte)) break;
				if (Byte == 0) { bTerminated = true; break; }
				Raw.Add(Byte);
			}
			if (!bTerminated)
				return FResult::Error(WoTRemainingError(
					EWoTBootstrapError::CorruptInstallation,
					TEXT("the UE1 name table is truncated"), Source.Path));
		}
		uint32 NameFlags = 0;
		FString Name;
		if (!Reader.ReadU32(NameFlags) || !WoTRemainingLatin1(Raw, Name)
			|| Name.IsEmpty() || Name.Contains(TEXT("."), ESearchCase::CaseSensitive))
		{
			return FResult::Error(WoTRemainingError(EWoTBootstrapError::CorruptInstallation,
				TEXT("the UE1 name table contains an unsafe component"), Source.Path));
		}
		const FString FoldedName = WoTRemainingFold(Name);
		// Reject only CASE-VARIANT collisions: two different raw spellings that
		// fold to one UE1 identity are corruption or an aliasing attempt (FName
		// is case-insensitive). Identical repeats are legal: 8 of 20
		// retail System packages serialize the literal name "None" more than
		// once (Angreal.u seven times), and the first version of this check -
		// which refused ANY folded repeat - rejected them all. Python's rule
		// (build_payload_cache "duplicate package name after case folding")
		// makes exactly this spelling distinction; mirror it.
		if (const FString* Existing = SeenNames.Find(FoldedName))
		{
			if (!Existing->Equals(Name, ESearchCase::CaseSensitive))
			{
				return FResult::Error(WoTRemainingError(EWoTBootstrapError::DuplicateField,
					TEXT("the UE1 name table repeats a case-folded name"), Source.Path));
			}
		}
		else
		{
			SeenNames.Add(FoldedName, Name);
		}
		Source.Names.Add(MoveTemp(Name));
	}
	if (!Reader.SetOffset(static_cast<uint64>(ImportOffset)))
		return FResult::Error(WoTRemainingError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the UE1 import table is outside the package"), Source.Path));
	Source.Imports.Reserve(ImportCount);
	for (int32 Index = 0; Index < ImportCount; ++Index)
	{
		int32 ClassPackage = 0, ClassName = 0, Outer = 0, Name = 0;
		if (!Reader.ReadCompact(ClassPackage) || !Reader.ReadCompact(ClassName)
			|| !Reader.ReadI32(Outer) || !Reader.ReadCompact(Name)
			|| Source.NameAt(ClassPackage) == nullptr
			|| Source.NameAt(ClassName) == nullptr || Source.NameAt(Name) == nullptr)
		{
			return FResult::Error(WoTRemainingError(EWoTBootstrapError::CorruptInstallation,
				TEXT("the UE1 import table is invalid"), Source.Path));
		}
		FWoTRemainingImport& Import = Source.Imports.AddDefaulted_GetRef();
		Import.ClassPackage = *Source.NameAt(ClassPackage);
		Import.ClassName = *Source.NameAt(ClassName);
		Import.Outer = Outer;
		Import.Name = *Source.NameAt(Name);
	}
	if (!Reader.SetOffset(static_cast<uint64>(ExportOffset)))
		return FResult::Error(WoTRemainingError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the UE1 export table is outside the package"), Source.Path));
	Source.Exports.Reserve(ExportCount);
	for (int32 Index = 0; Index < ExportCount; ++Index)
	{
		int32 ClassReference = 0, SuperReference = 0, Outer = 0, Name = 0;
		uint32 Flags = 0;
		int32 Size = 0, Offset = 0;
		if (!Reader.ReadCompact(ClassReference) || !Reader.ReadCompact(SuperReference)
			|| !Reader.ReadI32(Outer) || !Reader.ReadCompact(Name)
			|| !Reader.ReadU32(Flags) || !Reader.ReadCompact(Size) || Size < 0
			|| Source.NameAt(Name) == nullptr
			|| (Size > 0 && !Reader.ReadCompact(Offset)) || Offset < 0
			|| static_cast<uint64>(Offset) > static_cast<uint64>(Source.Bytes.Num())
			|| static_cast<uint64>(Size)
				> static_cast<uint64>(Source.Bytes.Num()) - static_cast<uint64>(Offset))
		{
			return FResult::Error(WoTRemainingError(EWoTBootstrapError::CorruptInstallation,
				TEXT("the UE1 export table is invalid"), Source.Path));
		}
		FWoTRemainingExport& Export = Source.Exports.AddDefaulted_GetRef();
		Export.ClassReference = ClassReference;
		Export.SuperReference = SuperReference;
		Export.Outer = Outer;
		Export.Name = *Source.NameAt(Name);
		Export.Flags = Flags;
		Export.Offset = static_cast<uint64>(Offset);
		Export.Size = static_cast<uint64>(Size);
	}
	FWoTBootstrapStatus Canonical = WoTRemainingBuildCanonicalIdentityTable(Source);
	if (Canonical.IsError()) return FResult::Error(MoveTemp(Canonical).GetError());
	// Null-class UClass and conform-padding exports are valid retail data.
	return FResult::Success(MoveTemp(Source));
}

bool WoTRemainingOpenExport(const FWoTRemainingSource& Source,
	const FWoTRemainingExport& Export, FWoTRemainingReader& Out)
{
	Out = FWoTRemainingReader(Source.Bytes, Export.Offset, Export.Offset + Export.Size);
	if ((Export.Flags & WoTRemainingHasStack) != 0)
	{
		int32 Node = 0, StateNode = 0;
		if (!Out.ReadCompact(Node) || !Out.ReadCompact(StateNode) || !Out.Skip(12)
			|| (Node != 0 && !Out.ReadCompact(StateNode))) return false;
	}
	return Out.IsValid();
}

bool WoTRemainingReadName(FWoTRemainingReader& Reader,
	const FWoTRemainingSource& Source, FString& Out)
{
	int32 Index = 0;
	if (!Reader.ReadCompact(Index) || Source.NameAt(Index) == nullptr) return false;
	Out = *Source.NameAt(Index);
	return true;
}

FWoTRemainingPackageResolver::FWoTRemainingPackageResolver(
	const FWoTRetailContract& Contract, const FWoTRemainingSourceLoader& InLoader)
	: Loader(InLoader)
{
	for (const FWoTRequiredFact& Fact : Contract.Facts)
	{
		const FString Stem = FPaths::GetBaseFilename(Fact.SourcePath);
		if (Stem.IsEmpty()) continue;
		SourcePathByPackage.FindOrAdd(WoTRemainingFold(Stem), Fact.SourcePath);
	}
}

TWoTBootstrapResult<const FWoTRemainingSource*>
FWoTRemainingPackageResolver::Resolve(
	FStringView PackageName, const FWoTRemainingSource& Current)
{
	using FResult = TWoTBootstrapResult<const FWoTRemainingSource*>;
	const FString Folded = WoTRemainingFold(PackageName);
	if (Folded == WoTRemainingFold(Current.PackageName))
		return FResult::Success(&Current);
	if (const TUniquePtr<FWoTRemainingSource>* Cached = Parsed.Find(Folded))
		return FResult::Success(Cached->Get());
	const FString* SourcePath = SourcePathByPackage.Find(Folded);
	if (SourcePath == nullptr)
	{
		return FResult::Error(WoTRemainingError(EWoTBootstrapError::UnknownField,
			TEXT("a procedural reference names a package outside the contract"),
			FString(PackageName)));
	}
	TWoTBootstrapResult<TArray<uint8>> Loaded = Loader(*SourcePath);
	if (Loaded.IsError()) return FResult::Error(MoveTemp(Loaded).GetError());
	TWoTBootstrapResult<FWoTRemainingSource> Source =
		WoTRemainingParsePackage(Loaded.GetValue(), *SourcePath);
	if (Source.IsError()) return FResult::Error(MoveTemp(Source).GetError());
	TUniquePtr<FWoTRemainingSource>& Slot = Parsed.Add(Folded,
		MakeUnique<FWoTRemainingSource>(MoveTemp(Source).GetValue()));
	return FResult::Success(Slot.Get());
}
