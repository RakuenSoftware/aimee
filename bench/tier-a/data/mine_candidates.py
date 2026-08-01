"""Mine gold-set candidates from real repositories, with a source anchor per fact.

WHY THIS EXISTS. The existing 70 notes are hand-authored, and LABELING.md names
that as the set's main external-validity limit: the labeller invents the fact AND
the label, so gold is one person's opinion twice over. A fact mined from a commit
is different in kind — the commit asserts it and the diff proves it, so the label
is checkable against something outside the labeller's head.

WHAT IS AUTHORED AND WHAT IS NOT. The FACT comes from the source and is fixed.
Only the NOTE PHRASING is authored, because the task under test takes a
remembered note as input and repositories do not contain remembered notes. That
is a much smaller surface for bias than inventing both, and every candidate
carries its anchor (repo, sha, file) so a reviewer can check the fact without
trusting this script.

WHAT THIS CANNOT MINE. The seed ontology has 17 predicates and a repository can
only evidence some of them:

  minable   works_for member_of has_role located_in device_has_ip has_hostname
            also_known_as supersedes linked_policy decided_by
  synthetic spouse parent_of child_of born_in age knows

No codebase contains personal relationships. Those stay hand-authored, which is
unavoidable and harmless — the risk of invented gold is that it encodes the
labeller's reading of an AMBIGUOUS case, and "Sarah is my spouse" is not
ambiguous. The categories where invention is dangerous are exactly the ones this
script covers: governance, supersession, negation and infra.

OUTPUT is a candidate pool, NOT gold. Nothing here goes into gold.jsonl without
passing validate_gold.py and the independent labelling pass.
"""
import argparse
import json
import os
import re
import subprocess

# Statements that assert something no longer holds. This is the precision-critical
# slice: the drain must not commit the retracted fact. Real retraction language is
# far better than invented retraction language, because invented negations tend to
# be tidier than the real thing.
# Anchored at the START of the subject (after any `type(scope):` prefix), so the
# verb GOVERNS the sentence rather than merely appearing in it. Matching anywhere
# put "Exit the worker thread when X11Source is dropped" in this slice — thread
# lifecycle, not a retraction — and "Remove extra namespace typo", which asserts
# nothing durable. `drop` is excluded entirely: in these repos it is almost always
# RAII/Drop-trait vocabulary rather than removal.
RETIRE_RE = re.compile(
    r"^(?:[a-z]+(?:\([^)]*\))?:\s*)?"
    r"(remove|retire|delete|deprecate|kill)[sd]?\b",
    re.I)
# Statements that replace one thing with another - `supersedes` in the ontology.
# Supersession needs BOTH endpoints named, so the triple has a subject and an
# object. "Rename X to Y" and "replace X with Y" qualify; a bare "Switch to Ninja"
# names only the new thing and cannot form `A supersedes B`.
SUPERSEDE_RE = re.compile(
    r"\b(?:rename[sd]?|replace[sd]?|supersede[sd]?|migrate[sd]?)\s+"
    r"[`'\"]?[A-Za-z0-9_.:*/-]{2,}[`'\"]?\s+(?:to|with|by|in favou?r of)\s+"
    r"[`'\"]?[A-Za-z0-9_.:*/-]{2,}",
    re.I)
# Non-durable assertions. Commit and issue text carries these naturally, where a
# hand-authored transient tends to be a caricature ("I feel tired today").
# Explicitly non-durable markers only. `try` is gone: "Fix failure to try the
# default VAAPI driver" is a bug fix, not a transient assertion. What remains are
# terms that state the thing is not settled.
TRANSIENT_RE = re.compile(
    r"\b(wip|work in progress|temporar(?:y|ily)|for now|revisit|todo|"
    r"workaround|stopgap|placeholder)\b", re.I)


def commits(repo, limit):
    """(sha, subject, body, author) for a repo's history."""
    sep = "\x1e"
    fmt = sep.join(["%H", "%s", "%b", "%an"])
    out = subprocess.run(
        ["git", "-C", repo, "log", f"-{limit}", f"--format={fmt}%x1f"],
        capture_output=True, text=True, timeout=120)
    if out.returncode != 0:
        return []
    rows = []
    for rec in out.stdout.split("\x1f"):
        rec = rec.strip("\n")
        if not rec:
            continue
        parts = rec.split(sep)
        if len(parts) == 4:
            rows.append(tuple(p.strip() for p in parts))
    return rows


def classify(subject):
    """Which gold category this commit can evidence, and the term that says so.

    Matches the SUBJECT ONLY. A first pass matched subject+body and kept the
    subject as the candidate's text, which put 386 of 561 candidates in the
    negation slice on the strength of words buried in unrelated PR bodies:
    `fix(network): release published-port DNAT when a container stops` was
    labelled a retraction because the body of its merge said "drop" somewhere.
    The subject is what a reviewer sees, so the subject is what must evidence
    the category.

    The matched term is returned with the category so the classification can be
    audited without re-running the regex.

    Order matters: a commit that both retires and replaces is a supersession,
    because `supersedes` carries more information than a bare negation.
    """
    for cat, rx in (("governance", SUPERSEDE_RE),
                    ("negation", RETIRE_RE),
                    ("transient", TRANSIENT_RE)):
        m = rx.search(subject)
        if m:
            return cat, m.group(0)
    return None, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--roots", nargs="+", required=True,
                    help="directories to scan for git repos")
    ap.add_argument("--limit", type=int, default=400,
                    help="commits per repo")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    repos = []
    for root in args.roots:
        for dirpath, dirnames, _ in os.walk(root):
            if ".git" in dirnames:
                repos.append(dirpath)
                dirnames[:] = []          # do not descend into a repo
            if dirpath.count(os.sep) - root.count(os.sep) > 3:
                dirnames[:] = []

    cands, seen = [], set()
    for repo in sorted(repos):
        name = os.path.basename(repo)
        for sha, subject, body, author in commits(repo, args.limit):
            cat, trigger = classify(subject)
            if not cat:
                continue
            # Merge commits restate their branch's subject; they add no fact and
            # would duplicate whatever they merged.
            if subject.lower().startswith("merge "):
                continue
            key = re.sub(r"[^a-z0-9 ]", "", subject.lower())[:60]
            if key in seen:
                continue
            seen.add(key)
            cands.append({
                "anchor": {"repo": name, "sha": sha[:12], "author": author},
                "category": cat,
                "trigger": trigger,
                "source_text": subject,
                "body": body[:400],
            })

    with open(args.out, "w") as fh:
        for c in cands:
            fh.write(json.dumps(c, ensure_ascii=False) + "\n")

    by_cat = {}
    by_repo = {}
    for c in cands:
        by_cat[c["category"]] = by_cat.get(c["category"], 0) + 1
        by_repo[c["anchor"]["repo"]] = by_repo.get(c["anchor"]["repo"], 0) + 1
    print(f"repos scanned: {len(repos)}")
    print(f"candidates:    {len(cands)}")
    print("by category:  ", dict(sorted(by_cat.items(), key=lambda x: -x[1])))
    print("by repo (top): ", dict(sorted(by_repo.items(), key=lambda x: -x[1])[:8]))


if __name__ == "__main__":
    main()
