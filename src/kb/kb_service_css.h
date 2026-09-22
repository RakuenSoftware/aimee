#ifndef AIMEE_KB_SERVICE_CSS_H
#define AIMEE_KB_SERVICE_CSS_H

#include "cJSON.h"

/* KB host transport to the Go memory owner. Returns an owned JSON receipt,
 * or NULL when the owner is unavailable or its response is invalid. */
cJSON *db2_kb_service_css_conventions_json(const char *project, int sync);

#endif
