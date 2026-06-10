#ifndef DEC_AIMEE_H
#define DEC_AIMEE_H 1

#include "platform.h"
#include "aimee_version.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Limits */
#define MAX_PATH_LEN        4096
#define MAX_QUERY_LEN       8192
#define MAX_LINE_LEN        (10 * 1024 * 1024) /* 10MB max JSONL line */
#define MAX_SESSION_RULES   20
#define MAX_SESSION_CHARS   2000
#define MAX_RULE_TEXT_LEN   120
#define MAX_STMT_CACHE      256
#define MAX_FILE_SIZE       (1 << 20) /* 1MB */
#define MAX_CONTEXT_TOTAL   8000
#define MAX_CONTEXT_MEMS    16
#define MAX_MEM_CONTENT_LEN 256
#define CACHE_TTL_SECONDS   300

/* Memory tiers — functional hierarchy:
 *   L0/L1  Experience  raw session logs, atomic actions
 *   L2     Observation deduplicated facts consolidated from experiences
 *   L3     World       slow-changing environment / infrastructure context
 *   L4     Mental Models approved directives injected at highest prompt priority
 *   L5     Patterns    large-scale architectural patterns synthesised across sessions
 *
 * Scope (global / workspace / project) is orthogonal to tier and should
 * never be encoded as an extra tier. */
#define TIER_L0 "L0"
#define TIER_L1 "L1"
#define TIER_L2 "L2"
#define TIER_L3 "L3"
#define TIER_L4 "L4"
#define TIER_L5 "L5"

/* Human-readable functional tier names */
#define TIER_L0_NAME "Experience"
#define TIER_L1_NAME "Experience"
#define TIER_L2_NAME "Observation"
#define TIER_L3_NAME "World"
#define TIER_L4_NAME "MentalModel"
#define TIER_L5_NAME "Pattern"

/* Memory kinds */
#define KIND_FACT       "fact"
#define KIND_PREFERENCE "preference"
#define KIND_DECISION   "decision"
#define KIND_EPISODE    "episode"
#define KIND_TASK       "task"
#define KIND_SCRATCH    "scratch"
#define KIND_PROCEDURE  "procedure"
#define KIND_POLICY     "policy"
#define KIND_WORKFLOW   "workflow"
#define KIND_OPINION    "opinion"
#define KIND_COUNT      10

/* Promotion thresholds */
#define PROMOTE_L1_USE_COUNT  3
#define PROMOTE_L1_CONFIDENCE 0.9
#define DEMOTE_L2_DAYS        60
#define DEMOTE_L2_CONFIDENCE  0.7
#define EXPIRE_L1_DAYS        30
#define CONFLICT_CONF_DECAY   0.7
#define DEDUP_THRESHOLD       0.85
#define GATE_CONFIDENCE_FLOOR 0.7 /* max confidence without evidence markers */

/* Content safety retention limits (days) */
#define RETENTION_RESTRICTED_DAYS 7
#define RETENTION_SENSITIVE_DAYS  90

/* L3 failure episode thresholds */
#define FAILURE_EPISODE_WINDOW 14 /* days to look back for failures */
#define FAILURE_EPISODE_MIN    2  /* minimum failures to trigger episode */

/* Retroactive conflict detection */
#define RETRO_CONFLICT_INTERVAL  86400 /* seconds between scans (1 day) */
#define RETRO_CONFLICT_MAX_PAIRS 200   /* max candidate pairs per scan */
#define RETRO_CONFLICT_MIN_L2    10    /* skip scan if fewer L2 memories */

/* Memory effectiveness */
#define EFFECTIVENESS_DEMOTE_THRESHOLD 0.3
#define EFFECTIVENESS_MIN_SAMPLES      10

/* Embedding retrieval */
#define EMBED_MAX_DIM              1536
#define EMBED_SIMILARITY_THRESHOLD 0.7
#define EMBED_ALPHA                0.5 /* hybrid blend: alpha*lexical + (1-alpha)*embed */
#define EMBED_MAX_OUTPUT           (EMBED_MAX_DIM * 16)

/* Compaction thresholds (days) and term retention */
#define SUMMARY_AGE                30
#define FACT_AGE                   90
#define COMPACT_BASE_TERMS_SUMMARY 10
#define COMPACT_BASE_TERMS_FACT    5
#define COMPACT_FILE_REFS_KEEP     3

/* Task states */
#define TASK_TODO        "todo"
#define TASK_IN_PROGRESS "in_progress"
#define TASK_BLOCKED     "blocked"
#define TASK_DONE        "done"
#define TASK_FAILED      "failed"

/* Guardrail modes */
#define MODE_APPROVE "approve"
#define MODE_PROMPT  "prompt"
#define MODE_DENY    "deny"

/* Session modes */
#define MODE_PLAN      "plan"
#define MODE_IMPLEMENT "implement"

/* Workspace scoping */
#define SHARED_WORKSPACE "_shared"

/* Guardrail severity */
typedef enum
{
   SEV_GREEN = 0,
   SEV_YELLOW,
   SEV_AMBER,
   SEV_RED,
   SEV_BLOCK
} severity_t;

/* Forward declarations */
typedef struct config config_t;

/* Application context (replaces globals, passed through command handlers).
 *
 * No caller-owned DB connection on app_ctx_t: DB1 is owned by the db1 module (db1_init /
 * db1_conn) and DB2 (incl. pgvector) is owned cross-process by aimee-kb.
 * Code that needs DB1 calls db1_init() directly. */
typedef struct
{
   int json_output;
   const char *json_fields;
   const char *response_profile;
   config_t *cfg; /* pre-loaded config (NULL if not available) */
} app_ctx_t;

/* Command registry: each command is a {name, help, handler, tier} entry. */
typedef void (*cmd_handler_t)(app_ctx_t *ctx, int argc, char **argv);

typedef enum
{
   CMD_TIER_CORE,     /* session, hooks, memory, rules, config, index, delegate */
   CMD_TIER_ADVANCED, /* workspace, worktree, trace, jobs, plans, status, work */
   CMD_TIER_ADMIN     /* dashboard, webchat, eval, import/export, db, branch, git */
} cmd_tier_t;

typedef struct
{
   const char *name;
   const char *help;
   cmd_handler_t handler;
   cmd_tier_t tier;
} command_t;

/* Utility: fatal error (exits) */
__attribute__((noreturn, format(printf, 1, 2))) void fatal(const char *fmt, ...);

/* Utility: timestamp */
void now_utc(char *buf, size_t len);

#include "config.h"
#include "util.h"
#include "rules.h"
#include "context_discover.h"
#include "learning.h"
#include "feedback.h"
#include "guardrails.h"
#include "index.h"
#include "memory.h"
#include "tasks.h"
#include "render.h"
#include "notes.h"
#include "collab_rules.h"
#include "agent_outcomes.h"

/* Agent headers are NOT included here. Include the specific
 * narrow header you need in your .c file:
 *   agent.h        - umbrella (includes all agent headers)
 *   agent_types.h  - shared types and constants only
 *   agent_config.h - config loading, routing, auth
 *   agent_exec.h   - execution, context, SSH, policy, metrics
 *   agent_tasks.h  - plan IR, durable jobs, cache
 *   agent_eval.h   - eval harness
 *   agent_coord.h  - coordination, directives, coord-jobs
 *   agent_tools.h  - tool execution, checkpoints
 *   dashboard.h    - dashboard server
 */

#endif /* DEC_AIMEE_H */
