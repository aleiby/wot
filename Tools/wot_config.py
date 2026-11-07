#!/usr/bin/env python3
"""Shared paths for private reference data and machine-local tool settings.

This module is deliberately stdlib-only and lives directly under ``Tools`` so
normal Python and Unreal's embedded Python can both import it after adding the
``Tools`` directory to ``sys.path``.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path
from typing import Any, Dict, Optional, Union

PathLike = Union[str, os.PathLike]

TOOLS_DIR = Path(__file__).resolve().parent
REPOSITORY_ROOT = TOOLS_DIR.parent
LOCAL_CONFIG_PATH = TOOLS_DIR / "local.json"


class ConfigurationError(RuntimeError):
    """A requested private input or machine-local setting is unavailable."""


def _absolute(path: PathLike) -> Path:
    return Path(path).expanduser().resolve()


def reference_root(explicit: Optional[PathLike] = None) -> Path:
    """Return the private reference root using CLI, env, repository precedence."""
    if explicit is not None and str(explicit).strip():
        return _absolute(explicit)
    configured = os.environ.get("WOT_REFERENCE_ROOT", "").strip()
    return _absolute(configured) if configured else REPOSITORY_ROOT


def require_path(path: PathLike, description: str = "private reference input",
                 directory: Optional[bool] = None) -> Path:
    """Require a path and give public-checkout users an actionable error."""
    resolved = _absolute(path)
    present = (resolved.is_dir() if directory is True else
               resolved.is_file() if directory is False else resolved.exists())
    if not present:
        raise ConfigurationError(
            "%s is missing at %s; set WOT_REFERENCE_ROOT to the reference root "
            "that contains original/ and exports/ (docs/LOCAL-CONFIGURATION.md)"
            % (description, resolved))
    return resolved


def _reference_dir(name: str, explicit: Optional[PathLike], required: bool) -> Path:
    path = reference_root(explicit) / name
    return require_path(path, "%s reference directory" % name, directory=True) \
        if required else path


def original_dir(explicit: Optional[PathLike] = None, required: bool = True) -> Path:
    return _reference_dir("original", explicit, required)


def devtree_dir(explicit: Optional[PathLike] = None, required: bool = True) -> Path:
    return _reference_dir("devtree", explicit, required)


def exports_dir(explicit: Optional[PathLike] = None, required: bool = True) -> Path:
    return _reference_dir("exports", explicit, required)


def load_local_config(config_path: Optional[PathLike] = None) -> Dict[str, Any]:
    """Load the optional machine-local JSON object without requiring any key."""
    path = _absolute(config_path or LOCAL_CONFIG_PATH)
    if not path.exists():
        return {}
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ConfigurationError("cannot parse machine-local config %s: %s" %
                                 (path, exc)) from exc
    if not isinstance(document, dict):
        raise ConfigurationError("machine-local config %s must contain a JSON object" % path)
    return document


def environment_name(key: str) -> str:
    return "WOT_%s" % key.upper()


def local_value(key: str, *, config_path: Optional[PathLike] = None,
                required: bool = True, default: Optional[Any] = None) -> Any:
    """Read one setting, with its ``WOT_<KEY>`` environment override first."""
    env_name = environment_name(key)
    env_value = os.environ.get(env_name, "").strip()
    if env_value:
        return env_value
    value = load_local_config(config_path).get(key, default)
    if value is None or (isinstance(value, str) and not value.strip()):
        if required:
            raise ConfigurationError(
                "machine-local setting %s is required; set %s or copy "
                "Tools/local.example.json to Tools/local.json and fill it in"
                % (key, env_name))
        return None
    return value


def local_path(key: str, *, config_path: Optional[PathLike] = None,
               required: bool = True, default: Optional[PathLike] = None,
               must_exist: bool = False) -> Optional[Path]:
    value = local_value(key, config_path=config_path, required=required, default=default)
    if value is None:
        return None
    path = Path(value).expanduser()
    if not path.is_absolute():
        path = REPOSITORY_ROOT / path
    path = path.resolve()
    if must_exist and not path.exists():
        raise ConfigurationError(
            "machine-local setting %s points to missing path %s; update %s or %s"
            % (key, path, environment_name(key), config_path or LOCAL_CONFIG_PATH))
    return path


def source_engine_root(platform_name: str, *, required: bool = True) -> Optional[Path]:
    return local_path("source_engine_root_%s" % platform_name.lower(), required=required)


def installed_engine_root(platform_name: str, *, required: bool = True) -> Optional[Path]:
    return local_path("installed_engine_root_%s" % platform_name.lower(), required=required)


def _main(argv: Optional[list] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    get = subparsers.add_parser("get", help="print one required local path")
    get.add_argument("key")
    for command in ("reference-root", "original-dir", "devtree-dir", "exports-dir"):
        subparsers.add_parser(command)
    args = parser.parse_args(argv)
    try:
        if args.command == "get":
            value = local_path(args.key)
        elif args.command == "reference-root":
            value = reference_root()
        elif args.command == "original-dir":
            value = original_dir()
        elif args.command == "devtree-dir":
            value = devtree_dir()
        else:
            value = exports_dir()
    except ConfigurationError as exc:
        print("CONFIGURATION ERROR: %s" % exc, file=sys.stderr)
        return 2
    print(value)
    return 0


if __name__ == "__main__":
    raise SystemExit(_main())
