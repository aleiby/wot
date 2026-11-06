"""Create the four cooked mover-wave permutation parents.

Runtime movers bind transient MIDs, which cannot change material static-switch
permutations.  These tiny generated MICs inherit the existing world response
graphs with all six Delta slots compiled on; runtime binds black textures to
unused slots and immutable mover response atlases to used slots.
"""

from __future__ import annotations

import unreal


ROOT = "/Game/WoT/Materials"
VARIANTS = (
    ("MI_WoT_MoverWaveRuntime", "M_WoT_WorldWave"),
    ("MI_WoT_MoverWaveRuntimeMasked", "M_WoT_WorldWaveMasked"),
    ("MI_WoT_MoverWaveRuntimeTranslucent", "M_WoT_WorldWaveTranslucent"),
    ("MI_WoT_MoverWaveRuntimeModulated", "M_WoT_WorldWaveModulated"),
)


def _already_built(instance, parent, materials) -> bool:
    """True when an existing instance already has this parent and every
    UseDelta switch on. Such an instance is left untouched: resaving it would
    replace pinned neutral-support bytes (Tools/dist/neutral-support-assets-v1.json)
    with this machine's bytes and fail the package seed check."""
    current = instance.get_editor_property("parent")
    if current is None or current.get_path_name() != parent.get_path_name():
        return False
    return all(
        materials.get_material_instance_static_switch_parameter_value(
            instance, f"UseDelta{index}")
        for index in range(6))


def build() -> list[str]:
    """Create or repair the four instances; returns the paths it saved."""
    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    editor_assets = unreal.EditorAssetLibrary
    materials = unreal.MaterialEditingLibrary
    saved = []
    for instance_name, parent_name in VARIANTS:
        instance_path = f"{ROOT}/{instance_name}"
        parent = unreal.load_asset(f"{ROOT}/{parent_name}")
        if parent is None:
            raise RuntimeError(f"missing mover-wave parent {parent_name}")
        instance = unreal.load_asset(instance_path)
        if instance is None:
            instance = asset_tools.create_asset(
                instance_name,
                ROOT,
                unreal.MaterialInstanceConstant,
                unreal.MaterialInstanceConstantFactoryNew(),
            )
        elif _already_built(instance, parent, materials):
            continue
        instance.set_editor_property("parent", parent)
        for index in range(6):
            materials.set_material_instance_static_switch_parameter_value(
                instance, f"UseDelta{index}", True)
        if not editor_assets.save_asset(instance_path):
            raise RuntimeError(f"failed to save {instance_path}")
        saved.append(instance_path)
    return saved


if __name__ == "__main__":
    build()
