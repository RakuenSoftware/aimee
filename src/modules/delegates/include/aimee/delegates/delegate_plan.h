#ifndef DEC_DELEGATE_PLAN_H
#define DEC_DELEGATE_PLAN_H 1

#include <stddef.h>
#include "cJSON.h"

/* Build a read-only delegate work-packet plan from proposal markdown.
 * Caller owns the returned cJSON object. */
cJSON *delegate_plan_build_from_text(const char *source_path, const char *proposal_text,
                                     char *errbuf, size_t errbuf_len);

/* Read proposal_path and build a delegate work-packet plan.
 * Caller owns the returned cJSON object. */
cJSON *delegate_plan_build_from_file(const char *proposal_path, char *errbuf, size_t errbuf_len);

#endif /* DEC_DELEGATE_PLAN_H */
