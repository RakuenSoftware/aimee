/* process_mgr.c: background process management for agent sessions.
 *
 * Provides start/kill/list/output operations on background processes.
 * A single reader thread polls all active process pipes via poll() and
 * appends captured lines to per-process ring buffers.
 */
#include "aimee.h"
#include "process_mgr.h"

#ifdef AIMEE_WINDOWS

#include <stdio.h>

int proc_start(const char *command, const char *cwd, char *errbuf, size_t errbuf_size)
{
   (void)command;
   (void)cwd;
   if (errbuf && errbuf_size > 0)
      snprintf(errbuf, errbuf_size, "error: background processes are not supported on Windows");
   return -1;
}

int proc_get_output(int id, int tail_lines, char *out, size_t out_size)
{
   (void)tail_lines;
   if (!out || out_size == 0)
      return -1;
   snprintf(out, out_size, "error: process id %d not found", id);
   return -1;
}

int proc_kill(int id)
{
   (void)id;
   return -1;
}

int proc_list(char *out, size_t out_size)
{
   if (!out || out_size == 0)
      return -1;
   snprintf(out, out_size, "[]");
   return 0;
}

void proc_cleanup_all(void)
{
}

#else

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <poll.h>
#include <signal.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* --- Constants --- */

#define PROC_STATUS_RUNNING 0
#define PROC_STATUS_EXITED  1
#define READER_POLL_MS      100

/* --- Types --- */

typedef struct
{
   char lines[PROC_OUTPUT_RING_LINES][PROC_MAX_LINE_LEN];
   int head;  /* index of next slot to write */
   int count; /* total lines stored (capped at PROC_OUTPUT_RING_LINES) */
} ring_buf_t;

typedef struct
{
   int id;
   pid_t pid;
   char command[512];
   int status;     /* PROC_STATUS_RUNNING or PROC_STATUS_EXITED */
   int is_service; /* If true, auto-restart on exit */
   int exit_code;  /* meaningful when status == PROC_STATUS_EXITED */
   time_t start_time;
   int stdout_fd;
   int stderr_fd;
   ring_buf_t ring; /* combined stdout+stderr ring buffer */
   /* partial line accumulators */
   char out_partial[PROC_MAX_LINE_LEN];
   int out_partial_len;
   char err_partial[PROC_MAX_LINE_LEN];
   int err_partial_len;
} managed_proc_t;

/* --- Global state --- */

static managed_proc_t g_procs[PROC_MAX_CONCURRENT];
static int g_proc_count = 0;
static int g_next_id = 1;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_reader_tid;
static int g_reader_running = 0;
static int g_shutdown_pipe[2] = {-1, -1};

/* --- Ring buffer helpers --- */

static void ring_push(ring_buf_t *r, const char *line)
{
   snprintf(r->lines[r->head], PROC_MAX_LINE_LEN, "%s", line);
   r->head = (r->head + 1) % PROC_OUTPUT_RING_LINES;
   if (r->count < PROC_OUTPUT_RING_LINES)
      r->count++;
}

/* --- Pipe flushing --- */

/* Drain fd into ring, splitting on newlines, accumulating partial lines in *partial.
 * Returns 1 if EOF reached (pipe closed), 0 otherwise. */
static int flush_fd(int fd, ring_buf_t *ring, char *partial, int *partial_len)
{
   char buf[4096];
   ssize_t n;
   int eof = 0;

   while ((n = read(fd, buf, sizeof(buf) - 1)) > 0)
   {
      buf[n] = '\0';
      for (ssize_t i = 0; i < n; i++)
      {
         char c = buf[i];
         if (c == '\n' || c == '\r')
         {
            if (*partial_len > 0)
            {
               partial[*partial_len] = '\0';
               ring_push(ring, partial);
               *partial_len = 0;
            }
         }
         else
         {
            if (*partial_len < PROC_MAX_LINE_LEN - 1)
               partial[(*partial_len)++] = c;
         }
      }
   }
   if (n == 0)
      eof = 1;
   else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
      eof = 1;
   return eof;
}

/* --- Reader thread --- */

static void *reader_thread(void *arg)
{
   (void)arg;
   while (1)
   {
      struct pollfd pfds[1 + (PROC_MAX_CONCURRENT * 2)];
      int proc_index[1 + (PROC_MAX_CONCURRENT * 2)];
      int proc_stream[1 + (PROC_MAX_CONCURRENT * 2)];
      nfds_t nfds = 0;

      pfds[nfds].fd = g_shutdown_pipe[0];
      pfds[nfds].events = POLLIN;
      pfds[nfds].revents = 0;
      proc_index[nfds] = -1;
      proc_stream[nfds] = -1;
      nfds++;

      pthread_mutex_lock(&g_lock);
      for (int i = 0; i < g_proc_count; i++)
      {
         managed_proc_t *p = &g_procs[i];
         if (p->status == PROC_STATUS_RUNNING)
         {
            if (p->stdout_fd >= 0)
            {
               pfds[nfds].fd = p->stdout_fd;
               pfds[nfds].events = POLLIN | POLLHUP | POLLERR;
               pfds[nfds].revents = 0;
               proc_index[nfds] = i;
               proc_stream[nfds] = 0;
               nfds++;
            }
            if (p->stderr_fd >= 0)
            {
               pfds[nfds].fd = p->stderr_fd;
               pfds[nfds].events = POLLIN | POLLHUP | POLLERR;
               pfds[nfds].revents = 0;
               proc_index[nfds] = i;
               proc_stream[nfds] = 1;
               nfds++;
            }
         }
      }
      pthread_mutex_unlock(&g_lock);

      int ready = poll(pfds, nfds, READER_POLL_MS);

      if (ready < 0)
      {
         if (errno == EINTR)
            continue;
         break;
      }
      if (ready == 0)
         continue;

      /* Check shutdown signal */
      if (pfds[0].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))
         break;

      pthread_mutex_lock(&g_lock);
      for (nfds_t fi = 1; fi < nfds; fi++)
      {
         if (!(pfds[fi].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)))
            continue;

         int i = proc_index[fi];
         if (i < 0 || i >= g_proc_count)
            continue;

         managed_proc_t *p = &g_procs[i];
         if (p->status != PROC_STATUS_RUNNING)
            continue;

         int out_eof = 0, err_eof = 0;

         if (proc_stream[fi] == 0 && p->stdout_fd == pfds[fi].fd)
            out_eof = flush_fd(p->stdout_fd, &p->ring, p->out_partial, &p->out_partial_len);
         if (proc_stream[fi] == 1 && p->stderr_fd == pfds[fi].fd)
            err_eof = flush_fd(p->stderr_fd, &p->ring, p->err_partial, &p->err_partial_len);

         if (out_eof && p->stdout_fd >= 0)
         {
            close(p->stdout_fd);
            p->stdout_fd = -1;
         }
         if (err_eof && p->stderr_fd >= 0)
         {
            close(p->stderr_fd);
            p->stderr_fd = -1;
         }

         /* If both pipes closed, wait for exit status */
         if (p->stdout_fd < 0 && p->stderr_fd < 0)
         {
            int wstatus = 0;
            pid_t waited = waitpid(p->pid, &wstatus, WNOHANG);
            if (waited == p->pid)
            {
               if (WIFEXITED(wstatus))
                  p->exit_code = WEXITSTATUS(wstatus);
               else if (WIFSIGNALED(wstatus))
                  p->exit_code = 128 + WTERMSIG(wstatus);
               else
                  p->exit_code = 0;
            }
            else if (waited < 0)
            {
               p->exit_code = -1;
            }
            /* Service respawn remains a follow-up; for now we at least capture exit state. */
            p->status = PROC_STATUS_EXITED;
         }
      }
      pthread_mutex_unlock(&g_lock);
   }
   return NULL;
}

static void ensure_reader_running(void)
{
   if (g_reader_running)
      return;
   if (pipe(g_shutdown_pipe) != 0)
      return;
   pthread_attr_t attr;
   pthread_attr_init(&attr);
   pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
   if (pthread_create(&g_reader_tid, &attr, reader_thread, NULL) == 0)
      g_reader_running = 1;
   pthread_attr_destroy(&attr);
}

/* --- Public API --- */

int proc_start(const char *command, const char *cwd, char *errbuf, size_t errbuf_size)
{
   if (!command || command[0] == '\0')
   {
      if (errbuf && errbuf_size > 0)
         snprintf(errbuf, errbuf_size, "error: empty command");
      return -1;
   }

   pthread_mutex_lock(&g_lock);

   /* Count currently running processes */
   int running = 0;
   for (int i = 0; i < g_proc_count; i++)
      if (g_procs[i].status == PROC_STATUS_RUNNING)
         running++;

   if (running >= PROC_MAX_CONCURRENT)
   {
      pthread_mutex_unlock(&g_lock);
      if (errbuf && errbuf_size > 0)
         snprintf(errbuf, errbuf_size,
                  "error: concurrent process limit (%d) reached — kill a process first",
                  PROC_MAX_CONCURRENT);
      return -1;
   }

   /* Find a free slot (reuse exited entries) */
   int slot = -1;
   for (int i = 0; i < g_proc_count; i++)
   {
      if (g_procs[i].status == PROC_STATUS_EXITED && g_procs[i].stdout_fd < 0 &&
          g_procs[i].stderr_fd < 0)
      {
         slot = i;
         break;
      }
   }
   if (slot < 0)
   {
      if (g_proc_count >= PROC_MAX_CONCURRENT)
      {
         pthread_mutex_unlock(&g_lock);
         if (errbuf && errbuf_size > 0)
            snprintf(errbuf, errbuf_size,
                     "error: process table full — concurrent process limit (%d) reached",
                     PROC_MAX_CONCURRENT);
         return -1;
      }
      slot = g_proc_count++;
   }

   /* Create pipes */
   int stdout_pipe[2], stderr_pipe[2];
   if (pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0)
   {
      pthread_mutex_unlock(&g_lock);
      if (errbuf && errbuf_size > 0)
         snprintf(errbuf, errbuf_size, "error: pipe() failed: %s", strerror(errno));
      return -1;
   }

   long child_fd_limit = sysconf(_SC_OPEN_MAX);
   if (child_fd_limit < 3)
      child_fd_limit = 1024;

   pid_t pid = fork();
   if (pid < 0)
   {
      close(stdout_pipe[0]);
      close(stdout_pipe[1]);
      close(stderr_pipe[0]);
      close(stderr_pipe[1]);
      pthread_mutex_unlock(&g_lock);
      if (errbuf && errbuf_size > 0)
         snprintf(errbuf, errbuf_size, "error: fork() failed: %s", strerror(errno));
      return -1;
   }

   if (pid == 0)
   {
      /* Child */
      if (cwd && cwd[0] != '\0')
         (void)chdir(cwd);

      /* Redirect stdout and stderr */
      dup2(stdout_pipe[1], STDOUT_FILENO);
      dup2(stderr_pipe[1], STDERR_FILENO);

      /* Docker commonly grants a 1,048,576 descriptor ceiling. Walking that
       * entire range before every exec makes even `echo` take hundreds of
       * milliseconds. Use the kernel's constant-time range close when present;
       * the pre-fork limit feeds the portable, async-signal-safe fallback. */
#if defined(__linux__) && defined(SYS_close_range)
      if (syscall(SYS_close_range, 3u, ~0u, 0u) != 0)
#endif
         for (long fd = 3; fd < child_fd_limit; fd++)
            close((int)fd);

      execl("/bin/sh", "sh", "-c", command, (char *)NULL);
      _exit(127);
   }

   /* Parent: close write ends */
   close(stdout_pipe[1]);
   close(stderr_pipe[1]);

   /* Set read ends non-blocking */
   fcntl(stdout_pipe[0], F_SETFL, fcntl(stdout_pipe[0], F_GETFL) | O_NONBLOCK);
   fcntl(stderr_pipe[0], F_SETFL, fcntl(stderr_pipe[0], F_GETFL) | O_NONBLOCK);

   /* Initialize slot */
   managed_proc_t *p = &g_procs[slot];
   memset(p, 0, sizeof(*p));
   p->id = g_next_id++;
   p->pid = pid;
   snprintf(p->command, sizeof(p->command), "%s", command);
   p->status = PROC_STATUS_RUNNING;
   p->exit_code = 0;
   p->start_time = time(NULL);
   p->stdout_fd = stdout_pipe[0];
   p->stderr_fd = stderr_pipe[0];

   int new_id = p->id;

   ensure_reader_running();
   pthread_mutex_unlock(&g_lock);
   return new_id;
}

int proc_get_output(int id, int tail_lines, char *out, size_t out_size)
{
   if (!out || out_size == 0)
      return -1;

   if (tail_lines <= 0)
      tail_lines = 50;
   if (tail_lines > PROC_OUTPUT_RING_LINES)
      tail_lines = PROC_OUTPUT_RING_LINES;

   pthread_mutex_lock(&g_lock);

   managed_proc_t *found = NULL;
   for (int i = 0; i < g_proc_count; i++)
   {
      if (g_procs[i].id == id)
      {
         found = &g_procs[i];
         break;
      }
   }

   if (!found)
   {
      pthread_mutex_unlock(&g_lock);
      snprintf(out, out_size, "error: process id %d not found", id);
      return -1;
   }

   /* Copy the ring buffer contents into a local snapshot to avoid
    * holding the lock while building the output string. */
   ring_buf_t snap = found->ring;
   pthread_mutex_unlock(&g_lock);

   int count = snap.count;
   if (tail_lines > count)
      tail_lines = count;

   /* Compute starting index: walk back `tail_lines` entries from head */
   int start = (snap.head - tail_lines + PROC_OUTPUT_RING_LINES) % PROC_OUTPUT_RING_LINES;

   size_t written = 0;
   for (int i = 0; i < tail_lines; i++)
   {
      int idx = (start + i) % PROC_OUTPUT_RING_LINES;
      int n = snprintf(out + written, out_size - written, "%s\n", snap.lines[idx]);
      if (n < 0 || (size_t)n >= out_size - written)
         break;
      written += (size_t)n;
   }
   if (written == 0 && out_size > 0)
      out[0] = '\0';
   return 0;
}

int proc_kill(int id)
{
   pthread_mutex_lock(&g_lock);

   managed_proc_t *found = NULL;
   for (int i = 0; i < g_proc_count; i++)
   {
      if (g_procs[i].id == id)
      {
         found = &g_procs[i];
         break;
      }
   }

   if (!found || found->status != PROC_STATUS_RUNNING)
   {
      pthread_mutex_unlock(&g_lock);
      return -1;
   }

   kill(found->pid, SIGTERM);
   found->status = PROC_STATUS_EXITED;

   /* Harvest exit status */
   int wstatus = 0;
   waitpid(found->pid, &wstatus, 0);
   found->exit_code = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : -1;

   if (found->stdout_fd >= 0)
   {
      close(found->stdout_fd);
      found->stdout_fd = -1;
   }
   if (found->stderr_fd >= 0)
   {
      close(found->stderr_fd);
      found->stderr_fd = -1;
   }

   pthread_mutex_unlock(&g_lock);
   return 0;
}

int proc_list(char *out, size_t out_size)
{
   if (!out || out_size == 0)
      return -1;

   pthread_mutex_lock(&g_lock);

   size_t pos = 0;
   int n = snprintf(out + pos, out_size - pos, "[");
   if (n < 0 || (size_t)n >= out_size - pos)
   {
      pthread_mutex_unlock(&g_lock);
      return -1;
   }
   pos += (size_t)n;

   int first = 1;
   for (int i = 0; i < g_proc_count; i++)
   {
      managed_proc_t *p = &g_procs[i];
      const char *status_str = (p->status == PROC_STATUS_RUNNING) ? "running" : "exited";
      n = snprintf(out + pos, out_size - pos,
                   "%s{\"id\":%d,\"pid\":%d,\"command\":\"%s\","
                   "\"status\":\"%s\",\"exit_code\":%d}",
                   first ? "" : ",", p->id, (int)p->pid, p->command, status_str, p->exit_code);
      if (n < 0 || (size_t)n >= out_size - pos)
         break;
      pos += (size_t)n;
      first = 0;
   }

   n = snprintf(out + pos, out_size - pos, "]");
   if (n < 0 || (size_t)n >= out_size - pos)
      out[out_size - 1] = '\0';
   else
      pos += (size_t)n;

   pthread_mutex_unlock(&g_lock);
   return 0;
}

void proc_cleanup_all(void)
{
   pthread_mutex_lock(&g_lock);

   /* Signal reader thread to stop */
   if (g_reader_running && g_shutdown_pipe[1] >= 0)
   {
      char b = 1;
      (void)write(g_shutdown_pipe[1], &b, 1);
   }

   /* Kill all running processes */
   for (int i = 0; i < g_proc_count; i++)
   {
      managed_proc_t *p = &g_procs[i];
      if (p->status == PROC_STATUS_RUNNING)
      {
         kill(p->pid, SIGTERM);
         waitpid(p->pid, NULL, 0);
         p->status = PROC_STATUS_EXITED;
      }
      if (p->stdout_fd >= 0)
      {
         close(p->stdout_fd);
         p->stdout_fd = -1;
      }
      if (p->stderr_fd >= 0)
      {
         close(p->stderr_fd);
         p->stderr_fd = -1;
      }
   }
   g_proc_count = 0;
   g_next_id = 1;

   if (g_shutdown_pipe[0] >= 0)
   {
      close(g_shutdown_pipe[0]);
      g_shutdown_pipe[0] = -1;
   }
   if (g_shutdown_pipe[1] >= 0)
   {
      close(g_shutdown_pipe[1]);
      g_shutdown_pipe[1] = -1;
   }
   g_reader_running = 0;

   pthread_mutex_unlock(&g_lock);
}

#endif /* AIMEE_WINDOWS */
