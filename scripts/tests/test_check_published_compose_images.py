#!/usr/bin/env python3
"""Mutation tests for the Compose image publishing contract."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import shutil
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[2]
CHECKER_PATH = REPO / "scripts/check-published-compose-images.py"
SPEC = importlib.util.spec_from_file_location("published_compose_images", CHECKER_PATH)
assert SPEC and SPEC.loader
checker = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(checker)


class PublishedComposeImagesTests(unittest.TestCase):
    def fixture(self, root: Path) -> None:
        for relative in (
            "compose.yaml",
            "compose.server.yaml",
            ".github/workflows/publish-testing.yml",
            ".github/workflows/publish-images.yml",
            "scripts/publish_testing_plan.py",
            ".github/workflows/publish-llm.yml",
            ".github/workflows/publish-embedder.yml",
            ".github/workflows/auto-release.yml",
        ):
            source = REPO / relative
            target = root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)

    def assert_rejected(self, mutate, message: str) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.fixture(root)
            mutate(root)
            with self.assertRaisesRegex(checker.PublisherError, message):
                checker.validate(root)

    def test_repository_passes(self) -> None:
        image_count, workflow_count = checker.validate(REPO)
        self.assertGreaterEqual(image_count, 4)
        self.assertEqual(workflow_count, 4)

    def test_compose_image_missing_from_either_publisher_is_rejected(self) -> None:
        for workflow in ("publish-testing.yml", "publish-images.yml"):
            with self.subTest(workflow=workflow):
                def remove_postgres(root: Path, name: str = workflow) -> None:
                    if name == "publish-testing.yml":
                        path = root / "scripts/publish_testing_plan.py"
                        old = 'Image("aimee-postgres", "Dockerfile.postgres")'
                        new = 'Image("omitted-postgres", "Dockerfile.postgres")'
                    else:
                        path = root / ".github/workflows" / name
                        old = "{ name: aimee-postgres, dockerfile: Dockerfile.postgres }"
                        new = "{ name: omitted-postgres, dockerfile: Dockerfile.postgres }"
                    path.write_text(path.read_text().replace(old, new, 1))

                self.assert_rejected(remove_postgres, "does not publish aimee-postgres")

    def test_manifest_merge_cannot_omit_a_built_image(self) -> None:
        def remove_merge(root: Path) -> None:
            path = root / ".github/workflows/publish-images.yml"
            path.write_text(path.read_text().replace("image: [aimee, aimee-postgres,", "image: [aimee,"))
        self.assert_rejected(remove_merge, "does not merge published image aimee-postgres")

    def test_testing_workflow_must_invoke_the_planner(self) -> None:
        def disconnect_planner(root: Path) -> None:
            path = root / ".github/workflows/publish-testing.yml"
            path.write_text(
                path.read_text().replace(
                    "scripts/publish_testing_plan.py", "scripts/disconnected_plan.py"
                )
            )

        self.assert_rejected(disconnect_planner, "does not invoke")

    def test_base_must_not_install_kb(self) -> None:
        def add_kb(root: Path) -> None:
            path = root / "compose.yaml"
            path.write_text(path.read_text() + "\n  aimee-kb:\n    image: forbidden\n")
        self.assert_rejected(add_kb, "must not install a KB")

    def test_embedder_release_promotion_is_required(self) -> None:
        def remove_promotion(root: Path) -> None:
            path = root / ".github/workflows/auto-release.yml"
            path.write_text(path.read_text().replace("publish-embedder.yml", "disconnected.yml"))
        self.assert_rejected(remove_promotion, "never calls publish-embedder")


if __name__ == "__main__":
    unittest.main()
