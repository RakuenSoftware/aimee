#ifndef AIMEE_WORKSPACE_IDENTITY_PRIVATE_H
#define AIMEE_WORKSPACE_IDENTITY_PRIVATE_H
#include <stddef.h>
/* Shared bounded Git metadata read for identity and worktree lifecycle. */
int workspace_identity_git_line(const char *root, const char *arg, char *out, size_t out_len);
#endif
