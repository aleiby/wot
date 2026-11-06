WoT innoextract 1.9 vendor package
=================================

This directory pins the unmodified innoextract 1.9 release archive and one
reviewed library-adapter patch. The upstream project is Copyright (C) 2011-2020
Daniel Scharrer and is distributed under the license in LICENSE.

WoT never builds or ships the innoextract CLI. The CMake project verifies the
archive, inventory, patch, toolchains, and post-patch source hashes before it
defines the sole target, the static library `wot_innoextract`. Its only public
surface is `include/wot/innoextract_adapter.h`.

The adapter has no filename API. Callers provide an explicit byte size and a
read-at-offset callback, then stream an enumerated entry through a write
callback. Adapter allocations and diagnostic strings remain adapter-owned, C++
exceptions are caught at every C ABI boundary, and the last-error string is
thread-local. One library-wide mutex serializes adapter calls that may touch
upstream state; callbacks therefore must not re-enter the adapter.

Build and promotion are intentionally separate. CMake installs into a staging
prefix; `Tools/build/stage_innoextract_native.py promote` accepts only the fixed
header and platform library names and records the promoted library digest in
`vendor-manifest.json`.

CMake also emits `resolved-dependencies.json` in its build directory. Promotion
requires that file via `--resolved-dependencies`; it hashes the resolved SDK TBD
or the selected Homebrew libraries plus sorted include trees and refuses any
version/content mismatch with the per-platform manifest pins.

Getting the library
-------------------

The repository carries no Git LFS, so a checkout either fetches the reviewed library, which
is placed only if its SHA-256 equals the manifest pin:

    python Tools/build/fetch_innoextract_libs.py fetch --platform Win64

or builds one from source with `Tools/build/build_innoextract_native.py`. A
contributor build matches the pinned dependency versions but not their bytes,
so it leaves the manifest untouched and is not the reviewed artifact;
`stage_innoextract_native.py verify-library` says which library is in place.
See docs/BUILD-PATHS.md.
