/* config_internal.h: shared between config.c (load) and config_save.c (save).
 *
 * Not a public API — callers outside the config module should use config.h. */
#ifndef DEC_CONFIG_INTERNAL_H
#define DEC_CONFIG_INTERNAL_H 1

#include "aimee.h"
#include <sys/stat.h>
#include <time.h>

/* Portable accessor for the nanosecond-precision mtime in `struct stat`. */
#if defined(__APPLE__)
#define AIMEE_STAT_MTIM(st) ((st).st_mtimespec)
#elif defined(__linux__)
#define AIMEE_STAT_MTIM(st) ((st).st_mtim)
#elif defined(__unix__)
#define AIMEE_STAT_MTIM(st) ((st).st_mtim)
#else
#define AIMEE_STAT_MTIM(st) ((struct timespec){.tv_sec = (st).st_mtime, .tv_nsec = 0})
#endif

/* Serialize a mcp transport enum back to its YAML/JSON string form.
 * Defined in config.c alongside the parser. */
const char *config_mcp_transport_to_string(config_mcp_transport_t transport);

/* File-identity-keyed load cache, shared so config_save can stamp freshly-saved
 * values and short-circuit the next config_load() call. Keyed on mtime + size +
 * inode: mtime alone is spoofable by a same-timestamp (or clock-skewed) rewrite,
 * which the tiered appliance filesystem produces in practice — see the agents.json
 * fix in #1372. */
extern config_t g_config_cache;
extern struct timespec g_config_mtime;
extern off_t g_config_size;
extern ino_t g_config_ino;
extern char g_config_cache_path[MAX_PATH_LEN];
extern int g_config_cached;

/* THE file-identity key for every cached loader in this module.
 *
 * mtime alone is not safe: it is neither monotonic nor always distinct, so an
 * in-place rewrite can land with an equal (or older) timestamp and the cache then
 * serves stale content forever. Size and inode are free from the same stat() and
 * make that rewrite detectable. This was open-coded three times -- config_load_file,
 * the seed watcher, and the agent registry -- which is three chances to get one
 * subtle rule wrong. */
typedef struct
{
   struct timespec mtime;
   off_t size;
   ino_t ino;
} config_file_id_t;

void config_file_id_from(const struct stat *st, config_file_id_t *out);
int config_file_id_eq(const config_file_id_t *a, const config_file_id_t *b);

#endif /* DEC_CONFIG_INTERNAL_H */
