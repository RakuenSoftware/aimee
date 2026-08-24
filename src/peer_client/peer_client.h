/* peer_client.h: the server's side of peer messaging.
 *
 * The aimee module (principal ref 31) serves four stages and, until this file
 * existed, NOTHING in the product called any of them. The module was validated
 * on hardware by a probe binary written for the purpose, which is a test of the
 * engine and not of the feature: no agent had ever sent a message to another
 * agent, because there was no way for one to ask.
 *
 * This is that way. It speaks db1-fields-v2 -- the same counted-fields wire the
 * store uses -- over obs_bus_module_call, and it is deliberately thin: no
 * retries, no caching, no queue. The registry already owns delivery semantics,
 * and a second opinion here would be a second place for them to be wrong. */
#ifndef AIMEE_PEER_CLIENT_H
#define AIMEE_PEER_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* THREE outcomes, not two, and the distinction is the whole point.
    *
    * A transport failure means the module did not answer: unreachable, timed
    * out, or answered something this client could not parse. A refusal means it
    * answered, understood, and said no -- and `status` names which no. Folding
    * those together is what makes an unreachable module look like a rejected
    * message, and it is the exact defect this client was written to avoid: the
    * caller retries one and must not retry the other. */
   typedef enum
   {
      PEER_CLIENT_OK = 0,
      PEER_CLIENT_REFUSED = 1,
      PEER_CLIENT_TRANSPORT = -1,
   } peer_client_result_t;

   /* peerwire.Status. These integers cross the bus, so they are transcribed
    * explicitly -- C cannot import Go -- and the transcription is pinned by
    * peerwire/cwire_test.go, which READS THIS FILE and compares.
    *
    * The pin lives on the Go side deliberately. A check that can see only one
    * of two copies pins a value to itself; only the side that can see both can
    * tell them apart. Drift here is silent in the worst way -- every refusal
    * still arrives, each one named as a different refusal than was sent. */
   enum
   {
      PEER_CLIENT_STATUS_OK = 0,
      PEER_CLIENT_STATUS_NO_PEER = 1,
      PEER_CLIENT_STATUS_DENIED = 2,
      PEER_CLIENT_STATUS_INBOX_FULL = 3,
      PEER_CLIENT_STATUS_HOP_LIMIT = 4,
      PEER_CLIENT_STATUS_CYCLE = 5,
      PEER_CLIENT_STATUS_TIMEOUT = 6,
      PEER_CLIENT_STATUS_SELF = 7,
      PEER_CLIENT_STATUS_TOO_LONG = 8,
      PEER_CLIENT_STATUS_LABEL_TAKEN = 9,
      PEER_CLIENT_STATUS_UNKNOWN_SENDER = 10,
      PEER_CLIENT_STATUS_BAD_REQUEST = 11,
      PEER_CLIENT_STATUS_SHUTDOWN = 12,
      PEER_CLIENT_STATUS_NO_CHANNEL = 13,
      PEER_CLIENT_STATUS_NOT_MEMBER = 14,
      PEER_CLIENT_STATUS_CHANNEL_FULL = 15,
      PEER_CLIENT_STATUS_UNAVAILABLE = 16,
      PEER_CLIENT_STATUS_UNCLASSIFIED = 17,
      PEER_CLIENT_STATUS_AT_CAPACITY = 18,
      PEER_CLIENT_STATUS_NO_DIRECTORY = 19,
      PEER_CLIENT_STATUS_DIRECTORY_REFUSED = 20,
      PEER_CLIENT_STATUS_COUNT = 21
   };

   /* peerwire.MessageWidth. Cells APPEND, so a reader built against a narrower
    * width keeps its field numbering; this client refuses a row that is not
    * exactly this wide rather than reading a prefix of a wider one, because a
    * short row and a truncated row are indistinguishable once accepted. */
#define PEER_CLIENT_MESSAGE_WIDTH 11

   typedef struct
   {
      char *id;
      char *correlation_id;
      char *conversation_id;
      char *from_session;
      char *from_owner;
      char *from_label;
      char *origin_session;
      int hop;
      int is_reply;
      char *sent_at;
      char *text;
   } peer_client_message_t;

   /* Send `text` from `from` to `to`. On PEER_CLIENT_OK, `stamped` (when not
    * NULL) receives the module's re-stamped envelope -- its provenance, not the
    * caller's claim about it -- and the caller frees it with
    * peer_client_message_free. On PEER_CLIENT_REFUSED, *status names the no. */
   peer_client_result_t peer_client_send(const char *from, const char *to, const char *text,
                                         const char *conversation_id, int expect_reply,
                                         peer_client_message_t *stamped, uint32_t *status);

   /* PEER_CLIENT_INBOX_TAKE_MAX bounds ONE drain, not an inbox.
    *
    * The reply is a single bus message whose size this client must predict to
    * size its buffer, and 32 messages of 8KB each do not fit a prediction worth
    * making. The protocol already answers this: a take reply leads with how many
    * REMAIN, so a caller that wants everything calls again until that is zero.
    * Capping the ask is therefore not a limitation, it is using the field the
    * wire provides instead of allocating for a worst case that never arrives. */
#define PEER_CLIENT_INBOX_TAKE_MAX 8

   /* Drain up to `max` messages for `session` (clamped to
    * PEER_CLIENT_INBOX_TAKE_MAX). `out`/`count` receive a heap array the caller
    * frees with peer_client_messages_free; `remaining` receives how many are
    * still waiting, which is the only way to tell "that was all of it" from
    * "that was the first max of more". */
   peer_client_result_t peer_client_inbox_take(const char *session, int max,
                                               peer_client_message_t **out, size_t *count,
                                               int *remaining, uint32_t *status);

   /* How many are waiting, and how many were dropped on overflow. Answering
    * zero is a legitimate success: an empty inbox is an answer, not a failure. */
   peer_client_result_t peer_client_inbox_len(const char *session, int *waiting, int *dropped,
                                              uint32_t *status);

   void peer_client_message_free(peer_client_message_t *m);
   void peer_client_messages_free(peer_client_message_t *m, size_t count);

   /* A stable name for a status, for the text a caller shows a human. Returns
    * "unknown" for a status this build does not know -- which is a status the
    * MODULE added, so naming it "unclassified" would claim the module said
    * something it did not. */
   const char *peer_client_status_name(uint32_t status);

   /* Is anything serving peer delivery on this daemon's bus? A local check
    * against the bus registry, not a probe: with no module attached there is no
    * call to make, and saying so beats waiting out a deadline. */
   int peer_client_available(void);

#ifdef __cplusplus
}
#endif

#endif /* AIMEE_PEER_CLIENT_H */
