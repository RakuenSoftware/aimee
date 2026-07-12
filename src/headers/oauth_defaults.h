#ifndef OAUTH_DEFAULTS_H
#define OAUTH_DEFAULTS_H 1

/* oauth_defaults.h — built-in default OAuth App client IDs for the webchat
 * "Sign in with OAuth" buttons.
 *
 * Device-flow client IDs are PUBLIC (no secret is needed for the OAuth 2.0 device
 * grant), so a distribution can ship the client ID of a registered OAuth App and
 * let every deployment sign in out of the box — the operator never has to register
 * their own app or paste a client ID into the wizard.
 *
 * Resolution order in the oauth backends is: a value set from the UI (server
 * vault) → the matching AIMEE_*_OAUTH_CLIENT_ID env var → the built-in default
 * below. So a deployment can still override per-instance (vault) or per-image
 * (env) without a rebuild.
 *
 * To ship real defaults, set these to the client IDs of the Rakuen-registered
 * OAuth Apps — either edit the values here, or pass them at build time, e.g.
 *   make -C src CFLAGS='-DAIMEE_DEFAULT_GITHUB_OAUTH_CLIENT_ID=\"Iv1.abc123\"'
 * Each is #ifndef-guarded so a -D override wins over the value here. Empty ("")
 * means "no built-in default" — the button then needs the env or a UI-set ID.
 *
 * These are the ONLY hosts a single shared client ID makes sense for (a canonical
 * public host with one registered app). Self-hosted Gitea/GitLab instances each
 * run their own OAuth App, so they have no built-in default and must be configured
 * per-host from the UI. */

#ifndef AIMEE_DEFAULT_GITHUB_OAUTH_CLIENT_ID
#define AIMEE_DEFAULT_GITHUB_OAUTH_CLIENT_ID ""
#endif

/* Applies to gitlab.com only (the default GitLab host). */
#ifndef AIMEE_DEFAULT_GITLAB_OAUTH_CLIENT_ID
#define AIMEE_DEFAULT_GITLAB_OAUTH_CLIENT_ID ""
#endif

#endif /* OAUTH_DEFAULTS_H */
