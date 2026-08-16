/* Descriptor-owned ABI for DB2's UTF-8 repair support. */
#ifndef AIMEE_DB2_SUPPORT_TEXT_H
#define AIMEE_DB2_SUPPORT_TEXT_H

#include <stddef.h>

size_t text_sanitize_utf8(char *s);

#endif
