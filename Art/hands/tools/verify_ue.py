#!/usr/bin/env python3
"""Compare a UE Interchange import (ue_import_dump.py output) with the authored rig.
usage: verify_ue.py <class dir with joints.json, rig-report.json, ue_result.json> -> ue-verification.json"""
import json
import pathlib
import sys

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import hand_rig as hr  # noqa: E402

d = pathlib.Path(sys.argv[1])
r = json.loads((d / "ue_result.json").read_text())
J = {k: np.array(v) for k, v in json.loads((d / "joints.json").read_text()).items()}
rig = json.loads((d / "rig-report.json").read_text())
R = hr.bone_frames(J)
want_parent = {n: (p or "None") for n, p in hr.BONES}
rows, worst_pos, worst_ax, parents_ok = [], 0.0, 0.0, True
for b in r["bones"]:
    n = b["name"]
    dp = float(np.linalg.norm(np.array(b["location"]) - J[n]))
    dx = float(np.degrees(np.arccos(np.clip(np.dot(b["axis_x"], R[n][:, 0]), -1, 1))))
    dz = float(np.degrees(np.arccos(np.clip(np.dot(b["axis_z"], R[n][:, 2]), -1, 1))))
    ok_parent = want_parent[n] == b["parent"]
    parents_ok &= ok_parent
    worst_pos, worst_ax = max(worst_pos, dp), max(worst_ax, dx, dz)
    rows.append({"bone": n, "parent": b["parent"], "location_cm": b["location"], "axis_x": b["axis_x"],
                 "axis_z": b["axis_z"], "pos_err_cm": round(dp, 4), "axis_err_deg": round(max(dx, dz), 3)})
names = [b["name"] for b in r["bones"]]
mid = np.array(next(b["location"] for b in r["bones"] if b["name"] == "middle_tip_r"))
wrist = np.array(next(b["location"] for b in r["bones"] if b["name"] == "wrist_r"))
report = {
    "importer": "UE 5.8 Interchange glTF, default settings (headless)",
    "bones": len(names), "names_match_contract": sorted(names) == sorted(n for n, _ in hr.BONES),
    "parents_match_contract": parents_ok, "worst_position_error_cm": round(worst_pos, 4),
    "worst_axis_error_deg": round(worst_ax, 3), "wrist_at_origin": bool(np.linalg.norm(wrist) < 1e-3),
    "wrist_to_middle_tip_cm": round(float(np.linalg.norm(mid - wrist)), 2),
    "triangles": rig["triangles"], "max_influences": rig["max_influences"],
    "unweighted_bones": sorted(set(names) - set(rig["weighted_bones"])), "bones_detail": rows,
}
(d / "ue-verification.json").write_text(json.dumps(report, indent=1) + "\n")
print(d.name, {k: report[k] for k in ("bones", "names_match_contract", "parents_match_contract", "worst_position_error_cm",
                                        "worst_axis_error_deg", "wrist_at_origin", "wrist_to_middle_tip_cm", "triangles", "max_influences")},
      "unweighted:", report["unweighted_bones"])
