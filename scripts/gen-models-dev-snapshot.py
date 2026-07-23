#!/usr/bin/env python3
"""Regenerate data/models_dev_snapshot.json from the live models.dev registry.

The snapshot is aimee's OFFLINE fallback: it answers model capability lookups
when ~/.cache/aimee/models_dev.json is absent or stale (air-gapped installs,
first run, no network). It had drifted to 10 hand-maintained entries, none of
which were current models, so in practice every lookup fell through to the
built-in heuristic.

Emits the NESTED models.dev schema rather than the older flat
{"provider/model": {...}} form. Both are read (models_dev_cache.c), but only the
nested form carries `reasoning` and the human-facing `name` — the flat schema has
no field for either, so a flat snapshot cannot express MODEL_CAP_REASONING or a
display label.

Curated to the providers aimee can actually route to, and to non-deprecated
models, to keep the bundled artifact small.

Usage:  python3 scripts/gen-models-dev-snapshot.py [--out PATH]
        python3 scripts/gen-models-dev-snapshot.py --from-file api.json
"""

import argparse
import json
import sys
import urllib.request

DEFAULT_URL = "https://models.dev/api.json"
DEFAULT_OUT = "data/models_dev_snapshot.json"

# Providers aimee has a wire path to, plus the vendor keys its agents resolve to
# (see agent_derive_catalog_provider in src/server/agent_config.c).
KEEP_PROVIDERS = [
    "anthropic",
    "openai",
    "google",
    "mistral",
    "minimax",
    "moonshotai",
    "openrouter",
    "deepseek",
    "zai",
    "groq",
    "cerebras",
    "ollama",
    "llama",
]

# Fields the C reader consumes (fill_cap_from_nested). Anything else is dropped
# so the snapshot does not balloon with data nothing reads.
KEEP_MODEL_FIELDS = ["name", "limit", "cost", "tool_call", "reasoning", "modalities",
                     "open_weights", "knowledge"]


def prune_model(m):
    out = {k: m[k] for k in KEEP_MODEL_FIELDS if k in m}
    # cost carries per-context-band tiers that nothing reads yet; keep the base
    # axes plus cache_read, drop the bands to hold the artifact down.
    cost = out.get("cost")
    if isinstance(cost, dict):
        out["cost"] = {k: v for k, v in cost.items()
                       if k in ("input", "output", "cache_read", "cache_write")}
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default=DEFAULT_URL)
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--from-file", help="read the registry from disk instead of fetching "
                                        "(reproducible builds, air-gapped regeneration)")
    args = ap.parse_args()

    if args.from_file:
        with open(args.from_file) as f:
            registry = json.load(f)
    else:
        # models.dev rejects the stdlib default agent with HTTP 403.
        req = urllib.request.Request(args.url, headers={"User-Agent": "aimee-snapshot/1"})
        with urllib.request.urlopen(req, timeout=30) as r:
            registry = json.load(r)

    snapshot = {}
    kept_models = 0
    for pid in KEEP_PROVIDERS:
        prov = registry.get(pid)
        if not isinstance(prov, dict):
            continue
        models = prov.get("models")
        if not isinstance(models, dict):
            continue
        keep = {}
        for mid, m in models.items():
            if not isinstance(m, dict) or m.get("deprecated"):
                continue
            keep[mid] = prune_model(m)
        if not keep:
            continue
        entry = {"models": keep}
        if isinstance(prov.get("name"), str):
            entry["name"] = prov["name"]
        snapshot[pid] = entry
        kept_models += len(keep)

    if kept_models == 0:
        print("refusing to write an empty snapshot", file=sys.stderr)
        return 1

    with open(args.out, "w") as f:
        json.dump(snapshot, f, indent=1, sort_keys=True)
        f.write("\n")

    print(f"wrote {args.out}: {len(snapshot)} providers, {kept_models} models")
    return 0


if __name__ == "__main__":
    sys.exit(main())
