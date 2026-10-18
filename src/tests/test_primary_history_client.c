#include "db1_client/primary_sessions.h"
#include "db1_client/db1_module_api.h"
#include <aimee/core/event_bus/module_client.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static char *transcript;
static unsigned calls;
static int unavailable, malformed, too_large;
int obs_bus_module_available(uint32_t kind)
{
   (void)kind;
   return !unavailable;
}
aimee_module_call_result_t obs_bus_module_call(uint32_t kind, uint32_t stage, uint64_t trace,
                                               uint64_t deadline, const void *request,
                                               uint32_t request_len, void *response,
                                               uint32_t capacity, uint32_t *length,
                                               aimee_module_cancelled_fn cancelled, void *context)
{
   (void)kind;
   (void)stage;
   (void)trace;
   (void)deadline;
   (void)request_len;
   (void)cancelled;
   (void)context;
   calls++;
   uint32_t op = aimee_db1_get_u32(request);
   int load = op == AIMEE_DB1_OP_PRIMARY_SESSION_LOAD;
   const char *cells[] = {"session", "codex", "chatgpt", "created", "updated", transcript};
   int count = transcript ? (load ? 1 : 6) : 0;
   size_t total = 8;
   for (int i = 0; i < count; i++)
      total += 4 + strlen(load ? transcript : cells[i]);
   *length = too_large ? 17u * 1024u * 1024u : (uint32_t)total;
   if (*length > capacity)
      return AIMEE_MODULE_CALL_RESPONSE_TOO_LARGE;
   uint8_t *out = response;
   aimee_db1_put_u32(out, transcript ? AIMEE_DB1_STATUS_OK : AIMEE_DB1_STATUS_MISSING);
   aimee_db1_put_u32(out + 4, malformed ? 7 : (uint32_t)count);
   uint32_t at = 8;
   for (int i = 0; i < count; i++)
   {
      const char *cell = load ? transcript : cells[i];
      uint32_t size = (uint32_t)strlen(cell);
      aimee_db1_put_u32(out + at, size);
      at += 4;
      memcpy(out + at, cell, size);
      at += size;
   }
   return AIMEE_MODULE_CALL_OK;
}
int main(void)
{
   size_t size = 2u * 1024u * 1024u;
   transcript = malloc(size + 1);
   memset(transcript, 'x', size);
   transcript[size] = 0;
   char *loaded = db1_primary_session_load("session", "codex", "chatgpt");
   assert(loaded && !strcmp(loaded, transcript) && calls == 2);
   free(loaded);
   db1_primary_session_row_t row;
   assert(db1_primary_session_get_latest("session", &row) == 0);
   assert(!strcmp(row.messages_json, transcript) && !strcmp(row.provider, "chatgpt"));
   db1_primary_session_row_clear(&row);
   db1_primary_session_row_t *rows = NULL;
   assert(db1_primary_session_alloc_recent(&rows, 256) == 1);
   assert(!strcmp(rows[0].messages_json, transcript));
   db1_primary_session_rows_free(rows, 1);
   assert(db1_primary_session_alloc_search("find", &rows, 256) == 1);
   assert(!strcmp(rows[0].messages_json, transcript));
   db1_primary_session_rows_free(rows, 1);
   malformed = 1;
   assert(db1_primary_session_get_latest("session", &row) == -1 && !row.messages_json);
   malformed = 0;
   too_large = 1;
   unsigned before = calls;
   assert(db1_primary_session_get_latest("session", &row) == -1 && calls == before + 1);
   too_large = 0;
   free(transcript);
   transcript = NULL;
   assert(db1_primary_session_get_latest("session", &row) == 1);
   unavailable = 1;
   assert(db1_primary_session_get_latest("session", &row) == -1);
   puts(
       "primary history transport: large load/latest/list/search, bounds and missing/error passed");
   return 0;
}
