# Optional module-native tool adapter

The version1 adapter registers owned function definitions, validated policy
metadata and a fixed module callback before the first catalog/definition lookup.
No provider, callback or policy comes from request JSON. At most16 definitions
are copied; names are bounded ASCII identities, schemas at most64KiB, and
builtin, MCP, repeated and duplicate identities refuse. A later conflicting MCP
registration invalidates the module catalog and cannot reroute its calls.

Version1 accepts closed flat object schemas with string and boolean fields,
required properties and bounded string/enum constraints, using the existing
form validator. Other schema types/keywords refuse installation. Numeric
coordinates must be canonical decimal strings; arbitrary numeric JSON is not
accepted through this adapter. No claim of complete JSON Schema support is made.

All three model catalog renderers and fresh execution-definition lookup use the
same owned definitions. Optional source-custody availability filters remain in
force. Canonical tool admission compares the current module definition; normal
role, guardrail, policy/effect and audit paths still precede execution. The
module receives the effective arguments after policy rewrites, validated again
against its current schema. Unknown/noncanonical arguments never reach it.

Callbacks run synchronously outside registry locks with borrowed arguments and
fixed startup state. They return an owned NUL-terminated UTF8 result; they must
not retain the argument pointer, longjmp or exit. Deferred cancellation cleans
the adapter's owned argument buffers. A NULL result always sets error1 even if
the callback cleared the error flag, and is an unavailable/unknown outcome, not
success or permission to retry. No automatic retry occurs. The caller may omit
its error pointer; the callback still receives a valid temporary one.

Absent registration leaves ordinary catalogs and dispatch unchanged. The
retained marker aimee_module_native_tools_contract_v1 equals1. This generic
code contains no memory ontology, Vault, user identity or business schema. Its
metadata is a declaration for ordinary execution policy, not authorization to
any resource; the installed module independently enforces current authority.
