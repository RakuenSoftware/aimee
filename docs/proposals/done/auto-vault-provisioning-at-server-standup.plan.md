# Implementation plan — Automatic delegate-vault provisioning at standup

Plan for [auto-vault-provisioning-at-server-standup.md](auto-vault-provisioning-at-server-standup.md).
Grounded in `origin/testing` (server v0.2.85 line). Default-safe, no new always-on
behavior beyond an idempotent boot pass that no-ops when no secret source is set.

## Key existing primitives (already on testing — reuse, don't rebuild)

| Primitive | Where | Use |
|-----------|-------|-----|
| `vault_service_set_server(agent, cred, secret)` | `vault_service.c` | **Autonomous seal** under the server principal; derives the server KEK from the master key (`vault_server_kek`), creates the vault file, fail-closed if no KEK. No unlock/attestation needed. |
| `vault_store_has_entry(principal, agent, cred)` | `vault_store.h` | Idempotence check (don't overwrite an existing cred). |
| `VAULT_SERVER_PRINCIPAL` (`"server"`), `VAULT_API_KEY_CRED` (`"api_key"`) | `vault_service.h` | Principal + cred name for static API-key delegates. |
| `server_run_kb_bootstrap()` | `server.c:511`, invoked ~`:668` | **Precedent**: a boot-time bootstrap pass. Mirror it with `server_run_vault_bootstrap()`. |
| agent definitions loader | `agent_config.c` (`agents.json`) | Validate that a supplied secret names a known agent. |
| `ATTEST_TLS_BEARER` | `vault_principal.h` | Future complementary remote path (see WP-D). |

Delegates already resolve **vault-first** (`delegate_credential_retry.c`), so once
the server-principal vault holds `api_key` for an agent, delegate/roundtable calls
authenticate with no further change.

## Work packets

### WP-A — Boot-time vault bootstrap pass (core)
- Add `static int server_run_vault_bootstrap(void)` to `src/server/server.c`,
  invoked from the same boot vicinity as `server_run_kb_bootstrap()` (after the
  vault subsystem/master key are available, **before** the compute pool serves
  delegate work, i.e. before `server_run()` starts the HTTP accept thread).
- Resolve the secret source, in precedence order:
  1. `AIMEE_DELEGATE_SECRETS_FILE` → a JSON object `{ "<agent>": "<key>", … }`
     (default unset; e.g. `/run/secrets/aimee-delegates.json`). Parse with cJSON.
  2. `AIMEE_DELEGATE_KEY_<AGENT>` env vars (uppercased agent name; map back to the
     real agent name case-insensitively against `agents.json`).
- For each `(agent, secret)`:
  - Skip + `LOG_WARN` if `agent` is absent from `agents.json` (don't fail boot).
  - If `vault_store_has_entry(VAULT_SERVER_PRINCIPAL, agent, VAULT_API_KEY_CRED)`
    and `AIMEE_DELEGATE_SECRETS_OVERWRITE` is unset → skip (non-destructive).
  - Else `vault_service_set_server(agent, VAULT_API_KEY_CRED, secret)`; on
    non-`VAULT_OK`, `LOG_ERROR` and continue (one bad cred must not wedge boot).
- No-op (return early) when neither source is set — zero behavior change for
  existing deploys.
- **Secret hygiene:** after ingestion, `unsetenv` each `AIMEE_DELEGATE_KEY_*`;
  `OPENSSL_cleanse` the in-memory secret buffers and the parsed JSON strings;
  never write a secret into `$AIMEE_HOME` or any log. The secrets file stays
  operator-owned/read-only (we only read it).

### WP-B — Audit + observability
- Emit one `aimee_log(LOG_INFO, "vault.bootstrap", …)` per provisioned agent with
  **counts/fingerprints only** (reuse the `vault_cred_fingerprint()` style already
  in `server_vault.c:168`), never the secret. Summarize: `provisioned=N skipped=M
  unknown=K`.
- `aimee status` / kb-style health unaffected; optional: surface
  `vault_server_creds=N` in a status field (stretch, not required).

### WP-C — Container + compose wiring
- `deploy/container/combined-entrypoint.sh` and `server-entrypoint.sh`: pass through
  `AIMEE_DELEGATE_SECRETS_FILE` / `AIMEE_DELEGATE_KEY_*` to the dropped-priv
  `aimee-server` exec (they already forward env; just document + don't strip).
- `compose.combined.yaml` / `compose.server.yaml`: add a commented optional Docker
  secret / read-only bind mount example for `aimee-delegates.json` and the env
  form. **Never bake a secret into an image.**
- SmoothNAS plugin config (`deploy/smoothnas/…`): document the secrets-file/env
  field so a `.254`-style deploy provisions on standup.

### WP-D — (Optional, follow-on) remote provisioning over native TLS
- Once [native-tls-thin-client-backends.md](../pending/native-tls-thin-client-backends.md)
  lands, `ATTEST_TLS_BEARER` already authorizes server-principal writes over a
  confidential bearer channel — so an operator could `aimee vault set --server`
  remotely over `https://` instead of mounting a file. Note as complementary; not
  required for WP-A..C and not in this plan's critical path.

## Tests (WP-A/B)
- New `src/tests/test_vault_bootstrap.c`:
  - temp `AIMEE_HOME` + server master key fixture; fake `agents.json` with one
    known agent.
  - secrets-file path: asserts the cred is sealed (`vault_store_has_entry` true
    afterward) and **decrypts back** to the input via the server KEK.
  - env path: `AIMEE_DELEGATE_KEY_<AGENT>` provisions; env var is unset afterward.
  - idempotence: second run does not overwrite (and overwrite flag does).
  - unknown agent: warns, skips, boot returns success.
  - hygiene: no plaintext secret present in any file under `$AIMEE_HOME` after the
    pass (grep the tree in-test).
- Wire the test into the Makefile test target; stub any server objects the test
  link pulls in (per the usual test-link discipline).

## Acceptance (maps to proposal criteria)
1. Fresh combined stack + secrets file/env → populated vault; `aimee delegate
   review … --via mistral` succeeds with zero manual `vault set`. (WP-A,C)
2. No plaintext at rest / in logs; env scrubbed; audit records the event w/o value. (WP-A,B + test)
3. Recreate is a non-destructive no-op unless overwrite flag set. (WP-A + test)
4. Unknown-agent secret warns + skips; boot still succeeds. (WP-A + test)
5. The `.254` roundtable that 401'd succeeds after a re-provisioned standup. (live verify, user-gated)

## Risks / sequencing
- Boot-ordering is the main correctness risk: the pass must run after the master
  key is loadable (so `vault_server_kek` succeeds) and before delegates serve.
  `vault_service_set_server` is fail-closed (no KEK → `VAULT_ERR_CRYPTO`, never
  plaintext), so a too-early call degrades to "not provisioned", not a leak.
- This is the **gating dependency** for live roundtables — landing WP-A..C and
  re-provisioning `.254` unblocks the roundtable flow for every other proposal,
  including [native-tls-thin-client-backends.md](../pending/native-tls-thin-client-backends.md).
