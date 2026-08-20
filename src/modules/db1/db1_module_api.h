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
#define AIMEE_DB1_OP_CLARIFY_START              37u
#define AIMEE_DB1_OP_CLARIFY_GET                38u
#define AIMEE_DB1_OP_CLARIFY_ANSWER             39u
#define AIMEE_DB1_OP_CLARIFY_SCORE              40u
#define AIMEE_DB1_OP_CLARIFY_WEAKEST_DIM        41u
#define AIMEE_DB1_OP_CLARIFY_NEXT_QUESTION      42u
#define AIMEE_DB1_OP_CLARIFY_CRYSTALLIZE        43u
#define AIMEE_DB1_OP_WM_DELETE                  44u
#define AIMEE_DB1_OP_WM_CLEAR                   45u
#define AIMEE_DB1_OP_WM_GC                      46u

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

/* Family 6: server and webchat session rows: who is talking, when they last
 * spoke, and what the conversation was called. */

#define AIMEE_DB1_EVENT_SESSIONS 11782u
#define AIMEE_DB1_STAGE_SESSIONS 6u

#define AIMEE_DB1_OP_SERVER_SESSION_CREATE                 1u
#define AIMEE_DB1_OP_SERVER_SESSION_GET                    2u
#define AIMEE_DB1_OP_SERVER_SESSION_SET_OUTCOME            3u
#define AIMEE_DB1_OP_SERVER_SESSION_DELETE                 4u
#define AIMEE_DB1_OP_SERVER_SESSION_LIST_RECENT            5u
#define AIMEE_DB1_OP_SERVER_SESSION_SEARCH_BY_TITLE        6u
#define AIMEE_DB1_OP_SERVER_SESSION_COUNT                  7u
#define AIMEE_DB1_OP_SERVER_SESSION_LIST_EXPIRED           8u
#define AIMEE_DB1_OP_SERVER_SESSION_DELETE_EXPIRED         9u
#define AIMEE_DB1_OP_PRIMARY_SESSION_SAVE                  10u
#define AIMEE_DB1_OP_PRIMARY_SESSION_LOAD                  11u
#define AIMEE_DB1_OP_PRIMARY_SESSION_DELETE                12u
#define AIMEE_DB1_OP_PRIMARY_SESSION_ALLOC_RECENT          13u
#define AIMEE_DB1_OP_PRIMARY_SESSION_ALLOC_SEARCH          14u
#define AIMEE_DB1_OP_PRIMARY_SESSION_GET_LATEST            15u
#define AIMEE_DB1_OP_SESSION_WRITE_PATH_RECORD             16u
#define AIMEE_DB1_OP_SESSION_STALE_READS                   17u
#define AIMEE_DB1_OP_WEBCHAT_CLAUDE_SESSION_GET            18u
#define AIMEE_DB1_OP_WEBCHAT_CLAUDE_SESSION_OWNED_BY_OTHER 19u
#define AIMEE_DB1_OP_WEBCHAT_CLAUDE_SESSION_BIND           20u
#define AIMEE_DB1_OP_WEBCHAT_LIVE_SET                      21u
#define AIMEE_DB1_OP_WEBCHAT_LIVE_GET                      22u

/* Family 7: machine-local runtime state: caches, this box's operator and
 * clones, the model catalogue it fetched, and the snapshots it took. */

#define AIMEE_DB1_EVENT_RUNTIME 11783u
#define AIMEE_DB1_STAGE_RUNTIME 7u

#define AIMEE_DB1_OP_RUNTIME_STATE_SET                    1u
#define AIMEE_DB1_OP_RUNTIME_STATE_GET                    2u
#define AIMEE_DB1_OP_RUNTIME_STATE_ADD_INT                3u
#define AIMEE_DB1_OP_PROJECT_CLONE_UPSERT                 4u
#define AIMEE_DB1_OP_PROJECT_CLONE_GET                    5u
#define AIMEE_DB1_OP_PROJECT_CLONE_DELETE                 6u
#define AIMEE_DB1_OP_PROJECT_CLONE_LIST                   7u
#define AIMEE_DB1_OP_PROJECT_CLONE_LIST_BY_PROJECT        8u
#define AIMEE_DB1_OP_LOCAL_OPERATOR_UPSERT                9u
#define AIMEE_DB1_OP_LOCAL_OPERATOR_GET                   10u
#define AIMEE_DB1_OP_LOCAL_OPERATOR_GET_ACTIVE            11u
#define AIMEE_DB1_OP_LOCAL_OPERATOR_SET_ACTIVE            12u
#define AIMEE_DB1_OP_LOCAL_OPERATOR_DELETE                13u
#define AIMEE_DB1_OP_LOCAL_OPERATOR_LIST                  14u
#define AIMEE_DB1_OP_ENV_CAPABILITY_SET                   15u
#define AIMEE_DB1_OP_ENV_CAPABILITY_GET                   16u
#define AIMEE_DB1_OP_ENV_CAPABILITY_LIST                  17u
#define AIMEE_DB1_OP_MAINTENANCE_STATE_LOAD               18u
#define AIMEE_DB1_OP_MAINTENANCE_STATE_SAVE               19u
#define AIMEE_DB1_OP_MODEL_CATALOG_IS_FRESH               20u
#define AIMEE_DB1_OP_MODEL_CATALOG_GET                    21u
#define AIMEE_DB1_OP_MODEL_CATALOG_REPLACE                22u
#define AIMEE_DB1_OP_MODEL_PRICE_GET                      23u
#define AIMEE_DB1_OP_MODEL_PRICE_SET                      24u
#define AIMEE_DB1_OP_MODEL_PRICE_DELETE                   25u
#define AIMEE_DB1_OP_WORKING_PROFILE_LOCAL_OBSERVE        26u
#define AIMEE_DB1_OP_WORKING_PROFILE_LOCAL_LIST           27u
#define AIMEE_DB1_OP_WORKING_PROFILE_LOCAL_GET            28u
#define AIMEE_DB1_OP_WORKING_PROFILE_LOCAL_RESET_FIELD    29u
#define AIMEE_DB1_OP_TOOL_LOCAL_AVAILABILITY_SET          30u
#define AIMEE_DB1_OP_TOOL_LOCAL_AVAILABILITY_GET          31u
#define AIMEE_DB1_OP_TOOL_LOCAL_AVAILABILITY_DELETE       32u
#define AIMEE_DB1_OP_TOOL_LOCAL_AVAILABILITY_LIST         33u
#define AIMEE_DB1_OP_CONTEXT_CACHE_GET                    34u
#define AIMEE_DB1_OP_CONTEXT_CACHE_PUT                    35u
#define AIMEE_DB1_OP_CONTEXT_CACHE_INVALIDATE             36u
#define AIMEE_DB1_OP_CONTEXT_SNAPSHOT_INSERT              37u
#define AIMEE_DB1_OP_CONTEXT_SNAPSHOT_COUNT_MIN_SAMPLES   38u
#define AIMEE_DB1_OP_CONTEXT_SNAPSHOT_IDS_MIN_SAMPLES     39u
#define AIMEE_DB1_OP_CONTEXT_SNAPSHOT_COUNT_FOR_MEMORY    40u
#define AIMEE_DB1_OP_CONTEXT_SNAPSHOT_SESSIONS_FOR_MEMORY 41u
#define AIMEE_DB1_OP_CONTEXT_SNAPSHOT_HAS_MEMORY          42u
#define AIMEE_DB1_OP_AGENT_CACHE_GET                      43u
#define AIMEE_DB1_OP_AGENT_CACHE_PUT                      44u
#define AIMEE_DB1_OP_WEB_PAGE_GET                         45u
#define AIMEE_DB1_OP_WEB_PAGE_PUT                         46u
#define AIMEE_DB1_OP_WEB_PAGE_DROP                        47u
#define AIMEE_DB1_OP_WEB_PAGE_CANONICAL_URL               48u
#define AIMEE_DB1_OP_FSNAP_CREATE                         49u
#define AIMEE_DB1_OP_FSNAP_GET_OR_CREATE                  50u
#define AIMEE_DB1_OP_FSNAP_RECORD_FILE                    51u
#define AIMEE_DB1_OP_FSNAP_PRUNE                          52u
#define AIMEE_DB1_OP_FSNAP_LIST                           53u
#define AIMEE_DB1_OP_FSNAP_RESTORE                        54u
#define AIMEE_DB1_OP_FSNAP_GET                            55u
#define AIMEE_DB1_OP_DECISION_RECORD                      56u
#define AIMEE_DB1_OP_MCP_OSV_CACHE_GET                    57u
#define AIMEE_DB1_OP_MCP_OSV_CACHE_UPSERT                 58u
#define AIMEE_DB1_OP_MCP_OSV_CACHE_LIST                   59u
#define AIMEE_DB1_OP_MCP_OSV_AUDIT                        60u

/* Family 8: what the system spent and what it noticed: token and cost ledgers,
 * guardrail and interaction events, eval results and diagnoses. */

#define AIMEE_DB1_EVENT_TELEMETRY 11784u
#define AIMEE_DB1_STAGE_TELEMETRY 8u

#define AIMEE_DB1_OP_TOKEN_AUDIT_INSERT                     1u
#define AIMEE_DB1_OP_TOKEN_AUDIT_ENSURE_IDEM_INDEX          2u
#define AIMEE_DB1_OP_TOKEN_AUDIT_COST_FOR_DELEGATION        3u
#define AIMEE_DB1_OP_TOKEN_AUDIT_COST_FOR_DELEGATION_EX     4u
#define AIMEE_DB1_OP_TOKEN_AUDIT_SESSION_SPLIT              5u
#define AIMEE_DB1_OP_TOKEN_AUDIT_TOTALS                     6u
#define AIMEE_DB1_OP_TOKEN_AUDIT_SPEND_BREAKDOWN            7u
#define AIMEE_DB1_OP_TOKEN_AUDIT_BY_ROLE                    8u
#define AIMEE_DB1_OP_TOKEN_AUDIT_BY_TOOL                    9u
#define AIMEE_DB1_OP_TOKEN_AUDIT_BY_MODEL                   10u
#define AIMEE_DB1_OP_TOKEN_AUDIT_BY_SOURCE                  11u
#define AIMEE_DB1_OP_TOKEN_AUDIT_LIST_DASHBOARD             12u
#define AIMEE_DB1_OP_INSIGHTS_BY_PLATFORM                   13u
#define AIMEE_DB1_OP_INSIGHTS_TOP_SESSIONS                  14u
#define AIMEE_DB1_OP_INSIGHTS_DELEGATES_BY_ROLE             15u
#define AIMEE_DB1_OP_COST_FOLD_RECORD                       16u
#define AIMEE_DB1_OP_COST_FOLD_TOTAL                        17u
#define AIMEE_DB1_OP_INTERACTION_EVENT_RECORD               18u
#define AIMEE_DB1_OP_INTERACTION_EVENT_LIST_UNREFLECTED     19u
#define AIMEE_DB1_OP_INTERACTION_EVENT_LIST_FOR_SESSION     20u
#define AIMEE_DB1_OP_INTERACTION_EVENT_LIST_PROMOTION_FEED  21u
#define AIMEE_DB1_OP_INTERACTION_EVENT_MARK_REFLECTED       22u
#define AIMEE_DB1_OP_INTERACTION_EVENT_MARK_PROMOTED        23u
#define AIMEE_DB1_OP_INTERACTION_EVENT_EVICT_IF_NEEDED      24u
#define AIMEE_DB1_OP_GUARDRAIL_EVENT_INSERT                 25u
#define AIMEE_DB1_OP_GUARDRAIL_EVENT_COUNTS_7D              26u
#define AIMEE_DB1_OP_GUARDRAIL_EVENT_SESSION_ADVISORY_COUNT 27u
#define AIMEE_DB1_OP_GUARDRAIL_EVENT_LIST                   28u
#define AIMEE_DB1_OP_EVAL_RESULT_INSERT                     29u
#define AIMEE_DB1_OP_EVAL_FAILED_TASKS_RECENT               30u
#define AIMEE_DB1_OP_EVAL_PASSED_TASKS_RECENT               31u
#define AIMEE_DB1_OP_EVAL_RESULTS_LIST                      32u
#define AIMEE_DB1_OP_DIAGNOSE_START                         33u
#define AIMEE_DB1_OP_DIAGNOSE_ADD_OBSERVATION               34u
#define AIMEE_DB1_OP_DIAGNOSE_ADD_HYPOTHESIS                35u
#define AIMEE_DB1_OP_DIAGNOSE_ADD_EVIDENCE                  36u
#define AIMEE_DB1_OP_DIAGNOSE_ADD_PROBE                     37u
#define AIMEE_DB1_OP_DIAGNOSE_GET                           38u
#define AIMEE_DB1_OP_DIAGNOSE_LIST                          39u
#define AIMEE_DB1_OP_DIAGNOSE_LIST_ITEMS                    40u
#define AIMEE_DB1_OP_DIAGNOSE_LIST_HYPOTHESES               41u
#define AIMEE_DB1_OP_DIAGNOSE_RANK_HYPOTHESES               42u
#define AIMEE_DB1_OP_DIAGNOSE_CONCLUDE                      43u
#define AIMEE_DB1_OP_DIAGNOSE_ABANDON                       44u
#define AIMEE_DB1_OP_DIAGNOSE_SUGGEST_PROBES                48u

/* Family 9: per-session guardrail state: the hook's view of a session, stored
 * as a scalar row plus five child tables and read back as one nested struct. */

#define AIMEE_DB1_EVENT_GUARDRAIL_STATE 11785u
#define AIMEE_DB1_STAGE_GUARDRAIL_STATE 9u

#define AIMEE_DB1_OP_SESSION_STATE_LOAD         1u
#define AIMEE_DB1_OP_SESSION_STATE_SAVE         2u
#define AIMEE_DB1_OP_SESSION_STATE_DELETE       3u
#define AIMEE_DB1_OP_SESSION_STATE_EXISTS       4u
#define AIMEE_DB1_OP_SESSION_STATE_LIST         5u
#define AIMEE_DB1_OP_SESSION_STATE_GET_SUMMARY  6u
#define AIMEE_DB1_OP_SESSION_STATE_LIST_EXPIRED 7u

/* Family 10: multi-agent ensemble runs: one table holding a run's state, plus
 * the template interpretation and prompt building that only the run itself
 * uses. Template files are resolved from roots the caller names. */

#define AIMEE_DB1_EVENT_ENSEMBLE 11786u
#define AIMEE_DB1_STAGE_ENSEMBLE 10u

#define AIMEE_DB1_OP_ENSEMBLE_CREATE                  1u
#define AIMEE_DB1_OP_ENSEMBLE_VIEW                    2u
#define AIMEE_DB1_OP_ENSEMBLE_ADVANCE                 3u
#define AIMEE_DB1_OP_ENSEMBLE_PAUSE                   4u
#define AIMEE_DB1_OP_ENSEMBLE_LIST                    5u
#define AIMEE_DB1_OP_ENSEMBLE_FIND_CURRENT_BY_CHANNEL 6u

/* Family 11: the workflow engine's plans, traces, pipelines and bindings --
 * everything the engine stores that no multi-call transaction spans. */

#define AIMEE_DB1_EVENT_WORKFLOW 11787u
#define AIMEE_DB1_STAGE_WORKFLOW 11u

#define AIMEE_DB1_OP_EXECUTION_TRACE_INSERT                 1u
#define AIMEE_DB1_OP_EXECUTION_TRACE_COUNT_FOR_SESSION      2u
#define AIMEE_DB1_OP_EXECUTION_TRACE_LIST_RECENT            3u
#define AIMEE_DB1_OP_EXECUTION_TRACE_GET                    4u
#define AIMEE_DB1_OP_EXECUTION_TRACE_LIST_TOOL_CALLS        5u
#define AIMEE_DB1_OP_EXECUTION_TRACE_LIST_AFTER_ID          6u
#define AIMEE_DB1_OP_WFE_BIND                               7u
#define AIMEE_DB1_OP_WFE_BINDING_GET                        8u
#define AIMEE_DB1_OP_WFE_UNBIND                             9u
#define AIMEE_DB1_OP_WFE_LEASE_RENEW                        10u
#define AIMEE_DB1_OP_WFE_LEASE_EXPIRY_GET                   11u
#define AIMEE_DB1_OP_WFE_LEASE_STALE_WORK_ITEMS             12u
#define AIMEE_DB1_OP_WFE_LEASE_RECLAIM_STALE                13u
#define AIMEE_DB1_OP_PIPELINE_CREATE                        14u
#define AIMEE_DB1_OP_PIPELINE_GET                           15u
#define AIMEE_DB1_OP_PIPELINE_UPDATE                        16u
#define AIMEE_DB1_OP_PIPELINE_LINK_PLAN                     17u
#define AIMEE_DB1_OP_PIPELINE_LINK_JOB                      18u
#define AIMEE_DB1_OP_PIPELINE_CANCEL                        19u
#define AIMEE_DB1_OP_PIPELINE_LIST_ACTIVE                   20u
#define AIMEE_DB1_OP_ROADMAP_DISPATCH_UPSERT                21u
#define AIMEE_DB1_OP_ROADMAP_DISPATCH_GET                   22u
#define AIMEE_DB1_OP_ROADMAP_DISPATCH_SET_STATUS            23u
#define AIMEE_DB1_OP_ROADMAP_DISPATCH_SET_PHASE             24u
#define AIMEE_DB1_OP_ROADMAP_UNIT_ENSURE                    25u
#define AIMEE_DB1_OP_ROADMAP_UNIT_GET                       26u
#define AIMEE_DB1_OP_ROADMAP_UNIT_SET_STATE                 27u
#define AIMEE_DB1_OP_ROADMAP_UNIT_CLAIM                     28u
#define AIMEE_DB1_OP_ROADMAP_UNIT_HEARTBEAT                 29u
#define AIMEE_DB1_OP_ROADMAP_UNIT_FINISH                    30u
#define AIMEE_DB1_OP_ROADMAP_UNIT_SET_COORD_JOB             31u
#define AIMEE_DB1_OP_ROADMAP_UNIT_INCREMENT_VERIFY_ATTEMPTS 32u
#define AIMEE_DB1_OP_ROADMAP_UNIT_SELECT_NEXT               33u
#define AIMEE_DB1_OP_EXECUTION_PLAN_CREATE                  34u
#define AIMEE_DB1_OP_EXECUTION_PLAN_GET                     35u
#define AIMEE_DB1_OP_EXECUTION_PLAN_LIST_IDS                36u
#define AIMEE_DB1_OP_EXECUTION_PLAN_EXISTS                  37u
#define AIMEE_DB1_OP_EXECUTION_PLAN_COUNT_STEPS             38u
#define AIMEE_DB1_OP_EXECUTION_PLAN_LIST_RUNNING_IDS        39u
#define AIMEE_DB1_OP_EXECUTION_PLAN_LIST_RECENT_SUMMARIES   40u
#define AIMEE_DB1_OP_EXECUTION_PLAN_SET_STATUS              41u
#define AIMEE_DB1_OP_EXECUTION_PLAN_CANCEL_BY_ID            42u
#define AIMEE_DB1_OP_EXECUTION_PLAN_CANCEL_STALE            43u
#define AIMEE_DB1_OP_PLAN_STEP_SET_STATUS                   44u
#define AIMEE_DB1_OP_PLAN_STEP_SET_STATUS_OUTPUT            45u
#define AIMEE_DB1_OP_PLAN_STEP_CANCEL_ACTIVE_FOR_PLAN       46u
#define AIMEE_DB1_OP_PLAN_STEP_CANCEL_ORPHANS               47u
#define AIMEE_DB1_OP_STEP_EVIDENCE_INSERT                   48u
#define AIMEE_DB1_OP_STEP_EVIDENCE_GET_LATEST               49u

/* Family 12: roundtable pipeline runs, passes, attempts and gates: the review
 * pipeline's own state machine, split from workflow because one family's
 * generated client cannot hold both. */

#define AIMEE_DB1_EVENT_ROUNDTABLE 11788u
#define AIMEE_DB1_STAGE_ROUNDTABLE 12u

#define AIMEE_DB1_OP_ROUNDTABLE_RUN_CREATE               1u
#define AIMEE_DB1_OP_ROUNDTABLE_RUN_GET                  2u
#define AIMEE_DB1_OP_ROUNDTABLE_RUN_UPDATE               3u
#define AIMEE_DB1_OP_ROUNDTABLE_RUN_SET_STATE            4u
#define AIMEE_DB1_OP_ROUNDTABLE_RUN_CAS_STATE            5u
#define AIMEE_DB1_OP_ROUNDTABLE_RUN_LIST                 6u
#define AIMEE_DB1_OP_ROUNDTABLE_RUN_COUNT_ACTIVE         7u
#define AIMEE_DB1_OP_ROUNDTABLE_RUN_BRANCH_OWNER         8u
#define AIMEE_DB1_OP_ROUNDTABLE_PASS_CREATE              9u
#define AIMEE_DB1_OP_ROUNDTABLE_PASS_GET                 10u
#define AIMEE_DB1_OP_ROUNDTABLE_PASS_UPDATE              11u
#define AIMEE_DB1_OP_ROUNDTABLE_PASS_LATEST              12u
#define AIMEE_DB1_OP_ROUNDTABLE_PASS_MAX_NO              13u
#define AIMEE_DB1_OP_ROUNDTABLE_PASS_MAX_GROUP           14u
#define AIMEE_DB1_OP_ROUNDTABLE_PASS_GROUP_AGG           15u
#define AIMEE_DB1_OP_ROUNDTABLE_ATTEMPT_CREATE           16u
#define AIMEE_DB1_OP_ROUNDTABLE_ATTEMPT_GET_BY_RUN       17u
#define AIMEE_DB1_OP_ROUNDTABLE_ATTEMPT_CURRENT          18u
#define AIMEE_DB1_OP_ROUNDTABLE_ATTEMPT_UPDATE           19u
#define AIMEE_DB1_OP_ROUNDTABLE_ATTEMPT_MAX_NO           20u
#define AIMEE_DB1_OP_ROUNDTABLE_ATTEMPT_SUPERSEDE_OTHERS 21u
#define AIMEE_DB1_OP_ROUNDTABLE_GATE_CREATE              22u
#define AIMEE_DB1_OP_ROUNDTABLE_GATE_GET                 23u
#define AIMEE_DB1_OP_ROUNDTABLE_GATE_UPDATE              24u
#define AIMEE_DB1_OP_ROUNDTABLE_GATE_AGE_EXCEEDS_HOURS   25u

/* Family 13: the appliance's first-user remote-client grant: the enrollment
 * bearer, the certificate it binds to, and the tier that grant carries. */

#define AIMEE_DB1_EVENT_IDENTITY 11789u
#define AIMEE_DB1_STAGE_IDENTITY 13u

#define AIMEE_DB1_OP_REMOTE_CLIENT_CLAIM   1u
#define AIMEE_DB1_OP_REMOTE_CLIENT_ABANDON 2u
#define AIMEE_DB1_OP_REMOTE_CLIENT_BIND    3u
#define AIMEE_DB1_OP_REMOTE_CLIENT_TIER    4u

/* Family 14: session checkpoint rows: a label, the session it belongs to and
 * the snapshot it captured. */

#define AIMEE_DB1_EVENT_CHECKPOINTS 11790u
#define AIMEE_DB1_STAGE_CHECKPOINTS 14u

#define AIMEE_DB1_OP_CHECKPOINT_INSERT 1u
#define AIMEE_DB1_OP_CHECKPOINT_GET    2u
#define AIMEE_DB1_OP_CHECKPOINT_LIST   3u
#define AIMEE_DB1_OP_CHECKPOINT_DELETE 4u

/* Family 15: single-use JTI replay windows for the identity and management
 * token paths. Reserved: these entry points are not named db1_, which the
 * catalog requires, and each has a _consume_for_test twin that a production
 * wire has no place for. */

#define AIMEE_DB1_EVENT_JTI_REPLAY 11791u
#define AIMEE_DB1_STAGE_JTI_REPLAY 15u

#define AIMEE_DB1_OP_IDENTITY_JTI_CONSUME   1u
#define AIMEE_DB1_OP_MANAGEMENT_JTI_CONSUME 2u

/* Family 16: work-item state and its audit log. Split from workflow because
 * wfe_engine.c wraps sixteen of these writes in two transactions it opens and
 * commits across separate calls, which is a redesign of that engine's write
 * path rather than a wire question. */

#define AIMEE_DB1_EVENT_LIFECYCLE 11792u
#define AIMEE_DB1_STAGE_LIFECYCLE 16u

#define AIMEE_DB1_OP_WORK_ITEM_CREATE                    1u
#define AIMEE_DB1_OP_WORK_ITEM_GET                       2u
#define AIMEE_DB1_OP_WORK_ITEM_ID_BY_PROPOSAL            3u
#define AIMEE_DB1_OP_WORK_ITEM_ID_BY_PR_REF              4u
#define AIMEE_DB1_OP_WORK_ITEM_SET_STAGE                 5u
#define AIMEE_DB1_OP_WORK_ITEM_SET_PR_REF                6u
#define AIMEE_DB1_OP_WORK_ITEM_SET_WORKTREE              7u
#define AIMEE_DB1_OP_WORK_ITEM_SET_SUBMITTER             8u
#define AIMEE_DB1_OP_WORK_ITEM_SET_PARENT                9u
#define AIMEE_DB1_OP_WORK_ITEM_ABANDON_CHILDREN          10u
#define AIMEE_DB1_OP_WORK_ITEM_CHILD_COUNTS              11u
#define AIMEE_DB1_OP_WORK_ITEM_COUNT_ACTIVE_BY_SUBMITTER 12u
#define AIMEE_DB1_OP_WORK_ITEM_COUNT_RECENT_BY_SUBMITTER 13u
#define AIMEE_DB1_OP_WORK_ITEM_SUBMIT_CAPPED             14u
#define AIMEE_DB1_OP_WORK_ITEM_SET_TERMINAL              15u
#define AIMEE_DB1_OP_WORK_ITEM_GATE_APPLY                16u
#define AIMEE_DB1_OP_WORK_ITEM_SET_PAUSE                 17u
#define AIMEE_DB1_OP_WORK_ITEM_CLEAR_PAUSE               18u
#define AIMEE_DB1_OP_WORK_ITEM_CLEAR_PAUSE_IF            19u
#define AIMEE_DB1_OP_WORK_ITEM_ADD_COST                  20u
#define AIMEE_DB1_OP_WORK_ITEM_SET_COST_CAP              21u
#define AIMEE_DB1_OP_WORK_ITEM_INC_OVERRIDE              22u
#define AIMEE_DB1_OP_WORK_ITEM_DELETE                    23u
#define AIMEE_DB1_OP_WORK_ITEM_REAP_STALE_PARKS          24u
#define AIMEE_DB1_OP_WORK_ITEM_LIST                      25u
#define AIMEE_DB1_OP_WORK_ITEM_LIST_LRU                  26u
#define AIMEE_DB1_OP_LIFECYCLE_EVENT_ADD                 27u
#define AIMEE_DB1_OP_LIFECYCLE_EVENT_LIST                28u
#define AIMEE_DB1_OP_STAGE_ATTEMPT_INC                   29u
#define AIMEE_DB1_OP_STAGE_ATTEMPT_RESET                 30u
#define AIMEE_DB1_OP_STAGE_ATTEMPT_GET                   31u
#define AIMEE_DB1_OP_WORK_ITEM_RECORD_OUTCOME            32u
#define AIMEE_DB1_OP_WFE_CHILDREN_LIST                   33u
#define AIMEE_DB1_OP_WFE_ACTIVE_ROOT_COUNT               34u
#define AIMEE_DB1_OP_WFE_WORK_ITEM_ID_BY_GIT_PROPOSAL    35u
#define AIMEE_DB1_OP_WFE_EXECUTED_TURN_COUNT             36u
#define AIMEE_DB1_OP_WFE_STAGE_LOOP_COUNT                37u
#define AIMEE_DB1_OP_WFE_RUNNER_FAILURES_SINCE_PROGRESS  38u
#define AIMEE_DB1_OP_WFE_CAPACITY_WAITS_SINCE_PROGRESS   39u
#define AIMEE_DB1_OP_WFE_DESCENDANT_IDS                  40u
#define AIMEE_DB1_OP_WFE_RESUME_TRANSIENT                41u
#define AIMEE_DB1_OP_WFE_RESUME_WALL_CAPS                42u
#define AIMEE_DB1_OP_WFE_ABANDON_EXHAUSTED_WALL_CAPS     43u
#define AIMEE_DB1_OP_WFE_RESUME_READY_PARENTS            44u
#define AIMEE_DB1_OP_WFE_DELEGATE_JOB_SAVE               45u
#define AIMEE_DB1_OP_WFE_DELEGATE_JOBS_TERMINAL_CLAIM    46u
#define AIMEE_DB1_OP_WFE_BUDGET_RESERVE                  47u
#define AIMEE_DB1_OP_WFE_BUDGET_TOTALS                   48u
#define AIMEE_DB1_OP_WFE_BUDGET_RELEASE                  49u
#define AIMEE_DB1_OP_WFE_BUDGET_HEARTBEAT                50u
#define AIMEE_DB1_OP_WFE_BUDGET_RECONCILE                51u

/* Family 17: the management-JWKS cache row: the envelope the server verified,
 * when it is valid for, and the digests that pin it. Digests cross as hex
 * because the wire has no bytes. */

#define AIMEE_DB1_EVENT_MGMT_JWKS 11793u
#define AIMEE_DB1_STAGE_MGMT_JWKS 17u

#define AIMEE_DB1_OP_MGMT_JWKS_READ       1u
#define AIMEE_DB1_OP_MGMT_JWKS_GENERATION 2u
#define AIMEE_DB1_OP_MGMT_JWKS_INSTALL    3u

/* Family 18: management challenge nonces and the revocation high-water mark:
 * issued once, consumed once, and the counter that stops a replayed status
 * report rolling the server backwards. */

#define AIMEE_DB1_EVENT_MGMT_NONCE 11794u
#define AIMEE_DB1_STAGE_MGMT_NONCE 18u

#define AIMEE_DB1_OP_MGMT_NONCE_CLEAR     1u
#define AIMEE_DB1_OP_MGMT_NONCE_ISSUE     2u
#define AIMEE_DB1_OP_MGMT_NONCE_CONSUME   3u
#define AIMEE_DB1_OP_MGMT_STATUS_HWM_READ 4u
#define AIMEE_DB1_OP_MGMT_STATUS_HWM_SET  5u

/* Family 19: the client-certificate roster and the mTLS ramp: which
 * certificates exist, when each was last presented, and how far the ramp from
 * optional to required has come. */

#define AIMEE_DB1_EVENT_PKI 11795u
#define AIMEE_DB1_STAGE_PKI 19u

#define AIMEE_DB1_OP_PKI_CERT_UPSERT       1u
#define AIMEE_DB1_OP_PKI_CERT_LIST         2u
#define AIMEE_DB1_OP_PKI_REVOKED_SERIALS   3u
#define AIMEE_DB1_OP_PKI_CERT_REVOKE       4u
#define AIMEE_DB1_OP_PKI_CERT_CHECK        5u
#define AIMEE_DB1_OP_PKI_NOTE_PRESENTATION 6u
#define AIMEE_DB1_OP_PKI_RAMP_INIT         7u
#define AIMEE_DB1_OP_PKI_RAMP_READY        8u
#define AIMEE_DB1_OP_PKI_RAMP_ADVANCE      9u
#define AIMEE_DB1_OP_PKI_RAMP_GET          10u

/* Wire bounds, carried from the catalog. VALUE_MAX is the widest
   reply a stage may build; FIELDS_MAX is the widest request arity, and
   sizes the decoder's pointer array. Requests are NOT capped: they carry
   prompts and documents, an in-process caller passes those whole, and the
   frame already bounds what arrived. */
#define AIMEE_DB1_STATE_MAX  6144u
#define AIMEE_DB1_VALUE_MAX  1048576u
#define AIMEE_DB1_FIELDS_MAX 513u

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
