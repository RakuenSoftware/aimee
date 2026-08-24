# Contributing

Small, finished changes are easiest to review.

## Before you change code

1. Search memory and the code graph before reading the tree by hand.
2. Read [OWNERS.md](OWNERS.md) for module and storage boundaries.
3. Check [docs/proposals](docs/proposals/) for an accepted design or an owner already doing the work.
4. Preview the blast radius for shared symbols, routes, config, storage, and wire contracts.

Do not cross the DB1/DB2 boundary. The server's store and `aimee-kb` are separate databases with
separate owners, and the thin client owns neither.

This used to read "`aimee-server` owns SQLite", which is no longer true and would send you to the
wrong place. The server's store is PostgreSQL, served by the `aimee` module, which opens no database
itself: it reaches the `postgres` module over the event bus, and that module owns the connection and
the DSN (`AIMEE_STORE_URL`). `aimee-kb` owns DB2 and pgvector, also PostgreSQL.

`aimee-server` does still link libsqlite3, for the audit WORM ledger, PKI and kb-synthesis. Those are
the remaining SQLite users and they are unrelated to the store. `aimee` and `aimee-kb` link none:
`make -C src check-linking` asserts each binary's boundary and prints what it found.

## Build and test

From `src/`:

```bash
make -j4
make unit-tests
make lint
make docs-gen-check
```

Run the narrow test target while iterating. Run the full unit suite before sending a change. Add
ASAN or TSAN for memory ownership, concurrency, event-bus, and shutdown work.

`make unit-tests` links DB2 against a sqlite shim that *translates* its SQL, so engine-level
behaviour is unverified by it. `make unit-tests-pg` runs the same binaries against a real
PostgreSQL, which is what CI gates on:

```bash
make unit-tests-pg AIMEE_TEST_DB2_TEMPLATE_URL=postgresql://user@host/aimee_test_tpl
```

It rebuilds the template database, then clones it per test process. Point it at a disposable
server: it creates and drops databases beside the template. Touching DB2 SQL without running it is
how a statement that Postgres rejects outright can sit in a green tree.

The Makefile is canonical. Keep CMake in sync for Windows and macOS builds.

## Change contracts, not copies

- Commands come from the command registry and CLI help data.
- `/v1` routes come from the route descriptors and OpenAPI sources.
- Configuration comes from the field descriptors.
- Event-bus frames and kinds are versioned contracts shared by C and Go.

Regenerate documentation after changing one of those sources. Do not hand-edit files under
`docs/gen/`.

## Guards and their exceptions

A guard nothing runs is not a guard. Wire a new check into `LINT_CHECKS` in `src/Makefile` in the
same change that adds it.

**An exception must be able to expire on its own.** An exemption that can only be removed by someone
remembering to remove it will outlive the reason for it and still read as coverage. Give every
exception a condition the code can check: an entry that matches no files fails, a conditional
exemption retires when its condition goes false, an acceptance table fails when reality drifts from
it. Three separate guards in this repository arrived at that rule independently.

**Exempt exact paths, not prefixes.** A prefix exemption covers packages that do not exist yet,
including the one the guard was written for. `server-go/internal/peer/` owned a session directory,
message inboxes and an authorization table while three validators reported ok, because descriptors
are read to find what to *build* and a file nobody declares is never looked at. A prefix exemption
for `server-go/internal/` would have permitted it in silence: a guard that passes over its own
motivating case while looking thorough.

**Name debt as debt.** When a check finds something that needs an architectural decision, such as a
principal to allocate or an owner to choose, record it with what resolving it would take. Do not
invent the decision to make the check pass, and do not exempt it quietly: a silent exemption turns a
gap into a blessing.

**Verify a guard by breaking what it guards.** Re-introduce the defect and confirm the check fails
with a diagnostic that names it. A guard that has only ever been seen passing has not been tested.

## Documentation

Follow [Documentation voice and maintenance](docs/WRITING.md). Write short, direct prose. State what
is true, the boundary, and the failure behavior. Put current usage in a guide and design history in a
proposal. Do not copy command or configuration tables that can be generated.

Update [README.md](README.md), [What's new](docs/WHATS_NEW.md), and [status](docs/STATUS.md) when a
change alters the product surface.

Run `python3 scripts/check-docs.py` after changing maintained documentation.

## Pull requests

Keep one purpose per PR. Include:

- the problem and the contract changed;
- tests run and their result;
- compatibility or migration notes;
- security and failure behavior when the change crosses a trust boundary.

Do not include generated artifacts without their source change.

### The public-surface baseline gates `main`, not `testing`

`tests/baselines/refactor/index.json` is a digest of the PUBLIC surface: 370
public headers, the routes, the schemas, the CLI help. Asking "did the public
surface change, and did you mean it?" is a release question, so it is enforced
on pull requests into `main` and not on the integration branch.

On `testing` it was answering a question nobody asked. A branch whose job is to
absorb in-flight work touches public headers constantly, so every PR re-froze
the digest, and any two concurrent PRs then collided on the same regenerated
lines. It cost nineteen rebases in one migration and surfaced no real change to
the surface in any of them.

So on `testing`: do not re-freeze it. `make -C src lint` no longer asks you to,
and CI now REFUSES a pull request into `testing` that changes the baseline. If
you have already re-frozen it, revert that one file and push again:

    git checkout origin/testing -- tests/baselines/refactor/index.json

Adding or changing a public header does NOT require a re-freeze. The baseline is
expected to be stale on `testing`; that is what makes the promotion diff worth
reading.

When promoting `testing` into `main`:

    make -C src refactor-baseline-check              # what changed on the surface?
    python3 -I -S scripts/refactor_baselines.py freeze
    make -C src script-tests                         # every scripts/tests test

That diff is the point. One review of everything a release changes on the public
surface beats a mechanical re-freeze per PR that nobody reads.

`script-tests` discovers `scripts/tests/test_*.py` by wildcard and runs each one,
rather than naming them. The workflows invoke these individually, so the set that
runs is an enumerated list: eleven of forty-four were named by no workflow and
ran nowhere at all. All eleven passed once run, so it was idle coverage rather
than rot, and one of them could not run under `python3 -I` at all, because it
imported its subject as a package instead of loading it by path, which is why it
could never have been wired in the ordinary way.

It belongs HERE rather than in `make lint` on purpose. Some of these tests are
promote-time gates by deliberate design. `.github/workflows/module-inventory.yml`
says so outright, that the module boundary and the frozen DB2 consumer surface
are "promote-time questions, decided when work is promoted to `main`", and that
running them on `testing` would block ordinary feature work on a decision nobody
is making at that point. Pulling the whole set into the PR gate would override
that on the quiet. Run it when you promote, which is when those questions are
being asked.

If you do hit a conflict in a generated baseline anyway, do not merge two
digests by hand. Take either side and re-derive it:

    git checkout --theirs -- tests/baselines/refactor/index.json
    python3 -I -S scripts/refactor_baselines.py freeze --accept-dirty
    git add tests/baselines/refactor/index.json && git rebase --continue

Do NOT reach for a git merge driver here, however tempting. A driver runs DURING
the merge, before git has necessarily written every merged file, so a whole-tree
digest computed there is computed against a half-written tree. Measured: with a
driver configured the rebase completed CLEAN and produced a baseline that did
not match the tree, where the same rebase without it conflicted loudly. A quiet
mismatch is worse than a conflict, so regenerate after the merge, when the tree
is final.
