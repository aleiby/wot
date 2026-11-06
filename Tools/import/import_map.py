"""Build /Content for one converted map: textures → master material + instances
→ static mesh (OBJ) → level with mesh + light + PlayerStart. Headless:

UnrealEditor-Cmd WoT.uproject -run=pythonscript -script="...import_map.py <Map> <conv_dir> [mode]"
  <conv_dir> = bsp2obj/utx2png output root (expects <Map>.obj + tex/<Pkg>/*.png)
  [mode]     = "preview" (default: movable shadowless lights, no bake needed)
               or "baked" (static shadow-casting lights for the Lightmass pass)

``--stripped`` / ``--mode=stripped`` (or WOT_TEXTURE_IMPORT_MODE /
WOT_PAYLOAD_MODE) runs only the retail static-texture stage.  It requires an
explicit isolated ``--dest`` / WOT_PAYLOAD_CONTENT_ROOT and never touches map,
material, or canonical texture assets.

``--bsp-stripped`` is a full-map import mode for multiplayer maps. It replaces
only the split-aware main/portal BSP GLB parts with generated tiny carriers and
stamps their authenticated runtime payload descriptors. ``--bsp-proof-carrier``
remains an Arena_07 alias for this path.
"""
import json
import os
import re
import sys
from collections import Counter
from pathlib import Path

import unreal

IMPORT_DIR = Path(__file__).resolve().parent
if str(IMPORT_DIR) not in sys.path:
    sys.path.insert(0, str(IMPORT_DIR))
from spawn_pool_defaults import (  # noqa: E402
    ANGREAL_CLASSMAP,
    POOL_DEFAULTS,
    _PORTED_ANGREAL,
    pool_entries as _effective_pool_entries,
    pool_unported,
)
from payload_binding import ensure_texture_payload_binding
from payload_binding import full_mip_count
from import_transaction import run_isolated_import_transaction
from stripped_texture_import import (
    import_stripped_texture,
    importer_positionals,
    png_dimensions,
    prevalidate_stripped_textures,
    stripped_destination,
    stripped_mode_requested,
    validate_png,
)
from mesh_material_types import (
    GAMMA_SLOT_MASTERS,
    IMPORT_SLOT_MASTERS,
    actor_style_suffix,
    effective_material_type_suffix,
    material_type_suffix,
    slot_zero_skin_reference,
    split_slot_name,
)
from light_wave_material import (
    WORLD_RESPONSE_GRAPH_TAG,
    WORLD_RESPONSE_GRAPH_VERSION,
    bind_response_parameters,
    build_animated_scene_blend_graph,
    build_response_lightmap,
)
from dynamic_light_material import (
    DYNAMIC_LIGHT_ENABLE_PARAMETER,
    DYNAMIC_LIGHT_GRAPH_TAG,
    DYNAMIC_LIGHT_GRAPH_VERSION,
    build_dynamic_surface_sum,
    build_dynamic_world_factor,
    clear_material_graph,
    ensure_dynamic_light_collection,
)
from world_fractal_split import (
    record_bearing_slots,
    runtime_fractal_texture_keys,
    split_world_glb,
)
import mesh_master_bootstrap

_SOUND_REF_RE = re.compile(r"Sound'([^'.]+)\.(?:[^'.]+\.)?([^'.]+)'")


def sound_subpath(value):
    """Sound'Pkg.Group.Name' -> Pkg/Name, matching sound import folding."""
    match = _SOUND_REF_RE.search(str(value or ""))
    return f"{match.group(1)}/{match.group(2)}" if match else None

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "Tools"))
from wot_config import exports_dir  # noqa: E402
EXPORTS = Path(os.environ.get("WOT_EXPORTS_DIR", exports_dir(required=False)))
TOOLS_CONVERT = REPO / "Tools" / "convert"
if str(TOOLS_CONVERT) not in sys.path:
    sys.path.insert(0, str(TOOLS_CONVERT))
from bsp_linecheck import BspTree  # noqa: E402
from light_animation import (  # noqa: E402
    MPC_SCALAR_DEFAULTS,
    WAVEFORM_SHAPES,
    light_wave_group_selectors,
    light_wave_driver_groups,
    material_term_bindings,
)
from lightmaps import resolve_light, world_light_radius  # noqa: E402
from mesh_light_select import select_mesh_lights  # noqa: E402
from mover_light_select import (  # noqa: E402
    select_mover_surface_lights,
    transform_plane,
)
from mesh_texture_variants import (  # noqa: E402
    is_keyed_texture_name,
    palette0_rgb,
)
from bsp_payload import write_stripped_carrier  # noqa: E402
from bsp_payload import split_bsp_glb_parts  # noqa: E402

RAW_ARGS = sys.argv[1:]
POSITIONALS = importer_positionals(RAW_ARGS)
BSP_PROOF_CARRIER = "--bsp-proof-carrier" in RAW_ARGS
BSP_STRIPPED = "--bsp-stripped" in RAW_ARGS or BSP_PROOF_CARRIER
CANONICAL_TEXTURE_DESTINATION = "/Game/WoT/Textures"
_TEXTURE_MODE_ENV = os.environ
if "--bsp-stripped" in RAW_ARGS \
        and os.environ.get("WOT_PAYLOAD_MODE", "").casefold() == "stripped":
    # import_all's composed payload mode selects BSP stripping for the maps
    # step, but this leaf must otherwise perform the canonical full-map import.
    _TEXTURE_MODE_ENV = dict(os.environ)
    _TEXTURE_MODE_ENV.pop("WOT_PAYLOAD_MODE", None)
STRIPPED = stripped_mode_requested(RAW_ARGS, _TEXTURE_MODE_ENV)
TEXTURE_DESTINATION = stripped_destination(
    RAW_ARGS, CANONICAL_TEXTURE_DESTINATION, "map texture") \
    if STRIPPED else CANONICAL_TEXTURE_DESTINATION
MAP = POSITIONALS[0] if POSITIONALS else "Arena_01"
CONV = Path(POSITIONALS[1] if len(POSITIONALS) > 1 else "/tmp/wotconv")
MODE = POSITIONALS[2] if len(POSITIONALS) > 2 else "preview"
assert MODE in ("preview", "baked"), f"bad mode {MODE}"
# Global light-intensity multiplier (calibrated against the golden set —
# exports/reference/compare). Overridable for calibration sweeps.
MULT = float(POSITIONALS[3]) if len(POSITIONALS) > 3 else 5.0
if BSP_STRIPPED:
    if STRIPPED:
        raise RuntimeError("BSP-stripped import cannot be combined with texture --stripped")
if BSP_PROOF_CARRIER:
    if MAP != "Arena_07":
        raise RuntimeError("the BSP carrier proof is deliberately scoped to Arena_07")
# Reference-A twin of the carrier's CPU flags: the packaged proof harness
# reads LOD0 vertex/index buffers on the CPU, so the canonical Arena_07
# world/portal meshes must cook with bAllowCPUAccess/NeverStream exactly like
# the StampCarrier path (WoTBSPPayload.cpp). Geometry is untouched.
BSP_PROOF_CPU = "--bsp-proof-cpu" in RAW_ARGS
if BSP_PROOF_CPU:
    if BSP_STRIPPED:
        raise RuntimeError("--bsp-proof-cpu is the reference-A flag; the carrier already sets CPU access")
    if MAP != "Arena_07":
        raise RuntimeError("the BSP proof reference is deliberately scoped to Arena_07")

ATT = unreal.AssetToolsHelpers.get_asset_tools()
EAL = unreal.EditorAssetLibrary

# Per-texture export info (sizes + UE1 DetailTexture/MacroTexture refs).
# TEXINFO keyed by (package, name) — package == export folder — so same-named
# textures in different UE1 packages never alias. NEVER key by bare name.
TEXINFO = {}
for _man in (CONV / "tex").glob("*/_manifest.json"):
    _pkg = _man.parent.name
    for _tn, _ti in json.loads(_man.read_text())["textures"].items():
        TEXINFO[(_pkg, _tn)] = _ti

# Canonical export-folder case for a package (UE1 names are case-insensitive but
# t3d refs disagree with .utx stems, e.g. WOTDecorations vs WoTDecorations). Maps
# a lowercased package to the folder actually present in the conv tree.
_PKG_CANON = {}
for (_p, _n) in TEXINFO:
    _PKG_CANON.setdefault(_p.lower(), _p)


def _tkey(package, name):
    """Canonicalize a package-qualified (package, name) ref to the TEXINFO key
    (export-folder case). Bare/empty package stays empty → fail-closed lookup."""
    return (_PKG_CANON.get(package.lower(), package) if package else "", name)


def _slug(tkey):
    """(package, name) → 'Package__Name' — a filesystem/asset-safe, aliasing-free
    slug for naming derived assets (MICs) after a package-qualified texture."""
    return f"{tkey[0]}__{tkey[1]}"


def _stripped_texture_specs(needed):
    specs = []
    for ref in sorted(needed, key=lambda item: str(item).casefold()):
        if not isinstance(ref, tuple) or len(ref) != 2 or not ref[0] or not ref[1]:
            continue
        package, name = ref
        png = CONV / "tex" / package / f"{name}.png"
        if not png.is_file():
            continue
        info = TEXINFO.get((package, name), {})
        width = int(info.get("w", 0))
        height = int(info.get("h", 0))
        if width <= 0 or height <= 0:
            width, height = png_dimensions(png)
        specs.append({
            "source_png": png,
            "asset_path": f"{TEXTURE_DESTINATION}/{package}/{name}",
            "package": package,
            "source_name": name,
            "width": width,
            "height": height,
            "destination_mip_count": full_mip_count(width, height),
        })
    return specs


def _import_stripped_texture_specs(specs):
    found = {}
    for spec in specs:
        import_stripped_texture(
            unreal, spec,
            (("mip_gen_settings",
              unreal.TextureMipGenSettings.TMGS_FROM_TEXTURE_GROUP),))
        found[(spec["package"], spec["source_name"])] = spec["asset_path"]
    return found


def import_textures(needed):
    """Import the exact (package, name) textures the map references, from
    tex/<package>/<name>.png → /Game/WoT/Textures/<package>/<name>. Returns
    {(package, name): asset_path}. Package-qualified and deterministic — no
    rglob-by-stem, no bare-name aliasing."""
    if STRIPPED:
        specs = _stripped_texture_specs(needed)
        prevalidate_stripped_textures(
            unreal, specs, EAL.does_asset_exist, unreal.load_asset)
        return _import_stripped_texture_specs(specs)

    found = {}
    for ref in sorted(needed, key=lambda item: str(item).casefold()):
        if not isinstance(ref, tuple) or len(ref) != 2 or not ref[0] or not ref[1]:
            continue   # defensive: skip any malformed/unqualified ref (fail-closed)
        package, name = ref
        dest = f"{CANONICAL_TEXTURE_DESTINATION}/{package}"
        full = f"{dest}/{name}"
        if not EAL.does_asset_exist(full):
            png = CONV / "tex" / package / f"{name}.png"
            if not png.exists():
                continue
            task = unreal.AssetImportTask()
            task.filename = str(png)
            task.destination_path = dest
            task.automated = True
            task.save = True
            ATT.import_asset_tasks([task])
        ensure_texture_payload_binding(unreal, full, package, name)
        found[(package, name)] = full
    return found


def _panned_uv(m, x=-1750, y=-350):
    """UV0 + Time*(PanU,PanV): UE1 PF_AutoU/VPan scrolls surfaces at
    35*Zone.TexU/VPanSpeed texels/sec (the 0x3ffff wrap
    is a no-op for tiling pow-2 textures). PanU/PanV params are in UV/sec
    (importer divides by texture size); default 0 = static."""
    MEL = unreal.MaterialEditingLibrary

    def expr(cls, xx, yy):
        return MEL.create_material_expression(m, cls, xx, yy)

    tc = expr(unreal.MaterialExpressionTextureCoordinate, x, y)
    pu = expr(unreal.MaterialExpressionScalarParameter, x, y + 100)
    pu.set_editor_property("parameter_name", "PanU")
    pu.set_editor_property("default_value", 0.0)
    pv = expr(unreal.MaterialExpressionScalarParameter, x, y + 180)
    pv.set_editor_property("parameter_name", "PanV")
    pv.set_editor_property("default_value", 0.0)
    rate = expr(unreal.MaterialExpressionAppendVector, x + 130, y + 130)
    MEL.connect_material_expressions(pu, "", rate, "A")
    MEL.connect_material_expressions(pv, "", rate, "B")
    t = expr(unreal.MaterialExpressionTime, x + 130, y + 260)
    ofs = expr(unreal.MaterialExpressionMultiply, x + 250, y + 180)
    MEL.connect_material_expressions(rate, "", ofs, "A")
    MEL.connect_material_expressions(t, "", ofs, "B")
    uv = expr(unreal.MaterialExpressionAdd, x + 370, y + 60)
    MEL.connect_material_expressions(tc, "", uv, "A")
    MEL.connect_material_expressions(ofs, "", uv, "B")
    return uv


# ---------------------------------------------------------------------------
# GAMMA-SPACE EXPERIMENT (Battle3_01 falsification run): render the whole
# frame in display space, like UE1/Glide. Textures sample raw (srgb off),
# all shader math and HARDWARE BLENDING operate on display-encoded values
# (stacked additive/translucent accumulation becomes UE1-exact — the class
# that is unfixable in linear), and one boundary pass at the tonemapper
# (M_WoT_GammaBoundary: exact sRGB decode after the configured display ramp)
# cancels the output device's sRGB encode.
# All pow-2.2 transport nodes and constants below are gated off with the
# flag; lit MESH materials are knowingly wrong while it is on.
GAMMA_SPACE = True


def _lin_sampler(node):
    """Mark a sample node linear so raw display bytes pass through
    unconverted (the paired texture assets have srgb=False). The node also
    needs a LINEAR default texture: with none, the compiler validates the
    sampler against the engine's sRGB DefaultTexture and FAILS the whole
    material ("Sampler type is Linear Color, should be Color") — every
    surface then renders with the black Default Material."""
    if GAMMA_SPACE:
        node.set_editor_property("texture", _import_neutral())
        node.set_editor_property(
            "sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
    return node


def _make_world_blend_master(name, modulated, animated=False):
    """UE1 blended world surfaces (the SINGLE-TMU renderer model: blend setup
    + the unconditional lightmap modulate2x pass over the already-blended
    framebuffer):
      translucent: FB = M*(Tex + Dst*(1-Tex))   [ONE / ONE_MINUS_SRC_COLOR]
      modulated:   FB = M*2*Tex*Dst             [DST_COLOR / SRC_COLOR]
    with M = 2x the atlas sample (the scale-2 atlas decode, lightmap_atlas.py
    contract; fullbright texel 128 -> M ~ 1 for unlit/no-lightmap surfaces,
    so the same masters serve lit water and unlit fire/sky quads).
    Display-space math survives UE5's linear-space blending via pow-2.2
    transport: products are exact; the translucent dst factor M*(1-Tex) is
    per-channel in UE1 and must scalarize for UE5's Opacity — max3 is exact
    for gray media, and over BRIGHT backgrounds it measurably over-transmits
    the non-max channels (B3_02 SkyRest dome: +16G/+23B). Sky-box
    translucent slots on rest-maps therefore rebind to M_WoT_WorldT_SceneBlend
    (setup_childsky.py: per-channel algebra via an explicit SceneColor read);
    a GLOBAL migration of this master is queued, needing its own 19-map A/B
    (PORT_NOTES "Translucent blending").
    TESTED AND REFUTED (do not retry): the
    MULTI-TMU one-pass model (Glide driver, dst factor 1 - Tex*M,
    Opacity = min3(Tex*M)). Identical at M=1, it changes only LIT
    translucency — and every touched map REGRESSED vs golden (19-map
    scoreboard: B2_01 +0.93 bad% — PS5 pit water brightened PAST golden;
    B2_02/B3_01 +0.50, A11 +0.22). The goldens' Glide rig renders the
    SINGLE-TMU surface path for translucency; the opaque 4f/4g overbright
    evidence never discriminated TMU count (both paths give T*M for
    opaque — the translucent dst factor is the only discriminator)."""
    full = f"/Game/WoT/Materials/{name}"
    exists = EAL.does_asset_exist(full)
    current = unreal.load_asset(full) if exists else None
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
        m = ATT.create_asset(name, "/Game/WoT/Materials", unreal.Material,
                             unreal.MaterialFactoryNew())
    _build_world_blend_graph(m, modulated, animated)
    if animated:
        EAL.set_metadata_tag(
            m, WORLD_RESPONSE_GRAPH_TAG, WORLD_RESPONSE_GRAPH_VERSION)
    EAL.set_metadata_tag(
        m, DYNAMIC_LIGHT_GRAPH_TAG, DYNAMIC_LIGHT_GRAPH_VERSION)
    unreal.MaterialEditingLibrary.recompile_material(m)
    EAL.save_asset(full)
    return m


def _build_world_blend_graph(m, modulated, animated=False):
    """Expression graph for a world blend master (see _make_world_blend_master
    for the model). Split out so rebuild scripts can regenerate the graph on
    the existing asset (clear_material_graph + this) without
    breaking MIC parent references."""
    MEL = unreal.MaterialEditingLibrary

    def expr(cls, x, y):
        return MEL.create_material_expression(m, cls, x, y)

    m.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    m.set_editor_property("two_sided", True)
    tex = _lin_sampler(expr(unreal.MaterialExpressionTextureSampleParameter2D, -450, 0))
    tex.set_editor_property("parameter_name", "Tex")
    MEL.connect_material_expressions(_panned_uv(m), "", tex, "UVs")
    lm = _lin_sampler(expr(unreal.MaterialExpressionTextureSampleParameter2D, -450, 300))
    lm.set_editor_property("parameter_name", "Lightmap")
    lm.set_editor_property("const_coordinate", 1)   # TEXCOORD_1 = atlas UVs
    lm_out, lm_pin = lm, "RGB"
    if animated:
        lm_out = build_response_lightmap(
            unreal, m, lm, _make_mpc(), _lin_sampler, -900, 300)
        lm_pin = ""
    # M = x2 decode of the scale-2 atlas (lightmap_atlas.py contract). In
    # linear transport the display x2 is 2^2.2 on the sRGB-decoded sample,
    # so the pow-1/2.2 dst-factor path below recovers 2*M_disp exactly.
    lm2 = expr(unreal.MaterialExpressionMultiply, -330, 300)
    lm2.set_editor_property("const_b", 2.0 if GAMMA_SPACE else 2.0 ** 2.2)
    MEL.connect_material_expressions(lm_out, lm_pin, lm2, "A")
    light_factor = build_dynamic_world_factor(
        unreal, m, expr, lm2, "", -180, 300, lightmap_parameter=lm)
    prod = expr(unreal.MaterialExpressionMultiply, -250, 150)
    MEL.connect_material_expressions(tex, "RGB", prod, "A")
    MEL.connect_material_expressions(light_factor, "", prod, "B")
    if modulated:
        # FB_disp = (2*Tex*M)*Dst_disp -> linear factor (2*Tex*M)^2.2
        #         = 2^2.2 * Tex_lin * M_lin (sRGB-sampled products);
        # gamma space: the framebuffer IS display, factor = plain 2*Tex*M
        m.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MODULATE)
        k = expr(unreal.MaterialExpressionConstant, -250, 300)
        k.set_editor_property("r", 2.0 if GAMMA_SPACE else 2.0 ** 2.2)
        scaled = expr(unreal.MaterialExpressionMultiply, -100, 200)
        MEL.connect_material_expressions(prod, "", scaled, "A")
        MEL.connect_material_expressions(k, "", scaled, "B")
        MEL.connect_material_property(
            scaled, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    else:
        # premultiplied: FB = Emissive + Dst*(1-Opacity);
        # linear: Opacity = 1 - max3(M_disp*(1-Tex_disp))^2.2
        # gamma space: dst factor is raw M*(1-Tex) -> Opacity = 1 - max3(...)
        m.set_editor_property("blend_mode",
                              unreal.BlendMode.BLEND_ALPHA_COMPOSITE)
        MEL.connect_material_property(
            prod, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
        if GAMMA_SPACE:
            texd, lmd = tex, light_factor
            texd_pin, lmd_pin = "RGB", ""
        else:
            texd = expr(unreal.MaterialExpressionPower, -250, -150)
            texd.set_editor_property("const_exponent", 1.0 / 2.2)
            MEL.connect_material_expressions(tex, "RGB", texd, "Base")
            lmd = expr(unreal.MaterialExpressionPower, -250, -300)
            lmd.set_editor_property("const_exponent", 1.0 / 2.2)
            MEL.connect_material_expressions(light_factor, "", lmd, "Base")
            texd_pin, lmd_pin = "", ""
        omt = expr(unreal.MaterialExpressionOneMinus, -130, -150)
        MEL.connect_material_expressions(texd, texd_pin, omt, "")
        fdst = expr(unreal.MaterialExpressionMultiply, -30, -220)
        MEL.connect_material_expressions(lmd, lmd_pin, fdst, "A")
        MEL.connect_material_expressions(omt, "", fdst, "B")
        chans = []
        for i, ch in enumerate("rgb"):
            cm = expr(unreal.MaterialExpressionComponentMask, 70, -300 + i * 80)
            for c in "rgba":
                cm.set_editor_property(c, c == ch)
            MEL.connect_material_expressions(fdst, "", cm, "")
            chans.append(cm)
        mx1 = expr(unreal.MaterialExpressionMax, 190, -260)
        MEL.connect_material_expressions(chans[0], "", mx1, "A")
        MEL.connect_material_expressions(chans[1], "", mx1, "B")
        mx2 = expr(unreal.MaterialExpressionMax, 290, -220)
        MEL.connect_material_expressions(mx1, "", mx2, "A")
        MEL.connect_material_expressions(chans[2], "", mx2, "B")
        if GAMMA_SPACE:
            fout, fout_pin = mx2, ""
        else:
            fpow = expr(unreal.MaterialExpressionPower, 390, -220)
            fpow.set_editor_property("const_exponent", 2.2)
            MEL.connect_material_expressions(mx2, "", fpow, "Base")
            fout, fout_pin = fpow, ""
        op = expr(unreal.MaterialExpressionOneMinus, 490, -220)
        MEL.connect_material_expressions(fout, fout_pin, op, "")
        MEL.connect_material_property(
            op, "", unreal.MaterialProperty.MP_OPACITY)


def _import_neutral():
    """0.5-display gray (value 128): the modulate-2x identity texture."""
    full = "/Game/WoT/Textures/WOTDetail/T_WoT_Neutral"
    if not EAL.does_asset_exist(full):
        task = unreal.AssetImportTask()
        task.filename = str(CONV / "tex/WOTDetail/T_WoT_Neutral.png")
        task.destination_path = "/Game/WoT/Textures/WOTDetail"
        task.automated = True
        task.save = True
        ATT.import_asset_tasks([task])
    t = unreal.load_asset(full)
    if GAMMA_SPACE and t.get_editor_property("srgb"):
        t.set_editor_property("srgb", False)
        EAL.save_asset(full)
    return t


def _import_default_texture():
    """Engine.DefaultTexture (32x32 bubbles): retail's render-time stand-in
    for a surface whose texture is NULL ('__None' slots) or a dangling import
    (e.g. B3_02's WOT.Watr_400 — in no shipped package). Spec: Surf->Texture
    ? ... : Level->DefaultTexture, in the original renderer's zone pass and
    mover pass. Callers pair it with the surface's composed lightmap and
    pan flags like any resolved texture."""
    full = "/Game/WoT/Textures/Engine/DefaultTexture"
    if not EAL.does_asset_exist(full):
        task = unreal.AssetImportTask()
        task.filename = str(CONV / "tex/Engine/DefaultTexture.png")
        task.destination_path = "/Game/WoT/Textures/Engine"
        task.automated = True
        task.save = True
        ATT.import_asset_tasks([task])
    t = unreal.load_asset(full)
    ensure_texture_payload_binding(
        unreal, full, "Engine", "DefaultTexture")
    if GAMMA_SPACE and t.get_editor_property("srgb"):
        t.set_editor_property("srgb", False)
        EAL.save_asset(full)
    return t


def _overlay_sampler(m, tex_param, ratio_param, x, y,
                     offset_params=None, ratio_v_param=None):
    """Detail/macro sampler: UVs = UV0 * ratio (UE1 tiles overlays at the
    base texture's texel rate: detail_uv = base_uv * base_size/overlay_size).
    ratio_v_param gives V its own ratio: UV0 divides V by the base height, so
    a non-square base needs (H*DrawScale)/(oH*oDrawScale) on V, not the U
    ratio (Glide driver SetTexture: one texel rate per overlay, both axes).
    Default texture = neutral gray."""
    MEL = unreal.MaterialEditingLibrary
    s = MEL.create_material_expression(
        m, unreal.MaterialExpressionTextureSampleParameter2D, x, y)
    s.set_editor_property("parameter_name", tex_param)
    s.set_editor_property("texture", _import_neutral())
    _lin_sampler(s)
    tc = MEL.create_material_expression(
        m, unreal.MaterialExpressionTextureCoordinate, x - 250, y)
    ratio = MEL.create_material_expression(
        m, unreal.MaterialExpressionScalarParameter, x - 250, y + 120)
    ratio.set_editor_property("parameter_name", ratio_param)
    ratio.set_editor_property("default_value", 1.0)
    ratio_out = ratio
    if ratio_v_param is not None:
        ratio_v = MEL.create_material_expression(
            m, unreal.MaterialExpressionScalarParameter, x - 250, y + 240)
        ratio_v.set_editor_property("parameter_name", ratio_v_param)
        ratio_v.set_editor_property("default_value", 1.0)
        ratio_out = MEL.create_material_expression(
            m, unreal.MaterialExpressionAppendVector, x - 180, y + 180)
        MEL.connect_material_expressions(ratio, "", ratio_out, "A")
        MEL.connect_material_expressions(ratio_v, "", ratio_out, "B")
    uv = MEL.create_material_expression(
        m, unreal.MaterialExpressionMultiply, x - 120, y + 60)
    MEL.connect_material_expressions(tc, "", uv, "A")
    MEL.connect_material_expressions(ratio_out, "", uv, "B")
    final_uv = uv
    if offset_params is not None:
        ou = MEL.create_material_expression(
            m, unreal.MaterialExpressionScalarParameter, x - 120, y + 160)
        ou.set_editor_property("parameter_name", offset_params[0])
        ov = MEL.create_material_expression(
            m, unreal.MaterialExpressionScalarParameter, x - 120, y + 230)
        ov.set_editor_property("parameter_name", offset_params[1])
        offset = MEL.create_material_expression(
            m, unreal.MaterialExpressionAppendVector, x, y + 190)
        MEL.connect_material_expressions(ou, "", offset, "A")
        MEL.connect_material_expressions(ov, "", offset, "B")
        final_uv = MEL.create_material_expression(
            m, unreal.MaterialExpressionSubtract, x + 80, y + 70)
        MEL.connect_material_expressions(uv, "", final_uv, "A")
        MEL.connect_material_expressions(offset, "", final_uv, "B")
    MEL.connect_material_expressions(final_uv, "", s, "UVs")
    return s


def _detail_fade(m):
    """UE1 per-vertex detail alpha:
    a = min(100*(NearZ/z - 1), 255)/255, NearZ = 200uu, 0 beyond NearZ."""
    MEL = unreal.MaterialEditingLibrary
    pd = MEL.create_material_expression(
        m, unreal.MaterialExpressionPixelDepth, -1000, 620)
    inv = MEL.create_material_expression(
        m, unreal.MaterialExpressionDivide, -880, 620)
    inv.set_editor_property("const_a", 200.0)
    MEL.connect_material_expressions(pd, "", inv, "B")
    sub = MEL.create_material_expression(
        m, unreal.MaterialExpressionSubtract, -770, 620)
    sub.set_editor_property("const_b", 1.0)
    MEL.connect_material_expressions(inv, "", sub, "A")
    sc = MEL.create_material_expression(
        m, unreal.MaterialExpressionMultiply, -660, 620)
    sc.set_editor_property("const_b", 100.0 / 255.0)
    MEL.connect_material_expressions(sub, "", sc, "A")
    sat = MEL.create_material_expression(
        m, unreal.MaterialExpressionSaturate, -550, 620)
    MEL.connect_material_expressions(sc, "", sat, "")
    return sat


def _mod2x_term(m, sampler, fade_expr, x, y):
    """Modulate-2x overlay term in linear transport:
    term = (2*lerp(0.5, T_disp, fade))^2.2 with T_disp = pow(sample, 1/2.2);
    gamma space: samples are already display, term = 2*lerp(0.5, T, fade).
    fade_expr None = always full (macro pass has no distance fade)."""
    MEL = unreal.MaterialEditingLibrary
    if GAMMA_SPACE:
        td, td_pin = sampler, "RGB"
    else:
        td = MEL.create_material_expression(
            m, unreal.MaterialExpressionPower, x, y)
        td.set_editor_property("const_exponent", 1.0 / 2.2)
        MEL.connect_material_expressions(sampler, "RGB", td, "Base")
        td_pin = ""
    lerp = MEL.create_material_expression(
        m, unreal.MaterialExpressionLinearInterpolate, x + 130, y)
    lerp.set_editor_property("const_a", 0.5)
    MEL.connect_material_expressions(td, td_pin, lerp, "B")
    if fade_expr is not None:
        MEL.connect_material_expressions(fade_expr, "", lerp, "Alpha")
    else:
        lerp.set_editor_property("const_alpha", 1.0)
    two = MEL.create_material_expression(
        m, unreal.MaterialExpressionMultiply, x + 260, y)
    two.set_editor_property("const_b", 2.0)
    MEL.connect_material_expressions(lerp, "", two, "A")
    if GAMMA_SPACE:
        return two
    term = MEL.create_material_expression(
        m, unreal.MaterialExpressionPower, x + 380, y)
    term.set_editor_property("const_exponent", 2.2)
    MEL.connect_material_expressions(two, "", term, "Base")
    return term


def _make_mpc():
    """Create/reconcile the six-scalar, frozen-default light-wave MPC contract."""
    # The dynamic-light collection is bootstrapped beside the existing light-wave
    # collection so every standalone master-regeneration path sees both.
    ensure_dynamic_light_collection(unreal)
    full = "/Game/WoT/Materials/MPC_WoTLightWave"
    if EAL.does_asset_exist(full):
        mpc = unreal.load_asset(full)
    else:
        mpc = ATT.create_asset(
            "MPC_WoTLightWave", "/Game/WoT/Materials",
            unreal.MaterialParameterCollection,
            unreal.MaterialParameterCollectionFactoryNew())
    if not mpc:
        raise RuntimeError(f"could not create or load {full}")

    current = tuple(
        (str(row.get_editor_property("parameter_name")),
         float(row.get_editor_property("default_value")))
        for row in mpc.get_editor_property("scalar_parameters"))
    if current != MPC_SCALAR_DEFAULTS:
        parameters = []
        for name, default in MPC_SCALAR_DEFAULTS:
            row = unreal.CollectionScalarParameter()
            row.set_editor_property("parameter_name", name)
            row.set_editor_property("default_value", default)
            parameters.append(row)
        mpc.set_editor_property("scalar_parameters", parameters)
        EAL.save_asset(full)
    return mpc


def _make_animated_scene_blend_master():
    """Create or graph-version-rebuild the shared animated SceneBlend master."""
    name = "M_WoT_WorldWaveT_SceneBlend"
    path = f"/Game/WoT/Materials/{name}"
    exists = EAL.does_asset_exist(path)
    current = unreal.load_asset(path) if exists else None
    response_tagged = exists and EAL.get_metadata_tag(
        current, WORLD_RESPONSE_GRAPH_TAG) == WORLD_RESPONSE_GRAPH_VERSION
    dynamic_tagged = exists and EAL.get_metadata_tag(
        current, DYNAMIC_LIGHT_GRAPH_TAG) == DYNAMIC_LIGHT_GRAPH_VERSION
    if response_tagged and dynamic_tagged:
        return current
    if exists:
        material = current
        clear_material_graph(unreal, material)
    else:
        material = ATT.create_asset(
            name, "/Game/WoT/Materials", unreal.Material,
            unreal.MaterialFactoryNew())
    build_animated_scene_blend_graph(
        unreal, material, _make_mpc(), _lin_sampler, _panned_uv(material))
    EAL.set_metadata_tag(
        material, WORLD_RESPONSE_GRAPH_TAG, WORLD_RESPONSE_GRAPH_VERSION)
    EAL.set_metadata_tag(
        material, DYNAMIC_LIGHT_GRAPH_TAG, DYNAMIC_LIGHT_GRAPH_VERSION)
    unreal.MaterialEditingLibrary.recompile_material(material)
    EAL.save_asset(path)
    return material


def make_world_masters():
    """World-surface masters: UE1-composed lightmap injection.
    Emissive = Tex(UV0) x 2*Lightmap(UV1) — the x2 decodes the scale-2 atlas
    (lightmap_atlas.py contract; full UE1 overbright range to M~2); both
    sampled sRGB so the engine's decode reproduces UE1's display-space
    product (display = tex_disp * M within sRGB-toe error). BaseColor black for now — transient dynamic
    wall lighting (flicker deltas, projectiles) lands with lighting channels.
    Blended surfaces (water/waterfall portals, fire quads) get translucent/
    modulated masters (+ fractal-flipbook variants) — see
    _make_world_blend_master for the UE1 blend semantics."""
    path = "/Game/WoT/Materials"
    macro_graph_tag = "WoTMacroUVGraph"
    # v2: the detail sampler takes its own V ratio (DetailRatioV).
    macro_graph_version = "retail-pan-v2"
    _make_mpc()
    masters = {}
    # The *Flip sheet-scrub variants are RETIRED
    # (PORT_NOTES "Procedural textures animate when recently seen"): fractal
    # surfaces ride the plain blend masters and the
    # runtime binds the live procedural texture.
    for kind, name, mod, animated in (
            ("translucent", "M_WoT_WorldTranslucent", False, False),
            ("modulated", "M_WoT_WorldModulated", True, False),
            ("wave_translucent", "M_WoT_WorldWaveTranslucent", False, True),
            ("wave_modulated", "M_WoT_WorldWaveModulated", True, True)):
        masters[kind] = _make_world_blend_master(name, mod, animated)
    for name, kind in (("M_WoT_World", "opaque"), ("M_WoT_WorldMasked", "masked"),
                       ("M_WoT_WorldWave", "wave_opaque"),
                       ("M_WoT_WorldWaveMasked", "wave_masked")):
        masked = kind.endswith("masked")
        animated = kind.startswith("wave_")
        full = f"{path}/{name}"
        exists = EAL.does_asset_exist(full)
        current = unreal.load_asset(full) if exists else None
        response_tagged = (not animated) or (
            exists and EAL.get_metadata_tag(
                current, WORLD_RESPONSE_GRAPH_TAG)
            == WORLD_RESPONSE_GRAPH_VERSION)
        dynamic_tagged = exists and EAL.get_metadata_tag(
            current, DYNAMIC_LIGHT_GRAPH_TAG) == DYNAMIC_LIGHT_GRAPH_VERSION
        macro_tagged = exists and EAL.get_metadata_tag(
            current, macro_graph_tag) == macro_graph_version
        if exists and response_tagged and dynamic_tagged and macro_tagged:
            masters[kind] = current
            continue
        if exists:
            m = current
            clear_material_graph(unreal, m)
        else:
            m = ATT.create_asset(
                name, path, unreal.Material, unreal.MaterialFactoryNew())
        tex = _lin_sampler(unreal.MaterialEditingLibrary.create_material_expression(
            m, unreal.MaterialExpressionTextureSampleParameter2D, -600, 0))
        tex.set_editor_property("parameter_name", "Tex")
        unreal.MaterialEditingLibrary.connect_material_expressions(
            _panned_uv(m), "", tex, "UVs")
        lm = _lin_sampler(unreal.MaterialEditingLibrary.create_material_expression(
            m, unreal.MaterialExpressionTextureSampleParameter2D, -600, 300))
        lm.set_editor_property("parameter_name", "Lightmap")
        lm.set_editor_property("const_coordinate", 1)   # atlas UVs
        lm_out, lm_pin = lm, "RGB"
        if animated:
            # Literal mean base plus six unsigned, statically switched centered
            # responses. Frozen coefficients multiply every new sampler by 0.
            lm_out = build_response_lightmap(
                unreal, m, lm, _make_mpc(), _lin_sampler, -1050, 300)
            lm_pin = ""
        # M = x2 decode of the scale-2 atlas (lightmap_atlas.py contract);
        # in linear transport the display x2 is 2^2.2 on the decoded sample.
        lm2 = unreal.MaterialEditingLibrary.create_material_expression(
            m, unreal.MaterialExpressionMultiply, -360, 250)
        lm2.set_editor_property("const_b", 2.0 if GAMMA_SPACE else 2.0 ** 2.2)
        unreal.MaterialEditingLibrary.connect_material_expressions(
            lm_out, lm_pin, lm2, "A")
        lm_out = build_dynamic_world_factor(
            unreal, m, lambda cls, x, y:
                unreal.MaterialEditingLibrary.create_material_expression(
                    m, cls, x, y),
            lm2, "", -180, 250, lightmap_parameter=lm)
        lm_pin = ""
        mul = unreal.MaterialEditingLibrary.create_material_expression(
            m, unreal.MaterialExpressionMultiply, -300, 150)
        unreal.MaterialEditingLibrary.connect_material_expressions(tex, "RGB", mul, "A")
        unreal.MaterialEditingLibrary.connect_material_expressions(
            lm_out, lm_pin, mul, "B")
        # UE1 detail (near-fade) + macro overlays: FB *= 2*lerp(0.5,T,fade)
        # each — neutral-gray defaults keep untextured slots at exactly 1x.
        det = _overlay_sampler(m, "DetailTex", "DetailRatio", -600, 600,
                               ratio_v_param="DetailRatioV")
        mac = _overlay_sampler(m, "MacroTex", "MacroRatio", -600, 900,
                               ("MacroOffsetU", "MacroOffsetV"))
        dterm = _mod2x_term(m, det, _detail_fade(m), -150, 500)
        mterm = _mod2x_term(m, mac, None, -150, 800)
        mul2 = unreal.MaterialEditingLibrary.create_material_expression(
            m, unreal.MaterialExpressionMultiply, -50, 300)
        unreal.MaterialEditingLibrary.connect_material_expressions(mul, "", mul2, "A")
        unreal.MaterialEditingLibrary.connect_material_expressions(dterm, "", mul2, "B")
        mul3 = unreal.MaterialEditingLibrary.create_material_expression(
            m, unreal.MaterialExpressionMultiply, 50, 400)
        unreal.MaterialEditingLibrary.connect_material_expressions(mul2, "", mul3, "A")
        unreal.MaterialEditingLibrary.connect_material_expressions(mterm, "", mul3, "B")
        unreal.MaterialEditingLibrary.connect_material_property(
            mul3, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
        black = unreal.MaterialEditingLibrary.create_material_expression(
            m, unreal.MaterialExpressionConstant3Vector, -300, -150)
        black.set_editor_property("constant", unreal.LinearColor(0, 0, 0, 1))
        unreal.MaterialEditingLibrary.connect_material_property(
            black, "", unreal.MaterialProperty.MP_BASE_COLOR)
        # no PBR response at all: default specular 0.5 turns the 160 movable
        # actor lights into a sheen across the whole map (measured ~4x lift)
        rough = unreal.MaterialEditingLibrary.create_material_expression(
            m, unreal.MaterialExpressionConstant, -300, -300)
        rough.set_editor_property("r", 1.0)
        unreal.MaterialEditingLibrary.connect_material_property(
            rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
        spec = unreal.MaterialEditingLibrary.create_material_expression(
            m, unreal.MaterialExpressionConstant, -300, -400)
        spec.set_editor_property("r", 0.0)
        unreal.MaterialEditingLibrary.connect_material_property(
            spec, "", unreal.MaterialProperty.MP_SPECULAR)
        if masked:
            m.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED)
            # UE1 Glide alpha test: GR_CMP_GREATER ref 127
            # = 0.5 threshold; UE's 0.333 default fattens masked edges
            m.set_editor_property("opacity_mask_clip_value", 0.5)
            unreal.MaterialEditingLibrary.connect_material_property(
                tex, "A", unreal.MaterialProperty.MP_OPACITY_MASK)
        if animated:
            EAL.set_metadata_tag(
                m, WORLD_RESPONSE_GRAPH_TAG, WORLD_RESPONSE_GRAPH_VERSION)
        EAL.set_metadata_tag(
            m, DYNAMIC_LIGHT_GRAPH_TAG, DYNAMIC_LIGHT_GRAPH_VERSION)
        EAL.set_metadata_tag(m, macro_graph_tag, macro_graph_version)
        unreal.MaterialEditingLibrary.recompile_material(m)
        EAL.save_asset(full)
        masters[kind] = m
    # This shared parent used to be deleted with the six ordinary animated
    # masters but recreated only on a conditional rest-sky path. Reconcile it
    # here on every response import so no consumer can retain a missing parent.
    masters["wave_scene_blend"] = _make_animated_scene_blend_master()
    return masters


def load_legacy_world_masters():
    """Load the legacy (pre-response) world graph set without mutating shared assets."""
    masters = {}
    for kind, name in (
            ("translucent", "M_WoT_WorldTranslucent"),
            ("modulated", "M_WoT_WorldModulated"),
            ("opaque", "M_WoT_World"),
            ("masked", "M_WoT_WorldMasked"),
            ("wave_opaque", "M_WoT_WorldWave"),
            ("wave_masked", "M_WoT_WorldWaveMasked")):
        path = f"/Game/WoT/Materials/{name}"
        material = unreal.load_asset(path) if EAL.does_asset_exist(path) else None
        if not material:
            raise RuntimeError(
                f"legacy lightmap manifest requires existing shared master {path}; "
                "the compatibility path will not synthesize shared assets")
        masters[kind] = material
    return masters


def prevalidate_lightmap_atlases():
    """Load every lightmap input before the first full-import asset mutation.

    New manifests validate all response records and complete PNG structures.
    A manifest with no ``animation`` member is deliberately the legacy
    endpoint contract: validate its optional hi/lo pair and leave shared
    response masters/MPC untouched.
    """
    document_path = CONV / f"{MAP}_lmatlas.json"
    if not document_path.is_file():
        raise RuntimeError(f"missing lightmap atlas manifest {document_path}")
    try:
        document = json.loads(document_path.read_text())
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise RuntimeError(f"invalid {document_path.name}: {exc}") from exc
    if not isinstance(document, dict):
        raise RuntimeError(f"invalid {document_path.name}: root must be an object")
    size = document.get("size")
    if not isinstance(size, list) or len(size) != 2 \
            or any(not isinstance(value, int) or isinstance(value, bool)
                   or value <= 0 for value in size):
        raise RuntimeError(
            f"invalid {document_path.name}: size must be two positive integers")
    expected_size = tuple(size)

    base_png = CONV / f"{MAP}_lmatlas.png"
    if not base_png.is_file():
        raise RuntimeError(f"missing lightmap atlas {base_png}")
    actual_size = validate_png(
        base_png, expected_bit_depth=8, allowed_color_types=(2,))
    if actual_size != expected_size:
        raise RuntimeError(
            f"lightmap atlas dimensions disagree for {base_png}: "
            f"{actual_size[0]}x{actual_size[1]} != "
            f"{expected_size[0]}x{expected_size[1]}")

    if "animation" not in document:
        legacy_files = tuple(
            CONV / f"{MAP}_lmatlas_{endpoint}.png"
            for endpoint in ("hi", "lo"))
        present = tuple(path.is_file() for path in legacy_files)
        if any(present) and not all(present):
            raise RuntimeError(
                f"legacy endpoint atlas pair is incomplete for {MAP}")
        if all(present):
            for png in legacy_files:
                actual_size = validate_png(
                    png, expected_bit_depth=8, allowed_color_types=(2,))
                if actual_size != expected_size:
                    raise RuntimeError(
                        f"legacy endpoint dimensions disagree for {png}: "
                        f"{actual_size[0]}x{actual_size[1]} != "
                        f"{expected_size[0]}x{expected_size[1]}")
            return document, (), legacy_files
        return document, (), None

    try:
        bindings = material_term_bindings(document["animation"])
    except (KeyError, TypeError, ValueError) as exc:
        raise RuntimeError(f"invalid {document_path.name}: {exc}") from exc
    response_files = []
    for binding in bindings:
        response_name = binding["response_png"]
        png = CONV / response_name
        if not png.is_file():
            raise RuntimeError(f"manifest response PNG is missing: {png}")
        actual_size = validate_png(
            png, expected_bit_depth=8, allowed_color_types=(2,))
        if actual_size != expected_size:
            raise RuntimeError(
                f"response atlas dimensions disagree for {png}: "
                f"{actual_size[0]}x{actual_size[1]} != "
                f"{expected_size[0]}x{expected_size[1]}")
        response_files.append((response_name, png))
    return document, tuple(response_files), None


def import_atlas():
    """Import this map's composed lightmap atlas: sRGB, uncompressed, no mips
    (1px gutters can't survive mipping; UE1 lightmaps had no mips either)."""
    png = CONV / f"{MAP}_lmatlas.png"
    if not png.exists():
        raise RuntimeError(f"missing lightmap atlas {png}")
    dest = "/Game/WoT/Maps/Lightmaps"
    full = f"{dest}/{MAP}_lmatlas"
    task = unreal.AssetImportTask()
    task.filename = str(png)
    task.destination_path = dest
    task.automated = True
    task.save = False
    task.replace_existing = True
    ATT.import_asset_tasks([task])
    t = unreal.load_asset(full)
    # linear scheme: sRGB ON is load-bearing (the encode/decode pair
    # reproduces UE1's display-space product). TC_VectorDisplacementmap
    # silently forces LINEAR sampling (ignores the srgb flag -> dark texels
    # lifted ~M^(1/2.2)); TC_Default + compression_none keeps sRGB
    # semantics, uncompressed. Gamma-space experiment: raw bytes throughout.
    t.set_editor_property("srgb", not GAMMA_SPACE)
    t.set_editor_property("compression_settings",
                          unreal.TextureCompressionSettings.TC_EDITOR_ICON)
    t.set_editor_property("mip_gen_settings", unreal.TextureMipGenSettings.TMGS_NO_MIPMAPS)
    EAL.save_asset(full)
    return full


_pan_zone_cache = None


def _zone_pan(zone_actor_name):
    """(TexUPanSpeed, TexVPanSpeed) of a zone actor; ZoneInfo defaults 1.0."""
    global _pan_zone_cache
    if _pan_zone_cache is None:
        _pan_zone_cache = {}
        f = CONV / f"{MAP}.t3d.json"
        if f.exists():
            for a in json.loads(f.read_text())["actors"]:
                p = a.get("props", {})
                _pan_zone_cache[a["name"].lower()] = (
                    float(p.get("TexUPanSpeed", 1.0)),
                    float(p.get("TexVPanSpeed", 1.0)))
    return _pan_zone_cache.get((zone_actor_name or "").lower(), (1.0, 1.0))


def import_response_atlases(document, response_files):
    """Import the already-prevalidated light-map responses in manifest order."""
    dest = "/Game/WoT/Maps/Lightmaps"
    textures = {}
    for response_name, png in response_files:
        full = f"{dest}/{png.stem}"
        task = unreal.AssetImportTask()
        task.filename = str(png)
        task.destination_path = dest
        task.automated = True
        task.save = False
        task.replace_existing = True
        ATT.import_asset_tasks([task])
        texture = unreal.load_asset(full)
        if not texture:
            raise RuntimeError(f"response atlas import produced no asset: {full}")
        # Response bytes are unsigned 2R in display-space atlas units. They
        # must remain raw; the material adds them to the raw mean sample before
        # the one existing x2 decode.
        texture.set_editor_property("srgb", False)
        texture.set_editor_property(
            "compression_settings",
            unreal.TextureCompressionSettings.TC_EDITOR_ICON)
        texture.set_editor_property(
            "mip_gen_settings", unreal.TextureMipGenSettings.TMGS_NO_MIPMAPS)
        EAL.save_asset(full)
        textures[response_name] = texture
    return document, textures


def import_legacy_wave_atlases(legacy_files):
    """Import the legacy hi/lo endpoint pair after full prevalidation."""
    if not legacy_files:
        return None
    dest = "/Game/WoT/Maps/Lightmaps"
    paths = []
    for png in legacy_files:
        full = f"{dest}/{png.stem}"
        task = unreal.AssetImportTask()
        task.filename = str(png)
        task.destination_path = dest
        task.automated = True
        task.save = False
        task.replace_existing = True
        ATT.import_asset_tasks([task])
        texture = unreal.load_asset(full)
        if not texture:
            raise RuntimeError(f"legacy endpoint import produced no asset: {full}")
        texture.set_editor_property("srgb", not GAMMA_SPACE)
        texture.set_editor_property(
            "compression_settings", unreal.TextureCompressionSettings.TC_EDITOR_ICON)
        texture.set_editor_property(
            "mip_gen_settings", unreal.TextureMipGenSettings.TMGS_NO_MIPMAPS)
        EAL.save_asset(full)
        paths.append(full)
    return tuple(paths)


_FRACTAL_DIMS = None


def _fractal_frame_dims(tkey):
    """(width, height) of a fractal record — the pan-rate frame size the sheet
    asset used to provide (sheet_w/Cols == record width). Read once from the
    conv flipbooks manifests; unknown records fall back to 256 (the census
    majority) rather than failing a whole map import over pan cosmetics."""
    global _FRACTAL_DIMS
    if _FRACTAL_DIMS is None:
        _FRACTAL_DIMS = {}
        for man in sorted((CONV / "flipbooks").glob("*/_flipbooks.json")):
            data = json.loads(man.read_text())
            for section in ("fire", "runtime"):
                for name, info in data.get(section, {}).items():
                    key = (man.parent.name, name)
                    if key not in _FRACTAL_DIMS:
                        # "fire" entries carry w/h; "runtime" carries width/height.
                        _FRACTAL_DIMS[key] = (
                            int(info.get("w", info.get("width", 256))),
                            int(info.get("h", info.get("height", 256))))
    return _FRACTAL_DIMS.get(tkey, (256, 256))


def make_world_instances(mats, textures, world_masters, atlas_path,
                         atlas_document, response_textures,
                         legacy_wave_paths=None):
    """Per-map world MICs: parent = world master by blend family, Tex + this
    map's atlas. Translucent/modulated slots route here even when unlit (their
    UV1 sits on the atlas fullbright texel, so Lightmap=1); fractal textures
    (no static PNG, shared MI_<tex> exists) get neutral pre-activation Tex +
    manifest-sourced pan frame dims; the subsystem binds the live texture at
    DiscoverAndBind. PF_FakeBackdrop
    slots keep their shared instances; setup_childsky.py replaces them with
    the hidden window material on the world actor. Opaque unlit slots also
    keep the shared fullbright instances (correct: no lightmap pass)."""
    MEL = unreal.MaterialEditingLibrary
    insts = {}
    path = f"/Game/WoT/Materials/Instances/{MAP}"
    atlas = unreal.load_asset(atlas_path)
    animation = atlas_document.get("animation")
    response_animated = animation is not None \
        and bool(material_term_bindings(animation))
    legacy_animated = bool(legacy_wave_paths)
    for mat, info in mats.items():
        texname = info["texture"]
        tkey = (info["package"], texname)     # authoritative (package, name)
        fl = info["flags"]
        translucent = bool(fl & 0x4)          # PF_Translucent
        modulated = bool(fl & 0x40)           # PF_Modulated
        # PF_FakeBackdrop rides the unlit shared-instance route (its window
        # texture, fullbright) — setup_childsky then hides those slots on the
        # world actor. Skipping them instead would land MI_Missing (the
        # assign_materials fallback) on every fresh import until childsky ran.
        unlit = bool(fl & (0x400000 | 0x80))  # PF_Unlit | PF_FakeBackdrop
        flip = tkey not in textures and EAL.does_asset_exist(
            f"/Game/WoT/Materials/Instances/MI_{texname}")
        # NULL surface texture ('__None') or dangling import (B3_02's
        # WOT.Watr_400 class): retail substitutes Level->DefaultTexture at
        # render time (the zone pass) — Engine.DefaultTexture x the
        # composed lightmap, pan flags live. bsp2obj normalized these UVs by its
        # (1,1) unresolved fallback, not the substituted texture's 32px, so
        # tiling runs denser (PORT_NOTES);
        # mipped/grazing views collapse to the same mip-tail mean. Without
        # this, the slot fell to MI_Missing (developer checker on the lit
        # mesh master — renders black in-game, matching nothing retail).
        dangling = not flip and tkey not in textures
        if translucent or modulated:
            # the renderer checks PF_Translucent before PF_Modulated
            kind = "translucent" if translucent else "modulated"
        elif unlit and not dangling:
            continue   # opaque unlit/sky surfaces keep the fullbright masters
        else:
            # dangling-unlit routes here too: its UV1 sits on the atlas
            # fullbright texel, so the lit world master renders it Tex*1
            kind = "masked" if fl & 0x2 else "opaque"
        if response_animated:
            # Every atlas-consuming family must see the same centered sum;
            # delta masks are black where a surface has no affected texels.
            kind = "wave_" + kind
        elif legacy_animated and kind in ("opaque", "masked"):
            # The checked-in endpoint masters animate only opaque/masked world
            # surfaces. Preserve that exact legacy routing for endpoint manifests.
            kind = "wave_" + kind
        parent = world_masters[kind]
        safe = mat.replace("-", "_")
        full = f"{path}/MI_W_{safe}"
        if not EAL.does_asset_exist(full):
            mi = ATT.create_asset(f"MI_W_{safe}", path,
                                  unreal.MaterialInstanceConstant,
                                  unreal.MaterialInstanceConstantFactoryNew())
            mi.set_editor_property("parent", parent)
        else:
            # earlier imports may predate the blend masters — reparent
            mi = unreal.load_asset(full)
            if mi.get_editor_property("parent") != parent:
                mi.set_editor_property("parent", parent)
        if flip:
            # Sheet-scrub retirement: the shared MI_<tex> is an
            # identity child with NO parameters — the fractal subsystem swaps
            # the live texture in at DiscoverAndBind, before the first frame.
            # Neutral gray pre-activation (never rendered); pan-rate frame
            # dims come from the conv flipbooks manifest, which is what the
            # retired sheet's size/Cols quotient used to encode.
            MEL.set_material_instance_texture_parameter_value(
                mi, "Tex", _import_neutral())
            tex_w, tex_h = _fractal_frame_dims(tkey)
        elif dangling:
            MEL.set_material_instance_texture_parameter_value(
                mi, "Tex", _import_default_texture())
            tex_w, tex_h = 32, 32   # pan rate over the SUBSTITUTED texture
        else:
            MEL.set_material_instance_texture_parameter_value(
                mi, "Tex", unreal.load_asset(textures[tkey]))
            ti = TEXINFO.get(tkey, {})
            tex_w, tex_h = ti.get("w", 256), ti.get("h", 256)
        MEL.set_material_instance_texture_parameter_value(mi, "Lightmap", atlas)
        # PF_SpecialLit is split into its own converter slot. Every current
        # runtime emitter is bSpecialLit=false, so those authored receivers
        # keep their static atlas but reject the moving-light term.
        MEL.set_material_instance_scalar_parameter_value(
            mi, DYNAMIC_LIGHT_ENABLE_PARAMETER,
            0.0 if fl & 0x00100000 else 1.0)
        if legacy_animated and kind in ("wave_opaque", "wave_masked"):
            hi_path, lo_path = legacy_wave_paths
            MEL.set_material_instance_texture_parameter_value(
                mi, "LightmapHi", unreal.load_asset(hi_path))
            MEL.set_material_instance_texture_parameter_value(
                mi, "LightmapLo", unreal.load_asset(lo_path))
        if response_animated:
            bind_response_parameters(
                unreal, mi, atlas_document, response_textures)
            # Reused MICs keep parameter VALUES for parameters their rebuilt
            # parent no longer exposes -- stale LightmapHi/Lo entries pin the
            # retired endpoint atlases as referencers forever. Drop them.
            stale = [v for v in mi.get_editor_property(
                         "texture_parameter_values")
                     if str(v.get_editor_property("parameter_info")
                            .get_editor_property("name"))
                     not in ("LightmapHi", "LightmapLo")]
            if len(stale) != len(mi.get_editor_property(
                    "texture_parameter_values")):
                mi.set_editor_property("texture_parameter_values", stale)
        if fl & 0x600:   # PF_AutoUPan / PF_AutoVPan: 35*speed texels/sec
            su, sv = _zone_pan(info.get("pan_zone"))
            if fl & 0x200:
                MEL.set_material_instance_scalar_parameter_value(
                    mi, "PanU", 35.0 * su / max(tex_w, 1))
            if fl & 0x400:
                MEL.set_material_instance_scalar_parameter_value(
                    mi, "PanV", 35.0 * sv / max(tex_h, 1))
        if kind.replace("wave_", "") in ("opaque", "masked"):
            # UE1 detail/macro overlay refs (utx2png manifest); ratio ties
            # the overlay to the base texture's texel rate.
            ti = TEXINFO.get(tkey, {})
            for key, pt, pr in (("detail", "DetailTex", "DetailRatio"),
                                ("macro", "MacroTex", "MacroRatio")):
                ov = ti.get(key)
                ovk = tuple(ov) if ov else None   # detail/macro stored [pkg,name]
                if ovk and ovk in textures and ovk in TEXINFO:
                    MEL.set_material_instance_texture_parameter_value(
                        mi, pt, unreal.load_asset(textures[ovk]))
                    MEL.set_material_instance_scalar_parameter_value(
                        mi, pr, ti["w"] / TEXINFO[ovk]["w"])
        EAL.save_asset(full)
        insts[mat] = full
    return insts


def make_master():
    """Masters keyed by (masked, unlit). Lit: BaseColor=Tex, diffuse-only —
    used by MOVERS/DECORATIONS (dynamic actor lighting, as UE1 vertex-lit
    them). Unlit (PF_UNLIT / PF_FAKEBACKDROP skies): Emissive=Tex fullbright."""
    material_root = "/Game/WoT/Materials"
    path = material_root
    masters = {}
    variants = (("M_WoT_Master", False, False), ("M_WoT_Masked", True, False),
                ("M_WoT_Unlit", False, True), ("M_WoT_UnlitMasked", True, True))
    for name, masked, unlit in variants:
        full = f"{path}/{name}"
        if EAL.does_asset_exist(full):
            masters[(masked, unlit)] = unreal.load_asset(full)
            continue
        m = ATT.create_asset(name, path, unreal.Material, unreal.MaterialFactoryNew())
        # required once geometry is lightmapped: without it -game falls back
        # to the default (checker) material ("missing usage flag StaticLighting")
        m.set_editor_property("used_with_static_lighting", True)
        # required by the SKELETAL consumers (pawn/creature skins MI_*_Base,
        # non-J decoration skins like Skull03): the editor auto-sets usage on
        # first render, but -game cannot and falls back to the default
        # material ("missing usage flag SkeletalMesh! Default Material will
        # be used in game."), which reads near-black in the dark maps.
        m.set_editor_property("used_with_skeletal_mesh", True)
        tex = unreal.MaterialEditingLibrary.create_material_expression(
            m, unreal.MaterialExpressionTextureSampleParameter2D, -400, 0)
        tex.set_editor_property("parameter_name", "Tex")
        if unlit:
            # gamma-space: raw sample + boundary pow nets the same bytes,
            # so only the sampler-type warning needs silencing
            _lin_sampler(tex)
        if unlit:
            m.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
            unreal.MaterialEditingLibrary.connect_material_property(
                tex, "RGB", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
        else:
            unreal.MaterialEditingLibrary.connect_material_property(
                tex, "RGB", unreal.MaterialProperty.MP_BASE_COLOR)
            # period look: pure diffuse, no specular sheen (1999 had no PBR response)
            rough = unreal.MaterialEditingLibrary.create_material_expression(
                m, unreal.MaterialExpressionConstant, -400, 300)
            rough.set_editor_property("r", 1.0)
            unreal.MaterialEditingLibrary.connect_material_property(
                rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
            spec = unreal.MaterialEditingLibrary.create_material_expression(
                m, unreal.MaterialExpressionConstant, -400, 400)
            spec.set_editor_property("r", 0.0)
            unreal.MaterialEditingLibrary.connect_material_property(
                spec, "", unreal.MaterialProperty.MP_SPECULAR)
        if masked:
            m.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED)
            # UE1 alpha test = 0.5 (GR_CMP_GREATER ref 127)
            m.set_editor_property("opacity_mask_clip_value", 0.5)
            unreal.MaterialEditingLibrary.connect_material_property(
                tex, "A", unreal.MaterialProperty.MP_OPACITY_MASK)
        unreal.MaterialEditingLibrary.recompile_material(m)
        EAL.save_asset(f"{path}/{name}")
        masters[(masked, unlit)] = m
    return masters


def load_legacy_actor_masters():
    """Load mover/decoration masters without any shared-asset write."""
    masters = {}
    for name, masked, unlit in (
            ("M_WoT_Master", False, False),
            ("M_WoT_Masked", True, False),
            ("M_WoT_Unlit", False, True),
            ("M_WoT_UnlitMasked", True, True)):
        path = f"/Game/WoT/Materials/{name}"
        material = unreal.load_asset(path) if EAL.does_asset_exist(path) else None
        if not material:
            raise RuntimeError(
                f"legacy lightmap manifest requires existing shared master {path}; "
                "the compatibility path will not synthesize shared assets")
        masters[(masked, unlit)] = material
    return masters


def make_instances(mats, textures, masters):
    insts = {}
    path = "/Game/WoT/Materials/Instances"
    for mat, info in mats.items():
        texname = info["texture"]
        tkey = (info["package"], texname)
        if tkey not in textures:
            fb = f"{path}/MI_{texname}"
            if EAL.does_asset_exist(fb):     # baked fractal flipbook
                insts[mat] = fb
            continue
        safe = mat.replace("-", "_")
        full = f"{path}/MI_{safe}"
        masked = bool(info["flags"] & 0x2)
        unlit = bool(info["flags"] & (0x400000 | 0x80))  # PF_UNLIT | PF_FAKEBACKDROP
        if not EAL.does_asset_exist(full):
            mi = ATT.create_asset(f"MI_{safe}", path,
                                  unreal.MaterialInstanceConstant,
                                  unreal.MaterialInstanceConstantFactoryNew())
            mi.set_editor_property("parent", masters[(masked, unlit)])
            unreal.MaterialEditingLibrary.set_material_instance_texture_parameter_value(
                mi, "Tex", unreal.load_asset(textures[tkey]))
            EAL.save_asset(full)
        else:
            # earlier imports predate the unlit masters — reparent if wrong
            mi = unreal.load_asset(full)
            if mi.get_editor_property("parent") != masters[(masked, unlit)]:
                mi.set_editor_property("parent", masters[(masked, unlit)])
                EAL.save_asset(full)
        insts[mat] = full
    return insts


def lightmap_pipeline():
    """Interchange pipeline = default stack, Nanite off, generated lightmap
    UVs OFF: UV channel 1 now carries the atlas UVs authored in the GLB
    (UE1-composed lightmap injection). GPULM-generated UVs would clobber it;
    the modern-lighting variant can regenerate them on its own import."""
    full = "/Game/WoT/Pipelines/IP_WoTMapLightmap"
    if not EAL.does_asset_exist(full):
        if not EAL.duplicate_asset("/Interchange/Pipelines/DefaultAssetsPipeline", full):
            raise RuntimeError("could not duplicate default Interchange pipeline")
    p = unreal.load_asset(full)
    mp = p.get_editor_property("mesh_pipeline")
    mp.set_editor_property("generate_lightmap_u_vs", False)
    # Interchange defaults to Nanite (5.8). For the BSP world meshes that is
    # wrong twice over: GPU Lightmass walks the decimated Nanite fallback
    # (tiny sections collapse to 0 tris -> RT segment/batch mismatch assert),
    # and complex-as-simple collision also runs on the fallback, not the real
    # geometry. ~10k-tri period maps gain nothing from Nanite anyway.
    mp.set_editor_property("build_nanite", False)
    EAL.save_asset(full)
    return full


def import_mesh(suffix="", source_suffix=None):
    # Per-map dir: avoids cross-map asset-name collisions between OBJ groups.
    dest = f"/Game/WoT/Maps/Geo/{MAP}{suffix}"
    if EAL.does_directory_exist(dest):
        EAL.delete_directory(dest)
    # GLB, one mesh node — UE's OBJ translator silently drops faces; glTF is the
    # well-supported Interchange path.
    params = unreal.ImportAssetParameters()
    params.is_automated = True
    params.override_pipelines = [unreal.SoftObjectPath(lightmap_pipeline())]
    mgr = unreal.InterchangeManager.get_interchange_manager_scripted()
    source_suffix = suffix if source_suffix is None else source_suffix
    source_file = CONV / f"{MAP}{source_suffix}.glb"
    if source_suffix != suffix:
        # Interchange names the imported asset after the SOURCE FILENAME, not
        # the GLB mesh node — a carrier imported from Arena_07_carrier.glb
        # landed as "Arena_07_carrier", breaking the canonical-identity
        # contract (the runtime carrier bind rejects AssetPath mismatches).
        # Stage the bytes under the canonical filename first.
        staged_dir = Path(unreal.Paths.convert_relative_path_to_full(
            unreal.Paths.project_intermediate_dir())) / "BSPCarrierStage"
        staged_dir.mkdir(parents=True, exist_ok=True)
        staged = staged_dir / f"{MAP}{suffix}.glb"
        staged.write_bytes(source_file.read_bytes())
        source_file = staged
    source = unreal.InterchangeManager.create_source_data(str(source_file))
    if not mgr.import_asset(dest, source, params):
        raise RuntimeError("interchange import_asset returned false")
    mgr.wait_until_all_tasks_done(False)
    EAL.save_directory(dest)
    meshes = [a for a in EAL.list_assets(dest)
              if isinstance(unreal.load_asset(a.split(".")[0]), unreal.StaticMesh)]
    if len(meshes) != 1:
        raise RuntimeError(f"expected 1 combined mesh, got {meshes}")
    return meshes[0].split(".")[0]


def import_split_meshes(split_slots, suffix=""):
    """Import a base mesh plus one component-sized mesh per fractal slot.

    The derived GLBs retain the source binary chunk/accessors verbatim; only
    the primitive lists and material tables are partitioned. All assets stay
    in the historical per-map directory so existing world-material scanners
    see the same slot set and re-import deletion removes stale split assets.
    """
    source_path = CONV / f"{MAP}{suffix}.glb"
    asset_name = f"{MAP}{suffix}"
    parts = split_world_glb(
        source_path,
        REPO / "Intermediate" / "FractalWorldSplit" / asset_name,
        split_slots, asset_name)
    dest = f"/Game/WoT/Maps/Geo/{asset_name}"
    if EAL.does_directory_exist(dest):
        EAL.delete_directory(dest)

    params = unreal.ImportAssetParameters()
    params.is_automated = True
    params.override_pipelines = [unreal.SoftObjectPath(lightmap_pipeline())]
    mgr = unreal.InterchangeManager.get_interchange_manager_scripted()
    imported = []
    known_meshes = set()
    for part in parts:
        source = unreal.InterchangeManager.create_source_data(str(part["path"]))
        if not mgr.import_asset(dest, source, params):
            raise RuntimeError(
                f"interchange import_asset returned false for {part['path']}")
        mgr.wait_until_all_tasks_done(False)
        meshes = {
            asset.split(".")[0] for asset in EAL.list_assets(dest)
            if isinstance(unreal.load_asset(asset.split(".")[0]),
                          unreal.StaticMesh)
        }
        new_meshes = sorted(meshes - known_meshes,
                            key=lambda value: value.casefold())
        if len(new_meshes) != 1:
            raise RuntimeError(
                f"expected one new split mesh for {part['path']}, got "
                f"{new_meshes} from {sorted(meshes)}")
        known_meshes = meshes
        imported.append((new_meshes[0], part["material_slot"]))
    EAL.save_directory(dest)
    return imported


def import_stripped_bsp_parts(parts, suffix, world_map, masters):
    """Import and stamp one synthetic carrier for every canonical split part."""
    asset_root = f"{MAP}{suffix}"
    dest = f"/Game/WoT/Maps/Geo/{asset_root}"
    if EAL.does_directory_exist(dest):
        EAL.delete_directory(dest)
    params = unreal.ImportAssetParameters()
    params.is_automated = True
    params.override_pipelines = [unreal.SoftObjectPath(lightmap_pipeline())]
    mgr = unreal.InterchangeManager.get_interchange_manager_scripted()
    known_meshes = set()
    imported = []
    geometries = []
    assigned = 0
    carrier_dir = REPO / "Intermediate" / "BSPCarrierParts" / asset_root
    staged_dir = Path(unreal.Paths.convert_relative_path_to_full(
        unreal.Paths.project_intermediate_dir())) / "BSPCarrierStage" / asset_root
    carrier_dir.mkdir(parents=True, exist_ok=True)
    staged_dir.mkdir(parents=True, exist_ok=True)
    expected_role = "portal" if suffix == "_portals" else "main"
    for part in parts:
        if part["role"] != expected_role:
            raise RuntimeError(
                f"BSP carrier role mismatch for {part['asset_name']}: "
                f"{part['role']} != {expected_role}")
        asset_name = part["asset_name"]
        carrier = carrier_dir / f"{asset_name}_carrier.glb"
        geometry = write_stripped_carrier(part["path"], carrier, expected_role)
        # Interchange names the asset from the source filename. Always stage
        # under the canonical split filename before import.
        staged = staged_dir / f"{asset_name}.glb"
        staged.write_bytes(carrier.read_bytes())
        source = unreal.InterchangeManager.create_source_data(str(staged))
        if not mgr.import_asset(dest, source, params):
            raise RuntimeError(
                f"interchange import_asset returned false for {carrier}")
        mgr.wait_until_all_tasks_done(False)
        meshes = {
            asset.split(".")[0] for asset in EAL.list_assets(dest)
            if isinstance(unreal.load_asset(asset.split(".")[0]),
                          unreal.StaticMesh)
        }
        new_meshes = sorted(meshes - known_meshes,
                            key=lambda value: value.casefold())
        if len(new_meshes) != 1:
            raise RuntimeError(
                f"expected one new BSP carrier for {asset_name}, got "
                f"{new_meshes} from {sorted(meshes)}")
        known_meshes = meshes
        mesh_path = new_meshes[0]
        prep_lightmap(mesh_path)
        if expected_role == "main":
            fix_collision(mesh_path)
        assigned += assign_materials(mesh_path, world_map, masters)
        stamp_bsp_proof_carrier(
            mesh_path, geometry, expected_role, asset_name)
        imported.append((mesh_path, part["material_slot"]))
        geometries.append(geometry)
    EAL.save_directory(dest)
    return imported, geometries, assigned


def prep_lightmap(mesh_path):
    """Point the mesh's lightmap at the generated channel; whole-map mesh at
    UE1-era texel density (~32uu/texel) fits comfortably in 2048²."""
    mesh = unreal.load_asset(mesh_path)
    mesh.set_editor_property("light_map_coordinate_index", 1)
    mesh.set_editor_property("light_map_resolution", 2048)
    EAL.save_asset(mesh_path)


def fix_collision(mesh_path):
    """Strip auto-generated convex hulls; collide against the actual polys
    (period-correct for BSP world geometry)."""
    mesh = unreal.load_asset(mesh_path)
    # StaticMeshEditorSubsystem is unavailable in commandlet mode; setting the
    # trace flag alone works — complex-as-simple ignores any stored hulls.
    bs = mesh.get_editor_property("body_setup")
    bs.set_editor_property("collision_trace_flag",
                           unreal.CollisionTraceFlag.CTF_USE_COMPLEX_AS_SIMPLE)
    EAL.save_asset(mesh_path)


def fallback_mi(masters):
    """Diffuse-only fallback for slots whose texture failed to decode —
    otherwise they keep Interchange's PBR-default material (plastic sheen)."""
    full = "/Game/WoT/Materials/Instances/MI_Missing"
    if not EAL.does_asset_exist(full):
        png = CONV / "tex/DeveloperT/TypeMask_001clp.png"
        task = unreal.AssetImportTask()
        task.filename = str(png)
        task.destination_path = "/Game/WoT/Textures/DeveloperT"
        task.automated = True
        task.save = True
        ATT.import_asset_tasks([task])
        mi = ATT.create_asset("MI_Missing", "/Game/WoT/Materials/Instances",
                              unreal.MaterialInstanceConstant,
                              unreal.MaterialInstanceConstantFactoryNew())
        mi.set_editor_property("parent", masters[(False, False)])
        unreal.MaterialEditingLibrary.set_material_instance_texture_parameter_value(
            mi, "Tex", unreal.load_asset("/Game/WoT/Textures/DeveloperT/TypeMask_001clp"))
        EAL.save_asset(full)
    return full


def assign_materials(mesh_path, insts, masters):
    mesh = unreal.load_asset(mesh_path)
    missing = fallback_mi(masters)
    n = 0
    for i, smat in enumerate(mesh.static_materials):
        slot = str(smat.material_slot_name)
        smat.material_interface = unreal.load_asset(insts.get(slot, missing))
        mesh.set_material(i, smat.material_interface)
        n += slot in insts
    EAL.save_asset(mesh_path)
    return n


def stamp_bsp_proof_carrier(mesh_path, geometry, role, asset_name=None):
    """Stamp trusted real-geometry metadata onto a synthetic carrier."""
    sections = geometry["sections"]
    asset_name = asset_name or f"{MAP}{'_portals' if role == 'portal' else ''}"
    error = unreal.WoTBSPPayloadLibrary.stamp_carrier(
        unreal.load_asset(mesh_path),
        f"bsp:{MAP}__{asset_name}",
        role,
        int(geometry["vertex_count"]),
        int(geometry["index_count"]),
        unreal.Vector(*geometry["bounds_min"]),
        unreal.Vector(*geometry["bounds_max"]),
        geometry["tangent_algorithm"],
        [item["material_slot"] for item in sections],
        [int(item["first_index"]) for item in sections],
        [int(item["index_count"]) for item in sections],
        [int(item["min_vertex"]) for item in sections],
        [int(item["max_vertex"]) for item in sections],
        [bool(item["collision_enabled"]) for item in sections],
    )
    if error:
        raise RuntimeError(f"BSP proof carrier stamp failed: {error}")
    EAL.save_asset(mesh_path)


def ue1_rot(r):
    """UE1 rotator units (65536 = 360°) → UE5 Rotator.
    NB: unreal.Rotator's positional args are (roll, pitch, yaw)."""
    s = 360.0 / 65536.0
    return unreal.Rotator(roll=r[2] * s, pitch=r[0] * s, yaw=r[1] * s)


def ue1_light_color(props):
    """UE1 HSB (saturation inverted: 255 = white) → 8-bit Color."""
    import colorsys
    h = float(props.get("LightHue", 0)) / 255.0
    s = (255.0 - float(props.get("LightSaturation", 255))) / 255.0
    r, g, b = colorsys.hsv_to_rgb(h, s, 1.0)
    return unreal.Color(int(b * 255), int(g * 255), int(r * 255), 255)  # BGRA order


def place_semantics(eas):
    """Original lights + player starts from the map's T3D semantics JSON."""
    f = CONV / f"{MAP}.t3d.json"
    if not f.exists():
        return {}
    actors = json.loads(f.read_text())["actors"]
    lightning = {x["props"].get("LightTag", "").lower()
                 for x in actors if x["cls"].lower() == "lightning"} - {""}
    counts = {"Light": 0, "PlayerStart": 0}
    for a in actors:
        loc = unreal.Vector(*a["location"])
        if a["cls"] == "PlayerStart":
            # +55uu lift. UE1 mappers floor-snap the PlayerStart's OWN 40-uu-
            # half-height cylinder (UE1 PlayerStart CollisionHeight=40;
            # BSP down-trace over 104 retail starts: median floor+40, worst
            # floor+14), so the 46-uu-half-height pawn (shipped WOTPlayer.uc
            # 17x46 == our capsule) spawns up to ~32uu EMBEDDED; retail's
            # SpawnActor->FindSpot shoved it flush. Our AlwaysSpawn spawn path
            # has no such shove, so lift clear of the worst embed instead: the
            # pawn falls and lands at floor+46 — exactly retail's rest height —
            # for ANY lift >= ~32uu. The 55uu import lift is an intentional
            # divergence (PORT_NOTES); keep it in sync before changing it.
            loc.z += 55
            # AWoTPlayerStart carries the UE1 TeamNumber tag (engine APlayerStart
            # dropped it). T3D omits default-valued props, so an absent line IS
            # team 0 (UE1 PlayerStart default); 255 = explicit neutral. Drives the
            # Battle team-area spawn weighting (ChoosePlayerStart -4000/-20000).
            ps = eas.spawn_actor_from_class(unreal.WoTPlayerStart, loc)
            ps.set_actor_rotation(ue1_rot(a["rotation"]), False)
            ps.set_editor_property("team_number", int(a["props"].get("TeamNumber", 0)))
            counts["PlayerStart"] += 1
        elif a["cls"] == "Light":
            p = a["props"]
            # Same selection as _mesh_light_state: UE1 actor lighting only
            # considers a light whose bSpecialLit matches the actor's — pawns
            # and movers are False, so special-lit lights never light them.
            # LT_None / zero-brightness lights are dark; Lightning-gated
            # lights are off at rest.
            ltype = p.get("LightType", "LT_Steady").lower()
            bright = float(p.get("LightBrightness", 64))
            if (ltype in ("lt_none", "none") or p.get("bSpecialLit") == "True"
                    or p.get("Tag", "").lower() in lightning or bright <= 0):
                counts["LightSkipped"] = counts.get("LightSkipped", 0) + 1
                continue
            li = eas.spawn_actor_from_class(unreal.PointLight, loc)
            c = li.light_component
            # World-surface lighting is the injected UE1 lightmap (emissive
            # material, ignores lights). These lights exist for DYNAMIC
            # actors — pawns/hands/movers — exactly UE1's actor-lighting
            # split: movable, shadowless, analytic falloff.
            c.set_editor_property("mobility", unreal.ComponentMobility.MOVABLE)
            c.set_editor_property("cast_shadows", False)
            # actor-lighting channel: world surfaces (channel 0) carry baked
            # UE1 lightmaps and must not receive these; pawns/movers/hands
            # opt into channel 1. Transient effect lights will use channel 0.
            c.set_editor_property("lighting_channels",
                                  unreal.LightingChannels(channel0=False, channel1=True))
            c.set_editor_property("light_color", ue1_light_color(p))
            c.set_editor_property("attenuation_radius",
                                  float(p.get("LightRadius", 64)) * 25.0)
            c.set_editor_property("use_inverse_squared_falloff", False)
            # UE1 falloff is Hermite (2s^3-3s^2+1), s=d/R, in DISPLAY space
            # (the LightSqrt table). UE5's family is
            # (1-s^2)^E in linear space; E=5.3 is the least-squares fit to
            # hermite^2.2. The old E=2.0 was far too fat — it lit far pixels
            # the original left dark (measured as a gamma-like bend).
            c.set_editor_property("light_falloff_exponent", 5.3)
            # UE1 multiplied texture x light in DISPLAY space; UE5 is linear
            # with an sRGB encode at output. Matching the golden captures
            # therefore needs the UE1 brightness gamma-DECODED into linear
            # (measured: UE ~= golden^(1/2.2) before this fix — see
            # exports/reference/compare). MULT is the single linear-scale
            # calibration constant on top.
            # Waveform types (LT_Pulse etc.) place at the display-space
            # waveform mean — the same static stand-in _ml_bake uses.
            b = bright / 255.0 * _ML_TYPE_MEAN.get(ltype, 1.0)
            c.set_editor_property("intensity", (b ** 2.2) * MULT)
            # UE1 had no bounce lighting: direct + zone ambient only. Baked GI
            # fills original shadows and measures as a gamma-like bend vs the
            # golden set — off for parity (revisit as a deliberate divergence).
            c.set_editor_property("indirect_lighting_intensity", 0.0)
            counts["Light"] += 1
        elif a["cls"].endswith("ZoneInfo"):
            amb = float(a["props"].get("AmbientBrightness", 0))
            if amb > 0:
                counts["ambient_min"] = min(counts.get("ambient_min", 255.0), amb)
    return counts


def import_small_glb(glb_path, dest):
    """Import one non-world GLB (mover brush) as a static mesh."""
    params = unreal.ImportAssetParameters()
    params.is_automated = True
    mgr = unreal.InterchangeManager.get_interchange_manager_scripted()
    src = unreal.InterchangeManager.create_source_data(str(glb_path))
    if not mgr.import_asset(dest, src, params):
        return None
    mgr.wait_until_all_tasks_done(False)
    EAL.save_directory(dest)
    for a in EAL.list_assets(dest, recursive=True):
        o = unreal.load_asset(a.split(".")[0])
        if isinstance(o, unreal.StaticMesh):
            return a.split(".")[0]
    return None


MOVER_STATES = {
    "BumpOpenTimed": unreal.WoTMoverState.BUMP_OPEN_TIMED,
    "TriggerOpenTimed": unreal.WoTMoverState.TRIGGER_OPEN_TIMED,
    "TriggerToggle": unreal.WoTMoverState.TRIGGER_TOGGLE,
    "TriggerControl": unreal.WoTMoverState.TRIGGER_CONTROL,
    "StandOpenTimed": unreal.WoTMoverState.STAND_OPEN_TIMED,
    "TriggerPound": unreal.WoTMoverState.TRIGGER_POUND,
    "BumpButton": unreal.WoTMoverState.BUMP_BUTTON,
    "GradualTriggerOpenTimed": unreal.WoTMoverState.GRADUAL_TRIGGER_OPEN_TIMED,
    "GradualTriggerPound": unreal.WoTMoverState.GRADUAL_TRIGGER_POUND,
    "GradualTriggerToggle": unreal.WoTMoverState.GRADUAL_TRIGGER_TOGGLE,
}

# Only Engine.Mover states belong in InitialState. Subclass state labels are
# dispatched by their native AWoT*Mover implementation and therefore stamp None.
BASE_MOVER_STATE_NAMES = {
    "BumpOpenTimed", "TriggerOpenTimed", "TriggerToggle", "TriggerControl",
    "StandOpenTimed", "TriggerPound", "BumpButton",
}

# Retained as a compatibility hook for any future inert mover-family fallback.
DEFERRED_MOVER_CLASSES = set()
# Every Mover-family class movers2glb emits (kept in sync with its MOVER_CLASSES).
ALL_MOVER_CLASSES = {"Mover", "DoorMover", "LockableMover",
                     "PortcullisMover", "ExplodingMover", "RotatingMover",
                     "LoopMover", "TimedMover",
                     "AssertMover", "ElevatorMover", "AttachMover",
                     "GradualMover", "MixMover"} | DEFERRED_MOVER_CLASSES
_ml_mover_inst_cache = {}
_ml_mover_master_cache = None


def _meshlit_instance(mat, mats, textures):
    """MI_ML_<slug> for a BSP material slot on a MOVER mesh: parented to the
    M_WoT_MoverLit family (surface lighting) for opaque/masked, or the MeshLitT
    master for translucent; Tex = the surface texture. The UWoTMeshLight component
    pushes the baked light selection into a MID over this at BeginPlay. Returns
    the asset path, or None (slot absent from the manifest → caller leaves it
    on the shared fallback). NULL-texture faces ('__None' — mover builders
    leave unseen faces untextured) substitute Engine.DefaultTexture, the
    retail render-time stand-in (the mover pass)."""
    info = mats.get(mat)
    if not info:
        _ml_mover_inst_cache[mat] = None
        return None
    # Plane-split mover slots share the source material's MIC. The unique slot
    # name exists only to preserve a one-plane UE section.
    source_mat = info.get("source_material", mat)
    if source_mat in _ml_mover_inst_cache:
        return _ml_mover_inst_cache[source_mat]
    tkey = (info["package"], info["texture"])
    dangling = tkey not in textures
    flags = info["flags"]
    if flags & 0x4:                          # PF_Translucent
        master = make_meshlit_translucent_master()
    else:
        # Movers are surface-lit, not mesh-Gouraud — bind opaque/
        # masked mover slots to the surface master, not the M_WoT_MeshLit family.
        master = make_moverlit_masters()[bool(flags & 0x2)]   # PF_Masked
    path = "/Game/WoT/Materials/Instances"
    safe = source_mat.replace("-", "_")
    full = f"{path}/MI_ML_{safe}"
    MEL = unreal.MaterialEditingLibrary
    if not EAL.does_asset_exist(full):
        mi = ATT.create_asset(f"MI_ML_{safe}", path,
                              unreal.MaterialInstanceConstant,
                              unreal.MaterialInstanceConstantFactoryNew())
        mi.set_editor_property("parent", master)
        MEL.set_material_instance_texture_parameter_value(
            mi, "Tex", _import_default_texture() if dangling
            else unreal.load_asset(textures[tkey]))
    else:
        mi = unreal.load_asset(full)
        if mi.get_editor_property("parent") != master:
            mi.set_editor_property("parent", master)
    # SCALE knob lives on the MIC (per-instance scalar → tunes without a master
    # rebuild, unlike a baked graph constant). Only the surface family exposes it.
    if not (flags & 0x4):
        MEL.set_material_instance_scalar_parameter_value(mi, "MLSurfScale",
                                                         _ML_SURF_SCALE)
    EAL.save_asset(full)
    _ml_mover_inst_cache[source_mat] = full
    return full


def _meshlit_insts_for_mesh(mesh_path, mats, textures):
    """{slot_name: MI_ML path} for every resolvable slot on an imported mover
    mesh (unresolved slots omitted → assign_materials leaves them on fallback)."""
    mesh = unreal.load_asset(mesh_path)
    out = {}
    for smat in mesh.static_materials:
        slot = str(smat.material_slot_name)
        p = _meshlit_instance(slot, mats, textures)
        if p:
            out[slot] = p
    return out


def place_movers(eas, insts, masters, mats=None, textures=None):
    mdir = CONV / "movers" / MAP
    f = mdir / "movers.json"
    if not f.exists():
        return {}
    data = json.loads(f.read_text())
    counts = dict(movers=0, triggers=0, door_triggers=0,
                  stage_triggers=0, portcullis_triggers=0,
                  dispatchers=0, counters=0)
    # t3d Mover actors (props/Region/ScaleGlow) for the mesh-light bake
    t3d_movers = {}
    if GAMMA_SPACE:
        tf = CONV / f"{MAP}.t3d.json"
        if tf.exists():
            t3d_movers = {a["name"]: a
                          for a in json.loads(tf.read_text())["actors"]
                          if a["cls"] in ALL_MOVER_CLASSES}
        # mover-ONLY textures (never on a world surface, e.g. TileFlor2021) aren't
        # in the map's imported set — import them + merge their material info so
        # the MeshLit MICs resolve instead of falling to MI_Missing.
        mover_mats = data.get("materials", {})
        if mover_mats and mats is not None:
            mats = {**mats, **mover_mats}
            need = {(m["package"], m["texture"]) for m in mover_mats.values()}
            need -= set(textures or {})
            if need:
                textures = {**(textures or {}), **import_textures(need)}
    for mv in data["movers"]:
        dest = f"/Game/WoT/Maps/Geo/{MAP}/Movers/{mv['model']}"
        if EAL.does_directory_exist(dest):
            EAL.delete_directory(dest)
        mesh_path = import_small_glb(mdir / f"{mv['model']}.glb", dest)
        if not mesh_path:
            continue
        fix_collision(mesh_path)
        # Movers are BSP brushes, SURFACE-lit at runtime (dynamic movers have no
        # BAKED lightmap because it's computed live from leaf-permeating lights,
        # not because they're mesh-lit).
        # In the gamma scheme, bind the mesh to the M_WoT_MoverLit family (surface
        # radial + spotlight cone) and bake the light selection at the base
        # pose (below). Linear scheme keeps the old lit masters.
        if GAMMA_SPACE and mats is not None:
            assign_materials(mesh_path,
                             _meshlit_insts_for_mesh(mesh_path, mats, textures),
                             masters)
        else:
            assign_materials(mesh_path, insts, masters)
        # Ported mover subclasses get their native C++ actors. Dependency-bearing
        # deferred subclasses remain inert AWoTMovers so only their geometry lands.
        cls = mv.get("cls", "Mover")
        mover_classes = {
            "DoorMover": unreal.WoTDoorMover,
            "PortcullisMover": unreal.WoTPortcullisMover,
            "ExplodingMover": unreal.WoTExplodingMover,
            "RotatingMover": unreal.WoTRotatingMover,
            "LoopMover": unreal.WoTLoopMover,
            "TimedMover": unreal.WoTTimedMover,
            "AssertMover": unreal.WoTAssertMover,
            "ElevatorMover": unreal.WoTElevatorMover,
            "AttachMover": unreal.WoTAttachMover,
            "GradualMover": unreal.WoTGradualMover,
            "MixMover": unreal.WoTMixMover,
        }
        mover_cls = mover_classes.get(cls, unreal.WoTMover)
        a = eas.spawn_actor_from_class(mover_cls, unreal.Vector(*mv["base_pos"]))
        a.set_actor_rotation(ue1_rot(mv["base_rot"]), False)
        a.set_actor_label(mv["name"])
        a.set_editor_property("special_lit", mv.get("special_lit", False))
        mesh = a.get_editor_property("mesh")
        mesh.set_static_mesh(unreal.load_asset(mesh_path))
        mesh.set_editor_property("lighting_channels",
                                 unreal.LightingChannels(channel0=True, channel1=True))
        # Dynamic-light movers receive their retail per-surface base-pose
        # selection here. Runtime arms the existing movement/leaf seam below
        # BeginPlay; ordinary movers retain the historical actor-bake path.
        if GAMMA_SPACE and mv["name"] in t3d_movers:
            dynamic_light_mover = mv.get("dynamic_light_mover", False)
            _ml_bake(
                a, t3d_movers[mv["name"]], surface_zone=mv.get("zone"),
                surface_planes=(mv.get("surfaces", [])
                                if dynamic_light_mover
                                else None),
                surface_pose=((mv["base_pos"], mv["base_rot"])
                              if dynamic_light_mover else None),
            )
        nkeys = max((int(k) for k in mv["keys"]), default=0) + 1
        kp = [unreal.Vector(0, 0, 0)]
        kr = [unreal.Rotator(0, 0, 0)]
        for i in range(1, nkeys):
            k = mv["keys"].get(str(i), {})
            kp.append(unreal.Vector(*k.get("Pos", [0, 0, 0])))
            kr.append(ue1_rot(k.get("Rot", [0, 0, 0])))
        a.set_editor_property("key_pos", kp)
        a.set_editor_property("key_rot", kr)
        a.set_editor_property("move_time", mv["move_time"])
        a.set_editor_property("stay_open_time", mv["stay_open_time"])
        a.set_editor_property("delay_time", mv["delay_time"])
        # Deferred subclasses: inert (None) so the base FSM never slides them
        # open. DoorMover's directional port owns its dispatch (None). The ported
        # subclasses (PORTED_SUBCLASS_MOVERS) honor an authored BASE-FSM state —
        # UE1: an instance InitialState replaces the class's auto state
        # COMPLETELY, e.g. Battle2_02 AssertMover2 authors TriggerOpenTimed and
        # retail runs the base open/dwell/close with NO UnTrigger response —
        # while unauthored/custom states keep the native subclass FSM (None).
        # PortcullisMover is NOT stamped at all: its constructor default
        # (TriggerToggle) is what holds the raised gate put; stamping the JSON
        # base fallback (BumpOpenTimed — no shipped instance authors a state)
        # made the base Tick auto-drop every gate after StayOpenTime and let
        # pawn bumps re-raise a dropped one.
        authored_state = mv["initial_state"]
        if authored_state in BASE_MOVER_STATE_NAMES:
            state = MOVER_STATES[authored_state]
        else:
            state = unreal.WoTMoverState.NONE
            unreal.log(f"MOVERSTATE {mv['name']} {cls} "
                       f"InitialState={authored_state or '<class custom>'} -> native")
        if cls != "PortcullisMover":
            a.set_editor_property("initial_state", state)
        if cls in ("GradualMover", "MixMover") and state == unreal.WoTMoverState.NONE:
            a.set_editor_property("native_initial_state", unreal.Name(authored_state))
        # DoorMover(bLockable) / LockableMover(by class): inventory-key gate.
        if mv.get("lockable") or cls == "LockableMover":
            a.set_editor_property("lockable", True)
            a.set_editor_property("stay_unlocked", mv.get("stay_unlocked", False))
        if "unlocked" in mv:
            a.set_editor_property("unlocked", bool(mv["unlocked"]))
        a.set_editor_property("trigger_once_only", mv["once_only"])
        a.set_editor_property("damage_triggered", mv.get("damage_triggered", False))
        encroach_types = {
            "Stop": unreal.WoTMoverEncroachType.STOP_WHEN_ENCROACH,
            "Crush": unreal.WoTMoverEncroachType.CRUSH_WHEN_ENCROACH,
            "Return": unreal.WoTMoverEncroachType.RETURN_WHEN_ENCROACH,
            "Ignore": unreal.WoTMoverEncroachType.IGNORE_WHEN_ENCROACH,
        }
        a.set_editor_property("encroach_type",
                              encroach_types[mv.get("encroach_type", "Return")])
        a.set_editor_property("encroach_damage", int(mv.get("encroach_damage", 0)))
        a.set_editor_property("slave", bool(mv.get("slave", False)))
        if "return_group" in mv:
            a.set_editor_property("return_group", mv["return_group"])
        lock_props = {
            "locked_sound": "locked_sound_path",
            "unlock_sound": "un_lock_sound_path",
            "locked_message": "locked_message",
            "unlock_message": "un_lock_message",
            "locked_message_duration": "locked_message_duration",
            "unlock_message_duration": "un_lock_message_duration",
            "min_lock_sound_time": "min_lock_sound_time",
            "unlock_duration": "un_lock_duration",
        }
        for source, dest_prop in lock_props.items():
            if source in mv:
                a.set_editor_property(dest_prop, mv[source])
        a.set_editor_property("tag_name", mv["tag"])
        a.set_editor_property("event_name", mv["event"])
        if cls == "ExplodingMover":
            a.set_editor_property("explosion_size", mv["explosion_size"])
            a.set_editor_property("explosion_dimensions", mv["explosion_dimensions"])
            a.set_editor_property("wall_particle_size", mv["wall_particle_size"])
            a.set_editor_property("wood_particle_size", mv["wood_particle_size"])
            a.set_editor_property("glass_particle_size", mv["glass_particle_size"])
            a.set_editor_property("num_wall_chunks", mv["num_wall_chunks"])
            a.set_editor_property("num_wood_chunks", mv["num_wood_chunks"])
            a.set_editor_property("num_glass_chunks", mv["num_glass_chunks"])
            a.set_editor_property("health", mv["health"])
            a.set_editor_property("activated_by",
                                  [unreal.Name(v) for v in mv["activated_by"]])
            a.set_editor_property("wall_texture_name", mv["wall_texture_name"])
            a.set_editor_property("wood_texture_name", mv["wood_texture_name"])
            a.set_editor_property("glass_texture_name", mv["glass_texture_name"])
            a.set_editor_property("breaking_sound", mv["breaking_sound"])
            a.set_editor_property("translucent_glass", mv["translucent_glass"])
            a.set_editor_property("unlit_glass", mv["unlit_glass"])
            a.set_editor_property("only_triggerable", mv["only_triggerable"])
        elif cls == "RotatingMover":
            a.set_editor_property("rotate_rate", ue1_rot(mv["rotate_rate"]))
        elif cls == "LoopMover":
            a.set_editor_property("next_key_num", mv["next_key_num"])
        elif cls == "TimedMover":
            a.set_editor_property("delays", mv["delays"])
            a.set_editor_property("move_times", mv["move_times"])
            a.set_editor_property("sounds", [unreal.Name(s) for s in mv["sounds"]])
        elif cls == "AssertMover":
            a.set_editor_property("open_times", mv["open_times"])
            a.set_editor_property("close_times", mv["close_times"])
            a.set_editor_property("once_only_stop_open", mv["once_only_stop_open"])
            a.set_editor_property("wait_un_assert_time", mv["wait_unassert_time"])
            a.set_editor_property("close_immediately", mv["close_immediately"])
            a.set_editor_property("last_key_num", mv["last_key_num"])
        elif cls in ("GradualMover", "MixMover"):
            if "open_times" in mv:
                a.set_editor_property("open_times", mv["open_times"])
            if "close_times" in mv:
                a.set_editor_property("close_times", mv["close_times"])
            if "tags" in mv:
                a.set_editor_property("tags", [unreal.Name(v) for v in mv["tags"]])
            if "events" in mv:
                a.set_editor_property("events", [unreal.Name(v) for v in mv["events"]])
            if cls == "MixMover" and "attach_tag" in mv:
                a.set_editor_property("attach_tag", mv["attach_tag"])
        elif cls == "AttachMover" and "attach_tag" in mv:
            a.set_editor_property("attach_tag", mv["attach_tag"])
        counts["movers"] += 1
    for t in data["triggers"]:
        a = eas.spawn_actor_from_class(unreal.WoTTrigger, unreal.Vector(*t["location"]))
        a.set_actor_label(t["name"])
        a.set_editor_property("tag_name", t["tag"])
        a.set_editor_property("event_name", t["event"])
        a.set_editor_property("trigger_once_only", t["once_only"])
        a.set_editor_property("re_trigger_delay", t["retrigger"])
        cap = a.get_editor_property("capsule")
        cap.set_capsule_size(t["radius"], max(t["height"], t["radius"]))
        counts["triggers"] += 1
    door_directions = {
        "Forward": unreal.WoTDoorDirection.FORWARD,
        "Backward": unreal.WoTDoorDirection.BACKWARD,
    }
    for t in data.get("door_triggers", []):
        a = eas.spawn_actor_from_class(unreal.WoTDoorTrigger,
                                       unreal.Vector(*t["location"]))
        a.set_actor_label(t["name"])
        a.set_editor_property("tag_name", t["tag"])
        a.set_editor_property("event_name", t["event"])
        a.set_editor_property("trigger_once_only", t["once_only"])
        a.set_editor_property("re_trigger_delay", t["retrigger"])
        a.set_editor_property("direction", door_directions[t["direction"]])
        cap = a.get_editor_property("capsule")
        cap.set_capsule_size(t["radius"], max(t["height"], t["radius"]))
        counts["door_triggers"] += 1
    for t in data.get("stage_triggers", []):
        a = eas.spawn_actor_from_class(unreal.WoTStageTrigger,
                                       unreal.Vector(*t["location"]))
        a.set_actor_label(t["name"])
        a.set_editor_property("tag_name", t["tag"])
        a.set_editor_property("event_name", t["event"])
        a.set_editor_property("trigger_once_only", t["once_only"])
        a.set_editor_property("re_trigger_delay", t["retrigger"])
        a.set_editor_property("tag_dec", t["tag_dec"])
        a.set_editor_property("tag_inc", t["tag_inc"])
        a.set_editor_property("max_key_frame", t["max_key_frame"])
        a.set_editor_property("water_events",
                              [unreal.Name(v) for v in t["water_events"]])
        a.set_editor_property("dispatcher_tags",
                              [unreal.Name(v) for v in t["dispatcher_tags"]])
        cap = a.get_editor_property("capsule")
        cap.set_capsule_size(t["radius"], max(t["height"], t["radius"]))
        counts["stage_triggers"] += 1
    for t in data.get("portcullis_triggers", []):
        # The invisible archway trigger (AWoTPortcullis) that owns a
        # PortcullisMover. Its Event is the mover's Tag; BeginPlay resolves the
        # mover by matching TagName. Behavior is class defaults; it sits
        # Inactive until a citadel-activation path arms it (editor deferred).
        a = eas.spawn_actor_from_class(unreal.WoTPortcullis,
                                       unreal.Vector(*t["location"]))
        a.set_actor_label(t["name"])
        # Authored CollisionRadius/CollisionHeight override the class's 64x160
        # (9 instances across Battle2_02/Battle3_02 author radius 50/40/32).
        # Same UE1 half-height convention as the Trigger capsules above;
        # .get() keeps a pre-radius movers.json importable at class defaults.
        cap = a.get_editor_property("capsule")
        cap.set_capsule_size(float(t.get("radius", 64.0)),
                             max(float(t.get("height", 160.0)),
                                 float(t.get("radius", 64.0))))
        # Event names the owned mover's Tag. Two Battle3_02 archways ship an EMPTY
        # Event but a Tag that matches their intended mover (the original's
        # empty-Event `foreach AllActors(...,Event)` grabbed an arbitrary first
        # mover — a source map bug); fall back to Tag so they wire to intent.
        a.set_editor_property("mover_tag", t["event"] or t["tag"])
        counts["portcullis_triggers"] += 1
    for d in data["dispatchers"]:
        a = eas.spawn_actor_from_class(unreal.WoTDispatcher, unreal.Vector(*d["location"]))
        a.set_actor_label(d["name"])
        a.set_editor_property("tag_name", d["tag"])
        a.set_editor_property("out_events", [e or "None" for e in d["out_events"]])
        a.set_editor_property("out_delays", d["out_delays"])
        counts["dispatchers"] += 1
    for c in data["counters"]:
        a = eas.spawn_actor_from_class(unreal.WoTCounter, unreal.Vector(*c["location"]))
        a.set_actor_label(c["name"])
        a.set_editor_property("tag_name", c["tag"])
        a.set_editor_property("event_name", c["event"])
        a.set_editor_property("num_to_count", c["num"])
        counts["counters"] += 1
    return counts


def _make_corona_master(name, modulated):
    """UE1 corona sprite: canvas overlay icon, blend
    FB = Src + Dst*(1-Src) with a PER-CHANNEL dst factor. One translucent
    material can only scalarize that (max3 over-darkened the background
    behind colored halos), so it splits exactly into two passes:
      modulate (drawn first):  FB *= (1-Src)      -> Emissive=(1-Tex_disp*CB)^2.2
      additive (drawn second): FB += Src          -> Emissive=Tex_lin*CB^2.2
    with CB = Color*Bright in display space (driven per-frame by AWoTCorona).
    Both unlit, two-sided, no z-test (UE1 draws coronas post-world)."""
    full = f"/Game/WoT/Materials/{name}"
    if EAL.does_asset_exist(full):
        return unreal.load_asset(full)
    MEL = unreal.MaterialEditingLibrary
    m = ATT.create_asset(name, "/Game/WoT/Materials", unreal.Material,
                         unreal.MaterialFactoryNew())

    def expr(cls, x, y):
        return MEL.create_material_expression(m, cls, x, y)

    m.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    m.set_editor_property(
        "blend_mode",
        unreal.BlendMode.BLEND_MODULATE if modulated else unreal.BlendMode.BLEND_ADDITIVE)
    m.set_editor_property("two_sided", True)
    try:
        m.set_editor_property("disable_depth_test", True)
    except Exception as ex:  # noqa: BLE001
        unreal.log_warning(f"{name}: disable_depth_test not set ({ex})")
    tex = _lin_sampler(expr(unreal.MaterialExpressionTextureSampleParameter2D, -700, 0))
    tex.set_editor_property("parameter_name", "Tex")
    color = expr(unreal.MaterialExpressionVectorParameter, -700, 300)
    color.set_editor_property("parameter_name", "Color")
    color.set_editor_property("default_value", unreal.LinearColor(1, 1, 1, 1))
    bright = expr(unreal.MaterialExpressionScalarParameter, -700, 500)
    bright.set_editor_property("parameter_name", "Bright")
    bright.set_editor_property("default_value", 1.0)
    cb = expr(unreal.MaterialExpressionMultiply, -520, 380)
    MEL.connect_material_expressions(color, "", cb, "A")
    MEL.connect_material_expressions(bright, "", cb, "B")
    if modulated and GAMMA_SPACE:
        # display space throughout: FB *= 1 - Tex*CB, no transport pows
        srcd = expr(unreal.MaterialExpressionMultiply, -380, 150)
        MEL.connect_material_expressions(tex, "RGB", srcd, "A")
        MEL.connect_material_expressions(cb, "", srcd, "B")
        e = expr(unreal.MaterialExpressionOneMinus, -260, 150)
        MEL.connect_material_expressions(srcd, "", e, "")
    elif modulated:
        texd = expr(unreal.MaterialExpressionPower, -520, 0)
        texd.set_editor_property("const_exponent", 1.0 / 2.2)
        MEL.connect_material_expressions(tex, "RGB", texd, "Base")
        srcd = expr(unreal.MaterialExpressionMultiply, -380, 150)
        MEL.connect_material_expressions(texd, "", srcd, "A")
        MEL.connect_material_expressions(cb, "", srcd, "B")
        om = expr(unreal.MaterialExpressionOneMinus, -260, 150)
        MEL.connect_material_expressions(srcd, "", om, "")
        e = expr(unreal.MaterialExpressionPower, -140, 150)
        e.set_editor_property("const_exponent", 2.2)
        MEL.connect_material_expressions(om, "", e, "Base")
    elif GAMMA_SPACE:
        # display space: FB += Tex*Color*Bright, no transport pow
        e = expr(unreal.MaterialExpressionMultiply, -250, 150)
        MEL.connect_material_expressions(tex, "RGB", e, "A")
        MEL.connect_material_expressions(cb, "", e, "B")
        cb22 = None
    else:
        cb22 = expr(unreal.MaterialExpressionPower, -400, 380)
        cb22.set_editor_property("const_exponent", 2.2)
        MEL.connect_material_expressions(cb, "", cb22, "Base")
        e = expr(unreal.MaterialExpressionMultiply, -250, 150)
        MEL.connect_material_expressions(tex, "RGB", e, "A")
        MEL.connect_material_expressions(cb22, "", e, "B")
    MEL.connect_material_property(e, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    MEL.recompile_material(m)
    EAL.save_asset(full)
    return m


_SKIN_RE = re.compile(r"Texture'([\w\-]+)\.(?:[\w\-]+\.)*([\w\-]+)'")
_ML_SKIN_RE = re.compile(
    r"(?:Texture|WaveTexture)'([\w\-]+)\.(?:[\w\-]+\.)*([\w\-]+)'")
# Like _SKIN_RE but matches ONLY the exact class `Texture` (negative lookbehind
# rejects WetTexture'/FireTexture'/… parametric refs, which are flipbook-baked
# elsewhere, not static PNGs). Group 1 = package, group 2 = texture name.
_PART_RE = re.compile(r"(?<![A-Za-z])Texture'([\w\-]+)\.(?:[\w\-]+\.)*([\w\-]+)'")


def _load_leafbake():
    """exports/leaves/<MAP>.leaves.json (leafbake.py) or None."""
    f = EXPORTS / "leaves" / f"{MAP}.leaves.json"
    return json.loads(f.read_text()) if f.exists() else None


def place_coronas(eas):
    """Spawn AWoTCorona for every bCorona light with a Skin (the engine's
    corona gate). Lightning-controlled lights are skipped — off at start,
    same gate as the lightmap composer. With a leaf bake present, each
    corona carries its retail permeated-leaf set (exact visibility; the
    Reach distance gate stays as the no-bake fallback)."""
    f = CONV / f"{MAP}.t3d.json"
    if not f.exists():
        return 0
    actors = json.loads(f.read_text())["actors"]
    lightning_tags = {a["props"].get("LightTag", "").lower()
                      for a in actors if a["cls"].lower() == "lightning"} - {""}
    bake = _load_leafbake()
    baked_sets = bake["coronas"] if bake else {}
    add_master = _make_corona_master("M_WoT_CoronaAdd", False)
    mod_master = _make_corona_master("M_WoT_CoronaMod", True)
    n = 0
    for a in actors:
        p = a.get("props", {})
        if p.get("bCorona") != "True" or "Skin" not in p:
            continue
        if p.get("Tag", "").lower() in lightning_tags:
            continue
        sm = _SKIN_RE.match(p["Skin"])
        if not sm:
            continue
        pkg, name = sm.group(1), sm.group(2)
        png = CONV / "tex" / pkg / f"{name}.png"
        if not png.exists():
            continue
        tex_path = f"/Game/WoT/Textures/{pkg}/{name}"
        if not EAL.does_asset_exist(tex_path):
            task = unreal.AssetImportTask()
            task.filename = str(png)
            task.destination_path = f"/Game/WoT/Textures/{pkg}"
            task.automated = True
            task.save = True
            ATT.import_asset_tasks([task])
        ensure_texture_payload_binding(unreal, tex_path, pkg, name)
        if GAMMA_SPACE:
            # raw display bytes, like every _lin_sampler-paired texture. The
            # shipped corona skins already arrive srgb=False via the mesh-skin
            # import paths (shared flame/particle textures); this makes
            # place_coronas order-independent for corona-only skins.
            t = unreal.load_asset(tex_path)
            if t.get_editor_property("srgb"):
                t.set_editor_property("srgb", False)
                EAL.save_asset(tex_path)
        mics = {}
        for prefix, parent in (("MI_Corona_", add_master),
                               ("MI_CoronaMod_", mod_master)):
            mic_path = f"/Game/WoT/Materials/Instances/{prefix}{name}"
            if not EAL.does_asset_exist(mic_path):
                mic = ATT.create_asset(f"{prefix}{name}",
                                       "/Game/WoT/Materials/Instances",
                                       unreal.MaterialInstanceConstant,
                                       unreal.MaterialInstanceConstantFactoryNew())
                mic.set_editor_property("parent", parent)
                unreal.MaterialEditingLibrary.set_material_instance_texture_parameter_value(
                    mic, "Tex", unreal.load_asset(tex_path))
                EAL.save_asset(mic_path)
            else:
                mic = unreal.load_asset(mic_path)
                if mic.get_editor_property("parent") != parent:
                    mic.set_editor_property("parent", parent)
                    EAL.save_asset(mic_path)
            mics[prefix] = mic
        act = eas.spawn_actor_from_class(unreal.WoTCorona,
                                         unreal.Vector(*a["location"]))
        act.set_actor_label(f"{a['name']}_corona")
        act.set_editor_property("draw_scale", float(p.get("DrawScale", 1.0)))
        # Viewer-in-reach gate: UE1 draws a corona only when the camera's
        # leaf is in the light's permeating/volumetric lists. Measured on
        # Arena_12 golden: halos appear only within 25*(LightRadius+1).
        reach = 25.0 * (float(p.get("LightRadius", 64)) + 1.0)
        vol = float(p.get("VolumeRadius", 0))
        if vol > 0:
            reach = max(reach, 25.0 * (vol + 1.0))
        act.set_editor_property("reach", reach)
        if a["name"] in baked_sets:
            act.set_editor_property("permeating_leaves", baked_sets[a["name"]])
            act.set_editor_property("leaf_gated", True)
        # hue triangle + saturation lerp to white
        h = int(p.get("LightHue", 0))
        s = int(p.get("LightSaturation", 255))
        if h < 86:
            hue = ((85 - h) / 85.0, h / 85.0, 0.0)
        elif h < 171:
            hue = (0.0, (170 - h) / 85.0, (h - 85) / 85.0)
        else:
            hue = ((h - 170) / 85.0, 0.0, (255 - h) / 84.0)
        al = s / 255.0
        col = [c + al * (1.0 - c) for c in hue]
        act.set_editor_property("color", unreal.LinearColor(col[0], col[1], col[2], 1.0))
        act.get_editor_property("sprite").set_material(0, mics["MI_Corona_"])
        act.get_editor_property("sprite_mod").set_material(0, mics["MI_CoronaMod_"])
        n += 1
    return n


def place_leafmap(eas):
    """One AWoTLeafMap info actor carrying the shipped BSP's point->leaf walk
    data (leafbake.py), the lookup behind the corona permeated-leaf gate."""
    bake = _load_leafbake()
    if not bake:
        return 0
    nodes = bake["nodes"]
    act = eas.spawn_actor_from_class(unreal.WoTLeafMap, unreal.Vector(0, 0, 0))
    act.set_actor_label(f"LeafMap_{MAP}")
    act.set_editor_property("node_planes",
                            [c for pl in nodes["planes"] for c in pl])
    act.set_editor_property("node_children",
                            [c for ch in nodes["children"] for c in ch])
    act.set_editor_property("node_leaves",
                            [c for lf in nodes["leaves"] for c in lf])
    if "csg" in nodes:                # corona LOS bake (leafbake.py)
        act.set_editor_property("node_csg", [int(c) for c in nodes["csg"]])
        act.set_editor_property("root_outside",
                                bool(bake.get("root_outside", 0)))
    act.set_editor_property("num_leaves", int(bake["num_leaves"]))
    return 1


def _make_particle_master(name, modulated):
    """Sprite particle materials (STY_Translucent draw style):
    same two-pass split as the coronas, but z-tested (particles are in-world
    sprites) and brightness = per-instance custom data 0 (ScaleGlow)."""
    full = f"/Game/WoT/Materials/{name}"
    if EAL.does_asset_exist(full):
        return unreal.load_asset(full)
    MEL = unreal.MaterialEditingLibrary
    m = ATT.create_asset(name, "/Game/WoT/Materials", unreal.Material,
                         unreal.MaterialFactoryNew())

    def expr(cls, x, y):
        return MEL.create_material_expression(m, cls, x, y)

    m.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    m.set_editor_property(
        "blend_mode",
        unreal.BlendMode.BLEND_MODULATE if modulated else unreal.BlendMode.BLEND_ADDITIVE)
    m.set_editor_property("two_sided", True)
    m.set_editor_property("used_with_instanced_static_meshes", True)
    tex = _lin_sampler(expr(unreal.MaterialExpressionTextureSampleParameter2D, -700, 0))
    tex.set_editor_property("parameter_name", "Tex")
    glow = expr(unreal.MaterialExpressionPerInstanceCustomData, -700, 300)
    glow.set_editor_property("data_index", 0)
    if modulated and GAMMA_SPACE:
        # display space: FB *= 1 - Tex*glow
        srcd = expr(unreal.MaterialExpressionMultiply, -380, 150)
        MEL.connect_material_expressions(tex, "RGB", srcd, "A")
        MEL.connect_material_expressions(glow, "", srcd, "B")
        e = expr(unreal.MaterialExpressionOneMinus, -260, 150)
        MEL.connect_material_expressions(srcd, "", e, "")
    elif modulated:
        texd = expr(unreal.MaterialExpressionPower, -520, 0)
        texd.set_editor_property("const_exponent", 1.0 / 2.2)
        MEL.connect_material_expressions(tex, "RGB", texd, "Base")
        srcd = expr(unreal.MaterialExpressionMultiply, -380, 150)
        MEL.connect_material_expressions(texd, "", srcd, "A")
        MEL.connect_material_expressions(glow, "", srcd, "B")
        om = expr(unreal.MaterialExpressionOneMinus, -260, 150)
        MEL.connect_material_expressions(srcd, "", om, "")
        e = expr(unreal.MaterialExpressionPower, -140, 150)
        e.set_editor_property("const_exponent", 2.2)
        MEL.connect_material_expressions(om, "", e, "Base")
    elif GAMMA_SPACE:
        # display space: FB += Tex*glow; hardware blending accumulates in
        # display domain so stacked-sprite saturation is UE1-exact — no
        # approximation needed. (The linear scheme's StackComp curve, a
        # two-deep-stack-matched compromise, was deleted when gamma-space
        # merged: display accumulation solves that class exactly.)
        e = expr(unreal.MaterialExpressionMultiply, -380, 150)
        MEL.connect_material_expressions(tex, "RGB", e, "A")
        MEL.connect_material_expressions(glow, "", e, "B")
    else:
        # legacy linear scheme: per-sprite-exact additive E = Tex_lin *
        # glow^2.2 (stacked-sprite whitening is NOT reproducible here —
        # use GAMMA_SPACE for parity work)
        g22 = expr(unreal.MaterialExpressionPower, -520, 300)
        g22.set_editor_property("const_exponent", 2.2)
        MEL.connect_material_expressions(glow, "", g22, "Base")
        e = expr(unreal.MaterialExpressionMultiply, -380, 150)
        MEL.connect_material_expressions(tex, "RGB", e, "A")
        MEL.connect_material_expressions(g22, "", e, "B")
    MEL.connect_material_property(e, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    MEL.recompile_material(m)
    EAL.save_asset(full)
    return m


# ---- ParticleSprayer default resolution (self-contained .uc chain walk) ----

_SPRAYER_TDEF = dict(LifeSpan=1.0, Weight=1.0, MaxInitialVelocity=0.0,
                     MinInitialVelocity=0.0, MaxDrawScale=1.0, MinDrawScale=1.0,
                     MaxScaleGlow=1.0, MinScaleGlow=1.0, GrowPhase=0,
                     MaxGrowRate=0.0, MinGrowRate=0.0, FadePhase=0,
                     MaxFadeRate=0.0, MinFadeRate=0.0)
_SPRAYER_SCALARS = dict(Spread=45.0, Volume=10.0, MinVolume=0.0,
                        VolumeScalePct=0.5, PrimeCount=0.0,
                        VisibilityRadius=1600.0, VisibilityHeight=1600.0,
                        NumTemplates=0, bInitiallyOn=True, bLOSClip=False)
_uc_files = None
_uc_cache = {}


def _sprayer_uc(cls):
    """(parent, defaultproperties lines) for a class, from exports/script."""
    global _uc_files
    key = cls.lower()
    if key in _uc_cache:
        return _uc_cache[key]
    if _uc_files is None:
        _uc_files = {p.stem.lower(): p for p in (EXPORTS / "script").rglob("*.uc")}
    parent, lines = None, []
    p = _uc_files.get(key)
    if p:
        text = p.read_text(encoding="latin-1")
        mm = re.search(r"^\s*class\s+\w+\s+(?:expands|extends)\s+(\w+)", text, re.M | re.I)
        parent = mm.group(1) if mm else None
        dp = re.search(r"^defaultproperties\s*\{(.*?)^\}", text, re.M | re.S | re.I)
        if dp:
            lines = [ln.strip() for ln in dp.group(1).splitlines() if "=" in ln]
    _uc_cache[key] = (parent, lines)
    return parent, lines


def _apply_sprayer_props(res, items):
    """Apply prop assignments (name=value pairs) into the resolved dict."""
    for k, v in items:
        tm = re.match(r"Templates\((\d+)\)$", k)
        pm = re.match(r"Particles\((\d+)\)$", k)
        if tm:
            slot = int(tm.group(1))
            for fk, fv in re.findall(r"(\w+)=([\d.+-]+)", v):
                if fk in _SPRAYER_TDEF:
                    res["templates"].setdefault(slot, dict(_SPRAYER_TDEF))[fk] = float(fv)
        elif pm:
            # Static-PNG particles only (plain Texture'Pkg.Group.Name'); the
            # parametric WetTexture/FireTexture particles are flipbook-baked
            # elsewhere and never match this. Store the package-qualified key
            # (authoritative — first component is the package) so the lookup
            # can't alias same-named textures across packages.
            nm = _PART_RE.search(v)
            if nm:
                res["particles"][int(pm.group(1))] = _tkey(
                    nm.group(1), nm.group(2))
        elif k == "Gravity":
            for ax, fv in re.findall(r"([XYZ])=([\d.+-]+)", v):
                res["gravity"]["XYZ".index(ax)] = float(fv)
        elif k == "Rotation":
            for ax, fv in re.findall(r"(Pitch|Yaw|Roll)=([\d.+-]+)", v):
                res["rotation"][("Pitch", "Yaw", "Roll").index(ax)] = float(fv)
        elif k in _SPRAYER_SCALARS:
            res[k] = v.strip().lower() == "true" if k.startswith("b") \
                else float(v)
        elif k == "bOn":
            res["bOn"] = v.strip().lower() == "true"


def resolve_sprayer(actor):
    """t3d sprayer actor → resolved dict (chain defaults base→leaf, then
    instance overrides). Struct slot assignments merge per field, matching
    UE1 defaultproperties semantics (unset fields inherit)."""
    res = dict(_SPRAYER_SCALARS)
    res.update(templates={}, particles={}, gravity=[0.0, 0.0, 0.0],
               rotation=[16384.0, 0.0, 0.0], bOn=None)
    chain, c, seen = [], actor["cls"], set()
    while c and c.lower() not in seen and c.lower() != "actor":
        seen.add(c.lower())
        chain.append(c)
        c = _sprayer_uc(c)[0]
    for cls in reversed(chain):
        lines = _sprayer_uc(cls)[1]
        _apply_sprayer_props(
            res, [(ln.split("=", 1)[0].strip(), ln.split("=", 1)[1].strip())
                  for ln in lines])
    _apply_sprayer_props(res, list(actor.get("props", {}).items()))
    if actor.get("rotation"):
        res["rotation"] = actor["rotation"]
    if res["bOn"] is None:
        res["bOn"] = res["bInitiallyOn"]
    return res


def sprayer_texture_names():
    """Particle texture names needed by this map's sprayer-family actors."""
    f = CONV / f"{MAP}.t3d.json"
    if not f.exists():
        return set()
    out = set()
    for a in json.loads(f.read_text())["actors"]:
        c, seen = a["cls"], set()
        while c and c.lower() not in seen:
            seen.add(c.lower())
            c = _sprayer_uc(c)[0]
        if "particlesprayer" in seen:
            out |= set(resolve_sprayer(a)["particles"].values())
    return out


def place_sprayers(eas, textures):
    """Spawn AWoTParticleSprayer for placed ParticleSprayer-family actors."""
    f = CONV / f"{MAP}.t3d.json"
    if not f.exists():
        return 0
    add_master = _make_particle_master("M_WoT_ParticleAdd", False)
    mod_master = _make_particle_master("M_WoT_ParticleMod", True)
    MEL = unreal.MaterialEditingLibrary
    n = 0
    for a in json.loads(f.read_text())["actors"]:
        chain, c, seen = set(), a["cls"], set()
        while c and c.lower() not in seen:
            seen.add(c.lower())
            chain.add(c.lower())
            c = _sprayer_uc(c)[0]
        if "particlesprayer" not in chain:
            continue
        res = resolve_sprayer(a)
        ntpl = int(res["NumTemplates"])
        if ntpl <= 0 or not res["bOn"]:
            unreal.log(f"SPRAYER-SKIP {a['name']} ({a['cls']}): "
                       f"templates={ntpl} on={res['bOn']}")
            continue
        tpls, adds, mods = [], [], []
        ok = True
        for i in range(ntpl):
            texname = res["particles"].get(i)
            tinfo = TEXINFO.get(texname)
            if not texname or texname not in textures or tinfo is None:
                unreal.log(f"SPRAYER-SKIP {a['name']} ({a['cls']}): "
                           f"slot {i} texture {texname!r} unavailable")
                ok = False
                break
            t = unreal.WoTParticleTemplate()
            src = res["templates"].get(i, dict(_SPRAYER_TDEF))
            for fk, prop in (("LifeSpan", "life_span"), ("Weight", "weight"),
                             ("MaxInitialVelocity", "max_initial_velocity"),
                             ("MinInitialVelocity", "min_initial_velocity"),
                             ("MaxDrawScale", "max_draw_scale"),
                             ("MinDrawScale", "min_draw_scale"),
                             ("MaxScaleGlow", "max_scale_glow"),
                             ("MinScaleGlow", "min_scale_glow"),
                             ("MaxGrowRate", "max_grow_rate"),
                             ("MinGrowRate", "min_grow_rate"),
                             ("MaxFadeRate", "max_fade_rate"),
                             ("MinFadeRate", "min_fade_rate")):
                t.set_editor_property(prop, float(src[fk]))
            t.set_editor_property("grow_phase", int(src["GrowPhase"]))
            t.set_editor_property("fade_phase", int(src["FadePhase"]))
            t.set_editor_property("tex_width", float(tinfo["w"]))
            t.set_editor_property("tex_height", float(tinfo["h"]))
            tpls.append(t)
            pair = []
            slug = _slug(texname)
            for prefix, parent in (("MI_PSAdd_", add_master),
                                   ("MI_PSMod_", mod_master)):
                mic_path = f"/Game/WoT/Materials/Instances/{prefix}{slug}"
                if not EAL.does_asset_exist(mic_path):
                    mic = ATT.create_asset(f"{prefix}{slug}",
                                           "/Game/WoT/Materials/Instances",
                                           unreal.MaterialInstanceConstant,
                                           unreal.MaterialInstanceConstantFactoryNew())
                    mic.set_editor_property("parent", parent)
                    MEL.set_material_instance_texture_parameter_value(
                        mic, "Tex", unreal.load_asset(textures[texname]))
                    EAL.save_asset(mic_path)
                else:
                    mic = unreal.load_asset(mic_path)
                    if mic.get_editor_property("parent") != parent:
                        mic.set_editor_property("parent", parent)
                        EAL.save_asset(mic_path)
                pair.append(mic)
            adds.append(pair[0])
            mods.append(pair[1])
        if not ok:
            continue
        act = eas.spawn_actor_from_class(unreal.WoTParticleSprayer,
                                         unreal.Vector(*a["location"]))
        act.set_actor_rotation(ue1_rot(res["rotation"]), False)
        act.set_actor_label(a["name"])
        act.set_editor_property("spread", float(res["Spread"]))
        act.set_editor_property("volume", float(res["Volume"]))
        act.set_editor_property("min_volume", float(res["MinVolume"]))
        act.set_editor_property("gravity", unreal.Vector(*res["gravity"]))
        # UE1 reaches steady state within seconds of the player looking;
        # prime long enough that golden-pose shots start there too
        act.set_editor_property("prime_count", max(float(res["PrimeCount"]), 90.0))
        act.set_editor_property("volume_scale_pct", float(res["VolumeScalePct"]))
        act.set_editor_property("visibility_radius", float(res["VisibilityRadius"]))
        act.set_editor_property("visibility_height", float(res["VisibilityHeight"]))
        act.set_editor_property("on", bool(res["bOn"]))
        act.set_editor_property("los_clip", bool(res["bLOSClip"]))
        act.set_editor_property("templates", tpls)
        act.set_editor_property("add_materials", adds)
        act.set_editor_property("mod_materials", mods)
        n += 1
    return n


CLASS2MESH = {}
for _c2m in (CONV / "class2mesh.json",
             EXPORTS / "inventory" / "class2mesh.json"):
    if _c2m.exists():
        CLASS2MESH = json.loads(_c2m.read_text())
        break


# ---- UE1 mesh actor lighting (gamma-space port) -------------------------
# Static bake per placed
# decoration: light selection done here at import with an
# editor LOS trace; the formula itself runs in M_WoT_MeshLit's Custom HLSL
# with per-actor params pushed by UWoTMeshLight at BeginPlay.

_ML_TYPE_MEAN = {"lt_steady": 1.0, **{
    light_type.lower(): shape[0]
    for light_type, shape in WAVEFORM_SHAPES.items()
}}


def _fget_hsv(h, s, v):
    """HSV→RGB lighting conversion -> (r, g, b) floats (same as lightmaps.py)."""
    import math
    b = v * 1.4 / 255.0
    b *= 0.70 / (0.01 + math.sqrt(b))
    b = min(max(b, 0.0), 1.0)
    if h < 86:
        hue = ((85 - h) / 85.0, h / 85.0, 0.0)
    elif h < 171:
        hue = (0.0, (170 - h) / 85.0, (h - 85) / 85.0)
    else:
        hue = ((h - 170) / 85.0, 0.0, (255 - h) / 84.0)
    return tuple((c + s / 255.0 * (1.0 - c)) * b for c in hue)


def _rot_vector(rotation):
    """Forward unit vector of a rotator = (Cos(Pitch)Cos(Yaw),
    Cos(Pitch)Sin(Yaw), Sin(Pitch)); roll cancels. UE1 rotator units
    (65536 = 360deg), quantized through the 16384-entry sine table
    (same as lightmaps.rot_vector). Positions/dirs are 1:1 UE1==UE5 here."""
    import math

    def sintab(i):
        return math.sin(((int(i) >> 2) & 16383) * (2.0 * math.pi / 16384.0))
    pitch, yaw = rotation[0], rotation[1]
    cp, sp = sintab(int(pitch) + 16384), sintab(pitch)
    cy, sy = sintab(int(yaw) + 16384), sintab(yaw)
    return (cp * cy, cp * sy, sp)


_ml_cache = None
_zone_export_cache = None
_ml_bsp_cache = None
_ml_wave_group_cache = None


def _zone_export_data():
    """Expanded per-map zones export, accepting the legacy list shape."""
    global _zone_export_cache
    if _zone_export_cache is not None:
        return _zone_export_cache
    zone_file = EXPORTS / "zones" / f"{MAP}.zones.json"
    if not zone_file.exists():
        _zone_export_cache = {}
    else:
        data = json.loads(zone_file.read_text(encoding="utf-8"))
        _zone_export_cache = ({"zones": data} if isinstance(data, list)
                              else data)
    return _zone_export_cache


def _mesh_light_bsp():
    """Cached shared point/solid tree, or None for an unregenerated export."""
    global _ml_bsp_cache
    if _ml_bsp_cache is None:
        data = _zone_export_data()
        if data.get("zone_bsp") and data.get("leaf_bsp"):
            _ml_bsp_cache = BspTree.from_export(data)
        else:
            _ml_bsp_cache = False
    return _ml_bsp_cache or None


def _mesh_light_wave_groups():
    """Ordered light-wave selector and material constants from zones.light_anim."""
    global _ml_wave_group_cache
    if _ml_wave_group_cache is None:
        data = _zone_export_data()
        names = data.get("light_actors", [])
        props = data.get("light_props", [])
        anim = data.get("light_anim", [])
        if not names and not props and not anim:
            _ml_wave_group_cache = (
                {}, (1.0, 1.0, 1.0, 1.0), (0.0, 0.0, 0.0, 0.0))
        else:
            _ml_wave_group_cache = light_wave_group_selectors(
                names, props, anim)
    return _ml_wave_group_cache


def _mesh_light_state():
    """(lights, zones, level_brightness) parsed once from the t3d.
    lights: dictionaries keyed by actor name and retail selection properties.
    Named rows retain radius, raw brightness, special-lit/emission state,
    display-ready color, and spotlight data. Lightning-gated / LT_None rows
    remain candidates at rest but never reach the material light arrays.
    zones: name.lower() -> ambient (r,g,b) via the HSV lighting conversion."""
    global _ml_cache
    if _ml_cache is not None:
        return _ml_cache
    f = CONV / f"{MAP}.t3d.json"
    actors = json.loads(f.read_text())["actors"] if f.exists() else []
    actors_by_name = {a["name"]: a for a in actors}
    lightning = {a["props"].get("LightTag", "").lower()
                 for a in actors if a["cls"].lower() == "lightning"} - {""}
    export_data = _zone_export_data()
    exported_names = export_data.get("light_actors", [])
    exported_props = export_data.get("light_props", [])
    wave_selectors, _wave_means, _wave_amplitudes = _mesh_light_wave_groups()
    retail_props = {
        name: exported_props[index]
        for index, name in enumerate(exported_names)
        if index < len(exported_props)
    }
    considered_names = set(exported_names) if exported_names else None
    level_b = 1.0
    zones = {}
    lights = []
    for a in actors:
        p = a.get("props", {})
        cls = a["cls"].lower()
        if cls == "levelinfo":
            level_b = float(p.get("Brightness", 1.0))
        # zone-ambient sources: any ZoneInfo subclass, WaterZone (expands
        # WOTZoneInfo — name doesn't end in "zoneinfo"), and LevelInfo (extends
        # ZoneInfo; the engine's zone-actor lookup falls back to it for zone 0, so a
        # pivot-in-solid mover reads the LevelInfo's AmbientBrightness).
        if cls.endswith("zoneinfo") or cls in ("waterzone", "levelinfo"):
            zones[a["name"].lower()] = _fget_hsv(
                int(float(p.get("AmbientHue", 0))),
                int(float(p.get("AmbientSaturation", 255))),
                int(float(p.get("AmbientBrightness", 0))))
            if cls == "levelinfo":
                # zone-0 fallback key: _ml_bake resolves an unresolved/None
                # surface zone to "" (UE1 Zones[0].ZoneActor==NULL -> LevelInfo)
                zones[""] = zones[a["name"].lower()]
        # Only actors referenced by the shipped permeating table participate.
        # For legacy exports with no table, retain the broad scan. Every light
        # property comes through the lightmap baker's .uc-chain resolver, so
        # class-default SunLight/PixieSprayerBlue records agree across export,
        # import baking, and runtime table generation.
        if considered_names is not None and a["name"] not in considered_names:
            continue
        # UE1 exports can contain duplicate object-name records (Arena_01's
        # Light63); match zones.py's by-name last-record policy so exactly one
        # resolved row backs every exported name.
        if actors_by_name.get(a["name"]) is not a:
            continue
        resolved = resolve_light(a)
        ltype = str(resolved["LightType"])
        exact = retail_props.get(a["name"])
        bright = (int(exact[0]) if exact is not None
                  else int(resolved["LightBrightness"]))
        gated = str(p.get("Tag", "")).lower() in lightning
        emits = (bool(exact[3]) if exact is not None and len(exact) > 3
                  else not gated and ltype.lower() not in ("lt_none", "none"))
        emits = emits and bright != 0
        mean = _ML_TYPE_MEAN.get(ltype.lower(), 1.0)
        col = _fget_hsv(int(resolved["LightHue"]),
                        int(resolved["LightSaturation"]), 255)
        scale = (bright / 255.0) * mean            # * level_b applied below
        # LE_Spotlight: bake the cone (Sine = 1 - LightCone/256) + axis so the
        # surface master can gate to the cone. Sine < 0 = omni.
        # Spotlight-class actors resolve LE_Spotlight + LightCone=128 through
        # the same defaults chain even when neither value is serialized.
        leffect = str(resolved["LightEffect"])
        if leffect == "LE_Spotlight":
            sine = 1.0 - float(resolved["LightCone"]) / 256.0
            ldir = _rot_vector(resolved["rotation"])
        else:
            sine, ldir = -1.0, (0.0, 0.0, 0.0)
        lights.append(dict(
            name=a["name"],
            location=tuple(resolved["location"]),
            radius=(float(exact[1]) if exact is not None
                    else world_light_radius(resolved["cls"],
                                            resolved["LightRadius"])),
            color=tuple(c * scale for c in col),
            sine=sine,
            direction=ldir,
            brightness=bright,
            special_lit=(bool(exact[2]) if exact is not None else
                         bool(resolved["bSpecialLit"])),
            emits=emits,
            wave_group=wave_selectors.get(a["name"], -1),
        ))
    for light in lights:
        light["color"] = tuple(c * level_b for c in light["color"])
    if considered_names is not None:
        resolved_names = {light["name"] for light in lights}
        missing = sorted(considered_names - resolved_names)
        if missing:
            raise ValueError("Permeating lights missing from resolved T3D: "
                             + ", ".join(missing))
    _ml_cache = (lights, zones, level_b)
    return _ml_cache


def _ml_class_prop(cls, key, default):
    """Class-default property via the .uc chain (instance props override)."""
    c, seen = cls, set()
    while c and c.lower() not in seen:
        seen.add(c.lower())
        parent, lines = _sprayer_uc(c)
        for ln in lines:
            k, _, v = ln.partition("=")
            if k.strip() == key:
                return v.strip()
        c = parent
    return default


_ZONE_RE = re.compile(r"Zone=\w+'[\w\-]+\.([\w\-]+)'")
_LEAF_RE = re.compile(r"\biLeaf=(-?\d+)")


def _ml_bool(value):
    return value is True or str(value).lower() == "true"


def _ml_zone_light_cap(data, zone_number):
    """High-quality steady-state cap: actor-zone MaxLightCount, 1..16."""
    for row in data.get("ambient_zones", []):
        if int(row.get("zone", -1)) == int(zone_number):
            return max(1, min(int(row.get("max_light_count", 6)), 16))
    return 6


_MESH_AMBIENT = object()   # _ml_bake sentinel: decoration (mesh) ambient path


def _ml_bake(act, a, surface_zone=_MESH_AMBIENT, surface_planes=None,
             surface_pose=None):
    """Bake the UE1 light selection into the actor's UWoTMeshLight.

    surface_zone: pass the mover's point-in-BSP zone actor name (or None) to
    use the SURFACE ambient model (zone hsv*_ML_SURF_AMB_SCALE, no
    AmbientGlow — movers are brushes). Omit (sentinel) for the MESH ambient
    model used by decorations (AmbientGlow + full hsv of the T3D Region).

    ``surface_planes`` is present only for authored bDynamicLightMover actors.
    Each row is one converter-guaranteed planar material section; its selection
    uses SetupMoverSurface rather than the SetupForActor path below."""
    comp = act.get_editor_property("mesh_light")
    _wave_selectors, wave_means, wave_amplitudes = _mesh_light_wave_groups()
    comp.set_editor_property("wave_means", unreal.Vector4(*wave_means))
    comp.set_editor_property(
        "wave_amplitudes", unreal.Vector4(*wave_amplitudes))
    p = a.get("props", {})
    cls = a["cls"]
    sglow = float(p.get("ScaleGlow", _ml_class_prop(cls, "ScaleGlow", 1.0)))
    aglow = float(p.get("AmbientGlow", _ml_class_prop(cls, "AmbientGlow", 0.0)))
    unlit = _ml_bool(p.get("bUnlit", _ml_class_prop(cls, "bUnlit", "False")))
    if unlit:
        # flat unlit color, no lights, no zone ambient
        g = min(max(sglow * 0.5 + aglow / 256.0, 0.0), 1.0)
        comp.set_editor_property("ambient", unreal.LinearColor(g, g, g, 1.0))
        comp.set_editor_property("scale_glow", sglow)
        comp.set_editor_property("baked", True)
        return
    lights, zones, _ = _mesh_light_state()
    loc = tuple(surface_pose[0]) if surface_pose else a["location"]
    export_data = _zone_export_data()
    tree = _mesh_light_bsp()
    region = tree.point_region(loc) if tree else None
    if region is not None:
        serialized = _LEAF_RE.search(p.get("Region", ""))
        if serialized and int(serialized.group(1)) != region.leaf:
            unreal.log_warning(
                f"MESH-LIGHT LEAF MISMATCH {a['name']}: "
                f"Region.iLeaf={serialized.group(1)} descent={region.leaf}; "
                "trusting compiled BSP descent")
    if surface_zone is not _MESH_AMBIENT:
        # MOVER surface ambient: the mover's runtime zone (point-in-BSP), scaled
        # to the world-surface display multiplier. None (pivot in solid: UE1
        # Zones[0].ZoneActor==NULL) resolves via the "" key to the LevelInfo's
        # ambient — LevelInfo extends ZoneInfo and authors AmbientBrightness on
        # several Battle maps.
        za = zones.get((surface_zone or "").lower(), (0.0, 0.0, 0.0))
        amb = unreal.LinearColor(za[0] * _ML_SURF_AMB_SCALE, za[1] * _ML_SURF_AMB_SCALE,
                                 za[2] * _ML_SURF_AMB_SCALE, 1.0)
    else:
        # DECORATION mesh ambient: AmbientGlow/255 + full zone hsv (Region).
        zm = _ZONE_RE.search(p.get("Region", ""))
        za = zones.get(zm.group(1).lower(), (0.0, 0.0, 0.0)) if zm else (0.0, 0.0, 0.0)
        c0 = aglow / 255.0
        amb = unreal.LinearColor(c0 + za[0], c0 + za[1], c0 + za[2], 1.0)
    if surface_planes is not None:
        actor_names = export_data.get("light_actors", [])
        run = (export_data.get("leaf_lights", {}).get(str(region.leaf), [])
               if region is not None and region.leaf >= 0 else [])
        permeating_names = [actor_names[int(index)] for index in run
                            if 0 <= int(index) < len(actor_names)]
        lights_by_name = {light["name"]: light for light in lights}
        permeating = [lights_by_name[name] for name in permeating_names
                      if name in lights_by_name]
        mover_special = _ml_bool(p.get(
            "bSpecialLit", _ml_class_prop(cls, "bSpecialLit", "False")))
        mover_rotation = tuple(surface_pose[1]) if surface_pose else a["rotation"]
        baked_surfaces = []
        for surface in surface_planes:
            world_origin, world_normal = transform_plane(
                loc, mover_rotation, surface["origin"], surface["normal"])
            selected = select_mover_surface_lights(
                permeating, world_origin, world_normal, mover_special,
                emitting_only=True)
            if len(selected) > _ML_MOVER_LIGHTS:
                unreal.log_error(
                    f"MOVER-LIGHT OVERFLOW {a['name']} {surface['slot']}: "
                    f"selected {len(selected)} lights, material contract is "
                    f"{_ML_MOVER_LIGHTS}; "
                    "truncating loudly")
            selected = selected[:_ML_MOVER_LIGHTS]
            row = unreal.WoTMoverSurfaceLight()
            row.set_editor_property("material_slot", unreal.Name(surface["slot"]))
            row.set_editor_property("local_origin", unreal.Vector(*surface["origin"]))
            row.set_editor_property("local_normal", unreal.Vector(*surface["normal"]))
            row.set_editor_property("light_pos_r", [
                unreal.Vector4(*light["location"],
                               1.0 / max(light["radius"], 1.0))
                for light in selected
            ])
            row.set_editor_property("light_color", [
                unreal.LinearColor(*light["color"], 1.0)
                for light in selected
            ])
            row.set_editor_property("light_wave_group", [
                int(light["wave_group"]) for light in selected
            ])
            row.set_editor_property("light_dir_cone", [
                unreal.Vector4(*light["direction"], light["sine"])
                for light in selected
            ])
            baked_surfaces.append(row)
        comp.set_editor_property("mover_surfaces", baked_surfaces)
        comp.set_editor_property("light_pos_r", [])
        comp.set_editor_property("light_color", [])
        comp.set_editor_property("light_wave_group", [])
        comp.set_editor_property("light_dir_cone", [])
        comp.set_editor_property("ambient", amb)
        comp.set_editor_property("scale_glow", sglow)
        comp.set_editor_property("baked", True)
        return
    # Retail SetupForActor starts from this compiled leaf's shipped permeating
    # run. AddConsider filters special-lit/radius, tags by RAW LightBrightness,
    # and the zero-extent model LineCheck tests the solid CSG hull. The static
    # steady state retains only visible members, then applies threshold/cap.
    selected = []
    if tree is not None and region.leaf >= 0:
        actor_names = export_data.get("light_actors", [])
        run = export_data.get("leaf_lights", {}).get(str(region.leaf), [])
        permeating = [actor_names[int(index)] for index in run
                      if 0 <= int(index) < len(actor_names)]
        selected = select_mesh_lights(
            actor_location=loc,
            actor_special_lit=_ml_bool(p.get(
                "bSpecialLit", _ml_class_prop(cls, "bSpecialLit", "False"))),
            max_lights=_ml_zone_light_cap(export_data, region.zone),
            permeating_names=permeating,
            lights_by_name={light["name"]: light for light in lights},
            line_check=tree.line_check,
        )
    sel_pos, sel_col, sel_group, sel_dir = [], [], [], []
    for light in selected:
        pos, r, col = light["location"], light["radius"], light["color"]
        sine, ldir = light["sine"], light["direction"]
        sel_pos.append(unreal.Vector4(pos[0], pos[1], pos[2], 1.0 / max(r, 1.0)))
        sel_col.append(unreal.LinearColor(col[0], col[1], col[2], 1.0))
        sel_group.append(int(light["wave_group"]))
        # xyz = spotlight cone axis, w = Sine (surface master; <0 = omni).
        sel_dir.append(unreal.Vector4(ldir[0], ldir[1], ldir[2], sine))
    comp.set_editor_property("light_pos_r", sel_pos)
    comp.set_editor_property("light_color", sel_col)
    comp.set_editor_property("light_wave_group", sel_group)
    comp.set_editor_property("light_dir_cone", sel_dir)
    comp.set_editor_property("ambient", amb)
    comp.set_editor_property("scale_glow", sglow)
    comp.set_editor_property("baked", True)


_ROTOFF_RE = re.compile(r"\(Pitch=(-?\d+),Yaw=(-?\d+),Roll=(-?\d+)\)")


def place_rotators(eas):
    """Spawn AWoTActorRotator for placed Legend.ActorRotator systems (B2_01 /
    B3_01 teleporter sprayer orbits + flies/twotriangles swarms; no other MP
    map has any). Retail's GetActor links every actor whose Tag matches the
    rotator's Event and CLONES itself per extra match (each clone
    re-randomizes) — mirrored here by spawning one rotator per driven actor;
    bLongDistance instead drives all matches about their own origins from a
    single rotator. Links resolve by actor label (placement preserves t3d
    names); driven roots are forced MOVABLE."""
    f = CONV / f"{MAP}.t3d.json"
    if not f.exists() or not hasattr(unreal, "WoTActorRotator"):
        return 0
    actors = json.loads(f.read_text())["actors"]
    rotators = [a for a in actors if a["cls"].lower() == "actorrotator"]
    if not rotators:
        return 0
    by_label = {}
    for placed in eas.get_all_level_actors():
        by_label.setdefault(placed.get_actor_label(), placed)
    placed_n = 0
    for a in rotators:
        p = a.get("props", {})
        event = p.get("Event", "")
        if not event:
            continue
        own_tag = p.get("Tag", "")
        linked = []
        for x in actors:
            xp = x.get("props", {})
            if xp.get("Tag", "") != event or x["name"] == a["name"]:
                continue
            if own_tag and xp.get("Tag", "") == own_tag:
                continue          # retail skips tags matching the rotator's own
            hit = by_label.get(x["name"])
            if hit is not None and hit not in linked:
                linked.append((hit, x["location"]))
        if not linked:
            unreal.log(f"ROTATOR-SKIP {a['name']}: no placed actor tagged {event}")
            continue

        def fset(act, prop, key, default=0.0):
            act.set_editor_property(prop, float(p.get(key, default)))

        long_distance = p.get("bLongDistance") == "True"
        groups = [linked] if long_distance else [[pair] for pair in linked]
        for gi, group in enumerate(groups):
            rot = eas.spawn_actor_from_class(
                unreal.WoTActorRotator, unreal.Vector(*a["location"]))
            rot.set_actor_label(f"{a['name']}_{gi}" if len(groups) > 1 else a["name"])
            rot.set_actor_rotation(ue1_rot(a.get("rotation", [0, 0, 0])), False)
            for prop, key in (("min_radius", "MinRadius"),
                              ("max_radius", "MaxRadius"),
                              ("radius_shift_rate", "RadiusShiftRate"),
                              ("min_rotation_rate", "MinRotationRate"),
                              ("max_rotation_rate", "MaxRotationRate"),
                              ("rotation_shift_rate", "RotationShiftRate"),
                              ("min_height", "MinHeight"),
                              ("max_height", "MaxHeight"),
                              ("height_shift_rate", "HeightShiftRate")):
                fset(rot, prop, key)
            for prop, key in (("bounce_radius", "bBounceRadius"),
                              ("bounce_rotation_rate", "bBounceRotationRate"),
                              ("bounce_height", "bBounceHeight"),
                              ("face_in", "bFaceIn"),
                              ("face_out", "bFaceOut")):
                rot.set_editor_property(prop, p.get(key) == "True")
            rot.set_editor_property(
                "update_actor_rotation",
                p.get("bUpdateActorRotation", "True") == "True")
            m = _ROTOFF_RE.search(p.get("ActorRotationOffset", ""))
            if m:
                rot.set_editor_property("actor_rotation_offset", unreal.Rotator(
                    roll=float(m.group(3)), pitch=float(m.group(1)),
                    yaw=float(m.group(2))))
            rot.set_editor_property("driven_actors", [g[0] for g in group])
            if long_distance:
                rot.set_editor_property(
                    "driven_origins", [unreal.Vector(*g[1]) for g in group])
            for driven, _loc in group:
                root = driven.get_editor_property("root_component")
                if root and root.get_editor_property("mobility") \
                        != unreal.ComponentMobility.MOVABLE:
                    root.set_editor_property(
                        "mobility", unreal.ComponentMobility.MOVABLE)
            placed_n += 1
    return placed_n


def _zone_export_sections():
    """Return gameplay, ambient, and BSP sections from old or new zone JSON."""
    data = _zone_export_data()
    return (data.get("zones", []), data.get("ambient_zones", []),
            data.get("zone_bsp", []))


def make_light_table():
    """Emit the per-map runtime light table (UWoTMapLightTable DataAsset at
    /Game/WoT/Maps/LightTables/LT_<MAP>) consumed by the runtime mesh-
    light bake: runtime-spawned actors (pawns/hand/projectiles/gibs) descend
    to a compiled leaf and select its shipped permeating run with the same
    rules _ml_bake uses at
    import. Rows are _mesh_light_state() verbatim — FloatColor is display-
    space ready (FGetHSV x brightness/255 x waveform mean x
    Level.Brightness), radius is already the resolved WorldLightRadius
    (including SunLight's 250x override), and raw brightness remains separate
    for retail priority."""
    if not hasattr(unreal, "WoTMapLightTable"):
        return 0                     # C++ side not built yet: skip quietly
    lights, _, level_brightness = _mesh_light_state()
    _wave_selectors, wave_means, wave_amplitudes = _mesh_light_wave_groups()
    path = "/Game/WoT/Maps/LightTables"
    name = f"LT_{MAP}"
    full = f"{path}/{name}"
    if EAL.does_asset_exist(full):
        asset = unreal.load_asset(full)
    else:
        asset = ATT.create_asset(name, path, unreal.WoTMapLightTable, None)
    asset.set_editor_property("level_brightness", float(level_brightness))
    rows = []
    name_to_row = {}
    for light in lights:
        pos, r, col = light["location"], light["radius"], light["color"]
        rec = unreal.WoTLightRec()
        rec.set_editor_property("pos", unreal.Vector(*pos))
        rec.set_editor_property("inv_radius", 1.0 / max(r, 1.0))
        rec.set_editor_property("radius", r)
        rec.set_editor_property("color",
                                unreal.LinearColor(col[0], col[1], col[2], 1.0))
        rec.set_editor_property("wave_group", int(light["wave_group"]))
        rec.set_editor_property("sine", light["sine"])
        rec.set_editor_property("dir", unreal.Vector(*light["direction"]))
        rec.set_editor_property("brightness", int(light["brightness"]))
        rec.set_editor_property("special_lit", bool(light["special_lit"]))
        rec.set_editor_property("emits", bool(light["emits"]))
        name_to_row[light["name"]] = len(rows)
        rows.append(rec)
    asset.set_editor_property("lights", rows)
    asset.set_editor_property("wave_means", unreal.Vector4(*wave_means))
    asset.set_editor_property(
        "wave_amplitudes", unreal.Vector4(*wave_amplitudes))
    # Ambient rows are independent of gameplay volumes: zones.py emits every
    # BSP ZoneActor's raw HSV bytes plus the LevelInfo zone-zero fallback.
    # Conversion stays on the shared _fget_hsv path used by _mesh_light_state.
    ambs = []
    export_data = _zone_export_data()
    ambient_rows = export_data.get("ambient_zones", [])
    zone_bsp = export_data.get("zone_bsp", [])
    for zr in ambient_rows:
        za = _fget_hsv(int(zr.get("ambient_hue", 0)),
                       int(zr.get("ambient_saturation", 255)),
                       int(zr.get("ambient_brightness", 0)))
        # Authored-black zones stay explicit so they beat a lit default.
        rec = unreal.WoTZoneAmbient()
        rec.set_editor_property("zone", int(zr["zone"]))
        rec.set_editor_property("color",
                                unreal.LinearColor(za[0], za[1], za[2], 1.0))
        rec.set_editor_property("default", int(zr["zone"]) == 0)
        ambs.append(rec)
    asset.set_editor_property("zone_ambients", ambs)
    asset.set_editor_property(
        "zone_bsp_planes", [unreal.Vector4(*node["plane"]) for node in zone_bsp])
    asset.set_editor_property(
        "zone_bsp_front", [int(node["front"]) for node in zone_bsp])
    asset.set_editor_property(
        "zone_bsp_back", [int(node["back"]) for node in zone_bsp])

    max_zone = max((int(row.get("zone", 0)) for row in ambient_rows), default=0)
    zone_min_counts = [6] * (max_zone + 1)
    zone_max_counts = [6] * (max_zone + 1)
    for row in ambient_rows:
        zone = int(row.get("zone", 0))
        zone_min_counts[zone] = int(row.get("min_light_count", 6))
        zone_max_counts[zone] = int(row.get("max_light_count", 6))
    asset.set_editor_property("zone_min_light_counts", zone_min_counts)
    asset.set_editor_property("zone_max_light_counts", zone_max_counts)

    leaf_bsp = export_data.get("leaf_bsp", {})
    leaf_nodes = leaf_bsp.get("nodes", [])
    asset.set_editor_property(
        "leaf_bsp_leaves",
        [int(value) for node in leaf_nodes for value in node[:2]])
    asset.set_editor_property(
        "leaf_bsp_csg", [int(node[2]) for node in leaf_nodes])
    asset.set_editor_property(
        "bsp_root_outside", bool(leaf_bsp.get("root_outside", False)))
    asset.set_editor_property(
        "check_walk_surfaces", bool(export_data.get("check_walk_surfaces", True)))
    asset.set_editor_property(
        "ideal_player_count", int(export_data.get("ideal_player_count", 0)))

    friction_nodes = export_data.get("friction_nodes", [])
    asset.set_editor_property(
        "friction_node_indices", [int(row[0]) for row in friction_nodes])
    asset.set_editor_property(
        "friction_node_values", [float(row[1]) for row in friction_nodes])

    actor_names = export_data.get("light_actors", [])
    leaf_runs = export_data.get("leaf_lights", {})
    leaf_offsets = [0]
    leaf_indices = []
    for leaf_index in range(int(leaf_bsp.get("num_leaves", 0))):
        for actor_index in leaf_runs.get(str(leaf_index), []):
            actor_index = int(actor_index)
            if not 0 <= actor_index < len(actor_names):
                continue
            light_row = name_to_row.get(actor_names[actor_index])
            if light_row is not None:
                leaf_indices.append(light_row)
        leaf_offsets.append(len(leaf_indices))
    asset.set_editor_property("leaf_light_offsets", leaf_offsets)
    asset.set_editor_property("leaf_light_indices", leaf_indices)
    EAL.save_asset(full)
    unreal.log(f"LIGHTTABLE: {MAP} {len(rows)} light(s), "
               f"{len(ambs)} ambient zone(s), {len(zone_bsp)} BSP node(s), "
               f"{len(friction_nodes)} friction node(s), "
               f"{len(leaf_offsets) - 1} leaves/{len(leaf_indices)} light refs")
    return len(rows)


# Surface-lightmap lighting for MOVERS (the surface path, not the mesh
# lighting path). Movers are BSP brushes lit per-texel via radial +
# spotlight falloff, NOT mesh-Gouraud. Per light: perp = height of the light
# above the pixel's surface plane (dot(L,N)); the UE1 texel value
# Diffuse*LightSqrt reduces, for a flat face, to (perp/R)*(1-S)^2(2S+1)/S =
# cos(incidence)*(1-S)^2(2S+1) <= 1 (the 1/S source-spike cancels perp's S).
# Hemisphere-gated per pixel; spotlights (Sine>=0) gate to the cone. No
# ScaleGlow*1.4 (that is the mesh path's Diffuse, not the surface path's).
# _ML_SURF_SCALE is the one brightness knob, wired as the MLSurfScale material
# parameter (set on the MICs, NOT baked into the HLSL) so it tunes without a
# master rebuild — a graph-constant change doesn't reliably invalidate the
# replay's shader cache, but a per-instance scalar param does. The on-spotlight-
# axis wedges match golden at 2.0.
_ML_SURF_SCALE = 2.0
# Mover surface AMBIENT: UE1 seeds the static lightmap with the mover's
# runtime-zone ambient hsv*0.25 (packed and scaled *0.25); the opaque
# lightmap pass then modulate-2x's it (folded into our atlas), so the net
# display multiplier a world surface gets is hsv*0.5.
# Movers carry no BAKED lightmap (runtime-computed) so we add that same term
# analytically as MLAmbient. Zone = point-in-BSP at the mover Location
# (movers2glb, matching UE1's runtime zone assignment) — NOT the stale T3D
# Region (LevelInfo0, ambient 0, which left off-cone wedges black). The mesh
# path (decorations) keeps full hsv (no *0.25).
_ML_SURF_AMB_SCALE = 0.5
_ML_MOVER_LIGHTS = 20
_ML_MOVER_BATCH = 10
_ML_MOVER_GRAPH_TAG = "WoTMoverSurfaceGraph"
_ML_MOVER_GRAPH_VERSION = "plane-sections-20-wave-groups-dynamic-g45-v4-shell"
_ML_SURFACE_HLSL = """
float3 N = normalize(Nrm);
float4 pr[10] = { float4(P0, R0), float4(P1, R1), float4(P2, R2),
                  float4(P3, R3), float4(P4, R4), float4(P5, R5),
                  float4(P6, R6), float4(P7, R7), float4(P8, R8),
                  float4(P9, R9) };
float3 cl[10] = { C0, C1, C2, C3, C4, C5, C6, C7, C8, C9 };
float3 dr[10] = { D0, D1, D2, D3, D4, D5, D6, D7, D8, D9 };
float sn[10] = { S0, S1, S2, S3, S4, S5, S6, S7, S8, S9 };
float wg[10] = { G0, G1, G2, G3, G4, G5, G6, G7, G8, G9 };
float4 waves = float4(Wave0, Wave1, Wave2, Wave3);
float3 sum = 0;
[unroll] for (int i = 0; i < 10; i++)
{
    if (pr[i].w <= 0) continue;
    float3 Lw = pr[i].xyz - WP;
    float d = length(Lw);
    float S = d * pr[i].w;                 // d / R
    if (S >= 1.0) continue;
    float perp = dot(Lw, N);               // light height above this pixel's plane
    if (perp <= 0) continue;               // hemisphere gate
    float atten = (1.0 - S) * (1.0 - S) * (2.0 * S + 1.0) / max(S, 1e-4);
    float g = (perp * pr[i].w) * atten;    // (perp/R)*LightSqrt = cos*(1-S)^2(2S+1)
    if (sn[i] >= 0.0)                       // spotlight cone
    {
        float dot_ = dot(-Lw / max(d, 1e-4), dr[i]);
        float c = saturate((dot_ - sn[i]) / max(1.0 - sn[i], 1e-4));
        g *= c * c;
    }
    float ratio = 1.0;
    int group = (int)floor(wg[i] + 0.5);
    if (group >= 0 && group < 4 && MLWaveMeans[group] > 0.0)
    {
        ratio += MLWaveAmplitudes[group]
               * (2.0 * waves[group] - 1.0) / MLWaveMeans[group];
    }
    if (g > 0) sum += cl[i] * (g * ratio);
}
return sum;
"""


def make_meshlit_masters():
    """Build/load the opaque, two-sided, and masked MeshLit masters."""
    return mesh_master_bootstrap.make_meshlit_masters(unreal, GAMMA_SPACE)


def _ml_wire_moverlight(m, expr):
    """Wire two ten-light surface batches and the shared ambient/scale tail."""
    MEL = unreal.MaterialEditingLibrary
    wp = expr(unreal.MaterialExpressionWorldPosition, -700, 0)
    nrm = expr(unreal.MaterialExpressionVertexNormalWS, -700, 80)
    wave_sources = mesh_master_bootstrap.light_wave_parameter_nodes(
        unreal, m, expr, -950, 160)
    batches = []
    for batch_start in range(0, _ML_MOVER_LIGHTS, _ML_MOVER_BATCH):
        cu = expr(unreal.MaterialExpressionCustom, -350,
                  100 + batch_start * 80)
        cu.set_editor_property("code", _ML_SURFACE_HLSL)
        cu.set_editor_property("output_type",
                               unreal.CustomMaterialOutputType.CMOT_FLOAT3)
        cu.set_editor_property("description",
                               f"UE1SurfaceLight{batch_start // _ML_MOVER_BATCH}")
        ins, wires = [], []
        for nm, node in (("WP", wp), ("Nrm", nrm)):
            ci = unreal.CustomInput()
            ci.set_editor_property("input_name", nm)
            ins.append(ci)
            wires.append((node, nm))
        for nm, node in wave_sources:
            ci = unreal.CustomInput()
            ci.set_editor_property("input_name", nm)
            ins.append(ci)
            wires.append((node, nm))
        y = 160 + batch_start * 210
        for local_index in range(_ML_MOVER_BATCH):
            light_index = batch_start + local_index
            pos = expr(unreal.MaterialExpressionVectorParameter, -700, y)
            pos.set_editor_property("parameter_name", f"MLPos{light_index}")
            rad = expr(unreal.MaterialExpressionScalarParameter, -700, y + 40)
            rad.set_editor_property("parameter_name", f"MLRad{light_index}")
            rad.set_editor_property("default_value", 0.0)
            col = expr(unreal.MaterialExpressionVectorParameter, -700, y + 80)
            col.set_editor_property("parameter_name", f"MLCol{light_index}")
            dr = expr(unreal.MaterialExpressionVectorParameter, -700, y + 120)
            dr.set_editor_property("parameter_name", f"MLDir{light_index}")
            sn = expr(unreal.MaterialExpressionScalarParameter, -700, y + 160)
            sn.set_editor_property("parameter_name", f"MLSine{light_index}")
            sn.set_editor_property("default_value", -1.0)
            group = expr(unreal.MaterialExpressionScalarParameter, -700, y + 200)
            group.set_editor_property(
                "parameter_name", f"MLWaveGroup{light_index}")
            group.set_editor_property("default_value", -1.0)
            for nm, node in (
                    (f"P{local_index}", pos), (f"R{local_index}", rad),
                    (f"C{local_index}", col), (f"D{local_index}", dr),
                    (f"S{local_index}", sn), (f"G{local_index}", group)):
                ci = unreal.CustomInput()
                ci.set_editor_property("input_name", nm)
                ins.append(ci)
                wires.append((node, nm))
            y += 250
        cu.set_editor_property("inputs", ins)
        for node, nm in wires:
            MEL.connect_material_expressions(node, "", cu, nm)
        batches.append(cu)

    summed = expr(unreal.MaterialExpressionAdd, -150, 180)
    MEL.connect_material_expressions(batches[0], "", summed, "A")
    MEL.connect_material_expressions(batches[1], "", summed, "B")
    dynamic = build_dynamic_surface_sum(
        unreal, m, expr, -100, 520,
        enable_parameter="MLDynEnable",
        description="UE1DynamicMoverSurfaceLights")
    with_dynamic = expr(unreal.MaterialExpressionAdd, 0, 180)
    MEL.connect_material_expressions(summed, "", with_dynamic, "A")
    MEL.connect_material_expressions(dynamic, "", with_dynamic, "B")
    amb = expr(unreal.MaterialExpressionVectorParameter, -150, 300)
    amb.set_editor_property("parameter_name", "MLAmbient")
    sc = expr(unreal.MaterialExpressionScalarParameter, -150, 380)
    sc.set_editor_property("parameter_name", "MLSurfScale")
    sc.set_editor_property("default_value", _ML_SURF_SCALE)
    scaled = expr(unreal.MaterialExpressionMultiply, 40, 180)
    # Static table colors and dynamic MPC colors are both UE1 FloatColor.
    # Applying MLSurfScale after their sum keeps equal color/radius lights at
    # equal strength under the mover master's calibrated 2.0 convention.
    MEL.connect_material_expressions(with_dynamic, "", scaled, "A")
    MEL.connect_material_expressions(sc, "", scaled, "B")
    lit = expr(unreal.MaterialExpressionAdd, 180, 180)
    MEL.connect_material_expressions(scaled, "", lit, "A")
    MEL.connect_material_expressions(amb, "", lit, "B")
    ceiling = expr(unreal.MaterialExpressionConstant, 180, 320)
    ceiling.set_editor_property("r", 2.0)
    clamped = expr(unreal.MaterialExpressionMin, 320, 180)
    MEL.connect_material_expressions(lit, "", clamped, "A")
    MEL.connect_material_expressions(ceiling, "", clamped, "B")
    return clamped


def make_moverlit_masters():
    """M_WoT_MoverLit(+Masked): UE1 SURFACE-lightmap lighting for movers (BSP
    brushes lit via the surface path, NOT the mesh lighting path). Same asset shape
    as make_meshlit_masters but with the surface Custom node."""
    global _ml_mover_master_cache
    if _ml_mover_master_cache is not None:
        return _ml_mover_master_cache
    MEL = unreal.MaterialEditingLibrary
    out = {}
    for name, masked in (("M_WoT_MoverLit", False), ("M_WoT_MoverLitMasked", True)):
        full = f"/Game/WoT/Materials/{name}"
        exists = EAL.does_asset_exist(full)
        m = unreal.load_asset(full) if exists else None
        tagged = (exists and EAL.get_metadata_tag(m, _ML_MOVER_GRAPH_TAG)
                  == _ML_MOVER_GRAPH_VERSION)
        if tagged:
            out[masked] = m
            continue
        if exists:
            clear_material_graph(unreal, m)
        else:
            m = ATT.create_asset(name, "/Game/WoT/Materials", unreal.Material,
                                 unreal.MaterialFactoryNew())

        def expr(cls, x, y):
            return MEL.create_material_expression(m, cls, x, y)

        m.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
        m.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED
                              if masked else unreal.BlendMode.BLEND_OPAQUE)
        m.set_editor_property("used_with_skeletal_mesh", True)
        m.set_editor_property("used_with_static_lighting", True)
        tex = _lin_sampler(expr(unreal.MaterialExpressionTextureSampleParameter2D,
                                -700, -200))
        tex.set_editor_property("parameter_name", "Tex")
        cu = _ml_wire_moverlight(m, expr)
        e = expr(unreal.MaterialExpressionMultiply, -100, -50)
        MEL.connect_material_expressions(tex, "RGB", e, "A")
        MEL.connect_material_expressions(cu, "", e, "B")
        MEL.connect_material_property(e, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
        if masked:
            m.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED)
            m.set_editor_property("opacity_mask_clip_value", 0.5)
            MEL.connect_material_property(
                tex, "A", unreal.MaterialProperty.MP_OPACITY_MASK)
        MEL.recompile_material(m)
        EAL.set_metadata_tag(m, _ML_MOVER_GRAPH_TAG, _ML_MOVER_GRAPH_VERSION)
        EAL.save_asset(full)
        out[masked] = m
    _ml_mover_master_cache = out
    return _ml_mover_master_cache


def make_meshlit_translucent_master():
    """Build/load the lit translucent MeshLit master."""
    return mesh_master_bootstrap.make_meshlit_translucent_master(
        unreal, GAMMA_SPACE)


def _ml_texture_info(package, name):
    key = _tkey(package, name)
    info = TEXINFO.get(key)
    if info is None:
        match = next((item for item in TEXINFO
                      if item[0].casefold() == package.casefold()
                      and item[1].casefold() == name.casefold()), None)
        if match is not None:
            key, info = match, TEXINFO[match]
    return key, info


def _ml_import_texture_variant(package, name, *, key0=False):
    """Import one mesh texture variant and return its asset path/info."""
    key, info = _ml_texture_info(package, name)
    if info is None:
        return None, None
    package, source_name = key
    filename = info.get("key0") if key0 else f"{source_name}.png"
    if not filename:
        return None, info
    png = CONV / "tex" / package / filename
    if not png.is_file():
        raise RuntimeError(f"manifest texture PNG is missing: {png}")
    asset_name = png.stem
    path = f"/Game/WoT/Textures/{package}/{asset_name}"
    if not EAL.does_asset_exist(path):
        task = unreal.AssetImportTask()
        task.filename = str(png)
        task.destination_path = f"/Game/WoT/Textures/{package}"
        task.automated = True
        task.save = True
        ATT.import_asset_tasks([task])
    if not key0:
        ensure_texture_payload_binding(unreal, path, package, source_name)
    if GAMMA_SPACE and EAL.does_asset_exist(path):
        texture = unreal.load_asset(path)
        if texture.get_editor_property("srgb"):
            texture.set_editor_property("srgb", False)
            EAL.save_asset(path)
    return path, info


def _ml_stamp_key_color(mic, info):
    red, green, blue = palette0_rgb(info)
    unreal.MaterialEditingLibrary.set_material_instance_vector_parameter_value(
        mic, "KeyColor",
        unreal.LinearColor(red / 255.0, green / 255.0, blue / 255.0, 1.0))


def _ml_fix_mesh_skins():
    """(a) Import any missing mesh-skin textures (the J* skin PNGs live in
    the conv tree under WoTDecorations/WoTPawns/WoTTraps AND the code
    packages Angreal/Legend/MachinShin/WOT, but were never committed —
    MICs reference them by path, so importing restores the refs).
    (b) In gamma mode, reparent mesh-skin MICs to the James slot-type master.
    (c) MICs created while their texture was ABSENT have NO Tex override
    at all (MI_JAndilayRoot rendered the neutral default — the dark
    Arena_06 glow-roots); rebind Tex from the matching texture by name."""
    n_tex = 0
    for pkg in ("WoTDecorations", "WoTPawns", "WoTTraps",
                "Angreal", "Legend", "MachinShin", "WOT"):
        d = CONV / "tex" / pkg
        if not d.exists():
            continue
        for png in d.glob("*.png"):
            if is_keyed_texture_name(png.stem):
                continue  # imported only for a proven _M mesh slot below
            dest = f"/Game/WoT/Textures/{pkg}/{png.stem}"
            if EAL.does_asset_exist(dest):
                ensure_texture_payload_binding(
                    unreal, dest, pkg, png.stem)
                continue
            task = unreal.AssetImportTask()
            task.filename = str(png)
            task.destination_path = f"/Game/WoT/Textures/{pkg}"
            task.automated = True
            task.save = True
            ATT.import_asset_tasks([task])
            ensure_texture_payload_binding(unreal, dest, pkg, png.stem)
            if GAMMA_SPACE and EAL.does_asset_exist(dest):
                t = unreal.load_asset(dest)
                t.set_editor_property("srgb", False)
                EAL.save_asset(dest)
            n_tex += 1
    MEL = unreal.MaterialEditingLibrary
    n_re = n_fix = n_key0 = 0
    ml = make_meshlit_masters() if GAMMA_SPACE else None
    mesh_slots = set()
    for meta_file in (CONV / "mesh").glob("*.meta.json"):
        mesh_slots.update(json.loads(meta_file.read_text()).get("slot_types", {}))
    if GAMMA_SPACE:
        by_name = {
            "M_WoT_MeshLit": ml[False],
            "M_WoT_MeshLitMasked": ml[True],
            "M_WoT_MeshLit2S": ml["2s"],
            "M_WoT_MeshLitT": make_meshlit_translucent_master(),
            "M_WoT_MeshModulate": unreal.load_asset(
                "/Game/WoT/Materials/M_WoT_MeshModulate"),
        }
        typed_targets = {
            suffix: by_name[name] for suffix, name in GAMMA_SLOT_MASTERS.items()
        }
        swap = {"M_WoT_Master": typed_targets[""],
                "M_WoT_Masked": typed_targets["_M"]}
    else:
        typed_targets = {}
        swap = {}
    for asset in EAL.list_assets("/Game/WoT/Materials/Instances",
                                 recursive=False):
        path = asset.split(".")[0]
        slot = path.rsplit("/MI_", 1)[-1]
        if slot not in mesh_slots and "/MI_J" not in path:
            continue
        base, suffix = split_slot_name(slot)
        mic = unreal.load_asset(path)
        dirty = False
        if GAMMA_SPACE:
            parent = mic.get_editor_property("parent")
            # Typed slots always converge to their real base-property master;
            # ordinary slots retain the established legacy-root swap only, so
            # fractal/particle fallback MICs keep their material logic.
            tgt = typed_targets.get(suffix) if suffix and slot in mesh_slots \
                else swap.get(parent.get_name() if parent else "")
            if tgt is not None and parent != tgt:
                mic.set_editor_property("parent", tgt)
                dirty = True
                n_re += 1
        tex = MEL.get_material_instance_texture_parameter_value(mic, "Tex")
        want_key0 = suffix == "_M" and slot in mesh_slots
        needs_texture = (want_key0 or tex is None
                         or tex.get_name() == "T_WoT_Neutral")
        for pkg in ("WoTDecorations", "WoTPawns", "WoTTraps",
                    "Angreal", "Legend", "MachinShin", "WOT"):
            _key, info = _ml_texture_info(pkg, base)
            if info is None:
                continue
            if needs_texture:
                cand, info = _ml_import_texture_variant(
                    pkg, base, key0=want_key0)
                if cand is None:
                    if want_key0:
                        raise RuntimeError(
                            f"masked mesh slot {slot} has no key0 variant")
                elif tex is None or tex.get_path_name().split(".")[0] != cand:
                    MEL.set_material_instance_texture_parameter_value(
                        mic, "Tex", unreal.load_asset(cand))
                    dirty = True
                    n_fix += 1
                    n_key0 += int(want_key0)
            _ml_stamp_key_color(mic, info)
            dirty = True
            break
        if dirty:
            EAL.save_asset(path)
    unreal.log(f"ML-SKINS textures={n_tex} reparented={n_re} "
               f"rebound={n_fix} key0={n_key0}")


def place_decorations(eas):
    f = CONV / f"{MAP}.t3d.json"
    if not f.exists() or not CLASS2MESH:
        return 0
    _ml_fix_mesh_skins()
    n = 0
    for a in json.loads(f.read_text())["actors"]:
        if a["cls"].lower() == "raincyd":
            n += _place_raincyd(eas, a)   # falling rain stacks, own actor
            continue
        # instance Mesh= override beats the class default (the Battle3 lamp
        # globes are class Cylinder with Mesh=Mesh'Angreal.AMAPearl')
        mm = re.search(r"Mesh'[\w\-]+\.([\w\-]+)'", a["props"].get("Mesh", ""))
        meshname = mm.group(1) if mm else CLASS2MESH.get(a["cls"].lower())
        if not meshname:
            continue
        if _ml_bool(a["props"].get("bMeshCurvy", False)):
            # The shipped object is AMAPearl, while its canonical #exec mesh
            # name (and generated asset path) is spelled AMApearl.
            meshname = ("AMApearl" if meshname.casefold() == "amapearl"
                        else meshname) + "_Curvy"
        base = f"/Game/WoT/Meshes/{meshname}"
        mesh = None
        if EAL.does_directory_exist(base):
            for asset in EAL.list_assets(base, recursive=True):
                o = unreal.load_asset(asset.split(".")[0])
                if isinstance(o, unreal.SkeletalMesh):
                    mesh = o
                    break
        if not mesh:
            unreal.log(f"DECO-SKIP {a['name']}: mesh {meshname!r} unavailable")
            continue
        act = eas.spawn_actor_from_class(unreal.WoTDecoration,
                                         unreal.Vector(*a["location"]))
        act.set_actor_rotation(ue1_rot(a["rotation"]), False)
        act.set_actor_label(a["name"])
        act.get_editor_property("mesh").set_editor_property(
            "lighting_channels", unreal.LightingChannels(channel0=True, channel1=True))
        scale = float(a["props"].get("DrawScale", 1.0))
        if scale != 1.0:
            act.set_actor_scale3d(unreal.Vector(scale, scale, scale))
        act.get_editor_property("mesh").set_skeletal_mesh_asset(mesh)
        _ml_skin_override(act, a)
        if GAMMA_SPACE:
            _ml_bake(act, a)
        n += 1
    return n


def _ml_material_root_name(material):
    """Return the root material name for a MIC/MID parent chain."""
    current = material
    while current is not None:
        if not isinstance(current, unreal.MaterialInstance):
            return current.get_name()
        parent = current.get_editor_property("parent")
        if parent is None:
            return current.get_name()
        current = parent
    return ""


def _ml_skin_override(act, a):
    """Mirror ``Tri.PolyFlags | Actor ExtraFlags`` for editor mesh slot zero.

    Skin wins over MultiSkins(0). With no authored skin override, Actor.Style
    still re-types the current material while retaining its bound texture.
    """
    p = a.get("props", {})
    component = act.get_editor_property("mesh")
    current_material = component.get_material(0)
    root = _ml_material_root_name(current_material)
    slot_suffix = material_type_suffix(root)
    style = p.get("Style", _ml_class_prop(a["cls"], "Style", ""))
    style_suffix = actor_style_suffix(style)
    type_suffix = effective_material_type_suffix(slot_suffix, style)
    skin_reference = slot_zero_skin_reference(p)
    if skin_reference is None and type_suffix == slot_suffix:
        return

    MEL = unreal.MaterialEditingLibrary
    texture = None
    texture_info = None
    pkg = name = None
    if skin_reference is not None:
        sm = _ML_SKIN_RE.match(skin_reference)
        if not sm:
            unreal.log(f"SKIN-SKIP {a['name']}: malformed {skin_reference}")
            return
        pkg, name = sm.group(1), sm.group(2)
        if skin_reference.startswith("WaveTexture'"):
            tex_path = f"/Game/WoT/Textures/{pkg}/{name}"
            texture = unreal.load_asset(tex_path)
        else:
            tex_path, texture_info = _ml_import_texture_variant(
                pkg, name, key0=type_suffix == "_M")
            texture = unreal.load_asset(tex_path) if tex_path else None
    else:
        texture = MEL.get_material_instance_texture_parameter_value(
            current_material, "Tex")
        if texture is not None:
            name = texture.get_name()
            asset_path = texture.get_path_name().split(".", 1)[0]
            marker = "/Game/WoT/Textures/"
            tail = asset_path.split(marker, 1)[1] if marker in asset_path else ""
            pkg = tail.split("/", 1)[0] if "/" in tail else None
        if type_suffix == "_M" and slot_suffix != "_M" and pkg and name:
            tex_path, texture_info = _ml_import_texture_variant(
                pkg, name, key0=True)
            texture = unreal.load_asset(tex_path) if tex_path else None
    if texture is None:
        unreal.log(f"SKIN-SKIP {a['name']}: slot-0 texture unavailable")
        return

    if style_suffix == "_T":
        master = make_meshlit_translucent_master() if GAMMA_SPACE \
            else make_mesh_translucent_master()
        mic_path = f"/Game/WoT/Materials/Instances/MI_MeshT_{name}"
    else:
        if GAMMA_SPACE:
            ml = make_meshlit_masters()
            typed = {
                "": ml[False],
                "_2S": ml["2s"],
                "_M": ml[True],
                "_T": make_meshlit_translucent_master(),
                "_Mod": unreal.load_asset(
                    "/Game/WoT/Materials/M_WoT_MeshModulate"),
            }
            master = typed[type_suffix]
        else:
            master = unreal.load_asset(
                f"/Game/WoT/Materials/{IMPORT_SLOT_MASTERS[type_suffix]}")
        # Typed overrides need distinct MIC assets so a shared Skin= texture
        # cannot be reparented back and forth by actors with different slot-0
        # types. The ordinary opaque path keeps its historic asset name.
        mic_path = f"/Game/WoT/Materials/Instances/MI_Skin_{name}{type_suffix}"
    if not EAL.does_asset_exist(mic_path):
        mic = ATT.create_asset(mic_path.rsplit("/", 1)[1],
                               "/Game/WoT/Materials/Instances",
                               unreal.MaterialInstanceConstant,
                               unreal.MaterialInstanceConstantFactoryNew())
    else:
        mic = unreal.load_asset(mic_path)
    if mic.get_editor_property("parent") != master:
        mic.set_editor_property("parent", master)
    MEL.set_material_instance_texture_parameter_value(
        mic, "Tex", texture)
    if texture_info is not None:
        _ml_stamp_key_color(mic, texture_info)
    # UE1 env-maps mesh tris whose JAMES per-poly flags carry PF_Environment,
    # sampling the owner's Texture as the map. slot_types now preserves that
    # flag for a future env-map master; the current retail content has no env-bit
    # mesh slots, so the existing explicit Texture= / bEnvironment signal
    # remains the active glass-globe path.
    if TEXINFO.get(_tkey(pkg, name), {}).get("env") \
            or _SKIN_RE.match(p.get("Texture", "")):
        MEL.set_material_instance_scalar_parameter_value(mic, "EnvMap", 1.0)
    EAL.save_asset(mic_path)
    act.get_editor_property("mesh").set_material(0, mic)


_MESH_TRANSLUCENT_GRAPH_TAG = "WoTMeshTranslucentGraph"
_MESH_TRANSLUCENT_GRAPH_VERSION = "retail-per-channel-thin-v2"


def make_mesh_translucent_master():
    """Unlit translucent mesh skins (RainCyd rain streaks etc.):
    STY_Translucent without a lightmap — FB = S + Dst*(1-S), evaluated
    independently for RGB as the UE1 framebuffer combiner did.

    UE's blend modes carry one scalar destination factor. The per-channel
    factor goes through the thin-translucent model's dual-source blend,
    FB = Add + Dst*Mul against the live framebuffer (BasePassPixelShader.usf,
    ThinTranslucentCommon.ush AccumulateThinTranslucentModel). The previous
    graph composed Dst from a SceneColor read, which holds only the opaque
    scene: every translucent layer behind the mesh was erased wherever the
    mesh covered the screen, black texels included (Taint's comets cut
    hard-edged wedges out of Arena_01's translucent ground glow).

    Specular 0 removes the Fresnel term (F_Schlick scales it by
    saturate(50*F0)) and BaseColor 0 removes all lighting, so the model is
        Add = Emissive * Opacity
        Mul = TransmittanceColor^(1/NoV) * (1 - Opacity).
    Opacity 0.5, Emissive 2S and TransmittanceColor (2(1-S))^NoV, with NoV
    computed as the engine does, give Add = S and Mul = 1-S at every view
    angle."""
    full = "/Game/WoT/Materials/M_WoT_MeshTranslucent"
    MEL = unreal.MaterialEditingLibrary
    if EAL.does_asset_exist(full):
        m = unreal.load_asset(full)
        if EAL.get_metadata_tag(
                m, _MESH_TRANSLUCENT_GRAPH_TAG) == \
                _MESH_TRANSLUCENT_GRAPH_VERSION:
            return m
        clear_material_graph(unreal, m)
    else:
        m = ATT.create_asset("M_WoT_MeshTranslucent", "/Game/WoT/Materials",
                             unreal.Material, unreal.MaterialFactoryNew())

    def expr(cls, x, y):
        return MEL.create_material_expression(m, cls, x, y)

    def constant(value, x, y):
        c = expr(unreal.MaterialExpressionConstant, x, y)
        c.set_editor_property("r", value)
        return c

    def connect(source, target, pin):
        if not MEL.connect_material_expressions(source, "", target, pin):
            raise RuntimeError(f"M_WoT_MeshTranslucent: cannot connect {pin}")

    m.set_editor_property(
        "shading_model", unreal.MaterialShadingModel.MSM_THIN_TRANSLUCENT)
    m.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    m.set_editor_property(
        "translucency_lighting_mode",
        unreal.TranslucencyLightingMode.TLM_SURFACE_PER_PIXEL_LIGHTING)
    m.set_editor_property("two_sided", True)
    m.set_editor_property("used_with_skeletal_mesh", True)
    tex = _lin_sampler(expr(unreal.MaterialExpressionTextureSampleParameter2D, -600, 0))
    tex.set_editor_property("parameter_name", "Tex")
    # UV tiling dial: the James decoder appears to drop the UE1 MeshMap
    # scale (RainCyd streaks render ~4x too wide at 1:1) — empirical until
    # the decoder carries the real factor.
    tc = expr(unreal.MaterialExpressionTextureCoordinate, -880, -40)
    ut = expr(unreal.MaterialExpressionScalarParameter, -880, 60)
    ut.set_editor_property("parameter_name", "UTile")
    ut.set_editor_property("default_value", 1.0)
    vt = expr(unreal.MaterialExpressionScalarParameter, -880, 140)
    vt.set_editor_property("parameter_name", "VTile")
    vt.set_editor_property("default_value", 1.0)
    tile = expr(unreal.MaterialExpressionAppendVector, -760, 90)
    MEL.connect_material_expressions(ut, "", tile, "A")
    MEL.connect_material_expressions(vt, "", tile, "B")
    tuv = expr(unreal.MaterialExpressionMultiply, -680, 0)
    MEL.connect_material_expressions(tc, "", tuv, "A")
    MEL.connect_material_expressions(tile, "", tuv, "B")
    MEL.connect_material_expressions(tuv, "", tex, "UVs")
    # RGB x A: masked textures' key-color dilation (utx2png) filled the
    # transparent texels' RGB — multiply by alpha to restore
    # black-is-transparent for the translucent blend
    rgba0 = expr(unreal.MaterialExpressionMultiply, -560, 30)
    MEL.connect_material_expressions(tex, "RGB", rgba0, "A")
    MEL.connect_material_expressions(tex, "A", rgba0, "B")
    # UE1 unlit-translucent meshes scale the SOURCE by GUnlitColor
    # (clamp(ScaleGlow*0.5+AmbientGlow/256)) before the blend — driven per
    # actor via UWoTMeshLight's MLAmbient (default white = unchanged;
    # RainCyd has no MeshLight component and keeps the default).
    mla = expr(unreal.MaterialExpressionVectorParameter, -560, 160)
    mla.set_editor_property("parameter_name", "MLAmbient")
    mla.set_editor_property("default_value", unreal.LinearColor(1, 1, 1, 1))
    rgba = expr(unreal.MaterialExpressionMultiply, -480, 30)
    MEL.connect_material_expressions(rgba0, "", rgba, "A")
    MEL.connect_material_expressions(mla, "", rgba, "B")

    # Add = Emissive * Opacity = 2S * 0.5.
    emissive = expr(unreal.MaterialExpressionMultiply, -240, 0)
    emissive.set_editor_property("const_b", 2.0)
    MEL.connect_material_expressions(rgba, "", emissive, "A")
    MEL.connect_material_property(
        emissive, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    MEL.connect_material_property(
        constant(0.5, -240, 120), "", unreal.MaterialProperty.MP_OPACITY)
    for value, prop, y in ((0.0, unreal.MaterialProperty.MP_BASE_COLOR, 200),
                           (0.0, unreal.MaterialProperty.MP_METALLIC, 260),
                           (0.0, unreal.MaterialProperty.MP_SPECULAR, 320),
                           (1.0, unreal.MaterialProperty.MP_ROUGHNESS, 380)):
        MEL.connect_material_property(constant(value, -240, y), "", prop)

    # NoV exactly as AccumulateThinTranslucentModel: saturate(|N.V| + 1e-5).
    normal = expr(unreal.MaterialExpressionPixelNormalWS, -880, -400)
    view = expr(unreal.MaterialExpressionCameraVectorWS, -880, -320)
    n_dot_v = expr(unreal.MaterialExpressionDotProduct, -760, -360)
    connect(normal, n_dot_v, "A")
    connect(view, n_dot_v, "B")
    abs_n_dot_v = expr(unreal.MaterialExpressionAbs, -640, -360)
    connect(n_dot_v, abs_n_dot_v, "")
    biased = expr(unreal.MaterialExpressionAdd, -540, -360)
    biased.set_editor_property("const_b", 1e-5)
    connect(abs_n_dot_v, biased, "A")
    no_v = expr(unreal.MaterialExpressionSaturate, -440, -360)
    connect(biased, no_v, "")
    # Mul = TransmittanceColor^(1/NoV) * (1 - 0.5) = 1-S.
    one_minus_source = expr(unreal.MaterialExpressionOneMinus, -440, -200)
    connect(rgba, one_minus_source, "")
    doubled = expr(unreal.MaterialExpressionMultiply, -340, -200)
    doubled.set_editor_property("const_b", 2.0)
    connect(one_minus_source, doubled, "A")
    transmittance = expr(unreal.MaterialExpressionPower, -240, -260)
    connect(doubled, transmittance, "Base")
    connect(no_v, transmittance, "Exp")
    thin = expr(unreal.MaterialExpressionThinTranslucentMaterialOutput, -100, -260)
    connect(transmittance, thin, "TransmittanceColor")
    connect(constant(1.0, -240, -120), thin, "SurfaceCoverage")
    EAL.set_metadata_tag(
        m, _MESH_TRANSLUCENT_GRAPH_TAG, _MESH_TRANSLUCENT_GRAPH_VERSION)
    MEL.recompile_material(m)
    EAL.save_asset(full)
    return m


def _place_raincyd(eas, a):
    """RainCyd: falling rain-streak cylinder stacks (Environment/RainCyd.uc:
    STY_Translucent bUnlit Skin=RainDropsA; motion = the actor falling at
    500-700 uu/s — AWoTRainCyd wraps the offset per segment). Source term =
    UE1 GUnlitColor: unlit meshes draw at clamp(ScaleGlow*0.5 +
    AmbientGlow/256) — placed RainCyds author ScaleGlow 0.085-0.1, so the
    retail streaks run at ~4-5% of texture brightness (mesh renderer; measured
    golden peak excess ~6 luma). The MIC is keyed per g. RainDropsA gets
    NoMipmaps: the streak texels are 1-2px lines on black — mip-averaged
    sampling dilutes the source ~9x where retail's unmipped path keeps
    them at full luma (dilution and g together masqueraded as a width
    error; FWHM was measured identical at 3.0px)."""
    p = a.get("props", {})
    g = min(1.0, 0.5 * float(p.get("ScaleGlow", 1.0))
            + float(p.get("AmbientGlow", 0)) / 256.0)
    tex_path = "/Game/WoT/Textures/Environment/RainDropsA"
    if not EAL.does_asset_exist(tex_path):
        png = CONV / "tex/Environment/RainDropsA.png"
        if not png.exists():
            return False
        task = unreal.AssetImportTask()
        task.filename = str(png)
        task.destination_path = "/Game/WoT/Textures/Environment"
        task.automated = True
        task.save = True
        ATT.import_asset_tasks([task])
    ensure_texture_payload_binding(
        unreal, tex_path, "Environment", "RainDropsA", 1)
    tex = unreal.load_asset(tex_path)
    if tex.get_editor_property("mip_gen_settings") != \
            unreal.TextureMipGenSettings.TMGS_NO_MIPMAPS:
        tex.set_editor_property("mip_gen_settings",
                                unreal.TextureMipGenSettings.TMGS_NO_MIPMAPS)
        EAL.save_asset(tex_path)
    master = make_mesh_translucent_master()
    mic_name = f"MI_MeshT_RainDropsA_g{int(round(g * 10000)):04d}"
    mic_path = f"/Game/WoT/Materials/Instances/{mic_name}"
    if not EAL.does_asset_exist(mic_path):
        mic = ATT.create_asset(mic_name,
                               "/Game/WoT/Materials/Instances",
                               unreal.MaterialInstanceConstant,
                               unreal.MaterialInstanceConstantFactoryNew())
        unreal.MaterialEditingLibrary.set_material_instance_texture_parameter_value(
            mic, "Tex", unreal.load_asset(tex_path))
        EAL.save_asset(mic_path)
    else:
        mic = unreal.load_asset(mic_path)
    # always repair the parent: master regeneration leaves stale refs
    if mic.get_editor_property("parent") != master:
        mic.set_editor_property("parent", master)
    unreal.MaterialEditingLibrary.set_material_instance_vector_parameter_value(
        mic, "MLAmbient", unreal.LinearColor(g, g, g, 1.0))
    # streak width calibrated vs the golden dripshaft plate
    unreal.MaterialEditingLibrary.set_material_instance_scalar_parameter_value(
        mic, "UTile", 4.0)
    EAL.save_asset(mic_path)
    mesh = None
    base = "/Game/WoT/Meshes/RainCyd"
    if EAL.does_directory_exist(base):
        for asset in EAL.list_assets(base, recursive=True):
            o = unreal.load_asset(asset.split(".")[0])
            if isinstance(o, unreal.SkeletalMesh):
                mesh = o
                break
    if not mesh:
        return False
    act = eas.spawn_actor_from_class(unreal.WoTRainCyd,
                                     unreal.Vector(*a["location"]))
    act.set_actor_label(a["name"])
    scale = float(a["props"].get("DrawScale", 1.0))
    if scale != 1.0:
        act.set_actor_scale3d(unreal.Vector(scale, scale, scale))
    # Authored per-actor overrides (census 2026-07-24: StackNum 2-7, fall
    # rates 300-1200, TraceDepth 5000, BuryDistance 0/-64/-128 across maps) —
    # the stack/anchor math consumes these at BeginPlay.
    if "StackNum" in p:
        act.set_editor_property("stack_num", int(p["StackNum"]))
    for key, prop in (("MinFallRate", "min_fall_rate"),
                      ("MaxFallRate", "max_fall_rate"),
                      ("TraceDepth", "trace_depth"),
                      ("BuryDistance", "bury_distance"),
                      ("CylinderHeight", "cylinder_height")):
        if key in p:
            act.set_editor_property(prop, float(p[key]))
    comp = act.get_editor_property("mesh")
    comp.set_skeletal_mesh_asset(mesh)
    comp.set_material(0, unreal.load_asset(mic_path))
    return True


def place_lightwave(eas, atlas_document, legacy_wave=False):
    """Spawn the manifest driver or the retained legacy endpoint driver."""
    if legacy_wave:
        actor_path = CONV / f"{MAP}.t3d.json"
        if not actor_path.is_file():
            return 0
        groups = Counter()
        for actor in json.loads(actor_path.read_text())["actors"]:
            props = actor.get("props", {})
            if "light" in actor["cls"].lower() and props.get("LightType") \
                    in ("LT_Pulse", "LT_SubtlePulse"):
                groups[(int(float(props.get("LightPeriod", 64))),
                        int(float(props.get("LightPhase", 0))))] += 1
        if not groups:
            return 0
        period, phase = groups.most_common(1)[0][0]
        act = eas.spawn_actor_from_class(
            unreal.WoTLightWave, unreal.Vector(0, 0, 0))
        act.set_actor_label(f"LightWave_{MAP}")
        act.set_editor_property("period", float(period))
        act.set_editor_property("phase", float(phase))
        return 1

    try:
        groups = light_wave_driver_groups(atlas_document["animation"])
    except (KeyError, TypeError, ValueError) as exc:
        raise RuntimeError(
            f"invalid {MAP}_lmatlas.json light-wave driver: {exc}") from exc
    if groups is None:
        return 0
    act = eas.spawn_actor_from_class(unreal.WoTLightWave,
                                     unreal.Vector(0, 0, 0))
    act.set_actor_label(f"LightWave_{MAP}")
    actor_groups = []
    for wire_type, period, phase in groups:
        group = unreal.WoTLightWaveGroup()
        group.set_editor_property("type", wire_type)
        group.set_editor_property("period", period)
        group.set_editor_property("phase", phase)
        actor_groups.append(group)
    act.set_editor_property("groups", actor_groups)
    return 1


def make_gamma_boundary():
    """Apply retail Glide's display ramp, then exactly cancel sRGB output.

    The scene arrives in display space.  Raising it to ``RampInvGamma`` here,
    before the piecewise sRGB decode, makes the final output D^(1/g).  Reconcile
    a version tag so an existing pre-ramp asset cannot stay stale.
    """
    collection_path = "/Game/WoT/Materials/MPC_WoTDisplay"
    collection_exists = EAL.does_asset_exist(collection_path)
    collection = unreal.load_asset(collection_path) if collection_exists else \
        ATT.create_asset("MPC_WoTDisplay", "/Game/WoT/Materials",
                         unreal.MaterialParameterCollection,
                         unreal.MaterialParameterCollectionFactoryNew())
    if not collection:
        raise RuntimeError(f"could not create or load {collection_path}")
    current_scalars = tuple(
        (str(row.get_editor_property("parameter_name")),
         float(row.get_editor_property("default_value")))
        for row in collection.get_editor_property("scalar_parameters"))
    expected_default = 1.0 / 1.4
    scalar_contract_matches = len(current_scalars) == 1 \
        and current_scalars[0][0] == "RampInvGamma" \
        and abs(current_scalars[0][1] - expected_default) < 1.0e-6
    collection_dirty = not collection_exists
    if not scalar_contract_matches:
        parameter = unreal.CollectionScalarParameter()
        parameter.set_editor_property("parameter_name", "RampInvGamma")
        parameter.set_editor_property("default_value", expected_default)
        collection.set_editor_property("scalar_parameters", [parameter])
        collection_dirty = True
    if collection.get_editor_property("vector_parameters"):
        collection.set_editor_property("vector_parameters", [])
        collection_dirty = True
    if collection_dirty:
        EAL.save_asset(collection_path)

    material_path = "/Game/WoT/Materials/M_WoT_GammaBoundary"
    mi_path = "/Game/WoT/Materials/Instances/MI_GammaBoundary"
    graph_tag = "WoTGammaBoundaryGraph"
    graph_version = "glide-display-ramp-v2"
    MEL = unreal.MaterialEditingLibrary
    if EAL.does_asset_exist(material_path):
        m = unreal.load_asset(material_path)
        if not m:
            raise RuntimeError(f"could not load {material_path}")
        existing_mi = unreal.load_asset(mi_path) \
            if EAL.does_asset_exist(mi_path) else None
        if not collection_dirty \
                and EAL.get_metadata_tag(m, graph_tag) == graph_version \
                and existing_mi \
                and existing_mi.get_editor_property("parent") == m:
            return existing_mi
        clear_material_graph(unreal, m)
    else:
        m = ATT.create_asset("M_WoT_GammaBoundary", "/Game/WoT/Materials",
                             unreal.Material, unreal.MaterialFactoryNew())
    if not m:
        raise RuntimeError(f"could not create {material_path}")
    m.set_editor_property("material_domain", unreal.MaterialDomain.MD_POST_PROCESS)
    loc = getattr(unreal.BlendableLocation, "BL_SCENE_COLOR_BEFORE_BLOOM", None)
    if loc is None:
        loc = getattr(unreal.BlendableLocation, "BL_BEFORE_BLOOM")
    m.set_editor_property("blendable_location", loc)
    scene = MEL.create_material_expression(
        m, unreal.MaterialExpressionSceneTexture, -600, 0)
    scene.set_editor_property("scene_texture_id",
                              unreal.SceneTextureId.PPI_POST_PROCESS_INPUT0)
    ramp = MEL.create_material_expression(
        m, unreal.MaterialExpressionCollectionParameter, -600, 180)
    # Set the name first.  Assigning the collection then invokes the node's
    # editor change handler, which persists the collection row's GUID.
    ramp.set_editor_property("parameter_name", "RampInvGamma")
    ramp.set_editor_property("collection", collection)
    custom = MEL.create_material_expression(
        m, unreal.MaterialExpressionCustom, -300, 0)
    custom.set_editor_property("code", (
        "float3 D = pow(saturate(In.rgb), K);\n"
        "float3 lo = D / 12.92;\n"
        "float3 hi = pow((D + 0.055) / 1.055, 2.4);\n"
        "float3 t = step(0.04045, D);\n"
        "return lerp(lo, hi, t);\n"))
    custom.set_editor_property("output_type",
                               unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    custom.set_editor_property("description", "GlideRampThenSRGBDecode")
    scene_input = unreal.CustomInput()
    scene_input.set_editor_property("input_name", "In")
    ramp_input = unreal.CustomInput()
    ramp_input.set_editor_property("input_name", "K")
    custom.set_editor_property("inputs", [scene_input, ramp_input])
    MEL.connect_material_expressions(scene, "Color", custom, "In")
    MEL.connect_material_expressions(ramp, "", custom, "K")
    MEL.connect_material_property(
        custom, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    MEL.recompile_material(m)
    EAL.set_metadata_tag(m, graph_tag, graph_version)
    EAL.save_asset(material_path)

    mi = unreal.load_asset(mi_path) if EAL.does_asset_exist(mi_path) else \
        ATT.create_asset("MI_GammaBoundary", "/Game/WoT/Materials/Instances",
                         unreal.MaterialInstanceConstant,
                         unreal.MaterialInstanceConstantFactoryNew())
    if not mi:
        raise RuntimeError(f"could not create or load {mi_path}")
    mi.set_editor_property("parent", m)
    EAL.save_asset(mi_path)
    return mi


def place_navmesh(eas, meshes, real_bounds=None):
    # NavMesh bounds sized to the world-mesh bounds so seeking projectiles'
    # blocked-route steering activates (AWoTSeekingProjectile path queries).
    # Tiles generate at map load (RuntimeGeneration=Dynamic in DefaultEngine
    # .ini) — no editor-time nav build, which would need an async tick loop
    # under the commandlet. Volume brush = the class default 200uu cube,
    # scaled to cover the bounds (+one pawn height of headroom).
    if real_bounds is not None:
        minimum, maximum = real_bounds
        origin = unreal.Vector(*[(a + b) * 0.5 for a, b in zip(minimum, maximum)])
        ext = unreal.Vector(*[(b - a) * 0.5 for a, b in zip(minimum, maximum)])
    else:
        # Splitting render-gated fractal slots must not shrink navigation coverage.
        # For the historical one-mesh path this union reduces exactly to that
        # mesh's original origin and extent.
        bounds = [mesh.get_bounds() for mesh in meshes]
        if not bounds:
            return 0
        if len(bounds) == 1:
            # Preserve the no-split map serialization exactly (including floats).
            origin, ext = bounds[0].origin, bounds[0].box_extent
        else:
            min_x = min(b.origin.x - b.box_extent.x for b in bounds)
            min_y = min(b.origin.y - b.box_extent.y for b in bounds)
            min_z = min(b.origin.z - b.box_extent.z for b in bounds)
            max_x = max(b.origin.x + b.box_extent.x for b in bounds)
            max_y = max(b.origin.y + b.box_extent.y for b in bounds)
            max_z = max(b.origin.z + b.box_extent.z for b in bounds)
            origin = unreal.Vector((min_x + max_x) * 0.5,
                                   (min_y + max_y) * 0.5,
                                   (min_z + max_z) * 0.5)
            ext = unreal.Vector((max_x - min_x) * 0.5,
                                (max_y - min_y) * 0.5,
                                (max_z - min_z) * 0.5)
    vol = eas.spawn_actor_from_class(unreal.NavMeshBoundsVolume, origin)
    pad = 100.0
    vol.set_actor_scale3d(unreal.Vector((ext.x + pad) / 100.0,
                                        (ext.y + pad) / 100.0,
                                        (ext.z + pad) / 100.0))
    vol.set_actor_label(f"NavBounds_{MAP}")
    return 1


# --- Angreal spawn pools (the MP maps' artifact pickups) -----------------
# The arenas place no fixed artifacts — every pickup comes from a weighted
# AngrealSpawnPool (Common/Uncommon/Rare). Map the UE1 pool classes to the
# ported AWoTAngrealInv*; unported angreal are skipped, and a pool left with
# nothing ported is not placed. See PORT_NOTES.
# Each ported UE1 'AngrealInvX' leaf has a UE5 'WoTAngrealInvX' class AND a
# generated pickup leaf BP (Tools/import/import_pickups.py) that carries its view
# mesh — place_spawnpools prefers the BP so the pickup renders (_pickup_class).
# Retail rarity RespawnTime comes from the {Common,Uncommon,Rare}AngrealSpawnPool
# subclass CDOs (exports/script/Angreal/*.uc); the port folds the near-empty subclasses
# into one AWoTAngrealSpawnPool and applies the value per placed instance instead
# (RespawnTime is EditAnywhere on AWoTInventory, matching retail's var()).
POOL_RESPAWN = {
    "AngrealSpawnPool":         0.0,
    "CommonAngrealSpawnPool":   20.0,
    "UncommonAngrealSpawnPool": 40.0,
    "RareAngrealSpawnPool":     80.0,
}


def pool_entries(props, cls):
    """[(ue5_class_name, weight), ...] for a pool's ported entries. Pure —
    the parsing/mapping is unit-tested against real t3d (no editor needed)."""
    return _effective_pool_entries(props, cls)


def _pickup_class(ue5):
    """The spawnable class for a ported artifact: its generated pickup leaf BP
    (which carries the view mesh, so the pickup renders) when one exists, else the
    bare C++ class (functional but invisible) as a fallback."""
    bp_path = f"/Game/WoT/Blueprints/Pickups/BP_{ue5}"
    if EAL.does_asset_exist(bp_path):
        bp = unreal.load_asset(bp_path)
        gc = bp.generated_class() if bp else None
        if gc is not None:
            return gc
    return getattr(unreal, ue5, None)


def place_spawnpools(eas):
    """Place the map's Angreal spawn pools as AWoTAngrealSpawnPool actors."""
    f = CONV / f"{MAP}.t3d.json"
    if not f.exists():
        return 0
    actors = json.loads(f.read_text())["actors"]
    placed = empty = 0
    unported = Counter()
    unresolved = Counter()
    for a in actors:
        cls = a["cls"]
        if cls not in POOL_RESPAWN:
            continue
        props = a.get("props", {})
        for name, _weight in pool_unported(props, cls):
            unported[name] += 1
        pool, weight = [], []
        for ue5, w in pool_entries(props, cls):
            cls_obj = _pickup_class(ue5)
            if cls_obj is not None:
                pool.append(cls_obj)
                weight.append(w)
            else:
                unresolved[ue5] += 1
        if not pool:
            empty += 1
            continue   # no ported angreal in this pool yet
        act = eas.spawn_actor_from_class(unreal.WoTAngrealSpawnPool,
                                         unreal.Vector(*a["location"]))
        act.set_actor_rotation(ue1_rot(a["rotation"]), False)
        act.set_editor_property("pool", pool)
        act.set_editor_property("weight", weight)
        act.set_editor_property("respawn_time", POOL_RESPAWN[cls])
        act.set_editor_property("respawn", True)   # bRespawn — UE strips the bool b-prefix
        act.set_actor_label(f"{cls}_{a.get('name', placed)}")
        placed += 1
    skipped_classes = ", ".join(
        f"{name}:{count}" for name, count in sorted(unported.items())) or "none"
    unreal.log(f"SPAWNPOOL {MAP}: placed={placed} still_empty={empty} "
               f"unported_skips={sum(unported.values())} "
               f"classes={skipped_classes} "
               f"unresolved_ported={dict(sorted(unresolved.items()))}")
    return placed


def place_zones(eas):
    """Place gameplay zone volumes (water/pain/heal/gravity/current/friction) as
    AWoTZoneVolume actors, extracted from the compiled BSP by Tools/convert/
    zones.py. Extent is the zone's node-polygon AABB (raw UE1 units — same space
    actors place in)."""
    f = EXPORTS / "zones" / f"{MAP}.zones.json"
    if not f.exists():
        return 0
    placed = 0
    gameplay_zones, _, _ = _zone_export_sections()
    for z in gameplay_zones:
        lo, hi = z["aabb"]
        center = [(lo[i] + hi[i]) / 2.0 for i in range(3)]
        half = [max((hi[i] - lo[i]) / 2.0, 1.0) for i in range(3)]
        act = eas.spawn_actor_from_class(unreal.WoTZoneVolume, unreal.Vector(*center))
        # 'Collision' UPROPERTY isn't a Python attr; fetch the box by class.
        act.get_component_by_class(unreal.BoxComponent).set_box_extent(
            unreal.Vector(*half), False)
        act.set_editor_property("edit_team", int(z.get("edit_team", 255)))
        act.set_editor_property(
            "min_edit_distance", int(z.get("min_edit_distance", 0)))
        act.set_editor_property(
            "edit_view_angle",
            unreal.Rotator(
                roll=0.0,
                pitch=float(z.get("edit_view_angle_pitch", -8192)) / 65536.0 * 360.0,
                yaw=0.0))
        act.set_editor_property(
            "edit_view_distance", float(z.get("edit_view_distance", 32.0)))
        act.set_editor_property("allow_editing", bool(z.get("allow_editing", True)))
        if z.get("water"):
            act.set_editor_property("water_zone", True)   # bWaterZone (b stripped)
        if z.get("fog"):
            act.set_editor_property("fog_zone", True)     # viewer gate for volumetrics
        if z.get("pain") and z.get("damage_per_sec"):
            act.set_editor_property("pain_zone", True)
            act.set_editor_property("damage_per_sec", float(z["damage_per_sec"]))
            if z.get("damage_type"):
                # ZoneInfo.DamageType (AEFWS name, e.g. xxFxx) — lets element
                # shields eat typed zone pain; absent = class default None.
                act.set_editor_property("damage_type", z["damage_type"])
        if z.get("heal") and z.get("heal_per_sec"):
            act.set_editor_property("heal_zone", True)    # bHealZone (b stripped)
            act.set_editor_property("heal_per_sec", int(z["heal_per_sec"]))
            if z.get("heal_sound"):
                act.set_editor_property("heal_sound_path", z["heal_sound"])
        if z.get("ground_friction", 8.0) != 8.0:
            act.set_editor_property("zone_ground_friction",
                                    float(z["ground_friction"]))
        if abs(z.get("fluid_friction", 1.2) - 1.2) > 1e-6:
            act.set_editor_property("zone_fluid_friction",
                                    float(z["fluid_friction"]))
        vel = z.get("velocity") or []
        if any(vel):
            act.set_editor_property("zone_velocity", unreal.Vector(*vel))
        grav = z.get("gravity") or []
        if any(grav):
            act.set_editor_property("override_gravity", True)
            act.set_editor_property("zone_gravity", unreal.Vector(*grav))
        act.set_actor_label(f"Zone_{z['actor']}")
        placed += 1
    unreal.log(f"ZONES: placed {placed} volume(s)")
    return placed


_BUDGET_LINE = re.compile(
    r"Budget\((\d+)\)=\((?:ResourcePackageName=(\w+),ResourceAdapterName=(\w+),)?"
    r"ItemCount=(\d+)\)")


def _budget_defaults(cls):
    """Faction budget class defaultproperties (exports/script/WOT/<cls>.uc):
    index -> [pkg, adapter, count]. T3D serializes only the CHANGED lines
    (often bare ItemCount overrides), so the class defaults are the base."""
    f = EXPORTS / "script" / "WOT" / f"{cls}.uc"
    lines = {}
    if f.exists():
        for m in _BUDGET_LINE.finditer(f.read_text(errors="replace")):
            i, pkg, adapter, count = m.groups()
            lines[int(i)] = [pkg or "", adapter or "", int(count)]
    return lines


_SOUND_DISPATCHER_EVENT = re.compile(r"EventList\((\d+)\)$")


def _sound_dispatcher_event(value):
    """Parse one T3D SoundEvent tuple into importer-facing scalar fields."""
    text = str(value or "").strip()
    if text.startswith("(") and text.endswith(")"):
        text = text[1:-1]
    fields = {}
    for item in text.split(","):
        if "=" in item:
            key, field_value = item.split("=", 1)
            fields[key.strip()] = field_value.strip()
    subpath = sound_subpath(fields.get("Sound"))
    sound = ""
    if subpath:
        leaf = subpath.rsplit("/", 1)[-1]
        sound = f"/Game/WoT/Sounds/{subpath}.{leaf}"
    return {
        "delay": float(fields.get("Delay", 0.0)),
        "event": fields.get("Event", ""),
        "sound_source_tag": fields.get("SoundSourceTag", ""),
        "sound": sound,
        "sound_table_tag": fields.get("SoundTableTag", ""),
        "sound_table_slot": fields.get("SoundTableSlot", ""),
    }


def _sound_dispatcher_events(props):
    entries = {}
    for key, value in props.items():
        match = _SOUND_DISPATCHER_EVENT.match(key)
        if match:
            entries[int(match.group(1))] = _sound_dispatcher_event(value)
    if not entries:
        return []
    # Retain internal fixed-array holes. Runtime skips a blank row without its
    # delay, but still performs the per-index range gate like retail.
    return [entries.get(index, _sound_dispatcher_event(""))
            for index in range(max(entries) + 1)]


def _set_sound_dispatcher_properties(actor, props):
    trigger_types = {
        "TT_PlayerProximity": unreal.WoTTriggerType.PLAYER_PROXIMITY,
        "TT_PawnProximity": unreal.WoTTriggerType.PAWN_PROXIMITY,
        "TT_ClassProximity": unreal.WoTTriggerType.CLASS_PROXIMITY,
        "TT_AnyProximity": unreal.WoTTriggerType.ANY_PROXIMITY,
        "TT_Shoot": unreal.WoTTriggerType.SHOOT,
    }
    actor.set_editor_property("tag_name", props.get("Tag", ""))
    actor.set_editor_property("event_name", props.get("Event", ""))
    actor.set_editor_property(
        "trigger_type", trigger_types[props.get("TriggerType", "TT_PlayerProximity")])
    actor.set_editor_property("re_trigger_delay", float(props.get("ReTriggerDelay", 0.0)))
    actor.set_editor_property("sound_volume", float(props.get("SoundVolume", 1.0)))
    actor.set_editor_property("sound_radius", float(props.get("SoundRadius", 2048.0)))
    actor.set_editor_property("sound_pitch", float(props.get("SoundPitch", 1.0)))
    actor.set_editor_property(
        "sound_override", str(props.get("bSoundOverride", "False")).lower() == "true")
    actor.set_editor_property("max_distance", int(float(props.get("MaxDistance", 0))))
    actor.set_editor_property(
        "from_sound_source", str(props.get("bFromSoundSource", "False")).lower() == "true")
    actor.set_editor_property(
        "terminate", str(props.get("bTerminate", "False")).lower() == "true")
    rows = []
    for entry in _sound_dispatcher_events(props):
        row = unreal.WoTSoundDispatcherEvent()
        for field in ("delay", "event", "sound_source_tag",
                      "sound_table_tag", "sound_table_slot"):
            row.set_editor_property(field, entry[field])
        if entry["sound"]:
            sound_asset = unreal.load_asset(entry["sound"])
            if sound_asset is None:
                raise RuntimeError(f"missing SoundDispatcher sound {entry['sound']}")
            # FSoftObjectProperty's Python bridge accepts the loaded UObject;
            # the TSoftObjectPtr serializes its asset path into the map.
            row.set_editor_property("sound", sound_asset)
        rows.append(row)
    actor.set_editor_property("event_list", rows)


def _sound_dispatcher_markers(actors):
    """Markers for absent/non-registry/duplicate first tagged sound sources."""
    by_tag = {}
    tag_counts = Counter()
    for actor in actors:
        tag = actor.get("props", {}).get("Tag")
        if tag:
            tag_counts[tag] += 1
            by_tag.setdefault(tag, actor)  # UE1 AllActors returns the first actor
    registry_classes = {"Trigger", "SoundDispatcher"}
    markers = {}
    for dispatcher in (actor for actor in actors
                       if actor["cls"] == "SoundDispatcher"):
        for entry in _sound_dispatcher_events(dispatcher.get("props", {})):
            tag = entry["sound_source_tag"]
            if not tag or tag in markers:
                continue
            source = by_tag.get(tag)
            if source and source["cls"] in registry_classes \
                    and tag_counts[tag] == 1:
                continue
            markers[tag] = (source or dispatcher)["location"]
    return markers


def place_battle_actors(eas):
    """Place Battle economy/teleport actors and T3D SoundDispatchers."""
    f = CONV / f"{MAP}.t3d.json"
    if not f.exists():
        return 0
    actors = json.loads(f.read_text())["actors"]
    sound_markers = _sound_dispatcher_markers(actors)
    placed = 0
    for a in actors:
        cls, p = a["cls"], a.get("props", {})
        loc = unreal.Vector(*a["location"])
        if cls == "BattleInfo":
            act = eas.spawn_actor_from_class(unreal.WoTBattleInfo, loc)
            act.set_editor_property("num_teams", int(p.get("NumTeams", 0)))
            # TeamClass(n) name faction pawns (WOTPawns.Forsaken...); factions
            # are unported, so every team maps to the one player character
            # (recorded divergence — faction identity returns with the pawns).
            n = int(p.get("NumTeams", 0))
            try:
                act.set_editor_property(
                    "team_class",
                    [unreal.WoTCharacter if i < n else None for i in range(4)])
            except Exception as e:  # noqa: BLE001 — fixed-array set varies by UE
                unreal.log_warning(f"BATTLE: team_class not set ({e})")
            act.set_actor_label(a["name"])
        elif cls == "SealAltar":
            act = eas.spawn_actor_from_class(unreal.WoTSealAltar, loc)
            act.set_editor_property("team", int(p.get("Team", 255)))
            if "SealGoal" in p:
                act.set_editor_property("seal_goal", int(p["SealGoal"]))
            act.set_actor_label(a["name"])
        elif cls == "MashadarGuide":
            # MashadarGuide.uc:26-35: map-placed guides begin parked and are
            # deployed by the nearest MashadarFocusPawn. RestPoint/path nodes
            # fold into the guide's spawn anchor; MashadarManager is runtime-folded.
            act = eas.spawn_actor_from_class(unreal.WoTMashadarGuide, loc)
            act.set_actor_rotation(ue1_rot(a.get("rotation", [0, 0, 0])), False)
            act.set_editor_property("team", int(p.get("Team", 255)))
            act.set_editor_property("search_radius", float(p.get("SearchRadius", 600.0)))
            # Deployed guides trigger at TriggerRadius (MashadarGuide.uc:247); the
            # Battle maps author per-instance overrides (e.g. 500 vs the 200 CDO).
            act.set_editor_property("trigger_radius", float(p.get("TriggerRadius", 200.0)))
            act.set_editor_property("citadel_placed", True)
            act.set_actor_label(a["name"])
        elif cls.endswith("Budget") and cls != "BudgetTrigger":
            act = eas.spawn_actor_from_class(unreal.WoTBudgetInfo, loc)
            act.set_editor_property("team", int(p.get("Team", 0)))
            act.set_editor_property("begin_edit_event", p.get("BeginEditEvent", ""))
            act.set_editor_property("end_edit_event", p.get("EndEditEvent", ""))
            lines = _budget_defaults(cls)
            for k, v in p.items():
                m = re.match(r"Budget\((\d+)\)$", k)
                if not m:
                    continue
                i = int(m.group(1))
                cm = re.search(r"ItemCount=(\d+)", v)
                if cm:
                    lines.setdefault(i, ["", "", 0])[2] = int(cm.group(1))
            budget = []
            for i in sorted(lines):
                pkg, adapter, count = lines[i]
                r = unreal.WoTBudgetResource()
                r.set_editor_property("resource_package_name", pkg)
                r.set_editor_property("resource_adapter_name", adapter)
                r.set_editor_property("item_count", count)
                budget.append(r)
            act.set_editor_property("budget", budget)
            act.set_actor_label(a["name"])
        elif cls == "SoundDispatcher":
            act = eas.spawn_actor_from_class(unreal.WoTSoundDispatcher, loc)
            act.set_actor_rotation(ue1_rot(a.get("rotation", [0, 0, 0])), False)
            _set_sound_dispatcher_properties(act, p)
            radius = float(p.get("CollisionRadius", 40.0))
            height = float(p.get("CollisionHeight", 40.0))
            act.get_editor_property("capsule").set_capsule_size(
                radius, max(height, radius), False)
            act.set_actor_label(a["name"])
        elif cls == "TeleporterWithEffects" or cls == "Teleporter":
            act = eas.spawn_actor_from_class(unreal.WoTTeleporter, loc)
            act.set_actor_rotation(ue1_rot(a.get("rotation", [0, 0, 0])), False)
            act.set_editor_property("url", p.get("URL", "").strip('"'))
            if p.get("Tag"):
                act.set_editor_property("tele_tag", p["Tag"])
            if p.get("Event"):
                act.set_editor_property("event_name", p["Event"])
            for source, dest in (
                    ("EnterEffectClass", "enter_effect_config"),
                    ("ExitEffectClass", "exit_effect_config")):
                if p.get(source):
                    value = str(p[source])
                    if "'" in value:
                        value = value.split("'", 1)[1].rsplit("'", 1)[0]
                    act.set_editor_property(dest, value.rsplit(".", 1)[-1])
            for source, dest in (
                    ("EnterEffectSound", "enter_effect_sound_path"),
                    ("ExitEffectSound", "exit_effect_sound_path")):
                if p.get(source):
                    act.set_editor_property(dest, sound_subpath(p[source]) or "")
            cap = act.get_component_by_class(unreal.CapsuleComponent)
            cap.set_capsule_size(float(p.get("CollisionRadius", 18.0)),
                                 float(p.get("CollisionHeight", 40.0)), False)
            act.set_actor_label(a["name"])
        else:
            continue
        placed += 1
    for tag, location in sound_markers.items():
        marker = eas.spawn_actor_from_class(
            unreal.WoTTaggedMarker, unreal.Vector(*location))
        marker.set_editor_property("tag_name", tag)
        marker.set_actor_label(f"WoTTaggedMarker_{tag}")
        placed += 1
    if placed:
        unreal.log(f"BATTLE: placed {placed} win-economy actor(s)")
    return placed


_FOG_HLSL = r"""
float3 rd = WPos - Cam;
float rl = length(rd);
if (rl < 1e-3) return float3(0, 0, 0);
rd /= rl;
float3 L = Center - Cam;
float c0 = dot(L, rd);
float dp2 = dot(L, L) - c0 * c0;
float R2 = R * R;
if (dp2 >= R2) return float3(0, 0, 0);
// Along-ray distance to the opaque scene: SceneDepth is view-Z; our own
// sphere pixel sits at view-Z PixD and ray-distance rl, so the conversion
// factor is rl/PixD (no view-forward needed).
float tscene = SceneD * rl / max(PixD, 1e-3);
float h = sqrt(R2 - dp2);
float u2 = min(c0, h);
float u1 = max(c0 - tscene, -h);
if (u1 >= u2) return float3(0, 0, 0);
float rR = 1.0 / R;
float D2 = dp2 * rR * rR;
float I = 3.0 - 3.0 * D2;
float a = u2 * rR;
float b = u1 * rR;
float F = saturate(VolB * (a * (I - a * a) - b * (I - b * b)));
float3 C = clamp(FogScale * FogColor * F, 0.0, 1.0);
// Additive emissive in the linear buffer: pow encodes the display-space fog
// (corona convention). Linear addition's sub-additivity vs the display sum
// stands in for the screen blend's dst damping (measured on Arena_07's pit).
return pow(C, 2.2);
"""


def _make_volumefog_master(name, modulated):
    """UE1 volumetric fog, one ADDITIVE sphere per light. Display-space fog
    C = min(color*min(VolB*chord,1),1) with the retail parabolic-density chord
    integral (density 3(1-r^2/R^2) along the eye ray, clipped to the sphere
    and the opaque scene). No 2x: the surface fog-map path stores color*F
    directly (the vertex-fog 2x is mesh-only) — golden's unsaturated pit core
    (214) matches color*F through the blend; a 2x model clamps to 255.

    Retail frame blend is per-channel screen (C + dst*(1-C)). Both a
    modulate+additive split (UE5 composites Modulate in a separate LATER
    translucency pass; the pair self-cancels) and AlphaComposite (emissive
    path measured dead under this renderer) failed here, so the port ships
    the proven single ADDITIVE pass: emissive = C^2.2 into the linear buffer.
    Linear addition is sub-additive vs the display-space sum, which stands in
    for screen's dst damping (Arena_07 pit: predicted 213 vs golden 214 at
    the core). All math lives in the Custom node; emissive connects directly
    (the minimal graph is the one measured working)."""
    full = f"/Game/WoT/Materials/{name}"
    if EAL.does_asset_exist(full):
        return unreal.load_asset(full)
    MEL = unreal.MaterialEditingLibrary
    m = ATT.create_asset(name, "/Game/WoT/Materials", unreal.Material,
                         unreal.MaterialFactoryNew())

    def expr(cls, x, y):
        return MEL.create_material_expression(m, cls, x, y)

    m.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    m.set_editor_property("blend_mode", unreal.BlendMode.BLEND_ADDITIVE)
    try:
        m.set_editor_property("disable_depth_test", True)
    except Exception as ex:  # noqa: BLE001
        unreal.log_warning(f"{name}: disable_depth_test not set ({ex})")

    cu = expr(unreal.MaterialExpressionCustom, -700, 0)
    cu.set_editor_property("code", _FOG_HLSL)
    cu.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    cu.set_editor_property("description", "WoTVolumeFog")
    ins = []
    for nm in ("WPos", "Cam", "Center", "SceneD", "PixD", "FogColor", "VolB", "R",
               "FogScale"):
        ci = unreal.CustomInput()
        ci.set_editor_property("input_name", nm)
        ins.append(ci)
    cu.set_editor_property("inputs", ins)

    wp = expr(unreal.MaterialExpressionWorldPosition, -1000, -250)
    cam = expr(unreal.MaterialExpressionCameraPositionWS, -1000, -150)
    ctr = expr(unreal.MaterialExpressionObjectPositionWS, -1000, -50)
    sd = expr(unreal.MaterialExpressionSceneDepth, -1000, 50)
    pd = expr(unreal.MaterialExpressionPixelDepth, -1000, 150)
    fc = expr(unreal.MaterialExpressionVectorParameter, -1000, 250)
    fc.set_editor_property("parameter_name", "FogColor")
    fc.set_editor_property("default_value", unreal.LinearColor(1, 1, 1, 1))
    vb = expr(unreal.MaterialExpressionScalarParameter, -1000, 450)
    vb.set_editor_property("parameter_name", "VolB")
    vb.set_editor_property("default_value", 1.0)
    rr = expr(unreal.MaterialExpressionScalarParameter, -1000, 550)
    rr.set_editor_property("parameter_name", "R")
    rr.set_editor_property("default_value", 25.0)
    fs = expr(unreal.MaterialExpressionScalarParameter, -1000, 650)
    fs.set_editor_property("parameter_name", "FogScale")
    # Calibrated on Arena_07's red pit vs golden (mean-exact at 2.5: 110.8 vs
    # 109.5; the 1x model read 65, 2x 90). Folds the RGBA7 fog-map upload
    # doubling plus the additive-in-linear sub-additivity compensation into
    # one measured constant — the same calibration pattern as MLSurfScale.
    fs.set_editor_property("default_value", 2.5)
    for src, out, idx in ((wp, "", 0), (cam, "", 1), (ctr, "", 2), (sd, "", 3),
                          (pd, "", 4), (fc, "", 5), (vb, "", 6), (rr, "", 7),
                          (fs, "", 8)):
        MEL.connect_material_expressions(
            src, out, cu, ins[idx].get_editor_property("input_name"))

    MEL.connect_material_property(cu, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    unreal.MaterialEditingLibrary.recompile_material(m)
    EAL.save_asset(full)
    return m


def place_foglights(eas):
    """Place AWoTFogLight sphere pairs from exports/fog/<Map>.fog.json (the
    retail leaf-baked volumetric set + the runtime dynamic-light rule)."""
    f = EXPORTS / "fog" / f"{MAP}.fog.json"
    if not f.exists():
        return 0
    lights = json.loads(f.read_text())
    if not lights:
        return 0
    fog_master = _make_volumefog_master("M_WoT_VolumeFog", False)
    MEL = unreal.MaterialEditingLibrary
    n = 0
    for fl in lights:
        act = eas.spawn_actor_from_class(unreal.WoTFogLight,
                                         unreal.Vector(*fl["location"]))
        act.set_actor_label(f"Fog_{fl['name']}")
        act.set_editor_property("radius", float(fl["radius"]))
        c = fl["color"]
        act.set_editor_property("fog_color",
                                unreal.LinearColor(c[0], c[1], c[2], 1.0))
        act.set_editor_property("vol_brightness", float(fl["vol_b"]))
        s = float(fl["radius"]) / 50.0    # engine sphere radius = 50
        act.get_editor_property("sphere_mod").set_editor_property("visible", False)
        for comp_prop, master, prefix in (
                ("sphere_add", fog_master, "MI_Fog_"),):
            mic_name = f"{prefix}{MAP}_{fl['name']}"
            mic_path = f"/Game/WoT/Materials/Instances/{mic_name}"
            if not EAL.does_asset_exist(mic_path):
                mic = ATT.create_asset(mic_name, "/Game/WoT/Materials/Instances",
                                       unreal.MaterialInstanceConstant,
                                       unreal.MaterialInstanceConstantFactoryNew())
                mic.set_editor_property("parent", master)
            else:
                mic = unreal.load_asset(mic_path)
                if mic.get_editor_property("parent") != master:
                    mic.set_editor_property("parent", master)
            MEL.set_material_instance_vector_parameter_value(
                mic, "FogColor", unreal.LinearColor(c[0], c[1], c[2], 1.0))
            MEL.set_material_instance_scalar_parameter_value(
                mic, "VolB", float(fl["vol_b"]))
            MEL.set_material_instance_scalar_parameter_value(
                mic, "R", float(fl["radius"]))
            EAL.save_asset(mic_path)
            comp = act.get_editor_property(comp_prop)
            comp.set_relative_scale3d(unreal.Vector(-s, s, s))
            comp.set_material(0, mic)
        n += 1
    unreal.log(f"FOG: placed {n} volumetric fog light(s)")
    return n


def _mirror_world_component(source_actor, target_actor):
    """Copy bookkeeping formerly owned by the one combined world component."""
    target_actor.set_editor_property(
        "tags", source_actor.get_editor_property("tags"))
    source = source_actor.static_mesh_component
    target = target_actor.static_mesh_component
    for prop in ("component_tags", "lighting_channels", "mobility"):
        target.set_editor_property(prop, source.get_editor_property(prop))


def _spawn_world_meshes(eas, mesh_parts, portal=False):
    actors = []
    template = None
    has_base = any(material_slot is None for _, material_slot in mesh_parts)
    for index, (mesh_path, material_slot) in enumerate(mesh_parts):
        actor = eas.spawn_actor_from_class(
            unreal.StaticMeshActor, unreal.Vector(0, 0, 0))
        actor.static_mesh_component.set_static_mesh(unreal.load_asset(mesh_path))
        if template:
            _mirror_world_component(template, actor)
        else:
            template = actor
        if portal:
            # PROFILE, not just the enabled flag: a PLACED actor's saved
            # BodyInstance re-applies its collision profile template on load,
            # so a bare
            # set_collision_enabled(NO_COLLISION) silently reverts to
            # BlockAll in the running game — the packaged BSP proof caught
            # portal-directed darts colliding with the pit lava portal.
            actor.static_mesh_component.set_collision_profile_name(
                "NoCollision")
            actor.static_mesh_component.set_collision_enabled(
                unreal.CollisionEnabled.NO_COLLISION)
        if material_slot is None or (not has_base and index == 0):
            actor.set_actor_label(
                f"BSP_{MAP}_Portals" if portal else f"BSP_{MAP}")
        else:
            actor.set_actor_label(f"BSP_{mesh_path.rsplit('/', 1)[-1]}")
        actors.append(actor)
    return actors


def build_level(mesh_parts, insts, masters, textures, portal_mesh_parts=None,
                mats=None, bsp_real_bounds=None, atlas_document=None,
                legacy_wave=False):
    les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    level_path = f"/Game/WoT/Maps/{MAP}"
    if EAL.does_asset_exist(level_path):
        # deleting + new_level trips stale-registry validation; reuse instead
        if not les.load_level(level_path):
            raise RuntimeError("load_level failed")
        eas0 = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
        for a in eas0.get_all_level_actors():
            eas0.destroy_actor(a)
    elif not les.new_level(level_path):
        raise RuntimeError("new_level failed")
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    # spawn_actor_from_object crashes headless (viewport placement subsystem);
    # spawn actor classes and assign each mesh directly. Record-bearing slots
    # own components so LastRenderTimeOnScreen is consumer-specific.
    _spawn_world_meshes(eas, mesh_parts)
    if portal_mesh_parts:
        # textured zone portals (water surfaces etc.): visible, never solid —
        # UE1 portals don't collide (players wade through the water plane)
        _spawn_world_meshes(eas, portal_mesh_parts, portal=True)
    counts = place_semantics(eas)
    counts.update(place_movers(eas, insts, masters, mats, textures))
    counts["decorations"] = place_decorations(eas)
    counts["coronas"] = place_coronas(eas)
    counts["leafmap"] = place_leafmap(eas)
    counts["sprayers"] = place_sprayers(eas, textures)
    counts["rotators"] = place_rotators(eas)
    counts["lightwave"] = place_lightwave(
        eas, atlas_document, legacy_wave=legacy_wave)
    counts["navmesh"] = place_navmesh(
        eas, [unreal.load_asset(path) for path, _ in mesh_parts],
        bsp_real_bounds)
    counts["spawnpools"] = place_spawnpools(eas)
    counts["zones"] = place_zones(eas)
    counts["battle"] = place_battle_actors(eas)
    counts["foglights"] = place_foglights(eas)
    counts["lighttable"] = make_light_table()
    # Skylight approximates UE1 zone ambient (flat add). Most MP maps have
    # tiny/no ambient — the old flat 0.3 was an invention that inflated every
    # map's brightness. min-nonzero across zones dodges the 255 sky-void zones.
    ambient = counts.pop("ambient_min", 0.0) / 255.0
    sky = eas.spawn_actor_from_class(unreal.SkyLight, unreal.Vector(0, 0, 2000))
    # same display->linear decode as point lights. NO floor: a linear floor of
    # just 0.005 is ~23/255 in display space and flattens the dark end that
    # the original leaves near black.
    sky.light_component.set_editor_property("intensity", ambient ** 2.2)
    sky.light_component.set_editor_property("mobility", unreal.ComponentMobility.MOVABLE)
    if not counts.get("PlayerStart"):
        eas.spawn_actor_from_class(unreal.PlayerStart, unreal.Vector(0, 0, 200))
    if GAMMA_SPACE:
        ppv = eas.spawn_actor_from_class(unreal.PostProcessVolume,
                                         unreal.Vector(0, 0, 0))
        ppv.set_actor_label("GammaBoundary")
        ppv.set_editor_property("unbound", True)
        pset = ppv.get_editor_property("settings")
        pset.set_editor_property("weighted_blendables", unreal.WeightedBlendables(
            array=[unreal.WeightedBlendable(1.0, make_gamma_boundary())]))
        ppv.set_editor_property("settings", pset)
        counts["gamma_boundary"] = 1
    les.save_current_level()
    unreal.log(f"IMPORT-MAP-SEMANTICS {counts}")
    return level_path


def run_childsky():
    """Child-scene sky wiring (Tools/convert/setup_childsky.py) as the final
    import stage: imports <Map>_skygeo.glb, binds world MICs + SkyRest groups,
    spawns the camera-locked AWoTSkyBox, hides the FakeBackdrop windows + far sky
    slots, re-saves the level. Before this ran here, every map re-import
    silently LOST the childsky wiring until the script was re-run by hand.
    Exec'd, not imported: the script is a self-driving commandlet body (the
    documented wrapper pattern); argv carries (maps, conv_dir).

    Staleness gates (warn, don't fail — regeneration is an OFFLINE stage):
    <Map>_skygeo.json older than the map's t3d.json, and sky maps with no
    <Map>_skyrest.json at all (skybake --rest never probed what retail draws
    through portal sky sheets — sealed maps still emit covered_frac=1.0, so
    MISSING means unprobed, not sealed; the A04/07 black-notch bug hid behind
    exactly that gap)."""
    sk = CONV / f"{MAP}_skygeo.json"
    if not sk.exists():
        return "no-skygeo"
    if not json.loads(sk.read_text()).get("sky_origin"):
        return "no-sky"
    t3d = CONV / f"{MAP}.t3d.json"
    if t3d.exists() and sk.stat().st_mtime < t3d.stat().st_mtime:
        unreal.log_warning(
            f"[childsky] {MAP}: {sk.name} is OLDER than {t3d.name} — re-run "
            "skygeo.py (and skybake.py --rest) before trusting this import")
    rest = CONV / f"{MAP}_skyrest.json"
    if not rest.exists():
        unreal.log_warning(
            f"[childsky] {MAP}: no {rest.name} — skybake.py --rest has never "
            "probed this sky; portal-unsealed content would be missing")
    elif t3d.exists() and rest.stat().st_mtime < t3d.stat().st_mtime:
        unreal.log_warning(
            f"[childsky] {MAP}: {rest.name} is OLDER than {t3d.name} — re-run "
            "skybake.py --rest (then skygeo.py)")
    script = Path(unreal.Paths.convert_relative_path_to_full(
        unreal.Paths.project_dir())) / "Tools" / "convert" / "setup_childsky.py"
    saved_argv = sys.argv
    sys.argv = ["setup_childsky.py", MAP, str(CONV)]
    g = {}
    try:
        exec(compile(script.read_text(), str(script), "exec"), g)
    finally:
        sys.argv = saved_argv
    return g.get("res", {}).get(MAP, "no-result")


def run_gameplay_look():
    """WoTGameplayLook PPV (Tools/import/import_gameplay_look.py) as the final
    import stage: the unbound ToneCurveAmount=0/ExpandGamut=0 volume that makes
    gameplay/PIE render the flat-sRGB period look the capture rig measures.
    Before this ran here, every map re-import silently LOST the volume until
    the standalone script was re-run by hand — the exact failure mode
    run_childsky() closed for the sky wiring. Imported, not subprocessed/exec'd:
    the module guards its CLI under __main__ and exposes apply_gameplay_look(),
    which mutates an existing labeled PPV in place (no actor-GUID churn on
    re-import). Runs on the current level (build_level/run_childsky both leave
    MAP loaded); no per-map generated inputs exist, so unlike run_childsky
    there is no staleness to warn about."""
    tools_import = Path(unreal.Paths.convert_relative_path_to_full(
        unreal.Paths.project_dir())) / "Tools" / "import"
    if str(tools_import) not in sys.path:
        sys.path.insert(0, str(tools_import))
    import import_gameplay_look
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    res = import_gameplay_look.apply_gameplay_look(eas)
    unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).save_current_level()
    return res


def main():
    info = json.loads((CONV / f"{MAP}.bsp.json").read_text())
    world_mats = dict(info["materials"])
    pmats = info.get("portal_materials", {})
    mats = dict(world_mats)
    mats.update(pmats)          # portal slots share the same routing rules
    needed = {(m["package"], m["texture"]) for m in mats.values()}
    # detail/macro overlays (stored [package, name] in the manifest) of every
    # base texture the map uses
    needed |= {tuple(TEXINFO[t][k]) for t in list(needed) if t in TEXINFO
               for k in ("detail", "macro") if TEXINFO[t].get(k)}
    needed |= sprayer_texture_names()   # package-qualified (package, name) tuples
    unreal.log(f"IMPORT-MAP {MAP}: {len(mats)} materials "
               f"({len(pmats)} portal), {len(needed)} textures")
    if STRIPPED:
        # Validate every PNG and existing target class before the first asset
        # mutation, then make the entire isolated texture root failure-atomic.
        specs = _stripped_texture_specs(needed)
        prevalidate_stripped_textures(
            unreal, specs, EAL.does_asset_exist, unreal.load_asset)
        textures = run_isolated_import_transaction(
            lambda: _import_stripped_texture_specs(specs), TEXTURE_DESTINATION,
            EAL.does_directory_exist, EAL.delete_directory)
        unreal.log(
            f"IMPORT-MAP-STRIPPED-OK map={MAP} tex={len(textures)} "
            f"dest={TEXTURE_DESTINATION}")
        return
    # Failure ordering is load-bearing: parse the complete manifest and every
    # referenced PNG before texture import or shared graph reconciliation.
    atlas_document, response_files, legacy_files = \
        prevalidate_lightmap_atlases()
    legacy_manifest = "animation" not in atlas_document
    tools_import = Path(unreal.Paths.convert_relative_path_to_full(
        unreal.Paths.project_dir())) / "Tools" / "import"
    if str(tools_import) not in sys.path:
        sys.path.insert(0, str(tools_import))
    import make_mesh_modulate
    if legacy_manifest:
        # Resolve every shared dependency read-only before ordinary per-map
        # imports begin. Missing legacy infrastructure fails without repairing,
        # deleting, or replacing any shared material/MPC asset.
        masters = load_legacy_actor_masters()
        world_masters = load_legacy_world_masters()
        mesh_modulate = unreal.load_asset(make_mesh_modulate.ASSET_PATH)
        if not mesh_modulate:
            raise RuntimeError(
                "legacy lightmap manifest requires existing shared mesh "
                "modulate master; the compatibility path will not synthesize it")
    textures = import_textures(needed)
    if not legacy_manifest:
        masters = make_master()
        world_masters = make_world_masters()
        mesh_modulate = make_mesh_modulate.make_mesh_modulate_master(GAMMA_SPACE)
    insts = make_instances(mats, textures, masters)
    atlas = import_atlas()
    if legacy_manifest:
        response_textures = {}
        legacy_wave_paths = import_legacy_wave_atlases(legacy_files)
    else:
        atlas_document, response_textures = import_response_atlases(
            atlas_document, response_files)
        legacy_wave_paths = None
    # The wave-atlas plumbing became response atlases; the old
    # Arena_07-only carrier block moved into
    # import_stripped_bsp_parts (split-aware, all maps).
    winsts = make_world_instances(mats, textures, world_masters, atlas,
                                  atlas_document, response_textures,
                                  legacy_wave_paths)
    # world mesh slots: lit surfaces -> per-map lightmap MICs; opaque
    # unlit/sky slots keep the shared instances
    world_map = dict(insts)
    world_map.update(winsts)
    if BSP_STRIPPED:
        split_parts = split_bsp_glb_parts(
            CONV, MAP, REPO / "Intermediate" / "FractalWorldSplit" / MAP)
        main_parts = [part for part in split_parts if part["role"] == "main"]
        portal_parts = [part for part in split_parts if part["role"] == "portal"]
        if BSP_PROOF_CARRIER and not portal_parts:
            raise RuntimeError("Arena_07 proof requires its portal GLB")
        fractal_slots = [part["material_slot"] for part in main_parts
                         if part["material_slot"] is not None]
        portal_fractal_slots = [part["material_slot"] for part in portal_parts
                                if part["material_slot"] is not None]
        mesh_parts, main_geometries, assigned = import_stripped_bsp_parts(
            main_parts, "", world_map, masters)
        pmesh_parts = []
        if portal_parts:
            pmesh_parts, _portal_geometries, portal_assigned = \
                import_stripped_bsp_parts(
                    portal_parts, "_portals", world_map, masters)
            assigned += portal_assigned
        # Navigation bounds must cover the complete retail main world, not the
        # tiny carriers. The split parts are an exact partition of that source.
        bsp_real_bounds = (
            [min(geometry["bounds_min"][axis] for geometry in main_geometries)
             for axis in range(3)],
            [max(geometry["bounds_max"][axis] for geometry in main_geometries)
             for axis in range(3)],
        )
    else:
        runtime_keys = runtime_fractal_texture_keys(CONV / "flipbooks")
        fractal_slots = record_bearing_slots(world_mats, runtime_keys)
        portal_fractal_slots = record_bearing_slots(pmats, runtime_keys)
        mesh_parts = (import_split_meshes(fractal_slots) if fractal_slots
                      else [(import_mesh(), None)])
        assigned = 0
        for mesh_path, _ in mesh_parts:
            fix_collision(mesh_path)
            assigned += assign_materials(mesh_path, world_map, masters)
        pmesh_parts = []
        if pmats and (CONV / f"{MAP}_portals.glb").exists():
            pmesh_parts = (import_split_meshes(
                portal_fractal_slots, "_portals") if portal_fractal_slots
                else [(import_mesh("_portals"), None)])
            for pmesh_path, _ in pmesh_parts:
                assigned += assign_materials(pmesh_path, world_map, masters)
        bsp_real_bounds = None
        if BSP_PROOF_CPU:
            for proof_path, _ in list(mesh_parts) + list(pmesh_parts):
                proof_mesh = unreal.load_asset(proof_path)
                if proof_mesh is None:
                    raise RuntimeError(f"--bsp-proof-cpu: missing mesh {proof_path}")
                proof_mesh.set_editor_property("allow_cpu_access", True)
                proof_mesh.set_editor_property("never_stream", True)
                EAL.save_asset(proof_path)
    level = build_level(
        mesh_parts, insts, masters, textures, pmesh_parts, mats,
        bsp_real_bounds, atlas_document,
        legacy_wave=bool(legacy_wave_paths))
    childsky = run_childsky()
    look = run_gameplay_look()
    unreal.log(f"IMPORT-MAP-OK level={level} "
               f"mesh={[path for path, _ in mesh_parts]} tex={len(textures)} "
               f"mi={len(insts)} world_mi={len(winsts)} assigned={assigned} "
               f"portals={[path for path, _ in pmesh_parts] or 'none'} "
               f"fractal_slots={fractal_slots} "
               f"portal_fractal_slots={portal_fractal_slots} childsky={childsky} "
               f"meshmodulate={mesh_modulate.get_name()} gameplaylook={look}")


# The pythonscript commandlet runs this file as __main__ (same contract
# import_gameplay_look.py and make_mesh_modulate.py rely on), so the full
# import still fires on every commandlet invocation. The guard exists for
# DRIVER scripts that need a stage function (e.g. make_light_table) without
# the full import: set sys.argv = [_, MAP, CONV, MODE] and importlib.reload.
if __name__ == "__main__":
    main()
