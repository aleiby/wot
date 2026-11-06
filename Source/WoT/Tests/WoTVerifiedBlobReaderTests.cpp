#include "Misc/AutomationTest.h"

#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "../Payload/WoTCacheStore.h"
#include "WoTCacheTestSupport.h"
#include "../Payload/WoTVerifiedBlobReader.h"
#include "WoTBootstrapCore/Hash.h"
#include "WoTBootstrapCore/ProfileSession.h"
#include "WoTRetailReader/CacheManifest.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#elif PLATFORM_MAC
#include <sys/stat.h>
#include <unistd.h>
#endif

#if WITH_AUTOMATION_TESTS

constexpr EAutomationTestFlags WoTVerifiedBlobSpecFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

struct FWoTVerifiedBlobSpecRoot
{
	FWoTVerifiedBlobSpecRoot()
	{
		Path = FPaths::Combine(FPaths::AutomationTransientDir(),
			TEXT("wot-verified-blob-")
				+ FGuid::NewGuid().ToString(EGuidFormats::Digits));
		Path = FPaths::Combine(FPaths::ConvertRelativePathToFull(Path),
			TEXT("ContentCache"));
		FPaths::NormalizeDirectoryName(Path);
		IFileManager::Get().MakeDirectory(*Path, true);
	}
	~FWoTVerifiedBlobSpecRoot()
	{
		IFileManager::Get().DeleteDirectory(*FPaths::GetPath(Path), false, true);
	}
	FString Path;
};

TArray<uint8> WoTVerifiedBlobSpecBytes(const ANSICHAR* Text)
{
	TArray<uint8> Bytes;
	while (*Text != '\0') Bytes.Add(static_cast<uint8>(*Text++));
	return Bytes;
}

FString WoTVerifiedBlobSpecFileName(FStringView LogicalId)
{
	const FString LogicalString(LogicalId);
	const FTCHARToUTF8 Utf8(*LogicalString);
	return Sha256(TConstArrayView<uint8>(reinterpret_cast<const uint8*>(Utf8.Get()),
		Utf8.Length())).ToHex() + TEXT(".blob");
}

// Overwrites a blob WHILE THE READER HOLDS IT, which is the whole point of the
// TOCTOU fixtures below. FFileHelper cannot be that writer on Windows: its
// OpenWrite requests a zero share mode, which conflicts with the reader's
// outstanding handle and fails with a sharing violation -- the "tamper" then
// silently never happens and the spec fails asserting that it did. Windows
// share modes are symmetric, so the writer must offer the same three share
// flags the reader's own open already grants.
bool WoTVerifiedBlobSpecOverwrite(const FString& AbsolutePath,
	TConstArrayView<uint8> Bytes)
{
#if PLATFORM_WINDOWS
	const HANDLE Handle = ::CreateFileW(*AbsolutePath, GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
		CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (Handle == INVALID_HANDLE_VALUE)
	{
		return false;
	}
	DWORD Written = 0;
	const BOOL bOk = ::WriteFile(Handle, Bytes.GetData(),
		static_cast<DWORD>(Bytes.Num()), &Written, nullptr);
	::CloseHandle(Handle);
	return bOk != 0 && Written == static_cast<DWORD>(Bytes.Num());
#else
	return FFileHelper::SaveArrayToFile(
		TArrayView64<const uint8>(Bytes.GetData(), Bytes.Num()), *AbsolutePath);
#endif
}

FWoTBootstrapStatus WoTVerifiedBlobSpecPublish(
	FWoTCacheStore& Store, const FString& Root, const FString& LogicalId,
	TConstArrayView<uint8> Bytes)
{
	using FResult = FWoTBootstrapStatus;
	auto SessionResult = AcquireWoTProfileSessionShared(
		FPaths::GetPath(Root));
	if (SessionResult.IsError())
		return FResult::Error(MoveTemp(SessionResult).GetError());
	FWoTProfileSession Session = MoveTemp(SessionResult).GetValue();
	auto Exclusive = Session.TryAcquireExclusive();
	if (Exclusive.IsError())
		return FResult::Error(MoveTemp(Exclusive).GetError());
	if (!Exclusive.GetValue())
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::ServiceUnavailable,
			TEXT("the verified-blob test profile is contended")));
	FWoTBootstrapStatus Permit = Store.BeginBuild(Session);
	if (Permit.IsError()) return FResult::Error(MoveTemp(Permit).GetError());
	FWoTBootstrapStatus Written = Store.WriteObject(
		LogicalId, TEXT("wot-reader-test-v1"), Bytes, Sha256(Bytes),
		FMath::Max<uint64>(4096, static_cast<uint64>(Bytes.Num())));
	if (Written.IsError()) return FResult::Error(MoveTemp(Written).GetError());
	FWoTCacheBuildResult Build;
	FWoTCacheBlob& Blob = Build.Manifest.Blobs.AddDefaulted_GetRef();
	Blob.Satisfies.Add(FWoTRequiredFactId{TEXT("spec.") + LogicalId});
	Blob.LogicalId = LogicalId;
	Blob.Schema = TEXT("wot-reader-test-v1");
	Blob.Size = static_cast<uint64>(Bytes.Num());
	Blob.Sha256 = Sha256(Bytes);
	TWoTBootstrapResult<TArray<uint8>> Manifest =
		WriteCanonicalCacheManifest(Build.Manifest);
	if (Manifest.IsError()) return FResult::Error(MoveTemp(Manifest).GetError());
	Build.CanonicalManifest = MoveTemp(Manifest).GetValue();
	Build.ManifestSha256 = Sha256(Build.CanonicalManifest);
	FWoTBootstrapStatus Published = Store.Publish(Build);
	if (Published.IsError()) return MoveTemp(Published);
	return Session.RestoreShared();
}

bool WoTVerifiedBlobSpecReplace(const FString& Victim, const FString& Replacement,
	const FString& Retired)
{
#if PLATFORM_WINDOWS
	return ::MoveFileExW(*Victim, *Retired, 0) != 0
		&& ::MoveFileExW(*Replacement, *Victim, 0) != 0;
#elif PLATFORM_MAC
	const FTCHARToUTF8 VictimUtf8(*Victim);
	const FTCHARToUTF8 ReplacementUtf8(*Replacement);
	const FTCHARToUTF8 RetiredUtf8(*Retired);
	return rename(VictimUtf8.Get(), RetiredUtf8.Get()) == 0
		&& rename(ReplacementUtf8.Get(), VictimUtf8.Get()) == 0;
#else
	return false;
#endif
}

bool WoTVerifiedBlobSpecHardLink(const FString& Link, const FString& Existing)
{
#if PLATFORM_WINDOWS
	return ::CreateHardLinkW(*Link, *Existing, nullptr) != 0;
#elif PLATFORM_MAC
	const FTCHARToUTF8 LinkUtf8(*Link);
	const FTCHARToUTF8 ExistingUtf8(*Existing);
	return link(ExistingUtf8.Get(), LinkUtf8.Get()) == 0;
#else
	return false;
#endif
}

bool WoTVerifiedBlobSpecSymbolicLink(const FString& Link, const FString& Target)
{
#if PLATFORM_WINDOWS
	return ::CreateSymbolicLinkW(*Link, *Target, 0) != 0;
#elif PLATFORM_MAC
	const FTCHARToUTF8 LinkUtf8(*Link);
	const FTCHARToUTF8 TargetUtf8(*Target);
	return symlink(TargetUtf8.Get(), LinkUtf8.Get()) == 0;
#else
	return false;
#endif
}

void WoTVerifiedBlobSpecFixtureCreationDenied(
	FAutomationTestBase& Test, const FString& Fixture)
{
#if PLATFORM_WINDOWS
	Test.AddInfo(FString::Printf(
		TEXT("SKIP %s: Windows fixture creation was denied by platform policy (error %u)"),
		*Fixture, static_cast<uint32>(::GetLastError())));
#else
	Test.AddError(FString::Printf(TEXT("required fixture creation failed: %s"), *Fixture));
#endif
}

bool WoTVerifiedBlobSpecFifo(const FString& Path)
{
#if PLATFORM_MAC
	const FTCHARToUTF8 Utf8(*Path);
	return mkfifo(Utf8.Get(), 0600) == 0;
#else
	(void)Path;
	return false;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVerifiedBlobReaderBoundsTamperSpec,
	"WoT.Cache.VerifiedBlobReader.BoundsTamperAndSingleRead", WoTVerifiedBlobSpecFlags)

bool FWoTVerifiedBlobReaderBoundsTamperSpec::RunTest(const FString&)
{
	FWoTVerifiedBlobSpecRoot Root;
	TWoTBootstrapResult<FWoTCacheStore> Made = FWoTCacheStore::ForTesting(Root.Path);
	if (Made.IsError()) return false;
	FWoTCacheStore Store = MoveTemp(Made).GetValue();
	const FString LogicalId(TEXT("cache/reader/bounds"));
	const TArray<uint8> Original = WoTVerifiedBlobSpecBytes("verified-reader-bytes");
	FWoTBootstrapStatus Published =
		WoTVerifiedBlobSpecPublish(Store, Root.Path, LogicalId, Original);
	TWoTBootstrapResult<FWoTCacheGenerationLease> Lease = WoTAcquireCurrentForTesting(Store);
	if (Published.IsError() || Lease.IsError()) return false;
	FWoTCacheBlobDescriptor Descriptor;
	Descriptor.LogicalId = LogicalId;
	Descriptor.Schema = TEXT("wot-reader-test-v1");
	Descriptor.Size = Original.Num();
	Descriptor.Sha256 = Sha256(Original);
	FWoTCacheBlobReadLimits Limits;
	TWoTBootstrapResult<FWoTImmutableVerifiedBlob> Read =
		ReadVerifiedCacheBlob(Lease.GetValue(), Descriptor, Limits);
	if (TestFalse(TEXT("valid bytes verify"), Read.IsError()))
	{
		TestTrue(TEXT("the immutable view contains exactly the verified allocation"),
			Read.GetValue().Bytes().Num() == Original.Num()
			&& FMemory::Memcmp(Read.GetValue().Bytes().GetData(),
				Original.GetData(), Original.Num()) == 0);
	}
	FWoTCacheBlobDescriptor Overrun = Descriptor;
	++Overrun.Size;
	TestTrue(TEXT("declared-size overrun is rejected"),
		ReadVerifiedCacheBlob(Lease.GetValue(), Overrun, Limits).IsError());
	FWoTCacheBlobDescriptor Underrun = Descriptor;
	--Underrun.Size;
	TestTrue(TEXT("declared-size underrun is rejected"),
		ReadVerifiedCacheBlob(Lease.GetValue(), Underrun, Limits).IsError());
	FWoTCacheBlobDescriptor WrongSchema = Descriptor;
	WrongSchema.Schema = TEXT("wot-test-v2");
	TestTrue(TEXT("caller schema mismatch is rejected by the leased manifest"),
		ReadVerifiedCacheBlob(Lease.GetValue(), WrongSchema, Limits).IsError());
	FWoTCacheBlobReadLimits TinyLimit;
	TinyLimit.MaximumBlobBytes = Original.Num() - 1;
	TinyLimit.MaximumAllocationBytes = TNumericLimits<uint64>::Max();
	TestTrue(TEXT("read-limit overflow is rejected before allocation"),
		ReadVerifiedCacheBlob(Lease.GetValue(), Descriptor, TinyLimit).IsError());
	FWoTCacheBlobReadLimits TinyAllocation;
	TinyAllocation.MaximumBlobBytes = TNumericLimits<uint64>::Max();
	TinyAllocation.MaximumAllocationBytes = Original.Num() - 1;
	TestTrue(TEXT("allocation-limit overflow is rejected before allocation"),
		ReadVerifiedCacheBlob(Lease.GetValue(), Descriptor, TinyAllocation).IsError());
	FWoTCacheBlobDescriptor ArrayOverflow = Descriptor;
	ArrayOverflow.Size = static_cast<uint64>(TNumericLimits<int32>::Max()) + 1;
	FWoTCacheBlobReadLimits UnboundedForOverflow;
	UnboundedForOverflow.MaximumBlobBytes = TNumericLimits<uint64>::Max();
	UnboundedForOverflow.MaximumAllocationBytes = TNumericLimits<uint64>::Max();
	TestTrue(TEXT("array-index overflow is rejected before open or allocation"),
		ReadVerifiedCacheBlob(
			Lease.GetValue(), ArrayOverflow, UnboundedForOverflow).IsError());

	const FString BlobPath = FPaths::Combine(Root.Path,
		TEXT("current"), TEXT("blobs"),
		WoTVerifiedBlobSpecFileName(LogicalId));
	TArray<uint8> Tampered = Original;
	Tampered[0] ^= 0xffu;
	FWoTCacheBlobReadLimits BeforeReadTamper;
	BeforeReadTamper.ImmediatelyBeforeSingleReadForTesting = [&]()
	{
		WoTVerifiedBlobSpecOverwrite(BlobPath, Tampered);
	};
	TestTrue(TEXT("same-size tampering immediately before the one read is rejected"),
		ReadVerifiedCacheBlob(Lease.GetValue(), Descriptor, BeforeReadTamper).IsError());
	FWoTCacheBlobDescriptor ForgedDescriptor = Descriptor;
	ForgedDescriptor.Sha256 = Sha256(Tampered);
	TestTrue(TEXT("a self-consistent caller descriptor cannot override the leased manifest"),
		ReadVerifiedCacheBlob(Lease.GetValue(), ForgedDescriptor, Limits).IsError());
	if (!Read.IsError())
	{
		bool bParserSawOnlyVerifiedBuffer = false;
		auto ParserFixture = [&](TConstArrayView<uint8> VerifiedBytes)
		{
			bParserSawOnlyVerifiedBuffer = VerifiedBytes.Num() == Original.Num()
				&& FMemory::Memcmp(VerifiedBytes.GetData(),
					Original.GetData(), Original.Num()) == 0;
		};
		ParserFixture(Read.GetValue().Bytes());
		TestTrue(TEXT("the parser fixture sees only the immutable verified snapshot"),
			bParserSawOnlyVerifiedBuffer);
		TestTrue(TEXT("later pathname mutation cannot change the returned snapshot"),
			FMemory::Memcmp(Read.GetValue().Bytes().GetData(),
				Original.GetData(), Original.Num()) == 0);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVerifiedBlobReaderIdentitySpec,
	"WoT.Cache.VerifiedBlobReader.IdentityLinksSwapAndDelayedUse", WoTVerifiedBlobSpecFlags)

bool FWoTVerifiedBlobReaderIdentitySpec::RunTest(const FString&)
{
	FWoTVerifiedBlobSpecRoot Root;
	TWoTBootstrapResult<FWoTCacheStore> Made = FWoTCacheStore::ForTesting(Root.Path);
	if (Made.IsError()) return false;
	FWoTCacheStore Store = MoveTemp(Made).GetValue();
	const FString LogicalId(TEXT("cache/reader/identity"));
	const TArray<uint8> Original = WoTVerifiedBlobSpecBytes("held-object-original");
	FWoTBootstrapStatus Published =
		WoTVerifiedBlobSpecPublish(Store, Root.Path, LogicalId, Original);
	TWoTBootstrapResult<FWoTCacheGenerationLease> Lease = WoTAcquireCurrentForTesting(Store);
	if (Published.IsError() || Lease.IsError()) return false;
	FWoTCacheBlobDescriptor Descriptor{LogicalId, TEXT("wot-reader-test-v1"),
		static_cast<uint64>(Original.Num()), Sha256(Original)};
	const FString BlobPath = FPaths::Combine(Root.Path,
		TEXT("current"), TEXT("blobs"),
		WoTVerifiedBlobSpecFileName(LogicalId));
	const FString ReplacementPath = BlobPath + TEXT(".replacement");
	const FString RetiredPath = BlobPath + TEXT(".retired");
	TArray<uint8> Replacement = Original;
	Replacement[0] ^= 0x55u;
	FFileHelper::SaveArrayToFile(Replacement, *ReplacementPath);
	bool bSwapped = false;
	FWoTCacheBlobReadLimits SwapLimits;
	SwapLimits.ImmediatelyBeforeSingleReadForTesting = [&]()
	{
		bSwapped = WoTVerifiedBlobSpecReplace(BlobPath, ReplacementPath, RetiredPath);
	};
	TWoTBootstrapResult<FWoTImmutableVerifiedBlob> SwappedRead =
		ReadVerifiedCacheBlob(Lease.GetValue(), Descriptor, SwapLimits);
	if (bSwapped)
	{
		TestFalse(TEXT("a pathname swap cannot redirect the held object"),
			SwappedRead.IsError());
		IFileManager::Get().Delete(*BlobPath);
		if (!IFileManager::Get().Move(
			*BlobPath, *RetiredPath, true, false, false, true))
		{
			AddError(TEXT("the pathname-swap fixture could not restore the generation"));
		}
	}
	else
	{
		AddError(TEXT("the required pathname-swap fixture could not be constructed"));
	}

	// Use a new immutable generation for the independent link-shape adversaries.
	const TArray<uint8> LinkBytes = WoTVerifiedBlobSpecBytes("link-object-bytes");
	Lease = TWoTBootstrapResult<FWoTCacheGenerationLease>::Error(
		MakeWoTBootstrapError(EWoTBootstrapError::Io, TEXT("released test lease")));
	Published = WoTVerifiedBlobSpecPublish(Store, Root.Path, LogicalId, LinkBytes);
	Lease = WoTAcquireCurrentForTesting(Store);
	if (Published.IsError() || Lease.IsError()) return false;
	Descriptor.Size = LinkBytes.Num();
	Descriptor.Sha256 = Sha256(LinkBytes);
	const FString LinkBlobPath = FPaths::Combine(Root.Path,
		TEXT("current"), TEXT("blobs"),
		WoTVerifiedBlobSpecFileName(LogicalId));
	const FString HardLinkPath = LinkBlobPath + TEXT(".hard");
	if (WoTVerifiedBlobSpecHardLink(HardLinkPath, LinkBlobPath))
	{
		TestTrue(TEXT("a multiply-linked blob is rejected"),
			ReadVerifiedCacheBlob(Lease.GetValue(), Descriptor,
				FWoTCacheBlobReadLimits()).IsError());
		IFileManager::Get().Delete(*HardLinkPath);
	}
	else WoTVerifiedBlobSpecFixtureCreationDenied(*this, TEXT("blob hard-link"));

	const FString RealPath = LinkBlobPath + TEXT(".real");
	const bool bRetiredForLink = IFileManager::Get().Move(
		*RealPath, *LinkBlobPath, true, false, false, true);
	if (bRetiredForLink && WoTVerifiedBlobSpecSymbolicLink(LinkBlobPath, RealPath))
	{
		TestTrue(TEXT("a symlink or reparse substitution is rejected"),
			ReadVerifiedCacheBlob(Lease.GetValue(), Descriptor,
				FWoTCacheBlobReadLimits()).IsError());
		IFileManager::Get().Delete(*LinkBlobPath);
		IFileManager::Get().Move(*LinkBlobPath, *RealPath, true, false, false, true);
	}
	else if (bRetiredForLink)
	{
		WoTVerifiedBlobSpecFixtureCreationDenied(*this, TEXT("blob symlink/reparse"));
		IFileManager::Get().Move(*LinkBlobPath, *RealPath, true, false, false, true);
	}
	else
	{
		AddError(TEXT("blob symlink fixture could not retire the original object"));
	}

	TArray<uint8> DelayedTamper = LinkBytes;
	DelayedTamper[DelayedTamper.Num() - 1] ^= 0x11u;
	FFileHelper::SaveArrayToFile(DelayedTamper, *LinkBlobPath);
	TestTrue(TEXT("mutation after activation but before delayed use is rejected"),
		ReadVerifiedCacheBlob(Lease.GetValue(), Descriptor,
			FWoTCacheBlobReadLimits()).IsError());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVerifiedBlobReaderConcurrentMutationSpec,
	"WoT.Cache.VerifiedBlobReader.ConcurrentSingleReadMutation",
	WoTVerifiedBlobSpecFlags)

bool FWoTVerifiedBlobReaderConcurrentMutationSpec::RunTest(const FString&)
{
	FWoTVerifiedBlobSpecRoot Root;
	TWoTBootstrapResult<FWoTCacheStore> Made = FWoTCacheStore::ForTesting(Root.Path);
	if (Made.IsError()) return false;
	FWoTCacheStore Store = MoveTemp(Made).GetValue();
	const FString LogicalId(TEXT("cache/reader/concurrent"));
	TArray<uint8> Original;
	Original.SetNumUninitialized(8 * 1024 * 1024);
	FMemory::Memset(Original.GetData(), 0x31, Original.Num());
	TArray<uint8> Tampered = Original;
	FMemory::Memset(Tampered.GetData(), 0x72, Tampered.Num());
	FWoTBootstrapStatus Published =
		WoTVerifiedBlobSpecPublish(Store, Root.Path, LogicalId, Original);
	TWoTBootstrapResult<FWoTCacheGenerationLease> Lease = WoTAcquireCurrentForTesting(Store);
	if (Published.IsError() || Lease.IsError()) return false;
	FWoTCacheBlobDescriptor Descriptor{LogicalId, TEXT("wot-reader-test-v1"),
		static_cast<uint64>(Original.Num()), Sha256(Original)};
	const FString BlobPath = FPaths::Combine(Root.Path,
		TEXT("current"), TEXT("blobs"),
		WoTVerifiedBlobSpecFileName(LogicalId));
	bool bConcurrentWriterRan = false;
	FEvent* WriterReady = FPlatformProcess::GetSynchEventFromPool(true);
	FEvent* BeginMutation = FPlatformProcess::GetSynchEventFromPool(true);
	FWoTCacheBlobReadLimits ConcurrentLimits;
	ConcurrentLimits.ConcurrentWithSingleReadForTesting = [&]()
	{
		WriterReady->Trigger();
		BeginMutation->Wait();
		bConcurrentWriterRan = true;
		// The share-friendly writer, or on Windows this mutation silently never
		// lands and the assertion below passes vacuously with the original bytes.
		WoTVerifiedBlobSpecOverwrite(BlobPath, Tampered);
	};
	ConcurrentLimits.ImmediatelyBeforeReadAfterWorkerStartedForTesting = [&]()
	{
		TestTrue(TEXT("the concurrent writer reaches the read rendezvous"),
			WriterReady->Wait(1000));
		BeginMutation->Trigger();
	};
	TWoTBootstrapResult<FWoTImmutableVerifiedBlob> Concurrent =
		ReadVerifiedCacheBlob(Lease.GetValue(), Descriptor, ConcurrentLimits);
	FPlatformProcess::ReturnSynchEventToPool(WriterReady);
	FPlatformProcess::ReturnSynchEventToPool(BeginMutation);
	TestTrue(TEXT("the concurrent mutation fixture ran beside the sole read"),
		bConcurrentWriterRan);
	TestTrue(TEXT("a concurrent same-size mutation returns no unverified bytes"),
		Concurrent.IsError()
		|| (Concurrent.GetValue().Bytes().Num() == Original.Num()
			&& FMemory::Memcmp(Concurrent.GetValue().Bytes().GetData(),
				Original.GetData(), Original.Num()) == 0));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVerifiedBlobReaderShortNonRegularSpec,
	"WoT.Cache.VerifiedBlobReader.ShortReadAndNonRegular", WoTVerifiedBlobSpecFlags)

bool FWoTVerifiedBlobReaderShortNonRegularSpec::RunTest(const FString&)
{
	FWoTVerifiedBlobSpecRoot Root;
	TWoTBootstrapResult<FWoTCacheStore> Made = FWoTCacheStore::ForTesting(Root.Path);
	if (Made.IsError()) return false;
	FWoTCacheStore Store = MoveTemp(Made).GetValue();
	const FString LogicalId(TEXT("cache/reader/short"));
	const TArray<uint8> Original = WoTVerifiedBlobSpecBytes("short-read-fixture");
	FWoTBootstrapStatus Published =
		WoTVerifiedBlobSpecPublish(Store, Root.Path, LogicalId, Original);
	TWoTBootstrapResult<FWoTCacheGenerationLease> Lease = WoTAcquireCurrentForTesting(Store);
	if (Published.IsError() || Lease.IsError()) return false;
	FWoTCacheBlobDescriptor Descriptor{LogicalId, TEXT("wot-reader-test-v1"),
		static_cast<uint64>(Original.Num()), Sha256(Original)};
	const FString BlobPath = FPaths::Combine(Root.Path,
		TEXT("current"), TEXT("blobs"),
		WoTVerifiedBlobSpecFileName(LogicalId));
	FWoTCacheBlobReadLimits ShortLimits;
	ShortLimits.ImmediatelyBeforeSingleReadForTesting = [&]()
	{
		const TArray<uint8> Short = WoTVerifiedBlobSpecBytes("x");
		WoTVerifiedBlobSpecOverwrite(BlobPath, Short);
	};
	TestTrue(TEXT("a short concurrent read returns no bytes"),
		ReadVerifiedCacheBlob(Lease.GetValue(), Descriptor, ShortLimits).IsError());
	FFileHelper::SaveArrayToFile(Original, *BlobPath);

	// Publish again, then substitute a directory for the blob name.
	Lease = TWoTBootstrapResult<FWoTCacheGenerationLease>::Error(
		MakeWoTBootstrapError(EWoTBootstrapError::Io, TEXT("released test lease")));
	Published = WoTVerifiedBlobSpecPublish(Store, Root.Path, LogicalId, Original);
	Lease = WoTAcquireCurrentForTesting(Store);
	if (Published.IsError() || Lease.IsError()) return false;
	const FString DirectoryBlobPath = FPaths::Combine(Root.Path,
		TEXT("current"), TEXT("blobs"),
		WoTVerifiedBlobSpecFileName(LogicalId));
	IFileManager::Get().Delete(*DirectoryBlobPath);
	IFileManager::Get().MakeDirectory(*DirectoryBlobPath);
	TestTrue(TEXT("a non-regular blob object is rejected"),
		ReadVerifiedCacheBlob(Lease.GetValue(), Descriptor,
			FWoTCacheBlobReadLimits()).IsError());
#if PLATFORM_MAC
	IFileManager::Get().DeleteDirectory(*DirectoryBlobPath, false, true);
	FFileHelper::SaveArrayToFile(Original, *DirectoryBlobPath);
	Lease = TWoTBootstrapResult<FWoTCacheGenerationLease>::Error(
		MakeWoTBootstrapError(EWoTBootstrapError::Io, TEXT("released test lease")));
	Published = WoTVerifiedBlobSpecPublish(Store, Root.Path, LogicalId, Original);
	Lease = WoTAcquireCurrentForTesting(Store);
	if (Published.IsError() || Lease.IsError()) return false;
	const FString FifoBlobPath = FPaths::Combine(Root.Path,
		TEXT("current"), TEXT("blobs"),
		WoTVerifiedBlobSpecFileName(LogicalId));
	if (!IFileManager::Get().Delete(*FifoBlobPath, false, true, true)
		|| !WoTVerifiedBlobSpecFifo(FifoBlobPath))
	{
		AddError(TEXT("required blob FIFO fixture could not be created"));
		return false;
	}
	TFuture<TWoTBootstrapResult<FWoTImmutableVerifiedBlob>> FifoRead = Async(
		EAsyncExecution::Thread, [&]()
		{
			return ReadVerifiedCacheBlob(
				Lease.GetValue(), Descriptor, FWoTCacheBlobReadLimits());
		});
	const bool bFifoRefusedWithinBound = FifoRead.WaitFor(FTimespan::FromSeconds(1));
	TestTrue(TEXT("a blob FIFO is refused without a blocking open"),
		bFifoRefusedWithinBound);
	if (bFifoRefusedWithinBound)
	{
		TestTrue(TEXT("the bounded FIFO read returns an error"),
			FifoRead.Consume().IsError());
	}
#else
	AddInfo(TEXT("SKIP blob FIFO: mkfifo is a POSIX-only fixture"));
#endif
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVerifiedBlobReaderSnapshotMemoSpec,
	"WoT.Cache.VerifiedBlobReader.SnapshotMemoAndSharedRevocation",
	WoTVerifiedBlobSpecFlags)

bool FWoTVerifiedBlobReaderSnapshotMemoSpec::RunTest(const FString&)
{
	FWoTVerifiedBlobSpecRoot Root;
	TWoTBootstrapResult<FWoTCacheStore> Made = FWoTCacheStore::ForTesting(Root.Path);
	if (Made.IsError()) return false;
	FWoTCacheStore Store = MoveTemp(Made).GetValue();
	const FString CriticalId(TEXT("cache/reader/memo-critical"));
	const FString LazyId(TEXT("cache/reader/memo-lazy"));
	const TArray<uint8> CriticalBytes =
		WoTVerifiedBlobSpecBytes("memo-critical-bytes");
	const TArray<uint8> LazyBytes = WoTVerifiedBlobSpecBytes("memo-lazy-bytes");

	auto SessionResult = AcquireWoTProfileSessionShared(
		FPaths::GetPath(Root.Path));
	if (SessionResult.IsError()) return false;
	FWoTProfileSession Session = MoveTemp(SessionResult).GetValue();
	auto Exclusive = Session.TryAcquireExclusive();
	if (Exclusive.IsError() || !Exclusive.GetValue()) return false;
	FWoTBootstrapStatus Permit = Store.BeginBuild(Session);
	if (Permit.IsError()) return false;
	if (Store.WriteObject(CriticalId, TEXT("wot-reader-test-v1"),
			CriticalBytes, Sha256(CriticalBytes), 4096).IsError()
		|| Store.WriteObject(LazyId, TEXT("wot-reader-test-v1"),
			LazyBytes, Sha256(LazyBytes), 4096).IsError())
	{
		return false;
	}
	FWoTCacheBuildResult Build;
	auto AddBlob = [&Build](const FString& LogicalId, TConstArrayView<uint8> Bytes)
	{
		FWoTCacheBlob& Blob = Build.Manifest.Blobs.AddDefaulted_GetRef();
		Blob.Satisfies.Add(FWoTRequiredFactId{TEXT("spec.") + LogicalId});
		Blob.LogicalId = LogicalId;
		Blob.Schema = TEXT("wot-reader-test-v1");
		Blob.Size = static_cast<uint64>(Bytes.Num());
		Blob.Sha256 = Sha256(Bytes);
	};
	AddBlob(CriticalId, CriticalBytes);
	AddBlob(LazyId, LazyBytes);
	TWoTBootstrapResult<TArray<uint8>> Manifest =
		WriteCanonicalCacheManifest(Build.Manifest);
	if (Manifest.IsError()) return false;
	Build.CanonicalManifest = MoveTemp(Manifest).GetValue();
	Build.ManifestSha256 = Sha256(Build.CanonicalManifest);
	FWoTBootstrapStatus Published =
		Store.Publish(Build);
	if (Published.IsError()) return false;
	if (Session.RestoreShared().IsError()) return false;

	int32 PhysicalReads = 0;
	WoTSetVerifiedBlobBodyReadObserverForTesting([&PhysicalReads]()
	{
		++PhysicalReads;
	});
    auto Snapshot = WoTAcquireCurrentForTesting(Store);
    if (Snapshot.IsError())
    {
        WoTSetVerifiedBlobBodyReadObserverForTesting({});
        return false;
    }
    const TArray<FString> CriticalIds{CriticalId};
    auto Verified = Snapshot.GetValue().VerifyCriticalClosureForTesting(CriticalIds, nullptr);
    if (!TestFalse(TEXT("critical closure verifies"), Verified.IsError()))
    {
        WoTSetVerifiedBlobBodyReadObserverForTesting({});
        return false;
    }
	TestEqual(TEXT("critical closure performs one physical verification"),
		PhysicalReads, 1);
	FWoTCacheGenerationLease PayloadLease = Snapshot.GetValue();
	FWoTCacheGenerationLease WorldLease = Snapshot.GetValue();
	FWoTCacheBlobDescriptor CriticalDescriptor{CriticalId,
		TEXT("wot-reader-test-v1"), static_cast<uint64>(CriticalBytes.Num()),
		Sha256(CriticalBytes)};
	TWoTBootstrapResult<FWoTImmutableVerifiedBlob> CriticalRead =
		ReadVerifiedCacheBlob(PayloadLease, CriticalDescriptor,
			FWoTCacheBlobReadLimits());
	TestFalse(TEXT("critical bytes are served from the retained exact buffer"),
		CriticalRead.IsError());
	TestEqual(TEXT("critical reuse performs no second disk read"), PhysicalReads, 1);

	FWoTCacheBlobDescriptor LazyDescriptor{LazyId, TEXT("wot-reader-test-v1"),
		static_cast<uint64>(LazyBytes.Num()), Sha256(LazyBytes)};
	{
		TWoTBootstrapResult<FWoTImmutableVerifiedBlob> First =
			ReadVerifiedCacheBlob(PayloadLease, LazyDescriptor,
				FWoTCacheBlobReadLimits());
		TWoTBootstrapResult<FWoTImmutableVerifiedBlob> Second =
			ReadVerifiedCacheBlob(WorldLease, LazyDescriptor,
				FWoTCacheBlobReadLimits());
		if (First.IsError() || Second.IsError())
		{
			WoTSetVerifiedBlobBodyReadObserverForTesting({});
			return false;
		}
		TestTrue(TEXT("live successful reads reuse the exact immutable allocation"),
			First.GetValue().Bytes().GetData()
				== Second.GetValue().Bytes().GetData());
		TestEqual(TEXT("two live views perform one lazy physical read"),
			PhysicalReads, 2);
	}
	TWoTBootstrapResult<FWoTImmutableVerifiedBlob> Later =
		ReadVerifiedCacheBlob(WorldLease, LazyDescriptor,
			FWoTCacheBlobReadLimits());
	WoTSetVerifiedBlobBodyReadObserverForTesting({});
	TestFalse(TEXT("later lazy read reauthenticates"), Later.IsError());
	TestEqual(TEXT("released lazy buffer causes a fresh physical verification"),
		PhysicalReads, 3);

	FWoTCacheBlobDescriptor Forged = LazyDescriptor;
	Forged.Schema = TEXT("wot-reader-test-v2");
	TestTrue(TEXT("memo lookup remains keyed by the complete descriptor"),
		ReadVerifiedCacheBlob(PayloadLease, Forged,
			FWoTCacheBlobReadLimits()).IsError());
	TestTrue(TEXT("caller descriptor mismatch does not revoke authentic state"),
		PayloadLease.IsValid() && WorldLease.IsValid());
	const FString LazyPath = FPaths::Combine(Root.Path,
		TEXT("current"), TEXT("blobs"),
		WoTVerifiedBlobSpecFileName(LazyId));
	TArray<uint8> MutatedLazy = LazyBytes;
	MutatedLazy[0] ^= 0x33u;
	FWoTCacheBlobReadLimits MutationLimits;
	MutationLimits.ImmediatelyBeforeSingleReadForTesting = [&]()
	{
		WoTVerifiedBlobSpecOverwrite(LazyPath, MutatedLazy);
	};
	TestTrue(TEXT("a later identity mismatch returns no bytes"),
		ReadVerifiedCacheBlob(PayloadLease, LazyDescriptor,
			MutationLimits).IsError());
	TestFalse(TEXT("identity mismatch revokes both domain lease views"),
		PayloadLease.IsValid() || WorldLease.IsValid());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVerifiedBlobRuntimeIntegrityRecoverySpec,
    "WoT.Cache.VerifiedBlobReader.RuntimeDamageMarkerAndRestart", WoTVerifiedBlobSpecFlags)

bool FWoTVerifiedBlobRuntimeIntegrityRecoverySpec::RunTest(const FString&)
{
    FWoTVerifiedBlobSpecRoot Root;
    auto Opened = FWoTCacheStore::ForTesting(Root.Path);
    if (Opened.IsError()) return false;
    auto Store = MoveTemp(Opened).GetValue();
    const FString LogicalId(TEXT("cache/reader/lazy-damage"));
    const auto Bytes = WoTVerifiedBlobSpecBytes("lazy-authenticated-bytes");
    if (WoTVerifiedBlobSpecPublish(
		Store, Root.Path, LogicalId, Bytes).IsError()) return false;
    {
        auto Snapshot = WoTAcquireCurrentForTesting(Store);
        if (Snapshot.IsError()) return false;
        auto Verified = Snapshot.GetValue().VerifyCriticalClosureForTesting({}, nullptr);
        if (Verified.IsError()) return false;
		auto PayloadLease = Snapshot.GetValue();
		auto WorldLease = Snapshot.GetValue();
        auto DamagedBytes = Bytes;
        DamagedBytes[0] ^= 0x33;
        const FString BlobPath = Root.Path / TEXT("current/blobs") / WoTVerifiedBlobSpecFileName(LogicalId);
        if (!TestTrue(TEXT("lazy payload damage is written"),
            WoTVerifiedBlobSpecOverwrite(BlobPath, DamagedBytes))) return false;
        FWoTCacheBlobDescriptor Descriptor{LogicalId, TEXT("wot-reader-test-v1"),
            static_cast<uint64>(Bytes.Num()), Sha256(Bytes)};
		auto Failed = ReadVerifiedCacheBlob(PayloadLease, Descriptor, {});
        TestTrue(TEXT("lazy hash failure refuses bytes"), Failed.IsError());
		TestTrue(TEXT("failure damages shared snapshot"), WorldLease.IsDamaged());
        TestTrue(TEXT("one failure is available to the departure flow"),
			WorldLease.TakeReadFailure().IsSet());
        TestFalse(TEXT("failure is consumed once"),
			PayloadLease.TakeReadFailure().IsSet());
        const FString Marker = Root.Path / TEXT("current/damaged");
        TestEqual(TEXT("damage marker is an empty file"), IFileManager::Get().FileSize(*Marker), int64(0));
        auto Restart = Store.TryOpenCurrent({}, nullptr);
        TestTrue(TEXT("next opener rejects marked current"), !Restart.IsError() && !Restart.GetValue().IsSet());
    }
    if (!TestFalse(TEXT("fresh build after readers leave succeeds"),
        WoTVerifiedBlobSpecPublish(
			Store, Root.Path, LogicalId, Bytes).IsError())) return false;
    TestFalse(TEXT("fresh build discards damaged marker"),
        IFileManager::Get().FileExists(*(Root.Path / TEXT("current/damaged"))));
    auto Restart = WoTAcquireCurrentForTesting(Store);
    TestFalse(TEXT("fresh current opens"), Restart.IsError());
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVerifiedBlobMarkerWriteFailureSpec,
    "WoT.Cache.VerifiedBlobReader.MarkerWriteFailureStillRevokes", WoTVerifiedBlobSpecFlags)

bool FWoTVerifiedBlobMarkerWriteFailureSpec::RunTest(const FString&)
{
    FWoTVerifiedBlobSpecRoot Root;
    auto Opened = FWoTCacheStore::ForTesting(Root.Path);
    if (Opened.IsError()) return false;
    auto Store = MoveTemp(Opened).GetValue();
    const FString LogicalId(TEXT("cache/reader/marker-failure"));
    const auto Bytes = WoTVerifiedBlobSpecBytes("marker-failure-bytes");
    if (WoTVerifiedBlobSpecPublish(
		Store, Root.Path, LogicalId, Bytes).IsError()) return false;
    auto Snapshot = WoTAcquireCurrentForTesting(Store);
    if (Snapshot.IsError()) return false;
	auto PayloadLease = Snapshot.GetValue();
	auto WorldLease = Snapshot.GetValue();
    if (!TestTrue(TEXT("marker file destination is obstructed"), IFileManager::Get().MakeDirectory(
        *(Root.Path / TEXT("current/damaged")), false))) return false;
    auto DamagedBytes = Bytes;
    DamagedBytes[0] ^= 0x33;
    if (!WoTVerifiedBlobSpecOverwrite(Root.Path / TEXT("current/blobs")
        / WoTVerifiedBlobSpecFileName(LogicalId), DamagedBytes)) return false;
    FWoTCacheBlobDescriptor Descriptor{LogicalId, TEXT("wot-reader-test-v1"),
        static_cast<uint64>(Bytes.Num()), Sha256(Bytes)};
	auto Failed = ReadVerifiedCacheBlob(PayloadLease, Descriptor, {});
    TestTrue(TEXT("marker failure never releases corrupt bytes"), Failed.IsError());
	TestTrue(TEXT("marker failure still revokes every shared view"), WorldLease.IsDamaged());
    TestTrue(TEXT("departure can consume the damage despite marker failure"),
		WorldLease.TakeReadFailure().IsSet());
    TestFalse(TEXT("failed marker write has not produced a file"),
        IFileManager::Get().FileExists(*(Root.Path / TEXT("current/damaged"))));
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVerifiedBlobOrdinaryIoClassificationSpec,
    "WoT.Cache.VerifiedBlobReader.OrdinaryIoIsNotDamage", WoTVerifiedBlobSpecFlags)

bool FWoTVerifiedBlobOrdinaryIoClassificationSpec::RunTest(const FString&)
{
    FWoTVerifiedBlobSpecRoot Root;
    auto Opened = FWoTCacheStore::ForTesting(Root.Path);
    if (Opened.IsError()) return false;
    auto Store = MoveTemp(Opened).GetValue();
    const FString LogicalId(TEXT("cache/reader/lazy-io"));
    const auto Bytes = WoTVerifiedBlobSpecBytes("lazy-io-bytes");
    if (WoTVerifiedBlobSpecPublish(
		Store, Root.Path, LogicalId, Bytes).IsError()) return false;
    auto Lease = WoTAcquireCurrentForTesting(Store);
    if (Lease.IsError()) return false;
    FWoTCacheBlobReadLimits Limits;
    Limits.BodyReadFailureForTesting = []() -> TOptional<FWoTBootstrapError>
    {
        auto Error = MakeWoTBootstrapError(EWoTBootstrapError::Io, TEXT("injected ordinary read failure"));
        Error.NativeCode = 5;
        return Error;
    };
    FWoTCacheBlobDescriptor Descriptor{LogicalId, TEXT("wot-reader-test-v1"),
        static_cast<uint64>(Bytes.Num()), Sha256(Bytes)};
    auto Failed = ReadVerifiedCacheBlob(Lease.GetValue(), Descriptor, Limits);
    if (!TestTrue(TEXT("ordinary read failure is propagated"), Failed.IsError())) return false;
    TestEqual(TEXT("ordinary I/O native code is preserved"), Failed.GetError().NativeCode, uint32(5));
    TestFalse(TEXT("ordinary I/O does not mark snapshot damaged"), Lease.GetValue().IsDamaged());
    TestFalse(TEXT("ordinary I/O leaves no damage marker"),
        IFileManager::Get().FileExists(*(Root.Path / TEXT("current/damaged"))));
    TestFalse(TEXT("ordinary I/O emits no content failure"), Lease.GetValue().TakeReadFailure().IsSet());
    auto StillCurrent = Store.TryOpenCurrent({}, nullptr);
    TestTrue(TEXT("ordinary I/O leaves current selectable"),
        !StillCurrent.IsError() && StillCurrent.GetValue().IsSet());
    return !HasAnyErrors();
}

#endif
