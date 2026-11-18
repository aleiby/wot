#!/usr/bin/env python3
"""One-shot request to a running guarded Test instance's net-test channel.

    python probe.py --port 8187 snapshot
    python probe.py --port 8188 exec c="stat fps"
    python probe.py --port 8187 give pawn=WoTAesSedai_0 class=WoTAngrealInvHeal charges=5

The channel accepts several authenticated connections, so this can run while
a driver holds its own connection (live diagnosis of a held instance).
"""
from __future__ import annotations

import argparse
import json
import socket
import sys


def request(port: int, token: str, command: str, fields: dict, timeout: float = 30.0):
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as sock:
        sock.settimeout(timeout)
        sock.sendall((json.dumps({"token": token}) + "\n").encode())
        sock.sendall((json.dumps({"id": 1, "cmd": command, **fields}) + "\n").encode())
        buf = b""
        while b"\n" not in buf:
            chunk = sock.recv(1 << 20)
            if not chunk:
                break
            buf += chunk
        return json.loads(buf.split(b"\n", 1)[0])


def parse_fields(pairs):
    fields = {}
    for pair in pairs:
        key, _, value = pair.partition("=")
        try:
            fields[key] = json.loads(value)
        except ValueError:
            fields[key] = value
    return fields


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--token", default="wotangreal")
    parser.add_argument("command")
    parser.add_argument("fields", nargs="*")
    args = parser.parse_args(argv)
    response = request(args.port, args.token, args.command, parse_fields(args.fields))
    print(json.dumps(response, indent=1))
    return 0 if response.get("ok") else 1


if __name__ == "__main__":
    sys.exit(main())
