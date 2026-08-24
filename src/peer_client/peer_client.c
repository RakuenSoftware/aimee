/* peer_client.c: db1-fields-v2 to the aimee module, from the server.
 *
 * See peer_client.h for why this exists at all. The wire is:
 *
 *   request:  op(u32) | field_count(u32) | (len(u32) | bytes) * n
 *   response: status(u32) | field_count(u32) | (len(u32) | bytes) * n
 *
 * little-endian, every value as text. This is db1's wire, reused deliberately
 * rather than reinvented -- one dialect in the tree, and the encode/decode
 * helpers below are the same aimee_db1_put_u32/get_u32 the store's clients use.
 */
#include "peer_client.h"

#include "db1_module_api.h"

#include <aimee/audit/obs_bus.h>
#include <aimee/core/event_bus/module_client.h>
#include <aimee/core/event_bus/module_protocol.h>

#include "log.h"
#include "module_json_call.h" /* aimee_module_call_deadline_ns */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The bus formula, not a transcribed number: kind = 4096 + ref*256 + stage.
 * A module's identity and the kinds it answers on are ONE fact, and writing
 * 12033 here would be a second copy of it free to drift when the aimee module
 * is folded into db1's ref. Derived, the renumber lands in one place. */
#define PEER_PRINCIPAL_REF     31u
#define PEER_EVENT_KIND(stage) (4096u + PEER_PRINCIPAL_REF * 256u + (uint32_t)(stage))

#define PEER_STAGE_DELIVERY 1u
#define PEER_STAGE_INBOX    2u

#define PEER_OP_SEND       1u
#define PEER_OP_INBOX_LEN  1u
#define PEER_OP_INBOX_TAKE 3u

#define PEER_CALL_TIMEOUT_MS 5000

/* Response capacity, derived from the module's own ceilings rather than picked.
 *
 * peer.MaxTextBytes is 8192 and peerwire.MessageWidth is 11, so one row is one
 * text cell plus ten identifier-ish cells. PEER_CELL_MAX bounds those ten
 * generously; a reply whose cell exceeds it overruns the buffer, the bus call
 * fails, and this client reports a TRANSPORT failure -- which is the honest
 * answer, because a reply it could not receive is not a refusal. */
#define PEER_TEXT_MAX 8192u
#define PEER_CELL_MAX 1024u
#define PEER_ROW_BYTES                                                                             \
   ((uint32_t)(PEER_CLIENT_MESSAGE_WIDTH - 1) * (4u + PEER_CELL_MAX) + 4u + PEER_TEXT_MAX)
/* header + the leading `remaining` cell + the rows one take may return */
#define PEER_RESPONSE_MAX (8u + 4u + 32u + (uint32_t)PEER_CLIENT_INBOX_TAKE_MAX * PEER_ROW_BYTES)

/* A name for each transport outcome, so a failure says WHICH failure.
 *
 * These are aimee_module_call_result_t values. Naming them matters more here
 * than it looks: "the module did not answer" covers a module that is absent, a
 * grant that denied the call, a deadline that expired, and a reply too large to
 * receive -- four different things to do about it, and the first three are
 * indistinguishable to anyone reading a log that says only "unreachable". */
static const char *call_result_name(int rc)
{
   switch (rc)
   {
   case AIMEE_MODULE_CALL_OK:
      return "ok";
   case AIMEE_MODULE_CALL_CAPABILITY_ABSENT:
      return "capability_absent";
   case AIMEE_MODULE_CALL_CAPABILITY_DENIED:
      return "capability_denied";
   case AIMEE_MODULE_CALL_CANCELLED:
      return "cancelled";
   case AIMEE_MODULE_CALL_DEADLINE_EXCEEDED:
      return "deadline_exceeded";
   case AIMEE_MODULE_CALL_INVALID_REQUEST:
      return "invalid_request";
   case AIMEE_MODULE_CALL_INTERNAL:
      return "internal";
   case AIMEE_MODULE_CALL_RESPONSE_TOO_LARGE:
      return "response_too_large";
   case AIMEE_MODULE_CALL_TRANSPORT:
      return "transport";
   case AIMEE_MODULE_CALL_PROTOCOL:
      return "protocol";
   case AIMEE_MODULE_CALL_INVALID_ARGUMENT:
      return "invalid_argument";
   default:
      return "unknown";
   }
}

/* Warn once per DISTINCT result, not once per process.
 *
 * It was once per process, and that is a defect with a long silent tail: the
 * first failure claims the single warning, and every later one -- including a
 * different failure, for a different reason, needing a different repair -- is
 * silent. A module that goes absent, comes back, then starts denying the grant
 * logs "unreachable" once and never mentions the denial. One message standing
 * for several repairs.
 *
 * Per distinct code keeps the volume bounded (eleven lines at the absolute
 * worst, for the life of the process) while never letting a NEW kind of failure
 * hide behind an old one. */
static void warn_unreachable(int reason)
{
   static unsigned seen;
   unsigned bit = (reason >= 0 && reason < 31) ? (1u << reason) : (1u << 31);
   if (seen & bit)
      return;
   seen |= bit;
   LOG_WARN("peer.client", "aimee peer module call failed: %s (%d)", call_result_name(reason),
            reason);
}

const char *peer_client_transport_name(int transport)
{
   return call_result_name(transport);
}

int peer_client_available(void)
{
   return obs_bus_module_available(PEER_EVENT_KIND(PEER_STAGE_DELIVERY));
}

const char *peer_client_status_name(uint32_t status)
{
   switch (status)
   {
   case PEER_CLIENT_STATUS_OK:
      return "ok";
   case PEER_CLIENT_STATUS_NO_PEER:
      return "no_peer";
   case PEER_CLIENT_STATUS_DENIED:
      return "denied";
   case PEER_CLIENT_STATUS_INBOX_FULL:
      return "inbox_full";
   case PEER_CLIENT_STATUS_HOP_LIMIT:
      return "hop_limit";
   case PEER_CLIENT_STATUS_CYCLE:
      return "cycle";
   case PEER_CLIENT_STATUS_TIMEOUT:
      return "timeout";
   case PEER_CLIENT_STATUS_SELF:
      return "self";
   case PEER_CLIENT_STATUS_TOO_LONG:
      return "too_long";
   case PEER_CLIENT_STATUS_LABEL_TAKEN:
      return "label_taken";
   case PEER_CLIENT_STATUS_UNKNOWN_SENDER:
      return "unknown_sender";
   case PEER_CLIENT_STATUS_BAD_REQUEST:
      return "bad_request";
   case PEER_CLIENT_STATUS_SHUTDOWN:
      return "shutdown";
   case PEER_CLIENT_STATUS_NO_CHANNEL:
      return "no_channel";
   case PEER_CLIENT_STATUS_NOT_MEMBER:
      return "not_member";
   case PEER_CLIENT_STATUS_CHANNEL_FULL:
      return "channel_full";
   case PEER_CLIENT_STATUS_UNAVAILABLE:
      return "unavailable";
   case PEER_CLIENT_STATUS_UNCLASSIFIED:
      return "unclassified";
   case PEER_CLIENT_STATUS_AT_CAPACITY:
      return "at_capacity";
   case PEER_CLIENT_STATUS_NO_DIRECTORY:
      return "no_directory";
   case PEER_CLIENT_STATUS_DIRECTORY_REFUSED:
      return "directory_refused";
   default:
      return "unknown";
   }
}

/* ── frame encode ─────────────────────────────────────────────────────────── */

static int frame_size(const char *const *fields, uint32_t count, size_t *need_out)
{
   if (count > AIMEE_DB1_FIELDS_MAX)
      return -1;
   size_t need = 8u;
   for (uint32_t i = 0; i < count; ++i)
   {
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

/* ── frame decode ─────────────────────────────────────────────────────────── */

/* A decoded reply. `cells` are NUL-terminated copies; the module refuses to
 * emit a field containing NUL precisely so this copy is lossless. */
struct reply
{
   uint32_t status;
   uint32_t count;
   char **cells;
};

static void reply_free(struct reply *r)
{
   if (!r || !r->cells)
      return;
   for (uint32_t i = 0; i < r->count; ++i)
      free(r->cells[i]);
   free(r->cells);
   r->cells = NULL;
   r->count = 0;
}

/* Say WHICH protocol disagreement, once per distinct one.
 *
 * Same lesson as warn_unreachable, one level down: "the reply was unreadable"
 * covers a short header, a cell count past the ceiling, a length running off
 * the end, trailing bytes the frame never declared, and a row of the wrong
 * width. Five disagreements, five different things to go and look at, and a
 * reader given only "protocol" has to reproduce the failure to learn which.
 *
 * This is not hypothetical tidiness: a `protocol` verdict with no detail is
 * exactly where the CT 9101 diagnosis stopped, and the next question -- which
 * part of the frame -- needed another container to answer. */
static void warn_protocol(const char *what, uint32_t got, uint32_t want)
{
   static const char *seen[8];
   static unsigned count;
   for (unsigned i = 0; i < count; i++)
      if (seen[i] == what)
         return;
   if (count < sizeof(seen) / sizeof(seen[0]))
      seen[count++] = what;
   LOG_WARN("peer.client", "aimee peer reply is unreadable: %s (got %u, want %u)", what, got, want);
}

/* Returns 0 when the whole frame parsed, -1 otherwise. A partial parse is NOT
 * a partial success: a truncated reply and a short reply read the same once the
 * cells are handed on, so the frame is accepted whole or not at all. */
static int reply_decode(const uint8_t *body, uint32_t len, struct reply *out)
{
   out->cells = NULL;
   out->count = 0;
   if (len < 8u)
   {
      warn_protocol("reply shorter than its header", len, 8u);
      return -1;
   }
   out->status = aimee_db1_get_u32(body);
   uint32_t count = aimee_db1_get_u32(body + 4u);
   if (count > AIMEE_DB1_FIELDS_MAX)
   {
      warn_protocol("declared cell count above the ceiling", count, AIMEE_DB1_FIELDS_MAX);
      return -1;
   }
   if (count == 0)
      return 0;
   char **cells = calloc(count, sizeof(*cells));
   if (!cells)
      return -1;
   uint32_t at = 8u;
   for (uint32_t i = 0; i < count; ++i)
   {
      if (at + 4u > len)
      {
         warn_protocol("frame ended where a cell length was expected", len, at + 4u);
         goto bad;
      }
      uint32_t n = aimee_db1_get_u32(body + at);
      at += 4u;
      /* A declared length running past what arrived is not a cell to read part
         of -- and the addition is checked for wrap before the comparison, so a
         hostile length cannot make `at + n` fold back inside the buffer. */
      if (n > len || at > len - n)
      {
         warn_protocol("cell length runs past the end of the frame", n, len - at);
         goto bad;
      }
      cells[i] = malloc((size_t)n + 1u);
      if (!cells[i])
         goto bad;
      memcpy(cells[i], body + at, n);
      cells[i][n] = '\0';
      at += n;
   }
   /* Trailing bytes the frame never declared mean the sender and this reader
      disagree about the frame, which is not something to ignore because the
      part we understood looked fine. */
   if (at != len)
   {
      /* Trailing bytes the frame never declared. Worth naming precisely: it is
         the one disagreement that looks like success right up until the counts
         are compared, because every cell before it read correctly. */
      warn_protocol("trailing bytes the frame never declared", len, at);
      goto bad;
   }
   out->cells = cells;
   out->count = count;
   return 0;
bad:
   for (uint32_t i = 0; i < count; ++i)
      free(cells[i]);
   free(cells);
   return -1;
}

/* One call. Returns PEER_CLIENT_OK with `out` filled, PEER_CLIENT_REFUSED with
 * `out->status` set and no cells, or PEER_CLIENT_TRANSPORT. */
static peer_client_result_t call_stage(uint32_t stage, uint32_t op, const char *const *fields,
                                       uint32_t count, struct reply *out, int *transport)
{
   uint32_t kind = PEER_EVENT_KIND(stage);
   if (transport)
      *transport = AIMEE_MODULE_CALL_OK;
   if (!obs_bus_module_available(kind))
   {
      /* Nothing serves this kind on the local bus. Distinct from a call that
         went out and failed, and the distinction is the whole reason the
         out-param exists: this one means the module is not there. */
      if (transport)
         *transport = AIMEE_MODULE_CALL_CAPABILITY_ABSENT;
      warn_unreachable(AIMEE_MODULE_CALL_CAPABILITY_ABSENT);
      return PEER_CLIENT_TRANSPORT;
   }
   size_t request_len = 0;
   if (frame_size(fields, count, &request_len) != 0)
   {
      if (transport)
         *transport = AIMEE_MODULE_CALL_INVALID_ARGUMENT;
      return PEER_CLIENT_TRANSPORT;
   }
   uint8_t *request = malloc(request_len);
   uint8_t *response = malloc(PEER_RESPONSE_MAX);
   if (!request || !response)
   {
      free(request);
      free(response);
      if (transport)
         *transport = AIMEE_MODULE_CALL_INTERNAL;
      return PEER_CLIENT_TRANSPORT;
   }
   encode(request, op, fields, count);
   uint32_t response_len = 0;
   uint64_t deadline = aimee_module_call_deadline_ns(PEER_CALL_TIMEOUT_MS);
   aimee_module_call_result_t rc =
       obs_bus_module_call(kind, stage, 0, deadline, request, (uint32_t)request_len, response,
                           PEER_RESPONSE_MAX, &response_len, NULL, NULL);
   free(request);
   if (rc != AIMEE_MODULE_CALL_OK)
   {
      if (transport)
         *transport = (int)rc;
      warn_unreachable((int)rc);
      free(response);
      return PEER_CLIENT_TRANSPORT;
   }
   int decoded = reply_decode(response, response_len, out);
   free(response);
   if (decoded != 0)
   {
      /* The call SUCCEEDED at the transport and the frame is unreadable, which
         is a protocol disagreement rather than an unreachable module. Reported
         as such so it is not mistaken for the module being down. */
      if (transport)
         *transport = AIMEE_MODULE_CALL_PROTOCOL;
      return PEER_CLIENT_TRANSPORT;
   }
   if (out->status != PEER_CLIENT_STATUS_OK)
   {
      /* A refusal carries no cells worth reading, and freeing them here means
         no caller has to remember that a refusal still allocated. */
      reply_free(out);
      return PEER_CLIENT_REFUSED;
   }
   return PEER_CLIENT_OK;
}

/* ── message rows ─────────────────────────────────────────────────────────── */

void peer_client_message_free(peer_client_message_t *m)
{
   if (!m)
      return;
   free(m->id);
   free(m->correlation_id);
   free(m->conversation_id);
   free(m->from_session);
   free(m->from_owner);
   free(m->from_label);
   free(m->origin_session);
   free(m->sent_at);
   free(m->text);
   memset(m, 0, sizeof(*m));
}

void peer_client_messages_free(peer_client_message_t *m, size_t count)
{
   if (!m)
      return;
   for (size_t i = 0; i < count; ++i)
      peer_client_message_free(&m[i]);
   free(m);
}

/* Take ownership of one row's cells. On failure the caller frees whatever was
 * already moved via peer_client_message_free, so nothing is double-freed: every
 * cell this consumes is NULLed in the source array. */
static int row_take(char **cells, peer_client_message_t *m)
{
   m->id = cells[0];
   m->correlation_id = cells[1];
   m->conversation_id = cells[2];
   m->from_session = cells[3];
   m->from_owner = cells[4];
   m->from_label = cells[5];
   m->origin_session = cells[6];
   m->sent_at = cells[9];
   m->text = cells[10];
   for (int i = 0; i < PEER_CLIENT_MESSAGE_WIDTH; ++i)
      if (i != 7 && i != 8)
         cells[i] = NULL;
   /* hop and is_reply travel as text like every other cell. A hop that does not
      parse is a malformed row rather than hop zero: defaulting it would make a
      corrupt frame look like a first-hop message. */
   char *end = NULL;
   long hop = strtol(cells[7], &end, 10);
   if (!end || *end != '\0' || end == cells[7] || hop < 0 || hop > 1000000)
      return -1;
   m->hop = (int)hop;
   /* The wire's boolean grammar is peerwire.Atob's, mirrored EXACTLY.
      peerwire.Btoa writes "1"/"0" and Atob reads "1"/"true" and "0"/"false"/"",
      and this reader once accepted only "true"/"false" -- so it rejected every
      row the module has ever sent, because Btoa never writes those words.
      The asymmetry is what hid it: Atob's leniency accepted this client's
      requests, so half the exchange worked and only the reply direction broke,
      which reads at the caller as the module failing rather than as a grammar
      this side got wrong. */
   if (strcmp(cells[8], "1") == 0 || strcmp(cells[8], "true") == 0)
      m->is_reply = 1;
   else if (strcmp(cells[8], "0") == 0 || strcmp(cells[8], "false") == 0 || cells[8][0] == '\0')
      m->is_reply = 0;
   else
      return -1;
   return 0;
}

/* ── operations ───────────────────────────────────────────────────────────── */

peer_client_result_t peer_client_send(const char *from, const char *to, const char *text,
                                      const char *conversation_id, int expect_reply,
                                      peer_client_message_t *stamped, uint32_t *status,
                                      int *transport)
{
   if (status)
      *status = PEER_CLIENT_STATUS_OK;
   if (transport)
      *transport = AIMEE_MODULE_CALL_OK;
   if (stamped)
      memset(stamped, 0, sizeof(*stamped));
   if (!from || !to || !text)
   {
      if (transport)
         *transport = AIMEE_MODULE_CALL_INVALID_ARGUMENT;
      return PEER_CLIENT_TRANSPORT;
   }
   /* from, to, text, conversation_id, hop, expect_reply */
   /* "1"/"0", which is what peerwire.Btoa writes. Atob accepts "true"/"false"
      too, so this cell worked either way -- and that is precisely why writing
      the other spelling was not caught: one lenient reader kept a second
      grammar alive on the wire until the strict direction met it. */
   const char *fields[6] = {
       from, to, text, conversation_id ? conversation_id : "", "0", expect_reply ? "1" : "0"};
   struct reply r;
   peer_client_result_t rc =
       call_stage(PEER_STAGE_DELIVERY, PEER_OP_SEND, fields, 6, &r, transport);
   if (rc == PEER_CLIENT_REFUSED && status)
      *status = r.status;
   if (rc != PEER_CLIENT_OK)
      return rc;
   if (r.count != PEER_CLIENT_MESSAGE_WIDTH)
   {
      /* The module answered OK with a shape this build does not know. Reading a
         prefix of it would be reading a different contract's row. */
      warn_protocol("send reply is not one whole message row", r.count,
                    (uint32_t)PEER_CLIENT_MESSAGE_WIDTH);
      reply_free(&r);
      if (transport)
         *transport = AIMEE_MODULE_CALL_PROTOCOL;
      return PEER_CLIENT_TRANSPORT;
   }
   if (stamped)
   {
      if (row_take(r.cells, stamped) != 0)
      {
         peer_client_message_free(stamped);
         reply_free(&r);
         if (transport)
            *transport = AIMEE_MODULE_CALL_PROTOCOL;
         return PEER_CLIENT_TRANSPORT;
      }
   }
   reply_free(&r);
   return PEER_CLIENT_OK;
}

peer_client_result_t peer_client_inbox_len(const char *session, int *waiting, int *dropped,
                                           uint32_t *status, int *transport)
{
   if (status)
      *status = PEER_CLIENT_STATUS_OK;
   if (transport)
      *transport = AIMEE_MODULE_CALL_OK;
   if (waiting)
      *waiting = 0;
   if (dropped)
      *dropped = 0;
   if (!session)
   {
      if (transport)
         *transport = AIMEE_MODULE_CALL_INVALID_ARGUMENT;
      return PEER_CLIENT_TRANSPORT;
   }
   const char *fields[1] = {session};
   struct reply r;
   peer_client_result_t rc =
       call_stage(PEER_STAGE_INBOX, PEER_OP_INBOX_LEN, fields, 1, &r, transport);
   if (rc == PEER_CLIENT_REFUSED && status)
      *status = r.status;
   if (rc != PEER_CLIENT_OK)
      return rc;
   if (r.count != 2)
   {
      warn_protocol("inbox_len reply is not two cells", r.count, 2u);
      reply_free(&r);
      if (transport)
         *transport = AIMEE_MODULE_CALL_PROTOCOL;
      return PEER_CLIENT_TRANSPORT;
   }
   if (waiting)
      *waiting = atoi(r.cells[0]);
   if (dropped)
      *dropped = atoi(r.cells[1]);
   reply_free(&r);
   return PEER_CLIENT_OK;
}

peer_client_result_t peer_client_inbox_take(const char *session, int max,
                                            peer_client_message_t **out, size_t *count,
                                            int *remaining, uint32_t *status, int *transport)
{
   if (status)
      *status = PEER_CLIENT_STATUS_OK;
   if (transport)
      *transport = AIMEE_MODULE_CALL_OK;
   if (out)
      *out = NULL;
   if (count)
      *count = 0;
   if (remaining)
      *remaining = 0;
   if (!session || !out || !count)
   {
      if (transport)
         *transport = AIMEE_MODULE_CALL_INVALID_ARGUMENT;
      return PEER_CLIENT_TRANSPORT;
   }
   if (max <= 0 || max > PEER_CLIENT_INBOX_TAKE_MAX)
      max = PEER_CLIENT_INBOX_TAKE_MAX;
   char maxbuf[16];
   snprintf(maxbuf, sizeof(maxbuf), "%d", max);
   const char *fields[2] = {session, maxbuf};
   struct reply r;
   peer_client_result_t rc =
       call_stage(PEER_STAGE_INBOX, PEER_OP_INBOX_TAKE, fields, 2, &r, transport);
   if (rc == PEER_CLIENT_REFUSED && status)
      *status = r.status;
   if (rc != PEER_CLIENT_OK)
      return rc;
   /* The reply LEADS with how many remain, then whole rows. A cell count that
      is not one plus a whole number of rows means this reader and the module
      disagree about the row width, and accepting the remainder would hand back
      a final row whose tail is whatever the next row's head was. */
   if (r.count < 1 || (r.count - 1) % PEER_CLIENT_MESSAGE_WIDTH != 0)
   {
      warn_protocol("take reply is not a remaining-count plus whole rows", r.count,
                    (uint32_t)PEER_CLIENT_MESSAGE_WIDTH);
      reply_free(&r);
      if (transport)
         *transport = AIMEE_MODULE_CALL_PROTOCOL;
      return PEER_CLIENT_TRANSPORT;
   }
   size_t rows = (size_t)(r.count - 1) / PEER_CLIENT_MESSAGE_WIDTH;
   if (remaining)
      *remaining = atoi(r.cells[0]);
   if (rows == 0)
   {
      reply_free(&r);
      return PEER_CLIENT_OK; /* understood, and the answer is none */
   }
   peer_client_message_t *msgs = calloc(rows, sizeof(*msgs));
   if (!msgs)
   {
      reply_free(&r);
      if (transport)
         *transport = AIMEE_MODULE_CALL_INTERNAL;
      return PEER_CLIENT_TRANSPORT;
   }
   for (size_t i = 0; i < rows; ++i)
   {
      if (row_take(&r.cells[1 + i * PEER_CLIENT_MESSAGE_WIDTH], &msgs[i]) != 0)
      {
         peer_client_messages_free(msgs, rows);
         reply_free(&r);
         if (transport)
            *transport = AIMEE_MODULE_CALL_PROTOCOL;
         return PEER_CLIENT_TRANSPORT;
      }
   }
   reply_free(&r);
   *out = msgs;
   *count = rows;
   return PEER_CLIENT_OK;
}
