# Debian 13 (trixie) for libpq 17. The kb links db2/vault_operator_status_runtime.c,
# which uses the PostgreSQL 17 async-cancel API (PQcancelCreate / PGcancelConn /
# PQcancelPoll). Bookworm ships libpq 15, where those symbols do not exist, so the
# kb build failed here with -Werror=implicit-function-declaration while the native
# CI build stayed green -- `make all server` does not compile KB_SRCS, so this
# image was the only place that file was ever built.
# Global build args, declared before the first FROM so every stage can use them.
ARG PG_MAJOR=18
ARG PGVECTORSCALE_VERSION=0.9.0

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

# pgvectorscale (StreamingDiskANN). Always built: it adds ~1 MB to the image, and
# the kb already decides at RUNTIME whether to use it -- pgvec_vectorscale_available()
# probes pg_extension and falls back to HNSW with a warning when it is absent
# (src/db2/pgvec_transport.c). Gating it at build time would defeat that and make
# the index type a property of which image you happened to pull.
#
# The Rust toolchain and pgrx live only in this stage; the runtime image receives
# the built extension files and none of the build chain. CI caches this layer
# (cache-from/to type=gha,mode=max), so it recompiles only when the pins below move.
FROM debian:trixie-slim AS pgvectorscale-build
ARG PG_MAJOR
ARG PGVECTORSCALE_VERSION
# Same retry as the runtime stage: one transient TLS reset here fails the build.
RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates curl gnupg \
    && install -d /usr/share/postgresql-common/pgdg \
    && for a in 1 2 3 4 5; do \
         curl -fsS --connect-timeout 10 --max-time 60 \
           -o /usr/share/postgresql-common/pgdg/apt.postgresql.org.asc \
           https://www.postgresql.org/media/keys/ACCC4CF8.asc && break; \
         echo "pgdg key fetch failed (attempt $a/5); backing off"; sleep $((a * 5)); \
       done \
    && test -s /usr/share/postgresql-common/pgdg/apt.postgresql.org.asc \
    && echo "deb [signed-by=/usr/share/postgresql-common/pgdg/apt.postgresql.org.asc] http://apt.postgresql.org/pub/repos/apt trixie-pgdg main" \
        > /etc/apt/sources.list.d/pgdg.list \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential clang git pkg-config libssl-dev \
        "postgresql-${PG_MAJOR}" "postgresql-server-dev-${PG_MAJOR}" \
    && rm -rf /var/lib/apt/lists/*
ENV CARGO_HOME=/usr/local/cargo
ENV PATH=/usr/local/cargo/bin:$PATH
# cargo-pgrx must match the pgrx the crate depends on, so it is read from the
# checkout's Cargo.toml rather than pinned separately here.
RUN curl -fsS https://sh.rustup.rs | sh -s -- -y --profile minimal --default-toolchain stable \
    && git clone --depth 1 --branch "${PGVECTORSCALE_VERSION}" \
        https://github.com/timescale/pgvectorscale.git /src/pgvectorscale \
    && cd /src/pgvectorscale/pgvectorscale \
    && pgrx_version="$(awk -F'"' '/^pgrx[[:space:]]*=/{print $2; exit}' Cargo.toml)" \
    && echo "building pgvectorscale ${PGVECTORSCALE_VERSION} against pgrx ${pgrx_version}" \
    && cargo install --locked cargo-pgrx --version "${pgrx_version}" \
    && cargo pgrx init "--pg${PG_MAJOR}=/usr/lib/postgresql/${PG_MAJOR}/bin/pg_config" \
    && cargo pgrx install --release --pg-config "/usr/lib/postgresql/${PG_MAJOR}/bin/pg_config"
# Collect only the installed extension artifacts, at the paths the runtime uses.
RUN mkdir -p "/pgvectorscale/usr/lib/postgresql/${PG_MAJOR}/lib" \
        "/pgvectorscale/usr/share/postgresql/${PG_MAJOR}/extension" \
    && cp "/usr/lib/postgresql/${PG_MAJOR}/lib/vectorscale"*.so \
        "/pgvectorscale/usr/lib/postgresql/${PG_MAJOR}/lib/" \
    && cp "/usr/share/postgresql/${PG_MAJOR}/extension/vectorscale"* \
        "/pgvectorscale/usr/share/postgresql/${PG_MAJOR}/extension/"

# Runtime must match the build stage: libpq5 here has to provide at least the
# PostgreSQL 17 symbols the kb was linked against (PGDG's libpq 18 does).
FROM debian:trixie-slim

# python3 runs the sidecar clients the kb popens (llm-chat.py,
# learning-synthesize.py, curator-extract.py -> LLM endpoint) AND the in-container
# embedder below, which needs pip to install torch.
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        curl \
        libgomp1 \
        libpq5 \
        libssl3 \
        libzstd1 \
        python3 \
        python3-pip \
        python3-venv \
        zlib1g \
    && rm -rf /var/lib/apt/lists/*

# ---- the in-container embedder ------------------------------------------------
# The kb embeds ITSELF now. There is no embedder sidecar and no aimee-llm hop on the
# retrieval path: embedder-server.py runs on loopback inside this container with the
# weights BAKED IN, so a fresh container embeds immediately with no model download and
# no network at all.
#
# This deliberately reverses the split the unified-llm cutover introduced. That design
# bought a model-less kb image at the cost of a second container, a supervisor role, a
# gateway, an HTTP hop and a per-role env matrix — all to serve an embedder small enough
# to bake. With the reranker gone (measured negative) the only thing left on that path
# was embedding, so the container is retired and its per-model facts move back to the one
# place that reads them: scripts/embedders.json.
#
# CPU-only torch: the CUDA wheels are ~2GB of accelerator runtime this image never uses.
ENV EMBEDDER_VENV=/opt/aimee/embedder-venv
RUN python3 -m venv "$EMBEDDER_VENV" \
    && "$EMBEDDER_VENV/bin/pip" install --no-cache-dir --quiet \
        --index-url https://download.pytorch.org/whl/cpu torch \
    && "$EMBEDDER_VENV/bin/pip" install --no-cache-dir --quiet \
        "sentence-transformers>=3.3" "transformers>=5.2" einops \
    && find "$EMBEDDER_VENV" -name '__pycache__' -type d -prune -exec rm -rf {} + \
    && rm -rf /root/.cache/pip

# DB2 engine, from PGDG rather than Debian: trixie ships PostgreSQL 17 with
# pgvector 0.8.0, and its pgvector hard-depends on postgresql-17-jit-llvm, which
# drags LLVM into the runtime image. PGDG carries PostgreSQL 18 (the current
# stable major) with pgvector 0.8.5 depending only on the server and libc.
# PostgreSQL 18 is also the version pgvectorscale builds against.
ARG PG_MAJOR=18
# The PGDG key fetch is retried: it is a single point of build failure on a
# network hiccup and this layer runs on every kb build. Seen failing CI with
# "curl: (56) OpenSSL SSL_read: unexpected eof while reading".
RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates curl \
    && install -d /usr/share/postgresql-common/pgdg \
    && for a in 1 2 3 4 5; do \
         curl -fsS --connect-timeout 10 --max-time 60 \
           -o /usr/share/postgresql-common/pgdg/apt.postgresql.org.asc \
           https://www.postgresql.org/media/keys/ACCC4CF8.asc && break; \
         echo "pgdg key fetch failed (attempt $a/5); backing off"; sleep $((a * 5)); \
       done \
    && test -s /usr/share/postgresql-common/pgdg/apt.postgresql.org.asc \
    && echo "deb [signed-by=/usr/share/postgresql-common/pgdg/apt.postgresql.org.asc] http://apt.postgresql.org/pub/repos/apt trixie-pgdg main" \
        > /etc/apt/sources.list.d/pgdg.list \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
        "postgresql-${PG_MAJOR}" \
        "postgresql-${PG_MAJOR}-pgvector" \
    && rm -rf /var/lib/apt/lists/*
ENV AIMEE_DB2_PG_MAJOR=${PG_MAJOR}

# The vectorscale extension files, built above. Whether the kb actually creates
# diskann indexes stays a runtime decision (context.kb index type + corpus size).
COPY --from=pgvectorscale-build /pgvectorscale/ /

ENV AIMEE_HOME=/var/lib/aimee
# DB2 is unset on purpose. Unset means "the operator configured nothing", and the
# entrypoint then runs the in-image PostgreSQL 18 + pgvector cluster under
# $AIMEE_HOME/postgres. Setting it to any URL selects an external server and the
# entrypoint starts nothing — that path is fully supported and unchanged.
#
# This used to default to postgresql://aimee:aimee@postgres:5432/aimee_shared,
# which made "nothing configured" indistinguishable from "use the sibling
# container" and left a bare `docker run` pointed at a host that does not exist.
# New-install Compose manifests leave AIMEE_DB2_URL unset so the KB owns this
# internal cluster. Operators can still set the variable to select an external DB2.
# No baked embedder/LLM endpoint defaults. The kb runs NO model runtime; it calls
# an external aimee-llm container (CPU/GPU) or endpoint. Point it with ONE of:
#   AIMEE_LLM_URL       unified container -> embed + synth (one knob)
#   AIMEE_EMBEDDER_URL  pin the embedder (/embed) independently
#   LLM_ENDPOINT        Tier-A synth only (small-model interface)
# One of these is REQUIRED. The seeded default config selects the remote embedder
# sidecar, so with no endpoint set the DB2 dim probe fails and db2_init refuses to
# initialise rather than record a wrong vector width (db2_init.c) -- the kb then
# never binds its HTTP port. Verified on the shipped image with nothing else
# configured: the embedded cluster comes up, the service does not.
#
# The 384-dim builtin embedder exists but is NOT reached in that state: it applies
# only when no embed command is configured at all, and the baked config always
# configures one. The deploy unit (compose / smoothnas plugin) sets these and
# brings up a default CPU aimee-llm sibling, which is why this is invisible there. (Old combined leftovers
# AIMEE_EMBEDDER_URL=embedder:8080 / LLM_ENDPOINT=llm:8080 were removed: on a
# split deploy they silently pointed at non-existent services.)
# Bind the /v1 HTTP API on 0.0.0.0 (not the 127.0.0.1 default), so the published
# port and container-IP access reach it from outside the container.
ENV AIMEE_KB_HTTP_BIND=1

RUN useradd --system --home-dir /var/lib/aimee --create-home --shell /usr/sbin/nologin aimee
COPY --from=build /src/aimee-kb /usr/local/bin/aimee-kb
COPY --from=build /src/aimee-kb-resolver /usr/local/bin/aimee-kb-resolver

# The embedder registry: every per-model fact that changes the vectors (pooling, width,
# context, prefixes), keyed by model identity. Read by the in-container embedder to know
# what to load and which prefixes to apply, and by the server for the wizard's picker.
COPY scripts/embedders.json /opt/aimee/embedders.json
COPY scripts/embedder-server.py /opt/aimee/scripts/embedder-server.py

# Bake the weights for every registered embedder — one ships — so a fresh container
# embeds with no download and an air-gapped install works. Pinned to the registry's
# revision: a floating ref would let a rebuild change the vector space silently.
#
# A deployment that wants a wider embedder points AIMEE_EMBEDDER_URL at its own endpoint
# rather than baking a second model in here.
#
# FETCH ONLY WHAT THE TORCH PATH LOADS. snapshot_download takes the whole repo by
# default, and a repo may publish the same weights several times over: bekko ships an
# onnx/ tree (nine variants, ~2.2GB) and an openvino/ tree next to its safetensors, none
# of which sentence-transformers touches. Baking them cost 3.4GB of image for nothing.
# Excluding the alternate runtimes and the legacy .bin duplicates keeps this to the
# safetensors + tokenizer the loader actually reads.
# The baked cache is READ-ONLY at runtime (built as root, served as `aimee`), but
# trust_remote_code compiles the fetched modelling code into a writable module cache. It
# defaults inside HF_HOME, which the non-root user cannot write — nomic then failed with
# "Permission denied: /opt/aimee/models/modules". Point that one writable path at the
# home volume and leave the weights immutable.
ENV HF_HOME=/opt/aimee/models \
    HF_MODULES_CACHE=/var/lib/aimee/.cache/huggingface/modules \
    HF_HUB_OFFLINE=1
RUN --mount=type=cache,target=/root/.cache/huggingface \
    HF_HUB_OFFLINE=0 "$EMBEDDER_VENV/bin/python" - <<'PYBAKE'
import json
from huggingface_hub import snapshot_download
with open("/opt/aimee/embedders.json", encoding="utf-8") as handle:
    table = json.load(handle)["embedders"]
SKIP = [
    "onnx/*", "openvino/*", "*.onnx", "*.onnx_data",   # alternate runtimes
    "*.bin", "*.h5", "*.msgpack", "*.tflite", "*.ckpt",  # duplicate/legacy formats
    "*.gguf",                                          # not this runtime either
]
def code_repos(snapshot_dir):
    """Repos holding this model's custom modelling code, from config.json auto_map.

    A trust_remote_code model does not necessarily carry its own code — auto_map may point
    every class at a SEPARATE repo. Downloading only the weights then leaves the loader
    reaching for the Hub at first use, which fails closed under HF_HUB_OFFLINE, defeating
    the point of baking. So follow the references.

    The bundled embedder needs none of this (ModernBERT is native to transformers), but a
    swapped-in model may, and the failure is a container that starts and cannot embed.
    """
    import os
    out = set()
    cfg = os.path.join(snapshot_dir, "config.json")
    if not os.path.exists(cfg):
        return out
    with open(cfg, encoding="utf-8") as handle:
        auto_map = (json.load(handle).get("auto_map") or {})
    for target in auto_map.values():
        if isinstance(target, str) and "--" in target:
            out.add(target.split("--", 1)[0])
        elif isinstance(target, (list, tuple)):
            for item in target:
                if isinstance(item, str) and "--" in item:
                    out.add(item.split("--", 1)[0])
    return out


for name, spec in table.items():
    repo, revision = spec["repo"], spec.get("revision") or "main"
    print(f"baking {name}: {repo}@{revision}", flush=True)
    local = snapshot_download(repo, revision=revision, ignore_patterns=SKIP)
    # Code repos are referenced by name only — auto_map carries no revision — so these
    # take the default branch. That is a looser pin than the weights get; a model whose
    # code must be pinned needs its auto_map repo added to the registry explicitly.
    for code_repo in sorted(code_repos(local)):
        print(f"  + code: {code_repo}", flush=True)
        snapshot_download(code_repo, allow_patterns=["*.py", "*.json", "*.txt"])
PYBAKE

# Sidecar clients (the LLM access code the kb invokes via popen).
COPY scripts/embed-remote.py scripts/llm-chat.py \
     scripts/learning-synthesize.py scripts/curator-extract.py scripts/llm-rewrite.py \
     scripts/guardrails-semantic.py /opt/aimee/scripts/
# Baked default config: selects the sidecar commands (endpoints come from env).
# Kept OUTSIDE $AIMEE_HOME so a bind mount over /var/lib/aimee can't shadow it;
# the entrypoint seeds it into $AIMEE_HOME/.config/aimee on first start.
COPY deploy/container/aimee.yaml /opt/aimee/defaults/aimee.yaml
COPY deploy/container/aimee-kb-entrypoint.sh /usr/local/bin/aimee-kb-entrypoint.sh
COPY deploy/container/aimee-kb-db-export.sh /usr/local/bin/aimee-kb-db-export
RUN chmod +x /usr/local/bin/aimee-kb-entrypoint.sh /usr/local/bin/aimee-kb-db-export

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
