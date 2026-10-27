/* Exercise the real trusted helper entry over its bounded byte protocol.
 * This is local held-root I/O, not a Docker or model/source-authority proof. */
#define main fixture_forwarder_main
#include "../aimee_forwarder.c"
#undef main
#include "platform_test_util.h"
#include <assert.h>
#include <dirent.h>
#include <time.h>

typedef struct
{
   pid_t pid;
   int input, output;
} sf_peer_t;

static void sf_transfer(int fd, void *buffer, size_t length, int writing)
{
   unsigned char *bytes = buffer;
   while (length)
   {
      struct pollfd ready = {.fd = fd, .events = writing ? POLLOUT : POLLIN};
      assert(poll(&ready, 1, 5000) > 0);
      ssize_t n = writing ? write(fd, bytes, length) : read(fd, bytes, length);
      assert(n > 0);
      bytes += n;
      length -= (size_t)n;
   }
}
static void sf_line(int fd, char *out, size_t capacity)
{
   size_t n = 0;
   do
   {
      assert(n + 1 < capacity);
      sf_transfer(fd, out + n, 1, 0);
   } while (out[n++] != '\n');
   out[n] = 0;
}
static sf_peer_t sf_start(const char *root)
{
   int input[2], output[2];
   assert(!pipe(input) && !pipe(output));
   fflush(NULL);
   pid_t pid = fork();
   assert(pid >= 0);
   if (!pid)
   {
      alarm(20);
      close(input[1]);
      close(output[0]);
      assert(dup2(input[0], STDIN_FILENO) == STDIN_FILENO);
      assert(dup2(output[1], STDOUT_FILENO) == STDOUT_FILENO);
      if (input[0] > STDERR_FILENO)
         close(input[0]);
      if (output[1] > STDERR_FILENO)
         close(output[1]);
      char *args[] = {"fixture-forwarder", "--file-serve", (char *)root, "/usr/bin/true", NULL};
      /* The production helper is nondumpable. Do not run the child's LSan
       * exit-time ptrace traversal; the instrumented parent owns leak checking. */
      _exit(fixture_forwarder_main(4, args));
   }
   close(input[0]);
   close(output[1]);
   sf_peer_t peer = {.pid = pid, .input = input[1], .output = output[0]};
   char line[128], extra;
   size_t command, home, mcp;
   sf_line(peer.output, line, sizeof line);
   assert(sscanf(line, "raw-owner-v1 ready %zu %zu %zu %c", &command, &home, &mcp, &extra) == 3);
   assert(command && home && command + home + mcp < 16384);
   char *ready = malloc(command + home + mcp);
   assert(ready);
   sf_transfer(peer.output, ready, command + home + mcp, 0);
   free(ready);
   return peer;
}
static void sf_stop(sf_peer_t *peer, int expected)
{
   if (peer->input >= 0)
      close(peer->input);
   peer->input = -1;
   int status = 0, joined = 0;
   for (int i = 0; i < 500 && !joined; i++)
   {
      pid_t result = waitpid(peer->pid, &status, WNOHANG);
      assert(result >= 0);
      joined = result == peer->pid;
      if (!joined)
         usleep(10000);
   }
   assert(joined && WIFEXITED(status) && WEXITSTATUS(status) == expected);
   close(peer->output);
   peer->output = -1;
}
static int sf_request(sf_peer_t *peer, char operation, const char *path, const void *input,
                      size_t length, char **output, size_t *output_length)
{
   char line[128], extra;
   int n = snprintf(line, sizeof line, "raw-file-v1 %c %zu %zu\n", operation, strlen(path), length);
   assert(n > 0 && (size_t)n < sizeof line);
   sf_transfer(peer->input, line, (size_t)n, 1);
   sf_transfer(peer->input, (void *)path, strlen(path), 1);
   if (length)
      sf_transfer(peer->input, (void *)input, length, 1);
   sf_line(peer->output, line, sizeof line);
   int status;
   size_t size;
   assert(sscanf(line, "raw-file-v1 %d %zu %c", &status, &size, &extra) == 2);
   assert(status >= 0 && status <= 2 && size <= SCOPED_FILE_IO_MAX);
   char *bytes = malloc(size + 1);
   assert(bytes);
   if (size)
      sf_transfer(peer->output, bytes, size, 0);
   bytes[size] = 0;
   *output = bytes;
   *output_length = size;
   return status;
}
static void sf_expect(sf_peer_t *peer, char op, const char *path, const void *input, size_t length,
                      int status, const void *expected, size_t expected_length)
{
   char *bytes;
   size_t size;
   assert(sf_request(peer, op, path, input, length, &bytes, &size) == status);
   assert(size == expected_length && (!size || !memcmp(bytes, expected, size)));
   free(bytes);
}
static int sf_has_name(const char *list, size_t length, const char *name)
{
   int found = 0;
   for (size_t i = 0; i < length;)
   {
      const char *end = memchr(list + i, 0, length - i);
      assert(end);
      if (!strcmp(list + i, name))
         found++;
      i = (size_t)(end - list) + 1;
   }
   return found;
}
static void sf_list(sf_peer_t *peer, const char *path, int count, const char *first,
                    const char *second)
{
   char *list;
   size_t length;
   assert(!sf_request(peer, 'D', path, NULL, 0, &list, &length));
   int actual = 0;
   for (size_t i = 0; i < length; i++)
      actual += list[i] == 0;
   assert(actual == count && sf_has_name(list, length, first) == 1);
   if (second)
      assert(sf_has_name(list, length, second) == 1);
   free(list);
}
static void sf_remove_tree(const char *root)
{
   DIR *directory = opendir(root);
   assert(directory);
   struct dirent *entry;
   while ((entry = readdir(directory)))
   {
      if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
         continue;
      char path[4096];
      assert(snprintf(path, sizeof path, "%s/%s", root, entry->d_name) < (int)sizeof path);
      struct stat st;
      assert(!lstat(path, &st));
      if (S_ISDIR(st.st_mode))
         sf_remove_tree(path);
      else
         assert(!unlink(path));
   }
   assert(!closedir(directory) && !rmdir(root));
}
int main(void)
{
   char root[4096], outside[4096], path[4096], alias[4096], secret[4096];
   assert(snprintf(root, sizeof root, "%s/scoped-file-XXXXXX", platform_tmpdir()) <
          (int)sizeof root);
   assert(snprintf(outside, sizeof outside, "%s/scoped-out-XXXXXX", platform_tmpdir()) <
          (int)sizeof outside);
   assert(mkdtemp(root) && mkdtemp(outside));
   sf_peer_t peer = sf_start(root);
   const char *name = "dollar$quote' file\n.txt";
   assert(snprintf(path, sizeof path, "%s/%s", root, name) < (int)sizeof path);
   const unsigned char payload[] = {'a', 0, 'b', '\n', 0xe6, 0x97, 0xa5, 0xe6, 0x9c, 0xac, '\n'};
   sf_expect(&peer, 'W', path, payload, sizeof payload, 0, NULL, 0);
   sf_expect(&peer, 'R', path, NULL, 0, 0, payload, sizeof payload);
   puts("scoped-file-io: actual helper READY and binary R/W PASS; requesting D");
   fflush(stdout);
   sf_list(&peer, root, 1, name, NULL);
   assert(snprintf(alias, sizeof alias, "%s/.", root) < (int)sizeof alias);
   sf_list(&peer, alias, 1, name, NULL);
   assert(snprintf(alias, sizeof alias, "%s/", root) < (int)sizeof alias);
   sf_list(&peer, alias, 1, name, NULL);
   const char *later = "later\nentry";
   assert(snprintf(alias, sizeof alias, "%s/%s", root, later) < (int)sizeof alias);
   int fd = open(alias, O_CREAT | O_EXCL | O_WRONLY, 0600);
   assert(fd >= 0 && !close(fd));
   sf_list(&peer, root, 2, name, later); /* Each D owns an independent cursor. */
   assert(snprintf(path, sizeof path, "%s/parent\n$'/child/text", root) < (int)sizeof path);
   sf_expect(&peer, 'P', path, payload, sizeof payload, 0, NULL, 0);
   sf_expect(&peer, 'R', path, NULL, 0, 0, payload, sizeof payload);
   assert(snprintf(path, sizeof path, "%s/missing/file", root) < (int)sizeof path);
   sf_expect(&peer, 'W', path, "x", 1, 1, NULL, 0);
   assert(snprintf(alias, sizeof alias, "%s/missing", root) < (int)sizeof alias);
   assert(access(alias, F_OK) && errno == ENOENT);
   assert(snprintf(path, sizeof path, "%s/invalid/../escape", root) < (int)sizeof path);
   sf_expect(&peer, 'P', path, "x", 1, 1, NULL, 0);
   assert(snprintf(alias, sizeof alias, "%s/invalid", root) < (int)sizeof alias);
   assert(access(alias, F_OK) && errno == ENOENT);
   assert(snprintf(secret, sizeof secret, "%s/secret", outside) < (int)sizeof secret);
   fd = open(secret, O_CREAT | O_EXCL | O_WRONLY, 0600);
   assert(fd >= 0 && write(fd, "OUTSIDE", 7) == 7 && !close(fd));
   assert(snprintf(path, sizeof path, "%s/link-file", root) < (int)sizeof path &&
          !symlink(secret, path));
   sf_expect(&peer, 'R', path, NULL, 0, 1, NULL, 0);
   sf_expect(&peer, 'W', path, "bad", 3, 1, NULL, 0);
   assert(snprintf(alias, sizeof alias, "%s/link-dir", root) < (int)sizeof alias &&
          !symlink(outside, alias));
   sf_expect(&peer, 'D', alias, NULL, 0, 1, NULL, 0);
   assert(snprintf(path, sizeof path, "%s/link-dir/new", root) < (int)sizeof path);
   sf_expect(&peer, 'P', path, "bad", 3, 1, NULL, 0);
   sf_expect(&peer, 'D', outside, NULL, 0, 1, NULL, 0);
   fd = open(secret, O_RDONLY);
   char observed[8] = {0};
   assert(fd >= 0 && read(fd, observed, 8) == 7 && !memcmp(observed, "OUTSIDE", 7) && !close(fd));
   assert(snprintf(path, sizeof path, "%s/new", outside) < (int)sizeof path && access(path, F_OK));
   assert(snprintf(path, sizeof path, "%s/too-large", root) < (int)sizeof path);
   fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
   assert(fd >= 0 && !ftruncate(fd, SCOPED_FILE_IO_MAX + 1) && !close(fd));
   sf_expect(&peer, 'R', path, NULL, 0, 1, NULL, 0);
   sf_stop(&peer, 0);
   peer = sf_start(root);
   assert(snprintf(path, sizeof path, "%s/partial-parent/file", root) < (int)sizeof path);
   char header[128];
   int n = snprintf(header, sizeof header, "raw-file-v1 P %zu 5\n", strlen(path));
   assert(n > 0 && (size_t)n < sizeof header);
   sf_transfer(peer.input, header, (size_t)n, 1);
   sf_transfer(peer.input, path, strlen(path), 1);
   sf_transfer(peer.input, "ab", 2, 1);
   sf_stop(&peer, 1);
   assert(snprintf(alias, sizeof alias, "%s/partial-parent", root) < (int)sizeof alias &&
          access(alias, F_OK));
   peer = sf_start(root);
   assert(snprintf(path, sizeof path, "%s/oversize-parent/file", root) < (int)sizeof path);
   n = snprintf(header, sizeof header, "raw-file-v1 P %zu %u\n", strlen(path),
                SCOPED_FILE_IO_MAX + 1);
   assert(n > 0 && (size_t)n < sizeof header);
   sf_transfer(peer.input, header, (size_t)n, 1);
   sf_stop(&peer, 1);
   assert(snprintf(alias, sizeof alias, "%s/oversize-parent", root) < (int)sizeof alias &&
          access(alias, F_OK));
   sf_remove_tree(root);
   sf_remove_tree(outside);
   puts("scoped-file-io: R/W/P/D binary/UTF8, independent cursors, parents, confinement, "
        "truncation and bounds PASS");
   return 0;
}
