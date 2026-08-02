# v5 = v4 with defect 33 corrected

Derived from v4 rather than regenerated, and that is deliberate.

## Why derived

The v4 corpus **cannot be reproduced**. `generate_gold.py` is seeded and
deterministic, and the README says so, but its `--inventory` and `--synth`
inputs were never tracked in git — they lived in a scratch directory. Four
surviving inventory files were tried against the recorded seed (20260802) and
none reproduce v4: 0 of 1001 notes match on text.

So regenerating would not have produced "v4 with the fix", it would have
produced an unrelated corpus, and every v4 result would have become
incomparable for a reason unrelated to the defect being fixed.

Deriving instead makes the v4 -> v5 delta exactly the fix:

| tier | triples relabelled | note-text diffs | id diffs | unexpected gold diffs |
|---|---:|---:|---:|---:|
| small | 28 | 0 | 0 | 0 |
| mid | 83 | 0 | 0 | 0 |
| large | 257 | 0 | 0 | 0 |

## The change

`code.infra.2` phrases a deployment as "{service} runs on {host}" and labelled
it `has_hostname`. Split by phrasing, gemma-4-E4B is 23/23 correct on the "has
hostname" wording and 0/28 on this one, identically across two runs — it answers
`runs_on`, which is what the sentence says. See MEASUREMENT_LOG.md defect 33.

`runs_on` became a seed relation in the same batch of work, so the label is now
expressible. Equivalent to what the corrected template in `data/templates.py`
emits: same subject, same object, relation `has_hostname` -> `runs_on`.

## Ids ARE stable v4 -> v5

Unlike every earlier version bump. `data/corpora/README.md` warns that
regenerating shuffles and renumbers, so scoring a v1 prediction against v2 gold
silently produces nonsense. That warning does not apply here: v5 keeps v4's ids
AND its note text, so a v4 prediction file can be scored against v5 gold and the
difference is only the corrected label.

## Reproducibility debt

Generating a genuinely new corpus is currently impossible without re-mining the
source repos, which would produce different entities. The inputs need to be
tracked (or their provenance recorded) before the next corpus version.
