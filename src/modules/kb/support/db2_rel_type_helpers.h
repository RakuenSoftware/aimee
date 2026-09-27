#ifndef AIMEE_DB2_REL_TYPE_HELPERS_H
#define AIMEE_DB2_REL_TYPE_HELPERS_H

#include <stddef.h>

void rel_type_normalize(const char *in, char *out, size_t out_len);
int rel_type_is_functional(const char *rel_type);

#endif
