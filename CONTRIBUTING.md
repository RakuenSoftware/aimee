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

That diff is the point. One review of everything a release changes on the public
surface beats a mechanical re-freeze per PR that nobody reads.

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
