#!/usr/bin/env python3
"""Generate the authenticated retail WheelOfTime entry descriptor sidecar.

Retail bytes are developer/release input only. This tool writes a closed
descriptor containing identities, exact semantic census, camera bit patterns,
resource logical IDs, and hashes; it never copies source payload bytes.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import struct
import sys
import tempfile
from collections import Counter
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from wot_config import original_dir  # noqa: E402


FORMAT = "wot-front-end-descriptor-sidecar-v2"
FIXTURE_FORMAT = "wot-front-end-semantic-fixture-v1"
MAP_NAME = "WheelOfTime"
MAP_SOURCE = "Maps/WheelOfTime.wot"
DESCRIPTOR_DOMAIN = b"WOT-FRONT-END-DESCRIPTOR-V2\0"
SIDECAR_DOMAIN = b"WOT-FRONT-END-DESCRIPTOR-SIDECAR-V2\0"
SYNTHETIC_SOURCE_DOMAIN = b"WOT-FRONT-END-SYNTHETIC-SOURCE-V1\0"
RETAIL_SOURCE_DOMAIN = b"WOT-FRONT-END-SOURCE-V1\0"
MAX_SOURCE_BYTES = 256 * 1024 * 1024
MAX_COORDINATE = 10_000_000.0

EXPECTED_CLASSES = {
    "levelinfo": 1,
    "brush": 76,
    "playerstart": 1,
    "wotzoneinfo": 1,
    "light": 39,
    "particlesprayer": 4,
    "ambientsound": 5,
    "randomlightning": 1,
    "lightning": 5,
    "waterzone": 1,
    "skyzoneinfo": 1,
    "raincyd": 6,
}
CENSUS_CLASSES = {
    "lights": "light",
    "ambient_sounds": "ambientsound",
    "splash_emitters": "particlesprayer",
    "rain_cylinders": "raincyd",
    "lightning_actors": "lightning",
    "random_lightning_controllers": "randomlightning",
}
DESCRIPTOR_FIELDS = {
    "bsp_logical_id", "camera", "census", "descriptor_root", "map_name",
    "required_logical_ids", "required_resources", "source_root",
    "thunder_sound_source_exports", "world_logical_id",
}
REQUIRED_RESOURCE_FIELDS = {
    "domain", "logical_id", "source_export", "source_path",
}
CAMERA_FIELDS = {"horizontal_fov_bits", "location_bits", "rotation_bits"}
CENSUS_FIELDS = set(CENSUS_CLASSES)
BINDING_FIELDS = {
    "installer_sha256", "contract_sha256", "production_descriptor_sha256",
}
SIDECAR_FIELDS = {
    "contract_sha256", "descriptor", "format", "installer_sha256",
    "production_descriptor_sha256", "record_sha256",
}
SAFE_LOGICAL_ID = re.compile(r"^cache/[A-Za-z0-9_.-]+(?:/[A-Za-z0-9_.-]+)*$")
OBJECT_LITERAL = re.compile(
    r"\b(Texture|Sound|Mesh|LodMesh|Font|FireTexture|WaterTexture|"
    r"WaveTexture|WetTexture|IceTexture)'([^']+)'", re.IGNORECASE)

EXPECTED_RESOURCE_GRAPH = (
    ("animation", "System/Environment.u", "Environment.RainCyd"),
    ("font", "System/WOT.u", "WOT.F_WOTReg14"),
    ("font", "System/WOT.u", "WOT.F_WOTReg30"),
    ("localization", "System/WoT.int", "WoT.int"),
    ("mesh", "System/Environment.u", "Environment.RainCyd"),
    ("procedural", "Textures/JHPFireEng.utx", "JHPFireEng.M01ModWet"),
    ("procedural", "Textures/WarrenT.utx", "WarrenT.Rain2"),
    ("sound", "Sounds/AmbientA.uax", "AmbientA.Water.DIVE"),
    ("sound", "Sounds/AmbientA.uax", "AmbientA.Water.Stream6"),
    ("sound", "Sounds/AmbientA.uax", "AmbientA.Water.Stream_1"),
    ("sound", "Sounds/AmbientA.uax", "AmbientA.Water.Trickle3"),
    ("sound", "Sounds/AmbientA.uax", "AmbientA.Water.WaterFll5"),
    ("sound", "Sounds/AmbientA.uax", "AmbientA.Wind.Wind"),
    ("sound", "System/Environment.u", "Environment.Thunder.Thunder10"),
    ("sound", "System/Environment.u", "Environment.Thunder.Thunder2"),
    ("sound", "System/Environment.u", "Environment.Thunder.Thunder3"),
    ("sound", "System/Environment.u", "Environment.Thunder.Thunder4"),
    ("sound", "System/Environment.u", "Environment.Thunder.Thunder8"),
    ("sound", "System/Environment.u", "Environment.Thunder.Thunder9"),
    ("sound", "System/WOT.u", "WOT.MoveSelector"),
    ("sound", "System/WOT.u", "WOT.Select"),
    ("texture", "System/Environment.u", "Environment.Skins.RainDropsA"),
    ("texture", "Textures/ForsakenT.utx", "ForsakenT.DetailArchitect.MarbBalc2720"),
    ("texture", "Textures/ForsakenT.utx", "ForsakenT.Dev.TypeMask_003sky"),
    ("texture", "Textures/ForsakenT.utx", "ForsakenT.SurfaceFloor.MarblStep2362"),
    ("texture", "Textures/JamesT.utx", "JamesT.flare_blue01"),
    ("texture", "Textures/MordethT.utx", "MordethT.DetailArchitect.MetlGril4002v02"),
    ("texture", "Textures/MordethT.utx", "MordethT.DetailArchitect.StonArch4348"),
    ("texture", "Textures/MordethT.utx", "MordethT.DetailArchitect.StonTrim4353"),
    ("texture", "Textures/MordethT.utx", "MordethT.DetailArchitect.StonWind4347v1"),
    ("texture", "Textures/MordethT.utx", "MordethT.SurfaceFloor.StonFlor4900"),
    ("texture", "Textures/MordethT.utx", "MordethT.SurfaceWall.PlasWall4373v1"),
    ("texture", "Textures/MordethT.utx", "MordethT.SurfaceWall.StonBlok4345"),
    ("texture", "Textures/MordethT.utx", "MordethT.SurfaceWall.StonBlok4345v1"),
    ("texture", "Textures/MordethT.utx", "MordethT.SurfaceWall.StonWall4018"),
    ("texture", "Textures/MordethT.utx", "MordethT.SurfaceWall.StonWall4031v02"),
    ("texture", "Textures/MordethT.utx", "MordethT.SurfaceWall.StonWall4333"),
    ("texture", "Textures/NatureT.utx", "NatureT.Floor.DirtDirt_010"),
    ("texture", "Textures/NatureT.utx", "NatureT.Sky.Sky_440"),
    ("texture", "Textures/NatureT.utx", "NatureT.VerticalFace.StonFace_931"),
    ("texture", "System/ParticleSystems.u", "ParticleSystems.Water.Splash02"),
    ("texture", "Textures/ScottT.utx", "ScottT.mountain2"),
    ("texture", "System/WOT.u", "WOT.F_WOTReg14.Texture9"),
    ("texture", "System/WOT.u", "WOT.F_WOTReg30.Texture10"),
    ("texture", "Textures/WarrenT.utx", "WarrenT.Developer.Black"),
    ("texture", "Textures/WarrenT.utx", "WarrenT.MarcHigh.RckFlr_9011mt"),
)
EXPECTED_TYPED_IDENTITIES = {
    (domain, source_export.casefold())
    for domain, _source_package, source_export in EXPECTED_RESOURCE_GRAPH
}


class DescriptorError(ValueError):
    pass


def canonical_bytes(value: Any) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"),
                      ensure_ascii=True).encode("ascii")


def _sha256(raw: bytes) -> str:
    return hashlib.sha256(raw).hexdigest()


def _read(path: Path, what: str, maximum: int | None = None) -> bytes:
    try:
        raw = path.read_bytes()
    except OSError as exc:
        raise DescriptorError(f"cannot read {what} {path}: {exc}") from exc
    if maximum is not None and (not raw or len(raw) > maximum):
        raise DescriptorError(f"{what} {path} is empty or exceeds {maximum} bytes")
    return raw


def _object(value: Any, fields: set[str], what: str) -> dict[str, Any]:
    if not isinstance(value, dict) or set(value) != fields:
        raise DescriptorError(f"{what} must contain exactly {sorted(fields)}")
    return value


def _hex_digest(value: Any, what: str) -> str:
    if (not isinstance(value, str) or len(value) != 64
            or any(character not in "0123456789abcdef" for character in value)):
        raise DescriptorError(f"{what} must be a lower-case SHA-256 digest")
    return value


def _logical_id(value: Any, what: str) -> str:
    if not isinstance(value, str) or SAFE_LOGICAL_ID.fullmatch(value) is None:
        raise DescriptorError(f"{what} is not a safe cache logical ID")
    if any(component in {"", ".", ".."} for component in value.split("/")):
        raise DescriptorError(f"{what} contains an unsafe path component")
    return value


def _signed_i64_bits(value: float) -> int:
    unsigned = struct.unpack("<Q", struct.pack("<d", value))[0]
    return unsigned if unsigned < (1 << 63) else unsigned - (1 << 64)


def _u32_bits(value: float) -> int:
    return struct.unpack("<I", struct.pack("<f", value))[0]


def _double_from_bits(value: Any, what: str) -> float:
    if isinstance(value, bool) or not isinstance(value, int) \
            or value < -(1 << 63) or value >= (1 << 63):
        raise DescriptorError(f"{what} is not a signed 64-bit bit pattern")
    result = struct.unpack("<d", struct.pack("<Q", value & ((1 << 64) - 1)))[0]
    if not math.isfinite(result):
        raise DescriptorError(f"{what} decodes to a non-finite number")
    return result


def _float_from_bits(value: Any, what: str) -> float:
    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value < (1 << 32):
        raise DescriptorError(f"{what} is not an unsigned 32-bit bit pattern")
    result = struct.unpack("<f", struct.pack("<I", value))[0]
    if not math.isfinite(result):
        raise DescriptorError(f"{what} decodes to a non-finite number")
    return result


def _contract_facts(contract: Any) -> list[dict[str, Any]]:
    if not isinstance(contract, dict):
        raise DescriptorError("contract must be an object")
    payload = contract.get("payload", contract)
    if not isinstance(payload, dict) or not isinstance(payload.get("facts"), list):
        raise DescriptorError("contract must contain a facts array")
    facts: list[dict[str, Any]] = []
    seen_ids: set[str] = set()
    for index, fact in enumerate(payload["facts"]):
        if not isinstance(fact, dict):
            raise DescriptorError(f"facts[{index}] is not an object")
        source = fact.get("source")
        outputs = fact.get("outputs")
        if (not isinstance(fact.get("domain"), str)
                or not isinstance(source, dict)
                or not isinstance(source.get("package"), str)
                or not isinstance(source.get("export"), str)
                or not isinstance(outputs, list) or not outputs):
            raise DescriptorError(f"facts[{index}] lacks its typed source or outputs")
        normalized_outputs = []
        for output_index, output in enumerate(outputs):
            if not isinstance(output, dict) or "logical_id" not in output:
                raise DescriptorError(f"facts[{index}].outputs[{output_index}] is malformed")
            logical_id = _logical_id(
                output["logical_id"], f"facts[{index}].outputs[{output_index}]")
            if logical_id in seen_ids:
                raise DescriptorError(f"contract has duplicate logical ID {logical_id!r}")
            seen_ids.add(logical_id)
            normalized_outputs.append(logical_id)
        facts.append({
            "domain": fact["domain"],
            "outputs": normalized_outputs,
            "source_export": source["export"],
            "source_package": source["package"],
        })
    return facts


def _close_contract(references: list[dict[str, str]], retail_contract: Any
                    ) -> tuple[str, str, list[dict[str, str]], list[str], list[str]]:
    facts = _contract_facts(retail_contract)
    bsp = [fact for fact in facts
           if fact["domain"] == "bsp"
           and fact["source_package"].casefold() == MAP_SOURCE.casefold()]
    world = [fact for fact in facts
             if fact["domain"] == "world"
             and fact["source_package"].casefold() == MAP_SOURCE.casefold()]
    if (len(bsp) != 1 or len(world) != 1 or len(bsp[0]["outputs"]) != 1
            or len(world[0]["outputs"]) != 1
            or bsp[0]["source_export"].casefold() != "wheeloftime.model"
            or world[0]["source_export"].casefold() != "wheeloftime.level"):
        raise DescriptorError("contract lacks the singular WheelOfTime BSP/world facts")
    folded_references = [
        (reference["domain"], reference["identity"].casefold())
        for reference in references
    ]
    if len(folded_references) != len(set(folded_references)):
        raise DescriptorError("resource closure contains a duplicate typed reference")
    if set(folded_references) != EXPECTED_TYPED_IDENTITIES:
        missing = sorted(EXPECTED_TYPED_IDENTITIES - set(folded_references))
        extra = sorted(set(folded_references) - EXPECTED_TYPED_IDENTITIES)
        raise DescriptorError(
            f"resource closure differs from the pinned graph; missing={missing}, extra={extra}")
    required_resources = [
        {
            "domain": "bsp",
            "logical_id": bsp[0]["outputs"][0],
            "source_export": bsp[0]["source_export"],
            "source_path": bsp[0]["source_package"],
        },
        {
            "domain": "world",
            "logical_id": world[0]["outputs"][0],
            "source_export": world[0]["source_export"],
            "source_path": world[0]["source_package"],
        },
    ]
    source_packages = {MAP_SOURCE}
    for reference in references:
        matches = [fact for fact in facts
                   if fact["domain"] == reference["domain"]
                   and fact["source_export"].casefold()
                   == reference["identity"].casefold()]
        if len(matches) != 1:
            raise DescriptorError(
                f"resource {reference!r} has {len(matches)} typed contract authorizations")
        if len(matches[0]["outputs"]) != 1:
            raise DescriptorError(f"resource {reference!r} must have exactly one output")
        required_resources.append({
            "domain": matches[0]["domain"],
            "logical_id": matches[0]["outputs"][0],
            "source_export": matches[0]["source_export"],
            "source_path": matches[0]["source_package"],
        })
        source_packages.add(matches[0]["source_package"])
    required_resources.sort(key=lambda item: (
        item["domain"], item["source_export"].casefold(), item["logical_id"]))
    required = [resource["logical_id"] for resource in required_resources]
    ordered = sorted(required)
    if len(set(ordered)) != len(required):
        raise DescriptorError("resource closure aliases a logical output")
    return (bsp[0]["outputs"][0], world[0]["outputs"][0], required_resources,
            ordered, sorted(source_packages))


def _validate_semantic_fixture(value: Any) -> dict[str, Any]:
    root = _object(value, {"actors", "camera", "format", "map_name", "references",
                           "thunder_sound_source_exports"},
                   "semantic fixture")
    if root["format"] != FIXTURE_FORMAT or root["map_name"] != MAP_NAME:
        raise DescriptorError("semantic fixture has the wrong protocol or map identity")
    if not isinstance(root["actors"], list) or not isinstance(root["references"], list):
        raise DescriptorError("semantic fixture actors/references must be arrays")
    camera = _object(root["camera"],
                     {"horizontal_fov", "location", "rotation"}, "camera")
    if (camera["horizontal_fov"] != 90
            or not isinstance(camera["location"], list)
            or not isinstance(camera["rotation"], list)
            or len(camera["location"]) != 3 or len(camera["rotation"]) != 3):
        raise DescriptorError("camera is missing, malformed, or has the wrong FOV")
    def camera_number(value: Any, label: str) -> float:
        if isinstance(value, bool) or not isinstance(value, (int, float, str)):
            raise DescriptorError(f"camera {label} is not numeric")
        try:
            result = float(value)
        except ValueError as exc:
            raise DescriptorError(f"camera {label} is not numeric") from exc
        if not math.isfinite(result):
            raise DescriptorError(f"camera {label} is non-finite")
        return result

    locations = [camera_number(value, f"location[{index}]")
                 for index, value in enumerate(camera["location"])]
    rotations = [camera_number(value, f"rotation[{index}]")
                 for index, value in enumerate(camera["rotation"])]
    if any(abs(value) > MAX_COORDINATE for value in locations):
        raise DescriptorError("camera location is non-finite or outside its bound")

    camera = {
        "horizontal_fov": camera["horizontal_fov"],
        "location": locations,
        "rotation": rotations,
    }
    identities: set[str] = set()
    classes: Counter[str] = Counter()
    player_starts = []
    for index, actor in enumerate(root["actors"]):
        actor = _object(actor, {"class", "identity"}, f"actors[{index}]")
        if not isinstance(actor["class"], str) or not isinstance(actor["identity"], str):
            raise DescriptorError(f"actors[{index}] has a non-string field")
        class_name = actor["class"].rsplit(".", 1)[-1].casefold()
        identity = actor["identity"].casefold()
        if class_name not in EXPECTED_CLASSES:
            raise DescriptorError(f"actor {actor['identity']!r} is unclassified or campaign")
        if not identity or identity in identities:
            raise DescriptorError(f"actor identity {actor['identity']!r} is duplicate")
        identities.add(identity)
        classes[class_name] += 1
        if class_name == "playerstart":
            player_starts.append(actor)
    if dict(classes) != EXPECTED_CLASSES or len(player_starts) != 1:
        raise DescriptorError("actor census disagrees with the pinned entry scene")
    references: list[dict[str, str]] = []
    for index, reference in enumerate(root["references"]):
        reference = _object(
            reference, {"domain", "identity", "kind"}, f"references[{index}]")
        if (reference["kind"] != "resource"
                or not isinstance(reference["domain"], str)
                or not isinstance(reference["identity"], str)
                or not reference["identity"]):
            raise DescriptorError(f"references[{index}] is unclassified")
        references.append({
            "domain": reference["domain"],
            "identity": reference["identity"],
        })
    thunder = root["thunder_sound_source_exports"]
    if (not isinstance(thunder, list) or len(thunder) != 10
            or any(not isinstance(identity, str) or not identity
                   for identity in thunder)):
        raise DescriptorError(
            "semantic fixture must contain ten ordered thunder sound exports")
    return {
        "actors": root["actors"],
        "camera": camera,
        "classes": classes,
        "map_name": root["map_name"],
        "references": references,
        "thunder_sound_source_exports": list(thunder),
    }


def _descriptor_payload(semantic: dict[str, Any], retail_contract: Any,
                        source_root: str) -> dict[str, Any]:
    bsp_id, world_id, required_resources, required_ids, _sources = _close_contract(
        semantic["references"], retail_contract)
    camera = semantic["camera"]
    payload: dict[str, Any] = {
        "bsp_logical_id": bsp_id,
        "camera": {
            "horizontal_fov_bits": _u32_bits(float(camera["horizontal_fov"])),
            "location_bits": [_signed_i64_bits(float(value))
                              for value in camera["location"]],
            "rotation_bits": [_signed_i64_bits(float(value))
                              for value in camera["rotation"]],
        },
        "census": {
            field: semantic["classes"][class_name]
            for field, class_name in CENSUS_CLASSES.items()
        },
        "map_name": semantic["map_name"],
        "required_logical_ids": required_ids,
        "required_resources": required_resources,
        "source_root": _hex_digest(source_root, "source_root"),
        "thunder_sound_source_exports": list(
            semantic["thunder_sound_source_exports"]),
        "world_logical_id": world_id,
    }
    payload["descriptor_root"] = _sha256(
        DESCRIPTOR_DOMAIN + canonical_bytes(payload))
    validate_descriptor(payload)
    return payload


def build_descriptor_from_fixture(value: Any, retail_contract: Any) -> dict[str, Any]:
    semantic = _validate_semantic_fixture(value)
    source_root = _sha256(SYNTHETIC_SOURCE_DOMAIN + canonical_bytes(value))
    return _descriptor_payload(semantic, retail_contract, source_root)


def validate_descriptor(value: Any) -> dict[str, Any]:
    descriptor = _object(value, DESCRIPTOR_FIELDS, "front-end descriptor")
    if descriptor["map_name"] != MAP_NAME:
        raise DescriptorError("front-end descriptor has the wrong map identity")
    bsp_id = _logical_id(descriptor["bsp_logical_id"], "bsp_logical_id")
    world_id = _logical_id(descriptor["world_logical_id"], "world_logical_id")
    if not bsp_id.startswith("cache/bsp/") or not world_id.startswith("cache/world/"):
        raise DescriptorError("front-end descriptor BSP/world domains are wrong")
    _hex_digest(descriptor["source_root"], "source_root")
    _hex_digest(descriptor["descriptor_root"], "descriptor_root")
    camera = _object(descriptor["camera"], CAMERA_FIELDS, "descriptor camera")
    if (not isinstance(camera["location_bits"], list)
            or not isinstance(camera["rotation_bits"], list)
            or len(camera["location_bits"]) != 3
            or len(camera["rotation_bits"]) != 3):
        raise DescriptorError("descriptor camera bit arrays must contain three values")
    locations = [_double_from_bits(value, f"location_bits[{index}]")
                 for index, value in enumerate(camera["location_bits"])]
    for index, value in enumerate(camera["rotation_bits"]):
        _double_from_bits(value, f"rotation_bits[{index}]")
    if any(abs(value) > MAX_COORDINATE for value in locations):
        raise DescriptorError("descriptor camera location exceeds its bound")
    if _float_from_bits(camera["horizontal_fov_bits"], "horizontal_fov_bits") != 90.0:
        raise DescriptorError("descriptor camera FOV is not pinned to 90")
    census = _object(descriptor["census"], CENSUS_FIELDS, "descriptor census")
    expected_census = {
        field: EXPECTED_CLASSES[class_name]
        for field, class_name in CENSUS_CLASSES.items()
    }
    if census != expected_census:
        raise DescriptorError("descriptor semantic census is not pinned")
    required = descriptor["required_logical_ids"]
    if not isinstance(required, list) or len(required) < 2:
        raise DescriptorError("descriptor resource closure is missing")
    checked = [_logical_id(value, f"required_logical_ids[{index}]")
               for index, value in enumerate(required)]
    if checked != sorted(set(checked)) or bsp_id not in checked or world_id not in checked:
        raise DescriptorError("descriptor resource closure is not sorted, unique, and complete")
    resources = descriptor["required_resources"]
    if not isinstance(resources, list) or len(resources) != 48:
        raise DescriptorError("typed resource closure must contain exactly 48 entries")
    expected_graph = {
        (domain, source_package.casefold(), source_export.casefold())
        for domain, source_package, source_export in EXPECTED_RESOURCE_GRAPH
    }
    expected_graph.update({
        ("bsp", MAP_SOURCE.casefold(), "wheeloftime.model"),
        ("world", MAP_SOURCE.casefold(), "wheeloftime.level"),
    })
    observed_graph: set[tuple[str, str, str]] = set()
    projected_ids = []
    prior_key: tuple[str, str, str] | None = None
    for index, resource in enumerate(resources):
        resource = _object(
            resource, REQUIRED_RESOURCE_FIELDS, f"required_resources[{index}]")
        domain = resource["domain"]
        source_path = resource["source_path"]
        source_export = resource["source_export"]
        if (not isinstance(domain, str) or not isinstance(source_path, str)
                or not isinstance(source_export, str)):
            raise DescriptorError(f"required_resources[{index}] has a non-string field")
        logical_id = _logical_id(
            resource["logical_id"], f"required_resources[{index}].logical_id")
        if not logical_id.startswith(f"cache/{domain}/"):
            raise DescriptorError(
                f"required_resources[{index}] logical ID has the wrong domain")
        graph_key = (domain, source_path.casefold(), source_export.casefold())
        order_key = (domain, source_export.casefold(), logical_id)
        if prior_key is not None and order_key <= prior_key:
            raise DescriptorError("typed resource closure is not sorted and unique")
        prior_key = order_key
        if graph_key in observed_graph:
            raise DescriptorError("typed resource closure has a duplicate graph edge")
        observed_graph.add(graph_key)
        projected_ids.append(logical_id)
    if observed_graph != expected_graph:
        raise DescriptorError("typed resource closure differs from the pinned graph")
    if sorted(projected_ids) != checked:
        raise DescriptorError("logical closure is not the exact typed-resource projection")
    thunder = descriptor["thunder_sound_source_exports"]
    if (not isinstance(thunder, list) or len(thunder) != 10
            or any(not isinstance(identity, str) or not identity
                   for identity in thunder)):
        raise DescriptorError(
            "descriptor must contain ten ordered thunder sound exports")
    thunder_resources = {
        resource["source_export"]
        for resource in resources
        if (resource["domain"] == "sound"
            and resource["source_path"].casefold() == "system/environment.u"
            and resource["source_export"].casefold().startswith(
                "environment.thunder."))
    }
    if set(thunder) != thunder_resources:
        raise DescriptorError(
            "ordered thunder authority differs from the typed resource closure")
    bsp_resources = [resource for resource in resources
                     if resource["domain"] == "bsp"]
    world_resources = [resource for resource in resources
                       if resource["domain"] == "world"]
    if (len(bsp_resources) != 1 or bsp_resources[0]["logical_id"] != bsp_id
            or len(world_resources) != 1
            or world_resources[0]["logical_id"] != world_id):
        raise DescriptorError("BSP/world fields disagree with their typed resources")
    unsigned = {key: descriptor[key] for key in sorted(DESCRIPTOR_FIELDS - {"descriptor_root"})}
    expected_root = _sha256(DESCRIPTOR_DOMAIN + canonical_bytes(unsigned))
    if descriptor["descriptor_root"] != expected_root:
        raise DescriptorError("descriptor_root does not match the descriptor fields")
    return descriptor


def _bindings(value: Any) -> dict[str, str]:
    bindings = _object(value, BINDING_FIELDS, "sidecar bindings")
    return {field: _hex_digest(bindings[field], field) for field in BINDING_FIELDS}


def write_sidecar(descriptor: Any, bindings: Any) -> bytes:
    descriptor = validate_descriptor(descriptor)
    bindings = _bindings(bindings)
    unsigned = {
        "contract_sha256": bindings["contract_sha256"],
        "descriptor": descriptor,
        "format": FORMAT,
        "installer_sha256": bindings["installer_sha256"],
        "production_descriptor_sha256": bindings["production_descriptor_sha256"],
    }
    sidecar = dict(unsigned)
    sidecar["record_sha256"] = _sha256(SIDECAR_DOMAIN + canonical_bytes(unsigned))
    return canonical_bytes(sidecar)


def parse_sidecar(raw: bytes, expected_bindings: Any,
                  expected_descriptor_root: str) -> dict[str, Any]:
    try:
        value = json.loads(raw.decode("ascii"))
    except (UnicodeError, json.JSONDecodeError) as exc:
        raise DescriptorError(f"sidecar is not canonical ASCII JSON: {exc}") from exc
    if canonical_bytes(value) != raw:
        raise DescriptorError("sidecar is not canonical JSON")
    sidecar = _object(value, SIDECAR_FIELDS, "front-end sidecar")
    bindings = _bindings(expected_bindings)
    if sidecar["format"] != FORMAT:
        raise DescriptorError("sidecar has the wrong protocol")
    for field in BINDING_FIELDS:
        if sidecar[field] != bindings[field]:
            raise DescriptorError(f"sidecar {field} does not match the authenticated input")
    expected_descriptor_root = _hex_digest(
        expected_descriptor_root, "expected_descriptor_root")
    _hex_digest(sidecar["record_sha256"], "record_sha256")
    unsigned = {key: sidecar[key] for key in SIDECAR_FIELDS - {"record_sha256"}}
    if sidecar["record_sha256"] != _sha256(
            SIDECAR_DOMAIN + canonical_bytes(unsigned)):
        raise DescriptorError("sidecar record seal is invalid")
    descriptor = validate_descriptor(sidecar["descriptor"])
    if descriptor["descriptor_root"] != expected_descriptor_root:
        raise DescriptorError("descriptor root is not the authenticated snapshot")
    return descriptor


def _import_retail_helpers() -> tuple[Any, ...]:
    repo = Path(__file__).resolve().parents[2]
    convert = repo / "Tools/convert"
    if str(convert) not in sys.path:
        sys.path.insert(0, str(convert))
    dist = repo / "Tools/dist"
    if str(dist) not in sys.path:
        sys.path.insert(0, str(dist))
    try:
        from t3d import parse, typed
        from ue1pkg import (Package, PayloadReader, Reader, _level_actor_refs,
                            _level_reader_after_actor_refs, _skip_level_url,
                            decode_font, decode_model, export_level,
                            read_property_tags)
        from build_payload_cache import (_canonical_identity_table,
                                         _decoded_class_defaults)
    except ImportError as exc:
        raise DescriptorError(f"cannot import the retail package helpers: {exc}") from exc
    return (Package, Reader, decode_font, decode_model, export_level,
            lambda path: typed(parse(path)), _level_reader_after_actor_refs,
            _skip_level_url, _canonical_identity_table, _decoded_class_defaults,
            PayloadReader, _level_actor_refs, read_property_tags)


def _domains_for_resource_class(class_name: str) -> tuple[str, ...]:
    folded = class_name.rsplit(".", 1)[-1].casefold()
    if folded == "sound":
        return ("sound",)
    if folded == "font":
        return ("font",)
    if folded in {"mesh", "lodmesh"}:
        return ("animation", "mesh")
    if folded == "texture":
        return ("texture",)
    if folded in {
        "firetexture", "watertexture", "wavetexture", "wettexture", "icetexture",
    }:
        return ("procedural",)
    raise DescriptorError(f"resource class {class_name!r} has no typed domain")


def _decode_default_string(Reader: Any, raw: bytes, what: str) -> str:
    reader = Reader(raw)
    count = reader.cindex()
    if count <= 0 or reader.o + count != len(raw):
        raise DescriptorError(f"{what} is not one bounded UE1 string")
    payload = raw[reader.o:]
    if payload[-1:] != b"\0" or b"\0" in payload[:-1]:
        raise DescriptorError(f"{what} is not one NUL-terminated UE1 string")
    try:
        return payload[:-1].decode("latin-1")
    except UnicodeError as exc:
        raise DescriptorError(f"{what} is not Latin-1") from exc


def _derive_thunder_sound_authority(
        defaults: list[dict[str, Any]], overrides: list[dict[str, Any]],
        exports: list[dict[str, str]]) -> list[str]:
    """Resolve UE1 RandomLightning string defaults/overrides to typed Sounds."""
    def ordered(rows: list[dict[str, Any]], indices: range,
                label: str) -> list[str]:
        selected = [row for row in rows if row.get("name") == "ThunderSounds"]
        if (len(selected) != len(indices)
                or sorted(row.get("array_index") for row in selected) != list(indices)
                or any(row.get("type") != 13
                       or not isinstance(row.get("value"), str)
                       or not row["value"] for row in selected)):
            raise DescriptorError(
                f"RandomLightning {label} thunder strings are malformed or noncontiguous")
        by_index = {row["array_index"]: row["value"] for row in selected}
        return [by_index[index] for index in indices]

    def resolve(short_identity: str, label: str) -> str:
        leaf = short_identity.rsplit(".", 1)[-1]
        if short_identity != f"Environment.{leaf}":
            raise DescriptorError(
                f"{label} {short_identity!r} has the wrong source root")
        matches = [entry for entry in exports
                   if entry.get("name") == leaf
                   and entry.get("class") == "Sound"]
        if len(matches) != 1:
            reason = "ambiguous" if len(matches) > 1 else "does not resolve to a Sound"
            raise DescriptorError(f"{label} {short_identity!r} {reason}")
        identity = matches[0].get("identity")
        if not isinstance(identity, str) or not identity:
            raise DescriptorError(f"{label} has no canonical Sound identity")
        return identity

    default_identities = [
        resolve(value, f"ThunderSounds[{index}]")
        for index, value in enumerate(ordered(defaults, range(6), "default"))
    ]
    if len(set(identity.casefold() for identity in default_identities)) != 6:
        raise DescriptorError("RandomLightning default thunder set is not unique")
    closed = {identity.casefold() for identity in default_identities}
    override_identities = [
        resolve(value, f"ThunderSounds[{index}]")
        for index, value in enumerate(
            ordered(overrides, range(6, 10), "instance override"), 6)
    ]
    for identity in override_identities:
        if identity.casefold() not in closed:
            raise DescriptorError(
                f"RandomLightning override {identity!r} is outside the closed thunder set")
    return [*default_identities, *override_identities]


def _class_export(package: Any, name: str) -> dict[str, Any]:
    matches = [export for export in package.exports
               if export["cls"] == 0 and export["name"] == name]
    if len(matches) != 1:
        raise DescriptorError(
            f"{package.name} must contain one UClass {name}, found {len(matches)}")
    return matches[0]


def _complete_retail_resource_graph(source: Path, direct: list[dict[str, str]],
                                    helpers: tuple[Any, ...],
                                    thunder_overrides: list[dict[str, Any]],
                                    dependency_root: Path | None = None
                                    ) -> tuple[list[dict[str, str]], list[str]]:
    (Package, Reader, decode_font, _decode_model, _export_level, _parse_typed,
     _level_reader, _skip_url, canonical_identity_table,
     decoded_class_defaults, _PayloadReader, _level_actor_refs,
     _read_property_tags) = helpers
    resources: dict[tuple[str, str], dict[str, str]] = {}

    def add(domain: str, identity: str, context: str) -> None:
        key = (domain, identity.casefold())
        if key in resources:
            raise DescriptorError(f"duplicate typed resource {identity!r} from {context}")
        resources[key] = {"domain": domain, "identity": identity, "kind": "resource"}

    for resource in direct:
        add(resource["domain"], resource["identity"], "entry map")

    system = (dependency_root if dependency_root is not None
              else source.parent.parent) / "System"
    environment = Package(system / "Environment.u")
    environment_identities = canonical_identity_table(environment)
    rain_defaults = decoded_class_defaults(
        environment, _class_export(environment, "RainCyd"),
        identity_table=environment_identities)
    for property_name, expected_domains in (
            ("Mesh", ("animation", "mesh")), ("Skin", ("texture",))):
        matches = [prop for prop in rain_defaults if prop["name"] == property_name]
        if len(matches) != 1 or not matches[0]["object_identity"]:
            raise DescriptorError(f"RainCyd must declare one typed {property_name}")
        identity = re.sub(r"#export-[0-9]+$", "", matches[0]["object_identity"])
        actual_domains = _domains_for_resource_class(matches[0]["object_class"])
        if tuple(sorted(actual_domains)) != tuple(sorted(expected_domains)):
            raise DescriptorError(f"RainCyd {property_name} has the wrong resource class")
        for domain in expected_domains:
            add(domain, identity, f"RainCyd.{property_name}")

    lightning_defaults = decoded_class_defaults(
        environment, _class_export(environment, "RandomLightning"),
        identity_table=environment_identities)
    thunder_defaults = []
    for prop in lightning_defaults:
        if prop["name"] != "ThunderSounds":
            continue
        thunder_defaults.append({
            "array_index": prop["array_index"],
            "name": prop["name"],
            "type": prop["type"],
            "value": _decode_default_string(
                Reader, prop["value"],
                f"ThunderSounds[{prop['array_index']}]")
            if prop["type"] == 13 else None,
        })
    environment_exports = [
        {
            "class": environment.objname(export["cls"]),
            "identity": f"Environment.{environment.objpath(index)}",
            "name": export["name"],
        }
        for index, export in enumerate(environment.exports, 1)
    ]
    thunder_authority = _derive_thunder_sound_authority(
        thunder_defaults, thunder_overrides, environment_exports)
    for index, identity in enumerate(thunder_authority[:6]):
        add("sound", identity, f"ThunderSounds[{index}]")

    wot = Package(system / "WOT.u")
    for font_identity in ("WOT.F_WOTReg14", "WOT.F_WOTReg30"):
        export_matches = [export for index, export in enumerate(wot.exports, 1)
                          if wot.objname(export["cls"]) == "Font"
                          and f"WOT.{wot.objpath(index)}" == font_identity]
        if len(export_matches) != 1:
            raise DescriptorError(f"required font {font_identity} is absent or ambiguous")
        add("font", font_identity, "required main-menu face")
        font = decode_font(wot, export_matches[0])
        if len(font["pages"]) != 1:
            raise DescriptorError(f"required font {font_identity} must have one atlas page")
        page_ref = font["pages"][0]["texture_ref"]
        add("texture", f"WOT.{wot.objpath(page_ref)}", f"{font_identity} atlas")

    add("localization", "WoT.int", "required main-menu labels")
    add("sound", "WOT.MoveSelector", "required main-menu navigation")
    add("sound", "WOT.Select", "required main-menu activation")
    return (sorted(resources.values(), key=lambda item: (
        item["domain"], item["identity"].casefold(), item["identity"])),
        thunder_authority)


def inspect_retail_map(source: Path,
                       dependency_root: Path | None = None) -> dict[str, Any]:
    source = source.resolve()
    raw = _read(source, "retail entry map", MAX_SOURCE_BYTES)
    del raw
    if source.name.casefold() != "wheeloftime.wot":
        raise DescriptorError("retail entry source must be Maps/WheelOfTime.wot")
    helpers = _import_retail_helpers()
    (Package, Reader, _decode_font, decode_model, export_level, parse_typed,
     level_reader_after_actor_refs, skip_level_url, _identity_table,
     _decoded_defaults, PayloadReader, level_actor_refs,
     read_property_tags) = helpers
    package = Package(source)
    if package.name != MAP_NAME:
        raise DescriptorError("retail entry package identity is not WheelOfTime")
    with tempfile.TemporaryDirectory() as temporary:
        t3d_path = Path(temporary) / "WheelOfTime.t3d"
        system_dir = (dependency_root if dependency_root is not None
                      else source.parent.parent) / "System"
        export_level(source, t3d_path, system_dir if system_dir.is_dir() else None)
        actors = parse_typed(t3d_path)
        t3d_text = t3d_path.read_text(encoding="latin-1")
    semantic_actors = [
        {"class": actor["cls"], "identity": f"{MAP_NAME}.{actor['name']}"}
        for actor in actors
    ]
    starts = [actor for actor in actors if actor["cls"].casefold() == "playerstart"]
    if len(starts) != 1:
        raise DescriptorError("retail entry map does not have exactly one PlayerStart")
    actor_refs = level_actor_refs(package)
    nonzero_actor_refs = [reference for reference in actor_refs if reference > 0]
    if len(nonzero_actor_refs) != len(set(nonzero_actor_refs)):
        raise DescriptorError("retail Level actor registry has a duplicate nonzero ref")
    actor_set = set(nonzero_actor_refs)
    start_refs = [
        reference for reference in actor_refs
        if reference > 0
        and package.objname(package.exports[reference - 1]["cls"]) == "PlayerStart"
    ]
    if len(start_refs) != 1:
        raise DescriptorError("retail entry actor registry has no singular PlayerStart")
    lightning_refs = [
        reference for reference in actor_refs
        if reference > 0
        and package.objname(
            package.exports[reference - 1]["cls"]) == "RandomLightning"
    ]
    if len(lightning_refs) != 1:
        raise DescriptorError(
            "retail entry actor registry has no singular RandomLightning controller")
    thunder_overrides: list[dict[str, Any]] = []
    for tag in read_property_tags(
            PayloadReader(package, package.exports[lightning_refs[0] - 1])):
        if tag["name"] != "ThunderSounds":
            continue
        thunder_overrides.append({
            "array_index": tag["array_index"],
            "name": tag["name"],
            "type": tag["type"],
            "value": _decode_default_string(
                Reader, tag["value"],
                f"ThunderSounds[{tag['array_index']}]")
            if tag["type"] == 13 else None,
        })
    start_properties = PayloadReader(
        package, package.exports[start_refs[0] - 1]).properties()
    location_raw = start_properties.get("_structs", {}).get("Location", {}).get(0)
    rotation_raw = start_properties.get("_structs", {}).get("Rotation", {}).get(0)
    if (not isinstance(location_raw, bytes) or len(location_raw) != 12
            or not isinstance(rotation_raw, bytes) or len(rotation_raw) != 12):
        raise DescriptorError("retail PlayerStart camera properties are malformed")
    raw_location = struct.unpack("<3f", location_raw)
    raw_rotation = struct.unpack("<3i", rotation_raw)
    camera_location = [float(raw_location[0]), float(raw_location[1]),
                       float(raw_location[2]) + 42.0]
    camera_rotation = [value * (360.0 / 65536.0) for value in raw_rotation]
    direct: dict[tuple[str, str], dict[str, str]] = {}

    def add_direct(class_name: str, identity: str) -> None:
        for domain in _domains_for_resource_class(class_name):
            key = (domain, identity.casefold())
            if key in direct:
                continue
            direct[key] = {"domain": domain, "identity": identity, "kind": "resource"}

    for match in OBJECT_LITERAL.finditer(t3d_text):
        add_direct(match.group(1), match.group(2))
    allowed_internal_classes = {
        "class", "levelinfo", "level", "levelsummary", "model",
    }
    for actor_reference in nonzero_actor_refs:
        actor_export = package.exports[actor_reference - 1]
        actor_reader = PayloadReader(package, actor_export)
        for tag in read_property_tags(actor_reader):
            if tag["type"] != 5:
                continue
            value_reader = helpers[1](tag["value"])
            reference = value_reader.cindex()
            if value_reader.o != len(tag["value"]):
                raise DescriptorError("actor object property has trailing bytes")
            if reference == 0 or reference in actor_set:
                continue
            identity = package.objpath(reference)
            if reference > 0:
                class_name = package.objname(
                    package.exports[reference - 1]["cls"])
            else:
                class_name = package.imports[-reference - 1][1]
            try:
                add_direct(class_name, identity)
            except DescriptorError:
                if class_name.casefold() not in allowed_internal_classes:
                    raise DescriptorError(
                        f"internal reference {identity!r} has unclassified "
                        f"class provenance {class_name!r}")
    level_reader, _actor_refs = level_reader_after_actor_refs(package)
    skip_level_url(level_reader)
    model_reference = level_reader.cindex()
    if (model_reference <= 0 or model_reference > len(package.exports)
            or package.objname(package.exports[model_reference - 1]["cls"]) != "Model"):
        raise DescriptorError("retail entry map has no singular Level Model")
    model = decode_model(package, package.exports[model_reference - 1])
    for surface in model["surfs"]:
        reference = surface["tex"]
        if reference:
            identity = package.objpath(reference)
            if reference > 0:
                resource_class = package.objname(
                    package.exports[reference - 1]["cls"])
            else:
                resource_class = package.imports[-reference - 1][1]
            if not resource_class:
                raise DescriptorError(
                    f"surface resource {identity!r} has no declared class")
            try:
                add_direct(resource_class, identity)
            except DescriptorError:
                if resource_class.casefold() not in allowed_internal_classes:
                    raise DescriptorError(
                        f"surface reference {identity!r} has unclassified "
                        f"class provenance {resource_class!r}")
    references, thunder_authority = _complete_retail_resource_graph(
        source, list(direct.values()), helpers, thunder_overrides,
        dependency_root)
    fixture = {
        "actors": semantic_actors,
        "camera": {
            "horizontal_fov": 90,
            "location": camera_location,
            "rotation": camera_rotation,
        },
        "format": FIXTURE_FORMAT,
        "map_name": MAP_NAME,
        "references": references,
        "thunder_sound_source_exports": thunder_authority,
    }
    semantic = _validate_semantic_fixture(fixture)
    return {
        "actors": fixture["actors"],
        "camera": fixture["camera"],
        "census": {
            field: semantic["classes"][class_name]
            for field, class_name in CENSUS_CLASSES.items()
        },
        "format": FIXTURE_FORMAT,
        "map_name": MAP_NAME,
        "references": fixture["references"],
        "thunder_sound_source_exports": fixture[
            "thunder_sound_source_exports"],
    }


def _retail_source_root(retail_root: Path, source_paths: list[str]) -> str:
    hasher = hashlib.sha256()
    hasher.update(RETAIL_SOURCE_DOMAIN)
    for relative in sorted(source_paths):
        path = (retail_root / Path(relative)).resolve()
        try:
            path.relative_to(retail_root.resolve())
        except ValueError as exc:
            raise DescriptorError(f"contract source escapes retail root: {relative}") from exc
        raw = _read(path, "contract-authorized dependency", MAX_SOURCE_BYTES)
        encoded = relative.encode("utf-8")
        hasher.update(struct.pack("<Q", len(encoded)))
        hasher.update(encoded)
        hasher.update(hashlib.sha256(raw).digest())
    return hasher.hexdigest()


def build_descriptor_from_retail(retail_root: Path, retail_contract: Any
                                 ) -> dict[str, Any]:
    observed = inspect_retail_map(retail_root / MAP_SOURCE)
    semantic = _validate_semantic_fixture({
        key: observed[key]
        for key in ("actors", "camera", "format", "map_name", "references",
                    "thunder_sound_source_exports")
    })
    _bsp, _world, _resources, _logical_ids, source_paths = _close_contract(
        semantic["references"], retail_contract)
    return _descriptor_payload(
        semantic, retail_contract, _retail_source_root(retail_root, source_paths))


def _load_json(path: Path, what: str) -> tuple[Any, bytes]:
    raw = _read(path, what)
    try:
        value = json.loads(raw.decode("utf-8"))
    except (UnicodeError, json.JSONDecodeError) as exc:
        raise DescriptorError(f"{what} {path} is invalid JSON: {exc}") from exc
    return value, raw


def _identity_from_args(path: Path | None, digest: str | None, what: str) -> str:
    if (path is None) == (digest is None):
        raise DescriptorError(f"specify exactly one of --{what} or --{what}-sha256")
    return _sha256(_read(path, what)) if path is not None else _hex_digest(digest, what)


def _validate_output_boundary(output: Path, retail_root: Path | None) -> None:
    resolved = output.resolve()
    repo = Path(__file__).resolve().parents[2]
    content = (repo / "Content").resolve()
    try:
        resolved.relative_to(content)
    except ValueError:
        pass
    else:
        raise DescriptorError("front-end descriptor output must stay outside Content/")
    if retail_root is not None:
        try:
            resolved.relative_to(retail_root.resolve())
        except ValueError:
            pass
        else:
            raise DescriptorError("front-end descriptor output must stay outside retail input")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--fixture", type=Path,
                        help="synthetic semantic fixture used by tests")
    source.add_argument("--retail-root", type=Path,
                        help="developer-local extracted retail root")
    parser.add_argument("--contract", required=True, type=Path)
    installer = parser.add_mutually_exclusive_group(required=True)
    installer.add_argument("--installer", type=Path)
    installer.add_argument("--installer-sha256")
    production = parser.add_mutually_exclusive_group(required=True)
    production.add_argument("--production-descriptor", type=Path)
    production.add_argument("--production-descriptor-sha256")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args(argv)
    try:
        contract_value, contract_raw = _load_json(args.contract, "retail contract")
        if args.fixture is not None:
            fixture_value, _fixture_raw = _load_json(args.fixture, "semantic fixture")
            descriptor = build_descriptor_from_fixture(fixture_value, contract_value)
            retail_root = None
        else:
            retail_root = (args.retail_root.resolve() if args.retail_root
                           else original_dir())
            descriptor = build_descriptor_from_retail(retail_root, contract_value)
        bindings = {
            "contract_sha256": _sha256(contract_raw),
            "installer_sha256": _identity_from_args(
                args.installer, args.installer_sha256, "installer"),
            "production_descriptor_sha256": _identity_from_args(
                args.production_descriptor, args.production_descriptor_sha256,
                "production-descriptor"),
        }
        expected = write_sidecar(descriptor, bindings)
        _validate_output_boundary(args.output, retail_root)
        if args.check:
            if _read(args.output, "front-end descriptor output") != expected:
                raise DescriptorError(f"front-end descriptor is stale: {args.output}")
        else:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_bytes(expected)
    except (DescriptorError, OSError, ValueError) as exc:
        print(f"front-end entry descriptor: {exc}", file=sys.stderr)
        return 1
    print(f"front-end entry descriptor OK: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
