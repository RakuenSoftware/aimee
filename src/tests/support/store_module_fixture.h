/* store_module_fixture: bring the real store module up on a real bus, for tests.
 *
 * The C store is gone. Every db1_* call in the tree is a bus client now, and a
 * suite that exercises code READING through the store -- memory maintenance
 * policy, trajectory batching, the management nonce lifecycle, the write-tier
 * replay, search-result fusion -- gets a failed call rather than an answer
 * unless the module is attached. Those suites used to stand up an in-process
 * SQLite database with db1_init(":memory:"); this is what replaces that.
 *
 * The module binary's path comes from AIMEE_TEST_MODULE_BIN, set by the make
 * rule that builds it, exactly as git_module_fixture does.
 *
 * UNLIKE the git module, this one needs a real PostgreSQL. That is why start()
 * is not the whole interface: `make unit-tests` must stay runnable on a machine
 * with no database, so a suite asks available() first and skips itself when the
 * answer is no. A skip is honest; a green run against a store that answered
 * nothing is not.
 */
#ifndef AIMEE_TESTS_STORE_MODULE_FIXTURE_H
#define AIMEE_TESTS_STORE_MODULE_FIXTURE_H 1

/* Can this machine run a store-backed suite?
 *
 * Yes when AIMEE_STORE_URL names a database. Prints one line when it does not,
 * so a skipped suite says why rather than just passing quietly.
 *
 * Nothing else is needed. The module embeds its schema and applies it at
 * startup, so an EMPTY database is a fine target and no postgres client has to
 * be installed on the machine running the tests. */
int store_module_fixture_available(void);

/* Start the module and wait until it serves. It creates its own schema.
 *
 * Aborts on failure. Once available() has said yes, a failure here is a real
 * fault -- an unreachable database, a schema that will not apply, a module that
 * exits -- and a suite continuing past it would test a store that answers
 * nothing while reporting green.
 *
 * Idempotent: a second call while the module is running is a no-op.
 *
 * The module's schema application is idempotent, so a database that already has
 * the tables is a fine target too. The suite still owns the ROWS:
 * this does not truncate, because a fixture that wiped the database would be a
 * loaded gun pointed at whatever AIMEE_STORE_URL happened to name. */
void store_module_fixture_start(void);

/* Stop the module. Registered with atexit() by start(), so a suite normally
 * does not call it; exposed for a test that wants absent-module behaviour. */
void store_module_fixture_stop(void);

#endif
