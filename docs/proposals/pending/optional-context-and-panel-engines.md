# Optional context and panel engines

Status: pending; excluded from the IR migration.

The former context-engine registry and delegates panel-provider facade had no
registered alternative implementation or shipping facade caller. They and their
exclusive tests were removed. Direct compaction and active ensemble execution
remain. The old code is preserved on `proposal/deferred-execution-and-accounting`
at `3fcaea26f8a0ef8f8878ded2b811e79d2ab9c657`.

Reintroduce an extension only with an actual second implementation, an identified
runtime registration and consumer, explicit result ownership, and integration
coverage of that path. Model requests and responses must retain canonical IR.
