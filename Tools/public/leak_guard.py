#!/usr/bin/env python3
"""Fail closed when a public tree contains private, local, or unreviewed data.

Text is recognized by its bytes (strict UTF-8, UTF-8 BOM, or BOM-marked
UTF-16), not by its filename. Every other file must match an exact path and
SHA-256 in ``Tools/public/binary-allowlist.sha256``. ``--staged`` reads changed
blobs from the Git index with ``git cat-file --batch`` and never reads worktree
bytes.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import ipaddress
import os
import re
import stat
import subprocess
import sys
from pathlib import Path
from typing import Dict, Iterable, Iterator, List, Optional, Sequence, Tuple, Union

RETAIL_EXTENSIONS = frozenset({
    ".u", ".utx", ".uax", ".umx", ".unr", ".uc", ".t3d", ".wot", ".int",
})
BINARY_EXTENSIONS = frozenset({
    ".exe", ".dll", ".dylib", ".so", ".a", ".lib", ".pak", ".utoc", ".ucas",
    ".msi", ".dmg", ".pkg", ".app",
})
PRIVATE_COMPONENTS = frozenset({
    "original", "devtree", "exports", "content", "saved", "intermediate",
    "deriveddatacache", "binaries", ".superpowers", ".worktrees",
})
LOCAL_CONFIG_FILES = frozenset({
    "claude.local.md",
})
BINARY_ALLOWLIST_PATH = "Tools/public/binary-allowlist.sha256"
LFS_POINTER_PREFIX = b"version https://git-lfs.github.com/spec/"
DEFAULT_MAX_BYTES = 4 * 1024 * 1024
PLACEHOLDER_NAMES = frozenset({
    "you", "user", "username", "name", "placeholder", "tester", "builder",
    "runner", "shared", "public", "default", "alice", "bob", "ci", "player",
    "<user>", "<name>", "<you>", "<placeholder>",
})
PLACEHOLDER_HOSTS = frozenset({
    "example", "foreign", "host", "hostname", "localhost", "server",
})
_MACHINE_PATH_RES = (
    ("posix-home", re.compile(r"/(?:Users|home)/([A-Za-z0-9_.<>-]+)/")),
    ("windows-home", re.compile(
        r"(?<![A-Za-z0-9])C:[\\/]Users[\\/]([A-Za-z0-9_.<>-]+)(?=[\\/])",
        re.I,
    )),
    ("windows-private-root", re.compile(
        r"(?<![A-Za-z0-9])G:\\(?:wot_scratch|wot|EpicGames|UnrealEngine)"
        r"\b[^\s\"'<>|]*",
        re.I,
    )),
    ("windows-unc", re.compile(
        r"(?<![\\])\\\\([A-Za-z0-9][A-Za-z0-9_.-]+)"
        r"\\(?![\\])[^\s\"'<>|]+",
        re.I,
    )),
)
_IPV4_RE = re.compile(r"(?<![0-9])(?:[0-9]{1,3}\.){3}[0-9]{1,3}(?![0-9])")
_IPV6_RE = re.compile(
    r"(?<![A-Za-z0-9_:])(?:[0-9A-Fa-f]{0,4}:){2,7}[0-9A-Fa-f]{0,4}"
    r"(?![A-Za-z0-9_:])"
)
_EMAIL_RE = re.compile(
    r"(?<![A-Za-z0-9.!#$%&'*+/=?^_`{|}~-])"
    r"[A-Za-z0-9.!#$%&'*+/=?^_`{|}~-]+@"
    r"[A-Za-z0-9](?:[A-Za-z0-9.-]*[A-Za-z0-9])?\.[A-Za-z]{2,}"
)
# Apple scale-suffixed asset names such as background@2x.png are not addresses.
_SCALE_ASSET_DOMAIN_RE = re.compile(r"[0-9]+x\.(?:png|jpe?g|gif|tiff?|webp|icns)")
RFC_1918_NETWORKS = tuple(ipaddress.ip_network(value) for value in (
    (0x0A000000, 8), (0xAC100000, 12), (0xC0A80000, 16),
))
RFC_5737_NETWORKS = tuple(ipaddress.ip_network(value) for value in (
    "192.0.2.0/24", "198.51.100.0/24", "203.0.113.0/24",
))
RFC_3849_NETWORK = ipaddress.ip_network("2001:db8::/32")
_TEXT_KEY_MARKERS = (
    re.compile(r"-----BEGIN (?:RSA |DSA |EC |OPENSSH )?PRIVATE KEY-----", re.I),
    re.compile(r"\bAGE-SECRET-KEY-[A-Z0-9-]+", re.I),
    re.compile(r"\bage-" + r"encryption\.org/v1\b", re.I),
    re.compile(r"\bssh-(?:rsa|dss|ed25519|ecdsa-[A-Za-z0-9-]+)\s+[A-Za-z0-9+/]{20,}={0,3}"),
)
_BINARY_KEY_MARKERS = (
    ("openssh", b"openssh-key-v1\x00"),
    ("age", b"age-" + b"encryption.org/v1"),
    ("pkcs-rsa-oid", b"\x06\x09\x2a\x86\x48\x86\xf7\x0d\x01\x01\x01"),
)


class GuardError(RuntimeError):
    """The guard could not completely and safely enumerate its input."""


class Violation:
    __slots__ = ("rule", "path", "detail")

    def __init__(self, rule: str, path: str, detail: str = "") -> None:
        self.rule = rule
        self.path = path
        self.detail = detail

    def __str__(self) -> str:
        suffix = (": " + self.detail) if self.detail else ""
        return "%s: %s%s" % (self.rule, self.path, suffix)


def _is_reparse(path: Path) -> bool:
    if path.is_symlink():
        return True
    try:
        attributes = getattr(path.lstat(), "st_file_attributes", 0)
    except OSError as exc:
        raise GuardError("could not inspect %s: %s" % (path, exc)) from exc
    return bool(attributes & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0))


def tree_files(root: Path) -> Iterator[Tuple[str, Path]]:
    def traversal_error(exc: OSError) -> None:
        raise GuardError("directory traversal failed: %s" % exc)

    for directory, names, files in os.walk(root, topdown=True, onerror=traversal_error):
        names[:] = sorted(name for name in names if name != ".git")
        for name in names:
            full = Path(directory) / name
            if _is_reparse(full):
                raise GuardError("symlink or reparse directory is forbidden: %s" % full)
        for name in sorted(name for name in files if name != ".git"):
            full = Path(directory) / name
            if _is_reparse(full):
                raise GuardError("symlink or reparse file is forbidden: %s" % full)
            yield full.relative_to(root).as_posix(), full


def _run_git(repo: Path, arguments: Sequence[str], input_bytes: Optional[bytes] = None) -> bytes:
    completed = subprocess.run(
        ["git", "-C", str(repo), *arguments], input=input_bytes,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    if completed.returncode:
        detail = completed.stderr.decode("utf-8", errors="replace").strip()
        raise GuardError(detail or "git command failed")
    return completed.stdout


def _cat_blobs(repo: Path, oids: Sequence[str]) -> Dict[str, bytes]:
    unique = list(dict.fromkeys(oids))
    raw = _run_git(
        repo, ["cat-file", "--batch"],
        "".join(oid + "\n" for oid in unique).encode("ascii"),
    )
    stream = io.BytesIO(raw)
    blobs: Dict[str, bytes] = {}
    for expected in unique:
        header = stream.readline()
        try:
            oid, object_type, raw_size = header.rstrip(b"\n").decode("ascii").split(" ")
            size = int(raw_size)
        except (ValueError, UnicodeDecodeError) as exc:
            raise GuardError("git cat-file returned malformed output") from exc
        if oid != expected or object_type != "blob" or size < 0:
            raise GuardError("git cat-file returned an unexpected object for %s" % expected)
        data = stream.read(size)
        if len(data) != size or stream.read(1) != b"\n":
            raise GuardError("git cat-file returned truncated output")
        blobs[oid] = data
    if stream.read(1):
        raise GuardError("git cat-file returned trailing output")
    return blobs


def _index_entries(repo: Path) -> List[Tuple[str, str, str, str]]:
    entries: List[Tuple[str, str, str, str]] = []
    for raw in _run_git(repo, ["ls-files", "--stage", "-z"]).split(b"\0"):
        if not raw:
            continue
        try:
            metadata, raw_path = raw.split(b"\t", 1)
            mode, oid, stage = metadata.decode("ascii").split(" ")
        except (ValueError, UnicodeDecodeError) as exc:
            raise GuardError("git ls-files returned malformed output") from exc
        entries.append((raw_path.decode("utf-8", errors="surrogateescape"),
                        mode, oid, stage))
    return entries


def staged_files(repo: Path) -> List[Tuple[str, bytes]]:
    """Return changed index blobs; reject staged links, gitlinks and conflicts."""
    changed = {
        raw.decode("utf-8", errors="surrogateescape")
        for raw in _run_git(
            repo, ["diff", "--cached", "--name-only", "--diff-filter=ACMR", "-z"]
        ).split(b"\0") if raw
    }
    parsed: List[Tuple[str, str]] = []
    for path, mode, oid, stage in _index_entries(repo):
        if path not in changed:
            continue
        if stage != "0":
            raise GuardError("unmerged index entry is forbidden: %s" % path)
        if mode not in {"100644", "100755"}:
            raise GuardError("index symlink, gitlink, or special mode is forbidden: %s %s" % (mode, path))
        parsed.append((path, oid))
    blobs = _cat_blobs(repo, [oid for _, oid in parsed])
    return [(path, blobs[oid]) for path, oid in parsed]


def index_blob(repo: Path, wanted_path: str) -> Optional[bytes]:
    matches = [(oid, mode, stage) for path, mode, oid, stage in _index_entries(repo)
               if path == wanted_path]
    if not matches:
        return None
    oid, mode, stage = matches[0]
    if len(matches) != 1 or stage != "0" or mode not in {"100644", "100755"}:
        raise GuardError("binary allowlist is not a regular stage-zero index blob")
    return _cat_blobs(repo, [oid])[oid]


def _decode_text(data: bytes) -> Optional[str]:
    try:
        if data.startswith(b"\xef\xbb\xbf"):
            return data.decode("utf-8-sig")
        if data.startswith((b"\xff\xfe", b"\xfe\xff")):
            return data.decode("utf-16")
        if b"\0" in data:
            return None
        text = data.decode("utf-8")
    except UnicodeDecodeError:
        return None
    controls = sum(ord(character) < 32 and character not in "\t\n\r\f\b"
                   for character in text)
    return None if controls > max(2, len(text) // 100) else text


def machine_path_hits(text: str, allow: Sequence[re.Pattern]) -> List[str]:
    hits: List[str] = []
    for label, pattern in _MACHINE_PATH_RES:
        for match in pattern.finditer(text):
            if label in {"posix-home", "windows-home"}:
                name = match.group(1).casefold()
                if (name in PLACEHOLDER_NAMES
                        or name.startswith("<") and name.endswith(">")):
                    continue
            fragment = match.group(0)
            if label == "windows-unc":
                host = match.group(1).casefold()
                share = fragment[2:].split("\\", 1)[1]
                if (host in PLACEHOLDER_HOSTS
                        or re.match(r"(?:x[0-9a-f]{2}|u[0-9a-f]{4})", share, re.I)):
                    continue
            if not any(rule.search(fragment) for rule in allow):
                hits.append("%s %s" % (label, fragment))
    return hits


def private_ip_hits(text: str, allow: Sequence[re.Pattern]) -> List[str]:
    hits: List[str] = []
    for match in _IPV4_RE.finditer(text):
        fragment = match.group(0)
        try:
            address = ipaddress.ip_address(fragment)
        except ValueError:
            continue
        if any(address in network for network in RFC_5737_NETWORKS):
            continue
        if (any(address in network for network in RFC_1918_NETWORKS)
                and not any(rule.search(fragment) for rule in allow)):
            hits.append("rfc1918 %s" % fragment)
    for match in _IPV6_RE.finditer(text):
        fragment = match.group(0)
        if not any(character in "0123456789abcdefABCDEF" for character in fragment):
            continue
        if fragment.count(":") <= 2 \
                and not any(character in "abcdefABCDEF" for character in fragment):
            continue
        try:
            address = ipaddress.ip_address(fragment)
        except ValueError:
            continue
        if address.version == 6 and address not in RFC_3849_NETWORK \
                and not any(rule.search(fragment) for rule in allow):
            hits.append("ipv6 %s" % fragment)
    return hits


def _url_userinfo(text: str, start: int, end: int) -> bool:
    before = text[max(0, start - 12):start].lower()
    after = text[end:end + 1]
    address = text[start:end]
    return (address.casefold() == "git@" + "github.com" and after == ":") or ((before.endswith("https://") or before.endswith("http://"))
            and after in "/:") or (address.startswith("//")
                                    and before.endswith(("https:", "http:"))
                                    and after in "/:")


def email_hits(text: str) -> List[str]:
    hits: List[str] = []
    for match in _EMAIL_RE.finditer(text):
        address = match.group(0)
        local, domain = (part.casefold() for part in address.rsplit("@", 1))
        if (_url_userinfo(text, match.start(), match.end())
                or local in {"noreply", "no-reply"}
                or domain in {"example.com", "example.org", "example.net"}
                or domain.endswith(".invalid")
                or _SCALE_ASSET_DOMAIN_RE.fullmatch(domain)
                or address.casefold() == "daniel@constexpr.org"):
            continue
        hits.append(address)
    return hits


def _read_source(source: Union[Path, bytes]) -> bytes:
    if isinstance(source, bytes):
        return source
    try:
        return source.read_bytes()
    except OSError as exc:
        raise GuardError("could not read %s: %s" % (source, exc)) from exc


def check_file(
    relative: str,
    source: Union[Path, bytes],
    max_bytes: int,
    allow: Sequence[re.Pattern],
    binary_allowlist: Optional[Dict[str, str]] = None,
    private_users: Sequence[str] = (),
    private_hosts: Sequence[str] = (),
) -> List[Violation]:
    found: List[Violation] = []
    parts = relative.split("/")
    suffix = Path(parts[-1]).suffix.casefold()
    if suffix in RETAIL_EXTENSIONS:
        found.append(Violation("retail-extension", relative))
    if suffix in BINARY_EXTENSIONS:
        found.append(Violation("binary-extension", relative))
    private = [part for part in parts[:-1] if part.casefold() in PRIVATE_COMPONENTS]
    if private:
        found.append(Violation("private-root", relative, "component %s" % private[0]))
    if (parts[-1].casefold() in {"local.json", "nodes.json"}
            or relative.casefold() in LOCAL_CONFIG_FILES):
        found.append(Violation("local-config", relative))

    data = _read_source(source)
    if data.startswith(LFS_POINTER_PREFIX):
        found.append(Violation("lfs-pointer", relative))
    if len(data) > max_bytes:
        found.append(Violation("large-file", relative, "%d bytes > %d" % (len(data), max_bytes)))
    for label, marker in _BINARY_KEY_MARKERS:
        if marker in data:
            found.append(Violation("key-material", relative, label))
    if len(data) >= 12 and data[0] == 0x30 and b"\x02\x01\x03\x30" in data[:16]:
        found.append(Violation("key-material", relative, "pkcs12/pfx"))

    text = _decode_text(data)
    if text is None:
        digest = hashlib.sha256(data).hexdigest()
        approved = (binary_allowlist or {}).get(relative)
        if approved is None:
            found.append(Violation("unapproved-binary", relative, "sha256=" + digest))
        elif approved != digest:
            found.append(Violation("binary-hash", relative, "sha256=" + digest))
        return found

    for marker in _TEXT_KEY_MARKERS:
        match = marker.search(text)
        if match:
            found.append(Violation("key-material", relative, match.group(0)[:80]))
    for hit in machine_path_hits(text, allow)[:5]:
        found.append(Violation("machine-path", relative, hit))
    for hit in private_ip_hits(text, allow)[:5]:
        found.append(Violation("private-ip", relative, hit))
    for hit in email_hits(text)[:5]:
        found.append(Violation("email", relative, hit))
    folded = text.casefold()
    for marker in private_users:
        if marker.casefold() in folded:
            found.append(Violation("private-user", relative, marker))
    for marker in private_hosts:
        if marker.casefold() in folded:
            found.append(Violation("private-host", relative, marker))
    return found


def parse_binary_allowlist(data: bytes) -> Dict[str, str]:
    try:
        lines = data.decode("utf-8").splitlines()
    except UnicodeDecodeError as exc:
        raise GuardError("binary allowlist is not UTF-8: %s" % exc) from exc
    entries: List[Tuple[str, str]] = []
    for number, line in enumerate(lines, start=1):
        if not line or line.startswith("#"):
            continue
        match = re.fullmatch(r"([0-9a-f]{64})  (.+)", line)
        if not match:
            raise GuardError("malformed binary allowlist line %d" % number)
        digest, path = match.groups()
        entries.append((path, digest))
    if entries != sorted(entries):
        raise GuardError("binary allowlist entries must be sorted by path")
    if len(entries) != len({path for path, _ in entries}):
        raise GuardError("binary allowlist contains duplicate paths")
    return dict(entries)


def check(
    files: Iterable[Tuple[str, Union[Path, bytes]]],
    max_bytes: int = DEFAULT_MAX_BYTES,
    allow: Sequence[str] = (),
    binary_allowlist: Optional[Dict[str, str]] = None,
    private_users: Sequence[str] = (),
    private_hosts: Sequence[str] = (),
    require_allowlist_paths: bool = True,
) -> Tuple[List[Violation], int]:
    rules = [re.compile(pattern) for pattern in allow]
    violations: List[Violation] = []
    count = 0
    seen = set()
    for relative, source in files:
        count += 1
        seen.add(relative)
        violations.extend(check_file(
            relative, source, max_bytes, rules, binary_allowlist,
            private_users, private_hosts,
        ))
    if require_allowlist_paths:
        for path in sorted(set(binary_allowlist or {}) - seen):
            violations.append(Violation("stale-binary-allowlist", path))
    return violations, count


def run(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--tree", default=".", help="directory to check (default: current)")
    source.add_argument("--staged", action="store_true", help="scan every blob in the Git index")
    parser.add_argument("--max-bytes", type=int, default=DEFAULT_MAX_BYTES)
    parser.add_argument("--allow", action="append", default=[],
                        help="regex for an allowed machine-path or IP fragment")
    parser.add_argument("--private-user", action="append", default=[],
                        help="case-insensitive private username marker (repeatable)")
    parser.add_argument("--private-host", action="append", default=[],
                        help="case-insensitive private hostname marker (repeatable)")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args(argv)
    try:
        if args.staged:
            repo = Path(".").resolve()
            files = staged_files(repo)
        else:
            root = Path(args.tree).resolve()
            if not root.is_dir():
                print("ERROR: not a directory: %s" % root, file=sys.stderr)
                return 2
            files = list(tree_files(root))
        by_path = dict(files)
        allowlist_source = by_path.get(BINARY_ALLOWLIST_PATH)
        if args.staged and allowlist_source is None:
            allowlist_source = index_blob(repo, BINARY_ALLOWLIST_PATH)
        binary_allowlist = (parse_binary_allowlist(_read_source(allowlist_source))
                            if allowlist_source is not None else {})
        violations, count = check(
            files, args.max_bytes, args.allow, binary_allowlist,
            args.private_user, args.private_host, not args.staged,
        )
    except (GuardError, OSError, re.error) as exc:
        print("ERROR: %s" % exc, file=sys.stderr)
        return 2
    for violation in violations:
        print(violation)
    if any((violation.path.endswith((".pyc", ".pyo"))
            or "__pycache__" in violation.path.split("/"))
           for violation in violations):
        print(
            "Python bytecode is not allowed in a public tree. Remove generated "
            "__pycache__ directories and .pyc/.pyo files, then rerun tests with "
            "PYTHONDONTWRITEBYTECODE=1.",
            file=sys.stderr,
        )
    if args.verbose or violations:
        print("leak guard: %d files checked, %d violations" % (count, len(violations)),
              file=sys.stderr)
    return 1 if violations else 0


if __name__ == "__main__":
    raise SystemExit(run())
