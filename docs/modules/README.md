# Modules

Module documents describe ownership and contracts, not every implementation file. The descriptor and
public header remain authoritative.

A module document should cover:

- responsibility and non-responsibility;
- public API and event kinds;
- dependencies and consumers;
- configuration and capabilities;
- data, trust, and failure boundaries;
- required tests and generated/attested status.

Current contracts:

- [audit](audit.md), [event bus](../../src/core/README.md), [config](config.md), and
  [module runtime](module-runtime.md);
- [memory](memory.md), [learning](learning.md), [KB synthesis](kb-synthesis.md), and
  [benchmarks](benchmarks.md);
- [delegates](delegates.md), [roundtables](roundtable.md), [routing](routing.md),
  [execution policy](execution-policy.md), and [governance](governance.md);
- [tools](tools.md), [skills](skills.md), [git](git.md), [workspace](workspace.md), and
  [workflows](workflows.md);
- [IR](ir.md), [translation](translation.md), [response composition](response-composition.md),
  and [protocols](protocols.md);
- [gateway](gateway.md), [runtime web](runtime-web.md), [control web](control-web.md), and
  [vault](vault.md).

See the [technical reference](../../src/README.md) for the process and source map.

## Turning optional modules on and off

The canonical inventory splits modules into **required** and **optional**
(`tests/baselines/modules/canonical-inventory.yaml`). Required modules always
run: a deployment without `memory` or `routing` is not a smaller deployment, it
is a broken one, so only the optional set is operator-controlled.

Set `AIMEE_MODULE_<ID>`, where `<ID>` is the module id uppercased with `-`
replaced by `_`:

| Value | Effect |
|---|---|
| `1`, `true`, `on`, `yes` | the module runs, even if the image shipped it off |
| `0`, `false`, `off`, `no` | the module does not run, even if the image shipped it on |
| unset or empty | keep whatever the image shipped |

```sh
AIMEE_MODULE_ROUNDTABLE=1     # turn the review panel on
AIMEE_MODULE_SANDBOX=0        # turn the shipped sandbox module off
AIMEE_MODULE_KB_SYNTHESIS=1   # kb-placed module
```

Optional modules by placement:

- **server**: `governance`, `roundtable`, `benchmarks`, `sandbox`, `runtime-web`
- **kb**: `kb-synthesis`, `control-web`, `benchmarks`

The setting is read at container start by `deploy/container/optional-modules-lib.sh`,
which rewrites a copy of the shipped module manifest before
`module-supervisor.sh` reads it. The manifest baked into the image is never
edited in place. A change takes effect on the next start.

Two specifics worth knowing:

- **`workflows` is not gated here.** It is optional and server-placed, but it is
  hosted by `/usr/local/bin/aimee-wfe` rather than the module-runtime multicall
  binary, and is governed by `AIMEE_WFE_ENGINE`. `AIMEE_MODULE_WORKFLOWS` would
  silently do nothing, so it is deliberately not accepted.
- **`runtime-web` follows the browser UI.** If `AIMEE_RUNTIME_WEB_ENABLED=0` and
  you say nothing about the module, the module is turned off too, because it would
  have nothing to serve. An explicit `AIMEE_MODULE_RUNTIME_WEB` overrides that.

Asking for a module that is not present in the image logs a warning and changes
nothing, rather than failing the start.
