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
import json
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

# Files that define publication, not image bytes.  Keeping these out of every image
# is important: fixing this planner must not itself trigger six unrelated rebuilds.
ORCHESTRATION = frozenset(
    (
        ".github/workflows/publish-testing.yml",
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
    if not path or path in ORCHESTRATION:
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

    if path.startswith("server-go/"):
        out = SERVER | KB  # both images build the module multicall runtime
        if path in ("server-go/go.mod", "server-go/go.sum") or path.startswith(
            "server-go/modules/control-web/policy/"
        ):
            out |= CONTROL
        return out

    if path.startswith("src/modules/kb_client/"):
        # Makefile's KB_CLIENT_OBJS are linked only into aimee-server.  A regression
        # test enforces that this directory never enters an aimee-kb target unnoticed.
        return SERVER
    if path.startswith("src/"):
        # The C make graph has broad shared closures.  Until a narrower ownership
        # boundary is mechanically proven, source outside the two exclusions above
        # invalidates every C image.  This is conservative, not a guessed omission.
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
