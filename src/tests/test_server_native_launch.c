/* Registered launchers select held native descriptors without executing scripts. */
#include "server_native_launch.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__x86_64__)
#define PLATFORM "linux-x64"
#define TRIPLE   "x86_64-unknown-linux-musl"
#elif defined(__aarch64__)
#define PLATFORM "linux-arm64"
#define TRIPLE   "aarch64-unknown-linux-musl"
#endif
static char root[4096], wrapper[4096], manifest[4096], native_manifest[4096], binary[4096];
static void write_file(const char *path, const char *text, mode_t mode)
{
   FILE *stream = fopen(path, "wb");
   assert(stream);
   size_t size = strlen(text);
   size_t written = fwrite(text, 1, size, stream);
   assert(written == size);
   int rc = fclose(stream);
   assert(!rc);
   rc = chmod(path, mode);
   assert(!rc);
}
static void copy_binary(const char *path)
{
   FILE *source = fopen("/bin/true", "rb"), *destination = fopen(path, "wb");
   assert(source && destination);
   char bytes[8192];
   size_t count;
   while ((count = fread(bytes, 1, sizeof bytes, source)))
   {
      size_t written = fwrite(bytes, 1, count, destination);
      assert(written == count);
   }
   assert(!ferror(source));
   int a = fclose(source), b = fclose(destination);
   assert(!a && !b);
   a = chmod(path, 0700);
   assert(!a);
}
static void directories(const char *path)
{
   char buffer[4096];
   snprintf(buffer, sizeof buffer, "%s", path);
   for (char *p = buffer + 1; *p; p++)
      if (*p == '/')
      {
         *p = 0;
         int rc = mkdir(buffer, 0700);
         struct stat st;
         int found = stat(buffer, &st);
         assert(!rc || (!found && S_ISDIR(st.st_mode)));
         *p = '/';
      }
}
static void expect(const char *sdk, const char *command, int success)
{
   char digest[65], error[512] = {0};
   int fd = server_native_launch_open(sdk, command, digest, error);
   assert((fd >= 0) == success);
   if (success)
   {
      assert(strlen(digest) == 64);
      char magic[4];
      ssize_t n = pread(fd, magic, sizeof magic, 0);
      assert(n == 4 && !memcmp(magic, "\177ELF", 4));
      struct stat expected, held;
      int a = stat(binary, &expected), b = fstat(fd, &held);
      assert(!a && !b && expected.st_dev == held.st_dev && expected.st_ino == held.st_ino);
      close(fd);
   }
   else
      assert(!digest[0] && strstr(error, "registered"));
}
static void main_manifest(const char *version, const char *dependency)
{
   char json[1024];
   snprintf(json, sizeof json,
            "{\"name\":\"@openai/codex\",\"version\":\"%s\",\"bin\":{\"codex\":\"bin/codex.js\"},"
            "\"optionalDependencies\":{\"@openai/codex-" PLATFORM "\":\"%s\"}}",
            version, dependency);
   write_file(manifest, json, 0600);
}
#endif
int main(void)
{
#if defined(__linux__) && defined(PLATFORM)
   char temporary[] = "/var/tmp/aimee-native-launch-XXXXXX";
   char *created = mkdtemp(temporary);
   assert(created);
   snprintf(root, sizeof root, "%s", created);
   snprintf(wrapper, sizeof wrapper, "%s/node_modules/@openai/codex/bin/codex.js", root);
   snprintf(manifest, sizeof manifest, "%s/node_modules/@openai/codex/package.json", root);
   snprintf(native_manifest, sizeof native_manifest,
            "%s/node_modules/@openai/codex-" PLATFORM "/package.json", root);
   snprintf(binary, sizeof binary,
            "%s/node_modules/@openai/codex-" PLATFORM "/vendor/" TRIPLE "/bin/codex", root);
   directories(wrapper);
   directories(binary);
   /* This script must never run; its marker is checked below. */
   char script[8192], marker[4096];
   snprintf(marker, sizeof marker, "%s/script-executed", root);
   snprintf(script, sizeof script, "#!/bin/sh\ntouch '%s'\nexit 99\n", marker);
   write_file(wrapper, script, 0700);
   main_manifest("1.2.3", "npm:@openai/codex@1.2.3-" PLATFORM);
   write_file(native_manifest, "{\"name\":\"@openai/codex\",\"version\":\"1.2.3-" PLATFORM "\"}",
              0600);
   copy_binary(binary);
   expect("codex", binary, 1);
   expect("claude", binary, 1);
   expect("codex", wrapper, 1);
   char link[4096];
   snprintf(link, sizeof link, "%s/codex", root);
   int rc = symlink(wrapper, link);
   assert(!rc);
   expect("codex", link, 1);
   const char *old_path = getenv("PATH");
   char *saved = old_path ? strdup(old_path) : NULL;
   rc = setenv("PATH", root, 1);
   assert(!rc);
   expect("codex", "codex", 1);
   rc = saved ? setenv("PATH", saved, 1) : unsetenv("PATH");
   assert(!rc);
   free(saved);
   expect("claude", wrapper, 0);
   expect("codex", "./codex", 0);
   main_manifest("1.2.4", "npm:@openai/codex@1.2.3-" PLATFORM);
   expect("codex", wrapper, 0);
   main_manifest("1.2.3", "npm:@openai/codex@1.2.3-" PLATFORM);
   rc = chmod(manifest, 0666);
   assert(!rc);
   expect("codex", wrapper, 0);
   rc = chmod(manifest, 0600);
   assert(!rc);
   write_file(native_manifest, "{\"name\":\"wrong\",\"version\":\"1.2.3-" PLATFORM "\"}", 0600);
   expect("codex", wrapper, 0);
   write_file(native_manifest, "{\"name\":\"@openai/codex\",\"version\":\"1.2.3-" PLATFORM "\"}",
              0600);
   rc = chmod(binary, 0777);
   assert(!rc);
   expect("codex", wrapper, 0);
   rc = chmod(binary, 0700);
   assert(!rc);
   /* An invalid nested registration must not fall through to valid hoisted SDK. */
   char nested[4096];
   snprintf(nested, sizeof nested,
            "%s/node_modules/@openai/codex/node_modules/@openai/codex-" PLATFORM "/package.json",
            root);
   directories(nested);
   write_file(nested, "{}", 0600);
   expect("codex", wrapper, 0);
   write_file(nested, "{\"name\":\"@openai/codex\",\"version\":\"1.2.3-" PLATFORM "\"}", 0600);
   expect("codex", wrapper, 0); /* Missing nested executable also refuses fallback. */
   char nested_binary[4096];
   snprintf(nested_binary, sizeof nested_binary,
            "%s/node_modules/@openai/codex/node_modules/@openai/codex-" PLATFORM "/vendor/" TRIPLE
            "/bin/codex",
            root);
   directories(nested_binary);
   copy_binary(nested_binary);
   snprintf(binary, sizeof binary, "%s", nested_binary);
   expect("codex", wrapper, 1);
   assert(access(marker, F_OK) != 0);
   /* Remove only the isolated fixture tree, with no shell interpolation. */
   char command[8192];
   snprintf(command, sizeof command, "rm -rf -- '%s'", root);
   rc = system(command);
   assert(!rc);
#else
   char digest[65], error[512];
   int fd = server_native_launch_open("codex", "codex", digest, error);
   assert(fd < 0 && !digest[0]);
#endif
   puts("native launcher registration: ok");
   return 0;
}
