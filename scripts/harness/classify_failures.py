#!/usr/bin/env python3
"""classify_failures.py: attribute agent-run failures to one of the four harness
parts — loop, tool, context, or control.

Framing (the "four-part harness"): every agent runtime is loop + tool interface +
context management + control. When a run misbehaves it is almost always ONE part,
not "the model". This tool turns that triage move into a mechanical classifier so
"context management is our biggest tax" becomes a measured distribution instead of
a hunch.

It is the runnable reference implementation of the classifier specced in
docs/proposals/pending/four-part-harness-taxonomy.md. The heuristics mirror the
signals aimee already computes in src/trace_analysis.c (the retry-loop detector,
the error-result indicators) so a later in-process port stays faithful.

Input is execution-trace rows — the same shape src/trace_analysis.c mines
(db1_execution_trace_mining_row_t): one JSON array of
  {plan_id, turn, direction, tool_name, tool_args, tool_result}
read from a file or stdin. Output is one incident per detected failure, each
tagged with its harness part, the signal that fired, and a confidence, plus an
aggregate distribution.

Pure analysis, no build and no aimee runtime needed. Run --self-test for a
red/green check against synthetic traces that exercise each of the four parts.

Usage:
  classify_failures.py traces.json
  aimee trajectory export --json | classify_failures.py -
  classify_failures.py --self-test
"""
import argparse
import json
import re
import sys

# Mirror src/trace_analysis.c: RETRY_THRESHOLD consecutive same-tool calls with
# >=2 errors is a retry loop. Keep these in lockstep with the C constants
# (src/trace_analysis.c: `#define RETRY_THRESHOLD 3` and the `errors >= 2` in
# detect_retry_loops()). Like the C, the run is keyed on tool_name only; the
# trace `direction` field is intentionally NOT a grouping key (detect_retry_loops()
# ignores it too), so a port off db1_execution_trace_mining_row_t stays faithful.
RETRY_THRESHOLD = 3
RETRY_MIN_ERRORS = 2
# A run with more turns than this and no explicit budget/limit marker reads as a
# missing stop condition (control), not as the model "being dumb".
RUNAWAY_TURNS = 50

# The four parts. Order matters: classification walks specific -> general.
PART_LOOP = "loop"
PART_TOOL = "tool"
PART_CONTEXT = "context"
PART_CONTROL = "control"
PARTS = (PART_LOOP, PART_TOOL, PART_CONTEXT, PART_CONTROL)

PART_BLURB = {
    PART_LOOP: "agent loop — looped forever or quit early",
    PART_TOOL: "tool interface — wrong tool, bad args, or dispatch fault",
    PART_CONTEXT: "context management — ignored information it was given",
    PART_CONTROL: "control — a budget/limit/timeout/circuit-breaker boundary",
}

# Error indicators, copied from result_looks_like_error() in src/trace_analysis.c
# so "is this row a failure" matches what the C miner already flags.
ERROR_MARKERS = (
    "error", "Error", "ERROR", "failed", "Failed", "FAILED",
    "No such file", "not found", "Permission denied", "command not found",
)

# A control boundary was hit: budget, rate, timeout, context-window, circuit.
# These outrank every other signal — a 429 is not a tool-design problem.
CONTROL_MARKERS = (
    "rate limit", "rate-limit", "429", "too many requests",
    "timeout", "timeouts", "timed out", "timed-out", "deadline exceeded",
    "context length", "context window", "token limit", "token limits",
    "maximum context", "budget", "quota", "quotas", "circuit",
    "max turns", "max steps",
    "killed", "oom", "out of memory", "cancelled", "canceled", "aborted",
)
# Note: markers are word-bounded (see _compile_markers), so inflected forms that
# are not listed (e.g. "rate-limited") fall through to 'unclassified' rather than
# being mis-attributed. The high-frequency plurals/participles above are listed
# explicitly; the trade-off is deliberate (under-count over corrupt).

# A tool-interface fault: the call itself was malformed or misdirected, as opposed
# to the world legitimately saying "that thing is not here".
TOOL_FAULT_MARKERS = (
    "command not found", "unknown tool", "no such tool", "unknown command",
    "invalid argument", "unexpected argument", "unrecognized argument",
    "unrecognized arguments",  # argparse's canonical message is always plural
    "usage:", "missing required", "required argument", "invalid arguments",
    "json parse", "could not parse", "malformed", "schema", "bad request",
    "permission denied",
)


def _compile_markers(markers):
    """Word-boundary matchers for the lowercased markers. Bare substring matching
    mis-fires on short alphabetic markers ('oom' inside 'room-service', 'aborted'
    inside ...), and because the control band outranks everything that corrupts the
    very distribution this tool exists to produce. Boundaries are added only on the
    word-character edges so phrase/colon markers ('usage:', 'rate-limit') still
    match. Returns (marker, compiled) pairs, order preserved for _first_marker."""
    out = []
    for m in markers:
        left = r"\b" if m[0].isalnum() else ""
        right = r"\b" if m[-1].isalnum() else ""
        out.append((m, re.compile(left + re.escape(m) + right)))
    return out


CONTROL_PATTERNS = _compile_markers(CONTROL_MARKERS)
TOOL_FAULT_PATTERNS = _compile_markers(TOOL_FAULT_MARKERS)


def looks_like_error(result):
    # Case-sensitive substring match, kept byte-for-byte identical to
    # result_looks_like_error() in src/trace_analysis.c (the C miner's own rule).
    if not result:
        return False
    return any(m in result for m in ERROR_MARKERS)


def has_any(result, patterns):
    if not result:
        return False
    low = result.lower()
    return any(p.search(low) for _, p in patterns)


def _key_args(args):
    """Stable, hashable key for tool_args. In real traces tool_args is a string
    (db1 char[]), but this tool ingests arbitrary JSON, so a list/dict arg would
    make the (tool, args) dict key unhashable and crash the run. Coerce anything
    non-scalar to a canonical JSON string so refetch detection still keys cleanly."""
    if isinstance(args, (str, bytes, int, float, bool)) or args is None:
        return args
    return json.dumps(args, sort_keys=True, default=str)


def _incident(part, signal, confidence, plan_id, turn, tool, detail):
    return {
        "part": part,
        "signal": signal,
        "confidence": round(confidence, 2),
        "plan_id": plan_id,
        "turn": turn,
        "tool": tool,
        "detail": detail,
    }


def classify_plan(rows):
    """Classify failures within a single plan's ordered trace rows.

    Returns a list of incident dicts. A retry loop is reported once for the run,
    not once per row, so a single stuck loop does not drown out everything else.
    """
    incidents = []
    consumed = [False] * len(rows)

    # 1. Runaway length with no explicit limit marker -> control (no stop cond).
    if rows:
        turns = {r.get("turn") for r in rows if r.get("turn") is not None}
        if len(turns) > RUNAWAY_TURNS and not any(
            has_any(r.get("tool_result"), CONTROL_PATTERNS) for r in rows
        ):
            incidents.append(_incident(
                PART_CONTROL, "runaway-length", 0.6,
                rows[0].get("plan_id"), max(turns), None,
                f"{len(turns)} turns with no budget/limit boundary observed",
            ))

    # 2. Retry loops: same tool >=RETRY_THRESHOLD consecutive with >=2 errors.
    #    This is the loop/control failure — the model is stuck, not wrong.
    i = 0
    n = len(rows)
    while i < n:
        if consumed[i] or not rows[i].get("tool_name"):
            i += 1
            continue
        run = 1
        errors = 1 if looks_like_error(rows[i].get("tool_result")) else 0
        j = i + 1
        while j < n and rows[j].get("tool_name") == rows[i].get("tool_name"):
            run += 1
            if looks_like_error(rows[j].get("tool_result")):
                errors += 1
            j += 1
        if run >= RETRY_THRESHOLD and errors >= RETRY_MIN_ERRORS:
            for k in range(i, j):
                consumed[k] = True
            incidents.append(_incident(
                PART_LOOP, "retry-loop", 0.8,
                rows[i].get("plan_id"), rows[i].get("turn"), rows[i].get("tool_name"),
                f"'{rows[i].get('tool_name')}' called {run}x consecutively, {errors} errors",
            ))
            i = j
            continue
        i += 1

    # 3. Remaining rows in temporal order. Track (tool, args) pairs that already
    #    SUCCEEDED, with their result bytes, so a later re-issue with nothing changed
    #    is a re-fetch of something already in context (context failure). Storing the
    #    result lets us exclude the legitimate read-edit-reread cycle: a re-read that
    #    returns DIFFERENT bytes is a valid re-read after a mutation, not ignored
    #    context.
    succeeded = {}  # key -> (idx, result_string)
    for idx, r in enumerate(rows):
        # Rows already attributed to a retry loop above are spoken for; skipping
        # them up front keeps a stuck loop from being double-counted here.
        if consumed[idx]:
            continue
        tool = r.get("tool_name")
        key = (tool, _key_args(r.get("tool_args")))
        result = r.get("tool_result")
        plan_id, turn = r.get("plan_id"), r.get("turn")

        # Control boundary outranks EVERYTHING — a limit/timeout/quota was hit,
        # whether or not the generic error-marker heuristic also fired. Checking it
        # before the success and refetch paths keeps the "control wins" invariant
        # honest: a result that says "deadline exceeded" but contains no error word
        # is still a control incident, not a clean success or a context refetch.
        if has_any(result, CONTROL_PATTERNS):
            incidents.append(_incident(
                PART_CONTROL, "limit-marker", 0.75, plan_id, turn, tool,
                _first_marker(result, CONTROL_PATTERNS),
            ))
            continue

        if not looks_like_error(result):
            if tool:
                prior = succeeded.get(key)
                # Byte-identical successful refetch: the prior result was in context
                # and re-fetched. Known false positive: an idempotent poll loop
                # (re-issuing the same call, same bytes, while waiting for state to
                # change) also matches. That is why this is the lowest-confidence
                # (0.5) non-unclassified signal — a nomination, not a verdict.
                if prior is not None and prior[1] == result:
                    incidents.append(_incident(
                        PART_CONTEXT, "redundant-refetch", 0.5, plan_id, turn, tool,
                        f"'{tool}' re-issued with identical args and byte-identical "
                        f"result; the prior result was already in context",
                    ))
                # Record the latest success so a changed re-read updates the baseline.
                succeeded[key] = (idx, result)
            continue

        # An erroring re-issue of a call that already succeeded = ignored context.
        if key in succeeded:
            incidents.append(_incident(
                PART_CONTEXT, "redundant-refetch", 0.65, plan_id, turn, tool,
                f"'{tool}' re-called with identical args after an earlier success",
            ))
            continue

        # Dispatch/arg-shaped error = the call was malformed or misdirected.
        if has_any(result, TOOL_FAULT_PATTERNS):
            incidents.append(_incident(
                PART_TOOL, "dispatch-or-arg-fault", 0.6, plan_id, turn, tool,
                _first_marker(result, TOOL_FAULT_PATTERNS),
            ))
            continue

        # A generic error we cannot attribute with confidence. Surfacing it as
        # unclassified is honest; silently dropping it would understate the tax.
        incidents.append(_incident(
            "unclassified", "generic-error", 0.2, plan_id, turn, tool,
            "error result with no distinguishing signal",
        ))

    return incidents


def _first_marker(result, patterns):
    low = result.lower()
    for m, p in patterns:
        if p.search(low):
            return f"matched '{m}'"
    return ""


def classify(rows):
    by_plan = {}
    for r in rows:
        by_plan.setdefault(r.get("plan_id"), []).append(r)
    incidents = []
    for plan_rows in by_plan.values():
        incidents.extend(classify_plan(plan_rows))
    return incidents


def distribution(incidents):
    counts = {p: 0 for p in PARTS}
    counts["unclassified"] = 0
    tools = {p: {} for p in list(PARTS) + ["unclassified"]}
    for inc in incidents:
        part = inc["part"]
        counts[part] = counts.get(part, 0) + 1
        if inc.get("tool"):
            tools[part][inc["tool"]] = tools[part].get(inc["tool"], 0) + 1
    return counts, tools


def render(incidents):
    counts, tools = distribution(incidents)
    total = sum(counts.values())
    out = []
    out.append(f"Failure attribution across {total} incident(s):\n")
    if total == 0:
        out.append("  (no failures detected)")
        return "\n".join(out)
    width = max(len(p) for p in counts)
    for part in list(PARTS) + ["unclassified"]:
        c = counts.get(part, 0)
        if c == 0:
            continue
        pct = 100.0 * c / total
        bar = "#" * int(round(pct / 5))
        blurb = PART_BLURB.get(part, "unclassified — no distinguishing signal")
        out.append(f"  {part.ljust(width)}  {c:3d}  {pct:5.1f}%  {bar}")
        out.append(f"  {' '.ljust(width)}        {blurb}")
        top = sorted(tools[part].items(), key=lambda kv: -kv[1])[:3]
        if top:
            out.append(f"  {' '.ljust(width)}        top tools: "
                       + ", ".join(f"{t}({n})" for t, n in top))
    out.append("")
    biggest = max(counts.items(), key=lambda kv: kv[1])
    if biggest[1]:
        out.append(f"Biggest tax: {biggest[0]} ({biggest[1]}/{total}). "
                   "Fix that part before blaming the model.")
    return "\n".join(out)


# --- self-test: synthetic plans, one per part (red/green) --------------------

def _row(plan, turn, tool, args="", result=""):
    return {"plan_id": plan, "turn": turn, "direction": "result",
            "tool_name": tool, "tool_args": args, "tool_result": result}


def _self_test_traces():
    plans = []
    # LOOP: same tool 3x consecutive, 3 errors.
    plans += [_row(1, t, "grep", "pat", "Error: bad regex") for t in range(3)]
    # TOOL: a single dispatch/arg fault.
    plans += [_row(2, 0, "bash", "frobnicate", "bash: frobnicate: command not found")]
    # CONTEXT: read a file successfully, then re-read it with byte-identical result.
    # The successful re-read IS flagged: the prior result was already in context.
    plans += [_row(3, 0, "read_file", "a.c", "int main(){}")]
    plans += [_row(3, 1, "read_file", "a.c", "int main(){}")]
    # CONTROL: a budget/limit boundary.
    plans += [_row(4, 0, "delegate", "review", "request failed: 429 rate limit exceeded")]
    return plans


def _by_plan_parts(incidents):
    out = {}
    for inc in incidents:
        out.setdefault(inc["plan_id"], []).append(inc["part"])
    return out


def self_test():
    """Red/green check. Each assertion fails loudly so a degenerate classifier
    (e.g. one that returns nothing, or labels everything 'control') cannot pass."""
    ok = True

    def check(name, cond):
        nonlocal ok
        status = "PASS" if cond else "FAIL"
        if not cond:
            ok = False
        print(f"  [{status}] {name}")

    # 1-4: one synthetic plan per part maps to the right part, and ONLY that part.
    incidents = classify(_self_test_traces())
    parts = _by_plan_parts(incidents)
    for plan, part in ((1, PART_LOOP), (2, PART_TOOL), (3, PART_CONTEXT), (4, PART_CONTROL)):
        got = parts.get(plan, [])
        check(f"plan {plan}: {part} and nothing else (got {got or ['(none)']})",
              got and all(p == part for p in got))

    # 5: read-edit-reread must NOT be flagged. Identical args, but the second read
    #    returns DIFFERENT bytes (a real edit happened), so it is a valid re-read.
    rer = classify([
        _row(5, 0, "read_file", "b.c", "version one"),
        _row(5, 1, "write_file", "b.c", "wrote b.c"),
        _row(5, 2, "read_file", "b.c", "version two"),
    ])
    check("read-edit-reread (changed bytes) yields no incident", rer == [])

    # 6: control markers are word-bounded. A benign error containing 'room' (which
    #    bare-substring 'oom' would have mis-matched) must NOT classify as control.
    room = classify([_row(6, 0, "bash", "curl", "Error: connection refused to room-service")])
    check("benign 'room' error is not mis-classified as control",
          all(inc["part"] != PART_CONTROL for inc in room))

    # 6b: an erroring re-issue of an already-succeeded call is the other context path.
    err_refetch = classify([
        _row(9, 0, "read_file", "c.c", "data"),
        _row(9, 1, "read_file", "c.c", "Error: vanished"),
    ])
    check("erroring re-issue after a success -> context",
          any(i["part"] == PART_CONTEXT and i["signal"] == "redundant-refetch"
              for i in err_refetch))

    # 7: edge cases the code handles but the happy path never exercised.
    check("empty trace yields no incidents", classify([]) == [])
    all_ok = classify([_row(7, 0, "read_file", "x", "ok"), _row(7, 1, "grep", "y", "match")])
    check("all-success trace yields no incidents", all_ok == [])
    check("render() reports 'no failures detected' on empty input",
          "no failures detected" in render([]))

    # 8: runaway length with no limit marker -> control, even with zero error rows.
    runaway = classify([_row(8, t, "step", str(t), "fine") for t in range(RUNAWAY_TURNS + 1)])
    check("runaway length (no limit marker) -> one control incident",
          len(runaway) == 1 and runaway[0]["part"] == PART_CONTROL
          and runaway[0]["signal"] == "runaway-length")

    # 8b: runaway suppression — when a control marker IS present, the limit-marker
    #     path wins and 'runaway-length' must NOT also fire (locks that branch).
    long_capped = [_row(10, t, "step", str(t), "fine") for t in range(RUNAWAY_TURNS)]
    long_capped.append(_row(10, RUNAWAY_TURNS, "delegate", "x", "Error: 429 rate limit"))
    sigs = {i["signal"] for i in classify(long_capped)}
    check("runaway with a control marker -> limit-marker, not runaway-length",
          "limit-marker" in sigs and "runaway-length" not in sigs)

    # 9: argparse's always-plural 'unrecognized arguments' must classify as tool.
    argp = classify([_row(11, 0, "bash", "tool --z", "error: unrecognized arguments: --z")])
    check("argparse 'unrecognized arguments' -> tool",
          any(i["part"] == PART_TOOL for i in argp))

    # 10: control outranks the success-refetch path. A non-error result carrying a
    #     control marker, re-issued byte-identically, must classify as control
    #     (limit-marker), NOT as a context redundant-refetch.
    ctrl_refetch = classify([
        _row(12, 0, "delegate", "x", "deadline exceeded"),
        _row(12, 1, "delegate", "x", "deadline exceeded"),
    ])
    check("control marker outranks byte-identical refetch -> control, not context",
          ctrl_refetch and all(i["part"] == PART_CONTROL for i in ctrl_refetch))

    # 11: non-scalar tool_args must not crash the (tool, args) key. A list arg is
    #     coerced to a stable string key; a true byte-identical refetch still fires.
    list_args = classify([
        _row(13, 0, "grep", ["pat", "-r"], "match"),
        _row(13, 1, "grep", ["pat", "-r"], "match"),
    ])
    check("non-hashable (list) tool_args is handled and still detects refetch",
          any(i["part"] == PART_CONTEXT and i["signal"] == "redundant-refetch"
              for i in list_args))

    print()
    print(render(incidents))
    return 0 if ok else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("traces", nargs="?",
                    help="JSON array of trace rows, or '-' for stdin")
    ap.add_argument("--json", action="store_true", help="emit incidents as JSON")
    ap.add_argument("--self-test", action="store_true",
                    help="run synthetic red/green check and exit")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()

    if not args.traces:
        ap.error("provide a traces file, '-' for stdin, or --self-test")

    raw = sys.stdin.read() if args.traces == "-" else open(args.traces, encoding="utf-8").read()
    try:
        rows = json.loads(raw)
    except json.JSONDecodeError as e:
        print(f"error: traces is not valid JSON: {e}", file=sys.stderr)
        return 2
    if not isinstance(rows, list):
        print("error: traces must be a JSON array of trace rows", file=sys.stderr)
        return 2

    incidents = classify(rows)
    if args.json:
        counts, _ = distribution(incidents)
        print(json.dumps({"incidents": incidents, "distribution": counts}, indent=2))
    else:
        print(render(incidents))
    return 0


if __name__ == "__main__":
    sys.exit(main())
