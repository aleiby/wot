#include "WoTUpdaterCore/ReleaseStaging.h"

#include "WoTUpdaterCore/UpdaterPlatform.h"
#include "WoTUpdaterCore/UpdateFailureNotice.h"
#include "WoTUpdaterCore/UpdateState.h"
#include "HAL/FileManager.h"
#include "Misc/DateTime.h"

#include "Misc/Compression.h"
#include "Misc/Crc.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "WoTBootstrapCore/ArchiveRules.h"
#include "WoTBootstrapCore/Hash.h"
#include "WoTBootstrapCore/InstalledTree.h"
#include "WoTBootstrapCore/NativeHandles.h"
#include "WoTBootstrapCore/ReleaseManifest.h"

THIRD_PARTY_INCLUDES_START
#include <zlib.h>
THIRD_PARTY_INCLUDES_END

namespace
{
constexpr uint32 WoTReleaseStageZipLocalSignature = 0x04034b50u;
constexpr uint32 WoTReleaseStageZipCentralSignature = 0x02014b50u;
constexpr uint32 WoTReleaseStageZipEndSignature = 0x06054b50u;
constexpr uint16 WoTReleaseStageZipUtf8Flag = 0x0800u;

#if WITH_AUTOMATION_TESTS
TAtomic<bool> GWoTReleaseStageFailArchiveReadForTesting(false);
FWoTReleaseStagingCodeSealVerifierForTesting
	GWoTReleaseStagingCodeSealVerifierForTesting;
#endif

FWoTBootstrapStatus WoTReleaseStageVerifyCodeSeal(const FString& StagedTreeDir)
{
#if WITH_AUTOMATION_TESTS
	if (GWoTReleaseStagingCodeSealVerifierForTesting)
		return GWoTReleaseStagingCodeSealVerifierForTesting(StagedTreeDir);
#endif
	return WoTVerifyStagedCodeSeal(StagedTreeDir);
}

#if PLATFORM_MAC
constexpr TCHAR WoTReleaseStageLiveLeaf[] = TEXT("WoT.app");
#elif PLATFORM_WINDOWS
constexpr TCHAR WoTReleaseStageLiveLeaf[] = TEXT("Game");
#else
#error "release staging is supported only on Win64 and Mac"
#endif

FWoTBootstrapError WoTReleaseStageError(EWoTBootstrapError Code,
	FString Message, FString Path = FString())
{
	return MakeWoTBootstrapError(Code, MoveTemp(Message), MoveTemp(Path));
}

FWoTBootstrapStatus WoTReleaseStageFlushFile(
	const FWoTNativeFileHandle& File);
FWoTBootstrapStatus WoTReleaseStageFlushDirectory(
	const FWoTNativeDirectoryHandle& Directory);
TWoTBootstrapResult<TArray<uint8>> WoTReleaseStageReadAllCancellable(
	const FWoTNativeFileHandle& File,
	TFunctionRef<FWoTBootstrapStatus()> CheckCancellation);
FWoTBootstrapStatus WoTReleaseStageWriteCancellable(
	FWoTNativeFileHandle& File, TConstArrayView<uint8> Bytes,
	TFunctionRef<FWoTBootstrapStatus()> CheckCancellation);

FWoTBootstrapStatus WoTReleaseStageCheckCancellation(
	TFunctionRef<FWoTBootstrapStatus()> CheckCancellation)
{
	FWoTBootstrapStatus Checked = CheckCancellation();
	return Checked.IsError()
		? FWoTBootstrapStatus::Error(MoveTemp(Checked).GetError())
		: FWoTBootstrapStatus::Success();
}

bool WoTReleaseStageSameRoot(const FWoTUpdaterStateRoot& Left,
	const FWoTUpdaterStateRoot& Right)
{
	return Left.UserStateRoot == Right.UserStateRoot
		&& Left.InstallRoot == Right.InstallRoot
		&& Left.LiveLeaf == Right.LiveLeaf
		&& Left.UpdateDir == Right.UpdateDir && Left.StateDir == Right.StateDir
		&& Left.StagingDir == Right.StagingDir
        && Left.WorkDir == Right.WorkDir && Left.DownloadPath == Right.DownloadPath
        && Left.StagedTreeDir == Right.StagedTreeDir && Left.PreviousTreeDir == Right.PreviousTreeDir
        && Left.ReadyPath == Right.ReadyPath && Left.TrialPath == Right.TrialPath
        && Left.RolledBackPath == Right.RolledBackPath;
}

bool WoTReleaseStageRead16(TConstArrayView<uint8> Bytes, uint64 Offset, uint16& Out)
{
	if (Offset > static_cast<uint64>(Bytes.Num())
		|| static_cast<uint64>(Bytes.Num()) - Offset < 2)
	{
		return false;
	}
	Out = static_cast<uint16>(Bytes[Offset])
		| static_cast<uint16>(static_cast<uint16>(Bytes[Offset + 1]) << 8);
	return true;
}

bool WoTReleaseStageRead32(TConstArrayView<uint8> Bytes, uint64 Offset, uint32& Out)
{
	if (Offset > static_cast<uint64>(Bytes.Num())
		|| static_cast<uint64>(Bytes.Num()) - Offset < 4)
	{
		return false;
	}
	Out = static_cast<uint32>(Bytes[Offset])
		| (static_cast<uint32>(Bytes[Offset + 1]) << 8)
		| (static_cast<uint32>(Bytes[Offset + 2]) << 16)
		| (static_cast<uint32>(Bytes[Offset + 3]) << 24);
	return true;
}

FWoTBootstrapStatus WoTReleaseStageEmptyTree(
	const FWoTNativeDirectoryHandle& Directory,
	TFunctionRef<FWoTBootstrapStatus()> CheckCancellation);

FWoTBootstrapStatus WoTReleaseStageRemoveTreeAt(
	const FWoTNativeDirectoryHandle& Parent, FStringView Leaf,
	TFunctionRef<FWoTBootstrapStatus()> CheckCancellation = []()
	{
		return FWoTBootstrapStatus::Success();
	})
{
	if (FWoTBootstrapStatus Cancelled =
			WoTReleaseStageCheckCancellation(CheckCancellation);
		Cancelled.IsError())
	{
		return Cancelled;
	}
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> Opened =
		OpenNativeDirectoryAtNoFollow(Parent, Leaf);
	if (Opened.IsError())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(Opened).GetError());
	}
	FWoTNativeDirectoryHandle Directory = MoveTemp(Opened).GetValue();
	if (FWoTBootstrapStatus Emptied = WoTReleaseStageEmptyTree(
			Directory, CheckCancellation);
		Emptied.IsError())
	{
		return Emptied;
	}
	TWoTBootstrapResult<FWoTNativeFileIdentity> EmptyIdentity =
		Directory.QueryCurrentIdentity();
	if (EmptyIdentity.IsError())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(EmptyIdentity).GetError());
	}
	Directory = FWoTNativeDirectoryHandle();
	return RemoveNativeEntryAtNoFollowIfIdentity(Parent, Leaf,
		EWoTTreeEntryType::Directory, EmptyIdentity.GetValue());
}

FWoTBootstrapStatus WoTReleaseStageEmptyTree(
	const FWoTNativeDirectoryHandle& Directory,
	TFunctionRef<FWoTBootstrapStatus()> CheckCancellation)
{
	if (FWoTBootstrapStatus Cancelled =
			WoTReleaseStageCheckCancellation(CheckCancellation);
		Cancelled.IsError())
	{
		return Cancelled;
	}
	// Widen first, through the object already held. In particular, a signed 0000
	// directory on macOS cannot be reopened for enumeration until this succeeds;
	// on Win64 SetNativeMode obtains and validates a metadata-capable same-object
	// reopen before clearing FILE_ATTRIBUTE_READONLY.
	if (FWoTBootstrapStatus Writable = SetNativeMode(Directory, 0755u);
		Writable.IsError())
	{
		return Writable;
	}
	TWoTBootstrapResult<TArray<FWoTNativeDirectoryEntry>> Listed =
		EnumerateNativeDirectory(Directory);
	if (Listed.IsError())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(Listed).GetError());
	}
	for (const FWoTNativeDirectoryEntry& Entry : Listed.GetValue())
	{
		if (FWoTBootstrapStatus Cancelled =
				WoTReleaseStageCheckCancellation(CheckCancellation);
			Cancelled.IsError())
		{
			return Cancelled;
		}
		if (Entry.Type == EWoTTreeEntryType::Directory)
		{
			if (FWoTBootstrapStatus Removed =
					WoTReleaseStageRemoveTreeAt(
						Directory, Entry.Name, CheckCancellation);
				Removed.IsError())
			{
				return Removed;
			}
		}
		else
		{
			if (Entry.Type == EWoTTreeEntryType::File)
			{
				TWoTBootstrapResult<FWoTNativeFileHandle> File =
					OpenNativeFileAtReadNoFollow(Directory, Entry.Name);
				if (File.IsError())
				{
					return FWoTBootstrapStatus::Error(MoveTemp(File).GetError());
				}
				if (FWoTBootstrapStatus Writable =
						SetNativeMode(File.GetValue(), 0644u); Writable.IsError())
				{
					return Writable;
				}
			}
			if (FWoTBootstrapStatus Removed = RemoveNativeEntryAtNoFollow(
					Directory, Entry.Name, Entry.Type); Removed.IsError())
			{
				return Removed;
			}
		}
	}
	return FWoTBootstrapStatus::Success();
}

bool WoTReleaseStageHasEntry(const FWoTNativeDirectoryHandle& Directory,
	FStringView Leaf, EWoTTreeEntryType Type)
{
	TWoTBootstrapResult<TArray<FWoTNativeDirectoryEntry>> Listed =
		EnumerateNativeDirectory(Directory);
	if (Listed.IsError())
	{
		return false;
	}
	for (const FWoTNativeDirectoryEntry& Entry : Listed.GetValue())
	{
		if (FStringView(Entry.Name).Equals(Leaf, ESearchCase::CaseSensitive)
			&& Entry.Type == Type)
		{
			return true;
		}
	}
	return false;
}

FWoTBootstrapStatus WoTReleaseStageRemoveIfPresent(
	const FWoTNativeDirectoryHandle& Directory,
	FStringView Leaf)
{
	TWoTBootstrapResult<TArray<FWoTNativeDirectoryEntry>> Listed =
		EnumerateNativeDirectory(Directory);
	if (Listed.IsError())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(Listed).GetError());
	}
	for (const FWoTNativeDirectoryEntry& Entry : Listed.GetValue())
	{
		if (FStringView(Entry.Name).Equals(Leaf, ESearchCase::CaseSensitive))
		{
			return RemoveNativeEntryAtNoFollow(Directory, Leaf, Entry.Type);
		}
	}
	return FWoTBootstrapStatus::Success();
}

TWoTBootstrapResult<TArray<uint8>> WoTReleaseStageReadAt(
	const FWoTNativeDirectoryHandle& Directory, FStringView Leaf,
	TFunctionRef<FWoTBootstrapStatus()> CheckCancellation)
{
	TWoTBootstrapResult<FWoTNativeFileHandle> Opened =
		OpenNativeFileAtReadNoFollow(Directory, Leaf);
	if (Opened.IsError())
	{
		return TWoTBootstrapResult<TArray<uint8>>::Error(
			MoveTemp(Opened).GetError());
	}
	return WoTReleaseStageReadAllCancellable(
		Opened.GetValue(), CheckCancellation);
}
class FWoTReleaseStageArchiveSink final : public IWoTReleaseDownloadSink
{
public:
	FWoTReleaseStageArchiveSink(IWoTReleaseDownload& InDownload,
		FWoTNativeFileHandle& InFile, uint64 InSize,
		TFunctionRef<FWoTBootstrapStatus()> InCancel)
		: Download(InDownload), File(InFile), Size(InSize), Cancel(InCancel) {}
	FWoTBootstrapStatus OnResponseStart(const FWoTReleaseDownloadResponse& Response) override
	{
		if (bStarted || Response.Status != 200 || Response.BodyLength != Size)
			return FWoTBootstrapStatus::Error(WoTReleaseStageError(EWoTBootstrapError::Io,
				TEXT("the complete archive response does not have its signed size")));
		bStarted = true;
		return FWoTBootstrapStatus::Success();
	}
	FWoTBootstrapStatus Write(TConstArrayView<uint8> Bytes) override
	{
		auto Checked = Cancel();
		if (Checked.IsError()) return Checked;
		if (!bStarted || static_cast<uint64>(Bytes.Num()) > Size - Written)
			return FWoTBootstrapStatus::Error(WoTReleaseStageError(EWoTBootstrapError::SizeLimit,
				TEXT("the archive body exceeds its signed size")));
		auto Result = WriteNativeFileAt(File, Written, Bytes);
		if (Result.IsError()) return Result;
		Written += Bytes.Num();
		Download.ReportProgress(Written);
		return FWoTBootstrapStatus::Success();
	}
	FWoTBootstrapStatus Finish()
	{
		if (!bStarted || Written != Size)
			return FWoTBootstrapStatus::Error(WoTReleaseStageError(EWoTBootstrapError::Io,
				TEXT("the archive transfer ended before the signed size")));
		return FWoTBootstrapStatus::Success();
	}
private:
	IWoTReleaseDownload& Download;
	FWoTNativeFileHandle& File;
	uint64 Size;
	TFunctionRef<FWoTBootstrapStatus()> Cancel;
	uint64 Written = 0;
	bool bStarted = false;
};

FWoTBootstrapStatus WoTReleaseStageDownloadArchive(IWoTReleaseDownload& Download,
	const FWoTSignedObjectRef& Expected, const FWoTNativeDirectoryHandle& Work,
	TFunctionRef<FWoTBootstrapStatus()> CheckCancellation)
{
	auto File = CreateNativeFileAtWriteNoFollow(Work, TEXT("download.zip"));
	if (File.IsError()) return FWoTBootstrapStatus::Error(MoveTemp(File).GetError());
	FWoTReleaseStageArchiveSink Sink(Download, File.GetValue(), Expected.Size, CheckCancellation);
	auto Fetched = Download.Fetch(Sink);
	return Fetched.IsError() ? MoveTemp(Fetched) : Sink.Finish();
}

struct FWoTReleaseStageZipEntry
{
	FWoTArchiveEntry Rule;
	uint16 Method = 0;
	uint32 Crc32 = 0;
	uint32 CompressedSize = 0;
	uint32 DataOffset = 0;
};

TWoTBootstrapResult<FString> WoTReleaseStageZipName(
	TConstArrayView<uint8> Archive, uint64 Offset, uint16 Length)
{
	using FResult = TWoTBootstrapResult<FString>;
	if (Offset > static_cast<uint64>(Archive.Num())
		|| static_cast<uint64>(Archive.Num()) - Offset < Length)
	{
		return FResult::Error(WoTReleaseStageError(EWoTBootstrapError::Io,
			TEXT("a ZIP name extends beyond the archive")));
	}
	const ANSICHAR* Data = reinterpret_cast<const ANSICHAR*>(
		Archive.GetData() + Offset);
	for (uint16 Index = 0; Index < Length; ++Index)
	{
		if (Data[Index] == '\0')
		{
			return FResult::Error(WoTReleaseStageError(
				EWoTBootstrapError::UnsafePath, TEXT("a ZIP name contains NUL")));
		}
	}
	const FUTF8ToTCHAR Converted(Data, Length);
	FString Name = FString::ConstructFromPtrSize(Converted.Get(), Converted.Length());
	const FTCHARToUTF8 RoundTrip(*Name);
	if (RoundTrip.Length() != Length
		|| (Length > 0 && FMemory::Memcmp(RoundTrip.Get(), Data, Length) != 0))
	{
		return FResult::Error(WoTReleaseStageError(
			EWoTBootstrapError::InvalidUtf8, TEXT("a ZIP name is not valid UTF-8")));
	}
	return FResult::Success(MoveTemp(Name));
}

FWoTBootstrapStatus WoTReleaseStageFlushFile(
	const FWoTNativeFileHandle& File)
{
	FWoTBootstrapStatus Result = FWoTBootstrapStatus::Success();
	Result = FlushNativeFile(File);
	return Result;
}

FWoTBootstrapStatus WoTReleaseStageFlushDirectory(
	const FWoTNativeDirectoryHandle& Directory)
{
	FWoTBootstrapStatus Result = FWoTBootstrapStatus::Success();
	Result = FlushNativeDirectory(Directory);
	return Result;
}

TWoTBootstrapResult<FString> WoTReleaseStageUtf8(
	TConstArrayView<uint8> Bytes, FStringView What)
{
	using FResult = TWoTBootstrapResult<FString>;
	if (Bytes.IsEmpty())
	{
		return FResult::Error(WoTReleaseStageError(EWoTBootstrapError::UnsafePath,
			TEXT("a symbolic-link target is empty"), FString(What)));
	}
	for (uint8 Byte : Bytes)
	{
		if (Byte == 0)
		{
			return FResult::Error(WoTReleaseStageError(EWoTBootstrapError::UnsafePath,
				TEXT("a symbolic-link target contains NUL"), FString(What)));
		}
	}
	const ANSICHAR* Data = reinterpret_cast<const ANSICHAR*>(Bytes.GetData());
	const FUTF8ToTCHAR Converted(Data, Bytes.Num());
	FString Value = FString::ConstructFromPtrSize(Converted.Get(), Converted.Length());
	const FTCHARToUTF8 RoundTrip(*Value);
	if (RoundTrip.Length() != Bytes.Num()
		|| FMemory::Memcmp(RoundTrip.Get(), Data, Bytes.Num()) != 0)
	{
		return FResult::Error(WoTReleaseStageError(EWoTBootstrapError::InvalidUtf8,
			TEXT("a symbolic-link target is not valid UTF-8"), FString(What)));
	}
	return FResult::Success(MoveTemp(Value));
}

TWoTBootstrapResult<TArray<FWoTReleaseStageZipEntry>>
WoTReleaseStageParseZip(TConstArrayView<uint8> Archive,
	TFunctionRef<FWoTBootstrapStatus()> CheckCancellation)
{
	using FResult = TWoTBootstrapResult<TArray<FWoTReleaseStageZipEntry>>;
	if (Archive.Num() < 22)
	{
		return FResult::Error(WoTReleaseStageError(
			EWoTBootstrapError::Io, TEXT("the release archive is not a ZIP file")));
	}
	int64 EndOffset = INDEX_NONE;
	const int64 SearchStart = FMath::Max<int64>(0, Archive.Num() - 22 - 65535);
	for (int64 Offset = Archive.Num() - 22; Offset >= SearchStart; --Offset)
	{
		if ((Offset & 0xfff) == 0)
		{
			FWoTBootstrapStatus Cancelled =
				WoTReleaseStageCheckCancellation(CheckCancellation);
			if (Cancelled.IsError())
			{
				return FResult::Error(MoveTemp(Cancelled).GetError());
			}
		}
		uint32 Signature = 0;
		if (WoTReleaseStageRead32(Archive, static_cast<uint64>(Offset), Signature)
			&& Signature == WoTReleaseStageZipEndSignature)
		{
			uint16 CommentLength = 0;
			if (WoTReleaseStageRead16(Archive,
					static_cast<uint64>(Offset) + 20, CommentLength)
				&& static_cast<uint64>(Offset) + 22 + CommentLength
					== static_cast<uint64>(Archive.Num()))
			{
				EndOffset = Offset;
				break;
			}
		}
	}
	if (EndOffset == INDEX_NONE)
	{
		return FResult::Error(WoTReleaseStageError(EWoTBootstrapError::Io,
			TEXT("the ZIP end-of-central-directory record is absent or malformed")));
	}

	uint16 Disk = 0;
	uint16 CentralDisk = 0;
	uint16 DiskEntries = 0;
	uint16 TotalEntries = 0;
	uint32 CentralSize = 0;
	uint32 CentralOffset = 0;
	if (!WoTReleaseStageRead16(Archive, EndOffset + 4, Disk)
		|| !WoTReleaseStageRead16(Archive, EndOffset + 6, CentralDisk)
		|| !WoTReleaseStageRead16(Archive, EndOffset + 8, DiskEntries)
		|| !WoTReleaseStageRead16(Archive, EndOffset + 10, TotalEntries)
		|| !WoTReleaseStageRead32(Archive, EndOffset + 12, CentralSize)
		|| !WoTReleaseStageRead32(Archive, EndOffset + 16, CentralOffset)
		|| Disk != 0 || CentralDisk != 0 || DiskEntries != TotalEntries
		|| static_cast<uint64>(CentralOffset) + CentralSize
			!= static_cast<uint64>(EndOffset))
	{
		return FResult::Error(WoTReleaseStageError(EWoTBootstrapError::Io,
			TEXT("the ZIP central directory is split, ZIP64, or inconsistent")));
	}

	TArray<FWoTReleaseStageZipEntry> Entries;
	Entries.Reserve(TotalEntries);
	uint64 Cursor = CentralOffset;
	TArray<TPair<uint64, uint64>> LocalRanges;
	for (uint16 EntryIndex = 0; EntryIndex < TotalEntries; ++EntryIndex)
	{
		FWoTBootstrapStatus Cancelled =
			WoTReleaseStageCheckCancellation(CheckCancellation);
		if (Cancelled.IsError())
		{
			return FResult::Error(MoveTemp(Cancelled).GetError());
		}
		uint32 Signature = 0;
		uint16 Flags = 0;
		uint16 Method = 0;
		uint32 Crc = 0;
		uint32 CompressedSize = 0;
		uint32 UncompressedSize = 0;
		uint16 NameLength = 0;
		uint16 ExtraLength = 0;
		uint16 CommentLength = 0;
		uint16 StartDisk = 0;
		uint32 External = 0;
		uint32 LocalOffset = 0;
		if (!WoTReleaseStageRead32(Archive, Cursor, Signature)
			|| Signature != WoTReleaseStageZipCentralSignature
			|| !WoTReleaseStageRead16(Archive, Cursor + 8, Flags)
			|| !WoTReleaseStageRead16(Archive, Cursor + 10, Method)
			|| !WoTReleaseStageRead32(Archive, Cursor + 16, Crc)
			|| !WoTReleaseStageRead32(Archive, Cursor + 20, CompressedSize)
			|| !WoTReleaseStageRead32(Archive, Cursor + 24, UncompressedSize)
			|| !WoTReleaseStageRead16(Archive, Cursor + 28, NameLength)
			|| !WoTReleaseStageRead16(Archive, Cursor + 30, ExtraLength)
			|| !WoTReleaseStageRead16(Archive, Cursor + 32, CommentLength)
			|| !WoTReleaseStageRead16(Archive, Cursor + 34, StartDisk)
			|| !WoTReleaseStageRead32(Archive, Cursor + 38, External)
			|| !WoTReleaseStageRead32(Archive, Cursor + 42, LocalOffset))
		{
			return FResult::Error(WoTReleaseStageError(EWoTBootstrapError::Io,
				TEXT("a ZIP central header is truncated or malformed")));
		}
		const uint64 CentralEnd = Cursor + 46ull + NameLength
			+ ExtraLength + CommentLength;
		if (CentralEnd > static_cast<uint64>(EndOffset) || StartDisk != 0
			|| CompressedSize == MAX_uint32 || UncompressedSize == MAX_uint32
			|| LocalOffset == MAX_uint32 || (Flags & ~WoTReleaseStageZipUtf8Flag) != 0
			|| (Method != 0 && Method != 8))
		{
			return FResult::Error(WoTReleaseStageError(EWoTBootstrapError::UnsupportedProtocol,
				TEXT("a ZIP entry uses an unsupported feature")));
		}
		TWoTBootstrapResult<FString> Name = WoTReleaseStageZipName(
			Archive, Cursor + 46, NameLength);
		if (Name.IsError())
		{
			return FResult::Error(MoveTemp(Name).GetError());
		}

		uint32 LocalSignature = 0;
		uint16 LocalFlags = 0;
		uint16 LocalMethod = 0;
		uint32 LocalCrc = 0;
		uint32 LocalCompressed = 0;
		uint32 LocalUncompressed = 0;
		uint16 LocalNameLength = 0;
		uint16 LocalExtraLength = 0;
		if (!WoTReleaseStageRead32(Archive, LocalOffset, LocalSignature)
			|| LocalSignature != WoTReleaseStageZipLocalSignature
			|| !WoTReleaseStageRead16(Archive, LocalOffset + 6, LocalFlags)
			|| !WoTReleaseStageRead16(Archive, LocalOffset + 8, LocalMethod)
			|| !WoTReleaseStageRead32(Archive, LocalOffset + 14, LocalCrc)
			|| !WoTReleaseStageRead32(Archive, LocalOffset + 18, LocalCompressed)
			|| !WoTReleaseStageRead32(Archive, LocalOffset + 22, LocalUncompressed)
			|| !WoTReleaseStageRead16(Archive, LocalOffset + 26, LocalNameLength)
			|| !WoTReleaseStageRead16(Archive, LocalOffset + 28, LocalExtraLength))
		{
			return FResult::Error(WoTReleaseStageError(EWoTBootstrapError::Io,
				TEXT("a ZIP local header is truncated or malformed")));
		}
		TWoTBootstrapResult<FString> LocalName = WoTReleaseStageZipName(
			Archive, static_cast<uint64>(LocalOffset) + 30, LocalNameLength);
		const uint64 DataOffset = static_cast<uint64>(LocalOffset) + 30
			+ LocalNameLength + LocalExtraLength;
		const uint64 DataEnd = DataOffset + CompressedSize;
		if (LocalName.IsError() || LocalFlags != Flags || LocalMethod != Method
			|| LocalCrc != Crc || LocalCompressed != CompressedSize
			|| LocalUncompressed != UncompressedSize
			|| LocalName.GetValue() != Name.GetValue()
			|| DataEnd > CentralOffset || DataOffset > MAX_uint32)
		{
			return FResult::Error(WoTReleaseStageError(EWoTBootstrapError::CorruptInstallation,
				TEXT("a ZIP central/local header disagreement was detected"),
				Name.GetValue()));
		}
		for (const TPair<uint64, uint64>& Range : LocalRanges)
		{
			if (static_cast<uint64>(LocalOffset) < Range.Value && DataEnd > Range.Key)
			{
				return FResult::Error(WoTReleaseStageError(EWoTBootstrapError::CorruptInstallation,
					TEXT("two ZIP local records overlap"), Name.GetValue()));
			}
		}
		LocalRanges.Emplace(LocalOffset, DataEnd);

		FString Path = MoveTemp(Name).GetValue();
		const bool bTrailingSlash = Path.EndsWith(TEXT("/"), ESearchCase::CaseSensitive);
		if (bTrailingSlash)
		{
			Path.LeftChopInline(1);
		}
		const uint32 UnixMode = (External >> 16) & 0xffffu;
		const uint32 UnixType = UnixMode & 0170000u;
		const bool bDosDirectory = (External & 0x10u) != 0;
		const bool bDosReparse = (External & 0x400u) != 0;
		FWoTReleaseStageZipEntry& Added = Entries.AddDefaulted_GetRef();
		Added.Rule.Path = MoveTemp(Path);
		Added.Rule.Size = UncompressedSize;
		Added.Rule.Mode = UnixMode & 0777u;
		if (UnixType == 0120000u || bDosReparse)
		{
			Added.Rule.Type = EWoTTreeEntryType::SymbolicLink;
			Added.Rule.LinkTarget = TEXT("inspected-link-entry");
		}
		else if (UnixType == 0040000u || bTrailingSlash || bDosDirectory)
		{
			Added.Rule.Type = EWoTTreeEntryType::Directory;
			Added.Rule.Size = 0;
			if (Added.Rule.Mode == 0) Added.Rule.Mode = 0755u;
		}
		else
		{
			Added.Rule.Type = EWoTTreeEntryType::File;
			if (Added.Rule.Mode == 0) Added.Rule.Mode = 0644u;
		}
		Added.Method = Method;
		Added.Crc32 = Crc;
		Added.CompressedSize = CompressedSize;
		Added.DataOffset = static_cast<uint32>(DataOffset);
		Cursor = CentralEnd;
	}
	if (Cursor != static_cast<uint64>(EndOffset))
	{
		return FResult::Error(WoTReleaseStageError(EWoTBootstrapError::Io,
			TEXT("the ZIP central directory has unparsed trailing bytes")));
	}
	return FResult::Success(MoveTemp(Entries));
}

TWoTBootstrapResult<TArray<uint8>> WoTReleaseStageInflate(
	TConstArrayView<uint8> Archive, const FWoTReleaseStageZipEntry& Entry,
	TFunctionRef<FWoTBootstrapStatus()> CheckCancellation)
{
	using FResult = TWoTBootstrapResult<TArray<uint8>>;
	if (Entry.Rule.Size > static_cast<uint64>(TNumericLimits<int32>::Max())
		|| static_cast<uint64>(Entry.DataOffset) + Entry.CompressedSize
			> static_cast<uint64>(Archive.Num()))
	{
		return FResult::Error(WoTReleaseStageError(EWoTBootstrapError::SizeLimit,
			TEXT("a ZIP member cannot fit in a bounded extraction buffer"),
			Entry.Rule.Path));
	}
	TArray<uint8> Bytes;
	Bytes.SetNumUninitialized(static_cast<int32>(Entry.Rule.Size));
	const uint8* Compressed = Archive.GetData() + Entry.DataOffset;
	if (Entry.Method == 0)
	{
		if (Entry.CompressedSize != Entry.Rule.Size)
		{
			return FResult::Error(WoTReleaseStageError(EWoTBootstrapError::CorruptInstallation,
				TEXT("a stored ZIP member has unequal sizes"), Entry.Rule.Path));
		}
		constexpr int32 ChunkBytes = 1024 * 1024;
		for (uint32 Offset = 0; Offset < Entry.CompressedSize;)
		{
			FWoTBootstrapStatus Cancelled =
				WoTReleaseStageCheckCancellation(CheckCancellation);
			if (Cancelled.IsError())
			{
				return FResult::Error(MoveTemp(Cancelled).GetError());
			}
			const uint32 Count = FMath::Min<uint32>(
				ChunkBytes, Entry.CompressedSize - Offset);
			FMemory::Memcpy(Bytes.GetData() + Offset, Compressed + Offset, Count);
			Offset += Count;
		}
	}
	else
	{
		z_stream Stream = {};
		Stream.next_in = const_cast<Bytef*>(
			reinterpret_cast<const Bytef*>(Compressed));
		Stream.avail_in = static_cast<uInt>(Entry.CompressedSize);
		if (inflateInit2(&Stream, -MAX_WBITS) != Z_OK)
		{
			return FResult::Error(WoTReleaseStageError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("a deflated ZIP member could not initialize"),
				Entry.Rule.Path));
		}
		struct FInflateEnd final
		{
			z_stream* Stream = nullptr;
			~FInflateEnd()
			{
				if (Stream != nullptr)
				{
					inflateEnd(Stream);
				}
			}
		} End{&Stream};
		constexpr uInt InflateChunkBytes = 1024 * 1024;
		uint8 EmptyOutput = 0;
		int32 InflateResult = Z_OK;
		do
		{
			FWoTBootstrapStatus Cancelled =
				WoTReleaseStageCheckCancellation(CheckCancellation);
			if (Cancelled.IsError())
			{
				return FResult::Error(MoveTemp(Cancelled).GetError());
			}
			const uint64 Produced = static_cast<uint64>(Stream.total_out);
			const uint64 Remaining = Entry.Rule.Size - Produced;
			Stream.next_out = Remaining == 0
				? &EmptyOutput
				: reinterpret_cast<Bytef*>(Bytes.GetData() + Produced);
			Stream.avail_out = Remaining == 0
				? 1u
				: static_cast<uInt>(FMath::Min<uint64>(
					InflateChunkBytes, Remaining));
			InflateResult = inflate(&Stream, Z_NO_FLUSH);
			if (static_cast<uint64>(Stream.total_out) > Entry.Rule.Size
				|| (InflateResult != Z_OK && InflateResult != Z_STREAM_END))
			{
				return FResult::Error(WoTReleaseStageError(
					EWoTBootstrapError::CorruptInstallation,
					TEXT("a deflated ZIP member is corrupt"), Entry.Rule.Path));
			}
		}
		while (InflateResult != Z_STREAM_END);
		if (static_cast<uint64>(Stream.total_out) != Entry.Rule.Size
			|| Stream.total_in != Entry.CompressedSize)
		{
			return FResult::Error(WoTReleaseStageError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("a deflated ZIP member has an inconsistent size"),
				Entry.Rule.Path));
		}
	}
	uint32 ActualCrc = 0;
	constexpr int32 CrcChunkBytes = 1024 * 1024;
	for (int32 Offset = 0; Offset < Bytes.Num(); Offset += CrcChunkBytes)
	{
		FWoTBootstrapStatus Cancelled =
			WoTReleaseStageCheckCancellation(CheckCancellation);
		if (Cancelled.IsError())
		{
			return FResult::Error(MoveTemp(Cancelled).GetError());
		}
		const int32 Count = FMath::Min(CrcChunkBytes, Bytes.Num() - Offset);
		ActualCrc = FCrc::MemCrc32(Bytes.GetData() + Offset, Count, ActualCrc);
	}
	if (ActualCrc != Entry.Crc32)
	{
		return FResult::Error(WoTReleaseStageError(EWoTBootstrapError::CorruptInstallation,
			TEXT("a ZIP member CRC does not match"), Entry.Rule.Path));
	}
	return FResult::Success(MoveTemp(Bytes));
}

FString WoTReleaseStageParentPath(const FString& Path)
{
	int32 Slash = INDEX_NONE;
	return Path.FindLastChar(TEXT('/'), Slash) ? Path.Left(Slash) : FString();
}

FString WoTReleaseStageLeaf(const FString& Path)
{
	int32 Slash = INDEX_NONE;
	return Path.FindLastChar(TEXT('/'), Slash) ? Path.Mid(Slash + 1) : Path;
}

int32 WoTReleaseStageDepth(const FString& Path)
{
	int32 Depth = 1;
	for (const TCHAR Character : Path) Depth += Character == TEXT('/') ? 1 : 0;
	return Depth;
}

TWoTBootstrapResult<FWoTNativeDirectoryHandle> WoTReleaseStageOpenParent(
	const FWoTNativeDirectoryHandle& Fresh, const FString& Path)
{
	const FString Parent = WoTReleaseStageParentPath(Path);
	if (Parent.IsEmpty())
	{
		return Fresh.DuplicateForRead();
	}
	return OpenNativeDirectoryAtNoFollow(Fresh, Parent);
}

struct FWoTReleaseStageHeldFile
{
	FString RelativePath;
	FWoTNativeFileHandle Handle;
};

struct FWoTReleaseStageHeldDirectory
{
	FString RelativePath;
	FWoTNativeDirectoryHandle Handle;
};

struct FWoTReleaseStageDirectorySnapshot
{
	FString RelativePath;
	FWoTNativeFileIdentity BeforeWalk;
	FWoTNativeDirectoryHandle Handle;
	/** The sorted entry set observed by the walk. Compared literally at
	    revalidation, because identity alone cannot be: NTFS advances directory
	    ChangeTime on the system-timer tick (~15.6ms), so an entry added in the
	    same tick as the last legitimate mutation leaves the strict identity
	    UNCHANGED (probed: 17-20 of 20 same-tick additions were invisible to
	    FILE_BASIC_INFO ChangeTime). APFS nanosecond ctime masks this on Mac. */
	TArray<FWoTNativeDirectoryEntry> Entries;
};

const FWoTReleaseStageHeldFile* WoTReleaseStageFindHeld(
	const TArray<FWoTReleaseStageHeldFile>& Held, FStringView RelativePath)
{
	for (const FWoTReleaseStageHeldFile& File : Held)
	{
		if (FStringView(File.RelativePath).Equals(
			RelativePath, ESearchCase::CaseSensitive))
		{
			return &File;
		}
	}
	return nullptr;
}

const FWoTReleaseStageHeldDirectory* WoTReleaseStageFindHeldDirectory(
	const TArray<FWoTReleaseStageHeldDirectory>& Held, FStringView RelativePath)
{
	for (const FWoTReleaseStageHeldDirectory& Directory : Held)
	{
		if (FStringView(Directory.RelativePath).Equals(
			RelativePath, ESearchCase::CaseSensitive))
		{
			return &Directory;
		}
	}
	return nullptr;
}

const FWoTInstalledTreeEntry* WoTReleaseStageFindExpected(
	const FWoTInstalledTreeManifest& Inventory, FStringView RelativePath)
{
	for (const FWoTInstalledTreeEntry& Entry : Inventory.Entries)
	{
		if (FStringView(Entry.Path).Equals(
			RelativePath, ESearchCase::CaseSensitive))
		{
			return &Entry;
		}
	}
	return nullptr;
}

TWoTBootstrapResult<FWoTSha256> WoTReleaseStageHashYielding(
	TConstArrayView<uint8> Bytes,
	TFunctionRef<FWoTBootstrapStatus()> CheckCancellation)
{
	using FResult = TWoTBootstrapResult<FWoTSha256>;
	constexpr int32 ChunkBytes = 1024 * 1024;
	FWoTSha256Hasher Hasher;
	for (int32 Offset = 0; Offset < Bytes.Num(); Offset += ChunkBytes)
	{
		const int32 Count = FMath::Min(ChunkBytes, Bytes.Num() - Offset);
		Hasher.Update(Bytes.Slice(Offset, Count));
		if (Offset + Count < Bytes.Num())
		{
			FWoTBootstrapStatus Cancelled =
				WoTReleaseStageCheckCancellation(CheckCancellation);
			if (Cancelled.IsError())
			{
				return FResult::Error(MoveTemp(Cancelled).GetError());
			}
		}
	}
	return FResult::Success(Hasher.Finish());
}

TWoTBootstrapResult<FWoTSha256> WoTReleaseStageHashFileYielding(
	const FWoTNativeFileHandle& File, uint64 Size,
	TFunctionRef<FWoTBootstrapStatus()> CheckCancellation)
{
	using FResult = TWoTBootstrapResult<FWoTSha256>;
	constexpr int32 ChunkBytes = 1024 * 1024;
	TArray<uint8> Buffer;
	Buffer.SetNumUninitialized(ChunkBytes);
	FWoTSha256Hasher Hasher;
	uint64 Offset = 0;
	while (Offset < Size)
	{
		if (FWoTBootstrapStatus Cancelled =
				WoTReleaseStageCheckCancellation(CheckCancellation);
			Cancelled.IsError())
		{
			return FResult::Error(MoveTemp(Cancelled).GetError());
		}
#if WITH_AUTOMATION_TESTS
		if (GWoTReleaseStageFailArchiveReadForTesting.Load())
		{
			return FResult::Error(WoTReleaseStageError(EWoTBootstrapError::Io,
				TEXT("injected complete-archive read failure")));
		}
#endif
		const int32 Count = static_cast<int32>(FMath::Min<uint64>(
			ChunkBytes, Size - Offset));
		TWoTBootstrapResult<uint64> Read = ReadNativeFileAt(
			File, Offset, MakeArrayView(Buffer.GetData(), Count));
		if (Read.IsError())
		{
			return FResult::Error(MoveTemp(Read).GetError());
		}
		if (Read.GetValue() == 0 || Read.GetValue() > static_cast<uint64>(Count))
		{
			return FResult::Error(WoTReleaseStageError(EWoTBootstrapError::Io,
				TEXT("the archive ended during its streaming hash")));
		}
		Hasher.Update(MakeArrayView(
			Buffer.GetData(), static_cast<int32>(Read.GetValue())));
		Offset += Read.GetValue();
	}
	return FResult::Success(Hasher.Finish());
}

FWoTBootstrapStatus WoTReleaseStageWalkExactTree(
	const FWoTNativeDirectoryHandle& RootHandle, FStringView Prefix,
	const FWoTInstalledTreeManifest& Expected,
	const TArray<FWoTReleaseStageHeldFile>& Held,
	FWoTInstalledTreeManifest& Actual,
	TArray<FWoTReleaseStageDirectorySnapshot>& DirectorySnapshots,
	TFunctionRef<FWoTBootstrapStatus()> CheckCancellation)
{
	if (FWoTBootstrapStatus Cancelled =
			WoTReleaseStageCheckCancellation(CheckCancellation);
		Cancelled.IsError())
	{
		return Cancelled;
	}
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> Opened = [&]()
	{
		if (Prefix.IsEmpty())
		{
			return RootHandle.DuplicateForRead();
		}
		return OpenNativeDirectoryAtNoFollow(RootHandle, Prefix);
	}();
	if (Opened.IsError())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(Opened).GetError());
	}
	FWoTNativeDirectoryHandle Current = MoveTemp(Opened).GetValue();
	const FWoTNativeFileIdentity BeforeWalk = Current.GetIdentity();
	if (!Prefix.IsEmpty())
	{
		const FWoTInstalledTreeEntry* SignedDirectory =
			WoTReleaseStageFindExpected(Expected, Prefix);
		if (SignedDirectory == nullptr
			|| SignedDirectory->Type != EWoTTreeEntryType::Directory)
		{
			return FWoTBootstrapStatus::Error(WoTReleaseStageError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("a traversed staging directory is not in the signed tree"),
				FString(Prefix)));
		}
		TWoTBootstrapResult<uint32> Mode = QueryNativeMode(Current);
		if (Mode.IsError() || Mode.GetValue() != SignedDirectory->Mode)
		{
			return FWoTBootstrapStatus::Error(Mode.IsError()
				? MoveTemp(Mode).GetError()
				: WoTReleaseStageError(EWoTBootstrapError::CorruptInstallation,
					TEXT("a staged directory mode differs from the signed mode"),
					FString(Prefix)));
		}
		FWoTInstalledTreeEntry Verified = *SignedDirectory;
		Verified.Mode = Mode.GetValue();
		Actual.Entries.Add(MoveTemp(Verified));
	}
	TWoTBootstrapResult<TArray<FWoTNativeDirectoryEntry>> Listed =
		EnumerateNativeDirectory(Current);
	if (Listed.IsError())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(Listed).GetError());
	}
	Listed.GetValue().Sort([](const FWoTNativeDirectoryEntry& Left,
		const FWoTNativeDirectoryEntry& Right)
	{
		return Left.Name.Compare(Right.Name, ESearchCase::CaseSensitive) < 0;
	});
	FWoTReleaseStageDirectorySnapshot Snapshot;
	Snapshot.RelativePath = FString(Prefix);
	Snapshot.BeforeWalk = BeforeWalk;
	Snapshot.Handle = MoveTemp(Current);
	Snapshot.Entries = Listed.GetValue();
	DirectorySnapshots.Add(MoveTemp(Snapshot));
	for (const FWoTNativeDirectoryEntry& Native : Listed.GetValue())
	{
		if (FWoTBootstrapStatus Cancelled =
				WoTReleaseStageCheckCancellation(CheckCancellation);
			Cancelled.IsError())
		{
			return Cancelled;
		}
		const FString Relative = Prefix.IsEmpty()
			? Native.Name : FString(Prefix) + TEXT("/") + Native.Name;
		const FWoTInstalledTreeEntry* Signed =
			WoTReleaseStageFindExpected(Expected, Relative);
		if (Signed == nullptr)
		{
			return FWoTBootstrapStatus::Error(WoTReleaseStageError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("the staged tree contains an unsigned extra entry"), Relative));
		}
		if (Native.Type != Signed->Type)
		{
			return FWoTBootstrapStatus::Error(WoTReleaseStageError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("a staged entry changed type"), Relative));
		}

		FWoTInstalledTreeEntry Verified = *Signed;
		if (Native.Type == EWoTTreeEntryType::Directory)
		{
			if (FWoTBootstrapStatus Nested = WoTReleaseStageWalkExactTree(
					RootHandle, Relative, Expected, Held, Actual,
					DirectorySnapshots, CheckCancellation); Nested.IsError())
			{
				return Nested;
			}
		}
		else if (Native.Type == EWoTTreeEntryType::File)
		{
			const FWoTReleaseStageHeldFile* Written =
				WoTReleaseStageFindHeld(Held, Relative);
			if (Written == nullptr || Written->Handle.GetIdentity().LinkCount != 1)
			{
				return FWoTBootstrapStatus::Error(WoTReleaseStageError(
					EWoTBootstrapError::CorruptInstallation,
					TEXT("the staged file is not the single-link object extraction held"),
					Relative));
			}
			TWoTBootstrapResult<FWoTNativeFileHandle> Named =
				OpenNativeFileAtReadNoFollow(RootHandle, Relative);
			if (Named.IsError()
				|| !Written->Handle.GetIdentity().IsSameObject(
					Named.GetValue().GetIdentity()))
			{
				return FWoTBootstrapStatus::Error(Named.IsError()
					? MoveTemp(Named).GetError()
					: WoTReleaseStageError(EWoTBootstrapError::CorruptInstallation,
						TEXT("the staged file name no longer denotes the object extraction held"),
						Relative));
			}
			TWoTBootstrapResult<FWoTNativeFileHandle> Reader =
				Written->Handle.DuplicateForRead();
			if (Reader.IsError())
			{
				return FWoTBootstrapStatus::Error(MoveTemp(Reader).GetError());
			}
			const FWoTNativeFileIdentity Before = Reader.GetValue().GetIdentity();
			TWoTBootstrapResult<TArray<uint8>> Bytes =
				WoTReleaseStageReadAllCancellable(
					Reader.GetValue(), CheckCancellation);
			TWoTBootstrapResult<FWoTNativeFileIdentity> After =
				Reader.GetValue().QueryCurrentIdentity();
			TWoTBootstrapResult<uint32> Mode = QueryNativeMode(Reader.GetValue());
			if (Bytes.IsError() || After.IsError() || Mode.IsError())
			{
				if (Bytes.IsError()) return FWoTBootstrapStatus::Error(MoveTemp(Bytes).GetError());
				if (After.IsError()) return FWoTBootstrapStatus::Error(MoveTemp(After).GetError());
				return FWoTBootstrapStatus::Error(MoveTemp(Mode).GetError());
			}
			TWoTBootstrapResult<FWoTSha256> Digest =
				WoTReleaseStageHashYielding(
					Bytes.GetValue(), CheckCancellation);
			if (Digest.IsError())
			{
				return FWoTBootstrapStatus::Error(MoveTemp(Digest).GetError());
			}
			if (Before != After.GetValue()
				|| static_cast<uint64>(Bytes.GetValue().Num()) != Signed->Size
				|| Digest.GetValue() != Signed->Sha256
				|| Mode.GetValue() != Signed->Mode)
			{
				return FWoTBootstrapStatus::Error(WoTReleaseStageError(
					EWoTBootstrapError::CorruptInstallation,
					TEXT("the held staged file bytes, size, or mode differ from the signed tree"),
					Relative));
			}
			Verified.Size = static_cast<uint64>(Bytes.GetValue().Num());
			Verified.Sha256 = Digest.GetValue();
			Verified.Mode = Mode.GetValue();
			Actual.Entries.Add(MoveTemp(Verified));
		}
		else if (Native.Type == EWoTTreeEntryType::SymbolicLink)
		{
			TWoTBootstrapResult<FString> Target =
				ReadNativeSymbolicLinkAtNoFollow(
					DirectorySnapshots.Last().Handle, Native.Name);
			TWoTBootstrapResult<uint32> Mode =
				QueryNativeSymbolicLinkModeAtNoFollow(
					DirectorySnapshots.Last().Handle, Native.Name);
			if (Target.IsError() || Mode.IsError())
			{
				return FWoTBootstrapStatus::Error(Target.IsError()
					? MoveTemp(Target).GetError() : MoveTemp(Mode).GetError());
			}
			if (Target.GetValue() != Signed->LinkTarget
				|| Mode.GetValue() != Signed->Mode)
			{
				return FWoTBootstrapStatus::Error(WoTReleaseStageError(
					EWoTBootstrapError::CorruptInstallation,
					TEXT("a staged symbolic link differs from the signed target or mode"),
					Relative));
			}
			Verified.LinkTarget = MoveTemp(Target).GetValue();
			Verified.Mode = Mode.GetValue();
			Actual.Entries.Add(MoveTemp(Verified));
		}
		else
		{
			return FWoTBootstrapStatus::Error(WoTReleaseStageError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("the staged tree contains an unsupported entry type"), Relative));
		}
	}
	return FWoTBootstrapStatus::Success();
}

FWoTBootstrapStatus WoTReleaseStageValidateExactTreeSnapshot(
	const FWoTNativeDirectoryHandle& RootHandle,
	const FWoTInstalledTreeManifest& Expected,
	const TArray<FWoTReleaseStageHeldFile>& Held,
	const TArray<FWoTReleaseStageDirectorySnapshot>& DirectorySnapshots,
	TFunctionRef<FWoTBootstrapStatus()> CheckCancellation)
{
	// Every final signed file name must still denote the exact held object whose
	// bytes and mode were hashed. Strict equality also catches an in-place change
	// between hashing and this final name check.
	for (const FWoTInstalledTreeEntry& Entry : Expected.Entries)
	{
		if (FWoTBootstrapStatus Cancelled =
				WoTReleaseStageCheckCancellation(CheckCancellation);
			Cancelled.IsError())
		{
			return Cancelled;
		}
		if (Entry.Type == EWoTTreeEntryType::SymbolicLink)
		{
			TWoTBootstrapResult<FWoTNativeDirectoryHandle> Parent =
				WoTReleaseStageOpenParent(RootHandle, Entry.Path);
			if (Parent.IsError())
			{
				return FWoTBootstrapStatus::Error(MoveTemp(Parent).GetError());
			}
			const FString Leaf = WoTReleaseStageLeaf(Entry.Path);
			TWoTBootstrapResult<FString> Target =
				ReadNativeSymbolicLinkAtNoFollow(Parent.GetValue(), Leaf);
			TWoTBootstrapResult<uint32> Mode =
				QueryNativeSymbolicLinkModeAtNoFollow(Parent.GetValue(), Leaf);
			if (Target.IsError() || Mode.IsError())
			{
				return FWoTBootstrapStatus::Error(Target.IsError()
					? MoveTemp(Target).GetError() : MoveTemp(Mode).GetError());
			}
			if (Target.GetValue() != Entry.LinkTarget || Mode.GetValue() != Entry.Mode)
			{
				return FWoTBootstrapStatus::Error(WoTReleaseStageError(
					EWoTBootstrapError::CorruptInstallation,
					TEXT("a final staged symbolic link changed after verification"),
					Entry.Path));
			}
			continue;
		}
		if (Entry.Type != EWoTTreeEntryType::File)
		{
			continue;
		}
		const FWoTReleaseStageHeldFile* Written =
			WoTReleaseStageFindHeld(Held, Entry.Path);
		if (Written == nullptr)
		{
			return FWoTBootstrapStatus::Error(WoTReleaseStageError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("a final staged file no longer has its extraction handle"),
				Entry.Path));
		}
		TWoTBootstrapResult<FWoTNativeFileIdentity> HeldNow =
			Written->Handle.QueryCurrentIdentity();
		if (HeldNow.IsError())
		{
			return FWoTBootstrapStatus::Error(MoveTemp(HeldNow).GetError());
		}
		TWoTBootstrapResult<FWoTNativeFileHandle> Named =
			OpenNativeFileAtReadNoFollow(RootHandle, Entry.Path);
		if (Named.IsError())
		{
			return FWoTBootstrapStatus::Error(MoveTemp(Named).GetError());
		}
		if (Written->Handle.GetIdentity() != HeldNow.GetValue()
			|| Named.GetValue().GetIdentity() != HeldNow.GetValue())
		{
			return FWoTBootstrapStatus::Error(WoTReleaseStageError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("a final staged file name or held object changed after hashing"),
				Entry.Path));
		}
	}

	// Directory identity is mutation-sensitive: additions, removals, and name
	// replacements change the held directory's strict identity. Re-query only
	// after every file hash/name check so a post-enumeration mutation cannot be
	// accepted as the earlier enumerated tree.
	for (const FWoTReleaseStageDirectorySnapshot& Snapshot : DirectorySnapshots)
	{
		if (FWoTBootstrapStatus Cancelled =
				WoTReleaseStageCheckCancellation(CheckCancellation);
			Cancelled.IsError())
		{
			return Cancelled;
		}
		TWoTBootstrapResult<FWoTNativeFileIdentity> AfterWalk =
			Snapshot.Handle.QueryCurrentIdentity();
		if (AfterWalk.IsError())
		{
			return FWoTBootstrapStatus::Error(MoveTemp(AfterWalk).GetError());
		}
		if (Snapshot.BeforeWalk != AfterWalk.GetValue())
		{
			// Name what moved: an intermittent root refusal on packaged runs
			// (about 1 in 25 stagings) arrived with no further detail.
			const FWoTNativeFileIdentity& Before = Snapshot.BeforeWalk;
			const FWoTNativeFileIdentity& After = AfterWalk.GetValue();
			return FWoTBootstrapStatus::Error(WoTReleaseStageError(
				EWoTBootstrapError::CorruptInstallation,
				FString::Printf(TEXT("a staged directory changed during exact-tree "
					"verification: identity change=%llu->%llu size=%llu->%llu "
					"links=%u->%u object=%s"),
					static_cast<unsigned long long>(Before.ChangeState),
					static_cast<unsigned long long>(After.ChangeState),
					static_cast<unsigned long long>(Before.Size),
					static_cast<unsigned long long>(After.Size),
					Before.LinkCount, After.LinkCount,
					Before.IsSameObject(After) ? TEXT("same") : TEXT("other")),
				Snapshot.RelativePath.IsEmpty() ? FString(TEXT("<root>"))
					: Snapshot.RelativePath));
		}
		// Identity is necessary but, on Windows, NOT sufficient: directory
		// ChangeTime advances on the system-timer tick, so a same-tick entry
		// addition leaves the strict identity unchanged (see the snapshot
		// struct's note). Re-enumerate through the held handle and require the
		// exact entry set the walk observed, name for name and type for type.
		TWoTBootstrapResult<TArray<FWoTNativeDirectoryEntry>> Relisted =
			EnumerateNativeDirectory(Snapshot.Handle);
		if (Relisted.IsError())
		{
			return FWoTBootstrapStatus::Error(MoveTemp(Relisted).GetError());
		}
		Relisted.GetValue().Sort([](const FWoTNativeDirectoryEntry& Left,
			const FWoTNativeDirectoryEntry& Right)
		{
			return Left.Name.Compare(Right.Name, ESearchCase::CaseSensitive) < 0;
		});
		bool bSameEntries = Relisted.GetValue().Num() == Snapshot.Entries.Num();
		for (int32 Index = 0; bSameEntries && Index < Snapshot.Entries.Num(); ++Index)
		{
			bSameEntries = Relisted.GetValue()[Index].Type
					== Snapshot.Entries[Index].Type
				&& Relisted.GetValue()[Index].Name.Equals(
					Snapshot.Entries[Index].Name, ESearchCase::CaseSensitive);
		}
		if (!bSameEntries)
		{
			FString Difference;
			for (const FWoTNativeDirectoryEntry& Now : Relisted.GetValue())
			{
				if (!Snapshot.Entries.ContainsByPredicate(
						[&Now](const FWoTNativeDirectoryEntry& Was)
						{
							return Was.Type == Now.Type && Was.Name.Equals(
								Now.Name, ESearchCase::CaseSensitive);
						}))
				{
					Difference += FString::Printf(TEXT(" +%s:%s"),
						LexToString(Now.Type), *Now.Name);
				}
			}
			for (const FWoTNativeDirectoryEntry& Was : Snapshot.Entries)
			{
				if (!Relisted.GetValue().ContainsByPredicate(
						[&Was](const FWoTNativeDirectoryEntry& Now)
						{
							return Now.Type == Was.Type && Now.Name.Equals(
								Was.Name, ESearchCase::CaseSensitive);
						}))
				{
					Difference += FString::Printf(TEXT(" -%s:%s"),
						LexToString(Was.Type), *Was.Name);
				}
			}
			return FWoTBootstrapStatus::Error(WoTReleaseStageError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("a staged directory changed during exact-tree verification:"
					" entries") + Difference,
				Snapshot.RelativePath.IsEmpty() ? FString(TEXT("<root>"))
					: Snapshot.RelativePath));
		}
	}
	return FWoTBootstrapStatus::Success();
}

TWoTBootstrapResult<FWoTEd25519Signature> WoTReleaseStageSignature(
	TConstArrayView<uint8> Bytes, FStringView What)
{
	using FResult = TWoTBootstrapResult<FWoTEd25519Signature>;
	if (Bytes.Num() != FWoTEd25519Signature::ByteCount)
	{
		return FResult::Error(WoTReleaseStageError(EWoTBootstrapError::Crypto,
			FString::Printf(TEXT("%.*s signature is not 64 bytes"),
				What.Len(), What.GetData())));
	}
	FWoTEd25519Signature Signature;
	FMemory::Memcpy(Signature.Bytes, Bytes.GetData(), FWoTEd25519Signature::ByteCount);
	return FResult::Success(Signature);
}

struct FWoTReleaseStageCleanup
{
	FWoTReleaseStageCleanup(const FWoTNativeDirectoryHandle* InParent,
		FString InLeaf, FWoTNativeDirectoryHandle&& InFresh,
		TFunctionRef<FWoTBootstrapStatus()> InCheckCancellation)
		: Parent(InParent)
		, Leaf(MoveTemp(InLeaf))
		, Fresh(MoveTemp(InFresh))
		, CheckCancellation(InCheckCancellation)
	{
	}

	const FWoTNativeDirectoryHandle* Parent = nullptr;
	FString Leaf;
	FWoTNativeDirectoryHandle Fresh;
	TArray<FWoTReleaseStageHeldDirectory> CreatedDirectories;
	TArray<FWoTReleaseStageHeldFile> CreatedFiles;
	TArray<FWoTReleaseStageHeldFile> HeldFiles;
	FWoTNativeDirectoryHandle StagedRoot;
	TArray<FWoTReleaseStageDirectorySnapshot> DirectorySnapshots;
	TFunctionRef<FWoTBootstrapStatus()> CheckCancellation;
	bool bArmed = true;

	FWoTBootstrapStatus RemoveChecked()
	{
		if (!bArmed)
		{
			return FWoTBootstrapStatus::Success();
		}
		if (FWoTBootstrapStatus Cancelled =
				WoTReleaseStageCheckCancellation([](){ return FWoTBootstrapStatus::Success(); });
			Cancelled.IsError())
		{
			return Cancelled;
		}
		TOptional<FWoTBootstrapError> FirstError;
		auto Remember = [&FirstError](FWoTBootstrapStatus&& Status)
		{
			if (!Status.IsError())
			{
				return;
			}
			FWoTBootstrapError Error = MoveTemp(Status).GetError();
			if (!FirstError.IsSet())
			{
				FirstError = MoveTemp(Error);
			}
			else
			{
				FirstError.GetValue().Message +=
					TEXT("; additional cleanup failure: ") + Error.ToString();
			}
		};

		// Widen every object through the handle retained from its creation before
		// closing any capability. This is what makes 0555/0000 signed directories
		// and Win64 read-only files removable on the refusal path.
		if (Fresh.IsValid())
		{
			Remember(SetNativeMode(Fresh, 0755u));
		}
		for (const FWoTReleaseStageHeldDirectory& Directory : CreatedDirectories)
		{
			Remember(SetNativeMode(Directory.Handle, 0755u));
		}
		for (const FWoTReleaseStageHeldFile& File : CreatedFiles)
		{
			TWoTBootstrapResult<FWoTNativeFileIdentity> Current =
				File.Handle.QueryCurrentIdentity();
			if (Current.IsError())
			{
				Remember(FWoTBootstrapStatus::Error(MoveTemp(Current).GetError()));
			}
			else if (Current.GetValue().LinkCount != 0)
			{
				Remember(SetNativeMode(File.Handle, 0644u));
			}
		}

		// Win64 does not remove a delete-pending directory until all sharing handles
		// close. The handles have done their widening job; release every duplicate
		// except Fresh before recursively emptying that held root.
		DirectorySnapshots.Reset();
		StagedRoot = FWoTNativeDirectoryHandle();
		HeldFiles.Reset();
		CreatedFiles.Reset();
		CreatedDirectories.Reset();

		if (!Fresh.IsValid())
		{
			Remember(FWoTBootstrapStatus::Error(WoTReleaseStageError(
				EWoTBootstrapError::Io,
				TEXT("the held fresh staging directory is unavailable for cleanup"),
				Leaf)));
		}
		else
		{
			Remember(WoTReleaseStageEmptyTree(Fresh, [](){ return FWoTBootstrapStatus::Success(); }));
		}
		if (!Fresh.IsValid())
		{
			return FWoTBootstrapStatus::Error(MoveTemp(FirstError.GetValue()));
		}

		// Do not remove a same-spelled directory supplied by a racing rename. The
		// parent name must still resolve to the exact fresh object held throughout.
		TWoTBootstrapResult<FWoTNativeDirectoryHandle> Named =
			OpenNativeDirectoryAtNoFollow(*Parent, Leaf);
		if (Named.IsError())
		{
			Remember(FWoTBootstrapStatus::Error(MoveTemp(Named).GetError()));
			return FWoTBootstrapStatus::Error(MoveTemp(FirstError.GetValue()));
		}
		FWoTNativeDirectoryHandle NamedHandle = MoveTemp(Named).GetValue();
		TWoTBootstrapResult<FWoTNativeFileIdentity> FreshNow =
			Fresh.QueryCurrentIdentity();
		if (FreshNow.IsError())
		{
			Remember(FWoTBootstrapStatus::Error(MoveTemp(FreshNow).GetError()));
			return FWoTBootstrapStatus::Error(MoveTemp(FirstError.GetValue()));
		}
		if (!FreshNow.GetValue().IsSameObject(NamedHandle.GetIdentity()))
		{
			Remember(FWoTBootstrapStatus::Error(WoTReleaseStageError(
				EWoTBootstrapError::UnsafePath,
				TEXT("the fresh staging name changed objects during refusal cleanup"),
				Leaf)));
			return FWoTBootstrapStatus::Error(MoveTemp(FirstError.GetValue()));
		}
		NamedHandle = FWoTNativeDirectoryHandle();
		Fresh = FWoTNativeDirectoryHandle();
		if (FWoTBootstrapStatus Removed = RemoveNativeEntryAtNoFollow(
				*Parent, Leaf, EWoTTreeEntryType::Directory); Removed.IsError())
		{
			Remember(MoveTemp(Removed));
		}
		else
		{
			bArmed = false;
		}
		if (FirstError.IsSet())
		{
			return FWoTBootstrapStatus::Error(MoveTemp(FirstError.GetValue()));
		}
		return FWoTBootstrapStatus::Success();
	}

	FWoTBootstrapError Refusal(FWoTBootstrapError&& Cause)
	{
		const FString CauseText = Cause.ToString();
		if (FWoTBootstrapStatus Removed = RemoveChecked(); Removed.IsError())
		{
			return WoTReleaseStageError(EWoTBootstrapError::Io,
				FString::Printf(
					TEXT("staging refusal cleanup failed after [%s]: %s"),
					*CauseText, *Removed.GetError().ToString()), Leaf);
		}
		return MoveTemp(Cause);
	}

	~FWoTReleaseStageCleanup()
	{
		if (bArmed && Parent != nullptr && Parent->IsValid())
		{
			// All normal refusal paths call Refusal() above. This is only a final
			// best-effort safety net for an unexpected control-flow escape.
			(void)RemoveChecked();
		}
	}
};
} // namespace

FWoTBootstrapStatus ProbeUpdaterInstallWritable(const FWoTUpdaterStateRoot& Root,
	TFunctionRef<FWoTBootstrapStatus()> CheckCancellation)
{
	if (FWoTBootstrapStatus Cancelled = CheckCancellation(); Cancelled.IsError())
	{
		return Cancelled;
	}
	TWoTBootstrapResult<FWoTUpdaterStateRoot> Resolved =
		ResolveUpdaterStateRoot(
			Root.UserStateRoot, Root.InstallRoot, Root.LiveLeaf);
	if (Resolved.IsError() || !WoTReleaseStageSameRoot(Resolved.GetValue(), Root))
	{
		return FWoTBootstrapStatus::Error(Resolved.IsError()
			? MoveTemp(Resolved).GetError()
			: WoTReleaseStageError(EWoTBootstrapError::UnsafePath,
				TEXT("the updater root is not canonical")));
	}
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> Install =
		OpenNativeDirectoryNoFollow(Root.InstallRoot);
	if (Install.IsError()) return FWoTBootstrapStatus::Error(MoveTemp(Install).GetError());
#if PLATFORM_WINDOWS
	const FString LiveTree = FPaths::Combine(Root.InstallRoot, Root.LiveLeaf);
	TWoTBootstrapResult<bool> LiveWritable =
		CheckNativeDirectoryMutationAccessAtNoFollow(
			Install.GetValue(), Root.LiveLeaf, CheckCancellation);
	if (LiveWritable.IsError())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(LiveWritable).GetError());
	}
	if (!LiveWritable.GetValue())
	{
		return FWoTBootstrapStatus::Error(WoTReleaseStageError(
			EWoTBootstrapError::ServiceUnavailable,
			TEXT("installed game tree is not writable"), LiveTree));
	}
	if (FWoTBootstrapStatus Cancelled = CheckCancellation(); Cancelled.IsError())
	{
		return Cancelled;
	}
#endif
	bool bCreatedUpdate = false;
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> Update =
		OpenOrCreateNativeDirectoryAtNoFollow(
			Install.GetValue(), TEXT(".wot-update"), bCreatedUpdate);
	if (Update.IsError())
	{
		return FWoTBootstrapStatus::Error(WoTReleaseStageError(
			EWoTBootstrapError::ServiceUnavailable,
			TEXT("install root is not writable"), Root.InstallRoot));
	}
	const auto RemoveProbeBounded = [&Update, &CheckCancellation](FStringView Leaf)
	{
		const double Deadline = FPlatformTime::Seconds() + 1.0;
		for (;;)
		{
			if (FWoTBootstrapStatus Cancelled = CheckCancellation();
				Cancelled.IsError())
			{
				return Cancelled;
			}
			FWoTBootstrapStatus Removed = RemoveNativeEntryAtNoFollow(
				Update.GetValue(), Leaf, EWoTTreeEntryType::File);
			if (!Removed.IsError() || FPlatformTime::Seconds() >= Deadline)
			{
				return Removed;
			}
			FPlatformProcess::SleepNoStats(0.05f);
		}
	};
	TWoTBootstrapResult<TArray<FWoTNativeDirectoryEntry>> Existing =
		EnumerateNativeDirectory(Update.GetValue());
	if (Existing.IsError())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(Existing).GetError());
	}
	for (const FWoTNativeDirectoryEntry& Entry : Existing.GetValue())
	{
		if (FWoTBootstrapStatus Cancelled = CheckCancellation();
			Cancelled.IsError())
		{
			return Cancelled;
		}
		if (Entry.Type == EWoTTreeEntryType::File
			&& Entry.Name.StartsWith(TEXT(".probe-"), ESearchCase::CaseSensitive))
		{
			if (FWoTBootstrapStatus Removed = RemoveProbeBounded(Entry.Name);
				Removed.IsError())
			{
				return FWoTBootstrapStatus::Error(WoTReleaseStageError(
					EWoTBootstrapError::ServiceUnavailable,
					TEXT("an orphaned install-root writability probe could not be removed"),
					Root.UpdateDir));
			}
		}
	}
	const FString ProbeLeaf = TEXT(".probe-")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
	if (FWoTBootstrapStatus Cancelled = CheckCancellation(); Cancelled.IsError())
	{
		return Cancelled;
	}
	TWoTBootstrapResult<FWoTNativeFileHandle> Probe =
		CreateNativeFileAtWriteNoFollow(Update.GetValue(), ProbeLeaf);
	if (Probe.IsError())
	{
		return FWoTBootstrapStatus::Error(WoTReleaseStageError(
			EWoTBootstrapError::ServiceUnavailable,
			TEXT("install root is not writable"), Root.UpdateDir));
	}
	Probe = TWoTBootstrapResult<FWoTNativeFileHandle>::Success(
		FWoTNativeFileHandle());
	if (FWoTBootstrapStatus Removed = RemoveProbeBounded(ProbeLeaf);
		Removed.IsError())
	{
		return FWoTBootstrapStatus::Error(WoTReleaseStageError(
			EWoTBootstrapError::ServiceUnavailable,
			TEXT("install-root writability probe could not be removed"),
			Root.UpdateDir));
	}
	return FWoTBootstrapStatus::Success();
}

namespace
{
TWoTBootstrapResult<TArray<uint8>> WoTReleaseStageReadAllCancellable(
	const FWoTNativeFileHandle& File,
	TFunctionRef<FWoTBootstrapStatus()> CheckCancellation)
{
	using FResult = TWoTBootstrapResult<TArray<uint8>>;
	const uint64 Size = File.GetIdentity().Size;
	if (Size > static_cast<uint64>(TNumericLimits<int32>::Max()))
	{
		return FResult::Error(WoTReleaseStageError(EWoTBootstrapError::SizeLimit,
			TEXT("a staged file exceeds the byte-array limit")));
	}
	TArray<uint8> Bytes;
	Bytes.SetNumUninitialized(static_cast<int32>(Size));
	constexpr int32 ChunkBytes = 1024 * 1024;
	uint64 Offset = 0;
	while (Offset < Size)
	{
		FWoTBootstrapStatus Cancelled =
			WoTReleaseStageCheckCancellation(CheckCancellation);
		if (Cancelled.IsError())
		{
			return FResult::Error(MoveTemp(Cancelled).GetError());
		}
		const int32 Count = static_cast<int32>(FMath::Min<uint64>(
			ChunkBytes, Size - Offset));
		TWoTBootstrapResult<uint64> Read = ReadNativeFileAt(
			File, Offset, MakeArrayView(Bytes.GetData() + Offset, Count));
		if (Read.IsError())
		{
			return FResult::Error(MoveTemp(Read).GetError());
		}
		if (Read.GetValue() == 0 || Read.GetValue() > static_cast<uint64>(Count))
		{
			return FResult::Error(WoTReleaseStageError(EWoTBootstrapError::Io,
				TEXT("a staged file ended during a chunked read")));
		}
		Offset += Read.GetValue();
	}
	return FResult::Success(MoveTemp(Bytes));
}

FWoTBootstrapStatus WoTReleaseStageWriteCancellable(
	FWoTNativeFileHandle& File, TConstArrayView<uint8> Bytes,
	TFunctionRef<FWoTBootstrapStatus()> CheckCancellation)
{
	if (FWoTBootstrapStatus Cancelled =
			WoTReleaseStageCheckCancellation(CheckCancellation);
		Cancelled.IsError())
	{
		return Cancelled;
	}
	if (FWoTBootstrapStatus Truncated = TruncateNativeFile(File, 0);
		Truncated.IsError())
	{
		return Truncated;
	}
	constexpr int32 ChunkBytes = 1024 * 1024;
	for (int32 Offset = 0; Offset < Bytes.Num(); Offset += ChunkBytes)
	{
		if (FWoTBootstrapStatus Cancelled =
				WoTReleaseStageCheckCancellation(CheckCancellation);
			Cancelled.IsError())
		{
			return Cancelled;
		}
		const int32 Count = FMath::Min(ChunkBytes, Bytes.Num() - Offset);
		if (FWoTBootstrapStatus Written = WriteNativeFileAt(
				File, static_cast<uint64>(Offset), Bytes.Slice(Offset, Count));
			Written.IsError())
		{
			return Written;
		}
	}
	return WoTReleaseStageCheckCancellation(CheckCancellation);
}
} // namespace

TWoTBootstrapResult<FWoTStagedRelease> StageRelease(
	const FWoTAnnouncementV1& Announcement, const FWoTKeyRing& TrustedRing,
	IWoTReleaseDownload& Download, const FWoTUpdaterStateRoot& Root,
	TFunctionRef<FWoTBootstrapStatus()> CheckCancellation)
{
	using FResult = TWoTBootstrapResult<FWoTStagedRelease>;
	if (FWoTBootstrapStatus Cancelled =
			WoTReleaseStageCheckCancellation(CheckCancellation);
		Cancelled.IsError())
	{
		return FResult::Error(MoveTemp(Cancelled).GetError());
	}
	TWoTBootstrapResult<FWoTUpdaterStateRoot> Resolved =
		ResolveUpdaterStateRoot(
			Root.UserStateRoot, Root.InstallRoot, Root.LiveLeaf);
	if (Resolved.IsError() || !WoTReleaseStageSameRoot(Resolved.GetValue(), Root))
	{
		return FResult::Error(Resolved.IsError()
			? MoveTemp(Resolved).GetError()
			: WoTReleaseStageError(EWoTBootstrapError::UnsafePath,
				TEXT("the updater root is not the canonical root for this install")));
	}

	TWoTBootstrapResult<FWoTNativeDirectoryHandle> Install =
		OpenNativeDirectoryNoFollow(Root.InstallRoot);
	if (Install.IsError()) return FResult::Error(MoveTemp(Install).GetError());
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> Update =
		OpenNativeDirectoryAtNoFollow(Install.GetValue(), TEXT(".wot-update"));
	if (Update.IsError()) return FResult::Error(MoveTemp(Update).GetError());

	FWoTReleaseStagingMetadata Metadata;
	if (FWoTBootstrapStatus Fetched = Download.FetchSignedMetadata(Metadata);
		Fetched.IsError())
	{
		return FResult::Error(MoveTemp(Fetched).GetError());
	}
	if (static_cast<uint64>(Metadata.ReleaseManifestBytes.Num())
			!= Announcement.Manifest.Size
		|| Sha256(Metadata.ReleaseManifestBytes) != Announcement.Manifest.Sha256)
	{
		return FResult::Error(WoTReleaseStageError(EWoTBootstrapError::Crypto,
			TEXT("the inner manifest bytes do not match the authenticated announcement")));
	}
	TWoTBootstrapResult<FWoTEd25519Signature> ManifestSignature =
		WoTReleaseStageSignature(Metadata.ReleaseManifestSignature,
			TEXT("release manifest"));
	if (ManifestSignature.IsError())
	{
		return FResult::Error(MoveTemp(ManifestSignature).GetError());
	}
	TWoTBootstrapResult<FWoTReleaseManifestV2> VerifiedManifest =
		VerifyReleaseManifestV2(Metadata.ReleaseManifestBytes,
			ManifestSignature.GetValue(), Announcement, TrustedRing);
	if (VerifiedManifest.IsError())
	{
		return FResult::Error(MoveTemp(VerifiedManifest).GetError());
	}
	FWoTReleaseManifestV2 Manifest = MoveTemp(VerifiedManifest).GetValue();
	if (!SupportsManifestProtocols(Manifest))
	{
		return FResult::Error(WoTReleaseStageError(EWoTBootstrapError::UnsupportedProtocol,
			TEXT("the release manifest requires a newer updater protocol")));
	}

	auto Failed = ReadUpdateFailedSequence(Root);
	if (Failed.IsError()) return FResult::Error(MoveTemp(Failed).GetError());
	if (Failed.GetValue() == Announcement.ReleaseSequence)
		return FResult::Error(WoTReleaseStageError(EWoTBootstrapError::Rollback, TEXT("this release failed to start")));
	if (auto Cleared = ClearUpdateFailedSequenceForNewerRelease(Root, Announcement.ReleaseSequence); Cleared.IsError())
		return FResult::Error(MoveTemp(Cleared).GetError());
	auto AlreadyReady = ReadWoTUpdateMarker(Root, TEXT("ready"));
	if (AlreadyReady.IsError()) return FResult::Error(MoveTemp(AlreadyReady).GetError());
	if (AlreadyReady.GetValue().IsSet())
		return FResult::Success(FWoTStagedRelease{AlreadyReady.GetValue()->Sequence, Root.StagedTreeDir, {}});
	// Each process owns only staging-<pid>; no exclusive profile hold is taken.
	auto Existing = EnumerateNativeDirectory(Update.GetValue());
	if (Existing.IsError()) return FResult::Error(MoveTemp(Existing).GetError());
	for (const auto& Entry : Existing.GetValue())
	{
		if (Entry.Type == EWoTTreeEntryType::Directory && Entry.Name.StartsWith(TEXT("staging-"))
			&& IFileManager::Get().GetTimeStamp(*FPaths::Combine(Root.UpdateDir, Entry.Name))
				< FDateTime::UtcNow() - FTimespan::FromHours(24))
			(void)RemoveWoTUpdateEntry(Update.GetValue(), Entry.Name);
	}
	const FString FreshLeaf = FPaths::GetCleanFilename(Root.WorkDir);
	if (auto Removed = RemoveWoTUpdateEntry(Update.GetValue(), FreshLeaf); Removed.IsError())
		return FResult::Error(MoveTemp(Removed).GetError());
	auto FreshCreated = CreateNativeDirectoryAtNoFollow(Update.GetValue(), FreshLeaf);
	if (FreshCreated.IsError()) return FResult::Error(MoveTemp(FreshCreated).GetError());
	FWoTReleaseStageCleanup Cleanup(&Update.GetValue(), FreshLeaf,
		MoveTemp(FreshCreated).GetValue(), CheckCancellation);
	auto Refuse = [&Cleanup](FWoTBootstrapError&& Error)
	{
		return FResult::Error(Cleanup.Refusal(MoveTemp(Error)));
	};
	auto Cancel = [&Refuse](FWoTBootstrapError&& Error) { return Refuse(MoveTemp(Error)); };
	auto Downloaded = WoTReleaseStageDownloadArchive(Download, Manifest.Archive, Cleanup.Fresh, CheckCancellation);
	if (Downloaded.IsError()) return Refuse(MoveTemp(Downloaded).GetError());
	if (auto Checkpoint = Download.BeforeArchiveVerification(Root.DownloadPath); Checkpoint.IsError())
		return Refuse(MoveTemp(Checkpoint).GetError());

	// Fixed order step 2: the complete held archive, never a partial member.
	TWoTBootstrapResult<FWoTNativeFileHandle> ArchiveOpened =
		OpenNativeFileAtReadNoFollow(Cleanup.Fresh, TEXT("download.zip"));
	if (ArchiveOpened.IsError())
	{
		return FResult::Error(MoveTemp(ArchiveOpened).GetError());
	}
	FWoTNativeFileHandle ArchiveHandle = MoveTemp(ArchiveOpened).GetValue();
	const FWoTNativeFileIdentity ArchiveBefore = ArchiveHandle.GetIdentity();
	if (ArchiveBefore.Size != Manifest.Archive.Size)
	{
		if (FWoTBootstrapStatus Cancelled =
				WoTReleaseStageCheckCancellation(CheckCancellation);
			Cancelled.IsError())
		{
			return FResult::Error(MoveTemp(Cancelled).GetError());
		}
		ArchiveHandle = FWoTNativeFileHandle();
		return FResult::Error(WoTReleaseStageError(EWoTBootstrapError::Crypto,
			TEXT("the complete archive does not have its signed size")));
	}
	TWoTBootstrapResult<FWoTSha256> ArchiveDigest =
		WoTReleaseStageHashFileYielding(
			ArchiveHandle, Manifest.Archive.Size, CheckCancellation);
	TWoTBootstrapResult<FWoTNativeFileIdentity> ArchiveAfterHash =
		ArchiveHandle.QueryCurrentIdentity();
	if (ArchiveDigest.IsError() || ArchiveAfterHash.IsError())
	{
		return FResult::Error(ArchiveDigest.IsError()
			? MoveTemp(ArchiveDigest).GetError()
			: MoveTemp(ArchiveAfterHash).GetError());
	}
	if (ArchiveBefore != ArchiveAfterHash.GetValue())
	{
		return FResult::Error(WoTReleaseStageError(EWoTBootstrapError::Crypto,
			TEXT("the complete archive does not match the signed digest and size")));
	}
	if (ArchiveDigest.GetValue() != Manifest.Archive.Sha256)
	{
		if (FWoTBootstrapStatus Cancelled =
				WoTReleaseStageCheckCancellation(CheckCancellation);
			Cancelled.IsError())
		{
			return FResult::Error(MoveTemp(Cancelled).GetError());
		}
		ArchiveHandle = FWoTNativeFileHandle();
		return FResult::Error(WoTReleaseStageError(EWoTBootstrapError::Crypto,
			TEXT("the complete archive does not match the signed digest and size")));
	}
	TWoTBootstrapResult<TArray<uint8>> ArchiveRead =
		WoTReleaseStageReadAllCancellable(ArchiveHandle, CheckCancellation);
	TWoTBootstrapResult<FWoTNativeFileIdentity> ArchiveAfter =
		ArchiveHandle.QueryCurrentIdentity();
	if (ArchiveRead.IsError() || ArchiveAfter.IsError())
	{
		return FResult::Error(ArchiveRead.IsError()
			? MoveTemp(ArchiveRead).GetError() : MoveTemp(ArchiveAfter).GetError());
	}
	TArray<uint8> Archive = MoveTemp(ArchiveRead).GetValue();
	if (ArchiveBefore != ArchiveAfter.GetValue()
		|| static_cast<uint64>(Archive.Num()) != Manifest.Archive.Size)
	{
		return FResult::Error(WoTReleaseStageError(EWoTBootstrapError::Crypto,
			TEXT("the complete archive does not match the signed digest and size")));
	}

	// Fixed order step 3: parse central+local facts, then validate the whole index.
	TWoTBootstrapResult<TArray<FWoTReleaseStageZipEntry>> ParsedZip =
		WoTReleaseStageParseZip(Archive, CheckCancellation);
	if (ParsedZip.IsError())
	{
		return FResult::Error(MoveTemp(ParsedZip).GetError());
	}
	TArray<FWoTArchiveEntry> Index;
	for (FWoTReleaseStageZipEntry& Entry : ParsedZip.GetValue())
	{
		FWoTBootstrapStatus Cancelled =
			WoTReleaseStageCheckCancellation(CheckCancellation);
		if (Cancelled.IsError())
		{
			return FResult::Error(MoveTemp(Cancelled).GetError());
		}
		if (Entry.Rule.Type == EWoTTreeEntryType::SymbolicLink)
		{
			TWoTBootstrapResult<TArray<uint8>> LinkBytes =
				WoTReleaseStageInflate(Archive, Entry, CheckCancellation);
			if (LinkBytes.IsError()) return FResult::Error(MoveTemp(LinkBytes).GetError());
			TWoTBootstrapResult<FString> LinkTarget =
				WoTReleaseStageUtf8(LinkBytes.GetValue(), Entry.Rule.Path);
			if (LinkTarget.IsError()) return FResult::Error(MoveTemp(LinkTarget).GetError());
			Entry.Rule.LinkTarget = MoveTemp(LinkTarget).GetValue();
		}
		Index.Add(Entry.Rule);
	}
	const FWoTArchiveLimits Limits;
#if PLATFORM_WINDOWS
	constexpr EWoTArchiveLayout Layout = EWoTArchiveLayout::Win64VersionTree;
#elif PLATFORM_MAC
	constexpr EWoTArchiveLayout Layout = EWoTArchiveLayout::MacAppBundle;
#else
#error "release staging is supported only on Win64 and Mac"
#endif
	TWoTBootstrapResult<FWoTSafeArchivePlan> Safe =
		ValidateArchiveIndex(Index, Limits, Layout);
	if (Safe.IsError())
	{
		return FResult::Error(MoveTemp(Safe).GetError());
	}
	if (Safe.GetValue().RootName != Manifest.InstalledTreeRoot.Name)
	{
		return FResult::Error(WoTReleaseStageError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the archive root does not match the signed installed-tree name")));
	}
	if (Safe.GetValue().RootName != WoTReleaseStageLiveLeaf)
	{
		return FResult::Error(WoTReleaseStageError(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("the archive root does not equal the canonical live tree leaf"),
			Safe.GetValue().RootName));
	}
	TSet<FString> ArchiveDirectories;
	for (const FWoTReleaseStageZipEntry& Entry : ParsedZip.GetValue())
	{
		FWoTBootstrapStatus Cancelled =
			WoTReleaseStageCheckCancellation(CheckCancellation);
		if (Cancelled.IsError())
		{
			return FResult::Error(MoveTemp(Cancelled).GetError());
		}
		if (Entry.Rule.Type == EWoTTreeEntryType::Directory)
		{
			ArchiveDirectories.Add(Entry.Rule.Path);
		}
	}
	for (const FWoTReleaseStageZipEntry& Entry : ParsedZip.GetValue())
	{
		FWoTBootstrapStatus Cancelled =
			WoTReleaseStageCheckCancellation(CheckCancellation);
		if (Cancelled.IsError())
		{
			return FResult::Error(MoveTemp(Cancelled).GetError());
		}
		const FString Parent = WoTReleaseStageParentPath(Entry.Rule.Path);
		if (!Parent.IsEmpty() && !ArchiveDirectories.Contains(Parent))
		{
			return FResult::Error(WoTReleaseStageError(EWoTBootstrapError::UnsafePath,
				TEXT("a ZIP member has no explicit parent directory"), Entry.Rule.Path));
		}
	}

	TArray<const FWoTReleaseStageZipEntry*> Directories;
	TArray<const FWoTReleaseStageZipEntry*> Files;
	for (const FWoTReleaseStageZipEntry& Entry : ParsedZip.GetValue())
	{
		(Entry.Rule.Type == EWoTTreeEntryType::Directory ? Directories : Files).Add(&Entry);
	}
	const FString InventoryArchivePath = Safe.GetValue().RootName
		+ TEXT("/installed-tree-v1.json");
	const FString InventorySignatureArchivePath = Safe.GetValue().RootName
		+ TEXT("/installed-tree-v1.json.sig");
	TMap<FString, TArray<uint8>> RootVerificationPayloads;
	Directories.Sort([](const FWoTReleaseStageZipEntry& Left,
		const FWoTReleaseStageZipEntry& Right)
	{
		const int32 LeftDepth = WoTReleaseStageDepth(Left.Rule.Path);
		const int32 RightDepth = WoTReleaseStageDepth(Right.Rule.Path);
		return LeftDepth != RightDepth ? LeftDepth < RightDepth
			: Left.Rule.Path.Compare(Right.Rule.Path, ESearchCase::CaseSensitive) < 0;
	});
	for (const FWoTReleaseStageZipEntry* Entry : Directories)
	{
		FWoTBootstrapStatus Cancelled =
			WoTReleaseStageCheckCancellation(CheckCancellation);
		if (Cancelled.IsError())
		{
			return Cancel(MoveTemp(Cancelled).GetError());
		}
		// Fresh has changed since creation (download.zip). Use that held parent
		// directly; DuplicateForRead requires an unchanged metadata snapshot.
		auto Created = [&]() -> TWoTBootstrapResult<FWoTNativeDirectoryHandle>
		{
			if (WoTReleaseStageParentPath(Entry->Rule.Path).IsEmpty())
				return CreateNativeDirectoryAtNoFollow(Cleanup.Fresh, Entry->Rule.Path);
			auto Parent = WoTReleaseStageOpenParent(Cleanup.Fresh, Entry->Rule.Path);
			if (Parent.IsError()) return TWoTBootstrapResult<FWoTNativeDirectoryHandle>::Error(MoveTemp(Parent).GetError());
			return CreateNativeDirectoryAtNoFollow(Parent.GetValue(), WoTReleaseStageLeaf(Entry->Rule.Path));
		}();
		if (Created.IsError()) return Refuse(MoveTemp(Created).GetError());
		FWoTReleaseStageHeldDirectory Held;
		Held.RelativePath = Entry->Rule.Path;
		Held.Handle = MoveTemp(Created).GetValue();
		Cleanup.CreatedDirectories.Add(MoveTemp(Held));
	}
	for (const FWoTReleaseStageZipEntry* Entry : Files)
	{
		FWoTBootstrapStatus Cancelled =
			WoTReleaseStageCheckCancellation(CheckCancellation);
		if (Cancelled.IsError())
		{
			return Cancel(MoveTemp(Cancelled).GetError());
		}
		TWoTBootstrapResult<TArray<uint8>> Bytes =
			WoTReleaseStageInflate(Archive, *Entry, CheckCancellation);
		if (Bytes.IsError()
			&& CheckCancellation().IsError())
		{
			return Cancel(MoveTemp(Bytes).GetError());
		}
		if (Bytes.IsError()) return Refuse(MoveTemp(Bytes).GetError());
		if (Entry->Rule.Type == EWoTTreeEntryType::File
			&& (Entry->Rule.Path == InventoryArchivePath
				|| Entry->Rule.Path == InventorySignatureArchivePath))
		{
			RootVerificationPayloads.Add(
				Entry->Rule.Path, MoveTemp(Bytes).GetValue());
			if (FWoTBootstrapStatus Checked =
					WoTReleaseStageCheckCancellation(CheckCancellation);
				Checked.IsError())
			{
				return Cancel(MoveTemp(Checked).GetError());
			}
			continue;
		}
		TWoTBootstrapResult<FWoTNativeDirectoryHandle> Parent =
			WoTReleaseStageOpenParent(Cleanup.Fresh, Entry->Rule.Path);
		if (Parent.IsError()) return Refuse(MoveTemp(Parent).GetError());
		if (Entry->Rule.Type == EWoTTreeEntryType::SymbolicLink)
		{
			if (FWoTBootstrapStatus Created = CreateNativeSymbolicLinkAtNoFollow(
					Parent.GetValue(), WoTReleaseStageLeaf(Entry->Rule.Path),
					Entry->Rule.LinkTarget); Created.IsError())
			{
				return Refuse(MoveTemp(Created).GetError());
			}
			TWoTBootstrapResult<uint32> Mode = QueryNativeSymbolicLinkModeAtNoFollow(
				Parent.GetValue(), WoTReleaseStageLeaf(Entry->Rule.Path));
			if (Mode.IsError()) return Refuse(MoveTemp(Mode).GetError());
			if (Mode.GetValue() != Entry->Rule.Mode)
			{
				return Refuse(WoTReleaseStageError(
					EWoTBootstrapError::CorruptInstallation,
					TEXT("the extracted symbolic-link mode differs from the signed mode"),
					Entry->Rule.Path));
			}
			continue;
		}
		TWoTBootstrapResult<FWoTNativeFileHandle> Created =
			CreateNativeFileAtWriteNoFollow(
				Parent.GetValue(), WoTReleaseStageLeaf(Entry->Rule.Path));
		if (Created.IsError()) return Refuse(MoveTemp(Created).GetError());
		FWoTReleaseStageHeldFile CreatedFile;
		CreatedFile.RelativePath = Entry->Rule.Path;
		CreatedFile.Handle = MoveTemp(Created).GetValue();
		Cleanup.CreatedFiles.Add(MoveTemp(CreatedFile));
		FWoTNativeFileHandle& File = Cleanup.CreatedFiles.Last().Handle;
		if (FWoTBootstrapStatus Written = WoTReleaseStageWriteCancellable(
				File, Bytes.GetValue(), CheckCancellation); Written.IsError())
		{
			if (CheckCancellation().IsError())
			{
				return Cancel(MoveTemp(Written).GetError());
			}
			return Refuse(MoveTemp(Written).GetError());
		}
		if (FWoTBootstrapStatus Mode = SetNativeMode(File, Entry->Rule.Mode);
			Mode.IsError()) return Refuse(MoveTemp(Mode).GetError());
		if (FWoTBootstrapStatus Flushed = WoTReleaseStageFlushFile(File);
			Flushed.IsError()) return Refuse(MoveTemp(Flushed).GetError());
		if (FWoTBootstrapStatus Checked =
				WoTReleaseStageCheckCancellation(CheckCancellation);
			Checked.IsError())
		{
			return Cancel(MoveTemp(Checked).GetError());
		}
	}
	const FString StagedTreeDir = FPaths::Combine(
		Root.WorkDir, Safe.GetValue().RootName);
	if (FWoTBootstrapStatus Checkpoint =
			Download.AfterArchiveExtracted(StagedTreeDir); Checkpoint.IsError())
	{
		return Refuse(MoveTemp(Checkpoint).GetError());
	}
	Directories.Sort([](const FWoTReleaseStageZipEntry& Left,
		const FWoTReleaseStageZipEntry& Right)
	{
		return WoTReleaseStageDepth(Left.Rule.Path)
			> WoTReleaseStageDepth(Right.Rule.Path);
	});
	for (const FWoTReleaseStageZipEntry* Entry : Directories)
	{
		if (FWoTBootstrapStatus Checked =
				WoTReleaseStageCheckCancellation(CheckCancellation);
			Checked.IsError())
		{
			return Cancel(MoveTemp(Checked).GetError());
		}
		const FWoTReleaseStageHeldDirectory* Directory =
			WoTReleaseStageFindHeldDirectory(
				Cleanup.CreatedDirectories, Entry->Rule.Path);
		if (Directory == nullptr)
		{
			return Refuse(WoTReleaseStageError(EWoTBootstrapError::CorruptInstallation,
				TEXT("a created staging directory lost its retained handle"),
				Entry->Rule.Path));
		}
		if (FWoTBootstrapStatus Mode = SetNativeMode(
				Directory->Handle, Entry->Rule.Mode); Mode.IsError())
		{
			return Refuse(MoveTemp(Mode).GetError());
		}
		if (FWoTBootstrapStatus Flushed =
			WoTReleaseStageFlushDirectory(Directory->Handle);
			Flushed.IsError()) return Refuse(MoveTemp(Flushed).GetError());
		if (FWoTBootstrapStatus Checked =
				WoTReleaseStageCheckCancellation(CheckCancellation);
			Checked.IsError())
		{
			return Cancel(MoveTemp(Checked).GetError());
		}
	}

	TWoTBootstrapResult<FWoTNativeDirectoryHandle> RootOpened =
		OpenNativeDirectoryAtNoFollow(Cleanup.Fresh, Safe.GetValue().RootName);
	if (RootOpened.IsError()) return Refuse(MoveTemp(RootOpened).GetError());
	Cleanup.StagedRoot = MoveTemp(RootOpened).GetValue();
	for (const FWoTReleaseStageZipEntry* Entry : Files)
	{
		if (FWoTBootstrapStatus Checked =
				WoTReleaseStageCheckCancellation(CheckCancellation);
			Checked.IsError())
		{
			return Cancel(MoveTemp(Checked).GetError());
		}
		if (Entry->Rule.Type != EWoTTreeEntryType::File) continue;
		if (Entry->Rule.Path == InventoryArchivePath
			|| Entry->Rule.Path == InventorySignatureArchivePath) continue;
		const FString Prefix = Safe.GetValue().RootName + TEXT("/");
		if (!Entry->Rule.Path.StartsWith(Prefix, ESearchCase::CaseSensitive)) continue;
		FWoTReleaseStageHeldFile Held;
		Held.RelativePath = Entry->Rule.Path.Mid(Prefix.Len());
		TWoTBootstrapResult<FWoTNativeFileHandle> Reopened =
			OpenNativeFileAtReadNoFollow(Cleanup.StagedRoot, Held.RelativePath);
		if (Reopened.IsError()) return Refuse(MoveTemp(Reopened).GetError());
		Held.Handle = MoveTemp(Reopened).GetValue();
		Cleanup.HeldFiles.Add(MoveTemp(Held));
	}

	const TArray<uint8>* InventoryBytes =
		RootVerificationPayloads.Find(InventoryArchivePath);
	if (InventoryBytes == nullptr)
	{
		return Refuse(WoTReleaseStageError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the archive does not contain installed-tree-v1.json")));
	}
	if (static_cast<uint64>(InventoryBytes->Num())
		!= Manifest.InstalledTreeRoot.Size)
	{
		return Refuse(WoTReleaseStageError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the installed-tree inventory has the wrong signed size")));
	}
	TWoTBootstrapResult<FWoTInstalledTreeManifest> ParsedInventory =
		ParseInstalledTreeV1(*InventoryBytes);
	if (ParsedInventory.IsError())
	{
		return Refuse(MoveTemp(ParsedInventory).GetError());
	}
	FWoTInstalledTreeManifest Inventory = MoveTemp(ParsedInventory).GetValue();
	if (Inventory.Platform != Manifest.Platform)
	{
		return Refuse(WoTReleaseStageError(EWoTBootstrapError::UnsupportedProtocol,
			TEXT("the installed-tree inventory is for another platform")));
	}

	if (FWoTBootstrapStatus Checkpoint =
			Download.BeforeInstalledTreeVerification(StagedTreeDir);
		Checkpoint.IsError())
	{
		return Refuse(MoveTemp(Checkpoint).GetError());
	}

	// Fixed order steps 5 and 6: held-object rehash, exact enumeration, then root.
	FWoTInstalledTreeManifest Actual;
	Actual.Format = Inventory.Format;
	Actual.Version = Inventory.Version;
	Actual.Platform = Inventory.Platform;
	if (FWoTBootstrapStatus Walked = WoTReleaseStageWalkExactTree(
			Cleanup.StagedRoot, FStringView(), Inventory, Cleanup.HeldFiles,
			Actual, Cleanup.DirectorySnapshots, CheckCancellation);
		Walked.IsError())
	{
		if (CheckCancellation().IsError())
		{
			return Cancel(MoveTemp(Walked).GetError());
		}
		return Refuse(MoveTemp(Walked).GetError());
	}
	if (FWoTBootstrapStatus Checkpoint =
			Download.BeforeInstalledTreeSnapshotValidation(StagedTreeDir);
		Checkpoint.IsError())
	{
		return Refuse(MoveTemp(Checkpoint).GetError());
	}
	if (FWoTBootstrapStatus Stable = WoTReleaseStageValidateExactTreeSnapshot(
			Cleanup.StagedRoot, Inventory, Cleanup.HeldFiles,
			Cleanup.DirectorySnapshots, CheckCancellation); Stable.IsError())
	{
		if (CheckCancellation().IsError())
		{
			return Cancel(MoveTemp(Stable).GetError());
		}
		return Refuse(MoveTemp(Stable).GetError());
	}
	Actual.Entries.Sort([](const FWoTInstalledTreeEntry& Left,
		const FWoTInstalledTreeEntry& Right)
	{
		return Left.Path.Compare(Right.Path, ESearchCase::CaseSensitive) < 0;
	});
	if (Actual.Entries.Num() != Inventory.Entries.Num())
	{
		return Refuse(WoTReleaseStageError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the staged tree is missing one or more signed entries")));
	}
	TWoTBootstrapResult<FWoTSha256> RootDigest = ComputeInstalledTreeRoot(Actual);
	if (RootDigest.IsError()) return Refuse(MoveTemp(RootDigest).GetError());
	if (RootDigest.GetValue() != Manifest.InstalledTreeRoot.Sha256)
	{
		return Refuse(WoTReleaseStageError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the rehashed staged-tree root differs from the signed manifest")));
	}

	const FWoTInstalledTreeEntry* Main =
		WoTReleaseStageFindExpected(Actual, Manifest.MainExecutable.Name);
	const FWoTReleaseStageHeldFile* MainHeld =
		WoTReleaseStageFindHeld(Cleanup.HeldFiles, Manifest.MainExecutable.Name);
	if (Main == nullptr || MainHeld == nullptr
		|| Main->Type != EWoTTreeEntryType::File
		|| Main->Size != Manifest.MainExecutable.Size
		|| Main->Sha256 != Manifest.MainExecutable.Sha256)
	{
		return Refuse(WoTReleaseStageError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the signed executable components do not match the closed tree")));
	}

	if (auto Seal = WoTReleaseStageVerifyCodeSeal(StagedTreeDir); Seal.IsError())
		return Refuse(MoveTemp(Seal).GetError());
	if (auto Checked = CheckCancellation(); Checked.IsError()) return Cancel(MoveTemp(Checked).GetError());
	const FWoTUpdateStateRecordReadResult ObservedState =
		ReadWoTUpdateState(Root);
	if (Announcement.ReleaseSequence
		< ObservedState.State.NewestAnnouncementSequence)
	{
		return Refuse(WoTReleaseStageError(EWoTBootstrapError::Rollback,
			TEXT("a newer authenticated announcement was observed before publication")));
	}
	if (FWoTBootstrapStatus CurrentRing = RequireRingEpochAtLeast(
			TrustedRing, ObservedState.State.KeyRevocationFloor);
		CurrentRing.IsError())
	{
		return Refuse(MoveTemp(CurrentRing).GetError());
	}
	bool bCreated = false;
	auto Published = OpenOrCreateNativeDirectoryAtNoFollow(Update.GetValue(), TEXT("staged"), bCreated);
	if (Published.IsError()) return Refuse(MoveTemp(Published).GetError());
	// Verification is complete. Close descendant handles before the directory rename.
	Cleanup.DirectorySnapshots.Reset();
	Cleanup.StagedRoot = FWoTNativeDirectoryHandle();
	Cleanup.HeldFiles.Reset();
	Cleanup.CreatedFiles.Reset();
	Cleanup.CreatedDirectories.Reset();
	auto Moved = RenameNativeEntryAtNoFollow(Cleanup.Fresh, Safe.GetValue().RootName,
		Published.GetValue(), Root.LiveLeaf, false);
#if PLATFORM_WINDOWS
	// Access denied here means another program still held a file inside the
	// staged tree after the rename's own retries; the fallback below would only
	// repeat that wait. (A taken destination reports a collision, not this.)
	if (Moved.IsError() && Moved.GetError().NativeCode == 5u)
		return Refuse(MoveTemp(Moved).GetError());
#endif
	if (Moved.IsError())
	{
		auto Ready = ReadWoTUpdateMarker(Root, TEXT("ready"));
		if (Ready.IsError()) return Refuse(MoveTemp(Ready).GetError());
		if (Ready.GetValue().IsSet())
			return FResult::Success(FWoTStagedRelease{Ready.GetValue()->Sequence, Root.StagedTreeDir, {}});
		if (auto Removed = RemoveWoTUpdateEntry(Published.GetValue(), Root.LiveLeaf); Removed.IsError())
			return Refuse(MoveTemp(Removed).GetError());
		Moved = RenameNativeEntryAtNoFollow(Cleanup.Fresh, Safe.GetValue().RootName,
			Published.GetValue(), Root.LiveLeaf, false);
		if (Moved.IsError()) return Refuse(MoveTemp(Moved).GetError());
	}
	auto Ready = WriteWoTUpdateMarker(Root, TEXT("ready"), {Announcement.ReleaseSequence});
	if (Ready.IsError())
	{
		(void)RemoveWoTUpdateEntry(Published.GetValue(), Root.LiveLeaf);
		(void)RemoveWoTUpdateEntry(Update.GetValue(), TEXT("ready"));
		return Refuse(MoveTemp(Ready).GetError());
	}
	return FResult::Success(FWoTStagedRelease{Announcement.ReleaseSequence, Root.StagedTreeDir, RootDigest.GetValue()});
}

#if WITH_AUTOMATION_TESTS
FWoTReleaseStagingCodeSealVerifierOverrideForTesting::
	FWoTReleaseStagingCodeSealVerifierOverrideForTesting(
		FWoTReleaseStagingCodeSealVerifierForTesting InVerifier)
	: Previous(MoveTemp(GWoTReleaseStagingCodeSealVerifierForTesting))
{
	GWoTReleaseStagingCodeSealVerifierForTesting = MoveTemp(InVerifier);
}

FWoTReleaseStagingCodeSealVerifierOverrideForTesting::
	~FWoTReleaseStagingCodeSealVerifierOverrideForTesting()
{
	GWoTReleaseStagingCodeSealVerifierForTesting = MoveTemp(Previous);
}

TWoTBootstrapResult<FWoTSha256> WoTStagingHashWithYieldForTesting(
	TConstArrayView<uint8> Bytes,
	TFunctionRef<FWoTBootstrapStatus()> CheckCancellation)
{
	return WoTReleaseStageHashYielding(Bytes, CheckCancellation);
}

void WoTSetStagingArchiveReadFailureForTesting(bool bFail)
{
	GWoTReleaseStageFailArchiveReadForTesting.Store(bFail);
}
#endif
