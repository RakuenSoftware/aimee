store: it runs — 17 modules, 93 tables, 21 migrations on a clean container

The previous commit made the SQL stage exist and proved it under a fixture that
spawns the store standalone. That is not the claim. The claim is the store
working under aimee-server and aimee-kb with the fleet up, and it did not: the
end-to-end run failed four different ways before it passed.

    == the module fleet ==
            module processes: 17
    PASS  the supervisor started modules (17)
    PASS  the store module is running

    == the store schema ==
    PASS  the store module created its schema (93)
    PASS  migrations were recorded (21)

    PASS=11 FAIL=0

21 rows in the ledger under owner db1 at version 21, with 21 DISTINCT checksums
-- one hashed per version as applied, not one copied.

THE WIRE WAS EXERCISED, NOT JUST THE SCHEMA. Applying the schema proves MIGRATE
and CURRENT_VERSION, which take their own path in the stage; EXEC and QUERY go
through h.statement, which none of it touches, and a broken statement path lets
the schema apply and fails everything after. Measured on the database rather
than guessed at through endpoints: driving the daemon moved xact_commit 163->168
and tup_returned by 242, the store module held two connections as root, and
pg_stat_activity caught a family query mid-flight returning rows.

Four findings, all real:

  - THE SUPERVISOR GIVES NO ORDERING GUARANTEE. It starts modules back to back
    and each registers asynchronously, so the store called before postgres had
    claimed 11266 and exited. A bounded wait now: thirty attempts, one second
    apart, retrying only ErrStoreUnavailable. Ordering the manifest would not
    fix it -- registration is async, so a window that closes on a fast machine
    reopens on a loaded one.
  - THE FIRST VERSION OF THAT WAIT RAN ZERO TIMES. It wrapped storeBackend,
    which talks to nothing: construction succeeded and the failure was one line
    later in ApplySchema, the first call that leaves the process. The run failed
    identically, which is how it was caught -- a fix that changes nothing looks
    exactly like a fix that was not deployed.
  - THE RIG NEVER CREATED THE ROLE THE DAEMON CONNECTS AS. The DSN carries no
    user, so libpq uses the process's OS name, and PostgreSQL answered SQLSTATE
    28000 role "root" does not exist. It could not have arisen before: the C
    store was a SQLite file with no roles. That error was also the first proof
    the stage worked -- a real SQLSTATE and message came back through the reply
    frame, which is why they are carried rather than collapsed into "failed".
  - install-modules ONLY CREATES MISSING NAMES, so redeploying a rebuilt binary
    left all seventeen names on the old one and reported "0 installed / 18
    present", which reads like success. Third instance of that trap here; the
    running binary's checksum is now compared against the local one.

Also in this commit: postgres placements sorted the way the contract means
(server, kb -- deployment order, not alphabetical, which the validator caught);
the outbound client grant pointed at the executable the process actually runs as
rather than one written from memory, since the runtime pins the resolved path;
and the migration ledger named schema_migrations, which is what the end-to-end
probe already said it would be -- nothing had ever created that table, so the
probe was the only statement of its name.

Still not covered, recorded at both sites and in the validation record: every
guardrail event the bus accepts reaches the store exactly once and a graceful
stop drains those in flight. It needs a test against the Go sink that does not
depend on the bus it verifies.

go test ./... , make -C src unit-tests and make -C src lint all exit 0.
Container destroyed and verified gone.

Claude-Session: https://claude.ai/code/session_011GbyoTHDrsxuq8ohM8YxFY
