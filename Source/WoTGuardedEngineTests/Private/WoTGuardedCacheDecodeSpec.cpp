// Release gate B: every blob of one content cache must decode through the
// readers the guarded (packaged) game runs for it. Editor automation compiles
// those readers with WOT_GUARDED_BOOTSTRAP=0 and cannot stand in for this.
//
// The spec accepts the cache exactly as the game does (production installer,
// contract and content label), activates it the way UWoTPayloadSubsystem does
// (runtime registry bound to the lease, activation published), and then runs
// each manifest entry through its guarded runtime decoder. Every decoder is
// production code; nothing here parses cache bytes itself.

#include "WoTGuardedEngineTests.h"

#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeExit.h"
#include "WoTBootstrapCore/ArtifactTypes.h"
#include "WoTBootstrapCore/CanonicalJson.h"
#include "WoTRetailReader/CacheBuilder.h"
#include "WoTRetailReader/CacheManifest.h"
#include "WoTRetailReader/FrontEndWorld.h"
#include "WoTRetailReader/ProductionContract.h"
#include "../../WoT/FrontEnd/WoTFrontEndSceneMaterializer.h"
#include "../../WoT/Mesh/WoTRuntimeVertexMesh.h"
#include "../../WoT/Payload/WoTCacheLease.h"
#include "../../WoT/Payload/WoTCacheStore.h"
#include "../../WoT/Payload/WoTPayloadActivation.h"
#include "../../WoT/Payload/WoTRuntimeFontResource.h"
#include "../../WoT/Payload/WoTRuntimeLocalizationDefaults.h"
#include "../../WoT/Payload/WoTRuntimePackageRegistry.h"
#include "../../WoT/Payload/WoTRuntimeProceduralResource.h"
#include "../../WoT/Payload/WoTRuntimeRegistry.h"
#include "../../WoT/Payload/WoTRuntimeWorldResource.h"
#include "../../WoT/World/WoTRuntimeClassDefaults.h"
#include "../../WoT/World/WoTWorldMaterializer.h"
#include "../../WoT/WoTAnimData.h"
#include "../../WoT/WoTBSPPayload.h"
#include "../../WoT/WoTFractalData.h"
#include "../../WoT/WoTPayloadSubsystem.h"

#if WITH_AUTOMATION_TESTS

/**
 * The activation steps UWoTPayloadSubsystem::ActivateVerifiedCache performs
 * for an accepted lease, without a game instance, plus a read of the lease's
 * complete manifest table. Named as a friend of the three owning classes.
 */
class FWoTGuardedCacheDecodeAccess final
{
public:
	static FWoTBootstrapStatus BindRegistry(const FWoTCacheGenerationLease& Lease)
	{
		return FWoTRuntimeRegistry::Get().BindVerifiedGeneration(Lease);
	}

	static FWoTBootstrapStatus PublishActivation(
		const FWoTCacheGenerationLease& Lease)
	{
		return FWoTPayloadActivationPublication::
			PublishWoTPayloadBootstrapActivation(Lease);
	}

	static void RevokeActivation(const FWoTCacheGenerationLease& Lease)
	{
		FWoTPayloadActivationPublication::
			RevokeWoTPayloadBootstrapActivation(Lease);
	}

	static void ResetRegistry()
	{
		FWoTRuntimeRegistry::Get().Reset();
	}

	static TArray<FWoTCacheLeaseManifestEntry> ManifestEntries(
		const FWoTCacheGenerationLease& Lease)
	{
		TArray<FWoTCacheLeaseManifestEntry> Entries;
		for (const TPair<FString, FWoTCacheLeaseManifestEntry>& Pair
			: Lease.GetManifestTable())
		{
			Entries.Add(Pair.Value);
		}
		Entries.Sort([](const FWoTCacheLeaseManifestEntry& A,
			const FWoTCacheLeaseManifestEntry& B)
		{
			return A.LogicalId.Compare(B.LogicalId, ESearchCase::CaseSensitive) < 0;
		});
		return Entries;
	}
};

namespace
{
const TCHAR* const WoTDecodeReceiptFormat = TEXT("wot-cache-decode-receipt-v1");
const TCHAR* const WoTFrontEndWorldLogicalId =
	TEXT("cache/front_end_world/WheelOfTime");

const TCHAR* const SchemaTexture = TEXT("wot-texture-v3");
const TCHAR* const SchemaSound = TEXT("wot-sound-v3");
const TCHAR* const SchemaMesh = TEXT("wot-mesh-v3");
const TCHAR* const SchemaAnimation = TEXT("wot-animation-v3");
const TCHAR* const SchemaBSP = TEXT("wot-bsp-v3");
const TCHAR* const SchemaWorld = TEXT("wot-world-v3");
const TCHAR* const SchemaFont = TEXT("wot-font-v3");
const TCHAR* const SchemaProcedural = TEXT("wot-procedural-v3");
const TCHAR* const SchemaRegistry = TEXT("wot-registry-v3");
const TCHAR* const SchemaLocalization = TEXT("wot-localization-v3");
const TCHAR* const SchemaClassDefault = TEXT("wot-class-default-v3");
const TCHAR* const SchemaFrontEndWorld = TEXT("wot-front-end-world-v3");

struct FWoTDecodeTally
{
	int64 Manifest = 0;
	int64 Decoded = 0;
};

struct FWoTDecodeFailure
{
	FString LogicalId;
	FString Schema;
	FString Error;
};

/** Logical ids and schemas are exact strings; FString == folds case. */
bool Is(const FString& Value, const TCHAR* Expected)
{
	return Value.Equals(Expected, ESearchCase::CaseSensitive);
}

/** Decode outcome of one entry: unset when the guarded reader accepted it. */
using FWoTDecodeOutcome = TOptional<FString>;

FWoTDecodeOutcome StatusOutcome(const FWoTBootstrapStatus& Status)
{
	return Status.IsError() ? FWoTDecodeOutcome(Status.GetError().ToString())
		: FWoTDecodeOutcome();
}

FWoTDecodeOutcome RejectOutcome(bool bDecoded, const FString& Reason)
{
	if (bDecoded)
	{
		return FWoTDecodeOutcome();
	}
	return FWoTDecodeOutcome(Reason.IsEmpty()
		? FString(TEXT("the guarded reader rejected the object")) : Reason);
}

/** "cache/<kind>/<Package>__<Name>" -> Package and Name (the mesh naming rule). */
bool SplitPackageName(const FString& LogicalId, const TCHAR* Prefix,
	FString& OutPackage, FString& OutName)
{
	if (!LogicalId.StartsWith(Prefix, ESearchCase::CaseSensitive))
	{
		return false;
	}
	const FString Tail = LogicalId.RightChop(FCString::Strlen(Prefix));
	const int32 Separator = Tail.Find(TEXT("__"), ESearchCase::CaseSensitive);
	if (Separator <= 0 || Separator + 2 >= Tail.Len())
	{
		return false;
	}
	OutPackage = Tail.Left(Separator);
	OutName = Tail.Mid(Separator + 2);
	return true;
}

FString RecordedManifestIdentity(const FString& CacheRoot)
{
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *FPaths::Combine(
		CacheRoot, TEXT("current"), TEXT("manifest.json"))))
	{
		return TEXT("current/manifest.json is unreadable");
	}
	FWoTCanonicalJsonLimits Limits;
	Limits.MaxBytes = 16u * 1024u * 1024u;
	Limits.MaxMembers = 100000;
	TWoTBootstrapResult<FWoTCanonicalJsonValue> Parsed =
		ParseCanonicalJson(Bytes, Limits);
	if (Parsed.IsError())
	{
		return FString::Printf(TEXT("current/manifest.json does not parse: %s"),
			*Parsed.GetError().ToString());
	}
	FString Installer;
	FString Contract;
	FString Label;
	ReadStringField(Parsed.GetValue(), TEXT("installer_sha256"),
		TEXT("cache manifest"), Installer);
	ReadStringField(Parsed.GetValue(), TEXT("contract_sha256"),
		TEXT("cache manifest"), Contract);
	ReadStringField(Parsed.GetValue(), TEXT("production_descriptor_sha256"),
		TEXT("cache manifest"), Label);
	return FString::Printf(TEXT("the manifest records installer %s, contract %s, label %s"),
		*Installer, *Contract, *Label);
}

struct FWoTDecodeReceipt
{
	FString Platform;
	FString Configuration;
	FString ManifestSha256;
	FString InstallerSha256;
	FString ContractSha256;
	FString Label;
	TMap<FString, FWoTDecodeTally> PerSchema;
	TArray<FWoTDecodeFailure> Failures;
	int64 Seconds = 0;
};

bool AddReceiptMember(FWoTCanonicalJsonValue& Object, const TCHAR* Key,
	FWoTCanonicalJsonValue&& Value)
{
	return Object.AddMember(Key, MoveTemp(Value));
}

TWoTBootstrapResult<TArray<uint8>> SerializeReceipt(const FWoTDecodeReceipt& Receipt)
{
	FWoTCanonicalJsonValue Failures = FWoTCanonicalJsonValue::MakeArray();
	for (const FWoTDecodeFailure& Failure : Receipt.Failures)
	{
		FWoTCanonicalJsonValue Row = FWoTCanonicalJsonValue::MakeObject();
		AddReceiptMember(Row, TEXT("error"), FWoTCanonicalJsonValue::MakeString(Failure.Error));
		AddReceiptMember(Row, TEXT("logical_id"),
			FWoTCanonicalJsonValue::MakeString(Failure.LogicalId));
		AddReceiptMember(Row, TEXT("schema"), FWoTCanonicalJsonValue::MakeString(Failure.Schema));
		Failures.AddElement(MoveTemp(Row));
	}
	TArray<FString> Schemas;
	Receipt.PerSchema.GetKeys(Schemas);
	Schemas.Sort([](const FString& A, const FString& B)
	{
		return A.Compare(B, ESearchCase::CaseSensitive) < 0;
	});
	FWoTCanonicalJsonValue PerSchema = FWoTCanonicalJsonValue::MakeObject();
	bool bSchemasOrdered = true;
	for (const FString& Schema : Schemas)
	{
		const FWoTDecodeTally& Tally = Receipt.PerSchema.FindChecked(Schema);
		FWoTCanonicalJsonValue Row = FWoTCanonicalJsonValue::MakeObject();
		AddReceiptMember(Row, TEXT("decoded"), FWoTCanonicalJsonValue::MakeInteger(Tally.Decoded));
		AddReceiptMember(Row, TEXT("manifest"), FWoTCanonicalJsonValue::MakeInteger(Tally.Manifest));
		bSchemasOrdered &= PerSchema.AddMember(Schema, MoveTemp(Row));
	}
	FWoTCanonicalJsonValue Root = FWoTCanonicalJsonValue::MakeObject();
	const bool bOrdered = bSchemasOrdered
		&& AddReceiptMember(Root, TEXT("configuration"),
			FWoTCanonicalJsonValue::MakeString(Receipt.Configuration))
		&& AddReceiptMember(Root, TEXT("contract_sha256"),
			FWoTCanonicalJsonValue::MakeString(Receipt.ContractSha256))
		&& AddReceiptMember(Root, TEXT("failures"), MoveTemp(Failures))
		&& AddReceiptMember(Root, TEXT("format"),
			FWoTCanonicalJsonValue::MakeString(WoTDecodeReceiptFormat))
		&& AddReceiptMember(Root, TEXT("guard"),
			FWoTCanonicalJsonValue::MakeInteger(WOT_GUARDED_BOOTSTRAP))
		&& AddReceiptMember(Root, TEXT("installer_sha256"),
			FWoTCanonicalJsonValue::MakeString(Receipt.InstallerSha256))
		&& AddReceiptMember(Root, TEXT("label"),
			FWoTCanonicalJsonValue::MakeString(Receipt.Label))
		&& AddReceiptMember(Root, TEXT("manifest_sha256"),
			FWoTCanonicalJsonValue::MakeString(Receipt.ManifestSha256))
		&& AddReceiptMember(Root, TEXT("per_schema"), MoveTemp(PerSchema))
		&& AddReceiptMember(Root, TEXT("platform"),
			FWoTCanonicalJsonValue::MakeString(Receipt.Platform))
		&& AddReceiptMember(Root, TEXT("seconds"),
			FWoTCanonicalJsonValue::MakeInteger(Receipt.Seconds));
	if (!bOrdered)
	{
		return TWoTBootstrapResult<TArray<uint8>>::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::NonCanonicalJson,
			TEXT("the decode receipt members are out of canonical order")));
	}
	return WriteCanonicalJson(Root);
}

const TCHAR* ReceiptPlatform()
{
#if PLATFORM_WINDOWS
	return TEXT("Win64");
#elif PLATFORM_MAC
	return TEXT("Mac");
#else
	return FPlatformProperties::IniPlatformName();
#endif
}
}  // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTGuardedCacheDecodeEveryBlobSpec,
	"WoT.GuardedEngine.CacheDecode.EveryBlob", WoTGuardedEngineSpecFlags)

bool FWoTGuardedCacheDecodeEveryBlobSpec::RunTest(const FString&)
{
#if !WOT_GUARDED_BOOTSTRAP
	AddError(TEXT("cache decode proof requires the guarded build (WOT_GUARDED_BOOTSTRAP=1)"));
	return false;
#else
	const FWoTGuardedCacheDecodeInputs& Inputs = WoTGuardedCacheDecodeInputs();
	if (Inputs.CacheRoot.IsEmpty())
	{
		AddWarning(TEXT("no --cache-root was given; no cache was decoded"));
		return true;
	}
	if (Inputs.DecodeReceipt.IsEmpty())
	{
		AddError(TEXT("--cache-root requires --decode-receipt"));
		return false;
	}
	const double Started = FPlatformTime::Seconds();

	FWoTDecodeReceipt Receipt;
	Receipt.Platform = ReceiptPlatform();
	Receipt.Configuration = LexToString(FApp::GetBuildConfiguration());
	auto Fail = [&Receipt](const FString& LogicalId, const FString& Schema,
		const FString& Error)
	{
		Receipt.Failures.Add({LogicalId, Schema, Error});
	};

	// 1. Acceptance with the production identity the guarded game expects.
	FWoTCacheGenerationBuildIdentity Expected;
	{
		TWoTBootstrapResult<FWoTSha256> Installer =
			FWoTSha256::FromHex(WoT::Bootstrap::InstallerSha256);
		TWoTBootstrapResult<FWoTRetailContract> Contract =
			WoTLoadProductionRetailContract();
		TWoTBootstrapResult<FWoTSha256> Label = GetCacheContentLabel();
		if (Installer.IsError() || Contract.IsError() || Label.IsError())
		{
			Fail(FString(), FString(),
				TEXT("acceptance: the compiled production identity is unavailable"));
		}
		else
		{
			Expected.bHasBuildIdentity = true;
			Expected.InstallerSha256 = Installer.GetValue();
			Expected.ContractSha256 =
				ComputeRetailContractIdentity(Contract.GetValue());
			Expected.ProductionDescriptorSha256 = Label.GetValue();
			Receipt.InstallerSha256 = Expected.InstallerSha256.ToHex();
			Receipt.ContractSha256 = Expected.ContractSha256.ToHex();
			Receipt.Label = Expected.ProductionDescriptorSha256.ToHex();
		}
	}

	TUniquePtr<FWoTCacheGenerationLease> Lease;
	if (Receipt.Failures.IsEmpty())
	{
		// Reading needs no profile session: only cache mutation (BeginBuild,
		// Publish, DeleteCache) requires the exclusive session.
		TWoTBootstrapResult<FWoTCacheStore> Store =
			FWoTCacheStore::Open(Inputs.CacheRoot);
		if (Store.IsError())
		{
			Fail(FString(), FString(), FString::Printf(
				TEXT("acceptance: the cache store does not open: %s"),
				*Store.GetError().ToString()));
		}
		else
		{
			TWoTBootstrapResult<TOptional<FWoTCacheGenerationLease>> Opened =
				Store.GetValue().TryOpenCurrent(Expected, nullptr);
			if (Opened.IsError())
			{
				Fail(FString(), FString(), FString::Printf(
					TEXT("acceptance: TryOpenCurrent failed: %s"),
					*Opened.GetError().ToString()));
			}
			else if (!Opened.GetValue().IsSet())
			{
				Fail(FString(), FString(), FString::Printf(
					TEXT("acceptance: the cache is not current for this build "
						"(identity, coverage, critical closure or sidecar); %s"),
					*RecordedManifestIdentity(Inputs.CacheRoot)));
			}
			else
			{
				Lease = MakeUnique<FWoTCacheGenerationLease>(
					MoveTemp(Opened.GetValue().GetValue()));
				Receipt.ManifestSha256 = Lease->GetManifestSha256().ToHex();
			}
		}
	}

	// 2. Activation, as UWoTPayloadSubsystem::ActivateVerifiedCache binds and
	// publishes the lease; torn down in its Deinitialize order.
	bool bRegistryBound = false;
	bool bPublished = false;
	ON_SCOPE_EXIT
	{
		WoTAnimData::ResetRuntimeCache();
		if (bPublished)
		{
			FWoTGuardedCacheDecodeAccess::RevokeActivation(*Lease);
		}
		if (bRegistryBound)
		{
			FWoTGuardedCacheDecodeAccess::ResetRegistry();
		}
		Lease.Reset();
	};
	if (Lease.IsValid())
	{
		FWoTBootstrapStatus Bound = FWoTGuardedCacheDecodeAccess::BindRegistry(*Lease);
		bRegistryBound = !Bound.IsError();
		FWoTBootstrapStatus Published = bRegistryBound
			? FWoTGuardedCacheDecodeAccess::PublishActivation(*Lease)
			: FWoTBootstrapStatus::Success();
		bPublished = bRegistryBound && !Published.IsError();
		if (Bound.IsError() || Published.IsError() || !IsWoTPayloadBootstrapActive())
		{
			Fail(FString(), FString(), FString::Printf(
				TEXT("activation failed: %s"),
				Bound.IsError() ? *Bound.GetError().ToString()
				: Published.IsError() ? *Published.GetError().ToString()
				: TEXT("the activation publication is not observable")));
		}
	}

	// 3. Every manifest entry through the reader the guarded game runs for it.
	const bool bActive = Receipt.Failures.IsEmpty() && Lease.IsValid();
	const TArray<FWoTCacheLeaseManifestEntry> Entries = Lease.IsValid()
		? FWoTGuardedCacheDecodeAccess::ManifestEntries(*Lease)
		: TArray<FWoTCacheLeaseManifestEntry>();
	if (bActive)
	{
		// World objects and BSP parts are decoded against the descriptors that
		// authorize them: the materializer's world descriptors and the
		// front-end descriptor the critical closure attested.
		TMap<FString, const FWoTWorldDescriptor*> WorldsById;
		TMap<FString, const FWoTBSPPartDescriptor*> PartsById;
		for (const FWoTWorldDescriptor& World : Lease->GetWorldDescriptors())
		{
			WorldsById.Add(World.WorldLogicalId, &World);
			for (const FWoTBSPPartDescriptor& Part : World.BSPParts)
			{
				PartsById.Add(Part.LogicalId, &Part);
			}
		}
		const FWoTFrontEndDescriptor* FrontEnd =
			Lease->HasFrontEndDescriptorAttestation()
				? &Lease->GetFrontEndDescriptor() : nullptr;

		// Class defaults load as one generation (FWoTRuntimeClassDefaults is what
		// activation and the world materializer consume); a failure there fails
		// every class-default object.
		FString ClassDefaultsError;
		bool bClassDefaultsLoaded = false;
		{
			FWoTRuntimeClassDefaults ClassDefaults;
			bClassDefaultsLoaded = ClassDefaults.LoadActiveGeneration(ClassDefaultsError);
		}

		for (const FWoTCacheLeaseManifestEntry& Entry : Entries)
		{
			const FString& Id = Entry.LogicalId;
			const FString& Schema = Entry.Schema;
			FWoTDecodeOutcome Outcome;
			if (Is(Schema, SchemaTexture))
			{
				FString Reason;
				const bool bDecoded =
					WoTDecodeActivatedTexturePayload(Id, Reason).IsValid();
				Outcome = RejectOutcome(bDecoded, Reason);
			}
			else if (Is(Schema, SchemaSound))
			{
				FString Reason;
				const bool bDecoded =
					WoTDecodeActivatedSoundPayload(Id, Reason).IsValid();
				Outcome = RejectOutcome(bDecoded, Reason);
			}
			else if (Is(Schema, SchemaMesh))
			{
				// The vertex-animation component's resolution by mesh name and
				// package, which loads the mesh with its animation.
				FString Package;
				FString Name;
				if (!SplitPackageName(Id, TEXT("cache/mesh/"), Package, Name))
				{
					Outcome = FString(TEXT("the mesh logical id has no Package__Name form"));
				}
				else
				{
					const FString AnimationId = TEXT("cache/animation/")
						+ Id.RightChop(FCString::Strlen(TEXT("cache/mesh/")));
					const FWoTRuntimeVertexMeshDataPtr Mesh =
						FWoTRuntimeVertexMesh::ResolveAndLoad(Name, FName(*Package));
					if (!Mesh.IsValid())
					{
						Outcome = FString(TEXT(
							"FWoTRuntimeVertexMesh::ResolveAndLoad rejected the mesh or its animation"));
					}
					else if (!Is(Mesh->GetMeshLogicalId(), *Id)
						|| !Is(Mesh->GetAnimationLogicalId(), *AnimationId))
					{
						Outcome = FString::Printf(
							TEXT("the mesh resolved to %s with %s"),
							*Mesh->GetMeshLogicalId(), *Mesh->GetAnimationLogicalId());
					}
				}
			}
			else if (Is(Schema, SchemaAnimation))
			{
				FString Package;
				FString Name;
				if (!SplitPackageName(Id, TEXT("cache/animation/"), Package, Name))
				{
					Outcome = FString(TEXT("the animation logical id has no Package__Name form"));
				}
				else if (WoTAnimData::FindSequences(FName(*Name), Id) == nullptr)
				{
					Outcome = FString(TEXT("WoTAnimData::FindSequences rejected the animation"));
				}
			}
			else if (Is(Schema, SchemaWorld))
			{
				FWoTRuntimeWorldResource World;
				const FWoTWorldDescriptor* const* Described = WorldsById.Find(Id);
				if (Described != nullptr && Is((*Described)->WorldLogicalId, *Id))
				{
					Outcome = StatusOutcome(
						WoTWorldLoadDescribedResource(**Described, World));
				}
				else if (FrontEnd != nullptr && Is(FrontEnd->WorldLogicalId, *Id))
				{
					Outcome = StatusOutcome(WoTFrontEndSceneLoadWorld(*FrontEnd, World));
				}
				else
				{
					Outcome = FString(TEXT("no world or front-end descriptor authorizes this world object"));
				}
			}
			else if (Is(Schema, SchemaBSP))
			{
				FWoTBSPPayloadData BSP;
				const FWoTBSPPartDescriptor* const* Part = PartsById.Find(Id);
				if (Part != nullptr && Is((*Part)->LogicalId, *Id))
				{
					Outcome = StatusOutcome(WoTWorldDecodeBSPPart(**Part, BSP));
				}
				else if (FrontEnd != nullptr && Is(FrontEnd->BspLogicalId, *Id))
				{
					Outcome = StatusOutcome(WoTFrontEndSceneDecodeBSP(
						FWoTNeutralResourceId{Id, EWoTNeutralResourceKind::BSP}, BSP));
				}
				else
				{
					Outcome = FString(TEXT("no world or front-end descriptor authorizes this BSP object"));
				}
			}
			else if (Is(Schema, SchemaFont))
			{
				FWoTRuntimeFontResource Font;
				Outcome = StatusOutcome(Font.LoadFirstUse(
					FWoTNeutralResourceId{Id, EWoTNeutralResourceKind::Font}));
			}
			else if (Is(Schema, SchemaProcedural))
			{
				// UWoTFractalSubsystem's guarded catalog: strict parse, then the
				// runtime record; either failure keeps the whole catalog dormant.
				FWoTRuntimeProceduralResource Procedural;
				Outcome = StatusOutcome(Procedural.LoadFirstUse(
					FWoTNeutralResourceId{Id, EWoTNeutralResourceKind::Procedural}));
				FWoTFractalTextureRecord Record;
				if (!Outcome.IsSet() && !Procedural.BuildRecord(Record))
				{
					Outcome = FString(TEXT("the procedural object has no runtime record"));
				}
			}
			else if (Is(Schema, SchemaRegistry))
			{
				FWoTRuntimePackageRegistry Registry;
				Outcome = StatusOutcome(Registry.LoadFirstUse(
					FWoTNeutralResourceId{Id, EWoTNeutralResourceKind::Registry}));
			}
			else if (Is(Schema, SchemaLocalization))
			{
				FWoTRuntimeLocalizationDefaults Localization;
				Outcome = StatusOutcome(Localization.LoadLocalizationFirstUse(
					FWoTNeutralResourceId{Id, EWoTNeutralResourceKind::Localization}));
			}
			else if (Is(Schema, SchemaClassDefault))
			{
				FWoTRuntimeLocalizationDefaults Defaults;
				Outcome = StatusOutcome(Defaults.LoadClassDefaultsFirstUse(
					FWoTNeutralResourceId{Id, EWoTNeutralResourceKind::ClassDefaults}));
				if (!Outcome.IsSet() && !bClassDefaultsLoaded)
				{
					Outcome = FString::Printf(
						TEXT("FWoTRuntimeClassDefaults::LoadActiveGeneration: %s"),
						*ClassDefaultsError);
				}
			}
			else if (Is(Schema, SchemaFrontEndWorld))
			{
				// The critical closure parsed the sealed front-end sidecar
				// (ParseSealedFrontEndDescriptorSidecar) during acceptance.
				if (FrontEnd == nullptr || !Is(Id, WoTFrontEndWorldLogicalId))
				{
					Outcome = FString(TEXT("acceptance did not attest this front-end descriptor"));
				}
			}
			else
			{
				Outcome = FString(TEXT("unknown schema: no guarded reader decodes it"));
			}

			FWoTDecodeTally& Tally = Receipt.PerSchema.FindOrAdd(Schema);
			++Tally.Manifest;
			if (Outcome.IsSet())
			{
				Fail(Id, Schema, Outcome.GetValue());
			}
			else
			{
				++Tally.Decoded;
			}
		}
	}
	else
	{
		for (const FWoTCacheLeaseManifestEntry& Entry : Entries)
		{
			++Receipt.PerSchema.FindOrAdd(Entry.Schema).Manifest;
		}
	}

	Receipt.Seconds = static_cast<int64>(
		FMath::CeilToDouble(FPlatformTime::Seconds() - Started));
	for (const TPair<FString, FWoTDecodeTally>& Pair : Receipt.PerSchema)
	{
		UE_LOG(LogTemp, Display, TEXT("cache decode %s: %lld of %lld"),
			*Pair.Key, Pair.Value.Decoded, Pair.Value.Manifest);
		if (Pair.Value.Decoded != Pair.Value.Manifest)
		{
			AddError(FString::Printf(TEXT("%s: %lld of %lld manifest entries decoded"),
				*Pair.Key, Pair.Value.Decoded, Pair.Value.Manifest));
		}
	}
	constexpr int32 ReportedFailures = 20;
	for (int32 Index = 0;
		Index < FMath::Min(Receipt.Failures.Num(), ReportedFailures); ++Index)
	{
		const FWoTDecodeFailure& Failure = Receipt.Failures[Index];
		AddError(FString::Printf(TEXT("%s (%s): %s"), *Failure.LogicalId,
			*Failure.Schema, *Failure.Error));
	}
	if (Receipt.Failures.Num() > ReportedFailures)
	{
		AddError(FString::Printf(TEXT("%d more failures are in the receipt"),
			Receipt.Failures.Num() - ReportedFailures));
	}

	// 4. The receipt, written whatever the outcome.
	TWoTBootstrapResult<TArray<uint8>> Bytes = SerializeReceipt(Receipt);
	if (Bytes.IsError())
	{
		AddError(Bytes.GetError().ToString());
	}
	else if (!FFileHelper::SaveArrayToFile(Bytes.GetValue(), *Inputs.DecodeReceipt))
	{
		AddError(FString::Printf(TEXT("could not write the decode receipt %s"),
			*Inputs.DecodeReceipt));
	}
	UE_LOG(LogTemp, Display, TEXT("cache decode: %d entries, %d failures, %lld s"),
		Entries.Num(), Receipt.Failures.Num(), Receipt.Seconds);
	return !HasAnyErrors();
#endif
}

#if !WITH_AUTOMATION_WORKER
// See WoTGuardedEngineTestsMain.cpp: without the automation worker the macro
// declares no registering instance, so supply one.
namespace
{
FWoTGuardedCacheDecodeEveryBlobSpec WoTGuardedCacheDecodeEveryBlobSpecInstance(
	TEXT("FWoTGuardedCacheDecodeEveryBlobSpec"));
}
#endif

#endif  // WITH_AUTOMATION_TESTS
