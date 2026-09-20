#ifndef DEC_KB_CLIENT_MEMORY_INTERNAL_H
#define DEC_KB_CLIENT_MEMORY_INTERNAL_H 1

#include "kb_client.h"
#include "cJSON.h"

int64_t kbc_memory_response_id(const cJSON *object);

void kbc_memory_row_from_json(cJSON *f, memory_t *m);

#endif
