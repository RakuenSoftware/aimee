#!/usr/bin/env python3
"""Plan testing-channel image publication from the files that actually changed.

The testing channel has one moving tag and one immutable tag per source revision.
An unchanged image does not need to be rebuilt to participate in a new channel
revision: its existing ``:testing`` manifest can be given the new immutable tag.

This classifier is deliberately conservative.  Known image inputs select only the
images that consume them; a path not covered by the contract selects every image.
That makes adding a new build input expensive until it is classified, never silent.
"""

from __future__ import annotations

import argparse
import functools
import json
import pathlib
import re
import subprocess
import sys
from dataclasses import dataclass


@dataclass(frozen=True)
class Image:
    name: str
    dockerfile: str
    args: str = ""


IMAGES = (
    Image("aimee-server", "Dockerfile.server"),
    Image("aimee-control-web", "Dockerfile.control-web"),
    Image("aimee-kb", "Dockerfile", "AIMEE_EMBEDDER=none"),
    Image("aimee-kb-a25m", "Dockerfile", "AIMEE_EMBEDDER=bekko-a25m"),
    Image(
        "aimee-kb-nomic",
        "Dockerfile",
        "AIMEE_EMBEDDER=nomic-embed-text-v2-moe",
    ),
    Image("aimee-authority-bootstrap", "Dockerfile.authority-bootstrap"),
)

ALL = frozenset(image.name for image in IMAGES)
SERVER = frozenset(("aimee-server",))
CONTROL = frozenset(("aimee-control-web",))
KB = frozenset(("aimee-kb", "aimee-kb-a25m", "aimee-kb-nomic"))
AUTHORITY = frozenset(("aimee-authority-bootstrap",))

ROOT = pathlib.Path(__file__).resolve().parents[1]


@functools.lru_cache(maxsize=1)
def _shipping_object_closures() -> dict[str, frozenset[str]] | None:
    """Expand the transitive C link graph for every published runtime image."""

    try:
        proc = subprocess.run(
            ["make", "-C", str(ROOT / "src"), "-pnRrq"],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            timeout=30,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    if not proc.stdout:
        return None

    rules: dict[str, set[str]] = {}
    assignments = {"=", ":=", "::=", "?=", "+="}
    for line in proc.stdout.splitlines():
        if not line or line[0].isspace() or line.startswith("#") or ":" not in line:
            continue
        left, right = line.split(":", 1)
        prerequisites = right.split("|", 1)[0].split()
        if any(token in assignments for token in prerequisites):
            continue
        for target in left.split():
            rules.setdefault(target, set()).update(prerequisites)

    roots = {
        "aimee-server": ("../aimee-server",),
        "aimee-kb": ("../aimee-kb", "../aimee-kb-worm"),
        "aimee-kb-a25m": ("../aimee-kb", "../aimee-kb-worm"),
        "aimee-kb-nomic": ("../aimee-kb", "../aimee-kb-worm"),
        "aimee-authority-bootstrap": (
            "../aimee-kb-token-roots-provision",
            "../aimee-kb-jwks-publish",
        ),
    }
    closures: dict[str, frozenset[str]] = {}
    for image, image_roots in roots.items():
        seen: set[str] = set()
        pending = list(image_roots)
        while pending:
            target = pending.pop()
            if target in seen:
                continue
            seen.add(target)
            pending.extend(rules.get(target, ()))
        closures[image] = frozenset(item for item in seen if item.endswith(".o"))
    if any(not closures[image] for image in roots):
        return None
    return closures


def _c_source_consumers(path: str) -> frozenset[str]:
    """Map a C source to images containing any of its compiled object variants."""

    closures = _shipping_object_closures()
    if closures is None:
        return ALL
    stem = path.removeprefix("src/").removesuffix(".c")
    candidates = {
        f"build/obj/{stem}.o",
        f"build/obj/server/{stem}.o",
        f"build/obj/kb/{stem}.o",
    }
    return frozenset(
        image for image, objects in closures.items() if not candidates.isdisjoint(objects)
    )


@functools.lru_cache(maxsize=1)
def _shipping_header_consumers() -> dict[str, frozenset[str]]:
    """Attribute checked-in headers through the shipping source include graph.

    Ambiguous include names are deliberately resolved to every matching header,
    which can cause an extra rebuild but cannot omit a real consumer.
    """

    source_root = ROOT / "src"
    files = sorted(source_root.rglob("*.c")) + sorted(source_root.rglob("*.h"))
    relative = {path: path.relative_to(source_root).as_posix() for path in files}
    aliases: dict[str, set[str]] = {}
    for path, rel in relative.items():
        if path.suffix != ".h":
            continue
        aliases.setdefault(rel, set()).add(rel)
        aliases.setdefault(path.name, set()).add(rel)
        if "/include/" in rel:
            aliases.setdefault(rel.split("/include/", 1)[1], set()).add(rel)

    include_re = re.compile(r'^\s*#\s*include\s*[<"]([^">]+)[">]', re.MULTILINE)
    edges: dict[str, set[str]] = {}
    for path, rel in relative.items():
        try:
            names = include_re.findall(path.read_text(encoding="utf-8", errors="ignore"))
        except OSError:
            continue
        resolved: set[str] = set()
        for name in names:
            local = (path.parent / name).resolve()
            try:
                if local.is_file() and local.is_relative_to(source_root):
                    resolved.add(local.relative_to(source_root).as_posix())
            except OSError:
                pass
            resolved.update(aliases.get(name, ()))
        edges[rel] = resolved

    consumers: dict[str, set[str]] = {}
    for path, rel in relative.items():
        if path.suffix != ".c":
            continue
        images = _c_source_consumers(f"src/{rel}")
        if not images:
            continue
        seen: set[str] = set()
        pending = list(edges.get(rel, ()))
        while pending:
            header = pending.pop()
            if header in seen:
                continue
            seen.add(header)
            pending.extend(edges.get(header, ()))
        for header in seen:
            consumers.setdefault(f"src/{header}", set()).update(images)
    return {path: frozenset(images) for path, images in consumers.items()}


def _header_consumers(path: str) -> frozenset[str]:
    consumers = _shipping_header_consumers()
    if path in consumers:
        return consumers[path]
    # A present header with no include edge is not an image input. A deleted or
    # otherwise unresolved header fails closed because its former edges are gone.
    return frozenset() if (ROOT / path).is_file() else ALL

# Files that define publication, not image bytes.  Keeping these out of every image
# is important: fixing this planner must not itself trigger six unrelated rebuilds.
PUBLISH_TOOLING = frozenset(
    (
        ".github/workflows/ci.yml",
        ".github/workflows/publish-testing.yml",
        "scripts/check-published-compose-images.py",
        "scripts/publish_testing_plan.py",
        "scripts/tests/test_publish_testing_plan.py",
    )
)

KB_CONTAINER_FILES = frozenset(
    (
        "deploy/container/aimee.yaml",
        "deploy/container/aimee-kb-entrypoint.sh",
        "deploy/container/optional-modules-lib.sh",
        "deploy/container/module-supervisor.sh",
        "deploy/container/aimee-kb-db-export.sh",
    )
)

SERVER_CONTAINER_FILES = frozenset(
    (
        "deploy/container/aimee-managed.compose.yaml",
        "deploy/container/pam.d-aimee",
        "deploy/container/runtime-web-lib.sh",
        "deploy/container/optional-modules-lib.sh",
        "deploy/container/plane-supervisor.sh",
        "deploy/container/module-supervisor.sh",
        "deploy/container/core-storage.sh",
        "deploy/container/server-entrypoint.sh",
        "deploy/container/aimee-server.yaml",
    )
)

AUTHORITY_CONTAINER_FILES = frozenset(
    ("deploy/container/aimee-managed-authority-bootstrap.sh",)
)


def consumers(path: str) -> frozenset[str]:
    """Return image names whose declared inputs include *path*.

    Empty means a known non-image input.  ALL is the fail-safe for an unknown path.
    """

    path = path.removeprefix("./")
    if not path or path in PUBLISH_TOOLING:
        return frozenset()
    if path.startswith("src/tests/") or path.startswith("scripts/tests/"):
        return frozenset()

    if path == ".dockerignore":
        return ALL
    if path == "Dockerfile":
        return KB
    if path == "Dockerfile.server":
        return SERVER
    if path == "Dockerfile.control-web":
        return CONTROL
    if path == "Dockerfile.authority-bootstrap":
        return AUTHORITY

    if path.startswith("control-web/"):
        return CONTROL
    if path.startswith("frontend/"):
        return SERVER | CONTROL
    if path.startswith("runtime-web/"):
        return SERVER
    if path.startswith("config/"):
        return SERVER

    if path == "api/openapi-server-v1.yaml":
        return SERVER
    if path == "api/openapi-v1.yaml":
        return KB
    if path.startswith("api/"):
        # Generated SDKs and API documentation are publication artifacts, not
        # inputs to any runtime image.
        return frozenset()

    if path.startswith("server-go/"):
        out = SERVER | KB  # both images build the module multicall runtime
        if path in ("server-go/go.mod", "server-go/go.sum") or path.startswith(
            "server-go/modules/control-web/policy/"
        ):
            out |= CONTROL
        return out

    if path.endswith(".c") and path.startswith("src/"):
        return _c_source_consumers(path)
    if path.endswith(".h") and path.startswith("src/"):
        return _header_consumers(path)
    if path.startswith("src/modules/kb_client/"):
        # Non-C files in the thin KB client are server-owned. C ownership comes
        # from the expanded shipping link graph above.
        return SERVER
    if path.startswith("src/server/"):
        return SERVER
    if path == "src/gen_openapi_server.py":
        return SERVER
    if path == "src/gen_openapi.py":
        return KB
    if path.startswith("src/"):
        # Header and generated-input dependencies are not fully represented by
        # link objects, so non-C source inputs remain conservative.
        return SERVER | KB | AUTHORITY

    if path in KB_CONTAINER_FILES:
        out = KB
        if path in SERVER_CONTAINER_FILES:
            out |= SERVER
        return out
    if path in SERVER_CONTAINER_FILES:
        return SERVER
    if path in AUTHORITY_CONTAINER_FILES:
        return AUTHORITY
    if path.startswith("deploy/container/"):
        return ALL

    if path.startswith("scripts/"):
        # Build/export scripts and runtime helpers are copied by the C images.  A
        # future script is therefore all-C until this contract classifies it.
        return SERVER | KB | AUTHORITY

    # publish-testing's trigger should make this rare.  If its path list expands,
    # rebuild everything until the new input is deliberately assigned above.
    return ALL


def plan(paths: list[str], force_all: bool = False) -> list[dict[str, object]]:
    affected = set(ALL if force_all else ())
    for path in paths:
        affected.update(consumers(path))
    return [
        {
            "name": image.name,
            "dockerfile": image.dockerfile,
            "args": image.args,
            "rebuild": image.name in affected,
        }
        for image in IMAGES
    ]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--all", action="store_true", help="rebuild every image (manual recovery)"
    )
    parser.add_argument(
        "--null",
        action="store_true",
        help="read NUL-separated paths from stdin (the format of git diff -z)",
    )
    args = parser.parse_args()
    raw = sys.stdin.buffer.read()
    separator = b"\0" if args.null else b"\n"
    paths = [item.decode("utf-8") for item in raw.split(separator) if item]
    result = plan(paths, force_all=args.all)
    rebuilt = [item["name"] for item in result if item["rebuild"]]
    reused = [item["name"] for item in result if not item["rebuild"]]
    print(f"rebuild: {', '.join(rebuilt) or '(none)'}", file=sys.stderr)
    print(f"reuse: {', '.join(reused) or '(none)'}", file=sys.stderr)
    print(json.dumps(result, separators=(",", ":")))


if __name__ == "__main__":
    main()
