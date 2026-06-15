/* cli_session_pty.c: server-hosted, PTY-attached CLI sessions (see header). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "cli_session_pty.h"
#include "cli_session.h"
#include "util.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define PTY_MAX_SESSIONS 16
#define PTY_INPUT_CAP    (64 * 1024)
#define PTY_READ_CHUNK   4096
#define PTY_HEARTBEAT_MS 15000

typedef struct
{
   int used;
   /* ready: the slot is fully initialised (master_fd/wake_pipe/pid published) and
    * safe to drive. ensure() reserves the slot (used=1, ready=0), builds tmux +
    * the forkpty child with g_lock RELEASED (the work is slow), then publishes
    * the fds under the lock and sets ready=1. input/resize/stream reject a
    * not-ready slot, and a kill() racing setup only flags `killing` so the
    * setup thread tears its own resources down — neither side touches a
    * half-initialised slot. */
   int ready;
   char id[128];
   char tmux_name[128];
   int master_fd;    /* PTY master (slot-owned) */
   pid_t pid;        /* forkpty child (tmux attach / override) */
   int wake_pipe[2]; /* self-pipe: [0] read by pump, [1] written by enqueuers */
   unsigned char *input;
   size_t input_len;
   int pend_rows, pend_cols;
   int resize_pending;
   int streaming; /* a stream pump currently owns the loop */
   int killing;   /* kill requested; pump should bail and free */
} pty_sess_t;

static pty_sess_t g_sessions[PTY_MAX_SESSIONS];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_attach_override[256];
static int g_forwarding_enabled = 0;

void cli_session_pty_set_forwarding(int on)
{
   g_forwarding_enabled = on ? 1 : 0;
}
int cli_session_pty_forwarding_enabled(void)
{
   return g_forwarding_enabled;
}

/* ── small helpers ──────────────────────────────────────────────────────── */

static int write_all(int fd, const char *buf, size_t len)
{
   size_t off = 0;
   while (off < len)
   {
      ssize_t w = write(fd, buf + off, len - off);
      if (w < 0)
      {
         if (errno == EINTR)
            continue;
         return -1;
      }
      if (w == 0)
         return -1;
      off += (size_t)w;
   }
   return 0;
}

static int find_locked(const char *id)
{
   for (int i = 0; i < PTY_MAX_SESSIONS; i++)
      if (g_sessions[i].used && strcmp(g_sessions[i].id, id) == 0)
         return i;
   return -1;
}

static int alloc_locked(void)
{
   for (int i = 0; i < PTY_MAX_SESSIONS; i++)
      if (!g_sessions[i].used)
         return i;
   return -1;
}

/* Free a slot's resources. Caller holds g_lock. Closes the PTY master, drains
 * the child, tears down the tmux session, and clears the slot. */
static void slot_free_locked(pty_sess_t *s)
{
   if (s->master_fd >= 0)
      close(s->master_fd);
   if (s->wake_pipe[0] >= 0)
      close(s->wake_pipe[0]);
   if (s->wake_pipe[1] >= 0)
      close(s->wake_pipe[1]);
   if (s->pid > 0)
   {
      kill(s->pid, SIGKILL);
      waitpid(s->pid, NULL, 0);
   }
   if (s->tmux_name[0])
   {
      char cmd[CLI_SESSION_NAME_MAX + 64];
      snprintf(cmd, sizeof(cmd), "tmux kill-session -t '%s' 2>/dev/null", s->tmux_name);
      int rc;
      char *out = run_cmd(cmd, &rc);
      free(out);
   }
   free(s->input);
   memset(s, 0, sizeof(*s));
}

void cli_session_pty_set_attach_override(const char *cmd)
{
   pthread_mutex_lock(&g_lock);
   snprintf(g_attach_override, sizeof(g_attach_override), "%s", cmd ? cmd : "");
   pthread_mutex_unlock(&g_lock);
}

/* ── lifecycle ──────────────────────────────────────────────────────────── */

int cli_session_pty_ensure(const char *id, const char *cli_cmd, const char *work_dir, int rows,
                           int cols, char *err, size_t errn)
{
   if (err && errn)
      err[0] = '\0';
   if (!id || !id[0])
   {
      if (err)
         snprintf(err, errn, "missing session id");
      return -1;
   }

   pthread_mutex_lock(&g_lock);
   if (find_locked(id) >= 0)
   {
      pthread_mutex_unlock(&g_lock);
      return 0; /* already live */
   }
   int idx = alloc_locked();
   if (idx < 0)
   {
      pthread_mutex_unlock(&g_lock);
      if (err)
         snprintf(err, errn, "too many PTY sessions");
      return -1;
   }
   pty_sess_t *s = &g_sessions[idx];
   memset(s, 0, sizeof(*s));
   s->used = 1; /* reserve the slot for the duration of setup (find/alloc see it) */
   s->master_fd = -1;
   s->wake_pipe[0] = s->wake_pipe[1] = -1;
   snprintf(s->id, sizeof(s->id), "%s", id);

   char override[256];
   snprintf(override, sizeof(override), "%s", g_attach_override);
   pthread_mutex_unlock(&g_lock);

   if (rows <= 0)
      rows = 50;
   if (cols <= 0)
      cols = 220;

   /* Build the tmux session (production) or run the override directly (tests). */
   char attach_cmd[512];
   if (override[0])
   {
      snprintf(attach_cmd, sizeof(attach_cmd), "%s", override);
   }
   else
   {
      char *name = cli_session_make_name(id, "pty");
      if (!name)
      {
         if (err)
            snprintf(err, errn, "out of memory");
         goto fail_unalloc;
      }
      cli_session_t tsess;
      int rc = cli_session_create(&tsess, name, (cli_cmd && cli_cmd[0]) ? cli_cmd : "claude",
                                  work_dir, 1 /*reuse*/);
      if (rc != 0)
      {
         free(name);
         if (err)
            snprintf(err, errn, "failed to create tmux session");
         goto fail_unalloc;
      }
      pthread_mutex_lock(&g_lock);
      snprintf(s->tmux_name, sizeof(s->tmux_name), "%s", name);
      pthread_mutex_unlock(&g_lock);
      char *esc = shell_escape(name);
      snprintf(attach_cmd, sizeof(attach_cmd), "tmux attach -t %s", esc ? esc : name);
      free(esc);
      free(name);
   }

   int wp[2];
   if (pipe(wp) != 0)
   {
      if (err)
         snprintf(err, errn, "pipe failed");
      goto fail_killtmux;
   }
   fcntl(wp[0], F_SETFL, O_NONBLOCK);
   fcntl(wp[1], F_SETFL, O_NONBLOCK);

   struct winsize ws = {0};
   ws.ws_row = (unsigned short)rows;
   ws.ws_col = (unsigned short)cols;

   int master = -1;
   pid_t pid = forkpty(&master, NULL, NULL, &ws);
   if (pid < 0)
   {
      close(wp[0]);
      close(wp[1]);
      if (err)
         snprintf(err, errn, "forkpty failed");
      goto fail_killtmux;
   }
   if (pid == 0)
   {
      /* Child: exec the attach command on the PTY slave (now our ctty). */
      execl("/bin/sh", "sh", "-c", attach_cmd, (char *)NULL);
      _exit(127);
   }

   pthread_mutex_lock(&g_lock);
   s->master_fd = master;
   s->pid = pid;
   s->wake_pipe[0] = wp[0];
   s->wake_pipe[1] = wp[1];
   s->pend_rows = rows;
   s->pend_cols = cols;
   if (s->killing)
   {
      /* A kill() arrived while we were building (it only flagged `killing` for a
       * not-ready slot). Now that the fds are published, tear them down here. */
      slot_free_locked(s);
      pthread_mutex_unlock(&g_lock);
      if (err)
         snprintf(err, errn, "session killed during setup");
      return -1;
   }
   s->ready = 1;
   pthread_mutex_unlock(&g_lock);
   return 0;

fail_killtmux:
   if (s->tmux_name[0])
   {
      char cmd[CLI_SESSION_NAME_MAX + 64];
      snprintf(cmd, sizeof(cmd), "tmux kill-session -t '%s' 2>/dev/null", s->tmux_name);
      int rc;
      char *out = run_cmd(cmd, &rc);
      free(out);
   }
fail_unalloc:
   pthread_mutex_lock(&g_lock);
   memset(s, 0, sizeof(*s));
   pthread_mutex_unlock(&g_lock);
   return -1;
}

int cli_session_pty_input(const char *id, const unsigned char *data, size_t len)
{
   if (!id || !data || len == 0)
      return -1;
   pthread_mutex_lock(&g_lock);
   int idx = find_locked(id);
   if (idx < 0 || !g_sessions[idx].ready)
   {
      pthread_mutex_unlock(&g_lock);
      return -1;
   }
   pty_sess_t *s = &g_sessions[idx];
   if (s->input_len + len > PTY_INPUT_CAP)
   {
      pthread_mutex_unlock(&g_lock);
      return -1; /* backpressure: drop oversized burst rather than grow unbounded */
   }
   unsigned char *grown = realloc(s->input, s->input_len + len);
   if (!grown)
   {
      pthread_mutex_unlock(&g_lock);
      return -1;
   }
   s->input = grown;
   memcpy(s->input + s->input_len, data, len);
   s->input_len += len;
   if (s->wake_pipe[1] >= 0)
   {
      ssize_t wr = write(s->wake_pipe[1], "x", 1);
      (void)wr;
   }
   pthread_mutex_unlock(&g_lock);
   return 0;
}

int cli_session_pty_resize(const char *id, int rows, int cols)
{
   if (!id || rows <= 0 || cols <= 0)
      return -1;
   pthread_mutex_lock(&g_lock);
   int idx = find_locked(id);
   if (idx < 0 || !g_sessions[idx].ready)
   {
      pthread_mutex_unlock(&g_lock);
      return -1;
   }
   pty_sess_t *s = &g_sessions[idx];
   s->pend_rows = rows;
   s->pend_cols = cols;
   s->resize_pending = 1;
   if (s->wake_pipe[1] >= 0)
   {
      ssize_t wr = write(s->wake_pipe[1], "x", 1);
      (void)wr;
   }
   pthread_mutex_unlock(&g_lock);
   return 0;
}

/* Drain queued input to the PTY and apply a pending resize. Caller holds the
 * lock; returns the master fd to operate on (or -1 if the slot vanished). */
static void pump_apply_locked(pty_sess_t *s)
{
   if (s->input_len > 0 && s->master_fd >= 0)
   {
      /* Best-effort write; on short write keep the remainder queued. */
      ssize_t w = write(s->master_fd, s->input, s->input_len);
      if (w > 0)
      {
         size_t rem = s->input_len - (size_t)w;
         if (rem > 0)
            memmove(s->input, s->input + w, rem);
         s->input_len = rem;
      }
   }
   if (s->resize_pending && s->master_fd >= 0)
   {
      struct winsize ws = {0};
      ws.ws_row = (unsigned short)s->pend_rows;
      ws.ws_col = (unsigned short)s->pend_cols;
      ioctl(s->master_fd, TIOCSWINSZ, &ws);
      if (s->pid > 0)
         kill(s->pid, SIGWINCH);
      s->resize_pending = 0;
   }
}

int cli_session_pty_stream(const char *id, int client_fd)
{
   pthread_mutex_lock(&g_lock);
   int idx = find_locked(id);
   if (idx < 0 || !g_sessions[idx].ready || g_sessions[idx].streaming)
   {
      pthread_mutex_unlock(&g_lock);
      return -1;
   }
   pty_sess_t *s = &g_sessions[idx];
   s->streaming = 1;
   int master = s->master_fd;
   int wake = s->wake_pipe[0];
   pthread_mutex_unlock(&g_lock);

   unsigned char rbuf[PTY_READ_CHUNK];
   char *frame = malloc(aimee_base64_encoded_len(PTY_READ_CHUNK) + 16);
   int rc = 0;
   if (!frame)
      rc = -1;

   while (rc == 0)
   {
      struct pollfd pfds[2];
      pfds[0].fd = master;
      pfds[0].events = POLLIN;
      pfds[0].revents = 0;
      pfds[1].fd = wake;
      pfds[1].events = POLLIN;
      pfds[1].revents = 0;
      int pr = poll(pfds, 2, PTY_HEARTBEAT_MS);
      if (pr < 0)
      {
         if (errno == EINTR)
            continue;
         break;
      }

      /* Kill requested? */
      pthread_mutex_lock(&g_lock);
      int killing = s->killing;
      pthread_mutex_unlock(&g_lock);
      if (killing)
         break;

      if (pr == 0)
      {
         /* Heartbeat doubles as a client-hangup probe. */
         if (write_all(client_fd, ":hb\n\n", 5) != 0)
            break;
         continue;
      }

      if (pfds[1].revents & POLLIN)
      {
         char drain[256];
         while (read(wake, drain, sizeof(drain)) > 0)
            ;
         pthread_mutex_lock(&g_lock);
         pump_apply_locked(s);
         pthread_mutex_unlock(&g_lock);
      }

      if (pfds[0].revents & (POLLIN | POLLHUP))
      {
         ssize_t n = read(master, rbuf, sizeof(rbuf));
         if (n <= 0)
            break; /* EOF: the CLI/tmux client exited */
         size_t b64n = aimee_base64_encode(rbuf, (size_t)n, frame + 6,
                                           aimee_base64_encoded_len(PTY_READ_CHUNK) + 10);
         memcpy(frame, "data: ", 6);
         frame[6 + b64n] = '\n';
         frame[6 + b64n + 1] = '\n';
         if (write_all(client_fd, frame, 6 + b64n + 2) != 0)
            break; /* client hangup */
      }
      else if (pfds[0].revents & POLLERR)
      {
         break;
      }
   }

   free(frame);

   /* "Last one out frees": if a kill was requested while we streamed, the slot
    * is ours to tear down; otherwise leave it live for reattach. */
   pthread_mutex_lock(&g_lock);
   s->streaming = 0;
   if (s->killing)
      slot_free_locked(s);
   pthread_mutex_unlock(&g_lock);
   return 0;
}

void cli_session_pty_kill(const char *id)
{
   if (!id)
      return;
   pthread_mutex_lock(&g_lock);
   int idx = find_locked(id);
   if (idx < 0)
   {
      pthread_mutex_unlock(&g_lock);
      return;
   }
   pty_sess_t *s = &g_sessions[idx];
   if (!s->ready)
   {
      /* Setup still running with the lock released: only flag it. The ensure()
       * thread publishes the fds under the lock and frees the slot itself when
       * it sees `killing`, so we never tear down a half-initialised slot. */
      s->killing = 1;
      pthread_mutex_unlock(&g_lock);
      return;
   }
   if (s->streaming)
   {
      /* A pump owns the loop: ask it to bail and free on its way out. */
      s->killing = 1;
      if (s->pid > 0)
         kill(s->pid, SIGKILL);
      if (s->wake_pipe[1] >= 0)
      {
         ssize_t wr = write(s->wake_pipe[1], "x", 1);
         (void)wr;
      }
      pthread_mutex_unlock(&g_lock);
      return;
   }
   slot_free_locked(s);
   pthread_mutex_unlock(&g_lock);
}
