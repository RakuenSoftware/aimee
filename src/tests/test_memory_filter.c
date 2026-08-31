#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "aimee.h"
#include "db1_client/db1.h"
#include "modules/db2/c/db2.h"
#include "modules/db2/c/db2_test_shim.h"
#include "memory.h"
#include "cJSON.h"

/* ---- memory_filter_from_scope ---- */

static void test_filter_scope_workspace(void)
{
   memory_filter_t f;
   memory_filter_from_scope("workspace", "myws", &f);
   assert(strcmp(f.scope.workspace, "myws") == 0);
   assert(f.scope.project[0] == '\0');
   assert(f.scope.session[0] == '\0');
   assert(f.scope.user[0] == '\0');
   assert(f.visibility == MEMORY_VISIBILITY_DEFAULT);
   printf("  filter_scope_workspace: ok\n");
}

static void test_filter_scope_project(void)
{
   memory_filter_t f;
   memory_filter_from_scope("project", "myproj", &f);
   assert(strcmp(f.scope.project, "myproj") == 0);
   assert(f.scope.workspace[0] == '\0');
   printf("  filter_scope_project: ok\n");
}

static void test_filter_scope_session(void)
{
   memory_filter_t f;
   memory_filter_from_scope("session", "sess-42", &f);
   assert(strcmp(f.scope.session, "sess-42") == 0);
   assert(f.scope.workspace[0] == '\0');
   printf("  filter_scope_session: ok\n");
}

static void test_filter_scope_user(void)
{
   memory_filter_t f;
   memory_filter_from_scope("user", "alice", &f);
   assert(strcmp(f.scope.user, "alice") == 0);
   assert(f.scope.workspace[0] == '\0');
   printf("  filter_scope_user: ok\n");
}

static void test_filter_scope_unknown_falls_back_to_workspace(void)
{
   memory_filter_t f;
   memory_filter_from_scope("whatever", "val", &f);
   assert(strcmp(f.scope.workspace, "val") == 0);
   printf("  filter_scope_unknown_fallback: ok\n");
}

static void test_filter_scope_empty_value_noop(void)
{
   memory_filter_t f;
   memory_filter_from_scope("workspace", "", &f);
   assert(f.scope.workspace[0] == '\0');
   printf("  filter_scope_empty_value_noop: ok\n");
}

static void test_filter_scope_null_type_with_value_noop(void)
{
   memory_filter_t f;
   memory_filter_from_scope(NULL, "x", &f);
   assert(f.scope.workspace[0] == '\0');
   assert(f.scope.project[0] == '\0');
   printf("  filter_scope_null_type_noop: ok\n");
}

/* ---- memory_effective_importance ---- */

static void test_effective_importance_null_returns_zero(void)
{
   double v = memory_effective_importance(NULL, 0);
   assert(v == 0.0);
   printf("  effective_importance_null: ok\n");
}

static void test_effective_importance_fresh_row(void)
{
   memory_t m;
   memset(&m, 0, sizeof(m));
   m.confidence = 0.9;
   m.salience = 0.9;
   m.use_count = 0;
   /* no created_at → age_days == 0, decay == 1.0, reinforcement == 1.0 */
   double v = memory_effective_importance(&m, 0);
   /* base = 0.9 * 0.9 = 0.81; result = 0.81 */
   assert(v > 0.8 && v < 0.82);
   printf("  effective_importance_fresh: ok\n");
}

static void test_effective_importance_decays_with_age(void)
{
   memory_t m;
   memset(&m, 0, sizeof(m));
   m.confidence = 1.0;
   m.salience = 1.0;
   m.use_count = 0;
   /* Created 365 days ago; default halflife 90d → exp(-365/90) ≈ 0.017 */
   time_t now = time(NULL);
   time_t created = now - 365 * 86400;
   struct tm *tm_val = gmtime(&created);
   strftime(m.created_at, sizeof(m.created_at), "%Y-%m-%dT%H:%M:%SZ", tm_val);
   double v = memory_effective_importance(&m, now);
   assert(v < 0.05); /* heavily decayed */
   printf("  effective_importance_decay: ok\n");
}

static void test_effective_importance_reinforcement(void)
{
   memory_t m;
   memset(&m, 0, sizeof(m));
   m.confidence = 0.8;
   m.salience = 0.8;
   m.use_count = 100;
   /* reinforcement = 1.0 + 0.3 * log1p(100) ≈ 1.0 + 0.3 * 4.615 ≈ 2.0 (capped) */
   double v = memory_effective_importance(&m, 0);
   /* base = 0.64, decay ≈ 1.0 (no created_at), reinforcement = 2.0; result = 1.28 → clamped 1.0 */
   assert(v == 1.0);
   printf("  effective_importance_reinforcement_cap: ok\n");
}

static void test_effective_importance_clamped(void)
{
   memory_t m;
   memset(&m, 0, sizeof(m));
   m.confidence = 2.0; /* out of range, clamped to 1.0 */
   m.salience = 2.0;
   m.use_count = 999;
   double v = memory_effective_importance(&m, 0);
   assert(v >= 0.0 && v <= 1.0);
   printf("  effective_importance_clamped: ok\n");
}

/* ---- memory_filter_to_json ---- */

static void test_filter_to_json_workspace(void)
{
   memory_filter_t f;
   memory_filter_from_scope("workspace", "ws1", &f);
   cJSON *j = memory_filter_to_json(&f);
   assert(j != NULL);
   cJSON *scope = cJSON_GetObjectItem(j, "scope");
   assert(scope != NULL);
   cJSON *ws = cJSON_GetObjectItem(scope, "workspace");
   assert(ws != NULL && strcmp(ws->valuestring, "ws1") == 0);
   cJSON *vis = cJSON_GetObjectItem(j, "visibility");
   assert(vis != NULL && strcmp(vis->valuestring, "default") == 0);
   cJSON_Delete(j);
   printf("  filter_to_json_workspace: ok\n");
}

static void test_filter_to_json_null(void)
{
   cJSON *j = memory_filter_to_json(NULL);
   assert(j != NULL);
   assert(cJSON_IsNull(j));
   cJSON_Delete(j);
   printf("  filter_to_json_null: ok\n");
}

int main(void)
{
   printf("memory_filter:\n");

   db2_test_shim_open();

   test_filter_scope_workspace();
   test_filter_scope_project();
   test_filter_scope_session();
   test_filter_scope_user();
   test_filter_scope_unknown_falls_back_to_workspace();
   test_filter_scope_empty_value_noop();
   test_filter_scope_null_type_with_value_noop();

   test_effective_importance_null_returns_zero();
   test_effective_importance_fresh_row();
   test_effective_importance_decays_with_age();
   test_effective_importance_reinforcement();
   test_effective_importance_clamped();

   test_filter_to_json_workspace();
   test_filter_to_json_null();

   printf("All memory_filter tests passed.\n");
   return 0;
}
