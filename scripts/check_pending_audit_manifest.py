#!/usr/bin/env python3
"""Validate the 2026-07-26 pending-proposal reconciliation manifest."""

from __future__ import annotations

import csv
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "docs/proposals/PENDING_AUDIT_2026-07-26.tsv"
ALLOWED = {"complete", "partial_archived", "pending_accurate", "pending_regressed"}


def fail(message: str) -> None:
    raise ValueError(message)


def main() -> int:
    with MANIFEST.open(encoding="utf-8", newline="") as handle:
        rows = list(csv.DictReader(handle, delimiter="\t"))
    required = {
        "original", "disposition", "final_path", "residual_path", "stale_updated",
        "evidence_anchor", "roundtable",
    }
    if not rows or set(rows[0]) != required:
        fail("manifest columns do not match the audit contract")
    if len(rows) != 79:
        fail(f"expected 79 originals, found {len(rows)}")
    originals = [row["original"] for row in rows]
    if len(set(originals)) != len(originals):
        fail("duplicate original proposal")

    expected_pending: set[str] = set()
    for row in rows:
        name = row["original"]
        disposition = row["disposition"]
        if disposition not in ALLOWED:
            fail(f"{name}: invalid disposition {disposition!r}")
        final = ROOT / row["final_path"]
        if not final.is_file():
            fail(f"{name}: final path does not exist: {row['final_path']}")
        text = final.read_text(encoding="utf-8")
        if disposition in {"complete", "partial_archived"}:
            if final.parent.name != "done" or "**State:** DONE" not in text or "Archived" not in text:
                fail(f"{name}: archived proposal lacks done state/archive notice")
        else:
            if final.parent.name != "pending":
                fail(f"{name}: live proposal is not in pending")
            if disposition == "pending_regressed" and "**State:** PENDING" not in text:
                fail(f"{name}: regressed proposal lacks explicit pending state")
            expected_pending.add(final.name)

        residual_value = row["residual_path"]
        if disposition == "partial_archived":
            residual = ROOT / residual_value
            if not residual.is_file() or residual.parent.name != "pending":
                fail(f"{name}: residual does not exist in pending")
            residual_text = residual.read_text(encoding="utf-8")
            if "**State:** PENDING" not in residual_text or name not in residual_text:
                fail(f"{name}: residual lacks pending state or archived-parent link")
            if residual.name not in text:
                fail(f"{name}: archive lacks reciprocal residual link")
            expected_pending.add(residual.name)
        elif residual_value != "-":
            fail(f"{name}: non-partial disposition has a residual")

        evidence = ROOT / "docs/proposals" / row["evidence_anchor"]
        if not evidence.is_file():
            fail(f"{name}: evidence anchor is missing")
        if not row["roundtable"].startswith("roundtable-"):
            fail(f"{name}: roundtable decision is missing")

    actual_pending = {path.name for path in (ROOT / "docs/proposals/pending").glob("*.md")}
    if actual_pending != expected_pending:
        fail(
            f"pending set mismatch: missing={sorted(expected_pending-actual_pending)}, "
            f"extra={sorted(actual_pending-expected_pending)}"
        )
    print(
        "pending-audit-manifest: ok "
        f"({len(rows)} originals, {len(actual_pending)} final pending proposals)"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as exc:
        print(f"pending-audit-manifest: ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
