#include "WoTRuntimeLocalizationDefaults.h"

#include "../World/WoTRuntimeWorldProperties.h"

#include "Containers/StringConv.h"

namespace
{
constexpr uint32 WoTRuntimeTableMaximumRows = 1000000;

FWoTBootstrapStatus WoTRuntimeTableError(const TCHAR* Message)
{
	return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
		EWoTBootstrapError::CorruptInstallation, Message));
}

bool WoTRuntimeTableReadU32(
	const FWoTCanonicalJsonValue& Header, const TCHAR* Field, uint32& Out)
{
	return !ReadUnsigned32Field(
		Header, Field, TEXT("runtime table header"), Out).IsSet();
}

bool WoTRuntimeTableReadString(
	const FWoTCanonicalJsonValue& Header, const TCHAR* Field, FString& Out)
{
	return !ReadStringField(
		Header, Field, TEXT("runtime table header"), Out).IsSet();
}

bool WoTRuntimeTableValidateSource(const FWoTCanonicalJsonValue& Header)
{
	const FWoTCanonicalJsonValue* Source = nullptr;
	static const TCHAR* SourceFields[] = {
		TEXT("export"), TEXT("package"), TEXT("sha256")
	};
	FString Export;
	FString Package;
	FString Sha256;
	return !ReadObjectField(
			Header, TEXT("source"), TEXT("runtime table header"), Source).IsSet()
		&& Source != nullptr
		&& !RequireExactMembers(
			*Source, MakeArrayView(SourceFields),
			TEXT("runtime table source")).IsSet()
		&& WoTRuntimeTableReadString(*Source, TEXT("export"), Export)
		&& WoTRuntimeTableReadString(*Source, TEXT("package"), Package)
		&& WoTRuntimeTableReadString(*Source, TEXT("sha256"), Sha256)
		&& !Package.IsEmpty() && Sha256.Len() == 64;
}

bool WoTRuntimeTableUtf8Valid(TConstArrayView<uint8> Bytes)
{
	for (int32 Offset = 0; Offset < Bytes.Num();)
	{
		const uint8 First = Bytes[Offset++];
		if (First < 0x80)
		{
			continue;
		}
		uint32 Codepoint = 0;
		int32 Continuations = 0;
		if (First >= 0xc2 && First <= 0xdf)
		{
			Codepoint = First & 0x1f;
			Continuations = 1;
		}
		else if (First >= 0xe0 && First <= 0xef)
		{
			Codepoint = First & 0x0f;
			Continuations = 2;
		}
		else if (First >= 0xf0 && First <= 0xf4)
		{
			Codepoint = First & 0x07;
			Continuations = 3;
		}
		else
		{
			return false;
		}
		if (Offset + Continuations > Bytes.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < Continuations; ++Index)
		{
			const uint8 Next = Bytes[Offset++];
			if ((Next & 0xc0) != 0x80)
			{
				return false;
			}
			Codepoint = (Codepoint << 6) | (Next & 0x3f);
		}
		if ((Continuations == 2 && Codepoint < 0x800)
			|| (Continuations == 3 && Codepoint < 0x10000)
			|| Codepoint > 0x10ffff
			|| (Codepoint >= 0xd800 && Codepoint <= 0xdfff))
		{
			return false;
		}
	}
	return true;
}

class FWoTRuntimeTableCursor final
{
public:
	explicit FWoTRuntimeTableCursor(TConstArrayView<uint8> InBytes)
		: Bytes(InBytes)
	{
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
		uint32 ByteCount = 0;
		if (!ReadU32(ByteCount) || ByteCount > 1024u * 1024u
			|| static_cast<uint64>(Offset) + ByteCount
				> static_cast<uint64>(Bytes.Num()))
		{
			return false;
		}
		const TConstArrayView<uint8> Encoded(
			Bytes.GetData() + Offset, static_cast<int32>(ByteCount));
		if (!WoTRuntimeTableUtf8Valid(Encoded))
		{
			return false;
		}
		const FUTF8ToTCHAR Converted(
			reinterpret_cast<const ANSICHAR*>(Encoded.GetData()),
			Encoded.Num());
		Out = FString(Converted.Length(), Converted.Get());
		Offset += static_cast<int32>(ByteCount);
		return true;
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

	bool AtEnd() const { return Offset == Bytes.Num(); }
	int32 Tell() const { return Offset; }
	TConstArrayView<uint8> View() const { return Bytes; }
	bool SetOffset(int32 Value)
	{
		if (Value < Offset || Value > Bytes.Num()) return false;
		Offset = Value;
		return true;
	}

private:
	TConstArrayView<uint8> Bytes;
	int32 Offset = 0;
};

uint32 WoTRuntimeTableReferenceCount(
	TConstArrayView<FWoTRuntimeWorldProperty> Properties)
{
	uint32 Result = 0;
	for (const FWoTRuntimeWorldProperty& Property : Properties)
	{
		Result += Property.ObjectIdentity.IsEmpty() ? 0u : 1u;
		Result += WoTRuntimeTableReferenceCount(Property.StructMembers);
	}
	return Result;
}

bool WoTRuntimeLocalizationWhitespace(uint8 Byte)
{
	return Byte == ' ' || Byte == '\t' || Byte == '\v' || Byte == '\f';
}

bool WoTRuntimeLocalizationLineBreak(uint8 Byte)
{
	return Byte == '\r' || Byte == '\n' || Byte == 0x0b || Byte == 0x0c
		|| Byte == 0x1c || Byte == 0x1d || Byte == 0x1e || Byte == 0x85;
}

bool WoTRuntimeValidateLocalization(
	TConstArrayView<uint8> Bytes, uint32& OutSections, uint32& OutEntries)
{
	if (Bytes.IsEmpty())
	{
		return false;
	}
	for (uint8 Byte : Bytes)
	{
		if (Byte == 0 || Byte == 0x81 || Byte == 0x8d || Byte == 0x8f
			|| Byte == 0x90 || Byte == 0x9d)
		{
			return false;
		}
	}
	bool bHasSection = false;
	int32 Start = 0;
	for (int32 Offset = 0; Offset <= Bytes.Num(); ++Offset)
	{
		const bool bEnd = Offset == Bytes.Num();
		if (!bEnd && !WoTRuntimeLocalizationLineBreak(Bytes[Offset]))
		{
			continue;
		}
		int32 First = Start;
		int32 Last = Offset;
		while (First < Last && WoTRuntimeLocalizationWhitespace(Bytes[First]))
		{
			++First;
		}
		while (Last > First
			&& WoTRuntimeLocalizationWhitespace(Bytes[Last - 1]))
		{
			--Last;
		}
		if (First < Last && Bytes[First] != ';' && Bytes[First] != '#')
		{
			if (Bytes[First] == '[' && Bytes[Last - 1] == ']')
			{
				int32 SectionFirst = First + 1;
				int32 SectionLast = Last - 1;
				while (SectionFirst < SectionLast
					&& WoTRuntimeLocalizationWhitespace(Bytes[SectionFirst]))
				{
					++SectionFirst;
				}
				while (SectionLast > SectionFirst
					&& WoTRuntimeLocalizationWhitespace(
						Bytes[SectionLast - 1]))
				{
					--SectionLast;
				}
				if (SectionFirst == SectionLast
					|| ++OutSections > WoTRuntimeTableMaximumRows)
				{
					return false;
				}
				bHasSection = true;
			}
			else
			{
				int32 Equals = INDEX_NONE;
				for (int32 Index = Start; Index < Offset; ++Index)
				{
					if (Bytes[Index] == '=')
					{
						Equals = Index;
						break;
					}
				}
				int32 KeyFirst = Start;
				int32 KeyLast = Equals;
				if (Equals == INDEX_NONE || !bHasSection)
				{
					return false;
				}
				while (KeyFirst < KeyLast
					&& WoTRuntimeLocalizationWhitespace(Bytes[KeyFirst]))
				{
					++KeyFirst;
				}
				while (KeyLast > KeyFirst
					&& WoTRuntimeLocalizationWhitespace(Bytes[KeyLast - 1]))
				{
					--KeyLast;
				}
				if (KeyFirst == KeyLast
					|| ++OutEntries > WoTRuntimeTableMaximumRows)
				{
					return false;
				}
			}
		}
		if (!bEnd && Bytes[Offset] == '\r'
			&& Offset + 1 < Bytes.Num() && Bytes[Offset + 1] == '\n')
		{
			++Offset;
		}
		Start = Offset + 1;
	}
	return OutSections > 0;
}
}

FWoTBootstrapStatus
FWoTRuntimeLocalizationDefaults::LoadLocalizationFirstUse(
	const FWoTNeutralResourceId& Resource)
{
	if (Localization.IsSet()
		|| Resource.Kind != EWoTNeutralResourceKind::Localization)
	{
		return WoTRuntimeTableError(
			TEXT("localization resource was reused or has the wrong kind"));
	}
	TWoTBootstrapResult<FWoTRuntimeVerifiedObject> Read =
		FWoTRuntimeRegistry::Get().ReadObject(
			Resource, 64ull * 1024ull * 1024ull,
			TEXT("windows-1252-ini-bytes-v1"), TEXT("localization"));
	if (Read.IsError())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(Read).GetError());
	}
	FWoTRuntimeVerifiedObject Object =
		MoveTemp(Read).GetValue();
	const FWoTCanonicalJsonValue& Header = Object.Header();
	static const TCHAR* HeaderFields[] = {
		TEXT("encoding"), TEXT("entry_count"), TEXT("fact_id"),
		TEXT("fallback_language"), TEXT("importer"),
		TEXT("importer_version"), TEXT("kind"),
		TEXT("payload_byte_length"), TEXT("section_count"), TEXT("source")
	};
	uint32 ExpectedEntries = 0;
	uint32 ExpectedSections = 0;
	uint32 ImporterVersion = 0;
	FString FactId;
	FString Fallback;
	FString Importer;
	uint32 ActualEntries = 0;
	uint32 ActualSections = 0;
	if (RequireExactMembers(
			Header, MakeArrayView(HeaderFields),
			TEXT("localization header")).IsSet()
		|| !WoTRuntimeTableReadU32(
			Header, TEXT("entry_count"), ExpectedEntries)
		|| !WoTRuntimeTableReadU32(
			Header, TEXT("section_count"), ExpectedSections)
		|| !WoTRuntimeTableReadU32(
			Header, TEXT("importer_version"), ImporterVersion)
		|| !WoTRuntimeTableReadString(Header, TEXT("fact_id"), FactId)
		|| !WoTRuntimeTableReadString(
			Header, TEXT("fallback_language"), Fallback)
		|| !WoTRuntimeTableReadString(Header, TEXT("importer"), Importer)
		|| FactId.IsEmpty() || Fallback != TEXT("en")
		|| Importer != TEXT("localization-v1") || ImporterVersion != 1
		|| !WoTRuntimeTableValidateSource(Header)
		|| !WoTRuntimeValidateLocalization(
			Object.Payload(), ActualSections, ActualEntries)
		|| ActualEntries != ExpectedEntries
		|| ActualSections != ExpectedSections)
	{
		return WoTRuntimeTableError(
			TEXT("localization header or INI payload is malformed"));
	}
	Localization.Emplace(MoveTemp(Object));
	return FWoTBootstrapStatus::Success();
}

FWoTBootstrapStatus
FWoTRuntimeLocalizationDefaults::LoadClassDefaultsFirstUse(
	const FWoTNeutralResourceId& Resource)
{
	if (ClassDefaults.IsSet()
		|| Resource.Kind != EWoTNeutralResourceKind::ClassDefaults)
	{
		return WoTRuntimeTableError(
			TEXT("class-default resource was reused or has the wrong kind"));
	}
	TWoTBootstrapResult<FWoTRuntimeVerifiedObject> Read =
		FWoTRuntimeRegistry::Get().ReadObject(
			Resource, 256ull * 1024ull * 1024ull,
			TEXT("ue1-class-defaults-v2"), TEXT("class_default"));
	if (Read.IsError())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(Read).GetError());
	}
	return LoadVerifiedClassDefaults(MoveTemp(Read).GetValue());
}

#if WITH_AUTOMATION_TESTS
FWoTBootstrapStatus
FWoTRuntimeLocalizationDefaults::LoadClassDefaultsFirstUseObjectForTesting(
	FWoTRuntimeVerifiedObject&& Object)
{
	if (ClassDefaults.IsSet())
	{
		return WoTRuntimeTableError(
			TEXT("class-default resource was reused"));
	}
	return LoadVerifiedClassDefaults(MoveTemp(Object));
}
#endif

FWoTBootstrapStatus
FWoTRuntimeLocalizationDefaults::LoadVerifiedClassDefaults(
	FWoTRuntimeVerifiedObject&& Object)
{
	const FWoTCanonicalJsonValue& Header = Object.Header();
	static const TCHAR* HeaderFields[] = {
		TEXT("class_count"), TEXT("default_count"), TEXT("encoding"),
		TEXT("fact_id"), TEXT("importer"), TEXT("importer_version"),
		TEXT("kind"), TEXT("payload_byte_length"), TEXT("reference_count"),
		TEXT("source")
	};
	uint32 ExpectedClasses = 0;
	uint32 ExpectedDefaults = 0;
	uint32 ExpectedReferences = 0;
	uint32 ImporterVersion = 0;
	FString FactId;
	FString Importer;
	if (RequireExactMembers(
			Header, MakeArrayView(HeaderFields),
			TEXT("class-default header")).IsSet()
		|| !WoTRuntimeTableReadU32(
			Header, TEXT("class_count"), ExpectedClasses)
		|| !WoTRuntimeTableReadU32(
			Header, TEXT("default_count"), ExpectedDefaults)
		|| !WoTRuntimeTableReadU32(
			Header, TEXT("reference_count"), ExpectedReferences)
		|| !WoTRuntimeTableReadU32(
			Header, TEXT("importer_version"), ImporterVersion)
		|| !WoTRuntimeTableReadString(Header, TEXT("fact_id"), FactId)
		|| !WoTRuntimeTableReadString(Header, TEXT("importer"), Importer)
		|| ExpectedClasses > WoTRuntimeTableMaximumRows
		|| FactId.IsEmpty() || Importer != TEXT("class-default-v2")
		|| ImporterVersion != 2 || !WoTRuntimeTableValidateSource(Header))
	{
		return WoTRuntimeTableError(
			TEXT("class-default header is malformed"));
	}
	FWoTRuntimeTableCursor Cursor(Object.Payload());
	uint16 Version = 0;
	uint16 Flags = 0;
	uint32 ClassCount = 0;
	const TConstArrayView<uint8> Payload = Object.Payload();
	if (Payload.Num() < 16
		|| FMemory::Memcmp(Payload.GetData(), "WOTDEF3", 8) != 0
		|| !Cursor.Skip(8) || !Cursor.ReadU16(Version)
		|| !Cursor.ReadU16(Flags) || !Cursor.ReadU32(ClassCount)
		|| Version != 3 || Flags != 0
		|| ClassCount != ExpectedClasses)
	{
		return WoTRuntimeTableError(
			TEXT("class-default payload framing is malformed"));
	}
	uint32 ActualDefaults = 0;
	uint32 ActualReferences = 0;
	TSet<FString> Identities;
	for (uint32 Index = 0; Index < ClassCount; ++Index)
	{
		FString Identity;
		FString Superclass;
		uint32 ExportIndex = 0;
		uint8 Placeholder = 0;
		TArray<FWoTRuntimeWorldProperty> Properties;
		FString PropertyError;
		int32 PropertyOffset = 0;
		if (!Cursor.ReadString(Identity) || Identity.IsEmpty()
			|| !Cursor.ReadU32(ExportIndex) || ExportIndex == 0
			|| !Cursor.ReadString(Superclass)
			|| !Cursor.ReadU8(Placeholder) || Placeholder > 1
			|| Identities.Contains(Identity))
		{
			return WoTRuntimeTableError(
				TEXT("class-default record is malformed"));
		}
		PropertyOffset = Cursor.Tell();
		if (!WoTRuntimeWorldDecodeProperties(Cursor.View(), PropertyOffset,
				Properties, PropertyError, nullptr, true,
				EWoTStructMemberPolicy::ReflectedGeneric)
			|| !Cursor.SetOffset(PropertyOffset))
		{
			return WoTRuntimeTableError(
				TEXT("class-default record is malformed"));
		}
		ActualDefaults += static_cast<uint32>(Properties.Num());
		ActualReferences += WoTRuntimeTableReferenceCount(Properties);
		Identities.Add(Identity);
	}
	if (!Cursor.AtEnd() || ActualDefaults != ExpectedDefaults
		|| ActualReferences != ExpectedReferences)
	{
		return WoTRuntimeTableError(
			TEXT("class-default counts or payload tail mismatched"));
	}
	ClassDefaults.Emplace(MoveTemp(Object));
	return FWoTBootstrapStatus::Success();
}
