/* cmd_describe.c: auto-describe projects via agent delegation */
#include "aimee.h"
#include "platform_path.h"
#include "commands.h"
#include <aimee/workspace/workspace.h>
#include "agent.h"
#include "config.h"
#include "cmd_describe_platform.h"
#include "kb_client.h"
#include "cJSON.h"
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

/* --- description file paths --- */

static void describe_dir(char *buf, size_t len)
{
   snprintf(buf, len, "%s/projects", config_default_dir());
}

static void describe_path(const char *project_name, char *buf, size_t len)
{
   snprintf(buf, len, "%s/projects/%s.md", config_default_dir(), project_name);
}

/* --- read existing description --- */

char *describe_read(const char *project_name)
{
   char path[MAX_PATH_LEN];
   describe_path(project_name, path, sizeof(path));

   FILE *f = fopen(path, "r");
   if (!f)
      return NULL;

   fseek(f, 0, SEEK_END);
   long sz = ftell(f);
   fseek(f, 0, SEEK_SET);

   if (sz <= 0 || sz > MAX_FILE_SIZE)
   {
      fclose(f);
      return NULL;
   }

   char *buf = malloc((size_t)sz + 1);
   if (!buf)
   {
      fclose(f);
      return NULL;
   }

   size_t n = fread(buf, 1, (size_t)sz, f);
   buf[n] = '\0';
   fclose(f);
   return buf;
}

/* --- extract commit from frontmatter --- */

static int extract_commit(const char *content, char *out, size_t out_len)
{
   const char *p = strstr(content, "commit: ");
   if (!p)
      return -1;
   p += 8;
   const char *end = strchr(p, '\n');
   if (!end)
      return -1;
   size_t len = (size_t)(end - p);
   if (len >= out_len)
      len = out_len - 1;
   memcpy(out, p, len);
   out[len] = '\0';
   return 0;
}

/* --- get HEAD commit for a project --- */

static int get_head_commit(const char *project_path, char *out, size_t out_len)
{
   const char *argv[] = {"git", "-C", project_path, "rev-parse", "--short", "HEAD", NULL};
   char *result = NULL;
   int rc = safe_exec_capture(argv, &result, 256);
   if (rc != 0 || !result)
   {
      free(result);
      return -1;
   }
   /* strip trailing newline */
   size_t len = strlen(result);
   while (len > 0 && (result[len - 1] == '\n' || result[len - 1] == '\r'))
      result[--len] = '\0';
   snprintf(out, out_len, "%s", result);
   free(result);
   return 0;
}

/* --- check if project has structural changes since last describe --- */

static int has_structural_changes(const char *project_path, const char *old_commit)
{
   /* diff --name-only between old commit and HEAD, filter for structural files */
   char range[128];
   snprintf(range, sizeof(range), "%s..HEAD", old_commit);

   const char *argv[] = {"git", "-C", project_path, "diff", "--name-only", range, NULL};
   char *result = NULL;
   int rc = safe_exec_capture(argv, &result, 8192);
   if (rc != 0 || !result)
   {
      free(result);
      return 1; /* assume changes if we can't check */
   }

   if (!result[0])
   {
      free(result);
      return 0; /* no changes at all */
   }

   /* Check for structural changes: new/deleted files at top level,
    * build files, config files, new directories */
   int structural = 0;
   char *line = result;
   while (line && *line)
   {
      char *nl = strchr(line, '\n');
      if (nl)
         *nl = '\0';

      /* Top-level new files (no directory separator before first component) */
      const char *slash = strchr(line, '/');

      /* Build/config files anywhere */
      const char *base = slash ? strrchr(line, '/') + 1 : line;
      if (strcmp(base, "Makefile") == 0 || strcmp(base, "CMakeLists.txt") == 0 ||
          strcmp(base, "package.json") == 0 || strcmp(base, "Cargo.toml") == 0 ||
          strcmp(base, "go.mod") == 0 || strcmp(base, "pyproject.toml") == 0 ||
          strcmp(base, "setup.py") == 0 || strcmp(base, "README.md") == 0 ||
          strstr(base, ".csproj") || strstr(base, ".sln"))
      {
         structural = 1;
         break;
      }

      /* New top-level directory (file path has exactly one slash at a shallow level) */
      if (slash && !strchr(slash + 1, '/'))
      {
         /* A new file directly under the project root - could indicate new module */
      }

      /* Header files (new APIs/modules) */
      size_t linelen = strlen(line);
      if (linelen > 2 && strcmp(line + linelen - 2, ".h") == 0)
      {
         structural = 1;
         break;
      }

      line = nl ? nl + 1 : NULL;
   }

   free(result);
   return structural;
}

/* --- check if project needs describing (returns 1 if work needed) --- */

static int needs_describe(const char *project_name, const char *project_path, int force)
{
   if (force)
      return 1;

   char *existing = describe_read(project_name);
   if (!existing)
      return 1;

   char old_commit[64] = {0};
   char head_commit[64] = {0};

   if (extract_commit(existing, old_commit, sizeof(old_commit)) == 0 &&
       get_head_commit(project_path, head_commit, sizeof(head_commit)) == 0)
   {
      if (strcmp(old_commit, head_commit) == 0)
      {
         fprintf(stderr, "describe: %s is up to date (%s)\n", project_name, head_commit);
         free(existing);
         return 0;
      }

      if (!has_structural_changes(project_path, old_commit))
      {
         fprintf(stderr, "describe: %s has no structural changes since %s, skipping\n",
                 project_name, old_commit);
         free(existing);
         return 0;
      }
   }

   free(existing);
   return 1;
}

/* --- describe prompt --- */

/* Render the repository-owned prompt templates without treating their bytes as
 * a printf program.  Only the exact "%s" placeholders are substituted; a
 * future stray '%' remains ordinary text. */
static char *render_prompt_template(const char *template, const char *const *values,
                                    size_t value_count)
{
   size_t total = strlen(template) + 1;
   const char *scan = template;
   size_t placeholders = 0;
   while ((scan = strstr(scan, "%s")) != NULL)
   {
      if (placeholders >= value_count)
         return NULL;
      size_t value_len = strlen(values[placeholders]);
      if (total > SIZE_MAX - value_len + 2)
         return NULL;
      total += value_len - 2;
      placeholders++;
      scan += 2;
   }
   if (placeholders != value_count)
      return NULL;

   char *out = malloc(total);
   if (!out)
      return NULL;
   char *dst = out;
   scan = template;
   for (size_t i = 0; i < value_count; i++)
   {
      const char *placeholder = strstr(scan, "%s");
      size_t literal_len = (size_t)(placeholder - scan);
      memcpy(dst, scan, literal_len);
      dst += literal_len;
      size_t value_len = strlen(values[i]);
      memcpy(dst, values[i], value_len);
      dst += value_len;
      scan = placeholder + 2;
   }
   strcpy(dst, scan);
   return out;
}

static const char *describe_system_prompt =
    "You are a code analyst. Your job is to produce a structured project description "
    "that gives other AI agents everything they need to navigate and work in a project "
    "without searching or exploring.\n\n"
    "Be thorough but concise. Only describe what exists in the code today. "
    "Do not speculate about future plans or aspirational features.\n"
    "Do not wrap your output in markdown code fences.\n\n"
    "IMPORTANT: Only describe files and directories that are tracked by git. "
    "Run `git ls-files` or `git ls-tree -r --name-only HEAD` to see what is tracked. "
    "Do NOT describe build output, binaries, .gitignored files, or untracked directories. "
    "If a directory only contains .gitignored content, omit it entirely.";

static const char *describe_user_template =
    "Analyze the project at %s and produce a structured description.\n\n"
    "Use the tools to:\n"
    "1. List the directory structure (top level and one level down in key dirs)\n"
    "2. Read build files (Makefile, package.json, Cargo.toml, etc.)\n"
    "3. Read entry point files to understand what the program does\n"
    "4. Read key source files to understand architecture\n\n"
    "Output the description in EXACTLY this format (no code fences):\n\n"
    "---\n"
    "project: %s\n"
    "generated: YYYY-MM-DDTHH:MM:SSZ\n"
    "commit: %s\n"
    "---\n\n"
    "# %s\n\n"
    "<2-3 sentence summary of what this project is and does>\n\n"
    "Type: <cli|server|library|webapp|game|infrastructure|tool|other>\n"
    "Language: <primary language(s)>\n"
    "Build: <exact build command>\n"
    "Test: <exact test command, or \"none\" if no tests>\n"
    "Lint: <exact lint command, or \"none\">\n\n"
    "## Entry Points\n"
    "- <file:function()> -- <what it does>\n\n"
    "## Directory Layout\n"
    "- <dir/> -- <purpose>\n\n"
    "## Key Files\n"
    "- <file> -- <one-line role>\n\n"
    "## Architecture\n"
    "<How the major components connect. What calls what. Data flow.\n"
    "Keep to 1-2 short paragraphs.>\n\n"
    "## Conventions\n"
    "<Naming patterns, code style, patterns used. 2-4 bullet points.>";

/* --- describe a single project (runs in child process) --- */

int describe_one(const char *project_name, const char *project_path)
{
   /* Get HEAD commit */
   char head[64] = {0};
   if (get_head_commit(project_path, head, sizeof(head)) != 0)
      snprintf(head, sizeof(head), "unknown");

   /* Build the prompt */
   const char *values[] = {project_path, project_name, head, project_name};
   char *prompt =
       render_prompt_template(describe_user_template, values, sizeof values / sizeof values[0]);
   if (!prompt)
      return -1;

   fprintf(stderr, "describe: analyzing %s at %s...\n", project_name, head);

   agent_config_t cfg;
   if (agent_load_config(&cfg) != 0 || cfg.agent_count == 0)
   {
      free(prompt);
      return -1;
   }

   /* Delegate to an agent with tools */
   agent_http_init();

   agent_result_t result;
   memset(&result, 0, sizeof(result));

   int rc = agent_run_with_tools(&cfg, "execute", describe_system_prompt, prompt, 4096, &result);

   free(prompt);

   if (rc != 0 || !result.response || !result.response[0])
   {
      fprintf(stderr, "describe: failed for %s: %s\n", project_name,
              result.error[0] ? result.error : "no response");
      agent_http_cleanup();
      free(result.response);
      return -1;
   }

   /* Ensure projects directory exists */
   char dir[MAX_PATH_LEN];
   describe_dir(dir, sizeof(dir));
   platform_mkdir_p(dir, 0755);

   /* Write description file */
   char path[MAX_PATH_LEN];
   describe_path(project_name, path, sizeof(path));

   FILE *f = fopen(path, "w");
   if (!f)
   {
      fprintf(stderr, "describe: cannot write %s: %s\n", path, strerror(errno));
      free(result.response);
      agent_http_cleanup();
      return -1;
   }

   fprintf(f, "%s\n", result.response);
   fclose(f);

   fprintf(stderr, "describe: wrote %s (%d turns, %d tool calls)\n", path, result.turns,
           result.tool_calls);

   free(result.response);
   agent_http_cleanup();
   return 0;
}

/* --- style analysis --- */

static void style_path(const char *project_name, char *buf, size_t len)
{
   snprintf(buf, len, "%s/projects/%s.style.md", config_default_dir(), project_name);
}

static int needs_style(const char *project_name, const char *project_path, int force)
{
   if (force)
      return 1;

   char *existing = style_read(project_name);
   if (!existing)
      return 1;

   char old_commit[64] = {0};
   char head_commit[64] = {0};

   if (extract_commit(existing, old_commit, sizeof(old_commit)) == 0 &&
       get_head_commit(project_path, head_commit, sizeof(head_commit)) == 0)
   {
      if (strcmp(old_commit, head_commit) == 0)
      {
         fprintf(stderr, "style: %s is up to date (%s)\n", project_name, head_commit);
         free(existing);
         return 0;
      }

      if (!has_structural_changes(project_path, old_commit))
      {
         fprintf(stderr, "style: %s has no structural changes since %s, skipping\n", project_name,
                 old_commit);
         free(existing);
         return 0;
      }
   }

   free(existing);
   return 1;
}

static const char *style_system_prompt =
    "You are a code style analyst. Your job is to analyze a project's source code "
    "and produce a structured coding style guide that other AI agents can follow "
    "when writing code in this project.\n\n"
    "Be precise and concrete. Report what the code actually does, not opinions. "
    "Only analyze files tracked by git. "
    "Do not wrap your output in markdown code fences.";

static const char *style_user_template =
    "Analyze the coding style of the project at %s.\n\n"
    "Use the tools to:\n"
    "1. Run `git -C %s ls-files` to see tracked source files\n"
    "2. Read 5-8 representative source files (entry points, core modules, utilities, tests)\n"
    "3. Identify concrete coding conventions\n\n"
    "Output the style guide in EXACTLY this format (no code fences):\n\n"
    "---\n"
    "project: %s\n"
    "commit: %s\n"
    "---\n\n"
    "# Style Guide: %s\n\n"
    "## Indentation\n"
    "- <tabs or spaces>, <width> per level\n\n"
    "## Naming Conventions\n"
    "- Functions: <snake_case|camelCase|PascalCase>\n"
    "- Variables: <convention>\n"
    "- Types/Structs: <convention>\n"
    "- Constants/Macros: <convention>\n"
    "- Files: <convention>\n\n"
    "## Braces & Formatting\n"
    "- Brace style: <K&R|Allman|same-line|other>\n"
    "- Max line length: ~<N> characters\n"
    "- Blank lines between functions: <yes|no>\n\n"
    "## Strings & Literals\n"
    "- Quote style: <single|double|N/A>\n"
    "- Semicolons: <always|never|N/A>\n\n"
    "## Imports / Includes\n"
    "- Order: <description of ordering convention>\n"
    "- Style: <description>\n\n"
    "## Error Handling\n"
    "- Pattern: <return codes|exceptions|Result type|other>\n"
    "- <additional details>\n\n"
    "## Comments\n"
    "- Style: <// preferred|/* */ preferred|doc comments|other>\n"
    "- When used: <description>\n\n"
    "## Patterns\n"
    "- <2-4 bullet points on recurring code patterns: guard clauses, early returns, etc.>";

int style_one(const char *project_name, const char *project_path)
{
   char head[64] = {0};
   if (get_head_commit(project_path, head, sizeof(head)) != 0)
      snprintf(head, sizeof(head), "unknown");

   /* Build the prompt (5 format args: path, path, name, head, name) */
   const char *values[] = {project_path, project_path, project_name, head, project_name};
   char *prompt =
       render_prompt_template(style_user_template, values, sizeof values / sizeof values[0]);
   if (!prompt)
      return -1;

   fprintf(stderr, "style: analyzing %s at %s...\n", project_name, head);

   agent_config_t cfg;
   if (agent_load_config(&cfg) != 0 || cfg.agent_count == 0)
   {
      free(prompt);
      return -1;
   }

   agent_http_init();

   agent_result_t result;
   memset(&result, 0, sizeof(result));

   int rc = agent_run_with_tools(&cfg, "execute", style_system_prompt, prompt, 4096, &result);

   free(prompt);

   if (rc != 0 || !result.response || !result.response[0])
   {
      fprintf(stderr, "style: failed for %s: %s\n", project_name,
              result.error[0] ? result.error : "no response");
      agent_http_cleanup();
      free(result.response);
      return -1;
   }

   /* Ensure projects directory exists */
   char dir[MAX_PATH_LEN];
   describe_dir(dir, sizeof(dir));
   platform_mkdir_p(dir, 0755);

   /* Write style file */
   char path[MAX_PATH_LEN];
   style_path(project_name, path, sizeof(path));

   FILE *f = fopen(path, "w");
   if (!f)
   {
      fprintf(stderr, "style: cannot write %s: %s\n", path, strerror(errno));
      free(result.response);
      agent_http_cleanup();
      return -1;
   }

   fprintf(f, "%s\n", result.response);
   fclose(f);

   fprintf(stderr, "style: wrote %s (%d turns, %d tool calls)\n", path, result.turns,
           result.tool_calls);

   free(result.response);
   agent_http_cleanup();
   return 0;
}

/* --- parallel execution with concurrency cap --- */

/* describe_job_t is defined in cmd_describe_platform.h */

/* --- cmd_describe --- */

void cmd_describe(app_ctx_t *ctx, int argc, char **argv)
{
   static const char *bool_flags[] = {"force", "style", "all", NULL};
   opt_parsed_t opts;
   opt_parse(argc, argv, bool_flags, &opts);

   int force = opt_has(&opts, "force");
   int do_style = opt_has(&opts, "style");
   int do_all = opt_has(&opts, "all");
   int retry_count = opt_get_int(&opts, "retry", 0);
   const char *target = opt_pos(&opts, 0);

   /* Get indexed projects from DB2's canonical index. */
   project_info_t all_projects[256];
   int pcount = kb_client_index_list(all_projects, 256);

   if (pcount == 0)
      fatal("no indexed projects found. Run 'aimee workspace add <path>' first.");

   /* Collect jobs that need work */
   describe_job_t jobs[256];
   memset(jobs, 0, sizeof(jobs));
   int job_count = 0;

   for (int i = 0; i < pcount; i++)
   {
      if (target && strcmp(target, all_projects[i].name) != 0)
         continue;

      struct stat st;
      if (stat(all_projects[i].root, &st) != 0 || !S_ISDIR(st.st_mode))
      {
         fprintf(stderr, "describe: %s not found at %s, skipping\n", all_projects[i].name,
                 all_projects[i].root);
         continue;
      }

      if (do_style)
      {
         if (!needs_style(all_projects[i].name, all_projects[i].root, force))
            continue;
      }
      else
      {
         if (!needs_describe(all_projects[i].name, all_projects[i].root, force))
            continue;
      }

      snprintf(jobs[job_count].name, sizeof(jobs[job_count].name), "%s", all_projects[i].name);
      snprintf(jobs[job_count].path, sizeof(jobs[job_count].path), "%s", all_projects[i].root);
      job_count++;
   }

   if (target && job_count == 0)
   {
      /* Check if project exists at all (might have been skipped as up-to-date) */
      int found = 0;
      for (int i = 0; i < pcount; i++)
      {
         if (strcmp(all_projects[i].name, target) == 0)
         {
            found = 1;
            break;
         }
      }
      if (!found)
         fatal("project '%s' not found in index", target);
      /* Otherwise it was up to date */
      if (ctx->json_output)
      {
         cJSON *obj = cJSON_CreateObject();
         cJSON_AddNumberToObject(obj, "described", 0);
         cJSON_AddNumberToObject(obj, "failed", 0);
         emit_json_ctx(obj, ctx->json_fields, ctx->response_profile);
      }
      return;
   }

   if (job_count == 0)
   {
      fprintf(stderr, "describe: all projects up to date\n");
      if (ctx->json_output)
      {
         cJSON *obj = cJSON_CreateObject();
         cJSON_AddNumberToObject(obj, "described", 0);
         cJSON_AddNumberToObject(obj, "failed", 0);
         emit_json_ctx(obj, ctx->json_fields, ctx->response_profile);
      }
      return;
   }

   /* Close parent's DB before forking (children open their own) */
   /* We haven't opened one yet in this path, so nothing to close */

   /* Ensure projects directory exists before forking */
   char dir[MAX_PATH_LEN];
   describe_dir(dir, sizeof(dir));
   platform_mkdir_p(dir, 0755);

   int described = 0;
   int failed = 0;
   int attempts = retry_count + 1;

   for (int attempt = 0; attempt < attempts; attempt++)
   {
      if (attempt > 0)
      {
         /* Rebuild job list with only failed projects */
         describe_job_t retry_jobs[256];
         int retry_count_actual = 0;

         for (int i = 0; i < job_count; i++)
         {
            /* Check if description file now exists (succeeded on prior attempt) */
            char *existing = describe_read(jobs[i].name);
            if (existing)
            {
               free(existing);
               continue;
            }
            retry_jobs[retry_count_actual++] = jobs[i];
         }

         if (retry_count_actual == 0)
            break;

         memcpy(jobs, retry_jobs, sizeof(describe_job_t) * (size_t)retry_count_actual);
         job_count = retry_count_actual;
         failed = 0; /* reset for this attempt */

         fprintf(stderr, "describe: retry %d/%d (%d projects remaining)...\n", attempt,
                 attempts - 1, job_count);
      }

      /* Determine per-agent parallelism cap */
      int max_parallel = DESCRIBE_MAX_PARALLEL_FALLBACK;
      {
         agent_config_t cfg;
         if (agent_load_config(&cfg) == 0)
         {
            agent_t *ag = agent_route(&cfg, "execute");
            if (ag && ag->max_parallel > 0)
               max_parallel = ag->max_parallel;
         }
         if (max_parallel > DESCRIBE_MAX_PARALLEL_CAP)
            max_parallel = DESCRIBE_MAX_PARALLEL_CAP;
         {
            int compute_cap = aimee_resolve_compute_threads(0);
            if (max_parallel > compute_cap)
               max_parallel = compute_cap;
         }
      }

      platform_describe_run_parallel(jobs, job_count, max_parallel, do_style, attempt, attempts,
                                     &described, &failed);
   }

   if (ctx->json_output)
   {
      cJSON *obj = cJSON_CreateObject();
      cJSON_AddNumberToObject(obj, "described", described);
      cJSON_AddNumberToObject(obj, "failed", failed);
      emit_json_ctx(obj, ctx->json_fields, ctx->response_profile);
   }
   else
   {
      const char *label = do_style ? "style" : "describe";
      fprintf(stderr, "%s: %d completed, %d failed\n", label, described, failed);
   }

   /* If --all, run style analysis as a second pass */
   if (do_all && !do_style)
   {
      /* Build argv for recursive call with --style */
      int new_argc = 0;
      char *new_argv[16];
      new_argv[new_argc++] = "--style";
      if (force)
         new_argv[new_argc++] = "--force";
      if (target)
         new_argv[new_argc++] = (char *)target;
      cmd_describe(ctx, new_argc, new_argv);
   }
}
