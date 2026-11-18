// Specs for the path-component classifier.
//
// The property under test: every reparse point is a link, except that under Wine
// the drive and mount mappings Wine itself presents are the directories they
// stand for. Nothing outside that narrow allowance may change classification.
#include "Misc/AutomationTest.h"
#include "WoTBootstrapCore/ReparseClassification.h"

#if WITH_AUTOMATION_TESTS

namespace
{
constexpr EAutomationTestFlags WoTReparseClassificationSpecFlags =
	EAutomationTestFlags::ProgramContext | EAutomationTestFlags::EditorContext
	| EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTReparseClassificationSpec,
	"WoT.BootstrapCore.Identity.ReparseClassification", WoTReparseClassificationSpecFlags)

bool FWoTReparseClassificationSpec::RunTest(const FString&)
{
	auto Classify = [](bool bReparse, uint32 Tag, bool bDirectory, bool bDriveRoot,
		bool bVolumeBoundary, bool bWine)
	{
		FWoTReparseComponentFacts Facts;
		Facts.bReparse = bReparse;
		Facts.Tag = Tag;
		Facts.bDirectory = bDirectory;
		Facts.bDriveRoot = bDriveRoot;
		Facts.bVolumeBoundary = bVolumeBoundary;
		Facts.bWine = bWine;
		return WoTClassifyPathComponent(Facts);
	};
	constexpr uint32 SymlinkTag = 0xA000000Cu;   // IO_REPARSE_TAG_SYMLINK
	constexpr uint32 JunctionTag = 0xA0000003u;  // IO_REPARSE_TAG_MOUNT_POINT

	TestEqual(TEXT("plain directory"), Classify(false, 0, true, false, false, false),
		EWoTTreeEntryType::Directory);
	TestEqual(TEXT("plain file"), Classify(false, 0, false, false, false, true),
		EWoTTreeEntryType::File);
	for (const uint32 Tag : {SymlinkTag, JunctionTag})
	{
		TestEqual(TEXT("native reparse dir at a drive root stays a link"),
			Classify(true, Tag, true, true, true, false), EWoTTreeEntryType::SymbolicLink);
		TestEqual(TEXT("native junction on a boundary stays a link"),
			Classify(true, Tag, true, false, true, false), EWoTTreeEntryType::SymbolicLink);
	}
	for (const uint32 Tag : WoTWineMappingReparseTags())
	{
		TestEqual(TEXT("Wine drive root is a directory"),
			Classify(true, Tag, true, true, false, true), EWoTTreeEntryType::Directory);
		TestEqual(TEXT("Wine mount boundary is a directory"),
			Classify(true, Tag, true, false, true, true), EWoTTreeEntryType::Directory);
		TestEqual(TEXT("Wine mapping tag inside a volume stays a link"),
			Classify(true, Tag, true, false, false, true), EWoTTreeEntryType::SymbolicLink);
		TestEqual(TEXT("Wine mapping tag on a file stays a link"),
			Classify(true, Tag, false, true, true, true), EWoTTreeEntryType::SymbolicLink);
	}
	TestEqual(TEXT("Wine unmeasured tag at a root stays a link"),
		Classify(true, SymlinkTag, true, true, true, true), EWoTTreeEntryType::SymbolicLink);
	TestTrue(TEXT("the measured Wine tag set is not empty"),
		WoTWineMappingReparseTags().Num() > 0);
	return !HasAnyErrors();
}

#endif  // WITH_AUTOMATION_TESTS
