#!/usr/bin/env python3
"""A provider seam must be registered in the daemon that CONSUMES it.

A seam is a `*_register_*()` entry point: the module process supplies the real
decision, and the C side falls back to a local implementation when nobody
registers one. That fallback is the hazard -- a gate answering from a hardcoded
cue list looks exactly like a gate that works.

Registering it *somewhere* is not enough, and that is the trap this exists for.
aimee-server registered the §7 PII providers and calls RETRIEVE, but nothing in
the server invokes the gate: its only callers are db2 code, which links into
aimee-kb and nothing else, and the kb registered nothing. The module never
decided anything, while a global "is this seam wired?" check looked green.

The seam -> consumer mapping is written out rather than inferred. Deriving it
needs a real C parser, and an approximate one produced confident nonsense (it
matched single-letter "functions" and reported delegate seams as db2 consumers).
A short explicit table that a reviewer can check against the headers is worth
more than a clever extractor that is wrong in ways nobody notices.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / "src"

# seam -> the consumer whose answer changes when the seam is unwired.
# Every entry here is consumed by db2 code, which links only into aimee-kb, so
# each must be registered by a kb source.
DB2_CONSUMED_SEAMS = {
    "memory_pii_register_turn_classifier": "memory_pii_turn_requests_sensitive",
    "memory_pii_register_sensitivity_batch": "memory_pii_rel_sensitivity_batch",
}

KB = SRC / "kb"
DB2 = SRC / "modules/db2"


def is_test(path: pathlib.Path) -> bool:
    return "tests" in path.parts or path.name.startswith("test_")


def text_of(root: pathlib.Path) -> str:
    return "\n".join(p.read_text(encoding="utf-8", errors="replace")
                     for p in root.rglob("*.c") if not is_test(p))


def main() -> int:
    kb_text = text_of(KB)
    db2_text = text_of(DB2)

    failures = []
    for seam, consumer in sorted(DB2_CONSUMED_SEAMS.items()):
        if not re.search(rf"\b{re.escape(consumer)}\s*\(", db2_text):
            failures.append(f"  {seam}: db2 no longer calls {consumer}(); "
                            f"drop this entry or point it at the new consumer")
            continue
        if not re.search(rf"\b{re.escape(seam)}\s*\(", kb_text):
            failures.append(f"  {seam}\n"
                            f"      db2 calls {consumer}(), and db2 runs in the kb\n"
                            f"      but no kb source registers it -- the gate will answer from"
                            f" its local implementation")

    if failures:
        print("check-provider-seams: provider seams consumed in the kb but not registered there:",
              file=sys.stderr)
        print("\n".join(failures), file=sys.stderr)
        print("\nAn unwired seam does not fail, it answers -- plausibly and locally. Register it"
              "\nin kb_module_stage_adapters.c, or delete the seam.", file=sys.stderr)
        return 1

    print(f"check-provider-seams: ok ({len(DB2_CONSUMED_SEAMS)} kb-consumed seam(s) registered)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
