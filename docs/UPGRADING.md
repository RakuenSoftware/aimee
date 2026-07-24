# Upgrading aimee

Notable user-facing changes, newest first. Each entry says what changed, whether
it can break an existing deployment, and what to do about it.

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
