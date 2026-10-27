# Native browser recall probe

This live probe checks factual recall across Claude A1, resumed Claude A2, HTTP
model B, and a return to the original Claude session. Both selectable scenarios
keep two independently generated user-provided values and the full native tool
profile. A2 must recall A1's first value. B must recall it and add the second value
supplied in B's user message. Returning A must recall both values without either
appearing in its new prompt.

`scenario_mode: "legacy-label-marker"` is the default and retains the reproducible
814 probe: two independently generated 80-bit label/marker values. Its first three
prompts remain exact. `scenario_mode: "user-reference-uuid"` is a proposed test
realism/provenance variant: two independent `crypto.randomUUID()` UUIDv4 values,
with 122 random bits each, named project reference ID and release reference ID.
The IDs are fixed before A1; no mode changes or value resampling occur after a
refusal. The UUID scenario keeps the same four-turn information flow, full-value
answer requirement, model/tool profile, and native identity/cache gates. It is
not a release-planning task or a substitute task with easier factual values.

The final question asks for the values supplied by the user. An earlier probe
incorrectly said the assistant introduced the marker, although B's user message
supplied it. Its recorded `reasoning_extraction` refusal remains a failed result.
The corrected user-provenance 814 label/marker probe also produced a retained
`reasoning_extraction` refusal on native return. Neither failed result is waived,
retried, or reclassified by adding the UUID scenario. No product bug or provider
refusal cause is established here. The UUID variant is unqualified until a
separately authorized live run supplies its own evidence; it does not change
SDKs, models, tools, history projection, native identity, or refusal handling.

The answer oracle checks only actual canonical assistant text from the new turn.
It requires the exact values once each, rejects guesses and truncated or extra
codes, and does not retrieve answers from user messages or thinking blocks.

## Local contract checks

```sh
node --test tests/e2e/native-recall-contract.test.cjs tests/e2e/native-recall-history.test.cjs
```

These checks also run in the frontend browser CI command. They validate the
probe's prompts and event handling; they are not evidence of live model recall.

## Live execution

Use a disposable, fully provisioned server with working native and HTTP model
connections. The runner consumes the fixture owner's configuration and independent
source, enrollment, certificate, session, and native-cache receipts:

```sh
node tests/e2e/native-recall-browser.cjs /path/to/fixture-config.json
```

The JSON configuration supplies `credentials`, `expected_url`, `actor`, `alias`,
`second_alias`, `project_label`, `project_root`, and private `output` paths. It
also supplies `source_inventory`, `producer_receipt`, `enrollment_receipt` and
its SHA-256, `certificate_receipt`, `pre_send_gate`, `fresh_sid_gate`, and the four
phase cache receipts in `gates`. No credential is embedded in the runner.
The producer receipt must explicitly qualify a fresh install
(`fresh_install_qualification_pass: true`) or a same-volume upgrade
(`upgrade_qualification_pass: true`). Both require exact source and image binding,
all 86 installed products, all 19 server modules and all three healthy services.
`chromium_executable` optionally selects the installed browser. TLS verification
remains enabled; a fixture with a private certificate supplies its exact UI
certificate SPKI in the certificate receipt.

The optional `scenario_mode` selects one of the two modes above. Unknown modes
fail before browser launch. Missing mode retains the legacy default; there is no
automatic fallback between modes. Reports and phase-start artifacts identify the
selected mode, and UUID-mode reports name both reference IDs explicitly.

Set `prepare_authorized` to prepare a fresh empty session. The runner waits for
the fixture owner's session receipt and exact `execution_gate` before sending.
The gate binds the source, image, actor, model connection, session and nonce, and
permits four Sends with zero replies or tools. Independent phase receipts prove
the actual SDK UUID, identity, cwd, preserved file prefixes and native process
quiescence. B must leave the native cache unchanged. Returning A must resume the
original SDK session and preserve the full cache present before B.

Any refusal, unexpected tool, duplicate Send, failed terminal, bad answer or
missing cache proof fails the run. There is no automatic retry or fallback.
Retain failed artifacts and clean up the disposable server and fixture credentials
after exporting the canonical history, cache proofs and result index.
