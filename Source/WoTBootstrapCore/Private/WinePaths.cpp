#include "WoTBootstrapCore/WinePaths.h"

#include "WoTBootstrapCore/NativeHandles.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

FString WoTWineUnixDownloadsPath(FStringView UnixHome)
{
	FString Home(UnixHome);
	while (Home.Len() > 1 && Home.EndsWith(TEXT("/")))
	{
		Home.LeftChopInline(1);
	}
	if (Home.Len() < 2 || !Home.StartsWith(TEXT("/")))
	{
		return FString();
	}
	return Home + TEXT("/Downloads");
}

FString WoTWineDosPathForUnixPath(FStringView UnixPath)
{
	if (UnixPath.IsEmpty() || !WoTIsRunningUnderWine())
	{
		return FString();
	}
#if PLATFORM_WINDOWS
	// Wine's kernel32 export; the result is allocated on the process heap.
	using FWineGetDosFileName = WCHAR* (CDECL*)(const char*);
	static const FWineGetDosFileName WineGetDosFileName =
		reinterpret_cast<FWineGetDosFileName>(::GetProcAddress(
			::GetModuleHandleW(L"kernel32.dll"), "wine_get_dos_file_name"));
	if (WineGetDosFileName == nullptr)
	{
		return FString();
	}
	const FString Unix(UnixPath);
	WCHAR* Mapped = WineGetDosFileName(TCHAR_TO_UTF8(*Unix));
	if (Mapped == nullptr)
	{
		return FString();
	}
	FString Result(Mapped);
	::HeapFree(::GetProcessHeap(), 0, Mapped);
	return Result;
#else
	return FString();
#endif
}
