/* Wire contract for the DB1 process's bounded stages.
 *
 * GENERATED from src/modules/db1/eventcontract/operations.json by
 * scripts/gen_db1_contract.py. Do not edit: add a family or an operation to the
 * catalog and regenerate, so the numbering and the wire cannot drift apart.
 *
 * DB1 is the server's SQLite store. It is becoming a module so that callers
 * reach it over the event bus instead of linking it, which is what the module
 * doctrine requires of state. The C implementation stays for now; only the
 * boundary is new. See docs/proposals/pending/db1-as-a-go-module.md.
 *
 * Event kinds are fixed by the process contract at 4096 + ref*256 + stage. DB1
 * declares principal ref 30, so these are not a free choice. */
#ifndef AIMEE_DB1_MODULE_API_H
#define AIMEE_DB1_MODULE_API_H 1

#include <stddef.h>
#include <stdint.h>

/* Family 1: the economizer's per-conversation reducer state. Chosen as the
 * first family because it has exactly one production caller, so it proved the
 * boundary without a wide cutover.
 *
 * Request:  op(u32) | key_len(u32) | key | json_len(u32) | json
 * Response: status(u32) | json_len(u32) | json
 * Lengths are little-endian, matching the rest of the bus surface. */

#define AIMEE_DB1_EVENT_ECONOMIZER_STATE 11777u
#define AIMEE_DB1_STAGE_ECONOMIZER_STATE 1u

#define AIMEE_DB1_OP_STATE_LOAD 1u
#define AIMEE_DB1_OP_STATE_SAVE 2u

/* Family 2: branch ownership for the MCP git flows. Rows say which session
 * owns which branch, so concurrent local sessions do not stomp on each other.
 *
 * Request:  op(u32) | field_count(u32) | (len(u32) | bytes) * field_count
 * Response: status(u32) | field_count(u32) | (len(u32) | bytes) * field_count
 *
 * Counted in both directions. The first family fixed its request at exactly two
 * fields, which suits a keyed blob and suits nothing with three, so the count is
 * explicit here rather than implied by the op.
 *
 * The reply counts for the same reason the request does: an operation that
 * answers with a row, or with a list of them, has somewhere to put the values.
 * A reply carrying nothing sends a count of zero, and one carrying a single
 * value sends a count of one -- the shape does not change with the arity. */

#define AIMEE_DB1_EVENT_GIT_OWNERSHIP 11778u
#define AIMEE_DB1_STAGE_GIT_OWNERSHIP 2u

#define AIMEE_DB1_OP_OWNERSHIP_UPSERT             1u
#define AIMEE_DB1_OP_OWNERSHIP_DELETE             2u
#define AIMEE_DB1_OP_OWNERSHIP_OWNER_GET          3u
#define AIMEE_DB1_OP_OWNERSHIP_BRANCH_FOR_SESSION 4u
#define AIMEE_DB1_OP_OWNERSHIP_SESSION_BY_PREFIX  5u
#define AIMEE_DB1_OP_FEATURE_BRANCH_UPSERT        6u
#define AIMEE_DB1_OP_FEATURE_BRANCH_GET           7u

/* Family 3: per-conversation context, clarifications and working memory. */

#define AIMEE_DB1_EVENT_CONVERSATION 11779u
#define AIMEE_DB1_STAGE_CONVERSATION 3u

#define AIMEE_DB1_OP_REWRITE_STATE_GET          1u
#define AIMEE_DB1_OP_REWRITE_STATE_SET          2u
#define AIMEE_DB1_OP_WM_SET                     3u
#define AIMEE_DB1_OP_WM_GET                     4u
#define AIMEE_DB1_OP_WM_LIST                    5u
#define AIMEE_DB1_OP_WM_ASSEMBLE_CONTEXT        6u
#define AIMEE_DB1_OP_REWRITE_RECORD             7u
#define AIMEE_DB1_OP_WM_SEARCH_SESSION_IDS      8u
#define AIMEE_DB1_OP_CONV_RECORD_EVENT          9u
#define AIMEE_DB1_OP_CONV_SET_CHAIN_ID          10u
#define AIMEE_DB1_OP_CONV_INSERT_CHAIN          11u
#define AIMEE_DB1_OP_CONV_PENDING_EVENTS        12u
#define AIMEE_DB1_OP_CONV_LIST_CHAINS           13u
#define AIMEE_DB1_OP_CONV_CHAIN_EVENTS          14u
#define AIMEE_DB1_OP_CONV_SEARCH_CHAINS         15u
#define AIMEE_DB1_OP_CONV_STATE_GET             16u
#define AIMEE_DB1_OP_CONV_STATE_UPDATE          17u
#define AIMEE_DB1_OP_WINDOW_SCAN_STATE          18u
#define AIMEE_DB1_OP_WINDOW_SESSION_ID          19u
#define AIMEE_DB1_OP_WINDOW_CREATE_RAW          20u
#define AIMEE_DB1_OP_WINDOW_ADD_TERM            21u
#define AIMEE_DB1_OP_WINDOW_ADD_FILE            22u
#define AIMEE_DB1_OP_WINDOW_IDS_BY_TIER         23u
#define AIMEE_DB1_OP_WINDOW_CANDIDATES_BY_TERMS 24u
#define AIMEE_DB1_OP_WINDOW_LIST_FILES          25u
#define AIMEE_DB1_OP_WINDOW_INDEX_SUMMARY       26u
#define AIMEE_DB1_OP_WINDOW_LEXICAL_HITS        27u
#define AIMEE_DB1_OP_WINDOW_SET_TIER            28u
#define AIMEE_DB1_OP_WINDOW_PRUNE_TERMS         29u
#define AIMEE_DB1_OP_WINDOW_DELETE_ALL_FILES    30u
#define AIMEE_DB1_OP_WINDOW_PRUNE_FILES         31u
#define AIMEE_DB1_OP_WINDOWS_DELETE_AFTER_TURN  32u
#define AIMEE_DB1_OP_USER_MEMORY_LIST_RECALL    33u
#define AIMEE_DB1_OP_USER_MEMORY_ANY            34u
#define AIMEE_DB1_OP_USER_MEMORY_UPSERT         35u

/* Family 4: queued agent work: logs, coordination jobs and the cron that
 * drives them. */

#define AIMEE_DB1_EVENT_AGENT_WORK 11780u
#define AIMEE_DB1_STAGE_AGENT_WORK 4u

#define AIMEE_DB1_OP_COGNIFY_ENQUEUE                   1u
#define AIMEE_DB1_OP_COGNIFY_STATUS                    2u
#define AIMEE_DB1_OP_COGNIFY_CLAIM_NEXT                3u
#define AIMEE_DB1_OP_COGNIFY_MARK                      4u
#define AIMEE_DB1_OP_AGENT_LOG_INSERT                  5u
#define AIMEE_DB1_OP_AGENT_LOG_LIST_RECENT             6u
#define AIMEE_DB1_OP_AGENT_LOG_LIST_BY_SESSION         7u
#define AIMEE_DB1_OP_AGENT_LOG_SEARCH_SESSIONS_BY_ROLE 8u
#define AIMEE_DB1_OP_AGENT_LOG_COUNT_PER_ROLE          9u
#define AIMEE_DB1_OP_AGENT_LOG_FAILURES_SINCE          10u
#define AIMEE_DB1_OP_AGENT_LOG_LIST_RECENT_ERRORS      11u
#define AIMEE_DB1_OP_AGENT_LOG_DELEGATION_PATTERNS     12u
#define AIMEE_DB1_OP_AGENT_LOG_FAILURE_SEEDS           13u
#define AIMEE_DB1_OP_AGENT_LOG_METRICS_BY_ROLE         14u
#define AIMEE_DB1_OP_AGENT_LOG_AGENT_STATS             15u
#define AIMEE_DB1_OP_AGENT_LOG_HUD_SUMMARY             16u
#define AIMEE_DB1_OP_AGENT_LOG_SESSION_OUTCOME         17u
#define AIMEE_DB1_OP_AGENT_LOG_PROMETHEUS              18u
#define AIMEE_DB1_OP_AGENT_LOG_STATS                   19u
#define AIMEE_DB1_OP_TRIGGER_INSERT                    20u
#define AIMEE_DB1_OP_TRIGGER_STATUS_SET                21u
#define AIMEE_DB1_OP_TRIGGER_GET                       22u
#define AIMEE_DB1_OP_TRIGGER_LIST_JSON                 23u
#define AIMEE_DB1_OP_COORD_JOB_CREATE                  24u
#define AIMEE_DB1_OP_COORD_TASK_ADD                    25u
#define AIMEE_DB1_OP_COORD_TASK_CLAIM_NEXT             26u
#define AIMEE_DB1_OP_COORD_TASK_COMPLETE               27u
#define AIMEE_DB1_OP_COORD_TASK_FAIL                   28u
#define AIMEE_DB1_OP_COORD_TASK_COMPLETE_OWNED         29u
#define AIMEE_DB1_OP_COORD_TASK_FAIL_OWNED             30u
#define AIMEE_DB1_OP_COORD_TASK_RELEASE                31u
#define AIMEE_DB1_OP_COORD_TASK_RELEASE_BOUNDED        32u
#define AIMEE_DB1_OP_COORD_TASK_RELEASE_BOUNDED_OWNED  33u
#define AIMEE_DB1_OP_COORD_OWNER_RECOVER               34u
#define AIMEE_DB1_OP_COORD_JOB_GET                     35u
#define AIMEE_DB1_OP_COORD_TASK_LIST                   36u
#define AIMEE_DB1_OP_COORD_JOB_CANCEL                  37u
#define AIMEE_DB1_OP_COORD_JOB_REFRESH_STATUS          38u
#define AIMEE_DB1_OP_COORD_JOB_FILE_CONFLICT           39u
#define AIMEE_DB1_OP_COORD_JOB_LIST_RECENT             40u
#define AIMEE_DB1_OP_COORD_JOB_LIST_ACTIVE_IDS         41u
#define AIMEE_DB1_OP_COORD_TASK_GET_DISPATCH           42u
#define AIMEE_DB1_OP_CRON_JOB_UPSERT                   43u
#define AIMEE_DB1_OP_CRON_JOB_GET                      44u
#define AIMEE_DB1_OP_CRON_JOB_LOAD                     45u
#define AIMEE_DB1_OP_CRON_JOB_SET_ENABLED              46u
#define AIMEE_DB1_OP_CRON_JOB_SET_ENABLED_ALL          47u
#define AIMEE_DB1_OP_CRON_JOB_DELETE                   48u
#define AIMEE_DB1_OP_CRON_JOB_RECORD_RUN               49u
#define AIMEE_DB1_OP_CRON_JOB_LIST_JSON                50u
#define AIMEE_DB1_OP_CRON_JOB_HISTORY_JSON             51u
#define AIMEE_DB1_OP_CRON_JOB_LATEST_OUTPUT            52u
#define AIMEE_DB1_OP_CRON_JOB_LAST_OUTPUT_HASH         53u

/* Family 5: delegation spawns, reservations, checkpoints and the agent_jobs
 * ledger they reserve against. These move as ONE unit: a reservation resolves
 * to a job id, and server_compute.c records that the ledger and the launch
 * were deliberately co-located because a ledger across a boundary from the
 * launch left paid-for jobs that nothing could replay. Splitting them across
 * families would restore exactly that topology one migration at a time. */

#define AIMEE_DB1_EVENT_DELEGATION 11781u
#define AIMEE_DB1_STAGE_DELEGATION 5u

#define AIMEE_DB1_OP_DELEGATION_MESSAGE_RECORD               1u
#define AIMEE_DB1_OP_DELEGATION_SPAWN_RECORD                 2u
#define AIMEE_DB1_OP_DELEGATION_SPAWN_COMPLETE               3u
#define AIMEE_DB1_OP_DELEGATION_SPAWN_PREEMPT                4u
#define AIMEE_DB1_OP_DELEGATION_SPAWN_STATUS                 5u
#define AIMEE_DB1_OP_DELEGATION_SPAWN_STOP_REASON            6u
#define AIMEE_DB1_OP_DELEGATION_SPAWN_IS_STOPPED             7u
#define AIMEE_DB1_OP_DELEGATION_SPAWN_IS_CANCELLED           8u
#define AIMEE_DB1_OP_DELEGATION_SPAWN_IS_ACTIVE              9u
#define AIMEE_DB1_OP_DELEGATION_SPAWN_COUNT_TOTAL            10u
#define AIMEE_DB1_OP_DELEGATION_SPAWN_FIND_ROOT              11u
#define AIMEE_DB1_OP_DELEGATION_SPAWN_COUNT_DESCENDANTS      12u
#define AIMEE_DB1_OP_DELEGATION_SPAWN_LIST_ACTIVE            13u
#define AIMEE_DB1_OP_DELEGATION_SPAWN_CANCEL_BY_ID           14u
#define AIMEE_DB1_OP_DELEGATION_SPAWN_CANCEL_RECURSIVE       15u
#define AIMEE_DB1_OP_DELEGATION_SPAWN_CANCEL_STALE           16u
#define AIMEE_DB1_OP_DELEGATE_RESERVATION_GET                17u
#define AIMEE_DB1_OP_DELEGATE_RESERVATION_ADOPT              18u
#define AIMEE_DB1_OP_DELEGATE_RESERVATION_SAVE               19u
#define AIMEE_DB1_OP_DELEGATE_RESERVATION_FORGET             20u
#define AIMEE_DB1_OP_DELEGATE_RESERVATION_FORGET_IF_MATCHES  21u
#define AIMEE_DB1_OP_DELEGATION_CHECKPOINT_SAVE              22u
#define AIMEE_DB1_OP_DELEGATION_CHECKPOINT_LOAD              23u
#define AIMEE_DB1_OP_AGENT_JOB_CREATE                        24u
#define AIMEE_DB1_OP_AGENT_JOB_UPDATE                        25u
#define AIMEE_DB1_OP_AGENT_JOB_COMPLETE                      26u
#define AIMEE_DB1_OP_AGENT_JOB_SET_AGENT                     27u
#define AIMEE_DB1_OP_AGENT_JOB_HEARTBEAT                     28u
#define AIMEE_DB1_OP_AGENT_JOB_HEARTBEAT_EXT                 29u
#define AIMEE_DB1_OP_AGENT_JOB_IS_CANCELLED                  30u
#define AIMEE_DB1_OP_AGENT_JOB_CLASSIFY_STALE                31u
#define AIMEE_DB1_OP_AGENT_JOB_GET                           32u
#define AIMEE_DB1_OP_AGENT_JOB_GET_BY_PARTICIPANT            33u
#define AIMEE_DB1_OP_AGENT_JOB_HEARTBEAT_IS_STALE            34u
#define AIMEE_DB1_OP_AGENT_JOB_TAKE_LEASE                    35u
#define AIMEE_DB1_OP_AGENT_JOB_LIST_RECENT                   36u
#define AIMEE_DB1_OP_AGENT_JOB_LIST_RUNNING_IDS              37u
#define AIMEE_DB1_OP_AGENT_JOB_CANCEL_BY_ID                  38u
#define AIMEE_DB1_OP_AGENT_JOB_CANCEL_UNASSIGNED             39u
#define AIMEE_DB1_OP_AGENT_JOB_CANCEL_NONTERMINAL_ON_RESTART 40u
#define AIMEE_DB1_OP_AGENT_JOB_CANCEL_STALE                  41u
#define AIMEE_DB1_OP_AGENT_LOG_ENTRY_LIST                    42u
#define AIMEE_DB1_OP_DELEGATE_LEARNING_RECORD                43u
#define AIMEE_DB1_OP_DELEGATE_LEARNING_INJECT_PROMPT         44u

/* Wire bounds, carried from the catalog. VALUE_MAX is the widest
   reply a stage may build; FIELDS_MAX is the widest request arity, and
   sizes the decoder's pointer array. Requests are NOT capped: they carry
   prompts and documents, an in-process caller passes those whole, and the
   frame already bounds what arrived. */
#define AIMEE_DB1_STATE_MAX  6144u
#define AIMEE_DB1_VALUE_MAX  1048576u
#define AIMEE_DB1_FIELDS_MAX 33u

#define AIMEE_DB1_STATUS_OK       0u
#define AIMEE_DB1_STATUS_MISSING  1u
#define AIMEE_DB1_STATUS_INVALID  2u
#define AIMEE_DB1_STATUS_TOO_LONG 3u
#define AIMEE_DB1_STATUS_FAILED   4u

static inline void aimee_db1_put_u32(uint8_t *p, uint32_t value)
{
   for (unsigned i = 0; i < 4; ++i)
      p[i] = (uint8_t)(value >> (i * 8u));
}

static inline uint32_t aimee_db1_get_u32(const uint8_t *p)
{
   uint32_t value = 0;
   for (unsigned i = 0; i < 4; ++i)
      value |= (uint32_t)p[i] << (i * 8u);
   return value;
}

#endif /* AIMEE_DB1_MODULE_API_H */
