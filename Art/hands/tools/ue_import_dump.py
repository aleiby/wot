"""UE 5.8 headless: import a .glb with the default (Interchange) importer, then dump every
bone's GLOBAL reference transform (location in cm, rotation axes) and parent to JSON.
Nothing is saved to disk. Needs MeshModelingToolset (pass -EnablePlugins=MeshModelingToolset).
usage (UnrealEditor-Cmd ... -run=pythonscript): -script="ue_import_dump.py <in.glb> <out.json> <dest>"
"""
import json
import sys
import traceback

import unreal

src, out, dest = sys.argv[1], sys.argv[2], sys.argv[3]
result = {"src": src, "dest": dest}
try:
    task = unreal.AssetImportTask()
    task.filename = src
    task.destination_path = dest
    task.automated = True
    task.replace_existing = True
    task.save = False
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    result["imported"] = list(task.imported_object_paths)
    meshes = []
    for path in unreal.EditorAssetLibrary.list_assets(dest, recursive=True):
        asset = unreal.EditorAssetLibrary.load_asset(path)
        result.setdefault("assets", []).append(f"{asset.get_class().get_name()} {path}")
        if isinstance(asset, unreal.SkeletalMesh):
            meshes.append(asset)
    mesh = meshes[0]
    modifier = unreal.new_object(unreal.SkeletonModifier)
    result["set_mesh"] = bool(modifier.set_skeletal_mesh(mesh))
    bones = []
    for name in modifier.get_all_bone_names():
        t = modifier.get_bone_transform(name, True)
        q = t.rotation

        def vec(v):
            return [round(v.x, 5), round(v.y, 5), round(v.z, 5)]
        bones.append({"name": str(name), "parent": str(modifier.get_parent_name(name)),
                      "location": vec(t.translation), "axis_x": vec(q.get_axis_x()),
                      "axis_y": vec(q.get_axis_y()), "axis_z": vec(q.get_axis_z()),
                      "scale": vec(t.scale3d)})
    result["bones"] = bones
    bounds = mesh.get_bounds()
    result["bounds_origin"] = [round(bounds.origin.x, 3), round(bounds.origin.y, 3), round(bounds.origin.z, 3)]
    result["bounds_extent"] = [round(bounds.box_extent.x, 3), round(bounds.box_extent.y, 3), round(bounds.box_extent.z, 3)]
    result["ok"] = True
except Exception:
    result["ok"] = False
    result["error"] = traceback.format_exc()
with open(out, "w") as handle:
    json.dump(result, handle, indent=1)
