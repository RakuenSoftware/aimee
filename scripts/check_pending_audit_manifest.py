#!/usr/bin/env python3
"""Validate the latest pending-proposal reconciliation manifest."""

from __future__ import annotations

import csv
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PROPOSALS = ROOT / "docs/proposals"
MANIFEST_RE = re.compile(r"^PENDING_AUDIT_\d{4}-\d{2}-\d{2}\.tsv$")
ALLOWED = {"complete", "partial_archived", "pending_accurate", "pending_regressed"}


def fail(message: str) -> None:
    raise ValueError(message)


def latest_manifest() -> Path:
    manifests = sorted(
        path for path in PROPOSALS.glob("PENDING_AUDIT_*.tsv")
        if MANIFEST_RE.fullmatch(path.name)
    )
    if not manifests:
        fail("no dated pending-audit manifest found")
    return manifests[-1]


def main() -> int:
    manifest = latest_manifest()
    with manifest.open(encoding="utf-8", newline="") as handle:
        rows = list(csv.DictReader(handle, delimiter="\t"))
    required_base = {
        "original", "disposition", "final_path", "residual_path", "stale_updated",
        "evidence_anchor",
    }
    if not rows:
        fail("manifest is empty")
    columns = set(rows[0])
    review_columns = columns & {"review_record", "roundtable"}
    if columns - {"review_record", "roundtable"} != required_base or len(review_columns) != 1:
        fail("manifest columns do not match the audit contract")
    review_column = next(iter(review_columns))
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
            if (
                final.parent.name != "done"
                or "**state:** done" not in text.lower()
                or "archiv" not in text.lower()
            ):
                fail(f"{name}: archived proposal lacks done state/archive notice")
        else:
            if final.parent.name != "pending":
                fail(f"{name}: live proposal is not in pending")
            if disposition == "pending_regressed" and "**state:** pending" not in text.lower():
                fail(f"{name}: regressed proposal lacks explicit pending state")
            expected_pending.add(final.name)

        residual_value = row["residual_path"]
        if disposition == "partial_archived":
            residual = ROOT / residual_value
            if not residual.is_file() or residual.parent.name != "pending":
                fail(f"{name}: residual does not exist in pending")
            residual_text = residual.read_text(encoding="utf-8")
            if "**state:** pending" not in residual_text.lower() or name not in residual_text:
                fail(f"{name}: residual lacks pending state or archived-parent link")
            if residual.name not in text:
                fail(f"{name}: archive lacks reciprocal residual link")
            expected_pending.add(residual.name)
        elif residual_value != "-":
            fail(f"{name}: non-partial disposition has a residual")

        evidence = PROPOSALS / row["evidence_anchor"]
        if not evidence.is_file():
            fail(f"{name}: evidence anchor is missing")
        if not row[review_column].strip():
            fail(f"{name}: review record is missing")

    actual_pending = {path.name for path in (ROOT / "docs/proposals/pending").glob("*.md")}
    # A manifest is a DATED snapshot, so a proposal written after it is expected to
    # be absent from it. Requiring set equality made every newly drafted proposal a
    # failure -- which is why this gate was never wired into anything and drifted
    # unnoticed. Only one direction is a real defect: the manifest says a proposal
    # is live in pending/ and it is not there, so a row describes a file that moved
    # without its row being reconciled.
    missing = sorted(expected_pending - actual_pending)
    if missing:
        fail(f"manifest lists these as pending but they are absent from pending/: {missing}")
    unlisted = sorted(actual_pending - expected_pending)
    print(
        "pending-audit-manifest: ok "
        f"({manifest.name}: {len(rows)} originals, "
        f"{len(actual_pending)} pending proposals, "
        f"{len(unlisted)} drafted since the snapshot)"
    )
    for name in unlisted:
        print(f"  not in the {manifest.name} snapshot: {name}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as exc:
        print(f"pending-audit-manifest: ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
