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

#include <aimee/core/event_bus/module_client.h>
#include "db1_module_api.h"
#include "wm.h"

/* --- the stubbed bus ------------------------------------------------------ */

#define WM_LIST_WIDTH 8u
/* A column is a list one value wide. */
#define WM_COLUMN_WIDTH 1u

static int stub_available = 1;
static aimee_module_call_result_t stub_result = AIMEE_MODULE_CALL_OK;
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

int obs_bus_module_available(uint32_t event_kind)
{
   assert(event_kind == AIMEE_DB1_EVENT_CONVERSATION);
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

aimee_module_call_result_t
obs_bus_module_call(uint32_t event_kind, uint32_t stage_id, uint64_t trace_id, uint64_t deadline_ns,
                    const void *request_body, uint32_t request_len, void *response_body,
                    uint32_t response_capacity, uint32_t *response_len,
                    aimee_module_cancelled_fn cancelled, void *cancel_context)
{
   (void)trace_id;
   (void)cancelled;
   (void)cancel_context;
   assert(event_kind == AIMEE_DB1_EVENT_CONVERSATION);
   assert(stage_id == AIMEE_DB1_STAGE_CONVERSATION);
   assert(deadline_ns != 0);

   stub_calls++;
   stub_request_len = request_len < sizeof stub_request ? request_len : 0;
   if (stub_request_len)
      memcpy(stub_request, request_body, stub_request_len);
   if (stub_result != AIMEE_MODULE_CALL_OK)
      return stub_result;

   uint8_t *out = (uint8_t *)response_body;
   if (response_capacity < 8u)
      return AIMEE_MODULE_CALL_RESPONSE_TOO_LARGE;
   if (stub_text)
   {
      uint32_t n = (uint32_t)strlen(stub_text);
      if (response_capacity < 12u + n)
         return AIMEE_MODULE_CALL_RESPONSE_TOO_LARGE;
      aimee_db1_put_u32(out, stub_status);
      aimee_db1_put_u32(out + 4u, 1u);
      aimee_db1_put_u32(out + 8u, n);
      memcpy(out + 12u, stub_text, n);
      *response_len = 12u + n;
      return AIMEE_MODULE_CALL_OK;
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
         return AIMEE_MODULE_CALL_RESPONSE_TOO_LARGE;
      aimee_db1_put_u32(out + at, n);
      at += 4u;
      memcpy(out + at, text, n);
      at += n;
   }
   *response_len = at;
   return AIMEE_MODULE_CALL_OK;
}

static void reset(void)
{
   stub_available = 1;
   stub_result = AIMEE_MODULE_CALL_OK;
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
   stub_result = AIMEE_MODULE_CALL_DEADLINE_EXCEEDED;
   assert(db1_wm_assemble_context("sess-1") == NULL);

   reset();
   assert(db1_wm_assemble_context(NULL) == NULL);
   assert(db1_wm_assemble_context("") == NULL);
   assert(stub_calls == 0);
   printf("  PASS: test_an_unreachable_store_returns_no_string\n");
}

/* A column is a list whose row is one value rather than a struct: the same
   frame and the same arithmetic, width one. The value lands in the caller's
   fixed-width row directly, with no member to name. */
static void test_a_column_fills_the_callers_rows(void)
{
   reset();
   stub_width = WM_COLUMN_WIDTH;
   stub_rows = 3u;
   char ids[8][WM_SESSION_ID_LEN];
   int found = db1_wm_search_session_ids("needle", ids, 8);
   assert(found == 3);
   /* member 0 of each row, because a column has only member 0. */
   assert(strcmp(ids[0], "1000") == 0);
   assert(strcmp(ids[1], "1001") == 0);
   assert(strcmp(ids[2], "1002") == 0);
   assert(ids[3][0] == '\0');
   printf("  PASS: test_a_column_fills_the_callers_rows\n");
}

/* The same refusals a struct list makes, because it is the same code path:
   an empty column is zero rows, and a reply longer than the caller's array is
   refused rather than truncated. */
static void test_a_column_refuses_what_a_row_list_refuses(void)
{
   reset();
   stub_width = WM_COLUMN_WIDTH;
   char ids[4][WM_SESSION_ID_LEN];
   assert(db1_wm_search_session_ids("needle", ids, 4) == 0);

   reset();
   stub_width = WM_COLUMN_WIDTH;
   stub_rows = 9u;
   assert(db1_wm_search_session_ids("needle", ids, 4) == -1);

   reset();
   stub_width = WM_COLUMN_WIDTH;
   stub_status = AIMEE_DB1_STATUS_FAILED;
   stub_rows = 1u;
   assert(db1_wm_search_session_ids("needle", ids, 4) == -1);

   reset();
   assert(db1_wm_search_session_ids(NULL, ids, 4) == -1);
   assert(db1_wm_search_session_ids("needle", NULL, 4) == -1);
   assert(db1_wm_search_session_ids("needle", ids, 0) == -1);
   assert(stub_calls == 0);
   printf("  PASS: test_a_column_refuses_what_a_row_list_refuses\n");
}

int main(void)
{
   printf("db1_conversation_client:\n");
   test_a_column_fills_the_callers_rows();
   test_a_column_refuses_what_a_row_list_refuses();
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
