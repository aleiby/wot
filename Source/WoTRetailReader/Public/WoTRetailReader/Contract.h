// The authenticated cache-v3 contract and the bounded importer interfaces.
#pragma once

#include "CoreMinimal.h"
#include "WoTBootstrapCore/ArtifactTypes.h"
#include "WoTBootstrapCore/Error.h"

enum class EWoTFactDomain : uint8
{
	Texture,
	Sound,
	/** RESERVED: no importer produces music today; it returns with replacement content. */
	Music,
	Mesh,
	Animation,
	BSP,
	World,
	Font,
	Procedural,
	Registry,
	Localization,
	ClassDefault,
	FrontEndWorld,
};

struct FWoTRequiredFactId
{
	FString Value;

	bool operator==(const FWoTRequiredFactId& Other) const { return Value == Other.Value; }
	friend uint32 GetTypeHash(const FWoTRequiredFactId& Id) { return GetTypeHash(Id.Value); }
};

struct FWoTRequiredFactOutput
{
	FString LogicalId;
	FString Schema;
	uint64 MaxDecodedSize = 0;
};

struct FWoTRequiredFact
{
	FString FactId;
	EWoTFactDomain Domain = EWoTFactDomain::Texture;
	FString SourcePath;
	FString Importer;
	FString RuntimeConsumer;
	FString AuditRule;

	/** Full export path inside a UE1 package, or the exact file name for a loose source. */
	FString SourceExport;
	TArray<FWoTRequiredFactOutput> Outputs;
};

struct FWoTRetailAuditBinding
{
	FString RuntimeConsumer;
	FString AuditRule;
};

struct FWoTRetailContract
{
	FString Format;
	uint32 ContractVersion = 0;
	FString ContractId;
	FString KeyId;
	uint64 ReleaseSequence = 0;
	TArray<FString> StartupCriticalLogicalIds;
	FWoTSha256 StartupCriticalLogicalIdsSha256;
	TArray<FWoTRetailAuditBinding> AuditSet;
	TArray<FWoTRequiredFact> Facts;
};

/** Importers write one already-bounded immutable object; they never choose a path. */
class IWoTObjectWriter
{
public:
	virtual ~IWoTObjectWriter() = default;
	virtual FWoTBootstrapStatus WriteObject(FStringView LogicalId, FStringView Schema,
		TConstArrayView<uint8> Bytes, const FWoTSha256& Sha256,
		uint64 MaximumBytes) = 0;
};

/** A bounded progress/cancellation surface shared by every typed importer. */
class IWoTProgressSink
{
public:
	virtual ~IWoTProgressSink() = default;
	virtual FWoTBootstrapStatus ReportProgress(FStringView Stage, uint64 Completed,
		uint64 Total) = 0;
	virtual bool IsCancellationRequested() const = 0;
};

WOTRETAILREADER_API TWoTBootstrapResult<FWoTRetailContract> ParseRetailContract(
	TArrayView<const uint8> CanonicalBytes, const FWoTKeyRing& Keys);

/** Parse the canonical payload grammar and fact closure without a signature. */
WOTRETAILREADER_API TWoTBootstrapResult<FWoTRetailContract> ParseRetailContractPayload(
	TArrayView<const uint8> CanonicalPayloadBytes);

WOTRETAILREADER_API FWoTBootstrapStatus ValidateFactClosure(
	const FWoTRetailContract& Contract);
