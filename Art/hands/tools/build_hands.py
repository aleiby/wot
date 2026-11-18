#!/usr/bin/env python3
"""Regenerate the first-person hand deliverables from their committed sources.

For each class, from <root>/<class>/source/{<class>-meshy-512.glb, colours.json}:
  1. base colour texture (the source's embedded 512 bake),
  2. colour clusters (k-means, checked against hands.json expect_means),
  3. optional geometric sleeve mask (dump_faces.py + geometry_sleeve.py),
  4. team-colour variants + mask (tint_regions.py),
  5. long-edge subdivision of the bending region (densify.py + subdivide_hand.py),
  6. rig to the skeleton contract and export (hand_rig.py),
then copies <class>.glb, <class>_{base,black,blue,red,green,mask}.png and build.json into
<root>/<class>/. Needs python3 with numpy, Pillow, opencv-python, and Blender (env BLENDER).
usage: build_hands.py <Art/hands root> [class ...]
"""
import hashlib
import json
import os
import pathlib
import platform
import shutil
import subprocess
import sys

TOOLS = pathlib.Path(__file__).resolve().parent
BLENDER = os.environ.get("BLENDER", "blender")
COLOURS = ("black", "blue", "red", "green")


def run(cmd):
    result = subprocess.run([str(c) for c in cmd], capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit(f"FAILED: {' '.join(map(str, cmd))}\n{result.stdout[-2000:]}\n{result.stderr[-2000:]}")
    return result.stdout


def blender(script, *args):
    return run([BLENDER, "-b", "--factory-startup", "--python", TOOLS / script, "--", *args])


def sha(path):
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()


def build(root, cls, cfg):
    sys.path.insert(0, str(TOOLS))
    import hand_rig  # noqa: E402
    src_dir = root / cls / "source"
    src = src_dir / f"{cls}-meshy-512.glb"
    work = root / cls / ".work"
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    base = work / "base.png"
    base.write_bytes(hand_rig.read_glb(src)["png"])
    clusters = json.loads(run([sys.executable, TOOLS / "tint_regions.py", "clusters", base, work, 6]))
    means = [c["mean"] for c in clusters]
    if means != cfg["expect_means"]:
        sys.exit(f"{cls}: clusters changed {means} != {cfg['expect_means']}; review the tint/covering ids")
    tint = cfg["tint"]
    if "geometry_sleeve" in tint:
        blender("dump_faces.py", src, work / "faces.json")
        run([sys.executable, TOOLS / "geometry_sleeve.py", work / "faces.json", work / "labels.npy",
             tint["geometry_sleeve"]["trim_cluster"]])
    run([sys.executable, TOOLS / "tint_regions.py", "variants", base, work,
         ",".join(map(str, tint["clusters"])), src_dir / "colours.json"])
    covering = ",".join(map(str, cfg["covering"]))
    run([sys.executable, TOOLS / "densify.py", cls, src, work / "labels.npy", covering, work / "dense.json"])
    sub = blender("subdivide_hand.py", src, work / "dense.glb", work / "dense.json")
    rig_out = run([sys.executable, TOOLS / "hand_rig.py", "analyse", cls, work / "dense.glb", work / "labels.npy",
                   covering, work / "rig", base])
    out = root / cls
    shutil.copy(work / "rig" / f"{cls}.glb", out / f"{cls}.glb")
    shutil.copy(base, out / f"{cls}_base.png")
    for c in COLOURS:
        shutil.copy(work / f"basecolor-{c}.png", out / f"{cls}_{c}.png")
    shutil.copy(work / "mask.png", out / f"{cls}_mask.png")
    report = json.loads((work / "rig" / "rig-report.json").read_text())
    import numpy
    import PIL
    import cv2
    build_info = {
        "class": cls, "source_glb_sha256": sha(src), "colours_sha256": sha(src_dir / "colours.json"),
        "outputs_sha256": {p.name: sha(p) for p in sorted(out.iterdir()) if p.is_file() and p.suffix in (".glb", ".png")},
        "triangles": report["triangles"], "bones": report["bones"], "max_influences": report["max_influences"],
        "wrist_to_middle_tip_cm": report["wrist_to_middle_tip_cm"],
        "subdivision": next((line for line in sub.splitlines() if line.startswith("SUBDIVIDED")), ""),
        "tools": {"python": platform.python_version(), "numpy": numpy.__version__, "Pillow": PIL.__version__,
                  "opencv": cv2.__version__, "blender": run([BLENDER, "-b", "--version"]).splitlines()[0].strip(),
                  "scripts_sha256": {p.name: sha(p) for p in sorted(TOOLS.glob("*.py")) + [TOOLS / "hands.json"]}},
    }
    (out / "build.json").write_text(json.dumps(build_info, indent=1) + "\n")
    shutil.rmtree(work)
    print(f"{cls}: {report['triangles']} tris, {report['wrist_to_middle_tip_cm']} cm, {report['bones']} bones")
    del rig_out


def main():
    root = pathlib.Path(sys.argv[1]).resolve()
    config = json.loads((TOOLS / "hands.json").read_text())
    for cls in sys.argv[2:] or [k for k in config if not k.startswith("_")]:
        build(root, cls, config[cls])


if __name__ == "__main__":
    main()
