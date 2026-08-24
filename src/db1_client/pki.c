/* db1_client/pki.c: the pki family, reached over the bus.
 *
 * WAS GENERATED from the store catalog by scripts/gen_db1_contract.py. Both
 * moved on: the catalog is now server-go/modules/aimee/operations.json, and the
 * generator was deleted with the C module.
 *
 * So this is maintained BY HAND now, and the header used to say "Do not edit"
 * while pointing at a generator that no longer exists and a path that no longer
 * resolves -- which is a dead end at exactly the moment someone needs to change
 * something. Edit it, and keep it agreeing with the catalog:
 * scripts/check-db1-client-contract.py matches every call site here against the
 * catalog by arity and reply width, and runs in lint on every pull request.
 * That check is what replaced the generator.
 *
 * Same functions, same contract, different side of the boundary: the daemon
 * links this instead of the DB1 domain, so nothing that calls these had to
 * change.
 *
 * It lives OUTSIDE modules/db1 deliberately. The module's descriptor owns every
 * .c beside it and compiles them into the DB1 process, so a client with these
 * names in that directory would be linked twice into the one binary that must
 * not have it -- once as the caller and once as the implementation.
 *
 * clang-format is off for the body below: its canonical form is whatever this
 * generator emits, and reflowing generated output would put the file and the
 * catalog permanently one reformat apart. */
/* clang-format off */
#include "pki_store.h"

#include "db1_module_api.h"

#include <aimee/audit/obs_bus.h>
#include <aimee/core/event_bus/module_client.h>
#include <aimee/core/event_bus/module_protocol.h>
#include "log.h"
#include "module_json_call.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DB1_PKI_CALL_TIMEOUT_MS 2000

static void warn_unreachable(int reason)
{
   static int warned;
   if (warned)
      return;
   warned = 1;
   /* Said once per process: enough to tell a store that is down from one that
      is quiet, without one line per call. The numeric
      aimee_module_call_result_t, not its name, so this does not pull the whole
      event-bus library in behind the client for one string. */
   LOG_WARN("db1.pki", "DB1 %s is unreachable (module call result %d)", "pki",
            reason);
}

/* Size the frame from the arguments themselves.

   These carry prompts, results and JSON documents, not just identifiers, and
   in-process callers have always passed them whole. A fixed cap here would
   refuse exactly those calls and return the same -1 as a broken store -- fine
   in a test with short strings, wrong the first time a real prompt arrives. The
   bus bounds the message instead. */
static int frame_size(const char *const *fields, uint32_t count, size_t *need_out)
{
   /* Zero fields is a legal request: an operation that takes no arguments
      sends the header alone. The upper bound still applies. */
   if (count > AIMEE_DB1_FIELDS_MAX)
      return -1;
   size_t need = 8u;
   for (uint32_t i = 0; i < count; ++i)
   {
      /* Empty is legal on the wire: an optional field the caller left out
         travels as zero length. Which fields may be empty is the operation's
         business, checked before the frame is built. */
      if (!fields[i])
         return -1;
      size_t n = strlen(fields[i]);
      if (n > AIMEE_MODULE_MESSAGE_MAX_BODY - need - 4u)
         return -1;
      need += 4u + n;
   }
   *need_out = need;
   return 0;
}

/* op(u32) | field_count(u32) | (len(u32) | bytes) * count, per db1_module_api.h. */
static void encode(uint8_t *out, uint32_t op, const char *const *fields, uint32_t count)
{
   uint32_t at = 0;
   aimee_db1_put_u32(out + at, op);
   at += 4u;
   aimee_db1_put_u32(out + at, count);
   at += 4u;
   for (uint32_t i = 0; i < count; ++i)
   {
      uint32_t n = (uint32_t)strlen(fields[i]);
      aimee_db1_put_u32(out + at, n);
      at += 4u;
      memcpy(out + at, fields[i], n);
      at += n;
   }
}

/* Returns the module's status, or -1 when the call never produced one. */
/* Fills up to `slots` reply values, each into the buffer and capacity the
   caller supplied. A write passes none; a read passes one; a row passes one per
   member; a list passes one per member per row it is willing to accept.

   `filled_out` reports how many values the reply actually carried, which is how
   a list learns its length: the rows are not counted separately on the wire
   because an operation already knows how wide its rows are. Callers that expect
   a fixed shape pass NULL. */
static int call_stage(uint32_t op, const char *const *fields, uint32_t count, char *const *values,
                      const size_t *caps, uint32_t slots, uint32_t *filled_out)
{
   if (filled_out)
      *filled_out = 0u;
   for (uint32_t i = 0; i < slots; ++i)
      if (values[i] && caps[i])
         values[i][0] = '\0';
   /* A local check, not a probe: with nothing serving the stage there is no
      call to make, and saying so beats waiting out a deadline. */
   if (!obs_bus_module_available(AIMEE_DB1_EVENT_PKI))
   {
      warn_unreachable(AIMEE_MODULE_CALL_CAPABILITY_ABSENT);
      return -1;
   }

   size_t request_len = 0;
   if (frame_size(fields, count, &request_len) != 0)
      return -1;
   /* The reply is bounded by the caller's own buffer: it asked for at most
      value_len bytes, so there is no reason to hold more than that. */
   size_t response_cap = 8u;
   for (uint32_t i = 0; i < slots; ++i)
      response_cap += 4u + caps[i];
   uint8_t *request = malloc(request_len);
   uint8_t *response = malloc(response_cap);
   if (!request || !response)
   {
      free(request);
      free(response);
      return -1;
   }
   encode(request, op, fields, count);

   uint32_t response_len = 0;
   uint64_t deadline = aimee_module_call_deadline_ns(DB1_PKI_CALL_TIMEOUT_MS);
   aimee_module_call_result_t rc =
       obs_bus_module_call(AIMEE_DB1_EVENT_PKI, AIMEE_DB1_STAGE_PKI, 0, deadline,
                           request, (uint32_t)request_len, response, (uint32_t)response_cap,
                           &response_len, NULL, NULL);
   free(request);

   int result = -1;
   if (rc != AIMEE_MODULE_CALL_OK || response_len < 8u)
      warn_unreachable((int)rc);
   else
   {
      uint32_t status = aimee_db1_get_u32(response);
      uint32_t fields_in = aimee_db1_get_u32(response + 4u);
      /* Read the reply's own count rather than assuming an arity: a status with
         no values is how a write answers, one value is a read, and a member
         apiece is a row. */
      result = (int)status;
      /* More values than the caller has room for is a contract mismatch, not
         something to read the first few of: the caller asked for at most this
         many rows, and a stage answering with more is not answering this call. */
      if (fields_in > slots)
         result = -1;
      else if (filled_out)
         *filled_out = fields_in;
      /* Fewer values than the caller has slots for is the same contract
         mismatch read from the other side, and it used to pass: the unfilled
         slots keep the empty string cleared above, so the caller reads a row
         whose last members are blank and cannot tell that from a row that is
         blank. A list says how many rows it found through filled_out and is
         variable by construction; every other shape has one arity, and a stage
         answering with a different one is a stage built against a different
         version of this contract. Two processes, two binaries, two deployment
         times -- so say it rather than zero-fill. */
      else if (status == (uint32_t)AIMEE_DB1_STATUS_OK && fields_in != slots)
         result = -1;
      uint32_t at = 8u;
      for (uint32_t i = 0; i < fields_in && result != -1; ++i)
      {
         if (at + 4u > response_len)
         {
            result = -1;
            break;
         }
         uint32_t n = aimee_db1_get_u32(response + at);
         at += 4u;
         /* A reply whose declared length runs past what arrived is not a reply
            to read part of. */
         if (at + n > response_len)
         {
            result = -1;
            break;
         }
         if (i < slots && values[i] && caps[i])
         {
            /* No room for the terminator is no room: writing it would land one
               byte past the buffer the caller owns. */
            if (n >= caps[i])
               result = -1;
            else
            {
               memcpy(values[i], response + at, n);
               values[i][n] = '\0';
            }
         }
         at += n;
      }
   }
   free(response);
   return result;
}

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
   return write_result(call_stage(AIMEE_DB1_OP_PKI_CERT_UPSERT, fields, 4, NULL, NULL, 0, NULL));
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
   int wire_status = call_stage(AIMEE_DB1_OP_PKI_CERT_LIST, fields, 1, wire_values, wire_caps,
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
   int wire_status = call_stage(AIMEE_DB1_OP_PKI_REVOKED_SERIALS, fields, 1, wire_values, wire_caps,
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
   return write_result(call_stage(AIMEE_DB1_OP_PKI_CERT_REVOKE, fields, 1, NULL, NULL, 0, NULL));
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
   int wire_status = call_stage(AIMEE_DB1_OP_PKI_CERT_CHECK, fields, 2, values, caps, 1, NULL);
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
   return write_result(call_stage(AIMEE_DB1_OP_PKI_NOTE_PRESENTATION, fields, 2, NULL, NULL, 0, NULL));
}

int db1_pki_ramp_init(int configured_mode)
{
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", configured_mode);
   const char *fields[] = {arg0};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_PKI_RAMP_INIT, fields, 1, values, caps, 1, NULL);
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
   int wire_status = call_stage(AIMEE_DB1_OP_PKI_RAMP_READY, fields, 1, values, caps, 1, NULL);
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
   int wire_status = call_stage(AIMEE_DB1_OP_PKI_RAMP_ADVANCE, fields, 1, values, caps, 1, NULL);
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
   int wire_status = call_stage(AIMEE_DB1_OP_PKI_RAMP_GET, fields, 0, values, caps, 3, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   *state_out = (int)strtol(slot0, NULL, 10);
   *advanced_at_out = (int64_t)strtoll(slot2, NULL, 10);
   return 0;
}

/* clang-format on */
