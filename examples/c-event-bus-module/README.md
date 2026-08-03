# External C module template

Copy this directory into a module's own repository. It consumes the installed
`aimee-core` package and links only `aimee-core-event-bus-client`; it does not
link server, KB, database, host, routing, or TLS implementation objects.

The daemon's module launcher supplies the local attach socket and an opaque
two-part principal identity. The daemon authenticates that identity and binds
the module's declared publish/subscribe/serve kinds in its host attach hook
before granting any mappings. The attach socket is closed immediately after
the handshake; events then use the module's shared-memory queue pair.

The example supplies only a stage map and handler. The installed core runtime
owns the attach loop, module envelope, correlation, heartbeats, deadlines,
cancellation, and shutdown. Production handlers should call
`aimee_module_invocation_cancelled()` between bounded units of expensive work.
