#define _GNU_SOURCE 1
/* git_ssh_agent.c — per-webuser in-memory ssh-agent. See git_ssh_agent.h. */
#include "git_ssh_agent.h"
#include "git_forge_vault.h" /* git_forge_vault_sshkey */
#include "webuser_runtime.h" /* webuser_runtime_dir */

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define SSHKEY_MAX 16384

/* Fork+exec argv with env; the child first bars core dumps (RLIMIT_CORE=0 +
 * PR_SET_DUMPABLE=0) so the key can never reach a core file. If `keyfd` >= 0 it
 * is dup'd onto fd 3 (cloexec cleared) so ssh-add can read /proc/self/fd/3. If
 * `cap_out` != NULL, the child's stdout is captured. Returns the exit code, or
 * -1 on fork/pipe failure. */
static int run_child(const char *const argv[], char *const envp[], int keyfd, char **cap_out)
{
   int pipefd[2] = {-1, -1};
   if (cap_out && pipe(pipefd) != 0)
      return -1;
   pid_t pid = fork();
   if (pid < 0)
   {
      if (pipefd[0] >= 0)
      {
         close(pipefd[0]);
         close(pipefd[1]);
      }
      return -1;
   }
   if (pid == 0)
   {
      struct rlimit z = {0, 0};
      setrlimit(RLIMIT_CORE, &z);
      prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
      if (keyfd >= 0)
      {
         if (keyfd != 3)
            dup2(keyfd, 3);
         fcntl(3, F_SETFD, 0); /* clear cloexec so it survives exec */
      }
      if (pipefd[1] >= 0)
      {
         dup2(pipefd[1], STDOUT_FILENO);
         close(pipefd[0]);
         close(pipefd[1]);
      }
      execvpe(argv[0], (char *const *)argv, envp);
      _exit(127);
   }
   if (pipefd[1] >= 0)
      close(pipefd[1]);
   if (cap_out)
   {
      char buf[4096];
      size_t cap = 8192, len = 0;
      char *acc = malloc(cap);
      if (acc)
      {
         ssize_t r;
         while ((r = read(pipefd[0], buf, sizeof(buf))) > 0)
         {
            if (len + (size_t)r + 1 > cap)
            {
               cap *= 2;
               char *n = realloc(acc, cap);
               if (!n)
                  break;
               acc = n;
            }
            memcpy(acc + len, buf, (size_t)r);
            len += (size_t)r;
         }
         acc[len] = '\0';
      }
      *cap_out = acc;
   }
   if (pipefd[0] >= 0)
      close(pipefd[0]);
   int st = 0;
   waitpid(pid, &st, 0);
   return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* Parse "SSH_AGENT_PID=12345;" out of ssh-agent's stdout. Returns the pid or 0. */
static long parse_agent_pid(const char *s)
{
   const char *p = s ? strstr(s, "SSH_AGENT_PID=") : NULL;
   return p ? strtol(p + strlen("SSH_AGENT_PID="), NULL, 10) : 0;
}

/* Build the socket + pidfile paths under the (server-controlled, bounded) tmpfs
 * runtime dir. Returns 0, or -1 on truncation (callers then bail rather than
 * operate on a malformed path). */
static int paths(const char *rt, char *sock, size_t sc, char *pidf, size_t pc)
{
   int a = snprintf(sock, sc, "%s/ssh-agent.sock", rt);
   int b = snprintf(pidf, pc, "%s/ssh-agent.pid", rt);
   return (a > 0 && (size_t)a < sc && b > 0 && (size_t)b < pc) ? 0 : -1;
}

/* Read a pidfile; return the pid if its process is alive, else 0. */
static long live_pid(const char *pidf)
{
   FILE *f = fopen(pidf, "r");
   if (!f)
      return 0;
   long pid = 0;
   if (fscanf(f, "%ld", &pid) != 1)
      pid = 0;
   fclose(f);
   if (pid > 0 && kill((pid_t)pid, 0) == 0)
      return pid;
   return 0;
}

int git_ssh_agent_ensure(const char *principal, char *out, size_t cap)
{
   if (out && cap)
      out[0] = '\0';
   char key[SSHKEY_MAX];
   int kr = git_forge_vault_sshkey(principal, key, sizeof(key));
   if (kr != 1)
      return kr == 0 ? 0 : -1; /* no key -> 0; vault error -> -1 */

   char rt[2048];
   if (webuser_runtime_dir(principal, rt, sizeof(rt)) != 0)
   {
      memset(key, 0, sizeof(key));
      return -1; /* tmpfs gate failed -> fail closed, never write the key */
   }
   char sock[2200], pidf[2200];
   if (paths(rt, sock, sizeof(sock), pidf, sizeof(pidf)) != 0)
   {
      memset(key, 0, sizeof(key));
      return -1;
   }

   /* Reuse a live agent (key already loaded). A benign race exists — the agent
    * could exit between this check and use — but the only consequence is the git
    * op failing to connect (retryable), never a credential exposure. */
   if (live_pid(pidf))
   {
      memset(key, 0, sizeof(key));
      if ((size_t)snprintf(out, cap, "%s", sock) >= cap)
         return -1;
      return 1;
   }

   /* Start a fresh agent bound to our tmpfs socket. */
   unlink(sock);
   const char *agent_argv[] = {"ssh-agent", "-a", sock, NULL};
   char *agent_out = NULL;
   int arc = run_child(agent_argv, environ, -1, &agent_out);
   long apid = parse_agent_pid(agent_out);
   free(agent_out);
   if (arc != 0 || apid <= 0)
   {
      memset(key, 0, sizeof(key));
      return -1;
   }
   FILE *pf = fopen(pidf, "w");
   if (pf)
   {
      fprintf(pf, "%ld\n", apid);
      fclose(pf);
      chmod(pidf, 0600);
   }

   /* Load the key from a memfd — no filesystem path ever holds it. MFD_CLOEXEC
    * so the parent's fd can't leak to an unrelated concurrent fork+exec; the
    * ssh-add child still gets it because run_child dup2()'s it onto fd 3, which
    * is created WITHOUT cloexec (and run_child clears it explicitly too). */
   int mfd = memfd_create("aimee-sshkey", MFD_CLOEXEC);
   int ok = 0;
   if (mfd >= 0)
   {
      /* ssh-add refuses a key whose file looks world-accessible; a memfd is
       * mode 0777 by default, so tighten it (the fd is private to this process
       * + the ssh-add child anyway). */
      fchmod(mfd, 0600);
      size_t klen = strlen(key);
      if (write(mfd, key, klen) == (ssize_t)klen)
      {
         lseek(mfd, 0, SEEK_SET);
         char sock_env[2300], askpass[64] = "SSH_ASKPASS=/bin/false";
         snprintf(sock_env, sizeof(sock_env), "SSH_AUTH_SOCK=%s", sock);
         char *env[] = {sock_env, askpass, (char *)"SSH_ASKPASS_REQUIRE=never",
                        (char *)"DISPLAY=", NULL};
         const char *add_argv[] = {"ssh-add", "/proc/self/fd/3", NULL};
         int rc = run_child(add_argv, env, mfd, NULL);
         ok = (rc == 0);
      }
      close(mfd);
   }
   memset(key, 0, sizeof(key));

   if (!ok)
   {
      git_ssh_agent_stop(principal); /* an encrypted key / failure: don't leave a keyless agent */
      return -1;
   }
   if ((size_t)snprintf(out, cap, "%s", sock) >= cap)
      return -1;
   return 1;
}

void git_ssh_agent_stop(const char *principal)
{
   char rt[2048];
   if (webuser_runtime_dir(principal, rt, sizeof(rt)) != 0)
      return;
   char sock[2200], pidf[2200];
   if (paths(rt, sock, sizeof(sock), pidf, sizeof(pidf)) != 0)
      return;
   long pid = live_pid(pidf);
   if (pid > 0)
      kill((pid_t)pid, SIGTERM);
   unlink(pidf);
   unlink(sock);
}
