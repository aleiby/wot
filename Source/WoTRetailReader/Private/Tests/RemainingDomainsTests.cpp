// Cache-v3 closure specs for fonts, procedural inputs, registries, .int bytes,
// and typed Class defaults.
#include "Misc/AutomationTest.h"

#include "Containers/StringConv.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "NativeHandleTestFactory.h"
#include "RemainingPackage.h"
#include "WoTBootstrapCore/CanonicalJson.h"
#include "WoTBootstrapCore/Hash.h"
#include "WoTBootstrapCore/NativeHandles.h"
#include "WoTRetailReader/CacheBuilder.h"
#include "WoTRetailReader/FontProcedural.h"
#include "WoTRetailReader/RegistryLocalization.h"
#include "WoTRetailReader/Testing/ReferencePaths.h"

#if WITH_AUTOMATION_TESTS
namespace
{
constexpr EAutomationTestFlags WoTRemainingDomainsSpecFlags =
	EAutomationTestFlags::ProgramContext | EAutomationTestFlags::EditorContext
	| EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter;

struct FWoTRemainingDomainsSpecRoot
{
	FString Path = WoTNativeHandleTestFactory::CreateTemporaryRoot();
	~FWoTRemainingDomainsSpecRoot() { WoTNativeHandleTestFactory::RemoveTree(Path); }
};

FString WoTRemainingDomainsSpecFixturePath(const TCHAR* Name)
{
	const FString ThisFile(ANSI_TO_TCHAR(__FILE__));
	return FPaths::ConvertRelativePathToFull(FPaths::GetPath(ThisFile)
		/ TEXT("../../../../Tools/dist/fixtures") / Name);
}

FString WoTRemainingDomainsSpecRetailPath(const TCHAR* Name)
{
	return WoTReferencePath(FString(TEXT("original/System/")) + Name);
}

bool WoTRemainingDomainsSpecLoadFixture(const TCHAR* Name, TArray<uint8>& Out)
{
	return FFileHelper::LoadFileToArray(Out, *WoTRemainingDomainsSpecFixturePath(Name));
}

TWoTBootstrapResult<FWoTHeldInstallerSnapshot> WoTRemainingDomainsSpecSnapshot(
	const FString& Path)
{
	using FResult = TWoTBootstrapResult<FWoTHeldInstallerSnapshot>;
	TWoTBootstrapResult<FWoTNativeFileHandle> Opened = OpenNativeFileReadNoFollow(Path);
	if (Opened.IsError()) return FResult::Error(MoveTemp(Opened).GetError());
	TWoTBootstrapResult<TArray<uint8>> Read = Opened.GetValue().ReadAll();
	if (Read.IsError()) return FResult::Error(MoveTemp(Read).GetError());
	FWoTHeldInstallerSnapshot Snapshot;
	Snapshot.Size = static_cast<uint64>(Read.GetValue().Num());
	Snapshot.Sha256 = Sha256(Read.GetValue());
	Snapshot.Identity = Opened.GetValue().GetIdentity();
	Snapshot.Handle = MoveTemp(Opened).GetValue();
	return FResult::Success(MoveTemp(Snapshot));
}

TWoTBootstrapResult<FWoTRetailPackage> WoTRemainingDomainsSpecOpen(
	FWoTRemainingDomainsSpecRoot& Root, const TCHAR* FixtureName)
{
	using FResult = TWoTBootstrapResult<FWoTRetailPackage>;
	TArray<uint8> Fixture;
	if (!WoTRemainingDomainsSpecLoadFixture(FixtureName, Fixture))
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Io,
			TEXT("a remaining-domain fixture is missing"), FixtureName));
	const FString Installer = Root.Path / FString(FixtureName);
	if (!WoTNativeHandleTestFactory::WriteFile(Installer, Fixture))
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Io,
			TEXT("a remaining-domain fixture could not be staged"), Installer));
	TWoTBootstrapResult<FWoTHeldInstallerSnapshot> Snapshot =
		WoTRemainingDomainsSpecSnapshot(Installer);
	if (Snapshot.IsError()) return FResult::Error(MoveTemp(Snapshot).GetError());
	return FWoTRetailPackage::Open(MoveTemp(Snapshot).GetValue(), FWoTArchiveLimits());
}

struct FWoTRemainingDomainsSpecWritten
{
	FString LogicalId;
	FString Schema;
	TArray<uint8> Bytes;
};

class FWoTRemainingDomainsSpecWriter final : public IWoTObjectWriter
{
public:
	FWoTBootstrapStatus WriteObject(FStringView LogicalId, FStringView Schema,
		TConstArrayView<uint8> Bytes, const FWoTSha256&, uint64 MaximumBytes) override
	{
		if (bRejectNext)
		{
			bRejectNext = false;
			return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::Io, TEXT("synthetic remaining-domain writer refusal")));
		}
		if (static_cast<uint64>(Bytes.Num()) > MaximumBytes)
			return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::SizeLimit, TEXT("remaining-domain writer overflow")));
		FWoTRemainingDomainsSpecWritten& Written = Objects.AddDefaulted_GetRef();
		Written.LogicalId = FString(LogicalId);
		Written.Schema = FString(Schema);
		Written.Bytes.Append(Bytes.GetData(), Bytes.Num());
		if (AfterWrite)
		{
			AfterWrite();
		}
		return FWoTBootstrapStatus::Success();
	}
	const FWoTRemainingDomainsSpecWritten* Find(FStringView LogicalId) const
	{
		for (const FWoTRemainingDomainsSpecWritten& Object : Objects)
			if (Object.LogicalId == LogicalId) return &Object;
		return nullptr;
	}
	TArray<FWoTRemainingDomainsSpecWritten> Objects;
	TFunction<void()> AfterWrite;
	bool bRejectNext = false;
};

class FWoTRemainingDomainsSpecProgress final : public IWoTProgressSink
{
public:
	FWoTBootstrapStatus ReportProgress(FStringView Stage, uint64 Completed,
		uint64 Total) override
	{
		LastStage = FString(Stage);
		LastCompleted = Completed;
		LastTotal = Total;
		if (Completed >= CancelAt) bCancelled = true;
		return FWoTBootstrapStatus::Success();
	}
	bool IsCancellationRequested() const override { return bCancelled; }
	FString LastStage;
	uint64 LastCompleted = 0;
	uint64 LastTotal = 0;
	uint64 CancelAt = TNumericLimits<uint64>::Max();
	bool bCancelled = false;
};

void WoTRemainingDomainsSpecAddFact(FWoTRetailContract& Contract,
	EWoTFactDomain Domain, const TCHAR* FactId, const TCHAR* SourcePath,
	const TCHAR* SourceExport, const TCHAR* Importer, const TCHAR* LogicalId,
	const TCHAR* Schema, uint64 MaximumBytes = 268435456)
{
	FWoTRequiredFact& Fact = Contract.Facts.AddDefaulted_GetRef();
	Fact.Domain = Domain;
	Fact.FactId = FactId;
	Fact.SourcePath = SourcePath;
	Fact.SourceExport = SourceExport;
	Fact.Importer = Importer;
	Fact.RuntimeConsumer = TEXT("UWoTPayloadSubsystem");
	Fact.AuditRule = TEXT("audit.remaining-domain-v1");
	FWoTRequiredFactOutput& Output = Fact.Outputs.AddDefaulted_GetRef();
	Output.LogicalId = LogicalId;
	Output.Schema = Schema;
	Output.MaxDecodedSize = MaximumBytes;
}

FWoTRetailContract WoTRemainingDomainsSpecContract()
{
	FWoTRetailContract Contract;
	WoTRemainingDomainsSpecAddFact(Contract, EWoTFactDomain::Font,
		TEXT("synthetic.font.pages"), TEXT("Synthetic/remaining.u"),
		TEXT("remaining.FontA"), TEXT("font-v2"),
		TEXT("cache/font/synthetic-pages"), TEXT("wot-font-v3"));
	WoTRemainingDomainsSpecAddFact(Contract, EWoTFactDomain::Texture,
		TEXT("synthetic.texture.font-atlas"), TEXT("Synthetic/remaining.u"),
		TEXT("remaining.Atlas"), TEXT("texture-v2"),
		TEXT("cache/texture/synthetic-atlas"), TEXT("wot-texture-v3"));
	WoTRemainingDomainsSpecAddFact(Contract, EWoTFactDomain::Procedural,
		TEXT("synthetic.procedural.fire"), TEXT("Synthetic/remaining.u"),
		TEXT("remaining.FireA"), TEXT("procedural-v1"),
		TEXT("cache/procedural/synthetic-fire"), TEXT("wot-procedural-v3"));
	WoTRemainingDomainsSpecAddFact(Contract, EWoTFactDomain::Registry,
		TEXT("synthetic.registry.package"), TEXT("Synthetic/remaining.u"),
		TEXT("remaining"), TEXT("registry-v1"),
		TEXT("cache/registry/synthetic-package"), TEXT("wot-registry-v3"));
	WoTRemainingDomainsSpecAddFact(Contract, EWoTFactDomain::Registry,
		TEXT("synthetic.registry.core"), TEXT("Synthetic/Core.u"),
		TEXT("Core"), TEXT("registry-v1"),
		TEXT("cache/registry/synthetic-core"), TEXT("wot-registry-v3"));
	WoTRemainingDomainsSpecAddFact(Contract, EWoTFactDomain::Localization,
		TEXT("synthetic.localization.english"), TEXT("Synthetic/remaining.int"),
		TEXT("remaining.int"), TEXT("localization-v1"),
		TEXT("cache/localization/synthetic-en"), TEXT("wot-localization-v3"));
	WoTRemainingDomainsSpecAddFact(Contract, EWoTFactDomain::ClassDefault,
		TEXT("synthetic.class-default.package"), TEXT("Synthetic/remaining.u"),
		TEXT("remaining"), TEXT("class-default-v2"),
		TEXT("cache/class-default/synthetic-package"), TEXT("wot-class-default-v3"));
	return Contract;
}

TWoTBootstrapResult<FWoTCanonicalJsonValue> WoTRemainingDomainsSpecHeader(
	const TArray<uint8>& Object)
{
	using FResult = TWoTBootstrapResult<FWoTCanonicalJsonValue>;
	if (Object.Num() < 12 || FMemory::Memcmp(Object.GetData(), "WOTOBJ3", 8) != 0)
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("a remaining-domain object frame is malformed")));
	const uint32 Size = static_cast<uint32>(Object[8])
		| (static_cast<uint32>(Object[9]) << 8)
		| (static_cast<uint32>(Object[10]) << 16)
		| (static_cast<uint32>(Object[11]) << 24);
	if (Size > static_cast<uint32>(Object.Num() - 12))
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("a remaining-domain header exceeds its frame")));
	return ParseCanonicalJson(TConstArrayView<uint8>(
		Object.GetData() + 12, static_cast<int32>(Size)));
}

bool WoTRemainingDomainsSpecExactHeader(const TArray<uint8>& Object,
	TConstArrayView<const TCHAR*> Keys)
{
	TWoTBootstrapResult<FWoTCanonicalJsonValue> Header =
		WoTRemainingDomainsSpecHeader(Object);
	return !Header.IsError()
		&& !RequireExactMembers(Header.GetValue(), Keys, TEXT("remaining header")).IsSet();
}

bool WoTRemainingDomainsSpecContains(
	TConstArrayView<uint8> Bytes, const ANSICHAR* Needle)
{
	const int32 Length = FCStringAnsi::Strlen(Needle);
	if (Length <= 0 || Length > Bytes.Num()) return false;
	for (int32 Offset = 0; Offset <= Bytes.Num() - Length; ++Offset)
		if (FMemory::Memcmp(Bytes.GetData() + Offset, Needle, Length) == 0) return true;
	return false;
}

bool WoTRemainingDomainsSpecReadString(
	FWoTRemainingReader& Reader, FString& Out)
{
	uint32 Count = 0;
	TArray<uint8> Bytes;
	if (!Reader.ReadU32(Count) || Count > 1024u * 1024u
		|| !Reader.ReadBytes(Count, Bytes)) return false;
	if (Count == 0)
	{
		Out.Empty();
		return true;
	}
	const FUTF8ToTCHAR Converted(
		reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
	if (Converted.Length() <= 0) return false;
	Out = FString(Converted.Length(), Converted.Get());
	FTCHARToUTF8 RoundTrip(*Out);
	return RoundTrip.Length() == Bytes.Num()
		&& FMemory::Memcmp(RoundTrip.Get(), Bytes.GetData(), Bytes.Num()) == 0;
}

struct FWoTRemainingDomainsSpecMemberCensus
{
	FString Canonical;
	TMap<FString, uint32> ImmediateMembersByStruct;
	TArray<FString> DecodedMembers;
	uint32 MaximumDepth = 0;
};

bool WoTRemainingDomainsSpecReadCensusProperty(
	FWoTRemainingReader& Reader, FStringView ParentPath,
	FWoTRemainingDomainsSpecMemberCensus& Census, uint32 Depth,
	bool bAncestorStruct)
{
	if (Depth > 8) return false;
	FString Name, StructName, NameValue, ObjectIdentity, ObjectClass;
	TArray<uint8> Value;
	uint8 Type = 0;
	uint32 ArrayIndex = 0, ValueBytes = 0, MemberCount = 0;
	if (!WoTRemainingDomainsSpecReadString(Reader, Name) || Name.IsEmpty()
		|| !Reader.ReadU8(Type) || !Reader.ReadU32(ArrayIndex)
		|| !WoTRemainingDomainsSpecReadString(Reader, StructName)
		|| !WoTRemainingDomainsSpecReadString(Reader, NameValue)
		|| !WoTRemainingDomainsSpecReadString(Reader, ObjectIdentity)
		|| !WoTRemainingDomainsSpecReadString(Reader, ObjectClass)
		|| !Reader.ReadU32(ValueBytes) || !Reader.ReadBytes(ValueBytes, Value)
		|| !Reader.ReadU32(MemberCount) || MemberCount > 4096) return false;
	const FString Path = FString(ParentPath) + TEXT(".") + Name
		+ FString::Printf(TEXT("[%u]"), ArrayIndex);
	const bool bCapture = bAncestorStruct || Type == 10;
	if (bCapture)
	{
		Census.Canonical += FString::Printf(TEXT("%s|%u|%s\n"),
			*Path, Type, *StructName);
	}
	if (bAncestorStruct)
	{
		Census.MaximumDepth = FMath::Max(Census.MaximumDepth, Depth);
		Census.DecodedMembers.Add(FString::Printf(TEXT("%s|%u|%s|%s|%s"),
			*Path, Type, *StructName, *NameValue,
			*BytesToHex(Value.GetData(), Value.Num())));
	}
	if (Type == 10)
	{
		Census.ImmediateMembersByStruct.FindOrAdd(
			WoTRemainingFold(StructName)) += MemberCount;
	}
	else if (MemberCount != 0)
	{
		return false;
	}
	for (uint32 MemberIndex = 0; MemberIndex < MemberCount; ++MemberIndex)
	{
		if (!WoTRemainingDomainsSpecReadCensusProperty(
			Reader, Path, Census, Depth + 1, bCapture)) return false;
	}
	return true;
}

bool WoTRemainingDomainsSpecClassDefaultCensus(
	TConstArrayView<uint8> Object, FWoTRemainingDomainsSpecMemberCensus& Out)
{
	if (Object.Num() < 28 || FMemory::Memcmp(Object.GetData(), "WOTOBJ3", 8) != 0)
		return false;
	const uint32 HeaderBytes = static_cast<uint32>(Object[8])
		| (static_cast<uint32>(Object[9]) << 8)
		| (static_cast<uint32>(Object[10]) << 16)
		| (static_cast<uint32>(Object[11]) << 24);
	const uint64 PayloadOffset = 12ull + HeaderBytes;
	if (PayloadOffset > static_cast<uint64>(Object.Num())
		|| static_cast<uint64>(Object.Num()) - PayloadOffset < 16
		|| FMemory::Memcmp(Object.GetData() + PayloadOffset, "WOTDEF3", 8) != 0)
		return false;
	FWoTRemainingReader Reader(Object, PayloadOffset, Object.Num());
	uint16 Version = 0, Flags = 0;
	uint32 ClassCount = 0;
	if (!Reader.Skip(8) || !Reader.ReadU16(Version) || Version != 3
		|| !Reader.ReadU16(Flags) || Flags != 0 || !Reader.ReadU32(ClassCount)
		|| ClassCount > WoTRemainingMaxItems) return false;
	for (uint32 ClassIndex = 0; ClassIndex < ClassCount; ++ClassIndex)
	{
		FString Identity, Superclass;
		uint32 ExportIndex = 0, PropertyCount = 0;
		uint8 Placeholder = 0;
		if (!WoTRemainingDomainsSpecReadString(Reader, Identity)
			|| Identity.IsEmpty() || !Reader.ReadU32(ExportIndex) || ExportIndex == 0
			|| !WoTRemainingDomainsSpecReadString(Reader, Superclass)
			|| !Reader.ReadU8(Placeholder) || Placeholder > 1
			|| !Reader.ReadU32(PropertyCount) || PropertyCount > WoTRemainingMaxItems)
			return false;
		for (uint32 PropertyIndex = 0; PropertyIndex < PropertyCount; ++PropertyIndex)
		{
			if (!WoTRemainingDomainsSpecReadCensusProperty(
				Reader, Identity, Out, 0, false)) return false;
		}
	}
	return Reader.Tell() == Reader.GetEnd();
}

FString WoTRemainingDomainsSpecMemberCensusDigest(
	const FWoTRemainingDomainsSpecMemberCensus& Census)
{
	FTCHARToUTF8 Encoded(*Census.Canonical);
	return Sha256(TConstArrayView<uint8>(
		reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length())).ToHex();
}

struct FWoTRemainingDomainsSpecAggregateRecord
{
	FString Identity;
	TArray<uint8> Bytes;
};

FString WoTRemainingDomainsSpecAggregateHash(const ANSICHAR* Domain,
	TArray<FWoTRemainingDomainsSpecAggregateRecord> Records)
{
	Records.Sort([](const FWoTRemainingDomainsSpecAggregateRecord& Left,
		const FWoTRemainingDomainsSpecAggregateRecord& Right)
	{
		const FString LF = Left.Identity.ToLower();
		const FString RF = Right.Identity.ToLower();
		return LF == RF ? Left.Identity < Right.Identity : LF < RF;
	});
	FWoTSha256Hasher Hasher;
	static constexpr ANSICHAR Prefix[] = "WOT-REMAINING-DOMAIN-V1";
	Hasher.Update(TConstArrayView<uint8>(
		reinterpret_cast<const uint8*>(Prefix), sizeof(Prefix)));
	const int32 DomainLength = FCStringAnsi::Strlen(Domain);
	Hasher.Update(TConstArrayView<uint8>(
		reinterpret_cast<const uint8*>(Domain), DomainLength + 1));
	auto AppendU32 = [&Hasher](uint32 Value)
	{
		const uint8 Bytes[] = {static_cast<uint8>(Value),
			static_cast<uint8>(Value >> 8), static_cast<uint8>(Value >> 16),
			static_cast<uint8>(Value >> 24)};
		Hasher.Update(MakeArrayView(Bytes));
	};
	AppendU32(static_cast<uint32>(Records.Num()));
	for (const FWoTRemainingDomainsSpecAggregateRecord& Record : Records)
	{
		FTCHARToUTF8 Encoded(*Record.Identity);
		AppendU32(static_cast<uint32>(Encoded.Length()));
		Hasher.Update(TConstArrayView<uint8>(
			reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length()));
		const FWoTSha256 ObjectHash = Sha256(Record.Bytes);
		Hasher.Update(TConstArrayView<uint8>(ObjectHash.GetData(), ObjectHash.Num()));
	}
	return Hasher.Finish().ToHex();
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRemainingDomainsSyntheticSpec,
	"WoT.RetailReader.RemainingDomains.SyntheticParity", WoTRemainingDomainsSpecFlags)

bool FWoTRemainingDomainsSyntheticSpec::RunTest(const FString&)
{
	FWoTRemainingDomainsSpecRoot Root;
	TWoTBootstrapResult<FWoTRetailPackage> Opened = WoTRemainingDomainsSpecOpen(
		Root, TEXT("cache-v3-remaining-importer-input.bin"));
	if (!TestFalse(TEXT("remaining-domain installer opens"), Opened.IsError())) return false;
	FWoTRemainingDomainsSpecWriter Writer;
	FWoTRemainingDomainsSpecProgress FontProgress, AggregateProgress;
	const FWoTRetailContract Contract = WoTRemainingDomainsSpecContract();
	TWoTBootstrapResult<TArray<FWoTCacheBlob>> Fonts = ImportFontProcedural(
		Opened.GetValue(), Contract, Writer, FontProgress);
	TWoTBootstrapResult<TArray<FWoTCacheBlob>> Aggregates =
		ImportRegistryLocalizationDefaults(
			Opened.GetValue(), Contract, Writer, AggregateProgress);
	if (!TestFalse(TEXT("font/procedural synthetic facts import"), Fonts.IsError()))
		return false;
	if (!TestFalse(TEXT("aggregate synthetic facts import"), Aggregates.IsError()))
		return false;
	TestEqual(TEXT("six importer-owned objects are written"), Writer.Objects.Num(), 6);
	TestEqual(TEXT("font/procedural fact total"), FontProgress.LastTotal, uint64(2));
	TestEqual(TEXT("aggregate fact total"), AggregateProgress.LastTotal, uint64(4));
	struct FExpected { const TCHAR* LogicalId; const TCHAR* Fixture; int32 Kind; };
	const FExpected Expected[] = {
		{TEXT("cache/font/synthetic-pages"), TEXT("cache-v3-font-pages.bin"), 0},
		{TEXT("cache/procedural/synthetic-fire"),
			TEXT("cache-v3-procedural-fire.bin"), 1},
		{TEXT("cache/registry/synthetic-package"),
			TEXT("cache-v3-registry-package.bin"), 2},
		{TEXT("cache/registry/synthetic-core"),
			TEXT("cache-v3-registry-core.bin"), 2},
		{TEXT("cache/localization/synthetic-en"),
			TEXT("cache-v3-localization-table.bin"), 3},
		{TEXT("cache/class-default/synthetic-package"),
			TEXT("cache-v3-class-defaults.bin"), 4},
	};
	const TCHAR* FontKeys[] = {TEXT("baseline"), TEXT("character_map_count"),
		TEXT("characters_per_page"), TEXT("encoding"), TEXT("fact_id"),
		TEXT("glyph_count"), TEXT("importer"), TEXT("importer_version"),
		TEXT("kerning_count"), TEXT("kind"), TEXT("line_height"),
		TEXT("page_count"), TEXT("pages"), TEXT("payload_byte_length"),
		TEXT("source")};
	const TCHAR* ProceduralKeys[] = {TEXT("class_name"), TEXT("dimensions"),
		TEXT("drop_count"), TEXT("encoding"), TEXT("fact_id"),
		TEXT("glass_class"), TEXT("glass_texture"), TEXT("importer"),
		TEXT("importer_version"), TEXT("kind"), TEXT("masked"), TEXT("mip_count"),
		TEXT("palette_identity"), TEXT("payload_byte_length"),
		TEXT("property_count"), TEXT("random_scope"), TEXT("random_seed"),
		TEXT("references"), TEXT("source"), TEXT("source_class"),
		TEXT("source_texture"), TEXT("state_count"), TEXT("state_kind")};
	const TCHAR* RegistryKeys[] = {TEXT("class_count"), TEXT("directive_count"),
		TEXT("encoding"), TEXT("export_count"), TEXT("fact_id"), TEXT("import_count"),
		TEXT("importer"), TEXT("importer_version"), TEXT("kind"), TEXT("name_count"),
		TEXT("payload_byte_length"), TEXT("source")};
	const TCHAR* LocalizationKeys[] = {TEXT("encoding"), TEXT("entry_count"),
		TEXT("fact_id"), TEXT("fallback_language"), TEXT("importer"),
		TEXT("importer_version"), TEXT("kind"), TEXT("payload_byte_length"),
		TEXT("section_count"), TEXT("source")};
	const TCHAR* DefaultKeys[] = {TEXT("class_count"), TEXT("default_count"),
		TEXT("encoding"), TEXT("fact_id"), TEXT("importer"), TEXT("importer_version"),
		TEXT("kind"), TEXT("payload_byte_length"), TEXT("reference_count"),
		TEXT("source")};
	for (const FExpected& Item : Expected)
	{
		TArray<uint8> Fixture;
		const FWoTRemainingDomainsSpecWritten* Written = Writer.Find(Item.LogicalId);
		TestTrue(FString::Printf(TEXT("%s expected bytes load"), Item.Fixture),
			WoTRemainingDomainsSpecLoadFixture(Item.Fixture, Fixture));
		if (!TestNotNull(Item.LogicalId, Written)) continue;
		TestTrue(FString::Printf(TEXT("%s exactly matches Python"), Item.LogicalId),
			Written->Bytes == Fixture);
		bool bExact = false;
		switch (Item.Kind)
		{
		case 0: bExact = WoTRemainingDomainsSpecExactHeader(
			Written->Bytes, MakeArrayView(FontKeys)); break;
		case 1: bExact = WoTRemainingDomainsSpecExactHeader(
			Written->Bytes, MakeArrayView(ProceduralKeys)); break;
		case 2: bExact = WoTRemainingDomainsSpecExactHeader(
			Written->Bytes, MakeArrayView(RegistryKeys)); break;
		case 3: bExact = WoTRemainingDomainsSpecExactHeader(
			Written->Bytes, MakeArrayView(LocalizationKeys)); break;
		case 4: bExact = WoTRemainingDomainsSpecExactHeader(
			Written->Bytes, MakeArrayView(DefaultKeys)); break;
		default: break;
		}
		TestTrue(FString::Printf(TEXT("%s has the exact header key set"), Item.LogicalId),
			bExact);
		if (Item.Kind == 0)
		{
			TWoTBootstrapResult<FWoTCanonicalJsonValue> Header =
				WoTRemainingDomainsSpecHeader(Written->Bytes);
			if (TestFalse(TEXT("font-v2 header parses"), Header.IsError()))
			{
				FString Encoding;
				FString Importer;
				uint32 Version = 0;
				const FWoTCanonicalJsonValue* Pages =
					Header.GetValue().FindMember(TEXT("pages"));
				TestFalse(TEXT("font-v2 encoding is exact"), ReadStringField(
					Header.GetValue(), TEXT("encoding"), TEXT("font-v2"), Encoding).IsSet());
				TestEqual(TEXT("font-v2 encoding value"), Encoding,
					FString(TEXT("ue1-font-pages-v2")));
				TestFalse(TEXT("font-v2 importer is exact"), ReadStringField(
					Header.GetValue(), TEXT("importer"), TEXT("font-v2"), Importer).IsSet());
				TestEqual(TEXT("font-v2 importer value"), Importer,
					FString(TEXT("font-v2")));
				TestFalse(TEXT("font-v2 version is readable"), ReadUnsigned32Field(
					Header.GetValue(), TEXT("importer_version"), TEXT("font-v2"), Version).IsSet());
				TestEqual(TEXT("font-v2 version value"), Version, uint32(2));
				if (TestNotNull(TEXT("font-v2 ordered pages exist"), Pages)
					&& TestTrue(TEXT("font-v2 pages are an array"), Pages->IsArray())
					&& TestEqual(TEXT("synthetic font has one page"), Pages->Num(), 1))
				{
					static const TCHAR* PageKeys[] = {TEXT("character_count"),
						TEXT("format"), TEXT("height"), TEXT("logical_id"),
						TEXT("ordinal"), TEXT("source_identity"),
						TEXT("source_sha256"), TEXT("width")};
					const FWoTCanonicalJsonValue& Page = Pages->GetElement(0);
					TestFalse(TEXT("font-v2 page has exact members"),
						RequireExactMembers(Page, MakeArrayView(PageKeys),
							TEXT("font-v2 page")).IsSet());
					FString LogicalId;
					TestFalse(TEXT("font-v2 page logical id is readable"),
						ReadStringField(Page, TEXT("logical_id"),
							TEXT("font-v2 page"), LogicalId).IsSet());
					TestEqual(TEXT("atlas identity binds its declared texture id"),
						LogicalId, FString(TEXT("cache/texture/synthetic-atlas")));
				}
			}
		}
	}
	const FWoTRemainingDomainsSpecWritten* SyntheticDefaults =
		Writer.Find(TEXT("cache/class-default/synthetic-package"));
	if (TestNotNull(TEXT("synthetic defaults exist for independent member checks"),
		SyntheticDefaults))
	{
		FWoTRemainingDomainsSpecMemberCensus Census;
		if (TestTrue(TEXT("synthetic defaults decode for independent member checks"),
			WoTRemainingDomainsSpecClassDefaultCensus(
				SyntheticDefaults->Bytes, Census)))
		{
			static const TCHAR* ExpectedMembers[] = {
				TEXT("remaining.SyntheticClass.Composite[0].FixedValues[0]|2|||07000000"),
				TEXT("remaining.SyntheticClass.Composite[0].FixedValues[1]|2|||08000000"),
				TEXT("remaining.SyntheticClass.Composite[0].FixedValues[2]|2|||09000000"),
				TEXT("remaining.SyntheticClass.Composite[0].ReplicatedValue[0]|2|||63000000"),
				TEXT("remaining.SyntheticClass.Composite[0].BaseValue[0]|2|||3A010000"),
			};
			for (const TCHAR* ExpectedMember : ExpectedMembers)
			{
				TestTrue(FString(TEXT("hard-coded reflected member: ")) + ExpectedMember,
					Census.DecodedMembers.Contains(ExpectedMember));
			}
			TestEqual(TEXT("producer accepts exactly eight nested member levels"),
				Census.MaximumDepth, 8u);
		}
	}
	FWoTRemainingDomainsSpecWriter RepeatWriter;
	FWoTRemainingDomainsSpecProgress RepeatProgress;
	TWoTBootstrapResult<TArray<FWoTCacheBlob>> Repeat =
		ImportRegistryLocalizationDefaults(
			Opened.GetValue(), Contract, RepeatWriter, RepeatProgress);
	if (TestFalse(TEXT("registry ordering repeats successfully"), Repeat.IsError()))
	{
		const TCHAR* RepeatLogicalIds[] = {TEXT("cache/registry/synthetic-package"),
			TEXT("cache/registry/synthetic-core"),
			TEXT("cache/localization/synthetic-en"),
			TEXT("cache/class-default/synthetic-package")};
		for (const TCHAR* LogicalId : RepeatLogicalIds)
		{
			const FWoTRemainingDomainsSpecWritten* First = Writer.Find(LogicalId);
			const FWoTRemainingDomainsSpecWritten* Second = RepeatWriter.Find(LogicalId);
			if (TestNotNull(TEXT("first deterministic object exists"), First)
				&& TestNotNull(TEXT("second deterministic object exists"), Second))
				TestTrue(TEXT("map-backed registry ordering is deterministic"),
					First->Bytes == Second->Bytes);
		}
	}
	FWoTRemainingDomainsSpecRoot CollisionRoot;
	TWoTBootstrapResult<FWoTRetailPackage> CollisionOpened =
		WoTRemainingDomainsSpecOpen(
			CollisionRoot, TEXT("cache-v3-reference-collision.bin"));
	if (TestFalse(TEXT("reference-collision installer opens"), CollisionOpened.IsError()))
	{
		FWoTRetailContract CollisionContract;
		for (const FWoTRequiredFact& Fact : Contract.Facts)
			if (Fact.Domain == EWoTFactDomain::Registry
				|| Fact.Domain == EWoTFactDomain::ClassDefault)
				CollisionContract.Facts.Add(Fact);
		FWoTRemainingDomainsSpecWriter CollisionWriter;
		FWoTRemainingDomainsSpecProgress CollisionProgress;
		TWoTBootstrapResult<TArray<FWoTCacheBlob>> CollisionResult =
			ImportRegistryLocalizationDefaults(CollisionOpened.GetValue(),
				CollisionContract, CollisionWriter, CollisionProgress);
		if (TestFalse(TEXT("colliding Class and Mesh references close"),
			CollisionResult.IsError()))
		{
			const FWoTRemainingDomainsSpecWritten* Registry =
				CollisionWriter.Find(TEXT("cache/registry/synthetic-package"));
			const FWoTRemainingDomainsSpecWritten* Defaults =
				CollisionWriter.Find(TEXT("cache/class-default/synthetic-package"));
			if (TestNotNull(TEXT("collision registry exists"), Registry))
			{
				TestTrue(TEXT("Class collision row is suffixed"),
					WoTRemainingDomainsSpecContains(Registry->Bytes,
						"remaining.SyntheticClass#export-00000007"));
				TestTrue(TEXT("Mesh collision row is suffixed"),
					WoTRemainingDomainsSpecContains(Registry->Bytes,
						"remaining.SyntheticClass#export-00000008"));
			}
			if (TestNotNull(TEXT("collision defaults exist"), Defaults))
				TestTrue(TEXT("typed Mesh property binds the suffixed Mesh row"),
					WoTRemainingDomainsSpecContains(Defaults->Bytes,
						"remaining.SyntheticClass#export-00000008"));
		}
	}
	FWoTRetailContract WrongFontClass;
	WoTRemainingDomainsSpecAddFact(WrongFontClass, EWoTFactDomain::Font,
		TEXT("synthetic.font.wrong-source"), TEXT("Synthetic/remaining.u"),
		TEXT("remaining.FireA"), TEXT("font-v2"),
		TEXT("cache/font/wrong-source"), TEXT("wot-font-v3"));
	FWoTRemainingDomainsSpecWriter WrongFontWriter;
	FWoTRemainingDomainsSpecProgress WrongFontProgress;
	TWoTBootstrapResult<TArray<FWoTCacheBlob>> WrongFont =
		ImportFontProcedural(Opened.GetValue(), WrongFontClass,
			WrongFontWriter, WrongFontProgress);
	TestTrue(TEXT("font identity class is asserted after canonical lookup"),
		WrongFont.IsError());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRemainingDomainsPublicationReverifySpec,
	"WoT.RetailReader.RemainingDomains.PerPublicationSnapshotReverification",
	WoTRemainingDomainsSpecFlags)

bool FWoTRemainingDomainsPublicationReverifySpec::RunTest(const FString&)
{
	constexpr const TCHAR* FixtureName =
		TEXT("cache-v3-remaining-importer-input.bin");
	FWoTRemainingDomainsSpecRoot Root;
	TWoTBootstrapResult<FWoTRetailPackage> Opened =
		WoTRemainingDomainsSpecOpen(Root, FixtureName);
	if (!TestFalse(TEXT("shared font/procedural package opens"),
		Opened.IsError()))
	{
		return false;
	}
	FWoTRetailContract Contract = WoTRemainingDomainsSpecContract();
	Contract.Facts.SetNum(3);
	TArray<uint8> Mutated;
	if (!TestTrue(TEXT("shared font/procedural installer bytes load"),
		WoTRemainingDomainsSpecLoadFixture(FixtureName, Mutated))
		|| Mutated.IsEmpty())
	{
		return false;
	}
	// Change the length as well as the bytes (same NTFS ChangeTime race as
	// MeshAnimation.PerPublicationSnapshotReverification, fixed in 5921249f6):
	// size is part of the held identity, so detection is deterministic.
	Mutated.Add(0x7c);
	bool bMutationAttempted = false;
	bool bMutationSucceeded = false;
	FWoTRemainingDomainsSpecWriter Writer;
	Writer.AfterWrite = [&]()
	{
		if (!bMutationAttempted && Writer.Objects.Num() == 1)
		{
			bMutationAttempted = true;
			bMutationSucceeded = WoTNativeHandleTestFactory::WriteFile(
				Root.Path / FixtureName, Mutated);
		}
	};
	FWoTRemainingDomainsSpecProgress Progress;
	TWoTBootstrapResult<TArray<FWoTCacheBlob>> Imported =
		ImportFontProcedural(Opened.GetValue(), Contract, Writer, Progress);
	TestTrue(TEXT("the held installer is mutated after the first font publication"),
		bMutationAttempted && bMutationSucceeded);
	TestTrue(TEXT("the next shared-package procedural fact fails closed"),
		Imported.IsError());
	TestEqual(TEXT("the second font/procedural object is never written"),
		Writer.Objects.Num(), 1);
	TestEqual(TEXT("font publication verification performs no second decode"),
		Opened.GetValue().EntryDecodeCountForTesting(), 1ull);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRemainingDomainsRejectionSpec,
	"WoT.RetailReader.RemainingDomains.Rejections", WoTRemainingDomainsSpecFlags)

bool FWoTRemainingDomainsRejectionSpec::RunTest(const FString&)
{
	auto MakeBoundedRecord = []
	{
		FWoTFontV2Record Record;
		Record.SourceExport = TEXT("Synthetic.Font");
		Record.CharactersPerPage = 256;
		Record.Baseline = 2;
		Record.LineHeight = 2;
		FWoTFontAtlasPageRecord& Page = Record.Pages.AddDefaulted_GetRef();
		Page.Width = 8;
		Page.Height = 8;
		Page.CharacterCount = 2;
		for (uint32 Index = 0; Index < 2; ++Index)
		{
			Record.CharacterMap.Add({Index, Index});
			FWoTFontGlyphRecord& Glyph = Record.Glyphs.AddDefaulted_GetRef();
			Glyph.Codepoint = Index;
			Glyph.AtlasRect = FIntRect(Index * 2, 0, Index * 2 + 2, 2);
			Glyph.Advance = 2;
		}
		return Record;
	};
	uint32 VisitedGlyphs = 999;
	FWoTFontV2Record Oversized = MakeBoundedRecord();
	Oversized.Pages[0].Width = 16384;
	Oversized.Pages[0].Height = 16384;
	TestTrue(TEXT("oversized font atlas rejects in preflight"),
		ValidateFontV2MaterializationBoundsForTesting(
			Oversized, VisitedGlyphs).IsError());
	TestEqual(TEXT("oversized atlas rejects before glyph work"), VisitedGlyphs, 0u);
	FWoTFontV2Record TooMany = MakeBoundedRecord();
	TooMany.Glyphs.SetNum(65537);
	TooMany.CharacterMap.SetNum(65537);
	TooMany.Pages[0].CharacterCount = 65537;
	VisitedGlyphs = 999;
	TestTrue(TEXT("excessive glyph count rejects in preflight"),
		ValidateFontV2MaterializationBoundsForTesting(
			TooMany, VisitedGlyphs).IsError());
	TestEqual(TEXT("excessive glyph count rejects before glyph work"),
		VisitedGlyphs, 0u);
	FWoTFontV2Record Empty = MakeBoundedRecord();
	Empty.Glyphs.Empty();
	Empty.CharacterMap.Empty();
	Empty.Pages[0].CharacterCount = 0;
	VisitedGlyphs = 999;
	TestTrue(TEXT("empty font rejects in preflight"),
		ValidateFontV2MaterializationBoundsForTesting(
			Empty, VisitedGlyphs).IsError());
	TestEqual(TEXT("empty font rejects before glyph work"), VisitedGlyphs, 0u);
	FWoTFontV2Record ExcessDeclared = MakeBoundedRecord();
	ExcessDeclared.Pages[0].Width = 1;
	ExcessDeclared.Pages[0].Height = 1;
	ExcessDeclared.Pages[0].CharacterCount = 32768;
	FWoTFontAtlasPageRecord& ExcessPage =
		ExcessDeclared.Pages.AddDefaulted_GetRef();
	ExcessPage.Ordinal = 1;
	ExcessPage.Width = 1;
	ExcessPage.Height = 1;
	ExcessPage.CharacterCount = 32769;
	VisitedGlyphs = 999;
	TestTrue(TEXT("aggregate declared glyph count rejects in preflight"),
		ValidateFontV2MaterializationBoundsForTesting(
			ExcessDeclared, VisitedGlyphs).IsError());
	TestEqual(TEXT("aggregate declared glyphs reject before glyph work"),
		VisitedGlyphs, 0u);
	FWoTFontV2Record ZeroLineMetrics = MakeBoundedRecord();
	ZeroLineMetrics.Baseline = 0;
	ZeroLineMetrics.LineHeight = 0;
	VisitedGlyphs = 999;
	TestTrue(TEXT("zero font line metrics reject in preflight"),
		ValidateFontV2MaterializationBoundsForTesting(
			ZeroLineMetrics, VisitedGlyphs).IsError());
	TestEqual(TEXT("zero line metrics reject before glyph work"),
		VisitedGlyphs, 0u);
	FWoTFontV2Record MaximumLineMetrics = MakeBoundedRecord();
	MaximumLineMetrics.Baseline = MAX_uint32;
	MaximumLineMetrics.LineHeight = MAX_uint32;
	VisitedGlyphs = 999;
	TestTrue(TEXT("maximum uint32 font line metrics reject in preflight"),
		ValidateFontV2MaterializationBoundsForTesting(
			MaximumLineMetrics, VisitedGlyphs).IsError());
	TestEqual(TEXT("maximum line metrics reject before glyph work"),
		VisitedGlyphs, 0u);
	FWoTFontV2Record Overlap = MakeBoundedRecord();
	Overlap.Glyphs[1].AtlasRect = FIntRect(1, 0, 3, 2);
	VisitedGlyphs = 999;
	TestTrue(TEXT("overlapping glyph rectangles reject in bounded preflight"),
		ValidateFontV2MaterializationBoundsForTesting(
			Overlap, VisitedGlyphs).IsError());
	TestEqual(TEXT("overlap stops at the second glyph"), VisitedGlyphs, 2u);
	uint32 Codepoint = 0;
	TestTrue(TEXT("font codepoint multiplication overflow rejects"),
		ComposeFontCodepointForTesting(
			MAX_uint32, MAX_uint32, MAX_uint32, Codepoint).IsError());
	TestTrue(TEXT("font surrogate codepoint rejects"),
		ComposeFontCodepointForTesting(0, 65536, 0xd800, Codepoint).IsError());
	TestTrue(TEXT("font codepoint beyond Unicode rejects"),
		ComposeFontCodepointForTesting(17, 65536, 0, Codepoint).IsError());
	TestFalse(TEXT("largest Unicode scalar composes"),
		ComposeFontCodepointForTesting(16, 65536, 0xffff, Codepoint).IsError());
	TestEqual(TEXT("largest Unicode scalar is exact"), Codepoint, 0x10ffffu);

	struct FReject { const TCHAR* Fixture; EWoTFactDomain Domain; };
	const FReject Rejects[] = {
		{TEXT("cache-v3-font-invalid-metric.bin"), EWoTFactDomain::Font},
		{TEXT("cache-v3-registry-case-collision.bin"), EWoTFactDomain::Registry},
		{TEXT("cache-v3-localization-invalid-encoding.bin"), EWoTFactDomain::Localization},
		{TEXT("cache-v3-localization-missing-fallback.bin"), EWoTFactDomain::Localization},
		{TEXT("cache-v3-class-default-unknown-type.bin"), EWoTFactDomain::ClassDefault},
		{TEXT("cache-v3-class-default-unsafe-reference.bin"), EWoTFactDomain::ClassDefault},
		{TEXT("cache-v3-class-default-next-cycle.bin"), EWoTFactDomain::ClassDefault},
		{TEXT("cache-v3-class-default-unresolvable-struct.bin"), EWoTFactDomain::ClassDefault},
		{TEXT("cache-v3-class-default-fixed-array-self-cycle.bin"),
			EWoTFactDomain::ClassDefault},
		{TEXT("cache-v3-class-default-fixed-array-two-node-cycle.bin"),
			EWoTFactDomain::ClassDefault},
		{TEXT("cache-v3-class-default-fixed-array-wrapper-overflow.bin"),
			EWoTFactDomain::ClassDefault},
		{TEXT("cache-v3-class-default-array-index-out-of-range.bin"),
			EWoTFactDomain::ClassDefault},
		{TEXT("cache-v3-class-default-depth-9.bin"), EWoTFactDomain::ClassDefault},
	};
	for (const FReject& Reject : Rejects)
	{
		FWoTRemainingDomainsSpecRoot Root;
		TWoTBootstrapResult<FWoTRetailPackage> Opened =
			WoTRemainingDomainsSpecOpen(Root, Reject.Fixture);
		if (!TestFalse(FString::Printf(TEXT("%s opens"), Reject.Fixture), Opened.IsError()))
			continue;
		FWoTRetailContract Full = WoTRemainingDomainsSpecContract();
		FWoTRetailContract Selected;
		for (const FWoTRequiredFact& Fact : Full.Facts)
			if (Fact.Domain == Reject.Domain) Selected.Facts.Add(Fact);
		FWoTRemainingDomainsSpecWriter Writer;
		FWoTRemainingDomainsSpecProgress Progress;
		TWoTBootstrapResult<TArray<FWoTCacheBlob>> Result =
			Reject.Domain == EWoTFactDomain::Font
			? ImportFontProcedural(Opened.GetValue(), Selected, Writer, Progress)
			: ImportRegistryLocalizationDefaults(
				Opened.GetValue(), Selected, Writer, Progress);
		TestTrue(FString::Printf(TEXT("%s is rejected"), Reject.Fixture), Result.IsError());
	}
	FWoTRemainingDomainsSpecRoot AtlasRoot;
	TWoTBootstrapResult<FWoTRetailPackage> AtlasPackage =
		WoTRemainingDomainsSpecOpen(
			AtlasRoot, TEXT("cache-v3-remaining-importer-input.bin"));
	if (TestFalse(TEXT("valid atlas-binding fixture opens"), AtlasPackage.IsError()))
	{
		const FWoTRetailContract Full = WoTRemainingDomainsSpecContract();
		FWoTRetailContract MissingAtlas;
		MissingAtlas.Facts.Add(Full.Facts[0]);
		FWoTRemainingDomainsSpecWriter MissingWriter;
		FWoTRemainingDomainsSpecProgress MissingProgress;
		TestTrue(TEXT("font-v2 rejects an unresolved atlas logical id"),
			ImportFontProcedural(AtlasPackage.GetValue(), MissingAtlas,
				MissingWriter, MissingProgress).IsError());

		FWoTRetailContract DuplicateAtlas;
		DuplicateAtlas.Facts.Add(Full.Facts[0]);
		DuplicateAtlas.Facts.Add(Full.Facts[1]);
		FWoTRequiredFact Duplicate = Full.Facts[1];
		Duplicate.FactId = TEXT("synthetic.texture.font-atlas.duplicate");
		Duplicate.Outputs[0].LogicalId =
			TEXT("cache/texture/synthetic-atlas-duplicate");
		DuplicateAtlas.Facts.Add(MoveTemp(Duplicate));
		FWoTRemainingDomainsSpecWriter DuplicateWriter;
		FWoTRemainingDomainsSpecProgress DuplicateProgress;
		TestTrue(TEXT("font-v2 rejects an ambiguous atlas binding"),
			ImportFontProcedural(AtlasPackage.GetValue(), DuplicateAtlas,
				DuplicateWriter, DuplicateProgress).IsError());

		FWoTRetailContract WrongAtlasSchema;
		WrongAtlasSchema.Facts.Add(Full.Facts[0]);
		FWoTRequiredFact Wrong = Full.Facts[1];
		Wrong.Outputs[0].Schema = TEXT("wot-font-v3");
		WrongAtlasSchema.Facts.Add(MoveTemp(Wrong));
		FWoTRemainingDomainsSpecWriter WrongWriter;
		FWoTRemainingDomainsSpecProgress WrongProgress;
		TestTrue(TEXT("font-v2 rejects a non-texture atlas declaration"),
			ImportFontProcedural(AtlasPackage.GetValue(), WrongAtlasSchema,
				WrongWriter, WrongProgress).IsError());
	}
	FWoTRetailContract Generic;
	FWoTRetailAuditBinding& Binding = Generic.AuditSet.AddDefaulted_GetRef();
	Binding.RuntimeConsumer = TEXT("UWoTPayloadSubsystem");
	Binding.AuditRule = TEXT("audit.remaining-domain-v1");
	WoTRemainingDomainsSpecAddFact(Generic, EWoTFactDomain::Font,
		TEXT("synthetic.generic.output"), TEXT("Synthetic/remaining.u"),
		TEXT("remaining.FontA"), TEXT("generic-authored-v1"),
		TEXT("cache/generic/forbidden"), TEXT("wot-font-v3"));
	TestTrue(TEXT("a generic authored fact cannot author an output"),
		ValidateFactClosure(Generic).IsError());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRemainingDomainsRetailCollisionSpec,
	"WoT.RetailReader.RemainingDomains.RetailCollision",
	WoTRemainingDomainsSpecFlags)

bool FWoTRemainingDomainsRetailCollisionSpec::RunTest(const FString&)
{
	if (!WoTReferenceTreeExists())
	{
		AddWarning(WoTReferenceMissingWarning(TEXT("original")));
		return true;
	}
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes,
			*WoTRemainingDomainsSpecRetailPath(TEXT("ParticleSystems.u"))))
	{
		AddError(TEXT("required original/System/ParticleSystems.u is missing"));
		return false;
	}
	const FString Identity =
		TEXT("ParticleSystems.Fire.Fire_Torch_001#export-00000575");
	TWoTBootstrapResult<FWoTRemainingPackageInspectionForTesting> Inspection =
		InspectRemainingPackageForTesting(Bytes, TEXT("System/ParticleSystems.u"));
	if (!TestFalse(TEXT("retail ParticleSystems.u inspects"), Inspection.IsError()))
		return false;
	const FWoTRemainingExportForTesting* Torch = nullptr;
	for (const FWoTRemainingExportForTesting& Item :
		Inspection.GetValue().Procedurals)
	{
		if (Item.CanonicalIdentity == Identity) Torch = &Item;
	}
	if (TestNotNull(TEXT("the colliding torch procedural registry row exists"), Torch))
	{
		TestEqual(TEXT("the colliding torch keeps its package-qualified raw path"),
			Torch->FactIdentity,
			FString(TEXT("ParticleSystems.Fire.Fire_Torch_001")));
	}
	FWoTRetailContract Contract;
	WoTRemainingDomainsSpecAddFact(Contract, EWoTFactDomain::Procedural,
		TEXT("retail.procedural.particlesystems.fire.fire_torch_001"),
		TEXT("System/ParticleSystems.u"), *Identity, TEXT("procedural-v1"),
		TEXT("pin/procedural/particle-systems-collision"),
		TEXT("wot-procedural-v3"));
	FWoTRemainingDomainsSpecWriter Writer;
	FWoTRemainingDomainsSpecProgress Progress;
	TWoTBootstrapResult<TArray<FWoTCacheBlob>> Encoded =
		ImportFontProceduralPackageBytesForTesting(Bytes,
			TEXT("System/ParticleSystems.u"), Contract, Writer, Progress);
	if (TestFalse(TEXT("the colliding torch resolves by canonical reference"),
		Encoded.IsError()))
	{
		const FWoTRemainingDomainsSpecWritten* Written = Writer.Find(
			TEXT("pin/procedural/particle-systems-collision"));
		if (TestNotNull(TEXT("the colliding torch object exists"), Written))
		{
			TWoTBootstrapResult<FWoTCanonicalJsonValue> Header =
				WoTRemainingDomainsSpecHeader(Written->Bytes);
			if (TestFalse(TEXT("the colliding torch header parses"), Header.IsError()))
			{
				const FWoTCanonicalJsonValue* Source =
					Header.GetValue().FindMember(TEXT("source"));
				TestEqual(TEXT("the resolved export is the FireTexture row"),
					Header.GetValue().FindMember(TEXT("class_name"))->GetString(),
					FString(TEXT("FireTexture")));
				TestEqual(TEXT("the source retains the reference-owned suffix"),
					Source->FindMember(TEXT("export"))->GetString(), Identity);
			}
		}
	}
	FWoTRetailContract RawContract;
	WoTRemainingDomainsSpecAddFact(RawContract, EWoTFactDomain::Procedural,
		TEXT("retail.procedural.particlesystems.fire_torch_001"),
		TEXT("System/ParticleSystems.u"),
		TEXT("ParticleSystems.Fire.Fire_Torch_001"),
		TEXT("procedural-v1"), TEXT("pin/procedural/raw-collision-row"),
		TEXT("wot-procedural-v3"));
	FWoTRemainingDomainsSpecWriter RawWriter;
	FWoTRemainingDomainsSpecProgress RawProgress;
	TWoTBootstrapResult<TArray<FWoTCacheBlob>> RawEncoded =
		ImportFontProceduralPackageBytesForTesting(Bytes,
			TEXT("System/ParticleSystems.u"), RawContract, RawWriter, RawProgress);
	if (TestFalse(TEXT("the production raw collision resolves by unique procedural class"),
		RawEncoded.IsError()))
	{
		const FWoTRemainingDomainsSpecWritten* Written =
			RawWriter.Find(TEXT("pin/procedural/raw-collision-row"));
		if (TestNotNull(TEXT("the raw collision object exists"), Written))
		{
			TWoTBootstrapResult<FWoTCanonicalJsonValue> Header =
				WoTRemainingDomainsSpecHeader(Written->Bytes);
			if (TestFalse(TEXT("the raw collision header parses"), Header.IsError()))
			{
				const FWoTCanonicalJsonValue* Source =
					Header.GetValue().FindMember(TEXT("source"));
				TestEqual(TEXT("the raw collision emits the canonical source identity"),
					Source->FindMember(TEXT("export"))->GetString(), Identity);
			}
		}
	}
	FWoTRetailContract WrongClass;
	WoTRemainingDomainsSpecAddFact(WrongClass, EWoTFactDomain::Procedural,
		TEXT("retail.procedural.particlesystems.palette-collision"),
		TEXT("System/ParticleSystems.u"),
		TEXT("ParticleSystems.Fire.Fire_Torch_001#export-00000123"),
		TEXT("procedural-v1"), TEXT("pin/procedural/wrong-collision-row"),
		TEXT("wot-procedural-v3"));
	FWoTRemainingDomainsSpecWriter WrongWriter;
	FWoTRemainingDomainsSpecProgress WrongProgress;
	TWoTBootstrapResult<TArray<FWoTCacheBlob>> Rejected =
		ImportFontProceduralPackageBytesForTesting(Bytes,
			TEXT("System/ParticleSystems.u"), WrongClass, WrongWriter, WrongProgress);
	TestTrue(TEXT("the colliding Palette row fails the post-lookup class assertion"),
		Rejected.IsError());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRemainingDomainsRetailPinSpec,
	"WoT.RetailReader.RemainingDomains.RetailPins", WoTRemainingDomainsSpecFlags)

bool FWoTRemainingDomainsRetailPinSpec::RunTest(const FString&)
{
	if (!WoTReferenceTreeExists())
	{
		AddWarning(WoTReferenceMissingWarning(TEXT("original")));
		return true;
	}
	const FString System = FPaths::GetPath(WoTRemainingDomainsSpecRetailPath(TEXT("WOT.u")));
	if (!TestTrue(TEXT("required original/System directory exists"),
		IFileManager::Get().DirectoryExists(*System))) return false;
	TArray<FString> PackageFiles, LocalizationFiles;
	IFileManager::Get().FindFiles(PackageFiles, *(System / TEXT("*.u")), true, false);
	IFileManager::Get().FindFiles(
		LocalizationFiles, *(System / TEXT("*.int")), true, false);
	PackageFiles.Sort();
	LocalizationFiles.Sort();
	TestEqual(TEXT("the retail System package census is complete"),
		PackageFiles.Num(), 20);
	TestEqual(TEXT("all English localization sources are present"),
		LocalizationFiles.Num(), 29);
	TArray<FWoTRemainingDomainsSpecAggregateRecord> Fonts, Procedurals;
	TArray<FWoTRemainingDomainsSpecAggregateRecord> Registries, Defaults;
	TArray<FWoTRemainingDomainsSpecAggregateRecord> Localizations;
	TMap<FString, FString> MemberCensusDigests;
	TMap<FString, uint32> DecodedStructMembers;
	const FString DumpRoot = FPlatformMisc::GetEnvironmentVariable(
		TEXT("WOT_CLASS_DEFAULT_DUMP_ROOT"));
	if (!DumpRoot.IsEmpty())
	{
		TestTrue(TEXT("the class-default dump directory can be created"),
			IFileManager::Get().MakeDirectory(*DumpRoot, true));
	}
	FWoTRetailContract AggregateContract;
	TArray<FWoTRemainingSourceBytesForTesting> Sources;
	uint32 ClassCount = 0;
	// v2 procedural objects resolve foreign palette/source references only
	// through packages the contract names (WoTTraps.FireWall.* palettes live in
	// ParticleSystems.u); every per-package contract therefore carries the
	// registry fact of every System package, and a sibling loader serves them.
	FWoTRemainingSiblingLoaderForTesting SiblingLoader =
		[System](FStringView Wanted) -> TWoTBootstrapResult<TArray<uint8>>
	{
		using FBytesResult = TWoTBootstrapResult<TArray<uint8>>;
		const FString Path(Wanted);
		TArray<uint8> Bytes;
		if (!Path.StartsWith(TEXT("System/"), ESearchCase::CaseSensitive)
			|| Path.Contains(TEXT(".."))
			|| !FFileHelper::LoadFileToArray(Bytes, *(System / Path.Mid(7))))
		{
			return FBytesResult::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::UnknownField,
				TEXT("the retail pin sibling package is absent"), Path));
		}
		return FBytesResult::Success(MoveTemp(Bytes));
	};
	for (const FString& Filename : PackageFiles)
	{
		TArray<uint8> Bytes;
		if (!TestTrue(Filename + TEXT(" loads"),
			FFileHelper::LoadFileToArray(Bytes, *(System / Filename)))) continue;
		const FString Stem = FPaths::GetBaseFilename(Filename);
		const FString SourcePath = TEXT("System/") + Filename;
		TWoTBootstrapResult<FWoTRemainingPackageInspectionForTesting> Inspection =
			InspectRemainingPackageForTesting(Bytes, SourcePath);
		if (!TestFalse(Filename + TEXT(" inspects"), Inspection.IsError()))
		{
			AddError(Filename + TEXT(": ") + Inspection.GetError().ToString());
			continue;
		}
		ClassCount += Inspection.GetValue().ClassCount;
		FWoTRetailContract PackageContract;
		for (const FString& Sibling : PackageFiles)
		{
			const FString SiblingStem = FPaths::GetBaseFilename(Sibling);
			WoTRemainingDomainsSpecAddFact(PackageContract, EWoTFactDomain::Registry,
				*(TEXT("retail.registry.") + SiblingStem.ToLower()),
				*(TEXT("System/") + Sibling), *SiblingStem, TEXT("registry-v1"),
				*(TEXT("pin/registry/") + SiblingStem), TEXT("wot-registry-v3"));
		}
		for (const FWoTRemainingExportForTesting& Item : Inspection.GetValue().Textures)
		{
			const FString FactId = TEXT("retail.texture.")
				+ Item.CanonicalIdentity.ToLower();
			const FString LogicalId = TEXT("pin/texture/") + Item.CanonicalIdentity;
			WoTRemainingDomainsSpecAddFact(PackageContract, EWoTFactDomain::Texture,
				*FactId, *SourcePath, *Item.CanonicalIdentity, TEXT("texture-v2"),
				*LogicalId, TEXT("wot-texture-v3"));
		}
		for (const FWoTRemainingExportForTesting& Item : Inspection.GetValue().Fonts)
		{
			const FString FactId = TEXT("retail.font.")
				+ Item.CanonicalIdentity.ToLower();
			const FString LogicalId = TEXT("pin/font/") + Item.CanonicalIdentity;
			WoTRemainingDomainsSpecAddFact(PackageContract, EWoTFactDomain::Font,
				*FactId, *SourcePath, *Item.CanonicalIdentity, TEXT("font-v2"),
				*LogicalId, TEXT("wot-font-v3"));
		}
		for (const FWoTRemainingExportForTesting& Item :
			Inspection.GetValue().Procedurals)
		{
			const FString FactId = TEXT("retail.procedural.")
				+ Item.CanonicalIdentity.ToLower();
			const FString LogicalId = TEXT("pin/procedural/") + Item.CanonicalIdentity;
			WoTRemainingDomainsSpecAddFact(PackageContract,
				EWoTFactDomain::Procedural, *FactId, *SourcePath,
				*Item.CanonicalIdentity, TEXT("procedural-v1"), *LogicalId,
				TEXT("wot-procedural-v3"));
		}
		FWoTRemainingDomainsSpecWriter PackageWriter;
		FWoTRemainingDomainsSpecProgress PackageProgress;
		TWoTBootstrapResult<TArray<FWoTCacheBlob>> PackageResult =
			ImportFontProceduralPackageBytesForTesting(
				Bytes, SourcePath, PackageContract, PackageWriter, PackageProgress,
				SiblingLoader);
		if (TestFalse(Filename + TEXT(" encodes every font/procedural"),
			PackageResult.IsError()))
		{
			for (const FWoTRemainingExportForTesting& Item : Inspection.GetValue().Fonts)
			{
				const FWoTRemainingDomainsSpecWritten* Written = PackageWriter.Find(
					TEXT("pin/font/") + Item.CanonicalIdentity);
				if (TestNotNull(TEXT("full-set font object exists"), Written))
					Fonts.Add(FWoTRemainingDomainsSpecAggregateRecord{
						Item.CanonicalIdentity, Written->Bytes});
			}
			for (const FWoTRemainingExportForTesting& Item :
				Inspection.GetValue().Procedurals)
			{
				const FWoTRemainingDomainsSpecWritten* Written = PackageWriter.Find(
					TEXT("pin/procedural/") + Item.CanonicalIdentity);
				if (TestNotNull(TEXT("full-set procedural object exists"), Written))
					Procedurals.Add(FWoTRemainingDomainsSpecAggregateRecord{
						Item.CanonicalIdentity, Written->Bytes});
			}
		}
		else
		{
			AddError(Filename + TEXT(": ") + PackageResult.GetError().ToString());
		}
		FWoTRemainingSourceBytesForTesting& Source = Sources.AddDefaulted_GetRef();
		Source.SourcePath = SourcePath;
		Source.Bytes = MoveTemp(Bytes);
		const FString RegistryLogical = TEXT("pin/registry/") + Stem;
		const FString DefaultsLogical = TEXT("pin/defaults/") + Stem;
		const FString RegistryFact = TEXT("retail.registry.") + Stem.ToLower();
		const FString DefaultsFact = TEXT("retail.class-default.") + Stem.ToLower();
		WoTRemainingDomainsSpecAddFact(AggregateContract, EWoTFactDomain::Registry,
			*RegistryFact, *SourcePath, *Stem, TEXT("registry-v1"),
			*RegistryLogical, TEXT("wot-registry-v3"));
		WoTRemainingDomainsSpecAddFact(AggregateContract,
			EWoTFactDomain::ClassDefault, *DefaultsFact, *SourcePath, *Stem,
			TEXT("class-default-v2"), *DefaultsLogical,
			TEXT("wot-class-default-v3"));
	}
	for (const FString& Filename : LocalizationFiles)
	{
		FWoTRemainingSourceBytesForTesting& Source = Sources.AddDefaulted_GetRef();
		Source.SourcePath = TEXT("System/") + Filename;
		if (!TestTrue(Filename + TEXT(" loads"), FFileHelper::LoadFileToArray(
			Source.Bytes, *(System / Filename)))) continue;
		const FString Stem = FPaths::GetBaseFilename(Filename);
		const FString FactId = TEXT("retail.localization.") + Stem.ToLower();
		const FString LogicalId = TEXT("pin/localization/") + Filename;
		WoTRemainingDomainsSpecAddFact(AggregateContract,
			EWoTFactDomain::Localization, *FactId, *Source.SourcePath, *Filename,
			TEXT("localization-v1"), *LogicalId, TEXT("wot-localization-v3"));
	}
	FWoTRemainingDomainsSpecWriter AggregateWriter;
	FWoTRemainingDomainsSpecProgress AggregateProgress;
	TWoTBootstrapResult<TArray<FWoTCacheBlob>> AggregateResult =
		ImportRegistryLocalizationDefaultsSourcesForTesting(
			Sources, AggregateContract, AggregateWriter, AggregateProgress);
	if (TestFalse(TEXT("all package/localization aggregates import"),
		AggregateResult.IsError()))
	{
		for (const FString& Filename : PackageFiles)
		{
			const FString Stem = FPaths::GetBaseFilename(Filename);
			const FWoTRemainingDomainsSpecWritten* Registry =
				AggregateWriter.Find(TEXT("pin/registry/") + Stem);
			const FWoTRemainingDomainsSpecWritten* Default =
				AggregateWriter.Find(TEXT("pin/defaults/") + Stem);
			if (TestNotNull(TEXT("full-set registry exists"), Registry))
				Registries.Add(FWoTRemainingDomainsSpecAggregateRecord{
					Stem, Registry->Bytes});
			if (TestNotNull(TEXT("full-set defaults exist"), Default))
			{
				Defaults.Add(FWoTRemainingDomainsSpecAggregateRecord{
					Stem, Default->Bytes});
				FWoTRemainingDomainsSpecMemberCensus Census;
				if (TestTrue(Stem + TEXT(" class-default member census parses"),
					WoTRemainingDomainsSpecClassDefaultCensus(Default->Bytes, Census)))
				{
					MemberCensusDigests.Add(Stem,
						WoTRemainingDomainsSpecMemberCensusDigest(Census));
					for (const TPair<FString, uint32>& Pair :
						Census.ImmediateMembersByStruct)
					{
						DecodedStructMembers.FindOrAdd(Pair.Key) += Pair.Value;
					}
				}
				if (!DumpRoot.IsEmpty())
				{
					TestTrue(Stem + TEXT(" class-default dump writes"),
						FFileHelper::SaveArrayToFile(Default->Bytes,
							*(DumpRoot / (Stem + TEXT(".bin")))));
				}
			}
		}
		for (const FString& Filename : LocalizationFiles)
		{
			const FWoTRemainingDomainsSpecWritten* Written =
				AggregateWriter.Find(TEXT("pin/localization/") + Filename);
			if (TestNotNull(TEXT("full-set localization exists"), Written))
				Localizations.Add(FWoTRemainingDomainsSpecAggregateRecord{
					Filename, Written->Bytes});
		}
	}
	else
	{
		AddError(AggregateResult.GetError().ToString());
	}
	TestEqual(TEXT("all 17 fonts are canonically encoded"), Fonts.Num(), 17);
	TestEqual(TEXT("all 37 procedural textures are canonically encoded"),
		Procedurals.Num(), 37);
	TestEqual(TEXT("all 1655 Class identities close"), ClassCount, uint32(1655));
	TestEqual(TEXT("every package registry is encoded"), Registries.Num(), 20);
	TestEqual(TEXT("every package class-default set is encoded"), Defaults.Num(), 20);
	TestEqual(TEXT("every package has a class-default member census"),
		MemberCensusDigests.Num(), 20);
	TestEqual(TEXT("all 29 localization files are encoded"), Localizations.Num(), 29);
	const TCHAR* RequiredStructs[] = {
		TEXT("AnimInfoT"), TEXT("SoundInfoT"), TEXT("SoundSlotTimerT"),
		TEXT("BodyPartT"), TEXT("EffectInfoT"), TEXT("PlayerTroopMap"),
		TEXT("SizeException"), TEXT("TDefensiveResponse"),
		TEXT("EngineAnimInfoT"), TEXT("EngineSoundInfoT")};
	for (const TCHAR* StructName : RequiredStructs)
	{
		const uint32* Count = DecodedStructMembers.Find(
			WoTRemainingFold(StructName));
		TestTrue(FString::Printf(TEXT("%s reflected members are decoded"), StructName),
			Count != nullptr && *Count > 0);
	}
	const TPair<const TCHAR*, const TCHAR*> ExpectedMemberCensus[] = {
		{TEXT("Angreal"), TEXT("3d8011e84d68befecd22922642ca49b3b431f6b77d7a96c3c5e6d51a61a9eabf")},
		{TEXT("Core"), TEXT("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")},
		{TEXT("Editor"), TEXT("94a5c27e8cb300774d5c7e50e49435f9e4053510250d9250170424ac0b6a8522")},
		{TEXT("Engine"), TEXT("e6f8956336aa6f1cd6ecd977b8e0a0c25b5b3502df0a932dd38d469af486e825")},
		{TEXT("Environment"), TEXT("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")},
		{TEXT("Fire"), TEXT("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")},
		{TEXT("IpDrv"), TEXT("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")},
		{TEXT("IpServer"), TEXT("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")},
		{TEXT("Legend"), TEXT("d24760b7a6bf35de12d38c554295dd0353f240512b680501f3880944de2846dd")},
		{TEXT("MachinShin"), TEXT("14fac3373505bd4f92223a0f20cabe382048dda4ec2acd04ca4e034cd9551d3e")},
		{TEXT("ParticleSystems"), TEXT("4e47e35ac02198c5601b017e81ee16c751648628db356e5498f920e95d6ff54c")},
		{TEXT("UBrowser"), TEXT("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")},
		{TEXT("UWindow"), TEXT("4ea3dfae96a6214b3529014a758f6919254f9515e5d01ce724e255c82e18ec52")},
		{TEXT("WinDrv"), TEXT("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")},
		{TEXT("WOT"), TEXT("a9256d20e6d98faff6855c5d49b9f09a25ddc5f529591fd2dcb7a4fb66584500")},
		{TEXT("WoTBase"), TEXT("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")},
		{TEXT("WoTBrowser"), TEXT("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")},
		{TEXT("WoTDecorations"), TEXT("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")},
		{TEXT("WoTPawns"), TEXT("8d8b4ceebb0a9e75d3641cc250263e4d2388d7f52f22d587efb3af2854427121")},
		{TEXT("WoTTraps"), TEXT("1e7f8bb70af555db7b3dbee3b1abdba636dfd8a9d0bf28524e7d83a46ce07249")},
	};
	for (const TPair<const TCHAR*, const TCHAR*>& Pin : ExpectedMemberCensus)
	{
		const FString* Actual = MemberCensusDigests.Find(Pin.Key);
		TestEqual(FString(Pin.Key) + TEXT(" member census pin"),
			Actual == nullptr ? FString() : *Actual, FString(Pin.Value));
	}
	bool bAllFontsAreCompleteV2 = true;
	for (const FWoTRemainingDomainsSpecAggregateRecord& Font : Fonts)
	{
		TWoTBootstrapResult<FWoTCanonicalJsonValue> Header =
			WoTRemainingDomainsSpecHeader(Font.Bytes);
		FString Encoding;
		uint32 CharacterMapCount = 0, GlyphCount = 0, PageCount = 0;
		const FWoTCanonicalJsonValue* Pages = Header.IsError() ? nullptr
			: Header.GetValue().FindMember(TEXT("pages"));
		bAllFontsAreCompleteV2 = bAllFontsAreCompleteV2 && !Header.IsError()
			&& !ReadStringField(Header.GetValue(), TEXT("encoding"),
				TEXT("retail font-v2"), Encoding).IsSet()
			&& Encoding == TEXT("ue1-font-pages-v2")
			&& !ReadUnsigned32Field(Header.GetValue(), TEXT("character_map_count"),
				TEXT("retail font-v2"), CharacterMapCount).IsSet()
			&& !ReadUnsigned32Field(Header.GetValue(), TEXT("glyph_count"),
				TEXT("retail font-v2"), GlyphCount).IsSet()
			&& !ReadUnsigned32Field(Header.GetValue(), TEXT("page_count"),
				TEXT("retail font-v2"), PageCount).IsSet()
			&& CharacterMapCount == GlyphCount && GlyphCount > 0
			&& Pages != nullptr && Pages->IsArray()
			&& PageCount == static_cast<uint32>(Pages->Num()) && PageCount > 0;
	}
	TestTrue(TEXT("all retail fonts carry complete semantic font-v2 census"),
		bAllFontsAreCompleteV2);
	TestEqual(TEXT("full procedural domain aggregate pin"),
		WoTRemainingDomainsSpecAggregateHash("procedurals", MoveTemp(Procedurals)),
		FString(TEXT("0f518523ca57706856558db522c01491fbec52271b669ef233a4b4bcfdff20de")));
	TestEqual(TEXT("full registry domain aggregate pin"),
		WoTRemainingDomainsSpecAggregateHash("registries", MoveTemp(Registries)),
		FString(TEXT("dce18ed08ca9a097aba31b8a7a0c9719597e006da3a8fc52195a4fa427ea3445")));
	TestEqual(TEXT("full class-default domain aggregate pin"),
		WoTRemainingDomainsSpecAggregateHash("class_defaults", MoveTemp(Defaults)),
		FString(TEXT("36b788aadd10bda3643f084ee7aed973b74666d8808a40b0ddbc1a44225560b2")));
	TestEqual(TEXT("full localization domain aggregate pin"),
		WoTRemainingDomainsSpecAggregateHash("localizations", MoveTemp(Localizations)),
		FString(TEXT("38c493d1be2628b52d5a4ae56927d992e800bf1586be0da475c9c43357a60d92")));
	return !HasAnyErrors();
}
#endif
