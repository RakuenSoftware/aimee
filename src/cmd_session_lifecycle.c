/* cmd_session_lifecycle.c: session-level CLI commands — session-start (with
 * its condensed-context and capabilities-reference builders), launch,
 * wrapup, and the stale-session pruner that session-start invokes.
 *
 * Extracted from cmd_hooks.c to keep file sizes tractable. Hooks-scope
 * helpers shared with cmd_hooks.c live in cmd_hooks_scope.c. */
#include "aimee.h"
#include "db1.h"
#include "db2/memory_query.h"
#include "db2/rules.h"
#include "kb_client.h"
#include "headers/plugin.h"
#include "headers/cmd_hooks_scope.h"
#include "platform_process.h"
#include "agent_config.h"
#include "agent_coord.h"
#include "lifecycle.h"
#include "agent_eval.h"
#include "log.h"
#include "trace_analysis.h"
#include "workspace.h"
#include "commands.h"
#include "cmd_session_start_util.h"
#include "cmd_hooks_platform.h"
#include "platform_random.h"
#include "slop_detect.h"
#include "git_verify.h"
#include "prompts.h"
#include "persona.h"
#include "session_briefing.h"
#include "cJSON.h"
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <stdarg.h>

/* Bounded formatted append into buf[cap] at *pos. Advances *pos only when the
 * whole formatted string fits; on truncation or a formatting error it restores
 * the prior NUL terminator and leaves *pos unchanged. A no-op once the buffer is
 * full. This keeps *pos <= cap for every caller, so a later append can never
 * compute an out-of-bounds `buf + *pos` or an underflowed `cap - *pos`. Returns
 * 1 if the whole string was written, 0 otherwise. */
static int ctx_appendf(char *buf, size_t cap, size_t *pos, const char *fmt, ...)
{
   if (*pos >= cap)
      return 0;
   va_list ap;
   va_start(ap, fmt);
   int n = vsnprintf(buf + *pos, cap - *pos, fmt, ap);
   va_end(ap);
   if (n < 0 || (size_t)n >= cap - *pos)
   {
      buf[*pos] = '\0';
      return 0;
   }
   *pos += (size_t)n;
   return 1;
}

/* --- cmd_session_start --- */

static int read_indexed_main_head(const char *project, char *out, size_t out_len)
{
   if (!project || !project[0] || !out || out_len == 0)
      return -1;
   out[0] = '\0';
   char path[MAX_PATH_LEN];
   snprintf(path, sizeof(path), "%s/index-main-%s.head", config_output_dir(), project);
   FILE *f = fopen(path, "r");
   if (!f)
      return -1;
   if (!fgets(out, (int)out_len, f))
   {
      fclose(f);
      out[0] = '\0';
      return -1;
   }
   fclose(f);
   size_t len = strlen(out);
   while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r' || out[len - 1] == ' ' ||
                      out[len - 1] == '\t'))
      out[--len] = '\0';
   return out[0] ? 0 : -1;
}

static int queue_main_index_scan_if_needed(const char *project, const char *root, const char *head,
                                           char idx_projects[][128], char idx_roots[][MAX_PATH_LEN],
                                           char idx_heads[][64], int *idx_count)
{
   if (!project || !project[0] || !root || !root[0] || !head || !head[0] || !idx_count ||
       *idx_count >= MAX_WORKTREES)
      return 0;

   char indexed_head[64];
   if (read_indexed_main_head(project, indexed_head, sizeof(indexed_head)) == 0 &&
       strcmp(indexed_head, head) == 0)
      return 0;

   for (int i = 0; i < *idx_count; i++)
      if (strcmp(idx_projects[i], project) == 0)
         return 0;

   snprintf(idx_projects[*idx_count], sizeof(idx_projects[0]), "%s", project);
   snprintf(idx_roots[*idx_count], sizeof(idx_roots[0]), "%s", root);
   snprintf(idx_heads[*idx_count], sizeof(idx_heads[0]), "%s", head);
   (*idx_count)++;
   return 1;
}

/* Build condensed session context from rules, CLAUDE.md, key facts, and
 * delegation hints. Caller owns the returned string (may be empty). */
/* Per-section scope-rank filter + qsort + render. The caller supplies
 * the candidate memory rows (already ordered by db2 priority); we
 * compute scope visibility per id, drop rows outside [min_rank,
 * max_rank], stable-sort by rank then arrival order, and emit up to
 * |limit| rows under |header|. */
typedef struct
{
   int64_t id;
   int scope_rank;
   int ordinal;
   char key[128];
   char content[384];
   char kind[32];
} session_scope_item_t;

static int session_scope_item_cmp(const void *a, const void *b)
{
   const session_scope_item_t *lhs = a;
   const session_scope_item_t *rhs = b;
   if (lhs->scope_rank != rhs->scope_rank)
      return rhs->scope_rank - lhs->scope_rank;
   return lhs->ordinal - rhs->ordinal;
}

static size_t session_append_scope_section(const memory_t *rows, int n_rows, char *buf, size_t pos,
                                           size_t cap, const char *header, const char *workspace,
                                           const char *project, int min_rank, int max_rank,
                                           int limit)
{
   session_scope_item_t items[32];
   int item_count = 0;
   int max_items = (int)(sizeof(items) / sizeof(items[0]));

   /* Batch the per-row visibility rank lookup into a single aimee-kb RPC. */
   int n_lookup = n_rows < max_items ? n_rows : max_items;
   int64_t lookup_ids[32];
   int lookup_ranks[32] = {0};
   for (int i = 0; i < n_lookup; i++)
      lookup_ids[i] = rows[i].id;
   if (n_lookup > 0)
      kb_client_memory_scope_visibility_rank(lookup_ids, n_lookup, workspace, project,
                                             lookup_ranks);

   for (int i = 0; i < n_rows && item_count < max_items; i++)
   {
      int rank = (i < n_lookup) ? lookup_ranks[i] : 0;
      if (rank < min_rank || rank > max_rank)
         continue;

      items[item_count].id = rows[i].id;
      items[item_count].scope_rank = rank;
      items[item_count].ordinal = i;
      snprintf(items[item_count].key, sizeof(items[item_count].key), "%s", rows[i].key);
      snprintf(items[item_count].content, sizeof(items[item_count].content), "%s", rows[i].content);
      snprintf(items[item_count].kind, sizeof(items[item_count].kind), "%s", rows[i].kind);
      item_count++;
   }

   if (item_count == 0)
      return pos;

   qsort(items, (size_t)item_count, sizeof(items[0]), session_scope_item_cmp);

   ctx_appendf(buf, cap, &pos, "%s\n", header);
   int emitted = 0;
   for (int i = 0; i < item_count && emitted < limit && pos < cap - 512; i++)
   {
      const char *text = items[i].content[0] ? items[i].content : items[i].key;
      if (items[i].kind[0])
         ctx_appendf(buf, cap, &pos, "- [%s] %.300s\n", items[i].kind, text);
      else
         ctx_appendf(buf, cap, &pos, "- %.300s\n", text);
      emitted++;
   }

   if (emitted > 0)
      ctx_appendf(buf, cap, &pos, "\n");
   return pos;
}

static void session_context_resolve_cwd(const char *client_cwd, char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return;
   out[0] = '\0';
   if (client_cwd && client_cwd[0])
   {
      snprintf(out, out_len, "%s", client_cwd);
      return;
   }
   if (!getcwd(out, out_len))
      out[0] = '\0';
}

static int session_context_verbose_enabled(void)
{
   const char *v = getenv("AIMEE_SESSION_START_VERBOSE");
   return v && (strcmp(v, "1") == 0 || strcmp(v, "true") == 0 || strcmp(v, "yes") == 0);
}

static char *build_session_context(const char *client_cwd)
{
   size_t cap = 65536;
   char *buf = malloc(cap);
   size_t pos = 0;
   char scope_cwd[MAX_PATH_LEN];
   int verbose = session_context_verbose_enabled();
   aimee_mode_t mode = config_current_mode();
   char persona_name[PERSONA_NAME_MAX];
   config_current_persona(persona_name, sizeof(persona_name));
   /* When nothing more specific selected a persona — no AIMEE_MODE override and no
    * durable /novel-style mode file, so the resolver fell through to the built-in
    * `engineer` default — honor the operator-configured default persona so a fresh
    * primary session starts as it (config.default_persona itself defaults to
    * engineer, so this is a no-op until the operator changes it). */
   if (strcmp(persona_name, "engineer") == 0)
   {
      config_t persona_cfg;
      if (config_load(&persona_cfg) == 0 && persona_cfg.default_persona[0] &&
          strcmp(persona_cfg.default_persona, "engineer") != 0)
      {
         snprintf(persona_name, sizeof(persona_name), "%s", persona_cfg.default_persona);
         mode = aimee_mode_from_string(persona_name);
      }
   }
   persona_t persona;
   persona_load(NULL, persona_name, &persona);

   session_context_resolve_cwd(client_cwd, scope_cwd, sizeof(scope_cwd));

   /* Principles and Aimee lookup hints lead the session context for primacy
    * bias, driven by the active persona. */
   ctx_appendf(buf, cap, &pos, "%s",
               persona.principles_text ? persona.principles_text : prompt_principles_text(mode));

   /* Inject the active persona's own prose at the start of every primary session
    * — uniformly, for every persona. Engineer is not special here: it is simply
    * the default persona, and its prose carries the manager/work-queue framing
    * because it is the orchestrator persona. Whichever persona resolved above,
    * its "You are ..." identity and role lead the context so the session adopts
    * it. Delegates never receive this block — they compose their prompt via
    * persona_compose_delegate_prompt, which omits the engineer manager framing.
    * Bounded append: pos is advanced only if the block fully fits, so a
    * truncating render can never push pos past cap and corrupt later appends. */
   {
      char *identity = persona_identity_prose(&persona, scope_cwd);
      if (identity && identity[0])
         ctx_appendf(buf, cap, &pos, "\n# Persona\n%s\n", identity);
      free(identity);
   }

   if (persona.brief_text)
      ctx_appendf(buf, cap, &pos,
                  "# Aimee Context\n%s"
                  "- Use `aimee session brief` to inspect the full startup brief.\n\n",
                  persona.brief_text);
   else
      ctx_appendf(buf, cap, &pos,
                  "# Aimee Context\n"
                  "- Use `aimee index overview` or `aimee index find <symbol>` for indexed code "
                  "lookup when it helps.\n"
                  "- Use `aimee memory search <terms>` for prior project context before "
                  "rediscovery.\n"
                  "- Use `aimee delegate <role> \"prompt\"` for bounded delegated or parallel "
                  "work.\n"
                  "- Use `aimee session brief` to inspect the full startup brief.\n\n");

   /* Enforce aimee's memory system as the single memory of record: steer the
    * agent to aimee memory commands rather than a native store. `.md` memory is
    * retired — a write under the memory dir is intercepted into aimee and never
    * persisted as a file. */
   ctx_appendf(
       buf, cap, &pos,
       "# Memory (use aimee, not your own store)\n"
       "- aimee is the single memory of record. Store durable memory with "
       "`aimee memory store <key> <content>`; set who-you-are / preferences with "
       "`aimee memory identity <key> <value>` and `aimee memory prefer <key> <value>`.\n"
       "- Retrieve prior context with `aimee memory search <terms>` or `aimee memory recall`.\n"
       "- Memory `.md` files are RETIRED: a `.md` write UNDER YOUR MEMORY DIR is intercepted "
       "into aimee and not persisted as a file. Writing `.md` files elsewhere (docs, READMEs, "
       "notes) is unaffected — only the memory dir is intercepted.\n");
   ctx_appendf(buf, cap, &pos, "\n");

   {
      char cwd[MAX_PATH_LEN];
      config_t skill_cfg;
      char *skill_index = NULL;
      config_load(&skill_cfg);
      if (skill_cfg.skills_dispatch_enabled)
      {
         snprintf(cwd, sizeof(cwd), "%s", scope_cwd);
         skill_index =
             session_briefing_render_skill_index(cwd, skill_cfg.skills_dispatch_max_index);
      }
      if (skill_index && skill_index[0])
      {
         size_t n = strlen(skill_index);
         if (pos + n < cap)
         {
            memcpy(buf + pos, skill_index, n);
            pos += n;
         }
      }
      free(skill_index);
   }

   ctx_appendf(buf, cap, &pos, "# Rules\n\n");

   /* Aimee rules (from feedback/learning) */

   char *rules = kb_client_rules_generate();
   if (rules && rules[0] && strcmp(rules, "# Rules\n\n") != 0 &&
       strcmp(rules, "No rules configured.") != 0)
   {
      const char *body = rules;
      if (strncmp(body, "# Rules\n", 8) == 0)
         body += 8;
      while (*body == '\n')
         body++;
      if (*body)
         ctx_appendf(buf, cap, &pos, "%s\n", body);
   }
   free(rules);

   /* Hierarchical local context: walk cwd up to project root and inject nearby
    * rule/convention files (.aimee-rules, AGENTS.md, CONTRIBUTING.md, ...). */
   {
      if (scope_cwd[0])
      {
         size_t used = pos;
         size_t budget = (used < cap) ? (cap - used) / 2 : 0;
         if (budget > CONTEXT_DISCOVER_BUDGET_DEFAULT)
            budget = CONTEXT_DISCOVER_BUDGET_DEFAULT;
         if (budget > 256)
         {
            context_discovery_t disc;
            if (context_discover(scope_cwd, budget, &disc) == 0 && disc.rendered)
            {
               if (pos + disc.bytes_used + 2 < cap)
               {
                  ctx_appendf(buf, cap, &pos, "%s\n", disc.rendered);
               }
            }
            context_discovery_free(&disc);
         }
      }
   }

   /* Key infrastructure facts (top 5 most-used) */
   if (verbose)
   {
      memory_t facts[15];
      int n_facts = kb_client_memory_top_l2_facts(facts, (int)(sizeof(facts) / sizeof(facts[0])));
      int has_facts = 0;
      for (int i = 0; i < n_facts && pos < cap - 600; i++)
      {
         if (!has_facts)
         {
            ctx_appendf(buf, cap, &pos, "# Key Facts\n");
            has_facts = 1;
         }
         ctx_appendf(buf, cap, &pos, "- %s: %.300s\n", facts[i].key, facts[i].content);
      }
      if (has_facts)
         ctx_appendf(buf, cap, &pos, "\n");
   }

   /* Open commitments + unresolved directives — phase-2-step-2 recall.
    * Empty outputs surface nothing; the sections only appear when
    * there's real state to show. */
   {
      char *commits = kb_client_session_briefing_commitments(0);
      if (commits && commits[0])
      {
         size_t n = strlen(commits);
         if (pos + n < cap)
         {
            memcpy(buf + pos, commits, n);
            pos += n;
         }
      }
      free(commits);

      char *qs = kb_client_session_briefing_directives(0);
      if (qs && qs[0])
      {
         size_t n = strlen(qs);
         if (pos + n < cap)
         {
            memcpy(buf + pos, qs, n);
            pos += n;
         }
      }
      free(qs);
   }

   /* Network summary */
   if (verbose)
   {
      agent_config_t net_cfg;
      if (agent_load_config(&net_cfg) == 0 && net_cfg.network.ssh_entry[0])
      {
         agent_network_t *nw = &net_cfg.network;
         ctx_appendf(buf, cap, &pos, "# Network\nEntry: %s\n", nw->ssh_entry);
         if (nw->host_count > 0)
         {
            ctx_appendf(buf, cap, &pos, "Hosts (%d): ", nw->host_count);
            int show = nw->host_count < 5 ? nw->host_count : 5;
            for (int i = 0; i < show && pos < cap - 128; i++)
               ctx_appendf(buf, cap, &pos, "%s%s (%s)", i > 0 ? ", " : "", nw->hosts[i].name,
                           nw->hosts[i].ip);
            if (nw->host_count > show)
               ctx_appendf(buf, cap, &pos, " +%d more", nw->host_count - show);
            ctx_appendf(buf, cap, &pos, "\n");
         }
         if (nw->network_count > 0)
         {
            for (int i = 0; i < nw->network_count && pos < cap - 128; i++)
               ctx_appendf(buf, cap, &pos, "- %s: %s (%s)\n", nw->networks[i].name,
                           nw->networks[i].cidr, nw->networks[i].desc);
         }
         ctx_appendf(buf, cap, &pos,
                     "Run `aimee agent network` or `aimee --json agent network` "
                     "for full host details.\n\n");
      }
   }

   /* Workspace-scoped project context */
   {
      if (scope_cwd[0])
      {
         config_t app_cfg;
         config_load(&app_cfg);
         char workspace_name[MAX_PATH_LEN];
         char project_name[MAX_PATH_LEN];
         hook_scope_labels_for_cwd(&app_cfg, scope_cwd, workspace_name, sizeof(workspace_name),
                                   project_name, sizeof(project_name));

         if ((project_name[0] || workspace_name[0]) && pos < cap - 512)
         {
            int has_section = 0;

            /* Canonical project/workspace-visible memories. */
            memory_t scope_rows[24];
            int n_scope = kb_client_memory_list_session_scope_priority(
                scope_rows, (int)(sizeof(scope_rows) / sizeof(scope_rows[0])));
            if (n_scope > 0)
            {
               size_t next = session_append_scope_section(
                   scope_rows, n_scope, buf, pos, cap,
                   project_name[0] ? "# Project Context" : "# Workspace Context",
                   workspace_name[0] ? workspace_name : NULL, project_name[0] ? project_name : NULL,
                   2, 3, 10);
               has_section = next != pos;
               pos = next;
            }

            /* Fallback: LIKE matching for memories not yet workspace-tagged */
            if (!has_section && project_name[0])
            {
               char like_pattern[256];
               snprintf(like_pattern, sizeof(like_pattern), "%%%s%%", project_name);

               memory_t like_rows[5];
               int n_like = kb_client_memory_list_session_scope_priority_like(
                   like_pattern, like_rows, (int)(sizeof(like_rows) / sizeof(like_rows[0])));
               for (int i = 0; i < n_like && pos < cap - 512; i++)
               {
                  if (!has_section)
                  {
                     ctx_appendf(buf, cap, &pos, "# Project Context (%s)\n", project_name);
                     has_section = 1;
                  }
                  const char *text =
                      like_rows[i].content[0] ? like_rows[i].content : like_rows[i].key;
                  if (like_rows[i].kind[0])
                     ctx_appendf(buf, cap, &pos, "- [%s] %.300s\n", like_rows[i].kind, text);
                  else
                     ctx_appendf(buf, cap, &pos, "- %.300s\n", text);
               }
            }

            /* Index stats for this project */
            int file_count = 0;
            int def_count = 0;
            if (project_name[0] &&
                kb_client_index_project_stats(project_name, &file_count, &def_count) == 0 &&
                (file_count > 0 || def_count > 0))
            {
               if (!has_section)
               {
                  ctx_appendf(buf, cap, &pos, "# Project Context (%s)\n", project_name);
                  has_section = 1;
               }
               ctx_appendf(buf, cap, &pos, "- %d files indexed, %d definitions\n", file_count,
                           def_count);
            }

            if (has_section)
               ctx_appendf(buf, cap, &pos, "\n");
         }
      }
   }

   /* Shared/global context resolved through canonical scope visibility. */
   if (verbose)
   {
      memory_t scope_rows[24];
      int n_scope = kb_client_memory_list_session_scope_priority(
          scope_rows, (int)(sizeof(scope_rows) / sizeof(scope_rows[0])));
      if (n_scope > 0)
      {
         char cwd[MAX_PATH_LEN];
         char workspace_name[MAX_PATH_LEN] = "";
         char project_name[MAX_PATH_LEN] = "";
         config_t app_cfg;
         config_load(&app_cfg);
         session_context_resolve_cwd(client_cwd, cwd, sizeof(cwd));
         if (cwd[0])
            hook_scope_labels_for_cwd(&app_cfg, cwd, workspace_name, sizeof(workspace_name),
                                      project_name, sizeof(project_name));
         pos = session_append_scope_section(scope_rows, n_scope, buf, pos, cap, "# Shared Context",
                                            workspace_name[0] ? workspace_name : NULL,
                                            project_name[0] ? project_name : NULL, 1, 1, 5);
      }
   }

   /* Sub-agent delegation (IMPORTANT) */
   {
      agent_config_t acfg;
      if (agent_load_config(&acfg) == 0 && acfg.agent_count > 0)
      {
         int has_tools = 0;
         for (int i = 0; i < acfg.agent_count; i++)
         {
            if (acfg.agents[i].tools_enabled)
            {
               has_tools = 1;
               break;
            }
         }
         /* Delegation guidance is driven by the persona's delegate policy
          * (full: must delegate; readonly: read-only delegates only; none: no
          * delegates). Emitted whenever delegate agents are configured, except
          * we still tell a no-delegate persona it has none. */
         if (has_tools || !persona_delegates_enabled(&persona))
         {
            char delblock[1024];
            persona_delegation_block(&persona, delblock, sizeof(delblock));
            ctx_appendf(buf, cap, &pos, "%s", delblock);
         }
      }
   }

   /* Recent delegation outcomes (DB1 agent_log). */
   if (verbose)
   {
      db1_agent_log_display_t rows[5];
      int n = db1_agent_log_list_recent(rows, 5);
      if (n > 0 && pos < cap - 256)
         ctx_appendf(buf, cap, &pos, "# Recent Delegations\n");
      for (int i = 0; i < n && pos < cap - 256; i++)
      {
         const char *drole = rows[i].role[0] ? rows[i].role : "?";
         const char *dagent = rows[i].agent_name[0] ? rows[i].agent_name : "?";
         if (rows[i].success)
            ctx_appendf(buf, cap, &pos, "- [%s] via %s: OK (%d turns, %d tools)\n", drole, dagent,
                        rows[i].turns, rows[i].tool_calls);
         else
            ctx_appendf(buf, cap, &pos, "- [%s] via %s: FAILED (%.80s)\n", drole, dagent,
                        rows[i].error[0] ? rows[i].error : "unknown");
      }
      if (n > 0 && pos < cap - 256)
         ctx_appendf(buf, cap, &pos, "\n");
   }

   /* Workspace project descriptions and style guides */
   if (verbose)
   {
      config_t ws_cfg;
      config_load(&ws_cfg);
      char *ws_ctx = workspace_build_context_from_config(&ws_cfg);
      if (ws_ctx && ws_ctx[0])
      {
         size_t ws_len = strlen(ws_ctx);
         if (pos + ws_len < cap)
         {
            memcpy(buf + pos, ws_ctx, ws_len);
            pos += ws_len;
         }
      }
      free(ws_ctx);
   }

   /* Work queue summary */
   if (verbose && pos + 256 < cap)
   {
      int wlen = work_queue_summary(buf + pos, cap - pos);
      if (wlen > 0)
      {
         pos += (size_t)wlen;
         if (pos < cap)
            buf[pos++] = '\n';
      }
   }

   /* aimee capabilities reference */
   if (verbose)
   {
      char *caps = build_capabilities_text();
      if (caps)
      {
         size_t clen = strlen(caps);
         if (pos + clen < cap)
         {
            memcpy(buf + pos, caps, clen);
            pos += clen;
         }
         free(caps);
      }
   }

   persona_free(&persona);
   buf[pos] = '\0';
   return buf;
}

/* Count active sessions in DB1 excluding the current session. */
static int count_active_sessions(void)
{
   db1_session_state_summary_t rows[128];
   int n = db1_session_state_list(rows, 128);
   const char *cur = session_id();
   int count = 0;
   for (int i = 0; i < n; i++)
   {
      if (rows[i].session_id[0] && strcmp(rows[i].session_id, cur) != 0)
         count++;
   }
   return count;
}

/* Return the configured stale-session threshold in seconds.
 * Defaults to CONFIG_DEFAULT_STALE_SESSION_SECS (14400 = 4 hours) when unset. */
static int stale_session_threshold_secs(const config_t *cfg)
{
   return (cfg && cfg->worktree_stale_secs > 0) ? cfg->worktree_stale_secs
                                                : CONFIG_DEFAULT_STALE_SESSION_SECS;
}

/* Remove sibling worktrees for a stale session.
 * Uses worktree_cleanup() which checks each workspace's git root for the
 * standard .aimee-<project>-<short_session> sibling directory. */
static void remove_stale_worktrees(const config_t *cfg, const char *sid)
{
   for (int i = 0; i < cfg->workspace_count; i++)
   {
      char git_root[MAX_PATH_LEN];
      if (git_repo_root(cfg->workspaces[i], git_root, sizeof(git_root)) == 0)
         worktree_cleanup(git_root, sid, NULL);
   }
}

/* Prune stale sessions: fold their memories, run maintenance, clean up worktrees
 * and DB1 state rows. This replaces the explicit wrapup command for ended sessions. */
void prune_stale_sessions(const config_t *cfg)
{
   int did_maintenance = 0;

   char stale_ids[64][DB1_SS_SID_LEN];
   int n = db1_session_state_list_expired(stale_session_threshold_secs(cfg), stale_ids, 64);
   const char *cur = session_id();

   for (int i = 0; i < n; i++)
   {
      const char *stale_sid = stale_ids[i];
      if (!stale_sid[0] || strcmp(stale_sid, cur) == 0)
         continue;

      /* Fold this session's L0 memories into L1 (runs inside aimee-kb). */
      kb_client_memory_fold_session(stale_sid);
      did_maintenance = 1;

      /* Release any work queue items claimed by this stale session */
      if (db1_work_queue_release_claimed_by(stale_sid) < 0)
         LOG_ERROR("prune_stale_sessions", "release work items failed for sid=%s", stale_sid);

      /* Remove worktrees for this session */
      remove_stale_worktrees(cfg, stale_sid);

      /* Delete the stale state row + child tables from DB1 */
      db1_session_state_delete(stale_sid);
   }

   /* Run global maintenance if any sessions were pruned */
   if (did_maintenance)
   {
      /* Scan conversations */
      char dirs[8][MAX_PATH_LEN];
      int dir_count = config_conversation_dirs(cfg, dirs, 8);
      kb_client_memory_scan_conversations(dirs, dir_count);

      /* Expire session directives */
      kb_client_directive_expire_session();

      /* Legacy monolithic command path. Shipped client requests route through
       * aimee-server; the server RPC port must split DB1 reads from DB2 writes
       * before this maintenance can be exposed through shipped surfaces. */
      int promoted = 0, demoted = 0, expired = 0;
      memory_run_maintenance(&promoted, &demoted, &expired);

      /* Extract anti-patterns */
      kb_client_anti_pattern_extract_from_feedback();
      kb_client_anti_pattern_extract_from_failures();

      /* Learn style */
      kb_client_memory_learn_style();

      /* Compact old windows */
      int sc = 0, fc = 0;
      kb_client_memory_compact_windows(&sc, &fc);

      /* Regenerate rules — round-tripped through aimee-kb so the cache
       * inside that process gets refreshed alongside the local one. */

      char *md = kb_client_rules_generate();
      free(md);
   }

   /* Clean up expired server_sessions and their worktrees (DB1). */
   {
      int threshold = stale_session_threshold_secs(cfg);
      char expired_ids[128][DB1_SS_ID_LEN];
      int n = db1_server_session_list_expired(threshold, expired_ids, 128);
      for (int i = 0; i < n; i++)
      {
         if (expired_ids[i][0])
            remove_stale_worktrees(cfg, expired_ids[i]);
      }
      (void)db1_server_session_delete_expired(threshold);
   }
}

static char *build_working_directories_text(const session_state_t *state)
{
   if (!state || state->worktree_count <= 0)
      return strdup("");

   int listed = 0;
   for (int i = 0; i < state->worktree_count; i++)
   {
      struct stat st;
      if (state->worktrees[i].git_root[0] && state->worktrees[i].worktree_path[0] &&
          stat(state->worktrees[i].worktree_path, &st) == 0 && S_ISDIR(st.st_mode))
         listed++;
   }
   if (listed == 0)
      return strdup("");

   size_t cap = 1024 + ((size_t)state->worktree_count * (MAX_PATH_LEN * 2 + 16));
   char *buf = calloc(1, cap);
   if (!buf)
      return strdup("");

   size_t off = 0;
#define APPEND_WORKDIR_FMT(...)                                                                    \
   do                                                                                              \
   {                                                                                               \
      if (off < cap)                                                                               \
      {                                                                                            \
         int _n = snprintf(buf + off, cap - off, __VA_ARGS__);                                     \
         if (_n > 0)                                                                               \
            off += (size_t)_n;                                                                     \
         if (off >= cap)                                                                           \
            off = cap - 1;                                                                         \
      }                                                                                            \
   } while (0)

   APPEND_WORKDIR_FMT("# Isolated Checkout\n"
                      "Aimee is forcing this session's tools into these worktrees. "
                      "The source checkout may belong to another session.\n");
   for (int i = 0; i < state->worktree_count; i++)
   {
      struct stat st;
      if (state->worktrees[i].git_root[0] && state->worktrees[i].worktree_path[0] &&
          stat(state->worktrees[i].worktree_path, &st) == 0 && S_ISDIR(st.st_mode))
         APPEND_WORKDIR_FMT("- %s -> %s\n", state->worktrees[i].git_root,
                            state->worktrees[i].worktree_path);
   }
   APPEND_WORKDIR_FMT("\n");

#undef APPEND_WORKDIR_FMT
   return buf;
}

/* session_start_emit: shared body for cmd_session_start (CLI dispatch) and
 * the hooks.session_start RPC. Callers supply the hook payload (already
 * read) and a sink for the brief text. The CLI wrapper uses stdout; the
 * server RPC uses an open_memstream buffer so the captured brief can be
 * returned to the thin client. */

/* Changelog + index-scan results produced by session_build_changelog and
 * consumed by the brief emitter and the background re-indexer. */
typedef struct
{
   char buf[WM_MAX_VALUE_LEN];
   int has_changelog;
   char index_projects[MAX_WORKTREES][128];
   char index_roots[MAX_WORKTREES][MAX_PATH_LEN];
   char index_heads[MAX_WORKTREES][64];
   int index_count;
} session_changelog_t;

/* Startup-only DB1 integrity gate: quick-check and auto-recover, aborting the
 * hook if the database cannot be opened or is unrecoverable. */
static void session_startup_db_check(const config_t *cfg)
{
   int rc = db1_diag_quick_check_recover(cfg->db1_path);
   if (rc == -2)
      fatal("cannot open database");
   if (rc == -1)
   {
      LOG_ERROR("db", "database corruption detected");
      fatal("database corrupted, run: aimee db recover --force");
   }
   if (rc == 1)
      LOG_INFO("db", "database recovered successfully");
}

/* Enforce the max-sessions cap: prune idle sessions when at the cap, then warn
 * (never refuse — a hook failure would block the user) if still over. */
static void session_enforce_session_cap(const config_t *cfg)
{
   if (cfg->max_sessions <= 0)
      return;
   int active = count_active_sessions();
   if (active >= cfg->max_sessions)
   {
      prune_stale_sessions(cfg);
      active = count_active_sessions();
      if (active >= cfg->max_sessions)
         fprintf(stderr,
                 "aimee: warning: %d active session(s) at cap (%d). "
                 "Run 'aimee session clean' to remove idle sessions.\n",
                 active, cfg->max_sessions);
   }
}

/* Enforce the max-worktrees cap across configured workspaces plus the CWD repo:
 * prune when at the cap, recount, then warn if still over. */
static void session_enforce_worktree_cap(const config_t *cfg)
{
   if (cfg->max_worktrees <= 0)
      return;
   int wt_total = 0;
   for (int i = 0; i < cfg->workspace_count; i++)
   {
      char gr[MAX_PATH_LEN];
      if (git_repo_root(cfg->workspaces[i], gr, sizeof(gr)) == 0)
         wt_total += count_active_worktrees_for_root(gr);
   }
   /* Also count CWD's repo if not already covered by configured workspaces */
   {
      char cwd[MAX_PATH_LEN];
      char gr[MAX_PATH_LEN];
      if (getcwd(cwd, sizeof(cwd)) && git_repo_root(cwd, gr, sizeof(gr)) == 0)
      {
         int already = 0;
         for (int i = 0; i < cfg->workspace_count; i++)
         {
            char wgr[MAX_PATH_LEN];
            if (git_repo_root(cfg->workspaces[i], wgr, sizeof(wgr)) == 0 && strcmp(wgr, gr) == 0)
            {
               already = 1;
               break;
            }
         }
         if (!already)
            wt_total += count_active_worktrees_for_root(gr);
      }
   }
   if (wt_total >= cfg->max_worktrees)
   {
      prune_stale_sessions(cfg);
      /* Recount after pruning */
      wt_total = 0;
      for (int i = 0; i < cfg->workspace_count; i++)
      {
         char gr[MAX_PATH_LEN];
         if (git_repo_root(cfg->workspaces[i], gr, sizeof(gr)) == 0)
            wt_total += count_active_worktrees_for_root(gr);
      }
      if (wt_total >= cfg->max_worktrees)
         fprintf(stderr,
                 "aimee: warning: %d active worktree(s) at cap (%d). "
                 "Run 'aimee session clean' to remove idle sessions and their worktrees.\n",
                 wt_total, cfg->max_worktrees);
   }
}

/* Register sibling worktree mappings for configured workspaces and the client
 * CWD's git repo, deduplicating by git root and respecting MAX_WORKTREES. */
static void session_register_worktrees(const config_t *cfg, const char *client_cwd, const char *sid,
                                       session_state_t *state)
{
   for (int i = 0; i < cfg->workspace_count && state->worktree_count < MAX_WORKTREES; i++)
   {
      struct stat ws_st;
      if (stat(cfg->workspaces[i], &ws_st) != 0 || !S_ISDIR(ws_st.st_mode))
      {
         LOG_WARN("workspace", "workspace '%s' does not exist, skipping worktree",
                  cfg->workspaces[i]);
         continue;
      }

      char gr[MAX_PATH_LEN];
      if (git_repo_root(cfg->workspaces[i], gr, sizeof(gr)) == 0)
      {
         /* Deduplicate by git_root */
         if (!session_worktree_is_registered(state, gr))
         {
            worktree_mapping_t *m = &state->worktrees[state->worktree_count];
            snprintf(m->git_root, sizeof(m->git_root), "%s", gr);
            worktree_sibling_path(gr, sid, NULL, m->worktree_path, sizeof(m->worktree_path));
            state->worktree_count++;
         }
      }
   }

   /* Auto-detect: if the client CWD is inside a git repo and not already covered,
    * register it. The server process CWD is often the install/runtime directory,
    * so prefer the hook-provided client path when available. */
   {
      char cwd[MAX_PATH_LEN];
      if (client_cwd[0] || getcwd(cwd, sizeof(cwd)))
      {
         if (client_cwd[0])
            snprintf(cwd, sizeof(cwd), "%s", client_cwd);
         char gr[MAX_PATH_LEN];
         if (git_repo_root(cwd, gr, sizeof(gr)) == 0)
         {
            if (!session_worktree_is_registered(state, gr) && state->worktree_count < MAX_WORKTREES)
            {
               worktree_mapping_t *m = &state->worktrees[state->worktree_count];
               snprintf(m->git_root, sizeof(m->git_root), "%s", gr);
               worktree_sibling_path(gr, sid, NULL, m->worktree_path, sizeof(m->worktree_path));
               state->worktree_count++;
            }
         }
      }
   }
}

/* Prepare an isolated sibling checkout for every registered repo. Never touch
 * the source checkout (it may belong to another session or the user); base the
 * session branch on the configured primary branch's remote ref when available. */
static void session_checkout_worktrees(const session_state_t *state, const char *sid)
{
   for (int i = 0; i < state->worktree_count; i++)
   {
      const char *gr = state->worktrees[i].git_root;
      if (!gr[0])
         continue;

      char primary_branch[256] = {0};
      char base_ref[320] = {0};
      if (project_primary_branch(gr, primary_branch, sizeof(primary_branch)) == 0)
      {
         const char *fetch_argv[] = {"fetch", "origin", primary_branch, NULL};
         char *fetch_out = NULL;
         int rc = git_net_exec(gr, fetch_argv, &fetch_out, 2048);
         if (rc != 0)
            LOG_WARN("session", "failed to fetch primary branch '%s' in %s: %s", primary_branch, gr,
                     fetch_out ? fetch_out : "unknown");
         free(fetch_out);

         char remote_ref[320];
         snprintf(remote_ref, sizeof(remote_ref), "origin/%s", primary_branch);
         const char *verify_remote_argv[] = {"git",      "-C",       gr,  "rev-parse",
                                             "--verify", remote_ref, NULL};
         char *verify_out = NULL;
         rc = safe_exec_capture(verify_remote_argv, &verify_out, 256);
         free(verify_out);
         if (rc == 0)
         {
            snprintf(base_ref, sizeof(base_ref), "%s", remote_ref);
         }
         else
         {
            const char *verify_local_argv[] = {"git",      "-C",           gr,  "rev-parse",
                                               "--verify", primary_branch, NULL};
            char *local_out = NULL;
            rc = safe_exec_capture(verify_local_argv, &local_out, 256);
            free(local_out);
            if (rc == 0)
               snprintf(base_ref, sizeof(base_ref), "%s", primary_branch);
         }
      }

      int rc = base_ref[0] ? worktree_create_sibling_from_ref(gr, sid, NULL, base_ref)
                           : worktree_create_sibling(gr, sid, NULL);
      if (rc != 0)
         LOG_WARN("session", "failed to prepare isolated worktree for %s", gr);
   }
}

/* Compute the per-project changelog vs the previous session's main HEADs,
 * persist the new HEADs for next time, store the changelog in working memory,
 * and record which projects need a background main-index scan. */
static void session_build_changelog(const session_state_t *state, const char *sid,
                                    session_changelog_t *cl)
{
   cl->has_changelog = 0;
   cl->index_count = 0;

   char heads_path[MAX_PATH_LEN];
   snprintf(heads_path, sizeof(heads_path), "%s/main_heads.json", config_output_dir());

   /* Load previous main heads */
   cJSON *prev_heads = NULL;
   {
      FILE *hf = fopen(heads_path, "r");
      if (hf)
      {
         fseek(hf, 0, SEEK_END);
         long hlen = ftell(hf);
         fseek(hf, 0, SEEK_SET);
         if (hlen > 0 && hlen < 65536)
         {
            char *hbuf = malloc(hlen + 1);
            if (hbuf)
            {
               size_t nr = fread(hbuf, 1, hlen, hf);
               hbuf[nr] = '\0';
               prev_heads = cJSON_Parse(hbuf);
               free(hbuf);
            }
         }
         fclose(hf);
      }
   }

   /* For each workspace, get current main HEAD and build changelog */
   int cl_off = 0;
   cJSON *new_heads = cJSON_CreateObject();

   for (int i = 0; i < state->worktree_count; i++)
   {
      const char *ws_root = state->worktrees[i].git_root;
      if (!ws_root[0])
         continue;

      const char *proj_name = session_project_name_from_root(ws_root);

      /* Get current HEAD of default branch */
      char head_cmd_buf[MAX_PATH_LEN + 64];
      snprintf(head_cmd_buf, sizeof(head_cmd_buf), "%s", ws_root);
      const char *rev_argv[] = {"git", "-C", head_cmd_buf, "rev-parse", "HEAD", NULL};
      char *head_out = NULL;
      int rc = safe_exec_capture(rev_argv, &head_out, 256);
      if (rc != 0 || !head_out)
      {
         free(head_out);
         continue;
      }

      /* Trim newline */
      size_t hlen = strlen(head_out);
      while (hlen > 0 && (head_out[hlen - 1] == '\n' || head_out[hlen - 1] == '\r'))
         head_out[--hlen] = '\0';

      if (hlen == 0)
      {
         free(head_out);
         continue;
      }

      cJSON_AddStringToObject(new_heads, proj_name, head_out);

      (void)queue_main_index_scan_if_needed(proj_name, ws_root, head_out, cl->index_projects,
                                            cl->index_roots, cl->index_heads, &cl->index_count);

      /* Check for previous HEAD */
      cJSON *ph = prev_heads ? cJSON_GetObjectItem(prev_heads, proj_name) : NULL;
      if (ph && cJSON_IsString(ph) && ph->valuestring[0] && strcmp(ph->valuestring, head_out) != 0)
      {
         /* Compute changelog: git log --oneline prev..current */
         char prev_range[192];
         snprintf(prev_range, sizeof(prev_range), "%s..%s", ph->valuestring, head_out);
         const char *log_argv[] = {"git",         "-C",  ws_root,    "log", "--oneline",
                                   "--no-merges", "-20", prev_range, NULL};
         char *log_out = NULL;
         rc = safe_exec_capture(log_argv, &log_out, 4096);

         /* Compute file diff stats */
         const char *stat_argv[] = {"git",      "-C", ws_root, "diff", "--stat", "--stat-width=60",
                                    prev_range, NULL};
         char *stat_out = NULL;
         int src = safe_exec_capture(stat_argv, &stat_out, 4096);

         /* Count commits */
         const char *count_argv[] = {"git", "-C", ws_root, "rev-list", "--count", prev_range, NULL};
         char *count_out = NULL;
         safe_exec_capture(count_argv, &count_out, 64);
         int commit_count = count_out ? atoi(count_out) : 0;

         if (rc == 0 && log_out && log_out[0])
         {
            cl->has_changelog = 1;
            session_changelog_append(cl->buf, sizeof(cl->buf), &cl_off, proj_name, commit_count,
                                     log_out, src == 0 ? stat_out : NULL);
         }

         free(log_out);
         free(stat_out);
         free(count_out);
      }
      free(head_out);
   }

   /* Save current heads for next session */
   {
      char *hj = cJSON_PrintUnformatted(new_heads);
      if (hj)
      {
         FILE *hf = fopen(heads_path, "w");
         if (hf)
         {
            fputs(hj, hf);
            fclose(hf);
         }
         free(hj);
      }
   }
   cJSON_Delete(new_heads);
   if (prev_heads)
      cJSON_Delete(prev_heads);

   /* Store changelog in working memory */
   if (cl->has_changelog)
   {
      cl->buf[sizeof(cl->buf) - 1] = '\0';
      db1_wm_set(sid, "session_changelog", cl->buf, "system", 0);
   }
}

/* Ensure .mcp.json + Claude Code trust exist in the CWD and each worktree path
 * so MCP-capable clients (including non-interactive delegates) get aimee tools. */
static void session_ensure_mcp_files(const session_state_t *state)
{
   char mcp_cwd[MAX_PATH_LEN];
   if (getcwd(mcp_cwd, sizeof(mcp_cwd)))
   {
      ensure_mcp_json(mcp_cwd);
      ensure_claude_code_trust(mcp_cwd);
   }
   /* Also place in each worktree path */
   for (int i = 0; i < state->worktree_count; i++)
   {
      if (state->worktrees[i].worktree_path[0])
      {
         ensure_mcp_json(state->worktrees[i].worktree_path);
         ensure_claude_code_trust(state->worktrees[i].worktree_path);
      }
   }
}

/* Build and emit the session brief (working dirs + context + changelog) to
 * `out`, and persist a copy to session-<sid>.brief.md for later audit. */
static void session_emit_brief(FILE *out, const char *sid, const char *client_cwd, int is_startup,
                               const session_state_t *state, int has_changelog,
                               const char *changelog_buf)
{
   char *brief = build_session_context(client_cwd[0] ? client_cwd : NULL);
   char *working_dirs = is_startup ? build_working_directories_text(state) : strdup("");

   /* Output: for hooks, print to stdout (text mode) */
   if (working_dirs && working_dirs[0])
      fputs(working_dirs, out);
   if (brief[0])
      fputs(brief, out);

   /* Output changelog */
   if (has_changelog)
      fprintf(out, "# Changes Since Last Session\n%s\n", changelog_buf);

   /* Persist the brief so operators can audit "what did aimee inject at session
    * start?" after the fact. Per-session file; ignore errors so no hook path
    * regresses on disk-full. */
   if ((working_dirs && working_dirs[0]) || brief[0] || has_changelog)
   {
      char brief_path[MAX_PATH_LEN];
      snprintf(brief_path, sizeof(brief_path), "%s/session-%s.brief.md", config_output_dir(), sid);
      FILE *bp = fopen(brief_path, "w");
      if (bp)
      {
         if (working_dirs && working_dirs[0])
            fputs(working_dirs, bp);
         if (brief[0])
            fputs(brief, bp);
         if (has_changelog)
            fprintf(bp, "# Changes Since Last Session\n%s\n", changelog_buf);
         fclose(bp);
      }
   }

   free(working_dirs);
   free(brief);
}

/* Background janitor: throttle via a timestamp file and touch it at most once
 * every 6 hours. (Worktree GC itself is now handled at session end.) */
static void session_run_janitor(void)
{
   char janitor_ts[MAX_PATH_LEN];
   snprintf(janitor_ts, sizeof(janitor_ts), "%s/janitor.ts", config_output_dir());
   int should_run = 0;
   struct stat jst;
   if (stat(janitor_ts, &jst) != 0)
      should_run = 1; /* never run before */
   else if (difftime(time(NULL), jst.st_mtime) > 6 * 3600)
      should_run = 1;

   if (should_run)
   {
      /* Touch the timestamp file first to prevent concurrent runs */
      FILE *tf = fopen(janitor_ts, "w");
      if (tf)
         fclose(tf);

      /* Worktree GC is no longer needed — sibling worktrees are visible to
       * users and cleaned up on session end via worktree_cleanup(). */
   }
}

void session_start_emit(app_ctx_t *ctx, const char *hook_input, FILE *out)
{
   if (!out)
      out = stdout;
   if (!hook_input)
      hook_input = "";

   /* Default to "startup" when the source is unknown. */
   int is_startup;
   char client_cwd[MAX_PATH_LEN];
   session_start_parse_hook(hook_input, &is_startup, client_cwd, sizeof(client_cwd));

   config_t cfg_buf;
   config_t *cfgp;
   if (ctx && ctx->cfg)
   {
      cfgp = ctx->cfg;
   }
   else
   {
      config_load(&cfg_buf);
      cfgp = &cfg_buf;
   }
   config_t cfg = *cfgp;

   /* Corruption detection, cap enforcement, and stale-session pruning only
    * run on "startup". On resume/compact the DB was already checked at
    * startup, the session counts don't change mid-session, and the heavy
    * cascade inside prune_stale_sessions (memory_scan_conversations,
    * anti-pattern extraction, rules_generate) would pile onto every compact
    * otherwise — a major source of CPU storms. */
   if (is_startup)
   {
      session_startup_db_check(&cfg);
      session_enforce_session_cap(&cfg);
      session_enforce_worktree_cap(&cfg);
   }

   /* Build session state: compute sibling worktree paths for workspaces.
    * Load existing state first to preserve user-set fields (session_mode, tdd_mode)
    * that must survive context compact/resume events. Then reset only the
    * fields that session-start owns (guardrail_mode, worktrees). */
   const char *sid = session_id();
   session_state_t state;
   session_state_load(&state, sid);
   /* Reset session-start-owned fields while preserving user settings */
   state.worktree_count = 0;
   state.hook_call_count = 0;
   state.orch_direct_edits = 0;
   state.orch_nudge_sent = 0;
   snprintf(state.guardrail_mode, sizeof(state.guardrail_mode), "%s", config_guardrail_mode(&cfg));

   /* Register sibling worktrees for configured workspaces and CWD's git repo. */
   session_register_worktrees(&cfg, client_cwd, sid, &state);

   /* Prepare isolated sibling checkouts (startup only). */
   if (is_startup)
      session_checkout_worktrees(&state, sid);

   /* Compute the changelog vs the previous session's HEADs and record any
    * projects needing a background main-index scan. */
   session_changelog_t cl;
   session_build_changelog(&state, sid, &cl);

   /* Persist state to DB1. */
   state.dirty = 1;
   session_state_force_save(&state, sid);

   session_ensure_mcp_files(&state);
   session_emit_brief(out, sid, client_cwd, is_startup, &state, cl.has_changelog, cl.buf);
   session_run_janitor();

   if (cl.index_count > 0)
      platform_hooks_background_reindex(cl.index_projects, cl.index_roots, cl.index_heads,
                                        cl.index_count);
}

/* session_brief_emit: the workspace-INDEPENDENT session brief for the remote
 * thin-client SessionStart path (Proposal 1 Phase 1 / session.brief_assemble).
 *
 * Runs only build_session_context — persona principles + learned Rules + key
 * facts — and deliberately NONE of session_start_emit's workspace side-effects
 * (no worktree checkout, no session_state persistence, no background reindex).
 * client_cwd is intentionally NOT passed: on a remote server the client's tree
 * is absent, and resolving paths from a client-supplied cwd would let a thin
 * client probe the server filesystem. The workspace-dependent half (local
 * .aimee-rules/AGENTS.md discovery) is assembled client-side in Phase 2.
 *
 * Endpoint-level never-empty floor: build_session_context already falls back to
 * persona/mode principles, but if a wholly-unconfigured server yields nothing,
 * emit a minimal neutral pointer block rather than an empty brief. */
void session_brief_emit(FILE *out)
{
   if (!out)
      out = stdout;
   char *brief = build_session_context(NULL);
   if (brief && brief[0])
      fputs(brief, out);
   else
      fputs("# Aimee Context\n"
            "- Use `aimee memory search <terms>` for prior project context.\n"
            "- Use `aimee index find <symbol>` for indexed code lookup.\n",
            out);
   free(brief);
}

/* CLI dispatch wrapper: read the hook payload from stdin and emit the brief
 * to stdout. Server callers reach session_start_emit() directly so they can
 * supply the payload they received over the wire and capture the output. */
void cmd_session_start(app_ctx_t *ctx, int argc, char **argv)
{
   (void)argv;
   (void)argc;

   char hook_input[4096];
   size_t hi_total = 0;
   if (platform_hooks_stdin_ready())
   {
      while (hi_total < sizeof(hook_input) - 1)
      {
         ssize_t n = read(STDIN_FILENO, hook_input + hi_total, sizeof(hook_input) - 1 - hi_total);
         if (n <= 0)
            break;
         hi_total += (size_t)n;
      }
   }
   hook_input[hi_total] = '\0';

   session_start_emit(ctx, hook_input, stdout);
}

/* --- cmd_launch --- */

void cmd_launch(app_ctx_t *ctx, int argc, char **argv)
{
   /* Always generate a fresh session ID for each launch.
    *
    * session_id() is keyed by PPID so that hooks and MCP servers (children
    * of a running Claude agent) can find their parent's session.  But for
    * a new launch we must NOT inherit a stale session-ppid-{PPID} file left
    * by a previous launch from the same parent process — that would put two
    * independent Claude sessions into the same worktree.
    *
    * Set the override BEFORE cmd_session_start so every downstream
    * session_id() call in this process uses the new unique ID. */
   {
      /* 10 random bytes -> 20 hex chars. The per-session worktree key uses the
       * first 16 chars (64 bits), so the id must carry at least that much
       * entropy; the old 4-byte (8-hex) id left the key at only 32 bits, where
       * independent launches collided onto a shared worktree. */
      unsigned char rnd[10];
      if (platform_random_bytes(rnd, sizeof(rnd)) != 0)
      {
         /* Fallback: seed an LCG from PID+time and emit one byte per step.
          * Advancing the state each byte (rather than shifting a single value)
          * keeps all bytes distinct without any out-of-range shift. */
         uint64_t mix = (uint64_t)getpid() * 2654435761u + (uint64_t)time(NULL);
         for (size_t i = 0; i < sizeof(rnd); i++)
         {
            mix = mix * 6364136223846793005ULL + 1442695040888963407ULL;
            rnd[i] = (unsigned char)(mix >> 56);
         }
      }
      char fresh_id[32];
      int off = 0;
      for (size_t i = 0; i < sizeof(rnd); i++)
         off += snprintf(fresh_id + off, sizeof(fresh_id) - off, "%02x", rnd[i]);
      session_id_set_override(fresh_id);
   }

   /* Run session-start (prints context to stdout) */
   cmd_session_start(ctx, argc, argv);

   /* Now emit a JSON metadata line with launch info for the client.
    * The client needs: provider, and the worktree path to chdir to. */
   config_t cfg_local;
   config_t *cfg_ptr = ctx->cfg;
   if (!cfg_ptr)
   {
      config_load(&cfg_local);
      cfg_ptr = &cfg_local;
   }
   config_t cfg = *cfg_ptr;

   /* Determine provider */
   const char *provider = "claude";
   if (cfg.provider[0])
      provider = cfg.provider;

   int use_builtin = 1;

   /* Determine worktree target for cwd */
   const char *launch_sid = session_id();
   session_state_t state;
   session_state_load(&state, launch_sid);

   /* Eagerly create sibling worktrees for all registered repos */
   {
      for (int i = 0; i < state.worktree_count; i++)
         worktree_create_sibling(state.worktrees[i].git_root, launch_sid, NULL);
   }

   char target_dir[MAX_PATH_LEN] = "";
   char cwd[MAX_PATH_LEN];
   if (getcwd(cwd, sizeof(cwd)) && state.worktree_count > 0)
   {
      /* If CWD is inside a tracked git root, compute the equivalent worktree path */
      const char *wt = worktree_for_cwd(&state, cwd);
      if (wt)
      {
         /* Compute the relative suffix within the git root */
         for (int i = 0; i < state.worktree_count; i++)
         {
            size_t rlen = strlen(state.worktrees[i].git_root);
            if (strncmp(cwd, state.worktrees[i].git_root, rlen) == 0 &&
                (cwd[rlen] == '/' || cwd[rlen] == '\0'))
            {
               const char *suffix = cwd + rlen;
               snprintf(target_dir, sizeof(target_dir), "%s%s", state.worktrees[i].worktree_path,
                        suffix);
               state.dirty = 1;
               session_state_save(&state, launch_sid);
               break;
            }
         }
      }
      else
      {
         /* CWD is not inside a tracked git root — check if CWD is a parent
          * of any tracked git root. This happens when aimee is launched from
          * a workspace directory (e.g. /home/user/dev) that contains tracked
          * repos (e.g. /home/user/dev/aimee). Without this fallback, the
          * worktree_cwd field is omitted from launch metadata and the agent
          * operates on the main repo instead of the worktree. */
         size_t cwd_len = strlen(cwd);
         for (int i = 0; i < state.worktree_count; i++)
         {
            const char *gr = state.worktrees[i].git_root;
            if (strncmp(gr, cwd, cwd_len) == 0 && (gr[cwd_len] == '/' || gr[cwd_len] == '\0'))
            {
               /* CWD is a parent of this git root. Map CWD to the
                * equivalent parent of the worktree path. E.g.:
                *   CWD      = /home/user/dev
                *   git_root = /home/user/dev/aimee
                *   worktree = /home/user/dev/.aimee-aimee-abc12345
                *   target   = /home/user/dev  (keep CWD, but now the
                *              agent knows the worktree mapping exists) */
               snprintf(target_dir, sizeof(target_dir), "%s", state.worktrees[i].worktree_path);
               state.dirty = 1;
               session_state_save(&state, launch_sid);
               break;
            }
         }
      }
   }

   /* Check if this session has a changelog in working memory */
   int launch_has_changelog = 0;
   {
      /* Ensure db1 is open for the wm_get lookup. Idempotent. */
      db1_init(cfg.db1_path);
      wm_entry_t wm_entry;
      if (db1_wm_get(session_id(), "session_changelog", &wm_entry) == 0)
         launch_has_changelog = 1;
   }

   if (cfg.autonomous)
      fprintf(stderr,
              "aimee: warning: autonomous mode is active — aimee guardrails are the sole safety "
              "boundary\n");

   /* Emit launch metadata as a JSON line with a known prefix */
   cJSON *meta = cJSON_CreateObject();
   cJSON_AddStringToObject(meta, "provider", provider);
   if (strcmp(provider, "claude") == 0 && cfg.claude_model[0])
      cJSON_AddStringToObject(meta, "model", cfg.claude_model);
   cJSON_AddBoolToObject(meta, "builtin", use_builtin);
   cJSON_AddStringToObject(meta, "session_id", session_id());
   if (cfg.autonomous)
      cJSON_AddBoolToObject(meta, "autonomous", 1);
   if (target_dir[0])
      cJSON_AddStringToObject(meta, "worktree_cwd", target_dir);
   if (launch_has_changelog)
      cJSON_AddBoolToObject(meta, "has_changelog", 1);
   char *json_str = cJSON_PrintUnformatted(meta);
   if (json_str)
   {
      printf("__LAUNCH__%s\n", json_str);
      free(json_str);
   }
   cJSON_Delete(meta);
}

/* --- cmd_wrapup --- */

/* Clean up worktrees for a session. Warns if unpushed commits exist. */
static void cleanup_worktrees(const session_state_t *state, const config_t *cfg, const char *sid)
{
   (void)cfg;
   if (state->worktree_count == 0)
      return;

   for (int i = 0; i < state->worktree_count; i++)
      worktree_cleanup(state->worktrees[i].git_root, sid, NULL);
}

void cmd_wrapup(app_ctx_t *ctx, int argc, char **argv)
{
   (void)argc;
   (void)argv;

   config_t cfg;
   config_load(&cfg);

   const char *sid = session_id();

   /* Load session state for worktree cleanup */
   session_state_t state;
   session_state_load(&state, sid);

   /* Clean up worktrees */
   cleanup_worktrees(&state, &cfg, sid);

   /* Run eval-to-behavior feedback loop — every helper below routes
    * through aimee-kb (kb_client_*), which auto-spawns the daemon if
    * needed, so no caller-side DB2 init check is required. */
   {
      /* Extract anti-patterns from feedback and failures */
      kb_client_anti_pattern_extract_from_feedback();
      kb_client_anti_pattern_extract_from_failures();

      /* Escalate anti-patterns with 5+ hits to hard directives */
      int escalated = kb_client_anti_pattern_escalate(5);

      /* Legacy monolithic command path. This file is not linked into the
       * shipped DB-free client; the server RPC port must split DB1 reads
       * from DB2 writes before this is routed through shipped surfaces. */
      int adjustments = eval_feedback_loop();
      if (adjustments < 0)
         adjustments = 0;

      /* Decay stale rule weights */

      int decayed = kb_client_rules_decay();

      /* Learn style preferences from feedback */
      kb_client_memory_learn_style();

      /* Record session outcome based on recent agent_log entries (DB1). */
      {
         int successes = 0, total = 0;
         if (db1_agent_log_session_outcome(sid, &successes, &total) == 0 && total > 0)
         {
            const char *outcome = (successes == total) ? "success"
                                  : (successes > 0)    ? "partial"
                                                       : "failure";
            (void)db1_server_session_set_outcome(sid, outcome);
         }
      }
      /* Legacy monolithic command path; typed server/kb RPCs are required
       * before this flow can be exposed through aimee-client again. */
      (void)trace_mine();

      if (!ctx->json_output && (escalated > 0 || adjustments > 0))
         fprintf(stderr, "Feedback loop: %d rule adjustments, %d anti-pattern escalations.\n",
                 adjustments, escalated);
      if (!ctx->json_output && (escalated > 0 || adjustments > 0 || decayed > 0))
         fprintf(stderr,
                 "Feedback loop: %d rule adjustments, %d anti-pattern escalations, %d rules "
                 "decayed.\n",
                 adjustments, escalated, decayed);
   }

   /* Delete session state (row + CASCADEd child tables) from DB1. */
   db1_session_state_delete(sid);

   /* Prune other stale sessions in background (non-blocking).
    * Done at exit so new session startup isn't impacted by old session cleanup. */
   platform_hooks_background_cleanup(&cfg);

   if (ctx->json_output)
      emit_ok_ctx(ctx->json_fields, ctx->response_profile);
   else
      fprintf(stderr, "Session cleaned up.\n");
}
