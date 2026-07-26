# Debian 13 (trixie) for libpq 17. The kb links db2/vault_operator_status_runtime.c,
# which uses the PostgreSQL 17 async-cancel API (PQcancelCreate / PGcancelConn /
# PQcancelPoll). Bookworm ships libpq 15, where those symbols do not exist, so the
# kb build failed here with -Werror=implicit-function-declaration while the native
# CI build stayed green -- `make all server` does not compile KB_SRCS, so this
# image was the only place that file was ever built.
FROM debian:trixie-slim AS build

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        clang \
        git \
        libp11-kit-dev \
        libpq-dev \
        libssl-dev \
        libzstd-dev \
        pkg-config \
        python3 \
        zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .
ARG AIMEE_VERSION=""
# Ship the tree-sitter extraction front-end: fetch + sha256-verify the pinned
# grammars (scripts/fetch-treesitter.sh; git + network needed only in this trusted
# build stage), then build the kb with AIMEE_TREESITTER=1 (real-AST C/C++ class/
# method extraction). See docs/proposals/pending/cpp-class-method-extraction.md.
RUN sh scripts/fetch-treesitter.sh \
    && make -C src ../aimee-kb -j"$(nproc)" AIMEE_TREESITTER=1 ${AIMEE_VERSION:+GIT_VERSION=v$AIMEE_VERSION}

# Runtime must match the build stage: libpq5 here has to provide the same
# PostgreSQL 17 symbols the kb was linked against.
FROM debian:trixie-slim

# python3 (stdlib only) runs the sidecar clients the kb popens: embed-remote.py
# (-> embedder service), llm-chat.py + learning-synthesize.py + curator-extract.py
# (-> LLM endpoint). No torch here — the model lives in the embedder service.
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        curl \
        libpq5 \
        libssl3 \
        libzstd1 \
        postgresql-17 \
        postgresql-17-pgvector \
        python3 \
        zlib1g \
    && rm -rf /var/lib/apt/lists/*

ENV AIMEE_HOME=/var/lib/aimee
# DB2 is unset on purpose. Unset means "the operator configured nothing", and the
# entrypoint then runs the in-image PostgreSQL 17 + pgvector cluster under
# $AIMEE_HOME/postgres. Setting it to any URL selects an external server and the
# entrypoint starts nothing — that path is fully supported and unchanged.
#
# This used to default to postgresql://aimee:aimee@postgres:5432/aimee_shared,
# which made "nothing configured" indistinguishable from "use the sibling
# container" and left a bare `docker run` pointed at a host that does not exist.
# compose.yaml and compose.server.yaml set AIMEE_DB2_URL explicitly, so stacks
# using them keep their own postgres service exactly as before.
# No baked embedder/LLM endpoint defaults. The kb runs NO model runtime; it calls
# an external aimee-llm container (CPU/GPU) or endpoint. Point it with ONE of:
#   AIMEE_LLM_URL       unified container -> embed + rerank + synth (one knob)
#   AIMEE_EMBEDDER_URL  pin the embedder (/embed) independently
#   AIMEE_RERANKER_URL  pin the reranker (/rerank) independently
#   LLM_ENDPOINT        Tier-A synth only (small-model interface)
# Unset, embeddings fall back to the 384-dim builtin (test/shim only). The deploy
# unit (compose / smoothnas plugin) sets these and brings up a default CPU
# aimee-llm sibling when nothing is configured. (Old combined leftovers
# AIMEE_EMBEDDER_URL=embedder:8080 / LLM_ENDPOINT=llm:8080 were removed: on a
# split deploy they silently pointed at non-existent services.)
# Bind the /v1 HTTP API on 0.0.0.0 (not the 127.0.0.1 default), so the published
# port and container-IP access reach it from outside the container.
ENV AIMEE_KB_HTTP_BIND=1

RUN useradd --system --home-dir /var/lib/aimee --create-home --shell /usr/sbin/nologin aimee
COPY --from=build /src/aimee-kb /usr/local/bin/aimee-kb
COPY --from=build /src/aimee-kb-resolver /usr/local/bin/aimee-kb-resolver

# Sidecar clients (the LLM/embedder access code the kb invokes via popen).
COPY scripts/embed-remote.py scripts/rerank-remote.py scripts/llm-chat.py \
     scripts/learning-synthesize.py scripts/curator-extract.py scripts/llm-rewrite.py \
     scripts/guardrails-semantic.py /opt/aimee/scripts/
# Baked default config: selects the sidecar commands (endpoints come from env).
# Kept OUTSIDE $AIMEE_HOME so a bind mount over /var/lib/aimee can't shadow it;
# the entrypoint seeds it into $AIMEE_HOME/.config/aimee on first start.
COPY deploy/container/aimee.yaml /opt/aimee/defaults/aimee.yaml
COPY deploy/container/aimee-kb-entrypoint.sh /usr/local/bin/aimee-kb-entrypoint.sh
RUN chmod +x /usr/local/bin/aimee-kb-entrypoint.sh

USER aimee
WORKDIR /var/lib/aimee
EXPOSE 8741

HEALTHCHECK --interval=30s --timeout=5s --start-period=10s --retries=3 \
    CMD curl -fsS http://127.0.0.1:8741/v1/health >/dev/null || exit 1

# The entrypoint raises the worker-thread stack rlimit and seeds the baked
# config into $AIMEE_HOME when a bind mount has shadowed the image copy, then
# execs aimee-kb. See deploy/container/aimee-kb-entrypoint.sh.
ENTRYPOINT ["/usr/local/bin/aimee-kb-entrypoint.sh"]
# aimee-kb serves the /v1 HTTP API only; --http-port is required (it exits without
# one). The legacy --socket transport was retired (#2747).
CMD ["--http-port=8741"]
