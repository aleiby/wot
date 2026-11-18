// Specs for resumable-download policy and safe-archive validation.
//
// The resume specs are driven by the SAME file the Python downloader is driven by,
// so "the two implementations agree" is a measured fact rather than a claim. If
// either side drifts, one of the two suites stops matching the vectors.
//
// The archive specs are all one question: what could this index do to the
// filesystem that the release did not intend?
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "WoTBootstrapCore/ArchiveRules.h"
#include "WoTBootstrapCore/CanonicalJson.h"

#if WITH_AUTOMATION_TESTS

namespace
{
constexpr EAutomationTestFlags WoTTransferSpecFlags =
	EAutomationTestFlags::ProgramContext | EAutomationTestFlags::EditorContext
	| EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter;

FString WoTTransferSpecVectorPath()
{
	// NOT FPaths::ProjectDir(): this suite is a Program target, so ProjectDir
	// resolves to Engine/Programs/WoTBootstrapCoreTests/ — nowhere near the repo —
	// and the vector read fails. That is platform-independent (it failed on
	// Win64; it would fail the same way anywhere), so the path is derived from
	// this translation unit's own location instead, which is exact by
	// construction: the fixture sits in Fixtures/ beside this file.
	const FString ThisFile(ANSI_TO_TCHAR(__FILE__));
	const FString Derived =
		FPaths::GetPath(ThisFile) / TEXT("Fixtures/download-resume-v1.json");
	if (FPaths::FileExists(Derived))
	{
		return Derived;
	}
	// Fallback for a binary run away from its source tree.
	return FPaths::ProjectDir()
		/ TEXT("Source/WoTBootstrapCore/Private/Tests/Fixtures/download-resume-v1.json");
}

FWoTArchiveEntry WoTTransferSpecFile(const TCHAR* Path, uint64 Size = 16)
{
	FWoTArchiveEntry Entry;
	Entry.Path = Path;
	Entry.Type = EWoTTreeEntryType::File;
	Entry.Size = Size;
	Entry.Mode = 420;
	return Entry;
}

FWoTArchiveEntry WoTTransferSpecDirectory(const TCHAR* Path)
{
	FWoTArchiveEntry Entry;
	Entry.Path = Path;
	Entry.Type = EWoTTreeEntryType::Directory;
	Entry.Mode = 493;
	return Entry;
}

FWoTArchiveEntry WoTTransferSpecLink(const TCHAR* Path, const TCHAR* Target)
{
	FWoTArchiveEntry Entry;
	Entry.Path = Path;
	Entry.Type = EWoTTreeEntryType::SymbolicLink;
	Entry.LinkTarget = Target;
	return Entry;
}

/** A minimal valid Mac bundle: root, a nested directory, and a file. */
TArray<FWoTArchiveEntry> WoTTransferSpecMacBundle()
{
	return {
		WoTTransferSpecDirectory(TEXT("WoT.app")),
		WoTTransferSpecDirectory(TEXT("WoT.app/Contents")),
		WoTTransferSpecDirectory(TEXT("WoT.app/Contents/MacOS")),
		WoTTransferSpecFile(TEXT("WoT.app/Contents/MacOS/WoT"), 2048),
		WoTTransferSpecFile(TEXT("WoT.app/Contents/Info.plist"), 512),
	};
}

TArray<FWoTArchiveEntry> WoTTransferSpecWindowsTree()
{
	return {
		WoTTransferSpecDirectory(TEXT("WoT-Win64")),
		WoTTransferSpecDirectory(TEXT("WoT-Win64/Engine")),
		WoTTransferSpecFile(TEXT("WoT-Win64/Engine/core.dll"), 4096),
		WoTTransferSpecFile(TEXT("WoT-Win64/content.pak"), 8192),
	};
}
}  // namespace


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTTransferArchiveSpec,
	"WoT.BootstrapCore.DownloadArchive.ArchiveRules", WoTTransferSpecFlags)

bool FWoTTransferArchiveSpec::RunTest(const FString&)
{
	const FWoTArchiveLimits Limits;
	{
		TArray<FWoTArchiveEntry> TopLevelAppleDouble =
			WoTTransferSpecMacBundle();
		TopLevelAppleDouble.Add(WoTTransferSpecDirectory(TEXT("__MACOSX")));
		TWoTBootstrapResult<FWoTSafeArchivePlan> TopLevelRefused =
			ValidateArchiveIndex(TopLevelAppleDouble, Limits,
				EWoTArchiveLayout::MacAppBundle);
		if (TestTrue(TEXT("top-level __MACOSX metadata is refused before extraction"),
			TopLevelRefused.IsError()))
		{
			TestTrue(TEXT("top-level __MACOSX is refused by the AppleDouble rule"),
				TopLevelRefused.GetError().Message.Contains(TEXT("AppleDouble")));
		}
		TArray<FWoTArchiveEntry> NestedAppleDouble =
			WoTTransferSpecMacBundle();
		NestedAppleDouble.Add(WoTTransferSpecFile(
			TEXT("WoT.app/Contents/._Info.plist")));
		TestTrue(TEXT("nested AppleDouble metadata is refused before extraction"),
			ValidateArchiveIndex(NestedAppleDouble, Limits,
				EWoTArchiveLayout::MacAppBundle).IsError());
	}

	// The two required layouts.
	{
		TWoTBootstrapResult<FWoTSafeArchivePlan> Mac = ValidateArchiveIndex(
			WoTTransferSpecMacBundle(), Limits, EWoTArchiveLayout::MacAppBundle);
		if (TestFalse(TEXT("a Mac bundle validates"), Mac.IsError()))
		{
			TestEqual(TEXT("its root is the bundle"), Mac.GetValue().RootName,
				FString(TEXT("WoT.app")));
			TestEqual(TEXT("its total size is summed"),
				static_cast<int64>(Mac.GetValue().TotalBytes), static_cast<int64>(2560));
		}

		TWoTBootstrapResult<FWoTSafeArchivePlan> Windows = ValidateArchiveIndex(
			WoTTransferSpecWindowsTree(), Limits, EWoTArchiveLayout::Win64VersionTree);
		if (TestFalse(TEXT("a Win64 version tree validates"), Windows.IsError()))
		{
			TestEqual(TEXT("its root is the version tree"), Windows.GetValue().RootName,
				FString(TEXT("WoT-Win64")));
		}

		// A layout is required, not inferred: the same entries in the wrong layout
		// are refused rather than accepted as "close enough".
		TestTrue(TEXT("a Mac bundle is not a Win64 tree"),
			ValidateArchiveIndex(WoTTransferSpecMacBundle(), Limits,
				EWoTArchiveLayout::Win64VersionTree).IsError());
		TestTrue(TEXT("a Win64 tree is not a Mac bundle"),
			ValidateArchiveIndex(WoTTransferSpecWindowsTree(), Limits,
				EWoTArchiveLayout::MacAppBundle).IsError());
	}

	// Links: allowed inside a Mac bundle, never on Windows.
	{
		TArray<FWoTArchiveEntry> WithLink = WoTTransferSpecMacBundle();
		WithLink.Add(WoTTransferSpecLink(TEXT("WoT.app/Contents/Current"),
			TEXT("MacOS/WoT")));
		TestFalse(TEXT("a bundle-internal link is allowed on Mac"),
			ValidateArchiveIndex(WithLink, Limits, EWoTArchiveLayout::MacAppBundle)
				.IsError());

		TArray<FWoTArchiveEntry> WindowsLink = WoTTransferSpecWindowsTree();
		WindowsLink.Add(WoTTransferSpecLink(TEXT("WoT-Win64/alias"),
			TEXT("content.pak")));
		TestTrue(TEXT("a Win64 archive may contain no links at all"),
			ValidateArchiveIndex(WindowsLink, Limits, EWoTArchiveLayout::Win64VersionTree)
				.IsError());

		// Escapes, dangling targets, self-references, and link chains.
		TArray<FWoTArchiveEntry> Escaping = WoTTransferSpecMacBundle();
		Escaping.Add(WoTTransferSpecLink(TEXT("WoT.app/Contents/escape"),
			TEXT("../../../../etc/passwd")));
		TestTrue(TEXT("a link escaping the archive root is refused"),
			ValidateArchiveIndex(Escaping, Limits, EWoTArchiveLayout::MacAppBundle)
				.IsError());

		TArray<FWoTArchiveEntry> Absolute = WoTTransferSpecMacBundle();
		Absolute.Add(WoTTransferSpecLink(TEXT("WoT.app/Contents/abs"), TEXT("/etc/passwd")));
		TestTrue(TEXT("an absolute link target is refused"),
			ValidateArchiveIndex(Absolute, Limits, EWoTArchiveLayout::MacAppBundle)
				.IsError());

		TArray<FWoTArchiveEntry> Dangling = WoTTransferSpecMacBundle();
		Dangling.Add(WoTTransferSpecLink(TEXT("WoT.app/Contents/dangling"),
			TEXT("MacOS/absent")));
		TestTrue(TEXT("a link to something not in the archive is refused"),
			ValidateArchiveIndex(Dangling, Limits, EWoTArchiveLayout::MacAppBundle)
				.IsError());

		TArray<FWoTArchiveEntry> Chain = WoTTransferSpecMacBundle();
		Chain.Add(WoTTransferSpecLink(TEXT("WoT.app/Contents/first"), TEXT("MacOS/WoT")));
		Chain.Add(WoTTransferSpecLink(TEXT("WoT.app/Contents/second"), TEXT("first")));
		TestTrue(TEXT("a link to another link is refused"),
			ValidateArchiveIndex(Chain, Limits, EWoTArchiveLayout::MacAppBundle).IsError());

		TArray<FWoTArchiveEntry> SelfLink = WoTTransferSpecMacBundle();
		SelfLink.Add(WoTTransferSpecLink(TEXT("WoT.app/Contents/loop"), TEXT("loop")));
		TestTrue(TEXT("a self-referential link is refused"),
			ValidateArchiveIndex(SelfLink, Limits, EWoTArchiveLayout::MacAppBundle)
				.IsError());

		TArray<FWoTArchiveEntry> ParentLoop = WoTTransferSpecMacBundle();
		ParentLoop.Add(WoTTransferSpecLink(TEXT("WoT.app/Contents/loop"), TEXT(".")));
		TestTrue(TEXT("a link to its own parent directory is refused"),
			ValidateArchiveIndex(ParentLoop, Limits, EWoTArchiveLayout::MacAppBundle)
				.IsError());

		TArray<FWoTArchiveEntry> TraversedLink = WoTTransferSpecMacBundle();
		TraversedLink.Add(WoTTransferSpecDirectory(TEXT("WoT.app/Contents/Real")));
		TraversedLink.Add(WoTTransferSpecLink(TEXT("WoT.app/Contents/Alias"), TEXT("Real")));
		TraversedLink.Add(WoTTransferSpecFile(TEXT("WoT.app/Contents/Alias/payload.bin")));
		TestTrue(TEXT("an entry path may not traverse a symlink"),
			ValidateArchiveIndex(TraversedLink, Limits, EWoTArchiveLayout::MacAppBundle)
				.IsError());
	}

	// Paths.
	{
		const auto RefusedMac = [this, &Limits](const TCHAR* Path, const TCHAR* Why)
		{
			TArray<FWoTArchiveEntry> Entries = WoTTransferSpecMacBundle();
			Entries.Add(WoTTransferSpecFile(Path));
			TestTrue(FString::Printf(TEXT("%s is refused"), Why),
				ValidateArchiveIndex(Entries, Limits, EWoTArchiveLayout::MacAppBundle)
					.IsError());
		};

		RefusedMac(TEXT("/etc/passwd"), TEXT("an absolute path"));
		RefusedMac(TEXT("WoT.app/../escape"), TEXT("a traversing path"));
		RefusedMac(TEXT("WoT.app/Contents/stream:name"), TEXT("an alternate data stream"));
		RefusedMac(TEXT("WoT.app/Contents\\back"), TEXT("a backslash separator"));
		RefusedMac(TEXT("WoT.app/Contents/con"), TEXT("a reserved device name"));
		RefusedMac(TEXT("WoT.app/Contents/CON.txt"), TEXT("a reserved device stem"));
		RefusedMac(TEXT("WoT.app/Contents/lpt1"), TEXT("a reserved printer device"));
		RefusedMac(TEXT("WoT.app/Contents/trailing."), TEXT("a trailing dot"));

		FString Precomposed = TEXT("WoT.app/Contents/");
		Precomposed.AppendChar(static_cast<TCHAR>(0x00E9));
		RefusedMac(*Precomposed, TEXT("a precomposed non-ASCII name"));
		FString Decomposed = TEXT("WoT.app/Contents/e");
		Decomposed.AppendChar(static_cast<TCHAR>(0x0301));
		RefusedMac(*Decomposed, TEXT("a decomposed non-ASCII name"));

		for (const TCHAR Superscript : {static_cast<TCHAR>(0x00B9),
			static_cast<TCHAR>(0x00B2), static_cast<TCHAR>(0x00B3)})
		{
			FString Device = TEXT("WoT.app/Contents/com");
			Device.AppendChar(Superscript);
			RefusedMac(*Device, TEXT("a superscript Windows COM device name"));
		}

		// A second top level would write somewhere the release does not describe.
		{
			TArray<FWoTArchiveEntry> TwoRoots = WoTTransferSpecMacBundle();
			TwoRoots.Add(WoTTransferSpecFile(TEXT("Other.app")));
			TestTrue(TEXT("a second top-level entry is refused"),
				ValidateArchiveIndex(TwoRoots, Limits, EWoTArchiveLayout::MacAppBundle)
					.IsError());
		}

		// Two entries differing only by case are ONE file on a case-insensitive
		// volume, so extraction order would decide the content.
		{
			TArray<FWoTArchiveEntry> CaseAlias = WoTTransferSpecMacBundle();
			CaseAlias.Add(WoTTransferSpecFile(TEXT("WoT.app/Contents/INFO.PLIST")));
			TestTrue(TEXT("a case-folded duplicate is refused"),
				ValidateArchiveIndex(CaseAlias, Limits, EWoTArchiveLayout::MacAppBundle)
					.IsError());

			TArray<FWoTArchiveEntry> Exact = WoTTransferSpecMacBundle();
			Exact.Add(WoTTransferSpecFile(TEXT("WoT.app/Contents/Info.plist")));
			TestTrue(TEXT("an exact duplicate is refused"),
				ValidateArchiveIndex(Exact, Limits, EWoTArchiveLayout::MacAppBundle)
					.IsError());
		}

		TestTrue(TEXT("an empty index is refused"),
			ValidateArchiveIndex(TArray<FWoTArchiveEntry>(), Limits,
				EWoTArchiveLayout::MacAppBundle).IsError());
	}

	// Bounds: a zip bomb is refused by arithmetic, not discovered by filling a disk.
	{
		FWoTArchiveLimits Tight;
		Tight.MaxEntries = 4;
		TestTrue(TEXT("too many entries is refused"),
			ValidateArchiveIndex(WoTTransferSpecMacBundle(), Tight,
				EWoTArchiveLayout::MacAppBundle).IsError());

		FWoTArchiveLimits SmallEntry;
		SmallEntry.MaxEntryBytes = 1024;
		TestTrue(TEXT("an oversized entry is refused"),
			ValidateArchiveIndex(WoTTransferSpecMacBundle(), SmallEntry,
				EWoTArchiveLayout::MacAppBundle).IsError());

		FWoTArchiveLimits SmallTotal;
		SmallTotal.MaxTotalBytes = 2000;
		TestTrue(TEXT("an oversized total is refused"),
			ValidateArchiveIndex(WoTTransferSpecMacBundle(), SmallTotal,
				EWoTArchiveLayout::MacAppBundle).IsError());

		TArray<FWoTArchiveEntry> SingleOversized = {
			WoTTransferSpecDirectory(TEXT("WoT.app")),
			WoTTransferSpecFile(TEXT("WoT.app/only.bin"), 2001),
		};
		FWoTArchiveLimits SingleTotal;
		SingleTotal.MaxEntryBytes = 4000;
		SingleTotal.MaxTotalBytes = 2000;
		TestTrue(TEXT("one entry larger than the total limit is refused"),
			ValidateArchiveIndex(SingleOversized, SingleTotal,
				EWoTArchiveLayout::MacAppBundle).IsError());

		FWoTArchiveLimits Shallow;
		Shallow.MaxDepth = 2;
		TestTrue(TEXT("an over-deep entry is refused"),
			ValidateArchiveIndex(WoTTransferSpecMacBundle(), Shallow,
				EWoTArchiveLayout::MacAppBundle).IsError());

		// The running total is checked BEFORE adding, so two enormous entries
		// cannot wrap around into looking acceptable.
		TArray<FWoTArchiveEntry> Overflow = {
			WoTTransferSpecDirectory(TEXT("WoT.app")),
			WoTTransferSpecFile(TEXT("WoT.app/a"), TNumericLimits<uint64>::Max() / 2 + 1),
			WoTTransferSpecFile(TEXT("WoT.app/b"), TNumericLimits<uint64>::Max() / 2 + 1),
		};
		FWoTArchiveLimits Huge;
		Huge.MaxEntryBytes = TNumericLimits<uint64>::Max();
		Huge.MaxTotalBytes = TNumericLimits<uint64>::Max();
		TestTrue(TEXT("a size total that would overflow is refused"),
			ValidateArchiveIndex(Overflow, Huge, EWoTArchiveLayout::MacAppBundle)
				.IsError());
	}

	// A non-link entry naming a link target is a confused index, not a hint.
	{
		TArray<FWoTArchiveEntry> Confused = WoTTransferSpecMacBundle();
		Confused[3].LinkTarget = TEXT("elsewhere");
		TestTrue(TEXT("a file naming a link target is refused"),
			ValidateArchiveIndex(Confused, Limits, EWoTArchiveLayout::MacAppBundle)
				.IsError());
	}

	return !HasAnyErrors();
}

#endif  // WITH_AUTOMATION_TESTS
