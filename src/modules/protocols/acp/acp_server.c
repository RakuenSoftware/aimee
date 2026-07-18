/* acp_server.c: inbound ACP server (`aimee acp-serve`). See acp_server.h.
 *
 * Phase 1: newline-delimited JSON-RPC 2.0 over stdio + the `initialize`
 * handshake advertising aimee's capabilities. Stdout carries ACP JSON-RPC only
 * (one message per line); logs/diagnostics belong on stderr. */
#include "acp_server.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef AIMEE_VERSION
#define AIMEE_VERSION "dev"
#endif

/* ACP slash commands aimee advertises (turn dispatch wires them in phase 2). */
static const char *const ACP_SLASH_COMMANDS[] = {"/help",  "/skill", "/personality", "/compact",
                                                 "/reset", "/stop",  "/queue"};
#define ACP_SLASH_COMMAND_COUNT ((int)(sizeof(ACP_SLASH_COMMANDS) / sizeof(ACP_SLASH_COMMANDS[0])))

/* Serialize `root` into `out` (NUL-terminated). Returns 1 on success, -1 else.
 * Takes ownership of `root` (deletes it). */
static int acp_emit(cJSON *root, char *out, size_t out_n)
{
   int rc = -1;
   char *s = cJSON_PrintUnformatted(root);
   if (s)
   {
      if (out && out_n)
         snprintf(out, out_n, "%s", s);
      free(s);
      rc = 1;
   }
   else if (out && out_n)
      out[0] = '\0';
   cJSON_Delete(root);
   return rc;
}

/* Attach the JSON-RPC id (a detached cJSON, or null) to a response object. */
static void acp_add_id(cJSON *root, cJSON *id)
{
   cJSON_AddItemToObject(root, "id", id ? id : cJSON_CreateNull());
}

static int acp_error(cJSON *id, int code, const char *message, char *out, size_t out_n)
{
   cJSON *root = cJSON_CreateObject();
   cJSON_AddStringToObject(root, "jsonrpc", "2.0");
   acp_add_id(root, id);
   cJSON *err = cJSON_AddObjectToObject(root, "error");
   cJSON_AddNumberToObject(err, "code", code);
   cJSON_AddStringToObject(err, "message", message ? message : "error");
   return acp_emit(root, out, out_n);
}

/* Build the initialize result for an already-detached id cJSON (or NULL). */
static int acp_initialize_for_id(cJSON *id, char *out, size_t out_n)
{
   cJSON *root = cJSON_CreateObject();
   cJSON_AddStringToObject(root, "jsonrpc", "2.0");
   acp_add_id(root, id);
   cJSON *result = cJSON_AddObjectToObject(root, "result");
   cJSON_AddNumberToObject(result, "protocolVersion", 1);
   /* Standard ACP initialize result: agentCapabilities + authMethods. */
   cJSON *acaps = cJSON_AddObjectToObject(result, "agentCapabilities");
   cJSON_AddBoolToObject(acaps, "loadSession", 1); /* session/load resumes by id */
   cJSON *pcaps = cJSON_AddObjectToObject(acaps, "promptCapabilities");
   cJSON_AddBoolToObject(pcaps, "image", 0);
   cJSON_AddBoolToObject(pcaps, "audio", 0);
   cJSON_AddBoolToObject(pcaps, "embeddedContext", 1);
   cJSON_AddArrayToObject(result, "authMethods"); /* none: stdio is trusted */
   cJSON *info = cJSON_AddObjectToObject(result, "agentInfo");
   cJSON_AddStringToObject(info, "name", "aimee");
   cJSON_AddStringToObject(info, "version", AIMEE_VERSION);
   /* Non-standard convenience: the local slash commands aimee answers. */
   cJSON *cmds = cJSON_AddArrayToObject(result, "slashCommands");
   for (int i = 0; i < ACP_SLASH_COMMAND_COUNT; i++)
      cJSON_AddItemToArray(cmds, cJSON_CreateString(ACP_SLASH_COMMANDS[i]));
   return acp_emit(root, out, out_n);
}

int acp_server_build_initialize(const char *id_json, char *out, size_t out_n)
{
   cJSON *id = (id_json && id_json[0]) ? cJSON_Parse(id_json) : NULL;
   return acp_initialize_for_id(id, out, out_n);
}

/* session/new: mint a fresh ACP session id. The chat turn that follows persists
 * to DB1 under this id (server_sessions + primary_sessions transcript), so the id
 * must be unique across acp-serve processes — a bare per-process counter reset to
 * 1 on every restart, colliding with a prior (e.g. crashed) session's DB rows and
 * making it unrecoverable. Qualify it with the pid and process start time; the
 * editor stores the returned id and replays it on session/load to resume. */
static int acp_session_new_for_id(cJSON *id, char *out, size_t out_n)
{
   /* acp_server_run drives session/new from a single stdin loop, so this static
    * seq is never touched concurrently. Seed the id with the process start time to
    * nanosecond resolution (not whole seconds) so two acp-serve processes that
    * reuse a pid can't collide on the same second. */
   static unsigned long seq = 0;
   static long long proc_seed = 0;
   if (proc_seed == 0)
   {
      struct timespec ts;
      proc_seed = (clock_gettime(CLOCK_REALTIME, &ts) == 0)
                      ? (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec
                      : (long long)time(NULL);
   }
   char sid[64];
   snprintf(sid, sizeof(sid), "acp-%d-%lld-%lu", (int)getpid(), proc_seed, ++seq);
   cJSON *root = cJSON_CreateObject();
   cJSON_AddStringToObject(root, "jsonrpc", "2.0");
   acp_add_id(root, id);
   cJSON *result = cJSON_AddObjectToObject(root, "result");
   cJSON_AddStringToObject(result, "sessionId", sid);
   return acp_emit(root, out, out_n);
}

static int acp_session_load(cJSON *id, const cJSON *params, char *out, size_t out_n);

/* Is `s` empty or whitespace-only? Such lines carry no JSON-RPC message. */
static int acp_line_blank(const char *s)
{
   if (!s)
      return 1;
   for (; *s; s++)
      if (*s != ' ' && *s != '\t' && *s != '\r' && *s != '\n')
         return 0;
   return 1;
}

int acp_server_handle_line(const char *line, char *out, size_t out_n)
{
   if (out && out_n)
      out[0] = '\0';
   if (acp_line_blank(line))
      return 0;

   cJSON *req = cJSON_Parse(line);
   if (!req)
      return acp_error(NULL, -32700, "parse error", out, out_n);

   /* Detach the id so it can be re-emitted in the response verbatim. */
   cJSON *id = cJSON_DetachItemFromObjectCaseSensitive(req, "id");
   const cJSON *method = cJSON_GetObjectItemCaseSensitive(req, "method");
   int has_id = id != NULL && !cJSON_IsNull(id);

   int rc;
   if (!cJSON_IsString(method) || !method->valuestring[0])
   {
      /* Malformed request. Respond only if it had an id (else it's noise). */
      if (has_id)
      {
         rc = acp_error(id, -32600, "invalid request", out, out_n);
      }
      else
      {
         cJSON_Delete(id);
         rc = 0;
      }
   }
   else if (strcmp(method->valuestring, "initialize") == 0)
   {
      rc = acp_initialize_for_id(id, out, out_n);
   }
   else if (strcmp(method->valuestring, "session/new") == 0 && has_id)
   {
      rc = acp_session_new_for_id(id, out, out_n);
   }
   else if (strcmp(method->valuestring, "session/load") == 0 && has_id)
   {
      rc = acp_session_load(id, cJSON_GetObjectItemCaseSensitive(req, "params"), out, out_n);
   }
   else if (!has_id)
   {
      /* Notification (no id) for an unhandled method: silently accept. */
      cJSON_Delete(id);
      rc = 0;
   }
   else
   {
      char msg[160];
      snprintf(msg, sizeof(msg), "method not found: %s", method->valuestring);
      rc = acp_error(id, -32601, msg, out, out_n);
   }

   cJSON_Delete(req);
   return rc;
}

int acp_prompt_parse(const char *line, char *content, size_t content_n, char *session_id,
                     size_t session_n, char *id_json, size_t id_n)
{
   if (content && content_n)
      content[0] = '\0';
   if (session_id && session_n)
      session_id[0] = '\0';
   if (id_json && id_n)
      id_json[0] = '\0';
   if (acp_line_blank(line))
      return 0;

   cJSON *req = cJSON_Parse(line);
   if (!req)
      return 0;
   const cJSON *method = cJSON_GetObjectItemCaseSensitive(req, "method");
   int kind = 0; /* 1 = legacy prompt/submit, 2 = standard session/prompt */
   if (cJSON_IsString(method) && strcmp(method->valuestring, "prompt/submit") == 0)
      kind = 1;
   else if (cJSON_IsString(method) && strcmp(method->valuestring, "session/prompt") == 0)
      kind = 2;
   if (!kind)
   {
      cJSON_Delete(req);
      return 0;
   }

   const cJSON *params = cJSON_GetObjectItemCaseSensitive(req, "params");
   if (kind == 1)
   {
      const cJSON *jc = params ? cJSON_GetObjectItemCaseSensitive(params, "content") : NULL;
      if (cJSON_IsString(jc) && content && content_n)
         snprintf(content, content_n, "%s", jc->valuestring);
   }
   else if (content && content_n)
   {
      /* session/prompt: params.prompt is an array of content blocks; concatenate
       * the text of the {type:"text",text} blocks into one prompt string. */
      const cJSON *arr = params ? cJSON_GetObjectItemCaseSensitive(params, "prompt") : NULL;
      content[0] = '\0';
      size_t off = 0;
      const cJSON *blk = NULL;
      if (cJSON_IsArray(arr))
         cJSON_ArrayForEach(blk, arr)
         {
            const cJSON *t = cJSON_GetObjectItemCaseSensitive(blk, "text");
            if (cJSON_IsString(t) && off + 1 < content_n)
               off += (size_t)snprintf(content + off, content_n - off, "%s", t->valuestring);
         }
   }
   const cJSON *js = params ? cJSON_GetObjectItemCaseSensitive(params, "sessionId") : NULL;
   if (cJSON_IsString(js) && session_id && session_n)
      snprintf(session_id, session_n, "%s", js->valuestring);

   const cJSON *id = cJSON_GetObjectItemCaseSensitive(req, "id");
   if (id && !cJSON_IsNull(id) && id_json && id_n)
   {
      char *s = cJSON_PrintUnformatted(id);
      if (s)
      {
         snprintf(id_json, id_n, "%s", s);
         free(s);
      }
   }
   cJSON_Delete(req);
   return kind;
}

int acp_build_prompt_result(const char *id_json, const char *reply, char *out, size_t out_n)
{
   cJSON *id = (id_json && id_json[0]) ? cJSON_Parse(id_json) : NULL;
   cJSON *root = cJSON_CreateObject();
   cJSON_AddStringToObject(root, "jsonrpc", "2.0");
   acp_add_id(root, id);
   cJSON *result = cJSON_AddObjectToObject(root, "result");
   cJSON_AddStringToObject(result, "content", reply ? reply : "");
   return acp_emit(root, out, out_n);
}

/* Standard ACP session/prompt response: the reply text was streamed via
 * session/update, so the result just carries the stop reason. */
int acp_build_prompt_stop(const char *id_json, const char *stop_reason, char *out, size_t out_n)
{
   cJSON *id = (id_json && id_json[0]) ? cJSON_Parse(id_json) : NULL;
   cJSON *root = cJSON_CreateObject();
   cJSON_AddStringToObject(root, "jsonrpc", "2.0");
   acp_add_id(root, id);
   cJSON *result = cJSON_AddObjectToObject(root, "result");
   cJSON_AddStringToObject(result, "stopReason", stop_reason ? stop_reason : "end_turn");
   return acp_emit(root, out, out_n);
}

int acp_slash_reply(const char *content, char *reply, size_t reply_n)
{
   if (reply && reply_n)
      reply[0] = '\0';
   if (!content)
      return 0;
   while (*content == ' ' || *content == '\t')
      content++;
   /* Only `/help` is answered locally so far; other slash commands fall through
    * to a normal turn until their server actions are wired (phase 5). */
   if (strncmp(content, "/help", 5) != 0)
      return 0;
   char tail = content[5];
   if (tail && tail != ' ' && tail != '\t' && tail != '\r' && tail != '\n')
      return 0;

   size_t off = 0;
   if (reply && reply_n)
      off += (size_t)snprintf(reply + off, reply_n - off, "aimee ACP slash commands:\n");
   for (int i = 0; i < ACP_SLASH_COMMAND_COUNT && reply && off < reply_n; i++)
      off += (size_t)snprintf(reply + off, reply_n - off, "  %s\n", ACP_SLASH_COMMANDS[i]);
   return 1;
}

int acp_build_update_notification(const char *session_id, const char *delta, char *out,
                                  size_t out_n)
{
   cJSON *root = cJSON_CreateObject();
   cJSON_AddStringToObject(root, "jsonrpc", "2.0");
   cJSON_AddStringToObject(root, "method", "session/update");
   cJSON *params = cJSON_AddObjectToObject(root, "params");
   cJSON_AddStringToObject(params, "sessionId", session_id ? session_id : "");
   cJSON *update = cJSON_AddObjectToObject(params, "update");
   /* Standard ACP: discriminated by sessionUpdate, content is a typed block. */
   cJSON_AddStringToObject(update, "sessionUpdate", "agent_message_chunk");
   cJSON *block = cJSON_AddObjectToObject(update, "content");
   cJSON_AddStringToObject(block, "type", "text");
   cJSON_AddStringToObject(block, "text", delta ? delta : "");
   return acp_emit(root, out, out_n);
}

/* Build a standard ACP tool-call session/update notification. `phase` is
 * "started" (→ sessionUpdate "tool_call", status "in_progress") or "completed"
 * (→ "tool_call_update", status "completed").
 *
 * `tool_call_id` must be unique WITHIN THE SESSION: it is the handle a client uses
 * to match a tool_call_update to the tool_call it updates. It used to be the tool
 * NAME, which is not unique — an agent that reads three files emitted three calls
 * sharing one id, and a client folds those into a single entry that flips between
 * in_progress and completed as each one lands. The caller numbers them; this stays
 * pure. `tool_name` remains the human-facing title.
 *
 * Pure. Returns 1. */
int acp_build_tool_update(const char *session_id, const char *tool_call_id, const char *phase,
                          const char *tool_name, char *out, size_t out_n)
{
   int completed = (phase && strcmp(phase, "completed") == 0);
   cJSON *root = cJSON_CreateObject();
   cJSON_AddStringToObject(root, "jsonrpc", "2.0");
   cJSON_AddStringToObject(root, "method", "session/update");
   cJSON *params = cJSON_AddObjectToObject(root, "params");
   cJSON_AddStringToObject(params, "sessionId", session_id ? session_id : "");
   cJSON *update = cJSON_AddObjectToObject(params, "update");
   cJSON_AddStringToObject(update, "sessionUpdate", completed ? "tool_call_update" : "tool_call");
   cJSON_AddStringToObject(update, "toolCallId",
                           (tool_call_id && tool_call_id[0]) ? tool_call_id
                                                             : (tool_name ? tool_name : ""));
   if (!completed)
      cJSON_AddStringToObject(update, "title", tool_name ? tool_name : "");
   cJSON_AddStringToObject(update, "status", completed ? "completed" : "in_progress");
   return acp_emit(root, out, out_n);
}

/* session/load: an editor resuming an existing session by id. aimee sessions
 * live server-side keyed by id, so loading just acknowledges the id (subsequent
 * prompt/submit with it continues the same server session). */
static int acp_session_load(cJSON *id, const cJSON *params, char *out, size_t out_n)
{
   const cJSON *sid = params ? cJSON_GetObjectItemCaseSensitive(params, "sessionId") : NULL;
   cJSON *root = cJSON_CreateObject();
   cJSON_AddStringToObject(root, "jsonrpc", "2.0");
   acp_add_id(root, id);
   cJSON *result = cJSON_AddObjectToObject(root, "result");
   cJSON_AddStringToObject(result, "sessionId",
                           (cJSON_IsString(sid) && sid->valuestring[0]) ? sid->valuestring : "");
   return acp_emit(root, out, out_n);
}

int acp_server_run(acp_prompt_dispatch_fn dispatch)
{
   /* ACP messages are newline-delimited; keep the line buffer generous so a
    * prompt payload fits on one line (matches the outbound client's framing). */
   static char line[1 << 18]; /* 256 KiB */
   static char content[1 << 18];
   char session[256];
   char id_json[256];
   char out[1 << 16];
   while (fgets(line, sizeof(line), stdin))
   {
      /* kind: 0 = not a prompt, 1 = legacy prompt/submit (result.content),
       *       2 = standard session/prompt (text streamed, result.stopReason). */
      int kind = acp_prompt_parse(line, content, sizeof(content), session, sizeof(session), id_json,
                                  sizeof(id_json));
      if (kind)
      {
         char slash[1024];
         if (acp_slash_reply(content, slash, sizeof(slash)))
         {
            /* Answered locally (e.g. /help) — no model turn. For standard ACP,
             * stream the text then close with stopReason; legacy returns it as
             * the result content. */
            if (kind == 2)
            {
               char upd[2048];
               acp_build_update_notification(session[0] ? session : "", slash, upd, sizeof(upd));
               fputs(upd, stdout);
               fputc('\n', stdout);
               acp_build_prompt_stop(id_json, "end_turn", out, sizeof(out));
            }
            else
               acp_build_prompt_result(id_json, slash, out, sizeof(out));
            fputs(out, stdout);
            fputc('\n', stdout);
            fflush(stdout);
            continue;
         }
         char *reply = dispatch ? dispatch(content, session[0] ? session : NULL) : NULL;
         if (reply)
         {
            /* The dispatch streams the reply via session/update as it runs. A
             * standard client reads it there and only needs the stopReason; a
             * legacy client gets the full text back in result.content. */
            if (kind == 2)
               acp_build_prompt_stop(id_json, "end_turn", out, sizeof(out));
            else
               acp_build_prompt_result(id_json, reply, out, sizeof(out));
            free(reply);
         }
         else
         {
            cJSON *id = id_json[0] ? cJSON_Parse(id_json) : NULL;
            acp_error(id, -32000, "turn dispatch failed", out, sizeof(out));
         }
         fputs(out, stdout);
         fputc('\n', stdout);
         fflush(stdout);
         continue;
      }
      if (acp_server_handle_line(line, out, sizeof(out)) == 1 && out[0])
      {
         fputs(out, stdout);
         fputc('\n', stdout);
         fflush(stdout);
      }
   }
   return 0;
}
