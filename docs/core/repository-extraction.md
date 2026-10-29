# Module repository extraction

The independently maintained source boundary is materialized with:

```sh
python3 scripts/export_c_repositories.py \
  --output-root /home/virant/dev/aimee-module-repositories
```

The command refuses to overwrite an existing output directory. It creates one
`aimee-module-<id>` Git repository for every required and optional canonical
module. The C core (`src/core`: codec, connection, types) is not exported: it is
built from this tree and bundled, and `core-c-package` CI proves it installs and
builds a consumer on Linux, macOS and Windows. The local ring is the ouroboros
ring, a repository of its own that modules link as a release. Each repository receives an
`origin` URL under `RakuenSoftware`, a tag matching `src/core/VERSION`, and an exact commit
pin. It does not push or create remote repositories.

Every module
repository preserves its descriptor-owned sources, headers, tests, and docs,
and builds its declared execution target. Every module process is Go: the catalog has 26 Go process identities and
no C process identities, and the exporter refuses a contract naming any other
runtime. Go processes use the shared module runtime and event-bus contract;
retained native sources are the C core's caller-side clients and host
adapters. Its generated grant is executable/UID/principal-bound and starts with no
event capabilities; capabilities are added only with the corresponding stable
event schema.

Container builds use the same descriptor contract through the runtime bundle.
`export_c_repositories.py --runtime-bundle <directory>` writes grants, placement
lists, `go.modules` and the manifest; the image installs the static Go
multicall runtime once per module named in `go.modules`, under that module's
attested executable name. There is no C module-process build: the generated C
process mains, `c-build.json` and `build_c_module_runtime_bundle.py` were
retired with the last C module adapter.

`dependencies/aimee-repositories.lock.json` records repository URLs, semantic
versions, exact commits, stable principal identities, and source digests.
`python3 scripts/check_c_repository_lock.py` fails when a vendored module
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

The `Module exports and runtime validation` workflow enforces this application snapshot on `main`
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
