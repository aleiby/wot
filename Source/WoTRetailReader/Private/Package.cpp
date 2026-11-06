#include "WoTRetailReader/Package.h"

#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"
#include "WoTBootstrapCore/Hash.h"

namespace
{
bool WoTRetailPackageReservedName(const FString& Component)
{
	static const TCHAR* const Names[] = {
		TEXT("con"), TEXT("prn"), TEXT("aux"), TEXT("nul"),
		TEXT("com1"), TEXT("com2"), TEXT("com3"), TEXT("com4"), TEXT("com5"),
		TEXT("com6"), TEXT("com7"), TEXT("com8"), TEXT("com9"),
		TEXT("lpt1"), TEXT("lpt2"), TEXT("lpt3"), TEXT("lpt4"), TEXT("lpt5"),
		TEXT("lpt6"), TEXT("lpt7"), TEXT("lpt8"), TEXT("lpt9"),
	};
	FString Stem = Component;
	int32 Dot = INDEX_NONE;
	if (Stem.FindChar(TEXT('.'), Dot))
	{
		Stem.LeftInline(Dot);
	}
	for (const TCHAR* Name : Names)
	{
		if (Stem.Equals(Name, ESearchCase::IgnoreCase))
		{
			return true;
		}
	}
	return false;
}

TWoTBootstrapResult<FString> WoTRetailPackageNormalizePath(
	FStringView RawPath, uint16 MaximumDepth)
{
	using FResult = TWoTBootstrapResult<FString>;
	FString Path(RawPath);
	Path.ReplaceInline(TEXT("\\"), TEXT("/"), ESearchCase::CaseSensitive);
	if (Path.IsEmpty() || Path.StartsWith(TEXT("/"), ESearchCase::CaseSensitive)
		|| Path.EndsWith(TEXT("/"), ESearchCase::CaseSensitive)
		|| Path.Contains(TEXT("//"), ESearchCase::CaseSensitive)
		|| Path.Contains(TEXT(":"), ESearchCase::CaseSensitive)
		|| Path.Contains(TEXT("{"), ESearchCase::CaseSensitive)
		|| Path.Contains(TEXT("}"), ESearchCase::CaseSensitive))
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::UnsafePath,
			TEXT("the Inno entry is not a safe relative logical path"), Path));
	}

	TArray<FString> Components;
	Path.ParseIntoArray(Components, TEXT("/"), false);
	if (Components.Num() == 0 || Components.Num() > static_cast<int32>(MaximumDepth))
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::UnsafePath,
			TEXT("the Inno entry exceeds the logical path depth limit"), Path));
	}
	for (const FString& Component : Components)
	{
		if (Component.IsEmpty() || Component == TEXT(".") || Component == TEXT("..")
			|| Component.EndsWith(TEXT("."), ESearchCase::CaseSensitive)
			|| Component.EndsWith(TEXT(" "), ESearchCase::CaseSensitive)
			|| WoTRetailPackageReservedName(Component))
		{
			return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::UnsafePath,
				TEXT("the Inno entry has an unsafe path component"), Path));
		}
		for (const TCHAR Character : Component)
		{
			// The retail contract uses portable ASCII package identities. Refusing
			// platform-dependent normalization keeps lookup identical on Mac/Win64.
			if (Character < 0x20 || Character > 0x7e
				|| Character == TEXT('<') || Character == TEXT('>')
				|| Character == TEXT(':') || Character == TEXT('"')
				|| Character == TEXT('|') || Character == TEXT('?')
				|| Character == TEXT('*'))
			{
				return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::UnsafePath,
					TEXT("the Inno entry path is outside portable ASCII"), Path));
			}
		}
	}
	return FResult::Success(MoveTemp(Path));
}

FWoTBootstrapError WoTRetailPackageChangedError(const TCHAR* Message)
{
	return MakeWoTBootstrapError(EWoTBootstrapError::CorruptInstallation, Message,
		TEXT("held-installer-snapshot"));
}

bool WoTRetailPackageEntryMatches(const FWoTPackageEntry& Left,
	const FWoTPackageEntry& Right)
{
	return Left.AdapterIndex == Right.AdapterIndex
		&& Left.LogicalPath.Equals(Right.LogicalPath, ESearchCase::CaseSensitive)
		&& Left.Size == Right.Size
		&& Left.ChecksumType == Right.ChecksumType
		&& Left.Checksum == Right.Checksum
		&& Left.Compression == Right.Compression;
}

class FWoTRetailDecodedEntry final : public IWoTByteSink
{
public:
	explicit FWoTRetailDecodedEntry(uint64 InLimit = 0)
		: Limit(InLimit)
	{
	}

	FWoTBootstrapStatus Consume(TConstArrayView<uint8> Bytes) override
	{
		const uint64 Count = static_cast<uint64>(Bytes.Num());
		if (Written > Limit || Count > Limit - Written)
		{
			return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::SizeLimit,
				TEXT("decoded entry exceeded its quarantine limit")));
		}
		TArray<uint8>& Chunk = Chunks.AddDefaulted_GetRef();
		Chunk.Append(Bytes.GetData(), Bytes.Num());
		Written += Count;
		return FWoTBootstrapStatus::Success();
	}

	FWoTBootstrapStatus Publish(IWoTByteSink& Destination) const
	{
		for (const TArray<uint8>& Chunk : Chunks)
		{
			FWoTBootstrapStatus Status = Destination.Consume(Chunk);
			if (Status.IsError())
			{
				return Status;
			}
		}
		return FWoTBootstrapStatus::Success();
	}

	void Adopt(uint32 InAdapterIndex, FWoTRetailDecodedEntry&& Decoded)
	{
		AdapterIndex = InAdapterIndex;
		Written = Decoded.Written;
		Limit = Decoded.Limit;
		Chunks = MoveTemp(Decoded.Chunks);
	}

	void Reset()
	{
		AdapterIndex.Reset();
		Written = 0;
		Chunks.Reset();
	}

	bool Matches(uint32 InAdapterIndex) const
	{
		return AdapterIndex.IsSet() && AdapterIndex.GetValue() == InAdapterIndex;
	}

	uint64 Num() const
	{
		return Written;
	}

private:
	TOptional<uint32> AdapterIndex;
	uint64 Limit = 0;
	uint64 Written = 0;
	TArray<TArray<uint8>> Chunks;
};
}  // namespace

struct FWoTRetailPackage::FImpl
{
	explicit FImpl(FWoTHeldInstallerSnapshot&& InSnapshot)
		: Snapshot(MoveTemp(InSnapshot))
	{
	}

	FWoTHeldInstallerSnapshot Snapshot;
	FWoTSha256 FrozenDigest;
	FWoTNativeFileIdentity FrozenIdentity;
	uint64 FrozenSize = 0;
	FString FrozenAdapterDescription;
	TUniquePtr<FWoTInnoExtractor> Extractor;
	TArray<FWoTPackageEntry> Entries;
	uint64 MaxEntryBytes = 0;
	mutable FCriticalSection EntryBufferMutex;
	mutable FWoTRetailDecodedEntry EntryBuffer;
	mutable uint64 EntryBufferHighWater = 0;
	mutable FCriticalSection HeldSnapshotVerificationMutex;
	mutable TOptional<FWoTNativeFileIdentity> LastVerifiedIdentity;
#if WITH_AUTOMATION_TESTS
	FWoTRetailPackage::FBeforeReverifyForTesting BeforeReverifyForTesting = nullptr;
	void* BeforeReverifyContext = nullptr;
	mutable uint64 EntryDecodeCount = 0;
#endif
};

FWoTRetailPackage::FWoTRetailPackage() = default;
FWoTRetailPackage::FWoTRetailPackage(TUniquePtr<FImpl>&& InImpl)
	: Impl(MoveTemp(InImpl))
{
}
FWoTRetailPackage::~FWoTRetailPackage() = default;
FWoTRetailPackage::FWoTRetailPackage(FWoTRetailPackage&& Other) = default;
FWoTRetailPackage& FWoTRetailPackage::operator=(FWoTRetailPackage&& Other) = default;

TWoTBootstrapResult<FWoTRetailPackage> FWoTRetailPackage::Open(
	FWoTHeldInstallerSnapshot&& Snapshot, const FWoTArchiveLimits& Limits)
{
	using FResult = TWoTBootstrapResult<FWoTRetailPackage>;
	if (!Snapshot.Handle.IsValid())
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Io,
			TEXT("the installer snapshot has no held native handle")));
	}
	if (Snapshot.Identity != Snapshot.Handle.GetIdentity()
		|| Snapshot.Size != Snapshot.Identity.Size)
	{
		return FResult::Error(WoTRetailPackageChangedError(
			TEXT("snapshot metadata does not describe its held handle")));
	}
	if (Snapshot.Identity.Type != EWoTTreeEntryType::File
		|| Snapshot.Identity.LinkCount != 1)
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::UnsafePath,
			TEXT("the installer must be one regular, singly-linked held file")));
	}
	if (Snapshot.Size == 0
		|| Snapshot.Size > static_cast<uint64>(TNumericLimits<int32>::Max()))
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::SizeLimit,
			TEXT("the held installer cannot be represented by the bounded adapter")));
	}

	TWoTBootstrapResult<FWoTNativeFileIdentity> Before =
		Snapshot.Handle.QueryCurrentIdentity();
	if (Before.IsError())
	{
		return FResult::Error(MoveTemp(Before).GetError());
	}
	if (Before.GetValue() != Snapshot.Identity)
	{
		return FResult::Error(WoTRetailPackageChangedError(
			TEXT("the held installer changed before package parsing")));
	}

	TWoTBootstrapResult<TArray<uint8>> Read = Snapshot.Handle.ReadAll();
	if (Read.IsError())
	{
		return FResult::Error(MoveTemp(Read).GetError());
	}
	TWoTBootstrapResult<FWoTNativeFileIdentity> After =
		Snapshot.Handle.QueryCurrentIdentity();
	if (After.IsError())
	{
		return FResult::Error(MoveTemp(After).GetError());
	}
	if (After.GetValue() != Snapshot.Identity)
	{
		return FResult::Error(WoTRetailPackageChangedError(
			TEXT("the held installer changed while package bytes were read")));
	}

	TUniquePtr<FImpl> NewImpl = MakeUnique<FImpl>(MoveTemp(Snapshot));
	TWoTBootstrapResult<TUniquePtr<FWoTInnoExtractor>> OpenedExtractor =
		FWoTInnoExtractor::OpenWithApprovedDigest(
			MoveTemp(Read).GetValue(), NewImpl->Snapshot.Sha256);
	if (OpenedExtractor.IsError())
	{
		return FResult::Error(MoveTemp(OpenedExtractor).GetError());
	}
	TWoTBootstrapResult<FWoTNativeFileIdentity> ParsedIdentity =
		NewImpl->Snapshot.Handle.QueryCurrentIdentity();
	if (ParsedIdentity.IsError())
	{
		return FResult::Error(MoveTemp(ParsedIdentity).GetError());
	}
	if (ParsedIdentity.GetValue() != NewImpl->Snapshot.Identity)
	{
		return FResult::Error(WoTRetailPackageChangedError(
			TEXT("the held installer changed while the adapter parsed it")));
	}
	NewImpl->FrozenDigest = NewImpl->Snapshot.Sha256;
	NewImpl->FrozenIdentity = NewImpl->Snapshot.Identity;
	NewImpl->FrozenSize = NewImpl->Snapshot.Size;
	NewImpl->Extractor = MoveTemp(OpenedExtractor).GetValue();
	NewImpl->LastVerifiedIdentity = ParsedIdentity.GetValue();
	if (NewImpl->Extractor->CachedInputSha256() != NewImpl->FrozenDigest)
	{
		return FResult::Error(WoTRetailPackageChangedError(
			TEXT("the adapter callback cache differs from the approved installer bytes")));
	}
	NewImpl->FrozenAdapterDescription = NewImpl->Extractor->Description();
	NewImpl->MaxEntryBytes = Limits.MaxEntryBytes;

	const TArray<FWoTInnoEntryDescription>& NativeEntries = NewImpl->Extractor->Entries();
	if (NativeEntries.Num() == 0
		|| static_cast<uint64>(NativeEntries.Num()) > Limits.MaxEntries)
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::SizeLimit,
			TEXT("the Inno package exceeds the entry-count limit")));
	}
	TSet<FString> SeenExact;
	TSet<FString> SeenFolded;
	uint64 TotalSize = 0;
	for (const FWoTInnoEntryDescription& NativeEntry : NativeEntries)
	{
		TWoTBootstrapResult<FString> Normalized = WoTRetailPackageNormalizePath(
			NativeEntry.Name, Limits.MaxDepth);
		if (Normalized.IsError())
		{
			return FResult::Error(MoveTemp(Normalized).GetError());
		}
		const FString Folded = Normalized.GetValue().ToLower();
		if (SeenExact.Contains(Normalized.GetValue()) || SeenFolded.Contains(Folded))
		{
			return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::DuplicateField,
				TEXT("two Inno entries normalize to the same logical path"),
				Normalized.GetValue()));
		}
		SeenExact.Add(Normalized.GetValue());
		SeenFolded.Add(Folded);

		if (NativeEntry.Size > Limits.MaxEntryBytes
			|| NativeEntry.Size > Limits.MaxTotalBytes
			|| TotalSize > Limits.MaxTotalBytes - NativeEntry.Size)
		{
			return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::SizeLimit,
				TEXT("the Inno package exceeds its decompression limits"),
				Normalized.GetValue()));
		}
		TotalSize += NativeEntry.Size;

		FWoTPackageEntry Entry;
		Entry.AdapterIndex = NativeEntry.AdapterIndex;
		Entry.LogicalPath = MoveTemp(Normalized).GetValue();
		Entry.Size = NativeEntry.Size;
		Entry.ChecksumType = NativeEntry.ChecksumType;
		Entry.Checksum = NativeEntry.Checksum;
		Entry.Compression = NativeEntry.Compression;
		NewImpl->Entries.Add(MoveTemp(Entry));
	}

	return FResult::Success(FWoTRetailPackage(MoveTemp(NewImpl)));
}

TWoTBootstrapResult<FWoTPackageEntry> FWoTRetailPackage::FindExact(
	FStringView LogicalPath) const
{
	using FResult = TWoTBootstrapResult<FWoTPackageEntry>;
	if (!Impl.IsValid())
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Io,
			TEXT("the retail package is not open")));
	}
	TWoTBootstrapResult<FString> Normalized = WoTRetailPackageNormalizePath(LogicalPath, 65535);
	if (Normalized.IsError())
	{
		return FResult::Error(MoveTemp(Normalized).GetError());
	}
	if (!Normalized.GetValue().Equals(FString(LogicalPath), ESearchCase::CaseSensitive))
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::UnsafePath,
			TEXT("lookup requires the canonical slash spelling"), FString(LogicalPath)));
	}
	for (const FWoTPackageEntry& Entry : Impl->Entries)
	{
		if (Entry.LogicalPath.Equals(Normalized.GetValue(), ESearchCase::CaseSensitive))
		{
			return FResult::Success(Entry);
		}
	}
	return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::UnknownField,
		TEXT("the retail package has no exact logical-path match"), FString(LogicalPath)));
}

FWoTBootstrapStatus FWoTRetailPackage::StreamExact(const FWoTPackageEntry& Entry,
	IWoTByteSink& Sink) const
{
	if (!Impl.IsValid() || !Impl->Entries.IsValidIndex(static_cast<int32>(Entry.AdapterIndex))
		|| !WoTRetailPackageEntryMatches(
			Impl->Entries[static_cast<int32>(Entry.AdapterIndex)], Entry))
	{
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::UnknownField,
			TEXT("the requested entry is not an exact member of this held package"),
			Entry.LogicalPath));
	}
	FWoTBootstrapStatus BeforeUse = ReverifyHeldSnapshotAfterExtraction();
	if (BeforeUse.IsError())
	{
		return BeforeUse;
	}
	// One package is consumed as one ordered pass. Retaining only the current
	// decoded entry lets all consecutive facts share that work while the native
	// adapter's cursor continues forward when the next entry is requested.
	// The lock keeps a sink callback from observing a buffer while another
	// caller advances and drops it.
	FScopeLock BufferLock(&Impl->EntryBufferMutex);
	if (!Impl->EntryBuffer.Matches(Entry.AdapterIndex))
	{
		// Drop before decoding the next entry, so resident decoded storage is
		// always O(largest entry), never current plus next.
		Impl->EntryBuffer.Reset();
		FWoTRetailDecodedEntry Decoded(Impl->MaxEntryBytes);
		FWoTBootstrapStatus Extracted = Impl->Extractor->Extract(
			Entry.AdapterIndex, Entry.Size, Decoded);
		if (Extracted.IsError())
		{
			return Extracted;
		}
		if (Decoded.Num() != Entry.Size)
		{
			return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("decoded entry quarantine disagrees with its declared size"),
				Entry.LogicalPath));
		}
#if WITH_AUTOMATION_TESTS
		++Impl->EntryDecodeCount;
#endif
		Impl->EntryBuffer.Adopt(Entry.AdapterIndex, MoveTemp(Decoded));
		Impl->EntryBufferHighWater =
			FMath::Max(Impl->EntryBufferHighWater, Impl->EntryBuffer.Num());
	}
#if WITH_AUTOMATION_TESTS
	if (Impl->BeforeReverifyForTesting != nullptr)
	{
		Impl->BeforeReverifyForTesting(Impl->BeforeReverifyContext);
	}
#endif
	FWoTBootstrapStatus Reverified = ReverifyHeldSnapshotAfterExtraction();
	if (Reverified.IsError())
	{
		// A failed held-snapshot check poisons even a previously verified cached
		// entry. No bytes from it may be published after source mutation.
		Impl->EntryBuffer.Reset();
		return Reverified;
	}
	return Impl->EntryBuffer.Publish(Sink);
}

FWoTSha256 FWoTRetailPackage::CachedInputSha256() const
{
	return Impl.IsValid() ? Impl->Extractor->CachedInputSha256() : FWoTSha256();
}

uint64 FWoTRetailPackage::EntryBufferHighWaterBytes() const
{
	if (!Impl.IsValid())
	{
		return 0;
	}
	FScopeLock BufferLock(&Impl->EntryBufferMutex);
	return Impl->EntryBufferHighWater;
}

#if WITH_AUTOMATION_TESTS
void FWoTRetailPackage::SetBeforeReverifyForTesting(
	FBeforeReverifyForTesting Callback, void* Context)
{
	check(Impl.IsValid());
	Impl->BeforeReverifyForTesting = Callback;
	Impl->BeforeReverifyContext = Context;
}

uint64 FWoTRetailPackage::EntryDecodeCountForTesting() const
{
	if (!Impl.IsValid())
	{
		return 0;
	}
	FScopeLock BufferLock(&Impl->EntryBufferMutex);
	return Impl->EntryDecodeCount;
}
#endif

FWoTBootstrapStatus FWoTRetailPackage::ReverifyHeldSnapshotAfterExtraction() const
{
	if (!Impl.IsValid() || !Impl->Snapshot.Handle.IsValid())
	{
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(EWoTBootstrapError::Io,
			TEXT("the retail package no longer holds its installer snapshot")));
	}
	FScopeLock VerificationLock(&Impl->HeldSnapshotVerificationMutex);
	const auto IdentityIsAcceptable = [this](const FWoTNativeFileIdentity& Identity)
	{
		// LinkCount zero is the expected held-object state after the adjacent name is
		// atomically replaced. More than one means an unsafe second name exists.
		return Impl->FrozenIdentity.IsSameObject(Identity)
			&& Identity.Type == EWoTTreeEntryType::File
			&& Identity.Size == Impl->FrozenSize
			&& Identity.LinkCount <= 1;
	};

	TWoTBootstrapResult<FWoTNativeFileIdentity> Before =
		Impl->Snapshot.Handle.QueryCurrentIdentity();
	if (Before.IsError())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(Before).GetError());
	}
	if (!IdentityIsAcceptable(Before.GetValue()))
	{
		return FWoTBootstrapStatus::Error(WoTRetailPackageChangedError(
			TEXT("the held installer identity, size, type, or link state changed")));
	}
	// ChangeState is the OS change time (not a caller-settable modification
	// timestamp) and strict identity also includes link count. Once acquisition
	// authenticated this held object, only exact identity continuity may reuse
	// that digest. A mutation fails closed instead of starting another full pass.
	if (Impl->LastVerifiedIdentity.IsSet()
		&& Before.GetValue() == Impl->LastVerifiedIdentity.GetValue()
		&& Impl->Extractor->Description() == Impl->FrozenAdapterDescription)
	{
		return FWoTBootstrapStatus::Success();
	}
	return FWoTBootstrapStatus::Error(WoTRetailPackageChangedError(
		TEXT("the held installer changed after its authenticated snapshot")));
}
