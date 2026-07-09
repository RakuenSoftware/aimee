/* cmd_workflow.c -- `aimee workflow` client command (local, no server):
 * inspect the block catalog, validate/show workflow definitions, list and
 * scaffold workflows. The engine that runs workflows lands in later slices. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aimee_home.h"
#include "aimee_client.h"
#include "cli_client.h"
#include "cJSON.h"
#include "wfe_def.h"

#ifndef _WIN32
#include <dirent.h>
#include <unistd.h>
#endif

static void print_accepts_builtin(wfe_block_type_t t)
{
   int any = 0;
   for (wfe_artifact_type_t a = WFE_ART_PROPOSAL; a < WFE_ART__COUNT; a++)
      if (wfe_block_accepts_input(t, a))
      {
         printf(" %s", wfe_artifact_name(a));
         any = 1;
      }
   if (!any)
      printf(" (none)");
}

static void print_blocks(void)
{
   /* enumerate the live catalog: every built-in (incl. gate.ci / check.mergeable)
    * plus the config-defined custom registry, so this never drifts from the
    * blocks the validator + web composer actually know. */
   char err[256] = "";
   wfe_custom_registry_ensure(err, sizeof err);
   printf("Workflow block catalog:\n");
   for (wfe_block_type_t t = WFE_BLK_UNKNOWN + 1; t < WFE_BLK_CUSTOM; t++)
   {
      printf("  %-18s produces %-12s accepts:", wfe_block_name(t),
             wfe_artifact_name(wfe_block_output(t)));
      print_accepts_builtin(t);
      printf("\n");
   }
   int n = wfe_custom_count();
   if (n > 0)
      printf("Custom blocks ($AIMEE_HOME/workflows/blocks.yaml):\n");
   for (int i = 0; i < n; i++)
   {
      const wfe_custom_block_t *c = wfe_custom_at(i);
      if (!c)
         continue;
      printf("  %-18s produces %-12s accepts:", c->name, wfe_artifact_name(c->produces));
      if (c->consumes != WFE_ART_NONE)
         printf(" %s", wfe_artifact_name(c->consumes));
      else
         printf(" (none)");
      printf("  [%s]\n", c->executor == WFE_EXEC_COMMAND ? "command" : "delegate");
   }
}

static int load_or_report(const char *path, wfe_def_t **out)
{
   char err[256];
   wfe_def_t *def = wfe_def_load_file(path, err, sizeof err);
   if (!def)
   {
      fprintf(stderr, "workflow: %s\n", err);
      return 1;
   }
   *out = def;
   return 0;
}

static int cmd_validate(const char *path, int json_output)
{
   wfe_def_t *def = NULL;
   if (load_or_report(path, &def) != 0)
      return 1;
   char err[256];
   int rc = wfe_def_validate(def, err, sizeof err);
   char ver[65] = "";
   if (rc == 0)
      wfe_def_compute_version(def, ver);
   if (json_output)
   {
      printf("{\"valid\":%s,\"name\":\"%s\",\"version\":\"%s\"", rc == 0 ? "true" : "false",
             def->name, ver);
      if (rc != 0)
         printf(",\"error\":\"%s\"", err);
      printf("}\n");
   }
   else if (rc == 0)
   {
      printf("ok: '%s' valid (%d nodes), version %s\n", def->name, def->n_nodes, ver);
   }
   else
   {
      fprintf(stderr, "invalid: %s\n", err);
   }
   wfe_def_free(def);
   return rc == 0 ? 0 : 1;
}

static int cmd_show(const char *path)
{
   wfe_def_t *def = NULL;
   if (load_or_report(path, &def) != 0)
      return 1;
   char ver[65] = "";
   wfe_def_compute_version(def, ver);
   char *canon = wfe_def_canonical(def);
   printf("# name: %s\n# version: %s\n%s", def->name, ver, canon ? canon : "");
   free(canon);
   wfe_def_free(def);
   return 0;
}

static int cmd_list(void)
{
#ifndef _WIN32
   char dir[1024];
   snprintf(dir, sizeof dir, "%s/workflows", aimee_home());
   DIR *d = opendir(dir);
   if (!d)
   {
      printf("(no workflows in %s)\n", dir);
      return 0;
   }
   struct dirent *e;
   int n = 0;
   while ((e = readdir(d)))
   {
      const char *dot = strrchr(e->d_name, '.');
      if (dot && strcmp(dot, ".yaml") == 0)
      {
         char path[2048];
         snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
         char err[256];
         wfe_def_t *def = wfe_def_load_file(path, err, sizeof err);
         char ver[65] = "";
         int valid = def && wfe_def_validate(def, err, sizeof err) == 0;
         if (valid)
            wfe_def_compute_version(def, ver);
         printf("  %-24s %s %s\n", e->d_name, valid ? "valid" : "INVALID", ver);
         if (def)
            wfe_def_free(def);
         n++;
      }
   }
   closedir(d);
   if (n == 0)
      printf("(no .yaml workflows in %s)\n", dir);
   return 0;
#else
   printf("workflow list: not supported on this platform\n");
   return 0;
#endif
}

static const char *TEMPLATE = "name: %s\n"
                              "start: draft\n"
                              "nodes:\n"
                              "  - id: draft\n"
                              "    block: author.proposal\n"
                              "    params:\n"
                              "      with_user: true\n"
                              "    next: review\n"
                              "  - id: review\n"
                              "    block: gate.roundtable\n"
                              "    in:\n"
                              "      src: draft.out\n"
                              "    params:\n"
                              "      panel:\n"
                              "        required:\n"
                              "          - security\n"
                              "          - architect\n"
                              "      quorum: 2\n"
                              "      max_rounds: 6\n"
                              "      max_iters: 3\n"
                              "      on_max: fail\n"
                              "    on_pass: pr\n"
                              "    on_fail: draft\n"
                              "  - id: pr\n"
                              "    block: pr.open\n"
                              "    in:\n"
                              "      src: draft.out\n"
                              "    next: done\n"
                              "  - id: done\n"
                              "    block: merge\n"
                              "    in:\n"
                              "      pr: pr.out\n";

static int cmd_new(const char *path)
{
   FILE *f = fopen(path, "wx");
   if (!f)
   {
      fprintf(stderr, "workflow: cannot create '%s' (exists?)\n", path);
      return 1;
   }
   const char *base = strrchr(path, '/');
   char name[64];
   snprintf(name, sizeof name, "%s", base ? base + 1 : path);
   char *dot = strrchr(name, '.');
   if (dot)
      *dot = '\0';
   fprintf(f, TEMPLATE, name);
   fclose(f);
   printf("created %s (edit, then `aimee workflow validate %s`)\n", path, path);
   return 0;
}

/* --- run / status: talk to aimee-server (unlike the local-only subcommands) --- */

static const char *jstr(const cJSON *o, const char *k)
{
   cJSON *x = o ? cJSON_GetObjectItemCaseSensitive((cJSON *)o, k) : NULL;
   return cJSON_IsString(x) ? x->valuestring : "";
}

/* Read a whole file (or stdin when path is "-" or NULL) into a heap buffer. */
static char *slurp_file_or_stdin(const char *path)
{
   FILE *f = (!path || strcmp(path, "-") == 0) ? stdin : fopen(path, "rb");
   if (!f)
      return NULL;
   size_t cap = 65536, len = 0;
   char *buf = malloc(cap);
   if (!buf)
   {
      if (f != stdin)
         fclose(f);
      return NULL;
   }
   size_t n;
   while ((n = fread(buf + len, 1, cap - len - 1, f)) > 0)
   {
      len += n;
      if (len + 1 >= cap)
      {
         cap *= 2;
         char *nb = realloc(buf, cap);
         if (!nb)
         {
            free(buf);
            if (f != stdin)
               fclose(f);
            return NULL;
         }
         buf = nb;
      }
   }
   buf[len] = '\0';
   if (f != stdin)
      fclose(f);
   return buf;
}

/* GET /v1/workflow/items/<id>; *out is the parsed body (caller frees), *status the
 * HTTP status. Returns -1 on transport failure. */
static int wf_item_get(const char *id, cJSON **out, int *status)
{
   char path[256];
   snprintf(path, sizeof path, "/v1/workflow/items/%s", id);
   char *resp = aimee_client_request("GET", path, NULL, status);
   if (!resp)
      return -1;
   *out = cJSON_Parse(resp);
   free(resp);
   return 0;
}

/* Poll the run until it leaves the "active" state (terminal or parked at a gate),
 * printing each state/stage transition. */
static int wf_watch(const char *id)
{
   char last[128] = "";
   for (;;)
   {
      cJSON *it = NULL;
      int st = 0;
      if (wf_item_get(id, &it, &st) != 0)
      {
         fprintf(stderr, "workflow status: server unavailable\n");
         return 1;
      }
      if (st != 200)
      {
         const char *m = jstr(it, "error");
         fprintf(stderr, "workflow status: %s (status %d)\n", m[0] ? m : "error", st);
         cJSON_Delete(it);
         return 1;
      }
      const char *state = jstr(it, "state");
      const char *stage = jstr(it, "stage");
      char cur[128];
      snprintf(cur, sizeof cur, "%s|%s", state, stage);
      if (strcmp(cur, last) != 0)
      {
         printf("  %-12s stage=%s\n", state, stage);
         snprintf(last, sizeof last, "%s", cur);
      }
      int active = strcmp(state, "active") == 0;
      cJSON_Delete(it);
      if (!active)
         return 0;
#ifndef _WIN32
      sleep(2);
#endif
   }
}

static int cmd_run(int argc, char **argv, int json_output)
{
   const char *workflow = NULL, *proposal_file = NULL, *message = NULL, *repo = "";
   int watch = 0, want_stdin = 0;
   for (int i = 0; i < argc; i++)
   {
      const char *a = argv[i];
      if (strcmp(a, "--proposal") == 0 && i + 1 < argc)
         proposal_file = argv[++i];
      else if (strncmp(a, "--proposal=", 11) == 0)
         proposal_file = a + 11;
      else if ((strcmp(a, "--message") == 0 || strcmp(a, "-m") == 0) && i + 1 < argc)
         message = argv[++i];
      else if (strncmp(a, "--message=", 10) == 0)
         message = a + 10;
      else if (strcmp(a, "--repo") == 0 && i + 1 < argc)
         repo = argv[++i];
      else if (strncmp(a, "--repo=", 7) == 0)
         repo = a + 7;
      else if (strcmp(a, "--watch") == 0)
         watch = 1;
      else if (strcmp(a, "-") == 0)
         want_stdin = 1;
      else if (a[0] != '-' && !workflow)
         workflow = a;
   }
   if (!workflow)
   {
      fprintf(stderr, "usage: aimee workflow run <name> [--proposal <file>|- | --message <text>]"
                      " [--repo <path>] [--watch]\n");
      return 2;
   }

   char *owned = NULL;
   const char *proposal = NULL;
   if (message)
      proposal = message;
   else if (proposal_file)
      proposal = owned = slurp_file_or_stdin(proposal_file);
   else if (want_stdin)
      proposal = owned = slurp_file_or_stdin("-");
   if (!proposal || !proposal[0])
   {
      fprintf(stderr, "workflow run: a proposal is required "
                      "(--proposal <file>, --message <text>, or '-' for stdin)\n");
      free(owned);
      return 2;
   }

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "workflow", workflow);
   cJSON_AddStringToObject(req, "proposal_md", proposal);
   if (repo && repo[0])
      cJSON_AddStringToObject(req, "repo", repo);
   char *body = cJSON_PrintUnformatted(req);
   cJSON_Delete(req);
   free(owned);

   int st = 0;
   char *resp = aimee_client_request("POST", "/v1/dev/submit", body, &st);
   free(body);
   if (!resp)
   {
      fprintf(stderr, "workflow run: server unavailable (is aimee-server running?)\n");
      return 1;
   }
   cJSON *r = cJSON_Parse(resp);
   free(resp);
   if (st != 200)
   {
      const char *m = jstr(r, "error");
      fprintf(stderr, "workflow run: %s (status %d)\n", m[0] ? m : "failed", st);
      cJSON_Delete(r);
      return 1;
   }
   char id_copy[128];
   snprintf(id_copy, sizeof id_copy, "%s", jstr(r, "work_item_id"));
   if (json_output)
   {
      char *s = cJSON_PrintUnformatted(r);
      printf("%s\n", s ? s : "{}");
      free(s);
   }
   else
   {
      printf("work_item_id: %s\n", id_copy);
      printf("workflow:     %s\n", jstr(r, "workflow"));
      printf("state:        %s\n", jstr(r, "state"));
      if (!watch)
         fprintf(stderr, "watch it with: aimee workflow status %s --watch\n", id_copy);
   }
   cJSON_Delete(r);
   if (watch && id_copy[0])
      return wf_watch(id_copy);
   return 0;
}

static void wf_print_item(const cJSON *it)
{
   printf("  id:       %s\n", jstr(it, "id"));
   printf("  workflow: %s (%s)\n", jstr(it, "workflow"), jstr(it, "version"));
   printf("  state:    %s\n", jstr(it, "state"));
   printf("  stage:    %s\n", jstr(it, "stage"));
   if (jstr(it, "pause_reason")[0])
      printf("  paused:   %s\n", jstr(it, "pause_reason"));
   if (jstr(it, "repo")[0])
      printf("  repo:     %s\n", jstr(it, "repo"));
   if (jstr(it, "pr_ref")[0])
      printf("  pr:       %s\n", jstr(it, "pr_ref"));
   cJSON *cost = cJSON_GetObjectItemCaseSensitive((cJSON *)it, "cum_cost_usd");
   if (cJSON_IsNumber(cost))
      printf("  cost:     $%.2f\n", cost->valuedouble);
}

static int cmd_status(int argc, char **argv, int json_output)
{
   const char *id = NULL;
   int watch = 0, events = 0;
   for (int i = 0; i < argc; i++)
   {
      if (strcmp(argv[i], "--watch") == 0)
         watch = 1;
      else if (strcmp(argv[i], "--events") == 0)
         events = 1;
      else if (argv[i][0] != '-' && !id)
         id = argv[i];
   }
   if (!id)
   {
      fprintf(stderr, "usage: aimee workflow status <id> [--watch] [--events]\n");
      return 2;
   }
   if (watch)
      return wf_watch(id);

   cJSON *it = NULL;
   int st = 0;
   if (wf_item_get(id, &it, &st) != 0)
   {
      fprintf(stderr, "workflow status: server unavailable (is aimee-server running?)\n");
      return 1;
   }
   if (st != 200)
   {
      const char *m = jstr(it, "error");
      fprintf(stderr, "workflow status: %s (status %d)\n", m[0] ? m : "not found", st);
      cJSON_Delete(it);
      return 1;
   }
   if (json_output)
   {
      char *s = cJSON_PrintUnformatted(it);
      printf("%s\n", s ? s : "{}");
      free(s);
   }
   else
   {
      wf_print_item(it);
   }
   cJSON_Delete(it);

   if (events && !json_output)
   {
      char path[256];
      snprintf(path, sizeof path, "/v1/workflow/items/%s/events", id);
      int est = 0;
      char *resp = aimee_client_request("GET", path, NULL, &est);
      if (resp && est == 200)
      {
         cJSON *eo = cJSON_Parse(resp);
         cJSON *arr = eo ? cJSON_GetObjectItemCaseSensitive(eo, "events") : NULL;
         if (cJSON_IsArray(arr))
         {
            printf("\n  events:\n");
            cJSON *e = NULL;
            cJSON_ArrayForEach(e, arr)
            {
               printf("    %-19s %-12s %-10s %s\n", jstr(e, "created_at"), jstr(e, "stage"),
                      jstr(e, "kind"), jstr(e, "detail"));
            }
         }
         cJSON_Delete(eo);
      }
      free(resp);
   }
   return 0;
}

static void usage(void)
{
   fprintf(stderr,
           "Usage: aimee workflow <subcommand>\n"
           "  blocks                 list the composable block catalog\n"
           "  validate <file.yaml>   typed-graph validate a workflow\n"
           "  show <file.yaml>       print the canonical form + version\n"
           "  list                   list workflows under $AIMEE_HOME/workflows\n"
           "  new <file.yaml>        scaffold a starter workflow\n"
           "  run <name> [--proposal <file>|- | --message <text>] [--repo <path>] [--watch]\n"
           "                         start a saved workflow run (needs aimee-server)\n"
           "  status <id> [--watch] [--events]\n"
           "                         show a run's status (needs aimee-server)\n");
}

int cmd_workflow_client_run(int argc, char **argv, int json_output)
{
   if (argc < 1)
   {
      usage();
      return 2;
   }
   const char *sub = argv[0];
   if (strcmp(sub, "blocks") == 0)
   {
      print_blocks();
      return 0;
   }
   if (strcmp(sub, "list") == 0)
      return cmd_list();
   if (strcmp(sub, "validate") == 0)
   {
      if (argc < 2)
      {
         usage();
         return 2;
      }
      return cmd_validate(argv[1], json_output);
   }
   if (strcmp(sub, "show") == 0)
   {
      if (argc < 2)
      {
         usage();
         return 2;
      }
      return cmd_show(argv[1]);
   }
   if (strcmp(sub, "new") == 0)
   {
      if (argc < 2)
      {
         usage();
         return 2;
      }
      return cmd_new(argv[1]);
   }
   if (strcmp(sub, "run") == 0)
      return cmd_run(argc - 1, argv + 1, json_output);
   if (strcmp(sub, "status") == 0)
      return cmd_status(argc - 1, argv + 1, json_output);
   usage();
   return 2;
}
