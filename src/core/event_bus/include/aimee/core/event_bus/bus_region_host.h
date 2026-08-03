#ifndef AIMEE_CORE_EVENT_BUS_REGION_HOST_H
#define AIMEE_CORE_EVENT_BUS_REGION_HOST_H 1

#include <aimee/core/event_bus/bus_region.h>

/* Host-only region lifecycle. External modules link the client archive, which
 * deliberately does not provide these symbols. */
bus_region_result_t bus_region_create(const char *name, size_t size, bus_region_t *out);
bus_region_result_t bus_control_init(bus_region_t *r, uint32_t slot_size, uint32_t inline_budget,
                                     uint32_t queue_capacity, uint64_t arena_size);
void bus_control_bump_epoch(bus_control_t *c);
void bus_control_heartbeat(bus_control_t *c, uint64_t now);
bus_region_result_t bus_qpair_init(bus_region_t *r, uint32_t slot_size, uint32_t capacity);
bus_region_result_t bus_arena_region_init(bus_region_t *r, uint64_t arena_size);

#endif
