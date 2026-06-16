/* agent_coord.c: multi-agent coordination (planner/critic/worker), quorum voting, hard directives
 */
#include "aimee.h"
#include "db1.h"
#include "kb_client.h"
#include "agent_coord.h"
#include "agent_config.h"
#include "agent_exec.h"
#include "agent_tasks.h"
#include "dstr.h"
#include "cJSON.h"
#include "git_verify.h"
#include "log.h"
#include <ctype.h>

/* --- File Reference Resolution --- */

static int is_path_char(char c)
{
   return c != '\0' && c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '"' && c != '\'' &&
          c != ',' && c != ')' && c != '}' && c != ']' && c != '>' && c != '|';
}

static int path_within_root(const char *project_root, const char *abs_path)
{
   if (!project_root || !project_root[0] || !abs_path)
      return 0;
   size_t root_len = strlen(project_root);
   if (strncmp(abs_path, project_root, root_len) != 0)
      return 0;
   return abs_path[root_len] == '/' || abs_path[root_len] == '\0';
}

char *resolve_file_references(const char *prompt, const char *project_root)
{
   if (!prompt)
      return NULL;

   dstr_t out;
   dstr_init(&out);

   const char *p = prompt;
   int refs_resolved = 0;

   while (*p)
   {
      if (*p != '@')
      {
         dstr_append_char(&out, *p++);
         continue;
      }

      const char *path_start = p + 1;
      /* Not a file reference: end-of-string, a non-path char, or a doubled '@'
       * (a unified-diff hunk header "@@ -a,b +c,d @@"). Emit the '@' literally so
       * diffs/code carried in the prompt are never corrupted. */
      if (!*path_start || !is_path_char(*path_start) || *path_start == '@')
      {
         dstr_append_char(&out, *p++);
         continue;
      }

      const char *path_end = path_start;
      while (is_path_char(*path_end))
         path_end++;
      size_t path_len = (size_t)(path_end - path_start);
      if (path_len == 0 || path_len >= FILE_REF_PATH_MAX)
      {
         dstr_append_char(&out, *p++);
         continue;
      }

      char rel_path[FILE_REF_PATH_MAX];
      memcpy(rel_path, path_start, path_len);
      rel_path[path_len] = '\0';

      char abs_path[MAX_PATH_LEN];
      if (rel_path[0] == '/')
         snprintf(abs_path, sizeof(abs_path), "%s", rel_path);
      else if (project_root && project_root[0])
         snprintf(abs_path, sizeof(abs_path), "%s/%s", project_root, rel_path);
      else
         snprintf(abs_path, sizeof(abs_path), "%s", rel_path);

      /* Only an EXISTING, in-project file is treated as a reference. A @token that
       * escapes the root or doesn't resolve to a real file is emitted LITERALLY
       * and is NOT counted against the budget: prompts routinely carry diffs and
       * code whose '@' tokens (decorators like @property, emails, doc tags, diff
       * hunk headers) are not file references, and rewriting them as markers
       * corrupted the content and exhausted the ref budget (the roundtable
       * "sandbox-blind" failure). */
      size_t abs_len = strlen(abs_path);
      int escapes = (strstr(abs_path, "/../") != NULL ||
                     (abs_len >= 3 && strcmp(abs_path + abs_len - 3, "/..") == 0));
      int in_root = !(project_root && project_root[0]) || path_within_root(project_root, abs_path);
      FILE *f = (!escapes && in_root) ? fopen(abs_path, "r") : NULL;
      if (!f)
      {
         dstr_append_char(&out, '@');
         dstr_append(&out, rel_path, path_len);
         p = path_end;
         continue;
      }

      /* A real file past the per-prompt budget: explicit marker (diff/code never
       * reaches here — its non-file @tokens are left literal above). */
      if (refs_resolved >= FILE_REF_MAX_REFS)
      {
         dstr_appendf(&out, "[file reference limit reached: @%s left unresolved]", rel_path);
         fclose(f);
         p = path_end;
         continue;
      }

      char *content = malloc(FILE_REF_MAX_SIZE + 1);
      if (!content)
      {
         fclose(f);
         dstr_append_char(&out, '@');
         dstr_append(&out, rel_path, path_len);
         p = path_end;
         continue;
      }

      size_t bytes_read = fread(content, 1, FILE_REF_MAX_SIZE, f);
      int truncated = !feof(f);
      fclose(f);
      content[bytes_read] = '\0';

      dstr_appendf(&out, "--- @%s ---\n", rel_path);
      dstr_append(&out, content, bytes_read);
      if (truncated)
         dstr_append_str(&out, "\n[TRUNCATED]");
      dstr_append_str(&out, "\n---");
      free(content);

      p = path_end;
      refs_resolved++;
   }

   char *result = dstr_steal(&out);
   if (!result)
      result = strdup("");
   return result;
}

/* --- Delegate Token Budget --- */

/* Estimate token count: approximately 4 chars per token (conservative). */
static int estimate_tokens(const char *s)
{
   if (!s)
      return 0;
   return (int)(strlen(s) / 4);
}

/* Remove a section from a prompt string identified by start/end pointers.
 * Replaces the section with a [TRUNCATED] marker. Returns a new string (caller frees). */
static char *remove_section(const char *prompt, size_t total_len, const char *sec_start,
                            const char *sec_end, const char *label)
{
   dstr_t out;
   dstr_init(&out);
   dstr_append(&out, prompt, (size_t)(sec_start - prompt));
   dstr_appendf(&out, "\n[TRUNCATED: %s removed to fit token budget]\n", label);
   if (sec_end < prompt + total_len)
      dstr_append(&out, sec_end, (size_t)(prompt + total_len - sec_end));
   char *r = dstr_steal(&out);
   return r;
}

/* Find the end of a "--- @path ---\n...\n---" file reference block. */
static const char *find_file_ref_end(const char *start)
{
   /* Skip past the opening "--- @..." line */
   const char *p = strchr(start + 1, '\n');
   if (!p)
      return start + strlen(start);
   p++;
   /* Find the closing "---" line */
   while (*p)
   {
      if (p[0] == '-' && p[1] == '-' && p[2] == '-' && (p[3] == '\n' || p[3] == '\0'))
         return p + 3 + (p[3] == '\n' ? 1 : 0);
      const char *nl = strchr(p, '\n');
      if (!nl)
         break;
      p = nl + 1;
   }
   return start + strlen(start);
}

char *delegate_prompt_limit(const char *prompt, int token_budget)
{
   if (!prompt)
      return NULL;

   if (token_budget <= 0)
      token_budget = DELEGATE_TOKEN_BUDGET_DEFAULT;

   if (estimate_tokens(prompt) <= token_budget)
      return safe_strdup(prompt);

   size_t budget_chars = (size_t)token_budget * 4;
   size_t min_task_chars = (size_t)DELEGATE_TOKEN_BUDGET_MIN_TASK * 4;

   /* Ensure budget is at least the minimum task size. */
   if (budget_chars < min_task_chars)
      budget_chars = min_task_chars;

   /* Work on a mutable copy so we can iteratively remove sections. */
   char *working = safe_strdup(prompt);
   if (!working)
      return NULL;

   /* --- Phase 1: Remove file reference blocks ("--- @path ---\n...\n---") --- */
   while (estimate_tokens(working) > token_budget)
   {
      /* Find the last file reference block (remove from end first). */
      const char *last_ref = NULL;
      const char *search = working;
      while ((search = strstr(search, "\n--- @")) != NULL)
      {
         last_ref = search + 1; /* point to "--- @" */
         search += 6;
      }
      if (!last_ref)
         break;

      const char *ref_end = find_file_ref_end(last_ref);
      size_t wlen = strlen(working);
      char *next = remove_section(working, wlen, last_ref, ref_end, "injected file content");
      free(working);
      working = next;
      if (!working)
         return safe_strdup(prompt);
      aimee_log(LOG_INFO, "delegate",
                "shed: removed @file reference block to fit token budget (%d tokens)",
                token_budget);
   }

   if (estimate_tokens(working) <= token_budget)
      return working;

   /* --- Phase 2: Remove context/background sections --- */
   static const char *ctx_markers[] = {"# Pre-loaded File Contents\n",
                                       "# Pre-loaded Directory Contents", "# Context Files\n",
                                       "# Relevant Context\n", NULL};
   for (int i = 0; ctx_markers[i] && estimate_tokens(working) > token_budget; i++)
   {
      char *sec = strstr(working, ctx_markers[i]);
      if (!sec)
         continue;

      /* Find the end: next markdown heading (any level) or end of string */
      const char *sec_end = sec + strlen(ctx_markers[i]);
      while (*sec_end)
      {
         if (sec_end[0] == '\n' && sec_end[1] == '#')
         {
            sec_end++; /* point to the '#' of the next section */
            break;
         }
         sec_end++;
      }

      size_t wlen = strlen(working);
      char *next = remove_section(working, wlen, sec, sec_end, "context section");
      free(working);
      working = next;
      if (!working)
         return safe_strdup(prompt);
      aimee_log(LOG_INFO, "delegate",
                "shed: removed context section '%s' to fit token budget (%d tokens)",
                ctx_markers[i], token_budget);
   }

   if (estimate_tokens(working) <= token_budget)
      return working;

   /* --- Phase 3: Truncate skill content --- */
   const char *skill_marker = strstr(working, "\n### ACTIVE SKILL:");
   if (skill_marker)
   {
      size_t base_len = (size_t)(skill_marker - working);
      if (base_len < budget_chars)
      {
         size_t skill_budget = budget_chars - base_len;
         size_t wlen = strlen(working);
         size_t skill_len = wlen - base_len;
         if (skill_len > skill_budget)
         {
            dstr_t out;
            dstr_init(&out);
            dstr_append(&out, working, base_len);
            dstr_append(&out, skill_marker, skill_budget);
            dstr_append_str(&out, "\n[TRUNCATED: skill context exceeded token budget]");
            free(working);
            aimee_log(LOG_INFO, "delegate",
                      "shed: truncated skill block to fit token budget (%d tokens)", token_budget);
            char *r = dstr_steal(&out);
            return r ? r : safe_strdup(prompt);
         }
      }
   }

   if (estimate_tokens(working) <= token_budget)
      return working;

   /* --- Phase 4: Generic tail truncation — keep first budget_chars of content --- */
   {
      dstr_t out;
      dstr_init(&out);
      size_t wlen = strlen(working);
      size_t keep = budget_chars < wlen ? budget_chars : wlen;
      dstr_append(&out, working, keep);
      dstr_append_str(&out, "\n[TRUNCATED: system prompt exceeded token budget]");
      free(working);
      aimee_log(LOG_WARN, "delegate",
                "shed: system prompt truncated to fit token budget (%d tokens) — "
                "core content may be incomplete",
                token_budget);
      char *r = dstr_steal(&out);
      return r ? r : safe_strdup(prompt);
   }
}

int delegate_token_budget_load(const char *project_root, const char *role)
{
   /* Reads from the global per-project config:
    *   ~/.config/aimee/projects/<name>/project.yaml
    * Worktrees and main checkout share one file (resolved via the
    * canonical main repo root in project_yaml_path).
    *
    * Supports per-role overrides: "delegate_token_budget_<role>: N"
    * takes precedence over "delegate_token_budget: N". */
   char path[MAX_PATH_LEN];
   if (project_yaml_path(project_root, path, sizeof(path)) != 0)
      return DELEGATE_TOKEN_BUDGET_DEFAULT;

   FILE *f = fopen(path, "r");
   if (!f)
      return DELEGATE_TOKEN_BUDGET_DEFAULT;

   /* Build the role-specific key if a role was provided */
   char role_key[128] = "";
   if (role && role[0])
      snprintf(role_key, sizeof(role_key), "delegate_token_budget_%s:", role);

   char line[256];
   int budget = DELEGATE_TOKEN_BUDGET_DEFAULT;
   int found_role = 0;
   while (fgets(line, sizeof(line), f))
   {
      /* Check role-specific key first (takes priority) */
      if (role_key[0] && strncmp(line, role_key, strlen(role_key)) == 0)
      {
         int val = atoi(line + strlen(role_key));
         if (val > 0)
         {
            budget = val;
            found_role = 1;
         }
      }
      /* Fall back to global key */
      else if (!found_role && strncmp(line, "delegate_token_budget:", 22) == 0)
      {
         int val = atoi(line + 22);
         if (val > 0)
            budget = val;
      }
   }
   fclose(f);
   return budget;
}

/* --- Multi-Agent Coordination (Feature 8) --- */

int agent_coordinate(agent_config_t *cfg, const char *task, agent_result_t *out)
{
   if (!cfg || !task || !out)
      return -1;

   memset(out, 0, sizeof(*out));

   /* Phase 1: Planner creates a plan */
   char planner_prompt[4096];
   snprintf(planner_prompt, sizeof(planner_prompt),
            "Create a step-by-step plan as a JSON array for this task. "
            "Each step should have: action (shell command), precondition (text), "
            "success_predicate (text to find in output), rollback (shell command or empty). "
            "Task: %s\n\nRespond ONLY with a JSON array, no other text.",
            task);

   agent_result_t planner_result;
   int rc = agent_run(cfg, "execute", NULL, planner_prompt, 0, &planner_result);
   if (rc != 0 || !planner_result.response)
   {
      snprintf(out->error, sizeof(out->error), "planner failed: %s", planner_result.error);
      free(planner_result.response);
      return -1;
   }

   /* Phase 2: Critic reviews the plan */
   char critic_prompt[8192];
   snprintf(critic_prompt, sizeof(critic_prompt),
            "Review this plan for a task. Flag any risks, missing steps, or unsafe commands. "
            "If the plan is good, respond with APPROVED. If not, respond with REJECTED followed "
            "by your concerns.\n\nTask: %s\n\nPlan:\n%s",
            task, planner_result.response);

   agent_result_t critic_result;
   rc = agent_run(cfg, "execute", NULL, critic_prompt, 0, &critic_result);

   int approved = 1;
   if (rc == 0 && critic_result.response)
   {
      if (strstr(critic_result.response, "REJECTED") != NULL)
         approved = 0;
   }
   free(critic_result.response);

   if (!approved)
   {
      snprintf(out->error, sizeof(out->error), "plan rejected by critic");
      out->response = planner_result.response;
      return -1;
   }

   /* Phase 3: Worker executes the plan */
   cJSON *plan_json = cJSON_Parse(planner_result.response);
   free(planner_result.response);

   if (!plan_json || !cJSON_IsArray(plan_json))
   {
      /* If the planner didn't return valid JSON, try direct execution */
      cJSON_Delete(plan_json);
      return agent_run(cfg, "execute", NULL, task, 0, out);
   }

   int plan_id = db1_execution_plan_create(cfg->default_agent, task, plan_json);
   cJSON_Delete(plan_json);

   if (plan_id < 0)
      return agent_run(cfg, "execute", NULL, task, 0, out);

   plan_t plan;
   if (db1_execution_plan_get(plan_id, &plan) != 0)
      return -1;

   agent_t *ag = agent_route(cfg, "execute");
   int timeout = ag ? ag->timeout_ms : AGENT_DEFAULT_TIMEOUT_MS;
   rc = agent_plan_execute(&plan, ag, timeout);

   /* Build response from plan outputs */
   size_t resp_len = 1024;
   for (int i = 0; i < plan.step_count; i++)
      resp_len += strlen(plan.steps[i].output) + 128;
   out->response = malloc(resp_len);
   if (out->response)
   {
      size_t pos = 0;
      for (int i = 0; i < plan.step_count; i++)
      {
         static const char *status_names[] = {"pending", "running", "done", "failed",
                                              "rolled_back"};
         const char *sn = (plan.steps[i].status >= 0 && plan.steps[i].status <= 4)
                              ? status_names[plan.steps[i].status]
                              : "unknown";
         pos += (size_t)snprintf(out->response + pos, resp_len - pos, "Step %d [%s]: %s\n%s\n\n",
                                 i + 1, sn, plan.steps[i].action, plan.steps[i].output);
      }
   }
   out->success = (rc == 0);
   snprintf(out->agent_name, MAX_AGENT_NAME, "%s", plan.agent_name);
   return rc;
}

int agent_vote(agent_config_t *cfg, const char *role, const char *prompt, int n_voters,
               agent_result_t *out)
{
   if (!cfg || !prompt || !out || n_voters <= 0)
      return -1;

   memset(out, 0, sizeof(*out));

   if (n_voters > AGENT_MAX_COORD_AGENTS)
      n_voters = AGENT_MAX_COORD_AGENTS;

   agent_task_t tasks[AGENT_MAX_COORD_AGENTS];
   agent_result_t results[AGENT_MAX_COORD_AGENTS];

   for (int i = 0; i < n_voters; i++)
   {
      tasks[i].role = role;
      tasks[i].system_prompt = NULL;
      tasks[i].user_prompt = prompt;
      tasks[i].max_tokens = 0;
      tasks[i].temperature = 0.3 + (0.1 * i); /* slight variation */
   }

   int successes = agent_run_parallel(cfg, tasks, n_voters, results, 0 /* no deadline */);
   if (successes == 0)
   {
      snprintf(out->error, sizeof(out->error), "all voters failed");
      return -1;
   }

   /* Simple majority: pick the most common response (by first 200 chars) */
   int best_idx = -1;
   int best_count = 0;
   for (int i = 0; i < n_voters; i++)
   {
      if (!results[i].success || !results[i].response)
         continue;
      int count = 0;
      for (int j = 0; j < n_voters; j++)
      {
         if (!results[j].success || !results[j].response)
            continue;
         if (strncmp(results[i].response, results[j].response, 200) == 0)
            count++;
      }
      if (count > best_count)
      {
         best_count = count;
         best_idx = i;
      }
   }

   if (best_idx >= 0)
   {
      out->response = results[best_idx].response;
      results[best_idx].response = NULL; /* don't free, transferred to out */
      out->success = 1;
      snprintf(out->agent_name, MAX_AGENT_NAME, "%s", results[best_idx].agent_name);
      out->confidence = (best_count * 100) / n_voters;
   }

   /* Free remaining results */
   for (int i = 0; i < n_voters; i++)
      free(results[i].response);

   return out->success ? 0 : -1;
}

/* --- Hard Directive Enforcement (Feature 15) --- */

int directive_check_tool(const char *tool_name, const char *args_json, char *reason_out,
                         size_t reason_len)
{
   if (!tool_name)
      return 0;

   /* Hard directives live in DB2 (owned by aimee-kb).  The kb-side
    * rules.list RPC returns rules ordered by weight DESC, so the
    * highest-priority directive is examined first. */
   rule_t rules[64];
   int rcount = kb_client_rules_list(rules, (int)(sizeof(rules) / sizeof(rules[0])));
   if (rcount <= 0)
      return 0;

   /* Parse command from args */
   char command[4096] = {0};
   if (args_json)
   {
      cJSON *args = cJSON_Parse(args_json);
      if (args)
      {
         cJSON *cmd = cJSON_GetObjectItem(args, "command");
         if (cmd && cJSON_IsString(cmd))
            snprintf(command, sizeof(command), "%s", cmd->valuestring);
         cJSON *path = cJSON_GetObjectItem(args, "path");
         if (path && cJSON_IsString(path))
            snprintf(command, sizeof(command), "%s", path->valuestring);
         cJSON_Delete(args);
      }
   }

   for (int i = 0; i < rcount; i++)
   {
      if (strcmp(rules[i].directive_type, "hard") != 0 || rules[i].weight <= 0)
         continue;
      const char *directive = rules[i].description;
      if (!directive || !directive[0])
         continue;

      /* Keyword matching: check if the tool call would violate the directive.
       * Extract keywords from the directive and check against the command. */

      /* "Never push to main" -> check for "push" + "main" */
      /* "Never delete production" -> check for "delete" + "production" */
      char lower_directive[1024];
      size_t dlen = strlen(directive);
      if (dlen >= sizeof(lower_directive))
         dlen = sizeof(lower_directive) - 1;
      for (size_t j = 0; j < dlen; j++)
         lower_directive[j] = (char)tolower((unsigned char)directive[j]);
      lower_directive[dlen] = '\0';

      char lower_command[4096];
      size_t clen = strlen(command);
      if (clen >= sizeof(lower_command))
         clen = sizeof(lower_command) - 1;
      for (size_t j = 0; j < clen; j++)
         lower_command[j] = (char)tolower((unsigned char)command[j]);
      lower_command[clen] = '\0';

      /* Check for "never" directives with keyword overlap */
      if (strstr(lower_directive, "never") || strstr(lower_directive, "must not") ||
          strstr(lower_directive, "do not"))
      {
         /* Extract significant words (>3 chars) from directive and check command */
         char *dp = lower_directive;
         char word[64];
         int matches = 0;
         int words_checked = 0;

         while (*dp)
         {
            /* Skip non-alpha */
            while (*dp && !isalpha((unsigned char)*dp))
               dp++;
            if (!*dp)
               break;

            /* Extract word */
            int wi = 0;
            while (*dp && isalpha((unsigned char)*dp) && wi < 63)
               word[wi++] = *dp++;
            word[wi] = '\0';

            /* Skip common words */
            if (strcmp(word, "never") == 0 || strcmp(word, "must") == 0 ||
                strcmp(word, "not") == 0 || strcmp(word, "the") == 0 || strcmp(word, "and") == 0 ||
                strcmp(word, "for") == 0 || strcmp(word, "this") == 0 ||
                strcmp(word, "that") == 0 || wi <= 3)
               continue;

            words_checked++;
            if (strstr(lower_command, word))
               matches++;
         }

         /* If more than half the significant words match, likely violation */
         if (words_checked > 0 && matches > 0 && matches * 2 >= words_checked)
         {
            snprintf(reason_out, reason_len, "hard directive violation: %s", directive);
            return -1;
         }
      }
   }

   return 0;
}
