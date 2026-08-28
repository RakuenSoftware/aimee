/* gateway_pairing.c: file-backed DM pairing for the gateway runtime.
 *
 * Shares ~/.aimee/gateway-pairs.json with the `aimee gateway pair` CLI.
 * Entry schema (one object per array element):
 *   { "platform": "telegram", "user_id": "123", "code": "048213",
 *     "expires_at": <unix>, "approved": true|false (absent = pending) }
 */
#include "gateway_pairing.h"
#include "aimee_home.h"
#include "log.h"
#include "platform_random.h"
#include <cJSON.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

#define PAIRING_TTL_SECONDS 3600
#define PAIRING_FILE_MAX    (1024 * 1024)

static pthread_mutex_t g_pairing_mutex = PTHREAD_MUTEX_INITIALIZER;

static void pairs_path(char *buf, size_t bufsz)
{
   snprintf(buf, bufsz, "%s/gateway-pairs.json", aimee_home());
}

static int pairs_lock(const char *path, int exclusive)
{
   char lock_path[640];
   snprintf(lock_path, sizeof(lock_path), "%s.lock", path);
   int fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
   if (fd < 0 || flock(fd, exclusive ? LOCK_EX : LOCK_SH) != 0)
   {
      if (fd >= 0)
         close(fd);
      return -1;
   }
   return fd;
}

static cJSON *pairs_load(const char *path)
{
   FILE *f = fopen(path, "rb");
   if (!f && errno == ENOENT)
      return cJSON_CreateArray();
   if (!f)
      return NULL;
   fseek(f, 0, SEEK_END);
   long sz = ftell(f);
   fseek(f, 0, SEEK_SET);
   if (sz <= 0 || sz > PAIRING_FILE_MAX)
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
   size_t n = fread(buf, 1, (size_t)sz, f);
   buf[n] = '\0';
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

static int pairs_save(const char *path, cJSON *arr)
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
   int rc = (off == len && fsync(fd) == 0) ? 0 : -1;
   if (close(fd) != 0)
      rc = -1;
   fd = -1;
   if (rc == 0 && rename(tmp, path) != 0)
      rc = -1;
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
      if (fd >= 0)
         (void)close(fd);
      (void)unlink(tmp);
   }
   free(s);
   return rc;
}

static int code_exists(cJSON *arr, const char *code)
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

static int pairing_random_code(cJSON *arr, char code[8])
{
   /* 4,294,000,000 is the largest multiple of 1,000,000 below 2^32;
    * rejection avoids modulo bias. Retry also enforces pending-code uniqueness. */
   for (int attempt = 0; attempt < 128; attempt++)
   {
      uint32_t draw = 0;
      if (platform_random_bytes(&draw, sizeof(draw)) != 0)
         return -1;
      if (draw >= UINT32_C(4294000000))
         continue;
      snprintf(code, 8, "%06u", draw % UINT32_C(1000000));
      if (!code_exists(arr, code))
         return 0;
   }
   return -1;
}

static cJSON *find_entry(cJSON *arr, const char *platform, const char *user_id)
{
   int n = cJSON_GetArraySize(arr);
   for (int i = 0; i < n; i++)
   {
      cJSON *e = cJSON_GetArrayItem(arr, i);
      const char *ep = cJSON_GetStringValue(cJSON_GetObjectItem(e, "platform"));
      const char *eu = cJSON_GetStringValue(cJSON_GetObjectItem(e, "user_id"));
      if (ep && eu && strcmp(ep, platform) == 0 && strcmp(eu, user_id) == 0)
         return e;
   }
   return NULL;
}

int gateway_pairing_is_approved(const char *platform, const char *user_id)
{
   if (!platform || !user_id || !platform[0] || !user_id[0])
      return 0;
   char path[512];
   pairs_path(path, sizeof(path));

   pthread_mutex_lock(&g_pairing_mutex);
   int lock_fd = pairs_lock(path, 0);
   if (lock_fd < 0)
   {
      pthread_mutex_unlock(&g_pairing_mutex);
      return 0;
   }
   cJSON *arr = pairs_load(path);
   if (!arr)
   {
      close(lock_fd);
      pthread_mutex_unlock(&g_pairing_mutex);
      return 0;
   }
   cJSON *e = find_entry(arr, platform, user_id);
   int approved = 0;
   if (e)
   {
      cJSON *japproved = cJSON_GetObjectItem(e, "approved");
      double exp = cJSON_GetNumberValue(cJSON_GetObjectItem(e, "expires_at"));
      /* Approved pairings do not expire; the expiry only bounds the pending
       * code window. Treat approved==true as authorized regardless of expiry. */
      if (cJSON_IsTrue(japproved))
         approved = 1;
      else
         (void)exp;
   }
   cJSON_Delete(arr);
   close(lock_fd);
   pthread_mutex_unlock(&g_pairing_mutex);
   return approved;
}

int gateway_pairing_issue_if_absent(const char *platform, const char *user_id, char *code_out,
                                    size_t code_size)
{
   if (!platform || !user_id || !platform[0] || !user_id[0] || !code_out || code_size < 7)
      return -1;
   char path[512];
   pairs_path(path, sizeof(path));

   pthread_mutex_lock(&g_pairing_mutex);
   int lock_fd = pairs_lock(path, 1);
   if (lock_fd < 0)
   {
      pthread_mutex_unlock(&g_pairing_mutex);
      return -1;
   }
   cJSON *arr = pairs_load(path);
   if (!arr)
   {
      close(lock_fd);
      pthread_mutex_unlock(&g_pairing_mutex);
      return -1;
   }
   cJSON *e = find_entry(arr, platform, user_id);
   if (e)
   {
      const char *code = cJSON_GetStringValue(cJSON_GetObjectItem(e, "code"));
      double exp = cJSON_GetNumberValue(cJSON_GetObjectItem(e, "expires_at"));
      int pending_expired =
          !cJSON_IsTrue(cJSON_GetObjectItem(e, "approved")) && (time_t)exp < time(NULL);
      if (code && code[0] && !pending_expired)
      {
         snprintf(code_out, code_size, "%s", code);
         cJSON_Delete(arr);
         close(lock_fd);
         pthread_mutex_unlock(&g_pairing_mutex);
         return 0;
      }
      /* Expired pending entry: detach and delete it, then issue a fresh code. */
      int count = cJSON_GetArraySize(arr);
      for (int i = 0; i < count; i++)
      {
         if (cJSON_GetArrayItem(arr, i) == e)
         {
            cJSON_DeleteItemFromArray(arr, i);
            break;
         }
      }
   }

   char code[8];
   if (pairing_random_code(arr, code) != 0)
   {
      cJSON_Delete(arr);
      close(lock_fd);
      pthread_mutex_unlock(&g_pairing_mutex);
      return -1;
   }

   cJSON *entry = cJSON_CreateObject();
   cJSON_AddStringToObject(entry, "platform", platform);
   cJSON_AddStringToObject(entry, "user_id", user_id);
   cJSON_AddStringToObject(entry, "code", code);
   cJSON_AddNumberToObject(entry, "expires_at", (double)(time(NULL) + PAIRING_TTL_SECONDS));
   cJSON_AddItemToArray(arr, entry);

   int rc = pairs_save(path, arr);
   cJSON_Delete(arr);
   close(lock_fd);
   pthread_mutex_unlock(&g_pairing_mutex);
   if (rc != 0)
   {
      LOG_WARN("gateway", "failed to write pairing code to %s", path);
      return -1;
   }
   snprintf(code_out, code_size, "%s", code);
   return 0;
}
