#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aimee.h"
#include "db1.h"
#include "artifacts.h"
#include "calibration.h"
#include "modules/db2/c/db2_test_shim.h"
#include "working_profile.h"
#include "platform_test_util.h"

typedef struct
{
   char tmpdir[PATH_MAX];
   char db_path[PATH_MAX];
} working_profile_test_db_t;

static working_profile_test_db_t setup(void)
{
   working_profile_test_db_t ctx;

   memset(&ctx, 0, sizeof(ctx));
   snprintf(ctx.tmpdir, sizeof(ctx.tmpdir), "%s/aimee-wp-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(ctx.tmpdir) != NULL);
   snprintf(ctx.db_path, sizeof(ctx.db_path), "%s/aimee.db", ctx.tmpdir);
   assert(db1_init(ctx.db_path) == 0);
   return ctx;
}

static void teardown(working_profile_test_db_t *ctx)
{
   if (!ctx)
      return;
   db1_shutdown();
   if (ctx->db_path[0])
      platform_test_remove_sqlite(ctx->db_path);
   if (ctx->tmpdir[0])
      platform_test_rmrf(ctx->tmpdir);
   memset(ctx, 0, sizeof(*ctx));
}

static void open_db2(void)
{
   db2_test_shim_close();
   db2_test_shim_open();
}

static void close_db2(void)
{
   db2_test_shim_close();
}

int main(void)
{
   printf("working_profile: ");

   /* --- field canonicality --- */
   {
      assert(working_profile_field_is_canonical(WORKING_PROFILE_FIELD_COMMUNICATION_STYLE));
      assert(working_profile_field_is_canonical(WORKING_PROFILE_FIELD_VERBOSITY));
      assert(working_profile_field_is_canonical(WORKING_PROFILE_FIELD_PROJECT_ROLE));
      assert(!working_profile_field_is_canonical("something_made_up"));
      assert(!working_profile_field_is_canonical(""));
      assert(!working_profile_field_is_canonical(NULL));
   }

   /* --- threshold commit: N observations flips working_profile_state --- */
   {
      working_profile_test_db_t ctx = setup();

      /* Two observations at threshold=3 shouldn't commit. */
      assert(db1_working_profile_local_observe(WORKING_PROFILE_FIELD_VERBOSITY, "terse", 0.8,
                                               session_id(), 3) == 0);
      assert(db1_working_profile_local_observe(WORKING_PROFILE_FIELD_VERBOSITY, "terse", 0.9,
                                               session_id(), 3) == 0);

      db1_working_profile_local_state_t got;
      assert(db1_working_profile_local_get(WORKING_PROFILE_FIELD_VERBOSITY, &got) == 1);

      /* Third observation trips the threshold. */
      assert(db1_working_profile_local_observe(WORKING_PROFILE_FIELD_VERBOSITY, "terse", 0.85,
                                               session_id(), 3) == 1);
      assert(db1_working_profile_local_get(WORKING_PROFILE_FIELD_VERBOSITY, &got) == 0);
      assert(strcmp(got.value, "terse") == 0);
      assert(got.observation_count == 3);
      /* Average of 0.8, 0.9, 0.85 = 0.85 */
      assert(got.score > 0.84 && got.score < 0.86);

      teardown(&ctx);
   }

   /* --- different values accumulate independently and don't clobber --- */
   {
      working_profile_test_db_t ctx = setup();
      /* Three terse observations commit terse. */
      for (int i = 0; i < 3; i++)
         assert(db1_working_profile_local_observe(WORKING_PROFILE_FIELD_VERBOSITY, "terse", 0.9,
                                                  session_id(), 3) >= 0);

      db1_working_profile_local_state_t got;
      assert(db1_working_profile_local_get(WORKING_PROFILE_FIELD_VERBOSITY, &got) == 0);
      assert(strcmp(got.value, "terse") == 0);

      /* Two verbose observations don't unseat the terse commit. */
      assert(db1_working_profile_local_observe(WORKING_PROFILE_FIELD_VERBOSITY, "verbose", 0.6,
                                               session_id(), 3) == 0);
      assert(db1_working_profile_local_observe(WORKING_PROFILE_FIELD_VERBOSITY, "verbose", 0.6,
                                               session_id(), 3) == 0);
      assert(db1_working_profile_local_get(WORKING_PROFILE_FIELD_VERBOSITY, &got) == 0);
      assert(strcmp(got.value, "terse") == 0);

      /* A third verbose observation: threshold clears, but its confidence
       * average (0.6) is LOWER than the existing commit's (0.9), so the
       * existing commit wins and verbose does not replace it. */
      assert(db1_working_profile_local_observe(WORKING_PROFILE_FIELD_VERBOSITY, "verbose", 0.6,
                                               session_id(), 3) == 0);
      assert(db1_working_profile_local_get(WORKING_PROFILE_FIELD_VERBOSITY, &got) == 0);
      assert(strcmp(got.value, "terse") == 0);

      teardown(&ctx);
   }

   /* --- higher-confidence replacement does commit --- */
   {
      working_profile_test_db_t ctx = setup();
      /* Three observations at low confidence commit. */
      for (int i = 0; i < 3; i++)
         assert(db1_working_profile_local_observe(WORKING_PROFILE_FIELD_PROJECT_ROLE, "reviewer",
                                                  0.4, session_id(), 3) >= 0);

      db1_working_profile_local_state_t got;
      assert(db1_working_profile_local_get(WORKING_PROFILE_FIELD_PROJECT_ROLE, &got) == 0);
      assert(strcmp(got.value, "reviewer") == 0);

      /* Three observations of a different value at higher confidence
       * should replace the existing commit. */
      for (int i = 0; i < 3; i++)
         assert(db1_working_profile_local_observe(WORKING_PROFILE_FIELD_PROJECT_ROLE, "implementer",
                                                  0.95, session_id(), 3) >= 0);

      assert(db1_working_profile_local_get(WORKING_PROFILE_FIELD_PROJECT_ROLE, &got) == 0);
      assert(strcmp(got.value, "implementer") == 0);
      assert(got.score > 0.9);

      teardown(&ctx);
   }

   /* --- working_profile_list orders by confidence desc --- */
   {
      working_profile_test_db_t ctx = setup();
      for (int i = 0; i < 3; i++)
         db1_working_profile_local_observe(WORKING_PROFILE_FIELD_VERBOSITY, "terse", 0.95,
                                           session_id(), 3);
      for (int i = 0; i < 3; i++)
         db1_working_profile_local_observe(WORKING_PROFILE_FIELD_COMMUNICATION_STYLE, "direct", 0.7,
                                           session_id(), 3);

      db1_working_profile_local_state_t rows[8];
      int n = db1_working_profile_local_list(rows, 8);
      assert(n == 2);
      /* Verbosity (0.95) before communication_style (0.70). */
      assert(strcmp(rows[0].field, WORKING_PROFILE_FIELD_VERBOSITY) == 0);
      assert(strcmp(rows[1].field, WORKING_PROFILE_FIELD_COMMUNICATION_STYLE) == 0);

      teardown(&ctx);
   }

   /* --- reset wipes both observations and committed state --- */
   {
      working_profile_test_db_t ctx = setup();
      for (int i = 0; i < 3; i++)
         db1_working_profile_local_observe(WORKING_PROFILE_FIELD_VERBOSITY, "terse", 0.9,
                                           session_id(), 3);
      db1_working_profile_local_state_t got;
      assert(db1_working_profile_local_get(WORKING_PROFILE_FIELD_VERBOSITY, &got) == 0);

      assert(db1_working_profile_local_reset_field(WORKING_PROFILE_FIELD_VERBOSITY) == 0);
      assert(db1_working_profile_local_get(WORKING_PROFILE_FIELD_VERBOSITY, &got) == 1);

      /* After reset, the next three observations must re-commit cleanly. */
      for (int i = 0; i < 3; i++)
         db1_working_profile_local_observe(WORKING_PROFILE_FIELD_VERBOSITY, "concise", 0.8,
                                           session_id(), 3);
      assert(db1_working_profile_local_get(WORKING_PROFILE_FIELD_VERBOSITY, &got) == 0);
      assert(strcmp(got.value, "concise") == 0);

      teardown(&ctx);
   }

   /* --- calibrated profile gates weak working-profile commits --- */
   {
      working_profile_test_db_t ctx = setup();
      open_db2();

      const char *profile = "{\"buckets\":["
                            "{\"range\":[0.0,0.8],\"lower_credible_bound\":0.20},"
                            "{\"range\":[0.8,0.9],\"lower_credible_bound\":0.90}],"
                            "\"conformal\":{\"reject_below\":0.0}}";
      assert(db2_calibration_profile_write("working_profile", "field", "global", "", "v1", profile,
                                           NULL, 0) == 0);

      for (int i = 0; i < 3; i++)
         assert(db1_working_profile_local_observe(WORKING_PROFILE_FIELD_VERBOSITY, "weak", 0.75,
                                                  session_id(), 3) == 0);

      db1_working_profile_local_state_t got;
      assert(db1_working_profile_local_get(WORKING_PROFILE_FIELD_VERBOSITY, &got) == 1);

      for (int i = 0; i < 3; i++)
         assert(db1_working_profile_local_observe(WORKING_PROFILE_FIELD_VERBOSITY, "strong", 0.85,
                                                  session_id(), 3) >= 0);
      assert(db1_working_profile_local_get(WORKING_PROFILE_FIELD_VERBOSITY, &got) == 0);
      assert(strcmp(got.value, "strong") == 0);

      close_db2();
      teardown(&ctx);
   }

   /* --- invalid input is rejected --- */
   {
      working_profile_test_db_t ctx = setup();
      assert(db1_working_profile_local_observe(NULL, "x", 0.5, session_id(), 3) == -1);
      assert(db1_working_profile_local_observe("", "x", 0.5, session_id(), 3) == -1);
      assert(db1_working_profile_local_observe("v", NULL, 0.5, session_id(), 3) == -1);
      assert(db1_working_profile_local_observe("v", "", 0.5, session_id(), 3) == -1);
      teardown(&ctx);
   }

   printf("all tests passed\n");
   return 0;
}
