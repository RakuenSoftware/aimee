#ifndef AIMEE_DB2_CODE_IMPORT_H
#define AIMEE_DB2_CODE_IMPORT_H

#include <stddef.h>

int code_path_import_identity(const char *path, char *out, size_t out_cap);
int code_import_identity(const char *importer_path, const char *raw_import, char *out,
                         size_t out_cap);
int code_import_resolves_path(const char *importer_path, const char *raw_import,
                              const char *target_path);

#endif
