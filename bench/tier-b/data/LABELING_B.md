# Tier-B synthesize gold: what is measured and why

The task under test is `KB_CURATOR_STAGE_SYNTHESIZE`, the highest-value Tier-B
call: a topic plus `CURATOR_SYNTH_DEFAULT_K = 8` source artifacts in, one
grounded paragraph out. The contract is `CURATOR_SYNTH_SYSTEM_PROMPT` and
`synth_build_request()` in `kb_curator_synthesize.c`, reconstructed verbatim by
`harness/prompt_b.py`, which fails the run if the C drifts.

## Why this is scored differently from Tier-A

Tier-A emits triples, so it can be scored by matching. Tier-B emits prose, and
there is no single correct paragraph. Scoring free text against a reference
answer measures style as much as substance, and the Tier-A experience, nine
defects, most of them the grader punishing a correct answer phrased differently,
argues strongly against building another matcher.

So nothing here is matched against a reference synthesis. Three properties are
measured instead, each objective and each derived from the prompt's own
instructions:

| property | what the prompt demands | how it is measured |
|---|---|---|
| **format** | `{"synthesis": "<text>"}` | production's own parse: first `{` to last `}`, `synthesis` or `text` key, non-empty string |
| **faithfulness** | "grounded only in those sources", "Do not invent facts" | every capitalised entity, number and identifier in the output must appear in the sources |
| **coverage** | "a faithful synthesis" of the sources | required facts, each with accepted surface forms, must be present |

Faithfulness is the one that matters most. This output is written to the
artifact store as a `synthesis` artifact and cited; an invented fact there is
durable and carries citations that appear to support it.

## Categories

| code | probes |
|---|---|
| `factual` | ordinary synthesis over consistent sources |
| `conflicting` | two sources disagree; a faithful synthesis reports the disagreement rather than silently picking one |
| `thin` | sources carry little; restraint is correct, padding is not |
| `distractor` | sources include material only loosely related to the topic |
| `numeric` | figures that must survive intact: the easiest thing to hallucinate |

## Faithfulness: what counts as invented

An entity, number or identifier in the synthesis that appears nowhere in the
sources. Deliberately narrow:

- Common words and connectives are ignored; only capitalised tokens, digits and
  identifier-shaped strings are checked.
- Sentence-initial capitalisation is not treated as an entity.
- Restating a source's own wording is not invention.

This will not catch a subtler failure, a true entity related in a way the
sources never claim. That limit is real and is why `conflicting` exists as a
category: it catches the specific case where a model resolves a contradiction by
asserting one side.

## Coverage: accepted forms

Each required fact lists surface forms that count as covering it, because there
is no single right phrasing. The list is generous on wording and strict on
substance, `"Rakuen Software"` and `"Rakuen"` both cover the employer fact,
`"a software company"` does not.

## Known limits

- **Hand-authored, single author.** Same weakness as the Tier-A set, and the
  consensus check that validated that one (a label no model produces is probably
  wrong) applies here too and should be run once there are results.
- **Small.** Treat differences between close models as noise.
- **Proxy metrics.** Faithfulness by entity-tracing is a proxy for groundedness,
  not a judgement of it. It catches invented names and numbers, which is the
  failure that matters for a cited artifact, and misses invented *relationships*.
- **No fluency or usefulness score.** A synthesis can pass all three checks and
  still read badly. That is not measured here and should not be claimed.
