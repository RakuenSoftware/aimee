/* Parity, failure, concurrency, and fork coverage for DB2-owned randomness. */
#include <assert.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

int platform_random_bytes(void *buf, size_t len);
int platform_random_hex(char *out, size_t hex_len);
int db2_support_platform_random_bytes(void *buf, size_t len);
int db2_support_platform_random_hex(char *out, size_t hex_len);
int legacy_platform_random_bytes(void *buf, size_t len);
int legacy_platform_random_hex(char *out, size_t hex_len);

static int fake_open_failure;
static size_t fake_read_limit;
static int fake_close_result;
static unsigned char fake_next;
static int fake_open_count;
static int fake_close_count;
static unsigned char fake_stream;

static void fake_reset(void)
{
   fake_open_failure = 0;
   fake_read_limit = (size_t)-1;
   fake_close_result = 0;
   fake_next = 0x31;
   fake_open_count = 0;
   fake_close_count = 0;
}

FILE *db2_test_fopen(const char *path, const char *mode)
{
   assert(strcmp(path, "/dev/urandom") == 0);
   assert(strcmp(mode, "r") == 0);
   fake_open_count++;
   return fake_open_failure ? NULL : (FILE *)&fake_stream;
}

size_t db2_test_fread(void *buf, size_t size, size_t count, FILE *stream)
{
   assert(stream == (FILE *)&fake_stream);
   assert(size == 1);
   size_t take = count < fake_read_limit ? count : fake_read_limit;
   unsigned char *out = buf;
   for (size_t i = 0; i < take; i++)
      out[i] = fake_next++;
   return take;
}

int db2_test_fclose(FILE *stream)
{
   assert(stream == (FILE *)&fake_stream);
   fake_close_count++;
   return fake_close_result;
}

static void test_abi_and_deterministic_parity(void)
{
   int (*bytes_abi)(void *, size_t) = platform_random_bytes;
   int (*hex_abi)(char *, size_t) = platform_random_hex;
   assert(bytes_abi && hex_abi);

   unsigned char legacy[33] = {0};
   unsigned char support[33] = {0};
   fake_reset();
   int legacy_rc = legacy_platform_random_bytes(legacy + 1, 31);
   assert(fake_open_count == 1 && fake_close_count == 1);
   fake_reset();
   int support_rc = db2_support_platform_random_bytes(support + 1, 31);
   assert(fake_open_count == 1 && fake_close_count == 1);
   assert(legacy_rc == support_rc);
   assert(memcmp(legacy, support, sizeof(legacy)) == 0);

   char legacy_hex[67] = {0};
   char support_hex[67] = {0};
   legacy_hex[0] = support_hex[0] = '!';
   legacy_hex[66] = support_hex[66] = '?';
   fake_reset();
   legacy_rc = legacy_platform_random_hex(legacy_hex + 1, 64);
   fake_reset();
   support_rc = db2_support_platform_random_hex(support_hex + 1, 64);
   assert(legacy_rc == 0 && support_rc == 0);
   assert(memcmp(legacy_hex, support_hex, sizeof(legacy_hex)) == 0);
   assert(strcmp(legacy_hex + 1,
                 "3132333435363738393a3b3c3d3e3f404142434445464748494a4b4c4d4e4f50") == 0);
   assert(legacy_hex[0] == '!' && legacy_hex[66] == '?');
}

static void test_failure_parity(void)
{
   unsigned char legacy[8] = {0};
   unsigned char support[8] = {0};

   fake_reset();
   fake_open_failure = 1;
   int legacy_rc = legacy_platform_random_bytes(legacy, sizeof(legacy));
   assert(fake_open_count == 1 && fake_close_count == 0);
   fake_reset();
   fake_open_failure = 1;
   int support_rc = db2_support_platform_random_bytes(support, sizeof(support));
   assert(legacy_rc == -1 && support_rc == -1);
   assert(fake_open_count == 1 && fake_close_count == 0);

   fake_reset();
   fake_read_limit = 7;
   legacy_rc = legacy_platform_random_bytes(legacy, sizeof(legacy));
   assert(fake_close_count == 1);
   fake_reset();
   fake_read_limit = 7;
   support_rc = db2_support_platform_random_bytes(support, sizeof(support));
   assert(legacy_rc == -1 && support_rc == -1);
   assert(fake_close_count == 1);
   assert(memcmp(legacy, support, 7) == 0);

   fake_reset();
   fake_close_result = -1;
   legacy_rc = legacy_platform_random_bytes(legacy, sizeof(legacy));
   fake_reset();
   fake_close_result = -1;
   support_rc = db2_support_platform_random_bytes(support, sizeof(support));
   assert(legacy_rc == 0 && support_rc == 0);

   char legacy_hex[18] = "!unchanged?";
   char support_hex[18] = "!unchanged?";
   assert(legacy_platform_random_hex(legacy_hex, 0) == -1);
   assert(db2_support_platform_random_hex(support_hex, 0) == -1);
   assert(strcmp(legacy_hex, support_hex) == 0);
   assert(legacy_platform_random_hex(legacy_hex, 3) == -1);
   assert(db2_support_platform_random_hex(support_hex, 3) == -1);

   char legacy_zero[18];
   char support_zero[18];
   memset(legacy_zero, '!', sizeof(legacy_zero));
   memset(support_zero, '!', sizeof(support_zero));
   fake_reset();
   fake_open_failure = 1;
   legacy_rc = legacy_platform_random_hex(legacy_zero, 16);
   fake_reset();
   fake_open_failure = 1;
   support_rc = db2_support_platform_random_hex(support_zero, 16);
   assert(legacy_rc == -1 && support_rc == -1);
   assert(memcmp(legacy_zero, support_zero, sizeof(legacy_zero)) == 0);
   assert(legacy_zero[0] == '\0');

   char maximum[515];
   memset(maximum, '#', sizeof(maximum));
   fake_reset();
   assert(db2_support_platform_random_hex(maximum + 1, 512) == 0);
   assert(maximum[0] == '#' && maximum[514] == '#');
   assert(db2_support_platform_random_hex(maximum + 1, 514) == -1);
}

static int is_lower_hex(const char *text, size_t len)
{
   for (size_t i = 0; i < len; i++)
      if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f')))
         return 0;
   return text[len] == '\0';
}

typedef struct
{
   char last[65];
} random_thread_t;

static void *random_worker(void *opaque)
{
   random_thread_t *state = opaque;
   for (int i = 0; i < 32; i++)
   {
      assert(platform_random_hex(state->last, 64) == 0);
      assert(is_lower_hex(state->last, 64));
   }
   return NULL;
}

static void test_real_entropy_and_concurrency(void)
{
   unsigned char first[64];
   unsigned char second[64];
   assert(platform_random_bytes(first, sizeof(first)) == 0);
   assert(platform_random_bytes(second, sizeof(second)) == 0);
   unsigned char combined = 0;
   for (size_t i = 0; i < sizeof(first); i++)
      combined |= first[i];
   assert(combined != 0);
   assert(memcmp(first, second, sizeof(first)) != 0);

   enum
   {
      THREADS = 8
   };
   pthread_t threads[THREADS];
   random_thread_t states[THREADS] = {{{0}}};
   for (int i = 0; i < THREADS; i++)
      assert(pthread_create(&threads[i], NULL, random_worker, &states[i]) == 0);
   for (int i = 0; i < THREADS; i++)
      assert(pthread_join(threads[i], NULL) == 0);
   for (int i = 0; i < THREADS; i++)
      for (int j = i + 1; j < THREADS; j++)
         assert(strcmp(states[i].last, states[j].last) != 0);
}

#ifndef _WIN32
static void test_fork_safety(void)
{
   int fds[2];
   assert(pipe(fds) == 0);
   pid_t child = fork();
   assert(child >= 0);
   if (child == 0)
   {
      close(fds[0]);
      unsigned char bytes[64];
      if (platform_random_bytes(bytes, sizeof(bytes)) != 0 ||
          write(fds[1], bytes, sizeof(bytes)) != (ssize_t)sizeof(bytes))
         _exit(1);
      close(fds[1]);
      _exit(0);
   }

   close(fds[1]);
   unsigned char parent[64];
   unsigned char from_child[64];
   assert(platform_random_bytes(parent, sizeof(parent)) == 0);
   size_t received = 0;
   while (received < sizeof(from_child))
   {
      ssize_t n = read(fds[0], from_child + received, sizeof(from_child) - received);
      assert(n > 0);
      received += (size_t)n;
   }
   close(fds[0]);
   int status = 0;
   assert(waitpid(child, &status, 0) == child);
   assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
   assert(memcmp(parent, from_child, sizeof(parent)) != 0);
}

static void test_random_survives_descriptor_exhaustion(void)
{
   pid_t child = fork();
   assert(child >= 0);
   if (child == 0)
   {
      struct rlimit limit = {.rlim_cur = 64, .rlim_max = 64};
      if (setrlimit(RLIMIT_NOFILE, &limit) != 0)
         _exit(2);
      size_t count = 0;
      while (count < 64)
      {
         int fd = open("/dev/null", O_RDONLY);
         if (fd < 0)
            break;
         count++;
      }
      if (errno != EMFILE)
         _exit(3);
      unsigned char bytes[32];
      _exit(platform_random_bytes(bytes, sizeof(bytes)) == 0 ? 0 : 4);
   }
   int status = 0;
   assert(waitpid(child, &status, 0) == child);
   assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
#endif

int main(void)
{
   test_abi_and_deterministic_parity();
   test_failure_parity();
   test_real_entropy_and_concurrency();
#ifndef _WIN32
   test_fork_safety();
   test_random_survives_descriptor_exhaustion();
#endif
   return 0;
}
