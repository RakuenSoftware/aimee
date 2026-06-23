#!/usr/bin/env python3
"""Gate: reconcile proposal lifecycle state against the directory + the tree.

Companion lint gate for the autonomous-dev-execution-substrate proposal (§4/§5).
Sibling of check-proposal-links.py; wired into `make lint`. Three checks, all
decidable from the working tree alone (no build runner, no CI, no live stack):

  A. state<->folder consistency (the safe slice of "shipped-but-unfiled").
     The FOLDER is authoritative for a proposal's lifecycle. The "- **State:**"
     bullet is prose and drifts. We classify the bullet into a closed enum and
     compare it to the class the folder implies:
       FAIL  - a pending/ or accepted/ proposal classified terminal-done
               (claims completion while still filed as open work).
       WARN  - any other folder<->class mismatch (e.g. a done/ file whose bullet
               still reads draft/proposed/approved). Report-only, exit 0.

  B. acceptance-block schema (the executable shadow of a proposal's criteria).
     An optional fenced block:
         ```yaml acceptance
         - {id: 1, tier: mechanical, check: "make unit-tests TEST=test_foo"}
         ```
     Every entry must carry at least id/tier/check; id a positive int unique in
     the file; tier in {mechanical,integration,deployment,hardware}; check a
     non-empty string. Unknown keys are tolerated (forward-compat). A non-list,
     missing key, unknown tier, dup/non-positive id, or YAML parse error => FAIL.
     We validate the SHAPE only; running the checks is a later packet.

  C. premise-drift (report-only, never blocking by default).
     Acceptance `check:` targets that reference an in-tree artefact which no
     longer resolves (a `make ... TEST=<id>` whose test greps nowhere under
     src/tests, or a `python3 scripts/<f>` / `scripts/<f>` whose file is gone),
     plus backtick-quoted source paths a proposal names that do not exist and are
     not flagged as proposed-new. Reported in the `drift` array; --strict makes
     drift fatal too. Never auto-edits (renamed-but-equivalent => false positive).

  Triage: the drift report is consumed by the next plan revision / the autonomous
  driver, not a human inbox. --strict becomes the lint default once the drift
  count reaches zero and holds one packet (first review at P3). If Check C proves
  unworkably noisy it is removed, not endured.

Usage:
  check-proposal-reconcile.py [--proposals-dir DIR] [--strict] [--json] [--plant-test]

  --strict      Check-C drift findings also make the exit nonzero (default: only
                Check-A FAIL and Check-B FAIL are blocking).
  --json        Emit {blocking:[...], warnings:[...], drift:[...]} (all keys
                always present) instead of the human report.
  --plant-test  Inject one known-bad case per check class in-memory and confirm
                each is caught; proves the gate is wired and not vacuously
                passing. Exit 0 on success, 1 if any planted fault slips through.

Requires Python >= 3.8 and PyYAML (present in the lint CI job).
"""
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_PROPOSALS = ROOT / "docs" / "proposals"

# Lifecycle folders -> the state class the folder implies.
FOLDER_EXPECTED = {
    "pending": "in_flight",
    "accepted": "in_flight",
    "done": "terminal_done",
    "rejected": "terminal_closed",
    "deferred": "in_flight",
}

# State-bullet classification keywords, checked in this priority order (a "done"
# word wins over an "in_flight" word when both appear). Matched on WORD BOUNDARIES
# (lowercased) so e.g. "abandoned" does not count as "done".
TERMINAL_DONE_KW = ("done", "shipped", "merged", "deployed", "landed")
TERMINAL_CLOSED_KW = ("rejected", "withdrawn", "superseded", "declined")
IN_FLIGHT_KW = (
    "draft", "proposed", "pending", "accepted", "approved", "reviewed",
    "ready", "in progress", "in-progress", "blocked", "partial", "converged",
    "implementing", "design",
)


def _kw_re(words):
    return re.compile(r"\b(?:" + "|".join(re.escape(w) for w in words) + r")\b")


TERMINAL_DONE_RE = _kw_re(TERMINAL_DONE_KW)
TERMINAL_CLOSED_RE = _kw_re(TERMINAL_CLOSED_KW)
IN_FLIGHT_RE = _kw_re(IN_FLIGHT_KW)

VALID_TIERS = {"mechanical", "integration", "deployment", "hardware"}

# The first lifecycle bullet. The corpus uses both `- **State:** X` and bare
# `Status: X` / `**Status:** X`; accept all forms (and incidental whitespace
# around the colon), anchored to line start.
STATE_RE = re.compile(
    r"^(?:-\s*)?\*{0,2}(?:State|Status)\*{0,2}\s*:\s*\*{0,2}\s*(.*)$", re.IGNORECASE)
# A fenced block whose info string is `acceptance` or `<lang> acceptance`.
ACCEPTANCE_FENCE_RE = re.compile(r"^```[ \t]*(?:[A-Za-z0-9_-]+[ \t]+)?acceptance[ \t]*$")
FENCE_CLOSE_RE = re.compile(r"^```[ \t]*$")
FENCE_OPEN_RE = re.compile(r"^```")  # any fence delimiter (toggles in/out)
# Backtick-quoted in-tree source path (concrete file, not a directory).
SRC_PATH_RE = re.compile(
    r"`((?:src|scripts|\.github)/[\w./-]+\.(?:c|h|py|sh|inc|ya?ml)|Makefile|Dockerfile[\w.-]*)`"
)
# A `make ... TEST=<id>` target inside an acceptance check string.
TEST_ID_RE = re.compile(r"\bTEST=([A-Za-z0-9_./-]+)")
# A `python3 scripts/x` or bare `scripts/x` target inside an acceptance check.
SCRIPT_TARGET_RE = re.compile(r"(scripts/[\w./-]+\.(?:py|sh))")

try:
    import yaml
except ImportError:  # pragma: no cover - yaml is present in CI
    print("check-proposal-reconcile: FAIL - PyYAML required for the "
          "acceptance-block check (pip install pyyaml)", file=sys.stderr)
    sys.exit(2)

SKELETON = (
    "  acceptance block must be:\n"
    "    ```yaml acceptance\n"
    "    - {id: 1, tier: mechanical, check: \"make unit-tests TEST=test_foo\"}\n"
    "    ```\n"
    "  each entry: id (positive int, unique), tier in "
    "{mechanical,integration,deployment,hardware}, check (non-empty string).")


def is_primary_proposal(path):
    """A proposal's main file, not its .plan.md / .PR.md companions or .gitkeep."""
    n = path.name
    return n.endswith(".md") and not (n.endswith(".plan.md") or n.endswith(".PR.md"))


def classify_state(text):
    """Map a State-bullet's prose to a lifecycle class."""
    if text is None:
        return "unknown"
    low = text.lower()
    if TERMINAL_DONE_RE.search(low):
        return "terminal_done"
    if TERMINAL_CLOSED_RE.search(low):
        return "terminal_closed"
    if IN_FLIGHT_RE.search(low):
        return "in_flight"
    return "unknown"


def extract_state(text):
    """Return the first State-bullet's prose (or None)."""
    for line in text.splitlines():
        m = STATE_RE.match(line)
        if m:
            return m.group(1).strip()
    return None


def extract_acceptance_blocks(text):
    """Yield the raw YAML body of every fenced acceptance block in `text`."""
    lines = text.splitlines()
    i = 0
    while i < len(lines):
        if ACCEPTANCE_FENCE_RE.match(lines[i]):
            body = []
            i += 1
            while i < len(lines) and not FENCE_CLOSE_RE.match(lines[i]):
                body.append(lines[i])
                i += 1
            yield "\n".join(body)
        i += 1


def check_state_folder(folder, rel, text):
    """Return (blocking, warnings) for one proposal's state<->folder consistency."""
    expected = FOLDER_EXPECTED.get(folder)
    if expected is None:
        return [], []
    state_text = extract_state(text)
    cls = classify_state(state_text)
    if cls == expected:
        return [], []
    # The one dangerous direction: open work claiming completion.
    if folder in ("pending", "accepted") and cls == "terminal_done":
        return [f"{rel}: in {folder}/ but State bullet reads as DONE "
                f"({state_text!r}) - file it to done/ or correct the bullet"], []
    if cls == "unknown":
        return [], [f"{rel}: State bullet unrecognised or missing "
                    f"({state_text!r}); expected {expected}"]
    return [], [f"{rel}: in {folder}/ (expected {expected}) but State bullet "
                f"classifies {cls} ({state_text!r})"]


def validate_acceptance(rel, text):
    """Return a list of blocking schema errors for one proposal's acceptance blocks."""
    errors = []
    seen_ids = set()
    for raw in extract_acceptance_blocks(text):
        try:
            data = yaml.safe_load(raw) if raw.strip() else []
        except yaml.YAMLError as e:
            errors.append(f"{rel}: acceptance block is not valid YAML: {e}")
            continue
        if data is None:
            data = []
        if not isinstance(data, list):
            errors.append(f"{rel}: acceptance block must be a list, got "
                          f"{type(data).__name__}")
            continue
        for entry in data:
            if not isinstance(entry, dict):
                errors.append(f"{rel}: acceptance entry must be a mapping, got "
                              f"{entry!r}")
                continue
            for key in ("id", "tier", "check"):
                if key not in entry:
                    errors.append(f"{rel}: acceptance entry missing '{key}': {entry!r}")
            if errors and errors[-1].startswith(f"{rel}: acceptance entry missing"):
                continue
            eid, tier, check = entry.get("id"), entry.get("tier"), entry.get("check")
            if not isinstance(eid, int) or isinstance(eid, bool) or eid <= 0:
                errors.append(f"{rel}: acceptance id must be a positive int: {eid!r}")
            elif eid in seen_ids:
                errors.append(f"{rel}: duplicate acceptance id {eid}")
            else:
                seen_ids.add(eid)
            if tier not in VALID_TIERS:
                errors.append(f"{rel}: acceptance tier {tier!r} not in "
                              f"{sorted(VALID_TIERS)}")
            if not isinstance(check, str) or not check.strip():
                errors.append(f"{rel}: acceptance check must be a non-empty "
                              f"string: {check!r}")
    return errors


def _test_id_exists(test_id, tests_dir):
    """True if a TEST=<id> resolves to something under src/tests/."""
    if not tests_dir.is_dir():
        return True  # can't disprove; don't false-flag
    for p in tests_dir.rglob("*.c"):
        if test_id in p.read_text(encoding="utf-8", errors="ignore"):
            return True
    return (tests_dir / test_id).exists()


def check_drift(rel, text, root, tests_dir):
    """Return a list of report-only premise-drift findings for one proposal."""
    drift = []
    # High-signal: acceptance check targets that don't resolve.
    for raw in extract_acceptance_blocks(text):
        try:
            data = yaml.safe_load(raw) if raw.strip() else []
        except yaml.YAMLError:
            continue
        if not isinstance(data, list):
            continue
        for entry in data:
            if not isinstance(entry, dict):
                continue
            check = entry.get("check")
            if not isinstance(check, str):
                continue
            for m in TEST_ID_RE.finditer(check):
                if not _test_id_exists(m.group(1), tests_dir):
                    drift.append(f"{rel}: acceptance check TEST={m.group(1)} "
                                 f"resolves to no test under src/tests/")
            for m in SCRIPT_TARGET_RE.finditer(check):
                if not (root / m.group(1)).exists():
                    drift.append(f"{rel}: acceptance check references "
                                 f"{m.group(1)} which does not exist")
    # Lower-signal (advisory): backtick source paths that don't exist. Skip
    # fenced code examples and lines explicitly marking a proposed-new file
    # (`**new**`), which are design targets, not drift.
    in_fence = False
    for line in text.splitlines():
        if FENCE_OPEN_RE.match(line):
            in_fence = not in_fence
            continue
        if in_fence or "**new**" in line:
            continue
        for m in SRC_PATH_RE.finditer(line):
            path = m.group(1)
            if not _within(root, path):
                continue  # ignore any `..`-escaping reference (never read it)
            if not (root / path).exists():
                drift.append(f"{rel}: references `{path}` which does not exist "
                             f"in the tree (may be proposed-new)")
    return drift


def _within(root, rel_path):
    """True if root/rel_path resolves inside root (no `..` escape). Any path the
    filesystem can't resolve (embedded NUL, symlink loop, ...) is treated as
    'not within' so a garbled prose reference skips rather than aborting the scan."""
    try:
        (root / rel_path).resolve().relative_to(root.resolve())
        return True
    except (ValueError, OSError, RuntimeError):
        return False


def run(proposals_dir, root, strict):
    """Run all three checks over the tree. Return (blocking, warnings, drift)."""
    blocking, warnings, drift = [], [], []
    tests_dir = root / "src" / "tests"
    for folder in FOLDER_EXPECTED:
        d = proposals_dir / folder
        if not d.is_dir():
            continue
        for f in sorted(d.glob("*.md")):
            if not is_primary_proposal(f):
                continue
            try:
                rel = f.relative_to(root).as_posix()
            except ValueError:
                rel = f.as_posix()  # --proposals-dir outside the repo (tests)
            text = f.read_text(encoding="utf-8", errors="replace")
            b, w = check_state_folder(folder, rel, text)
            blocking += b
            warnings += w
            blocking += validate_acceptance(rel, text)
            if folder in ("pending", "accepted"):
                drift += check_drift(rel, text, root, tests_dir)
    return _dedup(blocking), _dedup(warnings), _dedup(drift)


def _dedup(items):
    """Drop duplicate findings, preserving first-seen order."""
    seen, out = set(), []
    for x in items:
        if x not in seen:
            seen.add(x)
            out.append(x)
    return out


def plant_test(proposals_dir, root):
    """Confirm each check class catches a known-bad input. Exit-status int."""
    ok = True
    # A: a pending file whose bullet reads DONE must be blocking.
    b, _ = check_state_folder("pending", "x/y.md",
                              "- **State:** done and shipped to testing\n")
    if not b:
        print("check-proposal-reconcile: FAIL - plant-test A (done-in-pending) "
              "not caught")
        ok = False
    # B: an unknown tier must be a schema error.
    errs = validate_acceptance("x/y.md",
                               "```yaml acceptance\n- {id: 1, tier: bogus, "
                               "check: \"x\"}\n```\n")
    if not errs:
        print("check-proposal-reconcile: FAIL - plant-test B (bad tier) not caught")
        ok = False
    # C: an acceptance check pointing at a missing script must be drift.
    d = check_drift("x/y.md",
                    "```yaml acceptance\n- {id: 1, tier: mechanical, "
                    "check: \"python3 scripts/__no_such__.py\"}\n```\n",
                    root, root / "src" / "tests")
    if not d:
        print("check-proposal-reconcile: FAIL - plant-test C (missing script) "
              "not caught")
        ok = False
    if ok:
        print("check-proposal-reconcile: plant-test ok (A/B/C all caught)")
        return 0
    return 1


def main(argv=None):
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("--proposals-dir", default=str(DEFAULT_PROPOSALS))
    ap.add_argument("--strict", action="store_true")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--plant-test", action="store_true")
    args = ap.parse_args(argv)

    proposals_dir = Path(args.proposals_dir).resolve()
    root = ROOT

    if args.plant_test:
        return plant_test(proposals_dir, root)

    if not proposals_dir.is_dir():
        print(f"check-proposal-reconcile: FAIL - proposals dir not found: "
              f"{proposals_dir}", file=sys.stderr)
        return 2

    blocking, warnings, drift = run(proposals_dir, root, args.strict)

    if args.json:
        import json
        print(json.dumps({"blocking": blocking, "warnings": warnings,
                          "drift": drift}, indent=2))
    else:
        if blocking:
            print(f"check-proposal-reconcile: FAIL - {len(blocking)} blocking "
                  f"finding(s):")
            for x in blocking:
                print(f"  {x}")
            print(SKELETON)
        if warnings:
            print(f"check-proposal-reconcile: {len(warnings)} warning(s) "
                  f"(report-only):")
            for x in warnings:
                print(f"  {x}")
        if drift:
            print(f"check-proposal-reconcile: {len(drift)} premise-drift "
                  f"finding(s) (report-only{', FATAL under --strict' if args.strict else ''}):")
            for x in drift:
                print(f"  {x}")
        if not (blocking or warnings or drift):
            print("check-proposal-reconcile: ok (state<->folder consistent, "
                  "acceptance blocks valid, no drift)")

    rc = 1 if blocking else 0
    if args.strict and drift:
        rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
