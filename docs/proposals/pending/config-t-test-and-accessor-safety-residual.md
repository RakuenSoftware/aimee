# `config_t` test migration and accessor-safety residual

- **State:** PENDING — residual scope only.
- **Archived parent:** [`config-t-encapsulation.md`](../done/config-t-encapsulation.md).

## Delivered foundation

Production is at zero `config_t`/`config_load` users outside the config module. Generated accessors,
the local converter, schema-drift checks, and a ratcheted default lint gate hold that boundary.

## Remaining work

- Replace the remaining test-owned `config_t` construction and direct `config_load` calls with
  isolated config files, snapshots, or narrow test seams; keep the ratchet monotonic.
- Finish the string-accessor audit for callers where an empty value has semantics distinct from
  “unset”.
- Resolve whole-struct or borrowed-pointer hazards that cannot be mechanically rewritten, including
  ownership/lifetime contracts rather than test-only workarounds.
- Separate tolerated unknown keys from invalid known-key shapes before making runtime strict
  validation fatal.
- Remove or replace any whole-struct storage/heap-copy escape that survives outside config.

## Acceptance

The checker reports zero production and test consumers outside config, focused accessor lifetime and
read-failure tests pass, and runtime validation rejects malformed known keys without rejecting or
discarding preserved operator annotations.

