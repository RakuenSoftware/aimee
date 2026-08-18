# Proposal: what the identity family needs before it can migrate

- **State:** OPEN — two decisions belong to whoever owns secrets handling and
  the JTI replay window, not to the migration that reached them first.

`identity` is the smallest reserved DB1 family — four sources, nine
operations — and it is the next one the wire can carry. It is written up rather
than migrated because two of its four sources are not what the family name
suggests, and both questions change behaviour rather than plumbing.

## secrets.c does not touch DB1

It stores each secret as a file under a path, through a pluggable backend:

    static int file_store(const char *service, const char *key, const char *value)
    static int file_load(const char *service, const char *key, char *buf, size_t len)

`db1_secret_store` / `_load` / `_remove` call the backend and never open the
database. The source sits in `src/modules/db1/` and is named `db1_`, and
neither of those makes it DB1 storage.

Migrating it as declared would move secret reads and writes into the module
process: a different uid boundary, a different set of processes that can open
the files, and a different answer to "who can read a secret at rest". That may
well be the right end state — a module that owns secrets is easier to reason
about than a library every binary links — but it is a security decision, not a
mechanical one.

The alternatives are (a) migrate it and accept the process boundary change,
(b) leave it linked in the daemon and take it out of the family's `sources`
because it is not DB1 storage, or (c) move it to a module of its own. Nothing
in the wire prefers any of them.

## The JTI sources do not use the db1_ naming the catalog requires

    server_identity_jti_result_t server_identity_jti_consume(const server_identity_jti_t *token,
                                                             int64_t consumed_at);
    server_management_jti_result_t server_management_jti_consume(...);

Both return an enum with five named values, take a struct of pointers, and have
a `_consume_for_test` twin that takes an extra `live_limit`. Three things
follow:

The catalog requires a `db1_` prefix on every operation's C symbol, which is
how a storage entry point is identified. Renaming these is mechanical and
touches few callers, and it is still a public API rename in the replay-
protection path.

The `_for_test` twin exists to drive the saturation branch with a small limit.
Once the real one is served by the module, the test twin either crosses too --
declaring a test-only operation on a production wire -- or the saturation test
loses its handle. Neither is obviously right.

A replay check that answers "storage error" and one that answers "replay" must
stay distinguishable across the bus, because the caller admits a request on one
and refuses it on the other. The enum return is what carries that, so the
shape is required, not optional.

## What the wire already has for it

The return-beside-a-reply shape (`c_returns: "rc"`) landed with the delegation
family and covers a read whose buffer and whose verdict are separate answers.
Two extensions were written and then reverted rather than committed unused,
because dead generator paths are how a shape gets its first bug:

- the returned value casting to the header's declared type rather than to
  int64_t, which is what an enum-returning domain needs;
- a struct reply that carries the return in one more cell, which is what
  `db1_remote_client_claim` needs — it fills a grant AND says whether the
  claim was new, unbound, bound or owned by somebody else.

Both are small and both are only worth writing once there is an operation to
prove them against.

## Why this is not simply "do it anyway"

The rest of DB1 migrated by preserving each function's exact contract, so a
mistake shows up as a link error or a failing assertion. These two are the
first where the contract is fine and the QUESTION is what the contract should
be. Answering that inside a migration would bury a security decision in a
refactor, and the person who needs to review it would be reviewing 900 lines
of generated client to find it.
