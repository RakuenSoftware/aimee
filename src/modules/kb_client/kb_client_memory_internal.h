#ifndef DEC_KB_CLIENT_MEMORY_INTERNAL_H
#define DEC_KB_CLIENT_MEMORY_INTERNAL_H 1

#include "kb_client.h"
#include "cJSON.h"

char *kb_v1_action_request(const char *method, cJSON *req);
void kbc_memory_row_from_json(cJSON *f, memory_t *m);

#endif
