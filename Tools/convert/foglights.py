"""Extract UE1 volumetric fog lights -> exports/fog/<Map>.fog.json.

The shipped map BAKES the volumetric set: each BSP leaf carries an
iVolumetric run into the model's Lights array (the renderer walks
Leaf.iVolumetric when the viewer stands in a bFogZone zone). We take that
authoritative per-leaf data — every light referenced by any leaf's
volumetric run — rather than re-deriving a qualification rule.

Per light we emit the parameters of the retail fog model:
  radius   world volumetric radius = 25*(VolumeRadius+1)
  color    display-space light color: FGetHSV(Hue,Sat,255) * Brightness/255
           * LevelInfo.Brightness * light-type wave mean (same composition
           as the surface lightmap color)
  vol_b    VolBrightness = wave * VolumeBrightness/64 (fog integral scale)
  vol_a    VolumeFog/255 (per-light occlusion alpha in the merge; 0 for all
           but 2 lights across the 19 MP maps)
The chord integral itself (parabolic density 3(1-r^2/R^2) along the clipped
eye ray, F = min(vol_b * chord, 1), contribution = min(2*color*F, 1),
per-channel screen blend vs the frame) is implemented by the M_WoT_VolumeFog
material pair the importer builds.

Usage: python foglights.py <Map.wot> <t3d.json> <out.json>
       python foglights.py --all   # all 19 MP maps -> exports/fog/
"""
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from lightmaps import (decode_model_full, fget_hsv, load_level,   # noqa: E402
                       point_zone, resolve_light, zone_actor_name,
                       _wave_scale, _find_uc)
from ue1pkg import Package  # noqa: E402
from wot_config import exports_dir, original_dir  # noqa: E402

REPO = Path(__file__).resolve().parents[2]

_VOL_PROPS = ("VolumeBrightness", "VolumeRadius", "VolumeFog")
_vol_cache = {}


def _vol_defaults(cls):
    """VolumeBrightness/Radius/Fog defaultproperties up the .uc chain.
    UE1 Light: VolumeBrightness=64, VolumeRadius=0; Actor: VolumeFog=0."""
    key = cls.lower()
    if key in _vol_cache:
        return _vol_cache[key]
    out = {"VolumeBrightness": 64, "VolumeRadius": 0, "VolumeFog": 0}
    chain, cur, guard = [], cls, 0
    while cur and guard < 20:
        path = _find_uc(cur)
        if not path:
            break
        text = path.read_text(errors="replace")
        chain.append(text)
        m = re.search(r"^\s*class\s+\w+\s+(?:expands|extends)\s+(\w+)",
                      text, re.M | re.I)
        cur = m.group(1) if m else None
        guard += 1
    for text in reversed(chain):        # base-most first, leaf overrides win
        for prop in _VOL_PROPS:
            m = re.search(rf"^\s*{prop}=(\d+)", text, re.M)
            if m:
                out[prop] = int(m.group(1))
    _vol_cache[key] = out
    return out


def extract(map_path, t3d_path):
    pkg, bsp = load_level(map_path)
    actors = {a["name"]: a for a in json.load(open(t3d_path))["actors"]}
    li = next(a for a in actors.values() if a["cls"] == "LevelInfo")
    level_brightness = float(li["props"].get("Brightness", 1.0))

    # 1) Every leaf's baked volumetric run (0-terminated in Lights[]) — the
    #    STATIC lights the editor qualified.
    lights_arr = bsp["lights"]
    names = set()
    for _izone, _iperm, ivol in bsp["leaves"]:
        if ivol is None or ivol < 0:
            continue
        i = ivol
        while i < len(lights_arr) and lights_arr[i] != 0:
            n = pkg.objname(lights_arr[i])
            if n:
                names.add(n)
            i += 1

    # 2) DYNAMIC lights qualify at runtime by the renderer's rule: the light's
    #    own zone is a fog zone AND VolumeRadius AND VolumeBrightness are
    #    non-zero (plus a live LightType/Brightness/Radius and the
    #    dynamic-actor gate). Torch flames etc. are non-static actors, so the
    #    leaf bake never contains them.
    fog_zone_actors = set()
    for a in actors.values():
        if a["props"].get("bFogZone") == "True":
            fog_zone_actors.add(a["name"])
    for a in actors.values():
        pr = a.get("props", {})
        if int(pr.get("VolumeRadius", 0) or 0) <= 0:
            continue
        if a["name"] in names:
            continue
        zn = zone_actor_name(pkg, bsp, point_zone(bsp, a["location"]))
        if zn in fog_zone_actors:
            names.add(a["name"])

    out = []
    for lname in sorted(names):
        if lname not in actors:
            continue
        rec = resolve_light(actors[lname])
        vol = _vol_defaults(rec["cls"])
        for prop in _VOL_PROPS:
            if prop in actors[lname]["props"]:
                vol[prop] = int(actors[lname]["props"][prop])
        if vol["VolumeBrightness"] <= 0:
            continue
        wave = _wave_scale(rec["LightType"], "mean")
        if rec["LightType"] == "LT_None" or rec["LightBrightness"] <= 0:
            continue
        color = (fget_hsv(rec["LightHue"], rec["LightSaturation"], 255)
                 * (rec["LightBrightness"] / 255.0) * level_brightness * wave)
        out.append(dict(
            name=lname,
            location=[float(c) for c in rec["location"]],
            radius=25.0 * (vol["VolumeRadius"] + 1),
            color=[round(float(c), 5) for c in color],
            vol_b=round(wave * vol["VolumeBrightness"] / 64.0, 5),
            vol_a=round(vol["VolumeFog"] / 255.0, 5),
        ))
    return out


def main():
    if sys.argv[1:2] == ["--all"]:
        maps = ["Arena_01", "Arena_02", "Arena_03", "Arena_04", "Arena_05",
                "Arena_06", "Arena_07", "Arena_08", "Arena_09", "Arena_11",
                "Arena_12", "Battle2_01", "Battle2_02", "Battle2_03",
                "Battle2_04", "Battle2_05", "Battle3_01", "Battle3_02",
                "Battle4_01"]
        conv = Path(sys.argv[2]) if len(sys.argv) > 2 else None
        outdir = exports_dir() / "fog"
        outdir.mkdir(parents=True, exist_ok=True)
        for m in maps:
            wot = original_dir() / "Maps" / f"{m}.wot"
            t3d = (conv / f"{m}.t3d.json") if conv else None
            lights = extract(str(wot), str(t3d))
            (outdir / f"{m}.fog.json").write_text(json.dumps(lights, indent=1))
            print(f"{m}: {len(lights)} fog light(s)")
    else:
        lights = extract(sys.argv[1], sys.argv[2])
        Path(sys.argv[3]).write_text(json.dumps(lights, indent=1))
        print(f"{len(lights)} fog light(s)")


if __name__ == "__main__":
    main()
