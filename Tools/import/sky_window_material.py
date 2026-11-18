"""Generate the opaque live child-sky window master.

The runtime supplies one raw display-space cube target for each active sky
plan.  A world-space FakeBackdrop sheet samples that cube by the normalized
pixel-to-camera direction and writes the sampled value unchanged to Emissive.
Opaque + two-sided makes both faces solid and depth-writing, matching UE1's
PF_FakeBackdrop occlusion contract.

Run inside Unreal's Python commandlet with the closed destination::

    UnrealEditor-Cmd.exe WoT.uproject -run=pythonscript \
      -script="Tools/import/sky_window_material.py --output-root /Game/WoT/Materials"

Import-safe: ``unreal`` is imported only by the command-line entry point.
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
from typing import Any

MATERIAL_ROOT = "/Game/WoT/Materials"
MATERIAL_NAME = "M_WoT_SkyWindow"
MATERIAL_PATH = f"{MATERIAL_ROOT}/{MATERIAL_NAME}"
CUBE_PARAMETER = "SkyCube"
DEFAULT_CUBE = "/Engine/EngineResources/DefaultTextureCube"
GRAPH_TAG = "WoT.SkyWindowGraph"
GRAPH_VERSION = "sky-window-v1"
RECEIPT_FORMAT = "wot-sky-window-material-receipt-v1"


def view_direction(world_position, camera_position):
    """Python twin of normalize(AbsoluteWorldPosition - CameraPositionWS)."""
    delta = tuple(w - c for w, c in zip(world_position, camera_position))
    length = math.sqrt(sum(value * value for value in delta))
    if length == 0.0:
        return (0.0, 0.0, 0.0)
    return tuple(value / length for value in delta)


def transport_color(sample):
    """The unlit emissive transport deliberately performs no colour math."""
    return tuple(sample)


def graph_contract() -> dict[str, Any]:
    return {
        "material": MATERIAL_NAME,
        "blend": "opaque",
        "shading_model": "unlit",
        "two_sided": True,
        "cube_parameter": CUBE_PARAMETER,
        "default_cube": DEFAULT_CUBE,
        "direction": "normalize(AbsoluteWorldPosition-CameraPositionWS)",
        "transport": "identity-emissive",
        "fog": "none-authored",
    }


def validate_output_root(output_root: str) -> str:
    if output_root != MATERIAL_ROOT:
        raise ValueError(
            f"output root must be the closed material root {MATERIAL_ROOT}")
    return output_root


def build(unreal: Any) -> Any:
    eal = unreal.EditorAssetLibrary
    mel = unreal.MaterialEditingLibrary
    default_cube = unreal.load_asset(DEFAULT_CUBE)
    if default_cube is None:
        raise RuntimeError(f"engine default cube is missing: {DEFAULT_CUBE}")
    if eal.does_asset_exist(MATERIAL_PATH) and not eal.delete_asset(MATERIAL_PATH):
        raise RuntimeError(f"could not replace stale {MATERIAL_PATH}")
    material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        MATERIAL_NAME, MATERIAL_ROOT, unreal.Material,
        unreal.MaterialFactoryNew())
    if material is None:
        raise RuntimeError(f"could not create {MATERIAL_PATH}")
    material.set_editor_property(
        "shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_OPAQUE)
    material.set_editor_property("two_sided", True)

    def expr(cls, x, y):
        return mel.create_material_expression(material, cls, x, y)

    world = expr(unreal.MaterialExpressionWorldPosition, -800, -120)
    camera = expr(unreal.MaterialExpressionCameraPositionWS, -800, 40)
    delta = expr(unreal.MaterialExpressionSubtract, -600, -40)
    mel.connect_material_expressions(world, "", delta, "A")
    mel.connect_material_expressions(camera, "", delta, "B")
    direction = expr(unreal.MaterialExpressionNormalize, -400, -40)
    mel.connect_material_expressions(delta, "", direction, "VectorInput")
    cube = expr(unreal.MaterialExpressionTextureSampleParameterCube, -180, -40)
    cube.set_editor_property("parameter_name", CUBE_PARAMETER)
    cube.set_editor_property("texture", default_cube)
    cube.set_editor_property(
        # The engine default cube is typed Color, while the runtime target's
        # bForceLinearGamma disables the actual sRGB decode at sample time.
        "sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_COLOR)
    mel.connect_material_expressions(direction, "", cube, "UVs")
    mel.connect_material_property(
        cube, "RGB", unreal.MaterialProperty.MP_EMISSIVE_COLOR)

    eal.set_metadata_tag(material, GRAPH_TAG, GRAPH_VERSION)
    errors = list(mel.recompile_material(material))
    if errors:
        raise RuntimeError(f"{MATERIAL_PATH}: shader compile failed: "
                           + "; ".join(str(error) for error in errors))
    if not eal.save_asset(MATERIAL_PATH):
        raise RuntimeError(f"could not save {MATERIAL_PATH}")
    return material


def validate(unreal: Any, material: Any) -> dict[str, Any]:
    mel = unreal.MaterialEditingLibrary
    if material.get_editor_property("blend_mode") != unreal.BlendMode.BLEND_OPAQUE:
        raise RuntimeError(f"{MATERIAL_PATH}: blend mode is not opaque")
    if material.get_editor_property(
            "shading_model") != unreal.MaterialShadingModel.MSM_UNLIT:
        raise RuntimeError(f"{MATERIAL_PATH}: shading model is not unlit")
    if not bool(material.get_editor_property("two_sided")):
        raise RuntimeError(f"{MATERIAL_PATH}: master must be two-sided")
    cube_parameters = []
    for node in mel.get_material_expressions(material):
        if str(node.get_class().get_name()) \
                != "MaterialExpressionTextureSampleParameterCube":
            continue
        cube_parameters.append(str(node.get_editor_property("parameter_name")))
        if node.get_editor_property(
                "sampler_type") != unreal.MaterialSamplerType.SAMPLERTYPE_COLOR:
            raise RuntimeError(f"{MATERIAL_PATH}: cube parameter type is not Color")
    if cube_parameters != [CUBE_PARAMETER]:
        raise RuntimeError(
            f"{MATERIAL_PATH}: cube parameters are {cube_parameters!r}")
    return {"format": RECEIPT_FORMAT, "graph_version": GRAPH_VERSION,
            "package": MATERIAL_PATH, "parameters": cube_parameters}


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
        material = unreal.load_asset(MATERIAL_PATH)
        if material is None:
            raise RuntimeError(f"{MATERIAL_PATH} is absent")
    else:
        material = build(unreal)
    receipt = validate(unreal, material)
    result = Path(unreal.Paths.project_saved_dir()) \
        / "sky_window_material_result.json"
    result.write_text(json.dumps(receipt, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
