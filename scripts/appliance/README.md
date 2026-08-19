# Appliance verification for the served thin client

These check the thin-client claim against a **real running aimee-server**, not an
in-process harness: that what the client puts on the wire is decided by what the
server serves.

Run them in a disposable container. Never against a live appliance, and never in
a container another session is using.

## Why these exist

`test_cli_argspec` compares the spec interpreter against the compiled marshaller.
That is the right check for "does the spec describe the marshaller", and it is
blind to two things by construction:

1. **Whether the running server actually serves the spec.** The test includes the
   same data header the emitter does, so both agree even if the route is broken.
   `check_served_manifest.sh` fetches `/v1/cli/manifest` over HTTP from a real
   server and looks for the specs and the vocabulary items in them.

2. **What survives the wire.** A spec and a marshaller can agree and both be
   wrong about the value that arrives. `wire_recorder.py` proxies the client to
   the server and records the request bodies; `wire_assertions.sh` then asserts
   on what was actually sent.

The second one earned its place. `memory.delete` shipped with a spec saying
`atoi` where the marshaller calls `atoll()`, and against the shipped binary:

```
  typed id     -> id actually sent
  2147483647   -> 2147483647
  2147483648   -> -2147483648
  4294967296   -> 0
  4294967297   -> 1            <-- deletes memory 1
```

atoi() keeps the low 32 bits as a signed int, so the wrap lands near zero — on
low-numbered, early rows. The differential test could not see it: its samples
come from the spec, and every id it generated was small.

It also pinned a ceiling no spec can lift: a JSON number is a double, so an id
above 2^53 cannot round-trip regardless of which C parse the spec names.

## The A/B that demonstrates the actual claim

Hold the client binary **constant** and swap only the server:

```
=== A: OLD server, client UNCHANGED
    wire: id = 0
=== B: NEW server, same client binary
    wire: id = 4294967296
```

The client did not change. If its behaviour still changes, it is obeying the
server — which is the whole point of the thin client, and the reason a fix like
this ships without touching a single installed client.

## Running them

Copy the three files into the container, start a server with a TCP listener
(`aimee.api.http_port` + `bearer_token`), then:

```sh
bash check_served_manifest.sh        # does the running server serve the specs?
bash wire_assertions.sh              # does the client send what they say?
```

Point the client at the recorder with `AIMEE_SERVER_URL` / `AIMEE_SERVER_TOKEN`
(not `AIMEE_API_*` — that pair is read by other tooling and leaves the client
unauthenticated, which shows up as `manifest -> 401` and a fallback to the
client's compiled tables, quietly testing the wrong thing).
