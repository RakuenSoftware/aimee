# Deployment

## Managed server

```bash
docker compose -f compose.server-managed.yaml up -d
```

The browser wizard launches the KB container. This requires the host Docker socket, which gives the
server Docker-host authority.

Use it for a trusted single-host install where browser-managed setup matters more than that larger
boundary.

## Split stack

> **This profile does not currently complete the server-to-KB link.** Everything below brings both
> halves up healthy, and then every server call to the KB fails. Use the managed profile until this
> is resolved; the detail is in [What is broken](#what-is-broken-in-this-profile) so a deployment is
> not attempted on the assumption that it merely needs more configuration.

```bash
cp -n .env.example .env
for v in ADMIN MIGRATOR RUNTIME; do
  echo "AIMEE_STORE_${v}_PASSWORD=$(openssl rand -hex 32)" >> .env
done
export AIMEE_KB_API_BEARER_TOKEN=$(openssl rand -hex 32)
./scripts/aimee-compose-vault-bootstrap.sh -f deploy/compose/aimee.yaml all
unset AIMEE_KB_API_BEARER_TOKEN
docker compose --env-file .env -f deploy/compose/aimee.yaml up -d
```

Server and one KB are declared together, and no browser action needs to create them. The KB owns its
embedding and synthesis role placements. Each role can run inside the KB container or use a remote
endpoint supported by the selected profile. There is no separate inference service. This is intended
as the safer default when the server must not control Docker.

`all`, not `kb`: the same token is the KB's inbound credential and the server's outbound one, so
sealing it into only the KB leaves the halves unable to talk at all.

**`--env-file .env` is not optional on this path.** Compose takes its project directory from the
first `-f` file, so for `deploy/compose/aimee.yaml` it looks for `deploy/compose/.env` and never
reads the one at the repository root. Without the flag the command fails on the store passwords even
though the file exists. The root-level profiles need no flag, because their project directory
already is the repository root.

### What is broken in this profile

The three requirements below cannot all be satisfied at once, so no value of
`AIMEE_KB_API_BEARER_TOKEN` makes this profile work:

1. The profile sets `AIMEE_KB_HTTP_BIND=1`, and it has to: the server reaches the KB across the
   Compose network by name, and a loopback-only listener is unreachable from another container.
2. Binding `0.0.0.0` with no bearer is a hard refusal, not a warning. The KB logs
   `refusing to bind 0.0.0.0:8741 with no bearer configured` and exits, and because the container is
   `restart: unless-stopped` that presents as a KB restarting every thirty seconds while
   `aimee-server` sits in `Created` behind its `service_healthy` gate.
3. Once a bearer exists, the server refuses to present it over the profile's own
   `AIMEE_KB_API_URL: http://aimee-kb:8741`, because that is cleartext to a non-loopback host:

   ```text
   kb_client: refusing to send the kb bearer in cleartext to non-loopback host 'aimee-kb';
   use the mTLS endpoint or terminate TLS in front of the kb
   ```

   The request is never sent, so `/v1/kb/status` reports `did not respond` and the circuit opens.
   That line goes to `/var/lib/aimee/server.log` inside the container and not to `docker logs`, so
   the visible symptom carries none of the cause.

Resolving it needs the server to reach the KB over mTLS or through a TLS terminator, which this
profile does not yet configure. Both halves report healthy throughout, so container health is not a
signal that the link works.

The one-KB Compose files are deployment profiles, not the fleet limit. The target architecture can
route among several KB containers with explicit corpus, authority, and capability identity. Fleet
routing is not integrated in this checkout; see [KB fleet and model placement](KB_FLEET.md).

## The server's store database

Every Compose file that runs `aimee-server` also runs `aimee-store-db`: a stock
upstream `postgres:18` holding DB1, the tables the daemon keeps. It is a plain
image on purpose. DB1 declares no extensions, so unlike DB2 it needs neither
pgvector nor pgvectorscale, and any PostgreSQL an operator already supports will
do.

It is reached only across the Compose network and publishes no port. To use your
own PostgreSQL instead, set `AIMEE_STORE_URL` and the bundled service goes
unused:

```bash
export AIMEE_STORE_URL='postgres://user:password@host:5432/aimee_store'
docker compose -f compose.server.yaml up -d
```

The bundled service still needs `AIMEE_STORE_ADMIN_PASSWORD`,
`AIMEE_STORE_MIGRATOR_PASSWORD` and `AIMEE_STORE_RUNTIME_PASSWORD` to be set even when
`AIMEE_STORE_URL` points elsewhere, because Compose interpolates every service it parses before it
decides which to start. Generate them into `.env` as above.

Create that database `ENCODING UTF8 TEMPLATE template0`. This is load-bearing
rather than tidiness: the store bounds text with `octet_length`, and in a
SQL_ASCII database `octet_length` and `char_length` are the same function, so
every byte-limit `CHECK` would pass whether or not it held.

One profile, one database: being PostgreSQL does not make DB1 shareable.

## External PostgreSQL

Use `AIMEE_DB2_URL` only as first-boot input, seal it into the KB Vault with a
disposable container, then remove it before creating the long-lived service:

```bash
export AIMEE_DB2_URL='postgresql://...'
./scripts/aimee-compose-vault-bootstrap.sh -f deploy/compose/aimee.yaml kb
unset AIMEE_DB2_URL
docker compose -f deploy/compose/aimee.yaml up -d
```

The operator owns:

- PostgreSQL availability and backups;
- TLS and service identity;
- pgvector/pgvectorscale versions;
- connection limits and latency;
- migration and restore testing.

No long-lived server or KB container stores DB2 credentials in `Config.Env`.
The disposable `--rm` bootstrap streams first-boot values over stdin, seals
them synchronously, and exits before the service is created.

## Inference

Embedding and synthesis belong to the KB that serves the request. A role can run inside its KB
container or use a remote endpoint. Internal availability depends on the KB image and profile;
remote placement needs an explicit endpoint and credential. No standalone inference container is
part of either topology.

The KB must report explicit degradation when a configured inference stage is unavailable. It cannot
claim a dense or synthesized result after silently skipping that stage.

## Network ports

Use the compose files and generated configuration as the source of truth. Typical defaults are:

| Service | Port | Exposure |
| --- | ---: | --- |
| browser | 8443 | user network, HTTPS |
| server `/v1` | 8743 | enrolled clients only |
| KB `/v1` | 8741 | deployment network only |
| remote model endpoint | provider-defined | deployment network only |

Do not publish PostgreSQL or a remote model endpoint unless a separate host needs it. Apply TLS and
service identity before crossing a trusted container network.

## Volumes and backup

Back up:

- server config and DB1;
- PostgreSQL-backed workflow state and filesystem artifacts;
- KB PostgreSQL;
- vault root-key or external custody metadata;
- TLS enrollment and revocation state;
- both persistent SQLite WORM ledgers, seals, and off-host anchor state;
- workspace mirrors when rebuilding them is expensive.

Use the KB export helper for the embedded database. Test a restore. `docker compose down -v` deletes
named volumes.

## Hardening

- change bootstrap browser credentials;
- keep server and KB networks private;
- verify TLS fingerprints and issue one client identity per machine;
- configure server ID, team ID, and the root-owned management-JWKS trust bundle;
- grant remote users individually and review revoked rows;
- use the split stack if the Docker socket is not required;
- keep delegates networkless by default;
- stream first-boot provider, git, database, and witness secrets into their owning Vault, then
  recreate/start long-lived services without credential environment mappings;
- ship WORM evidence to an off-host witness when host compromise is in scope;
- run exactly one KB WORM worker with its own persistent volume and credential;
- alert on failed health, audit verification, witness lag, bus drops, database pressure, and agent
  reaping.

### Git forge credential

`aimee git pr` and the other forge API calls authenticate with one environment-wide
token, held in the **server principal's** Vault as `(git, forge_token)`. It is the
only slot those calls read, and nothing outside the server ever sees it: agents and
sessions can use `aimee git`, but cannot read the token back out.

Supply it at first boot as `AIMEE_FORGE_TOKEN`, which the server seals and then
unsets. If a deployment came up without it, seal it afterwards instead of
re-creating the stack. Run it as the Vault owner, the same way the webchat seal
does, so the entry is not written by root:

```bash
# inside the server container
printf '%s' "$TOKEN" | runuser -u aimee -- aimee-server --forge-vault-seal forge_token
```

No restart is needed: the next forge call reads the new value straight from
Vault.

The secret travels on stdin only, never argv or an environment mapping, so it
cannot leak through a process list or `/proc`. Re-sealing replaces the value, so
this is also how you rotate. Note that `aimee vault set git forge_token ...` is
**not** a substitute: it stores under the calling principal, which the forge
reader never consults.

Symptom of a missing token: every `aimee git pr` action fails with
`no github credential`, while `aimee git push` keeps working because it
authenticates over SSH.

## Upgrade

See [Upgrading from v0.2.192](UPGRADING.md). Deployment topology changes are data migrations, not
just compose edits.
