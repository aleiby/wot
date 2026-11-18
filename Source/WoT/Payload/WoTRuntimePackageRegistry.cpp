#include "WoTRuntimePackageRegistry.h"

#include "Containers/StringConv.h"

namespace
{
constexpr uint32 WoTRuntimeRegistryMaximumRows = 1000000;

FWoTBootstrapStatus WoTRuntimePackageRegistryError(const TCHAR* Message)
{
	return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
		EWoTBootstrapError::CorruptInstallation, Message));
}

FString WoTRuntimePackageRegistryFold(FStringView Value)
{
	FString Folded(Value);
	Folded.ToLowerInline();
	return Folded;
}

class FWoTRuntimePackageRegistryCursor final
{
public:
	explicit FWoTRuntimePackageRegistryCursor(TConstArrayView<uint8> InBytes)
		: Bytes(InBytes)
	{
	}

	bool Skip(uint32 Count)
	{
		if (static_cast<uint64>(Offset) + Count
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

	bool ReadString(FString& Out)
	{
		uint32 Count = 0;
		if (!ReadU32(Count) || Count > 1024u * 1024u
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

private:
	TConstArrayView<uint8> Bytes;
	int32 Offset = 0;
};

bool WoTRuntimePackageRegistryReadU32(
	const FWoTCanonicalJsonValue& Header, const TCHAR* Field, uint32& Out)
{
	return !ReadUnsigned32Field(
		Header, Field, TEXT("package registry header"), Out).IsSet();
}

bool WoTRuntimePackageRegistryReadString(
	const FWoTCanonicalJsonValue& Header, const TCHAR* Field, FString& Out)
{
	return !ReadStringField(
		Header, Field, TEXT("package registry header"), Out).IsSet();
}

bool WoTRuntimePackageRegistryValidateSource(
	const FWoTCanonicalJsonValue& Header)
{
	const FWoTCanonicalJsonValue* Source = nullptr;
	static const TCHAR* Fields[] = {
		TEXT("export"), TEXT("package"), TEXT("sha256")
	};
	FString Export;
	FString Package;
	FString Hash;
	return !ReadObjectField(
			Header, TEXT("source"), TEXT("package registry header"), Source).IsSet()
		&& Source != nullptr
		&& !RequireExactMembers(
			*Source, MakeArrayView(Fields),
			TEXT("package registry source")).IsSet()
		&& WoTRuntimePackageRegistryReadString(
			*Source, TEXT("export"), Export)
		&& WoTRuntimePackageRegistryReadString(
			*Source, TEXT("package"), Package)
		&& WoTRuntimePackageRegistryReadString(
			*Source, TEXT("sha256"), Hash)
		&& !Package.IsEmpty() && Hash.Len() == 64;
}

bool WoTRuntimePackageRegistryOrdered(
	const FString& Previous, const FString& Current)
{
	if (Previous.IsEmpty())
	{
		return true;
	}
	const FString PreviousFolded = WoTRuntimePackageRegistryFold(Previous);
	const FString CurrentFolded = WoTRuntimePackageRegistryFold(Current);
	return CurrentFolded > PreviousFolded
		|| (CurrentFolded == PreviousFolded && Current > Previous);
}
}

FWoTBootstrapStatus FWoTRuntimePackageRegistry::LoadFirstUse(
	const FWoTNeutralResourceId& Resource)
{
	if (Object.IsSet() || Resource.Kind != EWoTNeutralResourceKind::Registry)
	{
		return WoTRuntimePackageRegistryError(
			TEXT("package registry was reused or has the wrong semantic kind"));
	}
	TWoTBootstrapResult<FWoTRuntimeVerifiedObject> Read =
		FWoTRuntimeRegistry::Get().ReadObject(
			Resource, 256ull * 1024ull * 1024ull,
			TEXT("ue1-package-registry-v1"), TEXT("registry"));
	if (Read.IsError())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(Read).GetError());
	}
	FWoTRuntimeVerifiedObject Verified =
		MoveTemp(Read).GetValue();
	const FWoTCanonicalJsonValue& Header = Verified.Header();
	static const TCHAR* HeaderFields[] = {
		TEXT("class_count"), TEXT("directive_count"), TEXT("encoding"),
		TEXT("export_count"), TEXT("fact_id"), TEXT("import_count"),
		TEXT("importer"), TEXT("importer_version"), TEXT("kind"),
		TEXT("name_count"), TEXT("payload_byte_length"), TEXT("source")
	};
	uint32 ClassCount = 0;
	uint32 DirectiveCount = 0;
	uint32 ExportCount = 0;
	uint32 ImportCount = 0;
	uint32 NameCount = 0;
	uint32 ImporterVersion = 0;
	FString FactId;
	FString Importer;
	if (RequireExactMembers(
			Header, MakeArrayView(HeaderFields),
			TEXT("package registry header")).IsSet()
		|| !WoTRuntimePackageRegistryReadU32(
			Header, TEXT("class_count"), ClassCount)
		|| !WoTRuntimePackageRegistryReadU32(
			Header, TEXT("directive_count"), DirectiveCount)
		|| !WoTRuntimePackageRegistryReadU32(
			Header, TEXT("export_count"), ExportCount)
		|| !WoTRuntimePackageRegistryReadU32(
			Header, TEXT("import_count"), ImportCount)
		|| !WoTRuntimePackageRegistryReadU32(
			Header, TEXT("name_count"), NameCount)
		|| !WoTRuntimePackageRegistryReadU32(
			Header, TEXT("importer_version"), ImporterVersion)
		|| !WoTRuntimePackageRegistryReadString(
			Header, TEXT("fact_id"), FactId)
		|| !WoTRuntimePackageRegistryReadString(
			Header, TEXT("importer"), Importer)
		|| ClassCount > WoTRuntimeRegistryMaximumRows
		|| DirectiveCount > WoTRuntimeRegistryMaximumRows
		|| ExportCount > WoTRuntimeRegistryMaximumRows
		|| ImportCount > WoTRuntimeRegistryMaximumRows
		|| NameCount > WoTRuntimeRegistryMaximumRows
		|| FactId.IsEmpty() || Importer != TEXT("registry-v1")
		|| ImporterVersion != 1
		|| !WoTRuntimePackageRegistryValidateSource(Header))
	{
		return WoTRuntimePackageRegistryError(
			TEXT("package registry canonical header is malformed"));
	}

	const TConstArrayView<uint8> Payload = Verified.Payload();
	FWoTRuntimePackageRegistryCursor Cursor(Payload);
	uint16 Version = 0;
	uint16 Flags = 0;
	uint32 PayloadNameCount = 0;
	uint32 PayloadImportCount = 0;
	uint32 PayloadExportCount = 0;
	uint32 PayloadClassCount = 0;
	uint32 PayloadDirectiveCount = 0;
	if (Payload.Num() < 32
		|| FMemory::Memcmp(Payload.GetData(), "WOTREG3", 8) != 0
		|| !Cursor.Skip(8) || !Cursor.ReadU16(Version)
		|| !Cursor.ReadU16(Flags) || !Cursor.ReadU32(PayloadNameCount)
		|| !Cursor.ReadU32(PayloadImportCount)
		|| !Cursor.ReadU32(PayloadExportCount)
		|| !Cursor.ReadU32(PayloadClassCount)
		|| !Cursor.ReadU32(PayloadDirectiveCount)
		|| Version != 1 || Flags != 0
		|| PayloadNameCount != NameCount
		|| PayloadImportCount != ImportCount
		|| PayloadExportCount != ExportCount
		|| PayloadClassCount != ClassCount
		|| PayloadDirectiveCount != DirectiveCount)
	{
		return WoTRuntimePackageRegistryError(
			TEXT("package registry payload framing is malformed"));
	}

	TSet<FString> ParsedNames;
	FString Previous;
	for (uint32 Index = 0; Index < NameCount; ++Index)
	{
		FString Name;
		if (!Cursor.ReadString(Name) || Name.IsEmpty()
			|| !WoTRuntimePackageRegistryOrdered(Previous, Name)
			|| ParsedNames.Contains(WoTRuntimePackageRegistryFold(Name)))
		{
			return WoTRuntimePackageRegistryError(
				TEXT("package registry name table is malformed"));
		}
		ParsedNames.Add(WoTRuntimePackageRegistryFold(Name));
		Previous = MoveTemp(Name);
	}

	Previous.Empty();
	for (uint32 Index = 0; Index < ImportCount; ++Index)
	{
		FString Identity;
		FString ClassIdentity;
		if (!Cursor.ReadString(Identity) || Identity.IsEmpty()
			|| !Cursor.ReadString(ClassIdentity) || ClassIdentity.IsEmpty()
			|| !WoTRuntimePackageRegistryOrdered(Previous, Identity))
		{
			return WoTRuntimePackageRegistryError(
				TEXT("package registry import table is malformed"));
		}
		Previous = MoveTemp(Identity);
	}

	TMap<FString, FWoTRuntimePackageExport> ParsedExports;
	Previous.Empty();
	for (uint32 Index = 0; Index < ExportCount; ++Index)
	{
		FString Identity;
		FWoTRuntimePackageExport Export;
		uint8 Placeholder = 0;
		if (!Cursor.ReadString(Identity) || Identity.IsEmpty()
			|| !Cursor.ReadString(Export.ClassIdentity)
			|| Export.ClassIdentity.IsEmpty()
			|| !Cursor.ReadU32(Export.ExportIndex) || Export.ExportIndex == 0
			|| !Cursor.ReadU8(Placeholder) || Placeholder > 1
			|| !WoTRuntimePackageRegistryOrdered(Previous, Identity)
			|| ParsedExports.Contains(
				WoTRuntimePackageRegistryFold(Identity)))
		{
			return WoTRuntimePackageRegistryError(
				TEXT("package registry export table is malformed"));
		}
		Export.bPlaceholder = Placeholder != 0;
		ParsedExports.Add(
			WoTRuntimePackageRegistryFold(Identity), MoveTemp(Export));
		Previous = MoveTemp(Identity);
	}

	Previous.Empty();
	for (uint32 Index = 0; Index < ClassCount; ++Index)
	{
		FString Identity;
		uint32 ExportIndex = 0;
		uint8 Placeholder = 0;
		if (!Cursor.ReadString(Identity) || Identity.IsEmpty()
			|| !Cursor.ReadU32(ExportIndex) || ExportIndex == 0
			|| !Cursor.ReadU8(Placeholder) || Placeholder > 1
			|| !WoTRuntimePackageRegistryOrdered(Previous, Identity))
		{
			return WoTRuntimePackageRegistryError(
				TEXT("package registry class table is malformed"));
		}
		const FWoTRuntimePackageExport* Export =
			ParsedExports.Find(WoTRuntimePackageRegistryFold(Identity));
		if (Export == nullptr || Export->ExportIndex != ExportIndex
			|| Export->bPlaceholder != (Placeholder != 0))
		{
			return WoTRuntimePackageRegistryError(
				TEXT("package registry class/export binding mismatched"));
		}
		Previous = MoveTemp(Identity);
	}

	FString PreviousOwner;
	uint32 PreviousLine = 0;
	for (uint32 Index = 0; Index < DirectiveCount; ++Index)
	{
		FString Owner;
		uint32 LineIndex = 0;
		uint32 LineBytes = 0;
		if (!Cursor.ReadString(Owner) || Owner.IsEmpty()
			|| !Cursor.ReadU32(LineIndex) || !Cursor.ReadU32(LineBytes)
			|| LineBytes > 1024u * 1024u || !Cursor.Skip(LineBytes))
		{
			return WoTRuntimePackageRegistryError(
				TEXT("package registry directive table is malformed"));
		}
		const FString FoldedOwner = WoTRuntimePackageRegistryFold(Owner);
		const FString FoldedPrevious =
			WoTRuntimePackageRegistryFold(PreviousOwner);
		if (Index > 0
			&& (FoldedOwner < FoldedPrevious
				|| (FoldedOwner == FoldedPrevious
					&& (Owner < PreviousOwner
						|| (Owner == PreviousOwner
							&& LineIndex <= PreviousLine)))))
		{
			return WoTRuntimePackageRegistryError(
				TEXT("package registry directive order is non-canonical"));
		}
		PreviousOwner = MoveTemp(Owner);
		PreviousLine = LineIndex;
	}
	if (!Cursor.AtEnd())
	{
		return WoTRuntimePackageRegistryError(
			TEXT("package registry payload has a trailing byte"));
	}
	Names = MoveTemp(ParsedNames);
	Exports = MoveTemp(ParsedExports);
	Object.Emplace(MoveTemp(Verified));
	return FWoTBootstrapStatus::Success();
}

const FWoTRuntimePackageExport*
FWoTRuntimePackageRegistry::FindExport(FStringView Identity) const
{
	return Object.IsSet()
		? Exports.Find(WoTRuntimePackageRegistryFold(Identity)) : nullptr;
}

bool FWoTRuntimePackageRegistry::ContainsName(FStringView Name) const
{
	return Object.IsSet()
		&& Names.Contains(WoTRuntimePackageRegistryFold(Name));
}
