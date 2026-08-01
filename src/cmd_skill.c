/* cmd_skill.c: aimee skill — list and show project-scoped skill files */
#include "aimee.h"
#include "commands.h"
#include <aimee/skills/skill.h>
#include "config.h"
#include "cJSON.h"
#include "kb_client.h"
#include <sys/stat.h>
#include <unistd.h>

static void skill_print_help(void)
{
   fprintf(stderr, "Usage: aimee skill <subcommand> [args]\n\n"
                   "Subcommands:\n"
                   "  list [--json]     List available skills\n"
                   "  show <name> [--file references/<path>]\n"
                   "                    Print a skill body or support file\n\n"
                   "  lint <name> | --all\n"
                   "  eval <name> [--json]\n"
                   "  create <name> <file> [--agent]\n"
                   "  edit <name> <file>\n"
                   "  patch <name> <old> <new> [--all]\n"
                   "  archive <name> [--absorbed-into <name>]\n"
                   "  export <name>\n"
                   "  import <file> [--agent]\n"
                   "  rollback <snapshot>\n"
                   "  lifecycle [--json] [--stale-days N] [--archive-days N]\n"
                   "  autostub [--json] [--force] [--snapshot <file>]\n"
                   "  pin <name> | unpin <name>\n\n"
                   "Examples:\n"
                   "  aimee skill list\n"
                   "  aimee skill show security-review\n\n"
                   "Skill files are markdown files stored in:\n"
                   "  .aimee/skills/<name>/SKILL.md or <name>.md          (project-level)\n"
                   "  ~/.config/aimee/skills/<name>/SKILL.md or <name>.md (user-level)\n"
                   "  ~/.local/share/aimee/skills/<name>/SKILL.md        (bundled)\n\n"
                   "Activate a skill in chat with:\n"
                   "  /skill <name>     Inject skill into system prompt\n"
                   "  /skill            Show active skill\n"
                   "  /skill clear      Remove active skill\n");
}

static char *skill_read_arg_file(const char *path)
{
   FILE *f = fopen(path, "r");
   if (!f)
      return NULL;
   if (fseek(f, 0, SEEK_END) != 0)
   {
      fclose(f);
      return NULL;
   }
   long len = ftell(f);
   if (len < 0 || len > SKILL_SUPPORT_MAX_SIZE)
   {
      fclose(f);
      return NULL;
   }
   rewind(f);
   char *buf = malloc((size_t)len + 1);
   if (!buf)
   {
      fclose(f);
      return NULL;
   }
   size_t n = fread(buf, 1, (size_t)len, f);
   if (n != (size_t)len || ferror(f))
   {
      free(buf);
      fclose(f);
      return NULL;
   }
   buf[n] = '\0';
   fclose(f);
   return buf;
}

static void skill_cmd_list(app_ctx_t *ctx, int argc, char **argv)
{
   int json_output = ctx && ctx->json_output;
   for (int i = 0; i < argc; i++)
   {
      if (strcmp(argv[i], "--json") == 0)
         json_output = 1;
      else
      {
         fprintf(stderr, "Usage: aimee skill list [--json]\n");
         return;
      }
   }

   char cwd[MAX_PATH_LEN];
   if (!getcwd(cwd, sizeof(cwd)))
      cwd[0] = '\0';

   char names[SKILL_MAX_SKILLS][SKILL_NAME_MAX];
   int n = skill_list(cwd, names, SKILL_MAX_SKILLS);

   if (n == 0)
   {
      if (json_output)
         fputs("{\"skills\":[]}\n", stdout);
      else
         fprintf(stderr, "No skills found. Create .aimee/skills/<name>.md to add one.\n");
      return;
   }

   cJSON *root = NULL;
   cJSON *arr = NULL;
   if (json_output)
   {
      root = cJSON_CreateObject();
      arr = cJSON_CreateArray();
      if (root && arr)
         cJSON_AddItemToObject(root, "skills", arr);
   }

   for (int i = 0; i < n; i++)
   {
      const char *source = skill_source(cwd, names[i]);
      if (!source)
         source = "user";

      if (json_output && arr)
      {
         skill_usage_t usage;
         skill_usage_get(cwd, names[i], &usage);
         cJSON *item = cJSON_CreateObject();
         cJSON_AddStringToObject(item, "name", names[i]);
         cJSON_AddStringToObject(item, "source", source);
         cJSON_AddNumberToObject(item, "use_count", usage.use_count);
         cJSON_AddNumberToObject(item, "view_count", usage.view_count);
         cJSON_AddNumberToObject(item, "patch_count", usage.patch_count);
         cJSON_AddStringToObject(item, "created_at", usage.created_at);
         cJSON_AddStringToObject(item, "last_used_at", usage.last_used_at);
         cJSON_AddStringToObject(item, "last_patched_at", usage.last_patched_at);
         cJSON_AddStringToObject(item, "state", usage.state);
         cJSON_AddBoolToObject(item, "pinned", usage.pinned);
         cJSON_AddStringToObject(item, "created_by", usage.created_by);
         cJSON_AddItemToArray(arr, item);
      }
      else
         fprintf(stdout, "%-32s  %s\n", names[i], source);
   }

   if (json_output && root)
   {
      char *rendered = cJSON_Print(root);
      if (rendered)
      {
         fputs(rendered, stdout);
         fputc('\n', stdout);
         free(rendered);
      }
      cJSON_Delete(root);
   }
}

static void skill_cmd_show(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;

   if (argc != 1 && argc != 3)
   {
      fprintf(stderr, "Usage: aimee skill show <name> [--file references/<path>]\n");
      return;
   }
   const char *support_file = NULL;
   if (argc == 3)
   {
      if (strcmp(argv[1], "--file") != 0)
      {
         fprintf(stderr, "Usage: aimee skill show <name> [--file references/<path>]\n");
         return;
      }
      support_file = argv[2];
   }

   char cwd[MAX_PATH_LEN];
   if (!getcwd(cwd, sizeof(cwd)))
      cwd[0] = '\0';

   char err[256] = "";
   char *content = support_file
                       ? skill_support_file_load(cwd, argv[0], support_file, err, sizeof(err))
                       : skill_load(cwd, argv[0]);
   if (!content)
   {
      fprintf(stderr, "%s\n", err[0] ? err : "Skill not found");
      return;
   }

   (void)skill_record_view(cwd, argv[0]);
   fputs(content, stdout);
   if (content[strlen(content) - 1] != '\n')
      fputc('\n', stdout);
   free(content);
}

static void skill_cmd_lint(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   if (argc != 1)
   {
      fprintf(stderr, "Usage: aimee skill lint <name> | --all\n");
      exit(2);
   }

   char cwd[MAX_PATH_LEN];
   if (!getcwd(cwd, sizeof(cwd)))
      cwd[0] = '\0';

   int issues = 0;
   if (strcmp(argv[0], "--all") == 0)
   {
      char names[SKILL_MAX_SKILLS][SKILL_NAME_MAX];
      int n = skill_list(cwd, names, SKILL_MAX_SKILLS);
      for (int i = 0; i < n; i++)
      {
         char report[2048];
         int rc = skill_lint(cwd, names[i], report, sizeof(report));
         if (rc > 0)
         {
            fputs(report, stderr);
            issues += rc;
         }
      }
      if (issues == 0)
         printf("skill lint: %d skills passed\n", n);
   }
   else
   {
      char report[2048];
      issues = skill_lint(cwd, argv[0], report, sizeof(report));
      if (issues > 0)
         fputs(report, stderr);
      else
         printf("skill lint: %s passed\n", argv[0]);
   }
   if (issues > 0)
      exit(1);
}

static void skill_cmd_eval(app_ctx_t *ctx, int argc, char **argv)
{
   int json_output = ctx && ctx->json_output;
   const char *name = NULL;
   for (int i = 0; i < argc; i++)
   {
      if (strcmp(argv[i], "--json") == 0)
         json_output = 1;
      else if (!name)
         name = argv[i];
      else
      {
         fprintf(stderr, "Usage: aimee skill eval <name> [--json]\n");
         exit(2);
      }
   }
   if (!name)
   {
      fprintf(stderr, "Usage: aimee skill eval <name> [--json]\n");
      exit(2);
   }

   char cwd[MAX_PATH_LEN], err[256] = "";
   if (!getcwd(cwd, sizeof(cwd)))
      cwd[0] = '\0';

   skill_eval_result_t result;
   if (skill_eval_run(cwd, name, &result, err, sizeof(err)) != 0)
   {
      fprintf(stderr, "%s\n", err[0] ? err : "skill eval failed");
      exit(2);
   }

   if (json_output)
   {
      cJSON *root = cJSON_CreateObject();
      cJSON_AddStringToObject(root, "skill", name);
      cJSON_AddStringToObject(root, "status", result.passed ? "pass" : "fail");
      cJSON_AddBoolToObject(root, "passed", result.passed ? 1 : 0);
      cJSON_AddNumberToObject(root, "scenarios", result.scenarios);
      cJSON_AddNumberToObject(root, "baseline_violations", result.baseline_violations);
      cJSON_AddNumberToObject(root, "baseline_compliances", result.baseline_compliances);
      cJSON_AddNumberToObject(root, "treatment_compliances", result.treatment_compliances);
      cJSON_AddNumberToObject(root, "compliance_delta", result.compliance_delta);
      if (result.first_failure[0])
         cJSON_AddStringToObject(root, "first_failure", result.first_failure);
      char *rendered = cJSON_Print(root);
      if (rendered)
      {
         fputs(rendered, stdout);
         fputc('\n', stdout);
         free(rendered);
      }
      cJSON_Delete(root);
   }
   else
   {
      printf("skill eval: %s %s\n", name, result.passed ? "PASS" : "FAIL");
      printf("scenarios: %d\n", result.scenarios);
      printf("baseline violations: %d/%d\n", result.baseline_violations, result.scenarios);
      printf("treatment compliance: %d/%d\n", result.treatment_compliances, result.scenarios);
      printf("compliance delta: %.2f\n", result.compliance_delta);
      if (result.first_failure[0])
         printf("first failure: %s\n", result.first_failure);
   }

   if (!result.passed)
      exit(1);
}

static void skill_cmd_create(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   if (argc < 2)
   {
      fprintf(stderr, "Usage: aimee skill create <name> <file> [--agent]\n");
      return;
   }
   const char *actor = argc > 2 && strcmp(argv[2], "--agent") == 0 ? "agent" : "user";
   char cwd[MAX_PATH_LEN], err[256] = "";
   if (!getcwd(cwd, sizeof(cwd)))
      cwd[0] = '\0';
   char *content = skill_read_arg_file(argv[1]);
   if (!content)
   {
      fprintf(stderr, "Failed to read skill file: %s\n", argv[1]);
      return;
   }
   char report[2048];
   int issues = skill_lint_content(argv[0], content, report, sizeof(report));
   if (issues > 0)
   {
      fputs(report, stderr);
      free(content);
      exit(1);
   }
   int rc = skill_manage_create(cwd, argv[0], content, actor, err, sizeof(err));
   free(content);
   if (rc != 0)
      fprintf(stderr, "%s\n", err[0] ? err : "skill create failed");
}

static void skill_cmd_edit(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   if (argc != 2)
   {
      fprintf(stderr, "Usage: aimee skill edit <name> <file>\n");
      return;
   }
   char cwd[MAX_PATH_LEN], err[256] = "";
   if (!getcwd(cwd, sizeof(cwd)))
      cwd[0] = '\0';
   char *content = skill_read_arg_file(argv[1]);
   if (!content)
   {
      fprintf(stderr, "Failed to read skill file: %s\n", argv[1]);
      return;
   }
   int rc = skill_manage_edit(cwd, argv[0], content, "user", err, sizeof(err));
   free(content);
   if (rc != 0)
      fprintf(stderr, "%s\n", err[0] ? err : "skill edit failed");
}

static void skill_cmd_patch(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   if (argc < 3)
   {
      fprintf(stderr, "Usage: aimee skill patch <name> <old> <new> [--all]\n");
      return;
   }
   int all = argc > 3 && strcmp(argv[3], "--all") == 0;
   char cwd[MAX_PATH_LEN], err[256] = "";
   if (!getcwd(cwd, sizeof(cwd)))
      cwd[0] = '\0';
   if (skill_manage_patch(cwd, argv[0], argv[1], argv[2], all, "user", err, sizeof(err)) != 0)
      fprintf(stderr, "%s\n", err[0] ? err : "skill patch failed");
}

static void skill_cmd_archive(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   if (argc < 1)
   {
      fprintf(stderr, "Usage: aimee skill archive <name> [--absorbed-into <name>]\n");
      return;
   }
   const char *absorbed = argc > 2 && strcmp(argv[1], "--absorbed-into") == 0 ? argv[2] : NULL;
   char cwd[MAX_PATH_LEN], err[256] = "";
   if (!getcwd(cwd, sizeof(cwd)))
      cwd[0] = '\0';
   if (skill_manage_archive(cwd, argv[0], absorbed, err, sizeof(err)) != 0)
      fprintf(stderr, "%s\n", err[0] ? err : "skill archive failed");
}

static void skill_cmd_export(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   if (argc != 1)
   {
      fprintf(stderr, "Usage: aimee skill export <name>\n");
      return;
   }
   char cwd[MAX_PATH_LEN];
   if (!getcwd(cwd, sizeof(cwd)))
      cwd[0] = '\0';
   char *content = skill_load(cwd, argv[0]);
   if (!content)
   {
      fprintf(stderr, "Skill not found: %s\n", argv[0]);
      return;
   }
   fputs(content, stdout);
   if (content[0] && content[strlen(content) - 1] != '\n')
      fputc('\n', stdout);
   free(content);
}

static void skill_cmd_import(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   if (argc < 1 || argc > 2 || (argc == 2 && strcmp(argv[1], "--agent") != 0))
   {
      fprintf(stderr, "Usage: aimee skill import <file> [--agent]\n");
      return;
   }
   char cwd[MAX_PATH_LEN], err[256] = "", name[SKILL_NAME_MAX] = "";
   if (!getcwd(cwd, sizeof(cwd)))
      cwd[0] = '\0';
   char *content = skill_read_arg_file(argv[0]);
   if (!content)
   {
      fprintf(stderr, "Failed to read skill file: %s\n", argv[0]);
      return;
   }
   const char *actor = argc == 2 ? "agent" : "user";
   int rc = skill_import_content(cwd, content, actor, name, sizeof(name), err, sizeof(err));
   free(content);
   if (rc != 0)
      fprintf(stderr, "%s\n", err[0] ? err : "skill import failed");
   else
      printf("skill import: %s\n", name);
}

static void skill_cmd_pin(app_ctx_t *ctx, int argc, char **argv, int pinned)
{
   (void)ctx;
   if (argc != 1)
   {
      fprintf(stderr, "Usage: aimee skill %s <name>\n", pinned ? "pin" : "unpin");
      return;
   }
   char cwd[MAX_PATH_LEN];
   if (!getcwd(cwd, sizeof(cwd)))
      cwd[0] = '\0';
   if (skill_set_pinned(cwd, argv[0], pinned) != 0)
      fprintf(stderr, "Skill not found: %s\n", argv[0]);
}

static void skill_cmd_rollback(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   if (argc != 1)
   {
      fprintf(stderr, "Usage: aimee skill rollback <snapshot>\n");
      return;
   }
   char err[256] = "";
   char cwd[MAX_PATH_LEN];
   if (!getcwd(cwd, sizeof(cwd)))
      cwd[0] = '\0';
   if (skill_rollback_snapshot(cwd, argv[0], err, sizeof(err)) != 0)
      fprintf(stderr, "%s\n", err[0] ? err : "skill rollback failed");
   else
      printf("skill rollback: %s\n", argv[0]);
}

static int skill_arg_int(const char *s, int *out)
{
   if (!s || !s[0])
      return 0;
   char *end = NULL;
   long v = strtol(s, &end, 10);
   if (!end || *end != '\0' || v <= 0 || v >= 100000)
      return 0;
   if (out)
      *out = (int)v;
   return 1;
}

static void skill_cmd_lifecycle(app_ctx_t *ctx, int argc, char **argv)
{
   int json_output = ctx && ctx->json_output;
   int stale_days = config_skills_stale_after_days();
   int archive_days = config_skills_archive_after_days();
   for (int i = 0; i < argc; i++)
   {
      if (strcmp(argv[i], "--json") == 0)
         json_output = 1;
      else if (strcmp(argv[i], "--stale-days") == 0 && i + 1 < argc)
      {
         if (!skill_arg_int(argv[++i], &stale_days))
         {
            fprintf(stderr, "Invalid --stale-days value: %s\n", argv[i]);
            return;
         }
      }
      else if (strcmp(argv[i], "--archive-days") == 0 && i + 1 < argc)
      {
         if (!skill_arg_int(argv[++i], &archive_days))
         {
            fprintf(stderr, "Invalid --archive-days value: %s\n", argv[i]);
            return;
         }
      }
      else
      {
         fprintf(stderr,
                 "Usage: aimee skill lifecycle [--json] [--stale-days N] [--archive-days N]\n");
         return;
      }
   }

   char cwd[MAX_PATH_LEN], err[256] = "";
   if (!getcwd(cwd, sizeof(cwd)))
      cwd[0] = '\0';
   skill_lifecycle_result_t result;
   int rc = skill_lifecycle_apply(cwd, stale_days, archive_days, &result, err, sizeof(err));
   if (json_output)
   {
      printf("{\"status\":\"%s\",\"considered\":%d,\"stale_marked\":%d,\"archived\":%d,"
             "\"skipped_pinned\":%d,\"errors\":%d}\n",
             rc == 0 ? "ok" : "error", result.considered, result.stale_marked, result.archived,
             result.skipped_pinned, result.errors);
   }
   else if (rc == 0)
      printf("skill lifecycle: %d considered, %d stale, %d archived, %d pinned skipped\n",
             result.considered, result.stale_marked, result.archived, result.skipped_pinned);
   else
      fprintf(stderr, "%s\n", err[0] ? err : "skill lifecycle failed");
}

static void skill_cmd_autostub(app_ctx_t *ctx, int argc, char **argv)
{
   int json_output = ctx && ctx->json_output;
   int force = 0;
   const char *snapshot_path = NULL;
   for (int i = 0; i < argc; i++)
   {
      if (strcmp(argv[i], "--json") == 0)
         json_output = 1;
      else if (strcmp(argv[i], "--force") == 0)
         force = 1;
      else if (strcmp(argv[i], "--snapshot") == 0 && i + 1 < argc)
         snapshot_path = argv[++i];
      else
      {
         fprintf(stderr, "Usage: aimee skill autostub [--json] [--force] [--snapshot <file>]\n");
         return;
      }
   }
   if (!config_skills_capability_autostub() && !force)
   {
      if (json_output)
         printf("{\"status\":\"disabled\",\"reason\":\"skills.capability.autostub is false\"}\n");
      else
         printf("skill autostub: disabled (set skills.capability.autostub or pass --force)\n");
      return;
   }

   char *snapshot =
       snapshot_path ? skill_read_arg_file(snapshot_path) : kb_client_tool_registry_snapshot_json();
   if (!snapshot)
   {
      fprintf(stderr, "skill autostub: failed to read tool registry snapshot\n");
      exit(1);
   }

   char cwd[MAX_PATH_LEN], err[256] = "";
   if (!getcwd(cwd, sizeof(cwd)))
      cwd[0] = '\0';

   skill_capability_autostub_result_t result;
   int rc = skill_capability_autostub_from_json(cwd, snapshot, &result, err, sizeof(err));
   free(snapshot);
   if (json_output)
   {
      cJSON *root = cJSON_CreateObject();
      cJSON_AddStringToObject(root, "status", rc == 0 ? "ok" : "error");
      cJSON_AddNumberToObject(root, "scanned", result.scanned);
      cJSON_AddNumberToObject(root, "existing", result.existing);
      cJSON_AddNumberToObject(root, "proposed", result.proposed);
      cJSON_AddNumberToObject(root, "skipped", result.skipped);
      cJSON_AddNumberToObject(root, "errors", result.errors);
      if (result.first_proposal[0])
         cJSON_AddStringToObject(root, "first_proposal", result.first_proposal);
      if (result.first_change_path[0])
         cJSON_AddStringToObject(root, "first_change_path", result.first_change_path);
      if (rc != 0)
         cJSON_AddStringToObject(root, "message", err[0] ? err : "skill autostub failed");
      char *rendered = cJSON_Print(root);
      if (rendered)
      {
         fputs(rendered, stdout);
         fputc('\n', stdout);
         free(rendered);
      }
      cJSON_Delete(root);
   }
   else if (rc == 0)
      printf("skill autostub: %d scanned, %d covered, %d proposed, %d skipped\n", result.scanned,
             result.existing, result.proposed, result.skipped);
   else
      fprintf(stderr, "%s\n", err[0] ? err : "skill autostub failed");
   if (rc != 0)
      exit(1);
}

void cmd_skill(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
   {
      skill_print_help();
      return;
   }

   const char *sub = argv[0];
   argc--;
   argv++;

   if (subcmd_dispatch(get_skill_subcmds(), sub, ctx, argc, argv) != 0)
   {
      fprintf(stderr, "Unknown skill subcommand: %s\n\n", sub);
      skill_print_help();
   }
}

static void skill_cmd_pin_sub(app_ctx_t *ctx, int argc, char **argv)
{
   skill_cmd_pin(ctx, argc, argv, 1);
}

static void skill_cmd_unpin_sub(app_ctx_t *ctx, int argc, char **argv)
{
   skill_cmd_pin(ctx, argc, argv, 0);
}

const subcmd_t *get_skill_subcmds(void)
{
   static const subcmd_t skill_subcmds[] = {
       {"list", "List available skills", skill_cmd_list},
       {"show", "Print a skill body or support file", skill_cmd_show},
       {"lint", "Lint skill frontmatter and authoring conventions", skill_cmd_lint},
       {"eval", "Run skill compliance eval fixtures", skill_cmd_eval},
       {"create", "Create a project skill from a markdown file", skill_cmd_create},
       {"edit", "Replace a project skill from a markdown file", skill_cmd_edit},
       {"patch", "Patch a project skill by string replacement", skill_cmd_patch},
       {"archive", "Archive a project skill", skill_cmd_archive},
       {"export", "Export a skill as SKILL.md markdown", skill_cmd_export},
       {"import", "Import a frontmatter SKILL.md markdown file", skill_cmd_import},
       {"rollback", "Restore project skills from a snapshot", skill_cmd_rollback},
       {"lifecycle", "Apply stale/archive lifecycle transitions", skill_cmd_lifecycle},
       {"autostub", "Propose capability skills for uncovered tools", skill_cmd_autostub},
       {"pin", "Pin a skill against automation", skill_cmd_pin_sub},
       {"unpin", "Unpin a skill", skill_cmd_unpin_sub},
       {NULL, NULL, NULL},
   };
   return skill_subcmds;
}
