#include "WoTCacheLease.h"

#include "Misc/ScopeLock.h"
#include "WoTBootstrapCore/NativeHandles.h"
#include "WoTBootstrapCore/Hash.h"
#include "WoTRetailReader/CacheBuilder.h"
#include "WoTRetailReader/Contract.h"
#include "WoTRetailReader/FrontEndDescriptorSidecar.h"
#include "WoTRetailReader/ProductionContract.h"
#include "WoTVerifiedBlobReader.h"

namespace
{
#if WITH_AUTOMATION_TESTS
// Intentional process-lifetime storage: a static or cross-TU token may drain
// during teardown, after ordinary namespace statics would have been destroyed.
FCriticalSection& GWoTCacheSnapshotPhysicalReadObserverGuard =
	*new FCriticalSection();
TArray<TSharedPtr<TFunction<void(FStringView)>, ESPMode::ThreadSafe>>
	& GWoTCacheSnapshotPhysicalReadObservers =
		*new TArray<TSharedPtr<TFunction<void(FStringView)>,
			ESPMode::ThreadSafe>>();
TSet<const void*>& GWoTActiveCacheSnapshotPhysicalReadObservers =
	*new TSet<const void*>();
TMap<const void*, int32>& GWoTCacheSnapshotPhysicalReadObserverInFlight =
	*new TMap<const void*, int32>();
TMap<const void*, TMap<uint32, int32>>
	& GWoTCacheSnapshotPhysicalReadObserverThreads =
		*new TMap<const void*, TMap<uint32, int32>>();
	TMap<const void*, FEvent*>
	& GWoTCacheSnapshotPhysicalReadObserverDrainedEvents =
		*new TMap<const void*, FEvent*>();
	FCriticalSection& GWoTCacheRevocationStartedObserverGuard =
		*new FCriticalSection();
	TFunction<void()>& GWoTCacheRevocationStartedObserver =
		*new TFunction<void()>();
#endif

template <typename T>
TWoTBootstrapResult<T> WoTCacheLeaseError(EWoTBootstrapError Code,
	const TCHAR* Message, const FString& Path = FString())
{
	return TWoTBootstrapResult<T>::Error(
		MakeWoTBootstrapError(Code, Message, Path));
}

FWoTBootstrapStatus WoTCacheLeaseStatus(EWoTBootstrapError Code,
	const TCHAR* Message, const FString& Path = FString())
{
	return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(Code, Message, Path));
}

struct FWoTCacheVerifiedDescriptorKey
{
	FString LogicalId;
	FString Schema;
	uint64 Size = 0;
	FWoTSha256 Sha256;

	bool operator==(const FWoTCacheVerifiedDescriptorKey& Other) const
	{
		return LogicalId == Other.LogicalId && Schema == Other.Schema
			&& Size == Other.Size && Sha256 == Other.Sha256;
	}

	friend uint32 GetTypeHash(const FWoTCacheVerifiedDescriptorKey& Key)
	{
		uint32 Hash = GetTypeHash(Key.LogicalId);
		Hash = HashCombine(Hash, GetTypeHash(Key.Schema));
		Hash = HashCombine(Hash, GetTypeHash(Key.Size));
		for (uint8 Byte : Key.Sha256.Bytes)
		{
			Hash = HashCombine(Hash, GetTypeHash(Byte));
		}
		return Hash;
	}
};

FWoTCacheVerifiedDescriptorKey WoTCacheDescriptorKey(
	const FWoTCacheBlobDescriptor& Descriptor)
{
	return {Descriptor.LogicalId, Descriptor.Schema,
		Descriptor.Size, Descriptor.Sha256};
}
}

#if WITH_AUTOMATION_TESTS
TSharedPtr<uint8, ESPMode::ThreadSafe>
WoTObserveCacheSnapshotPhysicalReadsForTesting(
	TFunction<void(FStringView)> Observer)
{
	return WoTRegisterAutomationObserverForTesting(
		GWoTCacheSnapshotPhysicalReadObserverGuard,
		GWoTCacheSnapshotPhysicalReadObservers,
		GWoTActiveCacheSnapshotPhysicalReadObservers,
		GWoTCacheSnapshotPhysicalReadObserverInFlight,
		GWoTCacheSnapshotPhysicalReadObserverThreads,
		GWoTCacheSnapshotPhysicalReadObserverDrainedEvents, MoveTemp(Observer));
}

void WoTObserveCacheSnapshotPhysicalReadForTesting(FStringView LogicalObject)
{
	WoTBroadcastAutomationObserverForTesting(
		GWoTCacheSnapshotPhysicalReadObserverGuard,
		GWoTCacheSnapshotPhysicalReadObservers,
		GWoTActiveCacheSnapshotPhysicalReadObservers,
		GWoTCacheSnapshotPhysicalReadObserverInFlight,
		GWoTCacheSnapshotPhysicalReadObserverThreads,
		GWoTCacheSnapshotPhysicalReadObserverDrainedEvents, LogicalObject);
}

void WoTSetCacheRevocationStartedObserverForTesting(TFunction<void()> Observer)
{
	FScopeLock Lock(&GWoTCacheRevocationStartedObserverGuard);
	GWoTCacheRevocationStartedObserver = MoveTemp(Observer);
}
#endif

struct FWoTCacheGenerationSharedState
{
	FString CanonicalRoot;
	FWoTNativeFileIdentity RootIdentity;
	FWoTSha256 ManifestSha256;
	FWoTCacheGenerationBuildIdentity BuildIdentity;
	TArray<FWoTWorldDescriptor> WorldDescriptors;
	bool bHasFrontEndDescriptorAttestation = false;
	FWoTSha256 ExpectedFrontEndDescriptorRoot;
	TSharedPtr<const FWoTFrontEndDescriptor, ESPMode::ThreadSafe>
		VerifiedFrontEndDescriptor;
	FWoTNativeDirectoryHandle GenerationDirectory;
	FWoTNativeDirectoryHandle BlobDirectory;
	TMap<FString, FWoTCacheLeaseManifestEntry> ManifestTable;

	mutable FCriticalSection Guard;
	mutable FCriticalSection VerifiedReadGuard;
	bool bRevoked = false;
	bool bRevocationInProgress = false;
	bool bRuntimeIntegrityFailure = false;
	bool bRuntimeIntegrityFrontEndIndependent = false;

	bool bReadFailureTaken = false;
	FString RuntimeIntegrityFailureOutput;
	FString RuntimeIntegrityFailureReason;
	bool bStartupCriticalClosureVerified = false;
	uint64 VerifiedCriticalObjectCount = 0;
	uint64 VerifiedCriticalBytes = 0;
	TMap<FWoTCacheVerifiedDescriptorKey,
		TWeakPtr<const TArray<uint8>, ESPMode::ThreadSafe>> VerifiedBuffers;
	TMap<FWoTCacheVerifiedDescriptorKey,
		TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe>> CriticalBuffers;
	uint64 NextRevocationSubscription = 1;
	TMap<uint64, TFunction<bool()>> RevocationSubscriptions;

	bool IsLive() const
	{
		FScopeLock Lock(&Guard);
		return !bRevoked && !CanonicalRoot.IsEmpty()
			&& GenerationDirectory.IsValid() && BlobDirectory.IsValid();
	}

	bool HasVerifiedStartupCriticalClosure() const
	{
		FScopeLock Lock(&Guard);
		return !bRevoked && bStartupCriticalClosureVerified;
	}

	bool CanPublishPresentation() const
	{
		FScopeLock Lock(&Guard);
		return !bRevoked && !bRevocationInProgress;
	}

	bool MayBeginVerifiedReadUnderVerifiedReadGuard() const
	{
		FScopeLock Lock(&Guard);
		return !bRevoked && !bRevocationInProgress
			&& !CanonicalRoot.IsEmpty()
			&& GenerationDirectory.IsValid() && BlobDirectory.IsValid();
	}

	bool BeginRevocationUnderVerifiedReadGuard(
		TArray<TFunction<bool()>>& OutCallbacks)
	{
		FScopeLock Lock(&Guard);
		if (bRevoked || bRevocationInProgress) return false;
		bRevocationInProgress = true;
		RevocationSubscriptions.GenerateValueArray(OutCallbacks);
#if WITH_AUTOMATION_TESTS
		TFunction<void()> Observer;
		{
			FScopeLock ObserverLock(&GWoTCacheRevocationStartedObserverGuard);
			Observer = GWoTCacheRevocationStartedObserver;
		}
		if (Observer) Observer();
#endif
		return true;
	}

	bool BeginRuntimeIntegrityFailureUnderVerifiedReadGuard(
		const FString& LogicalId, const FString& Reason,
		TArray<TFunction<bool()>>& OutCallbacks)
	{
		{
			FScopeLock Lock(&Guard);
			if (bRevoked || bRevocationInProgress) return false;
			bRuntimeIntegrityFailure = true;
			RuntimeIntegrityFailureOutput = LogicalId;
			RuntimeIntegrityFailureReason = Reason;
			auto HasRetainedCritical = [this](const FString& Candidate)
			{
				for (const TPair<FWoTCacheVerifiedDescriptorKey,
					TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe>>& Pair
					: CriticalBuffers)
				{
					if (Pair.Key.LogicalId == Candidate) return true;
				}
				return false;
			};
			const bool bFailedCritical = HasRetainedCritical(LogicalId);
			bool bFrontEndClosureCritical = bStartupCriticalClosureVerified
				&& bHasFrontEndDescriptorAttestation
				&& VerifiedFrontEndDescriptor.IsValid();
			if (bFrontEndClosureCritical)
			{
				for (const FString& Required :
					VerifiedFrontEndDescriptor->RequiredLogicalIds)
				{
					if (!HasRetainedCritical(Required))
					{
						bFrontEndClosureCritical = false;
						break;
					}
				}
			}
			bRuntimeIntegrityFrontEndIndependent =
				!bFailedCritical && bFrontEndClosureCritical;
			bRevocationInProgress = true;
			RevocationSubscriptions.GenerateValueArray(OutCallbacks);
		}
#if WITH_AUTOMATION_TESTS
		TFunction<void()> Observer;
		{
			FScopeLock ObserverLock(&GWoTCacheRevocationStartedObserverGuard);
			Observer = GWoTCacheRevocationStartedObserver;
		}
		if (Observer) Observer();
#endif
		return true;
	}

	void CompleteDeferredRevocation()
	{
		FScopeLock Serialized(&VerifiedReadGuard);
		FScopeLock Lock(&Guard);
		if (!bRevocationInProgress || bRevoked) return;
		bRevoked = true;
		bRevocationInProgress = false;
		VerifiedBuffers.Reset();
		CriticalBuffers.Reset();
		RevocationSubscriptions.Reset();
	}

	void FinishRevocation(TArray<TFunction<bool()>>&& Callbacks)
	{
		// Subscribers are teardown barriers, not notifications. Authority remains
		// live until every presentation has synchronously become inert. They run
		// outside VerifiedReadGuard so an off-thread verifier can wait for the game
		// thread without forming reader -> game thread -> reader lock inversion.
		bool bEveryBarrierComplete = true;
		for (TFunction<bool()>& Callback : Callbacks)
			if (Callback) bEveryBarrierComplete &= Callback();
		if (bEveryBarrierComplete) CompleteDeferredRevocation();
	}

	void Revoke()
	{
		TArray<TFunction<bool()>> Callbacks;
		{
			FScopeLock Serialized(&VerifiedReadGuard);
			if (!BeginRevocationUnderVerifiedReadGuard(Callbacks)) return;
		}
		FinishRevocation(MoveTemp(Callbacks));
	}
};

namespace
{
const FString& WoTCacheEmptyString()
{
	static const FString Empty;
	return Empty;
}

const FWoTSha256& WoTCacheEmptySha()
{
	static const FWoTSha256 Empty;
	return Empty;
}

const FWoTCacheGenerationBuildIdentity& WoTCacheEmptyBuildIdentity()
{
	static const FWoTCacheGenerationBuildIdentity Empty;
	return Empty;
}

const TMap<FString, FWoTCacheLeaseManifestEntry>& WoTCacheEmptyManifestTable()
{
	static const TMap<FString, FWoTCacheLeaseManifestEntry> Empty;
	return Empty;
}

const FWoTNativeDirectoryHandle& WoTCacheEmptyDirectoryHandle()
{
	static const FWoTNativeDirectoryHandle Empty;
	return Empty;
}
}

FWoTCacheGenerationLease::FWoTCacheGenerationLease(
	TSharedPtr<FWoTCacheGenerationSharedState, ESPMode::ThreadSafe> InState)
	: State(MoveTemp(InState))
{
}

FWoTCacheGenerationLease::~FWoTCacheGenerationLease() = default;
FWoTCacheGenerationLease::FWoTCacheGenerationLease(
	FWoTCacheGenerationLease&& Other) = default;
FWoTCacheGenerationLease& FWoTCacheGenerationLease::operator=(
	FWoTCacheGenerationLease&& Other) = default;

const FString& FWoTCacheGenerationLease::GetCanonicalRoot() const
{
	return State.IsValid() ? State->CanonicalRoot : WoTCacheEmptyString();
}

FWoTNativeFileIdentity FWoTCacheGenerationLease::GetRootIdentity() const
{
	return State.IsValid() ? State->RootIdentity : FWoTNativeFileIdentity{};
}

const FWoTSha256& FWoTCacheGenerationLease::GetManifestSha256() const
{
	return State.IsValid() ? State->ManifestSha256 : WoTCacheEmptySha();
}

const FWoTCacheGenerationBuildIdentity&
FWoTCacheGenerationLease::GetBuildIdentity() const
{
	return State.IsValid() ? State->BuildIdentity : WoTCacheEmptyBuildIdentity();
}

bool FWoTCacheGenerationLease::HasFrontEndDescriptorAttestation() const
{
	return State.IsValid() && State->bHasFrontEndDescriptorAttestation;
}

const FWoTSha256& FWoTCacheGenerationLease::GetExpectedFrontEndDescriptorRoot() const
{
	return State.IsValid() ? State->ExpectedFrontEndDescriptorRoot : WoTCacheEmptySha();
}

uint64 FWoTCacheGenerationLease::GetVerifiedCriticalObjectCount() const
{
	return State.IsValid() ? State->VerifiedCriticalObjectCount : 0;
}

uint64 FWoTCacheGenerationLease::GetVerifiedCriticalBytes() const
{
	return State.IsValid() ? State->VerifiedCriticalBytes : 0;
}



bool FWoTCacheGenerationLease::IsValid() const
{
	return State.IsValid() && State->IsLive();
}

bool FWoTCacheGenerationLease::HasRuntimeIntegrityFailure() const
{
	if (!State.IsValid()) return false;
	FScopeLock Lock(&State->Guard);
	return State->bRuntimeIntegrityFailure;
}

bool FWoTCacheGenerationLease::CanRecoverFrontEndAfterRuntimeIntegrityFailure() const
{
	if (!State.IsValid()) return false;
	FScopeLock Lock(&State->Guard);
	return State->bRuntimeIntegrityFailure
		&& State->bRuntimeIntegrityFrontEndIndependent;
}

FString FWoTCacheGenerationLease::GetRuntimeIntegrityFailureOutput() const
{
	if (!State.IsValid()) return FString();
	FScopeLock Lock(&State->Guard);
	return State->RuntimeIntegrityFailureOutput;
}

FString FWoTCacheGenerationLease::GetRuntimeIntegrityFailureReason() const
{
	if (!State.IsValid()) return FString();
	FScopeLock Lock(&State->Guard);
	return State->RuntimeIntegrityFailureReason;
}

TOptional<FWoTBootstrapError> FWoTCacheGenerationLease::TakeReadFailure()
{
	if (!State.IsValid()) return {};
	FScopeLock Lock(&State->Guard);
	if (!State->bRuntimeIntegrityFailure || State->bReadFailureTaken) return {};
	State->bReadFailureTaken = true;
	return MakeWoTBootstrapError(EWoTBootstrapError::CorruptInstallation,
		State->RuntimeIntegrityFailureReason, State->RuntimeIntegrityFailureOutput);
}

TConstArrayView<FWoTWorldDescriptor> FWoTCacheGenerationLease::GetWorldDescriptors() const
{
	return State.IsValid() ? TConstArrayView<FWoTWorldDescriptor>(State->WorldDescriptors)
		: TConstArrayView<FWoTWorldDescriptor>();
}

const FWoTFrontEndDescriptor& FWoTCacheGenerationLease::GetFrontEndDescriptor() const
{
	check(State.IsValid() && State->VerifiedFrontEndDescriptor.IsValid());
	return *State->VerifiedFrontEndDescriptor;
}

FString FWoTCacheGenerationLease::MarkDamagedBestEffort() const
{
	if (!State.IsValid()) return TEXT("cache snapshot unavailable");
	bool bCreated = false;
	auto Marker = OpenOrCreateNativeFileAtReadWriteNoFollow(
		State->GenerationDirectory, TEXT("damaged"), bCreated);
	if (Marker.IsError())
	{
		UE_LOG(LogTemp, Warning, TEXT("Could not mark damaged cache %s: %s"),
			*State->CanonicalRoot, *Marker.GetError().Message);
		return Marker.GetError().ToString();
	}
	auto Empty = TruncateNativeFile(Marker.GetValue(), 0);
	if (Empty.IsError())
	{
		UE_LOG(LogTemp, Warning, TEXT("Could not mark damaged cache %s: %s"),
			*State->CanonicalRoot, *Empty.GetError().Message);
		return Empty.GetError().ToString();
	}
	return FString();
}

bool FWoTCacheGenerationLease::SharesSnapshotWith(
	const FWoTCacheGenerationLease& Other) const
{
	return State.IsValid() && State == Other.State;
}

TWoTBootstrapResult<FWoTFrontEndDescriptor>
WoTDeriveFrontEndDescriptorFromActiveGeneration(
	const FWoTCacheGenerationLease& Lease)
{
	using FResult = TWoTBootstrapResult<FWoTFrontEndDescriptor>;
	if (!Lease.IsValid())
	{
		return WoTCacheLeaseError<FWoTFrontEndDescriptor>(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("front-end descriptor derivation requires one live world-domain lease"));
	}
	FScopeLock VerifiedOperation(&Lease.State->VerifiedReadGuard);
	if (!Lease.MayBeginVerifiedReadUnderVerifiedReadGuard())
	{
		return WoTCacheLeaseError<FWoTFrontEndDescriptor>(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("front-end descriptor derivation observed a revoked generation lease"));
	}
	const TSharedPtr<const FWoTFrontEndDescriptor, ESPMode::ThreadSafe> Descriptor =
		Lease.State->VerifiedFrontEndDescriptor;
	if (!Descriptor.IsValid())
	{
		return WoTCacheLeaseError<FWoTFrontEndDescriptor>(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("the retained front-end descriptor authority is incomplete"),
			Lease.GetManifestSha256().ToHex());
	}
	return FResult::Success(*Descriptor);
}

TWoTBootstrapResult<TArray<FWoTCacheLeaseManifestEntry>>
WoTDeriveFrontEndWorldManifestAuthorityFromActiveGeneration(
	const FWoTCacheGenerationLease& Lease)
{
	using FResult =
		TWoTBootstrapResult<TArray<FWoTCacheLeaseManifestEntry>>;
	auto Derived = WoTDeriveFrontEndDescriptorFromActiveGeneration(Lease);
	if (Derived.IsError())
	{
		return FResult::Error(MoveTemp(Derived).GetError());
	}
	TWoTBootstrapResult<FWoTRetailContract> ProductionContract =
		WoTLoadProductionRetailContract();
	if (ProductionContract.IsError())
	{
		return FResult::Error(MoveTemp(ProductionContract).GetError());
	}

	FScopeLock VerifiedOperation(&Lease.State->VerifiedReadGuard);
	if (!Lease.MayBeginVerifiedReadUnderVerifiedReadGuard())
	{
		return WoTCacheLeaseError<TArray<FWoTCacheLeaseManifestEntry>>(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("front-end world authority observed a revoked generation lease"));
	}
	const FWoTFrontEndDescriptor& Descriptor = Derived.GetValue();
	const FWoTFrontEndResourceRequirement* WorldRequirement = nullptr;
	const FWoTFrontEndResourceRequirement* BSPRequirement = nullptr;
	for (const FWoTFrontEndResourceRequirement& Requirement :
		Descriptor.RequiredResources)
	{
		if (Requirement.Domain == EWoTFactDomain::World)
		{
			if (WorldRequirement != nullptr)
			{
				return WoTCacheLeaseError<TArray<FWoTCacheLeaseManifestEntry>>(
					EWoTBootstrapError::CorruptInstallation,
					TEXT("the retained front-end descriptor has duplicate world authority"));
			}
			WorldRequirement = &Requirement;
		}
		else if (Requirement.Domain == EWoTFactDomain::BSP)
		{
			if (BSPRequirement != nullptr)
			{
				return WoTCacheLeaseError<TArray<FWoTCacheLeaseManifestEntry>>(
					EWoTBootstrapError::CorruptInstallation,
					TEXT("the retained front-end descriptor has duplicate BSP authority"));
			}
			BSPRequirement = &Requirement;
		}
	}
	auto ResolveFact = [&](const FWoTFrontEndResourceRequirement* Requirement,
		EWoTFactDomain Domain, const FString& DescriptorLogicalId,
		const TCHAR* Importer, const TCHAR* Schema,
		const FWoTRequiredFact*& OutFact) -> bool
	{
		if (Requirement == nullptr
			|| !Requirement->LogicalId.Equals(
				DescriptorLogicalId, ESearchCase::CaseSensitive))
		{
			return false;
		}
		for (const FWoTRequiredFact& Fact : ProductionContract.GetValue().Facts)
		{
			if (Fact.Domain != Domain)
			{
				continue;
			}
			const bool bSameSource = Fact.SourcePath.Equals(
				Requirement->SourcePath, ESearchCase::CaseSensitive)
				&& Fact.SourceExport.Equals(
					Requirement->SourceExport, ESearchCase::CaseSensitive);
			bool bNamesLogicalId = false;
			for (const FWoTRequiredFactOutput& Output : Fact.Outputs)
			{
				bNamesLogicalId |= Output.LogicalId.Equals(
					Requirement->LogicalId, ESearchCase::CaseSensitive);
			}
			if (!bSameSource && !bNamesLogicalId)
			{
				continue;
			}
			if (OutFact != nullptr)
			{
				return false;
			}
			OutFact = &Fact;
		}
		return OutFact != nullptr
			&& OutFact->Domain == Domain
			&& OutFact->SourcePath.Equals(
				Requirement->SourcePath, ESearchCase::CaseSensitive)
			&& OutFact->SourceExport.Equals(
				Requirement->SourceExport, ESearchCase::CaseSensitive)
			&& OutFact->Importer.Equals(Importer, ESearchCase::CaseSensitive)
			&& OutFact->Outputs.Num() == 1
			&& OutFact->Outputs[0].LogicalId.Equals(
				Requirement->LogicalId, ESearchCase::CaseSensitive)
			&& OutFact->Outputs[0].Schema.Equals(
				Schema, ESearchCase::CaseSensitive);
	};
	const FWoTRequiredFact* WorldFact = nullptr;
	const FWoTRequiredFact* BSPFact = nullptr;
	if (!ResolveFact(WorldRequirement, EWoTFactDomain::World,
			Descriptor.WorldLogicalId, TEXT("world-v1"), TEXT("wot-world-v3"),
			WorldFact)
		|| !ResolveFact(BSPRequirement, EWoTFactDomain::BSP,
			Descriptor.BspLogicalId, TEXT("bsp-v1"), TEXT("wot-bsp-v3"), BSPFact))
	{
		return WoTCacheLeaseError<TArray<FWoTCacheLeaseManifestEntry>>(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("the retained front-end world authority is not exact in the production contract"));
	}
	auto FindExactManifestEntry = [&](const FString& LogicalId)
		-> const FWoTCacheLeaseManifestEntry*
	{
		const FWoTCacheLeaseManifestEntry* Match = nullptr;
		for (const TPair<FString, FWoTCacheLeaseManifestEntry>& Pair :
			Lease.State->ManifestTable)
		{
			if (Pair.Key.Equals(LogicalId, ESearchCase::CaseSensitive))
			{
				if (Match != nullptr)
				{
					return nullptr;
				}
				Match = &Pair.Value;
			}
		}
		return Match;
	};
	const FWoTCacheLeaseManifestEntry* World =
		FindExactManifestEntry(Descriptor.WorldLogicalId);
	const FWoTCacheLeaseManifestEntry* BSP =
		FindExactManifestEntry(Descriptor.BspLogicalId);
	if (World == nullptr || BSP == nullptr
		|| !World->Schema.Equals(TEXT("wot-world-v3"), ESearchCase::CaseSensitive)
		|| !BSP->Schema.Equals(TEXT("wot-bsp-v3"), ESearchCase::CaseSensitive)
		|| World->Satisfies.Num() != 1 || BSP->Satisfies.Num() != 1
		|| !World->Satisfies[0].Equals(
			WorldFact->FactId, ESearchCase::CaseSensitive)
		|| !BSP->Satisfies[0].Equals(
			BSPFact->FactId, ESearchCase::CaseSensitive))
	{
		return WoTCacheLeaseError<TArray<FWoTCacheLeaseManifestEntry>>(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("the separately attested front-end world objects are not exactly bound"));
	}
	TArray<FWoTCacheLeaseManifestEntry> Authority;
	Authority.Add(*World);
	Authority.Add(*BSP);
	return FResult::Success(MoveTemp(Authority));
}

TWoTBootstrapResult<FWoTCacheGenerationLease> FWoTCacheGenerationLease::Acquire(
	FString InCanonicalRoot, const FWoTNativeFileIdentity& InRootIdentity,
	const FWoTSha256& InManifestSha256,
	const FWoTCacheGenerationBuildIdentity& InBuildIdentity,
	TArray<FWoTWorldDescriptor>&& InWorldDescriptors,
	const FWoTSha256& InExpectedFrontEndDescriptorRoot,
	FWoTNativeDirectoryHandle&& InGenerationDirectory,
	TArray<FWoTCacheLeaseManifestEntry>&& InManifestEntries)
{
	using FResult = TWoTBootstrapResult<FWoTCacheGenerationLease>;
	auto Blobs = OpenNativeDirectoryAtNoFollow(InGenerationDirectory, TEXT("blobs"));
	if (Blobs.IsError()) return FResult::Error(MoveTemp(Blobs).GetError());
	auto Shared = MakeShared<FWoTCacheGenerationSharedState, ESPMode::ThreadSafe>();
	Shared->CanonicalRoot = MoveTemp(InCanonicalRoot);
	Shared->RootIdentity = InRootIdentity;
	Shared->ManifestSha256 = InManifestSha256;
	Shared->BuildIdentity = InBuildIdentity;
	Shared->WorldDescriptors = MoveTemp(InWorldDescriptors);
	Shared->ExpectedFrontEndDescriptorRoot = InExpectedFrontEndDescriptorRoot;
	Shared->GenerationDirectory = MoveTemp(InGenerationDirectory);
	Shared->BlobDirectory = MoveTemp(Blobs).GetValue();
	for (FWoTCacheLeaseManifestEntry& Entry : InManifestEntries)
		Shared->ManifestTable.Add(Entry.LogicalId, MoveTemp(Entry));
	return FResult::Success(FWoTCacheGenerationLease(MoveTemp(Shared)));
}

const FWoTCacheLeaseManifestEntry* FWoTCacheGenerationLease::FindManifestEntry(
	const FString& LogicalId) const
{
	return State.IsValid() ? State->ManifestTable.Find(LogicalId) : nullptr;
}

const TMap<FString, FWoTCacheLeaseManifestEntry>&
FWoTCacheGenerationLease::GetManifestTable() const
{
	return State.IsValid() ? State->ManifestTable : WoTCacheEmptyManifestTable();
}

const FWoTNativeDirectoryHandle&
FWoTCacheGenerationLease::GetGenerationDirectory() const
{
	return State.IsValid() ? State->GenerationDirectory : WoTCacheEmptyDirectoryHandle();
}

const FWoTNativeDirectoryHandle& FWoTCacheGenerationLease::GetBlobDirectory() const
{
	return State.IsValid() ? State->BlobDirectory : WoTCacheEmptyDirectoryHandle();
}

FCriticalSection& FWoTCacheGenerationLease::GetVerifiedReadGuard() const
{
	check(State.IsValid());
	return State->VerifiedReadGuard;
}

bool FWoTCacheGenerationLease::MayBeginVerifiedReadUnderVerifiedReadGuard() const
{
	return State.IsValid()
		&& State->MayBeginVerifiedReadUnderVerifiedReadGuard();
}

TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe>
FWoTCacheGenerationLease::FindVerifiedBuffer(
	const FWoTCacheBlobDescriptor& Descriptor) const
{
	if (!State.IsValid()) return {};
	FScopeLock Lock(&State->Guard);
	if (State->bRevoked || State->bRevocationInProgress) return {};
	const FWoTCacheVerifiedDescriptorKey Key = WoTCacheDescriptorKey(Descriptor);
	if (const TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe>* Critical =
		State->CriticalBuffers.Find(Key))
	{
		return *Critical;
	}
	if (const TWeakPtr<const TArray<uint8>, ESPMode::ThreadSafe>* Weak =
		State->VerifiedBuffers.Find(Key))
	{
		return Weak->Pin();
	}
	return {};
}

void FWoTCacheGenerationLease::RememberVerifiedBuffer(
	const FWoTCacheBlobDescriptor& Descriptor,
	const TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe>& Buffer) const
{
	if (!State.IsValid() || !Buffer.IsValid()) return;
	FScopeLock Lock(&State->Guard);
	if (!State->bRevoked && !State->bRevocationInProgress)
	{
		State->VerifiedBuffers.Add(WoTCacheDescriptorKey(Descriptor), Buffer);
	}
}

bool FWoTCacheGenerationLease::PinCriticalBuffer(
	const FWoTCacheBlobDescriptor& Descriptor,
	const TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe>& Buffer) const
{
	if (!State.IsValid() || !Buffer.IsValid()) return false;
	FScopeLock Serialized(&State->VerifiedReadGuard);
	if (!MayBeginVerifiedReadUnderVerifiedReadGuard()) return false;
	FScopeLock Lock(&State->Guard);
	if (!State->bRevoked && !State->bRevocationInProgress)
	{
		const FWoTCacheVerifiedDescriptorKey Key = WoTCacheDescriptorKey(Descriptor);
		State->VerifiedBuffers.Add(Key, Buffer);
		State->CriticalBuffers.Add(Key, Buffer);
		return true;
	}
	return false;
}

bool FWoTCacheGenerationLease::CanPublishPresentation() const
{
	return State.IsValid() && State->CanPublishPresentation();
}

bool FWoTCacheGenerationLease::BeginRevocationUnderVerifiedReadGuard(
	TArray<TFunction<bool()>>& OutCallbacks) const
{
	return State.IsValid()
		&& State->BeginRevocationUnderVerifiedReadGuard(OutCallbacks);
}

bool FWoTCacheGenerationLease::BeginRuntimeIntegrityFailureUnderVerifiedReadGuard(
	const FString& LogicalId, const FString& Reason,
	TArray<TFunction<bool()>>& OutCallbacks) const
{
	return State.IsValid()
		&& State->BeginRuntimeIntegrityFailureUnderVerifiedReadGuard(
			LogicalId, Reason, OutCallbacks);
}

void FWoTCacheGenerationLease::FinishRevocation(
	TArray<TFunction<bool()>>&& Callbacks) const
{
	if (State.IsValid()) State->FinishRevocation(MoveTemp(Callbacks));
}

bool FWoTCacheGenerationLease::IsRevocationInProgress() const
{
	if (!State.IsValid()) return false;
	FScopeLock Lock(&State->Guard);
	return State->bRevocationInProgress && !State->bRevoked;
}

void FWoTCacheGenerationLease::CompleteDeferredRevocation() const
{
	if (State.IsValid()) State->CompleteDeferredRevocation();
}

void FWoTCacheGenerationLease::Revoke() const
{
	if (State.IsValid()) State->Revoke();
}

uint64 FWoTCacheGenerationLease::SubscribeRevocation(
	TFunction<bool()> Callback) const
{
	if (!State.IsValid() || !Callback) return 0;
	FScopeLock Lock(&State->Guard);
	if (State->bRevoked || State->bRevocationInProgress) return 0;
	const uint64 Subscription = State->NextRevocationSubscription++;
	State->RevocationSubscriptions.Add(Subscription, MoveTemp(Callback));
	return Subscription;
}

void FWoTCacheGenerationLease::UnsubscribeRevocation(uint64 Subscription) const
{
	if (!State.IsValid() || Subscription == 0) return;
	FScopeLock Lock(&State->Guard);
	State->RevocationSubscriptions.Remove(Subscription);
}

FWoTBootstrapStatus FWoTCacheGenerationLease::VerifyCriticalClosure(IWoTProgressSink* Progress)
{
	using FResult = FWoTBootstrapStatus;
	auto Contract = WoTLoadProductionRetailContract();
	if (Contract.IsError()) return FResult::Error(MoveTemp(Contract).GetError());
	if (State->BuildIdentity.ContractSha256 != ComputeRetailContractIdentity(Contract.GetValue()))
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the cache contract identity is incompatible")));
	return VerifyLogicalIds(Contract.GetValue().StartupCriticalLogicalIds, Progress, true);
}

#if WITH_AUTOMATION_TESTS
FWoTBootstrapStatus FWoTCacheGenerationLease::VerifyCriticalClosureForTesting(
	TConstArrayView<FString> LogicalIds, IWoTProgressSink* Progress)
{
	return VerifyLogicalIds(LogicalIds, Progress, false);
}
#endif

FWoTBootstrapStatus FWoTCacheGenerationLease::VerifyLogicalIds(
	TConstArrayView<FString> LogicalIds, IWoTProgressSink* Progress, bool bRequireFrontEnd)
{
	using FResult = FWoTBootstrapStatus;

	FWoTCacheGenerationLease Reader(State);
	TSet<FString> Seen;
	uint64 TotalBytes = 0;
	for (const FString& LogicalId : LogicalIds)
	{
		const FWoTCacheLeaseManifestEntry* Entry = Reader.FindManifestEntry(LogicalId);
		if (Entry == nullptr || Entry->Size > TNumericLimits<uint64>::Max() - TotalBytes)
		{
			Reader.Revoke();
			return WoTCacheLeaseStatus(
				EWoTBootstrapError::SizeLimit,
				TEXT("the critical closure byte total is missing or overflows"), LogicalId);
		}
		TotalBytes += Entry->Size;
	}
	uint64 VerifiedBytes = 0;
	for (int32 Index = 0; Index < LogicalIds.Num(); ++Index)
	{
		const FString& LogicalId = LogicalIds[Index];
		const FWoTCacheLeaseManifestEntry* Entry = Reader.FindManifestEntry(LogicalId);
		if (LogicalId.IsEmpty() || Seen.Contains(LogicalId) || Entry == nullptr)
		{
			Reader.Revoke();
			return WoTCacheLeaseStatus(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("the critical closure is missing, empty, or non-unique"), LogicalId);
		}
		Seen.Add(LogicalId);
		if (Progress != nullptr && Progress->IsCancellationRequested())
		{
			return WoTCacheLeaseStatus(
				EWoTBootstrapError::ServiceUnavailable,
				TEXT("critical cache verification was cancelled"), LogicalId);
		}
		FWoTCacheBlobDescriptor Descriptor{Entry->LogicalId, Entry->Schema,
			Entry->Size, Entry->Sha256};
		TWoTBootstrapResult<FWoTImmutableVerifiedBlob> Read =
			ReadVerifiedCacheBlob(Reader, Descriptor, FWoTCacheBlobReadLimits());
		if (Read.IsError())
		{
			return FResult::Error(MoveTemp(Read).GetError());
		}
		if (!Reader.PinCriticalBuffer(Descriptor, Read.GetValue().Buffer))
		{
			return WoTCacheLeaseStatus(
				EWoTBootstrapError::Io,
				TEXT("critical cache verification lost read authority"), LogicalId);
		}
		VerifiedBytes += Entry->Size;
		{
			FScopeLock Lock(&State->Guard);
			State->VerifiedCriticalObjectCount = static_cast<uint64>(Index + 1);
			State->VerifiedCriticalBytes = VerifiedBytes;
		}
		if (Progress != nullptr)
		{
			FWoTBootstrapStatus Reported = Progress->ReportProgress(
				TEXT("cache-critical-verify"), static_cast<uint64>(Index + 1),
				static_cast<uint64>(LogicalIds.Num()));
			if (Reported.IsError())
			{
				return FResult::Error(MoveTemp(Reported).GetError());
			}
			Reported = Progress->ReportProgress(TEXT("cache-critical-verify-bytes"),
				VerifiedBytes, TotalBytes);
			if (Reported.IsError())
				return FResult::Error(MoveTemp(Reported).GetError());
		}
	}
	{
		FScopeLock Serialized(&State->VerifiedReadGuard);
		if (!State->MayBeginVerifiedReadUnderVerifiedReadGuard())
			return WoTCacheLeaseStatus(
				EWoTBootstrapError::Io,
				TEXT("critical cache verification lost final read authority"));
		FScopeLock Lock(&State->Guard);
		State->bStartupCriticalClosureVerified = true;
	}
	const FWoTCacheLeaseManifestEntry* FrontEndEntry = FindManifestEntry(TEXT("cache/front_end_world/WheelOfTime"));
	if (FrontEndEntry == nullptr && !bRequireFrontEnd) return FResult::Success();
	if (FrontEndEntry == nullptr) return FResult::Error(MakeWoTBootstrapError(
		EWoTBootstrapError::CorruptInstallation, TEXT("the cache omits the front-end descriptor")));
	FWoTCacheBlobDescriptor FrontEndBlob{FrontEndEntry->LogicalId, FrontEndEntry->Schema,
		FrontEndEntry->Size, FrontEndEntry->Sha256};
	auto FrontEndRead = ReadVerifiedCacheBlob(*this, FrontEndBlob, FWoTCacheBlobReadLimits());
	if (FrontEndRead.IsError()) return FResult::Error(MoveTemp(FrontEndRead).GetError());
	auto FrontEnd = ParseSealedFrontEndDescriptorSidecar(FrontEndRead.GetValue().Bytes(),
		State->BuildIdentity.InstallerSha256, State->BuildIdentity.ContractSha256,
		State->BuildIdentity.ProductionDescriptorSha256, State->ExpectedFrontEndDescriptorRoot);
	if (FrontEnd.IsError()) return FResult::Error(MoveTemp(FrontEnd).GetError());
	State->VerifiedFrontEndDescriptor = MakeShared<const FWoTFrontEndDescriptor, ESPMode::ThreadSafe>(MoveTemp(FrontEnd).GetValue());
	State->bHasFrontEndDescriptorAttestation = true;
	return FResult::Success();
}
