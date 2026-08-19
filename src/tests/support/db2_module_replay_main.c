#include <aimee/core/event_bus/module_runtime.h>
#include <aimee/db2/module_api.h>

#include <stdio.h>

extern aimee_module_status_t aimee_module_handler(const aimee_module_invocation_t *,
                                                  const uint8_t *, uint32_t, uint8_t *, uint32_t,
                                                  uint32_t *, void *);
extern int aimee_db2_module_init(void);

/* Test-only registration for cataloged families that have not reached the
 * atomic production activation gate. The exported DB2 source closure and
 * handler are otherwise identical to the packaged production process.
 *
 * One entry per family, because a stage is a family: every operation inside
 * one arrives on the same stage and is told apart by its operation number. */
static const aimee_module_stage_t stages[] = {
    {AIMEE_DB2_EVENT_LIFECYCLE, AIMEE_DB2_FAMILY_LIFECYCLE},
    {AIMEE_DB2_EVENT_MEMORY, AIMEE_DB2_FAMILY_MEMORY},
    {AIMEE_DB2_EVENT_INDEX, AIMEE_DB2_FAMILY_INDEX},
    {AIMEE_DB2_EVENT_LEARNING, AIMEE_DB2_FAMILY_LEARNING},
    {AIMEE_DB2_EVENT_ORGANIZATION, AIMEE_DB2_FAMILY_ORGANIZATION},
    {AIMEE_DB2_EVENT_CUSTODY, AIMEE_DB2_FAMILY_CUSTODY},
    {AIMEE_DB2_EVENT_MAINTENANCE, AIMEE_DB2_FAMILY_MAINTENANCE},
};

int main(int argc, char **argv)
{
   if (argc != 2)
   {
      fprintf(stderr, "usage: %s DAEMON_MODULE_BUS_SOCKET\n", argv[0]);
      return 2;
   }
   if (aimee_db2_module_init() != 0)
   {
      fprintf(stderr, "db2: aimee_db2_module_init failed; refusing to serve\n");
      return 1;
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
