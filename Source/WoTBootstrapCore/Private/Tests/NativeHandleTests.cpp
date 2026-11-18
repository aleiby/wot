// Specs for the link-refusing native handles.
//
// The property under test: a PATH IS NOT AN OBJECT. Every case below is a way a
// name can stop referring to the thing it referred to a moment ago — a symlink in
// the way, a second hard link, a rename that replaces the name, an unlink that
// removes it entirely. In each case the production entry points must either refuse
// to follow, or keep answering for the object actually held.
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "NativeHandleTestFactory.h"
#include "WoTBootstrapCore/NativeHandles.h"
#include "WoTBootstrapCore/Hash.h"
#include "HAL/PlatformFileManager.h"

#if WITH_AUTOMATION_TESTS

namespace
{
constexpr EAutomationTestFlags WoTNativeHandleSpecFlags =
	EAutomationTestFlags::ProgramContext | EAutomationTestFlags::EditorContext
	| EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter;
#if PLATFORM_MAC
constexpr TCHAR WoTNativeExchangeSpecName[] =
	TEXT("WoT.BootstrapCore.Identity.NativeExchangeSwap");
#else
constexpr TCHAR WoTNativeExchangeSpecName[] =
	TEXT("WoT.BootstrapCore.Identity.NativeExchangeUnsupported");
#endif

TArray<uint8> WoTNativeHandleSpecBytes(const ANSICHAR* Text)
{
	TArray<uint8> Bytes;
	for (const ANSICHAR* Cursor = Text; *Cursor != '\0'; ++Cursor)
	{
		Bytes.Add(static_cast<uint8>(*Cursor));
	}
	return Bytes;
}

/** Removes its root on destruction so a failing spec cannot leak a temp tree. */
struct FWoTNativeHandleSpecScope
{
	FString Root;

	FWoTNativeHandleSpecScope()
		: Root(WoTNativeHandleTestFactory::CreateTemporaryRoot())
	{
	}

	~FWoTNativeHandleSpecScope()
	{
		WoTNativeHandleTestFactory::RemoveTree(Root);
	}

	FString operator/(const TCHAR* Leaf) const { return Root + TEXT("/") + Leaf; }
};
}  // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNativeHandlePathRulesSpec,
	"WoT.BootstrapCore.Identity.RelativePathRules", WoTNativeHandleSpecFlags)

bool FWoTNativeHandlePathRulesSpec::RunTest(const FString&)
{
	// One shared definition of "inside the tree", so the inventory reader and every
	// native walk can never disagree about it.
	const TCHAR* const Accepted[] = {
		TEXT("file.txt"), TEXT("dir/file.txt"), TEXT("a/b/c/d.bin"),
		TEXT("Content/Paks/WoT.pak"), TEXT("name with spaces.txt"),
	};
	for (const TCHAR* Path : Accepted)
	{
		TestFalse(FString::Printf(TEXT("'%s' is inside the tree"), Path),
			RequireSafeRelativePath(Path).IsError());
	}

	const TCHAR* const Refused[] = {
		TEXT(""), TEXT("/absolute"), TEXT("../escape"), TEXT("a/../b"),
		TEXT("a/./b"), TEXT("a//b"), TEXT("a\\b"), TEXT("C:/x"),
		TEXT("stream:name"), TEXT("trailing."), TEXT("trailing "),
	};
	for (const TCHAR* Path : Refused)
	{
		TestTrue(FString::Printf(TEXT("'%s' is refused"), Path),
			RequireSafeRelativePath(Path).IsError());
	}

	// A control character would let one logical name have two spellings.
	{
		FString Sneaky = TEXT("a");
		Sneaky.AppendChar(static_cast<TCHAR>(0x01));
		TestTrue(TEXT("a control character is refused"),
			RequireSafeRelativePath(Sneaky).IsError());
	}

	return !HasAnyErrors();
}

#if PLATFORM_WINDOWS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNativeDeepMutationAccessWalkSpec,
	"WoT.BootstrapCore.Identity.DeepMutationAccessWalk",
	WoTNativeHandleSpecFlags)

bool FWoTNativeDeepMutationAccessWalkSpec::RunTest(const FString&)
{
	constexpr int32 NestedDepth = 1500;
	FWoTNativeHandleSpecScope Scope;
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> Root =
		OpenNativeDirectoryNoFollow(Scope.Root);
	if (!TestFalse(TEXT("the deep-walk temporary root opens"), Root.IsError()))
	{
		if (Root.IsError()) AddError(Root.GetError().ToString());
		return false;
	}

	TArray<FWoTNativeDirectoryHandle> Levels;
	ON_SCOPE_EXIT
	{
		for (int32 Index = Levels.Num() - 1; Index >= 0; --Index)
		{
			Levels[Index] = FWoTNativeDirectoryHandle();
			const FWoTNativeDirectoryHandle& Parent = Index == 0
				? Root.GetValue() : Levels[Index - 1];
			FWoTBootstrapStatus Removed = RemoveNativeEntryAtNoFollow(
				Parent, Index == 0 ? TEXTVIEW("Game") : TEXTVIEW("d"),
				EWoTTreeEntryType::Directory);
			if (Removed.IsError())
			{
				AddError(TEXT("deep-walk handle-relative cleanup failed: ")
					+ Removed.GetError().ToString());
			}
		}
	};

	TWoTBootstrapResult<FWoTNativeDirectoryHandle> Game =
		CreateNativeDirectoryAtNoFollow(Root.GetValue(), TEXT("Game"));
	if (!TestFalse(TEXT("the deep-walk live root is created handle-relatively"),
		Game.IsError()))
	{
		if (Game.IsError()) AddError(Game.GetError().ToString());
		return false;
	}
	Levels.Add(MoveTemp(Game).GetValue());
	for (int32 Depth = 0; Depth < NestedDepth; ++Depth)
	{
		TWoTBootstrapResult<FWoTNativeDirectoryHandle> Child =
			CreateNativeDirectoryAtNoFollow(Levels.Last(), TEXT("d"));
		if (Child.IsError())
		{
			AddError(FString::Printf(
				TEXT("handle-relative fixture creation failed at depth %d: %s"),
				Depth + 1, *Child.GetError().ToString()));
			return false;
		}
		Levels.Add(MoveTemp(Child).GetValue());
	}

	int32 CancellationPolls = 0;
	// The poll runs inside the walk. A recursive walker calls it from a frame
	// that deepens with every level (1,500 levels x its frame size); the
	// explicit-stack walker calls it from one loop frame. Record the span of a
	// stack-local address across every poll to tell the two apart.
	UPTRINT LowestPollStack = MAX_uint64;
	UPTRINT HighestPollStack = 0;
	TWoTBootstrapResult<bool> Checked =
		CheckNativeDirectoryMutationAccessAtNoFollow(
			Root.GetValue(), TEXT("Game"),
			[&CancellationPolls, &LowestPollStack, &HighestPollStack]()
			{
				volatile int32 StackMarker = CancellationPolls;
				const UPTRINT Address = reinterpret_cast<UPTRINT>(&StackMarker);
				LowestPollStack = FMath::Min(LowestPollStack, Address);
				HighestPollStack = FMath::Max(HighestPollStack, Address);
				++CancellationPolls;
				return FWoTBootstrapStatus::Success();
			});
	if (!TestFalse(TEXT("the 1,500-level explicit-stack walk completes"),
		Checked.IsError()))
	{
		if (Checked.IsError()) AddError(Checked.GetError().ToString());
		return false;
	}
	TestTrue(TEXT("the deeply nested tree has the correct access verdict"),
		Checked.GetValue());
	TestTrue(TEXT("cancellation is polled between entries across deep levels"),
		CancellationPolls >= NestedDepth - 1);
	// 16 KiB is far below what 1,500 recursive frames need, and far above the
	// jitter of a poll made from one loop frame.
	constexpr UPTRINT MaxPollStackSpan = 16 * 1024;
	TestTrue(FString::Printf(
			TEXT("the walk does not grow the stack with depth (span %llu bytes)"),
			static_cast<uint64>(HighestPollStack - LowestPollStack)),
		CancellationPolls > 0
			&& HighestPollStack - LowestPollStack < MaxPollStackSpan);
	return !HasAnyErrors();
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNativeHandleOpenSpec,
	"WoT.BootstrapCore.Identity.NativeHandles", WoTNativeHandleSpecFlags)

bool FWoTNativeHandleOpenSpec::RunTest(const FString&)
{
	FWoTNativeHandleSpecScope Scope;
	if (!TestFalse(TEXT("a temporary root was created"), Scope.Root.IsEmpty()))
	{
		return false;
	}

	const TArray<uint8> Payload = WoTNativeHandleSpecBytes("the wheel weaves");
	const FString FilePath = Scope / TEXT("plain.bin");
	const FString DirPath = Scope / TEXT("sub");
	if (!TestTrue(TEXT("fixtures were created"),
			WoTNativeHandleTestFactory::WriteFile(FilePath, Payload)
			&& WoTNativeHandleTestFactory::CreateDirectory(DirPath)
			&& WoTNativeHandleTestFactory::WriteFile(
				DirPath + TEXT("/nested.bin"), Payload)))
	{
		return false;
	}

	// An ordinary file opens, and its bytes come from the handle.
	{
		TWoTBootstrapResult<FWoTNativeFileHandle> Handle =
			OpenNativeFileReadNoFollow(FilePath);
		if (!TestFalse(TEXT("an ordinary file opens"), Handle.IsError()))
		{
			AddError(Handle.GetError().ToString());
			return false;
		}
		const FWoTNativeFileHandle& File = Handle.GetValue();
		TestTrue(TEXT("the handle is valid"), File.IsValid());
		TestEqual(TEXT("the identity reports a file"), File.GetIdentity().Type,
			EWoTTreeEntryType::File);
		TestEqual(TEXT("the identity reports the size"),
			static_cast<int64>(File.GetIdentity().Size),
			static_cast<int64>(Payload.Num()));
		TestEqual(TEXT("an ordinary file has one name"),
			static_cast<int32>(File.GetIdentity().LinkCount), 1);

		TWoTBootstrapResult<TArray<uint8>> Read = File.ReadAll();
		if (TestFalse(TEXT("the held bytes are readable"), Read.IsError()))
		{
			TestEqual(TEXT("the bytes match"), Read.GetValue(), Payload);
		}

		// A duplicate refers to the same object, not to the same name.
		TWoTBootstrapResult<FWoTNativeFileHandle> Duplicate = File.DuplicateForRead();
		if (TestFalse(TEXT("the handle duplicates"), Duplicate.IsError()))
		{
			TestTrue(TEXT("the duplicate has identical identity"),
				Duplicate.GetValue().GetIdentity() == File.GetIdentity());
		}
	}

	// A directory opens as a directory, and a file does not.
	{
		TWoTBootstrapResult<FWoTNativeDirectoryHandle> Directory =
			OpenNativeDirectoryNoFollow(DirPath);
		if (TestFalse(TEXT("a directory opens"), Directory.IsError()))
		{
			TestEqual(TEXT("the identity reports a directory"),
				Directory.GetValue().GetIdentity().Type, EWoTTreeEntryType::Directory);
			TWoTBootstrapResult<FString> HeldPath =
				QueryNativeDirectoryPath(Directory.GetValue());
			if (TestFalse(TEXT("the held directory reports its native path"),
					HeldPath.IsError()))
			{
				FString Normalized = HeldPath.GetValue();
				FPaths::NormalizeDirectoryName(Normalized);
				TestEqual(TEXT("the held directory path preserves its live leaf"),
					FPaths::GetCleanFilename(Normalized), FString(TEXT("sub")));
			}

			TWoTBootstrapResult<FWoTNativeFileHandle> Nested =
				OpenNativeFileAtReadNoFollow(Directory.GetValue(), TEXT("nested.bin"));
			TestFalse(TEXT("a relative open beneath it succeeds"), Nested.IsError());

			// Escaping is refused by the shared path rule, before any syscall.
			TestTrue(TEXT("a relative open cannot escape upward"),
				OpenNativeFileAtReadNoFollow(Directory.GetValue(),
					TEXT("../plain.bin")).IsError());
			TestTrue(TEXT("a relative open cannot be absolute"),
				OpenNativeFileAtReadNoFollow(Directory.GetValue(),
					TEXT("/etc/hosts")).IsError());
		}
		TestTrue(TEXT("a file does not open as a directory"),
			OpenNativeDirectoryNoFollow(FilePath).IsError());
	}

	// Symlinks are refused rather than followed. This is the whole point.
	{
		const FString LinkPath = Scope / TEXT("link.bin");
		if (WoTNativeHandleTestFactory::CreateSymbolicLink(LinkPath, FilePath))
		{
			TestTrue(TEXT("a symlink is not followed to its target"),
				OpenNativeFileReadNoFollow(LinkPath).IsError());

			const FString DirLinkPath = Scope / TEXT("dirlink");
			if (WoTNativeHandleTestFactory::CreateSymbolicLink(DirLinkPath, DirPath))
			{
				TestTrue(TEXT("a symlinked directory is not followed"),
					OpenNativeDirectoryNoFollow(DirLinkPath).IsError());
			}

			// And a link INSIDE a walk is refused mid-path, not at the end.
			TWoTBootstrapResult<FWoTNativeDirectoryHandle> Root =
				OpenNativeDirectoryNoFollow(Scope.Root);
			if (TestFalse(TEXT("the root opens"), Root.IsError()))
			{
				TestTrue(TEXT("a relative walk refuses a symlink component"),
					OpenNativeFileAtReadNoFollow(Root.GetValue(),
						TEXT("link.bin")).IsError());
			}
		}
		else
		{
			AddInfo(TEXT("symlink creation unavailable; link refusal not exercised"));
		}
	}

	// Relative opens are rooted in the HELD directory object, not in the path that
	// happened to name it when it was opened. This specifically exercises the
	// NtCreateFile RootDirectory guarantee on Windows and openat on macOS.
	{
		const FString HeldPath = Scope / TEXT("held-parent");
		const FString ReplacementPath = Scope / TEXT("replacement-parent");
		const TArray<uint8> HeldBytes = WoTNativeHandleSpecBytes("held child");
		const TArray<uint8> ReplacementBytes = WoTNativeHandleSpecBytes("path child");
		if (TestTrue(TEXT("relative-root fixtures were created"),
				WoTNativeHandleTestFactory::CreateDirectory(HeldPath)
				&& WoTNativeHandleTestFactory::CreateDirectory(ReplacementPath)
				&& WoTNativeHandleTestFactory::WriteFile(
					HeldPath + TEXT("/child.bin"), HeldBytes)
				&& WoTNativeHandleTestFactory::WriteFile(
					ReplacementPath + TEXT("/child.bin"), ReplacementBytes)))
		{
			TWoTBootstrapResult<FWoTNativeDirectoryHandle> Held =
				OpenNativeDirectoryNoFollow(HeldPath);
			if (TestFalse(TEXT("the parent directory is held"), Held.IsError())
				&& TestTrue(TEXT("the held parent's name was replaced"),
					WoTNativeHandleTestFactory::ReplaceDirectoryByRename(
						HeldPath, ReplacementPath)))
			{
				TWoTBootstrapResult<FWoTNativeFileHandle> Child =
					OpenNativeFileAtReadNoFollow(Held.GetValue(), TEXT("child.bin"));
				if (TestFalse(TEXT("relative open still uses the held parent"), Child.IsError()))
				{
					TWoTBootstrapResult<TArray<uint8>> Read = Child.GetValue().ReadAll();
					if (TestFalse(TEXT("the held parent's child is readable"), Read.IsError()))
					{
						TestEqual(TEXT("the child came from the held parent object"),
							Read.GetValue(), HeldBytes);
					}
				}
			}
		}
	}

	// ReadAll returns a TArray, whose element count is int32. A sparse file just
	// beyond that bound must fail before allocation or narrowing.
	{
		const FString HugePath = Scope / TEXT("too-large.bin");
		const uint64 HugeSize = static_cast<uint64>(TNumericLimits<int32>::Max()) + 1;
		if (TestTrue(TEXT("an oversized sparse fixture was created"),
				WoTNativeHandleTestFactory::CreateSparseFile(HugePath, HugeSize)))
		{
			TWoTBootstrapResult<FWoTNativeFileHandle> Huge =
				OpenNativeFileReadNoFollow(HugePath);
			if (TestFalse(TEXT("the oversized regular file opens"), Huge.IsError()))
			{
				TWoTBootstrapResult<TArray<uint8>> Read = Huge.GetValue().ReadAll();
				if (TestTrue(TEXT("ReadAll refuses a size the array cannot represent"),
						Read.IsError()))
				{
					TestEqual(TEXT("the refusal is a size limit"), Read.GetError().Code,
						EWoTBootstrapError::SizeLimit);
				}
			}
		}
	}

	// A hard link is a second NAME for one object, and the count says so.
	{
		const FString HardPath = Scope / TEXT("hard.bin");
		if (WoTNativeHandleTestFactory::CreateHardLink(HardPath, FilePath))
		{
			TWoTBootstrapResult<FWoTNativeFileHandle> Handle =
				OpenNativeFileReadNoFollow(FilePath);
			if (TestFalse(TEXT("the file still opens"), Handle.IsError()))
			{
				TestTrue(TEXT("a second hard link is visible in the identity"),
					Handle.GetValue().GetIdentity().LinkCount >= 2);
			}
		}
		else
		{
			AddInfo(TEXT("hard link creation unavailable; link count not exercised"));
		}
	}

	// The held object survives its name being given to something else.
	//
	// CROSS-PLATFORM:
	// do NOT weaken this on Windows. MoveFileExW+MOVEFILE_REPLACE_EXISTING cannot
	// replace a file that something holds open, so the Windows factory stages the
	// race with FileRenameInfoEx + POSIX_SEMANTICS instead. That is correct and
	// intended, for two reasons. First, if the factory cannot stage the
	// replacement then this spec passes VACUOUSLY on Windows — it would assert
	// nothing about the property it exists to defend. Second, a real attacker on
	// Windows can do exactly this: POSIX-semantics rename is available to any
	// process with delete access, so recording "Windows cannot stage this" would
	// bake a false assumption into the threat model. Production code is untouched
	// on both platforms; only the attacker simulation differs, which is the right
	// boundary — the factory plays the attacker, production does the defending.
	{
		const FString VictimPath = Scope / TEXT("victim.bin");
		const FString ImposterPath = Scope / TEXT("imposter.bin");
		const TArray<uint8> Original = WoTNativeHandleSpecBytes("original");
		const TArray<uint8> Replacement = WoTNativeHandleSpecBytes("replacement!");
		if (TestTrue(TEXT("race fixtures were created"),
				WoTNativeHandleTestFactory::WriteFile(VictimPath, Original)
				&& WoTNativeHandleTestFactory::WriteFile(ImposterPath, Replacement)))
		{
			TWoTBootstrapResult<FWoTNativeFileHandle> Held =
				OpenNativeFileReadNoFollow(VictimPath);
			if (TestFalse(TEXT("the victim opens"), Held.IsError()))
			{
				const FWoTNativeFileIdentity Before = Held.GetValue().GetIdentity();

				TestTrue(TEXT("the name was replaced"),
					WoTNativeHandleTestFactory::ReplaceByRename(VictimPath, ImposterPath));

				// The path now names different bytes; the HANDLE does not.
				TWoTBootstrapResult<TArray<uint8>> Read = Held.GetValue().ReadAll();
				if (TestFalse(TEXT("the held bytes are still readable"), Read.IsError()))
				{
					TestEqual(TEXT("the handle still reads the original bytes"),
						Read.GetValue(), Original);
				}
				TWoTBootstrapResult<FWoTNativeFileIdentity> Now =
					Held.GetValue().QueryCurrentIdentity();
				if (TestFalse(TEXT("the held identity is still queryable"), Now.IsError()))
				{
					// Still the same OBJECT: same volume, file id, size, and type.
					TestTrue(TEXT("the held object is the same object after the rename"),
						Now.GetValue().IsSameObject(Before));
					// But not strictly unchanged — the rename removed its last name,
					// which moves the link count and the change time. Asserting this
					// explicitly rather than ignoring it: a caller that wants "has
					// anything moved" must not be handed "is it the same object".
					TestFalse(TEXT("strict identity notices the name was removed"),
						Now.GetValue() == Before);
					TestEqual(TEXT("the replaced file has no names left"),
						static_cast<int32>(Now.GetValue().LinkCount), 0);
				}

				// Re-opening the PATH now finds a different object entirely — which
				// is exactly why verification uses handles rather than paths.
				TWoTBootstrapResult<FWoTNativeFileHandle> Reopened =
					OpenNativeFileReadNoFollow(VictimPath);
				if (TestFalse(TEXT("the replaced path opens"), Reopened.IsError()))
				{
					TestFalse(TEXT("the path now refers to a different object"),
						Reopened.GetValue().GetIdentity().IsSameObject(Before));
				}
			}
		}
	}

	// A deleted name leaves the held bytes readable.
	{
		const FString DoomedPath = Scope / TEXT("doomed.bin");
		if (TestTrue(TEXT("the doomed fixture was created"),
				WoTNativeHandleTestFactory::WriteFile(DoomedPath, Payload)))
		{
			TWoTBootstrapResult<FWoTNativeFileHandle> Held =
				OpenNativeFileReadNoFollow(DoomedPath);
			if (TestFalse(TEXT("the doomed file opens"), Held.IsError()))
			{
				TestTrue(TEXT("its name was removed"),
					WoTNativeHandleTestFactory::RemoveName(DoomedPath));
				TWoTBootstrapResult<TArray<uint8>> Read = Held.GetValue().ReadAll();
				if (TestFalse(TEXT("the unlinked bytes are still readable"),
						Read.IsError()))
				{
					TestEqual(TEXT("the unlinked bytes are unchanged"),
						Read.GetValue(), Payload);
				}
				TestTrue(TEXT("the removed path no longer opens"),
					OpenNativeFileReadNoFollow(DoomedPath).IsError());
			}
		}
	}

	// Absent and non-absolute paths.
	TestTrue(TEXT("an absent path does not open"),
		OpenNativeFileReadNoFollow(Scope / TEXT("absent.bin")).IsError());
	TestTrue(TEXT("a relative path is refused by the absolute entry point"),
		OpenNativeFileReadNoFollow(TEXT("relative.bin")).IsError());

	return !HasAnyErrors();
}

#if PLATFORM_MAC
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNativeDirectoryVanishedEntrySpec,
	"WoT.BootstrapCore.Identity.DirectoryEnumerationVanishedEntry",
	WoTNativeHandleSpecFlags)

bool FWoTNativeDirectoryVanishedEntrySpec::RunTest(const FString&)
{
	FWoTNativeHandleSpecScope Scope;
	if (!TestFalse(TEXT("a temporary root was created"), Scope.Root.IsEmpty()))
	{
		return false;
	}

	const FString StableLeaf = TEXT("enumeration-stable.bin");
	const FString VanishingLeaf = TEXT("enumeration-vanishing.tmp");
	const TArray<uint8> Payload = WoTNativeHandleSpecBytes("stable");
	if (!TestTrue(TEXT("enumeration fixtures were created"),
			WoTNativeHandleTestFactory::WriteFile(Scope / *StableLeaf, Payload)
			&& WoTNativeHandleTestFactory::WriteFile(
				Scope / *VanishingLeaf, Payload)))
	{
		return false;
	}

	TWoTBootstrapResult<FWoTNativeDirectoryHandle> Root =
		OpenNativeDirectoryNoFollow(Scope.Root);
	if (!TestFalse(TEXT("the enumeration root opens"), Root.IsError()))
	{
		return false;
	}

	WoTSetNativeDirectoryEnumerationRemovalLeafForTesting(VanishingLeaf);
	TWoTBootstrapResult<TArray<FWoTNativeDirectoryEntry>> Entries =
		EnumerateNativeDirectory(Root.GetValue());
	if (!TestFalse(TEXT("a vanished entry does not fail enumeration"),
			Entries.IsError()))
	{
		if (Entries.IsError()) AddError(Entries.GetError().ToString());
		return false;
	}

	bool bFoundStable = false;
	bool bFoundVanishing = false;
	for (const FWoTNativeDirectoryEntry& Entry : Entries.GetValue())
	{
		bFoundStable |= Entry.Name == StableLeaf;
		bFoundVanishing |= Entry.Name == VanishingLeaf;
	}
	TestTrue(TEXT("the stable entry remains visible"), bFoundStable);
	TestFalse(TEXT("the vanished entry is omitted"), bFoundVanishing);
	TestTrue(TEXT("the seam removed the entry"),
		OpenNativeFileAtReadNoFollow(Root.GetValue(), VanishingLeaf).IsError());
	return !HasAnyErrors();
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNativeSingleLeafQuerySpec,
	"WoT.BootstrapCore.Identity.NativeSingleLeafQuery",
	WoTNativeHandleSpecFlags)

bool FWoTNativeSingleLeafQuerySpec::RunTest(const FString&)
{
	FWoTNativeHandleSpecScope Scope;
	const TArray<uint8> Payload = WoTNativeHandleSpecBytes("single leaf");
	const FString FilePath = Scope / TEXT("a");
	const FString DirectoryPath = Scope / TEXT("d");
	if (!TestTrue(TEXT("single-leaf fixtures are created"),
			WoTNativeHandleTestFactory::WriteFile(FilePath, Payload)
			&& WoTNativeHandleTestFactory::CreateDirectory(DirectoryPath)))
	{
		return false;
	}
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> Root =
		OpenNativeDirectoryNoFollow(Scope.Root);
	if (!TestFalse(TEXT("single-leaf root opens"), Root.IsError()))
	{
		return false;
	}
	auto Query = [this, &Root](const TCHAR* Leaf,
		TOptional<EWoTTreeEntryType> Expected)
	{
		TWoTBootstrapResult<TOptional<EWoTTreeEntryType>> Found =
			QueryNativeEntryTypeAtNoFollow(Root.GetValue(), Leaf);
		if (!TestFalse(FString::Printf(TEXT("query '%s' succeeds"), Leaf),
				Found.IsError()))
		{
			return;
		}
		TestEqual(FString::Printf(TEXT("query '%s' presence"), Leaf),
			Found.GetValue().IsSet(), Expected.IsSet());
		if (Expected.IsSet() && Found.GetValue().IsSet())
		{
			TestEqual(FString::Printf(TEXT("query '%s' type"), Leaf),
				Found.GetValue().GetValue(), Expected.GetValue());
		}
	};
	Query(TEXT("a"), EWoTTreeEntryType::File);
	Query(TEXT("d"), EWoTTreeEntryType::Directory);
	Query(TEXT("missing"), {});
	const FString LinkPath = Scope / TEXT("l");
	if (WoTNativeHandleTestFactory::CreateSymbolicLink(LinkPath, FilePath))
	{
		Query(TEXT("l"), EWoTTreeEntryType::SymbolicLink);
	}
	else
	{
		AddInfo(TEXT("symlink creation unavailable; link classification skipped"));
	}
	TWoTBootstrapResult<TOptional<EWoTTreeEntryType>> Nested =
		QueryNativeEntryTypeAtNoFollow(Root.GetValue(), TEXT("x/y"));
	TestTrue(TEXT("a nested leaf is refused"), Nested.IsError()
		&& Nested.GetError().Code == EWoTBootstrapError::UnsafePath);
	TWoTBootstrapResult<TOptional<EWoTTreeEntryType>> Parent =
		QueryNativeEntryTypeAtNoFollow(Root.GetValue(), TEXT(".."));
	TestTrue(TEXT("the parent leaf is refused"), Parent.IsError()
		&& Parent.GetError().Code == EWoTBootstrapError::UnsafePath);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNativeDirectorySameObjectSpec,
	"WoT.BootstrapCore.Identity.DirectorySameObjectAcrossChildren",
	WoTNativeHandleSpecFlags)

bool FWoTNativeDirectorySameObjectSpec::RunTest(const FString&)
{
	// A directory root bound by identity before its children exist (the
	// user-state root before UpdaterState) must still be the same object after
	// a child is created: APFS directory st_size tracks entries, and a
	// size-sensitive comparison made every fresh-profile Mac start exit 1.
	FWoTNativeHandleSpecScope Scope;
	const FString DirectoryPath = Scope / TEXT("BoundRoot");
	bool bCreated = false;
	TWoTBootstrapResult<FWoTCanonicalNativeDirectory> Created =
		OpenOrCreateCanonicalNativeDirectoryNoFollow(DirectoryPath, bCreated);
	if (!TestFalse(TEXT("the bound root is created"), Created.IsError()))
	{
		return false;
	}
	const FWoTNativeFileIdentity Before =
		Created.GetValue().Directory.GetIdentity();
	bool bChildCreated = false;
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> Child =
		OpenOrCreateNativeDirectoryAtNoFollow(Created.GetValue().Directory,
			TEXT("UpdaterState"), bChildCreated);
	TestFalse(TEXT("a child directory is created under the bound root"),
		Child.IsError());
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> Reopened =
		OpenNativeDirectoryNoFollow(DirectoryPath);
	if (!TestFalse(TEXT("the bound root reopens"), Reopened.IsError()))
	{
		return false;
	}
	const FWoTNativeFileIdentity After = Reopened.GetValue().GetIdentity();
	TestTrue(TEXT("the root is the same object after gaining a child"),
		After.IsSameObject(Before));
#if PLATFORM_MAC
	// The metadata really did move on APFS/HFS: strict identity notices it.
	TestFalse(TEXT("strict identity notices the new child"), After == Before);
#endif

	// A different directory at the same name is a different object.
	Child = TWoTBootstrapResult<FWoTNativeDirectoryHandle>::Error(
		MakeWoTBootstrapError(EWoTBootstrapError::Io, TEXT("released")));
	Reopened = TWoTBootstrapResult<FWoTNativeDirectoryHandle>::Error(
		MakeWoTBootstrapError(EWoTBootstrapError::Io, TEXT("released")));
	Created = TWoTBootstrapResult<FWoTCanonicalNativeDirectory>::Error(
		MakeWoTBootstrapError(EWoTBootstrapError::Io, TEXT("released")));
	WoTNativeHandleTestFactory::RemoveTree(DirectoryPath);
	bool bRecreated = false;
	TWoTBootstrapResult<FWoTCanonicalNativeDirectory> Replacement =
		OpenOrCreateCanonicalNativeDirectoryNoFollow(DirectoryPath, bRecreated);
	if (TestFalse(TEXT("a replacement directory is created"), Replacement.IsError()))
	{
		TestFalse(TEXT("a replacement directory is a different object"),
			Replacement.GetValue().Directory.GetIdentity().IsSameObject(Before));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNativeExecutableIdentitySpec,
	"WoT.BootstrapCore.Identity.ExecutableIdentity", WoTNativeHandleSpecFlags)

bool FWoTNativeExecutableIdentitySpec::RunTest(const FString&)
{
	TWoTBootstrapResult<FWoTNativeExecutableIdentity> Image =
		OpenCurrentExecutableIdentityNoFollow();
	if (!TestFalse(TEXT("the running image opens"), Image.IsError()))
	{
		AddError(Image.GetError().ToString());
		return false;
	}

	const FWoTNativeExecutableIdentity& Executable = Image.GetValue();
	TestTrue(TEXT("the image handle is valid"), Executable.IsValid());
	TestEqual(TEXT("the image is a regular file"), Executable.GetIdentity().Type,
		EWoTTreeEntryType::File);
	TestTrue(TEXT("the image is not empty"), Executable.GetIdentity().Size > 0);
	TestFalse(TEXT("the reported path is not empty"),
		Executable.GetReportedPath().IsEmpty());
#if PLATFORM_WINDOWS
	TestFalse(TEXT("the reported image path has no extended-length prefix"),
		Executable.GetReportedPath().StartsWith(
			TEXT("//?/"), ESearchCase::CaseSensitive));
	TestFalse(TEXT("the reported image path uses normalized separators"),
		Executable.GetReportedPath().Contains(TEXT("\\")));
#endif

	// Held, so it keeps answering for the code that is actually running.
	TWoTBootstrapResult<FWoTNativeFileIdentity> Now = Executable.QueryCurrentIdentity();
	if (TestFalse(TEXT("the image identity is queryable"), Now.IsError()))
	{
		TestTrue(TEXT("the running image has not changed under us"),
			Now.GetValue() == Executable.GetIdentity());
	}

	// Moving transfers the handle and empties the source.
	{
		FWoTNativeExecutableIdentity Moved = MoveTemp(Image.GetValue());
		TestTrue(TEXT("the moved-to image is valid"), Moved.IsValid());
		TestFalse(TEXT("the moved-from image is empty"), Image.GetValue().IsValid());
	}

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCanonicalNativeDirectorySpec,
	"WoT.BootstrapCore.Identity.CanonicalAncestorAndIdentity",
	WoTNativeHandleSpecFlags)

bool FWoTCanonicalNativeDirectorySpec::RunTest(const FString&)
{
	{
		FString Parent;
		FString Leaf;
		TestFalse(TEXT("a Win64 drive-root child splits safely"),
			SplitNativePathParentAndLeaf(
				TEXT("D:/WoTState"), Parent, Leaf).IsError());
		TestEqual(TEXT("a drive root keeps its slash"), Parent,
			FString(TEXT("D:/")));
		TestEqual(TEXT("the drive-root child retains its leaf"), Leaf,
			FString(TEXT("WoTState")));
		TestFalse(TEXT("a UNC share-root child splits safely"),
			SplitNativePathParentAndLeaf(
				TEXT("//server/share/WoTState"), Parent, Leaf).IsError());
		TestEqual(TEXT("the UNC share root remains intact"), Parent,
			FString(TEXT("//server/share")));
		TestEqual(TEXT("the UNC child retains its leaf"), Leaf,
			FString(TEXT("WoTState")));
		TestFalse(TEXT("a POSIX root child splits safely"),
			SplitNativePathParentAndLeaf(
				TEXT("/WoTState"), Parent, Leaf).IsError());
		TestEqual(TEXT("the POSIX parent remains root"), Parent,
			FString(TEXT("/")));
		TestTrue(TEXT("a filesystem root has no owned leaf"),
			SplitNativePathParentAndLeaf(TEXT("/"), Parent, Leaf).IsError());
	}

	FWoTNativeHandleSpecScope Scope;
	const FString RealParent = Scope / TEXT("real-parent");
	const FString OwnedRoot = RealParent + TEXT("/owned-root");
	const FString AncestorAlias = Scope / TEXT("ancestor-alias");
	if (!TestTrue(TEXT("canonical-root fixture directories are created"),
			WoTNativeHandleTestFactory::CreateDirectory(RealParent)
			&& WoTNativeHandleTestFactory::CreateDirectory(OwnedRoot)))
	{
		return false;
	}
	if (TestTrue(TEXT("symlink/junction ancestor is created"),
		WoTNativeHandleTestFactory::CreateDirectoryAncestorAlias(
			AncestorAlias, RealParent)))
	{
		TWoTBootstrapResult<FWoTCanonicalNativeDirectory> ResolvedAncestor =
			OpenCanonicalNativeDirectoryFollowing(AncestorAlias);
		if (TestFalse(
			TEXT("an immediate parent alias canonicalizes for a fresh owned root"),
			ResolvedAncestor.IsError()))
		{
			TWoTBootstrapResult<FWoTCanonicalNativeDirectory> ResolvedTarget =
				OpenCanonicalNativeDirectoryNoFollow(RealParent);
			TestTrue(TEXT("the immediate alias resolves to its target identity"),
				!ResolvedTarget.IsError()
				&& ResolvedAncestor.GetValue().Directory.GetIdentity().IsSameObject(
					ResolvedTarget.GetValue().Directory.GetIdentity()));
		}
		const FString LaunchRoot = AncestorAlias + TEXT("/owned-root");
		TWoTBootstrapResult<FWoTCanonicalNativeDirectory> Resolved =
			OpenCanonicalNativeDirectoryNoFollow(LaunchRoot);
		if (!TestFalse(TEXT("an alias above the owned root is accepted"),
			Resolved.IsError()))
		{
			if (Resolved.IsError()) AddError(Resolved.GetError().ToString());
			return false;
		}
		TestTrue(TEXT("the path comes from the opened target, not the alias"),
			!Resolved.GetValue().CanonicalPath.Contains(
				TEXT("ancestor-alias"), ESearchCase::CaseSensitive));
		TestEqual(TEXT("the canonical path retains the owned leaf"),
			FPaths::GetCleanFilename(Resolved.GetValue().CanonicalPath),
			FString(TEXT("owned-root")));
	}

	const FString VirginRoot = RealParent + TEXT("/virgin-root");
	bool bCreated = false;
	TWoTBootstrapResult<FWoTCanonicalNativeDirectory> Created =
		OpenOrCreateCanonicalNativeDirectoryNoFollow(VirginRoot, bCreated);
	if (TestFalse(TEXT("a virgin owned root is created without reopening its absolute name"),
			Created.IsError()))
	{
		TestTrue(TEXT("the virgin root reports creation"), bCreated);
		TestEqual(TEXT("the created root has the requested leaf"),
			FPaths::GetCleanFilename(Created.GetValue().CanonicalPath),
			FString(TEXT("virgin-root")));
		TWoTBootstrapResult<uint64> Available =
			QueryNativeDirectoryAvailableBytes(Created.GetValue().Directory);
		TestTrue(TEXT("free space is queried from the held root"),
			!Available.IsError() && Available.GetValue() > 0);
		const FString Redirect = RealParent + TEXT("/redirect-root");
		if (TestTrue(TEXT("a replacement root fixture is created"),
				WoTNativeHandleTestFactory::CreateDirectory(Redirect))
			&& TestTrue(TEXT("the owned root name is replaced"),
				WoTNativeHandleTestFactory::ReplaceDirectoryByRename(
					VirginRoot, Redirect)))
		{
			TWoTBootstrapResult<FWoTNativeDirectoryHandle> HeldChild =
				CreateNativeDirectoryAtNoFollow(
					Created.GetValue().Directory, TEXT("held-child"));
			TestFalse(TEXT("mutation remains rooted in the held owned root"),
				HeldChild.IsError());
			TestTrue(TEXT("the replacement name receives no child"),
				OpenNativeDirectoryNoFollow(
					VirginRoot + TEXT("/held-child")).IsError());
		}
	}

	const FString Victim = Scope / TEXT("identity-victim");
	const FString Replacement = Scope / TEXT("identity-replacement");
	if (!TestTrue(TEXT("identity-race fixture directories are created"),
			WoTNativeHandleTestFactory::CreateDirectory(Victim)
			&& WoTNativeHandleTestFactory::CreateDirectory(Replacement)))
	{
		return false;
	}
	bool bReplaced = false;
	TWoTBootstrapResult<FWoTCanonicalNativeDirectory> Mismatched =
		OpenCanonicalNativeDirectoryNoFollowForTesting(Victim,
			[&Victim, &Replacement, &bReplaced]()
			{
				bReplaced = WoTNativeHandleTestFactory::ReplaceDirectoryByRename(
					Victim, Replacement);
			});
	TestTrue(TEXT("the name is replaced between the two opens"), bReplaced);
	if (TestTrue(TEXT("a canonical re-open identity mismatch is refused"),
		Mismatched.IsError()))
	{
		TestEqual(TEXT("identity mismatch is a corrupt-installation result"),
			Mismatched.GetError().Code,
			EWoTBootstrapError::CorruptInstallation);
		TestTrue(TEXT("identity mismatch has a precise diagnostic"),
			Mismatched.GetError().Message.Contains(
				TEXT("identity changed"), ESearchCase::CaseSensitive));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNativeExchangeSpec,
	WoTNativeExchangeSpecName,
	WoTNativeHandleSpecFlags)

bool FWoTNativeExchangeSpec::RunTest(const FString&)
{
	FWoTNativeHandleSpecScope Scope;
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> Root =
		OpenNativeDirectoryNoFollow(Scope.Root);
	if (!TestFalse(TEXT("the exchange fixture root opens"), Root.IsError()))
	{
		return false;
	}
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> Left =
		CreateNativeDirectoryAtNoFollow(Root.GetValue(), TEXT("left"));
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> Right =
		CreateNativeDirectoryAtNoFollow(Root.GetValue(), TEXT("right"));
	if (!TestFalse(TEXT("the exchange fixture entries are created"),
		Left.IsError() || Right.IsError()))
	{
		return false;
	}
	const FWoTNativeFileIdentity LeftIdentity = Left.GetValue().GetIdentity();
	const FWoTNativeFileIdentity RightIdentity = Right.GetValue().GetIdentity();
	Left = TWoTBootstrapResult<FWoTNativeDirectoryHandle>::Success(
		FWoTNativeDirectoryHandle());
	Right = TWoTBootstrapResult<FWoTNativeDirectoryHandle>::Success(
		FWoTNativeDirectoryHandle());
	TWoTBootstrapResult<bool> Exchanged = ExchangeNativeEntriesAtNoFollow(
		Root.GetValue(), TEXT("left"), Root.GetValue(), TEXT("right"));
#if PLATFORM_MAC
	if (!TestFalse(TEXT("the native Mac exchange succeeds"), Exchanged.IsError()))
	{
		if (Exchanged.IsError()) AddError(Exchanged.GetError().ToString());
		return false;
	}
	TestTrue(TEXT("the native Mac exchange reports support"), Exchanged.GetValue());
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> LeftAfter =
		OpenNativeDirectoryAtNoFollow(Root.GetValue(), TEXT("left"));
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> RightAfter =
		OpenNativeDirectoryAtNoFollow(Root.GetValue(), TEXT("right"));
	if (!TestFalse(TEXT("both exchanged names remain present"),
		LeftAfter.IsError() || RightAfter.IsError()))
	{
		return false;
	}
	TestTrue(TEXT("the left name now has the right identity"),
		LeftAfter.GetValue().GetIdentity().IsSameObject(RightIdentity));
	TestTrue(TEXT("the right name now has the left identity"),
		RightAfter.GetValue().GetIdentity().IsSameObject(LeftIdentity));
#else
	if (TestTrue(TEXT("the task-3a exchange seam is unsupported"),
		Exchanged.IsError()))
	{
		TestEqual(TEXT("unsupported exchange has the protocol error code"),
			Exchanged.GetError().Code, EWoTBootstrapError::UnsupportedProtocol);
	}
#endif
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNativeMetadataChurnSpec,
	"WoT.BootstrapCore.Identity.MetadataChurnReverify", WoTNativeHandleSpecFlags)

bool FWoTNativeMetadataChurnSpec::RunTest(const FString&)
{
	// The OS writes metadata on its own schedule (macOS launch-time provenance/
	// macl xattrs, NTFS Zone.Identifier/SmartScreen streams) and only ChangeState
	// moves. A same-length rewrite looks exactly the same to the identity, so the
	// held BYTES, never the identity, decide.
	FWoTNativeHandleSpecScope Scope;
	if (!TestFalse(TEXT("a temporary root was created"), Scope.Root.IsEmpty()))
	{
		return false;
	}
	TArray<uint8> Payload;
	Payload.SetNumUninitialized(4096);
	for (int32 Index = 0; Index < Payload.Num(); ++Index)
	{
		Payload[Index] = static_cast<uint8>(Index * 7 + 3);
	}
	const FString FilePath = Scope / TEXT("held.bin");
	const FString DirPath = Scope / TEXT("dir");
	if (!TestTrue(TEXT("fixtures were created"),
			WoTNativeHandleTestFactory::WriteFile(FilePath, Payload)
			&& WoTNativeHandleTestFactory::CreateDirectory(DirPath)))
	{
		return false;
	}
	TWoTBootstrapResult<FWoTNativeFileHandle> Opened = OpenNativeFileReadNoFollow(FilePath);
	if (!TestFalse(TEXT("the file opens"), Opened.IsError()))
	{
		AddError(Opened.GetError().ToString());
		return false;
	}
	const FWoTNativeFileHandle& File = Opened.GetValue();
	const FWoTNativeFileIdentity Recorded = File.GetIdentity();
	const FWoTSha256 Hashed = Sha256(Payload);
	const uint64 Size = static_cast<uint64>(Payload.Num());
	TWoTBootstrapResult<uint32> ModeRead = QueryNativeMode(File);
	if (!TestFalse(TEXT("the file mode reads"), ModeRead.IsError()))
	{
		return false;
	}
	const uint32 VerifiedMode = ModeRead.GetValue();
	TestFalse(TEXT("an untouched file re-verifies"),
		WoTReverifyHeldFileAfterMetadataChurn(
			File, Recorded, Size, Hashed, VerifiedMode, TEXT("held.bin")).IsError());

	// Every mutation below first waits past the file system's ChangeState
	// tick: on NTFS a write in the capture's own tick leaves ChangeState equal.
	auto TickPast = [&File]()
	{
		TWoTBootstrapResult<FWoTNativeFileIdentity> Now = File.QueryCurrentIdentity();
		return !Now.IsError()
			&& WoTNativeHandleTestFactory::WaitPastChangeStateTick(Now.GetValue().ChangeState);
	};
	TestTrue(TEXT("the clock passes the recorded ChangeState"), TickPast());
	if (!TestTrue(TEXT("the file takes metadata"),
			WoTNativeHandleTestFactory::ChangeMetadataOnly(FilePath)))
	{
		return false;
	}
	TWoTBootstrapResult<FWoTNativeFileIdentity> Churned = File.QueryCurrentIdentity();
	if (TestFalse(TEXT("the churned identity reads"), Churned.IsError()))
	{
		TestTrue(TEXT("metadata-only churn breaks strict identity"),
			Churned.GetValue() != Recorded);
		TestTrue(TEXT("metadata-only churn moves ChangeState and nothing else"),
			Churned.GetValue().DiffersOnlyInChangeState(Recorded));
	}
	TWoTBootstrapResult<FWoTNativeFileIdentity> Reverified =
		WoTReverifyHeldFileAfterMetadataChurn(File, Recorded, Size, Hashed, VerifiedMode,
			TEXT("held.bin"));
	if (TestFalse(TEXT("metadata-only churn re-verifies through the held bytes"),
			Reverified.IsError()))
	{
		TWoTBootstrapResult<FWoTNativeFileIdentity> Now = File.QueryCurrentIdentity();
		TestTrue(TEXT("the re-verified identity is the new baseline"),
			!Now.IsError() && Now.GetValue() == Reverified.GetValue());
	}
	else
	{
		AddError(Reverified.GetError().ToString());
	}
	FWoTSha256 Wrong = Hashed;
	Wrong.Bytes[0] ^= 1;
	TestTrue(TEXT("re-verification is decided by the verified hash"),
		WoTReverifyHeldFileAfterMetadataChurn(File, Recorded, Size, Wrong, VerifiedMode,
			TEXT("held.bin")).IsError());

	// A mode change (chmod, or READONLY, which is the Win64 mode) is
	// ChangeState-only too, and the verified mode refuses it.
	{
		IPlatformFile& Platform = FPlatformFileManager::Get().GetPlatformFile();
		TestTrue(TEXT("the clock passes ChangeState before the mode change"), TickPast());
		TestTrue(TEXT("the file is made read-only"), Platform.SetReadOnly(*FilePath, true));
		TWoTBootstrapResult<FWoTNativeFileIdentity> Moded = File.QueryCurrentIdentity();
		TestTrue(TEXT("a mode change moves only ChangeState"),
			!Moded.IsError() && Moded.GetValue().DiffersOnlyInChangeState(Recorded));
		TestTrue(TEXT("a mode change is refused"),
			WoTReverifyHeldFileAfterMetadataChurn(File, Recorded, Size, Hashed,
				VerifiedMode, TEXT("held.bin")).IsError());
		TestTrue(TEXT("the file is made writable again"),
			Platform.SetReadOnly(*FilePath, false));
	}

	// A same-length in-place rewrite of the SAME object: also ChangeState-only,
	// and it must be refused.
	{
		TArray<uint8> Rewritten = Payload;
		Rewritten[100] ^= 0xFF;
		TestTrue(TEXT("the clock passes ChangeState before the rewrite"), TickPast());
		TUniquePtr<IFileHandle> Writer(FPlatformFileManager::Get().GetPlatformFile()
			.OpenWrite(*FilePath, /*bAppend=*/true, /*bAllowRead=*/true));
		if (!TestTrue(TEXT("the in-place writer opens"), Writer.IsValid()))
		{
			return false;
		}
		TestTrue(TEXT("the same-length rewrite lands in place"),
			Writer->Seek(0) && Writer->Write(Rewritten.GetData(), Rewritten.Num()));
		Writer.Reset();
	}
	TWoTBootstrapResult<FWoTNativeFileIdentity> Rewrote = File.QueryCurrentIdentity();
	if (TestFalse(TEXT("the rewritten identity reads"), Rewrote.IsError()))
	{
		TestTrue(TEXT("a same-length rewrite is indistinguishable by identity"),
			Rewrote.GetValue().DiffersOnlyInChangeState(Recorded));
	}
	TWoTBootstrapResult<FWoTNativeFileIdentity> Refused =
		WoTReverifyHeldFileAfterMetadataChurn(File, Recorded, Size, Hashed, VerifiedMode,
			TEXT("held.bin"));
	if (TestTrue(TEXT("a same-length rewrite is refused"), Refused.IsError()))
	{
		TestEqual(TEXT("the refusal is a corrupt installation"),
			Refused.GetError().Code, EWoTBootstrapError::CorruptInstallation);
	}

	// Growth is never metadata.
	TestTrue(TEXT("the clock passes ChangeState before the growth"), TickPast());
	{
		TUniquePtr<IFileHandle> Writer(FPlatformFileManager::Get().GetPlatformFile()
			.OpenWrite(*FilePath, /*bAppend=*/true, /*bAllowRead=*/true));
		const uint8 Extra = 0x5A;
		TestTrue(TEXT("one byte is appended"),
			Writer.IsValid() && Writer->Write(&Extra, 1));
	}
	TWoTBootstrapResult<FWoTNativeFileIdentity> Grown = File.QueryCurrentIdentity();
	TestTrue(TEXT("a size change is not ChangeState-only"),
		!Grown.IsError() && !Grown.GetValue().DiffersOnlyInChangeState(Recorded));

	// Directories churn the same way.
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> Directory =
		OpenNativeDirectoryNoFollow(DirPath);
	if (TestFalse(TEXT("the directory opens"), Directory.IsError()))
	{
		const FWoTNativeFileIdentity DirectoryBefore = Directory.GetValue().GetIdentity();
		TestTrue(TEXT("the clock passes the directory's ChangeState"),
			WoTNativeHandleTestFactory::WaitPastChangeStateTick(DirectoryBefore.ChangeState));
		TestTrue(TEXT("the directory takes metadata"),
			WoTNativeHandleTestFactory::ChangeMetadataOnly(DirPath));
		TWoTBootstrapResult<FWoTNativeFileIdentity> DirectoryAfter =
			Directory.GetValue().QueryCurrentIdentity();
		TestTrue(TEXT("directory metadata churn moves ChangeState and nothing else"),
			!DirectoryAfter.IsError()
			&& DirectoryAfter.GetValue().DiffersOnlyInChangeState(DirectoryBefore));
	}
	return !HasAnyErrors();
}

#endif  // WITH_AUTOMATION_TESTS
