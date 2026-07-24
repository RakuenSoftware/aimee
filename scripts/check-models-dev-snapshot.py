#!/usr/bin/env python3
"""Guard the bundled models.dev snapshot against silent rot.

data/models_dev_snapshot.json is the OFFLINE capability/price fallback: it
answers lookups when ~/.cache/aimee/models_dev.json is absent (air-gapped
install, first run, no network). It previously drifted to 10 hand-maintained
entries, none of which were current models, so every lookup silently fell
through to the built-in heuristic — which publishes no prices at all and cannot
distinguish model families.

This checks STRUCTURE and FLEET COVERAGE, deliberately not freshness: asserting
"the newest model exists" would fail the build every time a vendor ships, which
teaches people to disable the check. Regenerate with:

    python3 scripts/gen-models-dev-snapshot.py
"""

import json
import os
import sys

SNAPSHOT = os.path.join(os.path.dirname(__file__), "..", "data", "models_dev_snapshot.json")

# Vendor keys agent_derive_catalog_provider() resolves to. A snapshot missing one
# of these cannot answer for agents that derive to it.
REQUIRED_PROVIDERS = ["anthropic", "openai", "minimax", "moonshotai"]

# Each must resolve with a usable context window and both price axes, or the
# offline path silently degrades to the heuristic for the live fleet.
REQUIRED_MODELS = [
    ("anthropic", "claude-opus-4-8"),
    ("minimax", "MiniMax-M3"),
    ("moonshotai", "kimi-k2.7-code"),
    ("openai", "gpt-5.6-sol"),
]


def fail(msg):
    print(f"models-dev-snapshot: {msg}", file=sys.stderr)
    return 1


def main():
    try:
        with open(SNAPSHOT) as f:
            snap = json.load(f)
    except Exception as e:  # noqa: BLE001 - report any load failure identically
        return fail(f"cannot read {SNAPSHOT}: {e}")

    if not isinstance(snap, dict) or not snap:
        return fail("snapshot is empty or not an object")

    # Must be the NESTED schema: only it carries `reasoning` and `name`, so a
    # flat snapshot cannot express MODEL_CAP_REASONING or a display label.
    for pid, prov in snap.items():
        if "/" in pid:
            return fail(f"flat-schema key {pid!r}; regenerate with "
                        "scripts/gen-models-dev-snapshot.py")
        if not isinstance(prov, dict) or not isinstance(prov.get("models"), dict):
            return fail(f"provider {pid!r} has no models object")

    missing = [p for p in REQUIRED_PROVIDERS if p not in snap]
    if missing:
        return fail(f"missing required provider(s): {', '.join(missing)}")

    errors = []
    for pid, mid in REQUIRED_MODELS:
        m = snap.get(pid, {}).get("models", {}).get(mid)
        if not isinstance(m, dict):
            errors.append(f"{pid}/{mid}: absent")
            continue
        ctx = (m.get("limit") or {}).get("context")
        cost = m.get("cost") or {}
        if not isinstance(ctx, int) or ctx <= 0:
            errors.append(f"{pid}/{mid}: no usable limit.context")
        if not isinstance(cost.get("input"), (int, float)) or cost["input"] <= 0:
            errors.append(f"{pid}/{mid}: no cost.input")
        if not isinstance(cost.get("output"), (int, float)) or cost["output"] <= 0:
            errors.append(f"{pid}/{mid}: no cost.output")

    if errors:
        for e in errors:
            print(f"models-dev-snapshot: {e}", file=sys.stderr)
        return fail("regenerate with scripts/gen-models-dev-snapshot.py")

    total = sum(len(p["models"]) for p in snap.values())
    print(f"models-dev-snapshot: ok ({len(snap)} providers, {total} models)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
