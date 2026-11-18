#!/usr/bin/env python3
"""Meshy image-to-3D with provenance: submit one image, poll to a terminal state,
download the GLB(s), base-colour texture and thumbnails, and write provenance.json.

The API key is read from $MESHY_API_KEY or ~/.config/meshy/api_key; it is never printed.
usage: meshy_i23d.py --image in.png --out DIR [--polycount 3000] [--label NAME]
"""
import argparse
import base64
import json
import os
import pathlib
import sys
import time
import urllib.error
import urllib.request

API = "https://api.meshy.ai/openapi/v1/image-to-3d"


def api_key():
    key = os.environ.get("MESHY_API_KEY")
    if not key:
        key = pathlib.Path.home().joinpath(".config/meshy/api_key").read_text().strip()
    return key


def call(method, url, body=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method=method, headers={
        "Authorization": f"Bearer {api_key()}", "Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.loads(r.read())


def fetch(url, dest):
    with urllib.request.urlopen(url, timeout=300) as r, open(dest, "wb") as f:
        f.write(r.read())
    return dest


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--image", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--polycount", type=int, default=3000)
    p.add_argument("--label", default="hand")
    a = p.parse_args()
    out = pathlib.Path(a.out)
    out.mkdir(parents=True, exist_ok=False)
    image = pathlib.Path(a.image)
    mime = "image/png" if image.suffix.lower() == ".png" else "image/jpeg"
    params = {
        "ai_model": "latest",
        "should_texture": True,
        "enable_pbr": False,
        "texture_resolution": "2k",
        "remove_lighting": True,
        "should_remesh": True,
        "topology": "triangle",
        "target_polycount": a.polycount,
        "save_pre_remeshed_model": True,
        "target_formats": ["glb"],
        "multi_view_thumbnails": True,
    }
    body = dict(params, image_url=f"data:{mime};base64,{base64.b64encode(image.read_bytes()).decode()}")
    task_id = call("POST", API, body)["result"]
    print(f"[meshy] {a.label}: task {task_id}", flush=True)
    started = time.time()
    while True:
        task = call("GET", f"{API}/{task_id}")
        status = task.get("status")
        if status in ("SUCCEEDED", "FAILED", "CANCELED"):
            break
        if time.time() - started > 3600:
            print(f"[meshy] {a.label}: still {status} after 1 h; giving up polling", flush=True)
            sys.exit(2)
        time.sleep(15)
    files = {}
    if status == "SUCCEEDED":
        for fmt, url in (task.get("model_urls") or {}).items():
            if url:
                files[fmt] = str(fetch(url, out / f"{a.label}-{fmt}.glb"))
        for i, tex in enumerate(task.get("texture_urls") or []):
            for kind, url in tex.items():
                if url:
                    files[f"texture{i}_{kind}"] = str(fetch(url, out / f"{a.label}-tex{i}-{kind}.png"))
        thumbs = task.get("thumbnail_urls") or {}
        views = thumbs.items() if isinstance(thumbs, dict) else enumerate(thumbs)
        for view, url in views:
            if url:
                files[f"thumb_{view}"] = str(fetch(url, out / f"{a.label}-thumb-{view}.png"))
        if task.get("thumbnail_url"):
            files["thumb"] = str(fetch(task["thumbnail_url"], out / f"{a.label}-thumb.png"))
    provenance = {
        "tool": "meshy image-to-3d", "endpoint": API, "task_id": task_id, "status": status,
        "params": params, "input_image": image.name, "consumed_credits": task.get("consumed_credits"),
        "created_at": task.get("created_at"), "finished_at": task.get("finished_at"),
        "task_error": task.get("task_error"), "files": files,
    }
    (out / "provenance.json").write_text(json.dumps(provenance, indent=2) + "\n")
    print(f"[meshy] {a.label}: {status}, credits {task.get('consumed_credits')}, {len(files)} files -> {out}", flush=True)
    sys.exit(0 if status == "SUCCEEDED" else 1)


if __name__ == "__main__":
    main()
