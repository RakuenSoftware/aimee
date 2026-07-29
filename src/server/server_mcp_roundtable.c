/* Async MCP adapter for the Go-owned roundtable service.
 *
 * A default roundtable can legitimately spend ten minutes on its panel and a
 * second ten-minute phase on its chairman. MCP clients commonly cap one tool
 * call at five minutes, so holding tools/call open discards a result that keeps
 * running server-side and serializes every later call behind it. Reuse the
 * first-class /v1 op-run lifecycle instead: submit once, then poll by run id. */
#include "server_mcp_roundtable.h"

#include "server_http.h"
#include "server_http_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void roundtable_error(char *err, size_t err_n, const char *fmt, ...)
{
   if (!err || err_n == 0)
      return;
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(err, err_n, fmt, ap);
   va_end(ap);
}

static const char *response_error(const cJSON *response)
{
   cJSON *error = cJSON_GetObjectItemCaseSensitive(response, "error");
   if (cJSON_IsString(error) && error->valuestring[0])
      return error->valuestring;
   if (cJSON_IsObject(error))
   {
      cJSON *message = cJSON_GetObjectItemCaseSensitive(error, "message");
      if (cJSON_IsString(message) && message->valuestring[0])
         return message->valuestring;
   }
   cJSON *message = cJSON_GetObjectItemCaseSensitive(response, "message");
   return cJSON_IsString(message) && message->valuestring[0] ? message->valuestring : NULL;
}

static void add_poll_contract(cJSON *run)
{
   cJSON *id = cJSON_GetObjectItemCaseSensitive(run, "id");
   if (cJSON_IsString(id) && !cJSON_GetObjectItemCaseSensitive(run, "run_id"))
      cJSON_AddStringToObject(run, "run_id", id->valuestring);
   cJSON *status = cJSON_GetObjectItemCaseSensitive(run, "status");
   if (cJSON_IsString(status) && strcmp(status->valuestring, "completed") != 0 &&
       strcmp(status->valuestring, "failed") != 0 && strcmp(status->valuestring, "cancelled") != 0)
   {
      cJSON_AddStringToObject(run, "next_tool", "roundtable_status");
      cJSON_AddNumberToObject(run, "poll_after_ms", 1000);
   }
}

cJSON *mcp_roundtable_submit(cJSON *args, uint32_t capabilities, char *err, size_t err_n)
{
   if (err && err_n)
      err[0] = '\0';
   cJSON *diff = cJSON_GetObjectItemCaseSensitive(args, "diff");
   if (!cJSON_IsString(diff) || !diff->valuestring || !diff->valuestring[0])
   {
      roundtable_error(err, err_n, "roundtable_review requires 'diff'");
      return NULL;
   }
   if (strlen(diff->valuestring) < 20)
   {
      roundtable_error(err, err_n, "roundtable_review requires 'diff' of at least 20 characters");
      return NULL;
   }

   cJSON *body = cJSON_CreateObject();
   if (!body)
   {
      roundtable_error(err, err_n, "out of memory");
      return NULL;
   }
   cJSON_AddStringToObject(body, "prompt", diff->valuestring);
   cJSON_AddStringToObject(body, "mode", "review");
   for (const char *const *field =
            (const char *const[]){"original_request", "artifact_stage", "workdir", NULL};
        *field; field++)
   {
      cJSON *value = cJSON_GetObjectItemCaseSensitive(args, *field);
      if (!value)
         continue;
      if (!cJSON_IsString(value) || !value->valuestring || !value->valuestring[0])
      {
         cJSON_Delete(body);
         roundtable_error(err, err_n,
                          "roundtable_review evidence fields must be non-empty strings");
         return NULL;
      }
      cJSON_AddStringToObject(body, *field, value->valuestring);
   }
   cJSON *brief = cJSON_GetObjectItemCaseSensitive(args, "brief");
   if (brief)
   {
      if (!cJSON_IsObject(brief) && !cJSON_IsString(brief))
      {
         cJSON_Delete(body);
         roundtable_error(err, err_n, "roundtable_review 'brief' must be a string or object");
         return NULL;
      }
      cJSON *brief_copy = cJSON_Duplicate(brief, 1);
      if (!brief_copy)
      {
         cJSON_Delete(body);
         roundtable_error(err, err_n, "out of memory");
         return NULL;
      }
      cJSON_AddItemToObject(body, "brief", brief_copy);
   }
   cJSON *roundtable = cJSON_GetObjectItemCaseSensitive(args, "roundtable");
   if (roundtable)
   {
      if (!cJSON_IsString(roundtable) || !roundtable->valuestring || !roundtable->valuestring[0])
      {
         cJSON_Delete(body);
         roundtable_error(err, err_n, "roundtable_review 'roundtable' must name a saved preset");
         return NULL;
      }
      cJSON_AddStringToObject(body, "roundtable", roundtable->valuestring);
   }

   char *wire = cJSON_PrintUnformatted(body);
   cJSON_Delete(body);
   if (!wire)
   {
      roundtable_error(err, err_n, "out of memory");
      return NULL;
   }
   char response[SHTTP_RESP_MAX] = "";
   int status = server_http_submit_op_run("roundtable.review", wire, capabilities, response,
                                          (int)sizeof(response));
   free(wire);
   cJSON *run = cJSON_Parse(response);
   if (status < 200 || status >= 300 || !run)
   {
      const char *detail = run ? response_error(run) : NULL;
      roundtable_error(err, err_n, "roundtable submission failed%s%s", detail ? ": " : "",
                       detail ? detail : "");
      cJSON_Delete(run);
      return NULL;
   }
   cJSON *id = cJSON_GetObjectItemCaseSensitive(run, "id");
   if (!cJSON_IsString(id) || !id->valuestring[0])
   {
      cJSON_Delete(run);
      roundtable_error(err, err_n, "roundtable submission returned no run id");
      return NULL;
   }
   add_poll_contract(run);
   return run;
}

cJSON *mcp_roundtable_status(cJSON *args, char *err, size_t err_n)
{
   if (err && err_n)
      err[0] = '\0';
   cJSON *run_id = cJSON_GetObjectItemCaseSensitive(args, "run_id");
   if (!cJSON_IsString(run_id) || !run_id->valuestring || !run_id->valuestring[0])
   {
      roundtable_error(err, err_n, "roundtable_status requires 'run_id'");
      return NULL;
   }

   char response[SHTTP_RESP_MAX] = "";
   int status = route_runs_get(run_id->valuestring, response, (int)sizeof(response));
   cJSON *run = cJSON_Parse(response);
   if (status < 200 || status >= 300 || !run)
   {
      const char *detail = run ? response_error(run) : NULL;
      roundtable_error(err, err_n, "roundtable run is unavailable%s%s", detail ? ": " : "",
                       detail ? detail : "");
      cJSON_Delete(run);
      return NULL;
   }
   cJSON *method = cJSON_GetObjectItemCaseSensitive(run, "method");
   if (!cJSON_IsString(method) || strcmp(method->valuestring, "roundtable.review") != 0)
   {
      cJSON_Delete(run);
      roundtable_error(err, err_n, "run id does not belong to a roundtable review");
      return NULL;
   }
   add_poll_contract(run);
   return run;
}
