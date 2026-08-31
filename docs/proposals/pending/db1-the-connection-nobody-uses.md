# Proposal: the DB1 connection the daemon opens and never uses

- **State:** RESOLVED. The daemon opens no database. Only the module does, plus
  the path-based maintenance tools that operate on the file rather than on rows.

## What is true now

`db1_conn()` is defined in `db1_init.c` and called by **none** of the sources
the daemon still links:

| still linked | `db1_conn()` calls |
| --- | --- |
| `db1_init.c` | 0 |
| `db1_write.c` | 0 |
| `secrets.c` | 0 (never touched DB1) |
| `diagnostics.c` | 0 |
| `maintenance.c` | 0 |

`maintenance.c` and `diagnostics.c` do run SQL, and that is not a
contradiction: they are path-based. Backup, integrity-check and recover open the
file by name and cannot cross a fields wire, which is why they are
infrastructure rather than a family.

So the daemon opens a SQLite handle, keeps it for the life of the process, and
reads and writes nothing through it. Around 48 call sites still say
`db1_init(config_db1_path())`, in the server, and in most of the `cmd_*`
files.

## Why that is not simply dead code to delete

Three things ride on `db1_init` besides the handle:

- **Schema creation.** `db1_init` applies `db_schema`. The module also applies
  it (`aimee_db1_module_init`), so the schema exists either way, but which
  process creates a database that does not exist yet is currently answered by
  whoever starts first, and deleting the daemon's call changes that answer.

- **The CLI with no server.** `aimee` links the generated clients, which call
  over the bus. Standalone, with no daemon and no module, those already fail --
  so the CLI's own `db1_init` is probably vestigial too. "Probably" is doing
  work in that sentence and it should be checked command by command, because the
  failure mode is a CLI subcommand that silently reads an empty database
  instead of reporting that it cannot reach the store.

- **Tests.** Most unit tests call `db1_init(":memory:")` and link the domain
  objects directly. That is the right thing for them to keep doing; this is
  about the production binaries only.

## What it turned out to be

Fewer sites than the count suggested, and one thing the count hid.

Of the ~48 `db1_init` calls, most were in `cmd_*.c` files that ship in no
binary at all, compiled by `cmd-srcs-compile-check` and linked by nothing.
Fourteen were real, and a source-by-source sweep of every shipped binary's link
line found two more that a by-eye reading had missed: `cmd_identity.c` and
`cmd_session_lifecycle.c` both compile into server objects despite their names.
Reading the link line rather than the directory is what caught them.

The guards themselves were the interesting part. Every one asked "did db1_init
succeed", which since the migration answers nothing: opening a local SQLite file
says only that a file exists, not that the store will answer. They ask
`db1_store_ready()` now (whether the module is attached) which is the
question they were always trying to ask.

Two things were wrong in ways the count would never have shown:

- **The server's DB tuning was going to the wrong process.**
  `db1_apply_server_pragmas` sets cache_size, mmap_size and wal_autocheckpoint,
  and the server was applying them to the connection it no longer read or wrote
  through. The module applies them now, being the process that runs the queries.

- **A forked child was about to use the bus.**
  `platform_hooks_background_cleanup` double-forked and reopened DB1 in the
  child, because a SQLite connection does not survive `fork()`. Since sessions
  migrated, the child's work is bus calls instead, and an inherited bus client
  is a socket with a mutex and possibly a request in flight, which a forked
  child can neither use nor repair. It runs synchronously now, as Windows
  always has.

And one regression this change introduced and the tests then caught: the
server's readiness endpoints reported DB1 state from `db1_is_initialized()`,
which became permanently false the moment the server stopped opening the
database. `/v1/server/health` would have reported the store unavailable forever
while every request through it worked. Nothing asserted it, so the suite now
does, verified by inverting `db1_store_ready` and watching the check fail.

## What was worth doing

Delete the daemon's `db1_init` call, and then each CLI one, checking per command
that what remains either goes over the bus or is path-based. The end state is
that only two processes open the database: the module, and the path-based
maintenance tools that operate on the file rather than on rows.

The prize is not tidiness. A daemon holding an open handle to a database it
never uses is a daemon that can still be told to write to it, one `db1_conn()`
away from a new direct caller, and the whole point of this migration was to make
that impossible rather than merely absent.

## Verified on a live server, not only in the harness

The one scenario this change actually alters is a fresh install: the daemon used
to create and migrate the database at startup and no longer does. Run outside
the test harness, on an empty AIMEE_HOME:

- the server started with no database present, and created none. It cannot,
  the symbol is not in the binary;
- the module, started against the same path, created it: 102 tables;
- `/v1/server/health` then reported `"state":"ok"`, from a process holding no
  connection;
- a session created through `/v1/sessions/create` came back from
  `/v1/sessions/list` and was present in the module's file on disk.

Worth doing by hand because the harness starts the module every run, so it
cannot tell "the module created the database" from "something else did".

