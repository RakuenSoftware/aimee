/* Immutable identity and approval for executable agent artifacts. */
#include "artifact_trust.h"

#include "aimee_home.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <BaseTsd.h>
#include <direct.h>
#include <io.h>
#ifndef _SSIZE_T_DEFINED
typedef SSIZE_T ssize_t;
#define _SSIZE_T_DEFINED
#endif
#define open     _open
#define read     _read
#define write    _write
#define close    _close
#define unlink   _unlink
#define chmod    _chmod
#define fsync    _commit
#define fstat    _fstat
#define lstat    _stat
#define strtok_r strtok_s
#ifndef S_ISREG
#define S_ISREG(mode) (((mode) & _S_IFMT) == _S_IFREG)
#endif
static int artifact_mkdir(const char *path, int mode)
{
   (void)mode;
   return _mkdir(path);
}
#define mkdir artifact_mkdir
static char *artifact_realpath(const char *path, char *unused)
{
   (void)unused;
   return _fullpath(NULL, path, 0);
}
#define realpath artifact_realpath
#else
#include <unistd.h>
#endif

#ifndef _WIN32
#ifdef __APPLE__
#define AIMEE_OPTIONAL_SYMBOL __attribute__((weak_import))
#else
#define AIMEE_OPTIONAL_SYMBOL __attribute__((weak))
#endif
extern void obs_bus_emit_durable_event(const char *, const char *, const char *,
                                       const char *) AIMEE_OPTIONAL_SYMBOL;
extern void audit_action_log(const char *, const char *, const char *, const char *, const char *,
                             const char *, const char *, long long) AIMEE_OPTIONAL_SYMBOL;
#endif

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

static int unhex(const char *s, size_t n, unsigned char *out, size_t out_n)
{
   if (!s || n != out_n * 2)
      return -1;
   for (size_t i = 0; i < out_n; i++)
   {
      int hi = isdigit((unsigned char)s[i * 2])     ? s[i * 2] - '0'
               : s[i * 2] >= 'a' && s[i * 2] <= 'f' ? s[i * 2] - 'a' + 10
               : s[i * 2] >= 'A' && s[i * 2] <= 'F' ? s[i * 2] - 'A' + 10
                                                    : -1;
      int lo = isdigit((unsigned char)s[i * 2 + 1])         ? s[i * 2 + 1] - '0'
               : s[i * 2 + 1] >= 'a' && s[i * 2 + 1] <= 'f' ? s[i * 2 + 1] - 'a' + 10
               : s[i * 2 + 1] >= 'A' && s[i * 2 + 1] <= 'F' ? s[i * 2 + 1] - 'A' + 10
                                                            : -1;
      if (hi < 0 || lo < 0)
         return -1;
      out[i] = (unsigned char)((hi << 4) | lo);
   }
   return 0;
}

static void sha_hex(const void *bytes, size_t len, char out[65])
{
   unsigned char digest[SHA256_DIGEST_LENGTH];
   SHA256(bytes, len, digest);
   for (size_t i = 0; i < sizeof(digest); i++)
      snprintf(out + i * 2, 3, "%02x", digest[i]);
}

static char *read_private_file(const char *path, size_t max_len)
{
   int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
   if (fd < 0)
      return NULL;
   struct stat st;
   if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_size <= 0 ||
       (uintmax_t)st.st_size > max_len || (st.st_mode & 0022))
   {
      close(fd);
      return NULL;
   }
   char *buf = malloc((size_t)st.st_size + 1);
   size_t used = 0;
   if (!buf)
   {
      close(fd);
      return NULL;
   }
   while (used < (size_t)st.st_size)
   {
      ssize_t n = read(fd, buf + used, (size_t)st.st_size - used);
      if (n < 0 && errno == EINTR)
         continue;
      if (n <= 0)
      {
         free(buf);
         close(fd);
         return NULL;
      }
      used += (size_t)n;
   }
   close(fd);
   buf[used] = '\0';
   return buf;
}

static int signed_manifest_allows(const char *artifact_class, const char *artifact_id,
                                  const char *canonical_path, const char *digest)
{
   const char *manifest_path = getenv("AIMEE_ARTIFACT_APPROVAL_MANIFEST");
   const char *public_hex = getenv("AIMEE_ARTIFACT_APPROVAL_PUBLIC_KEY");
   if (!manifest_path || !manifest_path[0] || !public_hex)
      return 0;
   char sig_path[4096];
   if (snprintf(sig_path, sizeof(sig_path), "%s.sig", manifest_path) >= (int)sizeof(sig_path))
      return 0;
   char *manifest = read_private_file(manifest_path, 1024u * 1024u);
   char *sig_hex = read_private_file(sig_path, 256);
   if (!manifest || !sig_hex)
   {
      free(manifest);
      free(sig_hex);
      return 0;
   }
   sig_hex[strcspn(sig_hex, "\r\n")] = '\0';
   unsigned char public_key[32], signature[64];
   int valid = unhex(public_hex, strlen(public_hex), public_key, sizeof(public_key)) == 0 &&
               unhex(sig_hex, strlen(sig_hex), signature, sizeof(signature)) == 0;
   EVP_PKEY *key =
       valid ? EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, public_key, sizeof(public_key))
             : NULL;
   EVP_MD_CTX *ctx = key ? EVP_MD_CTX_new() : NULL;
   valid = ctx && EVP_DigestVerifyInit(ctx, NULL, NULL, NULL, key) == 1 &&
           EVP_DigestVerify(ctx, signature, sizeof(signature), (const unsigned char *)manifest,
                            strlen(manifest)) == 1;
   EVP_MD_CTX_free(ctx);
   EVP_PKEY_free(key);
   free(sig_hex);
   if (!valid)
   {
      free(manifest);
      return 0;
   }
   char wanted[8192];
   if (snprintf(wanted, sizeof(wanted), "%s  %s:%s %s", digest, artifact_class, artifact_id,
                canonical_path) >= (int)sizeof(wanted))
   {
      free(manifest);
      return 0;
   }
   int approved = 0;
   char *save = NULL;
   for (char *line = strtok_r(manifest, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
      if (!strcmp(line, wanted))
      {
         approved = 1;
         break;
      }
   free(manifest);
   return approved;
}

static int standard_pin_allows(const char *artifact_class, const char *artifact_id,
                               const char *canonical_path, const char *digest)
{
   char identity[8192], key[65], dir[4096], path[4096];
   if (snprintf(identity, sizeof(identity), "%s:%s %s", artifact_class, artifact_id,
                canonical_path) >= (int)sizeof(identity))
      return 0;
   sha_hex(identity, strlen(identity), key);
   if (snprintf(dir, sizeof(dir), "%s/artifact-trust", aimee_home()) >= (int)sizeof(dir) ||
       snprintf(path, sizeof(path), "%s/%s.pin", dir, key) >= (int)sizeof(path))
      return 0;
   if (mkdir(dir, 0700) != 0 && errno != EEXIST)
      return 0;
   (void)chmod(dir, 0700);
   int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
   if (fd >= 0)
   {
      size_t n = strlen(digest);
      int ok = write(fd, digest, n) == (ssize_t)n && write(fd, "\n", 1) == 1 && fsync(fd) == 0;
      if (close(fd) != 0)
         ok = 0;
      if (!ok)
         unlink(path);
      return ok;
   }
   if (errno != EEXIST)
      return 0;
   char *pin = read_private_file(path, 128);
   if (!pin)
      return 0;
   pin[strcspn(pin, "\r\n")] = '\0';
   int allowed = strlen(pin) == 64 && !strcmp(pin, digest);
   free(pin);
   return allowed;
}

int artifact_trust_verify_bytes(const char *artifact_class, const char *artifact_id,
                                const char *canonical_path, const void *bytes, size_t len,
                                char digest_hex[65], char *err, size_t errlen)
{
   if (err && errlen)
      err[0] = '\0';
   if (!artifact_class || !artifact_class[0] || !artifact_id || !artifact_id[0] ||
       !canonical_path || !canonical_path[0] || (!bytes && len))
      return -1;
   char digest[65];
   sha_hex(bytes ? bytes : "", len, digest);
   if (digest_hex)
      snprintf(digest_hex, 65, "%s", digest);
   const char *mode = getenv("AIMEE_ARTIFACT_TRUST_MODE");
   int hardened = mode && !strcmp(mode, "hardened");
   int allowed = hardened
                     ? signed_manifest_allows(artifact_class, artifact_id, canonical_path, digest)
                     : standard_pin_allows(artifact_class, artifact_id, canonical_path, digest);
   char detail[384];
   snprintf(detail, sizeof(detail),
            "{\"class\":\"%.48s\",\"id\":\"%.96s\",\"digest\":\"%s\","
            "\"mode\":\"%s\"}",
            artifact_class, artifact_id, digest, hardened ? "hardened" : "standard");
   /* Server/KB builds reach the synchronous WORM bridge; thin/offline builds
    * retain the same content-free verdict in their local audit log. */
#ifndef _WIN32
   if (obs_bus_emit_durable_event)
      obs_bus_emit_durable_event("artifact.load", artifact_class, allowed ? "allow" : "deny",
                                 detail);
   else
#endif
#ifndef _WIN32
       if (audit_action_log)
   {
      char action_hash[68];
      snprintf(action_hash, sizeof(action_hash), "v1-%s", digest);
      audit_action_log("artifact-security", "artifact.load", action_hash, artifact_class,
                       hardened ? "hardened" : "standard", allowed ? "approved" : "changed",
                       allowed ? "allow" : "deny", 0);
   }
#endif
   if (!allowed && err && errlen)
      snprintf(err, errlen, "%s artifact '%s' is unapproved or changed", artifact_class,
               artifact_id);
   return allowed ? 0 : -1;
}

int artifact_trust_read_file(const char *artifact_class, const char *artifact_id, const char *path,
                             size_t max_len, char **out, size_t *out_len, char digest_hex[65],
                             char *err, size_t errlen)
{
   if (out)
      *out = NULL;
   if (out_len)
      *out_len = 0;
   if (!out || !path || !path[0] || max_len == 0)
      return -1;
   char *canonical = realpath(path, NULL);
   int fd = canonical ? open(canonical, O_RDONLY | O_CLOEXEC | O_NOFOLLOW) : -1;
   struct stat st;
   if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1 ||
       st.st_size < 0 || (uintmax_t)st.st_size > max_len)
   {
      if (fd >= 0)
         close(fd);
      free(canonical);
      if (err && errlen)
         snprintf(err, errlen, "unsafe or oversized artifact file");
      return -1;
   }
   char *buf = malloc((size_t)st.st_size + 1);
   size_t used = 0;
   if (!buf)
   {
      close(fd);
      free(canonical);
      return -1;
   }
   while (used < (size_t)st.st_size)
   {
      ssize_t n = read(fd, buf + used, (size_t)st.st_size - used);
      if (n < 0 && errno == EINTR)
         continue;
      if (n <= 0)
         break;
      used += (size_t)n;
   }
   close(fd);
   buf[used] = '\0';
   int rc = used == (size_t)st.st_size
                ? artifact_trust_verify_bytes(artifact_class, artifact_id, canonical, buf, used,
                                              digest_hex, err, errlen)
                : -1;
   free(canonical);
   if (rc != 0)
   {
      memset(buf, 0, used);
      free(buf);
      return -1;
   }
   *out = buf;
   if (out_len)
      *out_len = used;
   return 0;
}
