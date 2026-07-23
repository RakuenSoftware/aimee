#include "kb_vault_operator_status.h"

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct
{
   int calls;
   int post_wipe_calls;
   int status_calls;
   int fail;
   int post_wipe_fail;
   unsigned sequence;
   unsigned mutate_order;
   unsigned post_wipe_order;
   unsigned status_order;
   kb_vault_operator_opcode_t opcode;
   uint8_t request_id[16];
   kb_vault_operator_result_t result;
} fixture_t;

static kb_vault_operator_status_t operational(void)
{
   kb_vault_operator_status_t status = {.state = KB_VAULT_OPERATOR_STATE_OPERATIONAL,
                                        .operation_state = KB_VAULT_OPERATOR_OPERATION_NONE,
                                        .remediation = KB_VAULT_OPERATOR_REMEDIATION_NONE,
                                        .seal_epoch = 3,
                                        .control_fence = 4,
                                        .last_opened_fence = 2};
   return status;
}

static int mutate(kb_vault_operator_opcode_t opcode, const uint8_t request_id[16],
                  const uint8_t *secret, size_t secret_len, kb_vault_operator_result_t *result,
                  void *opaque)
{
   fixture_t *f = opaque;
   ++f->calls;
   f->mutate_order = ++f->sequence;
   f->opcode = opcode;
   assert(secret && secret_len == 6 && !memcmp(secret, "s3cret", 6));
   if (opcode == KB_VAULT_OPERATOR_OPCODE_START)
   {
      assert(request_id);
      memcpy(f->request_id, request_id, 16);
   }
   else
      assert(!request_id);
   *result = f->result;
   return f->fail ? -1 : 0;
}

static int post_wipe(kb_vault_operator_opcode_t opcode, kb_vault_operator_result_t result,
                     void *opaque)
{
   fixture_t *f = opaque;
   ++f->post_wipe_calls;
   f->post_wipe_order = ++f->sequence;
   assert(f->calls == 1 && f->status_calls == 0);
   assert(opcode == f->opcode && result == f->result);
   return f->post_wipe_fail ? -1 : 0;
}

static int read_status(kb_vault_operator_status_t *status, void *opaque)
{
   fixture_t *f = opaque;
   ++f->status_calls;
   f->status_order = ++f->sequence;
   *status = operational();
   return 0;
}

typedef struct
{
   int fd;
   int served;
   fixture_t *fixture;
} server_arg_t;

static void *serve(void *opaque)
{
   server_arg_t *arg = opaque;
   arg->served = kb_vault_operator_serve_connection_mutations_ex(arg->fd, read_status, mutate,
                                                                 post_wipe, arg->fixture);
   close(arg->fd);
   return NULL;
}

static size_t exchange(fixture_t *fixture, const uint8_t *request, size_t request_len,
                       uint8_t response[112])
{
   int pair[2];
   assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
   server_arg_t arg = {.fd = pair[1], .served = -99, .fixture = fixture};
   pthread_t thread;
   assert(pthread_create(&thread, NULL, serve, &arg) == 0);
   assert(write(pair[0], request, request_len) == (ssize_t)request_len);
   assert(shutdown(pair[0], SHUT_WR) == 0);
   size_t got = 0;
   for (;;)
   {
      ssize_t n = read(pair[0], response + got, 112 - got);
      if (!n)
         break;
      assert(n > 0 && got + (size_t)n <= 112);
      got += (size_t)n;
   }
   close(pair[0]);
   assert(pthread_join(thread, NULL) == 0);
   return got;
}

static size_t make_request(kb_vault_operator_opcode_t opcode, uint8_t out[64])
{
   uint8_t request_id[16];
   memset(request_id, 0x7d, sizeof(request_id));
   size_t prefix_len = 0;
   assert(kb_vault_operator_mutation_request_prefix_encode(
              opcode, opcode == KB_VAULT_OPERATOR_OPCODE_START ? request_id : NULL, 6, out, 64,
              &prefix_len) == 0);
   memcpy(out + prefix_len, "s3cret", 6);
   return prefix_len + 6;
}

static void test_codecs(void)
{
   uint8_t parsed[16];
   assert(kb_vault_operator_request_id_parse("00112233445566778899aabbccddeeff", parsed) == 0);
   assert(parsed[0] == 0 && parsed[15] == 0xff);
   assert(kb_vault_operator_request_id_parse("00112233445566778899AABBCCDDEEFF", parsed) == -1);
   assert(kb_vault_operator_request_id_parse("0011", parsed) == -1);
   uint8_t request[64];
   size_t request_len = make_request(KB_VAULT_OPERATOR_OPCODE_START, request);
   assert(request_len == 42 && request[7] == 2 && request[11] == 26 && request[35] == 6);
   size_t ignored = 9;
   assert(kb_vault_operator_mutation_request_prefix_encode(
              KB_VAULT_OPERATOR_OPCODE_START, NULL, 6, request, sizeof(request), &ignored) == -1 &&
          ignored == 0);
   assert(kb_vault_operator_mutation_request_prefix_encode(
              KB_VAULT_OPERATOR_OPCODE_RESUME, NULL, 0, request, sizeof(request), &ignored) == -1);

   kb_vault_operator_status_t status = operational(), decoded;
   uint8_t response[112];
   size_t response_len = 0;
   assert(kb_vault_operator_mutation_response_encode(KB_VAULT_OPERATOR_RESULT_OPERATIONAL, &status,
                                                     response, sizeof(response),
                                                     &response_len) == 0 &&
          response_len == 112);
   kb_vault_operator_result_t result;
   assert(kb_vault_operator_mutation_response_decode(response, response_len, &result, &decoded) ==
          0);
   assert(result == KB_VAULT_OPERATOR_RESULT_OPERATIONAL &&
          decoded.state == KB_VAULT_OPERATOR_STATE_OPERATIONAL);
   char formatted[512];
   assert(kb_vault_operator_mutation_format(result, &decoded, 0, formatted, sizeof(formatted)) > 0);
   assert(!strcmp(formatted,
                  "result=operational vault=operational remediation=none operation=none\n"));
   assert(kb_vault_operator_mutation_format(result, &decoded, 1, formatted, sizeof(formatted)) > 0);
   assert(strstr(formatted, "\"result\":\"operational\"") && !strstr(formatted, "secret"));
   response[18] = 1;
   assert(kb_vault_operator_mutation_response_decode(response, response_len, &result, &decoded) ==
          -1);

   /* Attempt-local integrity must remain representable while the durable
    * barrier correctly remains completed_sealed. */
   status = (kb_vault_operator_status_t){.state = KB_VAULT_OPERATOR_STATE_COMPLETED_SEALED,
                                         .operation_state = KB_VAULT_OPERATOR_OPERATION_COMPLETED,
                                         .remediation = KB_VAULT_OPERATOR_REMEDIATION_FINALIZE,
                                         .flags = 1,
                                         .seal_epoch = 3,
                                         .control_fence = 4,
                                         .old_generation = 7,
                                         .new_generation = 8,
                                         .last_opened_fence = 2};
   memset(status.operation_id, 0x44, sizeof(status.operation_id));
   assert(kb_vault_operator_mutation_response_encode(KB_VAULT_OPERATOR_RESULT_INTEGRITY_FAILURE,
                                                     &status, response, sizeof(response),
                                                     &response_len) == 0);
   assert(kb_vault_operator_mutation_response_decode(response, response_len, &result, &decoded) ==
              0 &&
          result == KB_VAULT_OPERATOR_RESULT_INTEGRITY_FAILURE &&
          decoded.state == KB_VAULT_OPERATOR_STATE_COMPLETED_SEALED);
}

static void test_start_and_resume(void)
{
   if (geteuid() != 0)
      return;
   for (kb_vault_operator_opcode_t opcode = KB_VAULT_OPERATOR_OPCODE_START;
        opcode <= KB_VAULT_OPERATOR_OPCODE_UNSEAL; ++opcode)
   {
      fixture_t fixture = {.result = KB_VAULT_OPERATOR_RESULT_OPERATIONAL};
      uint8_t request[64], response[112];
      size_t request_len = make_request(opcode, request);
      size_t got = exchange(&fixture, request, request_len, response);
      assert(got == 112 && fixture.calls == 1 && fixture.post_wipe_calls == 1 &&
             fixture.status_calls == 1 && fixture.opcode == opcode);
      assert(fixture.mutate_order < fixture.post_wipe_order &&
             fixture.post_wipe_order < fixture.status_order);
      if (opcode == KB_VAULT_OPERATOR_OPCODE_START)
         for (size_t i = 0; i < 16; ++i)
            assert(fixture.request_id[i] == 0x7d);
      kb_vault_operator_result_t result;
      kb_vault_operator_status_t status;
      assert(kb_vault_operator_mutation_response_decode(response, got, &result, &status) == 0 &&
             result == KB_VAULT_OPERATOR_RESULT_OPERATIONAL);
   }
}

static void test_bad_frames_and_callback_failure(void)
{
   if (geteuid() != 0)
      return;
   uint8_t request[64], response[112];
   fixture_t fixture = {.result = KB_VAULT_OPERATOR_RESULT_OPERATIONAL};
   size_t request_len = make_request(KB_VAULT_OPERATOR_OPCODE_UNSEAL, request);
   request[request_len - 1] = 0;
   size_t got = exchange(&fixture, request, request_len, response);
   kb_vault_operator_transport_t transport;
   kb_vault_operator_status_t status;
   assert(got == 16 && fixture.calls == 0 && fixture.post_wipe_calls == 0 &&
          fixture.status_calls == 0 &&
          kb_vault_operator_response_decode(response, got, &transport, &status) == 0 &&
          transport == KB_VAULT_OPERATOR_TRANSPORT_BAD_FRAME);

   request_len = make_request(KB_VAULT_OPERATOR_OPCODE_UNSEAL, request);
   fixture.fail = 1;
   got = exchange(&fixture, request, request_len, response);
   assert(got == 16 && fixture.calls == 1 && fixture.post_wipe_calls == 0 &&
          fixture.status_calls == 0 &&
          kb_vault_operator_response_decode(response, got, &transport, &status) == 0 &&
          transport == KB_VAULT_OPERATOR_TRANSPORT_INTERNAL);

   request_len = make_request(KB_VAULT_OPERATOR_OPCODE_UNSEAL, request);
   fixture = (fixture_t){.result = KB_VAULT_OPERATOR_RESULT_OPERATIONAL, .post_wipe_fail = 1};
   got = exchange(&fixture, request, request_len, response);
   assert(got == 16 && fixture.calls == 1 && fixture.post_wipe_calls == 1 &&
          fixture.status_calls == 0 && fixture.mutate_order < fixture.post_wipe_order &&
          kb_vault_operator_response_decode(response, got, &transport, &status) == 0 &&
          transport == KB_VAULT_OPERATOR_TRANSPORT_INTERNAL);
}

int main(void)
{
   test_codecs();
   test_start_and_resume();
   test_bad_frames_and_callback_failure();
   puts("kb_vault_operator_mutation: all tests passed");
   return 0;
}
