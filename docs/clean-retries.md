# Clean native retries

Clean retry tracking is an explicit, initially disabled capability of native
`POST /v1/chat/stream` primary sessions. It uses the durable action lineage and
current Go memory assembly. External provider CLIs and compaction requests refuse
this option. Ordinary untracked turns retain their existing behavior.

Enable `clean_retry` in the existing operator `policy.json`, for example:

```json
{
  "clean_retry": {
    "enabled": true,
    "max_attempts": 3,
    "max_repeated_failures": 2,
    "max_wall_seconds": 900,
    "max_provider_bytes": 1000000,
    "nanodollars_per_byte": 1,
    "max_nanodollars": 1000000,
    "retain_input_seconds": 900
  }
}
```

An initial native chat request includes `"clean_retry": {}`. The stream emits a
`retry_attempt` event with the host-issued ID, including on an execution failure.
To retry, send a new request in the same owned session with:

```json
{
  "message": "The complete current task instructions and constraints",
  "aimee_session_id": "owned-session",
  "clean_retry": {
    "previous_attempt": "ID from the retry_attempt event",
    "replace_constraints": true
  }
}
```

The new message replaces prior user constraints. The runtime discards prior
provider history for this attempt, appends a bounded host failure/action summary,
then calls the ordinary current memory assembler and final provider budget gate.
The old assistant's speculation and speculative lessons are not copied. Source
corrections and revocations therefore use the same Go contracts as ordinary
turns. A new attempt, plan and evidence-requirement revision are issued. Existing
exploration and action task lineage remains shared; retry cannot reset it.

Baseline metadata commits to the original user input, policy, task-projection
revision and renderer. Input retention is explicitly opt-in through
`retain_input_seconds`; zero retains only a commitment and refuses reconstruction.
Expired, missing or corrupt input produces an explicit gap. The caller can submit
an explicit fresh plan within the same remaining root allowance, but the system
cannot claim to reconstruct unavailable input. Expired retained input is removed
when that root is next processed; this is not a physical-erasure guarantee.

Each actual provider handoff, including transport resends, reserves its exact
serialized request bytes and the configured byte-priced estimated cost before
sending. Failed calls and lost admission replies retain their reservations.
Limits and failure history survive restart and native task forks. These prices
are operator-defined estimates of provider-request work, not billing or complete
input/output token charges. There is no exact tokenizer at this seam: setting a
`max_tokens` hard limit (including zero) explicitly refuses execution with
`exact_token_counter_unavailable`; it never substitutes a bytes/4 guess. Wall
limits prohibit new admissions after the task deadline; an already-dispatched
network operation still has its transport timeout and may finish later.

Unknown or merely acknowledged mutations block clean reconstruction until the
MR-16 action owner reconciles them. Confirmed writes remain visible in the
summary with their exact destination and observed version. The summary is bounded
to 8 KiB; too much history is a reconstruction gap, not silent truncation. Actual
file state remains intact. Use the existing version-bound workspace preview and
restore operation separately if a restore is desired; retry never invokes it.

Only host failure classes and durable action receipts enter the summary. A model
cannot supply successful outcomes, cheaper reservations or a replacement policy.
An interrupted process may leave an attempt running with unknown completion;
that attempt cannot be relabeled failed by a public retry request. Inspect and
reconcile real effects, then use an explicit fresh plan within remaining limits.
Disabling retry prevents new admissions while preserving prior journals and
allowing an in-flight owner to persist its final outcome.
