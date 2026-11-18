cmake_minimum_required(VERSION 3.24)
project(wot_innoextract VERSION 1.9 LANGUAGES C CXX)

# The vendored flag set is pinned (vendor-manifest.json compiler_flags), so the
# configuration must not contribute optimization or runtime-check flags of its
# own. Forced rather than defaulted: a chainloaded toolchain can seed a build
# type before this point (vcpkg's seeds Debug), and MSVC hard-errors on Debug's
# /RTC1 combined with the pinned /O2 (D8016) where Clang silently takes the
# last flag -- which is why only the Win64 lane trips over it.
set(CMAKE_BUILD_TYPE Release CACHE STRING "Pinned vendored-library build type" FORCE)

set(WOT_PINNED_ARCHIVE_SHA256
  "6344a69fc1ed847d4ed3e272e0da5998948c6b828cb7af39c6321aba6cf88126")
foreach(required WOT_INNOEXTRACT_ARCHIVE WOT_INNOEXTRACT_PATCH WOT_VENDOR_MANIFEST)
  if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
    message(FATAL_ERROR "${required} is required")
  endif()
endforeach()

get_filename_component(WOT_VENDOR_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
# WOT_VENDOR_ROOT is <repo>/ThirdParty/innoextract/1.9 — exactly three levels
# below the repository root.
get_filename_component(WOT_REPOSITORY_ROOT "${WOT_VENDOR_ROOT}/../../.." ABSOLUTE)
set(WOT_STAGE_TOOL "${WOT_REPOSITORY_ROOT}/Tools/build/stage_innoextract_native.py")
find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)

file(SHA256 "${WOT_INNOEXTRACT_ARCHIVE}" WOT_ACTUAL_ARCHIVE_SHA256)
if(NOT WOT_ACTUAL_ARCHIVE_SHA256 STREQUAL WOT_PINNED_ARCHIVE_SHA256)
  message(FATAL_ERROR "innoextract archive SHA-256 mismatch")
endif()

execute_process(
  COMMAND "${Python3_EXECUTABLE}" "${WOT_STAGE_TOOL}" verify-source
    --archive "${WOT_INNOEXTRACT_ARCHIVE}"
    --sha256 "${WOT_PINNED_ARCHIVE_SHA256}"
    --patch "${WOT_INNOEXTRACT_PATCH}"
    --vendor-manifest "${WOT_VENDOR_MANIFEST}"
  RESULT_VARIABLE WOT_VERIFY_RESULT
  OUTPUT_VARIABLE WOT_VERIFY_OUT ERROR_VARIABLE WOT_VERIFY_ERR)
if(NOT WOT_VERIFY_RESULT EQUAL 0)
  # Surface the tool's own diagnosis. Collapsing it into one generic sentence
  # sent people hunting through CMake internals for a message the verifier had
  # already written.
  message(FATAL_ERROR
    "manifest-owned innoextract source verification failed.\n"
    "${WOT_VERIFY_ERR}${WOT_VERIFY_OUT}\n"
    "This is the VENDOR MAINTENANCE path: you only need it when changing the\n"
    "vendored library or its pinned dependencies. To build or run the game,\n"
    "use the ordinary development path (installed UE + UBT) — it consumes the\n"
    "committed ThirdParty/innoextract/1.9/lib/<Platform>/ artifact and needs\n"
    "none of this. See docs/BUILD-PATHS.md.")
endif()

set(WOT_PREPARED_PARENT "${CMAKE_CURRENT_BINARY_DIR}/manifest-source")
execute_process(
  COMMAND "${Python3_EXECUTABLE}" "${WOT_STAGE_TOOL}" prepare-source
    --archive "${WOT_INNOEXTRACT_ARCHIVE}"
    --patch "${WOT_INNOEXTRACT_PATCH}"
    --destination "${WOT_PREPARED_PARENT}"
  RESULT_VARIABLE WOT_PREPARE_RESULT)
if(NOT WOT_PREPARE_RESULT EQUAL 0)
  message(FATAL_ERROR "could not prepare the clean patched innoextract source")
endif()
set(WOT_SOURCE "${WOT_PREPARED_PARENT}/innoextract-1.9")

set(Boost_USE_STATIC_LIBS ON)
# boost_system is header-only since 1.69 (this list's own floor) and Boost
# 1.90 no longer ships it as a findable component at all.
find_package(Boost 1.69 REQUIRED COMPONENTS iostreams filesystem date_time)
find_package(LibLZMA REQUIRED)
find_package(ZLIB REQUIRED)
find_package(BZip2 REQUIRED)
if(NOT WIN32)
  find_package(Iconv REQUIRED)
endif()

# Promotion verifies the exact dependency artifacts CMake selected, rather than
# trusting a package-manager identity sentence. Paths are emitted after generator
# expressions resolve so imported target locations are the ones actually linked.
function(wot_json_array output)
  set(items "")
  set(values ${ARGN})
  list(REMOVE_DUPLICATES values)
  foreach(value IN LISTS values)
    string(REPLACE "\\" "\\\\" escaped "${value}")
    string(REPLACE "\"" "\\\"" escaped "${escaped}")
    if(NOT items STREQUAL "")
      string(APPEND items ",")
    endif()
    string(APPEND items "\"${escaped}\"")
  endforeach()
  set(${output} "[${items}]" PARENT_SCOPE)
endfunction()

wot_json_array(WOT_BOOST_INCLUDES ${Boost_INCLUDE_DIRS})
wot_json_array(WOT_LZMA_INCLUDES ${LIBLZMA_INCLUDE_DIRS} ${LIBLZMA_INCLUDE_DIR})
wot_json_array(WOT_ZLIB_INCLUDES ${ZLIB_INCLUDE_DIRS} ${ZLIB_INCLUDE_DIR})
wot_json_array(WOT_BZIP2_INCLUDES ${BZIP2_INCLUDE_DIRS} ${BZIP2_INCLUDE_DIR})
if(NOT WIN32)
  wot_json_array(WOT_ICONV_INCLUDES ${Iconv_INCLUDE_DIRS} ${Iconv_INCLUDE_DIR})
endif()

if(WIN32)
  # The Homebrew heuristic below recovers the version from the Cellar path,
  # which carries the package revision (1.90.0_1) that Boost itself does not
  # report. A vcpkg tree is laid out <root>/installed/<triplet>/include, so the
  # same walk yields the triplet name -- a "version" that would keep verifying
  # across Boost upgrades. Take what the find module reports instead.
  set(WOT_BOOST_RESOLVED_VERSION "${Boost_VERSION_STRING}")
else()
  list(GET Boost_INCLUDE_DIRS 0 WOT_BOOST_INCLUDE)
  get_filename_component(WOT_BOOST_VERSION_DIR "${WOT_BOOST_INCLUDE}" DIRECTORY)
  get_filename_component(WOT_BOOST_RESOLVED_VERSION "${WOT_BOOST_VERSION_DIR}" NAME)
  if(WOT_BOOST_RESOLVED_VERSION STREQUAL "include")
    set(WOT_BOOST_RESOLVED_VERSION "${Boost_VERSION_STRING}")
  endif()
endif()
set(WOT_LZMA_RESOLVED_VERSION "${LIBLZMA_VERSION_STRING}")
set(WOT_ZLIB_RESOLVED_VERSION "${ZLIB_VERSION_STRING}")
set(WOT_BZIP2_RESOLVED_VERSION "${BZIP2_VERSION_STRING}")
if(WOT_BZIP2_RESOLVED_VERSION STREQUAL "")
  set(WOT_BZIP2_RESOLVED_VERSION "1.0.8")
endif()
if(WIN32)
  set(WOT_RESOLVED_PLATFORM "Win64")
  set(WOT_ICONV_RESOLVED_VERSION "Win32-codepage")
  set(WOT_ICONV_INCLUDES "[]")
  set(WOT_ICONV_LIBRARIES "[]")
else()
  set(WOT_RESOLVED_PLATFORM "Mac")
  set(WOT_ICONV_RESOLVED_VERSION "7")
  # Iconv::Iconv is an INTERFACE target when the SDK provides libiconv, so
  # $<TARGET_FILE:> is an error; the find module's variable holds the real
  # artifact path (the SDK .tbd).
  wot_json_array(WOT_ICONV_LIBRARIES ${Iconv_LIBRARIES} ${Iconv_LIBRARY})
endif()

file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/resolved-dependencies.json" CONTENT
  "{\"dependencies\":{\"boost\":{\"include_paths\":${WOT_BOOST_INCLUDES},\"library_paths\":[\"$<TARGET_FILE:Boost::iostreams>\",\"$<TARGET_FILE:Boost::filesystem>\",\"$<TARGET_FILE:Boost::date_time>\"],\"version\":\"${WOT_BOOST_RESOLVED_VERSION}\"},\"bzip2\":{\"include_paths\":${WOT_BZIP2_INCLUDES},\"library_paths\":[\"$<TARGET_FILE:BZip2::BZip2>\"],\"version\":\"${WOT_BZIP2_RESOLVED_VERSION}\"},\"iconv\":{\"include_paths\":${WOT_ICONV_INCLUDES},\"library_paths\":${WOT_ICONV_LIBRARIES},\"version\":\"${WOT_ICONV_RESOLVED_VERSION}\"},\"liblzma\":{\"include_paths\":${WOT_LZMA_INCLUDES},\"library_paths\":[\"$<TARGET_FILE:LibLZMA::LibLZMA>\"],\"version\":\"${WOT_LZMA_RESOLVED_VERSION}\"},\"zlib\":{\"include_paths\":${WOT_ZLIB_INCLUDES},\"library_paths\":[\"$<TARGET_FILE:ZLIB::ZLIB>\"],\"version\":\"${WOT_ZLIB_RESOLVED_VERSION}\"}},\"format\":\"wot-innoextract-resolved-dependencies-v1\",\"platform\":\"${WOT_RESOLVED_PLATFORM}\"}\n")

set(INNOEXTRACT_HAVE_ARC4 1)
set(INNOEXTRACT_HAVE_LZMA 1)
set(INNOEXTRACT_HAVE_ALIGNOF 1)
set(INNOEXTRACT_HAVE_STD_UNIQUE_PTR 1)
if(WIN32)
  set(INNOEXTRACT_HAVE_STD_CODECVT_UTF8_UTF16 1)
  set(INNOEXTRACT_HAVE_WIN32_CONV 1)
else()
  set(INNOEXTRACT_HAVE_ICONV 1)
  set(INNOEXTRACT_HAVE_TIMEGM 1)
  set(INNOEXTRACT_HAVE_GMTIME_R 1)
  set(INNOEXTRACT_HAVE_UTIMENSAT 1)
  set(INNOEXTRACT_HAVE_AT_FDCWD 1)
  set(INNOEXTRACT_HAVE_UTIMES 1)
  set(INNOEXTRACT_HAVE_BUILTIN_BSWAP16 1)
  set(INNOEXTRACT_HAVE_BUILTIN_BSWAP32 1)
  set(INNOEXTRACT_HAVE_BUILTIN_BSWAP64 1)
endif()
configure_file("${WOT_SOURCE}/src/configure.hpp.in"
  "${CMAKE_CURRENT_BINARY_DIR}/configure.hpp")

set(WOT_SOURCES
  src/cli/goggalaxy.cpp
  src/crypto/adler32.cpp src/crypto/arc4.cpp src/crypto/checksum.cpp
  src/crypto/crc32.cpp src/crypto/hasher.cpp src/crypto/md5.cpp src/crypto/sha1.cpp
  src/loader/exereader.cpp src/loader/offsets.cpp
  src/setup/component.cpp src/setup/data.cpp src/setup/delete.cpp
  src/setup/directory.cpp src/setup/expression.cpp src/setup/file.cpp
  src/setup/filename.cpp src/setup/header.cpp src/setup/icon.cpp src/setup/info.cpp
  src/setup/ini.cpp src/setup/item.cpp src/setup/language.cpp src/setup/message.cpp
  src/setup/permission.cpp src/setup/registry.cpp src/setup/run.cpp
  src/setup/task.cpp src/setup/type.cpp src/setup/version.cpp src/setup/windows.cpp
  src/stream/block.cpp src/stream/callback.cpp src/stream/chunk.cpp
  src/stream/file.cpp src/stream/lzma.cpp src/stream/slice.cpp
  src/util/console.cpp src/util/encoding.cpp src/util/load.cpp src/util/log.cpp
  src/util/time.cpp src/wot/adapter.cpp src/wot/release.cpp)
if(WIN32)
  list(APPEND WOT_SOURCES src/util/windows.cpp)
endif()
list(TRANSFORM WOT_SOURCES PREPEND "${WOT_SOURCE}/")

add_library(wot_innoextract STATIC ${WOT_SOURCES})
target_compile_features(wot_innoextract PRIVATE cxx_std_17)
target_include_directories(wot_innoextract
  PRIVATE "${WOT_SOURCE}/src" "${CMAKE_CURRENT_BINARY_DIR}"
  PUBLIC "${WOT_VENDOR_ROOT}/include")
target_compile_definitions(wot_innoextract PRIVATE
  BOOST_ALL_NO_LIB=1 BOOST_IOSTREAMS_NO_LIB=1)
if(WIN32)
  # windows.h defines min/max as macros, which turns every std::min/std::max
  # call in the upstream sources into a syntax error (C2589). Upstream builds
  # for Windows through MinGW, where the same header is reached via a path that
  # already suppresses them, so this is unique to the MSVC lane.
  target_compile_definitions(wot_innoextract PRIVATE NOMINMAX)
endif()
target_link_libraries(wot_innoextract PRIVATE
  Boost::iostreams Boost::filesystem Boost::date_time
  LibLZMA::LibLZMA ZLIB::ZLIB BZip2::BZip2)
if(NOT WIN32)
  target_link_libraries(wot_innoextract PRIVATE Iconv::Iconv)
endif()

if(MSVC)
  # The CRT is deliberately absent from this list: it has to match the consuming
  # UE module rather than being pinned here, so it comes from
  # CMAKE_MSVC_RUNTIME_LIBRARY (toolchains/msvc-v143.cmake). A /MT spelled here
  # would silently defeat that setting by landing last on the command line.
  target_compile_options(wot_innoextract PRIVATE
    /O2 /EHsc /permissive- /W3 /Gy /Zc:__cplusplus /Zc:inline)
else()
  target_compile_options(wot_innoextract PRIVATE
    -O2 -fno-common -fvisibility=hidden -fvisibility-inlines-hidden
    -ffunction-sections -fdata-sections -Wall -Wextra)
endif()
set_target_properties(wot_innoextract PROPERTIES
  OUTPUT_NAME wot_innoextract
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES)

# UBT is deliberately allowed to name only the one fixed vendor archive. Merge
# non-SDK static dependencies into that archive after CMake creates the core
# object archive. Apple SDK zlib/bzip2/iconv remain normal SDK symbols already
# linked by the UE platform layer; Windows folds its resolved .lib inputs too.
set(WOT_CORE_ARCHIVE "$<TARGET_FILE:wot_innoextract>.core")
if(APPLE)
  add_custom_command(TARGET wot_innoextract POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E rename
      "$<TARGET_FILE:wot_innoextract>" "${WOT_CORE_ARCHIVE}"
    COMMAND /usr/bin/libtool -static -o "$<TARGET_FILE:wot_innoextract>"
      "${WOT_CORE_ARCHIVE}"
      "$<TARGET_FILE:Boost::iostreams>"
      "$<TARGET_FILE:Boost::filesystem>"
      "$<TARGET_FILE:Boost::date_time>"
      "$<TARGET_FILE:LibLZMA::LibLZMA>"
    COMMAND "${CMAKE_COMMAND}" -E rm -f "${WOT_CORE_ARCHIVE}"
    VERBATIM)
elseif(MSVC)
  add_custom_command(TARGET wot_innoextract POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E rename
      "$<TARGET_FILE:wot_innoextract>" "${WOT_CORE_ARCHIVE}"
    COMMAND "${CMAKE_AR}" /NOLOGO /OUT:$<TARGET_FILE:wot_innoextract>
      "${WOT_CORE_ARCHIVE}"
      "$<TARGET_FILE:Boost::iostreams>"
      "$<TARGET_FILE:Boost::filesystem>"
      "$<TARGET_FILE:Boost::date_time>"
      "$<TARGET_FILE:LibLZMA::LibLZMA>"
      "$<TARGET_FILE:ZLIB::ZLIB>"
      "$<TARGET_FILE:BZip2::BZip2>"
    COMMAND "${CMAKE_COMMAND}" -E rm -f "${WOT_CORE_ARCHIVE}"
    VERBATIM)
else()
  message(FATAL_ERROR "wot_innoextract has no reviewed archive merge for this toolchain")
endif()

include(GNUInstallDirs)
install(TARGETS wot_innoextract ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(FILES "${WOT_VENDOR_ROOT}/include/wot/innoextract_adapter.h"
  DESTINATION "include/wot")
