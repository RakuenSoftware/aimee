# Historical memory wire contract

The in-process Cognee/Hillock serving implementations described in older validation
reports are retired. The shared Go wire/types contract remains for current native
callers; independent implementations attach through Aimee's generic module adapter.
The reusable database implementation and concrete private memory module live in
[aims-db](https://github.com/RakuenSoftware/aims-db).

Existing legacy catalogs can be exported/imported offline with the private
`integrations/legacy-memory-transfer` utility in aims-db. It preserves canonical
records, revisions, original authorship, history and erasure authority. Migration
must not discard retained erasure controls or substitute an unrelated namespace.
The public `scripts/validation/memory/native-transfer.py` accepts this separately
built executable through `--validator`. Catalog interchange alone is not a native
aims-db import or a complete graph/vector/learning migration.
