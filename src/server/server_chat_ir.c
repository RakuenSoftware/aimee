/* Durable session capture owned by the server compute task. */
#include "server_compute_impl.h"
#include "agent_config.h"
#include "presence.h"
#include "db1_client/server_sessions.h"
#include "db1_client/primary_sessions.h"
#include "cJSON.h"
#include <openssl/rand.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* This is a disposable subscriber view, never a source of model context. Keep
 * exact rendered prefixes by canonical message/block so a final snapshot emits
 * only text not already streamed. Conflicting source observations refuse. */
static const char *chat_ir_string(const cJSON *value, const char *key)
{
   return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(value, key));
}
static int chat_ir_legacy_text(compute_ctx_t *c, const char *id, int block, const char *kind,
                               const char *text, int delta)
{
   if (!id || !text || !text[0])
      return 0;
   if (!c->ir_legacy_blocks)
      c->ir_legacy_blocks = cJSON_CreateArray();
   if (!c->ir_legacy_blocks)
      return -1;
   cJSON *entry = NULL, *candidate;
   cJSON_ArrayForEach(candidate, c->ir_legacy_blocks)
   {
      if (!strcmp(chat_ir_string(candidate, "message"), id) &&
          cJSON_GetObjectItemCaseSensitive(candidate, "block")->valueint == block)
      {
         entry = candidate;
         break;
      }
   }
   if (!entry)
   {
      if (cJSON_GetArraySize(c->ir_legacy_blocks) >= AIMEE_IR_MAX_BLOCKS)
         return -1;
      entry = cJSON_CreateObject();
      if (!entry || !cJSON_AddStringToObject(entry, "message", id) ||
          !cJSON_AddNumberToObject(entry, "block", block) ||
          !cJSON_AddStringToObject(entry, "kind", kind) ||
          !cJSON_AddStringToObject(entry, "text", "") ||
          !cJSON_AddItemToArray(c->ir_legacy_blocks, entry))
      {
         cJSON_Delete(entry);
         return -1;
      }
   }
   if (strcmp(chat_ir_string(entry, "kind"), kind))
      return -1;
   const char *previous = chat_ir_string(entry, "text");
   size_t before = strlen(previous), length = strlen(text);
   if (!delta && (length < before || memcmp(text, previous, before)))
      return -1;
   const char *tail = delta ? text : text + before;
   size_t added = delta ? length : length - before;
   if (!added)
      return 0;
   if (added > 16 * 1024 * 1024 - c->ir_legacy_bytes)
      return -1;
   char *joined = malloc(before + added + 1);
   if (!joined)
      return -1;
   memcpy(joined, previous, before);
   memcpy(joined + before, tail, added + 1);
   cJSON *updated = cJSON_CreateString(joined);
   free(joined);
   if (!updated || !cJSON_ReplaceItemInObjectCaseSensitive(entry, "text", updated))
   {
      cJSON_Delete(updated);
      return -1;
   }
   c->ir_legacy_bytes += added;
   /* A disconnected browser remains a lost subscriber; the presence ring is
    * still updated by this producer's existing delivery callback. */
   (void)c->ir_legacy_emit(c, kind, "content", tail);
   return 0;
}
static int chat_ir_legacy_envelope(compute_ctx_t *c, const char *json)
{
   cJSON *envelope = json ? aimee_ir_json_parse_frame(json, strlen(json)) : NULL;
   const cJSON *event = cJSON_GetObjectItemCaseSensitive(envelope, "event");
   const cJSON *payload = cJSON_GetObjectItemCaseSensitive(event, "payload");
   const char *kind = chat_ir_string(event, "kind");
   const char *id = chat_ir_string(event, "message_id");
   int rc = !kind || !payload ? -1 : 0;
   if (kind && !strcmp(kind, "delta"))
   {
      const char *type = chat_ir_string(payload, "type");
      const char *block_kind = chat_ir_string(payload, "kind");
      const char *text = chat_ir_string(payload, "text_delta");
      const cJSON *block = cJSON_GetObjectItemCaseSensitive(payload, "block_id");
      if (type && (!strcmp(type, "TURN_START") || !strcmp(type, "TURN_STOP")))
         (void)c->ir_legacy_emit(c, !strcmp(type, "TURN_START") ? "turn_start" : "turn_end", NULL,
                                 NULL);
      else if (type && !strcmp(type, "ERROR"))
         (void)c->ir_legacy_emit(c, "error", "message", chat_ir_string(payload, "error_message"));
      else if (type && (!strcmp(type, "BLOCK_START") || !strcmp(type, "BLOCK_DELTA")) &&
               block_kind && (!strcmp(block_kind, "text") || !strcmp(block_kind, "thinking")))
         rc = !cJSON_IsNumber(block)
                  ? -1
                  : chat_ir_legacy_text(c, id, block->valueint, block_kind, text, 1);
   }
   else if (kind && !strcmp(kind, "message") && chat_ir_string(payload, "role") &&
            !strcmp(chat_ir_string(payload, "role"), "assistant"))
   {
      aimee_message_t message = {0};
      rc = aimee_ir_message_parse(payload, &message);
      for (int i = 0; !rc && i < message.n_blocks; i++)
      {
         const aimee_block_t *block = &message.blocks[i];
         if (block->type == AIMEE_BLK_TEXT || block->type == AIMEE_BLK_THINKING)
            rc = chat_ir_legacy_text(c, id, i, block->type == AIMEE_BLK_TEXT ? "text" : "thinking",
                                     block->text, 0);
      }
      aimee_message_free(&message);
   }
   cJSON_Delete(envelope);
   return rc;
}

static int chat_ir_write_envelope(compute_ctx_t *c, const char *json)
{
   if (!c->ir_delivery)
      return c->ir_legacy_emit ? chat_ir_legacy_envelope(c, json) : 0;
   pthread_mutex_lock(c->write_mutex);
   if (c->conn_alive &&
       (write_all(c->conn_fd, json, strlen(json)) != 0 || write_all(c->conn_fd, "\n", 1) != 0))
      c->conn_alive = 0;
   pthread_mutex_unlock(c->write_mutex);
   return 0;
}

/* Other server owners can commit checkpoints between adapter emissions. Fill
 * that gap from the same durable log so a live subscriber sees exactly the
 * sequence it will see on reload. A disconnected subscriber needs no delivery. */
static int chat_ir_deliver(compute_ctx_t *c, const char *json, const char *sequence)
{
   unsigned long long target = strtoull(sequence, NULL, 10);
   /* Gap replay can already have delivered a later callback's event. Never
    * duplicate it or rewind the live cursor when its producer catches up. */
   if (c->ir_sequence[0] && target <= strtoull(c->ir_sequence, NULL, 10))
      return 0;
   if (((c->ir_delivery && c->conn_alive) || c->ir_legacy_emit) && c->ir_sequence[0] &&
       (!json || target > strtoull(c->ir_sequence, NULL, 10) + 1))
   {
      while (strtoull(c->ir_sequence, NULL, 10) < target)
      {
         char *page = db1_session_ir_page(c->ir_session, c->ir_sequence, 128, c->vault_principal);
         cJSON *events = page ? cJSON_Parse(page) : NULL;
         free(page);
         if (!cJSON_IsArray(events) || !cJSON_GetArraySize(events))
         {
            cJSON_Delete(events);
            return -1;
         }
         const cJSON *event;
         cJSON_ArrayForEach(event, events)
         {
            const char *next =
                cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(event, "sequence"));
            if (!next || strtoull(next, NULL, 10) != strtoull(c->ir_sequence, NULL, 10) + 1 ||
                strlen(next) >= sizeof c->ir_sequence)
            {
               cJSON_Delete(events);
               return -1;
            }
            char *wire = cJSON_PrintUnformatted(event);
            if (!wire)
            {
               cJSON_Delete(events);
               return -1;
            }
            int delivered = chat_ir_write_envelope(c, wire);
            free(wire);
            if (delivered)
            {
               cJSON_Delete(events);
               return -1;
            }
            snprintf(c->ir_sequence, sizeof c->ir_sequence, "%s", next);
         }
         cJSON_Delete(events);
      }
      return 0;
   }
   if (chat_ir_write_envelope(c, json))
      return -1;
   snprintf(c->ir_sequence, sizeof c->ir_sequence, "%s", sequence);
   return 0;
}

/* Called on the producer thread. Retained callback writers never borrow the
 * browser connection; delivery reads their committed tail from the same log. */
int server_chat_ir_progress(compute_ctx_t *c)
{
   if (!c || !c->ir_enabled || (!c->ir_legacy_emit && (!c->ir_delivery || !c->conn_alive)))
      return 0;
   char sequence[32];
   if (!c->ir_sequence[0] || server_session_capture_cursor(c->ir_capture, sequence) ||
       chat_ir_deliver(c, NULL, sequence))
   {
      c->ir_failed = 1;
      server_session_capture_fail(c->ir_capture);
      return -1;
   }
   return 0;
}

/* The session log is committed before any delivery. A dead browser socket is
 * only a lost subscriber; storage failure stops capture and the host turn. */
static int chat_ir_sink(const aimee_session_event_t *event, void *context)
{
   compute_ctx_t *c = context;
   int failure_terminal = event && event->kind == AIMEE_SESSION_EXECUTION &&
                          event->execution_status &&
                          (!strcmp(event->execution_status, "failed") ||
                           !strcmp(event->execution_status, "cancelled"));
   /* A failed producer cannot publish more content or success. Its host must
    * still close the real turn through the retained owner's storage checks. */
   if (!failure_terminal &&
       (c->ir_failed || server_control_scope_failed(c->ir_controls) ||
        server_session_capture_failed(c->ir_capture) || aimee_ir_session_capture_failed()))
   {
      c->ir_failed = 1;
      return -1;
   }
   char *json = NULL, sequence[32];
   if (server_session_capture_append(c->ir_capture, event, &json, sequence))
   {
      if (!server_session_capture_operator_cancelled(c->ir_capture))
         c->ir_failed = 1;
      return -1;
   }
   int rc = chat_ir_deliver(c, json, sequence);
   free(json);
   if (rc != 0)
   {
      c->ir_failed = 1;
      server_session_capture_fail(c->ir_capture);
   }
   return rc;
}

/* Preparing a continuation is atomic from the dispatcher's perspective: failure
 * leaves the old request alone, and the caller must never dispatch it. */
int server_chat_ir_replace_steer(cJSON *request, const char *message)
{
   if (!cJSON_IsObject(request) || !message || !message[0])
      return -1;
   cJSON *replacement = cJSON_CreateString(message);
   if (!replacement)
      return -1;
   if (!cJSON_ReplaceItemInObjectCaseSensitive(request, "message", replacement))
   {
      cJSON_Delete(replacement);
      return -1;
   }
   return 0;
}

/* Execution diagnostics never become user-message history. Retain a bounded,
 * escaped observation of the rejected input without authorizing its execution. */
int server_chat_ir_reject_steer(const char *session, const char *actor, const char *message)
{
   static const char reason[] = "Steering continuation was not dispatched; no model or tool "
                                "executed. Send it again explicitly.";
   db1_server_session_t registered;
   if (!session || !session[0] || !actor || !actor[0] || !message || !message[0] ||
       db1_server_session_get(session, &registered) || strcmp(registered.principal, actor))
      return -1;
   size_t length = strlen(message), preview_length = length < 1536 ? length : 1536;
   while (preview_length && ((unsigned char)message[preview_length] & 0xc0) == 0x80)
      --preview_length;
   char preview[1537], digest[65];
   memcpy(preview, message, preview_length);
   preview[preview_length] = 0;
   unsigned char hash[EVP_MAX_MD_SIZE];
   unsigned int hash_length = 0;
   if (!EVP_Digest(message, length, hash, &hash_length, EVP_sha256(), NULL) || hash_length != 32)
      return -1;
   for (size_t i = 0; i < 32; i++)
      snprintf(digest + 2 * i, 3, "%02x", hash[i]);
   cJSON *diagnostic = cJSON_CreateObject();
   char *failure = NULL;
   if (diagnostic && cJSON_AddStringToObject(diagnostic, "reason", reason) &&
       cJSON_AddBoolToObject(diagnostic, "dispatched", 0) &&
       cJSON_AddStringToObject(diagnostic, "submitted_input_sha256", digest) &&
       cJSON_AddNumberToObject(diagnostic, "submitted_input_bytes", (double)length) &&
       cJSON_AddStringToObject(diagnostic, "submitted_input_preview", preview) &&
       cJSON_AddBoolToObject(diagnostic, "submitted_input_truncated", preview_length < length))
      failure = cJSON_PrintUnformatted(diagnostic);
   cJSON_Delete(diagnostic);
   if (!failure)
      return -1;
   unsigned char random[16];
   char turn[33];
   if (RAND_bytes(random, sizeof random) != 1)
   {
      free(failure);
      return -1;
   }
   for (size_t i = 0; i < sizeof random; i++)
      snprintf(turn + 2 * i, 3, "%02x", random[i]);
   server_session_capture_t *capture = server_session_capture_open(session, turn, actor, session);
   if (!capture)
   {
      free(failure);
      return -1;
   }
   aimee_session_event_t running = {.kind = AIMEE_SESSION_EXECUTION, .execution_status = "running"};
   aimee_session_event_t failed = {
       .kind = AIMEE_SESSION_EXECUTION, .execution_status = "failed", .execution_error = failure};
   int result = server_session_capture_append(capture, &running, NULL, NULL);
   int terminal = server_session_capture_append(capture, &failed, NULL, NULL);
   server_session_capture_close(capture);
   free(failure);
   return result || terminal ? -1 : 0;
}

int server_chat_ir_prepare_turn(compute_ctx_t *c)
{
   if (!c)
      return -1;
   if (c->ir_turn[0])
      return strlen(c->ir_turn) == 32 && strspn(c->ir_turn, "0123456789abcdef") == 32 ? 0 : -1;
   unsigned char random[16];
   if (RAND_bytes(random, sizeof random) != 1)
      return -1;
   for (size_t i = 0; i < sizeof random; i++)
      snprintf(c->ir_turn + 2 * i, 3, "%02x", random[i]);
   return 0;
}

int server_chat_ir_begin(compute_ctx_t *c, const char *session, const char *text)
{
   if (!c->presence_locked || !session || !session[0] || strlen(session) >= sizeof(c->ir_session) ||
       !c->vault_principal[0])
      return -1;
   if (!presence_turn_host_own(c->presence_session, c->presence_turn_id))
      return -1;
   db1_server_session_t row;
   int found = db1_server_session_get(session, &row);
   if (found == 0 && strcmp(row.principal, c->vault_principal))
      return -1;
   if (found != 0 && db1_server_session_create(session, "webchat", c->vault_principal) != 0)
      return -1;
   snprintf(c->ir_session, sizeof(c->ir_session), "%s", session);
   /* Async admission already minted this host identity before publishing the
    * cancel registry. Direct history operations mint it here exactly once. */
   if (server_chat_ir_prepare_turn(c))
      return -1;
   c->ir_controls = server_control_scope_open(c->ir_session, c->ir_turn, c->vault_principal);
   if (!c->ir_controls)
      return -1;
   c->ir_previous_controls = server_control_scope_bind(c->ir_controls);
   c->ir_capture =
       server_session_capture_open(c->ir_session, c->ir_turn, c->vault_principal, c->ir_session);
   if (!c->ir_capture)
   {
      server_control_scope_bind(c->ir_previous_controls);
      server_control_scope_close(c->ir_controls);
      c->ir_controls = NULL;
      return -1;
   }
   c->ir_previous_capture = server_session_capture_bind(c->ir_capture);
   c->ir_previous_sink = aimee_ir_session_get_sink(&c->ir_previous_context);
   c->ir_enabled = 1;
   aimee_ir_session_set_sink(chat_ir_sink, c);
   aimee_session_event_t lifecycle = {.kind = AIMEE_SESSION_EXECUTION,
                                      .execution_status = "running"};
   if (aimee_ir_session_emit(&lifecycle) != 0)
      return -1;
   if (!text)
      return 0; /* A host history operation has no model/user message to invent. */
   aimee_block_t block = {.type = AIMEE_BLK_TEXT, .text = (char *)text};
   aimee_message_t message = {.role = "user", .blocks = &block, .n_blocks = 1};
   aimee_session_event_t event = {
       .kind = AIMEE_SESSION_MESSAGE, .message_id = "user", .message = &message};
   return aimee_ir_session_emit(&event);
}

int server_chat_ir_finish(compute_ctx_t *c, const char *error)
{
   if (!c->ir_enabled || c->ir_completed)
      return 0;
   c->ir_completed = 1;
   if (server_session_capture_operator_cancelled(c->ir_capture))
      return server_chat_ir_progress(c);
   server_session_capture_counts_t counts = {0};
   if (server_session_capture_counts(c->ir_capture, &counts))
      error = "canonical capture owner unavailable";
   if (!error &&
       (c->ir_failed || server_control_scope_failed(c->ir_controls) ||
        server_session_capture_failed(c->ir_capture) || aimee_ir_session_capture_failed()))
      error = "canonical capture failed";
   if (!error && !agent_request_cancelled() && !counts.model_events && !c->ir_history_checkpointed)
      error = "Selected harness produced no canonical model output";
   aimee_session_event_t event = {.kind = AIMEE_SESSION_EXECUTION,
                                  .execution_status = agent_request_cancelled()
                                                          ? "cancelled"
                                                          : (error ? "failed" : "completed"),
                                  .execution_error = error};
   int result = aimee_ir_session_emit(&event);
   if (result && server_session_capture_operator_cancelled(c->ir_capture))
      return server_chat_ir_progress(c);
   return result;
}

void server_chat_ir_close(compute_ctx_t *c)
{
   if (!c->ir_enabled)
      return;
   if (!c->ir_completed)
      server_chat_ir_finish(c, "execution ended without completion");
   aimee_ir_session_set_sink(c->ir_previous_sink, c->ir_previous_context);
   server_session_capture_bind(c->ir_previous_capture);
   server_session_capture_close(c->ir_capture);
   server_control_scope_bind(c->ir_previous_controls);
   server_control_scope_close(c->ir_controls);
   c->ir_capture = NULL;
   c->ir_controls = NULL;
   cJSON_Delete(c->ir_legacy_blocks);
   c->ir_legacy_blocks = NULL;
   c->ir_legacy_emit = NULL;
   c->ir_legacy_bytes = 0;
   c->ir_enabled = 0;
}
