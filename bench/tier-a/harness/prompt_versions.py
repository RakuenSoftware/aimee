"""Render an older prompt version from the live template, by explicit reversal.

A v5-vs-v7 comparison needs both prompts on the same corpus and the same server,
but prompt.py deliberately holds exactly one template -- the one production
sends -- and verifies it byte-for-byte against the C source. Keeping a second
copy of v5 here would defeat that check the moment either drifted.

So older versions are DERIVED from the live one by named substitutions, and each
substitution asserts it matched. If the live wording moves, this raises instead
of silently producing a prompt that is neither version.
"""

import prompt

# v6/v7 replaced these two spans. Both are reversed to recover v5.
SCHEMA_LIVE = ('{"facts":[{"subject":"","relation":"","object":"","confidence":0.0,'
               '"negated":false}]}')
SCHEMA_V5 = '{"facts":[{"subject":"","relation":"","object":"","confidence":0.0}]}'

RETRACT_LIVE_START = "emit the ORIGINAL fact it retracts"
RETRACT_LIVE_END = 'omit "negated" or set it false. '
RETRACT_V5 = ("do NOT emit the negated fact - a retraction asserts a fact is FALSE, "
              "so there is nothing durable to record. ")

# v7 added only this sentence on top of v6.
RENAME_V7 = ('A RENAME is NOT a retraction: "A is now called B" means A and B are '
             "the same thing, so emit also_known_as with negated FALSE. ")


def _require(hay, needle, what):
    if needle not in hay:
        raise SystemExit(f"prompt_versions: {what} no longer matches the live template.\n"
                         f"  looked for: {needle[:80]!r}")
    return hay


def live():
    prompt.verify_against_source()
    return prompt.system_prompt()


def v5(text=None):
    """The pre-polarity prompt: reasoning granted, retraction discarded."""
    s = text if text is not None else live()
    _require(s, SCHEMA_LIVE, "schema line")
    s = s.replace(SCHEMA_LIVE, SCHEMA_V5, 1)
    _require(s, RETRACT_LIVE_START, "retraction guidance")
    i = s.index(RETRACT_LIVE_START)
    j = s.index(RETRACT_LIVE_END, i) + len(RETRACT_LIVE_END)
    return s[:i] + RETRACT_V5 + s[j:]


def v6(text=None):
    """v7 minus the rename sentence."""
    s = text if text is not None else live()
    _require(s, RENAME_V7, "rename sentence")
    return s.replace(RENAME_V7, "", 1)


# Diagnostic variants for the reasoning question, added deliberately and named
# so no arm can inherit one by accident.
#
# THE QUESTION. Seven of fourteen models in the head-to-head emit no reasoning
# pass at all under the live prompt. That is either the model or the prompt, and
# the project has already been burned once by assuming the former: on gemma-4-E4B
# a single sentence, "No prose, no markdown.", suppressed reasoning across 10,000
# notes while every row recorded thinking:true, and restoring it was worth +0.116
# relation-agnostic recall. E2B, same family, never had the behaviour.
#
# WHAT THESE ARE FOR, and what they are NOT for. Both produce arms that answer
# "does this prompt suppress this model's reasoning channel". Neither produces a
# number comparable to the ranking table: a model scored under a different prompt
# is a different configuration, exactly as a different process count is. Every
# prediction row records its own prompt_version, so nothing banked is touched and
# nothing can be silently mixed.
FINAL_LIVE = ("Reason first if it helps; the answer that follows must be the JSON "
              "object only, no prose, no markdown.")
FINAL_V4 = "No prose, no markdown."


def v4clause(text=None):
    """POSITIVE CONTROL. Restores the wording that provably suppressed E4B.

    This variant must reproduce the known failure or the experiment is not
    measuring what it claims: run gemma-4-E4B under it and reasoning should
    collapse to ~0. If it does not, the variant is wrong, not the models.
    """
    s = text if text is not None else live()
    _require(s, FINAL_LIVE, "final output-format sentence")
    return s.replace(FINAL_LIVE, FINAL_V4, 1)


def noclause(text=None):
    """THE TEST. Removes the output-format constraint entirely.

    A model that reasons here and not under the live prompt is being suppressed
    by the prompt. A model that reasons under neither is not a reasoning model in
    this harness, and its place in the ranking is a capability result rather than
    a configuration artefact.

    Parse rate is expected to fall: the clause is doing real work, and removing it
    brought back fenced ```json output on 14 of 20 notes when this was measured on
    E4B. That is why this is a diagnostic and not a candidate prompt.
    """
    s = text if text is not None else live()
    _require(s, FINAL_LIVE, "final output-format sentence")
    return s.replace(FINAL_LIVE, "", 1).rstrip() + "\n"


VERSIONS = {"v5": v5, "v6": v6, "v4clause": v4clause, "noclause": noclause}


def render(version):
    """`version` may be any key in VERSIONS, or 'live' for the shipped prompt."""
    if version == "live":
        return live()
    if version not in VERSIONS:
        raise SystemExit(f"unknown version {version!r}; have live, {', '.join(VERSIONS)}")
    return VERSIONS[version]()


if __name__ == "__main__":
    import sys
    v = sys.argv[1] if len(sys.argv) > 1 else "live"
    s = render(v)
    print(f"--- {v} ({len(s.encode())} bytes) ---")
    print(s)
