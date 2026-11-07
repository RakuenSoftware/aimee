#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "trajectory.h"
#include <aimee/ir/session_event.h>

static const char *actor = "owner";
static const char *reply_override;
static int dispatch_status = 1, load_status, load_calls;
static const char canonical[] =
    "[{\"role\":\"assistant\",\"blocks\":["
    "{\"type\":\"thinking\",\"text\":\"private reasoning\",\"signature\":\"native-signature\"},"
    "{\"type\":\"text\",\"text\":\"fixture-secret\"},"
    "{\"type\":\"tool_use\",\"tool_id\":\"native:opaque-id\",\"namespace\":\"tools\",\"name\":"
    "\"Read\","
    "\"input\":{\"exact\":9007199254740993,\"unicode\":\"astral 😀\"}}]},"
    "{\"role\":\"tool\",\"blocks\":[{\"type\":\"tool_result\",\"tool_id\":\"native:opaque-id\","
    "\"is_error\":false,\"content\":[{\"type\":\"text\",\"text\":\"fixture-secret\"}]}]}]";
static const char *history = canonical;

int db1_session_ir_history_load(const char *session, const char *scope, const char *principal,
                                const char *active_turn, char **out)
{
   load_calls++;
   assert(!strcmp(session, "fixture") && !strcmp(scope, "primary") && !active_turn[0]);
   *out = NULL;
   if (strcmp(principal, "owner"))
      return -1;
   if (!load_status)
      *out = strdup(history);
   return load_status;
}

int aimee_module_commands_dispatch(const char *method, const cJSON *args, cJSON **result)
{
   assert(!strcmp(method, "memory.screen_content"));
   assert(cJSON_GetObjectItemCaseSensitive(args, "capacity")->valueint == 8192);
   if (reply_override || dispatch_status != 1)
   {
      *result = reply_override ? cJSON_Parse(reply_override) : NULL;
      return dispatch_status;
   }
   const char *content = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "content"));
   assert(content);
   *result = cJSON_Parse(
       (!strcmp(content, "fixture-secret") || !strcmp(content, "api_key=\"ordinary\"") ||
        !strcmp(content, "token=12345") || !strcmp(content, "password={}"))
           ? "{\"status\":\"ok\",\"verdict\":\"redact\",\"redacted\":\"[REDACTED]\"}"
           : "{\"status\":\"ok\",\"verdict\":\"allow\"}");
   return 1;
}

static cJSON *export_ok(trajectory_opts_t *opts)
{
   char *out = NULL;
   assert(!trajectory_export("fixture", actor, opts, &out) && out);
   cJSON *root = cJSON_Parse(out);
   assert(root && !strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "schema")),
                          "aimee.trajectory.v2"));
   assert(!cJSON_GetObjectItemCaseSensitive(root, "outcome"));
   assert(!cJSON_GetObjectItemCaseSensitive(root, "governed_actions"));
   assert(!cJSON_GetObjectItemCaseSensitive(root, "steps"));
   cJSON *messages = cJSON_GetObjectItemCaseSensitive(root, "messages");
   for (int i = 0; i < cJSON_GetArraySize(messages); i++)
   {
      aimee_message_t message;
      assert(!aimee_ir_message_parse(cJSON_GetArrayItem(messages, i), &message));
      aimee_message_free(&message);
   }
   assert(strstr(out, "9007199254740993") && strstr(out, "native:opaque-id"));
   assert(strstr(out, "native-signature") && strstr(out, "astral 😀"));
   if (opts->redact)
      assert(strstr(out, "[REDACTED]") && !strstr(out, "fixture-secret"));
   free(out);
   return root;
}

int main(void)
{
   trajectory_opts_t opts = {.redact = 1};
   cJSON *root = export_ok(&opts);
   cJSON *fidelity = cJSON_GetObjectItemCaseSensitive(root, "fidelity");
   assert(cJSON_GetObjectItemCaseSensitive(fidelity, "redactions")->valueint == 2);
   assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(fidelity, "content_unchanged")));
   cJSON_Delete(root);
   opts.redact = 0;
   root = export_ok(&opts);
   fidelity = cJSON_GetObjectItemCaseSensitive(root, "fidelity");
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(fidelity, "content_unchanged")));
   cJSON_Delete(root);
   opts.redact = opts.compress = 1;
   opts.max_tool_result_bytes = 8;
   root = export_ok(&opts);
   fidelity = cJSON_GetObjectItemCaseSensitive(root, "fidelity");
   cJSON *omissions = cJSON_GetObjectItemCaseSensitive(fidelity, "omissions");
   assert(cJSON_GetArraySize(omissions) == 1);
   cJSON *omission = cJSON_GetArrayItem(omissions, 0);
   assert(cJSON_GetObjectItemCaseSensitive(omission, "message_index")->valueint == 1);
   assert(cJSON_GetObjectItemCaseSensitive(omission, "block_index")->valueint == 0);
   assert(strlen(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(omission, "sha256"))) == 64);
   cJSON_Delete(root);

   char *out = NULL;
   const char *replies[] = {"{}", "{\"status\":\"ok\",\"verdict\":\"reject\"}",
                            "{\"status\":\"ok\",\"verdict\":\"unknown\"}",
                            "{\"status\":\"error\",\"verdict\":\"allow\"}",
                            "{\"status\":\"ok\",\"verdict\":\"redact\"}"};
   for (size_t i = 0; i < sizeof replies / sizeof replies[0]; i++)
   {
      reply_override = replies[i];
      assert(trajectory_export("fixture", actor, &opts, &out) == -1 && !out);
   }
   reply_override = NULL;
   for (dispatch_status = -1; dispatch_status <= 0; dispatch_status++)
      assert(trajectory_export("fixture", actor, &opts, &out) == -1 && !out);
   dispatch_status = 1;
   const char *unsafe[] = {
       "[{\"role\":\"assistant\",\"blocks\":[{\"type\":\"tool_use\",\"tool_"
       "id\":\"fixture-secret\",\"name\":\"Read\",\"input\":{}}]}]",
       "[{\"role\":\"assistant\",\"blocks\":[{\"type\":\"thinking\",\"text\":"
       "\"private\",\"signature\":\"fixture-secret\"}]}]",
       "[{\"role\":\"assistant\",\"blocks\":[{\"type\":\"text\",\"text\":"
       "\"ok\"}],\"fixture-secret\":true}]",
       "[{\"role\":\"assistant\",\"blocks\":[{\"type\":\"tool_use\",\"tool_"
       "id\":\"id\",\"name\":\"Read\",\"input\":{\"fixture-secret\":true}}]}]",
       "[{\"role\":\"assistant\",\"blocks\":[{\"type\":\"tool_use\",\"tool_"
       "id\":\"id\",\"name\":\"Read\",\"input\":{\"api_key\":\"ordinary\"}}]}]",
       "[{\"role\":\"tool\",\"blocks\":[{\"type\":\"tool_result\",\"tool_id\":"
       "\"id\",\"is_error\":false,\"content\":{\"token\":12345}}]}]",
       "[{\"role\":\"assistant\",\"blocks\":[{\"type\":\"tool_use\",\"tool_id\":\"id\",\"name\":"
       "\"Read\",\"input\":{\"password\":{\"value\":\"ordinary\"}}}]}]",
       "[{\"role\":\"assistant\",\"content\":\"legacy\"}]",
       "[] trailing",
       "[{\"role\":\"assistant\",\"blocks\":[],\"role\":\"user\"}]"};
   for (size_t i = 0; i < sizeof unsafe / sizeof unsafe[0]; i++)
   {
      history = unsafe[i];
      assert(trajectory_export("fixture", actor, &opts, &out) == -1 && !out);
   }
   history = canonical;
   actor = "foreign";
   assert(trajectory_export("fixture", actor, &opts, &out) == -1 && !out);
   actor = "";
   int before = load_calls;
   assert(trajectory_export("fixture", actor, &opts, &out) == -1 && !out && load_calls == before);
   actor = "owner";
   for (load_status = -1; load_status <= 1; load_status += 2)
      assert(trajectory_export("fixture", actor, &opts, &out) == -1 && !out);
   puts("canonical trajectory screening: ok");
   return 0;
}
