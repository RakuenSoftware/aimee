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
    * CLIENT (write via the provider), else the local fs. */
   int detached = sess_detached();
   char tmpfile[64];
   snprintf(tmpfile, sizeof(tmpfile), "/tmp/aimee_msg_%d.txt", (int)getpid());

   /* Build the message with a trailing newline for CLI submission. */
   size_t mlen = strlen(message);
   int need_nl = (mlen == 0 || message[mlen - 1] != '\n');
   size_t blen = mlen + (need_nl ? 1 : 0);
   char *buf = malloc(blen + 1);
   if (!buf)
      return -1;
   memcpy(buf, message, mlen);
   if (need_nl)
      buf[mlen] = '\n';
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

   snprintf(cmd, sizeof(cmd), "tmux load-buffer -b aimee_msg '%s' 2>/dev/null", tmpfile);
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

   snprintf(cmd, sizeof(cmd), "tmux paste-buffer -b aimee_msg -t '%s' -d 2>/dev/null",
            s->session_name);
   out = sess_run(cmd, &rc);
   free(out);

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

int cli_session_recv(cli_session_t *s, char *out, size_t out_max)
{
   if (!s->active || !out || out_max == 0)
      return -1;

   int stable = 0;
   unsigned long prev_hash = 0;

   for (;;)
   {
      if (!cli_session_is_alive(s))
      {
         out[0] = '\0';
         return -1;
      }

      msleep_ms(CLI_SESSION_POLL_MS);

      char cap[CLI_SESSION_BUF_MAX];
      if (cli_session_capture(s, cap, sizeof(cap)) != 0)
         continue;

      unsigned long h = djb2(cap);
      if (h == prev_hash)
      {
         stable++;
         if (stable >= CLI_SESSION_STABLE_N)
         {
            snprintf(out, out_max, "%s", cap);
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
