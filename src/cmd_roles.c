/* cmd_roles.c: aimee roles — list, show, and reset delegate role prompt templates */
#include "aimee.h"
#include "commands.h"
#include "config.h"
#include "platform_path.h"
#include "role_templates.h"
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static void roles_print_help(void)
{
   fprintf(stderr,
           "Usage: aimee roles <subcommand> [args]\n\n"
           "Subcommands:\n"
           "  list              List available role templates\n"
           "  show <role>       Print the effective template for a role\n"
           "  edit <role>       Open the user-level template in $EDITOR\n"
           "  reset <role>      Remove user-level override (restore built-in default)\n"
           "  install           Install built-in defaults to ~/.config/aimee/role_templates/\n\n"
           "Examples:\n"
           "  aimee roles list\n"
           "  aimee roles show review\n"
           "  aimee roles edit code\n"
           "  aimee roles reset code\n"
           "  aimee roles install\n\n"
           "Templates are loaded in priority order:\n"
           "  1. .aimee/role_templates/<role>.md  (project)\n"
           "  2. ~/.config/aimee/role_templates/<role>.md  (user)\n"
           "  3. Built-in default\n");
}

/* aimee roles list */
static void cmd_roles_list(app_ctx_t *ctx, int argc, char **argv)
{
   /* Determine project root from cwd */
   char cwd[MAX_PATH_LEN];
   if (!getcwd(cwd, sizeof(cwd)))
      cwd[0] = '\0';

   char names[ROLE_TEMPLATE_MAX_ROLES][ROLE_TEMPLATE_NAME_MAX];
   int n = role_template_list(cwd, names, ROLE_TEMPLATE_MAX_ROLES);

   char user_dir[ROLE_TEMPLATE_PATH_MAX];
   snprintf(user_dir, sizeof(user_dir), "%s/role_templates", config_default_dir());

   for (int i = 0; i < n; i++)
   {
      /* Show source: project, user, or built-in */
      char path[ROLE_TEMPLATE_PATH_MAX];
      const char *source = "built-in";

      if (cwd[0])
      {
         char proj[ROLE_TEMPLATE_PATH_MAX];
         snprintf(proj, sizeof(proj), "%s/.aimee/role_templates/%s.md", cwd, names[i]);
         struct stat st;
         if (stat(proj, &st) == 0)
            source = "project";
      }

      if (strcmp(source, "built-in") == 0)
      {
         snprintf(path, sizeof(path), "%s/%s.md", user_dir, names[i]);
         struct stat st;
         if (stat(path, &st) == 0)
            source = "user";
      }

      fprintf(stdout, "%-16s  %s\n", names[i], source);
   }
   return;
}

/* aimee roles show <role> */
static void cmd_roles_show(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 2)
   {
      fprintf(stderr, "Usage: aimee roles show <role>\n");
      return;
   }
   const char *role = argv[1];

   char cwd[MAX_PATH_LEN];
   if (!getcwd(cwd, sizeof(cwd)))
      cwd[0] = '\0';

   /* Show which file would be used */
   char path[ROLE_TEMPLATE_PATH_MAX];
   if (role_template_path(cwd, role, path, sizeof(path)) == 0)
      fprintf(stderr, "# Template: %s\n\n", path);
   else
      fprintf(stderr, "# Template: built-in default\n\n");

   char *built = role_template_build(cwd, role, "{{TASK}}", "{{CONTEXT}}");
   if (!built)
   {
      fprintf(stderr, "roles: no template found for role '%s'\n", role);
      return;
   }
   fputs(built, stdout);
   fputc('\n', stdout);
   free(built);
   return;
}

/* aimee roles edit <role> */
static void cmd_roles_edit(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 2)
   {
      fprintf(stderr, "Usage: aimee roles edit <role>\n");
      return;
   }
   const char *role = argv[1];

   char dir[ROLE_TEMPLATE_PATH_MAX];
   snprintf(dir, sizeof(dir), "%s/role_templates", config_default_dir());

   /* Ensure the directory exists */
   struct stat st;
   if (stat(dir, &st) != 0)
      platform_mkdir_p(dir, 0755);

   char path[ROLE_TEMPLATE_PATH_MAX];
   snprintf(path, sizeof(path), "%s/%s.md", dir, role);

   /* If the file doesn't exist yet, seed it from the built-in default */
   if (stat(path, &st) != 0)
   {
      char *builtin = role_template_build(NULL, role, "{{TASK}}", "{{CONTEXT}}");
      if (builtin)
      {
         FILE *f = fopen(path, "w");
         if (f)
         {
            fputs(builtin, f);
            fputc('\n', f);
            fclose(f);
            fprintf(stderr, "roles: created %s from built-in default\n", path);
         }
         free(builtin);
      }
      else
      {
         /* Unknown role — create empty template */
         FILE *f = fopen(path, "w");
         if (f)
         {
            fprintf(f, "# Role: %s\n\n## Task\n{{TASK}}\n\n## Context\n{{CONTEXT}}\n", role);
            fclose(f);
            fprintf(stderr, "roles: created %s (new role)\n", path);
         }
      }
   }

   const char *editor = getenv("EDITOR");
   if (!editor || !editor[0])
      editor = getenv("VISUAL");
   if (!editor || !editor[0])
      editor = "vi";

   char cmd[ROLE_TEMPLATE_PATH_MAX + 64];
   snprintf(cmd, sizeof(cmd), "%s \"%s\"", editor, path);
   int rc = system(cmd);
   if (rc != 0)
      fprintf(stderr, "roles: editor exited with status %d\n", rc);
   return;
}

/* aimee roles reset <role> */
static void cmd_roles_reset(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 2)
   {
      fprintf(stderr, "Usage: aimee roles reset <role>\n");
      return;
   }
   const char *role = argv[1];

   char path[ROLE_TEMPLATE_PATH_MAX];
   snprintf(path, sizeof(path), "%s/role_templates/%s.md", config_default_dir(), role);

   struct stat st;
   if (stat(path, &st) != 0)
   {
      fprintf(stderr, "roles: no user-level template for '%s' (nothing to reset)\n", role);
      return;
   }

   if (unlink(path) != 0)
   {
      fprintf(stderr, "roles: failed to remove %s\n", path);
      return;
   }
   fprintf(stderr, "Removed %s — role '%s' will now use built-in default.\n", path, role);
   return;
}

/* aimee roles install */
static void cmd_roles_install(app_ctx_t *ctx, int argc, char **argv)
{
   char dir[ROLE_TEMPLATE_PATH_MAX];
   snprintf(dir, sizeof(dir), "%s/role_templates", config_default_dir());

   int n = role_template_install_defaults(dir);
   if (n < 0)
   {
      fprintf(stderr, "roles: failed to install defaults to %s\n", dir);
      return;
   }
   if (n == 0)
      fprintf(stderr, "roles: all defaults already installed in %s\n", dir);
   else
      fprintf(stderr, "roles: installed %d default template(s) to %s\n", n, dir);
   return;
}

static const subcmd_t cmd_roles_subs[] = {
    {"list", "list available role templates", cmd_roles_list},
    {"show", "print the effective template for a role", cmd_roles_show},
    {"edit", "open the user-level template in $EDITOR", cmd_roles_edit},
    {"reset", "remove a user-level override (restore built-in default)", cmd_roles_reset},
    {"install", "install built-in defaults", cmd_roles_install},
    {NULL, NULL, NULL},
};

void cmd_roles(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
   {
      roles_print_help();
      return;
   }

   const char *sub = argv[0];

   if (subcmd_dispatch(cmd_roles_subs, sub, ctx, argc, argv) != 0)
   {
      fprintf(stderr, "roles: unknown subcommand '%s'\n\n", sub);
      roles_print_help();
   }
}
