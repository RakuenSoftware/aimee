"""Evaluator-only input accounting. Never pass these labels to a serving planner."""
from __future__ import annotations

import hashlib
from pathlib import Path


class DatasetCases(list):
    def __init__(self, path: str):
        super().__init__()
        self.inventory = {"version": 1, "dataset_sha256": hashlib.sha256(Path(path).read_bytes()).hexdigest(),
                          "cases": []}

    def record(self, case_id: str, answerable: bool | None, reason: str = "") -> None:
        if any(row["id"] == case_id for row in self.inventory["cases"]):
            raise ValueError("duplicate dataset question ID: " + case_id)
        self.inventory["cases"].append({"id": case_id, "answerable": answerable,
                                        "disposition": "excluded" if reason else "included",
                                        "reason": reason})


def validate_inventory_results(inventory: dict, results: list[dict]) -> None:
    if inventory.get("version") != 1 or len(inventory.get("dataset_sha256", "")) != 64:
        raise ValueError("invalid dataset inventory")
    expected = []
    seen = set()
    for row in inventory["cases"]:
        if not row["id"] or row["id"] in seen:
            raise ValueError("invalid inventory ID")
        seen.add(row["id"])
        if row["disposition"] == "included" and not row["reason"]:
            expected.append(row["id"])
        elif row["disposition"] != "excluded" or not row["reason"]:
            raise ValueError("invalid exclusion disposition")
    if [row["question_id"] for row in results] != expected:
        raise ValueError("missing, reordered or duplicate dataset results")
    labels = {row["id"]: row["answerable"] for row in inventory["cases"]}
    for row in results:
        if row.get("answerable") is not labels[row["question_id"]]:
            raise ValueError("answerability label changed")
        if row.get("run_status", "ok") not in {"ok", "timeout", "invalid", "infrastructure_error"}:
            raise ValueError("invalid result status")
