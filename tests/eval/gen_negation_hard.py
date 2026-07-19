#!/usr/bin/env python3
"""Deterministic generator for the HARD memory_negation validation corpus.

Design goals (vs the original 22-fixture corpus whose base recall@5 was already 1.0):
  1. ~130 fixtures so top-k retrieval is selective (headroom for recall).
  2. Each negation fact is paired with a POSITIVE twin that is LEXICALLY
     NEAR-IDENTICAL -- only the polarity clause differs -- so a bag-of-words /
     hash-embedding retriever cannot separate the pair. Discrimination then
     depends ENTIRELY on negation-awareness. This is where the A/B signal lives
     (MRR / recall@1 on the `dis` cases): the current append-only negation path
     cannot reorder, a reorder-aware path (Ai) should.
  3. Many topically-adjacent distractors so the pair members compete against a
     crowded neighbourhood, not just each other.

No randomness: output is byte-stable across runs.
Usage: python3 tests/eval/gen_negation_hard.py > tests/eval/memory_negation_corpus_hard.json
"""
import json

fixtures = []
cases = []

# 16 near-identical negation/positive pairs. Each pair shares an identical
# stem; only the polarity clause differs. neg fid = n0XX, pos fid = n1XX.
PAIRS = [
    ("redis-persistence", "Redis in the checkout service",
     "is configured without on-disk persistence, so keys are not written to disk",
     "is configured with on-disk persistence, so keys are written to disk",
     "which service runs redis without on-disk persistence",
     "the checkout redis that does not write keys to disk",
     "the checkout redis that writes keys to disk"),
    ("postgres-ssl", "The reporting database connection",
     "does not use SSL and sends traffic unencrypted over the socket",
     "does use SSL and sends traffic encrypted over the socket",
     "reporting database connection that does not use ssl",
     "reporting db link with no ssl on the socket",
     "reporting db link with ssl on the socket"),
    ("metrics-auth", "The internal metrics endpoint",
     "requires no authentication and is served without any token check",
     "requires authentication and is served with a token check",
     "metrics endpoint that requires no authentication",
     "internal metrics route with no token check",
     "internal metrics route with a token check"),
    ("job-retry", "Failed indexing jobs in the pipeline",
     "are never retried automatically and must be requeued by hand",
     "are always retried automatically by the scheduler on failure",
     "indexing jobs that are never retried automatically",
     "pipeline indexing jobs not retried without a human",
     "pipeline indexing jobs retried automatically"),
    ("cdn-purge", "The image CDN cache",
     "is not purged on deploy and entries expire only by TTL",
     "is purged on deploy and entries are refreshed immediately",
     "image cdn cache that is not purged on deploy",
     "image cdn that does not purge on deploy",
     "image cdn that purges on deploy"),
    ("staging-email", "The preview environment mailer",
     "does not send real email and captures messages in a local sink",
     "does send real email and delivers messages to real inboxes",
     "preview mailer that does not send real email",
     "preview environment that never sends real mail",
     "preview environment that sends real mail"),
    ("embedder-gpu", "The sentence embedder worker",
     "uses no GPU and runs entirely on CPU cores",
     "uses a GPU and offloads work from CPU cores",
     "embedder worker that uses no gpu",
     "sentence embedder running without a gpu",
     "sentence embedder running with a gpu"),
    ("backup-encryption", "The nightly volume backups",
     "are not encrypted and sit in plaintext on the disk",
     "are encrypted and sit as ciphertext on the disk",
     "nightly backups that are not encrypted",
     "nightly volume backups without encryption",
     "nightly volume backups with encryption"),
    ("api-ratelimit", "The partner search API",
     "is not rate limited and accepts unbounded request bursts",
     "is rate limited and rejects request bursts past the cap",
     "partner search api that is not rate limited",
     "partner api with no rate limit on bursts",
     "partner api with a rate limit on bursts"),
    ("ws-compression", "The realtime websocket channel",
     "does not use compression and sends frames uncompressed",
     "does use compression and sends frames compressed",
     "websocket channel that does not use compression",
     "realtime channel sending frames without compression",
     "realtime channel sending frames with compression"),
    ("audit-logging", "Configuration changes in the console",
     "are not written to the audit log and leave no trail",
     "are written to the audit log and leave a full trail",
     "console changes that are not written to the audit log",
     "console config edits with no audit trail",
     "console config edits with an audit trail"),
    ("request-tracing", "The gateway request path",
     "has no distributed tracing and emits no spans",
     "has distributed tracing and emits detailed spans",
     "gateway path that has no distributed tracing",
     "gateway requests emitting no trace spans",
     "gateway requests emitting trace spans"),
    ("auto-migrate", "Database schema migrations at boot",
     "are not applied automatically and wait for a manual run",
     "are applied automatically by the boot sequence itself",
     "schema migrations that are not applied automatically",
     "boot migrations not applied without a human",
     "boot migrations applied automatically"),
    ("flag-cache", "The feature flag values",
     "are not cached and are read fresh on every request",
     "are cached and are reused across requests",
     "feature flags that are not cached per request",
     "feature flag reads with no caching",
     "feature flag reads with caching"),
    ("cors-policy", "The public asset host",
     "does not allow cross-origin requests from third-party sites",
     "does allow cross-origin requests from third-party sites",
     "asset host that does not allow cross-origin requests",
     "public assets with no cross origin access",
     "public assets with cross origin access"),
    ("read-replica", "The orders database tier",
     "has no read replicas and serves all reads from the primary",
     "has read replicas and serves reads off the replicas",
     "orders database that has no read replicas",
     "orders tier serving reads without replicas",
     "orders tier serving reads from replicas"),
]

for i, (subj, stem, negc, posc, negq, disq, posq) in enumerate(PAIRS, start=1):
    nfid = f"n{i:03d}"
    pfid = f"n1{i:02d}"
    fixtures.append({"fid": nfid, "tier": "L2", "kind": "fact",
                     "key": f"{subj} negative", "content": f"{stem} {negc}."})
    fixtures.append({"fid": pfid, "tier": "L2", "kind": "fact",
                     "key": f"{subj} positive", "content": f"{stem} {posc}."})
    cases.append({"id": f"neg{i:02d}", "query": negq, "expected": [nfid],
                  "difficulty": "hard", "notes": f"negation recall: {subj}"})
    cases.append({"id": f"dis{i:02d}", "query": disq, "expected": [nfid],
                  "difficulty": "hard",
                  "notes": f"discriminate {nfid} (neg) from {pfid} (pos twin)"})
    cases.append({"id": f"pos{i:02d}", "query": posq, "expected": [pfid],
                  "difficulty": "easy", "notes": f"positive control: {subj}"})

DISTRACTOR_TOPICS = [
    "service {n} health checks run every {k} seconds against the readiness probe",
    "the {svc} worker pool scales to {k} replicas under sustained load",
    "cache tier {n} evicts entries with a least-recently-used policy at {k} megabytes",
    "the {svc} queue drains in batches of {k} messages per poll",
    "connection pool {n} caps out at {k} concurrent database sessions",
    "the {svc} cron job compacts old partitions once every {k} hours",
    "region {n} replicates object storage to a cold tier after {k} days",
    "the {svc} sidecar exports prometheus metrics on port {k}",
    "index {n} is rebuilt nightly and holds about {k} thousand documents",
    "the {svc} rollout uses a canary of {k} percent before full release",
    "shard {n} keeps a hot working set of roughly {k} gigabytes in memory",
    "the {svc} token issuer rotates signing keys every {k} days",
    "bucket {n} lifecycle rules archive logs older than {k} weeks",
    "the {svc} ingest path validates payloads against schema version {k}",
    "timer {n} flushes buffered writes to durable storage each {k} seconds",
]
SVCS = ["billing", "search", "auth", "notify", "ledger", "media", "graph", "sync"]
n = 0
for topic in DISTRACTOR_TOPICS:
    for j in range(8):
        n += 1
        svc = SVCS[j % len(SVCS)]
        k = 5 + (n * 7) % 90
        content = topic.format(n=n, k=k, svc=svc)
        fixtures.append({"fid": f"d{n:03d}", "tier": "L2", "kind": "fact",
                         "key": f"distractor {n}",
                         "content": content[0].upper() + content[1:] + "."})

corpus = {
    "version": 1,
    "description": ("HARD validation corpus for memory_negation. Each of 16 negation facts "
                    "(nNNN) is paired with a lexically near-identical POSITIVE twin (n1NN) that "
                    "differs only in polarity, so a bag-of-words/hash-embedding retriever cannot "
                    "separate the pair -- discrimination depends entirely on negation-awareness. "
                    f"{n} topically-adjacent distractors make top-k selective. Watch MRR / "
                    "recall@1 on the `dis` cases: that is where negation ON should beat OFF once "
                    "the query path can REORDER (Ai), not just append. Positive controls (pos) "
                    "must not regress. Not part of the gated golden."),
    "fixtures": fixtures,
    "cases": cases,
}
print(json.dumps(corpus, indent=2))
