#define WIN32_LEAN_AND_MEAN
#include <windows.h>

extern "C" void* __cdecl memset(void* Destination, int Value, size_t Count)
{
    unsigned char* Out = static_cast<unsigned char*>(Destination);
    while (Count--) *Out++ = static_cast<unsigned char>(Value);
    return Destination;
}

extern "C" void WINAPI wWinMainCRTStartup()
{
    wchar_t* Path = static_cast<wchar_t*>(HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, 32768 * sizeof(wchar_t)));
    // Opt-in exact Data/session.lock shared byte-range hold. The args file
    // is published after acquisition, so the parent can observe readiness.
    HANDLE Profile = INVALID_HANDLE_VALUE;
    OVERLAPPED Range = {};
    if (GetEnvironmentVariableW(L"WOT_LAUNCHER_TEST_PROFILE_LOCK", Path, 32768))
    {
        Profile = CreateFileW(Path, GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (Profile == INVALID_HANDLE_VALUE
            || !LockFileEx(Profile, LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &Range))
            ExitProcess(91);
    }
    const DWORD Count = GetEnvironmentVariableW(
        L"WOT_LAUNCHER_TEST_ARGS_FILE", Path, 32768);
    if (Count > 0 && Count < 32768)
    {
        HANDLE File = CreateFileW(Path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (File != INVALID_HANDLE_VALUE)
        {
            const WORD Bom = 0xfeff;
            DWORD Written = 0;
            WriteFile(File, &Bom, sizeof(Bom), &Written, nullptr);
            const wchar_t* Command = GetCommandLineW();
            DWORD Length = 0;
            while (Command[Length]) ++Length;
            WriteFile(File, Command, Length * sizeof(wchar_t), &Written, nullptr);
            CloseHandle(File);
        }
    }
    HeapFree(GetProcessHeap(), 0, Path);
    // Hold only when requested by the running-copy launcher fixture. No
    // sockets, game code or extra directory handles participate in the proof.
    wchar_t Hold[2] = {};
    if (GetEnvironmentVariableW(L"WOT_LAUNCHER_TEST_HOLD", Hold, 2))
    {
        Sleep(INFINITE);
    }
    wchar_t ExitText[16] = {};
    DWORD ExitCode = 0;
    if (GetEnvironmentVariableW(L"WOT_LAUNCHER_TEST_EXIT_CODE", ExitText, 16))
    {
        for (DWORD Index = 0; ExitText[Index] >= L'0' && ExitText[Index] <= L'9'; ++Index)
        {
            ExitCode = ExitCode * 10 + static_cast<DWORD>(ExitText[Index] - L'0');
        }
    }
    if (Profile != INVALID_HANDLE_VALUE)
    {
        UnlockFileEx(Profile, 0, 1, 0, &Range);
        CloseHandle(Profile);
    }
    ExitProcess(ExitCode);
}
