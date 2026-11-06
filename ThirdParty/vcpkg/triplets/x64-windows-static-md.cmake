# Overlay of vcpkg's stock x64-windows-static-md, passed with
# --overlay-triplets so it shadows the stock triplet under the same name.
#
# Static dependency libraries against the DYNAMIC UCRT, as the stock triplet:
# UBT leaves bUseStaticCRT false and excludes LIBCMT/LIBCPMT, so a /MT archive
# cannot be consumed at all.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_PROVIDED_FORTRAN ON)

# Pin the toolset to the one UBT links the game with. Unpinned, vcpkg takes the
# newest Visual Studio on the box (a 14.5x compiler once VS 18 is installed)
# while wot_innoextract itself is compiled by the v143 toolchain file and UBT
# links with 14.4x; MSVC requires the linker to be at least as new as every
# compiler whose objects it consumes.
set(VCPKG_PLATFORM_TOOLSET v143)

# The vendor recipe builds Release only, so the debug half is never consumed.
set(VCPKG_BUILD_TYPE release)
