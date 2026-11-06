# The three build paths

There are three ways to build things in this repo and they answer different
questions. Most contributors only ever need the first. If a gate has just
refused something and sent you here, you are probably on a path you did not
mean to be on.

## 1. Ordinary development — what you want almost always

UE 5.8 built from the project's patched engine source (see the README), plus UBT:

```
"<UE 5.8>/Engine/Build/BatchFiles/Build.bat" WoTEditor Win64 Development -project="<repo>/WoT.uproject"
"<UE 5.8>/Engine/Build/BatchFiles/Mac/Build.sh" WoTEditor Mac Development -project="<repo>/WoT.uproject"
```

No manifest, no hash gate, no pinned compiler check. **Any compiler, any
configuration — Debug, DebugGame, whatever you need.** This path consumes the
`ThirdParty/<pkg>/<ver>/lib/<Platform>/` artifact as-is; it does not rebuild it
and cannot invalidate it.

The repository carries no Git LFS, so a checkout needs the library once. (Use `python3`
where your system has no `python`.)

**Fetch the reviewed library (preferred):**

```
python Tools/build/fetch_innoextract_libs.py fetch --platform Win64   # or Mac
```

It downloads the asset named for the `vendor-manifest.json` pin from the
repository's `vendor-innoextract-1.9` prerelease and places it only if its
SHA-256 matches that pin. If that prerelease is not published yet, or is
unreachable, the fetch fails and places nothing. Build it from source instead.

**Or build it from source** (works offline; this is a contributor build, so
it runs but is not the reviewed artifact):

- Win64: Visual Studio 2022 with the C++ and "C++ CMake tools" components (CMake
  ships with it), plus a clone of https://github.com/microsoft/vcpkg with
  `bootstrap-vcpkg.bat` run once:

  ```
  python Tools/build/build_innoextract_native.py --vcpkg-root <vcpkg clone>
  ```

- Mac: Xcode command-line tools plus Homebrew `cmake`, `ninja`, `boost` and
  `xz` (`brew install cmake ninja boost xz`):

  ```
  python3 Tools/build/build_innoextract_native.py
  ```

Either way, `stage_innoextract_native.py verify-library --platform <P>
--vendor-manifest ThirdParty/innoextract/1.9/vendor-manifest.json` says whether
the library in place is the reviewed one. Development builds accept either
library. Guarded release packaging (path 3) accepts only the reviewed one.

If you are fixing gameplay, porting a class, or chasing a bug, stop here.

## 2. Vendor maintenance — only when changing a vendored library

Rebuilding and re-promoting a pinned third-party library (today: innoextract
1.9). This is the path that runs `cmake` against
`ThirdParty/innoextract/1.9/cmake` and then
`stage_innoextract_native.py promote`. One command runs all of it:

```
python Tools/build/build_innoextract_native.py --vcpkg-root <vcpkg clone>           # Win64 contributor
python Tools/build/build_innoextract_native.py --vcpkg-root <vcpkg clone> --record  # Win64 maintainer
python3 Tools/build/build_innoextract_native.py [--record]                         # Mac
```

Win64 resolves its inputs from vcpkg in manifest mode (`ThirdParty/vcpkg`,
pinned `builtin-baseline`, an in-repo overlay triplet that pins the v143
toolset UBT links with); the Mac resolves them from Homebrew (`cmake`,
`ninja`, `boost`, `xz`).

The default contributor build holds the dependency **versions** to the pins
and places the library without touching `vendor-manifest.json`. It cannot hold
the dependency **bytes**: MSVC and vcpkg builds are not byte-identical across
machines, so only the machine that recorded the pins passes that check. The result
works but is not the reviewed artifact. `--record` is the maintainer path:
bytes must match, and the new library digest is re-pinned. A re-pinned library
is then published for path 1 with `fetch_innoextract_libs.py stage-assets
--output <dir>` and uploaded to the `vendor-innoextract-1.9` prerelease. Keep
that release a prerelease: `/releases/latest` is what the game reads for
updates.

It verifies that the archive, the patch, every patched source, the build
recipe, and the resolved dependency artifacts match `vendor-manifest.json`,
and it refuses to promote anything that does not. That strictness is the
point: the promoted `.a`/`.lib` is an attested artifact whose provenance has
to be checkable later.

**A dependency-version mismatch here is the gate working, not a broken
build.** It means the versions your box resolved differ from the pinned ones.
Re-pinning is a deliberate, reviewed edit to `vendor-manifest.json`, not
something to work around.

Note that path 1 is not a way *around* this gate when you have genuinely
changed native inputs — it simply keeps using the previously committed
artifact, which will not contain your change.

## 3. Release evidence — the guarded wrappers

`Tools/build/build_guarded_ue58.{sh,ps1}` builds against the **pinned source
engine** and verifies engine commit, patch state, and exact compiler identity
before compiling, then records a build receipt.

The guarded build patches Unreal Engine source, so the engine patch itself
never ships in this repository; Epic's licence limits engine source to
licensees. What a checkout carries decides what it can do:

- **Public clone:** it carries `EnginePatches/UE-5.8/manifest.json` (file paths
  and SHA-256 values only) but not the patch. Normal and incremental guarded
  builds work against an engine that is **already patched**, and the wrapper
  proves it by checking every manifest-owned file against its patched hash.
- **Applying the patch:** a `-Clean`/`--clean` build, or any apply or record
  step, needs `guarded-bootstrap.patch` from the licensee-only EpicGames fork
  linked under "Building from source" in `README.md`. Without the patch these
  steps stop and say so.

This exists so that "which engine and which compiler produced these bytes"
has exactly one answer for anything we attest or ship. Configuration is free
here too (`--configuration Debug` works); it is the *toolchain* that is
pinned, not the optimization level.

Use this for release candidates, guarded test programs, and anything whose
output becomes evidence. Do not use it as your daily loop — it is slower and
it will refuse a box whose toolchain has drifted, which is correct behaviour
for release evidence and merely annoying for development.

## Which am I on?

| Symptom | Path | What to do |
|---|---|---|
| `ENGINE PATCH REJECTED`, `UNOWNED ENGINE DRIFT`, toolchain mismatch | 3 | You invoked a guarded wrapper. Use path 1 unless you meant to produce evidence. |
| `stage_innoextract_native: ... mismatch` | 2 | Your dependency versions differ from the pins. Only re-pin if you are deliberately changing native inputs. |
| `manifest-owned innoextract source verification failed` | 2 | The vendored source/patch/recipe does not match its pins; the tool's own message above it says which. |
| `The vendored innoextract library ... is missing` / `is a Git LFS pointer` | 1 | Run `fetch_innoextract_libs.py fetch --platform <P>` once. |
| Plain compile errors in game code | 1 | Normal development; nothing here applies. |

## Debugging into a vendored library

The promoted artifact is Release and is pinned by hash, so `promote` will
refuse a Debug build by design. To debug into it, build the library Debug into
a scratch directory, point at it by hand, and do not promote the result.
