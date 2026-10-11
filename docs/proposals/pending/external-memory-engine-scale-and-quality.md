# External memory retrieval scale and answer quality

- **State:** pending.
- **Completed integration:** [external-memory-conversation-support.md](../done/external-memory-conversation-support.md).
- **Contract:** [reusable external memory contract](../../../integrations/memory/CONTRACT.md). Native is exempt.

Cognee and Hillock now use independent durable catalogs and the same conversation profile. Recall, composition, briefing, verified export, fresh admission after erasure, source revalidation, send barriers and portable record migration are implemented. These are no longer pending integration blockers.

Remaining work:

1. Measure representative corpus sizes, query latency, concurrent workload and restart/indexing time. The catalog scans all eligible records, then selects up to 4 Cognee or 256 Hillock lexical candidates for provider ranking; this removes the 256-record corpus refusal but does not guarantee exhaustive semantic recall.
2. Compare real-model Cognee and Hillock retrieval and answer quality on attributed, multi-speaker trajectories: person versus mountain, exact units, corrections, negation, paraphrases and topic return. Cognee's deterministic model fixture proves integration, not real-model quality.
3. Improve candidate routing/indexing if those measurements require it. Cognee indexes at most 4 candidates synchronously and retains valid earlier derived datasets outside the query pool, which can make cold queries expensive. Persistent indexes/jobs are optional architectural choices with mandatory scope/version/erasure controls if enabled.
4. Add explicit optional graph, extraction, learning and procedure profiles only for justified Aimee features. Native's advanced SQL APIs are not a mandatory external profile.
5. Qualify end-to-end chatbot/Discord behavior and durable episode projection under [conversational memory](conversational-memory-and-human-interaction.md). Backend support does not itself establish complete answer quality.

No production backend switch is requested or implied. Finish workload and quality qualification before claiming production scale or identical retrieval behavior. Hillock's confirmed upstream graph/extractor defects remain in the [capability assessment](../../../integrations/hillock/CAPABILITY_GAP_ASSESSMENT.md); the supported stateless ranker bypasses those components.


## Qualification update, 2026-10-11

See [qualification evidence](../../validation/memory-backend-completion-2026-10-11/README.md).
The synthetic 100/1,000/10,000-record workload now passes a 30-second deadline for
both providers. Cognee retains exact acknowledged dataset completion and uses a
four-record synchronous pool; the previous 16-record pool exceeded the bridge
request budget. Eight conversational retrieval fixtures and real E2B answer
fixtures pass for native, Hillock and Cognee. Native question recall and external
source metadata/list priority defects found by these checks were corrected.

A real E2B/Bekko Cognee contract passed only with an extended diagnostic deadline;
its first shipping-timeout runs failed. Real-model cold indexing under the bridge
deadline remains unqualified. This single-needle corpus is not a representative
semantic recall benchmark. Live Discord qualification, exact final tokenizer
budgeting and richer feedback/projection acceptance remain pending. No production
backend selection has changed and no release is claimed by this evidence.
