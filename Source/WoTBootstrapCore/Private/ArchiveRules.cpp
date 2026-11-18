#include "WoTBootstrapCore/ArchiveRules.h"

#include "WoTBootstrapCore/InstalledTree.h"
#include "WoTBootstrapCore/NativeHandles.h"

namespace
{
/**
 * Names Windows resolves to devices no matter which directory they are in.
 *
 * Extracting to one of these does not create a file — it opens a device, which can
 * hang, consume the stream, or have side effects. They are refused on BOTH
 * platforms so that one archive is valid or invalid everywhere, rather than
 * quietly meaning different things per host.
 */
const TCHAR* const ReservedDeviceNames[] = {
	TEXT("con"), TEXT("prn"), TEXT("aux"), TEXT("nul"),
	TEXT("com1"), TEXT("com2"), TEXT("com3"), TEXT("com4"), TEXT("com5"),
	TEXT("com6"), TEXT("com7"), TEXT("com8"), TEXT("com9"),
	TEXT("lpt1"), TEXT("lpt2"), TEXT("lpt3"), TEXT("lpt4"), TEXT("lpt5"),
	TEXT("lpt6"), TEXT("lpt7"), TEXT("lpt8"), TEXT("lpt9"),
};

bool IsReservedDeviceName(const FString& Component)
{
	// The stem before the first dot is what Windows matches, so "con.txt" counts.
	FString Stem = Component;
	int32 Dot = INDEX_NONE;
	if (Stem.FindChar(TEXT('.'), Dot))
	{
		Stem.LeftInline(Dot);
	}
	Stem.ToLowerInline();
	for (const TCHAR* Reserved : ReservedDeviceNames)
	{
		if (Stem.Equals(Reserved, ESearchCase::CaseSensitive))
		{
			return true;
		}
	}
	return false;
}

FString TopLevelComponent(const FString& Path)
{
	int32 Slash = INDEX_NONE;
	if (Path.FindChar(TEXT('/'), Slash))
	{
		return Path.Left(Slash);
	}
	return Path;
}

int32 ComponentDepth(const FString& Path)
{
	int32 Depth = 1;
	for (const TCHAR Character : Path)
	{
		Depth += Character == TEXT('/') ? 1 : 0;
	}
	return Depth;
}

bool IsApprovedArchiveComponent(const FString& Component)
{
	// Release archives are produced by us and have no need for platform-dependent
	// Unicode normalization. This deliberately small set covers release file names
	// while having one byte spelling and one path spelling on every supported host.
	for (const TCHAR Character : Component)
	{
		const bool bAlphaNumeric = (Character >= TEXT('a') && Character <= TEXT('z'))
			|| (Character >= TEXT('A') && Character <= TEXT('Z'))
			|| (Character >= TEXT('0') && Character <= TEXT('9'));
		const bool bApprovedPunctuation = Character == TEXT(' ') || Character == TEXT('.')
			|| Character == TEXT('_') || Character == TEXT('-') || Character == TEXT('+')
			|| Character == TEXT('(') || Character == TEXT(')') || Character == TEXT('[')
			|| Character == TEXT(']') || Character == TEXT('@');
		if (!bAlphaNumeric && !bApprovedPunctuation)
		{
			return false;
		}
	}
	return true;
}

bool IsPathAncestorOrEqual(const FString& CandidateAncestor, const FString& Path)
{
	return Path == CandidateAncestor
		|| Path.StartsWith(CandidateAncestor + TEXT("/"), ESearchCase::CaseSensitive);
}

/** Resolve a link target relative to the link's own directory, refusing escapes. */
bool ResolveLinkTargetWithinTree(const FString& LinkPath, const FString& Target,
	FString& OutResolved)
{
	TArray<FString> Components;
	LinkPath.ParseIntoArray(Components, TEXT("/"), true);
	if (Components.Num() > 0)
	{
		Components.Pop();  // the link's own name
	}

	TArray<FString> TargetComponents;
	Target.ParseIntoArray(TargetComponents, TEXT("/"), true);
	for (const FString& Component : TargetComponents)
	{
		if (Component == TEXT("."))
		{
			continue;
		}
		if (Component == TEXT(".."))
		{
			if (Components.Num() == 0)
			{
				// Climbing past the archive root is the escape this exists to stop.
				return false;
			}
			Components.Pop();
			continue;
		}
		Components.Add(Component);
	}
	if (Components.Num() == 0)
	{
		return false;
	}
	OutResolved = FString::Join(Components, TEXT("/"));
	return true;
}
}  // namespace

const TCHAR* LexToString(EWoTArchiveLayout Layout)
{
	switch (Layout)
	{
	case EWoTArchiveLayout::Win64VersionTree: return TEXT("Win64VersionTree");
	case EWoTArchiveLayout::MacAppBundle:     return TEXT("MacAppBundle");
	}
	return TEXT("UnknownArchiveLayout");
}

TWoTBootstrapResult<FWoTSafeArchivePlan> ValidateArchiveIndex(
	TConstArrayView<FWoTArchiveEntry> Entries, const FWoTArchiveLimits& Limits,
	EWoTArchiveLayout Layout)
{
	using FResult = TWoTBootstrapResult<FWoTSafeArchivePlan>;

	const auto Unsafe = [](const TCHAR* Why, const FString& Path)
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::UnsafePath, Why, Path));
	};

	if (Entries.Num() == 0)
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::UnknownField,
			TEXT("the archive index is empty")));
	}
	if (static_cast<uint64>(Entries.Num()) > Limits.MaxEntries)
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::SizeLimit,
			FString::Printf(TEXT("the archive declares %d entries, over the %u limit"),
				Entries.Num(), Limits.MaxEntries)));
	}

	FWoTSafeArchivePlan Plan;
	Plan.Layout = Layout;

	TSet<FString> SeenPaths;
	TSet<FString> SeenFolded;
	TSet<FString> DirectoryPaths;
	TSet<FString> SymlinkPaths;
	uint64 TotalBytes = 0;

	for (const FWoTArchiveEntry& Entry : Entries)
	{
		// The same rule the installed tree uses, so an archive cannot contain a path
		// the tree would later refuse.
		if (FWoTBootstrapStatus Safe = RequireSafeRelativePath(Entry.Path); Safe.IsError())
		{
			return FResult::Error(MoveTemp(Safe).GetError());
		}
		if (ComponentDepth(Entry.Path) > static_cast<int32>(Limits.MaxDepth))
		{
			return Unsafe(TEXT("the entry is nested deeper than the archive limit"), Entry.Path);
		}

		TArray<FString> Components;
		Entry.Path.ParseIntoArray(Components, TEXT("/"), true);
		for (const FString& Component : Components)
		{
			if (IsAppleDoublePathComponent(Component))
			{
				return Unsafe(TEXT("the archive contains AppleDouble metadata"),
					Entry.Path);
			}
			if (!IsApprovedArchiveComponent(Component))
			{
				return Unsafe(TEXT("an archive path component is outside the approved ASCII set"),
					Entry.Path);
			}
			if (IsReservedDeviceName(Component))
			{
				return Unsafe(TEXT("the entry names a reserved device"), Entry.Path);
			}
		}

		if (SeenPaths.Contains(Entry.Path))
		{
			return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::DuplicateField,
				TEXT("the archive lists one path twice"), Entry.Path));
		}
		// Two entries differing only by case would be ONE file on a case-insensitive
		// volume: extraction order would decide the content, which is not a decision
		// extraction gets to make.
		const FString Folded = Entry.Path.ToLower();
		if (SeenFolded.Contains(Folded))
		{
			return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::DuplicateField,
				TEXT("two archive paths differ only by case"), Entry.Path));
		}
		SeenPaths.Add(Entry.Path);
		SeenFolded.Add(Folded);

		if (Entry.Type == EWoTTreeEntryType::Directory)
		{
			DirectoryPaths.Add(Entry.Path);
		}

		if (Entry.Size > Limits.MaxEntryBytes)
		{
			return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::SizeLimit,
				TEXT("an archive entry exceeds the per-entry limit"), Entry.Path));
		}
		// A single entry may exceed the total limit even when the per-entry limit is
		// looser. Check it before subtracting, then check the running total before
		// adding, so neither unsigned operation can wrap into looking acceptable.
		if (Entry.Size > Limits.MaxTotalBytes)
		{
			return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::SizeLimit,
				TEXT("an archive entry exceeds the total size limit"), Entry.Path));
		}
		if (TotalBytes > Limits.MaxTotalBytes - Entry.Size)
		{
			return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::SizeLimit,
				TEXT("the archive exceeds its total size limit"), Entry.Path));
		}
		TotalBytes += Entry.Size;

		if (Entry.Type == EWoTTreeEntryType::SymbolicLink)
		{
			SymlinkPaths.Add(Entry.Path);
			if (Layout == EWoTArchiveLayout::Win64VersionTree)
			{
				// No links at all on Windows: junctions and symlinks there are
				// reparse points with privilege and cross-volume behaviour that a
				// release archive has no business requesting.
				return Unsafe(TEXT("a Win64 archive may not contain links"), Entry.Path);
			}
			if (Entry.LinkTarget.IsEmpty())
			{
				return Unsafe(TEXT("a link entry names no target"), Entry.Path);
			}
			if (Entry.LinkTarget.StartsWith(TEXT("/"), ESearchCase::CaseSensitive))
			{
				return Unsafe(TEXT("an archive link target is absolute"), Entry.Path);
			}
			FString Resolved;
			if (!ResolveLinkTargetWithinTree(Entry.Path, Entry.LinkTarget, Resolved))
			{
				return Unsafe(TEXT("an archive link escapes the archive root"), Entry.Path);
			}
			if (Resolved == Entry.Path)
			{
				return Unsafe(TEXT("an archive link points at itself"), Entry.Path);
			}
			if (IsPathAncestorOrEqual(Resolved, Entry.Path))
			{
				return Unsafe(TEXT("an archive link points at its own ancestor"), Entry.Path);
			}
		}
		else if (!Entry.LinkTarget.IsEmpty())
		{
			return Unsafe(TEXT("a non-link entry names a link target"), Entry.Path);
		}
	}

	// Exactly one top-level object. A second root means the archive would write
	// somewhere the release identity does not describe.
	const FString Root = TopLevelComponent(Entries[0].Path);
	for (const FWoTArchiveEntry& Entry : Entries)
	{
		if (TopLevelComponent(Entry.Path) != Root)
		{
			return Unsafe(TEXT("the archive has more than one top-level entry"), Entry.Path);
		}
	}
	Plan.RootName = Root;

	if (Layout == EWoTArchiveLayout::MacAppBundle)
	{
		if (!Root.EndsWith(TEXT(".app"), ESearchCase::CaseSensitive))
		{
			return Unsafe(TEXT("a Mac archive root must be an .app bundle"), Root);
		}
		// Links are permitted only INSIDE the bundle — which the resolve step above
		// already guaranteed, since the bundle is the archive root.
	}
	else if (Root.EndsWith(TEXT(".app"), ESearchCase::CaseSensitive))
	{
		return Unsafe(TEXT("a Win64 archive root must not be an .app bundle"), Root);
	}

	// Second pass now that every directory is known: a link must resolve to
	// something the archive actually contains.
	for (const FWoTArchiveEntry& Entry : Entries)
	{
		for (const FString& SymlinkPath : SymlinkPaths)
		{
			if (Entry.Path != SymlinkPath && IsPathAncestorOrEqual(SymlinkPath, Entry.Path))
			{
				return Unsafe(TEXT("an archive entry traverses a symbolic link"), Entry.Path);
			}
		}
		if (Entry.Type != EWoTTreeEntryType::SymbolicLink)
		{
			continue;
		}
		FString Resolved;
		ResolveLinkTargetWithinTree(Entry.Path, Entry.LinkTarget, Resolved);
		if (!SeenPaths.Contains(Resolved))
		{
			return Unsafe(TEXT("an archive link points outside the archive contents"),
				Entry.Path);
		}
		// A link to a link can be made to cycle, and following one during extraction
		// would not terminate. Only links to real objects are permitted.
		for (const FWoTArchiveEntry& Candidate : Entries)
		{
			if (Candidate.Path == Resolved
				&& Candidate.Type == EWoTTreeEntryType::SymbolicLink)
			{
				return Unsafe(TEXT("an archive link points at another link"), Entry.Path);
			}
		}
	}

	Plan.TotalBytes = TotalBytes;
	Plan.Entries.Append(Entries.GetData(), Entries.Num());
	return FResult::Success(MoveTemp(Plan));
}
