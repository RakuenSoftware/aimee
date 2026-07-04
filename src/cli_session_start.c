/* cli_session_start.c: the `aimee session-start` SessionStart-hook entry,
 * extracted from cli_main.c. Forwards hooks.session_start to a co-located
 * server, or — against a remote /v1 endpoint — emits proactive recall directly
 * (POST /v1/memory/recall) so a thin client needs no local server. Keeping it in
 * its own TU holds cli_main.c under the source line limit. */
#include "cli_client.h"
#include "cli_session_start.h"
#include "harness_memory_hydrate.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <unistd.h>

struct ss_sbuf
{
   char *p;
   size_t cap, len;
};
static void ss_add(struct ss_sbuf *b, const char *s)
{
   if (!s || !s[0])
      return;
   size_t n = strlen(s);
   if (b->len + n + 1 > b->cap)
   {
      size_t nc = b->cap ? b->cap : 1024;
      while (nc < b->len + n + 1)
         nc *= 2;
      char *np = realloc(b->p, nc);
      if (!np)
         return;
      b->p = np;
      b->cap = nc;
   }
   memcpy(b->p + b->len, s, n);
   b->len += n;
   b->p[b->len] = '\0';
}

/* Render one recall section ([{title,description}|{text}]) as markdown. */
static void ss_render_section(struct ss_sbuf *b, const char *title, cJSON *arr)
{
   if (!cJSON_IsArray(arr) || cJSON_GetArraySize(arr) == 0)
      return;
   ss_add(b, "## ");
   ss_add(b, title);
   ss_add(b, "\n");
   cJSON *it = NULL;
   cJSON_ArrayForEach(it, arr)
   {
      const char *t = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(it, "title"));
      const char *d = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(it, "description"));
      if (!t)
         t = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(it, "text"));
      if (!t && !d)
         continue;
      ss_add(b, "- ");
      if (t)
         ss_add(b, t);
      if (d && d[0])
      {
         ss_add(b, ": ");
         ss_add(b, d);
      }
      ss_add(b, "\n");
   }
   ss_add(b, "\n");
}

/* Thin-client SessionStart fallback. When there is no co-located aimee-server
 * but a remote /v1 endpoint is configured, the thin client serves session-start
 * itself: it fetches proactive recall (read-only data-plane: POST
 * /v1/memory/recall, served by any aimee — local or the shared NAS kb) and emits
 * it as the hook's additionalContext. The execution-plane parts of the
 * server-side hook (git pull --ff-only, session_state writes) need a co-located
 * server and are intentionally skipped here; proactive recall is the injectable
 * value. Always soft-fails (exit 0) so the host session is never blocked. */
/* SessionStart recall retry policy. Total worst case ~= the old single 30s
 * shot: SESSION_START_RECALL_ATTEMPTS * per-attempt timeout + linear backoff.
 * Per-attempt timeout stays generous enough not to trip a slow-but-healthy
 * server (a 12s recall on a cold provider still lands on the first attempt). */
#define SESSION_START_RECALL_ATTEMPTS   3
#define SESSION_START_RECALL_TIMEOUT_MS 15000
#define SESSION_START_RECALL_BACKOFF_US 400000 /* multiplied by attempt: 0.4s, 0.8s */

/* Retried POST for the SessionStart hook. It fires exactly once and soft-fails
 * silently, so one transient failure would otherwise leave the session with no
 * context. Only transport failures (resp==NULL) and 5xx are retried — a 4xx is
 * deterministic (bad bearer/body/route). Bounded so a genuinely-down server
 * never blocks the prompt for long. Returns the first 200 response (ownership
 * passes to the caller) or NULL; on failure the last response is freed here. */
static cJSON *ss_retry_post(const char *endpoint, const char *bearer, const char *path,
                            const char *body_s)
{
   cJSON *resp = NULL;
   for (int attempt = 0; attempt < SESSION_START_RECALL_ATTEMPTS; attempt++)
   {
      if (attempt > 0)
         usleep(SESSION_START_RECALL_BACKOFF_US * attempt);
      int status = 0;
      resp = cli_http_request(endpoint, "POST", path, body_s, bearer,
                              SESSION_START_RECALL_TIMEOUT_MS, &status);
      if (status == 200)
         return resp; /* success */
      int retryable = (resp == NULL) || (status >= 500);
      cJSON_Delete(resp);
      resp = NULL;
      if (!retryable)
         break; /* deterministic client error — no point retrying */
   }
   return NULL;
}

static int handle_session_start_remote(void)
{
   char *endpoint = cli_v1_client_endpoint();
   if (!endpoint)
      return 0;
   char *bearer = cli_v1_client_bearer();

   struct ss_sbuf ctx = {0};

   /* 1) The full workspace-independent brief (persona principles + learned
    * Rules + key facts) from session.brief_assemble. This is the primary
    * payload and the never-empty floor: the server always emits at least
    * persona principles, so a fresh session is never left with an empty brief
    * (the pre-Phase-1 behaviour when recall was empty). The endpoint takes no
    * input; send an empty object. */
   cJSON *brief = ss_retry_post(endpoint, bearer, "/v1/session/brief_assemble", "{}");
   if (brief)
   {
      const char *out = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(brief, "output"));
      if (out && out[0])
         ss_add(&ctx, out);
      cJSON_Delete(brief);
   }

   /* 2) Proactive recall sections appended below the brief. These are distinct
    * data from the brief's Key Facts (the 7 curated recall categories), so
    * there is no double-render. */
   cJSON *rbody = cJSON_CreateObject();
   cJSON_AddStringToObject(rbody, "task_hint", "session start");
   cJSON_AddBoolToObject(rbody, "session_start", 1);
   char *rbody_s = cJSON_PrintUnformatted(rbody);
   cJSON_Delete(rbody);
   cJSON *resp = ss_retry_post(endpoint, bearer, "/v1/memory/recall", rbody_s);
   free(rbody_s);
   if (resp)
   {
      cJSON *recall = cJSON_GetObjectItemCaseSensitive(resp, "recall");
      struct ss_sbuf b = {0};
      ss_render_section(&b, "Always-On Rules", cJSON_GetObjectItem(recall, "always_on_rules"));
      ss_render_section(&b, "Identity", cJSON_GetObjectItem(recall, "identity"));
      ss_render_section(&b, "Preferences", cJSON_GetObjectItem(recall, "preferences"));
      ss_render_section(&b, "Active Context", cJSON_GetObjectItem(recall, "active_context"));
      ss_render_section(&b, "Open Commitments", cJSON_GetObjectItem(recall, "open_commitments"));
      ss_render_section(&b, "Reminders", cJSON_GetObjectItem(recall, "reminders"));
      ss_render_section(&b, "Directives", cJSON_GetObjectItem(recall, "directives"));
      if (b.p && b.p[0])
      {
         /* The brief already ends with a blank line, so no leading newline is
          * needed here (avoids a triple blank line between the two blocks). */
         ss_add(&ctx, "# Proactive Recall (session-start)\n\n");
         ss_add(&ctx, b.p);
      }
      free(b.p);
      cJSON_Delete(resp);
   }

   free(endpoint);
   free(bearer);

   if (ctx.p && ctx.p[0])
   {
      cJSON *out = cJSON_CreateObject();
      cJSON *hook_out = cJSON_AddObjectToObject(out, "hookSpecificOutput");
      cJSON_AddStringToObject(hook_out, "hookEventName", "SessionStart");
      cJSON_AddStringToObject(hook_out, "additionalContext", ctx.p);
      char *s = cJSON_PrintUnformatted(out);
      if (s)
      {
         fputs(s, stdout);
         fputc('\n', stdout);
         free(s);
      }
      cJSON_Delete(out);
   }
   free(ctx.p);
   return 0;
}

/* Thin-client UserPromptSubmit hook (P1 context pre-injection for Claude Code).
 * Fires once per user turn. Fetches recall seeded by the user's prompt
 * (read-only POST /v1/memory/recall, task_hint = the prompt) and emits the
 * turn-relevant slices — wrapped as an <aimee-context> envelope with an
 * explore-with pointer at aimee's own retrieval tools — as the hook's
 * additionalContext, so Claude Code reasons over already-loaded context instead
 * of re-exploring the repo and, when it needs more, explores THROUGH aimee.
 *
 * Deliberately the Claude-Code delivery path for pre-injection: the Anthropic
 * /v1/messages proxy is kept a pure stateless wire-format proxy (mutating it
 * would corrupt the context Claude Code builds), so the envelope rides in via
 * the hook rather than the wire. Renders only the per-turn sections (session-
 * level identity/preferences/rules already land at SessionStart). Always
 * soft-fails (exit 0) so a prompt is never blocked. */
int handle_user_prompt_submit(void)
{
   char *stdin_data = read_stdin();
   cJSON *hook_json = stdin_data ? cJSON_Parse(stdin_data) : NULL;
   const char *prompt =
       hook_json ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(hook_json, "prompt"))
                 : NULL;

   char *endpoint = cli_v1_client_endpoint();
   if (!prompt || !prompt[0] || !endpoint)
   {
      free(endpoint);
      cJSON_Delete(hook_json);
      free(stdin_data);
      return 0;
   }
   char *bearer = cli_v1_client_bearer();

   cJSON *body = cJSON_CreateObject();
   cJSON_AddStringToObject(body, "task_hint", prompt);
   cJSON_AddBoolToObject(body, "session_start", 0);
   char *body_s = cJSON_PrintUnformatted(body);
   cJSON_Delete(body);

   int status = 0;
   cJSON *resp =
       cli_http_request(endpoint, "POST", "/v1/memory/recall", body_s, bearer, 15000, &status);
   free(endpoint);
   free(bearer);
   free(body_s);
   cJSON_Delete(hook_json);
   free(stdin_data);
   if (!resp || status != 200)
   {
      cJSON_Delete(resp);
      return 0;
   }

   cJSON *recall = cJSON_GetObjectItemCaseSensitive(resp, "recall");
   struct ss_sbuf b = {0};
   ss_render_section(&b, "Active Context", cJSON_GetObjectItem(recall, "active_context"));
   ss_render_section(&b, "Open Commitments", cJSON_GetObjectItem(recall, "open_commitments"));
   ss_render_section(&b, "Reminders", cJSON_GetObjectItem(recall, "reminders"));
   ss_render_section(&b, "Directives", cJSON_GetObjectItem(recall, "directives"));

   if (b.p && b.p[0])
   {
      struct ss_sbuf ctx = {0};
      ss_add(&ctx, "<aimee-context>\n");
      ss_add(&ctx, b.p);
      ss_add(&ctx, "explore-with: find_symbol, lsp_references, ast_grep_search, search_graph, "
                   "get_context_block\n");
      ss_add(&ctx, "</aimee-context>");

      cJSON *out = cJSON_CreateObject();
      cJSON *hook_out = cJSON_AddObjectToObject(out, "hookSpecificOutput");
      cJSON_AddStringToObject(hook_out, "hookEventName", "UserPromptSubmit");
      cJSON_AddStringToObject(hook_out, "additionalContext", ctx.p ? ctx.p : b.p);
      char *s = cJSON_PrintUnformatted(out);
      if (s)
      {
         fputs(s, stdout);
         fputc('\n', stdout);
         free(s);
      }
      free(ctx.p);
      cJSON_Delete(out);
   }
   free(b.p);
   cJSON_Delete(resp);
   return 0;
}

/* Thin-client PreCompact hook (P3 re-prime). Claude Code fires PreCompact just
 * before it compacts the conversation, which drops the session-start context.
 * Re-emit the durable recall (same broad read-only POST /v1/memory/recall as
 * session-start) as additionalContext so the post-compaction context still
 * carries identity/preferences/rules/active-context. Soft-fails (exit 0). */
int handle_pre_compact(void)
{
   char *stdin_data = read_stdin();
   free(stdin_data); /* PreCompact payload is informational; recall is broad. */

   char *endpoint = cli_v1_client_endpoint();
   if (!endpoint)
      return 0;
   char *bearer = cli_v1_client_bearer();

   cJSON *body = cJSON_CreateObject();
   cJSON_AddStringToObject(body, "task_hint", "compaction re-prime");
   cJSON_AddBoolToObject(body, "session_start", 1);
   char *body_s = cJSON_PrintUnformatted(body);
   cJSON_Delete(body);

   int status = 0;
   cJSON *resp =
       cli_http_request(endpoint, "POST", "/v1/memory/recall", body_s, bearer, 30000, &status);
   free(endpoint);
   free(bearer);
   free(body_s);
   if (!resp || status != 200)
   {
      cJSON_Delete(resp);
      return 0;
   }

   cJSON *recall = cJSON_GetObjectItemCaseSensitive(resp, "recall");
   struct ss_sbuf b = {0};
   ss_render_section(&b, "Always-On Rules", cJSON_GetObjectItem(recall, "always_on_rules"));
   ss_render_section(&b, "Identity", cJSON_GetObjectItem(recall, "identity"));
   ss_render_section(&b, "Preferences", cJSON_GetObjectItem(recall, "preferences"));
   ss_render_section(&b, "Active Context", cJSON_GetObjectItem(recall, "active_context"));
   ss_render_section(&b, "Open Commitments", cJSON_GetObjectItem(recall, "open_commitments"));
   ss_render_section(&b, "Reminders", cJSON_GetObjectItem(recall, "reminders"));
   ss_render_section(&b, "Directives", cJSON_GetObjectItem(recall, "directives"));

   if (b.p && b.p[0])
   {
      cJSON *out = cJSON_CreateObject();
      cJSON *hook_out = cJSON_AddObjectToObject(out, "hookSpecificOutput");
      cJSON_AddStringToObject(hook_out, "hookEventName", "PreCompact");
      struct ss_sbuf ctx = {0};
      ss_add(&ctx, "# Proactive Recall (re-primed after compaction)\n\n");
      ss_add(&ctx, b.p);
      cJSON_AddStringToObject(hook_out, "additionalContext", ctx.p ? ctx.p : b.p);
      char *s = cJSON_PrintUnformatted(out);
      if (s)
      {
         fputs(s, stdout);
         fputc('\n', stdout);
         free(s);
      }
      free(ctx.p);
      cJSON_Delete(out);
   }
   free(b.p);
   cJSON_Delete(resp);
   return 0;
}

int handle_session_start(int json_output)
{
   char *stdin_data = read_stdin();
   const char *hook_input = stdin_data ? stdin_data : "";

   /* Pin the aimee session_id to the host-provided value so DB1 session_state
    * rows line up with hooks.pre / hooks.post. Falls back to known session
    * env vars when the host omits it (e.g. test harnesses piping empty stdin). */
   char hook_sid[64] = "";
   const char *sid = NULL;
   cJSON *hook_json = stdin_data ? cJSON_Parse(stdin_data) : NULL;
   if (client_hook_payload_session_id(hook_json, hook_sid, sizeof(hook_sid)))
      sid = hook_sid;

   /* Detect server-invoked context: AIMEE_SESSION_ID is set by chat_stream_worker
    * in the environment of the claude subprocess it forks. */
   int nonblocking = (getenv("AIMEE_SESSION_ID") != NULL);

   /* P4: surface this project's central memory into the local memory dir so a
    * fresh session/agent sees it (best-effort; never blocks session-start).
    * Skipped under the .md retirement (AIMEE_MEMORY_MD_RETIRE): memory is not
    * re-materialized as .md files — the agent uses aimee memory recall/search
    * (the session brief steers it there) instead. */
   {
      const char *mr = getenv("AIMEE_MEMORY_MD_RETIRE");
      int md_retire = mr && (mr[0] == '1' || mr[0] == 't' || mr[0] == 'T' || mr[0] == 'y');
      char hcwd[4096];
      if (!md_retire && getcwd(hcwd, sizeof(hcwd)))
         harness_memory_hydrate(hcwd);
   }

   const char *sock = cli_ensure_server_for_method("hooks.session_start");
   if (!sock)
   {
      /* No co-located server. If a remote /v1 endpoint is configured, the thin
       * client serves session-start itself via the read-only recall route — no
       * local aimee-server needed (see handle_session_start_remote). */
      if (cli_v1_has_remote_endpoint())
      {
         int rc = handle_session_start_remote();
         cJSON_Delete(hook_json);
         free(stdin_data);
         return rc;
      }
      if (!nonblocking)
         fprintf(stderr, "aimee: cannot run session-start; server unavailable\n");
      cJSON_Delete(hook_json);
      free(stdin_data);
      return nonblocking ? 0 : 1;
   }

   /* Inject the client's CWD into hook_input so the server-side worktree
    * isolation check targets the client's repo, not the server's CWD. */
   char client_cwd[4096];
   char *augmented_hook = NULL;
   if (hook_json && getcwd(client_cwd, sizeof(client_cwd)))
   {
      if (!cJSON_GetObjectItemCaseSensitive(hook_json, "client_cwd"))
         cJSON_AddStringToObject(hook_json, "client_cwd", client_cwd);
      augmented_hook = cJSON_PrintUnformatted(hook_json);
      if (augmented_hook)
         hook_input = augmented_hook;
   }

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", "hooks.session_start");
   cJSON_AddStringToObject(req, "hook_input", hook_input);
   if (sid && sid[0])
      cJSON_AddStringToObject(req, "session_id", sid);
   if (nonblocking)
      cJSON_AddBoolToObject(req, "nonblocking", 1);

   /* Use a shorter timeout in the nonblocking path: the server will respond
    * immediately once the background thread is launched (typically <50 ms).
    * If it doesn't, soft-fail after 10 s rather than blocking claude for 60 s. */
   int timeout_ms = nonblocking ? 10000 : 60000;
   cJSON *resp = cli_v1_dispatch_local(req, timeout_ms);
   cJSON_Delete(req);
   cJSON_Delete(hook_json);
   free(augmented_hook);
   free(stdin_data);

   int exit_code = nonblocking ? 0 : 1;
   if (resp)
   {
      cJSON *ec = cJSON_GetObjectItemCaseSensitive(resp, "exit_code");
      if (cJSON_IsNumber(ec))
         exit_code = (int)ec->valuedouble;

      cJSON *jout = cJSON_GetObjectItemCaseSensitive(resp, "output");
      if (cJSON_IsString(jout) && jout->valuestring[0])
      {
         cJSON *out = cJSON_CreateObject();
         cJSON *hook_out = out ? cJSON_AddObjectToObject(out, "hookSpecificOutput") : NULL;
         if (hook_out)
         {
            cJSON_AddStringToObject(hook_out, "hookEventName", "SessionStart");
            cJSON_AddStringToObject(hook_out, "additionalContext", jout->valuestring);
            char *s = cJSON_PrintUnformatted(out);
            if (s)
            {
               fputs(s, stdout);
               fputc('\n', stdout);
               free(s);
            }
         }
         else
            fputs(jout->valuestring, stdout);
         cJSON_Delete(out);
      }

      cJSON *msg = cJSON_GetObjectItemCaseSensitive(resp, "message");
      if (cJSON_IsString(msg) && msg->valuestring[0])
         fprintf(stderr, "aimee: %s\n", msg->valuestring);

      if (json_output)
      {
         cJSON *out = cJSON_CreateObject();
         cJSON_AddNumberToObject(out, "exit_code", exit_code);
         if (cJSON_IsString(jout) && jout->valuestring[0])
            cJSON_AddStringToObject(out, "output", jout->valuestring);
         char *s = cJSON_PrintUnformatted(out);
         if (s)
         {
            puts(s);
            free(s);
         }
         cJSON_Delete(out);
      }

      cJSON_Delete(resp);
   }
   else if (!nonblocking)
   {
      fprintf(stderr, "aimee: session-start RPC failed\n");
   }
   return exit_code;
}
