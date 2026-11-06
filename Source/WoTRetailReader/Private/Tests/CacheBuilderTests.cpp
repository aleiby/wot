// Fresh-build coordination, production closure, parity, and real-package pins.
#include "Misc/AutomationTest.h"

#include "Containers/StringConv.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "WoTBootstrapCore/CanonicalJson.h"
#include "WoTBootstrapCore/Hash.h"
#include "WoTRetailReader/BSPWorld.h"
#include "WoTRetailReader/CacheBuilder.h"
#include "WoTRetailReader/Generated/RequiredWorlds.generated.h"
#include "WoTRetailReader/Testing/ReferencePaths.h"

#if WITH_AUTOMATION_TESTS
namespace
{
constexpr EAutomationTestFlags WoTCacheBuilderSpecFlags =
	EAutomationTestFlags::ProgramContext | EAutomationTestFlags::EditorContext
	| EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter;

FString WoTCacheBuilderSpecRepoPath(const TCHAR* Relative)
{
	return WoTRepositoryOrReferencePath(Relative);
}

bool WoTCacheBuilderSpecLoad(const TCHAR* Relative, TArray<uint8>& Out)
{
	return FFileHelper::LoadFileToArray(Out, *WoTCacheBuilderSpecRepoPath(Relative));
}

bool WoTCacheBuilderSpecLoadProductionDescriptor(TArray<uint8>& Out)
{
	TWoTBootstrapResult<TArray<uint8>> Loaded =
		LoadCacheProductionDescriptorForTesting();
	if (Loaded.IsError()) return false;
	Out = MoveTemp(Loaded).GetValue();
	return true;
}

struct FWoTCacheBuilderSpecWritten
{
	FString Schema;
	TArray<uint8> Bytes;
	int32 Writes = 0;
};

class FWoTCacheBuilderSpecWriter final : public IWoTObjectWriter
{
public:
	virtual FWoTBootstrapStatus WriteObject(FStringView LogicalId,
		FStringView Schema, TConstArrayView<uint8> Bytes,
		const FWoTSha256& Sha256Digest, uint64 MaximumBytes) override
	{
		if (static_cast<uint64>(Bytes.Num()) > MaximumBytes)
		{
			return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::SizeLimit, TEXT("cache-builder test object overflow")));
		}
		if (Sha256Digest != Sha256(Bytes))
		{
			return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("cache-builder producer digest disagrees with its bytes"),
				FString(LogicalId)));
		}
		FWoTCacheBuilderSpecWritten& Object = Objects.FindOrAdd(FString(LogicalId));
		const bool bSameBytes = Object.Bytes.Num() == Bytes.Num()
			&& (Bytes.IsEmpty() || FMemory::Memcmp(
				Object.Bytes.GetData(), Bytes.GetData(), Bytes.Num()) == 0);
		if (Object.Writes != 0
			&& (Object.Schema != Schema || !bSameBytes))
		{
			return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("a repeated test write changed bytes"), FString(LogicalId)));
		}
		Object.Schema = FString(Schema);
		Object.Bytes.Reset();
		Object.Bytes.Append(Bytes.GetData(), Bytes.Num());
		++Object.Writes;
		return FWoTBootstrapStatus::Success();
	}

	int32 Writes(FStringView LogicalId) const
	{
		const FWoTCacheBuilderSpecWritten* Object = Objects.Find(FString(LogicalId));
		return Object == nullptr ? 0 : Object->Writes;
	}

	int32 TotalWrites() const
	{
		int32 Total = 0;
		for (const TPair<FString, FWoTCacheBuilderSpecWritten>& Pair : Objects)
			Total += Pair.Value.Writes;
		return Total;
	}

	TMap<FString, FWoTCacheBuilderSpecWritten> Objects;
};

class FWoTCacheBuilderSpecProgress final : public IWoTProgressSink
{
public:
	explicit FWoTCacheBuilderSpecProgress(bool bInStageScoped = false)
		: bStageScoped(bInStageScoped) {}

	virtual FWoTBootstrapStatus ReportProgress(FStringView Stage,
		uint64 Completed, uint64 Total) override
	{
		uint64& Last = bStageScoped
			? LastCompletedByStage.FindOrAdd(FString(Stage)) : LastCompleted;
		if (Completed < Last || Completed > Total)
		{
			return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::InvalidNumber,
				TEXT("cache-builder progress was not monotonic"), FString(Stage)));
		}
		Last = Completed;
		if (!bStageScoped) LastCompleted = Completed;
		LastTotal = Total;
		Reports.Add(Completed);
		return FWoTBootstrapStatus::Success();
	}
	virtual bool IsCancellationRequested() const override { return bCancelled; }

	uint64 LastCompleted = 0;
	uint64 LastTotal = 0;
	TArray<uint64> Reports;
	bool bCancelled = false;
	// Raw importers restart progress at zero per substage; their direct tests
	// opt into this map while coordinator sinks retain one global counter.
	TMap<FString, uint64> LastCompletedByStage;
	bool bStageScoped = false;
};

struct FWoTCacheBuilderSpecImporterDescription
{
	EWoTFactDomain Domain;
	const TCHAR* DomainName;
	const TCHAR* Importer;
	const TCHAR* Schema;
	const TCHAR* Consumer;
	const TCHAR* Audit;
	uint64 Maximum;
};

constexpr FWoTCacheBuilderSpecImporterDescription
WoTCacheBuilderSpecImporterDescriptions[] = {
	{EWoTFactDomain::Texture, TEXT("texture"), TEXT("texture-v2"),
		TEXT("wot-texture-v3"), TEXT("UWoTPayloadSubsystem"),
		TEXT("audit.texture-payload-v1"), 268435456ull},
	{EWoTFactDomain::Sound, TEXT("sound"), TEXT("sound-v1"),
		TEXT("wot-sound-v3"), TEXT("UWoTPayloadSubsystem"),
		TEXT("audit.sound-payload-v1"), 268435456ull},
	{EWoTFactDomain::Mesh, TEXT("mesh"), TEXT("mesh-v1"),
		TEXT("wot-mesh-v3"), TEXT("UWoTPayloadSubsystem"),
		TEXT("audit.mesh-payload-v1"), 536870912ull},
	{EWoTFactDomain::Animation, TEXT("animation"), TEXT("animation-v1"),
		TEXT("wot-animation-v3"), TEXT("UWoTVertexAnimComponent"),
		TEXT("audit.animation-payload-v1"), 536870912ull},
	{EWoTFactDomain::BSP, TEXT("bsp"), TEXT("bsp-v1"), TEXT("wot-bsp-v3"),
		TEXT("UWoTPayloadSubsystem"), TEXT("audit.bsp-payload-v1"), 1073741824ull},
	{EWoTFactDomain::World, TEXT("world"), TEXT("world-v1"),
		TEXT("wot-world-v3"), TEXT("UWoTPayloadSubsystem"),
		TEXT("audit.world-payload-v1"), 1073741824ull},
	{EWoTFactDomain::Font, TEXT("font"), TEXT("font-v2"), TEXT("wot-font-v3"),
		TEXT("FWoTFrontEndFontOwner"), TEXT("audit.font-payload-v2"), 268435456ull},
	{EWoTFactDomain::FrontEndWorld, TEXT("front_end_world"),
		TEXT("front-end-world-v1"), TEXT("wot-front-end-world-v3"),
		TEXT("FWoTCacheGenerationLease"),
		TEXT("audit.front-end-world-payload-v1"), 4194304ull},
	{EWoTFactDomain::Procedural, TEXT("procedural"), TEXT("procedural-v1"),
		TEXT("wot-procedural-v3"), TEXT("UWoTFractalSubsystem"),
		TEXT("audit.procedural-payload-v1"), 268435456ull},
	{EWoTFactDomain::Registry, TEXT("registry"), TEXT("registry-v1"),
		TEXT("wot-registry-v3"), TEXT("UWoTPayloadSubsystem"),
		TEXT("audit.registry-payload-v1"), 268435456ull},
	{EWoTFactDomain::Localization, TEXT("localization"), TEXT("localization-v1"),
		TEXT("wot-localization-v3"), TEXT("FWoTLocalizationRegistry"),
		TEXT("audit.localization-payload-v1"), 67108864ull},
	{EWoTFactDomain::ClassDefault, TEXT("class_default"), TEXT("class-default-v2"),
		TEXT("wot-class-default-v3"), TEXT("FWoTWorldMaterializer"),
		TEXT("audit.class-default-payload-v1"), 268435456ull},
};

void WoTCacheBuilderSpecAddAudit(FWoTRetailContract& Contract,
	const TCHAR* Consumer, const TCHAR* Audit)
{
	for (const FWoTRetailAuditBinding& Existing : Contract.AuditSet)
	{
		if (Existing.RuntimeConsumer == Consumer && Existing.AuditRule == Audit) return;
	}
	Contract.AuditSet.Add(FWoTRetailAuditBinding{Consumer, Audit});
}

void WoTCacheBuilderSpecAddFact(FWoTRetailContract& Contract,
	const FWoTCacheBuilderSpecImporterDescription& Description,
	FString FactId, FString SourcePath, FString SourceExport, FString LogicalId)
{
	WoTCacheBuilderSpecAddAudit(Contract, Description.Consumer, Description.Audit);
	FWoTRequiredFact& Fact = Contract.Facts.AddDefaulted_GetRef();
	Fact.Domain = Description.Domain;
	Fact.FactId = MoveTemp(FactId);
	Fact.SourcePath = MoveTemp(SourcePath);
	Fact.SourceExport = MoveTemp(SourceExport);
	Fact.Importer = Description.Importer;
	Fact.RuntimeConsumer = Description.Consumer;
	Fact.AuditRule = Description.Audit;
	FWoTRequiredFactOutput& Output = Fact.Outputs.AddDefaulted_GetRef();
	Output.LogicalId = MoveTemp(LogicalId);
	Output.Schema = Description.Schema;
	Output.MaxDecodedSize = Description.Maximum;
}

void WoTCacheBuilderSpecSetStartupCritical(FWoTRetailContract& Contract,
	const TCHAR* LogicalId, const TCHAR* DigestHex)
{
	Contract.StartupCriticalLogicalIds = {LogicalId};
	TWoTBootstrapResult<FWoTSha256> Digest = FWoTSha256::FromHex(DigestHex);
	if (!Digest.IsError())
	{
		Contract.StartupCriticalLogicalIdsSha256 = Digest.GetValue();
	}
}

FWoTRetailContract WoTCacheBuilderSpecSyntheticContract()
{
	FWoTRetailContract Contract;
	Contract.Format = TEXT("wot-cache-contract-v3");
	Contract.ContractVersion = 3;
	Contract.ContractId = TEXT("task25-synthetic");
	Contract.KeyId = TEXT("task25-test-key");
	Contract.ReleaseSequence = 25;
	for (const FWoTCacheBuilderSpecImporterDescription& Description :
		WoTCacheBuilderSpecImporterDescriptions)
	{
		const FString Domain = Description.DomainName;
		WoTCacheBuilderSpecAddFact(Contract, Description,
			TEXT("synthetic.") + Domain, TEXT("Synthetic/") + Domain + TEXT(".u"),
			TEXT("Synthetic.") + Domain, TEXT("cache/") + Domain + TEXT("/synthetic"));
	}
	for (FWoTRequiredFact& Fact : Contract.Facts)
	{
		if (Fact.Domain == EWoTFactDomain::BSP)
		{
			FWoTRequiredFactOutput& Part = Fact.Outputs.AddDefaulted_GetRef();
			Part.LogicalId = TEXT("cache/bsp/synthetic/part-2");
			Part.Schema = TEXT("wot-bsp-v3");
			Part.MaxDecodedSize = 1073741824ull;
			break;
		}
	}
	WoTCacheBuilderSpecSetStartupCritical(Contract,
		TEXT("cache/localization/synthetic"),
		TEXT("34234bed9db7c488e10e84c5e196cac5621ff22e5a0c7df0838a01978703fafa"));
	return Contract;
}

int32 WoTCacheBuilderSpecOutputCount(const FWoTRetailContract& Contract)
{
	int32 Count = 0;
	for (const FWoTRequiredFact& Fact : Contract.Facts)
	{
		Count += Fact.Outputs.Num();
	}
	return Count;
}

FString WoTCacheBuilderSpecStage(FStringView Importer)
{
	if (Importer == TEXT("texture-v2") || Importer == TEXT("sound-v1"))
		return TEXT("texture-sound");
	if (Importer == TEXT("mesh-v1") || Importer == TEXT("animation-v1"))
		return TEXT("mesh-animation");
	if (Importer == TEXT("bsp-v1") || Importer == TEXT("world-v1"))
		return TEXT("required-world-set");
	if (Importer == TEXT("front-end-world-v1"))
		return TEXT("front-end-world");
	if (Importer == TEXT("font-v2") || Importer == TEXT("procedural-v1"))
		return TEXT("font-procedural");
	return TEXT("registry-localization-defaults");
}

TArray<uint8> WoTCacheBuilderSpecObjectBytes(const FWoTRequiredFact& Fact)
{
	const FString Text = TEXT("task25-object:") + Fact.FactId + TEXT(":")
		+ Fact.SourcePath + TEXT(":") + Fact.SourceExport;
	const FTCHARToUTF8 Utf8(*Text);
	TArray<uint8> Bytes;
	Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	return Bytes;
}

struct FWoTCacheBuilderSpecImportMetrics
{
	TMap<FString, int32> SourceLoadsByFact;
	TMap<FString, int32> InvocationsByStage;
};

TArray<FWoTCacheImporterForTesting> WoTCacheBuilderSpecImporters(
	const FWoTRetailContract& Contract,
	FWoTCacheBuilderSpecImportMetrics* Metrics = nullptr)
{
	static const struct
	{
		const TCHAR* Stage;
		const TCHAR* Importers[3];
		int32 Count;
	} Groups[] = {
		{TEXT("texture-sound"), {TEXT("texture-v2"), TEXT("sound-v1")}, 2},
		{TEXT("mesh-animation"), {TEXT("mesh-v1"), TEXT("animation-v1")}, 2},
		{TEXT("required-world-set"), {TEXT("bsp-v1"), TEXT("world-v1")}, 2},
		{TEXT("front-end-world"), {TEXT("front-end-world-v1")}, 1},
		{TEXT("font-procedural"), {TEXT("font-v2"), TEXT("procedural-v1")}, 2},
		{TEXT("registry-localization-defaults"),
			{TEXT("registry-v1"), TEXT("localization-v1"), TEXT("class-default-v2")}, 3},
	};
	TArray<FWoTCacheImporterForTesting> Result;
	for (const auto& Group : Groups)
	{
		FWoTCacheImporterForTesting& Importer = Result.AddDefaulted_GetRef();
		Importer.Stage = Group.Stage;
		for (int32 Index = 0; Index < Group.Count; ++Index)
			Importer.Importers.Add(Group.Importers[Index]);
		const FString Stage = Importer.Stage;
		Importer.Run = [&Contract, Stage, Metrics](IWoTObjectWriter& Writer,
			IWoTProgressSink& Progress)
		{
			using FResult = TWoTBootstrapResult<TArray<FWoTCacheBlob>>;
			if (Metrics != nullptr)
			{
				++Metrics->InvocationsByStage.FindOrAdd(Stage);
			}
			TArray<const FWoTRequiredFact*> Facts;
			for (const FWoTRequiredFact& Fact : Contract.Facts)
			{
				if (WoTCacheBuilderSpecStage(Fact.Importer) == Stage)
				{
					Facts.Add(&Fact);
				}
			}
			FWoTBootstrapStatus Initial = Progress.ReportProgress(Stage, 0, Facts.Num());
			if (Initial.IsError()) return FResult::Error(MoveTemp(Initial).GetError());
			TArray<FWoTCacheBlob> Blobs;
			for (int32 FactIndex = 0; FactIndex < Facts.Num(); ++FactIndex)
			{
				const FWoTRequiredFact& Fact = *Facts[FactIndex];
				if (Metrics != nullptr)
				{
					++Metrics->SourceLoadsByFact.FindOrAdd(Fact.FactId);
				}
				TArray<uint8> Bytes = WoTCacheBuilderSpecObjectBytes(Fact);
				for (const FWoTRequiredFactOutput& Output : Fact.Outputs)
				{
					FWoTBootstrapStatus Written = Writer.WriteObject(Output.LogicalId,
						Output.Schema, Bytes, Sha256(Bytes), Output.MaxDecodedSize);
					if (Written.IsError())
						return FResult::Error(MoveTemp(Written).GetError());
					FWoTCacheBlob& Blob = Blobs.AddDefaulted_GetRef();
					Blob.LogicalId = Output.LogicalId;
					Blob.Schema = Output.Schema;
					Blob.Sha256 = Sha256(Bytes);
					Blob.Size = static_cast<uint64>(Bytes.Num());
					Blob.Satisfies.Add(FWoTRequiredFactId{Fact.FactId});
				}
				FWoTBootstrapStatus Reported = Progress.ReportProgress(
					Stage, FactIndex + 1, Facts.Num());
				if (Reported.IsError())
					return FResult::Error(MoveTemp(Reported).GetError());
			}
			return FResult::Success(MoveTemp(Blobs));
		};
	}
	return Result;
}

TMap<FString, uint64> WoTCacheBuilderSpecSyntheticSizes(
	const FWoTRetailContract& Contract)
{
	TMap<FString, uint64> Sizes;
	for (const FWoTRequiredFact& Fact : Contract.Facts)
		Sizes.Add(Fact.SourcePath, 100 + static_cast<uint64>(Sizes.Num()));
	return Sizes;
}

FWoTSha256 WoTCacheBuilderSpecInstallerSha()
{
	static const uint8 Bytes[] = {1, 3, 3, 7, 25};
	return Sha256(MakeArrayView(Bytes));
}


TArray<uint8> WoTCacheBuilderSpecRemovedDescriptorEntry(
	TConstArrayView<uint8> Descriptor, FStringView Importer, bool bRemoveEdge)
{
	const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Descriptor.GetData()),
		Descriptor.Num());
	FString Text(Converted.Length(), Converted.Get());
	const FString Needle = TEXT("\"importer\":\"") + FString(Importer) + TEXT("\"");
	const int32 ImporterAt = Text.Find(Needle, ESearchCase::CaseSensitive);
	if (ImporterAt == INDEX_NONE) return {};
	const int32 ObjectStart = Text.Find(TEXT("{"), ESearchCase::CaseSensitive,
		ESearchDir::FromEnd, ImporterAt);
	const int32 ObjectEnd = Text.Find(TEXT("}"), ESearchCase::CaseSensitive,
		ESearchDir::FromStart, ImporterAt);
	if (ObjectStart == INDEX_NONE || ObjectEnd == INDEX_NONE) return {};
	if (bRemoveEdge)
	{
		const FString Field = TEXT(",\"runtime_consumer\":");
		const int32 FieldAt = Text.Find(Field, ESearchCase::CaseSensitive,
			ESearchDir::FromStart, ObjectStart);
		if (FieldAt == INDEX_NONE || FieldAt > ObjectEnd) return {};
		const int32 ValueStart = FieldAt + Field.Len();
		const int32 ValueEnd = Text.Find(TEXT(",\"schema\":"), ESearchCase::CaseSensitive,
			ESearchDir::FromStart, ValueStart);
		if (ValueEnd == INDEX_NONE || ValueEnd > ObjectEnd) return {};
		Text.RemoveAt(FieldAt, ValueEnd - FieldAt);
	}
	else
	{
		int32 RemoveStart = ObjectStart;
		int32 RemoveLength = ObjectEnd - ObjectStart + 1;
		if (Text.IsValidIndex(ObjectEnd + 1) && Text[ObjectEnd + 1] == TEXT(','))
			++RemoveLength;
		else if (ObjectStart > 0 && Text[ObjectStart - 1] == TEXT(','))
		{
			--RemoveStart;
			++RemoveLength;
		}
		Text.RemoveAt(RemoveStart, RemoveLength);
	}
	const FTCHARToUTF8 Utf8(*Text);
	TArray<uint8> Result;
	Result.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	return Result;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCacheBuilderContractIdentityClosureSpec,
	"WoT.RetailReader.CacheBuilder.ContractIdentityStartupClosure",
	WoTCacheBuilderSpecFlags)

bool FWoTCacheBuilderContractIdentityClosureSpec::RunTest(const FString&)
{
	FWoTRetailContract Baseline = WoTCacheBuilderSpecSyntheticContract();
	FWoTRetailContract Changed = Baseline;
	WoTCacheBuilderSpecSetStartupCritical(Changed,
		TEXT("cache/registry/synthetic"),
		TEXT("5c59611ffda3a22c811e1e2c3318a6e87c757cb926a7f57fdf7fbcb027001c24"));
	TestFalse(TEXT("baseline startup closure is valid"),
		ValidateFactClosure(Baseline).IsError());
	TestFalse(TEXT("changed startup closure is independently valid"),
		ValidateFactClosure(Changed).IsError());
	TestNotEqual(TEXT("contract identity binds the ordered startup closure and digest"),
		ComputeRetailContractIdentity(Baseline),
		ComputeRetailContractIdentity(Changed));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCacheBuilderFrontEndPublicationFieldsSpec,
	"WoT.RetailReader.CacheBuilder.FrontEndPublicationFields",
	WoTCacheBuilderSpecFlags)

bool FWoTCacheBuilderFrontEndPublicationFieldsSpec::RunTest(const FString&)
{
    FWoTCacheManifestV3 Manifest;
    Manifest.bHasBuildIdentity = true;
    Manifest.FrontEndDescriptorRoot = Sha256(TArray<uint8>{1u, 2u, 3u});
    TWoTBootstrapResult<TArray<uint8>> Encoded = WriteCanonicalCacheManifest(Manifest);
    if (!TestFalse(TEXT("publication manifest serializes"), Encoded.IsError())) return false;
    auto Parsed = ParseCanonicalJson(Encoded.GetValue());
    if (!TestFalse(TEXT("publication manifest parses"), Parsed.IsError())) return false;
    const auto* Root = Parsed.GetValue().FindMember(TEXT("front_end_descriptor_root"));
    if (!TestNotNull(TEXT("descriptor root is inside manifest authority"), Root)) return false;
    TestEqual(TEXT("manifest binds descriptor root"), Root->GetString(),
        Manifest.FrontEndDescriptorRoot.ToHex());
    const auto* Worlds = Parsed.GetValue().FindMember(TEXT("world_descriptors"));
    TestTrue(TEXT("manifest carries world records without a separate seal"),
        Worlds != nullptr && Worlds->IsArray());
    const auto OriginalBytes = Encoded.GetValue();
    Manifest.FrontEndDescriptorRoot.Bytes[0] ^= 1;
    Encoded = WriteCanonicalCacheManifest(Manifest);
    TestTrue(TEXT("changing descriptor root changes manifest digest"),
        !Encoded.IsError() && Sha256(Encoded.GetValue()) != Sha256(OriginalBytes));
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCacheBuilderInterruptionSpec,
    "WoT.RetailReader.CacheBuilder.InterruptedBuildRestarts", WoTCacheBuilderSpecFlags)

bool FWoTCacheBuilderInterruptionSpec::RunTest(const FString&)
{
    TArray<uint8> Descriptor;
    if (!TestTrue(TEXT("production descriptor loads"),
        WoTCacheBuilderSpecLoadProductionDescriptor(Descriptor))) return false;
    const FWoTRetailContract Contract = WoTCacheBuilderSpecSyntheticContract();
    const auto Sizes = WoTCacheBuilderSpecSyntheticSizes(Contract);
    const auto InstallerSha = WoTCacheBuilderSpecInstallerSha();
    FWoTCacheBuilderSpecWriter PartialWriter;
    FWoTCacheBuilderSpecProgress PartialProgress;
    auto PartialImporters = WoTCacheBuilderSpecImporters(Contract);
    auto FirstStage = MoveTemp(PartialImporters[0].Run);
    PartialImporters[0].Run = [&PartialProgress, FirstStage = MoveTemp(FirstStage)](
        IWoTObjectWriter& Writer, IWoTProgressSink& Progress) mutable
    {
        auto Result = FirstStage(Writer, Progress);
        PartialProgress.bCancelled = true;
        return Result;
    };
    auto Interrupted = BuildRetailCacheForTesting(InstallerSha, Contract,
        PartialWriter, PartialProgress, Descriptor, Sizes, PartialImporters);
    TestTrue(TEXT("interrupted build returns an error"), Interrupted.IsError());
    TestTrue(TEXT("interruption happens after real writes"), PartialWriter.TotalWrites() > 0);
    FWoTCacheBuilderSpecWriter FreshWriter;
    FWoTCacheBuilderSpecProgress FreshProgress;
    FWoTCacheBuilderSpecImportMetrics Metrics;
    auto FreshImporters = WoTCacheBuilderSpecImporters(Contract, &Metrics);
    auto Fresh = BuildRetailCacheForTesting(InstallerSha, Contract,
        FreshWriter, FreshProgress, Descriptor, Sizes, FreshImporters);
    if (!TestFalse(TEXT("next build succeeds from zero"), Fresh.IsError())) return false;
    TestTrue(TEXT("new build starts progress at zero"),
        !FreshProgress.Reports.IsEmpty() && FreshProgress.Reports[0] == 0);
    TestEqual(TEXT("new build writes all outputs"), FreshWriter.TotalWrites(),
        WoTCacheBuilderSpecOutputCount(Contract));
    for (const auto& Fact : Contract.Facts)
        TestEqual(Fact.FactId + TEXT(" is decoded again"),
            Metrics.SourceLoadsByFact.FindRef(Fact.FactId), 1);
    TestEqual(TEXT("completed progress counts all facts"),
        Fresh.GetValue().Progress.GetCompletedUnits(), static_cast<uint64>(Contract.Facts.Num()));
    FWoTCacheBuilderSpecWriter CleanWriter;
    FWoTCacheBuilderSpecProgress CleanProgress;
    auto CleanImporters = WoTCacheBuilderSpecImporters(Contract);
    auto Clean = BuildRetailCacheForTesting(InstallerSha, Contract,
        CleanWriter, CleanProgress, Descriptor, Sizes, CleanImporters);
    if (!TestFalse(TEXT("independent clean build succeeds"), Clean.IsError())) return false;
    TestEqual(TEXT("interrupted restart and clean build have identical manifests"),
        Fresh.GetValue().CanonicalManifest, Clean.GetValue().CanonicalManifest);
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCacheBuilderDescriptorSpec,
	"WoT.RetailReader.CacheBuilder.ProductionDescriptorClosure",
	WoTCacheBuilderSpecFlags)

bool FWoTCacheBuilderDescriptorSpec::RunTest(const FString&)
{
	TArray<uint8> Descriptor;
	if (!TestTrue(TEXT("production descriptor loads"),
		WoTCacheBuilderSpecLoadProductionDescriptor(Descriptor)))
		return false;
	TestFalse(TEXT("exact production descriptor is accepted"),
		ValidateCacheProductionDescriptorForTesting(Descriptor).IsError());
	const FWoTRetailContract Contract = WoTCacheBuilderSpecSyntheticContract();
	const TMap<FString, uint64> Sizes = WoTCacheBuilderSpecSyntheticSizes(Contract);
	for (const FWoTCacheBuilderSpecImporterDescription& Description :
		WoTCacheBuilderSpecImporterDescriptions)
	{
		for (const bool bRemoveEdge : {false, true})
		{
			TArray<uint8> Mutated = WoTCacheBuilderSpecRemovedDescriptorEntry(
				Descriptor, Description.Importer, bRemoveEdge);
			if (!TestTrue(FString(Description.Importer)
				+ (bRemoveEdge ? TEXT(" edge mutation") : TEXT(" output mutation")),
				!Mutated.IsEmpty())) continue;
			FWoTCacheBuilderSpecWriter Writer;
			FWoTCacheBuilderSpecProgress Progress;
			TArray<FWoTCacheImporterForTesting> Importers =
				WoTCacheBuilderSpecImporters(Contract);
			TWoTBootstrapResult<FWoTCacheBuildResult> Refused =
				BuildRetailCacheForTesting(WoTCacheBuilderSpecInstallerSha(), Contract,
					Writer, Progress, Mutated, Sizes, Importers);
			TestTrue(FString(Description.Importer)
				+ (bRemoveEdge ? TEXT(" missing Shipping edge refuses")
					: TEXT(" missing output refuses")), Refused.IsError());
			if (Refused.IsError())
			{
				const FString ExpectedPath = FString(
					TEXT("production-descriptor.importer_outputs."))
					+ Description.Importer
					+ (bRemoveEdge ? TEXT(".shipping-edge") : TEXT(""));
				TestEqual(FString(Description.Importer)
					+ TEXT(" mutation reports its specific field path"),
					Refused.GetError().Path, ExpectedPath);
			}
			TestEqual(TEXT("descriptor refusal precedes importer reads"),
				Writer.TotalWrites(), 0);
		}
	}
	TArray<uint8> RegeneratedSubstitute = Descriptor;
	static const ANSICHAR StatusNeedle[] = "not_yet_cooked";
	for (int32 ByteIndex = 0;
		ByteIndex + UE_ARRAY_COUNT(StatusNeedle) - 1 <= RegeneratedSubstitute.Num();
		++ByteIndex)
	{
		if (FMemory::Memcmp(RegeneratedSubstitute.GetData() + ByteIndex,
			StatusNeedle, UE_ARRAY_COUNT(StatusNeedle) - 1) == 0)
		{
			RegeneratedSubstitute[ByteIndex + UE_ARRAY_COUNT(StatusNeedle) - 2] =
				static_cast<uint8>('e');
			break;
		}
	}
	FWoTBootstrapStatus Stale =
		ValidateCacheProductionDescriptorForTesting(RegeneratedSubstitute);
	TestTrue(TEXT("stale descriptor graph is refused"), Stale.IsError());
	if (Stale.IsError())
	{
		TestEqual(TEXT("stale descriptor reports graph identity context"),
			Stale.GetError().Path,
			FString(TEXT("production-descriptor.graph_sha256")));
	}

	// Repair the graph digest so this canonical substitute reaches the final
	// exact-document pin rather than the earlier semantic/staleness checks.
	const FUTF8ToTCHAR Converted(
		reinterpret_cast<const ANSICHAR*>(RegeneratedSubstitute.GetData()),
		RegeneratedSubstitute.Num());
	FString SubstituteText(Converted.Length(), Converted.Get());
	const FString GraphPrefix = TEXT("\"graph\":");
	const FString GraphSuffix = TEXT(",\"graph_sha256\":\"");
	const int32 GraphAt = SubstituteText.Find(GraphPrefix, ESearchCase::CaseSensitive);
	const int32 GraphStart = GraphAt == INDEX_NONE
		? INDEX_NONE : GraphAt + GraphPrefix.Len();
	const int32 GraphEnd = GraphStart == INDEX_NONE ? INDEX_NONE
		: SubstituteText.Find(GraphSuffix, ESearchCase::CaseSensitive,
			ESearchDir::FromStart, GraphStart);
	if (TestTrue(TEXT("substitute graph framing is found"),
		GraphStart != INDEX_NONE && GraphEnd != INDEX_NONE))
	{
		const FString GraphText = SubstituteText.Mid(GraphStart, GraphEnd - GraphStart);
		const FTCHARToUTF8 GraphUtf8(*GraphText);
		const FWoTSha256 RepairedGraphSha = Sha256(TConstArrayView<uint8>(
			reinterpret_cast<const uint8*>(GraphUtf8.Get()), GraphUtf8.Length()));
		const int32 DigestStart = GraphEnd + GraphSuffix.Len();
		SubstituteText.RemoveAt(DigestStart, 64);
		SubstituteText = SubstituteText.Left(DigestStart)
			+ RepairedGraphSha.ToHex() + SubstituteText.Mid(DigestStart);
		const FTCHARToUTF8 SubstituteUtf8(*SubstituteText);
		TArray<uint8> CanonicalSubstitute;
		CanonicalSubstitute.Append(
			reinterpret_cast<const uint8*>(SubstituteUtf8.Get()),
			SubstituteUtf8.Length());
		FWoTBootstrapStatus Substitute =
			ValidateCacheProductionDescriptorForTesting(CanonicalSubstitute);
		TestTrue(TEXT("caller-supplied canonical substitute is refused"),
			Substitute.IsError());
		if (Substitute.IsError())
		{
			TestEqual(TEXT("canonical substitute reaches exact-document pin"),
				Substitute.GetError().Path,
				FString(TEXT("Build/WoTRelease/cache-v3-production-descriptor.json")));
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCacheBuilderSyntheticParitySpec,
	"WoT.RetailReader.CacheBuilder.SyntheticNineteenMapParity",
	WoTCacheBuilderSpecFlags)

const FWoTCacheBuilderSpecImporterDescription& WoTCacheBuilderSpecDescription(
	EWoTFactDomain Domain);

TArray<FString> WoTCacheBuilderSpecDescriptorRejectionPaths(
	TConstArrayView<uint8> Descriptor)
{
	TArray<FString> Paths;
	for (const FWoTCacheBuilderSpecImporterDescription& Description :
		WoTCacheBuilderSpecImporterDescriptions)
	{
		for (const bool bRemoveEdge : {false, true})
		{
			TArray<uint8> Mutated = WoTCacheBuilderSpecRemovedDescriptorEntry(
				Descriptor, Description.Importer, bRemoveEdge);
			FWoTBootstrapStatus Refused =
				ValidateCacheProductionDescriptorForTesting(Mutated);
			if (Refused.IsError()) Paths.Add(Refused.GetError().Path);
		}
	}
	Paths.Sort();
	return Paths;
}

FWoTRetailContract WoTCacheBuilderSpecSyntheticWorldContract(
	TConstArrayView<FString> MapIdentities)
{
	FWoTRetailContract Contract;
	Contract.Format = TEXT("wot-cache-contract-v3");
	Contract.ContractVersion = 3;
	Contract.ContractId = TEXT("task25-synthetic-nineteen-map-parity");
	Contract.KeyId = TEXT("task25-test-key");
	Contract.ReleaseSequence = 25;
	WoTCacheBuilderSpecAddAudit(Contract, TEXT("UWoTPayloadSubsystem"),
		TEXT("audit.bsp-payload-v1"));
	WoTCacheBuilderSpecAddAudit(Contract, TEXT("UWoTPayloadSubsystem"),
		TEXT("audit.world-payload-v1"));
	for (const FString& Identity : MapIdentities)
	{
		const FString Source = TEXT("Synthetic/Maps/") + Identity + TEXT(".wot");
		WoTCacheBuilderSpecAddFact(Contract,
			WoTCacheBuilderSpecDescription(EWoTFactDomain::BSP),
			TEXT("synthetic.bsp.") + Identity, Source,
			Identity + TEXT(".Model"), TEXT("cache/bsp/") + Identity);
		WoTCacheBuilderSpecAddFact(Contract,
			WoTCacheBuilderSpecDescription(EWoTFactDomain::World),
			TEXT("synthetic.world.") + Identity, Source,
			Identity + TEXT(".Level"), TEXT("cache/world/") + Identity);
	}
	WoTCacheBuilderSpecSetStartupCritical(Contract,
		TEXT("cache/world/Arena_01"),
		TEXT("96f79ca528ade0bc9a7b80b0db1f1c4705417148fb150532dc4dd090cb37cbb5"));
	return Contract;
}

bool FWoTCacheBuilderSyntheticParitySpec::RunTest(const FString&)
{
	TArray<uint8> Fixture;
	if (!TestTrue(TEXT("nineteen-map semantic fixture loads"),
		WoTCacheBuilderSpecLoad(TEXT("Tools/dist/fixtures/map-set-v3-synthetic.json"),
			Fixture))) return false;
	TWoTBootstrapResult<FWoTWorldSetResult> Native =
		ValidateWorldSemanticFixtureForTesting(Fixture);
	if (!TestFalse(TEXT("native semantic importer accepts the frozen oracle"),
		Native.IsError())) return false;
	TestEqual(TEXT("all nineteen synthetic maps produce semantic event streams"),
		Native.GetValue().Worlds.Num(),
		static_cast<int32>(UE_ARRAY_COUNT(WoT::Retail::Generated::RequiredWorlds)));
	for (int32 MapIndex = 0; MapIndex < Native.GetValue().Worlds.Num(); ++MapIndex)
	{
		TestEqual(FString::Printf(TEXT("map %d identity parity"), MapIndex),
			Native.GetValue().Worlds[MapIndex].MapIdentity,
			FString(WoT::Retail::Generated::RequiredWorlds[MapIndex].Identity));
	}
	TWoTBootstrapResult<FWoTCanonicalJsonValue> Parsed = ParseCanonicalJson(Fixture);
	if (!TestFalse(TEXT("oracle fixture is canonical"), Parsed.IsError())) return false;
	const FWoTCanonicalJsonValue* MapValues =
		Parsed.GetValue().FindMember(TEXT("maps"));
	if (!TestTrue(TEXT("oracle fixture exposes its map vector"),
		MapValues != nullptr && MapValues->IsArray())) return false;
	TArray<FString> MapIdentities;
	TArray<TArray<uint8>> BSPObjects;
	TArray<TArray<uint8>> WorldObjects;
	TArray<FWoTSha256> EventRoots;
	for (int32 MapIndex = 0; MapIndex < MapValues->Num(); ++MapIndex)
	{
		const FWoTCanonicalJsonValue& MapValue = MapValues->GetElement(MapIndex);
		const FWoTCanonicalJsonValue* Identity = MapValue.FindMember(TEXT("identity"));
		const FWoTCanonicalJsonValue* BSP = MapValue.FindMember(TEXT("bsp"));
		const FWoTCanonicalJsonValue* EventRoot = MapValue.FindMember(TEXT("map_root"));
		if (!TestTrue(FString::Printf(TEXT("map %d parity fields exist"), MapIndex),
			Identity != nullptr
			&& Identity->GetKind() == EWoTCanonicalJsonKind::String
			&& BSP != nullptr && BSP->IsObject()
			&& EventRoot != nullptr
			&& EventRoot->GetKind() == EWoTCanonicalJsonKind::String)) return false;
		TWoTBootstrapResult<TArray<uint8>> BSPBytes = WriteCanonicalJson(*BSP);
		TWoTBootstrapResult<TArray<uint8>> WorldBytes = WriteCanonicalJson(MapValue);
		TWoTBootstrapResult<FWoTSha256> Root =
			FWoTSha256::FromHex(EventRoot->GetString());
		if (!TestFalse(TEXT("synthetic map projections serialize"),
			BSPBytes.IsError() || WorldBytes.IsError() || Root.IsError())) return false;
		MapIdentities.Add(Identity->GetString());
		BSPObjects.Add(MoveTemp(BSPBytes).GetValue());
		WorldObjects.Add(MoveTemp(WorldBytes).GetValue());
		EventRoots.Add(Root.GetValue());
	}
	for (int32 MapIndex = 0; MapIndex < MapIdentities.Num(); ++MapIndex)
	{
		TestEqual(FString::Printf(TEXT("map %d semantic event root"), MapIndex),
			Native.GetValue().Worlds[MapIndex].MapRoot.ToHex(),
			EventRoots[MapIndex].ToHex());
	}

	const FWoTRetailContract Contract =
		WoTCacheBuilderSpecSyntheticWorldContract(MapIdentities);
	TMap<FString, uint64> Sizes;
	for (int32 MapIndex = 0; MapIndex < MapIdentities.Num(); ++MapIndex)
	{
		Sizes.Add(TEXT("Synthetic/Maps/") + MapIdentities[MapIndex] + TEXT(".wot"),
			static_cast<uint64>(WorldObjects[MapIndex].Num()));
	}
	TArray<uint8> Descriptor;
	if (!TestTrue(TEXT("production descriptor loads"),
		WoTCacheBuilderSpecLoadProductionDescriptor(Descriptor))) return false;
	FWoTCacheBuilderSpecWriter Writer;
	FWoTCacheBuilderSpecProgress Progress;
	TArray<FWoTCacheImporterForTesting> Importers;
	FWoTCacheImporterForTesting& Importer = Importers.AddDefaulted_GetRef();
	Importer.Stage = TEXT("required-world-set");
	Importer.Importers = {TEXT("bsp-v1"), TEXT("world-v1")};
	Importer.Run = [&MapIdentities, &BSPObjects, &WorldObjects, &Fixture](
		IWoTObjectWriter& ObjectWriter,
		IWoTProgressSink& ObjectProgress)
	{
		using FResult = TWoTBootstrapResult<TArray<FWoTCacheBlob>>;
		TWoTBootstrapResult<FWoTWorldSetResult> Validated =
			ValidateWorldSemanticFixtureForTesting(Fixture);
		if (Validated.IsError())
			return FResult::Error(MoveTemp(Validated).GetError());
		const uint64 Total = static_cast<uint64>(MapIdentities.Num()) * 2;
		FWoTBootstrapStatus Reported = ObjectProgress.ReportProgress(
			TEXT("synthetic-world-events"), 0, Total);
		if (Reported.IsError()) return FResult::Error(MoveTemp(Reported).GetError());
		TArray<FWoTCacheBlob> Blobs;
		uint64 Completed = 0;
		for (int32 MapIndex = 0; MapIndex < MapIdentities.Num(); ++MapIndex)
		{
			const FString& Identity = MapIdentities[MapIndex];
			for (const bool bWorld : {false, true})
			{
				const FString LogicalId = bWorld ? TEXT("cache/world/") + Identity
					: TEXT("cache/bsp/") + Identity;
				const FString Schema = bWorld ? TEXT("wot-world-v3") : TEXT("wot-bsp-v3");
				const TArray<uint8>& Bytes = bWorld
					? WorldObjects[MapIndex] : BSPObjects[MapIndex];
				const FString FactId = bWorld ? TEXT("synthetic.world.") + Identity
					: TEXT("synthetic.bsp.") + Identity;
				FWoTBootstrapStatus Written = ObjectWriter.WriteObject(
					LogicalId, Schema, Bytes, Sha256(Bytes), 1073741824ull);
				if (Written.IsError())
					return FResult::Error(MoveTemp(Written).GetError());
				FWoTCacheBlob& Blob = Blobs.AddDefaulted_GetRef();
				Blob.LogicalId = LogicalId;
				Blob.Schema = Schema;
				Blob.Sha256 = Sha256(Bytes);
				Blob.Size = static_cast<uint64>(Bytes.Num());
				Blob.Satisfies.Add(FWoTRequiredFactId{FactId});
				Reported = ObjectProgress.ReportProgress(
					TEXT("synthetic-world-events"), ++Completed, Total);
				if (Reported.IsError())
					return FResult::Error(MoveTemp(Reported).GetError());
			}
		}
		return FResult::Success(MoveTemp(Blobs));
	};
	TWoTBootstrapResult<FWoTCacheBuildResult> Built = BuildRetailCacheForTesting(
		Sha256(Fixture), Contract, Writer, Progress,
		Descriptor, Sizes, Importers);
	if (!TestFalse(TEXT("Task 25 builder completes the nineteen-map set"),
		Built.IsError()))
	{
		if (Built.IsError()) AddError(Built.GetError().ToString());
		return false;
	}
	TestEqual(TEXT("nineteen maps emit BSP and world objects"),
		Writer.Objects.Num(), MapIdentities.Num() * 2);
	TestEqual(TEXT("synthetic progress completion equals its fixed total"),
		Built.GetValue().Progress.GetCompletedUnits(),
		Built.GetValue().Progress.GetTotalUnits());
	const TArray<FString> RejectedPaths =
		WoTCacheBuilderSpecDescriptorRejectionPaths(Descriptor);
	TestEqual(TEXT("descriptor mutation matrix contributes every field path"),
		RejectedPaths.Num(),
		static_cast<int32>(UE_ARRAY_COUNT(WoTCacheBuilderSpecImporterDescriptions)) * 2);
	TMap<FString, FWoTSha256> WorldEventRoots;
	for (int32 MapIndex = 0; MapIndex < MapIdentities.Num(); ++MapIndex)
	{
		WorldEventRoots.Add(MapIdentities[MapIndex],
			Native.GetValue().Worlds[MapIndex].MapRoot);
	}
	TWoTBootstrapResult<TArray<uint8>> Artifact =
		WriteCacheSyntheticParityArtifactForTesting(
			Built.GetValue(), WorldEventRoots, Native.GetValue().SetRoot,
			RejectedPaths);
	if (!TestFalse(TEXT("canonical parity artifact serializes"), Artifact.IsError()))
		return false;
	const FString ArtifactPath = WoTCacheBuilderSpecRepoPath(
		TEXT("Saved/ReleaseEvidence/cache-v3-parity/native-synthetic.json"));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(ArtifactPath), true);
	TestTrue(TEXT("canonical native parity artifact is emitted"),
		FFileHelper::SaveArrayToFile(Artifact.GetValue(), *ArtifactPath));
	return !HasAnyErrors();
}

const FWoTCacheBuilderSpecImporterDescription& WoTCacheBuilderSpecDescription(
	EWoTFactDomain Domain)
{
	for (const FWoTCacheBuilderSpecImporterDescription& Description :
		WoTCacheBuilderSpecImporterDescriptions)
		if (Description.Domain == Domain) return Description;
	checkNoEntry();
	return WoTCacheBuilderSpecImporterDescriptions[0];
}

struct FWoTCacheBuilderSpecRetailSource
{
	FString SourcePath;
	FString FullPath;
	EWoTFactDomain Domain = EWoTFactDomain::Registry;
	FString LogicalId;
	TArray<uint8> Bytes;
};

class FWoTCacheBuilderSpecHashingWriter final : public IWoTObjectWriter
{
public:
	virtual FWoTBootstrapStatus WriteObject(FStringView LogicalId,
		FStringView Schema, TConstArrayView<uint8> Bytes,
		const FWoTSha256& Sha256Digest, uint64 MaximumBytes) override
	{
		if (static_cast<uint64>(Bytes.Num()) > MaximumBytes)
			return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::SizeLimit, TEXT("retail semantic object overflow")));
		FWoTCacheBlob& Blob = Objects.AddDefaulted_GetRef();
		Blob.LogicalId = FString(LogicalId);
		Blob.Schema = FString(Schema);
		Blob.Size = static_cast<uint64>(Bytes.Num());
		Blob.Sha256 = Sha256Digest;
		return FWoTBootstrapStatus::Success();
	}
	TArray<FWoTCacheBlob> Objects;
};

TWoTBootstrapResult<FWoTRetailContract> WoTCacheBuilderSpecRetailWorldContract()
{
	using FResult = TWoTBootstrapResult<FWoTRetailContract>;
	TArray<uint8> Fixture;
	if (!WoTCacheBuilderSpecLoad(
			TEXT("Tools/dist/fixtures/bsp-native-parity-v1.json"), Fixture))
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Io,
			TEXT("the retail BSP parity fixture is missing")));
	TWoTBootstrapResult<FWoTCanonicalJsonValue> Parsed = ParseCanonicalJson(Fixture);
	if (Parsed.IsError()) return FResult::Error(MoveTemp(Parsed).GetError());
	const FWoTCanonicalJsonValue* Maps = Parsed.GetValue().FindMember(TEXT("maps"));
	if (Maps == nullptr || !Maps->IsArray()
		|| Maps->Num() != UE_ARRAY_COUNT(WoT::Retail::Generated::RequiredWorlds))
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::UnknownField,
			TEXT("the retail BSP parity fixture does not contain nineteen maps")));
	FWoTRetailContract Contract;
	Contract.Format = TEXT("wot-cache-contract-v3");
	Contract.ContractVersion = 3;
	Contract.ContractId = TEXT("task25-retail-world-events");
	Contract.KeyId = TEXT("task25-retail-pin");
	Contract.ReleaseSequence = 25;
	WoTCacheBuilderSpecAddAudit(Contract, TEXT("UWoTPayloadSubsystem"),
		TEXT("audit.bsp-payload-v1"));
	WoTCacheBuilderSpecAddAudit(Contract, TEXT("UWoTPayloadSubsystem"),
		TEXT("audit.world-payload-v1"));
	for (int32 MapIndex = 0; MapIndex < Maps->Num(); ++MapIndex)
	{
		const WoT::Retail::Generated::FRequiredWorld& Required =
			WoT::Retail::Generated::RequiredWorlds[MapIndex];
		const FWoTCanonicalJsonValue& Map = Maps->GetElement(MapIndex);
		const FWoTCanonicalJsonValue* Identity = Map.FindMember(TEXT("identity"));
		const FWoTCanonicalJsonValue* Parts = Map.FindMember(TEXT("parts"));
		if (Identity == nullptr
			|| Identity->GetKind() != EWoTCanonicalJsonKind::String
			|| Identity->GetString() != Required.Identity
			|| Parts == nullptr || !Parts->IsArray() || Parts->Num() == 0)
			return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::UnknownField,
				TEXT("the retail BSP parity map identity or part set is invalid")));
		const FString Name(Required.Identity);
		FWoTRequiredFact& BSP = Contract.Facts.AddDefaulted_GetRef();
		BSP.Domain = EWoTFactDomain::BSP;
		BSP.FactId = TEXT("retail.bsp.") + Name;
		BSP.SourcePath = Required.SourcePath;
		BSP.SourceExport = Name + TEXT(".Model");
		BSP.Importer = TEXT("bsp-v1");
		BSP.RuntimeConsumer = TEXT("UWoTPayloadSubsystem");
		BSP.AuditRule = TEXT("audit.bsp-payload-v1");
		for (int32 PartIndex = 0; PartIndex < Parts->Num(); ++PartIndex)
		{
			const FWoTCanonicalJsonValue* PartIdentity =
				Parts->GetElement(PartIndex).FindMember(TEXT("part_identity"));
			if (PartIdentity == nullptr
				|| PartIdentity->GetKind() != EWoTCanonicalJsonKind::String)
				return FResult::Error(MakeWoTBootstrapError(
					EWoTBootstrapError::UnknownField,
					TEXT("a retail BSP parity part has no identity")));
			FWoTRequiredFactOutput& Output = BSP.Outputs.AddDefaulted_GetRef();
			Output.LogicalId = TEXT("cache/bsp/") + Name + TEXT("/")
				+ PartIdentity->GetString();
			Output.Schema = TEXT("wot-bsp-v3");
			Output.MaxDecodedSize = 1073741824ull;
		}
		FWoTRequiredFact& World = Contract.Facts.AddDefaulted_GetRef();
		World.Domain = EWoTFactDomain::World;
		World.FactId = TEXT("retail.world.") + Name;
		World.SourcePath = Required.SourcePath;
		World.SourceExport = Name + TEXT(".Level");
		World.Importer = TEXT("world-v1");
		World.RuntimeConsumer = TEXT("UWoTPayloadSubsystem");
		World.AuditRule = TEXT("audit.world-payload-v1");
		FWoTRequiredFactOutput& WorldOutput = World.Outputs.AddDefaulted_GetRef();
		WorldOutput.LogicalId = TEXT("cache/world/") + Name;
		WorldOutput.Schema = TEXT("wot-world-v3");
		WorldOutput.MaxDecodedSize = 1073741824ull;
	}
	WoTCacheBuilderSpecSetStartupCritical(Contract,
		TEXT("cache/world/Arena_01"),
		TEXT("96f79ca528ade0bc9a7b80b0db1f1c4705417148fb150532dc4dd090cb37cbb5"));
	return FResult::Success(MoveTemp(Contract));
}

TArray<FWoTCacheImporterForTesting> WoTCacheBuilderSpecRetailImporters(
	const FWoTRetailContract& Contract,
	const TArray<FWoTCacheBuilderSpecRetailSource>& Sources)
{
	TArray<FWoTCacheImporterForTesting> Importers =
		WoTCacheBuilderSpecImporters(Contract);
	for (FWoTCacheImporterForTesting& Importer : Importers)
	{
		const FString Stage = Importer.Stage;
		Importer.Run = [&Contract, &Sources, Stage](IWoTObjectWriter& Writer,
			IWoTProgressSink& Progress)
		{
			using FResult = TWoTBootstrapResult<TArray<FWoTCacheBlob>>;
			TArray<const FWoTRequiredFact*> Facts;
			for (const FWoTRequiredFact& Fact : Contract.Facts)
				if (WoTCacheBuilderSpecStage(Fact.Importer) == Stage) Facts.Add(&Fact);
			FWoTBootstrapStatus Initial = Progress.ReportProgress(Stage, 0, Facts.Num());
			if (Initial.IsError()) return FResult::Error(MoveTemp(Initial).GetError());
			TArray<FWoTCacheBlob> Blobs;
			for (int32 FactIndex = 0; FactIndex < Facts.Num(); ++FactIndex)
			{
				const FWoTRequiredFact& Fact = *Facts[FactIndex];
				const FWoTCacheBuilderSpecRetailSource* Source = Sources.FindByPredicate(
					[&Fact](const FWoTCacheBuilderSpecRetailSource& Candidate)
					{
						return Candidate.SourcePath == Fact.SourcePath;
					});
				if (Source == nullptr)
					return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Io,
						TEXT("retail pin source disappeared"), Fact.SourcePath));
				for (const FWoTRequiredFactOutput& Output : Fact.Outputs)
				{
					FWoTBootstrapStatus Written = Writer.WriteObject(Output.LogicalId,
						Output.Schema, Source->Bytes, Sha256(Source->Bytes),
						Output.MaxDecodedSize);
					if (Written.IsError())
						return FResult::Error(MoveTemp(Written).GetError());
					FWoTCacheBlob& Blob = Blobs.AddDefaulted_GetRef();
					Blob.LogicalId = Output.LogicalId;
					Blob.Schema = Output.Schema;
					Blob.Sha256 = Sha256(Source->Bytes);
					Blob.Size = static_cast<uint64>(Source->Bytes.Num());
					Blob.Satisfies.Add(FWoTRequiredFactId{Fact.FactId});
				}
				FWoTBootstrapStatus Reported = Progress.ReportProgress(
					Stage, FactIndex + 1, Facts.Num());
				if (Reported.IsError())
					return FResult::Error(MoveTemp(Reported).GetError());
			}
			return FResult::Success(MoveTemp(Blobs));
		};
	}
	return Importers;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCacheBuilderRetailPinSpec,
	"WoT.RetailReader.CacheBuilder.RealRetailBuildPin",
	WoTCacheBuilderSpecFlags)

bool FWoTCacheBuilderRetailPinSpec::RunTest(const FString&)
{
	if (!WoTReferenceTreeExists())
	{
		AddWarning(TEXT("real retail data absent: production census not verified"));
		return true;
	}
	const FString System = WoTCacheBuilderSpecRepoPath(TEXT("original/System"));
	const FString Maps = WoTCacheBuilderSpecRepoPath(TEXT("original/Maps"));
	if (!TestTrue(TEXT("required original/System directory exists"),
		FPaths::DirectoryExists(System))) return false;
	if (!TestTrue(TEXT("required original/Maps directory exists"),
		FPaths::DirectoryExists(Maps))) return false;
	TArray<FString> Packages;
	TArray<FString> Localizations;
	IFileManager::Get().FindFiles(Packages, *(System / TEXT("*.u")), true, false);
	IFileManager::Get().FindFiles(Localizations, *(System / TEXT("*.int")), true, false);
	Packages.Sort();
	Localizations.Sort();
	TestEqual(TEXT("retail pin has all System .u packages"), Packages.Num(), 20);
	TestEqual(TEXT("retail pin has all System .int packages"), Localizations.Num(), 29);

	FWoTRetailContract Contract;
	Contract.Format = TEXT("wot-cache-contract-v3");
	Contract.ContractVersion = 3;
	Contract.ContractId = TEXT("task25-retail-system-maps-pin");
	Contract.KeyId = TEXT("task25-retail-pin");
	Contract.ReleaseSequence = 25;
	TArray<FWoTCacheBuilderSpecRetailSource> Sources;
	auto AddSource = [this, &Contract, &Sources](const FString& SourcePath,
		const FString& FullPath, EWoTFactDomain Domain, const FString& Identity)
	{
		FWoTCacheBuilderSpecRetailSource& Source = Sources.AddDefaulted_GetRef();
		Source.SourcePath = SourcePath;
		Source.FullPath = FullPath;
		Source.Domain = Domain;
		const FWoTCacheBuilderSpecImporterDescription& Description =
			WoTCacheBuilderSpecDescription(Domain);
		const FString DomainName = Description.DomainName;
		Source.LogicalId = TEXT("cache/") + DomainName + TEXT("/retail-pin/") + Identity;
		if (!TestTrue(SourcePath + TEXT(" loads"),
			FFileHelper::LoadFileToArray(Source.Bytes, *FullPath))) return;
		WoTCacheBuilderSpecAddFact(Contract, Description,
			TEXT("retail-pin.") + DomainName + TEXT(".") + Identity.ToLower(),
			SourcePath, Identity, Source.LogicalId);
	};
	for (const FString& Package : Packages)
	{
		const FString Stem = FPaths::GetBaseFilename(Package);
		AddSource(TEXT("System/") + Package, System / Package,
			EWoTFactDomain::Registry, Stem);
	}
	for (const FString& Localization : Localizations)
	{
		AddSource(TEXT("System/") + Localization, System / Localization,
			EWoTFactDomain::Localization, Localization);
	}
	for (const WoT::Retail::Generated::FRequiredWorld& World :
		WoT::Retail::Generated::RequiredWorlds)
	{
		AddSource(World.SourcePath, Maps / (FString(World.Identity) + TEXT(".wot")),
			EWoTFactDomain::World, World.Identity);
	}
	TestEqual(TEXT("retail pin covers 20 packages, 29 localizations, and 19 maps"),
		Sources.Num(), 68);
	WoTCacheBuilderSpecSetStartupCritical(Contract,
		TEXT("cache/localization/retail-pin/WoT.int"),
		TEXT("cebd1c400ea5c8476a82ea316c2eb9180cfcc24527356a5c32a572bad7c1da55"));
	if (HasAnyErrors()) return false;

	TWoTBootstrapResult<FWoTRetailContract> WorldContract =
		WoTCacheBuilderSpecRetailWorldContract();
	if (!TestFalse(TEXT("retail world event contract builds"), WorldContract.IsError()))
		return false;
	TArray<FWoTWorldPackageBytesForTesting> WorldSources;
	for (const FWoTCacheBuilderSpecRetailSource& Source : Sources)
	{
		if (Source.Domain != EWoTFactDomain::World) continue;
		FWoTWorldPackageBytesForTesting& WorldSource =
			WorldSources.AddDefaulted_GetRef();
		WorldSource.SourcePath = Source.SourcePath;
		WorldSource.Bytes = Source.Bytes;
	}
	FWoTCacheBuilderSpecHashingWriter WorldWriter;
	FWoTCacheBuilderSpecProgress WorldProgress(true);
	TWoTBootstrapResult<FWoTWorldSetResult> WorldEvents =
		ImportRequiredWorldSetPackageBytesForTesting(WorldSources,
			WorldContract.GetValue(), WorldWriter, WorldProgress);
	if (!TestFalse(TEXT("all actual map semantic event streams import"),
		WorldEvents.IsError()))
	{
		if (WorldEvents.IsError()) AddError(WorldEvents.GetError().ToString());
		return false;
	}
	TestEqual(TEXT("actual retail event streams retain nineteen identities"),
		WorldEvents.GetValue().Worlds.Num(), 19);
	for (int32 MapIndex = 0; MapIndex < WorldEvents.GetValue().Worlds.Num(); ++MapIndex)
	{
		TestEqual(FString::Printf(TEXT("retail event map %d identity"), MapIndex),
			WorldEvents.GetValue().Worlds[MapIndex].MapIdentity,
			FString(WoT::Retail::Generated::RequiredWorlds[MapIndex].Identity));
	}
	TestEqual(TEXT("actual event writer matches importer object inventory"),
		WorldWriter.Objects.Num(), WorldEvents.GetValue().Objects.Num());

	TArray<uint8> Descriptor;
	if (!TestTrue(TEXT("production descriptor loads"),
		WoTCacheBuilderSpecLoadProductionDescriptor(Descriptor)))
		return false;
	TMap<FString, uint64> Sizes;
	FWoTSha256Hasher InstallerHasher;
	for (const FWoTCacheBuilderSpecRetailSource& Source : Sources)
	{
		Sizes.Add(Source.SourcePath, static_cast<uint64>(Source.Bytes.Num()));
		const FTCHARToUTF8 Name(*Source.SourcePath);
		InstallerHasher.Update(TConstArrayView<uint8>(
			reinterpret_cast<const uint8*>(Name.Get()), Name.Length()));
		const uint8 Separator = 0;
		InstallerHasher.Update(MakeArrayView(&Separator, 1));
		InstallerHasher.Update(Source.Bytes);
	}
	const FWoTSha256 InstallerSha = InstallerHasher.Finish();

	// Pin the coordinator around the real nineteen-map importer as a separate
	// build from the 68-source orchestration/pass-through pin below.
	TMap<FString, uint64> WorldSizes;
	for (const FWoTWorldPackageBytesForTesting& WorldSource : WorldSources)
	{
		WorldSizes.Add(WorldSource.SourcePath,
			static_cast<uint64>(WorldSource.Bytes.Num()));
	}
	FWoTCacheBuilderSpecWriter WorldBuildWriter;
	FWoTCacheBuilderSpecProgress WorldBuildProgress;
	TArray<FWoTCacheImporterForTesting> WorldBuildImporters;
	FWoTCacheImporterForTesting& WorldBuildImporter =
		WorldBuildImporters.AddDefaulted_GetRef();
	WorldBuildImporter.Stage = TEXT("required-world-set");
	WorldBuildImporter.Importers = {TEXT("bsp-v1"), TEXT("world-v1")};
	WorldBuildImporter.Run = [&WorldSources, &WorldContract](
		IWoTObjectWriter& ObjectWriter, IWoTProgressSink& ObjectProgress)
	{
		using FObjectsResult = TWoTBootstrapResult<TArray<FWoTCacheBlob>>;
		TWoTBootstrapResult<FWoTWorldSetResult> Imported =
			ImportRequiredWorldSetPackageBytesForTesting(
				WorldSources, WorldContract.GetValue(), ObjectWriter, ObjectProgress);
		if (Imported.IsError())
			return FObjectsResult::Error(MoveTemp(Imported).GetError());
		return FObjectsResult::Success(MoveTemp(Imported).GetValue().Objects);
	};
	TWoTBootstrapResult<FWoTCacheBuildResult> WorldBuilt =
		BuildRetailCacheForTesting(InstallerSha, WorldContract.GetValue(),
			WorldBuildWriter, WorldBuildProgress,
			Descriptor, WorldSizes, WorldBuildImporters);
	if (!TestFalse(TEXT("builder runs the real nineteen-map world importer"),
		WorldBuilt.IsError()))
	{
		if (WorldBuilt.IsError()) AddError(WorldBuilt.GetError().ToString());
		return false;
	}
	TestEqual(TEXT("builder real-world inventory matches standalone importer"),
		WorldBuilt.GetValue().Manifest.Blobs.Num(), WorldEvents.GetValue().Objects.Num());
	for (const FWoTCacheBlob& Direct : WorldEvents.GetValue().Objects)
	{
		const FWoTCacheBlob* Coordinated =
			WorldBuilt.GetValue().Manifest.Blobs.FindByPredicate(
				[&Direct](const FWoTCacheBlob& Candidate)
				{
					return Candidate.LogicalId == Direct.LogicalId;
				});
		if (TestNotNull(Direct.LogicalId + TEXT(" is coordinated"), Coordinated))
		{
			TestEqual(Direct.LogicalId + TEXT(" hash matches direct importer"),
				Coordinated->Sha256.ToHex(), Direct.Sha256.ToHex());
			TestEqual(Direct.LogicalId + TEXT(" size matches direct importer"),
				Coordinated->Size, Direct.Size);
		}
	}
	// This importer-only fixture manifest carries empty world metadata and the
	// zero front-end root. Object hashes are checked independently against the direct
	// importer above. Recomputed 2026-10-15 for world-v6, BSP v10 exact macro transforms and
	// per-triangle surface provenance, with PF_NotSolid applied to the dedicated
	// mover collision streams. Generated from the canonical manifest writer after the
	// assertions above prove every coordinated object hash/size identical to
	// the direct native importer. The frozen-importer oracle separately pins
	// the complete native receipt in release evidence. Recomputed 2026-10-20:
	// mover collision streams now keep Invisible|Semisolid brush polys (UE1
	// NF_NotVisBlocking hulls collide), which moves the Arena_09, Battle2_04 and
	// Battle3_02 world blobs.
	const FString ExpectedWorldImporterManifestSha = TEXT(
		"91f916fef7a6f6e8811e02c4f85c8b52c1764ef391bdd6d84cd3308aeed4a3d0");
	const FWoTSha256 PriorProductionDescriptorSha =
		FWoTSha256::FromHex(TEXT(
			"6909b7ee175cecbcd276ddf70d0250fcc224d507fd50d9df65ba76717fe59cf4"))
		.GetValue();
	FWoTCacheManifestV3 WorldOracleManifest = WorldBuilt.GetValue().Manifest;
	WorldOracleManifest.ProductionDescriptorSha256 = PriorProductionDescriptorSha;
	TWoTBootstrapResult<TArray<uint8>> WorldOracleBytes =
		WriteCanonicalCacheManifest(WorldOracleManifest);
	const FString WorldOracleSha = WorldOracleBytes.IsError()
		? FString() : Sha256(WorldOracleBytes.GetValue()).ToHex();
	if (WorldOracleSha
		!= ExpectedWorldImporterManifestSha)
	{
		AddError(FString::Printf(
			TEXT("real-world builder components contract_sha256=%s installer_sha256=%s"),
			*ComputeRetailContractIdentity(WorldContract.GetValue()).ToHex(),
			*InstallerSha.ToHex()));
		for (const FWoTCacheBlob& Blob : WorldBuilt.GetValue().Manifest.Blobs)
		{
			AddError(FString::Printf(
				TEXT("real-world builder object %s size=%llu sha256=%s"),
				*Blob.LogicalId, static_cast<unsigned long long>(Blob.Size),
				*Blob.Sha256.ToHex()));
		}
	}
	TestEqual(TEXT("real-world importer builder manifest SHA-256 pin"),
		WorldOracleSha,
		ExpectedWorldImporterManifestSha);

	FWoTCacheBuilderSpecWriter Writer;
	FWoTCacheBuilderSpecProgress Progress;
	TArray<FWoTCacheImporterForTesting> Importers =
		WoTCacheBuilderSpecRetailImporters(Contract, Sources);
	TWoTBootstrapResult<FWoTCacheBuildResult> Built = BuildRetailCacheForTesting(
		InstallerSha, Contract, Writer, Progress,
		Descriptor, Sizes, Importers);
	if (!TestFalse(TEXT("real System/Maps cache pin builds"), Built.IsError()))
	{
		if (Built.IsError()) AddError(Built.GetError().ToString());
		return false;
	}
	// Updated for the world manifest metadata; no payload bytes or signed facts change.
	// Verified two-sided 2026-08-10: an independent Python reconstruction of
	// the contract identity, installer hash, and canonical manifest over the
	// same 68 retail sources (case-insensitive source ordering, matching
	// FString::Sort) reproduces this digest exactly.
	const FString ExpectedManifestSha = TEXT(
		"c08d14d16b2015bfc0684b6009f6eec14456851e725366352258b44285dbfd27");
	FWoTCacheManifestV3 RetailOracleManifest = Built.GetValue().Manifest;
	RetailOracleManifest.ProductionDescriptorSha256 = PriorProductionDescriptorSha;
	TWoTBootstrapResult<TArray<uint8>> RetailOracleBytes =
		WriteCanonicalCacheManifest(RetailOracleManifest);
	const FString RetailOracleSha = RetailOracleBytes.IsError()
		? FString() : Sha256(RetailOracleBytes.GetValue()).ToHex();
	if (RetailOracleSha != ExpectedManifestSha)
	{
		TMap<FString, int32> Counts;
		for (const FWoTCacheBlob& Blob : Built.GetValue().Manifest.Blobs)
		{
			const FString Domain = Blob.LogicalId.Left(
				Blob.LogicalId.Find(TEXT("/"), ESearchCase::CaseSensitive,
					ESearchDir::FromStart, 6));
			++Counts.FindOrAdd(Domain);
			AddError(FString::Printf(TEXT("retail pin object %s size=%llu sha256=%s"),
				*Blob.LogicalId, static_cast<unsigned long long>(Blob.Size),
				*Blob.Sha256.ToHex()));
		}
		for (const TPair<FString, int32>& Count : Counts)
			AddError(FString::Printf(TEXT("retail pin domain %s objects=%d"),
				*Count.Key, Count.Value));
	}
	TestEqual(TEXT("real retail manifest SHA-256 pin"),
		RetailOracleSha, ExpectedManifestSha);


	return !HasAnyErrors();
}
}
#endif
