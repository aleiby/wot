"""Bake procedural UE1 textures to runtime fractal records.

Usage: python3 bake_flipbooks.py <out_dir> [--retail-root DIR] [pkg ...]
Writes <out>/<Pkg>/_flipbooks.json (grid/frame dims, source params, runtime
records, unsupported spark types). Sheet PNGs are RETIRED:
the runtime simulates every fractal class and nothing reads a sheet.
AssetKey stays Name unless duplicate fractal leaves in one package require a
collision-free Group__Name key.
Fire, Wet (displaced source), Wave (phong-lit water), and Ice (glass-byte
source displacement) bake. Ice also emits <Name>.png at the serialized phase.
"""
import argparse
import base64
import json
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from wot_config import original_dir  # noqa: E402

import numpy as np
from PIL import Image

from fractalfire import bake
from fractalfire_exact import FireSim as ExactFireSim
from fractalice import bake as bake_ice
from fractalwater import (WaterSim, apply_wet, bake_wave, bake_wet,
                          build_wave_lut, global_random)
from ue1pkg import (FRACTAL_CLASSES, Package, decode_fractal, decode_palette,
                    decode_texture)

FRAMES, COLS = 32, 8

_PKG_CACHE = {}
RETAIL_ROOT = original_dir(required=False)


def sheet_grid_dims(frames):
    """Grid/frame dims for the manifest (import_map's pan frames read them).
    The sheet itself is retired — the sheet lane is gone and
    nothing reads a *_sheet.png anymore."""
    h, w = frames[0].shape[:2]
    rows = (FRAMES + COLS - 1) // COLS
    return w, h, rows


def open_package(name):
    if name not in _PKG_CACHE:
        for directory, suffix in (("Textures", ".utx"), ("System", ".u")):
            root = RETAIL_ROOT / directory
            candidate = root / f"{name}{suffix}"
            if not candidate.is_file() and root.is_dir():
                candidate = next(
                    (path for path in root.iterdir()
                     if path.is_file()
                     and path.name.lower() == f"{name}{suffix}".lower()),
                    candidate,
                )
            if candidate.is_file():
                _PKG_CACHE[name] = Package(candidate)
                break
        else:
            _PKG_CACHE[name] = None
    return _PKG_CACHE[name]


def _resolve_export(pkg, ref):
    """Resolve an object ref without dropping its UE1 group path."""
    if ref > 0:
        return pkg, ref, pkg.exports[ref - 1]
    if ref >= 0:
        return None, None, None
    full = pkg.objpath(ref)
    if not full or "." not in full:
        return None, None, None
    package_name, relative = full.split(".", 1)
    other = open_package(package_name)
    if other is None:
        return None, None, None
    for index, export in enumerate(other.exports, 1):
        if (other.objpath(index) or "").casefold() == relative.casefold():
            return other, index, export
    return None, None, None


def resolve_source(pkg, ref, seen=None):
    """SourceTexture objref -> texture-like bytes plus effective palette.

    Procedural sources retain their fractal payload so dependent Wet textures
    can be simulated instead of silently skipped.
    """
    target, index, export = _resolve_export(pkg, ref)
    if target is None:
        return None, None
    object_path = f"{target.name}.{target.objpath(index)}"
    seen = set() if seen is None else set(seen)
    key = object_path.casefold()
    if key in seen:
        raise ValueError(f"cyclic SourceTexture chain at {object_path}")
    seen.add(key)
    cls = target.objname(export["cls"]) or ""
    if cls == "Texture":
        src = decode_texture(target, export)
        pal_ref = src["palette"]
        pal = decode_palette(target, target.exports[pal_ref - 1]) \
            if pal_ref > 0 else None
    elif cls in ("FireTexture", "WetTexture", "WaveTexture", "IceTexture"):
        fractal = decode_fractal(target, export)
        width, height, data = fractal["mips"][0]
        src = dict(name=export["name"], w=width, h=height, data=data,
                   fractal=fractal)
        if cls == "WetTexture":
            _base, pal = resolve_source(
                target, fractal["props"].get("SourceTexture", 0), seen)
        elif cls == "IceTexture":
            _base, pal = resolve_source(
                target, fractal["props"].get("SourceTexture", 0), seen)
        else:
            pal_ref = fractal["props"].get("Palette", 0)
            pal = decode_palette(target, target.exports[pal_ref - 1]) \
                if pal_ref > 0 else None
    else:
        return None, None
    src["package"] = target.name
    src["_package_obj"] = target
    src["object_path"] = object_path
    src["class_name"] = cls
    src["_seen"] = seen
    return src, pal


class _FractalSourceOracle:
    """One exact constant-step source node for a dependent WetTexture bake."""

    def __init__(self, source, palette, rand, seen=None, construction=None):
        self.source = source
        self.palette = np.array(
            [color[:3] + (255,) for color in palette], dtype=np.uint8)
        fractal = source["fractal"]
        table = (build_wave_lut(fractal["props"])
                 if source["class_name"] == "WaveTexture" else None)
        if source["class_name"] == "FireTexture":
            self.sim = ExactFireSim(fractal, rand=rand)
        elif source["class_name"] in ("WetTexture", "WaveTexture"):
            self.sim = WaterSim(fractal, table=table, rand=rand)
        else:
            raise ValueError(
                f"unsupported procedural fallback source {source['class_name']}")
        source_indices = np.frombuffer(
            source["data"], dtype=np.uint8).reshape(source["h"], source["w"])
        self.current_rgba = self.palette[source_indices]
        self.parent_lock_time = np.float32(0.0)
        self.frame_delta = np.float32(1.0 / 32.0)
        self.child = None
        self.static_rgba = None
        if construction is None:
            construction = [0]
        if source["class_name"] == "WetTexture":
            child, child_palette = resolve_source(
                source["_package_obj"],
                fractal["props"].get("SourceTexture", 0), seen)
            if child is None or child_palette is None:
                raise ValueError(
                    f"unresolved source for {source['object_path']}")
            if "fractal" in child:
                self.child = _FractalSourceOracle(
                    child, child_palette, rand, child.get("_seen"),
                    construction)
            else:
                indices = np.frombuffer(child["data"], dtype=np.uint8).reshape(
                    child["h"], child["w"])
                child_rgba = np.array(
                    [color[:3] + (255,) for color in child_palette],
                    dtype=np.uint8)
                self.static_rgba = child_rgba[indices]
        # UE1's UTexture constructors sample appSeconds independently,
        # so a dependent texture's FIRST lock sees differing timestamps and
        # performs its Update/Prime immediately (UTexture::Lock). Children are
        # constructed above, before this stamp, mirroring the runtime port's
        # TextureConstructionEpsilon activation serials (source < parent). A
        # zero init made the first parent-time update a no-op and deferred the
        # source prime by one interval.
        construction[0] += 1
        self.last_update_time = np.float32(construction[0] * 1e-6)

    def update(self, current_time):
        """UTexture::Update at the exact lock time supplied by its parent."""
        current_time = np.float32(current_time)
        if current_time == self.last_update_time:
            return self.current_rgba
        old_time = self.last_update_time

        def before_step():
            if self.child is not None:
                self.child.update(old_time)

        self.sim.tick_texture(
            np.float32(current_time - old_time),
            before_step if self.child is not None else None)
        self.last_update_time = current_time
        if self.source["class_name"] == "WaveTexture":
            self.current_rgba = self.palette[self.sim.out]
        elif self.source["class_name"] == "FireTexture":
            indices = np.frombuffer(self.sim.output, dtype=np.uint8).reshape(
                self.source["h"], self.source["w"])
            self.current_rgba = self.palette[indices]
        else:
            child_rgba = (self.child.current_rgba
                          if self.child is not None else self.static_rgba)
            self.current_rgba = apply_wet(child_rgba, self.sim.out)
        return self.current_rgba

    def tick(self):
        # The parent locks at its old LastUpdateTime, then advances its own
        # time after the tick.  A 32-fps sheet uses 1/32 s render intervals.
        result = self.update(self.parent_lock_time)
        self.parent_lock_time = np.float32(
            self.parent_lock_time + self.frame_delta)
        return result


def _b64(data):
    return base64.b64encode(bytes(data)).decode("ascii")


def _drop_bytes(fractal):
    props = fractal["props"]
    raw = props.get("_structs", {}).get("Drops", {})
    count = max(0, min(int(props.get("NumDrops", 0)), 256))
    return [list(bytes(raw[i])[:8].ljust(8, b"\0"))
            for i in sorted(raw)[:count]]


def _spark_bytes(fractal):
    return [list(bytes(spark)[:8].ljust(8, b"\0"))
            for spark in fractal.get("sparks", [])]


def _palette_rgba(palette, masked=False):
    rgba = bytes(channel for index, color in enumerate(palette)
                 for channel in (color[0], color[1], color[2],
                                 0 if masked and index == 0 else 255))
    return _b64(rgba)


def _runtime_base(fractal, cls, palette, object_path, masked=False):
    props = fractal["props"]
    width, height, initial = fractal["mips"][0]
    return dict(
        class_name=cls, texture_name=fractal["name"],
        object_path=object_path, width=width, height=height,
        min_frame_rate=float(props.get("MinFrameRate", 0.0)),
        max_frame_rate=float(props.get("MaxFrameRate", 0.0)),
        prime_count=int(props.get("PrimeCount", 0)),
        wave_amp=int(props.get("WaveAmp", 0)),
        bump_map_light=int(props.get("BumpMapLight", 0)),
        bump_map_angle=int(props.get("BumpMapAngle", 0)),
        phong_range=int(props.get("PhongRange", 0)),
        phong_size=int(props.get("PhongSize", 0)),
        render_heat=int(props.get("RenderHeat", 0)),
        rising=bool(props.get("bRising", False)),
        sparks_limit=int(props.get("SparksLimit", 0)),
        palette_rgba=_palette_rgba(palette, masked),
        initial_bitmap=_b64(initial), sparks=_spark_bytes(fractal),
        drops=_drop_bytes(fractal),
        source_texture="", source_is_fractal=False,
        source_width=0, source_height=0, source_indices="",
        glass_texture="", glass_is_fractal=False,
        glass_width=0, glass_height=0, glass_indices="",
        panning_style=int(props.get("PanningStyle", 0)),
        time_method=int(props.get("TimeMethod", 0)),
        horiz_pan_speed=int(props.get("HorizPanSpeed", 0)),
        vert_pan_speed=int(props.get("VertPanSpeed", 0)),
        frequency=int(props.get("Frequency", 0)),
        amplitude=int(props.get("Amplitude", 0)),
        move_ice=bool(props.get("MoveIce", False)),
        master_count=float(props.get("MasterCount", 0.0)),
        u_displace=float(props.get("UDisplace", 0.0)),
        v_displace=float(props.get("VDisplace", 0.0)),
        u_position=float(props.get("UPosition", 0.0)),
        v_position=float(props.get("VPosition", 0.0)),
    )


def bake_package(path, out_root):
    pkg = Package(path)
    palettes = {i + 1: decode_palette(pkg, e)
                for i, e in enumerate(pkg.exports)
                if pkg.objname(e["cls"]) == "Palette"}
    out = Path(out_root) / Path(path).stem
    manifest, runtime, skipped = {}, {}, []
    fractal_leaf_counts = Counter(
        export["name"].casefold() for export in pkg.exports
        if pkg.objname(export["cls"]) in FRACTAL_CLASSES)
    for export_index, e in enumerate(pkg.exports, 1):
        cls = pkg.objname(e["cls"])
        if cls not in FRACTAL_CLASSES:
            continue
        relative_path = pkg.objpath(export_index)
        object_path = f"{pkg.name}.{relative_path}"
        # Preserve every existing leaf-name consumer.  Only a package-local
        # collision needs its group path encoded (WarrenT's two Rain3 exports).
        asset_key = (relative_path.replace(".", "__")
                     if fractal_leaf_counts[e["name"].casefold()] > 1
                     and "." in relative_path else e["name"])
        f = decode_fractal(pkg, e)
        if cls == "WetTexture":
            src_ref = f["props"].get("SourceTexture", 0)
            try:
                src, spal = resolve_source(pkg, src_ref)
            except Exception as ex:
                skipped.append(f"{object_path} (source: {ex})")
                continue
            if src is None or spal is None or not src["data"] or src["w"] == 0:
                skipped.append(f"{object_path} (unresolved/empty source)")
                continue
            sidx = np.frombuffer(src["data"], dtype=np.uint8).reshape(
                src["h"], src["w"])
            pal = np.array([c[:3] + (255,) for c in spal], dtype=np.uint8)
            source_oracle = None
            shared_rand = None
            if "fractal" in src:
                shared_rand = global_random()
                source_oracle = _FractalSourceOracle(
                    src, spal, shared_rand, src.get("_seen"))
            frames = bake_wet(
                f, sidx, pal[sidx], frames=FRAMES,
                source_oracle=source_oracle, rand=shared_rand)
            w, h, rows = sheet_grid_dims(frames)
            out.mkdir(parents=True, exist_ok=True)
            # Gameplay-created MIDs need a real imported texture in Tex before
            # UWoTFractalSubsystem swaps it for WoTLive_* on the next tick.
            # This is one representative frame, not the retired sheet lane.
            static_identity = {}
            if object_path.casefold() == "wotpawns.mashadar.mytex2":
                Image.fromarray(frames[0]).save(out / f"{asset_key}.png")
                static_identity = dict(
                    static_frame=f"{asset_key}.png",
                    static_phase="representative_first_frame")
            manifest[asset_key] = dict(
                w=w, h=h, frames=FRAMES, cols=COLS, rows=rows, fps=32,
                kind="wet", texture_name=e["name"],
                object_path=object_path, source=src["object_path"],
                drops=len(f["props"].get("_structs", {}).get("Drops", {})),
                **static_identity)
            record = _runtime_base(f, cls, spal, object_path)
            record.update(
                source_texture=src["object_path"],
                source_is_fractal="fractal" in src,
                source_width=src["w"], source_height=src["h"],
                source_indices=_b64(src["data"]),
            )
            runtime[asset_key] = record
            continue
        if cls == "WaveTexture":
            pal = palettes.get(f["props"].get("Palette", 0))
            if pal is None or not f["mips"]:
                skipped.append(f"{object_path} (no palette/mips)")
                continue
            pal_rgba = np.array([c[:3] + (255,) for c in pal], dtype=np.uint8)
            frames = bake_wave(f, pal_rgba, frames=FRAMES)
            w, h, rows = sheet_grid_dims(frames)
            out.mkdir(parents=True, exist_ok=True)
            manifest[asset_key] = dict(
                w=w, h=h, frames=FRAMES, cols=COLS, rows=rows, fps=32,
                kind="wave", texture_name=e["name"],
                object_path=object_path,
                drops=len(f["props"].get("_structs", {}).get("Drops", {})))
            record = _runtime_base(f, cls, pal, object_path)
            record.update(source_texture="", source_width=0, source_height=0,
                          source_is_fractal=False, source_indices="")
            runtime[asset_key] = record
            continue
        if cls == "IceTexture":
            src_ref = f["props"].get("SourceTexture", 0)
            glass_ref = f["props"].get("GlassTexture", 0)
            try:
                src, spal = resolve_source(pkg, src_ref)
                glass, _ = resolve_source(pkg, glass_ref)
            except Exception as ex:
                skipped.append(f"{object_path} (ice source/glass: {ex})")
                continue
            if (src is None or spal is None or glass is None
                    or not src["data"] or not glass["data"] or not f["mips"]):
                skipped.append(f"{object_path} (unresolved/empty ice source or glass)")
                continue
            target_w, target_h, _ = f["mips"][0]
            if ((src["w"], src["h"]) != (target_w, target_h)
                    or (glass["w"], glass["h"]) != (target_w, target_h)):
                skipped.append(f"{object_path} (ice source/glass size mismatch)")
                continue
            sidx = np.frombuffer(src["data"], dtype=np.uint8).reshape(
                src["h"], src["w"])
            gidx = np.frombuffer(glass["data"], dtype=np.uint8).reshape(
                glass["h"], glass["w"])
            try:
                frames = bake_ice(f, sidx, gidx, spal, frames=FRAMES, fps=32)
            except ValueError as ex:
                skipped.append(f"{object_path} (ice bake: {ex})")
                continue
            w, h, rows = sheet_grid_dims(frames)
            out.mkdir(parents=True, exist_ok=True)
            # The mesh-modulate consumer has no flipbook UV path yet. Its
            # representative static texture is the exact serialized phase.
            Image.fromarray(frames[0]).save(out / f"{asset_key}.png")
            manifest[asset_key] = dict(
                w=w, h=h, frames=FRAMES, cols=COLS, rows=rows, fps=32,
                kind="ice", texture_name=e["name"], object_path=object_path,
                source=src["object_path"], glass=glass["object_path"],
                panning_style=f["props"].get("PanningStyle", 0),
                time_method=f["props"].get("TimeMethod", 0),
                move_ice=bool(f["props"].get("MoveIce", False)),
                static_frame=f"{asset_key}.png",
                static_phase="authored_serialized")
            record = _runtime_base(
                f, cls, spal, object_path,
                masked=bool(f["props"].get("bMasked", False)))
            record.update(
                source_texture=src["object_path"],
                source_is_fractal="fractal" in src,
                source_width=src["w"], source_height=src["h"],
                source_indices=_b64(src["data"]),
                glass_texture=glass["object_path"],
                glass_is_fractal="fractal" in glass,
                glass_width=glass["w"], glass_height=glass["h"],
                glass_indices=_b64(glass["data"]),
            )
            runtime[asset_key] = record
            continue
        if cls != "FireTexture":
            skipped.append(f"{object_path} ({cls}: unsupported fractal class)")
            continue
        pal = palettes.get(f["props"].get("Palette", 0))
        if pal is None or not f["mips"]:
            skipped.append(f"{object_path} (no palette/mips)")
            continue
        frames, unsupported = bake(f, pal, frames=FRAMES)
        w, h, rows = sheet_grid_dims(frames)
        out.mkdir(parents=True, exist_ok=True)
        manifest[asset_key] = dict(
            w=w, h=h, frames=FRAMES, cols=COLS, rows=rows, fps=32,
            texture_name=e["name"], object_path=object_path,
            render_heat=f["props"].get("RenderHeat", 0),
            rising=bool(f["props"].get("bRising")),
            sparks=len(f.get("sparks", [])),
            unsupported_spark_types=unsupported)
        runtime[asset_key] = _runtime_base(f, cls, pal, object_path)
    if manifest or skipped:
        out.mkdir(parents=True, exist_ok=True)
        (out / "_flipbooks.json").write_text(
            json.dumps(dict(fire=manifest, runtime=runtime, skipped=skipped),
                       indent=1))
    return len(manifest), len(skipped)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("out_root", type=Path)
    parser.add_argument("packages", nargs="*", type=Path)
    parser.add_argument("--retail-root", type=Path,
                        help="retail tree used to resolve cross-package texture references")
    # packages is variadic, so use intermixed parsing to keep the documented
    # ``out --retail-root root package`` form accepted by the bootstrap.
    cli = parser.parse_intermixed_args()
    RETAIL_ROOT = (cli.retail_root.resolve() if cli.retail_root
                   else original_dir())
    out_root = cli.out_root
    args = cli.packages or (
        sorted((RETAIL_ROOT / "Textures").glob("*.utx"))
        + sorted((RETAIL_ROOT / "System").glob("*.u")))
    total = 0
    for p in args:
        try:
            n, sk = bake_package(p, out_root)
        except ValueError:
            continue
        if n or sk:
            print(f"{Path(p).name}: baked {n}, skipped {sk}")
        total += n
    print(f"TOTAL procedural flipbooks: {total}")
