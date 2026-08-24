# db3 module contract

Moved. This protocol is the **vector module**, and its contract lives in
[docs/modules/vectordb.md](modules/vectordb.md).

The name changed because `db3` reads as a third database tier, which is the wrong
mental model: it is an optional external vector store beside a `postgres` module
that owns PostgreSQL, and a deployment may have none.

This file is a pointer rather than a deletion because the old name appears in
database tables and DB2 operation names, which deliberately keep it -- renaming
those is a data migration and a wire change rather than a rename. Somebody
following `db3` from a table name should land somewhere that says so.
