#ifndef AIMEE_PROVIDERS_CLIENT_H
#define AIMEE_PROVIDERS_CLIENT_H
#include "cJSON.h"
/* ABI transport only. The returned object is owned by the caller. */
cJSON *providers_module_request(const char *operation, cJSON *arguments, const char *actor,
                                int secret_write_allowed);
#endif
