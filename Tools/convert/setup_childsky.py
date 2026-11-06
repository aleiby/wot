"""Wire the child-scene sky for one map (validation) — direct camera-locked mesh.

Steps (in-editor headless):
  1. Import <Map>_skygeo.glb -> sky static mesh (lightmap pipeline, UV1 index 1).
  2. Bind each sky-mesh slot to the SAME material the WORLD mesh uses for that
     slot (the lit lightmap MIC), so the sky renders identically to the world.
     "SkyRest__*" slots (portal-unsealed sky zones; skygeo.py emits the
     rest-of-map surfaces retail's child frame draws THROUGH the sky's
     translucent portal sheets) bind the world MIC of the
     un-prefixed slot name — live geometry, so pan/animation stay live.
     "SkyRestFB" (nested sky windows: retail re-enters the sky child frame
     there) binds the hidden material — content behind them in the box is the
     closest equivalent of that recursion. "SkyRestBlock" (flipped-winding
     solid-space blockers) binds opaque black. On rest-maps, translucent sky
     slots are then rebound to per-map MICs of M_WoT_WorldT_SceneBlend —
     retail's per-channel dst factor over the now-bright background (see
     scene_blend_master; over black the committed max3 master is identical).
  3. Spawn AWoTSkyBox (camera-locked), assign the sky mesh + Rotation/Scale. (No
     origin property: skygeo.py re-centers the mesh at the sky-zone origin and the
     box pivots on the camera location — sky_origin in the JSON is report-only.)
  4. On the world mesh ACTOR (component override, non-destructive), hide:
       - FakeBackdrop window slots (name ends in "_Sky")
       - the sky-geometry slots (names == the sky mesh's slots), which live in the
         world mesh at their far position and would double-image the sky box.
  5. Save the level.

argv: setup_childsky.py <Map>[,<Map>...] [<conv_dir>]
Also exec'd by import_map.py after every map import (same argv contract),
so a re-import can no longer lose the childsky wiring.

This is the developer-authored map/front-end validation path and intentionally
keeps its direct AWoTSkyBox + hidden-sheet presentation. Guarded runtime maps
do not consume these level edits: WoTWorldMaterializer retains FakeBackdrop
geometry as opaque M_WoT_SkyWindow sheets and AWoTSkyBox captures each linked
child scene into its own live cube. Keeping this script direct also preserves
the front-end menu backdrop, whose AWoTSkyBox never enables runtime capture.
"""
import json
import math
import shutil
import struct
import sys
from pathlib import Path

import unreal

TOOLS_IMPORT = Path(unreal.Paths.convert_relative_path_to_full(
    unreal.Paths.project_dir())) / "Tools" / "import"
if str(TOOLS_IMPORT) not in sys.path:
    sys.path.insert(0, str(TOOLS_IMPORT))
TOOLS_ROOT = TOOLS_IMPORT.parent
if str(TOOLS_ROOT) not in sys.path:
    sys.path.insert(0, str(TOOLS_ROOT))
from wot_config import local_path  # noqa: E402
from light_wave_material import (  # noqa: E402
    WORLD_RESPONSE_GRAPH_TAG,
    WORLD_RESPONSE_GRAPH_VERSION,
    bind_response_parameters,
    build_response_lightmap,
    material_term_bindings,
)
from dynamic_light_material import (  # noqa: E402
    DYNAMIC_LIGHT_ENABLE_PARAMETER,
    DYNAMIC_LIGHT_GRAPH_TAG,
    DYNAMIC_LIGHT_GRAPH_VERSION,
    build_dynamic_world_factor,
    clear_material_graph,
)

MAPS = sys.argv[1].split(",") if len(sys.argv) > 1 else ["Battle2_02"]
SKYDIR = (Path(sys.argv[2]).resolve() if len(sys.argv) > 2
          else local_path("conversion_root"))

EAL = unreal.EditorAssetLibrary
MEL = unreal.MaterialEditingLibrary
AT = unreal.AssetToolsHelpers.get_asset_tools()
les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)


def hidden_material():
    """Developer-map direct-sky override; guarded runtime windows are opaque."""
    p = "/Game/WoT/Materials/M_WoT_SkyHidden"
    if EAL.does_asset_exist(p):
        return unreal.load_asset(p)
    m = AT.create_asset("M_WoT_SkyHidden", "/Game/WoT/Materials",
                        unreal.Material, unreal.MaterialFactoryNew())
    m.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    m.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    z = MEL.create_material_expression(m, unreal.MaterialExpressionConstant, -300, 0)
    z.set_editor_property("r", 0.0)
    MEL.connect_material_property(z, "", unreal.MaterialProperty.MP_OPACITY)
    EAL.save_asset(p)
    return m


def import_sky_mesh(MAP):
    dest = f"/Game/WoT/Maps/Sky/{MAP}"
    if EAL.does_directory_exist(dest):
        EAL.delete_directory(dest)
    params = unreal.ImportAssetParameters()
    params.is_automated = True
    params.override_pipelines = [unreal.SoftObjectPath(
        "/Game/WoT/Pipelines/IP_WoTMapLightmap")]
    mgr = unreal.InterchangeManager.get_interchange_manager_scripted()
    src = unreal.InterchangeManager.create_source_data(str(Path(SKYDIR) / f"{MAP}_skygeo.glb"))
    if not mgr.import_asset(dest, src, params):
        raise RuntimeError("sky mesh import failed")
    mgr.wait_until_all_tasks_done(False)
    EAL.save_directory(dest)
    meshes = [a for a in EAL.list_assets(dest)
              if isinstance(unreal.load_asset(a.split(".")[0]), unreal.StaticMesh)]
    if not meshes:
        raise RuntimeError("no sky mesh produced")
    mp = meshes[0].split(".")[0]
    sm = unreal.load_asset(mp)
    sm.set_editor_property("light_map_coordinate_index", 1)
    EAL.save_asset(mp)
    return mp


def scene_blend_master(animated=False):
    """Per-channel retail translucency for sky sheets over a SkyRest
    background: FB = Tex*M + SceneColor*M*(1-Tex), Opacity 1 — computed in
    the shader from an explicit SceneColor read. The committed world
    translucent master must scalarize UE1's per-channel dst factor
    M*(1-Tex) into Opacity (max3) — exact over black/gray, but over
    Battle3_02's bright dome the warm-M sheets transmitted G/B at the R
    rate (+16G/+23B sky band, R clean: max3 = the R factor;
    measured). SceneColor here = the scene before translucency, which
    holds the OPAQUE rest-of-map copy; sheets must not overlap each other
    (B3_02's are disjoint quadrants). MSM_THIN_TRANSLUCENT (dual-source
    blending, the native per-channel route) was TRIED AND REFUTED in this
    project's Forward+ config: it fell back to opacity-1 opaque (zero
    transmission, R p99 back to -41) — do not retry without engine work."""
    name = ("M_WoT_WorldWaveT_SceneBlend" if animated
            else "M_WoT_WorldT_SceneBlend")
    p = f"/Game/WoT/Materials/{name}"
    exists = EAL.does_asset_exist(p)
    current = unreal.load_asset(p) if exists else None
    response_tagged = (not animated) or (
        exists and EAL.get_metadata_tag(
            current, WORLD_RESPONSE_GRAPH_TAG) == WORLD_RESPONSE_GRAPH_VERSION)
    dynamic_tagged = exists and EAL.get_metadata_tag(
        current, DYNAMIC_LIGHT_GRAPH_TAG) == DYNAMIC_LIGHT_GRAPH_VERSION
    if exists and response_tagged and dynamic_tagged:
        return current
    if exists:
        m = current
        clear_material_graph(unreal, m)
    else:
        m = AT.create_asset(name, "/Game/WoT/Materials",
                            unreal.Material, unreal.MaterialFactoryNew())

    def expr(cls, x, y):
        return MEL.create_material_expression(m, cls, x, y)

    m.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    m.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    m.set_editor_property("two_sided", True)
    neutral = unreal.load_asset("/Game/WoT/Textures/WOTDetail/T_WoT_Neutral")

    def lin(node):
        node.set_editor_property("texture", neutral)
        node.set_editor_property(
            "sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
        return node

    # panned UVs — same PanU/PanV (UV/s) params as the world masters, so the
    # world MIC's values copy across verbatim.
    tc = expr(unreal.MaterialExpressionTextureCoordinate, -1750, -350)
    pu = expr(unreal.MaterialExpressionScalarParameter, -1750, -250)
    pu.set_editor_property("parameter_name", "PanU")
    pv = expr(unreal.MaterialExpressionScalarParameter, -1750, -170)
    pv.set_editor_property("parameter_name", "PanV")
    rate = expr(unreal.MaterialExpressionAppendVector, -1620, -220)
    MEL.connect_material_expressions(pu, "", rate, "A")
    MEL.connect_material_expressions(pv, "", rate, "B")
    t = expr(unreal.MaterialExpressionTime, -1620, -90)
    ofs = expr(unreal.MaterialExpressionMultiply, -1500, -170)
    MEL.connect_material_expressions(rate, "", ofs, "A")
    MEL.connect_material_expressions(t, "", ofs, "B")
    uv = expr(unreal.MaterialExpressionAdd, -1380, -290)
    MEL.connect_material_expressions(tc, "", uv, "A")
    MEL.connect_material_expressions(ofs, "", uv, "B")

    tex = lin(expr(unreal.MaterialExpressionTextureSampleParameter2D, -1100, 0))
    tex.set_editor_property("parameter_name", "Tex")
    MEL.connect_material_expressions(uv, "", tex, "UVs")
    lm = lin(expr(unreal.MaterialExpressionTextureSampleParameter2D, -1100, 300))
    lm.set_editor_property("parameter_name", "Lightmap")
    lm.set_editor_property("const_coordinate", 1)   # TEXCOORD_1 = atlas UVs
    lm_out, lm_pin = lm, "RGB"
    if animated:
        lm_out = build_response_lightmap(
            unreal, m, lm,
            unreal.load_asset("/Game/WoT/Materials/MPC_WoTLightWave"),
            lin, -1550, 300)
        lm_pin = ""
    lm2 = expr(unreal.MaterialExpressionMultiply, -900, 300)
    lm2.set_editor_property("const_b", 2.0)         # scale-2 atlas decode
    MEL.connect_material_expressions(lm_out, lm_pin, lm2, "A")
    light_factor = build_dynamic_world_factor(
        unreal, m, expr, lm2, "", -720, 300, lightmap_parameter=lm)

    prod = expr(unreal.MaterialExpressionMultiply, -700, 150)   # Tex*M
    MEL.connect_material_expressions(tex, "RGB", prod, "A")
    MEL.connect_material_expressions(light_factor, "", prod, "B")
    omt = expr(unreal.MaterialExpressionOneMinus, -700, -150)   # 1-Tex
    MEL.connect_material_expressions(tex, "RGB", omt, "")
    fdst = expr(unreal.MaterialExpressionMultiply, -500, -200)  # M*(1-Tex)
    MEL.connect_material_expressions(light_factor, "", fdst, "A")
    MEL.connect_material_expressions(omt, "", fdst, "B")
    sc = expr(unreal.MaterialExpressionSceneColor, -500, -350)
    dst = expr(unreal.MaterialExpressionMultiply, -300, -250)
    MEL.connect_material_expressions(sc, "", dst, "A")
    MEL.connect_material_expressions(fdst, "", dst, "B")
    out = expr(unreal.MaterialExpressionAdd, -150, 0)
    MEL.connect_material_expressions(prod, "", out, "A")
    MEL.connect_material_expressions(dst, "", out, "B")
    MEL.connect_material_property(out, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    one = expr(unreal.MaterialExpressionConstant, -150, 150)
    one.set_editor_property("r", 1.0)
    MEL.connect_material_property(one, "", unreal.MaterialProperty.MP_OPACITY)
    if animated:
        EAL.set_metadata_tag(
            m, WORLD_RESPONSE_GRAPH_TAG, WORLD_RESPONSE_GRAPH_VERSION)
    EAL.set_metadata_tag(
        m, DYNAMIC_LIGHT_GRAPH_TAG, DYNAMIC_LIGHT_GRAPH_VERSION)
    MEL.recompile_material(m)
    EAL.save_asset(p)
    return m


def black_material():
    """Opaque unlit black: the SkyRestBlock faces (skybake.py --rest
    'blockers') — one-sided surfaces retail's traversal meets from BEHIND,
    i.e. solid volume; these flipped copies occlude the rest-of-map content
    the same way solid space does in the child frame."""
    p = "/Game/WoT/Materials/M_WoT_SkyBlack"
    if EAL.does_asset_exist(p):
        return unreal.load_asset(p)
    m = AT.create_asset("M_WoT_SkyBlack", "/Game/WoT/Materials",
                        unreal.Material, unreal.MaterialFactoryNew())
    m.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    z = MEL.create_material_expression(m, unreal.MaterialExpressionConstant3Vector,
                                       -300, 0)
    MEL.connect_material_property(z, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    EAL.save_asset(p)
    return m


def world_slot_materials(MAP):
    """slot_name -> MaterialInterface for the map's world mesh(es), and the
    largest world-mesh bounding-sphere radius (UU) for sky-scale sizing.
    PF_Portal faces (water surfaces, and some maps' sky sheets: B3_02 Sky_439,
    A04/A07 SkyMod443) live in the sibling Geo/<MAP>_portals mesh — scan it
    too, or portal-only sky slots keep the GLB default material and render
    black (the Battle3_02 black-sky bug). world_r stays main-mesh-only: the
    portals mesh can include far sky sheets that would inflate SkyScale."""
    out = {}
    world_r = 0.0
    for a in EAL.list_assets(f"/Game/WoT/Maps/Geo/{MAP}"):
        obj = unreal.load_asset(a.split(".")[0])
        if isinstance(obj, unreal.StaticMesh):
            world_r = max(world_r, obj.get_bounds().sphere_radius)
            for sm in obj.static_materials:
                out[str(sm.material_slot_name)] = sm.material_interface
    if EAL.does_directory_exist(f"/Game/WoT/Maps/Geo/{MAP}_portals"):
        for a in EAL.list_assets(f"/Game/WoT/Maps/Geo/{MAP}_portals"):
            obj = unreal.load_asset(a.split(".")[0])
            if isinstance(obj, unreal.StaticMesh):
                for sm in obj.static_materials:
                    slot = str(sm.material_slot_name)
                    if not out.get(slot):
                        out[slot] = sm.material_interface
    return out, world_r


def glb_min_vert_dist(path):
    """Min vertex distance from the (re-centered) pivot, in UU (GLB is meters)."""
    with open(path, "rb") as f:
        f.read(12)
        clen, _ = struct.unpack("<II", f.read(8))
        gltf = json.loads(f.read(clen))
        blen, _ = struct.unpack("<II", f.read(8))
        blob = f.read(blen)
    mind = float("inf")
    for mesh in gltf["meshes"]:
        for prim in mesh["primitives"]:
            acc = gltf["accessors"][prim["attributes"]["POSITION"]]
            bv = gltf["bufferViews"][acc["bufferView"]]
            off = bv.get("byteOffset", 0) + acc.get("byteOffset", 0)
            stride = bv.get("byteStride", 12)
            for i in range(acc["count"]):
                x, y, z = struct.unpack_from("<fff", blob, off + i * stride)
                mind = min(mind, math.sqrt(x * x + y * y + z * z))
    return mind * 100.0


def sky_scale_for(sky, MAP, world_r):
    """SkyScale so the NEAREST sky SURFACE point sits beyond the deepest
    possible view ray through the world (bounds diameter): direction is
    scale-invariant, so oversizing only pushes the shell farther behind the
    world geometry. min_surface_dist (face distance) is the correct bound — a
    cloud sheet's interior passes far closer to the pivot than its vertices."""
    min_dist = (sky.get("min_surface_dist") or sky.get("min_vert_dist")
                or glb_min_vert_dist(str(Path(SKYDIR) / f"{MAP}_skygeo.glb")))
    return max(12.0, math.ceil(2.0 * world_r / min_dist * 1.25))


def setup_map(MAP, hidden):
    sky = json.load(open(str(Path(SKYDIR) / f"{MAP}_skygeo.json")))
    if not sky.get("sky_origin"):
        return "no-sky"
    sky_mesh_path = import_sky_mesh(MAP)
    sky_mesh = unreal.load_asset(sky_mesh_path)

    world_mat, world_r = world_slot_materials(MAP)
    scale = sky_scale_for(sky, MAP, world_r)
    sky_slots = set()
    matched = 0
    rest_n = 0
    for i, sm in enumerate(sky_mesh.static_materials):
        slot = str(sm.material_slot_name)
        if slot.startswith("SkyRest"):
            # rest-of-map copy (portal-unsealed zones; see module doc). NEVER
            # in sky_slots: these materials also exist on real world surfaces
            # by construction — the hide loop must not touch those.
            if slot == "SkyRestFB":
                sky_mesh.set_material(i, hidden)
            elif slot == "SkyRestBlock":
                sky_mesh.set_material(i, black_material())
            else:
                base = slot[len("SkyRest__"):]
                if world_mat.get(base):
                    sky_mesh.set_material(i, world_mat[base])
                else:
                    unreal.log_warning(
                        f"[childsky] {MAP}: SkyRest slot '{base}' has no world material")
            rest_n += 1
            continue
        sky_slots.add(slot)
        if slot in world_mat and world_mat[slot]:
            sky_mesh.set_material(i, world_mat[slot])
            matched += 1
    # Translucent sheets over a SkyRest background need retail's PER-CHANNEL
    # dst factor; over black (every map without rest content) the max3 world
    # master is numerically identical, so only rest-maps rebind.
    sb = 0
    if rest_n:
        atlas_document = None
        response_textures = None
        for i, sm in enumerate(sky_mesh.static_materials):
            slot = str(sm.material_slot_name)
            if slot.startswith("SkyRest"):
                continue
            cur = sky_mesh.get_material(i)
            if not isinstance(cur, unreal.MaterialInstanceConstant):
                continue
            par = cur.get_editor_property("parent")
            pname = par.get_name() if par else ""
            if pname not in ("M_WoT_WorldTranslucent",
                              "M_WoT_WorldWaveTranslucent"):
                continue
            animated = pname == "M_WoT_WorldWaveTranslucent"
            sbm = scene_blend_master(animated)
            mi = AT.create_asset(f"MI_SB_{slot}", f"/Game/WoT/Maps/Sky/{MAP}",
                                 unreal.MaterialInstanceConstant,
                                 unreal.MaterialInstanceConstantFactoryNew())
            mi.set_editor_property("parent", sbm)
            for pn in ("Tex", "Lightmap"):
                v = MEL.get_material_instance_texture_parameter_value(cur, pn)
                if v:
                    MEL.set_material_instance_texture_parameter_value(mi, pn, v)
            for pn in ("PanU", "PanV"):
                MEL.set_material_instance_scalar_parameter_value(
                    mi, pn,
                    MEL.get_material_instance_scalar_parameter_value(cur, pn))
            MEL.set_material_instance_scalar_parameter_value(
                mi, DYNAMIC_LIGHT_ENABLE_PARAMETER,
                MEL.get_material_instance_scalar_parameter_value(
                    cur, DYNAMIC_LIGHT_ENABLE_PARAMETER))
            if animated:
                if atlas_document is None:
                    atlas_path = Path(SKYDIR) / f"{MAP}_lmatlas.json"
                    if not atlas_path.is_file():
                        raise RuntimeError(
                            f"animated child sky lacks {atlas_path.name}")
                    atlas_document = json.loads(atlas_path.read_text())
                    response_textures = {}
                    bindings = material_term_bindings(
                        atlas_document["animation"])
                    for binding in bindings:
                        response_name = binding["response_png"]
                        asset_path = (
                            "/Game/WoT/Maps/Lightmaps/"
                            + Path(response_name).stem)
                        texture = unreal.load_asset(asset_path)
                        if not texture:
                            raise RuntimeError(
                                f"animated child sky lacks {asset_path}")
                        response_textures[response_name] = texture
                bind_response_parameters(
                    unreal, mi, atlas_document, response_textures)
            EAL.save_asset(f"/Game/WoT/Maps/Sky/{MAP}/MI_SB_{slot}")
            sky_mesh.set_material(i, mi)
            sb += 1
    EAL.save_asset(sky_mesh_path)

    les.load_level(f"/Game/WoT/Maps/{MAP}")

    # remove a prior SkyBox / debug control for idempotency
    for a in eas.get_all_level_actors():
        if a.get_actor_label() in (f"SkyBox_{MAP}", "SkyCtrl"):
            eas.destroy_actor(a)
    box = eas.spawn_actor_from_class(unreal.WoTSkyBox, unreal.Vector(0, 0, 0))
    box.set_actor_label(f"SkyBox_{MAP}")
    # Never let World Partition stream the camera-locked sky box out (it follows
    # the camera, so it has no fixed cell; streamed out it neither ticks nor is
    # found by the capture harness).
    try:
        box.set_editor_property("is_spatially_loaded", False)
    except Exception:
        pass
    box.static_mesh_component.set_static_mesh(sky_mesh)
    # sky_origin is report-only: the mesh is already re-centered by skygeo.py and the
    # box pivots on the view location (AWoTSkyBox has no origin property).
    ox, oy, oz = sky["sky_origin"]
    r = sky.get("sky_rotation", [0, 0, 0])
    s = 65536.0 / 360.0
    box.set_editor_property("sky_rotation",
                            unreal.Rotator(roll=r[2] / s, pitch=r[0] / s, yaw=r[1] / s))
    box.set_editor_property("sky_scale", scale)

    # Developer-map direct-sky path only: hide FakeBackdrop windows (_Sky) AND
    # the far sky-geometry slots. Guarded runtime maps instead keep windows as
    # opaque cube-sampling world sections (module doc), so they are not holes.
    # per-instance component overrides on each world StaticMeshActor. Slots
    # whose material is ALSO used by non-sky world surfaces (world_shared,
    # from skygeo.py) must NOT be hidden — slot-level hiding would strip real
    # world geometry (Arena_06/B2_03 regression). Since the __SkyZone slot
    # split (bsp2obj.sky_shared_split + skygeo, same rule), shared-material
    # sky faces live in their own suffixed slot and world_shared is empty by
    # construction — the guard stays as a safety net for pre-split meshes.
    shared = set(sky.get("world_shared", []))
    n = 0
    for a in eas.get_all_level_actors():
        # NEVER the sky box itself: its mesh's slots are all sky slots by
        # definition, so this loop would override every one of them with the
        # hidden material — an invisible sky box that still "renders" every
        # frame. That exact self-hide once masqueraded as an engine bug (the
        # per-tick camera-locked box was material-hidden, not update-starved).
        if isinstance(a, unreal.WoTSkyBox):
            continue
        smc = a.get_component_by_class(unreal.StaticMeshComponent)
        if not smc:
            continue
        m = smc.get_editor_property("static_mesh")
        if not m:
            continue
        for i, sm in enumerate(m.static_materials):
            slot = str(sm.material_slot_name)
            if (slot in sky_slots and slot not in shared) or slot.endswith("_Sky"):
                smc.set_material(i, hidden)
                n += 1
    les.save_current_level()
    return (f"ok(sky-mats {matched}/{len(sky_slots)}, hid {n} world slots, "
            f"shared-kept {sorted(shared)}, scale {scale}, rest-slots {rest_n}, "
            f"scene-blend {sb}, origin={[round(x) for x in (ox,oy,oz)]})")


# __main__-guarded so importers (e.g. regen_dynamic_light_masters) can reuse
# scene_blend_master without triggering the destructive per-map setup (the
# body deletes/rebuilds sky asset dirs and re-saves levels). The pythonscript
# commandlet runs this file as __main__, so the CLI contract is unchanged.
if __name__ == "__main__":
    hidden = hidden_material()
    res = {}
    for MAP in MAPS:
        try:
            res[MAP] = setup_map(MAP, hidden)
        except Exception as e:
            res[MAP] = f"ERROR {e}"
            unreal.log_error(f"[childsky] {MAP}: {e}")
    unreal.log(f"[childsky] DONE: {res}")
