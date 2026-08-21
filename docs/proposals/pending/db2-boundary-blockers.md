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

## Three declarations ask about embeddings and none of them can

    int db2_kb_service_count_embeddings_for_version(const char *version);
    int db2_kb_service_list_unembedded_memory_ids(const char *version, int64_t *ids, int max);
    int db2_memory_promotion_list_unembedded_l2(const char *version, int64_t *ids, int max);

All three take a version. None of them uses it, and two of them do not look at
whether anything is embedded either:

    SELECT COUNT(*) FROM vector_index_ops
    WHERE collection = 'memory_units' AND status = 'ok' AND memory_id IS NOT NULL

    SELECT m.id FROM memories m WHERE m.tier IN ('L1', 'L2')

    SELECT m.id FROM memories m WHERE m.tier = 'L2' LIMIT ?1

The first counts every successful index operation whatever version produced it,
because `vector_index_ops` has no column saying. Its only caller,
`kb_handle_memory_reembed_rollback`, uses the count as a safety gate and
refuses to roll back to a version with no embeddings, so the gate passes for
any version string including one that was never embedded.

The two listers are named for memories that have not been embedded and select
memories by tier alone. Every L1 and L2 memory is "unembedded" to the first,
every L2 memory to the second, so a re-embedding pass driven by either does the
whole corpus every time and reports it as the backlog.

This is one gap, not three: nothing in the schema records which embedding
version produced a row, so no query can ask the question these three names
claim to answer. Publishing any of them writes a `version` field into the
contract that every reader would take to filter, and publishing them without it
would be honest about the SQL and dishonest about the names.

The fix is a version column on `vector_index_ops`, populated where the rows are
written, after which all three become unremarkable queries. That belongs to
whoever owns the re-embedding pipeline.

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

## Sixty-six declarations read and write under a tenant scope the request has no way to carry

This is the largest single item here, and it is a shape rather than a symbol.
Seventy DB2 functions sit behind `db2_tenant_require_pg()` or the tenant scope;
four are already `private-db2` and the other sixty-six are unreviewed.

The tables they touch are protected by row-level security keyed on a session
setting. `kb_team_membership` is the clearest case -- RLS enabled and FORCEd,
with a self-read policy of:

    identity_key = current_setting('aimee.principal', true)

Nothing in a request says who is asking. The setting lives on a connection, and
`db2_tenant_scope_begin(principal, team)` is what puts it there: it leases a
connection, opens a transaction, sets the GUCs, and the caller then does its
work and commits. Callers outside DB2 use it exactly that way:

    src/kb/kb_identity_resolve.c:27
    src/kb/kb_vault_key_use.c:32
    src/kb/kb_vault_rotation_ops.c:125
    src/kb/http/kb_http_telemetry.c:82

So the scope, like the purge guard above, is a transaction that already spans
the boundary -- and every operation inside it depends on state the envelope does
not carry.

Publishing one of these on its own would be worse than leaving it. The reply
would look ordinary and the read would be evaluated against whatever principal
the module's pooled connection happened to hold, which is none: the self policy
would match nothing and the answer would be a confident "no rows". Worse, the
answer depends on the database role. The replay connects as a superuser, which
bypasses RLS entirely, so a tenancy operation replayed there returns everything
and looks correct; a production role would return nothing. A test environment
that is more permissive than production is the one arrangement that cannot
catch this.

Two ways out, and neither belongs to a migration working one symbol at a time.
The envelope could carry an authenticated principal and team, which the module
sets on its connection for the life of the request -- a wire-format change, and
a decision about whether the module may be told who the caller is or must
establish it. Or the scope stays inside DB2 and each unit of tenant work
becomes one composite operation that opens the scope, does the work and
commits, which means enumerating those units rather than the functions they
call.

Until one of those exists the sixty-six stay unreviewed, and the count is worth
watching: it is roughly one in seven of what is left.

## Ten declarations run whatever the host process installed into them

`src/modules/db2/include/aimee/db2/host_contracts.h` lets the surrounding
process hand DB2 function pointers -- twelve registration functions, covering
embedding, fact extraction, retraction scanning, the fact gate, MDL scoring,
audit hashing, identity keys, CSS analysis and the vault's crypto. The
providers live in the process that installed them, and the module process
installs none.

The first count here said six, and it was wrong in a way worth naming: it came
from grepping each declaration's own body for a provider global. Four
declarations never mention one. They call a file-static helper that does, and a
body-only search cannot see through a single call. Following calls to a fixed
point inside each file finds ten, four of them already published:

    operation             declaration                    provider
    artifact_write        db2_artifact_write             mdl_score
    artifact_set_state    db2_artifact_set_state         mdl_score
    artifact_reject       db2_artifact_reject            mdl_score
    kb_audit_append       db2_kb_audit_append            audit_hash
    (pending)             db2_artifact_review_rollback   mdl_score
    (pending)             db2_fact_ingest_text           fact_extract
    (pending)             db2_typed_fact_ingress         fact_scan
    (pending)             db2_kb_pdf_search_chunks       embed
    (pending)             db2_fact_commit                fact_gate
    (pending)             db2_kb_embed_text              embed

What a missing provider does splits three ways, and only one of them is loud.

`db2_kb_audit_append` fails. The row hash comes from the provider, and without
it the append returns an error before it reaches the table. The replay proves
this: `custody.kb_audit_append` is answered, acknowledges nothing, and the
audit table stays empty. An operation that cannot succeed in the module process
is published because the boundary is real and the wire format is settled; what
is unsettled is which process hashes the row.

`db2_kb_pdf_search_chunks` degrades and says so in its own comment: the vector
leg is skipped and the lexical leg answers alone. Fewer results, no error, and
the code is explicit that this is the intended shape.

The rest degrade silently, which is the part to watch. The three artifact
writers succeed and quietly skip the MDL feature row, so in the module process
artifacts accumulate with no MDL features and nothing anywhere records that the
scorer was absent rather than the artifact unscorable. `db2_fact_commit`
without its gate leaves the verdict at DEFER and writes no semantic edge --
every edge withheld, and withheld looks exactly like rejected.
`db2_typed_fact_ingress` treats a missing scanner as "no answer" and declines
to retract, which is the safe direction and an equally quiet one.

This is a smaller problem than the tenant scope and a different kind. The seam
is deliberate and documented; what is undecided is which side of the boundary
each provider belongs on once DB2 is its own process. Embedding in particular
is a question about where model inference runs, not about DB2. Recorded here so
that these ten are not migrated as though they were ordinary reads and writes
-- and so that the four reached only through a helper are not lost again to the
next body-only search.

## One test declares its own copies of six DB2 row types and stubs them untyped

`src/tests/test_kb_http_routes.c` defines `test_term_hit_t`,
`test_project_info_t`, `test_definition_t`, `test_code_search_hit_t`,
`test_caller_hit_t` and `test_blast_radius_t`, each a copy of the real row, and
stubs the DB2 and canonical-index functions that fill them with `void *`:

    int canonical_index_find(const char *identifier, void *out, int max)
    {
       ...
       test_term_hit_t *hits = (test_term_hit_t *)out;

Two declarations of one function with different parameter types is a constraint
violation, and it is invisible while only one of them is in scope -- which is
the same defect that let the hosted bus test hand the DB2 handler a backend
struct shorter than the type the handler reads. Twenty-five stubs in this file
are divergent that way.

Every one of the six copies is byte-for-byte its original today, so nothing is
wrong at runtime. What is wrong is that nothing keeps them in step: the copy of
`term_hit_t` would not have noticed `line_end` being added to the original, and
a field added tomorrow lands the same way.

It is left alone here because untangling it is not a DB2 change. The file also
declares its own `memory_t` with three of the real struct's thirty-five fields
-- a deliberate stand-in rather than a copy -- and its own
`db2_bandit_arm_stats_t`, so including the real headers means deciding what
those two stubs should write, which is a question about this test rather than
about the boundary.

Everything of this shape inside DB2 has been repaired: the module contract
test, the hosted bus test and the graph test now use the declared types, and
`scripts` has no remaining divergence. A scan for the pattern lives in the
session notes rather than the tree, because it is a one-off check rather than a
gate: the compiler enforces it wherever both declarations are in scope, which
is now everywhere DB2's own tests are concerned.

## A default that needs a null argument stops applying at the boundary

Several backends give an absent argument a default the same way:

    db2_artifact_write(id, kind, state ? state : "proposed", ...)
    db2_demotion_profile_write(..., scope_kind ? scope_kind : "global", ...)
    db2_evidence_enqueue(artifact_id, collection ? collection : "evidence")

A caller in the same process can pass a null pointer or an empty string, and
those mean different things: null takes the default, empty is stored as empty.
A caller across the wire has only one of the two. The envelope carries a
length-prefixed string, and a string a caller leaves out arrives as the empty
one -- there is no null on the wire and adding one would mean a presence flag
per string field.

So every default of this shape stops applying the moment its operation crosses.
Nothing breaks: the empty string is stored where the default would have gone,
and a caller that wants the default can send it. But an operation whose reason
does not say so describes behaviour it no longer has.

Twelve published operations carry one, and their reasons now name it. Sixteen
more are in declarations still to be reviewed, listed here so they are caught
rather than rediscovered:

    db2_artifact_write_evidence          scope_kind -> "user", payload -> "{}"
    db2_calibration_profile_write        scope_kind -> "global", version -> "v1"
    db2_cross_repo_review_upsert         evidence_json -> "{}"
    db2_evidence_store_vector            collection -> "evidence", embedding -> "[]"
    db2_kb_documents_insert_chunk_pdf    chunk_strategy -> "heading"
    db2_kb_doc_regions_insert            content_type -> "text"
    db2_kb_doc_asset_insert              content_type -> "image/png"
    db2_learning_proposal_insert         evidence_refs -> "[]"
    db2_memory_contradiction_log         resolution -> "pending"
    db2_memory_coref_audit_insert        outcome -> "none"
    db2_memory_entity_insert             role -> "mention"

Whether any of these defaults should move into the schema, or into the wire as
an explicit value, is a decision per default rather than one for the boundary:
a default in a C signature and a default in a column mean different things to
everything that reads the table directly.

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

## Four list operations still return more rows than one reply can hold

This entry used to name sixty-three. The bound was not the boundary's: a
generated client declared its response buffer as a local array, so the reply
ceiling was set at what a caller's stack should hold, 65536 bytes. The
transport was never the limit -- the module protocol carries sixteen megabytes
and the bus wire one.

A reply wider than sixteen kilobytes is now allocated by the generated client
rather than declared, and the same in the module's handler, whose row buffer
and backend row array sit on a handler thread's stack. The ceiling is what the
wire carries, one megabyte less the envelope. Every operation that already
crossed is unchanged: it stays on the stack, with no allocation it did not
have.

Two of those handler arrays had been written `static` to keep them off the
stack, which two concurrent calls would have shared -- handlers run on their
own threads. They are allocated now, with the rest.

That leaves four declarations whose callers ask for more rows than a megabyte
holds:

    row type                bytes/row   rows per reply   caller asks for   n
    project_info_t               4265              245               512   2
    css_migration_unit_t         4405              238              1024   1
    db2_org_spend_row_t           338             3102              4096   1

Each is wide for its own reason -- a project row carries a 4KB path, a CSS
migration unit carries two -- and each caller's number is the size of an array
it declared, which is a ceiling it chose rather than a count it needs. The
honest options are unchanged and now apply to four operations rather than
sixty-three: page the reply, narrow the row to the fields the caller reads, or
establish that the caller's array is larger than any real result.

Twenty more could not be measured because their row type is not a plain struct
of scalars and fixed strings; they need reading one at a time.

## Two declarations list projects, and one of them should go

    int db2_canonical_index_list_projects(project_info_t *out, int max);
    int db2_code_index_project_list(project_info_t *out, int max);

They return the same rows in the same shape. Both are in the short list above
-- a project row is 4265 bytes at its widest, fifteen to a reply, against the
512 a caller asks for -- so whichever way that is settled applies to both. What
is particular to this pair is that one of them is redundant, and folding them
means choosing which name survives without being able to test that the survivor
returns what both callers expect. Doing that at the same time as changing how
the list crosses would make a behaviour change look like a plumbing change.
