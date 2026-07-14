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


def _now():
    """Monotonic timestamp helper for audit timestamps (seconds since epoch)."""
    return time.time()


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

# Hard model-token cap on per-call primary spend the supervisor authorizes.
# Without an explicit floor, a starting balance of 1_000_000 simply lets any
# charge succeed until the balance happens to cross zero - which is what made
# "balance went to -256" possible in earlier iterations. A charge that would
# drive the ledger below this floor is refused.
HARD_MODEL_TOKEN_CAP: int = 1_000_000

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
    used_tokens: int = 0
    detail: str = ""
    at: float = field(default_factory=time.time)


class EscalationDenied(RuntimeError):
    """Raised when a code tool is called without an active escalation, or after the gate closes."""


class PrimaryBudgetExhausted(RuntimeError):
    """Strict-mode refusal of ``record_turn``: per-turn spend would push past primary_budget.

    The live supervisor intentionally does NOT catch this - it uses ``try_record_turn``
    which records the rejection in the ledger. Tests that want the hard error
    should call ``record_turn`` directly.
    """


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
        # F2/F3 auditability: every attempted escalation - granted OR denied -
        # is recorded here so an auditor sees the full attempt history, including
        # denied tool-dispatch attempts that did not flow through
        # open_escalation. Append-only; read by audit.
        self.attempts = []

    @property
    def is_open(self):
        return self._open is not None

    @property
    def open_record(self):
        return self._open

    def open_escalation(self, *, caller, reason, used_tokens=0):
        """Open a new escalation. Charges self._cost from the ledger and binds it to caller.

        The authorization is bound to ``caller``: closing the gate requires that same
        identity, and a second open attempt while a gate is already open is rejected
        so the existing record's audit attribution is never lost.

        ``used_tokens`` is the realized escalation input token spend attributed to this
        authorization (recorded on the EscalationRecord) so a single audit row
        associates the caller, the fixed authorization fee, and the actual escalation
        consumption. It is audit-only - no second ledger charge is performed here,
        because caller-attributable consumption already flows through the pre-existing
        ``record_escalation`` path which credits its own accounting independently.
        """
        if self._open is not None:
            raise EscalationDenied(
                f"escalation already open for {self._open.caller!r}; close it before "
                f"opening a new one (caller={caller!r})"
            )
        if used_tokens < 0:
            raise ValueError("used_tokens must be >= 0")
        rec = EscalationRecord(
            caller=caller, reason=reason, cost_tokens=self._cost, granted=True,
            used_tokens=used_tokens,
            detail=f"opened for {caller}",
        )
        self._ledger.charge(rec.cost_tokens, source=f"escalation.open:{caller}")
        self._ledger.log(rec)
        self._open = rec
        return rec

    def close_escalation(self, *, caller, detail="", actual_used=True, used_tokens=None):
        """Close the active escalation. The caller MUST match the opening caller.

        The ``cost_tokens`` charge happens once, at ``open_escalation`` time. Closing
        must not charge again - that would silently double-book one gate permission as
        two billable events and obscure whether the dispatch actually succeeded. We
        therefore mutate the open record's outcome (``granted`` -> actual_used, plus
        detail, plus final used_tokens if supplied) rather than appending a fresh
        EscalationRecord.

        Caller identity is enforced: the supplied ``caller`` must match the caller
        stored on the open record. A mismatch is rejected so a third party cannot
        close an authorization it did not pay for.
        """
        if self._open is None:
            # Idempotent close: no open record means no spend to attribute. Returning
            # a record with granted=False would lie about a charge that never happened,
            # so return None and let callers branch on ``is_open``.
            return None
        if caller != self._open.caller:
            raise EscalationDenied(
                f"caller {caller!r} cannot close escalation opened by "
                f"{self._open.caller!r}; caller attribution is enforced"
            )
        self._open.granted = bool(actual_used)
        if used_tokens is not None:
            if used_tokens < 0:
                raise ValueError("used_tokens must be >= 0")
            self._open.used_tokens = used_tokens
        self._open.detail = detail or self._open.detail
        # No additional charge: the gate was bought at open time; closing just records
        # whether the privilege was actually used so the audit log reflects intent.
        closed = self._open
        self._open = None
        return closed

    def check_tool(self, tool):
        if tool in CODE_TOOLS and not self.is_open:
            # F2/F3: log the denied attempt before raising so the auditor sees
            # the dispatch denial, not just the exception.
            self.record_denied_tool_attempt(tool)
            raise EscalationDenied(
                f"tool {tool!r} requires an active escalation; open one via "
                f"gated_escalate before invoking code-reading tools"
            )

    def record_denied_tool_attempt(self, tool, *, caller="unknown"):
        """Append a denied-attempt audit row to ``self.attempts``.

        Called when a tool-dispatch is refused because the supervisor allowlist
        forbids the tool (no active escalation, unknown tool, etc). The attempt
        is recorded with ``granted=False`` and a denial_reason so the auditor
        sees every dispatch attempt, including denials.
        """
        self.attempts.append({
            "caller": caller,
            "reason": f"tool_dispatch:{tool}",
            "granted": False,
            "denial_reason": "tool_not_in_allowlist",
            "timestamp": _now(),
            "cost": 0,
        })


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

    def check(self, tool, *, caller="unknown"):
        if not tool_allowed(tool, escalated=self._gate.is_open):
            # F2/F3 auditability: every attempted escalation - granted OR denied -
            # is recorded in the ledger so the auditor can see denied attempts.
            # route the denial through the gate so ``self._gate.attempts`` gets
            # a row before we raise.
            try:
                self._gate.record_denied_tool_attempt(tool, caller=caller)
            except AttributeError:
                # Backwards-compat: if the gate does not yet implement the
                # denied-attempt recorder (older EscalationGate versions), log
                # a best-effort row directly so the ledger still has the audit.
                self._gate.attempts.append({
                    "caller": caller,
                    "reason": f"tool_dispatch:{tool}",
                    "granted": False,
                    "denial_reason": "tool_not_in_allowlist",
                    "timestamp": _now(),
                    "cost": 0,
                })
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

    def __init__(self, *, starting_balance=HARD_MODEL_TOKEN_CAP, primary_budget=None):
        """Build a budget ledger for S3 supervision audit.

        ``starting_balance`` is the per-supervisor-invocation hard model-token
        budget: charges that would drive the balance negative are refused.
        ``primary_budget`` is an independent (smaller) cap on the supervisor's
        primary token spend alone, so escalation consumption cannot quietly
        siphon the entire budget away from the model's headroom. Defaults to
        ``int(0.9 * starting_balance)`` if not provided.
        """
        self._balance = starting_balance
        self._primary_budget = (
            int(0.9 * starting_balance) if primary_budget is None else primary_budget
        )
        self._primary_spent = 0
        self.entries = []
        # Per-call to record_turn: stores the running total at end-of-turn so
        # primary_tokens_by_turn is the supervisor's accumulating telemetry curve.
        self.turn_boundaries = []
        self.escalations = []
        self.per_turn_primary = []  # one entry per record_turn call
        # Frame-level digest cap audit trail (F1: cap result must not be discarded).
        # One record per cap_digest() call so an auditor can see WHEN a digest was
        # truncated, what its original/capped sizes were, and which caller invoked it.
        self.cap_events = []
        # Primary turn rejections (F5): when record_turn refuses a spend that would
        # exceed primary_budget, the rejection is recorded here so a run that
        # exhausts its primary budget emits an explicit audit row instead of
        # aborting with an uncaught RuntimeError. Append-only; read by audit.
        self.primary_rejections = []
        # Per-turn observed primary tokens (F4): one point per try_record_turn
        # call (accepted OR refused), in arrival order. Distinct from
        # ``primary_tokens_by_turn`` (running total, monotonic). This view
        # preserves the per-turn shape so drift detection is not blind at the
        # refusal boundary where usage peaks.
        self._primary_observed = []
        # Partial-curve warnings: emitted by the integration seam when at least
        # one per-row try_record_turn succeeded but a later one failed, so the
        # per-turn curve is incomplete. Consumers distinguish partial from
        # fully-aggregated via this list.
        self.partial_curve_warnings = []      

    @property
    def balance(self):
        return self._balance

    def charge(self, tokens, *, source):
        if tokens < 0:
            raise ValueError("cannot credit via charge; use record_turn for primary spend")
        post = self._balance - tokens
        if post < 0:
            # Hard model-token cap: refuse the charge rather than letting the
            # ledger go negative. The caller must decide whether to escalate
            # or to stop the dispatch; either way a silent over-spend cannot
            # hide as a "balance went to -256" side-effect.
            raise RuntimeError(
                f"hard model-token cap exceeded: balance={self._balance} "
                f"charge={tokens} source={source!r}"
            )
        self._balance = post
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

        Distinct points: each call appends exactly one entry to ``turn_boundaries``
        AND to ``per_turn_primary`` so a caller cannot "aggregate into one point"
        by repeatedly calling record_turn with the same turn_index - the source
        suffix disambiguates and ``per_turn_primary`` retains the per-call value
        in insertion order. The per-turn primary spend is also capped against
        ``primary_budget`` and refused if it would exceed it.

        Strict-mode: raises ``PrimaryBudgetExhausted`` when the per-turn charge
        would push primary spend past ``primary_budget``. Callers that need to
        keep the run alive (the integration path under swebench_supervision)
        should use ``try_record_turn`` which records a rejection and returns
        False instead.
        """
        if primary_tokens_this_turn < 0:
            raise ValueError("primary_tokens_this_turn must be >= 0")
        # Per-turn distinct: refuse to collapse multiple record_turn calls into
        # a single point by inserting with a unique nonce-suffixed source.
        nonce = len(self.per_turn_primary)
        if self._primary_spent + primary_tokens_this_turn > self._primary_budget:
            raise PrimaryBudgetExhausted(
                f"primary_budget cap exceeded: spent={self._primary_spent} "
                f"charge={primary_tokens_this_turn} "
                f"primary_budget={self._primary_budget}"
            )
        prev = self.turn_boundaries[-1] if self.turn_boundaries else 0
        new_total = prev + primary_tokens_this_turn
        self.charge(primary_tokens_this_turn, source=f"primary.turn:{turn_index}#{nonce}")
        self.turn_boundaries.append(new_total)
        self.per_turn_primary.append({
            "turn_index": turn_index,
            "nonce": nonce,
            "primary_tokens_this_turn": primary_tokens_this_turn,
            "running_total": new_total,
        })
        self._primary_spent += primary_tokens_this_turn
        # Observed per-turn series (F4): one point per accepted turn, aligned 1:1
        # with ``turn_boundaries``. Recorded here (not only in ``try_record_turn``)
        # so a strict-path caller still gets a populated observed curve and the two
        # views never drift out of length-alignment. The refusal path in
        # ``try_record_turn`` appends its own observed point separately.
        self._primary_observed.append(int(primary_tokens_this_turn))
        return new_total

    def try_record_turn(self, turn_index, primary_tokens_this_turn, *, caller=""):
        """Like ``record_turn`` but NEVER raises on primary_budget overflow.

        Returns ``(accepted, running_total)`` where ``accepted`` is True if the
        charge was applied and False if it was refused. A refusal is recorded
        in ``primary_rejections`` (audit-visible) instead of being swallowed,
        so the integration path can keep running while the auditor still sees
        an explicit "budget exhausted" row per refused turn.

        This is the path the live supervisor should call - the original
        ``record_turn`` stays strict for callers that want a hard error.

        Drift-detection contract (roundtable F4): the running-total curve
        ``primary_tokens_by_turn`` records a point for every turn so the curve
        is not blind at peak usage. On refusal the running balance is not
        advanced, so the curve point equals the prior balance (monotonicity
        preserved). The per-turn observed primary tokens of the refused turn
        are recorded in ``primary_tokens_observed_by_turn`` (and
        ``primary_rejections``) so drift detection still has a per-turn shape.
        """
        if primary_tokens_this_turn < 0:
            raise ValueError("primary_tokens_this_turn must be >= 0")
        # Always record the observed per-turn primary tokens so the per-turn
        # telemetry view has one point per turn (accepted or refused).
        self._primary_observed.append(int(primary_tokens_this_turn))
        if self._primary_spent + primary_tokens_this_turn > self._primary_budget:
            self.primary_rejections.append({
                "turn_index": turn_index,
                "attempted": primary_tokens_this_turn,
                "primary_spent_at_reject": self._primary_spent,
                "primary_budget": self._primary_budget,
                "caller": caller,
                "reason": "primary_budget_exhausted",
            })
            # Curve stays monotonic on refusal: append the unchanged running
            # total so the per-turn point exists without advancing the balance.
            prev = self.turn_boundaries[-1] if self.turn_boundaries else 0
            self.turn_boundaries.append(prev)
            return False, prev
        # Hard model-token cap (record_turn -> charge -> RuntimeError): the
        # per-turn row's observed peak was already recorded above, but the
        # charge would push the model balance negative. Treat this as another
        # refusal class so callers get a (False, prev) tuple instead of an
        # exception escaping the supervisor. The observed peak stays visible
        # via primary_tokens_observed_by_turn / detect_drift(include_observed).
        prev = self.turn_boundaries[-1] if self.turn_boundaries else 0
        try:
            new_total = self.record_turn(turn_index, primary_tokens_this_turn)
        except (RuntimeError, PrimaryBudgetExhausted) as exc:
            self.primary_rejections.append({
                "turn_index": turn_index,
                "attempted": primary_tokens_this_turn,
                "primary_spent_at_reject": self._primary_spent,
                "primary_budget": self._primary_budget,
                "caller": caller,
                "reason": "model_token_hard_cap",
                "detail": str(exc),
            })
            # Curve stays monotonic on hard-cap refusal too: append prev.
            self.turn_boundaries.append(prev)
            return False, prev
        return True, new_total

    def record_cap(self, *, caller, original_tokens, capped_tokens, cap, kind="digest"):
        """Record a digest cap event for audit (F1: cap result must not be discarded).

        Append-only. Returns the cap event dict so callers can chain it through
        if they want. Equality ``capped_tokens == original_tokens`` means the
        digest was at-or-below the cap and no truncation happened; the audit row
        is still recorded so the auditor can see that a cap was *checked*.
        """
        ev = {
            "caller": caller,
            "kind": kind,
            "cap": cap,
            "original_tokens": original_tokens,
            "capped_tokens": capped_tokens,
            "truncated": capped_tokens < original_tokens,
        }
        self.cap_events.append(ev)
        return ev

    @property
    def primary_tokens_by_turn(self):
        """Running total of primary token spend at the end of each turn, oldest first.

        This is the supervisor's per-turn telemetry curve - one point per recorded turn,
        monotonically non-decreasing. It does NOT include escalation charges, which live
        in ``entries`` for separate audit.

        F4 auditability: a row is appended for every turn - accepted or refused - so the
        curve has no gaps. On refusal the row equals the unchanged running total
        (balance was not advanced), preserving monotonicity.
        """
        return list(self.turn_boundaries)

    @property
    def primary_tokens_observed_by_turn(self):
        """Per-turn observed primary tokens in arrival order, oldest first.

        Distinct from ``primary_tokens_by_turn``: this view records the ACTUAL primary
        tokens consumed each turn, regardless of whether the charge was accepted or
        refused. Use this view for drift detection (a single turn spike is visible).
        ``primary_tokens_by_turn`` is the monotonic running-total view; both views
        are length-aligned (same number of rows).
        """
        return list(self._primary_observed)

    def record_partial_curve_warning(self, *, appended: int, total: int) -> None:
        """Record that the per-turn primary curve is incomplete.

        Used by the integration seam in ``run_arm_c_supervised`` when at least
        one per-row ``try_record_turn`` succeeded but a later one failed. The
        ledger is left untouched (the rows already appended remain on the
        curve), and we just stamp an audit row so consumers can tell the
        curve is partial vs. fully aggregated.

        Per the security review fix (round 2): the aggregate fallback MUST NOT
        fire on top of already-appended rows, because that would double-count
        and corrupt the per-turn curve. This warning is the only artifact a
        caller gets in that case.
        """
        if total < 0 or appended < 0:
            raise ValueError("appended and total must be >= 0")
        if appended > total:
            raise ValueError("appended cannot exceed total")
        self.partial_curve_warnings.append({
            "appended": int(appended),
            "total": int(total),
            "timestamp": _now(),
        })

    @property
    def primary_tokens_total(self):
        """Accumulated primary token spend across all recorded turns."""
        return self.turn_boundaries[-1] if self.turn_boundaries else 0

    def detect_drift(self, *, rapid_growth_factor=4, rapid_growth_window=3, include_observed: bool = True):
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
        # 3. Per-turn observed primary spend (F4 drift-blindness fix): the
        # running-total series is BLIND to a peak that primary_budget refuses,
        # because try_record_turn does not advance the running total on
        # refusal. The observed series (one value per try_record_turn call,
        # accepted OR refused) preserves the actual turn shape, so a refused
        # peak IS visible here. Without this scan, a primary that hits
        # primary_budget on a runaway turn would silently pass drift checks.
        #
        # We scan the observed VALUES (not deltas) so a baseline of identical
        # small turns + a single big turn is still flagged: a delta-of-deltas
        # scan would lose the median when the lookback window is all zeros.
        if include_observed and len(self._primary_observed) >= 2:
            for i in range(1, len(self._primary_observed)):
                cur = self._primary_observed[i]
                lookback = self._primary_observed[max(0, i - window):i]
                if not lookback:
                    continue
                med = sorted(lookback)[len(lookback) // 2]
                if med > 0 and cur > rapid_growth_factor * med:
                    findings.append(
                        f"primary_tokens_observed grew rapidly at turn {i + 1}: "
                        f"observed={cur} vs median(previous {len(lookback)})={med} "
                        f"(factor > {rapid_growth_factor}; peak may have been "
                        f"refused by primary_budget, making this spike invisible "
                        f"to the running-total curve)"
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

    def __init__(self, *, ledger=None, cap=DIGEST_CAP,
                 starting_balance=HARD_MODEL_TOKEN_CAP, primary_budget=None):
        # Convenience: callers (and BudgetSupervisor) can pass the ledger-level
        # knobs here and we'll spin up a fresh ledger with them. If they pass an
        # explicit ledger, it's used as-is - the kwargs are ignored.
        if ledger is None:
            ledger = BudgetLedger(
                starting_balance=starting_balance,
                primary_budget=primary_budget,
            )
        self.ledger = ledger
        self.gate = EscalationGate(self.ledger)
        self.dispatcher = ToolDispatcher(self.gate)
        self.cap = cap

    def cap_digest(self, text, *, caller="supervisor", kind="digest"):
        """Cap a digest and AUDIT the cap result (F1).

        F1 fix: previously the call site discarded (text, orig, capped) and a
        truncated digest silently looked identical to a non-truncated one to
        downstream readers. Now the cap is recorded on the ledger's ``cap_events``
        so the auditor can see exactly when a digest was truncated, by whom, and
        how many tokens were shaved off.
        """
        text2, orig, capped = cap_digest(text, cap=self.cap)
        self.ledger.record_cap(
            caller=caller, original_tokens=orig, capped_tokens=capped,
            cap=self.cap, kind=kind,
        )
        return text2, orig, capped

    def check_tool_dispatch(self, tool, *, caller="unknown"):
        """Dispatch boundary: raises EscalationDenied if tool is not allowed.

        F2/F3 auditability: every denied attempt is recorded on
        ``self.gate.attempts`` before the exception propagates, so the ledger
        shows denied tool-dispatch attempts (not only granted escalations).
        """
        self.dispatcher.check(tool, caller=caller)

    def record_turn(self, turn_index, primary_tokens):
        return self.ledger.record_turn(turn_index, primary_tokens)

    def try_record_turn(self, turn_index, primary_tokens, *, caller=""):
        """Non-throwing primary spend (F5): records a rejection in the ledger when the
        per-turn charge would exceed primary_budget, instead of aborting the run."""
        return self.ledger.try_record_turn(turn_index, primary_tokens, caller=caller)

    def record_cap(self, *, caller, original_tokens, capped_tokens, cap, kind="digest"):
        return self.ledger.record_cap(
            caller=caller, original_tokens=original_tokens, capped_tokens=capped_tokens,
            cap=cap, kind=kind,
        )

    @property
    def primary_tokens_by_turn(self):
        return self.ledger.primary_tokens_by_turn

    @property
    def cap_events(self):
        return self.ledger.cap_events

    @property
    def primary_rejections(self):
        return self.ledger.primary_rejections


# Public alias: some callers (including the swebench integration seam and the
# reviewer-blocker tests) ask for the supervisor under the more descriptive name
# ``BudgetSupervisor``. Functionally identical to ``Supervisor`` - we expose the
# alias so the ledger surface area is reachable as ``sup.ledger.<field>`` and
# downstream code can spell out what the object actually is.
BudgetSupervisor = Supervisor
