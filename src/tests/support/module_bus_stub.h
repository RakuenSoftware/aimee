/* module_bus_stub.h: control surface for the test event-bus stub.
 * See module_bus_stub.c. */
#ifndef AIMEE_TEST_MODULE_BUS_STUB_H
#define AIMEE_TEST_MODULE_BUS_STUB_H

#include <stdint.h>

#include <aimee/audit/obs_bus.h>

/* Answer every module call with this JSON body. Passing NULL is the same as
 * module_bus_stub_absent(). */
void module_bus_stub_reply(const char *json);

/* Override one event while retaining the ordinary reply for all other events.
 * Used by seams that acquire a capability/key before making their main call. */
void module_bus_stub_reply_for_event(uint32_t event_kind, const char *json);

/* Answer every module call with these exact bytes.
 *
 * module_bus_stub_reply() takes a C string, which can only carry a JSON body --
 * the db1 operation wire is length-prefixed binary and contains zero bytes, so
 * strlen would truncate it at the first field length. A test standing in for
 * the store module needs this one.
 *
 * The bytes are not copied; they must outlive the calls that read them. */
void module_bus_stub_reply_bytes(const void *body, uint32_t len);

/* No module attached: calls short-circuit before reaching the bus. This is the
 * default, so a seam that must fail closed proves it without any setup. */
void module_bus_stub_absent(void);

/* Attached, but the call fails with this transport result. */
void module_bus_stub_fail(aimee_module_call_result_t result);

int module_bus_stub_calls(void);
uint32_t module_bus_stub_last_event(void);
uint32_t module_bus_stub_last_stage(void);
const char *module_bus_stub_last_request(void);

#endif /* AIMEE_TEST_MODULE_BUS_STUB_H */
