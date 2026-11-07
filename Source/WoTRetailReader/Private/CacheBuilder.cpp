#include "WoTRetailReader/CacheBuilder.h"

#include "CanonicalObjectBuilder.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "WoTBootstrapCore/CanonicalJson.h"
#include "WoTBootstrapCore/Hash.h"
#include "WoTRetailReader/BSPWorld.h"
#include "WoTRetailReader/FontProcedural.h"
#include "WoTRetailReader/FrontEndDescriptorSidecar.h"
#include "WoTRetailReader/FrontEndWorld.h"
#include "WoTRetailReader/MeshAnimation.h"
#include "WoTRetailReader/Package.h"
#include "WoTRetailReader/RegistryLocalization.h"
#include "WoTRetailReader/TextureSound.h"

namespace
{
constexpr TCHAR WoTCacheProductionDescriptorRelativePath[] =
	TEXT("Build/WoTRelease/cache-v3-production-descriptor.json");
// Updated together with the checked-in descriptor after the Shipping source
// inventory is regenerated. A different canonical document is a substitute,
// not the production input this binary was reviewed against. The descriptor
// is validated while a cache is built; it never reaches the cache itself,
// which records the cache content label below.
constexpr TCHAR WoTCacheExpectedProductionDescriptorSha256[] =
	TEXT("d0affffb73ba86e2ebac95571e1d75787918ecab467db67bef289efaa08e1df6");
constexpr TCHAR WoTCacheExpectedImporterSpecSha256[] =
	TEXT("63a333a7d7cffe985958e5790272ebfe0c844fe64c6920361a79c883817e8044");
// Domain-separated ComputeRetailContractIdentity of the reviewed compiled
// production contract. Updated with its generated payload and descriptor pin.
constexpr TCHAR WoTCacheExpectedProductionContractIdentity[] =
	TEXT("83d74231a05ac8211b401969be83a6860a43729497eda27c231bf001c5b791e0");
// The cache content revision. Bump it when, for the same installer and
// contract, any cache object's bytes, the manifest metadata or the front-end
// sidecar would differ. The release gate refuses a changed cache under an
// unchanged revision.
constexpr uint32 WoTCacheContentRevision = 1;
// Revision 1 keeps the digest the first public release recorded, so caches
// built before and after this labelling scheme are byte-identical.
constexpr TCHAR WoTCacheRevision1Label[] =
	TEXT("1a7e19b367523c11d9ad6c7fda42a39472a68d4b8c52233b6b2d568ca3daa58e");

#if WITH_AUTOMATION_TESTS
FString GWoTCacheProductionDescriptorPathForTesting;
FString GWoTCacheProductionDescriptorPinForTesting;
#endif

struct FWoTCacheDescriptorOutput
{
	const TCHAR* Domain;
	const TCHAR* Importer;
	const TCHAR* LogicalRoot;
	const TCHAR* Schema;
	const TCHAR* RuntimeConsumer;
	const TCHAR* AuditRule;
	uint64 MaximumBytes;
};

constexpr FWoTCacheDescriptorOutput WoTCacheDescriptorOutputs[] = {
	{TEXT("animation"), TEXT("animation-v1"), TEXT("cache/animation"),
		TEXT("wot-animation-v3"), TEXT("UWoTVertexAnimComponent"),
		TEXT("audit.animation-payload-v1"), 536870912ull},
	{TEXT("bsp"), TEXT("bsp-v1"), TEXT("cache/bsp"), TEXT("wot-bsp-v3"),
		TEXT("UWoTPayloadSubsystem"), TEXT("audit.bsp-payload-v1"), 1073741824ull},
	{TEXT("class_default"), TEXT("class-default-v2"), TEXT("cache/class_default"),
		TEXT("wot-class-default-v3"), TEXT("FWoTWorldMaterializer"),
		TEXT("audit.class-default-payload-v1"), 268435456ull},
	{TEXT("font"), TEXT("font-v2"), TEXT("cache/font"), TEXT("wot-font-v3"),
		TEXT("FWoTFrontEndFontOwner"), TEXT("audit.font-payload-v2"), 268435456ull},
	{TEXT("front_end_world"), TEXT("front-end-world-v1"),
		TEXT("cache/front_end_world"), TEXT("wot-front-end-world-v3"),
		TEXT("FWoTCacheGenerationLease"),
		TEXT("audit.front-end-world-payload-v1"), 4194304ull},
	{TEXT("localization"), TEXT("localization-v1"), TEXT("cache/localization"),
		TEXT("wot-localization-v3"), TEXT("FWoTLocalizationRegistry"),
		TEXT("audit.localization-payload-v1"), 67108864ull},
	{TEXT("mesh"), TEXT("mesh-v1"), TEXT("cache/mesh"), TEXT("wot-mesh-v3"),
		TEXT("UWoTPayloadSubsystem"), TEXT("audit.mesh-payload-v1"), 536870912ull},
	{TEXT("procedural"), TEXT("procedural-v1"), TEXT("cache/procedural"),
		TEXT("wot-procedural-v3"), TEXT("UWoTFractalSubsystem"),
		TEXT("audit.procedural-payload-v1"), 268435456ull},
	{TEXT("registry"), TEXT("registry-v1"), TEXT("cache/registry"),
		TEXT("wot-registry-v3"), TEXT("UWoTPayloadSubsystem"),
		TEXT("audit.registry-payload-v1"), 268435456ull},
	{TEXT("sound"), TEXT("sound-v1"), TEXT("cache/sound"), TEXT("wot-sound-v3"),
		TEXT("UWoTPayloadSubsystem"), TEXT("audit.sound-payload-v1"), 268435456ull},
	{TEXT("texture"), TEXT("texture-v2"), TEXT("cache/texture"),
		TEXT("wot-texture-v3"), TEXT("UWoTPayloadSubsystem"),
		TEXT("audit.texture-payload-v1"), 268435456ull},
	{TEXT("world"), TEXT("world-v1"), TEXT("cache/world"), TEXT("wot-world-v3"),
		TEXT("UWoTPayloadSubsystem"), TEXT("audit.world-payload-v1"), 1073741824ull},
};

struct FWoTCacheDeclaredOutput
{
	FString LogicalId;
	FString Schema;
	FString Importer;
	FString Stage;
	uint64 MaximumBytes = 0;
	TArray<FWoTRequiredFactId> Satisfies;
};

struct FWoTCacheImporterBoundary
{
	FString Stage;
	TArray<FString> Importers;
	TFunction<TWoTBootstrapResult<TArray<FWoTCacheBlob>>(
		IWoTObjectWriter&, IWoTProgressSink&)> Run;

};

FWoTBootstrapError WoTCacheError(EWoTBootstrapError Code, FString Message,
	FString Path = FString())
{
	return MakeWoTBootstrapError(Code, MoveTemp(Message), MoveTemp(Path));
}

FWoTBootstrapError WoTCacheCancelled(FStringView Stage)
{
	return WoTCacheError(EWoTBootstrapError::Io,
		TEXT("the cache build was interrupted at a deterministic boundary"),
		FString(Stage));
}

void WoTCacheHashUnsigned(FWoTSha256Hasher& Hasher, uint64 Value)
{
	uint8 Bytes[8];
	for (uint32 ByteIndex = 0; ByteIndex < UE_ARRAY_COUNT(Bytes); ++ByteIndex)
	{
		Bytes[ByteIndex] = static_cast<uint8>(Value >> (ByteIndex * 8));
	}
	Hasher.Update(MakeArrayView(Bytes));
}

void WoTCacheHashString(FWoTSha256Hasher& Hasher, FStringView Value)
{
	const FString Owned(Value);
	const FTCHARToUTF8 Utf8(*Owned);
	WoTCacheHashUnsigned(Hasher, static_cast<uint64>(Utf8.Length()));
	Hasher.Update(TConstArrayView<uint8>(
		reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()));
}

void WoTCacheHashSha(FWoTSha256Hasher& Hasher, const FWoTSha256& Value)
{
	Hasher.Update(MakeArrayView(Value.Bytes));
}

bool WoTCacheFactIdsEqual(TConstArrayView<FWoTRequiredFactId> Left,
	TConstArrayView<FWoTRequiredFactId> Right)
{
	if (Left.Num() != Right.Num()) return false;
	TArray<FString> LeftValues;
	TArray<FString> RightValues;
	for (const FWoTRequiredFactId& Id : Left) LeftValues.Add(Id.Value);
	for (const FWoTRequiredFactId& Id : Right) RightValues.Add(Id.Value);
	LeftValues.Sort();
	RightValues.Sort();
	return LeftValues == RightValues;
}

const FWoTCacheDescriptorOutput* WoTCacheDescriptorOutputByImporter(
	FStringView Importer)
{
	for (const FWoTCacheDescriptorOutput& Output : WoTCacheDescriptorOutputs)
	{
		if (Importer == Output.Importer) return &Output;
	}
	return nullptr;
}

FString WoTCacheStageForImporter(FStringView Importer)
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
	if (Importer == TEXT("registry-v1") || Importer == TEXT("localization-v1")
		|| Importer == TEXT("class-default-v2"))
		return TEXT("registry-localization-defaults");
	return FString();
}

TOptional<FWoTBootstrapError> WoTCacheRequireShape(
	const FWoTCanonicalJsonValue& Object, TConstArrayView<const TCHAR*> Fields,
	const TCHAR* What)
{
	if (TOptional<FWoTBootstrapError> Error = RequireExactMembers(
			Object, Fields, What); Error.IsSet())
	{
		return WoTCacheError(Error->Code,
			FString(What) + TEXT(" has a missing or unknown field"), What);
	}
	return {};
}

FWoTBootstrapStatus WoTCacheValidateProductionDescriptor(
	TConstArrayView<uint8> CanonicalBytes)
{
	TWoTBootstrapResult<FWoTCanonicalJsonValue> Parsed =
		ParseCanonicalJson(CanonicalBytes);
	if (Parsed.IsError())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(Parsed).GetError());
	}
	const FWoTCanonicalJsonValue& Root = Parsed.GetValue();
	const TCHAR* RootFields[] = {TEXT("format"), TEXT("graph"),
		TEXT("graph_sha256"), TEXT("importer_outputs"), TEXT("importer_spec")};
	if (TOptional<FWoTBootstrapError> Error = WoTCacheRequireShape(
			Root, MakeArrayView(RootFields), TEXT("production-descriptor")); Error.IsSet())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(Error.GetValue()));
	}
	FString Format;
	if (ReadStringField(Root, TEXT("format"), TEXT("production-descriptor"), Format).IsSet()
		|| Format != TEXT("cache-v3-production-descriptor-v1"))
	{
		return FWoTBootstrapStatus::Error(WoTCacheError(EWoTBootstrapError::UnknownField,
			TEXT("the production descriptor format is not cache-v3-production-descriptor-v1"),
			TEXT("production-descriptor.format")));
	}

	const FWoTCanonicalJsonValue* Graph = nullptr;
	if (TOptional<FWoTBootstrapError> Error = ReadObjectField(Root, TEXT("graph"),
			TEXT("production-descriptor"), Graph); Error.IsSet())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(Error.GetValue()));
	}
	TWoTBootstrapResult<TArray<uint8>> CanonicalGraph = WriteCanonicalJson(*Graph);
	FString GraphShaHex;
	if (CanonicalGraph.IsError()
		|| ReadStringField(Root, TEXT("graph_sha256"),
			TEXT("production-descriptor"), GraphShaHex).IsSet())
	{
		return FWoTBootstrapStatus::Error(WoTCacheError(EWoTBootstrapError::UnknownField,
			TEXT("the production descriptor graph identity is malformed"),
			TEXT("production-descriptor.graph_sha256")));
	}
	TWoTBootstrapResult<FWoTSha256> GraphSha = FWoTSha256::FromHex(GraphShaHex);
	if (GraphSha.IsError() || Sha256(CanonicalGraph.GetValue()) != GraphSha.GetValue())
	{
		return FWoTBootstrapStatus::Error(WoTCacheError(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("the production descriptor graph digest does not match its graph"),
			TEXT("production-descriptor.graph_sha256")));
	}

	const FWoTCanonicalJsonValue* ImporterSpec = nullptr;
	if (TOptional<FWoTBootstrapError> Error = ReadObjectField(Root,
			TEXT("importer_spec"), TEXT("production-descriptor"), ImporterSpec);
		Error.IsSet())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(Error.GetValue()));
	}
	const TCHAR* SpecFields[] = {TEXT("path"), TEXT("sha256")};
	if (TOptional<FWoTBootstrapError> Error = WoTCacheRequireShape(
			*ImporterSpec, MakeArrayView(SpecFields),
			TEXT("production-descriptor.importer_spec")); Error.IsSet())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(Error.GetValue()));
	}
	FString SpecPath;
	FString SpecSha;
	if (ReadStringField(*ImporterSpec, TEXT("path"), TEXT("importer_spec"),
			SpecPath).IsSet()
		|| ReadStringField(*ImporterSpec, TEXT("sha256"), TEXT("importer_spec"),
			SpecSha).IsSet()
		|| SpecPath != TEXT("Tools/dist/cache-v3-importer-outputs-v1.json")
		|| SpecSha != WoTCacheExpectedImporterSpecSha256)
	{
		return FWoTBootstrapStatus::Error(WoTCacheError(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("the descriptor is not bound to the production importer registry"),
			TEXT("production-descriptor.importer_spec")));
	}

	const FWoTCanonicalJsonValue* Outputs = Root.FindMember(TEXT("importer_outputs"));
	if (Outputs == nullptr || !Outputs->IsArray())
	{
		return FWoTBootstrapStatus::Error(WoTCacheError(EWoTBootstrapError::UnknownField,
			TEXT("the descriptor importer output inventory is not an array"),
			TEXT("production-descriptor.importer_outputs")));
	}
	TSet<FString> SeenImporters;
	for (int32 OutputIndex = 0; OutputIndex < Outputs->Num(); ++OutputIndex)
	{
		const FWoTCanonicalJsonValue& Value = Outputs->GetElement(OutputIndex);
		FString Importer;
		if (ReadStringField(Value, TEXT("importer"), TEXT("importer-output"),
				Importer).IsSet())
		{
			return FWoTBootstrapStatus::Error(WoTCacheError(
				EWoTBootstrapError::UnknownField,
				TEXT("an importer output has no importer identity"),
				TEXT("production-descriptor.importer_outputs.importer")));
		}
		const FString OutputContext = FString(
			TEXT("production-descriptor.importer_outputs.")) + Importer;
		if (Value.FindMember(TEXT("runtime_consumer")) == nullptr)
		{
			return FWoTBootstrapStatus::Error(WoTCacheError(
				EWoTBootstrapError::UnknownField,
				TEXT("a declared output is missing its Shipping consumer/load edge"),
				OutputContext + TEXT(".shipping-edge")));
		}
		const TCHAR* Fields[] = {TEXT("audit_rule"), TEXT("domain"), TEXT("importer"),
			TEXT("logical_id"), TEXT("max_decoded_size"), TEXT("runtime_consumer"),
			TEXT("schema")};
		if (TOptional<FWoTBootstrapError> Error = WoTCacheRequireShape(Value,
				MakeArrayView(Fields), TEXT("production-descriptor.importer-output"));
			Error.IsSet())
		{
			return FWoTBootstrapStatus::Error(MoveTemp(Error.GetValue()));
		}
		const FWoTCacheDescriptorOutput* Expected =
			WoTCacheDescriptorOutputByImporter(Importer);
		if (Expected == nullptr || SeenImporters.Contains(Importer))
		{
			return FWoTBootstrapStatus::Error(WoTCacheError(
				EWoTBootstrapError::DuplicateField,
				TEXT("the descriptor has an unknown or duplicate importer output"), Importer));
		}
		SeenImporters.Add(Importer);
		FString Domain, LogicalId, Schema, Consumer, Audit;
		uint64 Maximum = 0;
		if (ReadStringField(Value, TEXT("domain"), TEXT("importer-output"), Domain).IsSet()
			|| ReadStringField(Value, TEXT("logical_id"), TEXT("importer-output"),
				LogicalId).IsSet()
			|| ReadStringField(Value, TEXT("schema"), TEXT("importer-output"), Schema).IsSet()
			|| ReadStringField(Value, TEXT("runtime_consumer"), TEXT("importer-output"),
				Consumer).IsSet()
			|| ReadStringField(Value, TEXT("audit_rule"), TEXT("importer-output"),
				Audit).IsSet()
			|| ReadUnsignedField(Value, TEXT("max_decoded_size"),
				TEXT("importer-output"), Maximum).IsSet()
			|| Domain != Expected->Domain || LogicalId != Expected->LogicalRoot
			|| Schema != Expected->Schema || Audit != Expected->AuditRule
			|| Maximum != Expected->MaximumBytes)
		{
			return FWoTBootstrapStatus::Error(WoTCacheError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("a production importer output differs from its reviewed definition"),
				OutputContext + TEXT(".definition")));
		}
		if (Consumer != Expected->RuntimeConsumer)
		{
			return FWoTBootstrapStatus::Error(WoTCacheError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("a declared output is missing its exact Shipping consumer/load edge"),
				OutputContext + TEXT(".shipping-edge")));
		}
	}
	for (const FWoTCacheDescriptorOutput& Expected : WoTCacheDescriptorOutputs)
	{
		if (!SeenImporters.Contains(Expected.Importer))
		{
			return FWoTBootstrapStatus::Error(WoTCacheError(
				EWoTBootstrapError::UnknownField,
				TEXT("the descriptor omits a production importer output"),
				FString(TEXT("production-descriptor.importer_outputs."))
					+ Expected.Importer));
		}
	}

	// Semantic causes are reported before this exact-document identity gate so
	// descriptor mutation tests retain their specific rejection contexts.
	const FWoTSha256 ActualDescriptorSha = Sha256(CanonicalBytes);
	FStringView ExpectedDescriptorHex = WoTCacheExpectedProductionDescriptorSha256;
#if WITH_AUTOMATION_TESTS
	if (!GWoTCacheProductionDescriptorPinForTesting.IsEmpty())
	{
		ExpectedDescriptorHex = GWoTCacheProductionDescriptorPinForTesting;
	}
#endif
	TWoTBootstrapResult<FWoTSha256> ExpectedDescriptorSha =
		FWoTSha256::FromHex(ExpectedDescriptorHex);
	if (ExpectedDescriptorSha.IsError()
		|| ActualDescriptorSha != ExpectedDescriptorSha.GetValue())
	{
		return FWoTBootstrapStatus::Error(WoTCacheError(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("the production descriptor is stale or is a caller-supplied substitute"),
			WoTCacheProductionDescriptorRelativePath));
	}
	return FWoTBootstrapStatus::Success();
}

TWoTBootstrapResult<TArray<uint8>> WoTCacheLoadProductionDescriptor()
{
	using FResult = TWoTBootstrapResult<TArray<uint8>>;
	TArray<FString> Candidates;
	Candidates.Add(FPaths::ConvertRelativePathToFull(
		FPaths::ProjectDir() / WoTCacheProductionDescriptorRelativePath));
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	// Development tools run beside the source tree. A packaged game must find the
	// descriptor it shipped with (staged by WoTRetailReader.Build.cs), never the
	// build machine's copy at the compile-time source path.
	const FString ThisFile(ANSI_TO_TCHAR(__FILE__));
	Candidates.Add(FPaths::ConvertRelativePathToFull(FPaths::GetPath(ThisFile)
		/ TEXT("../../../") / WoTCacheProductionDescriptorRelativePath));
#endif
	for (const FString& Candidate : Candidates)
	{
		// Probe first: an engine-free Program's ProjectDir candidate is
		// expected to miss, and LoadFileToArray logs every miss as a warning.
		TArray<uint8> Bytes;
		if (FPaths::FileExists(Candidate)
			&& FFileHelper::LoadFileToArray(Bytes, *Candidate))
		{
			return FResult::Success(MoveTemp(Bytes));
		}
	}
	return FResult::Error(WoTCacheError(EWoTBootstrapError::Io,
		TEXT("the required checked-in production descriptor cannot be read"),
		WoTCacheProductionDescriptorRelativePath));
}

TWoTBootstrapResult<TMap<FString, FWoTCacheDeclaredOutput>>
WoTCacheDeclaredOutputs(const FWoTRetailContract& Contract)
{
	using FResult = TWoTBootstrapResult<TMap<FString, FWoTCacheDeclaredOutput>>;
	TMap<FString, FWoTCacheDeclaredOutput> Declared;
	for (const FWoTRequiredFact& Fact : Contract.Facts)
	{
		if (Fact.Outputs.IsEmpty()) continue;
		const FWoTCacheDescriptorOutput* Descriptor =
			WoTCacheDescriptorOutputByImporter(Fact.Importer);
		const FString Stage = WoTCacheStageForImporter(Fact.Importer);
		if (Descriptor == nullptr || Stage.IsEmpty()
			|| Fact.RuntimeConsumer != Descriptor->RuntimeConsumer
			|| Fact.AuditRule != Descriptor->AuditRule)
		{
			return FResult::Error(WoTCacheError(EWoTBootstrapError::UnknownField,
				TEXT("a contract output has no exact production descriptor edge"),
				Fact.FactId));
		}
		for (const FWoTRequiredFactOutput& Output : Fact.Outputs)
		{
			if ((Output.LogicalId != Descriptor->LogicalRoot
					&& !Output.LogicalId.StartsWith(
						FString(Descriptor->LogicalRoot) + TEXT("/"),
						ESearchCase::CaseSensitive))
				|| Output.Schema != Descriptor->Schema
				|| Output.MaxDecodedSize > Descriptor->MaximumBytes)
			{
				return FResult::Error(WoTCacheError(EWoTBootstrapError::UnknownField,
					TEXT("an importer output is outside the production descriptor"),
					Output.LogicalId));
			}
			FWoTCacheDeclaredOutput* Existing = Declared.Find(Output.LogicalId);
			if (Existing != nullptr)
			{
				if (Existing->Schema != Output.Schema || Existing->Importer != Fact.Importer)
				{
					return FResult::Error(WoTCacheError(EWoTBootstrapError::DuplicateField,
						TEXT("one logical output is declared with conflicting semantics"),
						Output.LogicalId));
				}
				Existing->MaximumBytes = FMath::Min(
					Existing->MaximumBytes, Output.MaxDecodedSize);
				Existing->Satisfies.Add(FWoTRequiredFactId{Fact.FactId});
			}
			else
			{
				FWoTCacheDeclaredOutput Item;
				Item.LogicalId = Output.LogicalId;
				Item.Schema = Output.Schema;
				Item.Importer = Fact.Importer;
				Item.Stage = Stage;
				Item.MaximumBytes = Output.MaxDecodedSize;
				Item.Satisfies.Add(FWoTRequiredFactId{Fact.FactId});
				Declared.Add(Item.LogicalId, MoveTemp(Item));
			}
		}
	}
	return FResult::Success(MoveTemp(Declared));
}

TMap<FString, uint64> WoTCacheContractSourceInventory(
	const FWoTRetailContract& Contract)
{
	TMap<FString, uint64> Sizes;
	for (const FWoTRequiredFact& Fact : Contract.Facts)
	{
		Sizes.FindOrAdd(Fact.SourcePath) = 0;
	}
	return Sizes;
}

class FWoTCacheCoordinatingWriter final : public IWoTObjectWriter
{
public:
	FWoTCacheCoordinatingWriter(IWoTObjectWriter& InDestination,
		const TMap<FString, FWoTCacheDeclaredOutput>& InDeclared,
		FWoTCacheProgressAggregator& InProgress)
		: Destination(InDestination)
		, Declared(InDeclared)
		, Progress(InProgress)
	{
	}

	void SetStage(FStringView InStage)
	{
		Stage = FString(InStage);
		StageObjects.Reset();
	}

	virtual FWoTBootstrapStatus WriteObject(FStringView LogicalId,
		FStringView Schema, TConstArrayView<uint8> Bytes,
		const FWoTSha256& Sha256Digest, uint64 MaximumBytes) override
	{
		const FWoTCacheDeclaredOutput* Expected = Declared.Find(FString(LogicalId));
		if (Expected == nullptr || Expected->Stage != Stage
			|| Expected->Schema != Schema || Expected->MaximumBytes != MaximumBytes)
		{
			return FWoTBootstrapStatus::Error(WoTCacheError(
				EWoTBootstrapError::UnknownField,
				TEXT("an importer attempted an undeclared or misbounded output"),
				FString(LogicalId)));
		}
		if (WrittenLogicalIds.Contains(Expected->LogicalId))
		{
			return FWoTBootstrapStatus::Error(WoTCacheError(
				EWoTBootstrapError::DuplicateField,
				TEXT("an importer attempted to write one logical object twice"),
				Expected->LogicalId));
		}
		if (static_cast<uint64>(Bytes.Num()) > MaximumBytes)
		{
			return FWoTBootstrapStatus::Error(WoTCacheError(
				EWoTBootstrapError::SizeLimit,
				TEXT("an importer object exceeds its contract maximum"),
				Expected->LogicalId));
		}

		if (Progress.IsCancellationRequested())
			return FWoTBootstrapStatus::Error(WoTCacheCancelled(Stage));
		FWoTCacheBlob Blob;
		Blob.LogicalId = Expected->LogicalId;
		Blob.Schema = Expected->Schema;
		Blob.Sha256 = Sha256Digest;
		Blob.Size = static_cast<uint64>(Bytes.Num());
		Blob.Satisfies = Expected->Satisfies;
		FWoTBootstrapStatus Written = Destination.WriteObject(
			LogicalId, Schema, Bytes, Sha256Digest, MaximumBytes);
		if (Written.IsError()) return Written;
		WrittenLogicalIds.Add(Blob.LogicalId);
		StageObjects.Add(Blob);
		AllObjects.Add(MoveTemp(Blob));
		if (Progress.IsCancellationRequested())
			return FWoTBootstrapStatus::Error(WoTCacheCancelled(Stage));
		return FWoTBootstrapStatus::Success();
	}

	FWoTBootstrapStatus ValidateImporterResult(
		TConstArrayView<FWoTCacheBlob> Returned) const
	{
		TSet<FString> Seen;
		for (const FWoTCacheBlob& Expected : StageObjects)
		{
			const FWoTCacheBlob* Match = nullptr;
			for (const FWoTCacheBlob& Candidate : Returned)
			{
				if (Candidate.LogicalId == Expected.LogicalId)
				{
					Match = &Candidate;
					break;
				}
			}
			if (Match == nullptr || Match->Schema != Expected.Schema
				|| Match->Size != Expected.Size || Match->Sha256 != Expected.Sha256
				|| !WoTCacheFactIdsEqual(Match->Satisfies, Expected.Satisfies))
			{
				return FWoTBootstrapStatus::Error(WoTCacheError(
					EWoTBootstrapError::CorruptInstallation,
					TEXT("an importer returned metadata that disagrees with written bytes"),
					Expected.LogicalId));
			}
		}
		for (const FWoTCacheBlob& Candidate : Returned)
		{
			if (Seen.Contains(Candidate.LogicalId))
			{
				return FWoTBootstrapStatus::Error(WoTCacheError(
					EWoTBootstrapError::DuplicateField,
					TEXT("an importer returned duplicate object metadata"),
					Candidate.LogicalId));
			}
			Seen.Add(Candidate.LogicalId);
			const FWoTCacheBlob* Written = StageObjects.FindByPredicate(
				[&Candidate](const FWoTCacheBlob& Object)
				{
					return Object.LogicalId == Candidate.LogicalId;
				});
			if (Written != nullptr)
			{
				continue;
			}
            return FWoTBootstrapStatus::Error(WoTCacheError(
                EWoTBootstrapError::CorruptInstallation,
                TEXT("an importer returned metadata for an unwritten object"),
                Candidate.LogicalId));
		}
		return FWoTBootstrapStatus::Success();
	}

	const TArray<FWoTCacheBlob>& GetAllObjects() const { return AllObjects; }

private:
	IWoTObjectWriter& Destination;
	const TMap<FString, FWoTCacheDeclaredOutput>& Declared;
	FWoTCacheProgressAggregator& Progress;
	FString Stage;
	TSet<FString> WrittenLogicalIds;
	TArray<FWoTCacheBlob> StageObjects;
	TArray<FWoTCacheBlob> AllObjects;
};

TWoTBootstrapResult<FWoTCacheBuildResult> WoTCacheBuildWithInputs(
	const FWoTSha256& InstallerSha, const FWoTRetailContract& Contract,
	IWoTObjectWriter& Destination,
	IWoTProgressSink& Downstream, TConstArrayView<uint8> DescriptorBytes,
	const TMap<FString, uint64>& SourceSizes,
	TArray<FWoTCacheImporterBoundary>& Boundaries,
	TFunctionRef<FWoTBootstrapStatus(FWoTCacheManifestV3&)> BeforeCompletion)
{
	using FResult = TWoTBootstrapResult<FWoTCacheBuildResult>;
	FWoTBootstrapStatus DescriptorStatus =
		WoTCacheValidateProductionDescriptor(DescriptorBytes);
	if (DescriptorStatus.IsError())
		return FResult::Error(MoveTemp(DescriptorStatus).GetError());
	FWoTBootstrapStatus Closure = ValidateFactClosure(Contract);
	if (Closure.IsError()) return FResult::Error(MoveTemp(Closure).GetError());
	TWoTBootstrapResult<TMap<FString, FWoTCacheDeclaredOutput>> DeclaredResult =
		WoTCacheDeclaredOutputs(Contract);
	if (DeclaredResult.IsError())
		return FResult::Error(MoveTemp(DeclaredResult).GetError());
	TMap<FString, FWoTCacheDeclaredOutput> Declared =
		MoveTemp(DeclaredResult).GetValue();

	TMap<FString, int32> StageIndices;
	for (int32 BoundaryIndex = 0; BoundaryIndex < Boundaries.Num(); ++BoundaryIndex)
	{
		const FWoTCacheImporterBoundary& Boundary = Boundaries[BoundaryIndex];
		if (Boundary.Stage.IsEmpty() || !Boundary.Run || StageIndices.Contains(Boundary.Stage))
		{
			return FResult::Error(WoTCacheError(EWoTBootstrapError::DuplicateField,
				TEXT("cache importer boundaries must be complete and unique"), Boundary.Stage));
		}
		StageIndices.Add(Boundary.Stage, BoundaryIndex);
	}
	for (const TPair<FString, FWoTCacheDeclaredOutput>& Pair : Declared)
	{
		if (!StageIndices.Contains(Pair.Value.Stage))
		{
			return FResult::Error(WoTCacheError(EWoTBootstrapError::UnknownField,
				TEXT("a declared output has no native importer boundary"), Pair.Key));
		}
	}

	TArray<uint64> StageUnits;
	StageUnits.Init(0, Boundaries.Num());
	const uint64 TotalUnits = static_cast<uint64>(Contract.Facts.Num());
	TSet<FString> DeclaredSources;
	for (const FWoTRequiredFact& Fact : Contract.Facts)
	{
		DeclaredSources.Add(Fact.SourcePath);
	}
	if (DeclaredSources.Num() != SourceSizes.Num())
	{
		return FResult::Error(WoTCacheError(EWoTBootstrapError::UnknownField,
			TEXT("bounded source progress must cover exactly the contract sources"),
			TEXT("cache-progress.sources")));
	}
	for (const FString& DeclaredSource : DeclaredSources)
	{
		if (!SourceSizes.Contains(DeclaredSource))
		{
			return FResult::Error(WoTCacheError(EWoTBootstrapError::UnknownField,
				TEXT("bounded source progress omits a contract source"), DeclaredSource));
		}
	}
	for (const FWoTRequiredFact& Fact : Contract.Facts)
	{
		const FString Stage = WoTCacheStageForImporter(Fact.Importer);
		if (const int32* StageIndex = StageIndices.Find(Stage))
		{
			++StageUnits[*StageIndex];
		}
	}
	const FWoTSha256 ContractSha = ComputeRetailContractIdentity(Contract);
	TWoTBootstrapResult<FWoTSha256> ContentLabelResult = GetCacheContentLabel();
	if (ContentLabelResult.IsError())
		return FResult::Error(MoveTemp(ContentLabelResult).GetError());
	const FWoTSha256 ContentLabel = ContentLabelResult.GetValue();
    FWoTCacheProgressAggregator Progress(Downstream, TotalUnits);
    FWoTCacheCoordinatingWriter Writer(Destination, Declared, Progress);
	TArray<FWoTCacheBlob> ManifestBlobs;
	uint64 CompletedStageUnits = 0;
	for (int32 BoundaryIndex = 0; BoundaryIndex < Boundaries.Num(); ++BoundaryIndex)
	{
		FWoTCacheImporterBoundary& Boundary = Boundaries[BoundaryIndex];
		FWoTBootstrapStatus Begun = Progress.BeginStage(
			Boundary.Stage, CompletedStageUnits, StageUnits[BoundaryIndex]);
		if (Begun.IsError()) return FResult::Error(MoveTemp(Begun).GetError());
        if (Progress.IsCancellationRequested())
            return FResult::Error(WoTCacheCancelled(Boundary.Stage));
        Writer.SetStage(Boundary.Stage);
        const int32 BeforeCount = Writer.GetAllObjects().Num();
        auto Imported = Boundary.Run(Writer, Progress);
        if (Imported.IsError()) return FResult::Error(MoveTemp(Imported).GetError());
        auto Inventory = Writer.ValidateImporterResult(Imported.GetValue());
        if (Inventory.IsError()) return FResult::Error(MoveTemp(Inventory).GetError());
        for (int32 ObjectIndex = BeforeCount; ObjectIndex < Writer.GetAllObjects().Num(); ++ObjectIndex)
            ManifestBlobs.Add(Writer.GetAllObjects()[ObjectIndex]);
        if (Progress.IsCancellationRequested())
            return FResult::Error(WoTCacheCancelled(Boundary.Stage));
		CompletedStageUnits += StageUnits[BoundaryIndex];
		FWoTBootstrapStatus Completed = Progress.CompleteStage();
		if (Completed.IsError())
			return FResult::Error(MoveTemp(Completed).GetError());
	}

	if (ManifestBlobs.Num() != Declared.Num())
	{
		return FResult::Error(WoTCacheError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the completed importer inventory does not satisfy every declared output")));
	}
	FWoTCacheBuildResult Result;
	Result.Manifest.bHasBuildIdentity = true;
	Result.Manifest.InstallerSha256 = InstallerSha;
	Result.Manifest.ContractSha256 = ContractSha;
	Result.Manifest.ProductionDescriptorSha256 = ContentLabel;
	Result.Manifest.Blobs = MoveTemp(ManifestBlobs);
    FWoTBootstrapStatus Finalized = BeforeCompletion(Result.Manifest);
    if (Finalized.IsError()) return FResult::Error(MoveTemp(Finalized).GetError());
	TWoTBootstrapResult<TArray<uint8>> ManifestBytes =
		WriteCanonicalCacheManifest(Result.Manifest);
	if (ManifestBytes.IsError())
		return FResult::Error(MoveTemp(ManifestBytes).GetError());
	Result.CanonicalManifest = MoveTemp(ManifestBytes).GetValue();
	Result.ManifestSha256 = Sha256(Result.CanonicalManifest);
	FWoTBootstrapStatus Finished = Progress.CompleteBuild();
	if (Finished.IsError()) return FResult::Error(MoveTemp(Finished).GetError());
	Result.Progress = Progress.Snapshot();
	return FResult::Success(MoveTemp(Result));
}
}

uint32 GetCacheContentRevision()
{
	return WoTCacheContentRevision;
}

TWoTBootstrapResult<FWoTSha256> ComputeCacheContentLabel(uint32 Revision)
{
	using FResult = TWoTBootstrapResult<FWoTSha256>;
	if (Revision == 0)
	{
		return FResult::Error(WoTCacheError(EWoTBootstrapError::InvalidNumber,
			TEXT("cache content revisions start at 1"),
			TEXT("cache-content-revision")));
	}
	if (Revision == 1)
	{
		return FWoTSha256::FromHex(WoTCacheRevision1Label);
	}
	FWoTSha256Hasher Hasher;
	// The domain includes its terminating NUL, framed like the contract identity.
	static const ANSICHAR Domain[] = "WOT-CACHE-CONTENT-IDENTITY-V1";
	Hasher.Update(TConstArrayView<uint8>(reinterpret_cast<const uint8*>(Domain),
		UE_ARRAY_COUNT(Domain)));
	WoTCacheHashUnsigned(Hasher, static_cast<uint64>(Revision));
	return FResult::Success(Hasher.Finish());
}

TWoTBootstrapResult<FWoTSha256> GetCacheContentLabel()
{
	return ComputeCacheContentLabel(WoTCacheContentRevision);
}

bool IsCurrentProductionCacheBuildIdentity(
	const FWoTSha256& ContractSha256,
	const FWoTSha256& RecordedContentLabel)
{
	const auto ExpectedContract = FWoTSha256::FromHex(
		WoTCacheExpectedProductionContractIdentity);
	const auto ExpectedLabel = GetCacheContentLabel();
	return !ExpectedContract.IsError() && !ExpectedLabel.IsError()
		&& ContractSha256 == ExpectedContract.GetValue()
		&& RecordedContentLabel == ExpectedLabel.GetValue();
}

FWoTSha256 ComputeRetailContractIdentity(const FWoTRetailContract& Contract)
{
	FWoTSha256Hasher Hasher;
	// V2 appends the parser-validated startup closure as a framed count,
	// ordered length-prefixed UTF-8 IDs, and the exact 32-byte closure digest.
	static const ANSICHAR Domain[] = "WOT-CACHE-CONTRACT-IDENTITY-V2";
	Hasher.Update(TConstArrayView<uint8>(reinterpret_cast<const uint8*>(Domain),
		UE_ARRAY_COUNT(Domain)));
	WoTCacheHashString(Hasher, Contract.Format);
	WoTCacheHashUnsigned(Hasher, Contract.ContractVersion);
	WoTCacheHashString(Hasher, Contract.ContractId);
	WoTCacheHashString(Hasher, Contract.KeyId);
	WoTCacheHashUnsigned(Hasher, Contract.ReleaseSequence);
	WoTCacheHashUnsigned(Hasher, static_cast<uint64>(Contract.AuditSet.Num()));
	for (const FWoTRetailAuditBinding& Audit : Contract.AuditSet)
	{
		WoTCacheHashString(Hasher, Audit.RuntimeConsumer);
		WoTCacheHashString(Hasher, Audit.AuditRule);
	}
	WoTCacheHashUnsigned(Hasher, static_cast<uint64>(Contract.Facts.Num()));
	for (const FWoTRequiredFact& Fact : Contract.Facts)
	{
		WoTCacheHashString(Hasher, Fact.FactId);
		WoTCacheHashUnsigned(Hasher, static_cast<uint8>(Fact.Domain));
		WoTCacheHashString(Hasher, Fact.SourcePath);
		WoTCacheHashString(Hasher, Fact.SourceExport);
		WoTCacheHashString(Hasher, Fact.Importer);
		WoTCacheHashString(Hasher, Fact.RuntimeConsumer);
		WoTCacheHashString(Hasher, Fact.AuditRule);
		WoTCacheHashUnsigned(Hasher, static_cast<uint64>(Fact.Outputs.Num()));
		for (const FWoTRequiredFactOutput& Output : Fact.Outputs)
		{
			WoTCacheHashString(Hasher, Output.LogicalId);
			WoTCacheHashString(Hasher, Output.Schema);
			WoTCacheHashUnsigned(Hasher, Output.MaxDecodedSize);
		}
	}
	WoTCacheHashUnsigned(Hasher,
		static_cast<uint64>(Contract.StartupCriticalLogicalIds.Num()));
	for (const FString& LogicalId : Contract.StartupCriticalLogicalIds)
	{
		WoTCacheHashString(Hasher, LogicalId);
	}
	WoTCacheHashSha(Hasher, Contract.StartupCriticalLogicalIdsSha256);
	return Hasher.Finish();
}

TWoTBootstrapResult<FWoTCacheBuildResult> WoT::Retail::Detail::BuildRetailCache(
	FWoTRetailPackage& Package, const FWoTRetailContract& Contract,
	IWoTObjectWriter& Writer,
	IWoTProgressSink& Progress)
{
	using FResult = TWoTBootstrapResult<FWoTCacheBuildResult>;
	TWoTBootstrapResult<TArray<uint8>> Descriptor =
		WoTCacheLoadProductionDescriptor();
	if (Descriptor.IsError()) return FResult::Error(MoveTemp(Descriptor).GetError());
	FWoTBootstrapStatus DescriptorStatus =
		WoTCacheValidateProductionDescriptor(Descriptor.GetValue());
	if (DescriptorStatus.IsError())
		return FResult::Error(MoveTemp(DescriptorStatus).GetError());
	FWoTBootstrapStatus Closure = ValidateFactClosure(Contract);
	if (Closure.IsError()) return FResult::Error(MoveTemp(Closure).GetError());
	TWoTBootstrapResult<TMap<FString, FWoTCacheDeclaredOutput>> Declared =
		WoTCacheDeclaredOutputs(Contract);
	if (Declared.IsError()) return FResult::Error(MoveTemp(Declared).GetError());
	// No retail source lookup is needed for global progress. Importers own their
	// byte bounds and decode every fact for each fresh build.
	const TMap<FString, uint64> SourceInventory =
		WoTCacheContractSourceInventory(Contract);

	TArray<FWoTCacheImporterBoundary> Boundaries;
	TArray<FWoTWorldDescriptor> WorldDescriptors;
	TArray<uint8> FrontEndDescriptorSidecar;
	FWoTSha256 ExpectedFrontEndDescriptorRoot;
	const FWoTSha256 ContractSha = ComputeRetailContractIdentity(Contract);
	TWoTBootstrapResult<FWoTSha256> ContentLabelResult = GetCacheContentLabel();
	if (ContentLabelResult.IsError())
		return FResult::Error(MoveTemp(ContentLabelResult).GetError());
	const FWoTSha256 ContentLabel = ContentLabelResult.GetValue();
	Boundaries.Add({TEXT("texture-sound"), {TEXT("texture-v2"), TEXT("sound-v1")},
		[&Package, &Contract](IWoTObjectWriter& ObjectWriter,
			IWoTProgressSink& ObjectProgress)
		{
			return ImportTextureSound(
				Package, Contract, ObjectWriter, ObjectProgress);
		}});
	Boundaries.Add({TEXT("mesh-animation"), {TEXT("mesh-v1"), TEXT("animation-v1")},
		[&Package, &Contract](IWoTObjectWriter& ObjectWriter,
			IWoTProgressSink& ObjectProgress)
		{
			return ImportMeshAnimation(
				Package, Contract, ObjectWriter, ObjectProgress);
		}});
	Boundaries.Add({TEXT("required-world-set"), {TEXT("bsp-v1"), TEXT("world-v1")},
		[&Package, &Contract, &WorldDescriptors](IWoTObjectWriter& ObjectWriter,
			IWoTProgressSink& ObjectProgress)
		{
			using FObjectsResult = TWoTBootstrapResult<TArray<FWoTCacheBlob>>;
			TWoTBootstrapResult<FWoTWorldSetResult> Worlds = ImportRequiredWorldSet(
				Package, Contract, ObjectWriter, ObjectProgress);
			if (Worlds.IsError())
				return FObjectsResult::Error(MoveTemp(Worlds).GetError());
			FWoTWorldSetResult WorldSet = MoveTemp(Worlds).GetValue();
			WorldDescriptors = MoveTemp(WorldSet.Worlds);
			TWoTBootstrapResult<FWoTWorldSetResult> Entry = ImportFrontEndWorld(
				Package, Contract, ObjectWriter, ObjectProgress);
			if (Entry.IsError())
				return FObjectsResult::Error(MoveTemp(Entry).GetError());
			WorldSet.Objects.Append(MoveTemp(Entry).GetValue().Objects);
			return FObjectsResult::Success(MoveTemp(WorldSet.Objects));
		}});
	Boundaries.Add({TEXT("front-end-world"), {TEXT("front-end-world-v1")},
		[&Package, &Contract, &FrontEndDescriptorSidecar,
			&ExpectedFrontEndDescriptorRoot, ContractSha, ContentLabel](
			IWoTObjectWriter& ObjectWriter, IWoTProgressSink& ObjectProgress)
		{
			using FObjectsResult = TWoTBootstrapResult<TArray<FWoTCacheBlob>>;
			TWoTBootstrapResult<FWoTFrontEndDescriptor> BuiltDescriptor =
				BuildFrontEndDescriptor(Package, Contract, ObjectProgress);
			if (BuiltDescriptor.IsError())
				return FObjectsResult::Error(MoveTemp(BuiltDescriptor).GetError());
			FWoTFrontEndDescriptor EntryDescriptor =
				MoveTemp(BuiltDescriptor).GetValue();
			TWoTBootstrapResult<TArray<uint8>> Sealed =
				WriteSealedFrontEndDescriptorSidecar(EntryDescriptor,
					Package.CachedInputSha256(), ContractSha, ContentLabel);
			if (Sealed.IsError())
				return FObjectsResult::Error(MoveTemp(Sealed).GetError());
			FrontEndDescriptorSidecar = MoveTemp(Sealed).GetValue();
			ExpectedFrontEndDescriptorRoot = EntryDescriptor.DescriptorRoot;
			const FWoTRequiredFact* Fact = Contract.Facts.FindByPredicate(
				[](const FWoTRequiredFact& Candidate)
				{
					return Candidate.Domain == EWoTFactDomain::FrontEndWorld;
				});
			if (Fact == nullptr || Fact->Importer != TEXT("front-end-world-v1")
				|| Fact->Outputs.Num() != 1)
			{
				return FObjectsResult::Error(WoTCacheError(
					EWoTBootstrapError::UnknownField,
					TEXT("the contract has no unique front-end descriptor output"),
					TEXT("front-end-world")));
			}
			const FWoTRequiredFactOutput& Output = Fact->Outputs[0];
			const FWoTSha256 SidecarSha = Sha256(FrontEndDescriptorSidecar);
			FWoTBootstrapStatus Written = ObjectWriter.WriteObject(
				Output.LogicalId, Output.Schema, FrontEndDescriptorSidecar,
				SidecarSha, Output.MaxDecodedSize);
			if (Written.IsError())
				return FObjectsResult::Error(MoveTemp(Written).GetError());
			FWoTCacheBlob Blob;
			Blob.LogicalId = Output.LogicalId;
			Blob.Schema = Output.Schema;
			Blob.Sha256 = SidecarSha;
			Blob.Size = static_cast<uint64>(FrontEndDescriptorSidecar.Num());
			Blob.Satisfies.AddDefaulted_GetRef().Value = Fact->FactId;
			TArray<FWoTCacheBlob> Objects;
			Objects.Add(MoveTemp(Blob));
			return FObjectsResult::Success(MoveTemp(Objects));
		}});
	Boundaries.Add({TEXT("font-procedural"), {TEXT("font-v2"), TEXT("procedural-v1")},
		[&Package, &Contract](IWoTObjectWriter& ObjectWriter,
			IWoTProgressSink& ObjectProgress)
		{
			return ImportFontProcedural(
				Package, Contract, ObjectWriter, ObjectProgress);
		}});
	Boundaries.Add({TEXT("registry-localization-defaults"),
		{TEXT("registry-v1"), TEXT("localization-v1"), TEXT("class-default-v2")},
		[&Package, &Contract](IWoTObjectWriter& ObjectWriter,
			IWoTProgressSink& ObjectProgress)
		{
			return ImportRegistryLocalizationDefaults(
				Package, Contract, ObjectWriter, ObjectProgress);
		}});
	FResult Built = WoTCacheBuildWithInputs(Package.CachedInputSha256(), Contract,
		Writer, Progress, Descriptor.GetValue(), SourceInventory,
        Boundaries, [&Package, &WorldDescriptors, &ExpectedFrontEndDescriptorRoot](
            FWoTCacheManifestV3& Manifest)
        {
            Manifest.WorldDescriptors = MoveTemp(WorldDescriptors);
            Manifest.FrontEndDescriptorRoot = ExpectedFrontEndDescriptorRoot;
            return Package.ReverifyHeldSnapshotAfterExtraction();
        });
	return Built;
}

#if WITH_AUTOMATION_TESTS
TWoTBootstrapResult<FWoTCacheBuildResult> BuildRetailCacheForTesting(
	const FWoTSha256& InstallerSha256, const FWoTRetailContract& Contract,
	IWoTObjectWriter& Writer,
	IWoTProgressSink& Progress, TConstArrayView<uint8> CanonicalProductionDescriptor,
	const TMap<FString, uint64>& BoundedSourceSizes,
	TArray<FWoTCacheImporterForTesting>& Importers)
{
	TArray<FWoTCacheImporterBoundary> Boundaries;
	for (FWoTCacheImporterForTesting& Importer : Importers)
	{
		FWoTCacheImporterBoundary& Boundary = Boundaries.AddDefaulted_GetRef();
		Boundary.Stage = MoveTemp(Importer.Stage);
		Boundary.Importers = MoveTemp(Importer.Importers);
		Boundary.Run = MoveTemp(Importer.Run);
	}
	return WoTCacheBuildWithInputs(InstallerSha256, Contract, Writer,
		Progress, CanonicalProductionDescriptor, BoundedSourceSizes, Boundaries,
		[](FWoTCacheManifestV3&) { return FWoTBootstrapStatus::Success(); });
}

FWoTBootstrapStatus ValidateCacheProductionDescriptorForTesting(
	TConstArrayView<uint8> CanonicalProductionDescriptor)
{
	return WoTCacheValidateProductionDescriptor(CanonicalProductionDescriptor);
}

void SetCacheProductionDescriptorPathForTesting(FString Path)
{
	GWoTCacheProductionDescriptorPathForTesting = MoveTemp(Path);
}

void SetCacheProductionDescriptorPinForTesting(FString HexOrEmpty)
{
	GWoTCacheProductionDescriptorPinForTesting = MoveTemp(HexOrEmpty);
}

TWoTBootstrapResult<TArray<uint8>> LoadCacheProductionDescriptorForTesting()
{
	using FResult = TWoTBootstrapResult<TArray<uint8>>;
	if (GWoTCacheProductionDescriptorPathForTesting.IsEmpty())
		return WoTCacheLoadProductionDescriptor();
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(
			Bytes, *GWoTCacheProductionDescriptorPathForTesting))
	{
		return FResult::Error(WoTCacheError(EWoTBootstrapError::Io,
			TEXT("the runner-supplied production descriptor cannot be read"),
			GWoTCacheProductionDescriptorPathForTesting));
	}
	return FResult::Success(MoveTemp(Bytes));
}

TWoTBootstrapResult<TArray<uint8>>
WriteCacheSyntheticParityArtifactForTesting(
	const FWoTCacheBuildResult& Build,
	const TMap<FString, FWoTSha256>& WorldEventRoots,
	const FWoTSha256& WorldSetRoot,
	TConstArrayView<FString> RejectedFieldPaths)
{
	using FResult = TWoTBootstrapResult<TArray<uint8>>;
	FWoTCanonicalObjectBuilder ObjectsBuilder;
	TArray<const FWoTCacheBlob*> Blobs;
	for (const FWoTCacheBlob& Blob : Build.Manifest.Blobs) Blobs.Add(&Blob);
	Blobs.Sort([](const FWoTCacheBlob& Left, const FWoTCacheBlob& Right)
	{
		return Left.LogicalId < Right.LogicalId;
	});
	for (const FWoTCacheBlob* Blob : Blobs)
	{
		if (Blob->Size > static_cast<uint64>(TNumericLimits<int64>::Max()))
		{
			return FResult::Error(WoTCacheError(EWoTBootstrapError::SizeLimit,
				TEXT("synthetic parity object size is not JSON-representable"),
				Blob->LogicalId));
		}
		FWoTCanonicalObjectBuilder ObjectBuilder;
		ObjectBuilder.Add(TEXT("sha256"),
			FWoTCanonicalJsonValue::MakeString(Blob->Sha256.ToHex()));
		ObjectBuilder.Add(TEXT("size"), FWoTCanonicalJsonValue::MakeInteger(
			static_cast<int64>(Blob->Size)));
		TWoTBootstrapResult<FWoTCanonicalJsonValue> Object =
			MoveTemp(ObjectBuilder).Build(TEXT("synthetic parity object"));
		if (Object.IsError()) return FResult::Error(MoveTemp(Object).GetError());
		ObjectsBuilder.Add(*Blob->LogicalId, MoveTemp(Object).GetValue());
	}
	TWoTBootstrapResult<FWoTCanonicalJsonValue> Objects =
		MoveTemp(ObjectsBuilder).Build(TEXT("synthetic parity objects"));
	if (Objects.IsError()) return FResult::Error(MoveTemp(Objects).GetError());

	if (Build.Progress.GetCompletedUnits()
			> static_cast<uint64>(TNumericLimits<int64>::Max())
		|| Build.Progress.GetTotalUnits()
			> static_cast<uint64>(TNumericLimits<int64>::Max()))
	{
		return FResult::Error(WoTCacheError(EWoTBootstrapError::SizeLimit,
			TEXT("synthetic parity progress is not JSON-representable")));
	}
	FWoTCanonicalObjectBuilder ProgressBuilder;
	ProgressBuilder.Add(TEXT("completed"), FWoTCanonicalJsonValue::MakeInteger(
		static_cast<int64>(Build.Progress.GetCompletedUnits())));
	ProgressBuilder.Add(TEXT("total"), FWoTCanonicalJsonValue::MakeInteger(
		static_cast<int64>(Build.Progress.GetTotalUnits())));
	TWoTBootstrapResult<FWoTCanonicalJsonValue> Progress =
		MoveTemp(ProgressBuilder).Build(TEXT("synthetic parity progress"));
	if (Progress.IsError()) return FResult::Error(MoveTemp(Progress).GetError());

	TArray<FString> OrderedPaths;
	OrderedPaths.Reserve(RejectedFieldPaths.Num());
	for (const FString& Path : RejectedFieldPaths) OrderedPaths.Add(Path);
	OrderedPaths.Sort();
	FWoTCanonicalJsonValue Rejections = FWoTCanonicalJsonValue::MakeArray();
	for (const FString& Path : OrderedPaths)
		Rejections.AddElement(FWoTCanonicalJsonValue::MakeString(Path));
	TArray<FString> WorldIdentities;
	WorldEventRoots.GetKeys(WorldIdentities);
	WorldIdentities.Sort();
	FWoTCanonicalObjectBuilder EventRootsBuilder;
	for (const FString& Identity : WorldIdentities)
	{
		EventRootsBuilder.Add(*Identity, FWoTCanonicalJsonValue::MakeString(
			WorldEventRoots.FindChecked(Identity).ToHex()));
	}
	TWoTBootstrapResult<FWoTCanonicalJsonValue> EventRoots =
		MoveTemp(EventRootsBuilder).Build(TEXT("synthetic world event roots"));
	if (EventRoots.IsError()) return FResult::Error(MoveTemp(EventRoots).GetError());

	FWoTCanonicalObjectBuilder RootBuilder;
	RootBuilder.Add(TEXT("format"), FWoTCanonicalJsonValue::MakeString(
		TEXT("wot-cache-v3-synthetic-parity-v1")));
	RootBuilder.Add(TEXT("manifest_sha256"), FWoTCanonicalJsonValue::MakeString(
		Build.ManifestSha256.ToHex()));
	RootBuilder.Add(TEXT("objects"), MoveTemp(Objects).GetValue());
	RootBuilder.Add(TEXT("progress"), MoveTemp(Progress).GetValue());
	RootBuilder.Add(TEXT("rejected_field_paths"), MoveTemp(Rejections));
	RootBuilder.Add(TEXT("world_event_roots"), MoveTemp(EventRoots).GetValue());
	RootBuilder.Add(TEXT("world_set_root"),
		FWoTCanonicalJsonValue::MakeString(WorldSetRoot.ToHex()));
	TWoTBootstrapResult<FWoTCanonicalJsonValue> Root =
		MoveTemp(RootBuilder).Build(TEXT("synthetic parity artifact"));
	if (Root.IsError()) return FResult::Error(MoveTemp(Root).GetError());
	return WriteCanonicalJson(Root.GetValue());
}
#endif
