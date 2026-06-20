# typed_facts + HyDE — overnight before/after (2026-06-20)

Run autonomously against .254 (aimee-combined, app v0.2.128, fence-fixed :latest).
Goal: before/after numbers to decide whether to ship `typed_facts` and `HyDE`
default-on.

## TL;DR

- **typed_facts: 0% → 80% answer accuracy** on a 15-question user-fact corpus
  (extraction coverage 87%). When a fact is in context the model answers it;
  without typed_facts the model has nothing and says "unknown". Recommend
  **default-on**, after fixing the auto-inject wiring bug below.
- **HyDE: not measurable as shipped — it was silently broken on .254** (the
  rewrite command's script was missing from the image, so the rewrite no-op'd
  and HyDE had *zero* effect in production). Fixed the script tonight; verified
  the rewrite now produces correct hypothetical answers. But once active it adds
  a per-query LLM rewrite (+rerank) call that makes semantic recall very slow
  (>120s in probes). Recommend **keep opt-in** until recall lift is measured via
  the native direct eval and the latency cost is addressed.

---

## typed_facts — answer accuracy (real before/after)

Method: ingest 15 natural-language "memories" stating one durable fact each
(mix of first-person + third-party, varied relations: job, location, prefs,
relationships, possessions, study). Let typed_facts extract (LLM extractor via
mistral). For each question: bare turn (OFF) vs same turn with the **recalled
typed-fact block injected** (ON, exactly what ingress would inject). Answers
scored by deterministic substring match against gold. Preinject forced off so
OFF is truly bare and ON isolates the injected block.

| metric | value |
|---|---|
| n | 15 |
| fact-extraction coverage (gold fact present in recall) | **13/15 = 87%** |
| OFF accuracy | **0/15 = 0%** |
| ON accuracy | **12/15 = 80%** |

- The 2 coverage misses ("I live in Portland, Oregon" → city; "training for a
  marathon" → event) were **not extracted** by the LLM extractor — the ceiling
  for ON was 13/15, and it hit 12. The one remaining miss (Caroline's job) *had*
  the fact in context but the model emitted verbose reasoning that got truncated
  before stating the answer — a formatting artifact, not a recall failure.
- OFF is uniformly 0%: with no memory layer the model cannot answer any "what is
  my X / what does <person> do" question. This is the gap typed_facts closes.

Conclusion: typed_facts converts unanswerable personal-fact questions into
correct answers. The dominant residual error is **extraction coverage**, not
recall or injection — i.e. tuning the extractor prompt/floor, not plumbing.

### Blocker found: auto-inject does not fire in live turns

Even with `typed_facts_enabled=true`, `ingress_preinject_enabled=true` (set + a
full container restart), facts in `entity_edges`, and the `memory.facts` RPC
returning them correctly, a live `/v1/messages` turn does **not** surface the
facts ("What does Caroline do?" → "I don't have information about Caroline",
while the RPC returns `works_as: counselor` for the same query). Controlled
injection via the `system` field *does* work (→ "Counselor"), which is why the
A/B above is valid. So the value is real but the **ingress→facts wiring has a
bug**: the envelope is not being built/applied on the messages path despite
correct config. Needs a code-level look at `ingress_preinject_build` on the
deployed path before flipping default-on (otherwise default-on is a no-op in
turns). Recall-side note: `db2_fact_recall_in_query` only resolves third-party
entities that are in `entity_registry`; the LLM extractor commits raw subject
names without registering them, so non-user entities only recall when already
registered.

---

## HyDE — retrieval recall

**HyDE was non-functional on .254.** `memory_rewrite.command` points at
`/opt/aimee/scripts/llm-rewrite.py`, which is **absent from the image's
`/opt/aimee/scripts/`** (only present in a workspace clone). The C caller treats
a failed/empty rewrite as "fall back to original query" and logs nothing, so
HyDE has silently done nothing in production. Same class of deployment gap as the
curator-llm script issues.

Tonight: copied `llm-rewrite.py` into `/opt/aimee/scripts/` and verified the
rewrite works end-to-end:

```
in : {"query":"where does the user like to camp","hyde":true}
out: {"hyde_answer":"The user prefers camping at the beach over mountains. They
      enjoy spending time with their family at beach camping sites..."}
```

So the mechanism is correct. Two consequences for the decision:

1. **No real "after" existed to measure** on .254 — HyDE never ran. The relevant
   "before" is the published direct-eval baseline (commit 366fac9):
   **Recall@5 ≈ 0.21, Recall@10 ≈ 0.32, MRR ≈ 0.18** (HyDE effectively off).
2. **Latency cost is now visible**: with HyDE active, `memory.find_facts_scoped`
   (the semantic route where the rewrite fires) exceeded 120s per query in
   probes — every semantic recall now incurs an extra LLM rewrite call (and the
   path also reranks). This is a material per-query cost that argues against
   blanket default-on.

A clean recall@k before/after needs the **native direct eval**, not the /v1
path: `/v1/memory/search` is keyword-only and never invokes the rewrite, so it
can't see HyDE. Run:

```
# off
aimee config set memory_rewrite_enabled 0 ; ./benchmarks/run-direct.sh
# on
aimee config set memory_rewrite_enabled 1 ; ./benchmarks/run-direct.sh
```

(needs the `aimee` client binary + `data/locomo/locomo10.json` + a live embedder;
none of which are in the combined container, so this runs from a built client
stack, not .254.)

Recommendation: **do not default-on HyDE yet.** Measure recall lift with the
native eval, and weigh it against the per-query LLM-rewrite latency. If lift is
real, gate it (e.g. only on factoid/why/how routes, or cache rewrites) rather
than running it on every semantic recall.

---

## .254 changes made tonight

- `aimee.yaml`: added `ingress_preinject_enabled: true` (was absent → off).
- Deployed `/opt/aimee/scripts/llm-rewrite.py` (was missing).
- `memory_rewrite_enabled` set back to **false** at end of run to restore fast
  semantic recall (HyDE script stays deployed for future A/B).
- Removed the 15 synthetic `tfb_*` benchmark memories + their extracted facts.
