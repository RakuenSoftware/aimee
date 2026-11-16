#!/usr/bin/env python3
"""Change notifications in the process contract are carved, capture-only and
name real subscribers."""

from __future__ import annotations

import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import validate_module_process_contracts as contracts  # noqa: E402


class NotificationContractTests(unittest.TestCase):
    def setUp(self) -> None:
        self.document = json.loads(contracts.CONTRACTS.read_text(encoding="utf-8"))

    def component(self, document: dict[str, object], identifier: str) -> dict[str, object]:
        return next(item for item in document["components"] if item["id"] == identifier)

    def validate(self, document: dict[str, object]) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "process-contracts.json"
            path.write_text(json.dumps(document), encoding="utf-8")
            with mock.patch.object(contracts, "CONTRACTS", path):
                contracts.validate()

    def mutated(self, change) -> dict[str, object]:
        document = copy.deepcopy(self.document)
        change(self.component(document, "config")["notifications"][0], document)
        return document

    def test_shipping_contract_declares_the_change_notifications(self) -> None:
        self.validate(self.document)
        subscribed = contracts.subscriptions(self.document)
        self.assertEqual(subscribed["wfe"], [4610, 11800])
        self.assertEqual(subscribed["core"], [4610, 11801, 11802])
        self.assertEqual(subscribed["memory-postgres"], [4610])
        self.assertEqual(contracts.notification_kinds(self.component(self.document, "config")),
                         [4610])

    def test_a_notification_must_be_capture_only(self) -> None:
        document = self.mutated(lambda n, _: n.update(durability="ledger"))
        with self.assertRaisesRegex(contracts.ContractError, "capture-only"):
            self.validate(document)

    def test_a_notification_kind_is_carved_from_its_id(self) -> None:
        document = self.mutated(lambda n, _: n.update(event_kind=4611))
        with self.assertRaisesRegex(contracts.ContractError, "event_kind must equal 4610"):
            self.validate(document)

    def test_a_notification_cannot_reuse_a_stage_id(self) -> None:
        document = self.mutated(lambda n, _: n.update(id=1, event_kind=4609))
        with self.assertRaisesRegex(contracts.ContractError, "must follow active stages"):
            self.validate(document)

    def test_an_unknown_subscriber_is_refused(self) -> None:
        document = self.mutated(lambda n, _: n["subscribers"].append("no-such-client"))
        with self.assertRaisesRegex(contracts.ContractError, "no-such-client"):
            self.validate(document)

    def test_a_publisher_does_not_subscribe_to_itself(self) -> None:
        document = self.mutated(lambda n, _: n["subscribers"].append("config"))
        with self.assertRaisesRegex(contracts.ContractError, "not the publisher"):
            self.validate(document)

    def test_a_notification_needs_a_subscriber(self) -> None:
        document = self.mutated(lambda n, _: n.update(subscribers=[]))
        with self.assertRaisesRegex(contracts.ContractError, "subscribers"):
            self.validate(document)


if __name__ == "__main__":
    unittest.main()
