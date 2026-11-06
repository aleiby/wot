#!/usr/bin/env python3
"""Publish a verified release set to GitHub Releases (dry-run unless --publish).

The only destination is github.com/aleiby/wot: the compiled announcement URLs,
recovery URL, and every signed per-tag URL name that repository
(BootstrapTrust.generated.h:82-97, release_formats.release_download_url).

Refusal rules (all checked before any remote call):

  * verify_release.verify_publish_set passes on the exact directory;
  * the git working tree is clean (`status --porcelain --untracked-files=all`);
  * HEAD is the commit the release was built from (manifest build_commit), which
    is also the commit the tag is created at;
  * `--publish` also requires `--confirm-tag` to repeat the tag verbatim.

Publication order (each step is a `gh` command, printed in dry-run):

  1. read-only preflight: `gh auth status`; the tag must not exist yet; the
     currently-latest announcements are downloaded and AUTHENTICATED, and the new
     set must not roll their sequence or key-ring epoch back;
  2. `gh release create <tag> --draft --target <sha> <assets...>` -- a draft is
     invisible, so no client can observe a half-uploaded set;
  3. the draft's asset list is read back and compared by name and size (and by
     GitHub's sha256 digest when the API reports one);
  4. `gh release edit <tag> --draft=false --latest` -- the single step that makes
     /releases/latest/download/ serve the new announcement pair.

This tool never deletes, re-tags, or edits an already-published release.

Stdlib only; runs on the Mac host's Python 3.9.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Callable, Dict, List, Optional, Sequence

sys.path.insert(0, str(Path(__file__).resolve().parent))
import release_crypto as crypto  # noqa: E402
import release_formats as formats  # noqa: E402
import verify_release  # noqa: E402

PUBLISH_RECEIPT_FORMAT = "wot-release-publish-receipt-v1"
DEFAULT_EVIDENCE = formats.REPOSITORY_ROOT / "Saved" / "ReleaseEvidence" / "release"
WHATS_NEW_START = "<!-- wot-whats-new:start -->"
WHATS_NEW_END = "<!-- wot-whats-new:end -->"

Runner = Callable[[Sequence[str]], subprocess.CompletedProcess]


def run_command(command: Sequence[str]) -> subprocess.CompletedProcess:
    # Uploads of the ~GB archives can take hours on a home uplink; gh itself
    # retries and times out per request, so no wall-clock cap is imposed here.
    return subprocess.run(list(command), capture_output=True, text=True)


class PublishError(Exception):
    pass


@dataclass
class Plan:
    tag: str
    target: str
    title: str
    repo: str
    assets: List[Path]
    player_downloads: Dict[str, str]
    notes: str
    release_notes_asset: bytes = b""
    local_failures: List[str] = field(default_factory=list)
    steps: List[Dict[str, object]] = field(default_factory=list)


def git_checks(repository: Path, build_commit: str, runner: Runner) -> tuple:
    """Return (head sha, failures) for the clean-tree and built-commit rules."""
    failures = []
    status = runner(["git", "-C", str(repository), "status", "--porcelain",
                     "--untracked-files=all"])
    if status.returncode != 0:
        failures.append(f"git status failed: {status.stderr.strip()}")
    elif status.stdout.strip():
        lines = status.stdout.strip().splitlines()
        failures.append(f"the working tree is not clean ({len(lines)} change(s), e.g. "
                        f"{lines[0].strip()!r})")
    head = runner(["git", "-C", str(repository), "rev-parse", "HEAD"])
    built = runner(["git", "-C", str(repository), "rev-parse", "--verify",
                    f"{build_commit}^{{commit}}"])
    head_sha = head.stdout.strip() if head.returncode == 0 else ""
    built_sha = built.stdout.strip() if built.returncode == 0 else ""
    if not head_sha:
        failures.append("cannot resolve HEAD")
    if not built_sha:
        failures.append(f"the release commit {build_commit} is not in this repository")
    elif head_sha and built_sha != head_sha:
        failures.append(f"HEAD {head_sha[:12]} is not the release commit "
                        f"{built_sha[:12]}; publish from the commit that was built")
    return built_sha or head_sha, failures


# Player-facing install text shared by every release.
GAME_DATA_NOTES = (
    "You need to own the original game. The README says where to get its",
    "installer: https://github.com/aleiby/wot#playing",
    "The first launch finds the installer in your Downloads folder by itself",
    "(or click Browse if it is somewhere else) and builds the game data from it in",
    "about a minute. The game never runs the installer, and nothing from the",
    "original game is included in this download.",
)
MAC_INSTALL_NOTES = (
    "### Mac",
    "",
    "1. Open <dmg> and drag WoT.app to",
    "   Applications, then open WoT.app from Applications.",
    "2. Multiplayer > LAN: the game tells you macOS is about to ask; choose",
    "   **Allow** so it can find and host games on your network (change it later",
    "   in System Settings > Privacy & Security > Local Network).",
    "3. Multiplayer > Internet: the game stores its anonymous online ID in your",
    "   keychain. When it says macOS will ask, choose **Always Allow**. After each",
    "   update macOS asks once more.",
)
MAC_UNNOTARIZED_INSTALL_NOTES = (
    "### Mac",
    "",
    "1. Open <dmg> and drag WoT.app to",
    "   Applications, then open WoT.app from Applications.",
    "2. macOS reports that it cannot verify the app: this build is",
    "   not notarized. Click Done, open System Settings > Privacy & Security,",
    "   scroll to Security, click **Open Anyway** next to WoT, confirm with your",
    "   password, then click **Open**. This is needed once per download.",
    "3. Multiplayer > LAN: the game tells you macOS is about to ask; choose",
    "   **Allow** so it can find and host games on your network (change it later",
    "   in System Settings > Privacy & Security > Local Network).",
    "4. Multiplayer > Internet: the game stores its anonymous online ID in your",
    "   keychain. When it says macOS will ask, choose **Always Allow**. After each",
    "   update macOS asks once more.",
)
# The guarded Win64 stage passes -NoBootstrapExe (build_stripped_pak.py
# _guarded_main_artifact_path), so there is no root WoT.exe: the entry point is
# the decorated client. The exe is unsigned, so a downloaded zip's Mark of the
# Web makes SmartScreen warn on first run.
WIN64_INSTALL_NOTES = (
    "### Windows",
    "",
    "1. Right-click the zip and choose Extract All.",
    "2. Run WoT Reborn\\WoT Reborn.exe (tip: right-click",
    "   it > Send to > Desktop to make a shortcut). Windows may say \"Windows",
    "   protected your PC\": this build is not code-signed. Click **More info**,",
    "   then **Run anyway**. This is needed once per download.",
    "3. The first time you host a game, Windows Firewall asks whether WoT may use",
    "   the network: allow it (Private networks is enough for LAN play).",
)


def release_notes(verified: verify_release.VerifiedSet, human_version: str,
                  whats_new: str) -> str:
    shipping = all(result.configuration == formats.SHIPPING_CONFIGURATION
                   for result in verified.platforms.values())
    flavor = "guarded-shipping" if shipping else "guarded"
    lines = [
        "## What's new",
        "",
        WHATS_NEW_START,
        whats_new,
        WHATS_NEW_END,
        "",
        f"WoT Reborn {human_version} (commit {verified.build_commit})",
        "",
        "### Which file do I download?",
        "",
    ]
    # GitHub lists every asset with its size and digest right below the notes,
    # so the notes only point players at the one file per platform they need.
    player_downloads = formats.player_download_asset_sources(verified)
    if player_downloads:
        downloads = (("Mac", formats.PLAYER_DOWNLOAD_ASSETS["Mac"]),
                     ("Windows", formats.PLAYER_DOWNLOAD_ASSETS["Win64"]))
        lines += [f"- **{label}:** {name}" for label, name in downloads
                  if name in player_downloads]
    else:
        downloads = (("Mac", f"WoT-Mac-{flavor}-{verified.build_commit}.dmg"),
                     ("Windows", f"WoT-Win64-{flavor}-{verified.build_commit}.zip"))
        lines += [f"- **{label}:** {name}" for label, name in downloads
                  if name in verified.assets]
    lines += [
        "",
        "Everything else is used by the game's built-in updater and its signature",
        "checks; you don't need to download it.",
        "",
        *GAME_DATA_NOTES,
        "",
    ]
    if "Win64" in verified.platforms:
        lines += [*WIN64_INSTALL_NOTES, ""]
    if "Mac" in verified.platforms:
        mac_notes = (MAC_INSTALL_NOTES
                     if getattr(verified.platforms["Mac"],
                                "dmg_notarized", False)
                     else MAC_UNNOTARIZED_INSTALL_NOTES)
        # A Shipping publication carries the fixed-name copy; a deliberate Test
        # publication only has the commit-named DMG.
        dmg_name = (formats.PLAYER_DOWNLOAD_ASSETS["Mac"]
                    if formats.PLAYER_DOWNLOAD_ASSETS["Mac"]
                    in formats.player_download_asset_sources(verified)
                    else verified.platforms["Mac"].dmg
                    or f"WoT-Mac-{flavor}-{verified.build_commit}.dmg")
        lines += [*(line.replace("<dmg>", dmg_name) for line in mac_notes), ""]
    return "\n".join(lines).rstrip("\n") + "\n"


def build_plan(verified: verify_release.VerifiedSet, *, target: str, repo: str,
               title: Optional[str], human_version: str,
               notes_text: str) -> Plan:
    assets = [verified.directory / name for name in sorted(verified.assets)]
    return Plan(tag=verified.tag, target=target, repo=repo,
                title=title or f"WoT Reborn {human_version}", assets=assets,
                player_downloads=formats.player_download_asset_sources(verified),
                notes=release_notes(verified, human_version, notes_text))


def _run_step(plan: Plan, runner: Runner, name: str, command: Sequence[str],
              *, expect_success: bool = True) -> subprocess.CompletedProcess:
    result = runner(command)
    plan.steps.append({"step": name, "command": list(command),
                       "returncode": result.returncode})
    if expect_success and result.returncode != 0:
        raise PublishError(f"{name} failed ({result.returncode}): "
                           f"{(result.stderr or result.stdout or '').strip()}")
    return result


def prepare_release_notes_asset(plan: Plan, runner: Runner, gh: str,
                                verified: verify_release.VerifiedSet,
                                notes_text: str) -> None:
    """Read the current cumulative asset and merge this release's entry."""
    previous = None
    with tempfile.TemporaryDirectory(prefix="wot-latest-notes-") as scratch:
        download = _run_step(
            plan, runner, "download-latest-release-notes",
            [gh, "release", "download", "--repo", plan.repo, "--pattern",
             formats.RELEASE_NOTES_ASSET, "--dir", scratch],
            expect_success=False)
        path = Path(scratch) / formats.RELEASE_NOTES_ASSET
        if download.returncode == 0:
            if not path.is_file():
                raise PublishError(
                    "gh reported a downloaded release-notes asset but produced no file")
            previous = path.read_bytes()
        else:
            message = (download.stderr or download.stdout or "").lower()
            if not any(fragment in message for fragment in
                       ("not found", "no assets", "no release")):
                raise PublishError(
                    "cannot read the currently-latest release notes: "
                    + (download.stderr or download.stdout or "").strip())
    plan.release_notes_asset = formats.merge_release_notes(
        previous, verified.release_sequence, notes_text)


def remote_preflight(plan: Plan, runner: Runner, gh: str, trust, compiled,
                     verified: verify_release.VerifiedSet,
                     allow_staging_sequence: bool, allow_test: bool = False,
                     allow_unnotarized: bool = False,
                     releases: Optional[Dict[str, Path]] = None,
                     funnel_receipts: Optional[Dict[str, Path]] = None) -> None:
    _run_step(plan, runner, "gh-auth", [gh, "auth", "status"])
    existing = _run_step(plan, runner, "tag-free",
                         [gh, "release", "view", plan.tag, "--repo", plan.repo,
                          "--json", "tagName"], expect_success=False)
    if existing.returncode == 0:
        raise PublishError(f"release {plan.tag} already exists in {plan.repo}")
    with tempfile.TemporaryDirectory(prefix="wot-latest-") as scratch:
        download = _run_step(plan, runner, "download-latest-announcements",
                             [gh, "release", "download", "--repo", plan.repo,
                              "--pattern", "WoT-*-announcement-v1.json*",
                              "--dir", scratch], expect_success=False)
        stderr = (download.stderr or "").lower()
        if download.returncode != 0 and "not found" not in stderr \
                and "no assets" not in stderr and "no release" not in stderr:
            raise PublishError("cannot read the currently-latest announcements: "
                               f"{(download.stderr or '').strip()}")
        previous = verify_release.load_previous_announcements(Path(scratch), trust,
                                                              compiled)
    if verified.release_sequence > 0 and set(previous) != set(formats.PLATFORMS):
        raise PublishError(
            "positive release remote preflight requires authenticated previous "
            "announcements for Win64 and Mac")
    verify_release.verify_publish_set(
        verified.directory, trust=trust, compiled=compiled, previous=previous,
        releases=releases, funnel_receipts=funnel_receipts,
        allow_staging_sequence=allow_staging_sequence, allow_test=allow_test,
        allow_unnotarized=allow_unnotarized)
    plan.steps.append({"step": "monotonic-versus-latest",
                       "previous": {platform: announcement.release_sequence
                                    for platform, announcement in previous.items()}})


def _asset_rows(view_stdout: str) -> Dict[str, dict]:
    try:
        document = json.loads(view_stdout)
    except ValueError as exc:
        raise PublishError(f"gh release view returned non-JSON: {exc}") from exc
    assets = document.get("assets") if isinstance(document, dict) else None
    if not isinstance(assets, list) or not all(isinstance(row, dict) for row in assets):
        raise PublishError("gh release view returned no asset list; the draft was left "
                           "unpublished")
    return {row.get("name"): row for row in assets}


def publish(plan: Plan, runner: Runner, gh: str,
            verified: verify_release.VerifiedSet) -> None:
    with tempfile.TemporaryDirectory(
            prefix="wot-publish-", dir=verified.directory.parent) as scratch_name:
        scratch = Path(scratch_name)
        notes = scratch / "notes.md"
        notes.write_text(plan.notes, encoding="utf-8")
        notes_asset = scratch / formats.RELEASE_NOTES_ASSET
        notes_asset.write_bytes(plan.release_notes_asset)
        staged_downloads = []
        for fixed_name, source_name in sorted(plan.player_downloads.items()):
            source = verified.directory / source_name
            staged = scratch / fixed_name
            try:
                os.link(source, staged)
            except OSError:
                shutil.copyfile(source, staged)
            staged_downloads.append(staged)
        _run_step(plan, runner, "create-draft",
                  [gh, "release", "create", plan.tag, "--repo", plan.repo, "--draft",
                   "--target", plan.target, "--title", plan.title, "--notes-file",
                   str(notes), *[str(path) for path in plan.assets],
                   *[str(path) for path in staged_downloads],
                   str(notes_asset)])
    view = _run_step(plan, runner, "read-back-draft",
                     [gh, "release", "view", plan.tag, "--repo", plan.repo, "--json",
                      "assets,isDraft,tagName"])
    rows = _asset_rows(view.stdout)
    expected = dict(verified.assets)
    expected[formats.RELEASE_NOTES_ASSET] = {
        "size": len(plan.release_notes_asset),
        "sha256": hashlib.sha256(plan.release_notes_asset).hexdigest(),
    }
    for fixed_name, source_name in plan.player_downloads.items():
        expected[fixed_name] = dict(verified.assets[source_name])
    if set(rows) != set(expected):
        raise PublishError(f"draft assets {sorted(rows)} differ from the verified set "
                           f"plus release notes {sorted(expected)}; the draft was left "
                           "unpublished")
    for name, row in expected.items():
        uploaded = rows[name]
        digest = uploaded.get("digest")
        if uploaded.get("size") != row["size"] \
                or uploaded.get("state", "uploaded") != "uploaded" \
                or (digest and digest != f"sha256:{row['sha256']}"):
            raise PublishError(f"uploaded {name} differs from the verified bytes; the "
                               "draft was left unpublished")
    _run_step(plan, runner, "publish-latest",
              [gh, "release", "edit", plan.tag, "--repo", plan.repo, "--draft=false",
               "--latest"])


def dry_run_commands(plan: Plan, gh: str) -> List[List[str]]:
    return [
        [gh, "release", "download", "--repo", plan.repo, "--pattern",
         formats.RELEASE_NOTES_ASSET, "--dir", "<temp>"],
        [gh, "auth", "status"],
        [gh, "release", "view", plan.tag, "--repo", plan.repo, "--json", "tagName"],
        [gh, "release", "download", "--repo", plan.repo, "--pattern",
         "WoT-*-announcement-v1.json*", "--dir", "<temp>"],
        [gh, "release", "create", plan.tag, "--repo", plan.repo, "--draft", "--target",
         plan.target, "--title", plan.title, "--notes-file", "<generated notes>",
         *[str(path) for path in plan.assets],
         *[f"<staged {name}>" for name in sorted(plan.player_downloads)],
         f"<generated {formats.RELEASE_NOTES_ASSET}>"],
        [gh, "release", "view", plan.tag, "--repo", plan.repo, "--json",
         "assets,isDraft,tagName"],
        [gh, "release", "edit", plan.tag, "--repo", plan.repo, "--draft=false",
         "--latest"],
    ]


def qualifier_commands(repo_root: Path, tag: str) -> List[List[str]]:
    evidence = f"Saved/ReleaseEvidence/release/{tag}"
    commands = []
    for platform in formats.PLATFORMS:
        url = (f"https://github.com/{formats.REPOSITORY}/releases/latest/download/"
               f"{formats.announcement_asset(platform)}")
        commands.append([
            "python3", "Tools/dist/qualify_announcement_transport.py", "--platform",
            platform, "--url", url, "--signature-suffix", ".sig", "--logical-fetches", "2",
            "--max-redirect-hops", "3", "--shared-deadline-seconds", "15",
            "--transport-only", "--discard-body", "--terminal-statuses", "200,206,404",
            "--receipt", f"{evidence}/qualifier-{platform}.json"])
    return commands


def main(argv: Optional[Sequence[str]] = None, runner: Runner = run_command) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--set", required=True, help="verified publish-set directory")
    parser.add_argument("--human-version", required=True,
                        help="for the release title/notes (the manifest carries it too)")
    parser.add_argument("--notes-file", required=True,
                        help="player-facing What's new text for this release")
    parser.add_argument("--publish", action="store_true",
                        help="actually create the release (default is a dry run)")
    parser.add_argument("--confirm-tag", help="required with --publish: the tag, verbatim")
    parser.add_argument("--check-remote", action="store_true",
                        help="dry run: also run the read-only remote preflight")
    parser.add_argument("--repo", default=formats.REPOSITORY)
    parser.add_argument("--title")
    parser.add_argument("--gh", default="gh")
    parser.add_argument("--repository-root", default=str(formats.REPOSITORY_ROOT))
    parser.add_argument("--public-target",
                        help="full commit sha in --repo to create the tag at. The public "
                             "repository is a fresh export, so the private release commit "
                             "(still proven clean HEAD in --repository-root) is not in it")
    parser.add_argument("--release", action="append", default=[],
                        help="PLATFORM=path/to/release.json; Mac is required for "
                        "every set containing the first-install DMG")
    parser.add_argument("--funnel-receipt", action="append", default=[],
                        help="PLATFORM=path/to/funnel-receipt.json; unproven "
                        "evidence warns without refusing publication")
    parser.add_argument("--previous", help="directory holding the authenticated "
                        "currently published Win64 and Mac announcement pairs; "
                        "required for a positive sequence")
    parser.add_argument("--allow-staging-sequence", action="store_true",
                        help="compatibility no-op; funnel receipts are advisory")
    parser.add_argument("--allow-test", action="store_true",
                        help="publish a set attesting the Test configuration (a "
                        "deliberate Test publication; verify_release flags it otherwise)")
    parser.add_argument("--allow-unnotarized", action="store_true",
                        help="publish an unnotarized Mac Shipping set intentionally")
    parser.add_argument("--receipt")
    verify_release.add_trust_arguments(parser)
    args = parser.parse_args(argv)

    if args.repo != formats.REPOSITORY:
        print(f"publish_release: the compiled URLs name {formats.REPOSITORY}; refusing "
              f"to publish to {args.repo}", file=sys.stderr)
        return 1
    try:
        trust, compiled = verify_release.load_trust_from_args(args)
        release_paths = verify_release.parse_platform_paths(args.release, "release")
        funnel_paths = verify_release.parse_platform_paths(
            args.funnel_receipt, "funnel-receipt")
        previous = verify_release.load_previous_announcements(
            Path(args.previous), trust, compiled) if args.previous else None
        verified = verify_release.verify_publish_set(
            Path(args.set), trust=trust, compiled=compiled,
            previous=previous,
            releases=release_paths, funnel_receipts=funnel_paths,
            allow_staging_sequence=args.allow_staging_sequence,
            allow_test=args.allow_test,
            allow_unnotarized=args.allow_unnotarized)
    except verify_release.VerifyError as exc:
        for failure in exc.failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        print("publish_release: refused -- the set does not verify", file=sys.stderr)
        return 1
    except (formats.FormatError, crypto.CryptoError, OSError) as exc:
        print(f"publish_release: refused -- {exc}", file=sys.stderr)
        return 1

    repository_root = Path(args.repository_root)
    try:
        notes_text = formats.normalize_release_notes_text(
            Path(args.notes_file).read_text(encoding="utf-8"))
    except (OSError, UnicodeError, formats.FormatError) as exc:
        print(f"publish_release: refused -- cannot read release notes: {exc}",
              file=sys.stderr)
        return 1
    target, failures = git_checks(repository_root, verified.build_commit, runner)
    if args.public_target is not None:
        if re.fullmatch(r"[0-9a-f]{40}", args.public_target) is None:
            failures.append("--public-target must be a full lowercase commit sha")
        else:
            target = args.public_target
    plan = build_plan(verified, target=target, repo=args.repo, title=args.title,
                      human_version=args.human_version, notes_text=notes_text)
    plan.local_failures = failures
    if args.publish and args.confirm_tag != verified.tag:
        failures.append(f"--publish requires --confirm-tag {verified.tag}")
    mode = "publish" if args.publish else "dry-run"
    outcome = "refused" if failures else "planned"
    error = None
    if not failures:
        try:
            prepare_release_notes_asset(
                plan, runner, args.gh, verified, notes_text)
            if args.publish or args.check_remote:
                remote_preflight(plan, runner, args.gh, trust, compiled, verified,
                                 args.allow_staging_sequence, args.allow_test,
                                 args.allow_unnotarized,
                                 release_paths, funnel_paths)
            if args.publish:
                publish(plan, runner, args.gh, verified)
                outcome = "published"
        except (PublishError, verify_release.VerifyError, formats.FormatError) as exc:
            error = str(exc)
            outcome = "failed"

    release_notes_summary = None
    if plan.release_notes_asset:
        parsed_notes = formats.parse_release_notes(plan.release_notes_asset)
        release_notes_summary = {
            "entry_sequences": [entry["release_sequence"]
                                for entry in parsed_notes["notes"]],
            "sha256": hashlib.sha256(plan.release_notes_asset).hexdigest(),
            "size": len(plan.release_notes_asset),
        }
    receipt = {
        "format": PUBLISH_RECEIPT_FORMAT,
        "generated_utc": datetime.now(timezone.utc).isoformat(),
        "mode": mode,
        "outcome": outcome,
        "error": error,
        "local_failures": failures,
        "repo": plan.repo,
        "tag": plan.tag,
        "target": plan.target,
        "set": verified.summary(),
        "player_downloads": {
            fixed_name: {
                "source": source_name,
                **verified.assets[source_name],
            }
            for fixed_name, source_name in plan.player_downloads.items()
        },
        "release_notes_asset": release_notes_summary,
        "steps": plan.steps,
    }
    receipt_path = Path(args.receipt) if args.receipt else \
        DEFAULT_EVIDENCE / plan.tag / f"publish-{mode}-receipt.json"
    verify_release.write_receipt(receipt_path, receipt)

    for failure in failures:
        print(f"REFUSED: {failure}", file=sys.stderr)
    if error:
        print(f"publish_release: {error}", file=sys.stderr)
    if not args.publish:
        print("publish_release: DRY RUN -- nothing was created. Commands --publish runs:")
        for command in dry_run_commands(plan, args.gh):
            print("  " + subprocess.list2cmdline(command))
    print("Then qualify the live transport (docs/RELEASE-RUNBOOK.md step 8):")
    for command in qualifier_commands(Path(args.repository_root), plan.tag):
        print("  " + subprocess.list2cmdline(command))
    print(f"publish_release: {outcome}; receipt {receipt_path}")
    return 0 if outcome in ("planned", "published") else 1


if __name__ == "__main__":
    sys.exit(main())
