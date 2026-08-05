#!/usr/bin/env python3
"""Check documentation links, images, structure, and mechanical voice rules."""

from __future__ import annotations

import re
import subprocess
import sys
import urllib.parse
import xml.etree.ElementTree as ET
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
SKIP_PARTS = {
    ".aimee",
    ".claude",
    ".git",
    "node_modules",
    "scratchpad",
    "vendor",
}
ARCHIVE_PREFIXES = (
    Path("benchmarks"),
    Path("docs/dev"),
    Path("docs/proposals"),
    Path("docs/validation"),
)
NON_PUBLIC_PREFIXES = (
    Path(".github"),
    Path("skills"),
    Path("src/tests"),
    Path("src/tool_prompts"),
)
ROOT_GUIDES = {
    Path("CONTRIBUTING.md"),
    Path("MANUAL.md"),
    Path("OWNERS.md"),
    Path("README.md"),
}
OPENAPI_DOCS = (
    ROOT / "api/openapi-v1.yaml",
    ROOT / "api/openapi-server-v1.yaml",
)
COMPONENT_GUIDE_PREFIXES = (
    Path("deploy"),
    Path("control-web"),
    Path("data"),
    Path("editors"),
    Path("runtime-web"),
    Path("scripts"),
    Path("server-go"),
    Path("src"),
)
BANNED = re.compile(
    r"\b(?:revolutionary|game[ -]changing|seamless|powerful|robust|"
    r"very|extremely|incredibly|massively)\b",
    re.IGNORECASE,
)
INLINE_LINK = re.compile(r"!?(?:\[[^\]]*\])\((<[^>]+>|[^\s)]+)(?:\s+[^)]*)?\)")
REFERENCE_TARGET = re.compile(r"^\s*\[[^\]]+\]:\s*(<[^>]+>|\S+)")
IMAGE = re.compile(r"!\[([^\]]*)\]\((<[^>]+>|[^\s)]+)(?:\s+[^)]*)?\)")
HTML_IMAGE = re.compile(r"<img\b[^>]*\bsrc=[\"']([^\"']+)[\"'][^>]*>", re.IGNORECASE)
HTML_ALT = re.compile(r"\balt=[\"']([^\"']*)[\"']", re.IGNORECASE)
SCHEME = re.compile(r"^[a-z][a-z0-9+.-]*:", re.IGNORECASE)
EMOJI = re.compile("[\U0001F300-\U0001FAFF]")


def relative(path: Path) -> Path:
    return path.relative_to(ROOT)


def under(path: Path, prefixes: tuple[Path, ...]) -> bool:
    rel = relative(path)
    return any(rel == prefix or prefix in rel.parents for prefix in prefixes)


def markdown_files() -> list[Path]:
    result = subprocess.run(
        ["git", "ls-files", "*.md"],
        cwd=ROOT,
        check=True,
        capture_output=True,
        text=True,
    )
    paths = {ROOT / line for line in result.stdout.splitlines() if line and (ROOT / line).exists()}
    # Include intentional new guides before their first commit.
    for relative_path in (
        Path("docs/KB_FLEET.md"),
        Path("docs/WRITING.md"),
        Path("docs/runbooks/change-embedder.md"),
    ):
        path = ROOT / relative_path
        if path.exists():
            paths.add(path)
    return sorted(
        path for path in paths
        if not any(part in SKIP_PARTS for part in relative(path).parts)
    )


def is_maintained(path: Path) -> bool:
    rel = relative(path)
    if rel in ROOT_GUIDES:
        return True
    if under(path, ARCHIVE_PREFIXES) or under(path, NON_PUBLIC_PREFIXES):
        return False
    if rel.parts and rel.parts[0] == "docs":
        return True
    return path.name == "README.md" and any(
        rel == prefix or prefix in rel.parents for prefix in COMPONENT_GUIDE_PREFIXES
    )


def visible_lines(text: str):
    """Yield non-fenced prose with source line numbers."""
    fenced = False
    marker = ""
    for number, line in enumerate(text.splitlines(), 1):
        stripped = line.lstrip()
        if not fenced and (stripped.startswith("```") or stripped.startswith("~~~")):
            fenced = True
            marker = stripped[:3]
            continue
        if fenced:
            if stripped.startswith(marker):
                fenced = False
            continue
        yield number, line


def prose(line: str) -> str:
    return re.sub(r"`[^`]*`", "", line)


def clean_target(raw: str) -> str:
    raw = raw.strip()
    if raw.startswith("<") and raw.endswith(">"):
        raw = raw[1:-1]
    return urllib.parse.unquote(raw.split("#", 1)[0].split("?", 1)[0])


def local_target(source: Path, raw: str) -> Path | None:
    target = clean_target(raw)
    if not target or target.startswith("#") or target.startswith("//") or SCHEME.match(target):
        return None
    if target.startswith("/"):
        return ROOT / target.lstrip("/")
    return source.parent / target


def add_error(errors: list[str], path: Path, line: int, message: str) -> None:
    errors.append(f"{relative(path)}:{line}: {message}")


def check_voice(errors: list[str], path: Path, number: int, line: str) -> None:
    plain = prose(line)
    if "—" in plain:
        add_error(errors, path, number, "em dash violates the project voice")
    match = BANNED.search(plain)
    if match:
        add_error(errors, path, number, f"avoid '{match.group(0)}' in maintained prose")
    if EMOJI.search(plain):
        add_error(errors, path, number, "emoji violates the project voice")


def check_link(errors: list[str], source: Path, line: int, raw: str, images: set[Path]) -> None:
    target = local_target(source, raw)
    if target is None:
        return
    resolved = target.resolve(strict=False)
    try:
        resolved.relative_to(ROOT.resolve())
    except ValueError:
        add_error(errors, source, line, f"local link escapes the repository: {raw}")
        return
    if not resolved.exists():
        add_error(errors, source, line, f"missing local target: {raw}")
        return
    if resolved.suffix.lower() in {".avif", ".gif", ".jpeg", ".jpg", ".png", ".svg", ".webp"}:
        images.add(resolved)


def check_file(path: Path, errors: list[str], images: set[Path]) -> None:
    try:
        text = path.read_text(encoding="utf-8")
    except UnicodeDecodeError as exc:
        errors.append(f"{relative(path)}: invalid UTF-8 at byte {exc.start}")
        return
    lines = list(visible_lines(text))

    if is_maintained(path):
        h1 = [(number, line) for number, line in lines if re.match(r"^#\s+\S", line)]
        if len(h1) != 1:
            add_error(errors, path, h1[0][0] if h1 else 1, f"expected one H1, found {len(h1)}")
        for number, line in lines:
            check_voice(errors, path, number, line)

    if not is_maintained(path):
        return

    for number, line in lines:
        for match in INLINE_LINK.finditer(line):
            check_link(errors, path, number, match.group(1), images)
        definition = REFERENCE_TARGET.match(line)
        if definition:
            check_link(errors, path, number, definition.group(1), images)
        for match in IMAGE.finditer(line):
            if not match.group(1).strip():
                add_error(errors, path, number, "image needs descriptive alt text")
        for match in HTML_IMAGE.finditer(line):
            check_link(errors, path, number, match.group(1), images)
            tag = match.group(0)
            alt = HTML_ALT.search(tag)
            if alt is None or not alt.group(1).strip():
                add_error(errors, path, number, "HTML image needs descriptive alt text")


def check_svg(path: Path, errors: list[str]) -> None:
    try:
        root = ET.parse(path).getroot()
    except ET.ParseError as exc:
        errors.append(f"{relative(path)}: invalid SVG: {exc}")
        return
    tags = {element.tag.rsplit("}", 1)[-1] for element in root.iter()}
    if "title" not in tags or "desc" not in tags:
        errors.append(f"{relative(path)}: SVG needs title and desc elements")


def main() -> int:
    errors: list[str] = []
    images: set[Path] = set()
    files = markdown_files()
    for path in files:
        check_file(path, errors, images)
    for path in OPENAPI_DOCS:
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            check_voice(errors, path, number, line)
    for path in sorted(images):
        if path.suffix.lower() == ".svg":
            check_svg(path, errors)

    if errors:
        print("check-docs: failed")
        for error in errors:
            print(f"  {error}")
        return 1
    maintained = sum(1 for path in files if is_maintained(path))
    print(
        f"check-docs: ok ({len(files)} Markdown files, "
        f"{maintained} maintained guides, {len(OPENAPI_DOCS)} OpenAPI sources, "
        f"{len(images)} local images)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
