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

import re
from dataclasses import dataclass, field
from enum import Enum

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


# ------------------------------------------------------------ live stub --------
def run_arm_c_supervised(instance: dict, *, workers: list[str], n: int, redirect: bool = False):
    raise NotImplementedError(
        "live arm-C supervision loop not wired: dispatch N tools-enabled workers on isolated "
        "workspaces (S1), build capped leak-guarded digests each turn (build_digest), keep the "
        "supervisor context bounded (ContextWindow), select deterministically (select_best_of_n), "
        "escalate only on the gated triggers, and read primary tokens via the S0 polarity + S2 "
        "arm-runner accounting. The optional LLM-supervisor selection is a separate ablation.")
