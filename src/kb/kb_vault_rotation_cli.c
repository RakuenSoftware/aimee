/* kb_vault_rotation_cli.c: see kb_vault_rotation_cli.h. */
#include "kb_vault_rotation_cli.h"

#include "cJSON.h"
#include "modules/kb/c/kb_store_tenant.h"
#include "org_vault_rotation.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define WAIT_SECONDS      900
#define WAIT_POLL_SECONDS 2
#define LIST_MAX          50

static int usage(void)
{
   fputs("Usage: aimee-kb vault rotate <team_id> <model_id> [--wait]\n"
         "       aimee-kb vault remediate <team_id> <rotation_id> [--wait]\n"
         "       aimee-kb vault rotations <team_id>\n",
         stderr);
   return 2;
}

static int parse_id(const char *text, int64_t *out)
{
   char *end = NULL;
   errno = 0;
   unsigned long long v = strtoull(text, &end, 10);
   if (!text[0] || text[0] == '-' || errno || !end || *end || v < 1 || v > INT64_MAX)
      return -1;
   *out = (int64_t)v;
   return 0;
}

int kb_vault_rotation_cli_handles(const char *subcommand)
{
   return subcommand && (!strcmp(subcommand, "rotate") || !strcmp(subcommand, "remediate") ||
                         !strcmp(subcommand, "rotations"));
}

static int scoped_list(const kb_principal_t *owner, const char *actor, int64_t team,
                       kb_store_vault_rotation_request_row_t *rows, int cap)
{
   if (kb_store_tenant_scope_begin(owner, team) != 0)
      return -1;
   int n = kb_store_vault_rotation_request_list(actor, team, rows, cap);
   if (n < 0)
   {
      kb_store_tenant_scope_rollback();
      return -1;
   }
   return kb_store_tenant_scope_commit() == 0 ? n : -1;
}

static cJSON *row_json(const kb_store_vault_rotation_request_row_t *r)
{
   cJSON *o = cJSON_CreateObject();
   if (!o)
      return NULL;
   cJSON_AddNumberToObject(o, "request_id", (double)r->id);
   cJSON_AddStringToObject(o, "kind", r->kind);
   if (r->model_id[0])
      cJSON_AddStringToObject(o, "model_id", r->model_id);
   cJSON_AddStringToObject(o, "request_state", r->state);
   if (r->detail[0])
      cJSON_AddStringToObject(o, "detail", r->detail);
   cJSON_AddStringToObject(o, "requested_at", r->created_at);
   if (r->rotation_id > 0)
   {
      cJSON_AddNumberToObject(o, "rotation_id", (double)r->rotation_id);
      cJSON_AddStringToObject(o, "rotation_state", r->rotation_state);
      if (r->failure_phase[0])
         cJSON_AddStringToObject(o, "failure_phase", r->failure_phase);
      if (r->last_error[0])
         cJSON_AddStringToObject(o, "last_error", r->last_error);
   }
   return o;
}

static void print_json(cJSON *o)
{
   char *text = o ? cJSON_PrintUnformatted(o) : NULL;
   if (text)
      printf("%s\n", text);
   free(text);
   cJSON_Delete(o);
}

/* 0 retired, 1 failed or rejected, 2 still running when the wait ran out, -1 unreadable. */
static int outcome(const kb_store_vault_rotation_request_row_t *r, int remediate)
{
   if (!strcmp(r->state, "rejected"))
      return 1;
   if (!strcmp(r->state, "pending"))
      return 2;
   if (!strcmp(r->rotation_state, "retired"))
      return 0;
   if (!strcmp(r->rotation_state, "failed"))
      return remediate ? 2 : 1;
   return 2;
}

static int wait_for(const kb_principal_t *owner, const char *actor, int64_t team,
                    int64_t request_id, int remediate)
{
   static kb_store_vault_rotation_request_row_t rows[LIST_MAX];
   time_t deadline = time(NULL) + WAIT_SECONDS;
   for (;;)
   {
      int n = scoped_list(owner, actor, team, rows, LIST_MAX);
      const kb_store_vault_rotation_request_row_t *mine = NULL;
      for (int i = 0; i < n; i++)
         if (rows[i].id == request_id)
            mine = &rows[i];
      if (!mine)
      {
         fputs("aimee-kb vault: the request could not be read back\n", stderr);
         return 1;
      }
      int rc = outcome(mine, remediate);
      if (rc != 2 || time(NULL) >= deadline)
      {
         print_json(row_json(mine));
         if (rc == 2)
            fputs("aimee-kb vault: still in progress; the daemon keeps driving it. Follow it "
                  "with `aimee-kb vault rotations`\n",
                  stderr);
         return rc;
      }
      struct timespec ts = {.tv_sec = WAIT_POLL_SECONDS, .tv_nsec = 0};
      nanosleep(&ts, NULL);
   }
}

int kb_vault_rotation_cli(int argc, char **argv, const kb_principal_t *owner)
{
   const char *sub = argv[2];
   char actor[576];
   int64_t team = 0;
   if (argc < 4 || parse_id(argv[3], &team) != 0 || kb_identity_key(owner, actor, sizeof(actor)))
      return usage();

   if (!strcmp(sub, "rotations"))
   {
      if (argc != 4)
         return usage();
      static kb_store_vault_rotation_request_row_t rows[LIST_MAX];
      int n = scoped_list(owner, actor, team, rows, LIST_MAX);
      if (n < 0)
      {
         fputs("aimee-kb vault rotations: not authorized or KB_STORE unavailable\n", stderr);
         return 1;
      }
      cJSON *arr = cJSON_CreateArray();
      for (int i = 0; arr && i < n; i++)
         cJSON_AddItemToArray(arr, row_json(&rows[i]));
      print_json(arr);
      return 0;
   }

   int remediate = !strcmp(sub, "remediate");
   int wait = 0;
   if (argc < 5 || argc > 6)
      return usage();
   if (argc == 6)
   {
      if (strcmp(argv[5], "--wait"))
         return usage();
      wait = 1;
   }
   int64_t rotation_id = 0;
   if (remediate && parse_id(argv[4], &rotation_id) != 0)
      return usage();
   if (!remediate && (!argv[4][0] || strlen(argv[4]) > 200))
      return usage();

   int64_t request_id = 0;
   if (kb_store_tenant_scope_begin(owner, team) != 0)
   {
      fputs("aimee-kb vault: tenant scope failed (is this a hardened tier? run migrations)\n",
            stderr);
      return 1;
   }
   int rc = kb_store_vault_rotation_request_add(actor, team, remediate ? "remediate" : "rotate",
                                                remediate ? "" : argv[4], rotation_id, &request_id);
   if (rc != 0 || kb_store_tenant_scope_commit() != 0)
   {
      if (rc != 0)
         kb_store_tenant_scope_rollback();
      fprintf(stderr, "aimee-kb vault %s: %s\n", sub,
              rc == KB_STORE_VAULT_ROTATION_REQUEST_UNKNOWN
                  ? (remediate ? "no such rotation for that team"
                               : "that team has no egress binding for that model")
                  : "the request was not recorded (admin required)");
      return 1;
   }
   if (wait)
      return wait_for(owner, actor, team, request_id, remediate);
   cJSON *o = cJSON_CreateObject();
   if (o)
   {
      cJSON_AddNumberToObject(o, "request_id", (double)request_id);
      cJSON_AddStringToObject(o, "request_state", "pending");
   }
   print_json(o);
   return 0;
}
