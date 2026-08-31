/* db1/secrets.c: user-local secret storage facade. */

#include "aimee.h"
#include "secrets.h"
#include "secrets_internal.h"
#include "platform_path.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int file_secret_path(const char *key, char *buf, size_t len)
{
   const char *dir = config_output_dir();
   if (platform_mkdir_p(dir, 0700) != 0)
      return -1;
   snprintf(buf, len, "%s/%s", dir, key);
   return 0;
}

static int file_store(const char *service, const char *key, const char *value)
{
   (void)service;
   char path[MAX_PATH_LEN];
   if (file_secret_path(key, path, sizeof(path)) != 0)
      return -1;

   FILE *f = fopen(path, "w");
   if (!f)
      return -1;
   fprintf(f, "%s\n", value);
   fclose(f);
   platform_set_permissions(path, 0600);
   return 0;
}

static int file_load(const char *service, const char *key, char *buf, size_t len)
{
   (void)service;
   char path[MAX_PATH_LEN];
   if (file_secret_path(key, path, sizeof(path)) != 0)
      return -1;

   FILE *f = fopen(path, "r");
   if (!f)
      return -1;
   size_t n = fread(buf, 1, len - 1, f);
   fclose(f);
   buf[n] = '\0';
   while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
      buf[--n] = '\0';
   return n > 0 ? 0 : -1;
}

static int file_remove(const char *service, const char *key)
{
   (void)service;
   char path[MAX_PATH_LEN];
   if (file_secret_path(key, path, sizeof(path)) != 0)
      return -1;
   return platform_unlink(path);
}

static const db1_secret_backend_t file_backend = {
    .store = file_store,
    .load = file_load,
    .remove = file_remove,
    .name = "file",
};

static const db1_secret_backend_t *secret_backend(void)
{
   static const db1_secret_backend_t *cached = NULL;
   if (!cached)
      cached = db1_secret_backend_detect(&file_backend);
   return cached;
}

int db1_secret_store(const char *key, const char *value)
{
   const db1_secret_backend_t *backend = secret_backend();
   if (!backend)
      return -1;
   return backend->store(AIMEE_SECRET_SERVICE_NAME, key, value);
}

int db1_secret_load(const char *key, char *buf, size_t len)
{
   const db1_secret_backend_t *backend = secret_backend();
   if (!backend)
      return -1;
   return backend->load(AIMEE_SECRET_SERVICE_NAME, key, buf, len);
}

int db1_secret_remove(const char *key)
{
   const db1_secret_backend_t *backend = secret_backend();
   if (!backend)
      return -1;
   return backend->remove(AIMEE_SECRET_SERVICE_NAME, key);
}
