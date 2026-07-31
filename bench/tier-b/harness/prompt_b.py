"""Reconstruct the production Tier-B synthesize contract, verbatim.

Source of truth: CURATOR_SYNTH_SYSTEM_PROMPT and synth_build_request() in
src/modules/kb-synthesis/kb_curator_synthesize.c. verify_against_source()
re-derives the prompt from the C so this cannot silently drift — the same guard
the Tier-A harness uses, which caught a real drift there.
"""

import json
import pathlib
import re

REPO = pathlib.Path(__file__).resolve().parents[3]
SRC = REPO / "src" / "modules" / "kb-synthesis" / "kb_curator_synthesize.c"

# Verbatim from CURATOR_SYNTH_SYSTEM_PROMPT.
SYSTEM_PROMPT = (
    "You are a knowledge-base curator. Given a topic and its source excerpts as "
    "JSON, write a faithful synthesis grounded only in those sources. Respond with "
    'a single JSON object: {"synthesis": "<text>"}. Do not invent facts.'
)

# CURATOR_SYNTH_DEFAULT_K — how many source artifacts are inlined per call.
DEFAULT_K = 8


def user_message(topic_id, topic_name, sources):
    """Mirror synth_build_request(): {task, topic:{id,name}, sources:[{id,kind,payload}]}."""
    return json.dumps({
        "task": "synthesize_topic",
        "topic": {"id": topic_id, "name": topic_name},
        "sources": [{"id": s["id"], "kind": s["kind"], "payload": s["payload"]}
                    for s in sources],
    }, ensure_ascii=False)


def verify_against_source():
    """Fail loudly if the C prompt no longer matches this reconstruction."""
    src = SRC.read_text()
    block = src[src.index("#define CURATOR_SYNTH_SYSTEM_PROMPT"):]
    block = block[:block.index("\n\n")]
    lits = re.findall(r'"((?:[^"\\]|\\.)*)"', block)
    joined = "".join(lits).replace('\\"', '"').replace("\\\\", "\\")
    if joined != SYSTEM_PROMPT:
        raise SystemExit(
            "prompt drift: CURATOR_SYNTH_SYSTEM_PROMPT no longer matches prompt_b.py\n"
            f"--- C source ---\n{joined!r}\n--- harness ---\n{SYSTEM_PROMPT!r}")

    k = re.search(r"#define CURATOR_SYNTH_DEFAULT_K\s+(\d+)", src)
    if k and int(k.group(1)) != DEFAULT_K:
        raise SystemExit(f"CURATOR_SYNTH_DEFAULT_K is {k.group(1)}, harness assumes {DEFAULT_K}")
    return True


if __name__ == "__main__":
    verify_against_source()
    print("prompt matches C source")
    print(f"K = {DEFAULT_K}")
    print("---")
    print(SYSTEM_PROMPT)
