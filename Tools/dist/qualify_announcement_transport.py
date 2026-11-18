#!/usr/bin/env python3
"""Qualify announcement endpoints without trusting announcement contents.

This tool performs transport-only GETs, follows redirects manually, and
discards response bodies without parsing, hashing, or signature verification.
Offline and connection failures return nonzero and write no receipt; transport
evidence is never synthesized.
"""
from __future__ import annotations

import argparse
import http.client
import ipaddress
import json
import socket
import ssl
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from urllib.parse import SplitResult, urlsplit

# Independent literal encoding of the production allow-list in
# Source/WoTUpdateTransports/Private/WoTUpdateTransportPolicy.cpp.
ALLOWED_REDIRECT_HOSTS = (
    "release-assets.githubusercontent.com",
    "objects.githubusercontent.com",
    "github-releases.githubusercontent.com",
)
REDIRECT_STATUSES = range(300, 400)


class QualificationError(Exception):
    """The endpoint could not produce transport evidence."""


class PolicyRejected(QualificationError):
    """Observed transport evidence violated the closed redirect policy."""


@dataclass(frozen=True)
class FetchResult:
    terminal_status: int
    redirect_count: int
    tls_identity: dict | None


def _parse_url(url: str) -> SplitResult | None:
    if (not url
            or any(ord(character) <= 0x20 or ord(character) == 0x7f
                   for character in url)
            or "\\" in url or "#" in url):
        return None
    try:
        parsed = urlsplit(url)
        host = parsed.hostname
        port = parsed.port
    except ValueError:
        return None
    if (not parsed.scheme or not parsed.netloc or host is None
            or parsed.fragment
            or parsed.username is not None or parsed.password is not None):
        return None
    if "%" in parsed.netloc or "[" in parsed.netloc or "]" in parsed.netloc:
        return None
    # Touch the value here so malformed ports are rejected in the try block.
    _ = port
    return parsed


def _is_loopback(host: str) -> bool:
    try:
        return ipaddress.ip_address(host).is_loopback
    except ValueError:
        return host.lower() == "localhost"


def _is_plain_http_test_url(parsed: SplitResult) -> bool:
    return parsed.scheme.lower() == "http" and _is_loopback(
        parsed.hostname or "")


def _validate_initial_url(url: str, plain_http_testing: bool) -> SplitResult:
    parsed = _parse_url(url)
    if parsed is None:
        raise PolicyRejected("initial URL is not an absolute safe URL")
    if plain_http_testing and _is_plain_http_test_url(parsed):
        return parsed
    if (parsed.scheme.lower() != "https"
            or parsed.hostname.lower() != "github.com"
            or parsed.port is not None):
        raise PolicyRejected(
            "initial URL must use HTTPS and the exact host github.com "
            "without userinfo or an explicit port")
    return parsed


_PLAIN_SEGMENT_CHARACTERS = frozenset(
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._+-")


def _github_path_segments(url: str) -> list[str] | None:
    """Path segments of an https://github.com URL with no query, or None
    (mirrors WoTUpdateTransportGitHubPathSegments)."""
    if not url.isascii():
        return None
    parsed = _parse_url(url)
    if (parsed is None or parsed.scheme.lower() != "https"
            or parsed.hostname.lower() != "github.com"
            or parsed.port is not None or parsed.query
            or "?" in url or parsed.netloc.lower() != "github.com"):
        return None
    if not parsed.path.startswith("/"):
        return None
    segments = parsed.path[1:].split("/")
    for segment in segments:
        if (not segment or segment in (".", "..")
                or any(c not in _PLAIN_SEGMENT_CHARACTERS for c in segment)):
            return None
    return segments


def latest_to_tag_hop(previous_url: str | None, location: str) -> bool:
    """GitHub's /<owner>/<repo>/releases/latest/download/<asset> 302 to
    /<owner>/<repo>/releases/download/<tag>/<asset> on github.com itself;
    mirrors WoTUpdateTransportIsLatestToTagHop."""
    if previous_url is None:
        return False
    source = _github_path_segments(previous_url)
    target = _github_path_segments(location)
    if source is None or target is None or len(source) != 6 or len(target) != 6:
        return False
    return (source[2:5] == ["releases", "latest", "download"]
            and target[2:4] == ["releases", "download"]
            and target[4] != "latest"
            and source[0] == target[0] and source[1] == target[1]
            and source[5] == target[5])


def redirect_allowed(location: str, *, hop_index: int,
                     max_redirect_hops: int,
                     allow_plain_http_for_testing: bool = False,
                     previous_url: str | None = None) -> bool:
    """Return whether an absolute redirect Location is inside policy."""
    if hop_index <= 0 or hop_index > max_redirect_hops:
        return False
    if hop_index == 1 and latest_to_tag_hop(previous_url, location):
        return True
    parsed = _parse_url(location)
    if parsed is None:
        return False
    host = parsed.hostname.lower()
    if allow_plain_http_for_testing and _is_plain_http_test_url(parsed):
        return True
    return (parsed.scheme.lower() == "https"
            and parsed.port is None
            and host in ALLOWED_REDIRECT_HOSTS)


def _request_target(parsed: SplitResult) -> str:
    target = parsed.path or "/"
    if parsed.query:
        target += "?" + parsed.query
    return target


def _certificate_identity(connection: http.client.HTTPSConnection) -> dict:
    certificate = connection.sock.getpeercert()
    return {
        "subject": certificate.get("subject"),
        "issuer": certificate.get("issuer"),
        "notAfter": certificate.get("notAfter"),
    }


def _remaining_seconds(deadline: float) -> float:
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise QualificationError("shared deadline exhausted")
    return remaining


def _one_hop(url: str, deadline: float,
             plain_http_testing: bool) -> tuple[int, str | None, dict | None]:
    parsed = _parse_url(url)
    if parsed is None:
        raise PolicyRejected(f"redirect produced an unsafe URL: {url}")
    remaining = _remaining_seconds(deadline)
    host = parsed.hostname
    headers = {
        "Accept": "*/*",
        "Connection": "close",
        "User-Agent": "WoT-announcement-transport-qualifier/1",
    }
    connection: http.client.HTTPConnection
    if parsed.scheme.lower() == "https":
        connection = http.client.HTTPSConnection(
            host, timeout=remaining, context=ssl.create_default_context())
    elif plain_http_testing and _is_plain_http_test_url(parsed):
        connection = http.client.HTTPConnection(
            host, port=parsed.port, timeout=remaining)
    else:
        raise PolicyRejected(f"refused non-HTTPS transport URL: {url}")

    try:
        connection.connect()
        tls_identity = (
            _certificate_identity(connection)
            if isinstance(connection, http.client.HTTPSConnection)
            else None
        )
        connection.sock.settimeout(_remaining_seconds(deadline))
        connection.request("GET", _request_target(parsed), headers=headers)
        response = connection.getresponse()
        status = response.status
        location = response.getheader("Location")
        # The candidate body is deliberately neither buffered nor interpreted.
        response.close()
        return status, location, tls_identity
    except socket.timeout as exc:
        raise QualificationError(
            "shared deadline exhausted during transport") from exc
    except (OSError, http.client.HTTPException, ssl.SSLError) as exc:
        raise QualificationError(f"connection failed: {exc}") from exc
    finally:
        connection.close()


def _fetch(url: str, *, deadline: float, max_redirect_hops: int,
           plain_http_testing: bool, hops: list[dict],
           evidence: dict) -> FetchResult:
    current_url = url
    redirect_count = 0
    tls_identity = None
    while True:
        status, location, tls_identity = _one_hop(
            current_url, deadline, plain_http_testing)
        evidence["tls_identity"] = tls_identity
        parsed = _parse_url(current_url)
        hops.append({
            "url": current_url,
            "status": status,
            "host": parsed.hostname.lower(),
        })
        if status not in REDIRECT_STATUSES:
            return FetchResult(status, redirect_count, tls_identity)
        next_hop_index = redirect_count + 1
        if next_hop_index > max_redirect_hops:
            raise PolicyRejected(
                f"redirect hop budget {max_redirect_hops} exceeded")
        if location is None:
            raise PolicyRejected(
                f"redirect status {status} omitted an absolute Location")
        if not redirect_allowed(
                location,
                hop_index=next_hop_index,
                max_redirect_hops=max_redirect_hops,
                allow_plain_http_for_testing=plain_http_testing,
                previous_url=current_url):
            raise PolicyRejected(f"redirect Location refused: {location}")
        current_url = location
        redirect_count = next_hop_index


def _receipt(args: argparse.Namespace, *, hops: list[dict],
             terminal_status: int | None, tls_identity: dict | None,
             started: float, qualified: str) -> dict:
    return {
        "platform": args.platform,
        "url": args.url,
        "hops": hops,
        "terminal_status": terminal_status,
        "tls_identity": tls_identity,
        "deadline_consumed_seconds": float(time.monotonic() - started),
        "qualified": qualified,
        "plain_http_testing": args.allow_plain_http_for_testing,
    }


def qualify(args: argparse.Namespace) -> tuple[dict, str | None]:
    started = time.monotonic()
    deadline = started + args.shared_deadline_seconds
    hops: list[dict] = []
    results: list[FetchResult] = []
    evidence = {"tls_identity": None}
    try:
        _validate_initial_url(
            args.url, args.allow_plain_http_for_testing)
        logical_urls = [args.url]
        if args.logical_fetches == 2:
            logical_urls.append(args.url + args.signature_suffix)
        for logical_url in logical_urls:
            result = _fetch(
                logical_url,
                deadline=deadline,
                max_redirect_hops=args.max_redirect_hops,
                plain_http_testing=args.allow_plain_http_for_testing,
                hops=hops,
                evidence=evidence)
            results.append(result)
    except PolicyRejected as exc:
        return _receipt(
            args, hops=hops, terminal_status=None,
            tls_identity=evidence["tls_identity"], started=started,
            qualified="unqualified"), str(exc)

    terminal_status = results[-1].terminal_status
    rejected = [
        result.terminal_status for result in results
        if result.terminal_status not in args.terminal_statuses
    ]
    if rejected:
        qualified = "unqualified"
        error = f"terminal status {rejected[0]} is not accepted"
    elif (all(result.terminal_status == 404 for result in results)
          and all(result.redirect_count == 0 for result in results)):
        qualified = "direct-path-only"
        error = None
    elif (all(result.terminal_status != 404 for result in results)
          and all(result.redirect_count > 0 for result in results)):
        qualified = "full-cdn-path"
        error = None
    else:
        qualified = "unqualified"
        error = "accepted statuses did not prove a direct 404 or full CDN path"

    return _receipt(
        args, hops=hops, terminal_status=terminal_status,
        tls_identity=evidence["tls_identity"], started=started,
        qualified=qualified), error


def _terminal_statuses(value: str) -> frozenset[int]:
    try:
        statuses = frozenset(int(item) for item in value.split(","))
    except ValueError as exc:
        raise argparse.ArgumentTypeError(
            "terminal statuses must be comma-separated integers") from exc
    if not statuses or any(status < 100 or status > 599
                           for status in statuses):
        raise argparse.ArgumentTypeError(
            "terminal statuses must be HTTP status integers")
    return statuses


def _nonnegative_integer(value: str) -> int:
    parsed = int(value)
    if parsed < 0:
        raise argparse.ArgumentTypeError("must be nonnegative")
    return parsed


def _positive_float(value: str) -> float:
    parsed = float(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return parsed


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=__doc__,
        epilog=(
            "Offline, DNS, TLS, timeout, and connection-refused failures are "
            "explicitly nonzero and leave no receipt; this tool never "
            "synthesizes transport evidence."))
    parser.add_argument("--platform", required=True)
    parser.add_argument("--url", required=True)
    parser.add_argument("--signature-suffix", required=True)
    parser.add_argument("--logical-fetches", required=True, type=int,
                        choices=(1, 2))
    parser.add_argument("--max-redirect-hops", required=True,
                        type=_nonnegative_integer)
    parser.add_argument("--shared-deadline-seconds", required=True,
                        type=_positive_float)
    parser.add_argument("--transport-only", required=True,
                        action="store_true")
    parser.add_argument("--discard-body", required=True, action="store_true")
    parser.add_argument("--terminal-statuses", required=True,
                        type=_terminal_statuses)
    parser.add_argument("--receipt", required=True, type=Path)
    parser.add_argument(
        "--allow-plain-http-for-testing", action="store_true",
        help="allow plain HTTP only when every fetched host is loopback")
    return parser


def _write_receipt(path: Path, receipt: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    # open(), not write_text(newline=...): that keyword needs Python 3.10,
    # and the Mac host runs 3.9.
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(json.dumps(receipt, indent=2, sort_keys=True) + "\n")


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    # A failed rerun must not leave stale evidence at the requested output.
    try:
        args.receipt.unlink(missing_ok=True)
    except OSError as exc:
        print(f"ANNOUNCEMENT TRANSPORT UNAVAILABLE: "
              f"cannot clear receipt path: {exc}", file=sys.stderr)
        return 1

    try:
        receipt, error = qualify(args)
    except QualificationError as exc:
        print(f"ANNOUNCEMENT TRANSPORT UNAVAILABLE: {exc}", file=sys.stderr)
        return 1

    try:
        _write_receipt(args.receipt, receipt)
    except OSError as exc:
        print(f"ANNOUNCEMENT TRANSPORT UNAVAILABLE: "
              f"cannot write receipt: {exc}", file=sys.stderr)
        return 1
    if error is not None:
        print(f"ANNOUNCEMENT TRANSPORT UNQUALIFIED: {error}",
              file=sys.stderr)
        return 1
    print(f"ANNOUNCEMENT TRANSPORT QUALIFIED: {receipt['qualified']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
