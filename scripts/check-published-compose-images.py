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
MODEL_WORKFLOWS = ("publish-llm.yml", "publish-embedder.yml")
TESTING_PLANNER = ROOT / "scripts/publish_testing_plan.py"



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
    files.extend(sorted((root / "deploy/smoothnas").glob("*.compose.yaml")))
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
    model_images = {name: set() for name in MODEL_WORKFLOWS}
    for path in compose_files(root):
        raw = path.read_text(encoding="utf-8")
        relative = path.relative_to(root)
        for line in IMAGE_LINE.findall(raw):
            if "ghcr.io/rakuensoftware/" not in line:
                continue
            for name in COMPOSE_IMAGE.findall(resolve(line)):
                if name.startswith("aimee-llm-"):
                    model_images["publish-llm.yml"].add(name)
                elif name.startswith("aimee-embedder-"):
                    model_images["publish-embedder.yml"].add(name)
                else:
                    images.add(name)
            if "AIMEE_IMAGE_TAG" not in line:
                errors.append(
                    f"{relative} pins {line.strip()} without "
                    "${AIMEE_IMAGE_TAG:-latest}; one channel must move every image "
                    "in the topology together"
                )

    base = (root / "compose.yaml").read_text(encoding="utf-8")
    if re.search(r"^  aimee-kb:", base, re.MULTILINE):
        errors.append("compose.yaml must not install a KB")
    if "ghcr.io/rakuensoftware/aimee:" not in base or "Dockerfile.postgres" not in base:
        errors.append("compose.yaml must use the unified application and standardized PostgreSQL images")
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
        if workflow.name == "publish-images.yml":
            match = re.search(r"^  merge:\n(.*?)(?=^  [a-z_-]+:|\Z)", text, re.MULTILINE | re.DOTALL)
            names = re.search(r"image:\s*\[([^\]]+)\]", match.group(1)) if match else None
            merged = {name.strip() for name in names.group(1).split(',')} if names else set()
            for image in sorted(images):
                if image not in merged:
                    errors.append(f"{workflow.relative_to(root)} does not merge published image {image}")

    release = (root / ".github/workflows/auto-release.yml").read_text(encoding="utf-8")
    for workflow, models in model_images.items():
        path = root / ".github/workflows" / workflow
        text = path.read_text(encoding="utf-8")
        for image in sorted(models):
            if not re.search(MATRIX_ENTRY % re.escape(image), text):
                errors.append(f"{workflow} does not publish {image}")
        if "workflow_call" not in text or "--tag \"${image}:latest\"" not in text:
            errors.append(f"{workflow} does not provide release model tags")
        if f"uses: ./.github/workflows/{workflow}" not in release:
            errors.append(f"auto-release.yml never calls {workflow}")

    if errors:
        raise PublisherError("\n".join(errors))
    return len(images) + sum(map(len, model_images.values())), len(workflows) + len(MODEL_WORKFLOWS)


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
