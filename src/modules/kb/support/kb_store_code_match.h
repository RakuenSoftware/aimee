/* Descriptor-owned ABI for KB_STORE code-search line enrichment. */
#ifndef AIMEE_KB_STORE_SUPPORT_CODE_MATCH_H
#define AIMEE_KB_STORE_SUPPORT_CODE_MATCH_H

#ifdef AIMEE_KB_STORE_CODE_MATCH_PREFIX
#define code_match_line kb_store_support_code_match_line
#endif

int code_match_line(const char *content, const char *marked_snippet);

#endif
