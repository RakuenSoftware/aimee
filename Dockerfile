FROM debian:bookworm-slim AS build

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        clang \
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
RUN make -C src ../aimee-kb ${AIMEE_VERSION:+GIT_VERSION=v$AIMEE_VERSION}

FROM debian:bookworm-slim

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
        python3 \
        zlib1g \
    && rm -rf /var/lib/apt/lists/*

ENV AIMEE_HOME=/var/lib/aimee
ENV AIMEE_DB2_URL=postgresql://aimee:aimee@postgres:5432/aimee_shared
# Defaults; compose overrides. The embedder service backs embeddings; point
# LLM_ENDPOINT at an OpenAI-compatible model (e.g. the llama.cpp `llm` service)
# to enable the synthesis / curator passes.
ENV AIMEE_EMBEDDER_URL=http://embedder:8080
ENV LLM_ENDPOINT=http://llm:8080/v1
# Bind the /v1 HTTP API on 0.0.0.0 (not the 127.0.0.1 default), so the published
# port and container-IP access reach it from outside the container.
ENV AIMEE_KB_HTTP_BIND=1

RUN useradd --system --home-dir /var/lib/aimee --create-home --shell /usr/sbin/nologin aimee
COPY --from=build /src/aimee-kb /usr/local/bin/aimee-kb

# Sidecar clients (the LLM/embedder access code the kb invokes via popen).
COPY scripts/embed-remote.py scripts/llm-chat.py scripts/learning-synthesize.py \
     scripts/curator-extract.py scripts/guardrails-semantic.py /opt/aimee/scripts/
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
