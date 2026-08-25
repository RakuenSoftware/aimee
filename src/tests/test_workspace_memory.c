#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "aimee.h"
#include "cJSON.h"
#include "dashboard.h"
#include "modules/db2/c/db2.h"
#include "modules/db2/c/db2_test_shim.h"
#include "platform_test_util.h"
#include <aimee/workspace/workspace.h>
#include "../modules/db2/c/db2_internal.h"
#include "../modules/db2/c/db_postgres.h"
#include "../modules/db2/c/lifecycle.h"
#include "../modules/db2/c/memory_briefing.h"
#include "../modules/db2/c/memory_lifecycle.h"
#include "../modules/db2/c/memory_query.h"
#include "../modules/db2/c/memory_relations.h"
#include "../modules/db2/c/memory_scope_query.h"

static char tmpdir[64];

/* Test helper: run a SELECT COUNT(*) query bound by ?1 = memory_id and
 * return the count. Used throughout this file to assert membership of
 * a memory in scope/workspace tables without bringing the sqlite3
 * statement plumbing into the test surface. */
static int count_for_memory(const char *sql, int64_t memory_id)
{
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(db2_conn(), sql, err, sizeof(err));
   assert(st != NULL);
   aimee_pg_bind_int64(st, "?1", memory_id);
   int v = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      v = aimee_pg_column_int(st, 0);
   aimee_pg_finalize(st);
   return v;
}

/* Test helper: same as count_for_memory but binds ?1 = key (TEXT). */
static int count_for_key(const char *sql, const char *key)
{
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(db2_conn(), sql, err, sizeof(err));
   assert(st != NULL);
   aimee_pg_bind_text(st, "?1", key);
   int v = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      v = aimee_pg_column_int(st, 0);
   aimee_pg_finalize(st);
   return v;
}

static void insert_memory_entity(int64_t memory_id, const char *entity)
{
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       db2_conn(), "INSERT INTO memory_entities(memory_id, entity) VALUES (?1, ?2)", err,
       sizeof(err));
   assert(st != NULL);
   assert(aimee_pg_bind_int64(st, "?1", memory_id) == 0);
   assert(aimee_pg_bind_text(st, "?2", entity) == 0);
   assert(aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE);
   aimee_pg_finalize(st);
}

static void setup(void)
{
   /* Isolate from real config so auto_tag_workspace doesn't pick up cwd workspaces */
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-wsmem-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);
   platform_setenv("HOME", tmpdir);

   db2_test_shim_open();
   db2_memory_scope_context_clear();
}

static void teardown(void)
{
   db2_memory_scope_context_clear();
   db2_test_shim_close();
   char cmd[256];
   snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
   (void)system(cmd);
}

static void test_tag_workspace(void)
{
   setup();
   memory_t m;
   memory_insert(TIER_L2, KIND_FACT, "db-host", "host at 10.0.0.5", 0.9, "s1", &m);

   int rc = memory_tag_workspace(m.id, "wol");
   assert(rc == 0);

   /* Verify tag exists */
   assert(count_for_memory(
              "SELECT COUNT(*) FROM memory_workspaces WHERE memory_id = ?1 AND workspace = 'wol'",
              m.id) == 1);
   assert(count_for_memory("SELECT COUNT(*) FROM memory_scopes WHERE memory_id = ?1"
                           " AND scope_type = 'workspace' AND scope_value = 'wol'",
                           m.id) == 1);

   teardown();
}

static void test_tag_generic_scope(void)
{
   setup();
   memory_t m;
   memory_insert(TIER_L2, KIND_FACT, "persona", "Alice prefers short summaries", 0.9, "s1", &m);

   assert(memory_tag_scope(m.id, "user", "alice") == 0);

   assert(count_for_memory("SELECT COUNT(*) FROM memory_scopes WHERE memory_id = ?1"
                           " AND scope_type = 'user' AND scope_value = 'alice'",
                           m.id) == 1);

   teardown();
}

static void test_tag_multiple_workspaces(void)
{
   setup();
   memory_t m;
   memory_insert(TIER_L2, KIND_FACT, "shared-fact", "network topology", 0.9, "s1", &m);

   memory_tag_workspace(m.id, "wol");
   memory_tag_workspace(m.id, "infrastructure");
   memory_tag_workspace(m.id, SHARED_WORKSPACE);

   assert(count_for_memory("SELECT COUNT(*) FROM memory_workspaces WHERE memory_id = ?1", m.id) ==
          3);

   teardown();
}

static void test_tag_idempotent(void)
{
   setup();
   memory_t m;
   memory_insert(TIER_L2, KIND_FACT, "key1", "content", 0.9, "s1", &m);

   /* Tag same workspace twice — should not duplicate */
   memory_tag_workspace(m.id, "wol");
   memory_tag_workspace(m.id, "wol");

   assert(count_for_memory("SELECT COUNT(*) FROM memory_workspaces WHERE memory_id = ?1", m.id) ==
          1);

   teardown();
}

static void test_cascade_delete(void)
{
   setup();
   memory_t m;
   memory_insert(TIER_L2, KIND_FACT, "deleteme", "content", 0.9, "s1", &m);
   memory_tag_workspace(m.id, "wol");

   memory_delete(m.id);

   /* Tags should be gone due to CASCADE */
   assert(count_for_memory("SELECT COUNT(*) FROM memory_workspaces WHERE memory_id = ?1", m.id) ==
          0);

   teardown();
}

static void test_ws_context_scoped_memories_first(void)
{
   setup();
   memory_t m;

   /* Insert a wol-scoped fact */
   memory_insert(TIER_L2, KIND_FACT, "wol-config", "WOL uses mTLS on port 8443", 0.9, "s1", &m);
   memory_tag_workspace(m.id, "wol");

   /* Insert an aimee-scoped fact */
   memory_insert(TIER_L2, KIND_FACT, "aimee-config", "aimee uses SQLite for storage", 0.9, "s1",
                 &m);
   memory_tag_workspace(m.id, "aimee");

   /* Insert a shared fact */
   memory_insert(TIER_L2, KIND_FACT, "infra-fact", "all services run on Proxmox VE", 0.9, "s1", &m);
   memory_tag_workspace(m.id, SHARED_WORKSPACE);

   /* Assemble context for "wol" workspace */
   char *ctx = memory_assemble_context_ws(NULL, "wol");
   assert(ctx != NULL);

   /* wol-scoped memory should be present */
   assert(strstr(ctx, "mTLS") != NULL);

   /* shared memory should be present */
   assert(strstr(ctx, "Proxmox") != NULL);

   /* aimee-scoped memory should NOT be in main sections (only in cross-workspace if high enough) */
   /* Since we don't set use_count>=5, it shouldn't appear in cross-workspace section either */
   char *aimee_ref = strstr(ctx, "SQLite for storage");
   assert(aimee_ref == NULL);

   free(ctx);
   teardown();
}

static void test_ws_untagged_treated_as_shared(void)
{
   setup();
   memory_t m;

   /* Insert an untagged legacy memory */
   memory_insert(TIER_L2, KIND_FACT, "legacy-fact", "legacy untagged memory content", 0.9, "s1",
                 &m);
   /* Don't tag it — should be treated as _shared */

   char *ctx = memory_assemble_context_ws(NULL, "wol");
   assert(ctx != NULL);

   /* Untagged memory should appear (treated as _shared) */
   assert(strstr(ctx, "legacy untagged") != NULL);

   free(ctx);
   teardown();
}

static void test_ws_cross_workspace_high_confidence(void)
{
   setup();
   memory_t m;

   /* Insert a high-confidence memory in another workspace */
   assert(memory_insert_ex(TIER_L2, KIND_FACT, "other-ws-fact",
                           "critical pattern from other project", "", 0.95, "s1",
                           MEMORY_AUTHORITY_USER, &m) == 0);
   memory_tag_workspace(m.id, "other-project");

   /* Bump use_count to >= 5 */
   for (int i = 0; i < 5; i++)
      memory_touch(m.id);

   char *ctx = memory_assemble_context_ws(NULL, "wol");
   assert(ctx != NULL);

   /* High-confidence cross-workspace memory should appear in Cross-Workspace section */
   assert(strstr(ctx, "Cross-Workspace") != NULL);
   assert(strstr(ctx, "critical pattern") != NULL);

   free(ctx);
   teardown();
}

static void test_ws_null_workspace_falls_back(void)
{
   setup();
   memory_t m;
   memory_insert(TIER_L2, KIND_FACT, "any-fact", "some content", 0.9, "s1", &m);

   /* NULL workspace should fall back to regular assembly */
   char *ctx = memory_assemble_context_ws(NULL, NULL);
   assert(ctx != NULL);
   assert(strstr(ctx, "some content") != NULL);

   free(ctx);
   teardown();
}

static int count_key(const char *key)
{
   return count_for_key("SELECT COUNT(*) FROM memories WHERE key = ?1", key);
}

static double conf_for_key(const char *key)
{
   char err[128] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       db2_conn(), "SELECT confidence FROM memories WHERE key = ?1", err, sizeof(err));
   assert(st != NULL);
   aimee_pg_bind_text(st, "?1", key);
   double c = -1.0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      c = aimee_pg_column_double(st, 0);
   aimee_pg_finalize(st);
   return c;
}

static void test_upsert_workflow_inserts_and_tags(void)
{
   setup();

   int64_t id = memory_upsert_workflow("SmoothNAS", "pr-target", "PRs target the `testing` branch.",
                                       0.6, "s1");
   assert(id > 0);

   /* Single row keyed by workflow:smoothnas:pr-target (key is normalized
    * lowercase by the memory layer). */
   assert(count_key("workflow:smoothnas:pr-target") == 1);

   /* Confidence starts around the observed value (0.6). */
   double c = conf_for_key("workflow:smoothnas:pr-target");
   assert(c >= 0.59 && c <= 0.61);

   /* Tagged to the requested workspace. */
   assert(count_for_memory("SELECT COUNT(*) FROM memory_workspaces WHERE memory_id = ?1"
                           " AND workspace = 'SmoothNAS'",
                           id) == 1);

   /* Row kind is `workflow`. */
   {
      char err[128] = "";
      aimee_pg_stmt_t *st =
          aimee_pg_prepare(db2_conn(), "SELECT kind FROM memories WHERE id = ?1", err, sizeof(err));
      assert(st != NULL);
      aimee_pg_bind_int64(st, "?1", id);
      assert(aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW);
      const char *kind = aimee_pg_column_text(st, 0);
      assert(kind && strcmp(kind, KIND_WORKFLOW) == 0);
      aimee_pg_finalize(st);
   }

   teardown();
}

static void test_upsert_workflow_dedupes_and_bumps(void)
{
   setup();

   int64_t first = memory_upsert_workflow("SmoothNAS", "pr-target",
                                          "PRs target the `testing` branch.", 0.6, "s1");
   assert(first > 0);

   /* Second observation with the same signal: must update, not duplicate. */
   int64_t second = memory_upsert_workflow("SmoothNAS", "pr-target",
                                           "PRs target the `testing` branch.", 0.6, "s2");
   assert(second == first);
   assert(count_key("workflow:smoothnas:pr-target") == 1);

   /* Confidence bumps toward the durable model-authored provenance ceiling. */
   double c2 = conf_for_key("workflow:smoothnas:pr-target");
   assert(c2 >= 0.79 && c2 <= 0.81);

   memory_upsert_workflow("SmoothNAS", "pr-target", "PRs target the `testing` branch.", 0.6, "s3");
   double c3 = conf_for_key("workflow:smoothnas:pr-target");
   assert(c3 >= 0.79 && c3 <= 0.81);

   /* Re-exposure cannot lift belief above the provenance ceiling. */
   memory_upsert_workflow("SmoothNAS", "pr-target", "PRs target the `testing` branch.", 0.6, "s4");
   double c4 = conf_for_key("workflow:smoothnas:pr-target");
   assert(c4 >= 0.79 && c4 <= 0.81);

   teardown();
}

static void test_upsert_workflow_rejects_empty_args(void)
{
   setup();
   assert(memory_upsert_workflow("", "pr-target", "rule", 0.6, "s1") == -1);
   assert(memory_upsert_workflow("ws", "", "rule", 0.6, "s1") == -1);
   assert(memory_upsert_workflow("ws", "pr-target", "", 0.6, "s1") == -1);
   teardown();
}

static void test_auto_tag_shared_keywords(void)
{
   setup();
   memory_t m;

   /* This memory mentions "auth" — should be auto-tagged _shared */
   memory_insert(TIER_L2, KIND_FACT, "auth-config", "PostgreSQL cert auth flow", 0.9, "s1", &m);

   /* auto_tag was called during insert, check for _shared tag */
   char ws_err[128] = "";
   aimee_pg_stmt_t *ws_st = aimee_pg_prepare(
       db2_conn(), "SELECT COUNT(*) FROM memory_workspaces WHERE memory_id = ?1 AND workspace = ?2",
       ws_err, sizeof(ws_err));
   assert(ws_st != NULL);
   aimee_pg_bind_int64(ws_st, "?1", m.id);
   aimee_pg_bind_text(ws_st, "?2", SHARED_WORKSPACE);
   assert(aimee_pg_step(ws_st, ws_err, sizeof(ws_err)) == AIMEE_PG_ROW);
   int count = aimee_pg_column_int(ws_st, 0);
   aimee_pg_finalize(ws_st);

   /* Should have _shared tag because "auth" is a shared keyword */
   assert(count == 1);

   teardown();
}

static void test_scoped_retrieval_filters_results(void)
{
   setup();
   memory_t user_mem, agent_mem;
   memory_insert(TIER_L2, KIND_FACT, "summary-style", "Alice prefers terse summaries", 0.9, "s1",
                 &user_mem);
   memory_insert(TIER_L2, KIND_FACT, "summary-style-agent",
                 "Reviewer agent prefers exhaustive summaries", 0.9, "s2", &agent_mem);

   assert(memory_tag_scope(user_mem.id, "user", "alice") == 0);
   assert(memory_tag_scope(agent_mem.id, "agent", "reviewer") == 0);

   memory_t results[8];
   int count = memory_find_facts_scoped("prefers summaries", "user", "alice", 5, results, 8);
   assert(count >= 1);
   assert(results[0].id == user_mem.id);

   count = memory_find_facts_scoped("exhaustive summaries", "agent", "reviewer", 5, results, 8);
   assert(count >= 1);
   assert(results[0].id == agent_mem.id);

   teardown();
}

static void test_visible_retrieval_prefers_narrower_scope(void)
{
   setup();
   memory_t global_mem, workspace_mem, project_mem;
   memory_insert(TIER_L2, KIND_FACT, "deploy-target-global", "Deploy target is shared infra", 0.9,
                 "s1", &global_mem);
   memory_insert(TIER_L2, KIND_FACT, "deploy-target-ws", "Deploy target is workspace cluster", 0.9,
                 "s2", &workspace_mem);
   memory_insert(TIER_L2, KIND_FACT, "deploy-target-project", "Deploy target is project sandbox",
                 0.9, "s3", &project_mem);

   assert(memory_tag_global(global_mem.id) == 0);
   assert(memory_tag_workspace(workspace_mem.id, "wol") == 0);
   assert(memory_tag_project(project_mem.id, "aimee") == 0);

   memory_t results[8];
   int count = memory_find_facts_visible("deploy target", "wol", "aimee", 5, results, 8);
   assert(count == 3);
   assert(results[0].id == project_mem.id);
   assert(results[1].id == workspace_mem.id);
   assert(results[2].id == global_mem.id);

   teardown();
}

static void test_local_first_applies_before_limits_across_memory_surfaces(void)
{
   setup();
   memory_t local, workspace_mem, global, other;
   memory_insert(TIER_L2, KIND_FACT, "identity:local-crowdout",
                 "crowdout routing needle belongs to the active project", 0.10, "local-session",
                 &local);
   assert(memory_tag_project(local.id, "active-project") == 0);
   assert(db2_memory_episode_insert(local.id, "local-crowdout-episode",
                                    "crowdout episode from active project", "local-session",
                                    "2026-07-29") > 0);
   insert_memory_entity(local.id, "LocalCrowdEntity");
   db2_memory_relation_upsert_full(local.id, 0, "CrowdEntity", "owned_by", "ActiveProject",
                                   "crowdout relation from active project", "", "", 0.10);

   memory_insert(TIER_L2, KIND_FACT, "identity:workspace-crowdout",
                 "crowdout routing needle belongs to the active workspace", 0.20,
                 "workspace-session", &workspace_mem);
   /* An explicit ownership change stamps both the compatibility projection
    * and the memory row used by authorization. */
   assert(memory_tag_workspace(workspace_mem.id, "active-workspace") == 0);

   /* Both buckets exceed every one-row request below. Their much higher
    * relevance/confidence and later insertion order reproduce the old failure:
    * a global LIMIT first would permanently discard the active-project row. */
   for (int i = 0; i < 12; i++)
   {
      char key[64];
      char content[160];
      snprintf(key, sizeof(key), "identity:global-crowdout-%02d", i);
      snprintf(content, sizeof(content), "crowdout routing needle global distractor %02d", i);
      memory_insert(TIER_L2, KIND_FACT, key, content, 0.99, "global-session", &global);
      assert(memory_tag_global(global.id) == 0);
      insert_memory_entity(global.id, "GlobalCrowdEntity");
      db2_memory_relation_upsert_full(global.id, 0, "CrowdEntity", "owned_by", "Global",
                                      "crowdout relation global distractor", "", "", 0.99);

      snprintf(key, sizeof(key), "identity:other-crowdout-%02d", i);
      snprintf(content, sizeof(content), "crowdout routing needle other project distractor %02d",
               i);
      memory_insert(TIER_L2, KIND_FACT, key, content, 0.99, "other-session", &other);
      assert(memory_tag_project(other.id, "other-project") == 0);
      insert_memory_entity(other.id, "OtherCrowdEntity");
      db2_memory_relation_upsert_full(other.id, 0, "CrowdEntity", "owned_by", "OtherProject",
                                      "crowdout relation other project distractor", "", "", 0.99);
   }
   /* A newer global episode in the same session must not become that
    * session's representative ahead of the older active-project episode. */
   assert(db2_memory_episode_insert(global.id, "global-same-session-episode",
                                    "crowdout newer global episode in local session",
                                    "local-session", "2026-07-30") > 0);

   memory_t facts[64];
   db2_memory_scope_context_set("active-workspace", "active-project", 0);
   assert(db2_memory_scope_context_rank(local.id) == 3);
   assert(db2_memory_scope_context_rank(workspace_mem.id) == 2);
   int direct_count = db2_memory_find_facts_like("crowdout routing needle", 2, facts, 64);
   assert(direct_count == 2);
   assert(facts[0].id == local.id);
   assert(facts[1].id == workspace_mem.id);
   db2_memory_scope_context_clear();
   int count = memory_find_facts_visible_ex("crowdout routing needle", "active-workspace",
                                            "active-project", 0, 1, facts, 64);
   assert(count == 1);
   assert(facts[0].id == local.id);

   /* Explicit all preserves the same bucket order but makes other projects
    * visible at the tail. */
   count = memory_find_facts_visible_ex("crowdout routing needle", "active-workspace",
                                        "active-project", 1, 64, facts, 64);
   assert(count > 1);
   assert(facts[0].id == local.id);
   assert(facts[1].id == workspace_mem.id);
   int saw_other = 0;
   for (int i = 0; i < count; i++)
      if (facts[i].id == other.id)
         saw_other = 1;
   assert(saw_other);

   /* With no active identity, only shared/global memory is returned. */
   count = memory_find_facts_visible_ex("crowdout routing needle", NULL, NULL, 0, 64, facts, 64);
   assert(count > 0);
   for (int i = 0; i < count; i++)
   {
      assert(facts[i].id != local.id);
      assert(facts[i].id != workspace_mem.id);
      assert(memory_scope_visibility_rank(facts[i].id, NULL, NULL) == 1);
   }

   db2_memory_scope_context_set("active-workspace", "active-project", 0);

   /* Ordered SQL readers used by list, context/recall, briefing, episodes,
    * graph, entity, and answer evidence all apply scope before LIMIT. */
   count = db2_memory_list(TIER_L2, KIND_FACT, 1, 1, facts, 64);
   assert(count == 1 && facts[0].id == local.id);

   count = db2_memory_top_l2_facts(facts, 1);
   assert(count == 1 && facts[0].id == local.id);

   count = db2_memory_list_session_scope_priority(facts, 1);
   assert(count == 1 && facts[0].id == local.id);

   count = db2_memory_list_session_scope_priority_like("%crowdout routing needle%", facts, 2);
   assert(count == 2);
   assert(facts[0].id == local.id);
   assert(facts[1].id == workspace_mem.id);

   memory_diagnostic_t diagnostics[2];
   count = memory_diagnose("crowdout routing needle", 1, diagnostics, 2);
   assert(count == 1 && diagnostics[0].memory.id == local.id);

   db2_memory_cand_row_t candidates[2];
   count = db2_memory_list_candidates(DB2_MEM_CAND_PRIMARY, candidates, 1);
   assert(count == 1 && candidates[0].id == local.id);

   db2_memory_cand_row_t recall[2];
   count = db2_memory_list_recall_section(DB2_MEM_RECALL_IDENTITY, recall, 1);
   assert(count == 1 && recall[0].id == local.id);

   db2_memory_briefing_fact_t briefing[2];
   count = db2_memory_briefing_list_key_facts(briefing, 1);
   assert(count == 1 && briefing[0].memory_id == local.id);

   db2_memory_briefing_activity_t activity[2];
   count = db2_memory_briefing_list_recent_activity(activity, 1);
   assert(count == 1 && strcmp(activity[0].session_id, "local-session") == 0);
   assert(strstr(activity[0].summary, "active project") != NULL);

   db2_memory_briefing_entity_t entities[2];
   count = db2_memory_briefing_list_active_entities(entities, 1);
   assert(count == 1);
   assert(strcmp(entities[0].name, "GlobalCrowdEntity") != 0);
   assert(strcmp(entities[0].name, "OtherCrowdEntity") != 0);

   memory_episode_t episodes[2];
   count = db2_memory_episodes_search("crowdout", 1, episodes, 2);
   assert(count == 1 && episodes[0].memory_id == local.id);

   memory_relation_t relations[2];
   count = db2_memory_relations_search("CrowdEntity", 1, relations, 2);
   assert(count == 1 && relations[0].memory_id == local.id);
   count = db2_memory_relations_for_entity("CrowdEntity", 1, relations, 2);
   assert(count == 1 && relations[0].memory_id == local.id);
   count = db2_memory_relations_search_as_of("CrowdEntity", "2026-07-29", 1, relations, 2);
   assert(count == 1 && relations[0].memory_id == local.id);
   count = db2_memory_relations_supporting("CrowdEntity", 1, relations, 2);
   assert(count == 1 && relations[0].memory_id == local.id);

   char *ctx = memory_assemble_context(NULL);
   assert(ctx != NULL);
   assert(strstr(ctx, "belongs to the active project") != NULL);
   assert(strstr(ctx, "other project distractor") == NULL);
   free(ctx);

   assert(db2_memory_lifecycle_update_state(local.id, "superseded", NULL) == 0);
   assert(db2_memory_lifecycle_update_state(workspace_mem.id, "superseded", NULL) == 0);
   assert(db2_memory_lifecycle_update_state(global.id, "superseded", NULL) == 0);
   assert(db2_memory_lifecycle_update_state(other.id, "superseded", NULL) == 0);
   db2_memory_lifecycle_superseded_t superseded[2];
   count = db2_memory_lifecycle_list_newly_superseded(NULL, superseded, 1);
   assert(count == 1 && superseded[0].memory_id == local.id);

   teardown();
}

static void test_collect_scopes_defaults_legacy_rows_to_global(void)
{
   setup();
   memory_t mem;
   memory_insert(TIER_L2, KIND_FACT, "legacy-scope", "legacy content", 0.9, "s1", &mem);

   memory_scope_tag_t scopes[4];
   int count = memory_collect_scopes(mem.id, scopes, 4);
   assert(count == 1);
   assert(strcmp(scopes[0].type, "global") == 0);
   assert(strcmp(scopes[0].value, "_global") == 0);

   char primary_value[128];
   memory_scope_level_t level = memory_primary_scope(mem.id, primary_value, sizeof(primary_value));
   assert(level == MEMORY_SCOPE_GLOBAL);
   assert(strcmp(primary_value, "_global") == 0);

   teardown();
}

static void test_ws_context_prefers_project_scope_when_available(void)
{
   setup();
   memory_t global_mem, workspace_mem, project_mem;
   char cwd[MAX_PATH_LEN];
   char project_root[MAX_PATH_LEN];
   assert(getcwd(cwd, sizeof(cwd)) != NULL);
   assert(workspace_active_root_from_cwd(cwd, project_root, sizeof(project_root)) == 0);
   /* Project scope keys on the repo identity (canonical remote) with a
    * basename fallback, matching memory_scope_labels_for_cwd() in production. */
   char project_buf[MAX_PATH_LEN];
   const char *project_name;
   if (workspace_repo_identity(cwd, project_buf, sizeof(project_buf), NULL, 0) == 0 &&
       project_buf[0])
   {
      project_name = project_buf;
   }
   else
   {
      const char *slash = strrchr(project_root, '/');
      project_name = slash ? slash + 1 : project_root;
   }

   memory_insert(TIER_L2, KIND_FACT, "scope-order-global", "scope order global", 0.9, "s1",
                 &global_mem);
   memory_insert(TIER_L2, KIND_FACT, "scope-order-workspace", "scope order workspace", 0.9, "s2",
                 &workspace_mem);
   memory_insert(TIER_L2, KIND_FACT, "scope-order-project", "scope order project", 0.9, "s3",
                 &project_mem);

   assert(memory_tag_global(global_mem.id) == 0);
   assert(memory_tag_workspace(workspace_mem.id, "wol") == 0);
   assert(memory_tag_project(project_mem.id, project_name) == 0);

   char *ctx = memory_assemble_context_ws(NULL, "wol");
   assert(ctx != NULL);

   char *project_ref = strstr(ctx, "scope order project");
   char *workspace_ref = strstr(ctx, "scope order workspace");
   char *global_ref = strstr(ctx, "scope order global");
   assert(project_ref != NULL);
   assert(workspace_ref != NULL);
   assert(global_ref != NULL);
   assert(project_ref < workspace_ref);
   assert(workspace_ref < global_ref);

   free(ctx);
   teardown();
}

static cJSON *find_scope_entry(cJSON *scopes, const char *scope)
{
   int n = cJSON_GetArraySize(scopes);
   for (int i = 0; i < n; i++)
   {
      cJSON *entry = cJSON_GetArrayItem(scopes, i);
      cJSON *name = cJSON_GetObjectItem(entry, "scope");
      if (cJSON_IsString(name) && strcmp(name->valuestring, scope) == 0)
         return entry;
   }
   return NULL;
}

static cJSON *find_tier_entry(cJSON *tiers, const char *tier)
{
   int n = cJSON_GetArraySize(tiers);
   for (int i = 0; i < n; i++)
   {
      cJSON *entry = cJSON_GetArrayItem(tiers, i);
      cJSON *name = cJSON_GetObjectItem(entry, "tier");
      if (cJSON_IsString(name) && strcmp(name->valuestring, tier) == 0)
         return entry;
   }
   return NULL;
}

static void test_api_memory_stats_includes_scope_counts(void)
{
   setup();
   memory_t global_mem, workspace_mem, project_mem;
   char cwd[MAX_PATH_LEN];
   char project_root[MAX_PATH_LEN];
   assert(getcwd(cwd, sizeof(cwd)) != NULL);
   assert(workspace_active_root_from_cwd(cwd, project_root, sizeof(project_root)) == 0);
   const char *slash = strrchr(project_root, '/');
   const char *project_name = slash ? slash + 1 : project_root;

   memory_insert(TIER_L2, KIND_FACT, "stats-global", "stats global", 0.9, "s1", &global_mem);
   memory_insert(TIER_L2, KIND_FACT, "stats-workspace", "stats workspace", 0.9, "s2",
                 &workspace_mem);
   memory_insert(TIER_L2, KIND_FACT, "stats-project", "stats project", 0.9, "s3", &project_mem);

   assert(memory_tag_global(global_mem.id) == 0);
   assert(memory_tag_workspace(workspace_mem.id, "wol") == 0);
   assert(memory_tag_project(project_mem.id, project_name) == 0);

   char conf_err[128] = "";
   aimee_pg_stmt_t *conf_st =
       aimee_pg_prepare(db2_conn(),
                        "INSERT INTO memory_conflicts (memory_a, memory_b, detected_at, resolved)"
                        " VALUES (?1, ?2, pg_now_text(), 0)",
                        conf_err, sizeof(conf_err));
   assert(conf_st != NULL);
   aimee_pg_bind_int64(conf_st, "?1", workspace_mem.id);
   aimee_pg_bind_int64(conf_st, "?2", project_mem.id);
   assert(aimee_pg_step(conf_st, conf_err, sizeof(conf_err)) == AIMEE_PG_DONE);
   aimee_pg_finalize(conf_st);

   char *json = api_memory_stats();
   assert(json != NULL);

   cJSON *root = cJSON_Parse(json);
   assert(cJSON_IsObject(root));
   cJSON *tiers = cJSON_GetObjectItem(root, "tiers");
   cJSON *scopes = cJSON_GetObjectItem(root, "scopes");
   assert(cJSON_IsArray(tiers));
   assert(cJSON_IsArray(scopes));

   cJSON *global_entry = find_scope_entry(scopes, "global");
   cJSON *workspace_entry = find_scope_entry(scopes, "workspace");
   cJSON *project_entry = find_scope_entry(scopes, "project");
   assert(cJSON_GetObjectItem(global_entry, "count")->valueint == 1);
   assert(cJSON_GetObjectItem(workspace_entry, "count")->valueint == 1);
   assert(cJSON_GetObjectItem(project_entry, "count")->valueint == 1);
   assert(cJSON_GetObjectItem(workspace_entry, "conflicted_memories")->valueint == 1);
   assert(cJSON_GetObjectItem(project_entry, "conflicted_memories")->valueint == 1);

   cJSON_Delete(root);
   free(json);
   teardown();
}

static void test_api_memory_stats_includes_functional_tiers(void)
{
   setup();
   memory_t l4_mem, l5_mem;

   assert(memory_insert(TIER_L4, KIND_POLICY, "naming-rule", "Prefer snake_case", 0.95, "s1",
                        &l4_mem) == 0);
   assert(memory_insert(TIER_L5, KIND_FACT, "cross-repo-pattern",
                        "Across repos, releases happen after staging validation.", 0.92, "s2",
                        &l5_mem) == 0);
   assert(memory_tag_global(l4_mem.id) == 0);
   assert(memory_tag_global(l5_mem.id) == 0);

   char *json = api_memory_stats();
   assert(json != NULL);

   cJSON *root = cJSON_Parse(json);
   assert(cJSON_IsObject(root));
   cJSON *tiers = cJSON_GetObjectItem(root, "tiers");
   cJSON *tier_kinds = cJSON_GetObjectItem(root, "tier_kinds");
   assert(cJSON_IsArray(tiers));
   assert(cJSON_IsArray(tier_kinds));

   cJSON *l4_entry = find_tier_entry(tiers, "L4");
   cJSON *l5_entry = find_tier_entry(tiers, "L5");
   assert(l4_entry != NULL);
   assert(l5_entry != NULL);
   assert(cJSON_GetObjectItem(l4_entry, "count")->valueint == 1);
   assert(cJSON_GetObjectItem(l5_entry, "count")->valueint == 1);
   assert(strcmp(cJSON_GetObjectItem(l4_entry, "functional_name")->valuestring, TIER_L4_NAME) == 0);
   assert(strcmp(cJSON_GetObjectItem(l5_entry, "functional_name")->valuestring, TIER_L5_NAME) == 0);

   cJSON_Delete(root);
   free(json);
   teardown();
}

int main(void)
{
   test_tag_workspace();
   test_tag_generic_scope();
   test_tag_multiple_workspaces();
   test_tag_idempotent();
   test_cascade_delete();
   test_ws_context_scoped_memories_first();
   test_ws_untagged_treated_as_shared();
   test_ws_cross_workspace_high_confidence();
   test_ws_null_workspace_falls_back();
   test_auto_tag_shared_keywords();
   test_scoped_retrieval_filters_results();
   test_visible_retrieval_prefers_narrower_scope();
   test_local_first_applies_before_limits_across_memory_surfaces();
   test_collect_scopes_defaults_legacy_rows_to_global();
   test_ws_context_prefers_project_scope_when_available();
   test_api_memory_stats_includes_scope_counts();
   test_api_memory_stats_includes_functional_tiers();
   test_upsert_workflow_inserts_and_tags();
   test_upsert_workflow_dedupes_and_bumps();
   test_upsert_workflow_rejects_empty_args();
   printf("workspace_memory: all tests passed\n");
   return 0;
}
