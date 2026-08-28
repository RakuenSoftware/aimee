#include "kb_mgmt_offline_hardening.h"

#include <ctype.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/magic.h>
#endif

#define SWAPS_TEXT_MAX 8192

static int bounded_contains(const char *text, size_t len, const char *needle, size_t needle_len)
{
   if (!text || !needle || needle_len > len)
      return 0;
   for (size_t i = 0; i <= len - needle_len; i++)
      if (memcmp(text + i, needle, needle_len) == 0)
         return 1;
   return 0;
}

int kb_mgmt_offline_swaps_text_active(const char *text, size_t len)
{
   if (!text || !len || len > SWAPS_TEXT_MAX)
      return -1;
   const char *newline = memchr(text, '\n', len);
   if (!newline)
      return -1;
   size_t header_len = (size_t)(newline - text);
   if (header_len < 8 || memcmp(text, "Filename", 8) != 0 ||
       !bounded_contains(text, header_len, "Type", 4) ||
       !bounded_contains(text, header_len, "Size", 4) ||
       !bounded_contains(text, header_len, "Used", 4) ||
       !bounded_contains(text, header_len, "Priority", 8))
      return -1;
   for (size_t i = header_len + 1; i < len; i++)
      if (!isspace((unsigned char)text[i]))
         return 1;
   return 0;
}

int kb_mgmt_offline_cgroup_swap_text_disabled(const char *text, size_t len)
{
   if (!text || !len || text[0] != '0')
      return 0;
   for (size_t i = 1; i < len; i++)
      if (!isspace((unsigned char)text[i]))
         return 0;
   return 1;
}

static int cgroup_has_swap_disabled(void)
{
   int fd = open("/sys/fs/cgroup/memory.swap.max", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
   if (fd < 0)
      return 0;
#ifdef CGROUP2_SUPER_MAGIC
   struct statfs fs;
   if (fstatfs(fd, &fs) != 0 || (unsigned long)fs.f_type != (unsigned long)CGROUP2_SUPER_MAGIC)
   {
      close(fd);
      return 0;
   }
#endif
   char text[32], extra;
   ssize_t got = read(fd, text, sizeof(text));
   ssize_t tail = got == (ssize_t)sizeof(text) ? read(fd, &extra, 1) : 0;
   int close_rc = close(fd);
   return got > 0 && tail == 0 && close_rc == 0 &&
          kb_mgmt_offline_cgroup_swap_text_disabled(text, (size_t)got);
}

static int kernel_has_no_active_swap(void)
{
   int fd = open("/proc/swaps", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
   if (fd < 0)
      return 0;
#ifdef PROC_SUPER_MAGIC
   struct statfs fs;
   if (fstatfs(fd, &fs) != 0 || (unsigned long)fs.f_type != (unsigned long)PROC_SUPER_MAGIC)
   {
      close(fd);
      return 0;
   }
#endif
   char text[SWAPS_TEXT_MAX];
   size_t used = 0;
   while (used < sizeof(text))
   {
      ssize_t got = read(fd, text + used, sizeof(text) - used);
      if (got < 0)
      {
         close(fd);
         return 0;
      }
      if (got == 0)
         break;
      used += (size_t)got;
   }
   char extra;
   ssize_t tail = used == sizeof(text) ? read(fd, &extra, 1) : 0;
   int close_rc = close(fd);
   return tail == 0 && close_rc == 0 && kb_mgmt_offline_swaps_text_active(text, used) == 0;
}

static int explicit_no_swap_fallback(void)
{
   const char *enabled = getenv("AIMEE_OFFLINE_ALLOW_NO_SWAP_MLOCK_FALLBACK");
   return enabled && strcmp(enabled, "1") == 0 &&
          (kernel_has_no_active_swap() || cgroup_has_swap_disabled());
}

const char *kb_mgmt_offline_harden_process(void)
{
   /* mlockall is the preferred invariant. A stock unprivileged LXC cannot
    * raise its 8 MiB hard limit, which is smaller than libpq + OpenSSL. The
    * managed installer opts into an equivalent fail-closed no-swap invariant;
    * operator-run provisioners retain mandatory mlockall by default. */
   struct rlimit no_core = {0, 0};
   (void)umask(077);
   if (setrlimit(RLIMIT_CORE, &no_core) != 0)
      return "hardening (core-dump limit)";
   if (prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) != 0)
      return "hardening (dumpable)";
   if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
      return "hardening (no-new-privs)";

   int memory_locked = mlockall(MCL_CURRENT | MCL_FUTURE) == 0;
   if (!memory_locked && !explicit_no_swap_fallback())
      return "hardening (mlockall; raise RLIMIT_MEMLOCK or disable swap for managed bootstrap)";
#ifdef PR_GET_DUMPABLE
   if (prctl(PR_GET_DUMPABLE, 0, 0, 0, 0) != 0)
   {
      if (memory_locked)
         (void)munlockall();
      return "hardening (dumpable readback)";
   }
#endif
   return NULL;
}
