#ifndef AIMEE_KB_STORE_RANDOM_H
#define AIMEE_KB_STORE_RANDOM_H

#include <stddef.h>

int platform_random_bytes(void *buf, size_t len);
int platform_random_hex(char *out, size_t hex_len);

#endif
