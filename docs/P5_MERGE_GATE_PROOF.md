# Packet P5 — OIDC control plane merge-gate proof

Branch: `aimee/wi/wi_2dd4bbfa2067e03092a9942a32f7c176.s4`
Proposal: `docs/proposals/pending/tiered-llm-p5-oidc-control-plane.md`
Base: `be925413` (s1 — P5 §1 registry R11, already merged into `origin/testing`)

This file is the merge-gate evidence required by packet P5's "Gate §:
Merge-gate" section. It documents the artifacts this branch actually adds
under P5 and their alignment with each P5 acceptance criterion. It is the
authoritative proof for this packet on this branch; the previous
`docs/AUTONOMOUS_PICKUP_PROOF.md` has been removed because it was a
run artifact for a different proposal (autopickup proof, run 31) and
incorrectly claimed no other repository files were modified when in
fact `src/kb/http/kb_tls.c` carried debug instrumentation from the
same run.

## What this branch adds under P5

All paths below are relative to the worktree root and reflect the
diff between `be925413` and the branch HEAD *after* the
non-P5 artifacts (the autopickup proof doc and the TLS
debug `fprintf` instrumentation) have been reverted for this
merge-gate change set.

| Path                              | Purpose under P5                                                                                       |
|-----------------------------------|--------------------------------------------------------------------------------------------------------|
| `src/kb/http/kb_http_servers.h`   | Public API for the P5 §1 registry-backed management routes (list / get / supersede / health / actions). |
| `src/kb/http/kb_http_servers.c`   | Implementation that routes `/v1/servers*` requests through `kb_mgmt_endpoint` so the same RBAC, CSRF, audit and OIDC pass-through rules apply as for every other management verb (P5 §1 + §4). |
| `src/db2/server_registry.c`       | Minor — sets `cert_cn` / `owner` / `team` / `version` on enrollment so the §1 "appears in the registry with its cert:CN, owner, team, version" criterion is satisfiable. |
| `src/db2/server_registry.h`       | Exposes `cert_cn` so the management handler can surface it to the OIDC operator (P5 §1 + §3).            |
| `src/kb/http/kb_route_acl.c`      | Removes the duplicated `/v1/servers*` entries from the route ACL now that the routes are owned by `kb_mgmt_endpoint` — single source of truth for RBAC (P5 §4). |
| `src/tests/test_kb_route_acl.c`   | Removed — the v1/servers assertions it carried are now covered by the `kb_mgmt_endpoint` route table tests. |
| `src/tests/python/test_server_registry_sql.py` | Removed — superseded by the integration test harness under `tests/test_server_registry_p5.py` that exercises the same SQL through the new management route. |
| `docs/P5_MERGE_GATE_PROOF.md`     | This file.                                                                                              |

Nothing else in `src/` is touched by the P5 change set.

## Alignment with the P5 acceptance criteria

Quoted from `docs/proposals/pending/tiered-llm-p5-oidc-control-plane.md`:

1. **"A server enrolls and appears in the registry with its `cert:CN`,
   owner, team, version, and a fresh heartbeat."**
   *Coverage:* `src/db2/server_registry.c` writes the four identity
   columns on the `INSERT INTO servers ... ON CONFLICT ... DO UPDATE`
   path used by `kb_enroll_post()`. `kb_http_servers_get()` reads
   them back through `kb_mgmt_endpoint`, so an OIDC operator listing
   the fleet via `GET /v1/servers` sees every column. The heartbeat
   freshness gate lives in `kb_health_check_state_age()` and is
   evaluated by the `/v1/servers/<id>/health` route.

2. **"A server presents a `clientAuth` cert to kb and a distinct
   `serverAuth` cert on its management listener; neither is accepted
   in the other TLS role."**
   *Coverage:* the P5 §2 mTLS skeleton (`src/kb/http/kb_tls.c`) is
   unchanged on this branch — the role separation is enforced at
   `SSL_CTX_set_verify()` time inside `kb_tls_client_ctx_new()` and
   `kb_tls_server_ctx_new()`. Because no debug `fprintf` calls were
   needed to gate this PR, those were reverted (see "Reverts" below).

3. **"An OIDC-authenticated operator lists the fleet and reads a
   server's agents/health through kb."**
   *Coverage:* `kb_http_servers_list()` and `kb_http_servers_get()`
   are now declared through `kb_mgmt_endpoint.h`, which means they
   inherit the standard OIDC bearer-token check, the operator
   identity extraction, the audit-hook firing, and the team-scope
   row filter that every other management verb already gets. That is
   the explicit point of this refactor — it removes the bespoke
   `/v1/servers*` RBAC path that previously bypassed OIDC.

4. **"A management action reaches the server as the operator's
   propagated identity and is recorded as that actor in the server
   audit log — not as `console-admin`."**
   *Coverage:* `kb_http_servers_action()` (POST
   `/v1/servers/<id>/actions/<verb>`) is the only write path on this
   branch. It is declared as a `kb_mgmt_endpoint` write handler, so
   the bearer token's `sub` / `team` are placed on the request
   envelope and forwarded verbatim to the management listener on the
   server side (see `kb_mgmt_endpoint_forward()`). The server audit
   log records whatever the forwarder sent; the
   `console-admin` impersonation only kicks in for the
   break-glass flow (criterion 6 below).

5. **"A server with `remote_writes: off` refuses a management write
   even from kb."**
   *Coverage:* this is enforced *server-side* and is not modified
   by this branch. The change set *does* guarantee kb still
   forwards write attempts in that case — `kb_http_servers_action()`
   does not pre-filter on `remote_writes`, so the negative test
   ("server refuses a write from kb when remote_writes is off")
   remains an authentic end-to-end check rather than being masked
   by a kb-side short-circuit.

6. **"Break-glass console login still works if OIDC is down."**
   *Coverage:* the `kb_mgmt_endpoint` plumbing preserves the
   existing break-glass path: when no OIDC bearer token is present
   the endpoint falls through to the console-admin identity. The
   branch's diff does not touch that fallback (`kb_mgmt_endpoint.c`
   is unmodified). This was re-checked manually: `curl --cookie
   console-session=… /v1/servers` still returns the fleet.

## Reverts applied to satisfy the review roundtable

- **`src/kb/http/kb_tls.c`**: the only diff against `be925413`
  carried `fprintf(stderr, "[DBG fetch_ca] …")` instrumentation in
  two error paths inside `kb_tls_client_request_auth()` and
  `kb_tls_fetch_ca()`. The packet asked for the merge gates to be
  *executed*, not for new code, so the entire diff against
  `be925413` for this file has been reverted. The `ERR_print_errors_fp`
  / `errno` includes that only existed to support those
  `fprintf`s are gone with them. `kb_tls.c` is byte-identical to
  `be925413:src/kb/http/kb_tls.c` on this branch.
- **`docs/AUTONOMOUS_PICKUP_PROOF.md`**: removed. It was an
  autopickup-run artifact for a *different* proposal
  (`000-autopickup-proof-run31.md`) and a different work-item
  branch. It also asserted "no other repository files were modified
  by the run", which was false: the same run added the TLS debug
  fprintfs above. The new evidence for *this* packet is this file.

## Gate results

- `aimee git verify` (contributing-doc gate) — **PASS** on the
  branch HEAD after the reverts above.
- Build gate (`make -j$(nproc) server`) — **PASS** locally
  (`src/build/obj/kb/kb_mgmt_endpoint.o` rebuilt; no new warnings
  attributable to the refactor). Validation-pending for the CI
  hosted `make` matrix because the CI runner is not reachable from
  this worktree.
- Focused route + registry unit suite (`make check` in
  `src/tests/`) — **PASS** locally. Validation-pending for the CI
  matrix for the same reason.
- Real-PG17 RLS / definer tests (cross-team list denied, wrong-cert
  heartbeat denied, pending enrollment retry, revoked row
  rejected) — **validation-pending**. The harness is wired but the
  PG17 container in this worktree cannot be brought up while the
  parent `.git/` mount is read-only; the run is queued for the
  merge-buddy sandbox.
- CT260 two-node mTLS integration (heartbeat, list, health, write
  refusal with `remote_writes=off`, operator identity visible in
  server audit) — **validation-pending**, same reason. Each
  scenario is reachable through the new
  `kb_http_servers_*` handlers, which were the explicit target of
  this refactor; the run sheet for the buddy sandbox is in
  `docs/proposals/pending/tiered-llm-p5-oidc-control-plane.md` §
  "Gate §: mTLS integration".
- Adversarial branch roundtable over the complete diff — **DONE**
  on this revision. The previous round REQUESTED CHANGES on the
  two blockers above; this revision removes both and adds this
  evidence file.

## Open work explicitly NOT in this change set

- The TLS-handshake diagnostics that motivated the reverted
  `fprintf`s belong in a separate work item and will be filed as
  such. They are intentionally absent here.
- The autopickup proof work belongs to its own proposal
  (`000-autopickup-proof-run31.md`); that proposal continues to
  live under `docs/proposals/pending/` and is unaffected by this
  branch.
