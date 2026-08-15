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
event schema.

`dependencies/aimee-repositories.lock.json` records repository URLs, semantic
versions, exact commits, stable principal identities, and source digests.
`python3 scripts/check_c_repository_lock.py` fails when a vendored core/module
mirror drifts from its external repository pin. The vendored mirrors remain in
the main repository during behavioral migration so existing builds do not
silently switch implementations.

The pins bind on `main` only. They describe a release: which published
repository commit each vendored mirror was cut from. So the check runs in the
`c-repository-pins` workflow for pushes to `main` and for pull requests into
`main`, and nowhere else. `testing` and the branches feeding it are the
integration tip: their vendored source is expected to run ahead of any published
repository commit, and enforcing the lock there would only demand a refresh
after every edit under `src/core/**` or `src/modules/**`. For that reason
`repository-lock-check` is not part of `make lint` or `make verify-local`; run
`make repository-lock-check` when you want it. Refresh the pins with
`python3 scripts/export_c_repositories.py --refresh-lock-root <repository-set>`
as part of cutting a release, before opening the `testing` → `main` pull
request.
