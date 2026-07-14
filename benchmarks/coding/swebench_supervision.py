#!/usr/bin/env python3
"""S3 — arm C supervision loop: the pure honesty core (agentic supervised SWE-bench, #987).

Arm C: N cheap/local fleet workers each run the S1 agentic loop; the primary SUPERVISES across
turns and we count only the primary's tokens. The public claim ("beats Reddit's -75.5% at no
wall-clock penalty") is only honest if the primary supervises CHEAPLY — reading short, capped,
leak-guarded digests and picking among candidate diffs, NEVER the raw code. This module is the
reproducibility-critical machinery that MECHANICALLY enforces that, per the S3 design roundtable
(2026-07-05, 6/7 panel). Everything here is a pure function — unit-testable with no live server,
no LLM, no network. The only live parts (worker dispatch; an optional LLM-supervisor selection
ablation) are marked stubs.

Ratified rulings baked in:
  Q1  the turn-digest is a CONSTRUCTED, allowlisted data product with HARD token caps, not a
      prompt convention; a serializer leak-guard REJECTS any field carrying file-like/raw-source
      content and neutralizes tool-call-looking syntax (anti prompt-injection).
  Q2  a hard tool allowlist; escalation is gated (only on worker terminal-fail or deterministic
      stuck), bounded, and separately attributed; a run whose escalation tokens exceed 40% of the
      primary's input is marked ESCALATION_DOMINATED and excluded from the headline.
  Q3  best-of-N selection is a DETERMINISTIC pure function over structured worker-reported signals
      (never the official grader, never raw logs) — an LLM selector would be non-reproducible;
      oracle-best-of-N (any candidate passes the grader) vs actual-best-of-N (the selected patch
      passes) decompose supervisor skill from worker diversity.
  Q4  context-drift is bounded by a sliding window of the last K digests + a deterministic
      fold-then-evict of older turns to a one-line summary, under a hard total cap.
  Q5  redirect defaults OFF for the public-claim set (PR#986's limiter was worker reliability,
      not supervision); if enabled it is a bounded, attributed ablation.
"""
from __future__ import annotations

import json
import os
import random
import re
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from enum import Enum

_FAKE = os.environ.get("AIMEE_BENCH_FAKE_AGENT") == "1"

# ------------------------------------------------------------ enums ------------
class VerifyEnum(str, Enum):
    PASS = "PASS"
    FAIL = "FAIL"
    ERROR = "ERROR"
    NOT_RUN = "NOT_RUN"


class WorkerState(str, Enum):
    RUNNING = "running"
    DONE = "done"
    FAILED = "failed"
    STUCK = "stuck"


# ------------------------------------------------------------ caps -------------
# Q1: hard caps low enough that supervision stays materially different from raw-code review.
DIGEST_TOKEN_CAP = 2048       # per worker-turn digest (total)
DIFF_TOKEN_SUBCAP = 512       # the unified-diff field within a digest
CONTEXT_TOTAL_CAP = 16000     # whole supervisor context
WINDOW_K = 3                  # sliding window: last K digests kept verbatim
FOLD_TOKEN_CAP = 512          # the folded one-line summary of evicted turns
ESCALATION_DOMINATED_FRAC = 0.40
_TOK = 4                      # ~chars per token (deterministic estimate; no tokenizer dep)


def est_tokens(s: str) -> int:
    """Deterministic token estimate (chars/4) — no external tokenizer, so CI is hermetic."""
    return (len(s) + _TOK - 1) // _TOK


# ------------------------------------------------------------ leak guard -------
# Q1: worker text/diffs are untrusted. Reject file-like/raw-source payloads and neutralize
# tool-call-looking syntax so digest content can never invoke a supervisor tool or smuggle code.
_SOURCE_MARKERS = (
    re.compile(r"^\s*(?:def|class)\s+\w+", re.M),
    re.compile(r"^\s*(?:import|from)\s+\w+", re.M),
    re.compile(r"^\s*#include\b", re.M),
    re.compile(r"</?\w+[^>]*>"),                      # markup / xml-ish
)
_TOOLCALL_RE = re.compile(r"<\s*(?:tool_use|tool_call|function_call|invoke)\b", re.I)


def looks_like_source(text: str, *, min_hits: int = 2) -> bool:
    """True if `text` looks like raw source (outside a unified-diff frame)."""
    hits = sum(1 for r in _SOURCE_MARKERS if r.search(text))
    return hits >= min_hits


def neutralize_toolcalls(text: str) -> str:
    """Defang tool-call-looking syntax so digest content can't be interpreted as a tool call."""
    return _TOOLCALL_RE.sub("<neutralized-", text)


# ------------------------------------------------------------ digest -----------
@dataclass
class WorkerTurn:
    """The raw per-turn signal a worker emits (input to digest construction)."""
    worker_id: str
    turn_index: int
    state: WorkerState
    action_label: str
    unified_diff: str
    verify: VerifyEnum
    rationale: str = ""
    files_touched: tuple = ()
    added: int = 0
    deleted: int = 0


@dataclass
class Digest:
    """The bounded, leak-guarded product the supervisor is allowed to see. NEVER raw code."""
    worker_id: str
    turn_index: int
    state: str
    action_label: str
    verify: str
    diff: str
    files_touched: tuple
    added: int
    deleted: int
    truncated: bool
    rejected_fields: tuple = ()

    def tokens(self) -> int:
        return est_tokens(self.diff) + est_tokens(self.action_label) + est_tokens(str(self.files_touched)) + 8


def _cap_diff(diff: str) -> tuple[str, bool]:
    """Cap the diff to DIFF_TOKEN_SUBCAP with a deterministic truncation marker."""
    max_chars = DIFF_TOKEN_SUBCAP * _TOK
    if len(diff) <= max_chars:
        return diff, False
    return diff[:max_chars] + "\n... [AIMEE_DIGEST_TRUNCATED]\n", True


def build_digest(turn: WorkerTurn) -> Digest:
    """Construct a hard-capped, leak-guarded digest from a worker turn (Q1). A rationale that
    looks like raw source is DROPPED (recorded in rejected_fields); the diff is capped; tool-call
    syntax is neutralized. The result is guaranteed <= DIGEST_TOKEN_CAP."""
    rejected = []
    diff, tdiff = _cap_diff(turn.unified_diff)
    diff = neutralize_toolcalls(diff)
    # The action_label/rationale are metadata; if the rationale smuggles source, drop it.
    if turn.rationale and looks_like_source(turn.rationale):
        rejected.append("rationale")
    action = neutralize_toolcalls(turn.action_label)[:200]
    d = Digest(worker_id=turn.worker_id, turn_index=turn.turn_index, state=turn.state.value,
               action_label=action, verify=turn.verify.value, diff=diff,
               files_touched=tuple(turn.files_touched), added=turn.added, deleted=turn.deleted,
               truncated=tdiff, rejected_fields=tuple(rejected))
    # Final hard cap: if still over budget, truncate the diff further, deterministically.
    while d.tokens() > DIGEST_TOKEN_CAP and d.diff:
        d.diff = d.diff[: max(0, len(d.diff) - 512)] + "\n... [AIMEE_DIGEST_TRUNCATED]\n"
        d.truncated = True
    return d


def serialize_supervisor_frame(digests: list[Digest]) -> tuple[str, int]:
    """Render the digests the supervisor sees, running a final source-leak gate over the whole
    frame. Returns (frame_text, rejected_count). Any digest whose diff STILL trips the source
    guard after capping is replaced with a metadata-only stub (belt-and-suspenders)."""
    rejected = 0
    parts = []
    for d in digests:
        body = d.diff
        # A capped unified diff is allowed; but if it reads as bare source (no diff frame), stub it.
        if body and looks_like_source(body) and not body.lstrip().startswith(("diff --git", "---", "+++", "@@")):
            body = "[REJECTED: non-diff source-like content]"
            rejected += 1
        parts.append(f"[{d.worker_id} t{d.turn_index} {d.state}/{d.verify} "
                     f"+{d.added}-{d.deleted} {d.action_label}]\n{body}")
    return "\n\n".join(parts), rejected


# ------------------------------------------------------------ selection --------
@dataclass
class Candidate:
    worker_id: str
    diff: str
    verify: VerifyEnum
    turns: int

    @property
    def diff_size(self) -> int:
        return self.diff.count("\n")

    @property
    def is_valid(self) -> bool:
        return bool(self.diff.strip()) and self.verify != VerifyEnum.ERROR


def select_best_of_n(candidates: list[Candidate]) -> Candidate | None:
    """Q3: a DETERMINISTIC pure selector (no LLM -> reproducible). Tie-break order:
    (1) prefer verify PASS, (2) fewer turns, (3) smaller diff, (4) worker_id lexicographic.
    Returns None (no-op empty patch) when no valid candidate exists."""
    valid = [c for c in candidates if c.is_valid]
    if not valid:
        return None
    return min(valid, key=lambda c: (0 if c.verify == VerifyEnum.PASS else 1,
                                     c.turns, c.diff_size, c.worker_id))


# selection-skill decomposition (Q3): oracle vs actual, over official-grader outcomes.
def oracle_pass(candidate_resolved: dict[str, bool]) -> bool:
    """oracle-best-of-N: success if ANY candidate patch passes the official grader."""
    return any(candidate_resolved.values())


def actual_pass(selected_worker: str | None, candidate_resolved: dict[str, bool]) -> bool:
    """actual-best-of-N: the supervisor-SELECTED patch passes the official grader."""
    return bool(selected_worker) and candidate_resolved.get(selected_worker, False)


def selection_skill_ratio(actual_rate: float, oracle_rate: float) -> float | None:
    """How much of the achievable (oracle) resolution the deterministic selector captured."""
    return round(actual_rate / oracle_rate, 4) if oracle_rate else None


# ------------------------------------------------------------ escalation -------
class EscalationTrigger(str, Enum):
    NONE = "none"
    ALL_WORKERS_FAILED = "all_workers_failed"
    STUCK = "stuck"


@dataclass
class EscalationState:
    """Q2: gated, bounded, separately-attributed escalation. Tokens spent while escalated are
    tracked so a run that leans on escalation (primary digging into code) is caught, not hidden."""
    stuck_turns: int = 0
    stuck_limit: int = 3
    escalation_tokens: int = 0
    escalations: int = 0

    def should_escalate(self, *, all_failed: bool, made_progress: bool) -> EscalationTrigger:
        if made_progress:
            self.stuck_turns = 0
        else:
            self.stuck_turns += 1
        if all_failed:
            return EscalationTrigger.ALL_WORKERS_FAILED
        if self.stuck_turns >= self.stuck_limit:
            return EscalationTrigger.STUCK
        return EscalationTrigger.NONE

    def record_escalation(self, tokens: int) -> None:
        self.escalations += 1
        self.escalation_tokens += max(0, tokens)


def is_escalation_dominated(escalation_tokens: int, primary_input_tokens: int) -> bool:
    """Q2/R3: >40% of the primary's input from escalation => excluded from the headline."""
    if primary_input_tokens <= 0:
        return escalation_tokens > 0
    return escalation_tokens / primary_input_tokens > ESCALATION_DOMINATED_FRAC


# ------------------------------------------------------------ context window ---
@dataclass
class ContextWindow:
    """Q4: sliding window of the last K digests + a deterministic fold-then-evict of older turns
    to a one-line summary (worker set, files, terminal states, counts — NO quoted content), under
    a hard total cap. Exposes the per-turn token curve so drift can't hide savings."""
    k: int = WINDOW_K
    total_cap: int = CONTEXT_TOTAL_CAP
    _recent: list = field(default_factory=list)
    _fold_workers: set = field(default_factory=set)
    _fold_files: set = field(default_factory=set)
    _fold_turns: int = 0
    _fold_pass: int = 0
    _fold_fail: int = 0
    token_curve: list = field(default_factory=list)

    def add(self, d: Digest) -> None:
        self._recent.append(d)
        while len(self._recent) > self.k:
            self._evict(self._recent.pop(0))
        self.token_curve.append(self.tokens())

    def _evict(self, d: Digest) -> None:
        self._fold_workers.add(d.worker_id)
        self._fold_files.update(d.files_touched)
        self._fold_turns += 1
        if d.verify == VerifyEnum.PASS.value:
            self._fold_pass += 1
        elif d.verify == VerifyEnum.FAIL.value:
            self._fold_fail += 1

    def fold_summary(self) -> str:
        if not self._fold_turns:
            return ""
        s = (f"[folded {self._fold_turns} turns | workers={sorted(self._fold_workers)} | "
             f"files={sorted(self._fold_files)} | pass={self._fold_pass} fail={self._fold_fail}]")
        return s[: FOLD_TOKEN_CAP * _TOK]

    def tokens(self) -> int:
        return est_tokens(self.fold_summary()) + sum(d.tokens() for d in self._recent)

    def within_cap(self) -> bool:
        return self.tokens() <= self.total_cap


# ------------------------------------------------------------ redirect ---------
@dataclass
class RedirectBudget:
    """Q5: default OFF. If enabled, bounded (<=1 per worker) and small (<=cap tokens), classified
    through the same code-free guard, and attributed. Redirect tokens are PRIMARY tokens."""
    enabled: bool = False
    per_worker_max: int = 1
    token_cap: int = 256
    _used: dict = field(default_factory=dict)
    redirect_tokens: int = 0

    def allow(self, worker_id: str) -> bool:
        return self.enabled and self._used.get(worker_id, 0) < self.per_worker_max

    def record(self, worker_id: str, text: str) -> str:
        """Record a redirect (capped, defanged); returns the sanitized message."""
        msg = neutralize_toolcalls(text)[: self.token_cap * _TOK]
        self._used[worker_id] = self._used.get(worker_id, 0) + 1
        self.redirect_tokens += est_tokens(msg)
        return msg


# ------------------------------------------------------------ tool allowlist ---
ARM_C_TOOL_ALLOWLIST = frozenset({
    "delegate", "select_best_of_n", "log_escalation", "gated_escalate",
    "terminate_worker", "inspect_digest_metadata", "summarize_fold", "redirect",
})
# code-reading tools are NOT here; they are only reachable via gated_escalate.
_CODE_TOOLS = frozenset({"read_file", "bash", "grep", "cat", "open", "edit_file"})


def tool_allowed(tool: str, *, escalated: bool = False) -> bool:
    """A code-reading tool is permitted ONLY while an escalation is active (Q2)."""
    if tool in ARM_C_TOOL_ALLOWLIST:
        return True
    if tool in _CODE_TOOLS:
        return escalated
    return False


# ------------------------------------------------------------ supervisor turn --
# Option A (roundtable 2026-07-08): the supervisor is dispatched as a delegate with TOOLS OFF, so
# "no raw code" is a STRUCTURAL guarantee (no tool loop) rather than an allowlist. Its input is the
# capped, leak-guarded digest frame; its tokens ARE the arm-C primary headline. The actual submitted
# patch is chosen by the PURE, deterministic select_best_of_n() (reproducible) — the LLM turn's
# stated preference is parsed and logged but never overrides the deterministic selection.
def supervisor_prompt(instance: dict, frame: str, candidate_ids: list[str]) -> str:
    problem = instance.get("problem") or instance.get("problem_statement", "")
    ids = ", ".join(candidate_ids) or "(none)"
    return ("You are supervising a team of engineers fixing a bug. You may read ONLY the capped "
            "progress digests below (worker id, state, verify result, and a short candidate diff) "
            "— you have NO access to the source. Decide which candidate best resolves the issue.\n\n"
            f"## Issue\n{problem}\n\n## Worker digests\n{frame}\n\n"
            f"Candidate worker ids: {ids}\n\nRespond with ONLY a JSON object: "
            '{"action":"select","worker_id":"<id>","rationale":"<=15 words"} '
            '(or {"action":"escalate","rationale":"..."} if no candidate is viable).')


def parse_supervisor_decision(text: str) -> dict:
    """Parse the supervisor's JSON decision (PURE). Falls back to {'action':'select','worker_id':None}
    on malformed output (H3) so a parse failure degrades to the deterministic selector, never a crash."""
    if text:
        m = re.search(r"\{.*\}", text, re.DOTALL)
        if m:
            try:
                d = json.loads(m.group(0))
                if isinstance(d, dict) and d.get("action") in ("select", "escalate", "redirect"):
                    return {"action": d["action"], "worker_id": d.get("worker_id"),
                            "rationale": str(d.get("rationale", ""))[:120]}
            except Exception:
                pass
    return {"action": "select", "worker_id": None, "rationale": "parse_error"}


def _pick_workers(pool: list[str], n: int, seed: str) -> list[str]:
    """N DISTINCT workers for one instance, seeded for reproducibility (matches PR #986 M6)."""
    rng = random.Random(seed)
    p = list(pool)
    rng.shuffle(p)
    return p[:min(n, len(p))]


def _worker_turn(res, worker_id: str) -> WorkerTurn:
    """Build a WorkerTurn from an agentic result. verify=NOT_RUN: in-loop tests default OFF (Q1);
    the OFFICIAL grader is the sole resolution source (run later), so the digest never claims a
    verify it did not run."""
    state = WorkerState.DONE if (res.status == "done" and res.patch.strip()) else WorkerState.FAILED
    added = sum(1 for ln in res.patch.splitlines() if ln.startswith("+") and not ln.startswith("+++"))
    deleted = sum(1 for ln in res.patch.splitlines() if ln.startswith("-") and not ln.startswith("---"))
    return WorkerTurn(worker_id=worker_id, turn_index=0, state=state, action_label=f"agentic loop ({res.api_calls} calls)",
                      unified_diff=res.patch, verify=VerifyEnum.NOT_RUN, added=added, deleted=deleted)


# ------------------------------------------------------------ live arm C -------
def run_arm_c_supervised(instance: dict, *, workers: list[str], n: int, allocator, budget,
                         token_db: str = "", base_repo: str | None = None, primary_agent: str = "codex",
                         primary_model: str = "gpt-5.5", seed: int = 1729, redirect: bool = False,
                         aimee_bin: str = "aimee", dispatch=None, max_concurrency: int = 8) -> dict:
    """Live arm-C supervision run for one instance (Option A). Dispatches N tools-enabled worker
    agentic loops CONCURRENTLY (each in its own worktree — the fleet-parallelism that answers
    Reddit's serial-worker wall-clock), builds capped leak-guarded digests, runs ONE tools-OFF
    supervisor turn over the digest frame (its tokens = the primary headline), and selects the
    submitted patch DETERMINISTICALLY with select_best_of_n. Worker tokens are read separately and
    priced $0. `dispatch` is injected so CI drives a fake fleet; `resolved` is set later by S5."""
    from benchmarks.coding import swebench_agentic_harness as H
    from benchmarks.coding import swebench_arm_runner as R
    from benchmarks.coding import swebench_live_transport as T
    from benchmarks.coding.supervision_budget import Supervisor as _BudgetSup
    if _FAKE:
        return _fake_arm_c_record(instance, primary_model, n)
    if dispatch is None:
        dispatch = T.dispatch_and_wait

    # S3 budget supervisor: audit trail for primary tokens, escalation cost, and
    # tool allowlist enforcement. Wired here so the headline telemetry curve
    # (``primary_tokens_by_turn``) is computed in one canonical place.
    _budget = _BudgetSup()

    instance_id = instance["instance_id"]
    picks = _pick_workers(workers, n, f"{seed}:{instance_id}")
    t0 = time.perf_counter()

    def _run_worker(idx_worker):
        idx, w = idx_worker
        env = allocator.allocate(instance_id, "C", idx)
        return w, H.run_agentic_loop(instance, env, budget, arm="C", worker=w, base_repo=base_repo,
                                     token_db=token_db, worker_idx=idx, aimee_bin=aimee_bin,
                                     dispatch=dispatch)

    with ThreadPoolExecutor(max_workers=min(max_concurrency, max(1, len(picks)))) as ex:
        worker_results = list(ex.map(_run_worker, list(enumerate(picks))))
    first_work = t0  # workers dispatched immediately; queue folded into work for a single round

    # Candidates + leak-guarded digests (the ONLY thing the supervisor sees).
    candidates, digests, worker_jobs, redactions, sources = [], [], [], 0, {}
    for w, res in worker_results:
        redactions += res.redactions
        sources[w] = res.patch_source
        if res.job_id is not None:
            worker_jobs.append(res.job_id)
        if res.patch.strip():
            candidates.append(Candidate(worker_id=w, diff=res.patch, verify=VerifyEnum.NOT_RUN,
                                        turns=res.api_calls))
        digests.append(build_digest(_worker_turn(res, w)))
    frame, _frame_rejected = serialize_supervisor_frame(digests)
    # S3 budget: enforce the whole-frame primary-context cap (16k) on what the supervisor
    # actually sees. build_digest already caps each worker turn to DIGEST_TOKEN_CAP (2048);
    # cap_digest is the frame-level ceiling that truncates + appends a labeled overflow note
    # so an over-cap frame cannot silently exhaust the primary context.
    frame, _frame_orig_tokens, _frame_tokens = _budget.cap_digest(frame, caller="run_arm_c_supervised", kind="frame")

    # Deterministic submitted patch (reproducible authority).
    best = select_best_of_n(candidates)

    # Supervisor turn: tools OFF, digests-only -> the primary headline spend.
    sup = dispatch("review", supervisor_prompt(instance, frame, [c.worker_id for c in candidates]),
                   via=primary_agent, tools=False, token_db=token_db, aimee_bin=aimee_bin)
    decision = parse_supervisor_decision(sup.result)
    t1 = time.perf_counter()

    # H1 runtime assertion: the tools-OFF supervisor must have ZERO tool rows (structural no-code).
    sup_tool_rows = T.supervisor_tool_call_rows(token_db, sup.delegation_id) if sup.delegation_id else -1

    tok = R.primary_tokens_by_jobs(token_db, primary_model, [sup.job_id],
                                   delegation_ids=[sup.delegation_id])
    # S3 budget telemetry (F4): emit ONE point per primary API/model turn, not one
    # aggregate for the entire job. The ledger's ``primary_tokens_by_turn`` is the
    # running total at end-of-turn so a multi-turn supervisor with N turns yields N
    # points. Earlier this call recorded a single aggregate ``tok.total_headline``
    # which collapsed a 5-turn supervisor into one point and made context drift
    # undetectable.
    #
    # The escalation turn (which is itself a tools-enabled primary call) is also
    # recorded as its own point so the auditor can see how much primary context
    # the escalation cost.
    # F4: per-row primary curve must be emitted for every primary turn. The
    # fallback below only fires when no per-row data could be extracted, AND
    # never after partial success (which would double-count on top of rows
    # already appended by the loop).
    # F4: per-row primary curve must be emitted for every primary turn. The
    # aggregate fallback below only fires when no per-row data could be
    # extracted AND never after partial success (which would double-count on
    # top of rows already appended by the loop).
    #
    # Failure-mode policy:
    #   - Success: appended_rows == len(prim_rows); emit a complete per-turn curve.
    #   - Mid-loop failure: appended_rows < len(prim_rows) but > 0; the partial
    #     curve is kept on the ledger as-is and a warning is raised. We do NOT
    #     fire the aggregate fallback because that would mix per-turn points
    #     with an aggregate number on the same curve (exactly the corruption
    #     the security reviewer flagged). Consumers see ``primary_curve_partial``
    #     in the run record.
    #   - Pre-loop failure or empty rows: appended_rows == 0; fall back to a
    #     SINGLE aggregate point tagged ``aggregate_fallback`` so the curve is
    #     never empty. This is the only honest representation we have in that
    #     case.
    prim_rows = []
    appended_rows = 0
    try:
        from benchmarks.coding.swebench_transport_verify import _primary_rows as _V_primary_rows
        prim_rows = _V_primary_rows(rows, primary_model) if rows else []
    except Exception:
        # Extraction itself failed before any rows were appended - leave
        # appended_rows at 0 so the aggregate-fallback block can fire below.
        prim_rows = []
    # Round-2 reviewer fix (qa+architect): when the per-row extractor returned
    # no rows (FAKE / synthetic / pre-loop failure), synthesize one row per
    # reported primary turn so the per-turn curve is not empty. Each synthetic
    # row carries the same ``total_headline`` slice (deterministic, so the
    # running total equals the aggregate). Synthetic rows are tagged so the
    # auditor can distinguish them from real per-dispatch data.
    if not prim_rows and tok.turns and tok.total_headline:
        per_turn = max(1, tok.total_headline // tok.turns)
        prim_rows = [
            {"prompt_tokens": 0, "completion_tokens": per_turn,
             "cache_read_tokens": 0, "_synthetic_from_aggregate": True}
            for _ in range(int(tok.turns))
        ]
    for i, r in enumerate(prim_rows):
        headline = (int(r.get("prompt_tokens", 0)) + int(r.get("completion_tokens", 0))
                    - int(r.get("cache_read_tokens", 0)))
        # F5: try_record_turn records a rejection on the ledger instead of
        # raising, so a run that exhausts primary_budget keeps going and the
        # auditor sees explicit primary_rejections rows. The observed value is
        # always recorded (in primary_tokens_observed_by_turn) regardless of
        # acceptance so drift detection sees per-turn shape even on rejection.
        try:
            accepted, _ = _budget.try_record_turn(
                appended_rows, headline, caller="run_arm_c_supervised")
            appended_rows += 1
        except Exception:
            # Per-row append failed mid-loop. The rows we DID append are
            # already on the ledger; do NOT reset appended_rows here (doing
            # so would let the aggregate fallback fire on top of the partial
            # curve and double-count). Stop the loop and surface the failure.
            break
    if appended_rows < len(prim_rows):
        # Curve is partial - record that fact so consumers can distinguish
        # a partial per-turn curve from an aggregate fallback curve.
        try:
            _budget.record_partial_curve_warning(
                appended=appended_rows, total=len(prim_rows))
        except AttributeError:
            # Back-compat: ledger predates partial-curve warning support.
            pass
    # Aggregate fallback fires ONLY when no per-row data was appended at all.
    # If appended_rows > 0 the partial curve is the honest representation;
    # mixing in tok.total_headline on top would corrupt the per-turn curve.
    # Always emit the aggregate point (even with ``total_headline=0``) so the
    # ledger carries a primary-turn marker for the supervisor's own dispatch
    # - this is what consumers like ``run_record_carries_ledger_telemetry``
    # rely on to confirm the supervisor turn was recorded at all.
    if appended_rows == 0:
        _budget.try_record_turn(
            0, tok.total_headline, caller="run_arm_c_supervised_aggregate_fallback")
    worker_dids = [res.delegation_id for _, res in worker_results if res.delegation_id]
    if worker_dids:
        wi, wc, wo, _ = T.read_realized_by_delegations(token_db, worker_dids)
        worker_in, worker_out = wi + wc, wo
    else:
        worker_in, worker_out = T.read_worker_tokens_by_jobs(token_db, worker_jobs)
    esc = EscalationState()
    # Gated escalation (R3): only when NO worker produced a candidate. A separate, bounded,
    # attributed read-enabled dispatch; its output is re-digested before it could reach the frame.
    if not candidates and base_repo:
        trig = esc.should_escalate(all_failed=True, made_progress=False)
        if trig != EscalationTrigger.NONE and esc.escalations < 3:
            # S3 budget: escalation is the ONLY path that unlocks code-reading tools. Open
            # the gate (charged + caller-attributed in the ledger) before the tools-ON
            # dispatch, and assert the allowlist now permits read_file/bash/grep. Closed in
            # `finally` so the gate can never leak past this bounded escalation.
            _budget.gate.open_escalation(caller=f"{primary_agent}@arm_c",
                                         reason=f"no candidate for {instance_id}")
            try:
                _budget.check_tool_dispatch("read_file")
                _budget.check_tool_dispatch("bash")
                _budget.check_tool_dispatch("grep")
                branch = H.worktree_branch(instance_id, "C-esc", 0)
                e = dispatch("code", H.agentic_prompt(instance, arm="C"), via=primary_agent, tools=True,
                             worktree=branch, token_db=token_db, aimee_bin=aimee_bin,
                             max_turns=budget.max_turns, timeout_ms=int(budget.max_wall_s * 1000))
                e_in, _ = T.read_worker_tokens_by_jobs(token_db, [e.job_id])
                esc.record_escalation(e_in)
                epatch = H.scan_and_redact_secrets(H.extract_diff_from_text(e.result))[0]
                if epatch.strip():
                    best = Candidate(worker_id=f"{primary_agent}@escalate", diff=epatch,
                                     verify=VerifyEnum.NOT_RUN, turns=e.api_calls)
                    sources[f"{primary_agent}@escalate"] = "response_text"
            finally:
                # The double-charge bug fix in ``close_escalation`` means we now
                # only mutate the open record's outcome, so passing the actual
                # dispatch result is what makes the audit honest: ``granted``
                # reflects whether the escalation actually produced a candidate
                # patch, not whether the gate was merely closed.
                _budget.gate.close_escalation(
                    caller=f"{primary_agent}@arm_c",
                    actual_used=best is not None,
                    detail=("produced_candidate" if best is not None
                            else "no_candidate"),
                )

    wall = R.WallClock(t0=t0, first_work=first_work, t1=t1)
    escalation_dominated = is_escalation_dominated(esc.escalation_tokens, tok.input_uncached)
    rec = R.build_arm_record(
        instance_id, "C", primary_model, tokens=tok, worker_in=worker_in, worker_out=worker_out,
        wall=wall, resolved=None, patch=best.diff if best else "",
        base_commit=instance.get("base_commit", ""), repo=instance.get("repo", ""),
        n_workers=len(picks), redactions=redactions, escalations=esc.escalations,
        invalid=(best is None) or escalation_dominated)
    rec.update({
        "patch": best.diff if best else "",   # secret-redacted patch for the official grader
        "patch_source": sources.get(best.worker_id, "none") if best else "none",
        "n_candidates": len(candidates),
        "selected_worker": best.worker_id if best else None,
        "supervisor_job_id": sup.job_id,
        "supervisor_decision": decision["action"],
        "supervisor_tool_rows": sup_tool_rows,   # MUST be 0 (or -1 if db unreadable) — H1 gate
        "escalation_tokens": esc.escalation_tokens,
        "escalation_dominated": escalation_dominated,
        "candidate_health_ok": len(candidates) >= -(-n // 2),  # >= ceil(n/2)
        # S3 budget telemetry: emit the supervisor's per-turn primary token curve
        # plus the audit ledger so the auditor can reproduce the headline.
        # F4 (round-2 reviewer fix): the observed per-turn series is emitted
        # alongside the running-total curve so a refused peak that the
        # running-total cannot advance is still visible to the consumer /
        # drift analysis. Both series are length-aligned (same number of
        # try_record_turn calls).
        "primary_tokens_by_turn": _budget.primary_tokens_by_turn,
        "primary_tokens_observed_by_turn": _budget.ledger.primary_tokens_observed_by_turn,
        "primary_tokens_total": _budget.ledger.primary_tokens_total,
        "supervision_budget_remaining": _budget.ledger.balance,
        # F4 (round-2 security review fix): the per-turn curve may be partial
        # (per-row try_record_turn succeeded for some rows then failed). When
        # that happens, this list gets an entry so consumers can tell partial
        # curves from aggregate-fallback curves. Consumers MUST check this
        # before trusting ``primary_tokens_by_turn`` for completeness.
        "primary_curve_warnings": list(_budget.ledger.partial_curve_warnings),
        "primary_curve_complete": (not _budget.ledger.partial_curve_warnings),
        # Caller-attributable audit ledger. Until this revision the ledger was
        # dropped when ``_budget`` went out of scope at function return, so the
        # caller attribution and per-attempt outcome records vanished. Emit
        # them now as plain dataclass dicts: any reviewer can replay the
        # charge/escalation timeline from this without re-running the harness.
        "budget_entries": [
            {"source": e.source, "tokens": e.tokens, "at": e.at}
            for e in _budget.ledger.entries
        ],
        "budget_escalations": [
            {
                "caller": er.caller, "reason": er.reason,
                "cost_tokens": er.cost_tokens, "granted": er.granted,
                "detail": er.detail, "at": er.at,
            }
            for er in _budget.ledger.escalations
        ],
        # Drift findings (monotonicity violations, rapid growth). Empty list
        # means the run was well-behaved; non-empty means the auditor should
        # inspect the ledger entries above.
        "budget_drift_findings": _budget.ledger.detect_drift(),
        # F1: frame-level digest cap audit trail. One row per cap_digest()
        # call so a truncated digest is never silently indistinguishable from
        # a clean one. ``truncated=True`` rows are the ones the auditor should
        # inspect for context loss.
        "budget_cap_events": [
            {k: v for k, v in ev.items()}
            for ev in _budget.ledger.cap_events
        ],
        # F5: refused primary spend. A run that runs out of primary_budget
        # records each refused turn here so the auditor can see "we tried to
        # spend N tokens but the budget was at primary_spent=N-1 and rejected".
        "primary_rejections": list(_budget.ledger.primary_rejections),
    })
    return rec


def _fake_arm_c_record(instance: dict, primary_model: str, n: int) -> dict:
    from benchmarks.coding import swebench_arm_runner as R
    from benchmarks.coding import swebench_transport_verify as V
    iid = instance["instance_id"]
    rows = V._fake_rows("pass", primary_model, "glm-5.2")
    tok = R.primary_token_totals(rows, primary_model)
    wall = R.WallClock(t0=0.0, first_work=0.4, t1=8.0)
    rec = R.build_arm_record(iid, "C", primary_model, tokens=tok, worker_in=1200, worker_out=5000,
                             wall=wall, resolved=True, patch="diff --git a/x b/x\n+ok\n",
                             base_commit=instance.get("base_commit", "0" * 40),
                             repo=instance.get("repo", "x/y"), n_workers=n)
    rec.update({"patch": "diff --git a/x b/x\n+ok\n", "n_candidates": n,
                "selected_worker": "glm-5.2", "supervisor_job_id": 1, "supervisor_decision": "select",
                "supervisor_tool_rows": 0, "escalation_tokens": 0, "escalation_dominated": False,
                "candidate_health_ok": True})
    return rec
