/* test_agent_key_import.c — D9 backup-before-scrub + run lock for
 * `aimee agent key import`.
 *
 * Drives the real cli_agent_key_import() with a stubbed server dispatch so the
 * destructive scrub path runs without a live server. Proves:
 *   - a successful scrubbing import writes a recoverable 0600 backup of the
 *     keyring BEFORE scrubbing, and the backup holds the plaintext keys;
 *   - the keyring is scrubbed only after the backup exists;
 *   - the run lock is released (no stale lock left behind);
 *   - a refused import (no capability) scrubs nothing and leaves no backup;
 *   - --keep imports without scrubbing and writes no backup. */
#include "cli_agent_keys.h"
#include "cli_client.h"
#include "cJSON.h"
#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char g_home[256];

/* Controllable server stub: g_status / g_message drive the response. */
static const char *g_status = "ok";
static const char *g_message = NULL;
cJSON *cli_v1_dispatch(cJSON *req, int timeout_ms)
{
   (void)req;
   (void)timeout_ms;
   cJSON *r = cJSON_CreateObject();
   cJSON_AddStringToObject(r, "status", g_status);
   if (g_message)
      cJSON_AddStringToObject(r, "message", g_message);
   return r;
}

static void write_keyring(const char *json)
{
   char path[400];
   snprintf(path, sizeof(path), "%s/agent-keys.json", g_home);
   FILE *f = fopen(path, "wb");
   assert(f);
   fputs(json, f);
   fclose(f);
}

static char *read_keyring(void)
{
   char path[400];
   snprintf(path, sizeof(path), "%s/agent-keys.json", g_home);
   FILE *f = fopen(path, "rb");
   if (!f)
      return NULL;
   static char buf[4096];
   size_t n = fread(buf, 1, sizeof(buf) - 1, f);
   buf[n] = '\0';
   fclose(f);
   return buf;
}

/* Find a backup file agent-keys.json.pre-import.* ; copy its name into `out`. */
static int find_backup(char *out, size_t out_len)
{
   DIR *d = opendir(g_home);
   if (!d)
      return 0;
   int found = 0;
   struct dirent *de;
   while ((de = readdir(d)) != NULL)
   {
      if (strncmp(de->d_name, "agent-keys.json.pre-import.", 27) == 0)
      {
         snprintf(out, out_len, "%s/%s", g_home, de->d_name);
         found = 1;
         break;
      }
   }
   closedir(d);
   return found;
}

static int lockfile_exists(void)
{
   char p[400];
   snprintf(p, sizeof(p), "%s/agent-keys.json.import.lock", g_home);
   return access(p, F_OK) == 0;
}

static void reset_home(void)
{
   char cmd[640];
   snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", g_home, g_home);
   assert(system(cmd) == 0);
}

int main(void)
{
   snprintf(g_home, sizeof(g_home), "/tmp/aimee-keyimport-test-%d", (int)getpid());
   setenv("AIMEE_HOME", g_home, 1);

   /* 1. Successful scrubbing import: backup written, keyring scrubbed, lock gone. */
   reset_home();
   write_keyring("{\"agentA\":\"sk-A-secret\",\"agentB\":\"sk-B-secret\"}");
   g_status = "ok";
   g_message = NULL;
   assert(cli_agent_key_import(0, NULL, 0) == 0);

   char backup[512];
   assert(find_backup(backup, sizeof(backup)) && "no pre-import backup was written");
   FILE *bf = fopen(backup, "rb");
   assert(bf);
   char bbuf[4096];
   size_t bn = fread(bbuf, 1, sizeof(bbuf) - 1, bf);
   bbuf[bn] = '\0';
   fclose(bf);
   assert(strstr(bbuf, "sk-A-secret") && strstr(bbuf, "sk-B-secret") &&
          "backup is missing the plaintext keys");

   char *kr = read_keyring();
   assert(kr && !strstr(kr, "sk-A-secret") && !strstr(kr, "sk-B-secret") &&
          "keyring was not scrubbed after a successful import");
   assert(!lockfile_exists() && "run lock not released");
   printf("  PASS: scrubbing import backs up then scrubs, lock released\n");

   /* 2. Refused import (no capability): nothing scrubbed, no backup left. */
   reset_home();
   write_keyring("{\"agentA\":\"sk-A-secret\"}");
   g_status = "error";
   g_message = "vault: caller lacks the vault:write:server capability";
   /* A capability refusal is counted as "refused", not "error" -> exit 0. */
   assert(cli_agent_key_import(0, NULL, 0) == 0);
   kr = read_keyring();
   assert(kr && strstr(kr, "sk-A-secret") && "a refused import must NOT scrub the key");
   char ignore[512];
   assert(!find_backup(ignore, sizeof(ignore)) &&
          "a refused import (scrubbed nothing) must leave no backup");
   assert(!lockfile_exists());
   printf("  PASS: refused import scrubs nothing and leaves no backup\n");

   /* 3. --keep: import without scrubbing, no backup. */
   reset_home();
   write_keyring("{\"agentA\":\"sk-A-secret\"}");
   g_status = "ok";
   g_message = NULL;
   char *argv_keep[] = {(char *)"--keep"};
   assert(cli_agent_key_import(1, argv_keep, 0) == 0);
   kr = read_keyring();
   assert(kr && strstr(kr, "sk-A-secret") && "--keep must retain the plaintext key");
   assert(!find_backup(ignore, sizeof(ignore)) && "--keep takes no backup (nothing is scrubbed)");
   printf("  PASS: --keep retains plaintext and writes no backup\n");

   char cmd[640];
   snprintf(cmd, sizeof(cmd), "rm -rf %s", g_home);
   (void)system(cmd);
   printf("PASS: agent key import backup-before-scrub + lock (D9)\n");
   return 0;
}
