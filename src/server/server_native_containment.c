#define _GNU_SOURCE
#include "server_native_containment.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __linux__
#include "aimee_sha256.h"
#include "platform_random.h"
#include <fcntl.h>
#include <inttypes.h>
#include <linux/capability.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <netinet/in.h>
#include <stddef.h>
#include <sys/socket.h>
#include <linux/magic.h>
#include <linux/securebits.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
struct server_native_containment
{
   int root, leaf, command[2], ready[2], used;
   long maxfd;
   uint16_t tcp_ports[8];
   size_t port_count;
   char uidmap[64], gidmap[64], name[64];
   struct stat root_stat, leaf_stat;
   cJSON *manifest;
   cli_native_spawn_guard_t guard;
};
static const char protocol[] = "user-mount-pid-cgroup-network-write-preexec-v3";
static const char *string(const cJSON *v, const char *key)
{
   return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(v, key));
}
static int text_at(int directory, const char *name, char *out, size_t capacity)
{
   int fd = openat(directory, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
   if (fd < 0)
      return -1;
   ssize_t n = read(fd, out, capacity - 1);
   char extra;
   int valid = n > 0 && read(fd, &extra, 1) == 0;
   close(fd);
   if (!valid)
      return -1;
   out[n] = 0;
   while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r'))
      out[--n] = 0;
   return 0;
}
static int write_at(int directory, const char *name, const char *text)
{
   int fd = openat(directory, name, O_WRONLY | O_NOFOLLOW | O_CLOEXEC);
   if (fd < 0)
      return -1;
   size_t n = strlen(text);
   ssize_t wrote;
   do
   {
      wrote = write(fd, text, n);
   } while (wrote < 0 && errno == EINTR);
   close(fd);
   return wrote == (ssize_t)n ? 0 : -1;
}
static int identity_text(const char *path, char out[128])
{
   return text_at(AT_FDCWD, path, out, 128);
}
static int namespace_identity(char out[128])
{
   struct stat s;
   if (stat("/proc/self/ns/mnt", &s))
      return -1;
   return snprintf(out, 128, "%" PRIuMAX ":%" PRIuMAX, (uintmax_t)s.st_dev, (uintmax_t)s.st_ino) >=
                  128
              ? -1
              : 0;
}
static int safe_mounts(void)
{
   FILE *f = fopen("/proc/self/mountinfo", "re");
   if (!f)
      return 0;
   char *line = NULL;
   size_t cap = 0;
   int cgroups = 0, proc = 0, valid = 1;
   while (getline(&line, &cap, f) > 0)
   {
      char root[4096], point[4096];
      char *type = strstr(line, " - ");
      if (sscanf(line, "%*u %*u %*s %4095s %4095s", root, point) != 2 || !type)
      {
         valid = 0;
         break;
      }
      if (!strncmp(type + 3, "cgroup2 ", 8))
      {
         cgroups++;
         if (strcmp(point, "/sys/fs/cgroup"))
            valid = 0;
      }
      if (!strncmp(type + 3, "proc ", 5) && !strcmp(root, "/"))
      {
         proc++;
         if (strcmp(point, "/proc"))
            valid = 0;
      }
   }
   free(line);
   fclose(f);
   return valid && cgroups == 1 && proc == 1;
}
static int open_root(const char *path)
{
   static const char prefix[] = "/sys/fs/cgroup/";
   if (!path || strncmp(path, prefix, sizeof(prefix) - 1) || strlen(path) >= 4096)
      return -1;
   int fd = open("/sys/fs/cgroup", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
   char copy[4096];
   snprintf(copy, sizeof copy, "%s", path + sizeof(prefix) - 1);
   char *save = NULL, *part = strtok_r(copy, "/", &save);
   while (fd >= 0 && part)
   {
      if (!strcmp(part, ".") || !strcmp(part, ".."))
      {
         close(fd);
         return -1;
      }
      int next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      close(fd);
      fd = next;
      part = strtok_r(NULL, "/", &save);
   }
   struct statfs fs;
   struct stat st;
   if (fd < 0)
      return -1;
   if (fstatfs(fd, &fs) || fs.f_type != CGROUP2_SUPER_MAGIC || fstat(fd, &st) ||
       st.st_uid != geteuid() || (st.st_mode & 0022))
   {
      close(fd);
      return -1;
   }
   return fd;
}
static int same_boundary(server_native_containment_t *o)
{
   struct stat root, leaf, linked;
   return o && o->root >= 0 && o->leaf >= 0 && !fstat(o->root, &root) && !fstat(o->leaf, &leaf) &&
          !fstatat(o->root, o->name, &linked, AT_SYMLINK_NOFOLLOW) && S_ISDIR(linked.st_mode) &&
          root.st_dev == o->root_stat.st_dev && root.st_ino == o->root_stat.st_ino &&
          leaf.st_dev == o->leaf_stat.st_dev && leaf.st_ino == o->leaf_stat.st_ino &&
          linked.st_dev == leaf.st_dev && linked.st_ino == leaf.st_ino;
}
static int byte_read(int fd, char expected)
{
   char c;
   ssize_t n;
   do
   {
      n = read(fd, &c, 1);
   } while (n < 0 && errno == EINTR);
   return n == 1 && c == expected ? 0 : -1;
}
static int byte_write(int fd, char c)
{
   /* No SIGPIPE in the daemon when a refused child exits. */
   sigset_t mask, previous, pending;
   sigemptyset(&mask);
   sigaddset(&mask, SIGPIPE);
   if (pthread_sigmask(SIG_BLOCK, &mask, &previous))
      return -1;
   sigpending(&pending);
   int old = sigismember(&pending, SIGPIPE);
   ssize_t n;
   do
   {
      n = write(fd, &c, 1);
   } while (n < 0 && errno == EINTR);
   if (n < 0 && errno == EPIPE && !old)
   {
      struct timespec zero = {0};
      sigtimedwait(&mask, NULL, &zero);
   }
   pthread_sigmask(SIG_SETMASK, &previous, NULL);
   return n == 1 ? 0 : -1;
}
static void close_other_fds(server_native_containment_t *o, int executable, int cwd)
{
   for (int fd = 3; fd < o->maxfd; fd++)
      if (fd != o->command[0] && fd != o->ready[1] && fd != executable && fd != cwd)
         close(fd);
}
/* The native materializer supplies a fresh n.XXXXXX HOME and one exact SDK
 * cache-state symlink. Only their interiors get write authority, never the
 * persistent binding/control parents. Caller credentials and cache locks remain
 * outside these rules even though the SDK has the same host filesystem UID. */
static int cache_write_directories(const char *cwd_path, int directories[2])
{
   char home[4096], link[4096], target[4096];
   if (strlen(cwd_path) >= sizeof home)
      return -1;
   snprintf(home, sizeof home, "%s", cwd_path);
   char *part = strrchr(home, '/');
   if (!part || strcmp(part, "/workspace"))
      return -1;
   *part = 0;
   part = strrchr(home, '/');
   if (!part || strlen(part + 1) != 8 || strncmp(part + 1, "n.", 2) ||
       strspn(part + 3, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") != 6)
      return -1;
   directories[0] = open(home, O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
   if (directories[0] < 0)
      return -1;
   const char *vendors[] = {"codex", "claude"}, *states[] = {"sessions", "projects"};
   int found = 0;
   for (unsigned i = 0; i < 2; i++)
   {
      if (snprintf(link, sizeof link, "%s/.%s/%s", home, vendors[i], states[i]) >= (int)sizeof link)
         goto failed;
      ssize_t n = readlink(link, target, sizeof target - 1);
      if (n < 0 && errno == ENOENT)
         continue;
      if (n < 1 || n >= (ssize_t)sizeof target - 1 || target[0] != '/')
         goto failed;
      target[n] = 0;
      char validate[4096];
      snprintf(validate, sizeof validate, "%s", target);
      char *last = strrchr(validate, '/');
      if (!last || strcmp(last + 1, states[i]))
         goto failed;
      *last = 0;
      last = strrchr(validate, '/');
      if (!last || strlen(last + 1) != 64 || strspn(last + 1, "0123456789abcdef") != 64)
         goto failed;
      *last = 0;
      last = strrchr(validate, '/');
      if (!last || !last[1] || strlen(last + 1) >= 128 ||
          strspn(last + 1, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") !=
              strlen(last + 1))
         goto failed;
      *last = 0;
      last = strrchr(validate, '/');
      char vendor[32];
      snprintf(vendor, sizeof vendor, "browser-%s", vendors[i]);
      if (!last || strcmp(last + 1, vendor) || found++)
         goto failed;
      directories[1] = open(target, O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (directories[1] < 0)
         goto failed;
      struct stat st;
      if (fstat(directories[0], &st) || st.st_uid != geteuid() || (st.st_mode & 0077) ||
          fstat(directories[1], &st) || st.st_uid != geteuid() || (st.st_mode & 0022))
         goto failed;
   }
   if (found == 1)
      return 0;
failed:
   if (directories[0] >= 0)
      close(directories[0]);
   if (directories[1] >= 0)
      close(directories[1]);
   directories[0] = directories[1] = -1;
   return -1;
}
/* Landlock ABI4 controls TCP ports, not destination addresses. No bind
 * grants: SDK listeners and arbitrary host-service ports remain unavailable. */
static int restrict_resources(const server_native_containment_t *o, const char *cwd_path)
{
#if defined(SYS_landlock_create_ruleset) && defined(SYS_landlock_add_rule) &&                      \
    defined(SYS_landlock_restrict_self) && defined(SYS_io_uring_setup) &&                          \
    (defined(__x86_64__) || defined(__aarch64__))
   if (syscall(SYS_landlock_create_ruleset, NULL, 0, 1) < 4)
      return -1;
   struct
   {
      uint64_t fs, net;
   } policy = {0x7ff2, 3};
   int rules = (int)syscall(SYS_landlock_create_ruleset, &policy, sizeof policy, 0);
   if (rules < 0)
      return -1;
   int directories[2] = {-1, -1};
   int valid = !cache_write_directories(cwd_path, directories);
   for (unsigned i = 0; valid && i < 2; i++)
   {
      struct
      {
         uint64_t access;
         int32_t fd;
      } __attribute__((packed)) grant = {0x7ff2 & ~((1ULL << 6) | (1ULL << 9) | (1ULL << 11)),
                                         directories[i]};
      if (syscall(SYS_landlock_add_rule, rules, 1, &grant, 0))
         valid = 0;
   }
   for (unsigned i = 0; i < 2; i++)
      if (directories[i] >= 0)
         close(directories[i]);
   for (size_t i = 0; i < o->port_count; i++)
   {
      struct
      {
         uint64_t access, port;
      } grant = {2, o->tcp_ports[i]};
      if (syscall(SYS_landlock_add_rule, rules, 2, &grant, 0))
         valid = 0;
   }
   if (valid && syscall(SYS_landlock_restrict_self, rules, 0))
      valid = 0;
   close(rules);
   if (!valid)
      return -1;
#if defined(__x86_64__)
#define NATIVE_AUDIT_ARCH AUDIT_ARCH_X86_64
#else
#define NATIVE_AUDIT_ARCH AUDIT_ARCH_AARCH64
#endif
#define NATIVE_DENY (SECCOMP_RET_ERRNO | EPERM)
   /* socketpair permits only already-connected anonymous Unix streams; no
    * datagram/seqpacket endpoint can reconnect/rebind to a host service. */
   struct sock_filter code[] = {
       BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
       BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, NATIVE_AUDIT_ARCH, 1, 0),
       BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
       BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
       BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, 0x40000000, 0, 1),
       BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
       BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_io_uring_setup, 0, 1),
       BPF_STMT(BPF_RET | BPF_K, NATIVE_DENY),
       BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_socket, 0, 11),
       BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[0])),
       BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AF_INET, 1, 0),
       BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AF_INET6, 0, 7),
       BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[1])),
       BPF_STMT(BPF_ALU | BPF_AND | BPF_K, ~(SOCK_CLOEXEC | SOCK_NONBLOCK)),
       BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SOCK_STREAM, 0, 4),
       BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[2])),
       BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 1, 0),
       BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, IPPROTO_TCP, 0, 1),
       BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
       BPF_STMT(BPF_RET | BPF_K, NATIVE_DENY),
       BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_socketpair, 0, 9),
       BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[0])),
       BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AF_UNIX, 0, 6),
       BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[1])),
       BPF_STMT(BPF_ALU | BPF_AND | BPF_K, ~(SOCK_CLOEXEC | SOCK_NONBLOCK)),
       BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SOCK_STREAM, 0, 3),
       BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[2])),
       BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 1),
       BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
       BPF_STMT(BPF_RET | BPF_K, NATIVE_DENY),
       BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
   };
   struct sock_fprog program = {(unsigned short)(sizeof code / sizeof code[0]), code};
   return (int)syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &program);
#undef NATIVE_AUDIT_ARCH
#undef NATIVE_DENY
#else
   (void)o;
   (void)cwd_path;
   return -1;
#endif
}
static int child_guard(void *context, int executable, int cwd)
{
   server_native_containment_t *o = context;
   close(o->command[1]);
   close(o->ready[0]);
   close_other_fds(o, executable, cwd);
   struct stat original_cwd, binary;
   char cwd_path[4096];
   if (cwd == executable || fstat(cwd, &original_cwd) || !S_ISDIR(original_cwd.st_mode) ||
       fstat(executable, &binary) || !S_ISREG(binary.st_mode) || fchdir(cwd) ||
       syscall(SYS_getcwd, cwd_path, sizeof cwd_path) <= 0 || cwd_path[0] != '/')
      return -1;
#define NATIVE_CHILD_REQUIRE(call)                                                                 \
   do                                                                                              \
   {                                                                                               \
      if ((call))                                                                                  \
      {                                                                                            \
         return -1;                                                                                \
      }                                                                                            \
   } while (0)
   NATIVE_CHILD_REQUIRE(byte_read(o->command[0], 'A'));
   NATIVE_CHILD_REQUIRE(unshare(CLONE_NEWUSER));
   NATIVE_CHILD_REQUIRE(write_at(AT_FDCWD, "/proc/self/setgroups", "deny\n"));
   NATIVE_CHILD_REQUIRE(write_at(AT_FDCWD, "/proc/self/uid_map", o->uidmap));
   NATIVE_CHILD_REQUIRE(write_at(AT_FDCWD, "/proc/self/gid_map", o->gidmap));
   NATIVE_CHILD_REQUIRE(unshare(CLONE_NEWNS | CLONE_NEWCGROUP | CLONE_NEWPID));
   NATIVE_CHILD_REQUIRE(mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL));
   pid_t child = fork();
   if (child < 0)
      return -1;
   if (child)
   {
      close(o->command[0]);
      close(o->ready[1]);
      close(executable);
      if (cwd != executable)
         close(cwd);
      int status;
      pid_t result;
      do
      {
         result = waitpid(child, &status, 0);
      } while (result < 0 && errno == EINTR);
      _exit(result == child && WIFEXITED(status) ? WEXITSTATUS(status) : 126);
   }
   NATIVE_CHILD_REQUIRE(mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL));
   NATIVE_CHILD_REQUIRE(mount("none", "/sys/fs/cgroup", "tmpfs",
                              MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC, "size=4096,mode=000"));
   /* A directory FD opened before unshare pins the old vfsmount. Resolve the
    * approved absolute cwd through the NEW namespace, prove the same inode,
    * then discard the old anchor even if the caller omitted FD_CLOEXEC. */
   int rebased_cwd = open(cwd_path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
   struct stat rebased;
   if (rebased_cwd < 0 || fstat(rebased_cwd, &rebased) || rebased.st_dev != original_cwd.st_dev ||
       rebased.st_ino != original_cwd.st_ino || fchdir(rebased_cwd))
   {
      if (rebased_cwd >= 0)
         close(rebased_cwd);
      return -1;
   }
   close(rebased_cwd);
   close(cwd);
   NATIVE_CHILD_REQUIRE(prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0));
   for (int cap = 0; cap < 64; cap++)
      if (prctl(PR_CAPBSET_DROP, cap, 0, 0, 0) && errno != EINVAL)
         return -1;
   if (prctl(PR_SET_SECUREBITS,
             SECBIT_NOROOT | SECBIT_NOROOT_LOCKED | SECBIT_NO_SETUID_FIXUP |
                 SECBIT_NO_SETUID_FIXUP_LOCKED,
             0, 0, 0))
      return -1;
   struct __user_cap_header_struct header = {_LINUX_CAPABILITY_VERSION_3, 0};
   struct __user_cap_data_struct capabilities[2] = {{0}};
   if (syscall(SYS_capset, &header, capabilities) || restrict_resources(o, cwd_path) ||
       byte_write(o->ready[1], 'R') || byte_read(o->command[0], 'X'))
      return -1;
   close(o->ready[1]);
   close(o->command[0]);
#undef NATIVE_CHILD_REQUIRE
   return 0;
}
static int parent_guard(void *context, pid_t child)
{
   server_native_containment_t *o = context;
   if (o->used || child <= 0 || !same_boundary(o))
      return -1;
   o->used = 1;
   close(o->command[0]);
   o->command[0] = -1;
   close(o->ready[1]);
   o->ready[1] = -1;
   char pid[32];
   snprintf(pid, sizeof pid, "%ld\n", (long)child);
   int valid = !write_at(o->leaf, "cgroup.procs", pid) && !byte_write(o->command[1], 'A');
   struct pollfd pollfd = {.fd = o->ready[0], .events = POLLIN};
   int ready;
   do
   {
      ready = poll(&pollfd, 1, 5000);
   } while (ready < 0 && errno == EINTR);
   valid = valid && ready == 1 && (pollfd.revents & POLLIN) && !byte_read(o->ready[0], 'R');
   if (valid)
      valid = !byte_write(o->command[1], 'X');
   close(o->command[1]);
   o->command[1] = -1;
   close(o->ready[0]);
   o->ready[0] = -1;
   return valid ? 0 : -1;
}
static void number(cJSON *v, const char *key, uintmax_t n)
{
   char text[64];
   snprintf(text, sizeof text, "%" PRIuMAX, n);
   cJSON_AddStringToObject(v, key, text);
}
static int matches_number(const cJSON *v, const char *key, uintmax_t n)
{
   char text[64];
   snprintf(text, sizeof text, "%" PRIuMAX, n);
   const char *value = string(v, key);
   return value && !strcmp(value, text);
}
static server_native_containment_t *allocate(void)
{
   server_native_containment_t *o = calloc(1, sizeof *o);
   if (o)
   {
      o->root = o->leaf = o->command[0] = o->command[1] = o->ready[0] = o->ready[1] = -1;
      o->maxfd = sysconf(_SC_OPEN_MAX);
   }
   return o;
}
server_native_containment_t *server_native_containment_open(const char *root,
                                                            const uint16_t *tcp_ports,
                                                            size_t port_count, char error[512])
{
   if (error)
      error[0] = 0;
   if (!root || !*root || !tcp_ports || !port_count || port_count > 8)
      return NULL;
   server_native_containment_t *o = allocate();
   char host[128], boot[128], mountns[128], nonce[33], type[128];
   int created = 0;
   if (o)
   {
      o->port_count = port_count;
      for (size_t i = 0; i < port_count; i++)
      {
         if (!tcp_ports[i] || (i && tcp_ports[i] <= tcp_ports[i - 1]))
            goto failed;
         o->tcp_ports[i] = tcp_ports[i];
      }
   }
   if (!o || o->maxfd < 0 || !safe_mounts() || identity_text("/etc/machine-id", host) ||
       identity_text("/proc/sys/kernel/random/boot_id", boot) || namespace_identity(mountns) ||
       platform_random_hex(nonce, 32) || (o->root = open_root(root)) < 0 ||
       fstat(o->root, &o->root_stat))
      goto failed;
   snprintf(o->name, sizeof o->name, "aimee-native-%s", nonce);
   if (mkdirat(o->root, o->name, 0700))
      goto failed;
   created = 1;
   o->leaf = openat(o->root, o->name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
   if (o->leaf < 0 || fstat(o->leaf, &o->leaf_stat) ||
       text_at(o->leaf, "cgroup.type", type, sizeof type) || strcmp(type, "domain") ||
       pipe2(o->command, O_CLOEXEC) || pipe2(o->ready, O_CLOEXEC))
      goto failed;
   snprintf(o->uidmap, sizeof o->uidmap, "0 %ld 1\n", (long)geteuid());
   snprintf(o->gidmap, sizeof o->gidmap, "0 %ld 1\n", (long)getegid());
   o->manifest = cJSON_CreateObject();
   if (!o->manifest)
      goto failed;
   cJSON_AddNumberToObject(o->manifest, "version", 1);
   cJSON_AddStringToObject(o->manifest, "kind", "linux_cgroup_v2_namespace");
   cJSON_AddStringToObject(o->manifest, "protocol", protocol);
   cJSON_AddStringToObject(o->manifest, "host_id", host);
   cJSON_AddStringToObject(o->manifest, "boot_id", boot);
   cJSON_AddStringToObject(o->manifest, "mount_namespace", mountns);
   cJSON_AddStringToObject(o->manifest, "root", root);
   cJSON_AddStringToObject(o->manifest, "leaf_name", o->name);
   number(o->manifest, "root_dev", o->root_stat.st_dev);
   number(o->manifest, "root_inode", o->root_stat.st_ino);
   number(o->manifest, "leaf_dev", o->leaf_stat.st_dev);
   number(o->manifest, "leaf_inode", o->leaf_stat.st_ino);
   cJSON *ports = cJSON_AddArrayToObject(o->manifest, "tcp_ports");
   for (size_t i = 0; ports && i < port_count; i++)
      cJSON_AddItemToArray(ports, cJSON_CreateNumber(tcp_ports[i]));
   if (cJSON_GetArraySize(o->manifest) != 13 || cJSON_GetArraySize(ports) != (int)port_count)
      goto failed;
   o->guard = (cli_native_spawn_guard_t){child_guard, parent_guard, o};
   return o;
failed:
   if (error)
      snprintf(error, 512, "qualified native cgroup/user/mount/PID custody unavailable");
   if (o && created && o->root >= 0)
      unlinkat(o->root, o->name, AT_REMOVEDIR);
   server_native_containment_close(o, 0);
   return NULL;
}
int server_native_containment_probe(const char *root, const uint16_t *tcp_ports, size_t port_count,
                                    char error[512])
{
   server_native_containment_t *o =
       server_native_containment_open(root, tcp_ports, port_count, error);
   if (!o)
      return -1;
   /* This disposable surrogate is not an SDK HOME or production cache. It
    * exercises the exact materialization policy before any real reservation. */
   char base[4096] = "/var/tmp/aimee-native-capability-XXXXXX";
   char paths[7][4096] = {{0}}, link[4096] = "";
   int layout = mkdtemp(base) != NULL;
   static const char *suffixes[] = {
       "browser-codex", "browser-codex/probe",
       "browser-codex/probe/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
       "browser-codex/probe/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/"
       "sessions"};
   for (unsigned i = 0; layout && i < 4; i++)
   {
      layout =
          snprintf(paths[i], sizeof paths[i], "%s/%s", base, suffixes[i]) < (int)sizeof paths[i] &&
          !mkdir(paths[i], 0700);
   }
   if (layout)
      layout = snprintf(paths[4], sizeof paths[4], "%s/n.XXXXXX", base) < (int)sizeof paths[4] &&
               mkdtemp(paths[4]);
   if (layout)
      layout = snprintf(paths[5], sizeof paths[5], "%s/.codex", paths[4]) < (int)sizeof paths[5] &&
               !mkdir(paths[5], 0700);
   if (layout)
      layout =
          snprintf(paths[6], sizeof paths[6], "%s/workspace", paths[4]) < (int)sizeof paths[6] &&
          !mkdir(paths[6], 0700);
   if (layout)
      layout = snprintf(link, sizeof link, "%s/sessions", paths[5]) < (int)sizeof link &&
               !symlink(paths[3], link);
   int executable = layout ? open("/proc/self/exe", O_RDONLY | O_CLOEXEC) : -1;
   int cwd = layout ? open(paths[6], O_RDONLY | O_DIRECTORY | O_CLOEXEC) : -1;
   pid_t child = executable >= 0 && cwd >= 0 ? fork() : -1;
   if (child == 0)
      _exit(child_guard(o, executable, cwd) ? 126 : 0);
   if (executable >= 0)
      close(executable);
   if (cwd >= 0)
      close(cwd);
   int admitted = child > 0 && !parent_guard(o, child);
   int status = 0;
   if (child > 0)
   {
      if (!admitted)
         (void)kill(child, SIGKILL);
      pid_t waited;
      do
      {
         waited = waitpid(child, &status, 0);
      } while (waited < 0 && errno == EINTR);
      admitted = admitted && waited == child && WIFEXITED(status) && !WEXITSTATUS(status);
   }
   int quiet = !server_native_containment_quiesce(o, 5000);
   server_native_containment_close(o, quiet);
   if (link[0])
      unlink(link);
   for (int i = 6; i >= 0; i--)
      if (paths[i][0])
         rmdir(paths[i]);
   if (layout || paths[0][0])
      rmdir(base);
   if (!admitted || !quiet)
   {
      if (error)
         snprintf(error, 512, "optional native containment capability probe unavailable");
      return -1;
   }
   return 0;
}
const cJSON *server_native_containment_manifest(const server_native_containment_t *o)
{
   return o ? o->manifest : NULL;
}
int server_native_containment_sha256(const server_native_containment_t *o, char out[65])
{
   char *wire = o ? cJSON_PrintUnformatted(o->manifest) : NULL;
   int rc = wire ? aimee_sha256_hex(wire, strlen(wire), out) : -1;
   free(wire);
   return rc;
}
const cli_native_spawn_guard_t *server_native_containment_guard(server_native_containment_t *o)
{
   return o ? &o->guard : NULL;
}
server_native_containment_t *server_native_containment_reopen(const cJSON *manifest,
                                                              char error[512])
{
   char host[128], boot[128], mountns[128];
   const char *kind = string(manifest, "kind"), *contract = string(manifest, "protocol"),
              *name = string(manifest, "leaf_name");
   const cJSON *version = cJSON_GetObjectItemCaseSensitive(manifest, "version");
   server_native_containment_t *o = allocate();
   if (!o || !cJSON_IsObject(manifest) || cJSON_GetArraySize(manifest) != 13 ||
       !cJSON_IsNumber(version) || version->valuedouble != 1 || !kind ||
       strcmp(kind, "linux_cgroup_v2_namespace") || !contract || strcmp(contract, protocol) ||
       !name || strlen(name) != 45 || strncmp(name, "aimee-native-", 13) ||
       strspn(name + 13, "0123456789abcdef") != 32 || identity_text("/etc/machine-id", host) ||
       identity_text("/proc/sys/kernel/random/boot_id", boot) || namespace_identity(mountns) ||
       !string(manifest, "host_id") || strcmp(string(manifest, "host_id"), host) ||
       !string(manifest, "boot_id") || strcmp(string(manifest, "boot_id"), boot) ||
       !string(manifest, "mount_namespace") ||
       strcmp(string(manifest, "mount_namespace"), mountns) || !safe_mounts() ||
       (o->root = open_root(string(manifest, "root"))) < 0 || fstat(o->root, &o->root_stat) ||
       !matches_number(manifest, "root_dev", o->root_stat.st_dev) ||
       !matches_number(manifest, "root_inode", o->root_stat.st_ino))
      goto failed;
   snprintf(o->name, sizeof o->name, "%s", name);
   o->leaf = openat(o->root, o->name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
   if (o->leaf < 0 || fstat(o->leaf, &o->leaf_stat) ||
       !matches_number(manifest, "leaf_dev", o->leaf_stat.st_dev) ||
       !matches_number(manifest, "leaf_inode", o->leaf_stat.st_ino) || !same_boundary(o))
      goto failed;
   const cJSON *ports = cJSON_GetObjectItemCaseSensitive(manifest, "tcp_ports");
   if (!cJSON_IsArray(ports) || cJSON_GetArraySize(ports) < 1 || cJSON_GetArraySize(ports) > 8)
      goto failed;
   o->port_count = (size_t)cJSON_GetArraySize(ports);
   for (size_t i = 0; i < o->port_count; i++)
   {
      const cJSON *port = cJSON_GetArrayItem(ports, (int)i);
      if (!cJSON_IsNumber(port) || port->valuedouble < 1 || port->valuedouble > 65535 ||
          port->valuedouble != (uint16_t)port->valuedouble ||
          (i && port->valuedouble <= o->tcp_ports[i - 1]))
         goto failed;
      o->tcp_ports[i] = (uint16_t)port->valuedouble;
   }
   o->manifest = cJSON_Duplicate(manifest, 1);
   o->used = 1;
   if (!o->manifest)
      goto failed;
   return o;
failed:
   if (error)
      snprintf(error, 512, "native containment identity unavailable or changed");
   server_native_containment_close(o, 0);
   return NULL;
}
int server_native_containment_empty(server_native_containment_t *o)
{
   char events[512];
   if (!same_boundary(o) || text_at(o->leaf, "cgroup.events", events, sizeof events))
      return -1;
   const char *p = events;
   int seen = 0, empty = 0;
   while (*p)
   {
      const char *end = strchr(p, '\n');
      size_t len = end ? (size_t)(end - p) : strlen(p);
      if (len >= 10 && !strncmp(p, "populated ", 10))
      {
         if (seen++ || len != 11 || (p[10] != '0' && p[10] != '1'))
            return -1;
         empty = p[10] == '0';
      }
      if (!end)
         break;
      p = end + 1;
   }
   return seen == 1 ? empty : -1;
}
int server_native_containment_quiesce(server_native_containment_t *o, int timeout_ms)
{
   if (timeout_ms < 0 || timeout_ms > 10000 || !same_boundary(o))
      return -1;
   int empty = server_native_containment_empty(o);
   if (empty == 1)
      return 0;
   if (empty < 0 || write_at(o->leaf, "cgroup.kill", "1\n"))
      return -1;
   for (int elapsed = 0; elapsed <= timeout_ms; elapsed += 10)
   {
      empty = server_native_containment_empty(o);
      if (empty == 1)
         return 0;
      if (empty < 0)
         return -1;
      struct timespec tick = {0, 10000000};
      nanosleep(&tick, NULL);
   }
   return -1;
}
void server_native_containment_close(server_native_containment_t *o, int remove_empty)
{
   if (!o)
      return;
   for (unsigned i = 0; i < 2; i++)
   {
      if (o->command[i] >= 0)
         close(o->command[i]);
      if (o->ready[i] >= 0)
         close(o->ready[i]);
   }
   if (remove_empty && server_native_containment_empty(o) == 1)
      unlinkat(o->root, o->name, AT_REMOVEDIR);
   if (o->leaf >= 0)
      close(o->leaf);
   if (o->root >= 0)
      close(o->root);
   cJSON_Delete(o->manifest);
   free(o);
}
#else
int server_native_containment_probe(const char *root, const uint16_t *tcp_ports, size_t port_count,
                                    char error[512])
{
   (void)root;
   (void)tcp_ports;
   (void)port_count;
   if (error)
      snprintf(error, 512, "optional native containment unavailable on this platform");
   return -1;
}
struct server_native_containment
{
   int unused;
};
server_native_containment_t *server_native_containment_open(const char *root,
                                                            const uint16_t *tcp_ports,
                                                            size_t port_count, char error[512])
{
   (void)root;
   (void)tcp_ports;
   (void)port_count;
   if (error)
      snprintf(error, 512, "qualified native containment unavailable on this platform");
   return NULL;
}
server_native_containment_t *server_native_containment_reopen(const cJSON *v, char error[512])
{
   (void)v;
   return server_native_containment_open(NULL, NULL, 0, error);
}
const cJSON *server_native_containment_manifest(const server_native_containment_t *o)
{
   (void)o;
   return NULL;
}
int server_native_containment_sha256(const server_native_containment_t *o, char out[65])
{
   (void)o;
   (void)out;
   return -1;
}
const cli_native_spawn_guard_t *server_native_containment_guard(server_native_containment_t *o)
{
   (void)o;
   return NULL;
}
int server_native_containment_empty(server_native_containment_t *o)
{
   (void)o;
   return -1;
}
int server_native_containment_quiesce(server_native_containment_t *o, int timeout)
{
   (void)o;
   (void)timeout;
   return -1;
}
void server_native_containment_close(server_native_containment_t *o, int remove)
{
   (void)o;
   (void)remove;
}
#endif
