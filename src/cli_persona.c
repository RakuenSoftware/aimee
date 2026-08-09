/* cli_persona.c: `aimee persona` — list, show, edit, add, and remove personas
 * from the thin client.
 *
 * Personas are server-owned config; per the persona registry contract the client
 * never reads the on-disk files directly, it goes over aimee-server's /v1 HTTP
 * API (cli_v1_path_request, so a remote thin client reaches its own server
 * rather than a local socket). `edit` round-trips the persona as JSON through $EDITOR
 * so no markdown parser is needed client-side. */
#include "http_uds_client.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void persona_usage(void)
{
   fprintf(stderr,
           "Usage: aimee persona <subcommand> [args]\n\n"
           "Subcommands:\n"
           "  list              List available personas\n"
           "  show <name>       Print a persona definition\n"
           "  edit <name>       Edit a persona as JSON in $EDITOR (creates it if new)\n"
           "  add <name>        Alias for edit (starts from a template if new)\n"
           "  rm <name>         Remove the user-level file (reset a built-in, delete a custom)\n\n"
           "Personas drive both the primary agent and any `aimee delegate --persona <name>`.\n");
}

static int persona_server_down(int status)
{
   if (status == 0)
   {
      fprintf(stderr, "aimee: aimee-server is not reachable; is it running?\n");
      return 1;
   }
   return 0;
}

/* GET a path and return the parsed JSON (caller cJSON_Delete), or NULL. Sets
 * *status to the HTTP status (0 = transport failure). */
static cJSON *persona_get_json(const char *path, int *status)
{
   char *resp = cli_v1_path_request("GET", path, NULL, status);
   cJSON *root = resp ? cJSON_Parse(resp) : NULL;
   free(resp);
   return root;
}

static int persona_list_cmd(int json_output)
{
   int st = 0;
   char *resp = cli_v1_path_request("GET", "/v1/personas", NULL, &st);
   if (persona_server_down(st))
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
   cJSON *arr = root ? cJSON_GetObjectItemCaseSensitive(root, "personas") : NULL;
   if (!cJSON_IsArray(arr))
   {
      fprintf(stderr, "aimee: unexpected response listing personas\n");
      cJSON_Delete(root);
      return 1;
   }
   cJSON *p = NULL;
   cJSON_ArrayForEach(p, arr)
   {
      cJSON *name = cJSON_GetObjectItemCaseSensitive(p, "name");
      cJSON *desc = cJSON_GetObjectItemCaseSensitive(p, "description");
      cJSON *builtin = cJSON_GetObjectItemCaseSensitive(p, "builtin");
      fprintf(stdout, "%-18s %-9s %s\n", cJSON_IsString(name) ? name->valuestring : "?",
              cJSON_IsTrue(builtin) ? "built-in" : "custom",
              cJSON_IsString(desc) ? desc->valuestring : "");
   }
   cJSON_Delete(root);
   return 0;
}

static int persona_show_cmd(const char *name, int json_output)
{
   char path[256];
   snprintf(path, sizeof(path), "/v1/personas/%s", name);
   int st = 0;
   char *resp = cli_v1_path_request("GET", path, NULL, &st);
   if (persona_server_down(st))
   {
      free(resp);
      return 1;
   }
   if (st == 404)
   {
      fprintf(stderr, "aimee: no such persona '%s'\n", name);
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
   cJSON *p = resp ? cJSON_Parse(resp) : NULL;
   free(resp);
   if (!p)
   {
      fprintf(stderr, "aimee: unexpected response for persona '%s'\n", name);
      return 1;
   }
#define PFLD(k)                                                                                    \
   do                                                                                              \
   {                                                                                               \
      cJSON *v = cJSON_GetObjectItemCaseSensitive(p, k);                                           \
      if (cJSON_IsString(v) && v->valuestring[0])                                                  \
         fprintf(stdout, "%s: %s\n", k, v->valuestring);                                           \
   } while (0)
   PFLD("name");
   PFLD("description");
   PFLD("delegates");
   cJSON *roles = cJSON_GetObjectItemCaseSensitive(p, "roles");
   if (cJSON_IsArray(roles) && cJSON_GetArraySize(roles) > 0)
   {
      fprintf(stdout, "roles:");
      cJSON *r = NULL;
      cJSON_ArrayForEach(r, roles) if (cJSON_IsString(r)) fprintf(stdout, " %s", r->valuestring);
      fprintf(stdout, "\n");
   }
   PFLD("check_role");
   cJSON *pt = cJSON_GetObjectItemCaseSensitive(p, "persona");
   if (cJSON_IsString(pt) && pt->valuestring[0])
      fprintf(stdout, "\n## Persona\n%s\n", pt->valuestring);
   cJSON *pr = cJSON_GetObjectItemCaseSensitive(p, "principles");
   if (cJSON_IsString(pr) && pr->valuestring[0])
      fprintf(stdout, "\n## Principles\n%s\n", pr->valuestring);
   cJSON *br = cJSON_GetObjectItemCaseSensitive(p, "brief");
   if (cJSON_IsString(br) && br->valuestring[0])
      fprintf(stdout, "\n## Brief\n%s\n", br->valuestring);
#undef PFLD
   cJSON_Delete(p);
   return 0;
}

/* Build the JSON object opened in the editor: the current persona (GET) when it
 * exists, otherwise a template carrying the requested name. */
static cJSON *persona_edit_seed(const char *name)
{
   char path[256];
   snprintf(path, sizeof(path), "/v1/personas/%s", name);
   int st = 0;
   cJSON *cur = persona_get_json(path, &st);
   if (st == 200 && cur)
   {
      /* Drop the read-only 'builtin' flag so the edited body is a clean write. */
      cJSON_DeleteItemFromObjectCaseSensitive(cur, "builtin");
      return cur;
   }
   cJSON_Delete(cur);
   cJSON *t = cJSON_CreateObject();
   cJSON_AddStringToObject(t, "name", name);
   cJSON_AddStringToObject(t, "description", "");
   cJSON_AddStringToObject(t, "delegates", "full");
   cJSON_AddItemToObject(t, "roles", cJSON_CreateArray());
   cJSON_AddStringToObject(t, "persona",
                           "You are ... (identity; %s is replaced with the working directory).");
   cJSON_AddStringToObject(t, "principles", "# Principles\n- ...\n");
   cJSON_AddStringToObject(t, "brief", "");
   return t;
}

static int persona_edit_cmd(const char *name)
{
   /* Probe the server first so we fail fast (and clearly) when it is down. */
   int st = 0;
   cJSON *probe = persona_get_json("/v1/personas", &st);
   cJSON_Delete(probe);
   if (persona_server_down(st))
      return 1;

   cJSON *seed = persona_edit_seed(name);
   char *pretty = cJSON_Print(seed);
   cJSON_Delete(seed);
   if (!pretty)
      return 1;

   char tmp[256];
   snprintf(tmp, sizeof(tmp), "/tmp/aimee-persona-%s-%d.json", name, (int)getpid());
   FILE *f = fopen(tmp, "w");
   if (!f)
   {
      free(pretty);
      fprintf(stderr, "aimee: cannot create temp file %s\n", tmp);
      return 1;
   }
   fputs(pretty, f);
   fputc('\n', f);
   fclose(f);
   free(pretty);

   const char *editor = getenv("EDITOR");
   if (!editor || !editor[0])
      editor = getenv("VISUAL");
   if (!editor || !editor[0])
      editor = "vi";
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "%s \"%s\"", editor, tmp);
   if (system(cmd) != 0)
      fprintf(stderr, "aimee: editor exited non-zero; continuing with the saved file\n");

   /* Read the edited file back. */
   f = fopen(tmp, "r");
   if (!f)
   {
      fprintf(stderr, "aimee: cannot reopen %s\n", tmp);
      return 1;
   }
   fseek(f, 0, SEEK_END);
   long len = ftell(f);
   rewind(f);
   char *buf = (len > 0 && len < (1 << 20)) ? malloc((size_t)len + 1) : NULL;
   if (!buf)
   {
      fclose(f);
      unlink(tmp);
      fprintf(stderr, "aimee: edited file is empty or too large\n");
      return 1;
   }
   size_t got = fread(buf, 1, (size_t)len, f);
   buf[got] = '\0';
   fclose(f);
   unlink(tmp);

   cJSON *edited = cJSON_Parse(buf);
   if (!edited)
   {
      fprintf(stderr, "aimee: edited file is not valid JSON; aborting (nothing written)\n");
      free(buf);
      return 1;
   }
   cJSON_Delete(edited);

   char path[256];
   snprintf(path, sizeof(path), "/v1/personas/%s", name);
   int wst = 0;
   char *resp = cli_v1_path_request("PUT", path, buf, &wst);
   free(buf);
   if (wst != 200)
   {
      fprintf(stderr, "aimee: failed to save persona '%s' (server status %d)\n", name, wst);
      if (resp)
         fprintf(stderr, "  %s\n", resp);
      free(resp);
      return 1;
   }
   free(resp);
   fprintf(stderr, "Saved persona '%s'.\n", name);
   return 0;
}

static int persona_rm_cmd(const char *name)
{
   char path[256];
   snprintf(path, sizeof(path), "/v1/personas/%s", name);
   int st = 0;
   char *resp = cli_v1_path_request("DELETE", path, NULL, &st);
   free(resp);
   if (persona_server_down(st))
      return 1;
   if (st == 404)
   {
      fprintf(stderr, "aimee: no user-level file for '%s' (nothing to remove)\n", name);
      return 1;
   }
   if (st != 200)
   {
      fprintf(stderr, "aimee: failed to remove persona '%s' (server status %d)\n", name, st);
      return 1;
   }
   fprintf(stderr, "Removed user-level persona file for '%s'.\n", name);
   return 0;
}

int cmd_persona_client_run(int argc, char **argv, int json_output)
{
   if (argc < 1)
   {
      persona_usage();
      return 1;
   }
   const char *sub = argv[0];
   if (strcmp(sub, "list") == 0)
      return persona_list_cmd(json_output);
   if (strcmp(sub, "show") == 0)
   {
      if (argc < 2)
      {
         fprintf(stderr, "Usage: aimee persona show <name>\n");
         return 1;
      }
      return persona_show_cmd(argv[1], json_output);
   }
   if (strcmp(sub, "edit") == 0 || strcmp(sub, "add") == 0)
   {
      if (argc < 2)
      {
         fprintf(stderr, "Usage: aimee persona %s <name>\n", sub);
         return 1;
      }
      return persona_edit_cmd(argv[1]);
   }
   if (strcmp(sub, "rm") == 0 || strcmp(sub, "remove") == 0 || strcmp(sub, "reset") == 0)
   {
      if (argc < 2)
      {
         fprintf(stderr, "Usage: aimee persona %s <name>\n", sub);
         return 1;
      }
      return persona_rm_cmd(argv[1]);
   }
   persona_usage();
   return 1;
}
