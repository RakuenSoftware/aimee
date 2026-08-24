#!/usr/bin/env python3
"""The validation record's check counts against the scripts that produce them.

A validation record is the one artefact in a tree with no build to fail it. Its
numbers are hand-copied from a run whose environment is then destroyed, so a
count that drifted when it was written stays drifted, and it fails in the
UNDER-claiming direction -- a dropped row looks like a smaller passing run
rather than like a missing check, which is the direction nobody investigates.

The session building the aimee module found exactly that in its own record: a
probe made fifteen checks, the record claimed fourteen and listed fourteen rows,
transcribed wrong in the commit that created both.

This compares the counts asserted in docs/validation/*.md against the number of
check-helper calls in the script each heading names. It cannot prove the run
happened or that the results were as claimed; it proves the record and the
script agree about how many checks there are, which is the half that can be
checked without the container.
"""

import glob
import os
import re
import sys
from pathlib import Path

# Resolved from this file, not the working directory: the Makefile target that
# runs this lives in src/, where the glob found nothing and the check passed
# having compared zero records -- which is the exact failure it exists to catch,
# in itself.
REPO_ROOT = Path(__file__).resolve().parent.parent
RECORDS = str(REPO_ROOT / "docs" / "validation" / "*.md")

# A heading naming a script and asserting a count, e.g.
#   ### `scripts/validation/db1-module-e2e.sh` -- 23 checks, all passing
#
# Any script path under scripts/, not only scripts/validation/. The narrower
# form silently stopped matching when the live probe moved to scripts/, so the
# check went on reporting success while comparing nothing -- the failure its own
# docstring describes for an empty glob, one level in.
HEADING = re.compile(
    r"^#+\s*`(?P<script>scripts/[\w./-]+\.sh)`\s*--\s*(?P<count>\d+)\s+checks",
    re.M,
)


def check_calls(path: str) -> tuple[int, list[str]]:
    """How many times the script calls one of its own ck* helpers."""
    text = open(path).read()
    lines = text.splitlines()
    helpers = {h for h in re.findall(r"(?m)^([a-z_]+)\(\)\s*\{", text) if h.startswith("ck")}
    if not helpers:
        return -1, []
    calls = 0
    for line in lines:
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        first = stripped.split()[0]
        if first in helpers and not re.match(rf"^{first}\(\)", stripped):
            calls += 1
    return calls, sorted(helpers)


# A record that declares itself history stops being compared against today's
# scripts, because the comparison is meaningless: its subject has been replaced.
#
# The marker is the assertion. A record either claims to describe the current
# program -- in which case its counts must match the scripts -- or it says
# plainly that it does not, and then it cannot be read as evidence for anything
# current. What it may not do is sit between the two, which is the state that
# lets a superseded run go on being cited.
HISTORICAL = re.compile(r"^>\s*\*\*HISTORICAL\b", re.M)


def main() -> int:
    problems = 0
    checked = 0
    historical = 0
    records = sorted(glob.glob(RECORDS))
    if not records:
        print(f"check-validation-record: no records found at {RECORDS}; "
              f"the layout moved and this check would pass having compared nothing")
        return 2
    for record in records:
        text = open(record).read()
        if HISTORICAL.search(text):
            historical += 1
            continue
        for m in HEADING.finditer(text):
            script, claimed = m.group("script"), int(m.group("count"))
            if not (REPO_ROOT / script).exists():
                print(f"{record}: names {script}, which does not exist")
                problems += 1
                continue
            actual, helpers = check_calls(str(REPO_ROOT / script))
            if actual < 0:
                print(f"{record}: {script} defines no ck* helper; the count cannot be checked")
                problems += 1
                continue
            # ZERO ON BOTH SIDES IS NOT AGREEMENT. A probe that stopped calling
            # its helper and a record whose table was emptied agree perfectly,
            # and the equality is between two absences. Worse than passing: it
            # increments `checked`, so an inert pair makes the summary line
            # report MORE comparisons than the tree actually supports.
            if claimed == 0 or actual == 0:
                print(f"{record}: {os.path.basename(script)} -- claimed {claimed}, "
                      f"script makes {actual}. A count of zero is not a comparison; "
                      f"a probe that checks nothing does not need a record.")
                problems += 1
                continue
            checked += 1
            if actual != claimed:
                # THE OBVIOUS REPAIR IS THE WRONG ONE, and it is one character
                # away in a markdown file, after which this goes green. Editing
                # the number to match the script makes the record assert a count
                # for a run that never produced it -- a claim about a container
                # that no longer exists, which is the exact thing this check was
                # written to stop, arriving through the check's own failure.
                #
                # A loud failure whose obvious fix over-claims is more dangerous
                # than a loud failure and less dangerous than a silent one. It
                # earns this comment even though the failure itself is plain.
                print(f"{record}: claims {claimed} checks for {os.path.basename(script)}, "
                      f"the script makes {actual} (helpers: {', '.join(helpers)})\n"
                      f"\n    THE repair: re-run the probe and record what the new"
                      f"\n    run reported. The record is evidence of a run, so a"
                      f"\n    changed probe needs a new run, not a new number."
                      f"\n"
                      f"\n    Do NOT edit the count to match. It is one character and"
                      f"\n    it turns this green, and it makes the record assert a"
                      f"\n    total that no run produced."
                      f"\n"
                      f"\n    Marking the record historical is available and is a"
                      f"\n    different claim, not a cheaper repair: it says the record"
                      f"\n    no longer describes the current program and may not be"
                      f"\n    cited for anything current. True after a rewrite; not"
                      f"\n    true merely because a count moved.")
                problems += 1

    if not checked and not problems:
        # NOT a pass. The empty-glob guard above exists because a check that
        # compared nothing must not report success -- and this is the same
        # condition one level in: records were found, read, and none of them
        # asserted anything to compare. It arrives silently, because marking the
        # last counted record HISTORICAL is enough to reach it, and that is an
        # ordinary edit nobody would expect to disarm a gate.
        # TWO REPAIRS, NOT EQUIVALENT, and this message used to offer them as
        # though they were. Marking a record historical is one line in a file
        # already open; adding a counted heading means running the probe to
        # learn N. Nobody at speed picks the second.
        #
        # And the cheap one is how this check went inert in the first place --
        # see the docstring. Offering it here as the alternative recommends the
        # exact act whose consequence this branch exists to catch.
        print("check-validation-record: records exist but none asserts a check "
              f"count ({historical} marked historical), so this compared nothing."
              "\n"
              "\n  THE repair is to give the live record a counted heading:"
              "\n      ### `scripts/<probe>.sh` -- N checks"
              "\n  where N is what the probe actually calls. That is the whole"
              "\n  point of the record, and it is what puts this check back to"
              "\n  work."
              "\n"
              "\n  Marking it historical is NOT the same act and is not the easy"
              "\n  version of it. It asserts that the record no longer describes"
              "\n  the current program and may not be cited as evidence for"
              "\n  anything current -- and it leaves nothing here being compared,"
              "\n  which is the state that produced this message. Do it only when"
              "\n  it is true.", file=sys.stderr)
        return 1
    if problems:
        print(f"\n{problems} record/script disagreement(s). A hand-copied count that "
              f"drifts stays drifted: the environment is gone and the run cannot be "
              f"repeated to settle it.")
        return 1
    # THE DENOMINATOR IS PART OF THE RESULT. Without it this reads "ok" beside a
    # directory of 88 records and implies it vouched for them; it compared one.
    # The other 86 assert no count, which is not a lie and not a defect -- a
    # record is free to be prose -- but a summary that omits how few it could
    # reach grows more reassuring as the directory grows, which is the wrong
    # direction for a number to move.
    #
    # NOT a reason to make all 88 carry counts. That is the over-claiming repair
    # at scale: it would put a check total against 87 runs nobody re-ran.
    silent = len(records) - checked - historical
    print(f"check-validation-record: ok ({checked} of {len(records)} record(s) "
          f"assert a check count and match their script; {silent} assert none, "
          f"{historical} marked historical)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
