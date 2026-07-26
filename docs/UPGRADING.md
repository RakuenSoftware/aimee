# Upgrading aimee

Notable user-facing changes, newest first. Each entry says what changed, whether
it can break an existing deployment, and what to do about it.

---

## 2026-07 — The `aimee-kb` image runs its own pgvector when you configure none

**What changed.** The `aimee-kb` image now ships PostgreSQL 18 with the `pgvector`
extension (18.4 + pgvector 0.8.5, from PGDG — the current stable major). If
`AIMEE_DB2_URL` is **unset**, the container initialises and runs its own cluster
under `$AIMEE_HOME/postgres`, reachable only over a local socket. If
`AIMEE_DB2_URL` is **set**, nothing is started and the external server is used
exactly as before — that path is fully supported and is still the right choice for
a shared, backed-up, or managed database.

**pgvectorscale** (StreamingDiskANN indexes) ships in the same image. There is no
separate build or image variant: it costs about 1 MB, needs PostgreSQL 18 which the
image now uses, and the kb already chooses the index type at **runtime** —
`pgvec_vectorscale_available()` probes for the extension and falls back to HNSW with
a warning when it is missing. Making it a build flag would have turned the index
type into a property of which image you pulled. Configure the index type as you
always have; nothing about the image selects it.

The image no longer bakes `AIMEE_DB2_URL=postgresql://aimee:aimee@postgres:5432/aimee_shared`.
That default made "the operator configured nothing" indistinguishable from "use the
sibling container", so the container could not tell when to run its own database —
and a bare `docker run` inherited a `postgres` hostname that does not resolve.

**Does this affect me?** Not if you use `compose.yaml`, `compose.server.yaml`, the
SmoothNAS units, or `deploy/`. All of them already set `AIMEE_DB2_URL` explicitly,
so they keep their own `postgres` service and their existing volume untouched.
Nothing to do, and no data moves.

You are affected only if you ran the `aimee-kb` image **without** setting
`AIMEE_DB2_URL` and relied on the baked default to reach a container named
`postgres`. Set it explicitly to keep that behaviour:

```
docker run -e AIMEE_DB2_URL=postgresql://aimee:aimee@postgres:5432/aimee_shared ...
```

**Why.** An unconfigured deployment previously had no working vector store, and the
default pulled `pgvector/pgvector` from Docker Hub at run time — an anonymous pull,
subject to a shared per-IP quota that fails as a hung connection rather than a clear
error. Shipping the engine in the image removes a third-party registry from the
production start path.

**Moving to an external server.** The image ships `aimee-kb-db-export` for exactly
this:

```
docker exec aimee-kb aimee-kb-db-export postgresql://user:pw@host:5432/aimee_shared
docker exec aimee-kb aimee-kb-db-export --wipe postgresql://user:pw@host:5432/aimee_shared
```

It refuses to start if the target is unreachable or lacks the `vector` extension,
dumps and restores, then compares the row count of every user table and **aborts
leaving the internal data intact** if they differ. `--wipe` removes the internal
data directory only after that comparison passed. Set `AIMEE_DB2_URL` to the target
afterwards and the container stops starting its own cluster.

---

## 2026-07 — The `plugin-loader` is removed

**What changed.** aimee's built-in `plugin-loader` subsystem has been removed
entirely. There is no longer a plugin discovery/registry/enable-disable
mechanism inside aimee, and the endpoints, config keys, and CLI that drove it are
gone. Extensibility in aimee is delivered through **hooks**, **MCP tools**, and
**skills** (all documented in `MANUAL.md`) and, for maintainers, through
first-class **modules** — not through a separate plugin loader.

**Does this affect me?** Only if you actively used the plugin loader. Concretely,
you are affected if any of these applied to your deployment:

- you set `AIMEE_ENABLE_PROJECT_PLUGINS`;
- you shipped a project-local plugin manifest or plugin directory for aimee to
  discover;
- you called the plugin HTTP routes — `GET /v1/plugins`, `POST /v1/plugins/enable`,
  `POST /v1/plugins/disable`, or `GET /v1/dashboard/plugins`;
- you scripted the plugin management subcommands.

If none of those applied, this change is transparent — a normal upgrade needs no
action.

**What was removed.**

| Removed | Replacement |
|---|---|
| `AIMEE_ENABLE_PROJECT_PLUGINS` env var | — (no equivalent; use a mechanism below) |
| `GET /v1/plugins`, `POST /v1/plugins/{enable,disable}` | — (now `404`) |
| `GET /v1/dashboard/plugins` | — (now `404`) |
| Plugin discovery of project-local plugin manifests | MCP tools / hooks / skills |
| Plugin management CLI | — |

The `/v1/openapi.yaml` served by `aimee-server` no longer lists any plugin route,
so generated clients pick the change up automatically.

**What to do instead.** Pick the mechanism that matches what your plugin did:

- **You added a tool the model could call.** Expose it over **MCP** and register
  the MCP server with aimee. This is the supported way to add callable tools and
  works with every client. See *§16 Skills and toolsets* and the MCP integration
  notes in `MANUAL.md`.
- **You intercepted or post-processed tool calls / injected context.** Use the
  client **hooks** aimee already registers — `SessionStart`, `PreToolUse`,
  `PostToolUse` — described under *Hooks* in `MANUAL.md`. These cover the
  interception and context-injection cases the plugin `pre-LLM` hook was used for.
- **You bundled reusable prompts/procedures.** Package them as a **skill**
  (`AIMEE_BUNDLED_SKILLS_DIR` still overrides the bundled-skills location).
- **You are a maintainer extending aimee's own binary.** Add a first-class
  **module** under `src/modules/<id>/` with a `module.yaml` descriptor, rather
  than a loadable plugin. See `docs/modules/` and `docs/refactor-baselines.md`.

**Note on Codex.** The "local plugin" line for the Codex CLI in `MANUAL.md` /
`docs/COMPATIBILITY.md` refers to *Codex's own* plugin mechanism, not aimee's
plugin-loader, and is unaffected.

---
