"""Mine a verifiable entity and fact inventory from git repositories.

This is step 1 of HARNESS_DESIGN.md. It does NOT write notes. It writes the
inventory a generator composes notes FROM, so that the gold triple is known
before the note exists rather than read back off it.

The distinction that makes a 10,000-note set possible: every fact here is
already true, and git can prove it. A rename is not an opinion —

    R097  src/backend/drm/compositor/dumb.rs -> src/backend/drm/exporter/dumb.rs

is a fact with a diff behind it, so `dumb.rs (compositor) also_known_as
dumb.rs (exporter)` needs no labeller. At 70 notes a human can label; at 10,000
the source has to.

WHAT IS AND IS NOT TRUSTWORTHY HERE.

  renames    git's own rename detection, `--diff-filter=R`. Similarity-scored,
             so R097 is near-certain and R050 is a guess. We keep >=90 only.
  deletions  a path that was deleted. Verifiable, but NOT proof the concept is
             gone — a file often moves out of one repo into another. These seed
             `negation` notes about THE FILE, never about the idea.
  authorship (author, repo) pairs. Real, but a single drive-by commit makes
             someone a "contributor" in a way a human would not say. Pairs are
             kept with their commit count so the generator can require a floor.
  versions   tags, ordered by creation date, giving genuine supersession chains.

Author names are real people. They appear in public git history, which is why
using them is reasonable, but they are used ONLY as entity strings in synthetic
notes about repository facts. Nothing here invents a personal attribute — no
addresses, employers, relationships or opinions are attached to a real name.
"""
import argparse
import collections
import json
import os
import re
import subprocess


def git(repo, *args, timeout=180):
    out = subprocess.run(["git", "-C", repo, *args],
                         capture_output=True, text=True, timeout=timeout)
    return out.stdout if out.returncode == 0 else ""


def find_repos(roots, max_depth=3):
    """Working trees (a .git subdirectory) and bare repos (a *.git directory).

    Bare clones matter because the org repos are fetched with
    `--bare --filter=blob:none`: full commit history, no file contents. Renames,
    deletions and authorship all come from commit metadata, so a blobless clone
    carries every fact this miner needs at a fraction of the size — 46 repos in
    26 MB against gigabytes for full checkouts.
    """
    repos = []
    for root in roots:
        root = os.path.expanduser(root)
        if root.endswith(".git") and os.path.isdir(root):
            repos.append(root)
            continue
        for dirpath, dirnames, _ in os.walk(root):
            if ".git" in dirnames:
                repos.append(dirpath)
                dirnames[:] = []
                continue
            bare = [d for d in dirnames if d.endswith(".git")]
            for b in bare:
                repos.append(os.path.join(dirpath, b))
            if bare:
                dirnames[:] = [d for d in dirnames if not d.endswith(".git")]
            if dirpath.count(os.sep) - root.count(os.sep) > max_depth:
                dirnames[:] = []
    return sorted(set(repos))


def mine_renames(repo, min_score=90):
    """High-confidence path changes, SPLIT BY KIND.

    git INFERS renames by content similarity; it does not record them. A low
    score means "these two files look a bit alike", which is not a fact. 90 is
    strict enough that the two paths are the same file.

    The split matters more than the threshold. Of 1,758 detected path changes in
    these repos, only 149 change the BASENAME; the other 1,609 keep the name and
    change the directory. Those are moves, not renames, and generating
    `also_known_as` from them would assert that a file is now called something it
    was already called — wrong gold, 1,609 times over. They are still facts, just
    a different predicate:

        kind="rename"  basename changed  -> also_known_as / supersedes
        kind="move"    directory changed -> located_in
    """
    out = git(repo, "log", "--diff-filter=R", "--name-status",
              "--format=%x01%H", "-M")
    facts, sha = [], None
    for line in out.splitlines():
        if line.startswith("\x01"):
            sha = line[1:].strip()
            continue
        m = re.match(r"^R(\d{3})\t(\S+)\t(\S+)$", line)
        if m and int(m.group(1)) >= min_score:
            old, new = m.group(2), m.group(3)
            kind = ("rename" if os.path.basename(old) != os.path.basename(new)
                    else "move")
            facts.append({"old": old, "new": new, "kind": kind,
                          "score": int(m.group(1)), "sha": (sha or "")[:12]})
    return facts


def mine_deletions(repo):
    out = git(repo, "log", "--diff-filter=D", "--name-status", "--format=%x01%H")
    facts, sha = [], None
    for line in out.splitlines():
        if line.startswith("\x01"):
            sha = line[1:].strip()
            continue
        m = re.match(r"^D\t(\S+)$", line)
        if m:
            facts.append({"path": m.group(1), "sha": (sha or "")[:12]})
    return facts


def mine_authors(repo):
    """(author -> commit count). The count lets the generator demand a floor:
    one drive-by commit does not make someone a contributor in the sense a
    person would assert in a remembered note."""
    counts = collections.Counter()
    for line in git(repo, "log", "--format=%an").splitlines():
        name = line.strip()
        # Bot and CI identities are not people and must not become subjects.
        if not name or re.search(r"\b(bot|\[bot\]|actions|ci|dependabot|renovate)\b",
                                 name, re.I):
            continue
        counts[name] += 1
    return counts


def mine_versions(repo):
    """Tags in creation order, so consecutive pairs are real supersessions."""
    out = git(repo, "tag", "--sort=creatordate")
    tags = [t.strip() for t in out.splitlines() if t.strip()]
    return [t for t in tags if re.search(r"\d", t)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--roots", nargs="+", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--min-rename-score", type=int, default=90)
    args = ap.parse_args()

    inventory = {"repos": {}, "people": {}, "generated_from": []}
    for repo in find_repos(args.roots):
        name = os.path.basename(repo)
        if name.endswith(".git"):
            name = name[:-4]
        # Bare clones are named "<org>-<repo>.git"; the org prefix is noise in a
        # note that says "X contributes to Y".
        for pre in ("RakuenSoftware-", "JBailes-"):
            if name.startswith(pre):
                name = name[len(pre):]
        authors = mine_authors(repo)
        if not authors:
            continue
        renames = mine_renames(repo, args.min_rename_score)
        deletions = mine_deletions(repo)
        versions = mine_versions(repo)
        inventory["repos"][name] = {
            "renames": renames,
            "deletions": deletions,
            "versions": versions,
            "authors": dict(authors.most_common()),
            "commits": sum(authors.values()),
        }
        for person, n in authors.items():
            slot = inventory["people"].setdefault(person, {})
            slot[name] = n
        inventory["generated_from"].append(repo)

    with open(args.out, "w") as fh:
        json.dump(inventory, fh, indent=1, ensure_ascii=False)

    ren = sum(1 for v in inventory["repos"].values()
              for x in v["renames"] if x["kind"] == "rename")
    mov = sum(1 for v in inventory["repos"].values()
              for x in v["renames"] if x["kind"] == "move")
    r = ren + mov
    d = sum(len(v["deletions"]) for v in inventory["repos"].values())
    t = sum(len(v["versions"]) for v in inventory["repos"].values())
    pairs = sum(len(v) for v in inventory["people"].values())
    multi = sum(1 for v in inventory["people"].values() if len(v) > 1)
    print(f"repos            {len(inventory['repos'])}")
    print(f"people           {len(inventory['people'])}  "
          f"({multi} contributed to more than one repo)")
    print(f"(person,repo)    {pairs}")
    print(f"renames (name changed)  {ren}   -> also_known_as")
    print(f"moves   (dir changed)   {mov}   -> located_in")
    print(f"deletions        {d}")
    print(f"versions         {t}")
    print(f"verifiable facts {r + d + pairs + t}")


if __name__ == "__main__":
    main()
