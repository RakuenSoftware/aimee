#include <string.h>

#include <aimee/core/event_bus/module_protocol.h>

static void put_u16(uint8_t *p, uint16_t value)
{
   p[0] = (uint8_t)value;
   p[1] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t *p, uint32_t value)
{
   for (unsigned i = 0; i < 4; ++i)
      p[i] = (uint8_t)(value >> (8u * i));
}

static void put_u64(uint8_t *p, uint64_t value)
{
   for (unsigned i = 0; i < 8; ++i)
      p[i] = (uint8_t)(value >> (8u * i));
}

static uint16_t get_u16(const uint8_t *p)
{
   return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}

static uint32_t get_u32(const uint8_t *p)
{
   uint32_t value = 0;
   for (unsigned i = 0; i < 4; ++i)
      value |= (uint32_t)p[i] << (8u * i);
   return value;
}

static uint64_t get_u64(const uint8_t *p)
{
   uint64_t value = 0;
   for (unsigned i = 0; i < 8; ++i)
      value |= (uint64_t)p[i] << (8u * i);
   return value;
}

static aimee_module_message_result_t validate(const aimee_module_message_t *message)
{
   if (message->operation != AIMEE_MODULE_OP_INVOKE && message->operation != AIMEE_MODULE_OP_RESULT)
      return AIMEE_MODULE_MESSAGE_ERR_OPERATION;
   if (message->status > AIMEE_MODULE_STATUS_INTERNAL)
      return AIMEE_MODULE_MESSAGE_ERR_STATUS;
   if (message->operation == AIMEE_MODULE_OP_INVOKE && message->status != AIMEE_MODULE_STATUS_OK)
      return AIMEE_MODULE_MESSAGE_ERR_STATUS;
   if (message->stage_id == 0)
      return AIMEE_MODULE_MESSAGE_ERR_STAGE;
   if (message->flags != 0)
      return AIMEE_MODULE_MESSAGE_ERR_FLAGS;
   if (message->body_len > AIMEE_MODULE_MESSAGE_MAX_BODY)
      return AIMEE_MODULE_MESSAGE_ERR_BODY;
   return AIMEE_MODULE_MESSAGE_OK;
}

size_t aimee_module_message_encode(const aimee_module_message_t *message, uint8_t *output,
                                   size_t output_len)
{
   if (!message || !output || output_len < AIMEE_MODULE_MESSAGE_HEADER_LEN ||
       validate(message) != AIMEE_MODULE_MESSAGE_OK)
      return 0;
   memset(output, 0, AIMEE_MODULE_MESSAGE_HEADER_LEN);
   put_u32(output, AIMEE_MODULE_MESSAGE_MAGIC);
   put_u16(output + 4, AIMEE_MODULE_MESSAGE_VERSION);
   put_u16(output + 6, AIMEE_MODULE_MESSAGE_HEADER_LEN);
   put_u16(output + 8, message->operation);
   put_u16(output + 10, message->status);
   put_u32(output + 12, message->stage_id);
   put_u32(output + 16, message->flags);
   put_u32(output + 20, message->body_len);
   put_u64(output + 24, message->deadline_ns);
   put_u64(output + 32, message->trace_id);
   return AIMEE_MODULE_MESSAGE_HEADER_LEN;
}

aimee_module_message_result_t aimee_module_message_decode(const uint8_t *input, size_t input_len,
                                                          aimee_module_message_t *message)
{
   if (!input || !message || input_len < AIMEE_MODULE_MESSAGE_HEADER_LEN)
      return AIMEE_MODULE_MESSAGE_ERR_SHORT;
   if (get_u32(input) != AIMEE_MODULE_MESSAGE_MAGIC)
      return AIMEE_MODULE_MESSAGE_ERR_MAGIC;
   if (get_u16(input + 4) != AIMEE_MODULE_MESSAGE_VERSION)
      return AIMEE_MODULE_MESSAGE_ERR_VERSION;
   if (get_u16(input + 6) != AIMEE_MODULE_MESSAGE_HEADER_LEN)
      return AIMEE_MODULE_MESSAGE_ERR_HEADER;

   aimee_module_message_t decoded;
   memset(&decoded, 0, sizeof decoded);
   decoded.operation = get_u16(input + 8);
   decoded.status = get_u16(input + 10);
   decoded.stage_id = get_u32(input + 12);
   decoded.flags = get_u32(input + 16);
   decoded.body_len = get_u32(input + 20);
   decoded.deadline_ns = get_u64(input + 24);
   decoded.trace_id = get_u64(input + 32);
   aimee_module_message_result_t result = validate(&decoded);
   if (result != AIMEE_MODULE_MESSAGE_OK)
      return result;
   if ((size_t)decoded.body_len > input_len - AIMEE_MODULE_MESSAGE_HEADER_LEN)
      return AIMEE_MODULE_MESSAGE_ERR_BODY;
   *message = decoded;
   return AIMEE_MODULE_MESSAGE_OK;
}

int aimee_module_deadline_expired(uint64_t deadline_ns, uint64_t now_ns)
{
   return deadline_ns != 0 && now_ns >= deadline_ns;
}

const char *aimee_module_message_result_name(aimee_module_message_result_t result)
{
   static const char *const names[] = {"OK",         "ERR_SHORT",     "ERR_MAGIC",  "ERR_VERSION",
                                       "ERR_HEADER", "ERR_OPERATION", "ERR_STATUS", "ERR_STAGE",
                                       "ERR_FLAGS",  "ERR_BODY"};
   return (unsigned)result < sizeof names / sizeof names[0] ? names[result] : "ERR_UNKNOWN";
}
