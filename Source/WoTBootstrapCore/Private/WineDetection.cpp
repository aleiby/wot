// Detects a Windows build running under Wine (Proton on Linux/Steam Deck).
//
// Wine's ntdll exports wine_get_version; native Windows never does. The answer
// cannot change while the process runs, so it is computed once. macOS builds
// are never under Wine. A test seam forces either answer on every platform.
#include "WoTBootstrapCore/NativeHandles.h"

#include "Templates/Atomic.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace
{
#if WITH_AUTOMATION_TESTS
// -1 = detect; 0 = forced native; 1 = forced Wine.
TAtomic<int8> GWoTRunningUnderWineForTesting(-1);
#endif

bool WoTDetectWine()
{
#if PLATFORM_WINDOWS
	const HMODULE Ntdll = ::GetModuleHandleW(L"ntdll.dll");
	return Ntdll != nullptr && ::GetProcAddress(Ntdll, "wine_get_version") != nullptr;
#else
	return false;
#endif
}
}  // namespace

bool WoTIsRunningUnderWine()
{
#if WITH_AUTOMATION_TESTS
	const int8 Forced = GWoTRunningUnderWineForTesting.Load();
	if (Forced >= 0)
	{
		return Forced != 0;
	}
#endif
	static const bool bWine = WoTDetectWine();
	return bWine;
}

#if WITH_AUTOMATION_TESTS
void WoTSetRunningUnderWineForTesting(TOptional<bool> bRunningUnderWine)
{
	GWoTRunningUnderWineForTesting.Store(bRunningUnderWine.IsSet()
		? static_cast<int8>(bRunningUnderWine.GetValue() ? 1 : 0) : static_cast<int8>(-1));
}
#endif
