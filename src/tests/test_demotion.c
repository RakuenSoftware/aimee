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
