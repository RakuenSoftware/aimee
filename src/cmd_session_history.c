/* cmd_session_history.c: session show, search, and stats subcommands */
#include "aimee.h"
#include "db1_client/db1.h"
#include "commands.h"
#include "config.h"
#include "cJSON.h"
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* --- session show --- */

void session_subcmd_show(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;

   if (argc < 1 || !argv[0] || !argv[0][0])
   {
      fprintf(stderr, "usage: aimee session show <session-id>\n");
      return;
   }
   const char *sid = argv[0];

   /* Look up server_session record (DB1). */
   {
      db1_server_session_t row;
      if (db1_server_session_get(sid, &row) == 0)
      {
         printf("session: %s\n", row.id[0] ? row.id : sid);
         if (row.client_type[0])
            printf("  client:      %s\n", row.client_type);
         if (row.title[0])
            printf("  title:       %s\n", row.title);
         if (row.created_at[0])
            printf("  created:     %s\n", row.created_at);
         if (row.last_activity_at[0])
            printf("  last active: %s\n", row.last_activity_at);
         if (row.outcome[0])
            printf("  outcome:     %s\n", row.outcome);
      }
      else
      {
         printf("session: %s  (no server record)\n", sid);
      }
   }

   /* Session state (DB1): guardrail/hook scratch that persists across turns. */
   {
      session_state_t state;
      session_state_load(&state, sid);
      if (db1_session_state_exists(sid))
      {
         db1_session_state_summary_t sum;
         printf("\nstate:\n");
         printf("  mode:        %s\n", state.session_mode[0] ? state.session_mode : "?");
         printf("  guardrail:   %s\n", state.guardrail_mode[0] ? state.guardrail_mode : "?");
         if (state.tdd_mode[0] && strcmp(state.tdd_mode, "off") != 0)
            printf("  tdd:         %s\n", state.tdd_mode);
         if (state.active_task_id > 0)
            printf("  active_task: %lld\n", (long long)state.active_task_id);
         printf("  hook calls:  %d\n", state.hook_call_count);
         printf("  seen paths:  %d\n", state.seen_count);
         printf("  read paths:  %d\n", state.read_path_count);
         if (state.worktree_count > 0)
         {
            printf("  worktrees:\n");
            for (int i = 0; i < state.worktree_count; i++)
               printf("    %s -> %s\n", state.worktrees[i].git_root,
                      state.worktrees[i].worktree_path);
         }
         if (db1_session_state_get_summary(sid, &sum) == 0 && sum.updated_at[0])
            printf("  updated:     %s\n", sum.updated_at);
      }
      else
      {
         printf("\nstate: none recorded (session never ran a guarded tool)\n");
      }
   }

   /* Session-start brief (on-disk markdown). */
   {
      char brief_path[MAX_PATH_LEN];
      snprintf(brief_path, sizeof(brief_path), "%s/session-%s.brief.md", config_output_dir(), sid);
      struct stat bst;
      if (stat(brief_path, &bst) == 0 && S_ISREG(bst.st_mode))
         printf("\nbrief:       %s (%ld bytes)\n", brief_path, (long)bst.st_size);
   }

   /* Delegations from agent_log (DB1) */
   {
      db1_agent_log_display_t rows[128];
      int n = db1_agent_log_list_by_session(sid, rows, 128);
      if (n > 0)
         printf("\ndelegations:\n");
      for (int i = 0; i < n; i++)
      {
         printf("  [%s] %s  turns=%d tools=%d tokens=%d+%d %s %s\n",
                rows[i].created_at[0] ? rows[i].created_at : "?", rows[i].role, rows[i].turns,
                rows[i].tool_calls, rows[i].prompt_tokens, rows[i].completion_tokens,
                rows[i].success ? "ok" : "failed", rows[i].agent_name);
      }
      if (n <= 0)
         printf("\ndelegations: none\n");
   }

   /* Working memory snapshot */
   {
      wm_entry_t entries[WM_MAX_RESULTS];
      int count = db1_wm_list(sid, NULL, entries, WM_MAX_RESULTS);
      if (count > 0)
      {
         printf("\nworking memory:\n");
         for (int i = 0; i < count; i++)
         {
            const char *val = entries[i].value;
            char vbuf[80];
            if (val && strlen(val) > 72)
            {
               snprintf(vbuf, sizeof(vbuf), "%.72s...", val);
               val = vbuf;
            }
            printf("  [%s] %s = %s\n", entries[i].category[0] ? entries[i].category : "general",
                   entries[i].key[0] ? entries[i].key : "?", val ? val : "");
         }
      }
   }
}

/* --- session search --- */

void session_subcmd_search(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;

   if (argc < 1 || !argv[0] || !argv[0][0])
   {
      fprintf(stderr, "usage: aimee session search <query>\n");
      return;
   }

   char pattern[256];
   snprintf(pattern, sizeof(pattern), "%%%s%%", argv[0]);

   int found = 0;
   {
      char ids[20][WM_SESSION_ID_LEN];
      int n = db1_wm_search_session_ids(argv[0], ids, 20);
      for (int i = 0; i < n; i++)
      {
         if (!ids[i][0])
            continue;
         printf("%.8s  (working memory match)\n", ids[i]);
         found++;
      }
   }

   /* Also search agent_log (role contains the task type) — DB1. */
   {
      char ids[20][DB1_AL_SESSION_LEN];
      int n = db1_agent_log_search_session_ids_by_role(pattern, ids, 20);
      for (int i = 0; i < n; i++)
      {
         if (!ids[i][0])
            continue;
         printf("%.8s  (delegation match)\n", ids[i]);
         found++;
      }
   }

   /* Search server_sessions titles (DB1). */
   {
      db1_server_session_t rows[20];
      int n = db1_server_session_search_by_title(pattern, rows, 20);
      for (int i = 0; i < n; i++)
      {
         if (!rows[i].id[0])
            continue;
         printf("%.8s  %s  \"%s\"\n", rows[i].id, rows[i].created_at[0] ? rows[i].created_at : "?",
                rows[i].title);
         found++;
      }
   }

   if (found == 0)
      fprintf(stderr, "No sessions found matching \"%s\".\n", argv[0]);
   else
      fprintf(stderr, "\n%d session(s) matched\n", found);
}

/* --- session stats --- */

void session_subcmd_stats(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;

   const char *since = NULL;
   for (int i = 0; i < argc - 1; i++)
   {
      if (strcmp(argv[i], "--since") == 0)
         since = argv[i + 1];
   }

   /* Total session count (DB1). */
   printf("sessions total:      %d\n", db1_server_session_count(since));

   /* Active state rows in DB1 */
   {
      db1_session_state_summary_t rows[256];
      int active = db1_session_state_list(rows, 256);
      printf("sessions active:     %d\n", active);
   }

   /* Delegation stats from agent_log (DB1). */
   {
      db1_agent_log_stats_t stats;
      if (db1_agent_log_stats(since, &stats) == 0)
      {
         printf("delegations total:   %d\n", stats.total);
         if (stats.total > 0)
            printf("delegations success: %d (%.0f%%)\n", stats.successes,
                   100.0 * stats.successes / stats.total);
         printf("turns total:         %lld\n", (long long)stats.turns);
         printf("tool calls total:    %lld\n", (long long)stats.tool_calls);
         printf("tokens in/out:       %lld / %lld\n", (long long)stats.prompt_tokens,
                (long long)stats.completion_tokens);
      }
   }

   /* Most-used roles (DB1). */
   {
      db1_agent_log_role_count_t roles[5];
      int n = db1_agent_log_count_per_role(since, roles, 5);
      if (n > 0)
         printf("\ntop delegate roles:\n");
      for (int i = 0; i < n; i++)
         printf("  %-20s %d\n", roles[i].role[0] ? roles[i].role : "(none)", roles[i].count);
   }

   if (since)
      fprintf(stderr, "\n(since %s)\n", since);
}

/* --- session tokens: supervisor-vs-worker token split for one session ---
 * `aimee session tokens <session_id> [--json]`. Reads the DB1 token_audit
 * ledger and reports the primary agent's own spend (delegation_id empty) vs
 * delegate workers (delegation_id set) within that session. This is the
 * measurement surface the supervised benchmark reads to quantify how much
 * expensive-supervisor spend a run offloaded onto (typically free) workers. */
static void tokens_add_bucket_json(cJSON *parent, const char *name, int calls, long long prompt,
                                   long long completion, long long cache_read,
                                   long long cache_write, double cost_usd)
{
   cJSON *b = cJSON_AddObjectToObject(parent, name);
   cJSON_AddNumberToObject(b, "calls", calls);
   cJSON_AddNumberToObject(b, "prompt_tokens", (double)prompt);
   cJSON_AddNumberToObject(b, "completion_tokens", (double)completion);
   cJSON_AddNumberToObject(b, "cache_read_tokens", (double)cache_read);
   cJSON_AddNumberToObject(b, "cache_write_tokens", (double)cache_write);
   cJSON_AddNumberToObject(b, "total_tokens", (double)(prompt + completion));
   cJSON_AddNumberToObject(b, "cost_usd", cost_usd);
}

void session_subcmd_tokens(app_ctx_t *ctx, int argc, char **argv)
{
   const char *sid = NULL;
   for (int i = 0; i < argc; i++)
   {
      if (argv[i][0] != '-' && !sid)
         sid = argv[i];
   }
   if (!sid || !sid[0])
   {
      fprintf(stderr, "Usage: aimee session tokens <session_id> [--json]\n");
      return;
   }

   db1_token_audit_session_split_t s;
   if (db1_token_audit_session_split(sid, &s) != 0)
   {
      fprintf(stderr, "session tokens: no token data for '%s' (DB unavailable)\n", sid);
      return;
   }

   long long sup_tok = s.supervisor_prompt_tokens + s.supervisor_completion_tokens;
   long long wrk_tok = s.worker_prompt_tokens + s.worker_completion_tokens;

   if (ctx->json_output)
   {
      cJSON *o = cJSON_CreateObject();
      cJSON_AddStringToObject(o, "session_id", sid);
      tokens_add_bucket_json(o, "supervisor", s.supervisor_calls, s.supervisor_prompt_tokens,
                             s.supervisor_completion_tokens, s.supervisor_cache_read_tokens,
                             s.supervisor_cache_write_tokens, s.supervisor_cost_usd);
      tokens_add_bucket_json(o, "worker", s.worker_calls, s.worker_prompt_tokens,
                             s.worker_completion_tokens, s.worker_cache_read_tokens,
                             s.worker_cache_write_tokens, s.worker_cost_usd);
      cJSON_AddNumberToObject(o, "total_tokens", (double)(sup_tok + wrk_tok));
      char *js = cJSON_PrintUnformatted(o);
      if (js)
      {
         printf("%s\n", js);
         free(js);
      }
      cJSON_Delete(o);
   }
   else
   {
      printf("session %s\n", sid);
      printf("  supervisor: calls=%d in=%lld out=%lld total=%lld cost=$%.4f\n", s.supervisor_calls,
             (long long)s.supervisor_prompt_tokens, (long long)s.supervisor_completion_tokens,
             sup_tok, s.supervisor_cost_usd);
      printf("  worker:     calls=%d in=%lld out=%lld total=%lld cost=$%.4f\n", s.worker_calls,
             (long long)s.worker_prompt_tokens, (long long)s.worker_completion_tokens, wrk_tok,
             s.worker_cost_usd);
   }
}

/* --- session brief: read the persisted session-start briefing ---
 * `aimee session brief` prints the current worktree session's brief when
 * invoked from an aimee-managed worktree, otherwise the most recent brief;
 * `aimee session brief --session <sid>` targets a specific id;
 * `aimee session brief --list` enumerates available briefs. */

static int session_brief_filename_sid(const char *name, char *sid, size_t sid_len)
{
   if (!name || strncmp(name, "session-", 8) != 0)
      return 0;
   const char *suffix = strstr(name, ".brief.md");
   if (!suffix || suffix[9] != '\0')
      return 0;

   size_t id_len = (size_t)(suffix - (name + 8));
   if (id_len == 0 || id_len >= sid_len)
      return 0;
   memcpy(sid, name + 8, id_len);
   sid[id_len] = '\0';
   return 1;
}

static int session_brief_list(const char *config_dir)
{
   DIR *d = opendir(config_dir);
   if (!d)
      return 0;
   struct dirent *ent;
   int count = 0;
   while ((ent = readdir(d)) != NULL)
   {
      char sid[64];
      if (!session_brief_filename_sid(ent->d_name, sid, sizeof(sid)))
         continue;

      char path[MAX_PATH_LEN];
      snprintf(path, sizeof(path), "%s/%s", config_dir, ent->d_name);
      struct stat st;
      if (stat(path, &st) != 0)
         continue;
      struct tm tm_buf;
      gmtime_r(&st.st_mtime, &tm_buf);
      char ts[24];
      strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M UTC", &tm_buf);
      printf("%s  %s  %ld bytes\n", sid, ts, (long)st.st_size);
      count++;
   }
   closedir(d);
   if (count == 0)
      fprintf(stderr, "No session briefs found.\n");
   return count;
}

static char *session_brief_latest_sid(const char *config_dir)
{
   DIR *d = opendir(config_dir);
   if (!d)
      return NULL;
   char best_sid[64] = "";
   time_t best_mtime = 0;
   struct dirent *ent;
   while ((ent = readdir(d)) != NULL)
   {
      char sid[64];
      if (!session_brief_filename_sid(ent->d_name, sid, sizeof(sid)))
         continue;
      char path[MAX_PATH_LEN];
      snprintf(path, sizeof(path), "%s/%s", config_dir, ent->d_name);
      struct stat st;
      if (stat(path, &st) != 0)
         continue;
      if (st.st_mtime > best_mtime)
      {
         best_mtime = st.st_mtime;
         snprintf(best_sid, sizeof(best_sid), "%s", sid);
      }
   }
   closedir(d);
   if (!best_sid[0])
      return NULL;
   return strdup(best_sid);
}

static int session_brief_current_worktree_prefix(char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return 0;
   out[0] = '\0';

   char cwd[MAX_PATH_LEN];
   if (!getcwd(cwd, sizeof(cwd)))
      return 0;

   const char *marker = strstr(cwd, "/.aimee/worktrees/");
   if (!marker)
      return 0;
   const char *start = marker + strlen("/.aimee/worktrees/");
   const char *end = strchr(start, '/');
   size_t len = end ? (size_t)(end - start) : strlen(start);
   if (len < 8 || len >= out_len)
      return 0;

   memcpy(out, start, len);
   out[len] = '\0';
   return 1;
}

static char *session_brief_sid_for_prefix(const char *config_dir, const char *prefix)
{
   if (!prefix || !prefix[0])
      return NULL;

   DIR *d = opendir(config_dir);
   if (!d)
      return NULL;

   char best_sid[64] = "";
   time_t best_mtime = 0;
   size_t prefix_len = strlen(prefix);
   struct dirent *ent;
   while ((ent = readdir(d)) != NULL)
   {
      char sid[64];
      if (!session_brief_filename_sid(ent->d_name, sid, sizeof(sid)))
         continue;
      if (strncmp(sid, prefix, prefix_len) != 0)
         continue;

      char path[MAX_PATH_LEN];
      snprintf(path, sizeof(path), "%s/%s", config_dir, ent->d_name);
      struct stat st;
      if (stat(path, &st) != 0)
         continue;
      if (st.st_mtime > best_mtime)
      {
         best_mtime = st.st_mtime;
         snprintf(best_sid, sizeof(best_sid), "%s", sid);
      }
   }
   closedir(d);

   return best_sid[0] ? strdup(best_sid) : NULL;
}

void session_subcmd_brief(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;

   const char *sid_arg = NULL;
   int list_mode = 0;
   for (int i = 0; i < argc; i++)
   {
      if (strcmp(argv[i], "--session") == 0 && i + 1 < argc)
         sid_arg = argv[++i];
      else if (strcmp(argv[i], "--list") == 0)
         list_mode = 1;
      else if (argv[i][0] != '-' && !sid_arg)
         sid_arg = argv[i];
      else
      {
         fprintf(stderr, "usage: aimee session brief [--session <sid>] [--list]\n");
         return;
      }
   }

   const char *config_dir = config_output_dir();

   if (list_mode)
   {
      session_brief_list(config_dir);
      return;
   }

   char *resolved_sid = NULL;
   if (!sid_arg)
   {
      char current_prefix[64];
      if (session_brief_current_worktree_prefix(current_prefix, sizeof(current_prefix)))
      {
         resolved_sid = session_brief_sid_for_prefix(config_dir, current_prefix);
         if (!resolved_sid)
            resolved_sid = strdup(current_prefix);
      }
      else
      {
         resolved_sid = session_brief_latest_sid(config_dir);
      }
      if (!resolved_sid)
      {
         fprintf(stderr, "No session briefs found. Run `aimee session-start` first.\n");
         return;
      }
      sid_arg = resolved_sid;
   }

   char path[MAX_PATH_LEN];
   snprintf(path, sizeof(path), "%s/session-%s.brief.md", config_dir, sid_arg);
   FILE *fp = fopen(path, "r");
   if (!fp)
   {
      fprintf(stderr, "No brief for session %s (expected at %s).\n", sid_arg, path);
      free(resolved_sid);
      return;
   }
   char buf[4096];
   size_t n;
   while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
      fwrite(buf, 1, n, stdout);
   fclose(fp);
   free(resolved_sid);
}
