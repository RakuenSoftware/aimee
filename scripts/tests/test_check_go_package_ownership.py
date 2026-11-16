#!/usr/bin/env python3
"""Mutation checks: an exact public module contract is not an unowned-prefix bypass."""
import contextlib
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("ownership", Path(__file__).resolve().parent.parent / "check_go_package_ownership.py")
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)

class Ownership(unittest.TestCase):
    def fixture(self, root):
        descriptor = root / "src/modules/ir/module.yaml"
        descriptor.parent.mkdir(parents=True)
        descriptor.write_text('{"id":"ir","go_sources":["server-go/modules/ir/runtime.go"]}')
        package = root / "server-go/ir"
        package.mkdir(parents=True)
        (package / "response.go").write_text("package ir\n")

    def test_existing_owner_accounts_for_contract(self):
        with tempfile.TemporaryDirectory() as d, patch.object(checker, "UNOWNED_PACKAGES", {}):
            root=Path(d); self.fixture(root)
            self.assertIn("1 module-owned public contract",checker.run(root))

    def test_unknown_owner_cannot_hide_source(self):
        with tempfile.TemporaryDirectory() as d, patch.object(checker,"UNOWNED_PACKAGES",{}), patch.object(checker,"PUBLIC_MODULE_CONTRACT_PACKAGES",{"server-go/ir":"missing"}):
            root=Path(d);self.fixture(root)
            with self.assertRaises(SystemExit): checker.run(root)

    def test_child_package_does_not_inherit_contract_ownership(self):
        with tempfile.TemporaryDirectory() as d, patch.object(checker,"UNOWNED_PACKAGES",{}), contextlib.redirect_stderr(io.StringIO()):
            root=Path(d);self.fixture(root)
            child=root/"server-go/ir/hidden";child.mkdir();(child/"authority.go").write_text("package hidden\n")
            with self.assertRaises(SystemExit): checker.run(root)

if __name__ == "__main__": unittest.main()
