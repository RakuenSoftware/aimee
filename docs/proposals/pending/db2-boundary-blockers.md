# Proposal: the DB2 declarations that cannot become wire operations as they stand

- **State:** OPEN — each entry needs a decision from whoever owns the behaviour,
  not from the migration that reached it.

The DB2 module-boundary migration reviews every `db2_` declaration and gives it
a disposition: `wire-operation` if it crosses the boundary as a typed operation,
`private-db2` if it stays inside the module. Most declarations take one or the
other. The ones below take neither, because moving them changes what they do and
leaving them where they are contradicts the callers they already have.

They are recorded here rather than given a disposition, so the review file keeps
meaning what it says: a symbol marked `private-db2` is one nothing outside DB2
calls, and none of these qualify.

## `db2_kb_purge_txn_guard` holds a lock that the bus would release

    int db2_kb_purge_txn_guard(const char *project);

It takes a project-keyed advisory lock:

    SELECT pg_advisory_xact_lock(hashtext('aimee_purge:' || ?1))

`pg_advisory_xact_lock` is released when the transaction ends. The guard exists
to serialize the purge route's fence-publish transaction against every writer's
check-then-commit, and both sides rely on holding it until they commit.

A wire operation is its own transaction. The lock would be taken and released
before the reply was written, and every caller would then commit outside it. The
operation would return success and serialize nothing — the failure mode the
guard was added to prevent, with the guard apparently in place.

It cannot be `private-db2` either. Two callers outside the module use it today:

    src/kb/kb.c:638                                     kb_purge_fenced_txn_begin
    src/modules/kb-synthesis/kb_curator_index_code_unit.c:160

Both call it between `db2_kb_txn_begin()` and a later commit, so the transaction
itself already spans the boundary. The unit of work that has to move is not the
guard but the whole sequence — begin, guard, check the fence, do the write,
commit — as one operation that holds its transaction inside DB2. That is a
composite operation with the caller's write in the middle of it, which is a
larger design question than this migration has been answering one symbol at a
time.

Until that exists, the guard stays where it is and stays unreviewed.

## `db2_kb_service_count_embeddings_for_version` ignores the version it is given

    int db2_kb_service_count_embeddings_for_version(const char *version);

The statement it runs has no version in it:

    SELECT COUNT(*) FROM vector_index_ops
    WHERE collection = 'memory_units' AND status = 'ok' AND memory_id IS NOT NULL

`vector_index_ops` has no column recording which embedding version produced a
row, so the argument has nowhere to go. Its only caller,
`kb_handle_memory_reembed_rollback`, uses the count as a safety gate: it refuses
to roll back to a version that has no embeddings. Because the count ignores the
version, that gate passes for any version string, including one that was never
embedded.

Publishing it as a wire operation would write the defect into the contract: a
`version` field that every reader would reasonably assume filters the count.
Publishing it without the field would be honest about the SQL and dishonest
about the name, and would silently turn the caller's gate into a check that the
table is non-empty.

The fix is a schema change — a version column on `vector_index_ops`, populated
where the rows are written — after which the operation is unremarkable. That
belongs to whoever owns the re-embedding pipeline.

## The two JSONL exports write to a path the caller names

    int db2_memory_decisions_export_jsonl(const char *path);
    int db2_rules_export_jsonl(const char *path);

Both open the path they are given and truncate it:

    FILE *f = fopen(path, "w");

The path is not confined to a directory, checked for traversal, or checked
against anything at all. It arrives from a request field:

    src/kb/kb_service_agent.c:47      kb_handle_rules_export_jsonl
    src/kb/kb_service_memory.c:504    the decisions equivalent

    cJSON *path_j = cJSON_GetObjectItemCaseSensitive(req, "path");
    ...
    db2_kb_service_rules_export_jsonl_json(path_j->valuestring);

So a client of the KB service can already name any file the service process can
write and have it truncated and replaced with JSONL. That is true today and is
not caused by this migration; it is recorded here because the migration is what
read the path end to end.

Publishing these as wire operations would move the write into the DB2 module
process and hand the same unconfined path to a second set of clients. The
migration should not do that quietly, and cannot fix it on its own: the two
honest shapes are an operation that returns the JSONL and lets the caller write
it -- which needs the reply format for large payloads that does not exist yet --
or a confined export that takes a name rather than a path and decides the
directory itself, which changes the service API. Both are decisions for whoever
owns the KB service.

## Operation ids are unique per family, and the envelope does not say the family

Every operation carries an id that is unique within its family, so
`memory` id 14 and `organization` id 14 are different operations with the same
number. The stage id in the envelope is what distinguishes them, and it does so
only because each family has its own adapter branch.

This works and is tested, but it means the id alone never identifies an
operation. Anything reading the wire — a trace, a log line, a future router —
has to carry the family with it or it is reading an ambiguous number. Whether
the envelope should carry an explicit family discriminator is a wire-format
decision, and changing it later is a breaking change; recorded now while the
wire is still only spoken by this repository.

## Two declarations list projects, and only one can have a reply

    int db2_canonical_index_list_projects(project_info_t *out, int max);
    int db2_code_index_project_list(project_info_t *out, int max);

They return the same rows in the same shape. One of them should go, but a
project row carries a path of up to 4KB and the count is unbounded, so neither
can be published until the reply format for a list of large rows exists. Folding
them before that would mean choosing which of the two names survives without
being able to test that the survivor returns what both callers expect.
