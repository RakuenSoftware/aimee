# Measurement log: every scoring bug, and how it was found

A running record of the defects found in this benchmark's **own instrumentation**,
kept because it is the most transferable thing here. The models behaved roughly
as expected throughout. The grader did not.

Nine defects. Every one inflated the apparent failure rate, several inverted a
conclusion, and none were in the models.

---

## The pattern

Eight of the nine share one shape: **the metric punished a correct answer
expressed differently from the label.** Once that pattern was named it became a
search strategy — look for places where "different" was being scored as "wrong" —
and it kept paying out.

The ninth (incomplete gold) is worse and subtler: the metric punished answers
that were correct and *not labelled at all*, which penalises the models that
extract most. That one biases the ranking rather than just depressing it.

---

## 1. Symmetric relations scored as wrong

**Found by** hand-reading E4B's disagreements after its score looked low.

`rel_type_def_t` carries `is_symmetric`, and the C comment states "one assertion
implies both directions". `knows` and `spouse` are symmetric, so `(sarah, spouse,
user)` is correct — but the scorer charged it twice, as a false positive *and* a
false negative.

**Impact** E4B 0.623 → 0.656. **Lesson** the ontology already encoded the answer;
the scorer just wasn't reading it.

## 2. Inverse relations scored as wrong

**Found by** suspecting the same class of bug and grepping the ontology for other
metadata the scorer ignored.

`inverse_rel_type` is documented "auto-enforced": asserting `(a parent_of b)`
commits `(b child_of a)`. The scorer treated the two directions as different
facts.

**Impact** small alone, but it confirmed the pattern was systematic.

## 3. Number words scored as wrong

`"Nina is seven"` → a model writing `7` and a label saying `seven` are the same
scalar to the ontology. Scored as a miss.

## 4. Fabrication metric raising false alarms

**Found by** validating a brand-new metric before trusting it — the one time
checking happened *before* publishing rather than after.

The grounding check flagged `kb_server` against "KB server", `7` against
"seven", and `me` as invented entities. **Qwen3.5-0.8B appeared to fabricate
11.4% of its triples; the true figure is 0.0%.**

**Lesson** a new metric deserves the same suspicion as a new model.

## 5. Schema validity — the one that inverted a conclusion

**Found by** an observation from outside: schema validity fell monotonically with
model size (1.00 → 0.96 → 0.84 → 0.77 across E4B, 12B, 26B, 35B), which is a
strange thing for bigger models to be worse at.

All 30 "schema failures" across all four models were the literal string `{}` or
`[]`, on notes asserting no durable fact. **Zero were malformed.** The models
were abstaining correctly and saying so tersely; the runner recorded anything
without a `facts` array as a failure. Production agreed with the models, not the
metric — `mf_commit_facts()` commits nothing either way.

**Impact** all four models are 1.00. It also excluded those notes from the
abstention denominator, understating abstention for exactly the models that
abstained most.

**What it cost** an entire published finding. A "dense models are more
disciplined than MoE" result, complete with a plausible mechanism (per-token
expert routing degrading format adherence), was **retracted**. The mechanism was
a story fitted to a bug.

## 6. Entity normalisation

`kb_server` ≠ `KB server`, `aimee-kb` ≠ `aimee kb`, `Dr. Okafor` ≠ `Dr Okafor`.
Endpoints had to match exactly under a normaliser that preserved underscores and
hyphens. Models write snake_case constantly.

**Impact** narrow but real: only Qwen3.5-0.8B moved, +0.040.

## 7. Incomplete gold — the largest single error

**Found by** auditing all 220 distinct false positives across 20 models, after
being told that finding one bug this way justified checking the rest.

Notes contained durable facts that were never labelled, so correct extractions
scored as false positives. `"That box's hostname is aimee-retrieval-test"` was
labelled EMPTY and **nine independent models** extracted the hostname. The models
were right.

**Impact** +0.02 to +0.10 F1 for every model. The incumbent went 0.705 → 0.754.

**Why it is the worst kind** it is *correlated with model quality*. Better models
extract more, so they collect more unlabelled-but-correct triples, so they are
penalised more. A metric that is merely noisy is survivable; one that is biased
against the thing you are trying to measure is not.

**Fix** seven facts promoted to required gold (64 → 72 triples). Where two
renderings are equally correct — naming a device by description rather than
hostname — an `alt` attaches to the specific gold triple and scores as a true
positive, rather than being excused from the denominator.

## 8. Equivalent predicates

`speaks` vs `speaks_language`, `attends` vs `member_of`, `daughter` vs
`child_of` — each cost a model both a false positive and a false negative for a
naming choice carrying no information.

Handled by an explicit equivalence table, kept deliberately narrow: `studied_at`
is **not** equivalent to `studied`, because "studied medicine" and "studied at
Otago" are different facts. Bare kinship terms were also missing from the
*production* alias table, so this one found a real product defect too.

## 9. Object containment

`"2 of the junior engineers"` for `"junior engineers"`, `"proxmox host in the
auckland rack"` for `"auckland rack"` — token-F1 0.5 and 0.571, just under the
0.6 threshold, despite the gold being fully *contained* in the prediction.

---

## Checks that came back clean

Worth recording, because "we looked and found nothing" is evidence too:

- **Greedy vs optimal matching.** Greedy 1-1 assignment could in principle
  undercount when one prediction is the only match for two gold triples. Checked
  against maximum bipartite matching for every model: **identical on all**.
- **Arithmetic invariants.** `tp+fn` equals the gold total and `tp+fp` equals the
  prediction total for every model.
- **False negatives.** Audited the same way as false positives. Unlike the FPs,
  these were overwhelmingly *genuine model errors* — `lives_in` where the note
  said `born_in`, `works_for` for colleagues. One gold fix came out of it.

## Guards now in place

Each defect left behind something that fails loudly rather than a promise to be
careful:

| Guard | Prevents |
|---|---|
| `prompt.verify_against_source()` | benchmark prompt drifting from the C source |
| `score.py` row-count check | a partial run scoring as a bad result (a 1-note remnant once scored F1 0.031 for a model that scores 0.903) |
| `validate_gold.py` | duplicate triples, blank fields, alternatives identical to their parent |
| `rel_types_self_validate()` | an alias pointing at a non-existent or inactive relation |
| `sync_results.sh` rescoring | the CT's stale scores silently reverting local fixes — this happened **three times** |
| `audit_errors.py`, `predicate_drift.py` | reclassifying every disagreement on demand |

## What this cost, and what it is worth

Five of the nine were found only after a number had been reported. Three
conclusions were retracted: the MoE discipline gap, "over-extraction is inherent
to this prompt", and an 11.4% fabrication rate.

The uncomfortable part is that **every single correction moved in the same
direction** — the models were better than measured, every time. A grader with
unbiased noise would have erred both ways. That it never did is the signature of
systematic error, and it is visible in hindsight in a way it was not in advance.

The practical lesson for anyone building an eval: the failure mode is not models
behaving unexpectedly. It is the grader being wrong in ways that correlate with
model quality — and a benchmark cannot detect that about itself. Every defect
here was found by reading raw outputs or by an outside observation that a number
looked strange. None were found by the aggregate metrics, which looked entirely
plausible throughout.
