# Intelligent routing acceptance on .253

Validated 2026-09-06 on Proxmox `192.168.1.253` with newly created, owned guests:

- Unprivileged Debian 13 CT `9431`, `aimee-routing-ct-01a077e6`.
- Debian 13 VM `9432`, `aimee-routing-vm-01a077e6`.
- Each: 4 vCPUs, 8 GiB RAM, 16 GiB disk, PostgreSQL 17 with pgvector,
  separate migration/runtime roles, disposable UTF8 databases, Docker 26.1.5.
- CT nesting/keyctl enabled and applied by restarting the new guest before Docker
  validation. No existing guest, production roster or production credential changed.

The actual native server and KB, Go modules, module admission policy, provider
manager, PostgreSQL, newly enrolled mTLS client, and network-isolated Docker
sandbox ran in each guest. Only the external completion endpoint was a deterministic
HTTP fixture. Assessment scores are fixture inputs, not measured model quality.

## Results

Both guests passed all twelve routing checks. Each made seven real HTTP completion
calls; all five refusal cases made zero provider calls. The public delegate route's
background jobs were polled to terminal state, and successful `agent_name` values
were matched to the model received by the HTTP fixture. Distinct case prompts avoid
mistaking response-cache reuse for provider execution.

| Case | Expected result | CT / VM |
| --- | --- | --- |
| explicit free beats unknown and paid | free | Pass / Pass |
| known paid beats unknown price | paid | Pass / Pass |
| estimated spend beats legacy tier | cheap | Pass / Pass |
| threshold excludes cheaper weak model | qualified | Pass / Pass |
| all below threshold refuses before dispatch | Refused before vendor | Pass / Pass |
| wildcard role cannot bypass missing assessment | Refused before vendor | Pass / Pass |
| model override cannot inherit assessment | Refused before vendor | Pass / Pass |
| persisted model replacement invalidates assessment | Refused before vendor | Pass / Pass |
| equal-cost selection is stable by name | alpha | Pass / Pass |
| roster order cannot change tie winner | alpha | Pass / Pass |
| routing process timeout refuses without vendor call | Refused before vendor | Pass / Pass |
| routing process resumes selection after recovery | alpha | Pass / Pass |

Both also passed real first-user enrollment, rejection of bearer-only writes, API
health/version, and the wire-contract and PostgreSQL job-completion regression tests.
The final scoped harness reported **3 passed, 0 failed** (health, version, twelve-case
probe) on each guest. The probe resumes its SIGSTOP-ed routing process in `finally`.

## Findings fixed during exploration

- Durable completion decoded the native client's result text as an API-call count.
  Jobs stayed `running` after success or preflight refusal. The Go store now follows
  `id, status, cursor, result, has_cost, cost_usd`, stores the cursor as schema-required
  text, and preserves the independently recorded API-call count. Real PostgreSQL
  tests cover successful completion and competence refusal.
- The live acceptance runner treated a queued job as a foreground result. It now
  polls `/v1/jobs/status` and rejects missing, partial, cancelled or timed-out jobs.
- The local-stack harness omitted feature-module client grants, memory placement,
  and the production sandbox helper. It now installs those declared grants, sets
  placement, builds/locates `aimee-delegate-egress`, and provides an explicit
  probe-only mode for scoped acceptance.

Exploration also verified refusal for an unregistered workspace, a non-Git directory,
and unavailable container isolation. After supplying a registered scratch Git checkout
and functioning nested Docker, the same routing requests completed successfully.

The broader memory/embedding harness was explored separately: enrollment, memory
write/read, and persisted mTLS identity/memory after server restart passed. It was
**not green overall**: KB search lacked an external embedder, and memory governance
could not list review rows. Those are outside routing acceptance; this report does
not claim semantic retrieval or memory-governance coverage. Paid vendor credentials,
real model quality/calibration and production learning were not exercised. Price-band
and premium-learning selection have Go/native/conformance coverage rather than live
vendor billing evidence.

## Reproduction and cleanup

Use [the disposable routing probe instructions](../../scripts/validation/providers/README.md#disposable-full-stack-routing-exploration).
The probe creates its own small Git workspace and replaces only the scratch roster.
Supply matching store runtime/migration schemas and provision extensions first.
`TMPDIR=/var/tmp` avoids exhausting a VM's small tmpfs when retaining multiple runs.

The identical tested binary SHA-256 values on both guests were:

```text
b1322f2978e788ccb861002f814da02418dd5a44f35b27e3c349518f3098624c  aimee-server
aecf61ce053852f7a7e91889d25ff45cf48496dbee0cfc74355f26c03bd81b2d  aimee-module
d88848c53e64b4d39d1af46089ff14d3c334bb82a6cb35c6b8eadc118eb505d2  aimee-delegate-egress
```

CT 9431 and VM 9432 were stopped and destroyed after checking their exact owned names.
Their Proxmox configurations, optane volumes and remote staging payload were verified
absent. Existing guests were left intact.
