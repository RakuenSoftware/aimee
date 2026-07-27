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

- [audit](audit.md), [event bus](bus.md), [config](config.md), and
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
