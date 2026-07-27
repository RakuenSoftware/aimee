# Scripts

Scripts are build gates, generators, deployment helpers, smoke tests, benchmarks, and narrow
sidecars. Keep each one tied to a source contract and safe to run from the repository root unless its
header says otherwise.

## Main groups

| Prefix | Purpose |
| --- | --- |
| `check_*` | static or mechanical contract gate |
| `test_*` | live integration/conformance harness |
| `gen-*` / `gen_*` | generated docs, APIs, schemas, SDKs, fixtures |
| `aimee-*` | deployment, packaging, or end-to-end helper |
| benchmark/eval scripts | reproduce a named benchmark artifact |

`make -C src lint` runs the fast required checks. Long live-service and benchmark harnesses remain
explicit targets.

## Important gates

- `check_tier_deps.sh`: DB1/DB2 ownership and forbidden storage vocabulary.
- route/API checks: descriptor, handler, OpenAPI, and thin-client parity.
- module checks: public headers, dependencies, descriptors, and documentation.
- event-bus checks: wire conformance, one host, isolation, flow control, capture, blast radius, and
  performance.
- container packaging checks: required files, users, entrypoints, database/inference ownership.

## Generation

```bash
make -C src docs-gen
make -C src docs-gen-check
make -C src gen-sdks
```

Change the source descriptor, not the generated output. Generators must be deterministic for an
unchanged input.

## Sidecars

`llm-chat.py` is a small OpenAI-compatible client used by experiments and command-backed stages.
`llm-rewrite.py` adapts it to the query-rewrite JSON contract. Configure endpoints and credentials
through the owning deployment; do not commit them or print them in diagnostics.

Sidecars read bounded input from stdin, emit the exact documented output on stdout, keep logs on
stderr, time out, and fail with an explicit degraded result. They do not become hidden storage or
policy owners.

## Adding a script

- state cwd, inputs, outputs, exit codes, network use, and destructive behavior in the header;
- use strict shell mode where appropriate;
- quote paths and reject an empty or broad destructive target;
- keep secrets out of argv and output;
- provide `--help` and a self-test for a reusable tool;
- wire a required gate into the owning Make target;
- add it here only when it creates a new category or public operator surface.
