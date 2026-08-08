# Proposal: the server serves its own thin client

- **State:** PENDING — design only, no code in this PR. Two of its three
  preconditions already landed (see *Where this starts from*); the third
  (release signing) does not exist yet and gates the final stage.
- **Owns:** the distribution edge for the thin-client **binary** — how a client
  obtains a build matched to the Runtime it talks to, and the trust model under
  which a client may replace its own executable.
- **Consumes (does not redefine):** the client↔Runtime registration edge and its
  generation-stamped projection
  ([`thin-client-capability-advertisement.md`](thin-client-capability-advertisement.md));
  the existing self-update machinery (`cmd_self_update.c`, `update_mode:`); the
  transport and principal classes of
  [`mtls-client-identity.md`](../done/mtls-client-identity.md).
- **Author:** JBailes
- **Date:** 2026-08-08

## Thesis

A thin client and its Runtime are a 1:1 pair whose halves are released on
different schedules, and nothing delivers the matching half. The client can now
*notice* it is behind, and CI can now *publish* a current build, but the binary
that would fix the drift already sits inside the server image and is never
offered to the client that needs it.

This is not a hypothetical. On 2026-08-08 a client seven days behind its server
rendered nothing at all for commands whose printers existed only server-side —
`exit 0`, no output, no signal. It was diagnosed first as a vault gate bug and
then as a remote read-scoping gap before anyone compared build dates. The whole
time, `/usr/local/bin/aimee` **inside the server container** was the exact
binary that would have fixed it.

## Where this starts from

Three things must be true for "the server keeps its clients current" to work.
Two are already true:

1. **Detection.** The drift check orders a non-semver `testing-<sha>` pair by
   `commit_time` from `server.info`, and says so at session start. *(Landed —
   PR #2417.)*
2. **Obtainability.** `publish-testing.yml` publishes a Linux thin client on the
   same cadence as the images, so a current build exists between releases.
   *(Landed — PR #2418.)*
3. **Delivery + trust.** The server hands over a matched binary and the client
   may install it. **This proposal.**

The server image already carries a matched client — `Dockerfile.server` installs
it at `/usr/local/bin/aimee`, stamped with the same version as the server. No
new build pipeline is required for the common case; the artifact exists.

## The decision that actually matters

**A server that hands its clients executables is remote code execution by
design.** Today a compromised Runtime can serve wrong data and read whatever the
vault gates let it read. Under naive auto-replacement it additionally owns every
machine that runs `aimee`, as that user, silently, on the next invocation. That
is a categorical escalation of what compromising a Runtime is worth, and it is
the whole design problem — the file transfer is trivial.

Therefore:

**The server is a transport, never an authority, over client binaries.**

The bundle is signed in CI with a release key the Runtime never holds; the
client verifies against a public key compiled into itself. A compromised server
can then serve only genuine, signed builds — it can withhold or delay an update,
which is a denial-of-service, not a takeover. Signing does not exist today
(`cmd_self_update.c` verifies a SHA-256 published by the GitHub API — keyless,
and explicitly noted there as pending a key-based signature), so **stage P3 is
blocked on it and must not ship without it.**

## Decisions

1. **Serve what the image already has.** `GET /v1/client/manifest` returns
   `{version, commit_time, platforms:{<asset>:{sha256,size}}, signature}`;
   `GET /v1/client/download?platform=<asset>` streams the bytes. The manifest is
   the authority on what is on offer; the download is dumb.
2. **Signature before installation, always.** Verify the manifest signature
   against the compiled-in public key, then the artifact's SHA-256 against the
   manifest, then execute the fetched binary and require it to report the
   expected version. Any failure aborts and leaves the running client untouched.
   Order matters: the signature gates the digest, not the reverse.
3. **Monotonic.** Refuse to install a build whose `commit_time` is older than
   the running client's. Without this, "the server decides" includes "the server
   decides you should go back to a version with a known hole."
4. **Never escalate to install.** If the install path is not writable by the
   invoking user, print the instruction and stop. A self-update that prompts for
   privilege is a phishing lesson taught by our own tooling.
5. **Staged, and the stages are separable:**
   - **P2 — `aimee self-update --from-server`.** Explicit, user-invoked, verified
     against the manifest. No ambient authority; the user chose the moment. This
     is shippable before signing exists, because the human is the gate.
   - **P3 — automatic under `update_mode: apply`.** Same path, no prompt.
     **Requires signing.** Opt-in, never the default.
6. **Platform honesty.** The image ships a Linux client of the image's own
   architecture. The manifest advertises only what is actually present; a macOS
   or Windows client asks, is told there is no artifact for it, and is pointed at
   the release channel. Silently offering nothing and silently offering the wrong
   architecture must not look the same.

## Non-goals

- Replacing the release channel. Releases stay the source of truth for tagged
  versions and for the platforms the image does not carry.
- Feature negotiation. Which *capabilities* a client may use is owned by
  [`thin-client-capability-advertisement.md`](thin-client-capability-advertisement.md);
  this proposal is only about the bytes of the binary.
- Server self-update. Runtimes are deployed by their operator's image channel.

## Open questions

1. **Where does the signing key live, and who can use it?** A release-signing key
   usable by any CI job is a key an attacker with CI access can use. This likely
   wants a protected environment mirroring the existing `release` gate.
2. **Key rotation.** A public key compiled into old clients cannot be rotated by
   fiat. Probably: ship a small trusted set, and treat rotation as a release
   event with an overlap window.
3. **Does P2 want the manifest at all, or just `server.info`?** `server.info`
   already reports version and `commit_time`. The manifest earns its place only
   when there is more than one artifact or a signature to carry — which is
   exactly P3. P2 may be able to skip it.
4. **Is `:testing` a legitimate update source?** A floating channel that moves on
   every merge is not obviously something a client should chase automatically,
   even when the operator pinned their server to it.

## Acceptance criteria

- A client older than its server can obtain and install the matched binary from
  that server in one command, with the signature and digest verified, and can be
  shown to refuse each of: a bad signature, a bad digest, an older
  `commit_time`, and a non-writable install path.
- The refusal paths are exercised by tests that fail when the corresponding check
  is removed — mutation-verified, not merely present.
- A macOS or Windows client against a Linux server image receives an explicit
  "no artifact for your platform" and a pointer to the release channel.
