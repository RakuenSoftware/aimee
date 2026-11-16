"""The per-turn hook must say so when recall is unreadable.

`aimee user-prompt-submit` always exits 0 and always emits an <aimee-context>
envelope carrying the static transport guidance. That is deliberate — a prompt
must never be blocked — but it means a reader cannot tell a turn where nothing
applied from one where every stored rule, preference and commitment was
unreachable. An agent in the second case proceeds as though unconstrained, and
nobody finds out.

session_degraded_notice exists for exactly this and was written with the right
words ("Absence of a result does NOT mean the thing does not exist"). It had no
caller in the shipped binary: its intended home, handle_session_start,
deliberately emits no context. This test pins the call site that was missing,
so the notice cannot quietly become dead code again.
"""
import json
import os
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
BINARY = ROOT / "aimee"

HOOK_INPUT = json.dumps(
    {
        "prompt": "commit this and push",
        "cwd": str(ROOT),
        "session_id": "degraded-notice-test",
    }
)

# Port 9 (discard) refuses fast and deterministically, so this models an
# unreachable control plane without a timeout or a fixture server.
DEAD_ENDPOINT = "https://127.0.0.1:9/v1"


def run_hook(endpoint):
    env = dict(os.environ)
    env["AIMEE_HOOK_CLIENT"] = "claude"
    env["AIMEE_HOOK_TRANSPORT"] = "cli"
    env["AIMEE_API_ENDPOINT"] = endpoint
    proc = subprocess.run(
        [str(BINARY), "user-prompt-submit"],
        input=HOOK_INPUT,
        capture_output=True,
        text=True,
        timeout=60,
        env=env,
    )
    return proc


def context_of(proc):
    assert proc.returncode == 0, (
        f"the hook must never block a prompt; exit={proc.returncode} stderr={proc.stderr!r}"
    )
    assert proc.stdout.strip(), "the hook emitted no envelope at all"
    payload = json.loads(proc.stdout)
    return payload["hookSpecificOutput"]["additionalContext"]


def test_unreachable_recall_is_announced():
    ctx = context_of(run_hook(DEAD_ENDPOINT))
    assert "degraded state" in ctx, (
        "recall failed and the envelope said nothing about it; an outage is "
        f"indistinguishable from an empty turn:\n{ctx}"
    )
    # The wording carries the instruction that makes the notice useful.
    assert "does NOT mean" in ctx
    assert "not fully functional" in ctx


def test_envelope_is_still_emitted_when_degraded():
    """The guidance must survive the failure path, not be replaced by it."""
    ctx = context_of(run_hook(DEAD_ENDPOINT))
    assert "<aimee-context>" in ctx or "explore-with" in ctx, (
        f"degraded turns lost the transport guidance entirely:\n{ctx}"
    )


def main():
    if not BINARY.exists():
        print(f"skip: {BINARY} not built")
        return 0
    failures = []
    for name, fn in sorted(globals().items()):
        if not name.startswith("test_") or not callable(fn):
            continue
        try:
            fn()
            print(f"ok   {name}")
        except AssertionError as exc:
            failures.append(name)
            print(f"FAIL {name}: {exc}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
