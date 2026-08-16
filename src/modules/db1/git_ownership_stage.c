/* modules/db1/git_ownership_stage.c: the git ownership stage handler.
 *
 * GENERATED from src/modules/db1/eventcontract/operations.json by
 * scripts/gen_db1_contract.py. Do not edit.
 *
 * The serving half of the boundary: decode the frame the client encoded, call
 * the domain, and answer. The domain itself is hand-written and untouched --
 * only the wire around it is generated.
 *
 * clang-format is off for the body below: its canonical form is whatever this
 * generator emits. */
/* clang-format off */
#include "db1_stages.h"

#include "db1_module_api.h"
#include "git_ownership.h"

#include <string.h>

/* Read one counted field, refusing anything that would run past the end or
   carry an embedded NUL: every field here is spliced into a query parameter,
   and a NUL would silently shorten it into a different row. */
static int read_counted(const uint8_t *body, uint32_t len, uint32_t *offset, char *out,
                        size_t out_sz)
{
   if (*offset + 4u > len)
      return 1;
   uint32_t n = aimee_db1_get_u32(body + *offset);
   *offset += 4u;
   if (n > len || *offset + n > len || n == 0u || n >= out_sz)
      return 1;
   if (memchr(body + *offset, 0, n) != NULL)
      return 1;
   memcpy(out, body + *offset, n);
   out[n] = '\0';
   *offset += n;
   return 0;
}

static uint32_t write_reply(uint8_t *out, uint32_t cap, uint32_t *out_len, uint32_t status,
                            const char *value)
{
   uint32_t value_len = (uint32_t)strlen(value);
   if (cap < 8u + value_len)
      return AIMEE_DB1_STATUS_FAILED;
   aimee_db1_put_u32(out, status);
   aimee_db1_put_u32(out + 4u, value_len);
   if (value_len)
      memcpy(out + 8u, value, value_len);
   *out_len = 8u + value_len;
   return status;
}

aimee_module_status_t aimee_db1_stage_git_ownership(const uint8_t *request_body, uint32_t request_len,
                                             uint8_t *response_body, uint32_t response_capacity,
                                             uint32_t *response_len)
{
   if (request_len < 8u)
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   uint32_t op = aimee_db1_get_u32(request_body);
   uint32_t count = aimee_db1_get_u32(request_body + 4u);
   /* Bounds the fixed array below. Without it a well-formed frame declaring
      more fields than any operation takes writes past it. */
   if (count == 0u || count > AIMEE_DB1_FIELDS_MAX)
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;

   char field[AIMEE_DB1_FIELDS_MAX][AIMEE_DB1_FIELD_MAX];
   uint32_t offset = 8u;
   for (uint32_t i = 0; i < count; ++i)
      if (read_counted(request_body, request_len, &offset, field[i], sizeof field[i]) != 0)
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   /* Trailing bytes mean the caller and the module disagree about the op's
      arity, which is a contract mismatch rather than something to tolerate. */
   if (offset != request_len)
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;

   char value[AIMEE_DB1_FIELD_MAX];
   value[0] = '\0';
   int rc = -1;
   int reads = 0;

   switch (op)
   {
   case AIMEE_DB1_OP_OWNERSHIP_UPSERT:
      if (count != 3u)
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      rc = db1_git_ownership_upsert(field[0], field[1], field[2]);
      break;
   case AIMEE_DB1_OP_OWNERSHIP_DELETE:
      if (count != 2u)
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      rc = db1_git_ownership_delete(field[0], field[1]);
      break;
   case AIMEE_DB1_OP_OWNERSHIP_OWNER_GET:
      if (count != 2u)
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      rc = db1_git_ownership_get_owner(field[0], field[1], value, sizeof value);
      reads = 1;
      break;
   case AIMEE_DB1_OP_OWNERSHIP_BRANCH_FOR_SESSION:
      if (count != 2u)
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      rc = db1_git_ownership_get_branch_for_session(field[0], field[1], value, sizeof value);
      reads = 1;
      break;
   case AIMEE_DB1_OP_OWNERSHIP_SESSION_BY_PREFIX:
      if (count != 1u)
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      rc = db1_git_ownership_find_session_by_prefix(field[0], value, sizeof value);
      reads = 1;
      break;
   default:
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   }

   /* The two conventions must not be flattened. A read returns FOUND(1),
      not-found(0) or error(-1); a write returns 0 or -1. Mapping a read's -1
      onto MISSING would report a broken store as "nothing recorded", and the
      caller would act on an absence that was never established. */
   uint32_t status;
   if (reads)
   {
      if (rc < 0)
         status = AIMEE_DB1_STATUS_FAILED;
      else if (rc == 0 || !value[0])
         status = AIMEE_DB1_STATUS_MISSING;
      else
         status = AIMEE_DB1_STATUS_OK;
   }
   else
      status = (rc == 0) ? AIMEE_DB1_STATUS_OK : AIMEE_DB1_STATUS_FAILED;

   write_reply(response_body, response_capacity, response_len,
               status, (status == AIMEE_DB1_STATUS_OK && reads) ? value : "");
   return AIMEE_MODULE_STATUS_OK;
}
/* clang-format on */
