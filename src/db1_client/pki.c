/* pki.c: typed DB1 operations over the shared bus transport.
 * Wire operations are owned by server-go/modules/aimee/operations.json. */
/* clang-format off */
#include "pki_store.h"

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


static db1_family_t family = {"db1.pki", "pki", AIMEE_DB1_EVENT_PKI, AIMEE_DB1_STAGE_PKI, 2000, 0};


/* A write answers 0 or -1; the store either took it or it did not. */
static int write_result(int status)
{
   return status == (int)AIMEE_DB1_STATUS_OK ? 0 : -1;
}


int db1_pki_cert_upsert(const char *serial, const char *cn, long issued_at, long expires_at)
{
   if (!serial || !serial[0])
      return -1;
   char arg2[32];
   snprintf(arg2, sizeof arg2, "%lld", (long long)issued_at);
   char arg3[32];
   snprintf(arg3, sizeof arg3, "%lld", (long long)expires_at);
   const char *fields[] = {serial, cn ? cn : "", arg2, arg3};
   return write_result(db1_call(&family, AIMEE_DB1_OP_PKI_CERT_UPSERT, fields, 4, NULL, NULL, 0, NULL));
}

int db1_pki_cert_list(db1_pki_cert_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 4096)
      max = 4096;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", max);
   const char *fields[] = {arg0};
   char **wire_values = malloc((size_t)max * 5u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 5u * sizeof *wire_caps);
   char (*wire_scratch)[32] = malloc((size_t)max * 3u * sizeof *wire_scratch);
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
      wire_values[wire_row * 5u + 0u] = out[wire_row].serial;
      wire_caps[wire_row * 5u + 0u] = sizeof out[wire_row].serial;
      wire_values[wire_row * 5u + 1u] = out[wire_row].cn;
      wire_caps[wire_row * 5u + 1u] = sizeof out[wire_row].cn;
      wire_values[wire_row * 5u + 2u] = wire_scratch[wire_row * 3u + 0u];
      wire_caps[wire_row * 5u + 2u] = sizeof wire_scratch[wire_row * 3u + 0u];
      wire_values[wire_row * 5u + 3u] = wire_scratch[wire_row * 3u + 1u];
      wire_caps[wire_row * 5u + 3u] = sizeof wire_scratch[wire_row * 3u + 1u];
      wire_values[wire_row * 5u + 4u] = wire_scratch[wire_row * 3u + 2u];
      wire_caps[wire_row * 5u + 4u] = sizeof wire_scratch[wire_row * 3u + 2u];
   }
   uint32_t wire_filled = 0;
   int wire_status = db1_call(&family, AIMEE_DB1_OP_PKI_CERT_LIST, fields, 1, wire_values, wire_caps,
                           (uint32_t)(max * 5), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 5u != 0u)
   {
      free(wire_scratch);
      return -1;
   }
   int wire_rows = (int)(wire_filled / 5u);
   for (int wire_row = 0; wire_row < wire_rows; ++wire_row)
   {
      out[wire_row].issued_at = (int64_t)strtoll(wire_scratch[wire_row * 3u + 0u], NULL, 10);
      out[wire_row].expires_at = (int64_t)strtoll(wire_scratch[wire_row * 3u + 1u], NULL, 10);
      out[wire_row].revoked = (int)strtol(wire_scratch[wire_row * 3u + 2u], NULL, 10);
   }
   free(wire_scratch);
   return wire_rows;
}

int db1_pki_revoked_serials(char (*out)[DB1_PKI_SERIAL_MAX], int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 4096)
      max = 4096;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", max);
   const char *fields[] = {arg0};
   char **wire_values = malloc((size_t)max * 1u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 1u * sizeof *wire_caps);
   if (!wire_values || !wire_caps)
   {
      free(wire_values);
      free(wire_caps);
      return -1;
   }
   memset(out, 0, (size_t)max * sizeof *out);
   for (int wire_row = 0; wire_row < max; ++wire_row)
   {
      wire_values[wire_row * 1u + 0u] = out[wire_row];
      wire_caps[wire_row * 1u + 0u] = sizeof out[wire_row];
   }
   uint32_t wire_filled = 0;
   int wire_status = db1_call(&family, AIMEE_DB1_OP_PKI_REVOKED_SERIALS, fields, 1, wire_values, wire_caps,
                           (uint32_t)(max * 1), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 1u != 0u)
   {
      return -1;
   }
   int wire_rows = (int)(wire_filled / 1u);
   return wire_rows;
}

int db1_pki_cert_revoke(const char *serial)
{
   if (!serial || !serial[0])
      return -1;
   const char *fields[] = {serial};
   return write_result(db1_call(&family, AIMEE_DB1_OP_PKI_CERT_REVOKE, fields, 1, NULL, NULL, 0, NULL));
}

int db1_pki_cert_check(const char *serial, long now)
{
   if (!serial || !serial[0])
      return -1;
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%lld", (long long)now);
   const char *fields[] = {serial, arg1};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = db1_call(&family, AIMEE_DB1_OP_PKI_CERT_CHECK, fields, 2, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_pki_note_presentation(const char *serial, long now)
{
   if (!serial || !serial[0])
      return -1;
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%lld", (long long)now);
   const char *fields[] = {serial, arg1};
   return write_result(db1_call(&family, AIMEE_DB1_OP_PKI_NOTE_PRESENTATION, fields, 2, NULL, NULL, 0, NULL));
}

int db1_pki_ramp_init(int configured_mode)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", configured_mode);
   const char *fields[] = {arg0};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = db1_call(&family, AIMEE_DB1_OP_PKI_RAMP_INIT, fields, 1, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_pki_ramp_ready(long now)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%lld", (long long)now);
   const char *fields[] = {arg0};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = db1_call(&family, AIMEE_DB1_OP_PKI_RAMP_READY, fields, 1, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_pki_ramp_advance(long now)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%lld", (long long)now);
   const char *fields[] = {arg0};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = db1_call(&family, AIMEE_DB1_OP_PKI_RAMP_ADVANCE, fields, 1, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_pki_ramp_get(int *state_out, char *hash_out, size_t hash_len, long *advanced_at_out)
{
   if (!state_out || !hash_out || hash_len == 0 || !advanced_at_out)
      return -1;
   const char *const *fields = NULL;
   char slot0[32];
   char slot2[32];
   char *const values[] = {slot0, hash_out, slot2};
   const size_t caps[] = {sizeof slot0, hash_len, sizeof slot2};
   int wire_status = db1_call(&family, AIMEE_DB1_OP_PKI_RAMP_GET, fields, 0, values, caps, 3, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   *state_out = (int)strtol(slot0, NULL, 10);
   *advanced_at_out = (int64_t)strtoll(slot2, NULL, 10);
   return 0;
}

/* clang-format on */
