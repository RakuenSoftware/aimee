/* kb_vault_rotation_cli.c: see kb_vault_rotation_cli.h. */
#include "kb_vault_rotation_cli.h"

#include "cJSON.h"
#include "modules/kb/c/kb_store_tenant.h"
#include "org_egress.h"
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
         "       aimee-kb vault rotations <team_id>\n"
         "       aimee-kb vault adopt <team_id> --key-id=K --principal=P --agent=A --cred=C "
         "[--wait]\n"
         "       aimee-kb vault bind <team_id> <model_id> --billable=M --pricing-version=N\n"
         "                --key-id=K --principal=P --agent=A --cred=C --max-input=N "
         "--max-output=N [--disabled]\n"
         "       aimee-kb vault schedule <team_id> [<model_id> <days>|off]\n",
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
                         !strcmp(subcommand, "rotations") || !strcmp(subcommand, "adopt") ||
                         !strcmp(subcommand, "bind") || !strcmp(subcommand, "schedule"));
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
   if (r->slot_principal[0])
      cJSON_AddStringToObject(o, "slot_principal", r->slot_principal);
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

/* "--name=value" options after the positionals. Each name may appear once. */
typedef struct
{
   const char *name;
   const char *value;
} cli_opt_t;

static int parse_opts(int argc, char **argv, int first, cli_opt_t *opts, size_t n, int *wait,
                      int *disabled)
{
   for (int i = first; i < argc; i++)
   {
      if (wait && !strcmp(argv[i], "--wait") && !*wait)
      {
         *wait = 1;
         continue;
      }
      if (disabled && !strcmp(argv[i], "--disabled") && !*disabled)
      {
         *disabled = 1;
         continue;
      }
      size_t k = 0;
      for (; k < n; k++)
      {
         size_t len = strlen(opts[k].name);
         if (!strncmp(argv[i], opts[k].name, len) && argv[i][len] == '=')
         {
            if (opts[k].value)
               return -1;
            opts[k].value = argv[i] + len + 1;
            break;
         }
      }
      if (k == n)
         return -1;
   }
   return 0;
}

static int copy_opt(char *dst, size_t cap, const char *value, int required)
{
   if (!value)
      value = "";
   if ((required && !value[0]) || strlen(value) >= cap)
      return -1;
   memcpy(dst, value, strlen(value) + 1);
   return 0;
}

static int scoped_end(int rc)
{
   if (rc != 0)
   {
      kb_store_tenant_scope_rollback();
      return rc;
   }
   return kb_store_tenant_scope_commit() == 0 ? 0 : -1;
}

static int cmd_adopt(int argc, char **argv, const kb_principal_t *owner, const char *actor,
                     int64_t team)
{
   cli_opt_t o[] = {{"--key-id", NULL}, {"--principal", NULL}, {"--agent", NULL}, {"--cred", NULL}};
   int wait = 0;
   if (parse_opts(argc, argv, 4, o, 4, &wait, NULL) != 0 || !o[0].value || !o[1].value)
      return usage();
   int64_t id = 0;
   if (kb_store_tenant_scope_begin(owner, team) != 0)
      return fputs("aimee-kb vault adopt: tenant scope failed\n", stderr), 1;
   int rc = scoped_end(kb_store_vault_rotation_request_adopt(actor, team, o[0].value, o[1].value,
                                                             o[2].value ? o[2].value : "",
                                                             o[3].value ? o[3].value : "", &id));
   if (rc != 0)
   {
      fprintf(stderr, "aimee-kb vault adopt: %s\n",
              rc == KB_STORE_VAULT_ROTATION_REQUEST_UNKNOWN ? "that team has no such vault slot"
              : rc == KB_STORE_VAULT_ROTATION_REQUEST_CONFLICT
                  ? "the slot is already usable or rotating; rotate it instead"
                  : "the request was not recorded (admin required)");
      return 1;
   }
   if (wait)
      return wait_for(owner, actor, team, id, 0);
   cJSON *out = cJSON_CreateObject();
   if (out)
   {
      cJSON_AddNumberToObject(out, "request_id", (double)id);
      cJSON_AddStringToObject(out, "request_state", "pending");
   }
   print_json(out);
   return 0;
}

static int cmd_bind(int argc, char **argv, const kb_principal_t *owner, int64_t team)
{
   if (argc < 5 || !argv[4][0] || argv[4][0] == '-')
      return usage();
   cli_opt_t o[] = {{"--billable", NULL},  {"--pricing-version", NULL}, {"--key-id", NULL},
                    {"--principal", NULL}, {"--agent", NULL},           {"--cred", NULL},
                    {"--max-input", NULL}, {"--max-output", NULL}};
   int disabled = 0;
   kb_store_org_egress_binding_t b;
   memset(&b, 0, sizeof(b));
   b.team_id = team;
   if (parse_opts(argc, argv, 5, o, 8, NULL, &disabled) != 0 ||
       copy_opt(b.model_id, sizeof(b.model_id), argv[4], 1) != 0 ||
       copy_opt(b.billable_model, sizeof(b.billable_model), o[0].value, 1) != 0 || !o[1].value ||
       parse_id(o[1].value, &b.pricing_version) != 0 ||
       copy_opt(b.key_id, sizeof(b.key_id), o[2].value, 1) != 0 ||
       copy_opt(b.vault_principal, sizeof(b.vault_principal), o[3].value, 1) != 0 ||
       copy_opt(b.vault_agent, sizeof(b.vault_agent), o[4].value, 0) != 0 ||
       copy_opt(b.vault_cred, sizeof(b.vault_cred), o[5].value, 0) != 0 || !o[6].value ||
       parse_id(o[6].value, &b.max_input_tokens) != 0 || !o[7].value ||
       parse_id(o[7].value, &b.max_output_tokens) != 0)
      return usage();
   b.enabled = !disabled;
   if (kb_store_tenant_scope_begin(owner, team) != 0)
      return fputs("aimee-kb vault bind: tenant scope failed\n", stderr), 1;
   int rc = scoped_end(kb_store_org_egress_binding_set(&b));
   if (rc != 0)
   {
      fprintf(stderr, "aimee-kb vault bind: %s\n",
              rc == KB_STORE_EGRESS_ERR_DENIED
                  ? "refused: the model or price is not current, the slot has no usable "
                    "version for that key (adopt it first), or you are not an admin"
                  : "the binding was not recorded");
      return 1;
   }
   cJSON *out = cJSON_CreateObject();
   if (out)
   {
      cJSON_AddNumberToObject(out, "team_id", (double)team);
      cJSON_AddStringToObject(out, "model_id", b.model_id);
      cJSON_AddBoolToObject(out, "enabled", b.enabled);
   }
   print_json(out);
   return 0;
}

static int cmd_schedule(int argc, char **argv, const kb_principal_t *owner, const char *actor,
                        int64_t team)
{
   if (argc == 6)
   {
      int64_t days = 0;
      if (!argv[4][0] || strlen(argv[4]) > 200 ||
          (strcmp(argv[5], "off") && (parse_id(argv[5], &days) != 0 || days > 3650)))
         return usage();
      if (kb_store_tenant_scope_begin(owner, team) != 0)
         return fputs("aimee-kb vault schedule: tenant scope failed\n", stderr), 1;
      int rc = scoped_end(kb_store_vault_rotation_schedule_set(actor, team, argv[4], (int)days));
      if (rc != 0)
      {
         fprintf(stderr, "aimee-kb vault schedule: %s\n",
                 rc == KB_STORE_VAULT_ROTATION_REQUEST_UNKNOWN
                     ? "that team has no egress binding for that model"
                     : "the schedule was not recorded (admin required)");
         return 1;
      }
   }
   else if (argc != 4)
      return usage();
   static kb_store_vault_rotation_schedule_t rows[LIST_MAX];
   if (kb_store_tenant_scope_begin(owner, team) != 0)
      return fputs("aimee-kb vault schedule: tenant scope failed\n", stderr), 1;
   int n = kb_store_vault_rotation_schedule_list(actor, team, rows, LIST_MAX);
   if (scoped_end(n < 0 ? -1 : 0) != 0)
   {
      fputs("aimee-kb vault schedule: not authorized or KB_STORE unavailable\n", stderr);
      return 1;
   }
   cJSON *arr = cJSON_CreateArray();
   for (int i = 0; arr && i < n; i++)
   {
      cJSON *o = cJSON_CreateObject();
      if (!o)
         break;
      cJSON_AddStringToObject(o, "model_id", rows[i].model_id);
      cJSON_AddNumberToObject(o, "interval_days", rows[i].interval_days);
      if (rows[i].last_retired_at[0])
         cJSON_AddStringToObject(o, "last_rotated_at", rows[i].last_retired_at);
      cJSON_AddItemToArray(arr, o);
   }
   print_json(arr);
   return 0;
}

int kb_vault_rotation_cli(int argc, char **argv, const kb_principal_t *owner)
{
   const char *sub = argv[2];
   char actor[576];
   int64_t team = 0;
   if (argc < 4 || parse_id(argv[3], &team) != 0 || kb_identity_key(owner, actor, sizeof(actor)))
      return usage();

   if (!strcmp(sub, "adopt"))
      return cmd_adopt(argc, argv, owner, actor, team);
   if (!strcmp(sub, "bind"))
      return cmd_bind(argc, argv, owner, team);
   if (!strcmp(sub, "schedule"))
      return cmd_schedule(argc, argv, owner, actor, team);
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
