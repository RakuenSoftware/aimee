#ifndef AIMEE_TEST_DB2_RANDOM_IO_SEAM_H
#define AIMEE_TEST_DB2_RANDOM_IO_SEAM_H

#include <stdio.h>

FILE *db2_test_fopen(const char *path, const char *mode);
size_t db2_test_fread(void *buf, size_t size, size_t count, FILE *stream);
int db2_test_fclose(FILE *stream);

#define fopen  db2_test_fopen
#define fread  db2_test_fread
#define fclose db2_test_fclose

#endif
