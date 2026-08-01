#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "kb_workload_helper_posix.h"

#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <openssl/crypto.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/close_range.h>
#endif

static int64_t monotonic_ms(void)
{
   struct timespec now;
   if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
      return -1;
   return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int remaining_ms(int64_t deadline)
{
   int64_t now = monotonic_ms();
   if (now < 0 || now >= deadline)
      return 0;
   int64_t left = deadline - now;
   return left > INT_MAX ? INT_MAX : (int)left;
}

static int trusted_directory(int fd)
{
   struct stat st;
   return fstat(fd, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == 0 && !(st.st_mode & 022);
}

static kb_workload_helper_result_t open_failure(int error)
{
   switch (error)
   {
   case EINTR:
   case EAGAIN:
   case EMFILE:
   case ENFILE:
   case ENOMEM:
   case EIO:
   case ESTALE:
   case ETIMEDOUT:
   case ENOENT:
      return KB_WORKLOAD_HELPER_UNAVAILABLE;
   default:
      return KB_WORKLOAD_HELPER_INTEGRITY;
   }
}

/* Reject malformed path spellings before opening even `/`. Besides making the
 * checked walk easier to audit, this keeps an invalid path's result independent
 * of transient descriptor pressure: `/usr//bin/cat` is invalid even when the
 * process cannot currently acquire a directory descriptor. */
static int canonical_absolute_path(const char *path)
{
   if (!path || path[0] != '/' || path[1] == 0 || strnlen(path, PATH_MAX) == PATH_MAX)
      return 0;
   const char *component = path + 1;
   for (;;)
   {
      const char *slash = strchr(component, '/');
      size_t length = slash ? (size_t)(slash - component) : strlen(component);
      if (!length || length > NAME_MAX || (length == 1 && component[0] == '.') ||
          (length == 2 && component[0] == '.' && component[1] == '.'))
         return 0;
      if (!slash)
         return 1;
      component = slash + 1;
   }
}

static int native_elf(int fd)
{
   unsigned char ident[EI_NIDENT];
   if (pread(fd, ident, sizeof(ident), 0) != (ssize_t)sizeof(ident) ||
       memcmp(ident, ELFMAG, SELFMAG) != 0 || ident[EI_VERSION] != EV_CURRENT)
      return 0;
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
   if (ident[EI_DATA] != ELFDATA2LSB)
      return 0;
#else
   if (ident[EI_DATA] != ELFDATA2MSB)
      return 0;
#endif
#if UINTPTR_MAX == UINT64_MAX
   Elf64_Ehdr header;
   if (ident[EI_CLASS] != ELFCLASS64 ||
       pread(fd, &header, sizeof(header), 0) != (ssize_t)sizeof(header) ||
       (header.e_type != ET_EXEC && header.e_type != ET_DYN))
      return 0;
   unsigned machine = header.e_machine;
#else
   Elf32_Ehdr header;
   if (ident[EI_CLASS] != ELFCLASS32 ||
       pread(fd, &header, sizeof(header), 0) != (ssize_t)sizeof(header) ||
       (header.e_type != ET_EXEC && header.e_type != ET_DYN))
      return 0;
   unsigned machine = header.e_machine;
#endif
#if defined(__x86_64__)
   return machine == EM_X86_64;
#elif defined(__aarch64__)
   return machine == EM_AARCH64;
#elif defined(__i386__)
   return machine == EM_386;
#elif defined(__arm__)
   return machine == EM_ARM;
#elif defined(__riscv)
   return machine == EM_RISCV;
#else
   (void)machine;
   return 0;
#endif
}

kb_workload_helper_result_t kb_workload_checked_root_file_open(const char *path,
                                                               int require_exec_elf, int *fd_out)
{
   if (fd_out)
      *fd_out = -1;
   if (!fd_out || (require_exec_elf != 0 && require_exec_elf != 1) ||
       !canonical_absolute_path(path))
      return KB_WORKLOAD_HELPER_INVALID;
#if !defined(__linux__)
   return KB_WORKLOAD_HELPER_DISABLED;
#else
   int directory = open("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
   if (directory < 0)
      return open_failure(errno);
   if (!trusted_directory(directory))
   {
      close(directory);
      return KB_WORKLOAD_HELPER_INTEGRITY;
   }
   const char *component = path + 1;
   for (;;)
   {
      const char *slash = strchr(component, '/');
      size_t length = slash ? (size_t)(slash - component) : strlen(component);
      if (!length || length > NAME_MAX || (length == 1 && component[0] == '.') ||
          (length == 2 && component[0] == '.' && component[1] == '.'))
      {
         close(directory);
         return KB_WORKLOAD_HELPER_INVALID;
      }
      char name[NAME_MAX + 1];
      memcpy(name, component, length);
      name[length] = 0;
      if (slash)
      {
         int next = openat(directory, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
         if (next < 0)
         {
            kb_workload_helper_result_t result = open_failure(errno);
            close(directory);
            return result;
         }
         if (!trusted_directory(next))
         {
            close(next);
            close(directory);
            return KB_WORKLOAD_HELPER_INTEGRITY;
         }
         close(directory);
         directory = next;
         component = slash + 1;
         continue;
      }
      int leaf = openat(directory, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
      close(directory);
      if (leaf < 0)
         return open_failure(errno);
      struct stat st;
      if (fstat(leaf, &st) != 0)
      {
         kb_workload_helper_result_t result = open_failure(errno);
         close(leaf);
         return result;
      }
      int trusted = S_ISREG(st.st_mode) && st.st_uid == 0 && !(st.st_mode & 022);
      if (trusted && require_exec_elf)
         trusted = !(st.st_mode & (S_ISUID | S_ISGID)) && (st.st_mode & 0111) && native_elf(leaf);
      if (!trusted)
      {
         close(leaf);
         return KB_WORKLOAD_HELPER_INTEGRITY;
      }
      if (leaf <= STDERR_FILENO)
      {
         int moved = fcntl(leaf, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
         close(leaf);
         if (moved < 0)
            return KB_WORKLOAD_HELPER_UNAVAILABLE;
         leaf = moved;
      }
      *fd_out = leaf;
      return KB_WORKLOAD_HELPER_OK;
   }
#endif
}

#if defined(__linux__)
static int pipe_above_stdio(int out[2])
{
   int raw[2];
   if (pipe2(raw, O_CLOEXEC) != 0)
      return -1;
   for (size_t i = 0; i < 2; ++i)
   {
      if (raw[i] > STDERR_FILENO)
         out[i] = raw[i];
      else
      {
         out[i] = fcntl(raw[i], F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
         close(raw[i]);
         if (out[i] < 0)
         {
            if (i)
               close(out[0]);
            return -1;
         }
      }
   }
   return 0;
}

static int nonblocking(int fd)
{
   int flags = fcntl(fd, F_GETFL, 0);
   return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 ? 0 : -1;
}

static void child_close_except(int preserved, long fallback_limit)
{
#if defined(SYS_close_range)
   int lower_ok = 1;
   if (preserved > STDERR_FILENO + 1)
      lower_ok = syscall(SYS_close_range, (unsigned)(STDERR_FILENO + 1), (unsigned)(preserved - 1),
                         0) == 0;
   if (lower_ok && syscall(SYS_close_range, (unsigned)(preserved + 1), ~0U, 0) == 0)
      return;
#endif
   for (long fd = STDERR_FILENO + 1; fd < fallback_limit; ++fd)
      if (fd != preserved)
         close((int)fd);
}

static void kill_group_and_reap(pid_t pid)
{
   if (pid <= 0)
      return;
   (void)kill(-pid, SIGKILL);
   (void)kill(pid, SIGKILL);
   while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
      ;
}
#endif

kb_workload_helper_result_t kb_workload_helper_invoke(int helper_fd, const unsigned char *request,
                                                      size_t request_len, unsigned char *response,
                                                      size_t response_cap, size_t *response_len,
                                                      int timeout_ms)
{
   if (response_len)
      *response_len = 0;
   if (response && response_cap)
      OPENSSL_cleanse(response, response_cap);
   if (helper_fd <= STDERR_FILENO || (!request && request_len) || !response || !response_cap ||
       !response_len || request_len > KB_WORKLOAD_HELPER_FRAME_MAX ||
       response_cap > KB_WORKLOAD_HELPER_FRAME_MAX || timeout_ms <= 0 ||
       timeout_ms > KB_WORKLOAD_HELPER_TIMEOUT_MAX_MS)
      return KB_WORKLOAD_HELPER_INVALID;
#if !defined(__linux__) || !defined(SYS_execveat)
   return KB_WORKLOAD_HELPER_DISABLED;
#else
   int64_t start = monotonic_ms();
   if (start < 0 || start > INT64_MAX - timeout_ms)
      return KB_WORKLOAD_HELPER_UNAVAILABLE;
   int64_t deadline = start + timeout_ms;
   struct stat helper_stat;
   if (fstat(helper_fd, &helper_stat) != 0 || !S_ISREG(helper_stat.st_mode) ||
       helper_stat.st_uid != 0 || (helper_stat.st_mode & 022) ||
       (helper_stat.st_mode & (S_ISUID | S_ISGID)) || !(helper_stat.st_mode & 0111) ||
       !native_elf(helper_fd))
      return KB_WORKLOAD_HELPER_INTEGRITY;
   int input[2] = {-1, -1}, output[2] = {-1, -1};
   int old_cancel_state = 0;
   if (pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old_cancel_state) != 0)
      return KB_WORKLOAD_HELPER_UNAVAILABLE;
   sigset_t sigpipe_set, old_signal_mask, pending_signals;
   sigemptyset(&sigpipe_set);
   sigaddset(&sigpipe_set, SIGPIPE);
   if (pthread_sigmask(SIG_BLOCK, &sigpipe_set, &old_signal_mask) != 0)
   {
      (void)pthread_setcancelstate(old_cancel_state, NULL);
      return KB_WORKLOAD_HELPER_UNAVAILABLE;
   }
   int sigpipe_was_pending =
       sigpending(&pending_signals) == 0 && sigismember(&pending_signals, SIGPIPE) == 1;
   if (pipe_above_stdio(input) != 0 || pipe_above_stdio(output) != 0)
   {
      if (input[0] >= 0)
         close(input[0]);
      if (input[1] >= 0)
         close(input[1]);
      if (output[0] >= 0)
         close(output[0]);
      if (output[1] >= 0)
         close(output[1]);
      (void)pthread_sigmask(SIG_SETMASK, &old_signal_mask, NULL);
      (void)pthread_setcancelstate(old_cancel_state, NULL);
      return KB_WORKLOAD_HELPER_UNAVAILABLE;
   }
   long fd_limit = sysconf(_SC_OPEN_MAX);
   if (fd_limit < 4)
      fd_limit = 1024;
   pid_t pid = fork();
   if (pid == 0)
   {
      (void)setsid();
      if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 ||
          dup2(input[0], STDIN_FILENO) != STDIN_FILENO ||
          dup2(output[1], STDOUT_FILENO) != STDOUT_FILENO)
         _exit(126);
      close(input[0]);
      close(input[1]);
      close(output[0]);
      close(output[1]);
      close(STDERR_FILENO);
      child_close_except(helper_fd, fd_limit);
      (void)sigprocmask(SIG_SETMASK, &old_signal_mask, NULL);
      char *const argv[] = {(char *)"aimee-workload-helper", NULL};
      char *const envp[] = {NULL};
      (void)syscall(SYS_execveat, helper_fd, "", argv, envp, AT_EMPTY_PATH);
      _exit(127);
   }
   close(input[0]);
   input[0] = -1;
   close(output[1]);
   output[1] = -1;
   kb_workload_helper_result_t result = KB_WORKLOAD_HELPER_UNAVAILABLE;
   size_t written = 0, received = 0;
   int input_closed = 0, output_eof = 0, reaped = 0, child_status = 0, surplus = 0;
   if (pid < 0 || nonblocking(input[1]) != 0 || nonblocking(output[0]) != 0)
      goto done;
   while (!output_eof || !reaped)
   {
      if (!input_closed && written == request_len)
      {
         close(input[1]);
         input[1] = -1;
         input_closed = 1;
      }
      if (!reaped)
      {
         pid_t waited = waitpid(pid, &child_status, WNOHANG);
         if (waited == pid)
            reaped = 1;
         else if (waited < 0 && errno != EINTR)
            goto done;
      }
      if (output_eof && reaped)
         break;
      int wait = remaining_ms(deadline);
      if (!wait)
      {
         result = KB_WORKLOAD_HELPER_TIMEOUT;
         goto done;
      }
      struct pollfd fds[2];
      nfds_t count = 0;
      int input_slot = -1, output_slot = -1;
      if (!input_closed)
      {
         input_slot = (int)count;
         fds[count++] = (struct pollfd){.fd = input[1], .events = POLLOUT};
      }
      if (!output_eof)
      {
         output_slot = (int)count;
         fds[count++] = (struct pollfd){.fd = output[0], .events = POLLIN};
      }
      if (!count)
      {
         (void)poll(NULL, 0, wait > 1 ? 1 : wait);
         continue;
      }
      int polled = poll(fds, count, wait);
      if (polled < 0)
      {
         if (errno == EINTR)
            continue;
         goto done;
      }
      if (polled == 0)
      {
         result = KB_WORKLOAD_HELPER_TIMEOUT;
         goto done;
      }
      if (input_slot >= 0 && (fds[input_slot].revents & POLLOUT))
      {
         ssize_t n = write(input[1], request + written, request_len - written);
         if (n > 0)
            written += (size_t)n;
         else if (n < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
            goto done;
      }
      if (input_slot >= 0 && (fds[input_slot].revents & (POLLERR | POLLHUP | POLLNVAL)))
         goto done;
      if (output_slot >= 0 && (fds[output_slot].revents & (POLLIN | POLLHUP)))
      {
         unsigned char extra;
         unsigned char *target = received < response_cap ? response + received : &extra;
         size_t capacity = received < response_cap ? response_cap - received : 1;
         ssize_t n = read(output[0], target, capacity);
         if (n > 0)
         {
            if (received == response_cap)
            {
               surplus = 1;
               result = KB_WORKLOAD_HELPER_UNAVAILABLE;
               goto done;
            }
            received += (size_t)n;
         }
         else if (n == 0)
            output_eof = 1;
         else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
            goto done;
      }
      if (output_slot >= 0 && (fds[output_slot].revents & (POLLERR | POLLNVAL)))
         goto done;
   }
   if (!surplus && written == request_len && input_closed && output_eof && reaped &&
       WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0 && remaining_ms(deadline) > 0)
   {
      *response_len = received;
      result = KB_WORKLOAD_HELPER_OK;
   }
done:
   if (input[0] >= 0)
      close(input[0]);
   if (input[1] >= 0)
      close(input[1]);
   if (output[0] >= 0)
      close(output[0]);
   if (output[1] >= 0)
      close(output[1]);
   if (pid > 0 && !reaped)
      kill_group_and_reap(pid);
   if (result != KB_WORKLOAD_HELPER_OK)
   {
      OPENSSL_cleanse(response, response_cap);
      *response_len = 0;
   }
   /* A failed pipe write generates SIGPIPE for this thread. Consume only a
    * signal introduced by this invocation; a signal that was already pending
    * belongs to the caller and must remain pending when its mask is restored. */
   if (!sigpipe_was_pending && sigpending(&pending_signals) == 0 &&
       sigismember(&pending_signals, SIGPIPE) == 1)
   {
      const struct timespec no_wait = {0, 0};
      while (sigtimedwait(&sigpipe_set, NULL, &no_wait) < 0 && errno == EINTR)
         ;
   }
   (void)pthread_sigmask(SIG_SETMASK, &old_signal_mask, NULL);
   (void)pthread_setcancelstate(old_cancel_state, NULL);
   return result;
#endif
}
