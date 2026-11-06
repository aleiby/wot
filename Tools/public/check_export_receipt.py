#!/usr/bin/env python3
"""Check that a public commit's tree is exactly what its export receipt records.

The public repository carries PUBLIC_EXPORT_RECEIPT.json, written by
export_public_tree.py. This check reads the committed tree (never the working
tree, so checkout line-ending conversion cannot affect it) and requires:

- every receipt file is in the tree, with the receipt's mode and sha256;
- the tree holds no file the receipt does not list, apart from the receipt;
- the receipt's file_count matches its file list.

A commit built on a filesystem without executable bits records every file as
100644 although the receipt says 100755; this check fails that commit.

Contributor commits change files without touching the receipt, so with
``--export-commits-only`` (what CI passes) a commit is checked only when it is
a root commit or changes the receipt, i.e. when it is an export sync.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path
from typing import Dict, List, Tuple

RECEIPT = "PUBLIC_EXPORT_RECEIPT.json"


class ReceiptError(Exception):
    """The receipt cannot be read."""


def _git(repo: Path, *args: str, data: bytes = None) -> bytes:
    return subprocess.run(["git", "-C", str(repo), *args], input=data,
                          capture_output=True, check=True).stdout


def committed_tree(repo: Path, rev: str) -> Dict[str, Tuple[str, str]]:
    """Map each file path in ``rev`` to its (mode, blob id)."""
    entries: Dict[str, Tuple[str, str]] = {}
    for item in _git(repo, "ls-tree", "-r", "-z", rev).split(b"\0"):
        if not item:
            continue
        meta, path = item.split(b"\t", 1)
        mode, kind, blob = meta.decode("ascii").split()
        if kind == "blob":
            entries[path.decode("utf-8")] = (mode, blob)
    return entries


def blob_sha256(repo: Path, blobs: List[str]) -> Dict[str, str]:
    """SHA-256 of each blob's committed bytes, read in one git process."""
    request = "".join(blob + "\n" for blob in blobs).encode("ascii")
    out = _git(repo, "cat-file", "--batch", data=request)
    digests: Dict[str, str] = {}
    offset = 0
    for blob in blobs:
        end = out.index(b"\n", offset)
        header = out[offset:end].decode("ascii").split()
        if len(header) != 3 or header[0] != blob:
            raise ReceiptError(f"unexpected git cat-file header for {blob}")
        size = int(header[2])
        start = end + 1
        digests[blob] = hashlib.sha256(out[start:start + size]).hexdigest()
        offset = start + size + 1
    return digests


def is_export_commit(repo: Path, rev: str = "HEAD") -> bool:
    """True for a root commit or one whose receipt differs from every parent's."""
    parents = _git(repo, "rev-list", "--parents", "-n", "1", rev).decode("ascii").split()[1:]
    if not parents:
        return True

    def receipt_blob(commit: str) -> str:
        listed = _git(repo, "ls-tree", commit, "--", RECEIPT).decode("utf-8").split()
        return listed[2] if len(listed) >= 3 else ""

    mine = receipt_blob(rev)
    return all(receipt_blob(parent) != mine for parent in parents)


def check(repo: Path, rev: str = "HEAD") -> List[str]:
    """Return every difference between ``rev``'s tree and its receipt."""
    tree = committed_tree(repo, rev)
    if RECEIPT not in tree:
        return [f"{RECEIPT} is not in the tree"]
    try:
        receipt = json.loads(_git(repo, "cat-file", "blob", tree[RECEIPT][1]))
        files = receipt["files"]
        count = receipt["file_count"]
        recorded = {entry["path"]: (entry["mode"], entry["sha256"]) for entry in files}
    except (ValueError, KeyError, TypeError) as exc:
        raise ReceiptError(f"{RECEIPT} is unreadable: {exc}") from exc
    problems: List[str] = []
    if count != len(files) or len(recorded) != len(files):
        problems.append(f"{RECEIPT} file_count {count} does not match its "
                        f"{len(files)} entries ({len(recorded)} distinct paths)")
    present = [path for path in recorded if path in tree]
    digests = blob_sha256(repo, sorted({tree[path][1] for path in present}))
    for path, (mode, sha256) in sorted(recorded.items()):
        if path not in tree:
            problems.append(f"{path}: listed in the receipt but missing from the tree")
            continue
        actual_mode, blob = tree[path]
        if actual_mode != mode:
            problems.append(f"{path}: mode {actual_mode} in the tree, {mode} in the receipt")
        if digests[blob] != sha256:
            problems.append(f"{path}: content sha256 differs from the receipt")
    for path in sorted(set(tree) - set(recorded) - {RECEIPT}):
        problems.append(f"{path}: in the tree but not in the receipt")
    return problems


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--tree", default=".", help="the public Git checkout")
    parser.add_argument("--rev", default="HEAD", help="the commit to check")
    parser.add_argument("--export-commits-only", action="store_true",
                        help="skip commits that neither are a root nor change the receipt")
    args = parser.parse_args(argv)
    try:
        if args.export_commits_only and not is_export_commit(Path(args.tree), args.rev):
            print("check_export_receipt: not an export commit (the receipt is "
                  "unchanged); skipped")
            return 0
        problems = check(Path(args.tree), args.rev)
    except (ReceiptError, subprocess.CalledProcessError) as exc:
        print(f"check_export_receipt: {exc}", file=sys.stderr)
        return 2
    for problem in problems:
        print(f"FAIL: {problem}", file=sys.stderr)
    if problems:
        print(f"check_export_receipt: {len(problems)} difference(s)", file=sys.stderr)
        return 1
    print("check_export_receipt: the tree matches its export receipt")
    return 0


if __name__ == "__main__":
    sys.exit(main())
