/* ensemble.c: typed DB1 operations over the shared bus transport.
 * Wire operations are owned by server-go/modules/aimee/operations.json. */
/* clang-format off */
#include "ensemble.h"

#include "db1_module_api.h"
#include "transport.h"

#include <aimee/audit/obs_bus.h>
#include <ouroboros/module_client.h>
#include <ouroboros/module_protocol.h>
#include "log.h"
#include "module_json_call.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static db1_family_t family = {"db1.ensemble", "ensemble", AIMEE_DB1_EVENT_ENSEMBLE, AIMEE_DB1_STAGE_ENSEMBLE, 2000, 0};

int db1_ensemble_create_id(const char *project_root, const char *config_dir, const char *template_name, const char *channel, const char *assignments_json, ensemble_id_result_t *out)
{
   if (!template_name || !template_name[0] || !out)
      return -1;
   const char *fields[] = {project_root ? project_root : "", config_dir ? config_dir : "", template_name, channel ? channel : "", assignments_json ? assignments_json : ""};
   char slot0[32];
   char slot2[32];
   memset(out, 0, sizeof *out);
   char *const values[] = {slot0, out->err, slot2};
   const size_t caps[] = {sizeof slot0, sizeof out->err, sizeof slot2};
   int wire_status = db1_call(&family, AIMEE_DB1_OP_ENSEMBLE_CREATE, fields, 5, values, caps, 3, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   out->rc = (int)strtol(slot0, NULL, 10);
   out->id = (int)strtol(slot2, NULL, 10);
   return 0;
}

int db1_ensemble_view(int id, ensemble_view_t *out)
{
   if (!out)
      return -1;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", id);
   const char *fields[] = {arg0};
   char slot0[32];
   char slot2[32];
   char slot6[32];
   char slot7[32];
   char slot8[32];
   char slot9[32];
   memset(out, 0, sizeof *out);
   out->request_ir = malloc(1048576u);
   if (!out->request_ir)
   {
      free(out->request_ir);
      free(out->context);
      memset(out, 0, sizeof *out);
      return -1;
   }
   out->request_ir[0] = '\0';
   out->context = malloc(1048576u);
   if (!out->context)
   {
      free(out->request_ir);
      free(out->context);
      memset(out, 0, sizeof *out);
      return -1;
   }
   out->context[0] = '\0';
   char *const values[] = {slot0, out->err, slot2, out->info.template_name, out->info.channel, out->info.status, slot6, slot7, slot8, slot9, out->info.phase_name, out->info.expected_agent, out->info.expected_role, out->info.paused_reason, out->info.created_at, out->info.updated_at, out->request_ir, out->context};
   const size_t caps[] = {sizeof slot0, sizeof out->err, sizeof slot2, sizeof out->info.template_name, sizeof out->info.channel, sizeof out->info.status, sizeof slot6, sizeof slot7, sizeof slot8, sizeof slot9, sizeof out->info.phase_name, sizeof out->info.expected_agent, sizeof out->info.expected_role, sizeof out->info.paused_reason, sizeof out->info.created_at, sizeof out->info.updated_at, 1048576u, 1048576u};
   int wire_status = db1_call(&family, AIMEE_DB1_OP_ENSEMBLE_VIEW, fields, 1, values, caps, 18, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      free(out->request_ir);
      free(out->context);
      memset(out, 0, sizeof *out);
      return -1;
   }
   out->rc = (int)strtol(slot0, NULL, 10);
   out->info.id = (int)strtol(slot2, NULL, 10);
   out->info.current_phase = (int)strtol(slot6, NULL, 10);
   out->info.current_turn = (int)strtol(slot7, NULL, 10);
   out->info.phase_count = (int)strtol(slot8, NULL, 10);
   out->info.turns_in_phase = (int)strtol(slot9, NULL, 10);
   char *shrunk_prompt = realloc(out->request_ir, strlen(out->request_ir) + 1u);
   if (shrunk_prompt)
      out->request_ir = shrunk_prompt;
   char *shrunk_context = realloc(out->context, strlen(out->context) + 1u);
   if (shrunk_context)
      out->context = shrunk_context;
   return 0;
}

int db1_ensemble_advance_view(int id, const char *sender, const char *message_ir, ensemble_view_t *out)
{
   if (!out)
      return -1;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", id);
   const char *fields[] = {arg0, sender ? sender : "", message_ir ? message_ir : ""};
   char slot0[32];
   char slot2[32];
   char slot6[32];
   char slot7[32];
   char slot8[32];
   char slot9[32];
   memset(out, 0, sizeof *out);
   out->request_ir = malloc(1048576u);
   if (!out->request_ir)
   {
      free(out->request_ir);
      free(out->context);
      memset(out, 0, sizeof *out);
      return -1;
   }
   out->request_ir[0] = '\0';
   out->context = malloc(1048576u);
   if (!out->context)
   {
      free(out->request_ir);
      free(out->context);
      memset(out, 0, sizeof *out);
      return -1;
   }
   out->context[0] = '\0';
   char *const values[] = {slot0, out->err, slot2, out->info.template_name, out->info.channel, out->info.status, slot6, slot7, slot8, slot9, out->info.phase_name, out->info.expected_agent, out->info.expected_role, out->info.paused_reason, out->info.created_at, out->info.updated_at, out->request_ir, out->context};
   const size_t caps[] = {sizeof slot0, sizeof out->err, sizeof slot2, sizeof out->info.template_name, sizeof out->info.channel, sizeof out->info.status, sizeof slot6, sizeof slot7, sizeof slot8, sizeof slot9, sizeof out->info.phase_name, sizeof out->info.expected_agent, sizeof out->info.expected_role, sizeof out->info.paused_reason, sizeof out->info.created_at, sizeof out->info.updated_at, 1048576u, 1048576u};
   int wire_status = db1_call(&family, AIMEE_DB1_OP_ENSEMBLE_ADVANCE, fields, 3, values, caps, 18, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      free(out->request_ir);
      free(out->context);
      memset(out, 0, sizeof *out);
      return -1;
   }
   out->rc = (int)strtol(slot0, NULL, 10);
   out->info.id = (int)strtol(slot2, NULL, 10);
   out->info.current_phase = (int)strtol(slot6, NULL, 10);
   out->info.current_turn = (int)strtol(slot7, NULL, 10);
   out->info.phase_count = (int)strtol(slot8, NULL, 10);
   out->info.turns_in_phase = (int)strtol(slot9, NULL, 10);
   char *shrunk_prompt = realloc(out->request_ir, strlen(out->request_ir) + 1u);
   if (shrunk_prompt)
      out->request_ir = shrunk_prompt;
   char *shrunk_context = realloc(out->context, strlen(out->context) + 1u);
   if (shrunk_context)
      out->context = shrunk_context;
   return 0;
}

int db1_ensemble_pause(int id, const char *reason, char *err, size_t errlen)
{
   if (!err || errlen == 0)
      return -1;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", id);
   const char *fields[] = {arg0, reason ? reason : ""};
   char slot_rc[32];
   char *const values[] = {err, slot_rc};
   const size_t caps[] = {errlen, sizeof slot_rc};
   int wire_status = db1_call(&family, AIMEE_DB1_OP_ENSEMBLE_PAUSE, fields, 2, values, caps, 2, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtol(slot_rc, NULL, 10);
}

int db1_ensemble_list_rows(ensemble_info_t **out, int *out_count)
{
   if (!out || !out_count)
      return -1;
   const char *const *fields = NULL;
   ensemble_info_t *wire_held = calloc((size_t)512, sizeof *wire_held);
   if (!wire_held)
      return -1;
   char **wire_values = malloc((size_t)512 * 14u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)512 * 14u * sizeof *wire_caps);
   char (*wire_scratch)[32] = malloc((size_t)512 * 5u * sizeof *wire_scratch);
   if (!wire_values || !wire_caps || !wire_scratch)
   {
      free(wire_values);
      free(wire_caps);
      free(wire_scratch);
      free(wire_held);
      return -1;
   }
   memset(wire_held, 0, (size_t)512 * sizeof *wire_held);
   for (int wire_row = 0; wire_row < 512; ++wire_row)
   {
      wire_values[wire_row * 14u + 0u] = wire_scratch[wire_row * 5u + 0u];
      wire_caps[wire_row * 14u + 0u] = sizeof wire_scratch[wire_row * 5u + 0u];
      wire_values[wire_row * 14u + 1u] = wire_held[wire_row].template_name;
      wire_caps[wire_row * 14u + 1u] = sizeof wire_held[wire_row].template_name;
      wire_values[wire_row * 14u + 2u] = wire_held[wire_row].channel;
      wire_caps[wire_row * 14u + 2u] = sizeof wire_held[wire_row].channel;
      wire_values[wire_row * 14u + 3u] = wire_held[wire_row].status;
      wire_caps[wire_row * 14u + 3u] = sizeof wire_held[wire_row].status;
      wire_values[wire_row * 14u + 4u] = wire_scratch[wire_row * 5u + 1u];
      wire_caps[wire_row * 14u + 4u] = sizeof wire_scratch[wire_row * 5u + 1u];
      wire_values[wire_row * 14u + 5u] = wire_scratch[wire_row * 5u + 2u];
      wire_caps[wire_row * 14u + 5u] = sizeof wire_scratch[wire_row * 5u + 2u];
      wire_values[wire_row * 14u + 6u] = wire_scratch[wire_row * 5u + 3u];
      wire_caps[wire_row * 14u + 6u] = sizeof wire_scratch[wire_row * 5u + 3u];
      wire_values[wire_row * 14u + 7u] = wire_scratch[wire_row * 5u + 4u];
      wire_caps[wire_row * 14u + 7u] = sizeof wire_scratch[wire_row * 5u + 4u];
      wire_values[wire_row * 14u + 8u] = wire_held[wire_row].phase_name;
      wire_caps[wire_row * 14u + 8u] = sizeof wire_held[wire_row].phase_name;
      wire_values[wire_row * 14u + 9u] = wire_held[wire_row].expected_agent;
      wire_caps[wire_row * 14u + 9u] = sizeof wire_held[wire_row].expected_agent;
      wire_values[wire_row * 14u + 10u] = wire_held[wire_row].expected_role;
      wire_caps[wire_row * 14u + 10u] = sizeof wire_held[wire_row].expected_role;
      wire_values[wire_row * 14u + 11u] = wire_held[wire_row].paused_reason;
      wire_caps[wire_row * 14u + 11u] = sizeof wire_held[wire_row].paused_reason;
      wire_values[wire_row * 14u + 12u] = wire_held[wire_row].created_at;
      wire_caps[wire_row * 14u + 12u] = sizeof wire_held[wire_row].created_at;
      wire_values[wire_row * 14u + 13u] = wire_held[wire_row].updated_at;
      wire_caps[wire_row * 14u + 13u] = sizeof wire_held[wire_row].updated_at;
   }
   uint32_t wire_filled = 0;
   int wire_status = db1_call(&family, AIMEE_DB1_OP_ENSEMBLE_LIST, fields, 0, wire_values, wire_caps,
                           (uint32_t)(512 * 14), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 14u != 0u)
   {
      free(wire_scratch);
      free(wire_held);
      return -1;
   }
   int wire_rows = (int)(wire_filled / 14u);
   for (int wire_row = 0; wire_row < wire_rows; ++wire_row)
   {
      wire_held[wire_row].id = (int)strtol(wire_scratch[wire_row * 5u + 0u], NULL, 10);
      wire_held[wire_row].current_phase = (int)strtol(wire_scratch[wire_row * 5u + 1u], NULL, 10);
      wire_held[wire_row].current_turn = (int)strtol(wire_scratch[wire_row * 5u + 2u], NULL, 10);
      wire_held[wire_row].phase_count = (int)strtol(wire_scratch[wire_row * 5u + 3u], NULL, 10);
      wire_held[wire_row].turns_in_phase = (int)strtol(wire_scratch[wire_row * 5u + 4u], NULL, 10);
   }
   free(wire_scratch);
   *out = wire_held;
   *out_count = wire_rows;
   return 0;
}

/* clang-format on */
