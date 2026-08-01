/* fuzz_kb_workload_wire.c: pure AIMEEWI1 response-parser/request-builder fuzzing.
 *
 * Build examples:
 *   libFuzzer: clang -fsanitize=fuzzer,address,undefined ...
 *   standalone: cc -DFUZZ_STANDALONE ...
 */
#include "kb/kb_workload_wire.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FUZZ_INPUT_MAX (KB_WORKLOAD_WIRE_FRAME_MAX + 1U)

static int all_zero(const void *data, size_t len)
{
   const unsigned char *bytes = data;
   for (size_t i = 0; i < len; i++)
      if (bytes[i] != 0)
         return 0;
   return 1;
}

static int view_in_input(kb_workload_wire_view_t view, const unsigned char *input, size_t size)
{
   if (view.len == 0)
      return view.ptr == NULL;
   uintptr_t base = (uintptr_t)input;
   uintptr_t ptr = (uintptr_t)view.ptr;
   if (!input || ptr < base)
      return 0;
   size_t offset = (size_t)(ptr - base);
   return offset <= size && view.len <= size - offset;
}

static void fuzz_responses(const unsigned char *data, size_t size)
{
   for (int op = KB_WORKLOAD_OP_ATTEST; op <= KB_WORKLOAD_OP_UNWRAP; op++)
   {
      kb_workload_wire_response_t response;
      memset(&response, 0xa5, sizeof(response));
      int rc = kb_workload_wire_parse_response(data, size, (kb_workload_operation_t)op, &response);
      assert(rc == 0 || rc == -1);
      if (rc == -1)
      {
         assert(all_zero(&response, sizeof(response)));
         continue;
      }

      assert(response.operation == (kb_workload_operation_t)op);
      assert(response.status >= KB_WORKLOAD_OK && response.status <= KB_WORKLOAD_INTEGRITY);
      assert(view_in_input(response.token, data, size));
      assert(view_in_input(response.proof_anchor_id, data, size));
      assert(view_in_input(response.custody_anchor_id, data, size));
      assert(view_in_input(response.proof, data, size));
      assert(view_in_input(response.data, data, size));
      if (response.status != KB_WORKLOAD_OK)
      {
         assert(!response.token.ptr && !response.proof_anchor_id.ptr &&
                !response.custody_anchor_id.ptr && !response.proof.ptr && !response.data.ptr);
         continue;
      }
      assert(response.token.len >= 1 && response.token.len <= KB_WORKLOAD_WIRE_TOKEN_MAX);
      assert(response.proof_anchor_id.len == KB_WORKLOAD_ANCHOR_LEN);
      assert(response.custody_anchor_id.len == KB_WORKLOAD_ANCHOR_LEN);
      assert(response.proof.len >= KB_WORKLOAD_WIRE_PROOF_MIN &&
             response.proof.len <= KB_WORKLOAD_WIRE_PROOF_MAX);
      if (op == KB_WORKLOAD_OP_ATTEST)
         assert(!response.data.ptr && response.data.len == 0);
      else if (op == KB_WORKLOAD_OP_WRAP)
         assert(response.data.len >= 1 && response.data.len <= KB_WORKLOAD_WIRE_CIPHER_MAX);
      else
         assert(response.data.len >= 1 && response.data.len <= KB_WORKLOAD_WIRE_PLAIN_MAX);
   }
}

static void fuzz_requests(const unsigned char *data, size_t size)
{
   unsigned char challenge[KB_WORKLOAD_CHALLENGE_LEN] = {0};
   unsigned char binding[KB_WORKLOAD_BINDING_LEN] = {0};
   size_t challenge_n = size < sizeof(challenge) ? size : sizeof(challenge);
   if (challenge_n)
      memcpy(challenge, data, challenge_n);
   if (size > challenge_n)
   {
      size_t binding_n = size - challenge_n;
      if (binding_n > sizeof(binding))
         binding_n = sizeof(binding);
      memcpy(binding, data + challenge_n, binding_n);
   }

   size_t data_offset = size < 64 ? size : 64;
   const unsigned char *field_data = data_offset < size ? data + data_offset : NULL;
   size_t available = size - data_offset;
   size_t lengths[4] = {0, available, available ? 1U : 0U,
                        available > 0 ? (size_t)data[0] % (available + 1U) : 0U};

   for (int op = 0; op <= 4; op++)
      for (size_t li = 0; li < sizeof(lengths) / sizeof(lengths[0]); li++)
      {
         unsigned char output[KB_WORKLOAD_WIRE_FRAME_MAX + 1U];
         memset(output, 0xa5, sizeof(output));
         size_t cap = KB_WORKLOAD_WIRE_FRAME_MAX;
         if (size > 1)
            cap = ((size_t)data[1] << 8 | data[0]) % (KB_WORKLOAD_WIRE_FRAME_MAX + 1U);
         size_t out_len = SIZE_MAX;
         int rc = kb_workload_wire_build_request((kb_workload_operation_t)op, challenge, binding,
                                                 field_data, lengths[li], output, cap, &out_len);
         assert(rc == 0 || rc == -1);
         assert(output[cap] == 0xa5); /* the builder never writes at or beyond cap */
         if (rc == -1)
         {
            assert(out_len == 0);
            continue;
         }
         assert(out_len >= KB_WORKLOAD_WIRE_HEADER_LEN && out_len <= cap &&
                out_len <= KB_WORKLOAD_WIRE_FRAME_MAX);
         assert(memcmp(output, "AIMEEWI1", 8) == 0);
         assert(output[8] == (unsigned char)op && output[9] == 0 && output[10] == 0 &&
                output[11] == 0);
         uint32_t payload = ((uint32_t)output[12] << 24) | ((uint32_t)output[13] << 16) |
                            ((uint32_t)output[14] << 8) | output[15];
         assert((size_t)payload == out_len - KB_WORKLOAD_WIRE_HEADER_LEN);
      }
}

static void fuzz_one(const unsigned char *data, size_t size)
{
   if ((!data && size) || size > FUZZ_INPUT_MAX)
      return;
   fuzz_responses(data, size);
   fuzz_requests(data, size);
}

#ifndef FUZZ_STANDALONE
int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size)
{
   fuzz_one(data, size);
   return 0;
}
#else
static int fuzz_file(const char *path)
{
   FILE *file = fopen(path, "rb");
   if (!file)
   {
      fprintf(stderr, "Cannot open: %s\n", path);
      return 1;
   }
   if (fseek(file, 0, SEEK_END) != 0)
   {
      fclose(file);
      return 1;
   }
   long length = ftell(file);
   if (length < 0 || (unsigned long)length > FUZZ_INPUT_MAX || fseek(file, 0, SEEK_SET) != 0)
   {
      fclose(file);
      return length < 0 ? 1 : 0;
   }
   size_t size = (size_t)length;
   unsigned char *data = malloc(size ? size : 1U);
   if (!data)
   {
      fclose(file);
      return 1;
   }
   size_t read_len = size ? fread(data, 1, size, file) : 0;
   fclose(file);
   if (read_len != size)
   {
      free(data);
      return 1;
   }
   fuzz_one(data, size);
   free(data);
   return 0;
}

int main(int argc, char **argv)
{
   if (argc < 2)
   {
      unsigned char input[FUZZ_INPUT_MAX];
      size_t size = fread(input, 1, sizeof(input), stdin);
      fuzz_one(input, size);
   }
   else
      for (int i = 1; i < argc; i++)
         if (fuzz_file(argv[i]) != 0)
            return 1;
   printf("fuzz_kb_workload_wire: %d inputs ok\n", argc > 1 ? argc - 1 : 1);
   return 0;
}
#endif
