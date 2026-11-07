#include "server_native_launch.h"
#include <stdio.h>
#include <string.h>
#if defined(__linux__)
#include "aimee_sha256.h"
#include <aimee/ir/aimee_ir.h>
#include "cJSON.h"
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static int trusted_file(int fd, int executable, off_t maximum, struct stat *st)
{
   return fd >= 0 && !fstat(fd, st) && S_ISREG(st->st_mode) &&
          (!executable || (st->st_mode & 0111)) && !(st->st_mode & 0022) &&
          (st->st_uid == getuid() || st->st_uid == 0) && st->st_size > 0 && st->st_size <= maximum;
}
static int binary_digest(int fd, char digest[65])
{
   struct stat before, after;
   if (!trusted_file(fd, 1, 512 * 1024 * 1024, &before) || before.st_size < SELFMAG)
      return -1;
   void *bytes = mmap(NULL, (size_t)before.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
   if (bytes == MAP_FAILED)
      return -1;
   int valid =
       !memcmp(bytes, ELFMAG, SELFMAG) && !aimee_sha256_hex(bytes, (size_t)before.st_size, digest);
   munmap(bytes, (size_t)before.st_size);
   return valid && !fstat(fd, &after) && before.st_dev == after.st_dev &&
                  before.st_ino == after.st_ino && before.st_size == after.st_size &&
                  before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
                  before.st_mtim.tv_nsec == after.st_mtim.tv_nsec
              ? 0
              : -1;
}
static int command_open(const char *command)
{
   if (command[0] == '/')
      return open(command, O_RDONLY | O_CLOEXEC);
   if (strchr(command, '/'))
      return -1;
   const char *search = getenv("PATH");
   char *paths = strdup(search ? search : "/usr/bin:/bin"), *save = NULL;
   if (!paths)
      return -1;
   int fd = -1;
   for (char *directory = strtok_r(paths, ":", &save); directory && fd < 0;
        directory = strtok_r(NULL, ":", &save))
   {
      if (directory[0] != '/')
         continue;
      char path[4096];
      if (snprintf(path, sizeof path, "%s/%s", directory, command) < (int)sizeof path)
         fd = open(path, O_RDONLY | O_CLOEXEC);
   }
   free(paths);
   return fd;
}
static cJSON *manifest_read(int fd)
{
   struct stat st;
   if (!trusted_file(fd, 0, 64 * 1024, &st))
      return NULL;
   char *bytes = malloc((size_t)st.st_size);
   if (!bytes)
      return NULL;
   size_t used = 0;
   while (used < (size_t)st.st_size)
   {
      ssize_t n = read(fd, bytes + used, (size_t)st.st_size - used);
      if (n < 0 && errno == EINTR)
         continue;
      if (n <= 0)
         break;
      used += (size_t)n;
   }
   cJSON *value = used == (size_t)st.st_size ? aimee_ir_json_parse_frame(bytes, used) : NULL;
   free(bytes);
   return value;
}
static const char *field(const cJSON *object, const char *key)
{
   return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, key));
}
static int codex_package_open(int wrapper)
{
#if defined(__x86_64__) || defined(__aarch64__)
#if defined(__x86_64__)
   const char *platform = "linux-x64", *triple = "x86_64-unknown-linux-musl";
#else
   const char *platform = "linux-arm64", *triple = "aarch64-unknown-linux-musl";
#endif
   struct stat st;
   char link[64], root[4096], path[4096], package[96], version[256], dependency[300];
   if (!trusted_file(wrapper, 1, 64 * 1024, &st))
      return -1;
   snprintf(link, sizeof link, "/proc/self/fd/%d", wrapper);
   ssize_t n = readlink(link, root, sizeof root - 1);
   const char suffix[] = "/bin/codex.js";
   if (n <= (ssize_t)strlen(suffix) || n >= (ssize_t)sizeof root - 1)
      return -1;
   root[n] = 0;
   if (strcmp(root + n - strlen(suffix), suffix))
      return -1;
   root[n - strlen(suffix)] = 0;
   if (snprintf(path, sizeof path, "%s/package.json", root) >= (int)sizeof path)
      return -1;
   int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
   cJSON *main = manifest_read(fd);
   if (fd >= 0)
      close(fd);
   int result = -1;
   const char *name = field(main, "name"), *base = field(main, "version");
   const char *bin = field(cJSON_GetObjectItemCaseSensitive(main, "bin"), "codex");
   snprintf(package, sizeof package, "@openai/codex-%s", platform);
   if (!name || strcmp(name, "@openai/codex") || !bin || strcmp(bin, "bin/codex.js") || !base ||
       !base[0] ||
       snprintf(version, sizeof version, "%s-%s", base, platform) >= (int)sizeof version ||
       snprintf(dependency, sizeof dependency, "npm:@openai/codex@%s", version) >=
           (int)sizeof dependency)
      goto done;
   const char *declared =
       field(cJSON_GetObjectItemCaseSensitive(main, "optionalDependencies"), package);
   if (!declared || strcmp(declared, dependency))
      goto done;
   for (int nested = 1; nested >= 0; nested--)
   {
      char directory[4096];
      int size = nested ? snprintf(directory, sizeof directory, "%s/node_modules/%s", root, package)
                        : snprintf(directory, sizeof directory, "%s/../codex-%s", root, platform);
      if (size >= (int)sizeof directory ||
          snprintf(path, sizeof path, "%s/package.json", directory) >= (int)sizeof path)
         break;
      fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
      if (fd < 0 && (errno == ENOENT || errno == ENOTDIR))
         continue;
      cJSON *native = manifest_read(fd);
      if (fd >= 0)
         close(fd);
      const char *native_name = field(native, "name"), *native_version = field(native, "version");
      int valid = native_name && !strcmp(native_name, "@openai/codex") && native_version &&
                  !strcmp(native_version, version);
      cJSON_Delete(native);
      /* An existing nested package owns resolution; never silently substitute
       * a different hoisted SDK when its metadata or binary is invalid. */
      if (valid && snprintf(path, sizeof path, "%s/vendor/%s/bin/codex", directory, triple) <
                       (int)sizeof path)
         result = open(path, O_RDONLY | O_CLOEXEC);
      break;
   }
done:
   cJSON_Delete(main);
   return result;
#else
   (void)wrapper;
   return -1;
#endif
}
#endif

int server_native_launch_open(const char *sdk, const char *command, char digest[65],
                              char error[512])
{
   if (digest)
      digest[0] = 0;
   if (!sdk || !digest || !error || (strcmp(sdk, "codex") && strcmp(sdk, "claude")))
      return -1;
#if defined(__linux__)
   int fd = command_open(command && command[0] ? command : sdk);
   if (!binary_digest(fd, digest))
      return fd;
   int native = !strcmp(sdk, "codex") ? codex_package_open(fd) : -1;
   if (fd >= 0)
      close(fd);
   if (!binary_digest(native, digest))
      return native;
   if (native >= 0)
      close(native);
   digest[0] = 0;
   snprintf(
       error, 512,
       "registered %s launcher has no attestable native executable or matching platform package",
       sdk);
#else
   (void)command;
   snprintf(error, 512, "registered %s descriptor launches are not qualified on this platform",
            sdk);
#endif
   return -1;
}
