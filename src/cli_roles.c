/* cli_roles.c: `aimee roles` — list, show, edit, and remove delegate role
 * templates from the thin client, over aimee-server's /v1 HTTP API.
 *
 * Role templates are the delegate analog of personas: the body
 * `aimee delegate <role>` uses. They are server-owned config; the client edits
 * them over /v1 (cli_v1_path_request, so a remote thin client reaches its own
 * server rather than a local socket) rather than touching files directly. `edit`
 * round-trips the raw markdown body through $EDITOR. */
#include "http_uds_client.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void roles_usage(void)
{
   fprintf(stderr,
           "Usage: aimee roles <subcommand> [args]\n\n"
           "Subcommands:\n"
           "  list             List delegate role templates\n"
           "  show <role>      Print a role template's body\n"
           "  edit <role>      Edit the template in $EDITOR (creates it if new)\n"
           "  rm <role>        Remove the user-level file (reset a built-in, delete a custom)\n\n"
           "Templates drive `aimee delegate <role> --persona <name> \"prompt\"`.\n");
}

static int roles_server_down(int status)
{
   if (status == 0)
   {
      fprintf(stderr, "aimee: aimee-server is not reachable; is it running?\n");
      return 1;
   }
   return 0;
}

static int roles_list_cmd(int json_output)
{
   int st = 0;
   char *resp = cli_v1_path_request("GET", "/v1/role_templates", NULL, &st);
   if (roles_server_down(st))
   {
      free(resp);
      return 1;
   }
   if (json_output)
   {
      if (resp)
         puts(resp);
      free(resp);
      return st == 200 ? 0 : 1;
   }
   cJSON *root = resp ? cJSON_Parse(resp) : NULL;
   free(resp);
   cJSON *arr = root ? cJSON_GetObjectItemCaseSensitive(root, "role_templates") : NULL;
   if (!cJSON_IsArray(arr))
   {
      fprintf(stderr, "aimee: unexpected response listing role templates\n");
      cJSON_Delete(root);
      return 1;
   }
   cJSON *r = NULL;
   cJSON_ArrayForEach(r, arr) if (cJSON_IsString(r)) fprintf(stdout, "%s\n", r->valuestring);
   cJSON_Delete(root);
   return 0;
}

/* GET the raw body for a role into *out (heap; caller frees). Returns the HTTP
 * status; *out is NULL on a non-200.
 *
 * `permissions_out` may be NULL when the caller only wants the text. When it is
 * not, it receives the DETACHED permissions object (caller frees) rather than a
 * pointer into a tree that is about to be deleted. */
static int roles_fetch_body(const char *role, char **out, cJSON **permissions_out)
{
   *out = NULL;
   if (permissions_out)
      *permissions_out = NULL;
   char path[256];
   snprintf(path, sizeof(path), "/v1/role_templates/%s", role);
   int st = 0;
   char *resp = cli_v1_path_request("GET", path, NULL, &st);
   if (st == 200 && resp)
   {
      cJSON *o = cJSON_Parse(resp);
      cJSON *c = o ? cJSON_GetObjectItemCaseSensitive(o, "content") : NULL;
      if (cJSON_IsString(c))
         *out = strdup(c->valuestring);
      if (permissions_out && o)
         *permissions_out = cJSON_DetachItemFromObjectCaseSensitive(o, "permissions");
      cJSON_Delete(o);
   }
   free(resp);
   return st;
}

/* Print what the role came to, under the template that declares it.
 *
 * The frontmatter says what an operator wrote; this says what it resolved to,
 * which is the part they cannot see. Two lines earn their place here: a
 * permission nothing enforces grants and denies nothing at runtime, and a tool
 * the set withholds will be refused however the toolset was chosen. Both used to
 * reach a log line and stop. */
static void roles_print_permissions(cJSON *perms)
{
   if (!cJSON_IsObject(perms))
      return;

   if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(perms, "resolved")))
   {
      printf("\nPermissions: could not be resolved, so this role holds none and a "
             "delegate for it is refused.\n");
      return;
   }

   cJSON *held = cJSON_GetObjectItemCaseSensitive(perms, "held");
   printf("\nPermissions:\n");
   if (cJSON_GetArraySize(held) == 0)
      printf("  (none: this role may read, and change nothing)\n");
   cJSON *g = NULL;
   cJSON_ArrayForEach(g, held)
   {
      const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(g, "name"));
      const char *at = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(g, "enforced_at"));
      printf("  %-16s enforced at %s", name ? name : "?", (at && at[0]) ? at : "nothing");
      cJSON *scopes = cJSON_GetObjectItemCaseSensitive(g, "scopes");
      if (cJSON_GetArraySize(scopes) > 0)
      {
         printf(", scoped to");
         cJSON *sc = NULL;
         cJSON_ArrayForEach(sc, scopes)
             printf(" %s", cJSON_GetStringValue(sc) ? cJSON_GetStringValue(sc) : "?");
      }
      printf("\n");
   }

   cJSON *unenforced = cJSON_GetObjectItemCaseSensitive(perms, "unenforced");
   if (cJSON_GetArraySize(unenforced) > 0)
   {
      printf("\nNothing enforces:");
      cJSON *u = NULL;
      cJSON_ArrayForEach(u, unenforced)
          printf(" %s", cJSON_GetStringValue(u) ? cJSON_GetStringValue(u) : "?");
      printf("\n  These are carried and evaluated by no one, so they grant nothing and\n"
             "  deny nothing. Bind a point that consults them, or drop them.\n");
   }

   cJSON *denied = cJSON_GetObjectItemCaseSensitive(perms, "denied_tools");
   if (cJSON_GetArraySize(denied) > 0)
   {
      printf("\nTools withheld:");
      cJSON *d = NULL;
      cJSON_ArrayForEach(d, denied)
          printf(" %s", cJSON_GetStringValue(d) ? cJSON_GetStringValue(d) : "?");
      printf("\n  Refused whatever toolset this role runs with.\n");
   }
}

static int roles_show_cmd(const char *role, int json_output)
{
   if (json_output)
   {
      char path[256];
      snprintf(path, sizeof(path), "/v1/role_templates/%s", role);
      int st = 0;
      char *resp = cli_v1_path_request("GET", path, NULL, &st);
      if (roles_server_down(st))
      {
         free(resp);
         return 1;
      }
      if (resp)
         puts(resp);
      free(resp);
      return st == 200 ? 0 : 1;
   }
   char *body = NULL;
   cJSON *permissions = NULL;
   int st = roles_fetch_body(role, &body, &permissions);
   if (roles_server_down(st))
      return 1;
   if (st == 404 || !body)
   {
      fprintf(stderr, "aimee: no such role template '%s'\n", role);
      cJSON_Delete(permissions);
      free(body);
      return 1;
   }
   fputs(body, stdout);
   if (body[0] && body[strlen(body) - 1] != '\n')
      fputc('\n', stdout);
   roles_print_permissions(permissions);
   cJSON_Delete(permissions);
   free(body);
   return 0;
}

static int roles_edit_cmd(const char *role)
{
   char *body = NULL;
   int st = roles_fetch_body(role, &body, NULL);
   if (roles_server_down(st))
      return 1;
   if (!body)
      body = strdup("# Role: \n\n## Task\n{{TASK}}\n\n## Context\n{{CONTEXT}}\n");

   char tmp[256];
   snprintf(tmp, sizeof(tmp), "/tmp/aimee-role-%s-%d.md", role, (int)getpid());
   FILE *f = fopen(tmp, "w");
   if (!f)
   {
      free(body);
      fprintf(stderr, "aimee: cannot create temp file %s\n", tmp);
      return 1;
   }
   fputs(body, f);
   fclose(f);
   free(body);

   const char *editor = getenv("EDITOR");
   if (!editor || !editor[0])
      editor = getenv("VISUAL");
   if (!editor || !editor[0])
      editor = "vi";
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "%s \"%s\"", editor, tmp);
   if (system(cmd) != 0)
      fprintf(stderr, "aimee: editor exited non-zero; continuing with the saved file\n");

   f = fopen(tmp, "r");
   if (!f)
   {
      fprintf(stderr, "aimee: cannot reopen %s\n", tmp);
      return 1;
   }
   fseek(f, 0, SEEK_END);
   long len = ftell(f);
   rewind(f);
   char *buf = (len >= 0 && len < (1 << 20)) ? malloc((size_t)len + 1) : NULL;
   if (!buf)
   {
      fclose(f);
      unlink(tmp);
      fprintf(stderr, "aimee: edited file too large\n");
      return 1;
   }
   size_t got = fread(buf, 1, (size_t)len, f);
   buf[got] = '\0';
   fclose(f);
   unlink(tmp);

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "content", buf);
   free(buf);
   char *reqbody = cJSON_PrintUnformatted(req);
   cJSON_Delete(req);

   char path[256];
   snprintf(path, sizeof(path), "/v1/role_templates/%s", role);
   int wst = 0;
   char *resp = cli_v1_path_request("PUT", path, reqbody, &wst);
   free(reqbody);
   if (wst != 200)
   {
      fprintf(stderr, "aimee: failed to save role template '%s' (server status %d)\n", role, wst);
      if (resp)
         fprintf(stderr, "  %s\n", resp);
      free(resp);
      return 1;
   }
   free(resp);
   fprintf(stderr, "Saved role template '%s'.\n", role);
   return 0;
}

static int roles_rm_cmd(const char *role)
{
   char path[256];
   snprintf(path, sizeof(path), "/v1/role_templates/%s", role);
   int st = 0;
   char *resp = cli_v1_path_request("DELETE", path, NULL, &st);
   free(resp);
   if (roles_server_down(st))
      return 1;
   if (st == 404)
   {
      fprintf(stderr, "aimee: no user-level file for '%s' (nothing to remove)\n", role);
      return 1;
   }
   if (st != 200)
   {
      fprintf(stderr, "aimee: failed to remove role template '%s' (server status %d)\n", role, st);
      return 1;
   }
   fprintf(stderr, "Removed user-level role template for '%s'.\n", role);
   return 0;
}

int cmd_roles_client_run(int argc, char **argv, int json_output)
{
   if (argc < 1)
   {
      roles_usage();
      return 1;
   }
   const char *sub = argv[0];
   if (strcmp(sub, "list") == 0)
      return roles_list_cmd(json_output);
   if (strcmp(sub, "show") == 0)
   {
      if (argc < 2)
      {
         fprintf(stderr, "Usage: aimee roles show <role>\n");
         return 1;
      }
      return roles_show_cmd(argv[1], json_output);
   }
   if (strcmp(sub, "edit") == 0 || strcmp(sub, "add") == 0)
   {
      if (argc < 2)
      {
         fprintf(stderr, "Usage: aimee roles %s <role>\n", sub);
         return 1;
      }
      return roles_edit_cmd(argv[1]);
   }
   if (strcmp(sub, "rm") == 0 || strcmp(sub, "remove") == 0 || strcmp(sub, "reset") == 0)
   {
      if (argc < 2)
      {
         fprintf(stderr, "Usage: aimee roles %s <role>\n", sub);
         return 1;
      }
      return roles_rm_cmd(argv[1]);
   }
   roles_usage();
   return 1;
}
