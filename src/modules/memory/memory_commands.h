/* memory_commands.h: the memory module's declaration to the core command table.
 *
 * Called once when the module comes up. Every surface -- CLI, v1 RPC, MCP, ACP --
 * routes memory commands from what this registers; none of them keeps a list of
 * its own. See src/headers/command_registry.h for why that property matters and
 * what it replaces. */
#ifndef DEC_MEMORY_COMMANDS_H
#define DEC_MEMORY_COMMANDS_H 1

/* Returns 0 on success, -1 if any registration was refused (duplicate name,
 * malformed name, or no surface). A refusal is fatal to the module's contract:
 * the commands it owns would be unroutable, so callers must not continue as if
 * memory were available. */
int memory_commands_register(void);

#endif /* DEC_MEMORY_COMMANDS_H */
