/* Actual worker transport with real pipes/backpressure; no SDK/model fixture. */
#include "../server/cli_codex.c"
#include <assert.h>
#include <sys/wait.h>
#include <sys/resource.h>

static int descriptor_floor = 1100;

int agent_request_cancelled(void)
{
   return 0;
}
static int capture(const aimee_session_event_t *event, void *context)
{
   (void)event;
   (void)context;
   return 0;
}
static void high_pipe(int fds[2])
{
   int raw[2];
   assert(!pipe(raw));
   for (int i = 0; i < 2; i++)
   {
      fds[i] = fcntl(raw[i], F_DUPFD_CLOEXEC, descriptor_floor);
      assert(fds[i] >= descriptor_floor);
      close(raw[i]);
   }
}
static void nonblocking(int fd)
{
   int flags = fcntl(fd, F_GETFL, 0);
   assert(flags >= 0 && !fcntl(fd, F_SETFL, flags | O_NONBLOCK));
}
static void write_exact(int fd, const char *bytes, size_t length)
{
   for (size_t offset = 0; offset < length;)
   {
      ssize_t count = write(fd, bytes + offset, length - offset);
      if (count < 0 && errno == EINTR)
         continue;
      assert(count > 0);
      offset += (size_t)count;
   }
}
static void high_fd_backpressure(void)
{
   int input[2], output[2], diagnostic[2];
   high_pipe(input);
   high_pipe(output);
   high_pipe(diagnostic);
   size_t length = 1024u * 1024u;
   char *payload = malloc(length);
   assert(payload);
   memset(payload, 'x', length);
   pid_t child = fork();
   assert(child >= 0);
   if (!child)
   {
      alarm(10);
      close(input[1]);
      close(output[0]);
      close(diagnostic[0]);
      const char *status = "{\"method\":\"fixture/status\"}\n";
      write_exact(output[1], status, strlen(status));
      /* stderr must drain too, before the peer can read our large request. */
      write_exact(diagnostic[1], payload, length);
      char bytes[4096];
      size_t consumed = 0;
      while (consumed < length)
      {
         ssize_t count = read(input[0], bytes, sizeof bytes);
         if (count < 0 && errno == EINTR)
            continue;
         assert(count > 0);
         for (ssize_t i = 0; i < count; i++)
            assert(bytes[i] == 'x');
         consumed += (size_t)count;
      }
      const char *done = "{\"method\":\"fixture/complete\"}\n";
      write_exact(output[1], done, strlen(done));
      _exit(0);
   }
   close(input[0]);
   close(output[1]);
   close(diagnostic[1]);
   nonblocking(input[1]);
   nonblocking(output[0]);
   nonblocking(diagnostic[0]);
   cli_codex_t worker = {
       .in_fd = input[1], .out_fd = output[0], .err_fd = diagnostic[0], .err_log_fd = -1};
   assert(!cli_codex_write_all(&worker, payload, length));
   cli_codex_read_status_t status;
   cJSON *frame = cli_codex_read_frame(&worker, 1000, util_now_ms() + 5000, &status);
   assert(frame == worker.frame && status == CLI_CODEX_READ_OK &&
          !strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(frame, "method")),
                  "fixture/status"));
   frame = cli_codex_read_frame(&worker, 1000, util_now_ms() + 5000, &status);
   assert(frame == worker.frame && status == CLI_CODEX_READ_OK &&
          !strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(frame, "method")),
                  "fixture/complete"));
   frame = cli_codex_read_frame(&worker, 1000, util_now_ms() + 5000, &status);
   assert(!frame && status == CLI_CODEX_READ_EOF);
   int code;
   assert(waitpid(child, &code, 0) == child && WIFEXITED(code) && !WEXITSTATUS(code));
   close(worker.in_fd);
   close(worker.out_fd);
   if (worker.err_fd >= 0)
      close(worker.err_fd);
   free(worker.buf);
   cJSON_Delete(worker.frame);
   aimee_harness_ir_free(&worker.ir);
   free(payload);
}
typedef struct
{
   long long start;
   int calls;
} progress_t;
static int expire(void *context)
{
   progress_t *state = context;
   state->calls++;
   return util_now_ms() - state->start >= 100;
}
static void owner_expires_under_write_backpressure(void)
{
   int input[2];
   high_pipe(input);
   nonblocking(input[1]);
   char bytes[4096] = {0};
   while (write(input[1], bytes, sizeof bytes) > 0)
   {
   }
   assert(errno == EAGAIN || errno == EWOULDBLOCK);
   progress_t progress = {.start = util_now_ms()};
   cli_codex_t worker = {.in_fd = input[1],
                         .out_fd = -1,
                         .err_fd = -1,
                         .progress = expire,
                         .progress_context = &progress};
   assert(cli_codex_write_all(&worker, "must not dispatch", 17) < 0);
   assert(progress.calls >= 2 && util_now_ms() - progress.start < 2000);
   assert(aimee_ir_session_capture_failed());
   close(input[0]);
   close(input[1]);
}
int main(void)
{
   struct rlimit original;
   assert(!getrlimit(RLIMIT_NOFILE, &original));
   int raised = 0;
   if (original.rlim_cur < 1200)
   {
      if (original.rlim_max != RLIM_INFINITY && original.rlim_max < 1200)
      {
         descriptor_floor = 10;
         puts("SKIP high-FD range: hard process limit below 1200; exercising all transport cases "
              "at low FDs");
      }
      else
      {
         struct rlimit limit = original;
         limit.rlim_cur = 1200;
         assert(!setrlimit(RLIMIT_NOFILE, &limit));
         raised = 1;
      }
   }
   signal(SIGPIPE, SIG_IGN);
   aimee_ir_session_set_sink(capture, NULL);
   high_fd_backpressure();
   owner_expires_under_write_backpressure();
   aimee_ir_session_set_sink(NULL, NULL);
   if (raised)
      assert(!setrlimit(RLIMIT_NOFILE, &original));
   printf("Codex transport FD floor %d, bidirectional backpressure, EOF and owner deadline: PASS\n",
          descriptor_floor);
   return 0;
}
