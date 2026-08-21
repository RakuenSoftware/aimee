/* modules/db1/guardrail_state_stage.c: the guardrail state stage handler.
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
#include "session_state.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Copy one counted field out of the frame, NUL-terminated.

   The frame bounds the field, not a fixed cap: these carry prompts, results and
   JSON documents, and an in-process caller has always passed them whole. An
   embedded NUL is still refused -- every field is spliced into a query
   parameter, and a NUL would silently shorten it into a different row. */
static int read_counted(const uint8_t *body, uint32_t len, uint32_t *offset, char **cursor,
                        const char **out)
{
   if (*offset + 4u > len)
      return 1;
   uint32_t n = aimee_db1_get_u32(body + *offset);
   *offset += 4u;
   if (n > len || *offset + n > len)
      return 1;
   if (memchr(body + *offset, 0, n) != NULL)
      return 1;
   memcpy(*cursor, body + *offset, n);
   (*cursor)[n] = '\0';
   *out = *cursor;
   *cursor += n + 1u;
   *offset += n;
   return 0;
}

/* Parse a field the catalog declared as an integer. Refuses anything that is
   not exactly a number: a partial parse would turn "12abc" into 12 and act on a
   value the caller never sent. */
static int parse_int(const char *text, int *out)
{{
   if (!text || !text[0])
      return 1;
   char *end = NULL;
   errno = 0;
   long value = strtol(text, &end, 10);
   if (errno != 0 || !end || *end != '\0' || value < INT_MIN || value > INT_MAX)
      return 1;
   *out = (int)value;
   return 0;
}}
/* The same, for a member the catalog declared as a 64-bit integer. */
static int parse_int64(const char *text, int64_t *out)
{
   if (!text || !text[0])
      return 1;
   char *end = NULL;
   errno = 0;
   long long value = strtoll(text, &end, 10);
   if (errno != 0 || !end || *end != '\0')
      return 1;
   *out = (int64_t)value;
   return 0;
}

/* The same, for a member the catalog declared unsigned. Signed parsing would
   accept "-1" and store it as the largest hash there is. */
static int parse_uint64(const char *text, uint64_t *out)
{
   if (!text || !text[0] || text[0] == '-')
      return 1;
   char *end = NULL;
   errno = 0;
   unsigned long long value = strtoull(text, &end, 10);
   if (errno != 0 || !end || *end != '\0')
      return 1;
   *out = (uint64_t)value;
   return 0;
}

/* status(u32) | field_count(u32) | (len(u32) | bytes) * count. A write answers
   with no values, a read with one, a row with a value per member. */
static uint32_t write_reply(uint8_t *out, uint32_t cap, uint32_t *out_len, uint32_t status,
                            const char *const *values, uint32_t count)
{
   uint32_t at = 8u;
   for (uint32_t i = 0; i < count; ++i)
   {
      uint32_t n = (uint32_t)strlen(values[i]);
      if (cap < at + 4u + n)
         return AIMEE_DB1_STATUS_FAILED;
      aimee_db1_put_u32(out + at, n);
      at += 4u;
      if (n)
         memcpy(out + at, values[i], n);
      at += n;
   }
   if (cap < 8u)
      return AIMEE_DB1_STATUS_FAILED;
   aimee_db1_put_u32(out, status);
   aimee_db1_put_u32(out + 4u, count);
   *out_len = at;
   return status;
}

aimee_module_status_t aimee_db1_stage_guardrail_state(const uint8_t *request_body, uint32_t request_len,
                                             uint8_t *response_body, uint32_t response_capacity,
                                             uint32_t *response_len)
{
   if (request_len < 8u)
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   uint32_t op = aimee_db1_get_u32(request_body);
   uint32_t count = aimee_db1_get_u32(request_body + 4u);
   /* Bounds the fixed array below. Without it a well-formed frame declaring
      more fields than any operation takes writes past it. Zero is allowed:
      an operation that takes no arguments decodes no fields, and the arity
      check in its own case is what refuses a frame that carries some. */
   if (count > AIMEE_DB1_FIELDS_MAX)
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;

   /* One allocation for every field, sized by the frame that carried them: the
      fields plus a NUL each cannot exceed this. */
   const char *field[AIMEE_DB1_FIELDS_MAX];
   char *scratch = malloc((size_t)request_len + AIMEE_DB1_FIELDS_MAX);
   if (!scratch)
      return AIMEE_MODULE_STATUS_INTERNAL;
   char *cursor = scratch;
   aimee_module_status_t decoded = AIMEE_MODULE_STATUS_OK;

   uint32_t offset = 8u;
   for (uint32_t i = 0; i < count; ++i)
      if (read_counted(request_body, request_len, &offset, &cursor, &field[i]) != 0)
         decoded = AIMEE_MODULE_STATUS_INVALID_REQUEST;
   /* Trailing bytes mean the caller and the module disagree about the op's
      arity, which is a contract mismatch rather than something to tolerate. */
   if (offset != request_len)
      decoded = AIMEE_MODULE_STATUS_INVALID_REQUEST;
   if (decoded != AIMEE_MODULE_STATUS_OK)
   {
      free(scratch);
      return decoded;
   }

   char value[AIMEE_DB1_VALUE_MAX];
   value[0] = '\0';
   int rc = -1;
   int reads = 0;
   /* A row answers with a value per member; a plain read answers with one; a
      list answers with a value per member per row. */
   const char *const *rows = NULL;
   uint32_t row_count = 0u;
   /* A list returns its length in rc, where a read returns found/not-found, so
      the two cannot share a status mapping. The three owned blocks below hold
      the domain's rows, the cell pointers into them and the text for numeric
      members: all three must outlive write_reply, because that is what reads
      them. Declared unconditionally so this stays one readable flow -- unlike
      the static helpers above, an unused local costs nothing. */
   int listed = 0;
   /* Set by an operation whose C return says 1 found / 0 nothing / negative
      failed. A row-returning domain usually answers 0 or -1 and has no such
      distinction; one that does must not have it flattened, or "the queue is
      empty" and "the queue is broken" reach the caller as the same answer. */
   int found = 0;
   session_state_t row_session_state_t;
   db1_session_state_summary_t row_db1_session_state_summary_t;
   const char *row_slots[386];
   char row_text[151][32];
   /* A domain that returns a string hands over the allocation with it. The
      reply is written straight out of it rather than copied into value: the
      stack buffer is sized for identifiers and these carry documents. */
   char *text_owned = NULL;
   void *domain_rows = NULL;
   void *cells_owned = NULL;
   void *numeric_owned = NULL;

   switch (op)
   {
   case AIMEE_DB1_OP_SESSION_STATE_LOAD:
   {
      if (count != 1u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      memset(&row_session_state_t, 0, sizeof row_session_state_t);
      rc = db1_session_state_load(field[0], &row_session_state_t);
      snprintf(row_text[0], sizeof row_text[0], "%d", row_session_state_t.seen_count);
      snprintf(row_text[1], sizeof row_text[1], "%lld", (long long)row_session_state_t.active_task_id);
      snprintf(row_text[2], sizeof row_text[2], "%d", row_session_state_t.hook_call_count);
      snprintf(row_text[3], sizeof row_text[3], "%d", row_session_state_t.dirty);
      snprintf(row_text[4], sizeof row_text[4], "%d", row_session_state_t.worktree_count);
      snprintf(row_text[5], sizeof row_text[5], "%d", row_session_state_t.is_delegate);
      snprintf(row_text[6], sizeof row_text[6], "%d", row_session_state_t.orch_direct_edits);
      snprintf(row_text[7], sizeof row_text[7], "%d", row_session_state_t.orch_nudge_sent);
      snprintf(row_text[8], sizeof row_text[8], "%d", row_session_state_t.skill_find_symbols_advisory_sent);
      snprintf(row_text[9], sizeof row_text[9], "%d", row_session_state_t.skill_condition_waiting_advisory_sent);
      snprintf(row_text[10], sizeof row_text[10], "%d", row_session_state_t.skill_tdd_advisory_sent);
      snprintf(row_text[11], sizeof row_text[11], "%d", row_session_state_t.tdd_writes[0].is_test);
      snprintf(row_text[12], sizeof row_text[12], "%d", row_session_state_t.tdd_writes[1].is_test);
      snprintf(row_text[13], sizeof row_text[13], "%d", row_session_state_t.tdd_writes[2].is_test);
      snprintf(row_text[14], sizeof row_text[14], "%d", row_session_state_t.tdd_writes[3].is_test);
      snprintf(row_text[15], sizeof row_text[15], "%d", row_session_state_t.tdd_writes[4].is_test);
      snprintf(row_text[16], sizeof row_text[16], "%d", row_session_state_t.tdd_writes[5].is_test);
      snprintf(row_text[17], sizeof row_text[17], "%d", row_session_state_t.tdd_writes[6].is_test);
      snprintf(row_text[18], sizeof row_text[18], "%d", row_session_state_t.tdd_writes[7].is_test);
      snprintf(row_text[19], sizeof row_text[19], "%d", row_session_state_t.tdd_write_count);
      snprintf(row_text[20], sizeof row_text[20], "%d", row_session_state_t.read_path_count);
      snprintf(row_text[21], sizeof row_text[21], "%llu", (unsigned long long)row_session_state_t.file_hashes[0].content_hash);
      snprintf(row_text[22], sizeof row_text[22], "%llu", (unsigned long long)row_session_state_t.file_hashes[1].content_hash);
      snprintf(row_text[23], sizeof row_text[23], "%llu", (unsigned long long)row_session_state_t.file_hashes[2].content_hash);
      snprintf(row_text[24], sizeof row_text[24], "%llu", (unsigned long long)row_session_state_t.file_hashes[3].content_hash);
      snprintf(row_text[25], sizeof row_text[25], "%llu", (unsigned long long)row_session_state_t.file_hashes[4].content_hash);
      snprintf(row_text[26], sizeof row_text[26], "%llu", (unsigned long long)row_session_state_t.file_hashes[5].content_hash);
      snprintf(row_text[27], sizeof row_text[27], "%llu", (unsigned long long)row_session_state_t.file_hashes[6].content_hash);
      snprintf(row_text[28], sizeof row_text[28], "%llu", (unsigned long long)row_session_state_t.file_hashes[7].content_hash);
      snprintf(row_text[29], sizeof row_text[29], "%llu", (unsigned long long)row_session_state_t.file_hashes[8].content_hash);
      snprintf(row_text[30], sizeof row_text[30], "%llu", (unsigned long long)row_session_state_t.file_hashes[9].content_hash);
      snprintf(row_text[31], sizeof row_text[31], "%llu", (unsigned long long)row_session_state_t.file_hashes[10].content_hash);
      snprintf(row_text[32], sizeof row_text[32], "%llu", (unsigned long long)row_session_state_t.file_hashes[11].content_hash);
      snprintf(row_text[33], sizeof row_text[33], "%llu", (unsigned long long)row_session_state_t.file_hashes[12].content_hash);
      snprintf(row_text[34], sizeof row_text[34], "%llu", (unsigned long long)row_session_state_t.file_hashes[13].content_hash);
      snprintf(row_text[35], sizeof row_text[35], "%llu", (unsigned long long)row_session_state_t.file_hashes[14].content_hash);
      snprintf(row_text[36], sizeof row_text[36], "%llu", (unsigned long long)row_session_state_t.file_hashes[15].content_hash);
      snprintf(row_text[37], sizeof row_text[37], "%llu", (unsigned long long)row_session_state_t.file_hashes[16].content_hash);
      snprintf(row_text[38], sizeof row_text[38], "%llu", (unsigned long long)row_session_state_t.file_hashes[17].content_hash);
      snprintf(row_text[39], sizeof row_text[39], "%llu", (unsigned long long)row_session_state_t.file_hashes[18].content_hash);
      snprintf(row_text[40], sizeof row_text[40], "%llu", (unsigned long long)row_session_state_t.file_hashes[19].content_hash);
      snprintf(row_text[41], sizeof row_text[41], "%llu", (unsigned long long)row_session_state_t.file_hashes[20].content_hash);
      snprintf(row_text[42], sizeof row_text[42], "%llu", (unsigned long long)row_session_state_t.file_hashes[21].content_hash);
      snprintf(row_text[43], sizeof row_text[43], "%llu", (unsigned long long)row_session_state_t.file_hashes[22].content_hash);
      snprintf(row_text[44], sizeof row_text[44], "%llu", (unsigned long long)row_session_state_t.file_hashes[23].content_hash);
      snprintf(row_text[45], sizeof row_text[45], "%llu", (unsigned long long)row_session_state_t.file_hashes[24].content_hash);
      snprintf(row_text[46], sizeof row_text[46], "%llu", (unsigned long long)row_session_state_t.file_hashes[25].content_hash);
      snprintf(row_text[47], sizeof row_text[47], "%llu", (unsigned long long)row_session_state_t.file_hashes[26].content_hash);
      snprintf(row_text[48], sizeof row_text[48], "%llu", (unsigned long long)row_session_state_t.file_hashes[27].content_hash);
      snprintf(row_text[49], sizeof row_text[49], "%llu", (unsigned long long)row_session_state_t.file_hashes[28].content_hash);
      snprintf(row_text[50], sizeof row_text[50], "%llu", (unsigned long long)row_session_state_t.file_hashes[29].content_hash);
      snprintf(row_text[51], sizeof row_text[51], "%llu", (unsigned long long)row_session_state_t.file_hashes[30].content_hash);
      snprintf(row_text[52], sizeof row_text[52], "%llu", (unsigned long long)row_session_state_t.file_hashes[31].content_hash);
      snprintf(row_text[53], sizeof row_text[53], "%llu", (unsigned long long)row_session_state_t.file_hashes[32].content_hash);
      snprintf(row_text[54], sizeof row_text[54], "%llu", (unsigned long long)row_session_state_t.file_hashes[33].content_hash);
      snprintf(row_text[55], sizeof row_text[55], "%llu", (unsigned long long)row_session_state_t.file_hashes[34].content_hash);
      snprintf(row_text[56], sizeof row_text[56], "%llu", (unsigned long long)row_session_state_t.file_hashes[35].content_hash);
      snprintf(row_text[57], sizeof row_text[57], "%llu", (unsigned long long)row_session_state_t.file_hashes[36].content_hash);
      snprintf(row_text[58], sizeof row_text[58], "%llu", (unsigned long long)row_session_state_t.file_hashes[37].content_hash);
      snprintf(row_text[59], sizeof row_text[59], "%llu", (unsigned long long)row_session_state_t.file_hashes[38].content_hash);
      snprintf(row_text[60], sizeof row_text[60], "%llu", (unsigned long long)row_session_state_t.file_hashes[39].content_hash);
      snprintf(row_text[61], sizeof row_text[61], "%llu", (unsigned long long)row_session_state_t.file_hashes[40].content_hash);
      snprintf(row_text[62], sizeof row_text[62], "%llu", (unsigned long long)row_session_state_t.file_hashes[41].content_hash);
      snprintf(row_text[63], sizeof row_text[63], "%llu", (unsigned long long)row_session_state_t.file_hashes[42].content_hash);
      snprintf(row_text[64], sizeof row_text[64], "%llu", (unsigned long long)row_session_state_t.file_hashes[43].content_hash);
      snprintf(row_text[65], sizeof row_text[65], "%llu", (unsigned long long)row_session_state_t.file_hashes[44].content_hash);
      snprintf(row_text[66], sizeof row_text[66], "%llu", (unsigned long long)row_session_state_t.file_hashes[45].content_hash);
      snprintf(row_text[67], sizeof row_text[67], "%llu", (unsigned long long)row_session_state_t.file_hashes[46].content_hash);
      snprintf(row_text[68], sizeof row_text[68], "%llu", (unsigned long long)row_session_state_t.file_hashes[47].content_hash);
      snprintf(row_text[69], sizeof row_text[69], "%llu", (unsigned long long)row_session_state_t.file_hashes[48].content_hash);
      snprintf(row_text[70], sizeof row_text[70], "%llu", (unsigned long long)row_session_state_t.file_hashes[49].content_hash);
      snprintf(row_text[71], sizeof row_text[71], "%llu", (unsigned long long)row_session_state_t.file_hashes[50].content_hash);
      snprintf(row_text[72], sizeof row_text[72], "%llu", (unsigned long long)row_session_state_t.file_hashes[51].content_hash);
      snprintf(row_text[73], sizeof row_text[73], "%llu", (unsigned long long)row_session_state_t.file_hashes[52].content_hash);
      snprintf(row_text[74], sizeof row_text[74], "%llu", (unsigned long long)row_session_state_t.file_hashes[53].content_hash);
      snprintf(row_text[75], sizeof row_text[75], "%llu", (unsigned long long)row_session_state_t.file_hashes[54].content_hash);
      snprintf(row_text[76], sizeof row_text[76], "%llu", (unsigned long long)row_session_state_t.file_hashes[55].content_hash);
      snprintf(row_text[77], sizeof row_text[77], "%llu", (unsigned long long)row_session_state_t.file_hashes[56].content_hash);
      snprintf(row_text[78], sizeof row_text[78], "%llu", (unsigned long long)row_session_state_t.file_hashes[57].content_hash);
      snprintf(row_text[79], sizeof row_text[79], "%llu", (unsigned long long)row_session_state_t.file_hashes[58].content_hash);
      snprintf(row_text[80], sizeof row_text[80], "%llu", (unsigned long long)row_session_state_t.file_hashes[59].content_hash);
      snprintf(row_text[81], sizeof row_text[81], "%llu", (unsigned long long)row_session_state_t.file_hashes[60].content_hash);
      snprintf(row_text[82], sizeof row_text[82], "%llu", (unsigned long long)row_session_state_t.file_hashes[61].content_hash);
      snprintf(row_text[83], sizeof row_text[83], "%llu", (unsigned long long)row_session_state_t.file_hashes[62].content_hash);
      snprintf(row_text[84], sizeof row_text[84], "%llu", (unsigned long long)row_session_state_t.file_hashes[63].content_hash);
      snprintf(row_text[85], sizeof row_text[85], "%d", row_session_state_t.file_hash_count);
      snprintf(row_text[86], sizeof row_text[86], "%lld", (long long)row_session_state_t.ap_hits[0].pattern_id);
      snprintf(row_text[87], sizeof row_text[87], "%d", row_session_state_t.ap_hits[0].hits);
      snprintf(row_text[88], sizeof row_text[88], "%lld", (long long)row_session_state_t.ap_hits[1].pattern_id);
      snprintf(row_text[89], sizeof row_text[89], "%d", row_session_state_t.ap_hits[1].hits);
      snprintf(row_text[90], sizeof row_text[90], "%lld", (long long)row_session_state_t.ap_hits[2].pattern_id);
      snprintf(row_text[91], sizeof row_text[91], "%d", row_session_state_t.ap_hits[2].hits);
      snprintf(row_text[92], sizeof row_text[92], "%lld", (long long)row_session_state_t.ap_hits[3].pattern_id);
      snprintf(row_text[93], sizeof row_text[93], "%d", row_session_state_t.ap_hits[3].hits);
      snprintf(row_text[94], sizeof row_text[94], "%lld", (long long)row_session_state_t.ap_hits[4].pattern_id);
      snprintf(row_text[95], sizeof row_text[95], "%d", row_session_state_t.ap_hits[4].hits);
      snprintf(row_text[96], sizeof row_text[96], "%lld", (long long)row_session_state_t.ap_hits[5].pattern_id);
      snprintf(row_text[97], sizeof row_text[97], "%d", row_session_state_t.ap_hits[5].hits);
      snprintf(row_text[98], sizeof row_text[98], "%lld", (long long)row_session_state_t.ap_hits[6].pattern_id);
      snprintf(row_text[99], sizeof row_text[99], "%d", row_session_state_t.ap_hits[6].hits);
      snprintf(row_text[100], sizeof row_text[100], "%lld", (long long)row_session_state_t.ap_hits[7].pattern_id);
      snprintf(row_text[101], sizeof row_text[101], "%d", row_session_state_t.ap_hits[7].hits);
      snprintf(row_text[102], sizeof row_text[102], "%lld", (long long)row_session_state_t.ap_hits[8].pattern_id);
      snprintf(row_text[103], sizeof row_text[103], "%d", row_session_state_t.ap_hits[8].hits);
      snprintf(row_text[104], sizeof row_text[104], "%lld", (long long)row_session_state_t.ap_hits[9].pattern_id);
      snprintf(row_text[105], sizeof row_text[105], "%d", row_session_state_t.ap_hits[9].hits);
      snprintf(row_text[106], sizeof row_text[106], "%lld", (long long)row_session_state_t.ap_hits[10].pattern_id);
      snprintf(row_text[107], sizeof row_text[107], "%d", row_session_state_t.ap_hits[10].hits);
      snprintf(row_text[108], sizeof row_text[108], "%lld", (long long)row_session_state_t.ap_hits[11].pattern_id);
      snprintf(row_text[109], sizeof row_text[109], "%d", row_session_state_t.ap_hits[11].hits);
      snprintf(row_text[110], sizeof row_text[110], "%lld", (long long)row_session_state_t.ap_hits[12].pattern_id);
      snprintf(row_text[111], sizeof row_text[111], "%d", row_session_state_t.ap_hits[12].hits);
      snprintf(row_text[112], sizeof row_text[112], "%lld", (long long)row_session_state_t.ap_hits[13].pattern_id);
      snprintf(row_text[113], sizeof row_text[113], "%d", row_session_state_t.ap_hits[13].hits);
      snprintf(row_text[114], sizeof row_text[114], "%lld", (long long)row_session_state_t.ap_hits[14].pattern_id);
      snprintf(row_text[115], sizeof row_text[115], "%d", row_session_state_t.ap_hits[14].hits);
      snprintf(row_text[116], sizeof row_text[116], "%lld", (long long)row_session_state_t.ap_hits[15].pattern_id);
      snprintf(row_text[117], sizeof row_text[117], "%d", row_session_state_t.ap_hits[15].hits);
      snprintf(row_text[118], sizeof row_text[118], "%lld", (long long)row_session_state_t.ap_hits[16].pattern_id);
      snprintf(row_text[119], sizeof row_text[119], "%d", row_session_state_t.ap_hits[16].hits);
      snprintf(row_text[120], sizeof row_text[120], "%lld", (long long)row_session_state_t.ap_hits[17].pattern_id);
      snprintf(row_text[121], sizeof row_text[121], "%d", row_session_state_t.ap_hits[17].hits);
      snprintf(row_text[122], sizeof row_text[122], "%lld", (long long)row_session_state_t.ap_hits[18].pattern_id);
      snprintf(row_text[123], sizeof row_text[123], "%d", row_session_state_t.ap_hits[18].hits);
      snprintf(row_text[124], sizeof row_text[124], "%lld", (long long)row_session_state_t.ap_hits[19].pattern_id);
      snprintf(row_text[125], sizeof row_text[125], "%d", row_session_state_t.ap_hits[19].hits);
      snprintf(row_text[126], sizeof row_text[126], "%lld", (long long)row_session_state_t.ap_hits[20].pattern_id);
      snprintf(row_text[127], sizeof row_text[127], "%d", row_session_state_t.ap_hits[20].hits);
      snprintf(row_text[128], sizeof row_text[128], "%lld", (long long)row_session_state_t.ap_hits[21].pattern_id);
      snprintf(row_text[129], sizeof row_text[129], "%d", row_session_state_t.ap_hits[21].hits);
      snprintf(row_text[130], sizeof row_text[130], "%lld", (long long)row_session_state_t.ap_hits[22].pattern_id);
      snprintf(row_text[131], sizeof row_text[131], "%d", row_session_state_t.ap_hits[22].hits);
      snprintf(row_text[132], sizeof row_text[132], "%lld", (long long)row_session_state_t.ap_hits[23].pattern_id);
      snprintf(row_text[133], sizeof row_text[133], "%d", row_session_state_t.ap_hits[23].hits);
      snprintf(row_text[134], sizeof row_text[134], "%lld", (long long)row_session_state_t.ap_hits[24].pattern_id);
      snprintf(row_text[135], sizeof row_text[135], "%d", row_session_state_t.ap_hits[24].hits);
      snprintf(row_text[136], sizeof row_text[136], "%lld", (long long)row_session_state_t.ap_hits[25].pattern_id);
      snprintf(row_text[137], sizeof row_text[137], "%d", row_session_state_t.ap_hits[25].hits);
      snprintf(row_text[138], sizeof row_text[138], "%lld", (long long)row_session_state_t.ap_hits[26].pattern_id);
      snprintf(row_text[139], sizeof row_text[139], "%d", row_session_state_t.ap_hits[26].hits);
      snprintf(row_text[140], sizeof row_text[140], "%lld", (long long)row_session_state_t.ap_hits[27].pattern_id);
      snprintf(row_text[141], sizeof row_text[141], "%d", row_session_state_t.ap_hits[27].hits);
      snprintf(row_text[142], sizeof row_text[142], "%lld", (long long)row_session_state_t.ap_hits[28].pattern_id);
      snprintf(row_text[143], sizeof row_text[143], "%d", row_session_state_t.ap_hits[28].hits);
      snprintf(row_text[144], sizeof row_text[144], "%lld", (long long)row_session_state_t.ap_hits[29].pattern_id);
      snprintf(row_text[145], sizeof row_text[145], "%d", row_session_state_t.ap_hits[29].hits);
      snprintf(row_text[146], sizeof row_text[146], "%lld", (long long)row_session_state_t.ap_hits[30].pattern_id);
      snprintf(row_text[147], sizeof row_text[147], "%d", row_session_state_t.ap_hits[30].hits);
      snprintf(row_text[148], sizeof row_text[148], "%lld", (long long)row_session_state_t.ap_hits[31].pattern_id);
      snprintf(row_text[149], sizeof row_text[149], "%d", row_session_state_t.ap_hits[31].hits);
      snprintf(row_text[150], sizeof row_text[150], "%d", row_session_state_t.ap_hit_count);
      row_slots[0] = row_session_state_t.seen_paths[0];
      row_slots[1] = row_session_state_t.seen_paths[1];
      row_slots[2] = row_session_state_t.seen_paths[2];
      row_slots[3] = row_session_state_t.seen_paths[3];
      row_slots[4] = row_session_state_t.seen_paths[4];
      row_slots[5] = row_session_state_t.seen_paths[5];
      row_slots[6] = row_session_state_t.seen_paths[6];
      row_slots[7] = row_session_state_t.seen_paths[7];
      row_slots[8] = row_session_state_t.seen_paths[8];
      row_slots[9] = row_session_state_t.seen_paths[9];
      row_slots[10] = row_session_state_t.seen_paths[10];
      row_slots[11] = row_session_state_t.seen_paths[11];
      row_slots[12] = row_session_state_t.seen_paths[12];
      row_slots[13] = row_session_state_t.seen_paths[13];
      row_slots[14] = row_session_state_t.seen_paths[14];
      row_slots[15] = row_session_state_t.seen_paths[15];
      row_slots[16] = row_session_state_t.seen_paths[16];
      row_slots[17] = row_session_state_t.seen_paths[17];
      row_slots[18] = row_session_state_t.seen_paths[18];
      row_slots[19] = row_session_state_t.seen_paths[19];
      row_slots[20] = row_session_state_t.seen_paths[20];
      row_slots[21] = row_session_state_t.seen_paths[21];
      row_slots[22] = row_session_state_t.seen_paths[22];
      row_slots[23] = row_session_state_t.seen_paths[23];
      row_slots[24] = row_session_state_t.seen_paths[24];
      row_slots[25] = row_session_state_t.seen_paths[25];
      row_slots[26] = row_session_state_t.seen_paths[26];
      row_slots[27] = row_session_state_t.seen_paths[27];
      row_slots[28] = row_session_state_t.seen_paths[28];
      row_slots[29] = row_session_state_t.seen_paths[29];
      row_slots[30] = row_session_state_t.seen_paths[30];
      row_slots[31] = row_session_state_t.seen_paths[31];
      row_slots[32] = row_session_state_t.seen_paths[32];
      row_slots[33] = row_session_state_t.seen_paths[33];
      row_slots[34] = row_session_state_t.seen_paths[34];
      row_slots[35] = row_session_state_t.seen_paths[35];
      row_slots[36] = row_session_state_t.seen_paths[36];
      row_slots[37] = row_session_state_t.seen_paths[37];
      row_slots[38] = row_session_state_t.seen_paths[38];
      row_slots[39] = row_session_state_t.seen_paths[39];
      row_slots[40] = row_session_state_t.seen_paths[40];
      row_slots[41] = row_session_state_t.seen_paths[41];
      row_slots[42] = row_session_state_t.seen_paths[42];
      row_slots[43] = row_session_state_t.seen_paths[43];
      row_slots[44] = row_session_state_t.seen_paths[44];
      row_slots[45] = row_session_state_t.seen_paths[45];
      row_slots[46] = row_session_state_t.seen_paths[46];
      row_slots[47] = row_session_state_t.seen_paths[47];
      row_slots[48] = row_session_state_t.seen_paths[48];
      row_slots[49] = row_session_state_t.seen_paths[49];
      row_slots[50] = row_session_state_t.seen_paths[50];
      row_slots[51] = row_session_state_t.seen_paths[51];
      row_slots[52] = row_session_state_t.seen_paths[52];
      row_slots[53] = row_session_state_t.seen_paths[53];
      row_slots[54] = row_session_state_t.seen_paths[54];
      row_slots[55] = row_session_state_t.seen_paths[55];
      row_slots[56] = row_session_state_t.seen_paths[56];
      row_slots[57] = row_session_state_t.seen_paths[57];
      row_slots[58] = row_session_state_t.seen_paths[58];
      row_slots[59] = row_session_state_t.seen_paths[59];
      row_slots[60] = row_session_state_t.seen_paths[60];
      row_slots[61] = row_session_state_t.seen_paths[61];
      row_slots[62] = row_session_state_t.seen_paths[62];
      row_slots[63] = row_session_state_t.seen_paths[63];
      row_slots[64] = row_text[0];
      row_slots[65] = row_session_state_t.session_mode;
      row_slots[66] = row_session_state_t.guardrail_mode;
      row_slots[67] = row_text[1];
      row_slots[68] = row_text[2];
      row_slots[69] = row_text[3];
      row_slots[70] = row_session_state_t.worktrees[0].git_root;
      row_slots[71] = row_session_state_t.worktrees[0].worktree_path;
      row_slots[72] = row_session_state_t.worktrees[1].git_root;
      row_slots[73] = row_session_state_t.worktrees[1].worktree_path;
      row_slots[74] = row_session_state_t.worktrees[2].git_root;
      row_slots[75] = row_session_state_t.worktrees[2].worktree_path;
      row_slots[76] = row_session_state_t.worktrees[3].git_root;
      row_slots[77] = row_session_state_t.worktrees[3].worktree_path;
      row_slots[78] = row_session_state_t.worktrees[4].git_root;
      row_slots[79] = row_session_state_t.worktrees[4].worktree_path;
      row_slots[80] = row_session_state_t.worktrees[5].git_root;
      row_slots[81] = row_session_state_t.worktrees[5].worktree_path;
      row_slots[82] = row_session_state_t.worktrees[6].git_root;
      row_slots[83] = row_session_state_t.worktrees[6].worktree_path;
      row_slots[84] = row_session_state_t.worktrees[7].git_root;
      row_slots[85] = row_session_state_t.worktrees[7].worktree_path;
      row_slots[86] = row_session_state_t.worktrees[8].git_root;
      row_slots[87] = row_session_state_t.worktrees[8].worktree_path;
      row_slots[88] = row_session_state_t.worktrees[9].git_root;
      row_slots[89] = row_session_state_t.worktrees[9].worktree_path;
      row_slots[90] = row_session_state_t.worktrees[10].git_root;
      row_slots[91] = row_session_state_t.worktrees[10].worktree_path;
      row_slots[92] = row_session_state_t.worktrees[11].git_root;
      row_slots[93] = row_session_state_t.worktrees[11].worktree_path;
      row_slots[94] = row_session_state_t.worktrees[12].git_root;
      row_slots[95] = row_session_state_t.worktrees[12].worktree_path;
      row_slots[96] = row_session_state_t.worktrees[13].git_root;
      row_slots[97] = row_session_state_t.worktrees[13].worktree_path;
      row_slots[98] = row_session_state_t.worktrees[14].git_root;
      row_slots[99] = row_session_state_t.worktrees[14].worktree_path;
      row_slots[100] = row_session_state_t.worktrees[15].git_root;
      row_slots[101] = row_session_state_t.worktrees[15].worktree_path;
      row_slots[102] = row_text[4];
      row_slots[103] = row_text[5];
      row_slots[104] = row_text[6];
      row_slots[105] = row_text[7];
      row_slots[106] = row_text[8];
      row_slots[107] = row_text[9];
      row_slots[108] = row_text[10];
      row_slots[109] = row_session_state_t.tdd_mode;
      row_slots[110] = row_session_state_t.tdd_writes[0].stem;
      row_slots[111] = row_text[11];
      row_slots[112] = row_session_state_t.tdd_writes[1].stem;
      row_slots[113] = row_text[12];
      row_slots[114] = row_session_state_t.tdd_writes[2].stem;
      row_slots[115] = row_text[13];
      row_slots[116] = row_session_state_t.tdd_writes[3].stem;
      row_slots[117] = row_text[14];
      row_slots[118] = row_session_state_t.tdd_writes[4].stem;
      row_slots[119] = row_text[15];
      row_slots[120] = row_session_state_t.tdd_writes[5].stem;
      row_slots[121] = row_text[16];
      row_slots[122] = row_session_state_t.tdd_writes[6].stem;
      row_slots[123] = row_text[17];
      row_slots[124] = row_session_state_t.tdd_writes[7].stem;
      row_slots[125] = row_text[18];
      row_slots[126] = row_text[19];
      row_slots[127] = row_session_state_t.read_paths[0];
      row_slots[128] = row_session_state_t.read_paths[1];
      row_slots[129] = row_session_state_t.read_paths[2];
      row_slots[130] = row_session_state_t.read_paths[3];
      row_slots[131] = row_session_state_t.read_paths[4];
      row_slots[132] = row_session_state_t.read_paths[5];
      row_slots[133] = row_session_state_t.read_paths[6];
      row_slots[134] = row_session_state_t.read_paths[7];
      row_slots[135] = row_session_state_t.read_paths[8];
      row_slots[136] = row_session_state_t.read_paths[9];
      row_slots[137] = row_session_state_t.read_paths[10];
      row_slots[138] = row_session_state_t.read_paths[11];
      row_slots[139] = row_session_state_t.read_paths[12];
      row_slots[140] = row_session_state_t.read_paths[13];
      row_slots[141] = row_session_state_t.read_paths[14];
      row_slots[142] = row_session_state_t.read_paths[15];
      row_slots[143] = row_session_state_t.read_paths[16];
      row_slots[144] = row_session_state_t.read_paths[17];
      row_slots[145] = row_session_state_t.read_paths[18];
      row_slots[146] = row_session_state_t.read_paths[19];
      row_slots[147] = row_session_state_t.read_paths[20];
      row_slots[148] = row_session_state_t.read_paths[21];
      row_slots[149] = row_session_state_t.read_paths[22];
      row_slots[150] = row_session_state_t.read_paths[23];
      row_slots[151] = row_session_state_t.read_paths[24];
      row_slots[152] = row_session_state_t.read_paths[25];
      row_slots[153] = row_session_state_t.read_paths[26];
      row_slots[154] = row_session_state_t.read_paths[27];
      row_slots[155] = row_session_state_t.read_paths[28];
      row_slots[156] = row_session_state_t.read_paths[29];
      row_slots[157] = row_session_state_t.read_paths[30];
      row_slots[158] = row_session_state_t.read_paths[31];
      row_slots[159] = row_session_state_t.read_paths[32];
      row_slots[160] = row_session_state_t.read_paths[33];
      row_slots[161] = row_session_state_t.read_paths[34];
      row_slots[162] = row_session_state_t.read_paths[35];
      row_slots[163] = row_session_state_t.read_paths[36];
      row_slots[164] = row_session_state_t.read_paths[37];
      row_slots[165] = row_session_state_t.read_paths[38];
      row_slots[166] = row_session_state_t.read_paths[39];
      row_slots[167] = row_session_state_t.read_paths[40];
      row_slots[168] = row_session_state_t.read_paths[41];
      row_slots[169] = row_session_state_t.read_paths[42];
      row_slots[170] = row_session_state_t.read_paths[43];
      row_slots[171] = row_session_state_t.read_paths[44];
      row_slots[172] = row_session_state_t.read_paths[45];
      row_slots[173] = row_session_state_t.read_paths[46];
      row_slots[174] = row_session_state_t.read_paths[47];
      row_slots[175] = row_session_state_t.read_paths[48];
      row_slots[176] = row_session_state_t.read_paths[49];
      row_slots[177] = row_session_state_t.read_paths[50];
      row_slots[178] = row_session_state_t.read_paths[51];
      row_slots[179] = row_session_state_t.read_paths[52];
      row_slots[180] = row_session_state_t.read_paths[53];
      row_slots[181] = row_session_state_t.read_paths[54];
      row_slots[182] = row_session_state_t.read_paths[55];
      row_slots[183] = row_session_state_t.read_paths[56];
      row_slots[184] = row_session_state_t.read_paths[57];
      row_slots[185] = row_session_state_t.read_paths[58];
      row_slots[186] = row_session_state_t.read_paths[59];
      row_slots[187] = row_session_state_t.read_paths[60];
      row_slots[188] = row_session_state_t.read_paths[61];
      row_slots[189] = row_session_state_t.read_paths[62];
      row_slots[190] = row_session_state_t.read_paths[63];
      row_slots[191] = row_text[20];
      row_slots[192] = row_session_state_t.file_hashes[0].path;
      row_slots[193] = row_text[21];
      row_slots[194] = row_session_state_t.file_hashes[1].path;
      row_slots[195] = row_text[22];
      row_slots[196] = row_session_state_t.file_hashes[2].path;
      row_slots[197] = row_text[23];
      row_slots[198] = row_session_state_t.file_hashes[3].path;
      row_slots[199] = row_text[24];
      row_slots[200] = row_session_state_t.file_hashes[4].path;
      row_slots[201] = row_text[25];
      row_slots[202] = row_session_state_t.file_hashes[5].path;
      row_slots[203] = row_text[26];
      row_slots[204] = row_session_state_t.file_hashes[6].path;
      row_slots[205] = row_text[27];
      row_slots[206] = row_session_state_t.file_hashes[7].path;
      row_slots[207] = row_text[28];
      row_slots[208] = row_session_state_t.file_hashes[8].path;
      row_slots[209] = row_text[29];
      row_slots[210] = row_session_state_t.file_hashes[9].path;
      row_slots[211] = row_text[30];
      row_slots[212] = row_session_state_t.file_hashes[10].path;
      row_slots[213] = row_text[31];
      row_slots[214] = row_session_state_t.file_hashes[11].path;
      row_slots[215] = row_text[32];
      row_slots[216] = row_session_state_t.file_hashes[12].path;
      row_slots[217] = row_text[33];
      row_slots[218] = row_session_state_t.file_hashes[13].path;
      row_slots[219] = row_text[34];
      row_slots[220] = row_session_state_t.file_hashes[14].path;
      row_slots[221] = row_text[35];
      row_slots[222] = row_session_state_t.file_hashes[15].path;
      row_slots[223] = row_text[36];
      row_slots[224] = row_session_state_t.file_hashes[16].path;
      row_slots[225] = row_text[37];
      row_slots[226] = row_session_state_t.file_hashes[17].path;
      row_slots[227] = row_text[38];
      row_slots[228] = row_session_state_t.file_hashes[18].path;
      row_slots[229] = row_text[39];
      row_slots[230] = row_session_state_t.file_hashes[19].path;
      row_slots[231] = row_text[40];
      row_slots[232] = row_session_state_t.file_hashes[20].path;
      row_slots[233] = row_text[41];
      row_slots[234] = row_session_state_t.file_hashes[21].path;
      row_slots[235] = row_text[42];
      row_slots[236] = row_session_state_t.file_hashes[22].path;
      row_slots[237] = row_text[43];
      row_slots[238] = row_session_state_t.file_hashes[23].path;
      row_slots[239] = row_text[44];
      row_slots[240] = row_session_state_t.file_hashes[24].path;
      row_slots[241] = row_text[45];
      row_slots[242] = row_session_state_t.file_hashes[25].path;
      row_slots[243] = row_text[46];
      row_slots[244] = row_session_state_t.file_hashes[26].path;
      row_slots[245] = row_text[47];
      row_slots[246] = row_session_state_t.file_hashes[27].path;
      row_slots[247] = row_text[48];
      row_slots[248] = row_session_state_t.file_hashes[28].path;
      row_slots[249] = row_text[49];
      row_slots[250] = row_session_state_t.file_hashes[29].path;
      row_slots[251] = row_text[50];
      row_slots[252] = row_session_state_t.file_hashes[30].path;
      row_slots[253] = row_text[51];
      row_slots[254] = row_session_state_t.file_hashes[31].path;
      row_slots[255] = row_text[52];
      row_slots[256] = row_session_state_t.file_hashes[32].path;
      row_slots[257] = row_text[53];
      row_slots[258] = row_session_state_t.file_hashes[33].path;
      row_slots[259] = row_text[54];
      row_slots[260] = row_session_state_t.file_hashes[34].path;
      row_slots[261] = row_text[55];
      row_slots[262] = row_session_state_t.file_hashes[35].path;
      row_slots[263] = row_text[56];
      row_slots[264] = row_session_state_t.file_hashes[36].path;
      row_slots[265] = row_text[57];
      row_slots[266] = row_session_state_t.file_hashes[37].path;
      row_slots[267] = row_text[58];
      row_slots[268] = row_session_state_t.file_hashes[38].path;
      row_slots[269] = row_text[59];
      row_slots[270] = row_session_state_t.file_hashes[39].path;
      row_slots[271] = row_text[60];
      row_slots[272] = row_session_state_t.file_hashes[40].path;
      row_slots[273] = row_text[61];
      row_slots[274] = row_session_state_t.file_hashes[41].path;
      row_slots[275] = row_text[62];
      row_slots[276] = row_session_state_t.file_hashes[42].path;
      row_slots[277] = row_text[63];
      row_slots[278] = row_session_state_t.file_hashes[43].path;
      row_slots[279] = row_text[64];
      row_slots[280] = row_session_state_t.file_hashes[44].path;
      row_slots[281] = row_text[65];
      row_slots[282] = row_session_state_t.file_hashes[45].path;
      row_slots[283] = row_text[66];
      row_slots[284] = row_session_state_t.file_hashes[46].path;
      row_slots[285] = row_text[67];
      row_slots[286] = row_session_state_t.file_hashes[47].path;
      row_slots[287] = row_text[68];
      row_slots[288] = row_session_state_t.file_hashes[48].path;
      row_slots[289] = row_text[69];
      row_slots[290] = row_session_state_t.file_hashes[49].path;
      row_slots[291] = row_text[70];
      row_slots[292] = row_session_state_t.file_hashes[50].path;
      row_slots[293] = row_text[71];
      row_slots[294] = row_session_state_t.file_hashes[51].path;
      row_slots[295] = row_text[72];
      row_slots[296] = row_session_state_t.file_hashes[52].path;
      row_slots[297] = row_text[73];
      row_slots[298] = row_session_state_t.file_hashes[53].path;
      row_slots[299] = row_text[74];
      row_slots[300] = row_session_state_t.file_hashes[54].path;
      row_slots[301] = row_text[75];
      row_slots[302] = row_session_state_t.file_hashes[55].path;
      row_slots[303] = row_text[76];
      row_slots[304] = row_session_state_t.file_hashes[56].path;
      row_slots[305] = row_text[77];
      row_slots[306] = row_session_state_t.file_hashes[57].path;
      row_slots[307] = row_text[78];
      row_slots[308] = row_session_state_t.file_hashes[58].path;
      row_slots[309] = row_text[79];
      row_slots[310] = row_session_state_t.file_hashes[59].path;
      row_slots[311] = row_text[80];
      row_slots[312] = row_session_state_t.file_hashes[60].path;
      row_slots[313] = row_text[81];
      row_slots[314] = row_session_state_t.file_hashes[61].path;
      row_slots[315] = row_text[82];
      row_slots[316] = row_session_state_t.file_hashes[62].path;
      row_slots[317] = row_text[83];
      row_slots[318] = row_session_state_t.file_hashes[63].path;
      row_slots[319] = row_text[84];
      row_slots[320] = row_text[85];
      row_slots[321] = row_text[86];
      row_slots[322] = row_text[87];
      row_slots[323] = row_text[88];
      row_slots[324] = row_text[89];
      row_slots[325] = row_text[90];
      row_slots[326] = row_text[91];
      row_slots[327] = row_text[92];
      row_slots[328] = row_text[93];
      row_slots[329] = row_text[94];
      row_slots[330] = row_text[95];
      row_slots[331] = row_text[96];
      row_slots[332] = row_text[97];
      row_slots[333] = row_text[98];
      row_slots[334] = row_text[99];
      row_slots[335] = row_text[100];
      row_slots[336] = row_text[101];
      row_slots[337] = row_text[102];
      row_slots[338] = row_text[103];
      row_slots[339] = row_text[104];
      row_slots[340] = row_text[105];
      row_slots[341] = row_text[106];
      row_slots[342] = row_text[107];
      row_slots[343] = row_text[108];
      row_slots[344] = row_text[109];
      row_slots[345] = row_text[110];
      row_slots[346] = row_text[111];
      row_slots[347] = row_text[112];
      row_slots[348] = row_text[113];
      row_slots[349] = row_text[114];
      row_slots[350] = row_text[115];
      row_slots[351] = row_text[116];
      row_slots[352] = row_text[117];
      row_slots[353] = row_text[118];
      row_slots[354] = row_text[119];
      row_slots[355] = row_text[120];
      row_slots[356] = row_text[121];
      row_slots[357] = row_text[122];
      row_slots[358] = row_text[123];
      row_slots[359] = row_text[124];
      row_slots[360] = row_text[125];
      row_slots[361] = row_text[126];
      row_slots[362] = row_text[127];
      row_slots[363] = row_text[128];
      row_slots[364] = row_text[129];
      row_slots[365] = row_text[130];
      row_slots[366] = row_text[131];
      row_slots[367] = row_text[132];
      row_slots[368] = row_text[133];
      row_slots[369] = row_text[134];
      row_slots[370] = row_text[135];
      row_slots[371] = row_text[136];
      row_slots[372] = row_text[137];
      row_slots[373] = row_text[138];
      row_slots[374] = row_text[139];
      row_slots[375] = row_text[140];
      row_slots[376] = row_text[141];
      row_slots[377] = row_text[142];
      row_slots[378] = row_text[143];
      row_slots[379] = row_text[144];
      row_slots[380] = row_text[145];
      row_slots[381] = row_text[146];
      row_slots[382] = row_text[147];
      row_slots[383] = row_text[148];
      row_slots[384] = row_text[149];
      row_slots[385] = row_text[150];
      rows = row_slots;
      row_count = 386u;
      reads = 1;
      break;
   }
   case AIMEE_DB1_OP_SESSION_STATE_SAVE:
   {
      if (count != 387u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      session_state_t row;
      memset(&row, 0, sizeof row);
      snprintf(row.seen_paths[0], sizeof row.seen_paths[0], "%s", field[1]);
      snprintf(row.seen_paths[1], sizeof row.seen_paths[1], "%s", field[2]);
      snprintf(row.seen_paths[2], sizeof row.seen_paths[2], "%s", field[3]);
      snprintf(row.seen_paths[3], sizeof row.seen_paths[3], "%s", field[4]);
      snprintf(row.seen_paths[4], sizeof row.seen_paths[4], "%s", field[5]);
      snprintf(row.seen_paths[5], sizeof row.seen_paths[5], "%s", field[6]);
      snprintf(row.seen_paths[6], sizeof row.seen_paths[6], "%s", field[7]);
      snprintf(row.seen_paths[7], sizeof row.seen_paths[7], "%s", field[8]);
      snprintf(row.seen_paths[8], sizeof row.seen_paths[8], "%s", field[9]);
      snprintf(row.seen_paths[9], sizeof row.seen_paths[9], "%s", field[10]);
      snprintf(row.seen_paths[10], sizeof row.seen_paths[10], "%s", field[11]);
      snprintf(row.seen_paths[11], sizeof row.seen_paths[11], "%s", field[12]);
      snprintf(row.seen_paths[12], sizeof row.seen_paths[12], "%s", field[13]);
      snprintf(row.seen_paths[13], sizeof row.seen_paths[13], "%s", field[14]);
      snprintf(row.seen_paths[14], sizeof row.seen_paths[14], "%s", field[15]);
      snprintf(row.seen_paths[15], sizeof row.seen_paths[15], "%s", field[16]);
      snprintf(row.seen_paths[16], sizeof row.seen_paths[16], "%s", field[17]);
      snprintf(row.seen_paths[17], sizeof row.seen_paths[17], "%s", field[18]);
      snprintf(row.seen_paths[18], sizeof row.seen_paths[18], "%s", field[19]);
      snprintf(row.seen_paths[19], sizeof row.seen_paths[19], "%s", field[20]);
      snprintf(row.seen_paths[20], sizeof row.seen_paths[20], "%s", field[21]);
      snprintf(row.seen_paths[21], sizeof row.seen_paths[21], "%s", field[22]);
      snprintf(row.seen_paths[22], sizeof row.seen_paths[22], "%s", field[23]);
      snprintf(row.seen_paths[23], sizeof row.seen_paths[23], "%s", field[24]);
      snprintf(row.seen_paths[24], sizeof row.seen_paths[24], "%s", field[25]);
      snprintf(row.seen_paths[25], sizeof row.seen_paths[25], "%s", field[26]);
      snprintf(row.seen_paths[26], sizeof row.seen_paths[26], "%s", field[27]);
      snprintf(row.seen_paths[27], sizeof row.seen_paths[27], "%s", field[28]);
      snprintf(row.seen_paths[28], sizeof row.seen_paths[28], "%s", field[29]);
      snprintf(row.seen_paths[29], sizeof row.seen_paths[29], "%s", field[30]);
      snprintf(row.seen_paths[30], sizeof row.seen_paths[30], "%s", field[31]);
      snprintf(row.seen_paths[31], sizeof row.seen_paths[31], "%s", field[32]);
      snprintf(row.seen_paths[32], sizeof row.seen_paths[32], "%s", field[33]);
      snprintf(row.seen_paths[33], sizeof row.seen_paths[33], "%s", field[34]);
      snprintf(row.seen_paths[34], sizeof row.seen_paths[34], "%s", field[35]);
      snprintf(row.seen_paths[35], sizeof row.seen_paths[35], "%s", field[36]);
      snprintf(row.seen_paths[36], sizeof row.seen_paths[36], "%s", field[37]);
      snprintf(row.seen_paths[37], sizeof row.seen_paths[37], "%s", field[38]);
      snprintf(row.seen_paths[38], sizeof row.seen_paths[38], "%s", field[39]);
      snprintf(row.seen_paths[39], sizeof row.seen_paths[39], "%s", field[40]);
      snprintf(row.seen_paths[40], sizeof row.seen_paths[40], "%s", field[41]);
      snprintf(row.seen_paths[41], sizeof row.seen_paths[41], "%s", field[42]);
      snprintf(row.seen_paths[42], sizeof row.seen_paths[42], "%s", field[43]);
      snprintf(row.seen_paths[43], sizeof row.seen_paths[43], "%s", field[44]);
      snprintf(row.seen_paths[44], sizeof row.seen_paths[44], "%s", field[45]);
      snprintf(row.seen_paths[45], sizeof row.seen_paths[45], "%s", field[46]);
      snprintf(row.seen_paths[46], sizeof row.seen_paths[46], "%s", field[47]);
      snprintf(row.seen_paths[47], sizeof row.seen_paths[47], "%s", field[48]);
      snprintf(row.seen_paths[48], sizeof row.seen_paths[48], "%s", field[49]);
      snprintf(row.seen_paths[49], sizeof row.seen_paths[49], "%s", field[50]);
      snprintf(row.seen_paths[50], sizeof row.seen_paths[50], "%s", field[51]);
      snprintf(row.seen_paths[51], sizeof row.seen_paths[51], "%s", field[52]);
      snprintf(row.seen_paths[52], sizeof row.seen_paths[52], "%s", field[53]);
      snprintf(row.seen_paths[53], sizeof row.seen_paths[53], "%s", field[54]);
      snprintf(row.seen_paths[54], sizeof row.seen_paths[54], "%s", field[55]);
      snprintf(row.seen_paths[55], sizeof row.seen_paths[55], "%s", field[56]);
      snprintf(row.seen_paths[56], sizeof row.seen_paths[56], "%s", field[57]);
      snprintf(row.seen_paths[57], sizeof row.seen_paths[57], "%s", field[58]);
      snprintf(row.seen_paths[58], sizeof row.seen_paths[58], "%s", field[59]);
      snprintf(row.seen_paths[59], sizeof row.seen_paths[59], "%s", field[60]);
      snprintf(row.seen_paths[60], sizeof row.seen_paths[60], "%s", field[61]);
      snprintf(row.seen_paths[61], sizeof row.seen_paths[61], "%s", field[62]);
      snprintf(row.seen_paths[62], sizeof row.seen_paths[62], "%s", field[63]);
      snprintf(row.seen_paths[63], sizeof row.seen_paths[63], "%s", field[64]);
      int member_65 = 0;
      if (parse_int(field[65], &member_65) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.seen_count = member_65;
      snprintf(row.session_mode, sizeof row.session_mode, "%s", field[66]);
      snprintf(row.guardrail_mode, sizeof row.guardrail_mode, "%s", field[67]);
      int64_t member_68 = 0;
      if (parse_int64(field[68], &member_68) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.active_task_id = member_68;
      int member_69 = 0;
      if (parse_int(field[69], &member_69) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.hook_call_count = member_69;
      int member_70 = 0;
      if (parse_int(field[70], &member_70) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.dirty = member_70;
      snprintf(row.worktrees[0].git_root, sizeof row.worktrees[0].git_root, "%s", field[71]);
      snprintf(row.worktrees[0].worktree_path, sizeof row.worktrees[0].worktree_path, "%s", field[72]);
      snprintf(row.worktrees[1].git_root, sizeof row.worktrees[1].git_root, "%s", field[73]);
      snprintf(row.worktrees[1].worktree_path, sizeof row.worktrees[1].worktree_path, "%s", field[74]);
      snprintf(row.worktrees[2].git_root, sizeof row.worktrees[2].git_root, "%s", field[75]);
      snprintf(row.worktrees[2].worktree_path, sizeof row.worktrees[2].worktree_path, "%s", field[76]);
      snprintf(row.worktrees[3].git_root, sizeof row.worktrees[3].git_root, "%s", field[77]);
      snprintf(row.worktrees[3].worktree_path, sizeof row.worktrees[3].worktree_path, "%s", field[78]);
      snprintf(row.worktrees[4].git_root, sizeof row.worktrees[4].git_root, "%s", field[79]);
      snprintf(row.worktrees[4].worktree_path, sizeof row.worktrees[4].worktree_path, "%s", field[80]);
      snprintf(row.worktrees[5].git_root, sizeof row.worktrees[5].git_root, "%s", field[81]);
      snprintf(row.worktrees[5].worktree_path, sizeof row.worktrees[5].worktree_path, "%s", field[82]);
      snprintf(row.worktrees[6].git_root, sizeof row.worktrees[6].git_root, "%s", field[83]);
      snprintf(row.worktrees[6].worktree_path, sizeof row.worktrees[6].worktree_path, "%s", field[84]);
      snprintf(row.worktrees[7].git_root, sizeof row.worktrees[7].git_root, "%s", field[85]);
      snprintf(row.worktrees[7].worktree_path, sizeof row.worktrees[7].worktree_path, "%s", field[86]);
      snprintf(row.worktrees[8].git_root, sizeof row.worktrees[8].git_root, "%s", field[87]);
      snprintf(row.worktrees[8].worktree_path, sizeof row.worktrees[8].worktree_path, "%s", field[88]);
      snprintf(row.worktrees[9].git_root, sizeof row.worktrees[9].git_root, "%s", field[89]);
      snprintf(row.worktrees[9].worktree_path, sizeof row.worktrees[9].worktree_path, "%s", field[90]);
      snprintf(row.worktrees[10].git_root, sizeof row.worktrees[10].git_root, "%s", field[91]);
      snprintf(row.worktrees[10].worktree_path, sizeof row.worktrees[10].worktree_path, "%s", field[92]);
      snprintf(row.worktrees[11].git_root, sizeof row.worktrees[11].git_root, "%s", field[93]);
      snprintf(row.worktrees[11].worktree_path, sizeof row.worktrees[11].worktree_path, "%s", field[94]);
      snprintf(row.worktrees[12].git_root, sizeof row.worktrees[12].git_root, "%s", field[95]);
      snprintf(row.worktrees[12].worktree_path, sizeof row.worktrees[12].worktree_path, "%s", field[96]);
      snprintf(row.worktrees[13].git_root, sizeof row.worktrees[13].git_root, "%s", field[97]);
      snprintf(row.worktrees[13].worktree_path, sizeof row.worktrees[13].worktree_path, "%s", field[98]);
      snprintf(row.worktrees[14].git_root, sizeof row.worktrees[14].git_root, "%s", field[99]);
      snprintf(row.worktrees[14].worktree_path, sizeof row.worktrees[14].worktree_path, "%s", field[100]);
      snprintf(row.worktrees[15].git_root, sizeof row.worktrees[15].git_root, "%s", field[101]);
      snprintf(row.worktrees[15].worktree_path, sizeof row.worktrees[15].worktree_path, "%s", field[102]);
      int member_103 = 0;
      if (parse_int(field[103], &member_103) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.worktree_count = member_103;
      int member_104 = 0;
      if (parse_int(field[104], &member_104) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.is_delegate = member_104;
      int member_105 = 0;
      if (parse_int(field[105], &member_105) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.orch_direct_edits = member_105;
      int member_106 = 0;
      if (parse_int(field[106], &member_106) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.orch_nudge_sent = member_106;
      int member_107 = 0;
      if (parse_int(field[107], &member_107) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.skill_find_symbols_advisory_sent = member_107;
      int member_108 = 0;
      if (parse_int(field[108], &member_108) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.skill_condition_waiting_advisory_sent = member_108;
      int member_109 = 0;
      if (parse_int(field[109], &member_109) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.skill_tdd_advisory_sent = member_109;
      snprintf(row.tdd_mode, sizeof row.tdd_mode, "%s", field[110]);
      snprintf(row.tdd_writes[0].stem, sizeof row.tdd_writes[0].stem, "%s", field[111]);
      int member_112 = 0;
      if (parse_int(field[112], &member_112) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.tdd_writes[0].is_test = member_112;
      snprintf(row.tdd_writes[1].stem, sizeof row.tdd_writes[1].stem, "%s", field[113]);
      int member_114 = 0;
      if (parse_int(field[114], &member_114) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.tdd_writes[1].is_test = member_114;
      snprintf(row.tdd_writes[2].stem, sizeof row.tdd_writes[2].stem, "%s", field[115]);
      int member_116 = 0;
      if (parse_int(field[116], &member_116) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.tdd_writes[2].is_test = member_116;
      snprintf(row.tdd_writes[3].stem, sizeof row.tdd_writes[3].stem, "%s", field[117]);
      int member_118 = 0;
      if (parse_int(field[118], &member_118) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.tdd_writes[3].is_test = member_118;
      snprintf(row.tdd_writes[4].stem, sizeof row.tdd_writes[4].stem, "%s", field[119]);
      int member_120 = 0;
      if (parse_int(field[120], &member_120) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.tdd_writes[4].is_test = member_120;
      snprintf(row.tdd_writes[5].stem, sizeof row.tdd_writes[5].stem, "%s", field[121]);
      int member_122 = 0;
      if (parse_int(field[122], &member_122) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.tdd_writes[5].is_test = member_122;
      snprintf(row.tdd_writes[6].stem, sizeof row.tdd_writes[6].stem, "%s", field[123]);
      int member_124 = 0;
      if (parse_int(field[124], &member_124) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.tdd_writes[6].is_test = member_124;
      snprintf(row.tdd_writes[7].stem, sizeof row.tdd_writes[7].stem, "%s", field[125]);
      int member_126 = 0;
      if (parse_int(field[126], &member_126) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.tdd_writes[7].is_test = member_126;
      int member_127 = 0;
      if (parse_int(field[127], &member_127) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.tdd_write_count = member_127;
      snprintf(row.read_paths[0], sizeof row.read_paths[0], "%s", field[128]);
      snprintf(row.read_paths[1], sizeof row.read_paths[1], "%s", field[129]);
      snprintf(row.read_paths[2], sizeof row.read_paths[2], "%s", field[130]);
      snprintf(row.read_paths[3], sizeof row.read_paths[3], "%s", field[131]);
      snprintf(row.read_paths[4], sizeof row.read_paths[4], "%s", field[132]);
      snprintf(row.read_paths[5], sizeof row.read_paths[5], "%s", field[133]);
      snprintf(row.read_paths[6], sizeof row.read_paths[6], "%s", field[134]);
      snprintf(row.read_paths[7], sizeof row.read_paths[7], "%s", field[135]);
      snprintf(row.read_paths[8], sizeof row.read_paths[8], "%s", field[136]);
      snprintf(row.read_paths[9], sizeof row.read_paths[9], "%s", field[137]);
      snprintf(row.read_paths[10], sizeof row.read_paths[10], "%s", field[138]);
      snprintf(row.read_paths[11], sizeof row.read_paths[11], "%s", field[139]);
      snprintf(row.read_paths[12], sizeof row.read_paths[12], "%s", field[140]);
      snprintf(row.read_paths[13], sizeof row.read_paths[13], "%s", field[141]);
      snprintf(row.read_paths[14], sizeof row.read_paths[14], "%s", field[142]);
      snprintf(row.read_paths[15], sizeof row.read_paths[15], "%s", field[143]);
      snprintf(row.read_paths[16], sizeof row.read_paths[16], "%s", field[144]);
      snprintf(row.read_paths[17], sizeof row.read_paths[17], "%s", field[145]);
      snprintf(row.read_paths[18], sizeof row.read_paths[18], "%s", field[146]);
      snprintf(row.read_paths[19], sizeof row.read_paths[19], "%s", field[147]);
      snprintf(row.read_paths[20], sizeof row.read_paths[20], "%s", field[148]);
      snprintf(row.read_paths[21], sizeof row.read_paths[21], "%s", field[149]);
      snprintf(row.read_paths[22], sizeof row.read_paths[22], "%s", field[150]);
      snprintf(row.read_paths[23], sizeof row.read_paths[23], "%s", field[151]);
      snprintf(row.read_paths[24], sizeof row.read_paths[24], "%s", field[152]);
      snprintf(row.read_paths[25], sizeof row.read_paths[25], "%s", field[153]);
      snprintf(row.read_paths[26], sizeof row.read_paths[26], "%s", field[154]);
      snprintf(row.read_paths[27], sizeof row.read_paths[27], "%s", field[155]);
      snprintf(row.read_paths[28], sizeof row.read_paths[28], "%s", field[156]);
      snprintf(row.read_paths[29], sizeof row.read_paths[29], "%s", field[157]);
      snprintf(row.read_paths[30], sizeof row.read_paths[30], "%s", field[158]);
      snprintf(row.read_paths[31], sizeof row.read_paths[31], "%s", field[159]);
      snprintf(row.read_paths[32], sizeof row.read_paths[32], "%s", field[160]);
      snprintf(row.read_paths[33], sizeof row.read_paths[33], "%s", field[161]);
      snprintf(row.read_paths[34], sizeof row.read_paths[34], "%s", field[162]);
      snprintf(row.read_paths[35], sizeof row.read_paths[35], "%s", field[163]);
      snprintf(row.read_paths[36], sizeof row.read_paths[36], "%s", field[164]);
      snprintf(row.read_paths[37], sizeof row.read_paths[37], "%s", field[165]);
      snprintf(row.read_paths[38], sizeof row.read_paths[38], "%s", field[166]);
      snprintf(row.read_paths[39], sizeof row.read_paths[39], "%s", field[167]);
      snprintf(row.read_paths[40], sizeof row.read_paths[40], "%s", field[168]);
      snprintf(row.read_paths[41], sizeof row.read_paths[41], "%s", field[169]);
      snprintf(row.read_paths[42], sizeof row.read_paths[42], "%s", field[170]);
      snprintf(row.read_paths[43], sizeof row.read_paths[43], "%s", field[171]);
      snprintf(row.read_paths[44], sizeof row.read_paths[44], "%s", field[172]);
      snprintf(row.read_paths[45], sizeof row.read_paths[45], "%s", field[173]);
      snprintf(row.read_paths[46], sizeof row.read_paths[46], "%s", field[174]);
      snprintf(row.read_paths[47], sizeof row.read_paths[47], "%s", field[175]);
      snprintf(row.read_paths[48], sizeof row.read_paths[48], "%s", field[176]);
      snprintf(row.read_paths[49], sizeof row.read_paths[49], "%s", field[177]);
      snprintf(row.read_paths[50], sizeof row.read_paths[50], "%s", field[178]);
      snprintf(row.read_paths[51], sizeof row.read_paths[51], "%s", field[179]);
      snprintf(row.read_paths[52], sizeof row.read_paths[52], "%s", field[180]);
      snprintf(row.read_paths[53], sizeof row.read_paths[53], "%s", field[181]);
      snprintf(row.read_paths[54], sizeof row.read_paths[54], "%s", field[182]);
      snprintf(row.read_paths[55], sizeof row.read_paths[55], "%s", field[183]);
      snprintf(row.read_paths[56], sizeof row.read_paths[56], "%s", field[184]);
      snprintf(row.read_paths[57], sizeof row.read_paths[57], "%s", field[185]);
      snprintf(row.read_paths[58], sizeof row.read_paths[58], "%s", field[186]);
      snprintf(row.read_paths[59], sizeof row.read_paths[59], "%s", field[187]);
      snprintf(row.read_paths[60], sizeof row.read_paths[60], "%s", field[188]);
      snprintf(row.read_paths[61], sizeof row.read_paths[61], "%s", field[189]);
      snprintf(row.read_paths[62], sizeof row.read_paths[62], "%s", field[190]);
      snprintf(row.read_paths[63], sizeof row.read_paths[63], "%s", field[191]);
      int member_192 = 0;
      if (parse_int(field[192], &member_192) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.read_path_count = member_192;
      snprintf(row.file_hashes[0].path, sizeof row.file_hashes[0].path, "%s", field[193]);
      uint64_t member_194 = 0;
      if (parse_uint64(field[194], &member_194) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[0].content_hash = member_194;
      snprintf(row.file_hashes[1].path, sizeof row.file_hashes[1].path, "%s", field[195]);
      uint64_t member_196 = 0;
      if (parse_uint64(field[196], &member_196) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[1].content_hash = member_196;
      snprintf(row.file_hashes[2].path, sizeof row.file_hashes[2].path, "%s", field[197]);
      uint64_t member_198 = 0;
      if (parse_uint64(field[198], &member_198) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[2].content_hash = member_198;
      snprintf(row.file_hashes[3].path, sizeof row.file_hashes[3].path, "%s", field[199]);
      uint64_t member_200 = 0;
      if (parse_uint64(field[200], &member_200) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[3].content_hash = member_200;
      snprintf(row.file_hashes[4].path, sizeof row.file_hashes[4].path, "%s", field[201]);
      uint64_t member_202 = 0;
      if (parse_uint64(field[202], &member_202) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[4].content_hash = member_202;
      snprintf(row.file_hashes[5].path, sizeof row.file_hashes[5].path, "%s", field[203]);
      uint64_t member_204 = 0;
      if (parse_uint64(field[204], &member_204) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[5].content_hash = member_204;
      snprintf(row.file_hashes[6].path, sizeof row.file_hashes[6].path, "%s", field[205]);
      uint64_t member_206 = 0;
      if (parse_uint64(field[206], &member_206) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[6].content_hash = member_206;
      snprintf(row.file_hashes[7].path, sizeof row.file_hashes[7].path, "%s", field[207]);
      uint64_t member_208 = 0;
      if (parse_uint64(field[208], &member_208) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[7].content_hash = member_208;
      snprintf(row.file_hashes[8].path, sizeof row.file_hashes[8].path, "%s", field[209]);
      uint64_t member_210 = 0;
      if (parse_uint64(field[210], &member_210) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[8].content_hash = member_210;
      snprintf(row.file_hashes[9].path, sizeof row.file_hashes[9].path, "%s", field[211]);
      uint64_t member_212 = 0;
      if (parse_uint64(field[212], &member_212) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[9].content_hash = member_212;
      snprintf(row.file_hashes[10].path, sizeof row.file_hashes[10].path, "%s", field[213]);
      uint64_t member_214 = 0;
      if (parse_uint64(field[214], &member_214) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[10].content_hash = member_214;
      snprintf(row.file_hashes[11].path, sizeof row.file_hashes[11].path, "%s", field[215]);
      uint64_t member_216 = 0;
      if (parse_uint64(field[216], &member_216) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[11].content_hash = member_216;
      snprintf(row.file_hashes[12].path, sizeof row.file_hashes[12].path, "%s", field[217]);
      uint64_t member_218 = 0;
      if (parse_uint64(field[218], &member_218) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[12].content_hash = member_218;
      snprintf(row.file_hashes[13].path, sizeof row.file_hashes[13].path, "%s", field[219]);
      uint64_t member_220 = 0;
      if (parse_uint64(field[220], &member_220) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[13].content_hash = member_220;
      snprintf(row.file_hashes[14].path, sizeof row.file_hashes[14].path, "%s", field[221]);
      uint64_t member_222 = 0;
      if (parse_uint64(field[222], &member_222) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[14].content_hash = member_222;
      snprintf(row.file_hashes[15].path, sizeof row.file_hashes[15].path, "%s", field[223]);
      uint64_t member_224 = 0;
      if (parse_uint64(field[224], &member_224) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[15].content_hash = member_224;
      snprintf(row.file_hashes[16].path, sizeof row.file_hashes[16].path, "%s", field[225]);
      uint64_t member_226 = 0;
      if (parse_uint64(field[226], &member_226) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[16].content_hash = member_226;
      snprintf(row.file_hashes[17].path, sizeof row.file_hashes[17].path, "%s", field[227]);
      uint64_t member_228 = 0;
      if (parse_uint64(field[228], &member_228) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[17].content_hash = member_228;
      snprintf(row.file_hashes[18].path, sizeof row.file_hashes[18].path, "%s", field[229]);
      uint64_t member_230 = 0;
      if (parse_uint64(field[230], &member_230) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[18].content_hash = member_230;
      snprintf(row.file_hashes[19].path, sizeof row.file_hashes[19].path, "%s", field[231]);
      uint64_t member_232 = 0;
      if (parse_uint64(field[232], &member_232) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[19].content_hash = member_232;
      snprintf(row.file_hashes[20].path, sizeof row.file_hashes[20].path, "%s", field[233]);
      uint64_t member_234 = 0;
      if (parse_uint64(field[234], &member_234) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[20].content_hash = member_234;
      snprintf(row.file_hashes[21].path, sizeof row.file_hashes[21].path, "%s", field[235]);
      uint64_t member_236 = 0;
      if (parse_uint64(field[236], &member_236) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[21].content_hash = member_236;
      snprintf(row.file_hashes[22].path, sizeof row.file_hashes[22].path, "%s", field[237]);
      uint64_t member_238 = 0;
      if (parse_uint64(field[238], &member_238) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[22].content_hash = member_238;
      snprintf(row.file_hashes[23].path, sizeof row.file_hashes[23].path, "%s", field[239]);
      uint64_t member_240 = 0;
      if (parse_uint64(field[240], &member_240) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[23].content_hash = member_240;
      snprintf(row.file_hashes[24].path, sizeof row.file_hashes[24].path, "%s", field[241]);
      uint64_t member_242 = 0;
      if (parse_uint64(field[242], &member_242) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[24].content_hash = member_242;
      snprintf(row.file_hashes[25].path, sizeof row.file_hashes[25].path, "%s", field[243]);
      uint64_t member_244 = 0;
      if (parse_uint64(field[244], &member_244) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[25].content_hash = member_244;
      snprintf(row.file_hashes[26].path, sizeof row.file_hashes[26].path, "%s", field[245]);
      uint64_t member_246 = 0;
      if (parse_uint64(field[246], &member_246) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[26].content_hash = member_246;
      snprintf(row.file_hashes[27].path, sizeof row.file_hashes[27].path, "%s", field[247]);
      uint64_t member_248 = 0;
      if (parse_uint64(field[248], &member_248) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[27].content_hash = member_248;
      snprintf(row.file_hashes[28].path, sizeof row.file_hashes[28].path, "%s", field[249]);
      uint64_t member_250 = 0;
      if (parse_uint64(field[250], &member_250) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[28].content_hash = member_250;
      snprintf(row.file_hashes[29].path, sizeof row.file_hashes[29].path, "%s", field[251]);
      uint64_t member_252 = 0;
      if (parse_uint64(field[252], &member_252) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[29].content_hash = member_252;
      snprintf(row.file_hashes[30].path, sizeof row.file_hashes[30].path, "%s", field[253]);
      uint64_t member_254 = 0;
      if (parse_uint64(field[254], &member_254) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[30].content_hash = member_254;
      snprintf(row.file_hashes[31].path, sizeof row.file_hashes[31].path, "%s", field[255]);
      uint64_t member_256 = 0;
      if (parse_uint64(field[256], &member_256) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[31].content_hash = member_256;
      snprintf(row.file_hashes[32].path, sizeof row.file_hashes[32].path, "%s", field[257]);
      uint64_t member_258 = 0;
      if (parse_uint64(field[258], &member_258) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[32].content_hash = member_258;
      snprintf(row.file_hashes[33].path, sizeof row.file_hashes[33].path, "%s", field[259]);
      uint64_t member_260 = 0;
      if (parse_uint64(field[260], &member_260) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[33].content_hash = member_260;
      snprintf(row.file_hashes[34].path, sizeof row.file_hashes[34].path, "%s", field[261]);
      uint64_t member_262 = 0;
      if (parse_uint64(field[262], &member_262) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[34].content_hash = member_262;
      snprintf(row.file_hashes[35].path, sizeof row.file_hashes[35].path, "%s", field[263]);
      uint64_t member_264 = 0;
      if (parse_uint64(field[264], &member_264) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[35].content_hash = member_264;
      snprintf(row.file_hashes[36].path, sizeof row.file_hashes[36].path, "%s", field[265]);
      uint64_t member_266 = 0;
      if (parse_uint64(field[266], &member_266) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[36].content_hash = member_266;
      snprintf(row.file_hashes[37].path, sizeof row.file_hashes[37].path, "%s", field[267]);
      uint64_t member_268 = 0;
      if (parse_uint64(field[268], &member_268) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[37].content_hash = member_268;
      snprintf(row.file_hashes[38].path, sizeof row.file_hashes[38].path, "%s", field[269]);
      uint64_t member_270 = 0;
      if (parse_uint64(field[270], &member_270) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[38].content_hash = member_270;
      snprintf(row.file_hashes[39].path, sizeof row.file_hashes[39].path, "%s", field[271]);
      uint64_t member_272 = 0;
      if (parse_uint64(field[272], &member_272) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[39].content_hash = member_272;
      snprintf(row.file_hashes[40].path, sizeof row.file_hashes[40].path, "%s", field[273]);
      uint64_t member_274 = 0;
      if (parse_uint64(field[274], &member_274) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[40].content_hash = member_274;
      snprintf(row.file_hashes[41].path, sizeof row.file_hashes[41].path, "%s", field[275]);
      uint64_t member_276 = 0;
      if (parse_uint64(field[276], &member_276) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[41].content_hash = member_276;
      snprintf(row.file_hashes[42].path, sizeof row.file_hashes[42].path, "%s", field[277]);
      uint64_t member_278 = 0;
      if (parse_uint64(field[278], &member_278) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[42].content_hash = member_278;
      snprintf(row.file_hashes[43].path, sizeof row.file_hashes[43].path, "%s", field[279]);
      uint64_t member_280 = 0;
      if (parse_uint64(field[280], &member_280) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[43].content_hash = member_280;
      snprintf(row.file_hashes[44].path, sizeof row.file_hashes[44].path, "%s", field[281]);
      uint64_t member_282 = 0;
      if (parse_uint64(field[282], &member_282) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[44].content_hash = member_282;
      snprintf(row.file_hashes[45].path, sizeof row.file_hashes[45].path, "%s", field[283]);
      uint64_t member_284 = 0;
      if (parse_uint64(field[284], &member_284) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[45].content_hash = member_284;
      snprintf(row.file_hashes[46].path, sizeof row.file_hashes[46].path, "%s", field[285]);
      uint64_t member_286 = 0;
      if (parse_uint64(field[286], &member_286) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[46].content_hash = member_286;
      snprintf(row.file_hashes[47].path, sizeof row.file_hashes[47].path, "%s", field[287]);
      uint64_t member_288 = 0;
      if (parse_uint64(field[288], &member_288) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[47].content_hash = member_288;
      snprintf(row.file_hashes[48].path, sizeof row.file_hashes[48].path, "%s", field[289]);
      uint64_t member_290 = 0;
      if (parse_uint64(field[290], &member_290) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[48].content_hash = member_290;
      snprintf(row.file_hashes[49].path, sizeof row.file_hashes[49].path, "%s", field[291]);
      uint64_t member_292 = 0;
      if (parse_uint64(field[292], &member_292) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[49].content_hash = member_292;
      snprintf(row.file_hashes[50].path, sizeof row.file_hashes[50].path, "%s", field[293]);
      uint64_t member_294 = 0;
      if (parse_uint64(field[294], &member_294) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[50].content_hash = member_294;
      snprintf(row.file_hashes[51].path, sizeof row.file_hashes[51].path, "%s", field[295]);
      uint64_t member_296 = 0;
      if (parse_uint64(field[296], &member_296) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[51].content_hash = member_296;
      snprintf(row.file_hashes[52].path, sizeof row.file_hashes[52].path, "%s", field[297]);
      uint64_t member_298 = 0;
      if (parse_uint64(field[298], &member_298) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[52].content_hash = member_298;
      snprintf(row.file_hashes[53].path, sizeof row.file_hashes[53].path, "%s", field[299]);
      uint64_t member_300 = 0;
      if (parse_uint64(field[300], &member_300) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[53].content_hash = member_300;
      snprintf(row.file_hashes[54].path, sizeof row.file_hashes[54].path, "%s", field[301]);
      uint64_t member_302 = 0;
      if (parse_uint64(field[302], &member_302) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[54].content_hash = member_302;
      snprintf(row.file_hashes[55].path, sizeof row.file_hashes[55].path, "%s", field[303]);
      uint64_t member_304 = 0;
      if (parse_uint64(field[304], &member_304) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[55].content_hash = member_304;
      snprintf(row.file_hashes[56].path, sizeof row.file_hashes[56].path, "%s", field[305]);
      uint64_t member_306 = 0;
      if (parse_uint64(field[306], &member_306) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[56].content_hash = member_306;
      snprintf(row.file_hashes[57].path, sizeof row.file_hashes[57].path, "%s", field[307]);
      uint64_t member_308 = 0;
      if (parse_uint64(field[308], &member_308) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[57].content_hash = member_308;
      snprintf(row.file_hashes[58].path, sizeof row.file_hashes[58].path, "%s", field[309]);
      uint64_t member_310 = 0;
      if (parse_uint64(field[310], &member_310) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[58].content_hash = member_310;
      snprintf(row.file_hashes[59].path, sizeof row.file_hashes[59].path, "%s", field[311]);
      uint64_t member_312 = 0;
      if (parse_uint64(field[312], &member_312) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[59].content_hash = member_312;
      snprintf(row.file_hashes[60].path, sizeof row.file_hashes[60].path, "%s", field[313]);
      uint64_t member_314 = 0;
      if (parse_uint64(field[314], &member_314) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[60].content_hash = member_314;
      snprintf(row.file_hashes[61].path, sizeof row.file_hashes[61].path, "%s", field[315]);
      uint64_t member_316 = 0;
      if (parse_uint64(field[316], &member_316) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[61].content_hash = member_316;
      snprintf(row.file_hashes[62].path, sizeof row.file_hashes[62].path, "%s", field[317]);
      uint64_t member_318 = 0;
      if (parse_uint64(field[318], &member_318) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[62].content_hash = member_318;
      snprintf(row.file_hashes[63].path, sizeof row.file_hashes[63].path, "%s", field[319]);
      uint64_t member_320 = 0;
      if (parse_uint64(field[320], &member_320) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hashes[63].content_hash = member_320;
      int member_321 = 0;
      if (parse_int(field[321], &member_321) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.file_hash_count = member_321;
      int64_t member_322 = 0;
      if (parse_int64(field[322], &member_322) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[0].pattern_id = member_322;
      int member_323 = 0;
      if (parse_int(field[323], &member_323) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[0].hits = member_323;
      int64_t member_324 = 0;
      if (parse_int64(field[324], &member_324) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[1].pattern_id = member_324;
      int member_325 = 0;
      if (parse_int(field[325], &member_325) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[1].hits = member_325;
      int64_t member_326 = 0;
      if (parse_int64(field[326], &member_326) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[2].pattern_id = member_326;
      int member_327 = 0;
      if (parse_int(field[327], &member_327) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[2].hits = member_327;
      int64_t member_328 = 0;
      if (parse_int64(field[328], &member_328) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[3].pattern_id = member_328;
      int member_329 = 0;
      if (parse_int(field[329], &member_329) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[3].hits = member_329;
      int64_t member_330 = 0;
      if (parse_int64(field[330], &member_330) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[4].pattern_id = member_330;
      int member_331 = 0;
      if (parse_int(field[331], &member_331) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[4].hits = member_331;
      int64_t member_332 = 0;
      if (parse_int64(field[332], &member_332) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[5].pattern_id = member_332;
      int member_333 = 0;
      if (parse_int(field[333], &member_333) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[5].hits = member_333;
      int64_t member_334 = 0;
      if (parse_int64(field[334], &member_334) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[6].pattern_id = member_334;
      int member_335 = 0;
      if (parse_int(field[335], &member_335) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[6].hits = member_335;
      int64_t member_336 = 0;
      if (parse_int64(field[336], &member_336) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[7].pattern_id = member_336;
      int member_337 = 0;
      if (parse_int(field[337], &member_337) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[7].hits = member_337;
      int64_t member_338 = 0;
      if (parse_int64(field[338], &member_338) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[8].pattern_id = member_338;
      int member_339 = 0;
      if (parse_int(field[339], &member_339) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[8].hits = member_339;
      int64_t member_340 = 0;
      if (parse_int64(field[340], &member_340) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[9].pattern_id = member_340;
      int member_341 = 0;
      if (parse_int(field[341], &member_341) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[9].hits = member_341;
      int64_t member_342 = 0;
      if (parse_int64(field[342], &member_342) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[10].pattern_id = member_342;
      int member_343 = 0;
      if (parse_int(field[343], &member_343) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[10].hits = member_343;
      int64_t member_344 = 0;
      if (parse_int64(field[344], &member_344) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[11].pattern_id = member_344;
      int member_345 = 0;
      if (parse_int(field[345], &member_345) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[11].hits = member_345;
      int64_t member_346 = 0;
      if (parse_int64(field[346], &member_346) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[12].pattern_id = member_346;
      int member_347 = 0;
      if (parse_int(field[347], &member_347) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[12].hits = member_347;
      int64_t member_348 = 0;
      if (parse_int64(field[348], &member_348) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[13].pattern_id = member_348;
      int member_349 = 0;
      if (parse_int(field[349], &member_349) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[13].hits = member_349;
      int64_t member_350 = 0;
      if (parse_int64(field[350], &member_350) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[14].pattern_id = member_350;
      int member_351 = 0;
      if (parse_int(field[351], &member_351) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[14].hits = member_351;
      int64_t member_352 = 0;
      if (parse_int64(field[352], &member_352) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[15].pattern_id = member_352;
      int member_353 = 0;
      if (parse_int(field[353], &member_353) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[15].hits = member_353;
      int64_t member_354 = 0;
      if (parse_int64(field[354], &member_354) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[16].pattern_id = member_354;
      int member_355 = 0;
      if (parse_int(field[355], &member_355) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[16].hits = member_355;
      int64_t member_356 = 0;
      if (parse_int64(field[356], &member_356) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[17].pattern_id = member_356;
      int member_357 = 0;
      if (parse_int(field[357], &member_357) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[17].hits = member_357;
      int64_t member_358 = 0;
      if (parse_int64(field[358], &member_358) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[18].pattern_id = member_358;
      int member_359 = 0;
      if (parse_int(field[359], &member_359) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[18].hits = member_359;
      int64_t member_360 = 0;
      if (parse_int64(field[360], &member_360) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[19].pattern_id = member_360;
      int member_361 = 0;
      if (parse_int(field[361], &member_361) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[19].hits = member_361;
      int64_t member_362 = 0;
      if (parse_int64(field[362], &member_362) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[20].pattern_id = member_362;
      int member_363 = 0;
      if (parse_int(field[363], &member_363) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[20].hits = member_363;
      int64_t member_364 = 0;
      if (parse_int64(field[364], &member_364) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[21].pattern_id = member_364;
      int member_365 = 0;
      if (parse_int(field[365], &member_365) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[21].hits = member_365;
      int64_t member_366 = 0;
      if (parse_int64(field[366], &member_366) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[22].pattern_id = member_366;
      int member_367 = 0;
      if (parse_int(field[367], &member_367) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[22].hits = member_367;
      int64_t member_368 = 0;
      if (parse_int64(field[368], &member_368) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[23].pattern_id = member_368;
      int member_369 = 0;
      if (parse_int(field[369], &member_369) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[23].hits = member_369;
      int64_t member_370 = 0;
      if (parse_int64(field[370], &member_370) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[24].pattern_id = member_370;
      int member_371 = 0;
      if (parse_int(field[371], &member_371) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[24].hits = member_371;
      int64_t member_372 = 0;
      if (parse_int64(field[372], &member_372) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[25].pattern_id = member_372;
      int member_373 = 0;
      if (parse_int(field[373], &member_373) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[25].hits = member_373;
      int64_t member_374 = 0;
      if (parse_int64(field[374], &member_374) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[26].pattern_id = member_374;
      int member_375 = 0;
      if (parse_int(field[375], &member_375) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[26].hits = member_375;
      int64_t member_376 = 0;
      if (parse_int64(field[376], &member_376) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[27].pattern_id = member_376;
      int member_377 = 0;
      if (parse_int(field[377], &member_377) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[27].hits = member_377;
      int64_t member_378 = 0;
      if (parse_int64(field[378], &member_378) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[28].pattern_id = member_378;
      int member_379 = 0;
      if (parse_int(field[379], &member_379) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[28].hits = member_379;
      int64_t member_380 = 0;
      if (parse_int64(field[380], &member_380) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[29].pattern_id = member_380;
      int member_381 = 0;
      if (parse_int(field[381], &member_381) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[29].hits = member_381;
      int64_t member_382 = 0;
      if (parse_int64(field[382], &member_382) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[30].pattern_id = member_382;
      int member_383 = 0;
      if (parse_int(field[383], &member_383) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[30].hits = member_383;
      int64_t member_384 = 0;
      if (parse_int64(field[384], &member_384) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[31].pattern_id = member_384;
      int member_385 = 0;
      if (parse_int(field[385], &member_385) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hits[31].hits = member_385;
      int member_386 = 0;
      if (parse_int(field[386], &member_386) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      row.ap_hit_count = member_386;
      rc = db1_session_state_save(field[0], &row);
      break;
   }
   case AIMEE_DB1_OP_SESSION_STATE_DELETE:
      if (count != 1u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      rc = db1_session_state_delete(field[0]);
      break;
   case AIMEE_DB1_OP_SESSION_STATE_EXISTS:
   {
      if (count != 1u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int produced = db1_session_state_exists(field[0]);
      rc = (produced >= 0) ? 0 : -1;
      snprintf(row_text[0], sizeof row_text[0], "%lld", (long long)produced);
      row_slots[0] = row_text[0];
      rows = row_slots;
      row_count = 1u;
      break;
   }
   case AIMEE_DB1_OP_SESSION_STATE_LIST:
   {
      if (count != 1u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int parsed0;
      if (parse_int(field[0], &parsed0) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (parsed0 <= 0 || parsed0 > 256)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      db1_session_state_summary_t *found = calloc((size_t)parsed0, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_session_state_list(found, parsed0);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed0)
                                 ? (uint32_t)rc : (uint32_t)parsed0;
         const char **cells = malloc((size_t)produced * 3u * sizeof *cells);
         char (*numbers)[32] = malloc((size_t)produced * 1u * sizeof *numbers);
         if (!cells || !numbers)
         {
            free(cells);
            free(numbers);
            free(scratch);
            free(domain_rows);
            return AIMEE_MODULE_STATUS_INTERNAL;
         }
         cells_owned = cells;
         numeric_owned = numbers;
         for (uint32_t row = 0; row < produced; ++row)
         {
            snprintf(numbers[row * 1u + 0u], 32,
                     "%d", found[row].hook_call_count);
            cells[row * 3u + 0u] = found[row].session_id;
            cells[row * 3u + 1u] = found[row].updated_at;
            cells[row * 3u + 2u] = numbers[row * 1u + 0u];
         }
         rows = cells;
         row_count = produced * 3u;
      }
      listed = 1;
      break;
   }
   case AIMEE_DB1_OP_SESSION_STATE_GET_SUMMARY:
   {
      if (count != 1u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      memset(&row_db1_session_state_summary_t, 0, sizeof row_db1_session_state_summary_t);
      rc = db1_session_state_get_summary(field[0], &row_db1_session_state_summary_t);
      snprintf(row_text[0], sizeof row_text[0], "%d", row_db1_session_state_summary_t.hook_call_count);
      row_slots[0] = row_db1_session_state_summary_t.session_id;
      row_slots[1] = row_db1_session_state_summary_t.updated_at;
      row_slots[2] = row_text[0];
      rows = row_slots;
      row_count = 3u;
      reads = 1;
      break;
   }
   case AIMEE_DB1_OP_SESSION_STATE_LIST_EXPIRED:
   {
      if (count != 2u)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[0][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (!field[1][0])
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int parsed0;
      if (parse_int(field[0], &parsed0) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      int parsed1;
      if (parse_int(field[1], &parsed1) != 0)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      if (parsed1 <= 0 || parsed1 > 256)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      }
      char (*found)[DB1_SS_SID_LEN] = calloc((size_t)parsed1, sizeof *found);
      if (!found)
      {
         free(scratch);
         return AIMEE_MODULE_STATUS_INTERNAL;
      }
      domain_rows = found;
      rc = db1_session_state_list_expired(parsed0, found, parsed1);
      if (rc > 0)
      {
         uint32_t produced = ((uint32_t)rc < (uint32_t)parsed1)
                                 ? (uint32_t)rc : (uint32_t)parsed1;
         const char **cells = malloc((size_t)produced * 1u * sizeof *cells);
         if (!cells)
         {
            free(cells);
            free(scratch);
            free(domain_rows);
            return AIMEE_MODULE_STATUS_INTERNAL;
         }
         cells_owned = cells;
         for (uint32_t row = 0; row < produced; ++row)
         {
            cells[row * 1u + 0u] = found[row];
         }
         rows = cells;
         row_count = produced * 1u;
      }
      listed = 1;
      break;
   }
   default:
      free(scratch);
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   }
   free(scratch);

   /* The two conventions must not be flattened. A read returns FOUND(1),
      not-found(0) or error(-1); a write returns 0 or -1. Mapping a read's -1
      onto MISSING would report a broken store as "nothing recorded", and the
      caller would act on an absence that was never established. */
   uint32_t status;
   if (listed)
      /* A list answers with how many rows it found, so any count is success and
         only a negative return is a failure. Zero rows is an empty list, not a
         miss: the caller asked what was there and the answer was nothing. */
      status = (rc >= 0) ? AIMEE_DB1_STATUS_OK : AIMEE_DB1_STATUS_FAILED;
   else if (found)
      status = (rc > 0) ? AIMEE_DB1_STATUS_OK
                        : (rc == 0 ? AIMEE_DB1_STATUS_MISSING : AIMEE_DB1_STATUS_FAILED);
   else if (rows)
      /* A row-returning domain usually answers 0 or -1: there is no
         found/not-found distinction to preserve, so neither is invented. */
      status = (rc == 0) ? AIMEE_DB1_STATUS_OK : AIMEE_DB1_STATUS_FAILED;
   else if (reads)
   {
      if (rc < 0)
         status = AIMEE_DB1_STATUS_FAILED;
      else if (rc == 0 || !(text_owned ? text_owned : value)[0])
         status = AIMEE_DB1_STATUS_MISSING;
      else
         status = AIMEE_DB1_STATUS_OK;
   }
   else
      status = (rc == 0) ? AIMEE_DB1_STATUS_OK : AIMEE_DB1_STATUS_FAILED;

   {
      const char *held = text_owned ? text_owned : value;
      const char *one = (status == AIMEE_DB1_STATUS_OK) ? held : "";
      const char *const single[] = {one};
      const char *const *out_values = rows ? rows : (reads ? single : NULL);
      uint32_t out_count = rows ? row_count : (reads ? 1u : 0u);
      if (status != AIMEE_DB1_STATUS_OK && rows)
         out_count = 0u; /* nothing to report but the status */
      /* A reply that does not fit is a failure, not a success with nothing in
         it. write_reply refuses rather than truncating -- which is right -- but
         discarding that answer left the caller a well-formed frame carrying no
         rows, and a read cannot tell that from a row that is genuinely empty.
         Say it in the frame instead: a bare status needs eight bytes, so the
         second call fits wherever the first did not. */
      if (write_reply(response_body, response_capacity, response_len, status, out_values,
                      out_count) != status)
         write_reply(response_body, response_capacity, response_len, AIMEE_DB1_STATUS_FAILED,
                     NULL, 0u);
   }
   free(cells_owned);
   free(numeric_owned);
   free(domain_rows);
   free(text_owned);
   return AIMEE_MODULE_STATUS_OK;
}
/* clang-format on */
