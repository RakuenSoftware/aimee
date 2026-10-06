/* Descriptor-owned ABI for KB_STORE's deterministic co-change pairing policy. */
#ifndef AIMEE_KB_STORE_SUPPORT_COCHANGE_H
#define AIMEE_KB_STORE_SUPPORT_COCHANGE_H

#ifdef AIMEE_KB_STORE_COCHANGE_PREFIX
#define cochange_is_hex_sha       kb_store_support_cochange_is_hex_sha
#define cochange_pairs_for_commit kb_store_support_cochange_pairs_for_commit
#endif

typedef struct
{
   char a[128];
   char b[128];
} kb_store_cochange_pair_t;

int cochange_pairs_for_commit(char names[][128], int n, int max_files,
                              kb_store_cochange_pair_t *out, int out_cap);
int cochange_is_hex_sha(const char *s);

#endif
