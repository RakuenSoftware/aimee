#include <aimee/core/event_bus/module_runtime.h>

#include <stdio.h>

extern aimee_module_status_t aimee_module_handler(
    const aimee_module_invocation_t *, const uint8_t *, uint32_t, uint8_t *, uint32_t,
    uint32_t *, void *);

extern int aimee_db1_module_init(void);


static const aimee_module_stage_t stages[] = {
   {11777u, 1u},
   {11778u, 2u},
   {11779u, 3u},
   {11780u, 4u},
   {11781u, 5u},
   {11782u, 6u},
   {11783u, 7u},
   {11784u, 8u},
};

int main(int argc, char **argv)
{
   if (argc != 2)
   {
      fprintf(stderr, "usage: %s DAEMON_MODULE_BUS_SOCKET\n", argv[0]);
      return 2;
   }
   if (aimee_db1_module_init() != 0)
   {
      fprintf(stderr, "db1: aimee_db1_module_init failed; refusing to serve\n");
      return 1;
   }
   const aimee_module_process_config_t config = {
       .socket_path = argv[1],
       .module_name = "db1",
       .principal_class = 1u,
       .principal_ref = 30u,
       .stages = stages,
       .stage_count = sizeof stages / sizeof stages[0],
       .handler = aimee_module_handler,
   };
   return aimee_module_process_run(&config);
}
