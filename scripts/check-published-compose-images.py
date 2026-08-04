#!/usr/bin/env python3
"""Ensure every GHCR image in a root Compose topology is published."""

from __future__ import annotations

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
COMPOSE_IMAGE = re.compile(r"ghcr\.io/rakuensoftware/([a-z0-9-]+)")
MATRIX_ENTRY = r"\{\s*name:\s*%s\s*,"
WORKFLOWS = (
    ROOT / ".github/workflows/publish-testing.yml",
    ROOT / ".github/workflows/publish-images.yml",
)
BUNDLED_KB_COMPOSE = (ROOT / "compose.yaml", ROOT / "compose.server.yaml")
BUNDLED_KB_IMAGE = "ghcr.io/rakuensoftware/aimee-kb-a25m:"


class PublisherError(RuntimeError):
    """A Compose image has no coherent publishing path."""


def validate(root: Path = ROOT) -> tuple[int, int]:
    images: set[str] = set()
    for path in root.glob("compose*.yaml"):
        images.update(COMPOSE_IMAGE.findall(path.read_text(encoding="utf-8")))

    errors: list[str] = []
    for relative in (Path("compose.yaml"), Path("compose.server.yaml")):
        compose = root / relative
        if BUNDLED_KB_IMAGE not in compose.read_text(encoding="utf-8"):
            errors.append(
                f"{relative} selects bekko-a25m but does not default "
                "to the bundled aimee-kb-a25m image"
            )
    workflows = tuple(root / path.relative_to(ROOT) for path in WORKFLOWS)
    for workflow in workflows:
        text = workflow.read_text(encoding="utf-8")
        for image in sorted(images):
            if not re.search(MATRIX_ENTRY % re.escape(image), text):
                errors.append(f"{workflow.relative_to(root)} does not publish {image}")

    if errors:
        raise PublisherError("\n".join(errors))
    return len(images), len(workflows)


def main() -> int:
    try:
        image_count, workflow_count = validate()
    except PublisherError as exc:
        for error in str(exc).splitlines():
            print(f"published-compose-images: ERROR {error}", file=sys.stderr)
        return 1
    print(
        "published-compose-images: ok "
        f"({image_count} compose image(s), {workflow_count} publisher(s))"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
