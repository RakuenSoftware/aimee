# Native attention delivery of runtime personas and goals

The implemented operator state, context path and native handoff artifact are recorded in
[the completed runtime proposal](../done/runtime-personas-conversational-goals.md).
This proposal covers only the remaining model-plugin work.

## Pending: model-plugin integration

The native plugin source and release belong to the Qwen/model-plugin repository;
Aimee owns operator state and authorization. Existing native record ingestion does
not expose a qualified request-scoped persona/goal bank selector.

Must haves:

1. Accept operator-authorized, scoped `aimee.behavior.v1` source manifests, distinct
   from retrieved human/bot factual memories. Recheck current authority on warm reuse. The manifest's `authority` field is metadata, not a credential; require an authenticated operator source binding.
2. Always select the active persona and active goal, independent of relevance search
   and its record cap. Paused/review/terminal goals must not be injected.
3. Prepare model-specific instruction-bearing banks; bind them to checkpoint digest,
   adapter/version, precision and position protocol. Treat their content as persona/
   goal conditioning, not ordinary factual recall. Measure instruction-following.
4. Acknowledge the exact scope, manifest hash and slot versions used by each inference
   request. Atomically select one snapshot; never mix old/new persona banks or allow
   cross-channel/cache collisions. Retire replaced banks and exclude inactive goals.
5. Negotiate attention capability explicitly. If unsupported, choose documented
   context fallback. If attention is requested as required, refuse rather than silently
   placing persona/goal text in a prompt. Keep core authority in the stable system layer.
6. Count native conditioning against the real attention/state budget, including its
   effect on prompt positions and recurrent state. Report preparation/reuse costs.
7. Validate cold/warm switching, return-to-default, goal pause/replacement/completion,
   concurrent isolated scopes, revocation, restart, stale in-flight turns, and no goal/
   persona prose in the textual prompt when attention delivery is acknowledged.

Nice to haves:

- Prewarm several profiles to make selection fast.
- Share a common objective across scopes with separately scoped persona and evidence.
- Semantic progress proposals backed by auditable quotes; the current controller uses
  operator-defined literal milestones and never lets the model declare final success.
- Model-specific adherence and quality scores for persona, language, goal progress,
  factual preservation, repetition and interference between persona/fact/goal banks.

Native memory is not required to conform to an external backend contract. This
manifest defines the application/plugin handoff, not a new requirement on Cognee or
Hillock. Neither an attention bank nor context instructions can grant tools, schedule
turns, prove success, or give a small model language skills it does not possess.

Persona and stable goal-definition banks should be reusable independently of frequently
changing progress state; otherwise each recorded turn would force cold preparation.
