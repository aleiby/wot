#include "WoTRuntimeProceduralResource.h"

#include "../WoTFractalData.h"
#include "Containers/StringConv.h"

namespace
{
constexpr uint32 WoTRuntimeProceduralMaximumRows = 1000000;
constexpr uint32 WoTRuntimeProceduralMaxDrops = 256;
constexpr uint16 WoTRuntimeProceduralPayloadVersion = 2;

FWoTBootstrapStatus WoTRuntimeProceduralError(const TCHAR* Message)
{
	return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
		EWoTBootstrapError::CorruptInstallation, Message));
}

class FWoTRuntimeProceduralCursor final
{
public:
	explicit FWoTRuntimeProceduralCursor(TConstArrayView<uint8> InBytes)
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

	bool ReadFloat(float& Out)
	{
		uint32 Bits = 0;
		if (!ReadU32(Bits)) return false;
		float Value = 0.0f;
		FMemory::Memcpy(&Value, &Bits, sizeof(float));
		if (!FMath::IsFinite(Value)) return false;
		Out = Value;
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

	bool ReadBytes(uint32 Count, TArray<uint8>& Out)
	{
		if (static_cast<uint64>(Offset) + Count
			> static_cast<uint64>(Bytes.Num())
			|| Count > static_cast<uint32>(MAX_int32))
		{
			return false;
		}
		Out.Append(Bytes.GetData() + Offset, static_cast<int32>(Count));
		Offset += static_cast<int32>(Count);
		return true;
	}

	bool AtEnd() const { return Offset == Bytes.Num(); }

private:
	TConstArrayView<uint8> Bytes;
	int32 Offset = 0;
};

bool WoTRuntimeProceduralReadU32(
	const FWoTCanonicalJsonValue& Header, const TCHAR* Field, uint32& Out)
{
	return !ReadUnsigned32Field(
		Header, Field, TEXT("procedural header"), Out).IsSet();
}

bool WoTRuntimeProceduralReadString(
	const FWoTCanonicalJsonValue& Header, const TCHAR* Field, FString& Out)
{
	return !ReadStringField(
		Header, Field, TEXT("procedural header"), Out).IsSet();
}

bool WoTRuntimeProceduralValidateSource(
	const FWoTCanonicalJsonValue& Header, FString& OutExport)
{
	const FWoTCanonicalJsonValue* Source = nullptr;
	static const TCHAR* Fields[] = {
		TEXT("export"), TEXT("package"), TEXT("sha256")
	};
	FString Package;
	FString Hash;
	return !ReadObjectField(
			Header, TEXT("source"), TEXT("procedural header"), Source).IsSet()
		&& Source != nullptr
		&& !RequireExactMembers(
			*Source, MakeArrayView(Fields), TEXT("procedural source")).IsSet()
		&& WoTRuntimeProceduralReadString(*Source, TEXT("export"), OutExport)
		&& WoTRuntimeProceduralReadString(*Source, TEXT("package"), Package)
		&& WoTRuntimeProceduralReadString(*Source, TEXT("sha256"), Hash)
		&& !OutExport.IsEmpty() && !Package.IsEmpty() && Hash.Len() == 64;
}

bool WoTRuntimeProceduralReadProperties(
	FWoTRuntimeProceduralCursor& Cursor, uint32 ExpectedCount)
{
	uint32 Count = 0;
	if (!Cursor.ReadU32(Count) || Count != ExpectedCount
		|| Count > WoTRuntimeProceduralMaximumRows)
	{
		return false;
	}
	FString PreviousFolded;
	FString PreviousName;
	uint32 PreviousArray = 0;
	for (uint32 Index = 0; Index < Count; ++Index)
	{
		FString Name;
		FString StructName;
		FString NameValue;
		FString ObjectIdentity;
		FString ObjectClassIdentity;
		uint8 Type = 0;
		uint32 ArrayIndex = 0;
		uint32 ValueBytes = 0;
		if (!Cursor.ReadString(Name) || Name.IsEmpty()
			|| !Cursor.ReadU8(Type) || !Cursor.ReadU32(ArrayIndex)
			|| !Cursor.ReadString(StructName)
			|| !Cursor.ReadString(NameValue)
			|| !Cursor.ReadString(ObjectIdentity)
			|| !Cursor.ReadString(ObjectClassIdentity)
			|| !Cursor.ReadU32(ValueBytes)
			|| (Type != 1 && Type != 2 && Type != 3 && Type != 4
				&& Type != 5 && Type != 6 && Type != 10 && Type != 13)
			|| ((Type == 5 || Type == 6) && ValueBytes != 0)
			|| (Type == 10 && StructName.IsEmpty())
			|| (Type != 10 && !StructName.IsEmpty())
			|| (Type != 6 && !NameValue.IsEmpty())
			|| (Type != 5
				&& (!ObjectIdentity.IsEmpty()
					|| !ObjectClassIdentity.IsEmpty()))
			|| (!ObjectIdentity.IsEmpty() && ObjectClassIdentity.IsEmpty())
			|| !Cursor.Skip(ValueBytes))
		{
			return false;
		}
		FString Folded = Name;
		Folded.ToLowerInline();
		if (Index > 0
			&& (Folded < PreviousFolded
				|| (Folded == PreviousFolded
					&& (ArrayIndex < PreviousArray
						|| (ArrayIndex == PreviousArray
							&& Name <= PreviousName)))))
		{
			return false;
		}
		PreviousFolded = MoveTemp(Folded);
		PreviousName = MoveTemp(Name);
		PreviousArray = ArrayIndex;
	}
	return true;
}

bool WoTRuntimeProceduralReadInput(FWoTRuntimeProceduralCursor& Cursor,
	FWoTRuntimeProceduralInput& Out)
{
	if (!Cursor.ReadU8(Out.Kind) || Out.Kind > 2
		|| !Cursor.ReadString(Out.Identity)
		|| !Cursor.ReadU32(Out.Width) || !Cursor.ReadU32(Out.Height))
	{
		return false;
	}
	if (Out.Kind == 0)
	{
		return Out.Identity.IsEmpty() && Out.Width == 0 && Out.Height == 0;
	}
	return !Out.Identity.IsEmpty()
		&& Out.Width > 0 && Out.Height > 0
		&& Out.Width <= 16384 && Out.Height <= 16384
		&& (Out.Width & (Out.Width - 1)) == 0
		&& (Out.Height & (Out.Height - 1)) == 0;
}

bool WoTRuntimeProceduralInputClassMatches(
	const FWoTRuntimeProceduralInput& Input)
{
	if (Input.Kind == 0) return Input.ClassName.IsEmpty();
	if (Input.Kind == 1) return Input.ClassName == TEXT("Texture");
	return Input.ClassName == TEXT("FireTexture")
		|| Input.ClassName == TEXT("WaterTexture")
		|| Input.ClassName == TEXT("WaveTexture")
		|| Input.ClassName == TEXT("WetTexture")
		|| Input.ClassName == TEXT("IceTexture");
}

FString WoTRuntimeProceduralStripRegistrySuffix(const FString& Component)
{
	int32 Hash = INDEX_NONE;
	if (Component.FindChar(TEXT('#'), Hash) && Hash > 0)
	{
		const FString Suffix = Component.Mid(Hash);
		if (Suffix.StartsWith(TEXT("#export-")) || Suffix.StartsWith(TEXT("#import-")))
			return Component.Left(Hash);
	}
	return Component;
}
}

FString FWoTRuntimeProceduralResource::PackageOfSourceExport(
	FStringView CanonicalIdentity)
{
	const FString Identity(CanonicalIdentity);
	int32 Dot = INDEX_NONE;
	return Identity.FindChar(TEXT('.'), Dot) ? Identity.Left(Dot) : Identity;
}

FString FWoTRuntimeProceduralResource::NameOfSourceExport(
	FStringView CanonicalIdentity)
{
	const FString Identity(CanonicalIdentity);
	int32 Dot = INDEX_NONE;
	const FString Leaf = Identity.FindLastChar(TEXT('.'), Dot)
		? Identity.Mid(Dot + 1) : Identity;
	return WoTRuntimeProceduralStripRegistrySuffix(Leaf);
}

FName FWoTRuntimeProceduralResource::IdentityKeyForSourceExport(
	FStringView CanonicalIdentity)
{
	FString Key = PackageOfSourceExport(CanonicalIdentity) + TEXT("__")
		+ NameOfSourceExport(CanonicalIdentity);
	// The importers' asset-name spelling folds '-' to '_' (import_map.py).
	Key.ReplaceCharInline(TEXT('-'), TEXT('_'));
	return FName(*Key);
}

FWoTBootstrapStatus FWoTRuntimeProceduralResource::LoadFirstUse(
	const FWoTNeutralResourceId& Resource)
{
	if (Object.IsSet() || Resource.Kind != EWoTNeutralResourceKind::Procedural)
	{
		return WoTRuntimeProceduralError(
			TEXT("procedural resource was reused or has the wrong kind"));
	}
	TWoTBootstrapResult<FWoTRuntimeVerifiedObject> Read =
		FWoTRuntimeRegistry::Get().ReadObject(
			Resource, 256ull * 1024ull * 1024ull,
			TEXT("ue1-fractal-inputs-v2"), TEXT("procedural"));
	if (Read.IsError())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(Read).GetError());
	}
	FWoTRuntimeVerifiedObject Verified =
		MoveTemp(Read).GetValue();
	const FWoTCanonicalJsonValue& Header = Verified.Header();
	static const TCHAR* HeaderFields[] = {
		TEXT("class_name"), TEXT("dimensions"), TEXT("drop_count"),
		TEXT("encoding"), TEXT("fact_id"), TEXT("glass_class"),
		TEXT("glass_texture"), TEXT("importer"), TEXT("importer_version"),
		TEXT("kind"), TEXT("masked"), TEXT("mip_count"),
		TEXT("palette_identity"), TEXT("payload_byte_length"),
		TEXT("property_count"), TEXT("random_scope"), TEXT("random_seed"),
		TEXT("references"), TEXT("source"), TEXT("source_class"),
		TEXT("source_texture"), TEXT("state_count"), TEXT("state_kind")
	};
	static const TCHAR* DimensionFields[] = {
		TEXT("height"), TEXT("width")
	};
	uint32 MipCount = 0;
	uint32 StateCount = 0;
	uint32 PropertyCount = 0;
	uint32 RandomSeed = 0;
	uint32 ImporterVersion = 0;
	uint32 DropCount = 0;
	uint32 Masked = 0;
	FString ClassName;
	FString FactId;
	FString Importer;
	FString RandomScope;
	FString StateKind;
	FString PaletteIdentity;
	FString SourceExport;
	FWoTRuntimeProceduralInput HeaderSource;
	FWoTRuntimeProceduralInput HeaderGlass;
	const FWoTCanonicalJsonValue* Dimensions =
		Header.FindMember(TEXT("dimensions"));
	const FWoTCanonicalJsonValue* References =
		Header.FindMember(TEXT("references"));
	if (RequireExactMembers(
			Header, MakeArrayView(HeaderFields),
			TEXT("procedural header")).IsSet()
		|| !WoTRuntimeProceduralReadU32(
			Header, TEXT("mip_count"), MipCount)
		|| !WoTRuntimeProceduralReadU32(
			Header, TEXT("state_count"), StateCount)
		|| !WoTRuntimeProceduralReadU32(
			Header, TEXT("property_count"), PropertyCount)
		|| !WoTRuntimeProceduralReadU32(
			Header, TEXT("random_seed"), RandomSeed)
		|| !WoTRuntimeProceduralReadU32(
			Header, TEXT("importer_version"), ImporterVersion)
		|| !WoTRuntimeProceduralReadU32(
			Header, TEXT("drop_count"), DropCount)
		|| !WoTRuntimeProceduralReadU32(
			Header, TEXT("masked"), Masked)
		|| !WoTRuntimeProceduralReadString(
			Header, TEXT("class_name"), ClassName)
		|| !WoTRuntimeProceduralReadString(
			Header, TEXT("fact_id"), FactId)
		|| !WoTRuntimeProceduralReadString(
			Header, TEXT("importer"), Importer)
		|| !WoTRuntimeProceduralReadString(
			Header, TEXT("random_scope"), RandomScope)
		|| !WoTRuntimeProceduralReadString(
			Header, TEXT("state_kind"), StateKind)
		|| !WoTRuntimeProceduralReadString(
			Header, TEXT("palette_identity"), PaletteIdentity)
		|| !WoTRuntimeProceduralReadString(
			Header, TEXT("source_texture"), HeaderSource.Identity)
		|| !WoTRuntimeProceduralReadString(
			Header, TEXT("source_class"), HeaderSource.ClassName)
		|| !WoTRuntimeProceduralReadString(
			Header, TEXT("glass_texture"), HeaderGlass.Identity)
		|| !WoTRuntimeProceduralReadString(
			Header, TEXT("glass_class"), HeaderGlass.ClassName)
		|| (ClassName != TEXT("FireTexture")
			&& ClassName != TEXT("WaterTexture")
			&& ClassName != TEXT("WaveTexture")
			&& ClassName != TEXT("WetTexture")
			&& ClassName != TEXT("IceTexture"))
		|| MipCount == 0 || MipCount > 32
		|| StateCount > WoTRuntimeProceduralMaximumRows
		|| PropertyCount > WoTRuntimeProceduralMaximumRows
		|| DropCount > WoTRuntimeProceduralMaxDrops
		|| Masked > 1 || PaletteIdentity.IsEmpty()
		|| (Masked == 1 && ClassName != TEXT("IceTexture"))
		|| FactId.IsEmpty() || Importer != TEXT("procedural-v1")
		|| ImporterVersion != 2
		|| RandomScope != TEXT("process-global-canonical-order")
		|| Dimensions == nullptr || !Dimensions->IsArray()
		|| Dimensions->Num() != static_cast<int32>(MipCount)
		|| References == nullptr || !References->IsArray()
		|| References->Num() > static_cast<int32>(
			WoTRuntimeProceduralMaximumRows)
		|| !WoTRuntimeProceduralValidateSource(Header, SourceExport)
		|| HeaderSource.Identity.IsEmpty() != HeaderSource.ClassName.IsEmpty()
		|| HeaderGlass.Identity.IsEmpty() != HeaderGlass.ClassName.IsEmpty())
	{
		return WoTRuntimeProceduralError(
			TEXT("procedural canonical header is malformed"));
	}

	TArray<FIntPoint> HeaderDimensions;
	HeaderDimensions.Reserve(Dimensions->Num());
	for (int32 Index = 0; Index < Dimensions->Num(); ++Index)
	{
		const FWoTCanonicalJsonValue& Dimension =
			Dimensions->GetElement(Index);
		uint32 Width = 0;
		uint32 Height = 0;
		if (RequireExactMembers(
				Dimension, MakeArrayView(DimensionFields),
				TEXT("procedural dimension")).IsSet()
			|| !WoTRuntimeProceduralReadU32(
				Dimension, TEXT("width"), Width)
			|| !WoTRuntimeProceduralReadU32(
				Dimension, TEXT("height"), Height)
			|| Width == 0 || Height == 0 || Width > 16384 || Height > 16384
			|| (Width & (Width - 1)) != 0 || (Height & (Height - 1)) != 0)
		{
			return WoTRuntimeProceduralError(
				TEXT("procedural dimension is malformed"));
		}
		HeaderDimensions.Emplace(
			static_cast<int32>(Width), static_cast<int32>(Height));
	}
	FString PreviousReference;
	FString PreviousReferenceFolded;
	bool bReferencesPalette = false;
	for (int32 Index = 0; Index < References->Num(); ++Index)
	{
		const FWoTCanonicalJsonValue& Reference =
			References->GetElement(Index);
		FString Folded = Reference.GetKind() == EWoTCanonicalJsonKind::String
			? Reference.GetString() : FString();
		Folded.ToLowerInline();
		if (Reference.GetKind() != EWoTCanonicalJsonKind::String
			|| Reference.GetString().IsEmpty()
			|| (Index > 0
				&& (Folded < PreviousReferenceFolded
					|| (Folded == PreviousReferenceFolded
						&& Reference.GetString() <= PreviousReference))))
		{
			return WoTRuntimeProceduralError(
				TEXT("procedural reference list is non-canonical"));
		}
		bReferencesPalette = bReferencesPalette
			|| Reference.GetString() == PaletteIdentity;
		PreviousReference = Reference.GetString();
		PreviousReferenceFolded = MoveTemp(Folded);
	}
	if (!bReferencesPalette)
	{
		return WoTRuntimeProceduralError(
			TEXT("procedural palette identity is outside its reference list"));
	}

	const TConstArrayView<uint8> Payload = Verified.Payload();
	FWoTRuntimeProceduralCursor Cursor(Payload);
	uint16 Version = 0;
	uint16 Flags = 0;
	uint32 PayloadMipCount = 0;
	uint32 PayloadStateCount = 0;
	uint32 PayloadRandomSeed = 0;
	if (Payload.Num() < 24
		|| FMemory::Memcmp(Payload.GetData(), "WOTPRC3", 8) != 0
		|| !Cursor.Skip(8) || !Cursor.ReadU16(Version)
		|| !Cursor.ReadU16(Flags) || !Cursor.ReadU32(PayloadMipCount)
		|| !Cursor.ReadU32(PayloadStateCount)
		|| !Cursor.ReadU32(PayloadRandomSeed)
		|| Version != WoTRuntimeProceduralPayloadVersion || Flags != 0
		|| PayloadMipCount != MipCount || PayloadStateCount != StateCount
		|| PayloadRandomSeed != RandomSeed
		|| !WoTRuntimeProceduralReadProperties(Cursor, PropertyCount))
	{
		return WoTRuntimeProceduralError(
			TEXT("procedural payload framing or properties are malformed"));
	}

	TArray<FWoTRuntimeProceduralMip> Mips;
	Mips.Reserve(static_cast<int32>(MipCount));
	for (uint32 Index = 0; Index < MipCount; ++Index)
	{
		FWoTRuntimeProceduralMip& Mip = Mips.AddDefaulted_GetRef();
		uint32 DataBytes = 0;
		uint64 ExpectedBytes = 0;
		if (!Cursor.ReadU32(Mip.Width) || !Cursor.ReadU32(Mip.Height)
			|| !Cursor.ReadU32(DataBytes))
		{
			return WoTRuntimeProceduralError(
				TEXT("procedural mip payload is malformed"));
		}
		ExpectedBytes = static_cast<uint64>(Mip.Width)
			* static_cast<uint64>(Mip.Height);
		if (Mip.Width
				!= static_cast<uint32>(
					HeaderDimensions[static_cast<int32>(Index)].X)
			|| Mip.Height
				!= static_cast<uint32>(
					HeaderDimensions[static_cast<int32>(Index)].Y)
			|| ExpectedBytes != DataBytes
			|| !Cursor.ReadBytes(DataBytes, Mip.Indices))
		{
			return WoTRuntimeProceduralError(
				TEXT("procedural mip payload is malformed"));
		}
	}
	TArray<TArray<uint8>> States;
	States.Reserve(static_cast<int32>(StateCount));
	for (uint32 Index = 0; Index < StateCount; ++Index)
	{
		TArray<uint8>& State = States.AddDefaulted_GetRef();
		if (!Cursor.ReadBytes(8, State))
		{
			return WoTRuntimeProceduralError(
				TEXT("procedural state payload is truncated"));
		}
	}
	// ---- v2 tail: palette, parameters, drops, inputs ----
	TArray<uint8> Palette;
	FWoTRuntimeProceduralParameters Parameters;
	uint8 MaskedByte = 0, RisingByte = 0, MoveIceByte = 0, ReservedByte = 0;
	uint32 PrimeCount = 0, SparksLimit = 0, PayloadDropCount = 0;
	if (!Cursor.ReadBytes(1024, Palette)
		|| !Cursor.ReadU8(MaskedByte) || !Cursor.ReadU8(RisingByte)
		|| !Cursor.ReadU8(MoveIceByte) || !Cursor.ReadU8(ReservedByte)
		|| MaskedByte > 1 || RisingByte > 1 || MoveIceByte > 1 || ReservedByte != 0
		|| MaskedByte != Masked
		|| !Cursor.ReadFloat(Parameters.MinFrameRate)
		|| !Cursor.ReadFloat(Parameters.MaxFrameRate)
		|| !Cursor.ReadU32(PrimeCount)
		|| !Cursor.ReadU8(Parameters.WaveAmp)
		|| !Cursor.ReadU8(Parameters.BumpMapLight)
		|| !Cursor.ReadU8(Parameters.BumpMapAngle)
		|| !Cursor.ReadU8(Parameters.PhongRange)
		|| !Cursor.ReadU8(Parameters.PhongSize)
		|| !Cursor.ReadU8(Parameters.RenderHeat)
		|| !Cursor.ReadU8(Parameters.PanningStyle)
		|| !Cursor.ReadU8(Parameters.TimeMethod)
		|| !Cursor.ReadU8(Parameters.HorizPanSpeed)
		|| !Cursor.ReadU8(Parameters.VertPanSpeed)
		|| !Cursor.ReadU8(Parameters.Frequency)
		|| !Cursor.ReadU8(Parameters.Amplitude)
		|| !Cursor.ReadU32(SparksLimit)
		|| !Cursor.ReadFloat(Parameters.MasterCount)
		|| !Cursor.ReadFloat(Parameters.UDisplace)
		|| !Cursor.ReadFloat(Parameters.VDisplace)
		|| !Cursor.ReadFloat(Parameters.UPosition)
		|| !Cursor.ReadFloat(Parameters.VPosition)
		|| !Cursor.ReadU32(PayloadDropCount)
		|| PayloadDropCount != DropCount
		|| PrimeCount > 255)
	{
		return WoTRuntimeProceduralError(
			TEXT("procedural palette or parameter block is malformed"));
	}
	Parameters.bMasked = MaskedByte != 0;
	Parameters.bRising = RisingByte != 0;
	Parameters.bMoveIce = MoveIceByte != 0;
	Parameters.PrimeCount = static_cast<int32>(PrimeCount);
	Parameters.SparksLimit = static_cast<int32>(SparksLimit);
	TArray<TArray<uint8>> Drops;
	Drops.Reserve(static_cast<int32>(DropCount));
	for (uint32 Index = 0; Index < DropCount; ++Index)
	{
		TArray<uint8>& Drop = Drops.AddDefaulted_GetRef();
		if (!Cursor.ReadBytes(8, Drop))
		{
			return WoTRuntimeProceduralError(
				TEXT("procedural drop table is truncated"));
		}
	}
	FWoTRuntimeProceduralInput SourceInput;
	FWoTRuntimeProceduralInput GlassInput;
	if (!WoTRuntimeProceduralReadInput(Cursor, SourceInput)
		|| !WoTRuntimeProceduralReadInput(Cursor, GlassInput)
		|| SourceInput.Identity != HeaderSource.Identity
		|| GlassInput.Identity != HeaderGlass.Identity)
	{
		return WoTRuntimeProceduralError(
			TEXT("procedural input records are malformed"));
	}
	SourceInput.ClassName = HeaderSource.ClassName;
	GlassInput.ClassName = HeaderGlass.ClassName;
	if (!WoTRuntimeProceduralInputClassMatches(SourceInput)
		|| !WoTRuntimeProceduralInputClassMatches(GlassInput)
		|| (ClassName == TEXT("IceTexture")
			&& (SourceInput.Kind == 0 || GlassInput.Kind == 0))
		|| (ClassName == TEXT("WetTexture") && SourceInput.Kind == 0)
		|| ((ClassName == TEXT("FireTexture") || ClassName == TEXT("WaveTexture")
				|| ClassName == TEXT("WaterTexture"))
			&& (SourceInput.Kind != 0 || GlassInput.Kind != 0))
		|| (ClassName != TEXT("IceTexture") && GlassInput.Kind != 0))
	{
		return WoTRuntimeProceduralError(
			TEXT("procedural inputs disagree with the fractal class"));
	}
	if (!Cursor.AtEnd()
		|| (ClassName == TEXT("FireTexture") && StateKind != TEXT("sparks"))
		|| ((ClassName == TEXT("WaterTexture")
				|| ClassName == TEXT("WetTexture")
				|| ClassName == TEXT("WaveTexture"))
			&& StateKind != TEXT("drops"))
		|| (ClassName == TEXT("IceTexture") && StateKind != TEXT("none")))
	{
		return WoTRuntimeProceduralError(
			TEXT("procedural state kind or payload tail mismatched"));
	}
	ParsedClassName = MoveTemp(ClassName);
	ParsedSourceExport = MoveTemp(SourceExport);
	ParsedLogicalId = Resource.LogicalId;
	ParsedMips = MoveTemp(Mips);
	ParsedStates = MoveTemp(States);
	ParsedPalette = MoveTemp(Palette);
	ParsedParameters = Parameters;
	ParsedDrops = MoveTemp(Drops);
	ParsedSource = MoveTemp(SourceInput);
	ParsedGlass = MoveTemp(GlassInput);
	ParsedPropertyCount = PropertyCount;
	ParsedRandomSeed = RandomSeed;
	Object.Emplace(MoveTemp(Verified));
	return FWoTBootstrapStatus::Success();
}

bool FWoTRuntimeProceduralResource::BuildRecord(
	FWoTFractalTextureRecord& Out) const
{
	if (!Object.IsSet() || ParsedMips.IsEmpty() || ParsedPalette.Num() != 1024)
	{
		return false;
	}
	EWoTFractalClass FractalClass = EWoTFractalClass::Unknown;
	if (ParsedClassName == TEXT("FireTexture")) FractalClass = EWoTFractalClass::Fire;
	else if (ParsedClassName == TEXT("WaterTexture")) FractalClass = EWoTFractalClass::Water;
	else if (ParsedClassName == TEXT("WaveTexture")) FractalClass = EWoTFractalClass::Wave;
	else if (ParsedClassName == TEXT("WetTexture")) FractalClass = EWoTFractalClass::Wet;
	else if (ParsedClassName == TEXT("IceTexture")) FractalClass = EWoTFractalClass::Ice;
	else return false;
	const FWoTRuntimeProceduralMip& Mip = ParsedMips[0];
	if (Mip.Width > static_cast<uint32>(MAX_int32)
		|| Mip.Height > static_cast<uint32>(MAX_int32)
		|| static_cast<uint64>(Mip.Width) * Mip.Height
			!= static_cast<uint64>(Mip.Indices.Num()))
	{
		return false;
	}
	FWoTFractalTextureRecord Record;
	Record.PackageName = FName(*PackageOfSourceExport(ParsedSourceExport));
	Record.TextureName = FName(*NameOfSourceExport(ParsedSourceExport));
	Record.ObjectPath = ParsedSourceExport;
	if (ParsedLogicalId.StartsWith(TEXT("cache/procedural/"), ESearchCase::CaseSensitive))
		Record.CacheLogicalBase = ParsedLogicalId.Mid(17);
	Record.FractalClass = FractalClass;
	Record.Width = static_cast<int32>(Mip.Width);
	Record.Height = static_cast<int32>(Mip.Height);
	Record.MinFrameRate = ParsedParameters.MinFrameRate;
	Record.MaxFrameRate = ParsedParameters.MaxFrameRate;
	Record.PrimeCount = ParsedParameters.PrimeCount;
	Record.WaveAmp = ParsedParameters.WaveAmp;
	Record.BumpMapLight = ParsedParameters.BumpMapLight;
	Record.BumpMapAngle = ParsedParameters.BumpMapAngle;
	Record.PhongRange = ParsedParameters.PhongRange;
	Record.PhongSize = ParsedParameters.PhongSize;
	Record.RenderHeat = ParsedParameters.RenderHeat;
	Record.bRising = ParsedParameters.bRising;
	Record.SparksLimit = ParsedParameters.SparksLimit;
	Record.SourceTextureRef = ParsedSource.Identity;
	Record.bSourceIsFractal = ParsedSource.Kind == 2;
	Record.SourceWidth = static_cast<int32>(ParsedSource.Width);
	Record.SourceHeight = static_cast<int32>(ParsedSource.Height);
	Record.GlassTextureRef = ParsedGlass.Identity;
	Record.bGlassIsFractal = ParsedGlass.Kind == 2;
	Record.GlassWidth = static_cast<int32>(ParsedGlass.Width);
	Record.GlassHeight = static_cast<int32>(ParsedGlass.Height);
	Record.PanningStyle = ParsedParameters.PanningStyle;
	Record.TimeMethod = ParsedParameters.TimeMethod;
	Record.HorizPanSpeed = ParsedParameters.HorizPanSpeed;
	Record.VertPanSpeed = ParsedParameters.VertPanSpeed;
	Record.Frequency = ParsedParameters.Frequency;
	Record.Amplitude = ParsedParameters.Amplitude;
	Record.bMoveIce = ParsedParameters.bMoveIce;
	Record.MasterCount = ParsedParameters.MasterCount;
	Record.UDisplace = ParsedParameters.UDisplace;
	Record.VDisplace = ParsedParameters.VDisplace;
	Record.UPosition = ParsedParameters.UPosition;
	Record.VPosition = ParsedParameters.VPosition;
	// bake_flipbooks._palette_rgba: RGB from the retail palette, alpha 255
	// except the masked palette slot zero.
	Record.PaletteRGBA.SetNumUninitialized(1024);
	for (int32 Index = 0; Index < 256; ++Index)
	{
		Record.PaletteRGBA[Index * 4 + 0] = ParsedPalette[Index * 4 + 0];
		Record.PaletteRGBA[Index * 4 + 1] = ParsedPalette[Index * 4 + 1];
		Record.PaletteRGBA[Index * 4 + 2] = ParsedPalette[Index * 4 + 2];
		Record.PaletteRGBA[Index * 4 + 3] =
			(ParsedParameters.bMasked && Index == 0) ? 0 : 255;
	}
	Record.InitialBitmap = Mip.Indices;
	if (FractalClass == EWoTFractalClass::Fire)
	{
		Record.Sparks.Reserve(ParsedStates.Num());
		for (const TArray<uint8>& State : ParsedStates)
		{
			if (State.Num() != 8) return false;
			FWoTFractalSpark& Spark = Record.Sparks.AddDefaulted_GetRef();
			Spark.Type = State[0]; Spark.Heat = State[1];
			Spark.X = State[2]; Spark.Y = State[3];
			Spark.ByteA = State[4]; Spark.ByteB = State[5];
			Spark.ByteC = State[6]; Spark.ByteD = State[7];
		}
	}
	Record.Drops.Reserve(ParsedDrops.Num());
	for (const TArray<uint8>& Raw : ParsedDrops)
	{
		if (Raw.Num() != 8) return false;
		FWoTFractalDrop& Drop = Record.Drops.AddDefaulted_GetRef();
		Drop.Type = Raw[0]; Drop.Depth = Raw[1];
		Drop.X = Raw[2]; Drop.Y = Raw[3];
		Drop.ByteA = Raw[4]; Drop.ByteB = Raw[5];
		Drop.ByteC = Raw[6]; Drop.ByteD = Raw[7];
	}
	Out = MoveTemp(Record);
	return true;
}
