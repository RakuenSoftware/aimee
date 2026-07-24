/* kb_workload_wire.c: allocation-free strict helper frame codec. */
#include "kb_workload_wire.h"

#include <limits.h>
#include <string.h>

static const unsigned char k_magic[8] = {'A', 'I', 'M', 'E', 'E', 'W', 'I', '1'};

static void put_u32be(unsigned char out[4], uint32_t value)
{
   out[0] = (unsigned char)(value >> 24);
   out[1] = (unsigned char)(value >> 16);
   out[2] = (unsigned char)(value >> 8);
   out[3] = (unsigned char)value;
}

static uint32_t get_u32be(const unsigned char in[4])
{
   return ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) | ((uint32_t)in[2] << 8) |
          (uint32_t)in[3];
}

static int valid_operation(kb_workload_operation_t operation)
{
   return operation == KB_WORKLOAD_OP_ATTEST || operation == KB_WORKLOAD_OP_WRAP ||
          operation == KB_WORKLOAD_OP_UNWRAP;
}

static int valid_request_data(kb_workload_operation_t operation, const void *data, size_t len)
{
   if (operation == KB_WORKLOAD_OP_ATTEST)
      return len == 0;
   if (!data || len == 0)
      return 0;
   return operation == KB_WORKLOAD_OP_WRAP ? len <= KB_WORKLOAD_WIRE_PLAIN_MAX
                                           : len <= KB_WORKLOAD_WIRE_CIPHER_MAX;
}

static void put_field(unsigned char *out, size_t *offset, const void *data, size_t len)
{
   put_u32be(out + *offset, (uint32_t)len);
   *offset += 4;
   if (len)
   {
      memcpy(out + *offset, data, len);
      *offset += len;
   }
}

int kb_workload_wire_build_request(kb_workload_operation_t operation,
                                   const unsigned char challenge[KB_WORKLOAD_CHALLENGE_LEN],
                                   const unsigned char binding[KB_WORKLOAD_BINDING_LEN],
                                   const void *data, size_t data_len, unsigned char *out,
                                   size_t cap, size_t *out_len)
{
   if (out_len)
      *out_len = 0;
   if (!out_len || !out || !challenge || !binding || !valid_operation(operation) ||
       !valid_request_data(operation, data, data_len) || data_len > UINT32_MAX)
      return -1;

   size_t fields = operation == KB_WORKLOAD_OP_ATTEST ? 2U : 3U;
   size_t payload_len =
       fields * 4U + KB_WORKLOAD_CHALLENGE_LEN + KB_WORKLOAD_BINDING_LEN + data_len;
   if (payload_len > KB_WORKLOAD_WIRE_PAYLOAD_MAX ||
       cap < KB_WORKLOAD_WIRE_HEADER_LEN + payload_len)
      return -1;

   memcpy(out, k_magic, sizeof(k_magic));
   out[8] = (unsigned char)operation;
   out[9] = out[10] = out[11] = 0;
   put_u32be(out + 12, (uint32_t)payload_len);
   size_t offset = KB_WORKLOAD_WIRE_HEADER_LEN;
   put_field(out, &offset, challenge, KB_WORKLOAD_CHALLENGE_LEN);
   put_field(out, &offset, binding, KB_WORKLOAD_BINDING_LEN);
   if (operation != KB_WORKLOAD_OP_ATTEST)
      put_field(out, &offset, data, data_len);
   *out_len = offset;
   return 0;
}

static int take_field(const unsigned char **cursor, size_t *remaining, size_t min, size_t max,
                      kb_workload_wire_view_t *out)
{
   if (*remaining < 4)
      return -1;
   uint32_t len = get_u32be(*cursor);
   *cursor += 4;
   *remaining -= 4;
   if ((size_t)len < min || (size_t)len > max || (size_t)len > *remaining)
      return -1;
   out->ptr = *cursor;
   out->len = len;
   *cursor += len;
   *remaining -= len;
   return 0;
}

int kb_workload_wire_parse_response(const unsigned char *frame, size_t frame_len,
                                    kb_workload_operation_t expected_operation,
                                    kb_workload_wire_response_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof(*out));
   if (!frame || !valid_operation(expected_operation) || frame_len < KB_WORKLOAD_WIRE_HEADER_LEN ||
       frame_len > KB_WORKLOAD_WIRE_FRAME_MAX || memcmp(frame, k_magic, sizeof(k_magic)) != 0 ||
       frame[8] != (unsigned char)expected_operation || frame[10] != 0 || frame[11] != 0)
      return -1;

   uint8_t status = frame[9];
   uint32_t payload_len = get_u32be(frame + 12);
   if (status > KB_WORKLOAD_INTEGRITY || payload_len > KB_WORKLOAD_WIRE_PAYLOAD_MAX ||
       (size_t)payload_len != frame_len - KB_WORKLOAD_WIRE_HEADER_LEN ||
       (status != KB_WORKLOAD_OK && payload_len != 0))
      return -1;

   out->operation = expected_operation;
   out->status = (kb_workload_result_t)status;
   if (status != KB_WORKLOAD_OK)
      return 0;

   const unsigned char *cursor = frame + KB_WORKLOAD_WIRE_HEADER_LEN;
   size_t remaining = payload_len;
   if (take_field(&cursor, &remaining, 1, KB_WORKLOAD_WIRE_TOKEN_MAX, &out->token) != 0 ||
       take_field(&cursor, &remaining, KB_WORKLOAD_ANCHOR_LEN, KB_WORKLOAD_ANCHOR_LEN,
                  &out->proof_anchor_id) != 0 ||
       take_field(&cursor, &remaining, KB_WORKLOAD_ANCHOR_LEN, KB_WORKLOAD_ANCHOR_LEN,
                  &out->custody_anchor_id) != 0 ||
       take_field(&cursor, &remaining, KB_WORKLOAD_WIRE_PROOF_MIN, KB_WORKLOAD_WIRE_PROOF_MAX,
                  &out->proof) != 0)
      goto malformed;

   if (expected_operation == KB_WORKLOAD_OP_WRAP &&
       take_field(&cursor, &remaining, 1, KB_WORKLOAD_WIRE_CIPHER_MAX, &out->data) != 0)
      goto malformed;
   if (expected_operation == KB_WORKLOAD_OP_UNWRAP &&
       take_field(&cursor, &remaining, 1, KB_WORKLOAD_WIRE_PLAIN_MAX, &out->data) != 0)
      goto malformed;
   if (remaining != 0)
      goto malformed;
   return 0;

malformed:
   memset(out, 0, sizeof(*out));
   return -1;
}
