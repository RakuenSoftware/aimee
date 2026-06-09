#!/usr/bin/env python3
"""Gate: every proposal link in the index (and between proposals) resolves.

Background: proposals live under docs/proposals/{pending,accepted,done,rejected}/.
The project convention is that only `pending/` keeps a live file; once a proposal
is accepted, shipped (done), or rejected, its file is delisted and the entry in
docs/PROPOSALS.md becomes plain summary text. The failure mode this guards is a
markdown link `[Title](proposals/.../x.md)` left pointing at a file that no
longer exists (or never did) — a dead link that makes the index dishonest and
hides genuinely-missing proposals among the merely-delisted ones. (This is the
exact rot called out in the agent-roundtable proposal, §0.4.)

The rule is simple and total: every markdown link whose target matches
`proposals/.../*.md` — in PROPOSALS.md and inside any pending proposal — must
resolve to a file that exists. A delisted proposal must be plain text, not a
dead link. Run as part of `make lint`.

  --plant-test  inject a known-bad link in-memory and confirm the check fails,
                proving the gate is wired and not vacuously passing.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DOCS = ROOT / "docs"
INDEX = DOCS / "PROPOSALS.md"
PENDING = DOCS / "proposals" / "pending"

# Markdown link whose target points into the proposals tree (relative forms too).
LINK_RE = re.compile(r"\[[^\]]+\]\(([^)]*proposals/[^)]+\.md|[A-Za-z0-9._/-]+\.md)\)")


def _iter_links(text):
    for m in re.finditer(r"\[[^\]]+\]\(([^)]+)\)", text):
        yield m.group(1)


def _resolve(target, base_dir):
    """Resolve a link target relative to the file's directory; return Path."""
    t = target.split("#", 1)[0].strip()
    if not t:
        return None
    return (base_dir / t).resolve()


def _is_proposal_target(target):
    t = target.split("#", 1)[0].strip()
    if not t.endswith(".md"):
        return False
    # Index links use `proposals/...`; intra-proposal links use a bare sibling
    # filename (e.g. `ingest-restoration-and-recall-contract.md`).
    return "proposals/" in t or "/" not in t


def check_file(path, extra_text=None):
    """Yield (target,) for each dead proposal link in `path`."""
    text = extra_text if extra_text is not None else path.read_text(encoding="utf-8")
    base = path.parent
    broken = []
    for target in _iter_links(text):
        if not _is_proposal_target(target):
            continue
        dest = _resolve(target, base)
        if dest is None or not dest.exists():
            broken.append(target)
    return broken


def main():
    plant = "--plant-test" in sys.argv

    files = [INDEX]
    if PENDING.is_dir():
        files += sorted(PENDING.glob("*.md"))

    failures = []
    checked_links = 0
    for f in files:
        if not f.exists():
            continue
        text = f.read_text(encoding="utf-8")
        checked_links += sum(1 for t in _iter_links(text) if _is_proposal_target(t))
        for target in check_file(f, text):
            failures.append((f.relative_to(ROOT), target))

    if plant:
        # Inject a guaranteed-dead link into the index text and confirm we catch it.
        bad = "[planted](proposals/done/__definitely_not_a_real_proposal__.md)"
        planted = check_file(INDEX, INDEX.read_text(encoding="utf-8") + "\n" + bad)
        if "proposals/done/__definitely_not_a_real_proposal__.md" not in planted:
            print("check-proposal-links: FAIL — plant-test did not catch a dead link")
            return 1
        print("check-proposal-links: plant-test ok (dead link detected)")
        return 0

    if failures:
        print(f"check-proposal-links: FAIL — {len(failures)} dead proposal link(s):")
        for src, target in failures:
            print(f"  {src}: {target}")
        print("  Fix: delist (make it plain text) or repoint to the real file. "
              "Only pending/ proposals keep live files.")
        return 1

    print(f"check-proposal-links: ok ({checked_links} proposal links resolve "
          f"across {len(files)} file(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main())
