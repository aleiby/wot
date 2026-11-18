#include "WoTCacheStore.h"
#include "Containers/StringConv.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"
#include "WoTBootstrapCore/CanonicalJson.h"
#include "WoTBootstrapCore/Hash.h"
#include "WoTBootstrapCore/NativeHandles.h"
#include "WoTRetailReader/ProductionContract.h"
#include "WoTRetailReader/WorldDescriptorSidecar.h"
#include "WoTRetailReader/FrontEndDescriptorSidecar.h"

namespace
{
constexpr uint64 WoTCacheMaximumManifestBytes = 16ull * 1024ull * 1024ull;
template<typename T> TWoTBootstrapResult<T> WoTCacheStoreError(EWoTBootstrapError Code,
	const TCHAR* Message, const FString& Path = FString())
{
	return TWoTBootstrapResult<T>::Error(MakeWoTBootstrapError(Code, Message, Path));
}
FWoTBootstrapStatus WoTCacheStoreStatus(EWoTBootstrapError Code,
	const TCHAR* Message, const FString& Path = FString())
{
	return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(Code, Message, Path));
}
FString WoTCacheBlobLeaf(FStringView LogicalId)
{
	const FTCHARToUTF8 Utf8(LogicalId.GetData(), LogicalId.Len());
	return Sha256(TConstArrayView<uint8>(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length())).ToHex() + TEXT(".blob");
}
bool WoTCacheContentError(const FWoTBootstrapError& Error)
{
	return Error.Code != EWoTBootstrapError::Io
		&& Error.Code != EWoTBootstrapError::Durability
		&& Error.Code != EWoTBootstrapError::ServiceUnavailable;
}
FWoTBootstrapStatus WoTCacheRequireExclusiveSession(
	const FWoTProfileSession& Session, const FString& CacheRoot,
	const FWoTNativeDirectoryHandle& CacheParent)
{
	FString ExpectedRoot = FPaths::GetPath(CacheRoot);
	FPaths::NormalizeDirectoryName(ExpectedRoot);
	if (!Session.IsExclusive() || Session.DataRoot() != ExpectedRoot
		|| !Session.DataRootIdentity().IsSameObject(
			CacheParent.GetIdentity())
		|| !FPaths::GetCleanFilename(CacheRoot).Equals(
			TEXT("ContentCache"), ESearchCase::CaseSensitive))
	{
		return WoTCacheStoreStatus(EWoTBootstrapError::UnsafePath,
			TEXT("cache mutation requires the held exclusive profile session"),
			CacheRoot);
	}
	return FWoTBootstrapStatus::Success();
}
FWoTBootstrapStatus WoTCacheRemoveContents(const FWoTNativeDirectoryHandle& Directory)
{
	auto Entries = EnumerateNativeDirectory(Directory);
	if (Entries.IsError()) return FWoTBootstrapStatus::Error(MoveTemp(Entries).GetError());
	for (const auto& Entry : Entries.GetValue())
	{
		if (Entry.Type == EWoTTreeEntryType::Directory)
		{
			auto Child = OpenNativeDirectoryAtNoFollow(Directory, Entry.Name);
			if (Child.IsError()) return FWoTBootstrapStatus::Error(MoveTemp(Child).GetError());
			auto Removed = WoTCacheRemoveContents(Child.GetValue());
			if (Removed.IsError()) return Removed;
		}
		auto Removed = RemoveNativeEntryAtNoFollow(Directory, Entry.Name, Entry.Type);
		if (Removed.IsError()) return Removed;
	}
	return FWoTBootstrapStatus::Success();
}
TWoTBootstrapResult<TArray<FWoTCacheLeaseManifestEntry>> WoTCacheParseManifestTable(
	TConstArrayView<uint8> Bytes,
	FWoTCacheGenerationBuildIdentity& OutBuildIdentity,
	TArray<FWoTWorldDescriptor>& OutWorldDescriptors, FWoTSha256& OutFrontEndRoot)
{
	using FResult = TWoTBootstrapResult<TArray<FWoTCacheLeaseManifestEntry>>;
	OutBuildIdentity = {};
	FWoTCanonicalJsonLimits Limits;
	Limits.MaxBytes = static_cast<uint32>(WoTCacheMaximumManifestBytes);
	Limits.MaxMembers = 100000;
	TWoTBootstrapResult<FWoTCanonicalJsonValue> Parsed = ParseCanonicalJson(Bytes, Limits);
	if (Parsed.IsError())
	{
		return FResult::Error(MoveTemp(Parsed).GetError());
	}
	const FWoTCanonicalJsonValue& Root = Parsed.GetValue();
	static const TCHAR* const BaseMembers[] = {
		TEXT("blobs"), TEXT("cache_version"), TEXT("format"), TEXT("hash_algorithm")};
	static const TCHAR* const BuildMembers[] = {
		TEXT("blobs"), TEXT("cache_version"), TEXT("contract_sha256"), TEXT("format"),
		TEXT("hash_algorithm"), TEXT("installer_sha256"),
		TEXT("production_descriptor_sha256"), TEXT("world_descriptors"), TEXT("front_end_descriptor_root")};
	const bool bHasBuildIdentity = Root.FindMember(TEXT("contract_sha256")) != nullptr
		|| Root.FindMember(TEXT("installer_sha256")) != nullptr
		|| Root.FindMember(TEXT("production_descriptor_sha256")) != nullptr;
	TOptional<FWoTBootstrapError> RootSchemaError = bHasBuildIdentity
		? RequireExactMembers(Root, BuildMembers, TEXT("cache manifest root"))
		: RequireExactMembers(Root, BaseMembers, TEXT("cache manifest root"));
	if (RootSchemaError.IsSet())
	{
		return FResult::Error(MoveTemp(RootSchemaError.GetValue()));
	}
	FString Format;
	FString HashAlgorithm;
	uint32 Version = 0;
	if (TOptional<FWoTBootstrapError> Error =
			ReadStringField(Root, TEXT("format"), TEXT("cache manifest"), Format);
		Error.IsSet())
	{
		return FResult::Error(MoveTemp(Error.GetValue()));
	}
	if (TOptional<FWoTBootstrapError> Error = ReadStringField(
			Root, TEXT("hash_algorithm"), TEXT("cache manifest"), HashAlgorithm);
		Error.IsSet())
	{
		return FResult::Error(MoveTemp(Error.GetValue()));
	}
	if (TOptional<FWoTBootstrapError> Error = ReadUnsigned32Field(
			Root, TEXT("cache_version"), TEXT("cache manifest"), Version);
		Error.IsSet())
	{
		return FResult::Error(MoveTemp(Error.GetValue()));
	}
	if (Format != TEXT("wot-payload-cache") || HashAlgorithm != TEXT("sha256")
		|| Version != 3)
	{
		return WoTCacheStoreError<TArray<FWoTCacheLeaseManifestEntry>>(
			EWoTBootstrapError::UnsupportedProtocol,
			TEXT("the cache manifest protocol identity is unsupported"));
	}
	if (bHasBuildIdentity)
	{
		auto ReadBuildDigest = [&Root](const TCHAR* Field,
			FWoTSha256& OutDigest) -> TOptional<FWoTBootstrapError>
		{
			FString Hex;
			if (TOptional<FWoTBootstrapError> Error = ReadStringField(
					Root, Field, TEXT("cache manifest"), Hex); Error.IsSet())
			{
				return Error;
			}
			TWoTBootstrapResult<FWoTSha256> Parsed = FWoTSha256::FromHex(Hex);
			if (Parsed.IsError())
			{
				return MakeWoTBootstrapError(EWoTBootstrapError::Crypto,
					TEXT("a cache manifest build digest is invalid"), Field);
			}
			OutDigest = MoveTemp(Parsed).GetValue();
			return {};
		};
		FWoTCacheGenerationBuildIdentity ParsedIdentity;
		if (TOptional<FWoTBootstrapError> Error = ReadBuildDigest(
				TEXT("installer_sha256"), ParsedIdentity.InstallerSha256);
			Error.IsSet())
		{
			return FResult::Error(MoveTemp(Error.GetValue()));
		}
		if (TOptional<FWoTBootstrapError> Error = ReadBuildDigest(
				TEXT("contract_sha256"), ParsedIdentity.ContractSha256);
			Error.IsSet())
		{
			return FResult::Error(MoveTemp(Error.GetValue()));
		}
		if (TOptional<FWoTBootstrapError> Error = ReadBuildDigest(
				TEXT("production_descriptor_sha256"),
				ParsedIdentity.ProductionDescriptorSha256); Error.IsSet())
		{
			return FResult::Error(MoveTemp(Error.GetValue()));
		}
		ParsedIdentity.bHasBuildIdentity = true;
		OutBuildIdentity = ParsedIdentity;
		FString FrontEndHex;
		if (auto Error = ReadStringField(Root, TEXT("front_end_descriptor_root"), TEXT("cache manifest"), FrontEndHex); Error.IsSet())
			return FResult::Error(MoveTemp(Error.GetValue()));
		auto FrontEndRoot = FWoTSha256::FromHex(FrontEndHex);
		if (FrontEndRoot.IsError()) return FResult::Error(MoveTemp(FrontEndRoot).GetError());
		OutFrontEndRoot = FrontEndRoot.GetValue();
		const auto* Worlds = Root.FindMember(TEXT("world_descriptors"));
		if (Worlds == nullptr) return WoTCacheStoreError<TArray<FWoTCacheLeaseManifestEntry>>(
			EWoTBootstrapError::CorruptInstallation, TEXT("the cache omits world descriptors"));
		auto ParsedWorlds = ParseCacheWorldDescriptors(*Worlds);
		if (ParsedWorlds.IsError()) return FResult::Error(MoveTemp(ParsedWorlds).GetError());
		OutWorldDescriptors = MoveTemp(ParsedWorlds).GetValue();
	}
	const FWoTCanonicalJsonValue* Blobs = Root.FindMember(TEXT("blobs"));
	if (Blobs == nullptr || !Blobs->IsArray() || Blobs->Num() == 0)
	{
		return WoTCacheStoreError<TArray<FWoTCacheLeaseManifestEntry>>(
			EWoTBootstrapError::UnknownField,
			TEXT("the cache manifest blobs field is not a non-empty array"));
	}
	static const TCHAR* const BlobMembers[] = {
		TEXT("logical_id"), TEXT("satisfies"), TEXT("schema"), TEXT("sha256"), TEXT("size")};
	TArray<FWoTCacheLeaseManifestEntry> Entries;
	Entries.Reserve(Blobs->Num());
	FString PreviousLogicalId;
	for (int32 Index = 0; Index < Blobs->Num(); ++Index)
	{
		const FWoTCanonicalJsonValue& Record = Blobs->GetElement(Index);
		if (TOptional<FWoTBootstrapError> Error = RequireExactMembers(
				Record, BlobMembers, TEXT("cache manifest blob"));
			Error.IsSet())
		{
			return FResult::Error(MoveTemp(Error.GetValue()));
		}
		FWoTCacheLeaseManifestEntry& Entry = Entries.AddDefaulted_GetRef();
		FString ShaHex;
		if (TOptional<FWoTBootstrapError> Error = ReadStringField(Record,
				TEXT("logical_id"), TEXT("cache manifest blob"), Entry.LogicalId);
			Error.IsSet())
		{
			return FResult::Error(MoveTemp(Error.GetValue()));
		}
		if (TOptional<FWoTBootstrapError> Error = ReadStringField(Record,
				TEXT("schema"), TEXT("cache manifest blob"), Entry.Schema); Error.IsSet())
		{
			return FResult::Error(MoveTemp(Error.GetValue()));
		}
		if (TOptional<FWoTBootstrapError> Error = ReadStringField(Record,
				TEXT("sha256"), TEXT("cache manifest blob"), ShaHex); Error.IsSet())
		{
			return FResult::Error(MoveTemp(Error.GetValue()));
		}
		if (TOptional<FWoTBootstrapError> Error = ReadUnsignedField(Record,
				TEXT("size"), TEXT("cache manifest blob"), Entry.Size); Error.IsSet())
		{
			return FResult::Error(MoveTemp(Error.GetValue()));
		}
		TWoTBootstrapResult<FWoTSha256> Sha = FWoTSha256::FromHex(ShaHex);
		const FWoTCanonicalJsonValue* Satisfies = Record.FindMember(TEXT("satisfies"));
		if (Entry.LogicalId.IsEmpty() || Entry.LogicalId.Len() > 4096
			|| Entry.Schema.IsEmpty() || Sha.IsError()
			|| Satisfies == nullptr || !Satisfies->IsArray() || Satisfies->Num() == 0
			|| (!PreviousLogicalId.IsEmpty() && Entry.LogicalId <= PreviousLogicalId))
		{
			return WoTCacheStoreError<TArray<FWoTCacheLeaseManifestEntry>>(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("a cache manifest blob record is invalid or non-unique"),
				Entry.LogicalId);
		}
		FString PreviousFact;
		for (int32 FactIndex = 0; FactIndex < Satisfies->Num(); ++FactIndex)
		{
			const FWoTCanonicalJsonValue& Fact = Satisfies->GetElement(FactIndex);
			if (Fact.GetKind() != EWoTCanonicalJsonKind::String
				|| Fact.GetString().IsEmpty()
				|| (!PreviousFact.IsEmpty() && Fact.GetString() <= PreviousFact))
			{
				return WoTCacheStoreError<TArray<FWoTCacheLeaseManifestEntry>>(
					EWoTBootstrapError::CorruptInstallation,
					TEXT("a cache manifest satisfies list is invalid"), Entry.LogicalId);
			}
			PreviousFact = Fact.GetString();
			Entry.Satisfies.Add(PreviousFact);
		}
		Entry.Sha256 = MoveTemp(Sha).GetValue();
		PreviousLogicalId = Entry.LogicalId;
	}
	return FResult::Success(MoveTemp(Entries));
}


FWoTBootstrapStatus WoTCacheValidateCoverage(
	const TArray<FWoTCacheLeaseManifestEntry>& Entries,
	const FWoTCacheGenerationBuildIdentity& Identity,
	const TArray<FWoTWorldDescriptor>& Worlds)
{
	if (!Identity.bHasBuildIdentity) return FWoTBootstrapStatus::Success();
	auto Contract = WoTLoadProductionRetailContract();
	if (Contract.IsError()) return FWoTBootstrapStatus::Error(MoveTemp(Contract).GetError());
	if (Identity.ContractSha256 != ComputeRetailContractIdentity(Contract.GetValue()))
		return WoTCacheStoreStatus(EWoTBootstrapError::CorruptInstallation, TEXT("the cache contract is incompatible"));
	TMap<FString, const FWoTCacheLeaseManifestEntry*> ById;
	for (const auto& Entry : Entries) ById.Add(Entry.LogicalId, &Entry);
	TMap<FString, TArray<FString>> ExpectedFacts;
	for (const auto& Fact : Contract.GetValue().Facts)
	{
		for (const auto& Output : Fact.Outputs)
		{
			const auto* const* Found = ById.Find(Output.LogicalId);
			if (Found == nullptr || (*Found)->Schema != Output.Schema || (*Found)->Size > Output.MaxDecodedSize)
				return WoTCacheStoreStatus(EWoTBootstrapError::CorruptInstallation, TEXT("the cache does not cover the required contract output"), Output.LogicalId);
			ExpectedFacts.FindOrAdd(Output.LogicalId).AddUnique(Fact.FactId);
		}
	}
	if (ExpectedFacts.Num() != Entries.Num()) return WoTCacheStoreStatus(
		EWoTBootstrapError::CorruptInstallation, TEXT("the cache contains undeclared outputs"));
	for (auto& Pair : ExpectedFacts)
	{
		Pair.Value.Sort();
		if (Pair.Value != ById.FindChecked(Pair.Key)->Satisfies)
			return WoTCacheStoreStatus(EWoTBootstrapError::CorruptInstallation, TEXT("the cache output has incorrect contract coverage"), Pair.Key);
	}
	auto Associations = ValidateWorldDescriptorContractAssociations(Worlds, Contract.GetValue());
	if (Associations.IsError()) return Associations;
	for (const auto& World : Worlds)
	{
		const auto* const* Entry = ById.Find(World.WorldLogicalId);
		if (Entry == nullptr || (*Entry)->Schema != World.WorldSchema || (*Entry)->Sha256 != World.WorldSha256)
			return WoTCacheStoreStatus(EWoTBootstrapError::CorruptInstallation, TEXT("the world descriptor does not match its manifest"), World.MapIdentity);
		for (const auto& Part : World.BSPParts)
		{
			Entry = ById.Find(Part.LogicalId);
			if (Entry == nullptr || (*Entry)->Schema != Part.Schema || (*Entry)->Sha256 != Part.Sha256)
				return WoTCacheStoreStatus(EWoTBootstrapError::CorruptInstallation, TEXT("the BSP descriptor does not match its manifest"), Part.LogicalId);
		}
	}
	return FWoTBootstrapStatus::Success();
}
}

struct FWoTCacheStoreState
{
	FString CanonicalRoot;
	FWoTNativeDirectoryHandle Parent;
	FWoTNativeDirectoryHandle Root;
	FWoTNativeDirectoryHandle Building;
	FWoTNativeDirectoryHandle Blobs;
	TMap<FString, FWoTCacheLeaseManifestEntry> Written;
	FCriticalSection WriterGuard;
	bool bBuilding = false;
#if WITH_AUTOMATION_TESTS
	bool bSemanticFixture = false;
#endif
};

FWoTCacheStore::FWoTCacheStore(TUniquePtr<FWoTCacheStoreState>&& InState) : State(MoveTemp(InState)) {}
FWoTCacheStore::~FWoTCacheStore() = default;
FWoTCacheStore::FWoTCacheStore(FWoTCacheStore&&) = default;
FWoTCacheStore& FWoTCacheStore::operator=(FWoTCacheStore&&) = default;

TWoTBootstrapResult<FWoTCacheStore> FWoTCacheStore::Open(FString Root)
{
	using FResult = TWoTBootstrapResult<FWoTCacheStore>;
	FPaths::NormalizeDirectoryName(Root);
	if (FPaths::IsRelative(Root) || FPaths::GetCleanFilename(Root).IsEmpty())
		return WoTCacheStoreError<FWoTCacheStore>(EWoTBootstrapError::UnsafePath, TEXT("the cache root must be absolute"), Root);
	auto Parent = OpenCanonicalNativeDirectoryFollowing(FPaths::GetPath(Root));
	if (Parent.IsError()) return FResult::Error(MoveTemp(Parent).GetError());
	auto NewState = MakeUnique<FWoTCacheStoreState>();
	NewState->Parent = MoveTemp(Parent.GetValue().Directory);
	auto ParentPath = QueryNativeDirectoryPath(NewState->Parent);
	if (ParentPath.IsError()) return FResult::Error(MoveTemp(ParentPath).GetError());
	NewState->CanonicalRoot = FPaths::Combine(ParentPath.GetValue(), FPaths::GetCleanFilename(Root));
	return FResult::Success(FWoTCacheStore(MoveTemp(NewState)));
}
#if WITH_AUTOMATION_TESTS
TWoTBootstrapResult<FWoTCacheStore> FWoTCacheStore::ForTesting(FString Root)
{
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Root), true);
	auto Store = Open(MoveTemp(Root));
	if (!Store.IsError()) Store.GetValue().State->bSemanticFixture = true;
	return Store;
}
#endif

TWoTBootstrapResult<TOptional<FWoTCacheGenerationLease>> FWoTCacheStore::TryOpenCurrent(
	const FWoTCacheGenerationBuildIdentity& Expected, IWoTProgressSink* Progress) const
{
	using FResult = TWoTBootstrapResult<TOptional<FWoTCacheGenerationLease>>;
	auto Empty = []() { return FResult::Success(TOptional<FWoTCacheGenerationLease>()); };
	auto Failure = [&](FWoTBootstrapError Error) { return WoTCacheContentError(Error) ? Empty() : FResult::Error(MoveTemp(Error)); };
	auto RootType = QueryNativeEntryTypeAtNoFollow(State->Parent, FPaths::GetCleanFilename(State->CanonicalRoot));
	if (RootType.IsError()) return FResult::Error(MoveTemp(RootType).GetError());
	if (!RootType.GetValue().IsSet() || RootType.GetValue().GetValue() != EWoTTreeEntryType::Directory) return Empty();
	auto Root = OpenNativeDirectoryAtNoFollow(State->Parent, FPaths::GetCleanFilename(State->CanonicalRoot));
	if (Root.IsError()) return Failure(MoveTemp(Root).GetError());
	auto CurrentType = QueryNativeEntryTypeAtNoFollow(Root.GetValue(), TEXT("current"));
	if (CurrentType.IsError()) return FResult::Error(MoveTemp(CurrentType).GetError());
	if (!CurrentType.GetValue().IsSet() || CurrentType.GetValue().GetValue() != EWoTTreeEntryType::Directory) return Empty();
	auto Current = OpenNativeDirectoryAtNoFollow(Root.GetValue(), TEXT("current"));
	if (Current.IsError()) return Failure(MoveTemp(Current).GetError());
	auto Damaged = QueryNativeEntryTypeAtNoFollow(Current.GetValue(), TEXT("damaged"));
	if (Damaged.IsError()) return FResult::Error(MoveTemp(Damaged).GetError());
	if (Damaged.GetValue().IsSet()) return Empty();
	auto BlobType = QueryNativeEntryTypeAtNoFollow(Current.GetValue(), TEXT("blobs"));
	if (BlobType.IsError()) return Failure(MoveTemp(BlobType).GetError());
	if (!BlobType.GetValue().IsSet() || BlobType.GetValue().GetValue() != EWoTTreeEntryType::Directory) return Empty();
	auto ManifestType = QueryNativeEntryTypeAtNoFollow(Current.GetValue(), TEXT("manifest.json"));
	if (ManifestType.IsError()) return FResult::Error(MoveTemp(ManifestType).GetError());
	if (!ManifestType.GetValue().IsSet() || ManifestType.GetValue().GetValue() != EWoTTreeEntryType::File) return Empty();
	auto Manifest = OpenNativeFileAtReadNoFollow(Current.GetValue(), TEXT("manifest.json"));
	if (Manifest.IsError()) return Failure(MoveTemp(Manifest).GetError());
	const auto Before = Manifest.GetValue().GetIdentity();
	if (Before.LinkCount != 1 || Before.Size > WoTCacheMaximumManifestBytes) return Empty();
#if WITH_AUTOMATION_TESTS
	WoTObserveCacheSnapshotPhysicalReadForTesting(TEXTVIEW("manifest.json"));
#endif
	auto Bytes = Manifest.GetValue().ReadAll();
	if (Bytes.IsError()) return FResult::Error(MoveTemp(Bytes).GetError());
	auto After = Manifest.GetValue().QueryCurrentIdentity();
	if (After.IsError()) return FResult::Error(MoveTemp(After).GetError());
	if (Before != After.GetValue() || Before.Size != static_cast<uint64>(Bytes.GetValue().Num())) return Empty();
	FWoTCacheGenerationBuildIdentity Identity;
	TArray<FWoTWorldDescriptor> Worlds;
	FWoTSha256 FrontEndRoot;
	auto Entries = WoTCacheParseManifestTable(Bytes.GetValue(), Identity, Worlds, FrontEndRoot);
	if (Entries.IsError()) return Empty();
	if (Expected.bHasBuildIdentity && (!Identity.bHasBuildIdentity
		|| Identity.InstallerSha256 != Expected.InstallerSha256
		|| Identity.ContractSha256 != Expected.ContractSha256
		|| Identity.ProductionDescriptorSha256 != Expected.ProductionDescriptorSha256)) return Empty();
	bool bCheckProduction = true;
#if WITH_AUTOMATION_TESTS
	bCheckProduction = !State->bSemanticFixture;
#endif
	if (bCheckProduction && (!Expected.bHasBuildIdentity || !Identity.bHasBuildIdentity)) return Empty();
	auto Coverage = bCheckProduction ? WoTCacheValidateCoverage(Entries.GetValue(), Identity, Worlds)
		: FWoTBootstrapStatus::Success();
	if (Coverage.IsError()) return Failure(MoveTemp(Coverage).GetError());
	auto Lease = FWoTCacheGenerationLease::Acquire(State->CanonicalRoot, Root.GetValue().GetIdentity(),
		Sha256(Bytes.GetValue()), Identity, MoveTemp(Worlds), FrontEndRoot,
		MoveTemp(Current).GetValue(), MoveTemp(Entries).GetValue());
	if (Lease.IsError()) return Failure(MoveTemp(Lease).GetError());
	if (Identity.bHasBuildIdentity && bCheckProduction)
	{
		auto Verified = Lease.GetValue().VerifyCriticalClosure(Progress);
		if (Verified.IsError()) return Failure(MoveTemp(Verified).GetError());
	}
#if WITH_AUTOMATION_TESTS
	if (State->bSemanticFixture)
	{
		auto Verified = Lease.GetValue().VerifyCriticalClosureForTesting({}, nullptr);
		if (Verified.IsError()) return Failure(MoveTemp(Verified).GetError());
	}
#endif
	return FResult::Success(TOptional<FWoTCacheGenerationLease>(MoveTemp(Lease).GetValue()));
}

FWoTBootstrapStatus FWoTCacheStore::DeleteCache(
	const FWoTProfileSession& Session)
{
	FScopeLock Guard(&State->WriterGuard);
	if (FWoTBootstrapStatus Held = WoTCacheRequireExclusiveSession(
			Session, State->CanonicalRoot, State->Parent); Held.IsError()) return Held;
	if (State->bBuilding) return WoTCacheStoreStatus(
		EWoTBootstrapError::ServiceUnavailable,
		TEXT("the cache is already building"), State->CanonicalRoot);
	const FString Leaf = FPaths::GetCleanFilename(State->CanonicalRoot);
	auto Existing = QueryNativeEntryTypeAtNoFollow(State->Parent, Leaf);
	if (Existing.IsError()) return FWoTBootstrapStatus::Error(MoveTemp(Existing).GetError());
	if (!Existing.GetValue().IsSet()) return FWoTBootstrapStatus::Success();
	if (Existing.GetValue().GetValue() == EWoTTreeEntryType::Directory)
	{
		auto Root = OpenNativeDirectoryAtNoFollow(State->Parent, Leaf);
		if (Root.IsError()) return FWoTBootstrapStatus::Error(MoveTemp(Root).GetError());
		auto Removed = WoTCacheRemoveContents(Root.GetValue());
		if (Removed.IsError()) return Removed;
	}
	return RemoveNativeEntryAtNoFollow(State->Parent, Leaf, Existing.GetValue().GetValue());
}

FWoTBootstrapStatus FWoTCacheStore::BeginBuild(
	const FWoTProfileSession& Session)
{
	FScopeLock Guard(&State->WriterGuard);
	if (FWoTBootstrapStatus Held = WoTCacheRequireExclusiveSession(
			Session, State->CanonicalRoot, State->Parent); Held.IsError()) return Held;
	if (State->bBuilding) return WoTCacheStoreStatus(EWoTBootstrapError::ServiceUnavailable, TEXT("the cache is already building"), State->CanonicalRoot);
	bool bCreated = false;
	auto Root = OpenOrCreateNativeDirectoryAtNoFollow(State->Parent, FPaths::GetCleanFilename(State->CanonicalRoot), bCreated);
	if (Root.IsError()) return FWoTBootstrapStatus::Error(MoveTemp(Root).GetError());
	auto Removed = WoTCacheRemoveContents(Root.GetValue());
	if (Removed.IsError()) return Removed;
	auto Building = CreateNativeDirectoryAtNoFollow(Root.GetValue(), TEXT("building"));
	if (Building.IsError()) return FWoTBootstrapStatus::Error(MoveTemp(Building).GetError());
	auto Blobs = CreateNativeDirectoryAtNoFollow(Building.GetValue(), TEXT("blobs"));
	if (Blobs.IsError()) return FWoTBootstrapStatus::Error(MoveTemp(Blobs).GetError());
	State->Root = MoveTemp(Root).GetValue();
	State->Building = MoveTemp(Building).GetValue();
	State->Blobs = MoveTemp(Blobs).GetValue();
	State->Written.Reset();
	State->bBuilding = true;
	return FWoTBootstrapStatus::Success();
}

FWoTBootstrapStatus FWoTCacheStore::WriteObject(FStringView LogicalId, FStringView Schema,
	TConstArrayView<uint8> Bytes, const FWoTSha256& Digest, uint64 MaximumBytes)
{
	FScopeLock Guard(&State->WriterGuard);
	if (!State->bBuilding || !State->Blobs.IsValid())
		return WoTCacheStoreStatus(EWoTBootstrapError::ServiceUnavailable, TEXT("cache writes require BeginBuild"));
	if (LogicalId.IsEmpty() || LogicalId.Len() > 4096 || Schema.IsEmpty()
		|| static_cast<uint64>(Bytes.Num()) > MaximumBytes || Sha256(Bytes) != Digest)
		return WoTCacheStoreStatus(EWoTBootstrapError::CorruptInstallation, TEXT("the cache output exceeds its bound or has an invalid digest"), FString(LogicalId));
	if (State->Written.Contains(FString(LogicalId))) return WoTCacheStoreStatus(
		EWoTBootstrapError::DuplicateField, TEXT("a cache output was written twice"), FString(LogicalId));
	auto File = CreateNativeFileAtWriteNoFollow(State->Blobs, WoTCacheBlobLeaf(LogicalId));
	if (File.IsError()) return FWoTBootstrapStatus::Error(MoveTemp(File).GetError());
	auto Written = WriteNativeFile(File.GetValue(), Bytes);
	if (Written.IsError()) return Written;
	State->Written.Add(FString(LogicalId), FWoTCacheLeaseManifestEntry{
		FString(LogicalId), FString(Schema), static_cast<uint64>(Bytes.Num()), Digest, {}});
	return FWoTBootstrapStatus::Success();
}

FWoTBootstrapStatus FWoTCacheStore::Publish(const FWoTCacheBuildResult& Build)
{
	FScopeLock Guard(&State->WriterGuard);
	if (!State->bBuilding) return WoTCacheStoreStatus(
		EWoTBootstrapError::ServiceUnavailable, TEXT("cache publication requires BeginBuild"));
	ON_SCOPE_EXIT
	{
		State->Blobs = FWoTNativeDirectoryHandle();
		State->Building = FWoTNativeDirectoryHandle();
		State->Root = FWoTNativeDirectoryHandle();
		State->bBuilding = false;
	};
	auto Canonical = WriteCanonicalCacheManifest(Build.Manifest);
	if (Canonical.IsError()) return FWoTBootstrapStatus::Error(MoveTemp(Canonical).GetError());
	if (Canonical.GetValue() != Build.CanonicalManifest || Sha256(Build.CanonicalManifest) != Build.ManifestSha256
		|| Build.Manifest.Blobs.Num() != State->Written.Num())
		return WoTCacheStoreStatus(EWoTBootstrapError::CorruptInstallation, TEXT("the cache manifest does not describe the completed build"));
	FWoTCacheGenerationBuildIdentity Identity;
	TArray<FWoTWorldDescriptor> Worlds;
	FWoTSha256 FrontEndRoot;
	auto Entries = WoTCacheParseManifestTable(Build.CanonicalManifest, Identity, Worlds, FrontEndRoot);
	if (Entries.IsError()) return FWoTBootstrapStatus::Error(MoveTemp(Entries).GetError());
	bool bCheckProduction = true;
#if WITH_AUTOMATION_TESTS
	bCheckProduction = !State->bSemanticFixture;
#endif
	if (bCheckProduction && !Identity.bHasBuildIdentity) return WoTCacheStoreStatus(
		EWoTBootstrapError::CorruptInstallation, TEXT("a production cache must bind all three build identities"));
	auto Coverage = bCheckProduction ? WoTCacheValidateCoverage(Entries.GetValue(), Identity, Worlds)
		: FWoTBootstrapStatus::Success();
	if (Coverage.IsError()) return Coverage;
	for (const auto& Entry : Entries.GetValue())
	{
		const auto* Written = State->Written.Find(Entry.LogicalId);
		if (Written == nullptr || Written->Schema != Entry.Schema || Written->Size != Entry.Size || Written->Sha256 != Entry.Sha256)
			return WoTCacheStoreStatus(EWoTBootstrapError::CorruptInstallation, TEXT("the manifest output differs from the written object"), Entry.LogicalId);
		auto File = OpenNativeFileAtReadNoFollow(State->Blobs, WoTCacheBlobLeaf(Entry.LogicalId));
		if (File.IsError()) return FWoTBootstrapStatus::Error(MoveTemp(File).GetError());
		if (File.GetValue().GetIdentity().Size != Entry.Size || File.GetValue().GetIdentity().LinkCount != 1)
			return WoTCacheStoreStatus(EWoTBootstrapError::CorruptInstallation, TEXT("the written cache object is incomplete"), Entry.LogicalId);
		if (Entry.LogicalId == TEXT("cache/front_end_world/WheelOfTime"))
		{
			if (Entry.Size > 4ull * 1024ull * 1024ull) return WoTCacheStoreStatus(
				EWoTBootstrapError::SizeLimit, TEXT("the front-end descriptor exceeds its bound"));
			auto Bytes = File.GetValue().ReadAll();
			if (Bytes.IsError()) return FWoTBootstrapStatus::Error(MoveTemp(Bytes).GetError());
			if (Sha256(Bytes.GetValue()) != Entry.Sha256) return WoTCacheStoreStatus(
				EWoTBootstrapError::CorruptInstallation, TEXT("the front-end descriptor was damaged before publication"));
			auto FrontEnd = ParseSealedFrontEndDescriptorSidecar(Bytes.GetValue(), Identity.InstallerSha256,
				Identity.ContractSha256, Identity.ProductionDescriptorSha256, FrontEndRoot);
			if (FrontEnd.IsError()) return FWoTBootstrapStatus::Error(MoveTemp(FrontEnd).GetError());
		}

	}
	{
		auto Manifest = CreateNativeFileAtWriteNoFollow(State->Building, TEXT("manifest.json"));
		if (Manifest.IsError()) return FWoTBootstrapStatus::Error(MoveTemp(Manifest).GetError());
		auto Written = WriteNativeFile(Manifest.GetValue(), Build.CanonicalManifest);
		if (Written.IsError()) return Written;
	}
	State->Blobs = FWoTNativeDirectoryHandle();
	State->Building = FWoTNativeDirectoryHandle();
	return RenameNativeEntryAtNoFollow(State->Root, TEXT("building"), State->Root, TEXT("current"), false);
}
