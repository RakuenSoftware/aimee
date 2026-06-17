/* cmd_trigger.c: `aimee trigger` CLI command — event-triggered autopilot */

#include "aimee.h"
#include "commands.h"
#include "headers/cli_client.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef V1_PROTOCOL_VERSION
#define V1_PROTOCOL_VERSION 1
#endif

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

static const char *tstr(cJSON *obj, const char *key)
{
   cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
   return cJSON_IsString(v) ? v->valuestring : "";
}

/* Truncate src into dst (buf must hold at least 51 bytes). Returns dst. */
static char *trunc50(char *dst, const char *src)
{
   if (!src || !src[0])
   {
      dst[0] = '\0';
      return dst;
   }
   size_t len = strlen(src);
   if (len <= 50)
   {
      memcpy(dst, src, len + 1);
   }
   else
   {
      memcpy(dst, src, 47);
      dst[47] = '.';
      dst[48] = '.';
      dst[49] = '.';
      dst[50] = '\0';
   }
   return dst;
}

/* Resolve the server socket path, printing an error on failure. */
static const char *trigger_socket(void)
{
   const char *sock = cli_ensure_server_for_method("trigger.list");
   if (!sock)
      fprintf(stderr, "aimee: server unavailable\n");
   return sock;
}

/* No-op kept for call-site symmetry: the thin client is connectionless per call
 * now (each trigger_rpc is a one-shot /v1 dispatch). Server availability is
 * already ensured by trigger_socket() → cli_ensure_server_for_method. */
static int trigger_connect(cli_conn_t *conn, const char *sock)
{
   (void)conn;
   (void)sock;
   return 0;
}

/* Send *req over the first-class /v1 dispatch (local aimee-http.sock), delete req.  Returns NULL
 * on transport error (error already printed). */
static cJSON *trigger_rpc(cli_conn_t *conn, cJSON *req)
{
   (void)conn;
   cJSON *resp = cli_v1_dispatch_local(req, CLIENT_DEFAULT_TIMEOUT_MS);
   cJSON_Delete(req);
   if (!resp)
      fprintf(stderr, "aimee: no response from server\n");
   return resp;
}

/* Check response for a server-side error.  Prints the message and returns 1
 * if an error is present, 0 otherwise. */
static int trigger_check_error(cJSON *resp)
{
   cJSON *err = cJSON_GetObjectItemCaseSensitive(resp, "error");
   if (cJSON_IsString(err) && err->valuestring[0])
   {
      fprintf(stderr, "aimee: %s\n", err->valuestring);
      return 1;
   }
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   if (cJSON_IsString(status) && strcmp(status->valuestring, "error") == 0)
   {
      cJSON *msg = cJSON_GetObjectItemCaseSensitive(resp, "message");
      if (cJSON_IsString(msg) && msg->valuestring[0])
         fprintf(stderr, "aimee: %s\n", msg->valuestring);
      else
         fprintf(stderr, "aimee: server error\n");
      return 1;
   }
   return 0;
}

/* ------------------------------------------------------------------ */
/* trigger list                                                          */
/* ------------------------------------------------------------------ */

static void trigger_cmd_list(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;

   const char *status_filter = NULL;
   for (int i = 0; i < argc - 1; i++)
   {
      if (strcmp(argv[i], "--status") == 0)
      {
         status_filter = argv[i + 1];
         i++;
      }
   }

   const char *sock = trigger_socket();
   if (!sock)
      return;

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", "trigger.list");
   cJSON_AddNumberToObject(req, "protocol_version", V1_PROTOCOL_VERSION);
   if (status_filter && status_filter[0])
      cJSON_AddStringToObject(req, "status", status_filter);

   cli_conn_t conn;
   if (trigger_connect(&conn, sock) != 0)
   {
      cJSON_Delete(req);
      return;
   }

   cJSON *resp = trigger_rpc(&conn, req);
   if (!resp)
      return;

   if (trigger_check_error(resp))
   {
      cJSON_Delete(resp);
      return;
   }

   cJSON *triggers = cJSON_GetObjectItemCaseSensitive(resp, "triggers");
   if (!cJSON_IsArray(triggers) || cJSON_GetArraySize(triggers) == 0)
   {
      printf("No trigger runs found.\n");
      cJSON_Delete(resp);
      return;
   }

   printf("%-36s  %-16s  %-10s  %-50s  %s\n", "ID", "SOURCE", "STATUS", "TASK", "QUEUED_AT");
   printf("%-36s  %-16s  %-10s  %-50s  %s\n", "--", "------", "------", "----", "---------");

   cJSON *t;
   char task_buf[51];
   cJSON_ArrayForEach(t, triggers)
   {
      printf("%-36s  %-16s  %-10s  %-50s  %s\n", tstr(t, "id"), tstr(t, "source"),
             tstr(t, "status"), trunc50(task_buf, tstr(t, "task")), tstr(t, "queued_at"));
   }

   cJSON_Delete(resp);
}

/* ------------------------------------------------------------------ */
/* trigger status                                                        */
/* ------------------------------------------------------------------ */

static void trigger_cmd_status(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;

   if (argc < 1 || !argv[0] || !argv[0][0])
   {
      fprintf(stderr, "usage: aimee trigger status <id>\n");
      return;
   }

   const char *sock = trigger_socket();
   if (!sock)
      return;

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", "trigger.status");
   cJSON_AddNumberToObject(req, "protocol_version", V1_PROTOCOL_VERSION);
   cJSON_AddStringToObject(req, "id", argv[0]);

   cli_conn_t conn;
   if (trigger_connect(&conn, sock) != 0)
   {
      cJSON_Delete(req);
      return;
   }

   cJSON *resp = trigger_rpc(&conn, req);
   if (!resp)
      return;

   if (trigger_check_error(resp))
   {
      cJSON_Delete(resp);
      return;
   }

   cJSON *t = cJSON_GetObjectItemCaseSensitive(resp, "trigger");
   if (!cJSON_IsObject(t))
      t = resp;

   printf("id:          %s\n", tstr(t, "id"));
   printf("source:      %s\n", tstr(t, "source"));
   printf("status:      %s\n", tstr(t, "status"));
   printf("task:        %s\n", tstr(t, "task"));
   printf("workspace:   %s\n", tstr(t, "workspace"));
   printf("queued_at:   %s\n", tstr(t, "queued_at"));
   printf("started_at:  %s\n", tstr(t, "started_at"));
   printf("finished_at: %s\n", tstr(t, "finished_at"));

   const char *result = tstr(t, "result");
   if (result[0])
      printf("result:      %s\n", result);

   const char *errstr = tstr(t, "error");
   if (errstr[0])
      printf("error:       %s\n", errstr);

   cJSON_Delete(resp);
}

/* ------------------------------------------------------------------ */
/* trigger cancel                                                        */
/* ------------------------------------------------------------------ */

static void trigger_cmd_cancel(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;

   if (argc < 1 || !argv[0] || !argv[0][0])
   {
      fprintf(stderr, "usage: aimee trigger cancel <id>\n");
      return;
   }

   const char *sock = trigger_socket();
   if (!sock)
      return;

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", "trigger.cancel");
   cJSON_AddNumberToObject(req, "protocol_version", V1_PROTOCOL_VERSION);
   cJSON_AddStringToObject(req, "id", argv[0]);

   cli_conn_t conn;
   if (trigger_connect(&conn, sock) != 0)
   {
      cJSON_Delete(req);
      return;
   }

   cJSON *resp = trigger_rpc(&conn, req);
   if (!resp)
      return;

   if (trigger_check_error(resp))
   {
      cJSON_Delete(resp);
      return;
   }

   cJSON *msg = cJSON_GetObjectItemCaseSensitive(resp, "message");
   if (cJSON_IsString(msg) && msg->valuestring[0])
      printf("%s\n", msg->valuestring);
   else
      printf("cancelled trigger %s\n", argv[0]);

   cJSON_Delete(resp);
}

/* ------------------------------------------------------------------ */
/* trigger fire                                                          */
/* ------------------------------------------------------------------ */

static void trigger_cmd_fire(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;

   const char *source = NULL;
   const char *event = NULL;
   const char *task = NULL;
   const char *workspace = NULL;
   const char *token = NULL;

   for (int i = 0; i < argc - 1; i++)
   {
      if (strcmp(argv[i], "--source") == 0)
      {
         source = argv[i + 1];
         i++;
      }
      else if (strcmp(argv[i], "--event") == 0)
      {
         event = argv[i + 1];
         i++;
      }
      else if (strcmp(argv[i], "--task") == 0)
      {
         task = argv[i + 1];
         i++;
      }
      else if (strcmp(argv[i], "--workspace") == 0)
      {
         workspace = argv[i + 1];
         i++;
      }
      else if (strcmp(argv[i], "--token") == 0)
      {
         token = argv[i + 1];
         i++;
      }
   }

   if (!source || !source[0])
   {
      fprintf(stderr, "usage: aimee trigger fire --source <source> --task <task> "
                      "[--workspace <ws>] [--token <auth_token>]\n");
      return;
   }
   if (!task || !task[0])
   {
      fprintf(stderr, "usage: aimee trigger fire --source <source> --task <task> "
                      "[--workspace <ws>] [--token <auth_token>]\n");
      return;
   }

   const char *sock = trigger_socket();
   if (!sock)
      return;

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", "trigger.fire");
   cJSON_AddNumberToObject(req, "protocol_version", V1_PROTOCOL_VERSION);
   cJSON_AddStringToObject(req, "source", source);
   cJSON_AddStringToObject(req, "task", task);
   if (event && event[0])
      cJSON_AddStringToObject(req, "event", event);
   if (workspace && workspace[0])
      cJSON_AddStringToObject(req, "workspace", workspace);
   if (token && token[0])
      cJSON_AddStringToObject(req, "auth_token", token);

   cli_conn_t conn;
   if (trigger_connect(&conn, sock) != 0)
   {
      cJSON_Delete(req);
      return;
   }

   cJSON *resp = trigger_rpc(&conn, req);
   if (!resp)
      return;

   if (trigger_check_error(resp))
   {
      cJSON_Delete(resp);
      return;
   }

   cJSON *tid = cJSON_GetObjectItemCaseSensitive(resp, "trigger_id");
   if (cJSON_IsString(tid) && tid->valuestring[0])
      printf("trigger_id: %s\n", tid->valuestring);
   else
   {
      cJSON *msg = cJSON_GetObjectItemCaseSensitive(resp, "message");
      if (cJSON_IsString(msg) && msg->valuestring[0])
         printf("%s\n", msg->valuestring);
      else
         printf("trigger fired\n");
   }

   cJSON_Delete(resp);
}

/* ------------------------------------------------------------------ */
/* Subcommand table and entry point                                      */
/* ------------------------------------------------------------------ */

static const subcmd_t trigger_subcmds[] = {
    {"list", "List trigger runs [--status <status>]", trigger_cmd_list},
    {"status", "Show details for a trigger run: <id>", trigger_cmd_status},
    {"cancel", "Cancel a trigger run: <id>", trigger_cmd_cancel},
    {"fire", "Fire a trigger: --source <s> --task <t> [--workspace <ws>] [--token <tok>]",
     trigger_cmd_fire},
    {NULL, NULL, NULL}};

const subcmd_t *get_trigger_subcmds(void)
{
   return trigger_subcmds;
}

void cmd_trigger(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
   {
      fprintf(stderr, "Usage: aimee trigger <subcommand> [options]\n\nSubcommands:\n");
      for (int i = 0; trigger_subcmds[i].name; i++)
         fprintf(stderr, "  %-10s %s\n", trigger_subcmds[i].name, trigger_subcmds[i].help);
      return;
   }

   if (subcmd_dispatch(trigger_subcmds, argv[0], ctx, argc - 1, argv + 1) != 0)
   {
      fprintf(stderr, "Unknown trigger subcommand: %s\n", argv[0]);
      fprintf(stderr, "Run 'aimee trigger' for usage.\n");
   }
}
