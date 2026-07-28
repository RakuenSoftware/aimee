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

Write short, direct prose. State what is true, the boundary, and the failure behavior. Put current
usage in a guide and design history in a proposal. Do not copy command or configuration tables that
can be generated.

Update [README.md](README.md), [What's new](docs/WHATS_NEW.md), and [status](docs/STATUS.md) when a
change alters the product surface.

## Pull requests

Keep one purpose per PR. Include:

- the problem and the contract changed;
- tests run and their result;
- compatibility or migration notes;
- security and failure behavior when the change crosses a trust boundary.

Do not include generated artifacts without their source change.
