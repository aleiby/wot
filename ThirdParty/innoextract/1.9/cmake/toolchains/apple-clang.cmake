set(CMAKE_SYSTEM_NAME Darwin)
set(CMAKE_SYSTEM_PROCESSOR arm64)
set(CMAKE_C_COMPILER /usr/bin/clang CACHE FILEPATH "Pinned Apple C compiler")
set(CMAKE_CXX_COMPILER /usr/bin/clang++ CACHE FILEPATH "Pinned Apple C++ compiler")
set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "WoT Mac architecture")
# Match the UE 5.8 Mac link target; without this the objects stamp the host
# SDK version and every UE link warns "built for newer macOS".
set(CMAKE_OSX_DEPLOYMENT_TARGET 14.0 CACHE STRING "WoT Mac deployment target")
# Pin the SDK to the one UBT actually links (its .rsp passes -isysroot to
# Xcode's SDK). Left unset, CMake resolved the CommandLineTools SDK instead,
# whose zlib/bzip2/iconv .tbd stubs hash DIFFERENTLY - so the vendor
# manifest was pinning the content of stubs the final binary never links.
# Found in the 2026-07-26 toolchain review; the vcpkg migration that was
# meant to absorb this fix was descoped to boost+liblzma, so the SDK stubs
# stay load-bearing and must be the right ones.
set(CMAKE_OSX_SYSROOT
    "/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk"
    CACHE PATH "WoT Mac SDK root (must match UBT's isysroot)")
set(CMAKE_POSITION_INDEPENDENT_CODE ON CACHE BOOL "Position-independent vendor objects")
set(Boost_ROOT /opt/homebrew/opt/boost CACHE PATH "Pinned Homebrew Boost prefix")
set(LibLZMA_ROOT /opt/homebrew/opt/xz CACHE PATH "Pinned Homebrew xz prefix")
set(LIBLZMA_LIBRARY /opt/homebrew/opt/xz/lib/liblzma.a CACHE FILEPATH "Pinned static liblzma")
set(CMAKE_FIND_PACKAGE_PREFER_CONFIG OFF CACHE BOOL "Use deterministic CMake find modules")
