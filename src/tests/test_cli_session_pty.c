/* test_cli_session_pty.c: unit tests for the server-hosted PTY session module.
 *
 * Uses the attach-override seam to forkpty `cat` instead of `tmux attach`, so
 * the full pump (ensure -> queued input -> PTY write -> read -> base64 SSE
 * frame -> client fd) is exercised without a real tmux or claude binary. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "cli_session_pty.h"
#include "util.h"

typedef struct
{
   const char *id;
   int fd;
} stream_arg_t;

static void *stream_thread(void *p)
{
   stream_arg_t *a = (stream_arg_t *)p;
   cli_session_pty_stream(a->id, a->fd);
   return NULL;
}

/* Read SSE frames from fd for up to ~timeout_ms; base64-decode each `data:`
 * frame and return 1 if the decoded bytes ever contain `needle`.
 *
 * The accumulator is scanned FROM A CURSOR and never mutated. Both halves of
 * that matter, and the earlier version did the opposite on each:
 *
 * It NUL-terminated each frame in place to bound the base64 (`*eol = 0`), and
 * it restarted every scan at the top of the buffer. So the second pass found
 * the first frame again, and strchr for its '\n' -- now a '\0' -- returned
 * NULL and broke out of the scan. Every byte that arrived after the first
 * COMPLETE frame was therefore never examined: the test passed only when the
 * needle happened to land in the same read() as that frame, and otherwise sat
 * out its whole timeout. On a loaded runner the echo splits across reads, and
 * that is one flake with no timing in it at all -- the data had arrived and was
 * sitting unscanned. */
static int await_decoded_contains(int fd, const char *needle, int timeout_ms)
{
   char acc[16384];
   size_t acc_len = 0, scanned = 0;
   int waited = 0;
   while (waited < timeout_ms && acc_len < sizeof(acc) - 1)
   {
      struct pollfd pfd = {.fd = fd, .events = POLLIN};
      int pr = poll(&pfd, 1, 200);
      waited += 200;
      if (pr <= 0)
         continue;
      ssize_t n = read(fd, acc + acc_len, sizeof(acc) - 1 - acc_len);
      if (n <= 0)
         break;
      acc_len += (size_t)n;

      /* Complete "data: <b64>\n" frames from where the last scan stopped. */
      for (;;)
      {
         char *line = memmem(acc + scanned, acc_len - scanned, "data: ", 6);
         if (!line)
            break;
         char *eol = memchr(line, '\n', acc_len - (size_t)(line - acc));
         if (!eol)
            break; /* frame still in flight; leave the cursor for the next read */
         /* Copied out rather than terminated in place, so the buffer this scan
            walks is the same one the next scan will walk. */
         size_t b64n = (size_t)(eol - line) - 6;
         char b64[8192];
         if (b64n < sizeof(b64))
         {
            memcpy(b64, line + 6, b64n);
            b64[b64n] = '\0';
            unsigned char decoded[8192];
            size_t dn = aimee_base64_decode(b64, decoded, sizeof(decoded) - 1);
            if (dn != (size_t)-1 && memmem(decoded, dn, needle, strlen(needle)))
               return 1;
         }
         scanned = (size_t)(eol - acc) + 1;
      }
   }
   return 0;
}

static void test_base64_roundtrip(void)
{
   const char *cases[] = {"", "a", "ab", "abc", "hello world\n", "\x00\x01\x02\xff binary"};
   /* Note: the embedded-NUL case is length-tested separately below. */
   for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]) - 1; i++)
   {
      char enc[256];
      size_t el =
          aimee_base64_encode((const unsigned char *)cases[i], strlen(cases[i]), enc, sizeof(enc));
      assert(el == strlen(enc));
      unsigned char dec[256];
      size_t dl = aimee_base64_decode(enc, dec, sizeof(dec));
      assert(dl == strlen(cases[i]));
      assert(memcmp(dec, cases[i], dl) == 0);
   }
   /* Binary-safe (embedded NUL + high byte). */
   unsigned char bin[] = {0x00, 0x01, 0x02, 0xff, 'A'};
   char enc[64];
   aimee_base64_encode(bin, sizeof(bin), enc, sizeof(enc));
   unsigned char dec[64];
   size_t dl = aimee_base64_decode(enc, dec, sizeof(dec));
   assert(dl == sizeof(bin));
   assert(memcmp(dec, bin, sizeof(bin)) == 0);
}

static void test_pty_echo_roundtrip(void)
{
   cli_session_pty_set_attach_override("cat");
   char err[128] = "";
   assert(cli_session_pty_ensure("t1", NULL, NULL, 24, 80, err, sizeof(err)) == 0);
   /* Idempotent for a live id. */
   assert(cli_session_pty_ensure("t1", NULL, NULL, 24, 80, err, sizeof(err)) == 0);

   int sp[2];
   assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0);
   stream_arg_t arg = {.id = "t1", .fd = sp[1]};
   pthread_t th;
   assert(pthread_create(&th, NULL, stream_thread, &arg) == 0);

   /* Type into the PTY; `cat` (and PTY echo) reflect it back to the stream. */
   const unsigned char msg[] = "marco-polo\n";
   assert(cli_session_pty_input("t1", msg, sizeof(msg) - 1) == 0);
   /* Unknown id rejected. */
   assert(cli_session_pty_input("nope", msg, sizeof(msg) - 1) == -1);

   int found = await_decoded_contains(sp[0], "marco-polo", 4000);
   assert(found == 1);

   /* Resize is accepted for a live id, rejected for an unknown one. */
   assert(cli_session_pty_resize("t1", 40, 100) == 0);
   assert(cli_session_pty_resize("nope", 40, 100) == -1);

   cli_session_pty_kill("t1");
   pthread_join(th, NULL);
   close(sp[0]);
   close(sp[1]);

   /* After kill the id is gone: input fails. */
   assert(cli_session_pty_input("t1", msg, sizeof(msg) - 1) == -1);
   cli_session_pty_set_attach_override(NULL);
}

int main(void)
{
   printf("test_base64_roundtrip... ");
   test_base64_roundtrip();
   printf("OK\n");

   printf("test_pty_echo_roundtrip... ");
   test_pty_echo_roundtrip();
   printf("OK\n");

   printf("All cli_session_pty tests passed.\n");
   return 0;
}
