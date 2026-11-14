#ifndef AIMEE_RUNTIME_CODE_INDEX_H
#define AIMEE_RUNTIME_CODE_INDEX_H
/* Current repository snapshots are runtime state, not personal memory. */
/* Additive runtime operation; existing shared DB1 ABI stays byte-identical. */
#define AIMEE_DB1_OP_RUNTIME_CODE_INDEX 67u
char *db1_runtime_code_index(const char *request_json);
#endif
