# scripts/

Helper scripts that aimee can shell out to.  Installable anywhere, but
they're kept here so they track the schema of the APIs they plumb into.

## `proposal_stats.py`, inspect the docs/proposals/ pipeline

Read-only stats inspector for the proposal-tracking pipeline under
`docs/proposals/`. Scans `docs/proposals/pending` and `docs/proposals/done`
relative to the repo root (resolved from the script's own location, not
the caller's cwd) and reports the proposal counts plus the total word
count of the pending bucket.  Pure read-only, stdlib-only Python.

### Usage

```bash
python3 scripts/proposal_stats.py            # human-readable summary
python3 scripts/proposal_stats.py --json     # one JSON object on stdout
```

Use `--json` when piping the numbers into another tool or dashboard;
the human-readable form is the default for ad-hoc shell runs.

## `check_tier_deps.sh`, enforce DB1/DB2 tier boundaries

Validates dependency boundaries for the two-database architecture (DB1 = local
SQLite owned by `aimee-server`; DB2 = shared Postgres owned by `aimee-kb`,
including the in-process pgvector extension):

- `sqlite3.h` only in `src/db1/`
- `libpq-fe.h` only in `src/db2/`
- `aimee_stores_` / `aimee_stores_t` / `PGconn` / `PGresult` / `PQ*` helper symbols / `sqlite3_` only in tier-owned source trees
- no legacy `project_store_` lifecycle aliases outside `src/db2`
- no legacy DB2 backend selector outside `src/db2`
- DB2 transitional SQLite lifecycle names stay inside `src/db2`
- DB2 public lifecycle headers hide SQLite backend knowledge
- DB2 public lifecycle headers hide shim backend vocabulary
- DB2 shared-store lifecycle APIs expose neutral names outside `src/db2`
- DB2 transaction primitives stay inside `src/db2`
- Legacy/helper vocabulary outside `src/db1` avoids SQLite names
- Worker-shell comments outside `src/db1` avoid SQLite names
- Vector verify row-count fields expose DB2 names instead of SQLite names
- Doctor DB output exposes DB2 names instead of Postgres names
- Non-DB2 source comments avoid Postgres/libpq implementation names
- No Qdrant HTTP collection/points URL paths in `src/` (vector store is now
  pgvector inside DB2, in-process, no HTTP sidecar)

Most checks scan `src/**/*.c`, `src/**/*.h`, and `src/**/*.inc` outside
`src/db1`, `src/db2`, `src/tests`, and `src/webchat`.

Run manually:

```bash
./scripts/check_tier_deps.sh
```

The `lint` CI job also runs this check.

Binary ownership is checked by `cd src && make check-linking`; that target
verifies the process boundary:

- `aimee-client` and `aimee-webchat` stay DB-free (`libsqlite3` and `libpq` absent).
- `aimee-server` stays DB1-only (`libsqlite3` present, `libpq` absent, no
  DB2 symbols).
- `aimee-kb` stays DB2-only (`libpq` present, `libsqlite3` absent, no
  DB1/sqlite symbols).

## `llm-chat.py`, generic OpenAI-compat chat client

Reads a prompt from `--prompt`, a positional argument, or stdin; posts to
an OpenAI-compat `/v1/chat/completions`; writes the reply's content to
stdout.  Works with any endpoint that speaks the OpenAI chat spec:

- OpenAI (`https://api.openai.com/v1`)
- Local `llama-server` (e.g. `http://host:8080/v1`)
- Ollama (`http://host:11434/v1`)
- vLLM, LM Studio, Together, Groq, …

All config via env vars (set once in aimee config) with CLI-flag overrides:

| Variable | Purpose |
|---|---|
| `LLM_ENDPOINT` | base URL (default `https://api.openai.com/v1`) |
| `LLM_MODEL` | model id (**required**) |
| `LLM_API_KEY` | bearer token; `cmd:<shell>` runs a command and uses its stdout |
| `LLM_SYSTEM` | default system prompt |
| `LLM_TEMPERATURE` | default `0.0` |
| `LLM_MAX_TOKENS` | default `2048` |
| `LLM_NO_THINKING` | `1` to disable Qwen-family hybrid-thinking tokens |
| `LLM_TIMEOUT` | seconds, default `120` |
| `LLM_RETRIES` | default `2` (5xx + network retries with backoff) |

Example, local Qwen3.6 via llama-server:

```bash
export LLM_ENDPOINT=http://192.168.0.115:8080/v1
export LLM_MODEL=qwen3.6
export LLM_NO_THINKING=1

echo "Summarise: typed extraction beats semantic-only by 0.16 P@5" \
    | ./scripts/llm-chat.py
```

Example, OpenAI with a secret manager:

```bash
export LLM_MODEL=gpt-4o-mini
export LLM_API_KEY="cmd:op read 'op://vault/openai/key'"
./scripts/llm-chat.py --prompt "ping"
```

Stdlib-only Python; runs on any 3.9+.

## `llm-rewrite.py`, `memory_rewrite_command` wrapper

Plugs an OpenAI-compat endpoint into aimee's query-rewrite pipeline
(HyDE + decomposition; see `src/memory_core_search.inc:memory_query_rewrite`).
Reads aimee's request JSON on stdin, calls `llm-chat.py`, returns the
rewrite JSON on stdout.  Silent fallback to `{}` on any error so a flaky
endpoint doesn't spam aimee's log with rewrite failures.

### Wiring

```bash
aimee config set memory_rewrite_command "python3 $(pwd)/scripts/llm-rewrite.py"
aimee config set memory_rewrite_enabled 1
aimee config set memory_rewrite_hyde 1
aimee config set memory_rewrite_decompose 1
```

(Env vars from `llm-chat.py` apply, same `LLM_ENDPOINT` / `LLM_MODEL` / …
that any other sidecar call uses.  Set them in `aimee-server`'s
environment or via your shell profile.)

Inherits the same generic config surface, nothing Qwen-specific in the
config path, except `LLM_NO_THINKING=1` for thinking-capable models that
would otherwise burn their token budget on `<think>` content.

## Writing your own sidecar

Any aimee config that expects a CLI (`memory_rewrite_command`,
`memory_query_expansion_command`, `embedding_command`, …) can be backed
by a small wrapper that:

1. Reads whatever JSON aimee pipes in on stdin (see the grep in
   `src/memory_core_search.inc` or the doc comment above the caller).
2. Constructs a prompt and calls `llm-chat.py`.
3. Parses the reply and emits the JSON the caller expects on stdout.

Keep the wrapper small; let `llm-chat.py` do the HTTP + retry work.
