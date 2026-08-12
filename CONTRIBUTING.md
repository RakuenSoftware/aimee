# Contributing

Small, finished changes are easiest to review.

## Before you change code

1. Search memory and the code graph before reading the tree by hand.
2. Read [OWNERS.md](OWNERS.md) for module and storage boundaries.
3. Check [docs/proposals](docs/proposals/) for an accepted design or an owner already doing the work.
4. Preview the blast radius for shared symbols, routes, config, storage, and wire contracts.

Do not cross the DB1/DB2 boundary. `aimee-server` owns SQLite. `aimee-kb` owns PostgreSQL and
pgvector. The thin client owns neither.

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

The Makefile is canonical. Keep CMake in sync for Windows and macOS builds.

## Change contracts, not copies

- Commands come from the command registry and CLI help data.
- `/v1` routes come from the route descriptors and OpenAPI sources.
- Configuration comes from the field descriptors.
- Event-bus frames and kinds are versioned contracts shared by C and Go.

Regenerate documentation after changing one of those sources. Do not hand-edit files under
`docs/gen/`.

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

### When `testing` moves under you

`tests/baselines/refactor/index.json` is generated: a digest per file of the
public surface. Two branches touching the same file rewrite the same digest
line, so rebasing onto a moved `testing` conflicts there routinely. Neither
side's bytes are the answer: the answer is whatever the merged tree produces.

    make -C src resolve-generated     # regenerate it, stage it
    git rebase --continue

This is a script rather than a git merge driver deliberately. A driver runs
DURING the merge, before git has necessarily written every merged file, so a
whole-tree digest computed there can be silently wrong: measured, a driver made
the rebase complete clean with a baseline that did not match the tree. A loud
conflict is better than a quiet mismatch, so the conflict stays and only its
resolution is automated.

The check itself is unchanged and still runs in CI. If you regenerate a baseline
you did not mean to change, that is a real finding. Read the diff.
