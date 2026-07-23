#include "kb_vault_operator_status.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static kb_vault_operator_status_t operational(void)
{
   kb_vault_operator_status_t status = {.state = KB_VAULT_OPERATOR_STATE_OPERATIONAL,
                                        .operation_state = KB_VAULT_OPERATOR_OPERATION_NONE,
                                        .remediation = KB_VAULT_OPERATOR_REMEDIATION_NONE,
                                        .seal_epoch = 7,
                                        .control_fence = 11,
                                        .last_opened_fence = 5};
   return status;
}

static int read_status(kb_vault_operator_status_t *status, void *opaque)
{
   (void)opaque;
   *status = operational();
   return 0;
}

typedef struct
{
   int fd;
   int result;
} server_arg_t;

static void *serve(void *opaque)
{
   server_arg_t *arg = opaque;
   arg->result = kb_vault_operator_serve_connection(arg->fd, read_status, NULL);
   close(arg->fd);
   return NULL;
}

static void test_codec(void)
{
   unsigned char request[16];
   assert(kb_vault_operator_request_encode(KB_VAULT_OPERATOR_OPCODE_STATUS, request) == 0);
   assert(!memcmp(request, "A7VS\0\1\0\1\0\0\0\0\0\0\0\0", 16));
   uint16_t opcode = 0;
   uint32_t payload_len = 99;
   assert(kb_vault_operator_request_decode(request, &opcode, &payload_len) == 0);
   assert(opcode == 1 && payload_len == 0);
   request[6] = 0;
   request[7] = 0;
   assert(kb_vault_operator_request_decode(request, &opcode, &payload_len) == 0);
   assert(opcode == 0 && payload_len == 0);
   request[15] = 1;
   assert(kb_vault_operator_request_decode(request, &opcode, &payload_len) == -1);

   kb_vault_operator_status_t status = operational();
   assert(kb_vault_operator_status_validate(&status));
   unsigned char response[96];
   size_t response_len = 0;
   assert(kb_vault_operator_response_encode(KB_VAULT_OPERATOR_TRANSPORT_OK, &status, response,
                                            sizeof(response), &response_len) == 0);
   assert(response_len == 96 && !memcmp(response, "A7VR\0\1\0\0\0\0\0P\0\0\0\0", 16));
   kb_vault_operator_transport_t result = KB_VAULT_OPERATOR_TRANSPORT_INTERNAL;
   kb_vault_operator_status_t decoded;
   assert(kb_vault_operator_response_decode(response, response_len, &result, &decoded) == 0);
   assert(result == KB_VAULT_OPERATOR_TRANSPORT_OK && decoded.state == status.state &&
          decoded.seal_epoch == 7 && decoded.control_fence == 11);
   response[95] = 1;
   assert(kb_vault_operator_response_decode(response, response_len, &result, &decoded) == -1);

   status.state = KB_VAULT_OPERATOR_STATE_SEALED_IDLE;
   status.remediation = KB_VAULT_OPERATOR_REMEDIATION_NONE;
   assert(!kb_vault_operator_status_validate(&status));
   status.remediation = KB_VAULT_OPERATOR_REMEDIATION_UNSEAL;
   assert(kb_vault_operator_status_validate(&status));
   status.state = KB_VAULT_OPERATOR_STATE_RESUME_REQUIRED;
   status.operation_state = KB_VAULT_OPERATOR_OPERATION_PREPARING;
   status.remediation = KB_VAULT_OPERATOR_REMEDIATION_RESUME;
   status.flags = 1;
   status.old_generation = 0;
   status.new_generation = 1;
   status.operation_id[15] = 1;
   assert(kb_vault_operator_status_validate(&status));
   status.new_generation = 2;
   assert(!kb_vault_operator_status_validate(&status));
   status = operational();
   status.control_fence = 0;
   assert(!kb_vault_operator_status_validate(&status));
   status = operational();
   assert(kb_vault_operator_status_exit_mapping(&status) == KB_VAULT_OPERATOR_CLIENT_OK);
   status.state = KB_VAULT_OPERATOR_STATE_SEALED_IDLE;
   status.remediation = KB_VAULT_OPERATOR_REMEDIATION_UNSEAL;
   assert(kb_vault_operator_status_exit_mapping(&status) ==
          KB_VAULT_OPERATOR_CLIENT_ACTION_REQUIRED);
   status.state = KB_VAULT_OPERATOR_STATE_INTEGRITY_FAILURE;
   status.remediation = KB_VAULT_OPERATOR_REMEDIATION_INTEGRITY;
   assert(kb_vault_operator_status_exit_mapping(&status) ==
          KB_VAULT_OPERATOR_CLIENT_INTEGRITY_FAILURE);

   assert(kb_vault_operator_startup_mode(KB_VAULT_OPERATOR_STATE_OPERATIONAL) == 0);
   assert(kb_vault_operator_startup_mode(KB_VAULT_OPERATOR_STATE_SEALED_IDLE) == 1);
   assert(kb_vault_operator_startup_mode(KB_VAULT_OPERATOR_STATE_LOCAL_UNSEAL_REQUIRED) == 1);
   assert(kb_vault_operator_startup_mode(KB_VAULT_OPERATOR_STATE_RESUME_REQUIRED) == 1);
   assert(kb_vault_operator_startup_mode(KB_VAULT_OPERATOR_STATE_RECOVERY_REQUIRED) == 1);
   assert(kb_vault_operator_startup_mode(KB_VAULT_OPERATOR_STATE_COMPLETED_SEALED) == 1);
   assert(kb_vault_operator_startup_mode(KB_VAULT_OPERATOR_STATE_BACKEND_UNAVAILABLE) == -1);
   assert(kb_vault_operator_startup_mode(KB_VAULT_OPERATOR_STATE_INTEGRITY_FAILURE) == -1);
}

static void test_bounded_frame_fuzz(void)
{
   uint32_t seed = 0x7d3a51u;
   for (unsigned iteration = 0; iteration < 20000; ++iteration)
   {
      unsigned char request[16];
      for (size_t i = 0; i < sizeof(request); ++i)
      {
         seed ^= seed << 13;
         seed ^= seed >> 17;
         seed ^= seed << 5;
         request[i] = (unsigned char)seed;
      }
      uint16_t opcode;
      uint32_t payload_len;
      (void)kb_vault_operator_request_decode(request, &opcode, &payload_len);

      unsigned char response[96];
      for (size_t i = 0; i < sizeof(response); ++i)
      {
         seed ^= seed << 13;
         seed ^= seed >> 17;
         seed ^= seed << 5;
         response[i] = (unsigned char)seed;
      }
      kb_vault_operator_transport_t result;
      kb_vault_operator_status_t status;
      size_t response_len = (size_t)(seed % (sizeof(response) + 1));
      (void)kb_vault_operator_response_decode(response, response_len, &result, &status);
   }
}

static void test_root_socketpair(void)
{
   if (geteuid() != 0)
   {
      /* An unprivileged build runner still proves that the server rejects the
       * peer before parsing or answering an otherwise valid request. */
      int denied_pair[2];
      assert(socketpair(AF_UNIX, SOCK_STREAM, 0, denied_pair) == 0);
      server_arg_t denied = {.fd = denied_pair[1], .result = -99};
      pthread_t denied_thread;
      assert(pthread_create(&denied_thread, NULL, serve, &denied) == 0);
      unsigned char denied_request[16];
      assert(kb_vault_operator_request_encode(KB_VAULT_OPERATOR_OPCODE_STATUS, denied_request) ==
             0);
      /* The send RACES the rejection, and losing that race is the correct
       * behaviour being tested: the server refuses the peer without reading and
       * closes, so our write can land fully, land short, or fail outright with
       * EPIPE/ECONNRESET. Asserting a full write asserted a scheduling order --
       * it held on an idle machine and failed a few percent of the time under
       * CPU contention, which is exactly what a parallel CI test run creates.
       *
       * The invariant is that the peer is rejected and NEVER answered, which the
       * checks below carry. Whether the request reached the socket is not part
       * of it, so only a genuinely unexpected errno fails here. */
      ssize_t sent = write(denied_pair[0], denied_request, sizeof(denied_request));
      assert(sent >= 0 || errno == EPIPE || errno == ECONNRESET);
      /* Likewise ENOTCONN: the half-close is a no-op once the peer is gone. */
      assert(shutdown(denied_pair[0], SHUT_WR) == 0 || errno == ENOTCONN);
      assert(read(denied_pair[0], denied_request, 1) <= 0); /* EOF or reset, never a response. */
      close(denied_pair[0]);
      assert(pthread_join(denied_thread, NULL) == 0 && denied.result == -1);
      return;
   }
   int pair[2];
   assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
   server_arg_t arg = {.fd = pair[1], .result = -99};
   pthread_t thread;
   assert(pthread_create(&thread, NULL, serve, &arg) == 0);
   unsigned char request[16];
   assert(kb_vault_operator_request_encode(KB_VAULT_OPERATOR_OPCODE_STATUS, request) == 0);
   assert(write(pair[0], request, sizeof(request)) == (ssize_t)sizeof(request));
   assert(shutdown(pair[0], SHUT_WR) == 0);
   unsigned char response[96];
   size_t got = 0;
   while (got < sizeof(response))
   {
      ssize_t n = read(pair[0], response + got, sizeof(response) - got);
      assert(n > 0);
      got += (size_t)n;
   }
   assert(read(pair[0], request, 1) == 0);
   close(pair[0]);
   assert(pthread_join(thread, NULL) == 0 && arg.result == 0);
   kb_vault_operator_transport_t result;
   kb_vault_operator_status_t status;
   assert(kb_vault_operator_response_decode(response, got, &result, &status) == 0);
   assert(result == KB_VAULT_OPERATOR_TRANSPORT_OK &&
          status.state == KB_VAULT_OPERATOR_STATE_OPERATIONAL);
}

static size_t root_exchange(const unsigned char *request, size_t request_len,
                            unsigned char response[96])
{
   int pair[2];
   assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
   server_arg_t arg = {.fd = pair[1], .result = -99};
   pthread_t thread;
   assert(pthread_create(&thread, NULL, serve, &arg) == 0);
   assert(write(pair[0], request, request_len) == (ssize_t)request_len);
   assert(shutdown(pair[0], SHUT_WR) == 0);
   size_t got = 0;
   for (;;)
   {
      ssize_t n = read(pair[0], response + got, sizeof(unsigned char[96]) - got);
      if (n == 0)
         break;
      assert(n > 0 && got + (size_t)n <= 96);
      got += (size_t)n;
   }
   close(pair[0]);
   assert(pthread_join(thread, NULL) == 0);
   return got;
}

static void test_root_strict_framing(void)
{
   if (geteuid() != 0)
      return;
   unsigned char request[17];
   unsigned char response[96];
   assert(kb_vault_operator_request_encode(KB_VAULT_OPERATOR_OPCODE_STATUS, request) == 0);
   request[16] = 0xaa;
   size_t got = root_exchange(request, sizeof(request), response);
   kb_vault_operator_transport_t result;
   kb_vault_operator_status_t status;
   assert(got == 16 && kb_vault_operator_response_decode(response, got, &result, &status) == 0 &&
          result == KB_VAULT_OPERATOR_TRANSPORT_BAD_FRAME);

   assert(kb_vault_operator_request_encode(KB_VAULT_OPERATOR_OPCODE_START, request) == 0);
   got = root_exchange(request, 16, response);
   assert(got == 16 && kb_vault_operator_response_decode(response, got, &result, &status) == 0 &&
          result == KB_VAULT_OPERATOR_TRANSPORT_UNSUPPORTED_OPCODE);

   memset(request, 0, 16);
   memcpy(request, "A7VS\0\1", 6); /* opcode zero: valid envelope, unknown operation */
   got = root_exchange(request, 16, response);
   assert(got == 16 && kb_vault_operator_response_decode(response, got, &result, &status) == 0 &&
          result == KB_VAULT_OPERATOR_TRANSPORT_UNSUPPORTED_OPCODE);

   assert(kb_vault_operator_request_encode(KB_VAULT_OPERATOR_OPCODE_STATUS, request) == 0);
   request[0] ^= 1;
   assert(root_exchange(request, 16, response) == 0); /* malformed envelopes are silent */
}

int main(void)
{
   /* Writing to a socket the server has already closed raises SIGPIPE, which
    * kills the process before write() can return the EPIPE the caller handles.
    * That is the same race as the one described in test_root_socketpair, just
    * reaching us as a signal instead of an errno -- under load it showed up as
    * a bare exit 141 with no output at all. */
   signal(SIGPIPE, SIG_IGN);
   test_codec();
   test_bounded_frame_fuzz();
   test_root_socketpair();
   test_root_strict_framing();
   puts("kb_vault_operator_status: all tests passed");
   return 0;
}
