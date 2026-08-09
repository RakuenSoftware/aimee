/* module_bus_stub.h: control surface for the test event-bus stub.
 * See module_bus_stub.c. */
#ifndef AIMEE_TEST_MODULE_BUS_STUB_H
#define AIMEE_TEST_MODULE_BUS_STUB_H

#include <stdint.h>

#include <aimee/audit/obs_bus.h>

/* Answer every module call with this JSON body. Passing NULL is the same as
 * module_bus_stub_absent(). */
void module_bus_stub_reply(const char *json);

/* No module attached: calls short-circuit before reaching the bus. This is the
 * default, so a seam that must fail closed proves it without any setup. */
void module_bus_stub_absent(void);

/* Attached, but the call fails with this transport result. */
void module_bus_stub_fail(aimee_module_call_result_t result);

int module_bus_stub_calls(void);
uint32_t module_bus_stub_last_event(void);
uint32_t module_bus_stub_last_stage(void);

#endif /* AIMEE_TEST_MODULE_BUS_STUB_H */
