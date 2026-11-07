#define _GNU_SOURCE
#include "server_native_containment.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/file.h>
#include <sys/un.h>
#include <sys/syscall.h>
#include <netinet/in.h>
#include <unistd.h>
static uint16_t allowed_ports[1], denied_port;
static const char *bound_home;
static char directory[4096], executable[4096], ready[4096], manifest_path[4096], written[4096];
static void filename(char out[4096], const char *name)
{
   assert(snprintf(out, 4096, "%s/%s", directory, name) < 4096);
}
static void write_file(const char *path, const char *text)
{
   int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
   assert(fd >= 0);
   size_t n = strlen(text);
   assert(write(fd, text, n) == (ssize_t)n);
   close(fd);
}
static void wait_file(const char *path)
{
   for (int i = 0; i < 1000; i++)
   {
      if (!access(path, F_OK))
         return;
      usleep(10000);
   }
   assert(!"child readiness timeout");
}
static void wait_ok(pid_t child)
{
   int status;
   assert(waitpid(child, &status, 0) == child);
   assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
static int sdk(const char *mode, const char *root, const char *output, const char *host_parent,
               const char *allowed, const char *denied, const char *home)
{
   assert(getpid() == 1);
   char own[4096];
   assert(getppid() == 0);
   assert(fcntl(81, F_GETFD) < 0 && errno == EBADF);
   assert(fcntl(84, F_GETFD) < 0 && errno == EBADF);
   assert(snprintf(own, sizeof own, "../../../../proc/%s/root/sys/fs/cgroup/cgroup.procs",
                   host_parent) < (int)sizeof own);
   assert(open(own, O_WRONLY) < 0);
   assert(snprintf(own, sizeof own,
                   "/proc/self/cwd/../../../../proc/%s/root/sys/fs/cgroup/cgroup.procs",
                   host_parent) < (int)sizeof own);
   assert(open(own, O_WRONLY) < 0);
   assert(open("../../../../sys/fs/cgroup/cgroup.procs", O_WRONLY) < 0);
   assert(open("/proc/self/cwd/../../../../sys/fs/cgroup/cgroup.procs", O_WRONLY) < 0);

   int fd = open("/proc/self/status", O_RDONLY);
   assert(fd >= 0);
   close(fd);
   assert(prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) == 1);
   assert(socket(AF_UNIX, SOCK_STREAM, 0) < 0 && errno == EPERM);
   assert(socket(AF_INET, SOCK_DGRAM, 0) < 0 && errno == EPERM);
   assert(socket(AF_NETLINK, SOCK_RAW, 0) < 0 && errno == EPERM);
#ifdef SYS_io_uring_setup
   assert(syscall(SYS_io_uring_setup, 1, NULL) < 0 && errno == EPERM);
#endif
   int pair[2];
   assert(socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) < 0 && errno == EPERM);
   assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair) < 0 && errno == EPERM);
   assert(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair));
   assert(write(pair[0], "x", 1) == 1);
   char byte;
   assert(read(pair[1], &byte, 1) == 1 && byte == 'x');
   struct sockaddr_un bus = {.sun_family = AF_UNIX};
   snprintf(bus.sun_path, sizeof bus.sun_path, "/run/user/1000/bus");
   assert(connect(pair[0], (struct sockaddr *)&bus, sizeof bus) < 0);
   struct sockaddr disconnect = {.sa_family = AF_UNSPEC};
   assert(connect(pair[0], &disconnect, sizeof disconnect) < 0);
   assert(connect(pair[0], (struct sockaddr *)&bus, sizeof bus) < 0);
   struct sockaddr_un rebound = {.sun_family = AF_UNIX};
   snprintf(rebound.sun_path, sizeof rebound.sun_path, "./pair-rebind.sock");
   assert(bind(pair[0], (struct sockaddr *)&rebound, sizeof rebound) < 0 && errno == EACCES);
   assert(listen(pair[0], 1) < 0);
   assert(!shutdown(pair[0], SHUT_RDWR));
   assert(connect(pair[0], (struct sockaddr *)&bus, sizeof bus) < 0);
   close(pair[0]);
   close(pair[1]);
   struct sockaddr_in target = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
   int tcp = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
   assert(tcp >= 0);
   target.sin_port = htons((uint16_t)atoi(denied));
   assert(connect(tcp, (struct sockaddr *)&target, sizeof target) < 0 && errno == EACCES);
   target.sin_port = htons((uint16_t)atoi(allowed));
   assert(!connect(tcp, (struct sockaddr *)&target, sizeof target));
   close(tcp);
   tcp = socket(AF_INET, SOCK_STREAM, 0);
   assert(tcp >= 0);
   target.sin_port = 0;
   assert(bind(tcp, (struct sockaddr *)&target, sizeof target) < 0 && errno == EACCES);
   close(tcp);

   fd = open("/sys/fs/cgroup/cgroup.procs", O_WRONLY);
   assert(fd < 0 && (errno == EROFS || errno == ENOENT || errno == EACCES));
   assert(snprintf(own, sizeof own, "%s/cgroup.procs", root) < (int)sizeof own);
   assert(open(own, O_WRONLY) < 0); // Ancestor host control path is outside the namespace root.
   char cache[4096], control[4096], destination[4096];
   const char *state_link = "../.codex/sessions";
   char explicit_link[4096];
   if (home)
   {
      assert(snprintf(explicit_link, sizeof explicit_link, "%s/.claude/projects", home) <
             (int)sizeof explicit_link);
      state_link = explicit_link;
      assert(open("private-cwd-write", O_WRONLY | O_CREAT, 0600) < 0 && errno == EACCES);
      assert(snprintf(destination, sizeof destination, "%s/ephemeral-write", home) <
             (int)sizeof destination);
      write_file(destination, "private HOME only");
      assert(!unlink(destination));
   }
   ssize_t length = readlink(state_link, cache, sizeof cache - 1);
   assert(length > 0 && length < (ssize_t)sizeof cache - 1);
   cache[length] = 0;
   assert(snprintf(destination, sizeof destination, "%s/retained-cache", cache) <
          (int)sizeof destination);
   fd = open(destination, O_WRONLY | O_CREAT | O_APPEND, 0600);
   assert(fd >= 0 && write(fd, "cache", 5) == 5);
   close(fd);
   char *slash = strrchr(cache, '/');
   assert(slash);
   *slash = 0;
   assert(snprintf(control, sizeof control, "%s/.aimee-native-owner.lock", cache) <
          (int)sizeof control);
   assert(open(control, O_WRONLY) < 0 && errno == EACCES);
   assert(unlink(control) < 0 && errno == EACCES);
   assert(truncate(control, 0) < 0 && errno == EACCES);
   assert(rename(control, "stolen-control") < 0);
   assert(link(control, "hardlinked-control") < 0);
   fd = open(control, O_RDONLY | O_CLOEXEC);
   assert(fd >= 0);
   char fdpath[64];
   snprintf(fdpath, sizeof fdpath, "/proc/self/fd/%d", fd);
   assert(linkat(AT_FDCWD, fdpath, AT_FDCWD, "fd-hardlinked-control", AT_SYMLINK_FOLLOW) < 0);
   close(fd);
   if (home)
      assert(symlink(control, "control-alias") < 0 && errno == EACCES);
   else
   {
      assert(!symlink(control, "control-alias"));
      assert(open("control-alias", O_WRONLY) < 0 && errno == EACCES);
      assert(!unlink("control-alias"));
   }
   assert(rename(cache, "moved-binding") < 0);
   if (!strcmp(mode, "--probe") || !strcmp(mode, "--stable-probe"))
      return 0;
   pid_t child = fork();
   assert(child >= 0);
   if (!child)
   {
      assert(setsid() >= 0);
      fd = open(output, O_WRONLY | O_CREAT | O_APPEND, 0600);
      assert(fd >= 0);
      for (;;)
      {
         assert(write(fd, "x", 1) == 1);
         usleep(10000);
      }
   }
   char path[4096];
   assert(snprintf(path, sizeof path, "%s.ready", output) < (int)sizeof path);
   write_file(path, "ready");
   for (;;)
      pause();
}
static pid_t spawn(server_native_containment_t *owner, const char *mode, const char *root,
                   const char *output, int admit)
{
   const cli_native_spawn_guard_t *guard = server_native_containment_guard(owner);
   int exe = open(executable, O_RDONLY | O_CLOEXEC),
       cwd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
   assert(exe >= 0 && cwd >= 0);
   int oldroot = open("/sys/fs/cgroup", O_RDONLY | O_DIRECTORY);
   assert(oldroot >= 0 && dup2(oldroot, 81) == 81 && dup2(cwd, 84) == 84);
   close(oldroot);
   close(cwd);
   cwd = 84; // Deliberately not CLOEXEC: the guard must discard old mount anchors.
   char host_parent[32], allowed[16], denied[16];
   snprintf(allowed, sizeof allowed, "%u", allowed_ports[0]);
   snprintf(denied, sizeof denied, "%u", denied_port);
   snprintf(host_parent, sizeof host_parent, "%ld", (long)getpid());
   pid_t child = fork();
   assert(child >= 0);
   if (!child)
   {
      assert(!fchdir(cwd));
      if (guard->child(guard->context, exe, cwd))
         _exit(126);
      assert(fcntl(81, F_GETFD) < 0 && errno == EBADF);
      assert(fcntl(cwd, F_GETFD) < 0 && errno == EBADF);
      char *args[] = {executable,     (char *)mode,       (char *)root,
                      (char *)output, host_parent,        allowed,
                      denied,         (char *)bound_home, NULL};
      char *env[] = {"PATH=/usr/bin:/bin", NULL};
      fexecve(exe, args, env);
      _exit(127);
   }
   close(exe);
   close(cwd);
   close(81);
   if (admit > 0)
      assert(!guard->parent(guard->context, child));
   else if (admit < 0)
      assert(guard->parent(guard->context, child));
   return child;
}
static cJSON *read_manifest(void)
{
   FILE *f = fopen(manifest_path, "r");
   assert(f);
   char buf[8192];
   size_t n = fread(buf, 1, sizeof buf - 1, f);
   assert(!ferror(f) && feof(f));
   fclose(f);
   buf[n] = 0;
   cJSON *v = cJSON_Parse(buf);
   assert(v);
   return v;
}
static void test_guard_channels_stdio_holes(void)
{
   fflush(NULL);
   for (int holes = 1; holes < 8; holes++)
   {
      pid_t child = fork();
      assert(child >= 0);
      if (!child)
      {
         for (int fd = 0; fd < 3; fd++)
            if (holes & (1 << fd))
               close(fd);
         int command[2], ready[2], io[2];
         assert(!cli_native_pipe(command) && !cli_native_pipe(ready) && !cli_native_pipe(io));
         assert(dup2(io[0], 0) == 0 && dup2(io[1], 1) == 1 && dup2(io[1], 2) == 2);
         for (int i = 0; i < 2; i++)
         {
            assert(command[i] >= 10 && ready[i] >= 10);
            assert(fcntl(command[i], F_GETFD) & FD_CLOEXEC);
            assert(fcntl(ready[i], F_GETFD) & FD_CLOEXEC);
         }
         char value;
         assert(write(command[1], "A", 1) == 1 && read(command[0], &value, 1) == 1 && value == 'A');
         assert(write(ready[1], "R", 1) == 1 && read(ready[0], &value, 1) == 1 && value == 'R');
         for (int i = 0; i < 2; i++)
         {
            close(command[i]);
            close(ready[i]);
            close(io[i]);
         }
         _exit(0);
      }
      int status;
      assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
   }
}
int main(int argc, char **argv)
{
   if ((argc == 7 || argc == 8) && (!strcmp(argv[1], "--probe") || !strcmp(argv[1], "--writer") ||
                                    !strcmp(argv[1], "--stable-probe")))
      return sdk(argv[1], argv[2], argv[3], argv[4], argv[5], argv[6], argc == 8 ? argv[7] : NULL);
   test_guard_channels_stdio_holes();
   const char *root = getenv("AIMEE_TEST_NATIVE_CGROUP_ROOT");
   if (!root || !*root)
   {
      puts("SKIP actual native containment: AIMEE_TEST_NATIVE_CGROUP_ROOT not configured");
      return 0;
   }
   assert(prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) == 0);
   assert(readlink("/proc/self/exe", executable, sizeof executable - 1) > 0);
   char temporary[] = "/var/tmp/aimee-native-containment-XXXXXX";
   assert(mkdtemp(temporary));
   char sdkhome[4096], vendor[4096], session[4096], binding[4096], state[4096], sdkconfig[4096],
       statelink[4096], control[4096];
   assert(snprintf(sdkhome, sizeof sdkhome, "%s/n.XXXXXX", temporary) < (int)sizeof sdkhome &&
          mkdtemp(sdkhome));
   assert(snprintf(directory, sizeof directory, "%s/workspace", sdkhome) < (int)sizeof directory &&
          !mkdir(directory, 0700));
   assert(snprintf(vendor, sizeof vendor, "%s/browser-codex", temporary) < (int)sizeof vendor &&
          !mkdir(vendor, 0700));
   assert(snprintf(session, sizeof session, "%s/session", vendor) < (int)sizeof session &&
          !mkdir(session, 0700));
   assert(snprintf(binding, sizeof binding,
                   "%s/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                   session) < (int)sizeof binding &&
          !mkdir(binding, 0700));
   assert(snprintf(state, sizeof state, "%s/sessions", binding) < (int)sizeof state &&
          !mkdir(state, 0700));
   assert(snprintf(sdkconfig, sizeof sdkconfig, "%s/.codex", sdkhome) < (int)sizeof sdkconfig &&
          !mkdir(sdkconfig, 0700));
   assert(snprintf(statelink, sizeof statelink, "%s/sessions", sdkconfig) < (int)sizeof statelink &&
          !symlink(state, statelink));
   assert(snprintf(control, sizeof control, "%s/.aimee-native-owner.lock", binding) <
          (int)sizeof control);
   int lock = open(control, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
   assert(lock >= 0 && write(lock, "owner", 5) == 5 && !flock(lock, LOCK_EX | LOCK_NB));
   struct stat original_lock;
   assert(!fstat(lock, &original_lock));
   filename(manifest_path, "owner.json");
   filename(written, "writer");
   assert(snprintf(ready, sizeof ready, "%s.ready", written) < (int)sizeof ready);
   int listeners[2];
   for (int i = 0; i < 2; i++)
   {
      listeners[i] = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
      assert(listeners[i] >= 0);
      struct sockaddr_in bind_address = {.sin_family = AF_INET,
                                         .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
      assert(!bind(listeners[i], (struct sockaddr *)&bind_address, sizeof bind_address));
      socklen_t length = sizeof bind_address;
      assert(!getsockname(listeners[i], (struct sockaddr *)&bind_address, &length));
      if (i == 0)
         allowed_ports[0] = ntohs(bind_address.sin_port);
      else
         denied_port = ntohs(bind_address.sin_port);
      assert(!listen(listeners[i], 16));
   }
   char error[512];
   assert(server_native_containment_probe("/var/tmp", allowed_ports, 1, error) == -1);
   assert(!server_native_containment_probe(root, allowed_ports, 1, error));
   assert(!server_native_containment_open("/var/tmp", allowed_ports, 1, error));
   server_native_containment_t *owner =
       server_native_containment_open(root, allowed_ports, 1, error);
   if (!owner)
   {
      fprintf(stderr, "configured actual containment refused: %s\n", error);
      return 1;
   }
   char hash[65];
   assert(!server_native_containment_sha256(owner, hash) && strlen(hash) == 64);
   pid_t child = spawn(owner, "--probe", root, written, 1);
   wait_ok(child);
   assert(server_native_containment_empty(owner) == 1);
   int competing = open(control, O_RDWR | O_CLOEXEC);
   assert(competing >= 0);
   struct stat current_lock;
   assert(!fstat(competing, &current_lock));
   assert(current_lock.st_ino == original_lock.st_ino &&
          current_lock.st_dev == original_lock.st_dev);
   assert(flock(competing, LOCK_EX | LOCK_NB) < 0 && (errno == EWOULDBLOCK || errno == EAGAIN));
   close(competing);

   cJSON *identity = cJSON_Duplicate(server_native_containment_manifest(owner), 1);
   assert(identity);
   server_native_containment_t *copy = server_native_containment_reopen(identity, error);
   assert(copy);
   server_native_containment_close(copy, 0);
   cJSON_ReplaceItemInObjectCaseSensitive(identity, "boot_id", cJSON_CreateString("changed"));
   assert(!server_native_containment_reopen(identity, error));
   cJSON_Delete(identity);
   server_native_containment_close(owner, 1);
   /* Claude keeps a stable read-only cwd outside its fresh credential HOME.
    * Bind only the original two write interiors; never grant the binding or
    * stable scheduler parents merely to make the SDK launch succeed. */
   char original_directory[4096], claude_home[4096], claude_paths[6][4096], claude_link[4096];
   snprintf(original_directory, sizeof original_directory, "%s", directory);
   assert(snprintf(claude_home, sizeof claude_home, "%s/n.XXXXXX", temporary) <
              (int)sizeof claude_home &&
          mkdtemp(claude_home));
   const char *suffixes[] = {
       "browser-claude", "browser-claude/session",
       "browser-claude/session/bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
       "browser-claude/session/bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb/"
       "projects"};
   for (unsigned i = 0; i < 4; i++)
      assert(snprintf(claude_paths[i], sizeof claude_paths[i], "%s/%s", temporary, suffixes[i]) <
                 (int)sizeof claude_paths[i] &&
             !mkdir(claude_paths[i], 0700));
   assert(snprintf(claude_paths[4], sizeof claude_paths[4], "%s/.claude", claude_home) <
              (int)sizeof claude_paths[4] &&
          !mkdir(claude_paths[4], 0700));
   assert(snprintf(claude_link, sizeof claude_link, "%s/projects", claude_paths[4]) <
              (int)sizeof claude_link &&
          !symlink(claude_paths[3], claude_link));
   assert(snprintf(claude_paths[5], sizeof claude_paths[5], "%s/.aimee-native-owner.lock",
                   claude_paths[2]) < (int)sizeof claude_paths[5]);
   write_file(claude_paths[5], "host control");
   assert(snprintf(directory, sizeof directory, "%s/stable-private-cwd", claude_paths[2]) <
              (int)sizeof directory &&
          !mkdir(directory, 0700));
   bound_home = claude_home;
   owner = server_native_containment_open(root, allowed_ports, 1, error);
   assert(owner && server_native_containment_bind_home(owner, temporary));
   assert(!server_native_containment_bind_home(owner, claude_home));
   assert(server_native_containment_bind_home(owner, claude_home)); // One-time immutable binding.
   child = spawn(owner, "--stable-probe", root, written, 1);
   wait_ok(child);
   assert(server_native_containment_empty(owner) == 1);
   server_native_containment_close(owner, 1);
   /* A state-path replacement after binding must fail before SDK exec. */
   owner = server_native_containment_open(root, allowed_ports, 1, error);
   assert(owner && !server_native_containment_bind_home(owner, claude_home));
   char moved_state[4096];
   assert(snprintf(moved_state, sizeof moved_state, "%s-moved", claude_paths[3]) <
          (int)sizeof moved_state);
   assert(!rename(claude_paths[3], moved_state) && !mkdir(claude_paths[3], 0700));
   child = spawn(owner, "--stable-probe", root, written, -1);
   int refusal;
   assert(waitpid(child, &refusal, 0) == child && WIFEXITED(refusal) && WEXITSTATUS(refusal) != 0);
   assert(server_native_containment_empty(owner) == 1);
   server_native_containment_close(owner, 1);
   assert(!rmdir(claude_paths[3]) && !rename(moved_state, claude_paths[3]));
   bound_home = NULL;
   assert(!rmdir(directory));
   snprintf(directory, sizeof directory, "%s", original_directory);
   char retained_claude[4096];
   assert(snprintf(retained_claude, sizeof retained_claude, "%s/retained-cache", claude_paths[3]) <
          (int)sizeof retained_claude);
   assert(!unlink(retained_claude) && !unlink(claude_link) && !unlink(claude_paths[5]));
   assert(!rmdir(claude_paths[4]) && !rmdir(claude_home));
   for (int i = 3; i >= 0; i--)
      assert(!rmdir(claude_paths[i]));
   // A real owner dies with a detached setsid writer still changing the cache.
   pid_t daemon = fork();
   assert(daemon >= 0);
   if (!daemon)
   {
      owner = server_native_containment_open(root, allowed_ports, 1, error);
      assert(owner);
      char *wire = cJSON_PrintUnformatted(server_native_containment_manifest(owner));
      assert(wire);
      write_file(manifest_path, wire);
      free(wire);
      spawn(owner, "--writer", root, written, 1);
      for (;;)
         pause();
   }
   wait_file(ready);
   identity = read_manifest();
   assert(!kill(daemon, SIGKILL));
   int status;
   assert(waitpid(daemon, &status, 0) == daemon && WIFSIGNALED(status));
   owner = server_native_containment_reopen(identity, error);
   assert(owner && server_native_containment_empty(owner) == 0);
   struct stat before, after;
   assert(!stat(written, &before));
   usleep(60000);
   assert(!stat(written, &after) && after.st_size > before.st_size);
   assert(!server_native_containment_quiesce(owner, 5000));
   assert(server_native_containment_empty(owner) == 1);
   server_native_containment_close(owner, 1);
   assert(!server_native_containment_reopen(identity, error));
   cJSON_Delete(identity);
   while (waitpid(-1, &status, 0) > 0)
   {
   }
   unlink(ready);
   unlink(written);
   // The owner dies before attaching/admitting the child. EOF must prevent
   // SDK exec, even though the child holds no membership in the new leaf yet.
   daemon = fork();
   assert(daemon >= 0);
   if (!daemon)
   {
      owner = server_native_containment_open(root, allowed_ports, 1, error);
      assert(owner);
      char *wire = cJSON_PrintUnformatted(server_native_containment_manifest(owner));
      assert(wire);
      write_file(manifest_path, wire);
      free(wire);
      spawn(owner, "--writer", root, written, 0);
      write_file(ready, "forked-before-admission");
      for (;;)
         pause();
   }
   wait_file(ready);
   identity = read_manifest();
   assert(!kill(daemon, SIGKILL));
   assert(waitpid(daemon, &status, 0) == daemon && WIFSIGNALED(status));
   while (waitpid(-1, &status, 0) > 0)
   {
   }
   assert(access(written, F_OK) && errno == ENOENT);
   owner = server_native_containment_reopen(identity, error);
   assert(owner && server_native_containment_empty(owner) == 1);
   server_native_containment_close(owner, 1);
   cJSON_Delete(identity);
   unlink(ready);
   unlink(written);
   unlink(manifest_path);
   rmdir(directory);
   unlink(statelink);
   rmdir(sdkconfig);
   rmdir(sdkhome);
   char retained[4096];
   assert(snprintf(retained, sizeof retained, "%s/retained-cache", state) < (int)sizeof retained);
   unlink(retained);
   close(lock);
   unlink(control);
   rmdir(state);
   rmdir(binding);
   rmdir(session);
   rmdir(vendor);
   rmdir(temporary);
   close(listeners[0]);
   close(listeners[1]);
   puts("actual kernel native containment: preexec namespace admission, host-control refusal, "
        "owner SIGKILL + escaped writer, exact boundary kill/empty and identity refusal passed");
   return 0;
}
