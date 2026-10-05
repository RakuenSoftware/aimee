#ifndef AIMEE_SERVER_NATIVE_PRIMITIVE_H
#define AIMEE_SERVER_NATIVE_PRIMITIVE_H

#include "cJSON.h"

/* Project authorized recall into unique source rows. Returns an owned array,
 * bounded to 32 records, or NULL when the view is incomplete or conflicting.
 * The caller retains recall; no storage or authorization is performed here. */
cJSON *server_native_primitive_rows(const cJSON *recall);

#endif
