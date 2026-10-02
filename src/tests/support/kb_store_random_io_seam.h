#ifndef AIMEE_TEST_KB_STORE_RANDOM_IO_SEAM_H
#define AIMEE_TEST_KB_STORE_RANDOM_IO_SEAM_H

#include <stdio.h>

FILE *kb_store_test_fopen(const char *path, const char *mode);
size_t kb_store_test_fread(void *buf, size_t size, size_t count, FILE *stream);
int kb_store_test_fclose(FILE *stream);

#define fopen  kb_store_test_fopen
#define fread  kb_store_test_fread
#define fclose kb_store_test_fclose

#endif
