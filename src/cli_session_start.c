/* cli_session_start.c: the `aimee session-start` SessionStart-hook entry,
 * extracted from cli_main.c. Forwards hooks.session_start to a co-located
 * server, or — against a remote /v1 endpoint — emits proactive recall directly
 * (POST /v1/memory/recall) so a thin client needs no local server. Keeping it in
 * its own TU holds cli_main.c under the source line limit. */
#include "cli_client.h"
#include "cli_session_start.h"
#include "session_degraded_notice.h"
#include "cJSON.h"
#include "cli_attention_guard.h"     /* attn_require_session_worktree */
#include "client_session_worktree.h" /* client_session_worktree_ensure, _id_publish */
#include "aimee_home.h"              /* aimee_home */
#include "cmd_self_update.h"         /* aimee_self_update_notice */
#include "agent_code_capabilities.h"
#include "aimee_session_guidance.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* This is a thin-client TU compiled without -I./-Idb2, and the client binary
 * links none of workspace.o/guardrails.o/config.o — so the per-session worktree
 * is prepared by client_session_worktree_ensure (git subprocesses), not by those
 * functions. */

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

/* Remote/thin session-start (handle_session_start_remote) does no local worktree
 * setup — it assumes the client tree is absent on the remote server. But the
 * attention-guard's `require_session_worktree` isolation still runs LOCALLY and
 * blocks every mutation outside a managed worktree, so a remote-server deployment
 * would wedge the session: nothing prepares or points to a worktree, yet the
 * guard demands one. Prepare the per-session worktree (branch cut from the
 * repository's default branch, via client_session_worktree_ensure) and surface a
 * directive telling the agent to enter it before its first mutating tool call.
 *
 * A hook cannot chdir its host, so for a hook-driven session this directive IS
 * the handoff — unlike `aimee mcp serve`, which enters the worktree itself.
 *
 * No-op when isolation is off, the session is already inside a worktree, or
 * there is no local git repo. */
static void ss_append_worktree_isolation(struct ss_sbuf *ctx, const char *sid)
{
   char wt[4200];
   if (client_session_worktree_ensure(sid, wt, sizeof wt) != 0)
      return; /* not applicable, or failed loudly on stderr already */

   ss_add(ctx, "\n# Isolated Checkout (REQUIRED before editing)\n");
   ss_add(ctx, "This session runs against a remote aimee-server, and session-worktree isolation "
               "(`require_session_worktree`) is ON. You are NOT in a managed worktree, so every "
               "edit/write/mutating shell command WILL be blocked until you enter one. An isolated "
               "worktree has been prepared for you:\n\n  ");
   ss_add(ctx, wt);
   ss_add(ctx,
          "\n\nBefore your first mutating tool call, switch into it — Claude Code: call "
          "`EnterWorktree` with that path; or `cd` into it for shell work. Do not edit the shared "
          "checkout.\n\n");
}

static int handle_session_start_remote(const char *sid)
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
    * (the pre-Phase-1 behaviour when recall was empty). */
   cJSON *brief_body = cJSON_CreateObject();
   ss_add_memory_cwd(brief_body);
   char *brief_body_s = cJSON_PrintUnformatted(brief_body);
   cJSON_Delete(brief_body);
   cJSON *brief = ss_retry_post(endpoint, bearer, "/v1/session/brief_assemble",
                                brief_body_s ? brief_body_s : "{}");
   free(brief_body_s);
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
   ss_add_memory_cwd(rbody);
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

   /* Dependency health: if the knowledge service is down, the agent needs to know
    * BEFORE it interprets an empty search as an authoritative "not found". Read
    * the same /v1/ready snapshot the operator sees, so the two never disagree.
    * A single GET, not ss_retry_post: this is advisory, and a server too sick to
    * answer once is exactly the case the notice describes. Must run here, while
    * endpoint/bearer are still live. */
   {
      int rstatus = 0;
      cJSON *rdy = cli_http_request(endpoint, "GET", "/v1/ready", NULL, bearer,
                                    SESSION_START_RECALL_TIMEOUT_MS, &rstatus);
      if (rdy)
      {
         const cJSON *deps = cJSON_GetObjectItemCaseSensitive(rdy, "dependencies");
         char notice[640];
         if (ss_degraded_notice(cJSON_GetStringValue(cJSON_GetObjectItem(deps, "kb")),
                                cJSON_GetStringValue(cJSON_GetObjectItem(deps, "retrieval")),
                                notice, sizeof notice))
            ss_add(&ctx, notice);
         cJSON_Delete(rdy);
      }
   }

   free(endpoint);
   free(bearer);

   /* Thin-client version drift: this client talks 1:1 to the remote server, so
    * if the server has moved ahead, surface a one-line notice steering the agent
    * (or user) to `aimee self-update`. Best-effort; never blocks the session. */
   {
      /* Both version strings are interpolated, and a git-describe version
       * ("pre-merge-safety-903-g6fee67ae87") is far longer than a release tag --
       * 256 truncated the dev-build notice mid-word on a real pair. */
      char notice[512];
      if (aimee_self_update_notice(notice, sizeof notice))
      {
         ss_add(&ctx, "\n# aimee update available\n");
         ss_add(&ctx, notice);
         ss_add(&ctx, "\n");
      }
      /* With `update_mode: apply`, catch up automatically (verified, rate-limited,
       * detached — never blocks this session). No-op in the default notify mode. */
      aimee_self_update_apply_async();
   }

   /* Local worktree isolation: even though compute is remote, the guard runs on
    * THIS host. Prepare + direct the agent into an isolated worktree so mutating
    * tools aren't blocked. Appended last so it shows even if the remote brief
    * fetch returned nothing (and so a wedged session always gets the way out). */
   ss_append_worktree_isolation(&ctx, sid);

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
   if (!prompt || !prompt[0] || !endpoint)
   {
      free(endpoint);
      cJSON_Delete(hook_json);
      free(stdin_data);
      return 0;
   }
   char *bearer = cli_v1_client_bearer();

   /* Log this session's conversation to DB1 under its real host id. The hook
    * payload carries both the session id and transcript_path (the full chat the
    * host keeps on disk); persist it every turn so a crashed session is
    * recoverable. Runs before the recall round-trip; best-effort. */
   {
      char rec_sid[64] = "";
      const char *tpath =
          cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(hook_json, "transcript_path"));
      if (client_hook_payload_session_id(hook_json, rec_sid, sizeof(rec_sid)) && rec_sid[0] &&
          tpath)
         ss_record_transcript(endpoint, bearer, rec_sid, tpath);
   }

   cJSON *body = cJSON_CreateObject();
   cJSON_AddStringToObject(body, "task_hint", prompt);
   cJSON_AddBoolToObject(body, "session_start", 0);
   ss_add_memory_cwd(body);
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
      ss_add(&ctx, AIMEE_GUIDANCE_BLOCK);
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
   const char *hook_input = stdin_data ? stdin_data : "";

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

   /* Detect server-invoked context: AIMEE_SESSION_ID is set by chat_stream_worker
    * in the environment of the claude subprocess it forks. */
   int nonblocking = (getenv("AIMEE_SESSION_ID") != NULL);

   /* Remote configuration is an exclusive transport choice.  Select it before
    * any local availability probe so one hook invocation cannot touch both the
    * co-located and remote servers. */
   if (cli_v1_has_remote_endpoint())
   {
      int rc = handle_session_start_remote(sid);
      cJSON_Delete(hook_json);
      free(stdin_data);
      return rc;
   }

   const char *sock = cli_ensure_server_for_method("hooks.session_start");

   /* .md memory is retired: central memory is NOT re-materialized into local .md
    * files at session-start — the agent uses `aimee memory recall`/`search` (the
    * session brief steers it there) instead. */

   if (!sock)
   {
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

   /* Prepare + surface the per-session worktree onboarding directive when this
    * session is running against the SHARED MAIN CLONE (not a managed worktree)
    * and isolation is enforced. This is the SAME onboarding the remote/thin path
    * already does (handle_session_start_remote): a co-located session started
    * outside the launcher (e.g. a raw `claude` in the repo) never went through
    * launch.run and so was never placed on a worktree — without this its first
    * edit is blocked by the guard. ss_append_worktree_isolation no-ops when the
    * session is already isolated (launcher/server-forked) or isolation is off, so
    * an already-placed session is unaffected. It is computed independently of the
    * RPC response so the directive is emitted even when the server returns no
    * brief/recall or the RPC failed. */
   struct ss_sbuf wt = {0};
   ss_append_worktree_isolation(&wt, sid);

   int exit_code = nonblocking ? 0 : 1;
   const char *server_out = NULL;
   if (resp)
   {
      cJSON *ec = cJSON_GetObjectItemCaseSensitive(resp, "exit_code");
      if (cJSON_IsNumber(ec))
         exit_code = (int)ec->valuedouble;

      cJSON *jout = cJSON_GetObjectItemCaseSensitive(resp, "output");
      if (cJSON_IsString(jout) && jout->valuestring[0])
         server_out = jout->valuestring;
   }

   /* additionalContext = [worktree directive] + [server brief/recall]. Emit the
    * hookSpecificOutput block whenever either part is present. */
   if ((wt.p && wt.p[0]) || server_out)
   {
      struct ss_sbuf ctx = {0};
      if (wt.p && wt.p[0])
         ss_add(&ctx, wt.p);
      if (server_out)
         ss_add(&ctx, server_out);

      cJSON *out = cJSON_CreateObject();
      cJSON *hook_out = out ? cJSON_AddObjectToObject(out, "hookSpecificOutput") : NULL;
      if (hook_out)
      {
         cJSON_AddStringToObject(hook_out, "hookEventName", "SessionStart");
         cJSON_AddStringToObject(hook_out, "additionalContext", ctx.p ? ctx.p : "");
         char *s = cJSON_PrintUnformatted(out);
         if (s)
         {
            fputs(s, stdout);
            fputc('\n', stdout);
            free(s);
         }
      }
      else if (server_out)
         fputs(server_out, stdout);
      cJSON_Delete(out);
      free(ctx.p);
   }

   if (resp)
   {
      cJSON *msg = cJSON_GetObjectItemCaseSensitive(resp, "message");
      if (cJSON_IsString(msg) && msg->valuestring[0])
         fprintf(stderr, "aimee: %s\n", msg->valuestring);

      if (json_output)
      {
         cJSON *out = cJSON_CreateObject();
         cJSON_AddNumberToObject(out, "exit_code", exit_code);
         if (server_out)
            cJSON_AddStringToObject(out, "output", server_out);
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
   free(wt.p);
   return exit_code;
}
