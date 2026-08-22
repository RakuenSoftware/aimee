/* test_payload_rewrite.c: policy tests for prompt-cache-aware payload rewrite. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "db1.h"
#include "payload_rewrite.h"
#include "platform_test_util.h"

static void reset_db(const char *path)
{
   db1_shutdown();
   platform_test_remove_sqlite(path);
   assert(db1_init(path) == 0);
}

static void set_rewrite_config(int enabled, int min_savings_tokens, double hard_threshold)
{
   assert(config_set_cache_aware_rewrite_enabled(enabled) == 0);
   assert(config_set_cache_aware_rewrite_min_savings_tokens(min_savings_tokens) == 0);
   assert(config_set_cache_aware_rewrite_hard_context_threshold(hard_threshold) == 0);
   platform_setenv("AIMEE_NO_CACHE", "1");
}

static void seed_state(const char *sid, const char *hash, int pending_bytes)
{
   payload_rewrite_state_t s;
   memset(&s, 0, sizeof(s));
   snprintf(s.session_id, sizeof(s.session_id), "%s", sid);
   snprintf(s.last_prefix_hash, sizeof(s.last_prefix_hash), "%s", hash);
   s.last_payload_tokens = 100;
   s.bytes_saved_pending = pending_bytes;
   assert(db1_payload_rewrite_state_set(&s) == 0);
}

static void seed_state_with_consecutive(const char *sid, const char *hash, int consecutive)
{
   payload_rewrite_state_t s;
   memset(&s, 0, sizeof(s));
   snprintf(s.session_id, sizeof(s.session_id), "%s", sid);
   snprintf(s.last_prefix_hash, sizeof(s.last_prefix_hash), "%s", hash);
   s.last_payload_tokens = 100;
   s.consecutive_deferred_count = consecutive;
   assert(db1_payload_rewrite_state_set(&s) == 0);
}

static void test_hash_stability(void)
{
   char a[17], b[17], c[17], d[17];
   payload_rewrite_prefix_hash("system", "ctx", a, sizeof(a));
   payload_rewrite_prefix_hash("system", "ctx", b, sizeof(b));
   payload_rewrite_prefix_hash("system2", "ctx", c, sizeof(c));
   payload_rewrite_prefix_hash("ab", "c", d, sizeof(d));

   assert(strlen(a) == 16);
   assert(strcmp(a, b) == 0);
   assert(strcmp(a, c) != 0);

   char e[17];
   payload_rewrite_prefix_hash("a", "bc", e, sizeof(e));
   assert(strcmp(d, e) != 0);
}

static void test_disabled_noop(const char *path)
{
   reset_db(path);
   set_rewrite_config(0, 500, 0.85);
   session_id_set_override("rewrite-disabled");

   assert(payload_rewrite_track_request("system", NULL, 100, 1000) == 0);

   payload_rewrite_state_t out;
   assert(db1_payload_rewrite_state_get("rewrite-disabled", &out) != 0);
   session_id_clear_override();
}

static void test_decision_matrix(const char *path)
{
   reset_db(path);
   set_rewrite_config(1, 500, 0.50);

   char hash[17], other[17];
   payload_rewrite_prefix_hash("stable", NULL, hash, sizeof(hash));
   payload_rewrite_prefix_hash("changed", NULL, other, sizeof(other));

   payload_rewrite_decision_t dec;
   assert(payload_rewrite_should_defer("matrix", hash, 100, 1000, &dec) == 0);
   assert(dec.defer == 0);
   assert(strcmp(dec.reason, "no_prior_state") == 0);

   seed_state("matrix", hash, 0);
   assert(payload_rewrite_should_defer("matrix", other, 100, 1000, &dec) == 0);
   assert(dec.defer == 0);
   assert(strcmp(dec.reason, "prefix_changed") == 0);

   seed_state("matrix", hash, 0);
   assert(payload_rewrite_should_defer("matrix", hash, 600, 1000, &dec) == 0);
   assert(dec.defer == 0);
   assert(strcmp(dec.reason, "context_pressure") == 0);

   seed_state("matrix", hash, 1000);
   assert(payload_rewrite_should_defer("matrix", hash, 100, 1000, &dec) == 0);
   assert(dec.defer == 1);
   assert(strcmp(dec.reason, "cache_warm") == 0);

   seed_state("matrix", hash, 2500);
   assert(payload_rewrite_should_defer("matrix", hash, 100, 1000, &dec) == 0);
   assert(dec.defer == 0);
   assert(strcmp(dec.reason, "savings_threshold") == 0);
}

static void test_track_request_records_flow(const char *path)
{
   reset_db(path);
   set_rewrite_config(1, 500, 0.85);
   session_id_set_override("rewrite-flow");

   assert(payload_rewrite_track_request("system", NULL, 100, 1000) == 0);
   payload_rewrite_state_t out;
   assert(db1_payload_rewrite_state_get("rewrite-flow", &out) == 0);
   assert(out.payload_epoch == 1);
   assert(out.deferred_rewrite_count == 0);
   assert(strcmp(out.rewrite_reason, "no_prior_state") == 0);

   assert(payload_rewrite_track_request("system", NULL, 100, 1000) == 0);
   assert(db1_payload_rewrite_state_get("rewrite-flow", &out) == 0);
   assert(out.payload_epoch == 1);
   assert(out.deferred_rewrite_count == 1);
   assert(out.bytes_saved_pending == 400);
   assert(strcmp(out.rewrite_reason, "cache_warm") == 0);

   assert(payload_rewrite_track_request("changed", NULL, 100, 1000) == 0);
   assert(db1_payload_rewrite_state_get("rewrite-flow", &out) == 0);
   assert(out.payload_epoch == 2);
   assert(out.bytes_saved_pending == 0);
   assert(strcmp(out.rewrite_reason, "prefix_changed") == 0);

   session_id_clear_override();
}

static void test_status_derived_fields(const char *path)
{
   reset_db(path);
   set_rewrite_config(1, 500, 0.85);
   session_id_set_override("rewrite-status");

   assert(payload_rewrite_record_deferred(400, 100, "cache_warm", "hash") == 0);
   assert(payload_rewrite_record_forced(200, "context_pressure", "hash") == 0);

   cJSON *status = tool_payload_rewrite_status(NULL);
   assert(status != NULL);
   cJSON *forced = cJSON_GetObjectItem(status, "forced_rewrite_count");
   cJSON *estimate = cJSON_GetObjectItem(status, "cache_hit_estimate");
   cJSON *deferred = cJSON_GetObjectItem(status, "deferred_rewrite_count");
   assert(cJSON_IsNumber(forced));
   assert(cJSON_IsNumber(estimate));
   assert(cJSON_IsNumber(deferred));
   assert(forced->valueint == 1);
   assert(deferred->valueint == 1);
   assert(fabs(estimate->valuedouble - 0.5) < 0.0001);
   cJSON_Delete(status);

   session_id_clear_override();
}

static void test_cache_horizon_trigger(const char *path)
{
   reset_db(path);
   set_rewrite_config(1, 500, 0.85);

   char hash[17];
   payload_rewrite_prefix_hash("stable", NULL, hash, sizeof(hash));

   /* One below ceiling: still defers */
   seed_state_with_consecutive("horizon-test", hash, 19);
   payload_rewrite_decision_t dec;
   assert(payload_rewrite_should_defer("horizon-test", hash, 100, 1000, &dec) == 0);
   assert(dec.defer == 1);
   assert(strcmp(dec.reason, "cache_warm") == 0);

   /* At ceiling: force */
   seed_state_with_consecutive("horizon-test", hash, 20);
   assert(payload_rewrite_should_defer("horizon-test", hash, 100, 1000, &dec) == 0);
   assert(dec.defer == 0);
   assert(strcmp(dec.reason, "cache_horizon") == 0);

   /* Max=0 disables the ceiling check (also disable segment check to isolate) */
   assert(config_set_cache_aware_rewrite_max_defer_turns(0) == 0);
   assert(config_set_cache_aware_rewrite_segment_check_turns(0) == 0);
   platform_setenv("AIMEE_NO_CACHE", "1");
   seed_state_with_consecutive("horizon-test", hash, 100);
   assert(payload_rewrite_should_defer("horizon-test", hash, 100, 1000, &dec) == 0);
   assert(dec.defer == 1);
   /* restore defaults */
   assert(config_set_cache_aware_rewrite_max_defer_turns(20) == 0);
   assert(config_set_cache_aware_rewrite_segment_check_turns(5) == 0);
   platform_setenv("AIMEE_NO_CACHE", "1");
}

static void test_required_segment_miss_trigger(const char *path)
{
   reset_db(path);
   set_rewrite_config(1, 500, 0.85);

   char hash[17];
   payload_rewrite_prefix_hash("stable", NULL, hash, sizeof(hash));
   payload_rewrite_decision_t dec;

   /* At segment check boundary (5): force with required_segment_miss */
   seed_state_with_consecutive("seg-test", hash, 5);
   assert(payload_rewrite_should_defer("seg-test", hash, 100, 1000, &dec) == 0);
   assert(dec.defer == 0);
   assert(strcmp(dec.reason, "required_segment_miss") == 0);

   /* One past boundary (6): defer again */
   seed_state_with_consecutive("seg-test", hash, 6);
   assert(payload_rewrite_should_defer("seg-test", hash, 100, 1000, &dec) == 0);
   assert(dec.defer == 1);
   assert(strcmp(dec.reason, "cache_warm") == 0);

   /* Segment check=0 disables periodic check */
   assert(config_set_cache_aware_rewrite_segment_check_turns(0) == 0);
   platform_setenv("AIMEE_NO_CACHE", "1");
   seed_state_with_consecutive("seg-test", hash, 5);
   assert(payload_rewrite_should_defer("seg-test", hash, 100, 1000, &dec) == 0);
   assert(dec.defer == 1);
   /* restore default */
   assert(config_set_cache_aware_rewrite_segment_check_turns(5) == 0);
   platform_setenv("AIMEE_NO_CACHE", "1");
}

static void test_multi_turn_deferral_sequence(const char *path)
{
   reset_db(path);
   set_rewrite_config(1, 10000, 0.85); /* high savings threshold so only triggers fire */
   session_id_set_override("multi-turn");

   /* Simulate repeated turns with stable prefix: first turn forces (no prior state),
    * then deferrals accumulate until segment check fires at turn 6. */
   assert(payload_rewrite_track_request("system", NULL, 100, 1000) == 0); /* turn 1: forced */

   payload_rewrite_state_t out;
   assert(db1_payload_rewrite_state_get("multi-turn", &out) == 0);
   assert(out.payload_epoch == 1);
   assert(out.consecutive_deferred_count == 0);

   /* turns 2-6: deferred (consecutive increments to 5 after 5 deferrals) */
   for (int i = 0; i < 5; i++)
      assert(payload_rewrite_track_request("system", NULL, 100, 1000) == 0);

   assert(db1_payload_rewrite_state_get("multi-turn", &out) == 0);
   assert(out.deferred_rewrite_count == 5);
   assert(out.consecutive_deferred_count == 5);

   /* turn 7: segment check fires → forced (consecutive=5 hits segment_check_turns=5) */
   assert(payload_rewrite_track_request("system", NULL, 100, 1000) == 0);
   assert(db1_payload_rewrite_state_get("multi-turn", &out) == 0);
   assert(out.payload_epoch == 2);
   assert(out.consecutive_deferred_count == 0);
   assert(strcmp(out.rewrite_reason, "required_segment_miss") == 0);

   session_id_clear_override();
}

static void test_consecutive_count_resets_on_force(const char *path)
{
   reset_db(path);
   set_rewrite_config(1, 500, 0.85);
   session_id_set_override("reset-test");

   /* force → defer × 3 → force (via prefix change) → consecutive should be 0 */
   assert(payload_rewrite_track_request("sys-a", NULL, 100, 1000) ==
          0); /* forced: no_prior_state */
   assert(payload_rewrite_track_request("sys-a", NULL, 100, 1000) == 0); /* deferred */
   assert(payload_rewrite_track_request("sys-a", NULL, 100, 1000) == 0); /* deferred */
   assert(payload_rewrite_track_request("sys-a", NULL, 100, 1000) == 0); /* deferred */

   payload_rewrite_state_t out;
   assert(db1_payload_rewrite_state_get("reset-test", &out) == 0);
   assert(out.consecutive_deferred_count == 3);

   /* prefix change forces rewrite, resets consecutive */
   assert(payload_rewrite_track_request("sys-b", NULL, 100, 1000) == 0);
   assert(db1_payload_rewrite_state_get("reset-test", &out) == 0);
   assert(out.consecutive_deferred_count == 0);
   assert(strcmp(out.rewrite_reason, "prefix_changed") == 0);

   session_id_clear_override();
}

static void test_context_pressure_overrides_deferral(const char *path)
{
   reset_db(path);
   set_rewrite_config(1, 500, 0.50); /* 50% context = pressure */

   char hash[17];
   payload_rewrite_prefix_hash("stable", NULL, hash, sizeof(hash));
   seed_state("pressure-test", hash, 0);

   /* Under threshold: defer */
   payload_rewrite_decision_t dec;
   assert(payload_rewrite_should_defer("pressure-test", hash, 499, 1000, &dec) == 0);
   assert(dec.defer == 1);

   /* At threshold: force, even with cache_warm prefix */
   assert(payload_rewrite_should_defer("pressure-test", hash, 501, 1000, &dec) == 0);
   assert(dec.defer == 0);
   assert(strcmp(dec.reason, "context_pressure") == 0);
}

static void test_failure_injection(const char *path)
{
   reset_db(path);
   set_rewrite_config(1, 500, 0.85);

   payload_rewrite_decision_t dec;

   /* Empty session ID → invalid_input error */
   assert(payload_rewrite_should_defer("", "deadbeef01234567", 100, 1000, &dec) == -1);

   /* Empty hash → invalid_input error */
   assert(payload_rewrite_should_defer("fail-session", "", 100, 1000, &dec) == -1);

   /* No prior state for valid session → no_prior_state */
   assert(payload_rewrite_should_defer("fail-session", "deadbeef01234567", 100, 1000, &dec) == 0);
   assert(dec.defer == 0);
   assert(strcmp(dec.reason, "no_prior_state") == 0);
}

int main(void)
{
   printf("payload_rewrite: ");

   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-payload-rewrite-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);
   platform_setenv("HOME", tmpdir);
   platform_setenv("AIMEE_NO_CACHE", "1");

   char path[512];
   snprintf(path, sizeof(path), "%s/payload-rewrite.db", tmpdir);

   test_hash_stability();
   test_disabled_noop(path);
   test_decision_matrix(path);
   test_track_request_records_flow(path);
   test_status_derived_fields(path);
   test_cache_horizon_trigger(path);
   test_required_segment_miss_trigger(path);
   test_multi_turn_deferral_sequence(path);
   test_consecutive_count_resets_on_force(path);
   test_context_pressure_overrides_deferral(path);
   test_failure_injection(path);

   db1_shutdown();
   platform_test_rmrf(tmpdir);
   printf("ok\n");
   return 0;
}
