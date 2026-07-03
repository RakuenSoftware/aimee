#!/usr/bin/env python3
"""Prepare SWE-bench instances for the supervised benchmark: fetch the dataset,
clone each repo at its base_commit, and extract the code region around the bug so
both the solo primary (arm A) and the delegate workers (arm C) see the same code.

Instance sets:
  reddit10  - the exact 10 SWE-bench Lite instances from the Reddit post
              (direct head-to-head).
  lite:N    - a deterministic wide sample of N SWE-bench Lite instances spanning
              all four repos (django/sympy/scikit-learn/pytest) for range.
  all       - every SWE-bench Lite instance (300).

Regions default to +/-300 lines around the gold-patch hunk (a realistic "the code
you must read to fix it"); tune with --region-lines.
"""
from __future__ import annotations
import argparse, json, os, re, subprocess, sys, urllib.request
from pathlib import Path

HF = ("https://datasets-server.huggingface.co/rows"
      "?dataset=princeton-nlp%2FSWE-bench_Lite&config=default&split=test")

REDDIT10 = [
    "pytest-dev__pytest-11143", "scikit-learn__scikit-learn-13439", "sympy__sympy-20212",
    "django__django-12908", "pytest-dev__pytest-6116", "django__django-13447",
    "django__django-15814", "django__django-11179", "sympy__sympy-13480",
    "scikit-learn__scikit-learn-13584",
]


def _fetch_lite() -> dict[str, dict]:
    """Fetch the full SWE-bench Lite test split via the HF rows API, paginating until
    a short/empty page (no hardcoded 300, so it survives dataset size changes; m5)."""
    rows: dict[str, dict] = {}
    off = 0
    while True:
        for attempt in range(3):
            try:
                with urllib.request.urlopen(f"{HF}&offset={off}&length=100", timeout=45) as r:
                    d = json.load(r)
                break
            except Exception:
                if attempt == 2:
                    raise
        page = d.get("rows", [])
        for item in page:
            rows[item["row"]["instance_id"]] = item["row"]
        if len(page) < 100:
            break
        off += 100
    return rows


def _select(all_rows: dict[str, dict], spec: str) -> list[dict]:
    if spec == "reddit10":
        return [all_rows[i] for i in REDDIT10 if i in all_rows]
    if spec == "all":
        return sorted(all_rows.values(), key=lambda r: r["instance_id"])
    if spec.startswith("lite:"):
        n = int(spec.split(":", 1)[1])
        # deterministic wide sample: round-robin across repos, sorted within each
        by_repo: dict[str, list] = {}
        for r in sorted(all_rows.values(), key=lambda r: r["instance_id"]):
            by_repo.setdefault(r["repo"], []).append(r)
        out, i = [], 0
        while len(out) < n and any(i < len(v) for v in by_repo.values()):
            for repo in sorted(by_repo):
                if i < len(by_repo[repo]) and len(out) < n:
                    out.append(by_repo[repo][i])
            i += 1
        return out
    raise SystemExit(f"unknown instance spec: {spec}")


def _extract_region(repo_dir: Path, patch: str, region_lines: int) -> tuple[str, str, int] | None:
    m_file = re.search(r"^\+\+\+ b/(\S+)", patch, re.M)
    m_hunk = re.search(r"^@@ -(\d+)", patch, re.M)
    if not m_file or not m_hunk:
        return None
    fpath, start = m_file.group(1), int(m_hunk.group(1))
    full = repo_dir / fpath
    try:
        lines = full.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return None
    lo = max(0, start - region_lines)
    hi = min(len(lines), start + region_lines)
    return fpath, "\n".join(lines[lo:hi]), lo + 1


def prepare(instances: list[dict], out_dir: Path, repos_dir: Path, region_lines: int) -> int:
    out_dir.mkdir(parents=True, exist_ok=True)
    repos_dir.mkdir(parents=True, exist_ok=True)
    for repo in {r["repo"] for r in instances}:
        dst = repos_dir / repo.replace("/", "__")
        if not dst.exists():
            print(f"cloning {repo}", file=sys.stderr)
            # Blobless partial clone: keeps full history (so ANY base_commit checks
            # out, unlike --depth 1) but fetches blobs on demand -> small (m4).
            subprocess.run(["git", "clone", "--quiet", "--filter=blob:none",
                            f"https://github.com/{repo}", str(dst)], check=True)
    n = 0
    for r in instances:
        dst = repos_dir / r["repo"].replace("/", "__")
        subprocess.run(["git", "checkout", "--quiet", "--force", r["base_commit"]], cwd=dst)
        subprocess.run(["git", "clean", "-fdq"], cwd=dst)
        reg = _extract_region(dst, r["patch"], region_lines)
        if not reg:
            print(f"{r['instance_id']} SKIP (no parseable region)", file=sys.stderr)
            continue
        fpath, region, region_start = reg
        json.dump(
            {"instance_id": r["instance_id"], "repo": r["repo"], "base_commit": r["base_commit"],
             "file": fpath, "region_start": region_start, "region": region,
             "problem": r["problem_statement"]},
            (out_dir / f"{r['instance_id']}.json").open("w"),
        )
        n += 1
        print(f"{r['instance_id']} ok ({fpath})", file=sys.stderr)
    return n


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--instances", default="reddit10", help="reddit10 | lite:N | all")
    ap.add_argument("--out", default="benchmarks/results/swebench_supervised/regions")
    ap.add_argument("--repos", default=os.environ.get("SWE_REPOS_DIR", "/tmp/swe-repos"))
    ap.add_argument("--region-lines", type=int, default=300)
    args = ap.parse_args()
    rows = _fetch_lite()
    picked = _select(rows, args.instances)
    print(f"selected {len(picked)} instances ({args.instances})", file=sys.stderr)
    n = prepare(picked, Path(args.out), Path(args.repos), args.region_lines)
    print(f"prepared {n} regions -> {args.out}", file=sys.stderr)


if __name__ == "__main__":
    main()
