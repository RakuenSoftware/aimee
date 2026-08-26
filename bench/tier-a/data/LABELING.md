# Tier-A gold set: labeling rules

The task under test is the **`memory_facts` drain**: the highest-volume Tier-A
call in the KB. One remembered note in, subject-relation-object triples out.
The contract is `MF_SYSTEM_PROMPT_TMPL` in `src/kb/kb_memory_facts.c:52`, the
gold labels below follow that prompt exactly, not a paraphrase of it.

## Contract the labels encode

- Output shape `{"facts":[{"subject","relation","object","confidence"}]}`.
- `relation` is the **single nearest fit** from the 17 seed predicates in
  `src/rel_types.c` when one reasonably applies; otherwise a concise snake_case
  predicate of the model's own. Never a generic catch-all.
- `subject` is `"user"` when the note is first-person about its author.
- Only **durable, generalizable** facts. Transient state, feelings, plans, and
  one-off events are explicitly out of scope.
- A note asserting no durable fact must yield an **empty list**.

Seed predicates: `works_for member_of has_role spouse knows parent_of child_of
lives_in born_in located_in device_has_ip has_hostname age also_known_as
supersedes linked_policy decided_by`

## Category taxonomy

Drawn from §4.1 of `docs/proposals/pending/dedicated-extraction-model-curator-tier-a.md`,
which calls for "real memories + synthetic edge cases: first/third person,
multi-fact, implicit, negation, ambiguous".

| Code | Category | What it probes |
|---|---|---|
| `first_person` | first-person simple | the `subject="user"` convention |
| `third_person` | third-person simple | baseline competence |
| `multi_fact` | several facts in one note | recall under multiplicity, no early stop |
| `implicit` | fact requires light inference | recall on non-literal statements |
| `negation` | retraction/correction/denial | precision: must not assert the negated fact |
| `transient` | feelings, plans, one-off events | **precision: gold is the empty list** |
| `ambiguous` | underspecified referent or value | calibration; partial credit allowed |
| `novel_pred` | no seed predicate fits | the §7.2 own-predicate path |
| `infra` | host/IP/network facts | `device_has_ip`, `has_hostname` |
| `governance` | decisions, policies, supersession | `decided_by`, `linked_policy`, `supersedes` |

## Why `transient` matters most

A model that emits triples for everything scores high recall and is useless: the
drain commits into `memory_facts` and the write gate promotes to durable edges.
Over-extraction is the expensive failure. Roughly a fifth of the set has an empty
gold list for exactly this reason, and the report breaks out precision on that
slice separately.

## Scoring

A predicted triple matches a gold triple when subject, relation, and object all
match after normalization (casefold, strip punctuation and leading articles,
collapse whitespace). Two scores are reported:

- **strict**: object must match exactly after normalization.
- **lenient**: object matches on token-set F1 ≥ 0.6, absorbing "Rakuen Software"
  vs "Rakuen Software Ltd". Relation and subject remain exact under both.

`confidence` is not scored. Notes with empty gold contribute to precision only
(any prediction is a false positive) and are excluded from the recall
denominator.

## Provenance and honest limits

Notes are hand-authored for this benchmark, seeded by the shapes visible in the
prompt and ontology. They are **not** sampled from the live `memory_facts`
corpus. I have no access to production memories from this session. That is the
main external-validity limit: the category mix here is my construction, so it
measures relative model capability on a faithful task, not absolute drain
quality. A follow-up pass over real notes is the obvious upgrade, and §4.3 of the
proposal (shadow on live traffic) is where that belongs.

Gold labels were authored by Claude Opus 5. None of the models under test
authored any label.

## Entity naming, and one genuinely ambiguous note

A triple is correct only if **both endpoints name the labelled entity**; only the
predicate may vary (`works_for` vs `member_of`, `born_in` vs `grew_up_in`).
Surface variation (`Dr.` vs `Dr`, case, underscores, hyphens) is absorbed by
normalisation and is not a difference.

This means coreference is NOT forgiven. If a note calls one device both "the
build host" and "forge", gold has to pick one, and a model choosing the other is
marked wrong. That is a real limitation and it lands on `mf04`:

> "The build host is called forge, sits at 192.168.1.42, and lives in the
> Auckland rack."

Gold uses **build host** as the subject throughout, with `forge` as the hostname
value. The justification is consistency with the two sibling notes, `in03`
("The KB server has hostname aimee-kb") and `in04` ("forge is the hostname we use
for the build machine") both use the descriptive entity as subject and the
hostname as the value, and with the sentence's own grammatical subject.

It is not a typing rule. `device_has_ip` is `{NODE_DEVICE} -> {NODE_IP}` and
`forge` is a device, so `forge | device_has_ip | ...` is well-formed. Models that
read it that way are penalised by a labelling choice, not by an error of theirs.
Anyone reproducing this should know `mf04` is the weakest note in the set.
