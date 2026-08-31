/* git_pr_api_stub.h: control surface for the test PR-API stub.
 * See git_pr_api_stub.c. */
#ifndef AIMEE_TEST_GIT_PR_API_STUB_H
#define AIMEE_TEST_GIT_PR_API_STUB_H

#include "modules/git/git_pr_api.h"

/* The verdict git_pr_ci_via_api_slug() hands the merge gate. The forge read is the
 * only part of the gate that is stubbed -- the ruling over this value is linked for
 * real from git_pr_ci_grade.o -- so setting it drives the gate through real policy.
 *
 * Defaults to GIT_PR_CI_ERROR, so a test that never calls this proves the gate
 * fails closed on an unreadable forge without any setup. */
void git_pr_api_stub_set_ci(git_pr_ci_t verdict);

#endif /* AIMEE_TEST_GIT_PR_API_STUB_H */
