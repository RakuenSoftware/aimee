#!/usr/bin/env python3
"""The hand-written capability fallback must not shadow the model catalogue.

src/model_registry.c keeps a small table of models the catalogue does not carry.
It is a FALLBACK, not a second source of truth: a row for a model the catalogue
already describes is either dead weight (it agrees, and the catalogue is
consulted first) or a latent wrong answer (it disagrees, and serves that answer
whenever the catalogue load comes up empty).

That is not hypothetical. Three of the nine original rows had drifted from the
snapshot -- claude-sonnet-4-6 and claude-opus-4-6 each claimed 200000/8192 where
the catalogue says 1000000/128000 -- and a stale row had already outranked the
snapshot once, when the nested-schema reader returned zero entries.

The existing unit test (test_registry_agrees_with_catalog) checks that the
catalogue wins on the normal path, which stayed true the whole time the rows were
wrong. This checks the property that actually prevents the drift: no row for a
model the catalogue carries.
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REGISTRY = ROOT / "src/model_registry.c"
SNAPSHOT = ROOT / "data/models_dev_snapshot.json"
TABLE = "static const capability_entry_t g_capabilities[] = {"

ROW = re.compile(r'\{"([^"]+)",\s*"([^"]+)",\s*\d+,\s*\d+,', re.S)


def fail(message: str) -> int:
    print(f"check_model_fallback_shadow: error: {message}", file=sys.stderr)
    return 1


def main() -> int:
    src = REGISTRY.read_text(encoding="utf-8")
    try:
        start = src.index(TABLE)
        end = src.index("\n};", start)
    except ValueError:
        return fail(f"could not find {TABLE.strip()} in {REGISTRY.relative_to(ROOT)}")

    rows = ROW.findall(src[start:end])
    if not rows:
        return fail("the fallback table parsed as empty; the row shape probably changed")

    catalogue: set[tuple[str, str]] = set()
    doc = json.loads(SNAPSHOT.read_text(encoding="utf-8"))
    for provider, entry in doc.items():
        models = entry.get("models") if isinstance(entry, dict) else None
        if isinstance(models, dict):
            for model_id in models:
                catalogue.add((provider.lower(), model_id.lower()))
    if not catalogue:
        return fail(f"{SNAPSHOT.relative_to(ROOT)} enumerated zero models")

    shadowed = [f"{p}/{m}" for p, m in rows if (p.lower(), m.lower()) in catalogue]
    if shadowed:
        return fail(
            "these fallback rows duplicate a model the catalogue already carries, so "
            "they are dead when it loads and a stale answer when it does not:\n  "
            + "\n  ".join(shadowed)
            + "\nDelete the row. The catalogue is the source for anything it lists; "
            "to retire a model regardless of upstream, add it to g_local_deprecations."
        )

    print(
        f"check_model_fallback_shadow: ok ({len(rows)} fallback row(s), "
        f"{len(catalogue)} catalogued models, no overlap)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
