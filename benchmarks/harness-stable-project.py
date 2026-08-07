#!/usr/bin/env python3
"""One index and one embed for the whole corpus, not one per cell.

Applies to ponytail-codex-benchmark/battery/codex_matrix_runner.py.

THE BUG. aimee_project() returned

    codex-<task>-r<replicate>-<attempt_id>        attempt_id = uuid4()[:8]

so every cell -- and every retry of a cell -- asked aimee to index and embed
under a project name nothing had ever used before. Vector dedup is keyed
(project, node_key, content_hash), so a fresh name means nothing is reused and
the entire corpus is re-embedded from zero. The 14 tasks are the SAME repository
at different commits, so this repeats near-identical work fourteen times over,
and each pass is hours: measured, one cold corpus embed produced ~39,000 doc
vectors and took the whole night.

The per-attempt suffix was there so a failed or canary attempt could not satisfy
a resumed cell's semantic read-after-write proof. That is a real concern, but it
is paid for by discarding every embedding the corpus has ever produced.

THE FIX. One stable project for the corpus. A new task, or a re-run, then hits
the existing per-project hash-skip path and embeds only what actually differs
between commits -- which is what the incremental scan is for, and is how aimee
is used against a real repository: one project per repo, re-indexed as it
changes.

The readiness proof stays honest without the suffix because it does not depend
on it: the gate asserts the probe symbol resolves, the expected callers resolve,
blast-radius answers, and a planted README passage is semantically retrievable
AGAINST THE CHECKOUT UNDER TEST. A stale attempt cannot satisfy those for a
checkout it never saw. attempt_id is still minted and still recorded in the
artifact, so a cell remains traceable to its attempt.
"""
import re
import sys
from pathlib import Path

RUNNER = Path("/opt/bench/ponytail-codex-benchmark/battery/codex_matrix_runner.py")

NEW_FN = '''def aimee_project(task: str, replicate: int, attempt_id: str) -> str:
    """The corpus's single project.

    Deliberately ignores task, replicate and attempt_id. Vector dedup is keyed
    (project, node_key, content_hash), so a name that varies per cell discards
    every embedding the corpus has: the 14 tasks are one repository at different
    commits, and a per-cell name re-embeds all of it fourteen times. One stable
    name lets the incremental scan do its job and embed only the delta.

    Override with PT_PROJECT if a run genuinely needs an isolated index.
    """
    return os.environ.get("PT_PROJECT", "am-corpus")
'''


def main():
    text = RUNNER.read_text()
    if 'PT_PROJECT' in text:
        print("already patched")
        return 0
    RUNNER.with_suffix(".py.pre-stableproject.bak").write_text(text)

    pattern = re.compile(
        r"def aimee_project\(task: str, replicate: int, attempt_id: str\) -> str:\n"
        r"(?:    .*\n|\n)*?"
        r"    return f\"codex-\{task\.replace\('_', '-'\)\}-r\{replicate\}-\{attempt_id\}\"\n")
    if not pattern.search(text):
        print("aimee_project not in the expected shape; not patching", file=sys.stderr)
        return 1
    text = pattern.sub(NEW_FN, text, count=1)
    RUNNER.write_text(text)
    print("patched:", RUNNER)
    return 0


if __name__ == "__main__":
    sys.exit(main())
