/* Descriptor-owned ABI for DB2's deterministic co-change pairing policy. */
#ifndef AIMEE_DB2_SUPPORT_COCHANGE_H
#define AIMEE_DB2_SUPPORT_COCHANGE_H

#ifdef AIMEE_DB2_COCHANGE_PREFIX
#define cochange_is_hex_sha       db2_support_cochange_is_hex_sha
#define cochange_pairs_for_commit db2_support_cochange_pairs_for_commit
#endif

typedef struct
{
   char a[128];
   char b[128];
} db2_cochange_pair_t;

int cochange_pairs_for_commit(char names[][128], int n, int max_files, db2_cochange_pair_t *out,
                              int out_cap);
int cochange_is_hex_sha(const char *s);

#endif
