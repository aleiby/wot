"""Generate the nine entry-only world material masters.

Run inside Unreal's Python commandlet with an explicit, closed destination:

    UnrealEditor-Cmd.exe WoT.uproject -run=pythonscript \
      -script=".../entry_weather_materials.py --output-root /Game/WoT/Materials"

The module stays import-safe for the pure graph-contract tests; ``unreal`` is
loaded only by the command-line entry point.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from typing import Any


MATERIAL_ROOT = "/Game/WoT/Materials"
NEUTRAL_TEXTURE = "/Game/WoT/Textures/WOTDetail/T_WoT_Neutral"
ENTRY_GRAPH_TAG = "WoT.EntryWeatherGraph"
# v4: opaque/masked clones carry the exact MacroOffsetU/V transform added to
# the shared world masters. v5: they carry the per-axis detail ratio
# (DetailRatioV). v6: the source masters' G45 term gained LE_Shell
# (g45-v3-shell). A source-master regeneration must bump this so the clones
# are rebuilt rather than left "current" on the old graph.
ENTRY_GRAPH_VERSION = "entry-weather-v6"
RECEIPT_FORMAT = "wot-entry-weather-materials-receipt-v1"
SCENE_TIME_PARAMETER = "WoTSceneTime"
DYNAMIC_LIGHT_PARAMETER = "DynLightEnable"

_SOURCE_MASTER_SHA256 = {
    "M_WoT_World":
        "c065f242f77fc79c5dcb79361f9e6cb54bb24d50e0e7b36ab248451a506294a7",
    "M_WoT_WorldMasked":
        "dc818bb3d00f7b14d7af8ebfedb49c8eee50268222d71ef1de8a249d447f8757",
    "M_WoT_WorldTranslucent":
        "9520fa9116e4286de539b88779476a1f65af6bee9c7cb05e3c033e31416efc2f",
    "M_WoT_WorldModulated":
        "da734a325a0c7a925ecf4eb5c818a0ded41417e1ab8cdf35811831862686f508",
    # setup_childsky.scene_blend_master: retail's per-channel translucent
    # dst factor M*(1-Tex) (the single-TMU Glide driver path)
    # from an explicit SceneColor read; guarded sky-box sheets bind the copy.
    "M_WoT_WorldT_SceneBlend":
        "00241fdd485b5f146037ca8eebcb1b26ea2ae5d0bac9937dc8c8730d6147cd1b",
}

_EDITOR_ASSERTIONS = (
    "source_sha256", "metadata", "blend_source_parity",
    "shading_source_parity",
    "two_sided", "pan_uv_connections", "no_global_time",
    "neutral_texture_defaults", "linear_samplers",
    "lightmap_uv1_times_two", "dynamic_light_default_zero",
    "expression_inventory_parity", "material_outputs", "shader_compile",
)


@dataclass(frozen=True)
class MaterialSpec:
    name: str
    source: str
    blend: str
    two_sided: bool


_SPECS = (
    MaterialSpec("M_WoT_EntryWorld", "M_WoT_World", "opaque", False),
    MaterialSpec("M_WoT_EntryWorld_TwoSided", "M_WoT_World", "opaque", True),
    MaterialSpec("M_WoT_EntryMasked", "M_WoT_WorldMasked", "masked", False),
    MaterialSpec("M_WoT_EntryMasked_TwoSided", "M_WoT_WorldMasked", "masked", True),
    MaterialSpec(
        "M_WoT_EntryTranslucent", "M_WoT_WorldTranslucent",
        "alpha_composite", False),
    MaterialSpec(
        "M_WoT_EntryTranslucent_TwoSided", "M_WoT_WorldTranslucent",
        "alpha_composite", True),
    MaterialSpec(
        "M_WoT_EntryModulated", "M_WoT_WorldModulated", "modulate", False),
    MaterialSpec(
        "M_WoT_EntryModulated_TwoSided", "M_WoT_WorldModulated",
        "modulate", True),
    MaterialSpec(
        "M_WoT_EntryTranslucent_SceneBlend", "M_WoT_WorldT_SceneBlend",
        "translucent", True),
)


def material_specs() -> tuple[MaterialSpec, ...]:
    return _SPECS


def source_master_sha256s() -> dict[str, str]:
    return dict(_SOURCE_MASTER_SHA256)


def required_editor_assertions() -> tuple[str, ...]:
    return _EDITOR_ASSERTIONS


def pan_uv(u: float, v: float, pan_u: float, pan_v: float,
           scene_time: float) -> tuple[float, float]:
    return u + pan_u * scene_time, v + pan_v * scene_time


def graph_contract(spec: MaterialSpec) -> dict[str, Any]:
    return {
        "material": spec.name,
        "source": spec.source,
        "blend": spec.blend,
        "two_sided": spec.two_sided,
        "pan_time_parameter": SCENE_TIME_PARAMETER,
        "expression_types": (
            "TextureCoordinate", "ScalarParameter", "AppendVector",
            "Multiply", "Add", "TextureSampleParameter2D"),
        "lightmap_multiplier": 2.0,
        "dynamic_light_default": 0.0,
        "macro_offset_parameters": ("MacroOffsetU", "MacroOffsetV"),
        "lightmap_uv_channel": 1,
        "unlit_uses_lightmap_uv1_fullbright": True,
        "source_graph_policy": "duplicate-and-targeted-rewire",
        "allowed_graph_mutations": (
            "Time->WoTSceneTime", "remove-unreachable-Time-orphans",
            "DynLightEnable=0", "two_sided"),
        "texture_defaults": (NEUTRAL_TEXTURE, NEUTRAL_TEXTURE),
        "sampler_types": ("linear_color", "linear_color"),
    }


def validate_output_root(output_root: str) -> str:
    if output_root != MATERIAL_ROOT:
        raise ValueError(
            f"output root must be the closed entry material root {MATERIAL_ROOT}")
    return output_root


def validate_default_references(references) -> tuple[str, ...]:
    values = tuple(references)
    if any(value != NEUTRAL_TEXTURE for value in values):
        raise ValueError("retail default references are forbidden")
    return values


def needs_rebuild(current_version: str | None) -> bool:
    return current_version != ENTRY_GRAPH_VERSION


def _rebuild_material(unreal: Any, output_root: str,
                      spec: MaterialSpec, source: Any, neutral: Any) -> Any:
    eal = unreal.EditorAssetLibrary
    mel = unreal.MaterialEditingLibrary
    target_path = f"{output_root}/{spec.name}"
    if eal.does_asset_exist(target_path):
        # Only generated entry targets are replaced. A fresh duplicate keeps
        # every source expression, output and material setting intact; in
        # particular, do not use delete_all_material_expressions here because
        # the pinned engine implementation mutates its collection mid-loop.
        if not eal.delete_asset(target_path):
            raise RuntimeError(f"could not replace stale entry material {target_path}")
    material = unreal.AssetToolsHelpers.get_asset_tools().duplicate_asset(
        spec.name, output_root, source)
    if material is None:
        raise RuntimeError(f"could not create entry material {target_path}")
    material.set_editor_property("two_sided", spec.two_sided)

    expressions = list(mel.get_material_expressions(material))
    # The pan chain is found from the LIVE Tex sampler: a source master may
    # also carry an unreachable orphan Tex (see _live_expressions).
    texture_nodes = [node for node in _live_expressions(unreal, material)
                     if str(node.get_class().get_name())
                     == "MaterialExpressionTextureSampleParameter2D"
                     and str(node.get_editor_property("parameter_name")) == "Tex"]
    if len(texture_nodes) != 1:
        raise RuntimeError(
            f"{target_path}: source clone has {len(texture_nodes)} live Tex parameters")
    texture = texture_nodes[0]

    def connected_inputs(node):
        return [value for value in
                mel.get_inputs_for_material_expression(material, node)
                if value is not None]

    ancestors = {}
    pending = [texture]
    while pending:
        node = pending.pop()
        key = str(node.get_path_name())
        if key in ancestors:
            continue
        ancestors[key] = node
        pending.extend(connected_inputs(node))
    pan_times = [node for node in ancestors.values()
                 if str(node.get_class().get_name()) == "MaterialExpressionTime"]
    all_times = [node for node in expressions
                 if str(node.get_class().get_name()) == "MaterialExpressionTime"]
    if len(pan_times) != 1 or not all_times:
        raise RuntimeError(
            f"{target_path}: source clone must have exactly one live pan Time node")
    old_time = pan_times[0]
    consumers_by_time = {str(node.get_path_name()): [] for node in all_times}
    for consumer in expressions:
        names = [str(value) for value in
                 mel.get_material_expression_input_names(consumer)]
        inputs = list(mel.get_inputs_for_material_expression(material, consumer))
        if len(names) != len(inputs):
            raise RuntimeError(f"{target_path}: input reflection mismatch")
        for name, value in zip(names, inputs):
            if value is not None and str(value.get_path_name()) in consumers_by_time:
                consumers_by_time[str(value.get_path_name())].append(
                    (consumer, name))
    consumers = consumers_by_time[str(old_time.get_path_name())]
    if len(consumers) != 1:
        raise RuntimeError(
            f"{target_path}: pan Time has {len(consumers)} consumers, expected one")
    used_non_pan_times = [node for node in all_times if node != old_time
                          and consumers_by_time[str(node.get_path_name())]]
    if used_non_pan_times:
        raise RuntimeError(
            f"{target_path}: a non-pan Time node is still graph-reachable")
    node_x, node_y = mel.get_material_expression_node_position(old_time)
    scene_time = mel.create_material_expression(
        material, unreal.MaterialExpressionScalarParameter, node_x, node_y)
    scene_time.set_editor_property("parameter_name", SCENE_TIME_PARAMETER)
    scene_time.set_editor_property("default_value", 0.0)
    consumer, input_name = consumers[0]
    if not mel.connect_material_expressions(
            scene_time, "", consumer, input_name):
        raise RuntimeError(f"{target_path}: could not rewire pan scene time")
    # The source packages contain one proven zero-consumer Time orphan from an
    # older bulk rebuild. Removing it changes no graph edge and lets the saved
    # entry copies enforce the stronger no-global-Time invariant.
    for time_node in all_times:
        mel.delete_material_expression(material, time_node)

    dynamic_nodes = [node for node in mel.get_material_expressions(material)
                     if str(node.get_class().get_name())
                     == "MaterialExpressionScalarParameter"
                     and str(node.get_editor_property("parameter_name"))
                     == DYNAMIC_LIGHT_PARAMETER]
    if len(dynamic_nodes) != 1:
        raise RuntimeError(
            f"{target_path}: source clone has {len(dynamic_nodes)} "
            f"{DYNAMIC_LIGHT_PARAMETER} parameters")
    dynamic_nodes[0].set_editor_property("default_value", 0.0)

    if any(str(node.get_class().get_name()) == "MaterialExpressionTime"
           for node in mel.get_material_expressions(material)):
        raise RuntimeError(f"{target_path}: global Time survived targeted rewire")

    eal.set_metadata_tag(material, ENTRY_GRAPH_TAG, ENTRY_GRAPH_VERSION)
    compile_errors = list(mel.recompile_material(material))
    if compile_errors:
        raise RuntimeError(
            f"{target_path}: shader compile failed: "
            + "; ".join(str(error) for error in compile_errors))
    if not eal.save_asset(target_path):
        raise RuntimeError(f"could not save entry material {target_path}")
    return material


def generate(args: argparse.Namespace, unreal: Any) -> dict[str, Any]:
    output_root = validate_output_root(str(args.output_root))
    validate_default_references(
        default for spec in material_specs()
        for default in graph_contract(spec)["texture_defaults"])
    eal = unreal.EditorAssetLibrary
    if not eal.does_asset_exist(NEUTRAL_TEXTURE):
        raise RuntimeError(f"approved neutral texture is missing: {NEUTRAL_TEXTURE}")
    neutral = unreal.load_asset(NEUTRAL_TEXTURE)
    if neutral is None:
        raise RuntimeError(f"approved neutral texture is missing: {NEUTRAL_TEXTURE}")

    # Preflight the complete table before replacing any stale target. A late
    # invalid source or supposedly-current target must not leave an earlier
    # entry material rebuilt from only a partially approved set.
    sources = {}
    planned = []
    for spec in material_specs():
        source_path = f"{MATERIAL_ROOT}/{spec.source}"
        target_path = f"{output_root}/{spec.name}"
        source = sources.get(spec.source)
        if source is None:
            if not eal.does_asset_exist(source_path):
                raise RuntimeError(
                    f"approved source master is missing: {source_path}")
            source = unreal.load_asset(source_path)
            if source is None:
                raise RuntimeError(
                    f"approved source master is missing: {source_path}")
            _validate_source_master_sha256(
                unreal, spec.source, source_path)
            _validate_neutral_texture_defaults(unreal, source)
            sources[spec.source] = source
        exists = bool(eal.does_asset_exist(target_path))
        target = unreal.load_asset(target_path) if exists else None
        version = str(eal.get_metadata_tag(target, ENTRY_GRAPH_TAG)) \
            if target is not None else None
        current = target is not None and not needs_rebuild(version)
        if current:
            _validate_neutral_texture_defaults(unreal, target)
        planned.append((spec, source, current))

    rows = []
    for spec, source, current in planned:
        target_path = f"{output_root}/{spec.name}"
        if current:
            action = "current"
        else:
            _rebuild_material(unreal, output_root, spec, source, neutral)
            action = "rebuilt"
        rows.append({"package": target_path, "action": action})
    return {
        "format": RECEIPT_FORMAT,
        "graph_version": ENTRY_GRAPH_VERSION,
        "output_root": output_root,
        "materials": rows,
    }


def _asset_path(asset: Any) -> str:
    return str(asset.get_path_name()).split(".", 1)[0] if asset else ""


def _expression_class(expression: Any) -> str:
    return str(expression.get_class().get_name()) if expression else ""


def _parameter_name(expression: Any) -> str:
    try:
        return str(expression.get_editor_property("parameter_name"))
    except Exception:
        return ""


def _validate_source_master_sha256(unreal: Any, source_name: str,
                                   source_path: str) -> str:
    content_root = Path(unreal.Paths.project_content_dir()).resolve()
    source_file = content_root / "WoT" / "Materials" / f"{source_name}.uasset"
    _require(source_file.is_file(), source_path, "source_sha256",
             f"source package file is absent: {source_file}")
    actual_hash = hashlib.sha256(source_file.read_bytes()).hexdigest()
    expected_hash = _SOURCE_MASTER_SHA256[source_name]
    _require(actual_hash == expected_hash, source_path, "source_sha256",
             f"expected {expected_hash}, found {actual_hash}")
    return actual_hash


def _validate_neutral_texture_defaults(unreal: Any, material: Any) -> None:
    expressions = list(
        unreal.MaterialEditingLibrary.get_material_expressions(material))
    references = (
        _asset_path(expression.get_editor_property("texture"))
        for expression in expressions
        if _expression_class(expression)
        == "MaterialExpressionTextureSampleParameter2D"
    )
    try:
        validate_default_references(references)
    except ValueError as error:
        raise ValueError(f"{_asset_path(material)}: {error}") from error


def _require(condition: bool, material_path: str, assertion: str,
             detail: str) -> None:
    if not condition:
        raise RuntimeError(
            f"{material_path}: {assertion} validation failed: {detail}")


def _find_parameter(expressions: list[Any], class_name: str,
                    parameter_name: str, material_path: str) -> Any:
    matches = [expression for expression in expressions
               if _expression_class(expression) == class_name
               and _parameter_name(expression) == parameter_name]
    _require(len(matches) == 1, material_path, "material_outputs",
             f"expected one {class_name} parameter {parameter_name}, "
             f"found {len(matches)}")
    return matches[0]


def _input_map(unreal: Any, material: Any, expression: Any) -> dict[str, Any]:
    mel = unreal.MaterialEditingLibrary
    names = [str(value) for value in
             mel.get_material_expression_input_names(expression)]
    inputs = list(mel.get_inputs_for_material_expression(material, expression))
    if len(names) != len(inputs):
        raise RuntimeError(
            f"{_asset_path(material)}: material expression input reflection "
            "returned mismatched names and nodes")
    return dict(zip(names, inputs))


def _connected_inputs(unreal: Any, material: Any,
                      expression: Any) -> list[Any]:
    return [value for value in _input_map(unreal, material, expression).values()
            if value is not None]


def _require_pin(unreal: Any, material: Any, expression: Any,
                 pin: str, expected: Any, assertion: str) -> None:
    material_path = _asset_path(material)
    inputs = _input_map(unreal, material, expression)
    _require(pin in inputs, material_path, assertion,
             f"{_expression_class(expression)} has no {pin} input")
    _require(inputs[pin] == expected, material_path, assertion,
             f"{_expression_class(expression)}.{pin} is not connected to "
             f"{_expression_class(expected)}")


def _ancestors(unreal: Any, material: Any, root: Any) -> list[Any]:
    found: dict[str, Any] = {}
    pending = [root] if root is not None else []
    while pending:
        expression = pending.pop()
        key = str(expression.get_path_name())
        if key in found:
            continue
        found[key] = expression
        pending.extend(_connected_inputs(unreal, material, expression))
    return list(found.values())


# Every material output a source master can drive; the live graph is what
# these reach.
_OUTPUT_PROPERTY_NAMES = (
    "MP_BASE_COLOR", "MP_EMISSIVE_COLOR", "MP_ROUGHNESS", "MP_SPECULAR",
    "MP_OPACITY", "MP_OPACITY_MASK",
)


def _live_expressions(unreal: Any, material: Any) -> list[Any]:
    """Every expression reachable from a material output.

    The committed source masters keep zero-consumer orphans from an older
    bulk rebuild (the pinned engine's delete_all_material_expressions mutates
    its collection mid-loop and skips nodes): M_WoT_WorldT_SceneBlend still
    holds a whole unreachable PanU/Tex/SceneColor chain beside its live one,
    so "the" Tex/Lightmap parameter of the graph contract is the one an
    output reaches. Orphans stay in the duplicate (they compile to nothing,
    and the inventory-parity check counts them on both sides) - only Time
    orphans are removed, as before."""
    mel = unreal.MaterialEditingLibrary
    roots = []
    for name in _OUTPUT_PROPERTY_NAMES:
        node = mel.get_material_property_input_node(
            material, getattr(unreal.MaterialProperty, name))
        if node is not None:
            roots.append(node)
    found: dict[str, Any] = {}
    pending = list(roots)
    while pending:
        expression = pending.pop()
        key = str(expression.get_path_name())
        if key in found:
            continue
        found[key] = expression
        pending.extend(_connected_inputs(unreal, material, expression))
    return list(found.values())


def _validate_neutral_sampler(unreal: Any, material: Any,
                              expression: Any, parameter_name: str) -> None:
    path = _asset_path(material)
    _require(_asset_path(expression.get_editor_property("texture"))
             == NEUTRAL_TEXTURE, path, "neutral_texture_defaults",
             f"{parameter_name} does not default to {NEUTRAL_TEXTURE}")
    _require(expression.get_editor_property("sampler_type")
             == unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR,
             path, "linear_samplers",
             f"{parameter_name} is not a linear-color sampler")


def _find_unary_constant_multiply(unreal: Any, material: Any,
                                  expressions: list[Any], source: Any,
                                  constant: float, assertion: str) -> Any:
    matches = []
    for expression in expressions:
        if _expression_class(expression) != "MaterialExpressionMultiply":
            continue
        inputs = _input_map(unreal, material, expression)
        if (inputs.get("A") == source and inputs.get("B") is None
                and abs(float(expression.get_editor_property("const_b"))
                        - constant) <= 1e-6):
            matches.append(expression)
    _require(len(matches) == 1, _asset_path(material), assertion,
             f"expected one {constant:g}x multiply sourced by "
             f"{_parameter_name(source) or _expression_class(source)}, "
             f"found {len(matches)}")
    return matches[0]


def _validate_source_graph(unreal: Any, material: Any,
                           expected_blend: Any) -> None:
    path = _asset_path(material)
    mel = unreal.MaterialEditingLibrary
    expressions = list(mel.get_material_expressions(material))
    _require(material.get_editor_property("blend_mode") == expected_blend,
             path, "blend_source_parity", "source blend mode changed")
    live = _live_expressions(unreal, material)
    texture = _find_parameter(
        live, "MaterialExpressionTextureSampleParameter2D", "Tex", path)
    lightmap = _find_parameter(
        live, "MaterialExpressionTextureSampleParameter2D",
        "Lightmap", path)
    _validate_neutral_sampler(unreal, material, texture, "Tex")
    _validate_neutral_sampler(unreal, material, lightmap, "Lightmap")
    _require(int(lightmap.get_editor_property("const_coordinate")) == 1,
             path, "lightmap_uv1_times_two",
             "source Lightmap does not use UV1")
    _find_unary_constant_multiply(
        unreal, material, expressions, lightmap, 2.0,
        "lightmap_uv1_times_two")
    texture_ancestors = _ancestors(unreal, material, texture)
    _require(any(_expression_class(node) == "MaterialExpressionTime"
                 for node in texture_ancestors),
             path, "pan_uv_connections",
             "source Tex pan path no longer contains its original Time node")


def _validate_target_graph(unreal: Any, material: Any, spec: MaterialSpec,
                           source: Any, expected_blend: Any) -> None:
    path = _asset_path(material)
    mel = unreal.MaterialEditingLibrary
    expressions = list(mel.get_material_expressions(material))
    _require(_asset_path(material) == f"{MATERIAL_ROOT}/{spec.name}",
             path, "metadata", "target package identity is not exact")
    _require(str(unreal.EditorAssetLibrary.get_metadata_tag(
        material, ENTRY_GRAPH_TAG)) == ENTRY_GRAPH_VERSION,
        path, "metadata", "graph-version tag is absent or stale")
    _require(material.get_editor_property("blend_mode") == expected_blend,
             path, "blend_source_parity", "target blend mode is wrong")
    _require(material.get_editor_property("blend_mode")
             == source.get_editor_property("blend_mode"),
             path, "blend_source_parity", "target differs from source blend")
    _require(material.get_editor_property("shading_model")
             == source.get_editor_property("shading_model"),
             path, "shading_source_parity",
             "target differs from source shading model")
    _require(bool(material.get_editor_property("two_sided")) == spec.two_sided,
             path, "two_sided", "target two-sided setting is wrong")
    _require(not any(_expression_class(node) == "MaterialExpressionTime"
                     for node in expressions),
             path, "no_global_time", "a global Time expression remains")

    live = _live_expressions(unreal, material)
    texture = _find_parameter(
        live, "MaterialExpressionTextureSampleParameter2D", "Tex", path)
    lightmap = _find_parameter(
        live, "MaterialExpressionTextureSampleParameter2D",
        "Lightmap", path)
    texture_ancestors = _ancestors(unreal, material, texture)
    pan_u = _find_parameter(
        texture_ancestors, "MaterialExpressionScalarParameter", "PanU", path)
    pan_v = _find_parameter(
        texture_ancestors, "MaterialExpressionScalarParameter", "PanV", path)
    scene_time = _find_parameter(
        texture_ancestors, "MaterialExpressionScalarParameter",
        SCENE_TIME_PARAMETER, path)
    dynamic_enable = _find_parameter(
        expressions, "MaterialExpressionScalarParameter",
        DYNAMIC_LIGHT_PARAMETER, path)
    texture_parameters = [node for node in expressions
                          if _expression_class(node)
                          == "MaterialExpressionTextureSampleParameter2D"]
    for node in texture_parameters:
        _validate_neutral_sampler(
            unreal, material, node, _parameter_name(node))
    for node, name in ((pan_u, "PanU"), (pan_v, "PanV"),
                       (scene_time, SCENE_TIME_PARAMETER),
                       (dynamic_enable, DYNAMIC_LIGHT_PARAMETER)):
        _require(abs(float(node.get_editor_property("default_value"))) <= 1e-6,
                 path, "dynamic_light_default_zero"
                 if name == DYNAMIC_LIGHT_PARAMETER else "pan_uv_connections",
                 f"{name} default is not zero")

    texture_inputs = _connected_inputs(unreal, material, texture)
    _require(len(texture_inputs) == 1
             and _expression_class(texture_inputs[0]) == "MaterialExpressionAdd",
             path, "pan_uv_connections",
             "Tex UV input is not the panned Add")
    panned_uv = texture_inputs[0]
    panned_inputs = _input_map(unreal, material, panned_uv)
    texture_coordinate = panned_inputs.get("A")
    pan_offset = panned_inputs.get("B")
    _require(_expression_class(texture_coordinate)
             == "MaterialExpressionTextureCoordinate"
             and int(texture_coordinate.get_editor_property("coordinate_index")) == 0,
             path, "pan_uv_connections", "panned Add.A is not UV0")
    _require(_expression_class(pan_offset) == "MaterialExpressionMultiply",
             path, "pan_uv_connections", "panned Add.B is not pan multiply")
    pan_inputs = _input_map(unreal, material, pan_offset)
    pan_rate = pan_inputs.get("A")
    _require(_expression_class(pan_rate) == "MaterialExpressionAppendVector"
             and pan_inputs.get("B") == scene_time,
             path, "pan_uv_connections",
             "pan multiply is not Append(PanU,PanV) * WoTSceneTime")
    _require_pin(unreal, material, pan_rate, "A", pan_u, "pan_uv_connections")
    _require_pin(unreal, material, pan_rate, "B", pan_v, "pan_uv_connections")

    _require(int(lightmap.get_editor_property("const_coordinate")) == 1
             and not _connected_inputs(unreal, material, lightmap),
             path, "lightmap_uv1_times_two",
             "Lightmap must use its unmodified UV1 coordinate")
    _find_unary_constant_multiply(
        unreal, material, expressions, lightmap, 2.0,
        "lightmap_uv1_times_two")

    source_expressions = list(mel.get_material_expressions(source))
    source_counts = Counter(_expression_class(node)
                            for node in source_expressions)
    expected_counts = source_counts.copy()
    if "MaterialExpressionTime" in expected_counts:
        del expected_counts["MaterialExpressionTime"]
    expected_counts["MaterialExpressionScalarParameter"] += 1
    target_counts = Counter(_expression_class(node) for node in expressions)
    _require(target_counts == expected_counts, path,
             "expression_inventory_parity",
             f"source-copy inventory mismatch: expected {expected_counts}, "
             f"found {target_counts}")
    source_parameters = Counter(
        (_expression_class(node), _parameter_name(node))
        for node in source_expressions if _parameter_name(node))
    expected_parameters = source_parameters.copy()
    expected_parameters[
        ("MaterialExpressionScalarParameter", SCENE_TIME_PARAMETER)] += 1
    target_parameters = Counter(
        (_expression_class(node), _parameter_name(node))
        for node in expressions if _parameter_name(node))
    _require(target_parameters == expected_parameters, path,
             "expression_inventory_parity",
             "source-copy parameter inventory differs beyond WoTSceneTime")

    output_properties = (
        unreal.MaterialProperty.MP_BASE_COLOR,
        unreal.MaterialProperty.MP_EMISSIVE_COLOR,
        unreal.MaterialProperty.MP_ROUGHNESS,
        unreal.MaterialProperty.MP_SPECULAR,
        unreal.MaterialProperty.MP_OPACITY,
        unreal.MaterialProperty.MP_OPACITY_MASK,
    )
    for property_ in output_properties:
        source_output = mel.get_material_property_input_node(source, property_)
        target_output = mel.get_material_property_input_node(material, property_)
        _require(_expression_class(target_output)
                 == _expression_class(source_output),
                 path, "material_outputs",
                 f"output {property_} root differs from source")
        _require(str(mel.get_material_property_input_node_output_name(
                     material, property_))
                 == str(mel.get_material_property_input_node_output_name(
                     source, property_)),
                 path, "material_outputs",
                 f"output {property_} pin differs from source")
    if spec.blend == "masked":
        mask = mel.get_material_property_input_node(
            material, unreal.MaterialProperty.MP_OPACITY_MASK)
        _require(mask == texture and abs(float(material.get_editor_property(
            "opacity_mask_clip_value")) - 0.5) <= 1e-6,
            path, "material_outputs",
            "masked output or 0.5 clip threshold is wrong")
    _require(abs(float(material.get_editor_property("opacity_mask_clip_value"))
                 - float(source.get_editor_property(
                     "opacity_mask_clip_value"))) <= 1e-6,
             path, "material_outputs",
             "opacity-mask clip threshold differs from source")

    compile_errors = list(mel.recompile_material(material))
    _require(not compile_errors, path, "shader_compile",
             "; ".join(str(error) for error in compile_errors))


def validate_editor_graphs(unreal: Any) -> dict[str, Any]:
    eal = unreal.EditorAssetLibrary
    blend_modes = {
        "opaque": unreal.BlendMode.BLEND_OPAQUE,
        "masked": unreal.BlendMode.BLEND_MASKED,
        "alpha_composite": unreal.BlendMode.BLEND_ALPHA_COMPOSITE,
        "modulate": unreal.BlendMode.BLEND_MODULATE,
        "translucent": unreal.BlendMode.BLEND_TRANSLUCENT,
    }
    validated_sources = set()
    rows = []
    for spec in material_specs():
        source_path = f"{MATERIAL_ROOT}/{spec.source}"
        target_path = f"{MATERIAL_ROOT}/{spec.name}"
        _require(eal.does_asset_exist(source_path), source_path,
                 "source_sha256", "source master is absent")
        _require(eal.does_asset_exist(target_path), target_path,
                 "metadata", "generated entry master is absent")
        actual_hash = _validate_source_master_sha256(
            unreal, spec.source, source_path)
        source = unreal.load_asset(source_path)
        target = unreal.load_asset(target_path)
        _require(source is not None, source_path, "source_sha256",
                 "source master could not be loaded")
        _require(target is not None, target_path, "metadata",
                 "generated entry master could not be loaded")
        if spec.source not in validated_sources:
            _validate_source_graph(unreal, source, blend_modes[spec.blend])
            validated_sources.add(spec.source)
        _validate_target_graph(
            unreal, target, spec, source, blend_modes[spec.blend])
        rows.append({
            "package": target_path,
            "source": source_path,
            "source_sha256": actual_hash,
            "assertions": list(required_editor_assertions()),
        })
    return {
        "format": "wot-entry-weather-material-validation-v1",
        "graph_version": ENTRY_GRAPH_VERSION,
        "materials": rows,
    }


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-root", required=True)
    parser.add_argument("--validate-only", action="store_true")
    return parser


def main() -> None:
    import unreal  # type: ignore

    args = _parser().parse_args()
    validate_output_root(str(args.output_root))
    receipt = (validate_editor_graphs(unreal) if args.validate_only
               else generate(args, unreal))
    result_path = Path(unreal.Paths.project_saved_dir()) / (
        "entry_weather_materials_validation.json" if args.validate_only
        else "entry_weather_materials_result.json")
    result_path.write_text(json.dumps(receipt, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
