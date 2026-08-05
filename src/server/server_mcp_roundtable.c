/* Synchronous MCP adapter for roundtable.review.
 *
 * One wait per layer, and every layer blocks on the one below it:
 *
 *   thin client -> aimee-server -> event bus -> roundtable -> model
 *
 * Nothing in that chain polls, and nothing in it is provider-specific: the bus
 * carries a review request and carries a verdict back, whichever model the
 * panel happens to be configured with.
 *
 * This used to submit the review as an asynchronous /v1 op-run and hand the
 * caller a run id to poll with. The layer below it was already synchronous --
 * handle_roundtable_review blocks in obs_bus_module_call until the verdict
 * arrives -- so the op-run wrapper did not make anything concurrent that was
 * not already. It only moved completion-detection to the one place where
 * waiting is expensive: the model's turn loop.
 *
 * Polling there looks cheap and is not. A status check costs microseconds
 * server-side but an entire model turn on the client, and every turn re-sends
 * the whole accumulated conversation as input. Advertising poll_after_ms=1000
 * on a job that can legitimately run twenty minutes is a request for hundreds
 * of full-context round trips. Measured on one benchmark task it was 32 of 84
 * tool calls and roughly two thirds of the run's token cost, to learn nothing
 * the server could not have said once, when it was done.
 *
 * Blocking costs one parked connection thread instead. Connections are
 * thread-per-conn and detached, so a review that sits for twenty minutes stalls
 * only its own caller. */
#include "server_mcp_roundtable.h"

#include "server.h"
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

cJSON *mcp_roundtable_review(cJSON *args, uint32_t capabilities, char *err, size_t err_n)
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

   /* Preflight the capability so a refusal reads as a refusal rather than as a
    * dispatch error from inside the review. */
   uint32_t required = server_capability_for_method("roundtable.review");
   if (required && (capabilities & required) == 0)
   {
      cJSON_Delete(body);
      roundtable_error(err, err_n, "forbidden: insufficient capabilities");
      return NULL;
   }
   cJSON_AddStringToObject(body, "method", "roundtable.review");

   char *wire = cJSON_PrintUnformatted(body);
   cJSON_Delete(body);
   if (!wire)
   {
      roundtable_error(err, err_n, "out of memory");
      return NULL;
   }

   /* Blocks until the panel and chair are done: dispatch runs
    * handle_roundtable_review inline, which waits on the bus for the verdict.
    * The review's own deadline (from the resolved panel) bounds this wait. */
   char *response = malloc(SHTTP_RESP_MAX);
   if (!response)
   {
      free(wire);
      roundtable_error(err, err_n, "out of memory");
      return NULL;
   }
   response[0] = '\0';
   int status = loopback_rpc(wire, (int)strlen(wire), response, SHTTP_RESP_MAX, capabilities);
   free(wire);

   cJSON *verdict = cJSON_Parse(response);
   free(response);
   if (status < 200 || status >= 300 || !verdict)
   {
      const char *detail = verdict ? response_error(verdict) : NULL;
      roundtable_error(err, err_n, "roundtable review failed%s%s", detail ? ": " : "",
                       detail ? detail : "");
      cJSON_Delete(verdict);
      return NULL;
   }
   /* A dispatch can answer 2xx with an error object; that is a failed review,
    * not a verdict. */
   const char *detail = response_error(verdict);
   if (detail)
   {
      roundtable_error(err, err_n, "roundtable review failed: %s", detail);
      cJSON_Delete(verdict);
      return NULL;
   }
   return verdict;
}

/* There is deliberately no status/poll entry point. roundtable_review returns
 * the verdict, so there is nothing to ask about afterwards -- and a status tool
 * on the surface is an invitation to poll even when the call already blocks. */
