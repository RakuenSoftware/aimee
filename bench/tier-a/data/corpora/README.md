# Corpus versions

A corpus is an input to the system under test. Changing it changes what is being
measured, so versions are kept side by side rather than overwritten, and every
result records which one produced it.

| version | prompt | notes |
| --- | --- | --- |
| v1 | prompt v1 | first generated corpus. `novel_pred` is unusable: see below. |
| v2 | prompt v2 | `novel_pred` rebuilt; retraction guidance added to the prompt. |

**Note ids are NOT stable across versions.** Regenerating shuffles and renumbers,
so `g000123` in v1 and v2 are different notes. Zero of 1000 ids kept their text
between v1 and v2. Scoring a v1 prediction file against v2 gold silently produces
nonsense. The ids line up and the notes do not.

That nearly happened here: v2 was generated while a Q4 run against v1 was still
in flight. The run was saved because the file was versioned rather than replaced.
The rule this produced: **never regenerate into the path a running job is
reading.** Write a new version and switch deliberately.

## What changed in v2

`novel_pred` in v1 asked for predicates a seed relation could reasonably cover,
`technical_contact_for` for "X is the technical contact for Y". The prompt tells
the model to prefer a canonical predicate "when one reasonably applies" and to
mint only "if NONE fits", so a model obeying the prompt answers `works_for`, and
E2B did: 7 times `works_for`, 5 times `has_role`, cell F1 0.000 across 40 gold
triples. The model was right; the gold was wrong. The hand-authored set scores
E2B 0.571 on the same category because its predicates (drives, founded, mentors)
have no seed equivalent.

v2 uses predicates with no seed equivalent and attaches `alt` spellings, because
a minted predicate has no canonical form and demanding exactly `renews_on` over
`renewal_date` measures spelling luck rather than extraction.

## Comparability

- v1 results compare with v1 results. v2 with v2.
- Cross-version comparison is invalid on both axes at once here: v2 changed the
  corpus AND the prompt, so a v1-to-v2 difference cannot be attributed to either.
