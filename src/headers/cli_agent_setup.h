/* cli_agent_setup.h: thin-client `aimee agent setup <provider>` wizard. */
#ifndef DEC_CLI_AGENT_SETUP_H
#define DEC_CLI_AGENT_SETUP_H 1

/* Drive `aimee agent setup <provider>` for the four supported providers
 * (openai, anthropic, codex-oauth, claude-oauth). argv[0] is the provider.
 * Returns a process exit code (0 = success). */
int handle_agent_setup_cmd(int argc, char **argv, int json_output);

/* `aimee codex <subcmd>` — currently `reauth`, the operator re-authentication for
 * a rejected codex OAuth refresh token (D6). Returns a process exit code. */
int handle_codex_cmd(int argc, char **argv, int json_output);

#endif /* DEC_CLI_AGENT_SETUP_H */
