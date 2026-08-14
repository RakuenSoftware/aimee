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

```bash
docker compose -f deploy/compose/aimee.yaml up -d
```

Server and one KB are declared together, and no browser action needs to create them. The KB owns its
embedding and synthesis role placements. Each role can run inside the KB container or use a remote
endpoint supported by the selected profile. There is no separate inference service. This is the
safer default when the server must not control Docker.

The one-KB Compose files are deployment profiles, not the fleet limit. The target architecture can
route among several KB containers with explicit corpus, authority, and capability identity. Fleet
routing is not integrated in this checkout; see [KB fleet and model placement](KB_FLEET.md).

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
- workflow SQLite and artifacts;
- KB PostgreSQL;
- vault root-key or external custody metadata;
- TLS enrollment and revocation state;
- WORM ledger, seals, and off-host anchor state;
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
- alert on failed health, audit verification, witness lag, bus drops, database pressure, and agent
  reaping.

### Git forge credential

`aimee git pr` and the other forge API calls authenticate with one environment-wide
token, held in the **server principal's** Vault as `(git, forge_token)`. It is the
only slot those calls read, and nothing outside the server ever sees it: agents and
sessions can use `aimee git`, but cannot read the token back out.

Supply it at first boot as `AIMEE_FORGE_TOKEN`, which the server seals and then
unsets. If a deployment came up without it, seal it afterwards instead of
re-creating the stack:

```bash
printf '%s' "$TOKEN" | aimee-server --forge-vault-seal forge_token
```

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
