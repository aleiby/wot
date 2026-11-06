set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)
# Must match the consuming UE module, not merely be self-consistent: UBT leaves
# bUseStaticCRT false (so /MD, and UE uses the release CRT even for Debug) and
# excludes LIBCMT/LIBCPMT from the link, so a static-CRT archive cannot be
# consumed. Pair this with a /MD vcpkg triplet -- x64-windows-static-md gives
# static dependency libraries against this same dynamic UCRT.
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL CACHE STRING "Dynamic MSVC runtime, matching UE")
set(CMAKE_CXX_STANDARD 17 CACHE STRING "Pinned C++ language mode")
set(CMAKE_CXX_EXTENSIONS OFF CACHE BOOL "No compiler language extensions")
if(DEFINED MSVC_TOOLSET_VERSION AND NOT MSVC_TOOLSET_VERSION MATCHES "^14[3-9]$")
  message(FATAL_ERROR "WoT innoextract requires the Visual Studio v143 toolset")
endif()
