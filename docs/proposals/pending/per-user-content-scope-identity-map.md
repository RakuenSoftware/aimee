# The KB boundary already has an identity; preserve it for content scope

Amends `per-user-content-scope-reader-identity.md`. The earlier framing treated every ingress surface
as if it connected to aimee-kb and therefore needed to carry independently verifiable proof to it.
That is the wrong boundary: CLI, web and MCP terminate at aimee-server. Only aimee-server connects to
aimee-kb.

No backwards compatibility is required: the next release moves a full version, so a service
connection or request context that does not satisfy the existing identity contract is refused rather
than accommodated.

## One KB service boundary, with the standard triple layer on every network hop

| ingress surface | authenticated at | KB connection |
|---|---|---|
| local CLI over UDS | aimee-server, using the local OS/peer credential | none; the CLI calls aimee-server |
| remote CLI / thinclient | aimee-server, using mTLS + rotating bearer + PAM/OIDC | none; the thinclient calls aimee-server |
| browser / webchat | aimee-server, using the configured standard web/PAM/OIDC flow | none; the web tier calls aimee-server |
| remote MCP | aimee-server, using standard MCP over mTLS + rotating bearer + PAM/OIDC | none; MCP calls aimee-server |

Those protocols keep their own authentication responsibilities. Content scope must not manufacture a
second CLI, web or MCP proof protocol at the KB boundary.

Every networked Aimee-to-Aimee hop uses that same triple layer. In particular, a thinclient has full
mutual TLS, rotating token bearing and PAM/OIDC identity to aimee-server; aimee-server then uses the
same three-layer rule to aimee-kb. Each mTLS relationship has unique, role-constrained certificate
material and rotates independently. The certificate identity aimee-server presents as a client to
aimee-kb is not the server certificate it presents to a thinclient, and neither is accepted in the
other role or on another peer relationship. The local UDS is not a weaker network fallback: it is the
host-local boundary, authenticated by the OS, and physical host compromise is outside its threat
model.

## The server-to-KB connection is already the KB proof boundary

Every aimee-server-to-aimee-kb connection is required to satisfy all three existing layers:

1. **Mutually verified TLS.** Each side verifies the other using unique pair/role certificate
   identities. Certificates are independently revocable and rotated; material is never reused for a
   different peer or for both client and server roles.
2. **A rotating bearer.** Every request is token-bearing; expiry, rotation and revocation remain part
   of the existing credential lifecycle.
3. **An enrolled service identity.** When OIDC is configured, aimee-server uses an OIDC identity that
   aimee-kb verifies through the federated issuer. Without OIDC, it uses a PAM identity that exists on
   aimee-kb. Failure to resolve that identity fails closed.

An external caller therefore does not become a KB principal by learning a username, reaching one
ingress protocol or compromising one side of a connection. The peer independently authenticates the
other enrolled system, and the request still needs the rotating token and PAM/OIDC identity. An
Aimee-specific compromise of the communication presupposes that both systems, and therefore the
boundary itself, are already compromised. Adding a fourth credential allowed to “speak for host
subjects” does not improve that state and creates another authority to enroll, rotate and audit.

## Connection identity and caller context are different things

The three layers above authenticate the **service connection**. The user who caused a request is
**caller context** on that connection:

- aimee-server validates the caller at its ingress boundary;
- it carries the resulting principal on the already-authenticated KB request;
- aimee-kb resolves that principal's teams and projects from its own membership data;
- no caller-supplied team or widened membership is accepted.

OIDC caller context can retain its existing KB-verifiable token. Local CLI, web/PAM and MCP context is
carried by the authenticated aimee-server service rather than being forced through a new proof type.
This is delegation across an authenticated service boundary, not anonymous trust.

A token-mint round trip based on the same already-authenticated context adds ceremony without changing
the authority. This proposal does not add a new protocol for a threat that requires both Aimee
endpoints to have already been lost. Likewise, an attacker who has obtained physical access
sufficient to impersonate a local UDS caller is outside the local-CLI threat model. Standard web and
MCP authentication remain owned by those ingress layers.

## Consequences

- **The local CLI does not go dark.** It has an OS-attested caller at aimee-server, and its KB request
  uses the same authenticated service channel as every other surface. It needs caller-context wiring,
  not a KB exemption or a new credential.
- **MCP is not a new KB principal kind.** It inherits the authenticated `/v1` request context selected
  by aimee-server.
- **Web is not a new KB proof problem.** Standard web/PAM/OIDC authentication terminates before the
  server opens the KB request.
- **Background work remains separate.** Ingest, re-embed, curator and code-indexer authorization is
  still the open question from #2646. It must be answered before content scope is enabled, but it is
  not evidence that CLI, web or MCP needs another authentication layer.

## Bounded slices

Slices 1–5 land while content RLS remains disabled, so propagating and resolving context cannot
change an observable content result. Slice 6 only declares the readers ready; scoping becomes
observable when an operator subsequently enables content scope.

1. **Pin the existing connection invariant in integration tests:** every network hop in a content
   call, including thinclient-to-server and server-to-KB, requires verified mTLS with unique rotating
   pair/role certificates, a valid rotating bearer and a resolved OIDC-or-PAM identity.
2. **Carry the existing caller context** from aimee-server on content reads. Do not add a new principal
   kind, host-subject credential or token-mint hop.
3. **Resolve service and caller context together** through the existing KB identity/team resolver,
   rejecting conflicts and caller-named membership that the KB does not contain.
4. **Open content tenant scope** from that resolved context and clear it on every pooled-connection
   exit.
5. **Exercise every server ingress:** web, local CLI, remote CLI/thinclient, and MCP. None may create a
   direct or weaker KB authentication path. Land the separately reviewed #2646 background-work
   decision before readiness is declared.
6. Only then set `kb_meta.content_scope_reader_ready = '1'`.

## Acceptance checks

- **Three layers, fail closed on every network hop.** Missing or invalid mTLS, bearer, or PAM/OIDC
  identity prevents thinclient-to-server and server-to-KB content requests. Rotation and revocation
  are exercised on pooled connections.
- **Pairwise certificate identity.** The thinclient-to-server and server-to-KB relationships use
  different certificate material, rotate independently, and reject cross-peer or cross-role reuse.
- **Federation modes.** With OIDC configured, aimee-kb verifies aimee-server's federated identity.
  Without OIDC, it resolves the configured aimee-server PAM identity that exists on the KB host.
- **One KB caller, every ingress preserved.** Web, local CLI, remote CLI/thinclient, and MCP all reach
  content through aimee-server; none introduces a direct KB credential or route, and each is tested
  by name to prove its expected caller context reaches the KB read rather than silently becoming
  nobody.
- **No widening.** The KB, not aimee-server or the ingress caller, remains authoritative for team and
  project membership.
- **No inheritance.** A request with no caller context cannot inherit the previous request's principal
  on a pooled connection.
- **Background decision remains explicit.** The separately selected #2646 policy is implemented and
  tested before the readiness marker is set; this proposal does not preselect it.

## Status

Pending. Supersedes the earlier proposal's proof-per-surface framing. The remaining work is identity
context propagation and tenant scoping over the existing authenticated service boundary, not a new
authentication mechanism.
