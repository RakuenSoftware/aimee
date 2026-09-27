#include "session_internal.h"
#include <aimee/postgres/client.h>
#include <aimee/postgres/module_api.h>
#include <aimee/audit/obs_bus.h>
#include <aimee/core/event_bus/module_client.h>
#include <aimee/core/event_bus/module_protocol.h>
#include <limits.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Offline tools link only the private provider transport. */
#pragma weak obs_bus_module_call
static uint64_t deadline_ns(const aimee_postgres_session_t *session)
{
   struct timespec now;
   if (clock_gettime(CLOCK_MONOTONIC, &now))
      return 0;
   uint64_t cap =
       (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec + UINT64_C(60000000000);
   return session && session->deadline_ns && session->deadline_ns < cap ? session->deadline_ns
                                                                        : cap;
}

#define FRAME_MAX     (16u * 1024u * 1024u)
#define STATEMENT_MAX (8u * 1024u * 1024u)
#define CELL_MAX      (1024u * 1024u)
#define ARG_MAXIMUM   4096u
struct cell
{
   char *data;
   size_t length;
   unsigned char *binary;
   size_t binary_length;
   int binary_ready;
};
struct aimee_postgres_result
{
   uint64_t changed;
   size_t rows, columns;
   int more;
   char **names;
   uint32_t *oids;
   struct cell *cells;
};
struct buffer
{
   unsigned char *bytes;
   size_t used, capacity;
   int failed;
};
struct input
{
   const unsigned char *bytes;
   size_t length, at;
   int failed;
};
static int reserve(struct buffer *b, size_t n)
{
   if (b->failed || n > FRAME_MAX || b->used > FRAME_MAX - n)
   {
      b->failed = 1;
      return -1;
   }
   if (b->capacity - b->used < n)
   {
      size_t cap = b->capacity ? b->capacity : 256;
      while (cap < b->used + n)
         cap = cap > FRAME_MAX / 2 ? FRAME_MAX : cap * 2;
      void *p = realloc(b->bytes, cap);
      if (!p)
      {
         b->failed = 1;
         return -1;
      }
      b->bytes = p;
      b->capacity = cap;
   }
   return 0;
}
static void bytes(struct buffer *b, const void *p, size_t n)
{
   if ((n && !p) || reserve(b, n))
   {
      b->failed = 1;
      return;
   }
   if (n)
      memcpy(b->bytes + b->used, p, n);
   b->used += n;
}
static void u32(struct buffer *b, uint32_t v)
{
   unsigned char p[4];
   for (unsigned i = 0; i < 4; i++)
      p[i] = (unsigned char)(v >> (8 * i));
   bytes(b, p, 4);
}
static void u64(struct buffer *b, uint64_t v)
{
   unsigned char p[8];
   for (unsigned i = 0; i < 8; i++)
      p[i] = (unsigned char)(v >> (8 * i));
   bytes(b, p, 8);
}
static void string(struct buffer *b, const void *p, size_t n)
{
   if (n > CELL_MAX)
   {
      b->failed = 1;
      return;
   }
   u32(b, (uint32_t)n);
   bytes(b, p, n);
}
static const unsigned char *take(struct input *r, size_t n)
{
   if (r->failed || n > r->length - r->at)
   {
      r->failed = 1;
      return NULL;
   }
   const unsigned char *p = r->bytes + r->at;
   r->at += n;
   return p;
}
static uint32_t read32(struct input *r)
{
   const unsigned char *p = take(r, 4);
   uint32_t v = 0;
   if (p)
      for (unsigned i = 0; i < 4; i++)
         v |= (uint32_t)p[i] << (8 * i);
   return v;
}
static uint64_t read64(struct input *r)
{
   const unsigned char *p = take(r, 8);
   uint64_t v = 0;
   if (p)
      for (unsigned i = 0; i < 8; i++)
         v |= (uint64_t)p[i] << (8 * i);
   return v;
}
static char *read_string(struct input *r, size_t maximum)
{
   uint32_t n = read32(r);
   if (r->failed || n > maximum)
   {
      r->failed = 1;
      return NULL;
   }
   const unsigned char *p = take(r, n);
   if (!p || memchr(p, 0, n))
   {
      r->failed = 1;
      return NULL;
   }
   char *s = malloc((size_t)n + 1);
   if (!s)
   {
      r->failed = 1;
      return NULL;
   }
   memcpy(s, p, n);
   s[n] = 0;
   return s;
}
static void explain(char *error, size_t cap, const char *message)
{
   if (error && cap)
      snprintf(error, cap, "%s", message);
}
static int parameters(struct buffer *b, const aimee_postgres_value_t *args, size_t count)
{
   if (count > ARG_MAXIMUM || (count && !args))
      return -1;
   u32(b, (uint32_t)count);
   for (size_t i = 0; i < count; i++)
   {
      unsigned char kind = (unsigned char)args[i].kind;
      bytes(b, &kind, 1);
      switch (args[i].kind)
      {
      case AIMEE_POSTGRES_NULL:
         break;
      case AIMEE_POSTGRES_TEXT:
      case AIMEE_POSTGRES_BYTES:
         string(b, args[i].data, args[i].length);
         break;
      case AIMEE_POSTGRES_INT:
         u64(b, (uint64_t)args[i].integer);
         break;
      case AIMEE_POSTGRES_FLOAT:
      {
         uint64_t v;
         memcpy(&v, &args[i].real, 8);
         u64(b, v);
         break;
      }
      case AIMEE_POSTGRES_BOOL:
      {
         unsigned char v = args[i].integer != 0;
         bytes(b, &v, 1);
         break;
      }
      default:
         return -1;
      }
   }
   return b->failed ? -1 : 0;
}
static unsigned char *call(aimee_postgres_session_t *session, uint32_t operation, uint64_t handle,
                           const char *sql, const aimee_postgres_value_t *args, size_t count,
                           struct input *r, char sqlstate[6], char *error, size_t cap)
{
   if (sqlstate)
      sqlstate[0] = 0;
   if (error && cap)
      error[0] = 0;
   struct buffer request = {0};
   u32(&request, operation);
   u64(&request, handle);
   if (operation == 3 || operation == 4)
   {
      if (!sql || !sql[0] || strlen(sql) > STATEMENT_MAX)
      {
         free(request.bytes);
         explain(error, cap, "invalid PostgreSQL statement");
         return NULL;
      }
      u32(&request, (uint32_t)strlen(sql));
      bytes(&request, sql, strlen(sql));
      if (parameters(&request, args, count))
         request.failed = 1;
   }
   if (operation == 7)
   {
      if (!count || count > 60000)
         request.failed = 1;
      else
         u32(&request, (uint32_t)count);
   }
   if (request.failed)
   {
      free(request.bytes);
      explain(error, cap, "PostgreSQL request exceeds bounds");
      return NULL;
   }
   size_t reply_cap = (operation == 4 || operation == 6) ? FRAME_MAX : 4096;
   unsigned char *reply = malloc(reply_cap);
   if (!reply)
   {
      free(request.bytes);
      return NULL;
   }
   uint32_t length = 0;
   aimee_module_call_result_t rc;
   if (session && session->child > 0)
      rc = aimee_postgres_local_exchange(session, request.bytes, (uint32_t)request.used, reply,
                                         (uint32_t)reply_cap, &length) == 0
               ? AIMEE_MODULE_CALL_OK
               : AIMEE_MODULE_CALL_TRANSPORT;
   else if ((session && session->channel == -2) || !obs_bus_module_call)
      rc = AIMEE_MODULE_CALL_TRANSPORT;
   else
      rc = obs_bus_module_call(AIMEE_POSTGRES_EVENT_SESSION, AIMEE_POSTGRES_STAGE_SESSION, 0,
                               deadline_ns(session), request.bytes, (uint32_t)request.used, reply,
                               (uint32_t)reply_cap, &length, NULL, NULL);
   free(request.bytes);
   if (rc != AIMEE_MODULE_CALL_OK || length > reply_cap)
   {
      free(reply);
      if (session)
         session->transaction_state = 0;
      explain(error, cap, "PostgreSQL module unavailable");
      return NULL;
   }
   *r = (struct input){.bytes = reply, .length = length};
   uint32_t status = read32(r);
   char *state = read_string(r, 5), *message = read_string(r, CELL_MAX);
   if (r->failed || !state || !message || status != 0)
   {
      if (state && sqlstate)
         snprintf(sqlstate, 6, "%s", state);
      explain(error, cap, message && message[0] ? message : "invalid PostgreSQL module reply");
      free(state);
      free(message);
      free(reply);
      return NULL;
   }
   free(state);
   free(message);
   return reply;
}
static aimee_postgres_session_t *session_open(const char *credential, uint32_t policy,
                                              int64_t deadline, char *error, size_t cap)
{
   aimee_postgres_session_t *s = malloc(sizeof(*s));
   if (!s)
      return NULL;
   *s = (aimee_postgres_session_t){.channel = -1, .local_policy = policy};
   if (aimee_postgres_session_deadline(s, deadline))
   {
      free(s);
      return NULL;
   }
   if (credential && aimee_postgres_local_start(s, credential))
   {
      free(s);
      explain(error, cap, "PostgreSQL local authority unavailable");
      return NULL;
   }
   struct input r;
   unsigned char *reply = call(s, 1, 0, NULL, NULL, 0, &r, NULL, error, cap);
   if (!reply)
   {
      aimee_postgres_local_stop(s);
      free(s);
      return NULL;
   }
   uint64_t handle = read64(&r);
   int valid = !r.failed && r.at == r.length && handle != 0;
   free(reply);
   if (!valid)
   {
      aimee_postgres_local_stop(s);
      free(s);
      explain(error, cap, "invalid PostgreSQL session reply");
      return NULL;
   }
   s->handle = handle;
   s->transaction_state = 'I';
   if (credential)
   {
      s->local_credential = strdup(credential);
      if (!s->local_credential)
      {
         aimee_postgres_session_close(s);
         return NULL;
      }
   }
   return s;
}
aimee_postgres_session_t *aimee_postgres_session_open(char *error, size_t cap)
{
   return session_open(NULL, 0, 0, error, cap);
}
aimee_postgres_session_t *aimee_postgres_session_open_local(const char *credential, char *error,
                                                            size_t cap)
{
   if (!credential || !*credential)
      return NULL;
   return session_open(credential, 0, 0, error, cap);
}
aimee_postgres_session_t *aimee_postgres_session_open_configured(char *error, size_t cap)
{
   return session_open("", 3, 0, error, cap);
}
aimee_postgres_session_t *aimee_postgres_session_open_migration(char *error, size_t cap)
{
   return session_open("", 2, 0, error, cap);
}
aimee_postgres_session_t *aimee_postgres_session_open_operator(const char *credential,
                                                               int64_t deadline, char *error,
                                                               size_t cap)
{
   if (!credential || !*credential || deadline <= 0)
      return NULL;
   return session_open(credential, 1, deadline, error, cap);
}
int aimee_postgres_session_deadline(aimee_postgres_session_t *session, int64_t milliseconds)
{
   if (!session || milliseconds < 0 || (uint64_t)milliseconds > UINT64_MAX / 1000000)
      return -1;
   session->deadline_ns = (uint64_t)milliseconds * 1000000;
   return 0;
}
static void clear_credential(aimee_postgres_session_t *s)
{
   if (!s->local_credential)
      return;
   volatile unsigned char *p = (volatile unsigned char *)s->local_credential;
   size_t n = strlen(s->local_credential);
   while (n--)
      *p++ = 0;
   free(s->local_credential);
   s->local_credential = NULL;
}
int aimee_postgres_session_reconnect(aimee_postgres_session_t *s, char *error, size_t capacity)
{
   if (!s)
      return -1;
   /* A private provider may still be holding locks after a transport failure.
    * Close its authority channel before acquiring a replacement. */
   if (s->local_credential)
      aimee_postgres_local_stop(s);
   else
   {
      struct input input;
      unsigned char *reply = call(s, 2, s->handle, NULL, NULL, 0, &input, NULL, NULL, 0);
      free(reply);
   }
   aimee_postgres_session_t *next = session_open(
       s->local_credential, s->local_policy, (int64_t)(s->deadline_ns / 1000000), error, capacity);
   s->handle = 0;
   if (!next)
      return -1;
   clear_credential(s);
   *s = *next;
   free(next);
   return 0;
}
static int close_session(aimee_postgres_session_t *s, uint32_t operation)
{
   if (!s)
      return 0;
   struct input r;
   unsigned char *reply = call(s, operation, s->handle, NULL, NULL, 0, &r, NULL, NULL, 0);
   int rc = reply && !r.failed && r.at == r.length ? 0 : -1;
   free(reply);
   aimee_postgres_local_stop(s);
   clear_credential(s);
   free(s);
   return rc;
}
int aimee_postgres_session_close(aimee_postgres_session_t *s)
{
   return close_session(s, 2);
}
int aimee_postgres_session_discard(aimee_postgres_session_t *s)
{
   return close_session(s, 8);
}
int aimee_postgres_session_stats(aimee_postgres_session_stats_t *stats)
{
   if (!stats)
      return -1;
   memset(stats, 0, sizeof(*stats));
   struct input input;
   unsigned char *reply = call(NULL, 9, 0, NULL, NULL, 0, &input, NULL, NULL, 0);
   if (!reply)
      return -1;
   stats->capacity = read32(&input);
   stats->in_use = read32(&input);
   stats->waiters = read32(&input);
   stats->refused = read64(&input);
   stats->expired = read64(&input);
   stats->discarded = read64(&input);
   int valid = !input.failed && input.at == input.length && stats->in_use <= stats->capacity;
   free(reply);
   return valid ? 0 : -1;
}
char aimee_postgres_session_cached_state(const aimee_postgres_session_t *session)
{
   return session ? session->transaction_state : 0;
}
int aimee_postgres_session_state(aimee_postgres_session_t *s, char *state)
{
   if (!s || !state)
      return -1;
   struct input r;
   unsigned char *reply = call(s, 5, s->handle, NULL, NULL, 0, &r, NULL, NULL, 0);
   if (!reply)
      return -1;
   const unsigned char *value = take(&r, 1);
   int rc = -1;
   if (value && r.at == r.length && (*value == 'I' || *value == 'T' || *value == 'E'))
   {
      *state = (char)*value;
      s->transaction_state = *state;
      rc = 0;
   }
   free(reply);
   return rc;
}
int aimee_postgres_session_exec(aimee_postgres_session_t *s, const char *sql,
                                const aimee_postgres_value_t *args, size_t count, uint64_t *changed,
                                char sqlstate[6], char *error, size_t cap)
{
   if (!s)
      return -1;
   struct input r;
   unsigned char *reply = call(s, 3, s->handle, sql, args, count, &r, sqlstate, error, cap);
   if (!reply)
      return -1;
   const unsigned char *state = take(&r, 1);
   uint64_t rows = read64(&r);
   int valid =
       state && (*state == 'I' || *state == 'T' || *state == 'E') && !r.failed && r.at == r.length;
   char transaction = state ? (char)*state : 0;
   free(reply);
   if (!valid)
   {
      explain(error, cap, "invalid PostgreSQL execution reply");
      return -1;
   }
   s->transaction_state = transaction;
   if (changed)
      *changed = rows;
   return 0;
}
int aimee_postgres_session_wait(aimee_postgres_session_t *session, unsigned milliseconds)
{
   if (!session || !milliseconds || milliseconds > 60000)
      return -1;
   struct input input;
   unsigned char *reply =
       call(session, 7, session->handle, NULL, NULL, milliseconds, &input, NULL, NULL, 0);
   if (!reply)
      return -1;
   uint32_t notified = read32(&input);
   int result = !input.failed && input.at == input.length && notified <= 1 ? (int)notified : -1;
   free(reply);
   return result;
}
void aimee_postgres_result_free(aimee_postgres_result_t *r)
{
   if (!r)
      return;
   if (r->names)
      for (size_t i = 0; i < r->columns; i++)
         free(r->names[i]);
   if (r->cells)
      for (size_t i = 0; i < r->rows * r->columns; i++)
      {
         free(r->cells[i].data);
         free(r->cells[i].binary);
      }
   free(r->names);
   free(r->oids);
   free(r->cells);
   free(r);
}
static aimee_postgres_result_t *read_page(uint32_t operation, aimee_postgres_session_t *s,
                                          const char *sql, const aimee_postgres_value_t *args,
                                          size_t count, char sqlstate[6], char *error, size_t cap)
{
   if (!s)
      return NULL;
   struct input input;
   unsigned char *reply =
       call(s, operation, s->handle, sql, args, count, &input, sqlstate, error, cap);
   if (!reply)
      return NULL;
   aimee_postgres_result_t *r = calloc(1, sizeof(*r));
   if (!r)
   {
      free(reply);
      return NULL;
   }
   const unsigned char *state = take(&input, 1);
   const unsigned char *more = take(&input, 1);
   r->more = more ? *more : 0;
   r->changed = read64(&input);
   r->columns = read32(&input);
   r->rows = read32(&input);
   if (!more || *more > 1 || !state || (*state != 'I' && *state != 'T' && *state != 'E') ||
       input.failed || r->columns > ARG_MAXIMUM || r->rows > 4096 ||
       (r->columns && r->rows > (input.length - input.at) / 4 / r->columns))
      goto invalid;
   r->names = calloc(r->columns, sizeof(*r->names));
   r->oids = calloc(r->columns, sizeof(*r->oids));
   if (r->columns && (!r->names || !r->oids))
      goto invalid;
   for (size_t c = 0; c < r->columns; c++)
   {
      r->names[c] = read_string(&input, 1024);
      r->oids[c] = read32(&input);
      if (input.failed || !r->names[c])
         goto invalid;
   }
   size_t cells = r->rows * r->columns;
   r->cells = calloc(cells, sizeof(*r->cells));
   if (cells && !r->cells)
      goto invalid;
   for (size_t i = 0; i < cells; i++)
   {
      uint32_t n = read32(&input);
      if (input.failed)
         goto invalid;
      if (n == UINT32_MAX)
         continue;
      if (n > CELL_MAX)
         goto invalid;
      const unsigned char *p = take(&input, n);
      if (!p)
         goto invalid;
      r->cells[i].data = malloc((size_t)n + 1);
      if (!r->cells[i].data)
         goto invalid;
      memcpy(r->cells[i].data, p, n);
      r->cells[i].data[n] = 0;
      r->cells[i].length = n;
   }
   if (input.failed || input.at != input.length)
      goto invalid;
   s->transaction_state = (char)*state;
   free(reply);
   return r;
invalid:
   free(reply);
   aimee_postgres_result_free(r);
   explain(error, cap, "invalid PostgreSQL result reply");
   return NULL;
}
static void abandon_result(aimee_postgres_session_t *session)
{
   struct input input;
   unsigned char *reply = call(session, 2, session->handle, NULL, NULL, 0, &input, NULL, NULL, 0);
   free(reply);
   session->handle = 0;
}
aimee_postgres_result_t *aimee_postgres_session_query(aimee_postgres_session_t *session,
                                                      const char *sql,
                                                      const aimee_postgres_value_t *args,
                                                      size_t count, char sqlstate[6], char *error,
                                                      size_t cap)
{
   char local_state[6] = "";
   char *state = sqlstate ? sqlstate : local_state;
   aimee_postgres_result_t *result = read_page(4, session, sql, args, count, state, error, cap);
   if (!result)
   {
      if (session && !state[0])
         abandon_result(session);
      return NULL;
   }
   while (result->more)
   {
      aimee_postgres_result_t *next = read_page(6, session, NULL, NULL, 0, state, error, cap);
      if (!next)
         goto failed;
      int matches = next->columns == result->columns;
      for (size_t i = 0; matches && i < result->columns; i++)
         matches = next->oids[i] == result->oids[i] && !strcmp(next->names[i], result->names[i]);
      if (!matches || next->rows > SIZE_MAX - result->rows ||
          (result->columns &&
           result->rows + next->rows > SIZE_MAX / sizeof(struct cell) / result->columns))
      {
         aimee_postgres_result_free(next);
         explain(error, cap, "invalid PostgreSQL continuation reply");
         goto failed;
      }
      size_t total = (result->rows + next->rows) * result->columns;
      if (next->rows && next->columns)
      {
         struct cell *cells = realloc(result->cells, total * sizeof(*cells));
         if (!cells)
         {
            aimee_postgres_result_free(next);
            explain(error, cap, "out of memory reading PostgreSQL result");
            goto failed;
         }
         result->cells = cells;
         memcpy(cells + result->rows * result->columns, next->cells,
                next->rows * next->columns * sizeof(*cells));
         free(next->cells);
         next->cells = NULL;
      }
      result->rows += next->rows;
      result->changed = next->changed;
      result->more = next->more;
      aimee_postgres_result_free(next);
   }
   return result;
failed:
   aimee_postgres_result_free(result);
   abandon_result(session);
   return NULL;
}
size_t aimee_postgres_result_rows(const aimee_postgres_result_t *r)
{
   return r ? r->rows : 0;
}
size_t aimee_postgres_result_columns(const aimee_postgres_result_t *r)
{
   return r ? r->columns : 0;
}
uint64_t aimee_postgres_result_changes(const aimee_postgres_result_t *r)
{
   return r ? r->changed : 0;
}
const char *aimee_postgres_result_name(const aimee_postgres_result_t *r, size_t c)
{
   return r && c < r->columns ? r->names[c] : NULL;
}
uint32_t aimee_postgres_result_oid(const aimee_postgres_result_t *r, size_t c)
{
   return r && c < r->columns ? r->oids[c] : 0;
}
const void *aimee_postgres_result_cell(const aimee_postgres_result_t *r, size_t row, size_t col,
                                       size_t *length)
{
   if (length)
      *length = 0;
   if (!r || row >= r->rows || col >= r->columns)
      return NULL;
   const struct cell *c = &r->cells[row * r->columns + col];
   if (length)
      *length = c->length;
   return c->data;
}

const void *aimee_postgres_result_binary(aimee_postgres_result_t *result, size_t row, size_t column,
                                         size_t *length)
{
   if (length)
      *length = 0;
   if (!result || row >= result->rows || column >= result->columns)
      return NULL;
   struct cell *cell = &result->cells[row * result->columns + column];
   if (!cell->data)
      return NULL;
   uint32_t oid = result->oids[column];
   if (oid != 16 && oid != 17 && oid != 20 && oid != 21 && oid != 23)
   {
      if (length)
         *length = cell->length;
      return cell->data;
   }
   if (!cell->binary_ready)
   {
      cell->binary_ready = 1;
      size_t n = oid == 20 ? 8 : oid == 21 ? 2 : oid == 23 ? 4 : 1;
      uint64_t value = 0;
      if (oid == 17)
      {
         if (cell->length < 2 || cell->data[0] != '\\' || cell->data[1] != 'x' ||
             (cell->length - 2) % 2)
            return NULL;
         n = (cell->length - 2) / 2;
      }
      else if (oid == 16)
      {
         if (cell->length != 1 || (cell->data[0] != 't' && cell->data[0] != 'f'))
            return NULL;
         value = cell->data[0] == 't';
      }
      else
      {
         errno = 0;
         char *end = NULL;
         long long number = strtoll(cell->data, &end, 10);
         if (errno || !end || end != cell->data + cell->length || end == cell->data ||
             (oid == 21 && (number < INT16_MIN || number > INT16_MAX)) ||
             (oid == 23 && (number < INT32_MIN || number > INT32_MAX)))
            return NULL;
         value = (uint64_t)number;
      }
      unsigned char *binary = malloc(n ? n : 1);
      if (!binary)
         return NULL;
      if (oid == 17)
      {
         for (size_t i = 0; i < n; i++)
         {
            unsigned char a = (unsigned char)cell->data[2 + i * 2],
                          b = (unsigned char)cell->data[3 + i * 2];
            int hi = a >= '0' && a <= '9' ? a - '0' : a >= 'a' && a <= 'f' ? a - 'a' + 10 : -1;
            int lo = b >= '0' && b <= '9' ? b - '0' : b >= 'a' && b <= 'f' ? b - 'a' + 10 : -1;
            if (hi < 0 || lo < 0)
            {
               free(binary);
               return NULL;
            }
            binary[i] = (unsigned char)(hi * 16 + lo);
         }
      }
      else
         for (size_t i = 0; i < n; i++)
            binary[n - i - 1] = (unsigned char)(value >> (i * 8));
      cell->binary = binary;
      cell->binary_length = n;
   }
   if (length)
      *length = cell->binary_length;
   return cell->binary;
}
