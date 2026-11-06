#include "WoTUpdaterCore/UpdateFailureNotice.h"

#include "Containers/StringConv.h"
#include "CoreGlobals.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "WoTBootstrapCore/NativeHandles.h"
#include "WoTBootstrapCore/TestOverrides.h"

namespace
{
constexpr TCHAR WoTUpdateFailedNoticeLeaf[] = TEXT("WoTUpdateFailedNotice");
constexpr TCHAR WoTUpdateFailedNoticeTempLeaf[] = TEXT("WoTUpdateFailedNotice.tmp");

#if WITH_AUTOMATION_TESTS
FString GWoTUpdateFailureNoticeDirectoryForTesting;
bool GWoTAllowCanonicalUpdateFailureNoticeDirectoryForTesting = false;
#endif

TWoTBootstrapResult<TOptional<EWoTTreeEntryType>> WoTNoticeFind(
	const FWoTNativeDirectoryHandle& Directory, FStringView Leaf)
{
	using FResult = TWoTBootstrapResult<TOptional<EWoTTreeEntryType>>;
	TWoTBootstrapResult<TArray<FWoTNativeDirectoryEntry>> Entries =
		EnumerateNativeDirectory(Directory);
	if (Entries.IsError()) return FResult::Error(MoveTemp(Entries).GetError());
	for (const FWoTNativeDirectoryEntry& Entry : Entries.GetValue())
	{
		if (Entry.Name == Leaf) return FResult::Success(Entry.Type);
	}
	return FResult::Success(TOptional<EWoTTreeEntryType>());
}

bool WoTNoticeIsSeenFor(TConstArrayView<uint8> Bytes, uint64 Sequence)
{
	if (Bytes.Num() <= 0 || Bytes.Num() >= 256) return false;
	FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()),
		Bytes.Num());
	const FString Text(Converted.Length(), Converted.Get());
	return Text == FString::Printf(
		TEXT("WUF1\nfailed=%llu\nstate=seen\n"), Sequence);
}

bool WoTNoticeMatchesFailure(TConstArrayView<uint8> Bytes,
	uint64 FailedReleaseSequence, uint64 RestoredReleaseSequence)
{
	if (WoTNoticeIsSeenFor(Bytes, FailedReleaseSequence)) return true;
	if (Bytes.Num() <= 0 || Bytes.Num() >= 256) return false;
	FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()),
		Bytes.Num());
	const FString Text(Converted.Length(), Converted.Get());
	return Text == FString::Printf(
		TEXT("WUF1\nfailed=%llu\nrestored=%llu\nstate=pending\n"),
		FailedReleaseSequence, RestoredReleaseSequence);
}

bool WoTResolveNoticeDirectoryPolicy(FStringView UpdaterUserStateRoot,
	FStringView CanonicalPreferencesDirectory, bool bMacPolicy,
	FString& OutDirectory)
{
	(void)CanonicalPreferencesDirectory;
	(void)bMacPolicy;
	FString Root(UpdaterUserStateRoot);
	if (Root.IsEmpty()) return false;
	Root = FPaths::ConvertRelativePathToFull(Root);
	FPaths::NormalizeDirectoryName(Root);
	if (Root.IsEmpty() || FPaths::IsRelative(Root)
		|| !FPaths::CollapseRelativeDirectories(Root))
	{
		return false;
	}
	OutDirectory = FPaths::Combine(Root, TEXT("Preferences"));
	return true;
}

TWoTBootstrapResult<FWoTNativeDirectoryHandle> WoTOpenNoticeDirectory(
	const FWoTUpdaterStateRoot& Root, const FString& ResolvedDirectory,
	bool bCreate)
{
	using FResult = TWoTBootstrapResult<FWoTNativeDirectoryHandle>;
	FString Expected = FPaths::Combine(Root.UserStateRoot, TEXT("Preferences"));
	FString Resolved = ResolvedDirectory;
	FPaths::NormalizeDirectoryName(Expected);
	FPaths::NormalizeDirectoryName(Resolved);
	if (Expected == Resolved)
	{
		TWoTBootstrapResult<FWoTNativeDirectoryHandle> UserRoot =
			OpenNativeDirectoryNoFollow(Root.UserStateRoot);
		if (UserRoot.IsError())
		{
			return FResult::Error(MoveTemp(UserRoot).GetError());
		}
		if (!bCreate)
		{
			return OpenNativeDirectoryAtNoFollow(
				UserRoot.GetValue(), TEXT("Preferences"));
		}
		bool bCreated = false;
		return OpenOrCreateNativeDirectoryAtNoFollow(
			UserRoot.GetValue(), TEXT("Preferences"), bCreated);
	}

	// Explicit automation and packaged-test seams resolve outside the canonical
	// R/Preferences pair and retain their isolated direct-directory behavior.
	if (!bCreate)
	{
		return OpenNativeDirectoryNoFollow(Resolved);
	}
	bool bCreated = false;
	TWoTBootstrapResult<FWoTCanonicalNativeDirectory> Canonical =
		OpenOrCreateCanonicalNativeDirectoryNoFollow(Resolved, bCreated);
	if (Canonical.IsError())
	{
		return FResult::Error(MoveTemp(Canonical).GetError());
	}
	return FResult::Success(MoveTemp(Canonical).GetValue().Directory);
}
}

bool WoTResolveUpdateFailureNoticeDirectory(
	FStringView UpdaterUserStateRoot, FString& OutDirectory)
{
	OutDirectory.Reset();
	#if PLATFORM_MAC && (WITH_AUTOMATION_TESTS || WOT_BOOTSTRAP_TEST_IDENTITIES)
	FString HarnessDirectory;
	if (FParse::Value(FCommandLine::Get(), TEXT("wottestpreferencesdir="),
		HarnessDirectory))
	{
		OutDirectory = MoveTemp(HarnessDirectory);
	}
	#endif
#if WITH_AUTOMATION_TESTS
	if (OutDirectory.IsEmpty()
		&& !GWoTUpdateFailureNoticeDirectoryForTesting.IsEmpty())
	{
		OutDirectory = GWoTUpdateFailureNoticeDirectoryForTesting;
	}
#endif
#if WITH_AUTOMATION_TESTS
	// A spec that never routed the notice must not write the developer's real
	// preferences: a leaked "update failed" notice would show in their game.
	// Under automation an unrouted notice lands in a per-process temp sandbox;
	// only specs that check the canonical policy opt back in, and they only
	// resolve it.
	if (OutDirectory.IsEmpty() && GIsAutomationTesting
		&& !GWoTAllowCanonicalUpdateFailureNoticeDirectoryForTesting)
	{
		OutDirectory = FPaths::Combine(FPlatformProcess::UserTempDir(),
			FString::Printf(TEXT("WoTNoticeSandbox-%u"),
				FPlatformProcess::GetCurrentProcessId()));
		IFileManager::Get().MakeDirectory(*OutDirectory, true);
	}
#endif
	if (OutDirectory.IsEmpty())
	{
		return WoTResolveNoticeDirectoryPolicy(
			UpdaterUserStateRoot, FStringView(), false, OutDirectory);
	}
	OutDirectory = FPaths::ConvertRelativePathToFull(OutDirectory);
	FPaths::NormalizeDirectoryName(OutDirectory);
	return !OutDirectory.IsEmpty() && !FPaths::IsRelative(OutDirectory)
		&& FPaths::CollapseRelativeDirectories(OutDirectory);
}

#if WITH_AUTOMATION_TESTS
void WoTAllowCanonicalUpdateFailureNoticeDirectoryForTesting(bool bAllow)
{
	GWoTAllowCanonicalUpdateFailureNoticeDirectoryForTesting = bAllow;
}

void WoTSetUpdateFailureNoticeDirectoryForTesting(FString Directory)
{
	if (!Directory.IsEmpty())
	{
		Directory = FPaths::ConvertRelativePathToFull(Directory);
		FPaths::NormalizeDirectoryName(Directory);
	}
	GWoTUpdateFailureNoticeDirectoryForTesting = MoveTemp(Directory);
}

bool WoTResolveUpdateFailureNoticeDirectoryForTesting(
	FStringView UpdaterUserStateRoot, FStringView CanonicalPreferencesDirectory,
	bool bMacPolicy, FString& OutDirectory)
{
	return WoTResolveNoticeDirectoryPolicy(UpdaterUserStateRoot,
		CanonicalPreferencesDirectory, bMacPolicy, OutDirectory);
}
#endif

FWoTBootstrapStatus RecordUpdateFailedNotice(const FWoTUpdaterStateRoot& Root,
	uint64 FailedReleaseSequence, uint64 RestoredReleaseSequence)
{
	// Every published update has a sequence, but the release it rolls back to
	// may be an unsequenced build (0: every release candidate so far).
	if (FailedReleaseSequence == 0)
	{
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::InvalidNumber,
			TEXT("the update-failed notice release identities are invalid")));
	}
	FString NoticeDirectory;
	if (!WoTResolveUpdateFailureNoticeDirectory(
			Root.UserStateRoot, NoticeDirectory))
	{
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::Io,
			TEXT("the update-failed notice directory could not be resolved")));
	}
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> OpenedDirectory =
		WoTOpenNoticeDirectory(Root, NoticeDirectory, true);
	if (OpenedDirectory.IsError())
	{
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::Io,
			TEXT("the update-failed notice directory could not be created"),
			NoticeDirectory));
	}
	FWoTNativeDirectoryHandle User =
		MoveTemp(OpenedDirectory).GetValue();
	TWoTBootstrapResult<TOptional<EWoTTreeEntryType>> Existing =
		WoTNoticeFind(User, WoTUpdateFailedNoticeLeaf);
	if (Existing.IsError())
		return FWoTBootstrapStatus::Error(MoveTemp(Existing).GetError());
	if (Existing.GetValue().IsSet())
	{
		if (Existing.GetValue().GetValue() != EWoTTreeEntryType::File)
		{
			return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::UnsafePath,
				TEXT("the update-failed notice marker is not a file")));
		}
		TWoTBootstrapResult<FWoTNativeFileHandle> Current =
			OpenNativeFileAtReadNoFollow(
				User, WoTUpdateFailedNoticeLeaf);
		if (Current.IsError())
			return FWoTBootstrapStatus::Error(MoveTemp(Current).GetError());
		TWoTBootstrapResult<TArray<uint8>> Bytes = Current.GetValue().ReadAll();
		if (Bytes.IsError())
			return FWoTBootstrapStatus::Error(MoveTemp(Bytes).GetError());
		if (WoTNoticeIsSeenFor(Bytes.GetValue(), FailedReleaseSequence))
			return FWoTBootstrapStatus::Success();
	}
	TWoTBootstrapResult<TOptional<EWoTTreeEntryType>> Stale =
		WoTNoticeFind(User, WoTUpdateFailedNoticeTempLeaf);
	if (Stale.IsError())
		return FWoTBootstrapStatus::Error(MoveTemp(Stale).GetError());
	if (Stale.GetValue().IsSet())
	{
		if (Stale.GetValue().GetValue() != EWoTTreeEntryType::File)
			return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::UnsafePath,
				TEXT("the update-failed notice temporary is not a file")));
		if (FWoTBootstrapStatus Removed = RemoveNativeEntryAtNoFollow(
				User, WoTUpdateFailedNoticeTempLeaf,
				EWoTTreeEntryType::File); Removed.IsError()) return Removed;
	}
	TWoTBootstrapResult<FWoTNativeFileHandle> Temporary =
		CreateNativeFileAtWriteNoFollow(
			User, WoTUpdateFailedNoticeTempLeaf);
	if (Temporary.IsError())
		return FWoTBootstrapStatus::Error(MoveTemp(Temporary).GetError());
	const FString Text = FString::Printf(
		TEXT("WUF1\nfailed=%llu\nrestored=%llu\nstate=pending\n"),
		FailedReleaseSequence, RestoredReleaseSequence);
	FTCHARToUTF8 Utf8(*Text);
	TArray<uint8> Bytes;
	Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	if (FWoTBootstrapStatus Written = WriteNativeFile(
			Temporary.GetValue(), Bytes); Written.IsError()) return Written;
	FWoTBootstrapStatus Flushed = FWoTBootstrapStatus::Success();
	Flushed = FlushNativeFile(Temporary.GetValue());
	if (Flushed.IsError()) return Flushed;
	Temporary.GetValue() = FWoTNativeFileHandle();
	FWoTBootstrapStatus Published = FWoTBootstrapStatus::Success();
	if (Existing.GetValue().IsSet())
	{
		Published = RenameNativeEntryAtNoFollow(User,
				WoTUpdateFailedNoticeTempLeaf, User,
				WoTUpdateFailedNoticeLeaf, true);
	}
	else
	{
		Published = RenameNativeEntryAtNoFollow(User,
				WoTUpdateFailedNoticeTempLeaf, User,
				WoTUpdateFailedNoticeLeaf, false);
	}
	if (Published.IsError()) return Published;
	FWoTBootstrapStatus DirectoryFlushed = FWoTBootstrapStatus::Success();
	DirectoryFlushed = FlushNativeDirectory(User);
	return DirectoryFlushed;
}

FWoTBootstrapStatus VerifyUpdateFailedNotice(const FWoTUpdaterStateRoot& Root,
	uint64 FailedReleaseSequence, uint64 RestoredReleaseSequence)
{
	if (FailedReleaseSequence == 0)
	{
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::InvalidNumber,
			TEXT("the update-failed notice verification identity is invalid")));
	}
	FString NoticeDirectory;
	if (!WoTResolveUpdateFailureNoticeDirectory(
			Root.UserStateRoot, NoticeDirectory))
	{
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::Io,
			TEXT("the update-failed notice verification directory could not be resolved")));
	}
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> User =
		WoTOpenNoticeDirectory(Root, NoticeDirectory, false);
	if (User.IsError())
		return FWoTBootstrapStatus::Error(MoveTemp(User).GetError());
	TWoTBootstrapResult<TOptional<EWoTTreeEntryType>> Found =
		WoTNoticeFind(User.GetValue(), WoTUpdateFailedNoticeLeaf);
	if (Found.IsError())
		return FWoTBootstrapStatus::Error(MoveTemp(Found).GetError());
	if (!Found.GetValue().IsSet()
		|| Found.GetValue().GetValue() != EWoTTreeEntryType::File)
	{
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("the update-failed notice was not durably published")));
	}
	TWoTBootstrapResult<FWoTNativeFileHandle> Current =
		OpenNativeFileAtReadNoFollow(User.GetValue(), WoTUpdateFailedNoticeLeaf);
	if (Current.IsError())
		return FWoTBootstrapStatus::Error(MoveTemp(Current).GetError());
	if (Current.GetValue().GetIdentity().LinkCount != 1)
	{
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::UnsafePath,
			TEXT("the update-failed notice is not single-link")));
	}
	TWoTBootstrapResult<TArray<uint8>> Bytes = Current.GetValue().ReadAll();
	if (Bytes.IsError())
		return FWoTBootstrapStatus::Error(MoveTemp(Bytes).GetError());
	if (!WoTNoticeMatchesFailure(Bytes.GetValue(), FailedReleaseSequence,
			RestoredReleaseSequence))
	{
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("the update-failed notice does not match the restored release")));
	}
	return FWoTBootstrapStatus::Success();
}

TWoTBootstrapResult<uint64> ReadUpdateFailedSequence(const FWoTUpdaterStateRoot& Root)
{
	using FResult = TWoTBootstrapResult<uint64>;
	FString DirectoryPath;
	if (!WoTResolveUpdateFailureNoticeDirectory(Root.UserStateRoot, DirectoryPath))
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Io, TEXT("cannot resolve update notice")));
	if (!IFileManager::Get().DirectoryExists(*DirectoryPath)) return FResult::Success(0);
	auto Directory = WoTOpenNoticeDirectory(Root, DirectoryPath, false);
	if (Directory.IsError()) return FResult::Error(MoveTemp(Directory).GetError());
	auto Found = WoTNoticeFind(Directory.GetValue(), WoTUpdateFailedNoticeLeaf);
	if (Found.IsError()) return FResult::Error(MoveTemp(Found).GetError());
	if (!Found.GetValue().IsSet()) return FResult::Success(0);
	auto File = OpenNativeFileAtReadNoFollow(Directory.GetValue(), WoTUpdateFailedNoticeLeaf);
	if (File.IsError()) return FResult::Error(MoveTemp(File).GetError());
	if (File.GetValue().GetIdentity().Size >= 256 || File.GetValue().GetIdentity().LinkCount != 1)
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::CorruptInstallation, TEXT("invalid update notice")));
	auto Bytes = File.GetValue().ReadAll();
	if (Bytes.IsError()) return FResult::Error(MoveTemp(Bytes).GetError());
	const FUTF8ToTCHAR Utf8(reinterpret_cast<const ANSICHAR*>(Bytes.GetValue().GetData()), Bytes.GetValue().Num());
	const FString Text(Utf8.Length(), Utf8.Get());
	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines);
	uint64 Sequence = 0;
	if (Lines.Num() >= 3 && Lines[0] == TEXT("WUF1") && Lines[1].StartsWith(TEXT("failed="))
		&& LexTryParseString(Sequence, *Lines[1].Mid(7)) && Sequence > 0
		&& Sequence <= static_cast<uint64>(MAX_int64)
		&& (Lines.Last() == TEXT("state=seen") || Lines.Last() == TEXT("state=pending")))
		return FResult::Success(Sequence);
	return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::CorruptInstallation, TEXT("invalid update notice")));
}

FWoTBootstrapStatus ClearUpdateFailedSequenceForNewerRelease(const FWoTUpdaterStateRoot& Root, uint64 AuthenticatedSequence)
{
	auto Failed = ReadUpdateFailedSequence(Root);
	if (Failed.IsError()) return FWoTBootstrapStatus::Error(MoveTemp(Failed).GetError());
	if (!Failed.GetValue() || AuthenticatedSequence <= Failed.GetValue()) return FWoTBootstrapStatus::Success();
	FString DirectoryPath;
	WoTResolveUpdateFailureNoticeDirectory(Root.UserStateRoot, DirectoryPath);
	auto Directory = WoTOpenNoticeDirectory(Root, DirectoryPath, false);
	if (Directory.IsError()) return FWoTBootstrapStatus::Error(MoveTemp(Directory).GetError());
	return RemoveNativeEntryAtNoFollow(Directory.GetValue(), WoTUpdateFailedNoticeLeaf, EWoTTreeEntryType::File);
}
