#include "kb_vault_tpm_runtime_lock.h"
#include "kb_vault_tpm_runtime_lock_test.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

struct kb_vault_tpm_runtime_lock
{
   int fd;
   int dir_fd;
   uid_t expected_uid;
   dev_t device;
   ino_t inode;
   char name[32];
};

/* One TPM-custodied daemon owns one runtime identity.  The registry exists only
 * so the atfork child hook can close its inherited descriptors without making
 * either descriptor visible outside this module. */
static pthread_mutex_t g_owner_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t g_atfork_once = PTHREAD_ONCE_INIT;
static int g_atfork_status = -1;
static kb_vault_tpm_runtime_lock_t *g_current_owner;

void kb_vault_tpm_runtime_identity(const char *configured_tcti,
                                   const char *configured_nv_index,
                                   const char **effective_tcti,
                                   const char **effective_nv_index)
{
   const char *env_tcti = getenv("AIMEE_VAULT_TPM2_TCTI");
   const char *env_nv_index = getenv("AIMEE_VAULT_TPM2_NV_INDEX");
   if (effective_tcti)
      *effective_tcti = env_tcti && env_tcti[0] ? env_tcti : configured_tcti;
   if (effective_nv_index)
      *effective_nv_index =
          env_nv_index && env_nv_index[0] ? env_nv_index : configured_nv_index;
}

static void close_child_descriptors(kb_vault_tpm_runtime_lock_t *owner)
{
   if (!owner)
      return;
   if (owner->fd >= 0)
      (void)close(owner->fd);
   if (owner->dir_fd >= 0)
      (void)close(owner->dir_fd);
   owner->fd = -1;
   owner->dir_fd = -1;
}

static void owner_atfork_prepare(void)
{
   (void)pthread_mutex_lock(&g_owner_mutex);
}

static void owner_atfork_parent(void)
{
   /* Fork is the common helper-launch boundary. Revalidate before fork returns
    * to any parent call site; a lost singleton must never resume serving. */
   if (g_current_owner &&
       kb_vault_tpm_runtime_lock_revalidate(g_current_owner) != KB_VAULT_TPM_RUNTIME_LOCK_OK)
      _exit(1);
   (void)pthread_mutex_unlock(&g_owner_mutex);
}

static void owner_atfork_child(void)
{
   close_child_descriptors(g_current_owner);
   g_current_owner = NULL;
   (void)pthread_mutex_unlock(&g_owner_mutex);
}

static void register_atfork(void)
{
   g_atfork_status = pthread_atfork(owner_atfork_prepare, owner_atfork_parent, owner_atfork_child);
}

static int ensure_atfork(void)
{
   return pthread_once(&g_atfork_once, register_atfork) == 0 && g_atfork_status == 0 ? 0 : -1;
}

static void set_error(char *errbuf, size_t errlen, const char *message)
{
   if (!errbuf || !errlen)
      return;
   (void)snprintf(errbuf, errlen, "%s", message ? message : "TPM runtime lock failure");
}

static int canonical_nv_index(const char *text, uint32_t *out)
{
   if (!text || strlen(text) != 10 || text[0] != '0' || text[1] != 'x')
      return -1;
   uint32_t value = 0;
   for (size_t i = 2; i < 10; i++)
   {
      unsigned digit;
      if (text[i] >= '0' && text[i] <= '9')
         digit = (unsigned)(text[i] - '0');
      else if (text[i] >= 'a' && text[i] <= 'f')
         digit = (unsigned)(text[i] - 'a') + 10U;
      else
         return -1;
      value = (value << 4) | digit;
   }
   /* TPMI_RH_NV_INDEX has TPM_HT_NV_INDEX (0x01) in its high byte. */
   if ((value & 0xff000000U) != 0x01000000U)
      return -1;
   char canonical[11];
   if (snprintf(canonical, sizeof(canonical), "0x%08x", value) != 10 ||
       strcmp(canonical, text) != 0)
      return -1;
   *out = value;
   return 0;
}

#if defined(AIMEE_P7_D3_INTEGRATION_TEST_OVERRIDE)
static int loopback_swtpm_tcti(const char *tcti)
{
   static const char prefix[] = "swtpm:host=127.0.0.1,port=";
   if (!tcti || strncmp(tcti, prefix, sizeof(prefix) - 1) != 0)
      return 0;
   const char *port = tcti + sizeof(prefix) - 1;
   if (!*port || (*port == '0' && port[1]))
      return 0;
   unsigned value = 0;
   for (const char *p = port; *p; p++)
   {
      if (*p < '0' || *p > '9')
         return 0;
      value = value * 10U + (unsigned)(*p - '0');
      if (value > 65535U)
         return 0;
   }
   return value != 0;
}
#endif

static int local_tcti_eligible(const char *tcti, int allow_loopback_swtpm)
{
   if (tcti && strcmp(tcti, "device:/dev/tpmrm0") == 0)
      return 1;
#if defined(AIMEE_P7_D3_INTEGRATION_TEST_OVERRIDE)
   return allow_loopback_swtpm && loopback_swtpm_tcti(tcti);
#else
   (void)allow_loopback_swtpm;
   return 0;
#endif
}

static int validate_dir_fd(int fd, uid_t uid, int exact_leaf)
{
   struct stat st;
   if (fd < 0 || fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != uid)
      return -1;
   mode_t mode = st.st_mode & 07777;
   return exact_leaf ? (mode == 0700 ? 0 : -1) : ((mode & 0022) == 0 ? 0 : -1);
}

#if defined(__linux__) && defined(WITH_TPM2)
static int open_component(int parent, const char *name, int exact_leaf)
{
   int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
   if (fd < 0 || validate_dir_fd(fd, 0, exact_leaf) != 0)
   {
      if (fd >= 0)
         close(fd);
      return -1;
   }
   return fd;
}

static int open_production_runtime_dir(void)
{
   int root = open("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
   if (root < 0 || validate_dir_fd(root, 0, 0) != 0)
   {
      if (root >= 0)
         close(root);
      return -1;
   }
   int run = open_component(root, "run", 0);
   close(root);
   if (run < 0)
      return -1;
   int aimee = open_component(run, "aimee", 0);
   close(run);
   if (aimee < 0)
      return -1;
   int locks = open_component(aimee, "vault-tpm2-locks", 1);
   close(aimee);
   return locks;
}
#endif

static kb_vault_tpm_runtime_lock_result_t acquire_at(
    int runtime_dir_fd, uid_t expected_uid, int build_supported, int allow_loopback_swtpm,
    const char *tcti, const char *nv_index, kb_vault_tpm_runtime_lock_t **out, char *errbuf,
    size_t errlen)
{
   if (out)
      *out = NULL;
   if (!out || runtime_dir_fd < 0 || !tcti || !nv_index)
   {
      set_error(errbuf, errlen, "TPM runtime lock: invalid arguments");
      return KB_VAULT_TPM_RUNTIME_LOCK_INVALID;
   }
   if (ensure_atfork() != 0)
   {
      set_error(errbuf, errlen, "TPM runtime lock: fork-safety registration failed");
      return KB_VAULT_TPM_RUNTIME_LOCK_IO;
   }
   if (!build_supported)
   {
      set_error(errbuf, errlen, "TPM runtime lock: local TPM2 build support unavailable");
      return KB_VAULT_TPM_RUNTIME_LOCK_UNSUPPORTED;
   }
   if (!local_tcti_eligible(tcti, allow_loopback_swtpm))
   {
      set_error(errbuf, errlen, "TPM runtime lock: non-local TPM resource manager refused");
      return KB_VAULT_TPM_RUNTIME_LOCK_INELIGIBLE;
   }
   uint32_t nv = 0;
   if (canonical_nv_index(nv_index, &nv) != 0)
   {
      set_error(errbuf, errlen, "TPM runtime lock: NV index is not canonical");
      return KB_VAULT_TPM_RUNTIME_LOCK_INELIGIBLE;
   }
   if (validate_dir_fd(runtime_dir_fd, expected_uid, 1) != 0)
   {
      set_error(errbuf, errlen, "TPM runtime lock: runtime directory owner or mode invalid");
      return KB_VAULT_TPM_RUNTIME_LOCK_IO;
   }

   kb_vault_tpm_runtime_lock_t *owner = calloc(1, sizeof(*owner));
   if (!owner)
      return KB_VAULT_TPM_RUNTIME_LOCK_IO;
   owner->fd = -1;
   owner->dir_fd = fcntl(runtime_dir_fd, F_DUPFD_CLOEXEC, 3);
   owner->expected_uid = expected_uid;
   if (owner->dir_fd < 0 ||
       snprintf(owner->name, sizeof(owner->name), "nv-%08x.lock", nv) != 16)
      goto io_fail;

   owner->fd = openat(owner->dir_fd, owner->name,
                      O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
   if (owner->fd < 0)
      goto io_fail;
   struct stat st;
   if (fstat(owner->fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != expected_uid ||
       (st.st_mode & 07777) != 0600 || st.st_nlink != 1)
      goto io_fail;
   owner->device = st.st_dev;
   owner->inode = st.st_ino;
   if (flock(owner->fd, LOCK_EX | LOCK_NB) != 0)
   {
      int lock_errno = errno;
      kb_vault_tpm_runtime_lock_release(&owner);
      set_error(errbuf, errlen, "TPM runtime lock: another runtime owns this NV index");
      return lock_errno == EWOULDBLOCK || lock_errno == EAGAIN
                 ? KB_VAULT_TPM_RUNTIME_LOCK_BUSY
                 : KB_VAULT_TPM_RUNTIME_LOCK_IO;
   }
   if (kb_vault_tpm_runtime_lock_revalidate(owner) != KB_VAULT_TPM_RUNTIME_LOCK_OK)
      goto io_fail;
   pthread_mutex_lock(&g_owner_mutex);
   if (g_current_owner)
   {
      pthread_mutex_unlock(&g_owner_mutex);
      set_error(errbuf, errlen, "TPM runtime lock: this process already owns a TPM runtime");
      goto invalid_fail;
   }
   g_current_owner = owner;
   pthread_mutex_unlock(&g_owner_mutex);
   *out = owner;
   return KB_VAULT_TPM_RUNTIME_LOCK_OK;

invalid_fail:
   if (owner->fd >= 0)
      close(owner->fd);
   if (owner->dir_fd >= 0)
      close(owner->dir_fd);
   free(owner);
   return KB_VAULT_TPM_RUNTIME_LOCK_INVALID;

io_fail:
   kb_vault_tpm_runtime_lock_release(&owner);
   set_error(errbuf, errlen, "TPM runtime lock: secure lock file acquisition failed");
   return KB_VAULT_TPM_RUNTIME_LOCK_IO;
}

kb_vault_tpm_runtime_lock_result_t
kb_vault_tpm_runtime_lock_acquire(const char *tcti, const char *nv_index,
                                  kb_vault_tpm_runtime_lock_t **out, char *errbuf, size_t errlen)
{
#if defined(__linux__) && defined(WITH_TPM2)
   int runtime_dir = open_production_runtime_dir();
   if (runtime_dir < 0)
   {
      if (out)
         *out = NULL;
      set_error(errbuf, errlen, "TPM runtime lock: fixed runtime directory unavailable");
      return KB_VAULT_TPM_RUNTIME_LOCK_IO;
   }
   kb_vault_tpm_runtime_lock_result_t result =
       acquire_at(runtime_dir, 0, 1, 1, tcti, nv_index, out, errbuf, errlen);
   close(runtime_dir);
   return result;
#else
   (void)tcti;
   (void)nv_index;
   if (out)
      *out = NULL;
   set_error(errbuf, errlen, "TPM runtime lock: local TPM2 build support unavailable");
   return KB_VAULT_TPM_RUNTIME_LOCK_UNSUPPORTED;
#endif
}

kb_vault_tpm_runtime_lock_result_t kb_vault_tpm_runtime_lock_acquire_at_for_test(
    int runtime_dir_fd, uid_t expected_uid, int build_supported, int allow_loopback_swtpm,
    const char *tcti, const char *nv_index, kb_vault_tpm_runtime_lock_t **out, char *errbuf,
    size_t errlen)
{
   return acquire_at(runtime_dir_fd, expected_uid, build_supported, allow_loopback_swtpm, tcti,
                     nv_index, out, errbuf, errlen);
}

kb_vault_tpm_runtime_lock_result_t
kb_vault_tpm_runtime_lock_revalidate(kb_vault_tpm_runtime_lock_t *owner)
{
   if (!owner || owner->fd < 0 || owner->dir_fd < 0)
      return KB_VAULT_TPM_RUNTIME_LOCK_LOST;
   if ((fcntl(owner->fd, F_GETFD) & FD_CLOEXEC) == 0 ||
       (fcntl(owner->fd, F_GETFL) & O_NONBLOCK) == 0)
      return KB_VAULT_TPM_RUNTIME_LOCK_LOST;
   struct stat held, named;
   if (fstat(owner->fd, &held) != 0 || !S_ISREG(held.st_mode) ||
       held.st_dev != owner->device || held.st_ino != owner->inode ||
       held.st_uid != owner->expected_uid || (held.st_mode & 07777) != 0600 ||
       held.st_nlink != 1)
      return KB_VAULT_TPM_RUNTIME_LOCK_LOST;
   int named_fd = openat(owner->dir_fd, owner->name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
   if (named_fd < 0)
      return KB_VAULT_TPM_RUNTIME_LOCK_LOST;
   int ok = fstat(named_fd, &named) == 0 && S_ISREG(named.st_mode) && named.st_dev == held.st_dev &&
            named.st_ino == held.st_ino && named.st_uid == owner->expected_uid &&
            (named.st_mode & 07777) == 0600 && named.st_nlink == 1;
   close(named_fd);
   if (!ok || flock(owner->fd, LOCK_EX | LOCK_NB) != 0)
      return KB_VAULT_TPM_RUNTIME_LOCK_LOST;
   return KB_VAULT_TPM_RUNTIME_LOCK_OK;
}

void kb_vault_tpm_runtime_lock_after_fork_child(kb_vault_tpm_runtime_lock_t *owner)
{
   close_child_descriptors(owner);
   if (g_current_owner == owner)
      g_current_owner = NULL;
}

void kb_vault_tpm_runtime_lock_release(kb_vault_tpm_runtime_lock_t **owner_ptr)
{
   if (!owner_ptr || !*owner_ptr)
      return;
   kb_vault_tpm_runtime_lock_t *owner = *owner_ptr;
   *owner_ptr = NULL;
   pthread_mutex_lock(&g_owner_mutex);
   if (g_current_owner == owner)
      g_current_owner = NULL;
   pthread_mutex_unlock(&g_owner_mutex);
   if (owner->fd >= 0)
      close(owner->fd);
   if (owner->dir_fd >= 0)
      close(owner->dir_fd);
   memset(owner, 0, sizeof(*owner));
   free(owner);
}
