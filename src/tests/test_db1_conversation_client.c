/* test_db1_conversation_client.c: the list half of the generated bus client.
 *
 * A list is the one shape whose reply length is not fixed by the operation, so
 * the client works out how many rows arrived by dividing the reply's own value
 * count by the width it knows. Everything that can go wrong goes wrong there:
 * a reply that is not a whole number of rows, a reply longer than the caller's
 * array, and a bound the caller chose rather than the domain.
 *
 * The bus is stubbed. What the real module answers is covered by
 * unit-test-db1-module-stage; what matters here is the framing, the row
 * arithmetic and the fact that a broken reply is refused rather than read. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ouroboros/module_client.h>
#include "db1_client/db1_module_api.h"
#include "db1_client/wm.h"
#include "db1_client/coord_jobs.h"
#include <aimee/ir/session_event.h>
#include "cJSON.h"
#include "db1_client/server_sessions.h"

/* --- the stubbed bus ------------------------------------------------------ */

#define WM_LIST_WIDTH 8u
/* A column is a list one value wide. */
#define WM_COLUMN_WIDTH 1u

static int stub_available = 1;
static ouro_module_call_result_t stub_result = OURO_MODULE_CALL_OK;
static uint32_t stub_status = AIMEE_DB1_STATUS_OK;
static int stub_calls;
static uint8_t stub_request[4096];
static uint32_t stub_request_len;
/* How many rows to answer with, and how many values to actually claim: the two
   are separate so a reply can be made to lie about its own shape. */
static uint32_t stub_rows;
static int stub_extra_values;
/* When set, the reply is this single value rather than rows: the returned
   string path answers with one. */
static const char *stub_text;
/* Rows are this many values wide; a column reply is one. */
static uint32_t stub_width = WM_LIST_WIDTH;

/* Explicit store peer: complete metadata rows over the actual sessions wire.
 * No actor/source/capture admission is affirmed by this transport fixture. */
static int metadata_mode, metadata_calls, metadata_bad_shape, metadata_nul, metadata_truncated;
static const char *metadata_cells[10] = {"metadata-session",
                                         "cold-default-7db-41d0e11e80bb5766",
                                         "uid:1000",
                                         "title",
                                         "2026-10-25 01:38:38",
                                         "2026-10-25 01:38:40",
                                         "sdk:opaque",
                                         "done",
                                         "source",
                                         "chat:key"};
static uint32_t metadata_rows = 1, metadata_status = AIMEE_DB1_STATUS_OK;
static ouro_module_call_result_t metadata_reply(const void *request, uint32_t request_len,
                                                void *response, uint32_t capacity,
                                                uint32_t *response_len)
{
   metadata_calls++;
   assert(request_len >= 8 && request_len <= sizeof stub_request);
   memcpy(stub_request, request, request_len);
   stub_request_len = request_len;
   uint32_t op = aimee_db1_get_u32(request);
   assert(op == AIMEE_DB1_OP_SERVER_SESSION_GET || op == AIMEE_DB1_OP_SERVER_SESSION_LIST_RECENT ||
          op == AIMEE_DB1_OP_SERVER_SESSION_SEARCH_BY_TITLE);
   uint32_t n = metadata_status == AIMEE_DB1_STATUS_OK ? metadata_rows * 10 : 0;
   if (metadata_bad_shape)
      n++;
   size_t size = 8;
   for (uint32_t i = 0; i < n; i++)
      size += 4 + strlen(metadata_cells[i % 10]);
   *response_len = (uint32_t)size;
   if (size > capacity)
      return OURO_MODULE_CALL_RESPONSE_TOO_LARGE;
   uint8_t *out = response;
   aimee_db1_put_u32(out, metadata_status);
   aimee_db1_put_u32(out + 4, n);
   uint32_t at = 8;
   for (uint32_t i = 0; i < n; i++)
   {
      uint32_t len = (uint32_t)strlen(metadata_cells[i % 10]);
      aimee_db1_put_u32(out + at, len);
      at += 4;
      memcpy(out + at, metadata_cells[i % 10], len);
      if (metadata_nul && i == 1 && len)
         out[at] = 0;
      at += len;
   }
   if (metadata_truncated)
      (*response_len)--;
   return OURO_MODULE_CALL_OK;
}

static int coord_mode, coord_calls;
static char *coord_message;
static ouro_module_call_result_t coord_reply(const void *request, void *response, uint32_t capacity,
                                             uint32_t *length)
{
   const uint8_t *input = request;
   uint32_t op = aimee_db1_get_u32(input), count = aimee_db1_get_u32(input + 4);
   coord_calls++;
   const char *cells[5] = {"42"};
   uint32_t n = 1;
   if (op == AIMEE_DB1_OP_COORD_TASK_ADD)
   {
      assert(count == 7);
      size_t at = 8;
      for (uint32_t i = 0; i < count; i++)
      {
         uint32_t size = aimee_db1_get_u32(input + at);
         at += 4;
         if (i == 4)
            assert(size == strlen(coord_message) && !memcmp(input + at, coord_message, size));
         at += size;
      }
   }
   else
   {
      assert(op == AIMEE_DB1_OP_COORD_TASK_GET_DISPATCH && count == 1);
      cells[0] = "review";
      cells[1] = coord_mode == 2 ? "{\"role\":\"user\",\"blocks\":\"bad\"}" : coord_message;
      cells[2] = "[]";
      cells[3] = "/work";
      cells[4] = "engineer";
      n = 5;
   }
   uint8_t *out = response;
   assert(capacity >= 8);
   aimee_db1_put_u32(out, AIMEE_DB1_STATUS_OK);
   aimee_db1_put_u32(out + 4, n);
   size_t at = 8;
   for (uint32_t i = 0; i < n; i++)
   {
      size_t size = strlen(cells[i]);
      assert(at + 4 + size <= capacity);
      aimee_db1_put_u32(out + at, (uint32_t)size);
      at += 4;
      memcpy(out + at, cells[i], size);
      at += size;
   }
   *length = (uint32_t)at;
   return OURO_MODULE_CALL_OK;
}

int obs_bus_module_available(uint32_t event_kind)
{
   assert(event_kind == (coord_mode      ? AIMEE_DB1_EVENT_AGENT_WORK
                         : metadata_mode ? AIMEE_DB1_EVENT_SESSIONS
                                         : AIMEE_DB1_EVENT_CONVERSATION));
   return stub_available;
}

/* Row r, member m: a value distinct enough that a transposed index shows up. */
static void cell_text(char *out, size_t cap, uint32_t row, uint32_t member)
{
   if (member == 0u)
      snprintf(out, cap, "%u", 1000u + row); /* the int64 id */
   else
      snprintf(out, cap, "r%um%u", row, member);
}

ouro_module_call_result_t
obs_bus_module_call(uint32_t event_kind, uint32_t stage_id, uint64_t trace_id, uint64_t deadline_ns,
                    const void *request_body, uint32_t request_len, void *response_body,
                    uint32_t response_capacity, uint32_t *response_len,
                    ouro_module_cancelled_fn cancelled, void *cancel_context)
{
   (void)trace_id;
   (void)cancelled;
   (void)cancel_context;
   assert(event_kind == (coord_mode      ? AIMEE_DB1_EVENT_AGENT_WORK
                         : metadata_mode ? AIMEE_DB1_EVENT_SESSIONS
                                         : AIMEE_DB1_EVENT_CONVERSATION));
   if (coord_mode)
   {
      assert(stage_id == AIMEE_DB1_STAGE_AGENT_WORK && deadline_ns);
      return coord_reply(request_body, response_body, response_capacity, response_len);
   }
   if (metadata_mode)
   {
      assert(stage_id == AIMEE_DB1_STAGE_SESSIONS && deadline_ns);
      return metadata_reply(request_body, request_len, response_body, response_capacity,
                            response_len);
   }
   assert(stage_id == AIMEE_DB1_STAGE_CONVERSATION);
   assert(deadline_ns != 0);

   stub_calls++;
   stub_request_len = request_len < sizeof stub_request ? request_len : 0;
   if (stub_request_len)
      memcpy(stub_request, request_body, stub_request_len);
   if (stub_result != OURO_MODULE_CALL_OK)
      return stub_result;

   uint8_t *out = (uint8_t *)response_body;
   if (response_capacity < 8u)
      return OURO_MODULE_CALL_RESPONSE_TOO_LARGE;
   if (stub_text)
   {
      uint32_t n = (uint32_t)strlen(stub_text);
      if (response_capacity < 12u + n)
         return OURO_MODULE_CALL_RESPONSE_TOO_LARGE;
      aimee_db1_put_u32(out, stub_status);
      aimee_db1_put_u32(out + 4u, 1u);
      aimee_db1_put_u32(out + 8u, n);
      memcpy(out + 12u, stub_text, n);
      *response_len = 12u + n;
      return OURO_MODULE_CALL_OK;
   }
   uint32_t values = stub_rows * stub_width + (uint32_t)stub_extra_values;
   aimee_db1_put_u32(out, stub_status);
   aimee_db1_put_u32(out + 4u, values);
   uint32_t at = 8u;
   for (uint32_t i = 0; i < values; ++i)
   {
      char text[64];
      cell_text(text, sizeof text, i / stub_width, i % stub_width);
      uint32_t n = (uint32_t)strlen(text);
      if (response_capacity < at + 4u + n)
         return OURO_MODULE_CALL_RESPONSE_TOO_LARGE;
      aimee_db1_put_u32(out + at, n);
      at += 4u;
      memcpy(out + at, text, n);
      at += n;
   }
   *response_len = at;
   return OURO_MODULE_CALL_OK;
}

static void reset(void)
{
   stub_available = 1;
   stub_result = OURO_MODULE_CALL_OK;
   stub_status = AIMEE_DB1_STATUS_OK;
   stub_calls = 0;
   stub_request_len = 0;
   stub_rows = 0;
   stub_extra_values = 0;
   stub_text = NULL;
   stub_width = WM_LIST_WIDTH;
}

/* --- reading back the frame the client emitted ---------------------------- */

static const char *frame_field(uint32_t index, uint32_t *len_out)
{
   uint32_t count = aimee_db1_get_u32(stub_request + 4u);
   assert(index < count);
   uint32_t at = 8u;
   for (uint32_t i = 0;; ++i)
   {
      uint32_t n = aimee_db1_get_u32(stub_request + at);
      at += 4u;
      if (i == index)
      {
         *len_out = n;
         return (const char *)stub_request + at;
      }
      at += n;
   }
}

/* --- the tests ------------------------------------------------------------ */

/* The reply carries every member of every row and nothing that says how many
   rows there are: the width is the operation's own contract, so the count
   divides out. */
static void test_a_list_reply_becomes_rows(void)
{
   reset();
   stub_rows = 3u;
   wm_entry_t rows[8];
   int found = db1_wm_list("sess-1", "notes", rows, 8);
   assert(found == 3);
   assert(stub_calls == 1);

   /* Members land in their own columns, not shifted by one. */
   assert(strcmp(rows[0].key, "r0m2") == 0);
   assert(strcmp(rows[0].value, "r0m3") == 0);
   assert(strcmp(rows[2].key, "r2m2") == 0);
   assert(strcmp(rows[2].expires_at, "r2m7") == 0);
   /* The int64 member is converted, not left as the text it travelled as. */
   assert(rows[0].id == 1000);
   assert(rows[2].id == 1002);
   /* Rows beyond what arrived are left cleared rather than stale. */
   assert(rows[3].key[0] == '\0' && rows[3].id == 0);
   printf("  PASS: test_a_list_reply_becomes_rows\n");
}

/* Nothing found is an empty list and a success, not a failure and not a row of
   empty strings. A caller iterating the result must see zero iterations. */
static void test_an_empty_list_is_zero_rows_not_an_error(void)
{
   reset();
   stub_rows = 0u;
   wm_entry_t rows[4];
   assert(db1_wm_list("sess-1", "", rows, 4) == 0);
   assert(rows[0].key[0] == '\0');
   printf("  PASS: test_an_empty_list_is_zero_rows_not_an_error\n");
}

/* A reply that is not a whole number of rows is not this operation's reply.
   Reading the rows that did fit would hand the caller a truncated final row
   built from whatever the next row's members would have been. */
static void test_a_partial_row_is_refused(void)
{
   reset();
   stub_rows = 2u;
   stub_extra_values = 3; /* two rows and a fragment */
   wm_entry_t rows[8];
   assert(db1_wm_list("sess-1", "", rows, 8) == -1);
   printf("  PASS: test_a_partial_row_is_refused\n");
}

/* More values than the caller has room for is a contract mismatch. Reading the
   first few would silently drop rows the caller asked for and was told it got. */
static void test_more_rows_than_asked_for_is_refused(void)
{
   reset();
   stub_rows = 5u;
   wm_entry_t rows[2];
   assert(db1_wm_list("sess-1", "", rows, 2) == -1);
   printf("  PASS: test_more_rows_than_asked_for_is_refused\n");
}

/* A failed status is -1 even though the reply is well formed: the caller must
   not read "no working memory" out of a store that could not be reached. */
static void test_a_failed_status_is_not_an_empty_list(void)
{
   reset();
   stub_status = AIMEE_DB1_STATUS_FAILED;
   wm_entry_t rows[4];
   assert(db1_wm_list("sess-1", "", rows, 4) == -1);

   reset();
   stub_available = 0;
   assert(db1_wm_list("sess-1", "", rows, 4) == -1);
   assert(stub_calls == 0);
   printf("  PASS: test_a_failed_status_is_not_an_empty_list\n");
}

/* The bound the stage is told is the bound the client will honour. Sending the
   caller's larger number would invite a reply the client then refuses as too
   long -- a call that fails on well-formed data. */
static void test_the_bound_sent_is_the_bound_clamped(void)
{
   reset();
   stub_rows = 1u;
   wm_entry_t rows[100];
   assert(db1_wm_list("sess-1", "", rows, 100) == 1);
   uint32_t len = 0;
   const char *sent = frame_field(2u, &len);
   assert(len == 2 && memcmp(sent, "64", 2) == 0);

   /* A bound under the ceiling travels unchanged. */
   reset();
   stub_rows = 1u;
   assert(db1_wm_list("sess-1", "", rows, 7) == 1);
   sent = frame_field(2u, &len);
   assert(len == 1 && memcmp(sent, "7", 1) == 0);
   printf("  PASS: test_the_bound_sent_is_the_bound_clamped\n");
}

/* Arguments the operation cannot use cost no round trip: the optional category
   travels as empty, but a missing session key or an unusable bound is refused
   before a frame is built. */
static void test_unusable_arguments_cost_no_round_trip(void)
{
   reset();
   wm_entry_t rows[4];
   assert(db1_wm_list(NULL, "", rows, 4) == -1);
   assert(db1_wm_list("", "", rows, 4) == -1);
   assert(db1_wm_list("sess-1", "", NULL, 4) == -1);
   assert(db1_wm_list("sess-1", "", rows, 0) == -1);
   assert(db1_wm_list("sess-1", "", rows, -1) == -1);
   assert(stub_calls == 0);

   /* The optional category is allowed to be absent, and travels as empty. */
   reset();
   stub_rows = 1u;
   assert(db1_wm_list("sess-1", NULL, rows, 4) == 1);
   uint32_t len = 1;
   frame_field(1u, &len);
   assert(len == 0);
   printf("  PASS: test_unusable_arguments_cost_no_round_trip\n");
}

/* A returned string is memory the caller frees, exactly as the in-process
   domain's contract said. The client allocates it here instead of the domain,
   and the caller cannot tell -- which is the whole point of the boundary. */
static void test_a_returned_string_is_the_callers_to_free(void)
{
   reset();
   stub_rows = 0u;
   stub_text = "session context, assembled";
   char *got = db1_wm_assemble_context("sess-1");
   assert(got != NULL);
   assert(strcmp(got, "session context, assembled") == 0);
   /* Sized to the string, not to the buffer the call reserved: the reply's
      length is known only once it arrives, so the client shrinks to fit. */
   free(got);
   assert(stub_calls == 1);
   printf("  PASS: test_a_returned_string_is_the_callers_to_free\n");
}

/* NULL means "nothing to assemble", and an empty string must mean the same,
   because that is what the wire carries for a domain that returned NULL. A
   caller that took "" for content would build an empty context block instead
   of leaving it out. */
static void test_an_empty_returned_string_is_a_miss(void)
{
   reset();
   stub_text = "";
   assert(db1_wm_assemble_context("sess-1") == NULL);

   reset();
   stub_text = "something";
   stub_status = AIMEE_DB1_STATUS_MISSING;
   assert(db1_wm_assemble_context("sess-1") == NULL);
   printf("  PASS: test_an_empty_returned_string_is_a_miss\n");
}

/* A store that cannot be reached returns NULL, which is the same answer the
   in-process domain gave when a session had nothing. That conflation is the
   domain's own contract -- this only has to avoid inventing content. */
static void test_an_unreachable_store_returns_no_string(void)
{
   reset();
   stub_available = 0;
   assert(db1_wm_assemble_context("sess-1") == NULL);
   assert(stub_calls == 0);

   reset();
   stub_result = OURO_MODULE_CALL_DEADLINE_EXCEEDED;
   assert(db1_wm_assemble_context("sess-1") == NULL);

   reset();
   assert(db1_wm_assemble_context(NULL) == NULL);
   assert(db1_wm_assemble_context("") == NULL);
   assert(stub_calls == 0);
   printf("  PASS: test_an_unreachable_store_returns_no_string\n");
}

static void test_complete_session_metadata(void)
{
   reset();
   metadata_mode = 1;
   metadata_rows = 1;
   metadata_calls = 0;
#ifdef AIMEE_ORIGINAL_SESSION_METADATA_PROBE
   /* Direct reproduction of the pre-cutover owner consumer: the normal
    * created 33-byte label makes the old full fixed-row reader unavailable. */
   db1_server_session_t owner;
   assert(!db1_server_session_get("metadata-session", &owner));
   assert(!strcmp(owner.principal, "uid:1000"));
#else
   db1_server_session_owner_t owner;
   db1_server_session_record_t record = {0}, rows[2];
   assert(strlen(metadata_cells[1]) == 33);
   assert(!db1_server_session_owner_get("metadata-session", &owner));
   assert(!strcmp(owner.id, metadata_cells[0]) && !strcmp(owner.principal, "uid:1000") &&
          !strcmp(owner.created_at, metadata_cells[4]));
   assert(!db1_server_session_record_get("metadata-session", &record));
   assert(!strcmp(record.client_type, metadata_cells[1]) &&
          !strcmp(record.claude_session_id, metadata_cells[6]));
   db1_server_session_record_free(&record);
   db1_server_session_record_free(&record);
   assert(!record.id && !record.client_type && !record.principal);
   db1_server_session_t legacy;
   assert(db1_server_session_get("metadata-session", &legacy) == -1 && !legacy.id[0]);
   /* Strict legacy ABI still refuses an unrepresentable row. Shipping owner
    * reads and full metadata projections no longer route through that ABI. */
   metadata_cells[1] = "cli";
   assert(!db1_server_session_get("metadata-session", &legacy));
   assert(!strcmp(legacy.client_type, "cli"));
   char *large = malloc(12001);
   assert(large);
   for (int i = 0; i < 4000; i++)
      memcpy(large + i * 3, "雪", 3);
   large[12000] = 0;
   metadata_cells[1] = large;
   metadata_cells[3] = large;
   metadata_cells[6] = large;
   metadata_cells[7] = large;
   metadata_cells[8] = large;
   metadata_cells[9] = large;
   int before = metadata_calls;
   assert(!db1_server_session_record_get("metadata-session", &record));
   assert(metadata_calls == before + 2); /* measured frame-size resize, no cell ceiling */
   assert(!strcmp(record.client_type, large) && !strcmp(record.title, large) &&
          !strcmp(record.claude_session_id, large) && !strcmp(record.outcome, large) &&
          !strcmp(record.source, large) && !strcmp(record.chat_key, large));
   db1_server_session_record_free(&record);
   assert(!db1_server_session_owner_get("metadata-session", &owner));
   assert(!strcmp(owner.principal, "uid:1000") && !strcmp(owner.created_at, metadata_cells[4]));
   metadata_rows = 2;
   assert(db1_server_session_record_list_recent(rows, 2) == 2);
   for (int i = 0; i < 2; i++)
   {
      assert(!strcmp(rows[i].client_type, large) && !strcmp(rows[i].title, large));
      db1_server_session_record_free(&rows[i]);
   }
   uint32_t n;
   const char *field = frame_field(0, &n);
   assert(n == 1 && !memcmp(field, "2", 1));
   assert(db1_server_session_record_search_by_title("%雪%", rows, 2) == 2);
   field = frame_field(0, &n);
   assert(n == strlen("%雪%") && !memcmp(field, "%雪%", n));
   for (int i = 0; i < 2; i++)
   {
      assert(!strcmp(rows[i].title, large));
      db1_server_session_record_free(&rows[i]);
   }
   metadata_rows = 0;
   assert(db1_server_session_record_list_recent(rows, 2) == 0 && !rows[0].id);
   metadata_rows = 1;
   /* Exact foreign principals are preserved, never replaced with caller data. */
   metadata_cells[2] = "uid:2000";
   assert(!db1_server_session_owner_get("metadata-session", &owner) &&
          !strcmp(owner.principal, "uid:2000"));
   metadata_cells[2] = "uid:1000";
   metadata_cells[4] = "2026-10-25 01:39:00";
   assert(!db1_server_session_owner_get("metadata-session", &owner) &&
          !strcmp(owner.created_at, metadata_cells[4]));
   /* No epoch normalization: overlong identity/incarnation cannot be copied. */
   metadata_cells[4] = large;
   assert(db1_server_session_owner_get("metadata-session", &owner) == -1 && !owner.id[0]);
   metadata_cells[4] = "2026-10-25 01:38:38";
   metadata_cells[0] = "other-session";
   assert(db1_server_session_record_get("metadata-session", &record) == -1 && !record.id);
   metadata_cells[0] = "metadata-session";
   for (int mode = 0; mode < 3; mode++)
   {
      metadata_bad_shape = mode == 0;
      metadata_nul = mode == 1;
      metadata_truncated = mode == 2;
      assert(db1_server_session_record_get("metadata-session", &record) == -1 && !record.id);
      assert(db1_server_session_record_list_recent(rows, 2) == -1 && !rows[0].id);
   }
   metadata_bad_shape = metadata_nul = metadata_truncated = 0;
   metadata_status = AIMEE_DB1_STATUS_MISSING;
   assert(db1_server_session_record_get("metadata-session", &record) == -1 && !record.id);
   metadata_status = AIMEE_DB1_STATUS_OK;
   metadata_cells[1] = metadata_cells[3] = metadata_cells[6] = metadata_cells[7] =
       metadata_cells[8] = metadata_cells[9] = "";
   assert(!db1_server_session_record_get("metadata-session", &record));
   assert(record.client_type && !*record.client_type && record.title && !*record.title);
   db1_server_session_record_free(&record);
   free(large);
#endif
   metadata_mode = 0;
}

static void test_coord_queue_canonical_message_round_trip(void)
{
   aimee_block_t blocks[] = {{.type = AIMEE_BLK_TEXT, .text = "task 雪\nquoted: \"task\""},
                             {.type = AIMEE_BLK_IMAGE,
                              .media_type = "image/png",
                              .media_ref = "https://fixture.invalid/input.png"}};
   aimee_message_t source = {.role = "user", .blocks = blocks, .n_blocks = 2};
   cJSON *expected = aimee_ir_message_json(&source);
   assert(expected);
   coord_message = cJSON_PrintUnformatted(expected);
   assert(coord_message);
   coord_mode = 1;
   coord_calls = 0;
   assert(db1_coord_job_add_task(7, 0, "[]", "review", &source, "/work", "engineer") == 42);
   aimee_message_t loaded = {0};
   char role[32], files[32], cwd[64], persona[32];
   assert(!db1_coord_task_get_dispatch(42, role, sizeof role, &loaded, files, sizeof files, cwd,
                                       sizeof cwd, persona, sizeof persona));
   cJSON *actual = aimee_ir_message_json(&loaded);
   assert(actual && cJSON_Compare(expected, actual, 1));
   cJSON_Delete(actual);
   aimee_message_free(&loaded);
   assert(coord_calls == 2 && !strcmp(role, "review") && !strcmp(cwd, "/work"));
   source.raw = cJSON_CreateObject();
   assert(source.raw);
   assert(db1_coord_job_add_task(7, 0, "[]", "review", &source, "/work", "engineer") < 0);
   assert(coord_calls == 2);
   cJSON_Delete(source.raw);
   source.raw = NULL;
   coord_mode = 2;
   assert(db1_coord_task_get_dispatch(42, role, sizeof role, &loaded, files, sizeof files, cwd,
                                      sizeof cwd, persona, sizeof persona) != 0);
   aimee_message_free(&loaded);
   free(coord_message);
   cJSON_Delete(expected);
   coord_mode = 0;
   puts("coord queue client: full text/image IR round trip; malformed reply and sidecar loss "
        "refused");
}

int main(void)
{
   test_coord_queue_canonical_message_round_trip();
   test_complete_session_metadata();
   printf("db1_conversation_client:\n");
   test_a_returned_string_is_the_callers_to_free();
   test_an_empty_returned_string_is_a_miss();
   test_an_unreachable_store_returns_no_string();
   test_a_list_reply_becomes_rows();
   test_an_empty_list_is_zero_rows_not_an_error();
   test_a_partial_row_is_refused();
   test_more_rows_than_asked_for_is_refused();
   test_a_failed_status_is_not_an_empty_list();
   test_the_bound_sent_is_the_bound_clamped();
   test_unusable_arguments_cost_no_round_trip();
   printf("db1_conversation_client: ok\n");
   return 0;
}
