/* test_roadmap.c — unit tests for the spec-driven roadmap data model
 * (src/headers/roadmap.h, implemented in src/kb/roadmap.c).
 *
 * Tests:
 *   1. roadmap_validate_decomposition accepts a well-formed milestone/slice/task
 *      tree with a sibling dependency DAG.
 *   2. validation rejects a sibling dependency cycle.
 *   3. validation rejects a leaf task missing owned_files / acceptance_criteria.
 *   4. roadmap_create_from_decomposition writes a committed roadmap + plan_unit
 *      artifacts (proposed -> committed) with the documented kinds/scope.
 *   5. projection write + rebuild produce byte-identical ROADMAP.md / STATE.md
 *      (deterministic, no timestamps).
 *   6. roadmap_new drives create through the injectable decomposer, and returns
 *      -1 when no decomposer is registered.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "db2_test_shim.h"
#include "roadmap.h"
#include "../db2/artifacts.h"
#include "../db2/db2_internal.h"
#include "../db2/db_postgres.h"

static void open_db(void)
{
   db2_test_shim_close();
   db2_test_shim_open();
}

static void close_db(void)
{
   db2_test_shim_close();
}

/* A well-formed decomposition: m1 -> s1 -> {t1, t2}, t2 depends on t1. */
static const char *GOOD_DECOMP =
    "{\"goal\":\"Add device-flow login\",\"planning_depth\":\"standard\","
    "\"token_profile\":\"balanced\",\"units\":["
    "{\"local_id\":\"m1\",\"level\":\"milestone\",\"parent\":\"\",\"title\":\"Auth\","
    "\"intent\":\"add oauth\",\"depends_on\":[],\"acceptance_criteria\":[\"compiles\"],"
    "\"tool_policy_mode\":\"planning\"},"
    "{\"local_id\":\"s1\",\"level\":\"slice\",\"parent\":\"m1\",\"title\":\"Client\","
    "\"intent\":\"client side\",\"depends_on\":[],\"acceptance_criteria\":[\"x\"]},"
    "{\"local_id\":\"t1\",\"level\":\"task\",\"parent\":\"s1\",\"title\":\"poller\","
    "\"intent\":\"poll\",\"depends_on\":[],\"owned_files\":[\"src/auth/device.c\"],"
    "\"acceptance_criteria\":[\"polls with backoff\"],"
    "\"verification_commands\":[\"make auth-tests\"],\"tool_policy_mode\":\"execution\"},"
    "{\"local_id\":\"t2\",\"level\":\"task\",\"parent\":\"s1\",\"title\":\"timeout\","
    "\"intent\":\"timeout\",\"depends_on\":[\"t1\"],\"owned_files\":[\"src/auth/to.c\"],"
    "\"acceptance_criteria\":[\"returns AUTH_TIMEOUT\"]}"
    "]}";

/* ---- 1. validate accepts good ---- */
static void test_validate_accepts_good(void)
{
   char err[256] = "";
   int rc = roadmap_validate_decomposition(GOOD_DECOMP, err, sizeof(err));
   assert(rc == 0);
   printf("  validate_accepts_good: ok\n");
}

/* ---- 2. validate rejects a sibling cycle ---- */
static void test_validate_rejects_cycle(void)
{
   const char *cyc =
       "{\"goal\":\"g\",\"units\":["
       "{\"local_id\":\"m1\",\"level\":\"milestone\",\"parent\":\"\",\"title\":\"M\"},"
       "{\"local_id\":\"s1\",\"level\":\"slice\",\"parent\":\"m1\",\"title\":\"S\"},"
       "{\"local_id\":\"t1\",\"level\":\"task\",\"parent\":\"s1\",\"title\":\"T1\","
       "\"depends_on\":[\"t2\"],\"owned_files\":[\"a.c\"],\"acceptance_criteria\":[\"x\"]},"
       "{\"local_id\":\"t2\",\"level\":\"task\",\"parent\":\"s1\",\"title\":\"T2\","
       "\"depends_on\":[\"t1\"],\"owned_files\":[\"b.c\"],\"acceptance_criteria\":[\"y\"]}"
       "]}";
   char err[256] = "";
   int rc = roadmap_validate_decomposition(cyc, err, sizeof(err));
   assert(rc == -1);
   printf("  validate_rejects_cycle: ok\n");
}

/* ---- 3. validate rejects an incomplete leaf task ---- */
static void test_validate_rejects_bad_leaf(void)
{
   /* task with no owned_files */
   const char *no_files =
       "{\"goal\":\"g\",\"units\":["
       "{\"local_id\":\"m1\",\"level\":\"milestone\",\"parent\":\"\",\"title\":\"M\"},"
       "{\"local_id\":\"s1\",\"level\":\"slice\",\"parent\":\"m1\",\"title\":\"S\"},"
       "{\"local_id\":\"t1\",\"level\":\"task\",\"parent\":\"s1\",\"title\":\"T\","
       "\"owned_files\":[],\"acceptance_criteria\":[\"x\"]}"
       "]}";
   char err[256] = "";
   assert(roadmap_validate_decomposition(no_files, err, sizeof(err)) == -1);

   /* task with no acceptance_criteria */
   const char *no_ac =
       "{\"goal\":\"g\",\"units\":["
       "{\"local_id\":\"m1\",\"level\":\"milestone\",\"parent\":\"\",\"title\":\"M\"},"
       "{\"local_id\":\"s1\",\"level\":\"slice\",\"parent\":\"m1\",\"title\":\"S\"},"
       "{\"local_id\":\"t1\",\"level\":\"task\",\"parent\":\"s1\",\"title\":\"T\","
       "\"owned_files\":[\"a.c\"],\"acceptance_criteria\":[]}"
       "]}";
   assert(roadmap_validate_decomposition(no_ac, err, sizeof(err)) == -1);

   /* empty goal */
   const char *no_goal = "{\"goal\":\"\",\"units\":[]}";
   assert(roadmap_validate_decomposition(no_goal, err, sizeof(err)) == -1);
   printf("  validate_rejects_bad_leaf: ok\n");
}

/* ---- 4. create_from_decomposition commits artifacts ---- */
static void test_create_commits(void)
{
   open_db();

   char rid[64] = "";
   int rc = roadmap_create_from_decomposition(GOOD_DECOMP, rid, sizeof(rid));
   assert(rc == 0);
   assert(rid[0] != '\0');

   /* roadmap artifact landed, kind=roadmap, state=committed */
   db2_artifact_row_t row;
   int cc = 0;
   rc = db2_artifact_read(rid, &row, NULL, 0, &cc);
   assert(rc == 0);
   assert(strcmp(row.kind, "roadmap") == 0);
   assert(strcmp(row.state, "committed") == 0);

   /* the four plan_units landed, scoped to the roadmap, all committed */
   void *conn = db2_conn();
   assert(conn);
   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "SELECT count(*) FROM artifacts WHERE kind='plan_unit' AND scope_id=?1 AND "
                        "state='committed'",
                        err, sizeof(err));
   assert(st);
   aimee_pg_bind_text(st, "?1", rid);
   assert(aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW);
   assert(aimee_pg_column_int(st, 0) == 4);
   aimee_pg_finalize(st);

   /* rejects malformed input without writing */
   assert(roadmap_create_from_decomposition("not-json{", rid, sizeof(rid)) == -1);

   close_db();
   printf("  create_commits: ok\n");
}

static char *slurp(const char *path)
{
   FILE *f = fopen(path, "rb");
   if (!f)
      return NULL;
   fseek(f, 0, SEEK_END);
   long n = ftell(f);
   fseek(f, 0, SEEK_SET);
   char *buf = malloc((size_t)n + 1);
   size_t rd = fread(buf, 1, (size_t)n, f);
   buf[rd] = '\0';
   fclose(f);
   return buf;
}

/* ---- 5. projection write + rebuild are byte-identical ---- */
static void test_projection_rebuild_identity(void)
{
   open_db();

   char cwd[1024];
   assert(getcwd(cwd, sizeof(cwd)) != NULL);
   const char *tmp = "./.roadmap_test_tmp";
   mkdir(tmp, 0755);
   assert(chdir(tmp) == 0);

   char rid[64] = "";
   assert(roadmap_create_from_decomposition(GOOD_DECOMP, rid, sizeof(rid)) == 0);

   assert(roadmap_projections_write(rid) == 0);
   char *roadmap_a = slurp(".aimee/roadmap/ROADMAP.md");
   char *state_a = slurp(".aimee/roadmap/STATE.md");
   assert(roadmap_a && state_a);

   assert(roadmap_projections_rebuild(rid) == 0);
   char *roadmap_b = slurp(".aimee/roadmap/ROADMAP.md");
   char *state_b = slurp(".aimee/roadmap/STATE.md");
   assert(roadmap_b && state_b);

   assert(strcmp(roadmap_a, roadmap_b) == 0);
   assert(strcmp(state_a, state_b) == 0);

   free(roadmap_a);
   free(roadmap_b);
   free(state_a);
   free(state_b);

   /* cleanup */
   unlink(".aimee/roadmap/ROADMAP.md");
   unlink(".aimee/roadmap/STATE.md");
   rmdir(".aimee/roadmap");
   rmdir(".aimee");
   assert(chdir(cwd) == 0);
   rmdir(tmp);

   close_db();
   printf("  projection_rebuild_identity: ok\n");
}

/* ---- 6. roadmap_new via injectable decomposer ---- */
static int stub_decomposer(const char *goal, const char *depth, const char *profile,
                           char **out_json, void *ud)
{
   (void)goal;
   (void)depth;
   (void)profile;
   (void)ud;
   *out_json = strdup(GOOD_DECOMP);
   return 0;
}

static void test_new_via_decomposer(void)
{
   open_db();

   /* no decomposer registered -> -1 */
   roadmap_set_decomposer(NULL, NULL);
   char rid[64] = "";
   assert(roadmap_new("g", "standard", "balanced", rid, sizeof(rid)) == -1);

   /* with a stub decomposer -> commits */
   roadmap_set_decomposer(stub_decomposer, NULL);
   assert(roadmap_new("g", "standard", "balanced", rid, sizeof(rid)) == 0);
   assert(rid[0] != '\0');

   roadmap_set_decomposer(NULL, NULL);
   close_db();
   printf("  new_via_decomposer: ok\n");
}

/* ---- 7. roadmap_show_json / roadmap_list_json return the tree as JSON ---- */
static void test_show_and_list_json(void)
{
   open_db();

   char rid[64] = "";
   assert(roadmap_create_from_decomposition(GOOD_DECOMP, rid, sizeof(rid)) == 0);
   assert(rid[0] != '\0');

   /* show: {"roadmap":{...},"units":[...]} with the goal and all 4 units. */
   char *doc = NULL;
   assert(roadmap_show_json(rid, &doc) == 0);
   assert(doc != NULL);
   cJSON *o = cJSON_Parse(doc);
   free(doc);
   assert(o != NULL);
   cJSON *rm = cJSON_GetObjectItemCaseSensitive(o, "roadmap");
   assert(cJSON_IsObject(rm));
   cJSON *goal = cJSON_GetObjectItemCaseSensitive(rm, "goal");
   assert(cJSON_IsString(goal) && strcmp(goal->valuestring, "Add device-flow login") == 0);
   cJSON *units = cJSON_GetObjectItemCaseSensitive(o, "units");
   assert(cJSON_IsArray(units) && cJSON_GetArraySize(units) == 4);
   cJSON_Delete(o);

   /* show with a NULL id fails cleanly without leaking *out. */
   doc = NULL;
   assert(roadmap_show_json(NULL, &doc) == -1);
   assert(doc == NULL);

   /* list: {"roadmaps":[...]} containing the roadmap we just committed. */
   doc = NULL;
   assert(roadmap_list_json(&doc) == 0);
   assert(doc != NULL);
   cJSON *lo = cJSON_Parse(doc);
   free(doc);
   assert(lo != NULL);
   cJSON *arr = cJSON_GetObjectItemCaseSensitive(lo, "roadmaps");
   assert(cJSON_IsArray(arr));
   int found = 0;
   cJSON *e;
   cJSON_ArrayForEach(e, arr)
   {
      cJSON *id = cJSON_GetObjectItemCaseSensitive(e, "id");
      if (cJSON_IsString(id) && strcmp(id->valuestring, rid) == 0)
         found = 1;
   }
   assert(found);
   cJSON_Delete(lo);

   close_db();
   printf("  show_and_list_json: ok\n");
}

int main(void)
{
   printf("roadmap:\n");
   test_validate_accepts_good();
   test_validate_rejects_cycle();
   test_validate_rejects_bad_leaf();
   test_create_commits();
   test_projection_rebuild_identity();
   test_new_via_decomposer();
   test_show_and_list_json();
   printf("All roadmap tests passed.\n");
   return 0;
}
