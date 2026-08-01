# Commands

The exact command table is generated from the CLI registry: [CLI command reference](gen/cli-commands.md).
This page is the map.

The `aimee` binary is a thin client. A command either performs a small local operation or sends a
typed request to `aimee-server`. It never opens DB1 or DB2.

## Everyday commands

| Task | Commands |
| --- | --- |
| health | `status`, `kb status`, `remote status`, `workers` |
| durable memory | `memory store`, `memory search`, `memory list`, `memory get`, `memory read` |
| session scratch | `wm set`, `wm get`, `wm list` |
| code graph | `index scan`, `index find`, `index callers`, `index structure`, `index blast-radius`, `graph explain` |
| workspaces | `workspace add`, `workspace list`, `workspace remove`, `worktree gc` |
| delegates | `delegate`, `jobs`, `agent`, `provider`, `model`, `persona`, `roles` |
| review panels | `ensemble aggregate`, `ensemble roundtable` |
| workflows | `workflow`, `trigger`, `cron`, `pipeline` |
| CSS migration | `css report`, `css render-capture`, `css render-verify` |
| rules and reusable context | `rules`, `skill`, `toolset` |
| configuration and updates | `config`, `profile`, `remote`, `self-update` |

Use `--json` on server-backed commands when the help advertises it. Human output can change; JSON
fields follow the operation contract.

## Operations and evidence

| Task | Commands |
| --- | --- |
| WORM integrity | `audit verify`, `audit checkpoint`, `audit seal`, `audit snapshot` |
| retrieval evidence | `audit trace`, `audit provenance`, `audit fidelity` |
| runtime diagnosis | `doctor forensics`, `hud`, `insights`, `economizer stats` |
| KB operations | `kb search`, `kb ingest`, `kb docs push`, `curator` |
| job history | `jobs`, `job`, `episode`, `trajectory` |
| optimization | `optimize`, `dogfood`, `code audit` |
| service lifecycle | `server start`, `server restart` |

The event bus has no general remote shell command. Its audit results appear through `aimee audit`,
logs, metrics, and capture tooling. See [Event bus](EVENT_BUS.md).

## Integrations

- `mcp-serve` runs the local MCP stdio bridge.
- `acp-serve` runs the ACP stdio bridge where included in the build.
- `mcp audit` and `mcp recheck` inspect registered MCP packages.
- `api status` prints public endpoint and client setup information.
- `claude-proxy` points Claude Code at the Anthropic-compatible ingress.
- hook entry points are integration commands; call them through generated client configuration, not
  by hand.

## Command tiers

- **Core** commands appear in normal help and carry the strongest compatibility expectation.
- **Advanced** commands appear in `aimee help --all` and are safe for ordinary operators.
- **Admin/internal** commands may require local authority or change with the owning subsystem.

A command is not public merely because a server handler exists. The DB-free client advertises it
only after it has a local implementation or a typed route.

## Removed commands

- `aimee chat`: use the browser, MCP, ACP, or a compatible API client.
- `aimee work`: use workflows, triggers, coordinated jobs, or durable delegate jobs.
- `aimee migrate v2`: current daemons perform supported schema migration at startup.

Bare `aimee` prints usage; it does not launch a TUI.

## Help

```bash
aimee --help
aimee help --all
aimee help <command>
aimee <command> --help
```

Installed help is authoritative for the installed binary. The generated reference is authoritative
for this checkout.
