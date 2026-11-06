#include "WoTRetailReader/InnoExtractor.h"

#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"
#include "WoTBootstrapCore/Hash.h"

#ifndef WOT_HAS_INNOEXTRACT
	#define WOT_HAS_INNOEXTRACT 0
#endif

#if !WOT_HAS_INNOEXTRACT

// No reviewed library is promoted for this platform. The retail installer
// only targets Win64/Mac, so extraction is unavailable here — but the game
// module must still COMPILE and LINK, which is why this is a runtime error
// and not a build failure. Every out-of-line member the header declares is
// defined here; `Open` is the only way to obtain an instance and it always
// fails, so the accessors are unreachable rather than merely empty.
struct FWoTInnoExtractor::FImpl
{
	TArray<FWoTInnoEntryDescription> Entries;
	FString Description;
	FWoTSha256 InputSha256;
};

FWoTInnoExtractor::FWoTInnoExtractor(TUniquePtr<FImpl>&& InImpl)
	: Impl(MoveTemp(InImpl))
{
}

FWoTInnoExtractor::~FWoTInnoExtractor() = default;
FWoTInnoExtractor::FWoTInnoExtractor(FWoTInnoExtractor&& Other) = default;
FWoTInnoExtractor& FWoTInnoExtractor::operator=(FWoTInnoExtractor&& Other) = default;

TWoTBootstrapResult<TUniquePtr<FWoTInnoExtractor>> FWoTInnoExtractor::Open(
	TArray<uint8>&&)
{
	return TWoTBootstrapResult<TUniquePtr<FWoTInnoExtractor>>::Error(
		MakeWoTBootstrapError(EWoTBootstrapError::UnsupportedProtocol,
			TEXT("retail installer extraction is not available on this platform"),
			TEXT("WOT_HAS_INNOEXTRACT=0")));
}

const TArray<FWoTInnoEntryDescription>& FWoTInnoExtractor::Entries() const
{
	return Impl->Entries;
}

const FString& FWoTInnoExtractor::Description() const
{
	return Impl->Description;
}

const FWoTSha256& FWoTInnoExtractor::CachedInputSha256() const
{
	return Impl->InputSha256;
}

FWoTBootstrapStatus FWoTInnoExtractor::Extract(uint32, uint64, IWoTByteSink&) const
{
	return FWoTBootstrapStatus::Error(
		MakeWoTBootstrapError(EWoTBootstrapError::UnsupportedProtocol,
			TEXT("retail installer extraction is not available on this platform"),
			TEXT("WOT_HAS_INNOEXTRACT=0")));
}

#else

THIRD_PARTY_INCLUDES_START
#include "wot/innoextract_adapter.h"
THIRD_PARTY_INCLUDES_END

namespace
{
EWoTBootstrapError WoTInnoAdapterCode(int32 Code)
{
	switch (Code)
	{
	case WOT_INNOEXTRACT_INVALID_ARGUMENT: return EWoTBootstrapError::UnknownField;
	case WOT_INNOEXTRACT_IO_ERROR:         return EWoTBootstrapError::Io;
	case WOT_INNOEXTRACT_LIMIT_ERROR:      return EWoTBootstrapError::SizeLimit;
	case WOT_INNOEXTRACT_WRITE_ERROR:      return EWoTBootstrapError::Io;
	case WOT_INNOEXTRACT_FORMAT_ERROR:     return EWoTBootstrapError::CorruptInstallation;
	default:                               return EWoTBootstrapError::CorruptInstallation;
	}
}

FWoTBootstrapError WoTInnoAdapterError(int32 Code, FString Path = FString())
{
	const ANSICHAR* const Message = wot_innoextract_last_error();
	return MakeWoTBootstrapError(WoTInnoAdapterCode(Code),
		Message != nullptr ? UTF8_TO_TCHAR(Message) : TEXT("innoextract adapter failed"),
		MoveTemp(Path));
}

const TCHAR* WoTInnoChecksumType(wot_innoextract_checksum_type Type)
{
	switch (Type)
	{
	case WOT_INNOEXTRACT_CHECKSUM_NONE:    return TEXT("none");
	case WOT_INNOEXTRACT_CHECKSUM_ADLER32: return TEXT("adler32");
	case WOT_INNOEXTRACT_CHECKSUM_CRC32:   return TEXT("crc32");
	case WOT_INNOEXTRACT_CHECKSUM_MD5:     return TEXT("md5");
	case WOT_INNOEXTRACT_CHECKSUM_SHA1:    return TEXT("sha1");
	}
	return TEXT("unknown");
}

const TCHAR* WoTInnoCompression(wot_innoextract_compression Compression)
{
	switch (Compression)
	{
	case WOT_INNOEXTRACT_COMPRESSION_STORED: return TEXT("stored");
	case WOT_INNOEXTRACT_COMPRESSION_ZLIB:   return TEXT("zlib");
	case WOT_INNOEXTRACT_COMPRESSION_BZIP2:  return TEXT("bzip2");
	case WOT_INNOEXTRACT_COMPRESSION_LZMA1:  return TEXT("lzma1");
	case WOT_INNOEXTRACT_COMPRESSION_LZMA2:  return TEXT("lzma2");
	default:                                 return TEXT("unknown");
	}
}
}  // namespace

struct FWoTInnoExtractor::FImpl
{
	TArray<uint8> FrozenInput;
	FWoTSha256 FrozenInputSha256;
	wot_innoextract_package* Package = nullptr;
	TArray<FWoTInnoEntryDescription> Entries;
	FString Description;
	mutable FCriticalSection ExtractMutex;

	~FImpl()
	{
		wot_innoextract_close(Package);
		Package = nullptr;
	}

	static int64 ReadAt(void* Context, uint64 Offset, void* Output, uint64 ByteCount)
	{
		FImpl* const Self = static_cast<FImpl*>(Context);
		if (Self == nullptr || Output == nullptr)
		{
			return -1;
		}
		if (Offset > static_cast<uint64>(Self->FrozenInput.Num()))
		{
			return -1;
		}
		const uint64 Available = static_cast<uint64>(Self->FrozenInput.Num()) - Offset;
		const uint64 Count = FMath::Min(ByteCount, Available);
		if (Count > static_cast<uint64>(TNumericLimits<int64>::Max()))
		{
			return -1;
		}
		if (Count != 0)
		{
			FMemory::Memcpy(Output, Self->FrozenInput.GetData() + static_cast<int64>(Offset),
				static_cast<SIZE_T>(Count));
		}
		return static_cast<int64>(Count);
	}
};

namespace
{
struct FWoTInnoWriteContext
{
	IWoTByteSink& Sink;
	uint64 ExpectedSize = 0;
	uint64 Written = 0;
	TOptional<FWoTBootstrapError> Failure;
};

int32 WoTInnoWrite(void* Context, const void* Bytes, uint64 ByteCount)
{
	FWoTInnoWriteContext* const Write = static_cast<FWoTInnoWriteContext*>(Context);
	if (Write == nullptr || (Bytes == nullptr && ByteCount != 0)
		|| ByteCount > Write->ExpectedSize - FMath::Min(Write->Written, Write->ExpectedSize)
		|| ByteCount > static_cast<uint64>(TNumericLimits<int32>::Max()))
	{
		return 1;
	}
	FWoTBootstrapStatus Status = Write->Sink.Consume(TConstArrayView<uint8>(
		static_cast<const uint8*>(Bytes), static_cast<int32>(ByteCount)));
	if (Status.IsError())
	{
		Write->Failure = MoveTemp(Status).GetError();
		return 1;
	}
	Write->Written += ByteCount;
	return 0;
}
}  // namespace

FWoTInnoExtractor::FWoTInnoExtractor(TUniquePtr<FImpl>&& InImpl)
	: Impl(MoveTemp(InImpl))
{
}

FWoTInnoExtractor::~FWoTInnoExtractor() = default;
FWoTInnoExtractor::FWoTInnoExtractor(FWoTInnoExtractor&& Other) = default;
FWoTInnoExtractor& FWoTInnoExtractor::operator=(FWoTInnoExtractor&& Other) = default;

TWoTBootstrapResult<TUniquePtr<FWoTInnoExtractor>> FWoTInnoExtractor::Open(
	TArray<uint8>&& ApprovedInput)

{
	const FWoTSha256 ApprovedSha256 = Sha256(ApprovedInput);
	return OpenWithApprovedDigest(MoveTemp(ApprovedInput), ApprovedSha256);
}

TWoTBootstrapResult<TUniquePtr<FWoTInnoExtractor>>
FWoTInnoExtractor::OpenWithApprovedDigest(
	TArray<uint8>&& ApprovedInput, const FWoTSha256& ApprovedSha256)
{
	using FResult = TWoTBootstrapResult<TUniquePtr<FWoTInnoExtractor>>;
	if (ApprovedInput.Num() == 0)
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::UnknownField,
			TEXT("the approved installer callback input is empty")));
	}

	TUniquePtr<FImpl> NewImpl = MakeUnique<FImpl>();
	NewImpl->FrozenInput = MoveTemp(ApprovedInput);
	NewImpl->FrozenInputSha256 = ApprovedSha256;
	const uint64 InputSize = static_cast<uint64>(NewImpl->FrozenInput.Num());
	const int32 OpenCode = wot_innoextract_open(&FImpl::ReadAt, NewImpl.Get(),
		InputSize, &NewImpl->Package);
	if (OpenCode != WOT_INNOEXTRACT_OK)
	{
		return FResult::Error(WoTInnoAdapterError(OpenCode));
	}

	const uint64 Count = wot_innoextract_entry_count(NewImpl->Package);
	if (Count == 0 || Count > static_cast<uint64>(TNumericLimits<int32>::Max()))
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::SizeLimit,
			TEXT("the adapter returned an invalid entry count")));
	}
	NewImpl->Entries.Reserve(static_cast<int32>(Count));
	for (uint64 Index = 0; Index < Count; ++Index)
	{
		wot_innoextract_entry NativeEntry = {};
		const int32 EntryCode = wot_innoextract_entry_at(
			NewImpl->Package, Index, &NativeEntry);
		if (EntryCode != WOT_INNOEXTRACT_OK || NativeEntry.name_utf8 == nullptr
			|| NativeEntry.checksum_hex == nullptr)
		{
			return FResult::Error(WoTInnoAdapterError(EntryCode));
		}
		FWoTInnoEntryDescription Entry;
		Entry.AdapterIndex = static_cast<uint32>(Index);
		Entry.Name = UTF8_TO_TCHAR(NativeEntry.name_utf8);
		Entry.Size = NativeEntry.uncompressed_size;
		Entry.ChecksumType = WoTInnoChecksumType(NativeEntry.checksum_type);
		Entry.Checksum = UTF8_TO_TCHAR(NativeEntry.checksum_hex);
		Entry.Compression = WoTInnoCompression(NativeEntry.compression);
		NewImpl->Entries.Add(MoveTemp(Entry));
	}
	const ANSICHAR* const NativeDescription = wot_innoextract_description(NewImpl->Package);
	if (NativeDescription == nullptr || NativeDescription[0] == '\0')
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the adapter returned no held-snapshot description")));
	}
	NewImpl->Description = UTF8_TO_TCHAR(NativeDescription);

	TUniquePtr<FWoTInnoExtractor> Extractor(
		new FWoTInnoExtractor(MoveTemp(NewImpl)));
	return FResult::Success(MoveTemp(Extractor));
}

const TArray<FWoTInnoEntryDescription>& FWoTInnoExtractor::Entries() const
{
	check(Impl.IsValid());
	return Impl->Entries;
}

const FString& FWoTInnoExtractor::Description() const
{
	check(Impl.IsValid());
	return Impl->Description;
}

const FWoTSha256& FWoTInnoExtractor::CachedInputSha256() const
{
	check(Impl.IsValid());
	return Impl->FrozenInputSha256;
}

FWoTBootstrapStatus FWoTInnoExtractor::Extract(uint32 AdapterIndex,
	uint64 ExpectedSize, IWoTByteSink& Sink) const
{
	if (!Impl.IsValid() || !Impl->Entries.IsValidIndex(static_cast<int32>(AdapterIndex))
		|| Impl->Entries[static_cast<int32>(AdapterIndex)].Size != ExpectedSize)
	{
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::UnknownField, TEXT("the package entry does not belong to this adapter")));
	}
	FScopeLock Lock(&Impl->ExtractMutex);
	FWoTInnoWriteContext Write{Sink, ExpectedSize};
	const int32 Code = wot_innoextract_extract_entry(
		Impl->Package, AdapterIndex, &WoTInnoWrite, &Write);
	if (Write.Failure.IsSet())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(Write.Failure.GetValue()));
	}
	if (Code != WOT_INNOEXTRACT_OK)
	{
		return FWoTBootstrapStatus::Error(WoTInnoAdapterError(Code));
	}
	if (Write.Written != ExpectedSize)
	{
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("the adapter published a different byte count than it declared")));
	}
	return FWoTBootstrapStatus::Success();
}

#endif  // WOT_HAS_INNOEXTRACT
