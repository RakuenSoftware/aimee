/* cmd_infra.c: POSIX background index scan on commit (forwards to aimee-kb),
 * and gateway pair management via ~/.config/aimee/gateway-pairs.json. */
#include "aimee.h"
#include "config.h"
#include "commands.h"
#include "kb_client.h"
#include "aimee_home.h"
#include "platform_random.h"
#include <cJSON.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

void platform_infra_background_scan(const char *cwd)
{
   pid_t pid = fork();
   if (pid == 0)
   {
      const char *proj_name = strrchr(cwd, '/');
      proj_name = proj_name ? proj_name + 1 : cwd;
      kb_client_index_scan_result_t res;
      (void)kb_client_index_scan(proj_name, cwd, 0, &res);
      _exit(0);
   }
   if (pid > 0)
      waitpid(pid, NULL, WNOHANG);
}

/* ---- aimee gateway pair ---- */

/* Gateway pairs are stored in ~/.config/aimee/gateway-pairs.json so both
 * the aimee CLI and the aimee-gateway binary can read and write them
 * without needing an IPC channel in Phase 1. */

static void gateway_pairs_path(char *buf, size_t bufsz)
{
   snprintf(buf, bufsz, "%s/gateway-pairs.json", aimee_home());
}

static cJSON *gateway_pairs_load(const char *path)
{
   FILE *f = fopen(path, "rb");
   if (!f && errno == ENOENT)
      return cJSON_CreateArray();
   if (!f)
      return NULL;
   fseek(f, 0, SEEK_END);
   long sz = ftell(f);
   fseek(f, 0, SEEK_SET);
   if (sz <= 0 || sz > 1024 * 1024)
   {
      fclose(f);
      return NULL;
   }
   char *buf = malloc((size_t)sz + 1);
   if (!buf)
   {
      fclose(f);
      return NULL;
   }
   (void)fread(buf, 1, (size_t)sz, f);
   buf[sz] = '\0';
   fclose(f);
   cJSON *root = cJSON_Parse(buf);
   free(buf);
   if (!root || !cJSON_IsArray(root))
   {
      cJSON_Delete(root);
      return NULL;
   }
   return root;
}

static int gateway_pairs_lock(const char *path)
{
   char lock_path[640];
   snprintf(lock_path, sizeof(lock_path), "%s.lock", path);
   int fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
   if (fd < 0 || flock(fd, LOCK_EX) != 0)
   {
      if (fd >= 0)
         close(fd);
      return -1;
   }
   return fd;
}

static int gateway_pairs_save(const char *path, cJSON *arr)
{
   char *s = cJSON_PrintUnformatted(arr);
   if (!s)
      return -1;
   char tmp[640];
   snprintf(tmp, sizeof(tmp), "%s.tmp", path);
   (void)unlink(tmp);
   int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
   if (fd < 0)
   {
      free(s);
      return -1;
   }
   size_t len = strlen(s), off = 0;
   while (off < len)
   {
      ssize_t n = write(fd, s + off, len - off);
      if (n < 0 && errno == EINTR)
         continue;
      if (n <= 0)
         break;
      off += (size_t)n;
   }
   int rc = (off == len && fsync(fd) == 0 && close(fd) == 0 && rename(tmp, path) == 0) ? 0 : -1;
   if (rc == 0)
   {
      char parent[640];
      snprintf(parent, sizeof(parent), "%s", path);
      char *slash = strrchr(parent, '/');
      if (!slash)
         rc = -1;
      else
      {
         *slash = '\0';
         int dfd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
         if (dfd < 0 || fsync(dfd) != 0)
            rc = -1;
         if (dfd >= 0)
            close(dfd);
      }
   }
   if (rc != 0)
   {
      (void)close(fd);
      (void)unlink(tmp);
   }
   free(s);
   return rc;
}

static int gateway_pair_code_exists(cJSON *arr, const char *code)
{
   cJSON *e = NULL;
   cJSON_ArrayForEach(e, arr)
   {
      const char *existing = cJSON_GetStringValue(cJSON_GetObjectItem(e, "code"));
      if (existing && strcmp(existing, code) == 0)
         return 1;
   }
   return 0;
}

static int gateway_pair_random_code(cJSON *arr, char code[8])
{
   for (int attempt = 0; attempt < 128; attempt++)
   {
      uint32_t draw = 0;
      if (platform_random_bytes(&draw, sizeof(draw)) != 0)
         return -1;
      if (draw >= UINT32_C(4294000000))
         continue;
      snprintf(code, 8, "%06u", draw % UINT32_C(1000000));
      if (!gateway_pair_code_exists(arr, code))
         return 0;
   }
   return -1;
}

static void cmd_gateway_pair_list(const char *path)
{
   cJSON *arr = gateway_pairs_load(path);
   if (!arr)
   {
      fprintf(stderr, "aimee gateway: pairing store is unreadable or corrupt\n");
      return;
   }
   int n = cJSON_GetArraySize(arr);
   if (n == 0)
   {
      printf("No pairings.\n");
      cJSON_Delete(arr);
      return;
   }
   printf("%-12s %-24s %-8s %-8s %s\n", "PLATFORM", "USER_ID", "CODE", "STATUS", "EXPIRES");
   for (int i = 0; i < n; i++)
   {
      cJSON *e = cJSON_GetArrayItem(arr, i);
      const char *platform = cJSON_GetStringValue(cJSON_GetObjectItem(e, "platform"));
      const char *user_id = cJSON_GetStringValue(cJSON_GetObjectItem(e, "user_id"));
      const char *code = cJSON_GetStringValue(cJSON_GetObjectItem(e, "code"));
      int approved = cJSON_IsTrue(cJSON_GetObjectItem(e, "approved"));
      int revoked = cJSON_IsFalse(cJSON_GetObjectItem(e, "approved")) &&
                    cJSON_GetObjectItem(e, "approved") != NULL;
      long expires = (long)cJSON_GetNumberValue(cJSON_GetObjectItem(e, "expires_at"));
      char exp_buf[32];
      time_t t = (time_t)expires;
      struct tm *tm = localtime(&t);
      if (tm)
         strftime(exp_buf, sizeof(exp_buf), "%Y-%m-%d %H:%M", tm);
      else
         snprintf(exp_buf, sizeof(exp_buf), "%ld", expires);
      const char *status = approved ? "approved" : (revoked ? "revoked" : "pending");
      printf("%-12s %-24s %-8s %-8s %s\n", platform ? platform : "?", user_id ? user_id : "?",
             code ? code : "?", status, exp_buf);
   }
   cJSON_Delete(arr);
}

static void cmd_gateway_pair_issue(const char *path, const char *platform, const char *user_id,
                                   int ttl_s)
{
   cJSON *arr = gateway_pairs_load(path);
   if (!arr)
   {
      fprintf(stderr, "aimee gateway: pairing store is unreadable or corrupt\n");
      return;
   }
   /* Remove existing pending entry for this user */
   for (int i = cJSON_GetArraySize(arr) - 1; i >= 0; i--)
   {
      cJSON *e = cJSON_GetArrayItem(arr, i);
      const char *ep = cJSON_GetStringValue(cJSON_GetObjectItem(e, "platform"));
      const char *eu = cJSON_GetStringValue(cJSON_GetObjectItem(e, "user_id"));
      if (ep && eu && strcmp(ep, platform) == 0 && strcmp(eu, user_id) == 0)
         cJSON_DeleteItemFromArray(arr, i);
   }
   char code[8];
   if (gateway_pair_random_code(arr, code) != 0)
   {
      fprintf(stderr, "aimee gateway: secure random code generation failed\n");
      cJSON_Delete(arr);
      return;
   }
   time_t expires = time(NULL) + ttl_s;
   cJSON *entry = cJSON_CreateObject();
   cJSON_AddStringToObject(entry, "platform", platform);
   cJSON_AddStringToObject(entry, "user_id", user_id);
   cJSON_AddStringToObject(entry, "code", code);
   cJSON_AddNumberToObject(entry, "expires_at", (double)expires);
   /* Do not set "approved" yet; pending state */
   cJSON_AddItemToArray(arr, entry);
   if (gateway_pairs_save(path, arr) == 0)
      printf("Code: %s  (expires in %ds; use 'aimee gateway pair approve %s' to authorize)\n", code,
             ttl_s, code);
   else
      fprintf(stderr, "aimee gateway: failed to save pairs to %s\n", path);
   cJSON_Delete(arr);
}

static void cmd_gateway_pair_approve(const char *path, const char *code, const char *platform,
                                     const char *user_id)
{
   cJSON *arr = gateway_pairs_load(path);
   if (!arr)
   {
      fprintf(stderr, "aimee gateway: pairing store is unreadable or corrupt\n");
      return;
   }
   int found = 0;
   for (int i = 0; i < cJSON_GetArraySize(arr); i++)
   {
      cJSON *e = cJSON_GetArrayItem(arr, i);
      const char *ec = cJSON_GetStringValue(cJSON_GetObjectItem(e, "code"));
      const char *ep = cJSON_GetStringValue(cJSON_GetObjectItem(e, "platform"));
      const char *eu = cJSON_GetStringValue(cJSON_GetObjectItem(e, "user_id"));
      if (ec && ep && eu && strcmp(ec, code) == 0 && strcmp(ep, platform) == 0 &&
          strcmp(eu, user_id) == 0)
      {
         double exp = cJSON_GetNumberValue(cJSON_GetObjectItem(e, "expires_at"));
         if ((time_t)exp < time(NULL))
         {
            fprintf(stderr, "aimee gateway: code expired\n");
            cJSON_Delete(arr);
            return;
         }
         cJSON_DeleteItemFromObject(e, "approved");
         cJSON_AddBoolToObject(e, "approved", 1);
         found = 1;
         break;
      }
   }
   if (found)
   {
      if (gateway_pairs_save(path, arr) == 0)
         printf("approved %s/%s\n", platform, user_id);
      else
         fprintf(stderr, "aimee gateway: save failed\n");
   }
   else
      fprintf(stderr, "aimee gateway: code not found\n");
   cJSON_Delete(arr);
}

static void cmd_gateway_pair_revoke(const char *path, const char *platform, const char *user_id)
{
   cJSON *arr = gateway_pairs_load(path);
   if (!arr)
   {
      fprintf(stderr, "aimee gateway: pairing store is unreadable or corrupt\n");
      return;
   }
   int found = 0;
   for (int i = 0; i < cJSON_GetArraySize(arr); i++)
   {
      cJSON *e = cJSON_GetArrayItem(arr, i);
      const char *ep = cJSON_GetStringValue(cJSON_GetObjectItem(e, "platform"));
      const char *eu = cJSON_GetStringValue(cJSON_GetObjectItem(e, "user_id"));
      if (ep && eu && strcmp(ep, platform) == 0 && strcmp(eu, user_id) == 0)
      {
         cJSON_DeleteItemFromObject(e, "approved");
         cJSON_AddBoolToObject(e, "approved", 0);
         found = 1;
      }
   }
   if (found)
   {
      if (gateway_pairs_save(path, arr) == 0)
         printf("revoked\n");
      else
         fprintf(stderr, "aimee gateway: save failed\n");
   }
   else
      fprintf(stderr, "aimee gateway: pairing not found for %s/%s\n", platform, user_id);
   cJSON_Delete(arr);
}

void cmd_gateway(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   char path[512];
   gateway_pairs_path(path, sizeof(path));

   if (argc < 1)
   {
      printf("Usage: aimee gateway <subcommand>\n");
      printf("  pair list                         — list all pairings\n");
      printf("  pair issue <platform> <user_id>   — generate a pairing code\n");
      printf("  pair approve <code> <platform> <user_id> — confirm and approve identity\n");
      printf("  pair revoke <platform> <user_id>  — revoke a pairing\n");
      return;
   }

   if (strcmp(argv[0], "pair") == 0)
   {
      if (argc < 2)
      {
         fprintf(stderr, "aimee gateway pair: subcommand required (list|issue|approve|revoke)\n");
         return;
      }
      int lock_fd = gateway_pairs_lock(path);
      if (lock_fd < 0)
      {
         fprintf(stderr, "aimee gateway pair: could not lock pairing store\n");
         return;
      }
      if (strcmp(argv[1], "list") == 0)
         cmd_gateway_pair_list(path);
      else if (strcmp(argv[1], "issue") == 0)
      {
         if (argc < 4)
         {
            fprintf(stderr, "aimee gateway pair issue: <platform> <user_id> required\n");
            close(lock_fd);
            return;
         }
         cmd_gateway_pair_issue(path, argv[2], argv[3], 300);
      }
      else if (strcmp(argv[1], "approve") == 0)
      {
         if (argc < 5)
         {
            fprintf(stderr, "aimee gateway pair approve: <code> <platform> <user_id> required\n");
            close(lock_fd);
            return;
         }
         cmd_gateway_pair_approve(path, argv[2], argv[3], argv[4]);
      }
      else if (strcmp(argv[1], "revoke") == 0)
      {
         if (argc < 4)
         {
            fprintf(stderr, "aimee gateway pair revoke: <platform> <user_id> required\n");
            close(lock_fd);
            return;
         }
         cmd_gateway_pair_revoke(path, argv[2], argv[3]);
      }
      else
         fprintf(stderr, "aimee gateway pair: unknown subcommand '%s'\n", argv[1]);
      close(lock_fd);
      return;
   }
   fprintf(stderr, "aimee gateway: unknown subcommand '%s'\n", argv[0]);
}
