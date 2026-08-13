/* test_git_cred_inject.c — WP-C: a webchat user's git op gets their vaulted
 * forge token injected into the git child env (GH_TOKEN + GIT_ASKPASS), sourced
 * autonomously from the sealed vault, with any inherited GH_TOKEN dropped. */
#include "modules/git/git_cred_inject.h"
#include "modules/git/git_forge_vault.h"
#include "modules/git/git_host_cred.h"
#include "modules/git/git_host_resolve.h"
#include "modules/git/git_ssh_agent.h"
#include "util.h" /* safe_exec_capture_cwd_env_fd_timeout — fd-mode invariant test */
#include "vault_kek_cache.h"
#include "vault_service.h"

#define STR2(x) #x
#define STR(x)  STR2(x)

#include <assert.h>
#include <fcntl.h> /* O_DIRECTORY — the fd-collision regression below */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

/* Return the value of `key=` in envp, or NULL; also count matches into *count. */
static const char *env_val(char *const *envp, const char *key, int *count)
{
   size_t kl = strlen(key);
   const char *found = NULL;
   int n = 0;
   for (int i = 0; envp && envp[i]; i++)
   {
      if (strncmp(envp[i], key, kl) == 0 && envp[i][kl] == '=')
      {
         found = envp[i] + kl + 1;
         n++;
      }
   }
   if (count)
      *count = n;
   return found;
}

/* Read the token back out of the credential memfd, the way the askpass shim does
 * (reopen /proc/self/fd/<n>, so the read always starts at offset 0). This is how
 * a precedence assertion sees WHICH token was chosen now that no path puts one in
 * the environment. */
static int fd_token(int fd, char *out, size_t cap)
{
   out[0] = '\0';
   if (fd < 0)
      return 0;
   char path[64];
   snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
   FILE *f = fopen(path, "r");
   if (!f)
      return 0;
   if (!fgets(out, (int)cap, f))
   {
      fclose(f);
      return 0;
   }
   fclose(f);
   out[strcspn(out, "\n")] = '\0';
   return out[0] != '\0';
}

/* webuser_runtime fails closed until a name validator is registered; the askpass
 * socket lives under its runtime dir. See tests/support/webuser_name_validator.c. */
void webuser_test_install_name_validator(void);

int main(void)
{
   webuser_test_install_name_validator();
   int fdX = -1;
   char tokbuf[4096];
   char home[256];
   snprintf(home, sizeof(home), "/tmp/aimee-gci-test-%d", (int)getpid());
   char mk[320];
   snprintf(mk, sizeof(mk), "rm -rf %s && mkdir -p %s", home, home);
   assert(system(mk) == 0);
   setenv("AIMEE_HOME", home, 1);
   vault_kek_cache_clear();

   const long T0 = 100000;
   const char *alice = "webuser:alice";
   const char *bob = "webuser:bob";

   char *const parent[] = {(char *)"PATH=/usr/bin", (char *)"GH_TOKEN=INHERITED-MUST-BE-DROPPED",
                           (char *)"GIT_ASKPASS=/inherited/should/be/dropped",
                           (char *)"HOME=/whatever", NULL};

   /* No vaulted token yet -> NULL (caller uses ambient creds). */
   assert(git_cred_inject_build_env_for_repo(alice, NULL, NULL, NULL, parent, &fdX) == NULL);

   /* Store alice's PAT in her sealed vault (the WP-B intake path). */
   const uint8_t apw[] = "alice-pw";
   assert(vault_service_unlock_password(alice, ATTEST_WEBCHAT_TRUSTED, apw, sizeof(apw) - 1, T0) ==
          VAULT_OK);
   assert(vault_service_set_server(GIT_FORGE_VAULT_AGENT, GIT_FORGE_TOKEN_CRED,
                                   "ghp_aliceSECRET") == VAULT_OK);

   /* Simulate a background git op: clear the KEK cache so only the server wrap
    * can read it — the injected env must STILL carry alice's token. */
   vault_kek_cache_clear();
   char **env = git_cred_inject_build_env_for_repo(alice, NULL, NULL, NULL, parent, &fdX);
   assert(env != NULL);

   /* FD MODE is the only mode: the env advertises the descriptor, never the
    * secret, so the token is unreadable from /proc/<pid>/environ. */
   int n = 0;
   assert(env_val(env, "GH_TOKEN", &n) == NULL && n == 0);
   const char *fdv = env_val(env, "AIMEE_GIT_TOKEN_FD", &n);
   assert(n == 1 && fdv && atoi(fdv) == GIT_CRED_TOKEN_TARGET_FD);
   assert(fdX >= 0);
   for (int i = 0; env[i]; i++)
      assert(strstr(env[i], "ghp_aliceSECRET") == NULL);

   const char *askpass = env_val(env, "GIT_ASKPASS", &n);
   assert(n == 1 && askpass && askpass[0] == '/'); /* our shim, not the inherited */
   assert(strcmp(askpass, "/inherited/should/be/dropped") != 0);

   const char *prompt = env_val(env, "GIT_TERMINAL_PROMPT", &n);
   assert(n == 1 && prompt && strcmp(prompt, "0") == 0);

   /* PATH/HOME carried through. */
   assert(env_val(env, "PATH", &n) && n == 1);
   assert(env_val(env, "HOME", &n) && n == 1);

   git_cred_inject_free_env(env);

   /* Single-tenant: another actor resolves the same environment credential. */
   char **benv = git_cred_inject_build_env_for_repo(bob, NULL, NULL, NULL, parent, &fdX);
   assert(benv != NULL);
   git_cred_inject_free_env(benv);

   /* An empty principal is not a namespace miss any more: the credential belongs
    * to the environment, so a caller with no actor (the workflow forge and the
    * turn runner both pass NULL) resolves the same one. It is still delivered by
    * fd, so "no actor" never means "token in the environment". */
   int anon_fd = -1;
   char **anon = git_cred_inject_build_env_for_repo("", NULL, NULL, NULL, parent, &anon_fd);
   assert(anon != NULL && anon_fd >= 0);
   for (int i = 0; anon[i]; i++)
      assert(strncmp(anon[i], "GH_TOKEN=", 9) != 0);
   close(anon_fd);
   git_cred_inject_free_env(anon);

   /* --- vault-first precedence for a specific repo (the unified policy) --- */
   /* Wire the per-host vault seam and stash a token for a host that is NOT
    * alice's personal token, so we can tell which source won. */
   git_host_resolve_register(git_host_cred_for_url);
   assert(git_host_cred_set("gitlab.example.com", "glpat-HOSTSECRET") == 0);

   /* 1) A caller-supplied (inline/broker) token beats every vault source. */
   vault_kek_cache_clear();
   int e1_fd = -1;
   char **e1 = git_cred_inject_build_env_for_repo(alice, "https://gitlab.example.com/x/y", NULL,
                                                  "INLINE-WINS", parent, &e1_fd);
   assert(e1 && fd_token(e1_fd, tokbuf, sizeof(tokbuf)) && strcmp(tokbuf, "INLINE-WINS") == 0);
   git_cred_inject_free_env(e1);
   if (e1_fd >= 0)
      close(e1_fd);

   /* 2) The per-host vault token beats alice's own vaulted personal token. */
   vault_kek_cache_clear();
   int e2_fd = -1;
   char **e2 = git_cred_inject_build_env_for_repo(alice, "https://gitlab.example.com/x/y", NULL,
                                                  NULL, parent, &e2_fd);
   assert(e2 && fd_token(e2_fd, tokbuf, sizeof(tokbuf)) && strcmp(tokbuf, "glpat-HOSTSECRET") == 0);
   git_cred_inject_free_env(e2);
   if (e2_fd >= 0)
      close(e2_fd);

   /* 3) A host with no stored token falls back to alice's personal vault token. */
   vault_kek_cache_clear();
   int e3_fd = -1;
   char **e3 = git_cred_inject_build_env_for_repo(alice, "https://no-token-host.example/x/y", NULL,
                                                  NULL, parent, &e3_fd);
   assert(e3 && fd_token(e3_fd, tokbuf, sizeof(tokbuf)) && strcmp(tokbuf, "ghp_aliceSECRET") == 0);
   git_cred_inject_free_env(e3);
   if (e3_fd >= 0)
      close(e3_fd);

   /* 4) Seam dormant (unregistered): the per-host step is skipped entirely, so
    * even a stored host token is not consulted — falls to the personal token. */
   git_host_resolve_register(NULL);
   vault_kek_cache_clear();
   int e4_fd = -1;
   char **e4 = git_cred_inject_build_env_for_repo(alice, "https://gitlab.example.com/x/y", NULL,
                                                  NULL, parent, &e4_fd);
   assert(e4 && fd_token(e4_fd, tokbuf, sizeof(tokbuf)) && strcmp(tokbuf, "ghp_aliceSECRET") == 0);
   git_cred_inject_free_env(e4);
   if (e4_fd >= 0)
      close(e4_fd);
   git_host_resolve_register(git_host_cred_for_url);

   /* --- FD MODE (#3A): the HTTPS token rides an inherited memfd, never the env,
    * so it cannot appear in the git child's /proc/<pid>/environ. --- */
   {
      int tfd = -1;
      char **fe = git_cred_inject_build_env_for_repo(alice, "https://github.com/o/r", NULL,
                                                     "FD-MODE-SECRET", parent, &tfd);
      assert(fe && tfd >= 0);
      /* No GH_TOKEN; the fd number is advertised instead (a number, not a secret). */
      assert(env_val(fe, "GH_TOKEN", &n) == NULL && n == 0);
      const char *fdv = env_val(fe, "AIMEE_GIT_TOKEN_FD", &n);
      assert(n == 1 && fdv && atoi(fdv) == GIT_CRED_TOKEN_TARGET_FD);
      assert(env_val(fe, "GIT_ASKPASS", &n) && n == 1);
      assert(env_val(fe, "GIT_TERMINAL_PROMPT", &n) && n == 1);
      /* The secret must not appear ANYWHERE in the env strings. */
      for (int i = 0; fe[i]; i++)
         assert(strstr(fe[i], "FD-MODE-SECRET") == NULL);

      /* The binding invariant: a child exec'd with this env + the inherited fd can
       * read the token via /proc/self/fd/<target> (what the askpass does), but the
       * token is NOT in its /proc/self/environ. */
      const char *argv[] = {
          "/bin/sh", "-c",
          "cat /proc/self/fd/" STR(
              GIT_CRED_TOKEN_TARGET_FD) "; printf '\\n--ENVIRON--\\n'; tr '\\0' '\\n' < "
                                        "/proc/self/environ",
          NULL};
      char *out = NULL;
      int rc = safe_exec_capture_cwd_env_fd_timeout(argv, NULL, fe, &out, 1 << 16, 10000, tfd,
                                                    GIT_CRED_TOKEN_TARGET_FD);
      assert(rc == 0 && out);
      char *envmark = strstr(out, "--ENVIRON--");
      assert(envmark);
      *envmark = '\0'; /* split the capture into [fd read] | [environ dump] */
      assert(strstr(out, "FD-MODE-SECRET") != NULL);              /* fd carried the token */
      assert(strstr(envmark + 1, "FD-MODE-SECRET") == NULL);      /* NOT in environ */
      assert(strstr(envmark + 1, "AIMEE_GIT_TOKEN_FD=") != NULL); /* only the fd number is */
      free(out);
      close(tfd);
      git_cred_inject_free_env(fe);
   }

   /* A cwd pinned by descriptor must survive the credential fd landing on the
    * SAME descriptor number.
    *
    * git_project_clone pins the clone destination as "/proc/self/fd/<tmpfd>" so
    * git never resolves an attacker-influencable path string, and passes the
    * token memfd at GIT_CRED_TOKEN_TARGET_FD. The child used to dup2 the token
    * onto that number BEFORE chdir'ing, so whenever tmpfd happened to be 21 the
    * dup2 replaced the directory with the memfd, chdir failed on a non-directory,
    * and the child exited 127 -- surfacing as "git clone failed (rc=127)" while
    * git was never even reached. Which fd number the destination gets depends on
    * how many descriptors the server holds, so it struck unpredictably; bulk
    * cloning an org tripped it repeatedly.
    *
    * Force the collision rather than hoping for it: put the directory ON the
    * target number. */
   {
      char tmpl[256];
      snprintf(tmpl, sizeof tmpl, "%s/aimee_fdclash_XXXXXX", platform_tmpdir());
      assert(mkdtemp(tmpl));
      int dirfd = open(tmpl, O_RDONLY | O_DIRECTORY);
      assert(dirfd >= 0);
      assert(dup2(dirfd, GIT_CRED_TOKEN_TARGET_FD) == GIT_CRED_TOKEN_TARGET_FD);
      close(dirfd);

      int tfd = -1;
      char **fe = git_cred_inject_build_env_for_repo(alice, "https://github.com/o/r", NULL,
                                                     "CLASH-SECRET", parent, &tfd);
      assert(fe && tfd >= 0);
      char cwd[64];
      snprintf(cwd, sizeof(cwd), "/proc/self/fd/%d", GIT_CRED_TOKEN_TARGET_FD);

      /* pwd -P: the child must land in the directory the fd named, and the token
       * must still arrive on the target descriptor. */
      const char *argv[] = {"/bin/sh", "-c",
                            "pwd -P; cat /proc/self/fd/" STR(GIT_CRED_TOKEN_TARGET_FD), NULL};
      char *out = NULL;
      int rc = safe_exec_capture_cwd_env_fd_timeout(argv, cwd, fe, &out, 1 << 16, 10000, tfd,
                                                    GIT_CRED_TOKEN_TARGET_FD);
      assert(rc == 0 && out); /* was 127: chdir ran after the dup2 had clobbered it */
      assert(strstr(out, tmpl) != NULL);
      assert(strstr(out, "CLASH-SECRET") != NULL);
      free(out);
      close(tfd);
      git_cred_inject_free_env(fe);
      close(GIT_CRED_TOKEN_TARGET_FD);
      rmdir(tmpl);
   }

   /* SSH integration (WP-C2): a vaulted SSH key adds SSH_AUTH_SOCK to the env
    * (the in-memory agent). Guarded on openssh tooling. */
   if (system("command -v ssh-agent >/dev/null 2>&1 && command -v ssh-add >/dev/null 2>&1 && "
              "command -v ssh-keygen >/dev/null 2>&1") == 0)
   {
      char rt[300];
      snprintf(rt, sizeof(rt), "/dev/shm/aimee-gci-rt-%d", (int)getpid());
      setenv("AIMEE_RUNTIME_DIR", rt, 1);
      char kf[400], kgen[700];
      snprintf(kf, sizeof(kf), "%s/k", home);
      snprintf(kgen, sizeof(kgen), "ssh-keygen -q -t ed25519 -N '' -f %s && cat %s", kf, kf);
      FILE *p = popen(kgen, "r");
      assert(p);
      char keybuf[16384];
      size_t kl = fread(keybuf, 1, sizeof(keybuf) - 1, p);
      keybuf[kl] = '\0';
      pclose(p);
      /* re-unlock (the earlier cache clear locked alice's vault) before storing */
      const uint8_t apw2[] = "alice-pw";
      assert(vault_service_unlock_password(alice, ATTEST_WEBCHAT_TRUSTED, apw2, sizeof(apw2) - 1,
                                           T0) == VAULT_OK);
      assert(vault_service_set_server(GIT_FORGE_VAULT_AGENT, GIT_FORGE_SSHKEY_CRED, keybuf) ==
             VAULT_OK);
      char **se = git_cred_inject_build_env_for_repo(alice, NULL, NULL, NULL, parent, &fdX);
      assert(se != NULL);
      const char *sock = env_val(se, "SSH_AUTH_SOCK", &n);
      assert(n == 1 && sock && sock[0] == '/'); /* agent socket present */
      /* a TOFU SSH command rides alongside so an SSH remote uses the agent key
       * and doesn't stall on a first-time host key; known_hosts is pinned to a
       * per-principal file (beside the agent socket) so accept-new's first-use
       * trust never bleeds through the shared ~/.ssh/known_hosts to other tenants */
      const char *sshcmd = env_val(se, "GIT_SSH_COMMAND", &n);
      assert(n == 1 && sshcmd && strstr(sshcmd, "StrictHostKeyChecking=accept-new"));
      assert(strstr(sshcmd, "UserKnownHostsFile=") && strstr(sshcmd, "/known_hosts"));
      assert(strstr(sshcmd, rt)); /* the pinned path is inside THIS principal's runtime dir */
      /* the HTTPS token is still wired alongside — as a descriptor, never a value */
      assert(env_val(se, "GH_TOKEN", &n) == NULL && n == 0);
      assert(env_val(se, "AIMEE_GIT_TOKEN_FD", &n) && n == 1);
      git_cred_inject_free_env(se);
      git_ssh_agent_stop(alice);
      char rmrt[400];
      snprintf(rmrt, sizeof(rmrt), "rm -rf %s", rt);
      assert(system(rmrt) == 0);
      unsetenv("AIMEE_RUNTIME_DIR");
   }

   char clean[320];
   snprintf(clean, sizeof(clean), "rm -rf %s", home);
   assert(system(clean) == 0);
   printf("git_cred_inject: all tests passed\n");
   return 0;
}
