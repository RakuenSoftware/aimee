#include "server_session_question.h"
#include "server_session_control.h"
#include "agent_config.h"
#include "db1_client/primary_sessions.h"
#include "platform_random.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int question_record(const char *session, const char *turn, const char *principal,
                           const aimee_session_event_t *event)
{
   cJSON *payload = aimee_ir_session_event_json(event);
   char *wire = payload ? cJSON_PrintUnformatted(payload) : NULL;
   cJSON_Delete(payload);
   char id[192], sequence[32];
   int n = snprintf(id, sizeof id, "%s:host-question:%s", event->request_id,
                    event->kind == AIMEE_SESSION_CONTROL_REQUEST ? "request" : "response");
   int result = wire && n > 0 && (size_t)n < sizeof id
                    ? db1_session_ir_append(session, id, turn, principal, wire, sequence)
                    : -1;
   free(wire);
   return result;
}
static char *question_answer(const cJSON *reply)
{
   const char *decision = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(reply, "decision"));
   if (!decision || strcmp(decision, "allow"))
      return safe_strdup(decision && !strcmp(decision, "deny") ? "error: question declined"
                                                               : "error: question cancelled");
   const cJSON *answers = cJSON_GetObjectItemCaseSensitive(reply, "answers");
   const cJSON *values = cJSON_GetObjectItemCaseSensitive(answers, "answer"), *value;
   size_t length = 1;
   cJSON_ArrayForEach(value, values)
   {
      const char *text = cJSON_GetStringValue(value);
      if (!text || length > 65536 || strlen(text) >= 65536 - length)
         return safe_strdup("error: invalid question answer");
      length += strlen(text) + 1;
   }
   char *answer = calloc(1, length);
   if (!answer)
      return safe_strdup("error: could not allocate question answer");
   size_t used = 0;
   cJSON_ArrayForEach(value, values)
   {
      if (used)
         answer[used++] = '\n';
      size_t n = strlen(value->valuestring);
      memcpy(answer + used, value->valuestring, n);
      used += n;
   }
   return answer;
}
char *server_session_question(const char *question, int timeout_seconds)
{
   if (!server_control_scope_current())
      return NULL;
   server_control_scope_t *scope = NULL;
   if (server_control_scope_capture(session_id(), agent_get_request_vault_principal(), &scope))
      return safe_strdup("error: question control owner does not match this execution");
   char *answer = NULL;
   if (!scope || !question || !question[0] || strlen(question) > 65535 || timeout_seconds <= 0)
   {
      answer = safe_strdup("error: invalid question or timeout");
      goto finish;
   }
   if (agent_request_cancelled())
   {
      answer = safe_strdup("error: question cancelled before publication");
      goto finish;
   }
   struct timespec start;
   if (clock_gettime(CLOCK_MONOTONIC, &start))
   {
      answer = safe_strdup("error: question clock unavailable");
      goto finish;
   }
   char id[33];
   cJSON *request = cJSON_CreateObject();
   cJSON *questions = request ? cJSON_AddArrayToObject(request, "questions") : NULL;
   cJSON *item = questions ? cJSON_CreateObject() : NULL;
   int valid = item && cJSON_AddItemToArray(questions, item);
   if (!valid)
      cJSON_Delete(item);
   valid = valid && cJSON_AddStringToObject(item, "id", "answer") &&
           cJSON_AddStringToObject(item, "question", question) &&
           cJSON_AddBoolToObject(item, "multiple", 0) && cJSON_AddArrayToObject(item, "options");
   if (!valid || platform_random_hex(id, 32) ||
       server_control_register_recorded(scope, id, "question", request, question_record))
   {
      cJSON_Delete(request);
      answer = safe_strdup("error: could not durably publish question");
      goto finish;
   }
   cJSON_Delete(request);
   for (;;)
   {
      struct timespec now;
      int clock_failed = clock_gettime(CLOCK_MONOTONIC, &now);
      int cancelled = agent_request_cancelled();
      int timed_out =
          !clock_failed &&
          (now.tv_sec - start.tv_sec > timeout_seconds ||
           (now.tv_sec - start.tv_sec == timeout_seconds && now.tv_nsec >= start.tv_nsec));
      if (clock_failed || cancelled || timed_out)
      {
         int failed = server_control_abandon(scope, id);
         answer = safe_strdup(failed      ? "error: could not durably close question"
                              : cancelled ? "error: question cancelled"
                              : timed_out ? "error: question timed out"
                                          : "error: question clock unavailable");
         break;
      }
      char *taken_id = NULL, *kind = NULL;
      cJSON *original = NULL, *reply = NULL;
      int taken = server_control_take(scope, id, &taken_id, &kind, &original, &reply);
      if (taken)
      {
         answer = taken > 0 ? question_answer(reply)
                            : safe_strdup("error: question owner closed or durable capture failed");
         free(taken_id);
         free(kind);
         cJSON_Delete(original);
         cJSON_Delete(reply);
         break;
      }
      const struct timespec tick = {.tv_nsec = 20000000};
      nanosleep(&tick, NULL);
   }
finish:
   server_control_scope_release(scope);
   return answer;
}
