#include <aimee/core/event_bus/module_runtime.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXAMPLE_KIND 4200u

static uint32_t parse_identity(const char *text)
{
   char *end = NULL;
   errno = 0;
   unsigned long value = text ? strtoul(text, &end, 10) : 0;
   return !errno && end && !*end && value <= UINT32_MAX ? (uint32_t)value : UINT32_MAX;
}

static aimee_module_status_t handle(const aimee_module_invocation_t *invocation,
                                    const uint8_t *request, uint32_t request_len, uint8_t *response,
                                    uint32_t response_capacity, uint32_t *response_len,
                                    void *user_data)
{
   (void)user_data;
   if (aimee_module_invocation_cancelled(invocation))
      return AIMEE_MODULE_STATUS_CANCELLED;
   if (request_len > response_capacity)
      return AIMEE_MODULE_STATUS_INTERNAL;
   memcpy(response, request, request_len);
   *response_len = request_len;
   return AIMEE_MODULE_STATUS_OK;
}

int main(int argc, char **argv)
{
   if (argc != 4)
   {
      fprintf(stderr, "usage: %s ATTACH_SOCKET PRINCIPAL_CLASS PRINCIPAL_REF\n", argv[0]);
      return 2;
   }
   uint32_t principal_class = parse_identity(argv[2]);
   uint32_t principal_ref = parse_identity(argv[3]);
   if (principal_class == UINT32_MAX || principal_ref == UINT32_MAX)
      return 2;

   static const aimee_module_stage_t stages[] = {{EXAMPLE_KIND, 1u}};
   const aimee_module_process_config_t config = {.socket_path = argv[1],
                                                 .module_name = "example",
                                                 .principal_class = principal_class,
                                                 .principal_ref = principal_ref,
                                                 .stages = stages,
                                                 .stage_count = 1,
                                                 .handler = handle};
   return aimee_module_process_run(&config);
}
