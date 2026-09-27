#!/usr/bin/env python3
"""A provider seam must be registered in the daemon that CONSUMES it.

A seam is a `*_register_*()` entry point: the module process supplies the real
decision, and the C side falls back to a local implementation when nobody
registers one. That fallback is the hazard -- a gate answering from a hardcoded
cue list looks exactly like a gate that works.

Registering it *somewhere* is not enough, and that is the trap this exists for.
aimee-server registered the §7 PII providers and calls RETRIEVE, but nothing in
the server invokes the gate: its only callers are kb_store code, which links into
aimee-kb and nothing else, and the kb registered nothing. The module never
decided anything, while a global "is this seam wired?" check looked green.

The seam -> consumer mapping is written out rather than inferred. Deriving it
needs a real C parser, and an approximate one produced confident nonsense (it
matched single-letter "functions" and reported delegate seams as kb_store consumers).
A short explicit table that a reviewer can check against the headers is worth
more than a clever extractor that is wrong in ways nobody notices.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / "src"

# seam -> the consumer whose answer changes when the seam is unwired.
# Every entry here is consumed by kb_store code, which links only into aimee-kb, so
# each must be registered by a kb source.
KB_STORE_CONSUMED_SEAMS = {
    # Fact ingestion now runs entirely in Go; no native policy callback remains.
}

KB = SRC / "kb"
KB_STORE = SRC / "modules/kb_store"


def is_test(path: pathlib.Path) -> bool:
    return "tests" in path.parts or path.name.startswith("test_")


def text_of(root: pathlib.Path) -> str:
    return "\n".join(p.read_text(encoding="utf-8", errors="replace")
                     for p in root.rglob("*.c") if not is_test(p))


def main() -> int:
    kb_text = text_of(KB)
    kb_store_text = text_of(KB_STORE)

    failures = []
    retired = ("aimee_kb_store_register_fact_gate_provider", "kb_store_fact_commit_with_actor")
    for symbol in retired:
        if re.search(rf"\b{symbol}\s*\(", kb_text + kb_store_text):
            failures.append(f"  retired native fact policy returned: {symbol}; use the Go memory owner")
    for seam, consumer in sorted(KB_STORE_CONSUMED_SEAMS.items()):
        if not re.search(rf"\b{re.escape(consumer)}\s*\(", kb_store_text):
            failures.append(f"  {seam}: kb_store no longer calls {consumer}(); "
                            f"drop this entry or point it at the new consumer")
            continue
        if not re.search(rf"\b{re.escape(seam)}\s*\(", kb_text):
            failures.append(f"  {seam}\n"
                            f"      kb_store calls {consumer}(), and kb_store runs in the kb\n"
                            f"      but no kb source registers it -- the gate will answer from"
                            f" its local implementation")

    if failures:
        print("check-provider-seams: provider seams consumed in the kb but not registered there:",
              file=sys.stderr)
        print("\n".join(failures), file=sys.stderr)
        print("\nAn unwired seam does not fail, it answers -- plausibly and locally. Register it"
              "\nin kb_module_stage_adapters.c, or delete the seam.", file=sys.stderr)
        return 1

    print(f"check-provider-seams: ok ({len(KB_STORE_CONSUMED_SEAMS)} kb-consumed seam(s) registered; retired fact policy absent)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
