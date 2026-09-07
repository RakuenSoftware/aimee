# Server composition module

The Go `server` module attaches to the core event bus as principal 34. It
uses the same PostgreSQL, Vault, configuration and memory modules as the other
composition. The `server` placement includes this role alone.

First boot publishes a synced, read-only `instance-identity.json` in the instance
home. Subsequent boots must match its role and UUID; there is no config or API
operation that changes identity. Preserve the complete instance home in backups.
This is a startup contract, not protection against an administrator replacing
files on disk. Corrupt or writable identity records fail startup.

The core rejects a conflicting role even when a grant authorizes that principal.
A second live process for the same role is also rejected. Event
`12801` accepts only `{"operation":"identity"}` and returns the persisted
identity. KB connection settings are independent of the Server's identity.

The role also owns process composition through the shared Go module supervisor.
It validates the full manifest before launching anything, requires configuration,
PostgreSQL and memory, rejects the opposite role and duplicate entries, and waits
for the owning bus before attaching modules. Failed modules restart independently;
shutdown terminates their process groups within a bounded grace period.

The image still carries the native protocol/resource hosts for existing C
adapters. Their module selection and lifecycle are supplied by the Go composition;
this change does not rewrite those adapters in Go.
