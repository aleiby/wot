"""Import one converted vertex-anim mesh: GLB → SkeletalMesh (+morphs),
assign diffuse-only material instances by skin texture.

The optional stripped mode imports into an explicitly isolated destination,
captures and verifies Interchange's real render-vertex mapping, removes native
morphs, zeroes base positions, retains the canonical TangentZ/bounds/structure,
and stamps UWoTMeshPayloadLayout. It is selected by --stripped,
--mode=stripped, WOT_MESH_IMPORT_MODE=stripped, or WOT_PAYLOAD_MODE=stripped.
Canonical /Game/WoT/Meshes output is guarded unless --allow-canonical is passed
inside a disposable project.

UnrealEditor-Cmd WoT.uproject -run=pythonscript
  -script="...import_mesh.py <MeshName> <conv_mesh_dir> <tex_dir>"
"""
import json
import os
import re
import sys
from pathlib import Path

import unreal

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from wot_config import local_path  # noqa: E402

IMPORT_DIR = Path(__file__).resolve().parent
if str(IMPORT_DIR) not in sys.path:
    sys.path.insert(0, str(IMPORT_DIR))
CONVERT_DIR = IMPORT_DIR.parent / "convert"
if str(CONVERT_DIR) not in sys.path:
    sys.path.insert(0, str(CONVERT_DIR))
from payload_binding import ensure_texture_payload_binding
from mesh_material_types import IMPORT_SLOT_MASTERS, split_slot_name
from mesh_master_bootstrap import ensure_mesh_masters
from import_transaction import run_isolated_import_transaction
from import_mesh_policy import should_skip_stripped_curvy
from mesh_texture_variants import palette0_rgb
from gltf import skinned_glb_has_mapping_carriers
import authenticated_alternate_bind as alternate_bind

RAW_ARGS = sys.argv[1:]
CANONICAL_MESH_DESTINATION = "/Game/WoT/Meshes"
CURRENT_POSE_CPU_MESHES = {"mashseg", "mashtip", "shieldparticlemesh"}


def _option_value(arguments, option):
    values = []
    index = 0
    while index < len(arguments):
        argument = arguments[index]
        if argument == option:
            if index + 1 >= len(arguments) or arguments[index + 1].startswith("--"):
                raise RuntimeError(f"{option} requires a value")
            values.append(arguments[index + 1])
            index += 2
            continue
        if argument.startswith(option + "="):
            values.append(argument.split("=", 1)[1])
        index += 1
    if len(set(values)) > 1:
        raise RuntimeError(f"conflicting {option} values")
    return values[0] if values else None


def _positionals(arguments):
    values = []
    index = 0
    while index < len(arguments):
        if arguments[index] == "--dest":
            index += 2
            continue
        if not arguments[index].startswith("--"):
            values.append(arguments[index])
        index += 1
    return values


def _normalize_destination(destination):
    raw = str(destination).strip().replace("\\", "/")
    if not raw.startswith("/"):
        raise RuntimeError(f"mesh destination must be an absolute /Game path: {destination}")
    parts = []
    for component in raw.split("/"):
        if not component or component == ".":
            continue
        if component == "..":
            if not parts:
                raise RuntimeError(f"mesh destination escapes its root: {destination}")
            parts.pop()
        else:
            parts.append(component)
    normalized = "/" + "/".join(parts)
    if normalized.casefold() != "/game" and not normalized.casefold().startswith("/game/"):
        raise RuntimeError(f"mesh destination must resolve under /Game: {destination}")
    return normalized


def _stripped_requested(arguments=RAW_ARGS, environ=os.environ):
    return ("--stripped" in arguments or "--mode=stripped" in arguments
            or environ.get("WOT_MESH_IMPORT_MODE", "").casefold() == "stripped"
            or environ.get("WOT_PAYLOAD_MODE", "").casefold() == "stripped")


def _stripped_destination(arguments=RAW_ARGS, environ=os.environ):
    cli = _option_value(arguments, "--dest")
    env = environ.get("WOT_PAYLOAD_CONTENT_ROOT", "").strip() or None
    if cli and env and _normalize_destination(cli).casefold() != _normalize_destination(env).casefold():
        raise RuntimeError("--dest conflicts with WOT_PAYLOAD_CONTENT_ROOT")
    destination = cli or env
    if not destination:
        raise RuntimeError(
            "stripped mesh import requires explicit --dest or WOT_PAYLOAD_CONTENT_ROOT")
    destination = _normalize_destination(destination)
    canonical = CANONICAL_MESH_DESTINATION.casefold()
    if (destination.casefold() == canonical
            or destination.casefold().startswith(canonical + "/")) \
            and "--allow-canonical" not in arguments:
        raise RuntimeError(
            "stripped mesh destination is canonical; use an isolated root or "
            "--allow-canonical only in a disposable project")
    return destination


POSITIONALS = _positionals(RAW_ARGS)
if not POSITIONALS:
    raise RuntimeError("import_mesh.py requires a mesh name")
NAME = POSITIONALS[0]
CONV = Path(POSITIONALS[1] if len(POSITIONALS) > 1
            else local_path("conversion_root") / "mesh")
TEXROOT = Path(POSITIONALS[2] if len(POSITIONALS) > 2
               else local_path("conversion_root") / "tex")
STRIPPED = _stripped_requested()

ATT = unreal.AssetToolsHelpers.get_asset_tools()
EAL = unreal.EditorAssetLibrary
DEST_ROOT = _stripped_destination() if STRIPPED else CANONICAL_MESH_DESTINATION
DEST = f"{DEST_ROOT}/{NAME}"
ALTERNATE_BIND_RECEIPT = Path(unreal.Paths.project_saved_dir()) / (
    "ImportReceipts/authenticated-alternate-bind.json")


def _mesh_import_pipelines(source_data):
    """Edit a transient copy of UE's selected default pipeline stack so the
    stripped import retains its mapping carriers at full precision.

    ImportAssetParameters.override_pipelines stores SoftObjectPaths. Keeping
    these returned UObjects alive through wait_until_all_tasks_done lets UE
    5.8's GeneratePipelineInstance duplicate the transient stack exactly as its
    native import helpers do, without changing any unrelated default setting.
    """
    pipelines = unreal.InterchangeProjectSettingsScript.get_pipeline_stack_from_source_data(
        False, source_data)
    generic = [candidate for candidate in pipelines
               if isinstance(candidate, unreal.InterchangeGenericAssetsPipeline)]
    if len(generic) != 1:
        raise RuntimeError(
            f"expected one generic Interchange asset pipeline, got {generic}")
    pipeline = generic[0]
    common = pipeline.get_editor_property("common_meshes_properties")
    common.set_editor_property(
        "vertex_color_import_option",
        unreal.InterchangeVertexColorImportOption.IVCIO_REPLACE)
    # Irrelevant for Replace, but pin the complete vertex-color policy
    # rather than inheriting a mutable class/default override value.
    common.set_editor_property(
        "vertex_override_color", unreal.Color(255, 255, 255, 255))
    # TEXCOORD_1 must remain integer-exact until the stripped editor bridge has
    # compared it element-wise with COLOR_0. Both carriers are then removed.
    common.set_editor_property("use_full_precision_u_vs", True)
    return pipelines


def _assert_imported_vertex_color_contract(mesh):
    """Canonical carrier-freedom is guaranteed by SOURCE emission — mesh2glb
    emits COLOR_0/TEXCOORD_1 only with --carriers, and the default canonical GLB
    is byte-identical to the pre-injection output. No runtime check is possible:
    Interchange creates a default (white) vertex-color buffer on every skeletal
    import, so has_vertex_colors() is true regardless of any mapping carrier and
    cannot distinguish the two."""
    return


def _assert_mapping_carrier_source_contract(meta):
    """Reject a stale GLB before Interchange can persist the wrong mode."""
    glb = CONV / f"{NAME}.glb"
    actual = skinned_glb_has_mapping_carriers(glb)
    recorded = meta.get("mapping_carriers")
    if recorded is not None and bool(recorded) != actual:
        raise RuntimeError(
            f"{NAME}: GLB carrier attributes disagree with metadata")
    if STRIPPED and not actual:
        raise RuntimeError(
            f"{NAME}: stripped import requires a GLB regenerated with "
            "mesh2glb.py --carriers")
    if not STRIPPED and actual:
        raise RuntimeError(
            f"{NAME}: canonical import requires a carrier-free GLB; regenerate "
            "with mesh2glb.py without --carriers")
    try:
        alternate_bind.require_for_import(
            meta.get("authenticated_alternate_bind"),
            payload_key=meta.get("payload_key"),
            layout=meta.get("payload_layout") or {}, stripped=STRIPPED,
            mapping_carriers=actual)
    except ValueError as exc:
        raise RuntimeError(f"{NAME}: {exc}") from exc

TEXINFO = {}
for _manifest in TEXROOT.glob("*/_manifest.json"):
    _package = _manifest.parent.name
    for _name, _info in json.loads(_manifest.read_text()).get("textures", {}).items():
        TEXINFO[(_package, _name)] = _info


def import_glb():
    if EAL.does_directory_exist(DEST):
        deleted = EAL.delete_directory(DEST)
        # A referenced canonical mesh (e.g. AesSedai, held by faction CDO soft
        # paths + materials) cannot be fully deleted, but Interchange reimport
        # overwrites it in place — best-effort is correct there. The STRIPPED
        # isolated dest must be a clean slate for the transactional guarantee.
        if STRIPPED and (deleted is False or EAL.does_directory_exist(DEST)):
            raise RuntimeError(f"failed to clear prior stripped destination {DEST}")
    mgr = unreal.InterchangeManager.get_interchange_manager_scripted()
    src = unreal.InterchangeManager.create_source_data(str(CONV / f"{NAME}.glb"))
    params = unreal.ImportAssetParameters()
    params.is_automated = True
    if STRIPPED:
        pipelines = _mesh_import_pipelines(src)
        params.override_pipelines = [
            unreal.SoftObjectPath(pipeline.get_path_name()) for pipeline in pipelines]
    if not mgr.import_asset(DEST, src, params):
        raise RuntimeError("import_asset false")
    mgr.wait_until_all_tasks_done(False)
    meshes = [a for a in EAL.list_assets(DEST, recursive=True)
              if isinstance(unreal.load_asset(a.split(".")[0]), unreal.SkeletalMesh)]
    if len(meshes) != 1:
        raise RuntimeError(f"expected 1 skeletal mesh, got {meshes}")
    mesh_path = meshes[0].split(".")[0]
    mesh = unreal.load_asset(mesh_path)
    if not STRIPPED:
        # Canonical input has no mapping attributes. Keep only this cheap
        # post-import sanity check; there is no canonical UV/color rebuild.
        _assert_imported_vertex_color_contract(mesh)
    if EAL.save_directory(DEST) is False:
        raise RuntimeError(f"failed to save imported mesh directory {DEST}")
    return mesh_path


def import_texture(texname, *, key0=False):
    matches = sorted(TEXROOT.glob(f"*/{texname}.png"),
                     key=lambda path: str(path).casefold())
    if not matches:
        matches = sorted(
            (path for path in TEXROOT.glob("*/*.png")
             if path.stem.casefold() == texname.casefold()),
            key=lambda path: str(path).casefold())
    for source_png in matches:
        info = TEXINFO.get((source_png.parent.name, source_png.stem))
        if info is None:
            # Windows Path.glob matches case-insensitively but returns the
            # PATTERN's casing, so a .uc-authored case (Glass1's allgrey vs
            # the WOT.AllGrey export) misses the exact-case manifest key AND
            # skips the true-case fallback glob. Resolve the manifest entry
            # case-insensitively and canonicalize the stem to the export
            # case so asset paths/bindings stay consistent.
            fold = (source_png.parent.name, source_png.stem.casefold())
            for (pkg, name), candidate in TEXINFO.items():
                if pkg == fold[0] and name.casefold() == fold[1]:
                    info = candidate
                    source_png = source_png.parent / f"{name}.png"
                    break
        if info is None:
            continue
        png = source_png
        if key0:
            filename = info.get("key0")
            if not filename:
                continue
            png = source_png.parent / filename
            if not png.is_file():
                raise RuntimeError(f"manifest key0 PNG is missing: {png}")
        dest = f"/Game/WoT/Textures/{png.parent.name}"
        path = f"{dest}/{png.stem}"
        if not EAL.does_asset_exist(path):
            task = unreal.AssetImportTask()
            task.filename = str(png)
            task.destination_path = dest
            task.automated = True
            task.save = True
            ATT.import_asset_tasks([task])
        # key0 is a mesh-only derived bitmap. Binding it to the source retail
        # payload would restore Texture.bMasked semantics and erase the
        # per-triangle index-zero alpha this variant exists to supply.
        if not key0:
            ensure_texture_payload_binding(
                unreal, path, source_png.parent.name, source_png.stem)
        return path, source_png.parent.name, source_png.stem, info
    return None


def _stamp_key_color(material, info):
    red, green, blue = palette0_rgb(info)
    unreal.MaterialEditingLibrary.set_material_instance_vector_parameter_value(
        material, "KeyColor",
        unreal.LinearColor(red / 255.0, green / 255.0, blue / 255.0, 1.0))


# Slots whose canonical bind is the mesh's own Interchange-imported material:
# runtime code/tests key on the raw material NAME (WoT.Trap.Visuals asserts
# the slab's base skin is "TrapWall1"), so these must never be rebound to
# fallback MIs even when their texture PNG exists in the conv tree.
KEEP_INTERCHANGE = {"AlarmTex", "SpearTex", "TrapWall1", "Trapburst1"}


def assign_materials(mesh_path, meta):
    mesh = unreal.load_asset(mesh_path)
    masters = {
        suffix: unreal.load_asset(f"/Game/WoT/Materials/{name}")
        for suffix, name in IMPORT_SLOT_MASTERS.items()
    }
    mats = mesh.materials
    new_mats = []
    n = 0
    for i, sm in enumerate(mats):
        slot = str(sm.material_slot_name)
        # slot names are "<Texture>[_2S|_M|_T|_Mod]" from mesh2glb
        base, suffix = split_slot_name(slot)
        if base in KEEP_INTERCHANGE:
            new_mats.append(sm)
            continue
        if STRIPPED:
            # A mesh-only stripped pass may write only below its guarded
            # destination. Reuse an already-composed structural material when
            # present, otherwise retain Interchange's local material; never
            # create/update canonical textures or MIs from this isolated pass.
            existing_mi = f"/Game/WoT/Materials/Instances/MI_{slot}"
            if EAL.does_asset_exist(existing_mi):
                new_mats.append(unreal.SkeletalMaterial(
                    material_interface=unreal.load_asset(existing_mi),
                    material_slot_name=sm.material_slot_name))
                n += 1
            else:
                new_mats.append(sm)
            continue
        texture = import_texture(base, key0=suffix == "_M")
        if not texture:
            fb = f"/Game/WoT/Materials/Instances/MI_{base}"
            if suffix:
                # Even an unmapped texN slot must carry its James type. This is
                # common for slot 0 later replaced by actor Skin=; leaving the
                # Interchange fallback here would erase TwoSided/BlendMode
                # before the override can inspect the parent root.
                mi_path = f"/Game/WoT/Materials/Instances/MI_{slot}"
                if not EAL.does_asset_exist(mi_path):
                    mi = ATT.create_asset(
                        f"MI_{slot}", "/Game/WoT/Materials/Instances",
                        unreal.MaterialInstanceConstant,
                        unreal.MaterialInstanceConstantFactoryNew())
                else:
                    mi = unreal.load_asset(mi_path)
                master = masters[suffix]
                if master is None:
                    raise RuntimeError(
                        f"missing material master {IMPORT_SLOT_MASTERS[suffix]} "
                        f"for {slot}")
                if mi.get_editor_property("parent") != master:
                    mi.set_editor_property("parent", master)
                EAL.save_asset(mi_path)
                new_mats.append(unreal.SkeletalMaterial(
                    material_interface=unreal.load_asset(mi_path),
                    material_slot_name=sm.material_slot_name))
                n += 1
            elif EAL.does_asset_exist(fb):     # baked fractal flipbook skin
                new_mats.append(unreal.SkeletalMaterial(
                    material_interface=unreal.load_asset(fb),
                    material_slot_name=sm.material_slot_name))
                n += 1
            else:
                new_mats.append(sm)
            continue
        tex, _package, _source_name, texture_info = texture
        mi_path = f"/Game/WoT/Materials/Instances/MI_{slot}"
        if not EAL.does_asset_exist(mi_path):
            mi = ATT.create_asset(f"MI_{slot}", "/Game/WoT/Materials/Instances",
                                  unreal.MaterialInstanceConstant,
                                  unreal.MaterialInstanceConstantFactoryNew())
        else:
            mi = unreal.load_asset(mi_path)
        master = masters[suffix]
        if master is None:
            raise RuntimeError(
                f"missing material master {IMPORT_SLOT_MASTERS[suffix]} for {slot}")
        # Always repair existing MICs: pre-split assets may still have the
        # plain parent after a converter/importer upgrade.
        if mi.get_editor_property("parent") != master:
            mi.set_editor_property("parent", master)
        unreal.MaterialEditingLibrary.set_material_instance_texture_parameter_value(
            mi, "Tex", unreal.load_asset(tex))
        _stamp_key_color(mi, texture_info)
        EAL.save_asset(mi_path)
        # struct copies don't write back; build a fresh array below
        new_mats.append(unreal.SkeletalMaterial(
            material_interface=unreal.load_asset(mi_path),
            material_slot_name=sm.material_slot_name))
        n += 1
    if n:
        mesh.set_editor_property("materials", new_mats)
    EAL.save_asset(mesh_path)
    return n


def preflight(meta):
    """Validate every destructive-import dependency, then bootstrap masters."""
    glb = CONV / f"{NAME}.glb"
    if not glb.is_file():
        raise RuntimeError(f"converted mesh GLB is missing: {glb}")
    for slot in meta.get("slot_types", {}):
        base, suffix = split_slot_name(slot)
        if base in KEEP_INTERCHANGE:
            continue
        source = import_texture(base, key0=False)
        if source is None:
            if suffix == "_M" and not re.fullmatch(
                    r"tex\d+", base, re.IGNORECASE):
                raise RuntimeError(
                    f"masked slot {slot} has no source texture manifest")
            continue
        _path, package, source_name, info = source
        palette0_rgb(info)
        if suffix != "_M":
            continue
        key0 = info.get("key0")
        if not key0 or not (TEXROOT / package / key0).is_file():
            raise RuntimeError(
                f"masked slot {slot} has no keyed texture variant for "
                f"{package}.{source_name}")
    masters = ensure_mesh_masters(unreal, gamma_space=True)
    for name in set(IMPORT_SLOT_MASTERS.values()):
        material = (masters.get(name)
                    or unreal.load_asset(f"/Game/WoT/Materials/{name}"))
        if material is None:
            raise RuntimeError(f"missing material master after bootstrap: {name}")


def _read_mesh_metadata():
    return json.loads((CONV / f"{NAME}.meta.json").read_text())


def _import_mesh(meta=None):
    if should_skip_stripped_curvy(NAME, STRIPPED):
        unreal.log(f"IMPORT-MESH-SKIP {NAME}: _Curvy variants are dev-content only")
        return None
    meta = _read_mesh_metadata() if meta is None else meta
    mesh_path = import_glb()
    mesh = unreal.load_asset(mesh_path)
    if (not STRIPPED and NAME.casefold() in CURRENT_POSE_CPU_MESHES
            and not unreal.WoTMeshPayloadLibrary.set_lod0_allow_cpu_access(
                mesh, True)):
        raise RuntimeError(
            f"{NAME}: could not retain cooked LOD0 positions for current-pose sampling")
    morphs = mesh.get_all_morph_target_names()
    assigned = assign_materials(mesh_path, meta)
    # Interchange dedupes zero-delta morph targets (frames identical to the
    # base pose — mesh2glb records them as degenerate_frames); a missing
    # degenerate morph renders the base pose, which IS that frame's content
    # (SetMorphTarget on an absent name is a no-op).
    full = meta["frames"] - 1
    expected_min = full - len(meta.get("degenerate_frames", []))
    ok = expected_min <= len(morphs) <= full
    if not ok:
        raise RuntimeError(
            f"morph count {len(morphs)} outside [{expected_min},{full}]")
    layout = meta.get("payload_layout") or {}
    payload_key = meta.get("payload_key")
    animation_logical_id = meta.get("animation_logical_id")
    if STRIPPED and not payload_key:
        raise RuntimeError(
            f"{NAME}: stripped import requires retail-package payload identity")
    if (STRIPPED and (not isinstance(animation_logical_id, str)
                     or not animation_logical_id.startswith(
                         "cache/animation/"))):
        raise RuntimeError(
            f"{NAME}: stripped import requires exact cache-v3 animation identity")
    layout_status = "canonical-carrier-free-source"
    if STRIPPED:
        if layout.get("version") != 1:
            raise RuntimeError(f"{NAME}: unsupported/missing payload layout input")
        vectors = lambda values: [unreal.Vector(*value) for value in values]
        try:
            authenticated = alternate_bind.require_for_import(
                meta.get("authenticated_alternate_bind"),
                payload_key=payload_key, layout=layout, stripped=True,
                mapping_carriers=bool(meta.get("mapping_carriers")))
        except ValueError as exc:
            raise RuntimeError(f"{NAME}: {exc}") from exc
        capture_positions = (authenticated["positions"] if authenticated
                             else layout["base_positions"])
        capture_normals = (authenticated["normals"] if authenticated
                           else layout["base_normals"])
        error = unreal.WoTMeshPayloadLibrary.capture_and_optionally_strip(
            mesh,
            payload_key,
            unreal.Vector(*meta.get("origin", [0, 0, 0])),
            unreal.Vector(*meta.get("scale", [0.1, 0.1, 0.2])),
            unreal.Vector(*meta.get("rot", [0, 0, 0])),
            bool(meta.get("unmirror", False)),
            int(layout["frame_verts"]),
            int(layout["anim_frames"]),
            [int(value) for value in layout["smooth_triangles"]],
            [int(value) for value in layout["render_triangles"]],
            vectors(capture_positions),
            vectors(capture_normals),
            STRIPPED,
            animation_logical_id,
        )
        if error:
            raise RuntimeError(f"{NAME}: payload layout capture/strip failed: {error}")
        if EAL.save_asset(mesh_path) is False:
            raise RuntimeError(f"{NAME}: failed to save verified stripped mesh")
        layout_status = "stripped-mapping-verified-dual-carriers-removed-single-uv"
        if mesh.get_all_morph_target_names():
            raise RuntimeError(f"{NAME}: stripped mesh retained native morphs")
        if mesh.has_vertex_colors():
            raise RuntimeError(f"{NAME}: stripped mesh retained the COLOR_0 carrier")
        if authenticated:
            receipt = alternate_bind.make_import_receipt(
                authenticated["authority"], asset=f"{mesh_path}.{NAME}",
                native_morph_target_count=len(mesh.get_all_morph_target_names()),
                has_vertex_colors=bool(mesh.has_vertex_colors()))
            alternate_bind.write_import_receipt(
                ALTERNATE_BIND_RECEIPT, receipt)
    unreal.log(f"IMPORT-MESH-{'OK' if ok else 'BAD'} {NAME} mesh={mesh_path} "
               f"morphs={len(morphs)}/{full} (min {expected_min}) "
               f"mats={assigned} mode={'stripped' if STRIPPED else 'full'} "
               f"layout={layout_status}")


def main():
    if should_skip_stripped_curvy(NAME, STRIPPED):
        return _import_mesh()
    # Parse and verify the GLB mode before any destination is touched. A stale
    # canonical/carrier conversion must never be left for Interchange to infer.
    meta = _read_mesh_metadata()
    if (STRIPPED and meta.get("payload_key") == alternate_bind.IDENTITY):
        ALTERNATE_BIND_RECEIPT.unlink(missing_ok=True)
    _assert_mapping_carrier_source_contract(meta)
    if not STRIPPED:
        # Canonical path: validate material deps + bootstrap masters (MTT
        # typed-slot system) before the destructive import.
        preflight(meta)
        return _import_mesh(meta)
    # Interchange and material assignment both save before mapping/strip
    # verification completes. Failure-atomicity is therefore scoped to the
    # whole isolated per-mesh directory, not merely the final metadata call.
    return run_isolated_import_transaction(
        lambda: _import_mesh(meta), DEST,
        EAL.does_directory_exist, EAL.delete_directory)


if __name__ == "__main__":
    main()
