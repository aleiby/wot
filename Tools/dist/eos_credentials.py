#!/usr/bin/env python3
"""EOS (Epic Online Services) credentials: packaging-time injection only.

No EOS id or secret is ever committed. Config/DefaultEngine.ini carries the
EOS *settings* (Connect-only, relay policy, DefaultArtifactName) but no
``+Artifacts`` row; without one the game reports "Internet play is not
configured in this build" and never creates the EOS platform (LAN and direct
IP are unaffected).

The values live in ONE local file outside the repository:

    %WOT_EOS_CREDENTIALS%            (when the variable is set), otherwise
    %USERPROFILE%\\.wot-secrets\\eos-credentials.json

a JSON object with exactly these six non-empty string keys:

    ProductId  SandboxId  DeploymentIdTest  DeploymentIdPublic  ClientId  ClientSecret

build_stripped_pak.py rewrites the ISOLATED DefaultEngine.ini immediately
before each staging run, so the values reach only the staged config of that
package:

* a Test (or other non-Shipping) package gets ONE artifact "WoTTest" with
  DeploymentIdTest;
* a Shipping package gets ONE artifact "WoT" with DeploymentIdPublic.

(The game's FWoTEOSConfig::PreferredArtifactName: Shipping accepts only "WoT";
other configurations prefer "WoTTest".) A missing or invalid file disables EOS
cleanly in that package - it never fails the build. NOTHING here prints, logs
or records a value: receipts carry only the state, the key set, and the
SHA-256 of the canonical file.
"""
from __future__ import annotations

import hashlib
import json
import os
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Mapping, Optional

KEYS = ("ProductId", "SandboxId", "DeploymentIdTest", "DeploymentIdPublic",
        "ClientId", "ClientSecret")
ENV_FILE = "WOT_EOS_CREDENTIALS"
DEFAULT_RELATIVE = Path(".wot-secrets") / "eos-credentials.json"
SECTION = "[/Script/OnlineSubsystemEOS.EOSSettings]"
SHIPPING_ARTIFACT = "WoT"
TEST_ARTIFACT = "WoTTest"
BEGIN = "; BEGIN WOT EOS CREDENTIALS (packaging-time injection; never in git)"
END = "; END WOT EOS CREDENTIALS"
MAX_FILE_BYTES = 16 * 1024
# Printable ASCII without anything that could end the quoted value or the
# struct literal of a +Artifacts row (mirrors FWoTEOSConfig::ValidateArtifact).
_VALUE = re.compile(r"^[!#-'*+\x2d-\x5b\x5d-\x7e]{1,128}$")


class CredentialError(ValueError):
    """A config file is in a state this tool refuses to rewrite."""


@dataclass(frozen=True)
class Credentials:
    product_id: str
    sandbox_id: str
    deployment_test: str
    deployment_public: str
    client_id: str
    client_secret: str


@dataclass(frozen=True)
class LoadResult:
    state: str                      # "present" | "absent" | "invalid"
    source: str                     # "env" | "default" (never the path)
    credentials: Optional[Credentials] = None
    keys: tuple[str, ...] = ()
    file_sha256: Optional[str] = None
    problem: Optional[str] = None   # key NAMES only, never a value


def credentials_path(env: Optional[Mapping[str, str]] = None) -> tuple[Path, str]:
    env = os.environ if env is None else env
    if env.get(ENV_FILE):
        return Path(env[ENV_FILE]), "env"
    profile = env.get("USERPROFILE") or env.get("HOME") or str(Path.home())
    return Path(profile) / DEFAULT_RELATIVE, "default"


def _is_placeholder(value: str) -> bool:
    trimmed = value.strip()
    return (not trimmed
            or trimmed.upper().startswith(("REPLACE_WITH", "PLACEHOLDER"))
            or "<" in trimmed or ">" in trimmed)


def canonical_sha256(document: dict) -> str:
    return hashlib.sha256(json.dumps(
        document, sort_keys=True, separators=(",", ":"),
        ensure_ascii=True).encode("utf-8")).hexdigest()


def load(env: Optional[Mapping[str, str]] = None,
         explicit: Optional[Path] = None) -> LoadResult:
    """Resolve and validate the credential file without ever echoing it."""
    if explicit is not None:
        path, source = Path(explicit), "explicit"
    else:
        path, source = credentials_path(env)
    if not path.is_file():
        return LoadResult("absent", source)
    try:
        if path.stat().st_size > MAX_FILE_BYTES:
            return LoadResult("invalid", source, problem="file is too large")
        document = json.loads(path.read_bytes().decode("utf-8-sig"))
    except (OSError, UnicodeError, json.JSONDecodeError):
        return LoadResult("invalid", source, problem="not a readable UTF-8 JSON file")
    if not isinstance(document, dict):
        return LoadResult("invalid", source, problem="not a JSON object")
    # Only ALLOW-LISTED key names ever leave this function: an unexpected key
    # may itself be a credential (a swapped or malformed mapping), so unknown
    # keys are counted, never echoed.
    keys = tuple(key for key in sorted(KEYS) if key in document)
    digest = canonical_sha256(document)
    missing = [key for key in KEYS if key not in document]
    unknown = sum(1 for key in document if key not in KEYS)
    if missing or unknown:
        return LoadResult("invalid", source, keys=keys, file_sha256=digest,
                          problem=f"keys must be exactly {', '.join(KEYS)}; "
                                  f"missing={missing or 'none'} unknown_keys={unknown}")
    for key in KEYS:
        value = document[key]
        if not isinstance(value, str) or _is_placeholder(value):
            return LoadResult("invalid", source, keys=keys, file_sha256=digest,
                              problem=f"{key} is not a non-empty, non-placeholder string")
        if not _VALUE.fullmatch(value):
            return LoadResult("invalid", source, keys=keys, file_sha256=digest,
                              problem=f"{key} has characters a config row cannot carry")
    return LoadResult("present", source, Credentials(
        product_id=document["ProductId"], sandbox_id=document["SandboxId"],
        deployment_test=document["DeploymentIdTest"],
        deployment_public=document["DeploymentIdPublic"],
        client_id=document["ClientId"], client_secret=document["ClientSecret"]),
        keys=keys, file_sha256=digest)


def artifact_for(configuration: str) -> str:
    return SHIPPING_ARTIFACT if configuration == "Shipping" else TEST_ARTIFACT


def render_block(credentials: Optional[Credentials], configuration: str) -> str:
    """The text appended to the isolated DefaultEngine.ini ("" when absent)."""
    if credentials is None:
        return ""
    shipping = configuration == "Shipping"
    deployment = (credentials.deployment_public if shipping
                  else credentials.deployment_test)
    row = ("+Artifacts=(ArtifactName=\"{name}\",ClientId=\"{client_id}\","
           "ClientSecret=\"{client_secret}\",ProductId=\"{product_id}\","
           "SandboxId=\"{sandbox_id}\",DeploymentId=\"{deployment}\")").format(
        name=artifact_for(configuration), client_id=credentials.client_id,
        client_secret=credentials.client_secret,
        product_id=credentials.product_id, sandbox_id=credentials.sandbox_id,
        deployment=deployment)
    return "\n".join((BEGIN, SECTION, row, END)) + "\n"


def receipt(result: LoadResult, configuration: str) -> dict:
    """Build evidence: state, key set, canonical-file digest. Never a value."""
    record = {
        "format": "wot-eos-credentials-receipt-v1",
        "configuration": configuration,
        "state": result.state,
        "source": result.source,
        "keys": list(result.keys),
        "file_sha256": result.file_sha256,
        "artifact": artifact_for(configuration) if result.state == "present" else None,
        "internet_play": "configured" if result.state == "present" else "disabled",
    }
    if result.problem:
        record["problem"] = result.problem
    return record


def strip_block(text: str) -> str:
    """Remove a previously injected block (idempotent re-injection)."""
    kept = []
    inside = False
    for line in text.splitlines(keepends=True):
        stripped = line.strip()
        if stripped == BEGIN:
            if inside:
                raise CredentialError("nested EOS credential blocks")
            inside = True
            continue
        if stripped == END:
            if not inside:
                raise CredentialError("unbalanced EOS credential block end")
            inside = False
            continue
        if not inside:
            kept.append(line)
    if inside:
        raise CredentialError("unterminated EOS credential block")
    return "".join(kept)


def _has_artifact_rows(text: str) -> bool:
    return any(re.match(r"^\s*[+.!-]?\s*Artifacts\s*=", line)
               for line in text.splitlines())


def apply_to_config(config: Path, result: LoadResult, configuration: str) -> dict:
    """Rewrite the ISOLATED DefaultEngine.ini for one staging run and return
    the receipt. Absent/invalid credentials leave no row (EOS disabled)."""
    original = config.read_text(encoding="utf-8")
    text = strip_block(original)
    if _has_artifact_rows(text):
        raise CredentialError(
            f"{config} carries Artifacts rows outside the injected block; the "
            "repository config must never hold EOS credentials")
    block = render_block(result.credentials if result.state == "present" else None,
                         configuration)
    if (text != original or block) and text:
        # The block is always appended after exactly one blank line; drop the
        # separator a previous injection left so rewrites are idempotent.
        text = text.rstrip("\n") + "\n"
    if block:
        text += "\n" + block
    if text != original:
        temporary = config.with_name(config.name + ".tmp")
        # open(), not write_text(newline=...): that keyword needs Python
        # 3.10, and the Mac host pipeline runs 3.9.
        with open(temporary, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
        temporary.replace(config)
    return receipt(result, configuration)


def stage_identity(record: Optional[dict]) -> dict:
    """The cache-relevant part of a receipt (an absent file and a pre-EOS
    stamp are the same identity, so old caches stay valid)."""
    if not record or record.get("state") != "present":
        return {"state": "disabled"}
    return {"state": "present", "file_sha256": record.get("file_sha256"),
            "artifact": record.get("artifact")}
