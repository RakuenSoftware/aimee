/* test_git_cred_inject.c — WP-C: a webchat user's git op gets their vaulted
 * forge token injected into the git child env (GH_TOKEN + GIT_ASKPASS), sourced
 * autonomously from the sealed vault, with any inherited GH_TOKEN dropped. */
#include "git_cred_inject.h"
#include "git_forge_vault.h"
#include "git_ssh_agent.h"
#include "vault_kek_cache.h"
#include "vault_service.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

int main(void)
{
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
   assert(git_cred_inject_build_env(alice, parent) == NULL);

   /* Store alice's PAT in her sealed vault (the WP-B intake path). */
   const uint8_t apw[] = "alice-pw";
   assert(vault_service_unlock_password(alice, ATTEST_WEBCHAT_TRUSTED, apw, sizeof(apw) - 1, T0) ==
          VAULT_OK);
   assert(vault_service_set(alice, GIT_FORGE_VAULT_AGENT, GIT_FORGE_TOKEN_CRED, "ghp_aliceSECRET",
                            T0) == VAULT_OK);

   /* Simulate a background git op: clear the KEK cache so only the server wrap
    * can read it — the injected env must STILL carry alice's token. */
   vault_kek_cache_clear();
   char **env = git_cred_inject_build_env(alice, parent);
   assert(env != NULL);

   int n = 0;
   const char *tok = env_val(env, "GH_TOKEN", &n);
   assert(n == 1); /* exactly one GH_TOKEN — the inherited one was dropped */
   assert(tok && strcmp(tok, "ghp_aliceSECRET") == 0);

   const char *askpass = env_val(env, "GIT_ASKPASS", &n);
   assert(n == 1 && askpass && askpass[0] == '/'); /* our shim, not the inherited */
   assert(strcmp(askpass, "/inherited/should/be/dropped") != 0);

   const char *prompt = env_val(env, "GIT_TERMINAL_PROMPT", &n);
   assert(n == 1 && prompt && strcmp(prompt, "0") == 0);

   /* PATH/HOME carried through. */
   assert(env_val(env, "PATH", &n) && n == 1);
   assert(env_val(env, "HOME", &n) && n == 1);

   git_cred_inject_free_env(env);

   /* Cross-principal: bob has no token -> NULL, never alice's. */
   assert(git_cred_inject_build_env(bob, parent) == NULL);

   /* Empty / NULL principal -> NULL (no leak). */
   assert(git_cred_inject_build_env("", parent) == NULL);

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
      assert(vault_service_set(alice, GIT_FORGE_VAULT_AGENT, GIT_FORGE_SSHKEY_CRED, keybuf, T0) ==
             VAULT_OK);
      char **se = git_cred_inject_build_env(alice, parent);
      assert(se != NULL);
      const char *sock = env_val(se, "SSH_AUTH_SOCK", &n);
      assert(n == 1 && sock && sock[0] == '/'); /* agent socket present */
      /* the HTTPS token is still injected alongside */
      assert(env_val(se, "GH_TOKEN", &n) && n == 1);
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
