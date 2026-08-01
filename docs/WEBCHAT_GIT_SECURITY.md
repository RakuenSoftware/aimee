# Browser git security

The browser can register git accounts and clone HTTPS or SSH repositories. Credentials belong to the
authenticated principal and stay in the server vault.

## HTTPS

Access tokens and OAuth results are sealed in the vault. Git receives a credential only for the
matching host and operation. The token is not written into the clone URL, repository config, logs,
or workflow artifact.

## SSH

Private keys stay in sealed runtime storage and are loaded into a short-lived ssh-agent. Git runs
with batch mode, a bounded connection timeout, and a per-principal `known_hosts` file.

First contact uses trust on first use. Verify the host key through another channel for sensitive
repositories. A changed key is a hard failure.

The browser accepts ordinary SSH URL forms without rewriting them to HTTPS when the account uses an
SSH key.

## Repository confinement

Clone destinations are derived from the managed workspace root and canonical repository identity.
User input cannot choose an arbitrary host path. Existing repositories are checked for origin and
ownership before reuse.

Workflow forge operations are narrower still: the supervised peer names a typed operation, while the
server derives repository identity, branch namespace, destination, and credential from the managed
worktree.

## Browser boundary

- login and CSRF protect mutation;
- every account/project route checks the principal;
- account lists return metadata, never secret values;
- project selection is not authorization;
- OAuth polling requires a fresh, unexpired token;
- SSH and token failures are audited without logging the secret.

## Remove an account

Deleting an account removes its vault records and runtime agent state. Existing clones remain source
data but cannot fetch private changes without another authorized credential. Revoke the credential at
the git host as well.
