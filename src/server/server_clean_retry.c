/* Retry coordination only. Go owns snapshots, shared reservations and summaries;
 * normal Go memory assembly/source revalidation remains mandatory on each turn. */
#include "server.h"
#include "primary_session_adapter.h"
#include "request_context.h"
#include "db1_client/session_state.h"
#include "agent_exec.h"
#include "util.h"
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <stdlib.h>
#include <string.h>

static const char *retry_text(const cJSON *j, const char *key)
{
   const char *s = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, key));
   return s ? s : "";
}
static void retry_digest(const void *data, size_t len, char out[65])
{
   unsigned char hash[32];
   unsigned int n = 0;
   out[0] = 0;
   if (EVP_Digest(data, len, hash, &n, EVP_sha256(), NULL) != 1 || n != 32)
      return;
   for (unsigned int i = 0; i < n; i++)
      snprintf(out + i * 2, 3, "%02x", hash[i]);
}
static cJSON *retry_store(const char *principal, const char *sid, const char *op, cJSON *body)
{
   cJSON *request = cJSON_CreateObject();
   cJSON_AddStringToObject(request, "operation", op);
   cJSON_AddItemToObject(request, "retry", body);
   char *wire = cJSON_PrintUnformatted(request), *reply = malloc(65536);
   cJSON *result = NULL;
   if (wire && reply && db1_governed_action_apply(principal, sid, wire, reply, 65536) == 0)
      result = cJSON_ParseWithOpts(reply, NULL, 1);
   free(wire);
   free(reply);
   cJSON_Delete(request);
   return result;
}
int server_clean_retry_begin(const primary_session_request_t *req, cJSON **messages, char **summary,
                             char attempt[65], char *error, size_t error_len)
{
   const cJSON *options = cJSON_GetObjectItemCaseSensitive(req->task_request, "clean_retry");
   if (!options)
      return 0;
   const request_context_t *ctx = request_context_get();
   if (!cJSON_IsObject(options) || !ctx || !ctx->principal[0] || !ctx->request_id[0] ||
       !req->aimee_session_id || !req->aimee_session_id[0] || ctx->retry_attempt[0])
   {
      snprintf(error, error_len, "invalid clean retry request");
      return -1;
   }
   const cJSON *option;
   cJSON_ArrayForEach(option, options)
   {
      if (!option->string ||
          (strcmp(option->string, "previous_attempt") &&
           strcmp(option->string, "replace_constraints")) ||
          (!strcmp(option->string, "previous_attempt") && !cJSON_IsString(option)) ||
          (!strcmp(option->string, "replace_constraints") && !cJSON_IsBool(option)))
      {
         snprintf(error, error_len, "invalid clean retry option");
         return -1;
      }
   }
   if (policy_action_inherit(req->aimee_session_id) != 0)
   {
      snprintf(error, error_len, "retry action lineage unavailable");
      return -1;
   }
   cJSON *body = cJSON_CreateObject();
   cJSON_AddStringToObject(body, "request_id", ctx->request_id);
   cJSON_AddStringToObject(body, "input", req->user_prompt);
   cJSON_AddStringToObject(body, "previous_attempt", retry_text(options, "previous_attempt"));
   cJSON_AddBoolToObject(
       body, "replace_constraints",
       cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(options, "replace_constraints")));
   cJSON *result = retry_store(ctx->principal, req->aimee_session_id, "retry_begin", body);
   if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "allowed")))
   {
      snprintf(error, error_len, "clean retry refused: %s",
               result ? retry_text(result, "reason") : "owner_unavailable");
      cJSON_Delete(result);
      return -1;
   }
   snprintf(attempt, 65, "%s", retry_text(result, "attempt_id"));
   *summary = strdup(retry_text(result, "summary"));
   if (retry_text(options, "previous_attempt")[0])
   {
      cJSON_Delete(*messages);
      *messages = cJSON_CreateArray();
   }
   request_context_t next = *ctx;
   snprintf(next.retry_attempt, sizeof(next.retry_attempt), "%s", attempt);
   snprintf(next.retry_session, sizeof(next.retry_session), "%s", req->aimee_session_id);
   request_context_set(&next);
   cJSON_Delete(result);
   return 0;
}

/* Called for EVERY actual wire attempt, including automatic transport retries.
 * The exact provider bytes are charged once before handoff, without refund on
 * timeout. The policy's byte-priced cost is explicitly an estimate/reservation. */
int server_clean_retry_admit(const void *body, size_t length, const char *receipt)
{
   const request_context_t *ctx = request_context_get();
   if (!ctx || !ctx->retry_attempt[0])
      return 0;
   char digest[65], source[65], call_id[65];
   retry_digest(body, length, digest);
   retry_digest(ctx->memory_source_release, strlen(ctx->memory_source_release), source);
   /* A new ID per actual handoff; a lost owner reply spends the reservation and
    * refuses the send. It cannot turn a retry into a free provider call. */
   unsigned char random[32];
   if (RAND_bytes(random, sizeof(random)) != 1)
      return -1;
   retry_digest(random, sizeof(random), call_id);
   cJSON *request = cJSON_CreateObject(), *call = cJSON_CreateObject();
   cJSON_AddStringToObject(request, "attempt_id", ctx->retry_attempt);
   cJSON_AddStringToObject(call, "id", call_id);
   cJSON_AddNumberToObject(call, "reserved_bytes", (double)length);
   cJSON_AddStringToObject(call, "payload_sha256", digest);
   cJSON_AddStringToObject(call, "receipt_reference", receipt ? receipt : "");
   cJSON_AddStringToObject(call, "source_reference", source);
   cJSON_AddItemToObject(request, "call", call);
   cJSON *result = retry_store(ctx->principal, ctx->retry_session, "retry_send", request);
   int allowed = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "allowed"));
   if (!allowed)
      (void)request_context_refuse_assembly(result ? retry_text(result, "reason")
                                                   : "retry_owner_unavailable");
   cJSON_Delete(result);
   return allowed ? 0 : -1;
}
int server_clean_retry_finish(int rc, const agent_result_t *out)
{
   const request_context_t *ctx = request_context_get();
   if (!ctx || !ctx->retry_attempt[0])
      return 0;
   cJSON *body = cJSON_CreateObject();
   cJSON_AddStringToObject(body, "attempt_id", ctx->retry_attempt);
   const char *failure = rc == 0                                        ? "completed"
                         : !strcmp(out->stop_reason, "context_refused") ? "context_refused"
                                                                        : "execution_failure";
   cJSON_AddStringToObject(body, "failure_class", failure);
   cJSON *result = retry_store(ctx->principal, ctx->retry_session, "retry_finish", body);
   int allowed = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "allowed"));
   cJSON_Delete(result);
   request_context_t next = *ctx;
   next.retry_attempt[0] = next.retry_session[0] = 0;
   request_context_set(&next);
   return allowed ? 0 : -1;
}
