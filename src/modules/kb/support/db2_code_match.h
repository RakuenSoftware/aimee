/* Descriptor-owned ABI for DB2 code-search line enrichment. */
#ifndef AIMEE_DB2_SUPPORT_CODE_MATCH_H
#define AIMEE_DB2_SUPPORT_CODE_MATCH_H

#ifdef AIMEE_DB2_CODE_MATCH_PREFIX
#define code_match_line db2_support_code_match_line
#endif

int code_match_line(const char *content, const char *marked_snippet);

#endif
