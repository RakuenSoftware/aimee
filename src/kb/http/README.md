# KB HTTP boundary

This directory owns the public `aimee-kb /v1` handlers, listener, authentication, scope checks, body
limits, TLS, health, and OpenAPI serving.

Add new KB HTTP behavior here. Define the OpenAPI operation and route descriptor first; call a typed
KB owner below the handler. Do not put SQL, server internals, browser policy, or client-path reads in
the HTTP layer.

Run `make -C src api-conformance-check` and the narrow KB route tests after a change.
