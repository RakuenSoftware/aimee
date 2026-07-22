#define _POSIX_C_SOURCE 200809L
#include "kb_management_cert_storage.h"
#include "kb_management_cert_codec.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static int exact_hex(const char *s, size_t n)
{
   if (!s || strnlen(s, n + 1) != n)
      return 0;
   for (size_t i = 0; i < n; ++i)
      if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
         return 0;
   return 1;
}

static int memory_overlap(const void *a, size_t an, const void *b, size_t bn)
{
   if (!a || !b || !an || !bn)
      return 0;
   uintptr_t ap = (uintptr_t)a, bp = (uintptr_t)b;
   return ap < bp ? bp - ap < an : ap - bp < bn;
}

static int checked_dir(int fd)
{
   struct stat st;
   return fd >= 0 && fstat(fd, &st) == 0 && S_ISDIR(st.st_mode) &&
#ifdef AIMEE_MANAGEMENT_CERT_TESTING
          st.st_uid == geteuid() &&
#else
          st.st_uid == 0 &&
#endif
          (st.st_mode & 0022) == 0;
}

static int checked_file(int fd)
{
   struct stat st;
   return fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
#ifdef AIMEE_MANAGEMENT_CERT_TESTING
          st.st_uid == geteuid() &&
#else
          st.st_uid == 0 &&
#endif
          (st.st_mode & 0777) == 0600 && st.st_nlink == 1;
}

static kb_management_cert_storage_result_t open_error(int error, int missing_ok)
{
   if (missing_ok && error == ENOENT)
      return KB_MANAGEMENT_STORAGE_MISSING;
   if (error == ELOOP || error == ENOTDIR || error == EISDIR || error == ENXIO)
      return KB_MANAGEMENT_STORAGE_INTEGRITY;
   return KB_MANAGEMENT_STORAGE_UNAVAILABLE;
}

static int open_absolute_dir(const char *path)
{
   if (!path || path[0] != '/' || !path[1] || strnlen(path, PATH_MAX) >= PATH_MAX)
      return -1;
   int fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
   if (fd < 0)
      return -1;
   const char *p = path + 1;
   while (*p)
   {
      const char *slash = strchr(p, '/');
      size_t n = slash ? (size_t)(slash - p) : strlen(p);
      if (!n || n > NAME_MAX || (n == 1 && p[0] == '.') || (n == 2 && p[0] == '.' && p[1] == '.'))
      {
         close(fd);
         return -1;
      }
      char component[NAME_MAX + 1];
      memcpy(component, p, n);
      component[n] = 0;
      int next = openat(fd, component, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      close(fd);
      if (next < 0 || !checked_dir(next))
      {
         if (next >= 0)
            close(next);
         return -1;
      }
      fd = next;
      if (!slash)
         return fd;
      p = slash + 1;
   }
   close(fd);
   return -1;
}

kb_management_cert_storage_result_t
kb_management_cert_storage_open(const char *path, kb_management_cert_storage_t *out)
{
   if (!out)
      return KB_MANAGEMENT_STORAGE_INTEGRITY;
   out->dir_fd = -1;
   int fd = open_absolute_dir(path);
   if (fd < 0)
      return errno == EACCES || errno == EAGAIN ? KB_MANAGEMENT_STORAGE_UNAVAILABLE
                                                : KB_MANAGEMENT_STORAGE_INTEGRITY;
   if (flock(fd, LOCK_EX | LOCK_NB) != 0)
   {
      close(fd);
      return errno == EWOULDBLOCK ? KB_MANAGEMENT_STORAGE_CONFLICT
                                  : KB_MANAGEMENT_STORAGE_UNAVAILABLE;
   }
   out->dir_fd = fd;
   return KB_MANAGEMENT_STORAGE_OK;
}

void kb_management_cert_storage_close(kb_management_cert_storage_t *storage)
{
   if (storage && storage->dir_fd >= 0)
   {
      flock(storage->dir_fd, LOCK_UN);
      close(storage->dir_fd);
      storage->dir_fd = -1;
   }
}

static int record_name(const char *kind, const char operation[65], char out[80])
{
   if (kind && ((!strcmp(kind, "intent") && exact_hex(operation, 64)) ||
                (!strcmp(kind, "candidate") && exact_hex(operation, 64))))
      return snprintf(out, 80, "%s.%s", kind, operation) == (int)(strlen(kind) + 1 + 64) ? 0 : -1;
   return -1;
}

static int read_alias(const kb_management_cert_storage_t *storage, uint8_t *out, size_t cap,
                      size_t *out_len)
{
   return memory_overlap(out, cap, out_len, out_len ? sizeof(*out_len) : 0) ||
          memory_overlap(out, cap, storage, storage ? sizeof(*storage) : 0) ||
          memory_overlap(out_len, out_len ? sizeof(*out_len) : 0, storage,
                         storage ? sizeof(*storage) : 0);
}

static void clear_read_output(uint8_t *out, size_t cap, size_t *out_len)
{
   if (out_len)
      *out_len = 0;
   if (out && cap)
      OPENSSL_cleanse(out, cap);
}

static kb_management_cert_storage_result_t read_name(kb_management_cert_storage_t *storage,
                                                     const char *name, uint8_t *out, size_t cap,
                                                     size_t *out_len)
{
   if (read_alias(storage, out, cap, out_len))
      return KB_MANAGEMENT_STORAGE_INTEGRITY;
   clear_read_output(out, cap, out_len);
   if (!storage || storage->dir_fd < 0 || !name || !out || !cap || !out_len)
      return KB_MANAGEMENT_STORAGE_INTEGRITY;
   int fd = openat(storage->dir_fd, name, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
   if (fd < 0)
      return open_error(errno, 1);
   if (!checked_file(fd))
   {
      close(fd);
      return KB_MANAGEMENT_STORAGE_INTEGRITY;
   }
   size_t used = 0;
   while (used < cap)
   {
      ssize_t n = read(fd, out + used, cap - used);
      if (n > 0)
         used += (size_t)n;
      else if (!n)
         break;
      else if (errno != EINTR)
      {
         close(fd);
         OPENSSL_cleanse(out, cap);
         return KB_MANAGEMENT_STORAGE_UNAVAILABLE;
      }
   }
   uint8_t extra;
   ssize_t more;
   do
      more = read(fd, &extra, 1);
   while (more < 0 && errno == EINTR);
   close(fd);
   if (!used || more != 0)
   {
      OPENSSL_cleanse(out, cap);
      return more < 0 ? KB_MANAGEMENT_STORAGE_UNAVAILABLE : KB_MANAGEMENT_STORAGE_INTEGRITY;
   }
   *out_len = used;
   return KB_MANAGEMENT_STORAGE_OK;
}

kb_management_cert_storage_result_t
kb_management_cert_storage_read(kb_management_cert_storage_t *storage, const char *kind,
                                const char operation[65], uint8_t *out, size_t cap, size_t *out_len)
{
   char name[80];
   if (read_alias(storage, out, cap, out_len))
      return KB_MANAGEMENT_STORAGE_INTEGRITY;
   if (record_name(kind, operation, name))
   {
      clear_read_output(out, cap, out_len);
      return KB_MANAGEMENT_STORAGE_INTEGRITY;
   }
   return read_name(storage, name, out, cap, out_len);
}

static int complete_write(int fd, const uint8_t *bytes, size_t len)
{
   size_t used = 0;
   while (used < len)
   {
      ssize_t n = write(fd, bytes + used, len - used);
      if (n > 0)
         used += (size_t)n;
      else if (n < 0 && errno == EINTR)
         continue;
      else
         return -1;
   }
   return 0;
}

static int verify_contents(int fd, const void *bytes, size_t len)
{
   if (len > KB_MANAGEMENT_CERT_CANDIDATE_MAX || lseek(fd, 0, SEEK_SET) < 0)
      return -1;
   uint8_t verify[KB_MANAGEMENT_CERT_CANDIDATE_MAX];
   size_t used = 0;
   while (used < len)
   {
      ssize_t n = read(fd, verify + used, len - used);
      if (n > 0)
         used += (size_t)n;
      else if (n < 0 && errno == EINTR)
         continue;
      else
         break;
   }
   uint8_t extra;
   ssize_t more;
   do
      more = read(fd, &extra, 1);
   while (more < 0 && errno == EINTR);
   int same = used == len && more == 0 && CRYPTO_memcmp(verify, bytes, len) == 0;
   OPENSSL_cleanse(verify, sizeof(verify));
   return same ? 0 : -1;
}

/* 0 complete, 1 oversize, -1 I/O failure. */
static int read_small_contents(int fd, uint8_t out[1024], size_t *out_len)
{
   if (!out || !out_len || lseek(fd, 0, SEEK_SET) < 0)
      return -1;
   *out_len = 0;
   OPENSSL_cleanse(out, 1024);
   size_t used = 0;
   while (used < 1024)
   {
      ssize_t n = read(fd, out + used, 1024 - used);
      if (n > 0)
         used += (size_t)n;
      else if (!n)
         break;
      else if (errno != EINTR)
      {
         OPENSSL_cleanse(out, 1024);
         return -1;
      }
   }
   uint8_t extra;
   ssize_t more;
   do
      more = read(fd, &extra, 1);
   while (more < 0 && errno == EINTR);
   if (more < 0)
   {
      OPENSSL_cleanse(out, 1024);
      return -1;
   }
   if (more > 0)
   {
      OPENSSL_cleanse(out, 1024);
      return 1;
   }
   *out_len = used;
   return 0;
}

static int valid_operation_record(const char *, const char[65], const void *, size_t);

static kb_management_cert_storage_result_t replay_existing(kb_management_cert_storage_t *storage,
                                                           const char *name, const char *kind,
                                                           const char operation[65],
                                                           const void *bytes, size_t len)
{
   int fd = openat(storage->dir_fd, name, O_RDWR | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
   if (fd < 0)
      return open_error(errno, 0);
   if (!checked_file(fd))
   {
      close(fd);
      return KB_MANAGEMENT_STORAGE_INTEGRITY;
   }
   uint8_t existing[KB_MANAGEMENT_CERT_CANDIDATE_MAX];
   size_t used = 0;
   while (used < sizeof(existing))
   {
      ssize_t n = read(fd, existing + used, sizeof(existing) - used);
      if (n > 0)
         used += (size_t)n;
      else if (!n)
         break;
      else if (errno != EINTR)
      {
         OPENSSL_cleanse(existing, sizeof(existing));
         close(fd);
         return KB_MANAGEMENT_STORAGE_UNAVAILABLE;
      }
   }
   uint8_t extra;
   ssize_t more;
   do
      more = read(fd, &extra, 1);
   while (more < 0 && errno == EINTR);
   int valid = more == 0 && used && valid_operation_record(kind, operation, existing, used);
   int exact = valid && used == len && CRYPTO_memcmp(existing, bytes, len) == 0;
   OPENSSL_cleanse(existing, sizeof(existing));
   if (more < 0)
   {
      close(fd);
      return KB_MANAGEMENT_STORAGE_UNAVAILABLE;
   }
   if (!valid)
   {
      close(fd);
      return KB_MANAGEMENT_STORAGE_INTEGRITY;
   }
   if (!exact)
   {
      close(fd);
      return KB_MANAGEMENT_STORAGE_CONFLICT;
   }
   int sync_error = fdatasync(fd) != 0;
   if (close(fd) != 0)
      sync_error = 1;
   if (!sync_error && fsync(storage->dir_fd) != 0)
      sync_error = 1;
   return sync_error ? KB_MANAGEMENT_STORAGE_UNAVAILABLE : KB_MANAGEMENT_STORAGE_OK;
}

static int valid_operation_record(const char *kind, const char operation[65], const void *bytes,
                                  size_t len)
{
   if (!kind || !operation || !bytes || !len)
      return 0;
   if (!strcmp(kind, "intent"))
   {
      kb_management_cert_intent_view_t intent;
      return kb_management_cert_intent_decode(bytes, len, &intent) == 0 &&
             !strcmp(intent.operation_id, operation);
   }
   if (!strcmp(kind, "candidate"))
   {
      kb_management_cert_candidate_view_t candidate;
      return kb_management_cert_candidate_decode(bytes, len, &candidate) == 0 &&
             !strcmp(candidate.operation_id, operation);
   }
   return 0;
}

static int valid_pending_record(const void *bytes, size_t len)
{
   kb_management_cert_pending_manifest_t pending;
   return kb_management_cert_pending_decode(bytes, len, &pending) == 0;
}

static int valid_current_record(const void *bytes, size_t len)
{
   kb_management_cert_manifest_t current;
   return kb_management_cert_manifest_decode(bytes, len, &current) == 0;
}

static kb_management_cert_storage_result_t
inspect_existing_pending(kb_management_cert_storage_t *storage)
{
   uint8_t existing[1024];
   size_t len = 0;
   kb_management_cert_storage_result_t rc =
       read_name(storage, "pending", existing, sizeof(existing), &len);
   if (rc == KB_MANAGEMENT_STORAGE_OK)
      rc = valid_pending_record(existing, len) ? KB_MANAGEMENT_STORAGE_CONFLICT
                                               : KB_MANAGEMENT_STORAGE_INTEGRITY;
   OPENSSL_cleanse(existing, sizeof(existing));
   return rc;
}

kb_management_cert_storage_result_t
kb_management_cert_storage_stage(kb_management_cert_storage_t *storage, const char *kind,
                                 const char operation[65], const void *bytes, size_t len)
{
   char name[80];
   if (!storage || storage->dir_fd < 0 || record_name(kind, operation, name) || !bytes || !len ||
       len > KB_MANAGEMENT_CERT_CANDIDATE_MAX ||
       !valid_operation_record(kind, operation, bytes, len))
      return KB_MANAGEMENT_STORAGE_INTEGRITY;
   int fd = openat(storage->dir_fd, name, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
   if (fd < 0 && errno == EEXIST)
      return replay_existing(storage, name, kind, operation, bytes, len);
   if (fd < 0)
      return open_error(errno, 0);
   kb_management_cert_storage_result_t rc = KB_MANAGEMENT_STORAGE_UNAVAILABLE;
   if (!checked_file(fd))
   {
      rc = KB_MANAGEMENT_STORAGE_INTEGRITY;
      goto failed;
   }
   if (complete_write(fd, bytes, len) || fdatasync(fd))
      goto failed;
   if (verify_contents(fd, bytes, len))
   {
      close(fd);
      goto unlink_failed;
   }
   if (!checked_file(fd))
   {
      rc = KB_MANAGEMENT_STORAGE_INTEGRITY;
      close(fd);
      goto unlink_failed;
   }
   if (close(fd) || fsync(storage->dir_fd))
      goto unlink_failed;
   return KB_MANAGEMENT_STORAGE_OK;

failed:
   close(fd);
unlink_failed:
   if (unlinkat(storage->dir_fd, name, 0) == 0)
      fsync(storage->dir_fd);
   return rc;
}

static int random_hex(char out[33])
{
   static const char digits[] = "0123456789abcdef";
   uint8_t random[16];
   if (RAND_bytes(random, sizeof(random)) != 1)
      return -1;
   for (size_t i = 0; i < sizeof(random); ++i)
   {
      out[2 * i] = digits[random[i] >> 4];
      out[2 * i + 1] = digits[random[i] & 15];
   }
   out[32] = 0;
   OPENSSL_cleanse(random, sizeof(random));
   return 0;
}

kb_management_cert_storage_result_t
kb_management_cert_storage_promote(kb_management_cert_storage_t *storage, const void *bytes,
                                   size_t len)
{
   if (!storage || storage->dir_fd < 0 || !bytes || !len || len > 1024 ||
       !valid_current_record(bytes, len))
      return KB_MANAGEMENT_STORAGE_INTEGRITY;
   char suffix[33], name[48];
   if (random_hex(suffix) || snprintf(name, sizeof(name), "current.tmp.%s", suffix) != 44)
      return KB_MANAGEMENT_STORAGE_UNAVAILABLE;
   int fd = openat(storage->dir_fd, name, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
   if (fd < 0)
      return KB_MANAGEMENT_STORAGE_UNAVAILABLE;
   if (!checked_file(fd))
   {
      close(fd);
      unlinkat(storage->dir_fd, name, 0);
      fsync(storage->dir_fd);
      return KB_MANAGEMENT_STORAGE_INTEGRITY;
   }
   if (complete_write(fd, bytes, len) || fdatasync(fd) || verify_contents(fd, bytes, len))
   {
      close(fd);
      unlinkat(storage->dir_fd, name, 0);
      fsync(storage->dir_fd);
      return KB_MANAGEMENT_STORAGE_UNAVAILABLE;
   }
   if (!checked_file(fd))
   {
      close(fd);
      unlinkat(storage->dir_fd, name, 0);
      fsync(storage->dir_fd);
      return KB_MANAGEMENT_STORAGE_INTEGRITY;
   }
   if (close(fd) || renameat(storage->dir_fd, name, storage->dir_fd, "current") ||
       fsync(storage->dir_fd))
   {
      unlinkat(storage->dir_fd, name, 0);
      fsync(storage->dir_fd);
      return KB_MANAGEMENT_STORAGE_UNAVAILABLE;
   }
   return KB_MANAGEMENT_STORAGE_OK;
}

kb_management_cert_storage_result_t
kb_management_cert_storage_current(kb_management_cert_storage_t *storage, uint8_t *out, size_t cap,
                                   size_t *out_len)
{
   return read_name(storage, "current", out, cap, out_len);
}

kb_management_cert_storage_result_t
kb_management_cert_storage_pending_publish(kb_management_cert_storage_t *storage, const void *bytes,
                                           size_t len)
{
   if (!storage || storage->dir_fd < 0 || !bytes || !len || len > 1024 ||
       !valid_pending_record(bytes, len))
      return KB_MANAGEMENT_STORAGE_INTEGRITY;
   int fd =
       openat(storage->dir_fd, "pending", O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
   if (fd < 0 && errno == EEXIST)
      return inspect_existing_pending(storage);
   if (fd < 0)
      return open_error(errno, 0);
   if (!checked_file(fd))
   {
      close(fd);
      if (unlinkat(storage->dir_fd, "pending", 0) == 0)
         fsync(storage->dir_fd);
      return KB_MANAGEMENT_STORAGE_INTEGRITY;
   }
   if (complete_write(fd, bytes, len) || fdatasync(fd) || verify_contents(fd, bytes, len))
   {
      close(fd);
      if (unlinkat(storage->dir_fd, "pending", 0) == 0)
         fsync(storage->dir_fd);
      return KB_MANAGEMENT_STORAGE_UNAVAILABLE;
   }
   if (!checked_file(fd))
   {
      close(fd);
      if (unlinkat(storage->dir_fd, "pending", 0) == 0)
         fsync(storage->dir_fd);
      return KB_MANAGEMENT_STORAGE_INTEGRITY;
   }
   int sync_error = close(fd) != 0;
   if (fsync(storage->dir_fd) != 0)
      sync_error = 1;
   return sync_error ? KB_MANAGEMENT_STORAGE_UNAVAILABLE : KB_MANAGEMENT_STORAGE_OK;
}

kb_management_cert_storage_result_t
kb_management_cert_storage_pending_read(kb_management_cert_storage_t *storage, uint8_t *out,
                                        size_t cap, size_t *out_len)
{
   return read_name(storage, "pending", out, cap, out_len);
}

kb_management_cert_storage_result_t
kb_management_cert_storage_pending_clear_exact(kb_management_cert_storage_t *storage,
                                               const void *bytes, size_t len)
{
   if (!storage || storage->dir_fd < 0 || !bytes || !len || len > 1024 ||
       !valid_pending_record(bytes, len))
      return KB_MANAGEMENT_STORAGE_INTEGRITY;
   int fd = openat(storage->dir_fd, "pending", O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
   if (fd < 0)
      return open_error(errno, 1);
   if (!checked_file(fd))
   {
      close(fd);
      return KB_MANAGEMENT_STORAGE_INTEGRITY;
   }
   struct stat opened, named;
   if (fstat(fd, &opened) != 0)
   {
      close(fd);
      return KB_MANAGEMENT_STORAGE_UNAVAILABLE;
   }
   uint8_t existing[1024];
   size_t existing_len = 0;
   int read_rc = read_small_contents(fd, existing, &existing_len);
   if (read_rc < 0)
   {
      close(fd);
      return KB_MANAGEMENT_STORAGE_UNAVAILABLE;
   }
   if (read_rc > 0 || !valid_pending_record(existing, existing_len))
   {
      OPENSSL_cleanse(existing, sizeof(existing));
      close(fd);
      return KB_MANAGEMENT_STORAGE_INTEGRITY;
   }
   int exact = existing_len == len && CRYPTO_memcmp(existing, bytes, len) == 0;
   OPENSSL_cleanse(existing, sizeof(existing));
   if (!exact)
   {
      close(fd);
      return KB_MANAGEMENT_STORAGE_CONFLICT;
   }
   int named_ok = fstatat(storage->dir_fd, "pending", &named, AT_SYMLINK_NOFOLLOW) == 0 &&
                  S_ISREG(named.st_mode) &&
#ifdef AIMEE_MANAGEMENT_CERT_TESTING
                  named.st_uid == geteuid() &&
#else
                  named.st_uid == 0 &&
#endif
                  (named.st_mode & 0777) == 0600 && named.st_nlink == 1 &&
                  opened.st_dev == named.st_dev && opened.st_ino == named.st_ino;
   if (close(fd) != 0)
      return KB_MANAGEMENT_STORAGE_UNAVAILABLE;
   if (!named_ok)
      return KB_MANAGEMENT_STORAGE_INTEGRITY;
   if (unlinkat(storage->dir_fd, "pending", 0) != 0 || fsync(storage->dir_fd) != 0)
      return KB_MANAGEMENT_STORAGE_UNAVAILABLE;
   return KB_MANAGEMENT_STORAGE_OK;
}

/* cleanup is a fixed canonical binary recovery coordinate. It is deliberately
 * private to storage so lifecycle callers cannot select deletion targets. */
#define CLEANUP_MAGIC      "AMCLN001"
#define CLEANUP_TARGETS    3
#define CLEANUP_RECORD_LEN (8 + 4 + 64 + 32 + CLEANUP_TARGETS * (1 + 64 + 32))
enum
{
   CLEANUP_PROMOTION = 1,
   CLEANUP_TERMINAL = 2,
   CLEANUP_PREPARING = 3,
   CLEANUP_INTENT = 1,
   CLEANUP_CANDIDATE = 2
};
typedef struct
{
   uint8_t mode;
   uint8_t anchor_present;
   uint8_t target_count;
   char anchor[65];
   uint8_t anchor_digest[32];
   struct
   {
      uint8_t kind;
      char operation[65];
      uint8_t digest[32];
   } target[CLEANUP_TARGETS];
} cleanup_record_t;

static int cleanup_encode(const cleanup_record_t *record, uint8_t out[CLEANUP_RECORD_LEN])
{
   if (!record ||
       (record->mode != CLEANUP_PROMOTION && record->mode != CLEANUP_TERMINAL &&
        record->mode != CLEANUP_PREPARING) ||
       record->anchor_present > 1 || record->target_count < 1 ||
       record->target_count > CLEANUP_TARGETS ||
       (record->anchor_present ? !exact_hex(record->anchor, 64) : record->anchor[0]))
      return -1;
   memset(out, 0, CLEANUP_RECORD_LEN);
   memcpy(out, CLEANUP_MAGIC, 8);
   out[8] = record->mode;
   out[9] = record->anchor_present;
   out[10] = record->target_count;
   memcpy(out + 12, record->anchor, record->anchor_present ? 64 : 0);
   if (record->anchor_present)
      memcpy(out + 76, record->anchor_digest, 32);
   size_t offset = 108;
   for (unsigned i = 0; i < CLEANUP_TARGETS; ++i, offset += 97)
   {
      if (i >= record->target_count)
         continue;
      if ((record->target[i].kind != CLEANUP_INTENT &&
           record->target[i].kind != CLEANUP_CANDIDATE) ||
          !exact_hex(record->target[i].operation, 64))
         return -1;
      out[offset] = record->target[i].kind;
      memcpy(out + offset + 1, record->target[i].operation, 64);
      memcpy(out + offset + 65, record->target[i].digest, 32);
   }
   return 0;
}

static int cleanup_decode(const void *bytes, size_t len, cleanup_record_t *out)
{
   if (!bytes || len != CLEANUP_RECORD_LEN || !out || memcmp(bytes, CLEANUP_MAGIC, 8))
      return -1;
   const uint8_t *in = bytes;
   if (in[11] ||
       (in[8] != CLEANUP_PROMOTION && in[8] != CLEANUP_TERMINAL && in[8] != CLEANUP_PREPARING) ||
       in[9] > 1 || in[10] < 1 || in[10] > CLEANUP_TARGETS)
      return -1;
   memset(out, 0, sizeof(*out));
   out->mode = in[8];
   out->anchor_present = in[9];
   out->target_count = in[10];
   if (out->anchor_present)
   {
      memcpy(out->anchor, in + 12, 64);
      out->anchor[64] = 0;
      if (!exact_hex(out->anchor, 64))
         return -1;
   }
   else
      for (size_t i = 12; i < 108; ++i)
         if (in[i])
            return -1;
   if (out->anchor_present)
      memcpy(out->anchor_digest, in + 76, 32);
   size_t offset = 108;
   for (unsigned i = 0; i < CLEANUP_TARGETS; ++i, offset += 97)
   {
      if (i >= out->target_count)
      {
         for (size_t j = offset; j < offset + 97; ++j)
            if (in[j])
               return -1;
         continue;
      }
      out->target[i].kind = in[offset];
      memcpy(out->target[i].operation, in + offset + 1, 64);
      out->target[i].operation[64] = 0;
      memcpy(out->target[i].digest, in + offset + 65, 32);
      if ((out->target[i].kind != CLEANUP_INTENT && out->target[i].kind != CLEANUP_CANDIDATE) ||
          !exact_hex(out->target[i].operation, 64))
         return -1;
   }
   uint8_t canonical[CLEANUP_RECORD_LEN];
   int valid = cleanup_encode(out, canonical) == 0 &&
               CRYPTO_memcmp(canonical, bytes, sizeof(canonical)) == 0;
   OPENSSL_cleanse(canonical, sizeof(canonical));
   return valid ? 0 : -1;
}

static kb_management_cert_storage_result_t
read_manifest_name(kb_management_cert_storage_t *storage, const char *name,
                   kb_management_cert_manifest_t *manifest, int *present, uint8_t digest[32])
{
   uint8_t bytes[1024];
   size_t len = 0;
   *present = 0;
   kb_management_cert_storage_result_t rc = read_name(storage, name, bytes, sizeof(bytes), &len);
   if (rc == KB_MANAGEMENT_STORAGE_MISSING)
      return rc;
   if (rc != KB_MANAGEMENT_STORAGE_OK)
      return rc;
   if (kb_management_cert_manifest_decode(bytes, len, manifest))
      rc = KB_MANAGEMENT_STORAGE_INTEGRITY;
   else if (digest)
   {
      unsigned digest_len = 0;
      if (EVP_Digest(bytes, len, digest, &digest_len, EVP_sha256(), NULL) != 1 || digest_len != 32)
         rc = KB_MANAGEMENT_STORAGE_UNAVAILABLE;
      else
         *present = 1;
   }
   else
      *present = 1;
   OPENSSL_cleanse(bytes, sizeof(bytes));
   return rc;
}

static kb_management_cert_storage_result_t
cleanup_target_from_disk(kb_management_cert_storage_t *storage, cleanup_record_t *cleanup,
                         uint8_t kind, const char operation[65], int optional)
{
   if (cleanup->target_count >= CLEANUP_TARGETS)
      return KB_MANAGEMENT_STORAGE_INTEGRITY;
   uint8_t bytes[KB_MANAGEMENT_CERT_CANDIDATE_MAX];
   size_t len = 0;
   const char *kind_name = kind == CLEANUP_INTENT ? "intent" : "candidate";
   kb_management_cert_storage_result_t rc =
       kb_management_cert_storage_read(storage, kind_name, operation, bytes, sizeof(bytes), &len);
   if (rc == KB_MANAGEMENT_STORAGE_MISSING && optional)
      return KB_MANAGEMENT_STORAGE_OK;
   if (rc != KB_MANAGEMENT_STORAGE_OK)
      return rc == KB_MANAGEMENT_STORAGE_MISSING ? KB_MANAGEMENT_STORAGE_INTEGRITY : rc;
   unsigned digest_len = 0;
   cleanup->target[cleanup->target_count].kind = kind;
   memcpy(cleanup->target[cleanup->target_count].operation, operation, 65);
   if (EVP_Digest(bytes, len, cleanup->target[cleanup->target_count].digest, &digest_len,
                  EVP_sha256(), NULL) != 1 ||
       digest_len != 32)
      rc = KB_MANAGEMENT_STORAGE_UNAVAILABLE;
   else
      cleanup->target_count++;
   OPENSSL_cleanse(bytes, sizeof(bytes));
   return rc;
}

static kb_management_cert_storage_result_t cleanup_existing(kb_management_cert_storage_t *storage,
                                                            uint8_t bytes[CLEANUP_RECORD_LEN],
                                                            cleanup_record_t *out)
{
   size_t len = 0;
   kb_management_cert_storage_result_t rc =
       read_name(storage, "cleanup", bytes, CLEANUP_RECORD_LEN, &len);
   if (rc == KB_MANAGEMENT_STORAGE_OK && cleanup_decode(bytes, len, out))
      rc = KB_MANAGEMENT_STORAGE_INTEGRITY;
   return rc;
}

static kb_management_cert_storage_result_t cleanup_publish(kb_management_cert_storage_t *storage,
                                                           const cleanup_record_t *cleanup)
{
   uint8_t encoded[CLEANUP_RECORD_LEN], existing[CLEANUP_RECORD_LEN];
   cleanup_record_t decoded;
   if (cleanup_encode(cleanup, encoded))
      return KB_MANAGEMENT_STORAGE_INTEGRITY;
   kb_management_cert_storage_result_t existing_rc = cleanup_existing(storage, existing, &decoded);
   if (existing_rc == KB_MANAGEMENT_STORAGE_OK)
   {
      int exact = CRYPTO_memcmp(existing, encoded, sizeof(encoded)) == 0;
      OPENSSL_cleanse(existing, sizeof(existing));
      OPENSSL_cleanse(&decoded, sizeof(decoded));
      OPENSSL_cleanse(encoded, sizeof(encoded));
      return exact ? KB_MANAGEMENT_STORAGE_OK : KB_MANAGEMENT_STORAGE_CONFLICT;
   }
   if (existing_rc != KB_MANAGEMENT_STORAGE_MISSING)
   {
      OPENSSL_cleanse(encoded, sizeof(encoded));
      return existing_rc;
   }
   int fd =
       openat(storage->dir_fd, "cleanup", O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
   if (fd < 0)
   {
      OPENSSL_cleanse(encoded, sizeof(encoded));
      return errno == EEXIST ? KB_MANAGEMENT_STORAGE_CONFLICT : open_error(errno, 0);
   }
   kb_management_cert_storage_result_t rc = KB_MANAGEMENT_STORAGE_OK;
   if (!checked_file(fd) || complete_write(fd, encoded, sizeof(encoded)) || fdatasync(fd) ||
       verify_contents(fd, encoded, sizeof(encoded)))
      rc = KB_MANAGEMENT_STORAGE_UNAVAILABLE;
   if (close(fd) != 0 || (rc == KB_MANAGEMENT_STORAGE_OK && fsync(storage->dir_fd) != 0))
      rc = KB_MANAGEMENT_STORAGE_UNAVAILABLE;
   if (rc != KB_MANAGEMENT_STORAGE_OK && unlinkat(storage->dir_fd, "cleanup", 0) == 0)
      (void)fsync(storage->dir_fd);
   OPENSSL_cleanse(encoded, sizeof(encoded));
   return rc;
}

static int cleanup_target_matches_disk(kb_management_cert_storage_t *storage,
                                       const cleanup_record_t *cleanup, unsigned index)
{
   cleanup_record_t derived = {0};
   kb_management_cert_storage_result_t rc = cleanup_target_from_disk(
       storage, &derived, cleanup->target[index].kind, cleanup->target[index].operation, 0);
   int matches = rc == KB_MANAGEMENT_STORAGE_OK && derived.target_count == 1 &&
                 derived.target[0].kind == cleanup->target[index].kind &&
                 !strcmp(derived.target[0].operation, cleanup->target[index].operation) &&
                 CRYPTO_memcmp(derived.target[0].digest, cleanup->target[index].digest, 32) == 0;
   OPENSSL_cleanse(&derived, sizeof(derived));
   return matches;
}

kb_management_cert_storage_result_t
kb_management_cert_storage_cleanup_prepare_promotion(kb_management_cert_storage_t *storage)
{
   if (!storage || storage->dir_fd < 0)
      return KB_MANAGEMENT_STORAGE_INTEGRITY;
   uint8_t pending_bytes[1024];
   size_t pending_len = 0;
   kb_management_cert_pending_manifest_t pending;
   kb_management_cert_storage_result_t rc = kb_management_cert_storage_pending_read(
       storage, pending_bytes, sizeof(pending_bytes), &pending_len);
   if (rc != KB_MANAGEMENT_STORAGE_OK ||
       kb_management_cert_pending_decode(pending_bytes, pending_len, &pending))
   {
      OPENSSL_cleanse(pending_bytes, sizeof(pending_bytes));
      return rc == KB_MANAGEMENT_STORAGE_OK ? KB_MANAGEMENT_STORAGE_INTEGRITY : rc;
   }
   cleanup_record_t cleanup = {.mode = CLEANUP_PROMOTION, .anchor_present = 1};
   memcpy(cleanup.anchor, pending.operation_id, 65);
   /* The replacement candidate must exist but is categorically not a target. */
   uint8_t candidate[KB_MANAGEMENT_CERT_CANDIDATE_MAX];
   size_t candidate_len = 0;
   rc = kb_management_cert_storage_read(storage, "candidate", pending.operation_id, candidate,
                                        sizeof(candidate), &candidate_len);
   if (rc != KB_MANAGEMENT_STORAGE_OK)
      goto done_prepare;
   kb_management_cert_candidate_view_t replacement;
   kb_management_cert_manifest_t future = {.generation = pending.generation};
   uint8_t future_bytes[1024];
   size_t future_len = 0;
   unsigned digest_len = 0;
   if (kb_management_cert_candidate_decode(candidate, candidate_len, &replacement) ||
       strcmp(replacement.operation_id, pending.operation_id) ||
       replacement.generation != pending.generation)
   {
      rc = KB_MANAGEMENT_STORAGE_INTEGRITY;
      goto done_prepare;
   }
   memcpy(future.operation_id, pending.operation_id, 65);
   memcpy(future.public_bundle_digest, replacement.public_bundle_digest, 32);
   if (kb_management_cert_manifest_encode(&future, future_bytes, sizeof(future_bytes),
                                          &future_len) ||
       EVP_Digest(future_bytes, future_len, cleanup.anchor_digest, &digest_len, EVP_sha256(),
                  NULL) != 1 ||
       digest_len != 32)
   {
      rc = KB_MANAGEMENT_STORAGE_UNAVAILABLE;
      goto done_prepare;
   }
   kb_management_cert_manifest_t current = {0};
   int current_present = 0;
   rc = read_manifest_name(storage, "current", &current, &current_present, NULL);
   if (rc == KB_MANAGEMENT_STORAGE_MISSING)
      rc = KB_MANAGEMENT_STORAGE_OK;
   if (rc != KB_MANAGEMENT_STORAGE_OK)
      goto done_prepare;
   uint8_t existing[CLEANUP_RECORD_LEN];
   cleanup_record_t existing_cleanup;
   kb_management_cert_storage_result_t existing_rc =
       cleanup_existing(storage, existing, &existing_cleanup);
   if (existing_rc == KB_MANAGEMENT_STORAGE_OK && current_present &&
       !strcmp(current.operation_id, pending.operation_id))
   {
      int replay =
          existing_cleanup.mode == CLEANUP_PROMOTION && existing_cleanup.anchor_present &&
          !strcmp(existing_cleanup.anchor, pending.operation_id) &&
          CRYPTO_memcmp(existing_cleanup.anchor_digest, cleanup.anchor_digest, 32) == 0 &&
          (existing_cleanup.target_count == 1 || existing_cleanup.target_count == 2) &&
          existing_cleanup.target[existing_cleanup.target_count - 1].kind == CLEANUP_INTENT &&
          !strcmp(existing_cleanup.target[existing_cleanup.target_count - 1].operation,
                  pending.operation_id);
      for (unsigned i = 0; replay && i < existing_cleanup.target_count; ++i)
         replay = cleanup_target_matches_disk(storage, &existing_cleanup, i);
      if (replay && existing_cleanup.target_count == 2)
      {
         kb_management_cert_candidate_view_t predecessor;
         uint8_t predecessor_bytes[KB_MANAGEMENT_CERT_CANDIDATE_MAX];
         size_t predecessor_len = 0;
         replay = existing_cleanup.target[0].kind == CLEANUP_CANDIDATE &&
                  strcmp(existing_cleanup.target[0].operation, pending.operation_id) &&
                  kb_management_cert_storage_read(
                      storage, "candidate", existing_cleanup.target[0].operation, predecessor_bytes,
                      sizeof(predecessor_bytes), &predecessor_len) == KB_MANAGEMENT_STORAGE_OK &&
                  kb_management_cert_candidate_decode(predecessor_bytes, predecessor_len,
                                                      &predecessor) == 0 &&
                  predecessor.generation != INT64_MAX &&
                  predecessor.generation + 1 == pending.generation &&
                  !strcmp(predecessor.installation_id, pending.installation_id) &&
                  !strcmp(predecessor.lineage_id, pending.lineage_id) &&
                  !strcmp(predecessor.authority_id, pending.authority_id) &&
                  CRYPTO_memcmp(predecessor.binding_digest, pending.binding_digest, 32) == 0;
         OPENSSL_cleanse(predecessor_bytes, sizeof(predecessor_bytes));
         OPENSSL_cleanse(&predecessor, sizeof(predecessor));
      }
      OPENSSL_cleanse(existing, sizeof(existing));
      OPENSSL_cleanse(&existing_cleanup, sizeof(existing_cleanup));
      rc = replay ? KB_MANAGEMENT_STORAGE_OK : KB_MANAGEMENT_STORAGE_INTEGRITY;
      goto done_prepare;
   }
   if (existing_rc != KB_MANAGEMENT_STORAGE_MISSING && existing_rc != KB_MANAGEMENT_STORAGE_OK)
   {
      rc = existing_rc;
      goto done_prepare;
   }
   if (current_present && strcmp(current.operation_id, pending.operation_id))
   {
      rc = cleanup_target_from_disk(storage, &cleanup, CLEANUP_CANDIDATE, current.operation_id, 0);
   }
   if (rc == KB_MANAGEMENT_STORAGE_OK)
      rc = cleanup_target_from_disk(storage, &cleanup, CLEANUP_INTENT, pending.operation_id, 0);
   if (rc == KB_MANAGEMENT_STORAGE_OK)
      rc = cleanup_publish(storage, &cleanup);
   OPENSSL_cleanse(&current, sizeof(current));
done_prepare:
   OPENSSL_cleanse(candidate, sizeof(candidate));
   OPENSSL_cleanse(&replacement, sizeof(replacement));
   OPENSSL_cleanse(&future, sizeof(future));
   OPENSSL_cleanse(future_bytes, sizeof(future_bytes));
   OPENSSL_cleanse(&cleanup, sizeof(cleanup));
   OPENSSL_cleanse(&pending, sizeof(pending));
   OPENSSL_cleanse(pending_bytes, sizeof(pending_bytes));
   return rc;
}

kb_management_cert_storage_result_t
kb_management_cert_storage_cleanup_prepare_terminal(kb_management_cert_storage_t *storage)
{
   if (!storage || storage->dir_fd < 0)
      return KB_MANAGEMENT_STORAGE_INTEGRITY;
   uint8_t pending_bytes[1024];
   size_t pending_len = 0;
   kb_management_cert_pending_manifest_t pending;
   kb_management_cert_storage_result_t rc = kb_management_cert_storage_pending_read(
       storage, pending_bytes, sizeof(pending_bytes), &pending_len);
   if (rc != KB_MANAGEMENT_STORAGE_OK ||
       kb_management_cert_pending_decode(pending_bytes, pending_len, &pending))
   {
      OPENSSL_cleanse(pending_bytes, sizeof(pending_bytes));
      return rc == KB_MANAGEMENT_STORAGE_OK ? KB_MANAGEMENT_STORAGE_INTEGRITY : rc;
   }
   cleanup_record_t cleanup = {.mode = CLEANUP_TERMINAL};
   kb_management_cert_manifest_t current = {0};
   int current_present = 0;
   rc = read_manifest_name(storage, "current", &current, &current_present, cleanup.anchor_digest);
   if (rc == KB_MANAGEMENT_STORAGE_MISSING)
      rc = KB_MANAGEMENT_STORAGE_OK;
   if (rc != KB_MANAGEMENT_STORAGE_OK)
      goto terminal_done;
   cleanup.anchor_present = current_present ? 1 : 0;
   if (current_present)
      memcpy(cleanup.anchor, current.operation_id, 65);
   if (current_present && !strcmp(current.operation_id, pending.operation_id))
   {
      rc = KB_MANAGEMENT_STORAGE_INTEGRITY;
      goto terminal_done;
   }
   rc = cleanup_target_from_disk(storage, &cleanup, CLEANUP_CANDIDATE, pending.operation_id, 1);
   if (rc == KB_MANAGEMENT_STORAGE_OK)
      rc = cleanup_target_from_disk(storage, &cleanup, CLEANUP_INTENT, pending.operation_id, 0);
   if (rc == KB_MANAGEMENT_STORAGE_OK)
      rc = cleanup_publish(storage, &cleanup);
terminal_done:
   OPENSSL_cleanse(&current, sizeof(current));
   OPENSSL_cleanse(&cleanup, sizeof(cleanup));
   OPENSSL_cleanse(&pending, sizeof(pending));
   OPENSSL_cleanse(pending_bytes, sizeof(pending_bytes));
   return rc;
}

kb_management_cert_storage_result_t
kb_management_cert_storage_cleanup_prepare_intent(kb_management_cert_storage_t *storage,
                                                  const void *pending_bytes, size_t pending_len)
{
   if (!storage || storage->dir_fd < 0 || !pending_bytes || !pending_len || pending_len > 1024)
      return KB_MANAGEMENT_STORAGE_INTEGRITY;
   kb_management_cert_pending_manifest_t pending;
   if (kb_management_cert_pending_decode(pending_bytes, pending_len, &pending))
      return KB_MANAGEMENT_STORAGE_INTEGRITY;
   cleanup_record_t cleanup = {.mode = CLEANUP_PREPARING, .target_count = 1};
   cleanup.target[0].kind = CLEANUP_INTENT;
   memcpy(cleanup.target[0].operation, pending.operation_id, 65);
   memcpy(cleanup.target[0].digest, pending.intent_record_digest, 32);
   kb_management_cert_manifest_t current = {0};
   int current_present = 0;
   kb_management_cert_storage_result_t rc =
       read_manifest_name(storage, "current", &current, &current_present, cleanup.anchor_digest);
   if (rc == KB_MANAGEMENT_STORAGE_MISSING)
      rc = KB_MANAGEMENT_STORAGE_OK;
   if (rc == KB_MANAGEMENT_STORAGE_OK)
   {
      cleanup.anchor_present = current_present ? 1 : 0;
      if (current_present)
         memcpy(cleanup.anchor, current.operation_id, 65);
      if (current_present && !strcmp(current.operation_id, pending.operation_id))
         rc = KB_MANAGEMENT_STORAGE_INTEGRITY;
   }
   if (rc == KB_MANAGEMENT_STORAGE_OK)
      rc = cleanup_publish(storage, &cleanup);
   OPENSSL_cleanse(&current, sizeof(current));
   OPENSSL_cleanse(&cleanup, sizeof(cleanup));
   OPENSSL_cleanse(&pending, sizeof(pending));
   return rc;
}

static kb_management_cert_storage_result_t
cleanup_clear_exact(kb_management_cert_storage_t *storage,
                    const uint8_t encoded[CLEANUP_RECORD_LEN])
{
   int fd = openat(storage->dir_fd, "cleanup", O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
   if (fd < 0)
      return open_error(errno, 1);
   struct stat opened, named;
   uint8_t verify[1024];
   size_t verify_len = 0;
   int exact = checked_file(fd) && fstat(fd, &opened) == 0 &&
               read_small_contents(fd, verify, &verify_len) == 0 &&
               verify_len == CLEANUP_RECORD_LEN &&
               CRYPTO_memcmp(verify, encoded, CLEANUP_RECORD_LEN) == 0 &&
               fstatat(storage->dir_fd, "cleanup", &named, AT_SYMLINK_NOFOLLOW) == 0 &&
               opened.st_dev == named.st_dev && opened.st_ino == named.st_ino;
   OPENSSL_cleanse(verify, sizeof(verify));
   if (close(fd) != 0)
      return KB_MANAGEMENT_STORAGE_UNAVAILABLE;
   if (!exact)
      return KB_MANAGEMENT_STORAGE_INTEGRITY;
   if (unlinkat(storage->dir_fd, "cleanup", 0) != 0 || fsync(storage->dir_fd) != 0)
      return KB_MANAGEMENT_STORAGE_UNAVAILABLE;
   return KB_MANAGEMENT_STORAGE_OK;
}

kb_management_cert_storage_result_t
kb_management_cert_storage_cleanup_finish_intent(kb_management_cert_storage_t *storage,
                                                 const void *pending_bytes, size_t pending_len)
{
   if (!storage || storage->dir_fd < 0 || !pending_bytes || !pending_len || pending_len > 1024)
      return KB_MANAGEMENT_STORAGE_INTEGRITY;
   kb_management_cert_pending_manifest_t pending;
   if (kb_management_cert_pending_decode(pending_bytes, pending_len, &pending))
      return KB_MANAGEMENT_STORAGE_INTEGRITY;
   uint8_t encoded[CLEANUP_RECORD_LEN];
   cleanup_record_t cleanup;
   kb_management_cert_storage_result_t rc = cleanup_existing(storage, encoded, &cleanup);
   if (rc == KB_MANAGEMENT_STORAGE_OK && cleanup.mode != CLEANUP_PREPARING)
      rc = KB_MANAGEMENT_STORAGE_MISSING;
   if (rc == KB_MANAGEMENT_STORAGE_OK &&
       (cleanup.target_count != 1 || cleanup.target[0].kind != CLEANUP_INTENT ||
        strcmp(cleanup.target[0].operation, pending.operation_id) ||
        CRYPTO_memcmp(cleanup.target[0].digest, pending.intent_record_digest, 32)))
      rc = KB_MANAGEMENT_STORAGE_INTEGRITY;
   if (rc == KB_MANAGEMENT_STORAGE_OK)
      rc = cleanup_clear_exact(storage, encoded);
   OPENSSL_cleanse(encoded, sizeof(encoded));
   OPENSSL_cleanse(&cleanup, sizeof(cleanup));
   OPENSSL_cleanse(&pending, sizeof(pending));
   return rc;
}

static kb_management_cert_storage_result_t
cleanup_discard_target(kb_management_cert_storage_t *storage, const cleanup_record_t *cleanup,
                       unsigned index)
{
   const char *kind = cleanup->target[index].kind == CLEANUP_INTENT ? "intent" : "candidate";
   char name[80];
   if (record_name(kind, cleanup->target[index].operation, name))
      return KB_MANAGEMENT_STORAGE_INTEGRITY;
   int fd = openat(storage->dir_fd, name, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
   if (fd < 0)
      return errno == ENOENT ? KB_MANAGEMENT_STORAGE_OK : open_error(errno, 0);
   if (!checked_file(fd))
   {
      close(fd);
      return KB_MANAGEMENT_STORAGE_INTEGRITY;
   }
   struct stat opened, named;
   uint8_t bytes[KB_MANAGEMENT_CERT_CANDIDATE_MAX];
   size_t len = 0;
   int read_rc = read_small_contents(fd, bytes, &len);
   /* Operation records can exceed pending's 1KiB helper. Read the full record. */
   if (read_rc == 1)
   {
      OPENSSL_cleanse(bytes, sizeof(bytes));
      if (lseek(fd, 0, SEEK_SET) < 0)
         read_rc = -1;
      else
      {
         len = 0;
         read_rc = 0;
         while (len < sizeof(bytes))
         {
            ssize_t n = read(fd, bytes + len, sizeof(bytes) - len);
            if (n > 0)
               len += (size_t)n;
            else if (!n)
               break;
            else if (errno != EINTR)
            {
               read_rc = -1;
               break;
            }
         }
         if (read_rc != -1 && len == sizeof(bytes))
         {
            uint8_t extra;
            ssize_t more;
            do
               more = read(fd, &extra, 1);
            while (more < 0 && errno == EINTR);
            read_rc = more > 0 ? 1 : more < 0 ? -1 : 0;
         }
      }
   }
   unsigned digest_len = 0;
   uint8_t digest[32];
   int exact =
       read_rc == 0 && len &&
       valid_operation_record(kind, cleanup->target[index].operation, bytes, len) &&
       EVP_Digest(bytes, len, digest, &digest_len, EVP_sha256(), NULL) == 1 && digest_len == 32 &&
       CRYPTO_memcmp(digest, cleanup->target[index].digest, 32) == 0 && fstat(fd, &opened) == 0 &&
       fstatat(storage->dir_fd, name, &named, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(named.st_mode) &&
       opened.st_dev == named.st_dev && opened.st_ino == named.st_ino;
   OPENSSL_cleanse(bytes, sizeof(bytes));
   OPENSSL_cleanse(digest, sizeof(digest));
   if (close(fd) != 0)
      return KB_MANAGEMENT_STORAGE_UNAVAILABLE;
   if (!exact)
      return read_rc < 0 ? KB_MANAGEMENT_STORAGE_UNAVAILABLE : KB_MANAGEMENT_STORAGE_INTEGRITY;
   if (unlinkat(storage->dir_fd, name, 0) != 0 || fsync(storage->dir_fd) != 0)
      return KB_MANAGEMENT_STORAGE_UNAVAILABLE;
   return KB_MANAGEMENT_STORAGE_OK;
}

kb_management_cert_storage_result_t
kb_management_cert_storage_cleanup_apply(kb_management_cert_storage_t *storage)
{
   if (!storage || storage->dir_fd < 0)
      return KB_MANAGEMENT_STORAGE_INTEGRITY;
   uint8_t encoded[CLEANUP_RECORD_LEN];
   cleanup_record_t cleanup;
   kb_management_cert_storage_result_t rc = cleanup_existing(storage, encoded, &cleanup);
   if (rc != KB_MANAGEMENT_STORAGE_OK)
      return rc;
   uint8_t pending[1024];
   size_t pending_len = 0;
   rc = kb_management_cert_storage_pending_read(storage, pending, sizeof(pending), &pending_len);
   OPENSSL_cleanse(pending, sizeof(pending));
   if (rc != KB_MANAGEMENT_STORAGE_MISSING)
      return rc == KB_MANAGEMENT_STORAGE_OK ? KB_MANAGEMENT_STORAGE_CONFLICT : rc;
   kb_management_cert_manifest_t current = {0};
   int current_present = 0;
   uint8_t current_digest[32];
   rc = read_manifest_name(storage, "current", &current, &current_present, current_digest);
   if (rc == KB_MANAGEMENT_STORAGE_MISSING)
      rc = KB_MANAGEMENT_STORAGE_OK;
   if (rc != KB_MANAGEMENT_STORAGE_OK || current_present != cleanup.anchor_present ||
       (current_present && (strcmp(current.operation_id, cleanup.anchor) ||
                            CRYPTO_memcmp(current_digest, cleanup.anchor_digest, 32))))
   {
      OPENSSL_cleanse(&current, sizeof(current));
      return rc == KB_MANAGEMENT_STORAGE_OK ? KB_MANAGEMENT_STORAGE_INTEGRITY : rc;
   }
   OPENSSL_cleanse(&current, sizeof(current));
   OPENSSL_cleanse(current_digest, sizeof(current_digest));
   for (unsigned i = 0; i < cleanup.target_count; ++i)
   {
      if (cleanup.anchor_present && cleanup.target[i].kind == CLEANUP_CANDIDATE &&
          !strcmp(cleanup.target[i].operation, cleanup.anchor))
         return KB_MANAGEMENT_STORAGE_INTEGRITY;
      rc = cleanup_discard_target(storage, &cleanup, i);
      if (rc != KB_MANAGEMENT_STORAGE_OK)
         return rc;
   }
   rc = cleanup_clear_exact(storage, encoded);
   OPENSSL_cleanse(encoded, sizeof(encoded));
   OPENSSL_cleanse(&cleanup, sizeof(cleanup));
   return rc;
}
