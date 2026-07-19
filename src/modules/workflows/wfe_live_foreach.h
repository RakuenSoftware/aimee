/* wfe_live_foreach.h -- the live foreach.workflow child spawner (sliced-lifecycle
 * build). Fans a parent run's split packet-plan out to one child "slice" work item
 * per packet, linked via the DB parent<->child linkage. */
#ifndef DEC_WFE_LIVE_FOREACH_H
#define DEC_WFE_LIVE_FOREACH_H 1

#include <stddef.h>

/* Core spawn: create one `child_workflow` child work item per packet in the split
 * packet-plan JSON at `packets_path` ({"packets":[{packet_id,summary,...}]}), each
 * linked to `parent_wi` (db1_work_item_set_parent) and seeded with the packet as its
 * proposal. Idempotent: if children already exist, returns their count without
 * re-spawning. `packets_path` NULL/absent/empty-plan -> 0 children. `max_children`
 * bounds the fan-out (an over-cap plan is refused -> -1). Returns the number of
 * children after the call, or -1 on a fatal error (fills `err`). Exposed for the
 * unit test. */
int wfe_foreach_spawn(const char *parent_wi, const char *child_workflow, const char *packets_path,
                      int max_children, char *err, size_t errlen);

/* Register the live foreach spawn provider (called from wfe_autonomy_register). */
void wfe_live_foreach_register(void);

#endif /* DEC_WFE_LIVE_FOREACH_H */
