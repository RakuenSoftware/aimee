/* db1_client/mgmt_jwks.c: the mgmt_jwks family, reached over the bus.
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
#include "mgmt_jwks_cache.h"

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

#define DB1_MGMT_JWKS_CALL_TIMEOUT_MS 2000

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
   LOG_WARN("db1.mgmt_jwks", "DB1 %s is unreachable (module call result %d)", "mgmt jwks",
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
   if (!obs_bus_module_available(AIMEE_DB1_EVENT_MGMT_JWKS))
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
   uint64_t deadline = aimee_module_call_deadline_ns(DB1_MGMT_JWKS_CALL_TIMEOUT_MS);
   aimee_module_call_result_t rc =
       obs_bus_module_call(AIMEE_DB1_EVENT_MGMT_JWKS, AIMEE_DB1_STAGE_MGMT_JWKS, 0, deadline,
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


int db1_mgmt_jwks_read(db1_mgmt_jwks_row_t *out)
{
   if (!out)
      return -1;
   const char *const *fields = NULL;
   char slot0[32];
   char slot1[32];
   char slot2[32];
   memset(out, 0, sizeof *out);
   char *const values[] = {slot0, slot1, slot2, out->envelope, out->envelope_sha256, out->manifest_sha256, out->trust_bundle_sha256};
   const size_t caps[] = {sizeof slot0, sizeof slot1, sizeof slot2, sizeof out->envelope, sizeof out->envelope_sha256, sizeof out->manifest_sha256, sizeof out->trust_bundle_sha256};
   int wire_status = call_stage(AIMEE_DB1_OP_MGMT_JWKS_READ, fields, 0, values, caps, 7, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return wire_status == (int)AIMEE_DB1_STATUS_MISSING ? 0 : -1;
   }
   out->generation = (int64_t)strtoll(slot0, NULL, 10);
   out->valid_from = (int64_t)strtoll(slot1, NULL, 10);
   out->valid_until = (int64_t)strtoll(slot2, NULL, 10);
   return 1;
}

int db1_mgmt_jwks_generation(int64_t *out)
{
   if (!out)
      return -1;
   const char *const *fields = NULL;
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_MGMT_JWKS_GENERATION, fields, 0, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return wire_status == (int)AIMEE_DB1_STATUS_MISSING ? 0 : -1;
   }
   *out = (int64_t)strtoll(slot0, NULL, 10);
   return 1;
}

int db1_mgmt_jwks_install(const db1_mgmt_jwks_install_t *in)
{
   if (!in)
      return -1;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%lld", (long long)in->valid_from);
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%lld", (long long)in->valid_until);
   char arg2[32];
   snprintf(arg2, sizeof arg2, "%lld", (long long)in->fetched_at);
   const char *fields[] = {arg0, arg1, arg2, in->jwks, in->envelope, in->envelope_sha256, in->manifest_sha256, in->trust_bundle_sha256};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_MGMT_JWKS_INSTALL, fields, 8, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

/* clang-format on */
