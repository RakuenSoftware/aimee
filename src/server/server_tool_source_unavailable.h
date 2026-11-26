/* server_tool_source_unavailable.h: the host's fixed reply for a source tool
 * that could not issue an owner receipt. */
#ifndef AIMEE_SERVER_TOOL_SOURCE_UNAVAILABLE_H
#define AIMEE_SERVER_TOOL_SOURCE_UNAVAILABLE_H

/* Written by the host, never by the memory owner or the model, and it carries
 * no memory evidence. The history custody gate therefore admits this exact
 * result without a receipt: refusing it made one failed memory_serve call end
 * the whole delegate run with "failed to build request". */
#define AIMEE_TOOL_SOURCE_UNAVAILABLE_RESULT                                                       \
   "{\"status\":\"error\",\"kind\":\"source_custody_unavailable\",\"message\":"                    \
   "\"selected tool output requires an owner-issued source receipt\"}"

#endif
