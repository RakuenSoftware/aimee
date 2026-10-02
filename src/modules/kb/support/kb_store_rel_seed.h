#ifndef AIMEE_KB_STORE_REL_SEED_H
#define AIMEE_KB_STORE_REL_SEED_H

/* Descriptor-private mirror of rel_type_def_t's C ABI. The parity suite binds
 * every size, offset, enum value, and seed field to the authoritative type.
 * Keeping project ontology headers out of the standalone bundle is deliberate:
 * KB_STORE owns this generated, database-free seed copy rather than importing the
 * memory module or the monolithic rel_types translation unit. */
#define KB_STORE_REL_TYPE_NAME_MAX  64
#define KB_STORE_REL_TYPE_MAX_KINDS 8

typedef struct
{
   const char *rel_type;
   int head_kinds[KB_STORE_REL_TYPE_MAX_KINDS];
   int head_kind_count;
   int tail_kinds[KB_STORE_REL_TYPE_MAX_KINDS];
   int tail_kind_count;
   int is_symmetric;
   const char *inverse_rel_type;
   int correction_behavior;
   const char *category;
   int sensitivity;
   int is_hierarchy_rel;
   int status;
} kb_store_rel_seed_def_t;

int rel_types_seed_count(void);
const kb_store_rel_seed_def_t *rel_types_seed_at(int i);
const kb_store_rel_seed_def_t *rel_types_seed_lookup(const char *rel_type);

#endif
