#ifndef AIMEE_AGENT_CODE_CAPABILITIES_H
#define AIMEE_AGENT_CODE_CAPABILITIES_H

/* Canonical public vocabulary for agent-facing code intelligence.
 *
 * MCP descriptors, the lean presentation profile, and generated client guidance
 * consume these names. Mechanical tests keep the existing descriptor tables in
 * parity until they can be fully generated from one catalog. */
#define AIMEE_CODE_TOOL_FIND_SYMBOL          "find_symbol"
#define AIMEE_CODE_TOOL_AST_GREP_SEARCH      "ast_grep_search"
#define AIMEE_CODE_TOOL_INDEX                "index"
#define AIMEE_CODE_TOOL_PREVIEW_BLAST_RADIUS "preview_blast_radius"

#define AIMEE_CODE_INDEX_COMMAND_HYBRID      "hybrid"
#define AIMEE_CODE_INDEX_COMMAND_INVESTIGATE "investigate"
#define AIMEE_CODE_INDEX_COMMAND_PREVIEW     "preview"

#define AIMEE_CODE_SCOPE_CURRENT "current"
#define AIMEE_CODE_SCOPE_ALL     "all"

#define AIMEE_CODE_DISCOVERY_BLAST_RADIUS "blast radius"
#define AIMEE_CODE_DISCOVERY_PREVIEW      "preview"

#endif /* AIMEE_AGENT_CODE_CAPABILITIES_H */
