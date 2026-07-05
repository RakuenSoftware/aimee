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

import hashlib
from dataclasses import dataclass, field
from typing import Any


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


# ------------------------------------------------------------ live stub --------
def run_suite(only: list[str] = None, *, token_db: str, aimee_bin: str):
    raise NotImplementedError(
        "live suite execution not wired: for each RunCell from generate_run_plan(), dispatch arm "
        "A (run_arm_a) or arm C (run_arm_c_supervised) on the .254 fleet, hold CT101Lease.GRADER "
        "while grading each run_id's build_predictions() with the official swebench Docker harness "
        "on CT 101 under GraderRetry, aggregate_k across repeats, then feed the records to "
        "supervised_report + supervised_report_panels. Requires the live server + CT 101.")
