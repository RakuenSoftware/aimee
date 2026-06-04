#ifndef DEC_CLIENT_INTEGRATIONS_H
#define DEC_CLIENT_INTEGRATIONS_H 1

void ensure_client_integrations(void);

/* Enable (enable=1) or disable (enable=0) routing Claude Code through aimee's
 * Anthropic Messages ingress, by writing/removing ANTHROPIC_BASE_URL +
 * ANTHROPIC_AUTH_TOKEN in ~/.claude/settings.json. Opt-in only — enabling
 * reroutes the operator's live Claude Code to aimee's primary model. Returns 0
 * on success, -1 on error. */
int claude_code_proxy_configure(const char *server_url, const char *token, int enable);

void ensure_codex_project_trusted(const char *codex_home, const char *project_root);
void ensure_codex_current_project_trusted(const char *codex_home);

#endif /* DEC_CLIENT_INTEGRATIONS_H */
