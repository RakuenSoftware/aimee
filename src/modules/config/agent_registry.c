/* agent_registry.c: the cached agent registry and the accessors that read it.
 *
 * WHY THIS LIVES IN THE CONFIG MODULE. agent_config_t is 350,968 bytes, and the
 * tree declared it by value 262 times -- on routing, workflow and roundtable
 * paths, i.e. on request threads. Every one of those sites wanted a single
 * 16,720-byte agent and paid a stat(), a 343 KB memset and a 343 KB memcpy to
 * get it, then churned the copy through a per-thread malloc arena. That is the
 * same mistake config_t made before it moved behind accessors, which is why
 * check-config-encapsulation exists and why the fix here has the same shape:
 * the registry is a secret, callers ask for the value they want.
 *
 * The cache itself moved with the accessors deliberately. It is one unit -- the
 * lock, the stat-identity key, and the readers -- and splitting it would leave
 * the invariant ("no unlocked read of the shared registry") spread across two
 * files and two layers. src/server/agent_config.c keeps the parsing and saving
 * and publishes into this cache through the small API in agent_registry.h. */
#include "agent_registry.h"

#include <pthread.h>
#include <string.h>

/* The cache is read/written from many threads at once -- every delegate dispatch
 * loads the registry, and the parallel autonomy scheduler plus the panel seats
 * dispatch concurrently. An unguarded memcpy of this multi-KB struct while a
 * reloading thread rewrites it is a torn read (and TSan-class UB), so all cache
 * access holds g_cache_lock. The lock is NOT held across file I/O parsing: a
 * reloader parses into its own buffer first and only then publishes. */
static agent_config_t g_cache;
static struct timespec g_mtime;
/* mtime alone is not a safe cache key. It is not monotonic and not always
 * distinct: a rewritten agents.json can land with a timestamp equal to (or older
 * than) the cached one, and the cache then serves stale content forever. Observed
 * live on the tiered appliance filesystem, where a freshly installed agents.json
 * arrived with an mtime ~9h in the past and /v1/agents kept failing until the
 * file was touched. Size and inode are free from the same stat() and make an
 * in-place rewrite detectable. */
static off_t g_size;
static ino_t g_ino;
static int g_cached;
static pthread_mutex_t g_cache_lock = PTHREAD_MUTEX_INITIALIZER;

static struct timespec stat_mtime(const struct stat *st)
{
   struct timespec ts;
#if defined(__APPLE__)
   ts = st->st_mtimespec;
#elif defined(_WIN32) || defined(_WIN64)
   ts.tv_sec = st->st_mtime;
   ts.tv_nsec = 0;
#elif defined(__linux__)
   ts = st->st_mtim;
#else
   ts.tv_sec = st->st_mtime;
   ts.tv_nsec = 0;
#endif
   return ts;
}

static int mtime_eq(const struct timespec *a, const struct timespec *b)
{
   return a->tv_sec == b->tv_sec && a->tv_nsec == b->tv_nsec;
}

/* A cache hit requires the file to look identical on every cheap axis stat()
 * gives us: same mtime, same size, same inode. */
static int stat_eq(const struct stat *st)
{
   struct timespec mt = stat_mtime(st);
   return mtime_eq(&mt, &g_mtime) && st->st_size == g_size && st->st_ino == g_ino;
}

int agent_registry_cache_get(const struct stat *st, agent_config_t *cfg)
{
   if (!st || !cfg)
      return -1;
   pthread_mutex_lock(&g_cache_lock);
   int hit = g_cached && stat_eq(st);
   if (hit)
      memcpy(cfg, &g_cache, sizeof(*cfg));
   pthread_mutex_unlock(&g_cache_lock);
   return hit ? 0 : -1;
}

void agent_registry_cache_put(const agent_config_t *cfg, const struct stat *st)
{
   if (!cfg || !st)
      return;
   pthread_mutex_lock(&g_cache_lock);
   memcpy(&g_cache, cfg, sizeof(g_cache));
   g_mtime = stat_mtime(st);
   g_size = st->st_size;
   g_ino = st->st_ino;
   g_cached = 1;
   pthread_mutex_unlock(&g_cache_lock);
}

void agent_registry_cache_invalidate(void)
{
   pthread_mutex_lock(&g_cache_lock);
   g_cached = 0;
   pthread_mutex_unlock(&g_cache_lock);
}

int agent_registry_pick_cached(const struct stat *st, agent_t *out,
                               agent_t *(*pick)(agent_config_t *, const void *), const void *arg)
{
   if (!st || !out || !pick)
      return -1;
   pthread_mutex_lock(&g_cache_lock);
   int rc = -1;
   if (g_cached && stat_eq(st))
   {
      /* `pick` runs with the lock HELD and must only select and copy: no I/O, no
       * allocation, no reentry into the loader. Everything it needs is here. */
      agent_t *found = pick(&g_cache, arg);
      if (found)
      {
         memcpy(out, found, sizeof(*out));
         rc = 0;
      }
      else
      {
         rc = 1; /* cache was current; this registry simply has no such agent */
      }
   }
   pthread_mutex_unlock(&g_cache_lock);
   return rc;
}
