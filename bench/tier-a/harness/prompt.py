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
    "state, feelings, plans, and one-off events. If the note RETRACTS or DENIES "
    'something ("no longer", "did not", "never", "is not", "has left", '
    '"was removed"), do NOT emit the negated fact - a retraction asserts a fact '
    "is FALSE, so there is nothing durable to record. "
    'If the note asserts no durable fact, return exactly {"facts":[]} - the '
    "wrapper object is ALWAYS required, never a bare []. No prose, no markdown."
)

# Bump when TEMPLATE changes. Results taken under different prompt versions are
# NOT comparable: the prompt is an input to the system under test, so changing it
# changes what is being measured. Every result file should record which version
# produced it.
#
#   v1  original
#   v3  the empty case is spelled out as {"facts":[]}. v1 and v2 both said
#       "return an empty list" while the schema above showed a wrapper object,
#       and models did what the sentence said: E2B UD-Q6 wrote a bare [] on 297
#       of 1000 notes. mf_commit_facts looks for the first '{' and finds none, so
#       every one of those was discarded. 184 were correct abstentions that cost
#       nothing, but 113 were notes carrying real facts. The prompt asked for the
#       wrong thing and the model complied.
#   v2  explicit retraction/negation guidance. The v1 prompt named transient
#       state, feelings, plans and one-off events as out of scope but said
#       nothing about retractions, so "Ingrid is no longer on the renewals desk"
#       produced a member_of triple — 51 spurious triples in the negation slice
#       of the 1k small-corpus run, the graph-poisoning case that slice exists
#       to catch.
PROMPT_VERSION = "v3"

# Production caps the completion at MF_LLM_OUT_CAP.
MAX_NEW_TOKENS = 8192


KIND_WORD = {
    "NODE_PERSON": "person", "NODE_ORG": "org", "NODE_DEVICE": "device",
    "NODE_IP": "ip", "NODE_PLACE": "place", "NODE_SCALAR": "value",
    "NODE_CONCEPT": "concept", "NODE_EVENT": "event", "NODE_TIME_EXPR": "time",
    "NODE_OTHER": "any",
}


def _seed_body():
    """The SEED_ONTOLOGY initialiser only.

    Bounded at its closing brace: rel_types.c also holds SEED_ALIASES, whose
    entries look identical to a regex scanning for {"name". Reading to EOF
    silently pulled aliases in as if they were relations."""
    src = (REPO / "src" / "rel_types.c").read_text()
    start = src.index("SEED_ONTOLOGY[] = {")
    end = src.index("\n};", start)
    return src[start:end]


def seed_relations():
    """Parse the seed ontology order out of rel_types.c — same order the C builder emits."""
    return re.findall(r'^\s*\{"([a-z_]+)"', _seed_body(), re.M)


def _kinds_text(raw):
    kinds = re.findall(r"NODE_[A-Z_]+", raw)
    if not kinds or "NODE_OTHER" in kinds:
        return "any"
    return "|".join(KIND_WORD.get(k, "any") for k in kinds)


def seed_descriptors():
    """Mirror rel_types_describe(): "name (head->tail)" per seed relation.

    The prompt used to send bare names, which made the model guess our naming
    convention; the type signature was in the ontology all along."""
    body = _seed_body()
    out = []
    for m in re.finditer(
        r'\{"([a-z_]+)",\s*(\{[^}]*\}),\s*\d+,\s*(\{[^}]*\}),\s*\d+,', body):
        out.append(f"{m.group(1)} ({_kinds_text(m.group(2))}->{_kinds_text(m.group(3))})")
    return out


def seed_aliases():
    """Mirror SEED_ALIASES in rel_types.c: alias -> canonical seed relation.

    Production folds these in rel_type_canonicalize() before a triple reaches the
    gate, so the benchmark has to as well or it scores a system we no longer run."""
    src = (REPO / "src" / "rel_types.c").read_text()
    start = src.index("SEED_ALIASES[] = {")
    end = src.index("\n};", start)
    return dict(re.findall(r'\{"([a-z_]+)",\s*"([a-z_]+)"\}', src[start:end]))


def canonicalize_relation(rel):
    """Python twin of rel_type_canonicalize(): normalize, leave a real seed type
    alone, otherwise fold a known alias. Unknown labels pass through."""
    norm = re.sub(r"[^a-z0-9]+", "_", str(rel or "").casefold()).strip("_")
    if norm in set(seed_relations()):
        return norm
    return seed_aliases().get(norm, norm)


def symmetric_relations():
    """Relations the ontology declares symmetric (is_symmetric, field 6 of
    rel_type_def_t). For these the ontology states that one assertion implies
    both directions, so scoring must accept either argument order — the C
    comment is explicit: "one assertion implies both directions"."""
    body = _seed_body()
    out = set()
    for m in re.finditer(
        r'\{"([a-z_]+)",\s*\{[^}]*\},\s*\d+,\s*\{[^}]*\},\s*\d+,\s*(\d+)', body):
        if m.group(2) == "1":
            out.add(m.group(1))
    return out


def inverse_relations():
    """Map rel_type -> inverse_rel_type from the seed ontology (field 7).

    The header calls these "auto-enforced": asserting one direction commits the
    other, so both spellings are the same fact."""
    body = _seed_body()
    out = {}
    for m in re.finditer(
        r'\{"([a-z_]+)",\s*\{[^}]*\},\s*\d+,\s*\{[^}]*\},\s*\d+,\s*\d+,\s*(NULL|"([a-z_]+)")',
        body):
        if m.group(3):
            out[m.group(1)] = m.group(3)
    return out


def system_prompt():
    return TEMPLATE % ", ".join(seed_relations())


def system_prompt_with_signatures():
    """REJECTED EXPERIMENT, kept so the negative result stays reproducible.

    Sends "device_has_ip (device->ip)" instead of the bare name, on the theory
    that the model was guessing our naming convention. Benchmarked in
    results/promptfix: it regressed four of five models and collapsed
    Qwen3.5-0.8B from 35 triples to 13. Production sends bare names; the naming
    problem is handled by rel_type_canonicalize() instead.""" 
    return TEMPLATE % ", ".join(seed_descriptors())


def system_prompt_no_confidence():
    """ABLATION: drop the confidence field from the contract entirely.

    The field is requested, copied as the literal 0.0 by most models, and then
    used to discard their work. If it is not trustworthy enough to gate on, it
    may not be worth asking for — removing it shortens the prompt, removes the
    literal models imitate, and saves output tokens on every call in the highest
    volume LLM path in the KB."""
    base = system_prompt()
    out = base.replace('{"subject":"","relation":"","object":"","confidence":0.0}',
                       '{"subject":"","relation":"","object":""}')
    out = out.replace(" confidence is 0..1.", "")
    if out == base:
        raise SystemExit("ablation no-op: the confidence literal moved")
    return out


def system_prompt_conf_fixed():
    """Ablation: the production prompt with one literal changed.

    The schema example in MF_SYSTEM_PROMPT_TMPL contains "confidence":0.0. Small
    models copy that literal into every fact they emit, and MF_CONF_FLOOR (0.6)
    then discards the lot — so the drain commits nothing even when extraction was
    correct. This variant changes only that example value to 0.9 and is otherwise
    byte-identical, to test whether the failure is the model or the prompt.

    This is NOT what production sends. Results using it are labelled as an
    ablation.
    """
    base = system_prompt()
    fixed = base.replace('"confidence":0.0}', '"confidence":0.9}')
    if fixed == base:
        raise SystemExit("ablation no-op: the confidence literal moved; re-check the template")
    return fixed


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
