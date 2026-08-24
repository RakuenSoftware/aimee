/* test_peer_client.c — the server's peer-messaging client, against a scripted
 * module.
 *
 * What this exists to catch is not "does a send work". It is the set of ways a
 * client of a counted-cell wire quietly reads the wrong thing:
 *
 *   - a reply the module never sent, read as a refusal (the three outcomes)
 *   - a row of the wrong width, read as a prefix of a right one
 *   - a take reply whose cells are not a whole number of rows
 *   - a corrupt scalar cell, read as its zero value
 *
 * Each of those turns a malformed frame into a plausible answer, which is the
 * failure mode that has no symptom. The scripted responder below lets each be
 * produced deliberately, so the guard is shown failing on the bad frame rather
 * than merely passing on the good one. */
#include "peer_client.h"

#include <aimee/core/event_bus/module_client.h>

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── the scripted module ──────────────────────────────────────────────────── */

static int g_available = 1;
static aimee_module_call_result_t g_transport = AIMEE_MODULE_CALL_OK;
static uint8_t g_reply[64 * 1024];
static uint32_t g_reply_len;
/* What the client actually put on the wire, kept so a test can assert the
   request rather than only the answer. */
static uint8_t g_seen[64 * 1024];
static uint32_t g_seen_len;
static uint32_t g_seen_kind, g_seen_stage;

int obs_bus_module_available(uint32_t event_kind)
{
   (void)event_kind;
   return g_available;
}

aimee_module_call_result_t
obs_bus_module_call(uint32_t event_kind, uint32_t stage_id, uint64_t trace_id, uint64_t deadline_ns,
                    const void *request_body, uint32_t request_len, void *response_body,
                    uint32_t response_capacity, uint32_t *response_len,
                    aimee_module_cancelled_fn cancelled, void *cancel_context)
{
   (void)trace_id;
   (void)deadline_ns;
   (void)cancelled;
   (void)cancel_context;
   g_seen_kind = event_kind;
   g_seen_stage = stage_id;
   g_seen_len = request_len < sizeof(g_seen) ? request_len : (uint32_t)sizeof(g_seen);
   memcpy(g_seen, request_body, g_seen_len);
   if (g_transport != AIMEE_MODULE_CALL_OK)
      return g_transport;
   if (g_reply_len > response_capacity)
      return AIMEE_MODULE_CALL_INTERNAL;
   memcpy(response_body, g_reply, g_reply_len);
   *response_len = g_reply_len;
   return AIMEE_MODULE_CALL_OK;
}

/* ── frame building, for the replies the module is told to give ───────────── */

static void put_u32(uint8_t *p, uint32_t v)
{
   p[0] = (uint8_t)(v & 0xffu);
   p[1] = (uint8_t)((v >> 8) & 0xffu);
   p[2] = (uint8_t)((v >> 16) & 0xffu);
   p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static uint32_t get_u32(const uint8_t *p)
{
   return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void reply_set(uint32_t status, const char *const *cells, uint32_t count)
{
   uint32_t at = 0;
   put_u32(g_reply + at, status);
   at += 4;
   put_u32(g_reply + at, count);
   at += 4;
   for (uint32_t i = 0; i < count; i++)
   {
      uint32_t n = (uint32_t)strlen(cells[i]);
      put_u32(g_reply + at, n);
      at += 4;
      memcpy(g_reply + at, cells[i], n);
      at += n;
   }
   g_reply_len = at;
}

/* Read cell `want` out of whatever the client last sent. */
static const char *seen_cell(uint32_t want, char *buf, size_t cap)
{
   uint32_t count = get_u32(g_seen + 4);
   uint32_t at = 8;
   for (uint32_t i = 0; i < count; i++)
   {
      uint32_t n = get_u32(g_seen + at);
      at += 4;
      if (i == want)
      {
         size_t take = n < cap - 1 ? n : cap - 1;
         memcpy(buf, g_seen + at, take);
         buf[take] = '\0';
         return buf;
      }
      at += n;
   }
   buf[0] = '\0';
   return buf;
}

/* A well-formed 11-cell message row. */
/* The row the MODULE actually sends. is_reply is "0", because peerwire.Btoa
   writes "1"/"0" and never the words -- this fixture used to say "false", which
   agreed with the client's bug and made every check pass against a row that
   cannot come off the wire. A fixture written from the same misreading as the
   code under test confirms the misreading. */
static const char *ROW[PEER_CLIENT_MESSAGE_WIDTH] = {
    "msg-1",       "corr-1", "conv-1", "sess-a", "uid:1000",
    "alpha",       "sess-a", "0",      "0",      "1787554800000000000",
    "hello from a"};

static int checks;
static void ok(int cond, const char *what)
{
   checks++;
   if (!cond)
   {
      printf("FAIL  %s\n", what);
      exit(1);
   }
   printf("PASS  %s\n", what);
}

static void reset(void)
{
   g_available = 1;
   g_transport = AIMEE_MODULE_CALL_OK;
   g_reply_len = 0;
   g_seen_len = 0;
}

/* ── the outcomes stay three ──────────────────────────────────────────────── */

static void test_three_outcomes(void)
{
   char buf[64];
   uint32_t status = 999;
   peer_client_message_t m;

   /* 1. nothing serving the stage. NOT a refusal: the request was never judged,
         and a caller that reads this as "the peer said no" stops trying. */
   reset();
   g_available = 0;
   ok(peer_client_send("a", "b", "hi", NULL, 0, &m, &status, NULL) == PEER_CLIENT_TRANSPORT,
      "no module serving delivery is a transport failure, not a refusal");
   ok(status == PEER_CLIENT_STATUS_OK,
      "a transport failure leaves status untouched (there was no status to report)");

   /* 2. the module answered, understood, and said no. */
   reset();
   reply_set(PEER_CLIENT_STATUS_UNKNOWN_SENDER, NULL, 0);
   ok(peer_client_send("ghost", "b", "hi", NULL, 0, &m, &status, NULL) == PEER_CLIENT_REFUSED,
      "a non-zero status is a domain refusal");
   ok(status == PEER_CLIENT_STATUS_UNKNOWN_SENDER, "the refusal carries WHICH no");
   ok(strcmp(peer_client_status_name(status), "unknown_sender") == 0,
      "the status has a name a human can read");

   /* 3. the module answered and it worked. */
   reset();
   reply_set(PEER_CLIENT_STATUS_OK, ROW, PEER_CLIENT_MESSAGE_WIDTH);
   ok(peer_client_send("sess-a", "sess-b", "hello from a", "conv-1", 0, &m, &status, NULL) ==
          PEER_CLIENT_OK,
      "a zero status with a full row is a delivery");
   ok(strcmp(m.text, "hello from a") == 0, "the stamped envelope carries the text");
   ok(strcmp(m.from_owner, "uid:1000") == 0, "and the owner the MODULE stamped, not the caller's");
   ok(m.hop == 0 && m.is_reply == 0, "hop and is_reply decode from their text cells");
   peer_client_message_free(&m);

   /* The request the client actually built. The sender is a cell the caller
      cannot forge from above, so it is worth asserting it is the one passed. */
   ok(get_u32(g_seen) == 1u, "send uses op 1 on the delivery stage");
   ok(get_u32(g_seen + 4) == 6u, "send sends exactly six fields");
   ok(strcmp(seen_cell(0, buf, sizeof buf), "sess-a") == 0, "cell 0 is the sender");
   ok(strcmp(seen_cell(1, buf, sizeof buf), "sess-b") == 0, "cell 1 is the recipient");
   ok(strcmp(seen_cell(5, buf, sizeof buf), "0") == 0,
      "expect_reply travels in peerwire's grammar (\"0\"), not a second spelling");
   ok(strcmp(seen_cell(3, buf, sizeof buf), "conv-1") == 0,
      "conversation_id is passed through, so a caller can thread onto one");

   /* Both optional parameters are ADVERTISED in the tool schema, and an
      advertised parameter nothing exercises is a promise the handler has never
      been asked to keep. This repo has the precedent: git_commit came to
      advertise parameters its handler never accepted. */
   reset();
   reply_set(PEER_CLIENT_STATUS_OK, ROW, PEER_CLIENT_MESSAGE_WIDTH);
   ok(peer_client_send("sess-a", "sess-b", "t", NULL, 1, &m, &status, NULL) == PEER_CLIENT_OK,
      "a send with expect_reply set is accepted");
   peer_client_message_free(&m);
   ok(strcmp(seen_cell(5, buf, sizeof buf), "1") == 0,
      "expect_reply=true travels as \"1\", the spelling Btoa writes");
   ok(strcmp(seen_cell(3, buf, sizeof buf), "") == 0,
      "an omitted conversation_id travels as the empty cell, which opens a new one");
   ok(g_seen_kind == 4096u + 31u * 256u + 1u && g_seen_stage == 1u,
      "the kind is the bus formula for ref 31 stage 1, not a transcribed 12033");
}

/* ── a row of the wrong width is not a row ────────────────────────────────── */

static void test_row_width(void)
{
   peer_client_message_t m;
   uint32_t status = 0;

   /* Ten cells. Reading a prefix would succeed and hand back a message whose
      text is whatever cell ten happened to be -- indistinguishable, afterwards,
      from a real one. */
   reset();
   reply_set(PEER_CLIENT_STATUS_OK, ROW, PEER_CLIENT_MESSAGE_WIDTH - 1);
   ok(peer_client_send("a", "b", "hi", NULL, 0, &m, &status, NULL) == PEER_CLIENT_TRANSPORT,
      "a SHORT row is refused rather than read as a message");

   /* Twelve. A wider row is a newer module: cells append, so the first eleven
      are still right -- and this client still refuses, because "the first
      eleven are right" is an assumption about a module it was not built
      against. Refusing is the safe direction and is asserted so a later
      decision to accept wider rows has to change a test that says why. */
   reset();
   {
      const char *wide[PEER_CLIENT_MESSAGE_WIDTH + 1];
      for (int i = 0; i < PEER_CLIENT_MESSAGE_WIDTH; i++)
         wide[i] = ROW[i];
      wide[PEER_CLIENT_MESSAGE_WIDTH] = "a cell from a newer module";
      reply_set(PEER_CLIENT_STATUS_OK, wide, PEER_CLIENT_MESSAGE_WIDTH + 1);
   }
   ok(peer_client_send("a", "b", "hi", NULL, 0, &m, &status, NULL) == PEER_CLIENT_TRANSPORT,
      "a WIDER row is refused too, rather than read as its first eleven cells");
}

/* ── malformed frames ─────────────────────────────────────────────────────── */

static void test_malformed(void)
{
   peer_client_message_t m;
   uint32_t status = 0;

   /* A declared cell length running past the frame. */
   reset();
   reply_set(PEER_CLIENT_STATUS_OK, ROW, PEER_CLIENT_MESSAGE_WIDTH);
   put_u32(g_reply + 8, 0xfffffff0u); /* first cell claims to be enormous */
   ok(peer_client_send("a", "b", "hi", NULL, 0, &m, &status, NULL) == PEER_CLIENT_TRANSPORT,
      "a cell length past the end of the frame is refused, not clamped");

   /* Trailing bytes the frame never declared. */
   reset();
   reply_set(PEER_CLIENT_STATUS_OK, ROW, PEER_CLIENT_MESSAGE_WIDTH);
   g_reply[g_reply_len++] = 0x41;
   ok(peer_client_send("a", "b", "hi", NULL, 0, &m, &status, NULL) == PEER_CLIENT_TRANSPORT,
      "trailing undeclared bytes are refused, not ignored");

   /* A header alone, with a status of OK and no cells: a send that reports
      success and delivers no envelope. */
   reset();
   reply_set(PEER_CLIENT_STATUS_OK, NULL, 0);
   ok(peer_client_send("a", "b", "hi", NULL, 0, &m, &status, NULL) == PEER_CLIENT_TRANSPORT,
      "OK with no row is refused: a delivery with no envelope is not a delivery");

   /* A corrupt hop cell. Defaulting it to zero would make a corrupt frame look
      like a first-hop message, which is exactly the shape that has no symptom. */
   reset();
   {
      const char *bad[PEER_CLIENT_MESSAGE_WIDTH];
      for (int i = 0; i < PEER_CLIENT_MESSAGE_WIDTH; i++)
         bad[i] = ROW[i];
      bad[7] = "not-a-number";
      reply_set(PEER_CLIENT_STATUS_OK, bad, PEER_CLIENT_MESSAGE_WIDTH);
   }
   ok(peer_client_send("a", "b", "hi", NULL, 0, &m, &status, NULL) == PEER_CLIENT_TRANSPORT,
      "a hop cell that does not parse is refused, not read as hop 0");

   /* And an is_reply that is neither true nor false. */
   reset();
   {
      const char *bad[PEER_CLIENT_MESSAGE_WIDTH];
      for (int i = 0; i < PEER_CLIENT_MESSAGE_WIDTH; i++)
         bad[i] = ROW[i];
      bad[8] = "yes";
      reply_set(PEER_CLIENT_STATUS_OK, bad, PEER_CLIENT_MESSAGE_WIDTH);
   }
   ok(peer_client_send("a", "b", "hi", NULL, 0, &m, &status, NULL) == PEER_CLIENT_TRANSPORT,
      "an is_reply cell outside peerwire's grammar is refused, not read as false");

   /* Every spelling peerwire.Atob accepts must be accepted here, because the two
      sides have to share ONE grammar. This client accepted only the two words
      Btoa NEVER writes, so it rejected every real row while its own tests stayed
      green -- the fixture had been written from the same misreading. These are
      the checks that would have caught it. */
   {
      const char *const truthy[] = {"1", "true"};
      const char *const falsy[] = {"0", "false", ""};
      for (size_t i = 0; i < sizeof truthy / sizeof truthy[0]; i++)
      {
         const char *row[PEER_CLIENT_MESSAGE_WIDTH];
         for (int j = 0; j < PEER_CLIENT_MESSAGE_WIDTH; j++)
            row[j] = ROW[j];
         row[8] = truthy[i];
         reset();
         reply_set(PEER_CLIENT_STATUS_OK, row, PEER_CLIENT_MESSAGE_WIDTH);
         peer_client_message_t got;
         int good =
             peer_client_send("a", "b", "hi", NULL, 0, &got, &status, NULL) == PEER_CLIENT_OK &&
             got.is_reply == 1;
         if (good)
            peer_client_message_free(&got);
         ok(good, "peerwire's truthy spellings are read as a reply");
      }
      for (size_t i = 0; i < sizeof falsy / sizeof falsy[0]; i++)
      {
         const char *row[PEER_CLIENT_MESSAGE_WIDTH];
         for (int j = 0; j < PEER_CLIENT_MESSAGE_WIDTH; j++)
            row[j] = ROW[j];
         row[8] = falsy[i];
         reset();
         reply_set(PEER_CLIENT_STATUS_OK, row, PEER_CLIENT_MESSAGE_WIDTH);
         peer_client_message_t got;
         int good =
             peer_client_send("a", "b", "hi", NULL, 0, &got, &status, NULL) == PEER_CLIENT_OK &&
             got.is_reply == 0;
         if (good)
            peer_client_message_free(&got);
         ok(good, "peerwire's falsy spellings are read as not-a-reply");
      }
   }
}

/* ── the inbox ────────────────────────────────────────────────────────────── */

static void test_inbox(void)
{
   peer_client_message_t *msgs = NULL;
   size_t count = 0;
   int remaining = -1;
   uint32_t status = 0;
   char buf[64];

   /* Empty is an ANSWER. Zero rows with status OK must not read as a failure,
      or a caller with no mail concludes the module is broken. */
   reset();
   {
      const char *cells[1] = {"0"};
      reply_set(PEER_CLIENT_STATUS_OK, cells, 1);
   }
   ok(peer_client_inbox_take("sess-b", 0, &msgs, &count, &remaining, &status, NULL) ==
          PEER_CLIENT_OK,
      "an empty inbox is a success, not a failure");
   ok(count == 0 && remaining == 0 && msgs == NULL, "and it reports nothing taken, none waiting");

   /* One row, and five still waiting. The `remaining` cell is the only thing
      that distinguishes a complete drain from a capped one. */
   reset();
   {
      const char *cells[1 + PEER_CLIENT_MESSAGE_WIDTH];
      cells[0] = "5";
      for (int i = 0; i < PEER_CLIENT_MESSAGE_WIDTH; i++)
         cells[1 + i] = ROW[i];
      reply_set(PEER_CLIENT_STATUS_OK, cells, 1 + PEER_CLIENT_MESSAGE_WIDTH);
   }
   ok(peer_client_inbox_take("sess-b", 4, &msgs, &count, &remaining, &status, NULL) ==
          PEER_CLIENT_OK,
      "a take with rows succeeds");
   ok(count == 1, "one row decodes to one message");
   ok(remaining == 5, "and REMAINING is reported, so a caller knows to ask again");
   ok(strcmp(msgs[0].text, "hello from a") == 0, "the message text survives the round trip");
   peer_client_messages_free(msgs, count);

   ok(get_u32(g_seen) == 3u, "take uses op 3 on the inbox stage");
   ok(strcmp(seen_cell(1, buf, sizeof buf), "4") == 0, "the requested max travels as text");

   /* An over-large ask is clamped rather than sent, because the reply must fit
      a buffer this client sized from the module's own ceilings. */
   reset();
   {
      const char *cells[1] = {"0"};
      reply_set(PEER_CLIENT_STATUS_OK, cells, 1);
   }
   (void)peer_client_inbox_take("sess-b", 9999, &msgs, &count, &remaining, &status, NULL);
   {
      char asked[16];
      char want[16];
      snprintf(want, sizeof want, "%d", PEER_CLIENT_INBOX_TAKE_MAX);
      ok(strcmp(seen_cell(1, asked, sizeof asked), want) == 0,
         "an over-large max is clamped to what one reply can carry");
   }

   /* Cells that are not one plus a whole number of rows. Accepting the
      remainder would hand back a last row whose tail is the next row's head. */
   reset();
   {
      const char *cells[1 + PEER_CLIENT_MESSAGE_WIDTH];
      cells[0] = "0";
      for (int i = 0; i < PEER_CLIENT_MESSAGE_WIDTH; i++)
         cells[1 + i] = ROW[i];
      reply_set(PEER_CLIENT_STATUS_OK, cells, PEER_CLIENT_MESSAGE_WIDTH); /* one short */
   }
   ok(peer_client_inbox_take("sess-b", 4, &msgs, &count, &remaining, &status, NULL) ==
          PEER_CLIENT_TRANSPORT,
      "a take reply that is not a whole number of rows is refused");

   /* An unknown session is a refusal from the module, and must arrive as one. */
   reset();
   reply_set(PEER_CLIENT_STATUS_NO_PEER, NULL, 0);
   ok(peer_client_inbox_take("gone", 4, &msgs, &count, &remaining, &status, NULL) ==
          PEER_CLIENT_REFUSED,
      "an unknown session refuses rather than reporting an empty inbox");
   ok(status == PEER_CLIENT_STATUS_NO_PEER, "and says which no");

   /* inbox_len's own arity. */
   reset();
   {
      const char *cells[2] = {"3", "1"};
      reply_set(PEER_CLIENT_STATUS_OK, cells, 2);
   }
   int waiting = -1, dropped = -1;
   ok(peer_client_inbox_len("sess-b", &waiting, &dropped, &status, NULL) == PEER_CLIENT_OK,
      "inbox_len reads its two cells");
   ok(waiting == 3 && dropped == 1, "waiting and dropped are separate facts");

   reset();
   {
      const char *cells[1] = {"3"};
      reply_set(PEER_CLIENT_STATUS_OK, cells, 1);
   }
   ok(peer_client_inbox_len("sess-b", &waiting, &dropped, &status, NULL) == PEER_CLIENT_TRANSPORT,
      "a one-cell inbox_len reply is refused rather than leaving dropped stale");
}

/* ── the status table ─────────────────────────────────────────────────────── */

static void test_status_names(void)
{
   /* Every status this build knows has a name. A missing arm returns "unknown",
      which would make a real refusal unreadable in the one place a human sees
      it. Ranged rather than enumerated: an added status joins the loop by
      existing, instead of by someone remembering to list it. */
   for (uint32_t s = 0; s < PEER_CLIENT_STATUS_COUNT; s++)
   {
      if (strcmp(peer_client_status_name(s), "unknown") == 0)
      {
         printf("FAIL  status %u has no name\n", s);
         exit(1);
      }
   }
   checks++;
   printf("PASS  every status below PEER_CLIENT_STATUS_COUNT has a name\n");

   ok(strcmp(peer_client_status_name(PEER_CLIENT_STATUS_COUNT), "unknown") == 0,
      "a status this build does not know is 'unknown', not mislabelled as one it does");
}

/* ── the transport outcome is NAMED, not summarised ───────────────────────── */

static void test_transport_names(void)
{
   peer_client_message_t m;
   uint32_t status = 0;
   int transport = -1;

   /* Absent and denied and timed out are FOUR different repairs wearing one
      sentence if the code is not carried out. Each arm below is a condition a
      reader has to act on differently. */
   reset();
   g_available = 0;
   transport = -1;
   ok(peer_client_send("a", "b", "hi", NULL, 0, &m, &status, &transport) == PEER_CLIENT_TRANSPORT &&
          transport == AIMEE_MODULE_CALL_CAPABILITY_ABSENT,
      "nothing serving the stage reports capability_absent, not a bare failure");

   reset();
   g_transport = AIMEE_MODULE_CALL_DEADLINE_EXCEEDED;
   transport = -1;
   ok(peer_client_send("a", "b", "hi", NULL, 0, &m, &status, &transport) == PEER_CLIENT_TRANSPORT &&
          transport == AIMEE_MODULE_CALL_DEADLINE_EXCEEDED,
      "a module that ran out of time reports deadline_exceeded");

   reset();
   g_transport = AIMEE_MODULE_CALL_CAPABILITY_DENIED;
   transport = -1;
   ok(peer_client_send("a", "b", "hi", NULL, 0, &m, &status, &transport) == PEER_CLIENT_TRANSPORT &&
          transport == AIMEE_MODULE_CALL_CAPABILITY_DENIED,
      "a grant that denied the call reports capability_denied, not 'unreachable'");

   /* A reply that arrived and could not be read is a PROTOCOL disagreement, and
      calling it unreachable sends the reader to look at whether the module is
      running -- which it demonstrably is, since it replied. */
   reset();
   reply_set(PEER_CLIENT_STATUS_OK, ROW, PEER_CLIENT_MESSAGE_WIDTH - 1);
   transport = -1;
   ok(peer_client_send("a", "b", "hi", NULL, 0, &m, &status, &transport) == PEER_CLIENT_TRANSPORT &&
          transport == AIMEE_MODULE_CALL_PROTOCOL,
      "a reply of the wrong shape reports protocol, not an absent module");

   /* A DOMAIN refusal must leave the transport code alone: the call reached the
      module and was judged, so naming a transport failure there would invent one. */
   reset();
   reply_set(PEER_CLIENT_STATUS_DENIED, NULL, 0);
   transport = -1;
   ok(peer_client_send("a", "b", "hi", NULL, 0, &m, &status, &transport) == PEER_CLIENT_REFUSED &&
          transport == AIMEE_MODULE_CALL_OK,
      "a refusal leaves the transport code OK: the call was judged, not lost");

   /* And the names are distinct. A table where two codes share a string is a
      table that cannot tell them apart in the one place a human reads it. */
   const int codes[] = {AIMEE_MODULE_CALL_CAPABILITY_ABSENT, AIMEE_MODULE_CALL_CAPABILITY_DENIED,
                        AIMEE_MODULE_CALL_DEADLINE_EXCEEDED, AIMEE_MODULE_CALL_INTERNAL,
                        AIMEE_MODULE_CALL_PROTOCOL,          AIMEE_MODULE_CALL_TRANSPORT};
   int distinct = 1;
   for (size_t i = 0; i < sizeof codes / sizeof codes[0]; i++)
      for (size_t j = i + 1; j < sizeof codes / sizeof codes[0]; j++)
         if (strcmp(peer_client_transport_name(codes[i]), peer_client_transport_name(codes[j])) ==
             0)
            distinct = 0;
   ok(distinct, "every transport code this client reports has its OWN name");
   ok(strcmp(peer_client_transport_name(-1), "unknown") == 0,
      "a code this build does not know is 'unknown' rather than mislabelled");
}

int main(void)
{
   test_three_outcomes();
   test_transport_names();
   test_row_width();
   test_malformed();
   test_inbox();
   test_status_names();
   printf("\ntest_peer_client: all %d checks passed\n", checks);
   return 0;
}
