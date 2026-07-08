/* db1/ensemble.c: JSON-backed templated multi-agent ensembles. */
#include "ensemble.h"
#include "db1_internal.h"
#include "config.h"
#include "dstr.h"
#include "util.h"

#include <sqlite3.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Module-private prepare helper: wraps sqlite3_prepare_v2 against the
 * private db1 connection, returning NULL on any failure. */
static sqlite3_stmt *prep_stmt(const char *sql)
{
   sqlite3 *db = db1_conn();
   if (!db)
      return NULL;
   sqlite3_stmt *s = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK)
      return NULL;
   return s;
}

typedef struct
{
   cJSON *template_json;
   cJSON *context_json;
   ensemble_info_t info;
} wf_loaded_session_t;

typedef struct
{
   const char *name;
   const char *json;
} wf_builtin_template_t;

static const wf_builtin_template_t g_builtin_templates[] = {
    {"code-review", "{"
                    "\"name\":\"code-review\","
                    "\"description\":\"Structured code review with independent reviewers\","
                    "\"phases\":["
                    "{\"name\":\"initial-review\",\"participants\":[{\"role\":\"reviewer\"},{"
                    "\"role\":\"reviewer\"}]},"
                    "{\"name\":\"rebuttal\",\"participants\":[{\"role\":\"author\"}]},"
                    "{\"name\":\"final-verdict\",\"participants\":[{\"role\":\"reviewer\"},{"
                    "\"role\":\"reviewer\"}]}"
                    "]"
                    "}"},
    {"debate",
     "{"
     "\"name\":\"debate\","
     "\"description\":\"Adversarial analysis with alternating turns\","
     "\"phases\":["
     "{\"name\":\"opening\",\"participants\":[{\"role\":\"for\"},{\"role\":\"against\"}]},"
     "{\"name\":\"cross-examination\",\"participants\":[{\"role\":\"for\"},{\"role\":\"against\"}]}"
     ","
     "{\"name\":\"closing\",\"participants\":[{\"role\":\"for\"},{\"role\":\"against\"}]}"
     "]"
     "}"},
    {"planning", "{"
                 "\"name\":\"planning\","
                 "\"description\":\"Collaborative planning with critique and synthesis\","
                 "\"phases\":["
                 "{\"name\":\"brainstorm\",\"participants\":[{\"role\":\"planner\"}]},"
                 "{\"name\":\"critique\",\"participants\":[{\"role\":\"critic\"}]},"
                 "{\"name\":\"synthesis\",\"participants\":[{\"role\":\"planner\"}]}"
                 "]"
                 "}"},
    {"design-critique",
     "{"
     "\"name\":\"design-critique\","
     "\"description\":\"Design review with presentation, feedback, and revision\","
     "\"phases\":["
     "{\"name\":\"presentation\",\"participants\":[{\"role\":\"author\"}]},"
     "{\"name\":\"feedback\",\"participants\":[{\"role\":\"critic\"}]},"
     "{\"name\":\"revision\",\"participants\":[{\"role\":\"author\"}]}"
     "]"
     "}"},
    {NULL, NULL}};

static void wf_set_err(char *err, size_t errlen, const char *msg)
{
   if (err && errlen > 0)
      snprintf(err, errlen, "%s", msg ? msg : "ensemble error");
}

static cJSON *wf_get_phases(cJSON *root)
{
   cJSON *phases = cJSON_GetObjectItemCaseSensitive(root, "phases");
   return cJSON_IsArray(phases) ? phases : NULL;
}

static cJSON *wf_get_phase(cJSON *root, int phase_idx)
{
   cJSON *phases = wf_get_phases(root);
   if (!phases || phase_idx < 0)
      return NULL;
   return cJSON_GetArrayItem(phases, phase_idx);
}

static cJSON *wf_get_participants(cJSON *phase)
{
   cJSON *participants = cJSON_GetObjectItemCaseSensitive(phase, "participants");
   return cJSON_IsArray(participants) ? participants : NULL;
}

static cJSON *wf_get_participant(cJSON *root, int phase_idx, int turn_idx)
{
   cJSON *phase = wf_get_phase(root, phase_idx);
   cJSON *participants = wf_get_participants(phase);
   if (!participants || turn_idx < 0)
      return NULL;
   return cJSON_GetArrayItem(participants, turn_idx);
}

static const char *wf_json_string(cJSON *obj, const char *key)
{
   cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
   return cJSON_IsString(item) ? item->valuestring : NULL;
}

static int wf_participant_count(cJSON *root, int phase_idx)
{
   cJSON *phase = wf_get_phase(root, phase_idx);
   cJSON *participants = wf_get_participants(phase);
   return participants ? cJSON_GetArraySize(participants) : 0;
}

static int wf_phase_count(cJSON *root)
{
   cJSON *phases = wf_get_phases(root);
   return phases ? cJSON_GetArraySize(phases) : 0;
}

static int wf_find_assignment_slot(cJSON *state, const char *role)
{
   if (!state || !role)
      return 0;
   cJSON *item = cJSON_GetObjectItemCaseSensitive(state, role);
   if (!cJSON_IsNumber(item))
      return 0;
   return item->valueint;
}

static void wf_set_assignment_slot(cJSON *state, const char *role, int value)
{
   cJSON *item = cJSON_GetObjectItemCaseSensitive(state, role);
   if (item)
      cJSON_SetIntValue(item, value);
   else
      cJSON_AddNumberToObject(state, role, value);
}

static int wf_is_assigned_agent(cJSON *tmpl, const char *name)
{
   int phases = wf_phase_count(tmpl);
   for (int i = 0; i < phases; i++)
   {
      int turns = wf_participant_count(tmpl, i);
      for (int j = 0; j < turns; j++)
      {
         cJSON *p = wf_get_participant(tmpl, i, j);
         const char *agent = wf_json_string(p, "agent");
         if (agent && strcmp(agent, name) == 0)
            return 1;
      }
   }
   return 0;
}

static int wf_expand_assignments(cJSON *tmpl, cJSON *assignments, char *err, size_t errlen)
{
   cJSON *slots = cJSON_CreateObject();
   if (!slots)
   {
      wf_set_err(err, errlen, "out of memory");
      return -1;
   }

   int phases = wf_phase_count(tmpl);
   for (int i = 0; i < phases; i++)
   {
      int turns = wf_participant_count(tmpl, i);
      for (int j = 0; j < turns; j++)
      {
         cJSON *p = wf_get_participant(tmpl, i, j);
         const char *role = wf_json_string(p, "role");
         if (!role || !role[0])
         {
            cJSON_Delete(slots);
            wf_set_err(err, errlen, "template participant is missing role");
            return -1;
         }

         cJSON *arr = cJSON_GetObjectItemCaseSensitive(assignments, role);
         if (!cJSON_IsArray(arr) || cJSON_GetArraySize(arr) == 0)
         {
            cJSON_Delete(slots);
            snprintf(err, errlen, "missing --assign for role '%s'", role);
            return -1;
         }

         int slot = wf_find_assignment_slot(slots, role);
         cJSON *agent_item = cJSON_GetArrayItem(arr, slot % cJSON_GetArraySize(arr));
         if (!cJSON_IsString(agent_item) || !agent_item->valuestring[0])
         {
            cJSON_Delete(slots);
            snprintf(err, errlen, "invalid assignment for role '%s'", role);
            return -1;
         }

         cJSON_DeleteItemFromObjectCaseSensitive(p, "agent");
         cJSON_AddStringToObject(p, "agent", agent_item->valuestring);
         wf_set_assignment_slot(slots, role, slot + 1);
      }
   }

   cJSON_Delete(slots);
   return 0;
}

static char *wf_context_excerpt(cJSON *context)
{
   dstr_t out;
   dstr_init(&out);

   int count = cJSON_IsArray(context) ? cJSON_GetArraySize(context) : 0;
   int start = (count > 4) ? (count - 4) : 0;

   for (int i = start; i < count; i++)
   {
      cJSON *msg = cJSON_GetArrayItem(context, i);
      const char *sender = wf_json_string(msg, "sender");
      const char *text = wf_json_string(msg, "text");
      if (!sender || !text || !text[0])
         continue;
      dstr_appendf(&out, "- %s: %s\n", sender, text);
   }

   return dstr_steal(&out);
}

static char *wf_build_prompt(cJSON *tmpl, cJSON *context, int phase_idx, int turn_idx)
{
   cJSON *phase = wf_get_phase(tmpl, phase_idx);
   cJSON *participant = wf_get_participant(tmpl, phase_idx, turn_idx);
   const char *phase_name = wf_json_string(phase, "name");
   const char *agent = wf_json_string(participant, "agent");
   const char *role = wf_json_string(participant, "role");
   char *excerpt = wf_context_excerpt(context);

   dstr_t out;
   dstr_init(&out);
   dstr_appendf(&out, "Phase: %s\nAgent: %s\nRole: %s\n", phase_name ? phase_name : "?",
                agent ? agent : "?", role ? role : "?");
   if (db1_ensemble_role_needs_dissent(role))
   {
      dstr_append_str(&out,
                      "\nProvide your own independent analysis. Do not repeat or defer to "
                      "previous reviewers. If you agree on a point, acknowledge it briefly and "
                      "focus on what others missed or where you disagree.\n");
   }
   if (excerpt && excerpt[0])
   {
      dstr_append_str(&out, "\nRecent context:\n");
      dstr_append_str(&out, excerpt);
   }

   free(excerpt);
   return dstr_steal(&out);
}

static int wf_fill_info_from_template(ensemble_info_t *out, cJSON *tmpl)
{
   if (!out || !tmpl)
      return -1;

   out->phase_count = wf_phase_count(tmpl);
   if (strcmp(out->status, "complete") == 0)
   {
      out->turns_in_phase = 0;
      out->phase_name[0] = '\0';
      out->expected_agent[0] = '\0';
      out->expected_role[0] = '\0';
      return 0;
   }

   cJSON *phase = wf_get_phase(tmpl, out->current_phase);
   cJSON *participant = wf_get_participant(tmpl, out->current_phase, out->current_turn);
   out->turns_in_phase = wf_participant_count(tmpl, out->current_phase);
   snprintf(out->phase_name, sizeof(out->phase_name), "%s", wf_json_string(phase, "name") ?: "");
   snprintf(out->expected_agent, sizeof(out->expected_agent), "%s",
            wf_json_string(participant, "agent") ?: "");
   snprintf(out->expected_role, sizeof(out->expected_role), "%s",
            wf_json_string(participant, "role") ?: "");
   return 0;
}

static int wf_load_session_row(int id, wf_loaded_session_t *loaded, char *err, size_t errlen)
{
   memset(loaded, 0, sizeof(*loaded));

   const char *sql = "SELECT id, template_name, channel, status, current_phase, current_turn, "
                     "expected_agent, expected_role, paused_reason, template_json, context_json, "
                     "created_at, updated_at FROM ensembles WHERE id = ?";
   sqlite3_stmt *stmt = prep_stmt(sql);
   if (!stmt)
   {
      wf_set_err(err, errlen, sqlite3_errmsg(db1_conn()));
      return -1;
   }

   sqlite3_bind_int(stmt, 1, id);
   if (sqlite3_step(stmt) != SQLITE_ROW)
   {
      sqlite3_finalize(stmt);
      snprintf(err, errlen, "ensemble %d not found", id);
      return -1;
   }

   snprintf(loaded->info.template_name, sizeof(loaded->info.template_name), "%s",
            (const char *)sqlite3_column_text(stmt, 1) ? (const char *)sqlite3_column_text(stmt, 1)
                                                       : "");
   snprintf(loaded->info.channel, sizeof(loaded->info.channel), "%s",
            (const char *)sqlite3_column_text(stmt, 2) ? (const char *)sqlite3_column_text(stmt, 2)
                                                       : "");
   snprintf(loaded->info.status, sizeof(loaded->info.status), "%s",
            (const char *)sqlite3_column_text(stmt, 3) ? (const char *)sqlite3_column_text(stmt, 3)
                                                       : "");
   loaded->info.id = sqlite3_column_int(stmt, 0);
   loaded->info.current_phase = sqlite3_column_int(stmt, 4);
   loaded->info.current_turn = sqlite3_column_int(stmt, 5);
   snprintf(loaded->info.expected_agent, sizeof(loaded->info.expected_agent), "%s",
            (const char *)sqlite3_column_text(stmt, 6) ? (const char *)sqlite3_column_text(stmt, 6)
                                                       : "");
   snprintf(loaded->info.expected_role, sizeof(loaded->info.expected_role), "%s",
            (const char *)sqlite3_column_text(stmt, 7) ? (const char *)sqlite3_column_text(stmt, 7)
                                                       : "");
   snprintf(loaded->info.paused_reason, sizeof(loaded->info.paused_reason), "%s",
            (const char *)sqlite3_column_text(stmt, 8) ? (const char *)sqlite3_column_text(stmt, 8)
                                                       : "");
   snprintf(loaded->info.created_at, sizeof(loaded->info.created_at), "%s",
            (const char *)sqlite3_column_text(stmt, 11)
                ? (const char *)sqlite3_column_text(stmt, 11)
                : "");
   snprintf(loaded->info.updated_at, sizeof(loaded->info.updated_at), "%s",
            (const char *)sqlite3_column_text(stmt, 12)
                ? (const char *)sqlite3_column_text(stmt, 12)
                : "");

   const char *tmpl_raw = (const char *)sqlite3_column_text(stmt, 9)
                              ? (const char *)sqlite3_column_text(stmt, 9)
                              : "{}";
   const char *ctx_raw = (const char *)sqlite3_column_text(stmt, 10)
                             ? (const char *)sqlite3_column_text(stmt, 10)
                             : "[]";
   loaded->template_json = cJSON_Parse(tmpl_raw);
   loaded->context_json = cJSON_Parse(ctx_raw);
   sqlite3_finalize(stmt);

   if (!cJSON_IsObject(loaded->template_json) || !cJSON_IsArray(loaded->context_json))
   {
      cJSON_Delete(loaded->template_json);
      cJSON_Delete(loaded->context_json);
      loaded->template_json = NULL;
      loaded->context_json = NULL;
      wf_set_err(err, errlen, "ensemble stored invalid JSON");
      return -1;
   }

   wf_fill_info_from_template(&loaded->info, loaded->template_json);
   return 0;
}

static void wf_loaded_session_free(wf_loaded_session_t *loaded)
{
   if (!loaded)
      return;
   cJSON_Delete(loaded->template_json);
   cJSON_Delete(loaded->context_json);
   loaded->template_json = NULL;
   loaded->context_json = NULL;
}

static int wf_store_session(const wf_loaded_session_t *loaded, char *err, size_t errlen)
{
   char *ctx_raw = cJSON_PrintUnformatted(loaded->context_json);
   if (!ctx_raw)
   {
      wf_set_err(err, errlen, "failed to serialize ensemble context");
      return -1;
   }

   const char *sql = "UPDATE ensembles "
                     "SET status = ?, current_phase = ?, current_turn = ?, expected_agent = ?, "
                     "expected_role = ?, paused_reason = ?, context_json = ?, "
                     "updated_at = datetime('now') WHERE id = ?";
   sqlite3_stmt *stmt = prep_stmt(sql);
   if (!stmt)
   {
      free(ctx_raw);
      wf_set_err(err, errlen, sqlite3_errmsg(db1_conn()));
      return -1;
   }

   sqlite3_bind_text(stmt, 1, loaded->info.status, -1, SQLITE_STATIC);
   sqlite3_bind_int(stmt, 2, loaded->info.current_phase);
   sqlite3_bind_int(stmt, 3, loaded->info.current_turn);
   sqlite3_bind_text(stmt, 4, loaded->info.expected_agent, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 5, loaded->info.expected_role, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 6, loaded->info.paused_reason, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 7, ctx_raw, -1, SQLITE_STATIC);
   sqlite3_bind_int(stmt, 8, loaded->info.id);

   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   free(ctx_raw);
   if (rc != SQLITE_DONE)
   {
      wf_set_err(err, errlen, sqlite3_errmsg(db1_conn()));
      return -1;
   }
   return 0;
}

static void wf_append_context(cJSON *context, const char *sender, const char *text, int phase_idx,
                              int turn_idx)
{
   cJSON *msg = cJSON_CreateObject();
   if (!msg)
      return;
   cJSON_AddStringToObject(msg, "sender", sender ? sender : "");
   cJSON_AddStringToObject(msg, "text", text ? text : "");
   cJSON_AddNumberToObject(msg, "phase", phase_idx);
   cJSON_AddNumberToObject(msg, "turn", turn_idx);
   cJSON_AddItemToArray(context, msg);
}

int db1_ensemble_template_path(const char *project_root, const char *name, char *buf, size_t bufsz)
{
   struct stat st;

   if (!name || !name[0] || !buf || bufsz == 0)
      return -1;

   /* Prefer ensemble_templates/; fall back to the legacy session_templates/ dir
    * so project-local templates authored before the rename keep resolving. */
   static const char *const dirs[] = {"ensemble_templates", "session_templates"};

   for (size_t d = 0; d < sizeof(dirs) / sizeof(dirs[0]); d++)
   {
      if (project_root && project_root[0])
      {
         snprintf(buf, bufsz, "%s/%s/%s.json", project_root, dirs[d], name);
         if (stat(buf, &st) == 0 && S_ISREG(st.st_mode))
            return 0;
      }

      snprintf(buf, bufsz, "%s/%s/%s.json", config_default_dir(), dirs[d], name);
      if (stat(buf, &st) == 0 && S_ISREG(st.st_mode))
         return 0;
   }

   buf[0] = '\0';
   return -1;
}

cJSON *db1_ensemble_template_load(const char *project_root, const char *name, char *err,
                                  size_t errlen)
{
   char path[MAX_PATH_LEN];
   dstr_t raw;
   dstr_init(&raw);
   cJSON *root = NULL;

   if (db1_ensemble_template_path(project_root, name, path, sizeof(path)) == 0)
   {
      if (dstr_read_file(&raw, path) != 0)
      {
         snprintf(err, errlen, "failed to read ensemble template '%s'", path);
         dstr_free(&raw);
         return NULL;
      }
      root = cJSON_Parse(dstr_cstr(&raw));
      dstr_free(&raw);
   }
   else
   {
      for (int i = 0; g_builtin_templates[i].name; i++)
      {
         if (strcmp(g_builtin_templates[i].name, name) == 0)
         {
            root = cJSON_Parse(g_builtin_templates[i].json);
            break;
         }
      }
      if (!root)
      {
         snprintf(err, errlen, "ensemble template '%s' not found", name ? name : "");
         return NULL;
      }
   }

   if (!cJSON_IsObject(root))
   {
      wf_set_err(err, errlen, "invalid ensemble template JSON");
      cJSON_Delete(root);
      return NULL;
   }

   cJSON *phases = wf_get_phases(root);
   if (!phases || cJSON_GetArraySize(phases) == 0)
   {
      wf_set_err(err, errlen, "ensemble template requires at least one phase");
      cJSON_Delete(root);
      return NULL;
   }

   return root;
}

int db1_ensemble_role_needs_dissent(const char *role)
{
   static const char *roles[] = {"reviewer", "red_team", "critic", "challenger", "against", NULL};
   if (!role)
      return 0;
   for (int i = 0; roles[i]; i++)
   {
      if (strcmp(role, roles[i]) == 0)
         return 1;
   }
   return 0;
}

int db1_ensemble_create(const char *project_root, const char *template_name, const char *channel,
                        cJSON *assignments, int *out_id, char *err, size_t errlen)
{
   cJSON *tmpl = db1_ensemble_template_load(project_root, template_name, err, errlen);
   if (!tmpl)
      return -1;

   if (wf_expand_assignments(tmpl, assignments, err, errlen) != 0)
   {
      cJSON_Delete(tmpl);
      return -1;
   }

   char *tmpl_raw = cJSON_PrintUnformatted(tmpl);
   char *assign_raw = cJSON_PrintUnformatted(assignments);
   if (!tmpl_raw || !assign_raw)
   {
      free(tmpl_raw);
      free(assign_raw);
      cJSON_Delete(tmpl);
      wf_set_err(err, errlen, "failed to serialize ensemble template");
      return -1;
   }

   cJSON *first = wf_get_participant(tmpl, 0, 0);
   const char *expected_agent = wf_json_string(first, "agent");
   const char *expected_role = wf_json_string(first, "role");

   const char *sql =
       "INSERT INTO ensembles (template_name, channel, status, current_phase, "
       "current_turn, expected_agent, expected_role, paused_reason, template_json, "
       "assignments_json, context_json, created_at, updated_at) "
       "VALUES (?, ?, 'active', 0, 0, ?, ?, '', ?, ?, '[]', datetime('now'), datetime('now'))";
   sqlite3_stmt *stmt = prep_stmt(sql);
   if (!stmt)
   {
      free(tmpl_raw);
      free(assign_raw);
      cJSON_Delete(tmpl);
      wf_set_err(err, errlen, sqlite3_errmsg(db1_conn()));
      return -1;
   }

   sqlite3_bind_text(stmt, 1, template_name, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 2, (channel && channel[0]) ? channel : "general", -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 3, expected_agent ? expected_agent : "", -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 4, expected_role ? expected_role : "", -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 5, tmpl_raw, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 6, assign_raw, -1, SQLITE_STATIC);

   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   free(tmpl_raw);
   free(assign_raw);
   cJSON_Delete(tmpl);
   if (rc != SQLITE_DONE)
   {
      wf_set_err(err, errlen, sqlite3_errmsg(db1_conn()));
      return -1;
   }

   if (out_id)
      *out_id = (int)sqlite3_last_insert_rowid(db1_conn());
   return 0;
}

int db1_ensemble_get(int id, ensemble_info_t *out, char **prompt_out, char **context_out, char *err,
                     size_t errlen)
{
   wf_loaded_session_t loaded;
   if (wf_load_session_row(id, &loaded, err, errlen) != 0)
      return -1;

   if (out)
      *out = loaded.info;
   if (prompt_out)
      *prompt_out = (strcmp(loaded.info.status, "complete") == 0)
                        ? safe_strdup("")
                        : wf_build_prompt(loaded.template_json, loaded.context_json,
                                          loaded.info.current_phase, loaded.info.current_turn);
   if (context_out)
      *context_out = wf_context_excerpt(loaded.context_json);

   wf_loaded_session_free(&loaded);
   return 0;
}

int db1_ensemble_pause(int id, const char *reason, char *err, size_t errlen)
{
   const char *sql = "UPDATE ensembles SET status = 'paused', paused_reason = ?, "
                     "updated_at = datetime('now') WHERE id = ?";
   sqlite3_stmt *stmt = prep_stmt(sql);
   if (!stmt)
   {
      wf_set_err(err, errlen, sqlite3_errmsg(db1_conn()));
      return -1;
   }
   sqlite3_bind_text(stmt, 1, reason ? reason : "manual", -1, SQLITE_STATIC);
   sqlite3_bind_int(stmt, 2, id);
   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   if (rc != SQLITE_DONE || sqlite3_changes(db1_conn()) == 0)
   {
      snprintf(err, errlen, "ensemble %d not found", id);
      return -1;
   }
   return 0;
}

int db1_ensemble_advance(int id, const char *sender, const char *text, ensemble_info_t *out,
                         char **prompt_out, char *err, size_t errlen)
{
   wf_loaded_session_t loaded;
   if (wf_load_session_row(id, &loaded, err, errlen) != 0)
      return -1;

   if (strcmp(loaded.info.status, "complete") == 0)
   {
      wf_loaded_session_free(&loaded);
      wf_set_err(err, errlen, "ensemble is already complete");
      return -1;
   }
   if (!sender || !sender[0])
   {
      wf_loaded_session_free(&loaded);
      wf_set_err(err, errlen, "--speaker is required");
      return -1;
   }

   int sender_is_agent = wf_is_assigned_agent(loaded.template_json, sender);
   int sender_is_expected = (strcmp(sender, loaded.info.expected_agent) == 0);

   if (!sender_is_expected)
   {
      if (sender_is_agent)
      {
         wf_loaded_session_free(&loaded);
         snprintf(err, errlen, "expected '%s', got '%s'", loaded.info.expected_agent, sender);
         return -1;
      }

      wf_append_context(loaded.context_json, sender, text, loaded.info.current_phase,
                        loaded.info.current_turn);
      snprintf(loaded.info.status, sizeof(loaded.info.status), "paused");
      snprintf(loaded.info.paused_reason, sizeof(loaded.info.paused_reason), "human_interruption");
      if (wf_store_session(&loaded, err, errlen) != 0)
      {
         wf_loaded_session_free(&loaded);
         return -1;
      }
      if (out)
         *out = loaded.info;
      if (prompt_out)
         *prompt_out = wf_build_prompt(loaded.template_json, loaded.context_json,
                                       loaded.info.current_phase, loaded.info.current_turn);
      wf_loaded_session_free(&loaded);
      return 0;
   }

   wf_append_context(loaded.context_json, sender, text, loaded.info.current_phase,
                     loaded.info.current_turn);

   int next_phase = loaded.info.current_phase;
   int next_turn = loaded.info.current_turn + 1;
   int turns = wf_participant_count(loaded.template_json, loaded.info.current_phase);
   if (next_turn >= turns)
   {
      next_phase++;
      next_turn = 0;
   }

   if (next_phase >= wf_phase_count(loaded.template_json))
   {
      snprintf(loaded.info.status, sizeof(loaded.info.status), "complete");
      loaded.info.current_phase = next_phase - 1;
      loaded.info.current_turn = turns;
      loaded.info.expected_agent[0] = '\0';
      loaded.info.expected_role[0] = '\0';
      loaded.info.paused_reason[0] = '\0';
   }
   else
   {
      cJSON *next = wf_get_participant(loaded.template_json, next_phase, next_turn);
      snprintf(loaded.info.status, sizeof(loaded.info.status), "active");
      loaded.info.current_phase = next_phase;
      loaded.info.current_turn = next_turn;
      snprintf(loaded.info.expected_agent, sizeof(loaded.info.expected_agent), "%s",
               wf_json_string(next, "agent") ?: "");
      snprintf(loaded.info.expected_role, sizeof(loaded.info.expected_role), "%s",
               wf_json_string(next, "role") ?: "");
      loaded.info.paused_reason[0] = '\0';
   }

   if (wf_fill_info_from_template(&loaded.info, loaded.template_json) != 0)
   {
      wf_loaded_session_free(&loaded);
      wf_set_err(err, errlen, "failed to resolve next ensemble turn");
      return -1;
   }

   if (wf_store_session(&loaded, err, errlen) != 0)
   {
      wf_loaded_session_free(&loaded);
      return -1;
   }

   if (out)
      *out = loaded.info;
   if (prompt_out)
   {
      *prompt_out = (strcmp(loaded.info.status, "complete") == 0)
                        ? safe_strdup("")
                        : wf_build_prompt(loaded.template_json, loaded.context_json,
                                          loaded.info.current_phase, loaded.info.current_turn);
   }

   wf_loaded_session_free(&loaded);
   return 0;
}

int db1_ensemble_list(ensemble_info_t **out, int *out_count, char *err, size_t errlen)
{
   if (!out || !out_count)
   {
      wf_set_err(err, errlen, "invalid arguments");
      return -1;
   }
   *out = NULL;
   *out_count = 0;

   const char *sql =
       "SELECT id, template_name, channel, status, current_phase, current_turn, expected_agent, "
       "expected_role, paused_reason, template_json, created_at, updated_at "
       "FROM ensembles ORDER BY id";
   sqlite3_stmt *stmt = prep_stmt(sql);
   if (!stmt)
   {
      wf_set_err(err, errlen, sqlite3_errmsg(db1_conn()));
      return -1;
   }

   int cap = 8;
   int n = 0;
   ensemble_info_t *arr = calloc((size_t)cap, sizeof(*arr));
   if (!arr)
   {
      sqlite3_finalize(stmt);
      wf_set_err(err, errlen, "out of memory");
      return -1;
   }

   int rc;
   while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
   {
      if (n >= cap)
      {
         int new_cap = cap * 2;
         ensemble_info_t *grown = realloc(arr, (size_t)new_cap * sizeof(*arr));
         if (!grown)
         {
            free(arr);
            sqlite3_finalize(stmt);
            wf_set_err(err, errlen, "out of memory");
            return -1;
         }
         memset(grown + cap, 0, (size_t)(new_cap - cap) * sizeof(*arr));
         arr = grown;
         cap = new_cap;
      }

      ensemble_info_t *info = &arr[n];
      info->id = sqlite3_column_int(stmt, 0);
      snprintf(info->template_name, sizeof(info->template_name), "%s",
               (const char *)sqlite3_column_text(stmt, 1)
                   ? (const char *)sqlite3_column_text(stmt, 1)
                   : "");
      snprintf(info->channel, sizeof(info->channel), "%s",
               (const char *)sqlite3_column_text(stmt, 2)
                   ? (const char *)sqlite3_column_text(stmt, 2)
                   : "");
      snprintf(info->status, sizeof(info->status), "%s",
               (const char *)sqlite3_column_text(stmt, 3)
                   ? (const char *)sqlite3_column_text(stmt, 3)
                   : "");
      info->current_phase = sqlite3_column_int(stmt, 4);
      info->current_turn = sqlite3_column_int(stmt, 5);
      snprintf(info->expected_agent, sizeof(info->expected_agent), "%s",
               (const char *)sqlite3_column_text(stmt, 6)
                   ? (const char *)sqlite3_column_text(stmt, 6)
                   : "");
      snprintf(info->expected_role, sizeof(info->expected_role), "%s",
               (const char *)sqlite3_column_text(stmt, 7)
                   ? (const char *)sqlite3_column_text(stmt, 7)
                   : "");
      snprintf(info->paused_reason, sizeof(info->paused_reason), "%s",
               (const char *)sqlite3_column_text(stmt, 8)
                   ? (const char *)sqlite3_column_text(stmt, 8)
                   : "");
      const char *tmpl_raw = (const char *)sqlite3_column_text(stmt, 9)
                                 ? (const char *)sqlite3_column_text(stmt, 9)
                                 : "{}";
      snprintf(info->created_at, sizeof(info->created_at), "%s",
               (const char *)sqlite3_column_text(stmt, 10)
                   ? (const char *)sqlite3_column_text(stmt, 10)
                   : "");
      snprintf(info->updated_at, sizeof(info->updated_at), "%s",
               (const char *)sqlite3_column_text(stmt, 11)
                   ? (const char *)sqlite3_column_text(stmt, 11)
                   : "");

      cJSON *tmpl = cJSON_Parse(tmpl_raw);
      if (cJSON_IsObject(tmpl))
         wf_fill_info_from_template(info, tmpl);
      cJSON_Delete(tmpl);

      n++;
   }
   sqlite3_finalize(stmt);

   if (rc != SQLITE_DONE)
   {
      free(arr);
      wf_set_err(err, errlen, sqlite3_errmsg(db1_conn()));
      return -1;
   }

   *out = arr;
   *out_count = n;
   return 0;
}

int db1_ensemble_find_current_by_channel(const char *channel, int *out_id, char *err, size_t errlen)
{
   if (!channel || !channel[0] || !out_id)
   {
      wf_set_err(err, errlen, "invalid arguments");
      return -1;
   }

   const char *sql = "SELECT id FROM ensembles WHERE channel = ? "
                     "ORDER BY CASE status "
                     "  WHEN 'active' THEN 0 "
                     "  WHEN 'paused' THEN 1 "
                     "  WHEN 'complete' THEN 2 "
                     "  ELSE 3 END, "
                     "updated_at DESC, id DESC LIMIT 1";
   sqlite3_stmt *stmt = prep_stmt(sql);
   if (!stmt)
   {
      wf_set_err(err, errlen, sqlite3_errmsg(db1_conn()));
      return -1;
   }
   sqlite3_bind_text(stmt, 1, channel, -1, SQLITE_STATIC);

   int id = 0;
   int rc = sqlite3_step(stmt);
   if (rc == SQLITE_ROW)
      id = sqlite3_column_int(stmt, 0);
   sqlite3_finalize(stmt);

   if (rc != SQLITE_ROW || id <= 0)
   {
      snprintf(err, errlen, "ensemble for channel '%s' not found", channel);
      return -1;
   }

   *out_id = id;
   return 0;
}

cJSON *db1_ensemble_info_to_json(const ensemble_info_t *info, const char *prompt_text,
                                 const char *context_text)
{
   if (!info)
      return NULL;

   cJSON *obj = cJSON_CreateObject();
   if (!obj)
      return NULL;

   cJSON_AddNumberToObject(obj, "id", info->id);
   cJSON_AddStringToObject(obj, "template", info->template_name);
   cJSON_AddStringToObject(obj, "channel", info->channel);
   cJSON_AddStringToObject(obj, "status", info->status);
   cJSON_AddNumberToObject(obj, "current_phase", info->current_phase);
   cJSON_AddNumberToObject(obj, "current_turn", info->current_turn);
   cJSON_AddNumberToObject(obj, "phase_count", info->phase_count);
   cJSON_AddNumberToObject(obj, "turns_in_phase", info->turns_in_phase);
   cJSON_AddStringToObject(obj, "phase_name", info->phase_name);
   cJSON_AddStringToObject(obj, "expected_agent", info->expected_agent);
   cJSON_AddStringToObject(obj, "expected_role", info->expected_role);
   cJSON_AddStringToObject(obj, "paused_reason", info->paused_reason);
   cJSON_AddStringToObject(obj, "created_at", info->created_at);
   cJSON_AddStringToObject(obj, "updated_at", info->updated_at);
   if (prompt_text)
      cJSON_AddStringToObject(obj, "next_prompt", prompt_text);
   if (context_text)
      cJSON_AddStringToObject(obj, "recent_context", context_text);

   return obj;
}
