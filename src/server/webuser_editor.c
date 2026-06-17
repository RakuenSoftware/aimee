#define _GNU_SOURCE 1
/* webuser_editor.c — per-webuser code-server supervisor. See webuser_editor.h. */
#include "webuser_editor.h"

#include "aimee.h"           /* MAX_PATH_LEN */
#include "git_cred_inject.h" /* git_cred_inject_build_env/free_env (vault git env) */
#include "workspace_scope.h" /* ws_scope_user_root, ws_scope_name_valid */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define PRINCIPAL_MAX 256

typedef struct
{
   char principal[PRINCIPAL_MAX];
   pid_t pid;
   int port;
   long last_active; /* time(NULL) of last ensure/touch */
} editor_t;

static editor_t g_editors[WEBUSER_EDITOR_MAX];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* ── code-server discovery (cached) ─────────────────────────────────────────
 * Returns the absolute path to a code-server binary, or NULL if none is found.
 * WITH_VSCODE=1 images install it at /usr/bin/code-server; we also honour an
 * explicit override and a PATH lookup so non-container installs work. */
static const char *editor_binary_path(void)
{
   static char cached[MAX_PATH_LEN];
   static int resolved = 0;
   if (resolved)
      return cached[0] ? cached : NULL;

   const char *cands[3];
   int n = 0;
   const char *ovr = getenv("AIMEE_WEBCHAT_EDITOR_BIN");
   if (ovr && ovr[0])
      cands[n++] = ovr;
   cands[n++] = "/usr/bin/code-server";
   cands[n++] = "/usr/local/bin/code-server";
   for (int i = 0; i < n; i++)
   {
      if (access(cands[i], X_OK) == 0)
      {
         snprintf(cached, sizeof(cached), "%s", cands[i]);
         resolved = 1;
         return cached;
      }
   }
   /* Fall back to a PATH search. */
   const char *path = getenv("PATH");
   if (path)
   {
      char buf[MAX_PATH_LEN];
      const char *p = path;
      while (*p)
      {
         const char *colon = strchr(p, ':');
         size_t len = colon ? (size_t)(colon - p) : strlen(p);
         if (len > 0 && len < sizeof(buf) - 16)
         {
            memcpy(buf, p, len);
            snprintf(buf + len, sizeof(buf) - len, "/code-server");
            if (access(buf, X_OK) == 0)
            {
               snprintf(cached, sizeof(cached), "%s", buf);
               resolved = 1;
               return cached;
            }
         }
         if (!colon)
            break;
         p = colon + 1;
      }
   }
   resolved = 1;
   cached[0] = '\0';
   return NULL;
}

int webuser_editor_available(void)
{
   const char *en = getenv("AIMEE_WEBCHAT_EDITOR");
   if (!en || !en[0] || en[0] == '0')
      return 0;
   return editor_binary_path() != NULL;
}

/* Bind a transient loopback socket to grab a free port, then release it. A small
 * race exists between release and code-server's bind; ensure() retries on the
 * (rare) collision. Returns the port, or -1 on failure. */
static int free_loopback_port(void)
{
   int fd = socket(AF_INET, SOCK_STREAM, 0);
   if (fd < 0)
      return -1;
   struct sockaddr_in a;
   memset(&a, 0, sizeof(a));
   a.sin_family = AF_INET;
   a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   a.sin_port = 0;
   int port = -1;
   if (bind(fd, (struct sockaddr *)&a, sizeof(a)) == 0)
   {
      socklen_t sl = sizeof(a);
      if (getsockname(fd, (struct sockaddr *)&a, &sl) == 0)
         port = ntohs(a.sin_port);
   }
   close(fd);
   return port;
}

/* True once something accepts a loopback connection on `port`. */
static int port_listening(int port)
{
   int fd = socket(AF_INET, SOCK_STREAM, 0);
   if (fd < 0)
      return 0;
   struct sockaddr_in a;
   memset(&a, 0, sizeof(a));
   a.sin_family = AF_INET;
   a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   a.sin_port = htons((uint16_t)port);
   int ok = (connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0);
   close(fd);
   return ok;
}

/* Optional UID drop target. Resolve AIMEE_WEBCHAT_EDITOR_UID -> uid/gid in the
 * PARENT (getpwnam is not async-signal-safe, and aimee-server is multithreaded,
 * so it must never run between fork() and execve()). want_drop is set when a
 * non-root service user is configured and we are root. Returns 0 on success, -1
 * if the env is set but invalid (caller fails closed). */
typedef struct
{
   int want_drop;
   uid_t uid;
   gid_t gid;
} drop_ids_t;

static int resolve_drop_uid(drop_ids_t *out)
{
   out->want_drop = 0;
   const char *u = getenv("AIMEE_WEBCHAT_EDITOR_UID");
   if (!u || !u[0] || geteuid() != 0)
      return 0; /* not configured / not root -> run in place */
   struct passwd *pw = getpwnam(u);
   if (!pw || pw->pw_uid == 0)
      return -1; /* misconfigured: refuse rather than run the editor as root */
   out->want_drop = 1;
   out->uid = pw->pw_uid;
   out->gid = pw->pw_gid;
   return 0;
}

/* Fork+exec code-server bound to 127.0.0.1:<port>, rooted at userroot, hardened.
 * Returns the child pid, or -1. */
static pid_t spawn_code_server(const char *principal, const char *userroot, int port)
{
   const char *bin = editor_binary_path();
   if (!bin)
      return -1;

   /* Resolve the optional UID-drop target in the parent (getpwnam is not
    * async-signal-safe; must not run between fork() and execve()). */
   drop_ids_t drop;
   if (resolve_drop_uid(&drop) != 0)
      return -1;

   char bind_arg[64], data_dir[MAX_PATH_LEN], ext_dir[MAX_PATH_LEN], home_env[MAX_PATH_LEN + 8];
   snprintf(bind_arg, sizeof(bind_arg), "127.0.0.1:%d", port);
   snprintf(data_dir, sizeof(data_dir), "%s/.aimee-editor/data", userroot);
   snprintf(ext_dir, sizeof(ext_dir), "%s/.aimee-editor/ext", userroot);
   snprintf(home_env, sizeof(home_env), "HOME=%s", userroot);

   /* code-server gives the user an integrated terminal, so the child must NOT
    * inherit the server's full environment — that could expose secrets like
    * AIMEE_DB2_URL (DB password), AIMEE_SERVER_TOKEN, or provider keys. Start
    * from a minimal, curated base (PATH/LANG/TERM only) and layer the user's
    * vault-backed git env (GH_TOKEN+GIT_ASKPASS, SSH_AUTH_SOCK) on top, so the
    * editor authenticates to git without ever seeing unrelated server env. */
   const char *srv_path = getenv("PATH");
   const char *srv_lang = getenv("LANG");
   char path_env[MAX_PATH_LEN + 8], lang_env[256];
   snprintf(path_env, sizeof(path_env), "PATH=%s",
            (srv_path && srv_path[0]) ? srv_path : "/usr/bin:/bin");
   snprintf(lang_env, sizeof(lang_env), "LANG=%s",
            (srv_lang && srv_lang[0]) ? srv_lang : "C.UTF-8");
   char *mini[] = {path_env, lang_env, (char *)"TERM=xterm-256color", NULL};

   /* git_cred_inject_build_env copies from the given base (dropping any
    * GH_TOKEN/SSH_AUTH_SOCK) and appends the vaulted git vars; pass the minimal
    * base, not environ. Returns NULL when the user has no vaulted creds yet, in
    * which case the minimal base alone is the editor's env (still git-prompt
    * safe via GIT_TERMINAL_PROMPT default). envp ownership: the built array is
    * freed in the parent after fork. */
   char **base = git_cred_inject_build_env(principal, mini);
   int owns_base = (base != NULL);
   if (!base)
      base = mini;
   int bc = 0;
   while (base[bc])
      bc++;
   char **envp = calloc((size_t)bc + 2, sizeof(char *));
   if (!envp)
   {
      if (owns_base)
         git_cred_inject_free_env(base);
      return -1;
   }
   int eo = 0;
   for (int i = 0; i < bc; i++)
   {
      if (strncmp(base[i], "HOME=", 5) == 0)
         continue; /* override below */
      envp[eo++] = base[i];
   }
   envp[eo++] = home_env;
   envp[eo] = NULL;

   const char *argv[] = {bin,
                         "--bind-addr",
                         bind_arg,
                         "--auth",
                         "none",
                         "--disable-telemetry",
                         "--disable-update-check",
                         "--disable-workspace-trust",
                         "--user-data-dir",
                         data_dir,
                         "--extensions-dir",
                         ext_dir,
                         userroot,
                         NULL};

   pid_t pid = fork();
   if (pid < 0)
   {
      free(envp);
      if (owns_base)
         git_cred_inject_free_env(base);
      return -1;
   }
   if (pid == 0)
   {
      /* Hardened child: own session, no core dumps, no privilege escalation,
       * stdio to /dev/null, optional UID drop, cwd in the user's root. */
      setsid();
      struct rlimit z = {0, 0};
      setrlimit(RLIMIT_CORE, &z);
      prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
      prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
      int devnull = open("/dev/null", O_RDWR);
      if (devnull >= 0)
      {
         dup2(devnull, STDIN_FILENO);
         dup2(devnull, STDOUT_FILENO);
         dup2(devnull, STDERR_FILENO);
         if (devnull > STDERR_FILENO)
            close(devnull);
      }
      if (drop.want_drop)
      {
         /* setgid before setuid; bail (never run as root) if either fails. */
         if (setgid(drop.gid) != 0 || setuid(drop.uid) != 0)
            _exit(127);
      }
      if (chdir(userroot) != 0)
         _exit(127);
      execvpe(bin, (char *const *)argv, envp);
      _exit(127);
   }

   free(envp);
   if (owns_base)
      git_cred_inject_free_env(base); /* zeroes GH_TOKEN; child kept its own copy */
   return pid;
}

/* Reap one editor entry (kill its process group, clear the slot). Caller holds
 * g_lock. */
static void reap_locked(editor_t *e)
{
   pid_t pid = e->pid;
   if (pid > 0)
   {
      kill(-pid, SIGTERM); /* whole session (setsid) */
      kill(pid, SIGTERM);
      /* Wait for the child here so it is actually reaped — once we zero the slot
       * below, sweep_dead_locked can no longer find it, so an un-waited child
       * would leak as a permanent zombie. code-server exits on SIGTERM quickly;
       * escalate to SIGKILL if it ignores us. This is a rare path
       * (stop/idle/shutdown), so the bounded wait under the lock is acceptable. */
      int reaped = 0;
      for (int i = 0; i < 50; i++) /* up to ~500ms */
      {
         if (waitpid(pid, NULL, WNOHANG) == pid)
         {
            reaped = 1;
            break;
         }
         struct timespec ts = {0, 10 * 1000 * 1000}; /* 10ms */
         nanosleep(&ts, NULL);
      }
      if (!reaped)
      {
         kill(-pid, SIGKILL);
         kill(pid, SIGKILL);
         waitpid(pid, NULL, 0); /* SIGKILL is uncatchable → bounded */
      }
   }
   e->principal[0] = '\0';
   e->pid = 0;
   e->port = 0;
   e->last_active = 0;
}

/* Drop slots whose child has already exited. Caller holds g_lock. */
static void sweep_dead_locked(void)
{
   for (int i = 0; i < WEBUSER_EDITOR_MAX; i++)
   {
      if (g_editors[i].pid > 0 && waitpid(g_editors[i].pid, NULL, WNOHANG) == g_editors[i].pid)
      {
         g_editors[i].principal[0] = '\0';
         g_editors[i].pid = 0;
         g_editors[i].port = 0;
         g_editors[i].last_active = 0;
      }
   }
}

static editor_t *find_locked(const char *principal)
{
   for (int i = 0; i < WEBUSER_EDITOR_MAX; i++)
      if (g_editors[i].pid > 0 && strcmp(g_editors[i].principal, principal) == 0)
         return &g_editors[i];
   return NULL;
}

static editor_t *free_slot_locked(void)
{
   for (int i = 0; i < WEBUSER_EDITOR_MAX; i++)
      if (g_editors[i].pid == 0)
         return &g_editors[i];
   return NULL;
}

int webuser_editor_ensure(const char *principal, int *out_port, char *err, size_t errlen)
{
   if (err && errlen)
      err[0] = '\0';
   if (!principal || strncmp(principal, "webuser:", 8) != 0 || !out_port)
      return -1;
   if (!webuser_editor_available())
      return 0; /* feature off / code-server absent → fail closed */
   if (strlen(principal) >= PRINCIPAL_MAX)
      return -1;

   char userroot[MAX_PATH_LEN];
   if (ws_scope_user_root(principal, 1, userroot, sizeof(userroot)) != 0)
   {
      if (err)
         snprintf(err, errlen, "workspace unavailable");
      return -1;
   }

   pthread_mutex_lock(&g_lock);
   sweep_dead_locked();

   editor_t *e = find_locked(principal);
   if (e && port_listening(e->port))
   {
      e->last_active = (long)time(NULL);
      *out_port = e->port;
      pthread_mutex_unlock(&g_lock);
      return 1;
   }
   if (e) /* stale (process gone or not listening) — reap and respawn */
      reap_locked(e);

   editor_t *slot = free_slot_locked();
   if (!slot)
   {
      pthread_mutex_unlock(&g_lock);
      if (err)
         snprintf(err, errlen, "too many active editors");
      return -1;
   }

   int port = -1;
   pid_t pid = -1;
   for (int attempt = 0; attempt < 3 && pid < 0; attempt++)
   {
      port = free_loopback_port();
      if (port < 0)
         continue;
      pid = spawn_code_server(principal, userroot, port);
   }
   if (pid < 0)
   {
      pthread_mutex_unlock(&g_lock);
      if (err)
         snprintf(err, errlen, "failed to start editor");
      return -1;
   }

   snprintf(slot->principal, sizeof(slot->principal), "%s", principal);
   slot->pid = pid;
   slot->port = port;
   slot->last_active = (long)time(NULL);
   pthread_mutex_unlock(&g_lock);

   /* Wait (outside the lock) for code-server to come up. ~12s budget. */
   int ready = 0;
   for (int i = 0; i < 120; i++)
   {
      struct timespec ts = {0, 100 * 1000 * 1000}; /* 100ms */
      nanosleep(&ts, NULL);
      /* Bail early if the child died. */
      if (waitpid(pid, NULL, WNOHANG) == pid)
         break;
      if (port_listening(port))
      {
         ready = 1;
         break;
      }
   }
   if (!ready)
   {
      pthread_mutex_lock(&g_lock);
      editor_t *cur = find_locked(principal);
      if (cur && cur->pid == pid)
         reap_locked(cur);
      else
      {
         kill(-pid, SIGTERM);
         kill(pid, SIGTERM);
      }
      pthread_mutex_unlock(&g_lock);
      if (err)
         snprintf(err, errlen, "editor did not become ready");
      return -1;
   }

   *out_port = port;
   return 1;
}

void webuser_editor_touch(const char *principal)
{
   if (!principal)
      return;
   pthread_mutex_lock(&g_lock);
   editor_t *e = find_locked(principal);
   if (e)
      e->last_active = (long)time(NULL);
   pthread_mutex_unlock(&g_lock);
}

void webuser_editor_stop(const char *principal)
{
   if (!principal)
      return;
   pthread_mutex_lock(&g_lock);
   editor_t *e = find_locked(principal);
   if (e)
      reap_locked(e);
   pthread_mutex_unlock(&g_lock);
}

int webuser_editor_reap_idle(long idle_secs)
{
   if (idle_secs <= 0)
      return 0;
   long now = (long)time(NULL);
   int reaped = 0;
   pthread_mutex_lock(&g_lock);
   sweep_dead_locked();
   for (int i = 0; i < WEBUSER_EDITOR_MAX; i++)
   {
      if (g_editors[i].pid > 0 && now - g_editors[i].last_active > idle_secs)
      {
         reap_locked(&g_editors[i]);
         reaped++;
      }
   }
   pthread_mutex_unlock(&g_lock);
   return reaped;
}

void webuser_editor_shutdown(void)
{
   pthread_mutex_lock(&g_lock);
   for (int i = 0; i < WEBUSER_EDITOR_MAX; i++)
      if (g_editors[i].pid > 0)
         reap_locked(&g_editors[i]);
   pthread_mutex_unlock(&g_lock);
}
