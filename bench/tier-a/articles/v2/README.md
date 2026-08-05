# The slate: ten articles

Decided, not proposed. The count is set by how many distinct claims are worth a
reader's time, not by how the evidence partitions: the same measurement appears
in several pieces doing different work, and nothing is spent by using it twice.

Written against `/home/virant/dev/voice-guide/VOICE.md` (Parts I and III).
The previous six drafts remain untouched in `articles/` as source material and
provenance. They are not superseded, deleted, or to be edited.

## Order

| # | article | claim |
|---|---|---|
| 00 | Eighteen arms, one corpus, and the column that decides it is not F1 | the head-to-head; F1 and restraint are independent axes |
| 01 | Speculative decoding doubled throughput and cost nothing I could measure | a precise null, measured wrong twice first |
| 02 | Which quant beats how many bits | replication beats sample size; the quant is not a direction |
| 03 | My benchmark lied to me six times | the discarded column is the one that catches you |
| 04 | Every configuration reproduces itself exactly and no two agree | repeatable is the property you need; identical is not |
| 05 | Eight ways a run scores fine and is broken | what one column does not carry |
| 06 | One sentence in my prompt turned a model's reasoning off | reasoning is a property of the run |
| 07 | The parallelism limit was never VRAM | a per-process default nobody set |
| 08 | The corpus decides what your benchmark can find | the corpus is the experiment |
| 09 | I built a benchmark to rank models and it audited my production system | a scorer is a specification nobody reviews |

00 leads because it is what the project was built to produce and the only piece a
reader can act on without caring how it was measured. Everything after it earns
that table.

## What was cut, and why

**"How small can an extractor be" is dissolved.** Its ranking rested on n=69, and
the two arguments it carried have better homes: the field table is 00, and "the
quant matters more than the size" is 02's spine. What was left was a question
that 00 answers directly. Its one surviving original detail, the two-ways-to-
score-zero result, moved into 05.

## What every piece carries

The corpus-independence limit. Every ladder, null and ranking here ran on one
corpus from one pipeline with one generator model, so a measured effect and a
generator artefact are indistinguishable from inside. 02 and 08 state it against
their own conclusions. It is the largest open item in the work and no amount of
GPU time closes it; see `harness/second_corpus_plan.md`.

## Known gaps, per piece

- **00** every ordering claim needs its per-pair interval; the sweep is
  `harness/h2h_intervals.py`.
- **01** the sixth pair (E4B Q8) is still running; five of six pairs still need
  their own bootstrap.
- **04** defect 40's cache explanation is refuted and withdrawn; the third
  hypothesis (sequence position) has a registered test in flight.
- **06** the +0.116 relation-agnostic figure has no interval, because the
  bootstrap tool scores strict F1 only. It is the largest unbounded number here.
- **09** the polarity result is one error in 1,738 from two models on one corpus.
