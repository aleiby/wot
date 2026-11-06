"""Generate the two stackable child-sky translucent masters.

Retail Glide draws a lightmapped translucent surface in two passes: the
texture blends ONE/ONE_MINUS_SRC_COLOR, then the light-map pass modulates the
whole pixel, so FB = M*(Tex + Dst*(1-Tex)) per channel (WoTRuntimeMapSky.h).
M_WoT_EntryTranslucent_SceneBlend computes that from a SceneColor read, but
SceneColor is the scene before translucency, so two overlapping sky layers
cannot stack (Arena_02's sepia layer vanished behind its cloud sheet). The
same algebra split into two fixed-function passes does stack in draw order:

    pass A, BLEND_Modulate: Dst' = Dst * M*(1-Tex)
    pass B, BLEND_Additive: Dst'' = Dst' + Tex*M

Both masters are duplicated from M_WoT_EntryTranslucent_SceneBlend (the
Entry-family copy: WoTSceneTime pan, linear neutral samplers, the G45 light
factor and DynLightEnable=0), then only the output is rewired: the SceneColor
term is deleted and Emissive takes M*(1-Tex) or Tex*M. Parameter names are
unchanged, so a world-slot instance's values copy across verbatim.

Run inside Unreal's Python commandlet with the closed destination, after the
Entry-family step (entry_weather_materials.py)::

    UnrealEditor-Cmd.exe WoT.uproject -run=pythonscript \
      -script="Tools/import/sky_layer_materials.py --output-root /Game/WoT/Materials"

Import-safe: ``unreal`` is imported only by the command-line entry point.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

MATERIAL_ROOT = "/Game/WoT/Materials"
SOURCE_NAME = "M_WoT_EntryTranslucent_SceneBlend"
MODULATE_NAME = "M_WoT_EntrySkyModulate"
ADD_NAME = "M_WoT_EntrySkyAdd"
GRAPH_TAG = "WoT.SkyLayerGraph"
GRAPH_VERSION = "sky-layer-v1"
RECEIPT_FORMAT = "wot-sky-layer-materials-receipt-v1"
PARAMETERS = ("Tex", "Lightmap", "PanU", "PanV", "WoTSceneTime",
              "DynLightEnable")


def scene_blend(dst, tex, m):
    """The per-channel retail result the SceneBlend master computed."""
    return tuple(t * l + d * l * (1.0 - t) for d, t, l in zip(dst, tex, m))


def two_pass(dst, tex, m):
    """Pass A (modulate by M*(1-Tex)) then pass B (add Tex*M)."""
    modulated = tuple(d * l * (1.0 - t) for d, t, l in zip(dst, tex, m))
    return tuple(value + t * l for value, t, l in zip(modulated, tex, m))


def material_specs() -> tuple[tuple[str, str, str], ...]:
    """(name, blend, emissive term) for each generated master."""
    return ((MODULATE_NAME, "modulate", "M*(1-Tex)"),
            (ADD_NAME, "additive", "Tex*M"))


def validate_output_root(output_root: str) -> str:
    if output_root != MATERIAL_ROOT:
        raise ValueError(
            f"output root must be the closed material root {MATERIAL_ROOT}")
    return output_root


def _class(node: Any) -> str:
    return str(node.get_class().get_name()) if node is not None else ""


def _key(node: Any) -> str:
    return str(node.get_path_name()) if node is not None else ""


def _inputs(mel: Any, material: Any, node: Any) -> dict[str, Any]:
    names = [str(name) for name in
             mel.get_material_expression_input_names(node)]
    values = list(mel.get_inputs_for_material_expression(material, node))
    if len(names) != len(values):
        raise RuntimeError("material input reflection mismatch")
    return dict(zip(names, values))


def _rewire(unreal: Any, material: Any, path: str, blend: str) -> None:
    """Delete the SceneColor term and route Emissive to one pass's factor."""
    mel = unreal.MaterialEditingLibrary
    nodes = list(mel.get_material_expressions(material))
    scene_colors = [node for node in nodes
                    if _class(node) == "MaterialExpressionSceneColor"]
    if len(scene_colors) != 1:
        raise RuntimeError(f"{path}: expected one SceneColor, found {len(scene_colors)}")
    scene_color = scene_colors[0]
    # dst = SceneColor * fdst, out = prod + dst (setup_childsky.scene_blend_master).
    dst = out = fdst = prod = None
    for node in nodes:
        if _class(node) != "MaterialExpressionMultiply":
            continue
        pins = _inputs(mel, material, node)
        if _key(pins.get("A")) == _key(scene_color):
            dst, fdst = node, pins.get("B")
    for node in nodes:
        if _class(node) != "MaterialExpressionAdd" or dst is None:
            continue
        pins = _inputs(mel, material, node)
        if _key(pins.get("B")) == _key(dst):
            out, prod = node, pins.get("A")
    if None in (dst, out, fdst, prod):
        raise RuntimeError(f"{path}: SceneBlend output chain not found")
    keep = fdst if blend == "modulate" else prod
    if not mel.connect_material_property(
            keep, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR):
        raise RuntimeError(f"{path}: could not rewire Emissive")
    for node in (out, dst, scene_color):
        mel.delete_material_expression(material, node)
    material.set_editor_property(
        "blend_mode", unreal.BlendMode.BLEND_MODULATE if blend == "modulate"
        else unreal.BlendMode.BLEND_ADDITIVE)


def build(unreal: Any) -> list[Any]:
    eal = unreal.EditorAssetLibrary
    mel = unreal.MaterialEditingLibrary
    source_path = f"{MATERIAL_ROOT}/{SOURCE_NAME}"
    source = unreal.load_asset(source_path) \
        if eal.does_asset_exist(source_path) else None
    if source is None:
        raise RuntimeError(
            f"{source_path} is absent; run entry_weather_materials.py first")
    built = []
    for name, blend, _ in material_specs():
        path = f"{MATERIAL_ROOT}/{name}"
        if eal.does_asset_exist(path) and not eal.delete_asset(path):
            raise RuntimeError(f"could not replace stale {path}")
        material = unreal.AssetToolsHelpers.get_asset_tools().duplicate_asset(
            name, MATERIAL_ROOT, source)
        if material is None:
            raise RuntimeError(f"could not create {path}")
        _rewire(unreal, material, path, blend)
        eal.set_metadata_tag(material, GRAPH_TAG, GRAPH_VERSION)
        errors = list(mel.recompile_material(material))
        if errors:
            raise RuntimeError(f"{path}: shader compile failed: "
                               + "; ".join(str(error) for error in errors))
        if not eal.save_asset(path):
            raise RuntimeError(f"could not save {path}")
        built.append(material)
    return built


def validate(unreal: Any, materials: list[Any]) -> dict[str, Any]:
    eal = unreal.EditorAssetLibrary
    mel = unreal.MaterialEditingLibrary
    rows = []
    for (name, blend, term), material in zip(material_specs(), materials):
        path = f"{MATERIAL_ROOT}/{name}"
        expected = (unreal.BlendMode.BLEND_MODULATE if blend == "modulate"
                    else unreal.BlendMode.BLEND_ADDITIVE)
        if material.get_editor_property("blend_mode") != expected:
            raise RuntimeError(f"{path}: blend mode is not {blend}")
        if material.get_editor_property(
                "shading_model") != unreal.MaterialShadingModel.MSM_UNLIT:
            raise RuntimeError(f"{path}: shading model is not unlit")
        if not bool(material.get_editor_property("two_sided")):
            raise RuntimeError(f"{path}: master must be two-sided")
        if str(eal.get_metadata_tag(material, GRAPH_TAG)) != GRAPH_VERSION:
            raise RuntimeError(f"{path}: graph version tag is missing")
        nodes = list(mel.get_material_expressions(material))
        if any(_class(node) in ("MaterialExpressionSceneColor",
                                "MaterialExpressionTime") for node in nodes):
            raise RuntimeError(f"{path}: SceneColor or global Time survived")
        names = sorted({str(node.get_editor_property("parameter_name"))
                        for node in nodes
                        if _class(node) in (
                            "MaterialExpressionScalarParameter",
                            "MaterialExpressionTextureSampleParameter2D")})
        missing = [parameter for parameter in PARAMETERS if parameter not in names]
        if missing:
            raise RuntimeError(f"{path}: missing parameters {missing}")
        rows.append({"package": path, "blend": blend, "emissive": term,
                     "parameters": names})
    return {"format": RECEIPT_FORMAT, "graph_version": GRAPH_VERSION,
            "source": f"{MATERIAL_ROOT}/{SOURCE_NAME}", "materials": rows}


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-root", required=True)
    parser.add_argument("--validate-only", action="store_true")
    return parser


def main() -> None:
    import unreal  # type: ignore

    args = _parser().parse_args()
    validate_output_root(str(args.output_root))
    if args.validate_only:
        materials = [unreal.load_asset(f"{MATERIAL_ROOT}/{name}")
                     for name, _, _ in material_specs()]
        if any(material is None for material in materials):
            raise RuntimeError("a sky layer master is absent")
    else:
        materials = build(unreal)
    receipt = validate(unreal, materials)
    result = Path(unreal.Paths.project_saved_dir()) \
        / "sky_layer_materials_result.json"
    result.write_text(json.dumps(receipt, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
