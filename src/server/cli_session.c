/* cli_session.c: tmux-based CLI session driver */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "aimee.h"
#include "cli_session.h"
#include "util.h"
#include "workspace_provider.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Run a tmux shell command for the session. On a detached (thin-client)
 * workspace the standard `claude` CLI, tmux, and the working tree live on the
 * CLIENT, so marshal the command over the reverse channel to run there; a
 * co-located turn runs it locally. Same contract as run_cmd: returns malloc'd
 * combined output (caller frees) and sets *exit_code. */
static char *sess_run(const char *cmd, int *exit_code)
{
   const workspace_provider_t *ws = workspace_provider_active();
   if (ws && ws->kind == WS_PROVIDER_DETACHED && ws->exec_shell)
      return ws->exec_shell(ws, cmd, exit_code);
   return run_cmd(cmd, exit_code);
}

/* True when this turn's tmux session runs on a detached client. */
static int sess_detached(void)
{
   const workspace_provider_t *ws = workspace_provider_active();
   return ws && ws->kind == WS_PROVIDER_DETACHED && ws->exec_shell && ws->write_all;
}

/* djb2 hash */
static unsigned long djb2(const char *s)
{
   unsigned long h = 5381;
   while (*s)
      h = h * 31 + (unsigned char)*s++;
   return h;
}

static void msleep_ms(int ms)
{
   struct timespec ts;
   ts.tv_sec = ms / 1000;
   ts.tv_nsec = (ms % 1000) * 1000000L;
   nanosleep(&ts, NULL);
}

/* Monotonic millisecond clock for wall-clock timeouts (immune to wall-time
 * jumps). Mirrors the clock_gettime(CLOCK_MONOTONIC) idiom used elsewhere. */
static long long mono_ms(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Per-thread incremental stream callback (set by the chat worker before a turn,
 * cleared after). Thread-local so concurrent turns on the pool never see each
 * other's sink. */
static __thread cli_session_stream_cb_t g_stream_cb = NULL;
static __thread void *g_stream_ud = NULL;

void cli_session_set_stream_cb(cli_session_stream_cb_t cb, void *ud)
{
   g_stream_cb = cb;
   g_stream_ud = ud;
}

cli_session_stream_cb_t cli_session_get_stream_cb(void **ud_out)
{
   if (ud_out)
      *ud_out = g_stream_ud;
   return g_stream_cb;
}

void cli_session_set_kind(cli_session_t *s, const char *cli_kind)
{
   if (!s)
      return;
   snprintf(s->cli_kind, sizeof(s->cli_kind), "%s", cli_kind ? cli_kind : "");
}

void cli_session_mark_baseline(cli_session_t *s)
{
   if (!s)
      return;
   free(s->baseline);
   s->baseline = NULL;
   char *cap = malloc(CLI_SESSION_BUF_MAX);
   if (!cap)
      return;
   if (cli_session_capture(s, cap, CLI_SESSION_BUF_MAX) == 0)
      s->baseline = cli_session_strip_ansi(cap);
   free(cap);
}

static void cli_session_capture_cmd(const cli_session_t *s, char *cmd, size_t cmd_len)
{
   snprintf(cmd, cmd_len, "tmux capture-pane -p -t '%s' 2>/dev/null", s->session_name);
}

int cli_session_is_alive(const cli_session_t *s)
{
   char cmd[CLI_SESSION_NAME_MAX + 64];
   snprintf(cmd, sizeof(cmd), "tmux has-session -t '%s' 2>/dev/null", s->session_name);
   int rc;
   char *out = sess_run(cmd, &rc);
   free(out);
   return rc == 0;
}

int cli_session_create(cli_session_t *s, const char *session_name, const char *cli_cmd,
                       const char *work_dir, int reuse)
{
   memset(s, 0, sizeof(*s));
   snprintf(s->session_name, CLI_SESSION_NAME_MAX, "%s", session_name);
   snprintf(s->cli_cmd, CLI_SESSION_CMD_MAX, "%s", cli_cmd);
   s->reuse = reuse;

   /* If reuse is set and session exists, attach to it */
   if (reuse && cli_session_is_alive(s))
   {
      s->active = 1;
      s->last_activity = time(NULL);
      return 0;
   }

   /* Create new tmux session running the CLI directly rather than via an
    * interactive shell prompt. That avoids a race where the first user
    * message lands before the CLI is fully attached and also keeps shell
    * prompts out of captured output. */
   char create_cmd[512];
   char *esc_cli = shell_escape(cli_cmd && cli_cmd[0] ? cli_cmd : "claude");
   if (work_dir && work_dir[0])
   {
      char *esc_dir = shell_escape(work_dir);
      snprintf(create_cmd, sizeof(create_cmd),
               "tmux new-session -d -s '%s' -x 220 -y 50 -c %s /bin/sh -lc %s 2>/dev/null",
               session_name, esc_dir, esc_cli);
      free(esc_dir);
   }
   else
   {
      snprintf(create_cmd, sizeof(create_cmd),
               "tmux new-session -d -s '%s' -x 220 -y 50 /bin/sh -lc %s 2>/dev/null", session_name,
               esc_cli);
   }
   free(esc_cli);
   int rc;
   char *out = sess_run(create_cmd, &rc);
   free(out);
   if (rc != 0)
      return -1;

   /* Wait for CLI to produce output (up to 5s) */
   for (int i = 0; i < 10; i++)
   {
      msleep_ms(500);
      char cap_cmd[256];
      snprintf(cap_cmd, sizeof(cap_cmd), "tmux capture-pane -p -t '%s' 2>/dev/null", session_name);
      char *cap = sess_run(cap_cmd, &rc);
      int has_output = (cap && strlen(cap) > 2);
      free(cap);
      if (has_output)
         break;
   }

   s->active = 1;
   s->last_activity = time(NULL);
   return 0;
}

void cli_session_destroy(cli_session_t *s)
{
   free(s->baseline);
   s->baseline = NULL;
   free(s->stream_emitted);
   s->stream_emitted = NULL;
   if (!s->active)
      return;
   if (!cli_session_is_alive(s))
   {
      s->active = 0;
      return;
   }
   char cmd[CLI_SESSION_NAME_MAX + 64];
   snprintf(cmd, sizeof(cmd), "tmux kill-session -t '%s' 2>/dev/null", s->session_name);
   int rc;
   char *out = sess_run(cmd, &rc);
   free(out);
   s->active = 0;
}

int cli_session_send(cli_session_t *s, const char *message)
{
   if (!s->active || !message)
      return -1;

   /* Write message to a temp file to avoid quoting issues. The file must live on
    * the same host as the tmux session: on a detached workspace that is the
    * CLIENT (write via the provider), else the local fs.
    *
    * Both the tmpfile and the tmux paste buffer are keyed on the (unique,
    * tmux-sanitized) session name: concurrent turns on different panes would
    * otherwise race a single shared `aimee_msg` buffer / `/tmp/aimee_msg_<pid>.txt`
    * and paste each other's prompt into the wrong pane. Using the full name (not
    * a 32-bit hash) rules out a collision reintroducing that exact bleed. */
   int detached = sess_detached();
   char tmpfile[64 + CLI_SESSION_NAME_MAX];
   snprintf(tmpfile, sizeof(tmpfile), "/tmp/aimee_msg_%d_%s.txt", (int)getpid(), s->session_name);
   char bufname[16 + CLI_SESSION_NAME_MAX];
   snprintf(bufname, sizeof(bufname), "aimee_msg_%s", s->session_name);

   /* Paste the message verbatim — no trailing newline. A newline inside the
    * pasted buffer lands in the CLI's multi-line composer as a literal line
    * break (codex) rather than a submit; submission is the explicit Enter
    * keypress below, after a short settle so the TUI has finished ingesting the
    * bracketed paste. */
   size_t mlen = strlen(message);
   size_t blen = mlen;
   while (blen > 0 && (message[blen - 1] == '\n' || message[blen - 1] == '\r'))
      blen--;
   char *buf = malloc(blen + 1);
   if (!buf)
      return -1;
   memcpy(buf, message, blen);
   buf[blen] = '\0';

   if (detached)
   {
      const workspace_provider_t *ws = workspace_provider_active();
      if (ws->write_all(ws, tmpfile, buf, blen) != 0)
      {
         free(buf);
         return -1;
      }
   }
   else
   {
      FILE *f = fopen(tmpfile, "w");
      if (!f)
      {
         free(buf);
         return -1;
      }
      fwrite(buf, 1, blen, f);
      fclose(f);
   }
   free(buf);

   /* Load temp file as tmux buffer, paste into session, then press Enter */
   char cmd[512];
   int rc;
   char *out;

   snprintf(cmd, sizeof(cmd), "tmux load-buffer -b %s '%s' 2>/dev/null", bufname, tmpfile);
   out = sess_run(cmd, &rc);
   free(out);
   if (rc != 0)
   {
      if (detached)
      {
         snprintf(cmd, sizeof(cmd), "rm -f '%s'", tmpfile);
         free(sess_run(cmd, &rc));
      }
      else
         unlink(tmpfile);
      return -1;
   }

   snprintf(cmd, sizeof(cmd), "tmux paste-buffer -b %s -t '%s' -d 2>/dev/null", bufname,
            s->session_name);
   out = sess_run(cmd, &rc);
   free(out);

   /* Let the TUI finish ingesting the paste before submitting. A fast Enter can
    * land before the composer has the text — codex in particular leaves the
    * pasted text on a composer continuation line and ignores the Enter, so the
    * turn never starts. 500ms reliably clears this for claude and codex;
    * negligible next to a multi-second turn. */
   msleep_ms(500);

   /* Send Enter to submit */
   snprintf(cmd, sizeof(cmd), "tmux send-keys -t '%s' Enter 2>/dev/null", s->session_name);
   out = sess_run(cmd, &rc);
   free(out);

   if (detached)
   {
      snprintf(cmd, sizeof(cmd), "rm -f '%s'", tmpfile);
      free(sess_run(cmd, &rc));
   }
   else
      unlink(tmpfile);
   s->last_activity = time(NULL);
   return 0;
}

int cli_session_capture(cli_session_t *s, char *out, size_t out_max)
{
   if (!s || !s->active || !out || out_max == 0)
      return -1;

   char cap_cmd[CLI_SESSION_NAME_MAX + 64];
   cli_session_capture_cmd(s, cap_cmd, sizeof(cap_cmd));

   int rc;
   char *cap = sess_run(cap_cmd, &rc);
   if (!cap || rc != 0)
   {
      free(cap);
      out[0] = '\0';
      return -1;
   }

   snprintf(out, out_max, "%s", cap);
   free(cap);
   return 0;
}

/* --- TUI response extraction --------------------------------------------- */

static const char *cli_lstrip(const char *s)
{
   while (*s == ' ' || *s == '\t')
      s++;
   return s;
}

/* A horizontal rule / box edge (run of ─ ╭ ╮ ╰ ╯ │) with no letters/digits. */
static int cli_line_is_rule(const char *t)
{
   if (strncmp(t, "\xe2\x94\x80", 3) != 0 && strncmp(t, "\xe2\x95\xad", 3) != 0 &&
       strncmp(t, "\xe2\x95\xb0", 3) != 0 && strncmp(t, "\xe2\x94\x82", 3) != 0)
      return 0;
   for (const char *p = t; *p; p++)
      if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9'))
         return 0;
   return 1;
}

/* Lines that mark the end of (or are not part of) the assistant's answer:
 * the claude status star, separators, footers, interrupt hints. */
static int cli_line_is_chrome(const char *t)
{
   if (strncmp(t, "\xe2\x9c\xbb", 3) == 0) /* ✻ claude "Cooked/Baked for Ns" status */
      return 1;
   if (cli_line_is_rule(t))
      return 1;
   if (strstr(t, "? for shortcuts") || strstr(t, "for agents") || strstr(t, "to interrupt") ||
       strstr(t, "tmux detected") || strstr(t, "Shell cwd was reset") ||
       strstr(t, "Update available") || strstr(t, "/model to change"))
      return 1;
   return 0;
}

/* True if `body` appears as a whole line in `baseline` (after lstrip and an
 * optional leading assistant marker). A substring test would false-exclude a
 * short reply ("ok", "yes", a number) that merely occurs inside a longer prior
 * line — silently dropping the current turn — so the match is line-exact. */
static int cli_body_in_baseline(const char *baseline, const char *amark, const char *body)
{
   if (!baseline || !*baseline || !body || !*body)
      return 0;
   size_t blen = strlen(body);
   for (const char *p = baseline; p && *p;)
   {
      const char *eol = strchr(p, '\n');
      size_t llen = eol ? (size_t)(eol - p) : strlen(p);
      const char *ls = p;
      size_t ll = llen;
      while (ll && (*ls == ' ' || *ls == '\t'))
      {
         ls++;
         ll--;
      }
      if (ll >= 3 && strncmp(ls, amark, 3) == 0)
      {
         ls += 3;
         ll -= 3;
         while (ll && (*ls == ' ' || *ls == '\t'))
         {
            ls++;
            ll--;
         }
      }
      if (ll == blen && strncmp(ls, body, blen) == 0)
         return 1;
      if (!eol)
         break;
      p = eol + 1;
   }
   return 0;
}

char *cli_session_extract_response(const char *raw, const char *cli_kind, const char *baseline)
{
   char *text = cli_session_strip_ansi(raw ? raw : "");
   if (!text)
      return strdup("");

   int codex = (cli_kind && strstr(cli_kind, "codex") != NULL);
   const char *amark = codex ? "\xe2\x80\xa2" : "\xe2\x97\x8f"; /* • / ● assistant bullet */
   const char *umark = codex ? "\xe2\x80\xba" : "\xe2\x9d\xaf"; /* › / ❯ user prompt */

   /* Split a COPY into NUL-terminated lines (the split rewrites '\n' to '\0', so
    * `text` must stay intact for the fallback path and for sizing the result). */
   size_t textlen = strlen(text);
   char *work = strdup(text);
   if (!work)
      return text;
   size_t cap_lines = 256, n = 0;
   char **lines = malloc(cap_lines * sizeof(char *));
   if (!lines)
   {
      free(work);
      return text; /* degrade: whole stripped pane */
   }
   for (char *p = work;;)
   {
      char *nl = strchr(p, '\n');
      if (nl)
         *nl = '\0';
      if (n == cap_lines)
      {
         cap_lines *= 2;
         char **t2 = realloc(lines, cap_lines * sizeof(char *));
         if (!t2)
            break;
         lines = t2;
      }
      lines[n++] = p;
      if (!nl)
         break;
      p = nl + 1;
   }

   /* The answer is the assistant content of THIS turn: from the FIRST assistant
    * bullet not already in the baseline (pre-send) pane, through every following
    * bullet/continuation line, up to the trailing status/composer. Anchoring on
    * the assistant marker — not the user prompt — sidesteps the composer's greyed
    * placeholder (which also carries the user marker); excluding baseline bullets
    * drops prior turns still on a reused pane and any startup banner/hook that
    * fired before the turn; collecting ALL following bullets (not just the last)
    * keeps multi-paragraph / multi-bullet answers whole. */
   long block = -1;
   for (size_t i = 0; i < n; i++)
   {
      const char *t = cli_lstrip(lines[i]);
      if (strncmp(t, amark, 3) != 0)
         continue;
      const char *body = cli_lstrip(t + 3);
      if (!*body)
         continue;
      if (cli_body_in_baseline(baseline, amark, body))
         continue; /* this bullet was already on the pane before this turn */
      block = (long)i;
      break;
   }

   char *res = NULL;
   if (block >= 0)
   {
      size_t rcap = textlen + 2, rlen = 0;
      res = malloc(rcap);
      if (!res)
      {
         free(lines);
         free(work);
         return text;
      }
      res[0] = '\0';
      for (size_t i = (size_t)block; i < n; i++)
      {
         const char *t = cli_lstrip(lines[i]);
         if (strncmp(t, umark, 3) == 0)
            break; /* composer / next user turn ends the answer */
         if (cli_line_is_chrome(t))
            break; /* status (✻) / separator / footer ends the answer */
         const char *content = (strncmp(t, amark, 3) == 0) ? cli_lstrip(t + 3) : t;
         size_t cl = strlen(content);
         if (rlen + cl + 2 > rcap)
            break;
         memcpy(res + rlen, content, cl);
         rlen += cl;
         res[rlen++] = '\n';
         res[rlen] = '\0';
      }
      /* Trim trailing/leading blank lines and spaces. */
      while (rlen > 0 && (res[rlen - 1] == '\n' || res[rlen - 1] == ' ' || res[rlen - 1] == '\t'))
         res[--rlen] = '\0';
   }

   free(lines);
   free(work);
   if (res && res[0])
   {
      free(text);
      return res;
   }
   free(res);

   /* Fallback: no parseable bullet (e.g. a future TUI restyle). Return the
    * portion not already in the baseline so a reused pane still never replays a
    * prior turn, even if chrome leaks through. */
   if (baseline && baseline[0])
   {
      char *delta = cli_session_delta(baseline, text);
      free(text);
      return delta ? delta : strdup("");
   }
   return text;
}

int cli_session_recv(cli_session_t *s, char *out, size_t out_max, int timeout_ms)
{
   if (!s->active || !out || out_max == 0)
      return -1;

   int stable = 0;
   unsigned long prev_hash = 0;
   long long start = mono_ms();
   free(s->stream_emitted);
   s->stream_emitted = strdup("");

   for (;;)
   {
      if (!cli_session_is_alive(s))
      {
         out[0] = '\0';
         return -1;
      }

      /* Wall-clock backstop. Completion is detected by the pane going static
       * (stability hash below), but a CLI stuck in a provider retry/backoff
       * loop animates its spinner + elapsed-time counter forever, so the pane
       * never stabilises and the session never dies. Without this bound the
       * receive would hang indefinitely (the classic Anthropic-outage hang).
       * timeout_ms <= 0 preserves the legacy unbounded behaviour. */
      if (timeout_ms > 0 && (mono_ms() - start) >= timeout_ms)
      {
         out[0] = '\0';
         return -2;
      }

      msleep_ms(CLI_SESSION_POLL_MS);

      char cap[CLI_SESSION_BUF_MAX];
      if (cli_session_capture(s, cap, sizeof(cap)) != 0)
         continue;

      /* Stream the clean answer's growth as it is produced: extract this turn's
       * response so far and emit only the newly appended suffix. */
      if (g_stream_cb)
      {
         char *partial = cli_session_extract_response(cap, s->cli_kind, s->baseline);
         const char *prev = s->stream_emitted ? s->stream_emitted : "";
         size_t plen = strlen(prev);
         if (partial && strncmp(partial, prev, plen) == 0 && partial[plen])
         {
            g_stream_cb(partial + plen, g_stream_ud);
            free(s->stream_emitted);
            s->stream_emitted = partial;
         }
         else if (partial && strcmp(partial, prev) != 0)
         {
            /* A redraw rewrote earlier text (rare): adopt it silently, do not
             * re-emit, so the caller's transcript is not duplicated. */
            free(s->stream_emitted);
            s->stream_emitted = partial;
         }
         else
            free(partial);
      }

      unsigned long h = djb2(cap);
      if (h == prev_hash)
      {
         stable++;
         if (stable >= CLI_SESSION_STABLE_N)
         {
            char *resp = cli_session_extract_response(cap, s->cli_kind, s->baseline);
            snprintf(out, out_max, "%s", resp ? resp : "");
            free(resp);
            s->last_activity = time(NULL);
            return 0;
         }
      }
      else
      {
         stable = 0;
         prev_hash = h;
      }
   }
}

char *cli_session_make_name(const char *agent_name, const char *role)
{
   unsigned long h = djb2(role ? role : "default");
   char *name = malloc(CLI_SESSION_NAME_MAX);
   if (!name)
      return NULL;
   snprintf(name, CLI_SESSION_NAME_MAX, "aimee-%s-%08lx", agent_name, h & 0xffffffff);
   /* Replace chars invalid in tmux session names */
   for (char *p = name; *p; p++)
   {
      if (*p == ' ' || *p == '.' || *p == ':' || *p == '/')
         *p = '-';
   }
   return name;
}

char *cli_session_strip_ansi(const char *raw)
{
   if (!raw)
      return strdup("");
   size_t len = strlen(raw);
   char *out = malloc(len + 1);
   if (!out)
      return strdup("");

   size_t j = 0;
   for (size_t i = 0; i < len;)
   {
      if (raw[i] == '\033' && i + 1 < len && raw[i + 1] == '[')
      {
         /* Skip until letter */
         i += 2;
         while (i < len && (raw[i] < 'A' || raw[i] > 'Z') && (raw[i] < 'a' || raw[i] > 'z'))
            i++;
         if (i < len)
            i++; /* skip the terminal letter */
      }
      else
      {
         out[j++] = raw[i++];
      }
   }
   out[j] = '\0';
   return out;
}

char *cli_session_delta(const char *previous, const char *current)
{
   if (!current || !current[0])
      return strdup("");
   if (!previous || !previous[0])
      return strdup(current);

   size_t prev_len = strlen(previous);
   if (strncmp(previous, current, prev_len) == 0)
      return strdup(current + prev_len);

   return strdup(current);
}
