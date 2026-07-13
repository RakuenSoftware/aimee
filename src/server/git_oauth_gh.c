/* git_oauth_gh.c — GitHub sign-in via the bundled `gh` CLI (see git_oauth_gh.h). */
#define _GNU_SOURCE 1 /* execvpe */

#include "git_oauth_gh.h"

#include "git_host_cred.h" /* git_host_cred_set */
#include "util.h"          /* safe_exec_capture(_env) */

#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define GH_VERIFY_URI "https://github.com/login/device"
/* Wall-clock cap on one attempt (GitHub device codes expire in ~15 min). */
#define GH_SESSION_DEADLINE_SEC 1200
/* How long start() waits for gh to print the one-time code. */
#define GH_CODE_WAIT_SEC 30

/* Single-user server: one sign-in session at a time, mutex-guarded. DONE/ERROR
 * are one-shot results consumed by poll(). g_gen invalidates a stale reader
 * thread after the session it served is abandoned. */
enum gh_state
{
   GH_IDLE = 0,
   GH_RUNNING,
   GH_DONE,
   GH_ERROR
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static enum gh_state g_state = GH_IDLE;
static unsigned g_gen;
static pid_t g_pid = -1;
static char g_code[32];
static char g_principal[256];
static char g_err[256];

struct gh_thread_arg
{
   unsigned gen;
   pid_t pid;
   int master;
   char dir[128];
};

static int gh_on_path(void)
{
   const char *path = getenv("PATH");
   if (!path || !path[0])
      path = "/usr/local/bin:/usr/bin:/bin";
   char probe[512];
   while (*path)
   {
      const char *sep = strchr(path, ':');
      size_t len = sep ? (size_t)(sep - path) : strlen(path);
      if (len && len < sizeof(probe) - 4)
      {
         snprintf(probe, sizeof(probe), "%.*s/gh", (int)len, path);
         if (access(probe, X_OK) == 0)
            return 1;
      }
      path += len;
      if (*path == ':')
         path++;
   }
   return 0;
}

int git_oauth_gh_available(void)
{
   return gh_on_path();
}

/* Child environment: ephemeral config/home so nothing persists outside the
 * session dir, no real browser, no colors/TUI redraws to confuse the parser. */
#define GH_ENV_SLOTS 8
#define GH_ENV_LEN   640
static void build_env(const char *dir, char buf[GH_ENV_SLOTS][GH_ENV_LEN],
                      char *envp[GH_ENV_SLOTS + 1])
{
   const char *path = getenv("PATH");
   snprintf(buf[0], GH_ENV_LEN, "PATH=%s",
            (path && path[0]) ? path : "/usr/local/bin:/usr/bin:/bin");
   snprintf(buf[1], GH_ENV_LEN, "HOME=%s", dir);
   snprintf(buf[2], GH_ENV_LEN, "GH_CONFIG_DIR=%s", dir);
   snprintf(buf[3], GH_ENV_LEN, "BROWSER=/bin/true");
   snprintf(buf[4], GH_ENV_LEN, "NO_COLOR=1");
   snprintf(buf[5], GH_ENV_LEN, "TERM=dumb");
   snprintf(buf[6], GH_ENV_LEN, "GH_NO_UPDATE_NOTIFIER=1");
   snprintf(buf[7], GH_ENV_LEN, "GIT_TERMINAL_PROMPT=0");
   for (int i = 0; i < GH_ENV_SLOTS; i++)
      envp[i] = buf[i];
   envp[GH_ENV_SLOTS] = NULL;
}

/* Strip ANSI escape sequences + carriage returns in place (gh renders on a PTY,
 * so its output can carry styling/cursor moves despite NO_COLOR/TERM=dumb).
 * Non-static: test seam. */
void git_oauth_gh_strip_term_noise(char *s)
{
   char *w = s;
   const char *r = s;
   while (*r)
   {
      if (*r == 0x1b)
      {
         r++;
         if (*r == '[')
         {
            r++;
            while (*r && ((unsigned char)*r < 0x40 || (unsigned char)*r > 0x7e))
               r++;
            if (*r)
               r++;
         }
         else if (*r == ']')
         {
            r++;
            while (*r && *r != 0x07 && *r != 0x1b)
               r++;
            if (*r == 0x07)
               r++;
         }
         else if (*r)
            r++;
         continue;
      }
      if (*r == '\r')
      {
         r++;
         continue;
      }
      *w++ = *r++;
   }
   *w = '\0';
}

/* Find the XXXX-XXXX one-time code after gh's "one-time code:" marker.
 * Non-static: test seam. */
int git_oauth_gh_parse_code(const char *text, char *out, size_t cap)
{
   const char *m = strstr(text, "one-time code");
   if (!m || cap < 10)
      return 0;
   for (const char *p = m; *p; p++)
   {
      int i = 0;
      while (i < 4 && (isupper((unsigned char)p[i]) || isdigit((unsigned char)p[i])))
         i++;
      if (i != 4 || p[4] != '-')
         continue;
      int j = 0;
      while (j < 4 && (isupper((unsigned char)p[5 + j]) || isdigit((unsigned char)p[5 + j])))
         j++;
      if (j == 4 && !isalnum((unsigned char)p[9]))
      {
         memcpy(out, p, 9);
         out[9] = '\0';
         return 1;
      }
   }
   return 0;
}

/* Last non-blank line of gh's (stripped) output, printable chars only — the
 * closest thing gh gives to a machine-readable error. */
static void last_line(const char *text, char *out, size_t cap)
{
   if (!cap)
      return;
   out[0] = '\0';
   const char *best = NULL, *best_end = NULL;
   const char *p = text;
   while (*p)
   {
      const char *nl = strchr(p, '\n');
      const char *end = nl ? nl : p + strlen(p);
      const char *a = p, *b = end;
      while (a < b && isspace((unsigned char)*a))
         a++;
      while (b > a && isspace((unsigned char)b[-1]))
         b--;
      if (b > a)
      {
         best = a;
         best_end = b;
      }
      if (!nl)
         break;
      p = nl + 1;
   }
   if (!best)
      return;
   size_t n = 0;
   for (const char *q = best; q < best_end && n + 1 < cap; q++)
      if (isprint((unsigned char)*q))
         out[n++] = *q;
   out[n] = '\0';
}

/* Delete the ephemeral session dir (hosts.yml may hold the token until the
 * vault copy is made, so always clean up). */
static void remove_dir(const char *dir)
{
   if (!dir || strncmp(dir, "/tmp/aimee-gh-", 14) != 0)
      return;
   const char *argv[] = {"rm", "-rf", dir, NULL};
   char *out = NULL;
   safe_exec_capture(argv, &out, 256);
   free(out);
}

static long long now_ms(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Reader: pump the PTY until gh exits, playing just enough terminal to keep
 * gh's prompter (survey) moving. Two rules, established against real gh:
 *
 *  1. Answer every cursor-position query (ESC[6n) IMMEDIATELY with a report
 *     (ESC[1;1R) — the prompter blocks on the reply, and nothing else may be
 *     interleaved into that exchange (a stray byte corrupts the parse and the
 *     prompt hangs forever).
 *  2. Answer the interactive beats (the optional "Authenticate Git" question,
 *     the "Press Enter to open ..." pause) with '\r' — the prompter is in raw
 *     mode, so Enter is CR, not NL — and only after a ≥400ms output lull, so
 *     the reply lands when the prompter is back in its input loop rather than
 *     mid-query.
 *
 * Publishes the one-time code as soon as it appears; on a clean exit copies
 * the token into the credential store. Updates the shared state only while
 * its generation is current. */
static void *gh_reader(void *argp)
{
   struct gh_thread_arg a = *(struct gh_thread_arg *)argp;
   free(argp);

   char raw[16384];
   size_t rawlen = 0;
   size_t scan_6n = 0; /* raw offset already scanned for ESC[6n */
   int answered_git = 0, answered_enter = 0, have_code = 0;
   time_t deadline = time(NULL) + GH_SESSION_DEADLINE_SEC;
   long long quiet_since = now_ms();

   for (;;)
   {
      struct pollfd pfd = {.fd = a.master, .events = POLLIN};
      int pr = poll(&pfd, 1, 200);
      if (time(NULL) > deadline)
         kill(a.pid, SIGKILL); /* next read sees EOF/EIO and we fall through */
      if (pr < 0)
      {
         if (errno == EINTR)
            continue;
         break;
      }

      if (pr > 0)
      {
         char chunk[1024];
         ssize_t n = read(a.master, chunk, sizeof(chunk));
         if (n <= 0)
            break; /* EOF/EIO: gh exited */
         if (rawlen + (size_t)n >= sizeof(raw) - 1)
         {
            /* Keep the tail: the code appears early (already parsed by now)
             * and error text arrives at the end. */
            memmove(raw, raw + sizeof(raw) / 2, rawlen - sizeof(raw) / 2);
            rawlen -= sizeof(raw) / 2;
            scan_6n = scan_6n > sizeof(raw) / 2 ? scan_6n - sizeof(raw) / 2 : 0;
         }
         memcpy(raw + rawlen, chunk, (size_t)n);
         rawlen += (size_t)n;
         quiet_since = now_ms();

         /* Rule 1: answer cursor-position queries as they arrive. */
         const char *q;
         while ((q = memmem(raw + scan_6n, rawlen - scan_6n, "\x1b[6n", 4)) != NULL)
         {
            scan_6n = (size_t)(q - raw) + 4;
            if (write(a.master, "\x1b[1;1R", 6) < 0)
               break;
         }
         /* Back up over a possible partial ESC[6n split across reads. */
         if (scan_6n + 3 > rawlen)
            scan_6n = rawlen > 3 ? rawlen - 3 : 0;

         if (!have_code)
         {
            char text[sizeof(raw)];
            memcpy(text, raw, rawlen);
            text[rawlen] = '\0';
            git_oauth_gh_strip_term_noise(text);
            char code[32];
            if (git_oauth_gh_parse_code(text, code, sizeof(code)))
            {
               have_code = 1;
               pthread_mutex_lock(&g_lock);
               if (g_gen == a.gen)
               {
                  snprintf(g_code, sizeof(g_code), "%s", code);
                  pthread_cond_broadcast(&g_cond);
               }
               pthread_mutex_unlock(&g_lock);
            }
         }
         continue;
      }

      /* Rule 2: output lull — answer at most one pending prompt. */
      if (now_ms() - quiet_since < 400 || (answered_git && answered_enter))
         continue;
      char text[sizeof(raw)];
      memcpy(text, raw, rawlen);
      text[rawlen] = '\0';
      git_oauth_gh_strip_term_noise(text);
      if (!answered_git && strstr(text, "Authenticate Git"))
      {
         answered_git = 1;
         quiet_since = now_ms();
         if (write(a.master, "\r", 1) < 0)
            break;
      }
      else if (!answered_enter && strstr(text, "Press Enter"))
      {
         answered_enter = 1;
         quiet_since = now_ms();
         if (write(a.master, "\r", 1) < 0)
            break;
      }
   }
   close(a.master);

   int st = 0;
   int ok = waitpid(a.pid, &st, 0) == a.pid && WIFEXITED(st) && WEXITSTATUS(st) == 0;

   char errmsg[200] = "sign-in failed";
   int stored = 0;
   if (ok)
   {
      /* gh exited 0 → the token is in the ephemeral GH_CONFIG_DIR. Read it out
       * and move it into the vault-backed per-host store. */
      char envbuf[GH_ENV_SLOTS][GH_ENV_LEN];
      char *envp[GH_ENV_SLOTS + 1];
      build_env(a.dir, envbuf, envp);
      const char *argv[] = {"gh", "auth", "token", "--hostname", "github.com", NULL};
      char *out = NULL;
      int rc = safe_exec_capture_env(argv, envp, &out, 4096);
      if (rc == 0 && out)
      {
         char *tok = out;
         while (*tok && isspace((unsigned char)*tok))
            tok++;
         char *end = tok;
         while (*end && !isspace((unsigned char)*end))
            end++;
         *end = '\0';
         if (tok[0])
            stored = git_host_cred_set("github.com", tok) == 0;
         if (!stored)
            snprintf(errmsg, sizeof(errmsg),
                     tok[0] ? "could not store the GitHub token" : "gh returned no token");
      }
      else
         snprintf(errmsg, sizeof(errmsg), "could not read the token back from gh");
      if (out)
      {
         memset(out, 0, strlen(out)); /* wipe the token copy */
         free(out);
      }
   }
   else
   {
      char text[sizeof(raw)];
      memcpy(text, raw, rawlen);
      text[rawlen] = '\0';
      git_oauth_gh_strip_term_noise(text);
      last_line(text, errmsg, sizeof(errmsg));
      if (!errmsg[0])
         snprintf(errmsg, sizeof(errmsg), "sign-in failed");
   }
   remove_dir(a.dir);

   pthread_mutex_lock(&g_lock);
   if (g_gen == a.gen)
   {
      g_state = stored ? GH_DONE : GH_ERROR;
      if (!stored)
         snprintf(g_err, sizeof(g_err), "%s", errmsg);
      g_pid = -1;
      pthread_cond_broadcast(&g_cond);
   }
   pthread_mutex_unlock(&g_lock);
   return NULL;
}

int git_oauth_gh_pending(void)
{
   pthread_mutex_lock(&g_lock);
   int p = g_state != GH_IDLE;
   pthread_mutex_unlock(&g_lock);
   return p;
}

int git_oauth_gh_start(const char *principal, char *user_code, size_t uc_len, char *verify_uri,
                       size_t vu_len, int *interval, char *err, size_t errlen)
{
   if (user_code && uc_len)
      user_code[0] = '\0';
   if (verify_uri && vu_len)
      verify_uri[0] = '\0';
   if (!gh_on_path())
   {
      snprintf(err, errlen, "GitHub sign-in is not configured");
      return -1;
   }

   pthread_mutex_lock(&g_lock);
   /* A finished-but-unconsumed result (user navigated away) is stale. */
   if (g_state == GH_DONE || g_state == GH_ERROR)
      g_state = GH_IDLE;
   if (g_state == GH_RUNNING && g_code[0])
   {
      /* Same still-pending sign-in: re-issue the code (valid until the device
       * flow expires). */
      snprintf(user_code, uc_len, "%s", g_code);
      snprintf(verify_uri, vu_len, "%s", GH_VERIFY_URI);
      if (interval)
         *interval = 5;
      pthread_mutex_unlock(&g_lock);
      return 0;
   }
   if (g_state == GH_RUNNING)
   {
      /* Pending but codeless (spawn raced/hung) — abandon it and start fresh. */
      if (g_pid > 0)
         kill(g_pid, SIGKILL);
      g_gen++; /* orphan the old reader; it cleans its own dir */
      g_state = GH_IDLE;
      g_pid = -1;
   }

   char dir[128] = "/tmp/aimee-gh-XXXXXX";
   if (!mkdtemp(dir))
   {
      pthread_mutex_unlock(&g_lock);
      snprintf(err, errlen, "could not create the gh session dir");
      return -1;
   }
   char envbuf[GH_ENV_SLOTS][GH_ENV_LEN];
   char *envp[GH_ENV_SLOTS + 1];
   build_env(dir, envbuf, envp);

   int master = -1;
   pid_t pid = forkpty(&master, NULL, NULL, NULL);
   if (pid < 0)
   {
      pthread_mutex_unlock(&g_lock);
      remove_dir(dir);
      snprintf(err, errlen, "could not start gh");
      return -1;
   }
   if (pid == 0)
   {
      /* --insecure-storage: no keyring in the container; the config dir is
       * ephemeral and wiped once the token is moved into the vault. */
      const char *argv[] = {"gh",    "auth",           "login", "--hostname",
                            "github.com", "--git-protocol", "https", "--web",
                            "--insecure-storage", NULL};
      execvpe("gh", (char *const *)argv, envp);
      _exit(127);
   }

   g_state = GH_RUNNING;
   g_gen++;
   g_pid = pid;
   g_code[0] = '\0';
   g_err[0] = '\0';
   snprintf(g_principal, sizeof(g_principal), "%s", principal ? principal : "");

   struct gh_thread_arg *ta = malloc(sizeof(*ta));
   pthread_t th;
   if (!ta)
      goto spawn_fail;
   ta->gen = g_gen;
   ta->pid = pid;
   ta->master = master;
   snprintf(ta->dir, sizeof(ta->dir), "%s", dir);
   if (pthread_create(&th, NULL, gh_reader, ta) != 0)
   {
      free(ta);
      goto spawn_fail;
   }
   pthread_detach(th);

   /* Wait for the reader to surface the one-time code (or fail). */
   struct timespec ts;
   clock_gettime(CLOCK_REALTIME, &ts);
   ts.tv_sec += GH_CODE_WAIT_SEC;
   unsigned mygen = g_gen;
   int wrc = 0;
   while (g_gen == mygen && g_state == GH_RUNNING && !g_code[0] && wrc != ETIMEDOUT)
      wrc = pthread_cond_timedwait(&g_cond, &g_lock, &ts);
   if (g_gen == mygen && g_code[0])
   {
      snprintf(user_code, uc_len, "%s", g_code);
      snprintf(verify_uri, vu_len, "%s", GH_VERIFY_URI);
      if (interval)
         *interval = 5;
      pthread_mutex_unlock(&g_lock);
      return 0;
   }
   snprintf(err, errlen, "%s",
            (g_gen == mygen && g_err[0]) ? g_err : "gh did not produce a sign-in code");
   if (g_gen == mygen)
   {
      if (g_state == GH_RUNNING && g_pid > 0)
         kill(g_pid, SIGKILL);
      g_gen++;
      g_state = GH_IDLE;
      g_pid = -1;
   }
   pthread_mutex_unlock(&g_lock);
   return -1;

spawn_fail:
   kill(pid, SIGKILL);
   close(master);
   waitpid(pid, NULL, 0);
   g_state = GH_IDLE;
   g_pid = -1;
   pthread_mutex_unlock(&g_lock);
   remove_dir(dir);
   snprintf(err, errlen, "could not start gh");
   return -1;
}

int git_oauth_gh_poll(const char *principal, char *err, size_t errlen)
{
   pthread_mutex_lock(&g_lock);
   int rc;
   if (g_state == GH_IDLE ||
       (principal && g_principal[0] && strcmp(g_principal, principal) != 0))
   {
      snprintf(err, errlen, "no pending GitHub sign-in");
      rc = -1;
   }
   else if (g_state == GH_RUNNING)
      rc = 0;
   else if (g_state == GH_DONE)
   {
      g_state = GH_IDLE;
      rc = 1;
   }
   else /* GH_ERROR */
   {
      snprintf(err, errlen, "%s", g_err[0] ? g_err : "sign-in failed");
      g_state = GH_IDLE;
      rc = -1;
   }
   pthread_mutex_unlock(&g_lock);
   return rc;
}
