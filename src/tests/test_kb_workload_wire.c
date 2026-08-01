#include "kb/kb_workload_wire.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct
{
   unsigned char bytes[KB_WORKLOAD_WIRE_FRAME_MAX + 1];
   size_t len;
} frame_t;

static void u32be(unsigned char out[4], uint32_t value)
{
   out[0] = (unsigned char)(value >> 24);
   out[1] = (unsigned char)(value >> 16);
   out[2] = (unsigned char)(value >> 8);
   out[3] = (unsigned char)value;
}

static void field(frame_t *frame, const void *data, size_t len)
{
   assert(len <= UINT32_MAX && frame->len + 4 + len <= sizeof(frame->bytes));
   u32be(frame->bytes + frame->len, (uint32_t)len);
   frame->len += 4;
   memcpy(frame->bytes + frame->len, data, len);
   frame->len += len;
}

static frame_t response(kb_workload_operation_t op, kb_workload_result_t status)
{
   frame_t frame = {0};
   memcpy(frame.bytes, "AIMEEWI1", 8);
   frame.bytes[8] = (unsigned char)op;
   frame.bytes[9] = (unsigned char)status;
   frame.len = KB_WORKLOAD_WIRE_HEADER_LEN;
   if (status == KB_WORKLOAD_OK)
   {
      unsigned char token[] = "signed.jwt";
      unsigned char proof_anchor[32], custody_anchor[32], proof[8], data[33];
      memset(proof_anchor, 0x11, sizeof(proof_anchor));
      memset(custody_anchor, 0x22, sizeof(custody_anchor));
      memset(proof, 0x30, sizeof(proof));
      memset(data, 0x44, sizeof(data));
      field(&frame, token, sizeof(token) - 1);
      field(&frame, proof_anchor, sizeof(proof_anchor));
      field(&frame, custody_anchor, sizeof(custody_anchor));
      field(&frame, proof, sizeof(proof));
      if (op != KB_WORKLOAD_OP_ATTEST)
         field(&frame, data, sizeof(data));
   }
   u32be(frame.bytes + 12, (uint32_t)(frame.len - KB_WORKLOAD_WIRE_HEADER_LEN));
   return frame;
}

static int zeroed(const void *data, size_t len)
{
   const unsigned char *bytes = data;
   for (size_t i = 0; i < len; i++)
      if (bytes[i] != 0)
         return 0;
   return 1;
}

static void test_requests(void)
{
   unsigned char challenge[32], binding[32], data[64], frame[256];
   memset(challenge, 0x11, sizeof(challenge));
   memset(binding, 0x22, sizeof(binding));
   memset(data, 0x33, sizeof(data));
   size_t len = 99;
   assert(kb_workload_wire_build_request(KB_WORKLOAD_OP_ATTEST, challenge, binding, NULL, 0, frame,
                                         sizeof(frame), &len) == 0);
   assert(len == 88 && memcmp(frame, "AIMEEWI1\1\0\0\0\0\0\0H", 16) == 0);
   assert(frame[16] == 0 && frame[19] == 32 && memcmp(frame + 20, challenge, 32) == 0);

   assert(kb_workload_wire_build_request(KB_WORKLOAD_OP_WRAP, challenge, binding, data,
                                         sizeof(data), frame, sizeof(frame), &len) == 0);
   assert(len == 156 && frame[8] == KB_WORKLOAD_OP_WRAP && frame[15] == 140);
   assert(kb_workload_wire_build_request(KB_WORKLOAD_OP_UNWRAP, challenge, binding, data,
                                         sizeof(data), frame, sizeof(frame), &len) == 0);
   assert(frame[8] == KB_WORKLOAD_OP_UNWRAP);

   len = 55;
   assert(kb_workload_wire_build_request(KB_WORKLOAD_OP_ATTEST, challenge, binding, data, 1, frame,
                                         sizeof(frame), &len) == -1 &&
          len == 0);
   assert(kb_workload_wire_build_request(KB_WORKLOAD_OP_WRAP, challenge, binding, NULL, 1, frame,
                                         sizeof(frame), &len) == -1);
   assert(kb_workload_wire_build_request(KB_WORKLOAD_OP_WRAP, challenge, binding, data, 0, frame,
                                         sizeof(frame), &len) == -1);
   assert(kb_workload_wire_build_request((kb_workload_operation_t)4, challenge, binding, data, 1,
                                         frame, sizeof(frame), &len) == -1);
   assert(kb_workload_wire_build_request(KB_WORKLOAD_OP_WRAP, challenge, binding, data,
                                         sizeof(data), frame, 20, &len) == -1);
}

static void test_responses(void)
{
   for (int op = KB_WORKLOAD_OP_ATTEST; op <= KB_WORKLOAD_OP_UNWRAP; op++)
   {
      frame_t frame = response((kb_workload_operation_t)op, KB_WORKLOAD_OK);
      kb_workload_wire_response_t parsed;
      assert(kb_workload_wire_parse_response(frame.bytes, frame.len, (kb_workload_operation_t)op,
                                             &parsed) == 0);
      assert(parsed.operation == (kb_workload_operation_t)op && parsed.status == KB_WORKLOAD_OK &&
             parsed.token.len == 10 && parsed.proof_anchor_id.len == 32 &&
             parsed.custody_anchor_id.len == 32 && parsed.proof.len == 8);
      assert(parsed.data.len == (op == KB_WORKLOAD_OP_ATTEST ? 0U : 33U));
   }
   for (int status = KB_WORKLOAD_DISABLED; status <= KB_WORKLOAD_INTEGRITY; status++)
   {
      frame_t frame = response(KB_WORKLOAD_OP_WRAP, (kb_workload_result_t)status);
      kb_workload_wire_response_t parsed;
      assert(kb_workload_wire_parse_response(frame.bytes, frame.len, KB_WORKLOAD_OP_WRAP,
                                             &parsed) == 0);
      assert(parsed.status == (kb_workload_result_t)status && !parsed.token.ptr &&
             !parsed.data.ptr);
   }
}

static void reject(frame_t frame, kb_workload_operation_t op)
{
   kb_workload_wire_response_t parsed;
   memset(&parsed, 0xa5, sizeof(parsed));
   assert(kb_workload_wire_parse_response(frame.bytes, frame.len, op, &parsed) == -1);
   assert(zeroed(&parsed, sizeof(parsed)));
}

static void test_malformed(void)
{
   frame_t good = response(KB_WORKLOAD_OP_WRAP, KB_WORKLOAD_OK);
   for (size_t n = 0; n < good.len; n++)
   {
      frame_t cut = good;
      cut.len = n;
      reject(cut, KB_WORKLOAD_OP_WRAP);
   }
   frame_t bad = good;
   bad.bytes[bad.len++] = 0;
   reject(bad, KB_WORKLOAD_OP_WRAP);
   bad = good;
   bad.bytes[0] ^= 1;
   reject(bad, KB_WORKLOAD_OP_WRAP);
   bad = good;
   bad.bytes[8] = KB_WORKLOAD_OP_UNWRAP;
   reject(bad, KB_WORKLOAD_OP_WRAP);
   bad = good;
   bad.bytes[9] = KB_WORKLOAD_INVALID;
   reject(bad, KB_WORKLOAD_OP_WRAP);
   bad = good;
   bad.bytes[10] = 1;
   reject(bad, KB_WORKLOAD_OP_WRAP);
   bad = good;
   memset(bad.bytes + 12, 0xff, 4);
   reject(bad, KB_WORKLOAD_OP_WRAP);

   bad = response(KB_WORKLOAD_OP_WRAP, KB_WORKLOAD_DISABLED);
   bad.bytes[15] = 1;
   bad.bytes[bad.len++] = 0;
   reject(bad, KB_WORKLOAD_OP_WRAP);

   /* Token zero, fixed anchor wrong, proof too short, missing data, extra field. */
   bad = good;
   memset(bad.bytes + 16, 0, 4);
   reject(bad, KB_WORKLOAD_OP_WRAP);
   bad = good;
   bad.bytes[30] = 0;
   bad.bytes[31] = 31; /* first anchor length starts after 4+10 token bytes */
   reject(bad, KB_WORKLOAD_OP_WRAP);
   bad = good;
   bad.bytes[105] = 7; /* proof length offset */
   reject(bad, KB_WORKLOAD_OP_WRAP);
   bad = good;
   bad.len -= 37;
   u32be(bad.bytes + 12, (uint32_t)(bad.len - 16));
   reject(bad, KB_WORKLOAD_OP_WRAP);
   bad = good;
   bad.bytes[bad.len++] = 0;
   u32be(bad.bytes + 12, (uint32_t)(bad.len - 16));
   reject(bad, KB_WORKLOAD_OP_WRAP);
}

int main(void)
{
   test_requests();
   test_responses();
   test_malformed();
   puts("kb_workload_wire: ok");
   return 0;
}
