/* Descriptor-owned ABI for KB_STORE's dynamic-string support subset. */
#ifndef AIMEE_KB_STORE_SUPPORT_DSTR_H
#define AIMEE_KB_STORE_SUPPORT_DSTR_H

#include <stddef.h>

typedef struct
{
   char *data;
   size_t len;
   size_t cap;
} dstr_t;

void dstr_init(dstr_t *s);
__attribute__((format(printf, 2, 3))) void dstr_appendf(dstr_t *s, const char *fmt, ...);
char *dstr_steal(dstr_t *s);

#endif
