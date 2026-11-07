# Installed Vault handoff fixture

`main.go` is a test-only MCP caller, compiled by `egress-vault-e2e.py`. The gate
pins its executable in a narrowly scoped principal-712 grant. It uses the shipping
Go bus SDK, MCP SSE adapter and egress transport; bearer resolution and HTTP run
inside the application's installed egress process and authenticated Vault helper.
The fixture has no credential input or output API.

Run the installed gate with freshly built application images and a local Docker
engine, plus Go on PATH (or `GO=/absolute/path/to/go`):

```sh
python3 tests/e2e/egress-vault-e2e.py --output /absolute/disposable/output
```

The gate owns both role stacks, volumes and a randomly named `--internal` Docker
network. Its small IPAM subnet has a public address shape so production MCP's
public-destination guard remains active. The provider binds only that owned
network's host gateway; no public provider is contacted. The host running the
gate must be the Docker host, as with the preceding bridge-based fixture.

The gate checks successful GET/POST credential handoff, foreign-handle refusal,
provider authentication failure without output leakage, process hardening,
unattested-helper refusal, credential rotation through the existing helper pipe,
helper-failure recovery and metadata/log secrecy for both KB and Server.

Offline fixture protocol checks:

```sh
python3 tests/e2e/fixtures/egress-vault-probe/test_provider.py
```

These protocol checks and compiling the probe do not qualify the installed
Docker/Vault/process-hardening gate. That requires the full command above.
