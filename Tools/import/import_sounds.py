"""Import extracted UE1 package sounds as SoundWave assets.

UnrealEditor-Cmd WoT.uproject -run=pythonscript
  -script="...import_sounds.py [--stripped --dest /Game/...]
           [<sounds_root>] [<pkg> ...]"

exports/sounds/<Pkg>/<Name>.wav (Tools/extract/extract_all.sh batchexport;
retail PCM WAV) → /Game/WoT/Sounds/<Pkg>/<Name>. The UE1 reference
Sound'Pkg.Group.Name' maps to /Game/WoT/Sounds/Pkg/Name — the exporter
writes per-package flat names, so the group folds away (C++ CDO soft paths
use the same rule). Pass package names to limit the import (default: all).

Every imported wave explicitly uses PCM cooking. Retail played these samples
without a lossy transcode, and the future payload-injection path supplies the
same PCM stream; leaving the assets on the project default would cook Bink and
make the canonical and injected paths differ.

The loop-flag derivation is stdlib-only (no `unreal`) so it can be dry-run
standalone: import this module and call looping_waves() directly.

The optional stripped mode creates metadata-only UWoTPayloadSoundWave assets. It
is OFF by default and can be selected by --stripped, --mode=stripped,
WOT_SOUND_IMPORT_MODE=stripped, or WOT_PAYLOAD_MODE=stripped. It also requires an
explicit --dest or WOT_PAYLOAD_CONTENT_ROOT outside /Game/WoT/Sounds. A
disposable isolated workspace may opt into that canonical virtual path only
with --allow-canonical; the ordinary developer Content tree must never do so.
"""
import os
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from wot_config import exports_dir  # noqa: E402

args = sys.argv[1:]
CANONICAL_SOUND_DESTINATION = "/Game/WoT/Sounds"


def stripped_mode_requested(arguments=None, environ=None):
    arguments = list(args if arguments is None else arguments)
    environ = os.environ if environ is None else environ
    return ("--stripped" in arguments or "--mode=stripped" in arguments
            or environ.get("WOT_SOUND_IMPORT_MODE", "").casefold() == "stripped"
            or environ.get("WOT_PAYLOAD_MODE", "").casefold() == "stripped")


def _option_value(arguments, option):
    """Return one --option value, accepting --option=value or two argv slots."""
    found = []
    index = 0
    while index < len(arguments):
        argument = arguments[index]
        if argument == option:
            if index + 1 >= len(arguments) or arguments[index + 1].startswith("--"):
                raise RuntimeError(f"{option} requires a value")
            found.append(arguments[index + 1])
            index += 2
            continue
        if argument.startswith(option + "="):
            found.append(argument.split("=", 1)[1])
        index += 1
    if len(found) > 1 and len(set(found)) != 1:
        raise RuntimeError(f"conflicting {option} values")
    return found[0] if found else None


def _positionals(arguments):
    """Positionals excluding values consumed by supported named options."""
    result = []
    index = 0
    while index < len(arguments):
        argument = arguments[index]
        if argument == "--dest":
            if index + 1 >= len(arguments):
                raise RuntimeError("--dest requires a value")
            index += 2
            continue
        if not argument.startswith("--"):
            result.append(argument)
        index += 1
    return result


def normalize_unreal_destination(destination):
    """Resolve separators and dot components in an Unreal /Game path."""
    if not isinstance(destination, str) or not destination.strip():
        raise RuntimeError("stripped sound destination must be a non-empty path")
    raw = destination.strip().replace("\\", "/")
    if not raw.startswith("/"):
        raise RuntimeError(
            f"stripped sound destination must be an absolute /Game path: {destination}")
    parts = []
    for component in raw.split("/"):
        if not component or component == ".":
            continue
        if component == "..":
            if not parts:
                raise RuntimeError(
                    f"stripped sound destination escapes its root: {destination}")
            parts.pop()
        else:
            parts.append(component)
    normalized = "/" + "/".join(parts)
    folded = normalized.casefold()
    if folded != "/game" and not folded.startswith("/game/"):
        raise RuntimeError(
            f"stripped sound destination must resolve under /Game: {destination}")
    return normalized


def validate_stripped_destination(destination, allow_canonical=False):
    """Reject canonical-tree stripped output unless explicitly overridden."""
    normalized = normalize_unreal_destination(destination)
    canonical = CANONICAL_SOUND_DESTINATION.casefold()
    folded = normalized.casefold()
    if (folded == canonical or folded.startswith(canonical + "/")) \
            and not allow_canonical:
        raise RuntimeError(
            "stripped sound destination resolves under canonical "
            f"{CANONICAL_SOUND_DESTINATION}; choose an isolated non-canonical "
            "--dest/WOT_PAYLOAD_CONTENT_ROOT or pass --allow-canonical only in "
            "a disposable workspace")
    return normalized


def stripped_destination(arguments=None, environ=None):
    """Resolve the required explicit stripped-output root."""
    arguments = list(args if arguments is None else arguments)
    environ = os.environ if environ is None else environ
    cli_destination = _option_value(arguments, "--dest")
    env_destination = environ.get("WOT_PAYLOAD_CONTENT_ROOT", "").strip() or None
    if cli_destination and env_destination:
        if (normalize_unreal_destination(cli_destination).casefold()
                != normalize_unreal_destination(env_destination).casefold()):
            raise RuntimeError(
                "--dest conflicts with WOT_PAYLOAD_CONTENT_ROOT")
    destination = cli_destination or env_destination
    if not destination:
        raise RuntimeError(
            "stripped sound import requires explicit --dest or "
            "WOT_PAYLOAD_CONTENT_ROOT")
    return validate_stripped_destination(
        destination, allow_canonical="--allow-canonical" in arguments)


STRIPPED = stripped_mode_requested()
positionals = _positionals(args)
ROOT = Path(positionals[0]) if positionals and "/" in positionals[0] \
    else exports_dir() / "sounds"
ONLY = {a for a in positionals if "/" not in a}
SCRIPT = ROOT.parent / "script"
T3D = ROOT.parent / "t3d"

SOUND_PAT = re.compile(r"Sound'([^'.]+)\.(?:[^'.]+\.)?([^'.]+)'")
AMBIENT_PAT = re.compile(r"AmbientSound\s*=\s*" + SOUND_PAT.pattern)
ASSIGN_PAT = re.compile(r"AmbientSound\s*=\s*([A-Za-z_]\w*)")
SOUND_COMPRESSION_PROPERTY = "sound_asset_compression_type"


def wav_pcm_metadata(wav):
    """Return the structural PCM metadata serialized into a stripped wave."""
    data = Path(wav).read_bytes()
    if len(data) < 12 or data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise RuntimeError(f"invalid WAV RIFF header: {wav}")
    if struct.unpack_from("<I", data, 4)[0] != len(data) - 8:
        raise RuntimeError(f"RIFF length disagrees with file size: {wav}")
    offset = 12
    fmt = None
    data_length = None
    while offset < len(data):
        if offset + 8 > len(data):
            raise RuntimeError(f"truncated WAV chunk header: {wav}")
        chunk_id = data[offset:offset + 4]
        chunk_size = struct.unpack_from("<I", data, offset + 4)[0]
        chunk_start = offset + 8
        chunk_end = chunk_start + chunk_size
        padded_end = chunk_end + (chunk_size & 1)
        if padded_end > len(data):
            raise RuntimeError(f"truncated WAV chunk: {wav}")
        if chunk_id == b"fmt ":
            if fmt is not None or chunk_size < 16:
                raise RuntimeError(f"duplicate or short WAV fmt chunk: {wav}")
            fmt = struct.unpack_from("<HHIIHH", data, chunk_start)
        elif chunk_id == b"data":
            if data_length is not None:
                raise RuntimeError(f"duplicate WAV data chunk: {wav}")
            data_length = chunk_size
        offset = padded_end
    if fmt is None or data_length is None:
        raise RuntimeError(f"WAV requires one fmt and one data chunk: {wav}")
    format_tag, channels, rate, byte_rate, block_align, bits = fmt
    expected_align = channels * (bits // 8)
    if (format_tag != 1 or not 1 <= channels <= 8
            or not 1 <= rate <= 384_000 or bits not in (8, 16)
            or block_align != expected_align
            or byte_rate != rate * block_align
            or data_length % block_align):
        raise RuntimeError(f"unsupported or inconsistent PCM WAV: {wav}")
    return {
        "channels": channels,
        "sample_rate": rate,
        "bits_per_sample": bits,
        "frame_count": data_length // block_align,
    }


def wav_smpl_loops(wav):
    """True if the WAV carries a 'smpl' chunk with >=1 sample loop. This is
    UE1's per-wave loop ground truth: the engine walked the RIFF chunks for
    'smpl' and played the wave looped when it held sample-loop entries (the
    smpl chunk's cSampleLoops count)."""
    data = wav.read_bytes()
    if len(data) < 12 or data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        return False
    pos = 12
    while pos + 8 <= len(data):
        size = int.from_bytes(data[pos + 4:pos + 8], "little")
        if data[pos:pos + 4] == b"smpl":
            # cSampleLoops is the 8th DWORD of the chunk (data offset 28).
            return size >= 32 and pos + 40 <= len(data) and \
                int.from_bytes(data[pos + 36:pos + 40], "little") > 0
        pos += 8 + size + (size & 1)  # chunks are 16-bit padded
    return False


def uc_ambient_refs(root):
    """(pkg, name) pairs a shipped class assigns to AmbientSound — direct
    Sound'' literals plus same-class variable refs (AmbientSound = DecaySound;
    resolved against the file's own DecaySound=Sound'Angreal.Decay.LeechDC').
    Resolution stays file-scoped: map-set arrays like TimedMover's Sounds[]
    can't be chased by name — DynamicAmbientSound's Sounds[] are one-shots."""
    refs = set()
    for uc in sorted(root.rglob("*.uc")):
        text = uc.read_text(errors="ignore")
        refs.update(AMBIENT_PAT.findall(text))
        for var in set(ASSIGN_PAT.findall(text)) - {"Sound", "None"}:
            for line in re.findall(rf"^\s*{var}\s*(?:\(\d+\))?\s*=.*$", text, re.M):
                refs.update(SOUND_PAT.findall(line))
    return refs


def t3d_ambient_refs(root):
    """(pkg, name) pairs from map-placed actors' AmbientSound= (also movers'
    MoveAmbientSound= — the pattern is unanchored on purpose), which UE1
    always played looped regardless of the wave's own chunk."""
    refs = set()
    for t3d in sorted(root.rglob("*.t3d")):
        refs.update(AMBIENT_PAT.findall(t3d.read_text(errors="ignore")))
    return refs


def looping_waves(sounds_root=None, script_root=None, t3d_root=None):
    """Waves to import with Looping=True: the wave's own 'smpl' loop chunk
    (per-wave ground truth — UE1 DID have one, contrary to what this used to
    claim), unioned with usage — AmbientSound refs from shipped class
    defaultproperties/code (.uc) and from map-placed actors (T3D)."""
    sounds_root = sounds_root or ROOT
    loops = {(w.parent.name, w.stem)
             for w in sounds_root.rglob("*.wav") if wav_smpl_loops(w)}
    loops |= uc_ambient_refs(script_root or SCRIPT)
    loops |= t3d_ambient_refs(t3d_root or T3D)
    return loops


def sync_wave_settings(wave, path, loop, pcm, save_asset):
    """Converge one imported wave and save it exactly once iff it changed.

    ``pcm`` and ``save_asset`` are supplied by main so this stays importable and
    unit-testable without Unreal's Python module. The compression property name
    is UE 5.8's Python reflection spelling of USoundWave's editable
    SoundAssetCompressionType UPROPERTY.
    """
    changed = set()
    desired = {
        "looping": loop,
        SOUND_COMPRESSION_PROPERTY: pcm,
    }
    for prop, value in desired.items():
        if wave.get_editor_property(prop) != value:
            wave.set_editor_property(prop, value)
            changed.add(prop)
    if changed:
        save_asset(path)
    return changed


def sync_stripped_wave_settings(wave, path, payload_key, loop, metadata,
                                save_asset):
    """Converge one metadata-only payload wave with one conditional save."""
    desired = {
        "payload_key": payload_key,
        "payload_channels": metadata["channels"],
        "payload_sample_rate": metadata["sample_rate"],
        "payload_bits_per_sample": metadata["bits_per_sample"],
        "payload_frame_count": metadata["frame_count"],
        "looping": loop,
    }
    changed = set()
    for prop, value in desired.items():
        if wave.get_editor_property(prop) != value:
            wave.set_editor_property(prop, value)
            changed.add(prop)
    if changed:
        save_asset(path)
    return changed


def prevalidate_stripped_targets(target_paths, asset_exists, load_asset,
                                 payload_wave_class):
    """Scan the entire selected target set before stripped mode may mutate it."""
    mismatches = []
    for path in target_paths:
        if not asset_exists(path):
            continue
        asset = load_asset(path)
        if not asset or not isinstance(asset, payload_wave_class):
            mismatches.append(path)
    if mismatches:
        raise RuntimeError(
            "stripped sound batch prevalidation found existing non-payload "
            "SoundWave/class mismatches; zero assets were written: "
            + ", ".join(mismatches))


def main():
    import unreal

    att = unreal.AssetToolsHelpers.get_asset_tools()
    eal = unreal.EditorAssetLibrary
    loops = looping_waves()
    pcm = unreal.SoundAssetCompressionType.PCM
    mode = "stripped" if STRIPPED else "full"
    destination_root = stripped_destination() if STRIPPED \
        else CANONICAL_SOUND_DESTINATION
    unreal.log(f"IMPORT-SOUNDS mode={mode} loop set: {len(loops)} waves")

    total = skipped = flagged = pcm_fixed = metadata_fixed = 0
    package_dirs = []
    for pkg_dir in sorted(ROOT.iterdir()):
        if not pkg_dir.is_dir() or (ONLY and pkg_dir.name not in ONLY):
            continue
        # SP-only content stays out of the MP port (user scope ruling):
        # DialogA is single-player dialog, referenced by zero MP maps or code.
        # The full 42-package extract sweeps it in since the pure-Python sound
        # exporter; filter it here rather than at extract (the extract stays a
        # faithful mirror of retail).
        if pkg_dir.name == "DialogA":
            skipped += 1
            continue
        package_dirs.append(pkg_dir)

    # Stripped mode is transactional with respect to predictable input/class
    # failures: validate every WAV and every existing target before the first
    # create, property mutation, import, or save.
    stripped_metadata = {}
    if STRIPPED:
        target_paths = []
        for pkg_dir in package_dirs:
            for wav in sorted(pkg_dir.glob("*.wav")):
                stripped_metadata[wav] = wav_pcm_metadata(wav)
                target_paths.append(
                    f"{destination_root}/{pkg_dir.name}/{wav.stem}")
        prevalidate_stripped_targets(
            target_paths, eal.does_asset_exist, unreal.load_asset,
            unreal.WoTPayloadSoundWave)

    for pkg_dir in package_dirs:
        dest = f"{destination_root}/{pkg_dir.name}"
        for wav in sorted(pkg_dir.glob("*.wav")):
            path = f"{dest}/{wav.stem}"
            loop = (pkg_dir.name, wav.stem) in loops
            if eal.does_asset_exist(path):
                skipped += 1
            else:
                if STRIPPED:
                    created = att.create_asset(
                        wav.stem, dest, unreal.WoTPayloadSoundWave, None)
                    if not created:
                        raise RuntimeError(
                            f"failed to create stripped payload wave: {path}")
                else:
                    task = unreal.AssetImportTask()
                    task.filename = str(wav)
                    task.destination_path = dest
                    task.automated = True
                    task.save = True
                    att.import_asset_tasks([task])
                total += 1
            # Two-way sync so re-runs converge on the derivation and PCM cook
            # policy. Existing assets are saved only when a setting differs;
            # correctly configured waves remain byte-untouched (SB-D10 lesson).
            w = unreal.load_asset(path)
            if not w:
                raise RuntimeError(f"failed to load imported sound wave: {path}")
            is_payload_wave = isinstance(w, unreal.WoTPayloadSoundWave)
            if is_payload_wave != STRIPPED:
                raise RuntimeError(
                    f"sound import mode/class mismatch at {path}; use an isolated "
                    f"generated-content workspace when switching payload modes")
            if STRIPPED:
                key = f"cache/sound/{pkg_dir.name}__{wav.stem}"
                changed = sync_stripped_wave_settings(
                    w, path, key, loop, stripped_metadata[wav], eal.save_asset)
                metadata_fixed += int(bool(changed - {"looping"}))
            else:
                changed = sync_wave_settings(
                    w, path, loop, pcm, eal.save_asset)
            if "looping" in changed:
                flagged += 1
            if SOUND_COMPRESSION_PROPERTY in changed:
                pcm_fixed += 1
        unreal.log(f"IMPORT-SOUNDS {pkg_dir.name}: done")

    unreal.log(f"IMPORT-SOUNDS-OK imported={total} skipped={skipped} "
               f"reflagged={flagged} pcm_fixed={pcm_fixed} "
               f"metadata_fixed={metadata_fixed} mode={mode}")


if __name__ == "__main__":
    main()
