/* cli_session_start.c: legacy client hooks. SessionStart itself is a compatibility
 * no-op; behavior is attached to shared model ingress, not lifecycle callbacks. */
#include "cli_client.h"
#include "cli_session_start.h"
#include "cJSON.h"
#include "client_session_worktree.h" /* client_session_id_publish */
#include "aimee_home.h"              /* aimee_home */
#include "agent_code_capabilities.h"
#include "aimee_session_guidance.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

/* A remote or long-running daemon cannot infer the thin client's active
 * checkout from its own process cwd. Carry it with every ordered memory read. */
static void ss_add_memory_cwd(cJSON *body)
{
   char cwd[4096];
   if (body && getcwd(cwd, sizeof(cwd)))
      cJSON_AddStringToObject(body, "cwd", cwd);
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

static const char *ss_registered_transport_guidance(void)
{
   const char *transport = getenv("AIMEE_HOOK_TRANSPORT");
   if (transport && strcmp(transport, "mcp") == 0)
      return "explore-with: Aimee MCP is the configured primary repository-intelligence "
             "surface. Before ordinary repository tools, start with its index capability "
             "using command=investigate and a plain-language summary of the task. Use "
             "find_symbol, callers, blast-radius, span, hybrid, and search_memory for targeted "
             "follow-up. The CLI is only for capabilities explicitly registered as "
             "CLI-only.\n" AIMEE_GUIDANCE_FIX_SCOPE_LINE;
   const char *cli = getenv("AIMEE_CLI_PATH");
   if (cli && cli[0])
   {
      static char guidance[8192];
      snprintf(guidance, sizeof(guidance),
               "explore-with: Aimee's registered CLI executable is `%s`; do not assume "
               "`aimee` is on PATH. Before ordinary repository tools, start with `%s index "
               "investigate \"<question>\"`. For targeted follow-up use `%s index find "
               "<symbol>`, `%s index callers <symbol>`, `%s index blast-radius <file>`, `%s "
               "index span <file> <start> <end>`, and `%s memory search <terms>`. Chain "
               "independent commands with && in one shell call. Shell stays right for "
               "building, running tests, and editing.\n%s",
               cli, cli, cli, cli, cli, cli, cli, AIMEE_GUIDANCE_FIX_SCOPE_LINE);
      return guidance;
   }
   return AIMEE_GUIDANCE_BLOCK;
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
/* Persist the host's on-disk conversation transcript (Claude Code's
 * transcript_path — JSONL, one message object per line) to DB1 under the real
 * session id. This is what makes a chat that flows through the anonymous
 * /v1/messages model gateway — which stores no conversation — logged and
 * recoverable after a crash. Best-effort: a *failure* never blocks the turn
 * (errors are swallowed), but the work IS synchronous, so the POST timeout is
 * kept short for the local daemon.
 *
 * The transcript is bounded. When it exceeds the cap we ship the TAIL, not the
 * head: the file is chronological, so the tail holds the newest turns (the ones
 * that matter most for recovery) and we never truncate mid-line into corrupt
 * JSON. The first (partial) line of a tail read is dropped so only whole JSONL
 * objects are parsed. A capped read is thus a valid recent-window snapshot, not
 * a stale head prefix that silently omits the latest turns.
 *
 * The cap is kept below the server's per-method limit for this route
 * (LIMIT_TRANSCRIPT, 3 MiB, itself under the 4 MiB SHTTP_MAX_BODY), with headroom
 * for the JSON envelope — a larger body is rejected/truncated server-side and
 * stores nothing. */
#define SS_TRANSCRIPT_MAX     (2 * 1024 * 1024)
#define SS_TRANSCRIPT_POST_MS 5000
static void ss_record_transcript(const char *endpoint, const char *bearer, const char *session_id,
                                 const char *transcript_path)
{
   if (!endpoint || !session_id || !session_id[0] || !transcript_path || !transcript_path[0])
      return;
   FILE *fp = fopen(transcript_path, "rb");
   if (!fp)
      return;
   if (fseek(fp, 0, SEEK_END) != 0)
   {
      fclose(fp);
      return;
   }
   long fsz = ftell(fp);
   if (fsz <= 0)
   {
      fclose(fp);
      return;
   }
   /* Oversize: read only the last SS_TRANSCRIPT_MAX bytes (the newest turns). */
   int tail = ((size_t)fsz > SS_TRANSCRIPT_MAX);
   long start = tail ? (fsz - (long)SS_TRANSCRIPT_MAX) : 0;
   size_t want = (size_t)(fsz - start);
   if (fseek(fp, start, SEEK_SET) != 0)
   {
      fclose(fp);
      return;
   }
   char *buf = malloc(want + 1);
   if (!buf)
   {
      fclose(fp);
      return;
   }
   size_t n = fread(buf, 1, want, fp);
   fclose(fp);
   buf[n] = '\0';

   /* JSONL: one JSON object per line (line content never contains a raw newline).
    * On a tail read, skip the first line — it is the cut-through tail of a line
    * the cap split, so it is not a complete JSON object. */
   char *scan = buf;
   if (tail)
   {
      char *first_nl = strchr(buf, '\n');
      scan = first_nl ? first_nl + 1 : buf + n; /* no newline in window → nothing whole */
   }
   cJSON *messages = cJSON_CreateArray();
   for (char *p = scan; messages && p && *p;)
   {
      char *nl = strchr(p, '\n');
      if (nl)
         *nl = '\0';
      if (*p)
      {
         cJSON *obj = cJSON_Parse(p);
         if (obj)
            cJSON_AddItemToArray(messages, obj);
      }
      if (!nl)
         break;
      p = nl + 1;
   }
   free(buf);

   if (!messages || cJSON_GetArraySize(messages) == 0)
   {
      cJSON_Delete(messages);
      return;
   }

   cJSON *body = cJSON_CreateObject();
   cJSON_AddStringToObject(body, "session_id", session_id);
   cJSON_AddItemToObject(body, "messages", messages); /* takes ownership */
   char *body_s = cJSON_PrintUnformatted(body);
   cJSON_Delete(body);
   if (!body_s)
      return;
   int status = 0;
   cJSON *resp = cli_http_request(endpoint, "POST", "/v1/sessions/record_transcript", body_s,
                                  bearer, SS_TRANSCRIPT_POST_MS, &status);
   free(body_s);
   cJSON_Delete(resp);
}

int handle_user_prompt_submit(void)
{
   char *stdin_data = read_stdin();
   cJSON *hook_json = stdin_data ? cJSON_Parse(stdin_data) : NULL;
   const char *prompt =
       hook_json ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(hook_json, "prompt"))
                 : NULL;

   char *endpoint = cli_v1_client_endpoint();
   if (!prompt || !prompt[0])
   {
      free(endpoint);
      cJSON_Delete(hook_json);
      free(stdin_data);
      return 0;
   }
   char *bearer = endpoint ? cli_v1_client_bearer() : NULL;

   /* Log this session's conversation to DB1 under its real host id. The hook
    * payload carries both the session id and transcript_path (the full chat the
    * host keeps on disk); persist it every turn so a crashed session is
    * recoverable. Runs before the recall round-trip; best-effort. */
   {
      char rec_sid[64] = "";
      const char *tpath =
          cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(hook_json, "transcript_path"));
      if (endpoint && client_hook_payload_session_id(hook_json, rec_sid, sizeof(rec_sid)) &&
          rec_sid[0] && tpath)
         ss_record_transcript(endpoint, bearer, rec_sid, tpath);
   }

   int status = 0;
   cJSON *resp = NULL;
   if (endpoint)
   {
      cJSON *body = cJSON_CreateObject();
      cJSON_AddStringToObject(body, "task_hint", prompt);
      cJSON_AddBoolToObject(body, "session_start", 0);
      ss_add_memory_cwd(body);
      char *body_s = cJSON_PrintUnformatted(body);
      cJSON_Delete(body);
      if (body_s)
         resp = cli_http_request(endpoint, "POST", "/v1/memory/recall", body_s, bearer, 15000,
                                 &status);
      free(body_s);
   }
   free(endpoint);
   free(bearer);
   cJSON_Delete(hook_json);
   free(stdin_data);

   struct ss_sbuf b = {0};
   if (resp && status == 200)
   {
      cJSON *recall = cJSON_GetObjectItemCaseSensitive(resp, "recall");
      ss_render_section(&b, "Active Context", cJSON_GetObjectItem(recall, "active_context"));
      ss_render_section(&b, "Open Commitments", cJSON_GetObjectItem(recall, "open_commitments"));
      ss_render_section(&b, "Reminders", cJSON_GetObjectItem(recall, "reminders"));
      ss_render_section(&b, "Directives", cJSON_GetObjectItem(recall, "directives"));
   }

   /* The registered CLI/MCP guidance is useful even when recall is empty or the
    * server is temporarily unavailable. Keeping it outside the recall-success
    * branch makes a direct hook path obey the same activation contract as thin
    * clients and the shared model gateway. */
   struct ss_sbuf ctx = {0};
   ss_add(&ctx, "<aimee-context>\n");
   ss_add(&ctx, b.p);
   ss_add(&ctx, ss_registered_transport_guidance());
   ss_add(&ctx, "</aimee-context>");

   cJSON *out = cJSON_CreateObject();
   cJSON *hook_out = out ? cJSON_AddObjectToObject(out, "hookSpecificOutput") : NULL;
   if (hook_out && ctx.p)
   {
      cJSON_AddStringToObject(hook_out, "hookEventName", "UserPromptSubmit");
      cJSON_AddStringToObject(hook_out, "additionalContext", ctx.p);
      char *s = cJSON_PrintUnformatted(out);
      if (s)
      {
         fputs(s, stdout);
         fputc('\n', stdout);
         free(s);
      }
   }
   free(ctx.p);
   cJSON_Delete(out);
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
   ss_add_memory_cwd(body);
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

   /* Pin the aimee session_id to the host-provided value so DB1 session_state
    * rows line up with hooks.pre / hooks.post. Falls back to known session
    * env vars when the host omits it (e.g. test harnesses piping empty stdin). */
   char hook_sid[64] = "";
   const char *sid = NULL;
   cJSON *hook_json = stdin_data ? cJSON_Parse(stdin_data) : NULL;
   if (client_hook_payload_session_id(hook_json, hook_sid, sizeof(hook_sid)))
      sid = hook_sid;

   /* Share it with the rest of the session BEFORE any transport choice, because
    * both branches below return.
    *
    * This hook holds the only authoritative copy of the host's session id, and
    * used to keep it: it built its worktree from the id and then exited. `aimee
    * mcp serve`, which has no way to learn it, fell through to minting a random
    * one -- so the same Claude Code session ran on TWO session ids and therefore
    * two worktrees, with the proxy (and every delegate and `aimee git` call
    * behind it) bound to the empty one and refusing the worktree that actually
    * held the work. Publishing here is the half of that rendezvous that was
    * never written; the reader has always been there. */
   if (sid && sid[0])
      (void)client_session_id_publish(sid, aimee_home());

   /* Compatibility no-op for clients that still have an old SessionStart hook
    * installed. The launcher owns session id, worktree, and cwd before the host
    * process starts; editable persona content is prepended at model ingress.
    * Emitting additionalContext here would reintroduce client/version-specific
    * delivery and duplicate the first-message payload. */
   if (json_output)
      puts("{\"exit_code\":0}");
   cJSON_Delete(hook_json);
   free(stdin_data);
   return 0;
}
