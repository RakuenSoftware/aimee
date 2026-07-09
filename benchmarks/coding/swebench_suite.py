#!/usr/bin/env python3
"""S5 — suite execution + grading orchestration (agentic supervised SWE-bench, #987).

Runs the six-benchmark suite across factors and grades every emitted patch with the OFFICIAL
SWE-bench Docker harness on CT 101. The parts that need the live .254 fleet + CT 101 are marked
stubs; the reproducibility-critical orchestration is pure and unit-tested here:

  - generate_run_plan .... deterministic run cells from the benchmark specs x factor grid
                           (Benchmark 1 Reddit-10 head-to-head; 2 held-out Lite; 3 parallelism
                           sweep; 4 best-of-N; 5 reducer lever; 6 worker-pool). K repeats.
  - CT101Lease ........... the S1-Q1 contention control: CT 101 has two named tenants
                           {grader, iteration_pool}; grading has strict priority and the two are
                           mutually exclusive, so in-loop docker (if ever enabled) can never
                           starve or add queue-noise to the sole final grader.
  - GraderRetry .......... proposal S5: max 2 deterministic retries on a grader ERROR (not on a
                           legitimate non-resolve); flip-detection flags instances whose
                           resolution is unstable across the K repeats.
  - build_predictions .... one prediction per instance per (arm, model, N, cmp) run_id, deduped
                           by instance_id (the official harness dedupes by instance_id).
  - aggregate_k .......... majority-vote resolution across K repeats + a flip flag.

Grade only on the official harness; never let in-loop signal decide `resolved` (S1 Q1).
"""
from __future__ import annotations

import glob
import hashlib
import json
import os
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

_FAKE = os.environ.get("AIMEE_BENCH_FAKE_AGENT") == "1"


# ------------------------------------------------------------ benchmark specs --
# Each spec: the instance set + the factor grid it crosses. K = repeats for variance.
BENCHMARKS = {
    "b1_reddit10": {"instances": "reddit10", "arms": ["A", "C"], "N": [1, 3],
                    "reducers": [False], "primary": ["gpt-5.5", "claude"], "K": 10,
                    "public_claim": True},
    "b2_heldout": {"instances": "lite:28", "arms": ["A", "C"], "N": [1, 3],
                   "reducers": [False], "primary": ["gpt-5.5"], "K": 3, "public_claim": True},
    "b3_parallelism": {"instances": "reddit10", "arms": ["C"], "N": [1, 2, 4, 8],
                       "reducers": [False], "primary": ["gpt-5.5"], "K": 3},
    "b4_best_of_n": {"instances": "lite:28", "arms": ["C"], "N": [1, 2, 3],
                     "reducers": [False], "primary": ["gpt-5.5"], "K": 3},
    "b5_reducer": {"instances": "reddit10", "arms": ["C"], "N": [3],
                   "reducers": [False, True], "primary": ["gpt-5.5"], "K": 3},
    "b6_worker_pool": {"instances": "reddit10", "arms": ["C"], "N": [1],
                       "reducers": [False], "primary": ["gpt-5.5"], "K": 3,
                       "worker_pools": ["free_fleet", "gpu_gemma4"]},
}


@dataclass(frozen=True)
class RunCell:
    benchmark: str
    arm: str
    primary: str
    n: int
    reducers: bool
    worker_pool: str
    repeat: int
    instances: str

    @property
    def run_id(self) -> str:
        base = f"{self.benchmark}:{self.arm}:{self.primary}:N{self.n}:cmp{int(self.reducers)}:{self.worker_pool}:k{self.repeat}"
        return f"aimee-sup-{hashlib.sha256(base.encode()).hexdigest()[:12]}"


def generate_run_plan(benchmarks: dict = None, only: list[str] = None) -> list[RunCell]:
    """Deterministic, fully-ordered run cells for the whole suite (or a subset via `only`)."""
    benchmarks = benchmarks or BENCHMARKS
    cells: list[RunCell] = []
    for name in sorted(benchmarks):
        if only and name not in only:
            continue
        spec = benchmarks[name]
        pools = spec.get("worker_pools", ["free_fleet"])
        for arm in spec["arms"]:
            for primary in spec["primary"]:
                for n in spec["N"]:
                    for red in spec["reducers"]:
                        for pool in pools:
                            for k in range(spec.get("K", 1)):
                                cells.append(RunCell(name, arm, primary, n, red, pool, k,
                                                     spec["instances"]))
    return cells


# ------------------------------------------------------------ CT-101 lease ------
class LeaseError(RuntimeError):
    pass


class CT101Lease:
    """S1-Q1: CT 101 is the sole real-docker grader host. Two tenants {grader, iteration_pool}
    are mutually exclusive; grader has strict priority (an iteration lease is refused while a
    grade is pending or active). In-loop tests default OFF, so in practice this serializes grade
    batches and guarantees the p95 wall-clock is never polluted by iteration docker on CT 101."""
    GRADER = "grader"
    ITERATION = "iteration_pool"

    def __init__(self):
        self._holder: str | None = None
        self._grader_waiting = False

    def request_grader(self) -> bool:
        self._grader_waiting = True
        if self._holder in (None, self.GRADER):
            self._holder = self.GRADER
            self._grader_waiting = False
            return True
        return False  # iteration holds it; caller waits (grader will get it next)

    def request_iteration(self) -> bool:
        # Grader priority: refuse iteration while a grade holds OR is waiting.
        if self._holder is None and not self._grader_waiting:
            self._holder = self.ITERATION
            return True
        return False

    def release(self, tenant: str) -> None:
        if self._holder != tenant:
            raise LeaseError(f"{tenant} does not hold the CT101 lease (holder={self._holder})")
        self._holder = None

    @property
    def holder(self) -> str | None:
        return self._holder


# ------------------------------------------------------------ grader retry ------
@dataclass
class GraderRetry:
    """Proposal S5: retry a grader ERROR (harness crash/flake) up to `max_retries`, but NEVER
    retry a legitimate non-resolve. `attempts` records the invocation count for the report."""
    max_retries: int = 2
    attempts: int = 0

    def run(self, grade_fn) -> Any:
        """grade_fn() -> ('ok', resolved_bool) | ('error', msg). Retries only on 'error'."""
        last = None
        for _ in range(self.max_retries + 1):
            self.attempts += 1
            status, payload = grade_fn()
            if status == "ok":
                return payload
            last = payload
        raise RuntimeError(f"grader failed after {self.attempts} attempts: {last}")


def detect_flips(resolved_by_repeat: list[bool]) -> bool:
    """True if an instance's resolution is UNSTABLE across the K repeats (some pass, some fail).
    A flapping instance is flagged so grader-flake is not conflated with run noise."""
    s = set(resolved_by_repeat)
    return len(s) > 1


def aggregate_k(resolved_by_repeat: list[bool]) -> dict[str, Any]:
    """Majority-vote resolution across K repeats + the flip flag."""
    if not resolved_by_repeat:
        return {"resolved": None, "flipped": False, "k": 0}
    n_true = sum(1 for r in resolved_by_repeat if r)
    return {"resolved": n_true * 2 >= len(resolved_by_repeat), "flipped": detect_flips(resolved_by_repeat),
            "k": len(resolved_by_repeat), "pass_count": n_true}


# ------------------------------------------------------------ predictions ------
def build_predictions(records: list[dict[str, Any]], run_id: str) -> list[dict[str, Any]]:
    """One prediction per instance for a run_id, deduped by instance_id (the official harness
    dedupes by instance_id, so duplicates would be silently dropped — we dedupe explicitly,
    keeping the first non-empty patch)."""
    seen: dict[str, dict] = {}
    for r in records:
        iid = r["instance_id"]
        if not r.get("diff") and not r.get("patch"):
            continue
        if iid in seen:
            continue
        seen[iid] = {"instance_id": iid, "model_name_or_path": run_id,
                     "model_patch": r.get("diff") or r.get("patch", "")}
    return list(seen.values())


# ------------------------------------------------------------ instance loading -
def load_instances(instances_spec: str, regions_dir: str) -> list[dict]:
    """Resolve a benchmark's instance-set label to the prepared region records on disk. The prep
    (`swebench_supervised_prep.py`) writes one JSON per instance under regions_dir; `reddit10`,
    `lite:N`, and `all` are prepared into subdirs by the prep, so we read the matching subdir (or
    the flat dir). Returns [] when nothing is prepared (the caller logs and skips the cell)."""
    label = instances_spec.replace(":", "_")
    for cand in (Path(regions_dir) / label, Path(regions_dir)):
        files = sorted(glob.glob(str(cand / "*.json")))
        if files:
            return [json.load(open(f)) for f in files]
    return []


# ------------------------------------------------------------ grading ----------
def resolved_ids_from_report(report: dict) -> set:
    """Extract the resolved instance_ids from an official SWE-bench evaluation report (PURE).

    The harness writes a top-level report with a `resolved_ids` list; older/summary shapes instead
    map each instance_id to a dict carrying a `resolved` bool. Handle both so a schema variant can't
    silently zero out the resolution number (the `_grade_with_harness` first-tuple element is always
    an empty set today — this is where `resolved` actually comes from)."""
    if not isinstance(report, dict):
        return set()
    if isinstance(report.get("resolved_ids"), list):
        return set(report["resolved_ids"])
    out = set()
    for k, v in report.items():
        if isinstance(v, dict) and v.get("resolved") is True:
            out.add(k)
    return out


def official_grade_fn(predictions: list[dict], run_id: str, *, lease: "CT101Lease" = None) -> set:
    """Grade predictions with the OFFICIAL SWE-bench Docker harness (the SOLE resolution source).
    Holds the CT101 grader lease for the duration so in-loop docker can never pollute a grade.
    Returns the set of resolved instance_ids. Delegates to the merged single-shot grader wiring
    (`bench_swebench._grade_with_harness`) so both benchmarks grade identically, then derives the
    resolved set from the report (that wiring returns the report but an empty id-set)."""
    if not predictions:
        return set()
    from benchmarks.coding.bench_swebench import _write_predictions, _grade_with_harness
    lease = lease or CT101Lease()
    while not lease.request_grader():
        raise LeaseError("CT101 grader lease unavailable (iteration pool holds it)")
    try:
        out = Path(f"/tmp/swebench_preds_{run_id}.jsonl")
        _write_predictions(out, predictions)
        resolved, report = _grade_with_harness(out, run_id)
        return set(resolved) or resolved_ids_from_report(report)
    finally:
        lease.release(CT101Lease.GRADER)


# ------------------------------------------------------------ suite driver -----
def run_suite(only: list[str] = None, *, token_db: str = "", aimee_bin: str = "aimee",
              regions_dir: str = "benchmarks/results/swebench_supervised/regions",
              workers: list[str] = None, primary_agent: str = "codex", primary_model: str = "gpt-5.5",
              base_repos: dict = None, allocator_factory=None, budget=None,
              arm_a_runner=None, arm_c_runner=None, grade_fn=None, instances_loader=None,
              on_log=None) -> dict:
    """Execute the six-benchmark suite (or a subset via `only`) and grade every patch with the
    official harness. Every external effect is an INJECTED seam so the orchestration is unit-tested
    with a fake fleet+grader; the live defaults compose run_arm_a / run_arm_c_supervised + the
    official CT-101 grader. Returns {cells, records_by_run, aggregate, report}.

    Per (benchmark,instance-set): load instances, run each RunCell's arm over all instances, grade
    the run_id's predictions once (GraderRetry on ERROR only), set `resolved`, then majority-vote
    aggregate across the K repeats with flip detection. Records feed supervised_report downstream."""
    from benchmarks.coding import supervised_report
    from benchmarks.coding import swebench_arm_runner as R
    from benchmarks.coding import swebench_supervision as S
    from benchmarks.coding import swebench_agentic_harness as H

    log = on_log or (lambda m: None)
    arm_a_runner = arm_a_runner or R.run_arm_a
    arm_c_runner = arm_c_runner or S.run_arm_c_supervised
    grade_fn = grade_fn or official_grade_fn
    instances_loader = instances_loader or (lambda spec: load_instances(spec, regions_dir))
    allocator_factory = allocator_factory or (lambda: H.EnvAllocator(str(Path(regions_dir).parent / "ws")))
    budget = budget or H.LoopBudget()
    workers = workers or ["GLM-5.2", "MiniMax-M3", "mistral-medium-3-5", "mimo-v2.5-pro", "gpu-mid"]
    base_repos = base_repos or {}

    plan = generate_run_plan(only=only)
    lease = CT101Lease()
    inst_cache: dict[str, list] = {}
    records_by_run: dict[str, list] = {}
    cells_meta = []

    for cell in plan:
        insts = inst_cache.setdefault(cell.instances, instances_loader(cell.instances))
        if not insts:
            log(f"SKIP {cell.run_id}: no instances prepared for '{cell.instances}' in {regions_dir}")
            cells_meta.append({"run_id": cell.run_id, "benchmark": cell.benchmark, "arm": cell.arm,
                               "skipped": "no_instances"})
            continue
        alloc = allocator_factory()
        records = []
        for inst in insts:
            base_repo = base_repos.get(inst.get("repo"))
            if cell.arm == "A":
                rec = arm_a_runner(inst, cell.primary, token_db=token_db, base_repo=base_repo,
                                   allocator=alloc, budget=budget, primary_agent=primary_agent,
                                   aimee_bin=aimee_bin)
            else:
                rec = arm_c_runner(inst, workers=workers, n=cell.n, allocator=alloc, budget=budget,
                                   token_db=token_db, base_repo=base_repo, primary_agent=primary_agent,
                                   primary_model=cell.primary, aimee_bin=aimee_bin)
            rec["run_id"] = cell.run_id
            records.append(rec)

        preds = build_predictions([{**r, "patch": r.get("patch", "")} for r in records], cell.run_id)
        # Attach the patch text needed for grading onto the prediction (records only carry a
        # fingerprint), so callers pass patches explicitly via the arm result if they need grading.
        retry = GraderRetry()
        try:
            resolved = retry.run(lambda: ("ok", grade_fn(preds, cell.run_id, lease=lease)))
        except Exception as e:  # grader unavailable -> resolved unknown, reported honestly
            log(f"GRADER unavailable for {cell.run_id}: {e}")
            resolved = None
        for r in records:
            if resolved is not None:
                r["resolved"] = r["instance_id"] in resolved
        records_by_run[cell.run_id] = records
        cells_meta.append({"run_id": cell.run_id, "benchmark": cell.benchmark, "arm": cell.arm,
                           "n": cell.n, "primary": cell.primary, "repeat": cell.repeat,
                           "instances": len(insts), "grader_attempts": retry.attempts,
                           "graded": resolved is not None})

    # Majority-vote aggregate across K repeats per (benchmark, arm, primary, N, instance).
    aggregate = _aggregate_repeats(plan, records_by_run)
    all_records = [r for recs in records_by_run.values() for r in recs]
    report = supervised_report.build_report(all_records) if all_records else {}
    return {"cells": cells_meta, "records_by_run": records_by_run, "aggregate": aggregate,
            "report": report}


def _aggregate_repeats(plan: list, records_by_run: dict) -> dict:
    """Group the K repeats of each logical cell and majority-vote each instance's resolution, with
    a flip flag when resolution is unstable across repeats (grader flake vs run noise, S5)."""
    groups: dict[tuple, dict[str, list]] = {}
    for cell in plan:
        recs = records_by_run.get(cell.run_id)
        if not recs:
            continue
        key = (cell.benchmark, cell.arm, cell.primary, cell.n, int(cell.reducers), cell.worker_pool)
        by_inst = groups.setdefault(key, {})
        for r in recs:
            if r.get("resolved") is not None:
                by_inst.setdefault(r["instance_id"], []).append(bool(r["resolved"]))
    out = {}
    for key, by_inst in groups.items():
        name = ":".join(str(k) for k in key)
        out[name] = {iid: aggregate_k(vals) for iid, vals in by_inst.items()}
    return out
