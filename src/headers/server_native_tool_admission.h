#ifndef AIMEE_SERVER_NATIVE_TOOL_ADMISSION_H
#define AIMEE_SERVER_NATIVE_TOOL_ADMISSION_H
#include "cJSON.h"
#include <stddef.h>

/* Host preflight for an isolated native callback, after all callable aliases
 * resolve. NULL permits dispatch; otherwise returns a stable audit reason and
 * writes the refusal detail. Does not grant effect or human authorization. */
const char *server_native_tool_admit(const char *tool, const cJSON *arguments, const char *session,
                                     char *error, size_t error_capacity);
/* Host-supplied logical attempt after independently verified original-source
 * claiming. Never pass SDK/model idempotency fields here. */
const char *server_native_tool_admit_attempt(const char *tool, const cJSON *arguments,
                                             const char *session, const char *attempt, char *error,
                                             size_t error_capacity);
#endif
