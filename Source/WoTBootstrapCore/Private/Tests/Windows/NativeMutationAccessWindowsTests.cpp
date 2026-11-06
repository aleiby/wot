// Windows spec for the live-tree mutation access walk on a Modify-only ACL.
//
// A drive whose inherited ACL gives the user Modify (no FILE_DELETE_CHILD) makes
// the walk prove DELETE on every object itself. The running game holds its own
// paks and its working directory without FILE_SHARE_DELETE while it probes, so
// those opens fail with a sharing violation. NTFS checks the DACL before share
// access, so that violation still proves DELETE; an explicit DELETE deny must
// stay a "not writable" verdict even while the file is held.
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "../NativeHandleTestFactory.h"
#include "WoTBootstrapCore/NativeHandles.h"

#if WITH_AUTOMATION_TESTS

#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include <sddl.h>
#include "Windows/HideWindowsPlatformTypes.h"

#undef CreateDirectory
#undef CreateSymbolicLink
#undef CreateHardLink
#undef CreateFile
#undef DeleteFile
#undef MoveFileEx

namespace
{
constexpr EAutomationTestFlags WoTMutationAccessWindowsSpecFlags =
	EAutomationTestFlags::ProgramContext | EAutomationTestFlags::EditorContext
	| EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter;

// Modify for Authenticated Users (FILE_GENERIC_READ|WRITE|EXECUTE|DELETE):
// everything a typical secondary-drive ACL grants, and no FILE_DELETE_CHILD.
constexpr TCHAR WoTModifyOnlySddl[] = TEXT("D:P(A;;0x1301bf;;;AU)");
// The same, with DELETE explicitly denied on the object.
constexpr TCHAR WoTDeleteDeniedSddl[] = TEXT("D:P(D;;SD;;;AU)(A;;0x1301bf;;;AU)");
// Restored before cleanup so the temporary tree can always be removed.
constexpr TCHAR WoTFullAccessSddl[] = TEXT("D:P(A;;FA;;;AU)");

FString WoTWindowsPath(const FString& Path)
{
	FString Full = FPaths::ConvertRelativePathToFull(Path);
	Full.ReplaceInline(TEXT("/"), TEXT("\\"));
	return Full;
}

bool WoTSetDacl(const FString& Path, const TCHAR* Sddl)
{
	PSECURITY_DESCRIPTOR Descriptor = nullptr;
	if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
		Sddl, SDDL_REVISION_1, &Descriptor, nullptr))
	{
		return false;
	}
	ON_SCOPE_EXIT { ::LocalFree(Descriptor); };
	return ::SetFileSecurityW(*WoTWindowsPath(Path),
		DACL_SECURITY_INFORMATION, Descriptor) != 0;
}

bool WoTWriteFile(const FString& Path)
{
	const HANDLE File = ::CreateFileW(*WoTWindowsPath(Path), GENERIC_WRITE, 0,
		nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (File == INVALID_HANDLE_VALUE)
	{
		return false;
	}
	const char Bytes[] = "wot";
	DWORD Written = 0;
	const bool bWrote = ::WriteFile(File, Bytes, 3, &Written, nullptr) && Written == 3;
	::CloseHandle(File);
	return bWrote;
}

/** Removes its root on destruction so a failing spec cannot leak a temp tree. */
struct FWoTMutationAccessWindowsScope
{
	FString Root;
	TArray<FString> Restricted;
	TArray<HANDLE> Held;

	FWoTMutationAccessWindowsScope()
		: Root(WoTNativeHandleTestFactory::CreateTemporaryRoot())
	{
	}

	~FWoTMutationAccessWindowsScope()
	{
		for (HANDLE Handle : Held)
		{
			::CloseHandle(Handle);
		}
		// Children first never matters for SetFileSecurity, but full access on
		// every restricted object must be back before the tree is removed.
		for (const FString& Path : Restricted)
		{
			(void)WoTSetDacl(Path, WoTFullAccessSddl);
		}
		WoTNativeHandleTestFactory::RemoveTree(Root);
	}

	FString operator/(const TCHAR* Leaf) const { return Root + TEXT("/") + Leaf; }

	bool Restrict(const FString& Path, const TCHAR* Sddl)
	{
		Restricted.Add(Path);
		return WoTSetDacl(Path, Sddl);
	}

	/** Holds a file the way UE holds an open pak: read access, read sharing only. */
	bool HoldFile(const FString& Path)
	{
		const HANDLE Handle = ::CreateFileW(*WoTWindowsPath(Path), GENERIC_READ,
			FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (Handle == INVALID_HANDLE_VALUE) return false;
		Held.Add(Handle);
		return true;
	}

	/** Holds a directory the way a process's working directory is held. */
	bool HoldDirectory(const FString& Path)
	{
		const HANDLE Handle = ::CreateFileW(*WoTWindowsPath(Path),
			FILE_TRAVERSE | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
			OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
		if (Handle == INVALID_HANDLE_VALUE) return false;
		Held.Add(Handle);
		return true;
	}
};
}  // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNativeMutationAccessHeldModifyOnlySpec,
	"WoT.BootstrapCore.Identity.MutationAccessHeldModifyOnly",
	WoTMutationAccessWindowsSpecFlags)

bool FWoTNativeMutationAccessHeldModifyOnlySpec::RunTest(const FString&)
{
	FWoTMutationAccessWindowsScope Scope;
	const FString Game = Scope / TEXT("Game");
	const FString Binaries = Game + TEXT("/Binaries");
	const FString Denied = Scope / TEXT("Denied");
	if (!TestTrue(TEXT("the fixture tree is created"),
		::CreateDirectoryW(*WoTWindowsPath(Game), nullptr)
			&& ::CreateDirectoryW(*WoTWindowsPath(Binaries), nullptr)
			&& ::CreateDirectoryW(*WoTWindowsPath(Denied), nullptr)
			&& WoTWriteFile(Game + TEXT("/Game.pak"))
			&& WoTWriteFile(Binaries + TEXT("/Game.exe"))
			&& WoTWriteFile(Denied + TEXT("/Locked.pak"))))
	{
		return false;
	}
	// Leaves first, so each object is still reachable while its DACL changes.
	const bool bRestricted =
		Scope.Restrict(Binaries + TEXT("/Game.exe"), WoTModifyOnlySddl)
		&& Scope.Restrict(Game + TEXT("/Game.pak"), WoTModifyOnlySddl)
		&& Scope.Restrict(Binaries, WoTModifyOnlySddl)
		&& Scope.Restrict(Game, WoTModifyOnlySddl)
		&& Scope.Restrict(Denied + TEXT("/Locked.pak"), WoTDeleteDeniedSddl)
		&& Scope.Restrict(Denied, WoTModifyOnlySddl);
	if (!TestTrue(TEXT("the Modify-only DACLs are applied"), bRestricted))
	{
		return false;
	}
	if (!TestTrue(TEXT("the running game's pak, working directory and a denied pak are held"),
		Scope.HoldFile(Game + TEXT("/Game.pak"))
			&& Scope.HoldDirectory(Binaries)
			&& Scope.HoldFile(Denied + TEXT("/Locked.pak"))))
	{
		return false;
	}

	TWoTBootstrapResult<FWoTNativeDirectoryHandle> Root =
		OpenNativeDirectoryNoFollow(Scope.Root);
	if (!TestFalse(TEXT("the temporary root opens"), Root.IsError()))
	{
		if (Root.IsError()) AddError(Root.GetError().ToString());
		return false;
	}

	TWoTBootstrapResult<bool> Writable =
		CheckNativeDirectoryMutationAccessAtNoFollow(Root.GetValue(), TEXT("Game"),
			[]() { return FWoTBootstrapStatus::Success(); });
	if (!TestFalse(TEXT("held entries are not an access-check error"), Writable.IsError()))
	{
		AddError(Writable.GetError().ToString());
	}
	else
	{
		TestTrue(TEXT("a sharing violation proves DELETE: the live tree is writable"),
			Writable.GetValue());
	}

	TWoTBootstrapResult<bool> Locked =
		CheckNativeDirectoryMutationAccessAtNoFollow(Root.GetValue(), TEXT("Denied"),
			[]() { return FWoTBootstrapStatus::Success(); });
	if (!TestFalse(TEXT("an explicit DELETE deny is a verdict, not an error"),
		Locked.IsError()))
	{
		AddError(Locked.GetError().ToString());
	}
	else
	{
		TestFalse(TEXT("an explicit DELETE deny stays not writable while held"),
			Locked.GetValue());
	}
	return !HasAnyErrors();
}

#endif
