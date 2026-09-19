/* Host-side protocol identifiers for calls to the Go memory process.
 * These numbers must match process-contracts.json and the Go stage constants.
 * No module implementation or module-side bus adapter is defined here.
 */
#ifndef AIMEE_HOST_MEMORY_STAGE_CONTRACT_H
#define AIMEE_HOST_MEMORY_STAGE_CONTRACT_H 1

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

#endif
