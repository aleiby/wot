#include "../NativeHandleTestFactory.h"

#if WITH_AUTOMATION_TESTS

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"
#include "Misc/Guid.h"

#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include <winioctl.h>
#include "Windows/HideWindowsPlatformTypes.h"

// windows.h defines these factory names as A/W-dispatch MACROS, which rewrite
// the DEFINITIONS below (this TU sees windows.h) but not the DECLARATIONS in
// NativeHandleTestFactory.h (the consuming spec TU does not) — the mismatch is
// an unresolved external at link time, not a compile error. Undef them so the
// definitions keep the names the header declares; the ::CreateXxxW calls in
// the bodies are already explicit, so nothing else needs the macros.
#undef CreateDirectory
#undef CreateSymbolicLink
#undef CreateHardLink
#undef CreateFile
#undef DeleteFile
#undef MoveFileEx

namespace WoTNativeHandleTestFactory
{
FString CreateTemporaryRoot()
{
	TCHAR Buffer[MAX_PATH + 1] = {};
	const DWORD Length = ::GetTempPathW(MAX_PATH, Buffer);
	if (Length == 0)
	{
		return FString();
	}
	const FString Root = FString(Buffer) / FString::Printf(TEXT("wot-native-%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	return ::CreateDirectoryW(*Root, nullptr) ? Root : FString();
}

void RemoveTree(const FString& AbsolutePath)
{
	if (!AbsolutePath.IsEmpty())
	{
		FPlatformFileManager::Get().GetPlatformFile().DeleteDirectoryRecursively(
			*AbsolutePath);
	}
}

bool WriteFile(const FString& AbsolutePath, TConstArrayView<uint8> Bytes)
{
	// Sharing flags, not 0: the TOCTOU specs mutate an installer while a reader
	// still holds it, which is unremarkable on POSIX but needs saying here.
	// Windows share modes are symmetric -- a dwShareMode of 0 demands exclusive
	// access and fails with ERROR_SHARING_VIOLATION against any outstanding
	// handle, so the mutation those specs depend on could never happen and they
	// failed on the assertion that it had. The reader opens the same three flags
	// deliberately (NativeHandlesWindows.cpp).
	const HANDLE Handle = ::CreateFileW(*AbsolutePath, GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
		CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (Handle == INVALID_HANDLE_VALUE)
	{
		return false;
	}
	// `!= 0`, not `!= FALSE`, throughout this file: UE's
	// HideWindowsPlatformTypes.h undefines TRUE/FALSE (:23,:27) and every use
	// here is below that include.
	DWORD Written = 0;
	const BOOL bOk = ::WriteFile(Handle, Bytes.GetData(),
		static_cast<DWORD>(Bytes.Num()), &Written, nullptr);
	::CloseHandle(Handle);
	return bOk != 0 && Written == static_cast<DWORD>(Bytes.Num());
}

bool CreateSparseFile(const FString& AbsolutePath, uint64 Size)
{
	const HANDLE Handle = ::CreateFileW(*AbsolutePath, GENERIC_WRITE, 0, nullptr,
		CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (Handle == INVALID_HANDLE_VALUE)
	{
		return false;
	}
	DWORD Ignored = 0;
	const BOOL bSparse = ::DeviceIoControl(Handle, FSCTL_SET_SPARSE, nullptr, 0,
		nullptr, 0, &Ignored, nullptr);
	LARGE_INTEGER End = {};
	End.QuadPart = static_cast<LONGLONG>(Size);
	const BOOL bMoved = ::SetFilePointerEx(Handle, End, nullptr, FILE_BEGIN);
	const BOOL bSized = bMoved != 0 ? ::SetEndOfFile(Handle) : 0;
	::CloseHandle(Handle);
	return bSparse != 0 && bMoved != 0 && bSized != 0;
}

bool CreateDirectory(const FString& AbsolutePath)
{
	return ::CreateDirectoryW(*AbsolutePath, nullptr) != 0;
}

bool CreateSymbolicLink(const FString& LinkAbsolutePath, const FString& Target)
{
	// Unprivileged processes can only do this with developer mode enabled, so a
	// false return means "skip the spec", never "the production code is wrong".
	return ::CreateSymbolicLinkW(*LinkAbsolutePath, *Target,
		SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != 0;
}

bool CreateDirectoryAncestorAlias(
	const FString& LinkAbsolutePath, const FString& Target)
{
	const FString Arguments = FString::Printf(
		TEXT("/d /c mklink /J \"%s\" \"%s\" >nul"),
		*LinkAbsolutePath, *Target);
	int32 ReturnCode = INDEX_NONE;
	return FPlatformProcess::ExecProcess(
		TEXT("cmd.exe"), *Arguments, &ReturnCode, nullptr, nullptr)
		&& ReturnCode == 0;
}

bool CreateHardLink(const FString& LinkAbsolutePath, const FString& ExistingAbsolutePath)
{
	return ::CreateHardLinkW(*LinkAbsolutePath, *ExistingAbsolutePath, nullptr) != 0;
}

// FILE_RENAME_INFO as documented for the FileRenameInfoEx class. Declared
// locally rather than taken from the SDK because the Flags-union form only
// appears when the SDK's _WIN32_WINNT targets RS1+, and this file must not
// depend on that being set.
struct FWoTFileRenameInfoEx
{
	DWORD Flags;
	HANDLE RootDirectory;
	DWORD FileNameLength;
	WCHAR FileName[1];
};

bool ReplaceByRename(const FString& AbsolutePath, const FString& ReplacementAbsolutePath)
{
	// MoveFileExW CANNOT do this: with MOVEFILE_REPLACE_EXISTING it fails
	// ERROR_ACCESS_DENIED whenever the destination has an open handle, even one
	// opened with FILE_SHARE_DELETE (probed on this box: held handle -> err 5;
	// no handle -> success). POSIX rename() has no such restriction, so the
	// macOS factory can stage the swap-under-an-open-handle race directly and a
	// literal transliteration cannot. FileRenameInfoEx + POSIX_SEMANTICS is the
	// Win32 equivalent (Win10 1709+) and does replace an open file.
	//
	// This is the ATTACKER simulation, not production code: the spec's subject
	// is that a held HANDLE keeps naming the original object after its path is
	// replaced, so the fixture must be able to perform the replacement.
	const DWORD kDelete = 0x00010000;      // DELETE
	const DWORD kSynchronize = 0x00100000; // SYNCHRONIZE
	const DWORD kReplaceIfExists = 0x00000001;
	const DWORD kPosixSemantics = 0x00000002;
	const int32 kFileRenameInfoEx = 22;

	const HANDLE Source = ::CreateFileW(*ReplacementAbsolutePath,
		kDelete | kSynchronize,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
		OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	if (Source != INVALID_HANDLE_VALUE)
	{
		const int32 NameBytes = AbsolutePath.Len() * sizeof(WCHAR);
		TArray<uint8> Buffer;
		Buffer.SetNumZeroed(sizeof(FWoTFileRenameInfoEx) + NameBytes);
		FWoTFileRenameInfoEx* Info =
			reinterpret_cast<FWoTFileRenameInfoEx*>(Buffer.GetData());
		Info->Flags = kReplaceIfExists | kPosixSemantics;
		Info->RootDirectory = nullptr;
		Info->FileNameLength = static_cast<DWORD>(NameBytes);
		FMemory::Memcpy(Info->FileName, *AbsolutePath, NameBytes);

		const BOOL bRenamed = ::SetFileInformationByHandle(Source,
			static_cast<FILE_INFO_BY_HANDLE_CLASS>(kFileRenameInfoEx),
			Buffer.GetData(), static_cast<DWORD>(Buffer.Num()));
		::CloseHandle(Source);
		if (bRenamed != 0)
		{
			return true;
		}
	}

	// Pre-1709 (or a filesystem without POSIX rename): the open-handle case
	// genuinely cannot be staged, and the spec should fail loudly rather than
	// silently testing nothing.
	return ::MoveFileExW(*ReplacementAbsolutePath, *AbsolutePath,
		MOVEFILE_REPLACE_EXISTING) != 0;
}

bool ReplaceDirectoryByRename(const FString& AbsolutePath,
	const FString& ReplacementAbsolutePath)
{
	const FString Retired = AbsolutePath + TEXT("-retired");
	if (!::MoveFileExW(*AbsolutePath, *Retired, 0))
	{
		return false;
	}
	if (::MoveFileExW(*ReplacementAbsolutePath, *AbsolutePath, 0))
	{
		return true;
	}
	::MoveFileExW(*Retired, *AbsolutePath, 0);
	return false;
}

bool RemoveName(const FString& AbsolutePath)
{
	return ::DeleteFileW(*AbsolutePath) != 0;
}

bool WaitPastChangeStateTick(uint64 ChangeState)
{
	// ChangeState is FILE_BASIC_INFO.ChangeTime: FILETIME units in the same
	// domain as GetSystemTimeAsFileTime, which advances once per timer tick.
	for (int32 Attempt = 0; Attempt < 13; ++Attempt)
	{
		FILETIME Now = {};
		::GetSystemTimeAsFileTime(&Now);
		const uint64 Stamp = (static_cast<uint64>(Now.dwHighDateTime) << 32)
			| static_cast<uint64>(Now.dwLowDateTime);
		if (Stamp > ChangeState)
		{
			return true;
		}
		::Sleep(16);
	}
	return false;
}

bool ChangeMetadataOnly(const FString& AbsolutePath)
{
	// A named stream, as Zone.Identifier and SmartScreen are. NTFS moves the
	// object's ChangeTime (and LastWriteTime) but not its size, links or id.
	const FString Stream = AbsolutePath + TEXT(":wot.metadata-only");
	const HANDLE Handle = ::CreateFileW(*Stream, GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
		OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (Handle == INVALID_HANDLE_VALUE)
	{
		return false;
	}
	static uint32 Generation = 0;
	const FString Text = FString::Printf(
		TEXT("[ZoneTransfer]\r\nZoneId=3\r\n%u\r\n"), ++Generation);
	const FTCHARToUTF8 Utf8(*Text);
	DWORD Written = 0;
	const bool bWritten = ::WriteFile(Handle, Utf8.Get(),
		static_cast<DWORD>(Utf8.Length()), &Written, nullptr) != 0
		&& Written == static_cast<DWORD>(Utf8.Length());
	::CloseHandle(Handle);
	return bWritten;
}
}  // namespace WoTNativeHandleTestFactory

#endif  // WITH_AUTOMATION_TESTS
