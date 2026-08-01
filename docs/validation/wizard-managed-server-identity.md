# Wizard-managed server identity acceptance

**Status:** reproduced against the rolling `:testing` deployment on 2026-07-28;
resolved and accepted through `35f62657` on 2026-07-29.

## Expected contract

For the self-deploying managed stack, completing the browser setup wizard must
leave a usable deployment without a second, undocumented authority ceremony.
The wizard owns both bootstrap identities:

1. the first human user's certificate-bound enrollment; and
2. the aimee-server workload identity used to reach aimee-kb, bind the server to
   its team, refresh signed management JWKS, and authorize KB-backed writes.

The second identity includes a stable server id, a team id from the matching KB
registry row, a single-use server-to-KB mTLS enrollment, and the public JWKS trust
material. Private keys and enrollment tokens must remain out of browser responses
and logs.

## Original reproduction

Start `compose.server-managed.yaml` with rolling `:testing`, complete every page of
the web GUI wizard, press Deploy, enroll the first Linux client with the command
the wizard returns, and wait for the managed KB and LLM health checks to pass.

The first-user flow succeeds, but the server workload is still unprovisioned:

- `AIMEE_SERVER_ID` is empty;
- `AIMEE_SERVER_TEAM_ID` is empty;
- the managed KB has no `kb_server_registry` row;
- no server-to-KB identity has been saved as
  `$AIMEE_HOME/kb-client-identity.json`;
- the configured management JWKS trust bundle is absent;
- a remote `aimee kb build` is denied because the otherwise enrolled client has
  no usable write-authority path.

The KB is healthy and already contains indexed lexical data and vectors, so this
is not a KB or embedding-service outage.

## Original code path

At the reproduced revision, `POST /v1/deploy/apply` performed only two
operations:

1. `server_http_first_user_bootstrap()` creates/reuses the first browser user's
   enrollment bearer and eventual certificate-bound `full` grant;
2. `deploy_apply_start()` launches the managed KB/LLM compose project and verifies
   the KB-to-LLM bearer.

Neither path creates or persists the server workload identity. The top-level
compose file merely passes through optional operator-supplied
`AIMEE_SERVER_ID`, `AIMEE_SERVER_TEAM_ID`, and `AIMEE_KB_CONN` values and mounts
an optional, pre-existing JWKS trust directory. This makes the browser wizard a
partial installer even though it reports Deploy as successful.

## Required behavior

The managed deploy worker must add an idempotent server-identity phase after the
KB is healthy and before it reports success. The implementation may choose its
internal protocol, but it must satisfy these externally visible invariants:

- a fresh wizard-only install creates exactly one active server registry row and
  one default team binding;
- the server id and team survive server, KB, and full-stack restarts;
- the one-time enrollment is consumed once and the resulting private key remains
  in the server's private persistent volume with mode `0600`;
- the server can perform an authenticated KB heartbeat after enrollment;
- the public trust roots and a current signed JWKS publication are available to
  the server without a host operator copying files;
- re-running Deploy is idempotent: it reuses the same identity and cannot rotate
  or duplicate the team/registry row accidentally;
- a partial failure is visible in `/v1/deploy/status`; Deploy must not report a
  successful, writable installation while identity bootstrap is incomplete;
- browser JSON, compose output, process arguments, and logs never expose private
  keys, bearer credentials, or single-use enrollment tokens;
- explicit external-KB/operator-managed identity settings continue to take
  precedence and are never overwritten by the managed bootstrap.

## Acceptance gate

Run from empty named volumes and use only the documented compose command and GUI
wizard. No `docker exec` mutation, SQL, copied trust file, hand-authored server
id, or manually issued grant is permitted.

After Deploy:

1. the deploy-status response reports the workload identity as ready;
2. the first client enrollment command succeeds;
3. `aimee kb build` succeeds for a registered fixture;
4. symbol, caller, blast-radius, and semantic KB lookups all answer;
5. restarting every container preserves the same server/team identity and the
   same enrolled certificate;
6. re-running Deploy creates no duplicate registry, team, or enrollment rows;
7. revoking the server enrollment causes heartbeat/JWKS refresh to fail closed.

This gate is also the setup prerequisite for the Ponytail reanalysis Aimee arm:
benchmarking a manually repaired or half-installed deployment would not measure
Aimee's minimum standard installation.

## Resolution and final-head evidence

The managed-v2 wizard now performs the workload-identity transaction after the
KB is healthy and before Deploy succeeds. It creates and persists the server and
team identity, client certificate/key, owner/full grant, and signed JWKS trust
through the browser-driven deployment path. The runtime reads that protected
`kb-client-identity.json`; empty legacy `AIMEE_SERVER_ID`,
`AIMEE_SERVER_TEAM_ID`, and `AIMEE_KB_CONN` environment variables are expected
for this mode and are not evidence of an incomplete install.

The final benchmark acceptance deployment used these exact components:

- server `0.2.195-1009-g35f62657`;
- distributed KB `v0.2.195-1007-gb0e1f2c5`;
- thin client `v0.2.195-1003-g627c1ffc`.

The server reports both its own version and the distributed KB version. Its
wizard-generated identity is ready, bound to the default team, certificate
authenticated, and granted owner/full. The persistent server, store, vector
index, and embedder all report healthy.

A fresh per-project acceptance cycle registers a canonical checkout, uploads a
forced index scan, and waits for `kb build --force` to finish both code and
document embeddings. Before an agent is allowed to start, the gate requires:

1. symbol lookup for the planted `end_of_month` helper;
2. all three callers from billing, invoices, and reports;
3. blast-radius output for `app/dates.py`;
4. a project-scoped semantic hit for the planted README passage;
5. identical pre-edit Git heads and complete manifests for the indexed and
   execution checkouts; and
6. a connected Aimee MCP server and tool inventory in the actual agent startup
   event.

The acceptance exercise exposed additional defects after initial enrollment:
the wizard discarded the full mTLS grant, thin-client build returned before
project embeddings completed, background document refresh could starve later
projects, the distributed mTLS relay ignored caller timeouts, and server health
did not identify the distributed KB build. Those defects are covered by the
commits after `2d068b7a`; all are required for the benchmark-ready state above.
