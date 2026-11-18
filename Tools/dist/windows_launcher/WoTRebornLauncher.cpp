#define WIN32_LEAN_AND_MEAN
#include <windows.h>

extern "C" void* __cdecl memset(void* Destination, int Value, size_t Count)
{
    unsigned char* Out = static_cast<unsigned char*>(Destination);
    while (Count--)
    {
        *Out++ = static_cast<unsigned char>(Value);
    }
    return Destination;
}

namespace
{
constexpr wchar_t ProductTitle[] = L"WoT Reborn";
#if defined(WOT_LAUNCHER_CONFIGURATION_TEST) && \
    defined(WOT_LAUNCHER_CONFIGURATION_SHIPPING)
#error The launcher cannot target both guarded configurations.
#elif defined(WOT_LAUNCHER_CONFIGURATION_TEST)
constexpr wchar_t ChildRelative[] =
    L"Game\\WoT\\Binaries\\Win64\\WoT-Win64-Test.exe";
constexpr wchar_t MissingChildMessage[] =
    L"WoT Reborn couldn't find Game\\WoT\\Binaries\\Win64\\WoT-Win64-Test.exe. Re-download the complete WoT Reborn folder and start it again.";
constexpr wchar_t StartChildMessage[] =
    L"WoT Reborn couldn't start Game\\WoT\\Binaries\\Win64\\WoT-Win64-Test.exe.";
#elif defined(WOT_LAUNCHER_CONFIGURATION_SHIPPING)
constexpr wchar_t ChildRelative[] =
    L"Game\\WoT\\Binaries\\Win64\\WoT-Win64-Shipping.exe";
constexpr wchar_t MissingChildMessage[] =
    L"WoT Reborn couldn't find Game\\WoT\\Binaries\\Win64\\WoT-Win64-Shipping.exe. Re-download the complete WoT Reborn folder and start it again.";
constexpr wchar_t StartChildMessage[] =
    L"WoT Reborn couldn't start Game\\WoT\\Binaries\\Win64\\WoT-Win64-Shipping.exe.";
#else
#error The launcher must be compiled for exactly one guarded configuration.
#endif

SIZE_T Length(const wchar_t* Text)
{
    SIZE_T Result = 0;
    while (Text && Text[Result])
    {
        ++Result;
    }
    return Result;
}

void Copy(wchar_t*& Out, const wchar_t* Text)
{
    while (Text && *Text)
    {
        *Out++ = *Text++;
    }
}

wchar_t* Allocate(SIZE_T Characters)
{
    return static_cast<wchar_t*>(HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, Characters * sizeof(wchar_t)));
}

wchar_t* ExecutablePath()
{
    for (DWORD Capacity = 512; Capacity <= 32768; Capacity *= 2)
    {
        wchar_t* Buffer = Allocate(Capacity);
        if (!Buffer)
        {
            return nullptr;
        }
        const DWORD Count = GetModuleFileNameW(nullptr, Buffer, Capacity);
        if (Count > 0 && Count < Capacity - 1)
        {
            Buffer[Count] = 0;
            return Buffer;
        }
        HeapFree(GetProcessHeap(), 0, Buffer);
        if (Count == 0)
        {
            return nullptr;
        }
    }
    return nullptr;
}

const wchar_t* RawArgumentSuffix()
{
    const wchar_t* Cursor = GetCommandLineW();
    while (*Cursor == L' ' || *Cursor == L'\t')
    {
        ++Cursor;
    }
    if (*Cursor == L'"')
    {
        ++Cursor;
        while (*Cursor && *Cursor != L'"')
        {
            ++Cursor;
        }
        if (*Cursor == L'"')
        {
            ++Cursor;
        }
    }
    else
    {
        while (*Cursor && *Cursor != L' ' && *Cursor != L'\t')
        {
            ++Cursor;
        }
    }
    return Cursor;
}

DWORD WriteAll(HANDLE File, const void* Data, DWORD Bytes)
{
    const BYTE* Cursor = static_cast<const BYTE*>(Data);
    while (Bytes)
    {
        DWORD Written = 0;
        if (!WriteFile(File, Cursor, Bytes, &Written, nullptr) || Written == 0)
        {
            return ERROR_WRITE_FAULT;
        }
        Cursor += Written;
        Bytes -= Written;
    }
    return ERROR_SUCCESS;
}

void ReportError(const wchar_t* Message)
{
#if defined(WOT_LAUNCHER_TEST)
    wchar_t* ErrorPath = Allocate(32768);
    if (!ErrorPath)
    {
        MessageBoxW(nullptr, Message, ProductTitle,
                    MB_OK | MB_ICONERROR | MB_TASKMODAL);
        return;
    }
    const DWORD Count = GetEnvironmentVariableW(
        L"WOT_LAUNCHER_TEST_ERROR_FILE", ErrorPath, 32768);
    if (Count > 0 && Count < 32768)
    {
        HANDLE File = CreateFileW(ErrorPath, GENERIC_WRITE, FILE_SHARE_READ,
            nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (File != INVALID_HANDLE_VALUE)
        {
            const WORD Bom = 0xfeff;
            WriteAll(File, &Bom, sizeof(Bom));
            WriteAll(File, Message,
                static_cast<DWORD>(Length(Message) * sizeof(wchar_t)));
            CloseHandle(File);
            HeapFree(GetProcessHeap(), 0, ErrorPath);
            return;
        }
    }
    HeapFree(GetProcessHeap(), 0, ErrorPath);
#endif
    MessageBoxW(nullptr, Message, ProductTitle, MB_OK | MB_ICONERROR | MB_TASKMODAL);
}

wchar_t* Join(const wchar_t* Root, const wchar_t* Relative)
{
    const SIZE_T RootLength = Length(Root);
    wchar_t* Result = Allocate(RootLength + 1 + Length(Relative) + 1);
    if (!Result)
    {
        return nullptr;
    }
    wchar_t* Out = Result;
    Copy(Out, Root);
    *Out++ = L'\\';
    Copy(Out, Relative);
    *Out = 0;
    return Result;
}

bool PrepareWritableData(const wchar_t* InstallRoot)
{
    wchar_t* Data = Join(InstallRoot, L"Data");
    if (!Data)
    {
        return false;
    }
    if (!CreateDirectoryW(Data, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
    {
        // The exact diagnostic below deliberately handles creation and probing.
    }
    const DWORD Attributes = GetFileAttributesW(Data);
    bool Writable = Attributes != INVALID_FILE_ATTRIBUTES
        && (Attributes & FILE_ATTRIBUTE_DIRECTORY) != 0
        && (Attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
    wchar_t Leaf[96] = {};
    if (Writable)
    {
        wsprintfW(Leaf, L".wot-write-probe-%08lx-%08lx.tmp",
            GetCurrentProcessId(), GetTickCount());
        wchar_t* Probe = Join(Data, Leaf);
        HANDLE File = Probe ? CreateFileW(Probe, GENERIC_WRITE, 0, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr) : INVALID_HANDLE_VALUE;
        Writable = File != INVALID_HANDLE_VALUE;
        if (File != INVALID_HANDLE_VALUE)
        {
            CloseHandle(File);
            Writable = DeleteFileW(Probe) != 0;
        }
        if (Probe)
        {
            HeapFree(GetProcessHeap(), 0, Probe);
        }
    }
    if (!Writable)
    {
        constexpr wchar_t Prefix[] = L"WoT Reborn can't save its data in ";
        constexpr wchar_t Suffix[] = L". Move the WoT Reborn folder to a place you can write to, such as your user folder, and start it again.";
        wchar_t* Message = Allocate(Length(Prefix) + Length(Data) + Length(Suffix) + 1);
        if (Message)
        {
            wchar_t* Out = Message;
            Copy(Out, Prefix);
            Copy(Out, Data);
            Copy(Out, Suffix);
            *Out = 0;
            ReportError(Message);
            HeapFree(GetProcessHeap(), 0, Message);
        }
    }
    HeapFree(GetProcessHeap(), 0, Data);
    return Writable;
}

constexpr wchar_t UpdateMessage[] = L"WoT Reborn couldn't install the update. Close other copies, check free space and folder permissions, then start it again. Your current version will open.";
constexpr wchar_t RestoreMessage[] = L"WoT Reborn couldn't restore the previous version. Close every copy and try again. If it still fails, re-download WoT Reborn and keep your Data folder.";
constexpr wchar_t RecordMessage[] = L"The previous version is restored, but the update record could not be saved. Re-download WoT Reborn and keep your Data folder.";

struct Path
{
    wchar_t* Value;
    Path(const wchar_t* Root, const wchar_t* Leaf) : Value(Join(Root, Leaf)) {}
    ~Path() { if (Value) HeapFree(GetProcessHeap(), 0, Value); }
    operator const wchar_t*() const { return Value; }
};

bool Exists(const wchar_t* Name)
{
    return Name && GetFileAttributesW(Name) != INVALID_FILE_ATTRIBUTES;
}

bool Directory(const wchar_t* Name)
{
    const DWORD Attributes = Name ? GetFileAttributesW(Name) : INVALID_FILE_ATTRIBUTES;
    return Attributes != INVALID_FILE_ATTRIBUTES
        && (Attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
            == FILE_ATTRIBUTE_DIRECTORY;
}

bool EnsureDirectory(const wchar_t* Name)
{
    return Name && (CreateDirectoryW(Name, nullptr) || GetLastError() == ERROR_ALREADY_EXISTS)
        && Directory(Name);
}

bool RemoveFile(const wchar_t* Name)
{
    return DeleteFileW(Name) || GetLastError() == ERROR_FILE_NOT_FOUND;
}

// Only disposable workspace children reach this function. Never walk links.
bool RemoveTree(const wchar_t* Name)
{
    if (!Exists(Name)) return true;
    if (!Directory(Name)) return false;
    Path Pattern(Name, L"*");
    WIN32_FIND_DATAW Entry = {};
    HANDLE Find = FindFirstFileW(Pattern, &Entry);
    if (Find == INVALID_HANDLE_VALUE) return false;
    bool Removed = true;
    do
    {
        if (Entry.cFileName[0] == L'.' && (!Entry.cFileName[1]
            || (Entry.cFileName[1] == L'.' && !Entry.cFileName[2]))) continue;
        Path Child(Name, Entry.cFileName);
        if (Entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            Removed = false;
        else if (Entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            Removed = RemoveTree(Child);
        else
            Removed = DeleteFileW(Child) != 0;
        if (!Removed) break;
    } while (FindNextFileW(Find, &Entry));
    if (Removed && GetLastError() != ERROR_NO_MORE_FILES) Removed = false;
    FindClose(Find);
    return Removed && RemoveDirectoryW(Name);
}

enum class TrialState { None, Pending, Started };
struct Marker { unsigned long long Sequence = 0; TrialState State = TrialState::None; };

// Shared format: positive int64 decimal, optional exact trial state, LF, no BOM.
bool ReadMarker(const wchar_t* Name, bool Trial, Marker& Out)
{
    HANDLE File = CreateFileW(Name, GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (File == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION Info = {};
    char Bytes[64] = {};
    DWORD Count = 0;
    const bool Read = GetFileInformationByHandle(File, &Info)
        && !(Info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))
        && Info.nNumberOfLinks == 1 && Info.nFileSizeHigh == 0 && Info.nFileSizeLow < 64
        && ReadFile(File, Bytes, sizeof(Bytes), &Count, nullptr);
    CloseHandle(File);
    if (!Read || Count < 2 || Bytes[0] < '1' || Bytes[0] > '9') return false;
    DWORD Index = 0;
    unsigned long long Sequence = 0;
    while (Index < Count && Bytes[Index] >= '0' && Bytes[Index] <= '9')
    {
        const unsigned Digit = Bytes[Index++] - '0';
        if (Sequence > (9223372036854775807ull - Digit) / 10) return false;
        Sequence = Sequence * 10 + Digit;
    }
    TrialState State = TrialState::None;
    if (Trial)
    {
        if (Index >= Count || Bytes[Index++] != ' ') return false;
        const char* Expected = Index < Count && Bytes[Index] == 'p' ? "pending" : "started";
        State = *Expected == 'p' ? TrialState::Pending : TrialState::Started;
        while (*Expected) if (Index >= Count || Bytes[Index++] != *Expected++) return false;
    }
    if (Index + 1 != Count || Bytes[Index] != '\n') return false;
    Out.Sequence = Sequence;
    Out.State = State;
    return true;
}

bool WriteMarker(const wchar_t* Name, unsigned long long Sequence, bool Pending)
{
    char Bytes[64] = {};
    char Reverse[20] = {};
    DWORD Digits = 0, Count = 0;
    do { Reverse[Digits++] = static_cast<char>('0' + Sequence % 10); Sequence /= 10; } while (Sequence);
    while (Digits) Bytes[Count++] = Reverse[--Digits];
    if (Pending) for (const char* State = " pending"; *State; ++State) Bytes[Count++] = *State;
    Bytes[Count++] = '\n';
    // Write a sibling temporary and rename it over the marker, so a full disk,
    // crash or power loss leaves either the old marker or the complete new one.
    const SIZE_T NameLength = Length(Name);
    wchar_t* Temp = Allocate(NameLength + 5);
    if (!Temp) return false;
    wchar_t* Cursor = Temp;
    Copy(Cursor, Name);
    Copy(Cursor, L".tmp");
    HANDLE File = CreateFileW(Temp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    bool Written = File != INVALID_HANDLE_VALUE;
    if (Written)
    {
        Written = WriteAll(File, Bytes, Count) == ERROR_SUCCESS
            && FlushFileBuffers(File);
        CloseHandle(File);
    }
    Written = Written && MoveFileExW(Temp, Name,
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!Written) DeleteFileW(Temp);
    HeapFree(GetProcessHeap(), 0, Temp);
    return Written;
}

// LoadRunning uses this same bounded, canonical release-identity stamp.
// Missing/invalid/unstamped builds have sequence zero. No JSON library or CRT.
unsigned long long RunningSequence(const wchar_t* Live)
{
    Path Name(Live, L"WoT\\Build\\WoTRelease\\release-identity.json");
    HANDLE File = CreateFileW(Name, GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (File == INVALID_HANDLE_VALUE) return 0;
    BY_HANDLE_FILE_INFORMATION Info = {};
    static char Bytes[4097] = {};
    DWORD Count = 0;
    const bool Read = GetFileInformationByHandle(File, &Info)
        && !(Info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))
        && Info.nFileSizeHigh == 0 && Info.nFileSizeLow <= 4096
        && ReadFile(File, Bytes, 4096, &Count, nullptr) && Count == Info.nFileSizeLow;
    CloseHandle(File);
    if (!Read) return 0;
    DWORD Index = 0;
    auto Take = [&Index, Count](const char* Text)
    {
        while (*Text)
            if (Index >= Count || Bytes[Index++] != *Text++) return false;
        return true;
    };
    if (!Take("{\"format\":\"wot-release-identity-v1\",\"release_commit\":\"")) return 0;
    const DWORD CommitStart = Index;
    while (Index < Count && Bytes[Index] != '"')
    {
        // Release stamps contain the plain git commit, never escaped strings.
        if (Bytes[Index] < 0x20 || Bytes[Index] == '\\') return 0;
        ++Index;
    }
    if (Index == CommitStart || !Take("\",\"release_sequence\":")) return 0;
    if (Index >= Count || Bytes[Index] < '0' || Bytes[Index] > '9') return 0;
    const bool LeadingZero = Bytes[Index] == '0';
    const DWORD SequenceStart = Index;
    unsigned long long Sequence = 0;
    while (Index < Count && Bytes[Index] >= '0' && Bytes[Index] <= '9')
    {
        const unsigned Digit = Bytes[Index++] - '0';
        if (Sequence > (18446744073709551615ull - Digit) / 10) return 0;
        Sequence = Sequence * 10 + Digit;
    }
    if ((LeadingZero && Index != SequenceStart + 1) || !Take(",\"version\":1}")) return 0;
    return Index == Count ? Sequence : 0;
}

bool HandleUpdate(const wchar_t* Root)
{
    Path Workspace(Root, L".wot-update");
    if (!Exists(Workspace)) return true;
    if (!Directory(Workspace)) { ReportError(RestoreMessage); return false; }
    Path Live(Root, L"Game"), Prev(Workspace, L"prev"), Staged(Workspace, L"staged");
    Path Old(Prev, L"Game"), Candidate(Staged, L"Game");
    Path Ready(Workspace, L"ready"), Trial(Workspace, L"trial"), RolledBack(Workspace, L"rolled-back");
    if ((Exists(Prev) && !Directory(Prev)) || (Exists(Staged) && !Directory(Staged))
        || (Exists(Live) && !Directory(Live)))
    { ReportError(RestoreMessage); return false; }
    bool RestoredMissingGame = false;
    if (!Exists(Live) && Exists(Old))
    {
        if (!Directory(Old) || !MoveFileExW(Old, Live, 0))
        { ReportError(RestoreMessage); return false; }
        RestoredMissingGame = true;
    }
    Marker Value;
    bool HaveTrial = false;
    if (Exists(Trial))
    {
        // A torn or corrupt trial (full disk, power loss) is stale: drop it
        // rather than refusing every later launch.
        HaveTrial = ReadMarker(Trial, true, Value);
        if (!HaveTrial) RemoveFile(Trial);
    }
    if (HaveTrial)
    {
        // Only the game changes pending to started, after its shared profile hold.
        if (Value.State == TrialState::Pending) return true;
        // With no previous tree there is nothing to restore: keep the current one.
        if (!RestoredMissingGame && !Directory(Old))
        {
            RemoveFile(Trial);
            return true;
        }
        if (!RestoredMissingGame)
        {
            if (!RemoveTree(Staged) || !EnsureDirectory(Staged)
                || !MoveFileExW(Live, Candidate, 0))
            { ReportError(RestoreMessage); return false; }
            if (!MoveFileExW(Old, Live, 0))
            {
                MoveFileExW(Candidate, Live, 0);
                ReportError(RestoreMessage);
                return false;
            }
        }
        if (!RemoveFile(RolledBack) || !WriteMarker(RolledBack, Value.Sequence, false)
            || !RemoveFile(Trial) || !RemoveFile(Ready))
        { ReportError(RecordMessage); return false; }
        RemoveTree(Staged);
        return true;
    }
    if (!Exists(Ready)) return true;
    if (!ReadMarker(Ready, false, Value))
    {
        RemoveFile(Ready);
        return true;
    }
    if (Value.Sequence <= RunningSequence(Live))
    {
        RemoveFile(Ready);
        RemoveTree(Staged);
        return true;
    }
    Path CandidateExe(Candidate, L"WoT\\Binaries\\Win64\\"
#if defined(WOT_LAUNCHER_CONFIGURATION_TEST)
        L"WoT-Win64-Test.exe"
#else
        L"WoT-Win64-Shipping.exe"
#endif
    );
    if (!Directory(Candidate) || !Exists(CandidateExe))
    {
        // A torn or incomplete ready marker is stale; the game stages again.
        RemoveFile(Ready);
        return true;
    }
    if (!RemoveTree(Prev) || !EnsureDirectory(Prev) || !MoveFileExW(Live, Old, 0))
    { ReportError(UpdateMessage); return true; }
    if (!MoveFileExW(Candidate, Live, 0))
    {
        if (!MoveFileExW(Old, Live, 0)) { ReportError(RestoreMessage); return false; }
        ReportError(UpdateMessage);
        return true;
    }
    if (!WriteMarker(Trial, Value.Sequence, true) || !RemoveFile(Ready))
    {
        if (!MoveFileExW(Live, Candidate, 0) || !MoveFileExW(Old, Live, 0))
        { ReportError(RestoreMessage); return false; }
        if (!RemoveFile(Trial)) { ReportError(RecordMessage); return false; }
        ReportError(UpdateMessage);
    }
    return true;
}

bool HandleUpdateWithProfileLock(const wchar_t* Root)
{
    Path State(Root, L"Data");
    if (!EnsureDirectory(State)) { ReportError(RestoreMessage); return false; }
    Path LockPath(State, L"session.lock");
    HANDLE Lock = CreateFileW(LockPath, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (Lock == INVALID_HANDLE_VALUE) { ReportError(RestoreMessage); return false; }
    BY_HANDLE_FILE_INFORMATION Info = {};
    if (!GetFileInformationByHandle(Lock, &Info) || Info.nNumberOfLinks != 1
        || (Info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
    { CloseHandle(Lock); ReportError(RestoreMessage); return false; }
    OVERLAPPED Range = {};
    if (!LockFileEx(Lock, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &Range))
    {
        const DWORD Error = GetLastError();
        CloseHandle(Lock);
        if (Error == ERROR_LOCK_VIOLATION) return true; // Another copy: silently defer ALL update handling.
        ReportError(RestoreMessage);
        return false;
    }
    const bool Result = HandleUpdate(Root);
    UnlockFileEx(Lock, 0, 1, 0, &Range);
    CloseHandle(Lock); // The child must be able to acquire its shared hold.
    return Result;
}
}

extern "C" void WINAPI wWinMainCRTStartup()
{
    DWORD Result = ERROR_BAD_ENVIRONMENT;
    wchar_t* Launcher = ExecutablePath();
    if (!Launcher)
    {
        ReportError(L"WoT Reborn couldn't determine where it is installed.");
        ExitProcess(Result);
    }
    wchar_t* LastSeparator = Launcher;
    for (wchar_t* Cursor = Launcher; *Cursor; ++Cursor)
    {
        if (*Cursor == L'\\' || *Cursor == L'/')
        {
            LastSeparator = Cursor;
        }
    }
    *LastSeparator = 0;
    if (!PrepareWritableData(Launcher))
    {
        HeapFree(GetProcessHeap(), 0, Launcher);
        ExitProcess(ERROR_ACCESS_DENIED);
    }

    if (!HandleUpdateWithProfileLock(Launcher)) ExitProcess(ERROR_WRITE_FAULT);
    wchar_t* Child = Join(Launcher, ChildRelative);
    if (!Child || GetFileAttributesW(Child) == INVALID_FILE_ATTRIBUTES)
    {
        ReportError(MissingChildMessage);
        if (Child) HeapFree(GetProcessHeap(), 0, Child);
        HeapFree(GetProcessHeap(), 0, Launcher);
        ExitProcess(ERROR_FILE_NOT_FOUND);
    }
    const wchar_t* Arguments = RawArgumentSuffix();
    wchar_t* Command = Allocate(Length(Child) + Length(Arguments) + 3);
    if (!Command)
    {
        HeapFree(GetProcessHeap(), 0, Child);
        HeapFree(GetProcessHeap(), 0, Launcher);
        ExitProcess(ERROR_NOT_ENOUGH_MEMORY);
    }
    wchar_t* Out = Command;
    *Out++ = L'"';
    Copy(Out, Child);
    *Out++ = L'"';
    Copy(Out, Arguments);
    *Out = 0;
    wchar_t* GameDirectory = Join(Launcher, L"Game");
    STARTUPINFOW Startup = {};
    Startup.cb = sizeof(Startup);
    PROCESS_INFORMATION Process = {};
    if (!CreateProcessW(Child, Command, nullptr, nullptr, FALSE, 0, nullptr,
            GameDirectory, &Startup, &Process))
    {
        ReportError(StartChildMessage);
        Result = GetLastError();
    }
    else
    {
        CloseHandle(Process.hThread);
        WaitForSingleObject(Process.hProcess, INFINITE);
        if (!GetExitCodeProcess(Process.hProcess, &Result))
        {
            Result = ERROR_GEN_FAILURE;
        }
        CloseHandle(Process.hProcess);
    }
    if (GameDirectory) HeapFree(GetProcessHeap(), 0, GameDirectory);
    HeapFree(GetProcessHeap(), 0, Command);
    HeapFree(GetProcessHeap(), 0, Child);
    HeapFree(GetProcessHeap(), 0, Launcher);
    ExitProcess(Result);
}
