#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <aimee/core/event_bus/module_protocol.h>

static void must(int condition, const char *message)
{
   if (!condition)
   {
      fprintf(stderr, "FAIL: %s\n", message);
      abort();
   }
}

int main(void)
{
   aimee_module_message_t input = {.operation = AIMEE_MODULE_OP_INVOKE,
                                   .stage_id = 0x1701,
                                   .body_len = 3,
                                   .deadline_ns = 900,
                                   .trace_id = 42};
   uint8_t bytes[AIMEE_MODULE_MESSAGE_HEADER_LEN + 3];
   memset(bytes, 0, sizeof bytes);
   must(aimee_module_message_encode(&input, bytes, sizeof bytes) == AIMEE_MODULE_MESSAGE_HEADER_LEN,
        "encode invoke");
   memcpy(bytes + AIMEE_MODULE_MESSAGE_HEADER_LEN, "abc", 3);

   aimee_module_message_t output;
   memset(&output, 0xa5, sizeof output);
   must(aimee_module_message_decode(bytes, sizeof bytes, &output) == AIMEE_MODULE_MESSAGE_OK,
        "decode invoke");
   must(output.operation == input.operation && output.stage_id == input.stage_id &&
            output.body_len == input.body_len && output.deadline_ns == input.deadline_ns &&
            output.trace_id == input.trace_id,
        "round trip fields");
   must(!aimee_module_deadline_expired(900, 899), "deadline before boundary");
   must(aimee_module_deadline_expired(900, 900), "deadline at boundary");
   must(!aimee_module_deadline_expired(0, UINT64_MAX), "zero deadline disabled");

   aimee_module_message_t untouched;
   memset(&untouched, 0x5a, sizeof untouched);
   aimee_module_message_t snapshot = untouched;
   bytes[20] = 4;
   must(aimee_module_message_decode(bytes, sizeof bytes, &untouched) ==
            AIMEE_MODULE_MESSAGE_ERR_BODY,
        "reject truncated body");
   must(memcmp(&untouched, &snapshot, sizeof untouched) == 0,
        "failed decode leaves output untouched");

   input.operation = AIMEE_MODULE_OP_RESULT;
   input.status = AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
   input.body_len = 0;
   must(aimee_module_message_encode(&input, bytes, sizeof bytes) == AIMEE_MODULE_MESSAGE_HEADER_LEN,
        "encode typed capability absence");
   puts("test_module_protocol: ok");
   return 0;
}
