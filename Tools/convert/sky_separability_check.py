"""Can we cleanly separate sky-zone geometry from the world mesh?
For B2_02: list sky-zone surfaces (ids/flags/materials) and check whether each
sky material is used ONLY in the sky zone (=> slot exclusion is clean) or also
outside it (=> slot-level hiding would break world surfaces). Also flag whether
the FakeBackdrop windows are inside the sky-zone set.
NOTE: reports the RAW BSP sharing. Since the __SkyZone slot split
(bsp2obj.sky_shared_split + skygeo, same rule) shared materials are separable
anyway — their sky faces get their own suffixed slot in both converters."""
import json, sys
from collections import defaultdict, Counter
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))
from wot_config import local_path, original_dir  # noqa: E402
import lightmaps as L
import bsp2obj as B
import skygeo as SG
from ue1pkg import Package, PF_FAKEBACKDROP, PF_INVISIBLE

MAP = sys.argv[1] if len(sys.argv) > 1 else "Battle2_02"
conversion = (Path(sys.argv[2]).resolve() if len(sys.argv) > 2
              else local_path("conversion_root"))
wot = original_dir() / "Maps" / f"{MAP}.wot"
T3D = conversion / f"{MAP}.t3d.json"
tex_sizes = B.load_tex_sizes(conversion / "tex")

pkg, bsp = L.load_level(wot)
actors = json.load(open(T3D))["actors"]
sky_zone, winner = SG.sky_zone_of(pkg, bsp, actors)
sky_ids = SG.sky_surface_ids(bsp, sky_zone)
print(f"{MAP}: sky zone {sky_zone} ({winner['name']}), {len(sky_ids)} sky surfaces")

# material -> set of (surf in sky, surf outside sky)
mat_sky = defaultdict(set)
mat_world = defaultdict(set)
sky_mats = {}
for node in bsp["nodes"]:
    ivp, isurf, nv = node[0], node[1], node[2]
    if nv < 3:
        continue
    s = bsp["surfs"][isurf]
    if s["flags"] & PF_INVISIBLE:
        continue
    package, tex, w, h, ds = B.resolve_tex(pkg, s["tex"], tex_sizes)
    mat = Package.qualkey(package, tex) + B.flag_suffix(s["flags"])
    if isurf in sky_ids:
        mat_sky[mat].add(isurf)
        sky_mats[mat] = s["flags"]
    else:
        mat_world[mat].add(isurf)

print("\nsky-zone materials (fb=FakeBackdrop flag):")
shared = []
for mat, flags in sorted(sky_mats.items()):
    fb = "FB" if flags & PF_FAKEBACKDROP else "  "
    nworld = len(mat_world.get(mat, ()))
    tag = "  <-- ALSO IN WORLD" if nworld else ""
    if nworld:
        shared.append(mat)
    print(f"  {fb} {mat:34s} flags=0x{flags:06x} sky_surfs={len(mat_sky[mat])} world_surfs={nworld}{tag}")

print(f"\nclean-separable: {'NO' if shared else 'YES'}"
      + (f" (shared: {shared})" if shared else " — every sky material is sky-only"))
