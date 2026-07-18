/* memory_profile_pack.h: operator-authored configuration bundles for the
 * memory system. Profile packs live in a local directory and are loaded /
 * validated at command time; they do not create new DB tables. */
#ifndef DEC_MEMORY_PROFILE_PACK_H
#define DEC_MEMORY_PROFILE_PACK_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define MEMORY_PROFILE_PACK_NAME_LEN  64
#define MEMORY_PROFILE_PACK_DESC_LEN  256
#define MEMORY_PROFILE_PACK_MAX_KINDS 16
#define MEMORY_PROFILE_PACK_MAX_TIERS 8
#define MEMORY_PROFILE_PACK_MAX_LIST  64

   typedef struct
   {
      char name[MEMORY_PROFILE_PACK_NAME_LEN];
      char description[MEMORY_PROFILE_PACK_DESC_LEN];
      /* Optional narrowing — empty means "all allowed". */
      char allowed_tiers[MEMORY_PROFILE_PACK_MAX_TIERS][4];
      int allowed_tier_count;
      char allowed_kinds[MEMORY_PROFILE_PACK_MAX_KINDS][16];
      int allowed_kind_count;
      /* Optional defaults */
      char default_tier[4];
      char default_visibility[16]; /* "default" | "strict" | "expanded" */
   } memory_profile_pack_t;

   /* Resolve the directory that holds profile packs.
    * Priority: AIMEE_PACK_DIR env var → ~/.config/aimee/packs */
   const char *memory_profile_pack_dir(void);

   /* Validate a profile pack JSON file at |path|.
    * Returns 0 on success.  Writes a human-readable error into |errbuf|. */
   int memory_profile_pack_validate_file(const char *path, char *errbuf, size_t errlen);

   /* Load a named pack from the pack directory into |out|.
    * Returns 0 on success, -1 if not found or invalid. */
   int memory_profile_pack_load(const char *name, memory_profile_pack_t *out, char *errbuf,
                                size_t errlen);

   /* List available pack names into |names|.  Each slot is a NUL-terminated
    * string up to MEMORY_PROFILE_PACK_NAME_LEN.
    * Returns the count of packs found (capped at max). */
   int memory_profile_pack_list(char names[][MEMORY_PROFILE_PACK_NAME_LEN], int max);

   /* Return the name of the currently active pack (from active-pack file),
    * or "default" if none is set.  Writes into |out| (len >= NAME_LEN). */
   void memory_profile_pack_active(char *out, size_t len);

   /* Set the active pack by name.  Writes the name to the active-pack file.
    * Returns 0 on success, -1 on I/O error. */
   int memory_profile_pack_set_active(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* DEC_MEMORY_PROFILE_PACK_H */
