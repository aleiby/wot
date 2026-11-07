// Windows specs for the single-leaf lookup the installer probe uses instead of
// listing a folder. The shared spec covers the portable contract; these cover
// what only NTFS can do: a name whose object is already being deleted, a
// junction, case-insensitive lookup, and a writer that shares nothing.
#include "Misc/AutomationTest.h"
#include "HAL/PlatformProcess.h"
#include "../NativeHandleTestFactory.h"
#include "WoTBootstrapCore/NativeHandles.h"

#if WITH_AUTOMATION_TESTS

#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"

// As in NativeHandleTestFactoryWindows.cpp: windows.h's A/W-dispatch macros
// would rewrite the factory calls below, and any later TU in a unity blob.
#undef CreateDirectory
#undef CreateSymbolicLink
#undef CreateHardLink
#undef CreateFile
#undef DeleteFile
#undef MoveFileEx

namespace
{
constexpr EAutomationTestFlags WoTEntryQueryWindowsSpecFlags =
	EAutomationTestFlags::ProgramContext | EAutomationTestFlags::EditorContext
	| EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter;

/** Removes its root on destruction so a failing spec cannot leak a temp tree. */
struct FWoTEntryQueryWindowsScope
{
	FString Root;

	FWoTEntryQueryWindowsScope()
		: Root(WoTNativeHandleTestFactory::CreateTemporaryRoot())
	{
	}

	~FWoTEntryQueryWindowsScope()
	{
		WoTNativeHandleTestFactory::RemoveTree(Root);
	}

	FString operator/(const TCHAR* Leaf) const { return Root + TEXT("/") + Leaf; }
};

TArray<uint8> WoTEntryQueryWindowsBytes()
{
	return TArray<uint8>({'w', 'o', 't'});
}
}  // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNativeEntryQueryWindowsSpec,
	"WoT.BootstrapCore.Identity.EntryQueryWindows", WoTEntryQueryWindowsSpecFlags)

bool FWoTNativeEntryQueryWindowsSpec::RunTest(const FString&)
{
	for (uint32 CloudVariant = 0; CloudVariant <= 0xf; ++CloudVariant)
	{
		const uint32 Tag = 0x9000001au | (CloudVariant << 12);
		TestEqual(TEXT("every cloud data tag is installer-readable"),
			WoTClassifyInstallerSourceReparseTag(Tag),
			EWoTInstallerSourceReparseTagClass::DataContent);
	}
	for (const uint32 Tag : {0x80000013u, 0x80000017u})
	{
		TestEqual(TEXT("dedup/WOF tags are installer-readable"),
			WoTClassifyInstallerSourceReparseTag(Tag),
			EWoTInstallerSourceReparseTagClass::DataContent);
	}
	for (const uint32 Tag : {0xa0000003u, 0xa000000cu, 0x8000001bu})
	{
		TestEqual(TEXT("name-surrogate tags are refused"),
			WoTClassifyInstallerSourceReparseTag(Tag),
			EWoTInstallerSourceReparseTagClass::NameSurrogate);
	}
	TestEqual(TEXT("an unrecognized reparse tag is refused"),
		WoTClassifyInstallerSourceReparseTag(0x8000fff0u),
		EWoTInstallerSourceReparseTagClass::Unknown);

	FWoTEntryQueryWindowsScope Scope;
	if (!TestFalse(TEXT("a temporary root was created"), Scope.Root.IsEmpty()))
	{
		return false;
	}
	if (!TestTrue(TEXT("fixtures were created"),
			WoTNativeHandleTestFactory::WriteFile(Scope / TEXT("plain.bin"),
				WoTEntryQueryWindowsBytes())
			&& WoTNativeHandleTestFactory::CreateDirectory(Scope / TEXT("sub"))))
	{
		return false;
	}
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> Root =
		OpenNativeDirectoryNoFollow(Scope.Root);
	if (!TestFalse(TEXT("the root opens"), Root.IsError()))
	{
		AddError(Root.GetError().ToString());
		return false;
	}

	auto ExpectType = [this, &Root](const TCHAR* What, const TCHAR* Leaf,
		TOptional<EWoTTreeEntryType> Expected)
	{
		TWoTBootstrapResult<TOptional<EWoTTreeEntryType>> Found =
			QueryNativeEntryTypeAtNoFollow(Root.GetValue(), Leaf);
		if (!TestFalse(FString::Printf(TEXT("%s: the lookup succeeds"), What),
				Found.IsError()))
		{
			AddError(Found.GetError().ToString());
			return;
		}
		TestEqual(FString::Printf(TEXT("%s: present"), What),
			Found.GetValue().IsSet(), Expected.IsSet());
		if (Found.GetValue().IsSet() && Expected.IsSet())
		{
			TestEqual(FString::Printf(TEXT("%s: type"), What),
				Found.GetValue().GetValue(), Expected.GetValue());
		}
	};

	ExpectType(TEXT("a plain file"), TEXT("plain.bin"), EWoTTreeEntryType::File);
	ExpectType(TEXT("a directory"), TEXT("sub"), EWoTTreeEntryType::Directory);
	ExpectType(TEXT("a missing leaf"), TEXT("missing.bin"), {});
	// NTFS lookup ignores case, and the installer's
	// content pins (not its spelling) are the acceptance test.
	ExpectType(TEXT("another spelling of the same name"), TEXT("PLAIN.BIN"),
		EWoTTreeEntryType::File);

	// A junction needs no privilege, so link reporting is always exercised.
	if (TestTrue(TEXT("a junction was created"),
			WoTNativeHandleTestFactory::CreateDirectoryAncestorAlias(
				Scope / TEXT("alias"), Scope / TEXT("sub"))))
	{
		ExpectType(TEXT("a junction"), TEXT("alias"), EWoTTreeEntryType::SymbolicLink);
		TWoTBootstrapResult<TOptional<FWoTInstallerSourceEntryInfo>> InstallerEntry =
			QueryNativeInstallerSourceEntryAtNoFollow(
				Root.GetValue(), TEXT("alias"));
		if (TestFalse(TEXT("the installer junction query succeeds"),
				InstallerEntry.IsError())
			&& TestTrue(TEXT("the installer junction is present"),
				InstallerEntry.GetValue().IsSet()))
		{
			TestTrue(TEXT("the installer junction carries an explicit refusal"),
				InstallerEntry.GetValue()->bRefusedReparsePoint);
			TestEqual(TEXT("the installer junction retains its name-surrogate tag"),
				WoTClassifyInstallerSourceReparseTag(
					InstallerEntry.GetValue()->ReparseTag),
				EWoTInstallerSourceReparseTagClass::NameSurrogate);
		}
	}

	// compact.exe creates the real Microsoft WOF filter used by Windows. A
	// supported volume must exercise the entire no-follow validation -> file-ID
	// following read path; only an explicit volume capability result may skip it.
	{
		TWoTBootstrapResult<bool> SupportsWof =
			QueryNativeVolumeSupportsWof(Root.GetValue());
		if (!TestFalse(TEXT("the WOF volume capability query succeeds"),
				SupportsWof.IsError()))
		{
			AddError(SupportsWof.GetError().ToString());
			return false;
		}
		if (!SupportsWof.GetValue())
		{
			AddWarning(TEXT("real WOF fixture skipped: the temporary volume "
				"explicitly reports no NTFS file-compression/WOF support"));
		}
		else
		{
			const FString WofPath = Scope / TEXT("wof.bin");
			TArray<uint8> WofBytes;
			WofBytes.Init(static_cast<uint8>('W'), 2 * 1024 * 1024);
			if (!TestTrue(TEXT("the >=1 MiB WOF candidate file is created"),
					WoTNativeHandleTestFactory::WriteFile(WofPath, WofBytes)))
			{
				return false;
			}
			int32 ExitCode = INDEX_NONE;
			FString Output;
			FString Error;
			const FString Arguments = FString::Printf(
				TEXT("/c /exe:xpress4k \"%s\""), *WofPath);
			const bool bRan = FPlatformProcess::ExecProcess(TEXT("compact.exe"),
				*Arguments, &ExitCode, &Output, &Error);
			TestTrue(TEXT("compact.exe executes for the supported volume"), bRan);
			TestEqual(TEXT("compact.exe creates the WOF fixture"), ExitCode, 0);
			TWoTBootstrapResult<TOptional<FWoTInstallerSourceEntryInfo>> Wof =
				QueryNativeInstallerSourceEntryAtNoFollow(
					Root.GetValue(), TEXT("wof.bin"));
			if (!TestFalse(TEXT("the real WOF leaf query succeeds"), Wof.IsError())
				|| !TestTrue(TEXT("the real WOF leaf is present"),
					Wof.GetValue().IsSet()))
			{
				if (Wof.IsError()) AddError(Wof.GetError().ToString());
				AddError(Output + Error);
				return false;
			}
			TestEqual(TEXT("the physical reparse tag is IO_REPARSE_TAG_WOF"),
				Wof.GetValue()->ReparseTag, 0x80000017u);
			TestEqual(TEXT("the real WOF leaf remains a file"),
				Wof.GetValue()->Type, EWoTTreeEntryType::File);
			TWoTBootstrapResult<FWoTNativeFileHandle> Opened =
				OpenNativeInstallerSourceFileAtReadNoFollow(
					Root.GetValue(), TEXT("wof.bin"));
			if (TestFalse(TEXT("the real WOF leaf opens"), Opened.IsError()))
			{
				TWoTBootstrapResult<TArray<uint8>> Read = Opened.GetValue().ReadAll();
				TestTrue(TEXT("the filter-following data handle returns the original "
					"uncompressed bytes"), !Read.IsError()
						&& Read.GetValue() == WofBytes);
			}
		}
	}

	// Anything but one plain leaf is refused before any syscall; ':' would name
	// an NTFS alternate data stream.
	for (const TCHAR* Leaf : {TEXT(""), TEXT("."), TEXT(".."), TEXT("a/b"),
			TEXT("a\\b"), TEXT("plain.bin:stream")})
	{
		TWoTBootstrapResult<TOptional<EWoTTreeEntryType>> Refused =
			QueryNativeEntryTypeAtNoFollow(Root.GetValue(), Leaf);
		if (TestTrue(FString::Printf(TEXT("'%s' is refused"), Leaf), Refused.IsError()))
		{
			TestEqual(FString::Printf(TEXT("'%s' is refused as unsafe"), Leaf),
				Refused.GetError().Code, EWoTBootstrapError::UnsafePath);
		}
	}

	// A writer that shares nothing (as a download in progress may) does not
	// block the lookup: an attribute-only open takes no part in share checks.
	{
		const FString LockedPath = Scope / TEXT("locked.bin");
		const HANDLE Writer = ::CreateFileW(*LockedPath, GENERIC_WRITE, 0, nullptr,
			CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (TestTrue(TEXT("an exclusive writer holds a new file"),
				Writer != INVALID_HANDLE_VALUE))
		{
			ExpectType(TEXT("a file held with no sharing"), TEXT("locked.bin"),
				EWoTTreeEntryType::File);
			::CloseHandle(Writer);
		}
	}

	// A name whose object is already being deleted reads as absent. The legacy
	// disposition keeps the name visible until the last handle closes, and every
	// open of it fails with STATUS_DELETE_PENDING - which Win32 reports as
	// ERROR_ACCESS_DENIED, the reason the lookup must decide on the NTSTATUS.
	{
		const FString DoomedPath = Scope / TEXT("doomed.bin");
		if (TestTrue(TEXT("the doomed file was created"),
				WoTNativeHandleTestFactory::WriteFile(DoomedPath,
					WoTEntryQueryWindowsBytes())))
		{
			const HANDLE Holder = ::CreateFileW(*DoomedPath, DELETE,
				FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
				OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			// FILE_DISPOSITION_INFO is one BOOLEAN; its member name is rewritten by
			// the DeleteFile macro at header parse time, so pass the flag directly.
			static_assert(sizeof(FILE_DISPOSITION_INFO) == sizeof(BOOLEAN));
			BOOLEAN DeleteOnLastClose = 1;  // TRUE is hidden by HideWindowsPlatformTypes
			if (TestTrue(TEXT("the doomed file is marked delete-pending"),
					Holder != INVALID_HANDLE_VALUE
					&& ::SetFileInformationByHandle(Holder, FileDispositionInfo,
						&DeleteOnLastClose, sizeof(DeleteOnLastClose)) != 0))
			{
				const HANDLE Probe = ::CreateFileW(*DoomedPath, FILE_READ_ATTRIBUTES,
					FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
					OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
				const DWORD ProbeError = ::GetLastError();
				if (Probe != INVALID_HANDLE_VALUE)
				{
					::CloseHandle(Probe);
				}
				TestTrue(TEXT("Win32 reports the pending delete as access denied"),
					Probe == INVALID_HANDLE_VALUE && ProbeError == ERROR_ACCESS_DENIED);
				ExpectType(TEXT("a delete-pending name"), TEXT("doomed.bin"), {});
			}
			if (Holder != INVALID_HANDLE_VALUE)
			{
				::CloseHandle(Holder);
			}
			ExpectType(TEXT("the deleted name"), TEXT("doomed.bin"), {});
		}
	}

	return !HasAnyErrors();
}

// Player builds run without -unattended, so UE never sets
// SEM_FAILCRITICALERRORS; the bootstrap worker sets it on its own thread so
// a probe of an empty card reader returns an error instead of a dialog.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFailCriticalErrorsOnThisThreadSpec,
	"WoT.BootstrapCore.Windows.FailCriticalErrorsOnThisThread",
	WoTEntryQueryWindowsSpecFlags)
bool FWoTFailCriticalErrorsOnThisThreadSpec::RunTest(const FString&)
{
	const DWORD Before = ::GetThreadErrorMode();
	::SetThreadErrorMode(Before & ~static_cast<DWORD>(SEM_FAILCRITICALERRORS), nullptr);
	WoTFailCriticalErrorsOnThisThread();
	const DWORD After = ::GetThreadErrorMode();
	::SetThreadErrorMode(Before, nullptr);
	TestTrue(TEXT("the critical-error dialog is suppressed on this thread"),
		(After & SEM_FAILCRITICALERRORS) != 0);
	TestEqual(TEXT("other error-mode bits are kept"),
		After & ~static_cast<DWORD>(SEM_FAILCRITICALERRORS),
		Before & ~static_cast<DWORD>(SEM_FAILCRITICALERRORS));
	return !HasAnyErrors();
}

#endif  // WITH_AUTOMATION_TESTS
