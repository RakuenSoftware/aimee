#!/usr/bin/env python3
"""Ensure every GHCR image in a shipped Compose topology is published."""

from __future__ import annotations

import ast
from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
INTERPOLATION = re.compile(r"\$\{([A-Z0-9_]+):-([^}]*)\}")
COMPOSE_IMAGE = re.compile(r"ghcr\.io/rakuensoftware/([a-z0-9-]+)")
IMAGE_LINE = re.compile(r"^\s*image:\s*(\S.*?)\s*$", re.MULTILINE)
MATRIX_ENTRY = r"\{\s*name:\s*%s\s*,"
WORKFLOWS = (
    ROOT / ".github/workflows/publish-testing.yml",
    ROOT / ".github/workflows/publish-images.yml",
)
LLM_WORKFLOW = ROOT / ".github/workflows/publish-llm.yml"
TESTING_PLANNER = ROOT / "scripts/publish_testing_plan.py"
BUNDLED_KB_COMPOSE = (ROOT / "compose.yaml", ROOT / "compose.server.yaml")
BUNDLED_KB_IMAGE = "ghcr.io/rakuensoftware/aimee-kb-a25m:"


class PublisherError(RuntimeError):
    """A Compose image has no coherent publishing path."""


def testing_planner_images(root: Path) -> set[str]:
    """Read the canonical testing matrix without executing repository code."""

    path = root / TESTING_PLANNER.relative_to(ROOT)
    if not path.exists():
        raise PublisherError(f"{path.relative_to(root)} is missing")
    tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    for node in tree.body:
        if not isinstance(node, ast.Assign) or not any(
            isinstance(target, ast.Name) and target.id == "IMAGES"
            for target in node.targets
        ):
            continue
        if not isinstance(node.value, (ast.Tuple, ast.List)):
            break
        names: set[str] = set()
        for entry in node.value.elts:
            if (
                isinstance(entry, ast.Call)
                and isinstance(entry.func, ast.Name)
                and entry.func.id == "Image"
                and entry.args
                and isinstance(entry.args[0], ast.Constant)
                and isinstance(entry.args[0].value, str)
            ):
                names.add(entry.args[0].value)
        if names:
            return names
        break
    raise PublisherError(f"{path.relative_to(root)} has no static IMAGES contract")


def compose_files(root: Path) -> list[Path]:
    files = sorted(root.glob("compose*.yaml"))
    for extra in (
        "deploy/container/aimee-managed.compose.yaml",
        "deploy/compose/aimee.yaml",
        "deploy/compose/worm-worker.yaml",
    ):
        path = root / extra
        if path.exists():
            files.append(path)
    return files


def resolve(text: str) -> str:
    """Apply Compose ${VAR:-default} defaults so image names are complete."""
    previous = None
    while previous != text:
        previous = text
        text = INTERPOLATION.sub(lambda match: match.group(2), text)
    return text


def validate(root: Path = ROOT) -> tuple[int, int]:
    errors: list[str] = []
    images: set[str] = set()
    llm_images: set[str] = set()
    for path in compose_files(root):
        raw = path.read_text(encoding="utf-8")
        relative = path.relative_to(root)
        for line in IMAGE_LINE.findall(raw):
            if "ghcr.io/rakuensoftware/" not in line:
                continue
            for name in COMPOSE_IMAGE.findall(resolve(line)):
                (llm_images if name.startswith("aimee-llm-") else images).add(name)
            if "AIMEE_IMAGE_TAG" not in line:
                errors.append(
                    f"{relative} pins {line.strip()} without "
                    "${AIMEE_IMAGE_TAG:-latest}; one channel must move every image "
                    "in the topology together"
                )

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
        if workflow.name == "publish-testing.yml":
            planner_ref = "scripts/publish_testing_plan.py"
            if planner_ref not in text:
                errors.append(
                    f"{workflow.relative_to(root)} does not invoke {planner_ref}"
                )
            published = testing_planner_images(root)
        else:
            published = {
                image
                for image in images
                if re.search(MATRIX_ENTRY % re.escape(image), text)
            }
        for image in sorted(images):
            if image not in published:
                errors.append(f"{workflow.relative_to(root)} does not publish {image}")

    if llm_images:
        text = (root / LLM_WORKFLOW.relative_to(ROOT)).read_text(encoding="utf-8")
        for image in sorted(llm_images):
            if not re.search(MATRIX_ENTRY % re.escape(image), text):
                errors.append(f"{LLM_WORKFLOW.relative_to(ROOT)} does not publish {image}")
        if "workflow_call" not in text:
            errors.append(
                f"{LLM_WORKFLOW.relative_to(ROOT)} is not callable from a release; "
                "synthesis sidecars would never receive release tags"
            )
        release = (root / ".github/workflows/auto-release.yml").read_text(encoding="utf-8")
        if not re.search(
            r"^\s*uses:\s*\./\.github/workflows/publish-llm\.yml\s*$",
            release,
            re.MULTILINE,
        ):
            errors.append("auto-release.yml never calls publish-llm.yml")

    if errors:
        raise PublisherError("\n".join(errors))
    return len(images) + len(llm_images), len(workflows) + 1


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
