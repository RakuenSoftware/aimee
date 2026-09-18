/* Remaining fixed-stage identifiers for native callers awaiting retirement.
 * Memory wire encoding/decoding and all scoring/extraction policy live in Go. */
#ifndef AIMEE_MEMORY_MODULE_API_H
#define AIMEE_MEMORY_MODULE_API_H 1

#define AIMEE_MEMORY_EVENT_EXTRACT_INDEX    5889u
#define AIMEE_MEMORY_EVENT_WRITE            5890u
#define AIMEE_MEMORY_EVENT_EMBED            5891u
#define AIMEE_MEMORY_EVENT_RETRIEVE         5892u
#define AIMEE_MEMORY_EVENT_RERANK           5893u
#define AIMEE_MEMORY_EVENT_DECLARE_COMMANDS 5894u
#define AIMEE_MEMORY_EVENT_DATA             5895u
#define AIMEE_MEMORY_EVENT_COMMAND          5896u
#define AIMEE_MEMORY_STAGE_EXTRACT_INDEX    1u
#define AIMEE_MEMORY_STAGE_WRITE            2u
#define AIMEE_MEMORY_STAGE_EMBED            3u
#define AIMEE_MEMORY_STAGE_RETRIEVE         4u
#define AIMEE_MEMORY_STAGE_RERANK           5u
#define AIMEE_MEMORY_STAGE_DECLARE_COMMANDS 6u
#define AIMEE_MEMORY_STAGE_DATA             7u
#define AIMEE_MEMORY_STAGE_COMMAND          8u

/* Bound on raw relation labels accepted by the Go write decision. */
#define AIMEE_MEMORY_REL_TYPE_MAX 256u

#endif
