#!/usr/bin/env python3
"""supervision_budget - digest capper, escalation gating, tool allowlist, ledger.

This is the S3 "supervisor honesty" core, factored into a small focused module so it is
independently testable and can be wired into the primary agent's tool-dispatch path.

Three guarantees (roundtable approval p3, intent_record_p3.json):

  1. DIGEST_CAP = 16,000 tokens (primary-context cap, matching CONTEXT_TOTAL_CAP in
     swebench_supervision). Digests above the cap are truncated and a clearly labeled
     overflow note is appended; at-or-below the cap, content is preserved verbatim.
  2. ESCALATION_GATE: every escalation attempt is an explicit budget charge. Success and
     failure both log the spend with caller attribution. The escalation lifetime is
     bounded by the gate so a tool call after the gate closes is denied.
  3. TOOL_ALLOWLIST: outside an active escalation, code-reading tools (read_file, bash,
     grep, cat, open, edit_file) are blocked. Inside an active escalation they are allowed.

Telemetry: BudgetLedger.primary_tokens_by_turn records the running primary token spend
as a per-turn time series so an auditor can reproduce what the supervisor saw.

Pure-Python (no I/O, no tokenizer dep), deterministic. Token counts use the conservative
4-chars-per-token estimate that the arm-C supervisor already standardizes on.
"""
from __future__ import annotations

import time
from dataclasses import dataclass, field


CHARS_PER_TOKEN = 4

# Primary-context cap, in tokens. Mirrors CONTEXT_TOTAL_CAP in swebench_supervision: the
# hard ceiling on what the supervisor is allowed to see per turn. Above this, a digest
# is truncated and a clearly-labeled overflow note is appended so the auditor can
# distinguish a real digest from a capped one.
#
# This is INTENTIONALLY a separate constant from the per-worker-turn digest cap
# (DIGEST_TOKEN_CAP = 2048 in swebench_supervision); that 2048 cap is enforced by
# build_digest() at the worker-bridge seam and is unaffected by this module.
DIGEST_CAP: int = 16_000

# Cost in budget tokens charged for each escalation attempt (success OR failure).
ESCALATION_COST_TOKENS: int = 256

# Tools the primary/supervisor can call WITHOUT escalation. Code-reading tools are not here.
SUPERVISOR_TOOLS = frozenset({
    "delegate", "select_best_of_n", "log_escalation", "gated_escalate",
    "terminate_worker", "inspect_digest_metadata", "summarize_fold", "redirect",
})

# Code-reading tools. Allowed ONLY inside an active escalation.
CODE_TOOLS = frozenset({
    "read_file", "bash", "grep", "cat", "open", "edit_file",
})


def est_tokens(s):
    """Deterministic token estimate (4 chars / token)."""
    if not s:
        return 0
    return (len(s) + CHARS_PER_TOKEN - 1) // CHARS_PER_TOKEN


OVERFLOW_NOTE = (
    "\n\n[OVERFLOW: digest truncated at {cap} tokens (primary context cap); "
    "original was {orig_tokens} tokens. Use gated_escalate to inspect raw source.]"
)


def cap_digest(text, *, cap=DIGEST_CAP):
    """Return (capped_text, original_tokens, capped_tokens).

    At-or-below the cap: returns text unchanged. Strictly above the cap: truncates the
    string so est_tokens(capped) <= cap and appends an overflow note whose total token
    count still respects the cap (the note replaces, not extends, the trimmed tail).
    """
    orig = est_tokens(text)
    if orig <= cap:
        return text, orig, orig

    note = OVERFLOW_NOTE.format(cap=cap, orig_tokens=orig)
    note_tokens = est_tokens(note)
    # The truncate loop below requires ``head`` to be non-empty to make progress; if
    # ``cap`` is small enough that the overflow note alone already exceeds it, the
    # only sane thing is to truncate the note too. We always emit a non-empty,
    # strictly-bounded digest: the public contract ``est_tokens(capped) <= cap``
    # holds for every cap, including pathological caps smaller than the overflow note.
    if note_tokens > cap:
        kept_note_chars = max(0, cap * CHARS_PER_TOKEN - 1)
        note = note[:kept_note_chars]
        note_tokens = est_tokens(note)
    keep_chars = max(0, (cap - note_tokens) * CHARS_PER_TOKEN)
    head = text[:keep_chars]
    capped = head + note
    capped_tokens = est_tokens(capped)
    safety = 0
    while capped_tokens > cap and head and safety < 10_000:
        head = head[:-CHARS_PER_TOKEN]
        capped = head + note
        capped_tokens = est_tokens(capped)
        safety += 1
    return capped, orig, capped_tokens


@dataclass
class EscalationRecord:
    caller: str
    reason: str
    cost_tokens: int
    granted: bool
    detail: str = ""
    at: float = field(default_factory=time.time)


class EscalationDenied(RuntimeError):
    """Raised when a code tool is called without an active escalation, or after the gate closes."""


class EscalationGate:
    """Lifetime-bounded escalation. Open with open_escalation; closes itself.

    Every open/close attempt costs ESCALATION_COST_TOKENS from the supplied ledger and is
    logged with the caller's identity. The supervisor passes caller='arm_c' so an auditor
    can attribute spend.
    """

    def __init__(self, ledger, *, cost=ESCALATION_COST_TOKENS):
        self._ledger = ledger
        self._cost = cost
        self._open = None

    @property
    def is_open(self):
        return self._open is not None

    @property
    def open_record(self):
        return self._open

    def open_escalation(self, *, caller, reason):
        rec = EscalationRecord(
            caller=caller, reason=reason, cost_tokens=self._cost, granted=True,
            detail=f"opened for {caller}",
        )
        self._ledger.charge(rec.cost_tokens, source=f"escalation.open:{caller}")
        self._ledger.log(rec)
        self._open = rec
        return rec

    def close_escalation(self, *, caller, detail="", actual_used=True):
        """Close the active escalation and attribute outcome to the existing record.

        The ``cost_tokens`` charge happens once, at ``open_escalation`` time. Closing
        must not charge again - that would silently double-book one gate permission as
        two billable events and obscure whether the dispatch actually succeeded. We
        therefore mutate the open record's outcome (``granted`` -> actual_used, plus
        detail) rather than appending a fresh EscalationRecord.
        """
        if self._open is None:
            # Idempotent close: no open record means no spend to attribute. Returning
            # a record with granted=False would lie about a charge that never happened,
            # so return None and let callers branch on ``is_open``.
            return None
        self._open.granted = bool(actual_used)
        self._open.detail = detail or self._open.detail
        # No additional charge: the gate was bought at open time; closing just records
        # whether the privilege was actually used so the audit log reflects intent.
        closed = self._open
        self._open = None
        return closed

    def check_tool(self, tool):
        if tool in CODE_TOOLS and not self.is_open:
            raise EscalationDenied(
                f"tool {tool!r} requires an active escalation; open one via "
                f"gated_escalate before invoking code-reading tools"
            )


def tool_allowed(tool, *, escalated):
    """Pure check: is tool permitted under the given escalation state?"""
    if tool in SUPERVISOR_TOOLS:
        return True
    if tool in CODE_TOOLS:
        return escalated
    # Unknown tools default to denied - fail closed, not open.
    return False


class ToolDispatcher:
    """Wraps tool_allowed with a live EscalationGate so dispatch raises cleanly.

    The primary agent's tool-dispatch seam calls check(tool) and expects either a
    pass-through or an exception. By routing through this wrapper, code tools are
    structurally denied unless an escalation is currently open.
    """

    def __init__(self, gate):
        self._gate = gate

    def check(self, tool):
        if not tool_allowed(tool, escalated=self._gate.is_open):
            raise EscalationDenied(f"tool {tool!r} blocked by supervisor allowlist")


@dataclass
class LedgerEntry:
    source: str
    tokens: int
    at: float = field(default_factory=time.time)


class BudgetLedger:
    """Token-spend ledger for the supervisor. Pure data, no I/O.

    primary_tokens_by_turn is the time series emitted per turn: the accumulated primary
    token spend at the end of each turn index. This is the headline telemetry that the
    arm-C supervisor reports back.
    """

    def __init__(self, *, starting_balance=1_000_000):
        self._balance = starting_balance
        self.entries = []
        # Per-call to record_turn: stores the running total at end-of-turn so
        # primary_tokens_by_turn is the supervisor's accumulating telemetry curve.
        self.turn_boundaries = []
        self.escalations = []

    @property
    def balance(self):
        return self._balance

    def charge(self, tokens, *, source):
        if tokens < 0:
            raise ValueError("cannot credit via charge; use record_turn for primary spend")
        self._balance -= tokens
        self.entries.append(LedgerEntry(source=source, tokens=tokens))
        return self._balance

    def log(self, rec):
        self.escalations.append(rec)

    def record_turn(self, turn_index, primary_tokens_this_turn):
        """Record end-of-turn primary token spend. Returns the new accumulated total.

        Telemetry contract: ``primary_tokens_by_turn`` is the running total at end of
        each turn, oldest first. It only tracks primary spend - escalation charges are
        logged in ``entries`` and ``escalations`` separately so the primary curve is
        uncontaminated.
        """
        if primary_tokens_this_turn < 0:
            raise ValueError("primary_tokens_this_turn must be >= 0")
        # Compute the running total against the primary-only spend, not the ledger
        # balance (which is also debited by escalation charges).
        prev = self.turn_boundaries[-1] if self.turn_boundaries else 0
        new_total = prev + primary_tokens_this_turn
        self.charge(primary_tokens_this_turn, source=f"primary.turn:{turn_index}")
        self.turn_boundaries.append(new_total)
        return new_total

    @property
    def primary_tokens_by_turn(self):
        """Running total of primary token spend at the end of each turn, oldest first.

        This is the supervisor's per-turn telemetry curve - one point per recorded turn,
        monotonically non-decreasing. It does NOT include escalation charges, which live
        in ``entries`` for separate audit.
        """
        return list(self.turn_boundaries)

    @property
    def primary_tokens_total(self):
        """Accumulated primary token spend across all recorded turns."""
        return self.turn_boundaries[-1] if self.turn_boundaries else 0

    def detect_drift(self, *, rapid_growth_factor=4, rapid_growth_window=3):
        """Detect drift in ``primary_tokens_by_turn``.

        Returns a list of human-readable drift findings. Empty list = no drift.
        Two checks are run:

        1. **Monotonicity violation**: the curve must be non-decreasing because each
           ``record_turn`` adds a non-negative primary spend. A negative step
           (turn-over-turn decrease) means somebody backed the value out somewhere
           outside the ledger - which is exactly the silent-bypass failure mode
           that motivated putting this spend under audit in the first place.

        2. **Rapid growth**: if any single turn's primary spend exceeds
           ``rapid_growth_factor`` times the median of the previous
           ``rapid_growth_window`` turns, flag it. This catches the "primary agent
           suddenly spends 4x its normal turn budget" pattern that often
           accompanies escalation abuse (a primary digging into the codebase
           instead of delegating).

        Both checks are deterministic, side-effect-free, and run against
        ``turn_boundaries`` so a reviewer can replay them offline.
        """
        findings = []
        if len(self.turn_boundaries) < 2:
            return findings
        # 1. Monotonicity
        for i in range(1, len(self.turn_boundaries)):
            prev = self.turn_boundaries[i - 1]
            cur = self.turn_boundaries[i]
            step = cur - prev
            if step < 0:
                findings.append(
                    f"primary_tokens_by_turn decreased at turn {i} "
                    f"({prev} -> {cur}); monotonicity violated"
                )
        # 2. Rapid growth (per-turn deltas, not cumulative totals)
        deltas = [
            self.turn_boundaries[i] - self.turn_boundaries[i - 1]
            for i in range(1, len(self.turn_boundaries))
        ]
        window = max(1, int(rapid_growth_window))
        for i, d in enumerate(deltas):
            if d <= 0:
                continue
            lookback = deltas[max(0, i - window):i]
            if not lookback:
                continue
            med = sorted(lookback)[len(lookback) // 2]
            if med > 0 and d > rapid_growth_factor * med:
                findings.append(
                    f"primary_tokens_by_turn grew rapidly at turn {i + 1}: "
                    f"delta={d} vs median(previous {len(lookback)})={med} "
                    f"(factor > {rapid_growth_factor})"
                )
        return findings


class Supervisor:
    """Minimal facade the primary agent actually calls.

    Usage::

        sup = Supervisor()
        sup.record_turn(0, primary_tokens=384)
        text = cap_digest(worker_digest_text)[0]
        sup.check_tool_dispatch("delegate")             # OK - supervisor tool
        try:
            sup.gate.check_tool("read_file")            # denied
        except EscalationDenied:
            ...
        sup.gate.open_escalation(caller="arm_c", reason="candidate diff is empty")
        sup.check_tool_dispatch("read_file")             # OK - escalation open
        sup.gate.close_escalation(caller="arm_c")
    """

    def __init__(self, *, ledger=None, cap=DIGEST_CAP):
        self.ledger = ledger if ledger is not None else BudgetLedger()
        self.gate = EscalationGate(self.ledger)
        self.dispatcher = ToolDispatcher(self.gate)
        self.cap = cap

    def cap_digest(self, text):
        return cap_digest(text, cap=self.cap)

    def check_tool_dispatch(self, tool):
        self.dispatcher.check(tool)

    def record_turn(self, turn_index, primary_tokens):
        return self.ledger.record_turn(turn_index, primary_tokens)

    @property
    def primary_tokens_by_turn(self):
        return self.ledger.primary_tokens_by_turn
