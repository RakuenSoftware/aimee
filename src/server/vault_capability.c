/* vault_capability.c: see vault_capability.h. A 0600 line-per-principal allow-list
 * for the vault:write:server capability, with atomic (tmp+rename) rewrites. */
#include "vault_capability.h"
#include "config.h"        /* config_default_dir */
#include "platform_path.h" /* platform_mkdir_p */

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static pthread_mutex_t g_cap_mu = PTHREAD_MUTEX_INITIALIZER;
static char g_cap_path_override[512];

/* Compose <config_default_dir>/.vault/server-write-grants (or the test override). */
static int cap_path(char *out, size_t cap)
{
   if (g_cap_path_override[0])
      return (size_t)snprintf(out, cap, "%s", g_cap_path_override) >= cap ? -1 : 0;
   const char *base = config_default_dir();
   if (!base || !base[0])
      return -1;
   return (size_t)snprintf(out, cap, "%s/.vault/server-write-grants", base) >= cap ? -1 : 0;
}

void vault_capability_set_path_for_test(const char *path)
{
   pthread_mutex_lock(&g_cap_mu);
   if (path)
      snprintf(g_cap_path_override, sizeof(g_cap_path_override), "%s", path);
   else
      g_cap_path_override[0] = '\0';
   pthread_mutex_unlock(&g_cap_mu);
}

/* A principal must be non-empty, single-line, and bounded, so it cannot inject a
 * second grant line or overflow the store. */
static int principal_valid(const char *p)
{
   if (!p || !p[0])
      return 0;
   size_t n = strlen(p);
   if (n >= 160)
      return 0;
   return strchr(p, '\n') == NULL && strchr(p, '\r') == NULL;
}

/* Read the whole store into a malloc'd NUL-terminated buffer (caller frees), or
 * NULL when the file is absent/empty. */
static char *cap_read(const char *path)
{
   FILE *f = fopen(path, "r");
   if (!f)
      return NULL;
   if (fseek(f, 0, SEEK_END) != 0)
   {
      fclose(f);
      return NULL;
   }
   long sz = ftell(f);
   if (sz < 0 || fseek(f, 0, SEEK_SET) != 0)
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
   size_t rd = fread(buf, 1, (size_t)sz, f);
   fclose(f);
   buf[rd] = '\0';
   return buf;
}

/* Whole-line exact match of `principal` within the newline-separated `buf`. */
static int line_match(const char *buf, const char *principal)
{
   if (!buf)
      return 0;
   size_t plen = strlen(principal);
   const char *p = buf;
   while (*p)
   {
      const char *nl = strchr(p, '\n');
      size_t linelen = nl ? (size_t)(nl - p) : strlen(p);
      if (linelen == plen && strncmp(p, principal, plen) == 0)
         return 1;
      if (!nl)
         break;
      p = nl + 1;
   }
   return 0;
}

/* Atomic rewrite: ensure the parent dir, write tmp at 0600, fsync, rename. */
static int cap_write_atomic(const char *path, const char *content, size_t content_len)
{
   char dir[512];
   snprintf(dir, sizeof(dir), "%s", path);
   char *slash = strrchr(dir, '/');
   if (slash)
   {
      *slash = '\0';
      platform_mkdir_p(dir, 0700);
   }
   char tmp[600];
   if ((size_t)snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= sizeof(tmp))
      return -1;
   int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
   if (fd < 0)
      return -1;
   int ok = 1;
   if (content_len > 0)
   {
      ssize_t w = write(fd, content, content_len);
      ok = (w >= 0 && (size_t)w == content_len);
   }
   if (ok)
      (void)fsync(fd);
   close(fd);
   if (!ok || rename(tmp, path) != 0)
   {
      unlink(tmp);
      return -1;
   }
   /* fsync the parent dir so the rename (the new directory entry) is durable. */
   if (slash)
   {
      int dfd = open(dir, O_RDONLY | O_DIRECTORY);
      if (dfd >= 0)
      {
         (void)fsync(dfd);
         close(dfd);
      }
   }
   return 0;
}

int vault_capability_grant(const char *principal)
{
   if (!principal_valid(principal))
      return -1;
   char path[512];
   pthread_mutex_lock(&g_cap_mu);
   int rc = -1;
   if (cap_path(path, sizeof(path)) == 0)
   {
      char *buf = cap_read(path);
      if (buf && line_match(buf, principal))
      {
         rc = 0; /* already granted — idempotent */
      }
      else
      {
         size_t blen = buf ? strlen(buf) : 0;
         /* ensure a separating newline after any existing content */
         int need_sep = (blen > 0 && buf[blen - 1] != '\n');
         size_t need = blen + (need_sep ? 1 : 0) + strlen(principal) + 1;
         char *out = malloc(need + 1);
         if (out)
         {
            size_t off = 0;
            if (blen)
            {
               memcpy(out, buf, blen);
               off = blen;
            }
            if (need_sep)
               out[off++] = '\n';
            off += (size_t)snprintf(out + off, need + 1 - off, "%s\n", principal);
            rc = cap_write_atomic(path, out, off);
            free(out);
         }
      }
      free(buf);
   }
   pthread_mutex_unlock(&g_cap_mu);
   return rc;
}

int vault_capability_revoke(const char *principal)
{
   if (!principal_valid(principal))
      return -1;
   char path[512];
   pthread_mutex_lock(&g_cap_mu);
   int rc = -1;
   if (cap_path(path, sizeof(path)) == 0)
   {
      char *buf = cap_read(path);
      if (!buf || !line_match(buf, principal))
      {
         rc = 0; /* not present — idempotent */
      }
      else
      {
         size_t plen = strlen(principal);
         char *out = malloc(strlen(buf) + 1);
         if (out)
         {
            size_t off = 0;
            const char *p = buf;
            while (*p)
            {
               const char *nl = strchr(p, '\n');
               size_t linelen = nl ? (size_t)(nl - p) : strlen(p);
               int drop = (linelen == plen && strncmp(p, principal, plen) == 0);
               if (!drop && linelen > 0)
               {
                  memcpy(out + off, p, linelen);
                  off += linelen;
                  out[off++] = '\n';
               }
               if (!nl)
                  break;
               p = nl + 1;
            }
            out[off] = '\0';
            rc = cap_write_atomic(path, out, off);
            free(out);
         }
      }
      free(buf);
   }
   pthread_mutex_unlock(&g_cap_mu);
   return rc;
}

int vault_capability_server_write_allowed(attested_transport_t transport, const char *principal)
{
   int attested = (transport == ATTEST_UDS_PEERCRED || transport == ATTEST_WEBCHAT_TRUSTED);
   return attested && vault_capability_has(principal);
}

int vault_capability_has(const char *principal)
{
   if (!principal_valid(principal))
      return 0;
   char path[512];
   pthread_mutex_lock(&g_cap_mu);
   int has = 0;
   if (cap_path(path, sizeof(path)) == 0)
   {
      char *buf = cap_read(path);
      has = line_match(buf, principal);
      free(buf);
   }
   pthread_mutex_unlock(&g_cap_mu);
   return has;
}

int vault_capability_list(char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return -1;
   out[0] = '\0';
   char path[512];
   pthread_mutex_lock(&g_cap_mu);
   int count = -1;
   if (cap_path(path, sizeof(path)) == 0)
   {
      char *buf = cap_read(path);
      count = 0;
      if (buf)
      {
         snprintf(out, out_len, "%s", buf);
         for (const char *p = buf; *p; p++)
            if (*p == '\n')
               count++;
         /* a final line without a trailing newline still counts */
         size_t blen = strlen(buf);
         if (blen > 0 && buf[blen - 1] != '\n')
            count++;
      }
      free(buf);
   }
   pthread_mutex_unlock(&g_cap_mu);
   return count;
}
