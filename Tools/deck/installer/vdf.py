"""Steam VDF readers for the SteamOS installer (stdlib only, Python >= 3.9).

Binary VDF (``userdata/<id>/config/shortcuts.vdf``)
    ``parse_binary`` / ``dump_binary``. Every node keeps its raw key and value
    bytes, so entries the installer does not touch, unknown keys included,
    re-serialize byte for byte. The installer only appends.

    Wire format: a node is a type byte, a NUL-terminated key, then the value.
    0x00 map (children until 0x08), 0x01 NUL-terminated string, 0x02 int32,
    0x03 float32, 0x04 pointer, 0x05 UTF-16LE string ending in 0x0000,
    0x06 color, 0x07 uint64, 0x0A int64 (all little-endian). The whole file is
    one top-level map: its children, then 0x08. Any other type byte is refused,
    because its length is unknown and the file could not be rewritten safely.

Text VDF (``config/loginusers.vdf``)
    ``parse_text`` returns nested dicts (keys as written; ``get_ci`` looks them
    up case-insensitively, as Steam does).
"""
from __future__ import annotations

import struct
from typing import Dict, List, Optional, Union

TYPE_MAP = 0x00
TYPE_STRING = 0x01
TYPE_INT32 = 0x02
TYPE_FLOAT32 = 0x03
TYPE_POINTER = 0x04
TYPE_WSTRING = 0x05
TYPE_COLOR = 0x06
TYPE_UINT64 = 0x07
TYPE_END = 0x08
TYPE_INT64 = 0x0A

_FIXED_SIZES = {
    TYPE_INT32: 4,
    TYPE_FLOAT32: 4,
    TYPE_POINTER: 4,
    TYPE_COLOR: 4,
    TYPE_UINT64: 8,
    TYPE_INT64: 8,
}


class VdfError(ValueError):
    """The VDF data is malformed or uses a type this reader cannot rewrite."""


class Node:
    """One binary VDF node. ``value`` is raw bytes, or a list of child nodes for a map."""

    __slots__ = ("type", "key", "value")

    def __init__(self, node_type: int, key: bytes, value: Union[bytes, List["Node"]]) -> None:
        self.type = node_type
        self.key = key
        self.value = value

    def __repr__(self) -> str:
        return "Node(%#04x, %r, %r)" % (self.type, self.key, self.value)

    @property
    def name(self) -> str:
        return self.key.decode("utf-8", errors="surrogateescape")

    def child(self, key: str) -> Optional["Node"]:
        """The first child whose key matches, ignoring case (maps only)."""
        if self.type != TYPE_MAP:
            return None
        return find(self.value, key)  # type: ignore[arg-type]

    def text(self) -> Optional[str]:
        """A string node's value, or None for any other type."""
        if self.type != TYPE_STRING:
            return None
        return self.value.decode("utf-8", errors="surrogateescape")  # type: ignore[union-attr]

    def uint32(self) -> Optional[int]:
        """An int32 node's value as unsigned 32-bit, or None for any other type."""
        if self.type != TYPE_INT32:
            return None
        return struct.unpack("<I", self.value)[0]  # type: ignore[arg-type]


class Document:
    """A parsed binary VDF file: the top-level nodes plus any bytes after the final 0x08."""

    __slots__ = ("nodes", "trailer")

    def __init__(self, nodes: List[Node], trailer: bytes = b"") -> None:
        self.nodes = nodes
        self.trailer = trailer


def find(nodes: List[Node], key: str) -> Optional[Node]:
    folded = key.casefold()
    for node in nodes:
        if node.name.casefold() == folded:
            return node
    return None


def string_node(key: str, value: str) -> Node:
    raw = value.encode("utf-8")
    if b"\0" in raw:
        raise VdfError("string value contains NUL")
    return Node(TYPE_STRING, _key_bytes(key), raw)


def int32_node(key: str, value: int) -> Node:
    return Node(TYPE_INT32, _key_bytes(key), struct.pack("<I", value & 0xFFFFFFFF))


def map_node(key: str, children: List[Node]) -> Node:
    return Node(TYPE_MAP, _key_bytes(key), list(children))


def _key_bytes(key: str) -> bytes:
    raw = key.encode("utf-8")
    if b"\0" in raw:
        raise VdfError("key contains NUL")
    return raw


def parse_binary(data: bytes) -> Document:
    """Parse a whole binary VDF file. Raises VdfError on anything malformed."""
    nodes, offset = _parse_children(data, 0, 0)
    return Document(nodes, bytes(data[offset:]))


def _read_cstring(data: bytes, offset: int) -> "tuple[bytes, int]":
    end = data.find(b"\0", offset)
    if end < 0:
        raise VdfError("unterminated string at offset %d" % offset)
    return bytes(data[offset:end]), end + 1


def _parse_children(data: bytes, offset: int, depth: int) -> "tuple[List[Node], int]":
    if depth > 64:
        raise VdfError("maps nested too deeply")
    nodes: List[Node] = []
    while True:
        if offset >= len(data):
            raise VdfError("missing map end at offset %d" % offset)
        node_type = data[offset]
        offset += 1
        if node_type == TYPE_END:
            return nodes, offset
        key, offset = _read_cstring(data, offset)
        if node_type == TYPE_MAP:
            children, offset = _parse_children(data, offset, depth + 1)
            nodes.append(Node(node_type, key, children))
        elif node_type == TYPE_STRING:
            value, offset = _read_cstring(data, offset)
            nodes.append(Node(node_type, key, value))
        elif node_type == TYPE_WSTRING:
            end = offset
            while True:
                if end + 1 >= len(data):
                    raise VdfError("unterminated wide string at offset %d" % offset)
                if data[end] == 0 and data[end + 1] == 0:
                    break
                end += 2
            nodes.append(Node(node_type, key, bytes(data[offset:end])))
            offset = end + 2
        elif node_type in _FIXED_SIZES:
            size = _FIXED_SIZES[node_type]
            if offset + size > len(data):
                raise VdfError("truncated value at offset %d" % offset)
            nodes.append(Node(node_type, key, bytes(data[offset:offset + size])))
            offset += size
        else:
            raise VdfError("unknown value type %#04x at offset %d" % (node_type, offset - 1))


def dump_binary(document: Document) -> bytes:
    out = bytearray()
    _dump_children(document.nodes, out)
    out += document.trailer
    return bytes(out)


def _dump_children(nodes: List[Node], out: bytearray) -> None:
    for node in nodes:
        out.append(node.type)
        out += node.key
        out.append(0)
        if node.type == TYPE_MAP:
            _dump_children(node.value, out)  # type: ignore[arg-type]
        elif node.type == TYPE_STRING:
            out += node.value  # type: ignore[operator]
            out.append(0)
        elif node.type == TYPE_WSTRING:
            out += node.value  # type: ignore[operator]
            out += b"\0\0"
        elif node.type in _FIXED_SIZES:
            if len(node.value) != _FIXED_SIZES[node.type]:  # type: ignore[arg-type]
                raise VdfError("wrong value size for key %r" % node.name)
            out += node.value  # type: ignore[operator]
        else:
            raise VdfError("unknown value type %#04x" % node.type)
    out.append(TYPE_END)


# --- Text VDF ---------------------------------------------------------------

TextVdf = Dict[str, Union[str, "TextVdf"]]


def parse_text(text: str) -> TextVdf:
    """Parse text VDF into nested dicts. A later duplicate key replaces an earlier one."""
    tokens = _tokenize(text)
    root, index = _parse_text_map(tokens, 0, top=True)
    return root


def get_ci(mapping: TextVdf, key: str):
    """The value for key, ignoring case, or None."""
    folded = key.casefold()
    for name, value in mapping.items():
        if name.casefold() == folded:
            return value
    return None


_OPEN, _CLOSE, _STRING = "{", "}", "s"


def _tokenize(text: str) -> "List[tuple[str, str]]":
    tokens: "List[tuple[str, str]]" = []
    i, n = 0, len(text)
    escapes = {"n": "\n", "t": "\t", "\\": "\\", '"': '"'}
    while i < n:
        ch = text[i]
        if ch in " \t\r\n\ufeff":
            i += 1
        elif text.startswith("//", i):
            end = text.find("\n", i)
            i = n if end < 0 else end + 1
        elif ch == "{":
            tokens.append((_OPEN, ch))
            i += 1
        elif ch == "}":
            tokens.append((_CLOSE, ch))
            i += 1
        elif ch == "[":
            # A platform conditional such as [$WIN32]; it carries no data here.
            end = text.find("]", i)
            if end < 0:
                raise VdfError("unterminated conditional")
            i = end + 1
        elif ch == '"':
            i += 1
            parts = []
            while True:
                if i >= n:
                    raise VdfError("unterminated quoted string")
                ch = text[i]
                if ch == '"':
                    i += 1
                    break
                if ch == "\\" and i + 1 < n:
                    parts.append(escapes.get(text[i + 1], "\\" + text[i + 1]))
                    i += 2
                    continue
                parts.append(ch)
                i += 1
            tokens.append((_STRING, "".join(parts)))
        else:
            start = i
            while i < n and text[i] not in ' \t\r\n{}"':
                i += 1
            tokens.append((_STRING, text[start:i]))
    return tokens


def _parse_text_map(tokens, index: int, top: bool) -> "tuple[TextVdf, int]":
    result: TextVdf = {}
    while True:
        if index >= len(tokens):
            if top:
                return result, index
            raise VdfError("missing closing brace")
        kind, value = tokens[index]
        if kind == _CLOSE:
            if top:
                raise VdfError("unexpected closing brace")
            return result, index + 1
        if kind != _STRING:
            raise VdfError("expected a key")
        key = value
        index += 1
        if index >= len(tokens):
            raise VdfError("key %r has no value" % key)
        kind, value = tokens[index]
        if kind == _OPEN:
            child, index = _parse_text_map(tokens, index + 1, top=False)
            result[key] = child
        elif kind == _STRING:
            result[key] = value
            index += 1
        else:
            raise VdfError("key %r has no value" % key)
