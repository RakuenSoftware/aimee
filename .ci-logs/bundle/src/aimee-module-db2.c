#include <aimee/core/event_bus/module_runtime.h>

#include <stdio.h>

extern aimee_module_status_t aimee_module_handler(
    const aimee_module_invocation_t *, const uint8_t *, uint32_t, uint8_t *, uint32_t,
    uint32_t *, void *);


static const aimee_module_stage_t stages[] = {
   {11521u, 1u},
};

int main(int argc, char **argv)
{
   if (argc != 2)
   {
      fprintf(stderr, "usage: %s DAEMON_MODULE_BUS_SOCKET\n", argv[0]);
      return 2;
   }
   const aimee_module_process_config_t config = {
       .socket_path = argv[1],
       .module_name = "db2",
       .principal_class = 1u,
       .principal_ref = 29u,
       .stages = stages,
       .stage_count = sizeof stages / sizeof stages[0],
       .handler = aimee_module_handler,
   };
   return aimee_module_process_run(&config);
}
