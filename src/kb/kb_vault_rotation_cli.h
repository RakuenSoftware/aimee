/* kb_vault_rotation_cli.h: `aimee-kb vault rotate|remediate|rotations`.
 *
 *   aimee-kb vault rotate <team_id> <model_id> [--wait]
 *   aimee-kb vault remediate <team_id> <rotation_id> [--wait]
 *   aimee-kb vault rotations <team_id>
 *
 * Runs in-process against KB_STORE as the owner and only files or reads
 * requests; the running daemon performs the rotation (see
 * kb_vault_rotation_worker.h). --wait follows the request until the rotation
 * retires or fails. Output is JSON on stdout. */
#ifndef AIMEE_KB_VAULT_ROTATION_CLI_H
#define AIMEE_KB_VAULT_ROTATION_CLI_H 1

#include "kb_identity.h"

/* argv[2] is the subcommand. Returns the process exit status. */
int kb_vault_rotation_cli(int argc, char **argv, const kb_principal_t *owner);

/* 1 when argv[2] names one of these subcommands. */
int kb_vault_rotation_cli_handles(const char *subcommand);

#endif
