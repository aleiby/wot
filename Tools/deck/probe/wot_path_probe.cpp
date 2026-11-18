// wot_path_probe: a Wine/Proton capability census for WoT Reborn's Windows
// native file layer, updater locks and update transport.
//
// The game's path-safety walk, cache publish, staging, delete and update
// check rely on specific NT/Win32 behaviour. Under Wine some of it differs by
// drive (Z: maps the Linux root, C: the prefix), by mount (/, /home, SD card)
// and by Wine version. This program measures each primitive once and prints
// one JSON object per line, so a single run on a device settles every rule
// before code depends on it. It changes nothing outside the scratch
// directory it is given.
//
// Build (MSVC x64, Developer Command Prompt):
//   cl /nologo /W4 /O2 /EHsc /DUNICODE /D_UNICODE wot_path_probe.cpp ^
//      /link winhttp.lib shell32.lib ole32.lib advapi32.lib
//
// Usage:
//   wot_path_probe env
//   wot_path_probe walk <absolute path>            per-component identity
//   wot_path_probe census <writable scratch dir>   file primitives + locks
//   wot_path_probe http [https url]                update-transport options
//   wot_path_probe lockholder <file> <ms>          internal: shared lock child

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <winhttp.h>
#include <winioctl.h>
#include <shlobj.h>
#include <knownfolders.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace
{
// ---------------------------------------------------------------- JSON out

std::string Utf8(const std::wstring& Text)
{
	if (Text.empty()) return std::string();
	const int Bytes = ::WideCharToMultiByte(CP_UTF8, 0, Text.c_str(),
		static_cast<int>(Text.size()), nullptr, 0, nullptr, nullptr);
	std::string Out(static_cast<size_t>(Bytes), '\0');
	::WideCharToMultiByte(CP_UTF8, 0, Text.c_str(), static_cast<int>(Text.size()),
		Out.data(), Bytes, nullptr, nullptr);
	return Out;
}

std::string Escape(const std::string& Text)
{
	std::string Out;
	for (const unsigned char C : Text)
	{
		switch (C)
		{
		case '"': Out += "\\\""; break;
		case '\\': Out += "\\\\"; break;
		case '\n': Out += "\\n"; break;
		case '\r': Out += "\\r"; break;
		case '\t': Out += "\\t"; break;
		default:
			if (C < 0x20) { char B[8]; std::snprintf(B, sizeof(B), "\\u%04x", C); Out += B; }
			else Out += static_cast<char>(C);
		}
	}
	return Out;
}

std::string Hex(unsigned long long Value, int Digits)
{
	char B[32];
	std::snprintf(B, sizeof(B), "0x%0*llx", Digits, Value);
	return B;
}

class FLine
{
public:
	explicit FLine(const char* Check) { Body = "{\"check\":\"" + Escape(Check) + "\""; }
	FLine& Str(const char* Key, const std::string& Value)
	{ Body += ",\"" + std::string(Key) + "\":\"" + Escape(Value) + "\""; return *this; }
	FLine& Str(const char* Key, const std::wstring& Value) { return Str(Key, Utf8(Value)); }
	FLine& Num(const char* Key, unsigned long long Value)
	{ Body += ",\"" + std::string(Key) + "\":" + std::to_string(Value); return *this; }
	FLine& Bool(const char* Key, bool Value)
	{ Body += ",\"" + std::string(Key) + "\":" + (Value ? "true" : "false"); return *this; }
	FLine& Win32(DWORD Error) { return Num("win32", Error); }
	FLine& Status(NTSTATUS Value) { return Str("ntstatus", Hex(static_cast<ULONG>(Value), 8)); }
	void Emit() { std::printf("%s}\n", Body.c_str()); std::fflush(stdout); }
private:
	std::string Body;
};

// ------------------------------------------------------------- ntdll calls

#ifndef FILE_OPEN
#define FILE_OPEN 0x00000001
#endif
#ifndef FILE_CREATE
#define FILE_CREATE 0x00000002
#endif
#ifndef FILE_DIRECTORY_FILE
#define FILE_DIRECTORY_FILE 0x00000001
#endif
#ifndef FILE_NON_DIRECTORY_FILE
#define FILE_NON_DIRECTORY_FILE 0x00000040
#endif
#ifndef FILE_SYNCHRONOUS_IO_NONALERT
#define FILE_SYNCHRONOUS_IO_NONALERT 0x00000020
#endif
#ifndef FILE_DELETE_ON_CLOSE
#define FILE_DELETE_ON_CLOSE 0x00001000
#endif
#ifndef FILE_OPEN_FOR_BACKUP_INTENT
#define FILE_OPEN_FOR_BACKUP_INTENT 0x00004000
#endif
#ifndef FILE_OPEN_REPARSE_POINT
#define FILE_OPEN_REPARSE_POINT 0x00200000
#endif
#ifndef OBJ_CASE_INSENSITIVE
#define OBJ_CASE_INSENSITIVE 0x00000040L
#endif

constexpr int ClassFileRename = 10;
constexpr int ClassFileDispositionEx = 64;
constexpr int ClassFileRenameEx = 65;
constexpr ULONG RenameReplaceIfExists = 0x1;
constexpr ULONG RenamePosixSemantics = 0x2;
constexpr ULONG DispositionDelete = 0x1;
constexpr ULONG DispositionPosixSemantics = 0x2;
constexpr ULONG DispositionIgnoreReadOnly = 0x10;
constexpr NTSTATUS StatusUnsuccessful = static_cast<NTSTATUS>(0xC0000001L);

using FNtCreateFile = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES,
	PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
using FNtSetInformationFile = NTSTATUS(NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID,
	ULONG, int);
using FWineGetVersion = const char*(CDECL*)();
using FWineGetHostVersion = void(CDECL*)(const char**, const char**);
using FWineGetDosFileName = LPWSTR(CDECL*)(LPCSTR);

HMODULE Ntdll() { return ::GetModuleHandleW(L"ntdll.dll"); }
FNtCreateFile NtCreate()
{ return reinterpret_cast<FNtCreateFile>(::GetProcAddress(Ntdll(), "NtCreateFile")); }
FNtSetInformationFile NtSetInfo()
{ return reinterpret_cast<FNtSetInformationFile>(::GetProcAddress(Ntdll(), "NtSetInformationFile")); }

DWORD NtToWin32(NTSTATUS Status)
{
	using FConvert = ULONG(NTAPI*)(NTSTATUS);
	const auto Convert = reinterpret_cast<FConvert>(
		::GetProcAddress(Ntdll(), "RtlNtStatusToDosError"));
	return Convert ? Convert(Status) : 0;
}

// Opens Name relative to Parent (or absolute when Parent is null), exactly as
// the game's walk does: as the reparse point itself, with backup intent.
NTSTATUS OpenAt(HANDLE Parent, const std::wstring& Name, ACCESS_MASK Access,
	ULONG Disposition, ULONG Options, HANDLE& Out)
{
	UNICODE_STRING Unicode;
	Unicode.Buffer = const_cast<PWSTR>(Name.c_str());
	Unicode.Length = static_cast<USHORT>(Name.size() * sizeof(wchar_t));
	Unicode.MaximumLength = Unicode.Length;
	OBJECT_ATTRIBUTES Attributes;
	InitializeObjectAttributes(&Attributes, &Unicode, OBJ_CASE_INSENSITIVE, Parent, nullptr);
	IO_STATUS_BLOCK Io = {};
	Out = INVALID_HANDLE_VALUE;
	return NtCreate()(&Out, Access | SYNCHRONIZE, &Attributes, &Io, nullptr,
		FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		Disposition, Options | FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT
			| FILE_OPEN_FOR_BACKUP_INTENT, nullptr, 0);
}

NTSTATUS RenameAt(HANDLE File, HANDLE TargetDir, const std::wstring& NewName,
	int Class, ULONG FlagsOrReplace)
{
	// FILE_RENAME_INFORMATION and its Ex form share one layout: a 4-byte
	// ReplaceIfExists/Flags slot, the root handle, the length, the name.
	struct FHeader { ULONG Flags; HANDLE Root; ULONG Length; };
	std::vector<unsigned char> Buffer(sizeof(FHeader) + NewName.size() * sizeof(wchar_t) + 8, 0);
	auto* Header = reinterpret_cast<FHeader*>(Buffer.data());
	Header->Flags = FlagsOrReplace;
	Header->Root = TargetDir;
	Header->Length = static_cast<ULONG>(NewName.size() * sizeof(wchar_t));
	std::memcpy(Buffer.data() + offsetof(FILE_RENAME_INFO, FileName),
		NewName.c_str(), Header->Length);
	IO_STATUS_BLOCK Io = {};
	return NtSetInfo()(File, &Io, Buffer.data(), static_cast<ULONG>(Buffer.size()), Class);
}

// --------------------------------------------------------------- identity

void Identity(FLine& Line, HANDLE Handle)
{
	FILE_ATTRIBUTE_TAG_INFO Tag = {};
	if (::GetFileInformationByHandleEx(Handle, FileAttributeTagInfo, &Tag, sizeof(Tag)))
		Line.Str("attributes", Hex(Tag.FileAttributes, 8)).Str("reparse_tag", Hex(Tag.ReparseTag, 8));
	else
		Line.Num("tag_info_win32", ::GetLastError());

	FILE_ID_INFO Id = {};
	if (::GetFileInformationByHandleEx(Handle, FileIdInfo, &Id, sizeof(Id)))
	{
		unsigned long long Low = 0, High = 0;
		std::memcpy(&Low, Id.FileId.Identifier, 8);
		std::memcpy(&High, Id.FileId.Identifier + 8, 8);
		Line.Str("volume_serial", Hex(Id.VolumeSerialNumber, 16))
			.Str("file_id", Hex(High, 16) + Hex(Low, 16).substr(2));
	}
	else
		Line.Num("file_id_info_win32", ::GetLastError());

	BY_HANDLE_FILE_INFORMATION Info = {};
	if (::GetFileInformationByHandle(Handle, &Info))
		Line.Str("handle_serial", Hex(Info.dwVolumeSerialNumber, 8))
			.Num("links", Info.nNumberOfLinks)
			.Str("index", Hex((static_cast<unsigned long long>(Info.nFileIndexHigh) << 32)
				| Info.nFileIndexLow, 16));

	FILE_BASIC_INFO Basic = {};
	if (::GetFileInformationByHandleEx(Handle, FileBasicInfo, &Basic, sizeof(Basic)))
		Line.Num("change_time", static_cast<unsigned long long>(Basic.ChangeTime.QuadPart));

	wchar_t Path[1024];
	for (const auto& [Name, Flags] : { std::pair<const char*, DWORD>{"final_dos", VOLUME_NAME_DOS},
			{"final_guid", VOLUME_NAME_GUID}, {"final_nt", VOLUME_NAME_NT} })
	{
		const DWORD Length = ::GetFinalPathNameByHandleW(Handle, Path, 1024,
			FILE_NAME_NORMALIZED | Flags);
		if (Length > 0 && Length < 1024) Line.Str(Name, std::wstring(Path, Length));
		else Line.Num((std::string(Name) + "_win32").c_str(), ::GetLastError());
	}

	wchar_t FsName[MAX_PATH] = {};
	DWORD Serial = 0, MaxComponent = 0, FsFlags = 0;
	if (::GetVolumeInformationByHandleW(Handle, nullptr, 0, &Serial, &MaxComponent,
			&FsFlags, FsName, MAX_PATH))
		Line.Str("fs", std::wstring(FsName)).Str("fs_serial", Hex(Serial, 8))
			.Str("fs_flags", Hex(FsFlags, 8));
	else
		Line.Num("volume_info_win32", ::GetLastError());
}

std::vector<std::wstring> Split(const std::wstring& Path)
{
	std::vector<std::wstring> Parts;
	std::wstring Current;
	for (const wchar_t C : Path)
	{
		if (C == L'\\' || C == L'/') { if (!Current.empty()) Parts.push_back(Current); Current.clear(); }
		else Current += C;
	}
	if (!Current.empty()) Parts.push_back(Current);
	return Parts;
}

int Walk(const std::wstring& Path)
{
	const std::vector<std::wstring> Parts = Split(Path);
	if (Parts.empty() || Parts[0].size() != 2 || Parts[0][1] != L':')
	{
		FLine("walk").Str("error", "give an absolute drive path such as Z:\\home\\deck").Emit();
		return 2;
	}
	// The drive root, opened absolutely as the game opens a walk's start.
	std::wstring Prefix = Parts[0] + L"\\";
	HANDLE Parent = INVALID_HANDLE_VALUE;
	NTSTATUS Status = OpenAt(nullptr, L"\\??\\" + Prefix, FILE_READ_ATTRIBUTES,
		FILE_OPEN, FILE_DIRECTORY_FILE, Parent);
	{
		FLine Line("walk-component");
		Line.Str("path", Prefix).Num("depth", 0).Bool("drive_root", true).Status(Status);
		if (Status >= 0) Identity(Line, Parent); else Line.Win32(NtToWin32(Status));
		Line.Emit();
	}
	if (Status < 0) return 1;
	for (size_t Index = 1; Index < Parts.size(); ++Index)
	{
		Prefix += (Index > 1 ? L"\\" : L"") + Parts[Index];
		HANDLE Child = INVALID_HANDLE_VALUE;
		Status = OpenAt(Parent, Parts[Index], FILE_READ_ATTRIBUTES, FILE_OPEN, 0, Child);
		FLine Line("walk-component");
		Line.Str("path", Prefix).Num("depth", Index).Bool("drive_root", false).Status(Status);
		if (Status >= 0) Identity(Line, Child); else Line.Win32(NtToWin32(Status));
		// The same prefix opened absolutely through Win32, for comparison.
		HANDLE Absolute = ::CreateFileW(Prefix.c_str(), FILE_READ_ATTRIBUTES,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
			FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
		if (Absolute == INVALID_HANDLE_VALUE) Line.Num("absolute_win32", ::GetLastError());
		else
		{
			FILE_ATTRIBUTE_TAG_INFO Tag = {};
			if (::GetFileInformationByHandleEx(Absolute, FileAttributeTagInfo, &Tag, sizeof(Tag)))
				Line.Str("absolute_attributes", Hex(Tag.FileAttributes, 8))
					.Str("absolute_reparse_tag", Hex(Tag.ReparseTag, 8));
			::CloseHandle(Absolute);
		}
		Line.Emit();
		::CloseHandle(Parent);
		if (Status < 0) return 1;
		Parent = Child;
	}
	::CloseHandle(Parent);
	return 0;
}

// ------------------------------------------------------------------ census

std::wstring Unique(const wchar_t* Stem)
{
	return std::wstring(Stem) + L"-" + std::to_wstring(::GetCurrentProcessId());
}

bool WriteSmall(HANDLE File)
{
	const char Bytes[] = "wot-path-probe";
	DWORD Written = 0;
	return ::WriteFile(File, Bytes, sizeof(Bytes), &Written, nullptr) != 0;
}

void CheckNt(const char* Name, NTSTATUS Status)
{
	FLine Line(Name);
	Line.Bool("ok", Status >= 0).Status(Status);
	if (Status < 0) Line.Win32(NtToWin32(Status));
	Line.Emit();
}

void CheckWin32(const char* Name, BOOL Ok)
{
	const DWORD Error = Ok ? 0 : ::GetLastError();
	FLine(Name).Bool("ok", Ok != 0).Win32(Error).Emit();
}

int LockHolder(const std::wstring& File, DWORD Milliseconds)
{
	HANDLE Lock = ::CreateFileW(File.c_str(), GENERIC_READ | GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
		FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	if (Lock == INVALID_HANDLE_VALUE) return 3;
	OVERLAPPED Range = {};
	// The running game's hold: shared, one byte at offset 0.
	if (!::LockFileEx(Lock, 0, 0, 1, 0, &Range)) return 4;
	HANDLE Marker = ::CreateFileW((File + L".held").c_str(), GENERIC_WRITE, 0, nullptr,
		CREATE_ALWAYS, 0, nullptr);
	if (Marker != INVALID_HANDLE_VALUE) ::CloseHandle(Marker);
	::Sleep(Milliseconds);
	::UnlockFileEx(Lock, 0, 1, 0, &Range);
	::CloseHandle(Lock);
	return 0;
}

void CensusLocks(const std::wstring& Scratch)
{
	const std::wstring LockPath = Scratch + L"\\" + Unique(L"session") + L".lock";
	wchar_t Self[MAX_PATH];
	::GetModuleFileNameW(nullptr, Self, MAX_PATH);
	std::wstring Command = L"\"" + std::wstring(Self) + L"\" lockholder \"" + LockPath + L"\" 4000";
	STARTUPINFOW Startup = { sizeof(Startup) };
	PROCESS_INFORMATION Process = {};
	if (!::CreateProcessW(nullptr, Command.data(), nullptr, nullptr, FALSE, 0, nullptr,
			nullptr, &Startup, &Process))
	{
		FLine("lock-cross-process").Bool("ok", false).Str("step", "spawn")
			.Win32(::GetLastError()).Emit();
		return;
	}
	bool bHeld = false;
	for (int Poll = 0; Poll < 60 && !bHeld; ++Poll)
	{
		bHeld = ::GetFileAttributesW((LockPath + L".held").c_str()) != INVALID_FILE_ATTRIBUTES;
		if (!bHeld) ::Sleep(50);
	}
	// The launcher's attempt: exclusive and fail-immediately while the game holds shared.
	HANDLE Lock = ::CreateFileW(LockPath.c_str(), GENERIC_READ | GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
		FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	OVERLAPPED Range = {};
	const BOOL Blocked = Lock != INVALID_HANDLE_VALUE && ::LockFileEx(Lock,
		LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &Range);
	const DWORD BlockedError = Blocked ? 0 : ::GetLastError();
	if (Blocked) ::UnlockFileEx(Lock, 0, 1, 0, &Range);
	FLine("lock-exclusive-while-shared-held").Bool("child_held", bHeld)
		.Bool("ok", !Blocked && BlockedError == ERROR_LOCK_VIOLATION).Win32(BlockedError)
		.Str("expect", "win32 33 (ERROR_LOCK_VIOLATION)").Emit();
	::WaitForSingleObject(Process.hProcess, 10000);
	DWORD ChildExit = 0;
	::GetExitCodeProcess(Process.hProcess, &ChildExit);
	::CloseHandle(Process.hThread);
	::CloseHandle(Process.hProcess);
	OVERLAPPED After = {};
	const BOOL Acquired = Lock != INVALID_HANDLE_VALUE && ::LockFileEx(Lock,
		LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &After);
	FLine("lock-exclusive-after-release").Num("child_exit", ChildExit)
		.Bool("ok", Acquired != 0).Win32(Acquired ? 0 : ::GetLastError()).Emit();
	if (Acquired) ::UnlockFileEx(Lock, 0, 1, 0, &After);
	if (Lock != INVALID_HANDLE_VALUE) ::CloseHandle(Lock);
	::DeleteFileW((LockPath + L".held").c_str());
	::DeleteFileW(LockPath.c_str());
}

int Census(const std::wstring& Scratch)
{
	HANDLE Dir = ::CreateFileW(Scratch.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES
		| FILE_TRAVERSE | FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY | SYNCHRONIZE,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
		FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	if (Dir == INVALID_HANDLE_VALUE)
	{
		FLine("census-open-scratch").Bool("ok", false).Win32(::GetLastError())
			.Str("path", Scratch).Emit();
		return 2;
	}
	{
		FLine Line("census-scratch-identity");
		Line.Str("path", Scratch);
		Identity(Line, Dir);
		Line.Emit();
	}

	// Relative create and identity of a new file.
	const std::wstring NameA = Unique(L"a"), NameB = Unique(L"b"), NameC = Unique(L"c");
	HANDLE A = INVALID_HANDLE_VALUE;
	NTSTATUS Status = OpenAt(Dir, NameA, GENERIC_READ | GENERIC_WRITE | DELETE,
		FILE_CREATE, FILE_NON_DIRECTORY_FILE, A);
	CheckNt("nt-create-relative", Status);
	if (Status >= 0)
	{
		WriteSmall(A);
		FLine Line("file-identity");
		Identity(Line, A);
		Line.Emit();
		FILE_BASIC_INFO Before = {}, After = {};
		::GetFileInformationByHandleEx(A, FileBasicInfo, &Before, sizeof(Before));
		::Sleep(1100);
		WriteSmall(A);
		::FlushFileBuffers(A);
		::GetFileInformationByHandleEx(A, FileBasicInfo, &After, sizeof(After));
		FLine("change-time-moves-on-write")
			.Bool("ok", After.ChangeTime.QuadPart > Before.ChangeTime.QuadPart).Emit();
	}

	// Enumeration with ids, from the start (restart class first, as the game does).
	{
		std::vector<unsigned char> Buffer(64 * 1024);
		const BOOL Ok = ::GetFileInformationByHandleEx(Dir, FileIdBothDirectoryRestartInfo,
			Buffer.data(), static_cast<DWORD>(Buffer.size()));
		const DWORD Error = Ok ? 0 : ::GetLastError();
		bool bFound = false;
		unsigned long long Entries = 0;
		if (Ok)
		{
			auto* Entry = reinterpret_cast<FILE_ID_BOTH_DIR_INFO*>(Buffer.data());
			for (;;)
			{
				++Entries;
				if (std::wstring(Entry->FileName, Entry->FileNameLength / sizeof(wchar_t)) == NameA)
					bFound = true;
				if (Entry->NextEntryOffset == 0) break;
				Entry = reinterpret_cast<FILE_ID_BOTH_DIR_INFO*>(
					reinterpret_cast<unsigned char*>(Entry) + Entry->NextEntryOffset);
			}
		}
		FLine("enumerate-id-both-restart").Bool("ok", Ok != 0).Win32(Error)
			.Num("entries", Entries).Bool("found_created_file", bFound).Emit();
	}

	// Sharing: an open asking for DELETE while another handle withholds
	// FILE_SHARE_DELETE (the running game's paks). A fresh file, no other handles.
	{
		const std::wstring Shared = Scratch + L"\\" + Unique(L"share");
		HANDLE Seed = ::CreateFileW(Shared.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
		if (Seed != INVALID_HANDLE_VALUE) { WriteSmall(Seed); ::CloseHandle(Seed); }
		HANDLE NoShareDelete = ::CreateFileW(Shared.c_str(), GENERIC_READ,
			FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
		const DWORD HolderError = NoShareDelete == INVALID_HANDLE_VALUE ? ::GetLastError() : 0;
		HANDLE WantsDelete = ::CreateFileW(Shared.c_str(), DELETE,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0,
			nullptr);
		const DWORD Error = WantsDelete == INVALID_HANDLE_VALUE ? ::GetLastError() : 0;
		FLine("share-delete-withheld").Bool("holder_opened", NoShareDelete != INVALID_HANDLE_VALUE)
			.Num("holder_win32", HolderError)
			.Bool("ok", NoShareDelete != INVALID_HANDLE_VALUE && WantsDelete == INVALID_HANDLE_VALUE
				&& Error == ERROR_SHARING_VIOLATION)
			.Win32(Error).Str("expect", "win32 32 (ERROR_SHARING_VIOLATION)").Emit();
		if (WantsDelete != INVALID_HANDLE_VALUE) ::CloseHandle(WantsDelete);
		if (NoShareDelete != INVALID_HANDLE_VALUE) ::CloseHandle(NoShareDelete);
		::DeleteFileW(Shared.c_str());
	}

	// Plain rename a -> b, relative to the directory handle.
	if (A != INVALID_HANDLE_VALUE)
		CheckNt("rename-relative", RenameAt(A, Dir, NameB, ClassFileRename, 0));

	// Replace-rename c over b while b is still open (POSIX semantics, as the
	// game's atomic saves do when a reader transiently holds the target).
	HANDLE C = INVALID_HANDLE_VALUE;
	if (OpenAt(Dir, NameC, GENERIC_READ | GENERIC_WRITE | DELETE, FILE_CREATE,
			FILE_NON_DIRECTORY_FILE, C) >= 0)
	{
		WriteSmall(C);
		CheckNt("rename-ex-posix-replace-held-target",
			RenameAt(C, Dir, NameB, ClassFileRenameEx, RenameReplaceIfExists | RenamePosixSemantics));
		::CloseHandle(C);
	}
	if (A != INVALID_HANDLE_VALUE) ::CloseHandle(A);

	// The game's atomic saves (update state, markers, preferences, notices)
	// replace a target nobody holds; a concurrent reader is the rare case
	// above. Measure both rename classes, unheld and held.
	struct FReplaceCase { const char* Check; int Class; ULONG Flags; bool bHoldTarget; };
	for (const FReplaceCase& Case : {
			FReplaceCase{"rename-ex-posix-replace-unheld-target", ClassFileRenameEx,
				RenameReplaceIfExists | RenamePosixSemantics, false},
			FReplaceCase{"rename-replace-if-exists-unheld-target", ClassFileRename, 1, false},
			FReplaceCase{"rename-replace-if-exists-held-target", ClassFileRename, 1, true}})
	{
		const std::wstring Target = Unique(L"t"), Source = Unique(L"s");
		HANDLE TargetHandle = INVALID_HANDLE_VALUE, SourceHandle = INVALID_HANDLE_VALUE;
		if (OpenAt(Dir, Target, GENERIC_READ | GENERIC_WRITE | DELETE, FILE_CREATE,
				FILE_NON_DIRECTORY_FILE, TargetHandle) < 0)
			continue;
		WriteSmall(TargetHandle);
		if (!Case.bHoldTarget) { ::CloseHandle(TargetHandle); TargetHandle = INVALID_HANDLE_VALUE; }
		if (OpenAt(Dir, Source, GENERIC_READ | GENERIC_WRITE | DELETE, FILE_CREATE,
				FILE_NON_DIRECTORY_FILE, SourceHandle) >= 0)
		{
			WriteSmall(SourceHandle);
			const NTSTATUS Replaced = RenameAt(SourceHandle, Dir, Target, Case.Class, Case.Flags);
			FLine(Case.Check).Bool("ok", Replaced >= 0).Status(Replaced)
				.Win32(Replaced < 0 ? NtToWin32(Replaced) : 0).Emit();
			::CloseHandle(SourceHandle);
			::DeleteFileW((Scratch + L"\\" + Source).c_str());
		}
		if (TargetHandle != INVALID_HANDLE_VALUE) ::CloseHandle(TargetHandle);
		::DeleteFileW((Scratch + L"\\" + Target).c_str());
	}

	// Directory rename while a file inside it is open (Windows refuses; the
	// update swap and cache publish rely on knowing which).
	{
		const std::wstring DirName = Unique(L"d"), DirMoved = Unique(L"e");
		HANDLE Sub = INVALID_HANDLE_VALUE;
		if (OpenAt(Dir, DirName, GENERIC_READ | DELETE, FILE_CREATE, FILE_DIRECTORY_FILE, Sub) >= 0)
		{
			HANDLE Inner = INVALID_HANDLE_VALUE;
			OpenAt(Sub, L"inner", GENERIC_READ | GENERIC_WRITE, FILE_CREATE,
				FILE_NON_DIRECTORY_FILE, Inner);
			const NTSTATUS Moved = RenameAt(Sub, Dir, DirMoved, ClassFileRename, 0);
			FLine Line("rename-directory-with-open-child");
			Line.Bool("renamed", Moved >= 0).Status(Moved).Win32(Moved < 0 ? NtToWin32(Moved) : 0)
				.Str("windows", "refuses with 0xc0000022 / win32 5").Emit();
			if (Inner != INVALID_HANDLE_VALUE) ::CloseHandle(Inner);
			const std::wstring Final = Scratch + L"\\" + (Moved >= 0 ? DirMoved : DirName);
			::CloseHandle(Sub);
			::DeleteFileW((Final + L"\\inner").c_str());
			::RemoveDirectoryW(Final.c_str());
		}
	}

	// POSIX delete of a read-only file another handle still holds.
	{
		const std::wstring Name = Unique(L"ro");
		HANDLE Held = INVALID_HANDLE_VALUE;
		if (OpenAt(Dir, Name, GENERIC_READ | GENERIC_WRITE | DELETE, FILE_CREATE,
				FILE_NON_DIRECTORY_FILE, Held) >= 0)
		{
			WriteSmall(Held);
			::SetFileAttributesW((Scratch + L"\\" + Name).c_str(), FILE_ATTRIBUTE_READONLY);
			HANDLE Deleter = INVALID_HANDLE_VALUE;
			OpenAt(Dir, Name, DELETE, FILE_OPEN, FILE_NON_DIRECTORY_FILE, Deleter);
			ULONG Flags = DispositionDelete | DispositionPosixSemantics | DispositionIgnoreReadOnly;
			IO_STATUS_BLOCK Io = {};
			const NTSTATUS Deleted = Deleter == INVALID_HANDLE_VALUE ? StatusUnsuccessful
				: NtSetInfo()(Deleter, &Io, &Flags, sizeof(Flags), ClassFileDispositionEx);
			if (Deleter != INVALID_HANDLE_VALUE) ::CloseHandle(Deleter);
			const bool bGone = ::GetFileAttributesW((Scratch + L"\\" + Name).c_str())
				== INVALID_FILE_ATTRIBUTES;
			FLine("disposition-ex-posix-readonly-held").Bool("ok", Deleted >= 0 && bGone)
				.Bool("name_gone_while_held", bGone).Status(Deleted)
				.Win32(Deleted < 0 ? NtToWin32(Deleted) : 0).Emit();
			::CloseHandle(Held);
			::SetFileAttributesW((Scratch + L"\\" + Name).c_str(), FILE_ATTRIBUTE_NORMAL);
			::DeleteFileW((Scratch + L"\\" + Name).c_str());
		}
	}

	// Delete-on-close probe file (the writable-folder check).
	{
		const std::wstring Name = Unique(L"doc");
		HANDLE Probe = INVALID_HANDLE_VALUE;
		const NTSTATUS Created = OpenAt(Dir, Name, GENERIC_WRITE | DELETE, FILE_CREATE,
			FILE_NON_DIRECTORY_FILE | FILE_DELETE_ON_CLOSE, Probe);
		if (Created >= 0) ::CloseHandle(Probe);
		const bool bGone = ::GetFileAttributesW((Scratch + L"\\" + Name).c_str())
			== INVALID_FILE_ATTRIBUTES;
		FLine("create-delete-on-close").Bool("ok", Created >= 0 && bGone)
			.Bool("gone_after_close", bGone).Status(Created).Emit();
	}

	// Volume queries: free space by DOS root and by volume GUID, and the WOF probe.
	{
		wchar_t Root[MAX_PATH] = {};
		const BOOL HaveRoot = ::GetVolumePathNameW(Scratch.c_str(), Root, MAX_PATH);
		ULARGE_INTEGER Free = {};
		const BOOL FreeOk = HaveRoot && ::GetDiskFreeSpaceExW(Root, &Free, nullptr, nullptr);
		FLine("free-space-dos-root").Bool("ok", FreeOk != 0).Str("root", std::wstring(Root))
			.Num("bytes", Free.QuadPart).Win32(FreeOk ? 0 : ::GetLastError()).Emit();
		wchar_t Guid[1024];
		const DWORD Length = ::GetFinalPathNameByHandleW(Dir, Guid, 1024,
			FILE_NAME_NORMALIZED | VOLUME_NAME_GUID);
		std::wstring GuidPath = Length > 0 && Length < 1024 ? std::wstring(Guid, Length) : L"";
		const size_t End = GuidPath.find(L"}\\");
		const bool bGuidShape = GuidPath.rfind(L"\\\\?\\Volume{", 0) == 0 && End != std::wstring::npos;
		ULARGE_INTEGER GuidFree = {};
		const BOOL GuidOk = bGuidShape && ::GetDiskFreeSpaceExW(
			GuidPath.substr(0, End + 2).c_str(), &GuidFree, nullptr, nullptr);
		FLine("free-space-volume-guid").Bool("ok", GuidOk != 0).Bool("guid_shape", bGuidShape)
			.Str("final_guid", GuidPath).Win32(GuidOk ? 0 : ::GetLastError()).Emit();
		USHORT Compression = 0;
		DWORD Returned = 0;
		const BOOL Wof = ::DeviceIoControl(Dir, FSCTL_GET_COMPRESSION, nullptr, 0,
			&Compression, sizeof(Compression), &Returned, nullptr);
		FLine("fsctl-get-compression").Bool("ok", Wof != 0).Num("format", Compression)
			.Win32(Wof ? 0 : ::GetLastError())
			.Str("tolerated", "win32 1 (INVALID_FUNCTION) or 50 (NOT_SUPPORTED)").Emit();
	}

	CensusLocks(Scratch);
	const std::wstring Leftover = Scratch + L"\\" + NameB;
	::DeleteFileW(Leftover.c_str());
	::CloseHandle(Dir);
	return 0;
}

// ---------------------------------------------------------------- env/http

int Env()
{
	FLine Line("env");
	const auto WineVersion = reinterpret_cast<FWineGetVersion>(
		::GetProcAddress(Ntdll(), "wine_get_version"));
	Line.Bool("wine", WineVersion != nullptr);
	if (WineVersion) Line.Str("wine_version", std::string(WineVersion()));
	const auto HostVersion = reinterpret_cast<FWineGetHostVersion>(
		::GetProcAddress(Ntdll(), "wine_get_host_version"));
	if (HostVersion)
	{
		const char* System = nullptr; const char* Release = nullptr;
		HostVersion(&System, &Release);
		Line.Str("host_system", std::string(System ? System : ""))
			.Str("host_release", std::string(Release ? Release : ""));
	}
	PWSTR Downloads = nullptr;
	if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Downloads, KF_FLAG_DEFAULT, nullptr, &Downloads)))
	{
		Line.Str("known_folder_downloads", std::wstring(Downloads));
		::CoTaskMemFree(Downloads);
	}
	const auto DosName = reinterpret_cast<FWineGetDosFileName>(
		::GetProcAddress(::GetModuleHandleW(L"kernel32.dll"), "wine_get_dos_file_name"));
	Line.Bool("wine_get_dos_file_name", DosName != nullptr);
	char Home[1024] = {};
	if (::GetEnvironmentVariableA("HOME", Home, sizeof(Home)) > 0)
	{
		Line.Str("env_home", std::string(Home));
		if (DosName)
		{
			for (const char* Leaf : { "/Downloads", "/Desktop" })
			{
				LPWSTR Mapped = DosName((std::string(Home) + Leaf).c_str());
				Line.Str((std::string("dos") + (Leaf[1] == 'D' && Leaf[2] == 'o'
					? "_downloads" : "_desktop")).c_str(), Mapped ? std::wstring(Mapped) : L"");
				if (Mapped) ::HeapFree(::GetProcessHeap(), 0, Mapped);
			}
			LPWSTR Media = DosName("/run/media");
			Line.Str("dos_run_media", Media ? std::wstring(Media) : L"");
			if (Media) ::HeapFree(::GetProcessHeap(), 0, Media);
		}
	}
	Line.Emit();
	return 0;
}

int Http(const std::wstring& Url)
{
	URL_COMPONENTS Parts = { sizeof(Parts) };
	wchar_t Host[256] = {}, Path[2048] = {};
	Parts.lpszHostName = Host; Parts.dwHostNameLength = 256;
	Parts.lpszUrlPath = Path; Parts.dwUrlPathLength = 2048;
	if (!::WinHttpCrackUrl(Url.c_str(), 0, 0, &Parts))
	{
		FLine("http-crack-url").Bool("ok", false).Win32(::GetLastError()).Emit();
		return 2;
	}
	// The same access type, options and order as the game's update hop.
	HINTERNET Session = ::WinHttpOpen(L"WoT-Updater/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
		WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	CheckWin32("http-open", Session != nullptr);
	if (!Session) return 1;
	DWORD Attempts = 1;
	CheckWin32("http-option-connect-retries",
		::WinHttpSetOption(Session, WINHTTP_OPTION_CONNECT_RETRIES, &Attempts, sizeof(Attempts)));
	CheckWin32("http-set-timeouts", ::WinHttpSetTimeouts(Session, 5000, 5000, 5000, 5000));
	HINTERNET Connection = ::WinHttpConnect(Session, Host, Parts.nPort, 0);
	CheckWin32("http-connect", Connection != nullptr);
	HINTERNET Request = Connection ? ::WinHttpOpenRequest(Connection, L"GET", Path, nullptr,
		WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
		Parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : nullptr;
	CheckWin32("http-open-request", Request != nullptr);
	if (Request)
	{
		DWORD Redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
		DWORD Disabled = WINHTTP_DISABLE_AUTHENTICATION | WINHTTP_DISABLE_COOKIES;
		DWORD Autologon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
		CheckWin32("http-option-redirect-policy-never",
			::WinHttpSetOption(Request, WINHTTP_OPTION_REDIRECT_POLICY, &Redirect, sizeof(Redirect)));
		CheckWin32("http-option-disable-feature",
			::WinHttpSetOption(Request, WINHTTP_OPTION_DISABLE_FEATURE, &Disabled, sizeof(Disabled)));
		CheckWin32("http-option-autologon-policy",
			::WinHttpSetOption(Request, WINHTTP_OPTION_AUTOLOGON_POLICY, &Autologon, sizeof(Autologon)));
		const BOOL Sent = ::WinHttpSendRequest(Request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
			WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && ::WinHttpReceiveResponse(Request, nullptr);
		DWORD Code = 0, Size = sizeof(Code);
		wchar_t Location[2048] = {};
		DWORD LocationSize = sizeof(Location);
		if (Sent)
		{
			::WinHttpQueryHeaders(Request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
				WINHTTP_HEADER_NAME_BY_INDEX, &Code, &Size, WINHTTP_NO_HEADER_INDEX);
			::WinHttpQueryHeaders(Request, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
				Location, &LocationSize, WINHTTP_NO_HEADER_INDEX);
		}
		FLine("http-first-hop").Bool("ok", Sent != 0).Win32(Sent ? 0 : ::GetLastError())
			.Num("status", Code).Str("location", std::wstring(Location))
			.Str("expect", "a 3xx with a Location: the redirect is not followed").Emit();
		::WinHttpCloseHandle(Request);
	}
	if (Connection) ::WinHttpCloseHandle(Connection);
	::WinHttpCloseHandle(Session);
	return 0;
}
} // namespace

int wmain(int Argc, wchar_t** Argv)
{
	if (!NtCreate() || !NtSetInfo())
	{
		FLine("ntdll").Bool("ok", false).Str("error", "NtCreateFile/NtSetInformationFile missing").Emit();
		return 2;
	}
	const std::wstring Mode = Argc > 1 ? Argv[1] : L"";
	if (Mode == L"env") return Env();
	if (Mode == L"walk" && Argc > 2) return Walk(Argv[2]);
	if (Mode == L"census" && Argc > 2) return Census(Argv[2]);
	if (Mode == L"http")
		return Http(Argc > 2 ? Argv[2]
			: L"https://github.com/aleiby/wot/releases/latest/download/WoT-Win64-announcement-v1.json");
	if (Mode == L"lockholder" && Argc > 3)
		return LockHolder(Argv[2], static_cast<DWORD>(std::wcstoul(Argv[3], nullptr, 10)));
	std::fprintf(stderr, "usage: wot_path_probe env | walk <path> | census <scratch dir>"
		" | http [url]\n");
	return 2;
}
