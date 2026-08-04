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
        self.assertEqual(workflow_count, 2)

    def test_compose_image_missing_from_either_publisher_is_rejected(self) -> None:
        for workflow in ("publish-testing.yml", "publish-images.yml"):
            with self.subTest(workflow=workflow):
                def remove_control_web(root: Path, name: str = workflow) -> None:
                    path = root / ".github/workflows" / name
                    path.write_text(path.read_text().replace(
                        "{ name: aimee-control-web, dockerfile: Dockerfile.control-web }",
                        "{ name: omitted-control-web, dockerfile: Dockerfile.control-web }",
                        1,
                    ))

                self.assert_rejected(remove_control_web, "does not publish aimee-control-web")

    def test_weightless_image_is_rejected_for_bekko_topologies(self) -> None:
        def select_weightless(root: Path) -> None:
            path = root / "compose.server.yaml"
            path.write_text(path.read_text().replace("aimee-kb-a25m:", "aimee-kb:"))

        self.assert_rejected(select_weightless, "does not default to the bundled")


if __name__ == "__main__":
    unittest.main()
