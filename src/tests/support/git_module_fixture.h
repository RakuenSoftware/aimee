/* git_module_fixture: bring the real git module up on a real bus, for tests.
 *
 * git_verify_state.c is a seam now -- it asks the git module instead of
 * touching the repository. A suite that exercises the verify gate therefore
 * needs the module attached, or the gate correctly reports "nothing verified"
 * and the suite's assertions fail for a reason that has nothing to do with
 * what it is testing.
 *
 * The module binary's path comes from AIMEE_TEST_MODULE_BIN, set by the make
 * rule that builds it, so a suite does not have to grow an argv contract.
 */
#ifndef AIMEE_TESTS_GIT_MODULE_FIXTURE_H
#define AIMEE_TESTS_GIT_MODULE_FIXTURE_H 1

/* Configure and start the bus, exec the module, and wait until it serves the
 * verify stage. Aborts on failure: a suite that silently continued without the
 * module would report green while testing a fail-closed stub.
 *
 * Idempotent -- a second call while the module is running is a no-op, so a
 * suite may call it from more than one entry point. */
void git_module_fixture_start(void);

/* Stop the module. Registered with atexit() by start(), so a suite normally
 * does not call it; exposed for a test that wants to prove absent-module
 * behaviour. */
void git_module_fixture_stop(void);

#endif
