# aimee Agent Reference

Call `get_help` (MCP tool) at the start of every session before doing any work.
Returns this document: current routed client commands, MCP tools, workflows,
and conventions.

---

## MCP Tools

Call these directly without CLI. All available via the MCP server.

| Tool | Purpose |
|------|---------|
| `get_help` | This document. Call at session start. |
| `search_memory query` | Server-backed retrieval over stored facts |
| `list_facts` | List all L2 facts |
| `get_host name` | Look up host from network inventory |
| `list_hosts` | List all hosts |
| `find_symbol identifier` | Find code symbol (function/class/var) with file+line |
| `delegate role prompt` | Route task to sub-agent |
| `preview_blast_radius project paths` | Impact analysis before editing |
| `record_attempt` | Log failed approach (task_context, approach, outcome, lesson) |
| `list_attempts [filter]` | List failed approaches this session |
| `create_note title content [tags]` | Create/append investigation note |
| `list_notes [tag] [limit]` | List notes |
| `search_notes query` | Search notes by content/title |
| `git_status` | Working tree status |
| `git_commit message [files]` | Stage + commit |
| `git_push [force] [mirror]` | Push to origin |
| `git_branch action [name] [base]` | create/switch/list/delete/claim/orphan |
| `git_log [count] [ref] [diff_stat]` | Commit log |
| `git_diff_summary [ref] [stat_only] [files]` | Diff summary |
| `git_pr action [title] [body] [number] [base]` | create/view/list/edit/checks/watch/merge_status |
| `git_pull [rebase]` | Pull from remote |
| `git_clone url [path] [branch] [depth]` | Clone repo |
| `git_stash action [message] [index]` | push/pop/apply/list/drop |
| `git_tag action [name] [message] [ref]` | create/list/delete |
| `git_fetch [prune] [remote]` | Fetch without merging |
| `git_reset [ref] [mode]` | soft/mixed/hard reset |
| `git_restore files [staged] [source]` | Restore or unstage files |
| `git_verify [action] [async] [job_id] [base]` | Project verification, health, conflicts, and PR prep |

---

## Delegate

Primary agents must not use provider-native sub-agent tools such as Codex
`spawn_agent`, Claude `Agent`, or remote-agent launchers. Use the Aimee
`delegate` MCP tool for every delegated or parallel sub-task.

MCP delegate calls run in the background by default and return a `job_id`;
poll `delegate_status` for the result. CLI delegate calls remain synchronous
unless the command itself offers a background mode.

```
aimee delegate <role> "<prompt>"
aimee delegate <role> "<prompt>" --tools          # enable bash + file tools
aimee delegate <role> "<prompt>" --max-tokens N
aimee delegate <role> --prompt-file PATH          # prompt from file
aimee delegate <role> "task summary" --prompt-file PATH
```

Common roles: `code`, `review`, `explain`, `refactor`, `draft`, `summarize`,
`deploy`, `validate`, `test`, `diagnose`, `execute`

Read-only delegates use the parent session worktree. Only write-capable
delegates get sibling delegate worktrees.

---

## Memory CLI

```
aimee memory search "<query>"
aimee memory store <key> <value> --tier working|L2|L3 --kind fact|task|decision
aimee memory list [--tier T] [--kind K]
aimee memory get <id>
```

---

## Code Index

```
aimee index find <symbol>
aimee index list
```

For delegates, the code index is authoritative for discovery: use it to find
symbols, likely files, and broader structure before falling back to broad shell
search. Current source is authoritative for file contents. Source packets from
`--files`, `--context-file`, `--context-dir`, and same-worktree `read_file`
results override indexed snippets when they differ; index-backed tool results
may include freshness metadata such as `source_packet_current` or
`worktree_differs_from_main`. Main checkouts are re-indexed when their HEAD
differs from the last successfully indexed HEAD; the marker advances only after
the KB scan succeeds.

---

## Verification

### Verification discipline

1. **Red before green.** To claim something's broken, show the failure in the
   stock, unmodified system first, a red baseline. No red baseline, no saying
   "broken." A passing test of your fix is not admissible as evidence the original
   failed; that's treatment, not control.
2. **The deciding test can't be the one you skipped.** When the true end-to-end is
   unrunnable (e.g. a live Moonlight session), that's a stop. Downgrade to
   "hypothesis, unverified", explicitly, in those words, and do not open a
   PR-as-fix or write a root cause as fact. The thing you couldn't verify is
   exactly the thing you're most likely wrong about.
3. **Code outranks your repro.** When the system's own source contradicts your
   repro, the repro is guilty until you can explain the gap. Reading `need_context`
   and overriding it with a worse signal is backwards.

### The `git_verify` tool

The `git_verify` tool is a Swiss Army Knife for project health and Git lifecycle.

```
# Core actions
git_verify action=run                     # run project verification steps (default)
git_verify action=run async=true          # run in background, returns job_id
git_verify action=status job_id=N         # check status of background job
git_verify action=check                   # quick check of last verification state

# Diagnostics and Helpers
git_verify action=conflicts               # semantic conflict resolver with commit context
git_verify action=env                     # check environment for required project tools
git_verify action=prepare-pr [base=main]  # full PR readiness report
```

### Configuration

Define steps in `.aimee/project.yaml`:

```yaml
env_check:
  - go
  - node
  - gcc

verify:
  enforce: true
  incremental: false
  always_run_globs: Makefile,CMakeLists.txt,.aimee/project.yaml
  steps:
    - name: build
      run: make
      paths: src/**,CMakeLists.txt
    - name: test
      run: make unit-tests
      after: build
```

`incremental: true` is opt-in. With a passing baseline in
`.aimee/.last-verify`, steps with `paths:` that do not match the changed
files are reported as `SKIP`; steps without `paths:` still run. A step may set
`scope: changed` to receive `AIMEE_VERIFY_CHANGED_MATCHED`,
`AIMEE_VERIFY_CHANGED_FILES_FILE`, `AIMEE_VERIFY_BASELINE_REF`, and
`AIMEE_VERIFY_CHANGED_ALL`.

---

## Build & Test

```
cd src && make all server                 # build shipped artifacts
cd src && make unit-tests                 # run unit tests
cd src && make lint                       # clang-format check
pkill aimee-server; cd src && make all server             # rebuild before restart
```

Binaries land at repo root (`../aimee`, `../aimee-webchat`, `../aimee-server`, `../aimee-kb`).

---

## PR Workflow

```
git checkout -b feat/my-feature main
# implement
git commit -m "feat: short description"
gh pr create --title "feat: short description" --body "..."
```

PRs target `main`. Wait for CI before merging.

---

## Conventions

- Commit prefix: `fix:`, `feat:`, `chore:`. Subject ≤72 chars
- No AI attribution in commits or PR bodies
- Layer architecture: L0 Foundation → L1 Data → L2 Agent → L3 Commands
- Lower layers must not include higher-layer headers

---

## Diagnostics

```
aimee status           # system health overview
aimee hud              # current session telemetry
aimee git verify       # project verification before merge
```
