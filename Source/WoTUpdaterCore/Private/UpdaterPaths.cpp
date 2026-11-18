#include "WoTUpdaterCore/UpdaterPaths.h"

#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"
#include "HAL/FileManager.h"
#include "WoTUpdaterCore/UpdateFailureNotice.h"
#include "WoTBootstrapCore/NativeHandles.h"

namespace
{
TWoTBootstrapResult<FString> WoTCanonicalUpdaterRoot(
	FStringView Input, const TCHAR* Label, bool bAllowFilesystemRoot)
{
	using FResult = TWoTBootstrapResult<FString>;

	FString Canonical(Input);
	Canonical.TrimStartAndEndInline();
	if (Canonical.IsEmpty())
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::UnsafePath,
			FString::Printf(TEXT("the %s is empty"), Label)));
	}
	if (FPaths::IsRelative(Canonical))
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::UnsafePath,
			FString::Printf(TEXT("the %s must be absolute"), Label), Canonical));
	}
	Canonical = FPaths::ConvertRelativePathToFull(Canonical);
	const bool bPosixFilesystemRoot = Canonical == TEXT("/");
	const bool bWindowsFilesystemRoot = Canonical.Len() == 3
		&& FChar::IsAlpha(Canonical[0]) && Canonical[1] == TEXT(':')
		&& (Canonical[2] == TEXT('/') || Canonical[2] == TEXT('\\'));
	FPaths::NormalizeDirectoryName(Canonical);
	if (bPosixFilesystemRoot)
	{
		Canonical = TEXT("/");
	}
	else if (bWindowsFilesystemRoot && Canonical.Len() == 2)
	{
		Canonical += TEXT("/");
	}
	if (FPaths::IsRelative(Canonical) || !FPaths::CollapseRelativeDirectories(Canonical))
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::UnsafePath,
			FString::Printf(TEXT("the %s must be an absolute, non-escaping path"), Label),
			Canonical));
	}
	if (!bAllowFilesystemRoot && FPaths::GetPath(Canonical).IsEmpty())
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::UnsafePath,
			FString::Printf(TEXT("a filesystem root is not a %s"), Label), Canonical));
	}
	return FResult::Success(MoveTemp(Canonical));
}

const TCHAR* WoTDefaultLiveLeaf()
{
#if PLATFORM_MAC
	return TEXT("WoT.app");
#elif PLATFORM_WINDOWS
	return TEXT("Game");
#else
#error "updater paths are defined only for Win64 and Mac"
#endif
}
} // namespace

TWoTBootstrapResult<FWoTUpdaterStateRoot> ResolveUpdaterUserStateRoot(
	FStringView UserStateRoot)
{
	using FResult = TWoTBootstrapResult<FWoTUpdaterStateRoot>;
	TWoTBootstrapResult<FString> User = WoTCanonicalUpdaterRoot(
		UserStateRoot, TEXT("user state root"), false);
	if (User.IsError())
	{
		return FResult::Error(MoveTemp(User).GetError());
	}
	FWoTUpdaterStateRoot Root;
	Root.UserStateRoot = MoveTemp(User).GetValue();
	Root.StateDir = FPaths::Combine(Root.UserStateRoot, TEXT("UpdaterState"));
	FPaths::NormalizeDirectoryName(Root.StateDir);
	if (!FPaths::IsUnderDirectory(Root.StateDir, Root.UserStateRoot))
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::UnsafePath,
			TEXT("a per-user updater path escaped its owning root")));
	}
	return FResult::Success(MoveTemp(Root));
}

TWoTBootstrapResult<FWoTUpdaterStateRoot> ResolveUpdaterStateRoot(
	FStringView UserStateRoot, FStringView InstallRoot)
{
	return ResolveUpdaterStateRoot(
		UserStateRoot, InstallRoot, WoTDefaultLiveLeaf());
}

TWoTBootstrapResult<FWoTUpdaterStateRoot> ResolveUpdaterStateRoot(
	FStringView UserStateRoot, FStringView InstallRoot, FStringView LiveLeaf)
{
	using FResult = TWoTBootstrapResult<FWoTUpdaterStateRoot>;
	TWoTBootstrapResult<FWoTUpdaterStateRoot> User =
		ResolveUpdaterUserStateRoot(UserStateRoot);
	if (User.IsError())
	{
		return FResult::Error(MoveTemp(User).GetError());
	}
	TWoTBootstrapResult<FString> Install = WoTCanonicalUpdaterRoot(
		InstallRoot, TEXT("install root"), true);
	if (Install.IsError())
	{
		return FResult::Error(MoveTemp(Install).GetError());
	}
	if (LiveLeaf.IsEmpty() || LiveLeaf.Contains(TEXT("/"))
		|| LiveLeaf.Contains(TEXT("\\"))
		|| RequireSafeRelativePath(LiveLeaf).IsError())
	{
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::UnsafePath,
			TEXT("the live tree leaf is not one safe path component"),
			FString(LiveLeaf)));
	}
	if (LiveLeaf.Equals(TEXT(".wot-update"), ESearchCase::IgnoreCase))
	{
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::UnsafePath,
			TEXT("the live tree leaf is reserved for the updater workspace"),
			FString(LiveLeaf)));
	}
#if PLATFORM_MAC
	if (!LiveLeaf.EndsWith(TEXT(".app"), ESearchCase::CaseSensitive))
	{
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::UnsafePath,
			TEXT("the Mac live tree leaf is not an app bundle"),
			FString(LiveLeaf)));
	}
#endif

	FWoTUpdaterStateRoot Root = MoveTemp(User).GetValue();
	Root.InstallRoot = MoveTemp(Install).GetValue();
	Root.LiveLeaf = FString(LiveLeaf);
	Root.UpdateDir = FPaths::Combine(Root.InstallRoot, TEXT(".wot-update"));
	Root.StagingDir = FPaths::Combine(Root.UpdateDir, TEXT("staged"));
	Root.WorkDir = FPaths::Combine(Root.UpdateDir,
		FString::Printf(TEXT("staging-%u"), FPlatformProcess::GetCurrentProcessId()));
	Root.DownloadPath = FPaths::Combine(Root.WorkDir, TEXT("download.zip"));
	Root.StagedTreeDir = FPaths::Combine(Root.StagingDir, Root.LiveLeaf);
	Root.PreviousTreeDir = FPaths::Combine(Root.UpdateDir, TEXT("prev"), Root.LiveLeaf);
	Root.ReadyPath = FPaths::Combine(Root.UpdateDir, TEXT("ready"));
	Root.TrialPath = FPaths::Combine(Root.UpdateDir, TEXT("trial"));
	Root.RolledBackPath = FPaths::Combine(Root.UpdateDir, TEXT("rolled-back"));
	FPaths::NormalizeDirectoryName(Root.UpdateDir);
	FPaths::NormalizeDirectoryName(Root.StagingDir);

	const FString UpdateParent = FPaths::GetPath(Root.UpdateDir);
	const bool bPosixFilesystemRootParent = Root.InstallRoot == TEXT("/")
		&& Root.UpdateDir == TEXT("/.wot-update")
		&& UpdateParent.IsEmpty();
	const bool bVolumeRootParent = Root.InstallRoot.Len() == 3
		&& Root.InstallRoot[1] == TEXT(':')
		&& UpdateParent == Root.InstallRoot.Left(2);
	if (!FPaths::IsUnderDirectory(Root.StateDir, Root.UserStateRoot)
		|| (UpdateParent != Root.InstallRoot && !bVolumeRootParent
			&& !bPosixFilesystemRootParent)
		|| !FPaths::IsUnderDirectory(Root.StagingDir, Root.UpdateDir))
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::UnsafePath,
			TEXT("an updater path escaped its owning root")));
	}
	return FResult::Success(MoveTemp(Root));
}

TWoTBootstrapResult<FWoTUpdateMarker> ParseWoTUpdateMarker(
	TConstArrayView<uint8> Bytes, bool bTrial)
{
	using FResult = TWoTBootstrapResult<FWoTUpdateMarker>;
	auto Invalid = []() { return FResult::Error(MakeWoTBootstrapError(
		EWoTBootstrapError::CorruptInstallation, TEXT("the update marker is malformed"))); };
	if (Bytes.Num() < 2 || Bytes.Num() >= 64 || Bytes[0] < '1' || Bytes[0] > '9') return Invalid();
	FWoTUpdateMarker Marker;
	int32 Index = 0;
	while (Index < Bytes.Num() && Bytes[Index] >= '0' && Bytes[Index] <= '9')
	{
		const uint64 Digit = Bytes[Index++] - '0';
		if (Marker.Sequence > (static_cast<uint64>(MAX_int64) - Digit) / 10) return Invalid();
		Marker.Sequence = Marker.Sequence * 10 + Digit;
	}
	if (bTrial)
	{
		if (Index >= Bytes.Num() || Bytes[Index++] != ' ') return Invalid();
		const char* Expected = Index < Bytes.Num() && Bytes[Index] == 'p' ? "pending" : "started";
		Marker.State = *Expected == 'p' ? EWoTUpdateTrialState::Pending : EWoTUpdateTrialState::Started;
		while (*Expected) if (Index >= Bytes.Num() || Bytes[Index++] != *Expected++) return Invalid();
	}
	if (Index + 1 != Bytes.Num() || Bytes[Index] != '\n') return Invalid();
	return FResult::Success(Marker);
}

TWoTBootstrapResult<TOptional<FWoTUpdateMarker>> ReadWoTUpdateMarker(
	const FWoTUpdaterStateRoot& Root, FStringView Leaf, bool bTrial)
{
	using FResult = TWoTBootstrapResult<TOptional<FWoTUpdateMarker>>;
	if (!IFileManager::Get().DirectoryExists(*Root.UpdateDir)) return FResult::Success(TOptional<FWoTUpdateMarker>());
	auto Directory = OpenNativeDirectoryNoFollow(Root.UpdateDir);
	if (Directory.IsError()) return FResult::Error(MoveTemp(Directory).GetError());
	auto Entries = EnumerateNativeDirectory(Directory.GetValue());
	if (Entries.IsError()) return FResult::Error(MoveTemp(Entries).GetError());
	if (!Entries.GetValue().ContainsByPredicate([Leaf](const FWoTNativeDirectoryEntry& Entry) { return Entry.Name == Leaf; }))
		return FResult::Success(TOptional<FWoTUpdateMarker>());
	bool bStale = false;
	TOptional<FWoTUpdateMarker> Marker;
	{
		auto File = OpenNativeFileAtReadNoFollow(Directory.GetValue(), Leaf);
		if (File.IsError()) return FResult::Error(MoveTemp(File).GetError());
		if (File.GetValue().GetIdentity().Size >= 64 || File.GetValue().GetIdentity().LinkCount != 1)
		{
			bStale = true;
		}
		else
		{
			auto Bytes = File.GetValue().ReadAll();
			if (Bytes.IsError()) return FResult::Error(MoveTemp(Bytes).GetError());
			auto Parsed = ParseWoTUpdateMarker(Bytes.GetValue(), bTrial);
			if (Parsed.IsError()) bStale = true;
			else Marker = Parsed.GetValue();
		}
	}
	if (bStale)
	{
		// A torn write (full disk, crash, power loss) leaves a malformed marker.
		// It is stale: drop it rather than refusing every later start.
		auto Removed = RemoveWoTUpdateEntry(Directory.GetValue(), Leaf);
		if (Removed.IsError()) return FResult::Error(MoveTemp(Removed).GetError());
		return FResult::Success(TOptional<FWoTUpdateMarker>());
	}
	return FResult::Success(MoveTemp(Marker));
}

FWoTBootstrapStatus WriteWoTUpdateMarker(const FWoTUpdaterStateRoot& Root,
	FStringView Leaf, const FWoTUpdateMarker& Marker)
{
	if (!Marker.Sequence || Marker.Sequence > static_cast<uint64>(MAX_int64))
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(EWoTBootstrapError::InvalidNumber, TEXT("invalid update sequence")));
	auto Directory = OpenNativeDirectoryNoFollow(Root.UpdateDir);
	if (Directory.IsError()) return FWoTBootstrapStatus::Error(MoveTemp(Directory).GetError());
	// Write a sibling temporary and rename it over the marker, so a full disk,
	// crash or power loss leaves either the old marker or the complete new one.
	const FString TempLeaf = FString(Leaf) + TEXT(".tmp");
	auto Cleared = RemoveWoTUpdateEntry(Directory.GetValue(), TempLeaf);
	if (Cleared.IsError()) return Cleared;
	const TCHAR* State = Marker.State == EWoTUpdateTrialState::Pending ? TEXT(" pending")
		: Marker.State == EWoTUpdateTrialState::Started ? TEXT(" started") : TEXT("");
	const FString Text = FString::Printf(TEXT("%llu%s\n"), Marker.Sequence, State);
	FTCHARToUTF8 Utf8(*Text);
	{
		auto File = CreateNativeFileAtWriteNoFollow(Directory.GetValue(), TempLeaf);
		if (File.IsError()) return FWoTBootstrapStatus::Error(MoveTemp(File).GetError());
		auto Written = WriteNativeFile(File.GetValue(), MakeArrayView(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()));
		if (Written.IsError())
		{
			(void)RemoveWoTUpdateEntry(Directory.GetValue(), TempLeaf);
			return Written;
		}
		auto Flushed = FlushNativeFile(File.GetValue());
		if (Flushed.IsError())
		{
			(void)RemoveWoTUpdateEntry(Directory.GetValue(), TempLeaf);
			return Flushed;
		}
	}
	auto Renamed = RenameNativeEntryAtNoFollow(Directory.GetValue(), TempLeaf,
		Directory.GetValue(), Leaf, true);
	if (Renamed.IsError()) (void)RemoveWoTUpdateEntry(Directory.GetValue(), TempLeaf);
	return Renamed;
}

FWoTBootstrapStatus RemoveWoTUpdateEntry(const FWoTNativeDirectoryHandle& Parent, FStringView Leaf)
{
	auto Entries = EnumerateNativeDirectory(Parent);
	if (Entries.IsError()) return FWoTBootstrapStatus::Error(MoveTemp(Entries).GetError());
	for (const auto& Entry : Entries.GetValue())
	{
		if (Entry.Name != Leaf) continue;
		if (Entry.Type == EWoTTreeEntryType::Directory)
		{
			auto Directory = OpenNativeDirectoryAtNoFollow(Parent, Leaf);
			if (Directory.IsError()) return FWoTBootstrapStatus::Error(MoveTemp(Directory).GetError());
			auto Children = EnumerateNativeDirectory(Directory.GetValue());
			if (Children.IsError()) return FWoTBootstrapStatus::Error(MoveTemp(Children).GetError());
			for (const auto& Child : Children.GetValue())
			{
				auto Removed = RemoveWoTUpdateEntry(Directory.GetValue(), Child.Name);
				if (Removed.IsError()) return Removed;
			}
		}
		return RemoveNativeEntryAtNoFollow(Parent, Leaf, Entry.Type);
	}
	return FWoTBootstrapStatus::Success();
}

FWoTBootstrapStatus WoTMarkUpdateTrialStarted(const FWoTUpdaterStateRoot& Root)
{
	auto Trial = ReadWoTUpdateMarker(Root, TEXT("trial"), true);
	if (Trial.IsError()) return FWoTBootstrapStatus::Error(MoveTemp(Trial).GetError());
	if (!Trial.GetValue().IsSet() || Trial.GetValue()->State != EWoTUpdateTrialState::Pending)
		return FWoTBootstrapStatus::Success();
	return WriteWoTUpdateMarker(Root, TEXT("trial"), {Trial.GetValue()->Sequence, EWoTUpdateTrialState::Started});
}

FWoTBootstrapStatus WoTConsumeUpdateRollback(const FWoTUpdaterStateRoot& Root, uint64 RunningSequence)
{
	auto RolledBack = ReadWoTUpdateMarker(Root, TEXT("rolled-back"));
	if (RolledBack.IsError()) return FWoTBootstrapStatus::Error(MoveTemp(RolledBack).GetError());
	if (!RolledBack.GetValue().IsSet()) return FWoTBootstrapStatus::Success();
	auto Recorded = RecordUpdateFailedNotice(Root, RolledBack.GetValue()->Sequence, RunningSequence);
	if (Recorded.IsError()) return Recorded;
	auto Directory = OpenNativeDirectoryNoFollow(Root.UpdateDir);
	if (Directory.IsError()) return FWoTBootstrapStatus::Error(MoveTemp(Directory).GetError());
	return RemoveWoTUpdateEntry(Directory.GetValue(), TEXT("rolled-back"));
}

FWoTBootstrapStatus WoTCompleteUpdateTrial(const FWoTUpdaterStateRoot& Root, uint64 RunningSequence)
{
	auto Trial = ReadWoTUpdateMarker(Root, TEXT("trial"), true);
	if (Trial.IsError()) return FWoTBootstrapStatus::Error(MoveTemp(Trial).GetError());
	if (!Trial.GetValue().IsSet() || Trial.GetValue()->Sequence != RunningSequence)
		return FWoTBootstrapStatus::Success();
	auto Directory = OpenNativeDirectoryNoFollow(Root.UpdateDir);
	if (Directory.IsError()) return FWoTBootstrapStatus::Error(MoveTemp(Directory).GetError());
	auto Removed = RemoveWoTUpdateEntry(Directory.GetValue(), TEXT("trial"));
	if (Removed.IsError()) return Removed;
	// Trial retirement is authoritative; previous-tree cleanup is best effort.
	(void)RemoveWoTUpdateEntry(Directory.GetValue(), TEXT("prev"));
	return FWoTBootstrapStatus::Success();
}
