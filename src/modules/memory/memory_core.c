/* _GNU_SOURCE: strcasestr/memmem are GNU extensions; declare them before any
 * libc header so gcc-12 (the container toolchain) does not implicit-decl + -Werror. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
/* memory_core.c: high-level memory orchestration, context assembly, scoring,
 * and DB2 (incl. pgvector) retrieval routing. */
#include "aimee.h"
#include "memory_context_internal.h"
#include "memory_rewrite_llm.h" /* weak in-process rewrite seam (KB build only) */

#include "memory_core_internal.h"

#include "db1_optional.h"
#include "modules/db2/c/entity_edges.h"
#include "modules/db2/c/kb_runtime_state.h"
#include "modules/db2/c/memory_health.h"
#include "modules/db2/c/memory_payload.h"
#include "modules/db2/c/feature_rows.h"
#include "modules/db2/c/memory_promotion.h"
#include "modules/db2/c/memory_query.h"
#include "memory_graph_fusion.h"
#include "kb_mdl.h"
#include "modules/db2/c/memory_relations.h"
#include "modules/db2/c/memory_scenes.h"
#include "modules/db2/c/stopwords.h"
#include "modules/db2/c/vector_index_ops.h"
#include "modules/db2/c/vector_verify.h"
#include "memory_vectors.h"
#include "lifecycle.h"
#include "platform_process.h"
#include "memory_platform.h"
#include "log.h"
#include "util.h"       /* util_now_ms — memory.search stage timing */
#include "agent_exec.h" /* agent_http_post: in-process HTTP embedding (no fork) */
#include "cJSON.h"
#include "dogfood.h"
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <unistd.h>

/* Regex patterns, gate functions, and content scanning are platform-owned. */

#include <pthread.h>

pthread_mutex_t s_memory_pagerank_stats_mu = PTHREAD_MUTEX_INITIALIZER;
memory_pagerank_runtime_stats_t s_memory_pagerank_stats;

/* Keep the monolith split by responsibility without changing linkage.
 * The included fragments share this translation unit private state. */
