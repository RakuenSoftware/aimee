#!/usr/bin/env python3
"""Check the complete G0 boundary, including native clients outside the module.

Unlike the transitional C allowlist, this gate never treats native adapters as
complete migration. --report prints all remaining violations as JSON for the
cutover inventory; it still exits unsuccessfully when any violation remains.
"""

from __future__ import annotations

import argparse
import json
import os
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MANIFEST = Path("tests/baselines/modules/memory-native-retirement.json")
MEMORY_ROOTS = (Path("src/modules/memory"), Path("server-go/modules/memory"))
NATIVE_SUFFIXES = {".c", ".h", ".cc", ".hh", ".cpp", ".hpp", ".cxx", ".hxx", ".inc", ".inl",
                   ".m", ".mm", ".s", ".asm", ".syso", ".o", ".a", ".so", ".dll"}
SOURCE_SUFFIXES = NATIVE_SUFFIXES - {".syso", ".o", ".a", ".so", ".dll"}
SKIP_DIRECTORIES = {".git", "node_modules", "__pycache__", ".venv", "build"}
COMMENTS_AND_STRINGS = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|/\*.*?\*/|//[^\n]*', re.S)
GO_IMPORT = re.compile(r'\bimport\s+(?:\([^)]*\)|(?:(?:\w+|\.)\s+)?"[^"]*")', re.S)
MEMORY_INCLUDE = re.compile(r'#\s*include\s*[<"][^>"\n]*(?:aimee/memory/|modules/memory/)[^>"\n]*[>"]')

# The immutable source snapshot captured `} mode;` from an anonymous enum nested
# inside memory_temporal_constraint_t as if it were a public typedef. It is a
# member name, not a memory API: rejecting it globally also rejects unrelated
# JSON keys, file modes and other structures. Keep the inventory intact and match
# the defining enum family instead. The enclosing type is independently present
# in native_symbols, so calls, aliases and a renamed copy of this enum all fail.
# Evidence: source_commit a705a6d29468860f5d895a94794de954c25301b6,
# src/modules/memory/memory_core_internal.h, memory_temporal_constraint_t.
CAPTURED_MEMBERS = {"mode": r"MEM_DATE_CONSTRAINT_\w+"}


def without_comments(text: str) -> str:
    # Keep string literals intact: // in a URL is not a comment, and an include
    # path or a dlsym string can restore a native memory dependency too.
    return COMMENTS_AND_STRINGS.sub(
        lambda m: "\n" * m.group().count("\n") if m.group().startswith("/") else m.group(), text
    )


def source_files(root: Path):
    for directory, children, files in os.walk(root, followlinks=False):
        children[:] = sorted(name for name in children if name not in SKIP_DIRECTORIES)
        for name in sorted(files):
            yield Path(directory) / name


def violations(root: Path, manifest: dict) -> list[dict[str, str]]:
    failures = []

    def fail(rule: str, path: Path, detail: str) -> None:
        failures.append({"rule": rule, "file": path.relative_to(root).as_posix(), "detail": detail})

    if manifest.get("version") != 1 or not manifest.get("native_symbols") or not manifest.get("native_files"):
        raise ValueError("missing or invalid immutable native retirement inventory")
    native_patterns = [re.escape(symbol) if symbol not in CAPTURED_MEMBERS
                       else CAPTURED_MEMBERS[symbol] for symbol in manifest["native_symbols"]]
    symbols = re.compile(r"\b(?:" + "|".join(native_patterns)
                         + r"|AIMEE_MEMORY_\w+|aimee_memory_\w+|server_module_memory_\w+"
                         + r"|kb_module_memory_\w+|kb_client_memory_\w+)\b")
    native_names = {Path(p).name for p in manifest["native_files"]}
    native_build_names = native_names | {str(Path(p).with_suffix(".o").name) for p in manifest["native_files"]}
    native_include = re.compile(r'#\s*include\s*[<"](?:[^>"\n]*/)?(?:'
                                + "|".join(re.escape(name) for name in native_names)
                                + r')[>"]')

    for relative in MEMORY_ROOTS:
        directory = root / relative
        if directory.is_symlink():
            fail("memory-source-symlink", directory, "the module root must not be a forwarding link")
        if not directory.is_dir():
            fail("memory-root-missing", directory, "missing module owner")
            continue
        for path in directory.rglob("*"):
            if path.is_symlink():
                fail("memory-source-symlink", path, "forwarding through a symlink is not Go ownership")
            if path.suffix.lower() in NATIVE_SUFFIXES:
                fail("memory-native-file", path, "native files are forbidden in both memory trees")
            if path.suffix == ".go" and path.is_file():
                text = without_comments(path.read_text(encoding="utf-8"))
                if any(re.search(r'"C"', m.group()) for m in GO_IMPORT.finditer(text)):
                    fail("memory-cgo", path, 'import "C" is forbidden')

    descriptor_path = root / "src/modules/memory/module.yaml"
    descriptor = json.loads(descriptor_path.read_text(encoding="utf-8"))
    for role in ("sources", "public_headers", "private_headers", "tests"):
        for value in descriptor.get(role, []):
            if Path(value).suffix.lower() in NATIVE_SUFFIXES:
                fail("memory-native-descriptor", descriptor_path, f"{role}: {value}")

    for path in source_files(root):
        suffix = path.suffix.lower()
        if suffix in SOURCE_SUFFIXES:
            text = without_comments(path.read_text(encoding="utf-8"))
            included = MEMORY_INCLUDE.search(text) or native_include.search(text)
            matched = symbols.search(text)
            if included:
                fail("memory-native-include", path, included.group())
            if matched:
                fail("memory-native-api", path, matched.group())
        elif path.name in {"Makefile", "CMakeLists.txt"} or suffix in {".mk", ".cmake"}:
            for line in path.read_text(encoding="utf-8").splitlines():
                line = line.split("#", 1)[0]
                if any(re.search(r"(?<![\w.-])" + re.escape(name) + r"(?![\w.-])", line)
                       for name in native_build_names):
                    fail("memory-native-build", path, line.strip())
    return sorted(failures, key=lambda failure: (failure["file"], failure["rule"], failure["detail"]))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--report", action="store_true", help="emit JSON without accepting outstanding debt")
    args = parser.parse_args()
    root = args.root.resolve()
    try:
        failures = violations(root, json.loads((root / MANIFEST).read_text(encoding="utf-8")))
    except (ValueError, OSError) as exc:
        print(f"memory-go-only: {exc}")
        return 1
    if args.report:
        print(json.dumps({"complete": not failures, "violations": failures}, indent=2))
    elif failures:
        for failure in failures:
            print("memory-go-only: " + " ".join(f"{key}={value}" for key, value in failure.items()))
    else:
        print("memory-go-only: ok")
    return int(bool(failures))


if __name__ == "__main__":
    raise SystemExit(main())
