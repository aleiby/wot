"""One sampling policy for every texture the import pipeline writes.

The gamma-space pipeline (import_map.py GAMMA_SPACE) samples world and effect
textures raw: their bytes are display values and every master reads them
through a Linear Color sampler. Release builds build these textures at run time
with explicit options (srgb off for world and effect textures, on for HUD,
Slate/browser and font textures; Source/WoT/Assets/WoTRuntimeAssets.h), but
editor builds use the imported assets as saved. So every importer must save
the setting release uses, for new assets AND for the existing assets it reuses.
A PNG import defaults to srgb on, which decodes a modulated sprite's neutral
grey (byte 128) to about 0.21 instead of 0.5: the 2x modulate then darkens the
whole sprite square.

Every texture import or load site in Tools/import (and the child-sky stage)
acquires its texture here, naming its family explicitly:

    texture, changed = import_texture(unreal, png, asset_path, EFFECT)
    texture, changed = load_texture(unreal, asset_path, WORLD)
    texture = verify_texture_sampling(unreal, asset_path, WORLD)

test_texture_sampling.py enforces that with an AST check. Each importer also
declares the assets it owns as ``texture_sampling_claims``;
converge_texture_sampling.py applies those claims to existing content and scans
every Texture2D under /Game/WoT.

Stdlib only: engine callers pass the already-imported ``unreal`` module.
"""
from __future__ import annotations

from pathlib import Path
from typing import Dict, Iterable, List, NamedTuple, Optional, Tuple


WORLD = "world"
EFFECT = "effect"
UI = "ui"
LIGHTMAP = "lightmap"
LIGHTMAP_RESPONSE = "lightmap-response"

# family -> what it covers and its rule. "Raw in gamma space" is srgb off in the
# live gamma-space pipeline (GAMMA_SPACE = True in import_map.py, the default
# below) and srgb on in the retired linear one, exactly as import_map.py gated
# its own world textures and light maps.
FAMILIES = {
    WORLD: "BSP, mover, mesh skin, decoration, corona, rain and "
           "procedural-frame textures: raw in gamma space",
    EFFECT: "particle, sprite, decal and gore textures: raw in gamma space",
    UI: "HUD, UWindow/Slate and font atlas textures: always sRGB",
    LIGHTMAP: "composed light-map atlases and legacy hi/lo endpoints: raw in "
              "gamma space (import_map.py: srgb = not GAMMA_SPACE)",
    LIGHTMAP_RESPONSE: "animated light-map response atlases: always raw",
}


class TextureSamplingError(RuntimeError):
    """A texture is missing, is not a texture, or cannot take its policy."""


def desired_srgb(family, *, gamma_space=True):
    """Return the srgb flag a texture of ``family`` must be saved with.

    UI textures are always sRGB and response atlases always raw. World, effect
    and light-map textures are raw in the gamma-space pipeline; in the linear
    one an sRGB encode/decode pair reproduced UE1's display-space product, so
    they were sRGB there. import_map.py passes its GAMMA_SPACE explicitly.
    """
    if family == UI:
        return True
    if family == LIGHTMAP_RESPONSE:
        return False
    if family in (WORLD, EFFECT, LIGHTMAP):
        if not isinstance(gamma_space, bool):
            raise TextureSamplingError(
                f"gamma_space must be True or False, not {gamma_space!r}")
        return not gamma_space
    raise TextureSamplingError(f"unknown texture sampling family {family!r}")


def asset_package_path(texture_or_path):
    """'/Game/A/B.B', '/Game/A/B' or a loaded object -> '/Game/A/B'."""
    if isinstance(texture_or_path, str):
        path = texture_or_path
    else:
        path = texture_or_path.get_path_name()
    path = str(path).strip()
    tail = path.rsplit("/", 1)[-1]
    if "." in tail:
        path = path[:len(path) - len(tail)] + tail.split(".", 1)[0]
    return path


def _texture_class(unreal):
    texture_class = getattr(unreal, "Texture2D", None)
    return texture_class if isinstance(texture_class, type) else None


def _checked(unreal, texture, asset_path):
    if texture is None:
        raise TextureSamplingError(f"texture asset is missing: {asset_path}")
    texture_class = _texture_class(unreal)
    if texture_class is not None and not isinstance(texture, texture_class):
        raise TextureSamplingError(f"{asset_path} is not a Texture2D")
    return texture


def _load(unreal, asset_path):
    return _checked(unreal, unreal.load_asset(asset_path), asset_path)


def _resolve(unreal, texture_or_path):
    if isinstance(texture_or_path, str):
        path = asset_package_path(texture_or_path)
        return _load(unreal, path), path
    texture = _checked(unreal, texture_or_path, "<texture object>")
    return texture, asset_package_path(texture)


def current_srgb(texture, asset_path="<texture>"):
    """The texture's saved/in-memory srgb flag."""
    try:
        value = texture.get_editor_property("srgb")
    except Exception as exc:  # noqa: BLE001 - engine raises several types
        raise TextureSamplingError(
            f"{asset_path} has no srgb property: {exc}") from exc
    return bool(value)


def _save(unreal, asset_path):
    if unreal.EditorAssetLibrary.save_asset(asset_path) is False:
        raise TextureSamplingError(f"failed to save {asset_path}")


def _converge(unreal, texture, asset_path, want, save):
    """Set srgb to ``want``; True when it changed. Saves only on a change."""
    if current_srgb(texture, asset_path) == want:
        return False
    texture.set_editor_property("srgb", want)
    if current_srgb(texture, asset_path) != want:
        raise TextureSamplingError(f"{asset_path}: srgb={want} did not take")
    if save:
        _save(unreal, asset_path)
    return True


def ensure_texture_sampling(unreal, texture_or_path, family, *,
                            gamma_space=True, save=True):
    """Converge one texture to its family's policy; ``(texture, changed)``.

    Idempotent: an already-correct texture is neither modified nor saved.
    With ``save=False`` the caller owns the save (``changed`` says if needed).
    """
    want = desired_srgb(family, gamma_space=gamma_space)
    texture, path = _resolve(unreal, texture_or_path)
    return texture, _converge(unreal, texture, path, want, save)


def verify_texture_sampling(unreal, texture_or_path, family, *,
                            gamma_space=True):
    """Return the texture, or raise if it does not sample as ``family``.

    For consumers that only read a texture another importer owns (material
    builders binding T_WoT_Neutral as a sampler default): never writes.
    """
    want = desired_srgb(family, gamma_space=gamma_space)
    texture, path = _resolve(unreal, texture_or_path)
    have = current_srgb(texture, path)
    if have != want:
        raise TextureSamplingError(
            f"{path} has srgb={have}; the {family} family requires srgb={want}. "
            "Rerun its importer or Tools/import/converge_texture_sampling.py.")
    return texture


def load_texture(unreal, asset_path, family, *, gamma_space=True, save=True):
    """Load an existing texture and converge it; ``(texture, changed)``.

    A missing asset raises: callers that tolerate one check existence first.
    """
    return ensure_texture_sampling(
        unreal, asset_package_path(asset_path), family,
        gamma_space=gamma_space, save=save)


def import_texture(unreal, source_png, asset_path, family, *,
                   gamma_space=True, save=True, replace_existing=False,
                   replace_existing_settings=None):
    """Acquire ``asset_path`` from ``source_png`` under ``family``'s policy.

    A missing asset (or any asset when ``replace_existing``) is imported from
    the PNG; an existing one is loaded as is. Either way the family policy is
    applied before returning ``(texture, changed)``, so the early-return path
    for existing assets converges too. ``changed`` is True when the asset was
    imported or its sampling changed. With ``save=True`` the asset is saved
    once, only when changed; with ``save=False`` the caller saves.
    """
    want = desired_srgb(family, gamma_space=gamma_space)
    asset_path = asset_package_path(asset_path)
    destination, asset_name = asset_path.rsplit("/", 1)
    imported = False
    if replace_existing or not unreal.EditorAssetLibrary.does_asset_exist(asset_path):
        source = Path(source_png)
        if not source.is_file():
            raise TextureSamplingError(
                f"texture source PNG is missing: {source} (for {asset_path})")
        task = unreal.AssetImportTask()
        task.filename = str(source)
        task.destination_path = destination
        task.destination_name = asset_name
        task.automated = True
        task.save = False
        if replace_existing:
            task.replace_existing = True
        if replace_existing_settings is not None:
            task.replace_existing_settings = bool(replace_existing_settings)
        unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
        imported = True
    texture = _load(unreal, asset_path)
    changed = _converge(unreal, texture, asset_path, want, save=False)
    if save and (imported or changed):
        _save(unreal, asset_path)
    return texture, imported or changed


def replace_texture_source(unreal, source_png, asset_path):
    """Swap a texture's source pixels, keeping its settings and sampling.

    The stripped payload finalizer replaces every payload-bound source with a
    placeholder long after the owning importer applied the family policy. It
    cannot know the family, so it keeps exactly the srgb flag the owner saved:
    captured before the reimport and restored (then verified) after it. The
    caller saves.
    """
    asset_path = asset_package_path(asset_path)
    want = current_srgb(_load(unreal, asset_path), asset_path)
    source = Path(source_png)
    if not source.is_file():
        raise TextureSamplingError(
            f"texture source PNG is missing: {source} (for {asset_path})")
    destination, asset_name = asset_path.rsplit("/", 1)
    task = unreal.AssetImportTask()
    task.filename = str(source)
    task.destination_path = destination
    task.destination_name = asset_name
    task.automated = True
    task.save = False
    task.replace_existing = True
    task.replace_existing_settings = False
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    texture = _load(unreal, asset_path)
    _converge(unreal, texture, asset_path, want, save=False)
    return texture


# ---------------------------------------------------------------------------
# Ownership: importer manifests -> claims -> convergence and scan.

class ClaimContext(NamedTuple):
    """What an importer's ``texture_sampling_claims`` may read: the converted
    tree its manifests live in, and the editor (only for claims that name
    existing assets, such as the font atlas pages)."""
    conv_root: Path
    unreal: object = None
    maps: Tuple[str, ...] = ()


class TextureClaim(NamedTuple):
    """One importer's declaration that it acquires ``asset_path`` as ``family``.

    ``blanket`` marks a whole-package sweep (import_map.py's code-package skin
    pass) rather than a named consumer; a named claim wins over a blanket one,
    so HUD textures that sit beside world textures keep their UI policy.
    """
    asset_path: str
    family: str
    owner: str
    blanket: bool = False
    gamma_space: bool = True

    @property
    def srgb(self):
        return desired_srgb(self.family, gamma_space=self.gamma_space)


def claim(asset_path, family, owner, *, blanket=False, gamma_space=True):
    """Build a validated claim (the family must resolve to a setting)."""
    desired_srgb(family, gamma_space=gamma_space)
    return TextureClaim(asset_package_path(asset_path), family, owner,
                        bool(blanket), gamma_space)


class TextureOwnership(NamedTuple):
    """The resolved policy of one asset across every claim on it."""
    asset_path: str
    srgb: Optional[bool]          # None when the deciding claims conflict
    families: Tuple[str, ...]
    owners: Tuple[str, ...]
    blanket: bool
    conflicts: Tuple[TextureClaim, ...] = ()

    @property
    def family(self):
        return "+".join(self.families)


def resolve_claims(claims):
    """Map ``asset_path.casefold()`` to its ``TextureOwnership``.

    Named claims decide over blanket ones; the deciding claims must agree on
    the srgb setting (family names may differ: world and effect are both raw).
    A disagreement is a conflict that convergence reports and never writes:
    one source serving world and UI consumers needs two assets.
    """
    grouped: Dict[str, List[TextureClaim]] = {}
    for item in claims:
        grouped.setdefault(item.asset_path.casefold(), []).append(item)
    result = {}
    for key, group in grouped.items():
        named = [item for item in group if not item.blanket]
        deciding = named or group
        settings = sorted({item.srgb for item in deciding})
        consistent = len(settings) == 1
        result[key] = TextureOwnership(
            asset_path=sorted({item.asset_path for item in deciding})[0],
            srgb=settings[0] if consistent else None,
            families=tuple(sorted({item.family for item in deciding})),
            owners=tuple(sorted({item.owner for item in deciding})),
            blanket=not named,
            conflicts=() if consistent else tuple(
                sorted(set(deciding), key=_claim_order)),
        )
    return result


def _claim_order(item):
    return (item.family, item.owner, item.blanket, str(item.gamma_space),
            item.asset_path)


def converge_claims(unreal, claims, *, apply=True):
    """Apply every resolved claim to the existing assets (srgb flags only).

    Never imports, creates, deletes or moves anything. Returns a report with
    the changed assets (path, before, after, family, owners), the claimed
    assets that do not exist, the conflicts and the unchanged count.
    """
    eal = unreal.EditorAssetLibrary
    report = {"changed": [], "missing": [], "conflicts": [], "unchanged": 0,
              "applied": bool(apply)}
    owned = resolve_claims(claims)
    for key in sorted(owned):
        ownership = owned[key]
        path = ownership.asset_path
        if ownership.conflicts:
            report["conflicts"].append({
                "asset": path,
                "claims": [
                    {"family": item.family, "owner": item.owner,
                     "srgb": item.srgb, "blanket": item.blanket}
                    for item in ownership.conflicts],
            })
            continue
        if not eal.does_asset_exist(path):
            report["missing"].append({
                "asset": path, "family": ownership.family,
                "owners": list(ownership.owners)})
            continue
        texture = _load(unreal, path)
        before = current_srgb(texture, path)
        if before == ownership.srgb:
            report["unchanged"] += 1
            continue
        if apply:
            _converge(unreal, texture, path, ownership.srgb, save=True)
        report["changed"].append({
            "asset": path, "before": before, "after": ownership.srgb,
            "family": ownership.family, "owners": list(ownership.owners)})
    return report


def scan_textures(unreal, claims, *, root="/Game/WoT"):
    """Report every Texture2D under ``root``: family, srgb and status.

    Status is ``ok`` / ``mismatch`` for owned assets, ``conflict`` when the
    deciding claims disagree and ``unowned`` when no importer claims it.
    """
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    registry.scan_paths_synchronous([root], True)
    asset_filter = unreal.ARFilter(
        package_paths=[unreal.Name(root)],
        class_paths=[unreal.TopLevelAssetPath("/Script/Engine", "Texture2D")],
        recursive_paths=True,
        recursive_classes=True,
    )
    owned = resolve_claims(claims)
    rows = []
    for asset_data in sorted(registry.get_assets(asset_filter) or (),
                             key=lambda item: str(item.package_name).casefold()):
        path = str(asset_data.package_name)
        texture = _checked(unreal, asset_data.get_asset(), path)
        srgb = current_srgb(texture, path)
        ownership = owned.get(path.casefold())
        if ownership is None:
            status, family, owners, expected = "unowned", "unowned", [], None
        else:
            family, owners = ownership.family, list(ownership.owners)
            expected = ownership.srgb
            status = ("conflict" if ownership.conflicts
                      else "ok" if srgb == expected else "mismatch")
        rows.append({"asset": path, "family": family, "owners": owners,
                     "srgb": srgb, "expected": expected, "status": status})
    return rows


def scan_totals(rows):
    totals = {"total": len(rows), "ok": 0, "mismatch": 0, "conflict": 0,
              "unowned": 0}
    for row in rows:
        totals[row["status"]] += 1
    return totals


def unique_claims(claims: Iterable[TextureClaim]):
    """Drop exact duplicates while keeping first-seen order."""
    seen = set()
    result = []
    for item in claims:
        if item not in seen:
            seen.add(item)
            result.append(item)
    return result
