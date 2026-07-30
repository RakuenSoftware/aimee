"""Reconstruct the production Tier-A memory_facts prompt, verbatim.

Source of truth: MF_SYSTEM_PROMPT_TMPL in src/kb/kb_memory_facts.c:52, with the
relation list built by mf_build_system_prompt() from the seed ontology in
src/rel_types.c. verify_against_source() re-derives both from the C sources so
this file cannot silently drift from what production sends.
"""

import json
import pathlib
import re

REPO = pathlib.Path(__file__).resolve().parents[3]

# Verbatim from MF_SYSTEM_PROMPT_TMPL; %s is the canonical relation list.
TEMPLATE = (
    "You extract durable facts from a single remembered note. Return ONLY a JSON "
    'object: {"facts":[{"subject":"","relation":"","object":"","confidence":0.0}]}. '
    "Each fact is a stable subject-relation-object triple grounded strictly in the "
    "note. For relation, choose the single nearest fit from these canonical "
    "predicates when one reasonably applies: %s. If NONE fits, emit a concise "
    "snake_case predicate of your own (e.g. drives, founded, mentors) — NEVER a "
    'generic catch-all such as "other"/"unknown"/"misc". subject is the entity the '
    'fact is about (use "user" for the note\'s author when it is first-person). '
    "confidence is 0..1. Extract only durable, generalizable facts; skip transient "
    "state, feelings, plans, and one-off events. If the note asserts no durable "
    "fact, return an empty list. No prose, no markdown."
)

# Production caps the completion at MF_LLM_OUT_CAP.
MAX_NEW_TOKENS = 8192


def seed_relations():
    """Parse the seed ontology order out of rel_types.c — same order the C builder emits."""
    src = (REPO / "src" / "rel_types.c").read_text()
    body = src[src.index("SEED_ONTOLOGY[] = {"):]
    return re.findall(r'^\s*\{"([a-z_]+)"', body, re.M)


def system_prompt():
    return TEMPLATE % ", ".join(seed_relations())


def user_message(note: str) -> str:
    """Production sends the note as a JSON object: cJSON_AddStringToObject(req,"content",...)."""
    return json.dumps({"content": note}, ensure_ascii=False)


def verify_against_source():
    """Fail loudly if the C prompt text no longer matches this reconstruction."""
    src = (REPO / "src" / "kb" / "kb_memory_facts.c").read_text()
    block = src[src.index("#define MF_SYSTEM_PROMPT_TMPL"):src.index("/* Build the extraction system prompt")]
    # Concatenate the C string literals: take quoted runs, unescape.
    lits = re.findall(r'"((?:[^"\\]|\\.)*)"', block)
    joined = "".join(lits)
    joined = joined.replace('\\"', '"').replace("\\\\", "\\")
    if joined != TEMPLATE:
        raise SystemExit(
            "prompt drift: MF_SYSTEM_PROMPT_TMPL no longer matches prompt.py\n"
            f"--- C source ---\n{joined!r}\n--- harness ---\n{TEMPLATE!r}"
        )
    return True


if __name__ == "__main__":
    verify_against_source()
    print("prompt matches C source")
    print(f"relations ({len(seed_relations())}): {', '.join(seed_relations())}")
    print("---")
    print(system_prompt())
