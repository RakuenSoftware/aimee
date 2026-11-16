# Exact code-index generation coordinates

Blast-radius generation fields and graph-sync generation IDs represent positive
signed 64-bit identities. Consumers accept canonical decimal strings and exact
integer tokens retained by the vendored cJSON parser. They reject fractional or
exponent tokens, overflow, nonpositive generations and already-rounded unsafe
constructed doubles. A retained token is used only while its parsed numeric value
and zero sign still match the node; explicit mutation cannot resurrect an old ID.

The existing portable `jo_read_i64_exact` contract is unchanged. Its additive
`jo_read_i64_token_exact` counterpart handles original parsed integer tokens.
Graph-sync responses keep JSON numbers for safe integers and use canonical decimal
strings beyond ±(2^53−1), via the existing portable encoder. Edge counts remain
separate nonnegative integers and use the same lossless wire encoding.

Opaque native FieldRef/Version and Region objects are forwarded unchanged through
cJSON parsing, recursive duplication and printing. Their original integer tokens
are preserved; this i64 helper does not reinterpret u64 beliefs. The focused
`unit-test-kb-client-index-remote` test calls the actual KB graph-sync response
producer, client and server handler, plus the actual HTTP blast response encoder
and client. Storage/HTTP are fixture transports in this encoding test, not native
authorization evidence. It also covers an opaque u64 maximum belief and
adjacent nanosecond endpoints as transport evidence, not a claim that this test
qualifies the native source admission or provider guard lifecycle.
