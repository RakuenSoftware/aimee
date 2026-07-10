/* cli_session_start.c: the `aimee session-start` SessionStart-hook entry,
 * extracted from cli_main.c. Forwards hooks.session_start to a co-located
 * server, or — against a remote /v1 endpoint — emits proactive recall directly
 * (POST /v1/memory/recall) so a thin client needs no local server. Keeping it in
 * its own TU holds cli_main.c under the source line limit. */
#include "cli_client.h"
#include "cli_session_start.h"
#include "cJSON.h"
#include "cli_attention_guard.h" /* attn_require_session_worktree, attn_session_isolation_blocked */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* This is a thin-client TU compiled without -I./-Idb2, and the client binary
 * links none of workspace.o/guardrails.o/config.o — so worktree creation here
 * goes through git subprocesses (below), not those functions. Only the
 * lightweight attention-guard helpers (attn_require_session_worktree /
 * attn_session_isolation_blocked, from cli_attention_guard.h) are available. */

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

/* Capture the first trimmed stdout line of `cmd` into out[cap]; 1 on non-empty. */
static int ss_git_first_line(const char *cmd, char *out, size_t cap)
{
   if (cap)
      out[0] = '\0';
   FILE *f = popen(cmd, "r");
   if (!f)
      return 0;
   char buf[4096] = {0};
   char *got = fgets(buf, sizeof buf, f);
   pclose(f);
   if (!got)
      return 0;
   size_t n = strlen(buf);
   while (n > 0 &&
          (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ' || buf[n - 1] == '\t'))
      buf[--n] = '\0';
   if (!buf[0])
      return 0;
   snprintf(out, cap, "%s", buf);
   return 1;
}

/* Remote/thin session-start (handle_session_start_remote) does no local worktree
 * setup — it assumes the client tree is absent on the remote server. But the
 * attention-guard's `require_session_worktree` isolation still runs LOCALLY and
 * blocks every mutation outside a managed worktree, so a remote-server deployment
 * would wedge the session: nothing prepares or points to a worktree, yet the
 * guard demands one. Prepare the per-session sibling worktree here (via git, as
 * the thin client links no workspace helpers) and surface a directive telling the
 * agent to enter it before its first mutating tool call. No-op when isolation is
 * off, the session is already inside a worktree, or there is no local git repo. */
static void ss_append_worktree_isolation(struct ss_sbuf *ctx, const char *sid)
{
   if (!attn_require_session_worktree())
      return; /* isolation not enforced -> nothing to prepare or say */

   char cwd[4096];
   if (!getcwd(cwd, sizeof cwd))
      return;
   /* Already inside a managed (.aimee/.claude/.codex) worktree -> a mutating op
    * would not be blocked; don't nag or create a redundant worktree. Mirrors the
    * guard's own decision so the directive fires exactly when it would block. */
   if (!attn_session_isolation_blocked(ATTN_OP_SOFT, NULL, cwd))
      return;

   /* Need a stable session id to name the worktree; Claude Code always sends one. */
   if (!sid || !sid[0])
      return;
   char sid_safe[64];
   size_t si = 0;
   for (const char *p = sid; *p && si + 1 < sizeof sid_safe; p++)
   {
      char c = *p;
      int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
               c == '-' || c == '_' || c == '.';
      sid_safe[si++] = ok ? c : '-';
   }
   sid_safe[si] = '\0';
   if (!sid_safe[0])
      return;

   /* A single quote in a path would break the single-quoted popen/system commands;
    * such local repo paths are pathological -> skip rather than risk misquoting. */
   if (strchr(cwd, '\''))
      return;

   char cmd[10240];
   char git_root[4096];
   snprintf(cmd, sizeof cmd, "git -C '%s' rev-parse --show-toplevel 2>/dev/null", cwd);
   if (!ss_git_first_line(cmd, git_root, sizeof git_root) || strchr(git_root, '\''))
      return; /* not a git repo (or unquotable path) -> nothing to prepare */

   /* Base the session branch on the repo's default branch (origin HEAD -> HEAD). */
   char base_ref[256];
   snprintf(cmd, sizeof cmd, "git -C '%s' symbolic-ref --short refs/remotes/origin/HEAD 2>/dev/null",
            git_root);
   if (!ss_git_first_line(cmd, base_ref, sizeof base_ref))
   {
      snprintf(cmd, sizeof cmd, "git -C '%s' symbolic-ref --short HEAD 2>/dev/null", git_root);
      if (!ss_git_first_line(cmd, base_ref, sizeof base_ref))
         snprintf(base_ref, sizeof base_ref, "HEAD");
   }
   if (strchr(base_ref, '\''))
      return;

   char wt[4200];
   if (snprintf(wt, sizeof wt, "%s/.aimee/worktrees/%s/main", git_root, sid_safe) >=
       (int)sizeof wt)
      return;

   struct stat st;
   if (stat(wt, &st) != 0)
   {
      /* Prune stale registrations (best-effort), then create the worktree+branch.
       * git worktree add makes intermediate dirs; -Wno-unused-result covers system(). */
      snprintf(cmd, sizeof cmd, "git -C '%s' worktree prune >/dev/null 2>&1", git_root);
      (void)system(cmd);
      int cn = snprintf(cmd, sizeof cmd,
                        "git -C '%s' worktree add '%s' -b 'aimee/session/%s' '%s' >/dev/null 2>&1",
                        git_root, wt, sid_safe, base_ref);
      if (cn < 0 || cn >= (int)sizeof cmd)
         return; /* command truncated -> don't run a malformed git invocation */
      (void)system(cmd);
   }
   if (stat(wt, &st) != 0 || !S_ISDIR(st.st_mode))
      return; /* creation failed -> don't point the agent at a path that isn't there */

   ss_add(ctx, "\n# Isolated Checkout (REQUIRED before editing)\n");
   ss_add(ctx,
          "This session runs against a remote aimee-server, and session-worktree isolation "
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

   const char *sock = cli_ensure_server_for_method("hooks.session_start");

   /* .md memory is retired: central memory is NOT re-materialized into local .md
    * files at session-start — the agent uses `aimee memory recall`/`search` (the
    * session brief steers it there) instead. */

   if (!sock)
   {
      /* No co-located server. If a remote /v1 endpoint is configured, the thin
       * client serves session-start itself via the read-only recall route — no
       * local aimee-server needed (see handle_session_start_remote). */
      if (cli_v1_has_remote_endpoint())
      {
         int rc = handle_session_start_remote(sid);
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
