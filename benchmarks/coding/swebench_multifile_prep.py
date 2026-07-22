#!/usr/bin/env python3
"""Prepare MULTI-FILE SWE-bench Verified instances for the decomposition benchmark.

Unlike swebench_supervised_prep (SWE-bench Lite, single file), this fetches SWE-bench
Verified, keeps only instances whose gold patch touches >= --min-files .py files, and
extracts a code region for EACH touched file. These are the tasks where splitting the
work across parallel workers (one per file) can actually beat a single model solving the
whole thing.

Output per instance:
  {instance_id, repo, base_commit, problem,
   files: [{file, region_start, region}, ...]}

Example:
  python3 benchmarks/coding/swebench_multifile_prep.py \
    --min-files 3 --limit 12 \
    --out benchmarks/results/swebench_multifile/regions
"""
from __future__ import annotations
import argparse, json, os, re, subprocess, sys, urllib.request
from pathlib import Path

HF = ("https://datasets-server.huggingface.co/rows"
      "?dataset=princeton-nlp%2FSWE-bench_Verified&config=default&split=test")


def _fetch(pages: int) -> list[dict]:
    rows, off = [], 0
    for _ in range(pages):
        for attempt in range(3):
            try:
                with urllib.request.urlopen(f"{HF}&offset={off}&length=100", timeout=45) as r:
                    d = json.load(r)
                break
            except Exception:
                if attempt == 2:
                    raise
        page = d.get("rows", [])
        rows += [it["row"] for it in page]
        if len(page) < 100:
            break
        off += 100
    return rows


def _py_files(patch: str) -> list[str]:
    return sorted(set(f for f in re.findall(r"^\+\+\+ b/(\S+)", patch, re.M) if f.endswith(".py")))


def _hunk_start(patch: str, fpath: str) -> int | None:
    # first @@ hunk of the section that patches fpath
    m = re.search(rf"^\+\+\+ b/{re.escape(fpath)}\n(?:.*\n)*?@@ -(\d+)", patch, re.M)
    return int(m.group(1)) if m else None


def _region(repo_dir: Path, fpath: str, start: int, region_lines: int):
    full = repo_dir / fpath
    try:
        lines = full.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return None
    lo = max(0, start - region_lines)
    hi = min(len(lines), start + region_lines)
    return "\n".join(lines[lo:hi]), lo + 1


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--min-files", type=int, default=3)
    ap.add_argument("--limit", type=int, default=12)
    ap.add_argument("--pages", type=int, default=5)
    ap.add_argument("--out", default="benchmarks/results/swebench_multifile/regions")
    ap.add_argument("--repos", default=os.environ.get("SWE_REPOS_DIR", "/tmp/swe-repos"))
    ap.add_argument("--region-lines", type=int, default=200)
    args = ap.parse_args()

    rows = _fetch(args.pages)
    cand = []
    for r in rows:
        fs = _py_files(r.get("patch", ""))
        if len(fs) >= args.min_files:
            cand.append((r, fs))
    cand.sort(key=lambda x: (-len(x[1]), x[0]["instance_id"]))
    cand = cand[:args.limit]
    print(f"{len(rows)} verified rows; {len(cand)} with >= {args.min_files} .py files (capped {args.limit})",
          file=sys.stderr)

    out_dir, repos_dir = Path(args.out), Path(args.repos)
    out_dir.mkdir(parents=True, exist_ok=True)
    repos_dir.mkdir(parents=True, exist_ok=True)
    for repo in {r["repo"] for r, _ in cand}:
        dst = repos_dir / repo.replace("/", "__")
        if not dst.exists():
            print(f"cloning {repo} (blobless)", file=sys.stderr)
            subprocess.run(["git", "clone", "--quiet", "--filter=blob:none",
                            f"https://github.com/{repo}", str(dst)], check=True)

    n = 0
    for r, fs in cand:
        dst = repos_dir / r["repo"].replace("/", "__")
        subprocess.run(["git", "checkout", "--quiet", "--force", r["base_commit"]], cwd=dst)
        subprocess.run(["git", "clean", "-fdq"], cwd=dst)
        files = []
        for fp in fs:
            st = _hunk_start(r["patch"], fp)
            if st is None:
                continue
            reg = _region(dst, fp, st, args.region_lines)
            if reg:
                files.append({"file": fp, "region_start": reg[1], "region": reg[0]})
        if len(files) < args.min_files:
            print(f"{r['instance_id']} SKIP (only {len(files)} files extracted)", file=sys.stderr)
            continue
        json.dump({"instance_id": r["instance_id"], "repo": r["repo"],
                   "base_commit": r["base_commit"], "problem": r["problem_statement"],
                   "files": files},
                  (out_dir / f"{r['instance_id']}.json").open("w"))
        n += 1
        print(f"{r['instance_id']} ok ({len(files)} files: {', '.join(f['file'] for f in files)})",
              file=sys.stderr)
    print(f"prepared {n} multi-file instances -> {out_dir}", file=sys.stderr)


if __name__ == "__main__":
    main()
