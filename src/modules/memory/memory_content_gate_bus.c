/* Content-gate transport for the Go memory process.
 *
 * C owns only request/reply framing. Secret detection, redaction, stability,
 * evidence, and sensitivity policy live in content_gate.go.
 */
#include "memory_platform.h"
#include "headers/module_json_call.h"

#include <aimee/memory/module_api.h>
#include <aimee/core/event_bus/module_protocol.h>

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
   int sensitive_status;
   int ephemeral;
   int evidence;
   char *redacted;
   char classification[16];
} memory_content_gate_reply_t;

static void memory_content_gate_reply_clear(memory_content_gate_reply_t *reply)
{
   free(reply->redacted);
   memset(reply, 0, sizeof(*reply));
}

static int memory_content_gate_call(const char *content, size_t capacity,
                                    memory_content_gate_reply_t *reply)
{
   memset(reply, 0, sizeof(*reply));
   cJSON *request = cJSON_CreateObject();
   if (!request || !cJSON_AddStringToObject(request, "operation", "content-gate") ||
       !cJSON_AddStringToObject(request, "content", content ? content : "") ||
       !cJSON_AddNumberToObject(request, "content_capacity", (double)capacity))
   {
      cJSON_Delete(request);
      return -1;
   }
   aimee_module_call_result_t result = AIMEE_MODULE_CALL_INTERNAL;
   cJSON *response = aimee_module_json_call(
       AIMEE_MEMORY_EVENT_DATA, AIMEE_MEMORY_STAGE_DATA, request,
       AIMEE_MODULE_MESSAGE_MAX_BODY, 5000, &result);
   if (!response)
      return -1;

   const cJSON *status =
       cJSON_GetObjectItemCaseSensitive(response, "sensitive_status");
   const cJSON *ephemeral = cJSON_GetObjectItemCaseSensitive(response, "ephemeral");
   const cJSON *evidence = cJSON_GetObjectItemCaseSensitive(response, "evidence");
   const char *redacted =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(response, "redacted"));
   const char *classification =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(response, "classification"));
   if (!cJSON_IsNumber(status) || status->valueint < 0 || status->valueint > 2 ||
       !classification)
   {
      cJSON_Delete(response);
      return -1;
   }
   reply->sensitive_status = status->valueint;
   reply->ephemeral = cJSON_IsTrue(ephemeral);
   reply->evidence = cJSON_IsTrue(evidence);
   reply->redacted = redacted ? strdup(redacted) : NULL;
   snprintf(reply->classification, sizeof(reply->classification), "%s", classification);
   cJSON_Delete(response);
   return 0;
}

int gate_check_sensitive(const char *content, char *redacted, size_t redacted_cap)
{
   if (!content || !content[0])
      return 0;
   memory_content_gate_reply_t reply;
   if (memory_content_gate_call(content, redacted_cap, &reply) != 0)
      return 2; /* fail closed: module outage must not persist a secret */
   int status = reply.sensitive_status;
   if (status == 1)
   {
      if (!redacted || !reply.redacted || strlen(reply.redacted) >= redacted_cap)
         status = 2;
      else
         memcpy(redacted, reply.redacted, strlen(reply.redacted) + 1);
   }
   memory_content_gate_reply_clear(&reply);
   return status;
}

int gate_check_ephemeral(const char *content)
{
   memory_content_gate_reply_t reply;
   if (memory_content_gate_call(content, 0, &reply) != 0)
      return 1;
   int ephemeral = reply.ephemeral;
   memory_content_gate_reply_clear(&reply);
   return ephemeral;
}

int gate_has_evidence_markers(const char *content)
{
   memory_content_gate_reply_t reply;
   if (memory_content_gate_call(content, 0, &reply) != 0)
      return 0;
   int evidence = reply.evidence;
   memory_content_gate_reply_clear(&reply);
   return evidence;
}

const char *memory_scan_content(char *content, size_t content_len)
{
   memory_content_gate_reply_t reply;
   if (memory_content_gate_call(content, content_len, &reply) != 0)
      return NULL;
   if (strcmp(reply.classification, "blocked") == 0)
   {
      memory_content_gate_reply_clear(&reply);
      return NULL;
   }
   if (reply.redacted && content && strlen(reply.redacted) < content_len)
      memcpy(content, reply.redacted, strlen(reply.redacted) + 1);
   const char *classification = strcmp(reply.classification, "restricted") == 0
                                    ? "restricted"
                                : strcmp(reply.classification, "sensitive") == 0 ? "sensitive"
                                                                                 : "normal";
   memory_content_gate_reply_clear(&reply);
   return classification;
}
