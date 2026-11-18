#!/usr/bin/env python3
"""Signature framing, an independent Ed25519 verifier, and OpenSSL-backed signing.

Byte contract (the part the game checks):

  framed    = b"WOT-SIGNATURE\\0" || uint16le(context) || uint64le(len(payload)) || payload
  signature = Ed25519(private_key, framed)       # 64 raw bytes: the ".sig" sidecar

  * the tag is 13 ASCII bytes plus its NUL terminator (14 bytes) --
    Source/WoTBootstrapCore/Private/Ed25519.cpp:16-17 and 113-117;
  * context and size are little-endian -- Ed25519.cpp:22-34 and 118-119, pinned
    byte for byte by WoT.BootstrapCore.Crypto.Framing
    (Source/WoTBootstrapCore/Private/Tests/CryptoTests.cpp:196-226);
  * the context ordinals are the declaration order of EWoTSignatureContext --
    Source/WoTBootstrapCore/Public/WoTBootstrapCore/ArtifactTypes.h:48-59
    ("Values are part of the signed bytes and therefore never renumbered");
  * verification is one-shot RFC 8032 Ed25519 over the framed bytes --
    Ed25519.cpp:124-188 (EVP_DigestVerify, no digest);
  * a ".sig" sidecar is exactly the 64 raw signature bytes; the game refuses any
    other length (Source/WoT/Updater/WoTGameBootstrapWorker.cpp:183-197,
    Source/WoTUpdaterCore/Private/ReleaseStaging.cpp) and fetches it from
    literally `<json url> + ".sig"` (WoTUpdateTransportPolicy.cpp:388-389).

Custody rules this module keeps:

  * Python never reads, parses, prints, or copies private-key bytes. Signing hands
    the key PATH to the OpenSSL CLI (`pkeyutl -sign -rawin`), and public keys are
    derived with `pkey -pubout`, which emits only the public half.
  * Every signature produced here is immediately re-verified by the pure-Python
    verifier below, which shares no code with OpenSSL. A signer backend that
    framed or hashed differently is caught before anything is written.

Stdlib only; runs on the Mac host's Python 3.9.
"""
from __future__ import annotations

import base64
import hashlib
import os
import re
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path
from typing import List, Optional, Sequence

SIGNATURE_TAG = b"WOT-SIGNATURE\x00"
SIGNATURE_BYTES = 64
PUBLIC_KEY_BYTES = 32

# EWoTSignatureContext ordinals (ArtifactTypes.h:48-59). Never renumbered.
CONTEXT_ANNOUNCEMENT_V1 = 0
CONTEXT_COMPONENT_IDENTITY_V1 = 1
CONTEXT_RELEASE_KEY_RING_V1 = 2
CONTEXT_FULL_PACKAGE_V1 = 3
CONTEXT_RECOVERY_DOMAIN_V1 = 4
CONTEXT_RELEASE_MANIFEST_V2 = 5
CONTEXT_INSTALLED_TREE_V1 = 6
CONTEXT_CACHE_CONTRACT_V3 = 7
CONTEXT_CACHE_LINEAGE_V1 = 8

CONTEXT_NAMES = {
    CONTEXT_ANNOUNCEMENT_V1: "AnnouncementV1",
    CONTEXT_COMPONENT_IDENTITY_V1: "ComponentIdentityV1",
    CONTEXT_RELEASE_KEY_RING_V1: "ReleaseKeyRingV1",
    CONTEXT_FULL_PACKAGE_V1: "FullPackageV1",
    CONTEXT_RECOVERY_DOMAIN_V1: "RecoveryDomainV1",
    CONTEXT_RELEASE_MANIFEST_V2: "ReleaseManifestV2",
    CONTEXT_INSTALLED_TREE_V1: "InstalledTreeV1",
    CONTEXT_CACHE_CONTRACT_V3: "CacheContractV3",
    CONTEXT_CACHE_LINEAGE_V1: "CacheLineageV1",
}

# The fixed 12-byte DER prefix of an Ed25519 SubjectPublicKeyInfo (RFC 8410).
_ED25519_SPKI_PREFIX = bytes.fromhex("302a300506032b6570032100")


class CryptoError(Exception):
    """A signing or verification step could not be completed."""


def frame_signature_input(context: int, payload: bytes) -> bytes:
    """Byte-identical to FrameSignatureInput (Ed25519.cpp:107-122)."""
    if context not in CONTEXT_NAMES:
        raise CryptoError(f"unknown signature context {context!r}")
    return SIGNATURE_TAG + struct.pack("<HQ", context, len(payload)) + bytes(payload)


# ---------------------------------------------------------------------------
# Independent RFC 8032 Ed25519 verification (pure Python, public data only).
# ---------------------------------------------------------------------------

_P = 2 ** 255 - 19
_L = 2 ** 252 + 27742317777372353535851937790883648493
_D = (-121665 * pow(121666, _P - 2, _P)) % _P
_SQRT_M1 = pow(2, (_P - 1) // 4, _P)


def _recover_x(y: int, sign: int) -> Optional[int]:
    if y >= _P:
        return None
    x2 = (y * y - 1) * pow(_D * y * y + 1, _P - 2, _P) % _P
    if x2 == 0:
        return None if sign else 0
    x = pow(x2, (_P + 3) // 8, _P)
    if (x * x - x2) % _P != 0:
        x = x * _SQRT_M1 % _P
    if (x * x - x2) % _P != 0:
        return None
    if (x & 1) != sign:
        x = _P - x
    return x


_BY = 4 * pow(5, _P - 2, _P) % _P
_BX = _recover_x(_BY, 0)
_BASE = (_BX, _BY, 1, _BX * _BY % _P)
_IDENTITY = (0, 1, 1, 0)


def _point_add(left, right):
    x1, y1, z1, t1 = left
    x2, y2, z2, t2 = right
    a = (y1 - x1) * (y2 - x2) % _P
    b = (y1 + x1) * (y2 + x2) % _P
    c = 2 * t1 * t2 * _D % _P
    d = 2 * z1 * z2 % _P
    e, f, g, h = b - a, d - c, d + c, b + a
    return (e * f % _P, g * h % _P, f * g % _P, e * h % _P)


def _scalar_mult(scalar: int, point):
    result = _IDENTITY
    addend = point
    while scalar > 0:
        if scalar & 1:
            result = _point_add(result, addend)
        addend = _point_add(addend, addend)
        scalar >>= 1
    return result


def _encode_point(point) -> bytes:
    x, y, z, _ = point
    z_inverse = pow(z, _P - 2, _P)
    x = x * z_inverse % _P
    y = y * z_inverse % _P
    return (y | ((x & 1) << 255)).to_bytes(32, "little")


def _decode_point(encoded: bytes):
    if len(encoded) != 32:
        return None
    value = int.from_bytes(encoded, "little")
    sign = value >> 255
    y = value & ((1 << 255) - 1)
    x = _recover_x(y, sign)
    if x is None:
        return None
    return (x, y, 1, x * y % _P)


def ed25519_verify(public_key: bytes, message: bytes, signature: bytes) -> bool:
    """True only for a valid RFC 8032 signature. Never raises on bad input.

    Cofactorless check with canonical-S enforcement: the same answer OpenSSL's
    EVP_DigestVerify gives for every well-formed input this tooling produces.
    """
    if len(public_key) != PUBLIC_KEY_BYTES or len(signature) != SIGNATURE_BYTES:
        return False
    point_a = _decode_point(bytes(public_key))
    if point_a is None:
        return False
    r_bytes = bytes(signature[:32])
    s = int.from_bytes(bytes(signature[32:]), "little")
    if s >= _L:
        return False
    k = int.from_bytes(hashlib.sha512(r_bytes + bytes(public_key) + bytes(message))
                       .digest(), "little") % _L
    x, y, z, t = point_a
    negative_a = ((-x) % _P, y, z, (-t) % _P)
    candidate = _point_add(_scalar_mult(s, _BASE), _scalar_mult(k, negative_a))
    return _encode_point(candidate) == r_bytes


def verify_framed(public_key: bytes, context: int, payload: bytes,
                  signature: bytes) -> bool:
    """VerifyEd25519 (Ed25519.cpp:182-188): frame, then one-shot verify."""
    return ed25519_verify(public_key, frame_signature_input(context, payload),
                          signature)


# ---------------------------------------------------------------------------
# OpenSSL CLI backend: the only code path that touches a private key, and it
# touches it by path only.
# ---------------------------------------------------------------------------

_OPENSSL_CANDIDATES = (
    r"C:\Program Files\Git\mingw64\bin\openssl.exe",
    r"C:\Program Files\Git\usr\bin\openssl.exe",
    "/opt/homebrew/opt/openssl@3/bin/openssl",
    "/usr/local/opt/openssl@3/bin/openssl",
)


def find_openssl(explicit: Optional[str] = None) -> str:
    """Resolve an OpenSSL 3.x CLI.

    Order: explicit argument, $WOT_OPENSSL, PATH, then well-known Git/Homebrew
    locations. macOS /usr/bin/openssl is LibreSSL, which has no `pkeyutl -rawin`,
    so it is refused by the version gate instead of failing mid-signature.
    """
    candidates: List[str] = []
    if explicit:
        candidates.append(explicit)
    environment = os.environ.get("WOT_OPENSSL")
    if environment:
        candidates.append(environment)
    on_path = shutil.which("openssl")
    if on_path:
        candidates.append(on_path)
    candidates.extend(_OPENSSL_CANDIDATES)
    rejected = []
    for candidate in candidates:
        if not Path(candidate).is_file() and shutil.which(candidate) is None:
            continue
        try:
            version = subprocess.run([candidate, "version"], capture_output=True,
                                     text=True, timeout=30)
        except (OSError, subprocess.SubprocessError):
            continue
        text = (version.stdout or "").strip()
        match = re.match(r"OpenSSL (\d+)\.", text)
        if version.returncode == 0 and match and int(match.group(1)) >= 3:
            return candidate
        rejected.append(f"{candidate}: {text or 'unusable'}")
        if explicit and candidate == explicit:
            break
    detail = "; ".join(rejected) if rejected else "none found"
    raise CryptoError(
        "an OpenSSL 3.x CLI is required for Ed25519 `pkeyutl -rawin` "
        f"(pass --openssl or set WOT_OPENSSL); candidates: {detail}")


def _run_openssl(openssl: str, arguments: Sequence[str], what: str) -> bytes:
    try:
        result = subprocess.run([openssl, *arguments], capture_output=True,
                                timeout=120)
    except (OSError, subprocess.SubprocessError) as exc:
        raise CryptoError(f"openssl {what} could not run: {exc}") from exc
    if result.returncode != 0:
        # pkey/pkeyutl diagnostics name the failing operation, never key bytes.
        message = result.stderr.decode("utf-8", "replace").strip().splitlines()
        raise CryptoError(
            f"openssl {what} failed (exit {result.returncode}): "
            + (message[-1] if message else "no diagnostic"))
    return result.stdout


def public_key_from_private_pem(openssl: str, private_key: Path) -> bytes:
    """Derive the raw 32-byte public key. Only the public half crosses the pipe."""
    if not Path(private_key).is_file():
        raise CryptoError(f"private key file does not exist: {private_key}")
    der = _run_openssl(openssl, ["pkey", "-in", str(private_key), "-pubout",
                                 "-outform", "DER"], "public-key derivation")
    if len(der) != len(_ED25519_SPKI_PREFIX) + PUBLIC_KEY_BYTES \
            or not der.startswith(_ED25519_SPKI_PREFIX):
        raise CryptoError(f"{private_key} is not an Ed25519 key")
    return der[len(_ED25519_SPKI_PREFIX):]


def public_key_pem(public_key: bytes) -> bytes:
    """PEM SubjectPublicKeyInfo for a raw key, for `pkeyutl -verify -pubin`."""
    if len(public_key) != PUBLIC_KEY_BYTES:
        raise CryptoError("an Ed25519 public key is 32 bytes")
    body = base64.encodebytes(_ED25519_SPKI_PREFIX + bytes(public_key)).decode("ascii")
    return ("-----BEGIN PUBLIC KEY-----\n" + body
            + "-----END PUBLIC KEY-----\n").encode("ascii")


def sign_framed(openssl: str, private_key: Path, context: int, payload: bytes,
                expected_public_key: bytes) -> bytes:
    """Sign frame(context, payload) with OpenSSL and prove it independently.

    `expected_public_key` is the key the consumer verifies with. Deriving it from
    the private key first means a wrong key file fails here, loudly, and not on
    every player's machine.
    """
    derived = public_key_from_private_pem(openssl, Path(private_key))
    if derived != bytes(expected_public_key):
        raise CryptoError(
            f"{private_key} is not the expected signer: its public key is "
            f"{derived.hex()}, the trust material requires "
            f"{bytes(expected_public_key).hex()}")
    framed = frame_signature_input(context, payload)
    with tempfile.TemporaryDirectory(prefix="wot-sign-") as scratch:
        message_path = Path(scratch) / "framed.bin"
        signature_path = Path(scratch) / "signature.bin"
        message_path.write_bytes(framed)
        _run_openssl(openssl, ["pkeyutl", "-sign", "-inkey", str(private_key),
                               "-rawin", "-in", str(message_path),
                               "-out", str(signature_path)], "signing")
        signature = signature_path.read_bytes()
    if len(signature) != SIGNATURE_BYTES:
        raise CryptoError(
            f"openssl produced a {len(signature)}-byte signature, not 64")
    if not ed25519_verify(bytes(expected_public_key), framed, signature):
        raise CryptoError(
            "the OpenSSL signature does not verify under the independent "
            f"verifier for context {CONTEXT_NAMES[context]}")
    return signature


def openssl_verify_framed(openssl: str, public_key: bytes, context: int,
                          payload: bytes, signature: bytes) -> bool:
    """A second, OpenSSL-side opinion (the C++ verifier is OpenSSL EVP too)."""
    with tempfile.TemporaryDirectory(prefix="wot-verify-") as scratch:
        key_path = Path(scratch) / "public.pem"
        message_path = Path(scratch) / "framed.bin"
        signature_path = Path(scratch) / "signature.bin"
        key_path.write_bytes(public_key_pem(public_key))
        message_path.write_bytes(frame_signature_input(context, payload))
        signature_path.write_bytes(signature)
        result = subprocess.run(
            [openssl, "pkeyutl", "-verify", "-pubin", "-inkey", str(key_path),
             "-rawin", "-in", str(message_path), "-sigfile", str(signature_path)],
            capture_output=True, timeout=120)
        return result.returncode == 0


def generate_test_key(openssl: str, destination: Path) -> bytes:
    """TESTS ONLY: create a throwaway Ed25519 key inside a temporary directory.

    Refuses any destination outside the system temporary directory, so a test key
    can never be minted into the repository or the custody directory. Returns the
    public half; the private file never passes through Python.
    """
    temp_root = Path(tempfile.gettempdir()).resolve()
    resolved = Path(destination).resolve()
    try:
        resolved.relative_to(temp_root)
    except ValueError as exc:
        raise CryptoError(
            f"test keys may only be generated below {temp_root}, not {resolved}"
        ) from exc
    _run_openssl(openssl, ["genpkey", "-algorithm", "ed25519", "-out",
                           str(destination)], "test key generation")
    return public_key_from_private_pem(openssl, Path(destination))
