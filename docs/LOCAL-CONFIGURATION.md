# Local configuration for private inputs and machine paths

Public checkouts do not contain the purchaser-supplied retail files or generated
exports. Point all tools at a private reference directory containing `original/`
and `exports/`:

```sh
export WOT_REFERENCE_ROOT=/path/to/private-reference-root
```

An explicit reference-root command-line option takes precedence where a tool
already exposes one. Without either setting, tools look in the repository
root. If a requested input is absent, the
tool reports the missing path and names `WOT_REFERENCE_ROOT`.

Machine-local engine and scratch paths live in an ignored file. Copy the
template and replace only the values needed on that machine:

```sh
cp Tools/local.example.json Tools/local.json
```

Each key can instead be supplied as `WOT_<UPPERCASE_KEY>`; for example,
`WOT_SOURCE_ENGINE_ROOT_WIN64` overrides `source_engine_root_win64` and
`WOT_SCRATCH_ROOT` overrides `scratch_root`. Do not commit `Tools/local.json`.
Windows guarded builds normally require `source_engine_root_win64`,
`installed_engine_root_win64`, and `visual_studio_root_win64`; capture and
conversion utilities additionally use the template's scratch/conversion keys.
On Mac, `source_engine_root_mac` names the patched UE 5.8 source checkout used
by guarded builds. `installed_engine_root_mac` names only the separate,
developer-baseline installed engine used for ordinary development; it must not
resolve to the source checkout and may be left unset when that baseline is not
used. The verifier reads the installed key only to reject accidental guarded
builds from that baseline.
`conversion_root` is the packaging conversion directory
(`build_stripped_pak.py --conv-dir`, default `<repo>/Intermediate/Conv`).
The gamma diagnostics under `Tools/test/gamma` instead read an identity
conversion; its optional key `identity_conversion_root` defaults to
`<scratch_root>/conv_identity`. Do not point `conversion_root` at it, or
packaging reads the wrong tree.
