"""Shared stdlib plan and Unreal-editor reconcile core for fixed pickups.

The two callers deliberately keep their own stamp schemas and result-file
contracts.  This module owns the risky common mechanics: parse/filter a T3D,
key actors by their stable retail label, require both label and legacy mesh
before deleting a decoration, remove duplicate functional pickups, restamp,
and verify exactly one pickup per expected label.
"""


def plan_t3d(t3d_path, parse_t3d, typed_t3d, accepts, make_stamp):
    """Parse ``t3d_path`` and build stamps for actors accepted by the caller."""
    return [make_stamp(actor)
            for actor in typed_t3d(parse_t3d(t3d_path))
            if accepts(actor)]


def rotator(unreal, rotation):
    """Convert the UE1 65536-unit rotator to an Unreal Python rotator."""
    scale = 360.0 / 65536.0
    return unreal.Rotator(roll=rotation[2] * scale,
                          pitch=rotation[0] * scale,
                          yaw=rotation[1] * scale)


def mesh_name(actor):
    """Best-effort bound skeletal-mesh name for a legacy donor match."""
    try:
        component = actor.get_editor_property("mesh")
        try:
            mesh = component.get_skeletal_mesh_asset()
        except Exception:  # noqa: BLE001 - API differs across UE branches
            mesh = component.get_editor_property("skeletal_mesh_asset")
        return mesh.get_name() if mesh is not None else ""
    except Exception:  # noqa: BLE001 - unrelated same-label actors survive
        return ""


def is_legacy_decoration(unreal, actor, expected_label_meshes):
    """True only for an expected retail label bound to its expected mesh."""
    expected_mesh = expected_label_meshes.get(actor.get_actor_label())
    return (expected_mesh is not None
            and isinstance(actor, unreal.WoTDecoration)
            and mesh_name(actor).lower() == expected_mesh.lower())


def reconcile_map(unreal, eas, plan, runtime_classes, stamp_actor,
                  include_per_class=False):
    """Reconcile and verify functional pickups for one loaded map.

    Each stamp supplies ``label``, ``class_key``, and ``legacy_mesh``.
    ``runtime_classes[class_key]`` supplies the native ``base_class`` used to
    identify duplicates and the exact generated ``pickup_class`` to retain.
    """
    actors = list(eas.get_all_level_actors())
    expected_label_meshes = {
        stamp["label"]: stamp["legacy_mesh"] for stamp in plan
    }
    counts = {
        "planned": len(plan),
        "restamped": 0,
        "newly_placed": 0,
        "legacy_removed": 0,
        "duplicate_pickups_removed": 0,
    }
    per_class = {}
    for stamp in plan:
        class_counts = per_class.setdefault(stamp["class_key"], {
            "planned": 0,
            "restamped": 0,
            "newly_placed": 0,
            "legacy_removed": 0,
            "duplicate_pickups_removed": 0,
        })
        class_counts["planned"] += 1
        runtime = runtime_classes[stamp["class_key"]]
        base_class = runtime["base_class"]
        pickup_class = runtime["pickup_class"]
        matches = [actor for actor in actors
                   if actor.get_actor_label() == stamp["label"]]
        pickups = [actor for actor in matches
                   if isinstance(actor, base_class)]
        pickup = next((actor for actor in pickups
                       if actor.get_class() == pickup_class), None)
        for duplicate in pickups:
            if duplicate is pickup:
                continue
            eas.destroy_actor(duplicate)
            actors.remove(duplicate)
            counts["duplicate_pickups_removed"] += 1
            class_counts["duplicate_pickups_removed"] += 1

        for donor in list(matches):
            if donor is pickup or donor not in actors:
                continue
            if is_legacy_decoration(unreal, donor, expected_label_meshes):
                eas.destroy_actor(donor)
                actors.remove(donor)
                counts["legacy_removed"] += 1
                class_counts["legacy_removed"] += 1

        location = unreal.Vector(*stamp["location"])
        if pickup is None:
            pickup = eas.spawn_actor_from_class(pickup_class, location)
            if pickup is None:
                raise RuntimeError(f"spawn failed for {stamp['label']}")
            pickup.set_actor_label(stamp["label"])
            actors.append(pickup)
            counts["newly_placed"] += 1
            class_counts["newly_placed"] += 1
        else:
            counts["restamped"] += 1
            class_counts["restamped"] += 1

        pickup.set_actor_location(location, False, False)
        pickup.set_actor_rotation(rotator(unreal, stamp["rotation"]), False)
        stamp_actor(pickup, stamp)

    after = list(eas.get_all_level_actors())
    per_label = {stamp["label"]: 0 for stamp in plan}
    for stamp in plan:
        runtime = runtime_classes[stamp["class_key"]]
        present = [actor for actor in after
                   if actor.get_class() == runtime["pickup_class"]
                   and actor.get_actor_label() == stamp["label"]]
        per_label[stamp["label"]] = len(present)
        class_counts = per_class[stamp["class_key"]]
        class_counts["verified_pickups"] = (
            class_counts.get("verified_pickups", 0) + len(present))

    legacy = [actor for actor in after
              if is_legacy_decoration(unreal, actor,
                                      expected_label_meshes)]
    for actor in legacy:
        label = actor.get_actor_label()
        stamp = next(item for item in plan if item["label"] == label)
        class_counts = per_class[stamp["class_key"]]
        class_counts["legacy_remaining"] = (
            class_counts.get("legacy_remaining", 0) + 1)
    for class_counts in per_class.values():
        class_counts.setdefault("verified_pickups", 0)
        class_counts.setdefault("legacy_remaining", 0)
    for stamp in plan:
        if per_label[stamp["label"]] != 1:
            per_class[stamp["class_key"]].setdefault("bad_labels", []).append(
                stamp["label"])
    for class_counts in per_class.values():
        class_counts.setdefault("bad_labels", [])

    counts["verified_pickups"] = sum(per_label.values())
    counts["legacy_remaining"] = len(legacy)
    counts["bad_labels"] = sorted(
        label for label, count in per_label.items() if count != 1)
    if include_per_class:
        counts["classes"] = per_class
    return counts
