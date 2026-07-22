/* delegate_prompt.c: shared prompt planning for delegate CLI prompt sources. */
#include "cmd_agent_delegate_impl.h"
#include "agent_config.h"
#include "agent_coord.h"
#include "config.h"
#include "delegate_role.h"
#include "kb_client.h"
#include "log.h"
#include "persona.h"
#include "role_templates.h"
#include "util.h"
#include <sys/stat.h>
#include <ctype.h>
#include <unistd.h>

int delegate_resolve_prompt_inputs(const char *cli_prompt, const char *file_prompt,
                                   delegate_prompt_plan_t *out)
{
   const char *fallback_task = "Work from the user prompt provided below.";

   if (!out)
      return -1;

   memset(out, 0, sizeof(*out));

   if (file_prompt && file_prompt[0])
   {
      if (cli_prompt && cli_prompt[0])
      {
         size_t cap = strlen(cli_prompt) + strlen(file_prompt) + 32;
         out->owned_user_prompt = malloc(cap);
         if (!out->owned_user_prompt)
            return -1;
         snprintf(out->owned_user_prompt, cap, "%s\n\n# Prompt File\n%s", cli_prompt, file_prompt);
         out->task_prompt = cli_prompt;
         out->user_prompt = out->owned_user_prompt;
         return 0;
      }

      out->task_prompt = fallback_task;
      out->user_prompt = file_prompt;
      return 0;
   }

   if (cli_prompt && cli_prompt[0])
   {
      out->task_prompt = cli_prompt;
      out->user_prompt = cli_prompt;
      return 0;
   }

   return -1;
}

static void handoff_set_error(delegate_handoff_validation_t *out, const char *msg)
{
   if (!out)
      return;
   snprintf(out->error, sizeof(out->error), "%s", msg ? msg : "invalid handoff");
   snprintf(out->status, sizeof(out->status), "%s", "needs_supervisor_review");
   out->needs_supervisor_review = 1;
}

static int handoff_status_allowed(const char *status)
{
   return status && (strcmp(status, "done") == 0 || strcmp(status, "partial") == 0 ||
                     strcmp(status, "blocked") == 0 || strcmp(status, "failed") == 0);
}

static int json_array_string_count(cJSON *arr)
{
   if (!cJSON_IsArray(arr))
      return 0;
   int count = 0;
   cJSON *item;
   cJSON_ArrayForEach(item, arr)
   {
      if (cJSON_IsString(item) && item->valuestring[0])
         count++;
   }
   return count;
}

static int json_array_entry_count(cJSON *arr)
{
   return cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : 0;
}

static int json_array_contains_string(cJSON *arr, const char *needle)
{
   if (!cJSON_IsArray(arr) || !needle || !needle[0])
      return 0;
   cJSON *item;
   cJSON_ArrayForEach(item, arr)
   {
      if (cJSON_IsString(item) && strcmp(item->valuestring, needle) == 0)
         return 1;
   }
   return 0;
}

static int handoff_passed_test_count(cJSON *tests)
{
   if (!cJSON_IsArray(tests))
      return 0;
   int passed = 0;
   cJSON *item;
   cJSON_ArrayForEach(item, tests)
   {
      if (!cJSON_IsObject(item))
         continue;
      cJSON *status = cJSON_GetObjectItemCaseSensitive(item, "status");
      if (cJSON_IsString(status) &&
          (strcmp(status->valuestring, "passed") == 0 || strcmp(status->valuestring, "pass") == 0))
         passed++;
   }
   return passed;
}

static int handoff_outside_owned_count(cJSON *changed, cJSON *outside, cJSON *owned)
{
   int count = json_array_string_count(outside);
   if (!cJSON_IsArray(changed) || !cJSON_IsArray(owned) || cJSON_GetArraySize(owned) == 0)
      return count;

   cJSON *item;
   cJSON_ArrayForEach(item, changed)
   {
      if (!cJSON_IsString(item) || !item->valuestring[0])
         continue;
      if (!json_array_contains_string(owned, item->valuestring) &&
          !json_array_contains_string(outside, item->valuestring))
         count++;
   }
   return count;
}

static int handoff_string_blank(const char *s)
{
   if (!s)
      return 1;
   while (*s)
   {
      if (*s != ' ' && *s != '\t' && *s != '\n' && *s != '\r')
         return 0;
      s++;
   }
   return 1;
}

char *delegate_handoff_append_contract(const char *prompt, const char *packet_id)
{
   if (!prompt)
      prompt = "";

   static const char *contract =
       "\n\n# Structured Delegate Handoff Contract\n"
       "Return only valid JSON matching delegate_result_v1. Do not include markdown.\n"
       "Required top-level fields: schema_version, status, changed_files, tests, summary.\n"
       "Optional top-level fields include supervisor_actions, commands_run, "
       "outside_ownership_touches, risks, and packet_id.\n"
       "Use schema_version=\"delegate_result_v1\".\n"
       "Allowed status values: done, partial, blocked, failed.\n"
       "If blocked, use status=\"blocked\" and explain the blocker in supervisor_actions.\n"
       "If you touched files outside owned_files, list them in outside_ownership_touches.\n"
       "For status=\"done\", include at least one tests[] entry with status=\"passed\".\n";

   size_t cap = strlen(prompt) + strlen(contract) + 160;
   if (packet_id && packet_id[0])
      cap += strlen(packet_id);
   char *out = malloc(cap);
   if (!out)
      return NULL;
   if (packet_id && packet_id[0])
      snprintf(out, cap, "%s%sUse packet_id=\"%s\".\n", prompt, contract, packet_id);
   else
      snprintf(out, cap, "%s%s", prompt, contract);
   return out;
}

char *delegate_handoff_repair_prompt(const char *previous_response, const char *validation_error)
{
   const char *prev = previous_response ? previous_response : "";
   const char *err = validation_error && validation_error[0] ? validation_error : "invalid JSON";
   static const char *prefix =
       "Your previous delegate handoff was invalid. Repair it now.\n"
       "Return only valid JSON matching delegate_result_v1. Do not include markdown.\n"
       "Validation error: ";
   static const char *middle = "\n\nPrevious response:\n";
   size_t cap = strlen(prefix) + strlen(err) + strlen(middle) + strlen(prev) + 2;
   char *out = malloc(cap);
   if (!out)
      return NULL;
   snprintf(out, cap, "%s%s%s%s", prefix, err, middle, prev);
   return out;
}

int delegate_handoff_validate_text(const char *text, const char *owned_files_json,
                                   int require_verification, delegate_handoff_validation_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof(*out));
   snprintf(out->status, sizeof(out->status), "%s", "needs_supervisor_review");

   if (!text || !text[0])
   {
      handoff_set_error(out, "empty delegate handoff");
      return -1;
   }

   cJSON *root = cJSON_Parse(text);
   if (!cJSON_IsObject(root))
   {
      cJSON_Delete(root);
      handoff_set_error(out, "handoff is not valid JSON object");
      return -1;
   }

   cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
   if (!cJSON_IsString(schema) || strcmp(schema->valuestring, "delegate_result_v1") != 0)
   {
      cJSON_Delete(root);
      handoff_set_error(out, "missing schema_version delegate_result_v1");
      return -1;
   }

   cJSON *status = cJSON_GetObjectItemCaseSensitive(root, "status");
   if (!cJSON_IsString(status) || !handoff_status_allowed(status->valuestring))
   {
      cJSON_Delete(root);
      handoff_set_error(out, "invalid handoff status");
      return -1;
   }
   snprintf(out->raw_status, sizeof(out->raw_status), "%s", status->valuestring);
   snprintf(out->status, sizeof(out->status), "%s", status->valuestring);

   cJSON *changed = cJSON_GetObjectItemCaseSensitive(root, "changed_files");
   cJSON *tests = cJSON_GetObjectItemCaseSensitive(root, "tests");
   cJSON *supervisor = cJSON_GetObjectItemCaseSensitive(root, "supervisor_actions");
   cJSON *summary = cJSON_GetObjectItemCaseSensitive(root, "summary");
   if (!cJSON_IsArray(changed) || !cJSON_IsArray(tests) ||
       (supervisor && !cJSON_IsArray(supervisor)) || !cJSON_IsString(summary) ||
       handoff_string_blank(summary->valuestring))
   {
      cJSON_Delete(root);
      handoff_set_error(out, "handoff missing required fields");
      return -1;
   }

   cJSON *commands = cJSON_GetObjectItemCaseSensitive(root, "commands_run");
   cJSON *outside = cJSON_GetObjectItemCaseSensitive(root, "outside_ownership_touches");
   out->changed_files_count = json_array_string_count(changed);
   out->commands_run = json_array_entry_count(commands);
   out->passed_tests = handoff_passed_test_count(tests);

   cJSON *owned = owned_files_json && owned_files_json[0] ? cJSON_Parse(owned_files_json) : NULL;
   out->outside_ownership_count = handoff_outside_owned_count(changed, outside, owned);
   cJSON_Delete(owned);

   out->valid = 1;
   if (out->outside_ownership_count > 0)
   {
      snprintf(out->status, sizeof(out->status), "%s", "needs_supervisor_review");
      snprintf(out->error, sizeof(out->error), "%s", "handoff touched files outside owned_files");
      out->needs_supervisor_review = 1;
   }
   else if (strcmp(out->raw_status, "done") == 0 && require_verification && out->passed_tests == 0)
   {
      snprintf(out->status, sizeof(out->status), "%s", "partial");
      snprintf(out->error, sizeof(out->error), "%s",
               "status=done without passed focused verification; downgraded to partial");
      out->done_without_verification = 1;
   }

   cJSON_Delete(root);
   return 0;
}

void delegate_handoff_add_validation_json(cJSON *obj, const delegate_handoff_validation_t *v)
{
   if (!obj || !v)
      return;
   cJSON_AddBoolToObject(obj, "handoff_valid", v->valid);
   cJSON_AddStringToObject(obj, "handoff_status", v->status);
   if (v->raw_status[0])
      cJSON_AddStringToObject(obj, "handoff_raw_status", v->raw_status);
   if (v->error[0])
      cJSON_AddStringToObject(obj, "handoff_error", v->error);
   cJSON_AddBoolToObject(obj, "handoff_repair_attempted", v->repair_attempted);
   cJSON_AddBoolToObject(obj, "handoff_needs_supervisor_review", v->needs_supervisor_review);
   cJSON_AddNumberToObject(obj, "handoff_changed_files", v->changed_files_count);
   cJSON_AddNumberToObject(obj, "handoff_outside_ownership_touches", v->outside_ownership_count);
   cJSON_AddNumberToObject(obj, "handoff_passed_tests", v->passed_tests);
   cJSON_AddBoolToObject(obj, "handoff_done_without_verification", v->done_without_verification);
}

/* ---- Named-file drift detection ---- */

static const char *const drift_src_exts[] = {
    ".c",  ".h",    ".cpp", ".cc",   ".cxx",  ".py", ".js",  ".ts", ".go",   ".rs",  ".java", ".rb",
    ".sh", ".yaml", ".yml", ".json", ".toml", ".md", ".sql", ".mk", ".conf", ".cfg", NULL};

static int drift_is_src_extension(const char *ext)
{
   for (int i = 0; drift_src_exts[i]; i++)
      if (strcmp(ext, drift_src_exts[i]) == 0)
         return 1;
   return 0;
}

static int drift_path_char(char c)
{
   return isalnum((unsigned char)c) || c == '.' || c == '/' || c == '_' || c == '-';
}

static size_t drift_task_surface_len(const char *prompt)
{
   if (!prompt)
      return 0;

   size_t limit = strlen(prompt);
   static const char *const markers[] = {
       "\n\n# Prompt File\n",
       "\n# Prompt File\n",
       "\n\n---\n## Parent Worktree Diff Evidence\n",
       "\n---\n## Parent Worktree Diff Evidence\n",
       "\n\n---\n## Validation Evidence Bundle\n",
       "\n---\n## Validation Evidence Bundle\n",
       NULL,
   };

   for (int i = 0; markers[i]; i++)
   {
      const char *marker = strstr(prompt, markers[i]);
      if (marker)
      {
         size_t pos = (size_t)(marker - prompt);
         if (pos < limit)
            limit = pos;
      }
   }

   return limit;
}

/* Briefs commonly say things like "Do NOT touch src/agent_jobs.c" or
 * "skip src/foo.c — it is owned by another delegate". Treating those
 * negated mentions as required outputs leads to spurious "named file was
 * not created" warnings even on a successful run. We scan backwards from
 * the path token to the start of the current line looking for negation
 * cues; if any matches, the path is dropped. The heuristic is
 * intentionally conservative — false positives here just mean the
 * drift check skips a path it should have flagged, which is far less
 * noisy than the current false-negative pattern. */
static int drift_path_is_negated(const char *prompt, const char *path_start)
{
   const char *line_start = path_start;
   while (line_start > prompt && line_start[-1] != '\n')
      line_start--;

   size_t span = (size_t)(path_start - line_start);
   if (span == 0)
      return 0;

   char buf[256];
   if (span >= sizeof(buf))
      span = sizeof(buf) - 1;
   memcpy(buf, path_start - span, span);
   buf[span] = '\0';
   for (size_t i = 0; i < span; i++)
      buf[i] = (char)tolower((unsigned char)buf[i]);

   static const char *negations[] = {
       "do not ",     "don't ",     "does not ",   "doesn't ",   "must not ",    "mustn't ",
       "cannot ",     "can't ",     "should not ", "shouldn't ", "will not ",    "won't ",
       "skip ",       "off-limits", "off limits",  "no touch",   "do not touch", "not touch",
       "leave alone", "ignore ",    NULL};
   for (int i = 0; negations[i]; i++)
      if (strstr(buf, negations[i]))
         return 1;

   return 0;
}

/* Briefs frequently embed an illustrative JSON schema or example
 * (e.g. `"owned_files":["src/x.c"]`) to show a delegate the *shape* of its
 * output. Those quoted paths are documentation, not real output targets, and
 * scraping them produced spurious "named file was not created" failures. A path
 * is treated as such an example value when it is a double-quoted token whose
 * enclosing context is a JSON value position — the opening quote follows (past
 * whitespace) a '[', ',', or ':'. Prose instructions like `create "src/foo.c"`
 * are preserved because the char before the opening quote is a word/space, not a
 * JSON structural delimiter. Conservatively dropping here matches the rest of
 * this check: a missed flag is far quieter than a false failure. */
static int drift_path_is_json_example_value(const char *prompt, const char *tok_start,
                                            const char *tok_end)
{
   if (tok_start <= prompt || tok_start[-1] != '"')
      return 0;
   if (*tok_end != '"')
      return 0;
   const char *b = tok_start - 2; /* char before the opening quote */
   while (b > prompt && (*b == ' ' || *b == '\t' || *b == '\n' || *b == '\r'))
      b--;
   if (b < prompt)
      return 0;
   return (*b == '[' || *b == ',' || *b == ':');
}

int delegate_extract_named_paths(const char *prompt, char paths[][DELEGATE_DRIFT_PATH_MAX],
                                 int max_paths)
{
   if (!prompt || !paths || max_paths <= 0)
      return 0;

   int count = 0;
   const char *p = prompt;
   const char *end = prompt + drift_task_surface_len(prompt);

   while (p < end && *p && count < max_paths)
   {
      if (!drift_path_char(*p))
      {
         p++;
         continue;
      }

      const char *start = p;
      while (p < end && drift_path_char(*p))
         p++;
      size_t len = (size_t)(p - start);
      while (len > 0 && start[len - 1] == '.')
         len--;
      if (len == 0)
         continue;
      /* Skip angle-bracket system includes: `#include <sys/stat.h>` — the '<'
       * immediately precedes the token, making it a system path, not a repo path. */
      if (start > prompt && start[-1] == '<')
         continue;

      /* Skip paths the brief explicitly negates ("Do NOT touch X", "skip Y"). */
      if (drift_path_is_negated(prompt, start))
         continue;

      /* Skip paths that are JSON string-values inside an illustrative example
       * (e.g. a schema shown to the delegate), not real output targets. */
      if (drift_path_is_json_example_value(prompt, start, p))
         continue;

      const char *path_start = start;
      size_t path_len = len;
      if (path_len > 2 && (path_start[0] == 'a' || path_start[0] == 'b') && path_start[1] == '/')
      {
         path_start += 2;
         path_len -= 2;
      }

      /* Require at least one '/' to look like a path */
      int has_slash = 0;
      for (size_t i = 0; i < path_len; i++)
      {
         if (path_start[i] == '/')
         {
            has_slash = 1;
            break;
         }
      }
      if (!has_slash)
         continue;

      /* Require a known source extension */
      const char *last_dot = NULL;
      for (size_t i = 0; i < path_len; i++)
         if (path_start[i] == '.')
            last_dot = path_start + i;
      if (!last_dot)
         continue;

      size_t ext_len = (size_t)((path_start + path_len) - last_dot);
      if (ext_len == 0 || ext_len >= 16)
         continue;
      char ext[16];
      memcpy(ext, last_dot, ext_len);
      ext[ext_len] = '\0';
      if (!drift_is_src_extension(ext))
         continue;

      /* Skip duplicates */
      int dup = 0;
      for (int i = 0; i < count; i++)
      {
         if (strlen(paths[i]) == path_len && memcmp(paths[i], path_start, path_len) == 0)
         {
            dup = 1;
            break;
         }
      }
      if (dup)
         continue;

      if (path_len < DELEGATE_DRIFT_PATH_MAX)
      {
         memcpy(paths[count], path_start, path_len);
         paths[count][path_len] = '\0';
         count++;
      }
   }

   return count;
}

/* Returns 1 if prompt contains an explicit intent to create a new file. */
static int has_create_intent(const char *prompt)
{
   /* Case-insensitive — capitalised verbs at sentence start ("Implement
    * a foo …") were previously rejected by the literal strstr match,
    * which surfaced as a false-positive 'no create intent found'
    * pre-flight rejection. */
   static const char *const create_keywords[] = {"create", "new file", "add file", "implement",
                                                 "write",  "generate", NULL};
   for (int i = 0; create_keywords[i]; i++)
      if (str_contains_ci(prompt, create_keywords[i]))
         return 1;
   return 0;
}

int delegate_prompt_allows_writes(const char *prompt)
{
   if (!prompt || !prompt[0])
      return 1;

   if (str_contains_ci(prompt, "do not edit files") ||
       str_contains_ci(prompt, "do not modify anything") ||
       str_contains_ci(prompt, "do not write files") ||
       str_contains_ci(prompt, "do not change files") ||
       str_contains_ci(prompt, "do not make changes") || str_contains_ci(prompt, "read-only") ||
       str_contains_ci(prompt, "read only") || str_contains_ci(prompt, "inspect only") ||
       str_contains_ci(prompt, "analysis only"))
      return 0;

   int scoped_no_write =
       str_contains_ci(prompt, "do not edit") || str_contains_ci(prompt, "do not modify") ||
       str_contains_ci(prompt, "do not write") || str_contains_ci(prompt, "do not change");
   if (scoped_no_write)
   {
      static const char *const scoped_write_keywords[] = {
          "create", "new file", "add ",   "add file", "implement ", "update",
          "fix",    "refactor", "delete", "remove",   NULL};
      int has_scoped_write_intent = 0;
      for (int i = 0; scoped_write_keywords[i]; i++)
         if (str_contains_ci(prompt, scoped_write_keywords[i]))
         {
            has_scoped_write_intent = 1;
            break;
         }
      if (!has_scoped_write_intent)
         return 0;
   }

   static const char *const write_keywords[] = {
       "create",    "new file", "add file", "edit",   "modify", "update", "fix",
       "implement", "write",    "refactor", "delete", "remove", NULL};
   for (int i = 0; write_keywords[i]; i++)
      if (str_contains_ci(prompt, write_keywords[i]))
         return 1;

   return 0;
}

/* Run git diff --name-only HEAD in wt_path; return heap-allocated output or NULL. */
static char *drift_git_diff(const char *wt_path)
{
   if (!wt_path || !wt_path[0])
      return NULL;
   char cmd[512];
   snprintf(cmd, sizeof(cmd),
            "git -C '%s' diff --name-only HEAD 2>/dev/null && "
            "git -C '%s' ls-files --others --exclude-standard 2>/dev/null",
            wt_path, wt_path);
   FILE *fp = popen(cmd, "r");
   if (!fp)
      return NULL;
   char *buf = malloc(65536);
   if (!buf)
   {
      pclose(fp);
      return NULL;
   }
   size_t n = fread(buf, 1, 65535, fp);
   buf[n] = '\0';
   pclose(fp);
   return buf;
}

/* Return 1 if the diff output contains path or its basename. */
static int drift_in_diff(const char *diff_out, const char *path)
{
   if (!diff_out || !path)
      return 0;
   if (strstr(diff_out, path))
      return 1;
   const char *base = strrchr(path, '/');
   base = base ? base + 1 : path;
   return strstr(diff_out, base) != NULL;
}

static int drift_stat_named_path(const char *path, const char *base_path, struct stat *st)
{
   if (!path || !path[0])
      return -1;

   if (path[0] == '/' || !base_path || !base_path[0])
      return stat(path, st);

   char full[MAX_PATH_LEN];
   snprintf(full, sizeof(full), "%s/%s", base_path, path);
   return stat(full, st);
}

/* Return 1 if an absolute path does not resolve under the worktree root — i.e. a
 * referenced EXTERNAL path (another machine's file named by an scp target such as
 * `admin@host:/mnt/.../aimee.yaml`, or the `//host/...` residue of a URL) rather
 * than an in-worktree create target.  A delegate creates files inside its
 * worktree, never at an arbitrary absolute host path, so the pre-flight
 * create-intent guard must not hard-fail on these.  Relative paths always resolve
 * under the worktree and are never external. */
static int drift_path_is_external(const char *path, const char *wt_path)
{
   if (!path || path[0] != '/')
      return 0;
   if (!wt_path || !wt_path[0])
      return 1; /* absolute with no worktree root to anchor to */

   size_t wlen = strlen(wt_path);
   while (wlen > 0 && wt_path[wlen - 1] == '/')
      wlen--;
   if (strncmp(path, wt_path, wlen) != 0)
      return 1; /* outside the worktree subtree */
   /* Guard against prefix aliasing (wt=/a/b matching /a/bc): the char after the
    * root must be a separator or end-of-string for the path to be truly under it. */
   char after = path[wlen];
   return (after != '\0' && after != '/');
}

int delegate_check_named_file_drift(const char *const *paths, int path_count, const char *prompt,
                                    const char *response, const char *wt_path, int role_is_write,
                                    char *errbuf, size_t errbuf_size)
{
   if (!paths || path_count <= 0)
      return 0;

   if (errbuf && errbuf_size > 0)
      errbuf[0] = '\0';

   /* The delegate ROLE is the authoritative write-intent signal: only a
    * write-capable role can be expected to CREATE a named file, so only it can
    * hard-drift on one that is missing. A read/analysis delegate (WFE's
    * understand/split/review, the judge) produces its artifact from its reply and
    * modifies nothing, so a repo-relative path scraped out of reference content
    * threaded into its prompt must never fail it. The prompt heuristic stays as a
    * NARROWING override so an explicit "do not edit" still disables the hard-fail
    * for a write role. */
   int writes_allowed = role_is_write && delegate_prompt_allows_writes(prompt);

   int hard_drift = 0;
   int soft_drift = 0;

   /* For the post-run git-diff path, fetch diff output once. */
   char *diff_out = NULL;
   if (response && wt_path && wt_path[0])
      diff_out = drift_git_diff(wt_path);

   for (int i = 0; i < path_count; i++)
   {
      const char *path = paths[i];
      if (!path || !path[0])
         continue;

      struct stat st;
      int exists = (drift_stat_named_path(path, wt_path, &st) == 0);

      if (!response)
      {
         /* Pre-flight: for nonexistent paths, require explicit create intent.
          * Before hard-failing, confirm this is a real project file via the aimee
          * index.  If the index is reachable, use the file's basename stem to find
          * indexed symbols: if it has hits but none live in a file matching this
          * path, the path was extracted from prompt examples (e.g. a system include)
          * rather than naming a real project file — skip it silently. */
         if (!exists && prompt)
         {
            /* A path that can't be an in-worktree create target — an absolute
             * host/scp/URL path outside the worktree — is a referenced external
             * file, not an output.  Merely naming one (e.g. an ops/deploy brief
             * that mentions admin@host:/mnt/.../aimee.yaml) must not hard-fail the
             * delegate before it runs. */
            if (drift_path_is_external(path, wt_path))
               continue;

            if (!writes_allowed)
               continue;

            const char *ibase = strrchr(path, '/');
            ibase = ibase ? ibase + 1 : path;
            char stem[128];
            snprintf(stem, sizeof(stem), "%s", ibase);
            char *idot = strrchr(stem, '.');
            if (idot)
               *idot = '\0';
            if (stem[0])
            {
               term_hit_t idx_hits[8];
               int nhits = kb_client_index_find(stem, idx_hits, 8);
               if (nhits > 0)
               {
                  /* Index is reachable — check if any hit lives in this path. */
                  int found = 0;
                  size_t plen = strlen(path);
                  for (int h = 0; h < nhits && !found; h++)
                  {
                     size_t flen = strlen(idx_hits[h].file_path);
                     const char *tail = idx_hits[h].file_path + flen - plen;
                     if (flen >= plen && strcmp(tail, path) == 0)
                        found = 1;
                  }
                  if (!found)
                     continue; /* not a project file — skip */
               }
               /* nhits == 0: index unreachable or stem not indexed; fall through
                * to the create-intent check so existing behaviour is preserved. */
            }

            if (!has_create_intent(prompt))
            {
               if (errbuf && errbuf_size > 0)
                  snprintf(errbuf, errbuf_size,
                           "named file '%s' does not exist and no create intent found in prompt; "
                           "use 'create', 'new file', or 'implement' to create it",
                           path);
               hard_drift = 1;
            }
         }
      }
      else if (wt_path && wt_path[0])
      {
         /* Post-run with worktree: use git diff as ground truth.
          * A path in the diff was touched — no drift.
          * A path not in diff that exists was context — soft warn only.
          * A path not in diff that doesn't exist was never created — hard drift. */
         if (drift_in_diff(diff_out, path))
            continue;

         if (exists)
         {
            /* Pre-existing context file the delegate didn't touch — soft drift. */
            if (!soft_drift && errbuf && errbuf_size > 0)
               snprintf(
                   errbuf, errbuf_size,
                   "named file '%s' was not modified by delegate (possible context-only reference)",
                   path);
            soft_drift = 1;
         }
         else
         {
            if (!writes_allowed)
            {
               if (!soft_drift && errbuf && errbuf_size > 0)
                  snprintf(errbuf, errbuf_size,
                           "named file '%s' was read-only context and was not created", path);
               soft_drift = 1;
               continue;
            }

            /* New file the delegate was supposed to create but didn't. */
            if (errbuf && errbuf_size > 0)
               snprintf(errbuf, errbuf_size, "named file '%s' was not created by delegate", path);
            hard_drift = 1;
         }
      }
      else
      {
         /* Post-run without worktree: fall back to response-text matching. */
         if (!exists)
            continue;

         if (strstr(response, path))
            continue;

         const char *base = strrchr(path, '/');
         base = base ? base + 1 : path;
         if (strstr(response, base))
         {
            if (!soft_drift && errbuf && errbuf_size > 0)
               snprintf(errbuf, errbuf_size,
                        "named file '%s' matched only by basename '%s' in response (ambiguous)",
                        path, base);
            soft_drift = 1;
         }
         else
         {
            if (!writes_allowed)
            {
               if (!soft_drift && errbuf && errbuf_size > 0)
                  snprintf(errbuf, errbuf_size,
                           "named file '%s' was read-only context and not repeated in response",
                           path);
               soft_drift = 1;
            }
            else
            {
               if (errbuf && errbuf_size > 0)
                  snprintf(errbuf, errbuf_size,
                           "named file '%s' not found in delegate response (possible drift)", path);
               hard_drift = 1;
            }
         }
      }
   }

   free(diff_out);

   if (hard_drift)
      return -1;
   if (soft_drift)
      return 1;
   return 0;
}

static void delegate_bundle_trim_linebreaks(char *s)
{
   if (!s)
      return;
   size_t n = strlen(s);
   while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r'))
      s[--n] = '\0';
}

static void delegate_bundle_append(char *dst, size_t cap, const char *src)
{
   if (!dst || cap == 0 || !src || !src[0])
      return;

   size_t used = strlen(dst);
   if (used > 0 && used + 1 < cap && dst[used - 1] != '\n')
   {
      dst[used++] = '\n';
      dst[used] = '\0';
   }
   if (used < cap - 1)
      snprintf(dst + used, cap - used, "%s", src);
}

static int delegate_bundle_count_lines(const char *s)
{
   if (!s || !s[0])
      return 0;

   int count = 0;
   int in_line = 0;
   for (const char *p = s; *p; p++)
   {
      if (*p == '\n' || *p == '\r')
      {
         if (in_line)
            count++;
         in_line = 0;
      }
      else
         in_line = 1;
   }
   if (in_line)
      count++;
   return count;
}

static void delegate_bundle_copy_patch(char *dst, size_t cap, const char *src)
{
   if (!dst || cap == 0)
      return;
   if (!src || !src[0])
   {
      snprintf(dst, cap, "(no changes vs HEAD)");
      return;
   }

   size_t len = strlen(src);
   if (len < cap)
   {
      snprintf(dst, cap, "%s", src);
      return;
   }

   static const char marker[] = "\n[diff truncated]\n";
   size_t marker_len = strlen(marker);
   if (cap <= marker_len + 1)
   {
      snprintf(dst, cap, "%s", src);
      return;
   }

   size_t copy = cap - marker_len - 1;
   memcpy(dst, src, copy);
   memcpy(dst + copy, marker, marker_len);
   dst[copy + marker_len] = '\0';
}

static void delegate_bundle_csv_append(char *dst, size_t cap, const char *value)
{
   if (!dst || cap == 0 || !value || !value[0])
      return;

   size_t used = strlen(dst);
   if (used >= cap - 1)
      return;
   const char *sep = used > 0 ? ", " : "";
   snprintf(dst + used, cap - used, "%s%s", sep, value);
}

static int delegate_bundle_rel_exists(const char *root, const char *rel)
{
   if (!root || !root[0] || !rel || !rel[0])
      return 0;

   char path[MAX_PATH_LEN];
   snprintf(path, sizeof(path), "%s/%s", root, rel);
   return access(path, F_OK) == 0;
}

static void delegate_bundle_repo_evidence(const char *cwd, char *dst, size_t cap)
{
   if (!dst || cap == 0)
      return;
   dst[0] = '\0';

   char root[MAX_PATH_LEN] = "";
   {
      char *out = NULL;
      const char *const argv[] = {"git", "-C", cwd, "rev-parse", "--show-toplevel", NULL};
      if (safe_exec_capture(argv, &out, sizeof(root) - 1) == 0 && out && out[0])
      {
         delegate_bundle_trim_linebreaks(out);
         snprintf(root, sizeof(root), "%s", out);
      }
      free(out);
   }
   if (!root[0])
      snprintf(root, sizeof(root), "%s", cwd);

   char present[512] = "";
   char absent[512] = "";
   const char *const markers[] = {"Makefile",    "src/Makefile", "CMakeLists.txt",
                                  "meson.build", "package.json", "Cargo.toml",
                                  NULL};
   for (int i = 0; markers[i]; i++)
   {
      if (delegate_bundle_rel_exists(root, markers[i]))
         delegate_bundle_csv_append(present, sizeof(present), markers[i]);
      else
         delegate_bundle_csv_append(absent, sizeof(absent), markers[i]);
   }
   if (!present[0])
      snprintf(present, sizeof(present), "(none of common build markers checked)");
   if (!absent[0])
      snprintf(absent, sizeof(absent), "(none)");

   const char *verification_hint = "(inspect repository files before choosing commands)";
   if (delegate_bundle_rel_exists(root, "src/Makefile"))
      verification_hint = "make -C src ...";
   else if (delegate_bundle_rel_exists(root, "Makefile"))
      verification_hint = "make ...";
   else if (delegate_bundle_rel_exists(root, "CMakeLists.txt"))
      verification_hint = "cmake ...";

   snprintf(dst, cap,
            "repo_evidence:\n"
            "- git_root: %s\n"
            "- build_files_present: %s\n"
            "- build_files_absent: %s\n"
            "- verification_hint_from_files: %s\n",
            root, present, absent, verification_hint);
}

/* Build a compact validation evidence bundle for read-only inspection delegates.
 * The bundle includes HEAD ref, changed files, diff --stat, and a capped patch
 * so the delegate is grounded in the current worktree state even when it runs in
 * an isolated sibling worktree.
 * Returns a heap-allocated string; caller must free.  Returns NULL on failure. */
char *delegate_build_validation_bundle(const char *cwd)
{
   if (!cwd || !cwd[0])
      return NULL;

   char head[128] = "(unknown)";
   char diff_stat[4096] = "(none)";
   char changed_files[4096] = "(none)";
   char diff_patch[65536] = "(none)";
   char branch_base[128] = "(unavailable)";
   char branch_diff_stat[4096] = "(none)";
   char branch_changed_files[4096] = "(none)";
   char branch_diff_patch[65536] = "(none)";
   char repo_evidence[2048] = "";
   delegate_bundle_repo_evidence(cwd, repo_evidence, sizeof(repo_evidence));

   /* Get HEAD commit ref */
   {
      char *out = NULL;
      const char *const argv[] = {"git", "-C", cwd, "rev-parse", "--short", "HEAD", NULL};
      if (safe_exec_capture(argv, &out, 256) == 0 && out && out[0])
      {
         delegate_bundle_trim_linebreaks(out);
         if (out[0])
            snprintf(head, sizeof(head), "%s", out);
      }
      free(out);
   }

   /* Get diff --stat (staged + unstaged vs HEAD) */
   {
      char *out = NULL;
      const char *const argv[] = {"git", "-C", cwd, "diff", "--stat", "HEAD", NULL};
      if (safe_exec_capture(argv, &out, sizeof(diff_stat) - 1) == 0 && out)
      {
         snprintf(diff_stat, sizeof(diff_stat), "%s", out[0] ? out : "(no changes vs HEAD)");
      }
      free(out);
   }

   /* Get list of changed files */
   {
      char *out = NULL;
      const char *const diff_argv[] = {"git", "-C", cwd, "diff", "--name-only", "HEAD", NULL};
      changed_files[0] = '\0';
      if (safe_exec_capture(diff_argv, &out, sizeof(changed_files) - 1) == 0 && out)
      {
         delegate_bundle_append(changed_files, sizeof(changed_files), out);
      }
      free(out);

      out = NULL;
      const char *const untracked_argv[] = {
          "git", "-C", cwd, "ls-files", "--others", "--exclude-standard", NULL};
      if (safe_exec_capture(untracked_argv, &out, sizeof(changed_files) - 1) == 0 && out)
         delegate_bundle_append(changed_files, sizeof(changed_files), out);
      free(out);
   }
   int changed_file_count = delegate_bundle_count_lines(changed_files);
   if (!changed_files[0])
      snprintf(changed_files, sizeof(changed_files), "(none)");

   /* Get the committed branch delta against origin/main when available.  This
    * keeps review delegates grounded on clean feature branches where the
    * uncommitted diff is intentionally empty. */
   {
      char *out = NULL;
      const char *const base_argv[] = {"git",      "-C",      cwd,           "rev-parse",
                                       "--verify", "--quiet", "origin/main", NULL};
      if (safe_exec_capture(base_argv, &out, sizeof(branch_base) - 1) == 0 && out && out[0])
      {
         delegate_bundle_trim_linebreaks(out);
         snprintf(branch_base, sizeof(branch_base), "origin/main");
      }
      free(out);
   }

   if (strcmp(branch_base, "origin/main") == 0)
   {
      char *out = NULL;
      const char *const stat_argv[] = {"git", "-C", cwd, "diff", "--stat", "origin/main...HEAD",
                                       NULL};
      if (safe_exec_capture(stat_argv, &out, sizeof(branch_diff_stat) - 1) == 0 && out)
      {
         snprintf(branch_diff_stat, sizeof(branch_diff_stat), "%s",
                  out[0] ? out : "(no branch changes vs origin/main)");
      }
      free(out);

      out = NULL;
      const char *const names_argv[] = {
          "git", "-C", cwd, "diff", "--name-only", "origin/main...HEAD", NULL};
      branch_changed_files[0] = '\0';
      if (safe_exec_capture(names_argv, &out, sizeof(branch_changed_files) - 1) == 0 && out)
         delegate_bundle_append(branch_changed_files, sizeof(branch_changed_files), out);
      free(out);
      if (!branch_changed_files[0])
         snprintf(branch_changed_files, sizeof(branch_changed_files), "(none)");

      out = NULL;
      const char *const patch_argv[] = {
          "git", "-C", cwd, "diff", "--no-ext-diff", "--unified=12", "origin/main...HEAD", NULL};
      if (safe_exec_capture(patch_argv, &out, 262144) == 0 && out)
         delegate_bundle_copy_patch(branch_diff_patch, sizeof(branch_diff_patch), out);
      free(out);
   }
   int branch_changed_file_count = strcmp(branch_changed_files, "(none)") == 0
                                       ? 0
                                       : delegate_bundle_count_lines(branch_changed_files);

   /* Get a capped actual patch. */
   {
      char *out = NULL;
      const char *const argv[] = {"git",           "-C",           cwd,    "diff",
                                  "--no-ext-diff", "--unified=12", "HEAD", NULL};
      if (safe_exec_capture(argv, &out, 262144) == 0 && out)
         delegate_bundle_copy_patch(diff_patch, sizeof(diff_patch), out);
      free(out);
   }

   size_t cap = sizeof(head) + sizeof(diff_stat) + sizeof(changed_files) + sizeof(diff_patch) +
                sizeof(branch_base) + sizeof(branch_diff_stat) + sizeof(branch_changed_files) +
                sizeof(branch_diff_patch) + sizeof(repo_evidence) + strlen(cwd) * 2 + 2800;
   char *bundle = malloc(cap);
   if (!bundle)
      return NULL;

   snprintf(bundle, cap,
            "\n\n---\n"
            "## Validation Evidence Bundle\n"
            "base_ref: %s\n"
            "worktree_path: %s\n"
            "%s"
            "diff_source: uncommitted changes in worktree_path against its current HEAD; do not "
            "reinterpret this as git diff base_ref from another checkout.\n"
            "staleness_rule: if changed_file_count is greater than 0, a clean or different "
            "checkout is stale evidence for this task; base conclusions on changed_files and "
            "diff_patch unless you have directly inspected worktree_path.\n"
            "diff_command: git -C <worktree_path> diff --no-ext-diff --unified=12 HEAD\n"
            "changed_file_count: %d\n"
            "changed_files:\n%s\n"
            "diff_stat:\n%s\n"
            "diff_patch:\n%s\n"
            "\n"
            "branch_diff_source: committed branch delta against origin/main when that ref exists; "
            "use this section for PR review on a clean feature branch.\n"
            "branch_diff_command: git -C <worktree_path> diff --no-ext-diff --unified=12 "
            "origin/main...HEAD\n"
            "branch_diff_base: %s\n"
            "branch_changed_file_count: %d\n"
            "branch_changed_files:\n%s\n"
            "branch_diff_stat:\n%s\n"
            "branch_diff_patch:\n%s\n"
            "---\n"
            "Cite this bundle (base_ref: %s, worktree_path: %s) in your response and reference "
            "file/line numbers for every finding. Report findings from the listed changed_files "
            "or branch_changed_files unless the task explicitly asks for broader repository "
            "analysis. Do not assert a build system, symbol, struct field, file, or API exists "
            "unless it appears in this bundle or you inspected it directly; label anything else "
            "as unverified. Do not assert that something is missing unless you directly searched "
            "worktree_path for it and can cite the exact zero-result search command or the "
            "matching file/line evidence that proves absence in the relevant scope. Directory "
            "layout claims must cite the exact listing/search command and output from "
            "worktree_path; do not infer that module directories are absent from a clean or "
            "separate checkout. For split "
            "or migration work, absence searches must cover the whole relevant source tree, not "
            "only the proposed destination directory. For file moves, do not infer sibling "
            "headers or similarly named files moved; cite name-status, changed_files, or direct "
            "file-existence evidence for each moved path.\n",
            head, cwd, repo_evidence, changed_file_count, changed_files, diff_stat, diff_patch,
            branch_base, branch_changed_file_count, branch_changed_files, branch_diff_stat,
            branch_diff_patch, head, cwd);
   return bundle;
}

/* Guidance appended when the CALLER supplied the review target in the prompt
 * (--prompt-file / --prompt-stdin), e.g. a diff for an external code review. The
 * target is the provided content — NOT the delegate host's working directory, which
 * may be absent, on a different branch, or carry unrelated changes. For surrounding
 * context the reviewer explores the DEFAULT BRANCH through aimee's own index/memory
 * (code_search, find_symbol, search_memory, search_docs), which are branch-indexed
 * and always available, rather than filesystem/git tools against a worktree it does
 * not have. Returned string is heap-owned. */
static char *delegate_build_provided_target_block(void)
{
   return strdup(
       "\n\n---\n"
       "## Review Target & Exploration\n"
       "review_target: the diff / content provided in the prompt ABOVE is the sole subject of "
       "this review. Base every finding on it.\n"
       "no_local_worktree: do NOT run read_file/list_files/grep/git_diff/git_status against a "
       "local checkout — this delegate has no such worktree for the target; that path is empty, "
       "stale, or unrelated, and is not the review target.\n"
       "explore_via_aimee: to inspect surrounding code, callers, or prior context on the DEFAULT "
       "BRANCH, use aimee's own capabilities — code_search and find_symbol (branch-indexed code), "
       "search_memory and search_docs (project memory/knowledge). These reflect the committed "
       "default branch and are always available; prefer them over any filesystem tool.\n"
       "absence_claims: only assert something is missing if a code_search/find_symbol query for "
       "it returned no results; cite that query. Never infer absence from an unreadable worktree.\n"
       "---\n");
}

char *delegate_maybe_append_validation_bundle(const char *role, const char *cwd, char *owned_prompt,
                                              const char *fallback_prompt, int target_provided)
{
   if (!role)
      return owned_prompt;

   const char *canonical_role = delegate_role_canonicalize(role);
   int is_review_role = strcmp(canonical_role, "review") == 0 ||
                        strcmp(canonical_role, "validate") == 0 ||
                        strcmp(canonical_role, "diagnose") == 0;

   /* Caller-supplied target (a diff via --prompt-file/--prompt-stdin) wins: the
    * provided content is the review subject, so the host-cwd auto-diff bundle is
    * suppressed (it would review the wrong tree) and the reviewer is pointed at
    * aimee's branch-indexed capabilities for context instead of the filesystem. */
   char *bundle = target_provided ? delegate_build_provided_target_block()
                                  : (is_review_role ? delegate_build_validation_bundle(cwd) : NULL);
   if (!bundle)
      return owned_prompt;

   const char *base = owned_prompt ? owned_prompt : (fallback_prompt ? fallback_prompt : "");
   size_t cap = strlen(base) + strlen(bundle) + 1;
   char *combined = malloc(cap);
   if (combined)
   {
      snprintf(combined, cap, "%s%s", base, bundle);
      free(owned_prompt);
      owned_prompt = combined;
   }
   free(bundle);
   return owned_prompt;
}

static void review_norm_line(const char *src, char *dst, size_t dst_sz)
{
   if (!dst || dst_sz == 0)
      return;
   dst[0] = '\0';
   if (!src)
      return;

   while (*src && isspace((unsigned char)*src))
      src++;
   if ((*src == '+' || *src == '-') && src[1] && src[1] != *src)
      src++;
   while (*src && isspace((unsigned char)*src))
      src++;

   size_t pos = 0;
   int last_space = 0;
   for (; *src && pos + 1 < dst_sz; src++)
   {
      unsigned char ch = (unsigned char)*src;
      if (ch == '\r' || ch == '\n')
         break;
      if (isspace(ch))
      {
         if (!last_space)
         {
            dst[pos++] = ' ';
            last_space = 1;
         }
      }
      else
      {
         dst[pos++] = (char)ch;
         last_space = 0;
      }
   }
   while (pos > 0 && dst[pos - 1] == ' ')
      pos--;
   dst[pos] = '\0';
}

static int review_parse_location(const char *loc, char *path, size_t path_sz, int *line_out)
{
   const char *tick = strchr(loc, '`');
   if (!tick)
      return -1;
   const char *end = strchr(tick + 1, '`');
   if (!end || end <= tick + 1)
      return -1;

   char spec[MAX_PATH_LEN + 64];
   size_t n = (size_t)(end - tick - 1);
   if (n >= sizeof(spec))
      n = sizeof(spec) - 1;
   memcpy(spec, tick + 1, n);
   spec[n] = '\0';

   char *colon = strrchr(spec, ':');
   if (!colon)
      return -1;
   *colon = '\0';
   int line = atoi(colon + 1);
   if (line <= 0 || spec[0] == '/' || strstr(spec, ".."))
      return -1;

   snprintf(path, path_sz, "%s", spec);
   if (line_out)
      *line_out = line;
   return 0;
}

static int review_extract_snippet_lines(const char *code_start, const char *code_end,
                                        char lines[][256], int max_lines)
{
   int count = 0;
   const char *p = code_start;
   while (p && p < code_end && count < max_lines)
   {
      const char *nl = memchr(p, '\n', (size_t)(code_end - p));
      const char *line_end = nl ? nl : code_end;
      char raw[512];
      size_t n = (size_t)(line_end - p);
      if (n >= sizeof(raw))
         n = sizeof(raw) - 1;
      memcpy(raw, p, n);
      raw[n] = '\0';

      char norm[256];
      review_norm_line(raw, norm, sizeof(norm));
      char *review_note = strstr(norm, "\xE2\x86\x90");
      if (review_note)
      {
         while (review_note > norm && isspace((unsigned char)review_note[-1]))
            review_note--;
         *review_note = '\0';
      }
      if (norm[0] && strncmp(norm, "```", 3) != 0 && strcmp(norm, "...") != 0)
         snprintf(lines[count++], 256, "%s", norm);
      p = nl ? nl + 1 : code_end;
   }
   return count;
}

static int review_fence_is_historical_snippet(const char *section_start, const char *fence)
{
   if (!section_start || !fence || fence <= section_start)
      return 0;

   const char *p = fence;
   while (p > section_start && p[-1] != '\n')
      p--;
   while (p > section_start && isspace((unsigned char)p[-1]))
      p--;

   const char *line = p;
   while (line > section_start && line[-1] != '\n')
      line--;

   char intro[256];
   size_t n = (size_t)(p - line);
   if (n >= sizeof(intro))
      n = sizeof(intro) - 1;
   for (size_t i = 0; i < n; i++)
      intro[i] = (char)tolower((unsigned char)line[i]);
   intro[n] = '\0';

   if (strstr(intro, "from:") || strstr(intro, "before:") || strstr(intro, "old:") ||
       strstr(intro, "previous:") || strstr(intro, "was:"))
      return 1;
   return 0;
}

static int review_snippet_matches_file(const char *repo_root, const char *path, int line,
                                       char snippet[][256], int snippet_count)
{
   if (snippet_count < 2)
      return 1;

   char full[MAX_PATH_LEN * 2];
   snprintf(full, sizeof(full), "%s/%s", repo_root, path);
   FILE *f = fopen(full, "r");
   if (!f)
      return 1;

   char window[160][256];
   int count = 0;
   int lineno = 0;
   int start = line > 30 ? line - 30 : 1;
   int end = line + 80;
   char raw[1024];
   while (fgets(raw, sizeof(raw), f))
   {
      lineno++;
      if (lineno < start)
         continue;
      if (lineno > end)
         break;
      char norm[256];
      review_norm_line(raw, norm, sizeof(norm));
      if (norm[0] && count < (int)(sizeof(window) / sizeof(window[0])))
         snprintf(window[count++], sizeof(window[count]), "%s", norm);
   }
   fclose(f);

   int pos = 0;
   for (int i = 0; i < snippet_count; i++)
   {
      int found = 0;
      for (int j = pos; j < count; j++)
      {
         if (strcmp(window[j], snippet[i]) == 0)
         {
            pos = j + 1;
            found = 1;
            break;
         }
      }
      if (!found)
         return 0;
   }
   return 1;
}

int delegate_check_review_evidence_drift(const char *response, const char *repo_root, char *errbuf,
                                         size_t errbuf_size)
{
   if (!response || !repo_root || !repo_root[0])
      return -1;

   const char *p = response;
   while ((p = strstr(p, "Location:")) != NULL)
   {
      char path[MAX_PATH_LEN];
      int line = 0;
      if (review_parse_location(p, path, sizeof(path), &line) != 0)
      {
         p += 9;
         continue;
      }

      const char *fence = strstr(p, "```");
      const char *next_loc = strstr(p + 9, "Location:");
      while (fence && (!next_loc || fence < next_loc))
      {
         const char *code_start = strchr(fence, '\n');
         if (!code_start || (next_loc && code_start > next_loc))
            break;
         code_start++;
         const char *code_end = strstr(code_start, "```");
         if (!code_end || (next_loc && code_end > next_loc))
            break;

         if (!review_fence_is_historical_snippet(p, fence))
         {
            char snippet[64][256];
            int snippet_count = review_extract_snippet_lines(code_start, code_end, snippet, 64);
            if (!review_snippet_matches_file(repo_root, path, line, snippet, snippet_count))
            {
               if (errbuf && errbuf_size > 0)
                  snprintf(errbuf, errbuf_size,
                           "delegate evidence drift: cited snippet for %s:%d does not match the "
                           "current checkout near that line",
                           path, line);
               return 1;
            }
         }
         fence = strstr(code_end + 3, "```");
      }
      p = next_loc ? next_loc : p + 9;
   }
   return 0;
}

static int delegate_repo_has_uncommitted_changes(const char *repo_root)
{
   if (!repo_root || !repo_root[0])
      return 0;

   char *out = NULL;
   const char *const argv[] = {"git", "-C", repo_root, "status", "--porcelain", NULL};
   int rc = safe_exec_capture(argv, &out, 65536);
   int dirty = (rc == 0 && out && out[0]);
   free(out);
   return dirty;
}

static int delegate_response_claims_missing_parent_diff(const char *response)
{
   if (!response || !response[0])
      return 0;

   return str_contains_ci(response, "no uncommitted diff exists") ||
          str_contains_ci(response, "working tree is clean") ||
          str_contains_ci(response, "there is nothing to review") ||
          str_contains_ci(response, "cannot see the current diff") ||
          str_contains_ci(response, "cannot verify the diff");
}

void delegate_apply_review_evidence_guard(const char *role, const char *repo_root, int *rc,
                                          agent_result_t *result, int target_provided)
{
   if (!rc || *rc != 0 || !result || !result->response || !result->response[0] || !repo_root ||
       !repo_root[0])
      return;

   /* When the caller supplied the review target (a diff in the prompt), there is
    * no host-cwd evidence to drift against — the provided content IS the evidence.
    * Guarding against the local worktree here would penalize a correct review of
    * the provided diff, so skip it. */
   if (target_provided)
      return;

   if (!role || (strcmp(role, "review") != 0 && strcmp(role, "validate") != 0 &&
                 strcmp(role, "diagnose") != 0 && strcmp(role, "test") != 0 &&
                 strcmp(role, "check") != 0 && strcmp(role, "inspect") != 0))
      return;

   if (strcmp(role, "review") == 0 || strcmp(role, "validate") == 0 || strcmp(role, "test") == 0 ||
       strcmp(role, "check") == 0)
   {
      char drift_err[512];
      if (delegate_check_review_evidence_drift(result->response, repo_root, drift_err,
                                               sizeof(drift_err)) > 0)
      {
         *rc = -1;
         snprintf(result->error, sizeof(result->error), "%s", drift_err);
         return;
      }
   }

   if (delegate_repo_has_uncommitted_changes(repo_root) &&
       delegate_response_claims_missing_parent_diff(result->response))
   {
      *rc = -1;
      snprintf(result->error, sizeof(result->error),
               "delegate evidence drift: response claimed the parent diff was clean or missing "
               "while the parent worktree has uncommitted changes");
   }
}

/* Build a ## Context block by searching the code index for terms in the delegate
 * prompt.  Queries kb_client_index_code_search with the first 240 chars of the
 * prompt and formats up to 6 hits as fenced code blocks.  Returns a
 * heap-allocated string to append to the system prompt, or NULL if the kb is
 * unreachable or the query yields nothing.  Caller must free. */
char *delegate_inject_code_context(const char *prompt)
{
   if (!prompt || !prompt[0])
      return NULL;

   char query[256];
   snprintf(query, sizeof(query), "%s", prompt);

   code_search_hit_t hits[6];
   int n = kb_client_index_code_search(query, NULL, hits, 6);
   if (n <= 0)
      return NULL;

   char buf[2048];
   int pos = 0;
   int rem = (int)sizeof(buf);

   int w = snprintf(buf + pos, (size_t)rem, "\n\n## Context\n");
   if (w > 0 && w < rem)
   {
      pos += w;
      rem -= w;
   }

   for (int i = 0; i < n && rem > 64; i++)
   {
      const char *fp = hits[i].file_path[0] ? hits[i].file_path : "(unknown)";
      w = snprintf(buf + pos, (size_t)rem, "\n%s:\n```\n%.200s\n```\n", fp, hits[i].snippet);
      if (w > 0 && w < rem)
      {
         pos += w;
         rem -= w;
      }
   }

   if (pos <= 0)
      return NULL;

   buf[pos] = '\0'; /* a truncated final snprintf leaves buf[pos] non-NUL; terminate so
                     * the malloc(pos+1)/memcpy(pos+1) returns a valid C string. */
   char *out = malloc((size_t)(pos + 1));
   if (!out)
      return NULL;
   memcpy(out, buf, (size_t)(pos + 1));
   return out;
}

/* Append " a, b, c (+N)" — up to `max_names` of `count` entries from names[][]. */
static void graph_ctx_append_names(char *buf, int *pos, int *rem, char names[][MAX_PATH_LEN],
                                   int count, int max_names)
{
   int cap = count < max_names ? count : max_names;
   for (int k = 0; k < cap && *rem > 32; k++)
   {
      int w = snprintf(buf + *pos, (size_t)*rem, " %s%s", names[k], (k + 1 < cap) ? "," : "");
      if (w > 0 && w < *rem)
      {
         *pos += w;
         *rem -= w;
      }
   }
   if (count > cap)
   {
      int w = snprintf(buf + *pos, (size_t)*rem, " (+%d)", count - cap);
      if (w > 0 && w < *rem)
      {
         *pos += w;
         *rem -= w;
      }
   }
}

/* Resolve an abs path to its indexed project + relpath and fetch the structural
 * blast radius (call graph + projection edges). Returns 0 with *out filled on a
 * project match + successful fetch, -1 otherwise (FAIL-OPEN). Mirrors the §7
 * advisory's guardrails_blast_radius_for_abs_path, inlined here so delegate_prompt
 * stays decoupled from the guardrails object (it already links kb_client_index). */
static int delegate_blast_radius_for_abs_path(const char *abs_path, blast_radius_t *out)
{
   if (!abs_path || !abs_path[0] || !out)
      return -1;
   project_info_t projects[32];
   int pcount = kb_client_index_list(projects, 32);
   for (int p = 0; p < pcount; p++)
   {
      size_t rlen = strlen(projects[p].root);
      if (strncmp(abs_path, projects[p].root, rlen) == 0 &&
          (abs_path[rlen] == '/' || abs_path[rlen] == '\0'))
      {
         const char *rel = abs_path + rlen;
         if (*rel == '/')
            rel++;
         memset(out, 0, sizeof(*out));
         return kb_client_index_blast_radius(projects[p].name, rel, out) == 0 ? 0 : -1;
      }
   }
   return -1; /* no indexed project owns this path */
}

/* §7 graph-informed delegation: prepend a structural-context block — the callers
 * (dependents) and dependencies of the file paths the delegate task references —
 * so the delegate starts with the structural neighborhood. Opt-in
 * (`delegate_graph_context_enabled`, default off) and FAIL-OPEN (NULL on any
 * miss: flag off, no referenced paths, kb unreachable, or no structural edges).
 * Uses ONLY the deterministic structural graph (kb_client_index_blast_radius),
 * never the LLM entity graph — the same R1 constraint as the §7 blast-radius
 * advisory. Returns a heap block the caller frees, or NULL. */
char *delegate_inject_graph_context(const char *prompt, const char *cwd)
{
   if (!prompt || !prompt[0])
      return NULL;
   config_t cfg;
   if (config_load(&cfg) != 0 || !cfg.delegate_graph_context_enabled)
      return NULL;

   char paths[DELEGATE_DRIFT_MAX_PATHS][DELEGATE_DRIFT_PATH_MAX];
   int np = delegate_extract_named_paths(prompt, paths, DELEGATE_DRIFT_MAX_PATHS);
   if (np <= 0)
      return NULL;

   char buf[4096];
   int pos = 0, rem = (int)sizeof(buf);
   int w = snprintf(buf + pos, (size_t)rem,
                    "\n\n## Structural context (code graph)\n"
                    "Callers/dependencies of the files this task references, from the "
                    "structural index — review before changing a shared interface:\n");
   if (w > 0 && w < rem)
   {
      pos += w;
      rem -= w;
   }

   int files_emitted = 0;
   for (int i = 0; i < np && files_emitted < 6 && rem > 96; i++)
   {
      char abs[MAX_PATH_LEN];
      if (paths[i][0] == '/' || !cwd || !cwd[0])
         snprintf(abs, sizeof(abs), "%s", paths[i]);
      else
         snprintf(abs, sizeof(abs), "%s/%s", cwd, paths[i]);

      blast_radius_t br;
      if (delegate_blast_radius_for_abs_path(abs, &br) != 0)
         continue; /* fail-open: path not indexed / kb down */
      if (br.dependent_count <= 0 && br.dependency_count <= 0)
         continue;

      w = snprintf(buf + pos, (size_t)rem, "- `%s`", paths[i]);
      if (w <= 0 || w >= rem)
         break; /* the file header didn't fit; stop rather than attribute callers to nothing */
      pos += w;
      rem -= w;
      if (br.dependent_count > 0)
      {
         w = snprintf(buf + pos, (size_t)rem, " — callers:");
         if (w > 0 && w < rem)
         {
            pos += w;
            rem -= w;
         }
         graph_ctx_append_names(buf, &pos, &rem, br.dependents, br.dependent_count, 5);
      }
      if (br.dependency_count > 0)
      {
         w = snprintf(buf + pos, (size_t)rem,
                      "%s depends on:", br.dependent_count > 0 ? ";" : " —");
         if (w > 0 && w < rem)
         {
            pos += w;
            rem -= w;
         }
         graph_ctx_append_names(buf, &pos, &rem, br.dependencies, br.dependency_count, 5);
      }
      w = snprintf(buf + pos, (size_t)rem, "\n");
      if (w > 0 && w < rem)
      {
         pos += w;
         rem -= w;
      }
      files_emitted++;
   }

   if (files_emitted == 0)
      return NULL; /* no structural edges for any referenced file */

   buf[pos] = '\0'; /* defensive: ensure a valid C string even if a write truncated */
   char *out = malloc((size_t)(pos + 1));
   if (!out)
      return NULL;
   memcpy(out, buf, (size_t)(pos + 1));
   return out;
}

/* Nested delegate creation is denied at runtime; do not add a prompt block that
 * tells delegates to fan out work they cannot actually create. */
char *delegate_build_tier_context(const char *via_name, int tier_override, const char *role)
{
   (void)via_name;
   (void)tier_override;
   (void)role;
   return NULL;
}

char *delegate_rewrite_prompt_cwd(const char *prompt, const char *cwd, const char *worktree_path,
                                  int *occurrences_out)
{
   if (occurrences_out)
      *occurrences_out = 0;
   if (!prompt || !cwd || !cwd[0] || !worktree_path || !worktree_path[0])
      return NULL;

   size_t cwd_len = strlen(cwd);
   size_t wt_len = strlen(worktree_path);
   int occurrences = 0;
   const char *scan = prompt;
   while ((scan = strstr(scan, cwd)) != NULL)
   {
      occurrences++;
      scan += cwd_len;
   }
   if (occurrences == 0)
      return NULL;

   size_t old_len = strlen(prompt);
   size_t new_len = old_len - (size_t)occurrences * cwd_len + (size_t)occurrences * wt_len;
   char *rewritten = malloc(new_len + 1);
   if (!rewritten)
      return NULL;

   char *out = rewritten;
   const char *p = prompt;
   while (*p)
   {
      const char *found = strstr(p, cwd);
      if (!found)
      {
         size_t tail = strlen(p);
         memcpy(out, p, tail);
         out += tail;
         break;
      }
      size_t pre = (size_t)(found - p);
      memcpy(out, p, pre);
      out += pre;
      memcpy(out, worktree_path, wt_len);
      out += wt_len;
      p = found + cwd_len;
   }
   *out = '\0';
   if (occurrences_out)
      *occurrences_out = occurrences;
   return rewritten;
}

char *delegate_prompt_append_block(const char *base, const char *block)
{
   if (!block)
      return NULL;
   size_t base_len = base ? strlen(base) : 0;
   size_t blk_len = strlen(block);
   char *combined = malloc(base_len + blk_len + 1);
   if (!combined)
      return NULL;
   if (base)
      memcpy(combined, base, base_len);
   memcpy(combined + base_len, block, blk_len + 1);
   return combined;
}

const char *delegate_assemble_system_prompt(const char *in, const char *role, const char *prompt,
                                            const char *cwd, const char *persona,
                                            const char *worktree_path, char **owned_out)
{
   char *owned = NULL;
   if (!in)
   {
      const char *template_root =
          worktree_path[0] ? worktree_path : ((cwd[0] && cwd[0] == '/') ? cwd : NULL);
      owned = role_template_build(template_root, role, prompt, NULL);
      if (owned)
         in = owned;
   }
   if (!in)
   {
      static const char *fallback =
          "You are a sub-agent executing a delegated task. "
          "Complete the task and report results. "
          "If you need input from the parent agent, use the request_input tool.";
      in = fallback;
   }

   /* Compose the assigned persona's identity + principles onto the role/task
    * prompt. persona is guaranteed non-empty (required by the caller). */
   char *with_principles = persona_compose_delegate_prompt(persona, cwd, in);
   if (with_principles)
   {
      free(owned);
      owned = with_principles;
      in = with_principles;
   }

   /* Apply token budget to system prompt (progressive context shedding).
    * Mirrors the CLI delegate path so structured shedding is logged via aimee_log. */
   const char *budget_root = cwd[0] ? cwd : NULL;
   int budget = delegate_token_budget_load(budget_root, role);
   char *limited = delegate_prompt_limit(in, budget);
   if (limited)
   {
      free(owned);
      owned = limited;
      in = limited;
   }

   *owned_out = owned;
   return in;
}

char *delegate_prepend_parent_diff_evidence(const char *prompt, const char *role, int allows_writes,
                                            const char *cwd, const char *deleg_id)
{
   const char *canon_role = delegate_role_canonicalize(role);
   int needs_diff_bundle = strcmp(canon_role, "validate") == 0 ||
                           strcmp(canon_role, "review") == 0 ||
                           strcmp(canon_role, "diagnose") == 0 || strcmp(canon_role, "test") == 0;
   if (!needs_diff_bundle || allows_writes)
      return NULL;

   char bundle_cwd_buf[MAX_PATH_LEN];
   const char *bundle_cwd = (cwd && cwd[0]) ? cwd : NULL;
   if (!bundle_cwd && getcwd(bundle_cwd_buf, sizeof(bundle_cwd_buf)))
      bundle_cwd = bundle_cwd_buf;
   if (!bundle_cwd || !bundle_cwd[0])
   {
      aimee_log(LOG_WARN, "delegate",
                "delegate %s: skipped parent diff evidence bundle because cwd is unavailable",
                deleg_id ? deleg_id : "?");
      return NULL;
   }

   char *bundle = delegate_build_validation_bundle(bundle_cwd);
   if (!bundle)
      return NULL;

   static const char bundle_preamble[] =
       "\n\n---\n"
       "## Parent Worktree Diff Evidence\n"
       "Use this bundle as the source of truth for the parent worktree's current "
       "uncommitted changes. Your isolated delegate checkout may have a clean or "
       "different `git diff`; do not report findings from that isolated diff. "
       "Reading the parent worktree is allowed, but do not modify it; this copied "
       "evidence is the primary parent-worktree diff context for this task.\n";
   size_t prompt_len = prompt ? strlen(prompt) : 0;
   size_t pre_len = strlen(bundle_preamble);
   size_t bun_len = strlen(bundle);
   char *combined = malloc(prompt_len + pre_len + bun_len + 1);
   if (combined)
   {
      if (prompt)
         memcpy(combined, prompt, prompt_len);
      memcpy(combined + prompt_len, bundle_preamble, pre_len);
      memcpy(combined + prompt_len + pre_len, bundle, bun_len + 1);
   }
   free(bundle);
   return combined;
}

int delegate_worktree_has_changes(const char *wt_path)
{
   char *diff = drift_git_diff(wt_path);
   if (!diff)
      return 0;
   /* drift_git_diff concatenates `git diff --name-only HEAD` + `git ls-files
    * --others --exclude-standard`, so any non-empty content means there is
    * at least one tracked diff or one untracked file in wt_path. */
   int has_changes = 0;
   for (const char *p = diff; *p; p++)
   {
      if (*p != '\n' && *p != ' ' && *p != '\t')
      {
         has_changes = 1;
         break;
      }
   }
   free(diff);
   return has_changes;
}
