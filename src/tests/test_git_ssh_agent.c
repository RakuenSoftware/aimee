/* test_git_ssh_agent.c — WP-C2: a webuser's vaulted SSH key is loaded into a
 * per-user in-memory ssh-agent via a memfd, never touching disk; git/ssh reach
 * it via SSH_AUTH_SOCK. Skips gracefully if openssh tooling is unavailable. */
#include "git_ssh_agent.h"
#include "git_forge_vault.h"
#include "vault_kek_cache.h"
#include "vault_service.h"

#include <assert.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int have(const char *tool)
{
   char cmd[128];
   snprintf(cmd, sizeof(cmd), "command -v %s >/dev/null 2>&1", tool);
   return system(cmd) == 0;
}

/* 1 iff any regular file under dir contains `needle`. */
static int dir_has_plaintext(const char *dir, const char *needle)
{
   DIR *d = opendir(dir);
   if (!d)
      return 0;
   struct dirent *e;
   int hit = 0;
   while (!hit && (e = readdir(d)) != NULL)
   {
      if (e->d_name[0] == '.')
         continue;
      char p[1024];
      snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
      struct stat st;
      if (stat(p, &st) != 0 || !S_ISREG(st.st_mode))
         continue;
      FILE *f = fopen(p, "rb");
      if (!f)
         continue;
      char buf[65536];
      size_t n = fread(buf, 1, sizeof(buf) - 1, f);
      buf[n] = '\0';
      fclose(f);
      if (strstr(buf, needle))
         hit = 1;
   }
   closedir(d);
   return hit;
}

int main(void)
{
   if (!have("ssh-agent") || !have("ssh-add") || !have("ssh-keygen"))
   {
      printf("git_ssh_agent: SKIP (openssh tooling unavailable)\n");
      return 0;
   }

   char home[256];
   snprintf(home, sizeof(home), "/tmp/aimee-sshagent-%d", (int)getpid());
   char mk[320];
   snprintf(mk, sizeof(mk), "rm -rf %s && mkdir -p %s", home, home);
   assert(system(mk) == 0);
   setenv("AIMEE_HOME", home, 1);
   char ws[300], rt[300];
   snprintf(ws, sizeof(ws), "%s/ws", home);
   setenv("AIMEE_WORKSPACES_DIR", ws, 1);
   /* sockets must live on tmpfs (WP-L); /dev/shm is tmpfs. */
   snprintf(rt, sizeof(rt), "/dev/shm/aimee-sshagent-%d", (int)getpid());
   setenv("AIMEE_RUNTIME_DIR", rt, 1);
   vault_kek_cache_clear();

   /* Generate an UNENCRYPTED ed25519 key + read it. */
   char keyfile[400];
   snprintf(keyfile, sizeof(keyfile), "%s/id_ed25519", home);
   char kg[600];
   snprintf(kg, sizeof(kg), "ssh-keygen -q -t ed25519 -N '' -C aimee-test -f %s", keyfile);
   assert(system(kg) == 0);
   FILE *kf = fopen(keyfile, "rb");
   assert(kf);
   char key[16384];
   size_t klen = fread(key, 1, sizeof(key) - 1, kf);
   key[klen] = '\0';
   fclose(kf);
   /* remove the on-disk keygen artifacts so only the vault holds the key */
   char rmk[700];
   snprintf(rmk, sizeof(rmk), "rm -f %s %s.pub", keyfile, keyfile);
   assert(system(rmk) == 0);

   const long T0 = 100000;
   const char *alice = "webuser:alice";
   const uint8_t pw[] = "alice-pw";
   assert(vault_service_unlock_password(alice, ATTEST_WEBCHAT_TRUSTED, pw, sizeof(pw) - 1, T0) ==
          VAULT_OK);
   assert(vault_service_set(alice, GIT_FORGE_VAULT_AGENT, GIT_FORGE_SSHKEY_CRED, key, T0) ==
          VAULT_OK);

   /* Ensure the agent: loads the key from a memfd, returns SSH_AUTH_SOCK. */
   char sock[600];
   int r = git_ssh_agent_ensure(alice, sock, sizeof(sock));
   assert(r == 1);
   assert(sock[0] && strstr(sock, "/ssh-agent.sock"));

   /* The key is loaded: ssh-add -l against the socket exits 0 (an identity). */
   setenv("SSH_AUTH_SOCK", sock, 1);
   assert(system("ssh-add -l >/dev/null 2>&1") == 0);

   /* NO file in the runtime dir holds the private key plaintext (only a socket +
    * pidfile; the key crossed via a memfd). */
   char agentdir[400];
   snprintf(agentdir, sizeof(agentdir), "%s/webusers/alice", rt);
   assert(dir_has_plaintext(agentdir, "PRIVATE KEY") == 0);

   /* ensure() again reuses the live agent. */
   char sock2[600];
   assert(git_ssh_agent_ensure(alice, sock2, sizeof(sock2)) == 1);
   assert(strcmp(sock, sock2) == 0);

   /* a principal with no vaulted key -> 0 (no agent). */
   char nb[600];
   assert(git_ssh_agent_ensure("webuser:bob", nb, sizeof(nb)) == 0);

   /* stop tears the agent down. */
   git_ssh_agent_stop(alice);
   unsetenv("SSH_AUTH_SOCK");
   setenv("SSH_AUTH_SOCK", sock, 1);
   assert(system("ssh-add -l >/dev/null 2>&1") != 0); /* socket gone -> can't connect */

   git_ssh_agent_stop("webuser:bob");
   char clean[700];
   snprintf(clean, sizeof(clean), "rm -rf %s %s", home, rt);
   assert(system(clean) == 0);
   memset(key, 0, sizeof(key));
   printf("git_ssh_agent: all tests passed\n");
   return 0;
}
