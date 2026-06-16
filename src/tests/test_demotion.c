/* test_demotion.c — unit tests for the demotion DB2 module.
 *
 * Tests:
 *   1. retrieval_event_write: writes a retrieval_event artifact.
 *   2. retrieval_attribution_write: writes attribution and links to event.
 *   3. demotion_score_empty: returns NAN when no attribution data.
 *   4. demotion_score_basic: correct score with mixed verdicts.
 *   5. demotion_profile_write_read: round-trip test.
 *   6. demotion_profile_scope_fallback: narrowest-scope fallback.
 *   7. demotion_candidates: enumerates rows with data.
 *   8. config: demotion defaults are sane.
 */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "demotion.h"
#include "db2_test_shim.h"
#include "db2_internal.h"
#include "db_postgres.h"
#include "config.h"
#include "config_learning.h"

static void open_db(void)
{
   db2_test_shim_close();
   db2_test_shim_open();
}

static void close_db(void)
{
   db2_test_shim_close();
}

/* ---- 1. retrieval_event_write ---- */
static void test_retrieval_event_write(void)
{
   open_db();

   int64_t ids[3] = {101, 202, 303};
   char ev_id[64];
   int rc = db2_demotion_retrieval_event_write("fp123", "Recall", ids, 3, ev_id, sizeof(ev_id));
   assert(rc == 0);
   assert(strlen(ev_id) == 36);

   close_db();
   printf("  retrieval_event_write: ok\n");
}

/* ---- 1b. turn-keyed event write + lookup (auditable-correctness P1) ---- */
static void test_retrieval_event_turn(void)
{
   open_db();

   int64_t ids[2] = {11, 22};
   char ev_id[64];
   assert(db2_demotion_retrieval_event_write_turn("turn-abc", "fp", "Recall", ids, 2, ev_id,
                                                  sizeof(ev_id)) == 0);

   /* look it up by the caller-visible turn_id. */
   char got_id[64], payload[8192];
   assert(db2_demotion_retrieval_event_by_turn("turn-abc", got_id, sizeof(got_id), payload,
                                               sizeof(payload)) == 1);
   assert(strcmp(got_id, ev_id) == 0);
   assert(strstr(payload, "\"surfaced_ids\":[11,22]") != NULL);

   /* an unknown turn -> no event (0), not an error. */
   assert(db2_demotion_retrieval_event_by_turn("turn-missing", got_id, sizeof(got_id), NULL, 0) ==
          0);
   assert(got_id[0] == '\0');

   /* DUPLICATE turn_id is first-wins: a second write for the same turn returns the
    * AUTHORITATIVE (first) event id, and by_turn still resolves to that one event. */
   char dup_id[64];
   assert(db2_demotion_retrieval_event_write_turn("turn-abc", "fp2", "Recall", NULL, 0, dup_id,
                                                  sizeof(dup_id)) == 0);
   assert(strcmp(dup_id, ev_id) == 0); /* returned the first event's id, not the orphan's */
   char reget[64];
   assert(db2_demotion_retrieval_event_by_turn("turn-abc", reget, sizeof(reget), NULL, 0) == 1);
   assert(strcmp(reget, ev_id) == 0); /* still resolves to the original */

   /* a NULL/"" turn_id behaves like the base writer (no stamp, still written) and
    * such events are never found by a turn lookup. */
   char ev2[64];
   assert(db2_demotion_retrieval_event_write_turn(NULL, "fp", "Recall", NULL, 0, ev2,
                                                  sizeof(ev2)) == 0);
   assert(strlen(ev2) == 36);

   /* legacy (turn-less) events do not collide on the partial unique index. */
   assert(db2_demotion_retrieval_event_write("fp", "Recall", NULL, 0, NULL, 0) == 0);
   assert(db2_demotion_retrieval_event_write("fp", "Recall", NULL, 0, NULL, 0) == 0);

   /* bad args. */
   assert(db2_demotion_retrieval_event_by_turn(NULL, got_id, sizeof(got_id), NULL, 0) == -1);

   close_db();
   printf("  retrieval_event_turn: ok\n");
}

/* ---- 1b. retrieval_event_merge_turn (P1.5 idempotent two-writer merge) ---- */
static void test_retrieval_event_merge_turn(void)
{
   open_db();

   /* First writer on a fresh turn → behaves like write_turn (creates the event). */
   int64_t a[2] = {11, 22};
   char ev_id[64];
   assert(db2_demotion_retrieval_event_merge_turn("turn-m", "fp", "Recall", a, 2, ev_id,
                                                  sizeof(ev_id)) == 0);
   char payload[8192];
   assert(db2_demotion_retrieval_event_by_turn("turn-m", NULL, 0, payload, sizeof(payload)) == 1);
   assert(strstr(payload, "\"surfaced_ids\":[11,22]") != NULL);
   /* unified model (D3): the canonical surfaced_refs carries typed entries, and the
    * legacy surfaced_ids is a derived projection of the memory-typed ones. */
   assert(strstr(payload, "\"surfaced_refs\":") != NULL);
   assert(strstr(payload, "\"type\":\"memory\"") != NULL);

   /* Second writer on the SAME turn → merges new refs into the same event (22 is a
    * dup and is skipped; 33 is added). Returns the same canonical event id. */
   int64_t b[2] = {22, 33};
   char got[64];
   assert(db2_demotion_retrieval_event_merge_turn("turn-m", "fp2", "Recall", b, 2, got,
                                                  sizeof(got)) == 0);
   assert(strcmp(got, ev_id) == 0); /* same event, not a new one */
   assert(db2_demotion_retrieval_event_by_turn("turn-m", NULL, 0, payload, sizeof(payload)) == 1);
   assert(strstr(payload, "\"surfaced_ids\":[11,22,33]") != NULL);
   /* the merged ref also gets a surfaced_items entry (same shape as the writer) */
   assert(strstr(payload, "\"surfaced_items\":") != NULL);
   assert(strstr(payload, "{\"id\":33}") != NULL); /* id-only (no v: id 33 unresolved) */

   /* Idempotent: re-merging refs already present changes nothing. */
   int64_t c[2] = {11, 33};
   assert(db2_demotion_retrieval_event_merge_turn("turn-m", "fp3", "Recall", c, 2, NULL, 0) == 0);
   assert(db2_demotion_retrieval_event_by_turn("turn-m", NULL, 0, payload, sizeof(payload)) == 1);
   assert(strstr(payload, "\"surfaced_ids\":[11,22,33]") != NULL); /* unchanged */

   /* bad arg. */
   assert(db2_demotion_retrieval_event_merge_turn("", "fp", "Recall", a, 2, NULL, 0) == -1);

   /* LEGACY MIGRATION: an event written before the unified model (surfaced_ids only,
    * no surfaced_refs) is migrated on the next merge — surfaced_refs is back-filled
    * and the new ref added, with the projection kept in sync. */
   {
      void *conn = db2_conn();
      char e[256] = "";
      assert(aimee_pg_exec(conn,
                           "INSERT INTO artifacts (id, kind, turn_id, payload)"
                           " VALUES ('leg1','retrieval_event','turn-leg',"
                           " '{\"surfaced_ids\":[7],\"surfaced_items\":[{\"id\":7,\"v\":\"x\"}]}')",
                           e, sizeof e) == 0);
      int64_t d[1] = {8};
      assert(db2_demotion_retrieval_event_merge_turn("turn-leg", "fp", "Recall", d, 1, NULL, 0) ==
             0);
      assert(db2_demotion_retrieval_event_by_turn("turn-leg", NULL, 0, payload, sizeof(payload)) ==
             1);
      assert(strstr(payload, "\"surfaced_refs\":") != NULL);     /* back-filled */
      assert(strstr(payload, "\"surfaced_ids\":[7,8]") != NULL); /* migrated 7 + merged 8 */
      assert(strstr(payload, "\"v\":\"x\"") != NULL);            /* legacy v preserved */
      printf("  retrieval_event_merge_turn: legacy migration ok\n");
   }

   close_db();
   printf("  retrieval_event_merge_turn: ok\n");
}

/* ---- 2. retrieval_attribution_write ---- */
static void test_retrieval_attribution_write(void)
{
   open_db();

   char ev_id[64];
   assert(db2_demotion_retrieval_event_write("fp", "Recall", NULL, 0, ev_id, sizeof(ev_id)) == 0);

   int rc = db2_demotion_retrieval_attribution_write(ev_id, 42, DEMOTION_VERDICT_ACCEPTED, 0.8);
   assert(rc == 0);

   int rc2 = db2_demotion_retrieval_attribution_write(ev_id, 42, DEMOTION_VERDICT_CORRECTED, 0.5);
   assert(rc2 == 0);

   close_db();
   printf("  retrieval_attribution_write: ok\n");
}

/* ---- 3. demotion_score_empty (no data → NAN) ---- */
static void test_demotion_score_empty(void)
{
   open_db();

   double score = db2_demotion_score(9999, 64, 30.0, 5);
   assert(isnan(score));

   close_db();
   printf("  demotion_score_empty: ok\n");
}

/* ---- 4. demotion_score_basic ---- */
static void test_demotion_score_basic(void)
{
   open_db();

   char ev_id[64];
   assert(db2_demotion_retrieval_event_write("fp", "Recall", NULL, 0, ev_id, sizeof(ev_id)) == 0);

   /* Write 6 rows: 4 accepted, 2 corrected.  n_min = 5. */
   for (int i = 0; i < 4; i++)
      assert(db2_demotion_retrieval_attribution_write(ev_id, 77, DEMOTION_VERDICT_ACCEPTED, 1.0) ==
             0);
   for (int i = 0; i < 2; i++)
      assert(db2_demotion_retrieval_attribution_write(ev_id, 77, DEMOTION_VERDICT_CORRECTED, 1.0) ==
             0);

   /* With n_min=5 and 6 rows, we should get a valid score. */
   double score = db2_demotion_score(77, 64, 30.0, 5);
   assert(!isnan(score));
   /* net = 4*1 - 2*1 = +2 (decayed, so slightly less but positive) */
   assert(score > 0.0);

   /* With n_min=10 we don't have enough rows. */
   double score2 = db2_demotion_score(77, 64, 30.0, 10);
   assert(isnan(score2));

   close_db();
   printf("  demotion_score_basic: ok\n");
}

/* ---- 5. demotion_profile_write_read ---- */
static void test_profile_write_read(void)
{
   open_db();

   const char *payload = "{\"memory_class\":\"preference\",\"n_rows_scored\":3}";
   char id_out[64];
   int rc = db2_demotion_profile_write("preference", "global", "", payload, id_out, sizeof(id_out));
   assert(rc == 0);
   assert(strlen(id_out) == 36);

   char buf[512];
   rc = db2_demotion_profile_read("preference", "global", "", buf, sizeof(buf));
   assert(rc == 0);
   assert(strstr(buf, "preference") != NULL);

   close_db();
   printf("  demotion_profile_write_read: ok\n");
}

/* ---- 6. demotion_profile_scope_fallback ---- */
static void test_profile_scope_fallback(void)
{
   open_db();

   assert(db2_demotion_profile_write("fact", "global", "", "{\"scope\":\"global\"}", NULL, 0) == 0);

   char buf[256];
   /* Exact scope missing → falls back to global */
   int rc = db2_demotion_profile_read("fact", "user", "someone", buf, sizeof(buf));
   assert(rc == 0);
   assert(strstr(buf, "global") != NULL);

   /* Write user-scoped profile — exact match wins */
   assert(db2_demotion_profile_write("fact", "user", "someone", "{\"scope\":\"exact\"}", NULL, 0) ==
          0);
   rc = db2_demotion_profile_read("fact", "user", "someone", buf, sizeof(buf));
   assert(rc == 0);
   assert(strstr(buf, "exact") != NULL);

   close_db();
   printf("  demotion_profile_scope_fallback: ok\n");
}

/* ---- 7. demotion_candidates ---- */
static void test_demotion_candidates(void)
{
   open_db();

   char ev_id[64];
   assert(db2_demotion_retrieval_event_write("fp", "Recall", NULL, 0, ev_id, sizeof(ev_id)) == 0);

   /* Row 11: 3 attributions; Row 22: 1 attribution. */
   for (int i = 0; i < 3; i++)
      assert(db2_demotion_retrieval_attribution_write(ev_id, 11, DEMOTION_VERDICT_ACCEPTED, 1.0) ==
             0);
   assert(db2_demotion_retrieval_attribution_write(ev_id, 22, DEMOTION_VERDICT_IRRELEVANT, 1.0) ==
          0);

   db2_demotion_candidate_t cands[16];

   /* min=2: only row 11 qualifies */
   int n = db2_demotion_candidates(2, cands, 16);
   assert(n == 1);
   assert(cands[0].row_id == 11);
   assert(cands[0].attribution_n == 3);

   /* min=1: both rows qualify */
   n = db2_demotion_candidates(1, cands, 16);
   assert(n == 2);

   close_db();
   printf("  demotion_candidates: ok\n");
}

/* ---- 8. config defaults ---- */
static void test_config_defaults(void)
{
   config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   config_apply_demotion_settings(&cfg, NULL);

   /* Default flipped to 1 (shadow): scores/profiles are computed but no row is
    * demoted until 2 (live). See config_apply_demotion_settings rationale. */
   assert(cfg.demotion_enabled == 1);
   assert(cfg.demotion_window == 64);
   assert(fabs(cfg.demotion_half_life_days - 30.0) < 1e-9);
   assert(cfg.demotion_n_min == 5);

   printf("  config_defaults: ok\n");
}

/* ---- main ---- */
int main(void)
{
   printf("demotion:\n");

   test_retrieval_event_write();
   test_retrieval_event_turn();
   test_retrieval_event_merge_turn();
   test_retrieval_attribution_write();
   test_demotion_score_empty();
   test_demotion_score_basic();
   test_profile_write_read();
   test_profile_scope_fallback();
   test_demotion_candidates();
   test_config_defaults();

   printf("All demotion tests passed.\n");
   return 0;
}
