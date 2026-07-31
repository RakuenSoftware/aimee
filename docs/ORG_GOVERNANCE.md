# Teams, budgets, and rate limits

Every governance control lives on the knowledge base, not the server, and is driven by the
`aimee-kb` binary rather than the `aimee` client. That is why none of it appears in the
[command reference](gen/cli-commands.md), which documents the client.

The knowledge base owns the data these controls apply to, so it owns the controls. A server is a
member of a team; it does not decide what that team may spend.

## Run these inside the KB container, over its private socket

The routes behind these commands are never exposed to a remote bearer. Reach them through the
container:

```bash
KB=$(docker ps --filter label=com.docker.compose.service=aimee-kb --format '{{.ID}}')
docker exec -e 'AIMEE_DB2_URL=postgresql:///aimee_shared?host=/var/lib/aimee/run' "$KB" \
  aimee-kb team list
```

Every example below is the `aimee-kb ...` part of that.

## Teams own projects, and projects scope everything else

```bash
aimee-kb team create <name>
aimee-kb team list
aimee-kb team add-member <team> <subject>
aimee-kb team remove-member <team> <subject>

aimee-kb project create <team> <name>
aimee-kb project list --team <team>
```

A team is the unit a budget, a rate limit and a grant all attach to. Create one before anything
else; the managed wizard's Deploy step creates a `default` team for you.

## A catalog decides which models exist for whom

```bash
aimee-kb models list
aimee-kb models org add <model_id> <provider> <anthropic|openai|responses|gemini> \
  [display_name] [endpoint] [--disabled]
aimee-kb models org set <model_id> ...        # same shape, replaces the entry
aimee-kb models org remove <model_id>
aimee-kb models org entitle   <model_id> <team_id>
aimee-kb models org unentitle <model_id> <team_id>
```

`add` puts a model in the org catalog. `entitle` is what lets a specific team use it. A catalogued
model nobody is entitled to is visible and unusable, which is the intended shape for staging a model
before opening it up.

The fourth argument is the **wire format**, not the vendor: a third-party model served over
Anthropic's wire takes `anthropic`. Getting this wrong routes the request through the wrong
translation.

## Budgets stop spending, in the currency you were billed in

```bash
aimee-kb budget set --team <id> [--project <id>] --period day|month --limit <USD> [--soft <USD>]
aimee-kb budget show --team <id> [--project <id>]
```

`--limit` is the hard stop. `--soft` warns first and keeps serving, which is the one to set when you
want to hear about a runaway job rather than have it fail mid-run.

A project budget narrows a team budget; it does not widen it.

## Rate limits apply to five different things

```bash
aimee-kb rate set --dim team|project|cert|model|cred_slot --scope <S> --window <SECS> --max <N>
aimee-kb rate show --dim <D> --scope <S>
```

Pick the dimension that matches the abuse you are actually preventing:

- `team` and `project` bound a group's throughput;
- `cert` bounds one enrolled client, which is the one to reach for after a runaway loop on a single
  machine;
- `model` protects a specific upstream from everyone at once;
- `cred_slot` bounds one credential, which matters when several teams share a provider key.

## Spend reports are read-only and exact

```bash
aimee-kb spend --team <id> [--project <id>] [--since YYYY-MM-DD] [--until YYYY-MM-DD] [--json]
```

`--team` may be omitted for an org-wide report when the caller passes the admin gate.

Costs are NUMERIC strings, never floats, all the way out to the JSON. Do not parse them into a
binary float and compare for equality; you will get the wrong answer eventually and it will be a
rounding error nobody can reproduce.

## Telemetry is allowlisted per schema

```bash
aimee-kb telemetry show
aimee-kb telemetry allow --schema <S> --metrics a,b,c [--disabled]
```

Nothing is exported until a schema names the metrics it may export. `--disabled` registers the
allowlist without turning it on, so the set can be reviewed before it emits anything.

## These commands are missing from `aimee-kb --help`

`--help` lists only `enroll` and the `vault` subcommands. `team`, `project`, `models`, `spend`,
`budget`, `rate` and `telemetry` all dispatch (`src/kb/kb_main.c`), and each prints its own usage
when called with no arguments. Until the help text catches up, this page is the index.
