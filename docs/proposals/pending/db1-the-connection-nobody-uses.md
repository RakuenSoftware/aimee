# Proposal: the DB1 connection the daemon opens and never uses

- **State:** OPEN, and newly true. Every DB1 family is served, no source the
  daemon links calls `db1_conn()`, and the daemon still opens the database.

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
`db1_init(config_db1_path())` -- in the server, and in most of the `cmd_*`
files.

## Why that is not simply dead code to delete

Three things ride on `db1_init` besides the handle:

- **Schema creation.** `db1_init` applies `db_schema`. The module also applies
  it (`aimee_db1_module_init`), so the schema exists either way -- but which
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

## What is worth doing

Delete the daemon's `db1_init` call, and then each CLI one, checking per command
that what remains either goes over the bus or is path-based. The end state is
that only two processes open the database: the module, and the path-based
maintenance tools that operate on the file rather than on rows.

The prize is not tidiness. A daemon holding an open handle to a database it
never uses is a daemon that can still be told to write to it -- one `db1_conn()`
away from a new direct caller, and the whole point of this migration was to make
that impossible rather than merely absent.
