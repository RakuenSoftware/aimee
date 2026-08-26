# Upgrading from v0.2.192

There is no route back. 0.4.0 rewrites storage, credentials, and remote identity, and a 0.2 server
will not read what it leaves behind. Your backup is the rollback plan; there is no downgrade
command. Take the backup before step one, not after the first thing goes wrong.

Read [What's new](WHATS_NEW.md) first. This cycle changes deployment, storage, credentials, remote
identity, workflows, and removed commands.

Do not install from the `v0.2.196` tag. It was promoted in error part-way through this cycle and is
not a release; an installation from it is an untested mid-cycle build missing the fixes listed under
[If you installed from a mid-cycle tag](WHATS_NEW.md#if-you-installed-from-a-mid-cycle-tag).
Several of those are cases where a fresh install came up healthy and silently did nothing useful.

## Do all of this before you touch the running deployment

1. Stop starting new workflows and wait for active writes to finish.
2. Run `aimee audit checkpoint` and `aimee audit verify`.
3. Back up the server config directory, DB1, workflow store, vault custody, TLS state, and audit
   witness material.
4. Dump DB2 with `pg_dump` or the KB export helper.
5. Export old `work_queue` rows if you need them; the upgrade removes those tables.
6. Record current compose files, image digests, environment, external endpoints, and volume names.

Do not rely on a raw copy of a live SQLite main file. Take a consistent backup with its WAL state.

## The combined image is gone, and the new stack will not adopt your old database

- Replace `aimee-combined` with the managed server or split stack.
- New KB containers start private PostgreSQL when `AIMEE_DB2_URL` is unset.
- The new compose topology does not import an older sibling PostgreSQL volume.
- Keep the old database reachable and set `AIMEE_DB2_URL`, or dump and restore into the embedded
  cluster.
- Never use `docker compose down -v` until the new database has been verified and the backup has
  been restored in a clean test.

## Credentials move into the vault, and clients re-enrol

- Move agent keys and OAuth tokens into the server vault.
- Remove legacy client plaintext only after a successful provider probe.
- Re-enroll each thin client. Verify the server fingerprint before accepting the pin.
- Confirm the first wizard owner completed its certificate-bound enrollment. Give each additional
  user the required remote-write grant. The old global `remote_writes` value authorizes nothing.
- Review mTLS revocation, org catalogs, budgets, rate limits, and egress policy.

## This upgrade is one-way, by design

0.4.0 does not preserve backwards compatibility, and that includes the image itself: once a server
volume has been booted by this release, an older image will not start on it again. It crash-loops
with:

```
[webchat] ERROR: browser UI requires a complete first-boot login sealed in Vault
```

Older images gate startup on `aimee-server --webchat-vault-check`, which wants the vault to hold a
webchat login: the sealed `AIMEE_WEBCHAT_USER`/`AIMEE_WEBCHAT_PASSWORD` pair, or one of the
`legacy_primary` / `legacy_hashes` / `accounts` records. This release holds none of them on purpose:
PAM owns the accounts and the first-boot password is never sealed (see below). There is nothing for
the old check to find, and adding something for it to find would mean persisting a password this
release deliberately does not keep.

Plan accordingly rather than treating the image tag as a rollback: keep the backups from *Before*,
and restore a pre-upgrade volume if you need to go back.

## Browser logins become PAM accounts

The dashboard now authenticates against local PAM rather than credentials held in the server vault.
A host password is not one of aimee's own secrets; keeping it in the vault built a second identity
system beside the PAM and OIDC the KB already implements.

On upgrade, the first-boot pair (`AIMEE_WEBCHAT_USER`/`AIMEE_WEBCHAT_PASSWORD`, or the generated
one) is provisioned as a real system account in the `aimee-webchat` group. Logins are no longer
sealed into the vault, and the shadow verifiers behind them are no longer erased.

**Check this before upgrading.** An appliance that already ran an image which migrated
`webchat/logins` into the vault is in a state this upgrade cannot repair by itself: that migration
sealed the verifiers into the vault's `legacy_hashes` record and then erased them from
`/etc/shadow`. Those accounts therefore have no PAM credential to restore, and only the first-boot
pair is provisioned. Any dashboard account created through the wizard on such an appliance (the
account named in `webchat/bootstrap-replaced`) must be recreated after upgrading.

To tell whether you are affected:

```bash
docker exec <server> sh -c 'ls /var/lib/aimee/webchat/'
```

`bootstrap-replaced` present with no `logins` beside it means the vault migration ran. Sign in with
the first-boot pair after upgrading and recreate the wizard account. If neither credential is
available, provision one directly:

```bash
docker exec <server> sh -c 'useradd --system --no-create-home --shell /usr/sbin/nologin \
  --gid aimee-webchat <name> && passwd <name>'
```

## Restore remote writes

The shared bearer is read-only after this upgrade. The first wizard owner receives an explicit
certificate-bound `full` grant during enrollment. Additional remote users need KB-signed identities
and exact subject grants. `aimee.api.remote_writes=data|full` remains parsed, warns at startup, and
increments `remote_writes.global_ignored`; it does not authorize a user write.

Re-run the managed wizard and use the summary's Deploy action to create the default team, server
workload identity, signed generation-1 JWKS, and root-owned public trust volume.
The one-shot authority image remains separate from the ordinary KB/server
images, and the server mounts only its public output read-only.

For a split or operator-managed authority, configure the server explicitly with
`AIMEE_SERVER_ID`, `AIMEE_SERVER_TEAM_ID`,
`AIMEE_SERVER_MGMT_JWKS_TRUST_BUNDLE`, and the one-time `AIMEE_KB_CONN`
enrollment string. A complete explicit packet wins over managed enrollment;
partial packets fail closed. Missing team configuration returns
`no_team_configured`; missing or stale signing trust fails closed. The shipped
server Compose file retains the read-only operator mount at
`${AIMEE_SERVER_MANAGEMENT_DIR:-./server-management}:/run/aimee/management:ro`.

For that explicit split path, create the first team through its private
PostgreSQL socket:

```bash
KB_CONTAINER=$(docker ps --filter label=com.docker.compose.project=aimee \
  --filter label=com.docker.compose.service=aimee-kb --format '{{.ID}}')
docker exec \
  -e 'AIMEE_DB2_URL=postgresql:///aimee_shared?host=/var/lib/aimee/run' \
  "$KB_CONTAINER" aimee-kb team create default
```

Enroll the server into the returned team, finalize the matching `kb_server_registry` row, publish
the signed JWKS, and install the exported public bundle as `root:root 0644` under
`server-management/jwks-trust-bundle.json`. The container runs as UID 1000, so a root-owned
`0600` bundle cannot be read. The loader still rejects symlinks, extra hard links, a non-root
owner, and all group/world write bits.

Put all four values in `.env`, recreate the server container, and restart it once after the KB is
ready. Successful enrollment atomically saves the mTLS certificate and private key at
`$AIMEE_HOME/kb-client-identity.json` with owner-only mode `0600`. It does not save the one-time
token, and it validates the stored identity against the connection string's CA pin on every restart.
Startup logs name the missing input when a deployment remains read-only.

Each Aimee mTLS identity must now carry exactly one extended-key-usage role: `serverAuth` for a
listener or `clientAuth` for an outbound peer. Certificates with no EKU, `anyExtendedKeyUsage`, or
both roles are rejected. Reissue any older broad-purpose certificate before upgrading. Keep every
connection pair on distinct key material and rotate each pair independently; in particular,
aimee-server's thinclient-facing server certificate must not be reused as its KB client identity.

Stage this upgrade by issuing both role-specific identities before switching the new binaries over.
Keep the previous certificate/key bundles intact until both new handshakes and one authenticated
request have succeeded. A rollback restores each old certificate and its matching private key as a
pair; do not move either new identity into the other role to make a rollback connect. The new release
fails closed when the counterpart identity is absent, so partial certificate installation is a
startup/configuration error rather than a bearer-only fallback.

Every networked Aimee hop now applies three independent checks on ordinary requests: the verified
pair-specific mTLS identity, the current rotating connection bearer, and the enrolled PAM or OIDC
application identity. On server-to-KB traffic these are configured with
`AIMEE_KB_CLIENT_BEARER_TOKEN` plus either `AIMEE_KB_CLIENT_OIDC_TOKEN` or the
`AIMEE_KB_CLIENT_PAM_USERNAME`/`AIMEE_KB_CLIENT_PAM_PASSWORD` pair. OIDC mode does not fall back to
PAM when its federation is unavailable. On thinclient-to-server content traffic, a caller identity
JWT occupies `Authorization` while the independent rotating connection bearer is carried in
`x-api-key`; a verified client certificate no longer bypasses that bearer check.

Those four server-to-KB values are first-boot inputs: bootstrap seals them into Vault, removes them
from the process environment, and the client reads the current Vault values for every request so
bearer, OIDC-token, and PAM-password rotation takes effect on an existing pooled TLS connection.
Changing `AIMEE_KB_CLIENT_PAM_USERNAME` is an identity migration, not password rotation: provision
that PAM account on the KB and rotate the matching `service:<name>` certificate enrollment in the
same staged change.

Caller context is distinct from those service-connection checks. A KB-signed OIDC caller token is
forwarded unchanged and cryptographically verified again by aimee-kb with token type, issuer,
server audience, team and certificate-bound JWKS pinned. A host/PAM caller is asserted by the
triple-authenticated aimee-server channel. This is the explicit compromise: a compromised server can
name a host account, but it cannot forge an OIDC caller, and it cannot grant either caller a team or
project because membership remains KB-owned and is intersected with the enrolled server scope. A new
per-request mint would ask the KB to trust the same host assertion at mint time, so it would add a
round trip and audit artefact without changing that authority.

The local CLI remains on the OS-authenticated Unix-socket boundary and is resolved to its host
account before any KB content request; an unresolved uid is refused. Physical host takeover is not a
separate Aimee protocol threat. Browser and MCP identity continue to terminate at aimee-server and
use the same server-to-KB path. Caller-less ingest, re-embed, curator and code-index work uses a
closed-name, project-bound maintenance scope. Durable queues are read only far enough to claim work
and learn its project; content transactions then admit only that project's attributed rows and end
before any embedder, model, or sidecar call. The scope does not impersonate a user or add a network
credential. This release declares the content readers ready after live two-user/two-team coverage,
but applying the schema does not enable content RLS.

The attribution is deliberately numeric and explicit because tenancy-project names are unique only
inside a team. On the KB host, list the tenancy project ids and bind each existing code-index project
that owns documents or file-index rows:

```bash
aimee-kb project list [team-id]
aimee-kb project attribute <code-index-project> <kb-project-id>
```

Re-running `project attribute` replaces the prior binding atomically. It is an org-admin operation;
both exact projects must already exist, and no name-based fallback is attempted.

Before enabling, the following query must return no rows. Embeddings must agree with their owning
chunk, regions and table cells inherit through their foreign-key chain, and assets must still have a
matching document. If pgvector is intentionally unavailable, omit the two embedding arms because
those optional relations do not exist.

```bash
psql "$AIMEE_DB2_URL" -c "
  SELECT 'kb_documents' AS source, d.project, count(*) AS rows
    FROM kb_documents d LEFT JOIN projects p ON p.name=d.project
   WHERE p.kb_project IS NULL GROUP BY d.project
  UNION ALL
  SELECT 'kb_file_index', f.project, count(*)
    FROM kb_file_index f LEFT JOIN projects p ON p.name=f.project
   WHERE p.kb_project IS NULL GROUP BY f.project
  UNION ALL
  SELECT 'kb_embeddings', e.project, count(*)
    FROM kb_embeddings e
    LEFT JOIN kb_documents d ON d.id=e.point_id AND d.project=e.project
    LEFT JOIN projects p ON p.name=d.project
   WHERE p.kb_project IS NULL GROUP BY e.project
  UNION ALL
  SELECT 'kb_pdf_embeddings', e.project, count(*)
    FROM kb_pdf_embeddings e
    LEFT JOIN kb_documents d ON d.id=e.point_id AND d.project=e.project
    LEFT JOIN projects p ON p.name=d.project
   WHERE p.kb_project IS NULL GROUP BY e.project
  UNION ALL
  SELECT 'kb_doc_regions', coalesce(d.project,'<missing chunk>'), count(*)
    FROM kb_doc_regions r LEFT JOIN kb_documents d ON d.id=r.chunk_id
    LEFT JOIN projects p ON p.name=d.project
   WHERE p.kb_project IS NULL GROUP BY d.project
  UNION ALL
  SELECT 'kb_table_cells',
         coalesce(d.project,CASE WHEN r.id IS NULL THEN '<missing region>' ELSE '<missing chunk>' END),
         count(*)
    FROM kb_table_cells c LEFT JOIN kb_doc_regions r ON r.id=c.region_id
    LEFT JOIN kb_documents d ON d.id=r.chunk_id LEFT JOIN projects p ON p.name=d.project
   WHERE p.kb_project IS NULL
   GROUP BY coalesce(d.project,
                     CASE WHEN r.id IS NULL THEN '<missing region>' ELSE '<missing chunk>' END)
  UNION ALL
  SELECT 'kb_doc_assets', a.project, count(*)
    FROM kb_doc_assets a
    LEFT JOIN kb_documents d ON d.project=a.project AND d.generation=a.generation
                            AND d.file_path=a.document_key
    LEFT JOIN projects p ON p.name=d.project
   WHERE p.kb_project IS NULL GROUP BY a.project;"
```

Then enable the policies as a deliberate operator act:

```bash
psql "$AIMEE_DB2_URL" -c "select kb_content_scope_enable();"
```

The function refuses unless the release readiness marker is present and every content-bearing
project and child row is attributed consistently. It atomically enables documents, file index,
general/PDF embeddings, document regions, table cells, and document assets. To roll back enforcement
without changing attribution, call
`select kb_content_scope_disable();`.

Re-run `kb_content_scope_enable()` after this upgrade even when document/file-index scope was already
enabled. The call is idempotent and brings the newly covered vector and structured-document tables
under the same operator-controlled switch.

Grants are keyed by server, team, and exact authenticated subject:

| Subject | Form |
| --- | --- |
| PAM user | `alice` |
| OIDC user | `oidc:<percent-encoded-issuer>:<sub>` |
| mTLS identity | `cert:<issuer>:<serial>` |
| local single-org operator | `owner` |

Grants are administered **against aimee-kb**, not through aimee-server. The
server used to proxy this over its local Unix socket; that proxy was removed,
because proxying it meant aimee-server holding an administrative identity on
aimee-kb, which a single-tenant data-plane service should not have. The
`aimee kb grant …` commands went with it and no longer dispatch.

```bash
# on aimee-kb, as a principal with admin or team-lead authority IN the target team
POST /v1/write-tier-grants/set     {"server_id": "...", "team_id": N,
                                    "subject": "...", "tier": "data"}
POST /v1/write-tier-grants/revoke  {"server_id": "...", "team_id": N, "subject": "..."}
GET  /v1/write-tier-grants?server_id=...&team_id=N[&include_revoked=1][&subject=...]
```

The acting identity comes from authentication, never from the request body, and
the DB layer's `SECURITY DEFINER` check is the authority. A caller that is not a
member of the named team is refused with exactly that reason; the `(server,
team)` pair must also be registered.

`data` permits memory, document, and index writes. `full` also permits agent, delegate, runner, and
workspace control. The first grant uses the local `owner` operator context with team `0`; the
command's `--team` still names the target team. Interactive users obtain their write identity by
the KB's configured PAM or OIDC login. Give unattended callers separate service subjects.

Common refusal reasons are `absent`, `invalid`, `unknown_kid`, `wrong_team`,
`no_team_configured`, `replay`, and `replay_unavailable`. Use the structured `403`, request ID, and
server log. A grant for the wrong spelling is a grant for nobody.

## The embedder and synthesis settings are renamed, with no aliases

Every setting naming the embedder or the synthesis model changed name. There is no
alias: an old name is simply not read. A deployment that upgrades without editing its
environment and `aimee.yaml` therefore reads as having no embedder configured, and
**the KB refuses to start** rather than coming up without one. Synthesis goes idle,
which is a supported state and does not stop anything.

Earlier builds in this cycle did fall back to a builtin lexical embedder here, so the
KB came up healthy and answered searches with keyword matching while the renamed
settings sat unread. That fallback is gone precisely because nothing errored.

| Old | New |
| --- | --- |
| `AIMEE_EMBEDDER_URL`, config `embedding_endpoint` | `EMBEDDER_URL` / `embedder_url` |
| `AIMEE_EMBEDDING_DIM`, config `embedding_dim` | `EMBEDDER_DIMS` / `embedder_dims` |
| config `embedding_model` | `EMBEDDER_MODEL` / `embedder_model` |
| config `embedding_command` | `embedder_command` |
| `AIMEE_LLM_URL`, `LLM_ENDPOINT`, config `llm_synth_endpoint` | `SYNTHESIS_ENDPOINT` / `synthesis_endpoint` |
| `LLM_MODEL`, `AIMEE_LLM_MODEL`, config `llm_synth_model` | `SYNTHESIS_MODEL` / `synthesis_model` |
| `AIMEE_LLM_AUTH_TOKEN`, `LLM_API_KEY` | `SYNTHESIS_API_KEY` / `synthesis_api_key` |
| `AIMEE_LLM_AUTH_REQUIRED` | `SYNTHESIS_AUTH_REQUIRED` |

**This applies to `aimee.yaml` as well as the environment.** The config file used
the `embedding_*` spelling while the code had already moved on, so the file and the
setting could disagree. They are one name now, which means the old file keys are
dead: re-set them.

Deleted outright, because the container they configured is retired and the
`aimee-kb` image variant now encodes that choice: `llm_embed_backend`,
`llm_synth_backend`, `llm_synth_host`, `llm_synth_gpu`, `llm_synth_tier`,
`AIMEE_LLM_SYNTH_MODE`, `AIMEE_LLM_SYNTH_URL`, `AIMEE_LLM_SYNTH_TIER`. A config
still carrying `kb.curator.tier_b.*` is not an error: the key is ignored and
rewriting the file drops it.

**Check before upgrading**: `aimee config get embedder_model` returns the embedder you
mean to keep, under its new name. If it is empty, the KB will not start. Afterwards,
`aimee config get synthesis_endpoint` should also return what you expect.

## One synthesis role, and thinking is now a setting

Tier-A and Tier-B are gone. Every curator stage resolves the same provider, because
measurement did not support running a cheaper model on the mechanical stages. If you
configured a second provider under `tier_b.*`, that provider is no longer used.
Move it to `provider.*` if it is the one you want.

The split had a second effect that is worth knowing you no longer have: the
reasoning stages deliberately REFUSED the environment endpoint, so a deployment
configured only that way ran extraction and left synthesis idle. One endpoint under
one name cannot do that.

Thinking used to be implied by the stage, suppressed for the mechanical stages
because a reasoning pass can consume the output budget and truncate the answer. It
is now one global, user-settable switch, `synthesis_thinking`, **default on**. Turn
it off only if you point synthesis at a model that reasons past its output cap
without answering; `MF_LLM_OUT_CAP` bounds the damage either way.

## Choosing an aimee-kb image is a one-way door

There are three `aimee-kb` images, on one axis: the embedder. Its weights are baked,
so which embedder a deployment runs is decided by the tag it pulls:

| image | embedder | size |
| --- | --- | --- |
| `aimee-kb` | none; `EMBEDDER_URL` required | 373 MB |
| `aimee-kb-a25m` | bekko-a25m, 384-dim | 1.95 GB |
| `aimee-kb-nomic` | nomic-v2, 768-dim | 3.34 GB |

**The embedder axis cannot be changed after the KB has embedded anything.** DB2
records the vector-column width and refuses to start when the embedder cannot produce
it, so moving between a 384-dim and a 768-dim image means re-embedding the whole
corpus. Choose before you ingest. An external embedder (`EMBEDDER_URL`) may be any
width up to 4000, the DB2 column ceiling, not 4096, and the `aimee-kb` tag exists for
exactly that case: it carries neither PyTorch nor weights.

**A v0.2 corpus is usually neither 384 nor 768.** The 0.2 default was 1024, so a
corpus carried across on its existing `AIMEE_DB2_URL` matches no bundled image. The KB
refuses to start and says so:

```text
aimee: db2_init: embedder serves 384-dimension vectors but this corpus is recorded at
1024. Every write would be refused by the vector columns.
```

Check before you upgrade, so this is a decision rather than a surprise:

```bash
psql "$AIMEE_DB2_URL" -tAc \
  "select value from kb_meta where key='schema_embedding_dim'"
```

Two ways forward, and they are not equivalent: point `EMBEDDER_URL` at an embedder of
the recorded width and keep the corpus, or re-embed at a bundled width and lose nothing
but the time. Follow [Change the KB embedder](runbooks/change-embedder.md).

**Synthesis is no longer an axis here.** It was, which is why earlier drafts of this
page described six tags. It is now its own image deployed beside the kb, so the
matrix collapsed to the embedder alone:

| image | model |
| --- | --- |
| `aimee-llm-e2b` | gemma-4-E2B-it |
| `aimee-llm-e4b` | gemma-4-E4B-it |

If you are upgrading from an `aimee-kb-llm-*` or `aimee-kb-nomic-llm-*` tag, those no
longer exist. Pull the matching embedder-only kb image and deploy the sidecar beside
it: `aimee-kb-llm-e4b` becomes `aimee-kb-a25m` plus `aimee-llm-e4b`. The kb reaches
the sidecar over mutual TLS, using an identity the kb itself issues at startup, so
deploy the kb first.

Unlike the embedder, the synthesis choice is reversible: the sidecar holds no data, so
switching models or removing it entirely leaves the corpus untouched.

Bundling llama.cpp means this project pins it, rather than inheriting upstream's image
and its CVE fixes. `LLAMACPP_VERSION` in `Dockerfile.llm` is the single pin and needs
deliberate bumping.

## Bundled synthesis weights are baked into the image

The weights ship inside the `aimee-llm-*` images, at a quant chosen per model:
7.46 GB for E4B at UD-Q6_K_XL, 2.62 GB for E2B at qat-UD-Q4_K_XL. The container downloads nothing at any point: an image either has its model
or it does not, and `docker pull` is the one download, with the registry's retry and
resume behind it.

Budget for that in the image pull and in disk, not in a first-start delay. A `*-llm`
image is several gigabytes larger than the plain one, and an image upgrade re-pulls
the layer carrying the weights. Nothing lands on `$AIMEE_HOME`, so a volume wipe does
not cost you the model.

Leave `SYNTHESIS_ENDPOINT` empty for a bundled model. The container starts
llama-server against the baked file and points synthesis at loopback itself; a value
you write there points synthesis somewhere else and the bundled model is not loaded.

## What is gone, and what replaces it

- `aimee chat`
- `aimee work` and its routes/tools/tables
- `aimee migrate v2`
- generic `/v1/rpc`
- the combined image
- KB Unix-socket autostart
- per-session credential push

Update scripts to use named `/v1` routes, workflows/jobs, and browser/MCP/ACP/API chat surfaces.

## Verify these before you call the upgrade done

```bash
aimee remote status
aimee status
aimee kb status
aimee audit verify
aimee memory store upgrade-smoke "write ok"
aimee memory search "write ok"
```

Then:

- ingest one small source tree and check caller lookup;
- run one delegate probe and read its audit row;
- validate a workflow and inspect the Go workflow service;
- verify capture files are created and the event-bus drop counter is zero;
- restart the stack once and confirm active state recovers cleanly;
- restore the backup into a disposable deployment.

Keep the old volumes read-only until these checks pass.
