#ifndef KB_EGRESS_ADMISSION_H
#define KB_EGRESS_ADMISSION_H
#include <stdint.h>
#include <stddef.h>
typedef struct { const char *origin_cn,*request_id,*model,*cred_slot; int64_t team,project; int has_project; int64_t pricing_version; const char *reserve_max; } kb_egress_admission_t;
typedef int (*kb_egress_dispatch_fn)(void *, char *, size_t, char *, size_t);
int kb_egress_admit_dispatch(const kb_egress_admission_t *, kb_egress_dispatch_fn, void *, char *, size_t);
#endif
