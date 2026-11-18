#include "WoTRetailReader/RegistryLocalization.h"

#include "CanonicalObjectBuilder.h"
#include "RemainingPackage.h"
#include "WoTBootstrapCore/CanonicalJson.h"
#include "WoTBootstrapCore/Hash.h"

TWoTBootstrapResult<TArray<uint8>> WoTRemainingBuildPackageAggregateObject(
	TConstArrayView<uint8> SourceBytes, FStringView SourcePath,
	const FWoTRequiredFact& Fact, FWoTRemainingPackageResolver& Resolver);

namespace
{
constexpr uint64 WoTRegistryLocalizationMaxSourceBytes = 256ull * 1024 * 1024;
constexpr uint32 WoTRegistryLocalizationMaxRows = 1000000;
constexpr ANSICHAR WoTRegistryLocalizationObjectMagic[] = "WOTOBJ3";

FWoTBootstrapError WoTRegistryLocalizationError(EWoTBootstrapError Code,
	const TCHAR* Message, FString Path = FString())
{
	return MakeWoTBootstrapError(Code, Message, MoveTemp(Path));
}

void WoTRegistryLocalizationAppendU32(TArray<uint8>& Bytes, uint32 Value)
{
	Bytes.Add(static_cast<uint8>(Value));
	Bytes.Add(static_cast<uint8>(Value >> 8));
	Bytes.Add(static_cast<uint8>(Value >> 16));
	Bytes.Add(static_cast<uint8>(Value >> 24));
}

TWoTBootstrapResult<TArray<uint8>> WoTRegistryLocalizationFrame(
	const FWoTCanonicalJsonValue& Header, TConstArrayView<uint8> Payload)
{
	using FResult = TWoTBootstrapResult<TArray<uint8>>;
	TWoTBootstrapResult<TArray<uint8>> HeaderBytes = WriteCanonicalJson(Header);
	if (HeaderBytes.IsError()) return FResult::Error(MoveTemp(HeaderBytes).GetError());
	if (HeaderBytes.GetValue().IsEmpty() || HeaderBytes.GetValue().Num() > 1024 * 1024
		|| static_cast<uint64>(Payload.Num()) > WoTRegistryLocalizationMaxSourceBytes)
		return FResult::Error(WoTRegistryLocalizationError(EWoTBootstrapError::SizeLimit,
			TEXT("the registry/localization object exceeds its framing bound")));
	TArray<uint8> Object;
	Object.Reserve(12 + HeaderBytes.GetValue().Num() + Payload.Num());
	Object.Append(reinterpret_cast<const uint8*>(WoTRegistryLocalizationObjectMagic), 8);
	WoTRegistryLocalizationAppendU32(
		Object, static_cast<uint32>(HeaderBytes.GetValue().Num()));
	Object.Append(HeaderBytes.GetValue());
	Object.Append(Payload.GetData(), Payload.Num());
	return FResult::Success(MoveTemp(Object));
}

bool WoTRegistryLocalizationWhitespace(uint8 Byte)
{
	return Byte == ' ' || Byte == '\t' || Byte == '\v' || Byte == '\f';
}

bool WoTRegistryLocalizationLineBreak(uint8 Byte)
{
	return Byte == '\r' || Byte == '\n' || Byte == 0x0b || Byte == 0x0c
		|| Byte == 0x1c || Byte == 0x1d || Byte == 0x1e || Byte == 0x85;
}

TWoTBootstrapResult<TArray<uint8>> WoTRegistryLocalizationBuildLocalization(
	const FWoTRequiredFact& Fact, TConstArrayView<uint8> Bytes)
{
	using FResult = TWoTBootstrapResult<TArray<uint8>>;
	if (Bytes.IsEmpty()
		|| static_cast<uint64>(Bytes.Num()) > WoTRegistryLocalizationMaxSourceBytes)
		return FResult::Error(WoTRegistryLocalizationError(EWoTBootstrapError::SizeLimit,
			TEXT("the localization source is empty or oversized"), Fact.SourcePath));
	for (uint8 Byte : Bytes)
	{
		if (Byte == 0 || Byte == 0x81 || Byte == 0x8d || Byte == 0x8f
			|| Byte == 0x90 || Byte == 0x9d)
			return FResult::Error(WoTRegistryLocalizationError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("the localization source is not valid Windows-1252 text"),
				Fact.SourcePath));
	}
	uint32 SectionCount = 0, EntryCount = 0;
	bool bHasSection = false;
	int32 Start = 0;
	for (int32 Offset = 0; Offset <= Bytes.Num(); ++Offset)
	{
		const bool bEnd = Offset == Bytes.Num();
		if (!bEnd && !WoTRegistryLocalizationLineBreak(Bytes[Offset])) continue;
		int32 First = Start, Last = Offset;
		while (First < Last && WoTRegistryLocalizationWhitespace(Bytes[First])) ++First;
		while (Last > First && WoTRegistryLocalizationWhitespace(Bytes[Last - 1])) --Last;
		if (First < Last && Bytes[First] != ';' && Bytes[First] != '#')
		{
			if (Bytes[First] == '[' && Bytes[Last - 1] == ']')
			{
				int32 SectionFirst = First + 1, SectionLast = Last - 1;
				while (SectionFirst < SectionLast
					&& WoTRegistryLocalizationWhitespace(Bytes[SectionFirst])) ++SectionFirst;
				while (SectionLast > SectionFirst
					&& WoTRegistryLocalizationWhitespace(Bytes[SectionLast - 1])) --SectionLast;
				if (SectionFirst == SectionLast)
					return FResult::Error(WoTRegistryLocalizationError(
						EWoTBootstrapError::CorruptInstallation,
						TEXT("a localization section is empty"), Fact.SourcePath));
				bHasSection = true;
				if (++SectionCount > WoTRegistryLocalizationMaxRows)
					return FResult::Error(WoTRegistryLocalizationError(
						EWoTBootstrapError::SizeLimit,
						TEXT("the localization section count exceeds its bound"), Fact.SourcePath));
			}
			else
			{
				int32 Equals = INDEX_NONE;
				for (int32 Index = Start; Index < Offset; ++Index)
					if (Bytes[Index] == '=') { Equals = Index; break; }
				int32 KeyFirst = Start, KeyLast = Equals;
				if (Equals == INDEX_NONE || !bHasSection)
					return FResult::Error(WoTRegistryLocalizationError(
						EWoTBootstrapError::CorruptInstallation,
						TEXT("a localization row is outside a section or lacks '='"),
						Fact.SourcePath));
				while (KeyFirst < KeyLast
					&& WoTRegistryLocalizationWhitespace(Bytes[KeyFirst])) ++KeyFirst;
				while (KeyLast > KeyFirst
					&& WoTRegistryLocalizationWhitespace(Bytes[KeyLast - 1])) --KeyLast;
				if (KeyFirst == KeyLast)
					return FResult::Error(WoTRegistryLocalizationError(
						EWoTBootstrapError::CorruptInstallation,
						TEXT("a localization row has an empty key"), Fact.SourcePath));
				if (++EntryCount > WoTRegistryLocalizationMaxRows)
					return FResult::Error(WoTRegistryLocalizationError(
						EWoTBootstrapError::SizeLimit,
						TEXT("the localization row count exceeds its bound"), Fact.SourcePath));
			}
		}
		if (!bEnd && Bytes[Offset] == '\r' && Offset + 1 < Bytes.Num()
			&& Bytes[Offset + 1] == '\n') ++Offset;
		Start = Offset + 1;
	}
	if (SectionCount == 0)
		return FResult::Error(WoTRegistryLocalizationError(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("the localization source has no sections"), Fact.SourcePath));
	FWoTCanonicalObjectBuilder SourceBuilder;
	SourceBuilder.Add(TEXT("export"),
		FWoTCanonicalJsonValue::MakeString(Fact.SourceExport));
	SourceBuilder.Add(TEXT("package"),
		FWoTCanonicalJsonValue::MakeString(Fact.SourcePath));
	SourceBuilder.Add(TEXT("sha256"),
		FWoTCanonicalJsonValue::MakeString(Sha256(Bytes).ToHex()));
	TWoTBootstrapResult<FWoTCanonicalJsonValue> SourceJson =
		MoveTemp(SourceBuilder).Build(TEXT("localization source"));
	if (SourceJson.IsError()) return FResult::Error(MoveTemp(SourceJson).GetError());
	FWoTCanonicalObjectBuilder Builder;
	Builder.Add(TEXT("encoding"), FWoTCanonicalJsonValue::MakeString(
		TEXT("windows-1252-ini-bytes-v1")));
	Builder.Add(TEXT("entry_count"), FWoTCanonicalJsonValue::MakeInteger(EntryCount));
	Builder.Add(TEXT("fact_id"), FWoTCanonicalJsonValue::MakeString(Fact.FactId));
	Builder.Add(TEXT("fallback_language"), FWoTCanonicalJsonValue::MakeString(TEXT("en")));
	Builder.Add(TEXT("importer"),
		FWoTCanonicalJsonValue::MakeString(TEXT("localization-v1")));
	Builder.Add(TEXT("importer_version"), FWoTCanonicalJsonValue::MakeInteger(1));
	Builder.Add(TEXT("kind"),
		FWoTCanonicalJsonValue::MakeString(TEXT("localization")));
	Builder.Add(TEXT("payload_byte_length"),
		FWoTCanonicalJsonValue::MakeInteger(Bytes.Num()));
	Builder.Add(TEXT("section_count"),
		FWoTCanonicalJsonValue::MakeInteger(SectionCount));
	Builder.Add(TEXT("source"), MoveTemp(SourceJson).GetValue());
	TWoTBootstrapResult<FWoTCanonicalJsonValue> Header =
		MoveTemp(Builder).Build(TEXT("localization object header"));
	if (Header.IsError()) return FResult::Error(MoveTemp(Header).GetError());
	return WoTRegistryLocalizationFrame(Header.GetValue(), Bytes);
}

struct FWoTRegistryLocalizationFactType
{
	const TCHAR* Importer = nullptr;
	const TCHAR* Schema = nullptr;
};

bool WoTRegistryLocalizationSelectFact(const FWoTRequiredFact& Fact,
	FWoTRegistryLocalizationFactType& Out)
{
	switch (Fact.Domain)
	{
	case EWoTFactDomain::Registry:
		Out = {TEXT("registry-v1"), TEXT("wot-registry-v3")}; return true;
	case EWoTFactDomain::Localization:
		Out = {TEXT("localization-v1"), TEXT("wot-localization-v3")}; return true;
	case EWoTFactDomain::ClassDefault:
		Out = {TEXT("class-default-v2"), TEXT("wot-class-default-v3")}; return true;
	default: return false;
	}
}

class FWoTRegistryLocalizationCollectSink final : public IWoTByteSink
{
public:
	explicit FWoTRegistryLocalizationCollectSink(uint64 InLimit) : Limit(InLimit) {}
	FWoTBootstrapStatus Consume(TConstArrayView<uint8> Chunk) override
	{
		const uint64 Count = static_cast<uint64>(Chunk.Num());
		if (Count > Limit - Written
			|| Count > static_cast<uint64>(MAX_int32 - Bytes.Num()))
			return FWoTBootstrapStatus::Error(WoTRegistryLocalizationError(
				EWoTBootstrapError::SizeLimit,
				TEXT("a registry/localization source exceeds its quarantine bound")));
		Bytes.Append(Chunk.GetData(), Chunk.Num());
		Written += Count;
		return FWoTBootstrapStatus::Success();
	}
	TArray<uint8> Bytes;
private:
	uint64 Limit = 0;
	uint64 Written = 0;
};

using FWoTRegistryLocalizationLoader =
	TFunction<TWoTBootstrapResult<TArray<uint8>>(FStringView)>;

TWoTBootstrapResult<TArray<FWoTCacheBlob>> WoTRegistryLocalizationImportCore(
	const FWoTRetailContract& Contract, IWoTObjectWriter& Writer,
	IWoTProgressSink& Progress,
	FWoTRegistryLocalizationLoader&& Loader)
{
	using FResult = TWoTBootstrapResult<TArray<FWoTCacheBlob>>;
	uint64 Total = 0;
	for (const FWoTRequiredFact& Fact : Contract.Facts)
	{
		FWoTRegistryLocalizationFactType Type;
		if (WoTRegistryLocalizationSelectFact(Fact, Type))
		{
			++Total;
		}
	}
	FWoTBootstrapStatus Initial = Progress.ReportProgress(
		TEXT("registry-localization-defaults"), 0, Total);
	if (Initial.IsError()) return FResult::Error(MoveTemp(Initial).GetError());
	if (Progress.IsCancellationRequested())
		return FResult::Error(WoTRegistryLocalizationError(
			EWoTBootstrapError::ServiceUnavailable,
			TEXT("registry/localization/default import was cancelled")));
	TArray<FWoTCacheBlob> Blobs;
	TSet<FString> LogicalIds;
	uint64 Completed = 0;
	FWoTRemainingPackageResolver Resolver(Contract, Loader);
	for (const FWoTRequiredFact& Fact : Contract.Facts)
	{
		FWoTRegistryLocalizationFactType Type;
		if (!WoTRegistryLocalizationSelectFact(Fact, Type)) continue;
		if (Fact.FactId.IsEmpty() || Fact.SourcePath.IsEmpty()
			|| Fact.SourceExport.IsEmpty() || Fact.Importer != Type.Importer
			|| Fact.Outputs.Num() != 1 || Fact.Outputs[0].LogicalId.IsEmpty()
			|| Fact.Outputs[0].Schema != Type.Schema
			|| Fact.Outputs[0].MaxDecodedSize == 0)
			return FResult::Error(WoTRegistryLocalizationError(
				EWoTBootstrapError::UnknownField,
				TEXT("a registry/localization/default fact has a closed-schema mismatch"),
				Fact.FactId));
		const FWoTRequiredFactOutput& Output = Fact.Outputs[0];
		if (LogicalIds.Contains(Output.LogicalId))
			return FResult::Error(WoTRegistryLocalizationError(
				EWoTBootstrapError::DuplicateField,
					TEXT("two remaining-domain facts emit one logical object"), Output.LogicalId));
		LogicalIds.Add(Output.LogicalId);
		if (Progress.IsCancellationRequested())
			return FResult::Error(WoTRegistryLocalizationError(
				EWoTBootstrapError::ServiceUnavailable,
				TEXT("registry/localization/default import was cancelled"), Fact.FactId));
		TWoTBootstrapResult<TArray<uint8>> Loaded = Loader(Fact.SourcePath);
		if (Loaded.IsError()) return FResult::Error(MoveTemp(Loaded).GetError());
		TWoTBootstrapResult<TArray<uint8>> Object =
			Fact.Domain == EWoTFactDomain::Localization
			? WoTRegistryLocalizationBuildLocalization(Fact, Loaded.GetValue())
			: WoTRemainingBuildPackageAggregateObject(
				Loaded.GetValue(), Fact.SourcePath, Fact, Resolver);
		if (Object.IsError()) return FResult::Error(MoveTemp(Object).GetError());
		if (static_cast<uint64>(Object.GetValue().Num()) > Output.MaxDecodedSize)
			return FResult::Error(WoTRegistryLocalizationError(
				EWoTBootstrapError::SizeLimit,
				TEXT("the registry/localization/default object exceeds its contract maximum"),
				Output.LogicalId));
		if (Progress.IsCancellationRequested())
			return FResult::Error(WoTRegistryLocalizationError(
				EWoTBootstrapError::ServiceUnavailable,
				TEXT("registry/localization/default import was cancelled"), Fact.FactId));
		const FWoTSha256 ObjectSha256 = Sha256(Object.GetValue());
		FWoTBootstrapStatus Written = Writer.WriteObject(Output.LogicalId,
			Output.Schema, Object.GetValue(), ObjectSha256, Output.MaxDecodedSize);
		if (Written.IsError()) return FResult::Error(MoveTemp(Written).GetError());
		FWoTCacheBlob& Blob = Blobs.AddDefaulted_GetRef();
		Blob.LogicalId = Output.LogicalId;
		Blob.Schema = Output.Schema;
		Blob.Sha256 = ObjectSha256;
		Blob.Size = static_cast<uint64>(Object.GetValue().Num());
		Blob.Satisfies.AddDefaulted_GetRef().Value = Fact.FactId;
		FWoTBootstrapStatus Reported = Progress.ReportProgress(
			TEXT("registry-localization-defaults"), ++Completed, Total);
		if (Reported.IsError()) return FResult::Error(MoveTemp(Reported).GetError());
		if (Progress.IsCancellationRequested())
			return FResult::Error(WoTRegistryLocalizationError(
				EWoTBootstrapError::ServiceUnavailable,
				TEXT("registry/localization/default import was cancelled"), Fact.FactId));
	}
	return FResult::Success(MoveTemp(Blobs));
}
} // namespace

TWoTBootstrapResult<TArray<FWoTCacheBlob>> ImportRegistryLocalizationDefaults(
	const FWoTRetailPackage& Package, const FWoTRetailContract& Contract,
	IWoTObjectWriter& Writer, IWoTProgressSink& Progress)
{
	FWoTRegistryLocalizationLoader Loader = [&Package](FStringView SourcePath)
		-> TWoTBootstrapResult<TArray<uint8>>
	{
		using FBytesResult = TWoTBootstrapResult<TArray<uint8>>;
		TWoTBootstrapResult<FWoTPackageEntry> Entry = Package.FindExact(SourcePath);
		if (Entry.IsError()) return FBytesResult::Error(MoveTemp(Entry).GetError());
		if (Entry.GetValue().Size == 0
			|| Entry.GetValue().Size > WoTRegistryLocalizationMaxSourceBytes)
			return FBytesResult::Error(WoTRegistryLocalizationError(
				EWoTBootstrapError::SizeLimit,
				TEXT("a registry/localization source exceeds its declared bound"),
				FString(SourcePath)));
		FWoTRegistryLocalizationCollectSink Sink(WoTRegistryLocalizationMaxSourceBytes);
		FWoTBootstrapStatus Streamed = Package.StreamExact(Entry.GetValue(), Sink);
		if (Streamed.IsError()) return FBytesResult::Error(MoveTemp(Streamed).GetError());
		return FBytesResult::Success(MoveTemp(Sink.Bytes));
	};
	return WoTRegistryLocalizationImportCore(
		Contract, Writer, Progress, MoveTemp(Loader));
}

#if WITH_AUTOMATION_TESTS
TWoTBootstrapResult<TArray<FWoTCacheBlob>>
ImportRegistryLocalizationDefaultsSourcesForTesting(
	TConstArrayView<FWoTRemainingSourceBytesForTesting> Sources,
	const FWoTRetailContract& Contract, IWoTObjectWriter& Writer,
	IWoTProgressSink& Progress)
{
	FWoTRegistryLocalizationLoader Loader = [&Sources](FStringView SourcePath)
		-> TWoTBootstrapResult<TArray<uint8>>
	{
		using FBytesResult = TWoTBootstrapResult<TArray<uint8>>;
		for (const FWoTRemainingSourceBytesForTesting& Source : Sources)
			if (Source.SourcePath == SourcePath)
				return FBytesResult::Success(Source.Bytes);
		return FBytesResult::Error(WoTRegistryLocalizationError(
			EWoTBootstrapError::UnknownField,
			TEXT("the requested test fallback source is absent"), FString(SourcePath)));
	};
	return WoTRegistryLocalizationImportCore(
		Contract, Writer, Progress, MoveTemp(Loader));
}
#endif
