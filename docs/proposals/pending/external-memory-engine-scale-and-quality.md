# External memory retrieval scale and answer quality

- **State:** pending.
- **Completed integration:** [external-memory-conversation-support.md](../done/external-memory-conversation-support.md).
- **Contract:** [reusable external memory contract](../../../integrations/memory/CONTRACT.md). Native is exempt.

Cognee and Hillock now use independent durable catalogs and the same conversation profile. Recall, composition, briefing, verified export, fresh admission after erasure, source revalidation, send barriers and portable record migration are implemented. These are no longer pending integration blockers.

Remaining work:

1. Measure representative corpus sizes, query latency, concurrent workload and restart/indexing time. The catalog scans all eligible records, then selects up to 16 Cognee or 256 Hillock lexical candidates for provider ranking; this removes the 256-record corpus refusal but does not guarantee exhaustive semantic recall.
2. Compare real-model Cognee and Hillock retrieval and answer quality on attributed, multi-speaker trajectories: person versus mountain, exact units, corrections, negation, paraphrases and topic return. Cognee's deterministic model fixture proves integration, not real-model quality.
3. Improve candidate routing/indexing if those measurements require it. Cognee indexes at most 16 candidates synchronously and retains valid earlier derived datasets outside the query pool, which can make cold queries expensive. Persistent indexes/jobs are optional architectural choices with mandatory scope/version/erasure controls if enabled.
4. Add explicit optional graph, extraction, learning and procedure profiles only for justified Aimee features. Native's advanced SQL APIs are not a mandatory external profile.
5. Qualify end-to-end chatbot/Discord behavior and durable episode projection under [conversational memory](conversational-memory-and-human-interaction.md). Backend support does not itself establish complete answer quality.

No production backend switch is requested or implied. Finish workload and quality qualification before claiming production scale or identical retrieval behavior. Hillock's confirmed upstream graph/extractor defects remain in the [capability assessment](../../../integrations/hillock/CAPABILITY_GAP_ASSESSMENT.md); the supported stateless ranker bypasses those components.
