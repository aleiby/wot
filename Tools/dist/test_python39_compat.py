"""Host-side tools must run on the Mac host's Python 3.9 (Xcode's interpreter).

Windows and the Linux VM run newer interpreters, so a 3.10+ construct passes
every local test there and only breaks the Mac packaging lane (past breaks:
a runtime ``X | Y`` annotation in light_animation.py and two
``write_text(newline=)`` calls). This test pins the floor on every lane.

Scope: Tools/dist, Tools/build, Tools/convert and Tools/public run on the host
interpreter, as do the play-through gate (Tools/playthrough) and the rig and
match driver it launches through (Tools/trailer, Tools/test/battle). Tools/import normally runs inside UE's embedded Python 3.11 and is
exempt, although its standalone tests are still kept compatible where practical.

Checks:
1. every module parses with the 3.9 grammar (``ast.parse(feature_version)``);
2. no 3.10+ keyword arguments we have tripped over (``zip(strict=)``,
   ``write_text``/``read_text`` ``newline=``);
3. no ``typing`` names that 3.9 lacks;
4. no ``X | Y`` union in an annotation that 3.9 evaluates at runtime
   (function signatures and module/class-level annotated assignments) unless
   the module has ``from __future__ import annotations``.
"""
import ast
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
HOST_ROOTS = ("Tools/dist", "Tools/build", "Tools/convert", "Tools/public",
              "Tools/playthrough", "Tools/trailer", "Tools/test/battle")

NEWER_KEYWORDS = {
    ("zip", "strict"),
    ("write_text", "newline"),
    ("read_text", "newline"),
    ("rmtree", "onexc"),
}
NEWER_TYPING_NAMES = {
    "TypeAlias", "ParamSpec", "Concatenate", "TypeGuard",       # 3.10
    "Self", "Never", "LiteralString", "Required", "NotRequired",  # 3.11
    "Unpack", "TypeVarTuple", "assert_never", "assert_type", "reveal_type",
    "dataclass_transform", "override", "TypeIs", "ReadOnly",
}


def _modules():
    for root in HOST_ROOTS:
        for path in sorted((REPO / root).rglob("*.py")):
            if "__pycache__" not in path.parts:
                yield path


def _called_name(func):
    if isinstance(func, ast.Attribute):
        return func.attr
    if isinstance(func, ast.Name):
        return func.id
    return None


def _has_union(annotation):
    return annotation is not None and any(
        isinstance(node, ast.BinOp) and isinstance(node.op, ast.BitOr)
        for node in ast.walk(annotation))


def _runtime_annotations(tree):
    """Annotations Python 3.9 evaluates when the module runs."""
    def visit(body, in_function):
        for node in body:
            if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
                arguments = node.args
                for arg in (arguments.posonlyargs + arguments.args
                            + arguments.kwonlyargs
                            + [a for a in (arguments.vararg, arguments.kwarg) if a]):
                    yield arg.annotation
                yield node.returns
                yield from visit(node.body, True)
            elif isinstance(node, ast.ClassDef):
                yield from visit(node.body, in_function)
            elif isinstance(node, ast.AnnAssign) and not in_function:
                yield node.annotation
            elif hasattr(node, "body") and isinstance(node.body, list):
                yield from visit(node.body, in_function)
                for extra in ("orelse", "finalbody"):
                    yield from visit(getattr(node, extra, []) or [], in_function)
                for handler in getattr(node, "handlers", []) or []:
                    yield from visit(handler.body, in_function)
    yield from visit(tree.body, False)


def _future_annotations(tree):
    return any(isinstance(node, ast.ImportFrom) and node.module == "__future__"
               and any(alias.name == "annotations" for alias in node.names)
               for node in tree.body)


class Python39CompatibilityTests(unittest.TestCase):
    def _trees(self):
        for path in _modules():
            source = path.read_text(encoding="utf-8-sig")
            yield path.relative_to(REPO), source

    def test_every_host_tool_parses_with_the_python39_grammar(self):
        failures = []
        for relative, source in self._trees():
            try:
                ast.parse(source, filename=str(relative), feature_version=(3, 9))
            except SyntaxError as error:
                failures.append(f"{relative}:{error.lineno}: {error.msg}")
        self.assertEqual(failures, [])

    def test_no_python310_library_keywords_or_typing_names(self):
        failures = []
        for relative, source in self._trees():
            for node in ast.walk(ast.parse(source)):
                if isinstance(node, ast.Call):
                    name = _called_name(node.func)
                    for keyword in node.keywords:
                        if (name, keyword.arg) in NEWER_KEYWORDS:
                            failures.append(f"{relative}:{node.lineno}: "
                                            f"{name}({keyword.arg}=...) needs 3.10+")
                elif isinstance(node, ast.ImportFrom) and node.module == "typing":
                    for alias in node.names:
                        if alias.name in NEWER_TYPING_NAMES:
                            failures.append(f"{relative}:{node.lineno}: "
                                            f"typing.{alias.name} needs 3.10+")
                elif (isinstance(node, ast.Attribute) and node.attr in NEWER_TYPING_NAMES
                      and isinstance(node.value, ast.Name) and node.value.id == "typing"):
                    failures.append(f"{relative}:{node.lineno}: "
                                    f"typing.{node.attr} needs 3.10+")
        self.assertEqual(failures, [])

    def test_no_runtime_evaluated_union_annotations(self):
        failures = []
        for relative, source in self._trees():
            tree = ast.parse(source)
            if _future_annotations(tree):
                continue
            for annotation in _runtime_annotations(tree):
                if _has_union(annotation):
                    failures.append(f"{relative}:{annotation.lineno}: "
                                    "X | Y annotation is evaluated at runtime on 3.9")
        self.assertEqual(failures, [])


if __name__ == "__main__":
    unittest.main()
