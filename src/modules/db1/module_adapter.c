/* module_adapter.c: the DB1 process module's stage handler.
 *
 * DB1 is state, so the doctrine puts it behind a module reached over the event
 * bus rather than a library every component links. This is the first half of
 * that: the same C implementation, now serving a stage. Callers move across one
 * domain at a time (Phase B), and the implementation becomes Go afterwards
 * (Phase C), against a contract this file has already settled.
 *
 * The core owns attach, envelope validation, deadlines, cancellation and
 * shutdown, and export_c_repositories generates the main and the stage table
 * from the process contract. This file is only the handler, which is why it is
 * named aimee_module_handler: the generated main declares exactly that symbol. */
/* Deliberately NOT aimee.h: db1 depends on nothing from the core tree but
   aimee_home() and aimee_log(), which is what lets this link standalone. Pulling
   the umbrella header in would drag db2's rules.h along and make that false. */
#include <aimee/core/event_bus/module_runtime.h>
#include "db1_module_api.h"

#include <stdio.h>
#include <string.h>

#include "checkpoints.h" /* db1_economizer_state_load / _save */
#include "db1_stages.h"  /* the generated per-family handlers */

/* Read a length-prefixed field, refusing anything that would run past the end.
   Returns 0 on success and leaves *offset just after the field. */
static int read_field(const uint8_t *body, uint32_t len, uint32_t *offset, const uint8_t **out,
                      uint32_t *out_len)
{
   if (*offset + 4u > len)
      return 1;
   uint32_t n = aimee_db1_get_u32(body + *offset);
   *offset += 4u;
   if (n > len || *offset + n > len)
      return 1;
   *out = body + *offset;
   *out_len = n;
   *offset += n;
   return 0;
}

static uint32_t write_response(uint8_t *out, uint32_t cap, uint32_t *out_len, uint32_t status,
                               const char *json, uint32_t json_len)
{
   if (cap < 8u + json_len)
      return AIMEE_DB1_STATUS_FAILED;
   aimee_db1_put_u32(out, status);
   aimee_db1_put_u32(out + 4u, json_len);
   if (json_len)
      memcpy(out + 8u, json, json_len);
   *out_len = 8u + json_len;
   return status;
}

static aimee_module_status_t handle_economizer_state(const uint8_t *request_body,
                                                     uint32_t request_len, uint8_t *response_body,
                                                     uint32_t response_capacity,
                                                     uint32_t *response_len)
{
   uint32_t offset = 0;
   if (request_len < 4u)
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   uint32_t op = aimee_db1_get_u32(request_body);
   offset = 4u;

   const uint8_t *key = NULL, *json = NULL;
   uint32_t key_len = 0, json_len = 0;
   if (read_field(request_body, request_len, &offset, &key, &key_len) != 0)
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   if (read_field(request_body, request_len, &offset, &json, &json_len) != 0)
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   /* A key must be present and NUL-free: it is spliced into a query parameter,
      and an embedded NUL would silently shorten it. */
   if (key_len == 0 || key_len >= AIMEE_DB1_STATE_MAX || memchr(key, 0, key_len) != NULL)
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;

   char key_buf[512];
   if (key_len >= sizeof(key_buf))
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   memcpy(key_buf, key, key_len);
   key_buf[key_len] = '\0';

   if (op == AIMEE_DB1_OP_STATE_LOAD)
   {
      char blob[AIMEE_DB1_STATE_MAX];
      blob[0] = '\0';
      /* A miss is not an error: the first turn of a conversation has no state,
         and the caller starts from cold rather than failing. */
      int rc = db1_economizer_state_load(key_buf, blob, sizeof blob);
      uint32_t status = (rc == 0 && blob[0]) ? AIMEE_DB1_STATUS_OK : AIMEE_DB1_STATUS_MISSING;
      const char *payload = (status == AIMEE_DB1_STATUS_OK) ? blob : "";
      write_response(response_body, response_capacity, response_len, status, payload,
                     (uint32_t)strlen(payload));
      return AIMEE_MODULE_STATUS_OK;
   }

   if (op == AIMEE_DB1_OP_STATE_SAVE)
   {
      if (json_len >= AIMEE_DB1_STATE_MAX)
      {
         write_response(response_body, response_capacity, response_len, AIMEE_DB1_STATUS_TOO_LONG,
                        "", 0);
         return AIMEE_MODULE_STATUS_OK;
      }
      char blob[AIMEE_DB1_STATE_MAX];
      memcpy(blob, json, json_len);
      blob[json_len] = '\0';
      int rc = db1_economizer_state_save(key_buf, blob);
      write_response(response_body, response_capacity, response_len,
                     rc == 0 ? AIMEE_DB1_STATUS_OK : AIMEE_DB1_STATUS_FAILED, "", 0);
      return AIMEE_MODULE_STATUS_OK;
   }

   return AIMEE_MODULE_STATUS_INVALID_REQUEST;
}

aimee_module_status_t aimee_module_handler(const aimee_module_invocation_t *invocation,
                                           const uint8_t *request_body, uint32_t request_len,
                                           uint8_t *response_body, uint32_t response_capacity,
                                           uint32_t *response_len, void *user_data)
{
   (void)user_data;
   if (!invocation)
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;

   /* One family per stage. The runtime only ever invokes a stage this module
      declared, but dispatching explicitly keeps an added family from silently
      inheriting another one's decoder. */
   switch (invocation->stage_id)
   {
   case AIMEE_DB1_STAGE_ECONOMIZER_STATE:
      return handle_economizer_state(request_body, request_len, response_body, response_capacity,
                                     response_len);
   case AIMEE_DB1_STAGE_GIT_OWNERSHIP:
      return aimee_db1_stage_git_ownership(request_body, request_len, response_body,
                                           response_capacity, response_len);
   case AIMEE_DB1_STAGE_CONVERSATION:
      return aimee_db1_stage_conversation(request_body, request_len, response_body,
                                          response_capacity, response_len);
   case AIMEE_DB1_STAGE_AGENT_WORK:
      return aimee_db1_stage_agent_work(request_body, request_len, response_body, response_capacity,
                                        response_len);
   case AIMEE_DB1_STAGE_DELEGATION:
      return aimee_db1_stage_delegation(request_body, request_len, response_body, response_capacity,
                                        response_len);
   case AIMEE_DB1_STAGE_SESSIONS:
      return aimee_db1_stage_sessions(request_body, request_len, response_body, response_capacity,
                                      response_len);
   case AIMEE_DB1_STAGE_RUNTIME:
      return aimee_db1_stage_runtime(request_body, request_len, response_body, response_capacity,
                                     response_len);
   case AIMEE_DB1_STAGE_TELEMETRY:
      return aimee_db1_stage_telemetry(request_body, request_len, response_body, response_capacity,
                                       response_len);
   case AIMEE_DB1_STAGE_GUARDRAIL_STATE:
      return aimee_db1_stage_guardrail_state(request_body, request_len, response_body,
                                             response_capacity, response_len);
   case AIMEE_DB1_STAGE_ENSEMBLE:
      return aimee_db1_stage_ensemble(request_body, request_len, response_body, response_capacity,
                                      response_len);
   case AIMEE_DB1_STAGE_WORKFLOW:
      return aimee_db1_stage_workflow(request_body, request_len, response_body, response_capacity,
                                      response_len);
   case AIMEE_DB1_STAGE_IDENTITY:
      return aimee_db1_stage_identity(request_body, request_len, response_body, response_capacity,
                                      response_len);
   case AIMEE_DB1_STAGE_ROUNDTABLE:
      return aimee_db1_stage_roundtable(request_body, request_len, response_body, response_capacity,
                                        response_len);
   default:
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   }
}
