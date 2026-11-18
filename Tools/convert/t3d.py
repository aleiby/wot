"""T3D → JSON: actor semantics for the import pipeline.

Usage: python3 t3d.py <Map.t3d> <out.json>
Every placed actor becomes {class, name, props} with Location/Rotation typed.
Brush geometry blocks are skipped (render geo comes from the compiled BSP).
Unknown properties pass through as raw strings so nothing is silently dropped.
"""
import json
import re
import sys

VEC = re.compile(r"\(([^)]*)\)")
COMP = re.compile(r"([A-Za-z]+)=([-+0-9.eE]+)")
FIELD = re.compile(r"([A-Za-z]+)=([^\s]+)")


def parse_tuple(val):
    m = VEC.search(val)
    if not m:
        return None
    return {k.lower(): float(v) for k, v in COMP.findall(m.group(1))}


def parse(path):
    actors = []
    actor = None
    depth = 0  # nested Begin blocks inside an actor (Brush, Polygon…)
    with open(path, encoding="latin-1") as source:
        for raw in source:
            line = raw.strip()
            if actor is None:
                if line.startswith("Begin Actor"):
                    m = re.match(r"Begin Actor Class=(\S+) Name=(\S+)", line)
                    actor = dict(cls=m.group(1), name=m.group(2), props={})
                    depth = 0
                continue
            if line.startswith("Begin "):
                depth += 1
                continue
            if line.startswith("End Actor"):
                actors.append(actor)
                actor = None
                continue
            if line.startswith("End "):
                depth -= 1
                continue
            if depth or "=" not in line:
                continue
            k, v = line.split("=", 1)
            actor["props"][k.strip()] = v.strip()
    return actors


def parse_brushes(path):
    """Parse nested Begin Brush/Polygon geometry for oracle verification.

    The production actor consumer intentionally ignores these blocks because
    render BSP comes from the package.  Keeping the geometry parser separate
    preserves that JSON contract while allowing the pure-Python Level exporter
    to verify every UModel/UPolys field against UCC.
    """
    actors = []
    actor = brush = polygon = None

    def vector(text):
        return [float(part) for part in text.split(",")]

    for raw in open(path, encoding="latin-1"):
        line = raw.strip()
        if line.startswith("Begin Actor"):
            match = re.match(r"Begin Actor Class=(\S+) Name=(\S+)", line)
            actor = {"cls": match.group(1), "name": match.group(2),
                     "brushes": []}
            actors.append(actor)
        elif actor is not None and line.startswith("Begin Brush"):
            fields = dict(FIELD.findall(line))
            brush = {"name": fields.get("Name", ""), "polygons": []}
            actor["brushes"].append(brush)
        elif brush is not None and line.startswith("Begin Polygon"):
            fields = dict(FIELD.findall(line))
            polygon = {
                "item": fields.get("Item"),
                "texture": fields.get("Texture"),
                "flags": int(fields.get("Flags", 0)),
                "link": int(fields.get("Link", -1)),
                "origin": [], "normal": [], "texture_u": [],
                "texture_v": [], "pan": [0, 0], "vertices": [],
            }
            brush["polygons"].append(polygon)
        elif polygon is not None and line.startswith("Origin"):
            polygon["origin"] = vector(line.split(None, 1)[1])
        elif polygon is not None and line.startswith("Normal"):
            polygon["normal"] = vector(line.split(None, 1)[1])
        elif polygon is not None and line.startswith("TextureU"):
            polygon["texture_u"] = vector(line.split(None, 1)[1])
        elif polygon is not None and line.startswith("TextureV"):
            polygon["texture_v"] = vector(line.split(None, 1)[1])
        elif polygon is not None and line.startswith("Vertex"):
            polygon["vertices"].append(vector(line.split(None, 1)[1]))
        elif polygon is not None and line.startswith("Pan"):
            fields = dict(FIELD.findall(line))
            polygon["pan"] = [int(fields.get("U", 0)), int(fields.get("V", 0))]
        elif line.startswith("End Polygon"):
            polygon = None
        elif line.startswith("End Brush"):
            polygon = brush = None
        elif line.startswith("End Actor"):
            actor = brush = polygon = None
    return actors


def typed(actors):
    out = []
    for a in actors:
        p = dict(a["props"])
        loc = parse_tuple(p.pop("Location", "")) or {}
        rot = parse_tuple(p.pop("Rotation", "")) or {}
        out.append(dict(
            cls=a["cls"], name=a["name"],
            location=[loc.get("x", 0.0), loc.get("y", 0.0), loc.get("z", 0.0)],
            rotation=[rot.get("pitch", 0.0), rot.get("yaw", 0.0), rot.get("roll", 0.0)],
            props=p))
    return out


if __name__ == "__main__":
    actors = typed(parse(sys.argv[1]))
    hist = {}
    for a in actors:
        hist[a["cls"]] = hist.get(a["cls"], 0) + 1
    json.dump(dict(actors=actors, histogram=hist), open(sys.argv[2], "w"), indent=1)
    print(f"{sys.argv[1].rsplit('/',1)[-1]}: {len(actors)} actors, "
          f"{len(hist)} classes; lights={hist.get('Light',0)} "
          f"starts={hist.get('PlayerStart',0)}")
