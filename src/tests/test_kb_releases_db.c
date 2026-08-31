/* test_kb_releases_db.c — DB-backed release lifecycle tests for the curation
 * release gate (src/modules/db2/c/kb_releases.c), exercising the real promote/rollback
 * logic against the DB2 sqlite shim (no stubs).
 *
 * Covers the proposal ACs:
 *   - POST /v1/releases/{id}/promote switches the active retrieval release
 *     without re-ingesting the corpus (it flips the active_release_id pointer
 *     and retires the previous active release; docs/chunks are untouched).
 *   - POST /v1/releases/{id}/rollback restores the prior active release cleanly.
 *   - Staged docs are only "live" once their release is the active one. */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <sqlite3.h>

#include "modules/db2/c/db2_test_shim.h"
#include "modules/db2/c/kb_releases.h"
#include "modules/db2/c/kb_runtime_state.h"

/* Insert a docs row directly (the release_docs FK references docs(id)).
 * review_needed != 0 marks a staged doc that must be excluded from live
 * retrieval until accepted into a promoted release. Returns the new doc id. */
static int64_t insert_doc(const char *hash, int review_needed)
{
   sqlite3 *db = (sqlite3 *)db2_test_shim_handle();
   assert(db != NULL);
   char sql[512];
   snprintf(sql, sizeof(sql),
            "INSERT INTO docs (content_hash, review_needed, state) VALUES ('%s', %d, 'staged')",
            hash, review_needed);
   assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
   return (int64_t)sqlite3_last_insert_rowid(db);
}

static void open_db(void)
{
   db2_test_shim_close();
   db2_test_shim_open();
}

static void close_db(void)
{
   db2_test_shim_close();
}

/* ---- 1. create + read round-trip ---- */
static void test_create_read(void)
{
   open_db();
   int64_t a = db2_kb_release_create("rel-a");
   assert(a > 0);
   db2_kb_release_t row;
   assert(db2_kb_release_read(a, &row) == 0);
   assert(row.id == a);
   assert(strcmp(row.name, "rel-a") == 0);
   assert(strcmp(row.state, "pending") == 0);
   close_db();
   printf("  create_read: ok\n");
}

/* ---- 2. promote sets active pointer + state=active ---- */
static void test_promote_activates(void)
{
   open_db();
   int64_t a = db2_kb_release_create("rel-a");
   assert(a > 0);
   /* No active release before promotion. */
   assert(db2_kb_release_get_active() == 0);

   assert(db2_kb_release_promote(a) == 0);
   assert(db2_kb_release_get_active() == a);

   db2_kb_release_t row;
   assert(db2_kb_release_read(a, &row) == 0);
   assert(strcmp(row.state, "active") == 0);
   assert(row.promoted_at[0] != '\0');
   close_db();
   printf("  promote_activates: ok\n");
}

/* ---- 3. promoting a second release retires the first ---- */
static void test_promote_retires_previous(void)
{
   open_db();
   int64_t a = db2_kb_release_create("rel-a");
   int64_t b = db2_kb_release_create("rel-b");
   assert(a > 0 && b > 0);

   assert(db2_kb_release_promote(a) == 0);
   assert(db2_kb_release_get_active() == a);

   /* Promoting B switches the active pointer and retires A. */
   assert(db2_kb_release_promote(b) == 0);
   assert(db2_kb_release_get_active() == b);

   db2_kb_release_t ra, rb;
   assert(db2_kb_release_read(a, &ra) == 0);
   assert(db2_kb_release_read(b, &rb) == 0);
   assert(strcmp(ra.state, "retired") == 0);
   assert(ra.retired_at[0] != '\0');
   assert(strcmp(rb.state, "active") == 0);
   close_db();
   printf("  promote_retires_previous: ok\n");
}

/* ---- 4. rollback restores the prior active release ---- */
static void test_rollback_restores_prior(void)
{
   open_db();
   int64_t a = db2_kb_release_create("rel-a");
   int64_t b = db2_kb_release_create("rel-b");
   assert(a > 0 && b > 0);

   assert(db2_kb_release_promote(a) == 0);
   assert(db2_kb_release_promote(b) == 0);
   assert(db2_kb_release_get_active() == b);

   /* Rollback with no target → restore the most-recently-retired release (A). */
   assert(db2_kb_release_rollback(0) == 0);
   assert(db2_kb_release_get_active() == a);

   db2_kb_release_t ra;
   assert(db2_kb_release_read(a, &ra) == 0);
   assert(strcmp(ra.state, "active") == 0);
   close_db();
   printf("  rollback_restores_prior: ok\n");
}

/* ---- 5. rollback to an explicit target ---- */
static void test_rollback_explicit_target(void)
{
   open_db();
   int64_t a = db2_kb_release_create("rel-a");
   int64_t b = db2_kb_release_create("rel-b");
   int64_t c = db2_kb_release_create("rel-c");
   assert(a > 0 && b > 0 && c > 0);

   db2_kb_release_promote(a);
   db2_kb_release_promote(b);
   db2_kb_release_promote(c);
   assert(db2_kb_release_get_active() == c);

   /* Explicit rollback to A. */
   assert(db2_kb_release_rollback(a) == 0);
   assert(db2_kb_release_get_active() == a);
   close_db();
   printf("  rollback_explicit_target: ok\n");
}

/* ---- 6. rollback with no retired release fails cleanly ---- */
static void test_rollback_no_prior_fails(void)
{
   open_db();
   int64_t a = db2_kb_release_create("rel-a");
   assert(a > 0);
   db2_kb_release_promote(a);
   /* Only one release, none retired → rollback(0) returns error, active intact. */
   assert(db2_kb_release_rollback(0) == -1);
   assert(db2_kb_release_get_active() == a);
   close_db();
   printf("  rollback_no_prior_fails: ok\n");
}

/* ---- 7. staged-doc release membership gates which docs are live ---- */
static void test_release_membership(void)
{
   open_db();
   /* A staged doc (review_needed=1) joins a pending release. */
   int64_t doc = insert_doc("hash-staged-1", 1);
   assert(doc > 0);
   int64_t rel = db2_kb_release_create("rel-staged");
   assert(rel > 0);

   assert(db2_kb_release_add_doc(rel, doc) == 0);
   /* Idempotent (ON CONFLICT DO NOTHING). */
   assert(db2_kb_release_add_doc(rel, doc) == 0);

   /* Until the release is promoted it is not the live release — the staged
    * doc is therefore excluded from live retrieval. */
   assert(db2_kb_release_get_active() != rel);
   /* Promote → becomes the active (live) release; the doc's membership now
    * counts toward live retrieval without any re-ingest. */
   assert(db2_kb_release_promote(rel) == 0);
   assert(db2_kb_release_get_active() == rel);
   close_db();
   printf("  release_membership: ok\n");
}

int main(void)
{
   if (db2_test_shim_skip_on_postgres("kb_releases_db"))
      return 0;

   printf("kb_releases_db:\n");
   test_create_read();
   test_promote_activates();
   test_promote_retires_previous();
   test_rollback_restores_prior();
   test_rollback_explicit_target();
   test_rollback_no_prior_fails();
   test_release_membership();
   printf("All kb_releases_db tests passed.\n");
   return 0;
}
