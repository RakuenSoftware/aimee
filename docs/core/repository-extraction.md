# C core and module repository extraction

The independently maintained source boundary is materialized with:

```sh
python3 scripts/export_c_repositories.py \
  --output-root /home/virant/dev/aimee-module-repositories
```

The command refuses to overwrite an existing output directory. It creates one
`aimee-core-c` Git repository and one `aimee-module-<id>` Git repository for
every required and optional canonical module. Each repository receives an
`origin` URL under `RakuenSoftware`, a tag matching `src/core/VERSION`, and an exact commit
pin. It does not push or create remote repositories.

The core repository is a standalone installable CMake package. Every module
repository preserves its descriptor-owned sources, headers, tests, and docs,
and builds a separate Linux process against only the host-free event-bus client
target. Its generated grant is executable/UID/principal-bound and starts with no
event capabilities; capabilities are added only with the corresponding stable
event schema. A C module may also declare non-owned `header_dependencies` needed
by a transitional standalone build. Those inputs must be sorted, normalized,
real header files outside the module's own tree; the exporter copies them at
canonical paths without adding them to `owned_files`, records the complete
materialized set as `repository_files`, and includes that set in the source
digest.

Container builds use the same descriptor contract through a two-step runtime
bundle. `export_c_repositories.py --runtime-bundle <directory>` writes generated
process mains, grants, placement lists, and `c-build.json`. Then
`build_c_module_runtime_bundle.py` compiles each C process from its generated
main, every descriptor-owned C source, the canonical event-bus client sources,
and its declared include roots, header dependencies, pkg-config packages, and
system libraries. Header dependencies are admitted as build inputs but never
passed to the compiler as translation units. The
manifest is data rather than a shell fragment: paths and dependency tokens are
validated before they become compiler arguments, and no module source is
concatenated into the generated main.

`dependencies/aimee-repositories.lock.json` records repository URLs, semantic
versions, exact commits, stable principal identities, and source digests.
`python3 scripts/check_c_repository_lock.py` fails when a vendored core/module
mirror drifts from its external repository pin. The vendored mirrors remain in
the main repository during behavioral migration so existing builds do not
silently switch implementations.

Application releases build the bundled core and modules from the Aimee checkout.
They do not publish independent module repositories or require those repositories
to have matching releases. `dependencies/aimee-application-sources.lock.json`
records the bundled source digests, classifications, execution/placement contracts,
and process identities/grants. Its checker uses the same descriptor-owned files
and declared header dependencies as the exporter. Modules with `external_source`
(currently config) still have to match their existing repository pins and Go
dependency version; freezing the application snapshot cannot bypass that check.

The `c-repository-pins` workflow enforces this application snapshot on `main`
and PRs into `main`, and continues to build standalone exports locally on both
integration and release changes. Those fixture exports are never published.
At application promotion, review the source and public-surface diffs, then run:

```sh
python3 -I scripts/check_application_source_lock.py freeze
python3 -I -S scripts/refactor_baselines.py freeze --accept-dirty
python3 -I scripts/check_application_source_lock.py
python3 -I -S scripts/refactor_baselines.py
```

Commit both snapshots with the release preparation. They may go stale during
integration; CI does not automatically refresh them. The historical
`aimee-repositories.lock.json` and `src/core/VERSION` remain unchanged by an
application release. `make repository-lock-check` still verifies all independent
repository mirrors when preparing a separately requested module release.
`export_c_repositories.py --refresh-lock-root <repository-set>` belongs to that
separate workflow and is not an application-release prerequisite.
