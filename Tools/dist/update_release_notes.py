#!/usr/bin/env python3
"""Edit one entry in the latest cumulative release-notes asset.

Read-only dry run by default. ``--publish`` clobbers the latest release's
unsigned notes asset and updates the marked What's new section of the release
whose generated identity line names the requested release sequence. Only that
one entry changes: an edit that no longer fits the asset is refused, never
trimmed.
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Callable, Optional, Sequence

sys.path.insert(0, str(Path(__file__).resolve().parent))
import release_formats as formats  # noqa: E402


WHATS_NEW_START = "<!-- wot-whats-new:start -->"
WHATS_NEW_END = "<!-- wot-whats-new:end -->"
# gh pages internally up to this many releases. A listing that fills it may be
# truncated, so the edit refuses rather than search a partial history.
RELEASE_LIST_LIMIT = 10000
Runner = Callable[[Sequence[str]], subprocess.CompletedProcess]


class EditError(Exception):
    pass


def run_command(command: Sequence[str]) -> subprocess.CompletedProcess:
    return subprocess.run(list(command), capture_output=True, text=True)


def checked(runner: Runner, command: Sequence[str], what: str
            ) -> subprocess.CompletedProcess:
    result = runner(command)
    if result.returncode != 0:
        raise EditError(f"{what} failed: "
                        f"{(result.stderr or result.stdout or '').strip()}")
    return result


def marked_body(body: str, text: str) -> str:
    if body.count(WHATS_NEW_START) != 1 or body.count(WHATS_NEW_END) != 1:
        raise EditError("target release body does not have one What's new marker pair")
    start = body.index(WHATS_NEW_START)
    end = body.index(WHATS_NEW_END, start + len(WHATS_NEW_START))
    if end <= start:
        raise EditError("target release body's What's new markers are reversed")
    return (body[:start] + WHATS_NEW_START + "\n" + text + "\n"
            + body[end:])


def names_release_sequence(body: str, release_sequence: int) -> bool:
    """True when the publisher's generated identity line names the sequence.

    publish_release.release_notes writes exactly one line
    `WoT <version> (release sequence N, commit <sha>)` outside the marked
    What's new section. Notes prose, including anything inside the markers,
    never counts as a match.
    """
    outside = body
    start = body.find(WHATS_NEW_START)
    end = body.find(WHATS_NEW_END, start + len(WHATS_NEW_START)) \
        if start >= 0 else -1
    if start >= 0 and end >= 0:
        outside = body[:start] + body[end + len(WHATS_NEW_END):]
    identity = re.compile(
        rf"^WoT [^\n]+ \(release sequence {release_sequence}, commit [0-9a-f]+\)$",
        re.MULTILINE)
    return identity.search(outside) is not None


def _json_object(raw: str, what: str) -> dict:
    try:
        value = json.loads(raw)
    except ValueError as exc:
        raise EditError(f"{what} returned malformed JSON: {exc}") from exc
    if not isinstance(value, dict):
        raise EditError(f"{what} did not return an object")
    return value


def main(argv: Optional[Sequence[str]] = None,
         runner: Runner = run_command) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--sequence", required=True, type=int)
    parser.add_argument("--notes-file", required=True)
    parser.add_argument("--publish", action="store_true")
    parser.add_argument("--repo", default=formats.REPOSITORY)
    parser.add_argument("--gh", default="gh")
    args = parser.parse_args(argv)

    if args.repo != formats.REPOSITORY or args.sequence < 1:
        print("update_release_notes: refusing repository or non-positive sequence",
              file=sys.stderr)
        return 1
    try:
        text = formats.normalize_release_notes_text(
            Path(args.notes_file).read_text(encoding="utf-8"))
        with tempfile.TemporaryDirectory(prefix="wot-edit-notes-") as scratch:
            scratch_path = Path(scratch)
            checked(runner, [
                args.gh, "release", "download", "--repo", args.repo,
                "--pattern", formats.RELEASE_NOTES_ASSET, "--dir", scratch,
            ], "download latest release notes")
            asset_path = scratch_path / formats.RELEASE_NOTES_ASSET
            if not asset_path.is_file():
                raise EditError("latest release did not provide the notes asset")
            previous = asset_path.read_bytes()
            updated = formats.replace_release_notes_entry(
                previous, args.sequence, text)

            latest_view = checked(runner, [
                args.gh, "release", "view", "--repo", args.repo,
                "--json", "tagName",
            ], "read latest release tag")
            latest = _json_object(latest_view.stdout, "latest release view").get(
                "tagName")
            if not isinstance(latest, str) or not latest:
                raise EditError("latest release view did not name a tag")

            listed = checked(runner, [
                args.gh, "release", "list", "--repo", args.repo,
                "--limit", str(RELEASE_LIST_LIMIT), "--json", "tagName",
            ], "list releases")
            try:
                releases = json.loads(listed.stdout)
            except ValueError as exc:
                raise EditError(f"release list returned malformed JSON: {exc}") from exc
            if not isinstance(releases, list):
                raise EditError("release list did not return an array")
            if len(releases) >= RELEASE_LIST_LIMIT:
                raise EditError(
                    f"release list reached its {RELEASE_LIST_LIMIT}-release limit "
                    "and may be truncated")
            tags = []
            for row in releases:
                if (not isinstance(row, dict)
                        or not isinstance(row.get("tagName"), str)
                        or not row["tagName"]):
                    raise EditError("release list returned an invalid tag")
                tags.append(row["tagName"])
            matches = []
            for tag in tags:
                viewed = checked(runner, [
                    args.gh, "release", "view", tag, "--repo", args.repo,
                    "--json", "tagName,body",
                ], f"read release {tag}")
                release = _json_object(viewed.stdout, f"release {tag} view")
                if (release.get("tagName") != tag
                        or not isinstance(release.get("body"), str)):
                    raise EditError(f"release {tag} view returned invalid fields")
                if names_release_sequence(release["body"], args.sequence):
                    matches.append(release)
            if len(matches) != 1:
                raise EditError(
                    f"expected one release whose identity line names release "
                    f"sequence {args.sequence}; found {len(matches)}")
            target_tag = matches[0]["tagName"]
            updated_body = marked_body(matches[0]["body"], text)

            if not args.publish:
                print("update_release_notes: DRY RUN -- no release was changed.")
                print(subprocess.list2cmdline([
                    args.gh, "release", "upload", latest,
                    f"<generated {formats.RELEASE_NOTES_ASSET}>",
                    "--repo", args.repo, "--clobber"]))
                print(subprocess.list2cmdline([
                    args.gh, "release", "edit", target_tag, "--repo", args.repo,
                    "--notes-file", "<updated release body>"]))
                return 0

            asset_path.write_bytes(updated)
            body_path = scratch_path / "release-body.md"
            body_path.write_text(updated_body, encoding="utf-8")
            checked(runner, [
                args.gh, "release", "upload", latest, str(asset_path),
                "--repo", args.repo, "--clobber",
            ], "upload release notes")
            checked(runner, [
                args.gh, "release", "edit", target_tag, "--repo", args.repo,
                "--notes-file", str(body_path),
            ], "edit release body")
            print(f"update_release_notes: published sequence {args.sequence}")
            return 0
    except (OSError, UnicodeError, formats.FormatError, EditError) as exc:
        print(f"update_release_notes: refused -- {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
