/* checkpoints.c: typed DB1 operations over the shared bus transport.
 * Wire operations are owned by server-go/modules/aimee/operations.json. */
/* clang-format off */
#include "checkpoints.h"

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


static db1_family_t family = {"db1.checkpoints", "checkpoints", AIMEE_DB1_EVENT_CHECKPOINTS, AIMEE_DB1_STAGE_CHECKPOINTS, 2000, 0};


/* A write answers 0 or -1; the store either took it or it did not. */
static int write_result(int status)
{
   return status == (int)AIMEE_DB1_STATUS_OK ? 0 : -1;
}


int db1_checkpoint_insert(const char *label, const char *session_id, int64_t task_id, const char *snapshot_json, db1_checkpoint_t *out)
{
   if (!label || !label[0] || !out)
      return -1;
   char arg2[32];
   snprintf(arg2, sizeof arg2, "%lld", (long long)task_id);
   const char *fields[] = {label, session_id ? session_id : "", arg2, snapshot_json ? snapshot_json : ""};
   char slot0[32];
   char slot1[32];
   memset(out, 0, sizeof *out);
   char *const values[] = {slot0, slot1, out->session_id, out->label, out->snapshot, out->created_at};
   const size_t caps[] = {sizeof slot0, sizeof slot1, sizeof out->session_id, sizeof out->label, sizeof out->snapshot, sizeof out->created_at};
   int wire_status = db1_call(&family, AIMEE_DB1_OP_CHECKPOINT_INSERT, fields, 4, values, caps, 6, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   out->id = (int64_t)strtoll(slot0, NULL, 10);
   out->task_id = (int64_t)strtoll(slot1, NULL, 10);
   return 0;
}

int db1_checkpoint_get(int64_t id, db1_checkpoint_t *out)
{
   if (!out)
      return -1;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%lld", (long long)id);
   const char *fields[] = {arg0};
   char slot0[32];
   char slot1[32];
   memset(out, 0, sizeof *out);
   char *const values[] = {slot0, slot1, out->session_id, out->label, out->snapshot, out->created_at};
   const size_t caps[] = {sizeof slot0, sizeof slot1, sizeof out->session_id, sizeof out->label, sizeof out->snapshot, sizeof out->created_at};
   int wire_status = db1_call(&family, AIMEE_DB1_OP_CHECKPOINT_GET, fields, 1, values, caps, 6, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   out->id = (int64_t)strtoll(slot0, NULL, 10);
   out->task_id = (int64_t)strtoll(slot1, NULL, 10);
   return 0;
}

int db1_checkpoint_list(int limit, db1_checkpoint_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 64)
      max = 64;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", limit);
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", max);
   const char *fields[] = {arg0, arg1};
   char **wire_values = malloc((size_t)max * 6u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 6u * sizeof *wire_caps);
   char (*wire_scratch)[32] = malloc((size_t)max * 2u * sizeof *wire_scratch);
   if (!wire_values || !wire_caps || !wire_scratch)
   {
      free(wire_values);
      free(wire_caps);
      free(wire_scratch);
      return -1;
   }
   memset(out, 0, (size_t)max * sizeof *out);
   for (int wire_row = 0; wire_row < max; ++wire_row)
   {
      wire_values[wire_row * 6u + 0u] = wire_scratch[wire_row * 2u + 0u];
      wire_caps[wire_row * 6u + 0u] = sizeof wire_scratch[wire_row * 2u + 0u];
      wire_values[wire_row * 6u + 1u] = wire_scratch[wire_row * 2u + 1u];
      wire_caps[wire_row * 6u + 1u] = sizeof wire_scratch[wire_row * 2u + 1u];
      wire_values[wire_row * 6u + 2u] = out[wire_row].session_id;
      wire_caps[wire_row * 6u + 2u] = sizeof out[wire_row].session_id;
      wire_values[wire_row * 6u + 3u] = out[wire_row].label;
      wire_caps[wire_row * 6u + 3u] = sizeof out[wire_row].label;
      wire_values[wire_row * 6u + 4u] = out[wire_row].snapshot;
      wire_caps[wire_row * 6u + 4u] = sizeof out[wire_row].snapshot;
      wire_values[wire_row * 6u + 5u] = out[wire_row].created_at;
      wire_caps[wire_row * 6u + 5u] = sizeof out[wire_row].created_at;
   }
   uint32_t wire_filled = 0;
   int wire_status = db1_call(&family, AIMEE_DB1_OP_CHECKPOINT_LIST, fields, 2, wire_values, wire_caps,
                           (uint32_t)(max * 6), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 6u != 0u)
   {
      free(wire_scratch);
      return -1;
   }
   int wire_rows = (int)(wire_filled / 6u);
   for (int wire_row = 0; wire_row < wire_rows; ++wire_row)
   {
      out[wire_row].id = (int64_t)strtoll(wire_scratch[wire_row * 2u + 0u], NULL, 10);
      out[wire_row].task_id = (int64_t)strtoll(wire_scratch[wire_row * 2u + 1u], NULL, 10);
   }
   free(wire_scratch);
   return wire_rows;
}

int db1_checkpoint_delete(int64_t id)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%lld", (long long)id);
   const char *fields[] = {arg0};
   return write_result(db1_call(&family, AIMEE_DB1_OP_CHECKPOINT_DELETE, fields, 1, NULL, NULL, 0, NULL));
}

/* clang-format on */
