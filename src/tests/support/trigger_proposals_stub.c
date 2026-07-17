/* trigger_proposals_stub.c: no-op stub for trigger_proposals_file_one (the real one lives
 * in trigger_scheduler.c, which drags in the wfe engine + db1 subsystems). Tests that link
 * server_trigger.o — whose handle_trigger_fire references trigger_proposals_file_one — but
 * exercise only HTTP dispatch/routing, link this instead of the heavyweight scheduler TU.
 * Binaries that exercise the real proposals fire link trigger_scheduler.o and must NOT also
 * link this TU. */
#include "server_trigger.h"

int trigger_proposals_file_one(const char *workspace, const char *pipeline, const char *event_dir,
                               const char *ref, const char *mode, const char *proposal_name,
                               char out_id[80])
{
   (void)workspace;
   (void)pipeline;
   (void)event_dir;
   (void)ref;
   (void)mode;
   (void)proposal_name;
   if (out_id)
      out_id[0] = '\0';
   return -1;
}
